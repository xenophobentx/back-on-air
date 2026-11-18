// LOD smoothing: the scenery's mesh changes as a dithered cross-fade instead of a one-frame switch. The port's key,
// values and default:
//   lod_smoothing = off | on | high   (on; no key: on, with a splitsecond.conf)   SS_LOD_SMOOTHING (the fade's length in frames at 30 fps: 15 / 30)
//
// What the title does (the console's rule, the same code on PC): a scenery object has a resident low mesh (object
// +0x1C), a streamed full mesh (+0x20) and a high layer (+0x24). The view's sector list is built with each sector's
// LOD (0x7C5570, called through 0x78B0C0 right before the scene render 0x4B7BB0), and every pass draws the list's
// objects by the rule of 0x82DB70: LOD 1, or the streamed part not in, the low mesh; LOD 2-3 the full mesh. The two
// trade places in one frame when the sector crosses the mid distance and when the streamed part arrives.
//
// The port wraps the object's draw and calls it twice. On PC that draw is reached by direct calls only (the sector
// loop 0x7C1B10 -> 0x82EB30 -> 0x82DB70 -> mesh 0x832690 -> batch 0x832300 -> primitive 0x5089C0 -> the device), and
// the game's code is never written. So the second draw is made of data instead:
//   * A mesh is a table: passes (ids at mesh+8, records of 0x1C bytes at mesh+0x10), each with batches (0x18 bytes:
//     material, declaration, primitives) and each primitive (0x14 bytes) an index buffer, a vertex buffer, the
//     stride, the index count and the type. For a fading object a combined mesh is built in this DLL's memory: per
//     pass the new mesh's batches followed by the old mesh's, with copies of their primitives. The object's mesh
//     pointer is pointed at it for the frame's render only (from the first render target the frame sets, when the
//     view's list is built, to the present) and put back, so the game's update and streaming never see it.
//   * The copies' strides carry a mark in their high bits. The stride goes to the device's SetStreamSource as it is
//     (0x5089C0), so the SetStreamSource hook knows the draws that follow belong to a fade, which one, and whether
//     to the new or the old mesh; it passes the real stride on.
//   * DrawIndexedPrimitive for a marked stream swaps the pixel shader for a variant of the same shader that starts
//     with a screen-door mask (an 8x8 ordered dither of the pixel position, texkill), and sets one float constant
//     that shader does not read (the game's shaders read and its material binds write almost every register up to
//     c223, so the register is chosen per shader and the game's value is put back after the draw, from a copy kept
//     by the SetPixelShaderConstantF hook): the new mesh keeps the pixels whose threshold is at most the fade's
//     share, the old mesh exactly the others. Every pass that draws the object (pre-z, opaque, decals ...) takes the same mask, so
//     depth and the G-buffer agree. The sun shadow's caster passes (pass ids shadow_opaque / shadow_alphatest) take
//     the same mask at the same share, so a fading object's shadow fades over the same frames. There the pixel
//     position is the shadow atlas's texel: the cascades are moved by whole texels (0x897F72), so a static object's
//     mask stays on the same texels of the ground from frame to frame and does not crawl, and the shadow filter's
//     8 rotated taps over about 3 texels average the screen-door into a partial shadow. A caster pass drawn with no
//     pixel shader gets a bare program of the mask alone.
// The device slots are in d3d9.dll's memory (DXVK / wined3d): SetRenderTarget 37, DrawIndexedPrimitive 82,
// SetStreamSource 100, SetPixelShader 107, SetPixelShaderConstantF 109. The object's mesh
// pointer is a word in the game's heap. Nothing is written in the game's code or image.
//
// Five things fade:
//   * the low and the full mesh trading places (the mid distance, the streamed part arriving): both drawn, the new
//     one taking over;
//   * the high layer drawn with a full mesh that has just arrived at LOD 3: marked like the new mesh (it used to
//     appear whole in the frame the part came);
//   * an object whose sector the visibility map has just let through (its visible-set byte, or its LOD cap going
//     from 0: the caps act at console draw distance, where sectors come on at any distance when the camera changes
//     cell): its mesh alone, marked, fading in from the background.
//   * the power-play system's objects (the level's moving assets: the airport's turning radars on their masts; the
//     power-play set pieces): drawn by their own list, not the sector list, and switched on and off in one frame at
//     their draw distance (the title's fade parameters do not reach their shaders). Inside a band in front of that
//     distance their mesh is drawn marked at a share that falls with the distance, 1 at the band's near edge, 0 where
//     the title stops drawing them: they fade in as the camera comes and out as it goes (see ScanPowerplays).
//   * the shadow of a scenery object that switches in the cascades' own caster lists but not in the view (outside
//     the view's list the cascades pick the LOD by distance and the caps): its shadow passes alone, the shadow's
//     old mesh behind the new one, or fading in from nothing when the caps let its sector through (ScanShadows).
// Left hard: a sector coming back into the frustum (it is at the picture's edge) or inside the far distance (the
// title's own fade), sectors going (nothing is drawn any more), the full mesh lost to eviction, a camera cut (the
// camera moved more than 6 m + 150 m/s: nothing starts in that frame), split screen, and a sector new in a cascade's
// list (its shadow is drawn by the time the list can be read; mostly at the shadow's far end, inside the title's
// fade-out, or outside the picture). Off (the
// default), nothing is hooked.
//
// SS_LOD_TEST=<share>, a check and not a setting: every object drawing its full mesh with a low mesh to hand is drawn
// as a fade held at that share, to see the two masks in a still picture. SS_LOD_LOG=1: a line every ten seconds.
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>
#include "menu.h"
#include "ss_shared.h"

namespace {

constexpr uintptr_t kRenderer = 0xD64708;  // renderer; the device is renderer+0x578 (as patch.cpp)
constexpr uint32_t kRendererDevice = 0x578;
constexpr int kSetRenderTargetSlot = 37, kDrawIndexedSlot = 82, kSetStreamSlot = 100, kSetPixelShaderSlot = 107,
              kSetPsConstSlot = 109;

constexpr uintptr_t kGame = 0xD714D0;     // [[kGame]+0x28] the environment (0x4B636C)
constexpr uint32_t kGameEnv = 0x28;
constexpr uint32_t kEnvViewLists = 0x44;  // view v's sector list at +0x44 + v*0xC: +4 count, +8 entries of 0x40 bytes
constexpr uint32_t kEntrySize = 0x40;     // entry: +0 sector, +0x10 object count, +0x20 {object, kind} pairs
constexpr uint32_t kSectorLod = 0x16C;    // + view*4
constexpr uint32_t kObjLowPresent = 0x10, kObjPart = 0x14, kObjLow = 0x1C, kObjFull = 0x20, kObjHigh = 0x24;
constexpr uint32_t kEnvManager = 0x34;         // the sector manager: +0x18 sector count, +0x1C the sectors
constexpr uint32_t kManagerSectors = 0x18;
constexpr uint32_t kManagerCell = 0x26CC;      // view 0's cell of the visibility map (0x7C5629)
constexpr uint32_t kSectorVisible = 0x17C;     // a byte per cell: the sector is visible from it
// The visibility map's LOD caps (0x7C5749): [manager+0x25B8] -> +0 view 0's cell (-1: none), +0xC the cells (0x58
// bytes each, +0x54 a byte per sector: the highest LOD it may have from there, 0 = not drawn). Off altogether while
// the bool at 0xD82788 is set (draw_distance above console or visibility_lists = off sets it).
constexpr uint32_t kManagerCaps = 0x25B8;
constexpr uintptr_t kCapsOff = 0xD82788;
constexpr uint32_t kEnvCameras = 0x65C;        // view v's camera at +0x65C + v*0x2F4 (0x78B0C8)
constexpr uint32_t kCameraPosition = 0x10;
constexpr uint32_t kRecordSize = 0x1C, kBatchSize = 0x18, kPrimSize = 0x14, kBoundSize = 0x20;
// Pass name ids (made at start, in .data): the cascades' casters.
constexpr uintptr_t kShadowPassIds[] = {0xD824C0, 0xD824C4, 0xD824CC, 0xD824D0};
// The sun shadow's casters. Both shadow-map classes hand the casters a context whose byte +0x10 is 0 (0x8985B6,
// 0x899BE6), so the scenery's caster draw 0x78B260 builds cascade n's list at env+0x5C + n*0xC (0x7C5930: the
// sectors visible from the camera's cell whose corners pass the cascade's planes; entries of 0x40 bytes, the sector
// at +0) and draws every object of each listed sector (sector+0x174: +0xCC count, +0xD0 the objects; 0x82EB90) at
// the cascades' own LOD (0x7C1BD0): a sector in the view's list at its view LOD, at most Max Shadow Lod (0xD36864,
// 3); any other by its ground distance to view 0's camera (0x82CC20: the box at sector+8..+0x20, x and z), 3 under
// the high distance 0xD38A90, 2 under the mid distance 0xD38A98, else 1, at most the visibility map's cap.
constexpr uint32_t kEnvShadowLists = 0x5C;
constexpr int kCascades = 3;
constexpr uint32_t kSectorObjects = 0x174, kSectorIndex = 0x178, kSectorBox = 0x8, kRecObjCount = 0xCC, kRecObjs = 0xD0;
constexpr uintptr_t kHighDist = 0xD38A90, kMidDist = 0xD38A98, kMaxShadowLod = 0xD36864;

// The code this reads the layouts from, compared before anything is hooked (reads only).
struct Sig {
  uintptr_t va;
  uint8_t n;
  uint8_t b[16];
};
constexpr Sig kSigs[] = {
    {0x82DB82, 12, {0x8B, 0x4E, 0x14, 0x85, 0xC9, 0x74, 0x0E, 0xE8, 0xA2, 0x92, 0x2B, 0x00}},              // the object rule
    {0x8326B5, 16, {0x8D, 0x14, 0xC5, 0x00, 0x00, 0x00, 0x00, 0x2B, 0xD0, 0x8B, 0x41, 0x10, 0x8D, 0x34, 0x90, 0x85}},  // pass records
    {0x8323A8, 14, {0x8B, 0x46, 0x0C, 0x8B, 0x0E, 0x8B, 0x56, 0x08, 0x6A, 0x00, 0x50, 0x8B, 0x46, 0x04}},  // the primitive
    {0x7C1B62, 13, {0x8B, 0x47, 0x08, 0x8B, 0x04, 0x06, 0x8B, 0x8C, 0x98, 0x6C, 0x01, 0x00, 0x00}},        // the sector loop
    {0x82EB36, 8, {0x8B, 0x45, 0x00, 0x56, 0x8D, 0x75, 0x10, 0x8D}},                                      // the object list
    {0x4B636C, 10, {0x8B, 0x0D, 0xD0, 0x14, 0xD7, 0x00, 0x8B, 0x49, 0x28, 0x50}},                          // the environment
};
// The power-play objects' layouts (their part stays off on a mismatch, the scenery's does not).
constexpr Sig kPpSigs[] = {
    {0x81376B, 16, {0xF3, 0x0F, 0x10, 0x86, 0x40, 0x01, 0x00, 0x00, 0x0F, 0x2E, 0xC1, 0x9F, 0xF6, 0xC4, 0x44, 0x7A}},  // own distance
    {0x8138B7, 15, {0x8B, 0x86, 0xC0, 0x01, 0x00, 0x00, 0xF3, 0x0F, 0x5C, 0xD0, 0xF3, 0x0F, 0x11, 0x50, 0x10}},       // renderable
    {0x844788, 9, {0x8B, 0x46, 0x18, 0x83, 0x78, 0x10, 0x00, 0x74, 0x6A}},                                          // lean mesh
    {0x815562, 13, {0x8B, 0x8C, 0xF3, 0x90, 0x00, 0x00, 0x00, 0x8B, 0x03, 0x8B, 0x58, 0x18, 0x8B}},                  // object list
    {0x4B7CB9, 16, {0xA1, 0xD0, 0x14, 0xD7, 0x00, 0x69, 0xD2, 0xF4, 0x02, 0x00, 0x00, 0x8B, 0x88, 0x3C, 0x01, 0x00}},  // system, camera
    {0x813E98, 6, {0x8B, 0x0D, 0x10, 0x1A, 0xD7, 0x00}},                                                            // model cache
};
// The cascades' caster lists and their LOD rule (the shadows' own switches are left alone on a mismatch).
constexpr Sig kShadowSigs[] = {
    {0x78B292, 10, {0x8D, 0x0C, 0x76, 0x8D, 0x54, 0x8F, 0x5C, 0x52, 0x50, 0x56}},                                   // list n
    {0x7C1C53, 15, {0x8B, 0x46, 0x08, 0x8B, 0x1C, 0x07, 0x8B, 0x44, 0x24, 0x34, 0x8B, 0x84, 0x83, 0x6C, 0x01}},     // view LOD
    {0x7C1D48, 12, {0x8B, 0x46, 0x08, 0x8B, 0x14, 0x07, 0x8B, 0x8A, 0x74, 0x01, 0x00, 0x00}},                       // objects
    {0x82EB96, 12, {0x8B, 0xB7, 0xD0, 0x00, 0x00, 0x00, 0x8B, 0x8F, 0xCC, 0x00, 0x00, 0x00}},                       // object array
    {0x7C1BE7, 14, {0xF3, 0x0F, 0x11, 0x44, 0x24, 0x10, 0xF3, 0x0F, 0x10, 0x05, 0x98, 0x8A, 0xD3, 0x00}},           // distances
    {0x8985B6, 5, {0xC6, 0x44, 0x24, 0x54, 0x00}},                                                                 // the list path
};

// The power-play system's objects: the set pieces and the level's moving assets (the airport's turning radars, ...;
// moving_assets.xml, draw distances under powerplays/DrawDists/<level>/MovingAssets). Not in the sector list: the
// system [[kGame]+0x13C] keeps its manager at +0xD00, whose [+0] holds the objects (+0x14 count, +0x18 array). Each
// frame 0x8154A0 (called from the scene render at 0x4B7CCF) asks 0x813730 of every object: drawn when the distance
// from view's camera (camera +0x10) to its box (+0x218 min, +0x228 max; 0x579830) is at most its draw distance:
// its own (+0x140), else its group's ([+0x198]+0x88: the params file's), else size (+0x14C) x 5 + 10000, all only
// while enable_powerplay_draw_dists (0xD40A7C) is set, else no limit. Its fade range: +0x13C, else [+0x198]+0x8C,
// else 20 m (0xD40A94); the title hands start and range to the shader's Detail_FadeIn_* parameters (0x814E00),
// which the radars' shaders do not fade by: they appear and go in one frame at the draw distance. The object's
// renderable [+0x1C0] draws through 0x844750 / 0x844860 the lean mesh [[renderable+0x18]+0x10] (a shared model of
// 0x30 bytes from the model cache 0xD71A10: 0x813E9F / 0x7D7110; the mesh made by 0x8325C0, the scenery's form)
// with 0x832690 / 0x832920 / 0x8328A0 / 0x8329A0 -> batch 0x832300: the same primitives and strides as the scenery.
constexpr uint32_t kGamePowerplays = 0x13C, kPpManager = 0xD00;
constexpr uintptr_t kViews = 0xD66000;  // [+0x68 + view * 0x2F4] the view's camera (0x4B7CCA), +0x10 its position
constexpr uint32_t kPpRenderable = 0x1C0, kPpOwnDist = 0x140, kPpOwnFade = 0x13C, kPpGroup = 0x198, kPpSize = 0x14C,
                   kPpBox = 0x218, kRenderableModel = 0x18, kModelLean = 0x10, kModelSize = 0x30;
constexpr uintptr_t kPpDistsOn = 0xD40A7C, kPpAutoScale = 0xD40A84, kPpAutoOffset = 0xD40A8C, kPpAutoFade = 0xD40A94;

constexpr uint32_t kMark = 0x40000000, kMarkOld = 0x20000000, kMarkShadow = 0x10000000, kMarkSlotShift = 16,
                   kStrideMask = 0xFFFF;
// Which passes of a combined mesh carry the marked copies (the others keep the new mesh's own record).
constexpr uint8_t kPassView = 1, kPassShadow = 2, kPassAll = 3;
constexpr int kSlots = 48;     // the scenery's fades: mark slots 0..47
constexpr int kPpSlots = 16;   // the power-play objects': mark slots 48..63

volatile int g_frames = 0;       // the fade's length in frames at 30 fps; 0 off
float g_test_share = -1.0f;      // SS_LOD_TEST
bool g_log = false;              // SS_LOD_LOG

// ------------------------------------------------------------------ reading the game's memory

// A word of the game's heap. The readable regions it was found in are kept for the frame (the heap's regions are
// large, so a frame's reads take a few queries), 64 of them, the oldest replaced first (32 were kept and all of them
// forgotten when full); and the regions found NOT readable in the frame, 16 of them, so a word that points nowhere
// is not queried again on every read (SSPC-17 round 2).
struct Region {
  uintptr_t lo, hi;
};
constexpr int kRegionsKept = 64, kBadKept = 16;
Region g_regions[kRegionsKept], g_bad[kBadKept];
int g_nregions = 0, g_next_region = 0, g_nbad = 0, g_next_bad = 0;

void ForgetPages() { g_nregions = g_next_region = g_nbad = g_next_bad = 0; }

bool Get(uintptr_t p, uint32_t& v) {
  if (p < 0x10000 || p >= 0x7FFE0000 || (p & 3)) return false;
  bool ok = false;
  for (int i = 0; i < g_nregions && !ok; ++i) ok = p >= g_regions[i].lo && p < g_regions[i].hi;
  if (!ok) {
    for (int i = 0; i < g_nbad; ++i)
      if (p >= g_bad[i].lo && p < g_bad[i].hi) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!SS_VQ(reinterpret_cast<void*>(p), &mbi, sizeof mbi)) return false;
    const uintptr_t lo = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    if (mbi.State != MEM_COMMIT || mbi.Protect == 0 || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
      const int i = g_nbad < kBadKept ? g_nbad++ : g_next_bad;
      if (i == g_next_bad) g_next_bad = (g_next_bad + 1) % kBadKept;
      g_bad[i] = Region{lo, lo + mbi.RegionSize};
      return false;
    }
    const int i = g_nregions < kRegionsKept ? g_nregions++ : g_next_region;
    if (i == g_next_region) g_next_region = (g_next_region + 1) % kRegionsKept;
    g_regions[i] = Region{lo, lo + mbi.RegionSize};
  }
  v = *reinterpret_cast<const volatile uint32_t*>(p);
  return true;
}

