// Host test for extras_core.h: make extrastest
#include <cstdio>
#include <cstring>
#include "../src/extras_core.h"

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

using namespace extras;

int main() {
  uint8_t u[47];
  memset(u, 0, sizeof u);
  // All: 1 first, never 0 while others are missing.
  CHECK(NextDecal(u, 47, false, 0) == 1, "first missing");
  CHECK(NextDecal(u, 47, true, 0) == 41, "online first");
  for (int i = 1; i < 47; ++i) u[i] = 1;
  CHECK(NextDecal(u, 47, false, 0) == 0, "entry 0 last");
  CHECK(NextDecal(u, 47, true, 0) == 0, "online: entry 0 once all others are there");
  u[0] = 1;
  CHECK(NextDecal(u, 47, false, 0) == -1, "all done");
  // Online only with a single-player decal missing: 41..46 only, then nothing (0 not asked).
  memset(u, 0, sizeof u);
  for (int i = 41; i <= 46; ++i) u[i] = 1;
  CHECK(NextDecal(u, 47, true, 0) == -1, "online done, others missing: no entry 0");
  CHECK(NextDecal(u, 47, false, 0) == 1, "all: the single-player ones");
  // Skip: a refused id is passed over, and entry 0 is not asked while one is missing.
  memset(u, 0, sizeof u);
  CHECK(NextDecal(u, 47, false, 1ull << 1) == 2, "skip 1");
  for (int i = 2; i < 47; ++i) u[i] = 1;
  CHECK(NextDecal(u, 47, false, 1ull << 1) == -1, "1 skipped and missing: no entry 0");
  // Bounds.
  CHECK(NextDecal(u, 1, false, 0) == -1, "count 1");
  CHECK(NextDecal(u, 65, false, 0) == -1, "count 65");

  // Presses.
  const uint32_t t = 0xFFFFF000u;  // near the tick wrap
  CHECK(OnPress(IDLE, 0, 0, 1, t, false) == ACT_ASK, "first press asks");
  CHECK(OnPress(CONFIRM, 1, t + kConfirmMs, 1, t + 100, false) == ACT_START, "second press starts");
  CHECK(OnPress(CONFIRM, 1, t + kConfirmMs, 2, t + 100, false) == ACT_ASK, "other row asks again");
  CHECK(OnPress(CONFIRM, 1, t + kConfirmMs, 1, t + kConfirmMs, false) == ACT_ASK, "expired: asks again");
  CHECK(OnPress(CONFIRM, 1, t + kConfirmMs, 1, t + 100, true) == ACT_NONE, "online: nothing");
  CHECK(OnPress(IDLE, 0, 0, 1, t, true) == ACT_NONE, "online: no ask");
  CHECK(OnPress(BACKUP, 1, 0, 1, t, false) == ACT_NONE, "backup running");
  CHECK(OnPress(WORKING, 1, 0, 2, t, false) == ACT_STOP, "working: stop");
  CHECK(OnPress(WORKING, 1, 0, 1, t, true) == ACT_STOP, "working online: stop");
  CHECK(OnPress(DONE, 1, 0, 1, t, false) == ACT_ASK, "done: asks again");
  CHECK(OnPress(FAILED, 1, 0, 1, t, false) == ACT_ASK, "failed: asks again");

  if (g_fail) {
    printf("%d failed\n", g_fail);
    return 1;
  }
  printf("extrastest: all passed\n");
  return 0;
}
