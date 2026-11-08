// SPDX-License-Identifier: GPL-3.0-or-later
#include "ble/tunnel_frame.h"

#include <cstdlib>
#include <cstring>

namespace eego::ble::tunnel {

namespace {

uint8_t* defaultAlloc(size_t bytes) { return static_cast<uint8_t*>(std::malloc(bytes)); }
void defaultRelease(uint8_t* ptr) { std::free(ptr); }

void putU16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xff);
  p[1] = static_cast<uint8_t>(v >> 8);
}

void putU32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xff);
}

uint16_t getU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t getU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

uint32_t crc32(const uint8_t* data, size_t len, uint32_t seed) {
  uint32_t crc = ~seed;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

bool parseHeader(const uint8_t* frame, size_t len, FrameHeader& out) {
  if (frame == nullptr || len < kHeaderBytes) return false;
  out.flags = frame[0];
  out.id = frame[1];
  out.seq = getU16(frame + 2);
  return (out.flags & 0x0c) == 0 && out.type() <= kTypeEvent;
}

void writeHeader(uint8_t* out, uint8_t flags, uint8_t id, uint16_t seq) {
  out[0] = flags;
  out[1] = id;
  putU16(out + 2, seq);
}

void makeAck(uint8_t out[4], uint8_t id, uint16_t next_seq) {
  writeHeader(out, static_cast<uint8_t>(kTypeAck << 4), id, next_seq);
}

void makeAbort(uint8_t out[4], uint8_t id, AbortReason reason) {
  // ABORT 是单帧控制消息：START|END 置位（ACK 不置位）。
  writeHeader(out, static_cast<uint8_t>((kTypeAbort << 4) | kFlagStart | kFlagEnd), id,
              static_cast<uint16_t>(reason));
}

size_t frameCount(size_t total_len, size_t chunk) {
  if (chunk < kMinChunk) return 0;
  const size_t cap = chunk - kHeaderBytes;
  const size_t first = cap - kPreludeBytes;
  if (total_len <= first) return 1;
  return 1 + (total_len - first + cap - 1) / cap;
}

size_t encodeFrames(uint8_t id, const uint8_t* head, uint16_t head_len, const uint8_t* body,
                    size_t body_len, size_t chunk, const FrameSink& sink) {
  if (chunk < kMinChunk || (head_len > 0 && head == nullptr) || (body_len > 0 && body == nullptr)) {
    return 0;
  }
  const size_t total = static_cast<size_t>(head_len) + body_len;
  if (total > 0xffffffffu) return 0;
  uint32_t crc = crc32(head, head_len);
  crc = crc32(body, body_len, crc);
  const size_t frames = frameCount(total, chunk);
  if (frames == 0 || frames > 0xffff) return 0;

  uint8_t frame[512 + kHeaderBytes];
  const size_t limit = chunk > sizeof(frame) ? sizeof(frame) : chunk;
  size_t offset = 0;  // 消息字节偏移
  for (size_t seq = 0; seq < frames; seq++) {
    size_t pos = kHeaderBytes;
    uint8_t flags = static_cast<uint8_t>(kTypeData << 4);
    if (seq == 0) {
      flags |= kFlagStart;
      putU32(frame + pos, static_cast<uint32_t>(total));
      putU32(frame + pos + 4, crc);
      putU16(frame + pos + 8, head_len);
      pos += kPreludeBytes;
    }
    while (pos < limit && offset < total) {
      const size_t room = limit - pos;
      if (offset < head_len) {
        size_t n = head_len - offset;
        if (n > room) n = room;
        std::memcpy(frame + pos, head + offset, n);
        pos += n;
        offset += n;
      } else {
        const size_t body_off = offset - head_len;
        size_t n = body_len - body_off;
        if (n > room) n = room;
        std::memcpy(frame + pos, body + body_off, n);
        pos += n;
        offset += n;
      }
    }
    if (seq + 1 == frames) flags |= kFlagEnd;
    writeHeader(frame, flags, id, static_cast<uint16_t>(seq));
    if (!sink(frame, pos)) return 0;
  }
  return frames;
}

AbortReason abortReasonFor(Result r) {
  switch (r) {
    case Result::Busy: return AbortReason::Busy;
    case Result::TooLarge: return AbortReason::TooLarge;
    case Result::Crc: return AbortReason::Crc;
    case Result::Timeout: return AbortReason::Timeout;
    case Result::Cancelled: return AbortReason::Cancelled;
    default: return AbortReason::SeqGap;
  }
}

Reassembler::Reassembler(size_t max_total, BufferAllocator allocator)
    : max_total_(max_total), allocator_(allocator) {
  if (allocator_.alloc == nullptr || allocator_.release == nullptr) {
    allocator_.alloc = &defaultAlloc;
    allocator_.release = &defaultRelease;
  }
}

Reassembler::~Reassembler() { reset(); }

void Reassembler::reset() {
  if (buffer_ != nullptr) allocator_.release(buffer_);
  buffer_ = nullptr;
  state_ = State::Idle;
  total_ = 0;
  received_ = 0;
  crc_ = 0;
  head_len_ = 0;
  next_seq_ = 0;
  acked_seq_ = 0;
  ack_pending_ = false;
}

void Reassembler::release() { reset(); }

Result Reassembler::fail(Result r) {
  reset();
  return r;
}

bool Reassembler::expire(int64_t now_ms) {
  if (state_ != State::Receiving || now_ms - last_frame_ms_ < kIdleTimeoutMs) return false;
  reset();
  return true;
}

bool Reassembler::ackDue(uint16_t& next_seq) {
  if (!ack_pending_) return false;
  ack_pending_ = false;
  acked_seq_ = next_seq_;
  next_seq = next_seq_;
  return true;
}

Result Reassembler::feed(const uint8_t* frame, size_t len, int64_t now_ms) {
  FrameHeader h;
  if (!parseHeader(frame, len, h)) return state_ == State::Complete ? Result::BadFrame : fail(Result::BadFrame);
  if (h.type() == kTypeAbort) {
    if (state_ == State::Receiving && h.id == id_) reset();
    return Result::Cancelled;
  }
  if (h.type() != kTypeData) return Result::BadFrame;
  if (state_ == State::Complete) return Result::Busy;
  if (state_ == State::Receiving && now_ms - last_frame_ms_ >= kIdleTimeoutMs) reset();

  const uint8_t* payload = frame + kHeaderBytes;
  size_t payload_len = len - kHeaderBytes;
  if (h.start()) {
    // 新消息：放弃任何未完成的旧消息（浏览器重试）。
    reset();
    if (h.seq != 0 || payload_len < kPreludeBytes) return fail(Result::BadFrame);
    const uint32_t total = getU32(payload);
    const uint32_t crc = getU32(payload + 4);
    const uint16_t head_len = getU16(payload + 8);
    if (total > max_total_) return fail(Result::TooLarge);
    if (head_len > total) return fail(Result::BadFrame);
    buffer_ = allocator_.alloc(total > 0 ? total : 1);
    if (buffer_ == nullptr) return fail(Result::TooLarge);
    state_ = State::Receiving;
    id_ = h.id;
    total_ = total;
    crc_ = crc;
    head_len_ = head_len;
    payload += kPreludeBytes;
    payload_len -= kPreludeBytes;
  } else {
    if (state_ != State::Receiving || h.id != id_) return Result::SeqGap;
    if (h.seq != next_seq_) return fail(Result::SeqGap);
  }
  if (received_ + payload_len > total_) return fail(Result::SeqGap);
  if (payload_len > 0) std::memcpy(buffer_ + received_, payload, payload_len);
  received_ += payload_len;
  next_seq_++;
  last_frame_ms_ = now_ms;
  if (static_cast<uint16_t>(next_seq_ - acked_seq_) >= kAckEvery) ack_pending_ = true;
  if (!h.end()) return Result::Ok;
  if (received_ != total_) return fail(Result::SeqGap);
  if (crc32(buffer_, total_) != crc_) return fail(Result::Crc);
  state_ = State::Complete;
  ack_pending_ = true;
  return Result::Complete;
}

}  // namespace eego::ble::tunnel
