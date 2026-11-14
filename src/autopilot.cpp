// autopilot = on (developer / test aid, default off, no overlay row): the game's own AI drives player one's car in a
// race, so a test run (pipeline_warmup records, frame-time runs) drives every lap the same way and reaches the finish.
//
// Found in the decrypted image (the same mechanism the Xbox port's autopilot uses):
//  * The game has a debug switch "game_setup/ai_control_player": a byte at 0xD71944 (registered by 0xB6DE80 through
//    0x487B40, no default written). The control assignment 0x4BD0D0, called by the race update at 0x4B07A8 with
//    (race+0x134, race+0x50, race+0x4C, race+0xE4, race+0x30, race+0x14, ...), reads it for every local player
//    (0x4BD346): when it is set and the race clock (float +8 of race+0xE4) is above 0, the player's car gets the AI
//    driver the rivals have (0x8C3AB0 with 0) instead of the pad. Before the start the clock is 0 and the game keeps
//    the pad. The byte is plain .data: this module sets it while on (no code is written, nothing hooked).
//  * Laps, place and the clock are only read (the HUD's own sources, 0x4C53C0 and 0x4C6420): the player's entity is
//    *(settings + 0xD0 + 4 i) (0x78E490, settings = race+0x14), its component array *(*(world + 0x1C) + 4 entity) + 8
//    (0x4467E0, world = race+0xE4); the lap component [array + 4 + 4 * idx(0xD0321C)]: +4 and +5 set when valid, +8
//    the lap index (0 at the GO, laps at the last line crossing), +0xC the laps of the race; the place component
//    [array + 4 + 4 * idx(0xD0348C)]: +0x10 the place - 1 when +0x15 is set, else first.
// Log lines: "[autopilot] engaged", "lap L/N" (with the lap before's time), "place P" on a change, "finish place P time
// T s" (the race clock at the last crossing).
//
// Power Plays (autopilot_powerplay = on): the AI driving player one never asks for one, so the patch asks with the
// game's own request. Every player entity E has a "TriggeredPowerPlay" logic (vptr 0xBA795C, made by 0x437410, at
// E+0xE0; logic+0x30 = E, +0x38 = 2 when it requested in this update) whose update is vtable slot 4 (.rdata 0xBA796C
// -> 0x436D60, thiscall, one argument). That slot is wrapped (the only change: a .rdata word): after the game's update,
// for player one's logic only ([E+0x118] = 0), while the autopilot drives, at most every 40 updates, the first offered
// candidate whose level is enabled and that the power bar pays for is requested with 0x435060(logic, candidate) (the
// call the game's own trigger paths make, 0x435E15 / 0x43620D), and its cost is taken off the bar. Offers: snapshot
// component [E + 0x80 + 4 idx(0xD02B94)], player block [comp+8] + 0xC + 0x10268 * player; u16 count at +0xFA50 (<= 20),
// candidates 0xC84 bytes each, level u16 at +0xC76. Bar: float +8 of [[E + 0xA4 + 4 idx(0xD03214)] + 0x2C], 0..100;
// level N costs the int at 0xD36BE8 + 8 (N-1) (33 / 66 / 100), allowed by the byte 0xD36C00 + 8 (N-1); level 1 needs
// the bytes 0xD36AF8 and 0xD36AF0, above it 0xD36B00 and 0xD36AF0. Logged: each request (level, bar, clock), and the
// count on the lap and finish lines.
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "ss_shared.h"

namespace {

constexpr uintptr_t kAiControlPlayer = 0xD71944;  // byte: "game_setup/ai_control_player"
constexpr uintptr_t kAiControlRead = 0x4BD346;    // cmp byte ptr [0xD71944], 0 in the control assignment
const uint8_t kAiControlReadSig[] = {0x80, 0x3D, 0x44, 0x19, 0xD7, 0x00, 0x00};
constexpr uintptr_t kAiControlName = 0xBB7C18;
constexpr uintptr_t kRaceRun = 0xD66B28;  // *: the running race
constexpr uint32_t kAiMgrVt = 0xBC0A84;   // race+0xBC: the AI manager (proves the race object)
constexpr uint32_t kRaceSettings = 0x14, kRaceAiMgr = 0xBC, kRaceWorld = 0xE4;
constexpr uint32_t kWorldClock = 8, kWorldComps = 0x1C, kSettingsEntities = 0xD0;
constexpr uintptr_t kLapIdx = 0xD0321C, kPlaceIdx = 0xD0348C;  // component family indices (globals)

bool Get(uintptr_t at, void* out, size_t n) {
  if (at < 0x10000) return false;
  SIZE_T got = 0;
  return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(at), out, n, &got) && got == n;
}
uint32_t U32(uintptr_t at) {
  uint32_t v = 0;
  return Get(at, &v, 4) ? v : 0;
}
uint8_t U8(uintptr_t at) {
  uint8_t v = 0;
  return Get(at, &v, 1) ? v : 0;
}
bool Put8(uintptr_t at, uint8_t v) {
  SIZE_T put = 0;
  return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(at), &v, 1, &put) && put == 1;
}

