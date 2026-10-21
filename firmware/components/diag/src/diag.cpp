// SPDX-License-Identifier: GPL-3.0-or-later
// diag 组件实现 —— UART0 读行任务 + app_task 上下文分发（见 diag.h 契约）。
#include "diag/diag.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace eego::diag {

namespace {

const char* TAG = "diag";

constexpr uint32_t kQueueLen = 8;
constexpr uint32_t kCmdMaxLen = 1024;  // inject 单行 JSON 需要余量（更大批量走 HTTP）
constexpr uint32_t kTaskStackBytes = 3072;
constexpr UBaseType_t kTaskPriority = 3;

QueueHandle_t g_queue = nullptr;
Handlers g_handlers;

// UART0 读行任务：阻塞 fgets → 去 CRLF → 入队（命令执行不在本任务）。
void diagTask(void* arg) {
  (void)arg;
  char line[kCmdMaxLen];
  while (true) {
    if (fgets(line, sizeof(line), stdin) == nullptr) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
      line[--len] = '\0';
    }
    if (len == 0) continue;
    injectLine(line);
  }
}

void printHelp() {
  printf(
      "\ncommands:\n"
      "  help            - show this help\n"
      "  status          - boot status + power mode + uptime\n"
      "  battery         - battery voltage / percent / charging\n"
      "  rtc             - read PCF8563 wall-clock time\n"
      "  sd              - SD status + alertpanel dir listing\n"
      "  net             - wifi state + source health table\n"
      "  alerts          - alert snapshot summary\n"
      "  inject <json>   - ingest alert event(s) (enqueue only)\n"
      "  ack-all         - acknowledge all active alerts\n"
      "  smoke           - run self-check sequence\n"
      "  version         - firmware/chip/heap/framebuffer info\n"
      "  lang zh|en      - switch panel UI language\n"
      "  page s0|s1|s2|s3|s4 - jump to screen (bench aid)\n");
  fflush(stdout);
}

// 取命令动词（首 token）；arg 指向动词后的第一个非空字符（可能为空串）。
void splitVerb(char* line, char*& verb, char*& arg) {
  verb = line;
  while (*verb == ' ') verb++;
  arg = verb;
  while (*arg != '\0' && *arg != ' ') arg++;
  if (*arg != '\0') {
    *arg = '\0';
    arg++;
    while (*arg == ' ') arg++;
  }
}

void dispatch(char* line) {
  char* verb = nullptr;
  char* arg = nullptr;
  splitVerb(line, verb, arg);

  if (strcmp(verb, "help") == 0) {
    printHelp();
  } else if (strcmp(verb, "status") == 0 && g_handlers.status) {
    g_handlers.status();
  } else if (strcmp(verb, "battery") == 0 && g_handlers.battery) {
    g_handlers.battery();
  } else if (strcmp(verb, "rtc") == 0 && g_handlers.rtc) {
    g_handlers.rtc();
  } else if (strcmp(verb, "sd") == 0 && g_handlers.sd) {
    g_handlers.sd();
  } else if (strcmp(verb, "net") == 0 && g_handlers.net) {
    g_handlers.net();
  } else if (strcmp(verb, "alerts") == 0 && g_handlers.alerts) {
    g_handlers.alerts();
  } else if (strcmp(verb, "inject") == 0 && g_handlers.inject) {
    if (*arg == '\0') {
      printf("ERR usage: inject <json>\n");
    } else {
      g_handlers.inject(arg);
    }
  } else if (strcmp(verb, "ack-all") == 0 && g_handlers.ack_all) {
    g_handlers.ack_all();
  } else if (strcmp(verb, "smoke") == 0 && g_handlers.smoke) {
    g_handlers.smoke();
  } else if (strcmp(verb, "version") == 0 && g_handlers.version) {
    g_handlers.version();
  } else if (strcmp(verb, "lang") == 0 && g_handlers.lang) {
    g_handlers.lang(arg);
  } else if (strcmp(verb, "page") == 0 && g_handlers.page) {
    g_handlers.page(arg);
  } else {
    printf("ERR unknown cmd '%s', type help\n", verb);
  }
  fflush(stdout);
}

}  // namespace

bool start(const Handlers& handlers) {
  g_handlers = handlers;
  if (g_queue != nullptr) return true;  // 幂等
  g_queue = xQueueCreate(kQueueLen, kCmdMaxLen);
  if (g_queue == nullptr) {
    ESP_LOGE(TAG, "diag queue create failed");
    return false;
  }
  if (xTaskCreate(diagTask, "diag", kTaskStackBytes / sizeof(StackType_t), nullptr,
                  kTaskPriority, nullptr) != pdPASS) {
    ESP_LOGE(TAG, "diag task create failed");
    return false;
  }
  ESP_LOGI(TAG, "serial diagnostics ready (UART0 115200), type 'help'");
  printHelp();
  return true;
}

void service() {
  if (g_queue == nullptr) return;
  char line[kCmdMaxLen];
  if (xQueueReceive(g_queue, line, 0) == pdTRUE) {
    dispatch(line);
  }
}

bool injectLine(const char* line) {
  if (g_queue == nullptr || line == nullptr) return false;
  if (strlen(line) >= kCmdMaxLen) return false;
  return xQueueSend(g_queue, line, pdMS_TO_TICKS(100)) == pdTRUE;
}

}  // namespace eego::diag
