// pipewarm.cpp: pipeline_warmup (SSPC-35), DXVK's graphics pipelines of a level made while its load screen is up.
//
// Why (measured on Windows + AMD, DXVK 2.7.1, empty driver cache): the first race on a track has 40-800 ms Present
// blocks; the game already has every shader before the race (warmup.cpp's record), and making the shaders again at
// start changed nothing. The cost is the Vulkan graphics PIPELINE DXVK makes on its CS thread at the first draw of a
// (shader pair x pipeline state) combination.
//
// What DXVK 2.7.1 puts in a pipeline (public source, dxvk_graphics_state.h DxvkGraphicsPipelineStateInfo and the
// d3d9 binds that fill it), per DxvkGraphicsPipeline = per (VS, PS) DxvkShader pair (DXVK keeps one module per byte
// code SHA-1, so our own shader objects made from the same byte code reach the same pipeline object):
//   ia  primitive topology (the draw's D3DPRIMITIVETYPE)
//   il  vertex attributes (location, binding, format, offset) from the vertex declaration matched to the VS inputs;
//       bindings' input rate and divisor (SetStreamSourceFreq INSTANCEDATA); strides are dynamic (not in the key)
//   rs  fill mode, flat shading (D3DRS_SHADEMODE), sample count; depth clip; cull mode and front face are dynamic
//   ms  sample mask (D3DRS_MULTISAMPLEMASK), alpha to coverage (the ATOC / A2M1 hacks + an MSAA RT0)
//   rt  colour formats of RT0..3 (with the sRGB view when D3DRS_SRGBWRITEENABLE), depth format; a slot bound as a
//       texture and not written is dropped (hazard), feedback-loop aspects
//   omBlend / omSwizzle  blend enable, factors, ops (separate alpha), COLORWRITEENABLE0..3; X8 formats' alpha swizzle
//   sc  spec constants: alpha test op (+ precision from RT0's format), fog enable and modes, VS/PS bool constants (the
//       ones the shader reads), point mode (point lists), sampler null / depth (shadow) / D32f-upgrade masks of the
//       samplers the shaders use, fetch4, clip plane count; sampler types and projection only for ps_1_x
// Not in it (dynamic state in 2.7.1 once a depth format is bound): depth test / write / func, stencil, depth bias,
// cull mode, blend constants, stencil ref, viewport, scissor, vertex strides.
// The cost at a first draw on the CS thread: the shader pair's pipeline libraries again (a 32-bit process on a non-RADV
// driver tracks pipeline lifetime: the libraries are destroyed when no base pipeline uses them), a fast link, or a full
// compile where GPL cannot link (dual-source blend, flat-shaded inputs, VS inputs missing from the declaration, ...).
// Once drawn, DXVK queues an optimized pipeline on its workers and keeps it for the device's life: a later draw of the
// same state finds it.
//
// So: pipeline_warmup = record hooks the four draw slots directly above DXVK (installed before every other device
// hook, so it sees the state DXVK gets: lodfade's / shadow_smoothing's / deband's shader swaps included), reads that
// state back from DXVK (Get*: state blocks and our own modules' sets are all seen), and appends every new
// combination with the shaders' byte code to pipeline-warmup\<level>.bin beside the DLL (the level from the game's
// first "environments/levels/<name>" file open after the level load begins; "frontend" in the front end).
// pipeline_warmup = replay: when the level load state (cInGameLoadState) presents with a level whose file exists,
// every recorded combination is set on the device (DXVK's own slots, no other hook sees it) with small off-screen
// targets and dummy textures of the recorded formats and drawn once (one degenerate primitive), in slices of
// SS_PIPEWARM_SLICE_MS (default 40) per present inside a D3DSBT_ALL state block (render targets and depth surface
// saved and put back by hand: a state block does not hold them); then an event query waits for DXVK's CS thread, and
// a watcher logs when DXVK's compiler threads (dxvk-shader-*) go idle and the free address space.
// pipeline_warmup = both: replay and record (what the replay missed is recorded and counted).
// DXVK only (the module exporting DXVK_RegisterAnnotation, ordinal 28257). SS_PIPELINE_WARMUP.
// Records: pipeline-warmup\<level>.bin (shipped with the patch) and pipeline-warmup\local\<level>.bin (this machine's,
// the only place record mode writes); both are read, each combination once. Empty or auto = replay when records exist.
// With records the menu gets GRAPHICS > PRE-COMPILE (off / on, live from the next load) and PRE-COMPILE ALL TRACKS
// (every level now, with the shaders read from the game's own archives: see "pre-compile all" below), and with replay
// on at start DXVK gets 4 compiler threads (ss_pipewarm_start).
#include <windows.h>
#include <d3d9.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "arkscan.h"
#include "menu.h"
#include "ss_shared.h"

#define ENTRY_STD __attribute__((stdcall, force_align_arg_pointer))

