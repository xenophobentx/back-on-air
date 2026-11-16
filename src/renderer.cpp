// renderer.cpp: renderer = dxvk | native (Windows): which d3d9 the game draws with.
//
// The package keeps DXVK's 32-bit d3d9.dll in the dxvk folder beside the exe, not beside the exe itself. So the
// d3d9.dll the exe imports (a static import of the exe and of d3dx9_42.dll, bound by the loader before this DLL's
// DllMain runs) is Windows' own. The game calls Direct3DCreate9 once, at 0x50D340 through its import slot 0xB9B3BC.
//   * renderer = dxvk (the default): a 5-byte jump on the entry of Windows' d3d9!Direct3DCreate9 (system DLL memory,
//     nothing of the game; the same kind of jump as online.cpp's on ws2_32) sends every call, the game's and this
//     patch's own (display.cpp, dxvkwin.cpp), to Direct3DCreate9 of dxvk\d3d9.dll. That DLL is loaded by the first
//     call (on the caller's thread, not in DllMain under the loader lock). If it cannot be loaded, or its Direct3DCreate9
//     returns null (no Vulkan instance or adapter), the jump is taken out again and Windows' d3d9 answers that call
//     and every later one.
//   * renderer = native: nothing is changed: Windows' d3d9.
//   * A d3d9.dll beside the exe (the older layout): the exe imports that one, and it is used as it is, whatever the
//     key says (the dxvk folder is not used).
//   * Wine / Proton: the d3d9 is the one Proton gives (DXVK); nothing is changed and the row is not offered.
// DXVK reads dxvk.conf from the working directory (the game folder), wherever its DLL lies (dxvk config.cpp).
// Log lines [renderer].
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstring>
#include <string>

#include "menu.h"
#include "ss_shared.h"

namespace {

using CreateFn = IDirect3D9*(WINAPI*)(UINT);

wchar_t g_dxvk[MAX_PATH];   // <game folder>\dxvk\d3d9.dll
uint8_t* g_entry = nullptr;  // Windows' d3d9!Direct3DCreate9
uint8_t g_saved[5];          // its first five bytes, before the jump
CRITICAL_SECTION g_cs;
volatile LONG g_tried = 0;   // the first call has run: g_create is DXVK's, or null and the jump is out
CreateFn g_create = nullptr;
HMODULE g_module = nullptr;  // the DXVK module once loaded
bool g_routed = false;       // the jump went in at start
bool g_beside = false;       // a d3d9.dll beside the exe is the one the game imports

std::string Narrow(const wchar_t* w) {
  char s[MAX_PATH * 2];
  const int n = WideCharToMultiByte(CP_ACP, 0, w, -1, s, sizeof s, nullptr, nullptr);
  return n > 0 ? std::string(s) : std::string("?");
}

std::string PathOf(HMODULE m) {
  char p[MAX_PATH] = "?";
  if (m) GetModuleFileNameA(m, p, sizeof p);
  return p;
}

// Five bytes at t, swapped in one locked 8-byte write (the aligned qword when it holds all five; online.cpp's way).
bool Put5(uint8_t* t, const uint8_t* five) {
  const uintptr_t ta = reinterpret_cast<uintptr_t>(t);
  uint8_t* q = (ta & 7) <= 3 ? reinterpret_cast<uint8_t*>(ta & ~static_cast<uintptr_t>(7)) : t;
  const size_t at = static_cast<size_t>(t - q);
  DWORD old;
  if (!VirtualProtect(q, 8, PAGE_EXECUTE_READWRITE, &old)) return false;
  LONGLONG cur, now;
  memcpy(&cur, q, 8);
  uint8_t nb[8];
  memcpy(nb, &cur, 8);
  memcpy(nb + at, five, 5);
  memcpy(&now, nb, 8);
  const bool ok = InterlockedCompareExchange64(reinterpret_cast<volatile LONGLONG*>(q), now, cur) == cur;
  FlushInstructionCache(GetCurrentProcess(), q, 8);
  DWORD dummy;
  VirtualProtect(q, 8, old, &dummy);
  return ok;
}

IDirect3D9* WINAPI RoutedCreate9(UINT sdk) {
  EnterCriticalSection(&g_cs);
  if (!g_tried) {
    HMODULE m = LoadLibraryW(g_dxvk);
    const DWORD err = m ? 0 : GetLastError();
    CreateFn f = m ? reinterpret_cast<CreateFn>(GetProcAddress(m, "Direct3DCreate9")) : nullptr;
    if (f) {
      g_module = m;
      g_create = f;
      ss_log("[renderer] DXVK loaded: %s", PathOf(m).c_str());
    } else {
      const bool out = Put5(g_entry, g_saved);
      ss_log("[renderer] %s could not be loaded (%s, error %lu): Windows' own d3d9 is used%s", Narrow(g_dxvk).c_str(),
             m ? "no Direct3DCreate9 in it" : "LoadLibrary failed", err, out ? "" : "; the jump could not be taken out");
    }
    InterlockedExchange(&g_tried, 1);
  }
  LeaveCriticalSection(&g_cs);
  if (CreateFn f = g_create) {
    IDirect3D9* d = f(sdk);
    ss_log("[renderer] DXVK Direct3DCreate9(%u) = %p", sdk, static_cast<void*>(d));
    if (d) return d;
    // Null (DXVK found no Vulkan instance or adapter): the game would quit. Windows' own d3d9 from here on, as when
    // the DLL cannot be loaded.
    static bool out = false;  // the jump is out again (set by the first thread here)
    EnterCriticalSection(&g_cs);
    if (g_create) {
      out = Put5(g_entry, g_saved);
      g_create = nullptr;
      g_module = nullptr;
      ss_log("[renderer] DXVK gave no IDirect3D9: Windows' own d3d9 is used%s", out ? "" : "; the jump could not be taken out");
    }
    const bool back = out;
    LeaveCriticalSection(&g_cs);
    if (!back) return nullptr;  // the jump is still in: the entry would come back here
  }
  return reinterpret_cast<CreateFn>(g_entry)(sdk);  // the jump is out: Windows' own
}

bool InWindowsFolder(const char* path) {
  char win[MAX_PATH];
  const UINT wn = GetSystemWindowsDirectoryA(win, sizeof win);
  return wn && wn < sizeof win && strlen(path) > wn && !_strnicmp(path, win, wn) && (path[wn] == '\\' || path[wn] == '/');
}

void RegisterRow() {
  ss_menu_register(MenuChoiceRow("renderer", "RENDERER", MENU_PAGE_DISPLAY, MENU_RESTART, "dxvk",
                                 {{"dxvk", "DXVK (VULKAN)"}, {"native", "WINDOWS D3D9"}}, nullptr));
}

}  // namespace

