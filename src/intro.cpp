// Split/Second (PC, Steam build 385319): skip the boot movies (skip_intro, default true).
//
// The boot sequence (cIntroState, object 0xD6606C; its play list comes from UI/Bootup/Bootup.gfx) marks the movies
// skippable (Intro_CanSkip): a button press ends a movie along the game's own path. So while the intro runs, the
// first presented pad (pad.cpp's virtual pad #0) taps A: pressed for one poll window, then released, a few times a
// second. The intro is over when the game sets 0xD660C4 (0x414F89, the intro state's end); the taps stop there,
// before the title screen. Pages that are not skippable (the disclaimer) simply stay their few seconds.
// The taps also stop at the player's first own button press on any pad (pad.cpp reports it: from then on the player
// skips, or not, by hand), and in any case 60 s after start, so they can never reach the menus or a race even if the
// intro end is never seen.
//
// Nothing in the game is written. Two earlier tries were dropped after tests on the Bazzite copy: cutting the
// movies to one frame inside binkw32 (the game's Bink player read freed memory after the first movie), and the
// game's own -skipintro byte (boot state +0xE74; the front end then read a table, 0xD96ACC, that the skipped intro
// path would have built).
//
// Needs pad_support (the virtual pads); without it the movies play.
#include <windows.h>
#include <cstdint>
#include <cstdlib>
#include <string>
#include "ss_shared.h"

namespace {

constexpr uintptr_t kIntroDone = 0xD660C4;  // set when the intro is over
constexpr DWORD kTimeoutMs = 60000;          // the boot movies take well under a minute
volatile bool g_active = false;
volatile LONG g_user = 0;  // the player pressed a button: the taps stop
DWORD g_t0 = 0;

bool ParseBool(const std::string& v, bool dflt) {
  if (v.empty()) return dflt;
  return v == "1" || v == "true" || v == "yes" || v == "on";
}

bool WantSkip() {
  if (const char* e = getenv("SS_SKIP_INTRO")) return ParseBool(e, true);
  if (!ss_conf_present()) return false;  // no conf file: the DLL does nothing
  return ParseBool(ss_conf("skip_intro"), true);
}

bool Readable(uintptr_t p, size_t n) {
  MEMORY_BASIC_INFORMATION mi;
  if (!SS_VQ(reinterpret_cast<void*>(p), &mi, sizeof mi)) return false;
  if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
  return p + n <= reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize;
}

DWORD WINAPI WatchThread(LPVOID) {
  while (GetTickCount() - g_t0 < kTimeoutMs) {
    if (g_user) {
      g_active = false;
      ss_log("[intro] the player pressed a button after %.1f s; the taps stop", (GetTickCount() - g_t0) / 1000.0);
      return 0;
    }
    if (Readable(kIntroDone, 1) && *reinterpret_cast<volatile const uint8_t*>(kIntroDone) != 0) {
      g_active = false;
      ss_log("[intro] the intro is over after %.1f s; the taps stop", (GetTickCount() - g_t0) / 1000.0);
      return 0;
    }
    Sleep(20);
  }
  g_active = false;
  ss_log("[intro] the intro end was not seen within %lu s; the taps stop", static_cast<unsigned long>(kTimeoutMs / 1000));
  return 0;
}

}  // namespace

// pad.cpp asks on every state read of a presented pad: true = report A pressed now.
bool ss_intro_tap(int pad_index) {
  if (!g_active || g_user || pad_index != 0) return false;
  const DWORD t = GetTickCount() - g_t0;
  if (t >= kTimeoutMs) return false;
  if (t < 3000) return false;   // let the game set up its input first
  return (t / 120) % 4 == 0;    // 120 ms pressed, 360 ms released
}

// pad.cpp: a new button press on a real pad (its own buttons, never a tap of ours).
void ss_intro_user_input() {
  if (g_active) InterlockedExchange(&g_user, 1);
}

void ss_boot_start();  // boot.cpp: boot_to_menu (its default is skip_intro's)

bool ss_pad_support_on();  // pad.cpp: the controller layer is on (pad_support), so the virtual pad #0 exists

bool ss_intro_wanted() { return WantSkip(); }

void ss_intro_start() {
  ss_boot_start();
  if (!WantSkip()) {
    ss_log("[intro] skip_intro off: the boot movies play");
    return;
  }
  if (!ss_pad_support_on()) {  // the taps go through the virtual pad #0, which only the controller layer makes
    ss_log("[intro] skip_intro needs pad_support: the boot movies play");
    return;
  }
  g_t0 = GetTickCount();
  g_active = true;
  ss_log("[intro] skip_intro on: pad #0 taps A while the boot movies run");
  CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr);
}
