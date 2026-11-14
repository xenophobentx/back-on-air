// Anti-aliasing (FXAA or SMAA 1x) and sharpening (contrast adaptive) of the finished 3D picture, under the menus and HUD.
//
// Keys (the port's own rows, same names, values and defaults; taa is this patch's own): `aa = off | fxaa | smaa | taa`
// (default smaa when the key is missing, the release default of 2026-11-13; switch SS_AA) and
// `sharpen = 0 | 0.25 | 0.5 | 0.75 | 1` (default 0 = off; switch SS_SHARPEN). With aa = off and sharpen = 0 (and HDR's
// hooks not in: hdr_ready = false, hdr = false) this file does nothing at all: no thread, no hook, no device call.
//
// With HDR's hooks in (hdr or hdr_ready, the default) the same frame hooks carry hdr.cpp's passes (one owner per slot): its scene pass runs at the
// scene/UI split right after the passes here (so anti-aliasing and sharpening work on the SDR-graded picture, as
// without HDR, and HDR's luma reference is the anti-aliased picture), its mask pass at the game's EndScene. While
// HDR's passes run, hdr.cpp finds the split itself (ss_postfx_split: the first UI draw after the colour grade, which
// is before the car's HUD cards; the BeginDisplay split comes after them), so the passes here then also leave those
// cards alone.
//
// Where the picture is taken (reverse engineering of the PC image, run-time VAs of the Steam build 385319):
//   * The frame ends in 0x50E400: EndScene (the only EndScene call in the game), then, when the final colour target
//     is not the back buffer, StretchRect(final, back buffer), then Present. The final target is read there as
//     [[[0xD64708]+4]+0x64]+0x5C (renderer -> frame buffer set -> target object -> IDirect3DSurface9*).
//   * In a race frame the last post pass (the colour grade) writes that target with one full-screen draw, and the HUD
//     is then drawn straight onto the same target by Scaleform (the game's AppGfx::cRenderer, vtable 0xBC93DC in
//     read-only data, RTTI ".?AVcRenderer@AppGfx@@"). Every movie's Display starts with the renderer's
//     BeginDisplay(GColor, const GViewport&, x0, x1, y0, y1), slot 5 (0xBC93F0, holds 0x535080, thiscall, ret 0x18);
//     the vtable is referenced only by the renderer's two constructors and nothing calls 0x535080 directly.
// So the slot is pointed at a wrapper: at the first BeginDisplay of a frame whose render target 0 is the final target,
// the 3D picture is complete and no UI is on it yet; the passes run there, then the original BeginDisplay. A frame
// with no Scaleform display at all (videos, a screen with no movie) is filtered whole at EndScene instead (there is no
// UI on it to spare). A frame whose movies all went to other targets is left alone.
//
// Nothing of the game's code is written: one aligned 4-byte slot in the game's read-only data (as pad_glyphs does),
// and two slots of the D3D9 device's vtable (EndScene 42, Reset 16), which lives in d3d9.dll, not in the game.
// Every device state the passes touch is put back: an IDirect3DStateBlock9 (D3DSBT_ALL) is captured before and applied
// after, and the render targets and the depth surface (which a state block does not hold) are saved and set back.
//
// Shaders: HLSL compiled at run time with D3DXCompileShader from d3dx9_42.dll, which the game imports and uses itself.
//   Compiled on shadercache.cpp's worker thread (or read from its disk cache), never on the render thread: until a
//   mode's shaders are there the frame is left as the game drew it, and a mode the menu switches to takes over in one
//   frame once its shaders are done, the old mode staying on screen meanwhile.
//   * FXAA: the "quality" algorithm of FXAA 3.11 (Timothy Lottes, NVIDIA), medium preset, written from the published
//     description (the port's own fxaa.frag, translated to Shader Model 3); no NVIDIA code.
//   * Sharpening: contrast adaptive sharpening after AMD FidelityFX CAS (MIT, Copyright (c) 2019 Advanced Micro
//     Devices, Inc.), the port's cas.frag translated; no AMD code.
//   * SMAA 1x: the reference implementation, unmodified, third_party/smaa/SMAA.hlsl with its AreaTex/SearchTex tables
//     (MIT; Copyright (C) 2013 Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro, Diego Gutierrez).
//     Luma edge detection, blending weights, neighbourhood blending; preset HIGH. With `smaa_predication = true` (the
//     default; switch SS_SMAA_PREDICATION; live from the menu) the edge pass uses the reference's depth predication
//     with the game's G-buffer depth (an INTZ texture, found through a SetDepthStencilSurface wrap, slot 39); colour
//     only when no such depth of the picture's size was bound in the frame. hdr_gpu = 1 also times these passes.
//   * TAA (aa = taa): camera jitter and a temporal resolve with depth reprojection and neighbourhood
//     clipping (see "TAA" below); written from the published descriptions of the technique, no third-party code.
// Notices: THIRD_PARTY_NOTICES.md.
#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include "menu.h"
#include "ss_shared.h"
#include "shadercache.h"
#include "../third_party/smaa/AreaTex.h"
#include "../third_party/smaa/SearchTex.h"

// The SMAA reference shader, embedded as it is distributed.
asm(".section .rdata,\"dr\"\n"
    "ss_smaa_hlsl_begin:\n"
    ".incbin \"third_party/smaa/SMAA.hlsl\"\n"
    "ss_smaa_hlsl_end:\n"
    ".byte 0\n"
    ".text\n");
extern "C" const char g_smaa_hlsl_begin[] asm("ss_smaa_hlsl_begin");
extern "C" const char g_smaa_hlsl_end[] asm("ss_smaa_hlsl_end");

namespace {

// ------------------------------------------------------------------------------------------------- build constants

constexpr uintptr_t kRenderer = 0xD64708;      // renderer holder; +0x578 IDirect3DDevice9*
constexpr uint32_t kRendererDevice = 0x578;
constexpr uint32_t kRendererFrameSet = 0x4;    // frame buffer set; +0x64 its colour target object; +0x5C the surface
constexpr uint32_t kFrameSetColour = 0x64, kTargetSurface = 0x5C;
constexpr uintptr_t kGfxRendererVt = 0xBC93DC;  // AppGfx::cRenderer vtable (read-only data)
constexpr uintptr_t kBeginDisplaySlot = kGfxRendererVt + 5 * 4;  // 0xBC93F0
constexpr uint32_t kBeginDisplay = 0x535080;
constexpr uintptr_t kSubmitMaskSlot = kGfxRendererVt + 26 * 4;   // BeginSubmitMask, checked as a second witness
constexpr uint32_t kSubmitMask = 0x534630;
constexpr int kResetSlot = 16, kSetDepthSlot = 39, kEndSceneSlot = 42;
// TAA: the viewport manager's cameras (aspect.cpp has the layout): V at +0xD8, P at +0x158, VP = V * P at +0x198
// (row-major, row vectors, left-handed: P[2][3] = 1, so P[2][0] and P[2][1] move the picture in NDC; 0x50AAF0 leaves
// them 0), 0x50C250 publishes the three to their shader parameters.
constexpr uintptr_t kViewMgr = 0xD66000;
constexpr uint32_t kViewCams = 0x68, kCamSize = 0x2F4, kCamView = 0xD8, kCamProj = 0x158, kCamViewProj = 0x198;
constexpr uint32_t kCamOrtho = 0x1E0, kCamCustom = 0x1E1;
constexpr uintptr_t kCamPublish = 0x50C250;

// --------------------------------------------------------------------------------------------------------- config

enum Aa { AA_OFF = 0, AA_FXAA = 1, AA_SMAA = 2, AA_TAA = 3, AA_TAA_HIGH = 4 };
inline bool IsTaa(int aa) { return aa == AA_TAA || aa == AA_TAA_HIGH; }
bool g_hdr = false;  // HDR's hooks are in (hdr or hdr_ready at start): the frame hooks also carry hdr.cpp's passes
int g_aa = AA_OFF;  // what the passes run with (render thread)
float g_sharpen = 0.0f;
// The settings menu's change (ss_postfx_apply), taken over by the render thread at the next filtered frame.
volatile LONG g_want_aa = AA_OFF;
volatile float g_want_sharpen = 0.0f;
volatile LONG g_want_changed = 0;
// smaa_predication (default true; SS_SMAA_PREDICATION): SMAA's edge pass also reads the game's depth. Live from the
// menu (only a choice of pixel shader and one more texture; nothing is made again).
volatile LONG g_pred = 1;
bool g_gpu = false;  // hdr_gpu / SS_HDR_GPU: the passes here are timed too (timestamp queries, one frame in 16)

int ParseAa(const std::string& aa) { return aa == "fxaa" ? AA_FXAA : aa == "smaa" ? AA_SMAA : aa == "taa" ? AA_TAA : aa == "taa_high" ? AA_TAA_HIGH : AA_OFF; }
const char* AaName(int aa) { return aa == AA_FXAA ? "fxaa" : aa == AA_SMAA ? "smaa" : aa == AA_TAA ? "taa" : aa == AA_TAA_HIGH ? "taa_high" : "off"; }
float ParseSharpen(const std::string& sh) {
  const float v = strtof(sh.c_str(), nullptr);
  return !(v > 0.0f) ? 0.0f : v > 1.0f ? 1.0f : v;
}

inline uint32_t Rd(uintptr_t va) { return *reinterpret_cast<const volatile uint32_t*>(va); }

bool Readable(uintptr_t p, size_t n) {
  if (p < 0x10000 || p + n < p) return false;
  if (ss_region_known(p, n)) return true;  // checked in this phase of the frame (ss_shared.h)
  if (ss_region_bad(p)) return false;
  MEMORY_BASIC_INFORMATION mbi;
  if (!SS_VQ(reinterpret_cast<void*>(p), &mbi, sizeof mbi)) return false;
  if (mbi.State != MEM_COMMIT || mbi.Protect == 0 || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
    ss_region_note_bad(reinterpret_cast<uintptr_t>(mbi.BaseAddress), mbi.RegionSize);
    return false;
  }
  ss_region_note(reinterpret_cast<uintptr_t>(mbi.BaseAddress), mbi.RegionSize, mbi.Type);
  return reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize >= p + n;
}

IDirect3DDevice9* GameDevice() {
  const uintptr_t r = Rd(kRenderer);
  if (!r || !Readable(r + kRendererDevice, 4)) return nullptr;
  return reinterpret_cast<IDirect3DDevice9*>(Rd(r + kRendererDevice));
}

// The surface 0x50E400 copies to the back buffer (or presents itself): the frame's final colour target.
IDirect3DSurface9* FinalSurface() {
  const uintptr_t r = Rd(kRenderer);
  if (!r || !Readable(r + kRendererFrameSet, 4)) return nullptr;
  const uintptr_t set = Rd(r + kRendererFrameSet);
  if (!set || !Readable(set + kFrameSetColour, 4)) return nullptr;
  const uintptr_t target = Rd(set + kFrameSetColour);
  if (!target || !Readable(target + kTargetSurface, 4)) return nullptr;
  return reinterpret_cast<IDirect3DSurface9*>(Rd(target + kTargetSurface));
}

// --------------------------------------------------------------------------------------------------------- shaders

// The full-screen quad: position (already offset by half a pixel) and texture coordinate.
const char kQuadVs[] = R"(
void main(float4 p : POSITION, float2 t : TEXCOORD0, out float4 op : POSITION, out float2 ot : TEXCOORD0) {
  op = p;
  ot = t;
}
)";

// FXAA, quality algorithm, medium preset (12). c0 = (1/width, 1/height, -, -). Sampler 0 = the scene, linear, clamped.
const char kFxaaPs[] = R"(
sampler2D scene : register(s0);
float4 texel : register(c0);
float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
float4 At(float2 uv, float2 o) { return tex2Dlod(scene, float4(uv + o * texel.xy, 0, 0)); }
float LumaAt(float2 uv) { return Luma(tex2Dlod(scene, float4(uv, 0, 0)).rgb); }
float4 main(float2 uv : TEXCOORD0) : COLOR {
  float4 centre = At(uv, float2(0, 0));
  float lumaM = Luma(centre.rgb);
  float lumaN = Luma(At(uv, float2(0, -1)).rgb);
  float lumaS = Luma(At(uv, float2(0, 1)).rgb);
  float lumaW = Luma(At(uv, float2(-1, 0)).rgb);
  float lumaE = Luma(At(uv, float2(1, 0)).rgb);
  float rangeMax = max(max(lumaM, lumaN), max(max(lumaS, lumaW), lumaE));
  float rangeMin = min(min(lumaM, lumaN), min(min(lumaS, lumaW), lumaE));
  float range = rangeMax - rangeMin;
  [branch] if (range < max(0.0625, rangeMax * 0.125)) return centre;

  float lumaNW = Luma(At(uv, float2(-1, -1)).rgb);
  float lumaNE = Luma(At(uv, float2(1, -1)).rgb);
  float lumaSW = Luma(At(uv, float2(-1, 1)).rgb);
  float lumaSE = Luma(At(uv, float2(1, 1)).rgb);
  float edgeHorz = abs(-2.0 * lumaW + (lumaNW + lumaSW)) + abs(-2.0 * lumaM + (lumaN + lumaS)) * 2.0 +
                   abs(-2.0 * lumaE + (lumaNE + lumaSE));
  float edgeVert = abs(-2.0 * lumaS + (lumaSW + lumaSE)) + abs(-2.0 * lumaM + (lumaW + lumaE)) * 2.0 +
                   abs(-2.0 * lumaN + (lumaNW + lumaNE));
  bool horzSpan = edgeHorz >= edgeVert;

  float luma1 = horzSpan ? lumaN : lumaW;
  float luma2 = horzSpan ? lumaS : lumaE;
  float gradient1 = luma1 - lumaM;
  float gradient2 = luma2 - lumaM;
  bool steeper1 = abs(gradient1) >= abs(gradient2);
  float gradient = max(abs(gradient1), abs(gradient2));
  float lengthSign = horzSpan ? texel.y : texel.x;
  if (steeper1) lengthSign = -lengthSign;
  float lumaLocalAverage = 0.5 * ((steeper1 ? luma1 : luma2) + lumaM);

  float lumaAverage = ((lumaN + lumaS + lumaW + lumaE) * 2.0 + (lumaNW + lumaNE + lumaSW + lumaSE)) * (1.0 / 12.0);
  float subpixC = saturate(abs(lumaAverage - lumaM) / range);
  float subpixF = (-2.0 * subpixC + 3.0) * subpixC * subpixC;

  float2 pos = uv;
  float2 along = horzSpan ? float2(texel.x, 0.0) : float2(0.0, texel.y);
  if (horzSpan) pos.y += lengthSign * 0.5; else pos.x += lengthSign * 0.5;
  float gradientScaled = gradient * 0.25;
  float2 posN = pos - along;
  float2 posP = pos + along;
  float endN = LumaAt(posN) - lumaLocalAverage;
  float endP = LumaAt(posP) - lumaLocalAverage;
  bool doneN = abs(endN) >= gradientScaled;
  bool doneP = abs(endP) >= gradientScaled;
  const float steps[7] = { 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0 };
  [unroll] for (int i = 0; i < 7; ++i) {
    if (!doneN) { posN -= along * steps[i]; endN = LumaAt(posN) - lumaLocalAverage; doneN = abs(endN) >= gradientScaled; }
    if (!doneP) { posP += along * steps[i]; endP = LumaAt(posP) - lumaLocalAverage; doneP = abs(endP) >= gradientScaled; }
  }

  float distN = horzSpan ? uv.x - posN.x : uv.y - posN.y;
  float distP = horzSpan ? posP.x - uv.x : posP.y - uv.y;
  bool nearerN = distN < distP;
  float endNearer = nearerN ? endN : endP;
  float span = distN + distP;
  float distNearer = nearerN ? distN : distP;
  bool goodSpan = (endNearer < 0.0) != (lumaM - lumaLocalAverage < 0.0);
  float pixelOffset = goodSpan ? 0.5 - distNearer / span : 0.0;
  pixelOffset = max(pixelOffset, subpixF * subpixF * 0.75);
  float2 o = uv;
  if (horzSpan) o.y += pixelOffset * lengthSign; else o.x += pixelOffset * lengthSign;
  return float4(tex2Dlod(scene, float4(o, 0, 0)).rgb, centre.a);
}
)";

