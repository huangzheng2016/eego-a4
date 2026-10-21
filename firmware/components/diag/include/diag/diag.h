// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// diag 组件 —— 串口诊断命令通道（FW5/M18）。
//
// 并发模型（M12 定稿）：后台 diag 任务只做 UART0 阻塞读行 + 入队；
// 命令一律在 app_task 上下文经 service() 分发执行（I2C 单线程调用合同、
// UI/渲染属主唯一、SD 写单属主）。命令处理器由组合根（app_main）以
// Handlers 注入 —— 本组件不依赖 alert_core/net_stack/admin/eego_ui 任何头，
// 保持可独立测试；处理器输出直接用 printf（UART0 控制台，115200）。
//
// secrets 边界（强制）：本组件不读取/不输出任何凭据（WiFi 密码、管理
// 密码、ingest key、token）；help 文本与分发逻辑只含状态/计数类命令。
// 注入命令（inject）只把事件投递给组合根注册的处理器（内部只经
// NetStack 命令队列入队，绝不直写 AlertStore）。

#include <cstdint>
#include <functional>

namespace eego::diag {

// 命令处理器表（app_task 上下文执行；全部为可选，缺省回退 "unavailable"）。
struct Handlers {
  std::function<void()> status;                  // 硬件/启动状态 + 电源模式
  std::function<void()> battery;                 // 电池电压/百分比/充电状态
  std::function<void()> rtc;                     // RTC wall-clock
  std::function<void()> sd;                      // SD 状态 + 配置目录列表
  std::function<void()> net;                     // WiFi + 源健康（queryStatus）
  std::function<void()> alerts;                  // 快照摘要（计数/首条列表）
  std::function<void(const char* json)> inject;  // 告警注入（只入队 net_task）
  std::function<void()> ack_all;                 // 全部确认（AlertActionSink）
  std::function<void()> smoke;                   // 自检序列（PASS/FAIL 清单）
  std::function<void()> version;                 // 版本/chip/heap/帧缓冲信息
  std::function<void(const char* arg)> lang;     // lang zh|en（界面语言切换）
  std::function<void(const char* arg)> page;     // page s0|s1|s2|s3|s4（台架跳转）
  std::function<void(const char* arg)> dashboard;  // dashboard 状态/变量/面板/数据/刷新/smoke
};

// 启动 UART0 读行后台任务（栈/优先级内部固定）。幂等；false = 队列/任务创建失败。
bool start(const Handlers& handlers);

// app_task 主循环调用：非阻塞弹出至多一条命令并在调用方上下文分发执行。
void service();

// 台架/测试旁路：直接注入一行命令（同样经队列，仍在 app_task 上下文执行）。
bool injectLine(const char* line);

}  // namespace eego::diag
