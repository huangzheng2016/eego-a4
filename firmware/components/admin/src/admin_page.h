// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// 静态管理页资源（admin_page.cpp 定义；仅供 admin_httpd.cpp 注册路由）。
// 全部内容零数据、零外部资源；CSP default-src 'self'（M13 T11）。
// 所有告警字段一律 textContent 渲染（防存储型 XSS 偷管理令牌）。

namespace eego::admin::pages {

extern const char kAdminHtml[];   // GET /        —— 中英双语最小管理页
extern const char kAdminJs[];     // GET /admin.js
extern const char kAdminCss[];    // GET /admin.css

}  // namespace eego::admin::pages