// The active d3d9 module: DXVK from the dxvk folder once the first Direct3DCreate9 loaded it, else the d3d9.dll the
// exe imports.
HMODULE ss_d3d9_module() { return g_module ? g_module : GetModuleHandleA("d3d9.dll"); }

// True when the game will draw with DXVK on Windows: routed to the dxvk folder, or a d3d9.dll beside the exe.
bool ss_renderer_dxvk() { return (g_routed && !(g_tried && !g_create)) || g_beside; }

// From ss_patch_start (DllMain), before anything makes an IDirect3D9.
void ss_renderer_start() {
  HMODULE nt = GetModuleHandleA("ntdll.dll");
  HMODULE sys = GetModuleHandleA("d3d9.dll");
  const std::string sys_path = PathOf(sys);
  if (nt && GetProcAddress(nt, "wine_get_version")) {
    ss_log("[renderer] Wine / Proton: its own d3d9 (%s); the renderer key has no effect here", sys_path.c_str());
    return;
  }
  const std::string key = ss_conf("renderer");
  const bool native = key == "native";
  const char* asked = key.empty() ? "renderer = dxvk (default)" : native ? "renderer = native" : "renderer = dxvk";
  if (!sys) {
    ss_log("[renderer] no d3d9.dll loaded at start (%s): nothing changed", asked);
    return;
  }
  if (!InWindowsFolder(sys_path.c_str())) {
    g_beside = true;
    ss_log("[renderer] the game imports %s (a d3d9.dll beside the exe, the older layout): that one is used, whatever "
           "the renderer key says (%s); remove it to let the key choose",
           sys_path.c_str(), asked);
    return;
  }
  wchar_t dir[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, dir, MAX_PATH);
  wchar_t* slash = n && n < MAX_PATH ? wcsrchr(dir, L'\\') : nullptr;
  if (!slash || static_cast<size_t>(slash - dir) + 20 >= MAX_PATH) {
    ss_log("[renderer] the exe's folder is not known: Windows' own d3d9 (%s)", sys_path.c_str());
    return;
  }
  slash[1] = 0;
  wcscpy(g_dxvk, dir);
  wcscat(g_dxvk, L"dxvk\\d3d9.dll");
  const bool have = GetFileAttributesW(g_dxvk) != INVALID_FILE_ATTRIBUTES;
  if (have) RegisterRow();  // the choice is real only where both renderers are there
  if (native) {
    ss_log("[renderer] Windows' own d3d9 (%s; renderer = native)%s", sys_path.c_str(),
           have ? "" : "; no dxvk\\d3d9.dll either");
    return;
  }
  if (!have) {
    ss_log("[renderer] Windows' own d3d9 (%s): %s, but %s is not there", sys_path.c_str(), asked, Narrow(g_dxvk).c_str());
    return;
  }
  g_entry = reinterpret_cast<uint8_t*>(GetProcAddress(sys, "Direct3DCreate9"));
  if (!g_entry) {
    ss_log("[renderer] Windows' d3d9 has no Direct3DCreate9: it is used as it is");
    return;
  }
  InitializeCriticalSection(&g_cs);
  memcpy(g_saved, g_entry, 5);
  uint8_t jump[5] = {0xE9};
  const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint8_t*>(&RoutedCreate9) - (g_entry + 5));
  memcpy(jump + 1, &rel, 4);
  if (!Put5(g_entry, jump)) {
    ss_log("[renderer] cannot write Windows' d3d9!Direct3DCreate9 (error %lu): Windows' own d3d9 (%s)", GetLastError(),
           sys_path.c_str());
    return;
  }
  g_routed = true;
  ss_log("[renderer] DXVK (%s): Direct3DCreate9 of %s goes to %s", asked, sys_path.c_str(), Narrow(g_dxvk).c_str());
}