bool GetBlock(uintptr_t p, void* out, size_t n) {
  uint32_t v;
  for (size_t i = 0; i < n; i += 4) {
    if (!Get(p + i, v)) return false;
    memcpy(static_cast<char*>(out) + i, &v, 4);
  }
  return true;
}

double Now() {
  static LARGE_INTEGER freq = {};
  if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return static_cast<double>(t.QuadPart) / static_cast<double>(freq.QuadPart);
}

// ------------------------------------------------------------------ shader bytecode

// What one pixel shader's token stream uses (Direct3D 9 shader model 2 / 3 bytecode: a version token, instructions
// whose length is in bits 24..27 of the opcode token, comment blocks, an end token).
struct ShaderUse {
  uint32_t version = 0;
  uint32_t tokens = 0, instructions = 0;
  bool c[224] = {};  // float constants read or defined
  bool temp[32] = {};
  bool vpos = false, ok = false;
};

uint32_t RegType(uint32_t r) { return ((r >> 28) & 7) | ((r >> 8) & 0x18); }

ShaderUse Parse(const uint32_t* t, size_t n) {
  ShaderUse u;
  if (n < 2) return u;
  u.version = t[0];
  size_t i = 1;
  while (i < n) {
    const uint32_t op = t[i];
    if (op == 0x0000FFFF) {
      u.ok = true;
      u.tokens = static_cast<uint32_t>(i + 1);
      break;
    }
    if ((op & 0xFFFF) == 0xFFFE) {  // comment
      i += 1 + ((op >> 16) & 0x7FFF);
      continue;
    }
    const uint32_t code = op & 0xFFFF, len = (op >> 24) & 0xF;
    ++u.instructions;
    if (code == 0x51 && i + 1 < n) {  // def cN
      if (RegType(t[i + 1]) == 2 && (t[i + 1] & 0x7FF) < 224) u.c[t[i + 1] & 0x7FF] = true;
    } else if (code == 0x1F) {  // dcl
      if (len >= 2 && i + 2 < n && RegType(t[i + 2]) == 17 && (t[i + 2] & 0x7FF) == 0) u.vpos = true;
    } else if (code != 0x30 && code != 0x2F) {  // defi, defb carry no registers
      for (uint32_t k = 1; k <= len && i + k < n; ++k) {
        const uint32_t r = t[i + k];
        if (!(r & 0x80000000)) continue;
        const uint32_t type = RegType(r), num = r & 0x7FF;
        if (type == 2 && num < 224) u.c[num] = true;
        if (type == 0 && num < 32) u.temp[num] = true;
      }
    }
    i += 1 + len;
  }
  return u;
}

// Shader model 3 tokens. A register parameter: bit 31, the type in bits 28..30 and 11..12, the number in bits 0..10;
// a destination's write mask in bits 16..19, a source's swizzle in bits 16..23.
constexpr uint32_t kTemp = 0x80000000, kConst = 0xA0000000, kMisc = 0x90001000;
constexpr uint32_t kX = 0, kY = 1, kZ = 2, kW = 3;
constexpr uint32_t Dst(uint32_t reg, uint32_t mask) { return reg | (mask << 16); }
constexpr uint32_t Src(uint32_t reg, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  return reg | ((a | (b << 2) | (c << 4) | (d << 6)) << 16);
}
constexpr uint32_t Op(uint32_t code, uint32_t params) { return code | (params << 24); }
constexpr uint32_t kMov = 1, kSub = 3, kMad = 4, kMul = 5, kFrc = 0x13, kTexkill = 0x41, kDcl = 0x1F, kDef = 0x51;

uint32_t Bits(float f) {
  uint32_t v;
  memcpy(&v, &f, 4);
  return v;
}

// The shader with a screen-door mask in front of its own code: the pixel's threshold t in an 8x8 ordered dither
// (the Bayer matrix, 0 .. 63/64, from the pixel position alone: b(q) = frac(q.x / 2 + q.y^2 * 3/4) on the position
// mod 8 and its halves, b(q) + b(q / 2) / 4 + b(q / 4) / 16), then texkill of t * fade.y + fade.x, `fade` being the
// float constant *fade_reg set for the draw (a register the shader does not read). (share, -1) keeps the pixels
// whose threshold is at most the share, (-share - 1/128, 1) exactly the others. Empty when the shader cannot take it
// (not ps_3_0, or it has no two temporaries and three constants to spare).
std::vector<uint32_t> DitherVariant(const std::vector<uint32_t>& code, const ShaderUse& u, int* fade_reg) {
  std::vector<uint32_t> out;
  if (!u.ok || u.version != 0xFFFF0300) return out;
  int temp[2], konst[3], nt = 0, nk = 0;
  for (int i = 31; i >= 0 && nt < 2; --i)
    if (!u.temp[i]) temp[nt++] = i;
  for (int i = 223; i >= 0 && nk < 3; --i)
    if (!u.c[i]) konst[nk++] = i;
  if (nt < 2 || nk < 3) return out;
  *fade_reg = konst[2];
  size_t at = 1;  // after the shader's declarations and definitions
  while (at < u.tokens) {
    const uint32_t op = code[at];
    if ((op & 0xFFFF) == 0xFFFE) {
      at += 1 + ((op >> 16) & 0x7FFF);
      continue;
    }
    const uint32_t c = op & 0xFFFF;
    if (c != kDcl && c != kDef && c != 0x30 && c != 0x2F) break;
    at += 1 + ((op >> 24) & 0xF);
  }
  if (at >= u.tokens) return out;
  const uint32_t a = kTemp | static_cast<uint32_t>(temp[0]), b = kTemp | static_cast<uint32_t>(temp[1]);
  const uint32_t k0 = kConst | static_cast<uint32_t>(konst[0]), k1 = kConst | static_cast<uint32_t>(konst[1]);
  const uint32_t fade = kConst | static_cast<uint32_t>(konst[2]), pos = kMisc;
  std::vector<uint32_t> add = {
      Op(kDef, 5), Dst(k0, 15), Bits(0.125f), Bits(8.0f), Bits(0.5f), Bits(0.75f),
      Op(kDef, 5), Dst(k1, 15), Bits(0.25f), Bits(0.0625f), 0, 0,
  };
  if (!u.vpos) add.insert(add.end(), {Op(kDcl, 2), 0x80000000, Dst(pos, 3)});
  auto bayer = [&](uint32_t qx, uint32_t qy) {  // b.x = frac(q.x / 2 + q.y^2 * 3/4), q in a
    add.insert(add.end(), {
        Op(kMul, 3), Dst(b, 1), Src(a, qy, qy, qy, qy), Src(a, qy, qy, qy, qy),
        Op(kMul, 3), Dst(b, 1), Src(b, kX, kX, kX, kX), Src(k0, kW, kW, kW, kW),
        Op(kMad, 4), Dst(b, 1), Src(a, qx, qx, qx, qx), Src(k0, kZ, kZ, kZ, kZ), Src(b, kX, kX, kX, kX),
        Op(kFrc, 2), Dst(b, 1), Src(b, kX, kX, kX, kX),
    });
  };
  add.insert(add.end(), {  // a.xy = the position mod 8
      Op(kMul, 3), Dst(a, 3), Src(pos, kX, kY, kY, kY), Src(k0, kX, kX, kX, kX),
      Op(kFrc, 2), Dst(a, 3), Src(a, kX, kY, kY, kY),
      Op(kMul, 3), Dst(a, 3), Src(a, kX, kY, kY, kY), Src(k0, kY, kY, kY, kY),
  });
  bayer(kX, kY);
  add.insert(add.end(), {Op(kMov, 2), Dst(b, 4), Src(b, kX, kX, kX, kX)});  // b.z: the sum
  add.insert(add.end(), {  // a.zw = floor(a.xy / 2)
      Op(kMul, 3), Dst(a, 12), Src(a, kX, kY, kX, kY), Src(k0, kZ, kZ, kZ, kZ),
      Op(kFrc, 2), Dst(b, 3), Src(a, kZ, kW, kW, kW),
      Op(kSub, 3), Dst(a, 12), Src(a, kX, kY, kZ, kW), Src(b, kX, kY, kX, kY),
  });
  bayer(kZ, kW);
  add.insert(add.end(), {Op(kMad, 4), Dst(b, 4), Src(b, kX, kX, kX, kX), Src(k1, kX, kX, kX, kX), Src(b, kZ, kZ, kZ, kZ)});
  add.insert(add.end(), {  // a.xy = floor(a.zw / 2)
      Op(kMul, 3), Dst(a, 3), Src(a, kZ, kW, kW, kW), Src(k0, kZ, kZ, kZ, kZ),
      Op(kFrc, 2), Dst(b, 3), Src(a, kX, kY, kY, kY),
      Op(kSub, 3), Dst(a, 3), Src(a, kX, kY, kY, kY), Src(b, kX, kY, kY, kY),
  });
  bayer(kX, kY);
  add.insert(add.end(), {
      Op(kMad, 4), Dst(b, 4), Src(b, kX, kX, kX, kX), Src(k1, kY, kY, kY, kY), Src(b, kZ, kZ, kZ, kZ),
      Op(kMad, 4), Dst(b, 15), Src(b, kZ, kZ, kZ, kZ), Src(fade, kY, kY, kY, kY), Src(fade, kX, kX, kX, kX),
      Op(kTexkill, 1), Dst(b, 15),
  });
  out.assign(code.begin(), code.begin() + static_cast<ptrdiff_t>(at));
  out.insert(out.end(), add.begin(), add.end());
  out.insert(out.end(), code.begin() + static_cast<ptrdiff_t>(at), code.begin() + u.tokens);
  return out;
}

