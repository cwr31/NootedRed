// Renoir VCN 2.2 VP9 axis limits. The current tier-0 path additionally retains
// its validated pixel budget: the full 8192x4352 corner corrupts inter-frame
// chroma with this driver/firmware, even when the reference allocation grows.
// These are coded dimensions, not a promise of a particular frame rate.
#ifndef VCNVP9_LIMITS_H_
#define VCNVP9_LIMITS_H_

#include <stdbool.h>
#include <stdint.h>

#define VCN_VP9_MAX_WIDTH 8192u
#define VCN_VP9_MAX_HEIGHT 4352u
#define VCN_VP9_MAX_PIXELS (8192u * 4320u)

static inline bool vcnvp9_bounds_supported(uint32_t width, uint32_t height) {
  return width > 0 && height > 0 && width <= VCN_VP9_MAX_WIDTH &&
         height <= VCN_VP9_MAX_HEIGHT;
}

static inline bool vcnvp9_dimensions_supported(uint32_t width, uint32_t height) {
  return vcnvp9_bounds_supported(width, height) &&
         (uint64_t)width * height <= VCN_VP9_MAX_PIXELS;
}

#endif
