// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// 来源配置预设/映射模板（M16 任务 4）。
//
// 设计原则（用户定稿）：优先通用字段映射模板，不绑定单一厂商；
// 无法安全通用解析的接口不得伪装支持 —— 这类来源给"自定义脚本 → 规范事件
// 数组"的模板（builtin_poll=false），并在 notes 里写明脚本契约，而不是
// 假装能直接对话 NUT/SNMP/ubus/各厂 NAS 私有 API。
//
// 预设只是 SourceConfig 起点：用户改 url/token/interval 后经 admin
// PUT /config 生效，解析校验与手写配置完全一致。

#include <string>
#include <vector>

#include "net_stack/source_config.h"

namespace eego::net {

struct SourcePreset {
  std::string id;        // "home_assistant" / "prometheus" / ...
  std::string label_zh;  // 中文名（管理 UI 展示）
  std::string label_en;  // English name (admin UI, M22 bilingual follow-up)
  // 推荐接入方式："http_poll"（轮询模板）或 "http_push"（推送桥接，无 sources[] 项）
  std::string kind_hint;
  // http_poll 模板的 SourceConfig JSON（kind_hint=="http_poll" 时有效）；
  // id/url 为占位符，由用户替换。
  alert_core::JsonValue config_template;
  // true = 该模板开箱即可安全解析；false = 必须配合自定义脚本（见 notes）
  bool builtin_poll = true;
  std::string notes;     // 中文说明：覆盖能力、脚本输出契约、不伪装支持的边界
  std::string notes_en;  // English notes (same contract; M22 bilingual follow-up)
};

// 全部预设（静态存储，进程级有效）。找不到返回 nullptr。
const SourcePreset* findSourcePreset(const std::string& id);
const std::vector<SourcePreset>& sourcePresets();

}  // namespace eego::net
