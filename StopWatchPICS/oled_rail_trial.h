#pragma once
#include <M5Unified.h>
#include <driver/gpio.h>
#include <utility/M5IOE1_Class.hpp>
#include <lgfx/v1/platforms/esp32/common.hpp>

namespace oled_rail_trial {
// StopWatch C152 display only: M5IOE1 GPIO5=OLED reset, GPIO8=OLED rail.
// M5GFX normally exposes a framebuffer whose init/sleep are not the panel's.
// Access the owned physical panel through a standard base member pointer.
class PanelAccess : public m5gfx::M5GFX {
public:
  static lgfx::Panel_Device* physical(m5gfx::M5GFX& d) {
    return (d.*(&PanelAccess::_panel_last)).get();
  }
};
// M5IOE1 documents physical pins PYG1..14, but the driver indexes from zero.
constexpr uint8_t address=m5::M5IOE1_Class::DEFAULT_ADDRESS;
constexpr uint8_t reset_pin=m5::M5IOE1_Class::gpio5, rail_pin=m5::M5IOE1_Class::gpio8;
constexpr uint8_t reset_reg=0x05+(reset_pin>>3), reset_mask=1u<<(reset_pin&7);
constexpr uint8_t rail_reg=0x05+(rail_pin>>3), rail_mask=1u<<(rail_pin&7);
static_assert(reset_reg==0x05 && reset_mask==0x10 && rail_reg==0x05 && rail_mask==0x80);
constexpr int lines[]={39,40,38,41,42,46,45}; // CS, SCK, TE, D0-D3
inline lgfx::gpio::pin_backup_t saved[7];
inline bool pads_floating=false;
inline bool needs_restore=false;
inline bool tested=false;
inline constexpr uint32_t probe_tag=0x4f4c4432;
enum Error : uint8_t { NoError, Board, Panel, LatchRead, LatchLevel,
                       ResetWrite, PinRelease, RailWrite, WakeFailed };
struct Probe {
  uint32_t tag=probe_tag, attempt=0, blank_ms=0, rail_ms=0, wake_ms=0;
  uint8_t flags=0, button=0, latch=0;
  Error error=NoError;
};
inline Probe probe;

inline bool read(uint8_t reg,uint8_t& value) {
  return M5.In_I2C.readRegister(address,reg,&value,1,100000);
}
inline bool armed_pins(uint8_t& pins) {
  return read(0x05,pins) && (pins&(reset_mask|rail_mask))==(reset_mask|rail_mask);
}
inline void report_preflight() {
  uint8_t pins=0;
  const bool ready=armed_pins(pins);
  Serial.printf("PICS OLED preflight panel=%u reset=%u rail=%u ready=%u\n",
      unsigned(PanelAccess::physical(M5.Display)!=nullptr),unsigned(bool(pins&reset_mask)),
      unsigned(bool(pins&rail_mask)),unsigned(ready));
}
inline bool set(uint8_t reg,uint8_t mask,bool high) {
  uint8_t cur,check;
  if(!read(reg,cur)) return false;
  const uint8_t next=high ? cur|mask : cur&~mask;
  if(next!=cur && !M5.In_I2C.writeRegister8(address,reg,next,100000)) return false;
  return read(reg,check) && (check&mask)==(next&mask);
}
inline bool pins_to_high_z() {
  gpio_config_t cfg={};
  for(unsigned i=0;i<7;++i) {
    cfg.pin_bit_mask |= UINT64_C(1)<<lines[i];
    saved[i].setPin(lines[i]); saved[i].backup();
  }
  cfg.mode=GPIO_MODE_INPUT;
  cfg.pull_up_en=GPIO_PULLUP_DISABLE;
  cfg.pull_down_en=GPIO_PULLDOWN_DISABLE;
  cfg.intr_type=GPIO_INTR_DISABLE;
  pads_floating=true;
  return gpio_config(&cfg)==ESP_OK;
}
inline void restore_pins() {
  if (!pads_floating) return;
  for(auto& pin:saved) pin.restore();
  pads_floating=false;
}
inline bool inactive() { return needs_restore; }
inline bool wake(uint8_t brightness) {
  if(!needs_restore) return true;
  const uint32_t started=millis();
  auto* p=PanelAccess::physical(M5.Display);
  if(!p) return false;
  for(int i=0;i<2;++i) {
    if (!set(reset_reg,reset_mask,false) || !set(rail_reg,rail_mask,true)) continue;
    delay(25);
    restore_pins();
    if(!set(reset_reg,reset_mask,true)) continue;
    delay(65);
    const auto depth=p->getWriteDepth();
    const bool invert=p->getInvert();
    const auto rotation=p->getRotation();
    if(!p->init(false)) continue; // Exact StopWatch command table.
    // Mirror LGFX_Device::init_impl. Panel RAM's input format does not survive rail-off.
    p->setInvert(invert);
    if(p->setColorDepth(depth)!=depth) continue;
    p->setRotation(rotation);
    p->waitDisplay();
    M5.Display.setBrightness(brightness);
    needs_restore=false;
    probe.wake_ms=millis()-started;
    probe.flags|=4;
    Serial.println("PICS OLED trial restored");
    return true;
  }
  if(probe.error==NoError) probe.error=WakeFailed;
  Serial.println("PICS OLED trial recovery needs retry");
  return false;
}
inline bool cut(bool one_shot=true) {
  const uint32_t started=millis();
  if ((one_shot && tested) || needs_restore || M5.Display.getBoard()!=lgfx::board_M5StopWatch) {
    probe.error=Board; return false;
  }
  auto* p=PanelAccess::physical(M5.Display);
  if (!p) { probe.error=Panel; return false; }
  bool read_ok=false;
  for(int i=0;i<3 && !read_ok;++i) {
    read_ok=read(0x05,probe.latch);
    if(!read_ok && i<2) delay(4);
  }
  if(!read_ok) { probe.error=LatchRead; return false; }
  if((probe.latch&(reset_mask|rail_mask))!=(reset_mask|rail_mask)) {
    probe.error=LatchLevel; return false;
  }
  if(one_shot) tested=true;
  // Blank at the controller, rather than transferring a whole black frame.
  M5.Display.setBrightness(0);
  p->waitDisplay();
  p->setSleep(true);
  probe.blank_ms=millis()-started;
  probe.flags|=1;
  delay(125);  // Keep the panel's protection interval after the blank command.
  needs_restore=true;
  if (!set(reset_reg,reset_mask,false)) { probe.error=ResetWrite; return false; }
  if (!pins_to_high_z()) { probe.error=PinRelease; return false; }
  if (!set(rail_reg,rail_mask,false)) { probe.error=RailWrite; return false; }
  probe.rail_ms=millis()-started;
  probe.flags|=2;
  Serial.println("PICS OLED trial rail off (enable readback)");
  return true;
}
}
