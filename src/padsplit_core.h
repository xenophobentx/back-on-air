// Split triggers (pad.cpp, issue #5): the pure part, apart from the game so test/padsplittest.cpp can run it on any host.
//
// The virtual pad is a wired Xbox 360 pad: both triggers on one Z axis (LT +, RT -), as the game's own Xbox 360 table
// expects (brake = Z+, throttle = Z-). Held together they cancel. The game reads an action bound to a Z half from one
// of the pad's two DIJOYSTATE2 snapshots (lZ at +8; the game fills them in turn, one GetDeviceState each). So each
// GetDeviceState records what it wrote there: the combined lZ and the two halves alone. Around one action query that
// reads Z+ or Z-, the snapshots' lZ is set to that half alone (LT for Z+, RT for Z-) and set back afterwards. A
// snapshot whose lZ is no longer what the record says (the game or another fill changed it) is left alone, and the
// value is put back only while it still holds what was put there.

#pragma once
#include <cstdint>

namespace padsplit {

constexpr int kCtlZPos = 4;          // the game's control code for "Z axis +" (LT on an Xbox 360 pad)
constexpr int kCtlZNeg = 5;          // "Z axis -" (RT)
constexpr uintptr_t kLzOffset = 8;   // lZ in DIJOYSTATE / DIJOYSTATE2

struct Snap {             // one snapshot buffer as GetDeviceState last filled it
  uintptr_t data = 0;     // the buffer's address (0: none yet)
  int32_t plus = 0;       // lZ with LT alone
  int32_t minus = 0;      // lZ with RT alone
  int32_t comb = 0;       // lZ as written (LT - RT)
};

struct Dev {              // one virtual pad
  const void* dev = nullptr;  // the device object the game holds (0: a free record)
  int list_index = -1;        // for the log
  bool logged = false;        // the first split read was logged
  Snap snap[2];               // the two newest buffers, [1] the newest
};

// GetDeviceState filled `data`: remember it. A buffer seen before is updated in place; a new one replaces the oldest.
inline void Record(Dev& d, uintptr_t data, int32_t plus, int32_t minus, int32_t comb) {
  Snap* s = d.snap[1].data == data ? &d.snap[1] : d.snap[0].data == data ? &d.snap[0] : nullptr;
  if (!s) {
    d.snap[0] = d.snap[1];
    s = &d.snap[1];
  }
  s->data = data;
  s->plus = plus;
  s->minus = minus;
  s->comb = comb;
}

struct Swap {             // one lZ changed for a query
  int32_t* where = nullptr;
  int32_t put = 0;        // what was put there
  int32_t was = 0;        // what to put back
};

// Before a query that reads control `ctl` from the snapshot buffers `bufs` of the pad `d`: for Z+ / Z-, each buffer
// this pad filled and whose lZ still holds the combined value gets the half alone. Returns how many were changed
// (0..2, written to `out`); 0 for any other control.
inline int Apply(const Dev& d, const uintptr_t bufs[2], int ctl, Swap out[2]) {
  if (ctl != kCtlZPos && ctl != kCtlZNeg) return 0;
  int n = 0;
  for (int b = 0; b < 2; ++b) {
    for (const Snap& s : d.snap) {
      if (!s.data || s.data != bufs[b]) continue;
      int32_t* z = reinterpret_cast<int32_t*>(bufs[b] + kLzOffset);
      if (*z != s.comb) break;  // not what this pad wrote: left as it is
      out[n].where = z;
      out[n].put = ctl == kCtlZPos ? s.plus : s.minus;
      out[n].was = s.comb;
      *z = out[n].put;
      ++n;
      break;
    }
  }
  return n;
}

// After the query: each changed lZ back to the combined value, only while it still holds what Apply put there.
inline void Restore(const Swap* sw, int n) {
  for (int i = 0; i < n; ++i)
    if (*sw[i].where == sw[i].put) *sw[i].where = sw[i].was;
}

}  // namespace padsplit
