# SPDX-License-Identifier: GPL-3.0-or-later
# diag 组件 —— 串口诊断命令通道（FW5/M18）
#
# 后台任务只做 UART 读行 + 入队；命令在 app_task 上下文经 service() 分发。
# 处理器由组合根注入，本组件零业务依赖（仅 FreeRTOS + C 库），
# 不 include alert_core/net_stack/admin/eego_ui 任何头。
idf_component_register(
    SRCS "src/diag.cpp"
    INCLUDE_DIRS "include"
    REQUIRES freertos log
)
