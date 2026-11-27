#pragma once
#include <cstdint>

namespace daily_alarm {
constexpr uint16_t enabled_bit = 0x8000;
constexpr int default_minutes = 7 * 60;

constexpr int wrap(int value, int count) { return (value + count) % count; }
constexpr int set_hour(int minutes, int delta) {
  return wrap(minutes/60 + delta, 24)*60 + minutes%60;
}
constexpr int set_minute(int minutes, int delta) {
  return (minutes/60)*60 + wrap(minutes%60 + delta, 60);
}
constexpr uint16_t pack(int minutes, bool enabled) {
  return uint16_t(minutes) | (enabled ? enabled_bit : 0);
}
constexpr int minutes_of(uint16_t setting) { return setting & ~enabled_bit; }
constexpr bool due(bool enabled, int setting, int clock, uint32_t day, uint32_t last_day) {
  return enabled && setting == clock && day != 0 && day != last_day;
}
}
