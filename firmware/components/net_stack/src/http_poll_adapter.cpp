// SPDX-License-Identifier: GPL-3.0-or-later
// 通用 http_poll 适配器实现。
#include "net_stack/http_poll_adapter.h"

#include <algorithm>
#include <cstdlib>

#include "alert_core/time_util.h"
#include "esp_log.h"
#include "net_stack/source_cache.h"

namespace eego::net {

using alert_core::AlertEvent;
using alert_core::JsonValue;

static const char* TAG = "net.poll.adapter";

// 在根上按 '.' 分层解析路径（支持数组下标段）；空路径返回根本身。
static const JsonValue* resolvePath(const JsonValue& root, const std::string& path) {
  if (path.empty()) return &root;
  const JsonValue* cur = &root;
  size_t start = 0;
  while (start <= path.size()) {
    size_t dot = path.find('.', start);
    std::string seg =
        path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
    if (seg.empty()) return nullptr;
    if (cur->type() == JsonValue::Type::Object) {
      cur = cur->find(seg);
      if (!cur) return nullptr;
    } else if (cur->type() == JsonValue::Type::Array) {
      char* end = nullptr;
      long idx = strtol(seg.c_str(), &end, 10);
      if (!end || *end != '\0' || idx < 0 || (size_t)idx >= cur->arrayItems().size())
        return nullptr;
      cur = &cur->arrayItems()[(size_t)idx];
    } else {
      return nullptr;
    }
    if (dot == std::string::npos) break;
    start = dot + 1;
  }
  return cur;
}

struct BuiltEvent {
  AlertEvent ev;
  bool ok = false;
};

// 单个源元素 → 规范 AlertEvent。预算违反（title/message/fields 超规）返回
// ok=false（调用方计数 events_skipped；M11：不截断、不静默）。
static BuiltEvent buildEvent(const JsonValue& el, const SourceConfig& cfg) {
  BuiltEvent b;
  auto pathFor = [&](const char* key) -> std::string {
    auto it = cfg.field_map.find(key);
    return it == cfg.field_map.end() ? std::string(key) : it->second;
  };
  auto scalarAt = [&](const char* key) -> std::string {
    const JsonValue* v = resolvePath(el, pathFor(key));
    return (v && v->isScalar()) ? v->scalarText() : std::string();
  };

  AlertEvent& ev = b.ev;

  ev.type = scalarAt("type");
  if (ev.type.empty())
    ev.type = cfg.default_type.empty() ? "generic.any" : cfg.default_type;
  if (ev.type.empty() || ev.type.size() > alert_core::kMaxTypeIdLen) return b;

  ev.title = scalarAt("title");
  if (ev.title.empty()) ev.title = ev.type;
  if (ev.title.size() > alert_core::kMaxTitleLen) return b;
  ev.message = scalarAt("message");
  if (ev.message.size() > alert_core::kMaxMessageLen) return b;

  // 严重度：severity_map 映射 → 直读 canonical；未命中且非法则交给类型注册表默认
  std::string raw_sev = scalarAt("severity");
  if (!raw_sev.empty()) {
    std::string mapped = raw_sev;
    auto it = cfg.severity_map.find(raw_sev);
    if (it != cfg.severity_map.end()) mapped = it->second;
    alert_core::Severity s;
    if (alert_core::severityFromString(mapped, s)) {
      ev.has_severity = true;
      ev.severity = s;
    }
  }

  ev.cleared = [&] {
    static const std::vector<std::string> kDefaultClearStates = {"cleared"};
    const std::vector<std::string>& states =
        cfg.clear_states.empty() ? kDefaultClearStates : cfg.clear_states;
    const std::string st = scalarAt("state");
    return std::find(states.begin(), states.end(), st) != states.end();
  }();

  const JsonValue* tv = resolvePath(el, pathFor("ttl_s"));
  if (tv && tv->type() == JsonValue::Type::Int && tv->intValue() > 0 &&
      tv->intValue() <= (int64_t)alert_core::kMaxTtlSeconds) {
    ev.has_ttl = true;
    ev.ttl_s = (uint32_t)tv->intValue();
  }

  const JsonValue* tsv = resolvePath(el, pathFor("ts"));
  if (tsv) {
    if (tsv->type() == JsonValue::Type::Int && tsv->intValue() > 0) {
      ev.ts = (uint64_t)tsv->intValue();
    } else if (tsv->type() == JsonValue::Type::String) {
      uint64_t t = 0;
      if (alert_core::parseUtcIso8601(tsv->stringValue(), t)) ev.ts = t;
    }
  }

  // fields：映射路径必须是对象，只取标量成员，预算内（≤16 键、键≤32、值≤256）
  const JsonValue* fv = resolvePath(el, pathFor("fields"));
  JsonValue::Object fields_obj;
  if (fv && fv->type() == JsonValue::Type::Object) {
    for (const auto& kv : fv->objectItems()) {
      if (!kv.second.isScalar()) continue;
      if (kv.first.size() > alert_core::kMaxFieldKeyLen) return b;
      if (kv.second.scalarText().size() > alert_core::kMaxFieldValueLen) return b;
      if (fields_obj.size() >= alert_core::kMaxFieldsCount) break;
      fields_obj.emplace_back(kv.first, kv.second);
    }
  }
  ev.fields = JsonValue::makeObject(std::move(fields_obj));

  ev.dedup_key = cfg.dedup_key;
  b.ok = true;
  return b;
}

// 与 AlertStore::ingest 完全同源的 fingerprint（store.cpp:97-105 配方镜像）：
// 事件显式 fingerprint > computeFingerprint(type, 有效 dedup_key, fields,
// source.id, title)；有效 dedup_key = 事件显式 > 类型注册表默认 > 内置默认。
static std::string effectiveFingerprint(const AlertEvent& ev, const std::string& source_id,
                                        const alert_core::TypeRegistry& registry) {
  if (!ev.fingerprint.empty()) return ev.fingerprint;
  static const std::vector<std::string> kBuiltinDefault = {
      alert_core::kDedupKeySourceId, alert_core::kDedupKeyType, alert_core::kDedupKeyTitle};
  const alert_core::AlertTypeDescriptor desc = registry.resolve(ev.type);
  const std::vector<std::string>& keys =
      !ev.dedup_key.empty() ? ev.dedup_key
                            : (!desc.dedup_key.empty() ? desc.dedup_key : kBuiltinDefault);
  return alert_core::computeFingerprint(ev.type, keys, ev.fields, source_id, ev.title);
}

bool HttpPollAdapter::runCycle(AdapterContext& ctx, const SourceConfig& cfg,
                               SourceHealth& health) {
  // 1. 组请求：静态头 + Bearer 注入（token 只进内存头，不落日志）+ 条件 GET
  NetRequest req;
  req.url = cfg.url;
  req.timeout_ms = cfg.timeout_ms;
  req.tls_verify = cfg.tls_verify;
  req.conditional_get = true;
  req.headers = cfg.headers;
  if (!cfg.auth_token_ref.empty()) {
    std::string token = CredentialStore::resolveToken(cfg.auth_token_ref);
    if (!token.empty()) req.headers.push_back({"Authorization", "Bearer " + token});
  }

  NetResponse resp = ctx.pool->execute(req);
  health.last_attempt_at = ctx.now;
  if (resp.status == -1) {
    health.last_error = resp.error;
    return false;
  }
  health.last_http_status = resp.status;

  if (resp.status == 304) {
    // 数据未变：此前在册事件仍全部出现，miss 计数清零
    misses_.clear();
    health.last_success_at = ctx.now;
    health.last_error.clear();
    return true;
  }
  if (!resp.ok()) {
    health.last_error = "http " + std::to_string(resp.status);
    return false;
  }
  if (resp.length() > cfg.max_body_bytes) {
    health.last_error = "body too large";
    return false;
  }

  // 2. 解析（alert_core 严格 JSON；DOM 在内部 RAM，max_body_bytes 已限预算）
  JsonValue root;
  alert_core::JsonParseError perr;
  std::string text(resp.data(), resp.length());
  if (!alert_core::jsonParse(text, root, perr)) {
    ESP_LOGW(TAG, "source %s: json parse error at byte %u", cfg.id.c_str(),
             (unsigned)perr.offset);
    health.last_error = "json parse error";
    return false;
  }

  const JsonValue* arr = resolvePath(root, cfg.array_path);
  if (!arr) {
    health.last_error = "array_path not found";
    return false;
  }
  std::vector<const JsonValue*> elements;
  if (arr->type() == JsonValue::Type::Array) {
    for (const auto& it : arr->arrayItems()) elements.push_back(&it);
  } else if (arr->type() == JsonValue::Type::Object) {
    elements.push_back(arr);
  } else {
    health.last_error = "array_path not array/object";
    return false;
  }

  // 3. 映射 + 入 store
  alert_core::AlertSource source{cfg.id, alert_core::kSourceKindHttpPoll, cfg.url};
  std::set<std::string> seen;
  uint32_t ingested = 0, skipped = 0;
  for (const JsonValue* el : elements) {
    if (el->type() != JsonValue::Type::Object) {
      skipped++;
      continue;
    }
    BuiltEvent b = buildEvent(*el, cfg);
    if (!b.ok) {
      skipped++;
      continue;
    }
    seen.insert(effectiveFingerprint(b.ev, cfg.id, *ctx.registry));
    if (ctx.store) {
      alert_core::IngestOutcome outcome = ctx.store->ingest(b.ev, source, ctx.now);
      if (outcome.status == alert_core::IngestOutcome::Status::Rejected) {
        skipped++;  // store_full：绝不静默丢弃（M11），计入 skipped 由健康页可见
        continue;
      }
    }
    ingested++;
  }

  // 4. 原始 body 落离线缓存（tmp+rename；失败只记日志不判源失败）
  if (ctx.cache) {
    std::string err;
    if (ctx.cache->write(cfg.id, resp.data(), resp.length(), err)) {
      health.cache_fetched_at = ctx.now;
      health.cache_bytes = (uint32_t)resp.length();
    } else {
      ESP_LOGW(TAG, "source %s: cache write failed: %s", cfg.id.c_str(), err.c_str());
    }
  }

  // 5. clear_after_misses：本轮未出现的在册活动记录，连续 miss 达标即清除
  if (cfg.clear_after_misses > 0 && ctx.store) {
    std::vector<std::pair<std::string, std::string>> active =
        ctx.store->activeIdsBySource(cfg.id);
    for (const auto& id_fp : active) {
      if (seen.count(id_fp.second)) {
        misses_.erase(id_fp.second);
        continue;
      }
      uint32_t m = ++misses_[id_fp.second];
      if (m >= cfg.clear_after_misses) {
        ctx.store->clear(id_fp.first, ctx.now);
        misses_.erase(id_fp.second);
        health.events_cleared++;
      }
    }
  }

  health.last_success_at = ctx.now;
  health.last_error.clear();
  health.events_ingested += ingested;
  health.events_skipped += skipped;
  return true;
}

void registerHttpPollAdapter(SourceAdapterRegistry& reg) {
  reg.registerAdapter("http_poll", []() -> std::unique_ptr<ISourceAdapter> {
    return std::make_unique<HttpPollAdapter>();
  });
}

}  // namespace eego::net
