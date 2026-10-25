# 备份、刷写与恢复

EEGO A4 使用 16 MiB Flash。完整镜像写入 `0x0` 会覆盖 bootloader、分区、应用和
数据。只使用当前 ESP-IDF 构建生成的镜像和 `firmware/partitions.csv`，不要混用
其他项目的 app 或分区表。

## 写入前备份

先加载 ESP-IDF 环境，在仓库外准备备份目录并确认 `<PORT>`：

```sh
python3 -m esptool --chip esp32s3 --port <PORT> flash_id
python3 -m esptool --chip esp32s3 --port <PORT> read_flash \
  0x0 0x1000000 <backup>/eego-a4-original.bin
shasum -a 256 <backup>/eego-a4-original.bin \
  > <backup>/eego-a4-original.bin.sha256
```

备份必须恰好 16,777,216 字节，并至少保留两份。它可能包含 NVS、身份和用户数据，
不要提交到 Git 或公开分享。

## 构建与刷写

从仓库根目录执行：

```sh
idf.py -C firmware -B .tmp/firmware-build build
idf.py -C firmware -B .tmp/firmware-build -p <PORT> flash
```

`idf.py flash` 会根据同一构建目录的参数写入 bootloader、分区表和应用，不要手工
猜测偏移。当前固件仅支持 USB 刷写，没有远程 OTA。刷写后可使用：

```sh
idf.py -C firmware -B .tmp/firmware-build -p <PORT> monitor
```

## 从整片备份恢复

先离线确认摘要和备份属于目标设备，再执行：

```sh
shasum -a 256 -c <backup>/eego-a4-original.bin.sha256
python3 -m esptool --chip esp32s3 --port <PORT> --baud 921600 \
  write_flash 0x0 <backup>/eego-a4-original.bin
python3 -m esptool --chip esp32s3 --port <PORT> --baud 921600 \
  verify_flash 0x0 <backup>/eego-a4-original.bin
```

这是有意覆盖整片 Flash 的破坏性操作。恢复后检查启动、电源保持、屏幕、输入、SD
和 USB；精确整片哈希应在首次启动前读回，因为 NVS/OTA 状态会在启动后变化。

## 无法识别或屏幕不更新

USB 端口不出现时，先换数据线和主机端口、关闭占用串口的程序、重新枚举端口，
再让 ESP32-S3 进入 ROM download mode 并尝试 `flash_id`。**不要把 `erase_flash`
当作第一步**，先读取仍可用的 Flash。

MCU 运行但屏幕保留旧画面时，优先检查 GPIO6、EPD SPI3、RESET/BUSY，以及 SD
SPI2 与显示总线所有权。墨水屏断电保留旧图是正常现象；启动日志中的 framebuffer
信息也不能单独证明实体面板已正确刷新。

设备掉电或无法唤醒时，检查启动是否最先保持 GPIO4，以及深睡前是否保持电源并
设置有效唤醒条件。
