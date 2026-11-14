// Split/Second (PC, Steam build 385319): boot straight to the main menu (boot_to_menu, SSPC-26).
//
// After the boot movies (skip_intro, intro.cpp) the front end shows the title screen and waits for Press Start; then
// the save layer loads the profile and the main menu comes. boot_to_menu passes the title screen from the screen's own
// state, along the game's own path, so the owner does not press anything. Reverse engineering of the Steam build
// (run-time VAs):
//
// * Screen manager *(0xD64E04): the screens shown so far, by id, at +0x28 (count +0x24); the last one is the screen
//   now shown. FE_title_screen is id 7 (constructor 0x44D810, vtable 0xBAA2FC in read-only data), FE_main_menu id 9.
//   A screen's +0x90 is its transition state (1 coming in, 2 shown, 3 going out).
// * Layer stack *(0xD64E0C): the top index at +0x54 (-1: none), the layers at +0xC + 4 * index, a layer's id at +4.
//   0x425F30 (thiscall: pop 0/1, id, 0, 0) pushes or pops one. The Press Start overlay (FE_Press_Start, constructor
//   0x4809D0) is layer 8; the save layer ("createsave", CreateSave.gfx, constructor 0x4821E0) is 0x18; the profile
//   creation layer (no profile yet: a name to type, FE_ProfileSelection.gfx) is 0x25.
// * The title screen's update (vtable slot 15, word 0xBAA338, holds 0x44DDA0; thiscall (this, dt), ret 4): once the
//   screen is shown (+0x90 = 2) and layer 8 is on top, it asks the player-0 actions 0xC / 0x14 (Start / A) or the
//   platform's signed-in user. On a press it pops layer 8 and calls 0x44DBC0(this, pad 0): the profile in use (the
//   PC's profile list) decides between the save layer (a profile: load it) and the profile creation layer.
// * The save layer then runs by itself: slide in, load (DisplayLoading), DLC check, then it pops itself and opens the
//   main menu (0x482720 -> 0x451CD0(9)). Only a first start (no save: the training race) or a save error waits for a
//   button there; those are left to the player.
//
// What this file does (nothing in the game's code is written): the title screen's slot 15 word is swapped
// (ss_write_word, only while it holds the known original). The hook calls the original update; then, when the title
// screen is shown, layer 8 is still on top, the player has not pressed anything and the overlay has been up for
// kTitleDelayMs, it does what the original does on a Start press of pad 0: pops layer 8 and calls 0x44DBC0(this, 0).
// So the step can only happen on the title screen with Press Start up, and it happens once. A watcher thread logs
// each front-end step (the screen, the top layer, the save layer's state, a popup) and stops at the main menu, at the
// player's first own press (pad: pad.cpp; keyboard: a new key down while the game is in front) or after kTimeoutMs.
// The disclaimer and the autosave notice belong to the boot play list (cIntroState) and are not skippable; their
// time is the game's own.
//
// SSPC-25 (start in the online menu) hooks in at OnMainMenu().
#include <windows.h>
#include <cstdint>
#include <cstdlib>
#include <string>
#include "ss_shared.h"

#define ENTRY_THIS __attribute__((thiscall, force_align_arg_pointer))

bool ss_intro_wanted();  // intro.cpp: skip_intro (boot_to_menu's default)

namespace {

constexpr uintptr_t kScreenMgr = 0xD64E04;   // *: screen manager
constexpr uintptr_t kLayerStack = 0xD64E0C;  // *: layer stack
constexpr uintptr_t kPopups = 0xD64E10;      // *: +0x4C the popup queue, its first word the shown popup's level (-1: none)
constexpr uintptr_t kIntroState = 0xD6606C;  // cIntroState: +0x14 its step
constexpr uintptr_t kIntroDone = 0xD660C4;

constexpr uintptr_t kTitleVt = 0xBAA2FC;
constexpr uintptr_t kTitleSlotUpdate = kTitleVt + 15 * 4;  // 0xBAA338
constexpr uint32_t kOrigTitleUpdate = 0x44DDA0;
constexpr uint32_t kOrigTitleSlot0 = 0x44DAF0, kOrigTitleSlot1 = 0x457400;
constexpr uintptr_t kLayerPush = 0x425F30;  // thiscall (stack, pop, id, 0, 0), ret 0x10
constexpr uintptr_t kTitleStart = 0x44DBC0;  // thiscall (title, pad), ret 4

constexpr int kScreenTitle = 7, kScreenMain = 9;
constexpr int kLayerPressStart = 8, kLayerSave = 0x18, kLayerProfileCreate = 0x25;
constexpr DWORD kTitleDelayMs = 400;  // Press Start is up this long before the step (its slide-in)
constexpr DWORD kTimeoutMs = 180000;  // from the DLL's start: never later than this

volatile LONG g_active = 0;  // the steps may happen
volatile LONG g_user = 0;    // the player pressed something: hands off
volatile LONG g_title_done = 0;
DWORD g_t0 = 0;
DWORD g_title_seen = 0;  // tick when Press Start was first seen on top (game thread)

inline uint32_t Rd(uintptr_t va) { return *reinterpret_cast<const volatile uint32_t*>(va); }

bool Readable(uintptr_t p, size_t n) {
  if (p < 0x10000) return false;
  MEMORY_BASIC_INFORMATION mi;
  if (!SS_VQ(reinterpret_cast<const void*>(p), &mi, sizeof mi)) return false;
  if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || mi.Protect == 0) return false;
  return p + n <= reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize;
}

