// Passive PICS manufacturer advertisements. Research-only, not a crossing aid.
#include <M5Unified.h>
#include <Preferences.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include "red_button.h"
#include "oled_rail_trial.h"
#include "oled_auto_record.h"
#include "touch_button.h"
#include "ui_fonts.h"
#include "signal_mask.h"
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
#include "local_weather.h"

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
constexpr uint32_t test_stage_ms = 1000;
constexpr int test_stage_count = 19;
constexpr uint32_t test_duration_ms = test_stage_ms * test_stage_count;
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
  int drawer = 0;  // 1: bottom swipe up, -1: swipe down.
};

GalleryGesture gallery_gesture(uint32_t now) {
  static bool tracking = false;
  static uint16_t touch_id = 0;
  static uint32_t started_at = 0;
  static int last_x = 0, last_y = 0, origin_x=0, origin_y=0;
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
    last_x = origin_x = touch.x;
    last_y = origin_y = touch.y;
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
          touch.x, touch.y, 0, 0, false,
          gallery::drawer_swipe(dx,dy,origin_x,origin_y,elapsed,
                                M5.Display.width(),M5.Display.height())};
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
    if (photo_scale == 1 || photo_scale == 2) release_zoom_cache();
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
  // 8x reuses the 4x decode and scales only the visible area. An 8x
  // full-image RGB565 sprite could exhaust the available PSRAM.
  constexpr int decode_scale = 4;
  const float ratio = float(photo_scale) / decode_scale;
  const auto* jpeg = p.zoom4_jpeg;
  const size_t bytes = p.zoom4_bytes;
  if (cached_page != photo_page || cached_scale != decode_scale) {
    release_zoom_cache();
    cached_page = photo_page;
    cached_scale = decode_scale;
    zoom_sprite.setPsram(true);
    zoom_sprite.setColorDepth(16);
    if (zoom_sprite.createSprite(p.width * decode_scale, p.height * decode_scale)) {
      zoom_sprite.setPivot(0, 0);
      cached_success = zoom_sprite.drawJpg(jpeg, bytes, 0, 0);
      if (!cached_success) zoom_sprite.deleteSprite();
    }
  }
  d.setClipRect(0, 0, d.width(), photo_view_height());
  const int x = photo_view_x & ~1, y = photo_view_y & ~1;
  bool ok = true;
  if (cached_success && ratio == 1) zoom_sprite.pushSprite(&d, x, y);
  else if (cached_success) zoom_sprite.pushRotateZoom(&d, x, y, 0, ratio, ratio);
  else ok = d.drawJpg(jpeg, bytes, x, y, 0, 0, 0, 0, float(ratio), float(ratio));
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
  d.setFont(ui_fonts::body.get());
  d.setTextSize(1);
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
  p[15]=(level<<4)|(state==1 ? 3 : 1);
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
  const int step=int(age/test_stage_ms);
  if (step>=test_stage_count || step==test_step) return false;
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
constexpr uint32_t scan_slice_seconds = 2;
bool receiving = false;
bool scan_in_flight = false;
bool live_blue_held = false;
uint32_t start_retry = 0;
uint32_t scan_cycles = 0;
uint32_t screen_activity_at = 0;
bool screen_awake = true;
bool panel_ready = false;
bool oled_auto_active=false;
bool oled_auto_blocked=false;
void save_oled_probe();
void save_oled_auto();
bool wake_screen() {
  if (oled_rail_trial::inactive()) {
    if (!oled_rail_trial::wake(screen_brightness)) {
      if(oled_auto_active) {
        oled_auto_blocked=true;
        Serial.printf("PICS OLED auto wake failed error=%u\n",unsigned(oled_rail_trial::probe.error));
        save_oled_auto();
      }
      save_oled_probe(); return false;
    }
    if(oled_auto_active) {
      Serial.printf("PICS OLED auto wake ok=%lu ms\n",(unsigned long)oled_rail_trial::probe.wake_ms);
      if (oled_rail_trial::probe.error==oled_rail_trial::WakeFailed)
        oled_rail_trial::probe.error=oled_rail_trial::NoError;
      save_oled_auto();
      oled_auto_active=false;
    }
    save_oled_probe();
  }
  M5.Display.setBrightness(screen_brightness);
  screen_awake=true;
  return true;
}
int battery_percent = -1;
uint32_t battery_sample_at = 0;

enum class PowerSource : uint8_t { Unknown, Battery, External };
constexpr uint32_t endurance_tag = 0x45525431;
struct LegacyEndurance {
  uint32_t tag = endurance_tag;
  uint32_t last_seconds = 0;
  uint32_t checkpoint_seconds = 0;
  uint8_t last_valid = 0;
  uint8_t last_interrupted = 0;
  uint8_t active = 0;
  uint8_t reserved = 0;
};
static_assert(sizeof(LegacyEndurance)==16);
struct Activity {
  uint32_t screen=0, ble=0, wifi=0, speaker=0, idle=0, led=0;
  bool sample_seen=false;
};
struct EnduranceRecord : LegacyEndurance {
  Activity live{}, last{};
};
Preferences endurance_store;
EnduranceRecord endurance;
bool endurance_can_save = false;
uint32_t oled_attempts=0;
uint32_t oled_auto_sequence=0;
uint64_t oled_auto_last=0;
bool oled_auto_usb=false;
void log_oled_auto(uint64_t raw) {
  if(!oled_auto_record::valid(raw)) return;
  const auto d=oled_auto_record::decode(raw);
  Serial.printf("PICS OLED auto record seq=%lu off=%u wake=%u rail_ms=%lu wake_ms=%lu error=%u supply=%s\n",
      (unsigned long)d.seq,unsigned(bool(d.flags&2)),unsigned(bool(d.flags&4)),
      (unsigned long)d.rail_ms,(unsigned long)d.wake_ms,unsigned(d.error),d.usb?"usb":"battery");
}
void save_oled_auto() {
  if(!oled_auto_sequence) return;
  const auto& p=oled_rail_trial::probe;
  const uint64_t raw=oled_auto_record::encode({oled_auto_sequence,p.rail_ms,p.wake_ms,p.flags,p.error,oled_auto_usb});
  if(raw!=oled_auto_last) {
    if(endurance_can_save && endurance_store.putULong64("oled_auto1",raw)==sizeof(raw))
      oled_auto_last=raw;
    else Serial.println("PICS OLED auto record storage unavailable");
  }
  log_oled_auto(raw);
}
void load_oled_auto() {
  if(!endurance_can_save) return;
  const uint64_t raw=endurance_store.getULong64("oled_auto1",0);
  if(!oled_auto_record::valid(raw)) return;
  oled_auto_last=raw;
  oled_auto_sequence=oled_auto_record::decode(raw).seq;
  log_oled_auto(raw);
}
void log_oled_probe(const oled_rail_trial::Probe& p) {
  if(p.tag!=oled_rail_trial::probe_tag || !p.attempt) return;
  Serial.printf("PICS OLED detail seq=%lu button=%u error=%u latch=%02x flags=%u blank=%lu rail=%lu wake=%lu\n",
      (unsigned long)p.attempt,unsigned(p.button),unsigned(p.error),unsigned(p.latch),
      unsigned(p.flags),(unsigned long)p.blank_ms,(unsigned long)p.rail_ms,(unsigned long)p.wake_ms);
}
void save_oled_probe() {
  const auto& p=oled_rail_trial::probe;
  if(p.attempt && endurance_can_save) endurance_store.putBytes("oled_detail2",&p,sizeof(p));
  log_oled_probe(p);
}
uint64_t battery_started_ms = 0;
uint32_t battery_before_boot = 0;
bool endurance_resume_pending = false, endurance_approx = false;
RedButton red_button;
bool red_policy_ready = false;
PowerSource red_policy_source = PowerSource::Unknown;
uint32_t next_red_poll = 0;
uint64_t usage_sample_ms=0;
bool usage_view=false, help_view=false;
bool led_enabled=true;
PowerSource led_policy_source=PowerSource::Unknown;
PowerSource power_source = PowerSource::Unknown;
PowerSource source_candidate = PowerSource::Unknown;
uint8_t source_votes = 0;
uint64_t source_candidate_ms = 0;
uint64_t next_source_sample_ms = 0;
bool clock_set_jst = false;
constexpr uint8_t rx8130_address = 0x32, rx8130_flag = 0x1D;
constexpr uint32_t rtc_i2c_hz = 100000;
char clock_label[34] = "日時未設定（JST）";
constexpr const char* weekdays[]={"日","月","火","水","木","金","土"};
constexpr int weekday_of(int y,int m,int d) {
  constexpr int offset[]={0,3,2,5,0,3,5,1,4,6,2,4};
  if (m<1 || m>12 || d<1 || d>31) return -1;
  if (m<3) --y;
  return (y+y/4-y/100+y/400+offset[m-1]+d)%7;
}
static_assert(weekday_of(2026,11,27)==5 && weekday_of(2028,2,29)==2 &&
              weekday_of(2027,1,1)==5 && weekday_of(2026,0,1)==-1);
