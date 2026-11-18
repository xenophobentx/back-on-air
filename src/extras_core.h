// extras_core.h: the decisions of extras.cpp that need no game (host test: make extrastest).
#pragma once
#include <cstdint>

namespace extras {

// Achievements 41..46 are given only in the results of ranked online races.
constexpr int kOnlineFirst = 41, kOnlineLast = 46;
constexpr int kMaxDecals = 64;

// The next decal id to ask the game for: the first missing one of 1..count-1 (only 41..46 when online_only), then
// 0 (the decal that stands for all the others) once every other one is there. -1: nothing left to ask for.
// skip: ids given up on, one bit each (the game did not take them).
inline int NextDecal(const uint8_t* unlocked, int count, bool online_only, uint64_t skip) {
  if (count < 2 || count > kMaxDecals) return -1;
  bool others_missing = false;
  for (int i = 1; i < count; ++i) {
    if (unlocked[i]) continue;
    others_missing = true;
    if ((skip >> i) & 1) continue;
    if (!online_only || (i >= kOnlineFirst && i <= kOnlineLast)) return i;
  }
  if (!others_missing && !unlocked[0] && !(skip & 1)) return 0;
  return -1;
}

// The decal rows: nothing happens on one press. The first press asks, a second press on the same row within
// kConfirmMs starts (a backup first); a press while it works stops it.
enum Phase { IDLE, CONFIRM, BACKUP, WORKING, DONE, FAILED };
enum Act { ACT_NONE, ACT_ASK, ACT_START, ACT_STOP };
constexpr uint32_t kConfirmMs = 8000;

inline bool ConfirmOpen(Phase phase, uint32_t until, uint32_t now) {
  return phase == CONFIRM && static_cast<int32_t>(until - now) > 0;
}

inline Act OnPress(Phase phase, int cur_mode, uint32_t until, int mode, uint32_t now, bool online) {
  if (phase == BACKUP) return ACT_NONE;  // the backup finishes first
  if (phase == WORKING) return ACT_STOP;
  if (online) return ACT_NONE;
  if (cur_mode == mode && ConfirmOpen(phase, until, now)) return ACT_START;
  return ACT_ASK;
}

}  // namespace extras
