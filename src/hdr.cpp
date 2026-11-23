// HDR output (docs/plan-hdr.md): the light the game's 8-bit tone-mapped picture throws away, worked out again from
// the tone mapper's own inputs and carried past the 8-bit chain to an HDR10 display. The port's method and math
// (its hdr_scene / hdr_mask / hdr_out shaders, Shader Model 3 here), without its LIGHT SOURCES re-draws.
//
// Keys (the port's rows, same names, values and defaults; switches SS_HDR, SS_HDR_PEAK, ...):
//   hdr = false | true            the HDR output at start; the menu's HDR row switches it live either way
//   hdr_ready = true | false      true (default): the hooks are in and the back buffer is 10-bit from the start, so
//                                 HDR can be switched on and off live; with HDR off the passes do not run and the
//                                 picture is the SDR one. false, with hdr = false: this file does nothing at all (no
//                                 hook, no device call, the game's own back buffer) and the HDR row says RESTART NEEDED.
//   hdr_peak = 0                  nits; 0 = the display's (what DXVK says of it, else 1000)
//   hdr_paper_white = 240         nits of the game's white (200 on a handheld: SteamDeck=1 in the environment)
//   hdr_hud = 0                   nits of the HUD's and menus' white; 0 = AUTO, 0.8 x paper white
//   hdr_shadows = 1               power below mid grey (1 = off; 1.05, 1.1, 1.2)
//   hdr_highlights = 1.5          gain of the light past paper white
//   hdr_lift = 3                  the port's LIGHT SOURCES. Its re-draws (additive sprites, lamps) are not built here;
//                                 the value only sets the particle tag's strength (max(3, hdr_lift), as the port's).
//
// The frame, with hdr on and an HDR10 swap chain (everything of the game's own chain stays as it is):
//   A. DrawIndexedPrimitive (device vtable slot 82): at the tone mapper's draw (a pixel shader whose constant table
//      names SSFilmicToneMapper_Consts; classed when the game creates it, CreatePixelShader slot 106) the textures on
//      its samplers (lit scene, adaptation, bloom) and its constants are noted; at the particle composite's draw the
//      particle texture and the factor it is added with.
//   B. The scene pass, at the scene/UI split, after postfx.cpp's FXAA / SMAA / sharpening: the extra light over the
//      SDR picture per pixel, into a float target of the final target's size; alpha = the graded pixel's luma code.
//      The split, while the passes run: right after the colour grade's draw onto the final target (a shader
//      naming color_cube_* / ss_gradient_and_vignette_*), between two of the game's draws; else the first draw onto
//      it after that by a shader that is not the chain's own (run inside that draw, the powerplay meter's card was lost). That is earlier
//      than postfx.cpp's own split (the first Scaleform BeginDisplay on the final target): the car's HUD cards are
//      Scaleform textures the game draws onto the picture itself before that, and they must not count as scene.
//      A frame with no grade draw: postfx.cpp's split; a frame with no UI: at EndScene.
//   C. The mask pass, at EndScene: the extra kept where the finished frame still shows the graded scene; alpha = the
//      share of the pixel's light the HUD added (negative: a dark cover).
//   D. The output pass, in the Present hook after the settings overlay: back buffer (the SDR frame, UI and overlay on
//      it) + extra -> shadows, highlight gain, shoulder to the peak, paper white / HUD white, BT.2020, PQ.
// The swap chain: display.cpp creates and resets the device with an A2R10G10B10 back buffer (falling back to the
// game's format when that is refused); DXVK's ID3D9VkExtSwapchain switches it to HDR10 ST 2084 when the surface
// offers it. Not offered (an SDR display, Game Mode with HDR off, not DXVK): the 10-bit back buffer goes out as sRGB
// and no pass runs: the SDR picture, silently.
// The HDR row off (hdr_ready, HDR not switched on): the swap chain stays (or goes back to) sRGB, no pass runs and
// no draw is made again; the game's 8-bit frame reaches the 10-bit back buffer by its own StretchRect, the same codes.
//
// Levers: five more slots of the device's vtable in the d3d9 module's memory (the draw calls 81..84 and 106), calls
// on the game's device;
// the frame hooks are postfx.cpp's and display.cpp's. Nothing of the game is written.
//
// Test aids: SS_HDR_FORCE=1 (hdr_force) runs the passes and the PQ encoding without an HDR swap chain (the screen then
// shows PQ codes; for read-backs on a machine without HDR output); SS_HDR_READBACK="60,120" logs what the back buffer
// holds after the output pass at those seconds; SS_HDR_GPU=1 times the passes with timestamp queries;
// SS_HDR_PARTICLES=<0..4> and SS_HDR_LOG=<0..2> are the port's diagnostic strengths (particle tag 3, logarithmic
// extension 0.5); SS_HDR_CALIBRATE=<second> shows the calibration patterns from that second on;
// SS_HDR_TEST_TOGGLE="120,170" switches the HDR row at those seconds, as the menu does (on, off, ...); with HDR off a
// read-back second (SS_HDR_READBACK) compares the 10-bit back buffer with the game's 8-bit final target code for code;
// SS_HDR_TEST_RESET="70,160" makes the game reset its device at those seconds (one Present answers D3DERR_DEVICELOST
// and TestCooperativeLevel, slot 3, D3DERR_DEVICENOTRESET until the game has called Reset: its own lost-device path).
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "menu.h"
#include "ss_shared.h"
#include "shadercache.h"

// COM interfaces declared outside the anonymous namespace on purpose (see postfx.cpp: inside it GCC turns the calls
// into pure-virtual calls).
struct IHdrCode {  // ID3DXBuffer
  virtual HRESULT __stdcall QueryInterface(REFIID, void**) = 0;
  virtual ULONG __stdcall AddRef() = 0;
  virtual ULONG __stdcall Release() = 0;
  virtual void* __stdcall GetBufferPointer() = 0;
  virtual DWORD __stdcall GetBufferSize() = 0;
};
struct HdrMetadata {  // VkHdrMetadataEXT
  uint32_t sType;     // VK_STRUCTURE_TYPE_HDR_METADATA_EXT = 1000105000
  const void* pNext;
  float red[2], green[2], blue[2], white[2];
  float maxLuminance, minLuminance, maxContentLightLevel, maxFrameAverageLightLevel;
};
struct HdrOutputDesc {  // D3D9VkExtOutputMetadata
  float red[2], green[2], blue[2], white[2];
  float minLuminance, maxLuminance, maxFullFrameLuminance;
};
struct IHdrSwapchain : IUnknown {  // DXVK's ID3D9VkExtSwapchain (d3d9_interfaces.h), same order since v2.3
  virtual BOOL __stdcall CheckColorSpaceSupport(uint32_t colour_space) = 0;
  virtual HRESULT __stdcall SetColorSpace(uint32_t colour_space) = 0;
  virtual HRESULT __stdcall SetHDRMetaData(const HdrMetadata* metadata) = 0;
  virtual HRESULT __stdcall GetCurrentOutputDesc(HdrOutputDesc* desc) = 0;
  virtual void __stdcall UnlockAdditionalFormats() = 0;
};

namespace {

const GUID kIidVkExtSwapchain = {0x13776e93, 0x4aa9, 0x430a, {0xa4, 0xec, 0xfe, 0x9e, 0x28, 0x11, 0x81, 0xd5}};
const GUID kIidTexture9 = {0x85c31227, 0x3de5, 0x4f00, {0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5}};
constexpr uint32_t kCsSrgb = 0, kCsHdr10 = 1000104008;  // VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, _HDR10_ST2084_EXT

constexpr uintptr_t kRenderer = 0xD64708;  // renderer holder; +0x578 the device
constexpr int kDrawSlot = 81, kDrawIndexedSlot = 82, kDrawUpSlot = 83, kDrawIndexedUpSlot = 84, kCreatePixelShaderSlot = 106;

// ------------------------------------------------------------------------------------------------------ settings

bool g_want = false;         // hdr or hdr_ready at start: the hooks are in and the back buffer is asked for in 10 bits
volatile LONG g_on = 0;      // the HDR row (live): the output is HDR10 where the display offers it
bool g_force = false;        // SS_HDR_FORCE: the passes and the encoding without an HDR swap chain
volatile float g_peak = 0.0f, g_paper = 240.0f, g_hud = 0.0f, g_shadows = 1.0f, g_highlights = 1.5f, g_lift = 3.0f;
float g_particles = 3.0f;    // SS_HDR_PARTICLES
// The LIGHT SOURCES TEST row (hdr_lift_test, live; a diagnostic for SSPC-4): 0 both paths, 1 the lamps only (no
// additive re-draw), 2 the additive draws only (no lamp re-draw), 3 both re-drawn but not lifted (what the re-draws
// themselves do to the picture), 4 the old corona rule (lamp x (1 - add_tag) added), 5 the lamps without the depth
// agreement test.
// 5 since the stencil tag: the lamps by the old way (every self-lit draw made again into a mask, kept where its
// depth is the G-buffer's), to compare with.
volatile LONG g_test = 0;
// hdr_light_log = 1 (SS_HDR_LIGHT_LOG; a diagnostic for SSPC-4, off by default): ten times a second the log gets what
// the light sources did in a window in the middle of the picture (the middle 60 % each way, 64 x 64 samples of the
// scene pass's own terms) and which self-lit and additive draws the frame made; see ProbeWindow.
bool g_probe = false;
// hdr_light_log = 2: as 1, and which G-buffer draw covers a lamp that fails the depth agreement: every G-buffer draw
// leaves its pixel shader's number in the stencil (the tag's way), the numbers go into the lamps' target, and the
// window's failing lamp samples are counted by the shader that is in front of them. The lamps then run the old way.
bool g_probe_cover = false;
float g_log = 0.5f;          // SS_HDR_LOG: paper whites per stop past two stops over the curve's white point
constexpr float kLogStart = 4.0f, kSoftness = 0.35f;
bool g_gpu_timing = false;   // SS_HDR_GPU
int g_cal_at = -1;           // SS_HDR_CALIBRATE
std::vector<int> g_readback_at;
std::vector<int> g_test_reset_at;  // SS_HDR_TEST_RESET
std::vector<int> g_test_toggle_at;  // SS_HDR_TEST_TOGGLE
struct TestSet {  // SS_HDR_TEST_SET="120:hdr_lift_test=5,134:hdr_lift=0": a row's value at a second, as the menu sets it
  int at;
  std::string key, value;
};
std::vector<TestSet> g_test_set;
size_t g_test_set_next = 0;
size_t g_test_toggle_next = 0;
size_t g_test_reset_next = 0;
volatile LONG g_test_lost = 0;     // the game is being told its device is lost, until it has reset it
volatile LONG g_calibrating = 0;  // the calibration screen is up (menu.cpp)

std::string Setting(const char* env, const char* key) {
  const char* e = getenv(env);
  return e ? std::string(e) : ss_conf(key);
}
bool IsOn(const std::string& v) { return v == "1" || v == "true" || v == "yes" || v == "on"; }
bool Handheld() {
  const char* e = getenv("SteamDeck");
  return e && !strcmp(e, "1");
}
float Clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

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
// The renderer whose device word was found readable since the last refresh (0: check again). GameDevice runs on
// every draw of the G-buffer pass, and a VirtualQuery there was a system call per draw (SSPC-17, docs/perf-plan.md
// A1). The renderer is the game's long-lived singleton; the check is made again once a frame (Present), at a device
// creation and around a Reset, and whenever the renderer pointer changes. The device word itself is still read live
// on every call, so the answer is the same as before.
volatile uintptr_t g_dev_renderer = 0;
void ForgetGameDevice() { g_dev_renderer = 0; }
IDirect3DDevice9* GameDevice() {
  static const bool exe = GetModuleHandleA(nullptr) == reinterpret_cast<HMODULE>(0x400000);
  if (!exe) return nullptr;
  const uintptr_t r = Rd(kRenderer);
  if (!r) return nullptr;
  if (r != g_dev_renderer) {
    if (!Readable(r + 0x578, 4)) return nullptr;
    g_dev_renderer = r;
  }
  return reinterpret_cast<IDirect3DDevice9*>(Rd(r + 0x578));
}
// The surface the frame end (0x50E400) copies to the back buffer: renderer -> frame buffer set -> target -> surface
// (as postfx.cpp reads it). Compared only.
IDirect3DSurface9* FinalSurface() {
  const uintptr_t r = Rd(kRenderer);
  if (!r || !Readable(r + 4, 4)) return nullptr;
  const uintptr_t set = Rd(r + 4);
  if (!set || !Readable(set + 0x64, 4)) return nullptr;
  const uintptr_t target = Rd(set + 0x64);
  if (!target || !Readable(target + 0x5C, 4)) return nullptr;
  return reinterpret_cast<IDirect3DSurface9*>(Rd(target + 0x5C));
}

template <typename T>
void SafeRelease(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

// --------------------------------------------------------------------------------------------------------- shaders

const char kQuadVs[] = R"(
void main(float4 p : POSITION, float2 t : TEXCOORD0, out float4 op : POSITION, out float2 ot : TEXCOORD0) { op = p; ot = t; }
)";

// The scene pass (the port's hdr_scene.frag). Samplers: 0 the graded picture (the final target, 8-bit, sRGB encoded),
// 1 the lit scene (the tone mapper's input, float), 2 the adaptation (R32F, read at its centre), 3 the bloom (float),
// 4 the soft particles' light (float). The tone mapper, as the game runs it:
//   k = (E * c100.x - 1) * c100.y + 1, C = k * scene, L = luminance(C), f(L) = white * L^n / (L^n + w),
//   B = saturate(bloom ^ power), out = C * f / L * (1 - B) + B, clipped to 1 per channel by its 8-bit target.
// Out: rgb the extra light, display-linear, 1.0 = SDR white; a the graded pixel's luma code.
const char kScenePs[] = R"(
sampler2D graded : register(s0);
sampler2D scene : register(s1);
sampler2D adapt : register(s2);
sampler2D bloom : register(s3);
sampler2D particles : register(s4);
sampler2D additive : register(s5);
sampler2D lampmask : register(s6);    // the lamps' albedo (the G-buffer's own code: its square root; 0: nothing drawn)
sampler2D lampdepth : register(s7);   // the nearest lamp draw's depth
sampler2D scenedepth : register(s8);  // the game's G-buffer depth
sampler2D cover : register(s10);      // hdr_light_log = 2: a = the number of the pixel shader that drew the G-buffer's surface
sampler2D lampemit : register(s9);    // the lamps' own light per pixel, x / (x + 1), x = albedo x amount / reference amount
float4 c100 : register(c0);  // exposure scale, exposure mix, curve power n, curve w
float4 k1 : register(c1);    // the curve's white, bloom power, the particle tag's strength, the log extension's strength
float4 k3 : register(c3);    // x: the additive light sources' lift (0: none drawn this frame, or LIGHT SOURCES off),
                             // y: the lamps' lift in paper whites (0: none), z: 1 = the curve is SSToneMapper's
float4 k4 : register(c4);    // x: 1 = the old corona rule, y: 1 = the lamps by the re-draw (mask, light, depths), 0 = by
                             // the stencil tag (lampmask: rgb the albedo code, a the weight; no other lamp sampler),
                             // z: what to write instead of the extra light (hdr_light_log): 1, 2, 3 = p1, p2, p3
float4 k2 : register(c2);    // the particles' share of light (0: none), where the log extension starts, 1 = adaptation read,
                             // 1 = a tone map drew here (0: only the luma is written)
float4 k5 : register(c5);    // the lamps' distance fade: x, y the camera's depth terms (depth = x + y / distance), z the
                             // distance the fade starts at, w where the lift is gone (w = 0: no fade, the camera is not known)
static const float3 kLuma = float3(0.2125, 0.7154, 0.0721);
float3 Linear(float3 c) {  // the inverse of the sRGB write the game's tone mapper draws with
  return float3(c.r <= 0.04045 ? c.r / 12.92 : pow((c.r + 0.055) / 1.055, 2.4),
                c.g <= 0.04045 ? c.g / 12.92 : pow((c.g + 0.055) / 1.055, 2.4),
                c.b <= 0.04045 ? c.b / 12.92 : pow((c.b + 0.055) / 1.055, 2.4));
}
float4 main(float2 uv : TEXCOORD0) : COLOR {
  float4 g = tex2Dlod(graded, float4(uv, 0, 0));
  float yg = dot(g.rgb, float3(0.299, 0.587, 0.114));
  float3 s = max(tex2Dlod(scene, float4(uv, 0, 0)).rgb, 0.0);
  float e = k2.z > 0.5 ? tex2Dlod(adapt, float4(0.5, 0.5, 0, 0)).r : 1.0;
  float k = (e * c100.x - 1.0) * c100.y + 1.0;
  float3 c = k * s;
  float L = dot(c, kLuma);
  float3 bl = tex2Dlod(bloom, float4(uv, 0, 0)).rgb;
  float3 bp = saturate(exp2(log2(max(abs(bl), 1e-20)) * k1.y));
  float3 lp3 = tex2Dlod(particles, float4(uv, 0, 0)).rgb;
  float3 ad3 = max(tex2Dlod(additive, float4(uv, 0, 0)).rgb, 0.0);
  float4 lm4 = tex2Dlod(lampmask, float4(uv, 0, 0));
  float3 lm = lm4.rgb;
  float3 le = tex2Dlod(lampemit, float4(uv, 0, 0)).rgb;
  float zl = tex2Dlod(lampdepth, float4(uv, 0, 0)).r;
  float zs = tex2Dlod(scenedepth, float4(uv, 0, 0)).r;
  float3 extra = 0.0;
  // hdr_light_log: p1 = the lamp's share, its gate, the additive tag, the particle tag; p2 = 1 where a lamp was
  // drawn, the depths' difference (lamp - scene) over what is allowed, the mask's alpha (the draw's id), the lamp's albedo luma;
  // p3 = L over the white point, the lamp's lift, the sourced lift, the additive target's exposed luma.
  float4 p1 = 0.0, p2 = 0.0, p3 = 0.0;
  p2.x = k4.y > 0.5 ? (max(le.r, max(le.g, le.b)) > 0.002 ? 1.0 : 0.0) : (lm4.a > 0.004 ? 1.0 : 0.0);
  p2.y = k4.y > 0.5 ? clamp((zl - zs) / (1e-6 + 2e-5 * max(zl, zs)), -100.0, 100.0) : 0.0;
  p2.z = lm4.a;
  if (k4.y < 0.5) p2.y = -1.0;  // the stencil tag: no lift worked out here (the lamps' follow below, when there is one)
  p3.w = dot(k * ad3, kLuma);
  if (L > 1e-6 && k2.w > 0.5) {
    float n = c100.z, w = c100.w, white = k1.x;
    float ln = pow(L, n);
    float f = white * ln / (ln + w);
    // Where the curve reaches white: L1^n = w / (white - 1).
    float reaches = white > 1.0 ? 1.0 : 0.0;
    float L1 = white > 1.0 ? pow(w / (white - 1.0), 1.0 / n) : 1.0;
    float top = 1e6;  // how far the curve's own term may go
    if (k3.z > 0.5) {
      // The other tone mapper (SSToneMapper): f = L (1 + L / z) / (1 + L), z = c100.z; it reaches white at
      // L = sqrt(z) and grows without bound past it (L / z), so its own term is held to one white.
      float z = max(c100.z, 1e-4);
      f = L * (1.0 + L / z) / (1.0 + L);
      reaches = 1.0;
      L1 = sqrt(z);
      top = 1.0;
    }
    // The pixel's own light as the 8-bit target shows it (clipped), and the SDR picture with the bloom screened on.
    float3 clipped = min(c * (f / L), 1.0);
    float3 sdr = min(clipped * (1.0 - bp) + bp, 1.0);
    // The hue past white: the 8-bit clip turns a fire's orange into yellow-white; the pixel's own channel ratios
    // take over between L1 and twice L1.
    float3 chroma = c / max(max(c.r, c.g), max(c.b, 1e-4));
    float3 own = lerp(clipped, chroma, smoothstep(L1, 2.0 * L1, L));
    // The curve's own upper half: what the 8-bit target clips of it.
    extra = own * min(max(f - 1.0, 0.0), top);
    // A logarithmic extension past that: the same amount per stop wherever it is, from k2.y whites past the white
    // point, eased in over its first stop, and only where the graded picture is near white.
    float3 gl = Linear(g.rgb);
    float t = log2(max(L, 1e-6) / (k2.y * L1));
    float ease = t <= 0.0 ? 0.0 : (t < 1.0 ? 0.5 * t * t : min(t - 0.5, 8.0));
    float near_white = smoothstep(0.7, 0.95, max(gl.r, max(gl.g, gl.b)));
    extra += own * (k1.w * reaches * ease * near_white);
    // The sourced lift: a pixel a light made is taken higher, on its own colour.
    //  - The soft particles' light (a fire, a spark): where that light alone, as exposed, reaches white.
    //  - An additive draw into the lit scene (a corona, a glow; re-drawn into a target of its own): the share of the
    //    pixel's exposed luminance that draw made, and only where the pixel is near white or past it.
    // Together they go no higher than the stronger of the two.
    float lp = dot(k * k2.x * lp3, kLuma);
    float part_tag = smoothstep(0.7, 1.6, lp);
    float add_tag = saturate(dot(k * ad3, kLuma) / max(L, 1e-4)) * smoothstep(0.3 * L1, 0.8 * L1, L);
    float3 sourced = own * min(k3.x * add_tag + k1.z * part_tag, max(k3.x, k1.z));
    p1.z = add_tag;
    p1.w = part_tag;
    p3.x = L / L1;
    p3.z = dot(sourced, kLuma);
    // The lamps: the game's self-lit materials (a car's lamp lens, an LED board, a bulb). Which pixels are a lamp's
    // and with what weight comes from the stencil tag (the lamps' target: rgb the albedo as the G-buffer's own code,
    // a the weight; see "the lamps by a stencil tag"), or, by the old way, from the re-draw's mask and light, kept
    // where the mask's depth is the G-buffer's. The weight is 1 where the material lights the pixel with a quarter of
    // its amount or more, a lens's amount over 2, and 0 where the material does not light the pixel (an alpha of 0).
    // They shine in the pixel's own hue, where the lamp's texture is lit or the lit pixel itself is. A pixel takes
    // the larger of this lift and the sourced one above, per channel: a corona drawn over a lamp never lowers the
    // lamp's lift, and nothing is counted twice.
    float la = 0.0, share = 0.0, lw = 0.0;
    if (k4.y > 0.5) {
      if (max(le.r, max(le.g, le.b)) > 0.002 && abs(zl - zs) <= 1e-6 + 2e-5 * max(zl, zs)) {
        la = dot(lm * lm, kLuma);
        share = la > 0.002 ? dot(le / max(1.0 - le, 1.0 / 255.0), kLuma) / la : 0.0;
        lw = min(share, 1.0) * smoothstep(0.08, 0.16, share);
      }
    } else if (lm4.a > 0.004) {
      la = dot(lm * lm, kLuma);
      share = lm4.a;
      lw = lm4.a;
    }
    if (k3.y > 0.0 && lw > 0.0) {
      float gate = max(smoothstep(0.02, 0.2, la), smoothstep(0.03, 0.3, L));
      // The lift follows what the finished picture shows of the lamp (issue #10). The lamp's pixels are the
      // G-buffer's, sharp and unblurred, and the lift is added after the game's motion blur and depth of field and over
      // its smoke, so a blurred or covered lamp stood out as a sharp sticker. Three cuts, each 1 for a near lamp in
      // clear view:
      //  - blur: the graded pixel (after the blur) against the SDR value the unblurred scene gives here, brightest
      //    channel each; a lamp the blur has spread out is darker in the graded picture than in the scene;
      //  - smoke: the share of the pixel's light the soft particles made (the composite added it to the lit scene);
      //  - distance: from the G-buffer's depth through the camera's projection, gone by k5.w.
      float blur = smoothstep(0.45, 0.8, max(gl.r, max(gl.g, gl.b)) / max(max(sdr.r, max(sdr.g, sdr.b)), 1.0 / 64.0));
      float away = k5.w > 0.0 ? 1.0 - smoothstep(k5.z, k5.w, abs(k5.y / max(abs(zs - k5.x), 1e-9))) : 1.0;
      float follow = blur * away * (1.0 - smoothstep(0.25, 0.75, lp / max(L, 1e-4)));
      float3 lampv = chroma * (lw * gate * follow * k3.y);
      p1.x = share;
      p1.y = gate;
      p2.w = la;
      if (k4.y < 0.5) p2.yzw = float3(follow, blur, away);  // hdr_light_log (the stencil tag: p2's last three are free)
      p3.y = dot(lampv, kLuma);
      if (k4.x > 0.5) sourced += lampv * (1.0 - add_tag);
      else sourced = max(sourced, lampv);
    }
    extra += sourced;
    // The grade's response at this pixel: graded / SDR, linear, per channel, limited.
    extra *= clamp(gl / max(sdr, 1.0 / 64.0), 0.0, 1.25);
  }
  if (k4.z > 3.5) return float4(tex2Dlod(cover, float4(uv, 0, 0)).a, 0.0, 0.0, 0.0);
  if (k4.z > 2.5) return p3;
  if (k4.z > 1.5) return p2;
  if (k4.z > 0.5) return p1;
  return float4(min(extra, 64.0), yg);
}
)";

