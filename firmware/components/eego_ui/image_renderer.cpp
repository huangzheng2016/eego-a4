// SPDX-License-Identifier: GPL-3.0-or-later
// 移植自 eego-a4-weread（GPL-3.0）同名文件；原始硬件事实来自 eego-a4-template-firmware（MIT）。
// ImageRenderer 实现 — 见 image_renderer.h
//
// 设计要点：
// - JPEG：ESP32-S3 ROM tjpgd（jd_prepare/jd_decomp），RGB888 输出，回调里 BT.601 转灰度
//   缩放档选择：在 1/8~1/1 间选“解码后仍不小于目标”的最大档（避免上采样失真），
//   再由 drawGrayToCanvas 双线性下采样到目标尺寸
// - PNG：lodepng（third_party 组件，zlib 许可）lodepng_decode32 解码为 RGBA，再 BT.601 转灰度
//   lodepng 无缩放档，整图解码；超大图按 kMaxDecodedPixels 拦截防 OOM
//   （lodepng 分配器已改为 heap_caps 优先 PSRAM，见 third_party/lodepng/lodepng.cpp）
// - 工作区：tjpgd 需 ~5KB 工作缓冲，分配在 DRAM（MALLOC_CAP_INTERNAL）以保性能
// - 灰度缓冲 / 缩放临时缓冲一律 PSRAM
//
// 已知限制：GIF 未实现（项目无 AnimatedGIF），返回 false。
// 不可重入（tjpgd 回调靠 device 上下文传参），请单线程逐张调用。

#include "image_renderer.h"
#include "sd_card.h"

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "rom/tjpgd.h"  // ESP32-S3 ROM 内置 Tiny JPEG Decompressor

#include "lodepng.h"  // 单文件 PNG 解码库（third_party/lodepng 组件）

#include <cstring>
#include <cstdlib>

