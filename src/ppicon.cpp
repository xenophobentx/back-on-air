// Power Play icons: the same share of the screen at every resolution (GitHub issue #3, huge icons at 4K).
//
// What the PC build does (Steam build 385319, run-time VAs):
//
// * The race HUD controller (the object at *(0xD66B24) + 0x20, vptr 0xBB87F4, constructor 0x4C9AF0, made once per
//   race load at 0x4B1178) reads the frame buffer's size once (renderer *0xD64708 -> [+4] -> +0x4C / +0x50 into
//   +0x2B0 / +0x2B4, ints) and, at the end of its constructor (0x4C9E18..0x4C9E56), multiplies the "PP Icons" tweaks
//   Icon Min Scale (+0x214, 0.525 from .rdata 0xBB87F8) and Icon Max Scale (+0x218, 1.5 from 0xC3276C) by W / 1280.
// * Kind 1 (the Power Play icons over the rivals, 0x4C2B20): the scale is the lerp of max .. min over 10 .. 100 m;
//   kind 2 (two markers, 0x4C7CD9): the scale is W * [0xBB8494] (.rdata 2.25 / 1280, its only reader). Both are drawn
//   by 0x55E740 with a side of W * scale / 16 pixels (* 0.5 at quality Very High, where the frame buffer is twice
//   the back buffer).
// * So W is counted twice: the side grows with W squared. Its share of the screen is right only at W = 1280 (the
//   Xbox 360's 1280x720); at 1920 it is 1.5 times, at 3840 3 times the console's.
//
// What this file does (data only, nothing in the game's code is written):
//
// * 0xBB8494 is set to 2.25 / S (S the back buffer's width, renderer +0x30) and written again when S changes.
// * Once per HUD instance (every race load) +0x214 and +0x218 are multiplied by 1280 / S. Only an object with the
//   HUD's vptr whose two values are still the game's own (the tweak times W / 1280) is changed, so each instance is
//   scaled once; a new race's HUD comes with the game's values again and is scaled again.
// With both, the side is W * tweak * W / S / 16 = tweak * S / 16 (W = S), or 2S * tweak * 2 * 0.5 / 16 over a frame
// buffer of 2S (Very High): the console's share of the width at any resolution. At S = 1280 nothing changes.
// It runs from patch.cpp's Present hook (the game's thread, between frames); the HUD is looked at every 32nd present.

#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include "ss_shared.h"

