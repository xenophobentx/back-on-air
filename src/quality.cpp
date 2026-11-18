// Image quality and detail rows, with the port's keys, values and defaults (its settings catalogue):
//   anisotropy     = game | off | 2 | 4 | 8 | 16   (game)     SS_ANISOTROPY
//   draw_distance  = console | 1.5 | 2             (console)  SS_DRAW_DISTANCE
//   visibility_lists = game | off                  (game)     SS_VISIBILITY_LISTS
//   opponent_cars  = console | high (| pc)         (console)  SS_CAR_DETAIL
//   shadow_distance = console | 1.5 | 2            (console)  SS_SHADOW_DISTANCE
//   taa_lod_bias   = -2..0                          (-0.5)     SS_TAA_LOD_BIAS (this patch's own: aa = taa only)
//
// Nothing here writes the game's code. Three levers only:
//   * slot 69 (SetSamplerState) of the D3D9 device's vtable, which lives in d3d9.dll's memory (DXVK or wined3d), the
//     same way patch.cpp takes slot 17 (Present). The game's own sampler function (0x516A80) calls the device through
//     that slot ([vtable+0x114]) for every state of a sampler;
//     it also clamps U / V addressing while the motion blur pass runs stronger than the game's (picture.cpp);
//   * float / int tunables in the game's writable data (.data): the scenery LOD distances and the opponent cars' LOD
//     rule (PlayerLOD). The game reads them where they are every frame (sector LOD choice 0x7C1BD0, fades 0x7C21C0,
//     second pass 0x7C5570; car LOD assignment around 0x7B0700) and its params loader writes them through the tweak
//     objects (a plain store, no clamp: float tweak vtable 0xBB2468, slot 6 = 0x487540). Each value is watched: when
//     the game writes it (a level's CityLOD.params at the level load, PlayerLOD_ps3.params at start), that becomes
//     the title's value again and ours is put back over it from it. A value the loader leaves alone keeps its earlier
//     title value, so a factor never compounds;
//   * for shadow_distance only, three vtable slots of each of the two sun shadow classes, in the game's read-only
//     data (their compare, set-up and cascade fit).
// Every address is a run-time VA of the Steam build 385319; the tweak registrations (push <address>) are compared
// with the known bytes before anything is written, and on any mismatch this part does nothing.
#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include "menu.h"
#include "ss_shared.h"

