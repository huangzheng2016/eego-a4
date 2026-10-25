// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Loader page resources (defined in loader_page.cpp; served by admin_httpd.cpp).
// Zero data; scripts/styles only from 'self' and EEGO_WEB_APP_ORIGIN.

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
extern const char kLoaderCsp[];   // Content-Security-Policy for GET /

}  // namespace eego::admin::pages
