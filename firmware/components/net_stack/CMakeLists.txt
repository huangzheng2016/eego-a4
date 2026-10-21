# SPDX-License-Identifier: GPL-3.0-or-later
# net_stack 组件 —— FW3 网络栈与来源适配器（M16）
#
# WiFi STA 状态机（NVS 凭据）+ net_task 单属主（唯一 esp_http_client 属主、
# 唯一 AlertStore 写方）+ KeyedPool 会话池 + 通用 http_poll 适配器 +
# cache/<source>.json 离线缓存 + 源健康统计 + http_push 摄入桥接。
#
# 并发模型（M12 定稿）：net_task 独占 HTTP 会话与 store 写；其他任务经命令
# 队列交互。适配器只产规范 AlertEvent（alert_core），不碰 UI。
#
# 依赖方向：net_stack → alert_core + ESP-IDF 系统组件；不得 include UI 头。
idf_component_register(
    SRCS
        "src/credentials.cpp"
        "src/wifi_manager.cpp"
        "src/http_pool.cpp"
        "src/source_config.cpp"
        "src/source_adapter.cpp"
        "src/source_cache.cpp"
        "src/http_poll_adapter.cpp"
        "src/source_presets.cpp"
        "src/net_task.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        alert_core
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        esp_http_client
        esp_timer
        heap
        mbedtls
        freertos
)