// The mask pass (the port's hdr_mask.frag). Samplers: 0 the scene pass's result, 1 the finished frame (the final
// target with the UI on it). Out: rgb the extra kept where the frame still shows the graded scene; a the HUD's share.
const char kMaskPs[] = R"(
sampler2D extra : register(s0);
sampler2D front : register(s1);
float4 main(float2 uv : TEXCOORD0) : COLOR {
  float4 x = tex2Dlod(extra, float4(uv, 0, 0));
  float3 f = tex2Dlod(front, float4(uv, 0, 0)).rgb;
  float yf = dot(f, float3(0.299, 0.587, 0.114));
  float yg = x.a;
  // Brighter than the graded scene: the HUD (fading in over a few codes). Darker: the scene under a translucent
  // cover drawn after the grade; its light is attenuated with the picture. A near-opaque dark cover is a HUD panel.
  float bright = smoothstep(0.6 / 255.0, 6.0 / 255.0, yf - yg);
  float keep = yg > 1e-4 ? pow(saturate(yf / yg), 2.2) : 0.0;
  float cover = 1.0 - smoothstep(0.2, 0.5, keep);
  float hud = max(bright, cover);
  float share = yf > 1e-4 ? 1.0 - pow(saturate(yg / yf), 2.2) : 0.0;
  return float4(x.rgb * (keep * (1.0 - hud)), bright * share - cover);
}
)";

// The output pass (the port's hdr_out.frag). Samplers: 0 the back buffer's copy (the SDR frame, UI and overlay on
// it), 1 the mask pass's result. PQ in BT.2020.
const char kOutPs[] = R"(
sampler2D picture : register(s0);
sampler2D extra : register(s1);
float4 a : register(c0);      // peak / paper white, paper white / 10000 nits, highlight gain, HUD white / 10000 nits
float4 b : register(c1);      // shadows' power, shoulder softness, 1 = extra valid, 1 = the calibration patterns
float4 win : register(c2);    // x: the back buffer's width / height
float4 panel : register(c3);  // the settings overlay's panel, in uv: x0, y0, x1, y1
float4 cover : register(c4);  // the extra's scale under the overlay: x outside its panel (1: no overlay), y inside
static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
// The calibration screen's layout, in fractions of the screen from its top left (menu.cpp has the same numbers).
static const float4 kPeakPanel = float4(0.06, 0.24, 0.46, 0.70);
static const float4 kPaperPanel = float4(0.54, 0.24, 0.94, 0.70);
bool InBox(float2 w, float4 box) { return w.x >= box.x && w.x < box.z && w.y >= box.y && w.y < box.w; }
// The patterns, in units of 10000 nits; -1 outside them. Left: a small box at 10000 nits (the display shows it at
// its real peak) with a disc at the peak setting in it, on black; the box is under 5 % of the screen, so an OLED's
// brightness limiter leaves it alone. Right: a card at paper white beside one at 18 % of it.
float Pattern(float2 w) {
  float paper = a.y, peak = a.x * a.y;
  float r = -1.0;
  if (InBox(w, kPeakPanel)) {
    float2 centre = float2(0.5 * (kPeakPanel.x + kPeakPanel.z), 0.5 * (kPeakPanel.y + kPeakPanel.w));
    float2 d = (w - centre) * float2(win.x, 1.0);  // in screen heights
    r = length(d) < 0.065 ? peak : (max(abs(d.x), abs(d.y)) < 0.13 ? 1.0 : 0.0);
  }
  if (InBox(w, kPaperPanel)) {
    float u = (w.x - kPaperPanel.x) / (kPaperPanel.z - kPaperPanel.x);
    r = u < 0.56 ? paper : (u >= 0.60 ? 0.18 * paper : 0.0);
  }
  return r;
}
float3 Pq(float3 n) {
  float3 m = pow(saturate(n), 0.1593017578125);
  return pow((0.8359375 + 18.8515625 * m) / (1.0 + 18.6875 * m), 78.84375);
}
float4 main(float2 uv : TEXCOORD0) : COLOR {
  float3 c = max(tex2Dlod(picture, float4(uv, 0, 0)).rgb, 1e-6);
  float3 l = pow(c, 2.2);
  float4 x = tex2Dlod(extra, float4(uv, 0, 0));
  float hud = 0.0, hud_white = 0.0;
  if (b.z > 0.5) {
    l += max(x.rgb, 0.0) * (InBox(uv, panel) ? cover.y : cover.x);
    hud = saturate(abs(x.a));
    hud_white = saturate(x.a);  // the HUD's own white for the light it added; a dark cover stays at paper white
  }
  float y = dot(l, kLuma);
  if (y > 0.0 && y < 0.18 && b.x != 1.0) {
    // The scene's darks only; the power eases back to 1 between 0.09 and mid grey.
    float p = lerp(b.x, 1.0, smoothstep(0.09, 0.18, y));
    l = lerp(l * (0.18 * pow(y / 0.18, p) / y), l, hud);
  }
  float mx = max(l.r, max(l.g, l.b));
  float m = max(a.x, 1.0);
  if (mx > 1.0) {
    // The highlight gain eases in over the first half paper white past it.
    float xx = 1.0 + (mx - 1.0) * lerp(1.0, a.z, smoothstep(0.0, 0.5, mx - 1.0));
    float v = 1.0;
    if (m > 1.0001) {
      float k = m - 1.0;
      float e, r;
      if (k > 1.0001) {
        // Identity up to 2 paper whites, then an exponential roll-off over the room left to the peak, blended to
        // the rational form.
        float ks = k - 1.0;
        float t = max(xx, 2.0);
        e = (xx <= 2.0) ? xx : 2.0 + ks * (1.0 - exp(-(t - 2.0) / ks));
        r = (xx <= 2.0) ? xx : 2.0 + ks * (t - 2.0) / ((t - 2.0) + ks);
      } else {
        e = 1.0 + k * (1.0 - exp(-(xx - 1.0) / k));
        r = 1.0 + k * (xx - 1.0) / ((xx - 1.0) + k);
      }
      v = lerp(e, r, b.y * saturate(k - 0.5) * smoothstep(2.5, 4.0, xx));
    }
    l *= v / mx;
    // Toward white as the shoulder compresses; luminance kept.
    l = lerp(l, dot(l, kLuma), 0.3 * saturate(1.0 - v / xx));
  }
  float3 nits = l * lerp(a.y, a.w, hud_white);
  float pat = b.w > 0.5 ? Pattern(uv) : -1.0;
  if (pat >= 0.0) nits = pat;
  float3 o = float3(dot(nits, float3(0.6274, 0.3293, 0.0433)), dot(nits, float3(0.0691, 0.9195, 0.0114)),
                    dot(nits, float3(0.0164, 0.0880, 0.8956)));
  return float4(Pq(o), 1.0);
}
)";

// The passes' shaders (quad, scene, mask, output, the lamp tag's quad, in this order), compiled on shadercache.cpp's
// worker thread or read from its disk cache. Asked for at the first present with hdr or hdr_ready (no device needed),
// so the HDR row usually finds them done; the byte code is kept for a later device.
extern uint32_t g_frame;  // frames presented (defined below)
extern const char kTagPs[];  // the lamp tag's quad (below)
std::shared_ptr<SsShaderJob> g_job;
enum { kJobQuad, kJobScene, kJobMask, kJobOut, kJobTag };

void SubmitShaders() {
  auto j = std::make_shared<SsShaderJob>();
  j->tag = "[hdr]";
  j->what = "the HDR passes";
  j->Add(kQuadVs, "main", "vs_3_0", 1u << 15);
  j->Add(kScenePs, "main", "ps_3_0", 1u << 15);
  j->Add(kMaskPs, "main", "ps_3_0", 1u << 15);
  j->Add(kOutPs, "main", "ps_3_0", 1u << 15);
  j->Add(kTagPs, "main", "ps_3_0", 1u << 15);
  g_job = ss_shader_submit(j);
}

const DWORD* Code(int i) {
  return g_job && g_job->Done() && !g_job->items[i].code.empty() ? reinterpret_cast<const DWORD*>(g_job->items[i].code.data()) : nullptr;
}

// ------------------------------------------------------------------------------------------- the device's objects

struct Res {
  IDirect3DDevice9* dev = nullptr;
  bool failed = false;  // the shaders could not be made: the passes stay off, the swap chain stays sRGB
  IDirect3DVertexDeclaration9* decl = nullptr;
  IDirect3DVertexShader9* vs = nullptr;
  IDirect3DPixelShader9 *scene_ps = nullptr, *mask_ps = nullptr, *out_ps = nullptr;
  IDirect3DPixelShader9* tag_ps = nullptr;  // the lamps' stencil tag's quad (made on first use)
  // Default pool (released before a Reset):
  UINT w = 0, h = 0;  // the final target's size
  IDirect3DTexture9 *extra = nullptr, *masked = nullptr;
  IDirect3DTexture9* lamp_mask = nullptr;   // the lamps' albedo and own light (8-bit) and the depth of the lamp draws (INTZ)
  IDirect3DTexture9* lamp_emit = nullptr;
  IDirect3DTexture9* lamp_depth = nullptr;
  UINT lw = 0, lh = 0;
  IDirect3DTexture9* lamp_tag = nullptr;    // the lamps by the stencil tag: the albedo code and the weight (8-bit)
  UINT tw = 0, th = 0;
  IDirect3DTexture9* add = nullptr;  // the additive draws' light, of the lit scene target's size
  UINT aw = 0, ah = 0;
  IDirect3DTexture9* probe_rt = nullptr;   // hdr_light_log: 64 x 256, the window's four sets of terms
  IDirect3DSurface9* probe_sys = nullptr;  // ... read back (system memory)
  IDirect3DTexture9* fcopy = nullptr;  // the final target's copy, when it is not a texture's surface
  D3DSURFACE_DESC fcopy_desc = {};
  UINT bw = 0, bh = 0;
  IDirect3DTexture9* bbcopy = nullptr;
  IDirect3DStateBlock9* sb = nullptr;
  IDirect3DQuery9* q[10] = {};  // 0..5 the three passes; 6, 7 the G-buffer pass; 8, 9 the light pass
} g_r;

// What step A noted this frame. The textures are referenced from the draw to the scene pass (or EndScene), never
// across a frame.
struct Capture {
  IDirect3DTexture9 *scene = nullptr, *adapt = nullptr, *bloom = nullptr;
  float c100[4] = {};
  float white = 0, bloom_power = 1;
  bool reinhard = false;  // SSToneMapper's curve, not the filmic one
  RECT rc = {};       // the draw's viewport
  UINT tw = 0, th = 0;  // its target's size
};
constexpr int kMaxCaptures = 4;
Capture g_cap[kMaxCaptures];
int g_ncap = 0;
bool g_cap_unknown = false;  // a tone map program with a formula not known here drew this frame
IDirect3DTexture9* g_part = nullptr;
float g_part_share = 0.0f;
IDirect3DTexture9* g_scene_depth = nullptr;  // the G-buffer's depth texture, from the frame's first lamp draw
bool g_lamp_cleared = false;                 // the lamps' mask was drawn into this frame
bool g_grade_seen = false;   // the colour grade drew onto the final target this frame: the scene is complete
bool g_split_done = false;   // ... and the first draw onto it after that came (the scene/UI split)
bool g_scene_done = false;   // the scene pass had its turn this frame
bool g_scene_ok = false;     // ... and wrote the extra
bool g_extra_valid = false;  // the mask pass's result is this frame's (read by the output pass)

volatile bool g_run = false;  // the passes run (HDR10 is in use, or forced); render thread writes
bool g_active = false;        // the swap chain is in HDR10
bool g_encoding = false;      // this frame's back buffer holds PQ

void ReleaseCaptures() {
  for (int i = 0; i < g_ncap; ++i) {
    SafeRelease(g_cap[i].scene);
    SafeRelease(g_cap[i].adapt);
    SafeRelease(g_cap[i].bloom);
  }
  g_ncap = 0;
  g_cap_unknown = false;
  SafeRelease(g_part);
  g_part_share = 0.0f;
  SafeRelease(g_scene_depth);
  g_lamp_cleared = false;
}

void ReleaseDefaultPool() {
  ReleaseCaptures();
  SafeRelease(g_r.add);
  g_r.aw = g_r.ah = 0;
  SafeRelease(g_r.lamp_mask);
  SafeRelease(g_r.lamp_emit);
  SafeRelease(g_r.lamp_depth);
  g_r.lw = g_r.lh = 0;
  SafeRelease(g_r.lamp_tag);
  g_r.tw = g_r.th = 0;
  SafeRelease(g_r.extra);
  SafeRelease(g_r.masked);
  SafeRelease(g_r.probe_rt);
  SafeRelease(g_r.fcopy);
  SafeRelease(g_r.bbcopy);
  SafeRelease(g_r.sb);
  for (auto& q : g_r.q) SafeRelease(q);
  g_r.w = g_r.h = g_r.bw = g_r.bh = 0;
  g_scene_ok = g_extra_valid = false;
}

bool EnsureShaders(IDirect3DDevice9* d) {
  if (g_r.dev != d) {
    if (g_r.dev) ss_log("[hdr] the game made a new device (%p -> %p); the passes' objects are made again", g_r.dev, d);
    g_r = Res();  // the old device's objects are abandoned with it
    g_r.dev = d;
    g_ncap = 0;
    g_part = nullptr;
  }
  if (g_r.failed) return false;
  if (g_r.decl) return true;
  // Never waits: false until the worker has the shaders (the SDR picture until then).
  if (!g_job) SubmitShaders();
  if (!g_job->Done()) return false;
  const DWORD t0 = GetTickCount();
  static const D3DVERTEXELEMENT9 kDecl[] = {{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                            {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
                                            D3DDECL_END()};
  const DWORD *v = Code(kJobQuad), *s = Code(kJobScene), *m = Code(kJobMask), *o = Code(kJobOut);
  IDirect3DVertexDeclaration9* decl = nullptr;
  const bool ok = v && s && m && o && SUCCEEDED(d->CreateVertexShader(v, &g_r.vs)) && SUCCEEDED(d->CreatePixelShader(s, &g_r.scene_ps)) &&
                  SUCCEEDED(d->CreatePixelShader(m, &g_r.mask_ps)) && SUCCEEDED(d->CreatePixelShader(o, &g_r.out_ps)) &&
                  SUCCEEDED(d->CreateVertexDeclaration(kDecl, &decl));
  if (!ok) {
    SafeRelease(decl);
    SafeRelease(g_r.vs);
    SafeRelease(g_r.scene_ps);
    SafeRelease(g_r.mask_ps);
    SafeRelease(g_r.out_ps);
    g_r.failed = true;
    ss_log("[hdr] the passes' shaders could not be made; HDR stays off (the SDR picture)");
    return false;
  }
  g_r.decl = decl;
  ss_log("[hdr] shaders ready at frame %u (asked for %lu ms ago; objects made in %lu ms on the render thread)", g_frame,
         static_cast<unsigned long>(GetTickCount() - g_job->submitted), static_cast<unsigned long>(GetTickCount() - t0));
  return true;
}

bool MakeTarget(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f, IDirect3DTexture9** out) {
  return SUCCEEDED(d->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, f, D3DPOOL_DEFAULT, out, nullptr));
}

bool EnsureStateBlock(IDirect3DDevice9* d) { return g_r.sb || SUCCEEDED(d->CreateStateBlock(D3DSBT_ALL, &g_r.sb)); }

// The two float targets of the final target's size.
bool EnsureTargets(IDirect3DDevice9* d, UINT w, UINT h) {
  if (g_r.extra && g_r.masked && g_r.w == w && g_r.h == h) return true;
  SafeRelease(g_r.extra);
  SafeRelease(g_r.masked);
  g_extra_valid = false;
  if (!MakeTarget(d, w, h, D3DFMT_A16B16G16R16F, &g_r.extra) || !MakeTarget(d, w, h, D3DFMT_A16B16G16R16F, &g_r.masked)) {
    SafeRelease(g_r.extra);
    SafeRelease(g_r.masked);
    static int said = 0;
    if (said++ < 3) ss_log("[hdr] the float targets could not be made at %ux%u; this frame has no extra light", w, h);
    return false;
  }
  g_r.w = w;
  g_r.h = h;
  ss_log("[hdr] targets made at %ux%u (A16B16G16R16F)", w, h);
  return true;
}

// The final target as a texture to read: its own texture, or (when it is a plain surface, e.g. the back buffer
// itself) a copy made now. A reference the caller releases.
IDirect3DTexture9* TextureOf(IDirect3DDevice9* d, IDirect3DSurface9* f, const D3DSURFACE_DESC& desc) {
  IDirect3DTexture9* t = nullptr;
  if (SUCCEEDED(f->GetContainer(kIidTexture9, reinterpret_cast<void**>(&t))) && t) return t;
  if (!g_r.fcopy || g_r.fcopy_desc.Width != desc.Width || g_r.fcopy_desc.Height != desc.Height || g_r.fcopy_desc.Format != desc.Format) {
    SafeRelease(g_r.fcopy);
    if (!MakeTarget(d, desc.Width, desc.Height, desc.Format, &g_r.fcopy)) return nullptr;
    g_r.fcopy_desc = desc;
    ss_log("[hdr] the final target is not a texture (%ux%u format %d): read through a copy", desc.Width, desc.Height, static_cast<int>(desc.Format));
  }
  IDirect3DSurface9* s = nullptr;
  bool ok = SUCCEEDED(g_r.fcopy->GetSurfaceLevel(0, &s)) && s && SUCCEEDED(d->StretchRect(f, nullptr, s, nullptr, D3DTEXF_NONE));
  SafeRelease(s);
  if (!ok) return nullptr;
  g_r.fcopy->AddRef();
  return g_r.fcopy;
}

struct Vertex {
  float x, y, z, w, u, v;
};

// One full-screen draw into the bound target of w x h; the quad is moved by half a pixel so texel centres meet
// pixel centres.
void Quad(IDirect3DDevice9* d, UINT w, UINT h) {
  const float dx = -1.0f / w, dy = 1.0f / h;
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

// Our drawing on the game's device: every state is captured before and put back after (a state block, plus the
// render targets and the depth surface, which a state block does not hold).
struct Scope {
  IDirect3DDevice9* d;
  IDirect3DSurface9* rt[4] = {};
  IDirect3DSurface9* ds = nullptr;
  DWORD nrt = 1;
  bool ok = false;
  explicit Scope(IDirect3DDevice9* dev) : d(dev) {
    ++ss_own_drawing;
    if (!EnsureStateBlock(d) || FAILED(g_r.sb->Capture())) return;
    static DWORD count = 0;
    if (!count) {
      D3DCAPS9 caps;
      count = SUCCEEDED(d->GetDeviceCaps(&caps)) ? caps.NumSimultaneousRTs : 1;
      count = count > 4 ? 4 : count < 1 ? 1 : count;
    }
    nrt = count;
    for (DWORD i = 0; i < nrt; ++i) d->GetRenderTarget(i, &rt[i]);
    d->GetDepthStencilSurface(&ds);
    ok = true;
  }
  void States() {
    for (DWORD i = 1; i < nrt; ++i) d->SetRenderTarget(i, nullptr);
    d->SetDepthStencilSurface(nullptr);
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
    d->SetVertexShader(g_r.vs);
  }
  ~Scope() {
    if (ok) {
      if (rt[0]) d->SetRenderTarget(0, rt[0]);
      for (DWORD i = 1; i < nrt; ++i) d->SetRenderTarget(i, rt[i]);
      d->SetDepthStencilSurface(ds);
      g_r.sb->Apply();
    }
    for (auto& s : rt) SafeRelease(s);
    SafeRelease(ds);
    --ss_own_drawing;
  }
};

// ------------------------------------------------------------------------------------------------ timing (aids)

struct Timing {
  double cpu[3] = {};          // seconds in the scene, mask and output passes (CPU side: the calls)
  uint32_t frames = 0, with_extra = 0, plain = 0, outputs = 0, particles = 0;
  uint32_t composites = 0, composites_unread = 0;  // particle composite draws seen, and those whose inputs could not be read
  double gpu[3] = {};          // milliseconds, from timestamp queries
  uint32_t gpu_n = 0;
  double gpu2[2] = {};         // the G-buffer pass and the light pass (the lamps' and the additive draws' work is in them)
  uint32_t gpu2_n = 0;
  bool have2 = false;          // ... were stamped in the pending frame
  bool pending = false;        // the six queries were issued and wait to be read
  uint32_t issue_frame = 0;
  double freq = 0;
  bool no_queries = false;
} g_t;
uint32_t g_frame = 0;
int64_t g_qpf = 0;

int64_t Qpc() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}
double Secs(int64_t dt) { return g_qpf ? double(dt) / double(g_qpf) : 0.0; }

// Whether this frame's passes are bracketed by timestamps (one frame in 16; decided at the present before it).
bool g_timed = false;
unsigned g_stamped = 0;

void Stamp(IDirect3DDevice9* d, int i) {
  if (!g_r.q[i] && FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &g_r.q[i]))) {
    if (!g_t.no_queries) ss_log("[hdr] no timestamp queries on this device; SS_HDR_GPU gives nothing");
    g_t.no_queries = true;
    return;
  }
  g_r.q[i]->Issue(D3DISSUE_END);
  g_stamped |= 1u << i;
}

void ReadStamps(IDirect3DDevice9* d) {
  if (!g_t.pending || g_frame - g_t.issue_frame < 4) return;
  if (g_t.freq == 0) {
    IDirect3DQuery9* fq = nullptr;
    UINT64 f = 0;
    if (SUCCEEDED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &fq)) && fq) {
      fq->Issue(D3DISSUE_END);
      for (int i = 0; i < 100 && fq->GetData(&f, sizeof f, D3DGETDATA_FLUSH) == S_FALSE; ++i) Sleep(0);
      fq->Release();
    }
    g_t.freq = f ? double(f) : 1e9;
  }
  UINT64 v[10] = {};
  for (int i = 0; i < (g_t.have2 ? 10 : 6); ++i) {
    if (!g_r.q[i]) {  // a pass did not run in that frame: the sample is dropped
      g_t.pending = false;
      return;
    }
    if (g_r.q[i]->GetData(&v[i], sizeof v[i], 0) != S_OK) {
      if (g_frame - g_t.issue_frame > 120) g_t.pending = false;
      return;
    }
  }
  g_t.pending = false;
  for (int k = 0; k < 3; ++k) g_t.gpu[k] += double(v[2 * k + 1] - v[2 * k]) / g_t.freq * 1000.0;
  ++g_t.gpu_n;
  if (g_t.have2 && v[7] >= v[6] && v[9] >= v[8]) {
    g_t.gpu2[0] += double(v[7] - v[6]) / g_t.freq * 1000.0;
    g_t.gpu2[1] += double(v[9] - v[8]) / g_t.freq * 1000.0;
    ++g_t.gpu2_n;
  }
}

