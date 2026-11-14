// Display: window_mode (fullscreen | borderless | windowed), pause_on_focus_loss, display_log.
//
// Nothing here writes the game's image. The levers are
//   * one slot of the IDirect3D9 vtable (CreateDevice, slot 16) and, on the device the game creates, the Reset
//     slot (16) and the resource-creation slots of the IDirect3DDevice9 vtable. Both vtables live in d3d9.dll's
//     memory (DXVK, wined3d or the Windows runtime), the same kind of write as the Present hook in patch.cpp;
//   * ordinary user32 calls on the game's window (SetWindowLongA / SetWindowPos, a window-procedure subclass).
//
// How the game sets up its display (reverse engineering of the Steam build 385319, run-time VAs):
//   * 0xA13880 creates the window: CreateWindowExA (0xA13BD8), then SetWindowLongA(GWL_STYLE) (0xA13C10) with
//     WS_POPUP, or WS_CAPTION only when IsDebuggerPresent() says so (0xA139FA: the style kind, window+0x154 =
//     1 captioned / 2 popup), and 0xA12AF0 (AdjustWindowRectEx 0xA12B53 + MoveWindow(0, 0, w, h) 0xA12B78).
//     Before that, 0xA1392F -> 0x50D340 calls Direct3DCreate9 through 0x89F0D0 (a LoadLibrary of an optional
//     third-party DLL, else the import) and keeps the IDirect3D9 at renderer+0x574 (renderer = *0xD64708).
//   * 0x50DED0 fills the D3DPRESENT_PARAMETERS at renderer+0x57C: Windowed (+0x59C) = (style kind == 1), so
//     a normal start is always exclusive fullscreen; SwapEffect DISCARD; PresentationInterval from the vsync
//     option, IMMEDIATE when windowed. 0x50E190 calls IDirect3D9::CreateDevice (0x50E295, [vtbl+0x40]) with
//     the device written straight to renderer+0x578.
//   * Lost device: Present (0x50E4A7) returning D3DERR_DEVICELOST sets renderer+0xB48 = 1; the frame start
//     0x50E8D0 then polls TestCooperativeLevel (0x50E926) and on D3DERR_DEVICENOTRESET calls 0x50E620, which
//     releases its D3DPOOL_DEFAULT objects through the engine's resource registry (list at 0xD18060, 0x52EBD0),
//     unbinds every stream / texture / shader / target (0x50D1D0), calls Reset (0x50E6B2) and re-creates them
//     (0x52EB40). A failed Reset logs "Unable to reset D3D device", sets +0xB48 = 2 and is retried each frame.
//   * The resolution menu (SetDisplayMode, 0x470C28 / 0x470C4D) restyles and moves the window (0xA13150) and
//     then resets the device (0x50E970 -> 0x50DED0 + 0x50E620).
//   * Window procedure 0xA12BA0: WM_ACTIVATE sets the "application active" byte 0xD57108 (1 active, 0 not);
//     inactive mutes the audio (0x86C783, 0x86CDDB) and requests the pause menu in a race (0x4B0914).
//     WM_ACTIVATEAPP is not handled by the game (the D3D9 runtime handles it for exclusive fullscreen).
//
// What this file does:
//   * borderless / windowed: CreateDevice and Reset get Windowed = TRUE (the back buffer stays the game's
//     resolution; Present scales it to the window). After the device is created or reset, the window gets
//     WS_POPUP and the whole monitor (borderless) or a caption with a client area of the back buffer's size,
//     centred on the monitor (windowed). A windowed device is not lost on alt-tab, so alt-tab out and back
//     needs no Reset at all.
//   * pause_on_focus_loss = false: a deactivating WM_ACTIVATE is not passed to the game's window procedure, so
//     the game keeps running and keeps its sound when another window is in front.
//   * display_log = true: every D3DPOOL_DEFAULT object the game creates gets a private-data marker; every Reset
//     is logged, and a failed Reset lists the DEFAULT-pool objects still alive with the code that created them
//     (the objects a Reset needs released). This is the diagnostic for exclusive-fullscreen alt-tab.
//   * frame_pacing = smooth / vsync / vrr / auto (patch.cpp's ss_frame_vsync): CreateDevice and Reset get
//     PresentationInterval ONE when the game asked IMMEDIATE (its vsync option off); the game is told it got what it
//     asked for. frame_pacing = fastest (ss_frame_immediate): they get IMMEDIATE whatever the game asked.
//   * game_vsync = game (default) | off | on: overrides the game's own vsync option (config_ss.cfg) at CreateDevice
//     and Reset: off = IMMEDIATE, on = ONE; game leaves the game's interval alone. Applied before frame_pacing, so
//     frame_pacing wins: smooth still turns IMMEDIATE into ONE (its pacing needs the vblank), fastest ONE into
//     IMMEDIATE. With frame_pacing = present or step, game_vsync alone decides.
//
// The hook goes into the shared IDirect3D9 vtable from a thread started at DLL load: the game creates its device
// only after the copy protection has unpacked it and the engine has set up, so the hook is in place first. If
// the device already exists (logged), only the window procedure part is applied.
// window_mode = fullscreen (the default) with the other two keys at their defaults (and no HDR, no vsync) does nothing at all: no
// thread, no Direct3D call, no hook (under gamescope / Steam Deck the window mode is moot anyway).
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include "menu.h"
#include "ss_shared.h"

