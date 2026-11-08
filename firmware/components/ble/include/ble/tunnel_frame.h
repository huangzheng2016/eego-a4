// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

//
//   byte 4.. payload

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
// Device-initiated uplink messages (proxy requests sent as notifications, proxy responses
// written back by a computer agent). Same layout as DATA.
constexpr uint8_t kTypeProxy = 0x4;

// Central-initiated DATA messages use ids 1..127; device-initiated PROXY messages use
// 128..255. ACK/ABORT frames carry the id, so the range tells which message they belong to.
constexpr uint8_t kProxyIdFirst = 128;
inline bool isProxyId(uint8_t id) { return id >= kProxyIdFirst; }

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

uint32_t crc32(const uint8_t* data, size_t len, uint32_t seed = 0);

bool parseHeader(const uint8_t* frame, size_t len, FrameHeader& out);
void writeHeader(uint8_t* out, uint8_t flags, uint8_t id, uint16_t seq);

void makeAck(uint8_t out[4], uint8_t id, uint16_t next_seq);
void makeAbort(uint8_t out[4], uint8_t id, AbortReason reason);

using FrameSink = std::function<bool(const uint8_t* frame, size_t len)>;

size_t encodeFrames(uint8_t id, const uint8_t* head, uint16_t head_len, const uint8_t* body,
                    size_t body_len, size_t chunk, const FrameSink& sink,
                    uint8_t type = kTypeData);

size_t frameCount(size_t total_len, size_t chunk);

enum class Result : uint8_t {
  Ok = 0,
  Complete,
  Busy,
  TooLarge,
  SeqGap,
  Crc,
  Timeout,
  BadFrame,
  Cancelled,
};

AbortReason abortReasonFor(Result r);

struct BufferAllocator {
  uint8_t* (*alloc)(size_t bytes) = nullptr;
  void (*release)(uint8_t* ptr) = nullptr;
};

class Reassembler {
 public:
  // data_type: the frame type this reassembler collects (kTypeData or kTypeProxy).
  explicit Reassembler(size_t max_total = kMaxRequestBytes, BufferAllocator allocator = {},
                       uint8_t data_type = kTypeData);
  ~Reassembler();
  Reassembler(const Reassembler&) = delete;
  Reassembler& operator=(const Reassembler&) = delete;

  Result feed(const uint8_t* frame, size_t len, int64_t now_ms);
  bool expire(int64_t now_ms);

  bool receiving() const { return state_ == State::Receiving; }
  bool complete() const { return state_ == State::Complete; }
  uint8_t id() const { return id_; }
  const uint8_t* data() const { return buffer_; }
  size_t size() const { return total_; }
  uint16_t headLen() const { return head_len_; }
  uint16_t framesReceived() const { return next_seq_; }

  bool ackDue(uint16_t& next_seq);

  void release();
  void reset();
  // Hands the completed buffer to the caller (who frees it with the allocator's release)
  // and returns to Idle. Returns nullptr unless complete.
  uint8_t* takeBuffer();

 private:
  enum class State : uint8_t { Idle, Receiving, Complete };
  Result fail(Result r);

  size_t max_total_;
  BufferAllocator allocator_;
  uint8_t data_type_ = kTypeData;
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
