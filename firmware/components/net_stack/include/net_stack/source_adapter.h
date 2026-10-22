// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// 源适配器契约与注册表（M11 §2 + M16 扩展点）。
//
// 契约：适配器只产规范 AlertEvent，由 net_task 统一入 AlertStore（去重/确认/
// 过期语义全部在 alert_core）；适配器不碰 UI、不做渲染、不直接调
// esp_http_client（出站一律经 HttpPool）。
//
// 扩展点（轮询适配器与外部摄入后装均不触碰 net_task 属主边界）：
//   * ble_scan      —— ble 组件 NimBLE observer 解析广播 → AlertEvent；
//                      只经 NetStack::ingestExternal 投递，无需 HTTP 会话
//   * web_chart     —— 产 ChartSeries 而非 AlertRecord 的姊妹接口（DataSource），
//                      由 UI 侧消费，不经告警 ingest
//   * serial_inject —— 串口注入告警（诊断/主机 mock），不是轮询适配器：
//                      直接复用 NetStack::ingestExternal 摄入路径即可
//
// kind 字符串复用 alert_core 的 kSourceKind* 常量。

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "alert_core/store.h"
#include "alert_core/type_registry.h"
#include "net_stack/credentials.h"
#include "net_stack/http_pool.h"
#include "net_stack/source_config.h"
#include "net_stack/source_health.h"

namespace eego::net {

class SourceCache;

// 适配器运行时上下文（net_task 注入；全部对象由 net_task 持有，适配器不得保存
// 超出单次 runCycle 的指针——除了自身实例状态）。
struct AdapterContext {
  HttpPool* pool = nullptr;
  alert_core::AlertStore* store = nullptr;
  const alert_core::TypeRegistry* registry = nullptr;
  SourceCache* cache = nullptr;
  uint64_t now = 0;  // unix 秒
};

class ISourceAdapter {
 public:
  virtual ~ISourceAdapter() = default;
  virtual const char* kind() const = 0;

  // 在 net_task 上下文执行一轮轮询：拉取 → 解析 → 产 AlertEvent 入 store →
  // 缓存/清除记账 → 更新 health。返回 false = 本轮失败（net_task 计退避）。
  // 实现必须容忍 store 为 nullptr（纯干跑场景，如 sources/test 预览）。
  virtual bool runCycle(AdapterContext& ctx, const SourceConfig& cfg,
                        SourceHealth& health) = 0;
};

// 适配器工厂注册表（进程级单例）。未知 kind 的源在 addSource 时被拒绝并提示。
class SourceAdapterRegistry {
 public:
  using Factory = std::function<std::unique_ptr<ISourceAdapter>()>;

  static SourceAdapterRegistry& instance();

  // 注册/覆盖一个适配器工厂（后装扩展在此注册；重复注册同名覆盖）。
  void registerAdapter(const std::string& kind, Factory factory);
  // 创建实例；未知 kind 返回 nullptr。
  std::unique_ptr<ISourceAdapter> create(const std::string& kind) const;
  std::vector<std::string> kinds() const;

 private:
  std::map<std::string, Factory> factories_;
};

}  // namespace eego::net