namespace {

inline uint32_t Rd(uintptr_t va) { return *reinterpret_cast<const volatile uint32_t*>(va); }
inline bool Wr(uintptr_t va, uint32_t v) { return ss_write_word(reinterpret_cast<void*>(va), v); }

bool Readable(uintptr_t p, size_t n) {
  if (p < 0x10000 || p + n < p) return false;
  if (ss_region_known(p, n)) return true;  // checked in this phase of the frame (ss_shared.h)
  if (ss_region_bad(p)) return false;
  while (n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!SS_VQ(reinterpret_cast<void*>(p), &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT || mbi.Protect == 0 || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
      ss_region_note_bad(reinterpret_cast<uintptr_t>(mbi.BaseAddress), mbi.RegionSize);
      return false;
    }
    ss_region_note(reinterpret_cast<uintptr_t>(mbi.BaseAddress), mbi.RegionSize, mbi.Type);
    uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    size_t avail = end - p;
    if (avail >= n) return true;
    n -= avail;
    p = end;
  }
  return true;
}

std::string Setting(const char* env, const char* key) {
  const char* e = getenv(env);
  return e ? std::string(e) : ss_conf(key);
}

float AsFloat(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
uint32_t FromFloat(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }

// ------------------------------------------------------------------ anisotropy
//
// As the port's renderer does it: N is the level every linear-filtered texture with mips gets, whatever the title
// asked, and the cap on what it asks of the others; off is none at all; game is the title's own rule. The PC title's
// rule (0x516A80): at quality High / Very High (global 0xD0F7E8 >= 2) a sampler whose min, mag and mip filters are all
// at least LINEAR gets MINFILTER ANISOTROPIC and MAXANISOTROPY = the device's maximum ([0xD64708]+0x620); the mag
// filter stays LINEAR; at Low / Medium the sampler's own states. So 16 at High / Very High is the title's picture on
// a 16x device, and Low / Medium get it too.
//
// Only a sampler whose bound texture is a 2D texture with more than one mip level is made anisotropic; at 2..16 a
// sampler on any other texture (one level: render targets, the colour grade's cube, look-up and gradient textures;
// volume and cube textures) is kept off anisotropic, also where the title asked for it. The test has to be on the
// texture, not on the sampler's mip filter: the title sets a mip filter on samplers of one-level textures too, and
// DXVK (2.7.1, BindSampler) turns on Vulkan anisotropy for any sampler with a mip filter and MINFILTER ANISOTROPIC.
// AMD's hardware ignores anisotropy on a one-level texture (RADV sets radv_disable_aniso_single_level for DXVK, as
// D3D drivers behave), NVIDIA's does not: there the grade's colour look-up was sampled along a stretched footprint
// wherever the colour changes fast between pixels, which drew red contour lines over the whole picture (GitHub #1,
// RTX 3070). Which texture a sampler has comes from slot 65 (SetTexture) of the same vtable.

constexpr uintptr_t kRenderer = 0xD64708;  // renderer; the device is renderer+0x578 (as patch.cpp)
constexpr uint32_t kRendererDevice = 0x578;
constexpr int kPresentSlot = 17;
constexpr int kSamplerSlot = 69;  // IDirect3DDevice9::SetSamplerState (0x114 / 4)
constexpr int kSetTextureSlot = 65;  // IDirect3DDevice9::SetTexture (0x104 / 4)

constexpr DWORD kMag = 5, kMin = 6, kMip = 7, kBias = 8, kMaxAniso = 10;  // D3DSAMP_* (kBias: MIPMAPLODBIAS, a float's bits)
constexpr DWORD kNone = 0, kLinear = 2, kAnisotropic = 3;     // D3DTEXF_*
constexpr DWORD kUnsent = 0xFFFFFFFFu;

// 0 game (no hook at start; after a menu change with the hook in, the title's own states), 1 off, else the level.
volatile int g_aniso = 0;

using SetSamplerFn = long(__stdcall*)(void*, DWORD, DWORD, DWORD);
SetSamplerFn g_sampler_orig = nullptr;
using SetTextureFn = long(__stdcall*)(void*, DWORD, IDirect3DBaseTexture9*);
SetTextureFn g_settex_orig = nullptr;
uint32_t* g_sampler_vt = nullptr;
// Counters for the log, written by the render thread only (plain increments: no locked instruction per call), read by
// the worker for its lines.
volatile LONG g_sampler_calls = 0, g_sampler_promoted = 0, g_tex_mipped = 0, g_tex_other = 0;
volatile LONG g_sent = 0, g_skipped = 0, g_tex_calls = 0;

// What the title asked of each sampler (D3D9's defaults until it says), and the min filter and max anisotropy this
// hook last sent for it. The title's own state is always passed on; the two states that follow from the others (min
// filter, max anisotropy) are sent with another state or a texture of another kind only when they differ from what was
// last sent. A Reset or a state block can set samplers behind the hook's back, so what was sent is forgotten at every
// present (ss_quality_present): a stale value lasts one frame at most, and only until the title sets that state itself.
// The DLL's own drawing (the settings overlay, the anti-aliasing passes: ss_own_drawing) passes through untracked;
// both put the device back with a state block afterwards, so the title's states here stay the device's.
struct Sampler {
  DWORD mag = 1, min = 1, mip = kNone, aniso = 1;
  DWORD bias = 0;        // the title's MIPMAPLODBIAS (float bits)
  bool min_known = false;
  bool biased = false;   // the device has the title's bias plus ours
  bool mipped2d = false; // the bound texture is a 2D texture with more than one level (see the head comment)
  DWORD sent_min = kUnsent, sent_aniso = kUnsent;  // what this hook last sent (kUnsent: not known)
};
Sampler g_samplers[21];  // pixel samplers 0..15, then D3DDMAPSAMPLER (256) and the vertex samplers 257..260

int SamplerIndex(DWORD s) {
  if (s < 16) return static_cast<int>(s);
  if (s >= 256 && s <= 260) return 16 + static_cast<int>(s - 256);
  return -1;
}

bool Filtered(DWORD f) { return f == kLinear || f == kAnisotropic; }
// `lv` is g_aniso read once per call (the settings menu may change it in between).
bool Promoted(const Sampler& s, int lv) {
  return lv > 1 && s.mipped2d && Filtered(s.min) && Filtered(s.mag) && s.mip != kNone;
}
// Off, or a level with a texture that must not be filtered anisotropically: none at all.
bool NoAniso(const Sampler& s, int lv) { return lv == 1 || (lv > 1 && !s.mipped2d); }
DWORD EffMag(const Sampler& s, int lv) { return NoAniso(s, lv) && s.mag == kAnisotropic ? kLinear : s.mag; }
DWORD EffMin(const Sampler& s, int lv) {
  if (lv == 0) return s.min;
  if (Promoted(s, lv)) return kAnisotropic;
  return NoAniso(s, lv) && s.min == kAnisotropic ? kLinear : s.min;
}
DWORD EffAniso(const Sampler& s, int lv) {
  DWORD a = s.aniso ? s.aniso : 1;
  if (lv == 0) return a;
  if (NoAniso(s, lv)) return 1;
  if (Promoted(s, lv)) return static_cast<DWORD>(lv);
  return a > static_cast<DWORD>(lv) ? static_cast<DWORD>(lv) : a;
}

// ------------------------------------------------------------------ TAA's texture LOD bias
// While TAA jitters the camera (postfx.cpp: aa = taa), each frame's samples land on other sub-pixel positions and the
// resolve averages them, so textures can take a finer mip than one frame alone would show without shimmering: the
// title's mip-mapped pixel samplers (mip filter not NONE) get taa_lod_bias added to their MIPMAPLODBIAS from the
// frame's update (ss_quality_scene_begin) until the scene/UI split (ss_quality_scene_end). The HUD, the menus and the
// DLL's own passes come after the split or under ss_own_drawing and keep the title's bias. The G-buffer, the depth and
// the shadow maps are render targets of one level, where a bias cannot pick another mip. Render thread only.
float g_lod_bias = -0.5f;      // taa_lod_bias
bool g_bias_hook = false;      // the hook went in for the bias (aa = taa at start)
bool g_scene_bias = false;     // between the two calls, with the camera jittered
uint32_t g_bias_frames = 0;

DWORD BiasFor(const Sampler& s, int i) {
  return g_scene_bias && i < 16 && s.mip != kNone ? FromFloat(AsFloat(s.bias) + g_lod_bias) : s.bias;
}

long Put(void* dev, DWORD sampler, DWORD type, DWORD value) {
  ++g_sent;
  return g_sampler_orig(dev, sampler, type, value);
}
// A state that follows from the others: sent only when it differs from what was last sent.
void Follow(void* dev, DWORD sampler, DWORD type, DWORD value, DWORD& sent) {
  if (sent == value) {
    ++g_skipped;
    return;
  }
  sent = SUCCEEDED(Put(dev, sampler, type, value)) ? value : kUnsent;
}

// The motion blur pass's edge clamp (picture.cpp, ss_quality_edge_clamp): while it runs, the title's U / V addressing
// of samplers 0-7 is noted and the device gets CLAMP; the noted values go back when it ends.
volatile bool g_edge_clamp = false;
bool g_clamp_hook = false;  // the hook went in for it (ss_picture_clamp_wanted at start)
DWORD g_clamp_want[8][2];

long __stdcall SetSamplerHook(void* dev, DWORD sampler, DWORD type, DWORD value) {
  if (g_edge_clamp && sampler < 8 && (type == D3DSAMP_ADDRESSU || type == D3DSAMP_ADDRESSV) && ss_own_drawing == 0) {
    g_clamp_want[sampler][type - D3DSAMP_ADDRESSU] = value;
    return g_sampler_orig(dev, sampler, type, D3DTADDRESS_CLAMP);
  }
  const int i = SamplerIndex(sampler);
  if (i < 0 || !(type == kMag || type == kMin || type == kMip || type == kBias || type == kMaxAniso) || ss_own_drawing != 0)
    return g_sampler_orig(dev, sampler, type, value);
  Sampler& s = g_samplers[i];
  if (type == kBias) {  // the bias depends on the title's bias and the mip filter only
    s.bias = value;
    const DWORD b = BiasFor(s, i);
    s.biased = b != s.bias;
    return Put(dev, sampler, kBias, b);
  }
  switch (type) {
    case kMag: s.mag = value; break;
    case kMin: s.min = value; s.min_known = true; break;
    case kMip: s.mip = value; break;
    default: s.aniso = value; break;
  }
  const int lv = g_aniso;
  const DWORD mn = EffMin(s, lv), an = EffAniso(s, lv);
  long hr;
  switch (type) {
    case kMag: hr = Put(dev, sampler, kMag, EffMag(s, lv)); break;
    case kMin: hr = Put(dev, sampler, kMin, mn); s.sent_min = SUCCEEDED(hr) ? mn : kUnsent; break;
    case kMip: hr = Put(dev, sampler, kMip, value); break;
    default: hr = Put(dev, sampler, kMaxAniso, an); s.sent_aniso = SUCCEEDED(hr) ? an : kUnsent; break;
  }
  // The other states decide the min filter and the anisotropy: sent with them when they change (see Sampler).
  if (type != kMin && s.min_known) Follow(dev, sampler, kMin, mn, s.sent_min);
  if (type != kMaxAniso) Follow(dev, sampler, kMaxAniso, an, s.sent_aniso);
  if (type == kMip && (g_scene_bias || s.biased)) {  // the mip filter decides whether the bias applies
    const DWORD b = BiasFor(s, i);
    s.biased = b != s.bias;
    Put(dev, sampler, kBias, b);
  }
  ++g_sampler_calls;
  if (type == kMin && mn != value) ++g_sampler_promoted;
  return hr;
}

// The title binds a texture: when that changes whether its sampler may be anisotropic, the filter states that follow
// from it are sent again (the title may set the sampler's states before or after the texture; DXVK reads both at the
// draw). The DLL's own drawing passes through untracked: it puts the title's textures back with a state block.
long __stdcall SetTextureHook(void* dev, DWORD stage, IDirect3DBaseTexture9* tex) {
  const long hr = g_settex_orig(dev, stage, tex);
  ++g_tex_calls;
  const int i = SamplerIndex(stage);
  if (i < 0 || ss_own_drawing != 0 || FAILED(hr) || !g_sampler_orig) return hr;
  // The kind is asked of the texture at every bind, not cached by pointer: the level streams textures out and in, and
  // a new texture at a freed one's address may be of the other kind. Two cheap calls into d3d9.dll.
  const bool m = tex && tex->GetType() == D3DRTYPE_TEXTURE && tex->GetLevelCount() > 1;
  Sampler& s = g_samplers[i];
  if (m == s.mipped2d) return hr;
  ++(m ? g_tex_mipped : g_tex_other);
  const int lv = g_aniso;
  const DWORD mag = EffMag(s, lv);
  s.mipped2d = m;
  if (EffMag(s, lv) != mag) Put(dev, stage, kMag, EffMag(s, lv));
  if (s.min_known) Follow(dev, stage, kMin, EffMin(s, lv), s.sent_min);
  Follow(dev, stage, kMaxAniso, EffAniso(s, lv), s.sent_aniso);
  return hr;
}

// Puts the hook into the device's vtable (in d3d9.dll's memory) once the device exists; again if the vtable changes.
// patch.cpp writes slot 17 of the same vtable from its own thread: to keep the two page-protection changes apart this
// waits until that slot is already ours, or until the device has existed for 3 s (fps = 30 never hooks it).
void InstallSamplerHook() {
  static DWORD device_seen = 0;
  uintptr_t r = Rd(kRenderer);
  if (!r || !Readable(r + kRendererDevice, 4)) return;
  uintptr_t dev = Rd(r + kRendererDevice);
  if (!dev || !Readable(dev, 4)) return;
  uint32_t* vt = reinterpret_cast<uint32_t*>(Rd(dev));
  if (!vt || !Readable(reinterpret_cast<uintptr_t>(vt), (kSamplerSlot + 1) * 4)) return;
  const uint32_t mine = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&SetSamplerHook));
  if (vt[kSamplerSlot] == mine) return;
  if (!device_seen) device_seen = GetTickCount();
  HMODULE self = nullptr, present_owner = nullptr;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<const char*>(&SetSamplerHook), &self);
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<const char*>(vt[kPresentSlot]), &present_owner);
  if (present_owner != self && GetTickCount() - device_seen < 3000) return;
  HMODULE owner = nullptr, game = GetModuleHandleA(nullptr);
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<const char*>(vt[kSamplerSlot]), &owner) || !owner || owner == game || owner == self) {
    static bool said = false;
    if (!said) ss_log("[aniso] the device's SetSamplerState is not in a d3d9 module; anisotropy left to the game");
    said = true;
    return;
  }
  if (g_sampler_vt && g_sampler_vt != vt) ss_log("[aniso] the device's vtable changed (%p -> %p): hooked again", g_sampler_vt, vt);
  for (Sampler& s : g_samplers) s = Sampler();
  // SetTexture first: until it is in, no sampler counts as one on a mipped 2D texture (none is made anisotropic).
  const uint32_t mine_tex = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&SetTextureHook));
  if (vt[kSetTextureSlot] != mine_tex) {
    const SetTextureFn prev = g_settex_orig;
    g_settex_orig = reinterpret_cast<SetTextureFn>(vt[kSetTextureSlot]);
    if (!ss_write_word(&vt[kSetTextureSlot], mine_tex)) {
      ss_log("[aniso] cannot write the device vtable's SetTexture (error %lu); no anisotropic filtering", GetLastError());
      g_settex_orig = prev;
    }
  }
  g_sampler_orig = reinterpret_cast<SetSamplerFn>(vt[kSamplerSlot]);
  if (!ss_write_word(&vt[kSamplerSlot], mine)) {
    ss_log("[aniso] cannot write the device vtable (error %lu); anisotropy left to the game", GetLastError());
    g_sampler_orig = nullptr;
    return;
  }
  g_sampler_vt = vt;
  char name[MAX_PATH] = "?";
  GetModuleFileNameA(owner, name, sizeof name);
  ss_log("[aniso] SetSamplerState hooked in %s (device %p, vtable %p); %s", name, reinterpret_cast<void*>(dev), vt,
         g_aniso == 0   ? "the game's own filter states (hooked for TAA's texture LOD bias or the motion blur's edge clamp)"
         : g_aniso == 1 ? "no anisotropic filtering"
                        : "every linear-filtered sampler with mips on a mipped 2D texture anisotropic, none on other textures");
}

