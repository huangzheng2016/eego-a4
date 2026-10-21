#include "alert_core/types.h"

#include <algorithm>
#include <cctype>

#include "alert_core/sha1.h"
#include "alert_core/time_util.h"

namespace alert_core {
namespace {

bool isKeyChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
         c == '_' || c == '.' || c == '-';
}

bool validKeyString(const std::string& s, size_t max_len) {
  if (s.empty() || s.size() > max_len) return false;
  for (char c : s) {
    if (!isKeyChar(c)) return false;
  }
  return true;
}

bool isHex40(const std::string& s) {
  if (s.size() != kFingerprintHexLen) return false;
  for (char c : s) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
  }
  return true;
}

bool fail(ValidationError& err, const std::string& field, const std::string& message) {
  err.field = field;
  err.message = message;
  return false;
}

bool parseEventObject(const JsonValue& obj, AlertEvent& out, ValidationError& err,
                      const std::string& prefix) {
  if (obj.type() != JsonValue::Type::Object) {
    return fail(err, prefix, "event must be a JSON object");
  }

  const JsonValue* type = obj.find("type");
  if (type == nullptr) {
    return fail(err, prefix + "type", "missing required field \"type\"");
  }
  if (type->type() != JsonValue::Type::String ||
      !validKeyString(type->stringValue(), kMaxTypeIdLen)) {
    return fail(err, prefix + "type",
                "type must be a string of 1.." + std::to_string(kMaxTypeIdLen) +
                    " chars from [A-Za-z0-9_.-]");
  }
  out.type = type->stringValue();

  // Optional members: an explicit null is treated the same as absent (the M11
  // event schema example carries "fingerprint": null).
  if (const JsonValue* sev = obj.find("severity"); sev != nullptr && !sev->isNull()) {
    if (sev->type() != JsonValue::Type::String ||
        !severityFromString(sev->stringValue(), out.severity)) {
      return fail(err, prefix + "severity", "severity must be one of critical|warning|info");
    }
    out.has_severity = true;
  }

  if (const JsonValue* title = obj.find("title"); title != nullptr && !title->isNull()) {
    if (title->type() != JsonValue::Type::String) {
      return fail(err, prefix + "title", "title must be a string");
    }
    if (title->stringValue().size() > kMaxTitleLen) {
      return fail(err, prefix + "title",
                  "title exceeds " + std::to_string(kMaxTitleLen) + " bytes");
    }
    out.title = title->stringValue();
  }

  if (const JsonValue* msg = obj.find("message"); msg != nullptr && !msg->isNull()) {
    if (msg->type() != JsonValue::Type::String) {
      return fail(err, prefix + "message", "message must be a string");
    }
    if (msg->stringValue().size() > kMaxMessageLen) {
      return fail(err, prefix + "message",
                  "message exceeds " + std::to_string(kMaxMessageLen) + " bytes");
    }
    out.message = msg->stringValue();
  }

  if (const JsonValue* fields = obj.find("fields"); fields != nullptr && !fields->isNull()) {
    if (fields->type() != JsonValue::Type::Object) {
      return fail(err, prefix + "fields", "fields must be a JSON object");
    }
    if (fields->objectItems().size() > kMaxFieldsCount) {
      return fail(err, prefix + "fields",
                  "fields exceeds " + std::to_string(kMaxFieldsCount) + " keys");
    }
    JsonValue::Object normalized;
    normalized.reserve(fields->objectItems().size());
    for (const auto& kv : fields->objectItems()) {
      if (!validKeyString(kv.first, kMaxFieldKeyLen)) {
        return fail(err, prefix + "fields." + kv.first,
                    "field key must be 1.." + std::to_string(kMaxFieldKeyLen) +
                        " chars from [A-Za-z0-9_.-]");
      }
      if (!kv.second.isScalar()) {
        return fail(err, prefix + "fields." + kv.first, "field value must be a scalar");
      }
      if (kv.second.scalarText().size() > kMaxFieldValueLen) {
        return fail(err, prefix + "fields." + kv.first,
                    "field value exceeds " + std::to_string(kMaxFieldValueLen) + " bytes");
      }
      normalized.emplace_back(kv.first, kv.second);
    }
    out.fields = JsonValue::makeObject(std::move(normalized));
  }

  if (const JsonValue* state = obj.find("state"); state != nullptr && !state->isNull()) {
    if (state->type() != JsonValue::Type::String ||
        (state->stringValue() != "active" && state->stringValue() != "cleared")) {
      return fail(err, prefix + "state", "state must be \"active\" or \"cleared\"");
    }
    out.cleared = state->stringValue() == "cleared";
  }

  if (const JsonValue* ttl = obj.find("ttl_s"); ttl != nullptr && !ttl->isNull()) {
    if (ttl->type() != JsonValue::Type::Int && ttl->type() != JsonValue::Type::Double) {
      return fail(err, prefix + "ttl_s", "ttl_s must be a number of seconds");
    }
    double v = ttl->doubleValue();
    if (v != static_cast<double>(static_cast<int64_t>(v)) || v < 1 ||
        v > static_cast<double>(kMaxTtlSeconds)) {
      return fail(err, prefix + "ttl_s",
                  "ttl_s must be an integer in 1.." + std::to_string(kMaxTtlSeconds));
    }
    out.has_ttl = true;
    out.ttl_s = static_cast<uint32_t>(v);
  }

  if (const JsonValue* dk = obj.find("dedup_key"); dk != nullptr && !dk->isNull()) {
    if (dk->type() != JsonValue::Type::Array) {
      return fail(err, prefix + "dedup_key", "dedup_key must be an array of key names");
    }
    if (dk->arrayItems().size() > kMaxDedupKeys) {
      return fail(err, prefix + "dedup_key",
                  "dedup_key exceeds " + std::to_string(kMaxDedupKeys) + " keys");
    }
    for (const JsonValue& item : dk->arrayItems()) {
      if (item.type() != JsonValue::Type::String ||
          !validKeyString(item.stringValue(), kMaxFieldKeyLen)) {
        return fail(err, prefix + "dedup_key",
                    "dedup_key entries must be key names ([A-Za-z0-9_.-], <= " +
                        std::to_string(kMaxFieldKeyLen) + " chars)");
      }
      out.dedup_key.push_back(item.stringValue());
    }
  }

  if (const JsonValue* fp = obj.find("fingerprint"); fp != nullptr && !fp->isNull()) {
    if (fp->type() != JsonValue::Type::String || !isHex40(fp->stringValue())) {
      return fail(err, prefix + "fingerprint",
                  "fingerprint must be exactly 40 hex chars (sha1)");
    }
    out.fingerprint = fp->stringValue();
    std::transform(out.fingerprint.begin(), out.fingerprint.end(), out.fingerprint.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  }

  if (const JsonValue* ts = obj.find("ts"); ts != nullptr && !ts->isNull()) {
    uint64_t parsed = 0;
    if (ts->type() == JsonValue::Type::String) {
      if (!parseUtcIso8601(ts->stringValue(), parsed)) {
        return fail(err, prefix + "ts", "ts must be UTC ISO8601 (YYYY-MM-DDTHH:MM:SSZ)");
      }
    } else if (ts->type() == JsonValue::Type::Int && ts->intValue() >= 0) {
      parsed = static_cast<uint64_t>(ts->intValue());
    } else {
      return fail(err, prefix + "ts", "ts must be a UTC ISO8601 string or unix seconds");
    }
    out.ts = parsed;
  }

  return true;
}

JsonValue timeOrNull(uint64_t t) {
  if (t == 0) return JsonValue::makeNull();
  return JsonValue::makeString(formatUtcIso8601(t));
}

uint64_t jsonTimeOrZero(const JsonValue* v) {
  if (v == nullptr || v->isNull()) return 0;
  if (v->type() == JsonValue::Type::String) {
    uint64_t t = 0;
    if (parseUtcIso8601(v->stringValue(), t)) return t;
    return 0;
  }
  if (v->type() == JsonValue::Type::Int && v->intValue() >= 0)
    return static_cast<uint64_t>(v->intValue());
  return 0;
}

}  // namespace