void AdditiveReport();
// The GPU times since the last call, into the log, and the sums emptied (SS_HDR_GPU; at each SS_HDR_TEST_SET step).
void GpuSegment(const char* what) {
  if (!g_t.gpu_n && !g_t.gpu2_n) return;
  ss_log("[hdr] GPU (timestamps) %s, LIGHT SOURCES %.0f, test %ld: G-buffer pass %.3f ms, light pass %.3f ms (%u samples); scene %.3f ms, mask %.3f ms, "
         "output %.3f ms (%u samples)",
         what, static_cast<float>(g_lift), static_cast<long>(g_test), g_t.gpu2_n ? g_t.gpu2[0] / g_t.gpu2_n : 0.0, g_t.gpu2_n ? g_t.gpu2[1] / g_t.gpu2_n : 0.0,
         g_t.gpu2_n, g_t.gpu_n ? g_t.gpu[0] / g_t.gpu_n : 0.0, g_t.gpu_n ? g_t.gpu[1] / g_t.gpu_n : 0.0, g_t.gpu_n ? g_t.gpu[2] / g_t.gpu_n : 0.0, g_t.gpu_n);
  g_t.gpu[0] = g_t.gpu[1] = g_t.gpu[2] = g_t.gpu2[0] = g_t.gpu2[1] = 0;
  g_t.gpu_n = g_t.gpu2_n = 0;
}
void ClassedCounts(LONG out[4]);  // by kind: tone map, unknown tone map, particle composite, grade

void Report() {
  if (++g_t.frames < 3600) return;
  LONG kinds[4] = {};
  ClassedCounts(kinds);
  AdditiveReport();
  char gpu[160] = "";
  if (g_t.gpu_n)
    snprintf(gpu, sizeof gpu, "; GPU (timestamps, %u samples): scene %.3f ms, mask %.3f ms, output %.3f ms", g_t.gpu_n,
             g_t.gpu[0] / g_t.gpu_n, g_t.gpu[1] / g_t.gpu_n, g_t.gpu[2] / g_t.gpu_n);
  ss_log("[hdr] last %u frames: %u with the scene's light (%u of them with the particles' light read), %u without (no tone map draw), "
         "%u output passes; CPU per frame: scene %.3f ms, mask %.3f ms, output %.3f ms%s; particle composite draws %u (%u unread); shaders classed: %ld tone map, "
         "%ld unknown tone map, %ld particle composite, %ld grade",
         g_t.frames, g_t.with_extra, g_t.particles, g_t.plain, g_t.outputs, g_t.cpu[0] / g_t.frames * 1000, g_t.cpu[1] / g_t.frames * 1000,
         g_t.cpu[2] / g_t.frames * 1000, gpu, g_t.composites, g_t.composites_unread, kinds[0], kinds[1], kinds[2], kinds[3]);
  GpuSegment("of the last 3600 frames");
  const bool pending = g_t.pending, noq = g_t.no_queries, have2 = g_t.have2;
  const uint32_t issue = g_t.issue_frame;
  const double freq = g_t.freq;
  g_t = Timing();
  g_t.pending = pending, g_t.no_queries = noq, g_t.issue_frame = issue, g_t.freq = freq, g_t.have2 = have2;
}

// ------------------------------------------------------------------------------------- step A: the tone map draw

enum Kind { K_TONEMAP, K_TONEMAP_UNKNOWN, K_COMPOSITE, K_GRADE, K_EMISSIVE };
struct Classed {
  IDirect3DPixelShader9* ps;  // a reference is held, so the address stays this shader's
  Kind kind;
  int r_consts, r_consts2, r_curve, s_scene, s_adapt, s_bloom;  // tone map: registers (-1: not there)
  int r_div, r_alpha, s_part, s_lit;                            // composite (s_lit: the lit scene it adds the particles to)
  bool reinhard;                                                // tone map: SSToneMapper's curve
  // Self-lit materials (the lamps): 1 a car's lamp lens (VehicleEmissiveLight_emissiveValues, a vector; the lamp is
  // the component its lightIndex constant names), 2 Emissive_<n>_emissive, emissive_<n>_emissive,
  // Diffuse_Emissive_<n>_emissive, Emissive_Pulse_<n>_brightness, 3 Emissive_Alpha_<n>_emissive. Every one of them
  // writes its light into the G-buffer's third target as (albedo x amount x c[r_scale].z + albedo x ambient), kept
  // below 1 by x / (x + c[r_soft].x) (sslighting_constants1, sslighting_reflectionConstants).
  int e_kind, r_emissive, r_index, r_scale, r_soft;
  // For the stencil tag: the material lights by a per-pixel factor (its weight is read from the G-buffer's third
  // target); the registers of the light term's constants (sslighting_ambient, _sky, _ground, _diffuse2; -1: not
  // named); it writes a reflection into that target as well (no bound on its light term).
  bool decode, reflect;
  int r_amb, r_sky, r_gnd, r_dif2;
};
constexpr int kMaxClassed = 512;
Classed g_classed[kMaxClassed];
volatile LONG g_nclassed = 0;

void ClassedCounts(LONG out[4]) {
  for (LONG i = 0, n = g_nclassed; i < n; ++i)
    if (g_classed[i].kind < 4) ++out[g_classed[i].kind];
}

// The classed shaders by address (open addressing; made again whenever the list changes).
int16_t g_class_hash[2048];
void RebuildClassHash() {
  memset(g_class_hash, 0, sizeof g_class_hash);
  for (LONG i = 0, n = g_nclassed; i < n; ++i) {
    uint32_t h = (reinterpret_cast<uintptr_t>(g_classed[i].ps) >> 4) & 2047;
    while (g_class_hash[h]) h = (h + 1) & 2047;
    g_class_hash[h] = static_cast<int16_t>(i + 1);
  }
}
// The game's shader behind a variant another part binds in its place: shadow smoothing's (a shader that reads the
// cascades), LOD smoothing's dither variant (a fading draw), deband's (the tone mapper, the grade) and mblur's (the motion
// blur), one made of another.
// The answers are kept by shader address (direct-mapped), so a SetPixelShader costs one compare instead of up to six
// hash-map lookups (SSPC-17, perf-plan A5). The two maps only ever gain entries, and every insertion bumps
// ss_shader_map_gen, which empties the cache; so does a Reset. Same thread as the maps (the game's render thread).
struct OriginalEntry {
  IDirect3DPixelShader9* ps;
  IDirect3DPixelShader9* o;
};
OriginalEntry g_original_cache[512];
unsigned long g_original_gen = ~0ul;
void ForgetOriginals() { g_original_gen = ~0ul; }
IDirect3DPixelShader9* Original(IDirect3DPixelShader9* ps) {
  if (!ps) return ps;
  const unsigned long gen = ss_shader_map_gen;
  if (gen != g_original_gen) {
    memset(g_original_cache, 0, sizeof g_original_cache);
    g_original_gen = gen;
  }
  OriginalEntry& e = g_original_cache[(reinterpret_cast<uintptr_t>(ps) >> 4) & 511];
  if (e.ps == ps) return e.o;
  IDirect3DPixelShader9* const key = ps;
  for (int i = 0; i < 3 && ps; ++i) {
    IDirect3DPixelShader9* o = static_cast<IDirect3DPixelShader9*>(ss_blur_original(ss_deband_original(ss_lodfade_original(ss_shadowsmooth_original(ps)))));
    if (o == ps) break;
    ps = o;
  }
  e.ps = key;
  e.o = ps;
  return ps;
}

const Classed* LookupClass(const IDirect3DPixelShader9* ps) {
  if (!ps) return nullptr;
  ps = Original(const_cast<IDirect3DPixelShader9*>(ps));  // a variant has the class of the game's shader behind it
  for (uint32_t h = (reinterpret_cast<uintptr_t>(ps) >> 4) & 2047; g_class_hash[h]; h = (h + 1) & 2047)
    if (g_classed[g_class_hash[h] - 1].ps == ps) return &g_classed[g_class_hash[h] - 1];
  return nullptr;
}

struct CtabEntry {
  const char* name;
  int set, reg;  // set: 0 bool, 1 int, 2 float, 3 sampler
};

// The constant table (the CTAB comment the compiler leaves at the start of the byte code): names and registers.
// The byte code has no length of its own; only the leading comment tokens are walked, each by its own length.
int ParseCtab(const DWORD* code, CtabEntry* out, int max, bool vertex = false) {
  int n = 0;
  if (!code || (code[0] >> 16) != (vertex ? 0xFFFEu : 0xFFFFu)) return 0;  // not a pixel (vertex) shader
  for (UINT i = 1; i < 4096;) {
    if (!Readable(reinterpret_cast<uintptr_t>(&code[i]), 8)) break;
    if ((code[i] & 0xFFFF) != 0xFFFE) break;  // comments come first
    const UINT len = (code[i] >> 16) & 0x7FFF;
    if (len >= 8 && code[i + 1] == 0x42415443 && Readable(reinterpret_cast<uintptr_t>(&code[i + 2]), (len - 1) * 4)) {  // 'CTAB'
      const unsigned char* base = reinterpret_cast<const unsigned char*>(&code[i + 2]);
      const UINT size = (len - 1) * 4;
      const DWORD* h = reinterpret_cast<const DWORD*>(base);
      const UINT count = h[3], info = h[4];
      for (UINT k = 0; k < count && info + (k + 1) * 20 <= size && n < max; ++k) {
        const unsigned char* e = base + info + k * 20;
        const DWORD name = *reinterpret_cast<const DWORD*>(e);
        const WORD* w = reinterpret_cast<const WORD*>(e + 4);
        if (name >= size || !memchr(base + name, 0, size - name)) continue;
        out[n++] = {reinterpret_cast<const char*>(base + name), w[0], w[1]};
      }
      return n;
    }
    i += 1 + len;
  }
  return n;
}

// ---- Test aid, SS_HDR_CENSUS="150,200" (seconds): what the light pass and the G-buffer pass draw, for the light
// sources' work. At creation: every pixel shader naming an emissive constant, and the disassembly of the tone mappers
// whose formula is not known. In the frame after each of those seconds: every draw into the lit scene target with an
// additive blend (shader, its samplers and what is bound on them, blend, depth states), the count of the others,
// and every draw of a shader naming an emissive constant (its constants' values, the targets bound).
std::vector<int> g_census_at;
size_t g_census_next = 0;
bool g_census = false;
int g_census_lines = 0, g_census_lit = 0, g_census_additive = 0;
IDirect3DSurface9* g_lit_prev = nullptr;  // the lit scene target of the last frame (compared only)
struct CensusSeen {
  IDirect3DPixelShader9* ps;
  int n;
} g_census_seen[64];
int g_census_nseen = 0;

std::string CensusNames(const DWORD* code, bool samplers_only) {
  CtabEntry e[48];
  const int n = ParseCtab(code, e, 48);
  std::string out;
  for (int i = 0; i < n; ++i) {
    if (samplers_only && e[i].set != 3) continue;
    char b[160];
    snprintf(b, sizeof b, "%s%s %c%d", out.empty() ? "" : ", ", e[i].name, e[i].set == 3 ? 's' : e[i].set == 2 ? 'c' : e[i].set == 1 ? 'i' : 'b', e[i].reg);
    out += b;
    if (out.size() > 900) break;
  }
  return out;
}

uint64_t CodeHash(const DWORD* code, UINT bytes) {
  uint64_t h = 1469598103934665603ull;
  const unsigned char* b = reinterpret_cast<const unsigned char*>(code);
  for (UINT i = 0; i < bytes; ++i) h = (h ^ b[i]) * 1099511628211ull;
  return h;
}

void CensusDisassemble(const DWORD* code) {
  using DisFn = HRESULT(WINAPI*)(const DWORD*, BOOL, const char*, IHdrCode**);
  HMODULE m = GetModuleHandleA("d3dx9_42.dll");
  DisFn dis = m ? reinterpret_cast<DisFn>(GetProcAddress(m, "D3DXDisassembleShader")) : nullptr;
  IHdrCode* text = nullptr;
  if (!dis || FAILED(dis(code, FALSE, nullptr, &text)) || !text) return;
  std::string t(static_cast<const char*>(text->GetBufferPointer()), strnlen(static_cast<const char*>(text->GetBufferPointer()), text->GetBufferSize()));
  text->Release();
  int lines = 0;
  for (size_t at = 0; at < t.size() && lines < 90;) {
    size_t end = t.find('\n', at);
    if (end == std::string::npos) end = t.size();
    std::string line = t.substr(at, end - at);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (!line.empty() && line.compare(0, 2, "//") != 0) ss_log("[hdr] census:   %s", line.c_str()), ++lines;
    at = end + 1;
  }
}

void CensusCreated(IDirect3DPixelShader9* ps, const DWORD* code, bool unknown_tonemap) {
  if (g_census_at.empty()) return;
  static int said_e = 0, said_t = 0;
  const std::string names = CensusNames(code, false);
  if (unknown_tonemap && said_t++ < 4) {
    ss_log("[hdr] census: tone mapper %p with a formula not known: %s", ps, names.c_str());
    CensusDisassemble(code);
  } else if (names.find("missive") != std::string::npos && said_e++ < 200) {
    ss_log("[hdr] census: emissive shader %p created: %s", ps, names.c_str());
  }
}

// One draw of a listed frame.
void CensusDraw(IDirect3DDevice9* d, UINT prims, const char* how) {
  IDirect3DSurface9 *rt = nullptr, *rt1 = nullptr;
  IDirect3DPixelShader9* ps = nullptr;
  if (FAILED(d->GetRenderTarget(0, &rt)) || !rt) return;
  rt->Release();
  d->GetPixelShader(&ps);
  if (!ps) return;
  ps->Release();
  UINT size = 0;
  std::vector<DWORD> code;
  auto load = [&] {
    if (!code.empty()) return true;
    if (FAILED(ps->GetFunction(nullptr, &size)) || size < 8 || size > (1u << 20)) return false;
    code.resize((size + 3) / 4);
    return SUCCEEDED(ps->GetFunction(code.data(), &size));
  };
  DWORD blend = 0, src = 0, dst = 0, zen = 0, zwr = 0, zfunc = 0, cw = 0;
  d->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
  d->GetRenderState(D3DRS_DESTBLEND, &dst);
  const bool lit = rt == g_lit_prev;
  bool emissive = false;
  if (!lit) {  // the G-buffer pass has a second colour target
    if (SUCCEEDED(d->GetRenderTarget(1, &rt1)) && rt1) {
      rt1->Release();
      if (load()) emissive = CensusNames(code.data(), false).find("missive") != std::string::npos;
    }
    if (!emissive) return;
  }
  if (lit) {
    ++g_census_lit;
    if (!blend || dst != D3DBLEND_ONE) return;
    ++g_census_additive;
  }
  for (int i = 0; i < g_census_nseen; ++i)
    if (g_census_seen[i].ps == ps) {
      ++g_census_seen[i].n;
      return;
    }
  if (g_census_nseen < 64) g_census_seen[g_census_nseen++] = {ps, 1};
  if (g_census_lines++ > 120 || !load()) return;
  d->GetRenderState(D3DRS_SRCBLEND, &src);
  d->GetRenderState(D3DRS_ZENABLE, &zen);
  d->GetRenderState(D3DRS_ZWRITEENABLE, &zwr);
  d->GetRenderState(D3DRS_ZFUNC, &zfunc);
  d->GetRenderState(D3DRS_COLORWRITEENABLE, &cw);
  D3DSURFACE_DESC desc = {};
  rt->GetDesc(&desc);
  ss_log("[hdr] census: %s draw (%s, %u primitives) into %ux%u fmt %d: ps %p %016llx, blend %lu (%lu, %lu), z %lu write %lu func %lu, colour write %lx",
         lit ? "additive lit-scene" : "emissive G-buffer", how, prims, desc.Width, desc.Height, static_cast<int>(desc.Format), ps,
         static_cast<unsigned long long>(CodeHash(code.data(), size)), blend, src, dst, zen, zwr, zfunc, cw);
  CtabEntry e[48];
  const int n = ParseCtab(code.data(), e, 48);
  for (int i = 0; i < n; ++i) {
    if (e[i].set == 3) {
      IDirect3DBaseTexture9* t = nullptr;
      D3DSURFACE_DESC td = {};
      if (SUCCEEDED(d->GetTexture(e[i].reg, &t)) && t) {
        if (t->GetType() == D3DRTYPE_TEXTURE) static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &td);
        t->Release();
      }
      ss_log("[hdr] census:   %s s%d: %ux%u fmt %d%s", e[i].name, e[i].reg, td.Width, td.Height, static_cast<int>(td.Format),
             (td.Usage & D3DUSAGE_RENDERTARGET) ? " target" : (td.Usage & D3DUSAGE_DEPTHSTENCIL) ? " depth" : "");
    } else if (e[i].set == 2 && (emissive || strstr(e[i].name, "olour") || strstr(e[i].name, "olor") || strstr(e[i].name, "ntensity"))) {
      float v[4] = {};
      d->GetPixelShaderConstantF(e[i].reg, v, 1);
      ss_log("[hdr] census:   %s c%d = (%.4g, %.4g, %.4g, %.4g)", e[i].name, e[i].reg, v[0], v[1], v[2], v[3]);
    }
  }
}

void CensusEnd() {
  ss_log("[hdr] census: end of frame: %d draws into the lit scene target, %d of them additive; shaders listed above drew:", g_census_lit, g_census_additive);
  std::string line;
  for (int i = 0; i < g_census_nseen; ++i) {
    char b[40];
    snprintf(b, sizeof b, " %p x%d", g_census_seen[i].ps, g_census_seen[i].n);
    line += b;
  }
  ss_log("[hdr] census:%s", line.c_str());
  g_census = false;
  g_census_lines = g_census_lit = g_census_additive = g_census_nseen = 0;
}

void Classify(IDirect3DPixelShader9* ps, const DWORD* code) {
  CtabEntry e[64];
  const int n = ParseCtab(code, e, 64);
  if (n == 0) return;
  bool census_done = false;
  Classed c = {ps, K_TONEMAP, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, false, 0, -1, -1, -1, -1, false, false, -1, -1, -1, -1};
  bool filmic = false, plain = false, composite = false, grade = false;
  auto ends = [](const char* s, const char* tail) {
    const size_t a = strlen(s), b = strlen(tail);
    return a >= b && !strcmp(s + a - b, tail);
  };
  int other_sampler = -1;
  c.decode = c.reflect = false;
  c.r_amb = c.r_sky = c.r_gnd = c.r_dif2 = -1;
  for (int i = 0; i < n; ++i) {
    const char* s = e[i].name;
    const bool smp = e[i].set == 3, flt = e[i].set == 2;
    if (strstr(s, "_cubemap") || strstr(s, "_reflect_")) c.reflect = true;
    if (flt && !strcmp(s, "sslighting_ambient")) c.r_amb = e[i].reg;
    else if (flt && !strcmp(s, "sslighting_sky")) c.r_sky = e[i].reg;
    else if (flt && !strcmp(s, "sslighting_ground")) c.r_gnd = e[i].reg;
    else if (flt && !strcmp(s, "sslighting_diffuse2")) c.r_dif2 = e[i].reg;
    if (flt && ((!strncmp(s, "Emissive_Alpha_", 15) && ends(s, "_emissive")) || (!strncmp(s, "Diffuse_Emissive_", 17) && ends(s, "_emissive")) ||
                (!strncmp(s, "Emissive_Pulse_", 15) && ends(s, "_brightness"))))
      c.decode = true;
    if (!strcmp(s, "SSFilmicToneMapper_Consts") && flt) c.r_consts = e[i].reg, filmic = true;
    else if (!strcmp(s, "SSFilmicToneMapper_Consts2") && flt) c.r_consts2 = e[i].reg;
    else if (!strcmp(s, "SSFilmicToneMapper_AveLum") && smp) c.s_adapt = e[i].reg;
    else if (!strcmp(s, "SSBloomAdd_Curve") && flt) c.r_curve = e[i].reg;
    else if (!strcmp(s, "SSBloomAdd_tex") && smp) c.s_bloom = e[i].reg;
    else if (!strcmp(s, "SSToneMapper_Consts") && flt) c.r_consts = e[i].reg, plain = true;
    else if (!strcmp(s, "SSToneMapper_AveLum") && smp) c.s_adapt = e[i].reg, plain = true;
    else if (!strncmp(s, "SSToneMapper_", 13)) plain = true;
    else if (!strcmp(s, "VehicleEmissiveLight_emissiveValues") && flt) c.e_kind = 1, c.r_emissive = e[i].reg;
    else if (!strncmp(s, "VehicleEmissiveLight_", 21) && ends(s, "_lightIndex") && flt) c.r_index = e[i].reg;
    else if (!strncmp(s, "Emissive_Alpha_", 15) && ends(s, "_emissive") && flt) { if (!c.e_kind) c.e_kind = 3, c.r_emissive = e[i].reg; }
    else if ((!strncmp(s, "Emissive_", 9) || !strncmp(s, "emissive_", 9) || !strncmp(s, "Diffuse_Emissive_", 17)) && ends(s, "_emissive") && flt) {
      if (!c.e_kind) c.e_kind = 2, c.r_emissive = e[i].reg;
    }
    else if (!strncmp(s, "Emissive_Pulse_", 15) && ends(s, "_brightness") && flt) { if (!c.e_kind) c.e_kind = 2, c.r_emissive = e[i].reg; }
    else if (!strcmp(s, "sslighting_constants1") && flt) c.r_scale = e[i].reg;
    else if (!strcmp(s, "sslighting_reflectionConstants") && flt) c.r_soft = e[i].reg;
    else if (!strcmp(s, "LowResParticleLightComposite_particleTexture") && smp) c.s_part = e[i].reg, composite = true;
    else if (!strcmp(s, "LowResParticleLightComposite_sceneTexture") && smp) c.s_lit = e[i].reg;
    else if (!strcmp(s, "LowResParticleLightComposite_rcpOverDrawDiv") && flt) c.r_div = e[i].reg;
    else if (!strcmp(s, "LowResParticleLightComposite_AlphaPower") && flt) c.r_alpha = e[i].reg;
    else if (!strncmp(s, "color_cube_", 11) || !strncmp(s, "ss_gradient_and_vignette_", 25)) grade = true;
    else if (!strcmp(s, "PosUVUVPassThru_texture") && smp) c.s_scene = e[i].reg;
    else if (smp) other_sampler = e[i].reg;
  }
  CensusCreated(ps, code, false);
  if (c.e_kind && (c.r_scale < 0 || c.r_soft < 0)) c.e_kind = 0;  // not a G-buffer material that writes its own light
  if (!filmic && !plain && !composite && !grade && !c.e_kind) return;
  (void)census_done;
  if (!filmic && !plain && !composite && !grade) {
    c.kind = K_EMISSIVE;
  } else if (composite) {
    c.kind = K_COMPOSITE;
    if (c.r_div < 0 || c.r_alpha < 0) {
      static int said_variant = 0;
      if (said_variant++ < 4) ss_log("[hdr] a particle composite program without its two constants (div c%d, alpha c%d): not classed", c.r_div, c.r_alpha);
      return;
    }
  } else if (grade && !filmic && !plain) {
    c.kind = K_GRADE;
  } else {
    if (c.s_scene < 0) c.s_scene = other_sampler;
    c.reinhard = !filmic && plain && c.r_consts >= 0;
    c.kind = c.s_scene >= 0 && ((filmic && c.r_consts2 >= 0) || c.reinhard) ? K_TONEMAP : K_TONEMAP_UNKNOWN;
    if (!filmic) CensusCreated(ps, code, true);
  }
  LONG at = g_nclassed;
  if (at >= kMaxClassed) {
    // The game made these shaders again and again: the oldest grade pass is let go (there are many of those, a few
    // per level; the tone mappers and the particle composite are few and are kept), else the oldest of all.
    LONG drop = -1;
    for (LONG i = 0; i < at && drop < 0; ++i)
      if (g_classed[i].kind == K_EMISSIVE) drop = i;
    for (LONG i = 0; i < at && drop < 0; ++i)
      if (g_classed[i].kind == K_GRADE) drop = i;
    if (drop < 0) drop = 0;
    SafeRelease(g_classed[drop].ps);
    memmove(&g_classed[drop], &g_classed[drop + 1], sizeof(Classed) * (kMaxClassed - 1 - drop));
    at = kMaxClassed - 1;
  }
  ps->AddRef();
  g_classed[at] = c;
  g_nclassed = at + 1;
  RebuildClassHash();
  static int said = 0;
  if (c.kind != K_EMISSIVE && said++ < 16)
    ss_log("[hdr] pixel shader %p classed: %s%s", ps,
           c.kind == K_COMPOSITE ? "the particle composite" : c.kind == K_GRADE ? "a colour grade pass (colour cube / gradient and vignette)"
           : c.kind == K_TONEMAP ? (c.reinhard ? "the SSToneMapper tone mapper" : "the filmic tone mapper")
           : "a tone mapper with a formula not known here (no extra light on its frames)",
           c.kind == K_TONEMAP ? (c.s_bloom >= 0 ? " with bloom" : c.s_adapt >= 0 ? ", no bloom" : ", no bloom, no adaptation") : "");
}

