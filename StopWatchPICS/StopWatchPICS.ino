// Passive PICS manufacturer advertisements. Research-only, not a crossing aid.
#include <M5Unified.h>
#include <Preferences.h>
#include <esp_timer.h>
#include <time.h>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include "pics_protocol.h"
#include "system_kakari_mark.h"
#include "test_chime.h"
#include "photo_gallery.h"
#include "daily_alarm.h"

namespace {
constexpr uint16_t fg = 0xffff, muted = 0x9cf3, panel = 0x1928, bg = 0x0864;
constexpr uint32_t screen_timeout_ms = 30000;
constexpr uint8_t screen_brightness = 100;
BLEScan* scanner = nullptr;
pics::Receiver receiver;
pics::Receiver test_receiver;
bool test_active = false;
uint32_t test_started_at = 0;
int test_step = -1;
constexpr uint32_t test_duration_ms = 21000;
bool test_audio_ready=false, test_audio_pending=false;
uint32_t test_audio_at=0;
uint8_t selected_channel = 0;
uint32_t focused_id = 0;
bool focused = false;
bool focused_demo = false;
bool cycle_channel = false;

int photo_page = 0;  // 0 is the normal PICS display.
int photo_scale = 1;
int photo_view_x = 0, photo_view_y = 0;
bool pan_pending = false;
uint32_t last_pan_paint = 0;

M5Canvas zoom_sprite;
int cached_page = 0, cached_scale = 0;
bool cached_success = false;
bool first_tap = false;
int first_tap_x = 0, first_tap_y = 0;
uint32_t first_tap_at = 0;

struct GalleryGesture {
  int direction = 0;
  bool tap = false;
  int x = 0, y = 0;
  int dx = 0, dy = 0;
  bool drag_end = false;
};

GalleryGesture gallery_gesture(uint32_t now) {
  static bool tracking = false;
  static uint16_t touch_id = 0;
  static uint32_t started_at = 0;
  static int last_x = 0, last_y = 0;
  static bool dragging = false;
  if (M5.Touch.getCount() != 1) {
    tracking = false;
    return {};
  }
  const auto& touch = M5.Touch.getDetail();
  if (touch.wasPressed()) {
    tracking = true;
    touch_id = touch.id;
    started_at = now;
    last_x = touch.x;
    last_y = touch.y;
    dragging = false;
    return {};
  }
  if (!tracking || touch.id != touch_id) return {};
  if (touch.isPressed() && photo_page && photo_scale > 1) {
    if (!dragging && gallery::magnitude(touch.distanceX()) +
                    gallery::magnitude(touch.distanceY()) < 12) return {};
    dragging = true;
    const int dx = touch.x - last_x, dy = touch.y - last_y;
    last_x = touch.x;
    last_y = touch.y;
    return {0, false, touch.x, touch.y, dx, dy, false};
  }
  if (!touch.wasReleased()) return {};
  tracking = false;
  if (dragging) return {0, false, touch.x, touch.y, 0, 0, true};
  const int dx = touch.distanceX(), dy = touch.distanceY();
  const uint32_t elapsed = now - started_at;
  return {gallery::swipe_direction(dx, dy, elapsed),
          touch.wasClicked() && elapsed <= 260 &&
              gallery::magnitude(dx) <= 16 && gallery::magnitude(dy) <= 16,
          touch.x, touch.y};
}

void release_zoom_cache() {
  if (cached_success) zoom_sprite.deleteSprite();
  cached_page = cached_scale = 0;
  cached_success = false;
}

void reset_gallery_gesture() {
  photo_scale = 1;
  first_tap = false;
  pan_pending = false;
  release_zoom_cache();
}

int photo_view_height() { return (M5.Display.height()-57) & ~1; }


int photo_left(const gallery::Photo& p) {
  return ((M5.Display.width()-p.width)/2) & ~1;
}
int photo_top(const gallery::Photo& p) {
  return ((M5.Display.height()-p.height)/2 - 12) & ~1;
}

bool apply_gallery_tap(const GalleryGesture& g, uint32_t now) {
  const auto& p = gallery::photos[photo_page-1];
  if (photo_scale == 1 && (g.x < photo_left(p) || g.x >= photo_left(p)+p.width ||
                        g.y < photo_top(p) || g.y >= photo_top(p)+p.height)) {
    first_tap = false;
    return false;
  }
  if (first_tap && gallery::nearby_tap(g.x-first_tap_x, g.y-first_tap_y,
                                       now-first_tap_at)) {
    first_tap = false;
    const int source_x = photo_scale == 1 ? g.x-photo_left(p)
      : gallery::clamp_value((g.x-photo_view_x)/photo_scale, 0, p.width-1);
    const int source_y = photo_scale == 1 ? g.y-photo_top(p)
      : gallery::clamp_value((g.y-photo_view_y)/photo_scale, 0, p.height-1);
    photo_scale = gallery::step_zoom(photo_scale);
    pan_pending = false;
    release_zoom_cache();
    if (photo_scale > 1) {
      photo_view_x = gallery::zoom_origin(p.width, M5.Display.width(), source_x, photo_scale);
      photo_view_y = gallery::zoom_origin(p.height, photo_view_height(), source_y, photo_scale);
    }
    return true;
  }
  first_tap = true;
  first_tap_x = g.x;
  first_tap_y = g.y;
  first_tap_at = now;
  return false;
}

bool draw_zoom_view(const gallery::Photo& p) {
  auto& d = M5.Display;
  const auto* jpeg = photo_scale == 4 ? p.zoom4_jpeg : p.zoom_jpeg;
  const size_t bytes = photo_scale == 4 ? p.zoom4_bytes : p.zoom_bytes;
  if (cached_page != photo_page || cached_scale != photo_scale) {
    release_zoom_cache();
    cached_page = photo_page;
    cached_scale = photo_scale;
    zoom_sprite.setPsram(true);
    zoom_sprite.setColorDepth(16);
    if (zoom_sprite.createSprite(p.width * photo_scale, p.height * photo_scale)) {
      cached_success = zoom_sprite.drawJpg(jpeg, bytes, 0, 0);
      if (!cached_success) zoom_sprite.deleteSprite();
    }
  }
  d.setClipRect(0, 0, d.width(), photo_view_height());
  const int x = photo_view_x & ~1, y = photo_view_y & ~1;
  bool ok = true;
  if (cached_success) zoom_sprite.pushSprite(&d, x, y);
  else ok = d.drawJpg(jpeg, bytes, x, y);
  d.clearClipRect();
  return ok;
}

bool move_zoom_view(int dx, int dy) {
  const auto& p = gallery::photos[photo_page-1];
  const int x = gallery::pan_origin(photo_view_x, dx, p.width * photo_scale, M5.Display.width());
  const int y = gallery::pan_origin(photo_view_y, dy, p.height * photo_scale, photo_view_height());
  const bool changed = x != photo_view_x || y != photo_view_y;
  photo_view_x = x;
  photo_view_y = y;
  return changed;
}

void draw_gallery_photo(int page) {
  const auto& photo = gallery::photos[page-1];
  auto& d = M5.Display;
  d.fillScreen(0x0000);
  bool ok;
  if (photo_scale > 1) {
    ok = draw_zoom_view(photo);
  } else {
    ok = d.drawJpg(photo.jpeg, photo.bytes, photo_left(photo), photo_top(photo));
  }
  char label[22];
  if (photo_scale == 1) snprintf(label, sizeof(label), "%d / %d", page, gallery::photo_count);
  else snprintf(label, sizeof(label), "%d / %d  %dx", page, gallery::photo_count, photo_scale);
  d.setFont(&lgfx::fonts::Font0);
  d.setTextSize(2);
  d.setTextColor(0xffff, 0x0000);
  d.setTextDatum(lgfx::middle_center);
  d.drawString(ok ? label : "Image error", d.width()/2, d.height()-44);
  d.setTextDatum(lgfx::top_left);
  Serial.printf("PICS gallery photo=%d/%d ok=%u zoom=%d\n", page, gallery::photo_count,
                unsigned(ok), photo_scale);
}

bool selectable(pics::State state) {
  return state==pics::State::Red || state==pics::State::Green ||
         state==pics::State::BlinkGreen;
}
uint8_t pick_channel(const pics::Report& r,uint8_t start) {
  const uint8_t count = r.count > 6 ? 6 : r.count;
  if (!count) return 0;
  for (uint8_t i=0; i<count; ++i) {
    const uint8_t c=(start+i)%count;
    if (selectable(r.signal[c].state)) return c;
  }
  return 0;
}

bool feed_test_report(uint8_t step,uint32_t now) {
  // Synthetic bytes are local only. Never advertise, never enter the live Receiver.
  uint8_t p[20] = {19,0xff,0xce,0x01,0,0,2,step,0,0,0x54,0x45,0x53,0x54};
  // Red 8→1, green 8→1, then blinking green 3→1; these are stages.
  const uint8_t state=step<8 ? 1 : step<16 ? 3 : 2;
  const uint8_t level=step<8 ? 7-step : step<16 ? 15-step : 18-step;
  p[14]=(level<<4)|state;
  p[15]=((7-level)<<4)|(state==1 ? 3 : 1);
  p[16]=0x84; p[17]=0x84; p[18]=0x84; p[19]=0x84;
  pics::Report frame;
  if (!pics::parse_ad(p,sizeof(p),frame)) return false;
  test_receiver.add(frame,now);
  return true;
}

void end_test() {
  test_active=false;
  test_receiver.reset();
  test_audio_pending=false;
  if (test_audio_ready) {
    M5.Speaker.stop(0);
    M5.Speaker.end();
    test_audio_ready=false;
  }
  Serial.println("PICS TEST end");
}

bool update_test(uint32_t now) {
  if (!test_active) return false;
  if (test_audio_pending && int32_t(now-test_audio_at)>=0) {
    test_audio_pending=false;
    const bool queued=M5.Speaker.playRaw(test_chime_pcm,
      sizeof(test_chime_pcm)/sizeof(test_chime_pcm[0]),test_chime_sample_rate,false,1,0,true);
    Serial.printf("PICS TEST chime queued=%u running=%u\n",
      unsigned(queued),unsigned(M5.Speaker.isRunning()));
  }
  const uint32_t age=now-test_started_at;
  if (age>=test_duration_ms) { end_test();return true; }
  const int step=int(age/800);
  if (step>=19 || step==test_step) return false;
  test_step=step;
  const bool decoded=feed_test_report(uint8_t(step),now);
  if (step==0 || step==8 || step==16) {
    if (!test_audio_ready) {
      M5.Speaker.setVolume(225);
      M5.Speaker.setChannelVolume(0,255);
      test_audio_ready=M5.Speaker.begin();
      test_audio_at=millis()+140;
    } else test_audio_at=now;
    test_audio_pending=test_audio_ready;
  }
  Serial.printf("PICS TEST sample=%d decoded=%u\n",step+1,unsigned(decoded));
  return true;
}
portMUX_TYPE rx_lock = portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> scan_done{false};
bool start_pending = false;
uint32_t start_retry = 0;
uint32_t scan_cycles = 0;
uint32_t screen_activity_at = 0;
bool screen_awake = true;
bool panel_ready = false;
int battery_percent = -1;
uint32_t battery_sample_at = 0;

enum class PowerSource : uint8_t { Unknown, Battery, External };
constexpr uint32_t endurance_tag = 0x45525431;
struct EnduranceRecord {
  uint32_t tag = endurance_tag;
  uint32_t last_seconds = 0;
  uint32_t checkpoint_seconds = 0;
  uint8_t last_valid = 0;
  uint8_t last_interrupted = 0;
  uint8_t active = 0;
  uint8_t reserved = 0;
};
static_assert(sizeof(EnduranceRecord) == 16);
Preferences endurance_store;
EnduranceRecord endurance;
bool endurance_can_save = false;
uint64_t battery_started_ms = 0;
PowerSource power_source = PowerSource::Unknown;
PowerSource source_candidate = PowerSource::Unknown;
uint8_t source_votes = 0;
uint64_t source_candidate_ms = 0;
uint64_t next_source_sample_ms = 0;
bool clock_set_jst = false;
constexpr uint8_t rx8130_address = 0x32, rx8130_flag = 0x1D;
constexpr uint32_t rtc_i2c_hz = 100000;
char clock_label[34] = "日時未設定（JST）";

bool read_clock() {
  m5::rtc_datetime_t dt;
  uint8_t flags = 0;
  if (!clock_set_jst || !M5.Rtc.isEnabled() ||
      !M5.In_I2C.readRegister(rx8130_address, rx8130_flag, &flags, 1, rtc_i2c_hz) ||
      (flags & 0x02) || !M5.Rtc.getDateTime(&dt) ||
      dt.date.year < 2025 || dt.date.year > 2099) {
    snprintf(clock_label, sizeof(clock_label), "日時未設定（JST）");
    return false;
  }
  snprintf(clock_label, sizeof(clock_label), "%04d/%02d/%02d %02d:%02d:%02d",
      int(dt.date.year),int(dt.date.month),int(dt.date.date),int(dt.time.hours),
      int(dt.time.minutes),int(dt.time.seconds));
  return true;
}

void poll_clock_command() {
  static char line[32];
  static uint8_t len = 0;
  static bool overflow = false;
  for (int budget=48; budget>0 && Serial.available(); --budget) {
    const int c=Serial.read();
    if (c=='\r') continue;
    if (c=='\n') {
      line[len]=0;
      if (!overflow && len==20 && !memcmp(line,"JST_EPOCH ",10) &&
          power_source==PowerSource::External) {
        uint64_t epoch=0;
        bool digits=true;
        for (int i=10; i<20; ++i) {
          if (line[i]<'0' || line[i]>'9') { digits=false; break; }
          epoch=epoch*10+(line[i]-'0');
        }
        if (digits && epoch>=1735689600ULL && epoch<4102416000ULL) {
          const time_t in_jst=time_t(epoch+9*3600);
          tm value={};
          if (gmtime_r(&in_jst,&value) && M5.Rtc.setDateTime(&value) &&
              M5.In_I2C.writeRegister8(rx8130_address,rx8130_flag,0xBD,rtc_i2c_hz)) {
            // Flag register is W0C: preserve other flags and clear only VLF.
            clock_set_jst=true;
            if (endurance_can_save) endurance_store.putBool("clock_jst",true);
            if (read_clock()) Serial.printf("PICS clock set %s JST\n",clock_label);
            else Serial.println("PICS clock readback failed");
          } else Serial.println("PICS clock set failed");
        }
      }
      len=0; overflow=false;
    } else if (c>=0 && len<sizeof(line)-1) line[len++]=char(c);
    else overflow=true;
  }
}

constexpr uint32_t alarm_hold_ms = 850;
constexpr uint32_t alarm_ring_ms = 60000;
constexpr uint8_t alarm_volume = 255;
uint16_t alarm_setting = daily_alarm::pack(daily_alarm::default_minutes, false);
uint32_t alarm_last_day = 0;
int alarm_edit_minutes = daily_alarm::default_minutes;
bool alarm_edit_on = false, alarm_setting_open = false, alarm_save_error = false;
bool alarm_ringing = false, alarm_preview = false, alarm_audio = false;
bool alarm_ignore_yellow_release = false, alarm_ignore_blue_release = false;
uint32_t alarm_started = 0, alarm_next_sound = 0, alarm_clock_at = 0;
unsigned alarm_tone_index = 0;

void load_alarm() {
  if (endurance_can_save) {
    const uint16_t stored = endurance_store.getUShort("alarm_cfg", alarm_setting);
    if (daily_alarm::minutes_of(stored) < 1440) alarm_setting = stored;
    alarm_last_day = endurance_store.getUInt("alarm_day", 0);
  }
  Serial.printf("PICS alarm ready enabled=%u minutes=%d volume=%u persisted=%u\n",
                unsigned(bool(alarm_setting & daily_alarm::enabled_bit)),
                daily_alarm::minutes_of(alarm_setting),unsigned(alarm_volume),
                unsigned(endurance_can_save && endurance_store.getUShort("alarm_cfg",0)==alarm_setting));
}

bool save_alarm() {
  const uint16_t value = daily_alarm::pack(alarm_edit_minutes, alarm_edit_on);
  if (!endurance_can_save || endurance_store.putUShort("alarm_cfg", value) != sizeof(value)) {
    alarm_save_error = true;
    Serial.println("PICS alarm save failed");
    return false;
  }
  alarm_setting = value;
  Serial.printf("PICS alarm saved enabled=%u minutes=%d\n",
                unsigned(alarm_edit_on), alarm_edit_minutes);
  return true;
}

bool clock_stamp(uint32_t& day, int& minute) {
  if (!read_clock()) return false;
  int y=0,m=0,d=0,h=0,n=0;
  if (sscanf(clock_label,"%d/%d/%d %d:%d",&y,&m,&d,&h,&n) != 5 ||
      m<1 || m>12 || d<1 || d>31 || h<0 || h>23 || n<0 || n>59) return false;
  day = uint32_t(y)*10000u + uint32_t(m)*100u + uint32_t(d);
  minute = h*60+n;
  return true;
}

void begin_alarm(uint32_t now, bool preview) {
  if (test_active) end_test();
  alarm_ringing = true;
  alarm_preview = preview;
  alarm_started = now;
  alarm_next_sound = now + 140;
  alarm_tone_index = 0;
  M5.Speaker.setVolume(alarm_volume);
  M5.Speaker.setChannelVolume(0,255);
  alarm_audio = M5.Speaker.begin();
  M5.Display.setBrightness(screen_brightness);
  screen_awake = true;
  screen_activity_at = now;
  Serial.printf("PICS alarm %s speaker=%u\n", preview ? "preview" : "ring", unsigned(alarm_audio));
}

void end_alarm(uint32_t now) {
  if (alarm_audio) {
    M5.Speaker.stop(0);
    M5.Speaker.end();
  }
  alarm_audio = alarm_ringing = false;
  screen_activity_at = now;
  Serial.println("PICS alarm stopped");
}

bool alarm_tick(uint32_t now) {
  if (alarm_ringing) {
    if (now - alarm_started >= (alarm_preview ? 4000u : alarm_ring_ms)) {
      end_alarm(now);
      return true;
    }
    if (alarm_audio && int32_t(now - alarm_next_sound) >= 0) {
      const bool second = (alarm_tone_index++ % 2) != 0;
      const bool queued = M5.Speaker.tone(second ? 1047 : 784, 185, 0);
      if (alarm_tone_index == 1) Serial.printf("PICS alarm tone queued=%u\n",unsigned(queued));
      alarm_next_sound = now + (second ? 640 : 250);
    }
    return false;
  }
  if (!(alarm_setting & daily_alarm::enabled_bit) || int32_t(now-alarm_clock_at)<0) return false;
  alarm_clock_at = now + 1000;
  uint32_t day=0; int minute=0;
  if (!clock_stamp(day,minute) ||
      !daily_alarm::due(true,daily_alarm::minutes_of(alarm_setting),minute,day,alarm_last_day)) return false;
  alarm_last_day = day;
  if (endurance_can_save) endurance_store.putUInt("alarm_day", day);
  alarm_setting_open = false;
  photo_page = 0;
  reset_gallery_gesture();
  begin_alarm(now,false);
  return true;
}

uint64_t elapsed_ms() { return uint64_t(esp_timer_get_time()) / 1000; }

bool save_endurance() {
  if (!endurance_can_save) return false;
  const bool ok = endurance_store.putBytes("record", &endurance, sizeof(endurance))
                  == sizeof(endurance);
  if (!ok) Serial.println("PICS endurance save failed");
  return ok;
}

void load_endurance() {
  endurance_can_save = endurance_store.begin("pics_runtime", false);
  if (!endurance_can_save) {
    Serial.println("PICS endurance storage unavailable");
    return;
  }
  if (endurance_store.getBytesLength("record") == sizeof(endurance)) {
    EnduranceRecord loaded;
    if (endurance_store.getBytes("record", &loaded, sizeof(loaded)) == sizeof(loaded) &&
        loaded.tag == endurance_tag) endurance = loaded;
  }
  if (endurance.active) {
    endurance.last_seconds = endurance.checkpoint_seconds;
    endurance.last_valid = true;
    endurance.last_interrupted = true;
    endurance.active = false;
    endurance.checkpoint_seconds = 0;
    save_endurance();
  }
  clock_set_jst = endurance_store.getBool("clock_jst", false);
  Serial.printf("PICS endurance ready last_valid=%u last_s=%lu interrupted=%u\n",
      unsigned(endurance.last_valid), (unsigned long)endurance.last_seconds,
      unsigned(endurance.last_interrupted));
}

uint32_t battery_run_seconds(uint64_t now) {
  return endurance.active ? uint32_t((now - battery_started_ms) / 1000) : 0;
}

void update_power_source(uint64_t now) {
  if (now < next_source_sample_ms) return;
  next_source_sample_ms = now + 1000;
  uint8_t raw[2];
  // StopWatch M5PM1 VIN ADC (0x24-0x25); retain I2C error vs a real zero.
  const bool ok = M5.Power.M5pm1.readRegister(0x24, raw, sizeof(raw));
  const int vin_mv = ok ? int(raw[0]) + (int(raw[1]) << 8) : -1;
  const PowerSource measured = vin_mv >= 4200 && vin_mv < 6000 ? PowerSource::External :
      (vin_mv >= 0 && vin_mv <= 2000 ? PowerSource::Battery : PowerSource::Unknown);
  if (measured == PowerSource::Unknown || measured == power_source) {
    source_candidate = PowerSource::Unknown;
    source_votes = 0;
  } else {
    if (measured != source_candidate) {
      source_candidate = measured;
      source_candidate_ms = now;
      source_votes = 1;
    } else if (++source_votes >= 3) {
      power_source = measured;
      source_votes = 0;
      if (power_source == PowerSource::Battery) {
        battery_started_ms = source_candidate_ms;
        endurance.active = true;
        endurance.checkpoint_seconds = 0;
        save_endurance();
        Serial.printf("PICS endurance battery start vin=%d\n", vin_mv);
      } else {
        if (endurance.active) {
          endurance.last_seconds = battery_run_seconds(source_candidate_ms);
          endurance.last_valid = true;
          endurance.last_interrupted = false;
          endurance.active = false;
          endurance.checkpoint_seconds = 0;
          save_endurance();
          Serial.printf("PICS endurance finish last_s=%lu vin=%d\n",
                        (unsigned long)endurance.last_seconds, vin_mv);
        } else {
          Serial.printf("PICS endurance USB vin=%d\n", vin_mv);
        }
      }
    }
  }
  if (endurance.active && measured == PowerSource::Battery) {
    const uint32_t secs = battery_run_seconds(now);
    if (secs - endurance.checkpoint_seconds >= 60) {
      endurance.checkpoint_seconds = secs;
      save_endurance();
    }
  }
}

void format_duration(char* out, size_t n, uint32_t secs) {
  snprintf(out, n, "%02lu:%02lu:%02lu", (unsigned long)(secs / 3600),
           (unsigned long)((secs / 60) % 60), (unsigned long)(secs % 60));
}

bool update_battery() {
  int sample = M5.Power.getBatteryLevel();
  if (sample < 0 || sample > 100) sample = -1;
  const bool changed = sample != battery_percent;
  battery_percent = sample;
  battery_sample_at = millis();
  if (changed) Serial.printf("PICS battery estimate=%d\n", battery_percent);
  return changed;
}

class ScanReceiver final : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    pics::Report r{};
    if (!pics::parse_ad(dev.getPayload(), dev.getPayloadLength(), r)) return;
    const uint32_t t = millis();
    portENTER_CRITICAL(&rx_lock);
    receiver.add(r, t);
    portEXIT_CRITICAL(&rx_lock);
  }
};
ScanReceiver adv_cb;

