// Debanding: a fixed ±0.5-step noise added to the game's tone mapper and colour grade just before their 8-bit writes.
//   deband = off | on   (on)   SS_DEBAND
//
// What the title does (reverse engineering of the Steam build 385319; the shaders as the device receives them): the
// lit scene leaves float at the tone mapper (constant table names SSFilmicToneMapper_Consts, or SSToneMapper_Consts
// for the plain curve) and is cut to 8 bits there; the colour grade (constant table names
// color_cube_color_cube_texture: the 32x32x32 colour cube, then the gradient / vignette) reads those 8 bits, goes
// through the cube with linear blending and is cut to 8 bits again onto the final target. Neither has a dither step,
// so dark gradients (tunnels, night skies, fog, bloom halos, the vignette's fall-off) show steps, and the grade's
// second rounding widens them. Dithering after the grade (postfx.cpp) is too late: the steps are already there.
//
// What this does: every pixel shader the game sets whose constant table names one of those is replaced, at
// SetPixelShader (slot 107 of the device's vtable, in d3d9.dll's memory; the shadowsmooth.cpp route), by a variant made
// on first use: every write to oC0 goes to a free temporary instead, and at the end oC0.rgb = that + the step's
// (n - 0.5) times one 8-bit step, n an interleaved gradient noise of the pixel position (fixed on the screen: no
// flicker, nothing for TAA's history to fight). Alpha is copied as it is (HDR reads the scene's alpha). A value on an
// exact code stays on it (±0.5 step rounds back); a value between two codes lands on either in proportion.
//   * The tone mapper that writes linear light (mad / mul into oC0; its draw has D3DRS_SRGBWRITEENABLE on, so the
//     8-bit code is the sRGB encoding of the value) gets the step in linear terms: dL/de of the sRGB curve at the
//     pixel's value, max(2.2749 * L^(1/1.714), 1/12.92) per channel (the curve's slope above and below its linear
//     toe). The tone mapper that encodes itself (exp of 1/2.2 times the log: gamma output, sRGB write off) and the
//     grade (works on encoded values) get the plain 1/255.
//   * The cube build (color_cube_blending_*) is not touched: a dithered cube would be a fixed pattern in colour space.
//   * hdr.cpp classes the tone mapper and the grade by their shader object; with a variant bound it asks
//     ss_deband_original for the game's shader behind it (as for shadow and LOD smoothing).
// postfx.cpp's own 8-bit writes from filtered values (TAA's copy of its 16-bit history, contrast adaptive sharpening)
// add the same noise (ss_deband_on). FXAA and SMAA pass most pixels through unchanged; they are left alone, so those
// pixels are not dithered twice.
// Off: nothing is hooked (postfx adds nothing). The menu switches between on and off at once while the hook is in.
// Cost: about 20 instructions in two full-screen passes (inferred < 0.05 ms on the Deck).
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
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