bool On(const char* key) {
  const std::string v = ss_conf(key);
  return v == "on" || v == "true" || v == "1" || v == "yes";
}

// Player one's component (family index at the global idx), 0 when anything does not look right.
uintptr_t Component(uintptr_t world, uintptr_t comps, uintptr_t idx_global) {
  (void)world;
  const uint32_t idx = U32(idx_global);
  if (!comps || idx > 256) return 0;
  return U32(comps + 4 + 4 * idx);
}

// ---- Power Plays (see the header)
constexpr uintptr_t kPpSlot = 0xBA796C, kPpUpdate = 0x436D60, kPpLogicVt = 0xBA795C, kPpRequest = 0x435060;
constexpr uintptr_t kPpSnapIdx = 0xD02B94, kPpBarIdx = 0xD03214, kPpCost = 0xD36BE8, kPpAllowed = 0xD36C00;
constexpr uintptr_t kPpOnAll = 0xD36AF0, kPpOnL1 = 0xD36AF8, kPpOnL2 = 0xD36B00;
constexpr uint32_t kPpBlock = 0x10268, kPpCount = 0xFA50, kPpCand = 0xC84, kPpLevel = 0xC76;
typedef void(__thiscall* PpUpdateFn)(void*, uint32_t);
typedef bool(__thiscall* PpRequestFn)(void*, const void*);
volatile LONG g_pp_live = 0;   // the autopilot drives a race that has not finished (set by the thread)
volatile LONG g_pp_fired = 0;  // requests the game took this race
volatile float g_pp_clock = 0;
int g_pp_wait = 0;             // updates of player one's logic until the next try (game thread only)
bool g_pp_said = false;
volatile float g_pp_shot_at = -1;  // autopilot_powerplay_shots: a back-buffer capture 1.5 s after the first requests

float* PpBar(uintptr_t e) {
  const uint32_t idx = U32(kPpBarIdx);
  const uintptr_t c = idx < 256 ? U32(e + 0xA4 + 4 * idx) : 0;
  const uintptr_t st = c ? U32(c + 0x2C) : 0;
  float v;
  return st && Get(st + 8, &v, 4) ? reinterpret_cast<float*>(st + 8) : nullptr;
}

void PpTry(void* self) {
  const uintptr_t lg = reinterpret_cast<uintptr_t>(self);
  if (U32(lg) != kPpLogicVt) return;
  const uintptr_t e = U32(lg + 0x30);
  if (!e || U32(e + 0x118) != 0 || U32(lg + 0x38) == 2) return;
  if (g_pp_wait > 0) { --g_pp_wait; return; }
  const uint32_t sidx = U32(kPpSnapIdx);
  const uintptr_t comp = sidx < 256 ? U32(e + 0x80 + 4 * sidx) : 0;
  const uintptr_t snap = comp ? U32(comp + 8) : 0;
  float* bar = PpBar(e);
  if (!snap || !bar) return;
  const uintptr_t block = snap + 0xC;  // player one: index 0
  uint16_t n = 0;
  if (!Get(block + kPpCount, &n, 2) || n == 0 || n > 20) return;
  if (!g_pp_said) { g_pp_said = true; ss_log("[autopilot] power play offers seen (%u now, bar %.0f)", n, *bar); }
  for (uint16_t i = 0; i < n; ++i) {
    const uintptr_t cand = block + i * kPpCand;
    uint16_t level = 0;
    if (!Get(cand + kPpLevel, &level, 2) || level < 1 || level > 3) continue;
    if (!U8(kPpAllowed + 8 * (level - 1)) || !U8(kPpOnAll) || !U8(level == 1 ? kPpOnL1 : kPpOnL2)) continue;
    const int cost = static_cast<int>(U32(kPpCost + 8 * (level - 1)));
    if (cost <= 0 || cost > 100 || *bar + 0.01f < static_cast<float>(cost)) continue;
    const float before = *bar;
    if (!reinterpret_cast<PpRequestFn>(kPpRequest)(self, reinterpret_cast<const void*>(cand))) continue;
    *bar = before - static_cast<float>(cost) < 0 ? 0.0f : before - static_cast<float>(cost);
    const LONG k = InterlockedIncrement(&g_pp_fired);
    ss_log("[autopilot] power play %ld: level %u (cost %d, bar %.0f -> %.0f) at %.2f s", k, level, cost, before, *bar,
           g_pp_clock);
    g_pp_wait = 40;
    if (k <= 2) g_pp_shot_at = g_pp_clock + 1.5f;
    return;
  }
}

void __fastcall PpUpdate(void* self, void*, uint32_t a) {
  reinterpret_cast<PpUpdateFn>(kPpUpdate)(self, a);
  if (g_pp_live) PpTry(self);
}

bool PpInstall() {
  uintptr_t prev = 0;
  if (U32(kPpSlot) != kPpUpdate || !ss_write_word(reinterpret_cast<void*>(kPpSlot), reinterpret_cast<uintptr_t>(&PpUpdate), &prev)) {
    ss_log("[autopilot] power plays: the logic's update slot 0xBA796C is not the known one: none fired");
    return false;
  }
  ss_log("[autopilot] power plays: on (TriggeredPowerPlay update slot wrapped; first offer the bar pays for, then 40 updates' wait)");
  return true;
}

