// dxvkwin.cpp: DXVK on Windows inside the 2 GB of the exe (dxvk_fit; docs/plan-hdr.md section 17.6).
//
// SplitSecond.exe has no large-address flag, so on Windows it has 2 GB of address space. With DXVK's 32-bit
// d3d9.dll beside the exe the race load ran out of it (measured with vm_log on a Radeon RX 9060 XT):
//   * Every IDirect3D9 object of DXVK is a Vulkan instance. When the last one is released the Vulkan driver DLL is
//     unloaded, and AMD's driver leaves a 128 MB reservation behind each time; the next Direct3DCreate9 loads the
//     driver again and reserves another 128 MB. This DLL's own look at the vtable and the game's start-up made and
//     released several objects: about 380 MB of address space lost before the first frame.
//     Fix: one IDirect3D9 is made at start and kept for the life of the process, so the driver is loaded once.
//   * The exe's header gives every thread without a size of its own a 5 MB stack reservation; there are about 50
//     threads and most belong to DLLs (audio, drivers, this patch). Fix: such a thread gets 1 MB (the Windows
//     default for other programs) unless it starts in the game's own code. One slot of kernelbase.dll's import
//     table (NtCreateThreadEx) is redirected: system DLL memory, nothing of the game.
// dxvk_fit = auto (default): both, when this is Windows (not Wine) and the game draws with DXVK (renderer.cpp).
// dxvk_fit = true: both, on any Windows d3d9. dxvk_fit = false: nothing.
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "ss_shared.h"

HMODULE ss_d3d9_module();  // renderer.cpp: the d3d9 module that answers Direct3DCreate9

namespace {

using NtCreateThreadExFn = LONG(NTAPI*)(PHANDLE, ACCESS_MASK, PVOID, HANDLE, PVOID, PVOID, ULONG, SIZE_T, SIZE_T, SIZE_T, PVOID);
NtCreateThreadExFn g_create_thread = nullptr;
constexpr SIZE_T kStack = 1u << 20;
volatile LONG g_small = 0, g_kept = 0;  // threads given the small reservation; threads left alone
volatile LONG g_on = 0, g_hold = 0;     // dxvk_fit applies; the kept object: 0 not tried, 1 under way, 2 done

LONG NTAPI HookCreateThread(PHANDLE h, ACCESS_MASK access, PVOID attr, HANDLE process, PVOID start, PVOID arg, ULONG flags,
                            SIZE_T zero, SIZE_T stack, SIZE_T reserve, PVOID list) {
  if (process == GetCurrentProcess() && !reserve && stack <= kStack / 4) {
    MEMORY_BASIC_INFORMATION mbi;
    const bool game = VirtualQuery(start, &mbi, sizeof mbi) && reinterpret_cast<uintptr_t>(mbi.AllocationBase) == 0x400000;
    if (!game) {
      reserve = kStack;
      InterlockedIncrement(&g_small);
    } else {
      InterlockedIncrement(&g_kept);
    }
  }
  return g_create_thread(h, access, attr, process, start, arg, flags, zero, stack, reserve, list);
}

DWORD WINAPI HoldThread(LPVOID) {
  ss_dxvkwin_hold();
  return 0;
}

}  // namespace

// Makes the kept IDirect3D9 (once; a second caller waits for the first). display.cpp calls it before its own
// short-lived object, so that one's Release is never the last.
void ss_dxvkwin_hold() {
  if (!g_on) return;
  if (InterlockedCompareExchange(&g_hold, 1, 0) != 0) {
    for (int i = 0; i < 3000 && g_hold != 2; ++i) Sleep(10);
    return;
  }
  HMODULE d3d9 = LoadLibraryA("d3d9.dll");  // the module the game imports (renderer.cpp may send its entry to DXVK)
  using CreateFn = IDirect3D9*(WINAPI*)(UINT);
  CreateFn create = d3d9 ? reinterpret_cast<CreateFn>(GetProcAddress(d3d9, "Direct3DCreate9")) : nullptr;
  IDirect3D9* keep = create ? create(D3D_SDK_VERSION) : nullptr;
  d3d9 = ss_d3d9_module();  // the one that answered
  char path[MAX_PATH] = "?";
  if (d3d9) GetModuleFileNameA(d3d9, path, sizeof path);
  const bool dxvk = d3d9 && GetProcAddress(d3d9, "DXVK_RegisterAnnotation");
  ss_log("[dxvk_fit] %s (%s%s); threads of DLLs get a 1 MB stack reservation, not the exe's 5 MB",
         keep ? "one IDirect3D9 is kept for the whole run, so the Vulkan driver is loaded once" : "no IDirect3D9 could be made to keep",
         path, dxvk ? ", DXVK" : "");
  InterlockedExchange(&g_hold, 2);
}


void ss_dxvkwin_start() {
  const char* e = getenv("SS_DXVK_FIT");
  const std::string v = e ? e : ss_conf("dxvk_fit");
  if (v == "0" || v == "false" || v == "off" || v == "no") return;
  HMODULE nt = GetModuleHandleA("ntdll.dll");
  if (nt && GetProcAddress(nt, "wine_get_version")) return;  // Wine: its own address space rules, nothing to fix
  const bool forced = v == "1" || v == "true" || v == "on" || v == "yes";
  if (!forced) {
    if (!ss_renderer_dxvk()) return;  // Windows' own d3d9: nothing
  }
  g_on = 1;
  const bool hooked = ss_hook_import("kernelbase.dll", "ntdll.dll", "NtCreateThreadEx", reinterpret_cast<uintptr_t>(HookCreateThread),
                                     reinterpret_cast<uintptr_t*>(&g_create_thread));
  ss_log("[dxvk_fit] on (%s)%s", forced ? "dxvk_fit = true" : "Windows, DXVK",
         hooked ? "" : "; the thread stack size could not be hooked (stacks stay 5 MB)");
  CreateThread(nullptr, 0, HoldThread, nullptr, 0, nullptr);
}