namespace {

constexpr WORD kDxvkAnnotationOrdinal = 28257;
constexpr int kNumSlots = 119;
// IDirect3DDevice9 slots
enum {
  S_CreateTexture = 23, S_CreateVolumeTexture = 24, S_CreateCubeTexture = 25, S_CreateVertexBuffer = 26,
  S_CreateIndexBuffer = 27, S_CreateRenderTarget = 28, S_CreateDepthStencilSurface = 29, S_SetRenderTarget = 37,
  S_GetRenderTarget = 38, S_SetDepthStencilSurface = 39, S_GetDepthStencilSurface = 40, S_BeginScene = 41,
  S_EndScene = 42, S_SetViewport = 47, S_SetClipPlane = 55, S_GetClipPlane = 56, S_SetRenderState = 57,
  S_GetRenderState = 58, S_CreateStateBlock = 59, S_GetTexture = 64, S_SetTexture = 65, S_GetSamplerState = 68,
  S_SetSamplerState = 69, S_DrawPrimitive = 81, S_DrawIndexedPrimitive = 82, S_DrawPrimitiveUP = 83,
  S_DrawIndexedPrimitiveUP = 84, S_CreateVertexDeclaration = 86, S_SetVertexDeclaration = 87,
  S_GetVertexDeclaration = 88, S_SetFVF = 89, S_GetFVF = 90, S_CreateVertexShader = 91, S_SetVertexShader = 92,
  S_GetVertexShader = 93, S_SetVertexShaderConstantB = 98, S_GetVertexShaderConstantB = 99, S_SetStreamSource = 100,
  S_SetStreamSourceFreq = 102, S_GetStreamSourceFreq = 103, S_SetIndices = 104, S_CreatePixelShader = 106,
  S_SetPixelShader = 107, S_GetPixelShader = 108, S_SetPixelShaderConstantB = 113, S_GetPixelShaderConstantB = 114,
  S_CreateQuery = 118
};

void* g_orig[kNumSlots] = {};  // DXVK's own entries, copied before any other module hooks the device

template <int S, typename R = HRESULT, typename... A>
R Call(IDirect3DDevice9* d, A... a) {
  return reinterpret_cast<R(__stdcall*)(IDirect3DDevice9*, A...)>(g_orig[S])(d, a...);
}

// The render states that reach DXVK's pipeline key (see the head comment), in the record's order.
const D3DRENDERSTATETYPE kRs[] = {
    D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_SEPARATEALPHABLENDENABLE,
    D3DRS_SRCBLENDALPHA, D3DRS_DESTBLENDALPHA, D3DRS_BLENDOPALPHA, D3DRS_COLORWRITEENABLE, D3DRS_COLORWRITEENABLE1,
    D3DRS_COLORWRITEENABLE2, D3DRS_COLORWRITEENABLE3, D3DRS_FILLMODE, D3DRS_SHADEMODE, D3DRS_MULTISAMPLEMASK,
    D3DRS_MULTISAMPLEANTIALIAS, D3DRS_ALPHATESTENABLE, D3DRS_ALPHAFUNC, D3DRS_FOGENABLE, D3DRS_FOGTABLEMODE,
    D3DRS_FOGVERTEXMODE, D3DRS_SRGBWRITEENABLE, D3DRS_POINTSPRITEENABLE, D3DRS_POINTSCALEENABLE, D3DRS_POINTSIZE,
    D3DRS_ADAPTIVETESS_Y};
constexpr int kNumRs = sizeof(kRs) / sizeof(kRs[0]);
enum { R_ABLEND, R_SRC, R_DST, R_OP, R_SEP, R_SRCA, R_DSTA, R_OPA, R_CW0, R_CW1, R_CW2, R_CW3, R_FILL, R_SHADE,
       R_MSMASK, R_MSAA, R_ATEST, R_AFUNC, R_FOG, R_FOGT, R_FOGV, R_SRGB, R_PSPRITE, R_PSCALE, R_PSIZE, R_TESSY };
constexpr DWORD kA2M1 = MAKEFOURCC('A', '2', 'M', '1'), kAtoc = MAKEFOURCC('A', 'T', 'O', 'C');
constexpr DWORD kGet4 = MAKEFOURCC('G', 'E', 'T', '4');
constexpr int kSamplers = 20;  // PS s0..15, VS s0..3 (API stages 257..260)
constexpr int kMaxDecl = 16;

#pragma pack(push, 1)
struct Target {
  uint32_t fmt;     // D3DFORMAT, 0 = none
  uint8_t ms, msq;  // multisample type, quality (0 unless the type is NONMASKABLE)
  uint8_t tex;      // 1: level 0 of a texture (the replay makes it as one when a sampler reads it)
  uint8_t pad;
};
struct Tex {
  uint32_t fmt;
  uint8_t type;    // 0 none (a used sampler with nothing bound), 1 2D, 2 cube, 3 volume
  uint8_t flags;   // 1 render-target texture, 2 depth-stencil texture, 4 fetch4 asked (GET4 bias + point mag)
  uint8_t hazard;  // 0, or 1 + the target slot (0..3 colour, 4 depth) this texture's level 0 is bound to
  uint8_t pad;
};
struct Combo {
  uint64_t vs, ps;  // byte code hashes (0: none)
  uint8_t prim, ndecl, clip, pad0;
  uint16_t vsb, psb;  // bool constants the shaders read
  uint32_t fvf;       // only with no declaration
  uint32_t rs[kNumRs];
  Target rt[5];  // 0..3 colour, 4 depth
  Tex tex[kSamplers];
  uint32_t freq[4];  // INSTANCEDATA stream frequencies of streams 0..3 (else 0)
  D3DVERTEXELEMENT9 decl[kMaxDecl];
};
#pragma pack(pop)

constexpr uint32_t kFileMagic = 0x57505353;  // "SSPW"
constexpr uint32_t kFileVersion = 3;  // 2: no derived entries; 1: also held the shaders' byte code (read: its combinations only)
constexpr uint32_t kTagShader = 0x52444853, kTagCombo = 0x4F424D43, kTagDerived = 0x56495244;  // "DRIV"
// A pixel shader variant one of our parts makes of a game shader (lodfade's dither, shadowsmooth's, deband's): the
// replay asks that part for its variant of the held game shader, with the settings of the day.
#pragma pack(push, 1)
struct Derived {
  uint64_t variant, original;  // byte code hashes
  uint32_t maker;              // 1 lodfade, 2 shadowsmooth, 3 deband
  uint32_t pad;
};
#pragma pack(pop)
constexpr uint32_t kMaxShaderBytes = 256 * 1024;
constexpr size_t kMaxCombos = 30000;

uint64_t Fnv(const void* p, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
  const auto* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 0x100000001b3ull;
  return h;
}

double MsSince(LARGE_INTEGER a) {
  static LARGE_INTEGER f = {};
  if (!f.QuadPart) QueryPerformanceFrequency(&f);
  LARGE_INTEGER b;
  QueryPerformanceCounter(&b);
  return double(b.QuadPart - a.QuadPart) * 1000.0 / double(f.QuadPart);
}

unsigned FreeVaMb() {
  MEMORYSTATUSEX ms = {};
  ms.dwLength = sizeof ms;
  return GlobalMemoryStatusEx(&ms) ? static_cast<unsigned>(ms.ullAvailVirtual >> 20) : 0;
}

// ------------------------------------------------------------------------------------------------ config and state
enum Mode { M_OFF, M_RECORD, M_REPLAY, M_BOTH };
int g_mode = M_OFF;
bool g_record = false, g_replay = false;  // g_replay: the PRE-COMPILE row changes it live (next level load)
volatile bool g_ca_owns = false;          // "pre-compile all tracks" runs: it holds shaders and owns g_rp
long g_ca_drawn = 0, g_ca_skipped = 0;    // its replays' counts (FinishReplay adds them)
std::wstring g_dir;    // <DLL folder>\pipeline-warmup\ (with the backslash): the records shipped with the patch
std::wstring g_local;  // g_dir + local\ : what record mode writes on this machine (read too)
CRITICAL_SECTION g_cs;
bool g_cs_ready = false;

// The level: set from the game's file opens (CreateFileA import slot), read by the draw hook and the present hook.
char g_level[48] = "boot";
int g_level_state = -1;  // the game state when g_level was set from a file open
volatile LONG g_level_gen = 0;
int g_last_state = -2;
int g_level_logs = 0;

// ------------------------------------------------------------------------------------------------ shader byte code
struct ShInfo {
  uint64_t hash = 0;
  uint32_t size = 0;
  uint32_t samplers = 0;   // bits: PS 0..15, VS 16..19
  uint32_t bools = 0;      // bool registers read
  uint8_t stype[kSamplers] = {};  // the dcl's texture type (2 2D, 3 cube, 4 volume)
  uint8_t major = 0;
  bool vs = false;
};

void ParseShader(const DWORD* t, uint32_t bytes, bool vs, ShInfo& info) {
  const uint32_t n = bytes / 4;
  if (n < 2) return;
  info.major = static_cast<uint8_t>((t[0] >> 8) & 0xff);
  info.vs = vs;
  if (info.major < 2) {  // no instruction lengths: every sampler a ps_1_x can read
    if (!vs) info.samplers = 0x3f;
    return;
  }
  uint32_t i = 1;
  while (i < n) {
    const DWORD tok = t[i];
    if (tok == 0x0000FFFF) break;
    const DWORD op = tok & 0xFFFF;
    if (op == 0xFFFE) {
      i += 1 + ((tok >> 16) & 0x7FFF);
      continue;
    }
    const uint32_t len = (tok >> 24) & 0xF;
    if (op == 31 && i + 2 < n) {  // dcl
      const DWORD usage = t[i + 1], dst = t[i + 2];
      const uint32_t type = ((dst >> 28) & 7) | ((dst >> 8) & 0x18), num = dst & 0x7FF;
      if (type == 10) {  // sampler
        const uint32_t bit = vs ? 16 + num : num;
        if (bit < kSamplers) {
          info.samplers |= 1u << bit;
          info.stype[bit] = static_cast<uint8_t>((usage >> 27) & 0xF);
        }
      }
    } else if (op != 81 && op != 82 && op != 83) {  // not def / defi / defb (immediates)
      for (uint32_t k = 1; k <= len && i + k < n; ++k) {
        const DWORD r = t[i + k];
        const uint32_t type = ((r >> 28) & 7) | ((r >> 8) & 0x18);
        if (type == 14 && (r & 0x7FF) < 16) info.bools |= 1u << (r & 0x7FF);  // CONSTBOOL
      }
    }
    i += 1 + len;
  }
}

// The levels that have a record (<level>.bin in either folder; boot and frontend left out), sorted.
std::vector<std::string> RecordLevels(const std::wstring& dir, const std::wstring& local) {
  std::vector<std::string> out;
  for (const std::wstring* d : {&dir, &local}) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((*d + L"*.bin").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) continue;
    do {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      std::wstring n = fd.cFileName;
      n.resize(n.size() - 4);
      bool plain = !n.empty() && n.size() <= 47;
      for (wchar_t c : n) plain = plain && c < 128;
      if (!plain || n == L"boot" || n == L"frontend") continue;
      std::string level(n.begin(), n.end());
      if (std::find(out.begin(), out.end(), level) == out.end()) out.push_back(level);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  std::sort(out.begin(), out.end());
  return out;
}

// ------------------------------------------------------------------------------------------------ the record
struct LevelFile {
  std::string level;
  std::unordered_set<uint64_t> combos, shaders;
  std::unordered_map<uint64_t, Derived> derived;  // variants named in the file
  std::vector<uint8_t> pending;  // appended records, written at the next present
  HANDLE file = INVALID_HANDLE_VALUE;
  bool full_logged = false;
};
LevelFile g_rec;  // the level being recorded
std::unordered_map<uintptr_t, ShInfo> g_sh;           // shader object -> info (validated by size)
std::unordered_map<uintptr_t, std::vector<D3DVERTEXELEMENT9>> g_decls;
struct RecStats {
  long draws = 0, fresh = 0, missed_replay = 0, fixed = 0;
} g_rs;
std::unordered_set<uint64_t> g_replayed;  // combos the replay made this load (both: a new one is a miss)
std::unordered_map<uint64_t, Derived> g_derivs;  // every variant our parts made this session (variant hash -> entry)
// Shaders a record names that are made after the load (state 4/5: the race): by the game, and our parts' variants.
long g_late_wanted = 0, g_late_derived = 0;

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// Reads a level file: combos (in order) and shader byte code by hash. A bad tail is dropped.
bool ReadLevelFile(const std::wstring& dir, const std::string& level, std::vector<Combo>* combos,
                   std::unordered_set<uint64_t>* combo_hashes, std::unordered_set<uint64_t>* shader_hashes,
                   std::unordered_map<uint64_t, Derived>* derived) {
  const std::wstring path = dir + Widen(level) + L".bin";
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size = {};
  std::vector<uint8_t> all;
  if (GetFileSizeEx(f, &size) && size.QuadPart > 8 && size.QuadPart < (256ll << 20)) {
    all.resize(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    if (!ReadFile(f, all.data(), static_cast<DWORD>(all.size()), &got, nullptr) || got != all.size()) all.clear();
  }
  CloseHandle(f);
  if (all.size() < 8) return false;
  uint32_t h[2];
  memcpy(h, all.data(), 8);
  if (h[0] != kFileMagic || h[1] < 1 || h[1] > kFileVersion) {
    ss_log("[pipewarm] %ls: not a version %u record; ignored", path.c_str(), kFileVersion);
    return false;
  }
  size_t at = 8;
  while (at + 16 <= all.size()) {
    uint32_t tag, len;
    uint64_t hash;
    memcpy(&tag, &all[at], 4);
    memcpy(&len, &all[at + 4], 4);
    memcpy(&hash, &all[at + 8], 8);
    if (len > kMaxShaderBytes + 4 || all.size() - at - 16 < len) break;
    const uint8_t* p = &all[at + 16];
    if (Fnv(p, len) != hash) break;
    if (tag == kTagCombo && len == sizeof(Combo)) {
      if (combo_hashes && !combo_hashes->insert(hash).second) {  // in the other folder's file already
        at += 16 + len;
        continue;
      }
      Combo c;
      memcpy(&c, p, sizeof c);
      if (shader_hashes) shader_hashes->insert({c.vs, c.ps});
      if (combos) combos->push_back(c);
    } else if (tag == kTagDerived && len == sizeof(Derived)) {
      Derived x;
      memcpy(&x, p, sizeof x);
      if (shader_hashes) shader_hashes->insert(x.original);  // the game shader is held, its variant made from it
      if (derived) (*derived)[x.variant] = x;
    } else if (tag == kTagShader && h[1] == 1) {
      // version 1 only: a shader's byte code; skipped (the replay takes the shader objects the game makes)
    } else {
      break;
    }
    at += 16 + len;
  }
  if (at != all.size()) ss_log("[pipewarm] %ls: bad data after byte %u; the rest is ignored", path.c_str(), static_cast<unsigned>(at));
  return true;
}

// A level's record: the shipped file, then this machine's own (local\), combinations once.
bool ReadLevel(const std::string& level, std::vector<Combo>* combos, std::unordered_set<uint64_t>* combo_hashes,
               std::unordered_set<uint64_t>* shader_hashes, std::unordered_map<uint64_t, Derived>* derived = nullptr) {
  std::unordered_set<uint64_t> seen;
  if (!combo_hashes && combos) combo_hashes = &seen;
  const bool a = ReadLevelFile(g_dir, level, combos, combo_hashes, shader_hashes, derived);
  const bool b = ReadLevelFile(g_local, level, combos, combo_hashes, shader_hashes, derived);
  return a || b;
}

void Put(std::vector<uint8_t>& out, uint32_t tag, const void* a, uint32_t na, const void* b = nullptr, uint32_t nb = 0) {
  std::vector<uint8_t> payload(na + nb);
  memcpy(payload.data(), a, na);
  if (nb) memcpy(payload.data() + na, b, nb);
  const uint64_t hash = Fnv(payload.data(), payload.size());
  const uint32_t len = na + nb;
  const size_t at = out.size();
  out.resize(at + 16 + len);
  memcpy(&out[at], &tag, 4);
  memcpy(&out[at + 4], &len, 4);
  memcpy(&out[at + 8], &hash, 8);
  memcpy(&out[at + 16], payload.data(), len);
}

void FlushRecord() {  // under g_cs
  if (g_rec.pending.empty()) return;
  if (g_rec.file == INVALID_HANDLE_VALUE) {
    CreateDirectoryW(g_local.c_str(), nullptr);
    const std::wstring path = g_local + Widen(g_rec.level) + L".bin";
    g_rec.file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (g_rec.file == INVALID_HANDLE_VALUE) {
      g_rec.pending.clear();
      return;
    }
    LARGE_INTEGER sz = {};
    if (GetFileSizeEx(g_rec.file, &sz) && sz.QuadPart == 0) {
      const uint32_t h[2] = {kFileMagic, kFileVersion};
      DWORD w = 0;
      WriteFile(g_rec.file, h, 8, &w, nullptr);
    }
  }
  DWORD w = 0;
  WriteFile(g_rec.file, g_rec.pending.data(), static_cast<DWORD>(g_rec.pending.size()), &w, nullptr);
  g_rec.pending.clear();
}

void SwitchRecordLevel(const char* level) {  // under g_cs
  if (g_rec.level == level) return;
  FlushRecord();
  if (g_rec.file != INVALID_HANDLE_VALUE) CloseHandle(g_rec.file);
  if (!g_rec.level.empty())
    ss_log("[pipewarm] record: %s holds %u combinations, %u shaders", g_rec.level.c_str(),
           static_cast<unsigned>(g_rec.combos.size()), static_cast<unsigned>(g_rec.shaders.size()));
  g_rec = LevelFile{};
  g_rec.level = level;
  ReadLevel(level, nullptr, &g_rec.combos, &g_rec.shaders, &g_rec.derived);
  for (const auto& e : g_derivs)  // variants made in an earlier level that this file names without their source
    if (g_rec.shaders.count(e.first) && g_rec.derived.emplace(e.first, e.second).second)
      Put(g_rec.pending, kTagDerived, &e.second, sizeof e.second);
}

// ------------------------------------------------------------------------------------------------ the game's shaders
// The records hold shader hashes, not byte code (the game's data is never copied to a file). With replay on, the two
// create slots are hooked directly above DXVK; a new shader whose hash a level record names (g_wanted: every level
// file's shaders, not boot / frontend) is kept (AddRef) for the session, so the replay draws with the game's own
// objects. DXVK keeps every shader module for the device's life anyway (its module set is never emptied), so a held
// object costs only its small D3D9 wrapper. A shader the game has not made when the replay runs is skipped.
struct Held {
  IUnknown* obj;
  bool vs;
};
std::unordered_set<uint64_t> g_wanted;
std::unordered_set<uint64_t> g_variants;  // the records' variants of ours (made from a game shader, never in the game files)
std::unordered_map<uint64_t, Held> g_held;  // under g_cs
LARGE_INTEGER g_last_keep = {};             // the last wanted shader kept
long g_kept = 0, g_creates = 0;
std::unordered_set<IUnknown*> g_late_objs;  // wanted shaders kept in the race (our variants are taken out when named)

template <typename T>
bool CodeOf(T* s, std::vector<uint32_t>& code) {
  UINT n = 0;
  if (FAILED(s->GetFunction(nullptr, &n)) || n < 8 || n > kMaxShaderBytes || (n & 3)) return false;
  code.resize(n / 4);
  return SUCCEEDED(s->GetFunction(code.data(), &n));
}

uint64_t ShaderHash(const std::vector<uint32_t>& code, bool vs) {
  return Fnv(code.data(), code.size() * 4, vs ? 0x5653ull : 0x5053ull);
}

template <typename T>
void Keep(T* s, bool vs) {
  std::vector<uint32_t> code;
  if (!CodeOf(s, code)) return;
  const uint64_t h = ShaderHash(code, vs);
  EnterCriticalSection(&g_cs);
  ++g_creates;
  if ((g_replay || g_ca_owns) && g_wanted.count(h) && !g_held.count(h)) {
    s->AddRef();
    g_held[h] = Held{s, vs};
    ++g_kept;
    QueryPerformanceCounter(&g_last_keep);
    if (ss_game_state == 4 || ss_game_state == 5) {
      ++g_late_wanted;
      g_late_objs.insert(s);
    }
  }
  LeaveCriticalSection(&g_cs);
}

HRESULT ENTRY_STD CreateVsHook(IDirect3DDevice9* d, const DWORD* f, IDirect3DVertexShader9** out) {
  const HRESULT hr = Call<S_CreateVertexShader>(d, f, out);
  if (SUCCEEDED(hr) && out && *out) Keep(*out, true);
  return hr;
}
HRESULT ENTRY_STD CreatePsHook(IDirect3DDevice9* d, const DWORD* f, IDirect3DPixelShader9** out) {
  const HRESULT hr = Call<S_CreatePixelShader>(d, f, out);
  if (SUCCEEDED(hr) && out && *out) Keep(*out, false);
  return hr;
}

}  // namespace

void ss_pipewarm_derived(void* variant, void* original, int maker) {
  if (!g_cs_ready || !variant || !original) return;
  std::vector<uint32_t> a, b;
  if (!CodeOf(static_cast<IDirect3DPixelShader9*>(variant), a) || !CodeOf(static_cast<IDirect3DPixelShader9*>(original), b))
    return;
  const Derived x = {ShaderHash(a, false), ShaderHash(b, false), static_cast<uint32_t>(maker), 0};
  EnterCriticalSection(&g_cs);
  g_derivs[x.variant] = x;
  // The level file names the variant already (its combinations recorded before this entry existed): name its source.
  if (g_record && g_rec.shaders.count(x.variant) && g_rec.derived.emplace(x.variant, x).second)
    Put(g_rec.pending, kTagDerived, &x, sizeof x);
  if (g_late_objs.erase(static_cast<IUnknown*>(variant))) ++g_late_derived;
  LeaveCriticalSection(&g_cs);
}

namespace {

// Every level file's shader hashes (boot and frontend left out: the replay runs at a level load only).
void LoadWanted() {
  int files = 0;
  for (const std::string& level : RecordLevels(g_dir, g_local)) {
    std::unordered_map<uint64_t, Derived> derived;
    if (ReadLevel(level, nullptr, nullptr, &g_wanted, &derived)) ++files;
    for (const auto& e : derived) g_variants.insert(e.first);
  }
  g_wanted.erase(0);
  ss_log("[pipewarm] %d level records name %zu shaders", files, g_wanted.size());
}

// Shader info for a shader object bound in DXVK (byte code read back once per object; validated by its size).
template <typename T>
const ShInfo* Info(T* s, bool vs) {
  if (!s) return nullptr;
  UINT n = 0;
  if (FAILED(s->GetFunction(nullptr, &n)) || n < 8 || n > kMaxShaderBytes || (n & 3)) return nullptr;
  auto it = g_sh.find(reinterpret_cast<uintptr_t>(s));
  if (it != g_sh.end() && it->second.size == n) return &it->second;
  std::vector<uint32_t> code;
  if (!CodeOf(s, code)) return nullptr;
  ShInfo info;
  info.size = n;
  info.hash = ShaderHash(code, vs);
  ParseShader(reinterpret_cast<const DWORD*>(code.data()), n, vs, info);
  return &(g_sh[reinterpret_cast<uintptr_t>(s)] = info);
}

void ClassifyTarget(IDirect3DSurface9* s, Target& t) {
  if (!s) return;
  D3DSURFACE_DESC d;
  if (FAILED(s->GetDesc(&d))) return;
  t.fmt = d.Format;
  t.ms = static_cast<uint8_t>(d.MultiSampleType);
  t.msq = d.MultiSampleType == D3DMULTISAMPLE_NONMASKABLE ? static_cast<uint8_t>(d.MultiSampleQuality) : 0;
}

// The draw's pipeline-relevant state, read back from DXVK. False: not recordable (no shaders: fixed function).
bool BuildCombo(IDirect3DDevice9* d, D3DPRIMITIVETYPE prim, Combo& c) {
  memset(&c, 0, sizeof c);
  IDirect3DVertexShader9* vs = nullptr;
  IDirect3DPixelShader9* ps = nullptr;
  Call<S_GetVertexShader>(d, &vs);
  Call<S_GetPixelShader>(d, &ps);
  const ShInfo* vi = Info(vs, true);
  const ShInfo* pi = Info(ps, false);
  if (vs) vs->Release();
  if (ps) ps->Release();
  if (!vi || !pi) return false;
  c.vs = vi->hash;
  c.ps = pi->hash;
  c.prim = static_cast<uint8_t>(prim);

  IDirect3DVertexDeclaration9* decl = nullptr;
  Call<S_GetVertexDeclaration>(d, &decl);
  if (decl) {
    UINT n = 0;
    decl->GetDeclaration(nullptr, &n);
    auto& v = g_decls[reinterpret_cast<uintptr_t>(decl)];
    if (v.size() != n && n <= 65) {
      v.resize(n);
      decl->GetDeclaration(v.data(), &n);
    }
    decl->Release();
    uint32_t k = 0;
    for (const auto& e : v) {
      if (e.Stream == 0xFF) break;
      if (k == kMaxDecl) return false;
      c.decl[k++] = e;
    }
    c.ndecl = static_cast<uint8_t>(k);
  } else {
    Call<S_GetFVF>(d, &c.fvf);
  }
  uint32_t streams = 0;
  for (int k = 0; k < c.ndecl; ++k)
    if (c.decl[k].Stream < 4) streams |= 1u << c.decl[k].Stream;
  for (int s = 0; s < 4; ++s) {
    UINT f = 0;
    if ((streams >> s & 1) && SUCCEEDED(Call<S_GetStreamSourceFreq>(d, static_cast<UINT>(s), &f)) &&
        (f & D3DSTREAMSOURCE_INSTANCEDATA))
      c.freq[s] = f;
  }

  for (int k = 0; k < kNumRs; ++k) Call<S_GetRenderState>(d, kRs[k], &c.rs[k]);
  auto& r = c.rs;
  if (!r[R_ABLEND]) r[R_SRC] = r[R_DST] = r[R_OP] = r[R_SEP] = 0;
  if (!r[R_ABLEND] || !r[R_SEP]) r[R_SRCA] = r[R_DSTA] = r[R_OPA] = 0;
  if (!r[R_ATEST]) r[R_AFUNC] = 0;
  if (!r[R_FOG]) r[R_FOGT] = r[R_FOGV] = 0;
  if (prim != D3DPT_POINTLIST) r[R_PSPRITE] = r[R_PSCALE] = 0;
  if (r[R_PSIZE] != kA2M1) r[R_PSIZE] = 0;
  if (r[R_TESSY] != kAtoc) r[R_TESSY] = 0;

  DWORD clip = 0;
  Call<S_GetRenderState>(d, D3DRS_CLIPPLANEENABLE, &clip);
  for (int k = 0; k < 6; ++k) {
    float pl[4] = {};
    if ((clip >> k & 1) && SUCCEEDED(Call<S_GetClipPlane>(d, static_cast<DWORD>(k), pl)) &&
        (pl[0] != 0 || pl[1] != 0 || pl[2] != 0 || pl[3] != 0))
      ++c.clip;
  }

  IDirect3DSurface9* surf[5] = {};
  for (int k = 0; k < 4; ++k) Call<S_GetRenderTarget>(d, static_cast<DWORD>(k), &surf[k]);
  Call<S_GetDepthStencilSurface>(d, &surf[4]);
  for (int k = 0; k < 5; ++k) ClassifyTarget(surf[k], c.rt[k]);
  if (!c.rt[0].ms) c.rs[R_MSMASK] = 0;

  const uint32_t used = (vi->samplers & 0xF0000u) | (pi->samplers & 0xFFFFu);
  for (int b = 0; b < kSamplers; ++b) {
    if (!(used >> b & 1)) continue;
    const DWORD stage = b < 16 ? static_cast<DWORD>(b) : D3DVERTEXTEXTURESAMPLER0 + (b - 16);
    IDirect3DBaseTexture9* t = nullptr;
    Call<S_GetTexture>(d, stage, &t);
    if (!t) continue;
    Tex& x = c.tex[b];
    const D3DRESOURCETYPE type = t->GetType();
    D3DSURFACE_DESC sd = {};
    D3DVOLUME_DESC vd = {};
    HRESULT hr = E_FAIL;
    if (type == D3DRTYPE_TEXTURE) {
      x.type = 1;
      hr = static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &sd);
    } else if (type == D3DRTYPE_CUBETEXTURE) {
      x.type = 2;
      hr = static_cast<IDirect3DCubeTexture9*>(t)->GetLevelDesc(0, &sd);
    } else if (type == D3DRTYPE_VOLUMETEXTURE) {
      x.type = 3;
      hr = static_cast<IDirect3DVolumeTexture9*>(t)->GetLevelDesc(0, &vd);
      sd.Format = vd.Format;
      sd.Usage = vd.Usage;
    }
    if (SUCCEEDED(hr)) {
      x.fmt = sd.Format;
      if (sd.Usage & D3DUSAGE_RENDERTARGET) x.flags |= 1;
      if (sd.Usage & D3DUSAGE_DEPTHSTENCIL) x.flags |= 2;
      if (type == D3DRTYPE_TEXTURE && (x.flags & 3)) {  // hazard: its level 0 is a bound target
        IDirect3DSurface9* l0 = nullptr;
        if (SUCCEEDED(static_cast<IDirect3DTexture9*>(t)->GetSurfaceLevel(0, &l0)) && l0) {
          for (int k = 0; k < 5; ++k)
            if (surf[k] == l0) {
              x.hazard = static_cast<uint8_t>(1 + k);
              c.rt[k].tex = 1;
            }
          l0->Release();
        }
      }
    }
    if (b < 16) {
      DWORD bias = 0, mag = 0;
      Call<S_GetSamplerState>(d, stage, D3DSAMP_MIPMAPLODBIAS, &bias);
      Call<S_GetSamplerState>(d, stage, D3DSAMP_MAGFILTER, &mag);
      if (bias == kGet4 && mag == D3DTEXF_POINT) x.flags |= 4;
    }
    t->Release();
  }
  for (int k = 0; k < 5; ++k)
    if (surf[k]) surf[k]->Release();

  if (vi->bools) {
    BOOL b[16] = {};
    Call<S_GetVertexShaderConstantB>(d, 0u, b, 16u);
    for (int k = 0; k < 16; ++k)
      if (b[k] && (vi->bools >> k & 1)) c.vsb |= 1u << k;
  }
  if (pi->bools) {
    BOOL b[16] = {};
    Call<S_GetPixelShaderConstantB>(d, 0u, b, 16u);
    for (int k = 0; k < 16; ++k)
      if (b[k] && (pi->bools >> k & 1)) c.psb |= 1u << k;
  }
  return true;
}

void NoteDraw(IDirect3DDevice9* d, D3DPRIMITIVETYPE prim) {
  EnterCriticalSection(&g_cs);
  ++g_rs.draws;
  Combo c;
  if (!BuildCombo(d, prim, c)) {
    ++g_rs.fixed;
  } else {
    const uint64_t h = Fnv(&c, sizeof c);
    if (g_replay && !g_replayed.empty() && !g_replayed.count(h)) ++g_rs.missed_replay;
    if (g_rec.combos.size() < kMaxCombos && g_rec.combos.insert(h).second) {
      ++g_rs.fresh;
      g_rec.shaders.insert({c.vs, c.ps});
      Put(g_rec.pending, kTagCombo, &c, sizeof c);
      auto dv = g_derivs.find(c.ps);
      if (dv != g_derivs.end() && g_rec.derived.emplace(c.ps, dv->second).second)
        Put(g_rec.pending, kTagDerived, &dv->second, sizeof dv->second);
    } else if (g_rec.combos.size() >= kMaxCombos && !g_rec.full_logged) {
      g_rec.full_logged = true;
      ss_log("[pipewarm] record: %s is full (%u combinations)", g_rec.level.c_str(), static_cast<unsigned>(kMaxCombos));
    }
  }
  LeaveCriticalSection(&g_cs);
}

HRESULT ENTRY_STD DrawHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT a, UINT b) {
  NoteDraw(d, t);
  return Call<S_DrawPrimitive>(d, t, a, b);
}
HRESULT ENTRY_STD DrawIndexedHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT a, UINT b, UINT c, UINT e, UINT f) {
  NoteDraw(d, t);
  return Call<S_DrawIndexedPrimitive>(d, t, a, b, c, e, f);
}
HRESULT ENTRY_STD DrawUpHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT a, const void* p, UINT s) {
  NoteDraw(d, t);
  return Call<S_DrawPrimitiveUP>(d, t, a, p, s);
}
HRESULT ENTRY_STD DrawIndexedUpHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT a, UINT b, UINT c, const void* i,
                                    D3DFORMAT f, const void* v, UINT s) {
  NoteDraw(d, t);
  return Call<S_DrawIndexedPrimitiveUP>(d, t, a, b, c, i, f, v, s);
}