// Contrast adaptive sharpening. c0 = (1/width, 1/height, strength 0..1, -), c1.x = the dither step (deband: one step of
// the target's format, else 0). Sampler 0 = the picture, clamped.
const char kCasPs[] = R"(
sampler2D scene : register(s0);
float4 params : register(c0);
float4 dith : register(c1);
float3 At(float2 uv, float2 o) { return tex2Dlod(scene, float4(uv + o * params.xy, 0, 0)).rgb; }
float Ign(float2 p) { return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715)))); }
float4 main(float2 uv : TEXCOORD0, float2 vp : VPOS) : COLOR {
  float3 a = At(uv, float2(-1, -1));
  float3 b = At(uv, float2(0, -1));
  float3 c = At(uv, float2(1, -1));
  float3 d = At(uv, float2(-1, 0));
  float4 centre = tex2Dlod(scene, float4(uv, 0, 0));
  float3 e = centre.rgb;
  float3 f = At(uv, float2(1, 0));
  float3 g = At(uv, float2(-1, 1));
  float3 h = At(uv, float2(0, 1));
  float3 i = At(uv, float2(1, 1));
  float3 mn = min(min(min(d, e), min(f, b)), h);
  float3 mn2 = min(min(min(mn, a), c), min(g, i));
  mn += mn2;
  float3 mx = max(max(max(d, e), max(f, b)), h);
  float3 mx2 = max(max(max(mx, a), c), max(g, i));
  mx += mx2;
  float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-4)));
  float peak = -1.0 / lerp(8.0, 5.0, saturate(params.z));
  float3 w = amp * peak;
  float3 sharpened = (b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w);
  return float4(saturate(sharpened) + (Ign(vp) - 0.5) * dith.x, centre.a);
}
)";

// SMAA 1x around the reference shader. c0 (vertex and pixel) = (1/width, 1/height, width, height).
const char kSmaaPrefix[] = R"(
#define SMAA_HLSL_3 1
#define SMAA_PRESET_HIGH 1
#define SMAA_RT_METRICS smaaMetrics
float4 smaaMetrics : register(c0);
)";
// Depth predication (SMAA_PREDICATION, the reference's own switch): the edge pass also reads the game's G-buffer depth
// (an INTZ texture, s5) and lowers the colour threshold where the depth jumps, raises it where it does not. The depth
// is not linear (z/w of a perspective projection, about 1 - near/z), so it is read as -log2(1 - d) = log2(z / near)
// + a constant: the difference of two neighbours is then the log of their distances' ratio, whatever near and far
// are. A jump of 0.1 is about 7 % of the distance (a car before the road, a post before the field, the sky); the
// ground plane only crosses that within a few pixels of the horizon. Scale 1.5 / strength 0.5: the threshold is 0.15
// on flat surfaces (texture detail and painted text keep their edges unblurred) and 0.075 on depth edges.
// SMAAGather is the reference's porting macro (left undefined for Shader Model 3): defined here, it hands
// SMAAGatherNeighbours the three log depths in a gather's order (.g the pixel, .r the left one, .b the top one).
const char kSmaaPredPrefix[] = R"(
#define SMAA_PREDICATION 1
#define SMAA_PREDICATION_THRESHOLD 0.1
#define SMAA_PREDICATION_SCALE 1.5
#define SMAA_PREDICATION_STRENGTH 0.5
float SsLogDepth(sampler2D s, float2 uv) { return -log2(max(1.0 - tex2Dlod(s, float4(uv, 0.0, 0.0)).r, 1e-7)); }
float4 SsDepthGather(sampler2D s, float2 c) {
  float2 h = smaaMetrics.xy * 0.5;
  return float4(SsLogDepth(s, c + float2(-h.x, h.y)), SsLogDepth(s, c + h), SsLogDepth(s, c + float2(h.x, -h.y)), 0.0);
}
#define SMAAGather(tex, coord) SsDepthGather(tex, coord)
)";
const char kSmaaPredSuffix[] = R"(
sampler2D colorTex : register(s0);
sampler2D depthTex : register(s5);
float4 EdgePredPS(float2 t : TEXCOORD0, float4 off[3] : TEXCOORD1) : COLOR {
  return float4(SMAALumaEdgeDetectionPS(t, off, colorTex, depthTex), 0.0, 0.0);
}
)";
const char kSmaaSuffix[] = R"(
sampler2D colorTex : register(s0);
sampler2D edgesTex : register(s1);
sampler2D areaTex : register(s2);
sampler2D searchTex : register(s3);
sampler2D blendTex : register(s4);
void EdgeVS(float4 p : POSITION, float2 t : TEXCOORD0, out float4 op : POSITION, out float2 ot : TEXCOORD0,
            out float4 off[3] : TEXCOORD1) {
  op = p; ot = t; SMAAEdgeDetectionVS(t, off);
}
float4 EdgePS(float2 t : TEXCOORD0, float4 off[3] : TEXCOORD1) : COLOR {
  return float4(SMAALumaEdgeDetectionPS(t, off, colorTex), 0.0, 0.0);
}
void WeightVS(float4 p : POSITION, float2 t : TEXCOORD0, out float4 op : POSITION, out float2 ot : TEXCOORD0,
              out float2 pix : TEXCOORD1, out float4 off[3] : TEXCOORD2) {
  op = p; ot = t; SMAABlendingWeightCalculationVS(t, pix, off);
}
float4 WeightPS(float2 t : TEXCOORD0, float2 pix : TEXCOORD1, float4 off[3] : TEXCOORD2) : COLOR {
  return SMAABlendingWeightCalculationPS(t, pix, off, edgesTex, areaTex, searchTex, float4(0.0, 0.0, 0.0, 0.0));
}
void BlendVS(float4 p : POSITION, float2 t : TEXCOORD0, out float4 op : POSITION, out float2 ot : TEXCOORD0,
             out float4 off : TEXCOORD1) {
  op = p; ot = t; SMAANeighborhoodBlendingVS(t, off);
}
float4 BlendPS(float2 t : TEXCOORD0, float4 off : TEXCOORD1) : COLOR {
  return SMAANeighborhoodBlendingPS(t, off, colorTex, blendTex);
}
)";

// TAA (aa = taa): the temporal resolve. Written from the published descriptions of temporal
// anti-aliasing (reprojection of the last result through the depth and the camera matrices, YCoCg neighbourhood
// variance clipping, Catmull-Rom history fetch); no third-party code.
//   s0 the current (jittered) picture, point; s1 the last result, linear; s5 the game's depth (INTZ), point;
//   s6 the last frame's depth as -log2(1 - d) (R32F), point.
//   c0 = (1/width, 1/height, width, height); c1..c4 = the columns of M = inverse(VP now) * VP last (both without
//   jitter; a clip-space row vector times M is the same point in the last frame's clip space); c5 = (this frame's
//   jitter in NDC x, y; the weight of the current picture; 1 when there is a last result, else 0); c6 = (the last
//   frame's jitter in NDC x, y).
// The history is the picture without jitter: a pixel of it is the average of the jittered frames' samples around that
// pixel's centre. Two candidates for where the pixel was: the camera's reprojection (the world stood still) and the
// same pixel (an object that moves with the camera: the player's car under the chase camera, a car driving along),
// taken only where the reprojection's last depth does not match the expected one; neither matching (a car going
// past, a disocclusion, a place that was outside the picture) leaves the current picture.
// What is taken is clipped to the current 3x3 neighbourhood's colour range (mean +- 1..1.5 sigma in YCoCg), so a
// wrong history cannot stay on screen.
// TAA_HIGH 1 (aa = taa_high): s0 is the picture after SMAA 1x (the places the history is rejected at are still
// anti-aliased), and the nearest depth is looked for in all 9 neighbours instead of 5.
// TAA_MRT 1 (the device has two render targets of independent bit depths): the pass also writes this frame's depth
// for the next frame's match (as kTaaDepthPs) into a second target, so the depth copy is no pass of its own.
const char kTaaPs[] = R"(
sampler2D cur : register(s0);
sampler2D hist : register(s1);
sampler2D depth : register(s5);
sampler2D lastDepth : register(s6);
float4 rt : register(c0);
float4 m0 : register(c1);
float4 m1 : register(c2);
float4 m2 : register(c3);
float4 m3 : register(c4);
float4 p : register(c5);
float4 pj : register(c6);
float LogD(float d) { return -log2(max(1.0 - d, 1e-7)); }
float3 YCoCg(float3 c) { return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25))); }
float3 Rgb(float3 y) { return float3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z); }
float3 H(float2 uv) { return tex2Dlod(hist, float4(uv, 0.0, 0.0)).rgb; }
struct Out {
  float4 c : COLOR0;
#if TAA_MRT
  float4 z : COLOR1;
#endif
};
Out Done(float3 rgb, float a, float ld) {
  Out o;
  o.c = float4(rgb, a);
#if TAA_MRT
  o.z = float4(ld, 0.0, 0.0, 1.0);
#endif
  return o;
}
float3 HistoryCatmullRom(float2 uv) {
  float2 pos = uv * rt.zw;
  float2 c = floor(pos - 0.5) + 0.5;
  float2 f = pos - c;
  float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
  float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
  float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
  float2 w3 = f * f * (-0.5 + 0.5 * f);
  float2 w12 = w1 + w2;
  float2 t0 = (c - 1.0) * rt.xy, t3 = (c + 2.0) * rt.xy, t12 = (c + w2 / w12) * rt.xy;
  float a = w12.x * w0.y, b = w0.x * w12.y, m = w12.x * w12.y, e = w3.x * w12.y, g = w12.x * w3.y;
  float3 r = H(float2(t12.x, t0.y)) * a + H(float2(t0.x, t12.y)) * b + H(t12) * m + H(float2(t3.x, t12.y)) * e + H(float2(t12.x, t3.y)) * g;
  return max(r / (a + b + m + e + g), 0.0);
}
Out main(float2 uv : TEXCOORD0) {
  float4 centre = tex2Dlod(cur, float4(uv, 0.0, 0.0));
  float d = tex2Dlod(depth, float4(uv, 0.0, 0.0)).r;
  float ld = LogD(d);
  // The neighbourhood: mean, sigma, min and max in YCoCg; the depth range for the match tolerance and the nearest
  // depth (an edge pixel moves with the object in front of it).
  float3 m1s = 0.0, m2s = 0.0, mn = 1e5, mx = -1e5;
  float dmn = 1e5, dmx = -1e5, dnear = d;
  [unroll] for (int y = -1; y <= 1; ++y) {
    [unroll] for (int x = -1; x <= 1; ++x) {
      float2 o = uv + float2(x, y) * rt.xy;
      float3 c = YCoCg(tex2Dlod(cur, float4(o, 0.0, 0.0)).rgb);
      m1s += c; m2s += c * c; mn = min(mn, c); mx = max(mx, c);
#if !TAA_HIGH
      if (x == 0 || y == 0)
#endif
      {
        float dn = tex2Dlod(depth, float4(o, 0.0, 0.0)).r;
        float l = LogD(dn);
        dmn = min(dmn, l); dmx = max(dmx, l); dnear = min(dnear, dn);
      }
    }
  }
  float3 mean = m1s / 9.0;
  float3 sigma = sqrt(max(m2s / 9.0 - mean * mean, 0.0));
  if (p.w < 0.5) return Done(centre.rgb, centre.a, ld);

  // Where this point was: the pixel without this frame's jitter, through M; the history is the picture without
  // jitter, so the pixel that shows the point there is that place plus this frame's jitter again (a camera that
  // stands still reads the history at the same pixel, unfiltered), and in the last depth plus the last jitter.
  float4 cp = float4(uv.x * 2.0 - 1.0 - p.x, 1.0 - uv.y * 2.0 - p.y, dnear, 1.0);
  float4 q = float4(dot(cp, m0), dot(cp, m1), dot(cp, m2), dot(cp, m3));
  float qw = abs(q.w) < 1e-8 ? 1e-8 : q.w;
  float2 pn = q.xy / qw;
  float2 puv = float2((pn.x + p.x) * 0.5 + 0.5, 0.5 - (pn.y + p.y) * 0.5);
  float2 zuv = float2((pn.x + pj.x) * 0.5 + 0.5, 0.5 - (pn.y + pj.y) * 0.5);
  float2 suv = uv + float2(pj.x - p.x, p.y - pj.y) * 0.5;
  float tol = 0.02 + 0.5 * (dmx - dmn);
  float errR = abs(tex2Dlod(lastDepth, float4(zuv, 0.0, 0.0)).r - LogD(q.z / qw));
  bool off = q.w <= 0.0 || any(puv != saturate(puv));
  if (off) errR = 1e5;
  float errS = abs(tex2Dlod(lastDepth, float4(suv, 0.0, 0.0)).r - LogD(dnear));
  // The reprojection whenever it matches (on a straight road the same pixel's depth matches too, but its colour is
  // another part of the road); the same pixel only where the reprojection does not match and it does better.
  bool same = !off && errR > tol && errS < errR;
  float2 huv = same ? uv : puv;
  float err = same ? errS : errR;
  float trust = saturate(2.0 - err / tol);  // 1 up to the tolerance, 0 at twice it
  float moved = saturate(0.5 * length((puv - uv) * rt.zw));  // 0 standing, 1 from two pixels a frame

  // The colour box: wide where the picture stands (thin features do not flicker), tight where it moves or where
  // the history is the same pixel of an object whose own movement is not known (another car: no trail, less smear).
  float g = same ? 1.0 : lerp(1.5, 1.0, moved);
  float3 bmin = max(mn, mean - g * sigma), bmax = min(mx, mean + g * sigma);
  float3 h = YCoCg(HistoryCatmullRom(huv));
  // Clip the history towards the box's centre (not a per-channel clamp: keeps its hue).
  float3 bc = 0.5 * (bmax + bmin), be = 0.5 * (bmax - bmin) + 1e-4;
  float3 v = h - bc;
  float3 a = abs(v / be);
  float k = max(a.x, max(a.y, a.z));
  if (k > 1.0) h = bc + v / k;
  float w = lerp(1.0, same ? 1.5 * p.z : p.z, trust);
  // The same pixel's history far outside the box: the object moved on the screen (a car turning in front of the
  // camera); more of the current picture, so it does not smear.
  if (same) w = lerp(w, 1.0, 0.6 * saturate(0.25 * (k - 1.0)));
  float3 c0 = YCoCg(centre.rgb);
  // Luma weighting (bright sub-pixel sparks do not flicker the result).
  float wc = w / (1.0 + c0.x), wh = (1.0 - w) / (1.0 + h.x);
  float3 r = (c0 * wc + h * wh) / (wc + wh);
  return Done(saturate(Rgb(r)), centre.a, ld);
}
)";
// The depth for the next frame's match: -log2(1 - d) of the game's depth (s5). Only without TAA_MRT.
const char kTaaDepthPs[] = R"(
sampler2D depth : register(s5);
float4 main(float2 uv : TEXCOORD0) : COLOR {
  return float4(-log2(max(1.0 - tex2Dlod(depth, float4(uv, 0.0, 0.0)).r, 1e-7)), 0.0, 0.0, 1.0);
}
)";
// The history (A16B16G16R16F) into the final target's format. c0.x = the dither step (deband: one step of the target's
// format, the interleaved gradient noise of the game's tone mapper and grade, deband.cpp; else 0).
const char kCopyPs[] = R"(
sampler2D src : register(s0);
float4 dith : register(c0);
float4 main(float2 uv : TEXCOORD0, float2 vp : VPOS) : COLOR {
  float4 c = tex2Dlod(src, float4(uv, 0.0, 0.0));
  float n = frac(52.9829189 * frac(dot(vp, float2(0.06711056, 0.00583715))));
  return float4(c.rgb + (n - 0.5) * dith.x, c.a);
}
)";

