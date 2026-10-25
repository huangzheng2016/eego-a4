# SPDX-License-Identifier: GPL-3.0-or-later
# net_stack 组件 —— FW3 网络栈与来源适配器（M16）+ FW3B 告警动作与快照（M20）
# + FW5B 静音规则命令与时间同步（M21）
#
# WiFi STA 状态机（NVS 凭据）+ net_task 单属主（唯一 esp_http_client 属主、
# 唯一 AlertStore 写方）+ KeyedPool 会话池 + http_push 摄入桥接 + alerts.db
# 压缩维护。拉取型数据源统一由 dashboard data_pipeline 经 httpSubmit 执行。
# M20：告警动作（ack/ackAll/clear）命令入口 + SnapshotPublisher 快照发布 +
# expire 过期巡检 + SD/FatFS IAlertFileIo。
# M21：静音规则（type/source/fingerprint + TTL）set/clear/query 命令入口 +
# SNTP 同步（WiFi 连上触发）+ PCF8563 RTC 回写 + 统一时间源状态。
#
# 并发模型（M12 定稿）：net_task 独占 HTTP 会话与 store 写；其他任务经命令
# 队列交互。
#
# 依赖方向：net_stack → alert_core + eego_hal + ESP-IDF 系统组件；不得 include UI 头。
idf_component_register(
    SRCS
        "src/credentials.cpp"
        "src/wifi_manager.cpp"
        "src/http_pool.cpp"
        "src/net_task.cpp"
        "src/store_digest.cpp"
        "src/sd_alert_file_io.cpp"
        "src/time_sync.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        alert_core
        eego_hal
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        esp_http_client
        esp_timer
        heap
        mbedtls
        lwip
        freertos
)
