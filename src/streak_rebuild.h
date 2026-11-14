// The slipstream streak texture (0x03D53EF3, 256x256x16 DXT1, one mip) rebuilt from the PC release's own data.
// Plain C++, no Windows: the DLL calls it from its d3dx9 hook, test/streaktest.cpp runs it offline.
#pragma once
#include <stdint.h>

constexpr unsigned kStreakW = 256, kStreakH = 256, kStreakFrames = 16;
constexpr unsigned kStreakSliceBytes = (kStreakW / 4) * (kStreakH / 4) * 8;  // 32768

// True when the 4 filled slices (4 x 32768 bytes, slice 0 first) show the PC conversion's interleave: each slice holds
// two half-height frames, block rows 0-31 one and 32-63 the other, with the blank seam (block rows 0, 1, 30-33) fully
// transparent and both halves holding streaks. A correctly laid-out frame has its streaks across that seam.
bool StreakInterleaved(const uint8_t* slices4);

// FNV-1a 32 of the 4 filled slices, for the log (the PC release's data gives one fixed value).
uint32_t StreakHash(const uint8_t* slices4);

// Writes the 16 frames (16 x 32768 bytes, frame 0 first) into out16 from the 4 filled slices.
// Frames 0, 1, 4, 5, 8, 9, 12, 13: the PC data holds their even block rows (slice s rows 0-31 = frame 4s, rows 32-63 =
// frame 4s+1, PC row y = frame block row 2y); those blocks are copied as they are and every odd block row is
// re-encoded from the green channel interpolated linearly between the pixel rows above and below (the streaks are
// vertical bands). Frames 2, 3, 6, 7, 10, 11, 14, 15 are not in the PC data at all; each becomes the mirror image
// (left-right, exact at block level) of the rebuilt frame six later (2 <- 8, 3 <- 9, 6 <- 12, ... 15 <- 5), so the
// 16-frame loop shows 16 different streak patterns as on the console.
void StreakRebuild(const uint8_t* slices4, uint8_t* out16);
