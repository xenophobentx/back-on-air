// dinput8.dll proxy: forwards the DirectInput8 exports to the system DLL and
// starts the patcher (patch.cpp) from a thread of its own.
#include <windows.h>
#include "ss_common.h"

void ss_patch_start(HMODULE self);
void ss_intro_start();  // intro.cpp: skip_intro
void ss_display_start();  // display.cpp: window_mode, pause_on_focus_loss, display_log
void ss_glyphs_start();  // glyphs.cpp: button_prompts (nothing at all without a conf or an SS_* switch)
void ss_hdr_start();  // hdr.cpp: HDR output (hdr) and the feasibility probe (hdr_probe); nothing at all when both are off
void ss_postfx_start();  // postfx.cpp: anti-aliasing and sharpening (nothing at all when both are off)
void ss_online_start();  // online.cpp: online_server (default: the project's server; off: nothing at all)

static HMODULE g_real = nullptr;

static HMODULE Real() {
  if (!g_real) {
    char dir[MAX_PATH];
    UINT n = GetSystemDirectoryA(dir, MAX_PATH);
    if (n == 0 || n > MAX_PATH - 16) return nullptr;
    lstrcatA(dir, "\\dinput8.dll");
    g_real = LoadLibraryA(dir);
  }
  return g_real;
}

template <typename F>
static F Fn(const char* name) {
  HMODULE m = Real();
  return m ? reinterpret_cast<F>(GetProcAddress(m, name)) : nullptr;
}

extern "C" {
HRESULT WINAPI ss_DirectInput8Create(HINSTANCE h, DWORD v, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
  using F = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
  static F f = Fn<F>("DirectInput8Create");
  if (!f) return E_FAIL;
  HRESULT hr = f(h, v, riid, out, outer);
  // The controller layer (pad.cpp) swaps IDirectInput8A for its wrapper; every other interface is left alone.
  if (SUCCEEDED(hr) && out && !outer) ss_pad_wrap_directinput(&riid, out);
  return hr;
}
HRESULT WINAPI ss_DllCanUnloadNow() {
  using F = HRESULT(WINAPI*)();
  static F f = Fn<F>("DllCanUnloadNow");
  return f ? f() : S_FALSE;
}
HRESULT WINAPI ss_DllGetClassObject(REFCLSID c, REFIID i, LPVOID* o) {
  using F = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
  static F f = Fn<F>("DllGetClassObject");
  return f ? f(c, i, o) : E_FAIL;
}
HRESULT WINAPI ss_DllRegisterServer() {
  using F = HRESULT(WINAPI*)();
  static F f = Fn<F>("DllRegisterServer");
  return f ? f() : E_FAIL;
}
HRESULT WINAPI ss_DllUnregisterServer() {
  using F = HRESULT(WINAPI*)();
  static F f = Fn<F>("DllUnregisterServer");
  return f ? f() : E_FAIL;
}
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(inst);
    ss_patch_start(inst);
    ss_intro_start();
    ss_glyphs_start();   // before ss_display_start: it asks whether the device is wanted at its creation
    ss_hdr_start();      // before ss_display_start and ss_postfx_start: both ask it what it needs of them
    ss_display_start();  // after ss_patch_start: needs the conf directory and the log
    ss_postfx_start();
    ss_online_start();
  }
  return TRUE;
}
