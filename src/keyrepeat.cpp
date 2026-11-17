// Menu key repeat (SSPC-47): a held arrow key keeps scrolling in the menus (car selection, lists), as a held stick does.
//
// What the PC build does (Steam build 385319, run-time VAs):
//
// * The keyboard is polled with GetKeyboardState (0x40D2B0), not DirectInput. The input manager *(0xD646C8) keeps one
//   binding entry per device: count +0xCF6C, array +0xCF70, 0x584 bytes each, {type, index} at +0x564 (type 1 =
//   keyboard). An entry holds 0x45 rows of 0x14 bytes {action, key, pad control, pad kind, hat angle}; a keyboard
//   query (0x40EE10, vtable 0xBA2904 slot 1) reads only the key (a VK code into the key buffer), a pad query only the
//   last three fields.
// * The front end's input block (0x4D60F0) takes left / right / up / down from actions 0x1B / 0x1C / 0x19 / 0x1A on the
//   press edge only (these rows hold the arrow keys and the d-pad), and ORs in actions 0x25 / 0x26 / 0x27 / 0x28 (the
//   left stick) through the game's own repeat: at once, again after 0.4 s (0xD12698), then every 0.1 s (0xD126A0).
//   Those four rows are the only ones of the four actions in the game (a scan of the manager's callers); their key is 0
//   in both keyboard defaults (0xCFABF8 for keyboard index 0, 0xCFB06C for index 1, copied by 0x40EBA0), so a held
//   stick repeats and a held arrow key does not.
// * The Controls screen lists and conflict-checks only its twelve rows (0xBAA614: rows 0x1C.. 0x34), not these.
//
// What this file does (data only, nothing in the game's code is written):
//
// * The save keeps the live entries: the profile's "Controls" / "ControllersMap" block (0x78A9C0..) writes and reads the
//   manager's entry array whole. So the rows get key codes no keyboard has, VK 0x97..0x9A (unassigned in Windows), and
//   the game's GetKeyboardState import (menu.cpp's hook, IAT 0xB9B2A8) mirrors the arrows' state onto them. Without
//   the patch a saved copy of the rows does nothing: no key ever reports 0x97..0x9A down, as with the stock key 0.
// * The codes go into rows 24..27 (actions 0x25..0x28) of both .data keyboard defaults, so the game's own reset
//   (Controls > Reset, a new profile) keeps them, and of every live keyboard entry, where a loaded profile may have put
//   its own copy. A row is written only while it is the game's own (its action, key 0).
// * The mirror holds a code down only after its arrow has been down 0.35 s, so a tap is the game's own single step; a
//   held arrow then steps at once, 0.4 s later, then 10 a second. A press that starts while the in-game controller is
//   up (*0xD66B24, vptr 0xBB642C: driving, the pause, the results) never repeats: the arrows are the driving keys.
// It runs from patch.cpp's Present hook (the game's thread, between frames), every 32nd present.
//
// menu_key_repeat = false (SS_MENU_KEY_REPEAT): nothing is written. menu_key_repeat_log = true (a test aid): one log line
// per present that sees a left / right / up / down step in the front end's input block (*0xD64E08, bytes 4..7).

#include <windows.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "ss_shared.h"

