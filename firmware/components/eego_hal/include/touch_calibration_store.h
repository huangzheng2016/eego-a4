// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// 触控标定持久化：NVS 命名空间 "touchcal"，键 "affine"（touch_calibration.h 的
// 版本化 + CRC32 编码）。恢复出厂清除整个命名空间。

#include <esp_err.h>

#include "touch_calibration.h"

namespace eego::touchcal {

constexpr const char* kNvsNamespace = "touchcal";
constexpr const char* kNvsKey = "affine";

// 读取已保存标定；无记录/损坏/不合理返回非 ESP_OK（out 不变）。
esp_err_t loadPersisted(Affine& out);
esp_err_t savePersisted(const Affine& affine);
esp_err_t erasePersisted();

}  // namespace eego::touchcal