const char* severityName(Severity s) {
  switch (s) {
    case Severity::Critical:
      return "critical";
    case Severity::Warning:
      return "warning";
    default:
      return "info";
  }
}

bool severityFromString(const std::string& s, Severity& out) {
  if (s == "critical") {
    out = Severity::Critical;
    return true;
  }
  if (s == "warning") {
    out = Severity::Warning;
    return true;
  }
  if (s == "info") {
    out = Severity::Info;
    return true;
  }
  return false;
}

int severityRank(Severity s) { return static_cast<int>(s); }

uint32_t defaultTtlForSeverity(Severity s) {
  switch (s) {
    case Severity::Critical:
      return kTtlCritSeconds;
    case Severity::Warning:
      return kTtlWarnSeconds;
    default:
      return kTtlInfoSeconds;
  }
}

const char* alertStateName(AlertState st) {
  switch (st) {
    case AlertState::Acked:
      return "acked";
    case AlertState::Cleared:
      return "cleared";
    case AlertState::Expired:
      return "expired";
    default:
      return "active";
  }
}

bool alertStateFromString(const std::string& s, AlertState& out) {
  if (s == "active") {
    out = AlertState::Active;
    return true;
  }
  if (s == "acked") {
    out = AlertState::Acked;
    return true;
  }
  if (s == "cleared") {
    out = AlertState::Cleared;
    return true;
  }
  if (s == "expired") {
    out = AlertState::Expired;
    return true;
  }
  return false;
}

