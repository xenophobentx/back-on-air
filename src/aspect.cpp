// Picture shape: 16:10 and ultrawide (aspect, hud_width) and the field of view (fov).
//
// What the PC build does (reverse engineering of the Steam build 385319, run-time VAs):
//
// * The scene. The viewport manager (*0xD66000, no vtable: +0 views, +8 a single-view override, two cameras of 0x2F4
//   bytes at +0x68) holds the cameras the frame is drawn with. A camera's projection comes from a view window, two
//   floats at +0 (width) and +4 (height), 2 tan(fov/2) for a perspective camera (0x50B180 makes it from fov and
//   aspect; 0x50AAF0 puts 2/width and 2/height into the projection). Flags: +0x1E0 orthographic, +0x1E1 custom
//   projection, +0x1E2 dirty. Every update the in-game controller (0x4B4F90) calls 0x4B8DD0, which for every view
//   asks 0x794370(view, force) twice: force 0 gives the back buffer's shape (renderer *0xD64708, +0x30 / +0x34 the
//   back buffer's size), force 1 gives the constant 16/9 at 0xBAA9DC (both scaled alike for split screen). It makes
//   width = 2 tan(fov/2) * 16/9 and height = width / (back buffer's shape), builds the matrices (0x50C0D0) and copies
//   the camera into the manager (0x794B60 = copy 0x483C20 + publish the matrices 0x50C250). So the horizontal field
//   of view is the one of a 16:9 screen whatever the screen: on 16:10 there is more above and below (what the port
//   does there too), on 21:9 and 32:9 the picture is cut at the top and the bottom (Vert-).
// * The interface. Every Scaleform movie is made by 0x536920: CreateInstance, SetBackgroundAlpha and
//   SetViewScaleMode(2 = ExactFit); the game then gives it the whole screen as its viewport. ExactFit stretches the
//   16:9 stage over any screen: on 16:10 and on ultrawide the HUD, the menus and the letterbox bars are distorted.
//
// What this file does (nothing in the game's code is written):
//
// * Scene (Hor+): after the game's update, every perspective camera of the manager that the game made is widened:
//   wider than 16:9, the vertical field of view of the 16:9 picture is kept and the horizontal one grows; narrower,
//   the game's own picture is kept (the horizontal field of 16:9, more above and below). fov scales the vertical
//   angle on top. The camera's window is written (a heap object), its dirty flag set, and the game's own matrix
//   build and publish (0x50C0D0, 0x50C250) are called, as 0x4B8DD0 does. It runs from the frame step (patch.cpp,
//   right after the update) and, as a fallback that also covers fps = 30, from the device's BeginScene (slot 41 of
//   the D3D9 device vtable, in d3d9.dll's memory; not from the settings overlay's own BeginScene). A camera we wrote
//   is recognised by its two numbers and left; a camera that already has the numbers we would give it is not
//   rebuilt. On 16:9, and on 16:10 at fov 100 (the port keeps the game's picture there too), nothing is touched.
// * Interface (16:9 box): GFxMovieRoot::SetViewport (slot +0x50 of the vtable 0xC41CF4, read-only data, one aligned
//   word: 0xC41D44, holds 0x98CDF0) is pointed at a wrapper that makes the viewport the middle 16:9 part of what the
//   game asked for (pillarbox on a wider screen, letterbox on a narrower one) with the scissor on that box. The
//   movies that are full-screen bars or wipes (the route-change letterbox, the transitions) keep the game's
//   stretched viewport, so the bars still span the screen. Display (+0x7C: 0xC41D70, holds 0x987E60) is wrapped too,
//   only to give the box to a movie whose viewport was set before the wrapper was in place. Only a viewport whose
//   buffer is the back buffer (same size) is boxed; a movie drawn into a texture of its own is left as it is.
//   hud_width defaults to 16:9 as the port's row, and on a wider screen the default pillarboxes. On a screen narrower
//   than 16:9 (16:10, the Steam Deck's 1280x800) the game's own full-screen interface stays, whatever hud_width says,
//   unless hud_width = letterbox (the 16:9 box with bars above and below there; on a wider screen the same as 16:9).
// * Culling (the scene widened inside the game's own camera setup). The update copies each view's six frustum planes
//   from the manager's camera into the frame's visibility data (0x4B1C20, called at 0x4B2464, 0x4B427C and 0x4B54A5,
//   always after the camera setup 0x4B8DD0) and hands them to the visibility job. A camera widened only after the
//   update would be drawn wide but culled with the narrow planes: objects only in the added side strips would be
//   missing. So the constant 16/9 that 0x794370 returns for force 1 (0xBAA9DC, .rdata) is set to the screen's shape
//   (times the fov factor, see FixCameras) while the scene is widened: 0x4B8DD0 then makes width = 2 tan(fov/2) * A
//   and height = width / A itself, every frustum, plane copy and streaming view of the update is Hor+ from the start,
//   and the later rebuild has nothing left to change (fov 100). The same constant is read by the other camera builder
//   0x4513E0 (same formula), the camera manager's default camera (0x794950), the chase cameras' collision box
//   (0xAAAFD0), a horizontal-angle getter (0xA59950), the camera zone manager (0xA9F0F0) and the streaming look-ahead
//   view (0x4B45EA, 0x4B4C29): all ask "the screen's shape" for a 16:9 screen and get the real one.
// * When: nothing at all is hooked until the back buffer's shape needs it. A 16:9 back buffer at fov 100, or a 16:10
//   one at fov 100 without hud_width = letterbox, gets no hook; the worker looks again every 250 ms (resolution change).
//
// * Menus over the stopped race (pause, results, the track intro): boxed, their own bars and backdrops would end at the
//   box and the race would show on beside them. After such a movie is drawn (Display wrapper) the side strips are
//   cleared to black (IDirect3DDevice9::Clear with the viewport and scissor opened to the back buffer), so they are
//   pillarboxed like the loading screens, which have no picture behind them.
// * Only on a screen wider than 16:9 (findings from Glimpse-Fix PR #9, see docs/technical.md): the player name tags
//   keep their 16:9 shape and the engine's letterbox bars are off (EngineValues, four .data tweakables), and in split
//   screen the race HUD movies are drawn once per player half (DisplayHalves). Pure part: aspect_core.h.
//
// Markers: the Power Play icons (0x4C2B20) and the name tags (0x4C6F90) are projected during the update with the
// manager camera's matrix (cam+0x198, 0x45A150) and drawn by the engine (the movies render into textures of their own,
// "HUD_pass" / SpriteShader), not through a boxed viewport; with the camera widened inside the setup they follow the
// widened picture (before, they were placed from the 16:9 horizontal camera). Not seen in a test (no Power Play on the
// harness route). Screen-space effects keep their pixel sizes.

