// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// BLE 管理隧道桥：把经 BLE 收到的完整请求（ble/tunnel_frame.h）转发到本机管理 API
// （http://127.0.0.1/api/v1/...），响应完整读入 PSRAM 后再经 BLE 发回。
// 已认证（LE SC 配对码）的链路即管理员：桥为链路签发会话 token 并注入
// Authorization 头，断链即吊销。尚无管理密码（配网门户期间）时只接受
// POST /setup。

#include "esp_err.h"

namespace eego::admin {

esp_err_t startBleBridge();
void stopBleBridge();

}  // namespace eego::admin
