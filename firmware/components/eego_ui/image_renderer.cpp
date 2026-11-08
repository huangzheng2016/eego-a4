// SPDX-License-Identifier: GPL-3.0-or-later
//
//

#include "image_renderer.h"
#include "sd_card.h"

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "rom/tjpgd.h"

#include "lodepng.h"

#include <cstring>
#include <cstdlib>

namespace eego {

static const char* TAG = "eego.img";

static constexpr size_t kImgMaxBytes = 512 * 1024;
static constexpr size_t kJpegWorkSize = 8192;
static constexpr int64_t kMaxDecodedPixels = 1500000;


static int detectFormat(const uint8_t* d, size_t n) {
    if (n >= 2 && d[0] == 0xFF && d[1] == 0xD8) return 1;
    static const uint8_t kPngSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (n >= 8 && memcmp(d, kPngSig, 8) == 0) return 2;
    if (n >= 4 && d[0] == 0x47 && d[1] == 0x49 && d[2] == 0x46 && d[3] == 0x38) return 3;
    return 0;
}

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

static void scaleCoordinate(int index, int src, int dst, int& source, int& fraction) {
    const int64_t num = 2LL * index * src + src - dst;
    const int64_t denom = 2LL * dst;
    int64_t s = num / denom;
    if (num < 0 && num % denom != 0) s--;
    int64_t f = (num - s * denom) * 65536 / denom;
    if (s < 0) { s = 0; f = 0; }
    if (s >= src) { s = src - 1; f = 0; }
    source = (int)s;
    fraction = (int)f;
}

static bool bilinearScale(const uint8_t* src, int sw, int sh,
                          uint8_t* dst, int dw, int dh) {
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return false;

    int* sx0 = (int*)heap_caps_malloc(sizeof(int) * dw, MALLOC_CAP_SPIRAM);
    int* sx1 = (int*)heap_caps_malloc(sizeof(int) * dw, MALLOC_CAP_SPIRAM);
    int* fxq = (int*)heap_caps_malloc(sizeof(int) * dw, MALLOC_CAP_SPIRAM);
    if (!sx0 || !sx1 || !fxq) {
        heap_caps_free(sx0); heap_caps_free(sx1); heap_caps_free(fxq);
        return false;
    }
    for (int dx = 0; dx < dw; dx++) {
        int sxi = 0, fx = 0;
        scaleCoordinate(dx, sw, dw, sxi, fx);
        int sxi1 = sxi + 1;
        if (sxi1 >= sw) sxi1 = sw - 1;
        sx0[dx] = sxi; sx1[dx] = sxi1; fxq[dx] = fx;
    }

    for (int dy = 0; dy < dh; dy++) {
        int syi = 0, fy = 0;
        scaleCoordinate(dy, sh, dh, syi, fy);
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
    return true;
}


struct JpegCtx {
    const uint8_t* data;
    size_t len;
    size_t pos;
    uint8_t* gray;
    int outW;
    int outH;
};

static UINT jpeg_infunc(JDEC* jdec, BYTE* buf, UINT len) {
    JpegCtx* ctx = static_cast<JpegCtx*>(jdec->device);
    if (ctx->pos >= ctx->len) return 0;
    size_t avail = ctx->len - ctx->pos;
    if (len > avail) len = (UINT)avail;
    if (buf) memcpy(buf, ctx->data + ctx->pos, len);
    ctx->pos += len;
    return len;
}

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


ImageRenderer& ImageRenderer::instance() {
    static ImageRenderer inst;
    return inst;
}


bool ImageRenderer::decodeJpeg(const uint8_t* data, size_t len, uint8_t*& outGray,
                               int* width, int* height, int maxWidth, int maxHeight) {
    outGray = nullptr;
    if (width) *width = 0;
    if (height) *height = 0;
    if (!data || len < 4) return false;

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

    int scale = 0;  // 0=1/1, 1=1/2, 2=1/4, 3=1/8
    for (int s = 3; s >= 1; s--) {
        if ((int)(jdec.width >> s) >= maxWidth && (int)(jdec.height >> s) >= maxHeight) {
            scale = s;
            break;
        }
    }
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
    if (!data || len < 24 || memcmp(data + 12, "IHDR", 4) != 0) return false;
    const uint32_t sourceW = ((uint32_t)data[16] << 24) | ((uint32_t)data[17] << 16) |
                             ((uint32_t)data[18] << 8) | data[19];
    const uint32_t sourceH = ((uint32_t)data[20] << 24) | ((uint32_t)data[21] << 16) |
                             ((uint32_t)data[22] << 8) | data[23];
    if (sourceW == 0 || sourceH == 0 || (uint64_t)sourceW * sourceH > kMaxDecodedPixels) {
        ESP_LOGW(TAG, "png header too large: %ux%u", (unsigned)sourceW, (unsigned)sourceH);
        return false;
    }
    (void)maxWidth; (void)maxHeight;

    unsigned char* rgba = nullptr;
    unsigned uw = 0, uh = 0;
    unsigned err = lodepng_decode32(&rgba, &uw, &uh, data, (size_t)len);
    if (err != 0 || !rgba || uw == 0 || uh == 0) {
        ESP_LOGW(TAG, "png decode32 failed: %u (%s)", err, lodepng_error_text(err));
        free(rgba);
        return false;
    }

    int outW = (int)uw;
    int outH = (int)uh;

    if ((int64_t)outW * outH > kMaxDecodedPixels) {
        ESP_LOGW(TAG, "png too large: %dx%d", outW, outH);
        free(rgba);
        return false;
    }

    uint8_t* gray = (uint8_t*)heap_caps_malloc((size_t)outW * outH, MALLOC_CAP_SPIRAM);
    if (!gray) {
        ESP_LOGE(TAG, "png gray alloc failed (%dx%d)", outW, outH);
        free(rgba);
        return false;
    }

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
    return false;
}


static uint8_t ditherGray4(uint8_t gray, int x, int y) {
    static constexpr uint8_t kBayer4[4][4] = {
        {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    static constexpr uint8_t kLevels[4] = {0, 5, 10, 15};
    const uint32_t scaled = static_cast<uint32_t>(gray) * 3;
    uint32_t level = scaled / 255;
    const uint32_t fraction = scaled % 255;
    const uint8_t threshold = kBayer4[(y % 4 + 4) % 4][(x % 4 + 4) % 4];
    if (level < 3 && fraction * 32 > (threshold * 2 + 1) * 255) level++;
    return kLevels[level];
}

bool ImageRenderer::drawGrayToCanvas(PortraitCanvas& canvas, const uint8_t* gray,
                                     int srcW, int srcH, int16_t x, int16_t y,
                                     int16_t maxW, int16_t maxH) {
    if (!gray || srcW <= 0 || srcH <= 0 || maxW <= 0 || maxH <= 0) return false;

    int tw, th;
    fitBox(srcW, srcH, maxW, maxH, tw, th);
    int ox = x + (maxW - tw) / 2;
    int oy = y + (maxH - th) / 2;

    if (tw == srcW && th == srcH) {
        for (int dy = 0; dy < th; dy++) {
            const uint8_t* srow = gray + dy * srcW;
            for (int dx = 0; dx < tw; dx++) {
                const int px = ox + dx;
                const int py = oy + dy;
                canvas.drawPixelGray((int16_t)px, (int16_t)py,
                                     ditherGray4(srow[dx], px, py));
            }
        }
        return true;
    }

    uint8_t* tmp = (uint8_t*)heap_caps_malloc((size_t)tw * th, MALLOC_CAP_SPIRAM);
    if (!tmp) {
        ESP_LOGE(TAG, "drawGray scale alloc failed (%dx%d)", tw, th);
        return false;
    }
    if (!bilinearScale(gray, srcW, srcH, tmp, tw, th)) {
        heap_caps_free(tmp);
        return false;
    }
    for (int dy = 0; dy < th; dy++) {
        const uint8_t* srow = tmp + dy * tw;
        for (int dx = 0; dx < tw; dx++) {
            const int px = ox + dx;
            const int py = oy + dy;
            canvas.drawPixelGray((int16_t)px, (int16_t)py,
                                 ditherGray4(srow[dx], px, py));
        }
    }
    heap_caps_free(tmp);
    return true;
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

    const bool drawn = drawGrayToCanvas(canvas, gray, gw, gh, x, y, maxWidth, maxHeight);
    heap_caps_free(gray);
    return drawn;
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
