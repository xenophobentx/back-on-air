// extras.cpp: unlock_all, bonus_content, pure_decal and the two decal actions (SSPC-55).
//
// The addresses and what they mean were found by Glimpse-Fix (frankyfife, GPL-3.0). No code of theirs is used here,
// only these facts (Steam build, run-time VAs):
// * Four UI switches the menus read whenever they ask "is this locked", as the game's QA menu sets them: all game
//   modes 0xD6FC54, all episodes 0xD6FBA0, all events 0xD7D978, all vehicles 0xD6F8F0 (one byte each, .data).
// * The PC port's cheat-code extras ("PCcheat" switches: the Expressway track, the Cobretti SlipStream, the retailer
//   cars, the Euro livery pack): ten bytes from 0xD6C49C, 8 bytes apart (.data).
// * The Pure decal: the profile's HasPureSave byte at *(0xD64E6C) (heap); the game sets it when it finds a Pure
//   save and clears it again when it looks for one.
// * Decals are the achievements: the object at *(0xD64E00) + 0x208 (vptr 0xC4AAC4; bytes +4 and +0x3C non-zero
//   with a signed-in profile) keeps one byte per achievement at *(+0x2C), the count (47) at +0x38. Entry 0 stands
//   for all the others; 41..46 are given only by ranked online races. The game's own debug unlock: the byte switch
//   0xD71954 and the id 0xD11B6C (.data), read by the single-player race update, which clears the switch and
//   unlocks through the game's normal path; the profile is saved with it when the race ends.
//
// What this file does. Only data is written (the .data bytes and the profile's Pure byte, from patch.cpp's Present
// hook, on the game's thread between frames); nothing in the game's code, and never the save file itself.
// * unlock_all / bonus_content / pure_decal (default false, live): while on, each of their bytes that holds 0 is
//   set to 1 and remembered as ours; when off, each byte we set is put back to 0. So off gives the game's own values
//   back, and the save is not touched by the switch itself. What is played meanwhile is saved by the game as usual
//   (a race won in a locked event counts). So turning one on first makes a kept save backup (saves.cpp,
//   save-backups\before-unlock-<date>; not pruned), and the bytes are set only once that backup is there. When the
//   backup fails the switch stays off and the overlay says so. When the last one goes off again, the save is
//   compared with that backup and the log says whether the game wrote it meanwhile.
// * Online: from the game's online sign-in (the online setup object *(0xD98614): +0x18 signed in, +0x21C busy) or
//   a network game mode (*(0xD64E18) +8 = 2 or 3), all three are held off: our bytes go back to 0 and stay so
//   until the game is offline again. A car the career has not given is never ours online.
// * The decal rows (actions, no conf key: a conf line must never change a save by itself): the first press asks,
//   a second press within 8 s on the same row confirms. Then a kept backup (save-backups\before-decals-<date>) is
//   made; if it cannot be made, nothing happens. Then, in a single-player race (one local player, not online, a
//   signed-in profile, the achievement object's vptr and a count of 2..64), one decal a frame is asked for through
//   the game's switch: the next one only after the game has cleared the switch; an id the game does not take in 3
//   tries is skipped. Online the switch is cleared and the work waits. A press while it works stops it.
// The exe was checked as the known Steam build at start (patch.cpp); here every byte must hold 0 or 1 and its page
// must be writable, or nothing is ever written.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "extras_core.h"
#include "menu.h"
#include "ss_shared.h"