namespace {

constexpr uintptr_t kManager = 0xD646C8;
constexpr uintptr_t kCount = 0xCF6C, kEntries = 0xCF70;
constexpr uintptr_t kEntrySize = 0x584, kEntryType = 0x564;
constexpr uintptr_t kRowSize = 0x14;
constexpr int kFirstRow = 24;  // rows 24..27: actions 0x25..0x28
// Action 0x25 left, 0x26 right, 0x27 up, 0x28 down (0x4D60F0's bytes 4 / 5 / 6 / 7): the key each row reads (unassigned
// VK codes) and the arrow mirrored onto it.
constexpr uint32_t kVk[4] = {0x97, 0x98, 0x99, 0x9A};
constexpr uint8_t kArrow[4] = {VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN};
constexpr uintptr_t kDefaults[2] = {0xCFABF8, 0xCFB06C};  // keyboard index 0 / 1

bool g_on = false;
volatile bool g_mirror = false;  // the rows may hold our codes: the keyboard hook mirrors the arrows
volatile bool g_in_game = false; // the in-game controller is up (race, pause, results): a press there does not repeat
constexpr uintptr_t kController = 0xD66B24;  // the in-game run state's +0x10: InGame::cInGameController (patch.cpp)
constexpr uint32_t kControllerVt = 0xBB642C;
constexpr DWORD kHoldMs = 350;    // a press shorter than this is the game's own single step (the edge row only)
bool g_failed = false;
bool g_steplog = false;

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
inline uint32_t Rd(uintptr_t va) { return *reinterpret_cast<const volatile uint32_t*>(va); }
inline void Wr(uintptr_t va, uint32_t v) { *reinterpret_cast<volatile uint32_t*>(va) = v; }

// The code this relies on: the block builder's four repeat queries and the keyboard query's key read.
bool CheckBuild() {
  static const uint8_t q25[] = {0xC7, 0x44, 0x24, 0x20, 0x25, 0x00, 0x00, 0x00};  // 0x4D6367 mov [esp+0x20], 0x25
  static const uint8_t q28[] = {0xC7, 0x44, 0x24, 0x20, 0x28, 0x00, 0x00, 0x00};  // 0x4D6406 mov [esp+0x20], 0x28
  static const uint8_t key[] = {0x8B, 0x4C, 0x87, 0x04};                           // 0x40EEB3 mov ecx, [edi+eax*4+4]
  if (!Readable(0x40EEB3, 4) || !Readable(0x4D6367, 8) || !Readable(0x4D6406, 8)) return false;
  if (memcmp(reinterpret_cast<const void*>(0x4D6367), q25, 8) || memcmp(reinterpret_cast<const void*>(0x4D6406), q28, 8) ||
      memcmp(reinterpret_cast<const void*>(0x40EEB3), key, 4))
    return false;
  // Both defaults: rows 24..27 hold actions 0x25..0x28, the row before is 0x24 (Back) and the row after 0x29.
  for (uintptr_t t : kDefaults) {
    if (!Readable(t, 0x45 * kRowSize)) return false;
    if (Rd(t + (kFirstRow - 1) * kRowSize) != 0x24 || Rd(t + (kFirstRow + 4) * kRowSize) != 0x29) return false;
    for (int i = 0; i < 4; ++i) {
      const uintptr_t row = t + (kFirstRow + i) * kRowSize;
      if (Rd(row) != 0x25u + i || (Rd(row + 4) != 0 && Rd(row + 4) != kVk[i])) return false;
    }
  }
  return true;
}

// Rows 24..27 of one table: the arrow keys where the row is still the game's own. Returns how many were written.
int Fill(uintptr_t table) {
  int n = 0;
  for (int i = 0; i < 4; ++i) {
    const uintptr_t row = table + (kFirstRow + i) * kRowSize;
    if (Rd(row) == 0x25u + i && Rd(row + 4) == 0) {
      Wr(row + 4, kVk[i]);
      ++n;
    }
  }
  return n;
}

}  // namespace

void ss_keyrepeat_start() {
  if (!ss_conf_present()) return;  // no conf file: the DLL does nothing
  const char* e = getenv("SS_MENU_KEY_REPEAT");
  const std::string v = e ? std::string(e) : ss_conf("menu_key_repeat");  // absent = true
  if (v == "false" || v == "off" || v == "0") {
    ss_log("[keyrepeat] menu_key_repeat = false: a held arrow key moves once in the menus (the game's own)");
    return;
  }
  g_on = GetModuleHandleA(nullptr) == reinterpret_cast<HMODULE>(0x400000);
  g_steplog = ss_conf("menu_key_repeat_log") == "true";
}