bool ParseBool(std::string v, bool dflt) {
  for (char& c : v) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  if (v.empty()) return dflt;
  return v == "1" || v == "true" || v == "yes" || v == "on";
}

double Secs() { return (GetTickCount() - g_t0) / 1000.0; }

// The front end as far as it can be read (-1: not there).
struct Fe {
  int screen = -1, screen_state = -1;
  int layer = -1;
  uintptr_t layer_obj = 0;
  int save_state = -1;  // the save layer's step (+0x74) while it is on top
  int popup = -1;
  int intro_step = -1;
  bool operator!=(const Fe& o) const {
    return screen != o.screen || screen_state != o.screen_state || layer != o.layer || save_state != o.save_state ||
           popup != o.popup || intro_step != o.intro_step;
  }
};

Fe ReadFe() {
  Fe f;
  if (Readable(kIntroState + 0x14, 4)) f.intro_step = static_cast<int>(Rd(kIntroState + 0x14));
  const uintptr_t sm = Readable(kScreenMgr, 4) ? Rd(kScreenMgr) : 0;
  if (sm && Readable(sm + 0x24, 8)) {
    const uint32_t n = Rd(sm + 0x24), ids = Rd(sm + 0x28);
    if (n > 0 && n < 64 && Readable(ids + 4 * (n - 1), 4)) f.screen = static_cast<int>(Rd(ids + 4 * (n - 1)));
    // The screen objects: an array at +4, 0x98 bytes an id, the object at +0x94 (0x4502E0).
    const uintptr_t tab = Rd(sm + 4);
    if (f.screen >= 0 && f.screen < 0x80 && tab && Readable(tab + f.screen * 0x98 + 0x94, 4)) {
      const uintptr_t scr = Rd(tab + f.screen * 0x98 + 0x94);
      if (scr && Readable(scr + 0x90, 4)) f.screen_state = static_cast<int>(Rd(scr + 0x90));
    }
  }
  const uintptr_t ls = Readable(kLayerStack, 4) ? Rd(kLayerStack) : 0;
  if (ls && Readable(ls + 0x54, 4)) {
    const int top = static_cast<int>(Rd(ls + 0x54));
    if (top >= 0 && top < 64 && Readable(ls + 0xC + 4 * top, 4)) {
      const uintptr_t l = Rd(ls + 0xC + 4 * top);
      if (l && Readable(l + 4, 4)) {
        f.layer = static_cast<int>(Rd(l + 4));
        f.layer_obj = l;
        if (f.layer == kLayerSave && Readable(l + 0x74, 4)) f.save_state = static_cast<int>(Rd(l + 0x74));
      }
    }
  }
  const uintptr_t pm = Readable(kPopups, 4) ? Rd(kPopups) : 0;
  if (pm && Readable(pm + 0x4C, 4)) f.popup = static_cast<int>(Rd(pm + 0x4C));
  return f;
}

const char* ScreenName(int id) {
  switch (id) {
    case 7: return "title screen";
    case 9: return "main menu";
    case 0x1B: return "online play";
    case 0x1E: return "options";
    case 0x24: return "profile selection";
    default: return "";
  }
}

const char* LayerName(int id) {
  switch (id) {
    case 8: return "Press Start";
    case 0x17: return "sign-in";
    case 0x18: return "save (profile load)";
    case 0x25: return "profile creation";
    case 0x2C: return "install game data";
    default: return "";
  }
}

void Stop(const char* why) {
  if (InterlockedExchange(&g_active, 0)) ss_log("[boot] %s after %.1f s; boot_to_menu hands off", why, Secs());
}

// SSPC-25: the place to go on from the main menu (the online menu, with the automatic login). Nothing yet.
void OnMainMenu() {}

using UpdateFn = void(__attribute__((thiscall)) *)(void*, float);
using LayerFn = void(__attribute__((thiscall)) *)(void*, int, int, int, int);
using StartFn = void(__attribute__((thiscall)) *)(void*, int);

// The title screen's update (game thread).
void ENTRY_THIS TitleUpdate(void* self, float dt) {
  reinterpret_cast<UpdateFn>(kOrigTitleUpdate)(self, dt);
  if (!g_active || g_user || g_title_done || !self) return;
  const uintptr_t s = reinterpret_cast<uintptr_t>(self);
  if (Rd(s + 0x90) != 2) return;  // not shown yet, or going out
  const uintptr_t ls = Rd(kLayerStack);
  if (!ls) return;
  const int top = static_cast<int>(Rd(ls + 0x54));
  if (top < 0 || top >= 64) return;
  const uintptr_t l = Rd(ls + 0xC + 4 * top);
  if (!l || static_cast<int>(Rd(l + 4)) != kLayerPressStart) {
    g_title_seen = 0;
    return;
  }
  const DWORD now = GetTickCount();
  if (!g_title_seen) {
    g_title_seen = now ? now : 1;
    ss_log("[boot] title screen: Press Start is up after %.1f s", Secs());
    return;
  }
  if (now - g_title_seen < kTitleDelayMs) return;
  InterlockedExchange(&g_title_done, 1);
  ss_log("[boot] title screen: Press Start passed for pad 0 after %.1f s (the game's own path: layer 8 popped, 0x44DBC0)", Secs());
  reinterpret_cast<LayerFn>(kLayerPush)(reinterpret_cast<void*>(ls), 1, kLayerPressStart, 0, 0);
  reinterpret_cast<StartFn>(kTitleStart)(self, 0);
}