void scanComplete(BLEScanResults) { scan_done.store(true); }

const char* signal_label(pics::State s) {
  switch (s) {
    case pics::State::NoSignal: return "NO SIGNAL";
    case pics::State::Red: return "RED";
    case pics::State::BlinkGreen: return "GREEN BLINK";
    case pics::State::Green: return "GREEN";
    case pics::State::None: return "N/A";
    default: return "UNKNOWN";
  }
}

void line(int y, const char* content, uint8_t size, uint16_t color = fg) {
  auto& d = M5.Display;
  d.setTextDatum(lgfx::textdatum_t::middle_center);
  d.setTextColor(color);
  d.setTextSize(size);
  d.drawString(content, d.width()/2, y);
}

void alarm_tile(int x, int y, int w, const char* label, uint16_t color) {
  auto& d=M5.Display;
  d.fillRoundRect(x,y,w,48,12,panel);
  d.setFont(&lgfx::fonts::lgfxJapanGothicP_24);
  d.setTextDatum(lgfx::textdatum_t::middle_center);
  d.setTextSize(1);
  d.setTextColor(color);
  d.drawString(label,x+w/2,y+24);
}

void draw_alarm() {
  auto& d=M5.Display;
  d.fillScreen(bg);
  d.setFont(&lgfx::fonts::lgfxJapanGothicP_24);
  line(61, alarm_ringing ? (alarm_preview ? "アラーム試聴" : "アラーム") : "毎日のアラーム", 1, 0xffc8);
  if (alarm_ringing) {
    d.setFont(&lgfx::fonts::FreeSansBold24pt7b);
    if (read_clock()) {
      char hhmm[6];
      snprintf(hhmm,sizeof(hhmm),"%.5s",clock_label+11);
      line(217,hhmm,2);
    } else line(217,"ALARM",2);
    d.setFont(&lgfx::fonts::lgfxJapanGothic_16);
    line(320,"画面・黄・青：停止",1,0xffc8);
    return;
  }
  const bool valid=read_clock();
  d.setFont(&lgfx::fonts::lgfxJapanGothic_16);
  line(105, valid ? "日本時間・毎日・音量 MAX" : "時計未設定のため鳴りません",1,valid ? muted : 0xfaaa);
  char tm[8];
  snprintf(tm,sizeof(tm),"%02d:%02d",alarm_edit_minutes/60,alarm_edit_minutes%60);
  d.setFont(&lgfx::fonts::FreeSansBold24pt7b);
  line(161,tm,1,fg);
  alarm_tile(65,197,76,"時 −",fg);
  alarm_tile(148,197,76,"時 ＋",fg);
  alarm_tile(244,197,76,"分 −",fg);
  alarm_tile(327,197,76,"分 ＋",fg);
  alarm_tile(81,269,188,alarm_edit_on ? "ON  有効" : "OFF  無効",alarm_edit_on ? 0xaff5 : muted);
  alarm_tile(281,269,107,"試聴",0x86ff);
  d.setFont(&lgfx::fonts::lgfxJapanGothic_16);
  line(351,"黄：戻る　青：保存",1,fg);
  line(373,"青 長押し：試聴",1,0x86ff);
  if (alarm_save_error) line(394,"保存できませんでした",1,0xfaaa);
}

