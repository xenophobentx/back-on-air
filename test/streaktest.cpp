// Offline check of the streak texture rebuild: streaktest <file> <offset of slice 0> <out>
// Reads the 4 filled slices (131072 bytes) of the PC 256x256x16 DXT1 volume at <offset> in <file> (a DDS, a .texs
// or an extracted pack), runs the DLL's StreakInterleaved / StreakRebuild and writes the 16 frames (524288 bytes).
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include "../src/streak_rebuild.h"

int main(int argc, char** argv) {
  if (argc != 4) return fprintf(stderr, "usage: streaktest <file> <offset> <out>\n"), 2;
  FILE* f = fopen(argv[1], "rb");
  if (!f) return perror(argv[1]), 1;
  std::vector<uint8_t> in(4 * kStreakSliceBytes), out(kStreakFrames * kStreakSliceBytes);
  fseek(f, strtol(argv[2], nullptr, 0), SEEK_SET);
  const size_t n = fread(in.data(), 1, in.size(), f);
  fclose(f);
  if (n != in.size()) return fprintf(stderr, "short read %zu\n", n), 1;
  const bool il = StreakInterleaved(in.data());
  const auto t0 = std::chrono::steady_clock::now();
  StreakRebuild(in.data(), out.data());
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  printf("interleaved %s, hash %08x, rebuild %.2f ms\n", il ? "yes" : "no", StreakHash(in.data()), ms);
  FILE* o = fopen(argv[3], "wb");
  if (!o || fwrite(out.data(), 1, out.size(), o) != out.size()) return perror(argv[3]), 1;
  fclose(o);
  return il ? 0 : 3;
}
