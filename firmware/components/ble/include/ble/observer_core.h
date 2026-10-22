// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "alert_core/types.h"
#include "device_settings/settings_model.h"

namespace eego::ble {

struct Advertisement {
  std::array<uint8_t, 6> address{};
  int rssi = 0;
  bool has_manufacturer = false;
  uint16_t manufacturer_id = 0;
  std::vector<uint8_t> manufacturer_payload;
  bool has_service_data = false;
  std::string service_uuid;
  std::vector<uint8_t> service_payload;
};

struct Observation {
  alert_core::AlertSource source;
  alert_core::AlertEvent event;
};

struct ObserverProcessResult {
  size_t matched = 0;
  size_t emitted = 0;
  size_t duplicates = 0;
  size_t malformed = 0;
};

class ObserverCore {
 public:
  void configure(const std::vector<eego::settings::BleObserverRule>& rules,
                 uint32_t dedup_ms);
  ObserverProcessResult process(const Advertisement& advertisement, uint64_t now_ms,
                                std::vector<Observation>& out);

 private:
  struct DedupEntry {
    bool used = false;
    uint64_t hash = 0;
    uint64_t emitted_at_ms = 0;
  };

  bool duplicate(uint64_t hash, uint64_t now_ms);
  std::vector<eego::settings::BleObserverRule> rules_;
  uint32_t dedup_ms_ = 5000;
  DedupEntry dedup_[16]{};
};

}  // namespace eego::ble
