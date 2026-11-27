#pragma once

#include <cstddef>
#include <cstdint>

namespace gallery {
struct Photo {
  const uint8_t* jpeg;
  size_t bytes;
  int16_t width;
  int16_t height;
  const uint8_t* zoom_jpeg;
  size_t zoom_bytes;
  const uint8_t* zoom4_jpeg;
  size_t zoom4_bytes;
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
constexpr int drawer_swipe(int dx,int dy,int x,int y,uint32_t elapsed,int w,int h) {
  if (elapsed>950 || magnitude(dy)<55 || magnitude(dy)*2<magnitude(dx)*3) return 0;
  if (dy<0) return y>=h-83 && magnitude(x-w/2)<w/3 ? 1 : 0;
  return -1;
}
static_assert(drawer_swipe(0,-90,234,445,180,468,468)==1);
static_assert(drawer_swipe(0,-90,234,250,180,468,468)==0);
static_assert(drawer_swipe(90,-20,234,445,180,468,468)==0);
static_assert(drawer_swipe(4,80,234,280,180,468,468)==-1);
constexpr int clamp_value(int value, int low, int high) {
  return value < low ? low : (value > high ? high : value);
}

constexpr bool nearby_tap(int dx, int dy, uint32_t gap_ms) {
  return gap_ms <= 360 && dx * dx + dy * dy <= 44 * 44;
}

constexpr int step_zoom(int current) {
  return current == 1 ? 2 : current == 2 ? 4 : current == 4 ? 8 : 1;
}

static_assert(step_zoom(1) == 2 && step_zoom(2) == 4 &&
              step_zoom(4) == 8 && step_zoom(8) == 1);

constexpr int pan_origin(int current, int delta, int scaled_size, int viewport_size) {
  if (scaled_size <= viewport_size) return (viewport_size - scaled_size) / 2;
  return clamp_value(current + delta, viewport_size - scaled_size, 0);
}

constexpr int zoom_origin(int image_size, int viewport_size, int source_coord, int scale) {
  const int scaled_size = image_size * scale;
  const int aligned = (scaled_size <= viewport_size)
    ? (viewport_size - scaled_size) / 2
    : clamp_value(viewport_size / 2 - source_coord * scale, viewport_size - scaled_size, 0);
  return aligned & ~1;
}
}  // namespace gallery

#if __has_include("photo_gallery.local.h")
#include "photo_gallery.local.h"
#else
namespace gallery {
inline constexpr Photo photos[] = {{nullptr, 0, 0, 0, nullptr, 0, nullptr, 0}};
inline constexpr int photo_count = 0;
}  // namespace gallery
#endif