namespace {

enum WindowMode { WM_FULLSCREEN = 0, WM_BORDERLESS = 1, WM_WINDOWED = 2 };
const char* const kModeName[] = {"fullscreen", "borderless", "windowed"};

int g_mode = WM_FULLSCREEN;
volatile bool g_pause_on_focus_loss = true;  // the settings menu may change it while the subclass is in
bool g_track = false;  // display_log
// hdr (hdr.cpp: A2R10G10B10) or hdr_probe_backbuffer (hdr_probe.cpp): asked for instead of the game's back buffer
// format. When the runtime refuses it (CreateDevice or Reset fails), the call is made again with the game's own
// format and the game never sees the failure.
D3DFORMAT g_bb_format = D3DFMT_UNKNOWN;
// Test aid, SS_HDR_TEST_REFUSE=create | reset: the call with that format is answered D3DERR_INVALIDCALL without
// asking the runtime (DXVK takes A2R10G10B10 fullscreen and windowed), so the way back to the game's format runs.
int g_test_refuse = 0;  // 1 create, 2 reset

constexpr uintptr_t kRenderer = 0xD64708;  // renderer object; IDirect3D9 at +0x574, device at +0x578
constexpr int kD3DCreateDevice = 16;       // IDirect3D9 vtable slot
constexpr int kDevReset = 16;              // IDirect3DDevice9 vtable slots
constexpr int kDevCreateTexture = 23, kDevCreateVolumeTexture = 24, kDevCreateCubeTexture = 25;
constexpr int kDevCreateVertexBuffer = 26, kDevCreateIndexBuffer = 27, kDevCreateRenderTarget = 28;
constexpr int kDevCreateDepthStencil = 29, kDevCreateOffscreenPlain = 36;

std::string Setting(const char* env, const char* key) {
  const char* e = getenv(env);
  return e ? std::string(e) : ss_conf(key);
}
bool IsOff(const std::string& v) { return v == "0" || v == "false" || v == "no" || v == "off"; }
bool IsOn(const std::string& v) { return v == "1" || v == "true" || v == "yes" || v == "on"; }

bool Readable(const void* p, size_t n) {
  MEMORY_BASIC_INFORMATION mi;
  if (!p || !SS_VQ(p, &mi, sizeof mi) || mi.State != MEM_COMMIT) return false;
  if (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
  return reinterpret_cast<uintptr_t>(p) + n <= reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize;
}

// "module+offset" of a code address, for the log.
void Where(const void* addr, char* out, size_t n) {
  HMODULE m = nullptr;
  char path[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<const char*>(addr), &m) && m) {
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* s = strrchr(path, '\\');
    snprintf(out, n, "%s+0x%lx (%p)", s ? s + 1 : path,
             static_cast<unsigned long>(reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(m)), addr);
  } else {
    snprintf(out, n, "%p", addr);
  }
}

// One vtable word in a d3d9 module's memory. Returns the previous value, or 0 when it could not be written.
uintptr_t SwapSlot(void** vt, int slot, void* fn) {
  uintptr_t prev = 0;
  if (!ss_write_word(&vt[slot], reinterpret_cast<uintptr_t>(fn), &prev)) return 0;
  return prev;
}

// ------------------------------------------------------------------ window

HWND g_hwnd = nullptr;
WNDPROC g_prev_proc = nullptr;

LRESULT CALLBACK WindowProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE && !g_pause_on_focus_loss) {
    // The game's procedure would clear its "active" byte (pause + mute); the default handling still runs.
    return DefWindowProcA(h, msg, wp, lp);
  }
  return CallWindowProcA(g_prev_proc, h, msg, wp, lp);
}

void Subclass(HWND h) {
  if (g_pause_on_focus_loss || !h || g_prev_proc) return;
  // The old procedure is stored before ours is in place: a message can reach WindowProc as soon as the swap is done.
  WNDPROC prev = reinterpret_cast<WNDPROC>(GetWindowLongPtrA(h, GWLP_WNDPROC));
  if (prev) {
    g_prev_proc = prev;
    SetLastError(0);
    WNDPROC was = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WindowProc)));
    if (!was && GetLastError() != 0) g_prev_proc = nullptr;  // not installed
    else if (was && was != prev) g_prev_proc = was;          // changed in between: chain to what was really there
  }
  if (g_prev_proc) ss_log("[display] pause_on_focus_loss off: the game keeps running behind other windows (window %p)", h);
  else ss_log("[display] cannot subclass the window (error %lu); pause_on_focus_loss stays on", GetLastError());
}

// The window around a windowed device: the whole monitor (borderless), or a captioned window whose client area is
// the back buffer, centred on the monitor's work area (windowed). Runs on the game's thread after CreateDevice and
// after every Reset (the game itself restyles and moves the window to (0, 0, w, h) just before a Reset).
void LayOutWindow(HWND h, UINT bw, UINT bh) {
  if (!h || !IsWindow(h) || IsIconic(h)) return;
  MONITORINFO mi = {};
  mi.cbSize = sizeof mi;
  if (!GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi)) return;
  LONG style = GetWindowLongA(h, GWL_STYLE);
  LONG ex = GetWindowLongA(h, GWL_EXSTYLE);
  const LONG frame = WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_BORDER | WS_DLGFRAME;
  ex &= ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);
  RECT r;
  if (g_mode == WM_BORDERLESS) {
    style = (style & ~frame) | WS_POPUP | WS_VISIBLE;
    r = mi.rcMonitor;
  } else {
    style = (style & ~(frame | WS_POPUP)) | WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
    RECT c = {0, 0, static_cast<LONG>(bw), static_cast<LONG>(bh)};
    AdjustWindowRectEx(&c, static_cast<DWORD>(style), FALSE, static_cast<DWORD>(ex));
    const LONG w = c.right - c.left, hh = c.bottom - c.top;
    const RECT& wa = mi.rcWork;
    LONG x = wa.left + ((wa.right - wa.left) - w) / 2, y = wa.top + ((wa.bottom - wa.top) - hh) / 2;
    if (x < wa.left) x = wa.left;
    if (y < wa.top) y = wa.top;
    r = {x, y, x + w, y + hh};
  }
  SetWindowLongA(h, GWL_STYLE, style);
  SetWindowLongA(h, GWL_EXSTYLE, ex);
  SetWindowPos(h, HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
               SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
  static int said = 0;
  if (said++ < 8)
    ss_log("[display] window %p: %s, %ldx%ld at (%ld,%ld), back buffer %ux%u", h, kModeName[g_mode], r.right - r.left,
           r.bottom - r.top, r.left, r.top, bw, bh);
}

// The game's present parameters, made windowed. The back buffer keeps the game's size and format.
void MakeWindowed(D3DPRESENT_PARAMETERS& pp) {
  pp.Windowed = TRUE;
  pp.FullScreen_RefreshRateInHz = 0;
  if (pp.PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE && pp.PresentationInterval != D3DPRESENT_INTERVAL_DEFAULT)
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;  // TWO..FOUR are fullscreen-only on some runtimes
}