#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include "aspect_core.h"
#include "ss_shared.h"

#define ENTRY_THIS __attribute__((thiscall, force_align_arg_pointer))
#define ENTRY_STD __attribute__((stdcall, force_align_arg_pointer))

#define PILLARBOX_NAMES "PauseMenu", "IG_GameModeInstructions", "EventObjectives", "RaceEnd", "RaceResults", "RaceStats", "Rewards"

namespace {

constexpr double kConsole = 16.0 / 9.0;
constexpr double kPi = 3.14159265358979323846;

// ------------------------------------------------------------------------------------------------ addresses
constexpr uintptr_t kRenderer = 0xD64708;       // renderer: +0x30 / +0x34 back buffer size, +0x578 the D3D9 device
constexpr uintptr_t kRendererBufW = 0x30;
constexpr uintptr_t kRendererBufH = 0x34;
constexpr uintptr_t kRendererDevice = 0x578;
constexpr uintptr_t kViewMgr = 0xD66000;        // viewport manager pointer
constexpr uintptr_t kViewCams = 0x68;           // two cameras of kCamSize bytes
constexpr uintptr_t kCamSize = 0x2F4;
constexpr uintptr_t kCamOrtho = 0x1E0, kCamCustom = 0x1E1, kCamDirty = 0x1E2;
constexpr uintptr_t kCamBuild = 0x50C0D0;       // thiscall: matrices from the window when dirty
constexpr uintptr_t kCamPublish = 0x50C250;     // thiscall: the matrices to their bound shader parameters
constexpr int kBeginSceneSlot = 41;
constexpr uintptr_t kShapeConst = 0xBAA9DC;     // .rdata float 16/9: what 0x794370 returns for force 1
constexpr uint32_t kShapeConstOrig = 0x3FE38E39;

constexpr uintptr_t kMovieVt = 0xC41CF4;        // GFxMovieRoot
constexpr uintptr_t kSlotSetViewport = kMovieVt + 0x50;  // 0xC41D44
constexpr uint32_t kOrigSetViewport = 0x98CDF0;
constexpr uintptr_t kSlotDisplay = kMovieVt + 0x7C;      // 0xC41D70
constexpr uint32_t kOrigDisplay = 0x987E60;
constexpr uintptr_t kMovieViewport = 0x38;     // the movie's stored GViewport (GetViewport copies 13 words from here)
constexpr uintptr_t kMovieDef = 0x28;          // GetMovieDef
constexpr uint32_t kMovieDefVt = 0xC3D5CC;     // GFxMovieDefImpl

// GFx 3 viewport (13 words).
struct GViewport {
  int32_t bw, bh, l, t, w, h, sl, st, sw, sh;
  float scale, aspect;
  uint32_t flags;
};
static_assert(sizeof(GViewport) == 52, "GViewport is 13 words");
constexpr uint32_t kUseScissor = 4;

// ------------------------------------------------------------------------------------------------ helpers
bool Readable(uintptr_t p, size_t n) {
  if (!p) return false;
  if (ss_region_known(p, n)) return true;  // checked in this phase of the frame (ss_shared.h)
  if (ss_region_bad(p)) return false;
  MEMORY_BASIC_INFORMATION mi;
  uintptr_t end = p + n;
  while (p < end) {
    if (!SS_VQ(reinterpret_cast<const void*>(p), &mi, sizeof mi)) return false;
    if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
      ss_region_note_bad(reinterpret_cast<uintptr_t>(mi.BaseAddress), mi.RegionSize);
      return false;
    }
    if (mi.Protect != 0) ss_region_note(reinterpret_cast<uintptr_t>(mi.BaseAddress), mi.RegionSize, mi.Type);
    p = reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize;
  }
  return true;
}
inline uint32_t Rd(uintptr_t va) { return *reinterpret_cast<const volatile uint32_t*>(va); }
inline float RdF(uintptr_t va) {
  float f;
  memcpy(&f, reinterpret_cast<const void*>(va), 4);
  return f;
}
inline void WrF(uintptr_t va, float f) { memcpy(reinterpret_cast<void*>(va), &f, 4); }

bool Sig(uintptr_t va, const uint8_t* b, size_t n) { return Readable(va, n) && memcmp(reinterpret_cast<const void*>(va), b, n) == 0; }

// Swaps one aligned word in read-only memory (a vtable slot). Returns false when the page cannot be made writable.
bool SwapWord(uintptr_t at, uint32_t value) {
  return ss_write_word(reinterpret_cast<void*>(at), value);  // one lock for every slot write (testshot.cpp)
}

// "21:9", "21x9", "21/9", "2.39" -> ratio; 0 when not a screen shape.
double ParseRatio(const std::string& s) {
  double a = 0, b = 0;
  char sep = 0;
  double r = 0;
  if (sscanf(s.c_str(), "%lf%c%lf", &a, &sep, &b) == 3 && (sep == ':' || sep == 'x' || sep == '/') && a > 0 && b > 0) {
    r = a / b;
  } else if (sscanf(s.c_str(), "%lf", &a) == 1 && a > 0) {
    r = a;
  }
  return (r >= 1.0 && r <= 6.0) ? r : 0.0;
}

std::string Setting(const char* env, const char* key) {
  const char* e = getenv(env);
  std::string v = e ? e : ss_conf(key);
  for (char& c : v) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return v;
}

