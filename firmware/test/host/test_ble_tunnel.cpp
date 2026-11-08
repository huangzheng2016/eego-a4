// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ble/tunnel_frame.h"
#include "json_dom/json.h"
#include "test_framework.h"

using namespace eego::ble::tunnel;

namespace {

using Bytes = std::vector<uint8_t>;

Bytes fromHex(const std::string& hex) {
  Bytes out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  }
  return out;
}

std::string toHex(const uint8_t* p, size_t n) {
  static const char k[] = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) {
    s.push_back(k[p[i] >> 4]);
    s.push_back(k[p[i] & 15]);
  }
  return s;
}

std::vector<Bytes> encode(uint8_t id, const std::string& head, const Bytes& body, size_t chunk) {
  std::vector<Bytes> frames;
  encodeFrames(id, reinterpret_cast<const uint8_t*>(head.data()), static_cast<uint16_t>(head.size()),
               body.data(), body.size(), chunk, [&](const uint8_t* f, size_t n) {
                 frames.emplace_back(f, f + n);
                 return true;
               });
  return frames;
}

Bytes pattern(size_t n) {
  Bytes b(n);
  for (size_t i = 0; i < n; i++) b[i] = static_cast<uint8_t>((i * 7) & 0xff);
  return b;
}

}  // namespace

TEST(ble_tunnel, crc32_vectors) {
  const std::string s = "123456789";
  CHECK_EQ(crc32(reinterpret_cast<const uint8_t*>(s.data()), s.size()), 0xCBF43926u);
  CHECK_EQ(crc32(nullptr, 0), 0u);
  const uint8_t* p = reinterpret_cast<const uint8_t*>(s.data());
  CHECK_EQ(crc32(p + 4, 5, crc32(p, 4)), 0xCBF43926u);
}

TEST(ble_tunnel, roundtrip_chunk_sizes) {
  const std::string head = R"({"m":"PUT","p":"/dashboard","h":{"content-type":"application/json"}})";
  const Bytes body = pattern(5000);
  for (size_t chunk : {size_t(20), size_t(185), size_t(244), size_t(509)}) {
    const auto frames = encode(7, head, body, chunk);
    CHECK_EQ(frames.size(), frameCount(head.size() + body.size(), chunk));
    Reassembler r;
    Result last = Result::Ok;
    for (size_t i = 0; i < frames.size(); i++) {
      CHECK(frames[i].size() <= chunk);
      last = r.feed(frames[i].data(), frames[i].size(), 1000);
      if (i + 1 < frames.size()) CHECK_EQ(last, Result::Ok);
    }
    CHECK_EQ(last, Result::Complete);
    REQUIRE(r.complete());
    CHECK_EQ(r.id(), 7);
    CHECK_EQ(r.headLen(), head.size());
    CHECK_EQ(r.size(), head.size() + body.size());
    CHECK(std::memcmp(r.data(), head.data(), head.size()) == 0);
    CHECK(std::memcmp(r.data() + head.size(), body.data(), body.size()) == 0);
  }
}

TEST(ble_tunnel, single_frame_and_empty_body) {
  const auto frames = encode(3, R"({"s":204})", {}, 509);
  REQUIRE(frames.size() == 1);
  CHECK_EQ(frames[0][0], 0x03);  // DATA | START | END
  Reassembler r;
  CHECK_EQ(r.feed(frames[0].data(), frames[0].size(), 0), Result::Complete);
  CHECK_EQ(r.size(), 9u);
  uint16_t ack = 0;
  CHECK(r.ackDue(ack));
  CHECK_EQ(ack, 1);
  CHECK_EQ(r.feed(frames[0].data(), frames[0].size(), 0), Result::Busy);
  r.release();
  CHECK_EQ(r.feed(frames[0].data(), frames[0].size(), 0), Result::Complete);
}

TEST(ble_tunnel, too_large) {
  Reassembler r(1024);
  const auto frames = encode(1, "{}", pattern(2000), 244);
  CHECK_EQ(r.feed(frames[0].data(), frames[0].size(), 0), Result::TooLarge);
  CHECK(!r.receiving());
  Reassembler big;
  uint8_t start[14] = {0x01, 9, 0, 0};
  const uint32_t total = static_cast<uint32_t>(kMaxResponseBytes);
  std::memcpy(start + 4, &total, 4);
  CHECK_EQ(big.feed(start, sizeof(start), 0), Result::TooLarge);
}

TEST(ble_tunnel, seq_gap_and_crc) {
  const auto frames = encode(4, "{}", pattern(600), 100);
  REQUIRE(frames.size() >= 3);
  Reassembler r;
  CHECK_EQ(r.feed(frames[0].data(), frames[0].size(), 0), Result::Ok);
  CHECK_EQ(r.feed(frames[2].data(), frames[2].size(), 0), Result::SeqGap);
  CHECK(!r.receiving());

  auto bad = frames;
  bad.back()[bad.back().size() - 1] ^= 0x5a;
  Reassembler r2;
  Result last = Result::Ok;
  for (const auto& f : bad) last = r2.feed(f.data(), f.size(), 0);
  CHECK_EQ(last, Result::Crc);
  CHECK(!r2.complete());
}

