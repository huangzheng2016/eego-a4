# Notices and provenance — firmware/

This directory (`firmware/`) is the EEGO A4 alert/data panel firmware: a private,
GPL-3.0-or-later derivative work. It is an independent community project, not
produced, endorsed, or supported by EEGO, Xteink, CrossLink, Huawei, or their
respective owners. Product and project names may be trademarks of their owners.

## Source lineage

| Input | License | Use |
|---|---|---|
| [eego-a4-weread](https://github.com/MokuMMk/eego-a4-weread)（单 commit 85e63c0「项目停止维护」） | GPL-3.0 | 主要移植来源：`components/eego_hal`、`components/eego_ui` 显示/字体/图片/电源实现、`sdkconfig.defaults`、`partitions.csv` 的已验证板级经验 |
| eego-a4-template-firmware（历史 Arduino 模板，已从本仓库移除） | MIT | 板级硬件事实来源（引脚/时序/LUT/触控固件表），经 weread 移植链间接进入 |

衍生作品整体按 GPL-3.0-or-later 许可（见 [LICENSE](LICENSE)）；各源文件头部
SPDX 标识为准。原 weread 项目的阅读器业务形态（书籍/章节/登录/书架）不在
本固件范围内。

## Binary-derived interoperability data

与仓库根 [NOTICE.md](../NOTICE.md) 相同：`components/eego_hal/include/gslx680_firmware.h`
（GSLX680 触控固件表）与 `components/eego_hal/include/uc8279c_luts.h`（UC8279C
波形 LUT）含从用户提供的未加密固件镜像交叉验证出的互操作性数据。镜像本身不
入库；GSL 表可用 `scripts/extract_eego_a4_gsl_firmware.py` 从合法持有的镜像复现。
再分发这些表前，发布者应自行评估适用法律与来源材料条款。

## Deliberately NOT reused（明确未复用/未采用）

按架构定稿（M12 P4 清理）与 `tools/validate_firmware.py` 强制检查：

- **weread 协议/登录/加密代码**：`weread_api`、`weread_client`、`weread_crypto`、
  登录/cookie 流程（`wr_skey`/`wr_rt`/`wr_vid`）一律未复用；本固件无任何
  weread 服务端点。管理面认证为自研 PBKDF2-HMAC-SHA256 + Bearer 会话 +
  X-Ingest-Key（M13 契约）。
- **boot_logo 商标素材**：未复用；本固件不内置任何开机 logo/商标图片，
  UI 图标为代码绘制的几何近似（`eego_ui/include/icons.h`）。
- **miniz**：未采用（weread `components/third_party/miniz`，Unlicense）；
  本固件压缩/图片路径只用 LodePNG。
- **qrcodegen**（MIT，weread 组件内）：评估后未随固件打包；配网走门户页面，
  无二维码渲染需求。
- **HarmonyOS Sans SC 字体**（weread 字体目录，华为免费商用许可）：不随仓库
  分发字体二进制。用户可自备 12px `.eefont` 到 `components/eego_ui/fonts/`
  进行本地嵌入，也可使用 SD 卡 `.eefont`/`.bin` 或内置 5x7 ASCII 回退字体；
  字体许可由使用者自行核对。
- **Noto Sans CJK 字体**（notofonts/noto-cjk，SIL OFL 1.1）：不随仓库分发。
  用户可用 CrossPoint 官方转换工具生成小尺寸 CPFONT 并本地嵌入，许可由
  使用者自行核对。
- **web_flasher**（esptool-js，Apache-2.0）：不在本目录；如需浏览器 USB
  刷机可复用模板仓库的实现，遵守其许可。

## Known license-audit gap（已知缺口）

`components/alert_core/`（FW2/M15 新增的纯 C++ 领域核）22 个源文件暂缺
SPDX 许可头，属许可审计发现；文件为 GPL-3.0-or-later 衍生工程的一部分，
补头工作由后续任务处理。`tools/validate_firmware.py` 会持续报告该缺口直至
修复。
