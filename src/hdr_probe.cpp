// HDR output: the feasibility probe (docs/plan-hdr.md). Not the feature; a test aid that says what the feature can
// stand on. Off unless `hdr_probe = true` (switch SS_HDR_PROBE=1): then no thread, no hook, no device call.
//
// What it writes, into splitsecond-hdr-probe.txt beside the DLL (and a few [hdr] lines into the patch log):
//   * the device: creation flags, the back buffer and the game's final colour target (size, format), the d3d9 module;
//   * DXVK's swap chain extension (ID3D9VkExtSwapchain, queried from the implicit swap chain): whether it is there,
//     which colour spaces the surface offers, the display's description;
//   * a census of chosen frames (`hdr_probe_at = 20,45,120`, seconds after the first present): every run of draws
//     into one render target (size, format, draws) and every StretchRect; for each draw of at most two primitives
//     (the full-screen passes) the pixel shader (FNV-1a 64 of its byte code), its named constants with their values,
//     the textures on its samplers; each such shader's disassembly once (d3dx9_42's D3DXDisassembleShader).
// Two experiments on top, each off unless asked for:
//   * `hdr_probe_colorspace = hdr10 | scrgb` (+ `hdr_probe_colorspace_at = 30`): switches the swap chain's colour space
//     through the extension at that second and sets HDR10 metadata;
//   * `hdr_probe_output = true`: from then on every frame's back buffer (the finished SDR frame, UI included) is
//     re-encoded for that colour space at `hdr_probe_paper_white` nits (200): gamma 2.2 -> linear -> BT.2020 PQ, or
//     scRGB. SDR in an HDR container: the last step of the planned output pass, without the scene's extra light.
//     `force`: also when the swap chain did not take the colour space (the screen then shows PQ codes as sRGB; for
//     checking the pass by its read-back on a machine without HDR output). Each listed frame gets a read-back of
//     the back buffer: highest and mean code, what they are as PQ light, pixels per band;
//   * `hdr_probe_backbuffer = a2r10g10b10 | a16f`: display.cpp's CreateDevice / Reset wrap asks for that back buffer
//     format instead of the game's 8-bit one (a16f needs DXVK's format unlock, which this probe requests).
//
// Levers, as everywhere in this DLL: slots of the D3D9 device's vtable in the d3d9 module's memory (the four draw
// calls 81..84 and StretchRect 34; nothing else uses them here), the Present hook patch.cpp already has, and calls
// on the game's device. Nothing of the game is written, not even data.
#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "ss_shared.h"

// COM interfaces declared outside the anonymous namespace on purpose (see postfx.cpp: inside it GCC turns the calls
// into pure-virtual calls).
struct IHdrBlob {  // ID3DXBuffer
  virtual HRESULT __stdcall QueryInterface(REFIID, void**) = 0;
  virtual ULONG __stdcall AddRef() = 0;
  virtual ULONG __stdcall Release() = 0;
  virtual void* __stdcall GetBufferPointer() = 0;
  virtual DWORD __stdcall GetBufferSize() = 0;
};
struct HdrVkMetadata {  // VkHdrMetadataEXT
  uint32_t sType;       // VK_STRUCTURE_TYPE_HDR_METADATA_EXT = 1000105000
  const void* pNext;
  float red[2], green[2], blue[2], white[2];
  float maxLuminance, minLuminance, maxContentLightLevel, maxFrameAverageLightLevel;
};
struct HdrVkOutputDesc {  // D3D9VkExtOutputMetadata
  float red[2], green[2], blue[2], white[2];
  float minLuminance, maxLuminance, maxFullFrameLuminance;
};
struct IHdrVkExtSwapchain : IUnknown {  // DXVK's ID3D9VkExtSwapchain (d3d9_interfaces.h), same order since v2.3
  virtual BOOL __stdcall CheckColorSpaceSupport(uint32_t colour_space) = 0;
  virtual HRESULT __stdcall SetColorSpace(uint32_t colour_space) = 0;
  virtual HRESULT __stdcall SetHDRMetaData(const HdrVkMetadata* metadata) = 0;
  virtual HRESULT __stdcall GetCurrentOutputDesc(HdrVkOutputDesc* desc) = 0;
  virtual void __stdcall UnlockAdditionalFormats() = 0;
};
struct IHdrVkExtInterface : IUnknown {  // DXVK's ID3D9VkExtInterface (v3.0 on): the unlock, before the device exists
  virtual void __stdcall UnlockAdditionalFormats() = 0;
};