// frame_pacing = vsync: the device presents on the vblank (interval ONE). The game's own vsync option (ONE already)
// and the fullscreen-only TWO..FOUR are left as they are; only IMMEDIATE (the game's vsync off) is changed.
// game_vsync (off / on) is applied first; see the header for the precedence.
enum { GV_GAME, GV_OFF, GV_ON };
int g_game_vsync = GV_GAME;
const char* const kGameVsyncName[] = {"game", "off", "on"};

void Vsync(D3DPRESENT_PARAMETERS& pp) {
  const UINT asked = pp.PresentationInterval;
  if (g_game_vsync == GV_OFF) pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  else if (g_game_vsync == GV_ON) pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
  const bool fp = ss_frame_vsync() && pp.PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE;
  if (fp) pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
  const bool fast = ss_frame_immediate() && pp.PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE;
  if (fast) pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  if (g_game_vsync != GV_GAME || fp || fast)
    ss_log("[display] present interval 0x%lx (the game asked 0x%lx; game_vsync = %s%s)", static_cast<unsigned long>(pp.PresentationInterval),
           static_cast<unsigned long>(asked), kGameVsyncName[g_game_vsync],
           fp ? ", frame_pacing = smooth / vrr puts it on the vblank" : fast ? ", frame_pacing = fastest presents at once" : "");
}

// What the runtime wrote back into our copy goes back to the game, except the fields we changed: the game keeps
// believing it asked for (and got) what it asked for.
void CopyBack(D3DPRESENT_PARAMETERS* game, const D3DPRESENT_PARAMETERS& mine) {
  const BOOL w = game->Windowed;
  const UINT rr = game->FullScreen_RefreshRateInHz, pi = game->PresentationInterval;
  const D3DFORMAT bf = game->BackBufferFormat;
  *game = mine;
  if (g_bb_format != D3DFMT_UNKNOWN) game->BackBufferFormat = bf;
  game->Windowed = w;
  game->FullScreen_RefreshRateInHz = rr;
  game->PresentationInterval = pi;
}

// ----------------------------------------------- D3DPOOL_DEFAULT object tracker (display_log)

// Every DEFAULT-pool object gets an IUnknown as private data (D3DSPD_IUNKNOWN); the runtime releases it when the
// object is destroyed, so the marker list is exactly the set of DEFAULT objects alive.
CRITICAL_SECTION g_lock;
const char* const kKindName[] = {"texture", "volume texture", "cube texture", "vertex buffer", "index buffer",
                                 "render target", "depth stencil", "offscreen surface"};
const GUID kMarkerGuid = {0x5e1c7a20, 0x3b4d, 0x4c55, {0x9a, 0x61, 0x2d, 0x8f, 0x40, 0x17, 0xc3, 0x0b}};

struct Marker final : IUnknown {
  LONG refs = 1;
  int kind;
  UINT w, h;
  DWORD fmt, usage;
  void* caller;
  Marker* prev = nullptr;
  Marker* next = nullptr;
  Marker(int k, UINT w_, UINT h_, DWORD f, DWORD u, void* c) : kind(k), w(w_), h(h_), fmt(f), usage(u), caller(c) {}
  virtual ~Marker() = default;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown)) {
      *out = static_cast<IUnknown*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs)); }
  ULONG STDMETHODCALLTYPE Release() override;
};
Marker* g_head = nullptr;
LONG g_created = 0;

ULONG Marker::Release() {
  LONG n = InterlockedDecrement(&refs);
  if (n == 0) {
    EnterCriticalSection(&g_lock);
    if (prev) prev->next = next; else if (g_head == this) g_head = next;
    if (next) next->prev = prev;
    LeaveCriticalSection(&g_lock);
    delete this;
  }
  return static_cast<ULONG>(n);
}

// The first return address on the stack that lies in the game's image (D3DX calls in between are skipped).
void* GameCaller(void* direct) {
  void* frames[12];
  USHORT n = CaptureStackBackTrace(1, 12, frames, nullptr);
  const uintptr_t lo = 0x401000, hi = 0xB9B000;  // the game's code section
  for (USHORT i = 0; i < n; ++i) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(frames[i]);
    if (a >= lo && a < hi) return frames[i];
  }
  return direct;
}

void Track(IDirect3DResource9* res, int kind, UINT w, UINT h, DWORD fmt, DWORD usage, void* ret) {
  if (!res) return;
  Marker* m = new Marker(kind, w, h, fmt, usage, GameCaller(ret));
  EnterCriticalSection(&g_lock);
  m->next = g_head;
  if (g_head) g_head->prev = m;
  g_head = m;
  LeaveCriticalSection(&g_lock);
  InterlockedIncrement(&g_created);
  // The runtime takes its own reference; ours is dropped right after, so the object's destruction frees the marker.
  if (FAILED(res->SetPrivateData(kMarkerGuid, static_cast<IUnknown*>(m), sizeof(IUnknown*), D3DSPD_IUNKNOWN))) {
    static int said = 0;
    if (said++ < 3) ss_log("[display] SetPrivateData failed on a %s; it is not tracked", kKindName[kind]);
  }
  m->Release();
}

int AliveCount() {
  EnterCriticalSection(&g_lock);
  int n = 0;
  for (Marker* m = g_head; m; m = m->next) ++n;
  LeaveCriticalSection(&g_lock);
  return n;
}

void LogAlive(int limit) {
  EnterCriticalSection(&g_lock);
  int n = 0;
  for (Marker* m = g_head; m; m = m->next, ++n) {
    if (n >= limit) continue;
    char where[160];
    Where(m->caller, where, sizeof where);
    ss_log("[display]   alive: %s %ux%u format %lu usage 0x%lx, created from %s", kKindName[m->kind], m->w, m->h,
           static_cast<unsigned long>(m->fmt), static_cast<unsigned long>(m->usage), where);
  }
  LeaveCriticalSection(&g_lock);
  if (n > limit) ss_log("[display]   ... and %d more", n - limit);
}

using CreateTextureFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
using CreateVolumeTextureFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                               IDirect3DVolumeTexture9**, HANDLE*);
using CreateCubeTextureFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9**, HANDLE*);
using CreateVertexBufferFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*);
using CreateIndexBufferFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9**, HANDLE*);
using CreateSurfaceRtFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL,
                                           IDirect3DSurface9**, HANDLE*);
using CreateOffscreenFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DPOOL, IDirect3DSurface9**, HANDLE*);

CreateTextureFn o_tex = nullptr;
CreateVolumeTextureFn o_vol = nullptr;
CreateCubeTextureFn o_cube = nullptr;
CreateVertexBufferFn o_vb = nullptr;
CreateIndexBufferFn o_ib = nullptr;
CreateSurfaceRtFn o_rt = nullptr, o_ds = nullptr;
CreateOffscreenFn o_off = nullptr;

#define RET_ADDR __builtin_return_address(0)

HRESULT WINAPI HookTex(IDirect3DDevice9* d, UINT w, UINT h, UINT lv, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DTexture9** o, HANDLE* s) {
  HRESULT hr = o_tex(d, w, h, lv, u, f, p, o, s);
  if (SUCCEEDED(hr) && p == D3DPOOL_DEFAULT && o) Track(*o, 0, w, h, f, u, RET_ADDR);
  return hr;
}
HRESULT WINAPI HookVol(IDirect3DDevice9* d, UINT w, UINT h, UINT dp, UINT lv, DWORD u, D3DFORMAT f, D3DPOOL p,
                       IDirect3DVolumeTexture9** o, HANDLE* s) {
  HRESULT hr = o_vol(d, w, h, dp, lv, u, f, p, o, s);
  if (SUCCEEDED(hr) && p == D3DPOOL_DEFAULT && o) Track(*o, 1, w, h, f, u, RET_ADDR);
  return hr;
}
HRESULT WINAPI HookCube(IDirect3DDevice9* d, UINT e, UINT lv, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DCubeTexture9** o, HANDLE* s) {
  HRESULT hr = o_cube(d, e, lv, u, f, p, o, s);
  if (SUCCEEDED(hr) && p == D3DPOOL_DEFAULT && o) Track(*o, 2, e, e, f, u, RET_ADDR);
  return hr;
}
HRESULT WINAPI HookVb(IDirect3DDevice9* d, UINT len, DWORD u, DWORD fvf, D3DPOOL p, IDirect3DVertexBuffer9** o, HANDLE* s) {
  HRESULT hr = o_vb(d, len, u, fvf, p, o, s);
  if (SUCCEEDED(hr) && p == D3DPOOL_DEFAULT && o) Track(*o, 3, len, 1, fvf, u, RET_ADDR);
  return hr;
}
HRESULT WINAPI HookIb(IDirect3DDevice9* d, UINT len, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DIndexBuffer9** o, HANDLE* s) {
  HRESULT hr = o_ib(d, len, u, f, p, o, s);
  if (SUCCEEDED(hr) && p == D3DPOOL_DEFAULT && o) Track(*o, 4, len, 1, f, u, RET_ADDR);
  return hr;
}
HRESULT WINAPI HookRt(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL lk,
                      IDirect3DSurface9** o, HANDLE* s) {
  HRESULT hr = o_rt(d, w, h, f, ms, q, lk, o, s);
  if (SUCCEEDED(hr) && o) Track(*o, 5, w, h, f, D3DUSAGE_RENDERTARGET, RET_ADDR);
  return hr;
}
HRESULT WINAPI HookDs(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL dc,
                      IDirect3DSurface9** o, HANDLE* s) {
  HRESULT hr = o_ds(d, w, h, f, ms, q, dc, o, s);
  if (SUCCEEDED(hr) && o) Track(*o, 6, w, h, f, D3DUSAGE_DEPTHSTENCIL, RET_ADDR);
  return hr;
}
HRESULT WINAPI HookOff(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f, D3DPOOL p, IDirect3DSurface9** o, HANDLE* s) {
  HRESULT hr = o_off(d, w, h, f, p, o, s);
  if (SUCCEEDED(hr) && p == D3DPOOL_DEFAULT && o) Track(*o, 7, w, h, f, 0, RET_ADDR);
  return hr;
}

// ------------------------------------------------------------- device hooks

using ResetFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using CreateDeviceFn = HRESULT(WINAPI*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
ResetFn o_reset = nullptr;
CreateDeviceFn o_create = nullptr;
void** g_dev_vt = nullptr;

HRESULT WINAPI HookReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
  if (!pp) return o_reset(dev, pp);
  D3DPRESENT_PARAMETERS mine = *pp;
  if (g_mode != WM_FULLSCREEN) MakeWindowed(mine);
  Vsync(mine);
  if (g_bb_format != D3DFMT_UNKNOWN) mine.BackBufferFormat = g_bb_format;
  const int before = g_track ? AliveCount() : 0;
  ss_hdr_before_reset(dev);  // the HDR passes' default-pool objects (hdr.cpp)
  ss_glyphs_before_reset();  // glyphs.cpp lets go of the default-pool prompt textures it holds
  const bool other_format = g_bb_format != D3DFMT_UNKNOWN && pp->BackBufferFormat != g_bb_format;
  HRESULT hr = g_test_refuse == 2 && other_format ? D3DERR_INVALIDCALL : o_reset(dev, &mine);
  if (FAILED(hr) && other_format) {
    static int said = 0;
    if (said++ < 5)
      ss_log("[display] Reset with back buffer format %d failed (0x%08lx, %ux%u, %s); again with the game's format %d", static_cast<int>(g_bb_format),
             static_cast<unsigned long>(hr), mine.BackBufferWidth, mine.BackBufferHeight, mine.Windowed ? "windowed" : "fullscreen",
             static_cast<int>(pp->BackBufferFormat));
    mine = *pp;
    if (g_mode != WM_FULLSCREEN) MakeWindowed(mine);
  Vsync(mine);
    hr = o_reset(dev, &mine);
  }
  CopyBack(pp, mine);
  if (SUCCEEDED(hr)) {
    ss_frame_interval(mine.PresentationInterval);
    ss_hdr_after_reset(dev);
  }
  static int n = 0;
  ++n;
  if (FAILED(hr)) {
    static int failed = 0;
    if (failed++ < 5) {
      ss_log("[display] Reset #%d (%ux%u, %s) failed: 0x%08lx; %d DEFAULT-pool objects alive", n, mine.BackBufferWidth,
             mine.BackBufferHeight, mine.Windowed ? "windowed" : "fullscreen", static_cast<unsigned long>(hr), before);
      if (g_track) LogAlive(40);
    }
  } else {
    if (n <= 20)
      ss_log("[display] Reset #%d (%ux%u, %s) ok", n, mine.BackBufferWidth, mine.BackBufferHeight,
             mine.Windowed ? "windowed" : "fullscreen");
    if (g_mode != WM_FULLSCREEN) LayOutWindow(mine.hDeviceWindow ? mine.hDeviceWindow : g_hwnd, mine.BackBufferWidth, mine.BackBufferHeight);
  }
  return hr;
}