namespace {

constexpr uintptr_t kTextEnd = 0xB9B000;  // the exe's code ends here (and its import table starts)
constexpr uintptr_t kUnlockAll[] = {0xD6FC54, 0xD6FBA0, 0xD7D978, 0xD6F8F0};
constexpr uintptr_t kBonus = 0xD6C49C;
constexpr int kBonusCount = 10, kBonusStride = 8;
constexpr uintptr_t kPureFlagPtr = 0xD64E6C;
constexpr uintptr_t kAchOwner = 0xD64E00, kAchInOwner = 0x208;
constexpr uint32_t kAchVt = 0xC4AAC4;
constexpr uintptr_t kAskSwitch = 0xD71954;  // byte
constexpr uintptr_t kAskId = 0xD11B6C;      // int
constexpr uintptr_t kModeObj = 0xD64E18;    // +8: 2 or 3 = a network mode (patch.cpp's takedown check reads it too)
constexpr uintptr_t kOnlineSetup = 0xD98614;  // online_login.cpp: +0x18 signed in, +0x21C 1 = busy
constexpr uintptr_t kController = 0xD66B24;   // patch.cpp: the in-game controller; +0x14 the race
constexpr uint32_t kControllerVt = 0xBB642C, kAiMgrVt = 0xBC0A84;
static_assert(kAskSwitch > kTextEnd && kAskId > kTextEnd && kBonus > kTextEnd && kUnlockAll[0] > kTextEnd &&
                  kUnlockAll[1] > kTextEnd && kUnlockAll[2] > kTextEnd && kUnlockAll[3] > kTextEnd,
              "data only: never the game's code");

constexpr char kUnlockPrefix[] = "before-unlock-";
constexpr char kDecalPrefix[] = "before-decals-";

bool g_on = false;
bool g_checked = false, g_bad = false;  // the bytes were checked once; bad: never written
bool g_online = false, g_in_race = false;
unsigned g_frame = 0;

bool Readable(uintptr_t p, size_t n) {
  if (!p) return false;
  if (ss_region_known(p, n)) return true;
  if (ss_region_bad(p)) return false;
  MEMORY_BASIC_INFORMATION mi;
  if (!SS_VQ(reinterpret_cast<const void*>(p), &mi, sizeof mi)) return false;
  if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || mi.Protect == 0) {
    ss_region_note_bad(reinterpret_cast<uintptr_t>(mi.BaseAddress), mi.RegionSize);
    return false;
  }
  ss_region_note(reinterpret_cast<uintptr_t>(mi.BaseAddress), mi.RegionSize, mi.Type);
  return p + n <= reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize;
}

bool Writable(uintptr_t p) {
  MEMORY_BASIC_INFORMATION mi;
  if (!SS_VQ(reinterpret_cast<const void*>(p), &mi, sizeof mi) || mi.State != MEM_COMMIT) return false;
  const DWORD w = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
  return (mi.Protect & w) && !(mi.Protect & PAGE_GUARD);
}

inline uint32_t Rd(uintptr_t va) { return *reinterpret_cast<const volatile uint32_t*>(va); }
inline uint8_t Rd8(uintptr_t va) { return *reinterpret_cast<const volatile uint8_t*>(va); }
inline void Wr8(uintptr_t va, uint8_t v) { *reinterpret_cast<volatile uint8_t*>(va) = v; }

// ------------------------------------------------------------------------------------------- the background job

// One file job at a time on its own thread (a backup or a compare: a few small files).
enum JobKind { JOB_NONE, JOB_BACKUP_UNLOCK, JOB_BACKUP_DECALS, JOB_COMPARE };
volatile LONG g_job_state = 0;  // 0 free, 1 running, 2 done (the present hook takes the result and frees it)
JobKind g_job = JOB_NONE;
int g_job_result = 0;
std::string g_job_where;

DWORD WINAPI JobThread(void*) {
  if (g_job == JOB_COMPARE) {
    g_job_result = ss_saves_same_as_kept(kUnlockPrefix);
  } else {
    g_job_result = ss_saves_backup_kept(g_job == JOB_BACKUP_DECALS ? kDecalPrefix : kUnlockPrefix, &g_job_where);
  }
  InterlockedExchange(&g_job_state, 2);
  return 0;
}

bool StartJob(JobKind kind) {
  if (InterlockedCompareExchange(&g_job_state, 1, 0) != 0) return false;
  g_job = kind;
  g_job_result = 0;
  g_job_where.clear();
  HANDLE h = CreateThread(nullptr, 0, JobThread, nullptr, 0, nullptr);
  if (!h) {
    g_job_result = SAVES_KEPT_FAILED;
    InterlockedExchange(&g_job_state, 2);
    return true;
  }
  CloseHandle(h);
  return true;
}

// ------------------------------------------------------------------------------------------- the switches

enum Backup { BK_NONE, BK_NEEDED, BK_RUNNING, BK_OK, BK_FAILED };

struct Switch {
  const char* key;
  const char* what;  // for the log
  int n;             // bytes (pure: 1, behind the pointer)
  bool pure;
  uintptr_t at[kBonusCount];
  bool owned[kBonusCount];
  bool wanted, active;
  Backup bk;
  uintptr_t pure_at;  // pure: the profile byte we set
};