bool eventFromJson(const JsonValue& obj, AlertEvent& out, ValidationError& err) {
  out = AlertEvent();
  return parseEventObject(obj, out, err, "");
}

bool eventsFromJson(const JsonValue& root, std::vector<AlertEvent>& out, ValidationError& err) {
  out.clear();
  if (root.type() != JsonValue::Type::Object) {
    return fail(err, "", "request body must be a JSON object");
  }
  const JsonValue* alerts = root.find("alerts");
  if (alerts == nullptr) {
    AlertEvent ev;
    if (!parseEventObject(root, ev, err, "")) return false;
    out.push_back(std::move(ev));
    return true;
  }
  if (alerts->type() != JsonValue::Type::Array) {
    return fail(err, "alerts", "alerts must be an array of events");
  }
  if (alerts->arrayItems().size() > kMaxBatchEvents) {
    return fail(err, "alerts",
                "batch exceeds " + std::to_string(kMaxBatchEvents) + " events");
  }
  out.reserve(alerts->arrayItems().size());
  size_t index = 0;
  for (const JsonValue& item : alerts->arrayItems()) {
    AlertEvent ev;
    if (!parseEventObject(item, ev, err, "alerts[" + std::to_string(index) + "].")) return false;
    out.push_back(std::move(ev));
    ++index;
  }
  return true;
}

bool eventsFromJsonText(const std::string& body, std::vector<AlertEvent>& out,
                        ValidationError& err) {
  JsonValue root;
  JsonParseError perr;
  if (!jsonParse(body, root, perr)) {
    return fail(err, "", "invalid JSON at offset " + std::to_string(perr.offset) + ": " +
                             perr.message);
  }
  if (body.size() > kMaxEventJsonBytes && root.find("alerts") == nullptr) {
    return fail(err, "", "single event JSON exceeds " + std::to_string(kMaxEventJsonBytes) +
                             " bytes");
  }
  return eventsFromJson(root, out, err);
}

JsonValue recordToJson(const AlertRecord& r) {
  JsonValue::Object source;
  source.emplace_back("id", JsonValue::makeString(r.source.id));
  source.emplace_back("kind", JsonValue::makeString(r.source.kind));
  source.emplace_back("ref", JsonValue::makeString(r.source.ref));

  JsonValue::Object obj;
  obj.emplace_back("schema", JsonValue::makeString(kAlertSchema));
  obj.emplace_back("id", JsonValue::makeString(r.id));
  obj.emplace_back("fingerprint", JsonValue::makeString(r.fingerprint));
  obj.emplace_back("type", JsonValue::makeString(r.type));
  obj.emplace_back("severity", JsonValue::makeString(severityName(r.severity)));
  obj.emplace_back("source", JsonValue::makeObject(std::move(source)));
  obj.emplace_back("title", JsonValue::makeString(r.title));
  obj.emplace_back("message", JsonValue::makeString(r.message));
  obj.emplace_back("fields", r.fields.type() == JsonValue::Type::Object
                                 ? r.fields
                                 : JsonValue::makeObject({}));
  obj.emplace_back("state", JsonValue::makeString(alertStateName(r.state)));
  obj.emplace_back("muted", JsonValue::makeBool(r.muted));
  obj.emplace_back("first_seen", JsonValue::makeString(formatUtcIso8601(r.first_seen)));
  obj.emplace_back("last_seen", JsonValue::makeString(formatUtcIso8601(r.last_seen)));
  obj.emplace_back("occurrences", JsonValue::makeInt(r.occurrences));
  obj.emplace_back("ack_at", timeOrNull(r.ack_at));
  obj.emplace_back("ack_by",
                   r.ack_by.empty() ? JsonValue::makeNull() : JsonValue::makeString(r.ack_by));
  obj.emplace_back("expires_at", timeOrNull(r.expires_at));
  obj.emplace_back("cleared_at", timeOrNull(r.cleared_at));
  return JsonValue::makeObject(std::move(obj));
}

