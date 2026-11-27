#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdlib>

namespace wifi_profiles {
constexpr int32_t location_unset=9001;
struct Settings {
  char ssid[33]{};
  char password[64]{};
  char label[33] = "指定地点";
  int32_t lat=location_unset, lon=0;
};
constexpr int capacity=5;
struct Profile { Settings cfg; uint8_t mobile=0; };
struct Book { uint32_t tag=0x57465031; Profile entries[capacity]{}; };

inline bool has_location(const Settings& a) {
  return a.lat>=-9000 && a.lat<=9000 && a.lon>=-18000 && a.lon<=18000;
}
inline bool valid(const Profile& p) {
  const auto& a=p.cfg;
  if (p.mobile>1 || !a.ssid[0] || !a.label[0] || !memchr(a.ssid,0,sizeof(a.ssid)) ||
      !memchr(a.password,0,sizeof(a.password)) || !memchr(a.label,0,sizeof(a.label))) return false;
  const auto n=strlen(a.password);
  return (!n || n>=8) && (has_location(a) || (a.lat==location_unset && a.lon==0));
}
inline int find(const Book& b,const char* ssid) {
  if (!ssid || !ssid[0]) return -1;
  for(int i=0;i<capacity;++i)
    if (b.entries[i].cfg.ssid[0] && !strcmp(b.entries[i].cfg.ssid,ssid)) return i;
  return -1;
}
inline int empty(const Book& b) {
  for(int i=0;i<capacity;++i) if (!b.entries[i].cfg.ssid[0]) return i;
  return -1;
}
inline int count(const Book& b) {
  int n=0;
  for(const auto& p:b.entries) n+=p.cfg.ssid[0]!=0;
  return n;
}
inline bool valid_book(const Book& b) {
  if (b.tag!=Book{}.tag) return false;
  for(int i=0;i<capacity;++i) {
    const auto& p=b.entries[i];
    if (p.cfg.ssid[0] && (!valid(p) || find(b,p.cfg.ssid)!=i)) return false;
  }
  return true;
}
inline bool set(Book& b,int index,const Profile& p) {
  if(index<0 || index>=capacity || !valid(p)) return false;
  const int dup=find(b,p.cfg.ssid);
  if(dup>=0 && dup!=index) return false;
  b.entries[index]=p;
  return true;
}
inline Settings effective(const Book& b,int i) {
  Settings out=b.entries[i].cfg;
  if (b.entries[i].mobile) {
    for (const auto& p:b.entries) if (p.cfg.ssid[0] && !p.mobile && has_location(p.cfg)) {
      out.lat=p.cfg.lat; out.lon=p.cfg.lon;
      memcpy(out.label,p.cfg.label,sizeof(out.label));
      break;
    }
  }
  return out;
}
inline int candidate(const Book& b,const int (&rssi)[capacity],const bool (&tried)[capacity],int preferred) {
  if(preferred>=0 && preferred<capacity && b.entries[preferred].cfg.ssid[0] &&
      !tried[preferred] && rssi[preferred]>-999) return preferred;
  int best=-1;
  for(int i=0;i<capacity;++i) if(!tried[i] && b.entries[i].cfg.ssid[0] && rssi[i]>-999 &&
      (best<0 || rssi[i]>rssi[best])) best=i;
  return best;
}
inline bool coordinates(const char* text,int32_t& lat,int32_t& lon) {
  char *end=nullptr;
  const double a=strtod(text,&end);
  if(end==text || *end!=',') return false;
  const char* part=end+1;
  const double b=strtod(part,&end);
  if(end==part || *end || !std::isfinite(a) || !std::isfinite(b) ||
     a < -90 || a > 90 || b < -180 || b > 180) return false;
  lat=int32_t(std::lround(a*100)); lon=int32_t(std::lround(b*100)); return true;
}
inline void wipe(void* p,size_t n) {
  auto* o=static_cast<volatile unsigned char*>(p);
  while(n--) *o++=0;
}
}
