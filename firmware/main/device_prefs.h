// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// 设备本地偏好 —— NVS "ui" 命名空间（M18）。
//
// 为什么不进 admin config.json：config schema 是白名单强校验（未知顶层键
// PUT 即 400），界面语言/电源模式这类纯本机偏好不属于远程管理面；放 NVS
// 避免污染管理契约。NVS 由 net_stack CredentialStore::ensureNvs() 保证已初始化
// （app_main 启动早期调用），本类直接使用。
//
// 键布局（命名空间 "ui"）：
//   lang   u8   0=zh 1=en（默认 zh）
//   pmode  u8   0=auto 1=mains 2=battery（默认 auto）
//   bpoll  u32  电池模式轮询周期秒（默认 900，钳 60..21600）
//   mute   str  逗号分隔的告警指纹（40-hex）列表，≤32 条；UI 级静音
//
// UI 级静音语义（M11「muted 记录留在库但不出现在总览/列表」的本机实现）：
// store 级静音需要 net_task 上下文写 AlertStore，而 NetStack 暂未暴露静音
// 命令入口（已记 finding 建议后续补）；首版在渲染层按指纹过滤，效果等同。

#include <cstdint>
#include <string>
#include <vector>

#include "ui_text.h"

namespace panel {

enum class PowerMode : uint8_t {
  Auto = 0,  // 按充电状态自动判定（充电=常供电，否则=电池）
  Mains,     // 强制常供电：modem-sleep 常驻，即时 http_push
  Battery,   // 强制电池：按周期轮询 + 安全休眠唤醒
};

class DevicePrefs {
 public:
  static DevicePrefs& instance();

  // 从 NVS 读取（缺失/损坏回退默认）。幂等可重入（每次重读）。
  void load();
  // 写回 NVS（NVS 内部线程安全；仅在 app_task 上下文调用，避免并发写）。
  void save();

  Lang lang() const { return lang_; }
  void setLang(Lang l) { lang_ = (l == Lang::En) ? Lang::En : Lang::Zh; }

  PowerMode powerMode() const { return pmode_; }
  void setPowerMode(PowerMode m) { pmode_ = m; }

  uint32_t batteryPollS() const { return bpoll_s_; }
  void setBatteryPollS(uint32_t s);

  // UI 级静音指纹表。
  const std::vector<std::string>& mutes() const { return mutes_; }
  bool isMuted(const std::string& fingerprint) const;
  bool addMute(const std::string& fingerprint);  // 已存在/超上限返回 false
  void clearMutes() { mutes_.clear(); }

 private:
  DevicePrefs() = default;

  Lang lang_ = Lang::Zh;
  PowerMode pmode_ = PowerMode::Auto;
  uint32_t bpoll_s_ = 900;
  std::vector<std::string> mutes_;
};

}  // namespace panel