// ------------------------------------------------------------------------------------------------ settings
// aspect = auto | 16:9 | 16:10 | 21:9 | 32:9 | <w>:<h>   (the port's row; default auto)
//   auto: the back buffer's shape. 16:9 (also off / console): the PC build's own picture (only fov may act).
//   any other shape: used instead of the back buffer's (for a display mode whose pixels are not square).
// hud_width = 16:9 | 21:9 | full | letterbox   (the port's row; default 16:9): the shape of the interface's box. full:
//   the game's own stretch over the whole screen. (On the PC the box is stretched to that shape; the HUD's parts are
//   not moved to the edges one by one.) On a screen narrower than 16:9 only letterbox boxes the interface (16:9 with
//   bars above and below); the other values keep the game's full screen there.
// fov = <percent>   (default 100): the scene's vertical field of view in percent of the game's, 50..150.
bool g_on = false;          // anything to do at all
bool g_off = false;         // aspect = 16:9: the game's own shape (fov alone may still act)
bool g_fixed = false;       // aspect is a fixed shape
double g_fixed_ratio = kConsole;
double g_hud = kConsole;    // target shape of the interface box; 0 = the game's stretch
bool g_hud_letterbox = false;  // hud_width = letterbox: a screen narrower than 16:9 (16:10) is letterboxed too
double g_fov = 1.0;         // fov / 100
bool g_cam_on = false, g_ui_on = false;
bool g_early = true;        // aspect_culling (test switch, default true): widen inside the game's camera setup

void ReadSettings() {
  std::string a = Setting("SS_ASPECT", "aspect");
  bool off = false;
  if (a.empty() || a == "auto") {
    g_fixed = false;
  } else if (a == "off" || a == "console") {
    off = true;
  } else {
    double r = ParseRatio(a);
    if (r == 0) {
      ss_log("[aspect] aspect = %s is not a screen shape; the PC build's own picture is kept", a.c_str());
      off = true;
    } else if (std::fabs(r - kConsole) < 0.001) {
      off = true;
    } else {
      g_fixed = true;
      g_fixed_ratio = r;
    }
  }
  std::string h = Setting("SS_HUD_WIDTH", "hud_width");
  g_hud_letterbox = h == "letterbox";
  if (h == "full") {
    g_hud = 0;
  } else {
    double r = h.empty() || g_hud_letterbox ? kConsole : ParseRatio(h);
    g_hud = r >= kConsole ? r : kConsole;
  }
  std::string f = Setting("SS_FOV", "fov");
  if (!f.empty()) {
    double p = atof(f.c_str());
    if (p < 50) p = 50;
    if (p > 150) p = 150;
    g_fov = p / 100.0;
  }
  std::string e = Setting("SS_ASPECT_CULLING", "aspect_culling");
  g_early = !(e == "false" || e == "off" || e == "0");
  g_off = off;
  g_cam_on = !off || std::fabs(g_fov - 1.0) > 1e-6;
  g_ui_on = !off && g_hud > 0;
  g_on = g_cam_on || g_ui_on;
  if (off && !g_cam_on) {
    ss_log("[aspect] aspect = 16:9: the PC build's own picture (horizontal field of view of 16:9, stretched interface)");
  } else {
    char shape[32];
    if (off) snprintf(shape, sizeof shape, "16:9 (off)");
    else if (g_fixed) snprintf(shape, sizeof shape, "%.4f", g_fixed_ratio);
    else snprintf(shape, sizeof shape, "auto (the back buffer's)");
    ss_log("[aspect] aspect %s, hud_width %s, fov %.0f%%%s", shape, g_hud > 0 ? (g_hud_letterbox ? "letterbox" : std::fabs(g_hud - kConsole) < 0.001 ? "16:9" : h.c_str()) : "full",
           g_fov * 100.0, g_early ? "" : ", aspect_culling false (the scene widened after the update only)");
  }
}

// The back buffer's size, from the renderer (0 when not there yet).
bool BackBuffer(int& w, int& h) {
  uintptr_t r = Rd(kRenderer);
  if (!r || !Readable(r + kRendererBufW, 8)) return false;
  w = static_cast<int>(Rd(r + kRendererBufW));
  h = static_cast<int>(Rd(r + kRendererBufH));
  return w > 0 && h > 0 && w < 32768 && h < 32768;
}

// The screen's shape for a buffer of w x h pixels.
double ScreenRatio(int w, int h) {
  if (g_fixed) return g_fixed_ratio;
  return (w > 0 && h > 0) ? double(w) / double(h) : kConsole;
}

bool FovSet() { return std::fabs(g_fov - 1.0) > 1e-6; }

// Whether the scene's cameras change at all on a back buffer of w x h: fov, a screen wider than 16:9 (Hor+), or a
// fixed shape that is not the buffer's. A 16:9 or 16:10 screen at fov 100 keeps the game's own picture.
bool CamWanted(int w, int h) {
  if (!g_cam_on) return false;
  if (FovSet()) return true;
  if (g_off) return false;
  const double A = ScreenRatio(w, h);
  if (A > kConsole * 1.001) return true;
  return g_fixed && std::fabs(A / (double(w) / double(h)) - 1.0) > 1e-3;
}

// The interface box's shape on a screen of shape A, 0 when the game's full-screen stretch stays: hud_width, never
// wider than the screen; on a screen narrower than 16:9 (16:10) a 16:9 letterbox only with hud_width = letterbox.
double BoxShape(double A) {
  if (!g_ui_on) return 0;
  double T;
  if (A >= kConsole) T = A >= g_hud ? g_hud : A;
  else if (g_hud_letterbox) T = kConsole;
  else return 0;
  return std::fabs(A / T - 1.0) < 0.002 ? 0 : T;
}

bool UiWanted(int w, int h) { return BoxShape(ScreenRatio(w, h)) > 0; }

// ================================================================================================ the scene
bool g_cam_checked = false, g_cam_ok = false;
float g_last_w[2] = {}, g_last_h[2] = {};
uint32_t g_cam_fixes = 0;

using CamFn = void(__attribute__((thiscall)) *)(void*);