namespace {

const GUID kIidVkExtSwapchain = {0x13776e93, 0x4aa9, 0x430a, {0xa4, 0xec, 0xfe, 0x9e, 0x28, 0x11, 0x81, 0xd5}};
const GUID kIidVkExtInterface = {0x65b55086, 0xe3e3, 0x4c3e, {0xb3, 0xa0, 0x86, 0x81, 0x5c, 0xce, 0x2c, 0x4c}};
constexpr uint32_t kCsSrgb = 0, kCsScrgb = 1000104002, kCsBt2020Linear = 1000104007, kCsHdr10 = 1000104008,
                   kCsExtSrgbNonlinear = 1000104014;

constexpr uintptr_t kRenderer = 0xD64708;  // renderer holder; +0x578 the device, +4 the frame buffer set
constexpr int kStretchRectSlot = 34, kDrawSlots[4] = {81, 82, 83, 84};

bool g_probe = false;
uint32_t g_cs_want = 0;      // the colour space to switch to (0: none)
int g_cs_at = 30;            // seconds after the first present
bool g_output = false;       // re-encode the back buffer once the colour space is switched
bool g_output_force = false; // ... or from that second on whatever the swap chain said (the picture is then wrong on
                             // screen; the read-back still checks the pass)
float g_paper = 200.0f;
D3DFORMAT g_bb_format = D3DFMT_UNKNOWN;
std::vector<int> g_at;       // census times, seconds after the first present
FILE* g_out = nullptr;

inline uint32_t Rd(uintptr_t va) { return *reinterpret_cast<const volatile uint32_t*>(va); }
bool Readable(uintptr_t p, size_t n) {
  if (p < 0x10000 || p + n < p) return false;
  MEMORY_BASIC_INFORMATION mbi;
  if (!SS_VQ(reinterpret_cast<void*>(p), &mbi, sizeof mbi)) return false;
  if (mbi.State != MEM_COMMIT || mbi.Protect == 0 || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
  return reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize >= p + n;
}
IDirect3DDevice9* GameDevice() {
  const uintptr_t r = Rd(kRenderer);
  if (!r || !Readable(r + 0x578, 4)) return nullptr;
  return reinterpret_cast<IDirect3DDevice9*>(Rd(r + 0x578));
}
// The surface the frame end (0x50E400) copies to the back buffer, or presents: renderer -> set -> target -> surface.
IDirect3DSurface9* FinalSurface() {
  const uintptr_t r = Rd(kRenderer);
  if (!r || !Readable(r + 4, 4)) return nullptr;
  const uintptr_t set = Rd(r + 4);
  if (!set || !Readable(set + 0x64, 4)) return nullptr;
  const uintptr_t target = Rd(set + 0x64);
  if (!target || !Readable(target + 0x5C, 4)) return nullptr;
  return reinterpret_cast<IDirect3DSurface9*>(Rd(target + 0x5C));
}

void Out(const char* fmt, ...) {
  if (!g_out) return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_out, fmt, ap);
  va_end(ap);
  fputc('\n', g_out);
  fflush(g_out);
}

std::string Setting(const char* env, const char* key) {
  const char* e = getenv(env);
  return e ? std::string(e) : ss_conf(key);
}
bool IsOn(const std::string& v) { return v == "1" || v == "true" || v == "yes" || v == "on"; }

template <typename T>
void SafeRelease(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

// ------------------------------------------------------------------------------------------------ pixel shaders

struct PsConst {
  std::string name;
  int set, reg, count;  // set: 0 bool, 1 int, 2 float, 3 sampler
};
struct PsInfo {
  IDirect3DPixelShader9* ps;
  uint64_t hash;
  UINT size;
  std::vector<PsConst> consts;
  bool listed;
};
std::vector<PsInfo> g_ps;

HMODULE D3dx() {
  static HMODULE m = GetModuleHandleA("d3dx9_42.dll");
  return m;
}

// The constant table (the CTAB comment the compiler leaves in the byte code): names and registers.
void ParseCtab(const DWORD* code, UINT bytes, std::vector<PsConst>* out) {
  const UINT n = bytes / 4;
  for (UINT i = 1; i + 1 < n;) {
    if ((code[i] & 0xFFFF) != 0xFFFE) break;  // comments come first
    const UINT len = code[i] >> 16;
    if (i + 1 + len > n) break;
    if (len >= 8 && code[i + 1] == 0x42415443) {  // 'CTAB'
      const unsigned char* base = reinterpret_cast<const unsigned char*>(&code[i + 2]);
      const UINT size = (len - 1) * 4;
      const DWORD* h = reinterpret_cast<const DWORD*>(base);
      const UINT count = h[3], info = h[4];
      for (UINT k = 0; k < count && info + (k + 1) * 20 <= size; ++k) {
        const unsigned char* e = base + info + k * 20;
        const DWORD name = *reinterpret_cast<const DWORD*>(e);
        const WORD* w = reinterpret_cast<const WORD*>(e + 4);
        PsConst c;
        if (name < size) c.name.assign(reinterpret_cast<const char*>(base + name), strnlen(reinterpret_cast<const char*>(base + name), size - name));
        c.set = w[0];
        c.reg = w[1];
        c.count = w[2];
        out->push_back(c);
      }
      return;
    }
    i += 1 + len;
  }
}

const PsInfo* Info(IDirect3DPixelShader9* ps) {
  for (const auto& p : g_ps)
    if (p.ps == ps) return &p;
  PsInfo in = {ps, 0, 0, {}, false};
  UINT size = 0;
  if (SUCCEEDED(ps->GetFunction(nullptr, &size)) && size >= 8 && size < (1u << 20)) {
    std::vector<DWORD> code((size + 3) / 4);
    if (SUCCEEDED(ps->GetFunction(code.data(), &size))) {
      uint64_t h = 1469598103934665603ull;
      const unsigned char* b = reinterpret_cast<const unsigned char*>(code.data());
      for (UINT i = 0; i < size; ++i) h = (h ^ b[i]) * 1099511628211ull;
      in.hash = h;
      in.size = size;
      ParseCtab(code.data(), size, &in.consts);
    }
  }
  g_ps.push_back(in);
  return &g_ps.back();
}

// Once per shader: its constants and its disassembly.
void List(IDirect3DPixelShader9* ps) {
  for (auto& p : g_ps) {
    if (p.ps != ps || p.listed) continue;
    p.listed = true;
    for (const auto& q : g_ps)
      if (&q != &p && q.listed && q.hash == p.hash) return;  // the same code under another object
    Out("---- ps %016llx (%u bytes)", static_cast<unsigned long long>(p.hash), p.size);
    for (const auto& c : p.consts)
      Out("  %s %c%d (%d)", c.name.c_str(), c.set == 3 ? 's' : c.set == 2 ? 'c' : c.set == 1 ? 'i' : 'b', c.reg, c.count);
    using DisFn = HRESULT(WINAPI*)(const DWORD*, BOOL, const char*, IHdrBlob**);
    DisFn dis = D3dx() ? reinterpret_cast<DisFn>(GetProcAddress(D3dx(), "D3DXDisassembleShader")) : nullptr;
    std::vector<DWORD> code((p.size + 3) / 4);
    UINT size = p.size;
    IHdrBlob* text = nullptr;
    if (dis && SUCCEEDED(ps->GetFunction(code.data(), &size)) && SUCCEEDED(dis(code.data(), FALSE, nullptr, &text)) && text) {
      fwrite(text->GetBufferPointer(), 1, strnlen(static_cast<const char*>(text->GetBufferPointer()), text->GetBufferSize()), g_out);
      fputc('\n', g_out);
      text->Release();
    } else {
      Out("  (no disassembly)");
    }
    Out("----");
    return;
  }
}

// ------------------------------------------------------------------------------------------------------- census

bool g_census = false;  // this frame is being listed (render thread)
struct Run {
  IDirect3DSurface9* rt = nullptr;  // compared only, never used
  D3DSURFACE_DESC desc = {};
  int draws = 0, small = 0;
  uint64_t hashes[6] = {};
  int nhash = 0, more = 0;
} g_run;
int g_runs = 0, g_frame_draws = 0;
IDirect3DSurface9* g_back = nullptr;  // this frame's back buffer and final target (compared only)
IDirect3DSurface9* g_final = nullptr;

void FlushRun() {
  if (!g_run.rt) return;
  char hs[160] = "";
  int n = 0;
  for (int i = 0; i < g_run.nhash; ++i) n += snprintf(hs + n, sizeof hs - n, " %016llx", static_cast<unsigned long long>(g_run.hashes[i]));
  Out("run %3d: target %p %ux%u fmt %d%s%s: %d draws (%d of <= 2 primitives), ps%s%s", g_runs++, g_run.rt, g_run.desc.Width,
      g_run.desc.Height, static_cast<int>(g_run.desc.Format), g_run.rt == g_back ? " [back buffer]" : "",
      g_run.rt == g_final ? " [final target]" : "", g_run.draws, g_run.small, hs, g_run.more ? " ..." : "");
  g_run = Run();
}

void DescribeTexture(IDirect3DDevice9* d, int s) {
  IDirect3DBaseTexture9* t = nullptr;
  if (FAILED(d->GetTexture(s, &t)) || !t) {
    Out("    s%d: none", s);
    return;
  }
  DWORD srgb = 0, minf = 0;
  d->GetSamplerState(s, D3DSAMP_SRGBTEXTURE, &srgb);
  d->GetSamplerState(s, D3DSAMP_MINFILTER, &minf);
  if (t->GetType() == D3DRTYPE_TEXTURE) {
    D3DSURFACE_DESC desc;
    static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &desc);
    IDirect3DSurface9* level = nullptr;  // the pointer a later run's "target" shows when the texture is drawn into
    static_cast<IDirect3DTexture9*>(t)->GetSurfaceLevel(0, &level);
    Out("    s%d: %p (surface %p) %ux%u fmt %d%s levels %lu, srgb read %lu, min filter %lu", s, t, level, desc.Width, desc.Height,
        static_cast<int>(desc.Format), (desc.Usage & D3DUSAGE_RENDERTARGET) ? " target" : "", t->GetLevelCount(), srgb, minf);
    SafeRelease(level);
  } else {
    Out("    s%d: %p type %d", s, t, static_cast<int>(t->GetType()));
  }
  t->Release();
}

void NoteDraw(IDirect3DDevice9* d, UINT prims, const char* how) {
  if (!g_census || ss_own_drawing > 0 || d != GameDevice()) return;
  ++g_frame_draws;
  IDirect3DSurface9* rt = nullptr;
  if (FAILED(d->GetRenderTarget(0, &rt)) || !rt) return;
  if (rt != g_run.rt) {
    FlushRun();
    g_run.rt = rt;
    rt->GetDesc(&g_run.desc);
  }
  rt->Release();
  ++g_run.draws;
  IDirect3DPixelShader9* ps = nullptr;
  d->GetPixelShader(&ps);
  const PsInfo* in = ps ? Info(ps) : nullptr;
  const uint64_t hash = in ? in->hash : 0;
  bool seen = false;
  for (int i = 0; i < g_run.nhash; ++i) seen = seen || g_run.hashes[i] == hash;
  if (!seen) {
    if (g_run.nhash < 6) g_run.hashes[g_run.nhash++] = hash;
    else g_run.more = 1;
  }
  if (prims <= 2) {
    ++g_run.small;
    if (g_run.small <= 6) {
      DWORD srgbw = 0, blend = 0, src = 0, dst = 0, cw = 0;
      d->GetRenderState(D3DRS_SRGBWRITEENABLE, &srgbw);
      d->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
      d->GetRenderState(D3DRS_SRCBLEND, &src);
      d->GetRenderState(D3DRS_DESTBLEND, &dst);
      d->GetRenderState(D3DRS_COLORWRITEENABLE, &cw);
      D3DVIEWPORT9 vp = {};
      d->GetViewport(&vp);
      Out("  draw %d (%s, %u primitives) into %p %ux%u fmt %d, viewport %lu,%lu %lux%lu: ps %016llx, srgb write %lu, blend %lu (%lu, %lu), colour write %lx",
          g_frame_draws, how, prims, g_run.rt, g_run.desc.Width, g_run.desc.Height, static_cast<int>(g_run.desc.Format), vp.X, vp.Y,
          vp.Width, vp.Height, static_cast<unsigned long long>(hash), srgbw, blend, src, dst, cw);
      if (in) {
        bool sampler = false;
        for (const auto& c : in->consts) {
          if (c.set == 3) {
            sampler = true;
            Out("   %s:", c.name.c_str());
            for (int k = 0; k < c.count && k < 4; ++k) DescribeTexture(d, c.reg + k);
          } else if (c.set == 2) {
            float v[16] = {};
            const int regs = c.count > 4 ? 4 : c.count;
            const HRESULT hr = d->GetPixelShaderConstantF(c.reg, v, regs);
            char line[400];
            int n = snprintf(line, sizeof line, "   %s c%d:", c.name.c_str(), c.reg);
            for (int k = 0; k < regs * 4 && n < 360; ++k) n += snprintf(line + n, sizeof line - n, " %.6g", v[k]);
            if (FAILED(hr)) snprintf(line + n, sizeof line - n, " (read failed 0x%08lx)", static_cast<unsigned long>(hr));
            Out("%s", line);
          }
        }
        if (!sampler)
          for (int s = 0; s < 4; ++s) DescribeTexture(d, s);  // no table: what is bound
        List(ps);
      }
    }
  }
  if (ps) ps->Release();
}

// --------------------------------------------------------------------------------------------------------- hooks

using DrawPrimitiveFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawUpFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedUpFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
using StretchRectFn = HRESULT(__stdcall*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
DrawPrimitiveFn o_draw = nullptr;
DrawIndexedFn o_draw_indexed = nullptr;
DrawUpFn o_draw_up = nullptr;
DrawIndexedUpFn o_draw_indexed_up = nullptr;
StretchRectFn o_stretch = nullptr;

HRESULT __attribute__((stdcall, force_align_arg_pointer)) DrawHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT start, UINT prims) {
  NoteDraw(d, prims, "DrawPrimitive");
  return o_draw(d, t, start, prims);
}
HRESULT __attribute__((stdcall, force_align_arg_pointer)) DrawIndexedHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT base, UINT minv, UINT numv, UINT start, UINT prims) {
  NoteDraw(d, prims, "DrawIndexedPrimitive");
  return o_draw_indexed(d, t, base, minv, numv, start, prims);
}
HRESULT __attribute__((stdcall, force_align_arg_pointer)) DrawUpHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT prims, const void* data, UINT stride) {
  NoteDraw(d, prims, "DrawPrimitiveUP");
  return o_draw_up(d, t, prims, data, stride);
}
HRESULT __attribute__((stdcall, force_align_arg_pointer)) DrawIndexedUpHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT minv, UINT numv, UINT prims, const void* idx, D3DFORMAT fmt, const void* data, UINT stride) {
  NoteDraw(d, prims, "DrawIndexedPrimitiveUP");
  return o_draw_indexed_up(d, t, minv, numv, prims, idx, fmt, data, stride);
}
HRESULT __attribute__((stdcall, force_align_arg_pointer)) StretchRectHook(IDirect3DDevice9* d, IDirect3DSurface9* s, const RECT* sr, IDirect3DSurface9* t, const RECT* tr, D3DTEXTUREFILTERTYPE f) {
  if (g_census && ss_own_drawing == 0 && d == GameDevice() && s && t) {
    FlushRun();
    D3DSURFACE_DESC a = {}, b = {};
    s->GetDesc(&a);
    t->GetDesc(&b);
    Out("StretchRect %p %ux%u fmt %d%s -> %p %ux%u fmt %d%s, filter %d", s, a.Width, a.Height, static_cast<int>(a.Format),
        s == g_final ? " [final target]" : "", t, b.Width, b.Height, static_cast<int>(b.Format), t == g_back ? " [back buffer]" : "",
        static_cast<int>(f));
  }
  return o_stretch(d, s, sr, t, tr, f);
}

