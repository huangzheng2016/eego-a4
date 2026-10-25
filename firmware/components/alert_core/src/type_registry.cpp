// SPDX-License-Identifier: GPL-3.0-or-later
#include "alert_core/type_registry.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace alert_core {
namespace {

AlertTypeDescriptor makeDesc(std::string id, std::string name_zh, std::string name_en,
                             Severity severity, std::string icon, std::vector<std::string> cols,
                             std::vector<std::string> detail, std::vector<std::string> dedup,
                             std::string group) {
  AlertTypeDescriptor d;
  d.id = std::move(id);
  d.name_zh = std::move(name_zh);
  d.name_en = std::move(name_en);
  d.severity_default = severity;
  d.icon_key = std::move(icon);
  d.table_cols = std::move(cols);
  d.detail_fields = std::move(detail);
  d.dedup_key = std::move(dedup);
  d.ttl_default_s = defaultTtlForSeverity(severity);
  d.group = std::move(group);
  return d;
}

std::vector<AlertTypeDescriptor> buildBuiltins() {
  std::vector<AlertTypeDescriptor> v;
  v.reserve(40);
  // --- system (8) ---
  v.push_back(makeDesc("host.cpu_high", "CPU 使用率过高", "High CPU usage", Severity::Warning,
                       "chip", {"host", "used_pct"},
                       {"host", "used_pct", "threshold", "cores", "load_avg"}, {"host"},
                       "system"));
  v.push_back(makeDesc("host.mem_high", "内存使用率过高", "High memory usage", Severity::Warning,
                       "memory", {"host", "used_pct"},
                       {"host", "used_pct", "threshold", "total_gb", "avail_gb"}, {"host"},
                       "system"));
  v.push_back(makeDesc("host.disk_almost_full", "磁盘将满", "Disk almost full", Severity::Warning,
                       "disk", {"host", "mount", "used_pct"},
                       {"host", "mount", "fs", "used_pct", "free_gb", "threshold"},
                       {"host", "mount"}, "system"));
  v.push_back(makeDesc("host.disk_io_latency", "磁盘 IO 延迟高", "Disk I/O latency high",
                       Severity::Warning, "disk_io", {"host", "device", "latency_ms"},
                       {"host", "device", "latency_ms", "threshold_ms"}, {"host", "device"},
                       "system"));
  v.push_back(makeDesc("host.process_down", "进程停止", "Process down", Severity::Critical,
                       "process", {"host", "process"},
                       {"host", "process", "pid", "port", "cmd"}, {"host", "process"},
                       "system"));
  v.push_back(makeDesc("host.rebooted", "主机重启", "Host rebooted", Severity::Warning, "reboot",
                       {"host", "boot_reason"}, {"host", "boot_reason", "kernel", "uptime_s"},
                       {"host"}, "system"));
  v.push_back(makeDesc("host.time_drift", "系统时间漂移", "Time drift", Severity::Warning, "clock",
                       {"host", "drift_s"}, {"host", "drift_s", "ntp_server"}, {"host"},
                       "system"));
  v.push_back(makeDesc("host.oom_kill", "OOM 进程被杀", "OOM kill", Severity::Critical,
                       "memory_alert", {"host", "process"},
                       {"host", "process", "pid", "rss_mb", "oom_score"}, {"host", "process"},
                       "system"));
  // --- network (6) ---
  v.push_back(makeDesc("net.device_offline", "设备离线", "Device offline", Severity::Critical,
                       "net", {"device", "ip"}, {"device", "ip", "mac", "last_seen"}, {"device"},
                       "network"));
  v.push_back(makeDesc("net.ping_loss", "Ping 丢包", "Ping loss", Severity::Warning, "ping",
                       {"target", "loss_pct"}, {"target", "loss_pct", "rtt_ms", "threshold_pct"},
                       {"target"}, "network"));
  v.push_back(makeDesc("net.latency_high", "延迟过高", "High latency", Severity::Warning,
                       "latency", {"target", "rtt_ms"},
                       {"target", "rtt_ms", "threshold_ms", "loss_pct"}, {"target"}, "network"));
  v.push_back(makeDesc("net.bandwidth_saturation", "带宽饱和", "Bandwidth saturated",
                       Severity::Warning, "bandwidth", {"iface", "used_pct"},
                       {"iface", "used_pct", "in_mbps", "out_mbps", "capacity_mbps"}, {"iface"},
                       "network"));
  v.push_back(makeDesc("net.dhcp_pool_low", "DHCP 地址池不足", "DHCP pool low",
                       Severity::Warning, "dhcp", {"pool", "free"},
                       {"pool", "free", "total", "used_pct"}, {"pool"}, "network"));
  v.push_back(makeDesc("net.cert_expiring", "证书即将过期", "Certificate expiring",
                       Severity::Warning, "cert", {"host", "days_left"},
                       {"host", "port", "days_left", "issuer", "not_after"}, {"host", "port"},
                       "network"));
  // --- service (6) ---
  v.push_back(makeDesc("svc.http_down", "HTTP 服务宕机", "HTTP service down", Severity::Critical,
                       "http", {"url", "http_code"}, {"url", "http_code", "error", "timeout_ms"},
                       {"url"}, "service"));
  v.push_back(makeDesc("svc.http_5xx", "HTTP 5xx 错误", "HTTP 5xx errors", Severity::Critical,
                       "http_error", {"url", "http_code"},
                       {"url", "http_code", "count", "window_s"}, {"url"}, "service"));
  v.push_back(makeDesc("svc.cert_expiring", "服务证书即将过期", "Service certificate expiring",
                       Severity::Warning, "cert", {"domain", "days_left"},
                       {"domain", "days_left", "issuer", "not_after"}, {"domain"}, "service"));
  v.push_back(makeDesc("svc.backup_failed", "备份失败", "Backup failed", Severity::Critical,
                       "backup", {"job", "error"},
                       {"job", "target", "error", "started_at", "duration_s"}, {"job"},
                       "service"));
  v.push_back(makeDesc("svc.backup_ok", "备份成功", "Backup OK", Severity::Info, "backup_ok",
                       {"job", "duration_s"},
                       {"job", "target", "size_gb", "duration_s", "finished_at"}, {"job"},
                       "service"));
  v.push_back(makeDesc("svc.update_available", "有可用更新", "Update available", Severity::Info,
                       "update", {"component", "version"},
                       {"component", "current", "version", "url"}, {"component"}, "service"));
  // --- power_env (5) ---
  v.push_back(makeDesc("ups.on_battery", "UPS 电池供电", "UPS on battery", Severity::Warning,
                       "ups", {"ups", "runtime_s"}, {"ups", "runtime_s", "load_pct", "battery_pct"},
                       {"ups"}, "power_env"));
  v.push_back(makeDesc("ups.battery_low", "UPS 电池电量低", "UPS battery low", Severity::Critical,
                       "battery", {"ups", "battery_pct"},
                       {"ups", "battery_pct", "runtime_s", "threshold_pct"}, {"ups"},
                       "power_env"));
  v.push_back(makeDesc("ups.overload", "UPS 过载", "UPS overload", Severity::Critical, "overload",
                       {"ups", "load_pct"}, {"ups", "load_pct", "rated_w", "threshold_pct"},
                       {"ups"}, "power_env"));
  v.push_back(makeDesc("env.temp_high", "温度过高", "High temperature", Severity::Critical, "temp",
                       {"sensor", "temp_c"}, {"sensor", "temp_c", "threshold_c", "humidity"},
                       {"sensor"}, "power_env"));
  v.push_back(makeDesc("env.sensor_offline", "传感器离线", "Sensor offline", Severity::Warning,
                       "sensor", {"sensor", "last_seen"},
                       {"sensor", "location", "battery_pct", "last_seen"}, {"sensor"},
                       "power_env"));
  // --- security (5) ---
  v.push_back(makeDesc("sec.login_failed_burst", "登录失败激增", "Login failure burst",
                       Severity::Warning, "login", {"user", "count"},
                       {"user", "count", "src_ip", "window_s"}, {"user", "src_ip"}, "security"));
  v.push_back(makeDesc("sec.new_device", "新设备接入", "New device", Severity::Info, "device",
                       {"device", "ip"}, {"device", "ip", "mac", "first_seen"},
                       {"device", "mac"}, "security"));
  v.push_back(makeDesc("sec.port_opened", "端口开放", "Port opened", Severity::Warning, "port",
                       {"host", "port"}, {"host", "port", "proto", "service"}, {"host", "port"},
                       "security"));
  v.push_back(makeDesc("sec.cert_expired", "证书已过期", "Certificate expired",
                       Severity::Critical, "cert_expired", {"host", "expired_days"},
                       {"host", "port", "not_after", "issuer"}, {"host", "port"}, "security"));
  v.push_back(makeDesc("sec.unusual_traffic", "异常流量", "Unusual traffic", Severity::Warning,
                       "traffic", {"host", "direction"},
                       {"host", "direction", "volume_mb", "baseline_mb"}, {"host", "direction"},
                       "security"));
  // --- home (4) ---
  v.push_back(makeDesc("home.door_opened", "门打开", "Door opened", Severity::Warning, "door",
                       {"door", "state"}, {"door", "state", "battery_pct", "since"}, {"door"},
                       "home"));
  v.push_back(makeDesc("home.water_leak", "检测到水浸", "Water leak", Severity::Critical, "water",
                       {"sensor", "location"}, {"sensor", "location", "battery_pct", "since"},
                       {"sensor"}, "home"));
  v.push_back(makeDesc("home.smoke", "烟雾报警", "Smoke detected", Severity::Critical, "smoke",
                       {"sensor", "location"}, {"sensor", "location", "battery_pct", "since"},
                       {"sensor"}, "home"));
  v.push_back(makeDesc("home.motion", "检测到移动", "Motion detected", Severity::Info, "motion",
                       {"sensor", "location"}, {"sensor", "location", "time"}, {"sensor"},
                       "home"));
  // --- self (5) ---
  v.push_back(makeDesc("self.wifi_lost", "WiFi 断开", "WiFi lost", Severity::Critical, "wifi",
                       {"ssid", "since"}, {"ssid", "since", "rssi", "reason"}, {"ssid"}, "self"));
  v.push_back(makeDesc("self.source_stale", "告警源数据停滞", "Source stale", Severity::Warning,
                       "stale", {"source", "since"}, {"source", "kind", "interval_s", "since"},
                       {"source"}, "self"));
  v.push_back(makeDesc("self.store_near_full", "告警存储将满", "Alert store near full",
                       Severity::Warning, "database", {"used_pct"},
                       {"used_pct", "total", "max_records"}, {"store"}, "self"));
  v.push_back(makeDesc("self.battery_low", "设备电量低", "Battery low", Severity::Warning,
                       "battery_low", {"battery_pct"}, {"battery_pct", "voltage", "charging"},
                       {"battery"}, "self"));
  v.push_back(makeDesc("self.config_changed", "配置已更改", "Configuration changed",
                       Severity::Info, "config", {"key", "by"}, {"key", "by", "old", "new"},
                       {"key"}, "self"));
  // --- generic fallback (1) ---
  AlertTypeDescriptor generic = makeDesc("generic.any", "通用告警", "Generic alert",
                                         Severity::Info, "alert", {"source", "title"}, {}, {},
                                         "generic");
  generic.ttl_default_s = 0;  // derive from the event's severity band
  v.push_back(std::move(generic));
  return v;
}

bool validTypeId(const std::string& s) {
  if (s.empty() || s.size() > kMaxTypeIdLen) return false;
  for (char c : s) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '.' || c == '-';
    if (!ok) return false;
  }
  return true;
}

}  // namespace

