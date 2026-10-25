// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// refresh_policy —— 空闲感知的屏幕刷新节奏（纯逻辑，无 ESP-IDF 依赖，主机可测）。
//
//   * Active：最近 idle_after_min 分钟内有用户交互（触控/按键/前置键/web 应用）
//     → 一切照旧，屏幕更新立即显示；
//   * Idle：超过 idle_after_min 无交互 → 所有非交互触发的屏幕更新（状态栏分钟、
//     dashboard 状态行、数据驱动的面板重绘、告警快照重绘）只登记为待刷新，合并
//     到下一个本地墙钟边界（idle_refresh_min 的整数倍，如 :00/:05/:10）一次显示；
//   * 例外：Idle 中新出现的活动严重告警（critical_immediate）立即显示；任何交互
//     立即切回 Active，待刷新内容随即显示。
//
// 调用方（PanelApp）持有“是否有待显示内容”的脏标志；本类只回答“现在能否显示”。

#include <cstdint>
#include <string>
#include <vector>

namespace panel {

struct RefreshPolicyConfig {
  uint32_t idle_after_min = 10;  // 0 = 禁用（始终 Active）
  uint32_t idle_refresh_min = 5;
  bool critical_immediate = true;
  int timezone_offset_min = 0;   // 边界按本地时间对齐
};

// unix_s 之后（严格大于）的下一个本地墙钟边界（unix 秒）。interval_min=0 按 1 处理。
uint64_t nextRefreshBoundary(uint64_t unix_s, int timezone_offset_min, uint32_t interval_min);

class RefreshPolicy {
 public:
  // 低于此值的 unix 时间视为不可信（时间不可用时按单调时钟间隔兜底）。
  static constexpr uint64_t kMinValidUnix = 946684800ULL;  // 2000-01-01
  // 边界后宽限：边界后 kBoundaryGraceS 秒内才登记的待刷新内容（例如状态栏分钟
  // 恰在边界那一秒变化），若该边界之后尚未刷新过，视为本边界的刷新立即显示。
  static constexpr uint64_t kBoundaryGraceS = 60;

  void configure(const RefreshPolicyConfig& config);
  const RefreshPolicyConfig& config() const { return config_; }

  // 用户交互（切回 Active；清除已登记的边界等待）。
  void noteInteraction(int64_t mono_ms);
  // 保持唤醒（配置模式）：hold_until_ms（单调时钟）之前恒为 Active；0 = 取消。
  // 到期后按最后一次交互重新判定（过期即恢复原节奏，不额外宽限）。
  void setHoldUntil(int64_t hold_until_ms) { hold_until_ms_ = hold_until_ms; }
  int64_t holdUntil() const { return hold_until_ms_; }
  bool idle(int64_t mono_ms) const;
  int64_t lastInteractionMs() const { return last_interaction_ms_; }

  // 有待显示内容时调用：返回是否现在显示。Active / 严重告警例外 → true；Idle 时
  // 首次调用登记下一个边界，之后到达边界才返回 true。unix_s 不可信时以单调时钟
  // interval 兜底；登记时同时设单调时钟上限，防墙钟回拨导致无限推迟。
  bool allow(int64_t mono_ms, uint64_t unix_s, bool critical_arrived);

  // 实际刷新后调用：清除边界等待，记录刷新时刻（unix，可为 0）与当时已显示的
  // 活动严重告警身份。
  void noteRendered(uint64_t unix_s, std::vector<std::string> displayed_critical);
  void noteRendered(uint64_t unix_s);

  // 当前活动严重告警中是否有尚未显示过的（新出现）。critical_immediate=false 恒 false。
  bool criticalArrived(const std::vector<std::string>& active_critical) const;

  bool deferred() const { return due_mono_ms_ != 0; }
  uint64_t dueUnix() const { return due_unix_; }   // 0 = 未登记或时间不可信
  int64_t dueMonoMs() const { return due_mono_ms_; }

 private:
  RefreshPolicyConfig config_;
  int64_t last_interaction_ms_ = 0;
  int64_t hold_until_ms_ = 0;
  uint64_t due_unix_ = 0;
  int64_t due_mono_ms_ = 0;
  uint64_t last_render_unix_ = 0;
  std::vector<std::string> displayed_critical_;
};

}  // namespace panel
