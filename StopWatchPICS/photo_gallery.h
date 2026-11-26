#pragma once

#include <cstddef>
#include <cstdint>

namespace gallery {
struct Photo {
  const uint8_t* jpeg;
  size_t bytes;
  int16_t width;
  int16_t height;
};

constexpr int next_page(int current, int direction, int photos) {
  return (current + direction + photos + 1) % (photos + 1);
}

constexpr int magnitude(int value) { return value < 0 ? -value : value; }

constexpr int swipe_direction(int dx, int dy, uint32_t elapsed_ms) {
  // Return one page per deliberate horizontal gesture. Left means next.
  return elapsed_ms <= 950 && magnitude(dx) >= 54 &&
         magnitude(dx) * 2 >= magnitude(dy) * 3 ? (dx < 0 ? 1 : -1) : 0;
}
}  // namespace gallery

#if __has_include("photo_gallery.local.h")
#include "photo_gallery.local.h"
#else
namespace gallery {
inline constexpr Photo photos[] = {{nullptr, 0, 0, 0}};
inline constexpr int photo_count = 0;
}  // namespace gallery
#endif