int clock_weekday=-1;
bool read_clock() {
  m5::rtc_datetime_t dt;
  clock_weekday=-1;
  uint8_t flags = 0;
  if (!clock_set_jst || !M5.Rtc.isEnabled() ||
      !M5.In_I2C.readRegister(rx8130_address, rx8130_flag, &flags, 1, rtc_i2c_hz) ||
      (flags & 0x02) || !M5.Rtc.getDateTime(&dt) ||
      dt.date.year < 2025 || dt.date.year > 2099) {
    snprintf(clock_label, sizeof(clock_label), "日時未設定（JST）");
    return false;
  }
  clock_weekday=weekday_of(dt.date.year,dt.date.month,dt.date.date);
  snprintf(clock_label, sizeof(clock_label), "%04d/%02d/%02d %02d:%02d:%02d",
      int(dt.date.year),int(dt.date.month),int(dt.date.date),int(dt.time.hours),
      int(dt.time.minutes),int(dt.time.seconds));
  return true;
}

wifi_profiles::Book wifi_book;
int wifi_preferred=-1;
bool have_wifi() { return wifi_profiles::count(wifi_book)>0; }
bool wifi_busy() { return local_weather::read().status==local_weather::Result::Busy; }
local_weather::Result last_home_weather;
bool weather_view=false, weather_clock_applied=true;
local_weather::Result::Status weather_drawn=local_weather::Result::Idle;

bool save_clock_epoch(uint64_t epoch) {
  if (epoch<1735689600ULL || epoch>=4102416000ULL) return false;
  const time_t in_jst=time_t(epoch+9*3600);
  tm value={};
  if (!gmtime_r(&in_jst,&value) || !M5.Rtc.setDateTime(&value) ||
      !M5.In_I2C.writeRegister8(rx8130_address,rx8130_flag,0xBD,rtc_i2c_hz))
    return false;
  clock_set_jst=true;
  if (endurance_can_save) endurance_store.putBool("clock_jst",true);
  return read_clock();
}
int hex_digit(char c) {
  if (c>='0' && c<='9') return c-'0';
  if (c>='a' && c<='f') return c-'a'+10;
  if (c>='A' && c<='F') return c-'A'+10;
  return -1;
}
bool decode_hex(const char* in, char* out, size_t cap, size_t minimum) {
  const size_t n=strlen(in);
  if (n%2 || n/2>=cap || n/2<minimum) return false;
  for (size_t i=0;i<n/2;++i) {
    const int hi=hex_digit(in[2*i]), lo=hex_digit(in[2*i+1]);
    if (hi<0 || lo<0 || (hi==0 && lo==0)) return false;
    out[i]=char(hi*16+lo);
  }
  out[n/2]=0;
  return true;
}
bool save_wifi_book(const wifi_profiles::Book& book) {
  if (!endurance_can_save || wifi_busy() || local_weather::scanning() ||
      !wifi_profiles::valid_book(book) ||
      endurance_store.putBytes("wifi_book1",&book,sizeof(book))!=sizeof(book)) return false;
  wifi_book=book;
  if (endurance_store.isKey("wifi_cfg")) endurance_store.remove("wifi_cfg");
  wifi_preferred=-1;
  last_home_weather={};
  local_weather::publish(local_weather::Result{});
  weather_clock_applied=true;
  return true;
}
bool configure_network(const char* text) {
  if (!endurance_can_save || local_weather::read().status==local_weather::Result::Busy) return false;
  int lat=0,lon=0,used=0;
  char ssid[65]{}, pass[127]{}, city[65]{};
  const bool parsed=sscanf(text,"%d %d %64s %126s %64s%n",&lat,&lon,ssid,pass,city,&used)==5;
  local_weather::Settings cfg;
  const bool good=parsed && !text[used] && lat>=-9000 && lat<=9000 &&
      lon>=-18000 && lon<=18000 && decode_hex(ssid,cfg.ssid,sizeof(cfg.ssid),1) &&
      decode_hex(pass,cfg.password,sizeof(cfg.password),8) &&
      decode_hex(city,cfg.label,sizeof(cfg.label),1);
  if (good) { cfg.lat=lat; cfg.lon=lon; }
  auto next=wifi_book;
  int index=wifi_profiles::find(next,cfg.ssid);
  if(index<0) index=wifi_profiles::empty(next);
  const bool saved=good && wifi_profiles::set(next,index,{cfg,0}) && save_wifi_book(next);
  wifi_profiles::wipe(pass,sizeof(pass));
  wifi_profiles::wipe(cfg.password,sizeof(cfg.password));
  wifi_profiles::wipe(&next,sizeof(next));
  return saved;
}
const char* wifi_radio_state() {
  wifi_mode_t mode=WIFI_MODE_NULL;
  const auto status=esp_wifi_get_mode(&mode);
  if(status==ESP_ERR_WIFI_NOT_INIT) return "off";
  if(status!=ESP_OK) return "unknown";
  return mode==WIFI_MODE_NULL ? "off" : mode==WIFI_MODE_STA ? "sta" : "other";
}
const char* source_name(PowerSource source) {
  return source==PowerSource::External ? "usb" :
         source==PowerSource::Battery ? "battery" : "unknown";
}
bool start_weather(const char* reason) {
  if (!local_weather::start(wifi_book,wifi_preferred)) return false;
  if (wifi_profiles::count(wifi_book)>1) last_home_weather={};
  weather_clock_applied=false;
  Serial.printf("PICS network update started reason=%s\n",reason);
  return true;
}
void poll_weather() {
  if (weather_clock_applied) return;
  const auto r=local_weather::read();
  if (r.status==local_weather::Result::Busy) return;
  weather_clock_applied=true;
  if (r.profile>=0 && r.epoch) wifi_preferred=r.profile;
  if (r.status==local_weather::Result::Ready) {
    last_home_weather=r;
    Serial.printf("PICS home weather ready code=%d temp10=%d\n",r.code,r.temperature_tenths);
    if (r.next_code>=0)
      Serial.printf("PICS tomorrow date=%s code=%d min10=%d max10=%d\n",
          r.forecast_date,r.next_code,r.next_min_tenths,r.next_max_tenths);
  }
  if (r.epoch) {
    const time_t now=time(nullptr);
    if (now>=r.epoch && now-r.epoch<120 && save_clock_epoch(uint64_t(now)))
      Serial.printf("PICS clock network set %s JST\n",clock_label);
    else Serial.println("PICS clock network set failed");
  }
  Serial.printf("PICS network status=%u weather=%u\n",unsigned(r.status),
                unsigned(r.status==local_weather::Result::Ready));
}

void poll_clock_command() {
  static char line[296];
  static uint16_t len = 0;
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
        if (digits && save_clock_epoch(epoch)) Serial.printf("PICS clock set %s JST\n",clock_label);
        else Serial.println("PICS clock set failed");
      } else if (!overflow && !memcmp(line,"NETCFG ",7) && len>7 &&
                 power_source==PowerSource::External) {
        if (configure_network(line+7)) {
          Serial.println("PICS network configuration saved");
          start_weather("usb-config");
        } else Serial.println("PICS network configuration rejected");
      } else if (!overflow && len==9 && !memcmp(line,"NETFORGET",9) &&
                 power_source==PowerSource::External && endurance_can_save &&
                 local_weather::read().status!=local_weather::Result::Busy) {
        const bool ok=save_wifi_book(wifi_profiles::Book{});
        Serial.println(ok ? "PICS network configuration removed" : "PICS network configuration rejected");
      }
      memset(line,0,len+1);
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
    // Local device setup requested by the owner. Later menu choices persist.
    if (!endurance_store.getBool("alarm7max_v1",false)) {
      if (endurance_store.putUShort("alarm_cfg",alarm_setting) == sizeof(alarm_setting)) {
        endurance_store.putBool("alarm7max_v1",true);
      } else Serial.println("PICS alarm provision failed");
    }
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
  wake_screen();
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
  const bool ok = endurance_store.putBytes("record_v2", &endurance, sizeof(endurance))
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
  if (endurance_store.getBytesLength("record_v2")==sizeof(endurance)) {
    EnduranceRecord loaded;
    if (endurance_store.getBytes("record_v2",&loaded,sizeof(loaded))==sizeof(loaded) &&
        loaded.tag==endurance_tag) endurance=loaded;
  } else if (endurance_store.getBytesLength("record")==sizeof(LegacyEndurance)) {
    LegacyEndurance loaded;
    if (endurance_store.getBytes("record",&loaded,sizeof(loaded))==sizeof(loaded) &&
        loaded.tag==endurance_tag) static_cast<LegacyEndurance&>(endurance)=loaded;
  }
  endurance_resume_pending=endurance.active;
  if (endurance_resume_pending) {
    battery_before_boot=endurance.checkpoint_seconds;
    endurance_approx=(endurance.reserved!=1);
  }
  clock_set_jst = endurance_store.getBool("clock_jst", false);
  if (endurance_store.isKey("wifi_book1")) {
    wifi_profiles::Book loaded;
    if (endurance_store.getBytesLength("wifi_book1")==sizeof(loaded) &&
        endurance_store.getBytes("wifi_book1",&loaded,sizeof(loaded))==sizeof(loaded) &&
        wifi_profiles::valid_book(loaded)) wifi_book=loaded;
    wifi_profiles::wipe(&loaded,sizeof(loaded));
  } else {
    local_weather::Settings old;
    if (endurance_store.getBytesLength("wifi_cfg")==sizeof(old) &&
        endurance_store.getBytes("wifi_cfg",&old,sizeof(old))==sizeof(old))
      wifi_profiles::set(wifi_book,0,{old,0});
    wifi_profiles::wipe(&old,sizeof(old));
  }
  Serial.printf("PICS endurance ready last_valid=%u last_s=%lu interrupted=%u\n",
      unsigned(endurance.last_valid), (unsigned long)endurance.last_seconds,
      unsigned(endurance.last_interrupted));
}