// The functions we call and the camera code whose behaviour we correct must be the known build's (read once the
// game runs, so its code is decrypted). Only reads.
bool CheckCameraCode() {
  if (g_cam_checked) return g_cam_ok;
  g_cam_checked = true;
  // 0x50C0D0: sub esp,0x40; push esi; mov esi,ecx; cmp byte [esi+0x1E2],0
  static const uint8_t build[] = {0x83, 0xEC, 0x40, 0x56, 0x8B, 0xF1, 0x80, 0xBE, 0xE2, 0x01, 0x00, 0x00, 0x00};
  // 0x50C250: push ebx; push ebp; push esi; mov esi,ecx; cmp dword [esi+0x21C],0
  static const uint8_t publish[] = {0x53, 0x55, 0x56, 0x8B, 0xF1, 0x83, 0xBE, 0x1C, 0x02, 0x00, 0x00, 0x00};
  // 0x794370: the aspect getter: back buffer (renderer +0x30 / +0x34) unless the force byte asks for 16/9
  static const uint8_t aspect[] = {0x51, 0x80, 0x7C, 0x24, 0x0C, 0x00, 0xA1, 0x08, 0x47, 0xD6, 0x00, 0xF3, 0x0F, 0x2A, 0x40, 0x30,
                                   0xF3, 0x0F, 0x2A, 0x48, 0x34, 0xF3, 0x0F, 0x5E, 0xC1, 0xF3, 0x0F, 0x11, 0x04, 0x24, 0x74, 0x0D};
  // 0x4B8EF8: call 0x794370 (force 0); fstp; mov ecx,[0xD66000]; push ebx (= 1, force); push edi; call 0x794370
  static const uint8_t setup[] = {0xE8, 0x73, 0xB4, 0x2D, 0x00, 0xD9, 0x5C, 0x24, 0x10, 0x8B, 0x0D, 0x00, 0x60, 0xD6, 0x00, 0x53, 0x57, 0xE8};
  // 0x794B60: the copy into the manager: camera = manager + 0x68 + view * 0x2F4
  static const uint8_t copy[] = {0x8B, 0x44, 0x24, 0x04, 0x69, 0xC0, 0xF4, 0x02, 0x00, 0x00, 0x56, 0x8D, 0x74, 0x08, 0x68};
  g_cam_ok = Sig(kCamBuild, build, sizeof build) && Sig(kCamPublish, publish, sizeof publish) && Sig(0x794370, aspect, sizeof aspect) &&
             Sig(0x4B8EF8, setup, sizeof setup) && Sig(0x794B60, copy, sizeof copy) && Rd(kShapeConst) == kShapeConstOrig;
  if (!g_cam_ok) ss_log("[aspect] the camera code is not the known build; the scene's field of view is left as the game makes it");
  return g_cam_ok;
}

// The value 0x794370 hands the camera setup for "16:9" (aspect_culling): what the next update builds its cameras with.
float g_shape = 0;   // what we last wrote (0 = the game's own 16/9 is there)
double g_fov_k = 1;  // the fov factor tan(fov'/2) / tan(fov/2) of the last camera (fov other than 100)
uint32_t g_shape_writes = 0;

void SetShape(float v) {
  const uint32_t want = v > 0 ? [](float f) { uint32_t u; memcpy(&u, &f, 4); return u; }(v) : kShapeConstOrig;
  if (Rd(kShapeConst) == want) {
    g_shape = v;
    return;
  }
  if (!SwapWord(kShapeConst, want)) {
    static bool said = false;
    if (!said) ss_log("[aspect] cannot write the 16/9 constant (error %lu); the scene is widened after the update only", GetLastError());
    said = true;
    g_early = false;
    return;
  }
  g_shape = v;
  if (g_shape_writes++ < 4)
    ss_log("[aspect] camera setup shape %.4f (0x%08X at %p)%s", v > 0 ? v : kConsole, want, reinterpret_cast<void*>(kShapeConst),
           v > 0 ? ": the game builds and culls the widened scene itself" : ": the game's own 16/9");
}

void FixCameras() {
  if (!g_cam_on) return;
  int bw = 0, bh = 0;
  if (!BackBuffer(bw, bh)) return;
  if (!CamWanted(bw, bh) || !CheckCameraCode()) {  // the game's picture is right
    if (g_shape > 0) SetShape(0);
    return;
  }
  const double B = double(bw) / double(bh);
  const double A = g_off ? B : ScreenRatio(bw, bh);
  // The shape the cameras of this update were built with (16/9, or ours).
  float C;
  {
    const uint32_t u = Rd(kShapeConst);
    memcpy(&C, &u, 4);
    if (!(C > 1.0f && C < 20.0f)) C = float(kConsole);
  }
  const uintptr_t mgr = Rd(kViewMgr);
  if (!mgr || !Readable(mgr, kViewCams + 2 * kCamSize)) return;
  const uint32_t views = Rd(mgr + 8) == 1 ? 1 : Rd(mgr);
  if (views < 1 || views > 2) return;
  for (uint32_t i = 0; i < views; ++i) {
    const uintptr_t cam = mgr + kViewCams + i * kCamSize;
    const uint8_t* flags = reinterpret_cast<const uint8_t*>(cam);
    if (flags[kCamOrtho] || flags[kCamCustom]) continue;
    const float W = RdF(cam), H = RdF(cam + 4);
    if (!(W > 0.0f && H > 0.0f && W < 100.0f && H < 100.0f)) continue;
    if (W == g_last_w[i] && H == g_last_h[i]) continue;  // ours, from an earlier call
    // The game's camera: width = 2 tan(fov/2) * c, height = width / b, with b the view's buffer shape and c the shape
    // 0x794370 gave for 16:9 (16/9, or C while aspect_culling is on), both scaled by the same split-screen factor s.
    // Anything else was not made by that code: left alone.
    const double s = (double(W) / double(H)) / B;
    if (std::fabs(s - 1.0) > 0.01 && std::fabs(s - 0.5) > 0.005 && std::fabs(s - 2.0) > 0.02) continue;
    const double d = kConsole * s, a = A * s;
    const double t2 = double(W) / (double(C) * s);  // 2 tan(fov/2): the 16:9 picture's vertical window
    // Wider than 16:9: the 16:9 picture's vertical field; narrower (or aspect = 16:9): the game's own.
    double h1 = g_off ? t2 * d / (B * s) : a >= d ? t2 : t2 * d / a;
    if (std::fabs(g_fov - 1.0) > 1e-6) {
      const double v0 = 2.0 * std::atan(h1 / 2.0);
      double v = v0 * g_fov;
      if (v > 170.0 * kPi / 180.0) v = 170.0 * kPi / 180.0;
      const double h2 = 2.0 * std::tan(v / 2.0);
      if (i == 0) g_fov_k = h2 / h1;
      h1 = h2;
    }
    const float nw = float(h1 * a), nh = float(h1);
    g_last_w[i] = nw;
    g_last_h[i] = nh;
    const bool same = std::fabs(nw / W - 1.0f) < 1e-5f && std::fabs(nh / H - 1.0f) < 1e-5f;
    if (!same) {
      WrF(cam, nw);
      WrF(cam + 4, nh);
      *reinterpret_cast<volatile uint8_t*>(cam + kCamDirty) = 1;
      reinterpret_cast<CamFn>(kCamBuild)(reinterpret_cast<void*>(cam));
      reinterpret_cast<CamFn>(kCamPublish)(reinterpret_cast<void*>(cam));
    }
    if (g_cam_fixes++ == 0)
      ss_log("[aspect] first camera widened: view %u, buffer %dx%d, screen %.3f, built with shape %.4f, window %.4f x %.4f -> %.4f x %.4f%s "
             "(vertical %.1f -> %.1f deg, horizontal %.1f -> %.1f deg)",
             i, bw, bh, A, C, W, H, nw, nh, same ? " (the game's own, no rebuild)" : "", 2.0 * std::atan(H / 2.0) * 180.0 / kPi,
             2.0 * std::atan(nh / 2.0) * 180.0 / kPi, 2.0 * std::atan(W / 2.0) * 180.0 / kPi, 2.0 * std::atan(nw / 2.0) * 180.0 / kPi);
  }
  // The next update's cameras: built by the game at the final width (or a bit wider: a frustum that holds the final one
  // culls nothing that is seen). The width has to cover the screen's shape, and the buffer's when a fixed aspect is
  // narrower than it; the height (width / buffer shape) then covers the final one. fov other than 100 scales both
  // by the last camera's factor, with 1 % to spare for the game's own field-of-view changes between two frames.
  if (g_early) {
    double c = std::max(std::max(A, B), kConsole);
    if (std::fabs(g_fov - 1.0) > 1e-6) c *= g_fov_k * 1.01;
    const float cf = float(c);
    if (g_shape <= 0 || std::fabs(cf / g_shape - 1.0f) > (std::fabs(g_fov - 1.0) > 1e-6 ? 0.004f : 1e-6f)) SetShape(cf);
  }
}

