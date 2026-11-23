// The game's system-wide keyboard hook (GitHub #7): other programs' shortcuts (a tiling window manager's Alt+number)
// lag or get lost while the game runs, in the background or minimised too.
//
// What the PC build does (Steam build, run-time VAs, observed in the code):
//
// * WinMain (0xA172B0) first calls 0xA18790 (at 0xA172C9) with the exe's HINSTANCE. That function installs a
//   low-level keyboard hook for the whole desktop: SetWindowsHookExA(WH_KEYBOARD_LL, 0xA18700, hInstance, 0) through
//   its import slot 0xB9B2B8, and keeps the HHOOK in the .data word 0xD984D4 (a non-zero word there: it is not
//   installed again, -2). Nothing ever calls UnhookWindowsHookEx on it: the hook lives as long as the game.
// * The hook procedure 0xA18700 eats the Windows keys (VK 0x5B / 0x5C), Alt+Tab, Alt+Esc and Ctrl+Esc for every program
//   on the desktop (a MessageBeep on key down), with no check that the game is in front; every other key goes on
//   with CallNextHookEx(*0xD984D4, ...). 0xD984D4 is read nowhere else (a scan of the image: the installer's check and
//   store, the procedure's CallNextHookEx).
// * A low-level hook procedure runs on the thread that installed it, through its message loop: every key pressed
//   anywhere on the desktop can wait until the game's main thread pumps its messages. A slow or busy game thread
//   (loading, minimised, a hitch) can hold every other program's keys back, up to Windows' hook time-out (inferred;
//   not reproduced, see below).
// * Seen on Windows 11 build 26200 (2026-11-19): the call comes about 8.5 s after the start, from the main thread;
//   the game's window belongs to another thread. The hook is installed (a real handle, no error), yet there it neither
//   ate the Windows key nor held any key back (the first key after it 2 ms, the rest 0.4 ms, the game busy loading or
//   minimised). The report came from a newer Windows build; the hook is left out on every build all the same.
//
// What this file does (an import slot and an API call, nothing in the game's code is written):
//
// * game_key_hook = off (the default): once the import table is filled and quiet (ss_import_table_wait), the
//   SetWindowsHookExA slot is taken if the game has not installed its hook yet: a WH_KEYBOARD_LL call with the
//   game's procedure gets a stand-in handle (non-zero, never a real hook) and no hook; any other call goes to user32.
//   The game keeps the stand-in in 0xD984D4; its procedure is never called, so the stand-in is never passed on.
//   Logged as "skipped" (the case seen: the call comes seconds after the table is quiet).
// * When the hook is already in (WinMain ran before the import table was quiet), the HHOOK in 0xD984D4 is removed
//   with UnhookWindowsHookEx (from this thread: Windows checks the process, not the thread; seen to work with a test
//   switch). The word keeps its value, so the installer would not install it again (it is called once anyway).
//   Logged as "removed after start".
// * game_key_hook = on: nothing is done, the game's own hook as it ships (meant to block the Windows keys and Alt+Tab).
//   Without the hook the Windows key and Alt+Tab work while the game is in front, as in most PC games.
//
// (SS_GAME_KEY_HOOK=0|1)

#include <windows.h>
#include <cstdint>
#include <cstdlib>
#include <string>
#include "ss_shared.h"