uint32_t battery_run_seconds(uint64_t now) {
  if (!endurance.active) return 0;
  if (endurance_resume_pending || now<battery_started_ms) return battery_before_boot;
  return battery_before_boot+uint32_t((now-battery_started_ms)/1000);
}

int read_vin_mv() {
  uint8_t raw[2];
  return M5.Power.M5pm1.readRegister(0x24,raw,sizeof(raw))
           ? int(raw[0])+(int(raw[1])<<8) : -1;
}
PowerSource classify_vin(int mv) {
  if (mv>=4200 && mv<6000) return PowerSource::External;
  if (mv>=0 && mv<=2000) return PowerSource::Battery;
  return PowerSource::Unknown;
}
void finish_battery_run(uint64_t now,bool approximate) {
  if (!endurance.active) return;
  endurance.last_seconds=battery_run_seconds(now);
  endurance.last=endurance.live;
  endurance.live={};
  endurance.last_valid=true;
  endurance.last_interrupted=approximate;
  endurance.active=false;
  endurance.checkpoint_seconds=0;
  endurance.reserved=0;
  endurance_resume_pending=false;
  battery_before_boot=0;
  save_endurance();
  Serial.printf("PICS endurance finish last_s=%lu interrupted=%u\n",
                (unsigned long)endurance.last_seconds,unsigned(approximate));
  if (endurance.last.sample_seen) {
    const auto& a=endurance.last;
    Serial.printf("PICS usage ms screen=%lu ble=%lu wifi=%lu audio=%lu idle=%lu led=%lu\n",
        (unsigned long)a.screen,(unsigned long)a.ble,(unsigned long)a.wifi,
        (unsigned long)a.speaker,(unsigned long)a.idle,(unsigned long)a.led);
  }
}
// Official M5PM1 BTN_CFG_1 bit 0 disables short reset, BTN_CFG_2 bit 0
// disables double-click off. Never modify BTN_CFG_1 bit 7 (download lock).
bool apply_red_policy(bool battery) {
  auto& pm=M5.Power.M5pm1;
  constexpr uint8_t cfg1=0x49,cfg2=0x4a,mask=1;
  uint8_t old1=0,old2=0,got1=0,got2=0;
  if (!pm.readRegister(cfg1,&old1,1) || !pm.readRegister(cfg2,&old2,1)) return false;
  const uint8_t new1=(old1&~mask)|(battery?mask:0);
  const uint8_t new2=(old2&~mask)|(battery?mask:0);
  if ((new1==old1 || pm.writeRegister8(cfg1,new1)) &&
      (new2==old2 || pm.writeRegister8(cfg2,new2)) &&
      pm.readRegister(cfg1,&got1,1) && pm.readRegister(cfg2,&got2,1) &&
      got1==new1 && got2==new2) return true;
  if (new2!=old2) pm.writeRegister8(cfg2,old2);
  if (new1!=old1) pm.writeRegister8(cfg1,old1);
  return false;
}
void poll_red_button(uint32_t now,uint64_t run_ms) {
  if (!red_policy_ready || red_policy_source!=PowerSource::Battery ||
      power_source!=PowerSource::Battery || int32_t(now-next_red_poll)<0) return;
  next_red_poll=now+20;
  uint8_t raw=0;
  if (!M5.Power.M5pm1.readRegister(0x48,&raw,1)) { red_button.reset(); return; }
  const auto action=red_button.sample(raw&1,now);
  if (action==RedButton::None || classify_vin(read_vin_mv())!=PowerSource::Battery) return;
  if (action==RedButton::ShutDown) {
    const auto prior=endurance;
    const auto prior_offset=battery_before_boot;
    const bool approx=endurance_approx;
    finish_battery_run(run_ms,approx);
    Serial.println("PICS red shutdown");
    if (!M5.Power.M5pm1.powerOff()) {
      endurance=prior;
      // Continue the old session if the shutdown command was rejected.
      battery_before_boot=prior_offset;
      endurance_resume_pending=false;
      endurance_approx=approx;
      save_endurance();
      Serial.println("PICS red shutdown failed");
    }
  } else {
    if (endurance.active) {
      endurance.checkpoint_seconds=battery_run_seconds(run_ms);
      endurance.reserved=endurance_approx ? 0 : 1;
      save_endurance();
    }
    Serial.printf("PICS red reboot checkpoint_s=%lu\n",(unsigned long)endurance.checkpoint_seconds);
    esp_restart();
  }
}

void sample_activity(uint64_t now) {
  if (!usage_sample_ms) { usage_sample_ms=now; return; }
  if (now-usage_sample_ms<100) return;
  const uint32_t dt=uint32_t(std::min<uint64_t>(now-usage_sample_ms,1000));
  usage_sample_ms=now;
  if (!dt || power_source!=PowerSource::Battery || !endurance.active || endurance_resume_pending) return;
  const bool wifi=wifi_busy() || local_weather::scanning();
  const bool audio=test_audio_ready || alarm_audio;
  auto& live=endurance.live;
  live.sample_seen=true;
  if (screen_awake) live.screen+=dt;
  if (scan_in_flight) live.ble+=dt;
  if (wifi) live.wifi+=dt;
  if (audio) live.speaker+=dt;
  if (led_enabled) live.led+=dt;
  if (!screen_awake && !scan_in_flight && !wifi && !audio) live.idle+=dt;
}