uint32_t* g_hooked_vt = nullptr;

// The five slots, in the d3d9 module's memory. The originals are taken once per vtable, before a slot points at us.
bool HookDevice() {
  IDirect3DDevice9* d = GameDevice();
  if (!d || !Readable(reinterpret_cast<uintptr_t>(d), 4)) return false;
  uint32_t* vt = *reinterpret_cast<uint32_t**>(d);
  if (vt == g_hooked_vt) return true;
  if (g_hooked_vt || !vt || !Readable(reinterpret_cast<uintptr_t>(vt), 85 * 4)) return false;  // one vtable only: a probe
  HMODULE owner = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<const char*>(vt[kDrawSlots[0]]), &owner) ||
      !owner || owner == GetModuleHandleA(nullptr)) {
    ss_log("[hdr] probe: the device's draw calls are not in a d3d9 module; no census");
    g_hooked_vt = vt;
    return true;
  }
  struct { int slot; void* mine; void** orig; } slots[] = {
      {kDrawSlots[0], reinterpret_cast<void*>(&DrawHook), reinterpret_cast<void**>(&o_draw)},
      {kDrawSlots[1], reinterpret_cast<void*>(&DrawIndexedHook), reinterpret_cast<void**>(&o_draw_indexed)},
      {kDrawSlots[2], reinterpret_cast<void*>(&DrawUpHook), reinterpret_cast<void**>(&o_draw_up)},
      {kDrawSlots[3], reinterpret_cast<void*>(&DrawIndexedUpHook), reinterpret_cast<void**>(&o_draw_indexed_up)},
      {kStretchRectSlot, reinterpret_cast<void*>(&StretchRectHook), reinterpret_cast<void**>(&o_stretch)},
  };
  for (auto& s : slots) *s.orig = reinterpret_cast<void*>(static_cast<uintptr_t>(vt[s.slot]));  // before any slot points at us
  int ok = 0;
  for (auto& s : slots) ok += ss_write_word(&vt[s.slot], reinterpret_cast<uintptr_t>(s.mine)) ? 1 : 0;
  g_hooked_vt = vt;
  char name[MAX_PATH] = "?";
  GetModuleFileNameA(owner, name, sizeof name);
  ss_log("[hdr] probe: %d of 5 device slots (draws, StretchRect) wrapped in %s", ok, name);
  return true;
}

