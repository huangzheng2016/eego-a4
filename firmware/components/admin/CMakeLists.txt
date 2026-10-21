# SPDX-License-Identifier: GPL-3.0-or-later
# admin 组件 —— FW4 局域网管理与远程管理（M17，契约见 M13 定稿）
#
# 职责：管理 HTTP API（esp_http_server，httpd 上下文只解析/校验/入队）、
# Bearer 会话 / X-Ingest-Key 认证（PBKDF2、NVS sec/wifi 命名空间）、
# 登录与端点限速、Host/Origin 白名单、config.json 白名单校验 + 原子持久化、
# 首次配网门户（限时窗口、APSTA、强制管理密码）、AP 兜底、中英双语管理页。
#
# 并发边界（M12 定稿）：httpd handler 绝不直调 UI / SD 写 / 网络会话；
#   * 配置持久化、恢复出厂、重启 → AdminCommand 队列 → app_task 上下文执行；
#   * 告警摄入 / sources/test 出网 → NetStack 命令队列 → net_task 上下文执行；
#   * 告警读取 → SnapshotPublisher 无锁快照（M20 接线后可用）；
#   * 告警确认/清除 → AlertActionSink 注入点（M20 接线后可用）。
#
# 依赖方向：admin → {alert_core, dashboard_core, data_pipeline, net_stack, eego_hal}
# + ESP-IDF 系统组件；不得 include 任何 UI 头。dashboard 配置复用 M23/M25 的
# 声明式 schema/解析器，不引入脚本运行时。无任何 /firmware/* 或远程 OTA 端点。
idf_component_register(
    SRCS
        "src/admin.cpp"
        "src/admin_log.cpp"
        "src/admin_auth.cpp"
        "src/admin_config.cpp"
        "src/admin_commands.cpp"
        "src/config_transaction.cpp"
        "src/admin_httpd.cpp"
        "src/admin_page.cpp"
        "src/dashboard_api.cpp"
        "src/dashboard_config.cpp"
        "src/dashboard_page.cpp"
        "src/provision_portal.cpp"
        "src/portal_page.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        alert_core
        dashboard_core
        data_pipeline
        device_settings
        net_stack
        eego_hal
        esp_http_server
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        mbedtls
        esp_timer
        esp_hw_support
        heap
        freertos
    PRIV_REQUIRES
        esp_app_format
        fatfs
        vfs
)
