// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// 面板界面中英文案（M11：界面中英双语；默认中文，S4 设置/串口 lang 切换）。
//
// 使用：tr(Str::Xxx) 按当前语言返回静态字符串（生命周期永久，勿 free）。
// 新增文案：在 PANEL_STR_LIST 加一行 X(键, 中文, 英文)，枚举与表自动同步。
// 含 printf 占位符（%u/%s）的词条在两种语言中占位符顺序必须一致。

#include <cstdint>

namespace panel {

enum class Lang : uint8_t {
  Zh = 0,
  En = 1,
};

// X(键, 中文, English)
#define PANEL_STR_LIST(X) \
  X(AppName, "告警面板", "ALERT PANEL") \
  X(PageOverview, "总览", "OVERVIEW") \
  X(PageTable, "告警列表", "ALERTS") \
  X(PageDetail, "详情", "DETAIL") \
  X(PageStatus, "状态", "STATUS") \
  X(PageSettings, "设置", "SETTINGS") \
  X(Back, "返回", "BACK") \
  X(Refresh, "刷新", "REFRESH") \
  X(Open, "打开", "OPEN") \
  X(List, "列表", "LIST") \
  X(AckAll, "全部确认", "ACK ALL") \
  X(Ack, "确认", "ACK") \
  X(Mute, "静音", "MUTE") \
  X(Clear, "清除", "CLEAR") \
  X(SettingsKey, "设置", "SETTINGS") \
  X(On, "开", "ON") \
  X(Off, "关", "OFF") \
  X(Ok, "正常", "OK") \
  X(Fail, "失败", "FAIL") \
  X(SevCritical, "严重", "CRITICAL") \
  X(SevWarning, "警告", "WARNING") \
  X(SevInfo, "提示", "INFO") \
  X(ActiveTotal, "活动告警", "ACTIVE") \
  X(AckedCount, "已确认", "ACKED") \
  X(MutedCount, "已静音", "MUTED") \
  X(NoAlerts, "无活动告警", "NO ACTIVE ALERTS") \
  X(SourcesHealthy, "源正常", "SOURCES OK") \
  X(UpdatedAt, "更新", "UPDATED") \
  X(AlertsCountFmt, "告警 %u 条", "ALERTS %u") \
  X(PageFmt, "第 %u/%u 页", "PAGE %u/%u") \
  X(EmptyList, "无告警记录", "NO ALERTS") \
  X(StateActive, "活动", "ACT") \
  X(StateAcked, "已确认", "ACK") \
  X(StateCleared, "已清除", "CLR") \
  X(StateExpired, "已过期", "EXP") \
  X(LblSource, "来源", "SOURCE") \
  X(LblState, "状态", "STATE") \
  X(LblFirstSeen, "首次", "FIRST") \
  X(LblLastSeen, "最近", "LAST") \
  X(LblOccurrences, "次数", "COUNT") \
  X(LblExpires, "过期", "EXPIRES") \
  X(LblAckBy, "确认人", "ACK BY") \
  X(LblFields, "字段", "FIELDS") \
  X(LblMessage, "详情", "MESSAGE") \
  X(LblType, "类型", "TYPE") \
  X(Never, "从未", "NEVER") \
  X(None, "无", "NONE") \
  X(ConfirmHint, "再次按下以确认", "PRESS AGAIN TO CONFIRM") \
  X(DoneAck, "已确认", "ACKNOWLEDGED") \
  X(DoneClear, "已清除", "CLEARED") \
  X(DoneMute, "已静音", "MUTED") \
  X(ActionFailed, "操作失败", "ACTION FAILED") \
  X(ModeMains, "常供电", "MAINS") \
  X(ModeBattery, "电池", "BATTERY") \
  X(ModeAuto, "自动", "AUTO") \
  X(Charging, "充电中", "CHARGING") \
  X(BatteryFmt, "电池 %u%% %umV", "BAT %u%% %umV") \
  X(WifiDisconnected, "未连接", "DISCONNECTED") \
  X(WifiConnecting, "连接中", "CONNECTING") \
  X(WifiConnected, "已连接", "CONNECTED") \
  X(WifiFailed, "连接失败", "FAILED") \
  X(WifiOffline, "离线", "OFFLINE") \
  X(Sources, "告警源", "SOURCES") \
  X(NoSources, "无源", "NO SOURCES") \
  X(NextDueIn, "下次", "NEXT") \
  X(LastOk, "上次成功", "LAST OK") \
  X(StoreStats, "存储", "STORE") \
  X(SysStatus, "系统", "SYSTEM") \
  X(ConfigOk, "配置有效", "CONFIG OK") \
  X(ConfigBad, "配置无效", "CONFIG INVALID") \
  X(Uptime, "运行", "UPTIME") \
  X(Version, "版本", "VERSION") \
  X(PortalOpen, "配网窗口开", "PORTAL OPEN") \
  X(ApFallback, "兜底AP", "AP FALLBACK") \
  X(SetLanguage, "语言", "LANGUAGE") \
  X(SetPowerMode, "电源模式", "POWER MODE") \
  X(SetBatteryPoll, "电池轮询", "BATTERY POLL") \
  X(SetFrontlight, "前光", "FRONTLIGHT") \
  X(SetMutes, "静音列表", "MUTED LIST") \
  X(SetPollNow, "立即轮询", "POLL NOW") \
  X(SetAdmin, "管理后台", "ADMIN") \
  X(MutesFmt, "静音 %u 条", "MUTED %u") \
  X(ClearMutes, "清除全部静音", "CLEAR MUTES") \
  X(PollNowStarted, "已触发轮询", "POLL TRIGGERED") \
  X(FrontlightMissing, "无前光", "NO FRONTLIGHT") \
  X(PollEveryFmt, "每 %u 秒", "EVERY %us") \
  X(ProvTitle, "首次配网", "SETUP") \
  X(ProvAp, "连接热点", "CONNECT AP") \
  X(ProvStep1, "1. 手机连接 EEGO-A4-Setup", "1. Join EEGO-A4-Setup") \
  X(ProvStep2, "2. 浏览器打开 192.168.4.1", "2. Open http://192.168.4.1") \
  X(ProvStep3, "3. 选 WiFi 并设管理密码", "3. Pick WiFi + set password") \
  X(ProvWindowFmt, "窗口剩余 %u 秒", "WINDOW %us LEFT") \
  X(ProvDone, "配网成功，即将重启", "PROVISIONED, REBOOTING") \
  X(ProvClosed, "配网窗口已关闭", "PORTAL CLOSED") \
  X(ConfirmAckAll, "再次按下：全部确认", "AGAIN: ACK ALL") \
  X(Sleeping, "电池模式：休眠中", "BATTERY: SLEEPING") \
  X(WakeHint, "按电源键唤醒", "PRESS POWER TO WAKE") \
  X(Polling, "轮询中...", "POLLING...") \
  X(AgeNow, "刚刚", "NOW") \
  X(AgeMinFmt, "%u 分钟前", "%um ago") \
  X(AgeHourFmt, "%u 小时前", "%uh ago") \
  X(AgeDayFmt, "%u 天前", "%ud ago") \
  X(LangZh, "中文", "CHINESE") \
  X(LangEn, "英文", "ENGLISH")

enum class Str : uint8_t {
#define X(k, zh, en) k,
  PANEL_STR_LIST(X)
#undef X
};

// 当前界面语言（默认中文）。切换后由调用方触发整页重绘。
Lang currentLang();
void setLang(Lang lang);

// 按当前语言取词条。越界键返回空串（防御；编译期枚举不会越界）。
const char* tr(Str s);

// "zh"|"en"（大小写不敏感）→ Lang；失败返回 false。
bool langFromString(const char* s, Lang& out);
const char* langCode(Lang lang);

}  // namespace panel
