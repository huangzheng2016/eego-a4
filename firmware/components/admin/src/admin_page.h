// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// 静态管理页资源（admin_page.cpp 定义；仅供 admin_httpd.cpp 注册路由）。
// 全部内容零数据、零外部资源；CSP default-src 'self'（M13 T11）。
// 所有告警字段一律 textContent 渲染（防存储型 XSS 偷管理令牌）。

// Base URL of the externally hosted web app (GitHub Pages). Override at build
// time with -DEEGO_WEB_APP_ORIGIN=... -DEEGO_WEB_APP_BASE=... (e.g. via
// target_compile_definitions); BASE must start with ORIGIN, no trailing slash.
#ifndef EEGO_WEB_APP_ORIGIN
#define EEGO_WEB_APP_ORIGIN "https://huangzheng2016.github.io"
#endif
#ifndef EEGO_WEB_APP_BASE
#define EEGO_WEB_APP_BASE EEGO_WEB_APP_ORIGIN "/eego-a4"
#endif

namespace eego::admin::pages {

// GET / : tiny loader that pulls app.css/app.js from EEGO_WEB_APP_BASE; the
// app then runs at the device origin and calls /api/v1 same-origin.
extern const char kLoaderHtml[];  // GET /
extern const char kLoaderJs[];    // GET /loader.js
extern const char kLoaderCsp[];   // Content-Security-Policy for GET / only

extern const char kAdminHtml[];   // GET /legacy  —— 内置中英双语管理页（离线兜底）
extern const char kAdminJs[];     // GET /admin.js
extern const char kAdminCss[];    // GET /admin.css

}  // namespace eego::admin::pages