DWORD WINAPI Thread(LPVOID) {
  while (!HookDevice()) Sleep(100);
  return 0;
}

// ---------------------------------------------------------------------------------------- the device, the swap chain

IHdrVkExtSwapchain* Extension(IDirect3DDevice9* d) {
  IDirect3DSwapChain9* sc = nullptr;
  if (FAILED(d->GetSwapChain(0, &sc)) || !sc) return nullptr;
  IHdrVkExtSwapchain* ext = nullptr;
  if (FAILED(sc->QueryInterface(kIidVkExtSwapchain, reinterpret_cast<void**>(&ext)))) ext = nullptr;
  sc->Release();
  return ext;
}

void DescribeDevice(IDirect3DDevice9* d) {
  D3DDEVICE_CREATION_PARAMETERS cp = {};
  d->GetCreationParameters(&cp);
  HMODULE owner = nullptr;
  char name[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<const char*>((*reinterpret_cast<uint32_t**>(d))[3]), &owner) && owner)  // a slot nobody wraps
    GetModuleFileNameA(owner, name, sizeof name);
  Out("device %p, behaviour flags 0x%lx%s, d3d9 module %s", d, cp.BehaviorFlags, (cp.BehaviorFlags & D3DCREATE_PUREDEVICE) ? " (pure)" : "", name);
  IDirect3DSwapChain9* sc = nullptr;
  if (SUCCEEDED(d->GetSwapChain(0, &sc)) && sc) {
    D3DPRESENT_PARAMETERS pp = {};
    sc->GetPresentParameters(&pp);
    Out("swap chain: %ux%u format %d, %u back buffers, swap effect %d, %s, interval 0x%x, multisample %d", pp.BackBufferWidth,
        pp.BackBufferHeight, static_cast<int>(pp.BackBufferFormat), pp.BackBufferCount, static_cast<int>(pp.SwapEffect),
        pp.Windowed ? "windowed" : "fullscreen", pp.PresentationInterval, static_cast<int>(pp.MultiSampleType));
    sc->Release();
  }
  IDirect3DSurface9* bb = nullptr;
  D3DSURFACE_DESC desc = {};
  if (SUCCEEDED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
    bb->GetDesc(&desc);
    Out("back buffer %p: %ux%u format %d", bb, desc.Width, desc.Height, static_cast<int>(desc.Format));
    ss_log("[hdr] probe: back buffer %ux%u format %d", desc.Width, desc.Height, static_cast<int>(desc.Format));
  }
  IDirect3DSurface9* f = FinalSurface();
  if (f && Readable(reinterpret_cast<uintptr_t>(f), 4)) {
    f->GetDesc(&desc);
    Out("final colour target %p: %ux%u format %d, usage 0x%lx%s", f, desc.Width, desc.Height, static_cast<int>(desc.Format), desc.Usage,
        f == bb ? " (the back buffer itself)" : "");
  }
  SafeRelease(bb);

  IDirect3D9* d3d = nullptr;
  if (SUCCEEDED(d->GetDirect3D(&d3d)) && d3d) {
    IHdrVkExtInterface* xi = nullptr;
    const HRESULT hr = d3d->QueryInterface(kIidVkExtInterface, reinterpret_cast<void**>(&xi));
    Out("ID3D9VkExtInterface (format unlock on the IDirect3D9, DXVK 3.0 on): %s", SUCCEEDED(hr) && xi ? "there" : "not there");
    if (SUCCEEDED(hr) && xi) xi->Release();
    for (D3DFORMAT fmt : {D3DFMT_A2R10G10B10, D3DFMT_A2B10G10R10, D3DFMT_A16B16G16R16F}) {
      const HRESULT a = d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, fmt);
      const HRESULT b = d3d->CheckDeviceType(cp.AdapterOrdinal, cp.DeviceType, D3DFMT_X8R8G8B8, fmt, TRUE);
      const HRESULT c = d3d->CheckDeviceType(cp.AdapterOrdinal, cp.DeviceType, fmt, fmt, FALSE);
      Out("format %d: render target texture 0x%08lx, back buffer windowed 0x%08lx, fullscreen (as the adapter format too) 0x%08lx",
          static_cast<int>(fmt), static_cast<unsigned long>(a), static_cast<unsigned long>(b), static_cast<unsigned long>(c));
    }
    d3d->Release();
  }

  IHdrVkExtSwapchain* ext = Extension(d);
  if (!ext) {
    Out("ID3D9VkExtSwapchain: not there (not DXVK, or older than 2.3): no HDR swap chain through this d3d9");
    ss_log("[hdr] probe: DXVK's swap chain extension is not there");
    return;
  }
  const BOOL hdr10 = ext->CheckColorSpaceSupport(kCsHdr10), scrgb = ext->CheckColorSpaceSupport(kCsScrgb);
  Out("ID3D9VkExtSwapchain: there. Colour spaces the surface takes: sRGB %d, HDR10 ST2084 %d, scRGB (extended sRGB linear) %d, "
      "BT.2020 linear %d, extended sRGB nonlinear %d",
      ext->CheckColorSpaceSupport(kCsSrgb), hdr10, scrgb, ext->CheckColorSpaceSupport(kCsBt2020Linear),
      ext->CheckColorSpaceSupport(kCsExtSrgbNonlinear));
  HdrVkOutputDesc od = {};
  const HRESULT hr = ext->GetCurrentOutputDesc(&od);
  Out("output (0x%08lx): red %.4f %.4f, green %.4f %.4f, blue %.4f %.4f, white %.4f %.4f; luminance min %.4f, max %.1f, full frame %.1f",
      static_cast<unsigned long>(hr), od.red[0], od.red[1], od.green[0], od.green[1], od.blue[0], od.blue[1], od.white[0], od.white[1],
      od.minLuminance, od.maxLuminance, od.maxFullFrameLuminance);
  ss_log("[hdr] probe: DXVK's swap chain extension is there; HDR10 %s, scRGB %s", hdr10 ? "offered" : "not offered", scrgb ? "offered" : "not offered");
  ext->Release();
}