bool alarm_click(uint32_t now) {
  if (!M5.Touch.isEnabled() || M5.Touch.getCount()!=1) return false;
  const auto& t=M5.Touch.getDetail();
  // Use the first contact, not the library's click classification on release.
  if (!t.wasPressed()) return false;
  Serial.printf("PICS alarm touch x=%d y=%d\n",int(t.x),int(t.y));
  if (alarm_ringing) {
    end_alarm(now);
    return true;
  }
  if (t.y>=197 && t.y<245) {
    if (t.x>=65 && t.x<141) alarm_edit_minutes=daily_alarm::set_hour(alarm_edit_minutes,-1);
    else if (t.x>=148 && t.x<224) alarm_edit_minutes=daily_alarm::set_hour(alarm_edit_minutes,1);
    else if (t.x>=244 && t.x<320) alarm_edit_minutes=daily_alarm::set_minute(alarm_edit_minutes,-1);
    else if (t.x>=327 && t.x<403) alarm_edit_minutes=daily_alarm::set_minute(alarm_edit_minutes,1);
    else return false;
    return true;
  }
  // Include near-edge touches on the round panel, below the button graphics.
  if (t.y>=255 && t.y<363) {
    if (t.x>=72 && t.x<269) { alarm_edit_on=!alarm_edit_on; return true; }
    if (t.x>=273 && t.x<409) { begin_alarm(now,true); return true; }
  }
  return false;
}