namespace {

constexpr uintptr_t kRenderer = 0xD64708;     // renderer: +0x30 back buffer width
constexpr uintptr_t kRendererBufW = 0x30;
constexpr uintptr_t kHudOwner = 0xD66B24;     // the race object; the HUD controller is embedded at +0x20
constexpr uintptr_t kHudInOwner = 0x20;
constexpr uint32_t kHudVt = 0xBB87F4;
constexpr uintptr_t kHudW = 0x2B0;            // int: frame buffer width, read once by the constructor
constexpr uintptr_t kHudMin = 0x214, kHudMax = 0x218;
constexpr uintptr_t kTweakMin = 0xBB87F8;     // .rdata 0.525 (Icon Min Scale)
constexpr uintptr_t kTweakMax = 0xC3276C;     // .rdata 1.5 (Icon Max Scale, shared)
constexpr uintptr_t kMarkerConst = 0xBB8494;  // .rdata 2.25 / 1280, read only at 0x4C7CD9
constexpr uint32_t kMarkerConstOrig = 0x3AE66666;
constexpr double kConsoleW = 1280.0;

bool g_on = false;
bool g_failed = false;     // the build is not the known one: nothing done
int g_const_s = 0;         // the back buffer width 0xBB8494 was last set for
uint32_t g_const_bits = kMarkerConstOrig;
uintptr_t g_hud_done = 0;  // the instance we scaled and what we wrote (to see a new instance at the same address)
uint32_t g_min_bits = 0, g_max_bits = 0;

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
inline float F(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
inline uint32_t U(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
bool Near(float a, double b) { return std::fabs(double(a) - b) <= 1e-4 * std::fabs(b) + 1e-7; }

bool CheckBuild() {
  static const uint8_t mul[] = {0xF3, 0x0F, 0x59, 0x25, 0x94, 0x84, 0xBB, 0x00};  // 0x4C7CD9 mulss xmm4, [0xBB8494]
  static const uint8_t fild[] = {0xDB, 0x87, 0xB0, 0x02, 0x00, 0x00};               // 0x4C9E18 fild [edi+0x2B0]
  static const uint8_t fmul[] = {0xD8, 0x8F, 0x18, 0x02, 0x00, 0x00};               // 0x4C9E34 fmul [edi+0x218]
  return Readable(0x4C7CD9, 8) && !memcmp(reinterpret_cast<const void*>(0x4C7CD9), mul, 8) &&
         !memcmp(reinterpret_cast<const void*>(0x4C9E18), fild, 6) && !memcmp(reinterpret_cast<const void*>(0x4C9E34), fmul, 6) &&
         Readable(kMarkerConst, 4) && Rd(kMarkerConst) == kMarkerConstOrig && Rd(kTweakMin) == U(0.525f) && Rd(kTweakMax) == U(1.5f);
}

// The renderer lives as long as the game: its object is checked once (no VirtualQuery a frame).
int BackBufferWidth() {
  static uintptr_t checked = 0;
  const uintptr_t r = Rd(kRenderer);
  if (!r) return 0;
  if (r != checked) {
    if (!Readable(r + kRendererBufW, 4)) return 0;
    checked = r;
  }
  const int w = static_cast<int>(Rd(r + kRendererBufW));
  return w > 0 && w < 32768 ? w : 0;
}

}  // namespace

void ss_ppicon_start() {
  if (!ss_conf_present()) return;  // no conf file: the DLL does nothing
  g_on = GetModuleHandleA(nullptr) == reinterpret_cast<HMODULE>(0x400000);
}

// patch.cpp's Present hook (the game's thread): the marker constant for the back buffer, then the HUD's icon scales.
void ss_ppicon_present() {
  if (!g_on || g_failed) return;
  const int s = BackBufferWidth();
  if (!s) return;
  if (s != g_const_s) {
    static bool checked = false;
    if (!checked) {
      checked = true;
      if (!CheckBuild()) {
        g_failed = true;
        ss_log("[ppicon] the HUD's icon code is not the known build; the Power Play icons keep the game's size");
        return;
      }
    }
    const uint32_t want = U(static_cast<float>(2.25 / s));
    if (Rd(kMarkerConst) != g_const_bits || !ss_write_word(reinterpret_cast<void*>(kMarkerConst), want)) {
      g_failed = true;
      ss_log("[ppicon] cannot write the marker constant at %p (holds 0x%08X); the Power Play icons keep the game's size",
             reinterpret_cast<void*>(kMarkerConst), Rd(kMarkerConst));
      return;
    }
    g_const_bits = want;
    g_const_s = s;
  }

  // The HUD is looked at every 32nd present (a quarter of a second at 120 fps; a race's HUD is made during the load,
  // long before an icon can show).
  static unsigned n = 0;
  if (++n & 31) return;
  const uintptr_t owner = Rd(kHudOwner);
  if (!owner) return;
  const uintptr_t hud = owner + kHudInOwner;
  if (!Readable(hud, kHudMax + 4) || !Readable(hud + kHudW, 4) || Rd(hud) != kHudVt) return;
  const uint32_t min_bits = Rd(hud + kHudMin), max_bits = Rd(hud + kHudMax);
  if (hud == g_hud_done && min_bits == g_min_bits && max_bits == g_max_bits) return;  // already ours
  const int w = static_cast<int>(Rd(hud + kHudW));
  if (w <= 0 || w >= 32768) return;
  // Still the game's own values: the tweak times W / 1280 (the constructor's last step is done).
  const double k = w / kConsoleW;
  if (!Near(F(min_bits), 0.525 * k) || !Near(F(max_bits), 1.5 * k)) return;
  const float new_min = static_cast<float>(F(min_bits) * kConsoleW / s), new_max = static_cast<float>(F(max_bits) * kConsoleW / s);
  *reinterpret_cast<volatile uint32_t*>(hud + kHudMin) = U(new_min);
  *reinterpret_cast<volatile uint32_t*>(hud + kHudMax) = U(new_max);
  g_hud_done = hud;
  g_min_bits = U(new_min);
  g_max_bits = U(new_max);
  ss_log("[ppicon] HUD %p: frame buffer width %d, back buffer width %d: icon scale min %.4f -> %.4f, max %.4f -> %.4f; "
         "marker scale constant %.6g -> %.6g (2.25/%d)",
         reinterpret_cast<void*>(hud), w, s, F(min_bits), new_min, F(max_bits), new_max, F(kMarkerConstOrig), F(g_const_bits), s);
}