volatile int g_on = 0;
IDirect3DDevice9* g_dev = nullptr;
using SetPixelShaderFn = HRESULT(__stdcall*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
using CreatePixelShaderFn = HRESULT(__stdcall*)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
SetPixelShaderFn g_set_ps = nullptr;
CreatePixelShaderFn g_create_ps = nullptr;

// ------------------------------------------------------------------ shader bytecode (shader model 3)

constexpr uint32_t kTemp = 0x80000000, kConst = 0xA0000000, kMisc = 0x90001000, kColorOut = 0x80000800;  // register tokens
constexpr uint32_t kX = 0, kY = 1, kZ = 2, kW = 3;
constexpr uint32_t Dst(uint32_t reg, uint32_t mask) { return reg | (mask << 16); }
constexpr uint32_t Src(uint32_t reg, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  return reg | ((a | (b << 2) | (c << 4) | (d << 6)) << 16);
}
constexpr uint32_t Op(uint32_t code, uint32_t params) { return code | (params << 24); }
constexpr uint32_t kMov = 1, kAdd = 2, kMad = 4, kMul = 5, kMax = 0x0B, kExp = 0x0E, kLog = 0x0F, kFrc = 0x13, kPow = 0x20,
                   kExpp = 0x4E, kCall = 0x19, kCallnz = 0x1A, kRet = 0x1C, kLabel = 0x1E, kDcl = 0x1F, kDef = 0x51,
                   kDefi = 0x30, kDefb = 0x2F;
constexpr uint32_t kCtab = 0x42415443;  // 'CTAB'

uint32_t RegType(uint32_t r) { return ((r >> 28) & 7) | ((r >> 8) & 0x18); }
uint32_t Bits(float f) {
  uint32_t v;
  memcpy(&v, &f, 4);
  return v;
}

enum Kind { kOther = 0, kToneMapper = 1, kGrade = 2 };

// From the constant table: the tone mapper, the grade, or neither.
Kind Classify(const std::vector<uint32_t>& code) {
  for (size_t i = 1; i < code.size();) {
    const uint32_t op = code[i];
    if (op == 0x0000FFFF) break;
    if ((op & 0xFFFF) != 0xFFFE) break;  // comments come first
    const size_t words = (op >> 16) & 0x7FFF;
    if (words >= 8 && i + 1 + words <= code.size() && code[i + 1] == kCtab) {
      const uint8_t* t = reinterpret_cast<const uint8_t*>(&code[i + 2]);
      const size_t size = (words - 1) * 4;
      uint32_t count, info;
      memcpy(&count, t + 12, 4);
      memcpy(&info, t + 16, 4);
      Kind kind = kOther;
      for (uint32_t k = 0; k < count && info + (k + 1) * 20 <= size; ++k) {
        uint32_t name;
        memcpy(&name, t + info + k * 20, 4);
        if (name >= size || !memchr(t + name, 0, size - name)) continue;
        const char* s = reinterpret_cast<const char*>(t + name);
        if (!strcmp(s, "SSFilmicToneMapper_Consts") || !strcmp(s, "SSToneMapper_Consts")) return kToneMapper;
        if (!strcmp(s, "color_cube_color_cube_texture")) kind = kGrade;
      }
      return kind;
    }
    i += 1 + words;
  }
  return kOther;
}

// The variant: oC0 written through a temporary, then oC0.rgb = it + (n - 0.5) * step, oC0.a = its alpha. `linear`:
// the step is taken through the sRGB curve's slope (a tone mapper whose draw writes with sRGB encoding) unless the
// shader encodes its output itself (an exp / pow writes oC0). Empty when the shader cannot take it (not ps_3_0, no two
// temporaries or two constants to spare, subroutines, oC0 not written or written with relative addressing).
std::vector<uint32_t> DitherVariant(const std::vector<uint32_t>& code, bool linear, bool* encoded_out) {
  std::vector<uint32_t> out;
  if (code.size() < 2 || code[0] != 0xFFFF0300) return out;
  bool temp_used[32] = {}, const_used[224] = {}, vpos = false, ended = false, encoded = false;
  uint32_t written = 0;  // the oC0 components the shader writes
  size_t at = 0;         // the first instruction after the declarations and definitions
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
    if (i + len >= code.size()) return out;
    if (c == kCall || c == kCallnz || c == kRet || c == kLabel) return out;  // subroutines: the end is not one place
    if (c == kDef) {
      if (RegType(code[i + 1]) == 2 && (code[i + 1] & 0x7FF) < 224) const_used[code[i + 1] & 0x7FF] = true;
    } else if (c == kDcl) {
      if (len >= 2 && RegType(code[i + 2]) == 17 && (code[i + 2] & 0x7FF) == 0) vpos = true;
    } else if (c != kDefi && c != kDefb) {
      for (uint32_t k = 1; k <= len; ++k) {
        const uint32_t r = code[i + k];
        if (!(r & 0x80000000)) continue;
        const uint32_t type = RegType(r), num = r & 0x7FF;
        if (type == 2 && num < 224) const_used[num] = true;
        if (type == 0 && num < 32) temp_used[num] = true;
        if (type == 8 && num == 0) {
          if (k != 1 || (r & 0x2000)) return out;  // oC0 only ever a plain destination
          written |= (r >> 16) & 0xF;
          if (c == kExp || c == kExpp || c == kPow) encoded = true;
        }
      }
    }
    i += 1 + len;
  }
  if (!ended || !at || !(written & 7)) return out;
  int t = -1, u = -1, k0 = -1, k1 = -1;
  for (int i = 31; i >= 0 && u < 0; --i)
    if (!temp_used[i]) (t < 0 ? t : u) = i;
  for (int i = 223; i >= 0 && k1 < 0; --i)
    if (!const_used[i]) (k0 < 0 ? k0 : k1) = i;
  if (t < 0 || u < 0 || k0 < 0 || k1 < 0) return out;
  linear = linear && !encoded;
  *encoded_out = !linear;
  const uint32_t T = kTemp | static_cast<uint32_t>(t), U = kTemp | static_cast<uint32_t>(u),
                 c0 = kConst | static_cast<uint32_t>(k0), c1 = kConst | static_cast<uint32_t>(k1), pos = kMisc;
  std::vector<uint32_t> head = {
      Op(kDef, 5), Dst(c0, 15), Bits(0.06711056f), Bits(0.00583715f), Bits(52.9829189f), Bits(1.0f / 255.0f),
      Op(kDef, 5), Dst(c1, 15), Bits(-0.5f / 255.0f), Bits(1.0f / 1.714286f), Bits(2.2749f), Bits(1.0f / 12.92f),
  };
  if (!vpos) head.insert(head.end(), {Op(kDcl, 2), 0x80000000, Dst(pos, 3)});
  out.assign(code.begin(), code.begin() + static_cast<ptrdiff_t>(at));
  out.insert(out.end(), head.begin(), head.end());
  // The shader's own instructions, every write of oC0 a write of T (its mask and modifiers kept).
  for (size_t i = at; i < code.size();) {
    const uint32_t op = code[i];
    if (op == 0x0000FFFF) break;
    out.push_back(op);
    const size_t n = (op & 0xFFFF) == 0xFFFE ? ((op >> 16) & 0x7FFF) : ((op >> 24) & 0xF);
    const bool regs = (op & 0xFFFF) != 0xFFFE && (op & 0xFFFF) != kDef && (op & 0xFFFF) != kDefi && (op & 0xFFFF) != kDefb &&
                      (op & 0xFFFF) != kDcl;
    for (size_t k = 1; k <= n; ++k) {
      uint32_t r = code[i + k];
      if (regs && k == 1 && (r & 0x80000000) && RegType(r) == 8 && (r & 0x7FF) == 0) r = 0x80000000 | (r & 0x0FFF0000) | static_cast<uint32_t>(t);
      out.push_back(r);
    }
    i += 1 + n;
  }
  const uint32_t rgb = written & 7;
  out.insert(out.end(), {
      // U.x = frac(52.98 * frac(dot(vPos.xy, (0.0671, 0.00584)))): the interleaved gradient noise, 0 .. 1
      Op(kMul, 3), Dst(U, 3), Src(pos, kX, kY, kY, kY), Src(c0, kX, kY, kY, kY),
      Op(kAdd, 3), Dst(U, 1), Src(U, kX, kX, kX, kX), Src(U, kY, kY, kY, kY),
      Op(kFrc, 2), Dst(U, 1), Src(U, kX, kX, kX, kX),
      Op(kMul, 3), Dst(U, 1), Src(U, kX, kX, kX, kX), Src(c0, kZ, kZ, kZ, kZ),
      Op(kFrc, 2), Dst(U, 1), Src(U, kX, kX, kX, kX),
      // U.x = (n - 0.5) / 255
      Op(kMad, 4), Dst(U, 1), Src(U, kX, kX, kX, kX), Src(c0, kW, kW, kW, kW), Src(c1, kX, kX, kX, kX),
  });
  if (linear) {
    out.insert(out.end(), {
        // U.yzw = max(2.2749 * T.rgb ^ (1 / 1.714), 1 / 12.92): the sRGB curve's dL/de at the pixel's value
        Op(kLog, 2), Dst(U, 2), Src(T, kX, kX, kX, kX),
        Op(kLog, 2), Dst(U, 4), Src(T, kY, kY, kY, kY),
        Op(kLog, 2), Dst(U, 8), Src(T, kZ, kZ, kZ, kZ),
        Op(kMul, 3), Dst(U, 14), Src(U, kX, kY, kZ, kW), Src(c1, kY, kY, kY, kY),
        Op(kExp, 2), Dst(U, 2), Src(U, kY, kY, kY, kY),
        Op(kExp, 2), Dst(U, 4), Src(U, kZ, kZ, kZ, kZ),
        Op(kExp, 2), Dst(U, 8), Src(U, kW, kW, kW, kW),
        Op(kMul, 3), Dst(U, 14), Src(U, kX, kY, kZ, kW), Src(c1, kZ, kZ, kZ, kZ),
        Op(kMax, 3), Dst(U, 14), Src(U, kX, kY, kZ, kW), Src(c1, kW, kW, kW, kW),
        // oC0.rgb = U.yzw * U.x + T.rgb
        Op(kMad, 4), Dst(kColorOut, rgb), Src(U, kY, kZ, kW, kW), Src(U, kX, kX, kX, kX), Src(T, kX, kY, kZ, kW),
    });
  } else {
    out.insert(out.end(), {Op(kAdd, 3), Dst(kColorOut, rgb), Src(T, kX, kY, kZ, kW), Src(U, kX, kX, kX, kX)});
  }
  if (written & 8) out.insert(out.end(), {Op(kMov, 2), Dst(kColorOut, 8), Src(T, kX, kY, kZ, kW)});
  out.push_back(0x0000FFFF);
  return out;
}

