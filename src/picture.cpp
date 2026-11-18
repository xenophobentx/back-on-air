// Picture switches of the game's own post passes and tweakables (SSPC-54):
//
//   motion_blur     = console | game | off   (console)  SS_MOTION_BLUR
//   motion_blur_quality = game | low | high  (high; Deck low)  SS_MOTION_BLUR_QUALITY  (mblur.cpp: our blur shader)
//   vignette        = game | off             (game)     SS_VIGNETTE
//   screen_gradient = game | off             (game)     SS_SCREEN_GRADIENT
//   lens_flare      = game | off             (game)     SS_LENS_FLARE
//   explosion_shake = game | off             (game)     SS_EXPLOSION_SHAKE
//
// All live from the settings menu. Picture and camera only: nothing here reaches the simulation, so online play is
// not affected. The addresses below were found by Glimpse-Fix (frankyfife, GPL-3.0); none of its code is used, each
// one was checked again in the disassembly of the Steam build (run-time VAs):
//
// * The screen-space motion blur pass 0x49B490 (thiscall, 5 stack arguments, ret 14h) is reached only through the
//   vtable slot 0xBB42B0 in .rdata. It blurs along the camera's motion over one frame, scaled by the camera's blur
//   amount, the float at [[this+0x14]+0x14] (read at 0x49B60C into the pass's constant). The game was tuned at 30 fps;
//   at 60 one frame holds half the motion and the streaks are half as long, at 120 a quarter. console multiplies the
//   amount for the call by the update rate / 30 (patch.cpp's cadence: 30, 60, or the race's fps), at most 4, and puts
//   the camera's value back after it. A longer blur samples past the picture's edge and brings in a thin line at the
//   bottom: while the stronger pass runs, quality.cpp's SetSamplerState hook clamps U / V of samplers 0-7 to the edge
//   (and the title's own values go back after it). off sets the amount to 0 for the call. The game runs the pass only
//   at graphics quality High and Very High.
// * The colour grade pass 0x49A7A0 (same convention, slot 0xBB4048) reads the camera's vignette from the same block:
//   U and V scale at +0x48 / +0x4C (0x49A498), and the top-of-screen gradient's colour at +0x1C..+0x2C, alpha at
//   +0x2C (0x49A30F). vignette = off sets both scales to 0 for the call (every pixel then counts as the centre),
//   screen_gradient = off the alpha. The grading itself is not changed.
// * Two bool tweakables in .data: "PostProcess/SSLensFlare/z; Enable" 0xD10A3C (read at 0x499FC0) and
//   "LensFlare/Config/a; Enable" 0xD37D08 (0x7B1B36). lens_flare = off holds both at 0 (a level may set them again).
// * The bool tweakable "AreaOfEffects/DebugTrigger/DisallowAllCameraShake" 0xD8B4CC, checked by the camera shake
//   controller (0x82810A). explosion_shake = off holds it at 1.
//
// Nothing in the game's code is written: two .rdata words through ss_write_word, three .data bytes. On any mismatch
// with the known build the part concerned stays as the game has it, with one line in the log.
#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "menu.h"
#include "ss_shared.h"

namespace {

constexpr uintptr_t kRenderer = 0xD64708;  // renderer; the device is renderer+0x578 (as postfx.cpp)
constexpr uint32_t kRendererDevice = 0x578;
constexpr uintptr_t kBlurPass = 0x49B490, kBlurSlot = 0xBB42B0;
constexpr uintptr_t kGradePass = 0x49A7A0, kGradeSlot = 0xBB4048;
constexpr uint8_t kBlurSig[] = {0x83, 0xEC, 0x68, 0x57, 0x8B, 0xF9, 0x8B, 0x47};   // sub esp,68h; push edi; mov edi,ecx; mov eax,[edi+4]
constexpr uint8_t kGradeSig[] = {0x83, 0xEC, 0x08, 0x57, 0x8B, 0xF9, 0x8B, 0x47};  // sub esp,8; ...
constexpr uintptr_t kFlareA = 0xD10A3C, kFlareB = 0xD37D08, kNoShake = 0xD8B4CC;
// The tweak registrations (push <address>) in the game's static set-up: the image is unpacked and the bytes are these.
constexpr uintptr_t kFlareASite = 0xB6C1C0, kFlareBSite = 0xB80AB0, kNoShakeSite = 0xB84160;
constexpr float kMaxBlurFactor = 4.0f;

enum { BLUR_CONSOLE = 0, BLUR_GAME = 1, BLUR_OFF = 2 };
volatile LONG g_blur = BLUR_CONSOLE;
volatile LONG g_vignette_off = 0, g_gradient_off = 0, g_flare_off = 0, g_shake_off = 0;
volatile LONG g_blur_in = 0, g_grade_in = 0, g_data_in = 0, g_started = 0;

inline uint32_t Rd(uintptr_t va) { return *reinterpret_cast<const volatile uint32_t*>(va); }

std::string Setting(const char* env, const char* key) {
  const char* e = getenv(env);
  std::string v = e ? std::string(e) : ss_conf(key);
  for (char& c : v) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return v;
}

int ParseBlur(const std::string& v) {
  if (v == "game" || v == "pc") return BLUR_GAME;
  if (v == "off" || v == "false" || v == "0") return BLUR_OFF;
  return BLUR_CONSOLE;  // console, absent, anything else
}
bool ParseOff(const std::string& v) { return v == "off" || v == "false" || v == "0" || v == "no"; }

const char* BlurName(int b) { return b == BLUR_GAME ? "game" : b == BLUR_OFF ? "off" : "console"; }

bool Site(uintptr_t site, uintptr_t va) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(site);
  uint32_t imm;
  memcpy(&imm, p + 1, 4);
  return p[0] == 0x68 && imm == va;
}