namespace {

constexpr uintptr_t kIatSetWindowsHookExA = 0xB9B2B8;  // the game's import of user32!SetWindowsHookExA
constexpr uintptr_t kGameHook = 0xD984D4;              // .data: the HHOOK the installer 0xA18790 keeps
constexpr uintptr_t kGameHookProc = 0xA18700;          // the game's WH_KEYBOARD_LL procedure
constexpr int kWaitMs = 120000;                        // how long the word is watched for the game's hook

using SetWindowsHookExAFn = HHOOK(WINAPI*)(int, HOOKPROC, HINSTANCE, DWORD);
SetWindowsHookExAFn g_real = nullptr;
char g_stand_in_tag;  // its address is the stand-in handle: non-zero, never a hook handle of user32
HHOOK StandIn() { return reinterpret_cast<HHOOK>(&g_stand_in_tag); }
volatile LONG g_said = 0;

HHOOK WINAPI HkSetWindowsHookExA(int id, HOOKPROC proc, HINSTANCE mod, DWORD thread) {
  if (id == WH_KEYBOARD_LL && reinterpret_cast<uintptr_t>(proc) == kGameHookProc) {
    if (!InterlockedExchange(&g_said, 1))
      ss_log("[kbhook] skipped: the game's system-wide keyboard hook (WH_KEYBOARD_LL, procedure 0x%X) is not "
             "installed; other programs' keys no longer go through the game's thread, the Windows key and Alt+Tab "
             "work in front of it",
             static_cast<unsigned>(kGameHookProc));
    return StandIn();
  }
  return g_real(id, proc, mod, thread);
}

uintptr_t GameHook() { return *reinterpret_cast<const volatile uintptr_t*>(kGameHook); }

// Takes the import slot when it holds user32's function. -> true when the game's calls now come here.
bool TakeSlot() {
  HMODULE u = GetModuleHandleA("user32.dll");
  const uintptr_t real = u ? reinterpret_cast<uintptr_t>(GetProcAddress(u, "SetWindowsHookExA")) : 0;
  const uintptr_t cur = *reinterpret_cast<const volatile uintptr_t*>(kIatSetWindowsHookExA);
  if (!real || cur != real) {
    ss_log("[kbhook] the game's SetWindowsHookExA import slot 0x%X holds %08lx, not user32's %08lx: left alone",
           static_cast<unsigned>(kIatSetWindowsHookExA), static_cast<unsigned long>(cur),
           static_cast<unsigned long>(real));
    return false;
  }
  g_real = reinterpret_cast<SetWindowsHookExAFn>(real);
  if (!ss_write_word(reinterpret_cast<void*>(kIatSetWindowsHookExA), reinterpret_cast<uintptr_t>(&HkSetWindowsHookExA))) {
    ss_log("[kbhook] cannot write the game's SetWindowsHookExA import slot 0x%X: left alone",
           static_cast<unsigned>(kIatSetWindowsHookExA));
    return false;
  }
  return true;
}

DWORD WINAPI Thread(LPVOID) {
  const DWORD t0 = GetTickCount();
  ss_import_table_wait("kbhook");
  // Not installed yet: the slot is taken. A call already inside user32 then still installs it: the word below shows
  // its handle a moment later, and it is removed as in the other case.
  bool slot = false;
  if (!GameHook()) slot = TakeSlot();
  for (;;) {
    const uintptr_t h = GameHook();
    if (h && h == reinterpret_cast<uintptr_t>(StandIn())) return 0;  // HkSetWindowsHookExA said "skipped"
    if (h) {
      const BOOL ok = UnhookWindowsHookEx(reinterpret_cast<HHOOK>(h));
      const DWORD err = ok ? 0 : GetLastError();
      if (ok)
        ss_log("[kbhook] removed after start: the game's system-wide keyboard hook (WH_KEYBOARD_LL, handle %08lx, "
               "installed before the import table was quiet) is unhooked %lu ms after the start; other programs' keys "
               "no longer go through the game's thread, the Windows key and Alt+Tab work in front of it",
               static_cast<unsigned long>(h), static_cast<unsigned long>(GetTickCount() - t0));
      else
        ss_log("[kbhook] the game's keyboard hook (handle %08lx) could not be removed (error %lu): it stays as it ships",
               static_cast<unsigned long>(h), static_cast<unsigned long>(err));
      return 0;
    }
    if (GetTickCount() - t0 >= DWORD(kWaitMs)) {
      ss_log("[kbhook] the game installed no keyboard hook in %d s (the import slot %s)", kWaitMs / 1000,
             slot ? "is ours: a later one is skipped" : "is not ours");
      return 0;
    }
    Sleep(5);
  }
}

}  // namespace

void ss_kbhook_start() {
  if (GetModuleHandleA(nullptr) != reinterpret_cast<HMODULE>(0x400000)) return;
  const char* e = getenv("SS_GAME_KEY_HOOK");
  const std::string v = e ? std::string(e) : ss_conf("game_key_hook");  // absent = off
  if (v == "on" || v == "true" || v == "1") {
    ss_log("[kbhook] game_key_hook = on: the game's own system-wide keyboard hook stays (meant to block the Windows keys "
           "and Alt+Tab; every program's keys can wait for the game's thread)");
    return;
  }
  CreateThread(nullptr, 0, Thread, nullptr, 0, nullptr);
}