// The device's pixel samplers 0..15 to the bias the scene phase asks for.
void SendBiases(void* dev) {
  for (int i = 0; i < 16; ++i) {
    Sampler& s = g_samplers[i];
    const DWORD b = BiasFor(s, i);
    if (b == s.bias && !s.biased) continue;
    g_sampler_orig(dev, static_cast<DWORD>(i), kBias, b);
    s.biased = b != s.bias;
  }
}

float ParseLodBias(const std::string& v) {
  if (v.empty()) return -0.5f;
  float b = static_cast<float>(atof(v.c_str()));
  if (!(b >= -2.0f)) b = -2.0f;  // also NaN
  if (b > 0.0f) b = 0.0f;
  return b;
}

int ParseAnisotropy(const std::string& v) {
  if (v.empty()) return 0;  // default: game (PC ORIGINAL)
  if (v == "game") return 0;
  if (v == "off") return 1;
  int n = atoi(v.c_str());
  if (n < 1) return 0;
  if (n > 16) n = 16;
  return n;
}

int WantAnisotropy() { return ParseAnisotropy(Setting("SS_ANISOTROPY", "anisotropy")); }

// ------------------------------------------------------------------ detail tunables

// One value in the game's data and what we want of it, from the title's value.
enum class Rule { kScale, kSet, kAtLeast };
struct Tunable {
  uintptr_t va;
  uintptr_t site;  // its tweak registration: push <va>
  const char* name;
  bool is_int;
  Rule rule;
  double arg;
  bool have = false;
  uint32_t title = 0, written = 0;
};

// The scenery's LOD distances (tweaks EnvironmentSS/LODs/..., registered at 0xB81562..0xB81716). As the port: high
// and low and their fades times the factor, the mid distance and the stream radius (Sectors/Streaming/e; Max Sector
// Stream Dist 0xD389F0, 400 m) stay the level's. The mid switch has no fade in the title and a larger stream radius
// multiplies what streams; scaling them made objects snap from their low mesh in plain sight (the port's finding).
struct Scenery {
  uintptr_t va, site;
  const char* name;
};
constexpr Scenery kScenery[] = {
    {0xD38A90, 0xB81562, "high"},           {0xD38AA0, 0xB815C2, "low"},
    {0xD38AA8, 0xB815F6, "high fade"},      {0xD38AB0, 0xB81626, "low fade"},
    {0xD38AB8, 0xB81652, "split high"},     {0xD38AC8, 0xB816B2, "split low"},
    {0xD38AD0, 0xB816E6, "split high fade"}, {0xD38AD8, 0xB81716, "split low fade"},
};