// Puts our slots into the device's vtable (d3d9's memory). Called on the game's thread inside CreateDevice, before
// the game (or patch.cpp's Present hook, which polls renderer+0x578) can see the device.
void HookDevice(IDirect3DDevice9* dev) {
  void** vt = *reinterpret_cast<void***>(dev);
  if (!Readable(vt, (kDevCreateOffscreenPlain + 1) * sizeof(void*))) return;
  if (vt == g_dev_vt) return;  // the same class: already ours
  if (g_dev_vt) ss_log("[display] a device with another vtable (%p, first %p) was created; hooking it too", vt, g_dev_vt);
  struct { int slot; void* fn; void** orig; } slots[] = {
      {kDevReset, reinterpret_cast<void*>(&HookReset), reinterpret_cast<void**>(&o_reset)},
      {kDevCreateTexture, reinterpret_cast<void*>(&HookTex), reinterpret_cast<void**>(&o_tex)},
      {kDevCreateVolumeTexture, reinterpret_cast<void*>(&HookVol), reinterpret_cast<void**>(&o_vol)},
      {kDevCreateCubeTexture, reinterpret_cast<void*>(&HookCube), reinterpret_cast<void**>(&o_cube)},
      {kDevCreateVertexBuffer, reinterpret_cast<void*>(&HookVb), reinterpret_cast<void**>(&o_vb)},
      {kDevCreateIndexBuffer, reinterpret_cast<void*>(&HookIb), reinterpret_cast<void**>(&o_ib)},
      {kDevCreateRenderTarget, reinterpret_cast<void*>(&HookRt), reinterpret_cast<void**>(&o_rt)},
      {kDevCreateDepthStencil, reinterpret_cast<void*>(&HookDs), reinterpret_cast<void**>(&o_ds)},
      {kDevCreateOffscreenPlain, reinterpret_cast<void*>(&HookOff), reinterpret_cast<void**>(&o_off)},
  };
  const int count = g_track ? 9 : 1;  // only Reset unless the tracker is wanted
  for (int i = 0; i < count; ++i) {
    if (vt[slots[i].slot] == slots[i].fn) continue;
    void* prev = vt[slots[i].slot];
    if (g_dev_vt && *slots[i].orig && *slots[i].orig != prev) {
      ss_log("[display] slot %d of the second device vtable differs from the first; left alone", slots[i].slot);
      continue;
    }
    *slots[i].orig = prev;  // set before the slot points at us
    if (!SwapSlot(vt, slots[i].slot, slots[i].fn)) {
      ss_log("[display] cannot write device vtable slot %d (error %lu)", slots[i].slot, GetLastError());
      continue;
    }
  }
  g_dev_vt = vt;
  char where[160];
  Where(reinterpret_cast<void*>(o_reset), where, sizeof where);
  ss_log("[display] device %p hooked (vtable %p, Reset in %s)%s", dev, vt, where,
         g_track ? "; DEFAULT-pool objects are tracked" : "");
}

// Windows only: the 10-bit back buffer is for DXVK's HDR10 swap chain. Microsoft's d3d9 has no HDR output at all,
// so there the game keeps its own format. Under Wine nothing changes (DXVK or not, as before). Microsoft's d3d9 is
// the one loaded from the Windows folder; DXVK's lies in the dxvk folder (renderer.cpp) or beside the exe.
bool UnderWine() {
  HMODULE nt = GetModuleHandleA("ntdll.dll");
  return nt && GetProcAddress(nt, "wine_get_version");
}

// The file of the module that holds the IDirect3D9's vtable (the d3d9 that is really used); false when unknown.
bool D3d9Path(IDirect3D9* d3d, char* path, DWORD n) {
  HMODULE m = nullptr;
  void** vt = *reinterpret_cast<void***>(d3d);
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(vt), &m) || !m)
    return false;
  const DWORD pn = GetModuleFileNameA(m, path, n);
  return pn && pn < n;
}

bool InWindowsDir(const char* path) {
  char win[MAX_PATH];
  const UINT wn = GetSystemWindowsDirectoryA(win, sizeof win);
  if (!wn || wn >= sizeof win) return false;
  return strlen(path) > wn && !_strnicmp(path, win, wn) && (path[wn] == '\\' || path[wn] == '/');
}

bool TenBitUseless(IDirect3D9* d3d) {
  if (UnderWine()) return false;
  char path[MAX_PATH];
  return D3d9Path(d3d, path, sizeof path) && InWindowsDir(path);
}

// One [display] adapter line per run, at the game's CreateDevice: the GPU (vendor by PCI vendor ID, name, device ID),
// its driver as the d3d9 reports it, which d3d9 draws, and the vendor driver modules loaded by then. Nothing here
// changes behaviour (no feature depends on the vendor); it is so a tester's log says what it ran on. Under DXVK the
// name and IDs are the Vulkan device's (a dxvk.conf customVendorId / customDeviceId shows here too), the driver
// version is the one DXVK reports; the module list is the cross-check.
const char* VendorName(DWORD id) {
  switch (id) {
    case 0x10DE: return "NVIDIA";
    case 0x1002: case 0x1022: return "AMD";
    case 0x8086: return "Intel";
    case 0x5143: return "Qualcomm";
    case 0x15AD: return "VMware";
    case 0x1414: return "Microsoft (software)";
    default: return "other";
  }
}

