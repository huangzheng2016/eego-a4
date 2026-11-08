// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "admin/admin_types.h"

namespace eego::admin::internal {
const AlertSnapshotSource& snapshotSource();
const AlertActionSink& actionSink();
AdminEventCallback eventCallback();
void* eventCtx();
alert_core::TypeRegistry* registry();
void setRegistry(alert_core::TypeRegistry* reg);

}  // namespace eego::admin::internal