Switch g_sw[3] = {
    {"unlock_all", "events, tracks, modes and cars", 4, false, {}, {}, false, false, BK_NONE, 0},
    {"bonus_content", "the cheat-code extras", kBonusCount, false, {}, {}, false, false, BK_NONE, 0},
    {"pure_decal", "the Pure decal", 1, true, {}, {}, false, false, BK_NONE, 0},
};
bool g_compare = false;  // compare the save with the backup when the job is free

Switch* Find(const char* key) {
  for (Switch& s : g_sw)
    if (!strcmp(s.key, key)) return &s;
  return nullptr;
}

bool AnyWanted() {
  for (const Switch& s : g_sw)
    if (s.wanted) return true;
  return false;
}

bool IsOn(const std::string& v) { return v == "true" || v == "on" || v == "1"; }

// Once: every byte holds 0 or 1 and can be written. Anything else is not the build these addresses are for.
void CheckBytes() {
  g_checked = true;
  for (const Switch& s : g_sw) {
    if (s.pure) continue;
    for (int i = 0; i < s.n; ++i) {
      if (!Readable(s.at[i], 1) || !Writable(s.at[i]) || Rd8(s.at[i]) > 1) {
        g_bad = true;
        ss_log("[extras] %s: the byte at %p holds %u or cannot be written: not the known build, the unlock switches "
               "do nothing",
               s.key, reinterpret_cast<void*>(s.at[i]), Readable(s.at[i], 1) ? Rd8(s.at[i]) : 0xFFu);
        return;
      }
    }
  }
  if (!Writable(kAskSwitch) || !Writable(kAskId) || Rd8(kAskSwitch) > 1) {
    g_bad = true;
    ss_log("[extras] the decal switch at %p cannot be used: not the known build, the unlock switches do nothing",
           reinterpret_cast<void*>(kAskSwitch));
  }
}

// Puts the switch's bytes to work (on) or gives the game's values back (off). Returns the bytes changed.
int Enforce(Switch& s, bool on) {
  int changed = 0;
  if (s.pure) {
    const uintptr_t p = Rd(kPureFlagPtr);
    const bool same = p && p == s.pure_at && Readable(p, 1);
    if (!on) {
      if (same && Rd8(p) == 1) {
        Wr8(p, 0);
        ++changed;
      }
      s.pure_at = 0;
    } else if (p && Readable(p, 1) && Rd8(p) == 0) {  // the game clears it when it looks for a Pure save: set again
      Wr8(p, 1);
      if (p != s.pure_at) ++changed;
      s.pure_at = p;
    }
    return changed;
  }
  for (int i = 0; i < s.n; ++i) {
    const uint8_t v = Rd8(s.at[i]);
    if (on && v == 0) {
      Wr8(s.at[i], 1);
      if (!s.owned[i]) ++changed;
      s.owned[i] = true;
    } else if (!on && s.owned[i]) {
      if (v == 1) Wr8(s.at[i], 0);
      s.owned[i] = false;
      ++changed;
    }
  }
  return changed;
}

void SwitchesTick() {
  for (Switch& s : g_sw) {
    const bool want = s.wanted && s.bk == BK_OK && !g_online && !g_bad;
    const int changed = Enforce(s, want);
    if (want == s.active) continue;
    s.active = want;
    if (want) {
      ss_log("[extras] %s on: %s shown as unlocked (%d switches set)", s.key, s.what, changed);
    } else {
      ss_log("[extras] %s %s: %d switches back to the game's values", s.key,
             s.wanted && g_online ? "held off while online" : "off", changed);
    }
  }
}

bool ApplySwitch(const char* key, const char* value) {
  Switch* s = Find(key);
  if (!s) return false;
  const bool on = IsOn(value);
  if (on && !s->wanted) s->bk = BK_NEEDED;  // each turn on: a backup of the save as it is now
  const bool was_ok = s->wanted && s->bk == BK_OK;
  s->wanted = on;
  if (!on) {
    s->bk = BK_NONE;
    if (was_ok && !AnyWanted()) g_compare = true;
  }
  return true;
}

// ------------------------------------------------------------------------------------------- the decals