constexpr int mascot_band_y = 94;
constexpr int mascot_base_y = 96;
constexpr int mascot_band_w = 300;
constexpr uint32_t mascot_frame_ms=33;
uint32_t mascot_at=0;
uint32_t mascot_walk_ms=0;
uint32_t mascot_previous_ms=0;
uint32_t mascot_stats_at=0;
uint32_t mascot_stats_max=0, mascot_stats_frames=0, mascot_stats_slow=0;
int mascot_prev_x[2]={-1,-1};

void draw_dog(int x, int y, bool right, uint32_t walk_ms, bool mocha) {
  auto& d = M5.Display;
  const uint16_t coat = mocha ? 0xffdd : 0xd3ca;
  const uint16_t shade = mocha ? 0xbdf7 : 0x9265;
  const uint16_t cream = mocha ? 0xffff : 0xf64f;
  const uint16_t inner = mocha ? 0xfcd3 : 0xe3ef;
  const uint16_t face = 0x2104;
  auto X = [&](int u) { return x + (right ? u : 37 - u); };
  auto rect = [&](int u, int v, int w, int h, int r, uint16_t c) {
    d.fillRoundRect(X(right ? u : u+w-1), y+v, w, h, r, c);
  };
  auto tri = [&](int a,int b,int c,int e,int f,int g,uint16_t color) {
    d.fillTriangle(X(a),y+b,X(c),y+e,X(f),y+g,color);
  };
  constexpr float tau=6.2831853f;
  const float phase=float(walk_ms%300)*tau/300.0f;
  const int bob=int(std::lround(0.55f+0.55f*std::cos(phase*2)));
  const int wag=int(std::lround(std::sin(phase)*1.4f));
  for (int leg=0; leg<4; ++leg) {
    const int base = (leg < 2 ? 11 : 23) + (leg % 2)*2;
    const float angle=phase + ((leg%2)^(leg/2))*tau/2;
    const int paw=base+int(std::lround(2.6f*std::sin(angle)));
    const int lift=int(std::lround(std::max(0.0f,std::cos(angle))*2.1f));
    d.drawWideLine(X(base), y+17+bob, X(paw), y+26-(leg % 2)-lift, 3,
                   leg % 2 ? coat : shade);
  }
  d.drawWideLine(X(8), y+13+bob, X(3), y+8+wag, 3, shade);
  d.drawWideLine(X(3), y+8+wag, X(2), y+4, 3, coat);
  rect(7,10+bob,20,10,4,coat);
  rect(20,12+bob,7,8,3,cream);
  tri(23,8+bob,22,0+bob,29,7+bob,shade);
  tri(29,7+bob,31,0+bob,35,10+bob,coat);
  tri(30,7+bob,31,3+bob,33,8+bob,inner);
  rect(23,7+bob,12,11,4,coat);
  rect(30,12+bob,7,5,2,cream);
  d.fillCircle(X(31),y+10+bob,1,face);
  d.fillCircle(X(36),y+13+bob,1,face);
}

