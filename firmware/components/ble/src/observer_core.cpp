// SPDX-License-Identifier: GPL-3.0-or-later
#include "ble/observer_core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace eego::ble {

namespace {

uint64_t fnv1a(const uint8_t* data, size_t size, uint64_t hash) {
  for (size_t i = 0; i < size; i++) {
    hash ^= data[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

uint64_t fnv1a(const std::string& text, uint64_t hash) {
  return fnv1a(reinterpret_cast<const uint8_t*>(text.data()), text.size(), hash);
}

bool payloadMatch(const eego::settings::BleObserverRule& rule,
                  const std::vector<uint8_t>& payload) {
  const size_t begin = rule.payload_offset;
  return begin <= payload.size() && rule.payload_match.size() <= payload.size() - begin &&
         std::equal(rule.payload_match.begin(), rule.payload_match.end(), payload.begin() + begin);
}

bool fieldValue(const eego::settings::BleFieldMap& field, const std::vector<uint8_t>& payload,
                double& out) {
  const size_t offset = field.offset;
  size_t width = 0;
  switch (field.type) {
    case eego::settings::BleValueType::UInt8:
    case eego::settings::BleValueType::Int8: width = 1; break;
    case eego::settings::BleValueType::UInt16Le:
    case eego::settings::BleValueType::Int16Le: width = 2; break;
    case eego::settings::BleValueType::UInt32Le:
    case eego::settings::BleValueType::Int32Le:
    case eego::settings::BleValueType::Float32Le: width = 4; break;
  }
  if (offset > payload.size() || width > payload.size() - offset) return false;
  const uint8_t* data = payload.data() + offset;
  uint32_t raw = 0;
  for (size_t i = 0; i < width; i++) raw |= static_cast<uint32_t>(data[i]) << (i * 8);
  double value = 0.0;
  switch (field.type) {
    case eego::settings::BleValueType::UInt8: value = data[0]; break;
    case eego::settings::BleValueType::Int8: {
      const int8_t signed_value = static_cast<int8_t>(data[0]);
      value = signed_value;
      break;
    }
    case eego::settings::BleValueType::UInt16Le: value = raw; break;
    case eego::settings::BleValueType::Int16Le: {
      const int16_t signed_value = static_cast<int16_t>(raw & 0xffffu);
      value = signed_value;
      break;
    }
    case eego::settings::BleValueType::UInt32Le: value = raw; break;
    case eego::settings::BleValueType::Int32Le: {
      const int32_t signed_value = static_cast<int32_t>(raw);
      value = signed_value;
      break;
    }
    case eego::settings::BleValueType::Float32Le: {
      float float_value = 0.0f;
      static_assert(sizeof(float_value) == sizeof(raw), "32-bit float required");
      std::memcpy(&float_value, &raw, sizeof(raw));
      if (!std::isfinite(float_value)) return false;
      value = float_value;
      break;
    }
  }
  value *= field.scale;
  if (!std::isfinite(value)) return false;
  out = value;
  return true;
}

std::string addressText(const std::array<uint8_t, 6>& address) {
  char text[18];
  std::snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", address[0], address[1],
                address[2], address[3], address[4], address[5]);
  return text;
}

}  // namespace

void ObserverCore::configure(const std::vector<eego::settings::BleObserverRule>& rules,
                             uint32_t dedup_ms) {
  rules_ = rules;
  dedup_ms_ = dedup_ms;
  for (auto& entry : dedup_) entry = DedupEntry{};
}

ObserverProcessResult ObserverCore::process(const Advertisement& advertisement, uint64_t now_ms,
                                            std::vector<Observation>& out) {
  out.clear();
  ObserverProcessResult result;
  const std::string address = addressText(advertisement.address);
  for (const auto& rule : rules_) {
    const std::vector<uint8_t>* payload = nullptr;
    if (rule.manufacturer_id >= 0 && advertisement.has_manufacturer &&
        rule.manufacturer_id == advertisement.manufacturer_id) {
      payload = &advertisement.manufacturer_payload;
    } else if (rule.manufacturer_id < 0 && advertisement.has_service_data &&
               rule.service_uuid == advertisement.service_uuid) {
      payload = &advertisement.service_payload;
    }
    if (payload == nullptr || !payloadMatch(rule, *payload)) continue;
    result.matched++;

    alert_core::JsonValue::Object fields;
    bool valid = true;
    for (const auto& mapping : rule.fields) {
      double value = 0.0;
      if (!fieldValue(mapping, *payload, value)) {
        valid = false;
        break;
      }
      fields.emplace_back(mapping.name, alert_core::JsonValue::makeDouble(value));
    }
    if (!valid) {
      result.malformed++;
      continue;
    }
    fields.emplace_back("device", alert_core::JsonValue::makeString(address));
    fields.emplace_back("rssi", alert_core::JsonValue::makeInt(advertisement.rssi));
    fields.emplace_back("rule", alert_core::JsonValue::makeString(rule.id));

    uint64_t hash = 1469598103934665603ULL;
    hash = fnv1a(advertisement.address.data(), advertisement.address.size(), hash);
    hash = fnv1a(rule.id, hash);
    hash = fnv1a(payload->data(), payload->size(), hash);
    if (duplicate(hash, now_ms)) {
      result.duplicates++;
      continue;
    }

    Observation observation;
    observation.source = {"ble." + rule.id, alert_core::kSourceKindBleScan, address};
    observation.event.type = rule.event_type;
    observation.event.title = rule.title;
    observation.event.fields = alert_core::JsonValue::makeObject(std::move(fields));
    observation.event.has_ttl = true;
    observation.event.ttl_s = rule.ttl_s;
    observation.event.dedup_key = {"device", "rule"};
    out.push_back(std::move(observation));
    result.emitted++;
    if (out.size() >= eego::settings::kMaxBleObserverRules) break;
  }
  return result;
}

bool ObserverCore::duplicate(uint64_t hash, uint64_t now_ms) {
  size_t free_slot = 16;
  size_t oldest = 0;
  for (size_t i = 0; i < 16; i++) {
    DedupEntry& entry = dedup_[i];
    if (entry.used && entry.hash == hash) {
      if (dedup_ms_ > 0 && now_ms >= entry.emitted_at_ms &&
          now_ms - entry.emitted_at_ms < dedup_ms_) {
        return true;
      }
      entry.emitted_at_ms = now_ms;
      return false;
    }
    if (!entry.used && free_slot == 16) free_slot = i;
    if (entry.used && entry.emitted_at_ms < dedup_[oldest].emitted_at_ms) oldest = i;
  }
  const size_t slot = free_slot < 16 ? free_slot : oldest;
  dedup_[slot] = DedupEntry{true, hash, now_ms};
  return false;
}

}  // namespace eego::ble