IDirect3DTexture9* Texture2D(IDirect3DDevice9* d, int sampler) {
  IDirect3DBaseTexture9* t = nullptr;
  if (sampler < 0 || FAILED(d->GetTexture(sampler, &t)) || !t) return nullptr;
  if (t->GetType() != D3DRTYPE_TEXTURE) {
    t->Release();
    return nullptr;
  }
  return static_cast<IDirect3DTexture9*>(t);
}

const Classed* ClassOf(IDirect3DDevice9* d) {
  IDirect3DPixelShader9* ps = nullptr;
  if (FAILED(d->GetPixelShader(&ps)) || !ps) return nullptr;
  ps->Release();  // compared only
  return LookupClass(ps);
}

bool OnFinal(IDirect3DDevice9* d, IDirect3DSurface9** final_surface) {
  IDirect3DSurface9* rt = nullptr;
  if (FAILED(d->GetRenderTarget(0, &rt)) || !rt) return false;
  rt->Release();  // compared only
  *final_surface = FinalSurface();
  return rt == *final_surface;
}

// Any draw after the colour grade drew onto the final target: the first one onto that target by a shader that is
// not the chain's own is the first of the UI (the car's HUD cards, then the Scaleform movies). The scene is
// complete and nothing is on it yet: postfx.cpp's passes and the scene pass run here, before the draw.
void MaybeSplit(IDirect3DDevice9* d) {
  IDirect3DSurface9* f = nullptr;
  if (d != GameDevice() || !OnFinal(d, &f) || ClassOf(d)) return;
  g_split_done = true;
  static bool said = false;
  if (!said) ss_log("[hdr] scene/UI split found: the first draw onto the final target after the colour grade");
  said = true;
  ss_postfx_split(d, f);
}

// The same split, made right after the grade's own draw onto the final target (the scene is complete there).
void SplitAfterGrade(IDirect3DDevice9* d) {
  IDirect3DSurface9* f = nullptr;
  if (d != GameDevice() || !OnFinal(d, &f)) return;
  g_split_done = true;
  static bool said = false;
  if (!said) ss_log("[hdr] scene/UI split: right after the colour grade's draw onto the final target");
  said = true;
  ss_postfx_split(d, f);
}

void NoteDraw(IDirect3DDevice9* d) {
  const Classed* c = ClassOf(d);
  if (!c || d != GameDevice()) return;
  if (c->kind == K_GRADE) {
    IDirect3DSurface9* f = nullptr;
    if (OnFinal(d, &f)) g_grade_seen = true;
    return;
  }
  if (c->kind == K_COMPOSITE) {
    float div[4] = {}, alpha[4] = {};
    SafeRelease(g_part);
    g_part_share = 0.0f;
    if (SUCCEEDED(d->GetPixelShaderConstantF(c->r_div, div, 1)) && SUCCEEDED(d->GetPixelShaderConstantF(c->r_alpha, alpha, 1)) &&
        (g_part = Texture2D(d, c->s_part)) != nullptr)
      g_part_share = div[3] * alpha[0];
    ++g_t.composites;
    if (!g_part) ++g_t.composites_unread;
    if (IDirect3DTexture9* lit = Texture2D(d, c->s_lit)) {
      IDirect3DSurface9* ls = nullptr;
      if (SUCCEEDED(lit->GetSurfaceLevel(0, &ls)) && ls) {
        g_lit_prev = ls;  // compared only: the reference is dropped at once
        ls->Release();
      }
      lit->Release();
    }
    // Diagnostic: the first draws, and every change of whether the share is there.
    static int said = 0;
    static int was = -1;
    const int now = g_part && g_part_share > 0.0f ? 1 : 0;
    if (said < 3 || now != was) {
      if (said++ < 12)
        ss_log("[hdr] particle composite draw: texture %p (s%d), rcpOverDrawDiv c%d = (%.4g, %.4g, %.4g, %.4g), AlphaPower c%d = %.4g, share %.4g", g_part,
               c->s_part, c->r_div, div[0], div[1], div[2], div[3], c->r_alpha, alpha[0], g_part_share);
      was = now;
    }
    return;
  }
  if (c->kind == K_TONEMAP_UNKNOWN) {
    g_cap_unknown = true;
    return;
  }
  if (g_ncap >= kMaxCaptures) return;
  Capture cap;
  float c2[4] = {}, curve[4] = {1, 0, 0, 0};
  IDirect3DSurface9* rt = nullptr;
  D3DSURFACE_DESC desc = {};
  D3DVIEWPORT9 vp = {};
  if (FAILED(d->GetPixelShaderConstantF(c->r_consts, cap.c100, 1)) || (!c->reinhard && FAILED(d->GetPixelShaderConstantF(c->r_consts2, c2, 1))) ||
      FAILED(d->GetViewport(&vp)) || FAILED(d->GetRenderTarget(0, &rt)) || !rt)
    return;
  cap.reinhard = c->reinhard;
  rt->GetDesc(&desc);
  rt->Release();
  if (c->r_curve >= 0) d->GetPixelShaderConstantF(c->r_curve, curve, 1);
  cap.white = c2[0];
  cap.bloom_power = curve[0];
  cap.rc = {static_cast<LONG>(vp.X), static_cast<LONG>(vp.Y), static_cast<LONG>(vp.X + vp.Width), static_cast<LONG>(vp.Y + vp.Height)};
  cap.tw = desc.Width;
  cap.th = desc.Height;
  cap.scene = Texture2D(d, c->s_scene);
  if (!cap.scene) return;
  cap.adapt = Texture2D(d, c->s_adapt);
  cap.bloom = Texture2D(d, c->s_bloom);
  g_cap[g_ncap++] = cap;
}

// ------------------------------------------------------------------------- LIGHT SOURCES: the additive re-draw
//
// A draw into the lit scene target with a blend that adds (ADD, destination ONE, source ONE or SRCALPHA) by a pixel
// shader that does not read the G-buffer (the light shaders do; a corona, a glow card, a spark sprite do not) is
// made once more right after itself into a float target of our own, with the game's depth buffer and every state as
// it is: that target then holds the light those draws added, and the scene pass lifts the pixels they made.
// The lit scene target is known from the particle composite's draw of the frame before (the texture it reads as
// the scene); SetRenderTarget (slot 37) says when it is bound.
volatile bool g_in_lit = false;       // render target 0 is the lit scene target
bool g_add_cleared = false;           // the additive target was drawn into this frame
bool g_add_failed = false;
IDirect3DSurface9* g_add_back = nullptr;  // the lit target, while a re-draw is under way
D3DVIEWPORT9 g_add_vp;
RECT g_add_sc;
uint32_t g_add_draws = 0, g_add_frames = 0;  // for the report
// Per pixel shader: 1 = may be re-drawn, 2 = reads the G-buffer. Direct-mapped by the object's address; emptied on a
// Reset and every ten seconds (an address can be given to another shader).
struct AddClass {
  IDirect3DPixelShader9* ps;
  int kind;
} g_add_class[512];

int AdditiveClass(IDirect3DDevice9* d, IDirect3DPixelShader9* ps, UINT lit_w, UINT lit_h) {
  AddClass& slot = g_add_class[(reinterpret_cast<uintptr_t>(ps) >> 4) & 511];
  if (slot.ps == ps) return slot.kind;
  int kind = 1;
  UINT size = 0;
  if (FAILED(ps->GetFunction(nullptr, &size)) || size < 8 || size > (1u << 20)) return 2;
  std::vector<DWORD> code((size + 3) / 4);
  if (FAILED(ps->GetFunction(code.data(), &size))) return 2;
  CtabEntry e[48];
  const int n = ParseCtab(code.data(), e, 48);
  for (int i = 0; i < n && kind == 1; ++i) {
    if (e[i].set != 3) continue;
    IDirect3DBaseTexture9* t = nullptr;
    if (FAILED(d->GetTexture(e[i].reg, &t)) || !t) continue;
    if (t->GetType() == D3DRTYPE_TEXTURE) {
      D3DSURFACE_DESC td = {};
      static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &td);
      // A G-buffer layer: an 8-bit colour target of the lit scene's size.
      if ((td.Usage & D3DUSAGE_RENDERTARGET) && td.Width == lit_w && td.Height == lit_h &&
          (td.Format == D3DFMT_A8R8G8B8 || td.Format == D3DFMT_X8R8G8B8))
        kind = 2;
    }
    t->Release();
  }
  slot.ps = ps;
  slot.kind = kind;
  return kind;
}

// Before a draw: whether it is one to make again into the additive target.
bool AdditiveDue(IDirect3DDevice9* d) {
  if (!g_in_lit || !g_run || g_add_failed || ss_own_drawing != 0 || !(g_lift > 0.0f) || g_test == 1) return false;
  DWORD blend = 0, dst = 0, src = 0, op = 0;
  d->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
  if (!blend) return false;
  d->GetRenderState(D3DRS_DESTBLEND, &dst);
  if (dst != D3DBLEND_ONE) return false;
  d->GetRenderState(D3DRS_SRCBLEND, &src);
  d->GetRenderState(D3DRS_BLENDOP, &op);
  if ((src != D3DBLEND_ONE && src != D3DBLEND_SRCALPHA) || op != D3DBLENDOP_ADD || d != GameDevice()) return false;
  IDirect3DPixelShader9* ps = nullptr;
  if (FAILED(d->GetPixelShader(&ps)) || !ps) return false;
  ps->Release();
  IDirect3DSurface9* rt = nullptr;
  D3DSURFACE_DESC desc = {};
  if (FAILED(d->GetRenderTarget(0, &rt)) || !rt) return false;
  rt->GetDesc(&desc);
  rt->Release();
  return AdditiveClass(d, ps, desc.Width, desc.Height) == 1;
}

// After the draw: our target in place of the lit scene's, the viewport and the scissor kept (setting a target
// resets both). False: the draw is not made again.
bool AdditiveBegin(IDirect3DDevice9* d) {
  D3DSURFACE_DESC desc = {};
  IDirect3DSurface9* target = nullptr;
  if (FAILED(d->GetRenderTarget(0, &g_add_back)) || !g_add_back) return false;
  bool ok = SUCCEEDED(g_add_back->GetDesc(&desc)) && desc.MultiSampleType == D3DMULTISAMPLE_NONE;
  if (ok && (!g_r.add || g_r.aw != desc.Width || g_r.ah != desc.Height)) {
    SafeRelease(g_r.add);
    ok = g_r.dev == d && MakeTarget(d, desc.Width, desc.Height, D3DFMT_A16B16G16R16F, &g_r.add);
    g_r.aw = desc.Width, g_r.ah = desc.Height;
    g_add_cleared = false;
    if (ok) ss_log("[hdr] light sources: the additive target made at %ux%u (A16B16G16R16F)", desc.Width, desc.Height);
  }
  ok = ok && SUCCEEDED(g_r.add->GetSurfaceLevel(0, &target)) && target && SUCCEEDED(d->GetViewport(&g_add_vp)) &&
       SUCCEEDED(d->GetScissorRect(&g_add_sc));
  ++ss_own_drawing;
  if (ok && FAILED(d->SetRenderTarget(0, target))) {
    ok = false;
    g_add_failed = true;
    ss_log("[hdr] light sources: the additive target cannot be bound beside the game's depth buffer; no additive lift");
  }
  if (ok) {
    if (!g_add_cleared) {
      d->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
      g_add_cleared = true;
      ++g_add_frames;
    }
    d->SetViewport(&g_add_vp);
    d->SetScissorRect(&g_add_sc);
    ++g_add_draws;
  } else {
    --ss_own_drawing;
    SafeRelease(g_add_back);
  }
  SafeRelease(target);
  return ok;
}

void AdditiveEnd(IDirect3DDevice9* d) {
  d->SetRenderTarget(0, g_add_back);
  d->SetViewport(&g_add_vp);
  d->SetScissorRect(&g_add_sc);
  SafeRelease(g_add_back);
  --ss_own_drawing;
}

extern uint32_t g_lamp_draws, g_lamp_frames, g_tag_draws, g_tag_frames, g_tag_quads;
void AdditiveReport() {
  if (g_add_frames) ss_log("[hdr] light sources: %u additive draws made again in %u frames of the last 3600 (%.1f a frame)", g_add_draws, g_add_frames,
                           double(g_add_draws) / g_add_frames);
  if (g_lamp_frames) ss_log("[hdr] light sources: %u lamp draws made again in %u frames of the last 3600 (%.1f a frame)", g_lamp_draws, g_lamp_frames,
                            double(g_lamp_draws) / g_lamp_frames);
  if (g_tag_frames)
    ss_log("[hdr] light sources: %u lamp draws tagged in the stencil in %u frames of the last 3600 (%.1f a frame), %.1f quads a frame; none made again",
           g_tag_draws, g_tag_frames, double(g_tag_draws) / g_tag_frames, double(g_tag_quads) / g_tag_frames);
  g_tag_draws = g_tag_frames = g_tag_quads = 0;
  g_lamp_draws = g_lamp_frames = 0;
  g_add_draws = g_add_frames = 0;
  memset(g_add_class, 0, sizeof g_add_class);  // an address may by now be another shader's
}

// ------------------------------------------------------------------------------- LIGHT SOURCES: the lamps
//
// A G-buffer draw (a second colour target is bound) by a self-lit material's pixel shader (classed at creation by
// its emissive constant's name, kinds 1..3 above) is made once more right after itself into the lamps' mask: target
// 0 (the albedo, as the G-buffer's own code) and target 2 (the material's own light), a depth buffer of our own (an
// INTZ texture, so the scene pass can read it), no blend: the nearest lamp draw is what each pixel keeps. The light
// is the shader's own G-buffer output (albedo x amount x c[r_scale].z + albedo x ambient, then x / (x + c[r_soft].x))
// with the two constants changed for the re-draw: the scale to kEmitScale / the reference amount, the softening to
// kEmitScale, so the target holds x / (x + 1) with x = albedo x amount / reference, per pixel (an emissive texture's
// alpha, a pulse, a vertex colour included) and the ambient term reduced to nothing. The scene pass divides by the
// albedo: the pixel's amount over the reference. The reference: a lens's full amount 2 (a lamp that is off or dim
// gets little); any other material's own constant over kLampKnee, so only what varies per pixel is left and the
// weight is full from a quarter of the constant up (SSPC-4: with a fixed 0.6 for all of them the lift followed each
// material's constant, 0.1 to 1.0 on Airport, times its emissive texture's alpha, neither of which the sunlit
// picture shows much of; a sign whose two meshes carry different self-lit materials changed brightness at the
// hand-over). A draw whose amount is under an eighth of the full one (2, 0.6) is not made again. SetPixelShader
// (slot 107) says when such a shader is current.
const Classed* volatile g_cur_emissive = nullptr;  // the game's current pixel shader, when it is a self-lit one
bool g_lamp_failed = false;
uint32_t g_lamp_kinds = 7;       // SS_HDR_EMISSIVE_KINDS: bit 0 lenses, 1 Emissive_n and the like, 2 Emissive_Alpha_n
float g_lamp_gain = 1.0f;        // SS_HDR_EMISSIVE_GAIN
uint32_t g_lamp_draws = 0, g_lamp_frames = 0;
constexpr float kEmitScale = 4096.0f;
constexpr float kLampKnee = 4.0f;  // a material lights a pixel fully from 1 / kLampKnee of its amount
const D3DFORMAT kIntz = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));
const D3DRENDERSTATETYPE kLampStates[] = {D3DRS_ALPHABLENDENABLE, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_COLORWRITEENABLE, D3DRS_COLORWRITEENABLE1,
                                          D3DRS_COLORWRITEENABLE2, D3DRS_STENCILENABLE, D3DRS_SRGBWRITEENABLE,
                                          D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_SRCBLENDALPHA, D3DRS_DESTBLENDALPHA,
                                          D3DRS_BLENDOPALPHA, D3DRS_BLENDFACTOR};
int g_probe_id = 0;  // hdr_light_log: the id the lamp draw being made again leaves in the mask's alpha (0: none)
struct LampSave {
  IDirect3DSurface9* rt[4];
  IDirect3DSurface9* ds;
  D3DVIEWPORT9 vp;
  RECT sc;
  DWORD states[sizeof kLampStates / sizeof kLampStates[0]];
  int r_scale, r_soft;
  float scale[4], soft[4];
} g_lamp_save;

// The lamps' lift fades out with distance (issue #10: far signs and the terminal's lamps stood out too bright): whole
// up to kLampFadeFrom, gone at kLampFadeTo, in the camera's units (metres; the props' LOD steps are 150 and 400).
constexpr float kLampFadeFrom = 100.0f, kLampFadeTo = 400.0f;  // (near opponents' tail lights keep their lift)
float LampWhites() { return g_lift > 0.0f ? 2.5f * g_lamp_gain * g_lift / 3.0f : 0.0f; }

// Before a draw: the reference amount for a lamp draw to make again into the mask, or 0. `why` (hdr_light_log): '+'
// made again, 'o' the path is off, 'k' the kind is off, 's' another shader is bound, 't' no second colour target (the
// environment map), 'c' the constant cannot be read, 'a' the amount is under an eighth of the full one.
struct LampWhy {
  char why = 'o';
  float amount = 0.0f;
};
float LampDue(IDirect3DDevice9* d, LampWhy* w) {
  const Classed* c = g_cur_emissive;
  if (!c || !g_run || g_lamp_failed || ss_own_drawing != 0 || !(LampWhites() > 0.0f) || d != GameDevice() || g_test == 2) return 0.0f;
  w->why = 'k';
  if (!(g_lamp_kinds & (1u << (c->e_kind - 1)))) return 0.0f;
  w->why = 's';
  IDirect3DPixelShader9* ps = nullptr;
  if (FAILED(d->GetPixelShader(&ps)) || !ps) return 0.0f;
  ps->Release();
  if (Original(ps) != c->ps) return 0.0f;  // (a state block put another shader back since the last SetPixelShader)
  w->why = 't';
  IDirect3DSurface9* rt1 = nullptr;
  if (FAILED(d->GetRenderTarget(1, &rt1)) || !rt1) return 0.0f;  // not the G-buffer pass (the environment map's)
  rt1->Release();
  w->why = 'c';
  float v[4] = {}, index[4] = {};
  if (FAILED(d->GetPixelShaderConstantF(c->r_emissive, v, 1))) return 0.0f;
  int lane = 0;
  float full = 0.6f;
  if (c->e_kind == 1) {
    if (c->r_index >= 0) d->GetPixelShaderConstantF(c->r_index, index, 1);
    lane = index[0] >= 0.5f ? std::min(static_cast<int>(index[0] + 0.5f), 3) : 0;
    full = 2.0f;
  }
  const float amount = v[lane];
  w->amount = amount;
  w->why = 'a';
  if (!(amount > 0.0f) || !std::isfinite(amount) || amount / full < 0.125f) return 0.0f;
  w->why = '+';
  return c->e_kind == 1 ? full : amount / kLampKnee;
}

bool LampBegin(IDirect3DDevice9* d, float full) {
  LampSave& sv = g_lamp_save;
  memset(&sv, 0, sizeof sv);
  const Classed* c = g_cur_emissive;
  D3DSURFACE_DESC desc = {};
  IDirect3DSurface9 *mask = nullptr, *emit = nullptr, *depth = nullptr;
  for (DWORD i = 0; i < 4; ++i) d->GetRenderTarget(i, &sv.rt[i]);
  d->GetDepthStencilSurface(&sv.ds);
  bool ok = c && sv.rt[0] && sv.ds && SUCCEEDED(sv.rt[0]->GetDesc(&desc)) && desc.MultiSampleType == D3DMULTISAMPLE_NONE && g_r.dev == d;
  if (ok && !g_scene_depth) {  // the frame's first lamp draw: the G-buffer's depth must be a texture we can read
    D3DSURFACE_DESC dd = {};
    ok = SUCCEEDED(sv.ds->GetDesc(&dd)) && dd.Format == kIntz && dd.Width == desc.Width && dd.Height == desc.Height &&
         SUCCEEDED(sv.ds->GetContainer(kIidTexture9, reinterpret_cast<void**>(&g_scene_depth))) && g_scene_depth;
    if (!ok) {
      g_lamp_failed = true;
      ss_log("[hdr] light sources: the G-buffer's depth is not a readable texture (format %d, %ux%u); no lamps", static_cast<int>(dd.Format), dd.Width, dd.Height);
    }
  }
  if (ok && (!g_r.lamp_mask || !g_r.lamp_emit || !g_r.lamp_depth || g_r.lw != desc.Width || g_r.lh != desc.Height)) {
    SafeRelease(g_r.lamp_mask);
    SafeRelease(g_r.lamp_emit);
    SafeRelease(g_r.lamp_depth);
    ok = MakeTarget(d, desc.Width, desc.Height, D3DFMT_A8R8G8B8, &g_r.lamp_mask) && MakeTarget(d, desc.Width, desc.Height, D3DFMT_A8R8G8B8, &g_r.lamp_emit) &&
         SUCCEEDED(d->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_DEPTHSTENCIL, kIntz, D3DPOOL_DEFAULT, &g_r.lamp_depth, nullptr));
    g_r.lw = desc.Width, g_r.lh = desc.Height;
    g_lamp_cleared = false;
    if (ok) ss_log("[hdr] light sources: the lamps' mask, light and depth made at %ux%u", desc.Width, desc.Height);
    else g_lamp_failed = true, ss_log("[hdr] light sources: the lamps' targets or their depth texture (INTZ) could not be made; no lamps");
  }
  ok = ok && SUCCEEDED(g_r.lamp_mask->GetSurfaceLevel(0, &mask)) && mask && SUCCEEDED(g_r.lamp_emit->GetSurfaceLevel(0, &emit)) && emit &&
       SUCCEEDED(g_r.lamp_depth->GetSurfaceLevel(0, &depth)) && depth && SUCCEEDED(d->GetViewport(&sv.vp)) && SUCCEEDED(d->GetScissorRect(&sv.sc)) &&
       SUCCEEDED(d->GetPixelShaderConstantF(c->r_scale, sv.scale, 1)) && SUCCEEDED(d->GetPixelShaderConstantF(c->r_soft, sv.soft, 1));
  if (ok) {
    sv.r_scale = c->r_scale;
    sv.r_soft = c->r_soft;
    const int nstates = sizeof kLampStates / sizeof kLampStates[0];
    for (int i = 0; i < nstates; ++i) d->GetRenderState(kLampStates[i], &sv.states[i]);
    ++ss_own_drawing;
    for (DWORD i = 1; i < 4; ++i)
      if (sv.rt[i]) d->SetRenderTarget(i, nullptr);
    if (FAILED(d->SetRenderTarget(0, mask)) || FAILED(d->SetRenderTarget(2, emit)) || FAILED(d->SetDepthStencilSurface(depth))) {
      g_lamp_failed = true;
      ss_log("[hdr] light sources: the lamps' targets cannot be bound; no lamps");
      d->SetRenderTarget(0, sv.rt[0]);
      for (DWORD i = 1; i < 4; ++i) d->SetRenderTarget(i, sv.rt[i]);
      d->SetDepthStencilSurface(sv.ds);
      d->SetViewport(&sv.vp);
      d->SetScissorRect(&sv.sc);
      --ss_own_drawing;
      ok = false;
    }
  }
  if (ok) {
    if (!g_lamp_cleared) {
      d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, g_probe ? 0xFF000000u : 0u, 1.0f, 0);  // both targets
      g_lamp_cleared = true;
      ++g_lamp_frames;
    }
    d->SetViewport(&sv.vp);
    d->SetScissorRect(&sv.sc);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    d->SetRenderState(D3DRS_COLORWRITEENABLE1, 0xF);
    d->SetRenderState(D3DRS_COLORWRITEENABLE2, 0xF);
    d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    if (g_probe && g_probe_id > 0) {  // the colour as without a blend; the alpha (cleared to 1) times 1 - id / 64
      d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
      d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
      d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
      d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);
      d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
      d->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ZERO);
      d->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_BLENDFACTOR);
      d->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
      d->SetRenderState(D3DRS_BLENDFACTOR, D3DCOLOR_ARGB(255 - 4 * g_probe_id, 255, 255, 255));
    }
    const float scale[4] = {sv.scale[0], sv.scale[1], kEmitScale / full, sv.scale[3]};
    const float soft[4] = {kEmitScale, sv.soft[1], sv.soft[2], sv.soft[3]};
    d->SetPixelShaderConstantF(sv.r_scale, scale, 1);
    d->SetPixelShaderConstantF(sv.r_soft, soft, 1);
    ++g_lamp_draws;
  } else {
    for (auto& r : sv.rt) SafeRelease(r);
    SafeRelease(sv.ds);
  }
  SafeRelease(mask);
  SafeRelease(emit);
  SafeRelease(depth);
  return ok;
}