void draw_mascots(bool full) {
  auto& d=M5.Display;
  const int left=(d.width()-mascot_band_w)/2;
  constexpr uint32_t cycle_ms=4600;
  constexpr float turn_span=0.035f;
  for(int i=0;i<2;++i) {
    const uint32_t phase_time=mascot_walk_ms+(i?1100:0);
    const uint32_t turn=phase_time%cycle_ms;
    const float half=float(cycle_ms/2);
    const float t=turn<cycle_ms/2 ? float(turn)/half : float(cycle_ms-turn)/half;
    // Nearly constant speed; ease only the last 3.5% near each turnaround.
    const float edge=std::min(t,1.0f-t);
    const float eased_edge=edge<turn_span ? (edge*edge)/(2.0f*turn_span) : edge-turn_span/2.0f;
    const float offset=(t<=0.5f?eased_edge:1.0f-turn_span-eased_edge)/(1.0f-turn_span);
    const int x=left+(i?180:14)+int(std::lround(69.0f*offset));
    if (!full && mascot_prev_x[i]>=0) {
      const int x0=std::min(x,mascot_prev_x[i])-1;
      const int x1=std::max(x,mascot_prev_x[i])+39;
      d.fillRect(x0,mascot_band_y,x1-x0,32,bg);
    }
    draw_dog(x,mascot_base_y,turn<cycle_ms/2,phase_time,i==1);
    mascot_prev_x[i]=x;
  }
}

void draw_button_help() {
  auto& d = M5.Display;
  d.setFont(&lgfx::fonts::lgfxJapanGothic_16);
  line(337, "赤：1回 起動・再起動／2回 電源オフ", 1, 0xfaaa);
  line(360, "黄：表示・切替／長押し：アラーム", 1, 0xffc8);
  line(383, "青：受信テスト 開始・終了", 1, 0x86ff);
  d.setFont(&lgfx::fonts::Font0);
}

