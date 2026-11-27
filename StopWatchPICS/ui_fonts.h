#pragma once
#include <esp_heap_caps.h>
#include <miniz.h>
#include "ui_type.h"

namespace ui_fonts {
struct Face {
  lgfx::PointerWrapper data;
  lgfx::VLWfont font;
  uint8_t* backing=nullptr;
  const lgfx::IFont* fallback;
  explicit Face(const lgfx::IFont* base):fallback(base) {}
  bool load(const uint8_t* packed,size_t size,size_t expanded) {
    if (backing) return true;
    backing=static_cast<uint8_t*>(heap_caps_malloc(expanded,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if (!backing) return false;
    // The ROM one-shot wrapper places its large decoder on the caller stack.
    auto* decoder=static_cast<tinfl_decompressor*>(
        heap_caps_malloc(sizeof(tinfl_decompressor),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    if (!decoder) { heap_caps_free(backing); backing=nullptr; return false; }
    tinfl_init(decoder);
    size_t consumed=size, produced=expanded;
    const auto status=tinfl_decompress(decoder,packed,&consumed,backing,backing,&produced,
        TINFL_FLAG_PARSE_ZLIB_HEADER|TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    heap_caps_free(decoder);
    if (status!=TINFL_STATUS_DONE || produced!=expanded || consumed!=size) {
      heap_caps_free(backing); backing=nullptr; return false;
    }
    data.set(backing,expanded);
    if (font.loadFont(&data)) return true;
    font.unloadFont();
    heap_caps_free(backing); backing=nullptr;
    return false;
  }
  const lgfx::IFont* get() const { return backing ? static_cast<const lgfx::IFont*>(&font) : fallback; }
  const lgfx::IFont* for_text(const char* s) const {
    if (!backing) return fallback;
    const auto* p=reinterpret_cast<const uint8_t*>(s);
    while (*p) {
      uint16_t c=*p++;
      if (c>=0x80) {
        if (c<0xc2) return fallback;
        if (c<0xe0 && p[0]) { c=((c&0x1f)<<6)|(p[0]&0x3f); p+=1; }
        else if (c<0xf0 && p[0] && p[1]) { c=((c&0xf)<<12)|((p[0]&0x3f)<<6)|(p[1]&0x3f); p+=2; }
        else return fallback;
      }
      uint16_t n=0;
      if (!font.getUnicodeIndex(c,&n)) return fallback;
    }
    return &font;
  }
};
inline Face body(&lgfx::fonts::lgfxJapanGothic_16);
inline Face heading(&lgfx::fonts::lgfxJapanGothicP_24);
inline Face clock(&lgfx::fonts::FreeSansBold24pt7b);
inline Face large(&lgfx::fonts::FreeSansBold24pt7b);
inline void begin() {
  const bool b=body.load(ui_body,sizeof(ui_body),ui_body_length);
  const bool h=heading.load(ui_heading,sizeof(ui_heading),ui_heading_length);
  const bool c=clock.load(ui_clock,sizeof(ui_clock),ui_clock_length);
  const bool l=large.load(ui_large,sizeof(ui_large),ui_large_length);
  Serial.printf("PICS smooth text body=%u heading=%u clock=%u large=%u\n",unsigned(b),unsigned(h),unsigned(c),unsigned(l));
}
} // namespace ui_fonts
