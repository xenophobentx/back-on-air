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
// dxvk_engine_name (dxvk | auto | force): the Vulkan engine name DXVK gives the driver; see below, before
// RoutedCreate9. Both the dxvk folder and a d3d9.dll beside the exe.
// DXVK reads dxvk.conf from the working directory (the game folder), wherever its DLL lies (dxvk config.cpp).
// Log lines [renderer].
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

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

// ---- dxvk_engine_name: the Vulkan engine name DXVK gives the driver (issue #2, NVIDIA + Windows HDR) ----
// DXVK 2.7.1 makes its Vulkan instance with VkApplicationInfo.pEngineName = "DXVK" (dxvk_instance.cpp:237). A
// third-party bisection (NVIDIA developer forum 382517) found NVIDIA's Windows driver drops the display out of HDR
// for an instance named "DXVK", even with an HDR10 swap chain; the same program named otherwise keeps HDR. Our logs
// match (Windows HDR goes ON -> OFF whenever the game is in front, with a correct HDR10_ST2084 chain).
// How DXVK reaches vkCreateInstance (vulkan_loader.cpp, per DxvkInstance): LoadLibraryA("winevulkan.dll"), then
// "vulkan-1.dll"; kernel32!GetProcAddress(library, "vkGetInstanceProcAddr") through DXVK's own import slot; then
// vkGetInstanceProcAddr(NULL, "vkCreateInstance"). So DXVK's d3d9.dll import of KERNEL32.dll!GetProcAddress is
// redirected (a slot of DXVK's .idata, nothing of the game); it hands back our vkGetInstanceProcAddr, which hands back
// our vkCreateInstance for that one name and passes every other name through. Our vkCreateInstance copies the create
// info with pEngineName = "BackOnAir" (engine version kept) and, when the loader offers VK_EXT_swapchain_colorspace and
// DXVK did not ask for it (2.7.1 never does), adds it: the HDR10 colour space DXVK asks of the swap chain belongs to
// that extension. If the changed call fails, DXVK's own call is made as it was.
//   dxvk (the default; also an empty or unknown value): never renamed, DXVK as it ships. NVIDIA's driver may key other
//     DXVK handling on the name too, and the rename has not run on NVIDIA yet.
//   auto (test for #2): renamed when a display adapter is NVIDIA's (PCI vendor 0x10DE in EnumDisplayDevices). The
//     game's adapter is not known yet (the [display] line comes at CreateDevice, after the instance), so this is the
//     machine's adapters; an NVIDIA GPU that drives no display (some laptops) is not seen and nothing changes there.
//   force: renamed on any GPU (a test of the path on AMD / Intel).
// Windows only (under Wine / Proton ss_renderer_start changes nothing). Log line [renderer] Vulkan instance.
struct VkAppInfo {
  int32_t sType;
  const void* pNext;
  const char* pApplicationName;
  uint32_t applicationVersion;
  const char* pEngineName;
  uint32_t engineVersion;
  uint32_t apiVersion;
};
struct VkInstInfo {
  int32_t sType;
  const void* pNext;
  uint32_t flags;
  const VkAppInfo* pApplicationInfo;
  uint32_t enabledLayerCount;
  const char* const* ppEnabledLayerNames;
  uint32_t enabledExtensionCount;
  const char* const* ppEnabledExtensionNames;
};
struct VkExtProps {
  char extensionName[256];
  uint32_t specVersion;
};
using VkVoidFn = void(__stdcall*)();
using GipaFn = VkVoidFn(__stdcall*)(void* instance, const char* name);
using CreateInstanceFn = int32_t(__stdcall*)(const VkInstInfo* info, const void* alloc, void** instance);
using EnumExtFn = int32_t(__stdcall*)(const char* layer, uint32_t* count, VkExtProps* props);

const char kEngine[] = "BackOnAir";
const char kColorSpaceExt[] = "VK_EXT_swapchain_colorspace";
enum { ENGINE_AUTO, ENGINE_DXVK, ENGINE_FORCE };
int g_engine_mode = ENGINE_DXVK;
uintptr_t g_getproc = 0;                   // DXVK's import of kernel32!GetProcAddress, as it was
GipaFn g_gipa = nullptr;                   // vulkan-1!vkGetInstanceProcAddr, as DXVK found it
CreateInstanceFn g_create_instance = nullptr;  // the loader's vkCreateInstance
volatile LONG g_instances = 0;

// "NVIDIA" when a display adapter has PCI vendor 0x10DE, else the first adapter's name (for the log), once.
bool NvidiaAdapter(std::string* seen) {
  bool nv = false;
  DISPLAY_DEVICEA dd;
  for (DWORD i = 0; i < 16; ++i) {
    memset(&dd, 0, sizeof dd);
    dd.cb = sizeof dd;
    if (!EnumDisplayDevicesA(nullptr, i, &dd, 0)) break;
    if (strstr(dd.DeviceID, "VEN_10DE") || strstr(dd.DeviceID, "ven_10de")) nv = true;
    if (seen->find(dd.DeviceString) == std::string::npos) {
      if (!seen->empty()) *seen += ", ";
      *seen += dd.DeviceString;
    }
  }
  if (seen->empty()) *seen = "none listed";
  return nv;
}

