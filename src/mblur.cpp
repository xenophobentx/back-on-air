// motion_blur_quality = game | low | high (high; low on the Steam Deck)  SS_MOTION_BLUR_QUALITY  (SSPC-62, phase 1)
//
// low and high draw the game's screen-space motion blur with our own ps_3_0 shader (blur_core.h has what the game's program
// does and what ours does differently): more taps (one per 4 pixels of streak; low 6..12, high 8..24, high as low at
// QUALITY Very High) jittered by interleaved
// gradient noise instead of the game's 9 fixed copies (the stepped streaks SSPC-54's console length showed at 60 and
// 120 fps), another object's colour that is nearer than the centre left out of the background's streak, every tap
// kept in the viewport. Same inputs, same velocity and span as the game's: the length stays motion_blur's.
//
// How: while picture.cpp's BlurHook runs the pass (render thread), SetPixelShader (slot 107 of the device's vtable, in
// d3d9.dll's memory; chained with deband.cpp, shadowsmooth.cpp and lodfade.cpp through ss_write_word) looks at the
// program the pass binds: a ps_3_0 program whose constant table names MB_matMotion360 and MB_motionBlurSourceImage is
// one of the blur's (fixed, lvl0/1/2/4). Its table gives the registers; our source is made for them and compiled on
// shadercache.cpp's worker (or read from its disk cache), never on the render thread; until it is there, and for
// good when the table lacks something or the compile fails (one log line each), the game's program draws. Ours reads
// the constants and samplers the game has set; one register no entry of the table uses carries our own (the noise
// offset, the tap range): set when ours is bound, its old value put back when the pass ends.
// Nothing in the game's code or data is written.
#include <windows.h>
#include <d3d9.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "blur_core.h"
#include "shadercache.h"
#include "ss_shared.h"

