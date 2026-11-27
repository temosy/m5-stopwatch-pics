#pragma once
#include <stdint.h>

namespace oled_auto_record {
constexpr uint64_t marker=0xA000000000000000ULL;
constexpr uint32_t max_seq=0x00ffffff;
constexpr uint32_t next(uint32_t seq){return seq>=max_seq ? 1 : seq+1;}
constexpr uint64_t time12(uint32_t ms){return ms>4095u ? 4095u : ms;}
struct Data { uint32_t seq,rail_ms,wake_ms; uint8_t flags,error; bool usb=false; };
constexpr uint64_t encode(Data d) {
  return marker|(d.seq&max_seq)|(time12(d.rail_ms)<<24)|(time12(d.wake_ms)<<36)|
         (uint64_t(d.flags&7)<<48)|(uint64_t(d.error&15)<<51)|(uint64_t(d.usb)<<55);
}
constexpr bool valid(uint64_t raw) {
  return (raw&0xff00000000000000ULL)==marker && (raw&max_seq)!=0;
}
constexpr Data decode(uint64_t raw) {
  return {uint32_t(raw&max_seq),uint32_t((raw>>24)&4095),uint32_t((raw>>36)&4095),
          uint8_t((raw>>48)&7),uint8_t((raw>>51)&15),bool((raw>>55)&1)};
}
}