constexpr DWORD kOptimise3 = 1u << 15;  // D3DXSHADER_OPTIMIZATION_LEVEL3

// --------------------------------------------------------------------------------------------------------- the device side

uint32_t g_frame_no = 0;  // frames ended (EndScene)

struct Res {
  IDirect3DDevice9* dev = nullptr;
  bool failed = false;  // shaders or the lookup tables could not be made: the passes stay off
  IDirect3DVertexDeclaration9* decl = nullptr;
  IDirect3DVertexShader9* quad_vs = nullptr;
  IDirect3DPixelShader9* fxaa_ps = nullptr;
  IDirect3DPixelShader9* cas_ps = nullptr;
  IDirect3DVertexShader9* smaa_vs[3] = {};
  IDirect3DPixelShader9* smaa_ps[3] = {};
  IDirect3DPixelShader9* smaa_pred_ps = nullptr;  // the edge pass with depth predication (nullptr: colour only)
  IDirect3DPixelShader9* taa_ps = nullptr;        // TAA's resolve and its depth copy
  IDirect3DPixelShader9* taa_depth_ps = nullptr;  // (nullptr with taa_mrt: the resolve writes the depth itself)
  IDirect3DPixelShader9* copy_ps = nullptr;       // the history out into the final target's format
  bool taa_mrt = false;                           // the resolve writes hist and zlast at once (two render targets)
  D3DFORMAT taa_hist_fmt = D3DFMT_UNKNOWN;        // A16B16G16R16F, or UNKNOWN: the final target's format
  IDirect3DTexture9* area = nullptr;    // managed pool: survives Reset
  IDirect3DTexture9* search = nullptr;
  // Default pool (released before Reset): the scene copy, the intermediate, SMAA's edges and weights, the state block.
  UINT w = 0, h = 0;
  D3DFORMAT fmt = D3DFMT_UNKNOWN;
  IDirect3DTexture9* copy = nullptr;
  IDirect3DTexture9* mid = nullptr;
  IDirect3DTexture9* edges = nullptr;
  IDirect3DTexture9* weights = nullptr;
  IDirect3DTexture9* hist[2] = {};   // TAA: the result of this frame and of the last (ping-pong), taa_hist_fmt
  IDirect3DTexture9* zlast[2] = {};  // TAA: the depth of this frame and of the last as -log2(1 - d), R32F
  IDirect3DStateBlock9* sb = nullptr;
} g_r;

// TAA's state between frames (render thread).
struct Taa {
  bool valid = false;        // hist[flip ^ 1] and zlast[flip ^ 1] hold the last frame's result and depth, prev_vp its camera
  uint32_t last_frame = 0;   // g_frame_no of the last resolve
  int flip = 0;
  double prev_vp[16] = {};
  double prev_pos[3] = {}, prev_fwd[3] = {};
  float prev_jx = 0, prev_jy = 0;  // the last frame's jitter (NDC)
  bool jittered = false;     // the camera's projection carries an offset (SS_TAA_JITTER_CAMERA_UNTIL only)
  uint32_t seq = 0;          // the jitter sequence's position
  const char* said = nullptr;
  uint32_t ok = 0, cuts = 0, stale = 0, skipped = 0, jitters = 0;
} g_taa;
bool g_taa_jitter = true;  // SS_TAA_JITTER=0 (a test aid): the resolve without the camera's jitter

template <typename T>
void SafeRelease(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

void GpuRelease();
void ForgetDevice();
void ForgetDepth();
void ReleaseDefaultPool() {
  GpuRelease();
  SafeRelease(g_r.copy);
  SafeRelease(g_r.mid);
  SafeRelease(g_r.edges);
  SafeRelease(g_r.weights);
  for (auto& t : g_r.hist) SafeRelease(t);
  for (auto& t : g_r.zlast) SafeRelease(t);
  g_taa.valid = false;
  SafeRelease(g_r.sb);
  g_r.w = g_r.h = 0;
}

// Every object of the passes (the menu changed which passes run): made again for the new settings at the next frame.
void ReleaseAll() {
  ReleaseDefaultPool();
  SafeRelease(g_r.decl);
  SafeRelease(g_r.quad_vs);
  SafeRelease(g_r.fxaa_ps);
  SafeRelease(g_r.cas_ps);
  for (auto& v : g_r.smaa_vs) SafeRelease(v);
  for (auto& p : g_r.smaa_ps) SafeRelease(p);
  SafeRelease(g_r.smaa_pred_ps);
  SafeRelease(g_r.taa_ps);
  SafeRelease(g_r.taa_depth_ps);
  SafeRelease(g_r.copy_ps);
  SafeRelease(g_r.area);
  SafeRelease(g_r.search);
  g_r.failed = false;
}

bool FillTexture(IDirect3DTexture9* t, const unsigned char* src, UINT row_bytes, UINT rows) {
  D3DLOCKED_RECT lr;
  if (FAILED(t->LockRect(0, &lr, nullptr, 0))) return false;
  for (UINT y = 0; y < rows; ++y) memcpy(static_cast<unsigned char*>(lr.pBits) + y * lr.Pitch, src + y * row_bytes, row_bytes);
  t->UnlockRect(0);
  return true;
}

// Which shaders a mode needs: the anti-aliasing, sharpening on or off, and for TAA the history format and whether the
// resolve writes the depth as a second target (from the device).
struct Mode {
  int aa = AA_OFF;
  bool sharp = false;
  bool ab = false;  // SS_TAA_AB_AT's sharpened variants
  bool mrt = false;
  D3DFORMAT hist_fmt = D3DFMT_UNKNOWN;
  bool Same(int a, float sh) const { return aa == a && sharp == (sh > 0.0f); }
};

// TAA's history format and whether the resolve writes the depth as a second target: from the device, once per device.
// The history is A16B16G16R16F where it can be a filtered render target: with 10 % of the current picture per frame,
// 8 bits per channel never converge on a difference under ~5/255 (a residue, banding on slow gradients). Test aids:
// SS_TAA_HISTORY8=1 (the final target's format, as before), SS_TAA_MRT=0 (the depth copy as a pass of its own).
void TaaFormats(IDirect3DDevice9* d, Mode* m) {
  m->hist_fmt = D3DFMT_UNKNOWN;
  m->mrt = false;
  const char* h8 = getenv("SS_TAA_HISTORY8");
  const char* mrt = getenv("SS_TAA_MRT");
  D3DCAPS9 caps = {};
  IDirect3D9* d3d = nullptr;
  D3DDISPLAYMODE mode = {};
  const bool have_caps = SUCCEEDED(d->GetDeviceCaps(&caps));
  if (!(h8 && *h8 == '1') && SUCCEEDED(d->GetDirect3D(&d3d)) && d3d) {
    if (FAILED(d->GetDisplayMode(0, &mode))) mode.Format = D3DFMT_X8R8G8B8;
    const D3DFORMAT f16 = D3DFMT_A16B16G16R16F;
    if (have_caps && SUCCEEDED(d3d->CheckDeviceFormat(caps.AdapterOrdinal, caps.DeviceType, mode.Format, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, f16)) &&
        d3d->CheckDeviceFormat(caps.AdapterOrdinal, caps.DeviceType, mode.Format, D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_FILTER, D3DRTYPE_TEXTURE, f16) == D3D_OK)
      m->hist_fmt = f16;
    d3d->Release();
  }
  m->mrt = have_caps && caps.NumSimultaneousRTs >= 2 && (caps.PrimitiveMiscCaps & D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS) &&
           !(mrt && *mrt == '0');
  ss_log("[postfx] TAA: history %s; %s", m->hist_fmt == D3DFMT_A16B16G16R16F ? "A16B16G16R16F (the result copied out into the final target's format)"
         : h8 && *h8 == '1' ? "in the final target's format (SS_TAA_HISTORY8=1)" : "in the final target's format (A16B16G16R16F cannot be a filtered render target here)",
         m->mrt ? "the resolve writes the depth for the next frame as a second render target"
         : mrt && *mrt == '0' ? "the depth copy is a pass of its own (SS_TAA_MRT=0)" : "the depth copy is a pass of its own (no two render targets of independent bit depths)");
}

Mode ModeFor(IDirect3DDevice9* d, int aa, float sharpen) {
  Mode m;
  m.aa = aa;
  m.sharp = sharpen > 0.0f;
  m.ab = IsTaa(aa) && getenv("SS_TAA_AB_AT");
  if (IsTaa(aa)) TaaFormats(d, &m);
  return m;
}

std::string SmaaSource() { return std::string(kSmaaPrefix) + std::string(g_smaa_hlsl_begin, g_smaa_hlsl_end) + kSmaaSuffix; }

// The mode's sources, for the worker (shadercache.cpp). Build below takes the byte code in the same order.
std::shared_ptr<SsShaderJob> Submit(const Mode& m) {
  auto j = std::make_shared<SsShaderJob>();
  j->tag = "[postfx]";
  char what[96];
  snprintf(what, sizeof what, "aa %s, sharpening %s", AaName(m.aa), m.sharp ? "on" : "off");
  j->what = what;
  j->Add(kQuadVs, "main", "vs_3_0", kOptimise3);
  if (m.aa == AA_FXAA) j->Add(kFxaaPs, "main", "ps_3_0", kOptimise3);
  if (m.sharp || m.ab) j->Add(kCasPs, "main", "ps_3_0", kOptimise3);
  if (m.aa == AA_SMAA || m.aa == AA_TAA_HIGH) {
    const std::string src = SmaaSource();
    static const char* const kVs[3] = {"EdgeVS", "WeightVS", "BlendVS"};
    static const char* const kPs[3] = {"EdgePS", "WeightPS", "BlendPS"};
    for (int i = 0; i < 3; ++i) {
      j->Add(src, kVs[i], "vs_3_0", kOptimise3);
      j->Add(src, kPs[i], "ps_3_0", kOptimise3);
    }
    j->Add(std::string(kSmaaPrefix) + kSmaaPredPrefix + std::string(g_smaa_hlsl_begin, g_smaa_hlsl_end) + kSmaaPredSuffix,
           "EdgePredPS", "ps_3_0", kOptimise3);
  }
  if (IsTaa(m.aa)) {
    j->Add(std::string(m.mrt ? "#define TAA_MRT 1\n" : "#define TAA_MRT 0\n") + (m.aa == AA_TAA_HIGH ? "#define TAA_HIGH 1\n" : "#define TAA_HIGH 0\n") + kTaaPs,
           "main", "ps_3_0", kOptimise3);
    if (!m.mrt) j->Add(kTaaDepthPs, "main", "ps_3_0", kOptimise3);
    j->Add(kCopyPs, "main", "ps_3_0", kOptimise3);
  }
  return ss_shader_submit(j);
}

// The shader objects, the vertex declaration and SMAA's tables from a finished job (render thread; none of them is
// lost on Reset). The byte code is taken in Submit's order.
bool Build(IDirect3DDevice9* d, const Mode& m, SsShaderJob& j) {
  size_t k = 0;
  auto code = [&](const char** entry) -> const DWORD* {
    if (k >= j.items.size()) return nullptr;
    const SsShaderJob::Item& it = j.items[k++];
    *entry = it.entry.c_str();
    return it.code.empty() ? nullptr : reinterpret_cast<const DWORD*>(it.code.data());
  };
  auto vs = [&](IDirect3DVertexShader9** out) {
    const char* entry = "";
    const DWORD* c = code(&entry);
    if (!c) return false;
    const HRESULT hr = d->CreateVertexShader(c, out);
    if (FAILED(hr)) ss_log("[postfx] CreateVertexShader(%s) failed (0x%08lx)", entry, static_cast<unsigned long>(hr));
    return SUCCEEDED(hr);
  };
  auto ps = [&](IDirect3DPixelShader9** out) {
    const char* entry = "";
    const DWORD* c = code(&entry);
    if (!c) return false;
    const HRESULT hr = d->CreatePixelShader(c, out);
    if (FAILED(hr)) ss_log("[postfx] CreatePixelShader(%s) failed (0x%08lx)", entry, static_cast<unsigned long>(hr));
    return SUCCEEDED(hr);
  };
  g_r.taa_mrt = m.mrt;
  g_r.taa_hist_fmt = m.hist_fmt;
  bool ok = true;
  static const D3DVERTEXELEMENT9 kDecl[] = {
      {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END()};
  IDirect3DVertexDeclaration9* decl = nullptr;
  ok = SUCCEEDED(d->CreateVertexDeclaration(kDecl, &decl));
  ok = ok && vs(&g_r.quad_vs);
  if (ok && m.aa == AA_FXAA) ok = ps(&g_r.fxaa_ps);
  if (ok && (m.sharp || m.ab)) ok = ps(&g_r.cas_ps);
  if (ok && (m.aa == AA_SMAA || m.aa == AA_TAA_HIGH)) {  // taa_high: SMAA 1x on the jittered picture, then the resolve
    for (int i = 0; ok && i < 3; ++i) ok = vs(&g_r.smaa_vs[i]) && ps(&g_r.smaa_ps[i]);
    ok = ok && SUCCEEDED(d->CreateTexture(AREATEX_WIDTH, AREATEX_HEIGHT, 1, 0, D3DFMT_A8L8, D3DPOOL_MANAGED, &g_r.area, nullptr)) &&
         FillTexture(g_r.area, areaTexBytes, AREATEX_PITCH, AREATEX_HEIGHT);
    ok = ok && SUCCEEDED(d->CreateTexture(SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1, 0, D3DFMT_L8, D3DPOOL_MANAGED, &g_r.search, nullptr)) &&
         FillTexture(g_r.search, searchTexBytes, SEARCHTEX_PITCH, SEARCHTEX_HEIGHT);
    if (!ok) ss_log("[postfx] SMAA's shaders or lookup textures could not be made");
    // The predicated edge pass: made whatever smaa_predication says now (the menu switches it live); without it the
    // colour-only pass runs.
    if (ok && !ps(&g_r.smaa_pred_ps)) ss_log("[postfx] SMAA's predicated edge pass could not be made; SMAA finds edges from the colour only");
  }
  if (ok && IsTaa(m.aa)) {
    ok = ps(&g_r.taa_ps) && (m.mrt || ps(&g_r.taa_depth_ps)) && ps(&g_r.copy_ps);
    if (!ok) ss_log("[postfx] TAA's shaders could not be made");
  }
  if (!ok) {
    SafeRelease(decl);
    return false;
  }
  g_r.decl = decl;
  return true;
}

// The compile for the mode on screen (g_aa, g_sharpen), and one for a mode the menu asked for (g_next): the mode on
// screen keeps running until the next one's shaders are done, then the next takes over in one frame.
std::shared_ptr<SsShaderJob> g_job, g_next;
Mode g_job_mode, g_next_mode;
float g_next_sharpen = 0.0f;
uint32_t g_job_frame = 0;  // g_frame_no when g_job was asked for

void Cancel(std::shared_ptr<SsShaderJob>& j) {
  if (j) InterlockedExchange(&j->cancelled, 1);
  j.reset();
}

// The objects of the mode on screen. Never waits: until the worker has its shaders, false (the frame is left as the
// game drew it, or, during a switch, ApplyPasses keeps the old mode, whose objects are made).
bool EnsureShaders(IDirect3DDevice9* d) {
  if (g_r.dev != d) {
    if (g_r.dev) ss_log("[postfx] the game made a new device (%p -> %p); the passes' objects are made again", g_r.dev, d);
    g_r = Res();  // the old device's objects are abandoned with it
    g_taa.valid = false;
    ForgetDevice();
    g_r.dev = d;
    if (g_next) {  // a switch under way: the new mode at once (nothing of the old is on screen any more)
      g_aa = g_next_mode.aa;
      g_sharpen = g_next_sharpen;
    }
    Cancel(g_job);  // asked for again (TAA's formats are the device's)
    Cancel(g_next);
  }
  if (g_r.failed) return false;
  if (g_r.decl) return true;
  if (!g_job || !g_job_mode.Same(g_aa, g_sharpen)) {
    Cancel(g_job);
    g_job_mode = ModeFor(d, g_aa, g_sharpen);
    g_job = Submit(g_job_mode);
    g_job_frame = g_frame_no;
  }
  if (!g_job->Done()) return false;
  const DWORD t0 = GetTickCount();
  if (!Build(d, g_job_mode, *g_job)) {
    ReleaseAll();
    g_r.failed = true;
    ss_log("[postfx] anti-aliasing and sharpening stay off");
    return false;
  }
  ss_log("[postfx] shaders ready for aa %s, sharpen %.2f: the passes run from frame %u (asked for at frame %u, %lu ms ago; "
         "objects made in %lu ms on the render thread)", AaName(g_aa), g_sharpen, g_frame_no, g_job_frame,
         static_cast<unsigned long>(GetTickCount() - g_job->submitted), static_cast<unsigned long>(GetTickCount() - t0));
  return true;
}

// The settings menu's change, taken over by the render thread at the start of a filtered frame. Only the strength:
// a shader constant, at once. Another pass: compiled on the worker while the old mode stays on screen; switched in
// one frame when done (or at once when nothing is on screen to keep, or when the new mode is off).
void TakeChange(IDirect3DDevice9* d) {
  if (InterlockedExchange(&g_want_changed, 0)) {
    const int aa = static_cast<int>(g_want_aa);
    const float sh = g_want_sharpen;
    ss_log("[postfx] from the settings menu: aa %s, sharpen %.2f (frame %u)", AaName(aa), sh, g_frame_no);
    if (g_next_mode.Same(aa, sh) && g_next) {
      g_next_sharpen = sh;  // the switch under way already makes these passes
    } else {
      Cancel(g_next);
      if ((aa == g_aa && (sh > 0.0f) == (g_sharpen > 0.0f)) || (aa == AA_OFF && sh <= 0.0f) || !g_r.decl || g_r.failed) {
        if (aa != g_aa || (sh > 0.0f) != (g_sharpen > 0.0f)) ReleaseAll();
        g_aa = aa;
        g_sharpen = sh;
      } else {
        g_next_mode = ModeFor(d, aa, sh);
        g_next_sharpen = sh;
        g_next = Submit(g_next_mode);
        g_job_frame = g_frame_no;
        ss_log("[postfx] aa %s, sharpen %.2f compiles on the worker; aa %s, sharpen %.2f stays on screen until then", AaName(aa), sh,
               AaName(g_aa), g_sharpen);
      }
    }
  }
  if (g_next && g_next->Done()) {  // the new mode takes over this frame
    ReleaseAll();
    Cancel(g_job);
    g_job = g_next;
    g_job_mode = g_next_mode;
    g_next.reset();
    g_aa = g_job_mode.aa;
    g_sharpen = g_next_sharpen;
  }
}

bool MakeTarget(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f, IDirect3DTexture9** out) {
  return SUCCEEDED(d->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, f, D3DPOOL_DEFAULT, out, nullptr));
}

// The size-dependent targets: made at the final target's size and format, again when that changes.
bool EnsureTargets(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f) {
  if (g_r.copy && g_r.w == w && g_r.h == h && g_r.fmt == f) return true;
  ReleaseDefaultPool();
  // TAA's sharpening reads the history itself: no intermediate. taa_high: SMAA's result, which the resolve reads.
  const bool two = g_aa == AA_TAA_HIGH || (g_aa != AA_OFF && g_aa != AA_TAA && g_sharpen > 0.0f);
  const bool smaa = g_aa == AA_SMAA || g_aa == AA_TAA_HIGH;
  const D3DFORMAT hf = g_r.taa_hist_fmt != D3DFMT_UNKNOWN ? g_r.taa_hist_fmt : f;
  bool ok = MakeTarget(d, w, h, f, &g_r.copy) && (!two || MakeTarget(d, w, h, f, &g_r.mid)) &&
            (!smaa || (MakeTarget(d, w, h, D3DFMT_A8R8G8B8, &g_r.edges) && MakeTarget(d, w, h, D3DFMT_A8R8G8B8, &g_r.weights))) &&
            (!IsTaa(g_aa) || (MakeTarget(d, w, h, hf, &g_r.hist[0]) && MakeTarget(d, w, h, hf, &g_r.hist[1]) &&
                                MakeTarget(d, w, h, D3DFMT_R32F, &g_r.zlast[0]) && MakeTarget(d, w, h, D3DFMT_R32F, &g_r.zlast[1]))) &&
            SUCCEEDED(d->CreateStateBlock(D3DSBT_ALL, &g_r.sb));
  if (!ok) {
    ReleaseDefaultPool();
    static int said = 0;
    if (said++ < 3) ss_log("[postfx] the targets could not be made at %ux%u (format %d); this frame is left as it is", w, h, f);
    return false;
  }
  g_r.w = w;
  g_r.h = h;
  g_r.fmt = f;
  ss_log("[postfx] targets made at %ux%u (format %d)", w, h, f);
  return true;
}

struct Vertex {
  float x, y, z, w, u, v;
};

// One full-screen pass into `target`; the quad is moved by half a pixel so that texel centres meet pixel centres.
void Pass(IDirect3DDevice9* d, IDirect3DSurface9* target, IDirect3DVertexShader9* vs, IDirect3DPixelShader9* ps) {
  d->SetRenderTarget(0, target);  // also sets the viewport to the whole target
  d->SetVertexShader(vs);
  d->SetPixelShader(ps);
  const float dx = -1.0f / g_r.w, dy = 1.0f / g_r.h;
  const Vertex q[4] = {{-1 + dx, 1 + dy, 0, 1, 0, 0}, {1 + dx, 1 + dy, 0, 1, 1, 0}, {-1 + dx, -1 + dy, 0, 1, 0, 1}, {1 + dx, -1 + dy, 0, 1, 1, 1}};
  d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vertex));
}