void LogAdapter(IDirect3D9* d3d, UINT adapter) {
  static bool done = false;
  if (done) return;
  done = true;
  char path[MAX_PATH] = "";
  const bool have_path = D3d9Path(d3d, path, sizeof path);
  const char* which = UnderWine()                  ? "Wine / Proton's d3d9"
                      : ss_renderer_dxvk()         ? "DXVK"
                      : have_path && InWindowsDir(path) ? "Windows' own d3d9"
                                                   : "a d3d9 outside the Windows folder";
  // Vendor driver modules (32-bit names): D3D9 user-mode drivers, Vulkan ICDs, Microsoft's D3D9-on-12 layer.
  static const char* const kDrivers[] = {"nvd3dum.dll",  "nvoglv32.dll", "nvwgf2um.dll", "aticfx32.dll", "amdvlk32.dll",
                                         "amdxc32.dll",  "igdumdim32.dll", "igvk32.dll", "d3d9on12.dll"};
  std::string mods;
  for (const char* d : kDrivers)
    if (GetModuleHandleA(d)) mods += mods.empty() ? d : std::string(", ") + d;
  D3DADAPTER_IDENTIFIER9 id;
  memset(&id, 0, sizeof id);
  const HRESULT hr = d3d->GetAdapterIdentifier(adapter, 0, &id);
  if (FAILED(hr)) {
    ss_log("[display] adapter %u: GetAdapterIdentifier failed (0x%08lx); d3d9: %s (%s); driver modules: %s", adapter,
           static_cast<unsigned long>(hr), which, have_path ? path : "?", mods.empty() ? "none known" : mods.c_str());
    return;
  }
  id.Description[sizeof id.Description - 1] = 0;
  id.Driver[sizeof id.Driver - 1] = 0;
  const DWORD hi = static_cast<DWORD>(id.DriverVersion.HighPart), lo = id.DriverVersion.LowPart;
  ss_log("[display] adapter %u: %s (vendor 0x%04lX, device 0x%04lX), \"%s\", driver %s %lu.%lu.%lu.%lu; d3d9: %s (%s); "
         "driver modules: %s",
         adapter, VendorName(id.VendorId), static_cast<unsigned long>(id.VendorId), static_cast<unsigned long>(id.DeviceId),
         id.Description, id.Driver[0] ? id.Driver : "?", static_cast<unsigned long>(HIWORD(hi)),
         static_cast<unsigned long>(LOWORD(hi)), static_cast<unsigned long>(HIWORD(lo)), static_cast<unsigned long>(LOWORD(lo)),
         which, have_path ? path : "?", mods.empty() ? "none known" : mods.c_str());
  if (id.VendorId == 0x10DE && !UnderWine() && ss_renderer_dxvk())
    ss_log("[display] NVIDIA with DXVK on Windows has never been measured: if a race load crashes or the picture "
           "sticks, vm_log = 30 shows the 2 GB address space; renderer = native is the fallback (README, NVIDIA)");
}

HRESULT WINAPI HookCreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
  if (!pp || !out) return o_create(d3d, adapter, type, focus, flags, pp, out);
  if (g_bb_format != D3DFMT_UNKNOWN && TenBitUseless(d3d)) {
    ss_log("[display] this d3d9 is Windows' own: no HDR output through it, the back buffer keeps the game's format "
           "(HDR on Windows needs renderer = dxvk)");
    g_bb_format = D3DFMT_UNKNOWN;
  }
  D3DPRESENT_PARAMETERS mine = *pp;
  if (g_mode != WM_FULLSCREEN) MakeWindowed(mine);
  Vsync(mine);
  if (g_bb_format != D3DFMT_UNKNOWN) {
    ss_hdr_before_create(d3d);
    mine.BackBufferFormat = g_bb_format;
  }
  IDirect3DDevice9* dev = nullptr;
  const bool other_format = g_bb_format != D3DFMT_UNKNOWN && pp->BackBufferFormat != g_bb_format;
  HRESULT hr = g_test_refuse == 1 && other_format ? D3DERR_INVALIDCALL : o_create(d3d, adapter, type, focus, flags, &mine, &dev);
  if (FAILED(hr) && other_format) {
    ss_log("[display] CreateDevice with back buffer format %d failed (0x%08lx, %s); again with the game's format %d", static_cast<int>(g_bb_format),
           static_cast<unsigned long>(hr), mine.Windowed ? "windowed" : "fullscreen", static_cast<int>(pp->BackBufferFormat));
    mine = *pp;
    if (g_mode != WM_FULLSCREEN) MakeWindowed(mine);
  Vsync(mine);
    dev = nullptr;
    hr = o_create(d3d, adapter, type, focus, flags, &mine, &dev);
  }
  CopyBack(pp, mine);
  LogAdapter(d3d, adapter);  // after the create: the vendor's D3D9 driver module is loaded by then
  const HWND win = mine.hDeviceWindow ? mine.hDeviceWindow : focus;
  ss_log("[display] CreateDevice adapter %u, %ux%u format %lu, %s (game asked %s), interval 0x%lx, window %p: 0x%08lx",
         adapter, mine.BackBufferWidth, mine.BackBufferHeight, static_cast<unsigned long>(mine.BackBufferFormat),
         mine.Windowed ? "windowed" : "fullscreen", pp->Windowed ? "windowed" : "fullscreen",
         static_cast<unsigned long>(mine.PresentationInterval), win, static_cast<unsigned long>(hr));
  if (SUCCEEDED(hr) && dev) {
    ss_frame_interval(mine.PresentationInterval);
    HookDevice(dev);
    ss_pipewarm_device(dev);  // pipewarm.cpp: directly above DXVK's draw slots, before every other device hook
    ss_hdr_device_created(dev, static_cast<unsigned>(mine.BackBufferFormat));  // hdr.cpp's two slots, before the game has the device
    ss_glyphs_device(dev);  // glyphs.cpp: the texture hooks, before the game loads its prompt textures
    g_hwnd = win;
    if (g_mode != WM_FULLSCREEN) LayOutWindow(win, mine.BackBufferWidth, mine.BackBufferHeight);
    Subclass(focus ? focus : win);
  }
  *out = dev;  // only now: the game stores it straight into renderer+0x578
  return hr;
}

// --------------------------------------------------------------- start-up

constexpr uintptr_t kWindowObj = 0xD646B4;  // the app window object; HWND at +0x140 (0xA13BEA)

