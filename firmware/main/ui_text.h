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
  X(PageDetail, "告警详情", "ALERT") \
  X(PageStatus, "设备状态", "STATUS") \
  X(PageSettings, "设置", "SETTINGS") \
  X(PageOnboarding, "仪表盘 · 未配置", "DASHBOARD · NOT SET UP") \
  X(Back, "返回", "BACK") \
  X(Refresh, "刷新", "REFRESH") \
  X(Ack, "确认", "ACK") \
  X(Mute, "静音", "MUTE") \
  X(Clear, "删除", "DELETE") \
  X(Trend, "趋势", "TREND") \
  X(SettingsKey, "设置", "SETTINGS") \
  X(StatusKey, "状态", "STATUS") \
  X(On, "开", "ON") \
  X(Off, "关", "OFF") \
  X(Ok, "正常", "OK") \
  X(Fail, "失败", "FAIL") \
  X(SevCritical, "严重", "CRITICAL") \
  X(SevWarning, "警告", "WARNING") \
  X(SevInfo, "提示", "INFO") \
  X(MutedCount, "已静音", "MUTED") \
  X(EmptyList, "告警已全部恢复", "NO ALERTS LEFT") \
  X(StateActive, "活动", "ACTIVE") \
  X(StateAcked, "已确认", "ACKED") \
  X(StateCleared, "已恢复", "CLEARED") \
  X(StateExpired, "已过期", "EXPIRED") \
  X(LblSource, "来源", "SOURCE") \
  X(LblRule, "规则", "RULE") \
  X(LblTime, "时间", "TIME") \
  X(ExternalAlert, "外部告警", "EXTERNAL ALERT") \
  X(RuleAlert, "规则告警", "RULE ALERT") \
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
  X(ConfirmPress, "再按一次确认", "PRESS AGAIN TO CONFIRM") \
  X(DoneAck, "已确认", "ACKNOWLEDGED") \
  X(DoneClear, "已删除", "DELETED") \
  X(DoneMute, "已静音", "MUTED") \
  X(ActionFailed, "操作失败", "ACTION FAILED") \
  X(ModeMains, "外接电源", "EXTERNAL") \
  X(ModeBattery, "电池", "BATTERY") \
  X(ModeAuto, "自动判断", "AUTO") \
  X(PowerAlwaysExternal, "始终外接电源", "ALWAYS EXTERNAL") \
  X(PowerAlwaysBattery, "始终电池", "ALWAYS BATTERY") \
  X(PowerDetailFmt, "%s（%s）", "%s (%s)") \
  X(Charging, "充电中", "CHARGING") \
  X(WifiDisconnected, "WiFi 未连接", "DISCONNECTED") \
  X(WifiConnecting, "WiFi 连接中", "CONNECTING") \
  X(WifiConnected, "WiFi 已连接", "CONNECTED") \
  X(WifiFailed, "WiFi 连接失败", "FAILED") \
  X(WifiOffline, "WiFi 离线", "OFFLINE") \
  X(WifiNotTried, "未尝试", "NOT TRIED") \
  X(WifiNotSeen, "未扫描到", "NOT SEEN") \
  X(WifiDiag, "WiFi 诊断", "WIFI DIAG") \
  X(LblSignal10m, "近10分钟", "LAST 10 MIN") \
  X(LblTip, "建议", "TIP") \
  X(WifiWeakHint, "移近路由器/固定信道 1·6·11/20MHz", "MOVE CLOSER / CH 1·6·11 / 20MHZ") \
  X(Sources, "数据源", "DATA SOURCES") \
  X(NoSources, "未配置数据源", "NO SOURCES") \
  X(NoDashboard, "未配置仪表盘", "NO DASHBOARD") \
  X(LastOk, "更新", "UPDATED") \
  X(SourceOk, "正常", "OK") \
  X(SourceStale, "缓存", "CACHED") \
  X(SourceOffline, "离线", "OFFLINE") \
  X(SourceDisabled, "停用", "DISABLED") \
  X(SecNetwork, "网络", "NETWORK") \
  X(SecDevice, "设备", "DEVICE") \
  X(SecAlerts, "外部告警（推送）", "EXTERNAL ALERTS (PUSH)") \
  X(LblWifi, "WiFi", "WIFI") \
  X(LblSignal, "信号", "SIGNAL") \
  X(LblAddress, "管理地址", "ADDRESS") \
  X(LblPower, "供电", "POWER") \
  X(LblStorage, "存储", "STORAGE") \
  X(LblHardware, "硬件", "HARDWARE") \
  X(LblDashboard, "仪表盘", "DASHBOARD") \
  X(AlertStoreFmt, "活动 %u · 已确认 %u · 静音 %u", "ACTIVE %u · ACKED %u · MUTED %u") \
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
  X(SetChargeSignal, "充电信号", "CHARGE SIGNAL") \
  X(ChargeActiveHigh, "高有效", "ACTIVE HIGH") \
  X(ChargeActiveLow, "低有效", "ACTIVE LOW") \
  X(SetBatteryRefresh, "电池唤醒间隔", "BATTERY WAKE EVERY") \
  X(SetBatteryMaxStale, "最长不刷屏", "MAX UNCHANGED") \
  X(SetBatteryInteractive, "按键唤醒后保持", "AWAKE AFTER KEY") \
  X(TimeoutSecFmt, "%u 秒无操作", "%uS NO INPUT") \
  X(BatStatusFmt, "电量 %u%% · 更新于 %02d:%02d · 最迟 %02d:%02d", "BAT %u%% · UPDATED %02d:%02d · BY %02d:%02d") \
  X(BatStatusNoClockFmt, "电量 %u%% · 时间不可用", "BAT %u%% · NO CLOCK") \
  X(NightPauseFmt, "夜间暂停 · %02d:%02d 恢复", "NIGHT PAUSE · RESUMES %02d:%02d") \
  X(LowBatteryTitle, "电量过低（%u%%）", "BATTERY LOW (%u%%)") \
  X(LowBatteryHint, "已停止自动更新，请充电", "AUTO UPDATES STOPPED; PLEASE CHARGE") \
  X(LastDataFmt, "最后更新 %s", "LAST UPDATE %s") \
  X(SetFrontlight, "前光", "FRONTLIGHT") \
  X(SetMutes, "静音列表", "MUTED LIST") \
  X(SetPollNow, "立即刷新数据", "REFRESH NOW") \
  X(MutesFmt, "静音 %u 条", "MUTED %u") \
  X(ClearMutes, "取消全部静音", "CLEAR MUTES") \
  X(PollNowStarted, "已开始刷新数据", "REFRESH STARTED") \
  X(FrontlightMissing, "无前光", "NO FRONTLIGHT") \
  X(PollEveryFmt, "每 %u 秒", "EVERY %us") \
  X(ProvTitle, "首次配网", "SETUP") \
  X(ProvStep1, "1. 手机连接 EEGO-A4-Setup", "1. Join EEGO-A4-Setup") \
  X(ProvStep2, "2. 浏览器打开 192.168.4.1", "2. Open http://192.168.4.1") \
  X(ProvStep3, "3. 选 WiFi 并设管理密码", "3. Pick WiFi + set password") \
  X(ProvWindowFmt, "窗口剩余 %u 秒", "WINDOW %us LEFT") \
  X(ProvDone, "配网成功，即将重启", "PROVISIONED, REBOOTING") \
  X(ProvClosed, "配网窗口已关闭", "PORTAL CLOSED") \
  X(Busy, "忙，请稍后重试", "BUSY; TRY AGAIN") \
  X(Sleeping, "电池 · 休眠中", "ON BATTERY · ASLEEP") \
  X(WakeHint, "按电源键唤醒", "PRESS POWER TO WAKE") \
  X(OnbOpen, "在浏览器中打开", "OPEN IN A BROWSER") \
  X(OnbNoAddress, "等待网络连接…", "WAITING FOR NETWORK…") \
  X(OnbStep1, "1. 电脑或手机连接与本机相同的网络", "1. Join the same network as this device") \
  X(OnbStep2, "2. 打开上面的地址并登录管理后台", "2. Open the address above and sign in") \
  X(OnbStep3, "3. 选择模板或新建仪表盘，应用到设备", "3. Pick a template or build a dashboard, then apply") \
  X(OnbApStep1, "1. 手机连接热点 EEGO-A4-Setup", "1. Join Wi-Fi EEGO-A4-Setup") \
  X(OnbApStep2, "2. 打开上面的地址，选择 WiFi", "2. Open the address above and pick Wi-Fi") \
  X(OnbApStep3, "3. 联网后用新地址配置仪表盘", "3. Then configure the dashboard at the new address") \
  X(OnbNoWifi, "WiFi 未配置：设置 → 开启配网热点", "NO WIFI: SETTINGS → RECOVERY AP") \
  X(OnbInvalidFmt, "仪表盘配置无效：%s", "DASHBOARD CONFIG INVALID: %s") \
  X(OnbKeys, "前置键：长按 设置 · 双击 状态", "FRONT KEY: HOLD = SETTINGS · DOUBLE = STATUS") \
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
  X(SetIdleAfter, "节电判定", "ECO AFTER") \
  X(SetIdleRefresh, "节电刷新间隔", "ECO REFRESH") \
  X(SetScreenRotate, "自动轮播", "AUTO ROTATE") \
  X(IdleAfterFmt, "%u 分钟无操作", "%uMIN NO INPUT") \
  X(SetSleepPlan, "夜间暂停", "NIGHT PAUSE") \
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
  X(DeepSleepOffline, "暂停期间不联网、不更新", "NO NETWORK OR UPDATES WHILE PAUSED") \
  X(ConfirmPhysicalPress, "再按一次电源键确认", "PRESS POWER AGAIN TO CONFIRM") \
  X(ConfirmShutdown, "再按一次电源键关机", "PRESS POWER AGAIN TO SHUT DOWN") \
  X(PoweredOff, "已关机", "POWERED OFF") \
  X(PowerOnHint, "按电源键开机", "PRESS POWER TO TURN ON") \
  X(SleepBlocked, "配网进行中，暂不能休眠", "SETUP IN PROGRESS: CAN'T SLEEP") \
  X(OnbAsleep, "休眠中 · 按任意键唤醒", "ASLEEP · PRESS ANY KEY") \
  X(OnbAsleepNote, "唤醒后才能打开管理地址", "WAKE THE DEVICE TO OPEN THE ADMIN PAGE") \
  X(RecoveryApStarted, "恢复热点：EEGO-A4-Setup / 192.168.4.1", "AP: EEGO-A4-Setup / 192.168.4.1") \
  X(WifiResetDone, "WiFi 已重置，恢复热点已开启", "WIFI RESET; RECOVERY AP OPEN") \
  X(FactoryResetDone, "恢复完成，即将重启", "RESET COMPLETE; REBOOTING") \
  X(FullRefreshQueued, "已请求全屏刷新", "FULL REFRESH QUEUED") \
  X(WakeTimer, "定时器", "TIMER") \
  X(WakeButton, "电源键", "POWER BUTTON") \
  X(WakeKeys, "↑/↓ 键", "UP/DOWN KEY") \
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
  X(SetTouchCal, "触摸校准", "TOUCH CALIBRATION") \
  X(SetTouchMarks, "显示触点", "SHOW TOUCHES") \
  X(TouchCalibrated, "已校准", "CALIBRATED") \
  X(TouchFactory, "默认", "DEFAULT") \
  X(TouchCalTapFmt, "点击十字中心（%u/%u）", "TAP THE CROSS CENTER (%u/%u)") \
  X(TouchCalHoldStill, "请保持手指不动后抬起", "HOLD STILL, THEN LIFT") \
  X(TouchCalCancelHint, "长按电源键取消", "HOLD POWER TO CANCEL") \
  X(TouchCalVerify, "点击任意位置检查，触点会显示为圆点", "TAP ANYWHERE TO CHECK; TOUCHES SHOW AS DOTS") \
  X(TouchCalErrorFmt, "最大误差 %u px", "MAX ERROR %u PX") \
  X(TouchCalKeys, "电源键：短按保存 · 长按取消；上键重做", "POWER: SAVE · HOLD: CANCEL; UP: REDO") \
  X(TouchCalFailed, "校准失败：点击任意位置重做，长按电源键取消", "CALIBRATION FAILED: TAP TO REDO, HOLD POWER TO CANCEL") \
  X(TouchCalSave, "保存", "SAVE") \
  X(TouchCalRedo, "重做", "REDO") \
  X(TouchCalCancel, "取消", "CANCEL") \
  X(TouchCalSaved, "触摸校准已保存", "TOUCH CALIBRATION SAVED") \
  X(HelpUpDownShort, "↑/↓ 短按", "UP/DOWN SHORT") \
  X(HelpScreen, "上/下一屏；列表中上/下一项", "PREV/NEXT SCREEN; ITEM IN LISTS") \
  X(HelpUpDownLong, "↑/↓ 长按", "UP/DOWN LONG") \
  X(HelpFocus, "选择面板（焦点框）", "SELECT PANEL (FOCUS)") \
  X(HelpPower, "Power 短按 / 长按", "POWER SHORT / LONG") \
  X(HelpActivateBack, "休眠 / 关机（再按确认）", "SLEEP / SHUT DOWN (PRESS AGAIN)") \
  X(HelpFrontHome, "前置键 短按", "FRONT SHORT") \
  X(HelpHome, "返回 / 回到仪表盘（固定）", "BACK / DASHBOARD (FIXED)") \
  X(HelpFrontGestures, "前置键 长按 / 双击", "FRONT LONG / DOUBLE") \
  X(HelpSettingsStatus, "设置 / 状态", "SETTINGS / STATUS") \
  X(HelpSwipe, "滑动 左/上 · 右/下", "SWIPE LEFT/UP · RIGHT/DOWN") \
  X(HelpPage, "下一屏 / 上一屏；首屏右滑返回", "NEXT / PREV; RIGHT ON FIRST = BACK") \
  X(HelpTapAlert, "点按", "TAP") \
  X(HelpOpenDetail, "选择；告警 → 详情", "SELECT; ALERT → DETAIL") \
  X(HelpDanger, "危险操作", "DANGEROUS ACTION") \
  X(HelpFreshPress, "再按一次确认", "PRESS AGAIN TO CONFIRM") \
  X(HelpCustomBindings, "自定义按键", "CUSTOM BINDINGS") \
  X(HelpWebBindings, "网页端设置；设置 → 重置按键映射", "WEB ADMIN; SETTINGS → RESET INPUT") \
  X(LangZh, "中文", "CHINESE") \
  X(LangEn, "英文", "ENGLISH") \
  X(PrevPage, "上一页", "PREV") \
  X(NextPage, "下一页", "NEXT") \
  X(PollEveryMinFmt, "每 %u 分钟", "EVERY %uMIN") \
  X(PollEveryHourFmt, "每 %u 小时", "EVERY %uH") \
  X(PollEveryDayFmt, "每 %u 天", "EVERY %uD") \
  X(StateRunning, "运行中", "RUNNING") \
  X(StateKeepAwakeFmt, "保持唤醒 · 剩余 %u 分", "KEEP AWAKE · %uMIN LEFT") \
  X(StateIdleFmt, "节电 · 下次 %02d:%02d", "ECO · NEXT %02d:%02d") \
  X(StateIdleNoClockFmt, "节电 · 每 %u 分钟刷新", "ECO · EVERY %uMIN") \
  X(StateAwakeFmt, "已唤醒 · 无操作 %u 秒后休眠", "AWAKE · SLEEPS AFTER %uS IDLE") \
  X(StateSleepingFmt, "休眠中 · 下次 %02d:%02d · 按键唤醒", "ASLEEP · NEXT %02d:%02d · PRESS A KEY") \
  X(StateSleeping, "休眠中 · 按键唤醒", "ASLEEP · PRESS A KEY") \
  X(StateKeepAwakeShortFmt, "保持唤醒 %u 分", "AWAKE %uMIN") \
  X(StateIdleShortFmt, "节电 %02d:%02d", "ECO %02d:%02d") \
  X(StateAwake, "已唤醒", "AWAKE") \
  X(StateSleepingNextFmt, "休眠中 · 下次 %02d:%02d", "ASLEEP · NEXT %02d:%02d") \
  X(StateSleepingShort, "休眠中", "ASLEEP") \
  X(StateNightShort, "夜间暂停", "NIGHT PAUSE") \
  X(StateNightFmt, "夜间暂停 · %02d:%02d 恢复 · 按键唤醒", "NIGHT PAUSE · %02d:%02d · PRESS A KEY") \
  X(UpdatedFmt, "更新于 %02d:%02d", "UPDATED %02d:%02d") \
  X(UpdatedNoClock, "更新于 --:--", "UPDATED --:--") \
  X(OfflineCountFmt, "离线 %u", "OFF %u") \
  X(PreviewTag, "预览", "PREVIEW") \
  X(SecPower, "电源 / 休眠", "POWER / SLEEP") \
  X(LblRunState, "运行状态", "STATE") \
  X(LblCharge, "电量", "BATTERY") \
  X(LblAutoSleep, "自动休眠", "AUTO SLEEP") \
  X(LblNextWake, "下次唤醒", "NEXT WAKE") \
  X(LblWakeReason, "唤醒原因", "WAKE REASON") \
  X(LblSleeps, "休眠次数", "SLEEPS") \
  X(LblScreenUpdated, "屏幕更新", "SCREEN") \
  X(SetKeepAwakeFmt, "保持唤醒 %u 分钟", "KEEP AWAKE %uMIN") \
  X(KeepAwakeKey, "保持唤醒", "AWAKE") \
  X(KeepAwakeStartedFmt, "已保持唤醒 %u 分钟", "KEEPING AWAKE FOR %uMIN") \
  X(KeepAwakeExtendedFmt, "保持唤醒已延长到 %u 分钟", "KEEP AWAKE EXTENDED TO %uMIN") \
  X(KeepAwakeCancelled, "已取消保持唤醒", "KEEP AWAKE CANCELLED") \
  X(SleepNever, "不会自动休眠", "NO AUTO SLEEP") \
  X(SleepInFmt, "无操作 %u 秒后休眠", "SLEEPS AFTER %uS IDLE") \
  X(SleepsFmt, "%u 次（上电以来）", "%u SINCE POWER-ON") \
  X(ReasonForced, "手动设定", "SET MANUALLY") \
  X(ReasonCharging, "充电中", "CHARGING") \
  X(ReasonUsbHost, "USB 已连接", "USB HOST") \
  X(ReasonFullHold, "满电", "FULL") \
  X(HelpUpDownChord, "↑+↓ 同按", "UP+DOWN TOGETHER") \
  X(HelpKeepAwake, "保持唤醒（再按延长/取消）", "KEEP AWAKE (AGAIN: EXTEND/CANCEL)")

enum class Str : uint16_t {  // >255 entries possible
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
