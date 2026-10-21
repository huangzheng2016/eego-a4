// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "esp_http_server.h"

namespace eego::admin::internal {

// Handles /api/v1/dashboard*. apiDispatch has already enforced Host/Origin,
// Bearer authentication, and the appropriate GET/write/test rate limit.
esp_err_t handleDashboardApi(httpd_req_t* req, const char* path);

}  // namespace eego::admin::internal