// Sectors/Visibility/Disable Sector Visibility (bool, registered at 0xB81330). Read by the LOD choice (0x7C1CC2), the
// fades (0x7C2785) and the second pass (0x7C5739): set, they take LOD cap 3 instead of the visibility map's cap. The
// caps were made for the console's distances; above x1 the port sets it for the level so the distance alone decides,
// with the title's distance fades.
constexpr uintptr_t kNoVisibilityCaps = 0xD82788, kNoVisibilityCapsSite = 0xB81330;

// Sectors/Visibility/Disable Visibility Map (bool, registered at 0xB937E0; found by Glimpse-Fix). The visibility map
// object ([sector manager+0x25B8]) keeps each view's cell; set, 0xAE6430 leaves every view's cell at -1 and 0xAE6720
// answers "visible" for every entity at once. Read in the code: the cell at -1 also gives every sector LOD cap 3
// (0x7C1CEA, 0x7C5756), and the view's draw (0x7C374F) no longer takes the seclusion-zone path, so nothing inside the
// frustum is held back by the precomputed visibility (the bushes of lod-shadow.md section 10). 0xAE6720's callers are
// the dynamic things: the opponents (0xAC2A5F, with 0xD9A690), the power plays (0x81381C) and four other draw lists.
// Cost: no occlusion culling (more draws) and more full meshes asked of the streaming pool. Set once at start and
// never cleared: while it is set the map does not keep the entities' cells (0xAE65A0 returns at once), so clearing it
// in a level would cull by stale cells. visibility_lists = off sets it with the two switches above at any distance.
constexpr uintptr_t kNoVisibilityMap = 0xD9AFCC, kNoVisibilityMapSite = 0xB937E0;

// Particles (registered at 0xB7F212 / 0xB7F2B2). 0x7D1D90 copies the set for the frame (the split-screen set, or
// quality LOW, otherwise this one) and 0x7CF960 scales a system's emission by its distance d: max((D - d) / D, floor)
// below the LOD distance D (150 m, floor 0.5 / 0.75), the floor beyond it, 0.4 / 0.5 beyond the further distance
// (200 m), and d / 30 m nearer than the kill distance (a near thinning, not a far cull; left alone). Above console
// draw distance both distances take the factor: smoke and sparks thin out farther away. More particles drawn.
// Debris/FadeInDist (0xD193F0) is not read anywhere in the image (only its registration): left alone.
constexpr uintptr_t kParticleLod = 0xD373DC, kParticleLodSite = 0xB7F212;
constexpr uintptr_t kParticleFurther = 0xD373FC, kParticleFurtherSite = 0xB7F2B2;

// PlayerLOD (registered at 0xB807B4..0xB80A24). The PC loads Resident Generic/Params/PlayerLOD_ps3.params (string
// at 0xC17B50), the console PlayerLOD_xenon.params. They differ in two rows: MaxAIMidLODs 3 (PC) / 31 (console),
// AIMidToFar 100 / 200 m; the rest is equal (MaxAINearLODs 2, AINearToMid 20, AIFarToOff 300). So the PC draws the
// opponents' far LOD from 100 m and gives the mid LOD to three of them at most. Read by the assignment at 0x7B092B
// (distances) and 0x7B0A2A (counts); textureLodOverride at 0x7B117F (-1 = none).
constexpr uintptr_t kMaxAiNear = 0xD37C78, kMaxAiNearSite = 0xB807B4;
constexpr uintptr_t kMaxAiMid = 0xD37C80, kMaxAiMidSite = 0xB807D4;
constexpr uintptr_t kAiMidToFar = 0xD37C90, kAiMidToFarSite = 0xB80832;
constexpr uintptr_t kTextureLodOverride = 0xD37CF0, kTextureLodOverrideSite = 0xB80A24;
// The opponents' off distance (PlayerLOD/05;AIFarToOff 0xD37C98, 300 m in .data, 200 m as the PC's params load it
// (measured), registered at 0xB80862; Nemesis trucks
// 81;NemTruckFarToOff 0xD37CC8, 200 m, at 0xB80982). 0x7B096D: an opponent farther than it gets no LOD at all
// (0x7B02A0) instead of the far one (0x7B02E0): it is gone in one frame and back in one frame, with no fade, the one
// distant class of the picture that draw_distance did not reach. Above x1 both take the factor, so an opponent
// switches where the scenery's own detail does.
constexpr uintptr_t kAiFarToOff = 0xD37C98, kAiFarToOffSite = 0xB80862;
constexpr uintptr_t kNemTruckFarToOff = 0xD37CC8, kNemTruckFarToOffSite = 0xB80982;
constexpr int kConsoleMaxAiMid = 31;
constexpr float kConsoleAiMidToFar = 200.0f;
constexpr int kHighNearLods = 7;  // the port's CAR DETAIL HIGH: every opponent in the near range gets the near body

// The sun's cascaded shadow. Every frame the light pass (0x88A3E0) fills a settings record (+0 cascade count, +4
// range, +8 fadeout, +0xC split weight, ...): at quality HIGH / VERY HIGH from the tweaks Lighting/CSM_Max/Settings
// (0x894B60; the console's values: 300 m, 100 m, 0.8, or 0.7 when the pass's flag +0x192 is set), at LOW / MEDIUM from
// the level's lighting rig in the heap (0x8988B0). It asks the frame's shadow maps whether the record is still theirs
// (vtable +0x20) and, when not, sets them up again from a set-up record (vtable +0x18; +0x14 range, +0x18 fadeout)
// that also carries the atlas size, 1024 x 3072 for three cascades (immediates in code). The maps are of one of two
// classes (vtables 0xC314DC and 0xC31944) with the same three methods; HIGH runs the first, MEDIUM the second.
//
// So both classes' compare and set-up slots (in read-only data) are pointed at wrappers that hand on a copy of the
// record with range and fadeout times the factor: the maps are set up once with the raised range, every frame's
// compare agrees, and it holds for every quality level and for the rig's numbers as well as the tweaks.
//
// The cascade fit (vtable +0x10: 0x8950C0 / 0x898D70) places the splits between 1 m and the range by the split
// weight w: w * range^(i/N) + (1 - w) * (1 + (range - 1) * i/N). With the range alone raised every split moves out
// (x2 at weight 0.7: 35 -> 66 m, 91 -> 170 m): the sharp first cascade's 1024 texels cover more ground and the line
// where the coarser one takes over moves with it (the port's finding). So the fit's slot is pointed at a wrapper
// that, for the call only, gives the object the weight at which the first split stays where the title has it; the
// second lands a little farther and the last cascade takes the rest. At console no slot is touched.
struct CsmClass {
  uintptr_t fit_slot, fit, setup_slot, setup, compare_slot, compare;
  uint32_t splits, range, weight;  // fields of the object
};
constexpr CsmClass kCsm[2] = {
    {0xC314EC, 0x8950C0, 0xC314F4, 0x898330, 0xC314FC, 0x894D40, 0xEF0, 0xF04, 0xF0C},
    {0xC31954, 0x898D70, 0xC3195C, 0x899970, 0xC31964, 0x898BB0, 0xA80, 0xA94, 0xA9C},
};
constexpr uint32_t kCsmCount = 0x370;
constexpr uintptr_t kQuality = 0xD0F7E8, kQualitySite = 0xB6B2F4;  // the QUALITY option's level, 0 .. 3