bool g_cs_done = false, g_cs_on = false;

void SwitchColourSpace(IDirect3DDevice9* d) {
  g_cs_done = true;
  IHdrVkExtSwapchain* ext = Extension(d);
  if (!ext) return;
  const HRESULT hr = ext->SetColorSpace(g_cs_want);
  HdrVkMetadata md = {};
  md.sType = 1000105000;
  md.red[0] = 0.708f, md.red[1] = 0.292f, md.green[0] = 0.170f, md.green[1] = 0.797f, md.blue[0] = 0.131f, md.blue[1] = 0.046f;
  md.white[0] = 0.3127f, md.white[1] = 0.3290f;
  md.maxLuminance = 1000.0f, md.minLuminance = 0.0f, md.maxContentLightLevel = 1000.0f, md.maxFrameAverageLightLevel = g_paper;
  const HRESULT hm = ext->SetHDRMetaData(&md);
  ext->Release();
  g_cs_on = SUCCEEDED(hr);
  Out("SetColorSpace(%s): 0x%08lx; SetHDRMetaData (BT.2020, 1000 nits): 0x%08lx", g_cs_want == kCsHdr10 ? "HDR10 ST2084" : "scRGB",
      static_cast<unsigned long>(hr), static_cast<unsigned long>(hm));
  ss_log("[hdr] probe: swap chain colour space %s: 0x%08lx, metadata 0x%08lx", g_cs_want == kCsHdr10 ? "HDR10 ST2084" : "scRGB",
         static_cast<unsigned long>(hr), static_cast<unsigned long>(hm));
}

