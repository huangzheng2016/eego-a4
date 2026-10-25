# 备份、刷写与恢复

EEGO A4 使用 16 MiB Flash。完整镜像写入 `0x0` 会覆盖 bootloader、分区、应用和
数据；app-only 只适用于已经确认匹配的分区表。当前 ESP-IDF 固件与旧 Arduino
模板的分区布局不同，镜像不能混用。

## 写入前备份

在仓库外准备备份目录，确认 `<PORT>` 后执行：

```sh
python3 -m esptool --chip esp32s3 --port <PORT> flash-id
python3 -m esptool --chip esp32s3 --port <PORT> read-flash \
  0x0 0x1000000 <backup>/eego-a4-original.bin
shasum -a 256 <backup>/eego-a4-original.bin \
  > <backup>/eego-a4-original.bin.sha256
```

备份必须恰好 16,777,216 字节，并至少保留两份。它可能包含 NVS、身份和用户数据，
不要提交到 Git 或公开分享。

## 当前 ESP-IDF 固件

从仓库根目录构建并通过 USB 刷写：

```sh
idf.py -C firmware -B .tmp/firmware-build build
idf.py -C firmware -B .tmp/firmware-build -p <PORT> flash
```

使用 `firmware/partitions.csv`，不要把旧模板 app 镜像写入当前固件，也不要手工
猜测偏移。当前固件没有远程 OTA。

## 旧 Arduino 模板

先构建并打包：

```sh
pio run -e eego_a4_diagnostics
python3 scripts/package.py
python3 release/<version>/safe_flash_template.py \
  --package release/<version> --port <PORT> --preflight-only
```

预检会核对包哈希、芯片、16 MiB Flash 和分区表。首次安装或 OEM/未知分区使用
完整镜像：

```sh
python3 release/<version>/safe_flash_template.py \
  --package release/<version> --port <PORT> --first-install \
  --backup <backup>/eego-a4-before-first-install.bin \
  --confirm-mac 00:00:00:00:00:00
```

只有预检确认模板分区表逐字节匹配时才使用 `--app-update`，并把 `--backup`
换成一个新的不存在路径。真正写入必须准确重复预检显示的 MAC；不要用示例值。
不要移除脚本的备份、分区或 MAC 保护。

旧模板 full 镜像从 `0x0` 写入；app 镜像从 `0x10000` 写入，二者不可交换。

## 从整片备份恢复

先离线确认摘要和备份属于目标设备，再执行：

```sh
shasum -a 256 -c <backup>/eego-a4-original.bin.sha256
python3 -m esptool --chip esp32s3 --port <PORT> --baud 921600 \
  write-flash 0x0 <backup>/eego-a4-original.bin
python3 -m esptool --chip esp32s3 --port <PORT> --baud 921600 \
  verify-flash 0x0 <backup>/eego-a4-original.bin
```

这是有意覆盖整片 Flash 的破坏性操作。恢复后检查启动、电源保持、屏幕、输入、SD
和 USB；精确整片哈希应在首次启动前读回，因为 NVS/OTA 状态会在启动后变化。

## 无法识别或屏幕不更新

USB 端口不出现时，先换数据线和主机端口、关闭占用串口的程序、重新枚举端口，
再让 ESP32-S3 进入 ROM download mode 并尝试 `flash-id`。**不要把 `erase-flash`
当作第一步**，先读取仍可用的 Flash。

MCU 运行但屏幕保留旧画面时，优先检查 GPIO6、EPD SPI、RESET/BUSY，以及 SD 是否
错误复用全局 SPI。墨水屏断电保留旧图是正常现象；framebuffer 回读正常也不能单独
证明 SPI 和实体面板正常。

开发时设备掉电或睡死，检查 `eego::holdPower()` 是否为第一条硬件操作，以及深睡前
是否保持 GPIO4 并设置定时唤醒。
