// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// admin 组件内部共享（非公共 API；公共入口见 include/admin/admin.h）。
// 跨翻译单元共享注入点存储与事件回调，避免各自 static 副本。

#include "admin/admin_types.h"

namespace eego::admin::internal {

// 注入点存储（admin.cpp 定义；setXxx 写入，httpd/portal 读取）
const AlertSnapshotSource& snapshotSource();
const AlertActionSink& actionSink();
AdminEventCallback eventCallback();
void* eventCtx();
alert_core::TypeRegistry* registry();
void setRegistry(alert_core::TypeRegistry* reg);

}  // namespace eego::admin::internal