// ------------------------------------------------------------------ the device

using SetRenderTargetFn = HRESULT(__stdcall*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using SetStreamFn = HRESULT(__stdcall*)(IDirect3DDevice9*, UINT, IDirect3DVertexBuffer9*, UINT, UINT);
using SetPixelShaderFn = HRESULT(__stdcall*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
using SetPsConstFn = HRESULT(__stdcall*)(IDirect3DDevice9*, UINT, const float*, UINT);
using DrawIndexedFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using CreatePixelShaderFn = HRESULT(__stdcall*)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
SetRenderTargetFn g_set_rt = nullptr;
SetStreamFn g_set_stream = nullptr;
SetPixelShaderFn g_set_ps = nullptr;
SetPsConstFn g_set_ps_const = nullptr;
DrawIndexedFn g_draw_indexed = nullptr;
IDirect3DDevice9* g_dev = nullptr;
DWORD g_thread = 0;  // the thread that renders (the first SetRenderTarget's)

IDirect3DPixelShader9* g_ps = nullptr;  // what the game has set
uint32_t g_mark = 0;                    // of stream 0, from the stride

// A pixel shader's dither variant (null: it cannot have one) and the register its mask reads. The original is held,
// so its address stays its own.
struct Variant {
  IDirect3DPixelShader9* ps = nullptr;
  int reg = 0;
};
std::unordered_map<IDirect3DPixelShader9*, Variant> g_variants;
std::unordered_map<IDirect3DPixelShader9*, IDirect3DPixelShader9*> g_original;  // variant -> the shader it was made of
float g_ps_const[224][4];  // the pixel shader float constants as the game set them
bool g_ps_const_known[224];  // set since the hook went in (else asked of the device once)

// What happens to the scenery, counted for the SS_LOD_LOG line: by kind and by the sector's distance.
enum Event { kCrossLod, kCrossArrival, kAppearSet, kAppearOther, kVanishSet, kVanishOther, kEvents };
const char* const kEventNames[kEvents] = {"low<->full by LOD", "low<->full by arrival/eviction", "appears by the visibility map",
                                          "appears otherwise (frustum, far distance)", "goes by the visibility map",
                                          "goes otherwise"};
constexpr float kNear = 150.0f, kMid = 400.0f;

struct Counters {
  uint32_t frames, scans, objects, twice, cut, cancelled, finished, no_mesh, no_slot;
  uint32_t events[kEvents][3];  // objects: nearer than kNear, than kMid, farther
  uint32_t faded[kEvents];
  uint32_t marked_draws, unmasked_draws, variants, no_variant, max_active, const_past, other_thread;
  double scan_s, moved;
  float jump_max;
  uint32_t const_high;  // highest pixel shader float constant the game set
  double since;
} g_n = {};

void Count(Event e, float dist, bool faded) {
  ++g_n.events[e][dist < kNear ? 0 : dist < kMid ? 1 : 2];
  g_n.faded[e] += faded;
}

Variant VariantOf(IDirect3DDevice9* d, IDirect3DPixelShader9* ps) {
  auto it = g_variants.find(ps);
  if (it != g_variants.end()) return it->second;
  IDirect3DPixelShader9* made = nullptr;
  int reg = 0;
  UINT size = 0;
  std::vector<uint32_t> code;
  if (SUCCEEDED(ps->GetFunction(nullptr, &size)) && size >= 8) {
    code.resize(size / 4);
    if (FAILED(ps->GetFunction(code.data(), &size))) code.clear();
  }
  const std::vector<uint32_t> variant = DitherVariant(code, Parse(code.data(), code.size()), &reg);
  if (!variant.empty()) {
    void** vt = *reinterpret_cast<void***>(d);
    const CreatePixelShaderFn create = reinterpret_cast<CreatePixelShaderFn>(vt[106]);
    if (FAILED(create(d, reinterpret_cast<const DWORD*>(variant.data()), &made))) made = nullptr;
  }
  ++(made ? g_n.variants : g_n.no_variant);
  if (!made && g_n.no_variant <= 4)
    ss_log("[lod] a pixel shader has no dither variant (version 0x%08X, %u tokens): its draws stay whole",
           code.empty() ? 0u : code[0], static_cast<unsigned>(code.size()));
  ps->AddRef();
  if (made) {
    g_original[made] = ps;
    ++ss_shader_map_gen;  // hdr.cpp's cache of Original() is made again
    ss_pipewarm_derived(made, ps, 1);
  }
  return g_variants[ps] = Variant{made, reg};
}

// The mask alone, for a marked draw with no pixel shader bound (a caster pass that writes depth only): a shader
// model 3 program that writes 0 to colour 0 behind the same screen-door. Made on first use; null if refused.
Variant g_bare;
bool g_bare_tried = false;

Variant BareVariant(IDirect3DDevice9* d) {
  if (g_bare_tried) return g_bare;
  g_bare_tried = true;
  const std::vector<uint32_t> base = {
      0xFFFF0300,
      Op(kDef, 5), Dst(kConst | 0, 15), 0, 0, 0, 0,             // def c0, 0, 0, 0, 0
      Op(kMov, 2), 0x800F0800, Src(kConst | 0, kX, kY, kZ, kW),  // mov oC0, c0
      0x0000FFFF,
  };
  int reg = 0;
  const std::vector<uint32_t> variant = DitherVariant(base, Parse(base.data(), base.size()), &reg);
  IDirect3DPixelShader9* made = nullptr;
  if (!variant.empty()) {
    void** vt = *reinterpret_cast<void***>(d);
    const CreatePixelShaderFn create = reinterpret_cast<CreatePixelShaderFn>(vt[106]);
    if (FAILED(create(d, reinterpret_cast<const DWORD*>(variant.data()), &made))) made = nullptr;
  }
  ss_log("[lod] the mask for draws with no pixel shader: %s", made ? "made" : "refused (those draws stay whole)");
  return g_bare = Variant{made, reg};
}

// ------------------------------------------------------------------ the fades

// One mesh pointer of the object pointed at a mesh of ours for the frame's render.
struct Swap {
  uint32_t field = 0, own = 0, made = 0;  // the object's field, the game's mesh, ours (0: none)
  bool in = false;
  std::vector<uint32_t> mem;
};

struct Fade {
  bool active = false, held = false;
  bool shadow_only = false;  // the cascades' own switch: only the shadow passes are marked
  uintptr_t object = 0;
  uint8_t to = 0;         // the mesh it draws now: 1 low, 2 full
  uint32_t mesh_old = 0;  // the mesh it drew before (0: nothing, the object fades in from the background)
  Swap swap[2];           // the drawn mesh (with the old one's batches behind), and the high layer drawn with it
  uint32_t visited = 0;   // the frame of the last scan that found the object drawing it
  uint32_t born = 0;      // the frame it started in
  double start = 0;
  float share = 0;
};
Fade g_fades[kSlots];

struct Seen {
  uint32_t frame = 0;   // the last frame a scan found it in the view's list
  uint8_t mesh = 0;     // what it draws by the list: 0 nothing, 1 the low mesh, 2 the full mesh
  uint8_t before = 0;   // what it drew in the frame before `frame`
  uint8_t lod = 0, lod_before = 0;
  bool no_pair = false;  // its meshes did not combine: not tried again
  int8_t slot = -1;
  // The cascades: the last frame the shadow scan found it in a caster list, what the shadow pass draws of it (0, 1, 2
  // as `mesh`) and that mesh.
  uint32_t shadow_frame = 0, shadow_own = 0;
  uint8_t shadow_mesh = 0;
};
std::unordered_map<uintptr_t, Seen> g_seen;

// A sector as the last scan found it: its LOD in view 0 (0: not drawn: not visible from the camera's cell, too far,
// or outside the frustum), whether the visibility map lets it be drawn (its visible-set byte and its LOD cap), its
// distance while listed.
struct Sector {
  uint32_t frame = 0, appeared_by_set = 0, shadow_frame = 0;
  uint8_t lod = 0, vis = 1;
  float dist = 0;
};
std::unordered_map<uint32_t, Sector> g_sectors;

uint32_t g_frame = 1;
bool g_scanned = false;   // this frame's scan is done (the mesh pointers are in)
uint32_t g_presents = 0;  // frames shown through patch.cpp's present hook: without it nothing is put in
double g_frame_time = 0;  // of the frame's scan: one share for all of a frame's passes
float g_cam[3];           // view 0's camera at the last scan
double g_cam_time = 0;

bool IsShadowPass(uint32_t id) {
  for (uintptr_t va : kShadowPassIds)
    if (*reinterpret_cast<const volatile uint32_t*>(va) == id) return true;
  return false;
}

struct MeshView {
  uint32_t passes = 0, ids = 0, records = 0;
  bool ok = false;
};

MeshView ReadMesh(uint32_t mesh) {
  MeshView m;
  m.ok = Get(mesh + 4, m.passes) && Get(mesh + 8, m.ids) && Get(mesh + 0x10, m.records) && m.passes <= 32;
  return m;
}

// A mesh of ours in `mem` for a fade: a header like the new mesh's, the pass ids of both, and per pass the new
// mesh's batches then the old one's (none when mesh_old is 0), each with marked copies of its primitives; the passes
// `which` does not name (the view's or the cascades') keep the new mesh's own record. 0 when a mesh does not read as
// the known form.
bool Marked(uint32_t id, uint8_t which) { return (which & (IsShadowPass(id) ? kPassShadow : kPassView)) != 0; }

uint32_t Combine(std::vector<uint32_t>& mem, uint32_t mesh_new, uint32_t mesh_old, int slot, uint8_t which) {
  const MeshView a = ReadMesh(mesh_new);
  MeshView b;
  if (mesh_old) b = ReadMesh(mesh_old);
  else b.ok = true;
  if (!a.ok || !b.ok || a.passes == 0) return 0;
  struct Rec {
    uint32_t id, w[2][7];
    bool has[2];
  };
  std::vector<Rec> recs;
  for (int side = 0; side < 2; ++side) {
    const MeshView& m = side ? b : a;
    for (uint32_t i = 0; i < m.passes; ++i) {
      uint32_t id;
      if (!Get(m.ids + i * 4, id)) return 0;
      size_t k = 0;
      while (k < recs.size() && recs[k].id != id) ++k;
      if (k == recs.size()) {
        if (side && !Marked(id, which)) continue;  // a pass left as it is: the new mesh alone
        recs.push_back(Rec{id, {}, {false, false}});
      }
      if (recs[k].has[side]) continue;
      if (!GetBlock(m.records + i * kRecordSize, recs[k].w[side], kRecordSize)) return 0;
      recs[k].has[side] = true;
    }
  }
  // Sizes, in words: header 16, ids, records, then per combined pass its batches, bounds, the +0xC words, primitives.
  size_t words = 16 + recs.size() + recs.size() * (kRecordSize / 4);
  for (const Rec& r : recs) {
    if (!Marked(r.id, which)) continue;
    for (int side = 0; side < 2; ++side) {
      if (!r.has[side]) continue;
      const uint32_t n = r.w[side][4];
      if (n > 4096) return 0;
      words += n * (kBatchSize / 4 + kBoundSize / 4 + 1);
      for (uint32_t i = 0; i < n; ++i) {
        uint32_t prims;
        if (!Get(r.w[side][5] + i * kBatchSize + 0x10, prims) || prims > 4096) return 0;
        words += prims * (kPrimSize / 4);
      }
    }
  }
  mem.assign(words, 0);
  uint32_t* base = mem.data();
  auto va = [&](size_t word) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(base + word)); };
  if (!GetBlock(mesh_new, base, 0x14)) return 0;
  size_t at = 16;
  const size_t ids = at;
  at += recs.size();
  const size_t records = at;
  at += recs.size() * (kRecordSize / 4);
  base[1] = static_cast<uint32_t>(recs.size());
  base[2] = va(ids);
  base[4] = va(records);
  for (size_t k = 0; k < recs.size(); ++k) {
    const Rec& r = recs[k];
    base[ids + k] = r.id;
    uint32_t* rec = base + records + k * (kRecordSize / 4);
    memcpy(rec, r.w[r.has[0] ? 0 : 1], kRecordSize);
    if (!Marked(r.id, which)) continue;
    const uint32_t kind = IsShadowPass(r.id) ? kMarkShadow : 0;
    const uint32_t n0 = r.has[0] ? r.w[0][4] : 0, n1 = r.has[1] ? r.w[1][4] : 0, n = n0 + n1;
    const size_t batches = at, bounds = batches + n * (kBatchSize / 4), extra = bounds + n * (kBoundSize / 4);
    at = extra + n;
    bool have_bounds = true, have_extra = true;
    for (int side = 0; side < 2; ++side) {
      if (!r.has[side]) continue;
      const uint32_t count = side ? n1 : n0, first = side ? n0 : 0;
      const uint32_t mark = kMark | kind | (side ? kMarkOld : 0) | (static_cast<uint32_t>(slot) << kMarkSlotShift);
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t* batch = base + batches + (first + i) * (kBatchSize / 4);
        if (!GetBlock(r.w[side][5] + i * kBatchSize, batch, kBatchSize)) return 0;
        const uint32_t prims = batch[4], from = batch[5];
        batch[5] = va(at);
        for (uint32_t p = 0; p < prims; ++p) {
          uint32_t* prim = base + at + p * (kPrimSize / 4);
          if (!GetBlock(from + p * kPrimSize, prim, kPrimSize)) return 0;
          if (prim[2] & ~kStrideMask) return 0;  // not a stride
          prim[2] |= mark;
        }
        at += prims * (kPrimSize / 4);
        if (!r.w[side][1] || !GetBlock(r.w[side][1] + i * kBoundSize, base + bounds + (first + i) * (kBoundSize / 4), kBoundSize))
          have_bounds = false;
        if (!r.w[side][3] || !Get(r.w[side][3] + i * 4, base[extra + first + i])) have_extra = false;
      }
    }
    // The culled draws (0x832700 / 0x832800) read the bounds and the +0xC words per batch: both meshes must have
    // them when either does.
    const bool want_bounds = (r.has[0] && r.w[0][1]) || (r.has[1] && r.w[1][1]);
    const bool want_extra = (r.has[0] && r.w[0][3]) || (r.has[1] && r.w[1][3]);
    if ((want_bounds && !have_bounds) || (want_extra && !have_extra)) return 0;
    rec[1] = want_bounds ? va(bounds) : 0;
    rec[3] = want_extra ? va(extra) : 0;
    rec[4] = n;
    rec[5] = va(batches);
  }
  return va(0);
}