// The QUALITY level's VERY HIGH (3) is the game's 2x2 supersampling: the scene render graphs are built at twice the
// screen's width and height when a race loads (0xA06F80 modes 1 / 2, 0xA07260 on a resolution change) and scaled down
// at the frame end (LINEAR StretchRect, 0x50E46D); 4x the pixels, about 32 fps on the Steam Deck. The game's own
// option sets it; this patch leaves the level alone (its post_quality key was removed, SSPC-38).

Tunable g_tun[24];
int g_ntun = 0;
bool g_caps_off = false;
double g_draw_distance = 1.0;
bool g_map_off = false;  // visibility_lists = off

void Add(uintptr_t va, uintptr_t site, const char* name, bool is_int, Rule rule, double arg) {
  if (g_ntun >= 24) return;
  Tunable& t = g_tun[g_ntun++];
  t.va = va;
  t.site = site;
  t.name = name;
  t.is_int = is_int;
  t.rule = rule;
  t.arg = arg;
}

// The tweak registration pushes the address (68 imm32) and stores the tweak handle at address + 4 once it has run:
// the build is the known one and the game's static initialisers (after the protector unpacked the image) are done.
bool Registered(uintptr_t va, uintptr_t site) {
  if (!Readable(site, 5) || !Readable(va, 8)) return false;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(site);
  uint32_t imm;
  memcpy(&imm, p + 1, 4);
  if (p[0] != 0x68 || imm != va) return false;
  const uint32_t handle = Rd(va + 4);
  return handle >= 0x10000 && Readable(handle, 4);
}

uint32_t Want(const Tunable& t, uint32_t title) {
  if (t.is_int) {
    const int32_t v = static_cast<int32_t>(title);
    switch (t.rule) {
      case Rule::kSet: return static_cast<uint32_t>(static_cast<int32_t>(t.arg));
      case Rule::kAtLeast: return static_cast<uint32_t>(v > static_cast<int32_t>(t.arg) ? v : static_cast<int32_t>(t.arg));
      default: return static_cast<uint32_t>(static_cast<int32_t>(lround(v * t.arg)));
    }
  }
  const float f = AsFloat(title);
  if (!std::isfinite(f)) return title;
  switch (t.rule) {
    case Rule::kSet: return FromFloat(static_cast<float>(t.arg));
    case Rule::kAtLeast: return FromFloat(f > t.arg ? f : static_cast<float>(t.arg));
    default: return FromFloat(static_cast<float>(f * t.arg));
  }
}

std::string Show(const Tunable& t, uint32_t v) {
  char b[32];
  if (t.is_int) snprintf(b, sizeof b, "%d", static_cast<int32_t>(v));
  else snprintf(b, sizeof b, "%g", AsFloat(v));
  return b;
}

// One pass: every value the game has written since our last write becomes the title's, and ours goes back over it.
void ApplyTunables() {
  static int lines = 0;
  std::string changed;
  for (int i = 0; i < g_ntun; ++i) {
    Tunable& t = g_tun[i];
    const uint32_t cur = Rd(t.va);
    if (t.have && cur == t.written) continue;
    t.title = cur;
    t.have = true;
    uint32_t want = Want(t, cur);
    if (want != cur && !Wr(t.va, want)) want = cur;  // refused: the title's value stays, and is not written again
    t.written = want;
    changed += changed.empty() ? "" : ", ";
    changed += std::string(t.name) + " " + Show(t, cur) + (want != cur ? " -> " + Show(t, want) : "");
  }
  if (g_caps_off && *reinterpret_cast<volatile uint8_t*>(kNoVisibilityCaps) == 0) {
    Wr(kNoVisibilityCaps, Rd(kNoVisibilityCaps) | 1);  // a bool tweak: one byte, in a word of its own
    if (lines < 60) changed += changed.empty() ? "visibility map LOD caps off" : ", visibility map LOD caps off";
  }
  if (!changed.empty() && lines < 60) {
    ++lines;
    ss_log("[detail] the game's values (re)loaded: %s (quality level %u)", changed.c_str(), Rd(kQuality));
  }
}

double WantFactor(const char* env, const char* key) {
  std::string v = Setting(env, key);
  if (v.empty() || v == "console") return 1.0;
  double d = atof(v.c_str());
  if (!(d >= 1.0)) return 1.0;
  if (d > 3.0) d = 3.0;
  return d;
}
double WantDrawDistance() { return WantFactor("SS_DRAW_DISTANCE", "draw_distance"); }
bool WantMapOff() {
  const std::string v = Setting("SS_VISIBILITY_LISTS", "visibility_lists");
  return v == "off" || v == "false" || v == "0";
}

// ------------------------------------------------------------------ shadow distance: the cascade fit

volatile float g_shadow_factor = 1.0f;
using CsmFitFn = void(__attribute__((thiscall)) *)(void*, uint32_t);
using CsmRecordFn = uint32_t(__attribute__((thiscall)) *)(void*, const uint32_t*);

// The weight at which the first of n splits over 1 m .. range is where weight w puts it over 1 m .. range / factor.
float FirstSplitWeight(float w, float range, float factor, int n) {
  const double t = 1.0 / n, title = range / factor;
  const double keep = w * pow(title, t) + (1.0 - w) * (1.0 + (title - 1.0) * t);
  const double lin = 1.0 + (range - 1.0) * t, log_split = pow(range, t);
  if (!(lin - log_split > 1e-3)) return w;
  const double r = (lin - keep) / (lin - log_split);
  return static_cast<float>(r < w ? w : r > 0.98 ? 0.98 : r);
}

// The fit with the first split kept. The object's range is the raised one (the set-up wrapper's).
template <int K>
void __attribute__((thiscall, force_align_arg_pointer)) CsmFitHook(void* self, uint32_t arg) {
  const CsmClass& c = kCsm[K];
  const CsmFitFn fit = reinterpret_cast<CsmFitFn>(c.fit);
  char* o = static_cast<char*>(self);
  float range, weight;
  int32_t n;
  memcpy(&range, o + c.range, 4);
  memcpy(&weight, o + c.weight, 4);
  memcpy(&n, o + kCsmCount, 4);
  const float factor = g_shadow_factor;
  const bool raise = factor > 1.0f && range > 4.0f * factor && n >= 2 && n <= 4 && weight > 0.0f && weight < 1.0f;
  if (!raise) return fit(self, arg);
  const float fit_weight = FirstSplitWeight(weight, range, factor, n);
  memcpy(o + c.weight, &fit_weight, 4);
  fit(self, arg);
  memcpy(o + c.weight, &weight, 4);  // the compare must find the title's weight again
  static float said_range = 0.0f, said_weight = 0.0f;
  static int lines = 0;
  if ((said_range != range || said_weight != weight) && lines < 6) {
    ++lines;
    said_range = range;
    said_weight = weight;
    float s[5] = {};
    memcpy(s, o + c.splits, sizeof(float) * static_cast<size_t>(n + 1));
    ss_log("[detail] sun shadow (class %d, quality level %u): %d cascades over %g m (the title's %g m), split weight %g for "
           "the fit instead of %g: splits at %.1f / %.1f / %.1f / %.1f m",
           K, Rd(kQuality), n, range, range / factor, fit_weight, weight, s[1], s[2], s[3], s[4]);
  }
}