// ---------------------------------------------------------------- BeginScene (the fallback, and fps = 30)
using BeginSceneFn = long(__stdcall*)(void*);
BeginSceneFn g_bs_orig = nullptr;
uint32_t* g_bs_vt = nullptr;

long ENTRY_STD BeginSceneHook(void* dev) {
  if (ss_own_drawing == 0) FixCameras();  // not from the settings overlay's own BeginScene inside Present (menu.cpp)
  return g_bs_orig(dev);
}

void InstallBeginSceneHook() {
  uintptr_t r = Rd(kRenderer);
  if (!r || !Readable(r + kRendererDevice, 4)) return;
  uintptr_t dev = Rd(r + kRendererDevice);
  if (!dev || !Readable(dev, 4)) return;
  uint32_t* vt = reinterpret_cast<uint32_t*>(Rd(dev));
  if (!vt || !Readable(reinterpret_cast<uintptr_t>(vt), (kBeginSceneSlot + 1) * 4)) return;
  const uint32_t mine = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&BeginSceneHook));
  // Once per vtable: if another hook was later put over ours, the slot's word is not ours, and taking it again as our
  // original would make a loop (aspect -> other -> aspect ...).
  if (vt == g_bs_vt || vt[kBeginSceneSlot] == mine) return;
  HMODULE owner = nullptr, game = GetModuleHandleA(nullptr);
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<const char*>(vt[kBeginSceneSlot]), &owner) ||
      !owner || owner == game) {
    static bool said = false;
    if (!said) ss_log("[aspect] the device's BeginScene is not in a d3d9 module; the camera is widened from the frame step only");
    said = true;
    return;
  }
  g_bs_orig = reinterpret_cast<BeginSceneFn>(vt[kBeginSceneSlot]);
  if (!SwapWord(reinterpret_cast<uintptr_t>(&vt[kBeginSceneSlot]), mine)) {
    ss_log("[aspect] cannot write the device vtable (error %lu)", GetLastError());
    return;
  }
  if (g_bs_vt && g_bs_vt != vt) ss_log("[aspect] the device's vtable changed (%p -> %p): hooked again", g_bs_vt, vt);
  g_bs_vt = vt;
  ss_log("[aspect] BeginScene hooked (device %p, vtable %p)", reinterpret_cast<void*>(dev), vt);
}

// ================================================================================================ the interface
using SetViewportFn = void(__attribute__((thiscall)) *)(void*, const GViewport*);
using DisplayFn = void(__attribute__((thiscall)) *)(void*);

struct MovieEntry {
  void* movie;
  GViewport out;   // what we gave the movie last
  GViewport in;    // what the game asked for last
  uint32_t used;
  uintptr_t def;   // its movie definition when `full` was decided
  bool full;       // keeps the game's stretch
  bool bars;       // a full-screen menu over the stopped race: the side strips are made black after it is drawn
  bool named;      // its file name was read (an empty name is read again: the definition may still be loading)
  bool logged;     // its file name is in the log
  bool split_hud;  // Hud.gfx / TimerOverlay.gfx: in split screen drawn once per player half (DisplayHook)
  char name[80];   // the end of its file name (for the log)
};
constexpr int kMovies = 128;
MovieEntry g_movies[kMovies];
uint32_t g_tick = 0;
CRITICAL_SECTION g_cs;
uint32_t g_ui_logged = 0;
std::string g_ui_urls[100];  // the movie files already logged

MovieEntry* FindMovie(void* movie) {
  for (MovieEntry& e : g_movies)
    if (e.movie == movie) return &e;
  return nullptr;
}
MovieEntry* AddMovie(void* movie) {
  MovieEntry* best = &g_movies[0];
  for (MovieEntry& e : g_movies) {
    if (!e.movie) { best = &e; break; }
    if (e.used < best->used) best = &e;
  }
  memset(best, 0, sizeof *best);
  best->movie = movie;
  return best;
}

