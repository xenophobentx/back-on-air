// The way into the settings overlay from the game's own Options screen (SSPC-20, docs/plan-menu-entry.md).
//
// The Options list (FE_options, screen id 0x1E) is eight rows baked into its movie, so no row is added. Instead the
// screen's prompt bar gets one more button prompt, Y "PC SETTINGS", and Y on that screen opens the overlay
// (menu.cpp). Reverse engineering of the Steam build (run-time VAs):
//
// * FE_options' vtable 0xBAE914 is in read-only data. Slot 18 (word 0xBAE95C, holds 0x471240) chooses the prompt
//   bar: this+0x1F0 ("TEXT_SELECT;TEXT_BACK;;;", mask 3) while a resolution change waits for its confirmation
//   (this+0x1D5 set), else this+0x1EC (";TEXT_BACK;;;", mask 2); it puts it in this+0x98 and shows it (0x456D60).
//   Slot 11 (word 0xBAE940, holds 0x4715D0) is HandleInput(this, block): the 10-byte button block (0 A, 1 B, 2 X,
//   3 Y, 4/5 left/right, 6/7 up/down, 8 Start, 9 Back). The screen never reads X or Y; only its cheat-code watcher
//   (0x4710F0, three sequences) sees every byte. On a keyboard the game's Y is Tab (the prompt bar's key names:
//   A Return, B Escape, X Space, Y Tab, Start End).
// * A prompt bar (made by 0x44E110, 0x94 bytes): +0 the button mask (A 1, B 2, X 4, Y 8), +4 the prompt text as a
//   string object: +4 char pointer, +8 a flags word (bit 0: the text is on the heap; (word << 1) >> 4: capacity),
//   +0xC an inline buffer of 0x40 bytes (0x4136C0 / 0x52F6A0). The show (0x456D60) reads the mask and the text
//   every time, so writing both before the original slot 18 runs takes effect at once. The text is "A;B;X;Y;Start";
//   each part a dictionary key or plain text.
//
// What this file does (nothing in the game's code is written): both slot words are swapped (ss_write_word), each
// only while it still holds the known original. The bar hook writes the Y part and the Y bit into both bars, in
// place, only when the text is in the inline buffer and fits; anything else leaves the bar as it is (logged). The
// input hook, on Y with no resolution change waiting: asks the overlay to open (ss_menu_request_open; the Present
// hook opens it), plays the game's Menu/Menu/PromptSelect, and hands the block on with Y cleared (the screen does
// everything else as before). While the overlay is open, and after it closes until the closing button is let go,
// the game reads nothing pressed (menu.cpp), so the Options screen stays where it was.
//
// options_prompt = false (or menu = false): nothing is hooked.

#include <windows.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "menu.h"
#include "ss_shared.h"

#define ENTRY_THIS __attribute__((thiscall, force_align_arg_pointer))