bool HookTitle() {
  const uint32_t hook = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&TitleUpdate));
  for (int i = 0; i < 400; ++i) {
    if (Readable(kTitleVt, 0x40) && Rd(kTitleVt) == kOrigTitleSlot0 && Rd(kTitleVt + 4) == kOrigTitleSlot1) {
      const uint32_t cur = Rd(kTitleSlotUpdate);
      if (cur != kOrigTitleUpdate) {
        ss_log("[boot] the title screen's slot 15 holds %08x, not the known build's; boot_to_menu off", cur);
        return false;
      }
      if (!ss_write_word(reinterpret_cast<void*>(kTitleSlotUpdate), hook)) {
        ss_log("[boot] cannot write the title screen's vtable (error %lu); boot_to_menu off", GetLastError());
        return false;
      }
      return true;
    }
    Sleep(50);
  }
  ss_log("[boot] the title screen's vtable never looked like the known build's; boot_to_menu off");
  return false;
}

// A new key down on the keyboard while the game is in front (the player's own press).
bool NewKey(uint8_t* prev) {
  bool fg = false;
  if (HWND w = GetForegroundWindow()) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    fg = pid == GetCurrentProcessId();
  }
  bool hit = false;
  for (int vk = 8; vk < 0xFF; ++vk) {
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    if (down && !prev[vk] && fg) hit = true;
    prev[vk] = down;
  }
  return hit;
}

DWORD WINAPI Watch(LPVOID) {
  if (!HookTitle()) {
    InterlockedExchange(&g_active, 0);
    return 0;
  }
  ss_log("[boot] boot_to_menu on: the title screen's Press Start is passed for pad 0 (title update %p wrapped)",
         reinterpret_cast<void*>(kTitleSlotUpdate));
  uint8_t keys[256] = {};
  NewKey(keys);  // what is already held does not count
  Fe last;
  bool intro_logged = false;
  while (g_active) {
    if (g_user) {
      Stop("the player pressed a button");
      break;
    }
    if (NewKey(keys)) {
      InterlockedExchange(&g_user, 1);
      Stop("the player pressed a key");
      break;
    }
    if (GetTickCount() - g_t0 > kTimeoutMs) {
      Stop("the main menu was not seen in time");
      break;
    }
    if (!intro_logged && Readable(kIntroDone, 1) && *reinterpret_cast<volatile const uint8_t*>(kIntroDone)) {
      intro_logged = true;
      ss_log("[boot] the boot play list is over after %.1f s", Secs());
    }
    const Fe f = ReadFe();
    if (f != last) {
      ss_log("[boot] %.2f s: screen %d %s (state %d), top layer %d %s%s, save step %d, popup %d, intro step %d", Secs(), f.screen,
             ScreenName(f.screen), f.screen_state, f.layer, LayerName(f.layer), f.layer == kLayerProfileCreate ? " (no profile: left to the player)" : "",
             f.save_state, f.popup, f.intro_step);
      last = f;
    }
    if (f.screen == kScreenMain && f.screen_state == 2 && f.layer != kLayerSave) {
      ss_log("[boot] main menu reached after %.1f s with no button pressed", Secs());
      OnMainMenu();
      InterlockedExchange(&g_active, 0);
      break;
    }
    if (f.layer == kLayerProfileCreate) {
      Stop("no profile yet (profile creation: a name to type)");
      break;
    }
    Sleep(30);
  }
  return 0;
}

bool Wanted() {
  if (const char* e = getenv("SS_BOOT_TO_MENU")) return ParseBool(e, true);
  if (!ss_conf_present()) return false;
  return ParseBool(ss_conf("boot_to_menu"), ss_intro_wanted());
}

}  // namespace

// pad.cpp: a new button press on a real pad (the player's own, never a tap of ours).
void ss_boot_user_input() {
  if (g_active && !g_user) InterlockedExchange(&g_user, 1);
}

void ss_boot_start() {
  g_t0 = GetTickCount();
  if (!Wanted()) {
    ss_log("[boot] boot_to_menu off: the title screen waits for Press Start");
    return;
  }
  if (GetModuleHandleA(nullptr) != reinterpret_cast<HMODULE>(0x400000)) {
    ss_log("[boot] unexpected image base; boot_to_menu off");
    return;
  }
  g_active = 1;
  CreateThread(nullptr, 0, Watch, nullptr, 0, nullptr);
}
