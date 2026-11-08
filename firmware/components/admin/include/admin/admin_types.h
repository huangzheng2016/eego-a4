// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

//
//

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "alert_core/snapshot.h"

namespace eego::admin {
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
enum class AdminEventType : uint8_t {
  PortalOpened = 0,
  PortalClosed,
  ApFallbackOpened,
  ApFallbackClosed,
  ProvisionSucceeded,
  ProvisionConnecting,
  ProvisionFailed,
};

struct AdminEvent {
  AdminEventType type = AdminEventType::PortalOpened;
  const char* reason = "";
  uint32_t window_remaining_s = 0;
};

using AdminEventCallback = void (*)(const AdminEvent& ev, void* ctx);

// ---------------------------------------------------------------------------
//
// ---------------------------------------------------------------------------

using AlertSnapshotSource =
    std::function<std::shared_ptr<const alert_core::AlertSnapshot>()>;

enum class AlertActionKind : uint8_t {
  Ack = 0,
  AckAll,
  Clear,
};

enum class AlertActionResultCode : uint8_t {
  Ok = 0,
  NotFound,
  InvalidState,
  InvalidAction,
  Unavailable,
  NoStore,
};

struct AlertActionResult {
  AlertActionResultCode code = AlertActionResultCode::Unavailable;
  size_t affected = 0;
  std::string detail;
};

using AlertActionSink = std::function<AlertActionResult(
    AlertActionKind kind, const std::string& id, const std::string& ack_by)>;

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
namespace errc {
inline constexpr const char* kInvalidRequest = "invalid_request";        // 400
inline constexpr const char* kValidationFailed = "validation_failed";    // 400
inline constexpr const char* kUnauthorized = "unauthorized";             // 401
inline constexpr const char* kForbidden = "forbidden";                   // 403
inline constexpr const char* kNotFound = "not_found";                    // 404
inline constexpr const char* kConflict = "conflict";                     // 409
inline constexpr const char* kPayloadTooLarge = "payload_too_large";     // 413
inline constexpr const char* kUnsupportedMediaType = "unsupported_media_type";  // 415
inline constexpr const char* kTooManyRequests = "too_many_requests";     // 429
inline constexpr const char* kInternal = "internal_error";               // 500
inline constexpr const char* kStoreFull = "store_full";                  // 503
inline constexpr const char* kUnavailable = "unavailable";               // 503
}  // namespace errc

}  // namespace eego::admin
