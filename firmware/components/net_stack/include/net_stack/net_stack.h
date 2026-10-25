// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// net_stack 聚合头 —— FW3 网络栈与来源适配器（M16）+ FW3B 告警动作与快照（M20）
// + FW5B 静音规则与时间同步（M21）。
//
// 组件边界（M12 §2）：main → {admin, alert_core, diag} → net_stack →
// {eego_ui, eego_hal} → board；net_stack 不得 include 任何 UI 头。
// 本组件只依赖 alert_core（纯 C++ 领域核）、eego_hal（SD 挂载守卫）与
// ESP-IDF 系统组件。没有告警源轮询器：拉取统一走 dashboard data_pipeline
// （NetStackTransport → httpSubmit），外部告警只经推送/注入进入 store。
//
// 快速上手（app 组合根，M18 接线）：
//   NetStack::instance().start(cfg, &store, &registry, time_fn);
//   NetStack::instance().setEventCallback(on_net_event, &app);
// httpd 推送摄入（admin，M17）：
//   #include "net_stack/http_push_bridge.h"
//   auto r = eego::net::httpPushIngest(body, client_ip);
// 告警动作与快照（M20；admin 注入点，未接线 503 不崩溃）：
//   cfg.snapshot_publisher = &pub;                  // start 前注入
//   admin.setAlertSnapshotSource(net.alertSnapshotSource());
//   admin.setAlertActionSink(net.alertActionSink());
// 静音规则与时间同步（M21）：
//   cfg.rtc_write_fn = ...;                         // SNTP 成功后回写 PCF8563
//   cfg.time_valid_fn = ...;                        // 统一时间源可信判定
//   net.setMuteRules(rules, err);                   // net_task 执行 + 快照发布
//   net.queryMuteRules(out);                        // 当前规则表

#include "net_stack/alert_actions.h"
#include "net_stack/credentials.h"
#include "net_stack/http_pool.h"
#include "net_stack/http_push_bridge.h"
#include "net_stack/net_task.h"
#include "net_stack/sd_alert_file_io.h"
#include "net_stack/net_status.h"
#include "net_stack/time_sync.h"
#include "net_stack/wifi_manager.h"
