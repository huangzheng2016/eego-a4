// SPDX-License-Identifier: GPL-3.0-or-later
// AdminCommand 队列实现 —— 见 include/admin/admin_commands.h。
//
// 所有权：命令对象为 shared_ptr，队列只传指针的堆副本；httpd（入队等结果）
// 与 app_task（serviceCommands 执行）各持一份引用，后释放者回收。
// 等待超时不会 use-after-free：执行方写 res 在 give 之前，httpd 只在
// take 成功后读 res。
#include "admin/admin_commands.h"

#include <memory>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

namespace eego::admin {

namespace {

struct RawCmd {
  AdminCommand cmd;
  AdminResult res;
  SemaphoreHandle_t done = nullptr;
  ~RawCmd() {
    if (done != nullptr) vSemaphoreDelete(done);
  }
};
using RawCmdPtr = std::shared_ptr<RawCmd>;

QueueHandle_t cmdQueue() {
  // 静态局部初始化线程安全；队列生命周期 = 组件生命周期。
  // 队列元素为 RawCmdPtr*（堆副本指针，双方释放后 shared_ptr 回收）。
  static QueueHandle_t q = xQueueCreate(8, sizeof(RawCmdPtr*));
  return q;
}

AdminCommandHandler g_handler = nullptr;
void* g_ctx = nullptr;

}  // namespace

void setAdminCommandHandler(AdminCommandHandler handler, void* ctx) {
  g_handler = handler;
  g_ctx = ctx;
}

bool serviceCommands(uint32_t wait_ms) {
  QueueHandle_t q = cmdQueue();
  if (q == nullptr || g_handler == nullptr) return false;
  RawCmdPtr* slot = nullptr;
  if (xQueueReceive(q, &slot, pdMS_TO_TICKS(wait_ms)) != pdTRUE || slot == nullptr) {
    return false;
  }
  RawCmdPtr raw = *slot;
  delete slot;  // 释放队列中的堆副本；raw 仍持有对象
  g_handler(raw->cmd, raw->res, g_ctx);
  xSemaphoreGive(raw->done);
  return true;
}

bool enqueueAdminCommand(const AdminCommand& cmd, AdminResult& res, uint32_t wait_ms) {
  QueueHandle_t q = cmdQueue();
  if (q == nullptr || g_handler == nullptr) return false;

  RawCmdPtr raw = std::make_shared<RawCmd>();
  raw->cmd = cmd;
  raw->done = xSemaphoreCreateBinary();
  if (raw->done == nullptr) return false;

  RawCmdPtr* slot = new (std::nothrow) RawCmdPtr(raw);
  if (slot == nullptr) return false;
  if (xQueueSend(q, &slot, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
    delete slot;
    return false;
  }
  const bool got = (xSemaphoreTake(raw->done, pdMS_TO_TICKS(wait_ms)) == pdTRUE);
  if (got) res = raw->res;
  return got;
}

}  // namespace eego::admin
