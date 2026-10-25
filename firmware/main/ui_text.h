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
  X(AckAll, "全部已读", "READ ALL") \
  X(Ack, "标记已读", "MARK READ") \
  X(Mute, "静音", "MUTE") \
  X(Clear, "删除", "DELETE") \
  X(SettingsKey, "设置", "SETTINGS") \
  X(On, "开", "ON") \
  X(Off, "关", "OFF") \
  X(Ok, "正常", "OK") \
  X(Fail, "失败", "FAIL") \
  X(SevCritical, "严重", "CRITICAL") \
  X(SevWarning, "警告", "WARNING") \
  X(SevInfo, "提示", "INFO") \
  X(ActiveTotal, "活动告警", "ACTIVE") \
  X(AckedCount, "已读", "READ") \
  X(MutedCount, "已静音", "MUTED") \
  X(NoAlerts, "无活动告警", "NO ACTIVE ALERTS") \
  X(SourcesHealthy, "数据源正常", "SOURCES OK") \
  X(UpdatedAt, "更新时间", "UPDATED") \
  X(AlertsCountFmt, "告警 %u 条", "ALERTS %u") \
  X(PageFmt, "第 %u/%u 页", "PAGE %u/%u") \
  X(EmptyList, "无告警记录", "NO ALERTS") \
  X(StateActive, "活动", "ACT") \
  X(StateAcked, "已读", "READ") \
  X(StateCleared, "已清除", "CLR") \
  X(StateExpired, "已过期", "EXP") \
  X(LblSource, "来源", "SOURCE") \
  X(LblState, "状态", "STATE") \
  X(LblFirstSeen, "首次", "FIRST") \
  X(LblLastSeen, "最近", "LAST") \
  X(LblOccurrences, "次数", "COUNT") \
  X(LblExpires, "过期", "EXPIRES") \
  X(LblAckBy, "操作人", "ACK BY") \
  X(LblFields, "字段", "FIELDS") \
  X(LblMessage, "详情", "MESSAGE") \
  X(LblType, "类型", "TYPE") \
  X(Never, "从未", "NEVER") \
  X(None, "无", "NONE") \
  X(ConfirmHold, "按住同一按钮 0.7 秒确认", "HOLD SAME BUTTON 0.7S TO CONFIRM") \
  X(DoneAck, "已标记已读", "MARKED READ") \
  X(DoneClear, "已删除", "DELETED") \
  X(DoneMute, "已静音", "MUTED") \
  X(ActionFailed, "操作失败", "ACTION FAILED") \
  X(ModeMains, "持续供电", "MAINS") \
  X(ModeBattery, "电池供电", "BATTERY") \
  X(ModeAuto, "自动", "AUTO") \
  X(Charging, "充电中", "CHARGING") \
  X(BatteryFmt, "电池 %u%% %umV", "BAT %u%% %umV") \
  X(WifiDisconnected, "WiFi 未连接", "DISCONNECTED") \
  X(WifiConnecting, "WiFi 连接中", "CONNECTING") \
  X(WifiConnected, "WiFi 已连接", "CONNECTED") \
  X(WifiFailed, "WiFi 连接失败", "FAILED") \
  X(WifiOffline, "WiFi 离线", "OFFLINE") \
  X(Sources, "告警源", "SOURCES") \
  X(NoSources, "无源", "NO SOURCES") \
  X(NextDueIn, "下次", "NEXT") \
  X(LastOk, "上次成功", "LAST OK") \
  X(StoreStats, "存储", "STORE") \
  X(SysStatus, "系统", "SYSTEM") \
  X(TimeLabel, "时间", "TIME") \
  X(TimeRtc, "RTC", "RTC") \
  X(TimeUnavailable, "时间不可用", "NO CLOCK") \
  X(ConfigOk, "配置有效", "CONFIG OK") \
  X(ConfigBad, "配置无效", "CONFIG INVALID") \
  X(Uptime, "运行", "UPTIME") \
  X(Version, "版本", "VERSION") \
  X(PortalOpen, "配网窗口开", "PORTAL OPEN") \
  X(ApFallback, "兜底AP", "AP FALLBACK") \
  X(SetLanguage, "语言", "LANGUAGE") \
  X(SetPowerMode, "供电模式", "POWER MODE") \
  X(SetBatteryPoll, "电池刷新周期", "BATTERY POLL") \
  X(SetFrontlight, "前光", "FRONTLIGHT") \
  X(SetMutes, "静音列表", "MUTED LIST") \
  X(SetPollNow, "立即刷新数据", "REFRESH NOW") \
  X(SetAdmin, "管理后台", "ADMIN") \
  X(MutesFmt, "静音 %u 条", "MUTED %u") \
  X(ClearMutes, "取消全部静音", "CLEAR MUTES") \
  X(PollNowStarted, "已开始刷新数据", "REFRESH STARTED") \
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
  X(Busy, "忙，请稍后重试", "BUSY; TRY AGAIN") \
  X(Sleeping, "电池供电：休眠中", "BATTERY: SLEEPING") \
  X(WakeHint, "按电源键唤醒", "PRESS POWER TO WAKE") \
  X(Polling, "正在刷新...", "REFRESHING...") \
  X(AgeNow, "刚刚", "NOW") \
  X(AgeMinFmt, "%u 分钟前", "%um ago") \
  X(AgeHourFmt, "%u 小时前", "%uh ago") \
  X(AgeDayFmt, "%u 天前", "%ud ago") \
  X(SetNetwork, "网络设置", "NETWORK") \
  X(SetRecoveryAp, "开启配网热点", "RECOVERY AP") \
  X(SetWifiReset, "仅重置 WiFi", "WIFI-ONLY RESET") \
  X(SetBluetooth, "BLE 配网", "BLE SETUP") \
  X(SetBleObserver, "BLE 广播监听", "BLE SENSOR SCAN") \
  X(SetBleUnbind, "清除 BLE 配对", "BLE CLEAR BONDS") \
  X(BleProvStarted, "手机打开 BLE 配网，输入本机 PoP", "USE BLE APP; ENTER LOCAL POP") \
  X(BleProvSuccess, "BLE 配网成功", "BLE PROVISIONING SUCCEEDED") \
  X(BleProvFailed, "BLE 配网失败或已取消", "BLE PROVISIONING FAILED/CLOSED") \
  X(BleUnbindDone, "BLE 配对已清除", "BLE BONDS CLEARED") \
  X(SetTimezone, "时区（小时）", "TIMEZONE") \
  X(SetBrightness, "前光亮度", "BRIGHTNESS") \
  X(SetWarmth, "前光色温", "WARMTH") \
  X(SetDataRefresh, "数据刷新周期", "DATA REFRESH") \
  X(SetEpdFull, "墨水屏全刷周期", "EPD FULL INTERVAL") \
  X(SetEpdNow, "立即全屏刷新", "EPD FULL NOW") \
  X(SetIdleSleep, "自动休眠", "IDLE SLEEP") \
  X(SetSleepPlan, "定时休眠", "SLEEP SCHEDULE") \
  X(SetNextWake, "下次唤醒时间", "NEXT WAKE") \
  X(SetLastWake, "唤醒方式", "LAST WAKE") \
  X(SetStorage, "SD 卡状态", "STORAGE") \
  X(SetDiagnostics, "串口诊断", "SERIAL DIAGNOSTICS") \
  X(SetRunDiagnostics, "硬件自检", "RUN DIAGNOSTICS") \
  X(SetSecurity, "管理密码", "SECURITY") \
  X(SetFactoryReset, "恢复出厂设置", "FACTORY RESET") \
  X(Unavailable, "不可用", "UNAVAILABLE") \
  X(SourceSchedules, "跟随源", "SOURCE SCHEDULES") \
  X(ManualOnly, "仅手动", "MANUAL ONLY") \
  X(NotScheduled, "未排程", "NOT SCHEDULED") \
  X(DeepSleepOffline, "深睡时无法联网", "NO HTTP/BLE IN DEEP SLEEP") \
  X(ConfirmPhysicalHold, "按住实体电源键 0.7 秒确认", "HOLD POWER BUTTON 0.7S TO CONFIRM") \
  X(RecoveryApStarted, "恢复热点：EEGO-A4-Setup / 192.168.4.1", "AP: EEGO-A4-Setup / 192.168.4.1") \
  X(WifiResetDone, "WiFi 已重置，恢复热点已开启", "WIFI RESET; RECOVERY AP OPEN") \
  X(FactoryResetDone, "恢复完成，即将重启", "RESET COMPLETE; REBOOTING") \
  X(FullRefreshQueued, "已请求全屏刷新", "FULL REFRESH QUEUED") \
  X(WakeTimer, "定时器", "TIMER") \
  X(WakeButton, "电源键", "POWER BUTTON") \
  X(WakeReset, "复位/上电", "RESET/POWER ON") \
  X(WakeOther, "其他", "OTHER") \
  X(SecuritySet, "已设置管理密码", "PASSWORD SET") \
  X(SecurityMissing, "未设置管理密码", "NO PASSWORD") \
  X(SdReady, "SD 卡正常", "SD READY") \
  X(SdMissing, "SD 卡不可用", "SD UNAVAILABLE") \
  X(SettingsPageFmt, "第 %u/%u 页", "PAGE %u/%u") \
  X(PageHelp, "输入帮助", "INPUT HELP") \
  X(InputResetDone, "按键映射已恢复默认", "INPUT DEFAULTS RESTORED") \
  X(SetInputHelp, "输入帮助", "INPUT HELP") \
  X(SetInputReset, "重置按键映射", "RESET INPUT MAPPINGS") \
  X(HelpPower, "Power 短按 / 长按", "POWER SHORT / LONG") \
  X(HelpActivateBack, "确认 / 返回（固定）", "ACTIVATE / BACK (FIXED)") \
  X(HelpFrontHome, "Front 短按", "FRONT SHORT") \
  X(HelpHome, "返回主页（固定）", "HOME ESCAPE (FIXED)") \
  X(HelpFrontGestures, "Front 长按 / 双击", "FRONT LONG / DOUBLE") \
  X(HelpDefaultHelpStatus, "帮助 / 状态（默认）", "HELP / STATUS (DEFAULT)") \
  X(HelpUpDown, "Up/Down 短按 / 长按", "UP/DOWN SHORT / LONG") \
  X(HelpFocusPage, "焦点 / 翻页（默认）", "FOCUS / PAGE (DEFAULT)") \
  X(HelpSwipe, "滑动（左/上 · 右/下）", "SWIPE (LEFT/UP · RIGHT/DOWN)") \
  X(HelpPage, "下一页 / 上一页；首页右滑返回", "NEXT / PREV PAGE; RIGHT ON FIRST = BACK") \
  X(HelpChord, "Up+Down 组合键", "UP+DOWN CHORD") \
  X(HelpCustomOnly, "仅自定义动作", "CUSTOM ACTION ONLY") \
  X(HelpDanger, "危险操作", "DANGEROUS ACTION") \
  X(HelpFreshHold, "按住同一按钮 0.7 秒", "HOLD SAME BUTTON 0.7S") \
  X(HelpCustomBindings, "自定义按键", "CUSTOM BINDINGS") \
  X(HelpOverrideOrder, "页面设置优先；默认安全", "PAGE FIRST; SAFE DEFAULTS") \
  X(HelpLocalReset, "本地恢复", "LOCAL RESET") \
  X(HelpResetHere, "设置 → 重置按键映射", "SETTINGS → RESET INPUT") \
  X(LangZh, "中文", "CHINESE") \
  X(LangEn, "英文", "ENGLISH") \
  X(PrevPage, "上一页", "PREV") \
  X(NextPage, "下一页", "NEXT") \
  X(PollEveryMinFmt, "每 %u 分钟", "EVERY %uMIN") \
  X(PollEveryHourFmt, "每 %u 小时", "EVERY %uH") \
  X(PollEveryDayFmt, "每 %u 天", "EVERY %uD")

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