// ------------------------------------------------------------------------------------------------ the level name
using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
volatile uintptr_t g_prev_cfa = 0;
HMODULE g_self = nullptr;

void SeeLevelPath(const char* name) {
  if (!name) return;
  char low[300];
  size_t n = 0;
  for (; name[n] && n < sizeof low - 1; ++n) low[n] = name[n] == '\\' ? '/' : static_cast<char>(tolower(static_cast<unsigned char>(name[n])));
  low[n] = 0;
  const char* p = strstr(low, "environments/levels/");
  if (!p) return;
  p += 20;
  char lv[48];
  size_t k = 0;
  while (p[k] && p[k] != '/' && p[k] != '.' && k < sizeof lv - 1) {
    const char ch = p[k];
    if (!(isalnum(static_cast<unsigned char>(ch)) || ch == '_')) return;
    lv[k] = ch;
    ++k;
  }
  lv[k] = 0;
  if (!k) return;
  const int st = ss_game_state;
  EnterCriticalSection(&g_cs);
  const bool changed = strcmp(g_level, lv) != 0;
  if (changed) {
    strcpy(g_level, lv);
    InterlockedIncrement(&g_level_gen);
  }
  g_level_state = st;
  LeaveCriticalSection(&g_cs);
  if (changed || g_level_logs < 3) {
    ++g_level_logs;
    ss_log("[pipewarm] level %s (file %s, game state %d)", lv, name, st);
  }
}