// Per game shader: its kind and its variant (made on first use).
struct Entry {
  Kind kind = kOther;
  IDirect3DPixelShader9* variant = nullptr;
  bool tried = false;
};
std::unordered_map<IDirect3DPixelShader9*, Entry> g_shaders;
std::unordered_map<IDirect3DPixelShader9*, IDirect3DPixelShader9*> g_ours;  // our variants -> the game's shader
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

IDirect3DPixelShader9* VariantOf(IDirect3DDevice9* d, IDirect3DPixelShader9* ps) {
  auto it = g_shaders.find(ps);
  if (it == g_shaders.end()) {
    Entry e;
    e.kind = Classify(Code(ps));
    ps->AddRef();  // held, so the address stays this shader's
    it = g_shaders.emplace(ps, e).first;
  }
  Entry& e = it->second;
  if (e.kind == kOther) return nullptr;
  if (!e.tried) {
    e.tried = true;
    const std::vector<uint32_t> code = Code(ps);
    bool encoded = false;
    const std::vector<uint32_t> v = DitherVariant(code, e.kind == kToneMapper, &encoded);
    IDirect3DPixelShader9* made = nullptr;
    if (v.empty() || FAILED(g_create_ps(d, reinterpret_cast<const DWORD*>(v.data()), &made))) made = nullptr;
    e.variant = made;
    const char* what = e.kind == kToneMapper ? "a tone mapper" : "a colour grade";
    if (made) {
      g_ours[made] = ps;
      ss_pipewarm_derived(made, ps, 3);
      ++ss_shader_map_gen;  // hdr.cpp's cache of Original() is made again
      if (++g_made <= 24)
        ss_log("[deband] %s (%u tokens) took its dither variant (%u tokens, step %s; variant %u)", what, static_cast<unsigned>(code.size()),
               static_cast<unsigned>(v.size()), encoded ? "1/255 of its encoded output" : "1/255 through the sRGB slope of its linear output",
               g_made);
    } else if (++g_refused <= 8) {
      ss_log("[deband] %s (%u tokens) took no variant (%s): it writes 8 bits as before", what, static_cast<unsigned>(code.size()),
             v.empty() ? "its byte code does not fit the rewrite" : "the device refused the variant");
    }
  }
  return e.variant;
}