void LampEnd(IDirect3DDevice9* d) {
  LampSave& sv = g_lamp_save;
  d->SetPixelShaderConstantF(sv.r_scale, sv.scale, 1);
  d->SetPixelShaderConstantF(sv.r_soft, sv.soft, 1);
  const int nstates = sizeof kLampStates / sizeof kLampStates[0];
  for (int i = 0; i < nstates; ++i) d->SetRenderState(kLampStates[i], sv.states[i]);
  d->SetRenderTarget(0, sv.rt[0]);
  for (DWORD i = 1; i < 4; ++i) d->SetRenderTarget(i, sv.rt[i]);
  d->SetDepthStencilSurface(sv.ds);
  d->SetViewport(&sv.vp);
  d->SetScissorRect(&sv.sc);
  for (auto& r : sv.rt) SafeRelease(r);
  SafeRelease(sv.ds);
  --ss_own_drawing;
}

using SetPixelShaderFn = HRESULT(__stdcall*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
SetPixelShaderFn o_set_pixel_shader = nullptr;
HRESULT __attribute__((stdcall, force_align_arg_pointer)) SetPixelShaderHook(IDirect3DDevice9* d, IDirect3DPixelShader9* ps) {
  if (ss_own_drawing == 0) {
    const Classed* c = g_run ? LookupClass(ps) : nullptr;
    g_cur_emissive = c && c->kind == K_EMISSIVE ? c : nullptr;
  }
  return o_set_pixel_shader(d, ps);
}

// ---- hdr_light_log (a diagnostic, SSPC-4). The frame's self-lit and additive draws, by shader and by what was done
// with them, and ten times a second the scene pass's own terms in a window in the middle of the picture.
uint32_t g_cover_last[254];  // hdr_light_log = 2: the shaders' hashes by number, as the pass that just ended had them
int g_ncover_last = 0;
bool g_tag_failed = false;  // the lamps' stencil tag cannot be had on this device (see "the lamps by a stencil tag")
bool OldLampPath() { return g_test == 5 || g_tag_failed || g_probe_cover; }

struct ProbeShader {
  uint32_t hash;
  bool emissive;  // its constant table names an emissive constant
};
std::unordered_map<IDirect3DPixelShader9*, ProbeShader> g_probe_shaders;  // by the game's shader; emptied on a Reset
struct ProbeDraw {
  uint32_t hash;
  char path, why;  // path: 'L' a classed self-lit draw, 'U' a self-lit G-buffer draw that is not classed, 'A' an
                   // additive draw into the lit target, 'F' a blended (not additive) self-lit draw there
  int kind;
  float amount, ref;
  int lod;  // lod_smoothing's share of the draw in percent, -1: no fade
  bool old;
  int n;
};
constexpr int kProbeDraws = 63;
ProbeDraw g_pd[kProbeDraws];
int g_npd = 0, g_probe_lights = 0, g_probe_lines = 0;
constexpr int kProbeMaxLines = 9000;

ProbeShader ProbeShaderOf(IDirect3DPixelShader9* ps) {
  ps = Original(ps);
  auto it = g_probe_shaders.find(ps);
  if (it != g_probe_shaders.end()) return it->second;
  ProbeShader p = {0, false};
  UINT size = 0;
  if (SUCCEEDED(ps->GetFunction(nullptr, &size)) && size >= 8 && size < (1u << 20)) {
    std::vector<DWORD> code((size + 3) / 4);
    if (SUCCEEDED(ps->GetFunction(code.data(), &size))) {
      const uint64_t h = CodeHash(code.data(), size);
      p.hash = static_cast<uint32_t>(h ^ (h >> 32));
      const std::string names = CensusNames(code.data(), false);
      p.emissive = names.find("missive") != std::string::npos;
      static int said = 0;
      if (said++ < 400 && g_probe_lines++ < kProbeMaxLines) ss_log("[light] shader %08x: %s", p.hash, names.c_str());
    }
  }
  if (g_probe_shaders.size() > 4096) g_probe_shaders.clear();
  g_probe_shaders[ps] = p;
  return p;
}

// One draw into the frame's list; its place + 1 (the id a lamp draw leaves in the mask), 0 when the list is full.
int ProbeNote(uint32_t hash, char path, char why, int kind, float amount, float ref) {
  int old = 0;
  const float share = ss_lodfade_drawing(&old);
  const int lod = share < 0.0f ? -1 : static_cast<int>(share * 100.0f + 0.5f);
  for (int i = 0; i < g_npd; ++i) {
    ProbeDraw& p = g_pd[i];
    if (p.hash == hash && p.path == path && p.why == why && p.amount == amount && p.lod == lod && p.old == (old != 0)) {
      ++p.n;
      return i + 1;
    }
  }
  if (g_npd >= kProbeDraws) return 0;
  g_pd[g_npd] = {hash, path, why, kind, amount, ref, lod, old != 0, 1};
  return ++g_npd;
}

// A draw while the lit scene target is bound: additive (made again or not), or a blended self-lit one.
void ProbeLit(IDirect3DDevice9* d, bool again) {
  if (!g_run || ss_own_drawing != 0 || d != GameDevice()) return;
  DWORD blend = 0, dst = 0;
  d->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
  if (!blend) return;
  d->GetRenderState(D3DRS_DESTBLEND, &dst);
  if (dst == D3DBLEND_ONE && !again) {  // the light shaders (they read the G-buffer), or the path is off
    ++g_probe_lights;
    return;
  }
  IDirect3DPixelShader9* ps = nullptr;
  if (FAILED(d->GetPixelShader(&ps)) || !ps) return;
  ps->Release();
  const ProbeShader p = ProbeShaderOf(ps);
  if (dst == D3DBLEND_ONE) ProbeNote(p.hash, 'A', '+', 0, 0.0f, 0.0f);
  else if (p.emissive) ProbeNote(p.hash, 'F', 'b', 0, 0.0f, 0.0f);
}

// A self-lit G-buffer draw whose shader is not classed (so it is never a lamp).
void ProbeUnclassed(IDirect3DDevice9* d) {
  if (!g_run || ss_own_drawing != 0 || d != GameDevice()) return;
  IDirect3DPixelShader9* ps = nullptr;
  if (FAILED(d->GetPixelShader(&ps)) || !ps) return;
  ps->Release();
  const ProbeShader p = ProbeShaderOf(ps);
  if (!p.emissive) return;
  IDirect3DSurface9* rt1 = nullptr;
  if (FAILED(d->GetRenderTarget(1, &rt1)) || !rt1) return;
  rt1->Release();
  ProbeNote(p.hash, 'U', 'u', 0, 0.0f, 0.0f);
}

int ProbeLamp(IDirect3DDevice9* d, const LampWhy& w, float ref) {
  const Classed* c = g_cur_emissive;
  if (!c || w.why == 'o' || w.why == 't') return 0;  // (the environment map's draws are not listed)
  return ProbeNote(ProbeShaderOf(c->ps).hash, 'L', w.why, c->e_kind, w.amount, ref);
}

float HalfToFloat(uint16_t h) {
  const int e = (h >> 10) & 31, m = h & 1023;
  float v;
  if (e == 0) v = ldexpf(static_cast<float>(m), -24);
  else if (e == 31) v = m ? 0.0f : 65504.0f;
  else v = ldexpf(static_cast<float>(m + 1024), e - 25);
  return (h & 0x8000) ? -v : v;
}

constexpr int kProbeN = 64;
constexpr float kProbeLo = 0.2f, kProbeHi = 0.8f;  // the window: the middle 60 % of the picture each way

// Inside the scene pass, right after its draw (its samplers and constants are still set; `k` are those constants,
// `back` its target): the window drawn four times into a small target (the extra light, then the three sets of
// terms), read back, and put into the log with the frame's draws.
void ProbeWindow(IDirect3DDevice9* d, const float* k, IDirect3DSurface9* back) {
  static DWORD last = 0;
  const DWORD now = GetTickCount();
  if (now - last < 100 || g_probe_lines >= kProbeMaxLines) return;
  last = now;
  if (!g_r.probe_rt && !MakeTarget(d, kProbeN, kProbeN * 5, D3DFMT_A16B16G16R16F, &g_r.probe_rt)) return;
  if (!g_r.probe_sys &&
      FAILED(d->CreateOffscreenPlainSurface(kProbeN, kProbeN * 5, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &g_r.probe_sys, nullptr)))
    return;
  IDirect3DSurface9* rt = nullptr;
  if (FAILED(g_r.probe_rt->GetSurfaceLevel(0, &rt)) || !rt) return;
  d->SetRenderTarget(0, rt);
  float kk[20];
  memcpy(kk, k, sizeof kk);
  const float o = 1.0f / kProbeN;
  const Vertex q[4] = {{-1 - o, 1 + o, 0, 1, kProbeLo, kProbeLo}, {1 - o, 1 + o, 0, 1, kProbeHi, kProbeLo},
                       {-1 - o, -1 + o, 0, 1, kProbeLo, kProbeHi}, {1 - o, -1 + o, 0, 1, kProbeHi, kProbeHi}};
  for (int mode = 0; mode < 5; ++mode) {
    const D3DVIEWPORT9 vp = {0, static_cast<DWORD>(mode * kProbeN), kProbeN, kProbeN, 0.0f, 1.0f};
    d->SetViewport(&vp);
    kk[18] = static_cast<float>(mode);
    d->SetPixelShaderConstantF(0, kk, 5);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vertex));
  }
  d->SetPixelShaderConstantF(0, k, 5);
  d->SetRenderTarget(0, back);
  D3DLOCKED_RECT lr = {};
  const bool ok = SUCCEEDED(d->GetRenderTargetData(rt, g_r.probe_sys)) && SUCCEEDED(g_r.probe_sys->LockRect(&lr, nullptr, D3DLOCK_READONLY));
  rt->Release();
  if (!ok) {
    static int said = 0;
    if (said++ < 3) ss_log("[light] the window could not be read back");
    return;
  }
  struct Id {
    int n, ok, nearer;  // nearer: the lamp's depth is in front of the scene's by more than is allowed
    float share, gate, add, extra, lamp, off;  // off: the failing samples' |difference| in allowed differences
    float follow, blur, away;  // the stencil tag: the lift's cuts (issue #10), summed over the samples that have a lift
    int lifted, follow_low, follow_half;  // ... those samples, and of them the ones with follow under 0.9 and under 0.5
  } ids[64] = {};
  struct Pair {
    int lamp, cover, n;
    float off;
  } pairs[48] = {};
  int npairs = 0;
  int nl = 0, nok = 0, na = 0, np = 0;
  float sum = 0, top = 0, lamp_extra = 0, add_tag = 0, add_extra = 0, add_light = 0, sourced = 0, rel = 0;
  auto at = [&](int mode, int x, int y, float* v) {
    const uint16_t* t = reinterpret_cast<const uint16_t*>(static_cast<const unsigned char*>(lr.pBits) + (mode * kProbeN + y) * lr.Pitch) + x * 4;
    for (int i = 0; i < 4; ++i) v[i] = HalfToFloat(t[i]);
  };
  for (int y = 0; y < kProbeN; ++y)
    for (int x = 0; x < kProbeN; ++x) {
      float e[4], a[4], b[4], c[4];
      at(0, x, y, e), at(1, x, y, a), at(2, x, y, b), at(3, x, y, c);
      const float ex = 0.2126f * e[0] + 0.7152f * e[1] + 0.0722f * e[2];
      sum += ex;
      top = std::max(top, ex);
      if (b[0] > 0.5f) {
        ++nl;
        const bool same = !OldLampPath() || fabsf(b[1]) <= 1.0f;
        nok += same;
        lamp_extra += ex;
        Id& id = ids[OldLampPath() ? std::min(std::max(static_cast<int>((1.0f - b[2]) * 255.0f / 4.0f + 0.5f), 0), 63) : 0];
        if (!same) id.nearer += b[1] < 0.0f, id.off += fabsf(b[1]);
        if (g_probe_cover) {  // the lamp's draw and the shader of the surface the G-buffer shows there
          float cv[4];
          at(4, x, y, cv);
          const int lamp = static_cast<int>(&id - ids), cov = same ? -1 : static_cast<int>(cv[0] * 255.0f + 0.5f);
          int k = 0;
          while (k < npairs && (pairs[k].lamp != lamp || pairs[k].cover != cov)) ++k;
          if (k == npairs && npairs < 48) pairs[npairs++] = {lamp, cov, 0, 0.0f};
          if (k < 48) ++pairs[k].n, pairs[k].off += b[1];
        }
        ++id.n, id.ok += same, id.share += a[0], id.gate += a[1], id.add += a[2], id.extra += ex, id.lamp += c[1];
        if (!OldLampPath() && b[1] >= 0.0f)
          ++id.lifted, id.follow += b[1], id.blur += b[2], id.away += b[3], id.follow_low += b[1] < 0.9f, id.follow_half += b[1] < 0.5f;
      }
      if (a[2] > 0.02f) ++na, add_tag += a[2], add_extra += ex, add_light += c[3], sourced += c[2], rel += c[0];
      if (a[3] > 0.02f) ++np;
    }
  g_r.probe_sys->UnlockRect();
  // The frame's draws, when they are not the last logged ones.
  std::string draws;
  for (int i = 0; i < g_npd; ++i) {
    const ProbeDraw& p = g_pd[i];
    char t[120], lod[24] = "";
    if (p.lod >= 0) snprintf(lod, sizeof lod, " lod %s %d%%", p.old ? "old" : "new", p.lod);
    if (p.path == 'L') snprintf(t, sizeof t, " | #%d %08x L%c k%d amt %.3g ref %.3g%s x%d", i + 1, p.hash, p.why, p.kind, p.amount, p.ref, lod, p.n);
    else snprintf(t, sizeof t, " | %08x %c%c%s x%d", p.hash, p.path, p.why, lod, p.n);
    draws += t;
  }
  static std::string last_draws;
  const float all = static_cast<float>(kProbeN * kProbeN);
  const bool quiet = nl == 0 && na == 0 && top < 0.05f;
  static bool was_quiet = false;
  if (quiet && was_quiet && draws == last_draws) return;
  was_quiet = quiet;
  if (draws != last_draws) {
    ss_log("[light] f%u draws (%d light-shader draws not listed)%s", g_frame, g_probe_lights, draws.empty() ? " | none" : draws.c_str());
    last_draws = draws;
    ++g_probe_lines;
  }
  ss_log("[light] f%u test %ld lift %.0f: extra mean %.3f max %.2f; lamp px %d (depth ok %d) extra %.2f; additive px %d tag %.2f extra %.2f "
         "sourced %.2f light %.2f L/L1 %.2f; particle px %d",
         g_frame, static_cast<long>(g_test), static_cast<float>(g_lift), sum / all, top, nl, nok, nl ? lamp_extra / nl : 0.0f, na, na ? add_tag / na : 0.0f,
         na ? add_extra / na : 0.0f, na ? sourced / na : 0.0f, na ? add_light / na : 0.0f, na ? rel / na : 0.0f, np);
  ++g_probe_lines;
  for (int i = 0; i < 64; ++i) {
    const Id& id = ids[i];
    if (id.n < 3) continue;
    const float n = static_cast<float>(id.n);
    ss_log("[light]   #%d%s: px %d depth ok %d (of the others %d with the lamp in front, off by %.1f x allowed) share %.2f gate %.2f add %.2f lamp %.2f extra %.2f",
           i, !OldLampPath() ? " (all the tagged lamps)" : i >= 1 && i <= g_npd ? "" : " (mixed)", id.n, id.ok, id.nearer, id.n > id.ok ? id.off / static_cast<float>(id.n - id.ok) : 0.0f, id.share / n,
           id.gate / n, id.add / n, id.lamp / n, id.extra / n);
    ++g_probe_lines;
    if (!OldLampPath() && id.lifted) {
      const float m = static_cast<float>(id.lifted);
      ss_log("[light]   #%d follow %.2f (blur %.2f, distance %.2f) over %d lifted px; under 0.9: %d px, under 0.5: %d px", i, id.follow / m, id.blur / m, id.away / m,
             id.lifted, id.follow_low, id.follow_half);
      ++g_probe_lines;
    }
  }
  for (int k = 0; k < npairs; ++k) {
    const Pair& pr = pairs[k];
    if (pr.n < 3 || pr.cover < 0) continue;
    const uint32_t lamp = pr.lamp >= 1 && pr.lamp <= g_npd ? g_pd[pr.lamp - 1].hash : 0;
    ss_log("[light]   cover: lamp #%d %08x fails under shader %08x (number %d): px %d, the lamp's depth off by %+.1f x allowed", pr.lamp, lamp,
           pr.cover >= 1 && pr.cover <= g_ncover_last ? g_cover_last[pr.cover - 1] : 0u, pr.cover, pr.n, pr.off / static_cast<float>(pr.n));
    ++g_probe_lines;
  }
  if (g_probe_lines >= kProbeMaxLines) ss_log("[light] the log's share is used up (%d lines); no more this run", kProbeMaxLines);
}

// ------------------------------------------------------------------- LIGHT SOURCES: the lamps by a stencil tag
//
// The lamps without a second draw and without a depth comparison (SSPC-4, SSPC-13). The re-draw above compared its
// own depth with the G-buffer's per pixel; on the Deck and on the test machine that comparison fails on most of a
// lamp's pixels in a race (16 % pass; the more of the screen a lamp covers the fewer), which is the signs' bright,
// dotted, darker. Here the game's own G-buffer draw says which pixels are a lamp's:
//   * While a second colour target is bound (the G-buffer pass) the stencil is ours: the title clears it to 255
//     with the depth at the pass's start and to 0 at the start of the light pass, and draws the whole pass with the
//     stencil off (apitrace out-t8, frame 8100). Every draw that writes depth replaces the stencil where it passes
//     the depth test: a self-lit draw with its class's number (1 ..), any other with 0. A draw that does not write
//     depth (the decals) leaves it. So a pixel holds a class's number exactly where a lamp is the surface the
//     G-buffer shows: the title's own depth test decides, and texkill (the dither, an alpha test) with it.
//   * When the pass ends (the second target is taken off) one screen quad per class in use, with the stencil test
//     EQUAL, writes the lamps' target: rgb the G-buffer's albedo code, alpha the weight. No geometry is drawn again.
//   * A class is a weight: a lens's amount over 2 (as before), 1 for a material that lights its whole surface
//     (Emissive_n, emissive_n). A material that lights by a per-pixel factor (Emissive_Alpha_n: the emissive
//     texture's alpha; Diffuse_Emissive_n: the albedo's alpha; Emissive_Pulse_n: the pulse) gets its weight from the
//     G-buffer's third target, which holds r / (r + soft), r = albedo x (light + amount x scale x factor): the
//     weight is (r / albedo - bound) / (amount x scale / kLampKnee), 0 to 1, where `bound` is the most the light term
//     can be (the hemisphere ambient's and the second light's constants of the pixel shader, the radiosity
//     constant of the vertex shader, each at its largest channel). A pixel the material does not light is at or under
//     the bound: no lift (the telephone wires). Such a material with a reflection in the same target (a cube map)
//     has no bound and is left out.
// The stencil states are read before they are changed and put back before the pass's last call goes on, so the
// title's own state cache stays true. Blocked (the depth surface has no stencil, the quad's shader fails): the
// re-draw above runs instead (g_tag_failed), as with LIGHT SOURCES TEST = LAMPS BY RE-DRAW.
extern const char kTagPs[] = R"(
sampler2D g0 : register(s0);  // the G-buffer's first target: the albedo's square root
sampler2D g2 : register(s1);  // its third target: r / (r + soft)
float4 p : register(c0);      // x: the class's weight, y: 1 = the weight comes from the third target, z: the light's bound, w: the ramp
float4 q : register(c1);      // x: soft
static const float3 kLuma = float3(0.2125, 0.7154, 0.0721);
float4 main(float2 uv : TEXCOORD0) : COLOR {
  float4 a = tex2Dlod(g0, float4(uv, 0, 0));
  float w = p.x;
  if (p.y > 0.5) {
    float3 t = tex2Dlod(g2, float4(uv, 0, 0)).rgb;
    float3 r = q.x * t / max(1.0 - t, 1.0 / 255.0);
    float la = dot(a.rgb * a.rgb, kLuma);
    w = la > 0.002 ? saturate((dot(r, kLuma) / la - p.z) / max(p.w, 0.01)) : 0.0;
  }
  return float4(a.rgb, w);
}
)";

struct TagClass {
  bool decode;
  float w, bound, ramp;
};
constexpr int kMaxTags = 254;
TagClass g_tags[kMaxTags];
int g_ntags = 0;
bool g_gbuf = false;          // a second colour target is bound
bool g_tag_on = false;        // the stencil states are ours
int g_tag_ref = -1;           // what the stencil does now: -1 off, else the number it writes
bool g_tag_resolved = false;  // this frame's lamps' target is written
float g_tag_soft = 0.1f;      // sslighting_reflectionConstants.x as last read
uint32_t g_tag_draws = 0, g_tag_frames = 0, g_tag_quads = 0;
const D3DRENDERSTATETYPE kTagStates[] = {D3DRS_STENCILENABLE, D3DRS_STENCILFUNC, D3DRS_STENCILREF, D3DRS_STENCILMASK, D3DRS_STENCILWRITEMASK,
                                         D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL, D3DRS_STENCILPASS, D3DRS_TWOSIDEDSTENCILMODE};
DWORD g_tag_saved[sizeof kTagStates / sizeof kTagStates[0]];
std::unordered_map<IDirect3DVertexShader9*, int> g_vs_radiosity;  // the vertex shader's sslighting_radiosityTweaks register (-1: none)


// The most the light term of the current self-lit draw can be (see above).
float LightBound(IDirect3DDevice9* d, const Classed* c) {
  auto top = [&](int reg) {
    float v[4] = {};
    if (reg < 0 || FAILED(d->GetPixelShaderConstantF(reg, v, 1))) return 0.0f;
    return std::max(std::max(v[0], v[1]), std::max(v[2], 0.0f));
  };
  float bound = top(c->r_amb) + std::max(top(c->r_sky), top(c->r_gnd)) + top(c->r_dif2);
  IDirect3DVertexShader9* vs = nullptr;
  if (SUCCEEDED(d->GetVertexShader(&vs)) && vs) {
    vs->Release();  // compared only
    auto it = g_vs_radiosity.find(vs);
    if (it == g_vs_radiosity.end()) {
      int reg = -1;
      UINT size = 0;
      if (SUCCEEDED(vs->GetFunction(nullptr, &size)) && size >= 8 && size < (1u << 20)) {
        std::vector<DWORD> code((size + 3) / 4);
        CtabEntry e[64];
        if (SUCCEEDED(vs->GetFunction(code.data(), &size))) {
          const int n = ParseCtab(code.data(), e, 64, true);
          for (int i = 0; i < n; ++i)
            if (e[i].set == 2 && !strcmp(e[i].name, "sslighting_radiosityTweaks")) reg = e[i].reg;
        }
      }
      if (g_vs_radiosity.size() > 4096) g_vs_radiosity.clear();
      it = g_vs_radiosity.emplace(vs, reg).first;
    }
    float v[4] = {};
    if (it->second >= 0 && SUCCEEDED(d->GetVertexShaderConstantF(it->second, v, 1))) bound += std::max(std::max(v[0], v[1]), std::max(v[2], 0.0f));
  }
  return bound;
}