void Sampler(IDirect3DDevice9* d, DWORD s, IDirect3DBaseTexture9* t, bool linear) {
  d->SetTexture(s, t);
  d->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  d->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
  d->SetSamplerState(s, D3DSAMP_MINFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
  d->SetSamplerState(s, D3DSAMP_MAGFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
  d->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
  d->SetSamplerState(s, D3DSAMP_MIPMAPLODBIAS, 0);
  d->SetSamplerState(s, D3DSAMP_MAXMIPLEVEL, 0);
  d->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
}

struct Counts {
  uint32_t under_ui = 0, whole = 0, skipped = 0, frames = 0;
  bool said_under_ui = false, said_whole = false;
} g_n;

// ------------------------------------------------------------------------------- the game's depth (SMAA predication)
// The G-buffer's depth is an INTZ texture the game makes and binds itself (docs/plan-hdr.md 1.1, run 1); the motion
// blur reads it after the tone map, so at the scene/UI split it still holds the scene's depth. The device's
// SetDepthStencilSurface (slot 39, d3d9.dll's memory) is wrapped and notes every INTZ surface the game binds (one per
// size, the frame it was last bound in). No reference is kept (a Reset must find the game's objects released): a
// surface is only used when it was bound this frame or the one before, and its description is read again then.
// This does not depend on HDR's hooks.
const D3DFORMAT kIntz = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));
const GUID kIidTexture9 = {0x85c31227, 0x3de5, 0x4f00, {0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5}};
struct DepthSeen {
  IDirect3DSurface9* s;
  UINT w, h;
  uint32_t frame;
};
DepthSeen g_depth[4] = {};
IDirect3DSurface9* g_not_intz = nullptr;  // the last bound surface found not to be INTZ (not described again)

void ForgetDepth() {
  memset(g_depth, 0, sizeof g_depth);
  g_not_intz = nullptr;
}

void NoteDepth(IDirect3DSurface9* s) {
  if (!s || s == g_not_intz) return;
  for (auto& e : g_depth)
    if (e.s == s) {
      e.frame = g_frame_no;
      return;
    }
  D3DSURFACE_DESC dd;
  if (FAILED(s->GetDesc(&dd)) || dd.Format != kIntz) {
    g_not_intz = s;
    return;
  }
  DepthSeen* slot = nullptr;
  for (auto& e : g_depth)
    if (!slot && e.s && e.w == dd.Width && e.h == dd.Height) slot = &e;
  for (auto& e : g_depth)
    if (!slot && !e.s) slot = &e;
  if (!slot) {
    slot = &g_depth[0];
    for (auto& e : g_depth)
      if (e.frame < slot->frame) slot = &e;
  }
  *slot = {s, dd.Width, dd.Height, g_frame_no};
}

// The depth texture of the picture's size for this frame (referenced; the caller releases it), or nullptr.
IDirect3DTexture9* DepthFor(IDirect3DSurface9* bound, UINT w, UINT h) {
  IDirect3DSurface9* cand = nullptr;
  D3DSURFACE_DESC dd;
  if (bound && SUCCEEDED(bound->GetDesc(&dd)) && dd.Format == kIntz && dd.Width == w && dd.Height == h) cand = bound;
  for (auto& e : g_depth)
    if (!cand && e.s && e.w == w && e.h == h && g_frame_no - e.frame <= 1) cand = e.s;
  if (!cand || FAILED(cand->GetDesc(&dd)) || dd.Format != kIntz || dd.Width != w || dd.Height != h) return nullptr;
  IDirect3DTexture9* t = nullptr;
  if (FAILED(cand->GetContainer(kIidTexture9, reinterpret_cast<void**>(&t)))) return nullptr;
  return t;
}

// What the edge pass used last (for the log): -1 not yet, 0 colour only, 1 with the depth.
int g_pred_used = -1;
void SayPred(int used, const char* why) {
  if (used == g_pred_used) return;
  g_pred_used = used;
  ss_log("[postfx] SMAA edges: %s", why);
}

// ---------------------------------------------------------------------------------- GPU time (hdr_gpu, an aid)
// One frame in 16: timestamps before the copy, after the AA's edge pass (SMAA) and after the last pass, read 4 frames
// later without waiting. Logged every 3600 frames (Report) and whenever what is timed changes.
struct GpuTime {
  IDirect3DQuery9* q[3] = {};
  bool pending = false;
  uint32_t issued = 0;
  int mode = -1, pending_mode = -1;  // aa * 2 + predication used
  double edge = 0, all = 0, freq = 0;
  uint32_t n = 0;
  bool none = false;
} g_gt;

const char* ModeName(int m) {
  static const char* const k[] = {"off", "off", "fxaa", "fxaa", "smaa (colour only)", "smaa (depth predication)",
                                  "taa (no depth: not resolved)", "taa (copy + resolve | + depth copy, copy out)",
                                  "taa_high (no depth: not resolved)", "taa_high (copy + SMAA + resolve | + depth copy, copy out)"};
  return m >= 0 && m < 10 ? k[m] : "?";
}

void GpuFlush(const char* why) {
  if (g_gt.n)
    ss_log("[postfx] GPU (timestamps, %u samples, %s) aa %s, sharpen %.2f: copy + edges %.3f ms, all passes %.3f ms", g_gt.n, why,
           ModeName(g_gt.mode), g_sharpen, g_gt.edge / g_gt.n, g_gt.all / g_gt.n);
  g_gt.edge = g_gt.all = 0;
  g_gt.n = 0;
}

// A new device: the old one's queries and depth surfaces are abandoned with it (not released).
void ForgetDevice() {
  for (auto& q : g_gt.q) q = nullptr;
  g_gt.pending = false;
  ForgetDepth();
}

void GpuRelease() {
  for (auto& q : g_gt.q) SafeRelease(q);
  g_gt.pending = false;
}

bool GpuStamp(IDirect3DDevice9* d, int i) {
  if (!g_gt.q[i] && FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &g_gt.q[i]))) {
    if (!g_gt.none) ss_log("[postfx] no timestamp queries on this device; hdr_gpu times nothing here");
    g_gt.none = true;
    return false;
  }
  g_gt.q[i]->Issue(D3DISSUE_END);
  return true;
}