// The movie's file name (GFxMovieDefImpl::GetFileURL, read through the same fields as 0x922BD0), or "".
std::string MovieUrl(void* movie) {
  const uintptr_t m = reinterpret_cast<uintptr_t>(movie);
  if (!Readable(m + kMovieDef, 4)) return "";
  const uintptr_t def = Rd(m + kMovieDef);
  if (!def || !Readable(def, 0x20) || Rd(def) != kMovieDefVt) return "";
  uintptr_t p = Rd(def + 0x1C);
  if (!p || !Readable(p + 0x10, 4)) return "";
  p = Rd(p + 0x10);
  if (!p || !Readable(p + 0x1C, 4)) return "";
  p = Rd(p + 0x1C);
  if (!p || !Readable(p + 0x20, 4)) return "";
  p = Rd(p + 0x20);
  if (!p || !Readable(p + 0xC, 1)) return "";
  const char* s = reinterpret_cast<const char*>(p + 0xC);
  MEMORY_BASIC_INFORMATION mi;
  if (!SS_VQ(s, &mi, sizeof mi)) return "";
  const uintptr_t end = reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize;
  size_t n = 0;
  while (n < 260 && reinterpret_cast<uintptr_t>(s + n) < end && s[n]) ++n;
  return std::string(s, n);
}

// Movies that are full-screen bars or wipes keep the game's own stretch over the whole screen.
bool FullWidth(const std::string& url) {
  return url.find("Letterbox") != std::string::npos || url.find("Transitions") != std::string::npos;
}

// Full-screen menus drawn over the race's picture with bars or a backdrop of their own (the pause menu: black bars at
// the top and the bottom over the frozen race): boxed, their bars would end at the box and the race would show on in
// the side strips. After such a movie is drawn the strips are cleared to black, so the menu is pillarboxed like a
// menu over no picture (the loading screens).
bool Pillarbox(const std::string& url) {
  static const char* const kNames[] = {PILLARBOX_NAMES};
  for (const char* n : kNames)
    if (n[0] && url.find(n) != std::string::npos) return true;
  return false;
}

// Black side strips beside the box (the back buffer is the target: Box() only boxes a viewport of its size).
void BlackStrips(const GViewport& box) {
  int bw = 0, bh = 0;
  if (!BackBuffer(bw, bh) || box.w >= bw || box.w <= 0) return;
  const uintptr_t r = Rd(kRenderer);
  if (!r || !Readable(r + kRendererDevice, 4)) return;
  IDirect3DDevice9* dev = reinterpret_cast<IDirect3DDevice9*>(Rd(r + kRendererDevice));
  if (!dev) return;
  D3DVIEWPORT9 old, full = {0, 0, DWORD(bw), DWORD(bh), 0.0f, 1.0f};
  DWORD scissor = FALSE;
  if (FAILED(dev->GetViewport(&old))) return;
  dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor);
  dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
  dev->SetViewport(&full);
  const D3DRECT rc[2] = {{0, 0, box.l, bh}, {box.l + box.w, 0, bw, bh}};
  dev->Clear(2, rc, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
  dev->SetViewport(&old);
  dev->SetRenderState(D3DRS_SCISSORTESTENABLE, scissor);
}

// The 16:9 (hud_width) box in the middle of what the game asked for. False when nothing changes.
bool Box(const GViewport& in, GViewport& out) {
  out = in;
  if (in.w <= 0 || in.h <= 0) return false;
  // Only a movie drawn into the back buffer itself: one rendered into a texture of its own shape is left as it is.
  int bw = 0, bh = 0;
  if (!BackBuffer(bw, bh) || in.bw != bw || in.bh != bh) return false;
  const double A = ScreenRatio(bw, bh);
  const double T = BoxShape(A);
  if (T <= 0) return false;
  const double k = A / T;  // the screen's shape over the box's
  if (k > 1.0) {
    const int w = static_cast<int>(std::lround(in.w / k));
    out.l = in.l + (in.w - w) / 2;
    out.w = w;
  } else {
    const int h = static_cast<int>(std::lround(in.h * k));
    out.t = in.t + (in.h - h) / 2;
    out.h = h;
  }
  // Clip to the box (what the stage parks outside stays hidden, as it was at 16:9).
  int sl = in.l, st = in.t, sr = in.l + in.w, sb = in.t + in.h;
  if (in.flags & kUseScissor) {
    sl = in.sl, st = in.st, sr = in.sl + in.sw, sb = in.st + in.sh;
  }
  if (sl < out.l) sl = out.l;
  if (st < out.t) st = out.t;
  if (sr > out.l + out.w) sr = out.l + out.w;
  if (sb > out.t + out.h) sb = out.t + out.h;
  out.sl = sl;
  out.st = st;
  out.sw = sr > sl ? sr - sl : 0;
  out.sh = sb > st ? sb - st : 0;
  out.flags |= kUseScissor;
  return true;
}