// The frame's record: +4 range, +8 fadeout (0x30 bytes).
template <int K>
uint32_t __attribute__((thiscall, force_align_arg_pointer)) CsmCompareHook(void* self, const uint32_t* record) {
  uint32_t copy[12];
  memcpy(copy, record, sizeof copy);
  copy[1] = FromFloat(AsFloat(copy[1]) * g_shadow_factor);
  copy[2] = FromFloat(AsFloat(copy[2]) * g_shadow_factor);
  return reinterpret_cast<CsmRecordFn>(kCsm[K].compare)(self, copy);
}

// The set-up record (built at 0x88A52E): +0x14 range, +0x18 fadeout (0x40 bytes).
template <int K>
uint32_t __attribute__((thiscall, force_align_arg_pointer)) CsmSetupHook(void* self, const uint32_t* record) {
  uint32_t copy[16];
  memcpy(copy, record, sizeof copy);
  copy[5] = FromFloat(AsFloat(copy[5]) * g_shadow_factor);
  copy[6] = FromFloat(AsFloat(copy[6]) * g_shadow_factor);
  return reinterpret_cast<CsmRecordFn>(kCsm[K].setup)(self, copy);
}

// One class's three slots, all or none (its compare and set-up must agree), once they hold the known functions.
bool HookCsm(int k, uintptr_t fit, uintptr_t setup, uintptr_t compare) {
  const CsmClass& c = kCsm[k];
  if (!Readable(c.fit_slot, 0x14) || Rd(c.fit_slot) != c.fit || Rd(c.setup_slot) != c.setup || Rd(c.compare_slot) != c.compare)
    return false;
  if (!Wr(c.compare_slot, static_cast<uint32_t>(compare))) return false;
  if (!Wr(c.setup_slot, static_cast<uint32_t>(setup))) {
    Wr(c.compare_slot, static_cast<uint32_t>(c.compare));
    return false;
  }
  Wr(c.fit_slot, static_cast<uint32_t>(fit));
  return true;
}

// ------------------------------------------------------------------ the knock-over props' draw distance
//
// Posts, signs, barriers and the like (the physics objects) are not scenery sectors: their manager draws them up to
// one distance for the whole view, with the title's own fade over the last 20 m (0x7AD7B0). Every frame 0x7AE490
// takes that distance from 0x818510: the value of the box the camera is in (the level's
// PhysicsObjectsDrawDistanceBoxes.params), else Rendering/PhysicsObjects/DefaultDrawDistance (0xD40BF8, 200 m), and
// Dev_ForceDrawDistance (0xD40C00, -1) instead of either when it is not negative. So when the camera passes from one
// box into another the distance jumps in one frame, and every prop between the two values appears or goes at once
// (the port's finding for the lamp posts), and draw_distance did not reach them at all.
//
// From the present hook, on the game's own thread between two frames: the title's value is asked of 0x818510 with
// the force tweak at -1 for the call, times the draw_distance factor is where the distance should be, and the force
// tweak gets a value that moves there at 100 m a second at most, so the props come and go one after the other
// through the title's fade instead of all in a frame. On a camera cut it is there at once. The level load puts the
// tweak back to -1 (0x81880D); it is written again every frame. Wanted with draw_distance above console or
// lod_smoothing on; else the tweak is never touched.
constexpr uintptr_t kPropBoxes = 0xD825C4, kPropDistance = 0x818510;  // thiscall(table, const float* position) -> float
constexpr uintptr_t kPropForce = 0xD40C00, kPropForceSite = 0xB83A76;
constexpr uint8_t kPropDistanceSig[9] = {0x8B, 0x44, 0x24, 0x04, 0x57, 0x50, 0x8B, 0xF9, 0xE8};
constexpr uintptr_t kPropGame = 0xD714D0;  // [[game]+0x28] the environment; view 0's camera at +0x65C, its position +0x10
constexpr double kPropRate = 100.0;        // metres a second
// Power-play objects that are idle are asked of the visibility map before their distance (0x8137FB), so they appear
// in one frame on a cell change; Sectors/Visibility/Disable Powerplay Visibility (a bool tweak) lets the distance
// alone decide, as the scenery's own switch above x1.
constexpr uintptr_t kPowerplayNoVisibility = 0xD8B368, kPowerplayNoVisibilitySite = 0xB836A0;

volatile LONG g_props = 0;  // the present hook may work the force tweak
double g_props_factor = 1.0;

using PropDistanceFn = float(__attribute__((thiscall)) *)(void*, const float*);

void PropsPresent() {
  static double cur = -1.0, last_time = 0, said_title = -1.0;
  static float last_pos[3];
  static LARGE_INTEGER freq = {};
  static int lines = 0;
  const uintptr_t table = Rd(kPropBoxes), game = Rd(kPropGame);
  if (!table || !Readable(table + 0x10, 0xC) || !game || !Readable(game + 0x28, 4)) return;
  const uintptr_t env = Rd(game + 0x28);
  if (!env || !Readable(env + 0x65C + 0x10, 12)) return;
  float pos[3];
  memcpy(pos, reinterpret_cast<const void*>(env + 0x65C + 0x10), 12);
  if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  const double now = static_cast<double>(t.QuadPart) / static_cast<double>(freq.QuadPart), dt = now - last_time;
  // The title's distance (a walk over every prop box, about 0.2 ms on Windows) is asked at most 60 times a second:
  // above 60 fps a frame in between keeps the last answer and the camera moved less than a metre (SSPC-17 round 2).
  // A camera cut (more than 5 m since the last answer) asks at once.
  static double title = -1.0, asked = 0;
  static float asked_pos[3];
  double moved = 0;
  for (int i = 0; i < 3; ++i) moved += (pos[i] - asked_pos[i]) * (pos[i] - asked_pos[i]);
  if (title < 0 || now - asked >= 1.0 / 60.0 - 0.002 || now < asked || moved > 25.0) {
    if (!Wr(kPropForce, FromFloat(-1.0f))) return;
    title = reinterpret_cast<PropDistanceFn>(kPropDistance)(reinterpret_cast<void*>(table), pos);
    asked = now;
    memcpy(asked_pos, pos, 12);
  }
  if (!(title >= 0.0) || title > 100000.0) {
    title = -1.0;
    return;  // the tweak stays at -1: the title's own
  }
  const double target = title * g_props_factor;
  double jump = 0;
  for (int i = 0; i < 3; ++i) jump += (pos[i] - last_pos[i]) * (pos[i] - last_pos[i]);
  jump = sqrt(jump);
  if (cur < 0 || !(dt > 0 && dt < 0.5) || jump > 6.0 + 150.0 * dt) {
    cur = target;  // the first frame, or a camera cut
  } else {
    const double step = kPropRate * dt;
    cur = target > cur ? (target - cur > step ? cur + step : target) : (cur - target > step ? cur - step : target);
  }
  memcpy(last_pos, pos, 12);
  last_time = now;
  Wr(kPropForce, FromFloat(static_cast<float>(cur)));
  if (title != said_title && lines < 40) {
    ++lines;
    ss_log("[detail] props: the camera's box says %g m (was %g m); drawn to %.0f m, on its way to %.0f m", title, said_title, cur,
           target);
    said_title = title;
  }
}

