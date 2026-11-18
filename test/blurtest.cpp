// blurtest: blur_core.h's constant table parser, the blur layout, the tap rule and the shader source
// (motion_blur_quality = high, SSPC-62). The programs here are made up: a version token and a CTAB comment only.
#include "../src/blur_core.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_fail = 0;
void Check(bool ok, const char* what) {
  if (!ok) {
    ++g_fail;
    printf("FAIL %s\n", what);
  }
}

struct Def {
  const char* name;
  uint16_t set, reg, count, cls, elements;
};

// A ps_3_0 program with only a constant table: version, CTAB comment, end.
std::vector<uint32_t> Program(const std::vector<Def>& defs) {
  std::vector<uint8_t> t(28 + 20 * defs.size());
  auto put32 = [&](size_t at, uint32_t v) { memcpy(&t[at], &v, 4); };
  auto put16 = [&](size_t at, uint16_t v) { memcpy(&t[at], &v, 2); };
  auto str = [&](const char* s) {
    const size_t at = t.size();
    t.insert(t.end(), s, s + strlen(s) + 1);
    return static_cast<uint32_t>(at);
  };
  put32(0, 28);
  put32(4, str("test"));
  put32(8, 0xFFFF0300);
  put32(12, static_cast<uint32_t>(defs.size()));
  put32(16, 28);
  put32(20, 0);
  put32(24, str("ps_3_0"));
  for (size_t i = 0; i < defs.size(); ++i) {
    const size_t e = 28 + 20 * i;
    put32(e, str(defs[i].name));
    put16(e + 4, defs[i].set);
    put16(e + 6, defs[i].reg);
    put16(e + 8, defs[i].count);
    while (t.size() % 4) t.push_back(0);
    const uint32_t ti = static_cast<uint32_t>(t.size());
    t.resize(t.size() + 16);
    put16(ti, defs[i].cls);
    put16(ti + 2, defs[i].set == 3 ? 12 : 3);
    put16(ti + 8, defs[i].elements);
    put32(e + 12, ti);
  }
  while (t.size() % 4) t.push_back(0);
  const uint32_t words = static_cast<uint32_t>(t.size() / 4 + 1);
  std::vector<uint32_t> code = {0xFFFF0300u, 0xFFFEu | (words << 16), 0x42415443u};
  for (size_t i = 0; i < t.size(); i += 4) {
    uint32_t v;
    memcpy(&v, &t[i], 4);
    code.push_back(v);
  }
  code.push_back(0x0000FFFFu);
  return code;
}

// The Steam build's layout of the blur programs (as their tables give it).
const std::vector<Def> kGame = {
    {"MB_matMotion360", 2, 96, 128, 2, 32},
    {"MB_motionBlurParams", 2, 94, 1, 1, 1},
    {"MB_motionBlurSourceImage", 3, 10, 1, 4, 1},
    {"MB_viewportParams", 2, 95, 1, 1, 1},
    {"deferred_lighting_DepthTex360", 3, 3, 1, 4, 1},
};

}  // namespace

