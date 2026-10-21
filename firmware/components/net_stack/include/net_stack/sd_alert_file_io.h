// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// SD/FatFS 版 IAlertFileIo（M20）—— alert_core 只带宿主用 StdioFileIo，固件侧
// SD 实现此前缺失（persistence.h 注释预留 "M14/M16 接线" 但未落地）。组合根
// （M18/FW5）给 AlertDb 接线：
//
//   static eego::net::SdAlertFileIo alert_io;  // 生命周期 ≥ AlertDb
//   alert_core::AlertDb db(alert_io, "/sdcard/alertpanel/alerts.db");
//   store.attachDb(&db);
//
// 路径与挂载：
//   * 路径为绝对 VFS 路径（/sdcard/...，由 eego_hal SdCard 挂载，LFN/UTF-8）；
//   * SdCard 未挂载时所有操作快速失败并累计 ioFailures()（诊断可见），绝不崩溃。
//
// 持久性语义：
//   * writeAll/append 在 fclose 前 fflush + fsync（FatFS f_sync：数据与元数据
//     落卡）；rename 要求同目录（FatFS f_rename 同目录原子；跨目录返回失败）；
//   * SD 卡自身写缓存不在 fsync 管辖内——掉电最多丢最后一笔追加；alerts.db
//     JSONL 容忍撕裂尾行（load 跳过坏行并计数），压缩走 tmp+read-back+rename。
//
// 线程安全：实例级互斥锁串行化每个操作。FATFS VFS 本身按卷加锁，这里防的是
// 跨任务复合序列交错；store 单属主模型下实际只有 net_task 写 db，读侧（诊断/
// 宿主工具）经同一把锁不会看到半写状态。

#include <cstddef>
#include <mutex>
#include <string>

#include "alert_core/persistence.h"

namespace eego::net {

class SdAlertFileIo : public alert_core::IAlertFileIo {
 public:
  SdAlertFileIo() = default;
  ~SdAlertFileIo() override = default;
  SdAlertFileIo(const SdAlertFileIo&) = delete;
  SdAlertFileIo& operator=(const SdAlertFileIo&) = delete;

  bool writeAll(const std::string& path, const std::string& data) override;
  bool append(const std::string& path, const std::string& data) override;
  bool readAll(const std::string& path, std::string& out) override;
  bool exists(const std::string& path) const override;
  bool remove(const std::string& path) override;
  bool rename(const std::string& from, const std::string& to) override;
  bool fileSize(const std::string& path, size_t& out) const override;

  // 失败计数（未挂载/写失败/rename 失败）——诊断与告警用。
  size_t ioFailures() const {
    std::lock_guard<std::mutex> lk(mu_);
    return io_failures_;
  }

 private:
  // SdCard 挂载守卫；未挂载时累计失败计数并返回 false。
  bool readyLocked() const;

  mutable std::mutex mu_;
  mutable size_t io_failures_ = 0;
  mutable bool ready_logged_ = false;  // 未挂载告警只记一次（恢复后自动复位）
};

}  // namespace eego::net