void GpuRead(IDirect3DDevice9* d) {
  if (!g_gt.pending || g_frame_no - g_gt.issued < 4) return;
  if (g_gt.freq == 0) {
    IDirect3DQuery9* fq = nullptr;
    UINT64 f = 0;
    if (SUCCEEDED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &fq)) && fq) {
      fq->Issue(D3DISSUE_END);
      for (int i = 0; i < 100 && fq->GetData(&f, sizeof f, D3DGETDATA_FLUSH) == S_FALSE; ++i) Sleep(0);
      fq->Release();
    }
    g_gt.freq = f ? double(f) : 1e9;
  }
  UINT64 v[3] = {};
  for (int i = 0; i < 3; ++i)
    if (!g_gt.q[i] || g_gt.q[i]->GetData(&v[i], sizeof v[i], 0) != S_OK) {
      if (g_frame_no - g_gt.issued > 120) g_gt.pending = false;
      return;
    }
  g_gt.pending = false;
  if (v[1] < v[0] || v[2] < v[1]) return;
  if (g_gt.pending_mode != g_gt.mode) {
    GpuFlush("until a change");
    g_gt.mode = g_gt.pending_mode;
  }
  g_gt.edge += double(v[1] - v[0]) / g_gt.freq * 1000.0;
  g_gt.all += double(v[2] - v[0]) / g_gt.freq * 1000.0;
  ++g_gt.n;
}

// Test aids: SS_SMAA_AB_AT (seconds after the first filtered frame; see Apply) and SS_SMAA_PRED_OFF_AT (from that
// second on, smaa_predication = false, for a GPU time of both in one run).
DWORD g_t0 = 0;
int AbDue() {
  static std::vector<int> at;
  static size_t next = 0;
  static bool init = false;
  if (!init) {
    init = true;
    if (const char* e = getenv("SS_SMAA_AB_AT"))
      for (const char* p = e; *p;) {
        at.push_back(atoi(p));
        const char* c = strchr(p, ',');
        if (!c) break;
        p = c + 1;
      }
  }
  if (next >= at.size() || (GetTickCount() - g_t0) / 1000 < static_cast<DWORD>(at[next])) return -1;
  return at[next++];
}
void PredOffDue() {
  static const int off_at = [] { const char* e = getenv("SS_SMAA_PRED_OFF_AT"); return e ? atoi(e) : -1; }();
  static bool done = false;
  if (off_at < 0 || done || (GetTickCount() - g_t0) / 1000 < static_cast<DWORD>(off_at)) return;
  done = true;
  InterlockedExchange(&g_pred, 0);
  ss_log("[postfx] test aid: smaa_predication off from now (SS_SMAA_PRED_OFF_AT=%d)", off_at);
}

// ------------------------------------------------------------------------------------------- TAA (aa = taa)
// The jitter. The first way was an offset in the scene camera's P[2][0], P[2][1] after the game's update, published
// by the game's own 0x50C250. That reaches only the draws that take the camera's VP or P as shader parameters (c8,
// c4 and a few at c33); most of the scene is drawn with a world * view * projection matrix per object (c33 with the
// matrix's columns in the registers, c64 with its rows), and those do not carry an offset put into the camera after
// the update (SS_TAA_PROBE_AT with SS_TAA_JITTER_CAMERA_UNTIL: the plain VP uploads at c33 come both with and
// without it, and consecutive pictures of the standing start grid do not move though the offset cycles). So the
// picture stood still while the resolve took the offset out again: a standing picture wobbled.
// So the offset goes in where the matrices reach the device: right after the game's update (patch.cpp's frame step;
// the render follows on the same thread, a held frame is not rendered) this frame's sub-pixel offset is chosen
// (Halton 2, 3; 8 positions), and until the scene/UI split the device's SetVertexShaderConstantF (a slot of d3d9's
// vtable) adds it to every clip matrix of the scene camera that is uploaded (ClipOfCamera). The camera and the
// game's memory are not written. Only a frame after a resolved one is jittered, and the resolve takes out exactly
// the offset the uploads got (none when no upload was moved): a frame that was not jittered is never un-jittered.
// The resolve reads the camera after the render (V, P) and reprojects through inverse(VP now) * VP last.
using CamFn = void(__attribute__((thiscall)) *)(void*);

bool Sig(uintptr_t va, const uint8_t* b, size_t n) { return Readable(va, n) && memcmp(reinterpret_cast<const void*>(va), b, n) == 0; }

// The camera code the jitter and the reprojection rely on must be the known build's.
bool TaaCodeOk() {
  static int ok = -1;
  if (ok >= 0) return ok == 1;
  // 0x50C250: push ebx; push ebp; push esi; mov esi,ecx; cmp dword [esi+0x21C],0
  static const uint8_t publish[] = {0x53, 0x55, 0x56, 0x8B, 0xF1, 0x83, 0xBE, 0x1C, 0x02, 0x00, 0x00, 0x00};
  // 0x50C21E: lea eax,[esi+0x158]; push eax; lea ecx,[esp+0xC]; push edi (V, esi+0xD8); push ecx; call 0x49F350;
  // add esp,0xC; push eax; lea ecx,[esi+0x198] (VP = V * P)
  static const uint8_t vp[] = {0x8D, 0x86, 0x58, 0x01, 0x00, 0x00, 0x50, 0x8D, 0x4C, 0x24, 0x0C, 0x57, 0x51, 0xE8, 0x20, 0x31,
                               0xF9, 0xFF, 0x83, 0xC4, 0x0C, 0x50, 0x8D, 0x8E, 0x98, 0x01, 0x00, 0x00};
  // 0x50AB33: the perspective projection's P[2][0] = P[2][1] = 0 (movss [eax+0x20],xmm0; movss [eax+0x24],xmm0)
  static const uint8_t p20[] = {0xF3, 0x0F, 0x11, 0x40, 0x20, 0xF3, 0x0F, 0x11, 0x40, 0x24};
  // 0x50C0E4: lea edi,[esi+0xD8] (V)
  static const uint8_t v[] = {0x8D, 0xBE, 0xD8, 0x00, 0x00, 0x00};
  ok = Sig(kCamPublish, publish, sizeof publish) && Sig(0x50C21E, vp, sizeof vp) && Sig(0x50AB33, p20, sizeof p20) &&
       Sig(0x50C0E4, v, sizeof v);
  if (!ok) ss_log("[postfx] TAA: the camera code is not the known build; no jitter and no reprojection (the passes leave the picture as it is)");
  return ok == 1;
}

// The single view's perspective camera, or 0 (with why). Split screen (two views) has no TAA.
uintptr_t TaaCamera(const char** why) {
  if (!TaaCodeOk()) {
    *why = "the camera code is not the known build";
    return 0;
  }
  const uintptr_t mgr = Rd(kViewMgr);
  if (!mgr || !Readable(mgr, kViewCams + 2 * kCamSize)) {
    *why = "no viewport manager";
    return 0;
  }
  const uint32_t views = Rd(mgr + 8) == 1 ? 1 : Rd(mgr), idx = Rd(mgr + 0xC);
  if (views != 1 || idx > 1) {
    *why = "more than one view (split screen)";
    return 0;
  }
  const uintptr_t cam = mgr + kViewCams + idx * kCamSize;
  const uint8_t* flags = reinterpret_cast<const uint8_t*>(cam);
  if (flags[kCamOrtho] || flags[kCamCustom]) {
    *why = "the view's camera is not a plain perspective one";
    return 0;
  }
  return cam;
}

template <typename A, typename B>
void Mul(const A* a, const B* b, double* o) {  // o = a * b (4x4, row-major)
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      double s = 0;
      for (int k = 0; k < 4; ++k) s += double(a[i * 4 + k]) * double(b[k * 4 + j]);
      o[i * 4 + j] = s;
    }
}

bool Invert(const double* m, double* o) {  // Gauss-Jordan with partial pivoting
  double a[4][8];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 8; ++j) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1.0 : 0.0);
  for (int c = 0; c < 4; ++c) {
    int piv = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
    if (std::fabs(a[piv][c]) < 1e-12) return false;
    if (piv != c)
      for (int j = 0; j < 8; ++j) std::swap(a[c][j], a[piv][j]);
    const double inv = 1.0 / a[c][c];
    for (int j = 0; j < 8; ++j) a[c][j] *= inv;
    for (int r = 0; r < 4; ++r)
      if (r != c) {
        const double f = a[r][c];
        if (f != 0)
          for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
      }
  }
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) o[i * 4 + j] = a[i][j + 4];
  return true;
}

double Halton(uint32_t i, uint32_t b) {
  double f = 1, r = 0;
  for (; i; i /= b) {
    f /= b;
    r += f * (i % b);
  }
  return r;
}

void TaaSay(const char* what) {
  if (what == g_taa.said) return;
  g_taa.said = what;
  ss_log("[postfx] TAA: %s", what);
}

// The jitter in the scene's draws (render thread). `on` from the update of a frame that is to be jittered until the
// scene/UI split (or EndScene); a, b: the camera's P[2][2], P[3][2] (clip z = a * w + b marks its clip matrices).
struct Jit {
  bool on = false;
  float jx = 0, jy = 0;      // this frame's offset in NDC
  float a = 0, b = 0;
  uint32_t patched = 0;      // uploads moved this frame
  uint64_t patched_sum = 0;  // ... and since the last report, over `frames` jittered frames
  uint32_t frames = 0, said = 0;
} g_jit;
using SetVsConstFn = HRESULT(__attribute__((stdcall)) *)(IDirect3DDevice9*, UINT, const float*, UINT);
SetVsConstFn g_set_vs_const = nullptr;
constexpr int kSetVsConstSlot = 94;

// Is the 4x4 block a clip matrix of the scene camera (an affine matrix times its view-projection, or its projection
// alone)? Its z column is then a times its w column, plus b in the translation row, and it has a perspective w.
// z, w: the columns' four values each.
inline bool ClipOfCamera(const float* z, const float* w, float a, float b) {
  if (std::fabs(w[0]) + std::fabs(w[1]) + std::fabs(w[2]) < 1e-6f) return false;  // no perspective: 2D, a light, identity
  for (int i = 0; i < 3; ++i)
    if (std::fabs(z[i] - a * w[i]) > 1e-4f * (std::fabs(z[i]) + std::fabs(w[i])) + 1e-6f) return false;
  return std::fabs(z[3] - a * w[3] - b) <= 0.02f * std::fabs(b) + 4e-6f * (std::fabs(z[3]) + std::fabs(w[3]));
}

void ProbeCall(UINT reg, const float* data, UINT count);
bool g_probe_on = false;

// SetVertexShaderConstantF of the game's device. While a frame's scene is drawn jittered, a clip matrix of the scene
// camera at the start of the upload gets the offset: x += jx * w, y += jy * w (a copy is passed on; the game's
// memory is not written). Both layouts the title uploads are known: the matrix's columns in the registers (c4, c8,
// c33, c75: register 0 += jx * register 3, register 1 += jy * register 3) and its rows (c64).
HRESULT __attribute__((stdcall, force_align_arg_pointer)) SetVsConstHook(IDirect3DDevice9* d, UINT reg, const float* data, UINT count) {
  if (g_probe_on && data && ss_own_drawing == 0) ProbeCall(reg, data, count);
  Jit& j = g_jit;
  if (!j.on || count < 4 || count > 64 || !data || ss_own_drawing != 0) return g_set_vs_const(d, reg, data, count);
  float m[64 * 4];
  if (ClipOfCamera(data + 8, data + 12, j.a, j.b)) {  // columns in the registers
    memcpy(m, data, count * 16);
    for (int i = 0; i < 4; ++i) {
      m[i] += j.jx * data[12 + i];
      m[4 + i] += j.jy * data[12 + i];
    }
  } else {
    const float z[4] = {data[2], data[6], data[10], data[14]}, w[4] = {data[3], data[7], data[11], data[15]};
    if (!ClipOfCamera(z, w, j.a, j.b)) return g_set_vs_const(d, reg, data, count);
    memcpy(m, data, count * 16);  // rows in the registers
    for (int i = 0; i < 4; ++i) {
      m[i * 4] += j.jx * w[i];
      m[i * 4 + 1] += j.jy * w[i];
    }
  }
  ++j.patched;
  return g_set_vs_const(d, reg, m, count);
}

// Test aid: SS_TAA_PROBE_AT="150,200" (seconds after the first filtered frame): for one jittered frame each, every
// SetVertexShaderConstantF from the update to the scene/UI split is compared with the camera's matrices as the game
// has them after its update (4x4 blocks at any register of the upload, both layouts): the camera's VP or P itself,
// with or without the offset in P[2][0], P[2][1] (SS_TAA_JITTER_CAMERA_UNTIL), or another clip matrix of the camera.
// The tally is logged per first register.
struct Probe {
  std::vector<int> at;
  size_t next = 0;
  bool init = false;
  float ref[4][16];  // VP, P as in the camera; VP, P with P[2][0] = P[2][1] = 0
  float a = 0, b = 0;
  uint32_t calls = 0, said = 0;
  struct Row { uint32_t n[2][5]; };  // [rows, columns in the registers][the four of ref, another clip matrix]
  Row reg[256];
} g_probe;

void ProbeCall(UINT reg, const float* data, UINT count) {
  Probe& p = g_probe;
  ++p.calls;
  for (UINT o = 0; o + 4 <= count && reg + o < 256; ++o) {
    for (int tr = 0; tr < 2; ++tr) {
      float a[16];
      for (int i = 0; i < 16; ++i) a[i] = tr ? data[o * 4 + (i % 4) * 4 + i / 4] : data[o * 4 + i];
      const float z[4] = {a[2], a[6], a[10], a[14]}, w[4] = {a[3], a[7], a[11], a[15]};
      if (!ClipOfCamera(z, w, p.a, p.b)) continue;
      float dist[4];
      int best = 0;
      for (int k = 0; k < 4; ++k) {
        float dmax = 0, n = 0;
        for (int i = 0; i < 16; ++i) {
          dmax = std::max(dmax, std::fabs(a[i] - p.ref[k][i]));
          n = std::max(n, std::fabs(p.ref[k][i]));
        }
        dist[k] = dmax / n;
        if (dist[k] < dist[best]) best = k;
      }
      const int kind = dist[best] < 1e-5f ? best : 4;
      ++p.reg[reg + o].n[tr][kind];
      if (p.said < 16 && p.reg[reg + o].n[tr][kind] <= 1) {
        ++p.said;
        static const char* const kName[5] = {"the camera's VP as it is", "the camera's P as it is", "VP without the camera's offset",
                                             "P without the camera's offset", "another clip matrix of the camera"};
        ss_log("[postfx] TAA probe: call %u c%u (%s in the registers): %s (difference from VP %.1e, P %.1e, VP without offset %.1e, P without %.1e)",
               p.calls, reg + o, tr ? "columns" : "rows", kName[kind], dist[0], dist[1], dist[2], dist[3]);
      }
    }
  }
}