// One dword of the game's writable data behind a pointer (0 when unreadable). Read only.
uintptr_t Field(uintptr_t global, uintptr_t off) {
  if (reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)) != 0x400000) return 0;
  if (!Readable(reinterpret_cast<void*>(global), 4)) return 0;
  const uintptr_t o = *reinterpret_cast<volatile uintptr_t*>(global);
  if (!o || !Readable(reinterpret_cast<void*>(o + off), 4)) return 0;
  return *reinterpret_cast<volatile uintptr_t*>(o + off);
}

// Too late for the device (it already exists): only the window procedure part can still be applied.
void Late() {
  ss_log("[display] the game's device already exists; window_mode, display_log%s apply from the next start",
         g_bb_format != D3DFMT_UNKNOWN ? " and the HDR back buffer" : "");
  Subclass(reinterpret_cast<HWND>(Field(kWindowObj, 0x140)));
}

// Windows' application compatibility layer (apphelp.dll, the shim engine's COM hooks; in every process) hooks
// Direct3DCreate9 and writes its own CreateDevice into slot 16 of the IDirect3D9 vtable after every call. It
// looks its original up in its own records, where the first record of the slot wins, and that is the runtime's
// function. So a hook put into the slot before the game's Direct3DCreate9 is overwritten by that call and never
// runs (seen on Windows 11 with Microsoft's d3d9 and with DXVK: the hook was logged, the wrap was never called).
// The game calls GetAdapterDisplayMode (slot 8) straight after its Direct3DCreate9 (0x50D373..0x50D38B), on its
// own thread and long before CreateDevice, and the layer leaves that slot alone: the CreateDevice slot is checked
// there and taken again in front of whatever is in it.
constexpr int kD3DGetAdapterDisplayMode = 8;
using GetModeFn = HRESULT(WINAPI*)(IDirect3D9*, UINT, D3DDISPLAYMODE*);
GetModeFn o_get_mode = nullptr;

bool TakeCreateSlot(void** vt, bool again) {
  void* prev = vt[kD3DCreateDevice];
  if (prev == reinterpret_cast<void*>(&HookCreateDevice)) return true;
  o_create = reinterpret_cast<CreateDeviceFn>(prev);  // set before the slot points at us
  if (!SwapSlot(vt, kD3DCreateDevice, reinterpret_cast<void*>(&HookCreateDevice))) return false;
  if (again) {
    char where[160];
    Where(prev, where, sizeof where);
    ss_log("[display] the CreateDevice slot was taken back (now %s); hooked again in front of it", where);
  }
  return true;
}

// extra_resolution = <w>x<h> (window_mode borderless or windowed only): one more display mode in the list the game
// reads (IDirect3D9 GetAdapterModeCount slot 6 and EnumAdapterModes slot 7, appended after the driver's modes for every
// format), so the game's RESOLUTION option offers it and a size in config_ss.cfg that no monitor mode has (2560x1080
// on a 2560x1440 monitor) is kept instead of being replaced by a mode of the list. In a window any back buffer size
// works; this is how a 21:9 or 32:9 picture is tried on a 16:9 monitor.
constexpr int kD3DGetAdapterModeCount = 6, kD3DEnumAdapterModes = 7;
using ModeCountFn = UINT(WINAPI*)(IDirect3D9*, UINT, D3DFORMAT);
using EnumModesFn = HRESULT(WINAPI*)(IDirect3D9*, UINT, D3DFORMAT, UINT, D3DDISPLAYMODE*);
ModeCountFn o_mode_count = nullptr;
EnumModesFn o_enum_modes = nullptr;
UINT g_extra_w = 0, g_extra_h = 0;
volatile LONG g_extra_logged = 0;

UINT WINAPI HookGetAdapterModeCount(IDirect3D9* d3d, UINT adapter, D3DFORMAT format) {
  const UINT n = o_mode_count(d3d, adapter, format);
  return n > 0 ? n + 1 : n;  // only a format the adapter shows at all
}

HRESULT WINAPI HookEnumAdapterModes(IDirect3D9* d3d, UINT adapter, D3DFORMAT format, UINT index, D3DDISPLAYMODE* mode) {
  const UINT n = o_mode_count(d3d, adapter, format);
  if (index != n || n == 0 || !mode) return o_enum_modes(d3d, adapter, format, index, mode);
  HRESULT hr = o_enum_modes(d3d, adapter, format, n - 1, mode);  // the last real mode: its refresh rate and format
  if (FAILED(hr)) return hr;
  mode->Width = g_extra_w;
  mode->Height = g_extra_h;
  if (InterlockedExchange(&g_extra_logged, 1) == 0)
    ss_log("[display] extra_resolution %ux%u offered as mode %u of format %d (%u Hz)", g_extra_w, g_extra_h, n, int(format), mode->RefreshRate);
  return hr;
}

HRESULT WINAPI HookGetAdapterDisplayMode(IDirect3D9* d3d, UINT adapter, D3DDISPLAYMODE* mode) {
  void** vt = *reinterpret_cast<void***>(d3d);
  if (!Field(kRenderer, 0x578) && Readable(vt, (kD3DCreateDevice + 1) * sizeof(void*))) TakeCreateSlot(vt, true);
  return o_get_mode(d3d, adapter, mode);
}