// 0 pc (the PC release's PlayerLOD_ps3 values, untouched), 1 console (PlayerLOD_xenon's), 2 high.
int WantCarDetail() {
  std::string v = Setting("SS_CAR_DETAIL", "opponent_cars");
  if (v == "pc") return 0;
  if (v == "high" || v == "1") return 2;
  return 1;
}

DWORD WINAPI QualityWorker(LPVOID) {
  if (!ss_conf_present() && !getenv("SS_ANISOTROPY") && !getenv("SS_DRAW_DISTANCE") && !getenv("SS_VISIBILITY_LISTS") &&
      !getenv("SS_CAR_DETAIL") && !getenv("SS_SHADOW_DISTANCE") && !getenv("SS_TEST_QUALITY"))
    return 0;  // no conf file: the DLL does nothing, as for the frame rate
  g_aniso = WantAnisotropy();
  g_lod_bias = ParseLodBias(Setting("SS_TAA_LOD_BIAS", "taa_lod_bias"));
  const std::string aa = Setting("SS_AA", "aa");
  const bool taa = aa == "taa" || aa == "taa_high";
  g_bias_hook = taa && g_lod_bias != 0.0f;
  g_clamp_hook = ss_picture_clamp_wanted();
  if (taa)
    ss_log("[taa] texture LOD bias %.2f on the game's mip-mapped samplers while TAA jitters the camera%s", g_lod_bias,
           g_lod_bias == 0.0f ? " (off)" : "");
  g_draw_distance = WantDrawDistance();
  g_map_off = WantMapOff();
  const int cars = WantCarDetail();
  const double shadow = WantFactor("SS_SHADOW_DISTANCE", "shadow_distance");
  const bool raised = g_draw_distance > 1.0;
  ss_log("[detail] anisotropy %s, draw_distance %s (x%.2f: scenery, props, opponents' off distance, particles), "
         "visibility_lists %s (%s), opponent_cars %s, shadow_distance %s (x%.2f)",
         g_aniso == 0 ? "game" : g_aniso == 1 ? "off" : std::to_string(g_aniso).c_str(), raised ? "raised" : "console",
         g_draw_distance, g_map_off ? "off" : "game",
         g_map_off ? "visibility map, its LOD caps and the power plays' check off: drawn by distance, no occlusion culling"
         : raised  ? "the map's LOD caps and the power plays' check off, the map itself kept"
                   : "the game's",
         cars == 0 ? "pc" : cars == 1 ? "console" : "high", shadow == 1.0 ? "console" : "raised", shadow);

  if (raised) {
    for (const Scenery& s : kScenery) Add(s.va, s.site, s.name, false, Rule::kScale, g_draw_distance);
    Add(kAiFarToOff, kAiFarToOffSite, "AIFarToOff", false, Rule::kScale, g_draw_distance);
    Add(kNemTruckFarToOff, kNemTruckFarToOffSite, "NemTruckFarToOff", false, Rule::kScale, g_draw_distance);
    Add(kParticleLod, kParticleLodSite, "particles' LOD distance", false, Rule::kScale, g_draw_distance);
    Add(kParticleFurther, kParticleFurtherSite, "particles' further LOD distance", false, Rule::kScale, g_draw_distance);
  }
  g_caps_off = raised || g_map_off;
  if (g_map_off) Add(kNoVisibilityMap, kNoVisibilityMapSite, "visibility map off", true, Rule::kSet, 1);
  if (cars >= 1) {
    Add(kMaxAiMid, kMaxAiMidSite, "MaxAIMidLODs", true, Rule::kSet, kConsoleMaxAiMid);
    Add(kAiMidToFar, kAiMidToFarSite, "AIMidToFar", false, Rule::kSet, kConsoleAiMidToFar);
  }
  if (cars == 2) {
    Add(kMaxAiNear, kMaxAiNearSite, "MaxAINearLODs", true, Rule::kAtLeast, kHighNearLods);
    Add(kTextureLodOverride, kTextureLodOverrideSite, "textureLodOverride", true, Rule::kSet, 0);
  }
  // A check, not a setting: SS_TEST_QUALITY=<0..3> holds the QUALITY option's level for the run (the profile is not
  // written), to try both shadow paths on one install.
  if (const char* q = getenv("SS_TEST_QUALITY"))
    if (*q >= '0' && *q <= '3') Add(kQuality, kQualitySite, "quality level (SS_TEST_QUALITY)", true, Rule::kSet, *q - '0');
  // post_quality (this patch's VERY HIGH hold) was removed: the game's own QUALITY option rules alone.
  if (!Setting("SS_POST_QUALITY", "post_quality").empty())
    ss_log("[detail] post_quality removed: use the game's own QUALITY / post-processing option");
  const std::string smoothing = Setting("SS_LOD_SMOOTHING", "lod_smoothing");
  // lod_smoothing: no key is on (lodfade.cpp's default).
  const bool props = g_draw_distance > 1.0 || !(smoothing == "off" || smoothing == "false" || smoothing == "0");
  if (g_caps_off)
    Add(kPowerplayNoVisibility, kPowerplayNoVisibilitySite, "power plays off the visibility map", true, Rule::kSet, 1);
  if (g_aniso == 0 && g_ntun == 0 && shadow == 1.0 && !props && !g_bias_hook && !g_clamp_hook) return 0;  // all at the game's own values: nothing is touched

  // Wait for the image to be unpacked and the tweaks registered; reads only until then.
  bool ready = g_ntun == 0;
  const DWORD t0 = GetTickCount();
  while (!ready) {
    ready = true;
    for (int i = 0; i < g_ntun && ready; ++i) ready = Registered(g_tun[i].va, g_tun[i].site);
    if (ready && g_caps_off) ready = Registered(kNoVisibilityCaps, kNoVisibilityCapsSite);
    if (ready) break;
    if (GetTickCount() - t0 > 180000) {
      ss_log("[detail] the tweak registrations differ from the known build (or never ran): detail values left alone");
      g_ntun = 0;
      g_caps_off = false;
      break;
    }
    Sleep(50);
  }
  if (g_ntun) ss_log("[detail] %d values watched in the game's data", g_ntun);
  if (props) {
    const DWORD p0 = GetTickCount();
    while (!Registered(kPropForce, kPropForceSite) && GetTickCount() - p0 < 180000) Sleep(50);
    if (Registered(kPropForce, kPropForceSite) && Readable(kPropDistance, sizeof kPropDistanceSig) &&
        memcmp(reinterpret_cast<const void*>(kPropDistance), kPropDistanceSig, sizeof kPropDistanceSig) == 0) {
      g_props_factor = g_draw_distance;
      g_props = 1;
      ss_log("[detail] props: their draw distance x%.2f, moved at %.0f m/s at most (the force tweak, from the present hook)",
             g_props_factor, kPropRate);
    } else {
      ss_log("[detail] props: not the known build's distance lookup; their draw distance stays the title's");
    }
  }
  if (shadow != 1.0) {
    // The image unpacked: the slots hold the known functions (reads only until then).
    const DWORD s0 = GetTickCount();
    while (!(Readable(kCsm[0].fit_slot, 4) && Rd(kCsm[0].fit_slot) == kCsm[0].fit) && GetTickCount() - s0 < 180000) Sleep(50);
    g_shadow_factor = static_cast<float>(shadow);  // first: a wrapper may run as soon as its slot is written
#define SS_CSM(K)                                                                 \
  HookCsm(K, reinterpret_cast<uintptr_t>(&CsmFitHook<K>), reinterpret_cast<uintptr_t>(&CsmSetupHook<K>), \
          reinterpret_cast<uintptr_t>(&CsmCompareHook<K>))
    const bool a = SS_CSM(0), b = SS_CSM(1);
#undef SS_CSM
    ss_log("[detail] sun shadow x%.2f: the shadow classes' compare, set-up and fit wrapped: 0xC314DC %s, 0xC31944 %s", shadow,
           a ? "yes" : "NO (not the known build's slots)", b ? "yes" : "NO (not the known build's slots)");
  }
  if (g_aniso == 0 && g_ntun == 0 && !g_bias_hook && !g_clamp_hook) return 0;

  DWORD hooked_at = 0;
  bool counted = false;
  const std::string fl = ss_conf("frame_log");
  const bool rate_log = !fl.empty() && !(fl == "0" || fl == "false" || fl == "no" || fl == "off");
  DWORD rate_at = 0;
  LONG r_calls = 0, r_sent = 0, r_skipped = 0, r_tex = 0, r_kind = 0;
  const bool hook = g_aniso != 0 || g_bias_hook || g_clamp_hook;  // as at start: the settings menu may set game later, the hook stays (passing through)
  for (;;) {
    if (g_ntun) ApplyTunables();
    if (hook) {
      InstallSamplerHook();
      if (g_sampler_orig && !hooked_at) hooked_at = GetTickCount();
      if (hooked_at && !counted && GetTickCount() - hooked_at > 20000) {
        counted = true;
        ss_log("[aniso] first 20 s: %ld sampler filter states set, %ld min filters changed; a sampler's texture became a mipped 2D "
               "one %ld times, another kind (no anisotropy) %ld times", g_sampler_calls, g_sampler_promoted, g_tex_mipped, g_tex_other);
      }
      // frame_log: the hooks' traffic every 10 s, a second (beside the [frames] lines).
      if (rate_log && hooked_at && GetTickCount() - rate_at >= 10000) {
        const DWORD now = GetTickCount();
        const LONG c = g_sampler_calls, se = g_sent, sk = g_skipped, t = g_tex_calls, k = g_tex_mipped + g_tex_other;
        if (rate_at) {
          const double sec = (now - rate_at) / 1000.0;
          ss_log("[aniso] a second: %.0f filter states from the title, %.0f SetSamplerState calls sent to the device, %.0f "
                 "re-sends skipped (unchanged), %.0f textures bound, %.0f texture kind changes",
                 (c - r_calls) / sec, (se - r_sent) / sec, (sk - r_skipped) / sec, (t - r_tex) / sec, (k - r_kind) / sec);
        }
        rate_at = now; r_calls = c; r_sent = se; r_skipped = sk; r_tex = t; r_kind = k;
      }
    }
    Sleep(100);
  }
  return 0;
}

}  // namespace

