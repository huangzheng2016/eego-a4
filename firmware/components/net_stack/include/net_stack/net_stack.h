// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

//
//
//   NetStack::instance().start(cfg, &store, &registry, time_fn);
//   NetStack::instance().setEventCallback(on_net_event, &app);
//   #include "net_stack/http_push_bridge.h"
//   auto r = eego::net::httpPushIngest(body, client_ip);
//   admin.setAlertSnapshotSource(net.alertSnapshotSource());
//   admin.setAlertActionSink(net.alertActionSink());

#include "net_stack/alert_actions.h"
#include "net_stack/credentials.h"
#include "net_stack/http_pool.h"
#include "net_stack/http_push_bridge.h"
#include "net_stack/net_task.h"
#include "net_stack/sd_alert_file_io.h"
#include "net_stack/net_status.h"
#include "net_stack/time_sync.h"
#include "net_stack/wifi_manager.h"