HRESULT __attribute__((stdcall, force_align_arg_pointer)) SetPixelShaderHook(IDirect3DDevice9* d, IDirect3DPixelShader9* ps) {
  if (!ps || !g_on || d != g_dev || ss_own_drawing != 0) return g_set_ps(d, ps);
  const DWORD tid = GetCurrentThreadId();
  if (!g_thread) g_thread = tid;
  if (tid != g_thread || g_ours.count(ps)) return g_set_ps(d, ps);
  IDirect3DPixelShader9* v = VariantOf(d, ps);
  return g_set_ps(d, v ? v : ps);
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
    ss_log("[deband] the device's CreatePixelShader is not in a d3d9 module; the game's tone mapper and grade stay as they are");
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
  ss_log("[deband] device %p, vtable %p: SetPixelShader %s", reinterpret_cast<void*>(dev), vt, ok ? "hooked" : "NOT hooked (error)");
  if (!ok) g_dev = nullptr;
  return true;
}

DWORD WINAPI Worker(LPVOID) {
  Sleep(5000);  // with the other parts' slots of the same vtable; the hooks chain whatever the order
  while (!HookDevice()) Sleep(200);
  return 0;
}

int Parse(const std::string& v) { return v == "off" || v == "false" || v == "0" ? 0 : 1; }  // on, and the default

}  // namespace

void ss_deband_start() {
  if (!ss_conf_present() && !getenv("SS_DEBAND")) return;  // no conf file: the DLL does nothing
  const char* e = getenv("SS_DEBAND");
  g_on = Parse(e ? std::string(e) : ss_conf("deband"));
  ss_log("[deband] deband %s", g_on ? "on (tone mapper, colour grade, TAA copy-out and sharpening)" : "off");
  if (g_on) CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
}

// postfx.cpp: its own 8-bit writes from filtered values add the noise too.
bool ss_deband_on() { return g_on != 0; }

// The settings menu: at once while the hook is in (on at start); else RESTART NEEDED. The game's shaders take the change
// the next time they are set (every frame).
bool ss_deband_apply(const char* key, const char* value) {
  if (strcmp(key, "deband") || !g_dev) return false;
  g_on = Parse(value ? value : "");
  ss_log("[deband] from the settings menu: %s", g_on ? "on" : "off");
  return true;
}

// hdr.cpp: the game's shader behind one of our variants, else ps. On the game's render thread, as the hook.
void* ss_deband_variant(void* device, void* ps) {
  auto* d = static_cast<IDirect3DDevice9*>(device);
  if (!ps || !g_on || !g_create_ps || d != g_dev || g_ours.count(static_cast<IDirect3DPixelShader9*>(ps))) return nullptr;
  return VariantOf(d, static_cast<IDirect3DPixelShader9*>(ps));
}

void* ss_deband_original(void* ps) {
  auto it = g_ours.find(static_cast<IDirect3DPixelShader9*>(ps));
  return it == g_ours.end() ? ps : it->second;
}