namespace {

constexpr uintptr_t kRenderer = 0xD64708;  // renderer; the device is renderer+0x578 (as patch.cpp)
constexpr uint32_t kRendererDevice = 0x578;
constexpr uintptr_t kQuality = 0xD0F7E8;   // the QUALITY option's level, 0 .. 3 (quality.cpp; 3 = 2x2 supersampling)
constexpr int kCreatePixelShaderSlot = 106, kSetPixelShaderSlot = 107;
constexpr DWORD kOptimise3 = 1u << 15;     // D3DXSHADER_OPTIMIZATION_LEVEL3

using SetPixelShaderFn = HRESULT(__stdcall*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
using CreatePixelShaderFn = HRESULT(__stdcall*)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);

volatile LONG g_level = mblur::kLevelHigh;  // mblur::Level
volatile LONG g_started = 0;
IDirect3DDevice9* g_dev = nullptr;
SetPixelShaderFn g_set_ps = nullptr;
CreatePixelShaderFn g_create_ps = nullptr;
volatile DWORD g_pass_tid = 0;  // the thread inside the blur pass (picture.cpp's BlurHook), else 0
bool g_deck = false;
int g_view = 0;  // test aid SS_MOTION_BLUR_VIEW = 1 motion ID, 2 span, 3 taps (false colour instead of the blur)
bool g_timing = false;  // test aid motion_blur_timing = true: the GPU time of the pass, a log line per 600 passes

// Our shader of one layout (one for all the game's blur programs while they share it).
struct Variant {
  mblur::Layout lay;
  std::shared_ptr<SsShaderJob> job;
  IDirect3DPixelShader9* ps = nullptr;
  IDirect3DPixelShader9* first = nullptr;  // the game program it was first made for (pipewarm's record)
  bool failed = false;
};
std::vector<Variant> g_vars;
struct Prog {
  int var = -1;  // index in g_vars; -1: not a blur program, or one ours cannot take
};
std::unordered_map<IDirect3DPixelShader9*, Prog> g_progs;               // the game's programs seen in the pass
std::unordered_map<IDirect3DPixelShader9*, IDirect3DPixelShader9*> g_ours;  // ours -> the game program behind it
int g_said_layout = 0;

// The pass's own constant: put in once per pass, the old value back at its end.
bool g_own_set = false;
int g_own_reg = -1;
float g_own_old[4];

std::vector<uint32_t> Code(IDirect3DPixelShader9* ps) {
  std::vector<uint32_t> code;
  UINT size = 0;
  if (SUCCEEDED(ps->GetFunction(nullptr, &size)) && size >= 8 && size < (1u << 20)) {
    code.resize(size / 4);
    if (FAILED(ps->GetFunction(code.data(), &size))) code.clear();
  }
  return code;
}

Prog Classify(IDirect3DPixelShader9* ps) {
  Prog p;
  const std::vector<uint32_t> code = Code(ps);
  mblur::CtabConst c[32];
  const int n = mblur::ParseCtab(code.data(), code.size(), c, 32);
  if (!mblur::IsBlurProgram(c, n)) return p;
  mblur::Layout lay;
  const char* why = "";
  const bool ps3 = !code.empty() && code[0] == 0xFFFF0300u;
  if (!ps3 || !mblur::LayoutOf(c, n, &lay, &why)) {
    if (g_said_layout++ < 6)
      ss_log("[blur] a motion blur program (%u tokens) cannot take ours (%s): the game's program draws",
             static_cast<unsigned>(code.size()), ps3 ? why : "not ps_3_0");
    return p;
  }
  for (size_t i = 0; i < g_vars.size(); ++i)
    if (g_vars[i].lay == lay) {
      p.var = static_cast<int>(i);
      return p;
    }
  Variant v;
  v.lay = lay;
  v.first = ps;
  g_vars.push_back(v);
  p.var = static_cast<int>(g_vars.size() - 1);
  if (g_said_layout++ < 6)
    ss_log("[blur] motion blur program: matrices c%d (%d, %s), params c%d, viewport c%d, colour s%d, depth s%d; ours uses c%d too",
           lay.mat_reg, lay.mats, lay.row_major ? "rows" : "columns", lay.params_reg, lay.vp_reg, lay.src_sampler, lay.depth_sampler,
           lay.own_reg);
  return p;
}

// Ours for a game program, made now if its compile is done (render thread). Null: the game's program draws.
IDirect3DPixelShader9* VariantOf(IDirect3DDevice9* d, IDirect3DPixelShader9* ps, int* own_reg) {
  auto it = g_progs.find(ps);
  if (it == g_progs.end()) {
    ps->AddRef();  // held, so the address stays this program's
    it = g_progs.emplace(ps, Classify(ps)).first;
  }
  if (it->second.var < 0) return nullptr;
  Variant& v = g_vars[it->second.var];
  *own_reg = v.lay.own_reg;
  if (v.ps || v.failed) return v.ps;
  if (!v.job) {
    auto j = std::make_shared<SsShaderJob>();
    j->tag = "[blur]";
    j->what = "motion_blur_quality low/high";
    j->Add(mblur::Source(v.lay), "main", "ps_3_0", kOptimise3);
    v.job = ss_shader_submit(j);
    return nullptr;
  }
  if (!v.job->Done()) return nullptr;
  const std::vector<uint32_t>& code = v.job->items[0].code;
  IDirect3DPixelShader9* made = nullptr;
  HRESULT hr = E_FAIL;
  if (!code.empty()) {
    ++ss_own_drawing;  // hdr.cpp's create hook does not class it
    hr = g_create_ps(d, reinterpret_cast<const DWORD*>(code.data()), &made);
    --ss_own_drawing;
  }
  const bool compiled = !code.empty();
  const unsigned tokens = static_cast<unsigned>(code.size());
  v.job.reset();  // (code is gone with it)
  if (FAILED(hr) || !made) {
    v.failed = true;
    ss_log("[blur] motion_blur_quality %s: %s; the game's motion blur draws", mblur::LevelName(g_level), !compiled ? "our shader did not compile (see [shaders])"
                                                                                         : "the device refused our shader");
    return nullptr;
  }
  v.ps = made;
  g_ours[made] = v.first;
  ++ss_shader_map_gen;  // hdr.cpp's cache of Original() is made again
  ss_pipewarm_derived(made, v.first, 4);
  ss_log("[blur] motion_blur_quality %s: our blur shader is in (%u tokens; low 6..12 taps, high 8..24, 6..12 at Very High)",
         mblur::LevelName(g_level), tokens);
  return made;
}

// Test aid motion_blur_timing: timestamp queries around the pass (render thread), read back a few passes later
// without a flush; every 600 timed passes one line: mean and worst GPU time, the level, the QUALITY level and the
// race's MB_motionBlurParams.
struct Stamp {
  IDirect3DQuery9* b = nullptr;
  IDirect3DQuery9* e = nullptr;
  bool busy = false;
};
Stamp g_st[8];
int g_st_cur = -1;
double g_st_freq = 0.0, g_st_sum = 0.0, g_st_max = 0.0;
int g_st_n = 0, g_st_lines = 0;
bool g_st_none = false;
float g_st_par[4];  // MB_motionBlurParams as the last timed pass left it

void TimingRead(IDirect3DDevice9* d) {
  for (Stamp& s : g_st) {
    if (!s.busy) continue;
    UINT64 b = 0, e = 0;
    if (s.b->GetData(&b, sizeof b, 0) != S_OK || s.e->GetData(&e, sizeof e, 0) != S_OK) continue;
    s.busy = false;
    if (e < b || g_st_freq <= 0.0) continue;
    const double ms = static_cast<double>(e - b) * 1000.0 / g_st_freq;
    g_st_sum += ms;
    if (ms > g_st_max) g_st_max = ms;
    if (++g_st_n < 600) continue;
    const float* par = g_st_par;
    if (g_st_lines++ < 60)
      ss_log("[blur] timing: the blur pass took %.3f ms on the GPU (mean of %d, worst %.3f), motion_blur_quality %s, QUALITY %d; "
             "MB_motionBlurParams %.3f %.3f %.3f %.3f",
             g_st_sum / g_st_n, g_st_n, g_st_max, mblur::LevelName(g_level), static_cast<int>(*reinterpret_cast<const volatile uint32_t*>(kQuality)),
             par[0], par[1], par[2], par[3]);
    g_st_sum = g_st_max = 0.0;
    g_st_n = 0;
  }
}

void TimingBegin(IDirect3DDevice9* d) {
  g_st_cur = -1;
  if (!g_timing || g_st_none) return;
  if (g_st_freq <= 0.0) {
    IDirect3DQuery9* fq = nullptr;
    UINT64 f = 0;
    if (SUCCEEDED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &fq)) && fq) {
      fq->Issue(D3DISSUE_END);
      for (int i = 0; i < 100 && fq->GetData(&f, sizeof f, D3DGETDATA_FLUSH) == S_FALSE; ++i) Sleep(0);
      fq->Release();
    }
    if (!f) {
      g_st_none = true;
      ss_log("[blur] timing: no timestamp queries on this device");
      return;
    }
    g_st_freq = static_cast<double>(f);
  }
  TimingRead(d);
  for (int i = 0; i < 8; ++i) {
    Stamp& s = g_st[i];
    if (s.busy) continue;
    if (!s.b && (FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &s.b)) || FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &s.e)))) {
      g_st_none = true;
      ss_log("[blur] timing: no timestamp queries on this device");
      return;
    }
    s.b->Issue(D3DISSUE_END);
    g_st_cur = i;
    return;
  }
}