// The class number of the self-lit draw about to be made (1 ..), 0: not a lamp.
int TagClassOf(IDirect3DDevice9* d, const Classed* c, float amount, char* why) {
  TagClass t = {false, 1.0f, 0.0f, 0.0f};
  if (c->e_kind == 1) {
    const float share = amount / 2.0f, edge = Clamp((share - 0.08f) / 0.08f, 0.0f, 1.0f);
    t.w = std::min(share, 1.0f) * edge * edge * (3.0f - 2.0f * edge);
  } else if (c->decode) {
    if (c->reflect) {
      *why = 'r';
      return 0;
    }
    float scale[4] = {0, 0, 2.0f, 0}, soft[4] = {0.1f, 0, 0, 0};
    d->GetPixelShaderConstantF(c->r_scale, scale, 1);
    if (SUCCEEDED(d->GetPixelShaderConstantF(c->r_soft, soft, 1)) && soft[0] > 0.0f) g_tag_soft = soft[0];
    t.decode = true;
    t.w = 0.0f;
    t.bound = LightBound(d, c);
    t.ramp = amount * scale[2] / kLampKnee;
  }
  for (int i = 0; i < g_ntags; ++i) {
    const TagClass& o = g_tags[i];
    if (o.decode == t.decode && fabsf(o.w - t.w) < 0.02f && fabsf(o.bound - t.bound) < 0.03f && fabsf(o.ramp - t.ramp) < 0.03f) return i + 1;
  }
  if (g_ntags >= kMaxTags) {
    *why = 'f';
    return 0;
  }
  g_tags[g_ntags] = t;
  return ++g_ntags;
}

// The stencil states ours, at the pass's first draw (the title has bound its depth surface and cleared it by then).
bool TagBegin(IDirect3DDevice9* d) {
  IDirect3DSurface9* ds = nullptr;
  D3DSURFACE_DESC dd = {};
  const bool ok = SUCCEEDED(d->GetDepthStencilSurface(&ds)) && ds && SUCCEEDED(ds->GetDesc(&dd)) && (dd.Format == D3DFMT_D24S8 || dd.Format == kIntz) &&
                  dd.MultiSampleType == D3DMULTISAMPLE_NONE;
  SafeRelease(ds);
  if (!ok) {
    g_tag_failed = true;
    ss_log("[hdr] light sources: the G-buffer's depth surface has no stencil to tag (format %d); the lamps are drawn again instead", static_cast<int>(dd.Format));
    return false;
  }
  const int n = sizeof kTagStates / sizeof kTagStates[0];
  for (int i = 0; i < n; ++i) d->GetRenderState(kTagStates[i], &g_tag_saved[i]);
  d->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
  d->SetRenderState(D3DRS_STENCILREF, 0);
  d->SetRenderState(D3DRS_STENCILMASK, 0xFF);
  d->SetRenderState(D3DRS_STENCILWRITEMASK, 0xFF);
  d->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
  d->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
  d->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
  d->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
  d->SetRenderState(D3DRS_STENCILENABLE, TRUE);
  g_tag_on = true;
  g_tag_ref = 0;
  return true;
}

// The title's stencil states back.
void TagRestore(IDirect3DDevice9* d) {
  if (!g_tag_on) return;
  const int n = sizeof kTagStates / sizeof kTagStates[0];
  for (int i = n - 1; i >= 0; --i) d->SetRenderState(kTagStates[i], g_tag_saved[i]);
  g_tag_on = false;
  g_tag_ref = -1;
}

// hdr_light_log = 2: the frame's G-buffer pixel shaders by number (1 ..).
struct CoverShader {
  uint32_t hash;
  bool zwrite;
};
CoverShader g_cover[254];
int g_ncover = 0;
int CoverIndex(IDirect3DDevice9* d) {
  IDirect3DPixelShader9* ps = nullptr;
  if (FAILED(d->GetPixelShader(&ps)) || !ps) return 0;
  ps->Release();
  const uint32_t hash = ProbeShaderOf(ps).hash;
  for (int i = 0; i < g_ncover; ++i)
    if (g_cover[i].hash == hash) return i + 1;
  if (g_ncover >= 254) return 0;
  g_cover[g_ncover] = {hash, true};
  return ++g_ncover;
}

// Before every draw of the G-buffer pass: what the stencil is to hold where this draw is the surface.
inline void TagDraw(IDirect3DDevice9* d) {
  if (!g_gbuf || !g_run || ss_own_drawing != 0 || d != GameDevice()) return;
  if (!g_probe_cover && (OldLampPath() || g_test == 2 || !(LampWhites() > 0.0f))) return;
  int want = 0;
  if (g_probe_cover) {
    want = CoverIndex(d);
  } else if (g_cur_emissive) {
    LampWhy w;
    const float ref = LampDue(d, &w);
    if (ref > 0.0f) {
      want = TagClassOf(d, g_cur_emissive, w.amount, &w.why);
      g_tag_draws += want > 0;
    }
    if (g_probe) ProbeLamp(d, w, want > 0 ? g_tags[want - 1].w : 0.0f);
  } else if (g_probe) {
    ProbeUnclassed(d);
  }
  if (!g_tag_on && !TagBegin(d)) return;
  DWORD zwrite = TRUE;
  d->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);
  const int mode = zwrite ? want : -1;  // a draw that writes no depth is not the surface: the stencil stays
  if (mode == g_tag_ref) return;
  if (mode < 0) {
    d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
  } else {
    if (g_tag_ref < 0) d->SetRenderState(D3DRS_STENCILENABLE, TRUE);
    d->SetRenderState(D3DRS_STENCILREF, static_cast<DWORD>(mode));
  }
  g_tag_ref = mode;
}

// The pass ends (called before the title's call that takes the second target off goes on): the title's states back,
// then the classes' quads into the lamps' target.
void TagEnd(IDirect3DDevice9* d) {
  if (!g_tag_on) return;
  TagRestore(d);
  if (g_probe_cover) {  // the shaders' numbers as the classes' weights
    g_ncover_last = g_ncover;
    for (int i = 0; i < g_ncover; ++i) g_cover_last[i] = g_cover[i].hash;
    g_ncover = 0;
    g_ntags = std::min(g_ncover_last, kMaxTags);
    for (int i = 0; i < g_ntags; ++i) g_tags[i] = {false, (i + 1) / 255.0f, 0.0f, 0.0f};
  }
  const int n = g_ntags;
  g_ntags = 0;
  if (n == 0 || !g_run || !EnsureShaders(d)) return;
  if (!g_r.tag_ps) {
    const DWORD* code = Code(kJobTag);  // done: EnsureShaders made the others from the same job
    const bool ok = code && SUCCEEDED(d->CreatePixelShader(code, &g_r.tag_ps)) && g_r.tag_ps;
    if (!ok) {
      g_tag_failed = true;
      ss_log("[hdr] light sources: the lamp tag's shader could not be made; the lamps are drawn again instead");
      return;
    }
  }
  IDirect3DSurface9 *rt0 = nullptr, *rt2 = nullptr, *ds = nullptr, *target = nullptr;
  IDirect3DTexture9 *t0 = nullptr, *t2 = nullptr;
  D3DSURFACE_DESC desc = {};
  bool ok = SUCCEEDED(d->GetRenderTarget(0, &rt0)) && rt0 && SUCCEEDED(d->GetRenderTarget(2, &rt2)) && rt2 && SUCCEEDED(d->GetDepthStencilSurface(&ds)) && ds &&
            SUCCEEDED(rt0->GetDesc(&desc)) && desc.MultiSampleType == D3DMULTISAMPLE_NONE &&
            SUCCEEDED(rt0->GetContainer(kIidTexture9, reinterpret_cast<void**>(&t0))) && t0 &&
            SUCCEEDED(rt2->GetContainer(kIidTexture9, reinterpret_cast<void**>(&t2))) && t2;
  if (ok && (!g_r.lamp_tag || g_r.tw != desc.Width || g_r.th != desc.Height)) {
    SafeRelease(g_r.lamp_tag);
    ok = g_r.dev == d && MakeTarget(d, desc.Width, desc.Height, D3DFMT_A8R8G8B8, &g_r.lamp_tag);
    g_r.tw = desc.Width, g_r.th = desc.Height;
    g_tag_resolved = false;
    if (ok) ss_log("[hdr] light sources: the lamps' target made at %ux%u (stencil tag, no draw is made again for the lamps)", desc.Width, desc.Height);
  }
  ok = ok && SUCCEEDED(g_r.lamp_tag->GetSurfaceLevel(0, &target)) && target;
  if (ok) {
    Scope sc(d);
    if (sc.ok) {
      sc.States();
      if (SUCCEEDED(d->SetRenderTarget(0, target)) && SUCCEEDED(d->SetDepthStencilSurface(ds))) {
        if (!g_tag_resolved) {
          d->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
          ++g_tag_frames;
        }
        d->SetRenderState(D3DRS_STENCILENABLE, TRUE);
        d->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
        d->SetRenderState(D3DRS_STENCILMASK, 0xFF);
        d->SetRenderState(D3DRS_STENCILWRITEMASK, 0);
        d->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
        d->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
        d->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
        d->SetPixelShader(g_r.tag_ps);
        Sampler(d, 0, t0, false);
        Sampler(d, 1, t2, false);
        for (int i = 0; i < n; ++i) {
          const TagClass& t = g_tags[i];
          const float k[8] = {t.w, t.decode ? 1.0f : 0.0f, t.bound, t.ramp, g_tag_soft, 0, 0, 0};
          d->SetPixelShaderConstantF(0, k, 2);
          d->SetRenderState(D3DRS_STENCILREF, static_cast<DWORD>(i + 1));
          Quad(d, desc.Width, desc.Height);
          ++g_tag_quads;
        }
        d->SetTexture(0, nullptr);
        d->SetTexture(1, nullptr);
        g_tag_resolved = true;
        // The G-buffer's depth, for the lamps' distance fade in the scene pass (an INTZ texture, or no fade).
        D3DSURFACE_DESC dd = {};
        if (!g_scene_depth && SUCCEEDED(ds->GetDesc(&dd)) && dd.Format == kIntz)
          ds->GetContainer(kIidTexture9, reinterpret_cast<void**>(&g_scene_depth));
      } else {
        g_tag_failed = true;
        ss_log("[hdr] light sources: the lamps' target cannot be bound beside the game's depth surface; the lamps are drawn again instead");
      }
    }
  }
  SafeRelease(target);
  SafeRelease(t0);
  SafeRelease(t2);
  SafeRelease(rt0);
  SafeRelease(rt2);
  SafeRelease(ds);
}

// What a draw is to be made again for: 0 nothing, 1 the additive target, 2 the lamps' mask (`weight`).
inline int RedrawDue(IDirect3DDevice9* d, float* weight) {
  if (g_in_lit) {
    const bool again = AdditiveDue(d);
    if (g_probe) ProbeLit(d, again);
    return again ? 1 : 0;
  }
  if (!OldLampPath()) return 0;  // (the stencil tag: TagDraw)
  if (g_cur_emissive) {
    LampWhy w;
    *weight = LampDue(d, &w);
    if (g_probe) g_probe_id = ProbeLamp(d, w, *weight);
    if (*weight > 0.0f) return 2;
  } else if (g_probe) {
    ProbeUnclassed(d);
  }
  return 0;
}
inline bool RedrawBegin(IDirect3DDevice9* d, int what, float weight) { return what == 1 ? AdditiveBegin(d) : LampBegin(d, weight); }
inline void RedrawEnd(IDirect3DDevice9* d, int what) {
  if (what == 1) AdditiveEnd(d);
  else LampEnd(d);
}

using SetRenderTargetFn = HRESULT(__stdcall*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
SetRenderTargetFn o_set_render_target = nullptr;
HRESULT __attribute__((stdcall, force_align_arg_pointer)) SetRenderTargetHook(IDirect3DDevice9* d, DWORD index, IDirect3DSurface9* surface) {
  if (ss_own_drawing == 0) {
    if (index == 0) {
      const bool lit = surface && surface == g_lit_prev;
      if (g_timed && lit != g_in_lit && (lit ? !(g_stamped & (1u << 8)) : true)) Stamp(d, lit ? 8 : 9);
      g_in_lit = lit;
    } else if (index == 1) {
      if (surface) {
        if (!g_gbuf && g_timed && !(g_stamped & (1u << 6))) Stamp(d, 6);
        g_gbuf = true;
      } else if (g_gbuf) {  // the G-buffer pass ends: the tag's quads go in before this call
        g_gbuf = false;
        TagEnd(d);
        if (g_timed) Stamp(d, 7);
      }
    }
  }
  return o_set_render_target(d, index, surface);
}

using DrawFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawUpFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedUpFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
using CreatePixelShaderFn = HRESULT(__stdcall*)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
DrawFn o_draw = nullptr;
DrawIndexedFn o_draw_indexed = nullptr;
DrawUpFn o_draw_up = nullptr;
DrawIndexedUpFn o_draw_indexed_up = nullptr;
CreatePixelShaderFn o_create_ps = nullptr;
using TestCooperativeLevelFn = HRESULT(__stdcall*)(IDirect3DDevice9*);
TestCooperativeLevelFn o_test_cooperative = nullptr;
void** g_hooked_vt = nullptr;

// Test aid (SS_HDR_TEST_RESET only): the device says it wants a Reset until the game has made one.
HRESULT __attribute__((stdcall, force_align_arg_pointer)) TestCooperativeLevelHook(IDirect3DDevice9* d) {
  if (g_test_lost) return D3DERR_DEVICENOTRESET;
  return o_test_cooperative(d);
}

// Between the grade's draw and the split (a few dozen draws a frame) every draw is looked at; otherwise only the
// two-triangle indexed ones (the full-screen passes), and nothing at all while the passes do not run.
inline bool SplitDue() { return g_grade_seen && !g_split_done && g_run && ss_own_drawing == 0; }

HRESULT __attribute__((stdcall, force_align_arg_pointer)) DrawHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT start, UINT prims) {
  if (g_census && ss_own_drawing == 0) CensusDraw(d, prims, "DrawPrimitive");
  if (SplitDue()) MaybeSplit(d);
  float weight = 0.0f;
  TagDraw(d);
  const int again = RedrawDue(d, &weight);
  const HRESULT hr = o_draw(d, t, start, prims);
  if (again && SUCCEEDED(hr) && RedrawBegin(d, again, weight)) {
    o_draw(d, t, start, prims);
    RedrawEnd(d, again);
  }
  return hr;
}
HRESULT __attribute__((stdcall, force_align_arg_pointer)) DrawIndexedHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT base, UINT minv, UINT numv, UINT start, UINT prims) {
  if (g_census && ss_own_drawing == 0) CensusDraw(d, prims, "DrawIndexedPrimitive");
  bool grade_before = g_grade_seen;
  if (g_run && ss_own_drawing == 0) {
    if (g_grade_seen && !g_split_done) MaybeSplit(d);
    grade_before = g_grade_seen;
    if (prims == 2 && g_nclassed) NoteDraw(d);
  }
  float weight = 0.0f;
  TagDraw(d);
  const int again = RedrawDue(d, &weight);
  const HRESULT hr = o_draw_indexed(d, t, base, minv, numv, start, prims);
  // The grade has just drawn onto the final target: the split's passes run now, between two of the game's draws,
  // not inside the next one (that draw, the powerplay meter's card, came out missing after them).
  if (!grade_before && g_grade_seen && SplitDue()) SplitAfterGrade(d);
  if (again && SUCCEEDED(hr) && RedrawBegin(d, again, weight)) {
    o_draw_indexed(d, t, base, minv, numv, start, prims);
    RedrawEnd(d, again);
  }
  return hr;
}
HRESULT __attribute__((stdcall, force_align_arg_pointer)) DrawUpHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT prims, const void* data, UINT stride) {
  if (g_census && ss_own_drawing == 0) CensusDraw(d, prims, "DrawPrimitiveUP");
  if (SplitDue()) MaybeSplit(d);
  float weight = 0.0f;
  TagDraw(d);
  const int again = RedrawDue(d, &weight);
  const HRESULT hr = o_draw_up(d, t, prims, data, stride);
  if (again && SUCCEEDED(hr) && RedrawBegin(d, again, weight)) {
    o_draw_up(d, t, prims, data, stride);
    RedrawEnd(d, again);
  }
  return hr;
}
HRESULT __attribute__((stdcall, force_align_arg_pointer)) DrawIndexedUpHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT minv, UINT numv, UINT prims, const void* idx, D3DFORMAT fmt, const void* data, UINT stride) {
  if (g_census && ss_own_drawing == 0) CensusDraw(d, prims, "DrawIndexedPrimitiveUP");
  if (SplitDue()) MaybeSplit(d);
  float weight = 0.0f;
  TagDraw(d);
  const int again = RedrawDue(d, &weight);
  const HRESULT hr = o_draw_indexed_up(d, t, minv, numv, prims, idx, fmt, data, stride);
  if (again && SUCCEEDED(hr) && RedrawBegin(d, again, weight)) {
    o_draw_indexed_up(d, t, minv, numv, prims, idx, fmt, data, stride);
    RedrawEnd(d, again);
  }
  return hr;
}

HRESULT __attribute__((stdcall, force_align_arg_pointer)) CreatePixelShaderHook(IDirect3DDevice9* d, const DWORD* code, IDirect3DPixelShader9** out) {
  const HRESULT hr = o_create_ps(d, code, out);
  if (SUCCEEDED(hr) && out && *out && code && ss_own_drawing == 0) Classify(*out, code);
  return hr;
}

// The two slots, in the d3d9 module's memory. On the game's thread inside CreateDevice, before the game has the
// device: no shader is created and no frame drawn before they are in.
bool HookDevice(IDirect3DDevice9* d) {
  void** vt = *reinterpret_cast<void***>(d);
  if (vt == g_hooked_vt) return true;
  if (!Readable(reinterpret_cast<uintptr_t>(vt), (kCreatePixelShaderSlot + 1) * sizeof(void*))) return false;
  HMODULE owner = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<const char*>(vt[kDrawIndexedSlot]), &owner) ||
      !owner || owner == GetModuleHandleA(nullptr)) {
    ss_log("[hdr] the device's draw call is not in a d3d9 module; HDR stays off");
    return false;
  }
  struct { int slot; void* mine; void** orig; } slots[] = {
      {kCreatePixelShaderSlot, reinterpret_cast<void*>(&CreatePixelShaderHook), reinterpret_cast<void**>(&o_create_ps)},
      {kDrawSlot, reinterpret_cast<void*>(&DrawHook), reinterpret_cast<void**>(&o_draw)},
      {kDrawUpSlot, reinterpret_cast<void*>(&DrawUpHook), reinterpret_cast<void**>(&o_draw_up)},
      {kDrawIndexedUpSlot, reinterpret_cast<void*>(&DrawIndexedUpHook), reinterpret_cast<void**>(&o_draw_indexed_up)},
      {kDrawIndexedSlot, reinterpret_cast<void*>(&DrawIndexedHook), reinterpret_cast<void**>(&o_draw_indexed)},
      {37, reinterpret_cast<void*>(&SetRenderTargetHook), reinterpret_cast<void**>(&o_set_render_target)},
      {107, reinterpret_cast<void*>(&SetPixelShaderHook), reinterpret_cast<void**>(&o_set_pixel_shader)},
  };
  for (auto& sl : slots)
    if (vt[sl.slot] == sl.mine) return false;  // cannot happen: one vtable, once
  for (auto& sl : slots) *sl.orig = vt[sl.slot];  // set before a slot points at us
  for (auto& sl : slots) {
    if (ss_write_word(&vt[sl.slot], reinterpret_cast<uintptr_t>(sl.mine))) continue;
    ss_log("[hdr] cannot write device vtable slot %d (error %lu); HDR stays off", sl.slot, GetLastError());
    return false;  // a slot already written passes its calls on: g_run stays false
  }
  if (!g_test_reset_at.empty() && vt[3] != reinterpret_cast<void*>(&TestCooperativeLevelHook)) {
    o_test_cooperative = reinterpret_cast<TestCooperativeLevelFn>(vt[3]);
    ss_write_word(&vt[3], reinterpret_cast<uintptr_t>(&TestCooperativeLevelHook));
    ss_log("[hdr] test aid: TestCooperativeLevel wrapped (SS_HDR_TEST_RESET)");
  }
  if (g_hooked_vt) ss_log("[hdr] a device with another vtable (%p, first %p): hooked too", vt, g_hooked_vt);
  g_hooked_vt = vt;
  char name[MAX_PATH] = "?";
  GetModuleFileNameA(owner, name, sizeof name);
  ss_log("[hdr] the four draw calls, SetRenderTarget, SetPixelShader and CreatePixelShader wrapped in %s (device %p)", name, d);
  return true;
}

// --------------------------------------------------------------------------------------- steps B and C: the passes

// The scene pass over the final target `f`. The caller holds a Scope.
void ScenePass(IDirect3DDevice9* d, IDirect3DSurface9* f, Scope& sc) {
  g_scene_done = true;
  g_scene_ok = false;
  D3DSURFACE_DESC fd;
  if (FAILED(f->GetDesc(&fd)) || fd.MultiSampleType != D3DMULTISAMPLE_NONE) return;
  // The tone map draws of this frame that wrote a target of the final target's size (the picture's own).
  int use[kMaxCaptures], nuse = 0;
  bool whole = false;
  for (int i = 0; i < g_ncap; ++i) {
    const Capture& c = g_cap[i];
    if (c.tw != fd.Width || c.th != fd.Height) continue;
    use[nuse++] = i;
    whole = whole || (c.rc.left <= 0 && c.rc.top <= 0 && c.rc.right >= static_cast<LONG>(fd.Width) && c.rc.bottom >= static_cast<LONG>(fd.Height));
  }
  if (g_cap_unknown) {
    static bool said_unknown = false;
    if (!said_unknown) ss_log("[hdr] this frame's tone mapper is one whose formula is not known here: no extra light on such frames");
    said_unknown = true;
  }
  if (nuse == 0 || g_cap_unknown) return;
  if (!sc.ok || !EnsureTargets(d, fd.Width, fd.Height)) return;
  IDirect3DTexture9* graded = TextureOf(d, f, fd);
  IDirect3DSurface9* target = nullptr;
  if (graded && SUCCEEDED(g_r.extra->GetSurfaceLevel(0, &target)) && target) {
    const bool timed = g_timed;
    if (timed) Stamp(d, 0);
    sc.States();
    d->SetRenderTarget(0, target);
    d->SetPixelShader(g_r.scene_ps);
    Sampler(d, 0, graded, false);
    const float lift = g_particles > 0.0f ? std::max(static_cast<float>(g_lift), g_particles) : 0.0f;
    if (!whole || nuse > 1) {  // the luma everywhere first; then each tone map's rectangle
      const float zero[24] = {};
      d->SetPixelShaderConstantF(0, zero, 6);
      for (DWORD s = 1; s <= 10; ++s) Sampler(d, s, nullptr, false);
      Quad(d, fd.Width, fd.Height);
      d->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    }
    if (g_part && g_part_share > 0.0f && lift > 0.0f) ++g_t.particles;
    // The lamps' distance fade: the G-buffer's depth and the scene camera's projection (postfx.cpp), both or none.
    float za = 0.0f, zb = 0.0f;
    const bool dist = g_scene_depth && ss_camera_depth(&za, &zb);
    {
      static bool said_dist = false;
      if (dist && !said_dist && LampWhites() > 0.0f) {
        ss_log("[hdr] light sources: the lamps' lift fades with distance from %.0f to %.0f (the camera's near %.3g, far %.4g)", kLampFadeFrom, kLampFadeTo,
               -zb / za, zb / (1.0f - za));
        said_dist = true;
      }
    }
    for (int u = 0; u < nuse; ++u) {
      const Capture& c = g_cap[use[u]];
      const bool part = g_part && g_part_share > 0.0f && lift > 0.0f;
      // The additive target is the lit scene's size; the scene pass reads it by the picture's coordinates.
      const bool add = g_add_cleared && g_r.add && g_lift > 0.0f;
      const bool old_lamps = OldLampPath();
      const bool lamps = LampWhites() > 0.0f &&
                         (old_lamps ? g_lamp_cleared && g_r.lamp_mask && g_r.lamp_emit && g_r.lamp_depth && g_scene_depth && fd.Width == g_r.lw && fd.Height == g_r.lh
                                    : g_tag_resolved && g_r.lamp_tag && fd.Width == g_r.tw && fd.Height == g_r.th);
      const LONG test = g_test;
      const float k[24] = {c.c100[0], c.c100[1], c.c100[2], c.c100[3],
                           c.white, c.bloom ? c.bloom_power : 1.0f, part ? lift : 0.0f, g_log,
                           part ? g_part_share : 0.0f, kLogStart, c.adapt ? 1.0f : 0.0f, 1.0f,
                           add && test != 3 ? static_cast<float>(g_lift) : 0.0f, lamps && test != 3 ? LampWhites() : 0.0f, c.reinhard ? 1.0f : 0.0f, 0,
                           test == 4 ? 1.0f : 0.0f, old_lamps ? 1.0f : 0.0f, 0, 0,
                           za, zb, kLampFadeFrom, lamps && dist ? kLampFadeTo : 0.0f};
      d->SetPixelShaderConstantF(0, k, 6);
      Sampler(d, 5, add ? g_r.add : nullptr, false);
      Sampler(d, 6, lamps ? (old_lamps ? g_r.lamp_mask : g_r.lamp_tag) : nullptr, false);
      Sampler(d, 7, lamps && old_lamps ? g_r.lamp_depth : nullptr, false);
      Sampler(d, 8, lamps && (old_lamps || dist) ? g_scene_depth : nullptr, false);
      Sampler(d, 9, lamps && old_lamps ? g_r.lamp_emit : nullptr, false);
      Sampler(d, 10, g_probe_cover && g_tag_resolved ? g_r.lamp_tag : nullptr, false);
      Sampler(d, 1, c.scene, false);
      Sampler(d, 2, c.adapt, false);
      Sampler(d, 3, c.bloom, true);
      Sampler(d, 4, part ? g_part : nullptr, true);
      if (!whole || nuse > 1) d->SetScissorRect(&c.rc);
      Quad(d, fd.Width, fd.Height);
      if (g_probe && whole && nuse == 1) ProbeWindow(d, k, target);
    }
    for (DWORD s = 0; s <= 10; ++s) d->SetTexture(s, nullptr);
    if (timed) Stamp(d, 1);
    g_scene_ok = true;
    static bool said = false;
    if (!said) {
      const Capture& c = g_cap[use[0]];
      ss_log("[hdr] first frame with the scene's light: final target %ux%u, %d tone map draw%s, c100 = (%.4g, %.4g, %.4g, %.4g), white %.4g, "
             "bloom power %.4g%s, particles %s",
             fd.Width, fd.Height, nuse, nuse == 1 ? "" : "s", c.c100[0], c.c100[1], c.c100[2], c.c100[3], c.white, c.bloom_power,
             c.bloom ? "" : " (no bloom)", g_part ? "read" : "none");
      said = true;
    }
    static bool said_split = false;
    if (nuse > 1 && !said_split) {
      ss_log("[hdr] %d tone map draws in one frame (split screen?): each one's rectangle gets its own constants", nuse);
      said_split = true;
    }
  }
  SafeRelease(target);
  SafeRelease(graded);
}