TypeRegistry::TypeRegistry() : builtins_(buildBuiltins()) {}

const AlertTypeDescriptor* TypeRegistry::findBuiltin(const std::string& type_id) const {
  for (const AlertTypeDescriptor& d : builtins_) {
    if (d.id == type_id) return &d;
  }
  return nullptr;
}

const AlertTypeOverride* TypeRegistry::findOverride(const std::string& type_id) const {
  for (const auto& kv : overrides_) {
    if (kv.first == type_id) return &kv.second;
  }
  return nullptr;
}

bool TypeRegistry::isKnown(const std::string& type_id) const {
  return findBuiltin(type_id) != nullptr;
}

AlertTypeDescriptor TypeRegistry::resolve(const std::string& type_id) const {
  const AlertTypeDescriptor* base = findBuiltin(type_id);
  AlertTypeDescriptor d = base != nullptr ? *base : generic();
  if (base == nullptr) d.id = type_id;  // keep unknown ids distinguishable
  if (const AlertTypeOverride* o = findOverride(type_id)) {
    if (o->has_severity) d.severity_default = o->severity;
    if (o->has_ttl) d.ttl_default_s = o->ttl_s;
    if (o->has_name_zh) d.name_zh = o->name_zh;
    if (o->has_name_en) d.name_en = o->name_en;
    if (o->has_muted) d.muted_default = o->muted;
  }
  return d;
}