extras::Phase g_phase = extras::IDLE;
int g_mode = 0;  // 1: the online decals, 2: all
uint32_t g_until = 0;
bool g_decal_backup_needed = false;
int g_asked = -1;
int g_tries[extras::kMaxDecals];
uint64_t g_skip = 0;
int g_given = 0;
bool g_switch_ours = false;
bool g_paused_logged = false;

const char* ModeName(int mode) { return mode == 1 ? "the online decals" : "all decals"; }

// A single-player race of one local player is running (the same reads as patch.cpp's takedown check).
bool SoloRace() {
  const uintptr_t c = Rd(kController);
  if (!c || !Readable(c, 0x18) || Rd(c) != kControllerVt) return false;
  const uintptr_t race = Rd(c + 0x14);
  if (!race || !Readable(race, 0xD0)) return false;
  const uintptr_t ai = Rd(race + 0xBC);
  if (!ai || !Readable(ai, 4) || Rd(ai) != kAiMgrVt) return false;
  const uintptr_t settings = Rd(race + 0x14);
  return settings && Readable(settings + 0x9C, 4) && Rd(settings + 0x9C) == 1;
}

// The achievement object with a signed-in profile, its bytes and count; 0 when anything does not look right.
uintptr_t Achievements(const uint8_t** bytes, int* count) {
  const uintptr_t mgr = Rd(kAchOwner);
  if (!mgr) return 0;
  const uintptr_t ach = mgr + kAchInOwner;
  if (!Readable(ach, 0x40) || Rd(ach) != kAchVt || !Rd8(ach + 4) || !Rd8(ach + 0x3C)) return 0;
  const uintptr_t un = Rd(ach + 0x2C);
  const int n = static_cast<int>(Rd(ach + 0x38));
  if (n < 2 || n > extras::kMaxDecals || !Readable(un, static_cast<size_t>(n))) return 0;
  *bytes = reinterpret_cast<const uint8_t*>(un);
  *count = n;
  return ach;
}

void ClearOurSwitch() {
  if (g_switch_ours && Rd8(kAskSwitch) == 1) Wr8(kAskSwitch, 0);
  g_switch_ours = false;
  g_asked = -1;  // not taken: asked again later, not counted as a try
}

void DecalsTick() {
  if (g_phase != extras::WORKING) return;
  if (g_online) {
    ClearOurSwitch();
    if (!g_paused_logged) ss_log("[extras] decals: online, waiting until the game is offline again");
    g_paused_logged = true;
    return;
  }
  g_paused_logged = false;
  if (!g_in_race) return;
  const uint8_t* un = nullptr;
  int count = 0;
  if (!Achievements(&un, &count)) return;
  if (Rd8(kAskSwitch)) return;  // the game has not taken the last request yet
  if (g_asked >= 0) {
    if (un[g_asked]) {
      ++g_given;
      ss_log("[extras] decal %d unlocked", g_asked);
    } else if (++g_tries[g_asked] >= 3) {
      g_skip |= 1ull << g_asked;
      ss_log("[extras] decal %d: the game did not take it in 3 tries; skipped", g_asked);
    }
    g_asked = -1;
    g_switch_ours = false;
  }
  const int next = extras::NextDecal(un, count, g_mode == 1, g_skip);
  if (next < 0) {
    int have = 0;
    for (int i = 0; i < count; ++i) have += un[i] != 0;
    g_phase = extras::DONE;
    ss_log("[extras] decals done (%s): %d given now, %d of %d unlocked; the game saves them with the profile when "
           "the race ends",
           ModeName(g_mode), g_given, have, count);
    return;
  }
  *reinterpret_cast<volatile int32_t*>(kAskId) = next;
  Wr8(kAskSwitch, 1);
  g_switch_ours = true;
  g_asked = next;
}

