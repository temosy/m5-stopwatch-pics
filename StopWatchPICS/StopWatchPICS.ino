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
  line(360, "黄：表示・信号の切替", 1, 0xffc8);
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
  panel_ready = M5.Display.getBoard() == lgfx::board_M5StopWatch &&
                M5.Display.width() > 0 && M5.Display.height() > 0;
  if (panel_ready) {
    M5.Display.fillScreen(0xf81f);
    delay(750);
  }
  if (panel_ready) {
    load_endurance();
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
  bool force_redraw = false;
  if (M5.BtnA.wasPressed()) {
    screen_activity_at = now;
    if (screen_awake && focused) cycle_channel=true;
    if (panel_ready && !screen_awake) {
      M5.Display.wakeup();
      M5.Display.setBrightness(screen_brightness);
      screen_awake = true;
      force_redraw = true;
      Serial.printf("PICS yellow wake scans=%lu ms=%lu\n",
                    (unsigned long)scan_cycles, (unsigned long)now);
    }
  }
  if (panel_ready && M5.BtnB.wasPressed()) {
    screen_activity_at=now;
    if (!screen_awake) {
      M5.Display.wakeup();
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
    M5.Display.sleep();
    screen_awake = false;
    Serial.printf("PICS auto display off scans=%lu ms=%lu\n",
                  (unsigned long)scan_cycles, (unsigned long)now);
  }
  bool banner_dirty=false;
  if (panel_ready && screen_awake &&
      (force_redraw || uint32_t(millis() - battery_sample_at) >= 10000))
    banner_dirty=update_battery();
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