HANDLE WINAPI CreateFileAHook(LPCSTR name, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t) {
  SeeLevelPath(name);
  return reinterpret_cast<CreateFileAFn>(g_prev_cfa)(name, a, s, sa, c, f, t);
}

bool InOurModule(uintptr_t v) {
  HMODULE m = nullptr;
  return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(v), &m) && m == g_self;
}

// The exe's CreateFileA import slot (.idata, 0xB9B1A8): hooked when it holds something outside this DLL (the
// protection may refill the table after start; overlay.cpp may chain on top of us, or we on top of it). One minute.
DWORD WINAPI CreateFileThread(void*) {
  const uintptr_t slot = 0xB9B1A8;
  const DWORD t0 = GetTickCount();
  bool logged = false, in_chain = false;
  const uintptr_t mine = reinterpret_cast<uintptr_t>(&CreateFileAHook);
  while (GetTickCount() - t0 < 60000) {
    const uintptr_t v = *reinterpret_cast<volatile uintptr_t*>(slot);
    // Hook when the slot holds the system's entry (first time, or refilled), or overlay.cpp's hook before we are in.
    if (v && v != mine && (!InOurModule(v) || !in_chain)) {
      in_chain = true;
      g_prev_cfa = v;
      if (ss_write_word(reinterpret_cast<void*>(slot), reinterpret_cast<uintptr_t>(&CreateFileAHook))) {
        if (!logged) ss_log("[pipewarm] CreateFileA import hooked (level names)");
        logged = true;
      }
    }
    Sleep(50);
  }
  return 0;
}

// ------------------------------------------------------------------------------------------------ the replay
struct Replay {
  std::string level;
  std::vector<Combo> combos;
  size_t needed = 0;  // distinct shaders the combinations use
  long kept0 = 0;     // g_kept when the record was read
  size_t next = 0;
  bool active = false, finished = false, prepared = false;
  std::vector<uint64_t> need_vs, need_ps;  // distinct shaders of the combinations (a variant's game shader for it)
  std::unordered_map<uint64_t, Derived> derived;           // the record's variants
  std::unordered_map<uint64_t, IDirect3DPixelShader9*> made;  // variant hash -> what its part gave (or the game's shader)
  int variants = 0;                                         // variants our parts made for the replay
  LARGE_INTEGER wait0 = {};               // prepared (the load's first present with the level known)
  std::unordered_map<uint64_t, IDirect3DVertexDeclaration9*> decls;
  std::unordered_map<uint64_t, IDirect3DBaseTexture9*> managed;  // dummy textures (managed pool), kept to the end
  IDirect3DVertexBuffer9* vb = nullptr;
  IDirect3DIndexBuffer9* ib = nullptr;
  LARGE_INTEGER t0 = {};
  double ms_ours = 0;
  int slices = 0, drawn = 0, failed = 0, skipped = 0, start_state = -1;
  unsigned va0 = 0;
  std::unordered_set<std::string> fail_logged;
} g_rp;

// Per slice: default-pool objects (released at the slice's end, so a Reset between slices finds none).
struct Slice {
  std::unordered_map<uint64_t, IDirect3DSurface9*> targets;     // key: fmt, ms, msq, depth, as texture
  std::unordered_map<uint64_t, IDirect3DBaseTexture9*> owners;  // the textures behind texture targets
  std::unordered_map<uint64_t, IDirect3DBaseTexture9*> textures;  // default-pool dummies (RT / DS usage)
};

void FailOnce(const char* what, uint32_t fmt, HRESULT hr) {
  char k[64];
  snprintf(k, sizeof k, "%s %u", what, fmt);
  if (g_rp.fail_logged.insert(k).second && g_rp.fail_logged.size() < 40)
    ss_log("[pipewarm] replay: %s format %u failed (0x%08lx)", what, fmt, static_cast<unsigned long>(hr));
}

// A 16x16 target of the recorded format; as a texture's level 0 when a sampler reads it (*owner set).
IDirect3DSurface9* GetTarget(IDirect3DDevice9* d, Slice& sl, const Target& t, bool depth, bool as_tex,
                             IDirect3DBaseTexture9** owner) {
  const uint64_t key = uint64_t(t.fmt) | uint64_t(t.ms) << 32 | uint64_t(t.msq) << 40 | uint64_t(depth) << 48 |
                       uint64_t(as_tex) << 49;
  auto it = sl.targets.find(key);
  if (it != sl.targets.end()) {
    if (owner) *owner = sl.owners.count(key) ? sl.owners[key] : nullptr;
    return it->second;
  }
  IDirect3DSurface9* s = nullptr;
  IDirect3DTexture9* tex = nullptr;
  HRESULT hr = E_FAIL;
  const D3DFORMAT f = static_cast<D3DFORMAT>(t.fmt);
  const auto ms = static_cast<D3DMULTISAMPLE_TYPE>(t.ms);
  if (!as_tex) {
    hr = depth ? Call<S_CreateDepthStencilSurface>(d, 16u, 16u, f, ms, static_cast<DWORD>(t.msq), FALSE, &s, static_cast<HANDLE*>(nullptr))
               : Call<S_CreateRenderTarget>(d, 16u, 16u, f, ms, static_cast<DWORD>(t.msq), FALSE, &s, static_cast<HANDLE*>(nullptr));
  }
  if ((FAILED(hr) || !s) && !t.ms) {  // as a texture (asked for, or the surface was refused)
    s = nullptr;
    hr = Call<S_CreateTexture>(d, 16u, 16u, 1u, static_cast<DWORD>(depth ? D3DUSAGE_DEPTHSTENCIL : D3DUSAGE_RENDERTARGET), f,
                               D3DPOOL_DEFAULT, &tex, static_cast<HANDLE*>(nullptr));
    if (SUCCEEDED(hr) && tex) {
      tex->GetSurfaceLevel(0, &s);
      sl.owners[key] = tex;
    }
  }
  if (!s) FailOnce(depth ? "depth target" : "colour target", t.fmt, hr);
  sl.targets[key] = s;
  if (owner) *owner = sl.owners.count(key) ? sl.owners[key] : nullptr;
  return s;
}

IDirect3DBaseTexture9* GetDummy(IDirect3DDevice9* d, Slice& sl, const Tex& x) {
  const bool def = x.flags & 3;
  const uint64_t key = uint64_t(x.fmt) | uint64_t(x.type) << 32 | uint64_t(x.flags & 3) << 40;
  auto& pool = def ? sl.textures : g_rp.managed;
  auto it = pool.find(key);
  if (it != pool.end()) return it->second;
  const D3DFORMAT f = static_cast<D3DFORMAT>(x.fmt);
  const DWORD usage = (x.flags & 1) ? D3DUSAGE_RENDERTARGET : (x.flags & 2) ? D3DUSAGE_DEPTHSTENCIL : 0;
  const D3DPOOL p = def ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED;
  IDirect3DBaseTexture9* out = nullptr;
  HRESULT hr = E_FAIL;
  if (x.type == 1) {
    IDirect3DTexture9* t = nullptr;
    hr = Call<S_CreateTexture>(d, 4u, 4u, 1u, usage, f, p, &t, static_cast<HANDLE*>(nullptr));
    out = t;
  } else if (x.type == 2) {
    IDirect3DCubeTexture9* t = nullptr;
    hr = Call<S_CreateCubeTexture>(d, 4u, 1u, usage, f, p, &t, static_cast<HANDLE*>(nullptr));
    out = t;
  } else if (x.type == 3) {
    IDirect3DVolumeTexture9* t = nullptr;
    hr = Call<S_CreateVolumeTexture>(d, 4u, 4u, 4u, 1u, usage, f, p, &t, static_cast<HANDLE*>(nullptr));
    out = t;
  }
  if (FAILED(hr)) {
    out = nullptr;
    FailOnce("texture", x.fmt, hr);
  }
  pool[key] = out;
  return out;
}

// The game's shader object for a hash (held for the session, see Keep), or null.
IUnknown* HeldShader(uint64_t h, bool vs) {
  EnterCriticalSection(&g_cs);
  auto it = g_held.find(h);
  IUnknown* s = it != g_held.end() && it->second.vs == vs ? it->second.obj : nullptr;
  LeaveCriticalSection(&g_cs);
  return s;
}

