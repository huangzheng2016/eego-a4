// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// 离线 WiFi 诊断（GET /wifi-diag 页面 + /api/v1/wifi/diag JSON），配网门户与管理 API
// 共用。页面零外部资源（CSP default-src 'self'），手机连设备热点即可打开，不需要互联网。
//
// 认证：
//   * 管理 API（已设管理密码，含 AP 兜底热点）：页面本身零数据免认证；JSON/重试/扫描
//     需要 Bearer（页面内输入管理密码登录，token 只存内存）。
//   * 配网门户（尚无管理密码，10 分钟窗口）：与门户其余端点一致免认证（此时没有可用
//     的凭据），仅暴露 SSID/信号/原因码，绝不含密码；重试/扫描限速。

#include <string>

namespace eego::admin::wifi_diag {

extern const char kHtml[];  // GET /wifi-diag
extern const char kJs[];    // GET /wifi-diag.js
extern const char kCss[];   // GET /wifi-diag.css

// 诊断 JSON（已存网络 + 每网络最近结果/原因 + 最近扫描 + 当前连接）。不含密码。
std::string diagJson();

// POST retry：立即按列表重连（net_task 退避复位）。
bool retryNow();

// POST scan：立即扫描（正在连接时返回 false，诊断保留上一轮扫描）。
bool scanNow();

}  // namespace eego::admin::wifi_diag