void ENTRY_THIS SetViewportHook(void* movie, const GViewport* vp) {
  const SetViewportFn orig = reinterpret_cast<SetViewportFn>(kOrigSetViewport);
  if (!vp || !g_ui_on) return orig(movie, vp);
  GViewport out = *vp;
  bool log = false;
  std::string url;
  EnterCriticalSection(&g_cs);
  MovieEntry* e = FindMovie(movie);
  if (e && memcmp(vp, &e->out, sizeof out) == 0) {
    // The movie's own stored viewport coming back (SetViewScaleMode / SetViewAlignment pass it on): already ours.
    e->used = ++g_tick;
    LeaveCriticalSection(&g_cs);
    return orig(movie, vp);
  }
  if (!e) e = AddMovie(movie);
  e->used = ++g_tick;
  const uintptr_t def = Readable(reinterpret_cast<uintptr_t>(movie) + kMovieDef, 4) ? Rd(reinterpret_cast<uintptr_t>(movie) + kMovieDef) : 0;
  if (!def || def != e->def || !e->named) {
    url = MovieUrl(movie);
    e->full = FullWidth(url);
    e->bars = Pillarbox(url);
    e->split_hud = aspectcore::SplitHudName(url.c_str());
    e->def = def;
    e->named = !url.empty();
    const size_t n = url.size(), keep = sizeof e->name - 1, from = n > keep ? n - keep : 0;
    memcpy(e->name, url.c_str() + from, n - from);
    e->name[n - from] = 0;
  }
  const bool full = e->full;
  e->in = *vp;
  const bool changed = !full && Box(*vp, out);
  e->out = out;
  // Each movie file once (with the first viewport it was boxed in or kept full), up to 100 files: the list of the
  // game's movies on this screen. A file seen before under another movie object is not logged again.
  if (g_ui_logged < 100 && (changed || full) && e->named && !e->logged) {
    e->logged = true;
    url = e->name;
    log = true;
    for (uint32_t k = 0; k < g_ui_logged && log; ++k)
      if (g_ui_urls[k] == url) log = false;
    if (log) g_ui_urls[g_ui_logged++] = url;
  }
  const bool strips = e->bars;
  LeaveCriticalSection(&g_cs);
  if (log) {
    if (full)
      ss_log("[aspect] %s: full width (%dx%d at %d,%d), the game's stretch", url.empty() ? "movie" : url.c_str(), vp->w, vp->h, vp->l, vp->t);
    else
      ss_log("[aspect] %s: %dx%d at %d,%d -> %dx%d at %d,%d%s", url.empty() ? "movie" : url.c_str(), vp->w, vp->h, vp->l, vp->t, out.w, out.h,
             out.l, out.t, strips ? ", black side strips" : "");
  }
  orig(movie, &out);
}

// Two local players side by side: the viewport manager has two views and no forced single view (+8 = -1), the case in
// which 0x794370 halves the screen's shape for each view.
bool SplitScreen() {
  const uintptr_t mgr = Rd(kViewMgr);
  return mgr && Readable(mgr, 12) && Rd(mgr) == 2 && Rd(mgr + 8) == 0xFFFFFFFFu;
}

// Split screen on a screen wider than 16:9: the race HUD movies (Hud.gfx, TimerOverlay.gfx) hold player 1's HUD in
// the left half of their 16:9 stage and player 2's in the right half. Boxed whole, both would sit in the middle of the
// screen. So the movie is drawn twice instead: the stage at the box's shape, its left half in the middle of the left
// screen half, then its right half in the middle of the right one (scissor on that half), and the boxed viewport is
// put back. `in` is the game's full-screen viewport, `out` the box. False when this is not the case (drawn as before).
uint32_t g_split_logged = 0;
bool DisplayHalves(void* movie, const GViewport& in, const GViewport& out) {
  int bw = 0, bh = 0;
  if (!BackBuffer(bw, bh) || in.bw != bw || in.bh != bh || in.l != 0 || in.t != 0 || in.w != bw || in.h != bh) return false;
  const double A = ScreenRatio(bw, bh), T = BoxShape(A);
  if (!aspectcore::Wider(A) || T <= 0 || !SplitScreen()) return false;
  GViewport v[2];
  for (int p = 0; p < 2; ++p) {
    aspectcore::Rect vp, sc;
    if (!aspectcore::HalfView({in.l, in.t, in.w, in.h}, T, p, vp, sc)) return false;
    v[p] = in;
    v[p].l = vp.l;
    v[p].w = vp.w;
    v[p].sl = sc.l;
    v[p].st = sc.t;
    v[p].sw = sc.w;
    v[p].sh = sc.h;
    v[p].flags |= kUseScissor;
  }
  const SetViewportFn set = reinterpret_cast<SetViewportFn>(kOrigSetViewport);
  for (int p = 0; p < 2; ++p) {
    set(movie, &v[p]);
    reinterpret_cast<DisplayFn>(kOrigDisplay)(movie);
  }
  set(movie, &out);
  if (g_split_logged < 4) {
    ++g_split_logged;
    ss_log("[aspect] split screen: movie %p drawn once per player half (stage %dx%d at %d and %d, scissor %d px wide)", movie, v[0].w, v[0].h,
           v[0].l, v[1].l, v[0].sw);
  }
  return true;
}

// Gives the box to a movie whose viewport was set before the wrapper was there (or by anything that went around it).
void ENTRY_THIS DisplayHook(void* movie) {
  if (g_ui_on && movie) {
    GViewport stored;
    memcpy(&stored, reinterpret_cast<const char*>(movie) + kMovieViewport, sizeof stored);
    EnterCriticalSection(&g_cs);
    MovieEntry* e = FindMovie(movie);
    const bool known = e && memcmp(&stored, &e->out, sizeof stored) == 0;
    LeaveCriticalSection(&g_cs);
    if (!known) SetViewportHook(movie, &stored);
    GViewport in, out;
    bool halves = false;
    EnterCriticalSection(&g_cs);
    if ((e = FindMovie(movie)) && e->split_hud && !e->full && memcmp(&e->in, &e->out, sizeof in) != 0) {
      in = e->in;
      out = e->out;
      halves = true;
    }
    LeaveCriticalSection(&g_cs);
    if (halves && DisplayHalves(movie, in, out)) return;
  }
  reinterpret_cast<DisplayFn>(kOrigDisplay)(movie);
  if (g_ui_on && movie) {
    GViewport box;
    bool bars = false;
    EnterCriticalSection(&g_cs);
    if (MovieEntry* e = FindMovie(movie)) {
      bars = e->bars && !e->full;
      box = e->out;
    }
    LeaveCriticalSection(&g_cs);
    if (bars) BlackStrips(box);
  }
}

bool g_ui_hooked = false, g_ui_failed = false;

