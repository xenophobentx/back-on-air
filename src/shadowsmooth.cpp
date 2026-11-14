// Shadow smoothing: the sun shadow's hand-over from one cascade to the next as a dithered band instead of a line.
// The port's key, values and default (its SHADOW SMOOTHING row):
//   shadow_smoothing = off | on | high   (default high since 2026-11-13; the band 0.25 / 0.4 of each split)   SS_SHADOW_SMOOTHING
//
// What the title does (the same on PC and on the console; reverse engineering of the Steam build 385319, run-time VAs):
// the sun shadow is three cascades of 1024 x 1024 texels in one atlas. The fit (0x8950C0 at HIGH, 0x898D70 at MEDIUM)
// takes for each cascade a sphere around its slice of the view, with the radius of its first fit kept (cascade
// +0x748) and the light's projection moved to whole texels (the projected origin truncated to the texel grid,
// 0x897F72..0x898095 / 0x8995DC): the maps themselves are stable while driving. What moves is the line where one
// cascade gives way to the next: the shaders that read the shadow (the deferred sun pass and the forward ones) pick
// the cascade per pixel by its view depth against ShadowMap_SplitDists (x and y the first two splits, z the shadow's
// range; for example `if_lt depth, c40.x` in the deferred pass). The first split is 25 to 35 m ahead of the camera
// (split weight 0.8 or 0.7 by camera type), so a band where a sharp shadow turns into a coarse one runs along in front
// of the car and every shadow it crosses changes its look at once.
//
// What this does: every pixel shader the game sets that has ShadowMap_SplitDists in its constant table (the
// bytecode's CTAB; 18 of the PC's shaders, all ps_3_0, none with relative addressing) is replaced, at
// SetPixelShader (slot 107 of the device's vtable, in d3d9.dll's memory), by a variant made on first use: a few
// instructions in front put the splits into a free temporary, x and y moved towards the camera by up to the band's
// share of themselves by an interleaved gradient noise of the pixel position (fixed in screen space), and every read
// of the constant reads that temporary instead. Over the band before each split the finer cascade gives way to the
// next one in a growing share of the pixels: a dithered transition. Towards the camera, not away from it as the
// port's last form does: a cascade's map is the square around a sphere through its slice's far corners (its first
// fit's radius), so past the far split it covers the middle of the picture but not its sides (at 25 m with a 70
// degree view, 1.25 times the split is already outside at the edges, where the atlas's next tile would be read); the
// next, much larger cascade covers the band before the split everywhere. z and w (the range and the fade) are left
// as they are. Off: nothing is hooked; the menu switches between on and high at once,
// and to off (the game's shader again) while the hook is in.
//
// Softness and TAA's tap rotation (this patch's own key):
//   shadow_softness = 0.75 | console | 1.25 | 1.5   (console)   SS_SHADOW_SOFTNESS
// The readers take 8 Poisson taps (ShadowMap_Poisson) in the INTZ atlas, compare each with the receiver's depth and
// average: 9 shades. The taps are turned per pixel by an angle from ShadowMap_texRotate (s5) read at the screen
// position times the depth, range-reduced and put through one sincos, and scaled by ShadowMap_FilterScale of the
// cascade: 1.56 / 1024 x (1, 2/3, 1/3) in the race (the tweak 0xD4786C over the map's 1024 texels). The same variant
// reads that register times live.x and, before that sincos, turns the angle by live.y - 0.5 of a full turn
// (frac(angle / 2 pi + live.y) * 2 pi - pi), where live is a free float constant the hook sets every time it binds
// the variant. live.y moves only in frames TAA draws jittered (postfx.cpp), so TAA's history sees the 8 taps at many
// angles; elsewhere it is 0.5, the game's angle. The depth bias (in the cascade matrices) is not scaled. In the
// race's 18 readers: 12 deferred and forward ones with the rotation (one sincos each, after the s5 read), 4 forward
// ones with the radius and no rotation, 2 with the splits only.
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "menu.h"
#include "ss_shared.h"