std::string recordToJsonText(const AlertRecord& r) { return recordToJson(r).toString(); }

bool recordFromJson(const JsonValue& obj, AlertRecord& out) {
  if (obj.type() != JsonValue::Type::Object) return false;
  const JsonValue* id = obj.find("id");
  const JsonValue* fp = obj.find("fingerprint");
  const JsonValue* type = obj.find("type");
  if (id == nullptr || id->type() != JsonValue::Type::String || id->stringValue().size() != 26 ||
      fp == nullptr || fp->type() != JsonValue::Type::String || !isHex40(fp->stringValue()) ||
      type == nullptr || type->type() != JsonValue::Type::String || type->stringValue().empty()) {
    return false;
  }
  AlertRecord r;
  r.id = id->stringValue();
  r.fingerprint = fp->stringValue();
  r.type = type->stringValue();

  if (const JsonValue* v = obj.find("severity")) {
    if (v->type() != JsonValue::Type::String || !severityFromString(v->stringValue(), r.severity))
      return false;
  }
  if (const JsonValue* v = obj.find("source")) {
    if (v->type() == JsonValue::Type::Object) {
      if (const JsonValue* s = v->find("id"))
        if (s->type() == JsonValue::Type::String) r.source.id = s->stringValue();
      if (const JsonValue* s = v->find("kind"))
        if (s->type() == JsonValue::Type::String) r.source.kind = s->stringValue();
      if (const JsonValue* s = v->find("ref"))
        if (s->type() == JsonValue::Type::String) r.source.ref = s->stringValue();
    }
  }
  if (const JsonValue* v = obj.find("title"))
    if (v->type() == JsonValue::Type::String) r.title = v->stringValue();
  if (const JsonValue* v = obj.find("message"))
    if (v->type() == JsonValue::Type::String) r.message = v->stringValue();
  if (const JsonValue* v = obj.find("fields"))
    if (v->type() == JsonValue::Type::Object) r.fields = *v;
  if (const JsonValue* v = obj.find("state")) {
    if (v->type() != JsonValue::Type::String || !alertStateFromString(v->stringValue(), r.state))
      return false;
  }
  if (const JsonValue* v = obj.find("muted")) r.muted = v->boolValue(false);
  r.first_seen = jsonTimeOrZero(obj.find("first_seen"));
  r.last_seen = jsonTimeOrZero(obj.find("last_seen"));
  if (const JsonValue* v = obj.find("occurrences")) {
    int64_t n = v->intValue(1);
    r.occurrences = static_cast<uint32_t>(n < 1 ? 1 : n);
  }
  r.ack_at = jsonTimeOrZero(obj.find("ack_at"));
  if (const JsonValue* v = obj.find("ack_by"))
    if (v->type() == JsonValue::Type::String) r.ack_by = v->stringValue();
  r.expires_at = jsonTimeOrZero(obj.find("expires_at"));
  r.cleared_at = jsonTimeOrZero(obj.find("cleared_at"));
  out = std::move(r);
  return true;
}

std::string computeFingerprint(const std::string& type, const std::vector<std::string>& dedup_key,
                               const JsonValue& fields, const std::string& source_id,
                               const std::string& title) {
  // Recipe (stable across restarts; part of the store contract):
  //   sha1("fp1\n" + type + "\n" + key1 + "=" + value1 + "\n" + key2 + "=" + value2 + "\n"...)
  // Missing keys contribute an empty value; pseudo keys resolve to record values.
  std::string acc = "fp1\n";
  acc += type;
  acc.push_back('\n');
  for (const std::string& key : dedup_key) {
    std::string value;
    if (key == kDedupKeySourceId) {
      value = source_id;
    } else if (key == kDedupKeyType) {
      value = type;
    } else if (key == kDedupKeyTitle) {
      value = title;
    } else if (const JsonValue* v = fields.find(key)) {
      value = v->scalarText();
    }
    acc += key;
    acc.push_back('=');
    acc += value;
    acc.push_back('\n');
  }
  return sha1Hex(acc);
}

}  // namespace alert_core
