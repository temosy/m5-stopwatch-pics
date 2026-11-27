#pragma once
#include <cstdint>

struct RedButton {
  enum Action { None, ShutDown, Reboot };
  static constexpr uint32_t debounce=25, hold=1500, click_gap=650;
  bool candidate=false, stable=false, armed=false;
  uint8_t count=0;
  uint32_t changed=0, down=0, released=0;

  void reset() { *this=RedButton{}; }

  Action sample(bool pressed,uint32_t now) {
    if (!armed) {
      if (!pressed) { armed=true; changed=now; }
      return None;
    }
    if (pressed != candidate) { candidate=pressed; changed=now; }
    if (candidate!=stable && uint32_t(now-changed)>=debounce) {
      stable=candidate;
      if (stable) {
        if (count && uint32_t(now-released)>click_gap) count=0;
        down=now;
      } else {
        released=now;
        if (++count==3) { reset(); return Reboot; }
      }
    }
    if (stable && candidate && uint32_t(now-down)>=hold) { reset(); return ShutDown; }
    return None;
  }
};