// One combination: bound and drawn once (a degenerate primitive into 16x16 targets).
// A combination's pixel shader: the held one, or for a variant of ours (made otherwise only in the race) the variant
// its part makes now of the held game shader with today's settings (the game's shader itself when they make none).
IDirect3DPixelShader9* ReplayPs(IDirect3DDevice9* d, uint64_t h) {
  if (auto* ps = static_cast<IDirect3DPixelShader9*>(HeldShader(h, false))) return ps;
  auto dv = g_rp.derived.find(h);
  if (dv == g_rp.derived.end()) return nullptr;
  auto m = g_rp.made.find(h);
  if (m != g_rp.made.end()) return m->second;
  auto* orig = static_cast<IDirect3DPixelShader9*>(HeldShader(dv->second.original, false));
  void* v = nullptr;
  if (orig) {
    if (dv->second.maker == 1) v = ss_lodfade_variant(d, orig);
    else if (dv->second.maker == 2) v = ss_shadowsmooth_variant(d, orig);
    else if (dv->second.maker == 3) v = ss_deband_variant(d, orig);
  }
  if (v) ++g_rp.variants;
  return g_rp.made[h] = v ? static_cast<IDirect3DPixelShader9*>(v) : orig;
}

bool ReplayOne(IDirect3DDevice9* d, Slice& sl, const Combo& c) {
  auto* vs = static_cast<IDirect3DVertexShader9*>(HeldShader(c.vs, true));
  auto* ps = ReplayPs(d, c.ps);
  if (!vs || !ps) return false;
  // Targets: the ones a sampler reads as a texture (hazard) are made as textures.
  IDirect3DBaseTexture9* owner[5] = {};
  IDirect3DSurface9* tg[5] = {};
  for (int k = 0; k < 5; ++k) {
    if (!c.rt[k].fmt) continue;
    tg[k] = GetTarget(d, sl, c.rt[k], k == 4, c.rt[k].tex != 0, &owner[k]);
    if (!tg[k] && k == 0) return false;
  }
  for (int k = 0; k < 4; ++k) Call<S_SetRenderTarget>(d, static_cast<DWORD>(k), tg[k]);
  Call<S_SetDepthStencilSurface>(d, tg[4]);
  D3DVIEWPORT9 vp = {0, 0, 16, 16, 0.0f, 1.0f};
  Call<S_SetViewport>(d, &vp);

  if (c.ndecl) {
    const uint64_t h = Fnv(c.decl, sizeof(D3DVERTEXELEMENT9) * c.ndecl);
    auto it = g_rp.decls.find(h);
    IDirect3DVertexDeclaration9* decl = nullptr;
    if (it == g_rp.decls.end()) {
      D3DVERTEXELEMENT9 e[kMaxDecl + 1];
      memcpy(e, c.decl, sizeof(D3DVERTEXELEMENT9) * c.ndecl);
      e[c.ndecl] = D3DDECL_END();
      Call<S_CreateVertexDeclaration>(d, static_cast<const D3DVERTEXELEMENT9*>(e), &decl);
      g_rp.decls[h] = decl;
    } else {
      decl = it->second;
    }
    if (!decl) return false;
    Call<S_SetVertexDeclaration>(d, decl);
  } else {
    Call<S_SetFVF>(d, c.fvf);
  }
  // Streams: the dummy buffer with each stream's stride; the instancing frequencies as recorded.
  UINT stride[4] = {};
  for (int k = 0; k < c.ndecl; ++k) {
    static const UINT kSize[] = {4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 4, 4, 8};
    const auto& e = c.decl[k];
    if (e.Stream < 4 && e.Type < 17) stride[e.Stream] = std::max<UINT>(stride[e.Stream], e.Offset + kSize[e.Type]);
  }
  for (int s = 0; s < 4; ++s) {
    if (c.ndecl ? stride[s] != 0 : s == 0)
      Call<S_SetStreamSource>(d, static_cast<UINT>(s), g_rp.vb, 0u, c.ndecl ? (stride[s] + 3) & ~3u : 64u);
    Call<S_SetStreamSourceFreq>(d, static_cast<UINT>(s), c.freq[s] ? c.freq[s] : 1u);
  }
  if (c.freq[1] || c.freq[2] || c.freq[3]) Call<S_SetStreamSourceFreq>(d, 0u, static_cast<UINT>(D3DSTREAMSOURCE_INDEXEDDATA | 1u));
  Call<S_SetIndices>(d, g_rp.ib);

  Call<S_SetVertexShader>(d, vs);
  Call<S_SetPixelShader>(d, ps);
  for (int k = 0; k < kNumRs; ++k) Call<S_SetRenderState>(d, kRs[k], c.rs[k]);
  if (!c.rs[R_PSIZE]) Call<S_SetRenderState>(d, D3DRS_POINTSIZE, 0x3f800000u);  // 1.0
  Call<S_SetRenderState>(d, D3DRS_ZENABLE, static_cast<DWORD>(D3DZB_FALSE));
  Call<S_SetRenderState>(d, D3DRS_ZWRITEENABLE, static_cast<DWORD>(FALSE));
  Call<S_SetRenderState>(d, D3DRS_STENCILENABLE, static_cast<DWORD>(FALSE));
  Call<S_SetRenderState>(d, D3DRS_CULLMODE, static_cast<DWORD>(D3DCULL_NONE));
  for (int k = 0; k < 6; ++k) {
    static const float kPlane[4] = {0, 0, 0, 1};
    if (k < c.clip) Call<S_SetClipPlane>(d, static_cast<DWORD>(k), kPlane);
  }
  Call<S_SetRenderState>(d, D3DRS_CLIPPLANEENABLE, static_cast<DWORD>((1u << c.clip) - 1));
  BOOL vb[16], pb[16];
  for (int k = 0; k < 16; ++k) {
    vb[k] = (c.vsb >> k) & 1;
    pb[k] = (c.psb >> k) & 1;
  }
  Call<S_SetVertexShaderConstantB>(d, 0u, static_cast<const BOOL*>(vb), 16u);
  Call<S_SetPixelShaderConstantB>(d, 0u, static_cast<const BOOL*>(pb), 16u);

  for (int b = 0; b < kSamplers; ++b) {
    const Tex& x = c.tex[b];
    const DWORD stage = b < 16 ? static_cast<DWORD>(b) : D3DVERTEXTEXTURESAMPLER0 + (b - 16);
    IDirect3DBaseTexture9* t = nullptr;
    if (x.hazard) t = owner[x.hazard - 1];
    else if (x.type) t = GetDummy(d, sl, x);
    Call<S_SetTexture>(d, stage, t);
    if (b < 16) {
      Call<S_SetSamplerState>(d, stage, D3DSAMP_MIPMAPLODBIAS, (x.flags & 4) ? kGet4 : 0u);
      if (x.flags & 4) Call<S_SetSamplerState>(d, stage, D3DSAMP_MAGFILTER, static_cast<DWORD>(D3DTEXF_POINT));
    }
  }
  return SUCCEEDED(Call<S_DrawIndexedPrimitive>(d, static_cast<D3DPRIMITIVETYPE>(c.prim), 0, 0u, 4u, 0u, 1u));
}

// DXVK's compiler threads (named dxvk-shader-*): their CPU time, to see when the optimized pipelines are done.
using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
double DxvkWorkerCpuMs(int* threads) {
  static auto gtd = reinterpret_cast<GetThreadDescriptionFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
  *threads = 0;
  if (!gtd) return -1;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return -1;
  THREADENTRY32 te = {};
  te.dwSize = sizeof te;
  double ms = 0;
  const DWORD pid = GetCurrentProcessId();
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid) continue;
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
    if (!h) continue;
    PWSTR name = nullptr;
    if (SUCCEEDED(gtd(h, &name)) && name) {
      if (!wcsncmp(name, L"dxvk-shader", 11)) {
        FILETIME a, b, k, u;
        if (GetThreadTimes(h, &a, &b, &k, &u)) {
          ++*threads;
          ms += (double((uint64_t(k.dwHighDateTime) << 32) | k.dwLowDateTime) +
                 double((uint64_t(u.dwHighDateTime) << 32) | u.dwLowDateTime)) / 10000.0;
        }
      }
      LocalFree(name);
    }
    CloseHandle(h);
  }
  CloseHandle(snap);
  return ms;
}

DWORD WINAPI WatchWorkers(void*) {
  LARGE_INTEGER t0;
  QueryPerformanceCounter(&t0);
  int n = 0;
  const double c0 = DxvkWorkerCpuMs(&n);
  if (c0 < 0) {
    ss_log("[pipewarm] DXVK's compiler threads cannot be watched (no thread names)");
    return 0;
  }
  double last = c0;
  int quiet = 0;
  while (MsSince(t0) < 180000) {
    Sleep(100);
    const double c = DxvkWorkerCpuMs(&n);
    quiet = (c - last < 10.0) ? quiet + 1 : 0;  // under 10 ms of CPU in 100 ms over all its threads
    last = c;
    if (quiet >= 5) break;
  }
  ss_log("[pipewarm] DXVK's %d compiler threads idle %.1f s after the replay (%.0f ms of their CPU since); free address space %u MB",
         n, (MsSince(t0) - 500) / 1000.0, last - c0, FreeVaMb());
  Sleep(15000);
  ss_log("[pipewarm] 15 s later: free address space %u MB", FreeVaMb());
  return 0;
}

void FinishReplay(IDirect3DDevice9* d) {
  // Wait for DXVK's CS thread (it makes the pipelines as it executes our draws), the load screen covers it. "Pre-compile
  // all tracks" waits for it across presents instead (the menu stays drawn).
  LARGE_INTEGER w0;
  QueryPerformanceCounter(&w0);
  IDirect3DQuery9* q = nullptr;
  bool waited = false;
  if (!g_ca_owns && SUCCEEDED(Call<S_CreateQuery>(d, D3DQUERYTYPE_EVENT, &q)) && q) {
    q->Issue(D3DISSUE_END);
    while (MsSince(w0) < 30000) {
      if (q->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_OK) {
        waited = true;
        break;
      }
      Sleep(1);
    }
    q->Release();
  }
  const double wait_ms = MsSince(w0);
  for (auto& e : g_rp.decls) if (e.second) e.second->Release();
  for (auto& e : g_rp.managed) if (e.second) e.second->Release();
  if (g_rp.vb) g_rp.vb->Release();
  if (g_rp.ib) g_rp.ib->Release();
  ss_log("[pipewarm] replay %s: %d of %zu combinations drawn (%d failed, %d without their shaders; %d variants of ours made "
         "for it), %d slices, %.0f ms on the "
         "render thread, %.0f ms wall since the start; DXVK's CS thread %s %.0f ms later (state %d -> %d); free address space %u -> %u MB",
         g_rp.level.c_str(), g_rp.drawn, g_rp.combos.size(), g_rp.failed, g_rp.skipped, g_rp.variants, g_rp.slices, g_rp.ms_ours,
         MsSince(g_rp.t0) - wait_ms, waited ? "done" : g_ca_owns ? "checked later," : "NOT done", wait_ms, g_rp.start_state, static_cast<int>(ss_game_state),
         g_rp.va0, FreeVaMb());
  EnterCriticalSection(&g_cs);
  for (const Combo& c : g_rp.combos) g_replayed.insert(Fnv(&c, sizeof c));
  LeaveCriticalSection(&g_cs);
  if (g_ca_owns) {
    g_ca_drawn += g_rp.drawn;
    g_ca_skipped += g_rp.skipped;
  }
  const std::string level = g_rp.level;
  g_rp = Replay{};
  g_rp.level = level;
  g_rp.finished = true;
  if (g_ca_owns) return;
  HANDLE t = CreateThread(nullptr, 0, WatchWorkers, nullptr, 0, nullptr);
  if (t) CloseHandle(t);
}