void PutBack(Fade& f) {
  for (Swap& w : f.swap) {
    uint32_t cur;
    if (w.in && Get(f.object + w.field, cur) && cur == w.made) ss_write_word(reinterpret_cast<void*>(f.object + w.field), w.own);
    w.in = false;
  }
}

void End(Fade& f) {
  PutBack(f);
  f.active = false;
  auto it = g_seen.find(f.object);
  if (it != g_seen.end()) it->second.slot = -1;
}

// What the object draws at this LOD by the title's rule (0x82DB70), and its meshes: the low one when present, the
// full one when its streamed part is in, the high layer when it is drawn with the full one (LOD 3).
uint8_t DrawnMesh(uintptr_t object, uint32_t lod, uint32_t* low, uint32_t* full, uint32_t* high) {
  uint32_t present = 0, part = 0, head = 0, in = 0;
  *low = *full = *high = 0;
  if (!Get(object + kObjLowPresent, present) || !Get(object + kObjPart, part)) return 0;
  if (present) Get(object + kObjLow, *low);
  if (part && Get(part, head) && head && Get(head + 0x20, in) && in) Get(object + kObjFull, *full);
  if (lod == 0 || lod > 4) return 0;
  if (lod >= 2 && part && in) {
    if (lod == 4 || !*full) return 0;
    if (lod == 3) Get(object + kObjHigh, *high);
    return 2;
  }
  return present && *low ? 1 : 0;
}

// A fade for the object: its drawn mesh (and the high layer drawn with it) as marked copies, with `old` (the mesh it
// drew before, 0 for one that fades in from nothing) behind the drawn one.
bool Start(uintptr_t object, Seen& s, uint8_t to, uint32_t low, uint32_t full, uint32_t high, uint32_t old, bool held,
           uint8_t which = kPassAll) {
  int slot = -1;
  for (int i = 0; i < kSlots && slot < 0; ++i)
    if (!g_fades[i].active) slot = i;
  if (slot < 0) {
    ++g_n.no_slot;
    return false;
  }
  Fade& f = g_fades[slot];
  Swap& mesh = f.swap[0];
  mesh.field = to == 2 ? kObjFull : kObjLow;
  mesh.own = to == 2 ? full : low;
  mesh.made = mesh.own ? Combine(mesh.mem, mesh.own, old, slot, which) : 0;
  if (!mesh.made) {
    ++g_n.no_mesh;
    s.no_pair = true;
    return false;
  }
  Swap& layer = f.swap[1];
  layer.field = kObjHigh;
  layer.own = high;
  layer.made = high ? Combine(layer.mem, high, 0, slot, which) : 0;  // without it the layer is drawn whole, as before
  mesh.in = layer.in = false;
  f.object = object;
  f.to = to;
  f.mesh_old = old;
  f.active = true;
  f.held = held;
  f.shadow_only = which == kPassShadow;
  f.start = g_frame_time;
  f.share = 0;
  f.visited = f.born = g_frame;
  s.slot = static_cast<int8_t>(slot);
  return true;
}

uint8_t ByteAt(uint32_t p) {
  uint32_t w = 0;
  return Get(p & ~3u, w) ? static_cast<uint8_t>(w >> ((p & 3) * 8)) : 1;
}

// ------------------------------------------------------------------ the power-play objects

// One object drawn as a dithered share of itself for the frame: its renderable's model pointer pointed at a copy of
// the (shared) model whose lean mesh is a marked copy of the model's own. Per object, so the other instances of the
// same model stay whole.
struct PpFade {
  bool active = false, in = false;
  uintptr_t object = 0, renderable = 0;
  uint32_t model = 0, mesh = 0;  // the game's
  uint32_t made = 0;             // our mesh (in mem)
  uint32_t visited = 0;
  float share = 0;
  std::vector<uint32_t> mem;
  uint32_t proxy[kModelSize / 4];
};
PpFade g_pp[kPpSlots];
bool g_pp_known = false;  // the layouts read as the known build's
struct PpCounters {
  uint32_t scans, objects, banded, made, failed, no_slot, max_active;
} g_ppn = {};

float AsF(uint32_t v) {
  float f;
  memcpy(&f, &v, 4);
  return f;
}

float GetF(uintptr_t p, float fallback) {
  uint32_t v;
  return Get(p, v) ? AsF(v) : fallback;
}

void PpPutBack(PpFade& f) {
  uint32_t cur;
  if (f.in && Get(f.renderable + kRenderableModel, cur) && cur == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(f.proxy)))
    ss_write_word(reinterpret_cast<void*>(f.renderable + kRenderableModel), f.model);
  f.in = false;
}

// The share of the object the frame draws: 1 inside the draw distance less the band, falling to 0 at the draw
// distance, where the title stops drawing it. The band is the title's fade range, at least 2 m per frame of the
// fade's length (30 m on, 60 m high), at most half the distance. -1: no fade (no limit, or not in the band).
float PpShare(uintptr_t object, const float cam[3], int frames) {
  if (!*reinterpret_cast<const volatile uint8_t*>(kPpDistsOn)) return -1.0f;
  uint32_t group = 0;
  Get(object + kPpGroup, group);
  float dist = GetF(object + kPpOwnDist, 0.0f), range = GetF(object + kPpOwnFade, 0.0f);
  if (dist == 0.0f && group) dist = GetF(group + 0x88, 0.0f);
  if (dist == 0.0f)
    dist = GetF(object + kPpSize, 0.0f) * *reinterpret_cast<const volatile float*>(kPpAutoScale) +
           *reinterpret_cast<const volatile float*>(kPpAutoOffset);
  if (range == 0.0f && group) range = GetF(group + 0x8C, 0.0f);
  if (range == 0.0f) range = *reinterpret_cast<const volatile float*>(kPpAutoFade);
  if (!(dist > 0.0f) || dist > 1.0e6f) return -1.0f;
  float box[8];
  if (!GetBlock(object + kPpBox, box, 0x20)) return -1.0f;
  float d2 = 0;
  for (int i = 0; i < 3; ++i) {  // to the box (0x579830); 0 inside
    const float out = std::max(cam[i] - box[4 + i], box[i] - cam[i]);
    if (out > 0) d2 += out * out;
  }
  const float d = sqrtf(d2);
  float band = std::max(range, 2.0f * static_cast<float>(frames));
  band = std::min(band, 0.5f * dist);
  if (!(band > 0.0f) || d > dist) return -1.0f;
  const float share = (dist - d) / band;
  return share < 1.0f ? std::max(share, 1.0f / 64.0f) : -1.0f;
}

// Every frame at the scan: which power-play objects are in their band, and their models pointed at the faded copies.
void ScanPowerplays(int frames) {
  uint32_t game = 0, sys = 0, manager = 0, list = 0, count = 0, array = 0, views = 0;
  float cam[3];
  const bool ok = g_pp_known && Get(kGame, game) && game && Get(game + kGamePowerplays, sys) && sys &&
                  Get(sys + kPpManager, manager) && manager && Get(manager, list) && list && Get(list + 0x14, count) &&
                  Get(list + 0x18, array) && count <= 4096 && Get(kViews, views) && views &&
                  GetBlock(views + 0x68 + kCameraPosition, cam, 12);
  for (PpFade& f : g_pp) f.share = -1.0f;
  if (ok && frames >= 0 && (frames > 0 || g_test_share >= 0.0f)) {
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t object = 0, renderable = 0, model = 0, mesh = 0;
      if (!Get(array + i * 4, object) || !object) continue;
      ++g_ppn.objects;
      const float share = g_test_share >= 0.0f ? g_test_share : PpShare(object, cam, frames);
      if (share < 0.0f) continue;
      if (!Get(object + kPpRenderable, renderable) || !renderable || !Get(renderable + kRenderableModel, model) || !model ||
          !Get(model + kModelLean, mesh) || !mesh)
        continue;
      ++g_ppn.banded;
      PpFade* f = nullptr;
      for (PpFade& g : g_pp)
        if (g.active && g.object == object) f = &g;
      if (f && (f->renderable != renderable || f->model != model || f->mesh != mesh)) f->active = false;  // rebuilt
      if (!f || !f->active) {
        f = nullptr;
        for (PpFade& g : g_pp)
          if (!g.active && !f) f = &g;
        if (!f) {
          ++g_ppn.no_slot;
          continue;
        }
        const int slot = kSlots + static_cast<int>(f - g_pp);
        f->made = Combine(f->mem, mesh, 0, slot, kPassAll);  // the object and its shadow
        if (!f->made || !GetBlock(model, f->proxy, kModelSize)) {
          ++g_ppn.failed;
          continue;
        }
        f->proxy[kModelLean / 4] = f->made;
        f->object = object;
        f->renderable = renderable;
        f->model = model;
        f->mesh = mesh;
        f->active = true;
        f->in = false;
        ++g_ppn.made;
      }
      f->visited = g_frame;
      f->share = share;
    }
  }
  uint32_t active = 0;
  for (PpFade& f : g_pp) {
    if (!f.active) continue;
    if (f.visited != g_frame || f.share < 0.0f) {
      f.active = false;
      continue;
    }
    ++active;
    uint32_t cur;
    if (!f.in && Get(f.renderable + kRenderableModel, cur) && cur == f.model &&
        ss_write_word(reinterpret_cast<void*>(f.renderable + kRenderableModel),
                      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(f.proxy))))
      f.in = true;
  }
  if (active > g_ppn.max_active) g_ppn.max_active = active;
  ++g_ppn.scans;
}

// ------------------------------------------------------------------ the cascades' own switches

bool g_shadow_known = false;  // the caster lists and their LOD rule read as the known build's
struct ShadowCounters {
  uint32_t scans, sectors, objects, switched, switched_faded, appeared, appeared_faded, gone, marked, no_ps, unmasked;
  double scan_s;
} g_sh = {};

// The sector's ground distance from the camera as 0x82CC20 has it (x and z; negative inside its box).
float SectorDistance(uint32_t sector, const float cam[3]) {
  float box[7];
  if (!GetBlock(sector + kSectorBox, box, 0x1C)) return 1.0e9f;  // min x, y, z at +8, max at +0x18
  const float dx = std::max(cam[0] - box[4], box[0] - cam[0]), dz = std::max(cam[2] - box[6], box[2] - cam[2]);
  if (dx < 0.0f && dz < 0.0f) return std::max(dx, dz);
  const float ax = std::max(dx, 0.0f), az = std::max(dz, 0.0f);
  return sqrtf(ax * ax + az * az);
}

