// Test aid (not shipped): draws the button-prompt pictures (src/glyph_art.cpp) on the host, to look at them without
// the game. Build: make glyphtest (the host compiler).
//   build/glyphtest <out_dir> [<site> <raw>]...
// For every site and every drawing set it writes <out_dir>/<site>-<set>.ppm (the picture over a grey checkerboard,
// first level, magnified 4x). A raw file is the site's first level as the game holds it (width*height*4 bytes,
// BGRA; taken from the game's own archive by the person running the test, never kept in the repository): the hash
// is checked against the site table and the picture keeps what the game drew outside the slots. Without one the
// site's picture is drawn over transparency.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../src/glyph_art.h"

using namespace ss_glyph;

static void WritePpm(const std::string& path, const Level& L, int mag) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return;
  const int W = static_cast<int>(L.width) * mag, H = static_cast<int>(L.height) * mag;
  fprintf(f, "P6\n%d %d\n255\n", W, H);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const uint8_t* p = &L.rgba[(size_t(y / mag) * L.width + size_t(x / mag)) * 4];
      const int check = ((x / 8) + (y / 8)) & 1 ? 0x60 : 0x90;
      const float a = p[3] / 255.0f;
      unsigned char o[3];
      for (int c = 0; c < 3; ++c) o[c] = static_cast<unsigned char>(p[c] * a + check * (1 - a) + 0.5f);
      fwrite(o, 1, 3, f);
    }
  fclose(f);
}

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <out_dir> [<site> <raw>]...\n", argv[0]);
    return 2;
  }
  int n = 0;
  const Site* sites = Sites(&n);
  int failures = 0;
  for (int i = 0; i < n; ++i) {
    const Site& s = sites[i];
    std::vector<uint8_t> original(size_t(s.width) * s.height * 4, 0);
    for (int a = 2; a + 1 < argc; a += 2) {
      if (strcmp(argv[a], s.name)) continue;
      FILE* f = fopen(argv[a + 1], "rb");
      if (!f || fread(original.data(), 1, original.size(), f) != original.size()) {
        fprintf(stderr, "%s: cannot read %zu bytes from %s\n", s.name, original.size(), argv[a + 1]);
        return 1;
      }
      fclose(f);
      const uint64_t h = Fnv(original.data(), static_cast<int>(s.width * 4), s.width, s.height);
      const bool ok = h == s.hash[0];
      printf("%s: hash %016llX %s\n", s.name, static_cast<unsigned long long>(h), ok ? "matches the table" : "DIFFERS from the table");
      if (!ok) ++failures;
    }
    for (Set set : {kXbox, kNintendo, kSteam, kPlayStation}) {
      if (!Draws(s, set)) continue;
      std::vector<Level> pic = MakePicture(s, set, original, 0);
      if (pic.empty() || pic[0].width != s.width || pic[0].height != s.height) {
        printf("%s %s: no picture\n", s.name, kSetNames[set]);
        ++failures;
        continue;
      }
      std::string name = s.name;
      for (char& c : name)
        if (c == ' ' || c == '(' || c == ')') c = '_';
      WritePpm(std::string(argv[1]) + "/" + name + "-" + kSetNames[set] + ".ppm", pic[0], 4);
      printf("%s %s: %zu levels\n", s.name, kSetNames[set], pic.size());
    }
  }
  return failures ? 1 : 0;
}
