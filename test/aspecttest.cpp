// Host test for aspect_core.h: make aspecttest
#include <cstdio>
#include "../src/aspect_core.h"

static int g_fail = 0;
#define CHECK(c, ...)                             \
  do {                                            \
    if (!(c)) {                                   \
      printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                        \
      printf("\n");                               \
      ++g_fail;                                   \
    }                                             \
  } while (0)

using namespace aspectcore;

int main() {
  // 16:9 and 16:10 (the Deck) are never "wider": nothing of the ultrawide extras runs there.
  const int narrow[][2] = {{1920, 1080}, {2560, 1440}, {1280, 720}, {3840, 2160}, {1366, 768}, {1280, 800}, {1920, 1200}, {1024, 768}};
  for (auto& r : narrow) {
    const double a = double(r[0]) / r[1];
    CHECK(!Wider(a), "%dx%d counted as wider than 16:9", r[0], r[1]);
    CHECK(TagScale(a) == 1.0f, "%dx%d tag scale %f", r[0], r[1], TagScale(a));
  }
  const int wide[][2] = {{2560, 1080}, {3440, 1440}, {2560, 720}, {5120, 1440}, {3840, 1080}, {3840, 1440}};
  for (auto& r : wide) {
    const double a = double(r[0]) / r[1];
    CHECK(Wider(a), "%dx%d not counted as wider", r[0], r[1]);
    const float k = TagScale(a);
    CHECK(std::fabs(k * a - kConsole) < 1e-5, "%dx%d tag scale %f", r[0], r[1], k);
  }

  CHECK(SplitHudName("UI/HUD/Hud.gfx"), "Hud.gfx");
  CHECK(SplitHudName("Resident|UI\\HUD\\HUD.GFX"), "HUD.GFX with backslashes");
  CHECK(SplitHudName("UI/HUD/TimerOverlay.gfx"), "TimerOverlay.gfx");
  CHECK(!SplitHudName("UI/HUD/DebugHud.gfx"), "DebugHud.gfx");
  CHECK(!SplitHudName("UI/HUD/DialsPower.gfx"), "DialsPower.gfx");
  CHECK(!SplitHudName("UI/HUD/Hud.gfx2"), "Hud.gfx2");
  CHECK(!SplitHudName(""), "empty");
  CHECK(!SplitHudName(nullptr), "null");

  // 2560x1080 split screen, 16:9 stage (1920 wide): each player's 960-pixel half of the stage in the middle of his
  // 1280-pixel screen half.
  {
    Rect vp, sc;
    const Rect full = {0, 0, 2560, 1080};
    CHECK(HalfView(full, kConsole, 0, vp, sc), "21:9 p0");
    CHECK(vp.l == 160 && vp.w == 1920 && vp.t == 0 && vp.h == 1080, "21:9 p0 viewport %d %d", vp.l, vp.w);
    CHECK(sc.l == 160 && sc.w == 960, "21:9 p0 scissor %d %d", sc.l, sc.w);
    CHECK(HalfView(full, kConsole, 1, vp, sc), "21:9 p1");
    CHECK(vp.l == 1440 - 960 && vp.w == 1920, "21:9 p1 viewport %d %d", vp.l, vp.w);
    CHECK(sc.l == 1440 && sc.w == 960, "21:9 p1 scissor %d %d", sc.l, sc.w);
  }
  // 5120x1440 (32:9): each half is 16:9, the stage's half (1280) in its middle.
  {
    Rect vp, sc;
    const Rect full = {0, 0, 5120, 1440};
    for (int p = 0; p < 2; ++p) {
      CHECK(HalfView(full, kConsole, p, vp, sc), "32:9 p%d", p);
      const int centre = p ? 3840 : 1280;
      CHECK(sc.l == centre - 640 && sc.w == 1280, "32:9 p%d scissor %d %d", p, sc.l, sc.w);
      CHECK(vp.l + p * 1280 == sc.l && vp.w == 2560, "32:9 p%d viewport %d %d", p, vp.l, vp.w);
    }
  }
  // The scissor never leaves the player's screen half; a stage wider than the screen is refused.
  {
    Rect vp, sc;
    const Rect full = {0, 0, 2561, 1080};
    for (int p = 0; p < 2; ++p) {
      CHECK(HalfView(full, kConsole, p, vp, sc), "odd p%d", p);
      const int hl = p ? 1280 : 0, hr = p ? 2561 : 1280;
      CHECK(sc.l >= hl && sc.l + sc.w <= hr, "odd p%d scissor %d %d", p, sc.l, sc.w);
    }
    CHECK(!HalfView({0, 0, 1920, 1080}, 2.0, 0, vp, sc), "stage wider than the screen accepted");
    CHECK(!HalfView({0, 0, 0, 1080}, kConsole, 0, vp, sc), "empty viewport accepted");
  }

  if (g_fail) {
    printf("%d check(s) failed\n", g_fail);
    return 1;
  }
  printf("aspecttest: all checks passed\n");
  return 0;
}
