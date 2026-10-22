// SPDX-License-Identifier: GPL-3.0-or-later
// SourceConfig 解析/序列化实现。
#include "net_stack/source_config.h"

#include <cctype>

#include "net_stack/credentials.h"

namespace eego::net {

namespace {

std::string lowerAscii(const std::string& text) {
  std::string out = text;
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

bool reservedHeader(const std::string& name) {
  const std::string lower = lowerAscii(name);
  return lower == "authorization" || lower == "host" || lower == "content-length";
}

}  // namespace

using alert_core::JsonValue;
using alert_core::ValidationError;

static bool isValidSourceId(const std::string& id) {
  if (id.empty() || id.size() > 64) return false;
  for (char c : id) {
    if (!std::isalnum((unsigned char)c) && c != '-' && c != '_') return false;
  }
  return true;
}

static bool readUint32(const JsonValue& obj, const char* key, uint32_t min_v, uint32_t max_v,
                       uint32_t& out, ValidationError& err) {
  const JsonValue* v = obj.find(key);
  if (!v) return true;  // 缺省 = 调用方预设
  if (v->type() != JsonValue::Type::Int) {
    err.field = key;
    err.message = "must be an integer";
    return false;
  }
  int64_t n = v->intValue();
  if (n < (int64_t)min_v || n > (int64_t)max_v) {
    err.field = key;
    err.message = "out of range";
    return false;
  }
  out = (uint32_t)n;
  return true;
}

static bool readString(const JsonValue& obj, const char* key, std::string& out,
                       ValidationError& err) {
  const JsonValue* v = obj.find(key);
  if (!v) return true;
  if (v->type() != JsonValue::Type::String) {
    err.field = key;
    err.message = "must be a string";
    return false;
  }
  out = v->stringValue();
  return true;
}

bool SourceConfig::fromJson(const JsonValue& obj, SourceConfig& out, ValidationError& err) {
  if (obj.type() != JsonValue::Type::Object) {
    err.field = "";
    err.message = "source must be an object";
    return false;
  }
  out = SourceConfig();

  if (!readString(obj, "id", out.id, err)) return false;
  if (!readString(obj, "kind", out.kind, err)) return false;
  if (!readString(obj, "url", out.url, err)) return false;
  if (!readString(obj, "auth_token_ref", out.auth_token_ref, err)) return false;
  if (!readString(obj, "array_path", out.array_path, err)) return false;
  if (!readString(obj, "default_type", out.default_type, err)) return false;

  const JsonValue* en = obj.find("enabled");
  if (en) {
    if (en->type() != JsonValue::Type::Bool) {
      err.field = "enabled";
      err.message = "must be a boolean";
      return false;
    }
    out.enabled = en->boolValue();
  }
  const JsonValue* tv = obj.find("tls_verify");
  if (tv) {
    if (tv->type() != JsonValue::Type::Bool) {
      err.field = "tls_verify";
      err.message = "must be a boolean";
      return false;
    }
    out.tls_verify = tv->boolValue();
  }

  if (!readUint32(obj, "interval_ms", kSourceIntervalMinMs, kSourceIntervalMaxMs,
                  out.interval_ms, err))
    return false;
  if (!readUint32(obj, "timeout_ms", 1000, 120000, out.timeout_ms, err)) return false;
  if (!readUint32(obj, "max_body_bytes", 1024, 512 * 1024, out.max_body_bytes, err))
    return false;
  if (!readUint32(obj, "clear_after_misses", 0, 1000, out.clear_after_misses, err))
    return false;

  const JsonValue* fm = obj.find("field_map");
  if (fm) {
    if (fm->type() != JsonValue::Type::Object) {
      err.field = "field_map";
      err.message = "must be an object";
      return false;
    }
    for (const auto& kv : fm->objectItems()) {
      if (kv.second.type() != JsonValue::Type::String) {
        err.field = "field_map." + kv.first;
        err.message = "must be a string";
        return false;
      }
      out.field_map[kv.first] = kv.second.stringValue();
    }
  }

  const JsonValue* sm = obj.find("severity_map");
  if (sm) {
    if (sm->type() != JsonValue::Type::Object) {
      err.field = "severity_map";
      err.message = "must be an object";
      return false;
    }
    for (const auto& kv : sm->objectItems()) {
      if (kv.second.type() != JsonValue::Type::String) {
        err.field = "severity_map." + kv.first;
        err.message = "must be a string";
        return false;
      }
      out.severity_map[kv.first] = kv.second.stringValue();
    }
  }

  const JsonValue* dk = obj.find("dedup_key");
  if (dk) {
    if (dk->type() != JsonValue::Type::Array) {
      err.field = "dedup_key";
      err.message = "must be an array of strings";
      return false;
    }
    for (const auto& item : dk->arrayItems()) {
      if (item.type() != JsonValue::Type::String) {
        err.field = "dedup_key";
        err.message = "must be an array of strings";
        return false;
      }
      out.dedup_key.push_back(item.stringValue());
    }
    if (out.dedup_key.size() > alert_core::kMaxDedupKeys) {
      err.field = "dedup_key";
      err.message = "too many keys";
      return false;
    }
  }

  const JsonValue* cs = obj.find("clear_states");
  if (cs) {
    if (cs->type() != JsonValue::Type::Array) {
      err.field = "clear_states";
      err.message = "must be an array of strings";
      return false;
    }
    for (const auto& item : cs->arrayItems()) {
      if (item.type() != JsonValue::Type::String || item.stringValue().empty() ||
          out.clear_states.size() >= 8) {
        err.field = "clear_states";
        err.message = "must be an array of ≤8 non-empty strings";
        return false;
      }
      out.clear_states.push_back(item.stringValue());
    }
  }

  const JsonValue* hd = obj.find("headers");
  if (hd) {
    if (hd->type() != JsonValue::Type::Object || hd->objectItems().size() > 16) {
      err.field = "headers";
      err.message = "must be a bounded object";
      return false;
    }
    for (const auto& kv : hd->objectItems()) {
      if (kv.second.type() != JsonValue::Type::String) {
        err.field = "headers." + kv.first;
        err.message = "must be a string";
        return false;
      }
      if (reservedHeader(kv.first) || CredentialStore::sensitiveHeaderName(kv.first)) {
        err.field = "headers." + kv.first;
        err.message = "sensitive header values must use auth_token_ref or header_secret_refs";
        return false;
      }
      out.headers.push_back({kv.first, kv.second.stringValue()});
    }
  }

  const JsonValue* secret_headers = obj.find("header_secret_refs");
  if (secret_headers) {
    if (secret_headers->type() != JsonValue::Type::Object ||
        secret_headers->objectItems().size() > 16) {
      err.field = "header_secret_refs";
      err.message = "must be a bounded object of source credential refs";
      return false;
    }
    for (const auto& kv : secret_headers->objectItems()) {
      if (kv.second.type() != JsonValue::Type::String) {
        err.field = "header_secret_refs." + kv.first;
        err.message = "must be a source credential ref";
        return false;
      }
      out.header_secret_refs.push_back({kv.first, kv.second.stringValue()});
    }
  }

  std::string verr = out.validate();
  if (!verr.empty()) {
    err.field = "";
    err.message = verr;
    return false;
  }
  return true;
}

std::string SourceConfig::validate() const {
  if (!isValidSourceId(id)) return "invalid source id (need [a-z0-9_-]{1,64})";
  if (kind.empty() || kind.size() > 32) return "invalid kind";
  if (kind == "http_poll") {
    if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0) {
      return "url must start with http:// or https://";
    }
  }
  if (!auth_token_ref.empty() && !CredentialStore::validSourceRef(auth_token_ref)) {
    return "auth_token_ref must use nvs:source/<key>; re-create legacy refs via credentials API";
  }
  if (headers.size() + header_secret_refs.size() > 16) return "too many HTTP headers";
  for (size_t i = 0; i < headers.size(); ++i) {
    const auto& header = headers[i];
    if (!CredentialStore::validHeaderName(header.name) || reservedHeader(header.name) ||
        CredentialStore::sensitiveHeaderName(header.name) ||
        !CredentialStore::safeHeaderValue(header.value) || header.value.size() > 1024) {
      return "plain headers must be non-sensitive and safely bounded";
    }
    for (size_t j = 0; j < i; ++j) {
      if (lowerAscii(header.name) == lowerAscii(headers[j].name)) return "duplicate HTTP header";
    }
  }
  for (size_t i = 0; i < header_secret_refs.size(); ++i) {
    const auto& header = header_secret_refs[i];
    if (!CredentialStore::validHeaderName(header.name) || reservedHeader(header.name) ||
        !CredentialStore::validSourceRef(header.value)) {
      return "header_secret_refs requires safe names and nvs:source/<key> refs";
    }
    for (size_t j = 0; j < i; ++j) {
      if (lowerAscii(header.name) == lowerAscii(header_secret_refs[j].name)) {
        return "duplicate secret HTTP header";
      }
    }
    for (const auto& plain : headers) {
      if (lowerAscii(header.name) == lowerAscii(plain.name)) return "ambiguous HTTP header";
    }
  }
  return {};
}

JsonValue SourceConfig::toJson() const {
  JsonValue::Object o;
  o.emplace_back("id", JsonValue::makeString(id));
  o.emplace_back("kind", JsonValue::makeString(kind));
  o.emplace_back("enabled", JsonValue::makeBool(enabled));
  if (!url.empty()) o.emplace_back("url", JsonValue::makeString(url));
  o.emplace_back("interval_ms", JsonValue::makeInt(interval_ms));
  if (!auth_token_ref.empty())
    o.emplace_back("auth_token_ref", JsonValue::makeString(auth_token_ref));
  o.emplace_back("tls_verify", JsonValue::makeBool(tls_verify));
  o.emplace_back("clear_after_misses", JsonValue::makeInt(clear_after_misses));
  if (!array_path.empty()) o.emplace_back("array_path", JsonValue::makeString(array_path));
  if (!field_map.empty()) {
    JsonValue::Object m;
    for (const auto& kv : field_map) m.emplace_back(kv.first, JsonValue::makeString(kv.second));
    o.emplace_back("field_map", JsonValue::makeObject(std::move(m)));
  }
  if (!severity_map.empty()) {
    JsonValue::Object m;
    for (const auto& kv : severity_map)
      m.emplace_back(kv.first, JsonValue::makeString(kv.second));
    o.emplace_back("severity_map", JsonValue::makeObject(std::move(m)));
  }
  if (!dedup_key.empty()) {
    JsonValue::Array a;
    for (const auto& k : dedup_key) a.push_back(JsonValue::makeString(k));
    o.emplace_back("dedup_key", JsonValue::makeArray(std::move(a)));
  }
  if (!clear_states.empty()) {
    JsonValue::Array a;
    for (const auto& k : clear_states) a.push_back(JsonValue::makeString(k));
    o.emplace_back("clear_states", JsonValue::makeArray(std::move(a)));
  }
  if (!default_type.empty()) o.emplace_back("default_type", JsonValue::makeString(default_type));
  if (!headers.empty()) {
    JsonValue::Object m;
    for (const auto& h : headers) m.emplace_back(h.name, JsonValue::makeString(h.value));
    o.emplace_back("headers", JsonValue::makeObject(std::move(m)));
  }
  if (!header_secret_refs.empty()) {
    JsonValue::Object m;
    for (const auto& h : header_secret_refs) m.emplace_back(h.name, JsonValue::makeString(h.value));
    o.emplace_back("header_secret_refs", JsonValue::makeObject(std::move(m)));
  }
  o.emplace_back("timeout_ms", JsonValue::makeInt(timeout_ms));
  o.emplace_back("max_body_bytes", JsonValue::makeInt(max_body_bytes));
  return JsonValue::makeObject(std::move(o));
}

bool buildSourceRequest(const SourceConfig& cfg, NetRequest& out, std::string& error) {
  error = cfg.validate();
  if (!error.empty()) return false;
  out = NetRequest{};
  out.method = "GET";
  out.url = cfg.url;
  out.timeout_ms = cfg.timeout_ms;
  out.tls_verify = cfg.tls_verify;
  out.conditional_get = true;
  out.headers = cfg.headers;
  const auto resolve = [&](const std::string& ref, std::string& value) {
    const esp_err_t err = CredentialStore::resolveSourceCredential(ref, value);
    if (err != ESP_OK) error = "configured source credential is unavailable";
    return err == ESP_OK;
  };
  if (!cfg.auth_token_ref.empty()) {
    std::string token;
    if (!resolve(cfg.auth_token_ref, token)) {
      CredentialStore::secureErase(token);
      return false;
    }
    out.headers.push_back({"Authorization", "Bearer " + token});
    CredentialStore::secureErase(token);
  }
  for (const auto& header : cfg.header_secret_refs) {
    std::string secret;
    if (!resolve(header.value, secret)) {
      CredentialStore::secureErase(secret);
      return false;
    }
    out.headers.push_back({header.name, std::move(secret)});
    CredentialStore::secureErase(secret);
  }
  return true;
}

}  // namespace eego::net