// Every frame at the scan: the objects of the cascades' caster lists (as the last frame's shadow pass built them;
// this frame's are built inside the pass) at this frame's cascade LOD (0x7C1BD0's rule, with this frame's camera,
// view LODs and caps). An object whose shadow draws another mesh than in the frame before, with no fade of its own
// running, gets a fade of its shadow passes only: the shadow's low and full mesh trade places as a dither, and a
// caster the caps let through fades in. (A sector new in the lists, or a shadow that goes, stays hard: its shadow
// was drawn by the time the list is read, or nothing is left to draw. Inside the view's list the shadow follows the
// view's LOD, so its switches are the view's and fade with them.)
void ScanShadows(uint32_t env, uint32_t manager, const float cam[3], bool can_start) {
  if (!g_shadow_known) return;
  const double t0 = Now();
  const float high = *reinterpret_cast<const volatile float*>(kHighDist), mid = *reinterpret_cast<const volatile float*>(kMidDist);
  const uint32_t max_lod = *reinterpret_cast<const volatile uint32_t*>(kMaxShadowLod);
  // The caps as 0x7C1BD0 reads them: none (3) while they are off or the camera is in no cell.
  const bool caps_off = *reinterpret_cast<const volatile uint8_t*>(kCapsOff) != 0;
  uint32_t caps = 0, cell = 0xFFFFFFFF, cells = 0, bytes = 0;
  if (!caps_off && (!Get(manager + kManagerCaps, caps) || !caps || !Get(caps, cell))) return;
  if (!caps_off && cell != 0xFFFFFFFF &&
      (cell >= 0x10000 || !Get(caps + 0xC, cells) || !cells || !Get(cells + cell * 0x58 + 0x54, bytes) || !bytes))
    return;
  for (int c = 0; c < kCascades; ++c) {
    uint32_t count = 0, entries = 0;
    const uintptr_t list = env + kEnvShadowLists + c * 0xC;
    if (!Get(list + 4, count) || !Get(list + 8, entries) || count > 4096 || (count && !entries)) continue;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t sector = 0, view_lod = 0, rec = 0, n = 0, objs = 0;
      if (!Get(entries + i * kEntrySize, sector) || !sector || !Get(sector + kSectorLod, view_lod)) continue;
      Sector& sec = g_sectors[sector];
      if (sec.shadow_frame == g_frame) continue;  // in an earlier cascade's list
      sec.shadow_frame = g_frame;
      uint32_t lod;
      if (view_lod) {
        lod = std::min(view_lod, max_lod);
      } else {
        const float d = SectorDistance(sector, cam);
        lod = high > d ? 3 : mid > d ? 2 : 1;
        if (!caps_off && cell != 0xFFFFFFFF) {
          uint32_t index = 0;
          if (!Get(sector + kSectorIndex, index) || index >= 0x10000) continue;
          lod = std::min<uint32_t>(lod, ByteAt(bytes + index));
        }
      }
      if (lod > 4 || !Get(sector + kSectorObjects, rec) || !rec || !Get(rec + kRecObjCount, n) || !Get(rec + kRecObjs, objs) ||
          n > 1024)
        continue;
      ++g_sh.sectors;
      for (uint32_t k = 0; k < n; ++k) {
        uint32_t object = 0, low = 0, full = 0, layer = 0;
        if (!Get(objs + k * 4, object) || !object) continue;
        ++g_sh.objects;
        const uint8_t mesh = DrawnMesh(object, lod, &low, &full, &layer);
        const uint32_t own = mesh == 2 ? full : mesh == 1 ? low : 0;
        Seen& s = g_seen[object];
        if (s.shadow_frame == g_frame) continue;
        const bool known = s.shadow_frame + 1 == g_frame;
        const uint8_t before = s.shadow_mesh;
        const uint32_t before_own = s.shadow_own;
        s.shadow_frame = g_frame;
        s.shadow_mesh = mesh;
        s.shadow_own = own;
        if (s.slot >= 0) {
          Fade& f = g_fades[s.slot];
          if (!f.active || f.object != object) {
            s.slot = -1;
          } else if (f.shadow_only) {
            const Swap& m = f.swap[0];
            if (mesh != f.to || (own != m.own && own != m.made) || (f.mesh_old && f.mesh_old != (f.to == 2 ? low : full))) {
              End(f);  // it switched again, or one of its meshes moved or went
              ++g_n.cancelled;
            } else {
              f.visited = g_frame;
            }
            continue;
          } else {
            continue;  // the object's own fade marks its shadow too
          }
        }
        if (!known || before == mesh) continue;
        if (!mesh) {
          ++g_sh.gone;
          continue;
        }
        // The old mesh must still be the object's: then both are drawn, the new one taking over.
        const uint32_t old = before == 2 ? full : before == 1 ? low : 0;
        const bool pair = !before || (old && old == before_own);
        const bool fade =
            can_start && pair && !s.no_pair && Start(object, s, mesh, low, full, layer, before ? old : 0, false, kPassShadow);
        if (before) {
          ++g_sh.switched;
          g_sh.switched_faded += fade;
        } else {
          ++g_sh.appeared;
          g_sh.appeared_faded += fade;
        }
      }
    }
  }
  ++g_sh.scans;
  g_sh.scan_s += Now() - t0;
}

// ------------------------------------------------------------------ the knock-over props

// The physics objects (posts, signs, barrels, barriers) are drawn by the manager [kPropManager] through 0x7AD7B0
// (view) and 0x7AD820 (casters), both into 0x818FE0, which draws each prop whose visibility byte is set through the
// model's node form (0xA35550, then 0xA356A0 called at 0x819307). The view's byte is a sphere test against a frustum
// whose far plane is the manager's distance [manager+0x28] (0x4B1CE1: the plane set at frame+0x1A9B0, 0x50B680), so a
// prop goes in one frame when its sphere passes that plane; the shader parameters Detail_FadeIn_* the title writes
// for it (0x849AB0) are read by no shader on PC. The casters are culled by the cascades' volumes, not by that plane:
// their shadows do not switch there. The draw is not a lean mesh table, so it is told apart at the device instead:
// 0x818FE0 calls the model draw with its own frame on the game's (this) thread's stack, so a DrawIndexedPrimitive
// made under it finds 0xA356A0's return address 0x81930C in the stack above its arguments, with 0x818FE0's frame
// right above (its esp = that slot + 0x14), whose caller's return address is 0x81A443 for the view and 0x81A4AE for
// the casters, and whose local at +0x70 is the prop's world matrix (3 x 4, rows of 0x10: the translation at +0xA0;
// 0x7D7CC0 at 0x8191DE). A view draw of a prop whose planar depth is inside a band in front of the far plane is
// drawn with the mask at a share falling from 1 to 0 at the plane.
constexpr uintptr_t kPropManager = 0xD7126C;  // [+0x28] the view's prop distance (0x7AE490 <- 0x818510)
constexpr uint32_t kPropModelRet = 0x81930C, kPropViewRet = 0x81A443, kPropShadowRet = 0x81A4AE;
constexpr uintptr_t kModelDrawLo = 0xA356A0, kModelDrawHi = 0xA35815;  // 0xA356A0's body (its callees return there)
constexpr Sig kPropSigs[] = {
    {0x819300, 12, {0xE8, 0x4B, 0xC2, 0x21, 0x00, 0x8B, 0xC8, 0xE8, 0x94, 0xC3, 0x21, 0x00}},  // the model draw
    {0x81A43E, 5, {0xE8, 0x9D, 0xEB, 0xFF, 0xFF}},                                          // view group draw
    {0x81A4A9, 5, {0xE8, 0x32, 0xEB, 0xFF, 0xFF}},                                          // caster group draw
    {0x8191CE, 9, {0x8D, 0x44, 0x24, 0x70, 0x50, 0x8D, 0x4C, 0x24, 0x24}},                  // the matrix at +0x70
    {0x4B1CE1, 9, {0x8B, 0x15, 0x6C, 0x12, 0xD7, 0x00, 0xD9, 0x42, 0x28}},                  // the far plane
    {0x7AE4B6, 8, {0xE8, 0x55, 0xA0, 0x06, 0x00, 0xD9, 0x5E, 0x28}},                        // the distance
};
bool g_prop_known = false;
struct PropView {
  bool on;
  float pos[3], fwd[3], dist, band;
} g_pv = {};
uint32_t g_prop_off[8];  // the slot's offsets above a draw's arguments seen so far
int g_nprop_off = 0;
uint32_t g_prop_tick = 0;
struct PropCounters {
  uint32_t draws, banded, masked, casters, scans, learned;
  float min_depth, max_depth;
} g_pn = {};

// At the frame's scan: view 0's camera (position +0x10, forward +0x40: 0x50B680) and the props' far plane.
void PropFrame(int frames, bool split) {
  g_pv.on = false;
  uint32_t mgr = 0, views = 0;
  if (!g_prop_known || frames <= 0 || split || !Get(kPropManager, mgr) || !mgr || !Get(kViews, views) || !views) return;
  float cam[7];
  if (!GetBlock(views + 0x68 + 0x10, cam, 12) || !GetBlock(views + 0x68 + 0x40, cam + 3, 12)) return;
  const float dist = GetF(mgr + 0x28, -1.0f), len = sqrtf(cam[3] * cam[3] + cam[4] * cam[4] + cam[5] * cam[5]);
  if (!(dist > 1.0f) || dist > 1.0e5f || !(len > 0.5f) || len > 2.0f) return;
  for (int i = 0; i < 3; ++i) {
    g_pv.pos[i] = cam[i];
    g_pv.fwd[i] = cam[3 + i] / len;
  }
  g_pv.dist = dist;
  g_pv.band = std::min(2.0f * static_cast<float>(frames), 0.5f * dist);
  g_pv.on = true;
}

// The share of the prop this draw belongs to (anchor: the draw's arguments on the game's stack), or -1: not a prop
// view draw, or not in the band.
float PropShare(uintptr_t anchor) {
  if (!g_pv.on) return -1.0f;
  const uintptr_t top = reinterpret_cast<uintptr_t>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
  auto at = [&](uintptr_t p) { return *reinterpret_cast<const volatile uint32_t*>(p); };
  uintptr_t slot = 0;
  for (int i = 0; i < g_nprop_off && !slot; ++i) {
    const uintptr_t p = anchor + g_prop_off[i];
    if (p + 0x120 < top && at(p) == kPropModelRet) slot = p;
  }
  if (!slot) {
    // Not at a depth seen before: look through the stack now and then (a missed draw is drawn whole).
    if ((++g_prop_tick & 31) != 0) return -1.0f;
    ++g_pn.scans;
    for (uintptr_t p = anchor; p < anchor + 0x2000 && p + 0x120 < top; p += 4)
      if (at(p) == kPropModelRet) {
        slot = p;
        break;
      }
    if (!slot) return -1.0f;
  }
  // The live chain: a return into 0xA356A0's body just below the slot (the model draw is running now, not a frame
  // left on the stack by an earlier one).
  bool live = false;
  for (uintptr_t p = slot - 0x60; p < slot && !live; p += 4)
    if (p >= anchor) {
      const uint32_t v = at(p);
      live = v > kModelDrawLo && v < kModelDrawHi;
    }
  if (!live) return -1.0f;
  const uintptr_t frame = slot + 0x14;  // 0x818FE0's esp
  int pass = -1;
  for (uintptr_t p = frame + 0xEC; p <= frame + 0x108 && pass < 0; p += 4) {
    const uint32_t v = at(p);
    pass = v == kPropViewRet ? 0 : v == kPropShadowRet ? 1 : -1;
  }
  if (pass < 0) return -1.0f;
  const uint32_t off = static_cast<uint32_t>(slot - anchor);
  bool known = false;
  for (int i = 0; i < g_nprop_off; ++i) known |= g_prop_off[i] == off;
  if (!known && g_nprop_off < 8) {
    g_prop_off[g_nprop_off++] = off;
    ++g_pn.learned;
  }
  ++g_pn.draws;
  if (pass == 1) {
    ++g_pn.casters;
    return -1.0f;  // the casters are not cut by the far plane
  }
  const float* t = reinterpret_cast<const float*>(frame + 0xA0);
  float z = 0;
  for (int i = 0; i < 3; ++i) z += (t[i] - g_pv.pos[i]) * g_pv.fwd[i];
  if (!(z == z)) return -1.0f;
  if (g_pn.draws == 1 || z < g_pn.min_depth) g_pn.min_depth = z;
  if (g_pn.draws == 1 || z > g_pn.max_depth) g_pn.max_depth = z;
  const float share = (g_pv.dist - z) / g_pv.band;
  if (share >= 1.0f) return -1.0f;
  ++g_pn.banded;
  return std::max(share, 1.0f / 64.0f);
}

// ------------------------------------------------------------------ the seclusion zones (SSPC-14)