void draw_power_banner() {
  auto& d = M5.Display;
  const int cx=d.width()/2;
  d.fillRect(cx-102,20,204,68,bg);
  d.fillRect(cx-102,396,204,45,bg);
  const bool clock_valid=strlen(clock_label)==19 && clock_label[4]=='/' && clock_label[10]==' ';
  char date[24];
  if (clock_valid) snprintf(date,sizeof(date),"%.10s",clock_label);
  else snprintf(date,sizeof(date),"日時未設定");
  d.setFont(&lgfx::fonts::lgfxJapanGothic_16);
  char battery[18], time_text[18], row[64];
  if (battery_percent < 0) snprintf(battery, sizeof(battery), "--%%");
  else snprintf(battery, sizeof(battery), "%d%%", battery_percent);
  if (endurance.active) {
    format_duration(time_text, sizeof(time_text), battery_run_seconds(elapsed_ms()));
    snprintf(row, sizeof(row), "電池 %s  計測 %s", battery, time_text);
  } else {
    snprintf(row, sizeof(row), "電池 %s  %s", battery,
             power_source == PowerSource::External ? "USB給電" : "判定中");
  }
  line(407, row, 1, battery_percent >= 0 && battery_percent <= 15 ? 0xfaaa : 0xaff5);
  if (endurance.last_valid) {
    format_duration(time_text, sizeof(time_text), endurance.last_seconds);
    snprintf(row, sizeof(row), "前回 %s%s", endurance.last_interrupted ? "約 " : "", time_text);
  } else snprintf(row, sizeof(row), "前回 --:--:--");
  line(429, endurance_can_save ? row : "記録保存なし", 1, muted);
  d.setFont(&lgfx::fonts::lgfxJapanGothicP_24);
  line(32, date, 1, muted);
  d.setFont(&lgfx::fonts::FreeSansBold24pt7b);
  line(65, clock_valid ? clock_label+11 : "--:--:--", 1, fg);
  d.setFont(&lgfx::fonts::Font0);
}

void draw_brand() {
  auto& d=M5.Display;
  const bool swap=d.getSwapBytes();
  d.setSwapBytes(true);
  d.pushImage(d.width()/2-25,88,50,50,system_kakari_mark);
  d.setSwapBytes(swap);
  d.setFont(&lgfx::fonts::lgfxJapanGothicP_24);
  line(149,"システム係",1,fg);
  d.setFont(&lgfx::fonts::Font0);
}

void draw_focus(const pics::Snapshot& snap, bool blink_on) {
  auto& d=M5.Display;
  const auto& r=snap.report;
  const auto sig=r.signal[selected_channel];
  const bool red=sig.state==pics::State::Red;
  const bool blink=sig.state==pics::State::BlinkGreen;
  const bool valid=selectable(sig.state);
  const int level=valid && sig.remaining_stage>=1 && sig.remaining_stage<=8
                 ? sig.remaining_stage : 0;
  const uint16_t accent=red ? 0xfb97 : valid ? 0x4f99 : muted;
  const uint16_t dark=0x21ca;
  constexpr int starts[]={127,156,185,214,303,332,1,30};
  for (uint8_t i=0; i<8; ++i) {
    const bool lit=i<level && (!blink || blink_on);
    d.fillArc(d.width()/2,d.height()/2,218,205,starts[i],starts[i]+24,lit?accent:dark);
  }
  d.fillRect((d.width()-300)/2,161,300,202,bg);
  d.setFont(&lgfx::fonts::lgfxJapanGothic_16);
  line(170,test_active ? "テスト・模擬データ" : "受信データ・観測用",1,test_active ? 0xfda0 : muted);
  d.setFont(&lgfx::fonts::lgfxJapanGothic_24);
  line(189,red ? "赤信号" : blink ? "青点滅" : valid ? "青信号" : "状態不明",1,accent);
  char number[4];
  if (level) snprintf(number,sizeof(number),"%d",level);
  else snprintf(number,sizeof(number),"-");
  d.setFont(&lgfx::fonts::FreeSansBold24pt7b);
  line(239,number,3,fg);
  d.setFont(&lgfx::fonts::lgfxJapanGothic_16);
  line(293,"残り段階  /  8（秒ではない）",1,muted);
  char label[40];
  if (test_active) snprintf(label,sizeof(label),"DEMO / CH %02u",unsigned(selected_channel+1));
  else snprintf(label,sizeof(label),"%08lX / CH %02u",
                (unsigned long)r.intersection,unsigned(selected_channel+1));
  d.setFont(&lgfx::fonts::Font0);
  line(320,label,2,fg);
  const int slots=r.count<=6 ? r.count : 6;
  for(int i=0; i<slots; ++i) {
    const int x=d.width()/2 + (i*22) - (slots-1)*11;
    d.fillCircle(x,348, i==selected_channel ? 5 : 4,
                 i==selected_channel ? accent : selectable(r.signal[i].state) ? muted : dark);
    if (i==selected_channel) d.drawCircle(x,348,8,fg);
  }
}

void draw(const pics::Snapshot& snap, bool blink_on=true) {
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(bg);
  draw_power_banner();
  draw_mascots(true);
  draw_brand();
  if (snap.status == pics::Snapshot::Status::Single) {
    draw_focus(snap,blink_on);
    d.setFont(&lgfx::fonts::lgfxJapanGothic_16);
    line(379,"黄：CH切替　青：受信テスト",1,0x86ff);
    d.setFont(&lgfx::fonts::Font0);
  } else {
    if (test_active) {
      d.setFont(&lgfx::fonts::lgfxJapanGothic_16);
      line(170,"テスト・模擬データ",1,0xfda0);
      d.setFont(&lgfx::fonts::Font0);
    }
    d.setFont(&lgfx::fonts::lgfxJapanGothic_24);
    line(215,snap.status==pics::Snapshot::Status::Multiple ? "複数の交差点" : "受信待ち",1,fg);
    d.setFont(&lgfx::fonts::lgfxJapanGothic_16);
    line(252,snap.status==pics::Snapshot::Status::Multiple ? "信号の個別表示を停止" : "新しいデータはありません",1,muted);
    draw_button_help();
  }
  d.endWrite();
}
}  // namespace