void update_power_source(uint64_t now) {
  if (now < next_source_sample_ms) return;
  next_source_sample_ms=now+1000;
  const int vin_mv=read_vin_mv();
  const auto measured=classify_vin(vin_mv);
  if (measured==PowerSource::Unknown || measured==power_source) {
    source_candidate=PowerSource::Unknown;
    source_votes=0;
  } else {
    if (measured!=source_candidate) {
      source_candidate=measured;
      source_candidate_ms=now;
      source_votes=1;
    } else if (++source_votes>=3) {
      power_source=measured;
      source_votes=0;
      if (power_source==PowerSource::Battery) {
        battery_started_ms=source_candidate_ms;
        if (!endurance_resume_pending) {
          battery_before_boot=0;
          endurance_approx=false;
          endurance.checkpoint_seconds=0;
          endurance.live={};
        }
        endurance.active=true;
        endurance.reserved=0;
        endurance_resume_pending=false;
        save_endurance();
        Serial.printf("PICS endurance battery vin=%d resumed_s=%lu\n",vin_mv,(unsigned long)battery_before_boot);
      } else if (endurance.active) {
        finish_battery_run(source_candidate_ms,endurance_resume_pending || endurance_approx);
      } else {
        Serial.printf("PICS endurance USB vin=%d\n",vin_mv);
      }
    }
  }
  if (power_source!=PowerSource::Unknown && measured==power_source &&
      (!red_policy_ready || red_policy_source!=power_source)) {
    red_button.reset();
    red_policy_ready=apply_red_policy(power_source==PowerSource::Battery);
    red_policy_source=power_source;
    Serial.printf("PICS red policy battery=%u verified=%u\n",
                  unsigned(power_source==PowerSource::Battery),unsigned(red_policy_ready));
  }
  if (power_source!=PowerSource::Unknown && measured==power_source &&
      led_policy_source!=power_source) {
    auto& pm=M5.Power.M5pm1;
    const bool desired=power_source==PowerSource::External;
    uint8_t cfg=0;
    if (pm.readRegister(0x06,&cfg,1)) {
      const bool already=bool(cfg&0x10)==desired;
      if (already || (pm.setLedEnLevel(desired) && pm.readRegister(0x06,&cfg,1) &&
                      bool(cfg&0x10)==desired)) {
        led_policy_source=power_source;
        led_enabled=desired;
        Serial.printf("PICS status LED battery=%u enabled=%u verified=1\n",
                      unsigned(!desired),unsigned(desired));
      }
    }
  }
  if (endurance.active && !endurance_resume_pending && measured==PowerSource::Battery) {
    const uint32_t secs=battery_run_seconds(now);
    if (secs>=endurance.checkpoint_seconds && secs-endurance.checkpoint_seconds>=60) {
      endurance.checkpoint_seconds=secs;
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

void end_reception(const char* why) {
  if (!receiving) return;
  receiving = false;
  portENTER_CRITICAL(&rx_lock);
  receiver.reset();
  portEXIT_CRITICAL(&rx_lock);
  focused = false;
  Serial.printf("PICS reception off reason=%s scans=%lu\n", why,
                (unsigned long)scan_cycles);
}

bool begin_reception(uint32_t now) {
  if (!scanner) {
    if (!BLEDevice::init("")) {
      Serial.println("PICS reception BLE init failed");
      return false;
    }
    scanner = BLEDevice::getScan();
    if (!scanner) return false;
    scanner->setActiveScan(false);
    scanner->setInterval(160);
    scanner->setWindow(100);
    scanner->setAdvertisedDeviceCallbacks(&adv_cb, true, false);
  }
  portENTER_CRITICAL(&rx_lock);
  receiver.reset();
  portEXIT_CRITICAL(&rx_lock);
  receiving = true;
  start_retry = now;
  focused = false;
  Serial.println("PICS reception on mode=manual");
  return true;
}

void poll_reception(uint32_t now) {
  if (scan_done.exchange(false)) {
    scan_in_flight = false;
    ++scan_cycles;
  }
  if (!receiving || scan_in_flight || int32_t(now-start_retry)<0) return;
  // Repeat short passive scans until toggled off. Finish the in-flight slice
  // naturally on off rather than racing a GAP stop against its completion.
  scan_in_flight = scanner->start(scan_slice_seconds, scanComplete, false);
  start_retry = now + (scan_in_flight ? 0 : 1000);
}

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
  d.setTextColor(color,bg);
  d.setTextSize(size);
  d.drawString(content, d.width()/2, y);
}

// One visual language for touch actions; physical-button instructions stay as text.
void draw_button(TouchButton box,const char* label,uint16_t color=fg,bool enabled=true,bool large=false) {
  auto& d=M5.Display;
  const uint16_t fill=enabled ? panel : 0x10e6;
  const uint16_t border=enabled ? 0x4b34 : 0x294a;
  const int radius=box.h<=33 ? 8 : 11;
  d.fillRoundRect(box.x,box.y,box.w,box.h,radius,fill);
  d.drawRoundRect(box.x,box.y,box.w,box.h,radius,border);
  if(!label || !label[0]) return;
  d.setFont(large ? ui_fonts::heading.for_text(label) : ui_fonts::body.for_text(label));
  d.setTextDatum(lgfx::textdatum_t::middle_center);
  d.setTextSize(1);
  d.setTextColor(enabled ? color : muted,fill);
  char text[65];
  snprintf(text,sizeof(text),"%s",label);
  const int limit=box.w-(box.w<=44 ? 6 : 14);
  if(d.textWidth(text)>limit) {
    size_t n=strlen(text);
    while(n && d.textWidth(text)+d.textWidth("...")>limit) {
      do { --n; } while(n && (static_cast<uint8_t>(text[n])&0xc0)==0x80);
      text[n]=0;
    }
    strncat(text,"...",sizeof(text)-strlen(text)-1);
  }
  d.drawString(text,box.x+box.w/2,box.y+box.h/2);
}

void draw_usage() {
  auto& d=M5.Display;
  const bool current=endurance.active && power_source==PowerSource::Battery;
  const Activity& a=current ? endurance.live : endurance.last;
  d.startWrite();
  d.fillScreen(bg);
  d.setFont(ui_fonts::heading.get());
  line(65,"電池の使用状況",1,0x86ff);
  d.setFont(ui_fonts::body.get());
  char dur[18], row[64];
  format_duration(dur,sizeof(dur),current ? battery_run_seconds(elapsed_ms()) : endurance.last_seconds);
  if (current || endurance.last_valid)
    snprintf(row,sizeof(row),"%s %s",current ? "今回" : "前回",dur);
  else snprintf(row,sizeof(row),"計測記録なし");
  line(101,row,1,fg);
  if (!a.sample_seen) {
    line(208,"使用時間の内訳はまだありません",1,muted);
  } else {
    const char* names[]={"画面点灯","BLE受信","Wi-Fi","音の回路","画面消灯・待機","緑LED"};
    const uint32_t times[]={a.screen,a.ble,a.wifi,a.speaker,a.idle,a.led};
    for (int i=0;i<6;++i) {
      const int y=140+30*i;
      d.setTextDatum(lgfx::textdatum_t::middle_left);
      d.setTextColor(fg,bg);
      d.drawString(names[i],115,y);
      format_duration(dur,sizeof(dur),times[i]/1000);
      d.setTextDatum(lgfx::textdatum_t::middle_right);
      d.drawString(dur,353,y);
    }
  }
  line(321,"使用時間（重複あり）",1,muted);
  line(348,"消費電力は未計測",1,muted);
  line(384,"黄：戻る",1,0xffc8);
  d.setFont(&lgfx::fonts::Font0);
  d.endWrite();
}
void draw_weather() {
  auto& d=M5.Display;
  const auto r=local_weather::read();
  d.startWrite();
  d.fillScreen(bg);
  d.setFont(ui_fonts::heading.get());
  line(83,"現在の天気",1,0x86ff);
  if (r.label[0]) {
    d.setFont(ui_fonts::heading.for_text(r.label));
    line(129,r.label,1,fg);
  }
  if (r.status==local_weather::Result::Ready) {
    char temp[24], asof[32];
    snprintf(temp,sizeof(temp),"%.1f℃",r.temperature_tenths/10.0);
    snprintf(asof,sizeof(asof),"%s JST",r.time_jst);
    d.setFont(ui_fonts::clock.get());
    line(202,temp,1,fg);
    d.setFont(ui_fonts::heading.get());
    line(259,local_weather::label_for(r.code),1,fg);
    d.setFont(ui_fonts::body.get());
    line(300,asof,1,muted);
    line(323,"Open-Meteo / 推定値",1,muted);
  } else {
    const char* status=!have_wifi() ? "Wi-Fi未設定" :
      r.status==local_weather::Result::Busy ? "時刻・天気を取得中" :
      r.status==local_weather::Result::NoNetwork ? "Wi-Fiに接続できません" :
      r.status==local_weather::Result::NoTime ? "時刻を取得できません" :
      r.status==local_weather::Result::NoForecast ? "天気を取得できません" : "天気データなし";
    line(220,status,1,fg);
  }
  d.setFont(ui_fonts::body.get());
  draw_button(weather_wifi_btn,"Wi-Fi設定",0x86ff,true,true);
  d.setFont(ui_fonts::body.get());
  line(414,"黄：戻る",1,0xffc8);
  d.setFont(&lgfx::fonts::Font0);
  d.endWrite();
  weather_drawn=r.status;
}

#include "wifi_menu.h"

void draw_alarm() {
  auto& d=M5.Display;
  d.fillScreen(bg);
  d.setFont(ui_fonts::heading.get());
  line(61, alarm_ringing ? (alarm_preview ? "アラーム試聴" : "アラーム") : "毎日のアラーム", 1, 0xffc8);
  if (alarm_ringing) {
    d.setFont(ui_fonts::large.get());
    if (read_clock()) {
      char hhmm[6];
      snprintf(hhmm,sizeof(hhmm),"%.5s",clock_label+11);
      line(217,hhmm,1);
    } else line(217,"ALARM",1);
    d.setFont(ui_fonts::body.get());
    draw_button(alarm_stop_btn,"停止",0xffc8,true,true);
    line(375,"黄・青でも停止",1,muted);
    return;
  }
  const bool valid=read_clock();
  d.setFont(ui_fonts::body.get());
  line(105, valid ? "日本時間・毎日・音量 MAX" : "時計未設定のため鳴りません",1,valid ? muted : 0xfaaa);
  char tm[8];
  snprintf(tm,sizeof(tm),"%02d:%02d",alarm_edit_minutes/60,alarm_edit_minutes%60);
  d.setFont(ui_fonts::clock.get());
  line(161,tm,1,fg);
  draw_button(alarm_hours_minus_btn,"時 −",fg);
  draw_button(alarm_hours_plus_btn,"時 ＋",fg);
  draw_button(alarm_minutes_minus_btn,"分 −",fg);
  draw_button(alarm_minutes_plus_btn,"分 ＋",fg);
  draw_button(alarm_on_btn,alarm_edit_on ? "ON  有効" : "OFF  無効",alarm_edit_on ? 0xaff5 : muted);
  draw_button(alarm_preview_btn,"試聴",0x86ff);
  d.setFont(ui_fonts::body.get());
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
  const int x=t.x,y=t.y;
  if (alarm_ringing) {
    if (!alarm_stop_btn.hit(x,y)) return false;
    end_alarm(now);
    return true;
  }
  if (alarm_hours_minus_btn.hit(x,y)) alarm_edit_minutes=daily_alarm::set_hour(alarm_edit_minutes,-1);
  else if (alarm_hours_plus_btn.hit(x,y)) alarm_edit_minutes=daily_alarm::set_hour(alarm_edit_minutes,1);
  else if (alarm_minutes_minus_btn.hit(x,y)) alarm_edit_minutes=daily_alarm::set_minute(alarm_edit_minutes,-1);
  else if (alarm_minutes_plus_btn.hit(x,y)) alarm_edit_minutes=daily_alarm::set_minute(alarm_edit_minutes,1);
  else if (alarm_on_btn.hit(x,y)) alarm_edit_on=!alarm_edit_on;
  else if (alarm_preview_btn.hit(x,y)) begin_alarm(now,true);
  else return false;
  return true;
}

constexpr int mascot_band_y = 133;
constexpr int home_mascot_shift = 32;
constexpr int mascot_base_y = 135;
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
  const int dy=home_mascot_shift;
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
      d.fillRect(x0,mascot_band_y+dy,x1-x0,32,bg);
    }
    draw_dog(x,mascot_base_y+dy,turn<cycle_ms/2,phase_time,i==1);
    mascot_prev_x[i]=x;
  }
}