// The level's precomputed visibility ("Seclusion/a;Mode" 0xD718E8 = 2: zones along the track from the subtrack's
// .seclusion file, cSeclusionZonesAlongTrack; else a grid). Every frame the seclusion update 0x4BAD60 -> 0x4BACC0
// finds view v's zone from its camera's track link (0x4D0EA0; else the grid, 0x7CE780) and stores it in the sector
// manager (env+0x34) at +0x2080 + v*4 (0x7C1BB0) and in the power-play manager at +0x78 + v*4 (0x7A6A80 -> 0x814C40).
// A zone (0x18 bytes) is a bit set: +0 bits, +4 words, +8 the words. The view's draw 0x7C3720 takes the zone path
// (0x7C37D0: 0x7C2670 -> 0x82ECB0 -> 0x82E900 -> the lean draw 0x82DD10 -> 0x832800) when the zone is set and the
// camera's own sector ([manager+0x25B8] -> the view's cell, the sector [manager+0x1C][cell]) is at LOD 2-3 with its
// streamed part in; the power-play objects (0x8152B0 -> 0x844860 -> 0x8328A0 / 0x8329A0) whenever their zone is set.
// There a pass's batch is drawn only when its renderable id (the pass record's +0xC words, one per batch) has its bit
// set in the zone (0x7CE430; a negative id always). So when the camera crosses into the next link's zone, every
// renderable the new zone lets through and the old one did not appears in one frame, wherever it is in the picture
// (bushes on top of a tunnel portal, a box in front of it), and the other way round they go.
//
// The fade: at the scan view 0's zone bits are compared with the last ones; each id that flips fades over the fade's
// length, in from nothing or out to nothing. While any does, both zone words point for the frame's render at a zone of
// ours whose bits are the new zone's plus the ids fading out, and are put back in the present hook. The draw is told
// apart in the device hook from the game's stack (this thread's, read only): the batch draw 0x832300 calls the
// primitive draw 0x5089C0 at 0x8323C6, so a draw under it has the return 0x8323CB above its arguments, and 0x38 bytes
// above that (0x3C when its debug branch pushed one more word) 0x832300's return into its caller: 0x832885 (0x832800),
// 0x832907 (0x8328A0), 0x832A16 (0x8329A0). Right below it 0x832300's prologue pushed the caller's esi (the pass
// record) at -8 and edi (the batch's index) at -0xC. The id is [[record+0xC] + index*4].
constexpr uint32_t kManagerZone = 0x2080, kPpManagerZone = 0x78, kSectorPart = 0x14;
constexpr uint32_t kZoneBatchRet = 0x8323CB, kZoneRets[] = {0x832885, 0x832907, 0x832A16};
constexpr uint32_t kZoneMaxWords = 1u << 16;
constexpr Sig kZoneSigs[] = {
    {0x7C37D0, 9, {0x8B, 0x84, 0xBE, 0x80, 0x20, 0x00, 0x00, 0x85, 0xC0}},              // the zone path
    {0x7C1BB8, 7, {0x89, 0x84, 0x91, 0x80, 0x20, 0x00, 0x00}},                          // the zone stored
    {0x7C3746, 9, {0x8B, 0x86, 0xB8, 0x25, 0x00, 0x00, 0x8B, 0x04, 0xB8}},              // the camera's cell
    {0x8152B0, 4, {0x8B, 0x44, 0xAF, 0x78}},                                            // the power plays' zone
    {0x7CE453, 6, {0x8B, 0x4A, 0x08, 0x85, 0x34, 0x81}},                                // the bit test
    {0x832850, 9, {0x8B, 0x4E, 0x0C, 0x8B, 0x14, 0xB9, 0x8D, 0x04, 0xB9}},              // ids, 0x832800
    {0x832880, 5, {0xE8, 0x7B, 0xFA, 0xFF, 0xFF}},                                      // its batch draw
    {0x8328E7, 9, {0x8B, 0x4E, 0x0C, 0x8B, 0x14, 0xB9, 0x8D, 0x04, 0xB9}},              // ids, 0x8328A0
    {0x832902, 5, {0xE8, 0xF9, 0xF9, 0xFF, 0xFF}},                                      // its batch draw
    {0x8329E7, 9, {0x8B, 0x4E, 0x0C, 0x8B, 0x14, 0xB9, 0x8D, 0x04, 0xB9}},              // ids, 0x8329A0
    {0x832A11, 5, {0xE8, 0xEA, 0xF8, 0xFF, 0xFF}},                                      // its batch draw
    {0x832300, 12, {0x51, 0xA1, 0x00, 0x47, 0xD6, 0x00, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x10}},  // ecx, esi, edi pushed
    {0x832361, 4, {0x55, 0x8B, 0xCE, 0xE8}},                                            // ebp pushed
    {0x832378, 1, {0x53}},                                                              // ebx pushed
    {0x8323C6, 5, {0xE8, 0xF5, 0x65, 0xCD, 0xFF}},                                      // the primitive draw
};
bool g_zone_known = false;
struct ZoneState {
  bool valid = false;            // `bits` hold the last frame's zone
  uint32_t zone = 0;             // the game's zone word at the last scan (0: none, or the zone path not taken)
  uint32_t words = 0;            // the id space, in words (the largest zone seen)
  std::vector<uint32_t> bits;    // what the game lets through now (all set without a zone)
  std::vector<uint32_t> shown;   // ours: `bits` and the ids fading out
  std::vector<uint8_t> dir;      // per id: 0 none, 1 fading in, 2 fading out
  std::vector<double> start;     // per id: the fade's start (seconds, Now())
  std::vector<uint32_t> active;  // the ids fading
  uint32_t proxy[6] = {};        // our zone
  uintptr_t put[2] = {};         // the words pointed at it this frame (0: none)
  uint32_t own[2] = {};          // and what they held
  bool on = false;               // the draws look for faded ids this frame
} g_zs;
uint32_t g_zone_off[8];  // where the 0x8323CB slot was found above a draw's arguments
int g_nzone_off = 0;
uint32_t g_zone_tick = 0;
struct ZoneCounters {
  uint32_t scans, changes, cut, in, out, no_path, draws, masked_in, masked_out, stack_scans, learned, put, overwritten;
  uint32_t max_active;
} g_zn = {};

// View 0's zone as its draw uses it: the game's word when the zone path is taken (0x7C37D0), else 0 (all drawn).
uint32_t DrawnZone(uint32_t manager, uint32_t zone) {
  uint32_t caps = 0, cell = 0, sectors = 0, sector = 0, lod = 0, objs = 0, part = 0, head = 0, in = 0;
  if (!zone || !Get(manager + kManagerCaps, caps) || !caps || !Get(caps, cell) || cell == 0xFFFFFFFF || cell > 0xFFFF ||
      !Get(manager + kManagerSectors + 4, sectors) || !Get(sectors + cell * 4, sector) || !sector ||
      !Get(sector + kSectorLod, lod) || lod < 2 || !Get(sector + kSectorObjects, objs) || !objs ||
      !Get(objs + kSectorPart, part) || !part || !Get(part, head) || !head || !Get(head + 0x20, in) || !in)
    return 0;
  return zone;
}

void ZoneEnd() {
  for (uint32_t id : g_zs.active) g_zs.dir[id] = 0;
  g_zs.active.clear();
}

// At the frame's scan: the zone's switches, the fades' shares, and our zone put in while any fades.
void ScanZones(uint32_t manager, int frames, bool cut, bool split, double now) {
  g_zs.on = false;
  if (!g_zone_known) return;
  ++g_zn.scans;
  uint32_t raw = 0, game = 0, sys = 0, pp = 0;
  if (split || !Get(manager + kManagerZone, raw)) {
    ZoneEnd();
    g_zs.valid = false;
    return;
  }
  const uint32_t zone = DrawnZone(manager, raw);
  if (raw && !zone) ++g_zn.no_path;
  uint32_t words = 0, data = 0;
  if (zone && (!Get(zone + 4, words) || !Get(zone + 8, data) || !data || words == 0 || words > kZoneMaxWords)) return;
  if (words > g_zs.words) {  // a larger id space: the new ids have been shown (all set) until now
    g_zs.bits.resize(words, 0xFFFFFFFFu);
    g_zs.shown.resize(words);
    g_zs.dir.resize(static_cast<size_t>(words) * 32, 0);
    g_zs.start.resize(static_cast<size_t>(words) * 32, 0.0);
    g_zs.words = words;
  }
  if (!g_zs.words) return;  // no zone seen yet
  const bool fading = frames > 0 && g_test_share < 0.0f;
  if (zone != g_zs.zone || !g_zs.valid) {
    std::vector<uint32_t> now_bits(g_zs.words, 0xFFFFFFFFu);
    if (zone && !GetBlock(data, now_bits.data(), static_cast<size_t>(words) * 4)) return;
    if (!fading || cut || !g_zs.valid) {
      ZoneEnd();
      if (cut && g_zs.valid) ++g_zn.cut;
    } else {
      ++g_zn.changes;
      const float len = static_cast<float>(frames) / 30.0f;
      for (uint32_t w = 0; w < g_zs.words; ++w) {
        uint32_t flip = now_bits[w] ^ g_zs.bits[w];
        while (flip) {
          const uint32_t b = static_cast<uint32_t>(__builtin_ctz(flip));
          flip &= flip - 1;
          const uint32_t id = w * 32 + b;
          const uint8_t to = (now_bits[w] >> b) & 1 ? 1 : 2;
          // A fade under way turns round where it is: the share drawn stays the same.
          float done = 0.0f;
          if (g_zs.dir[id]) done = 1.0f - std::min(1.0f, static_cast<float>(now - g_zs.start[id]) / len);
          else g_zs.active.push_back(id);
          g_zs.dir[id] = to;
          g_zs.start[id] = now - static_cast<double>(done * len);
          ++(to == 1 ? g_zn.in : g_zn.out);
        }
      }
    }
    g_zs.bits.swap(now_bits);
    g_zs.zone = zone;
    g_zs.valid = true;
  }
  if (g_zs.active.empty()) return;
  if (!fading) {
    ZoneEnd();
    return;
  }
  // The shares; the finished ones leave.
  const float len = static_cast<float>(frames) / 30.0f;
  memcpy(g_zs.shown.data(), g_zs.bits.data(), static_cast<size_t>(g_zs.words) * 4);
  size_t kept = 0;
  for (uint32_t id : g_zs.active) {
    if (now - g_zs.start[id] >= len) {
      g_zs.dir[id] = 0;
      continue;
    }
    g_zs.active[kept++] = id;
    if (g_zs.dir[id] == 2) g_zs.shown[id >> 5] |= 1u << (id & 31);
  }
  g_zs.active.resize(kept);
  if (kept > g_zn.max_active) g_zn.max_active = static_cast<uint32_t>(kept);
  if (!kept) return;
  g_zs.on = true;
  // Our zone in both words for the frame's render: the sector manager's when its zone path is taken (else the game
  // draws all of it anyway), the power-play manager's when it holds the same zone.
  if (zone) GetBlock(zone, g_zs.proxy, sizeof g_zs.proxy);
  g_zs.proxy[0] = g_zs.words * 32;
  g_zs.proxy[1] = g_zs.words;
  g_zs.proxy[2] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_zs.shown.data()));
  const uint32_t mine = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_zs.proxy));
  uintptr_t where[2] = {manager + kManagerZone, 0};
  if (Get(kGame, game) && game && Get(game + kGamePowerplays, sys) && sys && Get(sys + kPpManager, pp) && pp)
    where[1] = pp + kPpManagerZone;
  for (int i = 0; i < 2; ++i) {
    uint32_t cur = 0;
    // Only while the view's zone path is taken (else the game draws all the scenery anyway, and the power-play
    // objects keep the game's zone: ours would let them in for the fade and drop them after it).
    if (g_zs.put[i] || !where[i] || raw != zone || !Get(where[i], cur) || cur != raw) continue;
    if (i == 1 && !raw) continue;  // the power-play objects are all drawn without a zone
    if (ss_write_word(reinterpret_cast<void*>(where[i]), mine)) {
      g_zs.put[i] = where[i];
      g_zs.own[i] = cur;
      ++g_zn.put;
    }
  }
}

// The present hook: the game's zones back.
void ZonePutBack() {
  const uint32_t mine = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_zs.proxy));
  for (int i = 0; i < 2; ++i) {
    if (!g_zs.put[i]) continue;
    uint32_t cur = 0;
    if (Get(g_zs.put[i], cur) && cur == mine) ss_write_word(reinterpret_cast<void*>(g_zs.put[i]), g_zs.own[i]);
    else ++g_zn.overwritten;  // the game stored its zone again before the present
    g_zs.put[i] = 0;
  }
  g_zs.on = false;
}

// The share of a zone draw whose renderable is fading (anchor: the draw's arguments on the game's stack), or -1.
// `old`: it fades out (drawn on the pixels a fade-in at that share leaves).
float ZoneShare(uintptr_t anchor, bool* old) {
  const uintptr_t top = reinterpret_cast<uintptr_t>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
  auto at = [&](uintptr_t p) { return *reinterpret_cast<const volatile uint32_t*>(p); };
  auto caller = [&](uintptr_t slot) -> uintptr_t {  // 0x832300's return into a zone draw, above the slot
    for (uintptr_t s = slot + 0x38; s <= slot + 0x3C; s += 4) {
      const uint32_t v = at(s);
      for (uint32_t r : kZoneRets)
        if (v == r) return s;
    }
    return 0;
  };
  uintptr_t slot = 0, ret = 0;
  for (int i = 0; i < g_nzone_off && !ret; ++i) {
    const uintptr_t p = anchor + g_zone_off[i];
    if (p + 0x40 < top && at(p) == kZoneBatchRet) {
      slot = p;
      ret = caller(p);
    }
  }
  if (!ret) {
    if ((++g_zone_tick & 31) != 0) return -1.0f;
    ++g_zn.stack_scans;
    for (uintptr_t p = anchor; p < anchor + 0x2000 && p + 0x40 < top && !ret; p += 4)
      if (at(p) == kZoneBatchRet) {
        slot = p;
        ret = caller(p);
      }
    if (!ret) return -1.0f;
  }
  const uint32_t off = static_cast<uint32_t>(slot - anchor);
  bool known = false;
  for (int i = 0; i < g_nzone_off; ++i) known |= g_zone_off[i] == off;
  if (!known && g_nzone_off < 8) {
    g_zone_off[g_nzone_off++] = off;
    ++g_zn.learned;
  }
  const uint32_t record = at(ret - 8), index = at(ret - 0xC);
  uint32_t n = 0, ids = 0, id = 0;
  if (!Get(record + 0x10, n) || index >= n || !Get(record + 0xC, ids) || !ids || !Get(ids + index * 4, id)) return -1.0f;
  ++g_zn.draws;
  if (id >= g_zs.words * 32 || !g_zs.dir[id]) return -1.0f;
  *old = g_zs.dir[id] == 2;
  ++(*old ? g_zn.masked_out : g_zn.masked_in);
  const float share = static_cast<float>((g_frame_time - g_zs.start[id]) * 30.0 / std::max(1, static_cast<int>(g_frames)));
  return std::min(std::max(share, 1.0f / 64.0f), 1.0f - 1.0f / 64.0f);
}