void ProbeArm() {
  Probe& p = g_probe;
  if (!p.init) {
    p.init = true;
    if (const char* e = getenv("SS_TAA_PROBE_AT"))
      for (const char* c = e; *c;) {
        p.at.push_back(atoi(c));
        const char* n = strchr(c, ',');
        if (!n) break;
        c = n + 1;
      }
  }
  if (g_probe_on || p.next >= p.at.size() || !g_t0 || !g_set_vs_const) return;
  if ((GetTickCount() - g_t0) / 1000 < static_cast<DWORD>(p.at[p.next])) return;
  const char* why = nullptr;
  const uintptr_t cam = TaaCamera(&why);
  if (!cam || !(g_jit.on || g_taa.jittered)) return;  // a jittered frame is wanted
  float V[16], P[16];
  memcpy(V, reinterpret_cast<const void*>(cam + kCamView), sizeof V);
  memcpy(P, reinterpret_cast<const void*>(cam + kCamProj), sizeof P);
  double vp[16];
  Mul(V, P, vp);
  for (int i = 0; i < 16; ++i) p.ref[0][i] = float(vp[i]), p.ref[1][i] = P[i];
  const float cx = P[8], cy = P[9];
  P[8] = P[9] = 0;
  Mul(V, P, vp);
  for (int i = 0; i < 16; ++i) p.ref[2][i] = float(vp[i]), p.ref[3][i] = P[i];
  p.a = P[10];
  p.b = P[14];
  memset(p.reg, 0, sizeof p.reg);
  p.calls = p.said = 0;
  g_probe_on = true;
  ss_log("[postfx] TAA probe at %d s: the camera's P[2][0], P[2][1] after the update %.5f, %.5f; the offset for the uploads %.5f, %.5f (NDC)",
         p.at[p.next], cx, cy, g_jit.on ? g_jit.jx : 0.0f, g_jit.on ? g_jit.jy : 0.0f);
}

void ProbeEnd() {
  Probe& p = g_probe;
  if (!g_probe_on) return;
  g_probe_on = false;
  ss_log("[postfx] TAA probe at %d s: %u SetVertexShaderConstantF calls up to the split, %u of them moved by the hook",
         p.at[p.next], p.calls, g_jit.patched);
  for (int r = 0; r < 256; ++r)
    for (int tr = 0; tr < 2; ++tr) {
      const uint32_t* n = p.reg[r].n[tr];
      if (n[0] + n[1] + n[2] + n[3] + n[4])
        ss_log("[postfx] TAA probe: c%d (%s in the registers): VP as in the camera %u, P as in the camera %u, VP without the camera's offset %u, P without %u, other clip matrices %u",
               r, tr ? "columns" : "rows", n[0], n[1], n[2], n[3], n[4]);
    }
  ++p.next;
}

// After the game's update: this frame's offset for the scene's draws (or none).
void TaaJitter() {
  // Test aid: SS_TAA_JITTER_OFF_AT=<s> (seconds after the first filtered frame): no jitter from then on.
  static const int off_at = [] { const char* e = getenv("SS_TAA_JITTER_OFF_AT"); return e ? atoi(e) : -1; }();
  if (off_at >= 0 && g_taa_jitter && g_t0 && (GetTickCount() - g_t0) / 1000 >= static_cast<DWORD>(off_at)) {
    g_taa_jitter = false;
    ss_log("[postfx] test aid: TAA without jitter from now (SS_TAA_JITTER_OFF_AT=%d)", off_at);
  }
  // Test aid: SS_TAA_JITTER_CAMERA_UNTIL=<s>: until then the offset goes into the camera's P and is published (the
  // first way), not into the uploads; SS_TAA_PROBE_AT and SS_TAA_AB_AT then show that the scene's draws do not get it.
  static const int cam_until = [] { const char* e = getenv("SS_TAA_JITTER_CAMERA_UNTIL"); return e ? atoi(e) : -1; }();
  const bool cam_way = cam_until >= 0 && (!g_t0 || (GetTickCount() - g_t0) / 1000 < static_cast<DWORD>(cam_until));
  Jit& j = g_jit;
  j.on = false;
  j.patched = 0;
  j.jx = j.jy = 0;
  const bool want = IsTaa(g_aa) && g_taa_jitter && g_taa.valid && g_taa.last_frame + 1 == g_frame_no && g_r.w && g_r.h &&
                    (g_set_vs_const || cam_way);
  if (!want && !g_taa.jittered) return;
  const char* why = nullptr;
  const uintptr_t cam = TaaCamera(&why);
  if (!cam) return;
  float P[16];
  memcpy(P, reinterpret_cast<const void*>(cam + kCamProj), sizeof P);
  if (!(P[11] == 1.0f && P[15] == 0.0f)) return;  // not the perspective projection 0x50AAF0 makes
  float jx = 0, jy = 0;
  if (want) {
    const uint32_t i = g_taa.seq++ % 8 + 1;
    // pixels -> NDC (x right, y up; the picture's rows go down)
    jx = float((Halton(i, 2) - 0.5) * 2.0 / g_r.w);
    jy = float(-(Halton(i, 3) - 0.5) * 2.0 / g_r.h);
    if (g_taa.jitters++ == 0) ss_log("[postfx] TAA: first frame jittered (Halton 2,3, 8 positions; the offset goes into the scene's clip matrices as they are uploaded)");
  }
  if (cam_way || g_taa.jittered) {
    float V[16];
    memcpy(V, reinterpret_cast<const void*>(cam + kCamView), sizeof V);
    P[8] = cam_way ? jx : 0.0f;
    P[9] = cam_way ? jy : 0.0f;
    double vp[16];
    Mul(V, P, vp);
    float VP[16];
    for (int i = 0; i < 16; ++i) VP[i] = float(vp[i]);
    memcpy(reinterpret_cast<void*>(cam + kCamProj), P, sizeof P);
    memcpy(reinterpret_cast<void*>(cam + kCamViewProj), VP, sizeof VP);
    reinterpret_cast<CamFn>(kCamPublish)(reinterpret_cast<void*>(cam));
    g_taa.jittered = cam_way && want;
    return;
  }
  if (!want) return;
  j.a = P[10];
  j.b = P[14];
  j.jx = jx;
  j.jy = jy;
  j.on = true;
}

// The scene is drawn (the split, or EndScene): the uploads are passed on as they are again.
void TaaJitterEnd() {
  Jit& j = g_jit;
  if (!j.on) return;
  j.on = false;
  j.patched_sum += j.patched;
  ++j.frames;
  if (!j.patched && j.said++ < 3) ss_log("[postfx] TAA: no clip matrix of the scene camera was uploaded in a frame to be jittered: resolved as not jittered");
}

// Test aid: SS_TAA_AB_AT="150,200" (seconds after the first filtered frame): from each, 4 frames in a row are saved
// before the UI: taa-<s>-<k>-in (the jittered picture) and taa-<s>-<k>-out (the result), and the result sharpened at
// 25 % and 50 %: taa-<s>-<k>-sh25, -sh50.
int TaaShotDue(int* k) {
  static std::vector<int> at;
  static size_t next = 0;
  static int left = 0, cur = -1;
  static bool init = false;
  if (!init) {
    init = true;
    if (const char* e = getenv("SS_TAA_AB_AT"))
      for (const char* p = e; *p;) {
        at.push_back(atoi(p));
        const char* c = strchr(p, ',');
        if (!c) break;
        p = c + 1;
      }
  }
  if (!left && next < at.size() && (GetTickCount() - g_t0) / 1000 >= static_cast<DWORD>(at[next])) {
    cur = at[next++];
    left = 4;
  }
  if (!left) return -1;
  *k = 4 - left--;
  return cur;
}

// deband (deband.cpp): one code step of `target`'s format for the dither of the passes here that write a filtered value
// into it (TAA's copy-out, sharpening); 0 when deband is off or the target is not 8 / 10 bits per channel.
float DitherStep(IDirect3DSurface9* target) {
  D3DSURFACE_DESC desc = {};
  if (!ss_deband_on() || !target || FAILED(target->GetDesc(&desc))) return 0.0f;
  switch (desc.Format) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8: return 1.0f / 255.0f;
    case D3DFMT_A2R10G10B10: case D3DFMT_A2B10G10R10: return 1.0f / 1023.0f;
    default: return 0.0f;
  }
}

// The history into `out` (the final target): a pass where the formats differ, else a copy.
bool TaaCopyOut(IDirect3DDevice9* d, IDirect3DTexture9* hist, IDirect3DSurface9* hs, IDirect3DSurface9* out) {
  if (g_r.taa_hist_fmt == D3DFMT_UNKNOWN) return SUCCEEDED(d->StretchRect(hs, nullptr, out, nullptr, D3DTEXF_NONE));
  const float dith[4] = {DitherStep(out), 0.0f, 0.0f, 0.0f};
  d->SetPixelShaderConstantF(0, dith, 1);
  Sampler(d, 0, hist, false);
  Pass(d, out, g_r.quad_vs, g_r.copy_ps);
  Sampler(d, 0, nullptr, false);
  return true;
}