// The level's record is read when the load's level is known; the replay starts once the game has made the shaders
// the record names (it makes a level's shaders on its loader thread during the load), see ReplayReady.
bool PrepareReplay(const char* level) {
  g_rp = Replay{};
  g_rp.level = level;
  g_rp.finished = true;  // whatever happens, once per load
  if (!ReadLevel(level, &g_rp.combos, nullptr, nullptr, &g_rp.derived) || g_rp.combos.empty()) {
    ss_log("[pipewarm] replay: no record for level %s", level);
    return false;
  }
  std::unordered_set<uint64_t> v, p;
  for (const Combo& c : g_rp.combos) {
    v.insert(c.vs);
    auto dv = g_rp.derived.find(c.ps);
    p.insert(dv != g_rp.derived.end() ? dv->second.original : c.ps);
  }
  g_rp.need_vs.assign(v.begin(), v.end());
  g_rp.need_ps.assign(p.begin(), p.end());
  g_rp.needed = v.size() + p.size();
  g_rp.prepared = true;
  g_rp.finished = false;
  EnterCriticalSection(&g_cs);
  g_rp.kept0 = g_kept;
  LeaveCriticalSection(&g_cs);
  QueryPerformanceCounter(&g_rp.wait0);
  return true;
}

// Ready: 95% of the shaders the record names are held, or 1.5 s passed without a new one after the load's burst began,
// or 4 s passed with none (a level whose shaders are held already), or 20 s; when the load ends first the replay starts
// then with what is held. Measured (Airport, warm): the game makes the level's shaders in a burst from ~0.8 to ~1.8 s
// into the load, which ends ~0.7 s later, but the burst has gaps: a 400 ms quiet once fired with 284 of 666 held (D1).
// The replay then runs on in slices whatever the state (the race intro and its continue cards).
bool ReplayReady(size_t* have) {
  *have = 0;
  EnterCriticalSection(&g_cs);
  for (uint64_t h : g_rp.need_vs) *have += g_held.count(h);
  for (uint64_t h : g_rp.need_ps) *have += g_held.count(h);
  const LARGE_INTEGER last = g_last_keep;
  const bool burst = g_kept > g_rp.kept0;  // the game made some of the level's shaders in this load
  LeaveCriticalSection(&g_cs);
  const double since = MsSince(g_rp.wait0);
  return *have * 100 >= g_rp.needed * 95 || (burst && MsSince(last) > 1500) || (!burst && since > 4000) || since > 20000;
}

bool StartReplay(IDirect3DDevice9* d, size_t have) {
  const char* level = g_rp.level.c_str();
  g_rp.finished = true;
  ss_log("[pipewarm] replay %s: %zu of the %zu shaders the record names are held (%.0f ms after the record was read; "
         "%ld shaders made, %ld kept so far)", level, have, g_rp.needed, MsSince(g_rp.wait0), g_creates, g_kept);
  if (FAILED(Call<S_CreateVertexBuffer>(d, 65536u, static_cast<DWORD>(D3DUSAGE_WRITEONLY), 0u, D3DPOOL_MANAGED, &g_rp.vb,
                                        static_cast<HANDLE*>(nullptr))) ||
      FAILED(Call<S_CreateIndexBuffer>(d, 64u, static_cast<DWORD>(D3DUSAGE_WRITEONLY), D3DFMT_INDEX16, D3DPOOL_MANAGED,
                                       &g_rp.ib, static_cast<HANDLE*>(nullptr)))) {
    ss_log("[pipewarm] replay: dummy buffers refused");
    if (g_rp.vb) g_rp.vb->Release();
    g_rp.vb = nullptr;
    return false;
  }
  void* p = nullptr;
  if (SUCCEEDED(g_rp.vb->Lock(0, 0, &p, 0)) && p) { memset(p, 0, 65536); g_rp.vb->Unlock(); }
  if (SUCCEEDED(g_rp.ib->Lock(0, 0, &p, 0)) && p) { memset(p, 0, 64); g_rp.ib->Unlock(); }
  g_rp.active = true;
  g_rp.finished = false;
  g_rp.start_state = ss_game_state;
  g_rp.va0 = FreeVaMb();
  QueryPerformanceCounter(&g_rp.t0);
  ss_log("[pipewarm] replay %s: %zu combinations at game state %d; free address space %u MB", level,
         g_rp.combos.size(), g_rp.start_state, g_rp.va0);
  return true;
}

// max_draws: at most that many combinations drawn (PRE-COMPILE ALL TRACKS: the menu keeps running between slices).
void ReplaySlice(IDirect3DDevice9* d, double budget_ms, size_t max_draws = SIZE_MAX) {
  LARGE_INTEGER s0;
  QueryPerformanceCounter(&s0);
  IDirect3DStateBlock9* sb = nullptr;
  if (FAILED(Call<S_CreateStateBlock>(d, D3DSBT_ALL, &sb)) || !sb) {
    ss_log("[pipewarm] replay: no state block; stopped");
    g_rp.next = g_rp.combos.size();
    FinishReplay(d);
    return;
  }
  IDirect3DSurface9* rt[4] = {};
  IDirect3DSurface9* ds = nullptr;
  for (int k = 0; k < 4; ++k) Call<S_GetRenderTarget>(d, static_cast<DWORD>(k), &rt[k]);
  Call<S_GetDepthStencilSurface>(d, &ds);
  Call<S_BeginScene>(d);
  Slice sl;
  while (g_rp.next < g_rp.combos.size()) {
    const Combo& c = g_rp.combos[g_rp.next++];
    // A variant of ours is never held: its game shader is (ReplayPs makes the variant from it).
    auto dv = g_rp.derived.find(c.ps);
    const bool ps_ok = HeldShader(c.ps, false) || (dv != g_rp.derived.end() && HeldShader(dv->second.original, false));
    if (!HeldShader(c.vs, true) || !ps_ok) { ++g_rp.skipped; continue; }
    if (ReplayOne(d, sl, c)) ++g_rp.drawn;
    else ++g_rp.failed;
    if (--max_draws == 0) break;
    if ((g_rp.next & 15) == 0 && MsSince(s0) > budget_ms) break;
  }
  Call<S_EndScene>(d);
  for (int k = 0; k < 4; ++k) Call<S_SetRenderTarget>(d, static_cast<DWORD>(k), rt[k]);
  Call<S_SetDepthStencilSurface>(d, ds);
  sb->Apply();  // after the targets: SetRenderTarget(0) resets the viewport, the block puts the game's back
  sb->Release();
  for (int k = 0; k < 4; ++k) if (rt[k]) rt[k]->Release();
  if (ds) ds->Release();
  for (auto& e : sl.targets) if (e.second) e.second->Release();
  for (auto& e : sl.owners) if (e.second) e.second->Release();
  for (auto& e : sl.textures) if (e.second) e.second->Release();
  ++g_rp.slices;
  g_rp.ms_ours += MsSince(s0);
  if (g_rp.next >= g_rp.combos.size()) FinishReplay(d);
}


void** g_vt = nullptr;
bool g_on = false;
double g_slice_ms = 40;

// A switch from the environment (GetEnvironmentVariable: env.NAME lines of the conf count too), else the conf key.
std::string Flag(const char* env, const char* key) {
  char buf[64];
  const DWORD n = GetEnvironmentVariableA(env, buf, sizeof buf);
  return n && n < sizeof buf ? std::string(buf) : ss_conf(key);
}

bool HasEnv(const char* name) { return GetEnvironmentVariableA(name, nullptr, 0) > 0; }

// The folder of a module (with the backslash): this DLL's, or the exe's (null).
std::wstring FolderOf(HMODULE m) {
  wchar_t path[MAX_PATH * 2] = {};
  if (!GetModuleFileNameW(m, path, MAX_PATH * 2)) return L"";
  wchar_t* slash = wcsrchr(path, L'\\');
  if (!slash) return L"";
  slash[1] = 0;
  return path;
}

void SetDirs() {
  if (!g_dir.empty()) return;
  HMODULE self = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&SetDirs), &self))
    return;
  const std::wstring d = FolderOf(self);
  if (d.empty()) return;
  g_dir = d + L"pipeline-warmup\\";
  g_local = g_dir + L"local\\";
}

// pipeline_warmup = off | on | record | both (replay = on). Empty or auto: on when there are records.
int ResolveMode(bool* have_records) {
  SetDirs();
  const bool recs = !g_dir.empty() && !RecordLevels(g_dir, g_local).empty();
  if (have_records) *have_records = recs;
  std::string v = Flag("SS_PIPELINE_WARMUP", "pipeline_warmup");
  for (char& c : v) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  if (v == "record") return M_RECORD;
  if (v == "both") return M_BOTH;
  if (v == "on" || v == "replay" || v == "true" || v == "1") return M_REPLAY;
  if (v.empty() || v == "auto") return recs ? M_REPLAY : M_OFF;
  return M_OFF;
}

// The PRE-COMPILE row: replay at the next level load, or not. Live while the create hooks are in.
bool ApplyRow(const char* key, const char* value) {
  if (!g_on || !g_cs_ready) return false;
  g_replay = !strcmp(value, "on") || !strcmp(value, "both");
  ss_log("[pipewarm] PRE-COMPILE %s from the menu (from the next level load)", g_replay ? "on" : "off");
  return true;
}

// ------------------------------------------------------------------------------------------------ pre-compile all
// The PRE-COMPILE ALL TRACKS row (main menu only): every level record is replayed now, so no track's first load needs
// the long pre-compile and the driver's pipeline cache holds every track. The replay draws with the game's shader
// objects, and the game makes a level's shaders only while that level loads, so here they come from the player's own
// game files: the archives under <game>\data are read (arkscan.cpp), every D3D9 program in their shader members is
// hashed, and the ones the records name are made on the device through DXVK's own create slots (the same byte code
// gives the same DXVK shader module, so the pipelines are the ones the race uses). No level is loaded, nothing is
// written to the game folder or the save, nothing is unlocked: it works on a fresh save. Steps, one per present, with
// the progress on the row: read the archives (worker thread), make the shaders, replay each level, wait for DXVK's CS
// thread. Stops when the game leaves the main menu, when free address space falls under kCaMinVaMb, or when the row is
// pressed again.
constexpr unsigned kCaMinVaMb = 300;
// Combinations drawn per present, and the next slice only once DXVK has built the last one (an event query): one
// slice of a whole level (~1000 draws in 40 ms) kept the menu's Present blocked 5-10 s per track on an empty driver
// cache (the window stopped answering); 16 a slice keeps it to a short wait per frame with the progress moving.
constexpr size_t kCaDrawsPerSlice = 16;
struct Found {
  uint64_t hash;
  bool vs;
  std::vector<uint32_t> code;
};
enum CaPhase { CA_IDLE, CA_SCAN, CA_CREATE, CA_LEVEL, CA_WAIT, CA_DONE, CA_STOPPED };
struct CompileAll {
  int phase = CA_IDLE;
  bool request = false;
  std::vector<std::string> levels;
  size_t li = 0;
  HANDLE thread = nullptr;
  volatile LONG arks = 0, arks_done = 0, scan_over = 0, stop = 0;
  // The worker's (read by the render thread after scan_over):
  std::unordered_set<uint64_t> missing;
  std::vector<Found> found;
  arkscan::Stats st;
  uint32_t programs = 0;
  double scan_ms = 0;
  // The render thread's:
  size_t needed = 0, ci = 0;
  int created = 0, create_failed = 0, levels_done = 0;
  IDirect3DQuery9* q = nullptr;
  IDirect3DQuery9* sq = nullptr;  // the last slice's event (CA_LEVEL)
  LARGE_INTEGER t0 = {}, tw = {}, ts = {};
  unsigned va0 = 0;
  std::string why;
} g_ca;
char g_ca_text[80] = "START";

void ListArchives(const std::wstring& dir, int depth, std::vector<std::pair<uint64_t, std::wstring>>& out) {
  if (depth > 6 || out.size() > 4096) return;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    const std::wstring n = fd.cFileName;
    if (n == L"." || n == L"..") continue;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) ListArchives(dir + n + L"\\", depth + 1, out);
      continue;
    }
    if (n.size() > 4 && _wcsicmp(n.c_str() + n.size() - 4, L".ark") == 0)
      out.push_back({(static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow, dir + n});
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}