void MaskPass(IDirect3DDevice9* d, IDirect3DSurface9* f, Scope& sc) {
  g_extra_valid = false;
  D3DSURFACE_DESC fd;
  if (!g_scene_ok || !sc.ok || FAILED(f->GetDesc(&fd)) || fd.Width != g_r.w || fd.Height != g_r.h) return;
  IDirect3DTexture9* front = TextureOf(d, f, fd);
  IDirect3DSurface9* target = nullptr;
  if (front && SUCCEEDED(g_r.masked->GetSurfaceLevel(0, &target)) && target) {
    const bool timed = g_timed;
    if (timed) Stamp(d, 2);
    sc.States();
    d->SetRenderTarget(0, target);
    d->SetPixelShader(g_r.mask_ps);
    Sampler(d, 0, g_r.extra, false);
    Sampler(d, 1, front, false);
    Quad(d, fd.Width, fd.Height);
    d->SetTexture(0, nullptr);
    d->SetTexture(1, nullptr);
    if (timed) Stamp(d, 3);
    g_extra_valid = true;
  }
  SafeRelease(target);
  SafeRelease(front);
}

// ----------------------------------------------------------------------------------- the swap chain and step D

IHdrSwapchain* Extension(IDirect3DDevice9* d) {
  IDirect3DSwapChain9* sc = nullptr;
  if (FAILED(d->GetSwapChain(0, &sc)) || !sc) return nullptr;
  IHdrSwapchain* ext = nullptr;
  if (FAILED(sc->QueryInterface(kIidVkExtSwapchain, reinterpret_cast<void**>(&ext)))) ext = nullptr;
  sc->Release();
  return ext;
}

float g_display_peak = 0.0f;  // what the swap chain says of the display (0: not known)
float g_display_fall = 0.0f;  // its full frame level (0: not known)
bool g_described = false, g_no_ext = false;
bool g_offered = false;       // the swap chain offered HDR10 when last asked (diag.cpp's [summary])
bool g_cs_dirty = false;      // a Reset happened: colour space and metadata are set again
float g_md_peak = 0, g_md_fall = 0;  // the metadata last sent
uint32_t g_next_try = 0;      // the frame at which HDR10 is asked for again
int g_failures = 0;
bool g_broken = false;        // the output pass failed again and again: HDR stays off for this run
bool g_ten_bit = false;       // the back buffer is A2R10G10B10

void Levels(float* peak, float* paper, float* hud) {
  const float pw = Clamp(g_paper, 80.0f, 1000.0f);
  const float hw = g_hud >= 10.0f ? Clamp(g_hud, 80.0f, 1000.0f) : 0.8f * pw;
  float pk = g_peak >= 10.0f ? static_cast<float>(g_peak) : (g_display_peak > 0.0f ? g_display_peak : 1000.0f);
  pk = std::max(pk, std::max(pw, hw));
  *peak = pk > 10000.0f ? 10000.0f : pk;
  *paper = pw;
  *hud = hw;
}

// The metadata's light levels: the display's own peak and full frame level where the swap chain told them, the
// settings where it did not. A display that tone maps by the metadata compresses whatever is over its own peak in
// it, and a value that followed the PEAK row moved its curve at every step of the calibration screen; told its own
// numbers (what Windows sends it for every composed window) it has nothing to compress and shows the signal as it is.
void MetadataLevels(float* peak, float* fall) {
  float pk, paper, hud;
  Levels(&pk, &paper, &hud);
  *peak = g_display_peak > 0.0f ? g_display_peak : pk;
  *fall = g_display_peak > 0.0f && g_display_fall > 0.0f ? g_display_fall : std::min(paper, *peak);
}

void SetMetadata(IHdrSwapchain* ext, bool always) {
  float peak, fall;
  MetadataLevels(&peak, &fall);
  if (!always && peak == g_md_peak && fall == g_md_fall) return;
  HdrMetadata md = {};
  md.sType = 1000105000;
  md.red[0] = 0.708f, md.red[1] = 0.292f, md.green[0] = 0.170f, md.green[1] = 0.797f, md.blue[0] = 0.131f, md.blue[1] = 0.046f;
  md.white[0] = 0.3127f, md.white[1] = 0.3290f;
  md.maxLuminance = peak, md.minLuminance = 0.0f, md.maxContentLightLevel = peak, md.maxFrameAverageLightLevel = fall;
  const HRESULT hr = ext->SetHDRMetaData(&md);
  if (peak != g_md_peak || fall != g_md_fall)
    ss_log("[hdr] metadata: BT.2020, max and MaxCLL %.0f nits, MaxFALL %.0f nits (%s) (0x%08lx)", peak, fall,
           g_display_peak > 0.0f ? "the display's own numbers" : "the display's are not known: the peak and paper white settings",
           static_cast<unsigned long>(hr));
  g_md_peak = peak;
  g_md_fall = fall;
}

void Describe(IDirect3DDevice9* d) {
  g_described = true;
  IHdrSwapchain* ext = Extension(d);
  if (!ext) {
    g_no_ext = true;
    ss_log("[hdr] DXVK's swap chain extension is not there (not DXVK, or older than 2.3): no HDR output through this d3d9; the SDR picture");
    return;
  }
  const BOOL offered = ext->CheckColorSpaceSupport(kCsHdr10);
  g_offered = offered != FALSE;
  HdrOutputDesc od = {};
  const HRESULT hr = ext->GetCurrentOutputDesc(&od);
  ss_log("[hdr] DXVK's swap chain extension is there; HDR10 ST 2084 %s; display (0x%08lx): %.2f to %.0f nits, full frame %.0f",
         offered ? "offered" : "not offered (an SDR display, or HDR is switched off): the SDR picture until it is", static_cast<unsigned long>(hr),
         od.minLuminance, od.maxLuminance, od.maxFullFrameLuminance);
  ext->Release();
}

void Deactivate(IDirect3DDevice9* d, const char* why) {
  if (!g_active) return;
  g_active = false;
  HRESULT hr = E_FAIL;
  if (IHdrSwapchain* ext = Extension(d)) {
    hr = ext->SetColorSpace(kCsSrgb);
    ext->Release();
  }
  ss_log("[hdr] HDR10 off (%s): swap chain back to sRGB (0x%08lx)", why, static_cast<unsigned long>(hr));
  ss_diag_hdr_event(false);
}

void TryActivate(IDirect3DDevice9* d) {
  g_next_try = g_frame + 120;
  if (g_no_ext) return;
  IHdrSwapchain* ext = Extension(d);
  if (!ext) return;
  g_offered = ext->CheckColorSpaceSupport(kCsHdr10) != FALSE;
  if (g_offered) {
    HdrOutputDesc od = {};
    if (SUCCEEDED(ext->GetCurrentOutputDesc(&od)) && od.maxLuminance >= 200.0f && od.maxLuminance <= 10000.0f) {
      g_display_peak = od.maxLuminance;
      g_display_fall = od.maxFullFrameLuminance >= 50.0f && od.maxFullFrameLuminance <= od.maxLuminance ? od.maxFullFrameLuminance : 0.0f;
    }
    const HRESULT hr = ext->SetColorSpace(kCsHdr10);
    if (SUCCEEDED(hr)) {
      g_active = true;
      g_cs_dirty = false;
      g_md_peak = g_md_fall = 0;
      float peak, paper, hud;
      Levels(&peak, &paper, &hud);
      ss_log("[hdr] HDR10 on: swap chain colour space ST 2084 (0x%08lx); the display's peak %s%.0f nits; peak %.0f, paper white %.0f, HUD %.0f nits; "
             "present: %s",
             static_cast<unsigned long>(hr), g_display_peak > 0 ? "" : "not known, taken as ", g_display_peak > 0 ? g_display_peak : 1000.0f, peak,
             paper, hud, ss_diag_present_text().c_str());
      SetMetadata(ext, true);
      ss_diag_hdr_event(true);  // diag.cpp: Windows HDR, the window and DXVK's swap chain a second later, off this thread
    } else {
      static int said = 0;
      if (said++ < 3) ss_log("[hdr] SetColorSpace(HDR10 ST 2084) failed (0x%08lx); the SDR picture", static_cast<unsigned long>(hr));
    }
  }
  ext->Release();
}

bool EnsureBbCopy(IDirect3DDevice9* d, const D3DSURFACE_DESC& desc) {
  if (g_r.bbcopy && g_r.bw == desc.Width && g_r.bh == desc.Height) return true;
  SafeRelease(g_r.bbcopy);
  if (!MakeTarget(d, desc.Width, desc.Height, desc.Format, &g_r.bbcopy)) return false;
  g_r.bw = desc.Width;
  g_r.bh = desc.Height;
  return true;
}

// The output pass: the back buffer re-encoded for the HDR10 swap chain. False: nothing was written.
bool Output(IDirect3DDevice9* d, IDirect3DSurface9* bb, const D3DSURFACE_DESC& desc, bool patterns) {
  if (!EnsureShaders(d) || !EnsureBbCopy(d, desc)) return false;
  IDirect3DSurface9* copy = nullptr;
  if (FAILED(g_r.bbcopy->GetSurfaceLevel(0, &copy)) || !copy) return false;
  bool ok = false;
  {
    Scope sc(d);
    const bool timed = g_timed;
    if (sc.ok) {
      if (timed) Stamp(d, 4);
      if (SUCCEEDED(d->StretchRect(bb, nullptr, copy, nullptr, D3DTEXF_NONE)) && SUCCEEDED(d->BeginScene())) {
        sc.States();
        d->SetRenderTarget(0, bb);
        d->SetPixelShader(g_r.out_ps);
        const bool extra = g_extra_valid && g_r.masked && !patterns;
        Sampler(d, 0, g_r.bbcopy, false);
        Sampler(d, 1, extra ? g_r.masked : nullptr, false);
        float peak, paper, hud;
        Levels(&peak, &paper, &hud);
        float rect[4] = {0, 0, 0, 0};
        bool cal = false;
        const bool menu = ss_menu_cover(rect, &cal);
        // The settings overlay dims the picture behind it (a black wash of alpha 0x70; its panel is near opaque): the
        // scene's extra light goes down with it.
        const float wash = powf(1.0f - 0x70 / 255.0f, 2.2f);
        const float k[20] = {peak / paper, paper / 10000.0f, g_highlights, hud / 10000.0f,
                             Clamp(g_shadows, 1.0f, 1.5f), kSoftness, extra ? 1.0f : 0.0f, patterns ? 1.0f : 0.0f,
                             float(desc.Width) / float(desc.Height), 0, 0, 0,
                             rect[0] / desc.Width, rect[1] / desc.Height, rect[2] / desc.Width, rect[3] / desc.Height,
                             menu ? wash : 1.0f, menu ? 0.0f : 1.0f, 0, 0};
        d->SetPixelShaderConstantF(0, k, 5);
        Quad(d, desc.Width, desc.Height);
        d->SetTexture(0, nullptr);
        d->SetTexture(1, nullptr);
        d->EndScene();
        ok = true;
      }
      if (timed) {
        Stamp(d, 5);
        if ((g_stamped & 0x3F) == 0x3F && !g_t.no_queries) {  // all three passes ran in this frame
          g_t.pending = true;
          g_t.have2 = (g_stamped & 0x3C0) == 0x3C0;
          g_t.issue_frame = g_frame;
        }
      }
    }
  }
  copy->Release();
  return ok;
}

double PqNits(double v) {
  const double p = pow(v, 1.0 / 78.84375), n = (p > 0.8359375 ? p - 0.8359375 : 0.0) / (18.8515625 - 18.6875 * p);
  return 10000.0 * pow(n, 1.0 / 0.1593017578125);
}

// Test aid: what the swap chain gets, read back (the brightest channel of each pixel as PQ light).
void Readback(IDirect3DDevice9* d, IDirect3DSurface9* bb, const D3DSURFACE_DESC& desc, int secs) {
  IDirect3DSurface9* sys = nullptr;
  D3DLOCKED_RECT lr = {};
  if (desc.Format == D3DFMT_A2R10G10B10 && SUCCEEDED(d->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
      SUCCEEDED(d->GetRenderTargetData(bb, sys)) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
    float peak, paper, hud;
    Levels(&peak, &paper, &hud);
    static const double kEdges[7] = {100, 200, 300, 400, 600, 1000, 2000};
    uint32_t bins[8] = {}, over = 0;
    uint32_t hist[1024] = {};
    double sum = 0;
    static double lut[1024];
    static bool have = false;
    if (!have)
      for (int i = 0; i < 1024; ++i) lut[i] = PqNits(i / 1023.0);
    have = true;
    for (UINT y = 0; y < desc.Height; ++y) {
      const uint32_t* row = reinterpret_cast<const uint32_t*>(static_cast<const unsigned char*>(lr.pBits) + y * lr.Pitch);
      for (UINT x = 0; x < desc.Width; ++x) {
        const uint32_t v = row[x];
        const uint32_t m = std::max((v >> 20) & 1023, std::max((v >> 10) & 1023, v & 1023));
        ++hist[m];
      }
    }
    sys->UnlockRect();
    const double px = double(desc.Width) * desc.Height;
    int top = 0;
    for (int i = 0; i < 1024; ++i) {
      if (!hist[i]) continue;
      top = i;
      const double n = lut[i];
      sum += n * hist[i];
      int k = 0;
      while (k < 7 && n >= kEdges[k] + 0.5) ++k;
      bins[k] += hist[i];
      if (n > paper + 1.0) over += hist[i];
    }
    uint32_t acc = 0;
    int p999 = top;
    for (int i = 1023; i >= 0; --i) {
      acc += hist[i];
      if (acc >= px / 1000.0) {
        p999 = i;
        break;
      }
    }
    ss_log("[hdr] read back at %d s (%ux%u, PQ, extra light %s): highest %.0f nits (code %d), 99.9th percentile %.0f, mean %.1f; above paper white (%.0f) "
           "%.2f%%; <=100 %.2f%%, <=200 %.2f%%, <=300 %.2f%%, <=400 %.2f%%, <=600 %.2f%%, <=1000 %.2f%%, <=2000 %.2f%%, more %.2f%%",
           secs, desc.Width, desc.Height, g_extra_valid ? "yes" : "no", lut[top], top, lut[p999], sum / px, paper, 100.0 * over / px, 100 * bins[0] / px,
           100 * bins[1] / px, 100 * bins[2] / px, 100 * bins[3] / px, 100 * bins[4] / px, 100 * bins[5] / px, 100 * bins[6] / px, 100 * bins[7] / px);
  } else {
    ss_log("[hdr] read back at %d s: failed (back buffer format %d)", secs, static_cast<int>(desc.Format));
  }
  SafeRelease(sys);
}

// Test aid: with HDR off, the back buffer (10-bit) against what the game's own 8-bit back buffer would hold, code for
// code: the SDR picture must be the game's.
void SdrCompare(IDirect3DDevice9* d, IDirect3DSurface9* bb, const D3DSURFACE_DESC& desc, int secs) {
  // The final target is copied by the game's frame end (StretchRect, scaled when its size is not the back buffer's):
  // the same copy is made here into an 8-bit target of the back buffer's size (the game's own back buffer without
  // hdr_ready), with no filter and with a linear one, and each is held against the 10-bit back buffer.
  IDirect3DSurface9* f = FinalSurface();
  D3DSURFACE_DESC fd = {};
  IDirect3DSurface9 *sb = nullptr, *rt8 = nullptr, *s8 = nullptr;
  D3DLOCKED_RECT lb = {};
  bool ok = f && f != bb && SUCCEEDED(f->GetDesc(&fd)) && (fd.Format == D3DFMT_A8R8G8B8 || fd.Format == D3DFMT_X8R8G8B8) &&
            desc.Format == D3DFMT_A2R10G10B10 &&
            SUCCEEDED(d->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sb, nullptr)) &&
            SUCCEEDED(d->CreateRenderTarget(desc.Width, desc.Height, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt8, nullptr)) &&
            SUCCEEDED(d->CreateOffscreenPlainSurface(desc.Width, desc.Height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &s8, nullptr)) &&
            SUCCEEDED(d->GetRenderTargetData(bb, sb)) && SUCCEEDED(sb->LockRect(&lb, nullptr, D3DLOCK_READONLY));
  if (!ok) {
    ss_log("[hdr] SDR check at %d s: not possible (back buffer format %d, final target %s format %d)", secs, static_cast<int>(desc.Format),
           !f ? "not known" : f == bb ? "is the back buffer," : "", static_cast<int>(fd.Format));
  }
  for (int pass = 0; ok && pass < 2; ++pass) {
    const D3DTEXTUREFILTERTYPE filter = pass ? D3DTEXF_LINEAR : D3DTEXF_NONE;
    D3DLOCKED_RECT l8 = {};
    if (FAILED(d->StretchRect(f, nullptr, rt8, nullptr, filter)) || FAILED(d->GetRenderTargetData(rt8, s8)) ||
        FAILED(s8->LockRect(&l8, nullptr, D3DLOCK_READONLY))) {
      ss_log("[hdr] SDR check at %d s: the 8-bit copy (%s) failed", secs, pass ? "linear" : "no filter");
      continue;
    }
    uint64_t off = 0;
    int worst = 0;
    for (UINT y = 0; y < desc.Height; ++y) {
      const uint32_t* rb = reinterpret_cast<const uint32_t*>(static_cast<const unsigned char*>(lb.pBits) + y * lb.Pitch);
      const uint32_t* r8 = reinterpret_cast<const uint32_t*>(static_cast<const unsigned char*>(l8.pBits) + y * l8.Pitch);
      for (UINT x = 0; x < desc.Width; ++x) {
        for (int ch = 0; ch < 3; ++ch) {
          const int v10 = static_cast<int>((rb[x] >> (10 * ch)) & 1023), v8 = static_cast<int>((r8[x] >> (8 * ch)) & 255);
          const int back = (v10 * 255 + 511) / 1023;  // the 10-bit code as the 8-bit one it shows as
          const int diff = back > v8 ? back - v8 : v8 - back;
          if (diff) ++off;
          if (diff > worst) worst = diff;
        }
      }
    }
    s8->UnlockRect();
    ss_log("[hdr] SDR check at %d s (HDR off): the 10-bit back buffer %ux%u against the final target (%ux%u, format %d) copied into 8 bits "
           "%s: %llu of %u channel codes differ, largest %d of 255",
           secs, desc.Width, desc.Height, fd.Width, fd.Height, static_cast<int>(fd.Format), pass ? "with a linear filter" : "with no filter",
           static_cast<unsigned long long>(off), desc.Width * desc.Height * 3, worst);
  }
  if (lb.pBits) sb->UnlockRect();
  SafeRelease(sb);
  SafeRelease(rt8);
  SafeRelease(s8);
}

DWORD g_t0 = 0;
size_t g_readback_next = 0;

bool Apply(const char* key, const char* value);  // the rows (below)

