// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

//

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "alert_core/snapshot.h"

namespace eego::net {
enum class AlertActionType : uint8_t {
  Ack = 0,
  AckAll,
  Clear,
};

struct AlertAction {
  AlertActionType type = AlertActionType::Ack;
  std::string id;
  std::string ack_by;
};

struct AlertActionResult {
  enum class Code {
    Ok = 0,
    NotFound,
    InvalidState,
    InvalidAction,
    Unavailable,
    NoStore,
  } code = Code::Unavailable;
  size_t affected = 0;
  std::string detail;
};

const char* toString(AlertActionResult::Code code);

//
using AlertSnapshotSource = std::function<std::shared_ptr<const alert_core::AlertSnapshot>()>;
using AlertActionSink = std::function<AlertActionResult(const AlertAction&)>;

}  // namespace eego::net