bool DecalPress(int mode) {
  const uint32_t now = GetTickCount();
  switch (extras::OnPress(g_phase, g_mode, g_until, mode, now, g_online)) {
    case extras::ACT_ASK:
      g_phase = extras::CONFIRM;
      g_mode = mode;
      g_until = now + extras::kConfirmMs;
      ss_log("[extras] decals (%s): asked to confirm", ModeName(mode));
      break;
    case extras::ACT_START:
      if (g_bad) {
        g_phase = extras::FAILED;
        ss_log("[extras] decals: not the known build, nothing done");
        break;
      }
      g_phase = extras::BACKUP;
      g_decal_backup_needed = true;
      ss_log("[extras] decals (%s): confirmed, a save backup first", ModeName(mode));
      break;
    case extras::ACT_STOP:
      ClearOurSwitch();
      g_phase = extras::IDLE;
      ss_log("[extras] decals (%s): stopped by the player, %d given", ModeName(g_mode), g_given);
      break;
    case extras::ACT_NONE:
      if (g_online) ss_log("[extras] decals: not available while online");
      break;
  }
  return true;
}

bool PressOnline(const char*, const char*) { return DecalPress(1); }
bool PressAll(const char*, const char*) { return DecalPress(2); }

const char* DecalStatus(int mode) {
  static char buf[48];
  const bool busy = g_phase == extras::BACKUP || g_phase == extras::WORKING;
  if (g_mode != mode) return busy ? "BUSY" : g_online ? "NOT ONLINE" : "START";
  switch (g_phase) {
    case extras::CONFIRM:
      if (extras::ConfirmOpen(g_phase, g_until, GetTickCount())) return "PRESS AGAIN TO CONFIRM";
      break;
    case extras::BACKUP:
      return "MAKING A BACKUP";
    case extras::WORKING:
      return g_online ? "PAUSED ONLINE" : g_in_race ? "UNLOCKING" : "IN THE NEXT RACE";
    case extras::DONE:
      // The game writes them only at CONTINUE on a finished race's results; the value box fits ~38 characters.
      snprintf(buf, sizeof buf, "%d GIVEN, FINISH A RACE TO SAVE", g_given);
      return buf;
    case extras::FAILED:
      return "BACKUP FAILED";
    default:
      break;
  }
  return g_online ? "NOT ONLINE" : "START";
}

const char* StatusOnline() { return DecalStatus(1); }
const char* StatusAll() { return DecalStatus(2); }

// ------------------------------------------------------------------------------------------- jobs, results

void JobsTick() {
  if (g_job_state == 2) {
    const int r = g_job_result;
    const std::string where = g_job_where;
    if (g_job == JOB_COMPARE) {
      if (r == 1) {
        ss_log("[extras] unlocks off: the save is the same as in the backup from before (the game did not write it)");
      } else if (r == 0) {
        ss_log("[extras] unlocks off: the game wrote its save while they were on; the save from before is the newest "
               "save-backups\\%s* folder",
               kUnlockPrefix);
      } else {
        ss_log("[extras] unlocks off: the save could not be compared with the backup");
      }
    } else if (g_job == JOB_BACKUP_DECALS) {
      if (r == SAVES_KEPT_MADE || r == SAVES_KEPT_SAME) {
        ss_log("[extras] decals: save backup %s (%s)", where.c_str(), r == SAVES_KEPT_MADE ? "made" : "same bytes, kept");
        if (g_phase == extras::BACKUP) {
          g_phase = extras::WORKING;
          g_asked = -1;
          g_given = 0;
          g_skip = 0;
          memset(g_tries, 0, sizeof g_tries);
        }
      } else {
        ss_log("[extras] decals: %s: nothing unlocked", r == SAVES_KEPT_NO_SAVE ? "no save to back up" : "the save backup failed");
        if (g_phase == extras::BACKUP) g_phase = extras::FAILED;
      }
    } else {
      const bool ok = r != SAVES_KEPT_FAILED;
      if (r == SAVES_KEPT_NO_SAVE) {
        ss_log("[extras] unlocks: no save yet, nothing to back up");
      } else {
        ss_log("[extras] unlocks: save backup %s (%s)", ok ? where.c_str() : "failed",
               r == SAVES_KEPT_MADE ? "made" : r == SAVES_KEPT_SAME ? "same bytes, kept" : "the switches stay off");
      }
      for (Switch& s : g_sw)
        if (s.bk == BK_RUNNING) s.bk = ok ? BK_OK : BK_FAILED;
    }
    g_job = JOB_NONE;
    InterlockedExchange(&g_job_state, 0);
  }
  if (g_job_state != 0) return;
  if (g_decal_backup_needed) {
    if (StartJob(JOB_BACKUP_DECALS)) g_decal_backup_needed = false;
    return;
  }
  bool needed = false;
  for (const Switch& s : g_sw) needed |= s.bk == BK_NEEDED;
  if (needed) {
    if (StartJob(JOB_BACKUP_UNLOCK))
      for (Switch& s : g_sw)
        if (s.bk == BK_NEEDED) s.bk = BK_RUNNING;
    return;
  }
  if (g_compare && StartJob(JOB_COMPARE)) g_compare = false;
}

