#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace pics {
constexpr uint16_t company = 0x01ce;
constexpr uint32_t fresh_ms = 5000;
enum class State : uint8_t { NoSignal, Red, BlinkGreen, Green, None, Unknown };
struct Signal {
  State state = State::Unknown;
  int8_t remaining_stage = -1;
};
struct Report {
  uint32_t intersection = 0;
  uint8_t message_id = 0;
  uint8_t count = 0;
  std::array<Signal,6> signal{};
  std::array<uint8_t,6> raw{};
};

inline bool parse_report(const uint8_t* payload, size_t n, Report& out) {
  // Manufacturer ID is already removed here. Only Type 2 is displayed.
  if (!payload || n < 11 || payload[2] != 2) return false;
  Report r{};
  r.intersection = (uint32_t(payload[6]) << 24) | (uint32_t(payload[7]) << 16) |
                   (uint32_t(payload[8]) << 8) | uint32_t(payload[9]);
  r.message_id = payload[3];
  // Match the upstream decoder: up to six channel bytes, ignoring any trailer.
  r.count = static_cast<uint8_t>((n - 10) < 6 ? (n - 10) : 6);
  for (size_t i=0; i<r.count; i++) {
    uint8_t b = payload[i+10];
    r.raw[i] = b;
    r.signal[i].state = (b & 0x0f) <= 4 ? static_cast<State>(b & 0x0f) : State::Unknown;
    r.signal[i].remaining_stage = (b >> 4) < 8 ? int8_t((b >> 4) + 1) : -1;
  }
  out = r;
  return true;
}

inline bool parse_ad(const uint8_t* buf, size_t size, Report& out) {
  // Bluetooth AD structures have [length][type][data]. Never read past the frame.
  if (!buf || size > 31) return false;
  bool found = false;
  Report candidate{};
  size_t at = 0;
  while (at < size) {
    const size_t len = buf[at++];
    if (!len) break;  // Explicit padding / end
    if (len > size - at) return false;
    if (buf[at] == 0xff && len >= 3 &&
        uint16_t(buf[at+1] | (uint16_t(buf[at+2]) << 8)) == company) {
      if (found || !parse_report(buf + at + 3, len - 3, candidate)) return false;
      found = true;
    }
    at += len;
  }
  if (found) out = candidate;
  return found;
}

inline bool same_sample(const Report& a, const Report& b) {
  return a.intersection == b.intersection && a.message_id == b.message_id &&
         a.count == b.count && a.raw == b.raw;
}

struct Snapshot {
  enum class Status : uint8_t { Unknown, Single, Multiple };
  Status status = Status::Unknown;
  Report report{};
  uint32_t age_ms = 0;
};

class Receiver {
 public:
  void reset() { slots_ = {}; overflow_at_ = 0; overflow_ = false; }
  void add(const Report& r, uint32_t now) {
    Slot* reusable = nullptr;
    for (auto& s : slots_) {
      if (s.used && s.r.intersection == r.intersection) {
        // The same retained sample does not keep a state fresh.
        if (same_sample(s.r, r)) return;
        s = Slot{r, now, true};
        return;
      }
      if (!reusable && (!s.used || uint32_t(now - s.at) >= fresh_ms))
        reusable = &s;
    }
    if (reusable) {
      *reusable = Slot{r, now, true};
    } else {
      overflow_ = true;
      overflow_at_ = now;
    }
  }

  Snapshot view(uint32_t now) const {
    Snapshot out{};
    if (overflow_ && uint32_t(now - overflow_at_) < fresh_ms) {
      out.status = Snapshot::Status::Multiple;
      return out;
    }
    for (const auto& s : slots_) {
      if (!s.used || uint32_t(now - s.at) >= fresh_ms) continue;
      if (out.status == Snapshot::Status::Single) {
        return Snapshot{Snapshot::Status::Multiple, {}, 0};
      }
      out = Snapshot{Snapshot::Status::Single, s.r, uint32_t(now - s.at)};
    }
    return out;
  }
 private:
  struct Slot { Report r{}; uint32_t at = 0; bool used = false; };
  std::array<Slot, 8> slots_{};
  uint32_t overflow_at_ = 0;
  bool overflow_ = false;
};
}  // namespace pics