struct Race {
  uintptr_t race = 0;
  bool engaged = false, finished = false;
  int lap = 0, laps = 0, place = 0;
  float lap_start = 0;
};

DWORD WINAPI Thread(void*) {
  Race r;
  bool warned = false;
  int state = -99;
  for (;;) {
    Sleep(20);
    if (ss_game_state != state) {  // the state timeline for the frame-time scripts when pipeline_warmup is off
      ss_log("[autopilot] game state %d -> %d", state, static_cast<int>(ss_game_state));
      state = ss_game_state;
    }
    if (U8(kAiControlPlayer) != 1) Put8(kAiControlPlayer, 1);
    const uintptr_t race = U32(kRaceRun);
    g_pp_live = r.engaged && !r.finished && r.race == race;
    if (race != r.race) {
      if (r.race && r.engaged && !r.finished) ss_log("[autopilot] race left before the finish (lap %d/%d, place %d)", r.lap, r.laps, r.place);
      r = Race();
      r.race = race;
    }
    if (!race || U32(U32(race + kRaceAiMgr)) != kAiMgrVt) continue;
    const uintptr_t world = U32(race + kRaceWorld), settings = U32(race + kRaceSettings);
    float clock = 0;
    if (!world || !settings || !Get(world + kWorldClock, &clock, 4)) continue;
    const uint32_t ent = U32(settings + kSettingsEntities);
    const uintptr_t table = U32(world + kWorldComps);
    const uintptr_t comps = table && ent < 4096 ? U32(table + 4 * ent) : 0;
    const uintptr_t arr = comps ? comps + 8 : 0;
    const uintptr_t lc = Component(world, arr, kLapIdx), pc = Component(world, arr, kPlaceIdx);
    if (!lc) {
      if (!warned && clock > 1) ss_log("[autopilot] player one's lap component not found (race %p)", reinterpret_cast<void*>(race));
      warned = true;
      continue;
    }
    g_pp_clock = clock;
    if (g_pp_shot_at > 0 && clock >= g_pp_shot_at) {
      g_pp_shot_at = -1;
      ss_testshot_ask();
      ss_log("[autopilot] shot asked (power play check) at %.2f s", clock);
    }
    if (!r.engaged && clock > 0) {
      r.engaged = true;
      InterlockedExchange(&g_pp_fired, 0);
      ss_log("[autopilot] engaged (race %p, clock %.2f s)", reinterpret_cast<void*>(race), clock);
    }
    if (!r.engaged || r.finished) continue;
    if (pc) {
      const int place = U8(pc + 0x15) ? static_cast<int>(U32(pc + 0x10)) + 1 : 1;
      if (place >= 1 && place <= 32) r.place = place;
    }
    if (!U8(lc + 4) || !U8(lc + 5)) continue;
    const int laps = static_cast<int>(U32(lc + 0xC)), lap = static_cast<int>(U32(lc + 8)) + 1;
    if (laps < 1 || laps > 99 || lap < 1 || lap > laps + 1) continue;
    r.laps = laps;
    if (lap != r.lap) {
      if (r.lap == 0)
        ss_log("[autopilot] lap %d/%d at %.2f s, place %d", lap, laps, clock, r.place);
      else if (lap <= laps)
        ss_log("[autopilot] lap %d/%d at %.2f s (lap %d took %.2f s), place %d, power plays %ld", lap, laps, clock, r.lap, clock - r.lap_start,
               r.place, g_pp_fired);
      const float start = r.lap_start;
      r.lap = lap;
      r.lap_start = clock;
      if (lap == laps + 1) {
        r.lap_start = start;
        r.finished = true;
        g_pp_live = 0;
        ss_log("[autopilot] finish place %d time %.2f s (last lap %.2f s), power plays %ld", r.place, clock, clock - r.lap_start, g_pp_fired);
      }
    }
  }
  return 0;
}

// The image is decrypted after the DLL loads: the thread waits (up to 2 minutes) for the switch's name and its read.
bool KnownBuild() {
  char name[30] = {};
  uint8_t code[sizeof kAiControlReadSig] = {};
  return Get(kAiControlName, name, sizeof name - 1) && strcmp(name, "game_setup/ai_control_player") == 0 &&
         Get(kAiControlRead, code, sizeof code) && memcmp(code, kAiControlReadSig, sizeof code) == 0;
}

DWORD WINAPI Start(void*) {
  const DWORD t0 = GetTickCount();
  while (!KnownBuild()) {
    if (GetTickCount() - t0 > 120000) {
      ss_log("[autopilot] not the known build (switch name or its read differ after 2 minutes): off");
      return 0;
    }
    Sleep(100);
  }
  ss_log("[autopilot] on: the game's AI drives player one while the race clock runs (game_setup/ai_control_player)");
  if (On("autopilot_powerplay")) PpInstall();
  return Thread(nullptr);
}

}  // namespace

void ss_autopilot_start() {
  if (On("autopilot")) CreateThread(nullptr, 0, Start, nullptr, 0, nullptr);
}