static void report_display() {
  Serial.printf("DISPLAY DIAG: board=%u display=%u candidate=%u fixed=%u size=%dx%d brightness=%u ms=%lu\n",
    unsigned(M5.getBoard()), unsigned(M5.Display.getBoard()), unsigned(M5.Display.getCandidateBoard()),
    unsigned(M5.Display.getFixedBoard()), M5.Display.width(), M5.Display.height(),
    unsigned(M5.Display.getBrightness()), (unsigned long)millis());
}
void setup() {
  Serial.begin(115200);
  // USB logging must not wait if macOS stops draining the serial stream.
  Serial.setTxTimeoutMs(0);
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(0);
  M5.Display.setBrightness(screen_brightness);
  M5.Display.setFont(&lgfx::fonts::Font0);
  report_display();
  if (gallery::photo_count) Serial.printf("PICS gallery ready photos=%d touch=%u zoom=4 rounded=1 pan=1\n", gallery::photo_count, unsigned(M5.Touch.isEnabled()));
  panel_ready = M5.Display.getBoard() == lgfx::board_M5StopWatch &&
                M5.Display.width() > 0 && M5.Display.height() > 0;
  if (panel_ready) {
    M5.Display.fillScreen(0xf81f);
    delay(750);
  }
  if (panel_ready) {
    load_endurance();
    load_alarm();
    if (read_clock()) Serial.printf("PICS clock retained %s JST\n",clock_label);
    update_battery();
    update_power_source(elapsed_ms());
  }
  draw(pics::Snapshot{});
  BLEDevice::init("");
  scanner = BLEDevice::getScan();
  scanner->setActiveScan(false);  // No broadcast, no scan requests.
  scanner->setInterval(160);
  scanner->setWindow(100);
  scanner->setAdvertisedDeviceCallbacks(&adv_cb, true, false);  // Raw bytes; no cached scan results.
  start_pending = true;
  screen_activity_at = millis();
}
void loop() {
  M5.update();
  const uint32_t now = millis();
  const uint64_t run_ms = elapsed_ms();
  if (panel_ready) {
    update_power_source(run_ms);
    poll_clock_command();
  }
  static uint32_t diag_at = 0;
  if ((int32_t)(now - diag_at) >= 0) {
    diag_at = now + 5000;
    Serial.printf("PICS screen=%s scans=%lu ms=%lu\n", screen_awake ? "on" : "off",
                  (unsigned long)scan_cycles, (unsigned long)now);
  }
  if (scan_done.exchange(false)) {
    ++scan_cycles;
    start_pending = true;
    start_retry = now;
  }
  if (start_pending && int32_t(now - start_retry) >= 0) {
    start_pending = !scanner->start(2, scanComplete, false);
    static bool logged_start = false;
    if (!start_pending && !logged_start) {
      Serial.println("PICS monitor: passive BLE scan started");
      logged_start = true;
    }
    start_retry = now + 1000;
  }
  bool force_redraw = panel_ready && alarm_tick(now);
  bool alarm_ui_dirty = force_redraw;
  // A short yellow press keeps the old action. Hold it to open settings.
  if (M5.BtnA.wasPressed()) {
    alarm_ignore_yellow_release = !screen_awake || alarm_ringing;
    if (!screen_awake && panel_ready) {
      M5.Display.setBrightness(screen_brightness);
      screen_awake = true;
      force_redraw = true;
      screen_activity_at = now;
      Serial.printf("PICS yellow wake scans=%lu ms=%lu\n",
                    (unsigned long)scan_cycles, (unsigned long)now);
    }
  }
  if (panel_ready && screen_awake && !alarm_ringing && !alarm_setting_open &&
      M5.BtnA.pressedFor(alarm_hold_ms)) {
    // The menu state prevents repeated opening while the button is held.
    alarm_ignore_yellow_release = true;
    alarm_setting_open = true;
    alarm_ignore_blue_release = false;
    alarm_edit_minutes = daily_alarm::minutes_of(alarm_setting);
    alarm_edit_on = bool(alarm_setting & daily_alarm::enabled_bit);
    alarm_save_error = false;
    if (test_active) end_test();
    photo_page = 0;
    reset_gallery_gesture();
    alarm_ui_dirty = true;
    Serial.println("PICS alarm menu");
  }
  bool swallow_blue = false, swallow_yellow = false;
  if (alarm_ringing || alarm_setting_open) {
    const bool was_ringing = alarm_ringing;
    if (alarm_click(now)) alarm_ui_dirty = true;
    if (was_ringing) {
      swallow_blue = M5.BtnB.wasPressed() || M5.BtnB.wasReleased();
      swallow_yellow = M5.BtnA.wasPressed() || M5.BtnA.wasReleased();
      if (M5.BtnB.wasPressed()) alarm_ignore_blue_release = true;
      if (M5.BtnB.wasReleased()) alarm_ignore_blue_release = false;
      if (alarm_ringing && (M5.BtnB.wasPressed() || M5.BtnA.wasPressed())) {
        end_alarm(now);
        alarm_ui_dirty = true;
      }
    } else if (alarm_setting_open) {
      if (!alarm_ringing && !alarm_ignore_blue_release && M5.BtnB.pressedFor(700)) {
        alarm_ignore_blue_release = true;
        Serial.println("PICS alarm preview button");
        begin_alarm(now,true);
        alarm_ui_dirty = true;
      }
      if (M5.BtnB.wasReleased()) {
        const bool ignore = alarm_ignore_blue_release;
        alarm_ignore_blue_release = false;
        swallow_blue = true;
        if (!ignore && !alarm_ringing) {
          if (save_alarm()) alarm_setting_open = false;
          alarm_ui_dirty = true;
        }
      } else if (M5.BtnA.wasReleased() && !alarm_ignore_yellow_release) {
        swallow_yellow = true;
        alarm_setting_open = false;
        alarm_ui_dirty = true;
      }
    }
    if (alarm_setting_open || alarm_ringing) {
      if (alarm_ui_dirty) draw_alarm();
      if (M5.BtnA.wasReleased()) alarm_ignore_yellow_release = false;
      delay(5);
      return;
    }
    screen_activity_at = now;
    force_redraw = true;
  }
  if (gallery::photo_count && panel_ready && M5.Touch.isEnabled()) {
    const auto g = gallery_gesture(now);
    if (screen_awake && !test_active && photo_page && photo_scale > 1 && (g.dx || g.dy || g.drag_end)) {
      screen_activity_at = now;
      first_tap = false;
      if (g.dx || g.dy) pan_pending = move_zoom_view(g.dx, g.dy) || pan_pending;
      if (g.drag_end) {
        Serial.printf("PICS gallery pan page=%d zoom=%d x=%d y=%d cached=%u\n",
                      photo_page, photo_scale, photo_view_x, photo_view_y, unsigned(cached_success));
      }
    } else if (screen_awake && !test_active && g.direction) {
      reset_gallery_gesture();
      photo_page = gallery::next_page(photo_page, g.direction, gallery::photo_count);
      screen_activity_at = now;
      force_redraw = true;
      if (!photo_page) Serial.println("PICS gallery home");
    } else if (screen_awake && !test_active && photo_page && g.tap) {
      screen_activity_at = now;
      force_redraw = apply_gallery_tap(g, now);
    }
  }
  if (M5.BtnA.wasReleased()) {
    const bool ignore = alarm_ignore_yellow_release || swallow_yellow;
    alarm_ignore_yellow_release = false;
    if (!ignore) {
      screen_activity_at = now;
      if (screen_awake && photo_page) {
        photo_page = 0;
        reset_gallery_gesture();
        force_redraw = true;
        Serial.println("PICS gallery home");
      } else if (screen_awake && focused) cycle_channel=true;
    }
  }
  if (panel_ready && !swallow_blue && M5.BtnB.wasPressed()) {
    photo_page = 0;
    reset_gallery_gesture();
    screen_activity_at=now;
    if (!screen_awake) {
      M5.Display.setBrightness(screen_brightness);
      screen_awake=true;
    }
    if (test_active) end_test();
    else {
      test_receiver.reset();
      test_active=true;
      test_started_at=now;
      test_step=-1;
      Serial.println("PICS TEST begin");
    }
    force_redraw=true;
  }
  update_test(now);
  if (panel_ready && screen_awake && !test_active &&
      uint32_t(now - screen_activity_at) >= screen_timeout_ms) {
    // Show black before dimming the AMOLED. Keep the panel controller
    // awake so the next button press need not exit hardware sleep.
    first_tap = false;
    M5.Display.fillScreen(0x0000);
    M5.Display.setBrightness(0);
    screen_awake = false;
    Serial.printf("PICS auto display off scans=%lu ms=%lu\n",
                  (unsigned long)scan_cycles, (unsigned long)now);
  }
  bool banner_dirty=false;
  if (panel_ready && screen_awake &&
      (force_redraw || uint32_t(millis() - battery_sample_at) >= 10000))
    banner_dirty=update_battery();
  if (photo_page) {
    // Skip the normal screen's painters, but keep scanning and accounting.
    if (screen_awake && force_redraw) {
      draw_gallery_photo(photo_page);
      pan_pending = false;
      last_pan_paint = now;
    } else if (screen_awake && photo_scale > 1 && pan_pending &&
               (now - last_pan_paint >= (cached_success ? 28u : 140u))) {
      draw_zoom_view(gallery::photos[photo_page-1]);
      pan_pending = false;
      last_pan_paint = now;
    }
    mascot_previous_ms=0;
    mascot_stats_at=mascot_stats_frames=mascot_stats_max=mascot_stats_slow=0;
    delay(5);
    return;
  }
  static uint32_t next_frame=0;
  if (screen_awake && (force_redraw || int32_t(now-next_frame)>=0)) {
    next_frame=now+80;
    pics::Snapshot s;
    if (test_active) s=test_receiver.view(now);
    else {
      portENTER_CRITICAL(&rx_lock);
      s=receiver.view(millis());
      portEXIT_CRITICAL(&rx_lock);
    }
    static pics::Snapshot::Status shown_status=pics::Snapshot::Status::Unknown;
    static bool shown_test=false, shown_blink=true;
    static uint8_t shown_channel=0, shown_count=0;
    static pics::State shown_state=pics::State::Unknown;
    static int8_t shown_level=-1;
    static uint8_t shown_usable=0;
    const bool single=s.status==pics::Snapshot::Status::Single && s.report.count>0;
    const bool source_changed=single && (!focused || focused_id!=s.report.intersection ||
                                         focused_demo!=test_active);
    if (single) {
      if (source_changed) selected_channel=pick_channel(s.report,0);
      else if (cycle_channel) selected_channel=pick_channel(s.report,selected_channel+1);
      else if (selected_channel>=s.report.count || !selectable(s.report.signal[selected_channel].state))
        selected_channel=pick_channel(s.report,selected_channel);
      focused_id=s.report.intersection;
      focused_demo=test_active;
    }
    focused=single;
    cycle_channel=false;
    const auto sig=single?s.report.signal[selected_channel]:pics::Signal{};
    uint8_t usable=0;
    if (single) for (uint8_t i=0;i<s.report.count && i<6;++i)
      if (selectable(s.report.signal[i].state)) usable|=(1u<<i);
    const bool blink=sig.state!=pics::State::BlinkGreen || ((now/425)%2==0);
    const bool new_layout=force_redraw || source_changed ||
                          s.status!=shown_status || test_active!=shown_test;
    if (new_layout) draw(s,blink);
    else if (single && (selected_channel!=shown_channel || s.report.count!=shown_count ||
             sig.state!=shown_state || sig.remaining_stage!=shown_level || usable!=shown_usable ||
             blink!=shown_blink)) {
      M5.Display.startWrite();
      draw_focus(s,blink);
      M5.Display.endWrite();
    }
    if (single && (source_changed || sig.state!=shown_state || sig.remaining_stage!=shown_level)) {
      Serial.printf("PICS DIAL mode=%s ch=%u state=%u level=%d\n",
         test_active?"test":"live",unsigned(selected_channel+1),unsigned(sig.state),
         int(sig.remaining_stage));
    }
    if (!single && shown_status==pics::Snapshot::Status::Single)
      Serial.printf("PICS DIAL mode=%s no_fresh_signal\n",test_active?"test":"live");
    shown_status=s.status; shown_test=test_active; shown_channel=selected_channel;
    shown_state=sig.state; shown_level=sig.remaining_stage; shown_blink=blink;
    shown_count=single?s.report.count:0;shown_usable=usable;
  }
  static uint64_t banner_second = UINT64_MAX;
  if (panel_ready && screen_awake && (force_redraw || banner_dirty || banner_second != run_ms / 1000)) {
    banner_second = run_ms / 1000;
    read_clock();
    M5.Display.startWrite();
    draw_power_banner();
    M5.Display.endWrite();
  }
  if (panel_ready && screen_awake) {
    const uint32_t frame_now=millis();
    if (uint32_t(frame_now-mascot_at)>=mascot_frame_ms) {
      const uint32_t gap=mascot_previous_ms?frame_now-mascot_previous_ms:mascot_frame_ms;
      mascot_at=frame_now;
      mascot_previous_ms=frame_now;
      mascot_walk_ms+=gap*(test_active?5:4)/4;
      M5.Display.startWrite();
      draw_mascots(false);
      M5.Display.endWrite();
      if (!mascot_stats_at) mascot_stats_at=frame_now;
      ++mascot_stats_frames;
      if (gap>70) ++mascot_stats_slow;
      mascot_stats_max=std::max(mascot_stats_max,gap);
      if (uint32_t(frame_now-mascot_stats_at)>=5000 && mascot_stats_frames>=5) {
        if (Serial.availableForWrite()>=104)
          Serial.printf("PICS ANIM mode=%s frames=%lu max_gap_ms=%lu over70=%lu\n",
              test_active?"test":"live",(unsigned long)mascot_stats_frames,
              (unsigned long)mascot_stats_max,(unsigned long)mascot_stats_slow);
        mascot_stats_at=frame_now;
        mascot_stats_frames=mascot_stats_max=mascot_stats_slow=0;
      }
    }
  } else {
    mascot_previous_ms=0;
    mascot_stats_at=mascot_stats_frames=mascot_stats_max=mascot_stats_slow=0;
  }
  delay(5);
}
