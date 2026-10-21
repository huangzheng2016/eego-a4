// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// 通用 http_poll 适配器（M16 任务 3）：一个实现覆盖全部轮询源，
// 差异全部来自 SourceConfig 的映射模板（field_map/severity_map/array_path）。
//
// 流程（每轮，net_task 上下文）：
//   1. 组 NetRequest（静态头 + Bearer token 注入 + 条件 GET 校验器）；
//   2. HttpPool 执行（304 → 数据未变，本轮无增删；2xx → 解析）；
//   3. array_path 定位事件数组（空 = 根数组/单对象）；
//   4. 逐元素映射成规范 AlertEvent（字段预算超规 → 跳过并计数，不截断）；
//   5. 入 store（AlertSource{id, "http_poll", url}）；
//   6. 2xx 原始 body 落 cache/<source>.json；
//   7. clear_after_misses 记账：本轮未出现的在册活动记录 fingerprint
//      连续 miss 达标 → store.clear（0 = 从不自动清除）。
//
// fingerprint 与 store 完全同源（computeFingerprint + 同一 dedup_key 优先级链：
// 事件显式 > 类型注册表默认 > 内置 source.id+type+title），miss 记账不会错位。

#include <map>
#include <set>
#include <string>

#include "net_stack/source_adapter.h"

namespace eego::net {

class HttpPollAdapter : public ISourceAdapter {
 public:
  const char* kind() const override { return "http_poll"; }
  bool runCycle(AdapterContext& ctx, const SourceConfig& cfg, SourceHealth& health) override;

 private:
  // clear_after_misses 记账：fingerprint → 连续 miss 数
  std::map<std::string, uint32_t> misses_;
};

// 内置适配器注册（NetStack::start 时调用一次）。
void registerHttpPollAdapter(SourceAdapterRegistry& reg);

}  // namespace eego::net