void TimingEnd() {
  if (g_st_cur < 0) return;
  g_dev->GetPixelShaderConstantF(static_cast<UINT>(g_vars.empty() ? 94 : g_vars[0].lay.params_reg), g_st_par, 1);
  g_st[g_st_cur].e->Issue(D3DISSUE_END);
  g_st[g_st_cur].busy = true;
  g_st_cur = -1;
}

// The exe's .data (picture.cpp checked the image base before anything here starts).
int QualityLevel() { return static_cast<int>(*reinterpret_cast<const volatile uint32_t*>(kQuality)); }

void SetOwn(IDirect3DDevice9* d, int reg) {
  if (reg < 0) return;
  if (!g_own_set) {
    if (FAILED(d->GetPixelShaderConstantF(static_cast<UINT>(reg), g_own_old, 1))) memset(g_own_old, 0, sizeof g_own_old);
    g_own_set = true;
    g_own_reg = reg;
  }
  const mblur::TapRange taps = mblur::TapsFor(g_level, QualityLevel());
  const float own[4] = {mblur::NoiseOffset(ss_postfx_taa_frame()), static_cast<float>(taps.cap), static_cast<float>(g_view),
                        static_cast<float>(taps.min)};
  d->SetPixelShaderConstantF(static_cast<UINT>(reg), own, 1);
}

HRESULT __attribute__((stdcall, force_align_arg_pointer)) SetPixelShaderHook(IDirect3DDevice9* d, IDirect3DPixelShader9* ps) {
  if (!ps || g_level == mblur::kLevelGame || d != g_dev || ss_own_drawing != 0 || g_pass_tid != GetCurrentThreadId() || g_ours.count(ps))
    return g_set_ps(d, ps);
  int reg = -1;
  IDirect3DPixelShader9* v = VariantOf(d, ps, &reg);
  if (!v) return g_set_ps(d, ps);
  const HRESULT hr = g_set_ps(d, v);
  if (SUCCEEDED(hr)) SetOwn(d, reg);
  return hr;
}