// ---------------------------------------------------------------------- the back buffer re-encoded (hdr_probe_output)

const char kQuadVs[] = R"(
void main(float4 p : POSITION, float2 t : TEXCOORD0, out float4 op : POSITION, out float2 ot : TEXCOORD0) { op = p; ot = t; }
)";
// c0 = (paper white / 10000 nits, 1 = scRGB instead of PQ, -, -). The last step of the port's hdr_out.frag.
const char kOutPs[] = R"(
sampler2D frame : register(s0);
float4 p : register(c0);
float3 Pq(float3 n) {
  float3 m = pow(saturate(n), 0.1593017578125);
  return pow((0.8359375 + 18.8515625 * m) / (1.0 + 18.6875 * m), 78.84375);
}
float4 main(float2 uv : TEXCOORD0) : COLOR {
  float3 l = pow(max(tex2D(frame, uv).rgb, 0.0), 2.2) * p.x;
  if (p.y > 0.5) return float4(l * 125.0, 1.0);
  float3 w = float3(dot(l, float3(0.6274, 0.3293, 0.0433)), dot(l, float3(0.0691, 0.9195, 0.0114)), dot(l, float3(0.0164, 0.0880, 0.8956)));
  return float4(Pq(w), 1.0);
}
)";

IDirect3DDevice9* g_out_dev = nullptr;
IDirect3DVertexShader9* g_out_vs = nullptr;
IDirect3DPixelShader9* g_out_ps = nullptr;
IDirect3DVertexDeclaration9* g_out_decl = nullptr;
bool g_out_failed = false;