DWORD WINAPI DisplayThread(LPVOID) {
  // The device must not exist yet: the game keeps it at renderer+0x578 (renderer = *0xD64708).
  if (Field(kRenderer, 0x578)) {
    Late();
    return 1;
  }
  ss_dxvkwin_hold();  // dxvk_fit: the kept object first, so the Release below is not the last (the driver stays loaded)
  HMODULE d3d9 = LoadLibraryA("d3d9.dll");  // the module the game imports (already loaded or loaded the same way)
  using CreateFn = IDirect3D9*(WINAPI*)(UINT);
  CreateFn create = d3d9 ? reinterpret_cast<CreateFn>(GetProcAddress(d3d9, "Direct3DCreate9")) : nullptr;
  // The object is kept for the life of the process, never released. Under Proton every IDirect3D9 of DXVK is a
  // Vulkan instance, and releasing this one ~30 ms after its creation (while the game is still unpacking, seconds
  // before its own Direct3DCreate9) ran vkDestroyInstance in the 32-bit radv: on Bazzite that sometimes ended the
  // process at once, with no exception (the start-ups that died after "CreateDevice hooked", mostly the first
  // start after a quit). A kept object costs a few MB and leaves the driver loaded for the game's own object.
  IDirect3D9* probe = create ? create(D3D_SDK_VERSION) : nullptr;
  if (!probe) {
    ss_log("[display] cannot create an IDirect3D9 to find its vtable; window_mode and display_log left off");
    return 1;
  }
  void** vt = *reinterpret_cast<void***>(probe);
  if (Field(kRenderer, 0x578)) {
    Late();
    return 1;
  }
  if (!TakeCreateSlot(vt, false)) {
    ss_log("[display] cannot write the IDirect3D9 vtable (error %lu); window_mode and display_log left off", GetLastError());
    return 1;
  }
  if (vt[kD3DGetAdapterDisplayMode] != reinterpret_cast<void*>(&HookGetAdapterDisplayMode)) {
    o_get_mode = reinterpret_cast<GetModeFn>(vt[kD3DGetAdapterDisplayMode]);  // set before the slot points at us
    if (!SwapSlot(vt, kD3DGetAdapterDisplayMode, reinterpret_cast<void*>(&HookGetAdapterDisplayMode)))
      ss_log("[display] cannot write the GetAdapterDisplayMode slot (error %lu): the CreateDevice hook is not re-checked", GetLastError());
  }
  if (g_extra_w && vt[kD3DEnumAdapterModes] != reinterpret_cast<void*>(&HookEnumAdapterModes)) {
    o_mode_count = reinterpret_cast<ModeCountFn>(vt[kD3DGetAdapterModeCount]);
    o_enum_modes = reinterpret_cast<EnumModesFn>(vt[kD3DEnumAdapterModes]);
    if (!SwapSlot(vt, kD3DEnumAdapterModes, reinterpret_cast<void*>(&HookEnumAdapterModes)) ||
        !SwapSlot(vt, kD3DGetAdapterModeCount, reinterpret_cast<void*>(&HookGetAdapterModeCount))) {
      SwapSlot(vt, kD3DEnumAdapterModes, reinterpret_cast<void*>(o_enum_modes));
      ss_log("[display] cannot write the mode list slots (error %lu): extra_resolution left off", GetLastError());
    }
  }
  char where[160];
  Where(reinterpret_cast<void*>(o_create), where, sizeof where);
  ss_log("[display] IDirect3D9::CreateDevice hooked (vtable %p, original in %s; the probe object kept)", vt, where);
  // The game's CreateDevice may have run between the check above and the swap.
  if (Field(kRenderer, 0x578) && !g_dev_vt) Late();  // the vtable is static in the d3d9 module and outlives the object
  return 0;
}

}  // namespace

void ss_display_start() {
  std::string v = Setting("SS_WINDOW_MODE", "window_mode");
  if (v == "borderless") g_mode = WM_BORDERLESS;
  else if (v == "windowed") g_mode = WM_WINDOWED;
  else if (!v.empty() && v != "fullscreen") ss_log("[display] window_mode = %s is not known; fullscreen (the game's own)", v.c_str());
  v = Setting("SS_PAUSE_ON_FOCUS_LOSS", "pause_on_focus_loss");
  if (IsOff(v)) g_pause_on_focus_loss = false;
  v = Setting("SS_DISPLAY_LOG", "display_log");
  g_track = IsOn(v);
  v = Setting("SS_GAME_VSYNC", "game_vsync");
  if (IsOff(v)) g_game_vsync = GV_OFF;
  else if (IsOn(v)) g_game_vsync = GV_ON;
  else if (!v.empty() && v != "game") ss_log("[display] game_vsync = %s is not known; game (the game's own option)", v.c_str());
  v = Setting("SS_EXTRA_RESOLUTION", "extra_resolution");
  if (!v.empty()) {
    unsigned w = 0, h = 0;
    if (sscanf(v.c_str(), "%ux%u", &w, &h) == 2 && w >= 640 && h >= 360 && w <= 16384 && h <= 8192) {
      if (g_mode != WM_FULLSCREEN) g_extra_w = w, g_extra_h = h;
      else ss_log("[display] extra_resolution = %s needs window_mode borderless or windowed; left off", v.c_str());
    } else {
      ss_log("[display] extra_resolution = %s is not <width>x<height>; left off", v.c_str());
    }
  }
  g_bb_format = static_cast<D3DFORMAT>(ss_hdr_backbuffer_format());
  if (const char* e = getenv("SS_HDR_TEST_REFUSE")) g_test_refuse = !strcmp(e, "create") ? 1 : !strcmp(e, "reset") ? 2 : 0;
  const bool vsync = ss_frame_vsync();
  const bool fastest = ss_frame_immediate();
  if (g_mode == WM_FULLSCREEN && g_pause_on_focus_loss && !g_track && g_bb_format == D3DFMT_UNKNOWN && !vsync && !fastest &&
      g_game_vsync == GV_GAME && !ss_glyphs_wanted() && !ss_pipewarm_wanted())
    return;  // nothing at all (glyphs.cpp wants the device as it is made: the prompt textures load right after)
  InitializeCriticalSection(&g_lock);
  if (g_extra_w) ss_log("[display] extra_resolution %ux%u (a mode added to the game's list)", g_extra_w, g_extra_h);
  ss_log("[display] window_mode %s, pause_on_focus_loss %s, display_log %s, game_vsync %s%s", kModeName[g_mode],
         g_pause_on_focus_loss ? "on" : "off", g_track ? "on" : "off", kGameVsyncName[g_game_vsync],
         vsync ? "; vsync: the device presents on the vblank (frame_pacing)" : fastest ? "; the device presents at once (frame_pacing = fastest)" : "");
  CreateThread(nullptr, 0, DisplayThread, nullptr, 0, nullptr);
}

// The settings menu (menu.cpp): pause_on_focus_loss at once while the window procedure is subclassed (it is only
// when the key was false at start); otherwise false (RESTART NEEDED). window_mode is restart-only (the device is made
// with it).
bool ss_display_apply(const char* key, const char* value) {
  if (strcmp(key, "pause_on_focus_loss") || !g_prev_proc) return false;
  const std::string v = value ? value : "";
  g_pause_on_focus_loss = !IsOff(v);
  ss_log("[display] pause_on_focus_loss %s (from the settings menu)", g_pause_on_focus_loss ? "on" : "off");
  return true;
}
