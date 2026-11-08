// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

//
//
//

#include <cstdint>

#include "alert_core/store.h"

namespace eego::net::internal {
uint64_t alertStoreDigest(const alert_core::AlertStore& store);

}  // namespace eego::net::internal
