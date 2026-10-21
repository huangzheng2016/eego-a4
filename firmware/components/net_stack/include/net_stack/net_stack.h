// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// net_stack 聚合头 —— FW3 网络栈与来源适配器（M16）。
//
// 组件边界（M12 §2）：main → {admin, alert_core, diag} → net_stack →
// {eego_ui, eego_hal} → board；net_stack 不得 include 任何 UI 头。
// 本组件只依赖 alert_core（纯 C++ 领域核）与 ESP-IDF 系统组件。
//
// 快速上手（app 组合根，M18 接线）：
//   NetStack::instance().start(cfg, &store, &registry, time_fn);
//   NetStack::instance().addSource(src_cfg, err);   // admin PUT /config 热生效
//   NetStack::instance().setEventCallback(on_net_event, &app);
// httpd 推送摄入（admin，M17）：
//   #include "net_stack/http_push_bridge.h"
//   auto r = eego::net::httpPushIngest(body, client_ip);

#include "net_stack/credentials.h"
#include "net_stack/http_poll_adapter.h"
#include "net_stack/http_pool.h"
#include "net_stack/http_push_bridge.h"
#include "net_stack/net_task.h"
#include "net_stack/source_adapter.h"
#include "net_stack/source_cache.h"
#include "net_stack/source_config.h"
#include "net_stack/source_health.h"
#include "net_stack/source_presets.h"
#include "net_stack/wifi_manager.h"