// From the online sign-in until signed out, or in a network game mode.
bool Online() {
  const uintptr_t m = Rd(kModeObj);
  if (m && Readable(m + 8, 4)) {
    const uint32_t v = Rd(m + 8);
    if (v == 2 || v == 3) return true;
  }
  const uintptr_t os = Rd(kOnlineSetup);
  return os && Readable(os, 0x220) && (Rd(os + 0x18) != 0 || Rd(os + 0x21C) == 1);
}

}  // namespace

static MenuRegistrar g_row_unlock(MenuChoiceRow("unlock_all", "UNLOCK ALL", MENU_PAGE_GAME, MENU_LIVE, "false",
                                                {{"false", "OFF"}, {"true", "ON"}}, ApplySwitch));
static MenuRegistrar g_row_bonus(MenuChoiceRow("bonus_content", "BONUS CONTENT", MENU_PAGE_GAME, MENU_LIVE, "false",
                                               {{"false", "OFF"}, {"true", "ON"}}, ApplySwitch));
static MenuRegistrar g_row_pure(MenuChoiceRow("pure_decal", "PURE DECAL", MENU_PAGE_GAME, MENU_LIVE, "false",
                                              {{"false", "OFF"}, {"true", "ON"}}, ApplySwitch));
static MenuRegistrar g_row_dec_online(MenuActionRow("decals_online", "UNLOCK ONLINE DECALS", MENU_PAGE_GAME, PressOnline,
                                                    StatusOnline));
static MenuRegistrar g_row_dec_all(MenuActionRow("decals_all", "UNLOCK ALL DECALS", MENU_PAGE_GAME, PressAll, StatusAll));

// patch.cpp, from ss_patch_start (the exe is the known Steam build): the conf's switches.
void ss_extras_start() {
  if (!ss_conf_present()) return;
  g_on = GetModuleHandleA(nullptr) == reinterpret_cast<HMODULE>(0x400000);
  if (!g_on) return;
  for (int i = 0; i < 4; ++i) g_sw[0].at[i] = kUnlockAll[i];
  for (int i = 0; i < kBonusCount; ++i) g_sw[1].at[i] = kBonus + i * kBonusStride;
  for (Switch& s : g_sw) ApplySwitch(s.key, ss_conf(s.key).c_str());
  if (AnyWanted())
    ss_log("[extras] unlock_all = %d, bonus_content = %d, pure_decal = %d: a save backup first, then on (off online)",
           g_sw[0].wanted, g_sw[1].wanted, g_sw[2].wanted);
}

// patch.cpp's Present hook (the game's thread, between frames).
void ss_extras_present() {
  if (!g_on) return;
  if (!g_checked) CheckBytes();
  if ((g_frame++ & 7) == 0) {  // every 8th frame: the online state and the race
    const bool online = Online();
    if (online != g_online) ss_log("[extras] %s", online ? "online: the unlock switches are held off" : "offline again");
    g_online = online;
    g_in_race = !g_online && SoloRace();
  }
  JobsTick();
  if (g_bad) return;
  SwitchesTick();
  DecalsTick();
}

const char* ss_extras_note(const char* key) {
  if (!g_on) return nullptr;
  if (const Switch* s = Find(key)) {
    if (!s->wanted) return nullptr;
    if (g_bad) return "This game build is not the known one: this switch does nothing.";
    if (s->bk == BK_FAILED) return "The save backup failed, so this stays off. See splitsecond-patch.log.";
    if (g_online) return "Held off while you are online. It comes back when you leave online play.";
    return nullptr;
  }
  if (!strcmp(key, "decals_online") || !strcmp(key, "decals_all")) {
    if (g_phase == extras::FAILED && g_mode == (key[7] == 'o' ? 1 : 2))
      return "No backup of the save could be made, so nothing was changed. See splitsecond-patch.log.";
    if (g_online) return "Not available while you are online.";
  }
  return nullptr;
}
