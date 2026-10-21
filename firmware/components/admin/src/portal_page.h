// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// 配网门户静态资源（portal_page.cpp 定义；仅供 provision_portal.cpp 使用）。
// 中英双语；所有网络名/错误文本一律 textContent 渲染。

namespace eego::admin::pages {

extern const char kPortalHtml[];   // GET /（门户首页，两步：WiFi → 管理密码）
extern const char kPortalJs[];     // GET /portal.js
extern const char kPortalCss[];    // GET /portal.css

}  // namespace eego::admin::pages
