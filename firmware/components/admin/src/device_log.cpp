// SPDX-License-Identifier: GPL-3.0-or-later
#include "admin/device_log.h"

#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <vector>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
#include "esp_core_dump.h"
#define EEGO_HAS_COREDUMP_SUMMARY 1
#else
#define EEGO_HAS_COREDUMP_SUMMARY 0
#endif

namespace eego::admin::devlog {
namespace {
constexpr char kTag[] = "devlog";
constexpr size_t kRingBytes = 32 * 1024;  // PSRAM

vprintf_like_t g_prev = nullptr;
SemaphoreHandle_t g_mutex = nullptr;
char* g_ring = nullptr;
size_t g_head = 0;
bool g_wrapped = false;
char g_line[256];

char levelOf(const char* fmt) {
  if (fmt == nullptr) return 0;
  if (fmt[0] == '\033') {
    const char* m = std::strchr(fmt, 'm');
    return m != nullptr ? m[1] : 0;
  }
  return fmt[0];
}

void appendLocked(const char* text, size_t n) {
  for (size_t i = 0; i < n; i++) {
    const char c = text[i];
    if (c == '\033') {
      while (i < n && text[i] != 'm') i++;
      continue;
    }
    g_ring[g_head++] = c;
    if (g_head == kRingBytes) {
      g_head = 0;
      g_wrapped = true;
    }
  }
}

int hook(const char* fmt, va_list ap) {
  va_list copy;
  va_copy(copy, ap);
  const int r = g_prev != nullptr ? g_prev(fmt, ap) : vprintf(fmt, ap);
  const char lv = levelOf(fmt);
  if ((lv == 'E' || lv == 'W' || lv == 'I') && g_ring != nullptr && !xPortInIsrContext() &&
      xSemaphoreTake(g_mutex, 0) == pdTRUE) {
    const int n = vsnprintf(g_line, sizeof(g_line), fmt, copy);
    if (n > 0) {
      size_t len = static_cast<size_t>(n) < sizeof(g_line) ? static_cast<size_t>(n) : sizeof(g_line) - 1;
      appendLocked(g_line, len);
      if (len > 0 && g_line[len - 1] != '\n') appendLocked("\n", 1);
    }
    xSemaphoreGive(g_mutex);
  }
  va_end(copy);
  return r;
}

const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt_watchdog";
    case ESP_RST_TASK_WDT: return "task_watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    case ESP_RST_EFUSE: return "efuse";
    case ESP_RST_PWR_GLITCH: return "power_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default: return "unknown";
  }
}

void appendHex(std::string& out, uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof(b), "\"0x%08lx\"", static_cast<unsigned long>(v));
  out += b;
}