void smooth_line(int y,const char* value,uint16_t fore,uint16_t back) {
  auto& d=M5.Display;
  d.setTextDatum(lgfx::textdatum_t::middle_center);
  d.setTextColor(fore,back);
  d.setTextSize(1);
  d.drawString(value,d.width()/2,y);
}

void draw_swipe_handle() {
  // Eight-by-eight subpixel coverage for the lower arc, with round caps.
  static constexpr uint8_t mask[][79]={
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,1,43,56,49,38,29,20,11,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,11,20,29,38,49,56,43,1,0,0},
    {0,0,17,64,64,64,64,64,64,64,64,58,50,43,35,29,22,16,8,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,8,16,22,29,35,43,50,58,64,64,64,64,64,64,64,64,17,0,0},
    {0,0,8,61,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,62,56,51,48,40,40,32,32,25,24,24,16,16,16,16,11,8,8,8,8,8,8,8,11,16,16,16,16,24,24,25,32,32,40,40,48,51,56,62,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,61,8,0,0},
    {0,0,0,10,28,37,46,57,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,57,46,37,28,10,0,0,0},
    {0,0,0,0,0,0,0,0,1,10,18,26,34,42,49,56,63,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,63,56,49,42,34,26,18,10,1,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,5,11,16,24,28,32,38,40,48,48,55,56,58,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,58,56,55,48,48,40,38,32,28,24,16,11,5,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,6,8,8,8,8,8,8,8,8,8,8,8,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  };
  auto& d=M5.Display;
  const int x0=d.width()/2-39, y0=d.height()-17;
  for (unsigned y=0;y<sizeof(mask)/sizeof(mask[0]);++y) {
    for (unsigned x=0;x<sizeof(mask[0]);++x) {
      const unsigned a=mask[y][x];
      if (!a) continue;
      const unsigned z=64-a;
      const unsigned r=(31*a+((bg>>11)&31)*z+32)>>6;
      const unsigned g=(63*a+((bg>>5)&63)*z+32)>>6;
      const unsigned b=(31*a+(bg&31)*z+32)>>6;
      d.drawPixel(x0+x,y0+y,uint16_t((r<<11)|(g<<5)|b));
    }
  }
}
enum class TrialButton : uint8_t { Ready=1, Usb, Detecting, Radio, Done, Hardware };
TrialButton display_trial_state() {
  if (oled_rail_trial::tested) return TrialButton::Done;
  if (power_source==PowerSource::External) return TrialButton::Usb;
  if (power_source!=PowerSource::Battery) return TrialButton::Detecting;
  if (receiving || wifi_busy() || local_weather::scanning()) return TrialButton::Radio;
  return TrialButton::Ready;
}
uint8_t oled_visible_error=0;
bool run_display_trial(uint32_t now,const char* trigger) {
  auto status=display_trial_state();
  if(status==TrialButton::Done) return false;
  oled_visible_error=0;
  oled_rail_trial::probe={};
  oled_rail_trial::probe.attempt=++oled_attempts;
  oled_rail_trial::probe.button=static_cast<uint8_t>(status);
  Serial.printf("PICS OLED trigger=%s\n",trigger);
  if (status==TrialButton::Ready) {
    const auto source=classify_vin(read_vin_mv());
    if(source==PowerSource::Battery) {
      if(oled_rail_trial::cut()) {
        help_view=false;
        screen_awake=false;
      } else {
        status=TrialButton::Hardware;
        oled_rail_trial::probe.button=static_cast<uint8_t>(status);
        oled_visible_error=oled_rail_trial::probe.error;
        if(oled_rail_trial::inactive()) {
          screen_awake=false;
          wake_screen();
        }
      }
    } else status=source==PowerSource::External ? TrialButton::Usb : TrialButton::Detecting;
  }
  oled_rail_trial::probe.button=static_cast<uint8_t>(status);
  save_oled_probe();
  screen_activity_at=now;
  return screen_awake;
}
void sleep_screen_for_idle() {
  first_tap=false;
  const bool may_cut=!oled_auto_blocked &&
      power_source!=PowerSource::Unknown &&
      !receiving && !scan_in_flight && !wifi_busy() && !local_weather::scanning() &&
      !alarm_ringing && !alarm_setting_open &&
      classify_vin(read_vin_mv())==power_source;
  if (may_cut) {
    // Keep only the latest cycle, in one compact NVS entry separate from the manual trial.
    oled_rail_trial::probe={};
    oled_auto_sequence=oled_auto_record::next(oled_auto_sequence);
    oled_auto_usb=power_source==PowerSource::External;
    oled_auto_active=true;
    if(oled_rail_trial::cut(false)) {
      screen_awake=false;
      Serial.printf("PICS OLED auto rail off ms=%lu\n",(unsigned long)oled_rail_trial::probe.rail_ms);
      save_oled_auto();
      return;
    }
    oled_auto_blocked=true;
    Serial.printf("PICS OLED auto fallback error=%u\n",unsigned(oled_rail_trial::probe.error));
    save_oled_auto();
    if(oled_rail_trial::inactive() && !wake_screen()) { screen_awake=false; return; }
    oled_auto_active=false;
  }
  M5.Display.fillScreen(0x0000);
  M5.Display.setBrightness(0);
  screen_awake=false;
  Serial.println("PICS auto display off (display circuit powered)");
}
void draw_button_help() {
  auto& d=M5.Display;
  d.startWrite();
  d.fillScreen(bg);
  d.setFont(ui_fonts::heading.get());
  smooth_line(62,"ボタン操作",fg,bg);
  const bool battery=power_source==PowerSource::Battery && red_policy_ready;
  d.setFont(ui_fonts::heading.get());
  smooth_line(111,"赤",0xfaaa,bg);
  d.setFont(ui_fonts::body.get());
  smooth_line(139,battery ? "長押し：電源オフ" : "1回：起動・再起動",fg,bg);
  smooth_line(164,battery ? "3回：再起動（計測を保存）" : "2回：電源オフ",fg,bg);
  d.setFont(ui_fonts::heading.get());
  smooth_line(202,"黄",0xffc8,bg);
  d.setFont(ui_fonts::body.get());
  smooth_line(229,"短押し：表示・写真から戻る・CH切替",fg,bg);
  smooth_line(254,"長押し：アラーム設定",fg,bg);
  d.setFont(ui_fonts::heading.get());
  smooth_line(293,"青",0x86ff,bg);
  d.setFont(ui_fonts::body.get());
  smooth_line(320,"短押し：テスト",fg,bg);
  smooth_line(345,"長押し：受信を開始・停止",fg,bg);
  const auto trial=display_trial_state();
  const char* trial_label=trial==TrialButton::Done ? "表示電源オフ：試験済み" :
      trial==TrialButton::Usb ? "試すにはUSBを抜く" :
      trial==TrialButton::Detecting ? "電池駆動を確認中" :
      trial==TrialButton::Radio ? "通信が終わるまで待機" : "表示電源オフを試す（青）";
  char failed[40];
  if(oled_visible_error) {
    snprintf(failed,sizeof(failed),"表示電源オフ失敗 E%u",unsigned(oled_visible_error));
    draw_button(display_trial_btn,failed,0xfaaa,!oled_rail_trial::tested);
  } else draw_button(display_trial_btn,trial_label,0xaff5,trial==TrialButton::Ready);
  d.setFont(ui_fonts::body.get());
  smooth_line(426,"下スワイプ／黄で閉じる",muted,bg);
  d.setFont(&lgfx::fonts::Font0);
  draw_swipe_handle();
  d.endWrite();
}

