#pragma once

#include "types.h"

/* Branchless integer select. */
#define isel(a, x, y) ((x & (~(a >> 31))) + (y & (a >> 31)))

/*
 * The ESP32-S3 build uses RGB565 and writes pixels to the ILI9341 in
 * big-endian byte order. CONVERT_COLOR() produces that wire order directly,
 * avoiding a separate framebuffer byte-swap pass.
 */
#define RED_MASK   0xf800
#define GREEN_MASK 0x7e0
#define BLUE_MASK  0x1f

#define RED_EXPAND   3
#define GREEN_EXPAND 2
#define BLUE_EXPAND  3

#define RED_SHIFT   11
#define GREEN_SHIFT 5
#define BLUE_SHIFT  0

#define CONVERT_COLOR(color) \
    (((color & 0x001f) << 3) | \
     ((color & 0x0380) >> 7) | \
     ((color & 0x0060) << 9) | \
     ((color & 0x0200) << 4) | \
     ((color & 0x7c00) >> 2))

/* ESP32-S3 is little-endian. */
#define READ16LE(x)       (*((u16 *)(x)))
#define READ32LE(x)       (*((u32 *)(x)))
#define WRITE16LE(x, v)   (*((u16 *)(x)) = (v))
#define WRITE32LE(x, v)   (*((u32 *)(x)) = (v))

/* Cache prefetch is not enabled in this build. */
#define CACHE_PREFETCH(prefetch)

/* GCC/ESP32-S3 compiler. */
#define FORCE_INLINE inline __attribute__((always_inline))