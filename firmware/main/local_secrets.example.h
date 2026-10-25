// SPDX-License-Identifier: GPL-3.0-or-later
// 本机默认凭据模板。使用方法：
//
//   cp firmware/main/local_secrets.example.h firmware/main/local_secrets.h
//
// 然后编辑 local_secrets.h（已被 .gitignore 排除，禁止提交）。本模板本身不会被
// 编译；app_main.cpp 只在 local_secrets.h 存在时 include 它。
//
// 用这些宏构建出的固件镜像与 Flash 备份含明文秘密，必须私有保存。
#pragma once

// ── WiFi 默认网络 ──────────────────────────────────────────────────────────────
// 按顺序尝试（列表顺序 = 连接优先级，首项最先）。最多 8 个；SSID 1..32 字节且
// 不重复；密码为空（开放网络）或 8..63 字节。不满足时编译期 static_assert 报错。
// 每次启动都会「确保」：列表中的网络一定存在、密码以这里为准（不同则更新）、并按
// 这里的顺序排在最前；之前保存的其他网络（配网门户/管理端/旧固件写入）保持原顺序
// 排在后面，超过 8 个时丢弃末尾的。启动日志会打印最终列表（只含 SSID）。
// 在管理端删除或改密码这些默认网络，重启后会被恢复——要改请改这里并重新构建。
#define EEGO_LOCAL_WIFI_LIST                    \
  {                                             \
    {"your-home-ssid", "your-home-password"},   \
    {"your-office-ssid", "your-office-password"}, \
    {"your-phone-hotspot", ""},                 \
  }

// 单网络简写（与 EEGO_LOCAL_WIFI_LIST 二选一；同时定义时以 LIST 为准）：
// #define EEGO_LOCAL_WIFI_SSID "your-ssid"
// #define EEGO_LOCAL_WIFI_PASSWORD "your-wifi-password"   // 省略 = 开放网络

// 定义后每次启动只保留上面的默认网络（删除其他已保存网络），用于清理残留网络。
// #define EEGO_LOCAL_WIFI_REPLACE

// ── 管理密码 ──────────────────────────────────────────────────────────────────
// 仅在设备尚未设置管理密码时写入（8..128 字节，编译期检查）。设置后设备启动直接
// 进入管理 API 模式，不再打开配网热点。
#define EEGO_LOCAL_ADMIN_PASSWORD "your-admin-password"

// ── 其他可选项 ────────────────────────────────────────────────────────────────
// 写入 Mains 电源模式（关闭深睡），确认生效后移除：
// #define EEGO_LOCAL_POWER_MODE_MAINS
// 显示时区（分钟，相对 UTC；新设备默认 480 = UTC+8），每次启动写入：
// #define EEGO_LOCAL_TIMEZONE_OFFSET_MIN 480