constexpr int home_date_y=49, home_clock_y=91, home_weather_y=141;
constexpr int home_top_divide_y=117, home_weather_end_y=161;
constexpr int home_logo_y=166, home_brand_y=239;
constexpr int home_battery_y=home_usage_btn.y+19, home_previous_y=home_usage_btn.y+50;
constexpr int home_idle_status_y=291, home_rx_status_y=281, home_rx_info_y=319;
// Shared focus layout for live PICS and the isolated demo.
constexpr int signal_logo_y=100, signal_brand_y=176, signal_heading_y=221;
constexpr int signal_state_y=267, signal_number_y=328;
constexpr int signal_clear_y=199, signal_clear_end=369, signal_clear_w=250;
constexpr int signal_rx_y=388, signal_rx_w=216;
void draw_weather_icon(int x,int y,int code,bool fresh) {
  auto& d=M5.Display;
  const bool ready=code>=0;
  const uint16_t sun=fresh ? 0xff46 : muted;
  const uint16_t cloud=fresh ? 0xb6bc : muted;
  const uint16_t rain=fresh ? 0x661f : muted;
  if (code==0 || code==1 || code==2) {
    const int sx=code==0 ? x : x-4;
    const int sy=code==0 ? y : y-3;
    d.fillCircle(sx,sy,6,sun);
    constexpr int8_t rays[8][4]={
      {0,-9,0,-12},{0,9,0,12},{-9,0,-12,0},{9,0,12,0},
      {-6,-6,-9,-9},{6,-6,9,-9},{-6,6,-9,9},{6,6,9,9}};
    for (const auto& r:rays) d.drawLine(sx+r[0],sy+r[1],sx+r[2],sy+r[3],sun);
  }
  if (code!=0) {
    const int cy=(code==1 || code==2) ? y+3 : y-1;
    if (ready) {
      d.fillCircle(x-6,cy,5,cloud);
      d.fillCircle(x+2,cy-4,7,cloud);
      d.fillCircle(x+9,cy,5,cloud);
      d.fillRoundRect(x-11,cy,25,6,3,cloud);
    } else {
      d.drawRoundRect(x-8,cy-2,17,7,3,cloud);
      d.drawCircle(x+1,cy-3,4,cloud);
    }
    if ((code>=51 && code<=67) || (code>=80 && code<=82))
      for (int i=-5;i<=5;i+=5) d.drawLine(x+i,cy+6,x+i-2,cy+9,rain);
    else if ((code>=71 && code<=77) || code==85 || code==86)
      for (int i=-5;i<=5;i+=5) d.fillCircle(x+i,cy+8,1,cloud);
    else if (code==45 || code==48)
      for (int k=0;k<2;++k) d.drawFastHLine(x-6+k*2,cy+6+3*k,13-k*3,cloud);
    else if (code>=95)
      d.fillTriangle(x+1,cy+3,x-4,cy+11,x+5,cy+5,sun);
  }
}

void draw_weather_row(int cx,int y) {
  auto& d=M5.Display;
  const auto& w=last_home_weather;
  const bool ready=w.status==local_weather::Result::Ready;
  const time_t utc=ready ? time(nullptr) : 0;
  const bool fresh=ready && utc>=w.epoch && utc-w.epoch<=90*60;
  const bool next=ready && local_weather::forecast_is_tomorrow(w,utc);
  const bool next_fresh=next && utc>=w.epoch && utc-w.epoch<=6*3600;
  char temp[20], other[24];
  if (ready) snprintf(temp,sizeof(temp),"%.1f℃",w.temperature_tenths/10.0);
  else snprintf(temp,sizeof(temp),"--℃");
  if (next) {
    const long lo=std::lround(w.next_min_tenths/10.0);
    const long hi=std::lround(w.next_max_tenths/10.0);
    snprintf(other,sizeof(other),"%ld〜%ld℃",lo,hi);
  } else snprintf(other,sizeof(other),"--℃");
  constexpr int icon_w=31;
  const bool enabled=!receiving;
  const uint16_t tile_bg=enabled ? panel : 0x10e6;
  draw_button(home_weather_btn,nullptr,fg,enabled);
  d.setTextDatum(lgfx::textdatum_t::middle_left);
  d.setTextSize(1);
  d.setFont(ui_fonts::heading.get());
  const int today_left=cx-94-(icon_w+7+d.textWidth(temp))/2;
  draw_weather_icon(today_left+icon_w/2,y,ready ? w.code : -1,fresh);
  d.setTextColor(fresh ? fg : muted,tile_bg);
  d.drawString(temp,today_left+icon_w+7,y);

  d.drawFastVLine(cx,y-12,24,0x296c);
  d.setFont(ui_fonts::body.get());
  const int label_w=d.textWidth("明日");
  const int next_left=cx+96-(label_w+icon_w+8+d.textWidth(other))/2;
  d.setTextColor(muted,tile_bg);
  d.drawString("明日",next_left,y);
  draw_weather_icon(next_left+label_w+4+icon_w/2,y,next ? w.next_code : -1,next_fresh);
  d.setTextColor(next_fresh ? fg : muted,tile_bg);
  d.drawString(other,next_left+label_w+icon_w+8,y);
}
// Native emoji-style icon: no emoji font needed, and the fill follows the charge.
constexpr int battery_icon_w=25, battery_icon_gap=7;
constexpr int battery_icon_fill(int percent) {
  return percent<=0 ? 0 : (16 * std::min(percent,100) + 99) / 100;
}
static_assert(battery_icon_fill(-1)==0 && battery_icon_fill(0)==0 &&
              battery_icon_fill(50)==8 && battery_icon_fill(100)==16);

void draw_battery_icon(int x,int y,int percent,uint16_t back) {
  auto& d=M5.Display;
  const uint16_t outline=percent<0 ? muted : 0xd6fb;
  d.fillSmoothRoundRect(x+21,y-3,4,6,1,outline);
  d.fillSmoothRoundRect(x,y-7,22,14,3,outline);
  d.fillSmoothRoundRect(x+2,y-5,18,10,2,back);
  const int charge=battery_icon_fill(percent);
  const uint16_t color=percent<=15 ? 0xfaaa : percent<=30 ? 0xff06 : 0x6f93;
  if (charge>=3) d.fillSmoothRoundRect(x+3,y-4,charge,8,2,color);
  else if (charge>0) d.fillRect(x+3,y-3,charge,6,color);
}

void draw_power_banner() {
  auto& d = M5.Display;
  const int cx=d.width()/2;
  const bool badge=have_wifi();
  const int divide_y=home_top_divide_y;
  const int weather_end=home_weather_end_y;
  const int date_y=home_date_y;
  d.fillRect(cx-125,18,250,divide_y-18,bg);
  d.fillRect(cx-188,divide_y,376,weather_end-divide_y,bg);
  const bool usage_enabled=endurance_can_save && !receiving;
  const uint16_t battery_bg=usage_enabled ? panel : 0x1127;
  if (usage_enabled) draw_button(home_usage_btn,nullptr);
  else d.fillRoundRect(home_usage_btn.x,home_usage_btn.y,home_usage_btn.w,home_usage_btn.h,11,battery_bg);
  const bool clock_valid=strlen(clock_label)==19 && clock_label[4]=='/' && clock_label[10]==' ';
  char date[24];
  if (clock_valid) snprintf(date,sizeof(date),"%.10s",clock_label);
  else snprintf(date,sizeof(date),"日時未設定");
  d.setFont(ui_fonts::body.get());
  char battery[18], time_text[18], row[64];
  if (battery_percent < 0) snprintf(battery, sizeof(battery), "--%%");
  else snprintf(battery, sizeof(battery), "%d%%", battery_percent);
  if (endurance.active) {
    format_duration(time_text, sizeof(time_text), battery_run_seconds(elapsed_ms()));
    snprintf(row, sizeof(row), "%s  計測 %s", battery, time_text);
  } else {
    snprintf(row, sizeof(row), "%s  %s", battery,
             power_source == PowerSource::External ? "USB給電" : "判定中");
  }
  d.setFont(ui_fonts::body.get());
  d.setTextSize(1);
  if (endurance.active && d.textWidth(row)+battery_icon_w+battery_icon_gap>210)
    snprintf(row,sizeof(row),"%s  %s",battery,time_text);
  const int battery_y=home_battery_y;
  const int row_left=cx-(d.textWidth(row)+battery_icon_w+battery_icon_gap)/2;
  draw_battery_icon(row_left,battery_y,battery_percent,battery_bg);
  d.setTextDatum(lgfx::textdatum_t::middle_left);
  d.setTextColor(battery_percent >= 0 && battery_percent <= 15 ? 0xfaaa : 0xaff5,battery_bg);
  d.drawString(row,row_left+battery_icon_w+battery_icon_gap,battery_y);
  if (endurance.last_valid) {
    format_duration(time_text, sizeof(time_text), endurance.last_seconds);
    snprintf(row, sizeof(row), "前回 %s%s", endurance.last_interrupted ? "約 " : "", time_text);
  } else snprintf(row, sizeof(row), "前回 --:--:--");
  smooth_line(home_previous_y,endurance_can_save ? row : "記録保存なし",muted,battery_bg);
  if (usage_enabled) smooth_line(home_usage_btn.y+81,"使用状況  >",0x86ff,battery_bg);
  d.setFont(ui_fonts::heading.get());
  if (clock_valid && clock_weekday>=0 && clock_weekday<7) {
    d.setTextSize(1);
    const int date_w=d.textWidth(date);
    char day[16];
    snprintf(day,sizeof(day),"（%s）",weekdays[clock_weekday]);
    d.setFont(ui_fonts::body.get());
    const int day_w=d.textWidth(day);
    const int left=cx-(date_w+day_w+3)/2;
    d.setTextColor(muted,bg);
    d.setTextDatum(lgfx::textdatum_t::middle_left);
    d.setFont(ui_fonts::heading.get());
    d.drawString(date,left,date_y);
    d.setFont(ui_fonts::body.get());
    d.drawString(day,left+date_w+3,date_y+2);
  } else line(date_y,date,1,muted);
  d.setFont(ui_fonts::clock.get());
  smooth_line(home_clock_y,clock_valid ? clock_label+11 : "--:--:--",fg,bg);
  if (badge) draw_weather_row(cx,home_weather_y);
  else draw_button(home_weather_btn,"天気・Wi-Fi設定",0x86ff,!receiving);
  draw_swipe_handle();
  d.setFont(&lgfx::fonts::Font0);
}