DWORD WINAPI ScanThread(void*) {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  LARGE_INTEGER t0;
  QueryPerformanceCounter(&t0);
  const std::wstring data = FolderOf(nullptr) + L"data\\";
  std::vector<std::pair<uint64_t, std::wstring>> arks;
  ListArchives(data, 0, arks);
  std::sort(arks.begin(), arks.end());  // small first: the level and sector archives before resident.ark
  InterlockedExchange(&g_ca.arks, static_cast<LONG>(arks.size()));
  for (const auto& a : arks) {
    if (g_ca.missing.empty() || g_ca.stop) break;
    HANDLE f = CreateFileW(a.second.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
      const arkscan::ReadFn rd = [f](uint64_t off, void* dst, size_t n) {
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(off);
        DWORD got = 0;
        return SetFilePointerEx(f, li, nullptr, FILE_BEGIN) && ReadFile(f, dst, static_cast<DWORD>(n), &got, nullptr) &&
               got == n;
      };
      arkscan::ScanArchive(
          rd, a.first,
          [](const uint8_t* p, size_t n) {
            arkscan::ScanPrograms(p, n, [](const uint32_t* c, size_t dw, bool vs) {
              ++g_ca.programs;
              const uint64_t h = arkscan::ProgramHash(c, dw * 4, vs);
              auto it = g_ca.missing.find(h);
              if (it == g_ca.missing.end()) return;
              g_ca.missing.erase(it);
              g_ca.found.push_back(Found{h, vs, std::vector<uint32_t>(c, c + dw)});
            });
          },
          &g_ca.st);
      CloseHandle(f);
    }
    InterlockedIncrement(&g_ca.arks_done);
  }
  g_ca.scan_ms = MsSince(t0);
  ss_log("[pipewarm] pre-compile all: %ld of %zu archives under %ls read in %.1f s (%u members, %u shader members of "
         "%.1f MB, %u unreadable), %u programs in them; %zu of the %zu shaders needed found, %zu not",
         g_ca.arks_done, arks.size(), data.c_str(), g_ca.scan_ms / 1000.0, g_ca.st.members, g_ca.st.fxc,
         g_ca.st.fxc_bytes / 1048576.0, g_ca.st.bad, g_ca.programs, g_ca.found.size(), g_ca.needed, g_ca.missing.size());
  int shown = 0;
  for (uint64_t h : g_ca.missing) {
    if (++shown > 8) break;
    ss_log("[pipewarm] pre-compile all: not in the game files: %016llx", static_cast<unsigned long long>(h));
  }
  InterlockedExchange(&g_ca.scan_over, 1);
  return 0;
}

void CaCleanup() {
  if (g_ca.q) g_ca.q->Release();
  g_ca.q = nullptr;
  if (g_ca.sq) g_ca.sq->Release();
  g_ca.sq = nullptr;
  g_ca.found.clear();
  g_ca.found.shrink_to_fit();
  g_ca.missing.clear();
  g_ca_owns = false;
  g_rp = Replay{};  // nothing prepared: a level load starts its own replay as usual
}

void CaStop(IDirect3DDevice9* d, const char* why) {
  g_ca.why = why;
  if (g_ca.phase == CA_SCAN) {  // the worker ends at the next archive; CompileAllStep finishes the stop then
    InterlockedExchange(&g_ca.stop, 1);
    return;
  }
  if (g_ca.phase == CA_LEVEL && g_rp.active) {
    g_rp.next = g_rp.combos.size();
    FinishReplay(d);
  }
  ss_log("[pipewarm] pre-compile all: %s after %.1f s (%d of %zu tracks done); free address space %u MB", why,
         MsSince(g_ca.t0) / 1000.0, g_ca.levels_done, g_ca.levels.size(), FreeVaMb());
  g_ca.phase = CA_STOPPED;
  CaCleanup();
}

void CaBegin(IDirect3DDevice9* d, int st) {
  if (g_ca.thread) CloseHandle(g_ca.thread);
  g_ca = CompileAll{};
  g_ca_drawn = g_ca_skipped = 0;
  QueryPerformanceCounter(&g_ca.t0);
  if (st != 7 || g_rp.active) {
    g_ca.phase = CA_STOPPED;
    g_ca.why = "IN THE MAIN MENU ONLY";
    return;
  }
  g_ca.levels = RecordLevels(g_dir, g_local);
  if (g_ca.levels.empty()) {
    g_ca.phase = CA_STOPPED;
    g_ca.why = "NO RECORDS";
    return;
  }
  EnterCriticalSection(&g_cs);
  for (uint64_t h : g_wanted)
    if (!g_held.count(h) && !g_variants.count(h)) g_ca.missing.insert(h);
  const size_t held = g_held.size();
  LeaveCriticalSection(&g_cs);
  g_ca.needed = g_ca.missing.size();
  g_ca.va0 = FreeVaMb();
  g_ca_owns = true;
  g_ca.phase = CA_SCAN;
  ss_log("[pipewarm] pre-compile all: %zu tracks, %zu shaders to find in the game files (%zu held already); free "
         "address space %u MB", g_ca.levels.size(), g_ca.needed, held, g_ca.va0);
  g_ca.thread = CreateThread(nullptr, 0, ScanThread, nullptr, 0, nullptr);
  if (!g_ca.thread) InterlockedExchange(&g_ca.scan_over, 1);
}

void CaFinish() {
  ss_log("[pipewarm] pre-compile all: done in %.1f s: %d tracks, %ld combinations drawn (%ld without their shaders), "
         "%d shaders made from the game files (%d refused); free address space %u -> %u MB",
         MsSince(g_ca.t0) / 1000.0, g_ca.levels_done, g_ca_drawn, g_ca_skipped, g_ca.created, g_ca.create_failed, g_ca.va0,
         FreeVaMb());
  g_ca.phase = CA_DONE;
  CaCleanup();
  HANDLE t = CreateThread(nullptr, 0, WatchWorkers, nullptr, 0, nullptr);
  if (t) CloseHandle(t);
}

// The next level with a record and its replay started; CaFinish after the last.
void CaNextLevel(IDirect3DDevice9* d) {
  while (g_ca.li < g_ca.levels.size()) {
    if (FreeVaMb() < kCaMinVaMb) {
      CaStop(d, "STOPPED: LOW MEMORY, RESTART THE GAME");
      return;
    }
    const std::string& level = g_ca.levels[g_ca.li];
    if (PrepareReplay(level.c_str())) {
      size_t have = 0;
      ReplayReady(&have);
      if (StartReplay(d, have)) {
        g_ca.phase = CA_LEVEL;
        return;
      }
    }
    ++g_ca.li;
  }
  CaFinish();
}

// Runs the steps; true while it owns the present (the level-load replay waits).
bool CompileAllStep(IDirect3DDevice9* d, int st) {
  if (g_ca.request) {
    g_ca.request = false;
    if (g_ca.phase >= CA_SCAN && g_ca.phase <= CA_WAIT) {
      if (!g_ca.stop) CaStop(d, "STOPPED");
    } else {
      CaBegin(d, st);
    }
  }
  if (g_ca.phase < CA_SCAN || g_ca.phase > CA_WAIT) return false;
  if (st != 7 && !g_ca.stop) CaStop(d, "STOPPED: LEFT THE MAIN MENU");
  switch (g_ca.phase) {
    case CA_SCAN:
      if (!g_ca.scan_over) return true;
      if (g_ca.thread) CloseHandle(g_ca.thread);
      g_ca.thread = nullptr;
      if (g_ca.stop) {
        g_ca.phase = CA_STOPPED;
        ss_log("[pipewarm] pre-compile all: %s while reading the game files", g_ca.why.c_str());
        CaCleanup();
        return false;
      }
      if (g_ca.found.empty() && g_ca.needed) {
        g_ca.phase = CA_STOPPED;
        g_ca.why = "SHADERS NOT FOUND IN THE GAME FILES";
        ss_log("[pipewarm] pre-compile all: none of the %zu shaders found in the game files: stopped", g_ca.needed);
        CaCleanup();
        return false;
      }
      g_ca.phase = CA_CREATE;
      return true;
    case CA_CREATE: {
      LARGE_INTEGER s0;
      QueryPerformanceCounter(&s0);
      while (g_ca.ci < g_ca.found.size() && MsSince(s0) < g_slice_ms) {
        Found& f = g_ca.found[g_ca.ci++];
        IUnknown* obj = nullptr;
        const auto* code = reinterpret_cast<const DWORD*>(f.code.data());
        const HRESULT hr = f.vs ? Call<S_CreateVertexShader>(d, code, reinterpret_cast<IDirect3DVertexShader9**>(&obj))
                                : Call<S_CreatePixelShader>(d, code, reinterpret_cast<IDirect3DPixelShader9**>(&obj));
        std::vector<uint32_t>().swap(f.code);
        if (FAILED(hr) || !obj) {
          ++g_ca.create_failed;
          continue;
        }
        EnterCriticalSection(&g_cs);
        const bool fresh = g_held.emplace(f.hash, Held{obj, f.vs}).second;
        LeaveCriticalSection(&g_cs);
        if (fresh) ++g_ca.created;
        else obj->Release();  // the game made it meanwhile
      }
      if (g_ca.ci < g_ca.found.size()) return true;
      ss_log("[pipewarm] pre-compile all: %d shaders made from the game files (%d refused) in %.1f s", g_ca.created,
             g_ca.create_failed, MsSince(g_ca.t0) / 1000.0 - g_ca.scan_ms / 1000.0);
      g_ca.found.clear();
      g_ca.found.shrink_to_fit();
      g_ca.li = 0;
      CaNextLevel(d);
      return g_ca.phase == CA_LEVEL;
    }
    case CA_LEVEL:
      if (g_rp.active) {
        // DXVK still building the last slice: no new draws this frame (at most 10 s, then on anyway).
        if (g_ca.sq && g_ca.sq->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE && MsSince(g_ca.ts) < 10000) return true;
        ReplaySlice(d, g_slice_ms, kCaDrawsPerSlice);
        if (g_rp.active) {
          if (!g_ca.sq && (FAILED(Call<S_CreateQuery>(d, D3DQUERYTYPE_EVENT, &g_ca.sq)) || !g_ca.sq)) g_ca.sq = nullptr;
          if (g_ca.sq) g_ca.sq->Issue(D3DISSUE_END);
          QueryPerformanceCounter(&g_ca.ts);
          return true;
        }
      }
      g_ca.q = nullptr;
      if (FAILED(Call<S_CreateQuery>(d, D3DQUERYTYPE_EVENT, &g_ca.q)) || !g_ca.q) g_ca.q = nullptr;
      if (g_ca.q) g_ca.q->Issue(D3DISSUE_END);
      QueryPerformanceCounter(&g_ca.tw);
      g_ca.phase = CA_WAIT;
      return true;
    case CA_WAIT:
      if (g_ca.q && g_ca.q->GetData(nullptr, 0, D3DGETDATA_FLUSH) != S_OK && MsSince(g_ca.tw) < 30000) return true;
      ss_log("[pipewarm] pre-compile all: track %s (%zu of %zu): DXVK's CS thread %s %.0f ms after the last draw; free "
             "address space %u MB", g_ca.levels[g_ca.li].c_str(), g_ca.li + 1, g_ca.levels.size(),
             g_ca.q ? "done" : "not asked,", MsSince(g_ca.tw), FreeVaMb());
      if (g_ca.q) g_ca.q->Release();
      g_ca.q = nullptr;
      ++g_ca.levels_done;
      ++g_ca.li;
      CaNextLevel(d);
      return g_ca.phase == CA_LEVEL;
  }
  return false;
}

const char* CompileAllStatus() {
  switch (g_ca.phase) {
    case CA_SCAN:
      snprintf(g_ca_text, sizeof g_ca_text, "READING GAME FILES %ld/%ld", g_ca.arks_done, g_ca.arks);
      break;
    case CA_CREATE:
      snprintf(g_ca_text, sizeof g_ca_text, "MAKING SHADERS %zu/%zu", g_ca.ci, g_ca.found.size());
      break;
    case CA_LEVEL:
      snprintf(g_ca_text, sizeof g_ca_text, "TRACK %zu/%zu  %zu/%zu", g_ca.li + 1, g_ca.levels.size(), g_rp.next,
               g_rp.combos.size());
      break;
    case CA_WAIT:
      snprintf(g_ca_text, sizeof g_ca_text, "TRACK %zu/%zu  FINISHING", g_ca.li + 1, g_ca.levels.size());
      break;
    case CA_DONE:
      snprintf(g_ca_text, sizeof g_ca_text, "DONE: %d TRACKS", g_ca.levels_done);
      break;
    case CA_STOPPED:
      snprintf(g_ca_text, sizeof g_ca_text, "%s", g_ca.why.c_str());
      break;
    default:
      snprintf(g_ca_text, sizeof g_ca_text, "START");
  }
  return g_ca_text;
}