// The resolve of the current picture (g_r.copy) with the last result into hist[flip], then copied into `out` (nullptr:
// not copied; the sharpening reads *result, the history, itself).
// False: nothing written (the caller's picture stays as it is) and the history is dropped.
bool TaaResolve(IDirect3DDevice9* d, IDirect3DTexture9* cur_tex, IDirect3DTexture9* depth, IDirect3DSurface9* out, bool timed, IDirect3DTexture9** result) {
  Taa& t = g_taa;
  const char* why = nullptr;
  const uintptr_t cam = TaaCamera(&why);
  if (cam && !depth) why = "no readable depth (INTZ) of the picture's size bound this frame: the picture is left as it is";
  if (!cam || !depth) {
    t.valid = false;
    ++t.skipped;
    TaaSay(why);
    return false;
  }
  float Vf[16], Pf[16];
  memcpy(Vf, reinterpret_cast<const void*>(cam + kCamView), sizeof Vf);
  memcpy(Pf, reinterpret_cast<const void*>(cam + kCamProj), sizeof Pf);
  // The offset this frame's uploads got (none moved: the picture is not jittered and nothing is taken out).
  const float jx = g_jit.patched ? g_jit.jx : 0.0f, jy = g_jit.patched ? g_jit.jy : 0.0f;
  Pf[8] = Pf[9] = 0;  // (SS_TAA_JITTER_CAMERA_UNTIL's offset, which the picture does not have)
  double V[16], vp[16], iv[16], ivp[16];
  for (int i = 0; i < 16; ++i) V[i] = Vf[i];
  Mul(Vf, Pf, vp);
  if (!Invert(V, iv) || !Invert(vp, ivp)) {
    t.valid = false;
    ++t.skipped;
    TaaSay("the camera's matrices cannot be inverted");
    return false;
  }
  // The camera's place and forward direction (row vectors: the inverse view's rows 3 and 2).
  const double pos[3] = {iv[12], iv[13], iv[14]};
  double fwd[3] = {iv[8], iv[9], iv[10]};
  const double fl = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
  if (fl > 0)
    for (double& c : fwd) c /= fl;
  bool have = t.valid;
  if (have && t.last_frame + 1 != g_frame_no) {
    have = false;
    ++t.stale;
  }
  if (have) {
    // A camera cut (replays, takedown cameras, a restart): farther than 5 m or turned more than 15 degrees in a frame.
    const double dx = pos[0] - t.prev_pos[0], dy = pos[1] - t.prev_pos[1], dz = pos[2] - t.prev_pos[2];
    const double dot = fwd[0] * t.prev_fwd[0] + fwd[1] * t.prev_fwd[1] + fwd[2] * t.prev_fwd[2];
    if (dx * dx + dy * dy + dz * dz > 25.0 || dot < 0.9659) {
      have = false;
      ++t.cuts;
    }
  }
  double M[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  if (have) Mul(ivp, t.prev_vp, M);
  float c[28] = {1.0f / g_r.w, 1.0f / g_r.h, float(g_r.w), float(g_r.h)};
  for (int j = 0; j < 4; ++j)
    for (int i = 0; i < 4; ++i) c[4 + j * 4 + i] = float(M[i * 4 + j]);
  c[20] = jx;
  c[21] = jy;
  c[22] = 0.1f;  // the current picture's weight where the history is trusted
  c[23] = have ? 1.0f : 0.0f;
  c[24] = t.prev_jx;
  c[25] = t.prev_jy;

  const int cur = t.flip, last = cur ^ 1;
  IDirect3DSurface9 *hs = nullptr, *zs = nullptr;
  g_r.hist[cur]->GetSurfaceLevel(0, &hs);
  g_r.zlast[cur]->GetSurfaceLevel(0, &zs);
  bool ok = hs && zs;
  if (ok) {
    d->SetPixelShaderConstantF(0, c, 7);
    Sampler(d, 0, cur_tex, false);
    Sampler(d, 1, g_r.hist[last], true);
    Sampler(d, 5, depth, false);
    Sampler(d, 6, g_r.zlast[last], false);
    if (g_r.taa_mrt) {
      d->SetRenderState(D3DRS_COLORWRITEENABLE1, 0xF);
      d->SetRenderTarget(1, zs);  // this frame's depth, for the next frame's match (target 0 is set by Pass)
    }
    Pass(d, hs, g_r.quad_vs, g_r.taa_ps);
    if (g_r.taa_mrt) d->SetRenderTarget(1, nullptr);
    if (timed) GpuStamp(d, 1);
    Sampler(d, 1, nullptr, false);
    Sampler(d, 6, nullptr, false);
    if (!g_r.taa_mrt) Pass(d, zs, g_r.quad_vs, g_r.taa_depth_ps);  // this frame's depth, for the next frame's match
    Sampler(d, 5, nullptr, false);
  }
  if (ok) {
    int k = 0;
    const int s = TaaShotDue(&k);
    if (s >= 0) {
      // g_r.copy (the input) is not read again this frame once resolved: the variants are drawn into it.
      char name[64];
      IDirect3DSurface9* in = nullptr;
      if (SUCCEEDED(g_r.copy->GetSurfaceLevel(0, &in)) && in) {
        snprintf(name, sizeof name, "taa-%d-%d-in", s, k);
        ss_testshot_surface(d, in, name);
        static const float kSharp[3] = {0.0f, 0.25f, 0.5f};
        static const char* const kSuffix[3] = {"out", "sh25", "sh50"};
        for (int v = 0; v < 3; ++v) {
          if (v == 0) {
            TaaCopyOut(d, g_r.hist[cur], hs, in);
          } else {
            if (!g_r.cas_ps) break;
            const float texel[4] = {1.0f / g_r.w, 1.0f / g_r.h, kSharp[v], 0.0f};
            const float dith[4] = {DitherStep(in), 0.0f, 0.0f, 0.0f};
            d->SetPixelShaderConstantF(0, texel, 1);
            d->SetPixelShaderConstantF(1, dith, 1);
            Sampler(d, 0, g_r.hist[cur], true);
            Pass(d, in, g_r.quad_vs, g_r.cas_ps);
            Sampler(d, 0, nullptr, false);
          }
          snprintf(name, sizeof name, "taa-%d-%d-%s", s, k, kSuffix[v]);
          ss_testshot_surface(d, in, name);
        }
        in->Release();
      }
      ss_log("[postfx] TAA test shot %d/%d: jitter %.3f, %.3f px (%u uploads moved), history %s", s, k, jx * g_r.w * 0.5, -jy * g_r.h * 0.5, g_jit.patched, have ? "used" : "none");
    }
    if (out) ok = TaaCopyOut(d, g_r.hist[cur], hs, out);
  }
  SafeRelease(hs);
  SafeRelease(zs);
  if (!ok) {
    t.valid = false;
    ++t.skipped;
    return false;
  }
  memcpy(t.prev_vp, vp, sizeof vp);
  memcpy(t.prev_pos, pos, sizeof pos);
  memcpy(t.prev_fwd, fwd, sizeof fwd);
  t.prev_jx = jx;
  t.prev_jy = jy;
  t.valid = true;
  t.last_frame = g_frame_no;
  t.flip = last;
  *result = g_r.hist[cur];
  if (t.ok++ == 0) ss_log("[postfx] TAA: first frame resolved (%ux%u)", g_r.w, g_r.h);
  TaaSay(g_taa_jitter ? "resolving (the scene is jittered from the next frame on)" : "resolving without jitter (SS_TAA_JITTER=0)");
  return true;
}

// The main thread's time in our own work (frame_log's company for the [frames] split): the passes (the D3D calls
// of Apply; under DXVK the GPU's part runs later on its own thread) and TAA's jitter with the game's camera publish.
// Two QueryPerformanceCounter reads each a frame; logged with the 3600-frame report.
struct CpuPart {
  double sum = 0, mx = 0;
  uint32_t n = 0;
  void Add(double s) {
    sum += s;
    if (s > mx) mx = s;
    ++n;
  }
};
CpuPart g_cpu_passes, g_cpu_jitter;
double CpuNow() {
  static double per = 0;
  LARGE_INTEGER t;
  if (per == 0) {
    QueryPerformanceFrequency(&t);
    per = t.QuadPart ? 1.0 / double(t.QuadPart) : 1e-7;
  }
  QueryPerformanceCounter(&t);
  return double(t.QuadPart) * per;
}

bool ApplyPasses(IDirect3DDevice9* d, IDirect3DSurface9* f);
// The passes over the final target `f`, timed.
bool Apply(IDirect3DDevice9* d, IDirect3DSurface9* f) {
  const double t0 = CpuNow();
  TaaJitterEnd();
  ProbeEnd();
  ss_quality_scene_end(d);  // the scene is drawn: TAA's texture LOD bias out (the UI keeps the game's)
  const bool ok = ApplyPasses(d, f);
  if (ok) g_cpu_passes.Add(CpuNow() - t0);
  return ok;
}

// The passes over the final target `f`: copy it, then FXAA or SMAA and/or sharpening, the result back into `f`.
bool ApplyPasses(IDirect3DDevice9* d, IDirect3DSurface9* f) {
  TakeChange(d);
  if (g_aa == AA_OFF && g_sharpen <= 0.0f) return false;  // both turned off in the menu: the hooks pass the frame on
  D3DSURFACE_DESC desc;
  if (FAILED(f->GetDesc(&desc)) || desc.MultiSampleType != D3DMULTISAMPLE_NONE || !(desc.Usage & D3DUSAGE_RENDERTARGET)) return false;
  if (!EnsureShaders(d) || !EnsureTargets(d, desc.Width, desc.Height, desc.Format)) return false;
  if (!g_t0) g_t0 = GetTickCount();
  PredOffDue();

  IDirect3DSurface9 *copy = nullptr, *mid = nullptr, *edges = nullptr, *weights = nullptr;
  g_r.copy->GetSurfaceLevel(0, &copy);
  if (g_r.mid) g_r.mid->GetSurfaceLevel(0, &mid);
  if (g_r.edges) g_r.edges->GetSurfaceLevel(0, &edges);
  if (g_r.weights) g_r.weights->GetSurfaceLevel(0, &weights);
  bool ok = copy && (!g_r.mid || mid) && (!g_r.edges || edges) && (!g_r.weights || weights);

  // What a state block does not hold: the render targets and the depth surface.
  IDirect3DSurface9* rt[4] = {};
  IDirect3DSurface9* ds = nullptr;
  static DWORD nrt = 0;  // the device's caps do not change: read once (they were read on every frame)
  if (!nrt) {
    D3DCAPS9 caps;
    nrt = SUCCEEDED(d->GetDeviceCaps(&caps)) ? caps.NumSimultaneousRTs : 1;
    if (nrt > 4) nrt = 4;
    if (nrt < 1) nrt = 1;
  }
  bool captured = false;
  const bool timed = g_gpu && !g_gt.pending && !g_gt.none && (g_frame_no & 15) == 0;
  IDirect3DTexture9* depth = nullptr;  // SMAA's predication texture this frame
  ++ss_own_drawing;  // our sampler states are not the game's (quality.cpp's SetSamplerState wrapper passes them on)
  if (ok) {
    captured = SUCCEEDED(g_r.sb->Capture());
    ok = captured;
  }
  if (ok) {
    for (DWORD i = 0; i < nrt; ++i) d->GetRenderTarget(i, &rt[i]);  // NOTFOUND for an empty slot leaves nullptr
    d->GetDepthStencilSurface(&ds);

    if (IsTaa(g_aa)) {
      depth = DepthFor(ds, g_r.w, g_r.h);  // the reprojection's depth (taa_high: also SMAA's predication)
    } else if (g_aa == AA_SMAA && g_pred && g_r.smaa_pred_ps) {
      depth = DepthFor(ds, g_r.w, g_r.h);
      if (depth) SayPred(1, "colour with the game's depth as predication (smaa_predication)");
      else SayPred(0, "colour only: no readable depth (INTZ) of the picture's size bound this frame");
    } else if (g_aa == AA_SMAA) {
      SayPred(0, g_pred ? "colour only (the predicated pass is not there)" : "colour only (smaa_predication = false)");
    }
    if (timed) GpuStamp(d, 0);
    ok = SUCCEEDED(d->StretchRect(f, nullptr, copy, nullptr, D3DTEXF_NONE));
  }
  if (ok) {
    for (DWORD i = 1; i < nrt; ++i) d->SetRenderTarget(i, nullptr);
    d->SetDepthStencilSurface(nullptr);
    for (DWORD s = 0; s < 8; ++s) d->SetTexture(s, nullptr);
    d->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    d->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    d->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_FOGENABLE, FALSE);
    d->SetVertexDeclaration(g_r.decl);

    const float texel[4] = {1.0f / g_r.w, 1.0f / g_r.h, g_sharpen, 0.0f};
    const float metrics[4] = {1.0f / g_r.w, 1.0f / g_r.h, float(g_r.w), float(g_r.h)};
    IDirect3DSurface9* aa_out = g_sharpen > 0.0f ? mid : f;  // the AA result, sharpened after when asked
    bool aa_done = g_aa != AA_OFF;  // false: the AA wrote nothing (TAA could not resolve this frame)
    IDirect3DTexture9* aa_tex = nullptr;  // TAA: its result (the history), which the sharpening reads
    // SMAA 1x of g_r.copy into `to` (z: the depth for the predicated edge pass, or nullptr).
    auto smaa = [&](IDirect3DTexture9* z, IDirect3DSurface9* to, bool stamp) {
      d->SetVertexShaderConstantF(0, metrics, 1);
      d->SetPixelShaderConstantF(0, metrics, 1);
      // 1: luma edges (the shader discards where there are none, so the target is cleared first)
      d->SetRenderTarget(0, edges);
      d->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
      Sampler(d, 0, g_r.copy, true);
      if (z) Sampler(d, 5, z, false);
      Pass(d, edges, g_r.smaa_vs[0], z ? g_r.smaa_pred_ps : g_r.smaa_ps[0]);
      if (z) Sampler(d, 5, nullptr, false);
      if (stamp) GpuStamp(d, 1);
      // 2: blending weights (edges linear, area linear, search point)
      Sampler(d, 1, g_r.edges, true);
      Sampler(d, 2, g_r.area, true);
      Sampler(d, 3, g_r.search, false);
      Pass(d, weights, g_r.smaa_vs[1], g_r.smaa_ps[1]);
      // 3: neighbourhood blending
      Sampler(d, 1, nullptr, true);
      Sampler(d, 4, g_r.weights, true);
      Pass(d, to, g_r.smaa_vs[2], g_r.smaa_ps[2]);
      Sampler(d, 4, nullptr, true);
      Sampler(d, 2, nullptr, true);
      Sampler(d, 3, nullptr, false);
    };
    if (g_aa == AA_FXAA) {
      d->SetPixelShaderConstantF(0, texel, 1);
      Sampler(d, 0, g_r.copy, true);
      Pass(d, aa_out, g_r.quad_vs, g_r.fxaa_ps);
    } else if (g_aa == AA_SMAA) {
      // Test aid (SS_SMAA_AB_AT="150,200"): at those seconds, one frame is anti-aliased both ways and saved before the
      // UI: smaa-<s>-none (no AA), -colour and -depth (the result), -edges-colour and -edges-depth (the edge pass).
      const int ab = depth ? AbDue() : -1;
      if (ab >= 0) {
        char name[64];
        snprintf(name, sizeof name, "smaa-%d-none", ab);
        ss_testshot_surface(d, copy, name);
        smaa(nullptr, aa_out, timed);
        snprintf(name, sizeof name, "smaa-%d-colour", ab);
        ss_testshot_surface(d, aa_out, name);
        snprintf(name, sizeof name, "smaa-%d-edges-colour", ab);
        ss_testshot_surface(d, edges, name);
      }
      smaa(depth, aa_out, timed);
      if (ab >= 0) {
        char name[64];
        snprintf(name, sizeof name, "smaa-%d-depth", ab);
        ss_testshot_surface(d, aa_out, name);
        snprintf(name, sizeof name, "smaa-%d-edges-depth", ab);
        ss_testshot_surface(d, edges, name);
      }
    } else if (g_aa == AA_TAA) {
      aa_done = TaaResolve(d, g_r.copy, depth, g_sharpen > 0.0f ? nullptr : f, timed, &aa_tex);
    } else if (g_aa == AA_TAA_HIGH) {
      // SMAA 1x of the jittered picture into the intermediate, which the resolve then takes as the current picture.
      // (No depth: no resolve either; the picture is left as it is, as with taa.)
      if (depth) smaa(g_pred && g_r.smaa_pred_ps ? depth : nullptr, mid, false);
      aa_done = TaaResolve(d, g_r.mid, depth, g_sharpen > 0.0f ? nullptr : f, timed, &aa_tex);
    }
    if (g_sharpen > 0.0f) {
      d->SetPixelShaderConstantF(0, texel, 1);
      const float dith[4] = {DitherStep(f), 0.0f, 0.0f, 0.0f};
      d->SetPixelShaderConstantF(1, dith, 1);
      Sampler(d, 0, !aa_done ? g_r.copy : aa_tex ? aa_tex : g_r.mid, true);
      Pass(d, f, g_r.quad_vs, g_r.cas_ps);
    }
    if (timed) {
      if (g_aa != AA_SMAA && !(IsTaa(g_aa) && aa_done)) GpuStamp(d, 1);  // no edge pass: "copy + edges" reads as the copy and the passes
      if (GpuStamp(d, 2) && g_gt.q[0] && g_gt.q[1]) {
        g_gt.pending = true;
        g_gt.issued = g_frame_no;
        g_gt.pending_mode = g_aa * 2 + (depth ? 1 : 0);
      }
    }
  }
  if (captured) {
    // Back as it was: the targets first (setting target 0 resets the viewport), then every other state.
    if (rt[0]) d->SetRenderTarget(0, rt[0]);
    for (DWORD i = 1; i < nrt; ++i) d->SetRenderTarget(i, rt[i]);
    d->SetDepthStencilSurface(ds);
    g_r.sb->Apply();
  }
  --ss_own_drawing;
  for (auto& s : rt) SafeRelease(s);
  SafeRelease(ds);
  SafeRelease(copy);
  SafeRelease(mid);
  SafeRelease(edges);
  SafeRelease(weights);
  SafeRelease(depth);
  return ok;
}

// ----------------------------------------------------------------------------------------------------------- hooks

using BeginDisplayFn = void(__attribute__((thiscall)) *)(void*, uint32_t, const void*, uint32_t, uint32_t, uint32_t, uint32_t);
using EndSceneFn = HRESULT(__stdcall*)(IDirect3DDevice9*);
using ResetFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using SetDepthFn = HRESULT(__stdcall*)(IDirect3DDevice9*, IDirect3DSurface9*);
SetDepthFn g_set_depth = nullptr;
EndSceneFn g_end_scene = nullptr;
ResetFn g_reset = nullptr;
uint32_t* g_hooked_vt = nullptr;
bool g_gfx_hooked = false;

// Per frame (render thread only): a Scaleform display has begun / the passes have run.
bool g_frame_display = false, g_frame_done = false;

void Report() {
  if (++g_n.frames < 3600) return;
  ss_log("[postfx] last %u frames: %u under the UI, %u whole (no UI), %u left alone", g_n.frames, g_n.under_ui, g_n.whole, g_n.skipped);
  if (IsTaa(g_aa) || g_taa.ok)
    ss_log("[postfx] TAA: %u resolved, %u jittered so far; history dropped: %u camera cuts, %u after a frame not resolved; %u not resolved",
           g_taa.ok, g_taa.jitters, g_taa.cuts, g_taa.stale, g_taa.skipped);
  if (g_jit.frames) {
    ss_log("[postfx] TAA: %u frames drawn jittered, %.0f clip matrices moved a frame", g_jit.frames, double(g_jit.patched_sum) / g_jit.frames);
    g_jit.frames = 0;
    g_jit.patched_sum = 0;
  }
  if (g_cpu_passes.n) {
    char j[96] = "";
    if (g_cpu_jitter.n)
      snprintf(j, sizeof j, "; TAA's jitter choice %.3f / %.3f (%u frames)", g_cpu_jitter.sum / g_cpu_jitter.n * 1000,
               g_cpu_jitter.mx * 1000, g_cpu_jitter.n);
    ss_log("[postfx] main thread (avg / max ms): the passes (aa %s, sharpen %.2f) %.3f / %.3f (%u frames)%s", AaName(g_aa), g_sharpen,
           g_cpu_passes.sum / g_cpu_passes.n * 1000, g_cpu_passes.mx * 1000, g_cpu_passes.n, j);
    g_cpu_passes = CpuPart();
    g_cpu_jitter = CpuPart();
  }
  GpuFlush("of the last 3600 frames");
  g_n.frames = g_n.under_ui = g_n.whole = g_n.skipped = 0;
}