bool ColorSpaceOffered() {
  EnumExtFn e = g_gipa ? reinterpret_cast<EnumExtFn>(g_gipa(nullptr, "vkEnumerateInstanceExtensionProperties")) : nullptr;
  uint32_t n = 0;
  if (!e || e(nullptr, &n, nullptr) != 0 || !n || n > 1024) return false;
  std::vector<VkExtProps> p(n);
  if (e(nullptr, &n, p.data()) < 0) return false;  // VK_INCOMPLETE (5) still lists n of them
  for (uint32_t i = 0; i < n && i < p.size(); ++i)
    if (!strcmp(p[i].extensionName, kColorSpaceExt)) return true;
  return false;
}

int32_t __stdcall HookCreateInstance(const VkInstInfo* ci, const void* alloc, void** out) {
  static std::string adapters;
  static const bool nvidia = NvidiaAdapter(&adapters);  // first call, on DXVK's thread (not in DllMain)
  const LONG k = InterlockedIncrement(&g_instances);
  const bool rename = g_engine_mode == ENGINE_FORCE || (g_engine_mode == ENGINE_AUTO && nvidia);
  const char* why = g_engine_mode == ENGINE_FORCE ? "dxvk_engine_name = force"
                    : nvidia                      ? "dxvk_engine_name = auto, an NVIDIA display adapter"
                                                  : "dxvk_engine_name = auto, no NVIDIA display adapter";
  const VkAppInfo* a = ci ? ci->pApplicationInfo : nullptr;
  if (!rename || !a) {
    const int32_t r = g_create_instance(ci, alloc, out);
    ss_log("[renderer] Vulkan instance %ld: engine name \"%s\" kept (%s; adapters: %s): vkCreateInstance = %d", k,
           a && a->pEngineName ? a->pEngineName : "?", why, adapters.c_str(), r);
    return r;
  }
  VkAppInfo app = *a;
  app.pEngineName = kEngine;
  VkInstInfo info = *ci;
  info.pApplicationInfo = &app;
  bool asked = false;
  for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i)
    if (ci->ppEnabledExtensionNames[i] && !strcmp(ci->ppEnabledExtensionNames[i], kColorSpaceExt)) asked = true;
  std::vector<const char*> names(ci->ppEnabledExtensionNames, ci->ppEnabledExtensionNames + ci->enabledExtensionCount);
  const bool add = !asked && ColorSpaceOffered();
  if (add) {
    names.push_back(kColorSpaceExt);
    info.enabledExtensionCount = static_cast<uint32_t>(names.size());
    info.ppEnabledExtensionNames = names.data();
  }
  int32_t r = g_create_instance(&info, alloc, out);
  const char* ext = asked ? "DXVK asked for " : add ? "added " : "not offered: ";
  if (r == 0) {
    ss_log("[renderer] Vulkan instance %ld: engine name \"%s\" -> \"%s\" (%s; adapters: %s; %s%s): vkCreateInstance = 0",
           k, a->pEngineName ? a->pEngineName : "(null)", kEngine, why, adapters.c_str(), ext, kColorSpaceExt);
    return r;
  }
  const int32_t changed = r;
  r = g_create_instance(ci, alloc, out);
  ss_log("[renderer] Vulkan instance %ld: renamed \"%s\" (%s%s) failed (%d); DXVK's own call: vkCreateInstance = %d", k,
         kEngine, ext, kColorSpaceExt, changed, r);
  return r;
}

VkVoidFn __stdcall HookGipa(void* instance, const char* name) {
  VkVoidFn f = g_gipa(instance, name);
  if (f && !instance && name && !strcmp(name, "vkCreateInstance")) {
    g_create_instance = reinterpret_cast<CreateInstanceFn>(f);
    return reinterpret_cast<VkVoidFn>(&HookCreateInstance);
  }
  return f;
}

FARPROC WINAPI HookGetProc(HMODULE m, LPCSTR name) {
  FARPROC f = reinterpret_cast<FARPROC(WINAPI*)(HMODULE, LPCSTR)>(g_getproc)(m, name);
  if (f && (reinterpret_cast<uintptr_t>(name) >> 16) && !strcmp(name, "vkGetInstanceProcAddr")) {
    g_gipa = reinterpret_cast<GipaFn>(f);
    return reinterpret_cast<FARPROC>(&HookGipa);
  }
  return f;
}

// DXVK's module is there (loaded by RoutedCreate9, or a d3d9.dll beside the exe): before its first Direct3DCreate9.
void HookEngineName(HMODULE dxvk) {
  if (g_engine_mode == ENGINE_DXVK) {
    ss_log("[renderer] dxvk_engine_name = dxvk (default): the Vulkan engine name stays \"DXVK\" (auto: the NVIDIA HDR "
           "test, #2)");
    return;
  }
  if (g_getproc) return;
  if (!ss_hook_import_at(dxvk, "KERNEL32.dll", "GetProcAddress", reinterpret_cast<uintptr_t>(&HookGetProc), &g_getproc))
    ss_log("[renderer] dxvk_engine_name: no KERNEL32.dll!GetProcAddress import in %s: the engine name is not changed",
           PathOf(dxvk).c_str());
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
      HookEngineName(m);
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
  const std::string engine = ss_conf("dxvk_engine_name");
  g_engine_mode = engine == "auto" ? ENGINE_AUTO : engine == "force" ? ENGINE_FORCE : ENGINE_DXVK;
  if (!engine.empty() && engine != "auto" && engine != "dxvk" && engine != "force")
    ss_log("[renderer] dxvk_engine_name = %s is not dxvk, auto or force: dxvk", engine.c_str());
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
    HookEngineName(sys);  // DXVK is loaded already, but makes no Vulkan instance before its first Direct3DCreate9
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