int main() {
  using namespace mblur;
  CtabConst c[16];
  {
    const std::vector<uint32_t> code = Program(kGame);
    const int n = ParseCtab(code.data(), code.size(), c, 16);
    Check(n == 5, "five entries");
    Check(!strcmp(c[0].name, "MB_matMotion360") && c[0].set == 2 && c[0].reg == 96 && c[0].count == 128 && c[0].cls == 2 &&
              c[0].elements == 32,
          "matrix entry read");
    Check(IsBlurProgram(c, n), "a blur program");
    Layout l;
    const char* why = nullptr;
    Check(LayoutOf(c, n, &l, &why), "layout found");
    Check(l.mat_reg == 96 && l.mats == 32 && l.row_major && l.params_reg == 94 && l.vp_reg == 95 && l.src_sampler == 10 &&
              l.depth_sampler == 3,
          "layout registers");
    Check(l.own_reg == 93, "own register just below the lowest");
    const std::string s = Source(l);
    Check(s.find("row_major float4x4 MBQ_mat[MBQ_N] : register(c96)") != std::string::npos, "source: matrices at c96");
    Check(s.find("register(c93)") != std::string::npos && s.find("register(s10)") != std::string::npos &&
              s.find("register(s3)") != std::string::npos && s.find("#define MBQ_N 32") != std::string::npos,
          "source: own constant, samplers, matrix count");
    Check(s.find("MB_") == std::string::npos, "source: none of the game's names");
    // Cut short: the parser stops at the end of the code.
    Check(ParseCtab(code.data(), 3, c, 16) == 0, "truncated program");
    Check(ParseCtab(code.data(), code.size(), c, 2) == 2, "at most max entries");
  }
  {
    // Another layout (columns, 16 matrices, everything low): read from the table, the own register above.
    const std::vector<Def> other = {
        {"MB_matMotion360", 2, 0, 64, 3, 16},
        {"MB_motionBlurParams", 2, 64, 1, 1, 1},
        {"MB_viewportParams", 2, 65, 1, 1, 1},
        {"MB_motionBlurSourceImage", 3, 0, 1, 4, 1},
        {"SomeDepthTex", 3, 1, 1, 4, 1},
    };
    const std::vector<uint32_t> code = Program(other);
    const int n = ParseCtab(code.data(), code.size(), c, 16);
    Layout l;
    const char* why = nullptr;
    Check(LayoutOf(c, n, &l, &why) && l.mats == 16 && !l.row_major && l.own_reg == 223 && l.depth_sampler == 1,
          "column layout, own register at the top");
    Check(Source(l).find("column_major") != std::string::npos, "source: column_major");
  }
  {
    std::vector<Def> no_depth(kGame.begin(), kGame.end() - 1);
    const std::vector<uint32_t> code = Program(no_depth);
    const int n = ParseCtab(code.data(), code.size(), c, 16);
    Layout l;
    const char* why = nullptr;
    Check(IsBlurProgram(c, n) && !LayoutOf(c, n, &l, &why) && why && strstr(why, "depth"), "no depth: refused with a reason");
    const std::vector<uint32_t> tone = Program({{"SSFilmicToneMapper_Consts", 2, 0, 3, 1, 1}});
    Check(!IsBlurProgram(c, ParseCtab(tone.data(), tone.size(), c, 16)), "another program is not a blur program");
    const uint32_t vs[] = {0xFFFE0300u, 0x0000FFFFu};
    Check(ParseCtab(vs, 2, c, 16) == 0, "a vertex shader has no entries here");
  }
  {
    Check(ParseLevel("game", false) == kLevelGame && ParseLevel("OFF", true) == kLevelGame && ParseLevel("low", false) == kLevelLow &&
              ParseLevel("High", true) == kLevelHigh && ParseLevel("", false) == kLevelHigh && ParseLevel("", true) == kLevelLow &&
              ParseLevel(nullptr, true) == kLevelLow && ParseLevel("medium", false) == kLevelHigh,
          "levels: game | low | high, default high, low on the Deck");
    Check(!strcmp(LevelName(kLevelGame), "game") && !strcmp(LevelName(kLevelLow), "low") && !strcmp(LevelName(kLevelHigh), "high"),
          "level names");
    const TapRange lo = TapsFor(kLevelLow, 2), hi = TapsFor(kLevelHigh, 2), hi_vh = TapsFor(kLevelHigh, 3), lo_vh = TapsFor(kLevelLow, 3);
    Check(lo.min == 6 && lo.cap == 12 && hi.min == 8 && hi.cap == 24, "tap ranges: low 6..12, high 8..24");
    Check(hi_vh.min == 6 && hi_vh.cap == 12 && lo_vh.min == 6 && lo_vh.cap == 12, "QUALITY Very High: high acts as low");
    Check(TapsFor(kLevelHigh, 0).cap == 24 && TapsFor(kLevelGame, 2).cap == 0, "high below Very High; game takes none");
    Check(hi.cap <= kMaxTaps, "high's cap within the shader's loop");
    Check(TapCount(0.2f, hi) == 0 && TapCount(1.0f, hi) == 8 && TapCount(40.0f, hi) == 10 && TapCount(96.0f, hi) == 24 &&
              TapCount(400.0f, hi) == 24,
          "tap count high: px / 4 within 8 .. 24");
    Check(TapCount(0.2f, lo) == 0 && TapCount(1.0f, lo) == 6 && TapCount(28.0f, lo) == 7 && TapCount(40.0f, lo) == 10 &&
              TapCount(400.0f, lo) == 12,
          "tap count low: px / 4 within 6 .. 12");
    Layout l;
    const char* why = nullptr;
    const std::vector<uint32_t> code = Program(kGame);
    const int n = ParseCtab(code.data(), code.size(), c, 16);
    Check(LayoutOf(c, n, &l, &why) && Source(l).find("clamp(floor(px * 0.25), MBQ_own.w, MBQ_own.y)") != std::string::npos,
          "source: one shader, the range from the own register");
    Check(NoiseOffset(-1) == 0.0f && NoiseOffset(64) == 0.0f && NoiseOffset(1) > 5.0f, "noise offset");
  }
  if (g_fail) return 1;
  printf("blurtest: all passed\n");
  return 0;
}