void draw_brand(bool signal_layout) {
  auto& d=M5.Display;
  const bool swap=d.getSwapBytes();
  d.setSwapBytes(true);
  d.pushImage(d.width()/2-25,signal_layout ? signal_logo_y : home_logo_y,50,50,system_kakari_mark);
  d.setSwapBytes(swap);
  d.setFont(ui_fonts::heading.get());
  line(signal_layout ? signal_brand_y : home_brand_y,"システム係",1,fg);
  d.setFont(&lgfx::fonts::Font0);
}

void draw_reception_status(int y, bool focused_view) {
  auto& d=M5.Display;
  const int w=focused_view ? signal_rx_w : 300;
  d.fillRect((d.width()-w)/2,y-13,w,26,bg);
  const char* label="受信中";
  d.setFont(ui_fonts::body.for_text(label));
  line(y,label,1,0x86ff);
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
  constexpr uint16_t signal_red=0xf944; // Vivid red, approximately #FF2821.
  const uint16_t accent=red ? signal_red : valid ? 0x4f99 : muted;
  const uint16_t dark=0x21ca;
  lgfx::rgb565_t on[16], off[16];
  auto palette=[&](lgfx::rgb565_t* out,uint16_t color) {
    for (unsigned a=0;a<16;++a) {
      const unsigned z=15-a;
      const unsigned r=(((color>>11)&31)*a+((bg>>11)&31)*z+7)/15;
      const unsigned g=(((color>>5)&63)*a+((bg>>5)&63)*z+7)/15;
      const unsigned b=((color&31)*a+(bg&31)*z+7)/15;
      out[a]=uint16_t((r<<11)|(g<<5)|b);
    }
  };
  palette(on,accent);
  palette(off,dark);
  for (uint8_t i=0;i<8;++i) {
    const auto& a=signal_mask::arcs[i];
    const bool lit=i<level && (!blink || blink_on);
    d.pushImage(d.width()/2+a.x,d.height()/2+a.y,a.w,a.h,
                static_cast<const void*>(a.pixels),0,lgfx::palette_4bit,lit ? on : off);
  }
  d.fillRect((d.width()-signal_clear_w)/2,signal_clear_y,signal_clear_w,signal_clear_end-signal_clear_y,bg);
  d.setFont(ui_fonts::body.get());
  line(signal_heading_y,test_active ? "テスト・模擬データ" : "受信データ・観測用",1,test_active ? 0xfda0 : muted);
  d.setFont(ui_fonts::heading.get());
  line(signal_state_y,red ? "赤信号" : blink ? "青点滅" : valid ? "青信号" : "状態不明",1,accent);
  char number[4];
  if (level) snprintf(number,sizeof(number),"%d",level);
  else snprintf(number,sizeof(number),"-");
  d.setFont(ui_fonts::large.get());
  line(signal_number_y,number,1,fg);
}

