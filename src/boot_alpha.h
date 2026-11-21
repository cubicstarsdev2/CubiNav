// Boot animation decoder for png2frames.py --format alpha4  (BOOT_FRAME_FORMAT 1).
//
// Stored data: per frame a list of runs, one byte each:  [ level:4 | length-1:4 ]
//   level  = darkness 0..15 (0 = transparent/white, 15 = solid black)
//   length = 1..16 source pixels, row-major, runs may continue across row ends.
// The frames are decoded row by row, upscaled by `scale` and ordered-dithered (Bayer) at the OUTPUT
// resolution into a 1-bpp buffer (MSB first, same layout as MonoTileRenderer). The pattern is fixed in screen
// space, so static areas stay identical from frame to frame and the renderer sends nothing for them.
//
// BOOT_DITHER_SIZE = side of the Bayer matrix, any power of two from 2 to 256 (default 4). Set it with
// png2frames.py --dither N (written into boot_frames.h) or override it with -DBOOT_DITHER_SIZE=8.
//   2 = coarse, 5 tones    4 = 17 tones (good default)    8..16 = finest, least visible pattern
//   Bigger sizes (32..256) change only WHICH pixels fill in, not how many: the stored alpha has 4 bits, so the tone
//   steps stay at 16 whatever the size, and the result is hard to tell apart from 8/16.
// BOOT_DITHER_SCALE = size of one dither dot in output pixels (default 1 = finest). 2 makes every dot 2x2 pixels, 3 makes
//   it 3x3, and so on (any integer 1..16): a chunkier, more visible "halftone" look. The matrix size is counted in dots,
//   so with scale 2 a 4x4 matrix spans 8x8 pixels. Set it with png2frames.py --dither-scale N or -DBOOT_DITHER_SCALE=N.
// Cost: the matrix is never stored. A threshold is computed from two small lookup tables, so RAM is
// 2 bytes * BOOT_DITHER_SIZE (512 B at 256).
#pragma once
#include <stdint.h>
#include <string.h>

#ifndef pgm_read_byte
#define pgm_read_byte(a) (*(const uint8_t*)(a))
#endif

namespace bootalpha {

#ifndef BOOT_DITHER_SIZE
#define BOOT_DITHER_SIZE 4
#endif
static_assert(BOOT_DITHER_SIZE >= 2 && BOOT_DITHER_SIZE <= 256 && (BOOT_DITHER_SIZE & (BOOT_DITHER_SIZE - 1)) == 0,
              "BOOT_DITHER_SIZE must be a power of two between 2 and 256");

#ifndef BOOT_DITHER_SCALE
#define BOOT_DITHER_SCALE 2
#endif
static_assert(BOOT_DITHER_SCALE >= 1 && BOOT_DITHER_SCALE <= 16, "BOOT_DITHER_SCALE must be 1..16");

constexpr int DN = BOOT_DITHER_SIZE, DN2 = DN * DN, DS = BOOT_DITHER_SCALE;

// Bayer matrix, closed form: M(x, y) = (SR(x ^ y) << 1) | SR(y), where SR(n) takes bit (k-1-j) of n to bit 2j
// (k = log2 DN). Same values as the recursive M(2n) = 4*M(n) + {{0,2},{3,1}}. Range 0..DN2-1 (up to 65535).
static uint16_t SR[DN];
static bool     srOk = false;

inline void buildSR() {
  int k = 0;
  while ((1 << k) < DN) k++;
  for (int n = 0; n < DN; n++) {
    uint16_t v = 0;
    for (int j = 0; j < k; j++) v |= (uint16_t)(((n >> (k - 1 - j)) & 1) << (2 * j));
    SR[n] = v;
  }
  srOk = true;
}

// out: (srcW*scale/8) bytes per row, srcH*scale rows. srcW <= 256.
inline void decode(const uint8_t* rle, int srcW, int srcH, int scale, uint8_t* out) {
  if (!srOk) buildSR();
  const int outW = srcW * scale, stride = outW / 8;
  uint8_t line[256];
  uint8_t lvl = 0;
  int left = 0;
  for (int sy = 0; sy < srcH; sy++) {
    for (int sx = 0; sx < srcW; sx++) {
      if (!left) { uint8_t b = pgm_read_byte(rle++); lvl = b >> 4; left = (b & 15) + 1; }
      line[sx] = lvl;
      left--;
    }
    for (int r = 0; r < scale; r++) {
      const int dy = sy * scale + r;
      uint8_t* row = out + dy * stride;
      const int y = (dy / DS) & (DN - 1);
      const int srY = SR[y];
      memset(row, 0, stride);
      for (int dx = 0; dx < outW; dx++) {
        const int th = (SR[((dx / DS) & (DN - 1)) ^ y] << 1) | srY;
        // level l (0..15) turns on about l/15 of the DN2 cells: l=0 never, l=15 always
        if (line[dx / scale] * DN2 > th * 15 + 7) row[dx >> 3] |= (uint8_t)(0x80 >> (dx & 7));
      }
    }
  }
}

// Like decode(), but to any output size outW x outH (outW a multiple of 8, outW >= srcW, outH >= srcH): nearest neighbour, so a
// non-integer ratio (64 -> 112 = x1.75) repeats some source pixels twice and others once. The dither is applied at the OUTPUT
// resolution, exactly as in decode(). out: (outW/8) bytes per row, outH rows. srcW <= 256.
inline void decodeSized(const uint8_t* rle, int srcW, int srcH, int outW, int outH, uint8_t* out) {
  if (!srOk) buildSR();
  const int stride = outW / 8;
  uint8_t line[256];
  uint8_t lvl = 0;
  int left = 0, have = -1;                          // `have` = last source row decoded into `line`
  for (int dy = 0; dy < outH; dy++) {
    const int sy = dy * srcH / outH;
    while (have < sy) {                             // runs are row-major, so source rows must be read in order
      for (int sx = 0; sx < srcW; sx++) {
        if (!left) { uint8_t b = pgm_read_byte(rle++); lvl = b >> 4; left = (b & 15) + 1; }
        line[sx] = lvl;
        left--;
      }
      have++;
    }
    uint8_t* row = out + dy * stride;
    const int y = (dy / DS) & (DN - 1);
    const int srY = SR[y];
    memset(row, 0, stride);
    for (int dx = 0; dx < outW; dx++) {
      const int th = (SR[((dx / DS) & (DN - 1)) ^ y] << 1) | srY;
      if (line[dx * srcW / outW] * DN2 > th * 15 + 7) row[dx >> 3] |= (uint8_t)(0x80 >> (dx & 7));
    }
  }
}

}  // namespace bootalpha