namespace {

constexpr uintptr_t kRenderer = 0xD64708;  // renderer; the device is renderer+0x578 (as patch.cpp)
constexpr uint32_t kRendererDevice = 0x578;
constexpr int kCreatePixelShaderSlot = 106, kSetPixelShaderSlot = 107;
constexpr float kBands[3] = {0.0f, 0.25f, 0.4f};  // off, on, high: the share of each split (the port's values)

volatile int g_level = 0;  // 0 off, 1 on, 2 high
volatile float g_softness = 1.0f;  // shadow_softness: the filter radius's factor (1 = console)
bool g_rotate = false;              // aa = taa / taa_high at start (and SS_SHADOW_ROTATE not 0): TAA's per-frame tap rotation
bool g_said_rotation = false;
int g_ab_n = 0, g_ab_phase = -1;  // test aid SS_SHADOW_AB_AT (seconds after the hook's first call)
DWORD g_ab_at[2] = {}, g_ab_t0 = 0;
IDirect3DDevice9* g_dev = nullptr;
using SetPixelShaderFn = HRESULT(__stdcall*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
using CreatePixelShaderFn = HRESULT(__stdcall*)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
SetPixelShaderFn g_set_ps = nullptr;
CreatePixelShaderFn g_create_ps = nullptr;

// ------------------------------------------------------------------ shader bytecode (shader model 3)

constexpr uint32_t kTemp = 0x80000000, kConst = 0xA0000000, kMisc = 0x90001000;  // register parameter tokens
constexpr uint32_t kX = 0, kY = 1, kZ = 2, kW = 3;
constexpr uint32_t Dst(uint32_t reg, uint32_t mask) { return reg | (mask << 16); }
constexpr uint32_t Src(uint32_t reg, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  return reg | ((a | (b << 2) | (c << 4) | (d << 6)) << 16);
}
constexpr uint32_t Op(uint32_t code, uint32_t params) { return code | (params << 24); }
constexpr uint32_t kMov = 1, kAdd = 2, kMad = 4, kMul = 5, kFrc = 0x13, kDcl = 0x1F, kDef = 0x51, kDefi = 0x30, kDefb = 0x2F;
constexpr uint32_t kSincos = 0x25, kTexld = 0x42, kTexldl = 0x5F;
constexpr uint32_t kCtab = 0x42415443;  // 'CTAB'

uint32_t RegType(uint32_t r) { return ((r >> 28) & 7) | ((r >> 8) & 0x18); }
uint32_t Bits(float f) {
  uint32_t v;
  memcpy(&v, &f, 4);
  return v;
}

// The register a parameter of the shader's constant table (the bytecode's CTAB) is bound to, or -1. set: 2 a float
// constant, 3 a sampler.
int CtabRegister(const std::vector<uint32_t>& code, const char* want, uint16_t want_set) {
  const size_t want_len = strlen(want) + 1;
  for (size_t i = 1; i < code.size();) {
    const uint32_t op = code[i];
    if (op == 0x0000FFFF) break;
    if ((op & 0xFFFF) != 0xFFFE) {
      i += 1 + ((op >> 24) & 0xF);
      continue;
    }
    const size_t words = (op >> 16) & 0x7FFF;
    if (words >= 8 && i + 1 + words <= code.size() && code[i + 1] == kCtab) {
      const uint8_t* t = reinterpret_cast<const uint8_t*>(&code[i + 2]);
      const size_t size = (words - 1) * 4;
      uint32_t count, info;
      memcpy(&count, t + 12, 4);
      memcpy(&info, t + 16, 4);
      for (uint32_t k = 0; k < count && info + (k + 1) * 20 <= size; ++k) {
        const uint8_t* c = t + info + k * 20;
        uint32_t name;
        uint16_t set, index;
        memcpy(&name, c, 4);
        memcpy(&set, c + 4, 2);
        memcpy(&index, c + 6, 2);
        if (set == want_set && name + want_len <= size && memcmp(t + name, want, want_len) == 0) return index;
      }
      return -1;
    }
    i += 1 + words;
  }
  return -1;
}

// What a shader that reads the sun shadow names: the splits, the filter radius per cascade, the rotation noise.
struct Regs {
  int split = -1;   // ShadowMap_SplitDists (float constant)
  int filter = -1;  // ShadowMap_FilterScale (float constant)
  int rotate = -1;  // ShadowMap_texRotate (sampler)
};

Regs ShadowRegs(const std::vector<uint32_t>& code) {
  Regs r;
  r.split = CtabRegister(code, "ShadowMap_SplitDists", 2);
  r.filter = CtabRegister(code, "ShadowMap_FilterScale", 2);
  r.rotate = CtabRegister(code, "ShadowMap_texRotate", 3);
  return r;
}

// A variant: what it carries and the free constant it reads at run time (-1: none; the hook sets it at every bind).
struct Made {
  std::vector<uint32_t> code;  // empty: no variant (refused, or nothing to change)
  bool refused = false;        // the shader could not take it (else nothing applied to it)
  int band_from = -1, filter_from = -1, sincos_at = -1, live = -1;
};

// The variant. band >= 0: the splits read from a temporary with x and y times 1 - band * noise (shadow smoothing).
// ext: the filter radius read times live.x (shadow_softness) and the taps' rotation angle turned by live.y - 0.5 of a
// full turn before its sincos (TAA's per-frame rotation; live.y = 0.5 leaves it as it is). Refused when the shader
// cannot take it: not ps_3_0, too few free temporaries or constants, a float constant read with relative addressing.
Made MakeVariant(const std::vector<uint32_t>& code, const Regs& regs, float band, bool ext) {
  Made m;
  const bool want_band = band >= 0.0f && regs.split >= 0 && regs.split < 224;
  const bool want_filter = ext && regs.filter >= 0 && regs.filter < 224;
  const bool want_rot = ext && regs.rotate >= 0 && regs.rotate < 16;
  if (!want_band && !want_filter && !want_rot) return m;
  m.refused = true;
  if (code.size() < 2 || code[0] != 0xFFFF0300) return m;
  bool temp_used[32] = {}, const_used[224] = {}, vpos = false, ended = false, rotate_read = false;
  size_t at = 0;  // the first instruction after the declarations and definitions
  size_t sincos = 0;  // the sincos that turns the rotation noise into the taps' sine and cosine
  for (size_t i = 1; i < code.size();) {
    const uint32_t op = code[i];
    if (op == 0x0000FFFF) {
      ended = true;
      break;
    }
    if ((op & 0xFFFF) == 0xFFFE) {
      i += 1 + ((op >> 16) & 0x7FFF);
      continue;
    }
    const uint32_t c = op & 0xFFFF, len = (op >> 24) & 0xF;
    if (!at && c != kDcl && c != kDef && c != kDefi && c != kDefb) at = i;
    if (i + len >= code.size()) return m;
    if (c == kDef) {
      if (RegType(code[i + 1]) == 2 && (code[i + 1] & 0x7FF) < 224) const_used[code[i + 1] & 0x7FF] = true;
    } else if (c == kDcl) {
      if (len >= 2 && RegType(code[i + 2]) == 17 && (code[i + 2] & 0x7FF) == 0) vpos = true;
    } else if (c != kDefi && c != kDefb) {
      // texld / texldl from the rotation sampler (its third token), then the first sincos with one plain source
      if ((c == kTexld || c == kTexldl) && len >= 3 && RegType(code[i + 3]) == 10 &&
          (code[i + 3] & 0x7FF) == static_cast<uint32_t>(regs.rotate))
        rotate_read = true;
      if (c == kSincos && rotate_read && !sincos && len == 2 && !(code[i + 2] & 0x2000)) sincos = i;
      for (uint32_t k = 1; k <= len; ++k) {
        const uint32_t r = code[i + k];
        if (!(r & 0x80000000)) continue;
        const uint32_t type = RegType(r), num = r & 0x7FF;
        if (type == 2 && num < 224) {
          const_used[num] = true;
          if (r & 0x2000) return m;  // relative addressing of a float constant: left alone
        }
        if (type == 0 && num < 32) temp_used[num] = true;
      }
    }
    i += 1 + len;
  }
  if (!ended || !at) return m;
  if (want_rot && !sincos) return m;  // the rotation is not where the known readers have it
  // Free registers, from the top.
  int temps[3] = {-1, -1, -1}, consts[4] = {-1, -1, -1, -1};
  int nt = 0, nc = 0;
  const int need_t = int(want_band) + int(want_filter) + int(want_rot);
  const int need_c = (want_band ? 2 : 0) + (want_rot ? 1 : 0) + (want_filter || want_rot ? 1 : 0);
  for (int i = 31; i >= 0 && nt < need_t; --i)
    if (!temp_used[i]) temps[nt++] = i;
  for (int i = 223; i >= 0 && nc < need_c; --i)
    if (!const_used[i]) consts[nc++] = i;
  if (nt < need_t || nc < need_c) return m;
  int ti = 0, ci = 0;
  const int t_band = want_band ? temps[ti++] : -1, t_filter = want_filter ? temps[ti++] : -1, t_rot = want_rot ? temps[ti++] : -1;
  const int k0 = want_band ? consts[ci++] : -1, k1 = want_band ? consts[ci++] : -1;
  const int kr = want_rot ? consts[ci++] : -1, live = want_filter || want_rot ? consts[ci++] : -1;

  // The definitions and the declaration first, then the instructions.
  std::vector<uint32_t> add;
  if (want_band) {
    add.insert(add.end(), {
        Op(kDef, 5), Dst(kConst | static_cast<uint32_t>(k0), 15), Bits(0.06711056f), Bits(0.00583715f), Bits(52.9829189f), Bits(-band),
        Op(kDef, 5), Dst(kConst | static_cast<uint32_t>(k1), 15), Bits(1.0f), 0, 0, 0,
    });
    if (!vpos) add.insert(add.end(), {Op(kDcl, 2), 0x80000000, Dst(kMisc, 3)});
  }
  if (want_rot)
    add.insert(add.end(), {Op(kDef, 5), Dst(kConst | static_cast<uint32_t>(kr), 15), Bits(0.159154943f), Bits(6.28318548f),
                           Bits(-3.14159274f), 0});
  if (want_band) {
    const uint32_t s = kTemp | static_cast<uint32_t>(t_band), c0 = kConst | static_cast<uint32_t>(k0),
                   c1 = kConst | static_cast<uint32_t>(k1), sp = kConst | static_cast<uint32_t>(regs.split), pos = kMisc;
    add.insert(add.end(), {
        // s.x = frac(52.98 * frac(dot(vPos.xy, (0.0671, 0.00584)))): the interleaved gradient noise, 0 .. 1
        Op(kMul, 3), Dst(s, 3), Src(pos, kX, kY, kY, kY), Src(c0, kX, kY, kY, kY),
        Op(kAdd, 3), Dst(s, 1), Src(s, kX, kX, kX, kX), Src(s, kY, kY, kY, kY),
        Op(kFrc, 2), Dst(s, 1), Src(s, kX, kX, kX, kX),
        Op(kMul, 3), Dst(s, 1), Src(s, kX, kX, kX, kX), Src(c0, kZ, kZ, kZ, kZ),
        Op(kFrc, 2), Dst(s, 1), Src(s, kX, kX, kX, kX),
        // s.x = 1 - band * noise; s.xy = the first two splits times it; s.zw the range and the fade as they are
        Op(kMad, 4), Dst(s, 1), Src(s, kX, kX, kX, kX), Src(c0, kW, kW, kW, kW), Src(c1, kX, kX, kX, kX),
        Op(kMul, 3), Dst(s, 3), Src(sp, kX, kY, kY, kY), Src(s, kX, kX, kX, kX),
        Op(kMov, 2), Dst(s, 12), Src(sp, kX, kY, kZ, kW),
    });
  }
  if (want_filter) {  // f = the filter radius of each cascade times live.x
    const uint32_t f = kTemp | static_cast<uint32_t>(t_filter), fc = kConst | static_cast<uint32_t>(regs.filter),
                   lv = kConst | static_cast<uint32_t>(live);
    add.insert(add.end(), {Op(kMul, 3), Dst(f, 15), Src(fc, kX, kY, kZ, kW), Src(lv, kX, kX, kX, kX)});
  }
  m.code.assign(code.begin(), code.begin() + static_cast<ptrdiff_t>(at));
  m.code.insert(m.code.end(), add.begin(), add.end());
  // The shader's own instructions: every read of the splits' or the filter's register made a read of its temporary
  // (its swizzle and modifier kept); before the rotation's sincos, its angle (-pi .. pi) turned by live.y - 0.5:
  //   r.x = frac(angle / 2 pi + live.y) * 2 pi - pi;  sincos dst, r.x
  for (size_t i = at; i < code.size();) {
    const uint32_t op = code[i];
    if (op == 0x0000FFFF) {
      m.code.push_back(op);
      break;
    }
    const uint32_t c = op & 0xFFFF;
    const size_t n = c == 0xFFFE ? ((op >> 16) & 0x7FFF) : ((op >> 24) & 0xF);
    const bool regs_here = c != 0xFFFE && c != kDef && c != kDefi && c != kDefb && c != kDcl;
    if (want_rot && i == sincos) {
      const uint32_t r = kTemp | static_cast<uint32_t>(t_rot), k = kConst | static_cast<uint32_t>(kr),
                     lv = kConst | static_cast<uint32_t>(live);
      m.code.insert(m.code.end(), {
          Op(kMad, 4), Dst(r, 1), code[i + 2], Src(k, kX, kX, kX, kX), Src(lv, kY, kY, kY, kY),
          Op(kFrc, 2), Dst(r, 1), Src(r, kX, kX, kX, kX),
          Op(kMad, 4), Dst(r, 1), Src(r, kX, kX, kX, kX), Src(k, kY, kY, kY, kY), Src(k, kZ, kZ, kZ, kZ),
          op, code[i + 1], Src(r, kX, kX, kX, kX),
      });
      i += 1 + n;
      continue;
    }
    m.code.push_back(op);
    for (size_t k = 1; k <= n; ++k) {
      uint32_t r = code[i + k];
      if (regs_here && (r & 0x80000000) && RegType(r) == 2) {
        const uint32_t num = r & 0x7FF;
        if (want_band && num == static_cast<uint32_t>(regs.split))
          r = 0x80000000 | (r & 0x0FFF0000) | static_cast<uint32_t>(t_band);
        else if (want_filter && num == static_cast<uint32_t>(regs.filter))
          r = 0x80000000 | (r & 0x0FFF0000) | static_cast<uint32_t>(t_filter);
      }
      m.code.push_back(r);
    }
    i += 1 + n;
  }
  m.refused = false;
  m.band_from = want_band ? regs.split : -1;
  m.filter_from = want_filter ? regs.filter : -1;
  m.sincos_at = want_rot ? static_cast<int>(sincos) : -1;
  m.live = live;
  return m;
}

// Per game shader: what it reads, and its variants per smoothing level (0 off, 1 on, 2 high) and with or without the
// softness / rotation part (made on first use).
struct Entry {
  Regs regs;
  IDirect3DPixelShader9* variant[3][2] = {};
  int live[3][2] = {{-1, -1}, {-1, -1}, {-1, -1}};
  bool tried[3][2] = {};
};
std::unordered_map<IDirect3DPixelShader9*, Entry> g_shaders;
std::unordered_map<IDirect3DPixelShader9*, IDirect3DPixelShader9*> g_ours;  // our variants (passed through as they are) -> the game's shader
DWORD g_thread = 0;
unsigned g_made = 0, g_refused = 0;

std::vector<uint32_t> Code(IDirect3DPixelShader9* ps) {
  std::vector<uint32_t> code;
  UINT size = 0;
  if (SUCCEEDED(ps->GetFunction(nullptr, &size)) && size >= 8 && size < (1u << 20)) {
    code.resize(size / 4);
    if (FAILED(ps->GetFunction(code.data(), &size))) code.clear();
  }
  return code;
}

IDirect3DPixelShader9* VariantOf(IDirect3DDevice9* d, IDirect3DPixelShader9* ps, int level, int ext, int* live) {
  auto it = g_shaders.find(ps);
  if (it == g_shaders.end()) {
    Entry e;
    e.regs = ShadowRegs(Code(ps));
    ps->AddRef();  // held, so the address stays this shader's
    it = g_shaders.emplace(ps, e).first;
  }
  Entry& e = it->second;
  if (e.regs.split < 0 && e.regs.filter < 0 && e.regs.rotate < 0) return nullptr;
  if (e.regs.filter < 0 && e.regs.rotate < 0) ext = 0;  // nothing of the softness / rotation part to carry
  if (!e.tried[level][ext]) {
    e.tried[level][ext] = true;
    const std::vector<uint32_t> code = Code(ps);
    const Made v = MakeVariant(code, e.regs, level > 0 ? kBands[level] : -1.0f, ext != 0);
    IDirect3DPixelShader9* made = nullptr;
    if (v.code.empty() || FAILED(g_create_ps(d, reinterpret_cast<const DWORD*>(v.code.data()), &made))) made = nullptr;
    e.variant[level][ext] = made;
    e.live[level][ext] = made ? v.live : -1;
    if (made) {
      g_ours[made] = ps;
      ++ss_shader_map_gen;  // hdr.cpp's cache of Original() is made again
      ss_pipewarm_derived(made, ps, 2);
      if (++g_made <= 60)
        ss_log("[shadow] variant %u: splits from c%d (band %.2f), filter radius from c%d, tap rotation %s; run-time constant c%d "
               "(%u -> %u tokens)",
               g_made, v.band_from, level > 0 ? kBands[level] : 0.0f, v.filter_from,
               v.sincos_at >= 0 ? ("at the sincos, token " + std::to_string(v.sincos_at)).c_str() : "none", v.live,
               static_cast<unsigned>(code.size()), static_cast<unsigned>(v.code.size()));
    } else if (v.refused && ++g_refused <= 8) {
      ss_log("[shadow] a shader reading the sun shadow (splits c%d, filter c%d, rotation s%d) took no variant (%u tokens): "
             "drawn as the game has it",
             e.regs.split, e.regs.filter, e.regs.rotate, static_cast<unsigned>(code.size()));
    }
  }
  *live = e.live[level][ext];
  return e.variant[level][ext];
}

HRESULT __attribute__((stdcall, force_align_arg_pointer)) SetPixelShaderHook(IDirect3DDevice9* d, IDirect3DPixelShader9* ps) {
  int level = g_level;
  if (level < 0 || level > 2) level = 0;
  float scale = g_softness;
  bool rotate = g_rotate;
  if (g_ab_n) {  // test aid SS_SHADOW_AB_AT="t1,t2": the console filter without rotation, then rotation, then both
    if (!g_ab_t0) g_ab_t0 = GetTickCount();
    const DWORD sec = (GetTickCount() - g_ab_t0) / 1000;
    const int phase = sec >= g_ab_at[1] ? 2 : sec >= g_ab_at[0] ? 1 : 0;
    if (phase != g_ab_phase) {
      g_ab_phase = phase;
      ss_log("[shadow] test aid: phase %d at %lu s (%s)", phase, static_cast<unsigned long>(sec),
             phase == 0 ? "console radius, no rotation" : phase == 1 ? "console radius, TAA rotation" : "configured radius, TAA rotation");
    }
    if (phase < 2) scale = 1.0f;
    if (phase < 1) rotate = false;
  }
  // TAA's per-frame rotation: only in a frame TAA's resolve takes jittered (it averages the frames); never without TAA,
  // where a new pattern every frame would shimmer.
  const int frame = rotate ? ss_postfx_taa_frame() : -1;
  const int ext = scale != 1.0f || frame >= 0;
  if (!ps || (level == 0 && !ext) || d != g_dev || ss_own_drawing != 0) return g_set_ps(d, ps);
  const DWORD tid = GetCurrentThreadId();
  if (!g_thread) g_thread = tid;
  if (tid != g_thread || g_ours.count(ps)) return g_set_ps(d, ps);
  int live = -1;
  IDirect3DPixelShader9* v = VariantOf(d, ps, level, ext, &live);
  const HRESULT hr = g_set_ps(d, v ? v : ps);
  if (v && live >= 0) {
    // x: the filter radius's factor; y: 0.5 + this frame's turn of the taps (a golden-ratio sequence: every frame a
    // new angle, evenly spread over any run of frames), 0.5 = the game's angle.
    float turn = 0.0f;
    if (frame >= 0) {
      const double t = static_cast<double>(static_cast<uint32_t>(frame)) * 0.6180339887498949;
      turn = static_cast<float>(t - static_cast<double>(static_cast<uint64_t>(t)));
      if (!g_said_rotation) {
        g_said_rotation = true;
        ss_log("[shadow] TAA: the shadow taps' rotation turns every jittered frame (first turn %.3f)", turn);
      }
    }
    const float c[4] = {scale, 0.5f + turn, 0.0f, 0.0f};
    d->SetPixelShaderConstantF(static_cast<UINT>(live), c, 1);
  }
  return hr;
}

bool Readable(uintptr_t p, size_t n) {
  MEMORY_BASIC_INFORMATION mbi;
  if (p < 0x10000 || !SS_VQ(reinterpret_cast<void*>(p), &mbi, sizeof mbi)) return false;
  if (mbi.State != MEM_COMMIT || mbi.Protect == 0 || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
  return p + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

bool HookDevice() {
  if (!Readable(kRenderer, 4)) return false;
  const uintptr_t r = *reinterpret_cast<const volatile uint32_t*>(kRenderer);
  if (!r || !Readable(r + kRendererDevice, 4)) return false;
  const uintptr_t dev = *reinterpret_cast<const volatile uint32_t*>(r + kRendererDevice);
  if (!dev || !Readable(dev, 4)) return false;
  uint32_t* vt = *reinterpret_cast<uint32_t**>(dev);
  if (!vt || !Readable(reinterpret_cast<uintptr_t>(vt), (kSetPixelShaderSlot + 1) * 4)) return false;
  HMODULE owner = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<const char*>(vt[kCreatePixelShaderSlot]), &owner) ||
      !owner || owner == GetModuleHandleA(nullptr)) {
    ss_log("[shadow] the device's CreatePixelShader is not in a d3d9 module; shadow smoothing stays off");
    return true;
  }
  g_create_ps = reinterpret_cast<CreatePixelShaderFn>(vt[kCreatePixelShaderSlot]);
  g_set_ps = reinterpret_cast<SetPixelShaderFn>(vt[kSetPixelShaderSlot]);
  g_dev = reinterpret_cast<IDirect3DDevice9*>(dev);
  // The slot's word is swapped under ss_write_word's lock and what was there comes back: another part's worker may
  // have put its own hook in since the read above (deband.cpp, shadowsmooth.cpp and lodfade.cpp hook this slot from
  // their own threads at about the same time); the hook calls what the swap gave back, so neither drops the other.
  uintptr_t prev = 0;
  const bool ok = ss_write_word(&vt[kSetPixelShaderSlot], reinterpret_cast<uintptr_t>(&SetPixelShaderHook), &prev);
  if (ok && prev) g_set_ps = reinterpret_cast<SetPixelShaderFn>(prev);
  ss_log("[shadow] smoothing: device %p, vtable %p: SetPixelShader %s; band %.2f of each split, filter radius x%.2f",
         reinterpret_cast<void*>(dev), vt, ok ? "hooked" : "NOT hooked (error)", kBands[g_level], g_softness);
  if (!ok) g_dev = nullptr;
  return true;
}

DWORD WINAPI Worker(LPVOID) {
  // After the other parts' slots of the same vtable (patch.cpp's present, quality.cpp, lodfade.cpp): their workers
  // wait about as long; the hooks chain whatever the order.
  Sleep(5000);
  while (!HookDevice()) Sleep(200);
  return 0;
}

int Parse(const std::string& v) {
  if (v == "off" || v == "false" || v == "0") return 0;
  if (v == "on" || v == "true" || v == "1") return 1;
  return 2;  // high, and the default (a missing key included)
}

float ParseSoftness(const std::string& v) {
  if (v == "0.75") return 0.75f;
  if (v == "1.25") return 1.25f;
  if (v == "1.5") return 1.5f;
  return 1.0f;  // console, and the default
}

}  // namespace

void ss_shadowsmooth_start() {
  if (!ss_conf_present() && !getenv("SS_SHADOW_SMOOTHING") && !getenv("SS_SHADOW_SOFTNESS")) return;  // no conf file: the DLL does nothing
  const char* e = getenv("SS_SHADOW_SMOOTHING");
  g_level = Parse(e ? std::string(e) : ss_conf("shadow_smoothing"));
  const char* sf = getenv("SS_SHADOW_SOFTNESS");
  g_softness = ParseSoftness(sf ? std::string(sf) : ss_conf("shadow_softness"));
  const char* a = getenv("SS_AA");
  const std::string aa = a ? std::string(a) : ss_conf("aa");
  const char* r = getenv("SS_SHADOW_ROTATE");  // a test aid: 0 = no per-frame rotation under TAA (A / B)
  g_rotate = (aa == "taa" || aa == "taa_high") && !(r && *r == '0');
  ss_log("[shadow] shadow_smoothing %s, shadow_softness x%.2f, TAA tap rotation %s",
         g_level == 0 ? "off" : g_level == 1 ? "on (band 0.25)" : "high (band 0.4)", g_softness,
         g_rotate ? "on (in jittered frames)" : (aa == "taa" || aa == "taa_high") ? "off (SS_SHADOW_ROTATE=0)" : "off (no TAA)");
  if (const char* ab = getenv("SS_SHADOW_AB_AT")) {
    unsigned a = 0, b = 0;
    if (sscanf(ab, "%u,%u", &a, &b) == 2 && a < b) {
      g_ab_at[0] = a;
      g_ab_at[1] = b;
      g_ab_n = 2;
      ss_log("[shadow] test aid: SS_SHADOW_AB_AT %u, %u", a, b);
    }
  }
  if (g_level > 0 || g_softness != 1.0f || g_rotate) CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
}

// The settings menu: at once while the hook is in (smoothing on or high, a softness other than console, or TAA at
// start); else RESTART NEEDED. The new value takes effect the next time the game sets a shader that reads the shadow
// (every frame for the sun pass).
bool ss_shadowsmooth_apply(const char* key, const char* value) {
  if (!g_dev) return false;
  if (!strcmp(key, "shadow_smoothing")) {
    g_level = Parse(value ? value : "");
    ss_log("[shadow] smoothing from the settings menu: %s", g_level == 0 ? "off" : g_level == 1 ? "on" : "high");
    return true;
  }
  if (!strcmp(key, "shadow_softness")) {
    g_softness = ParseSoftness(value ? value : "");
    ss_log("[shadow] softness from the settings menu: x%.2f", g_softness);
    return true;
  }
  return false;
}

// hdr.cpp classes the game's shaders (the tone mappers, the grade, the self-lit ones); with a variant bound in their
// place it asks for the shader behind it. Called on the game's render thread, as the hook that fills the map.
// What SetPixelShaderHook would bind (the TAA rotation counted as on when it is configured: the race draws jittered).
void* ss_shadowsmooth_variant(void* device, void* ps) {
  auto* d = static_cast<IDirect3DDevice9*>(device);
  int level = g_level;
  if (level < 0 || level > 2) level = 0;
  const int ext = g_softness != 1.0f || g_rotate;
  if (!ps || !g_create_ps || d != g_dev || g_ours.count(static_cast<IDirect3DPixelShader9*>(ps)) || (level == 0 && !ext))
    return nullptr;
  int live = -1;
  return VariantOf(d, static_cast<IDirect3DPixelShader9*>(ps), level, ext, &live);
}

void* ss_shadowsmooth_original(void* ps) {
  auto it = g_ours.find(static_cast<IDirect3DPixelShader9*>(ps));
  return it == g_ours.end() ? ps : it->second;
}