bool CompileAllAction(const char*, const char*) {
  g_ca.request = true;  // the menu runs on the render thread too: the next present starts (or stops) it
  return true;
}

}  // namespace

bool ss_pipewarm_wanted() {
  static int wanted = -1;
  if (wanted < 0) {
    if (!ss_conf_present() && !HasEnv("SS_PIPELINE_WARMUP")) {
      wanted = 0;
    } else {
      bool recs = false;
      const int mode = ResolveMode(&recs);
      // With records the create hooks go in even when off: the PRE-COMPILE row can switch it on without a restart.
      wanted = mode == M_RECORD || mode == M_BOTH || recs;
    }
  }
  return wanted > 0;
}

// patch.cpp at start, after the conf's env.* lines and before the game makes its Direct3D object: with pre-compile on,
// DXVK gets 4 compiler threads unless the player set numCompilerThreads (DXVK_CONFIG, DXVK_CONFIG_FILE or a dxvk.conf
// beside the game). Measured (Airport, cold): DXVK's default (one per CPU thread, 16) left ~250 MB of free address space
// in the race, 4 left ~440 MB, at the same load time. Read once by DXVK: a change needs a restart.
void ss_pipewarm_start() {
  if (!ss_pipewarm_wanted()) return;
  bool recs = false;
  const int mode = ResolveMode(&recs);
  if ((mode != M_REPLAY && mode != M_BOTH) || !recs) return;
  HMODULE nt = GetModuleHandleA("ntdll.dll");
  if (!(nt && GetProcAddress(nt, "wine_get_version")) && !ss_renderer_dxvk()) {
    ss_log("[pipewarm] Windows' own d3d9 (renderer = native, or no DXVK): pre-compile has nothing to do");
    return;
  }
  char cur[2048] = "";
  const DWORD n = GetEnvironmentVariableA("DXVK_CONFIG", cur, sizeof cur);
  if (n >= sizeof cur) return;
  if (strstr(cur, "numCompilerThreads")) {
    ss_log("[pipewarm] pre-compile on: DXVK_CONFIG sets numCompilerThreads already (%s): kept", cur);
    return;
  }
  // A config file that sets it: DXVK_CONFIG_FILE, or dxvk.conf beside the game.
  std::vector<std::string> files;
  char cf[MAX_PATH] = "";
  const DWORD m = GetEnvironmentVariableA("DXVK_CONFIG_FILE", cf, sizeof cf);
  if (m && m < sizeof cf) files.push_back(cf);
  const std::wstring exe_dir = FolderOf(nullptr);
  files.push_back(std::string(exe_dir.begin(), exe_dir.end()) + "dxvk.conf");
  for (const std::string& path : files) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) continue;
    char line[512];
    bool set = false;
    while (!set && fgets(line, sizeof line, f)) {
      const char* p = line;
      while (*p == ' ' || *p == '\t') ++p;
      set = *p != '#' && strstr(p, "numCompilerThreads");
    }
    fclose(f);
    if (set) {
      ss_log("[pipewarm] pre-compile on: %s sets numCompilerThreads: kept", path.c_str());
      return;
    }
  }
  std::string nv = cur;
  if (!nv.empty()) nv += ";";
  nv += "dxvk.numCompilerThreads = 4";
  SetEnvironmentVariableA("DXVK_CONFIG", nv.c_str());
  ss_log("[pipewarm] pre-compile on: DXVK_CONFIG = %s (4 compiler threads: more free address space in the race)", nv.c_str());
}

void ss_pipewarm_device(void* device) {
  auto* d = static_cast<IDirect3DDevice9*>(device);
  if (!d || !ss_pipewarm_wanted()) return;
  void** vt = *reinterpret_cast<void***>(d);
  if (vt == g_vt) return;
  if (g_vt) {
    ss_log("[pipewarm] a device with another vtable: not hooked");
    return;
  }
  HMODULE owner = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<const char*>(vt[S_DrawIndexedPrimitive]), &owner) || !owner ||
      !GetProcAddress(owner, MAKEINTRESOURCEA(kDxvkAnnotationOrdinal))) {
    ss_log("[pipewarm] the device is not DXVK's: pipeline_warmup does nothing");
    return;
  }
  bool recs = false;
  g_mode = ResolveMode(&recs);
  g_record = g_mode == M_RECORD || g_mode == M_BOTH;
  g_replay = g_mode == M_REPLAY || g_mode == M_BOTH;
  if (const char* s = getenv("SS_PIPEWARM_SLICE_MS")) g_slice_ms = std::max(5.0, atof(s));
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&ss_pipewarm_device), &g_self) ||
      g_dir.empty())
    return;
  if (g_record && !CreateDirectoryW(g_dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
    ss_log("[pipewarm] cannot make %ls (error %lu): off", g_dir.c_str(), GetLastError());
    return;
  }
  InitializeCriticalSection(&g_cs);
  g_cs_ready = true;
  for (int k = 0; k < kNumSlots; ++k) g_orig[k] = vt[k];
  if (g_record) {
    struct { int slot; void* mine; } hooks[] = {{S_DrawPrimitive, reinterpret_cast<void*>(&DrawHook)},
                                                {S_DrawIndexedPrimitive, reinterpret_cast<void*>(&DrawIndexedHook)},
                                                {S_DrawPrimitiveUP, reinterpret_cast<void*>(&DrawUpHook)},
                                                {S_DrawIndexedPrimitiveUP, reinterpret_cast<void*>(&DrawIndexedUpHook)}};
    for (auto& h : hooks)
      if (!ss_write_word(&vt[h.slot], reinterpret_cast<uintptr_t>(h.mine))) {
        ss_log("[pipewarm] cannot write draw slot %d (error %lu): record off", h.slot, GetLastError());
        for (auto& u : hooks) ss_write_word(&vt[u.slot], reinterpret_cast<uintptr_t>(g_orig[u.slot]));
        g_record = false;
        break;
      }
    EnterCriticalSection(&g_cs);
    SwitchRecordLevel("boot");
    LeaveCriticalSection(&g_cs);
  }
  bool creates = false;
  if (recs) {
    LoadWanted();
    creates = ss_write_word(&vt[S_CreateVertexShader], reinterpret_cast<uintptr_t>(&CreateVsHook)) &&
              ss_write_word(&vt[S_CreatePixelShader], reinterpret_cast<uintptr_t>(&CreatePsHook));
    if (!creates) {
      ss_log("[pipewarm] cannot write the shader create slots (error %lu): replay off", GetLastError());
      ss_write_word(&vt[S_CreateVertexShader], reinterpret_cast<uintptr_t>(g_orig[S_CreateVertexShader]));
    }
  }
  if (!creates) g_replay = false;
  g_vt = vt;
  g_on = true;
  HANDLE t = CreateThread(nullptr, 0, CreateFileThread, nullptr, 0, nullptr);
  if (t) CloseHandle(t);
  if (creates) {
    // DXVK only, with records: the rows. PRE-COMPILE is live (from the next level load); its fallback "on" is what an
    // empty key means here (records present).
    ss_menu_register(MenuChoiceRow("pipeline_warmup", "PRE-COMPILE", MENU_PAGE_GRAPHICS, MENU_LIVE, "on",
                                   {{"off", "OFF"}, {"on", "ON"}}, ApplyRow));
    ss_menu_register(MenuActionRow("pipeline_compile_all", "PRE-COMPILE ALL TRACKS", MENU_PAGE_GRAPHICS, CompileAllAction,
                                   CompileAllStatus));
  }
  ss_log("[pipewarm] on: %s%s%s, records in %ls (shipped) and %ls (this machine's), replay slices of %.0f ms; free "
         "address space %u MB",
         g_record ? "record" : "", g_record && g_replay ? " + replay" : g_replay ? "replay" : "",
         !g_record && !g_replay ? "off (the PRE-COMPILE row can switch it on)" : "", g_dir.c_str(), g_local.c_str(),
         g_slice_ms, FreeVaMb());
}

void ss_pipewarm_present(void* device) {
  if (!g_on) return;
  auto* d = static_cast<IDirect3DDevice9*>(device);
  const int st = ss_game_state;
  const bool all = CompileAllStep(d, st);  // first: a load that begins stops it before the load's replay is set up
  if (st != g_last_state) {
    EnterCriticalSection(&g_cs);
    const long made = g_creates, kept = g_kept;
    LeaveCriticalSection(&g_cs);
    ss_log("[pipewarm] game state %d -> %d (level %s); shaders made %ld, held %ld", g_last_state, st, g_level, made, kept);
    if (st == 1 || st == 3) {  // a load begins: the level name comes from its file opens (kept when one came already
      EnterCriticalSection(&g_cs);  // in this load's step, before this present saw the state)
      if (st == 3) strcpy(g_level, "frontend");
      else if (g_level_state != 1) g_level[0] = 0;
      LeaveCriticalSection(&g_cs);
      if (st == 1) {
        g_rp.finished = false;
        g_rp.prepared = false;
      }
    }
    g_last_state = st;
  }
  char level[48];
  EnterCriticalSection(&g_cs);
  strcpy(level, g_level);
  if (g_record && level[0]) SwitchRecordLevel(level);
  if (g_record) FlushRecord();
  LeaveCriticalSection(&g_cs);
  if (g_record) {
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (now - last >= 10000) {
      last = now;
      EnterCriticalSection(&g_cs);
      const RecStats s = g_rs;
      g_rs = RecStats{};
      const size_t n = g_rec.combos.size();
      LeaveCriticalSection(&g_cs);
      EnterCriticalSection(&g_cs);
      const long late = g_late_wanted, late_ours = g_late_derived;
      LeaveCriticalSection(&g_cs);
      if (s.fresh || s.missed_replay)
        ss_log("[pipewarm] record %s: %ld draws in 10 s (%ld without shaders), %ld new combinations (%zu in the file), %ld shaders held "
               "(%ld made in the race, %ld of them variants of ours)%s", level,
               s.draws, s.fixed, s.fresh, n, g_kept, late, late_ours,
               g_replay ? (std::string(", ") + std::to_string(s.missed_replay) + " draws of combinations the replay did not make").c_str() : "");
    }
  }
  if (all || !g_replay) return;
  if (g_rp.active) {
    ReplaySlice(d, g_slice_ms);
  } else if (!g_rp.finished && st == 1 && level[0]) {
    if (!g_rp.prepared) PrepareReplay(level);
    size_t have = 0;
    const bool ready = g_rp.prepared && ReplayReady(&have);
    if (g_rp.prepared && !ready) {
      static LARGE_INTEGER last_log = {};
      if (!last_log.QuadPart || MsSince(last_log) > 250) {
        QueryPerformanceCounter(&last_log);
        ss_log("[pipewarm] replay %s waits: %zu of %zu shaders held, %.0f ms into the wait, %ld made so far",
               g_rp.level.c_str(), have, g_rp.needed, MsSince(g_rp.wait0), g_creates);
      }
    }
    if (ready && StartReplay(d, have)) ReplaySlice(d, g_slice_ms);
  } else if (g_rp.prepared && !g_rp.finished && st != 1) {
    size_t have = 0;
    ReplayReady(&have);
    ss_log("[pipewarm] replay %s: the load ended first (%zu of %zu shaders held): started now", g_rp.level.c_str(), have,
           g_rp.needed);
    if (StartReplay(d, have)) ReplaySlice(d, g_slice_ms);
    g_rp.finished = true;
  }
}
