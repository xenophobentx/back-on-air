// Host test for keyrepeat_core.h: make keyrepeattest
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include "../src/keyrepeat_core.h"

static int g_fail = 0;
#define CHECK(c, ...)                         \
  do {                                        \
    if (!(c)) {                               \
      printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                    \
      printf("\n");                           \
      ++g_fail;                               \
    }                                         \
  } while (0)

struct Run {
  int steps = 0;        // repeat steps (codes reported down)
  int consecutive = 0;  // polls where the code was down in two polls in a row
};

// One arrow (down) over [t0, t1) of polls every `frame` ms. buffer(t) / physical(t) say whether it is down.
template <class B, class P>
static Run Sim(keyrepeat::State& s, uint32_t t0, uint32_t t1, uint32_t frame, B buffer, P physical, bool in_game = false) {
  Run r;
  bool was = false;
  for (uint32_t t = t0; static_cast<int32_t>(t1 - t) > 0; t += frame) {
    bool b[4] = {false, false, false, buffer(t)};
    bool p[4] = {false, false, false, physical(t)};
    const bool on = (keyrepeat::Poll(s, t, b, p, in_game) & 8) != 0;
    if (on) ++r.steps;
    if (on && was) ++r.consecutive;
    was = on;
  }
  return r;
}

int main() {
  const uint32_t base = 100000;
  auto hold = [](uint32_t from, uint32_t to) { return [=](uint32_t t) { return t - from < to - from; }; };

  for (uint32_t frame : {1u, 8u, 16u, 33u, 100u, 200u, 250u, 499u}) {
    keyrepeat::State s;
    Run r = Sim(s, base, base + 3000, frame, hold(base + 200, base + 350), hold(base + 200, base + 350));
    CHECK(r.steps == 0, "150 ms tap at %u ms/frame: %d repeat steps", frame, r.steps);
  }

  {  // 2.6 s hold at 60 fps: steps at 0.4 s, then every 0.1 s (plus the game's own edge step)
    keyrepeat::State s;
    Run r = Sim(s, base, base + 5000, 16, hold(base + 1000, base + 3600), hold(base + 1000, base + 3600));
    CHECK(r.steps >= 21 && r.steps <= 23, "2.6 s hold at 16 ms: %d steps", r.steps);
    CHECK(r.consecutive == 0, "2.6 s hold: code down in %d consecutive polls", r.consecutive);
  }

  for (uint32_t frame : {1u, 50u, 150u, 300u}) {  // never two polls in a row, whatever the frame time
    keyrepeat::State s;
    Run r = Sim(s, base, base + 4000, frame, hold(base, base + 3000), hold(base, base + 3000));
    CHECK(r.consecutive == 0, "hold at %u ms/frame: %d consecutive", frame, r.consecutive);
    CHECK(r.steps <= 27, "hold at %u ms/frame: %d steps (more than 10 a second)", frame, r.steps);
    CHECK(frame > 150 || r.steps >= 8, "hold at %u ms/frame: only %d steps", frame, r.steps);
  }

  {  // the buffer keeps the arrow down after the release (a missed or late key-up): stops with the physical key
    keyrepeat::State s;
    Run r = Sim(s, base, base + 10000, 16, [](uint32_t) { return true; }, hold(base, base + 150));
    CHECK(r.steps == 0, "stale buffer after a 150 ms tap: %d steps", r.steps);
    keyrepeat::State s2;
    Run r2 = Sim(s2, base, base + 10000, 16, [](uint32_t) { return true; }, hold(base, base + 1000));
    CHECK(r2.steps >= 6 && r2.steps <= 7, "stale buffer after a 1 s hold: %d steps", r2.steps);
  }

  {  // a gap (the game in the background) with the arrow held: no step at once after it
    keyrepeat::State s;
    auto down = [](uint32_t) { return true; };
    Sim(s, base, base + 1000, 16, down, down);  // held, repeating
    Run r = Sim(s, base + 3000, base + 3300, 16, down, down);
    CHECK(r.steps == 0, "after a 2 s gap: %d steps within 0.3 s", r.steps);
  }

  {  // a press made in the game proper never repeats; one made in the menus later does
    keyrepeat::State s;
    Run r = Sim(s, base, base + 3000, 16, hold(base, base + 2000), hold(base, base + 2000), true);
    CHECK(r.steps == 0, "in-game hold: %d steps", r.steps);
    Run r2 = Sim(s, base + 3000, base + 5000, 16, hold(base + 3000, base + 4000), hold(base + 3000, base + 4000));
    CHECK(r2.steps >= 6, "menu hold after the race: %d steps", r2.steps);
  }

  {  // GetTickCount wrapping during a hold
    keyrepeat::State s;
    const uint32_t w = 0xFFFFFFFFu - 1000;
    Run r = Sim(s, w, w + 3600, 16, hold(w + 1000, w + 3600), hold(w + 1000, w + 3600));
    CHECK(r.steps >= 21 && r.steps <= 23, "hold across the wrap: %d steps", r.steps);
  }

  if (g_fail) {
    printf("keyrepeattest: %d failed\n", g_fail);
    return 1;
  }
  printf("keyrepeattest: ok\n");
  return 0;
}