TEST(ble_tunnel, restart_and_abort) {
  const auto a = encode(1, "{}", pattern(600), 100);
  const auto b = encode(2, "{\"x\":1}", pattern(50), 100);
  Reassembler r;
  CHECK_EQ(r.feed(a[0].data(), a[0].size(), 0), Result::Ok);
  Result last = Result::Ok;
  for (const auto& f : b) last = r.feed(f.data(), f.size(), 0);
  CHECK_EQ(last, Result::Complete);
  CHECK_EQ(r.id(), 2);
  r.release();

  CHECK_EQ(r.feed(a[0].data(), a[0].size(), 0), Result::Ok);
  uint8_t abort[4];
  makeAbort(abort, 1, AbortReason::Cancelled);
  CHECK_EQ(r.feed(abort, sizeof(abort), 0), Result::Cancelled);
  CHECK(!r.receiving());
}

TEST(ble_tunnel, idle_timeout) {
  const auto frames = encode(5, "{}", pattern(600), 100);
  Reassembler r;
  CHECK_EQ(r.feed(frames[0].data(), frames[0].size(), 0), Result::Ok);
  CHECK(!r.expire(kIdleTimeoutMs - 1));
  CHECK(r.expire(kIdleTimeoutMs));
  CHECK(!r.receiving());
}

TEST(ble_tunnel, ack_cadence) {
  const auto frames = encode(6, "{}", pattern(3000), 100);
  REQUIRE(frames.size() > 17);
  Reassembler r;
  std::vector<uint16_t> acks;
  for (const auto& f : frames) {
    r.feed(f.data(), f.size(), 0);
    uint16_t seq = 0;
    if (r.ackDue(seq)) acks.push_back(seq);
  }
  REQUIRE(acks.size() >= 3);
  CHECK_EQ(acks[0], 8);
  CHECK_EQ(acks[1], 16);
  CHECK_EQ(acks.back(), frames.size());
  CHECK(r.complete());
}

TEST(ble_tunnel, control_frames) {
  uint8_t f[4];
  makeAck(f, 2, 8);
  CHECK_EQ(toHex(f, 4), std::string("10020800"));
  makeAbort(f, 5, AbortReason::Crc);
  CHECK_EQ(toHex(f, 4), std::string("23050400"));
  FrameHeader h;
  CHECK(parseHeader(f, 4, h));
  CHECK_EQ(h.type(), kTypeAbort);
  CHECK_EQ(h.seq, 4);
}

TEST(ble_tunnel, shared_fixture) {
  std::ifstream in(std::string(EEGO_FIRMWARE_ROOT) + "/test/host/fixtures/ble_tunnel_vectors.json");
  if (!in) {
    std::fprintf(stderr, "    (shared fixture absent; skipped)\n");
    return;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  eego::json::JsonValue root;
  eego::json::JsonParseError err;
  REQUIRE(eego::json::jsonParse(ss.str(), root, err));
  if (const auto* crcs = root.find("crc32")) {
    for (const auto& v : crcs->arrayItems()) {
      const Bytes data = fromHex(v.find("input_hex")->stringValue());
      char want[9];
      std::snprintf(want, sizeof(want), "%08x", crc32(data.data(), data.size()));
      CHECK_EQ(std::string(want), v.find("crc")->stringValue());
    }
  }
  size_t checked = 0;
  if (const auto* msgs = root.find("messages")) {
    for (const auto& m : msgs->arrayItems()) {
      const uint8_t id = static_cast<uint8_t>(m.find("id")->intValue());
      const size_t chunk = static_cast<size_t>(m.find("chunk")->intValue());
      const std::string head = m.find("head")->stringValue();
      const Bytes body = fromHex(m.find("body_hex")->stringValue());
      const auto frames = encode(id, head, body, chunk);
      const auto& want = m.find("frames")->arrayItems();
      CHECK_EQ(frames.size(), want.size());
      Reassembler r(kMaxResponseBytes);
      Result last = Result::Ok;
      for (size_t i = 0; i < frames.size() && i < want.size(); i++) {
        CHECK_EQ(toHex(frames[i].data(), frames[i].size()), want[i].stringValue());
        const Bytes w = fromHex(want[i].stringValue());
        last = r.feed(w.data(), w.size(), 0);
      }
      CHECK_EQ(last, Result::Complete);
      checked++;
    }
  }
  if (const auto* ctl = root.find("control")) {
    for (const auto& c : ctl->arrayItems()) {
      const Bytes f = fromHex(c.find("frame")->stringValue());
      FrameHeader h;
      CHECK(parseHeader(f.data(), f.size(), h));
      const std::string name = c.find("name")->stringValue();
      uint8_t mine[4];
      if (name.rfind("ack_", 0) == 0) {
        makeAck(mine, h.id, h.seq);
        CHECK_EQ(toHex(mine, 4), c.find("frame")->stringValue());
      } else if (name.rfind("abort_", 0) == 0) {
        makeAbort(mine, h.id, static_cast<AbortReason>(h.seq));
        CHECK_EQ(toHex(mine, 4), c.find("frame")->stringValue());
      } else {
        CHECK_EQ(h.type(), kTypeEvent);
        CHECK(h.start() && h.end());
      }
    }
  }
  CHECK(checked > 0);
}