namespace {

constexpr uintptr_t kVt = 0xBAE914;               // FE_options' vtable
constexpr uintptr_t kSlotInput = kVt + 11 * 4;    // 0xBAE940
constexpr uint32_t kOrigInput = 0x4715D0;
constexpr uintptr_t kSlotBar = kVt + 18 * 4;      // 0xBAE95C
constexpr uint32_t kOrigBar = 0x471240;
constexpr uint32_t kOrigSlot0 = 0x4713E0, kOrigSlot1 = 0x4714D0;  // neighbours checked: the known vtable
constexpr uintptr_t kPending = 0x1D5;             // a resolution change waits for its confirmation
constexpr uintptr_t kBarIdle = 0x1EC, kBarConfirm = 0x1F0;
constexpr uintptr_t kLanguage = 0xD3681C;         // 0..7, 9 = not read yet
constexpr uintptr_t kPlayEvent = 0x7845E0;        // cdecl (handle*, name, 0, 1, 0): plays a sound event
constexpr uintptr_t kReleaseEvent = 0x790B90;     // thiscall (handle*): lets the handle go
constexpr uintptr_t kPromptSelect = 0xBAA370;     // "Menu/Menu/PromptSelect" (the game's own string)
constexpr uint32_t kMaskY = 8;
constexpr int kYPart = 3;                         // A;B;X;Y;Start

// Our label by the game's language (the name table at 0xC4A844). Plain capitals without accents: the game's fonts
// are not known to have every letter, and Cyrillic would need the game's own text encoding; Russian keeps English.
const char* const kLabels[8] = {
    "PC-INSTELLINGEN",   // 0 Dutch
    "PC SETTINGS",       // 1 English
    "PARAMETRES PC",     // 2 French
    "IMPOSTAZIONI PC",   // 3 Italian
    "PC-EINSTELLUNGEN",  // 4 German
    "AJUSTES DE PC",     // 5 Spanish
    "PC SETTINGS",       // 6 Russian
    "USTAWIENIA PC",     // 7 Polish
};

const char* Label() {
  const int lang = *reinterpret_cast<const volatile int*>(kLanguage);
  return (lang >= 0 && lang < 8) ? kLabels[lang] : kLabels[1];
}

inline uint32_t Rd(uintptr_t va) { return *reinterpret_cast<const volatile uint32_t*>(va); }

bool Readable(uintptr_t p, size_t n) {
  if (!p) return false;
  MEMORY_BASIC_INFORMATION mi;
  const uintptr_t end = p + n;
  while (p < end) {
    if (!SS_VQ(reinterpret_cast<const void*>(p), &mi, sizeof mi)) return false;
    if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
      return false;
    p = reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize;
  }
  return true;
}

int g_bar_logs = 0;

// Puts our Y part and the Y bit into one bar, in place. True when the bar has our prompt now.
bool AddPrompt(uintptr_t bar, const char* which) {
  if (!bar || !Readable(bar, 0x4C)) return false;
  char* text = *reinterpret_cast<char**>(bar + 4);
  const uint32_t flags = Rd(bar + 8);
  const int cap = static_cast<int>(flags << 1) >> 4;
  const bool inline_buf = text == reinterpret_cast<char*>(bar + 0xC) && !(flags & 1) && cap > 0 && cap <= 0x40;
  if (!inline_buf) {
    if (g_bar_logs++ < 4)
      ss_log("[options] %s bar at %p: text %p not in its own buffer (flags %08x); left as it is", which, reinterpret_cast<void*>(bar),
             text, flags);
    return false;
  }
  // Split "A;B;X;Y;Start" (within the buffer).
  std::string cur(text, strnlen(text, cap));
  std::string parts[5];
  int n = 0;
  for (char c : cur) {
    if (c == ';') {
      if (++n > 4) break;
    } else if (n <= 4) {
      parts[n] += c;
    }
  }
  if (n != 4) {
    if (g_bar_logs++ < 4) ss_log("[options] %s bar text '%s' is not five parts; left as it is", which, cur.c_str());
    return false;
  }
  const char* label = Label();
  const uint32_t mask = Rd(bar);
  if (parts[kYPart] == label && (mask & kMaskY)) return true;  // already ours
  bool ours = false;  // one of our labels (the language can be read after a first write)
  for (const char* l : kLabels) ours |= parts[kYPart] == l;
  if (!parts[kYPart].empty() && !ours) {
    if (g_bar_logs++ < 4) ss_log("[options] %s bar has its own Y prompt '%s'; left as it is", which, parts[kYPart].c_str());
    return false;
  }
  parts[kYPart] = label;
  std::string out = parts[0] + ";" + parts[1] + ";" + parts[2] + ";" + parts[3] + ";" + parts[4];
  if (static_cast<int>(out.size()) + 1 > cap) {
    if (g_bar_logs++ < 4) ss_log("[options] %s bar: '%s' does not fit its %d bytes; left as it is", which, out.c_str(), cap);
    return false;
  }
  memcpy(text, out.c_str(), out.size() + 1);
  *reinterpret_cast<volatile uint32_t*>(bar) = mask | kMaskY;
  if (g_bar_logs++ < 4) ss_log("[options] %s bar: '%s' -> '%s', mask %u -> %u", which, cur.c_str(), out.c_str(), mask, mask | kMaskY);
  return true;
}

using BarFn = void(__attribute__((thiscall)) *)(void*);
using InputFn = void(__attribute__((thiscall)) *)(void*, unsigned char*);
using PlayFn = void*(__cdecl*)(void*, const char*, int, int, int);
using ReleaseFn = void(__attribute__((thiscall)) *)(void*);

void ENTRY_THIS BarHook(void* self) {
  const uintptr_t s = reinterpret_cast<uintptr_t>(self);
  if (self) {
    AddPrompt(Rd(s + kBarIdle), "Options");
    AddPrompt(Rd(s + kBarConfirm), "Options (confirm)");
  }
  reinterpret_cast<BarFn>(kOrigBar)(self);
}

void PlayPromptSelect() {
  uint32_t handle[4] = {0, 0, 0, 0};  // the game's handle is one pointer
  reinterpret_cast<PlayFn>(kPlayEvent)(handle, reinterpret_cast<const char*>(kPromptSelect), 0, 1, 0);
  reinterpret_cast<ReleaseFn>(kReleaseEvent)(handle);
}

void ENTRY_THIS InputHook(void* self, unsigned char* in) {
  const uintptr_t s = reinterpret_cast<uintptr_t>(self);
  if (self && in && in[kYPart] && !*reinterpret_cast<const volatile uint8_t*>(s + kPending) && !ss_menu_capturing()) {
    unsigned char block[10];
    memcpy(block, in, sizeof block);
    block[kYPart] = 0;
    ss_menu_request_open("Options screen, Y");
    PlayPromptSelect();
    reinterpret_cast<InputFn>(kOrigInput)(self, block);
    return;
  }
  reinterpret_cast<InputFn>(kOrigInput)(self, in);
}

bool Wanted() {
  if (!ss_conf_present() || !ss_menu_wanted()) return false;
  std::string v = ss_conf("options_prompt");
  if (const char* e = getenv("SS_OPTIONS_PROMPT")) v = e;
  for (char& c : v) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  if (v == "0" || v == "false" || v == "no" || v == "off") {
    ss_log("[options] options_prompt = false: no PC SETTINGS prompt on the Options screen");
    return false;
  }
  return true;
}

DWORD WINAPI Worker(LPVOID) {
  if (!Wanted()) return 0;
  if (GetModuleHandleA(nullptr) != reinterpret_cast<HMODULE>(0x400000)) {
    ss_log("[options] unexpected image base; no prompt on the Options screen");
    return 0;
  }
  const uint32_t bar = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&BarHook));
  const uint32_t input = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&InputHook));
  // The vtable is there once the image is unpacked: wait for its known neighbours, then check our two words.
  for (int i = 0; i < 240; ++i) {
    if (Rd(kVt) == kOrigSlot0 && Rd(kVt + 4) == kOrigSlot1) {
      const uint32_t cur_bar = Rd(kSlotBar), cur_in = Rd(kSlotInput);
      if (cur_bar != kOrigBar || cur_in != kOrigInput) {
        ss_log("[options] FE_options' slots 18 / 11 hold %08x / %08x, not the known build's; no prompt on the Options screen", cur_bar,
               cur_in);
        return 0;
      }
      if (!ss_write_word(reinterpret_cast<void*>(kSlotBar), bar) || !ss_write_word(reinterpret_cast<void*>(kSlotInput), input)) {
        ss_write_word(reinterpret_cast<void*>(kSlotBar), kOrigBar);
        ss_log("[options] cannot write FE_options' vtable (error %lu); no prompt on the Options screen", GetLastError());
        return 0;
      }
      ss_log("[options] Options screen: Y prompt '%s' (language %d) opens PC SETTINGS; prompt bar (%p) and input (%p) wrapped", Label(),
             *reinterpret_cast<const volatile int*>(kLanguage), reinterpret_cast<void*>(kSlotBar), reinterpret_cast<void*>(kSlotInput));
      return 0;
    }
    Sleep(250);
  }
  ss_log("[options] FE_options' vtable never looked like the known build's; no prompt on the Options screen");
  return 0;
}

}  // namespace

static MenuRegistrar g_options_row(MenuChoiceRow("options_prompt", "PC SETTINGS BUTTON IN OPTIONS", MENU_PAGE_GAME, MENU_RESTART,
                                                 "true", {{"false", "OFF"}, {"true", "ON"}}, nullptr));

void ss_options_entry_start() { CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr); }
