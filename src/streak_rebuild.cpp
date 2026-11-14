// See streak_rebuild.h. Only the green channel matters to the streak shader (red = colour, which is full everywhere,
// green = alpha); the re-encoded blocks keep red at 31 and blue at 0 like the PC data's own endpoints.
#include "streak_rebuild.h"
#include <string.h>

namespace {

constexpr unsigned kBlocksX = kStreakW / 4, kBlockRows = kStreakH / 4;  // 64, 64
constexpr unsigned kRowBytes = kBlocksX * 8;                            // one block row: 512 bytes

int Expand6(int g) { return (g << 2) | (g >> 4); }

// The 16 greens of one DXT1 block, row by row.
void DecodeGreen(const uint8_t* b, int px[16]) {
  const unsigned c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
  const int g0 = Expand6(c0 >> 5 & 63), g1 = Expand6(c1 >> 5 & 63);
  int pal[4] = {g0, g1, 0, 0};
  if (c0 > c1) pal[2] = (2 * g0 + g1 + 1) / 3, pal[3] = (g0 + 2 * g1 + 1) / 3;
  else pal[2] = (g0 + g1) / 2, pal[3] = 0;  // the 3-colour mode's index 3 is black
  const uint32_t idx = b[4] | b[5] << 8 | b[6] << 16 | static_cast<uint32_t>(b[7]) << 24;
  for (int i = 0; i < 16; ++i) px[i] = pal[idx >> (2 * i) & 3];
}

// One DXT1 block for 16 greens (row by row): red 31, blue 0, the two green endpoints searched around the rounded
// minimum and maximum for the least squared error.
void EncodeGreen(const int v[16], uint8_t* out) {
  int lo = 255, hi = 0;
  for (int i = 0; i < 16; ++i) lo = v[i] < lo ? v[i] : lo, hi = v[i] > hi ? v[i] : hi;
  const int qlo = (lo * 63 + 127) / 255, qhi = (hi * 63 + 127) / 255;
  long best = -1;
  int best_hi = 0, best_lo = 0;
  uint32_t best_idx = 0;
  for (int h = qhi - 1; h <= qhi + 1; ++h) {
    for (int l = qlo - 1; l <= qlo + 1; ++l) {
      if (h < 0 || h > 63 || l < 0 || l > 63 || l > h) continue;
      const int g0 = Expand6(h), g1 = Expand6(l);
      const int pal[4] = {g0, g1, (2 * g0 + g1 + 1) / 3, (g0 + 2 * g1 + 1) / 3};
      const int n = h > l ? 4 : 1;  // equal endpoints: index 0 only (c0 == c1 is the 3-colour mode)
      long err = 0;
      uint32_t idx = 0;
      for (int i = 0; i < 16; ++i) {
        int bi = 0, be = 1 << 30;
        for (int k = 0; k < n; ++k) {
          const int d = v[i] - pal[k];
          if (d * d < be) be = d * d, bi = k;
        }
        err += be;
        idx |= static_cast<uint32_t>(bi) << (2 * i);
      }
      if (best < 0 || err < best) best = err, best_hi = h, best_lo = l, best_idx = idx;
    }
  }
  const unsigned c0 = 31u << 11 | best_hi << 5, c1 = 31u << 11 | best_lo << 5;
  out[0] = c0 & 255, out[1] = c0 >> 8, out[2] = c1 & 255, out[3] = c1 >> 8;
  for (int i = 0; i < 4; ++i) out[4 + i] = best_idx >> (8 * i) & 255;
}

// Green of block row `by` of a 4-slice set, pixel rows 4 by .. 4 by + 3 into rows[4][256].
void DecodeBlockRow(const uint8_t* row, int (*rows)[kStreakW]) {
  for (unsigned bx = 0; bx < kBlocksX; ++bx) {
    int px[16];
    DecodeGreen(row + bx * 8, px);
    for (int i = 0; i < 16; ++i) rows[i / 4][bx * 4 + i % 4] = px[i];
  }
}

uint8_t MirrorIndexRow(uint8_t b) { return (b & 3) << 6 | (b >> 2 & 3) << 4 | (b >> 4 & 3) << 2 | (b >> 6 & 3); }

}  // namespace

