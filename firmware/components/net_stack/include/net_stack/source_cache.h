// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// 源离线缓存：最后一次成功轮询响应落 cache/<source>.json（M11 §4）。
//
// 布局（默认根目录 /sdcard/alertpanel/cache，SD 缺席时写失败仅记日志，
// 不视为源失败）：
//   * 文件内容为最后一次成功响应的原始 body（未改动，重启后可原样解析）；
//   * fetched_at 取文件 mtime（FAT 2s 粒度，对"offline as of <ts>"足够）；
//   * 写入 tmp+rename 原子替换（M13 对配置写的要求同样适用于缓存——
//     掉电不能毁掉上一份缓存）。
//
// 文件名即 source id；SourceConfig::validate 已限制 [a-z0-9_-]{1,64}，
// 本类仍再校验一次（纵深防御，M13 T4 路径拒 ".."/"/"）。

#include <cstdint>
#include <string>

namespace eego::net {

class SourceCache {
 public:
  explicit SourceCache(std::string dir = "/sdcard/alertpanel/cache");

  const std::string& dir() const { return dir_; }

  // 原子写缓存。source_id 非法或 IO 失败返回 false（err 说明原因）。
  bool write(const std::string& source_id, const char* data, size_t len, std::string& err);

  // 读缓存（重启后总览先显示缓存数据 + "offline as of" 横幅，M11 验收 1）。
  // 返回 false = 无缓存/IO 失败；fetched_at 为 unix 秒（mtime，0 = 取不到）。
  bool read(const std::string& source_id, std::string& body_out,
            uint64_t& fetched_at_out) const;

  bool remove(const std::string& source_id);

  // source id 字符集校验（[a-z0-9_-]{1,64}）
  static bool validSourceId(const std::string& id);

 private:
  std::string pathFor(const std::string& source_id) const;
  std::string dir_;
};

}  // namespace eego::net
