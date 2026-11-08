// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// BLE 管理隧道 v1 帧格式（纯 C++，无 IDF/NimBLE 依赖，可在主机单测）。
//
// 帧（小端）：
//   byte 0   flags  bit0 START, bit1 END, bit4-7 TYPE（0 DATA, 1 ACK, 2 ABORT, 3 EVENT）
//   byte 1   消息 id（浏览器选择；响应回显；EVENT 为 0）
//   byte 2-3 seq    DATA: 消息内帧序号（从 0）；ACK: 已连续收到的帧数；ABORT: 原因码
//   byte 4.. payload
// START DATA 帧的 payload 以 10 字节前导开头：u32 total_len、u32 crc32（zlib，覆盖
// head+body）、u16 head_len；随后是消息字节（head JSON + body 原始字节）。
// EVENT 帧为单帧（START|END），payload 是 UTF-8 JSON，无前导。

#include <cstddef>
#include <cstdint>
#include <functional>

namespace eego::ble::tunnel {

constexpr uint8_t kFlagStart = 0x01;
constexpr uint8_t kFlagEnd = 0x02;
constexpr uint8_t kTypeData = 0x0;
constexpr uint8_t kTypeAck = 0x1;
constexpr uint8_t kTypeAbort = 0x2;
constexpr uint8_t kTypeEvent = 0x3;

constexpr size_t kHeaderBytes = 4;
constexpr size_t kPreludeBytes = 10;
constexpr size_t kMinChunk = kHeaderBytes + kPreludeBytes + 1;
constexpr uint16_t kAckEvery = 8;
constexpr int64_t kIdleTimeoutMs = 10000;

constexpr size_t kMaxRequestBytes = 200704;   // 196 KiB body + 4 KiB head
constexpr size_t kMaxResponseBytes = 393216;  // 384 KiB

enum class AbortReason : uint16_t {
  Busy = 1,
  TooLarge = 2,
  SeqGap = 3,
  Crc = 4,
  Timeout = 5,
  Cancelled = 6,
};

struct FrameHeader {
  uint8_t flags = 0;
  uint8_t id = 0;
  uint16_t seq = 0;
  uint8_t type() const { return static_cast<uint8_t>(flags >> 4); }
  bool start() const { return (flags & kFlagStart) != 0; }
  bool end() const { return (flags & kFlagEnd) != 0; }
};

// zlib 语义：crc32(data, len, previous_result)；初值 0。
uint32_t crc32(const uint8_t* data, size_t len, uint32_t seed = 0);

bool parseHeader(const uint8_t* frame, size_t len, FrameHeader& out);
void writeHeader(uint8_t* out, uint8_t flags, uint8_t id, uint16_t seq);

// 控制帧（固定 4 字节）。
void makeAck(uint8_t out[4], uint8_t id, uint16_t next_seq);
void makeAbort(uint8_t out[4], uint8_t id, AbortReason reason);

using FrameSink = std::function<bool(const uint8_t* frame, size_t len)>;

// 把 head+body 切成 DATA 帧（每帧 ≤ chunk 字节）。sink 返回 false 时中止并返回 0。
// 返回帧数。chunk < kMinChunk 或 head_len 与总长不合法时返回 0。
size_t encodeFrames(uint8_t id, const uint8_t* head, uint16_t head_len, const uint8_t* body,
                    size_t body_len, size_t chunk, const FrameSink& sink);

// 帧数（不实际编码）：用于进度/测试。
size_t frameCount(size_t total_len, size_t chunk);

enum class Result : uint8_t {
  Ok = 0,       // 已接收，等待后续帧
  Complete,     // 整条消息到齐且 CRC 正确
  Busy,         // 上一条完整消息尚未 release()
  TooLarge,
  SeqGap,
  Crc,
  Timeout,
  BadFrame,
  Cancelled,    // 对端发送 ABORT
};

AbortReason abortReasonFor(Result r);

struct BufferAllocator {
  uint8_t* (*alloc)(size_t bytes) = nullptr;
  void (*release)(uint8_t* ptr) = nullptr;
};

class Reassembler {
 public:
  explicit Reassembler(size_t max_total = kMaxRequestBytes, BufferAllocator allocator = {});
  ~Reassembler();
  Reassembler(const Reassembler&) = delete;
  Reassembler& operator=(const Reassembler&) = delete;

  Result feed(const uint8_t* frame, size_t len, int64_t now_ms);
  // 正在接收且超过 kIdleTimeoutMs 无新帧 → true（并复位）。
  bool expire(int64_t now_ms);

  bool receiving() const { return state_ == State::Receiving; }
  bool complete() const { return state_ == State::Complete; }
  uint8_t id() const { return id_; }
  const uint8_t* data() const { return buffer_; }
  size_t size() const { return total_; }
  uint16_t headLen() const { return head_len_; }
  uint16_t framesReceived() const { return next_seq_; }

  // 每收满 kAckEvery 帧或消息结束时给出 ACK 序号（下一期望 seq）。
  bool ackDue(uint16_t& next_seq);

  // 处理完完整消息后归还缓冲区（复位到空闲）。
  void release();
  void reset();

 private:
  enum class State : uint8_t { Idle, Receiving, Complete };
  Result fail(Result r);

  size_t max_total_;
  BufferAllocator allocator_;
  State state_ = State::Idle;
  uint8_t* buffer_ = nullptr;
  size_t total_ = 0;
  size_t received_ = 0;
  uint32_t crc_ = 0;
  uint16_t head_len_ = 0;
  uint8_t id_ = 0;
  uint16_t next_seq_ = 0;
  uint16_t acked_seq_ = 0;
  bool ack_pending_ = false;
  int64_t last_frame_ms_ = 0;
};

}  // namespace eego::ble::tunnel