// ------------------------------------------------------------------------------------------- the two passes

using PassFn = void(__attribute__((thiscall)) *)(void*, void*, void*, void*, void*, void*);

float BlurFactor() {
  const LONG b = g_blur;
  if (b == BLUR_OFF) return 0.0f;
  if (b == BLUR_GAME) return 1.0f;
  float k = static_cast<float>(ss_frame_update_rate() / 30.0);
  if (!(k >= 1.0f)) k = 1.0f;
  return k > kMaxBlurFactor ? kMaxBlurFactor : k;
}

void __attribute__((thiscall, force_align_arg_pointer)) BlurHook(uint8_t* self, void* a, void* b, void* c, void* d, void* e) {
  const PassFn orig = reinterpret_cast<PassFn>(kBlurPass);
  const uint8_t* state = *reinterpret_cast<uint8_t* const*>(self + 4);  // the game reads it first, too
  uint8_t* params = *reinterpret_cast<uint8_t**>(self + 0x14);
  const float k = BlurFactor();
  ss_mblur_pass_begin();  // mblur.cpp: motion_blur_quality = low / high binds our shader for the pass's programs
  if (!state || !state[8] || !params || k == 1.0f) {
    orig(self, a, b, c, d, e);
    ss_mblur_pass_end();
    return;
  }
  float* amount = reinterpret_cast<float*>(params + 0x14);
  const float stock = *amount;
  *amount = stock * k;
  const bool clamp = k > 1.0f && ss_quality_edge_clamp(true);
  orig(self, a, b, c, d, e);
  if (clamp) ss_quality_edge_clamp(false);
  *amount = stock;
  ss_mblur_pass_end();
  static float said = -1.0f;
  static int lines = 0;
  if (k != said && lines < 8) {  // each factor once (fps 120: 60 in the menus, 120 in a race)
    said = k;
    ++lines;
    ss_log("[blur] the blur pass runs with the camera's amount %.3f x %.2f%s", stock, k,
           k > 1.0f ? (clamp ? ", edges clamped" : ", edges not clamped (no SetSamplerState hook)") : "");
  }
}

void __attribute__((thiscall, force_align_arg_pointer)) GradeHook(uint8_t* self, void* a, void* b, void* c, void* d, void* e) {
  const PassFn orig = reinterpret_cast<PassFn>(kGradePass);
  const bool vig = g_vignette_off != 0, grad = g_gradient_off != 0;
  const uint8_t* state = *reinterpret_cast<uint8_t* const*>(self + 4);
  uint8_t* params = *reinterpret_cast<uint8_t**>(self + 0x14);
  // As the pass itself: its block is read only when it is on (+8) and has its target (+0x74).
  if (!(vig || grad) || !state || !state[8] || !*reinterpret_cast<const uint32_t*>(self + 0x74) || !params)
    return orig(self, a, b, c, d, e);
  float* uv = reinterpret_cast<float*>(params + 0x48);
  float* alpha = reinterpret_cast<float*>(params + 0x2C);
  const float u = uv[0], v = uv[1], al = *alpha;
  if (vig) uv[0] = uv[1] = 0.0f;
  if (grad) *alpha = 0.0f;
  orig(self, a, b, c, d, e);
  uv[0] = u;
  uv[1] = v;
  *alpha = al;
}