namespace eego {

static const char* TAG = "eego.img";

// 单张图片文件大小上限（超出丢弃，防 OOM）
static constexpr size_t kImgMaxBytes = 512 * 1024;
// tjpgd 工作缓冲大小（JDEC + huffman/量化表 + MCU 缓冲，约 5KB，给 8KB 余量）
static constexpr size_t kJpegWorkSize = 8192;
// 解码后灰度缓冲像素上限（防超大图 OOM，超出则加大缩放档）
static constexpr int64_t kMaxDecodedPixels = 1500000;

// ==================== 工具函数 ====================

// magic bytes 判定：1=jpeg 2=png 3=gif 0=未知
static int detectFormat(const uint8_t* d, size_t n) {
    if (n >= 2 && d[0] == 0xFF && d[1] == 0xD8) return 1;
    // PNG 完整 8 字节签名：89 50 4E 47 0D 0A 1A 0A
    static const uint8_t kPngSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (n >= 8 && memcmp(d, kPngSig, 8) == 0) return 2;
    if (n >= 4 && d[0] == 0x47 && d[1] == 0x49 && d[2] == 0x46 && d[3] == 0x38) return 3;
    return 0;
}

// 等比适配进 max_w×max_h（保宽高比），结果至少 1×1
static void fitBox(int sw, int sh, int mw, int mh, int& tw, int& th) {
    if ((int64_t)sw * mh > (int64_t)mw * sh) {
        tw = mw;
        th = (int)((int64_t)sh * mw / sw);
    } else {
        th = mh;
        tw = (int)((int64_t)sw * mh / sh);
    }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
}

// 双线性缩放：src(sw×sh, 0-255) → dst(dw×dh, 0-255)
// 像素中心对齐：dst(dx,dy) ← src((dx+0.5)*sw/dw-0.5, (dy+0.5)*sh/dh-0.5)
// 边界 clamp。定点 Q16 计算。
static void bilinearScale(const uint8_t* src, int sw, int sh,
                          uint8_t* dst, int dw, int dh) {
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;

    // 预算每列的 sx0/sx1/fx_q16（PSRAM，避免每像素重复算除法）
    int* sx0 = (int*)heap_caps_malloc(sizeof(int) * dw, MALLOC_CAP_SPIRAM);
    int* sx1 = (int*)heap_caps_malloc(sizeof(int) * dw, MALLOC_CAP_SPIRAM);
    int* fxq = (int*)heap_caps_malloc(sizeof(int) * dw, MALLOC_CAP_SPIRAM);
    if (!sx0 || !sx1 || !fxq) {
        heap_caps_free(sx0); heap_caps_free(sx1); heap_caps_free(fxq);
        return;
    }
    for (int dx = 0; dx < dw; dx++) {
        // num = 2*dx*sw + sw - dw（= (dx+0.5)*sw/dw - 0.5 乘以 2*dw 后的整数）
        int num = 2 * dx * sw + sw - dw;
        int sxi = num / (2 * dw);  // 整数部分（向下取整，负数也向下）
        int fx = (num - sxi * (2 * dw)) * 65536 / (2 * dw);  // 小数 ×65536
        if (sxi < 0) { sxi = 0; fx = 0; }
        if (sxi >= sw) { sxi = sw - 1; fx = 0; }
        int sxi1 = sxi + 1;
        if (sxi1 >= sw) sxi1 = sw - 1;
        sx0[dx] = sxi; sx1[dx] = sxi1; fxq[dx] = fx;
    }

    for (int dy = 0; dy < dh; dy++) {
        int num = 2 * dy * sh + sh - dh;
        int syi = num / (2 * dh);
        int fy = (num - syi * (2 * dh)) * 65536 / (2 * dh);
        if (syi < 0) { syi = 0; fy = 0; }
        if (syi >= sh) { syi = sh - 1; fy = 0; }
        int syi1 = syi + 1;
        if (syi1 >= sh) syi1 = sh - 1;

        const uint8_t* r0 = src + syi * sw;
        const uint8_t* r1 = src + syi1 * sw;
        uint8_t* drow = dst + dy * dw;
        for (int dx = 0; dx < dw; dx++) {
            int p00 = r0[sx0[dx]];
            int p10 = r0[sx1[dx]];
            int p01 = r1[sx0[dx]];
            int p11 = r1[sx1[dx]];
            int top = p00 + (((p10 - p00) * fxq[dx]) >> 16);
            int bot = p01 + (((p11 - p01) * fxq[dx]) >> 16);
            int v = top + (((bot - top) * fy) >> 16);
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            drow[dx] = (uint8_t)v;
        }
    }

    heap_caps_free(sx0);
    heap_caps_free(sx1);
    heap_caps_free(fxq);
}

// ==================== tjpgd 回调 ====================

// tjpgd 会话上下文：同时承载输入流（data/len/pos）与输出灰度（gray/outW/outH）
struct JpegCtx {
    const uint8_t* data;
    size_t len;
    size_t pos;
    uint8_t* gray;   // 解码后灰度缓冲（PSRAM）
    int outW;        // 灰度缓冲 stride
    int outH;
};

// tjpgd 输入回调：从内存读取（buf==NULL 表示跳过）
static UINT jpeg_infunc(JDEC* jdec, BYTE* buf, UINT len) {
    JpegCtx* ctx = static_cast<JpegCtx*>(jdec->device);
    if (ctx->pos >= ctx->len) return 0;
    size_t avail = ctx->len - ctx->pos;
    if (len > avail) len = (UINT)avail;
    if (buf) memcpy(buf, ctx->data + ctx->pos, len);
    ctx->pos += len;
    return len;
}

// tjpgd 输出回调：RGB888 块 → BT.601 灰度写入 gray（按 outW stride）
// 返回 1 继续（0 会让 tjpgd 报 JDR_INTR）
static UINT jpeg_outfunc(JDEC* jdec, void* bitmap, JRECT* rect) {
    JpegCtx* ctx = static_cast<JpegCtx*>(jdec->device);
    const uint8_t* bmp = static_cast<const uint8_t*>(bitmap);
    int bw = rect->right - rect->left + 1;
    for (int y = rect->top; y <= rect->bottom; y++) {
        if (y >= ctx->outH) break;
        const uint8_t* srow = bmp + (y - rect->top) * bw * 3;
        uint8_t* drow = ctx->gray + y * ctx->outW;
        for (int x = rect->left; x <= rect->right; x++) {
            if (x >= ctx->outW) break;
            const uint8_t* px = srow + (x - rect->left) * 3;
            // BT.601: Y = (R*77 + G*150 + B*29) >> 8
            drow[x] = (uint8_t)((px[0] * 77 + px[1] * 150 + px[2] * 29) >> 8);
        }
    }
    return 1;
}

// ==================== 单例 ====================

ImageRenderer& ImageRenderer::instance() {
    static ImageRenderer inst;
    return inst;
}

// ==================== JPEG 解码 ====================

bool ImageRenderer::decodeJpeg(const uint8_t* data, size_t len, uint8_t*& outGray,
                               int* width, int* height, int maxWidth, int maxHeight) {
    outGray = nullptr;
    if (width) *width = 0;
    if (height) *height = 0;
    if (!data || len < 4) return false;

    // tjpgd 工作缓冲：优先 DRAM（性能），不足回退 PSRAM
    uint8_t* work = (uint8_t*)heap_caps_malloc(kJpegWorkSize, MALLOC_CAP_INTERNAL);
    if (!work) work = (uint8_t*)heap_caps_malloc(kJpegWorkSize, MALLOC_CAP_SPIRAM);
    if (!work) {
        ESP_LOGE(TAG, "jpeg work alloc failed");
        return false;
    }

    JDEC jdec;
    JpegCtx ctx{data, len, 0, nullptr, 0, 0};
    JRESULT jr = jd_prepare(&jdec, jpeg_infunc, work, (UINT)kJpegWorkSize, &ctx);
    if (jr != JDR_OK) {
        ESP_LOGW(TAG, "jd_prepare failed: %d (可能渐进式JPEG)", (int)jr);
        heap_caps_free(work);
        return false;
    }

    // 选缩放档（1/8~1/1）：选“解码后仍不小于目标”的最大档，避免上采样失真
    int scale = 0;  // 0=1/1, 1=1/2, 2=1/4, 3=1/8
    for (int s = 3; s >= 1; s--) {
        if ((int)(jdec.width >> s) >= maxWidth && (int)(jdec.height >> s) >= maxHeight) {
            scale = s;
            break;
        }
    }
    // 防超大图 OOM：解码像素超上限则加大缩放档
    while (scale < 3) {
        int ow = ((int)(jdec.width - 1) >> scale) + 1;
        int oh = ((int)(jdec.height - 1) >> scale) + 1;
        if ((int64_t)ow * oh > kMaxDecodedPixels) scale++;
        else break;
    }

    int outW = ((int)(jdec.width - 1) >> scale) + 1;
    int outH = ((int)(jdec.height - 1) >> scale) + 1;
    if (outW <= 0 || outH <= 0) {
        heap_caps_free(work);
        return false;
    }

    uint8_t* gray = (uint8_t*)heap_caps_malloc((size_t)outW * outH, MALLOC_CAP_SPIRAM);
    if (!gray) {
        ESP_LOGE(TAG, "jpeg gray alloc failed (%dx%d)", outW, outH);
        heap_caps_free(work);
        return false;
    }

    ctx.gray = gray;
    ctx.outW = outW;
    ctx.outH = outH;
    jr = jd_decomp(&jdec, jpeg_outfunc, (BYTE)scale);
    heap_caps_free(work);
    if (jr != JDR_OK) {
        ESP_LOGW(TAG, "jd_decomp failed: %d", (int)jr);
        heap_caps_free(gray);
        return false;
    }

    outGray = gray;
    if (width) *width = outW;
    if (height) *height = outH;
    return true;
}

bool ImageRenderer::decodePng(const uint8_t* data, size_t len, uint8_t*& outGray,
                              int* width, int* height, int maxWidth, int maxHeight) {
    outGray = nullptr;
    if (width) *width = 0;
    if (height) *height = 0;
    if (!data || len < 8) return false;
    (void)maxWidth; (void)maxHeight;  // lodepng 无缩放档，整图解码后由 drawGrayToCanvas 缩放

    // lodepng_decode32 一次性解码为 32-bit RGBA，缓冲由 lodepng 内部分配
    // （本工程 lodepng 副本的分配器已改为 heap_caps 优先 PSRAM，见
    //   third_party/lodepng/lodepng.cpp 顶部改动说明）。
    unsigned char* rgba = nullptr;
    unsigned uw = 0, uh = 0;
    unsigned err = lodepng_decode32(&rgba, &uw, &uh, data, (size_t)len);
    if (err != 0 || !rgba || uw == 0 || uh == 0) {
        ESP_LOGW(TAG, "png decode32 failed: %u (%s)", err, lodepng_error_text(err));
        free(rgba);  // 失败时 lodepng 不保证分配，free(nullptr) 安全
        return false;
    }

    int outW = (int)uw;
    int outH = (int)uh;

    // 防超大图 OOM：解码像素超上限则丢弃
    if ((int64_t)outW * outH > kMaxDecodedPixels) {
        ESP_LOGW(TAG, "png too large: %dx%d", outW, outH);
        free(rgba);
        return false;
    }

    // 灰度缓冲分配到 PSRAM
    uint8_t* gray = (uint8_t*)heap_caps_malloc((size_t)outW * outH, MALLOC_CAP_SPIRAM);
    if (!gray) {
        ESP_LOGE(TAG, "png gray alloc failed (%dx%d)", outW, outH);
        free(rgba);
        return false;
    }

    // RGBA → BT.601 灰度: Y = (R*299 + G*587 + B*114) / 1000
    // 透明像素（alpha<255）与白色背景混合：避免透明底 PNG 的 RGB 残留值
    // （通常为 0,0,0）在墨水屏上显示为黑块
    const unsigned char* src = rgba;
    for (int i = 0; i < outW * outH; i++) {
        uint32_t a = src[3];
        uint32_t r = ((uint32_t)src[0] * a + 255u * (255u - a)) / 255u;
        uint32_t g = ((uint32_t)src[1] * a + 255u * (255u - a)) / 255u;
        uint32_t b = ((uint32_t)src[2] * a + 255u * (255u - a)) / 255u;
        gray[i] = (uint8_t)((r * 299 + g * 587 + b * 114) / 1000);
        src += 4;
    }
    free(rgba);

    outGray = gray;
    if (width) *width = outW;
    if (height) *height = outH;
    return true;
}

bool ImageRenderer::decodeGifFirstFrame(const uint8_t* data, size_t len, uint8_t*& outGray,
                                        int* width, int* height, int maxWidth, int maxHeight) {
    (void)data; (void)len; (void)outGray; (void)width; (void)height;
    (void)maxWidth; (void)maxHeight;
    // 项目无 AnimatedGIF，暂未实现
    return false;
}

// ==================== 灰度上屏 ====================

void ImageRenderer::drawGrayToCanvas(PortraitCanvas& canvas, const uint8_t* gray,
                                     int srcW, int srcH, int16_t x, int16_t y,
                                     int16_t maxW, int16_t maxH) {
    if (!gray || srcW <= 0 || srcH <= 0 || maxW <= 0 || maxH <= 0) return;

    int tw, th;
    fitBox(srcW, srcH, maxW, maxH, tw, th);
    int ox = x + (maxW - tw) / 2;
    int oy = y + (maxH - th) / 2;

    // 1:1 直接上屏（省一次缩放）
    if (tw == srcW && th == srcH) {
        for (int dy = 0; dy < th; dy++) {
            const uint8_t* srow = gray + dy * srcW;
            for (int dx = 0; dx < tw; dx++) {
                uint8_t g15 = (uint8_t)((srow[dx] * 15 + 128) / 255);
                canvas.drawPixelGray((int16_t)(ox + dx), (int16_t)(oy + dy), g15);
            }
        }
        return;
    }

    // 双线性缩放到 tw×th 临时缓冲，再上屏
    uint8_t* tmp = (uint8_t*)heap_caps_malloc((size_t)tw * th, MALLOC_CAP_SPIRAM);
    if (!tmp) {
        ESP_LOGE(TAG, "drawGray scale alloc failed (%dx%d)", tw, th);
        return;
    }
    bilinearScale(gray, srcW, srcH, tmp, tw, th);
    for (int dy = 0; dy < th; dy++) {
        const uint8_t* srow = tmp + dy * tw;
        for (int dx = 0; dx < tw; dx++) {
            uint8_t g15 = (uint8_t)((srow[dx] * 15 + 128) / 255);
            canvas.drawPixelGray((int16_t)(ox + dx), (int16_t)(oy + dy), g15);
        }
    }
    heap_caps_free(tmp);
}

// ==================== render ====================

bool ImageRenderer::renderFromBuffer(PortraitCanvas& canvas,
                                     const uint8_t* data, size_t len,
                                     int16_t x, int16_t y,
                                     int16_t maxWidth, int16_t maxHeight) {
    if (!data || len < 4 || maxWidth <= 0 || maxHeight <= 0) return false;

    int fmt = detectFormat(data, len);
    uint8_t* gray = nullptr;
    int gw = 0, gh = 0;
    bool ok = false;
    if (fmt == 1) {
        ok = decodeJpeg(data, len, gray, &gw, &gh, maxWidth, maxHeight);
    } else if (fmt == 2) {
        ok = decodePng(data, len, gray, &gw, &gh, maxWidth, maxHeight);
    } else if (fmt == 3) {
        ok = decodeGifFirstFrame(data, len, gray, &gw, &gh, maxWidth, maxHeight);
    } else {
        ESP_LOGW(TAG, "unknown image format");
        return false;
    }
    if (!ok || !gray) {
        heap_caps_free(gray);
        return false;
    }

    drawGrayToCanvas(canvas, gray, gw, gh, x, y, maxWidth, maxHeight);
    heap_caps_free(gray);
    return true;
}

bool ImageRenderer::renderFromFile(PortraitCanvas& canvas,
                                   const char* path,
                                   int16_t x, int16_t y,
                                   int16_t maxWidth, int16_t maxHeight) {
    if (!path || maxWidth <= 0 || maxHeight <= 0) return false;
    SdCard& sd = SdCard::instance();
    if (!sd.ready()) return false;

    size_t fsz = sd.fileSize(path);
    if (fsz == 0 || fsz > kImgMaxBytes) {
        ESP_LOGW(TAG, "render: bad size %u (%s)", (unsigned)fsz, path);
        return false;
    }
    uint8_t* buf = (uint8_t*)heap_caps_malloc(fsz, MALLOC_CAP_SPIRAM);
    if (!buf) return false;
    size_t got = sd.readFile(path, buf, fsz);
    if (got != fsz) {
        heap_caps_free(buf);
        ESP_LOGW(TAG, "render: read failed %s", path);
        return false;
    }

    bool ok = renderFromBuffer(canvas, buf, got, x, y, maxWidth, maxHeight);
    heap_caps_free(buf);
    return ok;
}

}  // namespace eego
