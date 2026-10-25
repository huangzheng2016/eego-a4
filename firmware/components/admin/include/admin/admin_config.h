// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// 配置存储 —— config.json 白名单校验、未知字段保留、原子持久化、secrets 掩码
// （M13 F6/T4 + M12「config 读改写互斥锁」定稿）。
//
// 模型（M13 §3 schema v1 + M21 mute_rules，顶层键）：
//   schema_version        int，必须 1
//   admin                 admin 域（本组件强校验；wifi_list 只能经
//                         POST/DELETE /api/v1/config/wifi 变更，PUT 携带即 400）
//   alert_type_overrides  类型覆盖表（形状校验；应用在 app_task 上下文）
//   mute_rules            静音规则数组（M21；热生效同步到 NetStack，重启后恢复）
//   * 文件内未知顶层键      读取时保留（前向兼容），PUT body 内出现即 400
//
// 持久化（M13 F6 持久化层）：校验 → 应用 RAM → tmp 文件 + rename 原子写 →
// 回读校验 → 失败回滚 RAM 并报 500。启动校验失败：坏文件改名 config.json.corrupt、
// 加载默认、valid()=false（状态页可见）。
//
// 线程安全：config 互斥锁；commit 只允许在 app_task 上下文调用（SD 写单属主）。
// secrets 边界：本组件任何序列化输出默认掩码（password/secret/token/key
// 字符串值 → "********"；*_ref 引用照常显示）。

#include <string>
#include <vector>

#include "alert_core/json.h"
#include "alert_core/store.h"
#include "esp_err.h"

namespace eego::admin {

struct FieldError {
  std::string field;    // "admin.display.font"、"mute_rules[2].value"
  std::string message;  // 安全描述（不含请求值原文）
};
using FieldErrors = std::vector<FieldError>;


// admin 域强类型视图（M13 §3）。
struct AdminDomainConfig {
  bool auto_ap_fallback = false;
  uint32_t session_ttl_s = 86400;
  uint32_t login_lock_threshold = 5;
  std::string font;          // SD 绝对路径，禁 ".."
  std::string log_level = "INFO";
  bool sd_log = false;
};

class ConfigStore {
 public:
  static ConfigStore& instance();

  // 加载 + 启动校验（幂等）。文件缺失 → 默认配置（valid=true）；
  // 解析/校验失败 → 改名 config.json.corrupt + 默认配置（valid=false）。
  esp_err_t init(const std::string& path);
  bool valid() const;
  const std::string& path() const;

  // GET /config 响应体：全量 DOM 副本（secrets 已掩码）。
  alert_core::JsonValue maskedDom() const;

  // 全量 DOM 原始副本（未掩码；仅供组件内部命令处理器做读改写，
  // 禁止直接用于 HTTP 响应）。
  alert_core::JsonValue rawDom() const;

  // PUT /config：解析 + 白名单/强类型/范围校验 + 与现 DOM 合并。
  // 语义：body 中出现的顶层 section 整体替换，未出现的保留。
  // 成功返回 true 并输出 merged（尚未持久化，由 AdminCommand 在
  // app_task 上下文 commit）；失败返回 false 并填充字段级 errs。
  bool validateAndMerge(const std::string& body, alert_core::JsonValue& merged,
                        FieldErrors& errs) const;

  // 原子提交（仅 app_task 上下文）：tmp + rename + 回读校验；
  // 失败保持 RAM 不变并输出安全错误描述。
  bool commit(const alert_core::JsonValue& merged, std::string& err);

  // 当前 admin 域强类型视图（锁保护下解析）。
  AdminDomainConfig admin() const;

  // 当前静音规则表（M21；已校验形状，直接可喂 NetStack::setMuteRules）。
  // expires_at 为绝对 unix 秒（0 = 永不过期）——重启恢复后 TTL 语义不变。
  std::vector<alert_core::MuteRule> muteRules() const;

  // GET /config/schema 静态响应（JSON 字符串，static 存储）。
  static const char* schemaJson();

  // ---- 内部工具（供默认命令处理器/门户使用；均持 config 锁）----
  // 把 wifi 账号（NVS 权威）同步进 DOM 的 admin.wifi_list（ssid+pass_ref）。
  bool setWifiListInDom(alert_core::JsonValue& dom,
                        const std::vector<std::pair<std::string, std::string>>& ssid_ref_list)
      const;
  // 整体替换 DOM 的 mute_rules 节（M21；面板静音/清静音持久化用，app_task
  // 上下文读改写后经 commit 原子落盘）。
  bool setMuteRulesInDom(alert_core::JsonValue& dom,
                         const std::vector<alert_core::MuteRule>& rules) const;

 private:
  ConfigStore() = default;

  mutable void* mutex_ = nullptr;  // SemaphoreHandle_t
  std::string path_;
  alert_core::JsonValue dom_;
  AdminDomainConfig admin_;
  bool valid_ = true;
  bool loaded_ = false;
};

}  // namespace eego::admin