bool Readable(uintptr_t p, size_t n) {
  MEMORY_BASIC_INFORMATION mbi;
  if (p < 0x10000 || !SS_VQ(reinterpret_cast<void*>(p), &mbi, sizeof mbi)) return false;
  if (mbi.State != MEM_COMMIT || mbi.Protect == 0 || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
  return p + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

// As deband.cpp: the slot's word is swapped under ss_write_word's lock and the hook calls what was there.
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
    ss_log("[blur] the device's CreatePixelShader is not in a d3d9 module; motion_blur_quality stays game");
    return true;
  }
  g_create_ps = reinterpret_cast<CreatePixelShaderFn>(vt[kCreatePixelShaderSlot]);
  g_set_ps = reinterpret_cast<SetPixelShaderFn>(vt[kSetPixelShaderSlot]);
  uintptr_t prev = 0;
  const bool ok = ss_write_word(&vt[kSetPixelShaderSlot], reinterpret_cast<uintptr_t>(&SetPixelShaderHook), &prev);
  if (ok && prev) g_set_ps = reinterpret_cast<SetPixelShaderFn>(prev);
  if (ok) g_dev = reinterpret_cast<IDirect3DDevice9*>(dev);
  ss_log("[blur] motion_blur_quality %s: SetPixelShader %s (device %p)", mblur::LevelName(g_level), ok ? "hooked" : "NOT hooked (error)",
         reinterpret_cast<void*>(dev));
  return true;
}

DWORD WINAPI Worker(LPVOID) {
  Sleep(5000);  // with the other parts' slots of the same vtable; the hooks chain whatever the order
  while (!HookDevice()) Sleep(200);
  return 0;
}

}  // namespace

// SteamDeck=1 in the environment (as hdr.cpp's Handheld): the default is low. Safe from static initialisers.
bool ss_mblur_deck() {
  const char* e = getenv("SteamDeck");
  return e && !strcmp(e, "1");
}
const char* ss_mblur_default() { return mblur::LevelName(mblur::DefaultLevel(ss_mblur_deck())); }

// picture.cpp, at the start of the conf's read: the setting, and the hook's thread.
void ss_mblur_start() {
  if (InterlockedExchange(&g_started, 1)) return;
  const char* e = getenv("SS_MOTION_BLUR_QUALITY");
  const std::string v = e ? std::string(e) : ss_conf("motion_blur_quality");
  g_deck = ss_mblur_deck();
  g_level = mblur::ParseLevel(v.c_str(), g_deck);
  // Test aids: the env (Linux / Proton launch options) or the conf (Windows: the exe restarts itself without the env).
  const char* t = getenv("SS_MOTION_BLUR_VIEW");
  const int view = atoi(t ? t : ss_conf("motion_blur_view").c_str());
  g_view = view >= 1 && view <= 3 ? view : 0;
  const char* tm = getenv("SS_MOTION_BLUR_TIMING");
  const std::string tv = tm ? std::string(tm) : ss_conf("motion_blur_timing");
  g_timing = tv == "1" || tv == "true" || tv == "on";
  if (g_timing) ss_log("[blur] test aid motion_blur_timing: the GPU time of the blur pass is logged");
  if (g_view) ss_log("[blur] test aid SS_MOTION_BLUR_VIEW = %d: %s in false colour instead of the blur", g_view,
                     g_view == 1 ? "the motion ID" : g_view == 2 ? "the span" : "the tap count");
  ss_log("[blur] motion_blur_quality %s (%s)%s", mblur::LevelName(g_level),
         g_level == mblur::kLevelGame ? "the game's blur shader" : "our blur shader", g_deck ? "; Steam Deck: default low" : "");
  CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
}

// picture.cpp's BlurHook, around the game's pass (render thread).
void ss_mblur_pass_begin() {
  if (!g_dev) return;
  g_pass_tid = GetCurrentThreadId();
  TimingBegin(g_dev);
}
void ss_mblur_pass_end() {
  TimingEnd();
  g_pass_tid = 0;
  if (g_own_set && g_dev) g_dev->SetPixelShaderConstantF(static_cast<UINT>(g_own_reg), g_own_old, 1);
  g_own_set = false;
}

// The settings menu: at once (the next pass binds ours or the game's). False while the hook is not in.
bool ss_mblur_apply(const char* value) {
  if (!g_dev) return false;
  g_level = mblur::ParseLevel(value, g_deck);
  ss_log("[blur] from the settings menu: motion_blur_quality %s", mblur::LevelName(g_level));
  return true;
}

// hdr.cpp: the game's program behind ours, else ps.
void* ss_blur_original(void* ps) {
  if (g_ours.empty()) return ps;
  auto it = g_ours.find(static_cast<IDirect3DPixelShader9*>(ps));
  return it == g_ours.end() ? ps : it->second;
}

// pipewarm.cpp's replay at a level load (render thread): ours for a game blur program, made now if its compile is done.
void* ss_blur_variant(void* device, void* ps) {
  auto* d = static_cast<IDirect3DDevice9*>(device);
  if (!ps || g_level == mblur::kLevelGame || !g_create_ps || d != g_dev || g_ours.count(static_cast<IDirect3DPixelShader9*>(ps))) return nullptr;
  int reg = -1;
  return VariantOf(d, static_cast<IDirect3DPixelShader9*>(ps), &reg);
}