// Waits for the vtable to be the known one (its neighbouring slots GetViewport 0x9823F0 and SetViewScaleMode
// 0x982410), then swaps the two words.
void InstallUiHooks() {
  if (g_ui_hooked || g_ui_failed) return;
  if (!Readable(kMovieVt, 0x80) || Rd(kMovieVt + 0x54) != 0x9823F0 || Rd(kMovieVt + 0x58) != 0x982410) return;
  const uint32_t sv = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&SetViewportHook));
  const uint32_t dp = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&DisplayHook));
  const uint32_t cur_sv = Rd(kSlotSetViewport), cur_dp = Rd(kSlotDisplay);
  if ((cur_sv != kOrigSetViewport && cur_sv != sv) || (cur_dp != kOrigDisplay && cur_dp != dp)) {
    ss_log("[aspect] the movie vtable's SetViewport / Display slots hold %08x / %08x, not the known build's; interface left stretched", cur_sv,
           cur_dp);
    g_ui_failed = true;
    return;
  }
  if (!SwapWord(kSlotSetViewport, sv) || !SwapWord(kSlotDisplay, dp)) {
    ss_log("[aspect] cannot write the movie vtable (error %lu); interface left stretched", GetLastError());
    SwapWord(kSlotSetViewport, kOrigSetViewport);
    g_ui_failed = true;
    return;
  }
  g_ui_hooked = true;
  ss_log("[aspect] movie SetViewport (%p) and Display (%p) wrapped: the interface in a %.3f box", reinterpret_cast<void*>(kSlotSetViewport),
         reinterpret_cast<void*>(kSlotDisplay), g_hud);
}

// ================================================================================================ engine values
// Four engine tweakables (floats in .data, writable data; no code), set only while the screen is wider than 16:9:
// * the 3D player tags (the names over the other cars in split screen and online): their sizes are a share of the
//   viewport's width (0x5697xx: tweakable / view width), tuned for 16:9, so a wider screen stretches them. Name
//   Width, Icon Width and Icon Offset X are scaled by 16:9 / screen shape (heights untouched): they keep their shape.
// * LetterBox Amount (0.118, read by 0x793E50): the black bars the engine puts above and below the picture in the
//   car and race intros and cinematics. On a screen already wider than 16:9 they would turn the picture into a thin
//   strip: 0 (no bars). Our black side strips beside boxed menus are a different thing (they stay).
// Each is checked once, when a wider screen first asks for it: its name string (the tweakable's registration, .rdata)
// and its stock value. A value that is neither stock nor what we wrote is someone else's: left alone. Back on a
// screen that is not wider (resolution change), the stock values are put back. On 16:9 and 16:10 nothing is read
// or written.
struct Tweak {
  uintptr_t va;
  uint32_t stock;
  uintptr_t name_va;
  const char* name;
  bool tag;           // scaled by TagScale; otherwise set to 0
  int state;          // -1 not checked, 0 refused, 1 ok
  uint32_t written;   // what we wrote last (0 = nothing)
};
Tweak g_tweaks[] = {
    {0xD1DB68, 0x3F0A3D71, 0xBD1268, "UI/HUD/PlayerNames3D/ba;Name Width", true, -1, 0},
    {0xD1DB78, 0x3DCCCCCD, 0xBD12B0, "UI/HUD/PlayerNames3D/bc;Icon Width", true, -1, 0},
    {0xD1DB88, 0xBC814793, 0xBD12F8, "UI/HUD/PlayerNames3D/be;Icon Offset X", true, -1, 0},
    {0xD36F54, 0x3DF1A9FC, 0xC12178, "LetterBox/LetterBox Amount", false, -1, 0},
};

// a: the screen's shape, or 0 for "not wider" (aspect = 16:9).
void EngineValues(double a) {
  const bool wide = a > 0 && aspectcore::Wider(a);
  const float k = aspectcore::TagScale(wide ? a : 0);
  for (Tweak& t : g_tweaks) {
    if (t.state < 0) {
      if (!wide) continue;  // nothing is checked or touched until a wider screen asks
      const size_t n = strlen(t.name) + 1;
      t.state = Readable(t.name_va, n) && memcmp(reinterpret_cast<const void*>(t.name_va), t.name, n) == 0 && Readable(t.va, 4) &&
                        Rd(t.va) == t.stock
                    ? 1
                    : 0;
      if (!t.state) ss_log("[aspect] tweakable %s at %p is not the known build's; left as it is", t.name, reinterpret_cast<void*>(t.va));
    }
    if (t.state != 1) continue;
    uint32_t want = t.stock;
    if (wide) {
      float f = 0;
      if (t.tag) {
        memcpy(&f, &t.stock, 4);
        f *= k;
      }
      memcpy(&want, &f, 4);
    }
    const uint32_t cur = Rd(t.va);
    if (cur == want) continue;
    if (cur != t.stock && cur != t.written) {
      ss_log("[aspect] tweakable %s was changed by something else (0x%08X); left as it is", t.name, cur);
      t.state = 0;
      continue;
    }
    *reinterpret_cast<volatile uint32_t*>(t.va) = want;
    t.written = want;
    float f;
    memcpy(&f, &want, 4);
    ss_log("[aspect] %s = %.4f%s", t.name, f, wide ? "" : " (stock)");
  }
}

// ================================================================================================ worker
DWORD WINAPI AspectWorker(LPVOID) {
  if (!ss_conf_present()) return 0;  // no conf file: the DLL does nothing
  ReadSettings();
  if (!g_on) return 0;
  if (GetModuleHandleA(nullptr) != reinterpret_cast<HMODULE>(0x400000)) {
    ss_log("[aspect] unexpected image base; nothing done");
    return 0;
  }
  // Nothing is hooked until the back buffer's shape asks for it (a 16:9 screen at fov 100, or 16:10 with the
  // defaults, gets no hook at all). The device can be re-created (resolution change): keep looking.
  int seen_w = 0, seen_h = 0;
  for (;;) {
    int bw = 0, bh = 0;
    if (BackBuffer(bw, bh)) {
      const bool cam = CamWanted(bw, bh), ui = UiWanted(bw, bh);
      if (bw != seen_w || bh != seen_h) {
        seen_w = bw;
        seen_h = bh;
        ss_log("[aspect] back buffer %dx%d: scene %s, interface %s", bw, bh, cam ? "widened / fov" : "the game's own",
               ui ? "boxed" : "the game's full screen");
      }
      if (ui) InstallUiHooks();
      if (cam) InstallBeginSceneHook();
      EngineValues(g_off ? 0 : ScreenRatio(bw, bh));
    }
    Sleep(250);
  }
  return 0;
}

}  // namespace

// patch.cpp: the frame step calls this right after the game's update (before the render).
void ss_aspect_after_update() { FixCameras(); }

void ss_aspect_start() {
  InitializeCriticalSection(&g_cs);
  CreateThread(nullptr, 0, AspectWorker, nullptr, 0, nullptr);
}