void Present(IDirect3DDevice9* d) {
  ForgetGameDevice();  // the once-a-frame refresh of GameDevice's check
  if (d != GameDevice()) return;
  ++g_frame;
  if (!g_t0) g_t0 = GetTickCount();
  const int secs = static_cast<int>((GetTickCount() - g_t0) / 1000);
  g_encoding = false;
  IDirect3DSurface9* bb = nullptr;
  D3DSURFACE_DESC desc = {};
  if (FAILED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb || FAILED(bb->GetDesc(&desc))) {
    SafeRelease(bb);
    return;
  }
  const bool ten = desc.Format == D3DFMT_A2R10G10B10;
  if (ten != g_ten_bit || !g_described) {
    g_ten_bit = ten;
    ss_log("[hdr] back buffer %ux%u format %d%s", desc.Width, desc.Height, static_cast<int>(desc.Format),
           ten ? " (A2R10G10B10)" : ": not the 10-bit one HDR10 needs; the SDR picture");
  }
  if (!g_described) Describe(d);
  const bool on = g_on != 0;
  // The shaders are asked for at the first present (compiled on the worker, or from the disk cache); the swap chain
  // goes to HDR10 only once they are made, so the switch never waits on a compile.
  const bool ready = EnsureShaders(d);
  const bool want_active = on && ten && !g_broken && !g_r.failed && ready;
  if (g_active && !want_active)
    Deactivate(d, !on ? "the HDR row" : !ten ? "the back buffer is not 10-bit" : !ready && !g_r.failed ? "the passes' shaders are not made yet" : "the passes failed");
  if (!g_active && want_active && g_frame >= g_next_try) TryActivate(d);
  if (g_active) {
    float peak, fall;
    MetadataLevels(&peak, &fall);
    if (g_cs_dirty || peak != g_md_peak || fall != g_md_fall) {
      if (IHdrSwapchain* ext = Extension(d)) {
        if (g_cs_dirty) {  // after a Reset: DXVK keeps both; setting them again costs nothing
          const HRESULT hr = ext->SetColorSpace(kCsHdr10);
          ss_log("[hdr] after a device reset: swap chain colour space ST 2084 set again (0x%08lx)", static_cast<unsigned long>(hr));
          ss_diag_hdr_event(SUCCEEDED(hr));
          if (FAILED(hr)) {  // no longer offered: the SDR picture, and asked for again later
            ext->SetColorSpace(kCsSrgb);
            g_active = false;
            g_next_try = g_frame + 120;
          }
        }
        if (g_active) SetMetadata(ext, g_cs_dirty);
        g_cs_dirty = false;
        ext->Release();
      }
    }
  }
  const bool encode = (g_active || (g_force && on && ten)) && !g_r.failed && !g_broken && ready;
  if (encode && d->TestCooperativeLevel() == D3D_OK) {
    const bool patterns = g_calibrating != 0 || (g_cal_at >= 0 && secs >= g_cal_at);
    const int64_t t0 = Qpc();
    if (Output(d, bb, desc, patterns)) {
      g_encoding = true;
      g_failures = 0;
      ++g_t.outputs;
    } else if (g_active) {
      // An HDR10 swap chain must never get a frame that is not PQ: back to sRGB at once, and again later.
      Deactivate(d, "the output pass could not run");
      g_next_try = g_frame + 120;
      if (++g_failures >= 3) {
        g_broken = true;
        ss_log("[hdr] the output pass failed three times; HDR stays off for this run");
      }
    }
    g_t.cpu[2] += Secs(Qpc() - t0);
    if (g_encoding && g_readback_next < g_readback_at.size() && secs >= g_readback_at[g_readback_next]) {
      Readback(d, bb, desc, g_readback_at[g_readback_next]);
      ++g_readback_next;
    }
  } else if (!encode && g_readback_next < g_readback_at.size() && secs >= g_readback_at[g_readback_next]) {
    SdrCompare(d, bb, desc, g_readback_at[g_readback_next]);
    ++g_readback_next;
  }
  bb->Release();
  while (g_test_set_next < g_test_set.size() && secs >= g_test_set[g_test_set_next].at) {
    const TestSet& t = g_test_set[g_test_set_next++];
    GpuSegment("up to this step");
    ss_log("[hdr] test aid: at %d s %s = %s", secs, t.key.c_str(), t.value.c_str());
    Apply(t.key.c_str(), t.value.c_str());
  }
  if (g_test_toggle_next < g_test_toggle_at.size() && secs >= g_test_toggle_at[g_test_toggle_next]) {
    ++g_test_toggle_next;
    ss_log("[hdr] test aid: at %d s the HDR row is switched %s", secs, g_on ? "off" : "on");
    Apply("hdr", g_on ? "false" : "true");
  }
  if (g_test_reset_next < g_test_reset_at.size() && secs >= g_test_reset_at[g_test_reset_next] && o_test_cooperative) {
    ++g_test_reset_next;
    g_test_lost = 1;
    ss_log("[hdr] test aid: at %d s this Present answers D3DERR_DEVICELOST; the game is to reset its device", secs);
  }
  if (g_census) CensusEnd();
  if (g_census_next < g_census_at.size() && secs >= g_census_at[g_census_next]) {
    ++g_census_next;
    g_census = true;
    ss_log("[hdr] census: frame at %d s", secs);
  }
  if (g_gpu_timing) {
    ReadStamps(d);
    g_stamped = 0;
    g_timed = !g_t.no_queries && !g_t.pending && (g_frame & 15) == 0;
  }
  g_extra_valid = false;  // the next frame's mask pass says again
  // Switched off (the row, or the display stopped offering HDR10): the passes' targets are let go until they run again.
  if (g_run && !encode && g_r.dev == d) {
    ReleaseDefaultPool();
    g_in_lit = false;
    g_cur_emissive = nullptr;
  }
  g_run = encode;
  Report();
}

// ---------------------------------------------------------------------------------------------------------- rows

bool g_rows = false;

bool Apply(const char* key, const char* value) {
  const std::string v = value ? value : "";
  const float f = strtof(v.c_str(), nullptr);
  if (!strcmp(key, "hdr")) {
    if (!g_want) return false;  // the hooks and the 10-bit back buffer come with the start (hdr_ready = false)
    g_on = IsOn(v) ? 1 : 0;
    g_next_try = 0;
    ss_log("[hdr] hdr %s (from the settings menu)", g_on ? "on" : "off");
    return true;
  }
  if (!strcmp(key, "hdr_peak")) g_peak = f;
  else if (!strcmp(key, "hdr_paper_white")) g_paper = f >= 80.0f ? f : 240.0f;
  else if (!strcmp(key, "hdr_hud")) g_hud = f;
  else if (!strcmp(key, "hdr_shadows")) g_shadows = f >= 1.0f ? f : 1.0f;
  else if (!strcmp(key, "hdr_highlights")) g_highlights = f > 0.0f ? f : 1.5f;
  else if (!strcmp(key, "hdr_lift")) g_lift = Clamp(f, 0.0f, 4.0f);
  else if (!strcmp(key, "hdr_lift_test")) {
    g_test = static_cast<LONG>(Clamp(f, 0.0f, 5.0f));
    ss_log("[hdr] LIGHT SOURCES TEST %ld (0 both, 1 lamps only, 2 additive only, 3 not lifted, 4 old corona rule, 5 lamps by the old re-draw)",
           static_cast<long>(g_test));
  }
  else return false;
  return true;
}

void RegisterRows() {
  if (g_rows) return;
  g_rows = true;
  static const std::string paper = Handheld() ? "200" : "240";
  ss_menu_register(MenuChoiceRow("hdr", "HDR", MENU_PAGE_HDR, MENU_LIVE, "false", {{"false", "OFF"}, {"true", "ON"}}, Apply));
  ss_menu_register(MenuActionRow("hdr_calibrate", "CALIBRATE HDR", MENU_PAGE_HDR, ss_menu_calibration));
  ss_menu_register(MenuChoiceRow("hdr_peak", "PEAK BRIGHTNESS", MENU_PAGE_HDR, MENU_LIVE, "0",
                                 {{"0", "AUTO (SCREEN)"}, {"400", "400 NITS"}, {"600", "600 NITS"}, {"800", "800 NITS"}, {"1000", "1000 NITS"}, {"1500", "1500 NITS"}},
                                 Apply));
  ss_menu_register(MenuChoiceRow("hdr_paper_white", "GAME BRIGHTNESS", MENU_PAGE_HDR, MENU_LIVE, paper.c_str(),
                                 {{"120", "120 NITS"}, {"140", "140 NITS"}, {"160", "160 NITS"}, {"180", "180 NITS"}, {"200", "200 NITS"},
                                  {"220", "220 NITS"}, {"240", "240 NITS"}, {"260", "260 NITS"}, {"280", "280 NITS"}, {"300", "300 NITS"},
                                  {"320", "320 NITS"}, {"340", "340 NITS"}, {"360", "360 NITS"}, {"380", "380 NITS"}, {"400", "400 NITS"}},
                                 Apply));
  ss_menu_register(MenuChoiceRow("hdr_hud", "HUD BRIGHTNESS", MENU_PAGE_HDR, MENU_LIVE, "0",
                                 {{"0", "AUTO"}, {"120", "120 NITS"}, {"140", "140 NITS"}, {"160", "160 NITS"}, {"180", "180 NITS"},
                                  {"200", "200 NITS"}, {"220", "220 NITS"}, {"240", "240 NITS"}, {"260", "260 NITS"}, {"280", "280 NITS"},
                                  {"300", "300 NITS"}, {"320", "320 NITS"}, {"340", "340 NITS"}, {"360", "360 NITS"}, {"380", "380 NITS"},
                                  {"400", "400 NITS"}},
                                 Apply));
  ss_menu_register(MenuChoiceRow("hdr_shadows", "DEEPER DARKS", MENU_PAGE_HDR, MENU_LIVE, "1",
                                 {{"1", "OFF"}, {"1.05", "SLIGHT"}, {"1.1", "MEDIUM"}, {"1.2", "STRONG"}}, Apply));
  ss_menu_register(MenuChoiceRow("hdr_highlights", "HIGHLIGHTS", MENU_PAGE_HDR, MENU_LIVE, "1.5",
                                 {{"1", "OFF"}, {"1.25", "LOW"}, {"1.5", "NORMAL"}, {"2", "HIGH"}, {"3", "VERY HIGH"}}, Apply));
  ss_menu_register(MenuChoiceRow("hdr_lift", "LIGHT SOURCES", MENU_PAGE_HDR, MENU_LIVE, "3",
                                 {{"0", "OFF"}, {"1", "LOW"}, {"2", "MEDIUM"}, {"3", "HIGH"}, {"4", "VERY HIGH"}}, Apply));
  ss_menu_register(MenuChoiceRow("hdr_lift_test", "LIGHT SOURCES TEST", MENU_PAGE_DEVELOPER, MENU_LIVE, "0",
                                 {{"0", "BOTH"}, {"1", "LAMPS ONLY"}, {"2", "ADDITIVE ONLY"}, {"3", "DRAWN, NO LIFT"}, {"4", "OLD CORONA RULE"},
                                  {"5", "LAMPS BY RE-DRAW (OLD)"}},
                                 Apply));
}

float Number(const char* env, const char* key, float fallback) {
  const std::string v = Setting(env, key);
  if (v.empty()) return fallback;
  char* end = nullptr;
  const float f = strtof(v.c_str(), &end);
  return end == v.c_str() ? fallback : f;
}

// NVIDIA under Windows: DXVK must be allowed exclusive full screen (dxvk.allowFse = True) or the driver shows nothing
// of an HDR10 swap chain: with DXVK's default the picture froze on the last SDR frame, or stayed black from the start,
// while every call answered success; with the key the picture moves (GitHub #2, RTX 5090). DXVK reads DXVK_CONFIG when
// the Direct3D object is made, after this. The key does nothing under Wine / Proton and nothing for a window that
// does not cover the screen. Left alone where the player set allowFse (DXVK_CONFIG, DXVK_CONFIG_FILE, or a dxvk.conf
// in the game folder), and on other vendors' GPUs, where HDR works without it.
const char* g_allowfse = "not set";  // for diag.cpp's present line and [summary]

void AllowFullScreenOnNvidia() {
  const HMODULE nt = GetModuleHandleA("ntdll.dll");
  if (nt && GetProcAddress(nt, "wine_get_version")) {
    g_allowfse = "not used (Wine / Proton)";
    return;
  }
  char cur[2048] = "", file[MAX_PATH] = "";
  if (GetEnvironmentVariableA("DXVK_CONFIG", cur, sizeof cur) >= sizeof cur) return;
  bool set = strstr(cur, "allowFse") != nullptr;
  const DWORD m = GetEnvironmentVariableA("DXVK_CONFIG_FILE", file, sizeof file);
  for (const char* path : {m && m < sizeof file ? file : "", "dxvk.conf"}) {
    FILE* f = *path && !set ? fopen(path, "r") : nullptr;
    if (!f) continue;
    for (char line[512]; !set && fgets(line, sizeof line, f);) set = line[strspn(line, " \t")] != '#' && strstr(line, "allowFse");
    fclose(f);
  }
  if (set) g_allowfse = "set by the player";
  bool nvidia = false;
  DISPLAY_DEVICEA dd = {};
  dd.cb = sizeof dd;
  for (DWORD i = 0; !nvidia && EnumDisplayDevicesA(nullptr, i, &dd, 0); ++i) nvidia = strstr(dd.DeviceID, "VEN_10DE") != nullptr;
  if (!nvidia) return;
  if (set) {
    ss_log("[hdr] NVIDIA on Windows: dxvk.allowFse is set by the player: kept");
    return;
  }
  const std::string nv = std::string(cur) + (*cur ? ";" : "") + "dxvk.allowFse = True";
  SetEnvironmentVariableA("DXVK_CONFIG", nv.c_str());
  g_allowfse = "True (set by the patch)";
  ss_log("[hdr] NVIDIA on Windows: DXVK_CONFIG = %s (the driver shows an HDR10 picture only with exclusive full screen allowed)", nv.c_str());
}

}  // namespace

// ------------------------------------------------------------------------------------------------------ hook-ups

// The game draws with Windows' own d3d9 (renderer = native, or no DXVK to route to): no HDR output through it.
// Proton's d3d9 is DXVK.
static bool WindowsD3d9() {
  HMODULE nt = GetModuleHandleA("ntdll.dll");
  return !(nt && GetProcAddress(nt, "wine_get_version")) && !ss_renderer_dxvk();
}

void ss_hdr_start() {
  ss_hdr_probe_start();
  if (!ss_conf_present()) return;
  const bool on = IsOn(Setting("SS_HDR", "hdr"));
  const std::string ready = Setting("SS_HDR_READY", "hdr_ready");
  g_want = on || !(ready == "0" || ready == "false" || ready == "no" || ready == "off");
  if (!g_want) return;  // hdr and hdr_ready off: nothing at all
  if (GetModuleHandleA(nullptr) != reinterpret_cast<HMODULE>(0x400000)) {
    ss_log("[hdr] unexpected image base; HDR stays off");
    g_want = false;
    return;
  }
  g_on = on ? 1 : 0;
  if (WindowsD3d9())
    ss_log("[hdr] HDR is off: the game draws with Windows' own d3d9 (renderer = native), which cannot output HDR; "
           "HDR needs renderer = dxvk");
  AllowFullScreenOnNvidia();
  g_force = IsOn(Setting("SS_HDR_FORCE", "hdr_force"));
  g_peak = Number("SS_HDR_PEAK", "hdr_peak", 0.0f);
  g_paper = Number("SS_HDR_PAPER_WHITE", "hdr_paper_white", Handheld() ? 200.0f : 240.0f);
  if (g_paper < 80.0f) g_paper = Handheld() ? 200.0f : 240.0f;
  g_hud = Number("SS_HDR_HUD", "hdr_hud", 0.0f);
  g_shadows = Clamp(Number("SS_HDR_SHADOWS", "hdr_shadows", 1.0f), 1.0f, 1.5f);
  g_highlights = Clamp(Number("SS_HDR_HIGHLIGHTS", "hdr_highlights", 1.5f), 0.5f, 3.0f);
  g_lift = Clamp(Number("SS_HDR_LIFT", "hdr_lift", 3.0f), 0.0f, 4.0f);
  g_particles = Clamp(Number("SS_HDR_PARTICLES", "hdr_particles", 3.0f), 0.0f, 4.0f);
  g_log = Clamp(Number("SS_HDR_LOG", "hdr_log", 0.5f), 0.0f, 2.0f);
  g_gpu_timing = IsOn(Setting("SS_HDR_GPU", "hdr_gpu"));
  if (const char* e = getenv("SS_HDR_TEST_SET")) {
    for (const char* q = e; *q;) {
      const char* end = strchr(q, ',');
      const std::string item = end ? std::string(q, end) : std::string(q);
      const size_t colon = item.find(':'), eq = item.find('=');
      if (colon != std::string::npos && eq != std::string::npos && eq > colon)
        g_test_set.push_back({atoi(item.c_str()), item.substr(colon + 1, eq - colon - 1), item.substr(eq + 1)});
      if (!end) break;
      q = end + 1;
    }
  }
  g_probe_cover = Setting("SS_HDR_LIGHT_LOG", "hdr_light_log") == "2";
  g_probe = g_probe_cover || IsOn(Setting("SS_HDR_LIGHT_LOG", "hdr_light_log"));
  if (g_probe) ss_log("[hdr] hdr_light_log: the light sources' terms in the middle of the picture go into this log ten times a second ([light] lines)");
  g_test = static_cast<LONG>(Clamp(Number("SS_HDR_LIFT_TEST", "hdr_lift_test", 0.0f), 0.0f, 5.0f));
  g_lamp_kinds = static_cast<uint32_t>(Clamp(Number("SS_HDR_EMISSIVE_KINDS", "hdr_emissive_kinds", 7.0f), 0.0f, 7.0f));
  g_lamp_gain = Clamp(Number("SS_HDR_EMISSIVE_GAIN", "hdr_emissive_gain", 1.0f), 0.0f, 4.0f);
  if (const char* e = getenv("SS_HDR_CALIBRATE")) g_cal_at = atoi(e);
  for (auto [name, list] : {std::pair<const char*, std::vector<int>*>{"SS_HDR_READBACK", &g_readback_at}, {"SS_HDR_TEST_RESET", &g_test_reset_at}, {"SS_HDR_TEST_TOGGLE", &g_test_toggle_at},
                            {"SS_HDR_CENSUS", &g_census_at}})
    if (const char* e = getenv(name))
      for (const char* p = e; *p;) {
        list->push_back(atoi(p));
        p = strchr(p, ',');
        if (!p) break;
        ++p;
      }
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  g_qpf = f.QuadPart;
  ss_log("[hdr] hdr %s: peak %s%.0f, paper white %.0f%s, HUD %s%.0f, shadows %.2f, highlights %.2f; an A2R10G10B10 back buffer is asked for%s",
         on ? "on" : "off (ready: the menu's HDR row switches it on live)", g_peak >= 10.0f ? "" : "the display's (0) ", static_cast<float>(g_peak), static_cast<float>(g_paper), Handheld() ? " (handheld)" : "",
         g_hud >= 10.0f ? "" : "auto (0) ", static_cast<float>(g_hud), static_cast<float>(g_shadows), static_cast<float>(g_highlights),
         g_force ? "; test aid: the passes are forced without an HDR swap chain" : "");
}

bool ss_hdr_wanted() { return g_want; }

// display.cpp: the back buffer format to create and reset the device with (0: the game's).
unsigned ss_hdr_backbuffer_format() {
  const unsigned probe = ss_hdr_probe_backbuffer_format();  // the probe's own choice (a test aid) wins while HDR is off
  if (g_want && (g_on || !probe)) return static_cast<unsigned>(D3DFMT_A2R10G10B10);
  return probe;
}

void ss_hdr_before_create(void* d3d9) { ss_hdr_probe_before_create(d3d9); }

// display.cpp, inside its CreateDevice wrap, before the game has the device.
void ss_hdr_device_created(void* device, unsigned backbuffer_format) {
  ForgetGameDevice();
  if (!g_want) return;
  if (!HookDevice(static_cast<IDirect3DDevice9*>(device))) {
    g_broken = true;
    return;
  }
  if (backbuffer_format != static_cast<unsigned>(D3DFMT_A2R10G10B10))
    ss_log("[hdr] the device was made with back buffer format %u, not A2R10G10B10: the SDR picture", backbuffer_format);
}

// display.cpp's and postfx.cpp's Reset wraps, before the real Reset: nothing of ours may be alive in the default pool.
void ss_hdr_before_reset(void* device) {
  ForgetGameDevice();
  ForgetOriginals();
  if (!g_want) return;
  ReleaseCaptures();  // the game's textures noted this frame, whatever device the passes' objects are on
  g_in_lit = false;
  g_cur_emissive = nullptr;
  g_lit_prev = nullptr;
  g_add_cleared = false;
  memset(g_add_class, 0, sizeof g_add_class);
  g_probe_shaders.clear();
  g_vs_radiosity.clear();
  g_gbuf = g_tag_on = g_tag_resolved = false;  // (the device's states are its defaults after a Reset)
  g_tag_ref = -1;
  g_ntags = 0;
  g_scene_done = g_scene_ok = g_grade_seen = g_split_done = false;
  if (device == g_r.dev) ReleaseDefaultPool();
}

// display.cpp, after a Reset that succeeded.
void ss_hdr_after_reset(void* device) {
  ForgetGameDevice();
  if (!g_want) return;
  if (InterlockedExchange(&g_test_lost, 0)) ss_log("[hdr] test aid: the game reset its device");
  g_cs_dirty = true;
  g_next_try = 0;
}

// postfx.cpp, at the scene/UI split (after its own passes), `final_surface` being render target 0.
void ss_hdr_scene(void* device, void* final_surface) {
  if (!g_run || g_scene_done) return;
  IDirect3DDevice9* d = static_cast<IDirect3DDevice9*>(device);
  if (!EnsureShaders(d)) return;
  const int64_t t0 = Qpc();
  {
    Scope sc(d);
    ScenePass(d, static_cast<IDirect3DSurface9*>(final_surface), sc);
  }
  g_t.cpu[0] += Secs(Qpc() - t0);
}

// postfx.cpp, at the game's EndScene: the scene pass if the frame had no split, then the mask pass.
void ss_hdr_frame_end(void* device, void* final_surface) {
  if (!g_run) {
    if (g_ncap || g_part) ReleaseCaptures();
    TagRestore(static_cast<IDirect3DDevice9*>(device));
    g_gbuf = g_tag_resolved = false;
    g_ntags = 0;
    g_npd = g_probe_lights = 0;
    g_scene_done = g_grade_seen = g_split_done = false;
    return;
  }
  IDirect3DDevice9* d = static_cast<IDirect3DDevice9*>(device);
  IDirect3DSurface9* f = static_cast<IDirect3DSurface9*>(final_surface);
  if (f && EnsureShaders(d) && (g_ncap || g_scene_ok)) {
    const int64_t t0 = Qpc();  // with the state block's capture and apply, as the other two
    double scene = 0;
    {
      Scope sc(d);
      if (!g_scene_done) {
        const int64_t t1 = Qpc();
        ScenePass(d, f, sc);
        scene = Secs(Qpc() - t1);
      }
      MaskPass(d, f, sc);
    }
    g_t.cpu[0] += scene;
    g_t.cpu[1] += Secs(Qpc() - t0) - scene;
  }
  if (g_extra_valid) ++g_t.with_extra;
  else ++g_t.plain;
  ReleaseCaptures();
  TagRestore(d);  // (a pass that did not end by taking its second target off)
  g_gbuf = g_tag_resolved = false;
  g_ntags = 0;
  g_npd = g_probe_lights = 0;
  g_scene_done = g_scene_ok = false;
  g_grade_seen = g_split_done = false;
  g_add_cleared = false;
}

// patch.cpp's Present hook, after the settings overlay and before the real Present.
void ss_hdr_present(void* device) {
  ss_hdr_probe_present(device);
  if (g_want) Present(static_cast<IDirect3DDevice9*>(device));
}

// patch.cpp's Present hook (test aid, SS_HDR_TEST_RESET): true = answer D3DERR_DEVICELOST instead of presenting.
bool ss_hdr_test_lost() { return g_test_lost != 0; }

// menu.cpp, when the overlay opens: the HDR rows, where HDR can be had (the swap chain offers HDR10) or is on.
// With Windows' own d3d9 no HDR goes out: one note row in their place (SS_HDR_FORCE keeps the rows).
void ss_hdr_menu_rows(void* device) {
  if (g_rows) return;
  if (WindowsD3d9() && !g_force) {
    g_rows = true;
    ss_menu_register(MenuActionRow("hdr_needs_dxvk", "HDR", MENU_PAGE_HDR, nullptr, [] { return "NEEDS RENDERER = DXVK"; }));
    return;
  }
  bool show = g_on || g_force;
  if (!show && g_want && device && static_cast<IDirect3DDevice9*>(device) == GameDevice()) {
    if (IHdrSwapchain* ext = Extension(static_cast<IDirect3DDevice9*>(device))) {
      show = ext->CheckColorSpaceSupport(kCsHdr10) != FALSE;
      ext->Release();
    }
  }
  if (show) RegisterRows();
}

// diag.cpp's [summary] block (its own thread: plain reads, a frame old at worst).
void ss_hdr_state(SsHdrState* o) {
  o->wanted = g_want;
  o->on = g_on != 0;
  o->ten_bit = g_ten_bit;
  o->described = g_described;
  o->ext = g_described && !g_no_ext;
  o->offered = g_offered;
  o->active = g_active;
  o->broken = g_broken || g_r.failed;
  float hud;
  Levels(&o->peak, &o->paper, &hud);
  o->display_peak = g_display_peak;
}
const char* ss_hdr_allowfse() { return g_allowfse; }

// menu.cpp: the calibration screen.
bool ss_hdr_active() { return g_active || (g_run && g_force); }
void ss_hdr_calibrating(bool on) { g_calibrating = on ? 1 : 0; }
void ss_hdr_levels(float* peak, float* paper, float* hud) { Levels(peak, paper, hud); }
float ss_hdr_display_peak() { return g_display_peak; }
bool ss_hdr_apply(const char* key, const char* value) { return Apply(key, value); }

// testshot.cpp: whether the back buffer of the frame being presented holds PQ, and the paper white it was made with.
bool ss_hdr_encoded(float* paper_white) {
  float peak, hud;
  Levels(&peak, paper_white, &hud);
  return g_encoding;
}