// One pass, at the first render target the game sets in a frame (the view's list is built by then): every sector's
// LOD and visible-set byte, what every listed object draws now, the fades that start, and the fades' meshes put in.
void Scan() {
  if (!g_presents) return;
  const double t0 = Now();
  const double dt = t0 - g_frame_time;
  g_frame_time = t0;
  g_scanned = true;
  uint32_t game = 0, env = 0, count = 0, entries = 0, second = 0, manager = 0, nsectors = 0, sectors = 0, cell = 0;
  if (!Get(kGame, game) || !game || !Get(game + kGameEnv, env) || !env) return;
  if (!Get(env + kEnvViewLists + 4, count) || !Get(env + kEnvViewLists + 8, entries) || count > 4096) return;
  if (!Get(env + kEnvManager, manager) || !Get(manager + kManagerSectors, nsectors) || !Get(manager + kManagerSectors + 4, sectors) ||
      !Get(manager + kManagerCell, cell) || nsectors > 8192)
    return;
  const bool split = Get(env + kEnvViewLists + 0xC + 4, second) && second != 0;
  const int frames = g_frames;
  // A camera cut: the camera moved farther than driving does. Nothing starts to fade in such a frame.
  float cam[3] = {};
  bool cut = false;
  const bool have_cam = GetBlock(env + kEnvCameras + kCameraPosition, cam, 12);
  if (have_cam) {
    float d2 = 0;
    for (int i = 0; i < 3; ++i) d2 += (cam[i] - g_cam[i]) * (cam[i] - g_cam[i]);
    const float jump = sqrtf(d2);
    const bool known = g_cam_time != 0 && dt > 0 && dt < 0.5;
    cut = !known || jump > 6.0f + 150.0f * static_cast<float>(dt);
    if (known && !cut) {
      g_n.moved += jump;
      if (jump > g_n.jump_max) g_n.jump_max = jump;
    }
    memcpy(g_cam, cam, 12);
    g_cam_time = t0;
  }
  if (cut) ++g_n.cut;
  ScanPowerplays(split ? -1 : frames);  // split screen: none (the band is the first view's)
  PropFrame(frames, split);
  ScanZones(manager, frames, cut, split, t0);
  // The sectors: who came on and went, and why.
  uint32_t caps = 0, cap_cell = 0xFFFFFFFF, cap_cells = 0, cap_bytes = 0;
  if (*reinterpret_cast<const volatile uint8_t*>(kCapsOff) == 0 && Get(manager + kManagerCaps, caps) && caps &&
      Get(caps, cap_cell) && cap_cell < 0x10000 && Get(caps + 0xC, cap_cells) && cap_cells)
    Get(cap_cells + cap_cell * 0x58 + 0x54, cap_bytes);
  for (uint32_t i = 0; i < nsectors && !split; ++i) {
    uint32_t sector = 0, lod = 0, map = 0;
    if (!Get(sectors + i * 4, sector) || !sector || !Get(sector + kSectorLod, lod)) continue;
    const uint8_t set = Get(sector + kSectorVisible, map) && map && cell < 0x10000 ? ByteAt(map + cell) : 1;
    const uint8_t vis = set && (!cap_bytes || ByteAt(cap_bytes + i));
    Sector& sec = g_sectors[sector];
    if (sec.frame + 1 == g_frame && !cut) {
      if (!sec.lod && lod && !sec.vis) sec.appeared_by_set = g_frame;
      if (sec.lod && !lod) Count(vis ? kVanishOther : kVanishSet, sec.dist, false);
    }
    sec.frame = g_frame;
    sec.lod = static_cast<uint8_t>(lod);
    sec.vis = vis;
  }
  uint32_t objects = 0;
  bool twice = false;
  for (uint32_t i = 0; i < count && !split; ++i) {
    const uintptr_t e = entries + i * kEntrySize;
    uint32_t sector = 0, lod = 0, n = 0, dist_bits = 0;
    if (!Get(e, sector) || !Get(sector + kSectorLod, lod) || !Get(e + 4, dist_bits) || !Get(e + 0x10, n) || n > 4) continue;
    float dist;
    memcpy(&dist, &dist_bits, 4);
    Sector& sec = g_sectors[sector];
    sec.dist = dist;
    const bool by_set = sec.appeared_by_set == g_frame;
    for (uint32_t k = 0; k < n; ++k) {
      uint32_t object = 0, low = 0, full = 0, high = 0;
      if (!Get(e + 0x20 + k * 8, object) || !object) continue;
      ++objects;
      const uint8_t mesh = DrawnMesh(object, lod, &low, &full, &high);
      Seen& s = g_seen[object];
      const bool first = s.frame != g_frame;
      if (first) {
        s.before = s.frame + 1 == g_frame ? s.mesh : 0;
        s.lod_before = s.lod;
        s.frame = g_frame;
      } else if (s.mesh != mesh) {
        twice = true;
      }
      s.mesh = mesh;
      s.lod = static_cast<uint8_t>(lod);
      if (s.slot >= 0) {
        Fade& f = g_fades[s.slot];
        const Swap& m = f.swap[0];
        const Swap& h = f.swap[1];
        const uint32_t now = f.to == 2 ? full : low, other = f.to == 2 ? low : full;
        uint32_t layer = 0;
        if (h.made) Get(object + kObjHigh, layer);
        if (!f.active || f.object != object) {
          s.slot = -1;
        } else if (f.shadow_only) {
          // The cascades' own switch, kept by the shadow scan; the view's own switch takes over.
          if (first && s.before != mesh) {
            End(f);
            ++g_n.cancelled;
          }
        } else if (mesh != f.to || (now != m.own && now != m.made) || (f.mesh_old && other != f.mesh_old) ||
                   (h.made && layer != h.own && layer != h.made)) {
          End(f);  // it draws something else now, or one of its meshes moved or went
          ++g_n.cancelled;
        } else {
          f.visited = g_frame;
        }
      }
      if (s.slot >= 0 || !first || !mesh) continue;
      const bool can = !s.no_pair && !cut;
      if (g_test_share >= 0.0f) {
        if (mesh == 2 && low && !s.no_pair) Start(object, s, 2, low, full, high, low, true);
      } else if (s.before && s.before != mesh) {
        // The low and the full mesh trade places: the sector crossed the mid distance, or the streamed part came
        // (or went: then the old mesh is gone and the switch stays hard).
        const uint32_t old = mesh == 1 ? full : low;
        const bool fade = frames > 0 && can && old && Start(object, s, mesh, low, full, high, old, false);
        Count((s.lod_before >= 2) == (lod >= 2) ? kCrossArrival : kCrossLod, dist, fade);
      } else if (!s.before) {
        // It was not drawn in the last frame. Its sector became visible from the camera's cell: it fades in from
        // the background. Back in the frustum, or inside the far distance (the title's own fade): as it is.
        const bool fade = by_set && frames > 0 && can && Start(object, s, mesh, low, full, high, 0, false);
        Count(by_set ? kAppearSet : kAppearOther, dist, fade);
      }
    }
  }
  if (twice) ++g_n.twice;
  if (!split && have_cam) ScanShadows(env, manager, cam, frames > 0 && !cut && g_test_share < 0.0f);
  uint32_t active = 0;
  for (Fade& f : g_fades) {
    if (!f.active) continue;
    if (f.visited != g_frame || split || (frames <= 0 && !f.held)) {
      End(f);  // not in the list any more
      ++g_n.cancelled;
      continue;
    }
    f.share = f.held ? g_test_share : static_cast<float>((t0 - f.start) * 30.0 / frames) + 1.0f / 64.0f;
    if (f.share >= 1.0f) {
      End(f);
      ++g_n.finished;
      continue;
    }
    ++active;
    for (Swap& w : f.swap) {
      uint32_t cur;
      if (w.made && !w.in && Get(f.object + w.field, cur) && cur == w.own &&
          ss_write_word(reinterpret_cast<void*>(f.object + w.field), w.made))
        w.in = true;
    }
  }
  if (active > g_n.max_active) g_n.max_active = active;
  if (g_log) {
    ++g_n.scans;
    g_n.objects += objects;
    g_n.scan_s += Now() - t0;
  }
}

void Report() {
  const double now = Now();
  if (!g_n.since) g_n.since = now;
  if (now - g_n.since < 10.0) return;
  const double f = g_n.frames ? g_n.frames : 1, secs = now - g_n.since;
  std::string events;
  for (int e = 0; e < kEvents; ++e) {
    char b[160];
    snprintf(b, sizeof b, "%s%s %u / %u / %u (%u faded)", e ? "; " : "", kEventNames[e], g_n.events[e][0], g_n.events[e][1],
             g_n.events[e][2], g_n.faded[e]);
    events += b;
  }
  ss_log("[lod] %.0f s, %u frames; objects under %.0f m / under %.0f m / farther: %s", secs, g_n.frames, kNear, kMid, events.c_str());
  ss_log("[lod] list read %.1f us a frame (%.0f objects), two meshes for one object in %u frames; fades finished %u, cancelled %u, "
         "no mesh pair %u, no slot %u, most at once %u; camera cuts %u, %.0f m/s, longest step %.1f m; marked draws %.1f a "
         "frame (%u without a variant), variants %u made / %u not; constants to c%u, %u past c223, other threads %u",
         g_n.scans ? g_n.scan_s * 1e6 / g_n.scans : 0.0, g_n.scans ? static_cast<double>(g_n.objects) / g_n.scans : 0.0, g_n.twice,
         g_n.finished, g_n.cancelled, g_n.no_mesh, g_n.no_slot, g_n.max_active, g_n.cut, g_n.moved / secs, g_n.jump_max,
         g_n.marked_draws / f, g_n.unmasked_draws, g_n.variants, g_n.no_variant, g_n.const_high, g_n.const_past, g_n.other_thread);
  ss_log("[lod] power-play objects (moving assets, set pieces): %.0f a frame, %.1f a frame in their fade band, %u copies made, "
         "%u failed, %u without a slot, most at once %u",
         g_ppn.scans ? static_cast<double>(g_ppn.objects) / g_ppn.scans : 0.0,
         g_ppn.scans ? static_cast<double>(g_ppn.banded) / g_ppn.scans : 0.0, g_ppn.made, g_ppn.failed, g_ppn.no_slot,
         g_ppn.max_active);
  ss_log("[lod] shadows: cascade lists %.0f sectors / %.0f objects a frame, read in %.1f us; casters switching low<->full %u (%u "
         "faded), coming on %u (%u faded), going %u; marked shadow draws %.1f a frame (%u with no pixel shader, %u unmasked)",
         g_sh.scans ? static_cast<double>(g_sh.sectors) / g_sh.scans : 0.0, g_sh.scans ? static_cast<double>(g_sh.objects) / g_sh.scans : 0.0,
         g_sh.scans ? g_sh.scan_s * 1e6 / g_sh.scans : 0.0, g_sh.switched, g_sh.switched_faded, g_sh.appeared, g_sh.appeared_faded,
         g_sh.gone, g_sh.marked / f, g_sh.no_ps, g_sh.unmasked);
  ss_log("[lod] props: far plane %.0f m, band %.0f m; prop draws %.1f a frame (%.1f in the band, %.1f masked, %.1f casters), "
         "depth %.0f .. %.0f m; stack scans %u, depths learned %u (%d kept)",
         g_pv.dist, g_pv.band, g_pn.draws / f, g_pn.banded / f, g_pn.masked / f, g_pn.casters / f, g_pn.min_depth, g_pn.max_depth,
         g_pn.scans, g_pn.learned, g_nprop_off);
  ss_log("[lod] seclusion zones: %u scans, zone path not taken in %u; zone changes %u (%u at a camera cut), ids fading in %u / "
         "out %u, most at once %u; our zone put in %u times, overwritten before the present %u; zone draws %.1f a frame "
         "(%.1f masked in, %.1f masked out); stack scans %u, depths learned %u (%d kept)",
         g_zn.scans, g_zn.no_path, g_zn.changes, g_zn.cut, g_zn.in, g_zn.out, g_zn.max_active, g_zn.put, g_zn.overwritten,
         g_zn.draws / f, g_zn.masked_in / f, g_zn.masked_out / f, g_zn.stack_scans, g_zn.learned, g_nzone_off);
  g_ppn = {};
  g_sh = {};
  g_pn = {};
  g_zn = {};
  const uint32_t high = g_n.const_high, variants = g_n.variants, no_variant = g_n.no_variant;
  g_n = {};
  g_n.const_high = high;
  g_n.variants = variants;
  g_n.no_variant = no_variant;
  g_n.since = now;
}

// ------------------------------------------------------------------ the hooks

bool Mine(IDirect3DDevice9* d) {
  if (d != g_dev || ss_own_drawing != 0) return false;
  const DWORD t = GetCurrentThreadId();
  if (!g_thread) g_thread = t;
  if (g_thread != t) {
    ++g_n.other_thread;
    return false;
  }
  return true;
}

HRESULT __attribute__((stdcall, force_align_arg_pointer)) SetRenderTargetHook(IDirect3DDevice9* d, DWORD index, IDirect3DSurface9* s) {
  // Once a frame: the first render target a frame sets comes after the view's list was built (measured: in no
  // frame did a later scan find another mesh than the first).
  if (!g_scanned && Mine(d) && (g_frames > 0 || g_test_share >= 0.0f || g_log)) Scan();
  return g_set_rt(d, index, s);
}

HRESULT __attribute__((stdcall, force_align_arg_pointer)) SetStreamHook(IDirect3DDevice9* d, UINT stream, IDirect3DVertexBuffer9* vb, UINT offset,
                                                                       UINT stride) {
  if (stream == 0 && d == g_dev) {
    g_mark = stride & kMark ? stride : 0;
    if (g_mark) stride &= kStrideMask;
  }
  return g_set_stream(d, stream, vb, offset, stride);
}

HRESULT __attribute__((stdcall, force_align_arg_pointer)) SetPixelShaderHook(IDirect3DDevice9* d, IDirect3DPixelShader9* ps) {
  if (d == g_dev && ss_own_drawing == 0) g_ps = ps;
  return g_set_ps(d, ps);
}

HRESULT __attribute__((stdcall, force_align_arg_pointer)) SetPsConstHook(IDirect3DDevice9* d, UINT start, const float* data, UINT count) {
  if (d == g_dev && ss_own_drawing == 0 && count && data) {
    if (start < 224 && count <= 224 - start) {
      memcpy(g_ps_const[start], data, count * 16);
      memset(g_ps_const_known + start, 1, count);
      if (start + count - 1 > g_n.const_high) g_n.const_high = start + count - 1;
    } else {
      ++g_n.const_past;
    }
  }
  return g_set_ps_const(d, start, data, count);
}

float g_drawing_share = -1.0f;  // the masked draw under way (for hdr.cpp's hdr_light_log), -1: none
bool g_drawing_old = false;