// Runs in whatever context failed, possibly on a tiny stack: use the ROM printf (no heap,
// small frame) and skip ISR context entirely. The line reaches the serial console only.
void onAllocFailed(size_t size, uint32_t caps, const char* function) {
  if (xPortInIsrContext()) return;
  esp_rom_printf("E devlog: alloc failed: %u bytes caps=0x%x in %s; internal free=%u largest=%u\n",
                 static_cast<unsigned>(size), static_cast<unsigned>(caps), function ? function : "?",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(
                     heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
}

constexpr uint32_t kLowStackBytes = 1024;

// Snapshot of every task's stack high-water mark (bytes never used), lowest first.
std::vector<TaskStatus_t> taskSnapshot() {
  std::vector<TaskStatus_t> tasks(uxTaskGetNumberOfTasks() + 4);
  const UBaseType_t n = uxTaskGetSystemState(tasks.data(), tasks.size(), nullptr);
  tasks.resize(n);
  std::sort(tasks.begin(), tasks.end(), [](const TaskStatus_t& a, const TaskStatus_t& b) {
    return a.usStackHighWaterMark < b.usStackHighWaterMark;
  });
  return tasks;
}

void logHeap(void*) {
  ESP_LOGI(kTag, "heap: internal free=%u min=%u largest=%u; psram free=%u",
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  // Full table at 5 min and then every 30 min (stack headroom of every task, for tuning).
  static uint32_t calls = 0;
  const bool full = ++calls == 5 || calls % 30 == 0;
  std::string line;
  for (const TaskStatus_t& t : taskSnapshot()) {
    const uint32_t free_bytes = t.usStackHighWaterMark;  // ESP-IDF reports bytes
    if (free_bytes < kLowStackBytes) {
      ESP_LOGW(kTag, "stack low: task %s has %u bytes left", t.pcTaskName,
               static_cast<unsigned>(free_bytes));
    }
    if (full) {
      char item[40];
      std::snprintf(item, sizeof(item), " %s=%u", t.pcTaskName, static_cast<unsigned>(free_bytes));
      line += item;
      if (line.size() > 160) {
        ESP_LOGI(kTag, "stack free:%s", line.c_str());
        line.clear();
      }
    }
  }
  if (!line.empty()) ESP_LOGI(kTag, "stack free:%s", line.c_str());
}

}  // namespace

std::string tasksJson() {
  std::string out = "{\"tasks\":[";
  bool first = true;
  char line[96];
  for (const TaskStatus_t& t : taskSnapshot()) {
    char name[configMAX_TASK_NAME_LEN + 1] = {};
    std::snprintf(name, sizeof(name), "%s", t.pcTaskName);
    for (char& c : name) {
      if (c == '"' || c == '\\' || (c != 0 && static_cast<unsigned char>(c) < 0x20)) c = '?';
    }
    std::snprintf(line, sizeof(line), "%s{\"name\":\"%s\",\"stack_free\":%u,\"priority\":%u}",
                  first ? "" : ",", name, static_cast<unsigned>(t.usStackHighWaterMark),
                  static_cast<unsigned>(t.uxCurrentPriority));
    out += line;
    first = false;
  }
  out += "]}";
  return out;
}

void install() {
  if (g_ring != nullptr) return;
  g_mutex = xSemaphoreCreateMutex();
  g_ring = static_cast<char*>(heap_caps_calloc(1, kRingBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (g_mutex == nullptr || g_ring == nullptr) {
    ESP_LOGW(kTag, "device log buffer unavailable");
    return;
  }
  g_prev = esp_log_set_vprintf(&hook);
  heap_caps_register_failed_alloc_callback(&onAllocFailed);
  esp_log_level_set("wifi", ESP_LOG_ERROR);
  const esp_timer_create_args_t args = {.callback = &logHeap, .arg = nullptr,
                                        .dispatch_method = ESP_TIMER_TASK, .name = "devlog_heap",
                                        .skip_unhandled_events = true};
  esp_timer_handle_t timer = nullptr;
  if (esp_timer_create(&args, &timer) == ESP_OK) esp_timer_start_periodic(timer, 60ULL * 1000 * 1000);
  logHeap(nullptr);
}

std::string tail(size_t max_bytes) {
  std::string out;
  if (g_ring == nullptr || g_mutex == nullptr) return out;
  if (max_bytes > kRingBytes) max_bytes = kRingBytes;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  const size_t used = g_wrapped ? kRingBytes : g_head;
  const size_t n = used < max_bytes ? used : max_bytes;
  out.reserve(n);
  const size_t start = (g_head + kRingBytes - n) % kRingBytes;
  for (size_t i = 0; i < n; i++) out.push_back(g_ring[(start + i) % kRingBytes]);
  xSemaphoreGive(g_mutex);
  if (n == max_bytes) {
    const size_t nl = out.find('\n');
    if (nl != std::string::npos) out.erase(0, nl + 1);
  }
  return out;
}

std::string crashJson() {
  std::string out = "{\"reset_reason\":\"";
  out += resetReasonName(esp_reset_reason());
  out += "\",\"crashed\":";
#if EEGO_HAS_COREDUMP_SUMMARY
  auto* s = static_cast<esp_core_dump_summary_t*>(heap_caps_calloc(1, sizeof(esp_core_dump_summary_t),
                                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (s != nullptr && esp_core_dump_image_check() == ESP_OK && esp_core_dump_get_summary(s) == ESP_OK) {
    char task[sizeof(s->exc_task) + 1] = {};
    std::memcpy(task, s->exc_task, sizeof(s->exc_task));
    for (char& c : task) {
      if (c == '"' || c == '\\' || (c != 0 && static_cast<unsigned char>(c) < 0x20)) c = '?';
    }
    out += "true,\"task\":\"";
    out += task;
    out += "\",\"pc\":";
    appendHex(out, s->exc_pc);
    out += ",\"backtrace\":[";
    const uint32_t depth = s->exc_bt_info.depth < 16 ? s->exc_bt_info.depth : 16;
    for (uint32_t i = 0; i < depth; i++) {
      if (i) out += ",";
      appendHex(out, s->exc_bt_info.bt[i]);
    }
    out += "],\"backtrace_corrupted\":";
    out += s->exc_bt_info.corrupted ? "true" : "false";
    char b[64];
    std::snprintf(b, sizeof(b), ",\"cause\":%lu,\"vaddr\":",
                  static_cast<unsigned long>(s->ex_info.exc_cause));
    out += b;
    appendHex(out, s->ex_info.exc_vaddr);
    out += ",\"elf_sha256\":\"";
    char sha[sizeof(s->app_elf_sha256) + 1] = {};
    std::memcpy(sha, s->app_elf_sha256, sizeof(s->app_elf_sha256));
    for (char& c : sha) {
      if (c != 0 && !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) c = '?';
    }
    out += sha;
    out += "\"}";
    heap_caps_free(s);
    return out;
  }
  if (s != nullptr) heap_caps_free(s);
#endif
  out += "false}";
  return out;
}

esp_err_t clearCrash() {
#if EEGO_HAS_COREDUMP_SUMMARY
  return esp_core_dump_image_erase();
#else
  return ESP_OK;
#endif
}

void logBootReport() {
  const esp_reset_reason_t r = esp_reset_reason();
  ESP_LOGI(kTag, "reset reason: %s", resetReasonName(r));
  std::string crash = crashJson();
  // A dump from a different build cannot be decoded with this firmware's ELF: drop it so
  // it is not mistaken for a crash of the running firmware.
  const size_t at = crash.find("\"elf_sha256\":\"");
  if (at != std::string::npos) {
    char current[16] = {};
    esp_app_get_elf_sha256(current, sizeof(current));
    const std::string dumped = crash.substr(at + 14, crash.find('"', at + 14) - (at + 14));
    const size_t n = std::min(dumped.size(), std::strlen(current));
    if (n == 0 || dumped.compare(0, n, current, n) != 0) {
      ESP_LOGW(kTag, "discarding core dump from another firmware build (%s, running %s)",
               dumped.c_str(), current);
      clearCrash();
      crash = crashJson();
    }
  }
  if (crash.find("\"crashed\":true") != std::string::npos) {
    ESP_LOGE(kTag, "last crash: %s", crash.c_str());
    ESP_LOGE(kTag, "decode: xtensa-esp32s3-elf-addr2line -pfiaC -e eego-a4-alert-panel.elf <pc> <backtrace...>");
  }
}

}  // namespace eego::admin::devlog
