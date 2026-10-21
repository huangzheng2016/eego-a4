// SPDX-License-Identifier: GPL-3.0-or-later
// EEGO A4 告警/数据面板 — app_main 骨架（FW1 硬件基础，不含告警业务）
//
// 启动流程：
//   1. board::beginStandardHardware() 按启动合同初始化全部硬件
//      （GPIO4 锁存 → 触控/按键 → 前光探测 → 充电极性 → RTC → SD → 显示）
//   2. 加载 SD 卡字体（缺席时回退内置 5x7 ASCII 字体），在安全区内绘制
//      Alert Panel 启动页（含四级灰阶元素，验证灰阶链路）
//   3. 进入主循环：8ms 触控轮询 + 按键队列 + 串口诊断命令处理
//
// 串口诊断入口：UART0 115200，输入 help 查看命令。

#include "board/board_boot.h"
#include "eego_hal.h"
#include "eego_ui.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>
#include <strings.h>  // strcasecmp

namespace {

const char* TAG = "alert_panel";

constexpr uint32_t kTouchPollMs = 8;      // GSLX680 轮询周期（触控合同）
constexpr uint32_t kDiagQueueLen = 8;
constexpr uint32_t kDiagCmdMaxLen = 64;
constexpr uint32_t kDiagTaskStack = 4096;

eego::Uc8279cDisplay display;
eego::Gslx680Touch touch;
eego::PortraitCanvas canvas(display);
eego::board::BootStatus bootStatus;

QueueHandle_t diagQueue = nullptr;

// ── 字体 ─────────────────────────────────────────────────────────────────────

// 在 SD 卡根目录寻找首个 .eefont / .bin 字体并加载为 UI 字体。
// 找不到时保持未加载状态，渲染自动回退内置 5x7 字体。
void tryLoadUiFontFromSd() {
    if (!eego::SdCard::instance().ready()) return;
    eego::SdCard::DirEntry entries[16];
    const int n = eego::SdCard::instance().listDir("/", entries, 16);
    for (int i = 0; i < n; i++) {
        if (entries[i].isDir) continue;
        const char* dot = strrchr(entries[i].name, '.');
        if (!dot) continue;
        if (strcasecmp(dot, ".eefont") != 0 && strcasecmp(dot, ".bin") != 0) continue;
        char path[258];  // '/' + name(255) + NUL
        snprintf(path, sizeof(path), "/%s", entries[i].name);
        if (eego::FontManager::instance().loadUiFont(path)) {
            ESP_LOGI(TAG, "UI font loaded: %s", path);
            return;
        }
    }
    ESP_LOGI(TAG, "no SD font found, using built-in 5x7 fallback");
}

// ── 启动页（全部内容在 UI 矩形区内，即 R48 安全区内）─────────────────────────

void drawSplash() {
    canvas.clear();               // 全屏白底（背景允许覆盖整个物理面板）
    canvas.useUiContentRect();    // 普通 UI 裁剪到 (28,28,496×712)

    // 标题栏：深灰圆角矩形（验证四级灰阶链路），白色标题文字
    canvas.fillRoundRectGray(eego::UI_CONTENT_X, eego::UI_CONTENT_Y,
                             eego::UI_CONTENT_WIDTH, 64, 10, 4);
    eego::FontManager::instance().drawTextGray(canvas, eego::UI_CONTENT_X + 20,
                                               eego::UI_CONTENT_Y + 16,
                                               "ALERT PANEL", 3, 15);

    // 分隔线
    canvas.drawLine(eego::UI_CONTENT_X, eego::UI_CONTENT_Y + 80,
                    eego::UI_CONTENT_RIGHT, eego::UI_CONTENT_Y + 80, 0);

    // 硬件状态行
    const eego::BatteryInfo bat = eego::PowerManager::instance().readBattery();
    int16_t y = eego::UI_CONTENT_Y + 100;
    auto row = [&y](const char* text) {
        eego::FontManager::instance().drawText(canvas, eego::UI_CONTENT_X + 12, y,
                                               text, 2, false);
        y += 26;
    };
    row(bootStatus.touchReady ? "TOUCH OK" : "TOUCH FAIL");
    row(bootStatus.buttonsReady ? "BUTTONS OK" : "BUTTONS FAIL");
    row(bootStatus.frontlightPresent ? "FRONTLIGHT YES" : "FRONTLIGHT NO");
    row(bootStatus.rtcReady ? "RTC OK" : "RTC FAIL");
    row(bootStatus.sdReady ? "SD OK" : "SD FAIL");
    row(bootStatus.displayReady ? "DISPLAY OK" : "DISPLAY FAIL");
    char line[48];
    snprintf(line, sizeof(line), "BAT %u%% %umV", bat.percent, bat.millivolts);
    row(line);

    // 灰阶条：0/4/8/12 四段（黑/深灰/浅灰/白）
    const int16_t gy = eego::UI_CONTENT_BOTTOM - 60;
    for (int i = 0; i < 4; i++) {
        canvas.fillRoundRectGray(eego::UI_CONTENT_X + i * 60, gy, 52, 24, 4,
                                 static_cast<uint8_t>(i * 4));
    }

    // 底部提示（浅灰文字）
    eego::FontManager::instance().drawTextGray(canvas, eego::UI_CONTENT_X + 12,
                                               eego::UI_CONTENT_BOTTOM - 20,
                                               "UART0 115200 DIAG: HELP", 1, 8);

    canvas.display(eego::RefreshMode::Full);
    canvas.waitRefreshComplete();
}

// ── 串口诊断 ─────────────────────────────────────────────────────────────────

void diagPrintHelp() {
    printf("\ncommands:\n"
           "  help     - show this help\n"
           "  status   - boot status of all hardware\n"
           "  battery  - battery voltage / percent / charging\n"
           "  rtc      - read PCF8563 wall-clock time\n"
           "  sd       - SD card status + root listing\n"
           "  refresh  - redraw splash (full refresh)\n");
    fflush(stdout);
}

void diagStatus() {
    printf("boot: latch=%d touch=%d buttons=%d frontlight=%d rtc=%d sd=%d display=%d\n",
           bootStatus.powerLatched, bootStatus.touchReady, bootStatus.buttonsReady,
           bootStatus.frontlightPresent, bootStatus.rtcReady, bootStatus.sdReady,
           bootStatus.displayReady);
}

void diagBattery() {
    const eego::BatteryInfo bat = eego::PowerManager::instance().readBattery();
    printf("battery: %u mV, %u%%, charging=%d\n",
           bat.millivolts, bat.percent, bat.charging ? 1 : 0);
}

void diagRtc() {
    eego::Pcf8563Rtc rtc(I2C_NUM_0);
    const eego::RtcTime t = rtc.readTime();
    if (t.year == 0 && !t.valid) {
        printf("rtc: read failed\n");
        return;
    }
    printf("rtc: %04u-%02u-%02u %02u:%02u:%02u wday=%u valid=%d\n",
           t.year, t.month, t.day, t.hour, t.minute, t.second,
           t.weekday, t.valid ? 1 : 0);
}

void diagSd() {
    eego::SdCard& sd = eego::SdCard::instance();
    printf("sd: ready=%d\n", sd.ready() ? 1 : 0);
    if (!sd.ready()) return;
    eego::SdCard::DirEntry entries[16];
    const int n = sd.listDir("/", entries, 16);
    for (int i = 0; i < n; i++) {
        printf("  %s %s %u\n", entries[i].isDir ? "DIR " : "FILE",
               entries[i].name, static_cast<unsigned>(entries[i].size));
    }
}

void handleDiagCommand(const char* cmd) {
    if (strcmp(cmd, "help") == 0) {
        diagPrintHelp();
    } else if (strcmp(cmd, "status") == 0) {
        diagStatus();
    } else if (strcmp(cmd, "battery") == 0) {
        diagBattery();
    } else if (strcmp(cmd, "rtc") == 0) {
        diagRtc();
    } else if (strcmp(cmd, "sd") == 0) {
        diagSd();
    } else if (strcmp(cmd, "refresh") == 0) {
        if (bootStatus.displayReady) {
            drawSplash();
            printf("splash redrawn\n");
        } else {
            printf("display not ready\n");
        }
    } else {
        printf("ERR unknown cmd '%s', type help\n", cmd);
    }
    fflush(stdout);
}

// 诊断串口读取任务：阻塞读行后投递到主循环执行
// （命令在 app_main 上下文执行，保证 I2C 单线程调用合同）
void diagTask(void* arg) {
    (void)arg;
    char line[kDiagCmdMaxLen];
    while (true) {
        printf("diag> ");
        fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;
        xQueueSend(diagQueue, line, portMAX_DELAY);
    }
}

}  // namespace

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "EEGO A4 alert panel boot (FW1 hardware foundation)");

    // 1. 板级启动合同初始化（顺序即合同，见 board_boot.h）
    bootStatus = eego::board::beginStandardHardware(display, touch);

    // 2. 字体：SD 卡字体优先，缺席时回退内置 5x7
    tryLoadUiFontFromSd();

    // 3. Alert Panel 启动页（安全区内，含灰阶元素）
    if (bootStatus.displayReady) {
        drawSplash();
    }

    // 4. 串口诊断入口 + 输入事件循环（不含告警业务）
    diagQueue = xQueueCreate(kDiagQueueLen, kDiagCmdMaxLen);
    if (diagQueue == nullptr ||
        xTaskCreate(diagTask, "diag", kDiagTaskStack, nullptr, 3, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "diag task init failed");
    }
    ESP_LOGI(TAG, "serial diagnostics ready (115200), type 'help'");
    diagPrintHelp();

    char cmd[kDiagCmdMaxLen];
    eego::ButtonEvent bevt;
    while (true) {
        // 触控轮询（8ms 合同，边沿触发）
        const eego::TouchEvent tev = touch.poll();
        if (tev.type == eego::TouchEventType::Touch) {
            ESP_LOGI(TAG, "touch %s x=%u y=%u", tev.pressed ? "down" : "up",
                     tev.x, tev.y);
        } else if (tev.type == eego::TouchEventType::FrontKeyShort) {
            ESP_LOGI(TAG, "front key short");
        } else if (tev.type == eego::TouchEventType::FrontKeyLong) {
            ESP_LOGI(TAG, "front key long");
        }

        // 按键事件队列（后台任务去抖）
        while (eego::ButtonManager::instance().poll(&bevt, 0)) {
            ESP_LOGI(TAG, "button id=%d action=%d",
                     static_cast<int>(bevt.id), static_cast<int>(bevt.action));
        }

        // 串口诊断命令
        if (xQueueReceive(diagQueue, cmd, 0) == pdTRUE) {
            handleDiagCommand(cmd);
        }

        vTaskDelay(pdMS_TO_TICKS(kTouchPollMs));
    }
}