// postfx.cpp, after the update (TAA's jitter is in or out of the camera) and at the scene/UI split (and EndScene).
void ss_quality_scene_begin(void* device, bool jittered) {
  if (!g_sampler_orig || !g_bias_hook || !device) return;
  const bool want = jittered && g_lod_bias != 0.0f;
  if (!want) {
    ss_quality_scene_end(device);
    return;
  }
  g_scene_bias = true;
  SendBiases(device);  // also after the title's state blocks and a Reset, which set samplers behind the hook's back
  if (g_bias_frames++ == 0) ss_log("[taa] first frame with the texture LOD bias (%.2f)", g_lod_bias);
}

void ss_quality_scene_end(void* device) {
  if (!g_scene_bias || !g_sampler_orig || !device) return;
  g_scene_bias = false;
  SendBiases(device);
}

// picture.cpp's motion blur wrapper (render thread), around the game's pass: false when the hook is not in.
bool ss_quality_edge_clamp(bool on) {
  if (!g_sampler_orig || on == g_edge_clamp) return g_sampler_orig != nullptr;
  const uintptr_t r = Rd(kRenderer);
  IDirect3DDevice9* d = r ? reinterpret_cast<IDirect3DDevice9*>(Rd(r + kRendererDevice)) : nullptr;
  if (!d) return false;
  for (DWORD s = 0; s < 8; ++s)
    for (DWORD k = 0; k < 2; ++k) {
      const D3DSAMPLERSTATETYPE t = k ? D3DSAMP_ADDRESSV : D3DSAMP_ADDRESSU;
      if (on && FAILED(d->GetSamplerState(s, t, &g_clamp_want[s][k]))) g_clamp_want[s][k] = D3DTADDRESS_WRAP;
      g_sampler_orig(d, s, t, on ? static_cast<DWORD>(D3DTADDRESS_CLAMP) : g_clamp_want[s][k]);
    }
  g_edge_clamp = on;
  return true;
}

void ss_quality_start() { CreateThread(nullptr, 0, QualityWorker, nullptr, 0, nullptr); }

// patch.cpp's present hook (the game's thread, between two frames).
void ss_quality_present() {
  if (g_props) PropsPresent();
  // A Reset or a state block may have set samplers behind the hook's back: what was sent is not known any more.
  for (Sampler& s : g_samplers) s.sent_min = s.sent_aniso = kUnsent;
}

// The settings menu (menu.cpp): anisotropy at once while SetSamplerState is hooked (it is only when the start value
// was not game). A sampler takes the new level the next time the game sets one of its filter states. Otherwise false
// (RESTART NEEDED). draw_distance, opponent_cars and shadow_distance are restart-only: their watched values are chosen
// at start.
bool ss_quality_apply(const char* key, const char* value) {
  if (strcmp(key, "anisotropy") || !g_sampler_orig) return false;
  const int lv = ParseAnisotropy(value ? value : "");
  g_aniso = lv;
  ss_log("[aniso] from the settings menu: %s", lv == 0 ? "game (the title's own states)" : lv == 1 ? "off" : std::to_string(lv).c_str());
  return true;
}