// The slot holds the known pass and the pass starts with the known bytes: pointed at the wrapper.
bool Wrap(uintptr_t slot, uintptr_t pass, const uint8_t* sig, size_t n, void* hook) {
  const uintptr_t mine = reinterpret_cast<uintptr_t>(hook);
  if (Rd(slot) == mine) return true;
  if (Rd(slot) != pass || memcmp(reinterpret_cast<const void*>(pass), sig, n) != 0) return false;
  uintptr_t prev = 0;
  if (!ss_write_word(reinterpret_cast<void*>(slot), mine, &prev)) return false;
  if (prev != pass) {  // another writer in between: its word goes back
    ss_write_word(reinterpret_cast<void*>(slot), prev);
    return false;
  }
  return true;
}

// ------------------------------------------------------------------------------------------- the tweakables

struct Held {
  uintptr_t va;
  uint8_t game;   // the game's value we held over
  bool ours;      // the byte holds our value
};
Held g_flares[2] = {{kFlareA, 1, false}, {kFlareB, 1, false}};
Held g_shake = {kNoShake, 0, false};

// `hold`: the byte should read `want`; otherwise the game's value goes back once.
void Hold(Held& h, bool hold, uint8_t want) {
  volatile uint8_t* p = reinterpret_cast<volatile uint8_t*>(h.va);
  const uint8_t cur = *p;
  if (hold) {
    if (cur == want || cur > 1) return;  // a bool tweakable: anything else is not left to us
    h.game = cur;
    *p = want;
    h.ours = true;
  } else if (h.ours) {
    if (cur == want) *p = h.game;
    h.ours = false;
  }
}