void draw(const pics::Snapshot& snap, bool blink_on=true) {
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(bg);
  const bool single=snap.status==pics::Snapshot::Status::Single && snap.report.count>0;
  if (!test_active && !single) {
    draw_power_banner();
    draw_mascots(true);
  }
  draw_brand(test_active || single);
  if (single) {
    draw_focus(snap,blink_on);
    if (receiving && !test_active)
      draw_reception_status(signal_rx_y,true);
  } else if (!test_active) {
    const bool live=receiving;
    const int cx=d.width()/2;
    const int status_y=live ? home_rx_status_y : home_idle_status_y;
    d.fillRoundRect(cx-92,status_y-21,184,43,21,live ? 0x12ad : 0x1107);
    d.drawRoundRect(cx-92,status_y-21,184,43,21,live ? 0x3457 : 0x294c);
    d.setFont(ui_fonts::heading.get());
    smooth_line(status_y,snap.status==pics::Snapshot::Status::Multiple ? "複数の交差点" :
                live ? "受信待ち" : "受信オフ",live ? 0xdfff : muted,live ? 0x12ad : 0x1107);
    d.setFont(ui_fonts::body.get());
    if (receiving)
      draw_reception_status(home_rx_info_y,false);
    else if (snap.status==pics::Snapshot::Status::Multiple)
      line(home_rx_info_y,"信号の個別表示を停止",1,muted);
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
  Serial.printf("PICS boot reset=%u radio=%s\n",unsigned(esp_reset_reason()),wifi_radio_state());
  if (gallery::photo_count) Serial.printf("PICS gallery ready photos=%d touch=%u zoom=8 rounded=1 pan=1\n", gallery::photo_count, unsigned(M5.Touch.isEnabled()));
  panel_ready = M5.Display.getBoard() == lgfx::board_M5StopWatch &&
                M5.Display.width() > 0 && M5.Display.height() > 0;
  if (panel_ready) {
    M5.Display.fillScreen(0xf81f);
    delay(750);
  }
  if (panel_ready) {
    ui_fonts::begin();
    load_endurance();
    load_oled_auto();
    oled_rail_trial::report_preflight();
    oled_rail_trial::Probe last;
    if(endurance_can_save && endurance_store.getBytesLength("oled_detail2")==sizeof(last) &&
       endurance_store.getBytes("oled_detail2",&last,sizeof(last))==sizeof(last) &&
       last.tag==oled_rail_trial::probe_tag) {
      oled_attempts=last.attempt;
      log_oled_probe(last);
    }
    Serial.printf("PICS wifi profiles count=%d\n",wifi_profiles::count(wifi_book));
    load_alarm();
    if (read_clock()) Serial.printf("PICS clock retained %s JST weekday=%d\n",clock_label,clock_weekday);
    if (have_wifi()) start_weather("boot");
    update_battery();
    update_power_source(elapsed_ms());
  }
  draw(pics::Snapshot{});
  Serial.println("PICS reception standby BLE not initialized");
  screen_activity_at = millis();
}
void loop() {
  M5.update();
  const uint32_t now = millis();
  const uint64_t run_ms = elapsed_ms();
  const auto prior_power_source=power_source;
  const bool prior_red_ready=red_policy_ready;
  if (panel_ready) {
    sample_activity(run_ms);
    update_power_source(run_ms);
    if(power_source!=prior_power_source)
      Serial.printf("PICS supply %s -> %s radio=%s\n",source_name(prior_power_source),
                    source_name(power_source),wifi_radio_state());
    poll_red_button(now,run_ms);
    poll_clock_command();
    poll_weather();
  }
  static uint32_t diag_at = 0;
  if ((int32_t)(now - diag_at) >= 0) {
    diag_at = now + 5000;
    Serial.printf("PICS screen=%s scans=%lu alarm=%02d:%02d/%u vol=%u saved=%u rx=%u scan=%u supply=%s radio=%s\n",
                  screen_awake ? "on" : "off", (unsigned long)scan_cycles,
                  daily_alarm::minutes_of(alarm_setting)/60,daily_alarm::minutes_of(alarm_setting)%60,
                  unsigned(bool(alarm_setting & daily_alarm::enabled_bit)),unsigned(alarm_volume),
                  unsigned(endurance_can_save && endurance_store.getUShort("alarm_cfg",0)==alarm_setting),
                  unsigned(receiving), unsigned(scan_in_flight), source_name(power_source), wifi_radio_state());
  }
  const bool alarm_changed = panel_ready && alarm_tick(now);
  if (alarm_ringing) {
    weather_view=false; usage_view=false; help_view=false;
    if(wifi_page!=WifiPage::Closed) wifi_close();
  }
  poll_reception(now);
  bool force_redraw = alarm_changed ||
      prior_power_source!=power_source || prior_red_ready!=red_policy_ready;
  bool alarm_ui_dirty = alarm_changed;
  // A short yellow press keeps the old action. Hold it to open settings.
  if (M5.BtnA.wasPressed()) {
    alarm_ignore_yellow_release = !screen_awake || alarm_ringing;
    if (!screen_awake && panel_ready) {
      force_redraw = wake_screen();
      screen_activity_at = now;
      Serial.printf("PICS yellow wake ok=%u scans=%lu ms=%lu\n",unsigned(screen_awake),
                    (unsigned long)scan_cycles, (unsigned long)now);
    }
  }
  if (panel_ready && screen_awake && !alarm_ringing && !alarm_setting_open &&
      M5.BtnA.pressedFor(alarm_hold_ms)) {
    // The menu state prevents repeated opening while the button is held.
    alarm_ignore_yellow_release = true;
    if(wifi_page!=WifiPage::Closed) wifi_close();
    alarm_setting_open = true;
    alarm_ignore_blue_release = false;
    alarm_edit_minutes = daily_alarm::minutes_of(alarm_setting);
    alarm_edit_on = bool(alarm_setting & daily_alarm::enabled_bit);
    alarm_save_error = false;
    if (test_active) end_test();
    weather_view=false;
    usage_view=false;
    help_view=false;
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
      if (screen_awake && alarm_ui_dirty) draw_alarm();
      if (M5.BtnA.wasReleased()) alarm_ignore_yellow_release = false;
      delay(5);
      return;
    }
    screen_activity_at = now;
    force_redraw = true;
  }
  if (panel_ready && screen_awake && weather_view && M5.Touch.isEnabled() && M5.Touch.getCount()==1) {
    const auto& t=M5.Touch.getDetail();
    if(t.wasPressed()) {
      screen_activity_at=now;
      if(wifi_page!=WifiPage::Closed) wifi_tap(int(t.x),int(t.y));
      else if(weather_wifi_btn.hit(int(t.x),int(t.y))) {
        wifi_go(WifiPage::List);
      }
    }
  }
  if (wifi_page!=WifiPage::Closed) {
    if (!screen_awake || uint32_t(now-screen_activity_at)>=screen_timeout_ms) {
      wifi_close(); weather_view=false; force_redraw=true;
    } else {
      if (M5.BtnA.wasReleased()) {
        if (!alarm_ignore_yellow_release && !swallow_yellow) wifi_back();
        alarm_ignore_yellow_release=false;
        screen_activity_at=now;
      }
      if(wifi_page!=WifiPage::Closed) {
        if(wifi_deferred_scan && !wifi_busy()) {
          wifi_deferred_scan=false;
          local_weather::scan_start();
          wifi_ui_dirty=true;
        }
        if(wifi_ui_dirty || wifi_drawn_scan!=local_weather::scan_state() ||
           wifi_drawn_busy!=wifi_busy()) draw_wifi_menu();
        mascot_previous_ms=0;
        delay(5); return;
      }
      force_redraw=true;
      swallow_yellow=true;
    }
  }
  static bool trial_press_handled=false;
  if (!screen_awake || !help_view) trial_press_handled=false;
  if (panel_ready && M5.Touch.isEnabled() && !test_active && !weather_view && !usage_view) {
    const auto g = gallery_gesture(now);
    if (screen_awake && help_view) {
      bool trial_tap=g.tap && display_trial_btn.hit(g.x,g.y);
      if(M5.Touch.getCount()==1) {
        const auto& t=M5.Touch.getDetail();
        // Use the same first-contact event as Wi-Fi settings, with tap-release as a fallback.
        if(t.wasPressed()) {
          trial_press_handled=display_trial_btn.hit(int(t.x),int(t.y));
          trial_tap=trial_press_handled;
        } else if(t.wasReleased()) {
          trial_tap=trial_tap && !trial_press_handled;
          trial_press_handled=false;
        }
      } else trial_press_handled=false;
      if (g.drawer<0) {
        help_view=false;
        screen_activity_at=now;
        force_redraw=true;
        Serial.println("PICS button help closed by swipe");
      } else if (trial_tap) {
        force_redraw=run_display_trial(now,"touch") || force_redraw;
      }
    } else if (screen_awake && !photo_page && g.drawer>0) {
      oled_visible_error=0;
      help_view=true;
      screen_activity_at=now;
      force_redraw=true;
      Serial.println("PICS button help opened by swipe");
    } else if (screen_awake && !test_active && !receiving && !photo_page && g.tap &&
        endurance_can_save && home_usage_btn.hit(g.x,g.y)) {
      usage_view=true;
      Serial.println("PICS usage screen");
      screen_activity_at=now;
      force_redraw=true;
    } else if (screen_awake && !test_active && !receiving && !photo_page && g.tap &&
        home_weather_btn.hit(g.x,g.y)) {
      weather_view=true;
      screen_activity_at=now;
      start_weather("weather-tap");
      force_redraw=true;
    } else if (screen_awake && !test_active && photo_page && photo_scale > 1 && (g.dx || g.dy || g.drag_end)) {
      screen_activity_at = now;
      first_tap = false;
      if (g.dx || g.dy) pan_pending = move_zoom_view(g.dx, g.dy) || pan_pending;
      if (g.drag_end) {
        Serial.printf("PICS gallery pan page=%d zoom=%d x=%d y=%d cached=%u\n",
                      photo_page, photo_scale, photo_view_x, photo_view_y, unsigned(cached_success));
      }
    } else if (gallery::photo_count && screen_awake && !test_active && g.direction) {
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
      if (screen_awake && (weather_view || usage_view || help_view)) {
        weather_view=false;
        usage_view=false;
        help_view=false;
        force_redraw=true;
      } else if (screen_awake && photo_page) {
        photo_page = 0;
        reset_gallery_gesture();
        force_redraw = true;
        Serial.println("PICS gallery home");
      } else if (screen_awake && focused) cycle_channel=true;
    }
  }
  if (panel_ready && !swallow_blue) {
    if (M5.BtnB.wasPressed()) live_blue_held = false;
    const bool blue_long = !live_blue_held && M5.BtnB.pressedFor(700);
    const bool blue_short = !live_blue_held && M5.BtnB.wasReleased();
    if (blue_short && help_view && screen_awake) {
      force_redraw=run_display_trial(now,"blue") || force_redraw;
    } else if (blue_long || blue_short) {
      live_blue_held = blue_long;
      weather_view=false;
      usage_view=false;
      help_view=false;
      photo_page = 0;
      reset_gallery_gesture();
      screen_activity_at=now;
      if (!screen_awake) {
        if (!wake_screen()) { delay(5); return; }
      }
      if (blue_long) {
        if (test_active) end_test();
        if (receiving) end_reception("button");
        else begin_reception(now);
      } else if (test_active) {
        end_test();
      } else {
        test_receiver.reset();
        test_active=true;
        test_started_at=now;
        test_step=-1;
        Serial.println("PICS TEST begin");
      }
      force_redraw=true;
    }
  }
  const bool was_testing=test_active;
  update_test(now);
  if (was_testing && !test_active) {
    screen_activity_at=now;
    force_redraw=true;
  }
  if (panel_ready && screen_awake && !test_active && !receiving &&
      uint32_t(now - screen_activity_at) >= screen_timeout_ms) {
    sleep_screen_for_idle();
  }
  bool banner_dirty=false;
  if (panel_ready && screen_awake &&
      (force_redraw || uint32_t(millis() - battery_sample_at) >= 10000))
    banner_dirty=update_battery();
  if (help_view) {
    static auto previous_trial=TrialButton::Detecting;
    const auto trial=display_trial_state();
    if (screen_awake && (force_redraw || trial!=previous_trial)) draw_button_help();
    previous_trial=trial;
    mascot_previous_ms=0;
    delay(5);
    return;
  }
  if (usage_view) {
    static uint64_t usage_draw_second=UINT64_MAX;
    if (screen_awake && (force_redraw || usage_draw_second!=run_ms/1000)) {
      usage_draw_second=run_ms/1000;
      draw_usage();
    }
    mascot_previous_ms=0;
    delay(5);
    return;
  }
  if (weather_view) {
    if (screen_awake && (force_redraw || weather_drawn!=local_weather::read().status))
      draw_weather();
    mascot_previous_ms=0;
    delay(5);
    return;
  }
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
    else if (receiving) {
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
  if (panel_ready && screen_awake && !test_active && !focused && (force_redraw || banner_dirty || banner_second != run_ms / 1000)) {
    banner_second = run_ms / 1000;
    read_clock();
    M5.Display.startWrite();
    draw_power_banner();
    M5.Display.endWrite();
  }
  if (panel_ready && screen_awake && !test_active && !focused) {
    const uint32_t frame_now=millis();
    if (uint32_t(frame_now-mascot_at)>=mascot_frame_ms) {
      const uint32_t gap=mascot_previous_ms?frame_now-mascot_previous_ms:mascot_frame_ms;
      mascot_at=frame_now;
      mascot_previous_ms=frame_now;
      mascot_walk_ms+=gap;
      M5.Display.startWrite();
      draw_mascots(false);
      M5.Display.endWrite();
      if (!mascot_stats_at) mascot_stats_at=frame_now;
      ++mascot_stats_frames;
      if (gap>70) ++mascot_stats_slow;
      mascot_stats_max=std::max(mascot_stats_max,gap);
      if (uint32_t(frame_now-mascot_stats_at)>=5000 && mascot_stats_frames>=5) {
        if (Serial.availableForWrite()>=104)
          Serial.printf("PICS ANIM mode=live frames=%lu max_gap_ms=%lu over70=%lu\n",
              (unsigned long)mascot_stats_frames,
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