bool TypeRegistry::isTypeMuted(const std::string& type_id) const {
  const AlertTypeOverride* o = findOverride(type_id);
  return o != nullptr && o->has_muted && o->muted;
}

void TypeRegistry::setOverride(const std::string& type_id, const AlertTypeOverride& o) {
  if (o.empty()) {
    removeOverride(type_id);
    return;
  }
  for (auto& kv : overrides_) {
    if (kv.first == type_id) {
      kv.second = o;
      return;
    }
  }
  overrides_.emplace_back(type_id, o);
}

bool TypeRegistry::removeOverride(const std::string& type_id) {
  auto it = std::find_if(overrides_.begin(), overrides_.end(),
                         [&](const auto& kv) { return kv.first == type_id; });
  if (it == overrides_.end()) return false;
  overrides_.erase(it);
  return true;
}

void TypeRegistry::clearOverrides() { overrides_.clear(); }

bool TypeRegistry::applyOverridesJson(const JsonValue& obj, ValidationError& err) {
  if (obj.type() != JsonValue::Type::Object) {
    err.field = "";
    err.message = "alert_type_overrides must be a JSON object";
    return false;
  }
  for (const auto& kv : obj.objectItems()) {
    const std::string& id = kv.first;
    if (!validTypeId(id)) {
      err.field = id;
      err.message = "override key must be a type id ([A-Za-z0-9_.-], <= 64 chars)";
      return false;
    }
    if (kv.second.type() != JsonValue::Type::Object) {
      err.field = id;
      err.message = "override value must be an object";
      return false;
    }
    AlertTypeOverride o;
    // Optional members: explicit null is treated the same as absent.
    if (const JsonValue* v = kv.second.find("severity"); v != nullptr && !v->isNull()) {
      if (v->type() != JsonValue::Type::String ||
          !severityFromString(v->stringValue(), o.severity)) {
        err.field = id + ".severity";
        err.message = "severity must be one of critical|warning|info";
        return false;
      }
      o.has_severity = true;
    }
    if (const JsonValue* v = kv.second.find("ttl_s"); v != nullptr && !v->isNull()) {
      // Same strictness as the event path: integer-valued only and in range;
      // checks run before any cast (double→int64 is UB for inf/out-of-range).
      const double d = v->doubleValue(-1.0);
      if ((v->type() != JsonValue::Type::Int && v->type() != JsonValue::Type::Double) ||
          !std::isfinite(d) || d < 1.0 || d > static_cast<double>(kMaxTtlSeconds) ||
          d != std::trunc(d)) {
        err.field = id + ".ttl_s";
        err.message = "ttl_s must be an integer in 1.." + std::to_string(kMaxTtlSeconds);
        return false;
      }
      o.has_ttl = true;
      o.ttl_s = static_cast<uint32_t>(d);
    }
    if (const JsonValue* v = kv.second.find("name_zh"); v != nullptr && !v->isNull()) {
      if (v->type() != JsonValue::Type::String || v->stringValue().empty() ||
          v->stringValue().size() > 64) {
        err.field = id + ".name_zh";
        err.message = "name_zh must be a non-empty string of at most 64 bytes";
        return false;
      }
      o.has_name_zh = true;
      o.name_zh = v->stringValue();
    }
    if (const JsonValue* v = kv.second.find("name_en"); v != nullptr && !v->isNull()) {
      if (v->type() != JsonValue::Type::String || v->stringValue().empty() ||
          v->stringValue().size() > 64) {
        err.field = id + ".name_en";
        err.message = "name_en must be a non-empty string of at most 64 bytes";
        return false;
      }
      o.has_name_en = true;
      o.name_en = v->stringValue();
    }
    if (const JsonValue* v = kv.second.find("muted"); v != nullptr && !v->isNull()) {
      if (v->type() != JsonValue::Type::Bool) {
        err.field = id + ".muted";
        err.message = "muted must be a boolean";
        return false;
      }
      o.has_muted = true;
      o.muted = v->boolValue();
    }
    setOverride(id, o);
  }
  return true;
}

const AlertTypeDescriptor& TypeRegistry::generic() {
  static const AlertTypeDescriptor kGeneric = [] {
    AlertTypeDescriptor d = makeDesc("generic.any", "通用告警", "Generic alert", Severity::Info,
                                     "alert", {"source", "title"}, {}, {}, "generic");
    d.ttl_default_s = 0;
    return d;
  }();
  return kGeneric;
}

}  // namespace alert_core