bool DeviceReady() {
  const uintptr_t r = Rd(kRenderer);
  if (!r) return false;
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(reinterpret_cast<void*>(r + kRendererDevice), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
      (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !mbi.Protect)
    return false;
  return Rd(r + kRendererDevice) != 0;
}

DWORD WINAPI Worker(LPVOID) {
  // The game's device exists: the image is unpacked and its static set-up (the tweak registrations) has run.
  const DWORD t0 = GetTickCount();
  while (!DeviceReady()) {
    if (GetTickCount() - t0 > 300000) {
      ss_log("[picture] no game device after 300 s: motion blur, vignette, gradient, flares and shake left to the game");
      return 0;
    }
    Sleep(100);
  }
  // The passes' slots: up to 60 s more for the known words (they are there by now in the known build).
  bool blur = false, grade = false;
  for (int i = 0; i < 600 && !(blur && grade); ++i) {
    if (!blur) blur = Wrap(kBlurSlot, kBlurPass, kBlurSig, sizeof kBlurSig, reinterpret_cast<void*>(&BlurHook));
    if (!grade) grade = Wrap(kGradeSlot, kGradePass, kGradeSig, sizeof kGradeSig, reinterpret_cast<void*>(&GradeHook));
    if (!(blur && grade)) Sleep(100);
  }
  g_blur_in = blur;
  g_grade_in = grade;
  const LONG b = g_blur;
  if (blur)
    ss_log("[blur] motion_blur %s: the blur pass (0x49B490, slot 0xBB42B0) wrapped; %s", BlurName(b),
           b == BLUR_CONSOLE ? (ss_picture_clamp_wanted()
                                    ? "amount x update rate / 30 (x2 at 60, x4 at 120 and above), edges clamped while it runs"
                                    : "amount x update rate / 30 (x1 at fps 30)")
           : b == BLUR_OFF ? "amount 0 (no blur)"
                           : "the game's amount (shorter blur above 30 fps)");
  else
    ss_log("[blur] motion_blur %s: the blur pass's slot 0xBB42B0 is not the known build's (%08x); the game's blur", BlurName(b),
           Rd(kBlurSlot));
  ss_log("[picture] vignette %s: %s", g_vignette_off ? "off" : "game",
         grade ? "the colour grade pass (0x49A7A0, slot 0xBB4048) wrapped" : "the grade pass's slot 0xBB4048 is not the known build's; the game's");
  ss_log("[picture] screen_gradient %s: %s", g_gradient_off ? "off" : "game",
         grade ? "the same wrapper (its alpha at +0x2C)" : "the game's");
  const bool data = Site(kFlareASite, kFlareA) && Site(kFlareBSite, kFlareB) && Site(kNoShakeSite, kNoShake);
  g_data_in = data;
  ss_log("[picture] lens_flare %s: %s", g_flare_off ? "off" : "game",
         data ? "the switches 0xD10A3C and 0xD37D08 watched (held at 0 when off)" : "the tweak registrations are not the known build's; the game's");
  ss_log("[picture] explosion_shake %s: %s", g_shake_off ? "off" : "game",
         data ? "DisallowAllCameraShake 0xD8B4CC watched (held at 1 when off)" : "the tweak registration is not the known build's; the game's");
  if (!data) return 0;
  for (;;) {
    const bool flares = g_flare_off != 0;
    for (Held& h : g_flares) Hold(h, flares, 0);
    Hold(g_shake, g_shake_off != 0, 1);
    Sleep(50);
  }
  return 0;
}

// ------------------------------------------------------------------------------------------- the menu

bool Apply(const char* key, const char* value) {
  const std::string v = value ? value : "";
  if (!strcmp(key, "motion_blur_quality")) return g_blur_in && ss_mblur_apply(value);
  if (!strcmp(key, "motion_blur")) {
    if (!g_blur_in) return false;
    g_blur = ParseBlur(v);
    ss_log("[blur] from the settings menu: motion_blur %s", BlurName(g_blur));
    return true;
  }
  volatile LONG* flag = !strcmp(key, "vignette")          ? &g_vignette_off
                        : !strcmp(key, "screen_gradient") ? &g_gradient_off
                        : !strcmp(key, "lens_flare")      ? &g_flare_off
                        : !strcmp(key, "explosion_shake") ? &g_shake_off
                                                          : nullptr;
  if (!flag) return false;
  const bool pass = flag == &g_vignette_off || flag == &g_gradient_off;
  if (!(pass ? g_grade_in : g_data_in)) return false;
  *flag = ParseOff(v);
  ss_log("[picture] from the settings menu: %s %s", key, *flag ? "off" : "game");
  return true;
}

const MenuChoice kGameOff[] = {{"game", "ON"}, {"off", "OFF"}};
MenuRegistrar g_rows[] = {
    MenuRegistrar(MenuChoiceRow("motion_blur", "MOTION BLUR", MENU_PAGE_GRAPHICS, MENU_LIVE, "console",
                                {{"console", "AS AT 30 FPS"}, {"game", "PC ORIGINAL"}, {"off", "OFF"}}, Apply)),
    MenuRegistrar(MenuChoiceRow("motion_blur_quality", "MOTION BLUR QUALITY", MENU_PAGE_GRAPHICS, MENU_LIVE, ss_mblur_default(),
                                {{"game", "GAME"}, {"low", "LOW"}, {"high", "HIGH"}}, Apply)),
    MenuRegistrar(MenuChoiceRow("vignette", "VIGNETTE", MENU_PAGE_GRAPHICS, MENU_LIVE, "game", {kGameOff[0], kGameOff[1]}, Apply)),
    MenuRegistrar(MenuChoiceRow("screen_gradient", "TOP GRADIENT", MENU_PAGE_GRAPHICS, MENU_LIVE, "game", {kGameOff[0], kGameOff[1]}, Apply)),
    MenuRegistrar(MenuChoiceRow("lens_flare", "LENS FLARES", MENU_PAGE_GRAPHICS, MENU_LIVE, "game", {kGameOff[0], kGameOff[1]}, Apply)),
    MenuRegistrar(MenuChoiceRow("explosion_shake", "EXPLOSION CAMERA SHAKE", MENU_PAGE_GAME, MENU_LIVE, "game", {kGameOff[0], kGameOff[1]}, Apply)),
};

}  // namespace

bool ss_picture_clamp_wanted() {
  return ParseBlur(Setting("SS_MOTION_BLUR", "motion_blur")) == BLUR_CONSOLE && ss_frame_wanted_above_30();
}

void ss_picture_start() {
  if (InterlockedExchange(&g_started, 1)) return;
  if (GetModuleHandleA(nullptr) != reinterpret_cast<HMODULE>(0x400000)) {
    ss_log("[picture] unexpected image base; motion blur, vignette, gradient, flares and shake left to the game");
    return;
  }
  g_blur = ParseBlur(Setting("SS_MOTION_BLUR", "motion_blur"));
  g_vignette_off = ParseOff(Setting("SS_VIGNETTE", "vignette"));
  g_gradient_off = ParseOff(Setting("SS_SCREEN_GRADIENT", "screen_gradient"));
  g_flare_off = ParseOff(Setting("SS_LENS_FLARE", "lens_flare"));
  g_shake_off = ParseOff(Setting("SS_EXPLOSION_SHAKE", "explosion_shake"));
  CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
  ss_mblur_start();
}
