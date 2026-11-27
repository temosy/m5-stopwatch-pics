#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <esp_sntp.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include "weather_ca.h"
#include "wifi_profiles.h"

namespace local_weather {
using wifi_profiles::Settings;
struct Result {
  time_t epoch=0;
  int8_t profile=-1;
  char label[33]{};
  int temperature_tenths=0, code=-1;
  char time_jst[6]{};
  char forecast_date[11]{};
  int next_min_tenths=0, next_max_tenths=0, next_code=-1;
  enum Status : uint8_t { Idle, Busy, NoNetwork, NoTime, NoForecast, Ready } status=Idle;
};
inline Result result;
inline wifi_profiles::Book input;
inline int preferred_profile=-1;
struct Nearby { char ssid[33]{}; int16_t rssi=-999; bool secured=false; };
constexpr int scan_limit=16;
struct Survey {
  enum State : uint8_t { Idle, Busy, Ready, Failed } status=Idle;
  uint8_t count=0;
  Nearby entries[scan_limit]{};
};
inline Survey survey;
inline portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

inline Result read() {
  portENTER_CRITICAL(&lock);
  const Result copy=result;
  portEXIT_CRITICAL(&lock);
  return copy;
}
inline void publish(const Result& r) {
  portENTER_CRITICAL(&lock);
  result=r;
  portEXIT_CRITICAL(&lock);
}
inline const char* field(const char* data, const char* key) {
  const char* p=strstr(data,key);
  if (!p || !(p=strchr(p,':'))) return nullptr;
  do { ++p; } while (*p==' ' || *p=='\t' || *p=='\n' || *p=='\r');
  return p;
}
inline const char* skip_spaces(const char* s) {
  while (*s==' ' || *s=='\t' || *s=='\n' || *s=='\r') ++s;
  return s;
}
inline const char* second_item(const char* s) {
  if (!s || *s!='[') return nullptr;
  const char* comma=strchr(s,',');
  const char* end=strchr(s,']');
  if (!comma || !end || comma>=end) return nullptr;
  return skip_spaces(comma+1);
}
inline bool forecast_date(time_t utc,char* buffer,size_t capacity) {
  if (utc<1735689600LL) return false;
  const time_t tomorrow=utc+9*3600+86400;
  tm local{};
  return gmtime_r(&tomorrow,&local) &&
         strftime(buffer,capacity,"%Y-%m-%d",&local)==10;
}
inline bool forecast_is_tomorrow(const Result& r,time_t utc) {
  char date[11];
  return r.next_code>=0 && forecast_date(utc,date,sizeof(date)) && strcmp(date,r.forecast_date)==0;
}
inline bool decode_forecast(char* daily,Result& out) {
  if (!daily || *daily!='{') return false;
  char* stop=strchr(daily,'}');
  if (!stop) return false;
  *stop=0;
  const char* when=second_item(field(daily,"\"time\""));
  const char* lo=second_item(field(daily,"\"temperature_2m_min\""));
  const char* hi=second_item(field(daily,"\"temperature_2m_max\""));
  const char* code=second_item(field(daily,"\"weather_code\""));
  char wanted[11];
  bool ok=when && lo && hi && code && strlen(when)>=12 &&
      when[0]=='"' && when[11]=='"' && forecast_date(out.epoch,wanted,sizeof(wanted)) &&
      memcmp(when+1,wanted,10)==0;
  char *le=nullptr,*he=nullptr,*ce=nullptr;
  float minv=0,maxv=0;
  long c=-1;
  if (ok) {
    minv=strtof(lo,&le);
    maxv=strtof(hi,&he);
    c=strtol(code,&ce,10);
    ok=le!=lo && he!=hi && ce!=code &&
      std::isfinite(minv) && std::isfinite(maxv) && minv>=-100 && maxv<=100 &&
      minv<=maxv && c>=0 && c<=99 &&
      *skip_spaces(le)==']' && *skip_spaces(he)==']' && *skip_spaces(ce)==']';
  }
  *stop='}';
  if (!ok) return false;
  out.next_min_tenths=std::lround(minv*10);
  out.next_max_tenths=std::lround(maxv*10);
  out.next_code=int(c);
  memcpy(out.forecast_date,wanted,sizeof(wanted));
  return true;
}
inline bool decode(char* json, Result& out) {
  decode_forecast(const_cast<char*>(field(json,"\"daily\"")),out);
  const char* p=field(json,"\"current\"");
  if (!p || *p!='{') return false;
  char* last=strchr(const_cast<char*>(p),'}');
  if (!last) return false;
  *last=0;
  const char* temp=field(p,"\"temperature_2m\"");
  const char* code=field(p,"\"weather_code\"");
  const char* when=field(p,"\"time\"");
  if (!temp || !code || !when || strlen(when)<18 || when[0]!='"' ||
      when[11]!='T' || when[14]!=':') return false;
  char *tend=nullptr,*cend=nullptr;
  const float t=strtof(temp,&tend);
  const long c=strtol(code,&cend,10);
  if (tend==temp || cend==code || !std::isfinite(t) || t < -100 || t > 100 ||
      c < 0 || c > 99) return false;
  memcpy(out.time_jst,when+12,5);
  out.time_jst[5]=0;
  out.temperature_tenths=std::lround(t*10);
  out.code=int(c);
  return true;
}
inline bool fetch_weather(const Settings& cfg, Result& out) {
  if (!wifi_profiles::has_location(cfg)) return false;
  NetworkClientSecure tls;
  tls.setCACert(weather_ca);
  tls.setHandshakeTimeout(5);
  HTTPClient http;
  char url[365];
  snprintf(url,sizeof(url),
      "https://api.open-meteo.com/v1/forecast?latitude=%.2f&longitude=%.2f"
      "&current=temperature_2m%%2Cweather_code"
      "&daily=weather_code%%2Ctemperature_2m_min%%2Ctemperature_2m_max"
      "&timezone=Asia%%2FTokyo&forecast_days=2",
      cfg.lat/100.0,cfg.lon/100.0);
  http.setConnectTimeout(5500);
  http.setTimeout(5500);
  http.useHTTP10(true);
  if (!http.begin(tls,url)) { Serial.println("PICS weather HTTP begin failed"); return false; }
  http.addHeader("Accept-Encoding","identity");
  const int response=http.GET();
  if (response!=200) {
    Serial.printf("PICS weather HTTP code=%d\n",response);
    http.end(); return false;
  }
  auto* stream=http.getStreamPtr();
  char json[1500];
  const int expected=http.getSize();
  bool finished=false;
  size_t n=0;
  if (stream && expected < int(sizeof(json))) {
    const uint32_t started=millis();
    while (millis()-started<4500 && n+1<sizeof(json)) {
      const int available=stream->available();
      if (available>0) {
        const int got=stream->read(reinterpret_cast<uint8_t*>(json+n),
                  std::min(size_t(available),sizeof(json)-n-1));
        if (got>0) n+=got;
      } else if (!http.connected()) {
        finished=expected<0 || n==size_t(expected); break;
      } else delay(15);
      if (expected>=0 && n==size_t(expected)) { finished=true; break; }
    }
  }
  http.end();
  json[n]=0;
  const bool ok=finished && decode(json,out);
  if (!ok) Serial.printf("PICS weather body complete=%u read=%u expected=%d\n",
                         unsigned(finished),unsigned(n),expected);
  return ok;
}
inline Survey scan_read() {
  portENTER_CRITICAL(&lock);
  const Survey copy=survey;
  portEXIT_CRITICAL(&lock);
  return copy;
}
inline void scan_publish(const Survey& value) {
  portENTER_CRITICAL(&lock);
  survey=value;
  portEXIT_CRITICAL(&lock);
}
inline Survey::State scan_state() {
  portENTER_CRITICAL(&lock);
  const auto state=survey.status;
  portEXIT_CRITICAL(&lock);
  return state;
}
inline bool scanning() { return scan_state()==Survey::Busy; }
inline void init_radio() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
}
inline void stop_radio() {
  WiFi.scanDelete();
  WiFi.disconnect(true,false);
  WiFi.mode(WIFI_OFF);
}
inline bool personal(wifi_auth_mode_t auth) {
  return auth==WIFI_AUTH_OPEN || auth==WIFI_AUTH_WPA_PSK ||
      auth==WIFI_AUTH_WPA2_PSK || auth==WIFI_AUTH_WPA_WPA2_PSK ||
      auth==WIFI_AUTH_WPA3_PSK || auth==WIFI_AUTH_WPA2_WPA3_PSK;
}
inline void survey_worker(void*) {
  Survey r;
  init_radio();
  const auto n=WiFi.scanNetworks(false,false,false,120);
  r.status=n<0 ? Survey::Failed : Survey::Ready;
  for(int i=0;i<n && r.count<scan_limit;++i) {
    const auto auth=WiFi.encryptionType(i);
    const String name=WiFi.SSID(i);
    if (!personal(auth) || !name.length() || name.length()>32) continue;
    bool duplicate=false;
    for(uint8_t j=0;j<r.count;++j)
      if (!strcmp(r.entries[j].ssid,name.c_str())) { duplicate=true; break; }
    if (duplicate) continue;
    auto& dst=r.entries[r.count++];
    memcpy(dst.ssid,name.c_str(),name.length()+1);
    dst.rssi=WiFi.RSSI(i);
    dst.secured=auth!=WIFI_AUTH_OPEN;
  }
  std::sort(r.entries,r.entries+r.count,[](const Nearby& a,const Nearby& b){return a.rssi>b.rssi;});
  stop_radio();
  scan_publish(r);
  vTaskDelete(nullptr);
}
inline bool scan_start() {
  if (scanning() || read().status==Result::Busy) return false;
  Survey r; r.status=Survey::Busy; scan_publish(r);
  if (xTaskCreate(survey_worker,"wifi_scan",6144,nullptr,1,nullptr)==pdPASS) return true;
  r.status=Survey::Failed; scan_publish(r); return false;
}
inline void worker(void*) {
  Result r;
  r.status=Result::NoNetwork;
  init_radio();
  int rssi[wifi_profiles::capacity];
  bool tried[wifi_profiles::capacity]{};
  std::fill_n(rssi,wifi_profiles::capacity,-999);
  const int count=wifi_profiles::count(input);
  if (count==1) {
    for(int i=0;i<wifi_profiles::capacity;++i) if(input.entries[i].cfg.ssid[0]) rssi[i]=0;
  } else {
    const auto n=WiFi.scanNetworks(false,true,false,120);
    for(int i=0;i<n;++i) {
      const auto auth=WiFi.encryptionType(i);
      if(!personal(auth)) continue;
      const String name=WiFi.SSID(i);
      const int idx=wifi_profiles::find(input,name.c_str());
      if(idx>=0 && (auth!=WIFI_AUTH_OPEN)==bool(input.entries[idx].cfg.password[0]))
        rssi[idx]=std::max(rssi[idx],int(WiFi.RSSI(i)));
    }
    WiFi.scanDelete();
    // A saved hidden network may not appear in a survey. Try the last working one once.
    if(preferred_profile>=0 && preferred_profile<wifi_profiles::capacity && rssi[preferred_profile]==-999)
      rssi[preferred_profile]=-200;
  }
  const int prefer_visible=preferred_profile>=0 && preferred_profile<wifi_profiles::capacity &&
      rssi[preferred_profile]>-200 ? preferred_profile : -1;
  for(int attempts=0;attempts<3;++attempts) {
    const int idx=wifi_profiles::candidate(input,rssi,tried,prefer_visible);
    if(idx<0) break;
    tried[idx]=true;
    Settings cfg=wifi_profiles::effective(input,idx);
    // Personal encrypted profiles must not fall back to an open look-alike.
    WiFi.setMinSecurity(cfg.password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN);
    WiFi.begin(cfg.ssid,cfg.password);
    wifi_profiles::wipe(cfg.password,sizeof(cfg.password));
    auto connected=[&] { return WiFi.status()==WL_CONNECTED && WiFi.SSID()==cfg.ssid; };
    for (int i=0;i<(count==1 ? 40 : 22) && !connected();++i) delay(250);
    if (!connected()) { WiFi.disconnect(false,false); continue; }
    r.profile=idx;
    memcpy(r.label,cfg.label,sizeof(r.label));
    r.status=Result::NoTime;
    sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
    configTime(0,0,"ntp.nict.jp");
    for (int i=0;i<(count==1 ? 32 : 18);++i) {
      if (sntp_get_sync_status()==SNTP_SYNC_STATUS_COMPLETED) {
        const time_t utc=time(nullptr);
        if (utc>=1735689600LL && utc<4102416000LL) r.epoch=utc;
        break;
      }
      delay(250);
    }
    esp_sntp_stop();
    if (r.epoch) {
      bool ok=fetch_weather(cfg,r);
      if (!ok && wifi_profiles::has_location(cfg) && connected()) { delay(350); ok=fetch_weather(cfg,r); }
      r.status=ok ? Result::Ready : Result::NoForecast;
      break;
    }
    if (count==1) break;
    WiFi.disconnect(false,false);
    delay(90);
  }
  stop_radio();
  wifi_profiles::wipe(&input,sizeof(input));
  publish(r);
  vTaskDelete(nullptr);
}
inline bool start(const wifi_profiles::Book& cfg,int preferred) {
  if (read().status==Result::Busy || scanning() || !wifi_profiles::count(cfg)) return false;
  input=cfg;
  preferred_profile=preferred;
  Result r;
  r.status=Result::Busy;
  publish(r);
  if (xTaskCreate(worker,"weather",10240,nullptr,1,nullptr)==pdPASS) return true;
  wifi_profiles::wipe(&input,sizeof(input));
  r.status=Result::NoNetwork;
  publish(r);
  return false;
}
inline const char* label_for(int code) {
  if (code==0 || code==1) return "晴れ";
  if (code==2 || code==3) return "くもり";
  if (code==45 || code==48) return "霧";
  if (code>=51 && code<=67) return "雨";
  if ((code>=71 && code<=77) || code==85 || code==86) return "雪";
  if (code>=80 && code<=82) return "にわか雨";
  if (code>=95) return "雷雨";
  return "不明";
}
} // namespace local_weather