bool StreakInterleaved(const uint8_t* slices4) {
  static const unsigned kSeam[] = {0, 1, 30, 31, 32, 33};
  for (unsigned s = 0; s < 4; ++s) {
    const uint8_t* sl = slices4 + s * kStreakSliceBytes;
    int rows[4][kStreakW];
    for (unsigned r : kSeam) {
      DecodeBlockRow(sl + r * kRowBytes, rows);
      for (auto& row : rows)
        for (int g : row)
          if (g) return false;
    }
    for (unsigned half = 0; half < 2; ++half) {  // each half holds a frame: some streak at least 1/4 opaque
      int top = 0;
      for (unsigned r = half * 32 + 2; r < half * 32 + 30; ++r) {
        DecodeBlockRow(sl + r * kRowBytes, rows);
        for (auto& row : rows)
          for (int g : row) top = g > top ? g : top;
      }
      if (top < 64) return false;
    }
  }
  return true;
}

uint32_t StreakHash(const uint8_t* slices4) {
  uint32_t h = 2166136261u;
  for (unsigned i = 0; i < 4 * kStreakSliceBytes; ++i) h = (h ^ slices4[i]) * 16777619u;
  return h;
}

void StreakRebuild(const uint8_t* slices4, uint8_t* out16) {
  static int full[kStreakH][kStreakW];  // one frame's green, rebuilt to full height (256 KB, not on the stack)
  for (unsigned s = 0; s < 4; ++s) {
    for (unsigned half = 0; half < 2; ++half) {
      const unsigned frame = 4 * s + half;
      const uint8_t* src = slices4 + s * kStreakSliceBytes + half * 32 * kRowBytes;
      uint8_t* dst = out16 + frame * kStreakSliceBytes;
      for (unsigned y = 0; y < 32; ++y) DecodeBlockRow(src + y * kRowBytes, &full[8 * y]);
      for (unsigned y = 0; y < 32; ++y) {
        const int* a = full[8 * y + 3];
        const int* b = y < 31 ? full[8 * y + 8] : a;  // below the last row: held
        for (int k = 0; k < 4; ++k)
          for (unsigned x = 0; x < kStreakW; ++x) full[8 * y + 4 + k][x] = (a[x] * (4 - k) + b[x] * (k + 1) + 2) / 5;
      }
      for (unsigned y = 0; y < 32; ++y) {
        memcpy(dst + 2 * y * kRowBytes, src + y * kRowBytes, kRowBytes);  // the PC data's own blocks, as they are
        uint8_t* row = dst + (2 * y + 1) * kRowBytes;
        for (unsigned bx = 0; bx < kBlocksX; ++bx) {
          int v[16];
          for (int i = 0; i < 16; ++i) v[i] = full[8 * y + 4 + i / 4][bx * 4 + i % 4];
          EncodeGreen(v, row + bx * 8);
        }
      }
    }
  }
  for (unsigned f = 0; f < kStreakFrames; ++f) {
    if (f % 4 < 2) continue;
    const uint8_t* src = out16 + ((f + 6) % kStreakFrames) * kStreakSliceBytes;
    uint8_t* dst = out16 + f * kStreakSliceBytes;
    for (unsigned by = 0; by < kBlockRows; ++by) {
      for (unsigned bx = 0; bx < kBlocksX; ++bx) {
        const uint8_t* s = src + by * kRowBytes + (kBlocksX - 1 - bx) * 8;
        uint8_t* d = dst + by * kRowBytes + bx * 8;
        memcpy(d, s, 4);
        for (int i = 0; i < 4; ++i) d[4 + i] = MirrorIndexRow(s[4 + i]);
      }
    }
  }
}