IHdrBlob* Compile(const char* src, const char* profile) {
  using CompileFn = HRESULT(WINAPI*)(const char*, UINT, const void*, void*, const char*, const char*, DWORD, IHdrBlob**, IHdrBlob**, void**);
  CompileFn compile = D3dx() ? reinterpret_cast<CompileFn>(GetProcAddress(D3dx(), "D3DXCompileShader")) : nullptr;
  if (!compile) return nullptr;
  IHdrBlob *code = nullptr, *errors = nullptr;
  const HRESULT hr = compile(src, static_cast<UINT>(strlen(src)), nullptr, nullptr, "main", profile, 0, &code, &errors, nullptr);
  if (FAILED(hr) || !code) {
    ss_log("[hdr] probe: shader (%s) did not compile (0x%08lx): %.300s", profile, static_cast<unsigned long>(hr),
           errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
    if (code) code->Release();
    code = nullptr;
  }
  if (errors) errors->Release();
  return code;
}

// The objects of the default pool (the copy, the state block) are made and released within the frame: slow, but a
// probe needs no Reset hook of its own that way.
void Reencode(IDirect3DDevice9* d) {
  if (g_out_failed) return;
  if (g_out_dev != d) {
    g_out_dev = d;
    g_out_vs = nullptr, g_out_ps = nullptr, g_out_decl = nullptr;
    static const D3DVERTEXELEMENT9 kDecl[] = {{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                              {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
                                              D3DDECL_END()};
    IHdrBlob* v = Compile(kQuadVs, "vs_3_0");
    IHdrBlob* p = Compile(kOutPs, "ps_3_0");
    const bool ok = v && p && SUCCEEDED(d->CreateVertexShader(static_cast<const DWORD*>(v->GetBufferPointer()), &g_out_vs)) &&
                    SUCCEEDED(d->CreatePixelShader(static_cast<const DWORD*>(p->GetBufferPointer()), &g_out_ps)) &&
                    SUCCEEDED(d->CreateVertexDeclaration(kDecl, &g_out_decl));
    if (v) v->Release();
    if (p) p->Release();
    if (!ok) {
      g_out_failed = true;
      ss_log("[hdr] probe: the output pass could not be made");
      return;
    }
    ss_log("[hdr] probe: the output pass is in (paper white %.0f nits, %s)", g_paper, g_cs_want == kCsScrgb ? "scRGB" : "BT.2020 PQ");
  }
  IDirect3DSurface9 *bb = nullptr, *copy = nullptr, *rt = nullptr, *ds = nullptr;
  IDirect3DTexture9* tex = nullptr;
  IDirect3DStateBlock9* sb = nullptr;
  D3DSURFACE_DESC desc = {};
  ++ss_own_drawing;
  if (SUCCEEDED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb && SUCCEEDED(bb->GetDesc(&desc)) &&
      SUCCEEDED(d->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT, &tex, nullptr)) &&
      SUCCEEDED(tex->GetSurfaceLevel(0, &copy)) && SUCCEEDED(d->CreateStateBlock(D3DSBT_ALL, &sb)) && SUCCEEDED(sb->Capture()) &&
      SUCCEEDED(d->StretchRect(bb, nullptr, copy, nullptr, D3DTEXF_NONE)) && SUCCEEDED(d->BeginScene())) {
    d->GetRenderTarget(0, &rt);
    d->GetDepthStencilSurface(&ds);
    d->SetRenderTarget(0, bb);
    d->SetDepthStencilSurface(nullptr);
    d->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    d->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_FOGENABLE, FALSE);
    d->SetVertexDeclaration(g_out_decl);
    d->SetVertexShader(g_out_vs);
    d->SetPixelShader(g_out_ps);
    d->SetTexture(0, tex);
    d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    const float c0[4] = {g_paper / 10000.0f, g_cs_want == kCsScrgb ? 1.0f : 0.0f, 0.0f, 0.0f};
    d->SetPixelShaderConstantF(0, c0, 1);
    const float dx = -1.0f / desc.Width, dy = 1.0f / desc.Height;
    const float q[4][6] = {{-1 + dx, 1 + dy, 0, 1, 0, 0}, {1 + dx, 1 + dy, 0, 1, 1, 0}, {-1 + dx, -1 + dy, 0, 1, 0, 1}, {1 + dx, -1 + dy, 0, 1, 1, 1}};
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
    d->EndScene();
    if (rt) d->SetRenderTarget(0, rt);
    d->SetDepthStencilSurface(ds);
    sb->Apply();
  } else {
    static int said = 0;
    if (said++ < 3) ss_log("[hdr] probe: the output pass could not run this frame (back buffer format %d)", static_cast<int>(desc.Format));
  }
  --ss_own_drawing;
  SafeRelease(rt);
  SafeRelease(ds);
  SafeRelease(sb);
  SafeRelease(copy);
  SafeRelease(tex);
  SafeRelease(bb);
}

// The back buffer as the swap chain gets it, read back: the highest and the mean code, and what they are as PQ.
void Readback(IDirect3DDevice9* d) {
  IDirect3DSurface9 *bb = nullptr, *sys = nullptr;
  D3DSURFACE_DESC desc = {};
  D3DLOCKED_RECT lr = {};
  if (SUCCEEDED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb && SUCCEEDED(bb->GetDesc(&desc)) &&
      SUCCEEDED(d->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
      SUCCEEDED(d->GetRenderTargetData(bb, sys)) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
    double sum = 0, mx = 0;
    uint32_t bins[8] = {};  // pixels by their brightest channel: below 100, 200, 300, 400, 600, 1000, 2000 nits, above
    auto nits = [](double v) {
      const double p = pow(v, 1.0 / 78.84375), n = (p > 0.8359375 ? p - 0.8359375 : 0.0) / (18.8515625 - 18.6875 * p);
      return 10000.0 * pow(n, 1.0 / 0.1593017578125);
    };
    static const double kEdges[7] = {100, 200.5, 300, 400, 600, 1000, 2000};
    const bool scrgb = desc.Format == D3DFMT_A16B16G16R16F;
    for (UINT y = 0; y < desc.Height; ++y) {
      const unsigned char* row = static_cast<const unsigned char*>(lr.pBits) + y * lr.Pitch;
      for (UINT x = 0; x < desc.Width; ++x) {
        double r, g, b;
        if (desc.Format == D3DFMT_A2R10G10B10) {
          const uint32_t v = reinterpret_cast<const uint32_t*>(row)[x];
          r = ((v >> 20) & 1023) / 1023.0, g = ((v >> 10) & 1023) / 1023.0, b = (v & 1023) / 1023.0;
        } else if (scrgb) {
          // half floats, 1.0 = 80 nits: the green half's value stands for the pixel
          const uint16_t h = reinterpret_cast<const uint16_t*>(row)[x * 4 + 1];
          const int e = (h >> 10) & 31;
          r = g = b = e ? ldexp(1.0 + (h & 1023) / 1024.0, e - 15) : 0.0;
        } else {
          b = row[x * 4] / 255.0, g = row[x * 4 + 1] / 255.0, r = row[x * 4 + 2] / 255.0;
        }
        const double m = r > g ? (r > b ? r : b) : (g > b ? g : b);
        sum += m;
        if (m > mx) mx = m;
        const double n = scrgb ? m * 80.0 : nits(m);
        int k = 0;
        while (k < 7 && n >= kEdges[k]) ++k;
        ++bins[k];
      }
    }
    sys->UnlockRect();
    const double px = double(desc.Width) * desc.Height, mean = sum / px;
    if (scrgb)
      Out("read back (format %d, scRGB green): highest %.3f (%.0f nits), mean %.3f (%.0f nits)", static_cast<int>(desc.Format), mx, mx * 80.0, mean, mean * 80.0);
    else
      Out("read back (format %d): highest code %.4f (as PQ %.0f nits), mean code %.4f (as PQ %.0f nits)", static_cast<int>(desc.Format), mx, nits(mx), mean, nits(mean));
    Out("  pixels by brightest channel as light: <100 %.2f%%, <200 %.2f%%, <300 %.2f%%, <400 %.2f%%, <600 %.2f%%, <1000 %.2f%%, <2000 %.2f%%, more %.2f%%",
        100 * bins[0] / px, 100 * bins[1] / px, 100 * bins[2] / px, 100 * bins[3] / px, 100 * bins[4] / px, 100 * bins[5] / px,
        100 * bins[6] / px, 100 * bins[7] / px);
  } else {
    Out("read back: failed (back buffer format %d)", static_cast<int>(desc.Format));
  }
  SafeRelease(sys);
  SafeRelease(bb);
}

DWORD g_t0 = 0;
size_t g_next = 0;
uint32_t g_frame = 0;

}  // namespace

// patch.cpp's Present hook, before the real Present (after the overlay).
void ss_hdr_probe_present(void* device) {
  if (!g_probe || !g_out) return;
  IDirect3DDevice9* d = static_cast<IDirect3DDevice9*>(device);
  if (d != GameDevice()) return;
  ++g_frame;
  if (!g_t0) {
    g_t0 = GetTickCount();
    DescribeDevice(d);
    if (g_bb_format == D3DFMT_A16B16G16R16F) {
      // DXVK 2.x: the unlock is the swap chain's and counts from the next Reset; the request is logged either way.
      if (IHdrVkExtSwapchain* ext = Extension(d)) {
        ext->UnlockAdditionalFormats();
        ext->Release();
        Out("UnlockAdditionalFormats called on the swap chain");
      }
    }
  }
  const int secs = static_cast<int>((GetTickCount() - g_t0) / 1000);
  if (g_census) {  // the listed frame ends here
    FlushRun();
    Out("== end of frame %u: %d draws, %d runs", g_frame - 1, g_frame_draws, g_runs);
    g_census = false;
    SafeRelease(g_back);
  }
  if (g_cs_want && !g_cs_done && secs >= g_cs_at) SwitchColourSpace(d);
  const bool encode = g_output && (g_cs_on || (g_output_force && secs >= g_cs_at));
  if (encode) Reencode(d);
  if (g_next < g_at.size() && secs >= g_at[g_next] && g_hooked_vt && o_draw) {
    ++g_next;
    g_runs = g_frame_draws = 0;
    g_run = Run();
    d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &g_back);  // held for the frame, so the pointer stays this surface's
    g_final = FinalSurface();
    D3DSURFACE_DESC fd = {};
    if (g_final && Readable(reinterpret_cast<uintptr_t>(g_final), 4)) g_final->GetDesc(&fd);
    Out("== frame %u, %d s after the first present: back buffer %p, final target %p %ux%u fmt %d", g_frame, secs, g_back, g_final,
        fd.Width, fd.Height, static_cast<int>(fd.Format));
    if (g_cs_on || encode) Readback(d);  // the frame about to be presented (the one before the listed one), as the swap chain gets it
    g_census = true;
  }
}

