// Host test for padsplit_core.h: make padsplittest
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "../src/padsplit_core.h"

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

using namespace padsplit;

struct Block {            // the two DIJOYSTATE2 snapshots of one game pad block
  int32_t snap[2][0x110 / 4];
  uintptr_t Buf(int i) { return reinterpret_cast<uintptr_t>(snap[i]); }
  int32_t& Z(int i) { return snap[i][kLzOffset / 4]; }
};

// What the virtual pad writes: lZ = LT - RT, halves alone (range +-32767).
static void Fill(Dev& d, Block& b, int i, int lt, int rt) {
  b.Z(i) = lt - rt;
  Record(d, b.Buf(i), lt, -rt, lt - rt);
}

int main() {
  Block b;
  memset(&b, 0, sizeof b);
  Dev d;
  d.dev = &b;
  const uintptr_t bufs[2] = {b.Buf(0), b.Buf(1)};
  Swap sw[2];

  // Full throttle (RT) and a brake press (LT) at once: the combined lZ is 0, the halves are apart.
  Fill(d, b, 0, 32767, 32767);
  Fill(d, b, 1, 20000, 32767);
  CHECK(b.Z(0) == 0 && b.Z(1) == 20000 - 32767, "combined values");
  int n = Apply(d, bufs, kCtlZPos, sw);  // brake: LT alone
  CHECK(n == 2, "brake: both snapshots changed (%d)", n);
  CHECK(b.Z(0) == 32767 && b.Z(1) == 20000, "brake reads LT alone (%d %d)", b.Z(0), b.Z(1));
  Restore(sw, n);
  CHECK(b.Z(0) == 0 && b.Z(1) == 20000 - 32767, "brake: put back (%d %d)", b.Z(0), b.Z(1));
  n = Apply(d, bufs, kCtlZNeg, sw);  // throttle: RT alone
  CHECK(n == 2 && b.Z(0) == -32767 && b.Z(1) == -32767, "throttle reads RT alone (%d %d)", b.Z(0), b.Z(1));
  Restore(sw, n);
  CHECK(b.Z(0) == 0 && b.Z(1) == 20000 - 32767, "throttle: put back");

  // Any other control: nothing changes.
  for (int ctl : {0, 1, 2, 3, 6, 7, 10, 11, 15, -1}) {
    n = Apply(d, bufs, ctl, sw);
    CHECK(n == 0 && b.Z(0) == 0, "control %d left alone", ctl);
  }

  // A snapshot whose lZ is not what this pad wrote is left alone (the other one still split).
  b.Z(0) = 1234;
  n = Apply(d, bufs, kCtlZPos, sw);
  CHECK(n == 1 && b.Z(0) == 1234 && b.Z(1) == 20000, "changed buffer left alone (%d, %d %d)", n, b.Z(0), b.Z(1));
  Restore(sw, n);
  CHECK(b.Z(1) == 20000 - 32767, "the other put back");

  // Put back only while it still holds what was put there.
  Fill(d, b, 0, 100, 200);
  n = Apply(d, bufs, kCtlZNeg, sw);
  CHECK(n == 2 && b.Z(0) == -200, "throttle on new data");
  b.Z(0) = 777;  // something else wrote the buffer meanwhile
  Restore(sw, n);
  CHECK(b.Z(0) == 777 && b.Z(1) == 20000 - 32767, "a value changed in the query is kept (%d)", b.Z(0));

  // Buffers of another pad (or not filled yet) are not touched.
  Block other;
  memset(&other, 0, sizeof other);
  const uintptr_t obufs[2] = {other.Buf(0), other.Buf(1)};
  n = Apply(d, obufs, kCtlZPos, sw);
  CHECK(n == 0, "another pad's buffers untouched");
  Dev fresh;
  n = Apply(fresh, bufs, kCtlZPos, sw);
  CHECK(n == 0, "a pad with no record does nothing");

  // Record keeps the two newest buffers; a third replaces the oldest.
  Dev r;
  Record(r, 0x1000, 1, -1, 0);
  Record(r, 0x2000, 2, -2, 0);
  Record(r, 0x1000, 3, -3, 0);  // seen before: in place
  CHECK(r.snap[0].data == 0x1000 && r.snap[0].plus == 3 && r.snap[1].data == 0x2000, "in place");
  Record(r, 0x3000, 4, -4, 0);
  CHECK(r.snap[0].data == 0x2000 && r.snap[1].data == 0x3000 && r.snap[1].plus == 4, "oldest replaced");

  if (g_fail) {
    printf("%d failed\n", g_fail);
    return 1;
  }
  printf("padsplittest: all passed\n");
  return 0;
}