void __attribute__((thiscall, force_align_arg_pointer)) BeginDisplayHook(void* self, uint32_t bg, const void* vp, uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1) {
  if (!g_frame_done) {
    g_frame_display = true;
    IDirect3DDevice9* d = GameDevice();
    IDirect3DSurface9* f = FinalSurface();
    IDirect3DSurface9* rt = nullptr;
    if (d && f && SUCCEEDED(d->GetRenderTarget(0, &rt)) && rt) {
      if (rt == f) {
        g_frame_done = true;
        if (Apply(d, f)) {
          ++g_n.under_ui;
          if (!g_n.said_under_ui) ss_log("[postfx] first frame filtered under the UI (at the first Scaleform display on the final target)");
          g_n.said_under_ui = true;
        }
        // HDR's scene pass (hdr.cpp): after the anti-aliasing and the sharpening, so the luma it keeps of the graded
        // picture is the one the UI is then drawn over. The extra light itself is not anti-aliased.
        if (g_hdr) ss_hdr_scene(d, f);
      }
      rt->Release();
    }
  }
  reinterpret_cast<BeginDisplayFn>(kBeginDisplay)(self, bg, vp, x0, x1, y0, y1);
}

HRESULT __attribute__((stdcall, force_align_arg_pointer)) EndSceneHook(IDirect3DDevice9* d) {
  // The settings overlay's own BeginScene / EndScene (menu.cpp, from Present): not the game's frame end.
  if (ss_own_drawing == 0 && d == GameDevice()) {
    if (!g_frame_done && !g_frame_display) {
      IDirect3DSurface9* f = FinalSurface();
      if (f && Apply(d, f)) {
        ++g_n.whole;
        if (!g_n.said_whole) ss_log("[postfx] first frame with no UI filtered whole (at EndScene)");
        g_n.said_whole = true;
      }
    } else if (!g_frame_done) {
      ++g_n.skipped;
    }
    TaaJitterEnd();
    ss_quality_scene_end(d);  // a frame with no split: TAA's jitter and texture LOD bias end with it
    // HDR (hdr.cpp): the scene pass when the frame had no split, then the mask pass on the finished frame.
    if (g_hdr) ss_hdr_frame_end(d, FinalSurface());
    g_frame_done = g_frame_display = false;
    ++g_frame_no;
    if (g_gpu) GpuRead(d);
    Report();
  }
  return g_end_scene(d);
}

HRESULT __attribute__((stdcall, force_align_arg_pointer)) ResetHook(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* pp) {
  if (d == g_r.dev) ReleaseDefaultPool();  // default-pool objects and state blocks must be gone before a Reset
  ForgetDepth();
  if (g_hdr) ss_hdr_before_reset(d);
  return g_reset(d, pp);
}

HRESULT __attribute__((stdcall, force_align_arg_pointer)) SetDepthHook(IDirect3DDevice9* d, IDirect3DSurface9* s) {
  if (ss_own_drawing == 0) NoteDepth(s);
  return g_set_depth(d, s);
}

bool WriteSlot(uint32_t* slot, uint32_t value) {
  return ss_write_word(slot, value);
}

// The device's EndScene and Reset (in d3d9.dll's memory). Returns true once hooked.
bool HookDevice() {
  IDirect3DDevice9* d = GameDevice();
  if (!d || !Readable(reinterpret_cast<uintptr_t>(d), 4)) return false;
  uint32_t* vt = *reinterpret_cast<uint32_t**>(d);
  if (vt == g_hooked_vt) return true;
  if (!vt || !Readable(reinterpret_cast<uintptr_t>(vt), (kEndSceneSlot + 1) * 4)) return false;
  HMODULE owner = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<const char*>(vt[kEndSceneSlot]), &owner) ||
      !owner || owner == GetModuleHandleA(nullptr)) {
    static bool said = false;
    if (!said) ss_log("[postfx] the device's EndScene is not in a d3d9 module; anti-aliasing and sharpening stay off");
    said = true;
    return false;
  }
  const uint32_t es = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&EndSceneHook));
  const uint32_t rs = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&ResetHook));
  const uint32_t ds = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&SetDepthHook));
  // The originals are taken once per vtable, before a slot points at us. A retry after a failed write must not take
  // them again: menu.cpp wraps Reset too and may by then sit over ours, and taking its word as our original would
  // make a loop (postfx -> menu -> postfx ...). A slot is written only while it still holds the word we took.
  static uint32_t* captured_vt = nullptr;
  if (vt != captured_vt) {
    if (vt[kEndSceneSlot] != es) g_end_scene = reinterpret_cast<EndSceneFn>(vt[kEndSceneSlot]);
    if (vt[kResetSlot] != rs) g_reset = reinterpret_cast<ResetFn>(vt[kResetSlot]);
    if (vt[kSetDepthSlot] != ds) g_set_depth = reinterpret_cast<SetDepthFn>(vt[kSetDepthSlot]);
    captured_vt = vt;
  }
  auto put = [](uint32_t* slot, uint32_t mine, uint32_t orig) {
    if (*slot == mine) return true;
    if (*slot != orig) {
      SetLastError(ERROR_INVALID_DATA);  // another hook went over the original since: chaining through it would loop
      return false;
    }
    return WriteSlot(slot, mine);
  };
  if (!put(&vt[kResetSlot], rs, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_reset))) ||
      !put(&vt[kEndSceneSlot], es, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_end_scene)))) {
    static int said = 0;
    if (said++ < 3) ss_log("[postfx] cannot write the device vtable (error %lu)", GetLastError());
    return false;
  }
  // SetDepthStencilSurface, for SMAA's predication only: when it cannot be written, SMAA still finds the depth when the
  // game has it bound at the split, else it finds edges from the colour only.
  if (!put(&vt[kSetDepthSlot], ds, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_set_depth))))
    ss_log("[postfx] cannot write SetDepthStencilSurface's slot (error %lu); SMAA's predication only sees the depth bound at the split", GetLastError());
  // SetVertexShaderConstantF, for TAA's jitter (passes everything on while no frame is being jittered): without it
  // TAA resolves without jitter.
  if (Readable(reinterpret_cast<uintptr_t>(vt), (kSetVsConstSlot + 1) * 4)) {
    const uint32_t vc = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&SetVsConstHook));
    static uint32_t* vs_vt = nullptr;
    if (vt != vs_vt && vt[kSetVsConstSlot] != vc) {
      g_set_vs_const = reinterpret_cast<SetVsConstFn>(vt[kSetVsConstSlot]);
      vs_vt = vt;
    }
    if (!put(&vt[kSetVsConstSlot], vc, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_set_vs_const)))) {
      g_set_vs_const = nullptr;
      ss_log("[postfx] cannot write SetVertexShaderConstantF's slot (error %lu); TAA resolves without jitter", GetLastError());
    }
  }
  if (g_hooked_vt) ss_log("[postfx] the device's vtable changed (%p -> %p): hooked again", g_hooked_vt, vt);
  g_hooked_vt = vt;
  char name[MAX_PATH] = "?";
  GetModuleFileNameA(owner, name, sizeof name);
  ss_log("[postfx] EndScene, Reset and SetDepthStencilSurface hooked in %s (device %p)", name, d);
  return true;
}

// The Scaleform renderer's BeginDisplay slot (read-only data of the game, one aligned word).
bool HookGfx() {
  if (g_gfx_hooked) return true;
  const uint32_t mine = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&BeginDisplayHook));
  if (!Readable(kGfxRendererVt, 27 * 4)) return false;
  const uint32_t cur = Rd(kBeginDisplaySlot);
  if ((cur != kBeginDisplay && cur != mine) || Rd(kSubmitMaskSlot) != kSubmitMask) {
    ss_log("[postfx] the Scaleform renderer's vtable is not the known build (slot 5 = %08x); the passes run on the whole frame at EndScene", cur);
    g_gfx_hooked = true;  // not retried; EndScene still filters the frames (with the UI on them)
    return true;
  }
  if (cur != mine && !WriteSlot(reinterpret_cast<uint32_t*>(kBeginDisplaySlot), mine)) {
    ss_log("[postfx] cannot write the Scaleform renderer's slot (error %lu); the passes run on the whole frame at EndScene", GetLastError());
    g_gfx_hooked = true;
    return true;
  }
  g_gfx_hooked = true;
  ss_log("[postfx] Scaleform BeginDisplay slot %p wrapped: the passes run before the UI is drawn", reinterpret_cast<void*>(kBeginDisplaySlot));
  return true;
}

// Test aid SS_AA_SWITCH = "30=taa_high,45=taa,60=off": at those seconds after start the aa row is changed as the
// settings menu changes it (no menu input on a headless test run).
struct Switch {
  DWORD at_ms;
  std::string aa;
};
std::vector<Switch> g_switches;
DWORD g_start_ms = 0;

void SwitchesParse() {
  const char* e = getenv("SS_AA_SWITCH");
  if (!e) return;
  std::string v = e;
  size_t p = 0;
  while (p < v.size()) {
    size_t q = v.find(',', p);
    if (q == std::string::npos) q = v.size();
    const std::string one = v.substr(p, q - p);
    const size_t eq = one.find('=');
    if (eq != std::string::npos) g_switches.push_back({static_cast<DWORD>(atof(one.substr(0, eq).c_str()) * 1000.0), one.substr(eq + 1)});
    p = q + 1;
  }
  ss_log("[postfx] test aid SS_AA_SWITCH: %zu changes of the aa row", g_switches.size());
}

void SwitchesDue() {
  const DWORD now = GetTickCount() - g_start_ms;
  for (Switch& w : g_switches) {
    if (w.at_ms == ~0u || now < w.at_ms) continue;
    ss_log("[postfx] test aid SS_AA_SWITCH: aa = %s at %.1f s (frame %u)%s", w.aa.c_str(), now / 1000.0, g_frame_no,
           ss_postfx_apply("aa", w.aa.c_str()) ? "" : ": the passes' hooks are not in");
    w.at_ms = ~0u;
  }
}

DWORD WINAPI Thread(LPVOID) {
  // The device is made after the protector has unpacked the game; the slots are only looked at once it exists.
  for (;;) {
    Sleep(250);
    if (!g_gfx_hooked && GameDevice()) HookGfx();
    HookDevice();  // again if the game ever makes a device with another vtable
    if (!g_switches.empty()) SwitchesDue();
  }
  return 0;
}

std::string Setting(const char* env, const char* key) {
  const char* e = getenv(env);
  return e ? std::string(e) : ss_conf(key);
}

}  // namespace

void ss_postfx_start() {
  if (!ss_conf_present()) return;
  const std::string aa = Setting("SS_AA", "aa");
  g_aa = aa.empty() ? AA_SMAA : ParseAa(aa);  // no key: SMAA (release default)
  if (!aa.empty() && aa != "off" && g_aa == AA_OFF) ss_log("[postfx] aa = %s is not off, fxaa, smaa or taa: off", aa.c_str());
  const std::string sh = Setting("SS_SHARPEN", "sharpen");
  if (!sh.empty()) g_sharpen = ParseSharpen(sh);
  g_want_aa = g_aa;
  g_want_sharpen = g_sharpen;
  const std::string pr = Setting("SS_SMAA_PREDICATION", "smaa_predication");
  g_pred = !(pr == "false" || pr == "0" || pr == "off" || pr == "no");
  if (const char* j = getenv("SS_TAA_JITTER")) g_taa_jitter = *j != '0';
  const std::string gpu = Setting("SS_HDR_GPU", "hdr_gpu");
  g_gpu = gpu == "1" || gpu == "true" || gpu == "on" || gpu == "yes";
  g_hdr = ss_hdr_wanted();
  if (g_aa == AA_OFF && g_sharpen <= 0.0f && !g_hdr) return;  // off: nothing is hooked
  if (GetModuleHandleA(nullptr) != reinterpret_cast<HMODULE>(0x400000)) {
    ss_log("[postfx] unexpected image base; anti-aliasing and sharpening stay off");
    return;
  }
  g_start_ms = GetTickCount();
  SwitchesParse();
  ss_log("[postfx] aa %s, sharpen %.2f, smaa_predication %s%s%s", AaName(g_aa), g_sharpen,
         g_pred ? "true" : "false", g_hdr ? "; the frame hooks also carry the HDR passes" : "", g_gpu ? "; the passes are timed (hdr_gpu)" : "");
  CreateThread(nullptr, 0, Thread, nullptr, 0, nullptr);
}

// hdr.cpp, while its passes run: the scene is complete and the first UI draw onto the final target is about to
// happen (the car's HUD cards, which the game draws before the first Scaleform display). The passes run here, then
// HDR's scene pass; the BeginDisplay wrap then finds the frame done.
void ss_postfx_split(void* device, void* final_surface) {
  if (g_frame_done || !g_hooked_vt) return;
  IDirect3DDevice9* d = static_cast<IDirect3DDevice9*>(device);
  IDirect3DSurface9* f = static_cast<IDirect3DSurface9*>(final_surface);
  if (d != GameDevice() || !f || f != FinalSurface()) return;
  g_frame_done = true;
  if (Apply(d, f)) {
    ++g_n.under_ui;
    if (!g_n.said_under_ui) ss_log("[postfx] first frame filtered under the UI (at the first draw onto the final target after the colour grade)");
    g_n.said_under_ui = true;
  }
  ss_hdr_scene(d, f);
}

// patch.cpp's frame step, right after the game's update (and aspect.cpp's camera fix), before the render: TAA's jitter.
void ss_postfx_after_update() {
  if (!g_hooked_vt) return;
  if (!IsTaa(g_aa) && !g_taa.jittered && !g_jit.on) return;  // no offset to choose or to end
  const double t0 = CpuNow();
  TaaJitter();
  ProbeArm();
  ss_quality_scene_begin(GameDevice(), g_jit.on);  // TAA's texture LOD bias while the scene is jittered
  g_cpu_jitter.Add(CpuNow() - t0);
}

// The settings menu (menu.cpp): aa and sharpen act at once, but only while the passes' hooks are in (they go in at
// start when either key was on, never later); otherwise false, and the row says RESTART NEEDED.
bool ss_postfx_apply(const char* key, const char* value) {
  const bool is_aa = !strcmp(key, "aa");
  const bool is_pred = !strcmp(key, "smaa_predication");
  if (!is_aa && !is_pred && strcmp(key, "sharpen")) return false;
  if (!*static_cast<uint32_t* volatile*>(&g_hooked_vt)) return false;
  const std::string v = value ? value : "";
  if (is_pred) {  // only which edge pass runs: taken at the next frame, nothing is made again
    InterlockedExchange(&g_pred, v == "false" ? 0 : 1);
    return true;
  }
  if (is_aa) g_want_aa = ParseAa(v);
  else g_want_sharpen = ParseSharpen(v);
  InterlockedExchange(&g_want_changed, 1);
  return true;
}

// shadowsmooth.cpp (render thread, while the scene is drawn): the frame is drawn jittered for TAA's resolve; the number
// of jittered frames so far (the shadow taps' rotation follows it), else -1.
int ss_postfx_taa_frame() { return g_jit.on ? static_cast<int>(g_taa.seq & 0x7FFFFFFF) : -1; }