// The draw with the screen-door: the pixels whose threshold is at most `share` (old: exactly the others).
HRESULT DrawMasked(IDirect3DDevice9* d, const Variant& variant, float share, bool old, D3DPRIMITIVETYPE type, INT base,
                   UINT min_index, UINT vertices, UINT start, UINT prims) {
  const float fade[4] = {old ? -share - 1.0f / 128.0f : share, old ? 1.0f : -1.0f, 0.0f, 0.0f};
  if (!g_ps_const_known[variant.reg]) {  // set before the hook went in, if ever
    if (FAILED(d->GetPixelShaderConstantF(static_cast<UINT>(variant.reg), g_ps_const[variant.reg], 1)))
      memset(g_ps_const[variant.reg], 0, 16);
    g_ps_const_known[variant.reg] = true;
  }
  g_set_ps(d, variant.ps);
  g_set_ps_const(d, static_cast<UINT>(variant.reg), fade, 1);
  g_drawing_share = share;
  g_drawing_old = old;
  const HRESULT hr = g_draw_indexed(d, type, base, min_index, vertices, start, prims);
  g_drawing_share = -1.0f;
  g_set_ps_const(d, static_cast<UINT>(variant.reg), g_ps_const[variant.reg], 1);
  g_set_ps(d, g_ps);
  return hr;
}

HRESULT __attribute__((stdcall, force_align_arg_pointer)) DrawIndexedHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE type, INT base, UINT min_index,
                                                                         UINT vertices, UINT start, UINT prims) {
  const uint32_t mark = g_mark;
  if (!mark) {
    if (g_pv.on && g_ps && Mine(d)) {
      volatile uint32_t here = 0;  // the stack above it holds the game's frames
      const float share = PropShare(reinterpret_cast<uintptr_t>(&here));
      if (share >= 0.0f) {
        const Variant variant = VariantOf(d, g_ps);
        if (variant.ps) {
          ++g_pn.masked;
          return DrawMasked(d, variant, share, false, type, base, min_index, vertices, start, prims);
        }
      }
    }
    if (g_zs.on && g_ps && Mine(d)) {
      volatile uint32_t here = 0;
      bool old = false;
      const float share = ZoneShare(reinterpret_cast<uintptr_t>(&here), &old);
      if (share >= 0.0f) {
        const Variant variant = VariantOf(d, g_ps);
        if (variant.ps) return DrawMasked(d, variant, share, old, type, base, min_index, vertices, start, prims);
      }
    }
    return g_draw_indexed(d, type, base, min_index, vertices, start, prims);
  }
  if (!Mine(d)) return g_draw_indexed(d, type, base, min_index, vertices, start, prims);
  const uint32_t slot = (mark >> kMarkSlotShift) & 0x3F;
  const bool pp = slot >= kSlots && slot < kSlots + kPpSlots;
  const bool active = slot < kSlots ? g_fades[slot].active : pp && g_pp[slot - kSlots].active;
  const float share = slot < kSlots ? g_fades[slot].share : pp ? g_pp[slot - kSlots].share : 1.0f;
  const bool old = (mark & kMarkOld) != 0, shadow = (mark & kMarkShadow) != 0;
  ++g_n.marked_draws;
  if (shadow) {
    ++g_sh.marked;
    g_sh.no_ps += !g_ps;
  }
  const Variant variant = !active ? Variant() : g_ps ? VariantOf(d, g_ps) : BareVariant(d);
  if (!variant.ps) {
    ++g_n.unmasked_draws;
    g_sh.unmasked += shadow;
    // No mask to be had: the new mesh whole, the old one not at all.
    return old ? D3D_OK : g_draw_indexed(d, type, base, min_index, vertices, start, prims);
  }
  return DrawMasked(d, variant, share, old, type, base, min_index, vertices, start, prims);
}

bool Readable(uintptr_t p, size_t n) {
  MEMORY_BASIC_INFORMATION mbi;
  if (p < 0x10000 || !SS_VQ(reinterpret_cast<void*>(p), &mbi, sizeof mbi)) return false;
  if (mbi.State != MEM_COMMIT || mbi.Protect == 0 || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
  return p + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

template <size_t N>
bool Matches(const Sig (&sigs)[N]) {
  for (const Sig& s : sigs)
    if (!Readable(s.va, s.n) || memcmp(reinterpret_cast<const void*>(s.va), s.b, s.n) != 0) return false;
  return true;
}

bool KnownBuild() { return Matches(kSigs); }

// The device's slots, once it exists (and after patch.cpp / quality.cpp took theirs of the same vtable).
bool HookDevice() {
  const uintptr_t r = *reinterpret_cast<const volatile uint32_t*>(kRenderer);
  if (!r || !Readable(r + kRendererDevice, 4)) return false;
  const uintptr_t dev = *reinterpret_cast<const volatile uint32_t*>(r + kRendererDevice);
  if (!dev || !Readable(dev, 4)) return false;
  uint32_t* vt = *reinterpret_cast<uint32_t**>(dev);
  if (!vt || !Readable(reinterpret_cast<uintptr_t>(vt), (kSetPsConstSlot + 1) * 4)) return false;
  HMODULE owner = nullptr, self = nullptr;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<const char*>(&HookDevice), &self);
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<const char*>(vt[kDrawIndexedSlot]), &owner) ||
      !owner || owner == GetModuleHandleA(nullptr)) {
    ss_log("[lod] the device's DrawIndexedPrimitive is not in a d3d9 module; LOD smoothing stays off");
    return true;
  }
  // Another part of this DLL may hold the slots already (hdr.cpp's draw and SetPixelShader hooks, shadowsmooth.cpp's
  // SetPixelShader): this chains in front of them. Only its own hooks already in place (the same vtable again) stop it.
  if (owner == self && (vt[kDrawIndexedSlot] == reinterpret_cast<uint32_t>(&DrawIndexedHook) ||
                        vt[kSetPixelShaderSlot] == reinterpret_cast<uint32_t>(&SetPixelShaderHook)))
    return true;
  g_set_rt = reinterpret_cast<SetRenderTargetFn>(vt[kSetRenderTargetSlot]);
  g_set_stream = reinterpret_cast<SetStreamFn>(vt[kSetStreamSlot]);
  g_set_ps = reinterpret_cast<SetPixelShaderFn>(vt[kSetPixelShaderSlot]);
  g_set_ps_const = reinterpret_cast<SetPsConstFn>(vt[kSetPsConstSlot]);
  g_draw_indexed = reinterpret_cast<DrawIndexedFn>(vt[kDrawIndexedSlot]);
  g_dev = reinterpret_cast<IDirect3DDevice9*>(dev);
  // The draw last: it is the one that acts on what the others note.
  // The slot's word is swapped under ss_write_word's lock and what was there comes back: another part's worker may
  // have put its own hook in since the read above (deband.cpp, shadowsmooth.cpp and lodfade.cpp hook these slots from
  // their own threads at about the same time); the hook calls what the swap gave back, so neither drops the other.
  uintptr_t p_ps = 0, p_stream = 0, p_const = 0, p_draw = 0, p_rt = 0;
  const bool ok = ss_write_word(&vt[kSetPixelShaderSlot], reinterpret_cast<uintptr_t>(&SetPixelShaderHook), &p_ps) &&
                  ss_write_word(&vt[kSetStreamSlot], reinterpret_cast<uintptr_t>(&SetStreamHook), &p_stream) &&
                  ss_write_word(&vt[kSetPsConstSlot], reinterpret_cast<uintptr_t>(&SetPsConstHook), &p_const) &&
                  ss_write_word(&vt[kDrawIndexedSlot], reinterpret_cast<uintptr_t>(&DrawIndexedHook), &p_draw) &&
                  ss_write_word(&vt[kSetRenderTargetSlot], reinterpret_cast<uintptr_t>(&SetRenderTargetHook), &p_rt);
  if (p_ps) g_set_ps = reinterpret_cast<SetPixelShaderFn>(p_ps);
  if (p_stream) g_set_stream = reinterpret_cast<SetStreamFn>(p_stream);
  if (p_const) g_set_ps_const = reinterpret_cast<SetPsConstFn>(p_const);
  if (p_draw) g_draw_indexed = reinterpret_cast<DrawIndexedFn>(p_draw);
  if (p_rt) g_set_rt = reinterpret_cast<SetRenderTargetFn>(p_rt);
  ss_log("[lod] device %p, vtable %p: SetRenderTarget, SetStreamSource, SetPixelShader(ConstantF), DrawIndexedPrimitive %s; fades of "
         "%d frames at 30 fps%s",
         reinterpret_cast<void*>(dev), vt, ok ? "hooked" : "NOT all hooked (error)", static_cast<int>(g_frames),
         g_test_share >= 0.0f ? " (SS_LOD_TEST: held fades)" : "");
  return true;
}

DWORD WINAPI Worker(LPVOID) {
  // The image unpacked (the code reads as the known build), then the device, after the other parts' slots.
  const DWORD t0 = GetTickCount();
  while (!KnownBuild()) {
    if (GetTickCount() - t0 > 180000) {
      ss_log("[lod] the scenery's draw code is not the known build's: LOD smoothing stays off");
      return 0;
    }
    Sleep(200);
  }
  g_pp_known = Matches(kPpSigs);
  if (!g_pp_known) ss_log("[lod] the power-play objects' code is not the known build's: they stay unfaded");
  g_prop_known = Matches(kPropSigs);
  if (!g_prop_known) ss_log("[lod] the knock-over props' draw code is not the known build's: they stay unfaded");
  g_shadow_known = Matches(kShadowSigs);
  if (!g_shadow_known) ss_log("[lod] the cascades' caster code is not the known build's: their own switches stay hard");
  g_zone_known = Matches(kZoneSigs);
  if (!g_zone_known) ss_log("[lod] the seclusion zones' code is not the known build's: their switches stay hard");
  Sleep(4000);
  while (!HookDevice()) Sleep(200);
  return 0;
}

int ParseFrames(const std::string& v) {
  if (v.empty() || v == "off" || v == "false") return 0;
  if (v == "on" || v == "true") return 15;
  if (v == "high") return 30;
  const int n = atoi(v.c_str());
  return n < 0 ? 0 : n > 120 ? 120 : n;
}

}  // namespace

void ss_lodfade_start() {
  const char* e = getenv("SS_LOD_SMOOTHING");
  const std::string v = e ? std::string(e) : ss_conf("lod_smoothing");
  // No key: on (the release default of 2026-11-13), as long as there is a splitsecond.conf (without one the DLL does nothing).
  g_frames = v.empty() ? (ss_conf_present() ? 15 : 0) : ParseFrames(v);
  const char* t = getenv("SS_LOD_TEST");
  if (t && *t) {
    const double share = atof(t);
    g_test_share = static_cast<float>(share < 0.02 ? 0.02 : share > 0.98 ? 0.98 : share);
  }
  const char* l = getenv("SS_LOD_LOG");
  g_log = l && atoi(l) > 0;
  if (g_frames <= 0 && g_test_share < 0.0f && !g_log) return;  // off: nothing is hooked
  ss_log("[lod] lod_smoothing %s (%d frames at 30 fps)", g_frames == 0 ? "off" : g_frames == 15 ? "on" : g_frames == 30 ? "high" : "set",
         static_cast<int>(g_frames));
  CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
}

// patch.cpp's present hook, before the frame is shown: the objects get their own meshes back until the next frame's
// render, so the game's update and streaming never meet a combined mesh.
void ss_lodfade_present() {
  if (!g_dev) return;
  for (Fade& f : g_fades)
    if (f.active) PutBack(f);
  for (PpFade& f : g_pp) PpPutBack(f);
  ZonePutBack();
  ForgetPages();
  ++g_frame;
  ++g_presents;
  g_scanned = false;
  g_mark = 0;
  if (g_log) {
    ++g_n.frames;
    Report();
  }
  if ((g_frame & 0x3FF) == 0) {  // objects not seen for a while
    for (auto it = g_seen.begin(); it != g_seen.end();)
      it = g_frame - std::max(it->second.frame, it->second.shadow_frame) > 600 && it->second.slot < 0 ? g_seen.erase(it)
                                                                                                       : std::next(it);
    for (auto it = g_sectors.begin(); it != g_sectors.end();)
      it = g_frame - std::max(it->second.frame, it->second.shadow_frame) > 600 ? g_sectors.erase(it) : std::next(it);
  }
}

// The settings menu: at once while the hooks are in (the key was on at start, or a check switch); else RESTART NEEDED.
bool ss_lodfade_apply(const char* key, const char* value) {
  if (strcmp(key, "lod_smoothing") || !g_dev) return false;
  g_frames = ParseFrames(value ? value : "");
  ss_log("[lod] from the settings menu: %d frames at 30 fps", static_cast<int>(g_frames));
  return true;
}

// hdr.cpp classes the game's shaders (the self-lit ones for the lamps' mask); a fading draw binds a dither variant in
// their place, and it asks for the shader behind it. Called on the game's render thread, as the hook that fills the map.
// hdr.cpp (hdr_light_log), from inside a draw: the share of the masked draw under way and whether it is the old mesh's;
// -1 when the draw is not a masked one.
float ss_lodfade_drawing(int* old) {
  *old = g_drawing_old ? 1 : 0;
  return g_drawing_share;
}

void* ss_lodfade_variant(void* device, void* ps) {
  auto* d = static_cast<IDirect3DDevice9*>(device);
  if (!ps || g_frames == 0 || d != g_dev) return nullptr;
  return VariantOf(d, static_cast<IDirect3DPixelShader9*>(ps)).ps;
}

void* ss_lodfade_original(void* ps) {
  auto it = g_original.find(static_cast<IDirect3DPixelShader9*>(ps));
  return it == g_original.end() ? ps : it->second;
}