namespace {
// Test aid: the steps the menus get this frame (the block is rebuilt once per update and read here between frames).
void StepLog() {
  const uintptr_t fe = Rd(0xD64E08);
  if (!fe || !Readable(fe, 0x68) || !Rd(fe + 0x64)) return;  // +0x64: the menus' player (none yet: no block built)
  const uint32_t b = Rd(fe + 4);  // left, right, up, down
  static uint32_t last = 0;
  static int was_in_game = -1;
  if (int(g_in_game) != was_in_game) {
    was_in_game = g_in_game;
    ss_log("[keyrepeat] in-game controller %08X: %s", Rd(kController), g_in_game ? "up (a press here does not repeat)" : "none");
  }
  static DWORD t0 = GetTickCount();
  if (b & 0x01010101) ss_log("[keyrepeat] step %s%s%s%s t=%lu ms%s", b & 0xFF ? "L" : "", b & 0xFF00 ? "R" : "",
                            b & 0xFF0000 ? "U" : "", b & 0xFF000000 ? "D" : "", GetTickCount() - t0, b == last ? " (same as last present)" : "");
  last = b;
}
}  // namespace

// menu.cpp's GetKeyboardState hook (the game's thread, once per update): the arrows' state on the rows' codes.
// The code goes down only once the arrow has been held kHoldMs: a tap is the game's own single step (the edge row),
// whatever the repeat timer does (it fires again on a frame whose time step is 0, seen on the main menu as it loads).
// A held arrow then steps at once, again 0.4 s later, then every 0.1 s (the stick's timer).
// A press that starts in the game proper (driving, the pause, the results) does not repeat: the driving keys are the
// arrows, and a held accelerator would otherwise scroll the pause or results menu once it comes up.
void ss_keyrepeat_keys(unsigned char* state) {
  if (!g_mirror) return;
  static bool held[4], allowed[4];
  static DWORD since[4];
  const DWORD now = GetTickCount();
  for (int i = 0; i < 4; ++i) {
    const bool down = (state[kArrow[i]] & 0x80) != 0;
    if (down && !held[i]) {
      allowed[i] = !g_in_game;
      since[i] = now;
    }
    held[i] = down;
    state[kVk[i]] = down && allowed[i] && now - since[i] >= kHoldMs ? 0x80 : 0;
  }
}

// patch.cpp's Present hook (the game's thread): the defaults once, then the live keyboard entries every 32nd present
// (a profile load or a reset can put a fresh copy there).
void ss_keyrepeat_present() {
  if (!g_on || g_failed) return;
  static bool defaults_done = false;
  if (!defaults_done) {
    if (!ss_game_keyboard_hook()) {
      g_failed = true;
      ss_log("[keyrepeat] the game's keyboard import is not ours; a held arrow key moves once (the game's own)");
      return;
    }
    if (!CheckBuild()) {
      g_failed = true;
      ss_log("[keyrepeat] the menu input code is not the known build; a held arrow key moves once (the game's own)");
      return;
    }
    g_mirror = true;
    const int n = Fill(kDefaults[0]) + Fill(kDefaults[1]);
    defaults_done = true;
    ss_log("[keyrepeat] keys 0x97..0x9A (the arrows, mirrored) on the menu repeat rows (actions 0x25..0x28) of the keyboard "
           "defaults: %d rows written", n);
  }
  {
    const uintptr_t c = Rd(kController);
    g_in_game = c && Readable(c, 4) && Rd(c) == kControllerVt;
  }
  if (g_steplog) StepLog();
  static unsigned frame = 0;
  if (++frame & 31) return;
  const uintptr_t m = Rd(kManager);
  if (!m || !Readable(m + kCount, 8)) return;
  const uint32_t count = Rd(m + kCount);
  const uintptr_t arr = Rd(m + kEntries);
  if (!count || count > 16 || !arr || !Readable(arr, count * kEntrySize)) return;
  for (uint32_t k = 0; k < count; ++k) {
    const uintptr_t entry = arr + k * kEntrySize;
    if (Rd(entry + kEntryType) != 1) continue;  // keyboard only; a pad's rows never read the key field
    const int n = Fill(entry);
    if (n) {
      static int said = 0;
      if (said < 8) {
        ++said;
        ss_log("[keyrepeat] keyboard entry %u (index %u) at %p: keys 0x97..0x9A on %d menu repeat rows (a held arrow repeats "
               "after 0.4 s, then 10 a second)",
               k, Rd(entry + kEntryType + 4), reinterpret_cast<void*>(entry), n);
      }
    }
  }
}