// display.cpp: the back buffer format the probe wants the device made (and reset) with; D3DFMT_UNKNOWN: the game's.
unsigned ss_hdr_probe_backbuffer_format() { return static_cast<unsigned>(g_bb_format); }

// display.cpp, before the device is created: a float back buffer needs DXVK's format unlock (3.0 on: on the IDirect3D9).
void ss_hdr_probe_before_create(void* d3d9) {
  if (g_bb_format != D3DFMT_A16B16G16R16F || !d3d9) return;
  IHdrVkExtInterface* xi = nullptr;
  if (SUCCEEDED(static_cast<IDirect3D9*>(d3d9)->QueryInterface(kIidVkExtInterface, reinterpret_cast<void**>(&xi))) && xi) {
    xi->UnlockAdditionalFormats();
    xi->Release();
    ss_log("[hdr] probe: DXVK's additional formats unlocked on the IDirect3D9");
  } else {
    ss_log("[hdr] probe: no ID3D9VkExtInterface on the IDirect3D9 (DXVK before 3.0, or not DXVK): a float back buffer will be refused");
  }
}

void ss_hdr_probe_start() {
  if (!ss_conf_present() || !IsOn(Setting("SS_HDR_PROBE", "hdr_probe"))) return;
  if (GetModuleHandleA(nullptr) != reinterpret_cast<HMODULE>(0x400000)) return;
  g_probe = true;
  const std::string cs = Setting("SS_HDR_PROBE_COLORSPACE", "hdr_probe_colorspace");
  g_cs_want = cs == "hdr10" ? kCsHdr10 : cs == "scrgb" ? kCsScrgb : 0;
  std::string v = Setting("SS_HDR_PROBE_COLORSPACE_AT", "hdr_probe_colorspace_at");
  if (!v.empty()) g_cs_at = atoi(v.c_str());
  v = Setting("SS_HDR_PROBE_OUTPUT", "hdr_probe_output");
  g_output_force = v == "force";
  g_output = g_output_force || IsOn(v);
  v = Setting("SS_HDR_PROBE_PAPER_WHITE", "hdr_probe_paper_white");
  if (!v.empty() && atof(v.c_str()) >= 80.0) g_paper = static_cast<float>(atof(v.c_str()));
  v = Setting("SS_HDR_PROBE_BACKBUFFER", "hdr_probe_backbuffer");
  g_bb_format = v == "a2r10g10b10" ? D3DFMT_A2R10G10B10 : v == "a16f" ? D3DFMT_A16B16G16R16F : D3DFMT_UNKNOWN;
  v = Setting("SS_HDR_PROBE_AT", "hdr_probe_at");
  if (v.empty()) v = "20,45,120";
  for (const char* p = v.c_str(); *p;) {
    g_at.push_back(atoi(p));
    p = strchr(p, ',');
    if (!p) break;
    ++p;
  }
  char path[MAX_PATH + 40];
  HMODULE me = nullptr;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<const char*>(&ss_hdr_probe_start), &me);
  GetModuleFileNameA(me, path, MAX_PATH);
  char* slash = strrchr(path, '\\');
  strcpy(slash ? slash + 1 : path, "splitsecond-hdr-probe.txt");
  g_out = fopen(path, "w");
  ss_log("[hdr] probe on: census at %s s, colour space %s at %d s, output pass %s, back buffer %s -> %s", v.c_str(),
         g_cs_want ? cs.c_str() : "unchanged", g_cs_at, g_output ? "on" : "off",
         g_bb_format == D3DFMT_UNKNOWN ? "the game's" : g_bb_format == D3DFMT_A2R10G10B10 ? "A2R10G10B10" : "A16B16G16R16F",
         g_out ? "splitsecond-hdr-probe.txt" : "(cannot open splitsecond-hdr-probe.txt)");
  CreateThread(nullptr, 0, Thread, nullptr, 0, nullptr);
}
