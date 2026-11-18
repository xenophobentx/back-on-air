// Menu key repeat (keyrepeat.cpp): the timing, apart from the game so test/keyrepeattest.cpp can run it on any host.
//
// One call per poll of the game's own keyboard buffer (0x40D2B0, once per input update). For each arrow it says
// whether the arrow's repeat code (VK 0x97..0x9A) is reported down in this poll. The code is down for one poll per
// step, never two polls in a row, so every step is a fresh press for the game's stick timer (it fires on a fresh press
// at once, then would fire again on its own after 0.4 s and on every frame whose time step is 0). The step count is
// then ours alone: none before the arrow has been held kDelayMs, then one every kRateMs at most, one per two polls at
// most at a low frame rate.
//
// An arrow counts as held only while the game's buffer and the physical key (GetAsyncKeyState) both say down: a key
// release the buffer never saw (it reflects the key messages the game's thread took, and a release can go to another
// window or arrive late) cannot keep the repeat going. A gap of more than kGapMs between polls (the game in the
// background, where 0x40D2B0 does not read the keyboard, or a stall) starts every held arrow again from zero.

#pragma once
#include <cstdint>

namespace keyrepeat {

constexpr uint32_t kDelayMs = 400;  // the first repeat step after the press (the game's own stick delay)
constexpr uint32_t kRateMs = 100;   // then one step every 0.1 s (the stick's rate)
constexpr uint32_t kGapMs = 500;    // polls further apart than this: every held arrow starts again

struct Arrow {
  bool held = false;     // down in the last poll
  bool allowed = false;  // the press started in the menus (not driving, the pause or the results)
  bool pulsed = false;   // the code was down in the last poll: this poll reports it up
  uint32_t next = 0;     // the time of the next step
};

struct State {
  Arrow arrow[4];
  uint32_t last = 0;
  bool seen = false;
};

// now: milliseconds (GetTickCount; wraps). buffer / physical: each arrow down in the game's buffer / on the keyboard.
// in_game: the in-game controller is up. Returns bit i set when arrow i's code is down in this poll.
inline unsigned Poll(State& s, uint32_t now, const bool buffer[4], const bool physical[4], bool in_game) {
  const bool gap = !s.seen || now - s.last > kGapMs;
  s.seen = true;
  s.last = now;
  unsigned out = 0;
  for (int i = 0; i < 4; ++i) {
    Arrow& a = s.arrow[i];
    if (!buffer[i] || !physical[i]) {
      a.held = false;
      continue;
    }
    if (!a.held || gap) {  // a new press (or one held across a gap): the game's own edge step only
      a.held = true;
      a.allowed = !in_game;
      a.pulsed = false;
      a.next = now + kDelayMs;
      continue;
    }
    if (!a.allowed) continue;
    if (a.pulsed) {  // up for one poll after each step
      a.pulsed = false;
      continue;
    }
    if (static_cast<int32_t>(now - a.next) < 0) continue;
    out |= 1u << i;
    a.pulsed = true;
    a.next += kRateMs;
    if (static_cast<int32_t>(now - a.next) >= 0) a.next = now + kRateMs;  // late (a slow frame): no catch-up burst
  }
  return out;
}

}  // namespace keyrepeat
