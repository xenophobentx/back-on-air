// shadow_resolution (SSPC-50): the sun shadow atlas at 2x or 4x the console's texels.
//
// The game's sun shadow is three 1024x1024 cascades stacked in one 1024x3072 atlas: a colour target (A8R8G8B8 /
// X8R8G8B8) and an INTZ depth texture, both made with CreateTexture(1024, 3072, ...). The size comes from immediates
// in .text that must agree (render graph 0xA06CC6 -> 0xB20DB0, pool 0x88BDDE / 0x88BE19, set-up record 0x88A5C6, the
// pool finds its target by the name "DepthMap1024x3072_0", 0x88F4B0), so no data write can change it
// (docs/shadowres-estimate.md). Nothing here writes the game's image. The levers are slots of the device's vtable
// (d3d9's memory), as in display.cpp:
//   * CreateTexture (slot 23): a DEFAULT-pool render-target or depth-stencil texture of exactly 1024x3072 is made
//     k times as wide and high. The game keeps 1024x3072 as its view of the atlas. The size is unique in the game
//     (the other targets are screen-sized, 768x384 reflection, 512x512, 1024x32; split screen uses 1024x1024 maps,
//     left alone).
//   * SetRenderTarget (37) / SetDepthStencilSurface (39): note whether the bound colour target 0 or depth surface is
//     one of the enlarged atlas surfaces (by its size, k*1024 x k*3072: no pointer kept, so a released and re-made
//     texture after a Reset needs nothing).
//   * SetViewport (47) / SetScissorRect (75): while the atlas is bound, X, Y, width and height times k. The game
//     makes each cascade's viewport from the target size it knows (1024 x 3072) times a normalised rectangle
//     {0, i/3, 1, 1/3} (0x50DA40), and clears with no rectangles (the viewport), so cascade i becomes
//     (0, k*1024*i, k*1024, k*1024).
// The readers sample in UV (texture matrix 0x895318, the 1/3 tile layout), so they need nothing. The filter radius
// ShadowMap_FilterScale is in UV too (1.56/1024 of the atlas), so the penumbra keeps its width in the world; what
// gains is the staircase and the small casters. Texel snapping stays on the 1024 grid (moves by k texels).
// Test aid SS_SHADOW_BIAS_SCALE=<f> (the environment, or env.SS_SHADOW_BIAS_SCALE in the conf): while the atlas is bound, D3DRS_SLOPESCALEDEPTHBIAS (set at 0x50A1A0 from the
// cascade's +0x3B8) is multiplied by f (k keeps the slope bias in world units); SetRenderState (57) is only wrapped
// then.
// shadow_resolution = console (the default, a missing key included) does nothing at all: no hook.
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include "ss_common.h"
#include "ss_shared.h"

namespace {

constexpr UINT kAtlasW = 1024, kAtlasH = 3072;
constexpr int kSlotCreateTexture = 23, kSlotSetRenderTarget = 37, kSlotSetDepthStencil = 39, kSlotSetViewport = 47;
constexpr int kSlotSetRenderState = 57, kSlotSetScissorRect = 75;

int g_k = 1;                // 1 = console (off), 2 or 4
float g_bias_scale = 1.0f;  // SS_SHADOW_BIAS_SCALE
bool g_parsed = false;
void** g_vt = nullptr;

using CreateTextureFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
using SetRenderTargetFn = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using SetDepthStencilFn = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DSurface9*);
using SetViewportFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
using SetScissorRectFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*);
using SetRenderStateFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
CreateTextureFn o_create_texture = nullptr;
SetRenderTargetFn o_set_rt = nullptr;
SetDepthStencilFn o_set_ds = nullptr;
SetViewportFn o_set_viewport = nullptr;
SetScissorRectFn o_set_scissor = nullptr;
SetRenderStateFn o_set_rs = nullptr;

// Render thread only (the game's D3D9 calls and ours all come from it).
bool g_rt_atlas = false, g_ds_atlas = false;
DWORD g_bias_game = 0;  // the game's last D3DRS_SLOPESCALEDEPTHBIAS (float bits)
bool g_bias_applied = false;

bool Bound() { return g_rt_atlas || g_ds_atlas; }

bool IsAtlas(IDirect3DSurface9* s) {
  D3DSURFACE_DESC d;
  return s && SUCCEEDED(s->GetDesc(&d)) && d.Width == kAtlasW * g_k && d.Height == kAtlasH * g_k;
}

DWORD ScaledBias(DWORD bits) {
  float f;
  memcpy(&f, &bits, sizeof f);
  f *= g_bias_scale;
  memcpy(&bits, &f, sizeof f);
  return bits;
}

// After a change of the bound targets: the slope bias follows (test aid only).
void BiasFollows(IDirect3DDevice9* d) {
  if (!o_set_rs || Bound() == g_bias_applied) return;
  g_bias_applied = Bound();
  o_set_rs(d, D3DRS_SLOPESCALEDEPTHBIAS, g_bias_applied ? ScaledBias(g_bias_game) : g_bias_game);
}

HRESULT WINAPI CreateTextureHook(IDirect3DDevice9* d, UINT w, UINT h, UINT lv, DWORD u, D3DFORMAT f, D3DPOOL p,
                                 IDirect3DTexture9** o, HANDLE* s) {
  if (w != kAtlasW || h != kAtlasH || p != D3DPOOL_DEFAULT || !(u & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)))
    return o_create_texture(d, w, h, lv, u, f, p, o, s);
  HRESULT hr = o_create_texture(d, w * g_k, h * g_k, lv, u, f, p, o, s);
  static int said = 0;
  if (said < 8) {
    ++said;
    ss_log("[shadowres] shadow atlas %ux%u (format 0x%lx, usage 0x%lx) made %ux%u: 0x%08lx", w, h, static_cast<unsigned long>(f),
           static_cast<unsigned long>(u), w * g_k, h * g_k, static_cast<unsigned long>(hr));
  }
  if (FAILED(hr)) {  // too big for this device: the game's own size
    ss_log("[shadowres] the %ux%u atlas was refused; this one stays %ux%u", w * g_k, h * g_k, w, h);
    hr = o_create_texture(d, w, h, lv, u, f, p, o, s);
  }
  return hr;
}

HRESULT WINAPI SetRenderTargetHook(IDirect3DDevice9* d, DWORD i, IDirect3DSurface9* s) {
  const HRESULT hr = o_set_rt(d, i, s);
  if (i == 0 && SUCCEEDED(hr)) {
    g_rt_atlas = IsAtlas(s);
    BiasFollows(d);
  }
  return hr;
}

HRESULT WINAPI SetDepthStencilHook(IDirect3DDevice9* d, IDirect3DSurface9* s) {
  const HRESULT hr = o_set_ds(d, s);
  if (SUCCEEDED(hr)) {
    g_ds_atlas = IsAtlas(s);
    BiasFollows(d);
  }
  return hr;
}

HRESULT WINAPI SetViewportHook(IDirect3DDevice9* d, const D3DVIEWPORT9* vp) {
  if (!vp || !Bound()) return o_set_viewport(d, vp);
  D3DVIEWPORT9 v = *vp;
  v.X *= g_k;
  v.Y *= g_k;
  v.Width *= g_k;
  v.Height *= g_k;
  static int said = 0;
  if (said < 6) {
    ++said;
    ss_log("[shadowres] atlas viewport %lu,%lu %lux%lu -> %lu,%lu %lux%lu", vp->X, vp->Y, vp->Width, vp->Height, v.X, v.Y, v.Width, v.Height);
  }
  return o_set_viewport(d, &v);
}

HRESULT WINAPI SetScissorRectHook(IDirect3DDevice9* d, const RECT* r) {
  if (!r || !Bound()) return o_set_scissor(d, r);
  RECT s = {r->left * g_k, r->top * g_k, r->right * g_k, r->bottom * g_k};
  return o_set_scissor(d, &s);
}

HRESULT WINAPI SetRenderStateHook(IDirect3DDevice9* d, D3DRENDERSTATETYPE st, DWORD v) {
  if (st != D3DRS_SLOPESCALEDEPTHBIAS) return o_set_rs(d, st, v);
  g_bias_game = v;
  return o_set_rs(d, st, g_bias_applied ? ScaledBias(v) : v);
}

// The process environment (conf env.* lines are set with SetEnvironmentVariableA, which the CRT's getenv may not see).
bool Env(const char* name, std::string* out) {
  char buf[64];
  const DWORD n = GetEnvironmentVariableA(name, buf, sizeof buf);
  if (!n || n >= sizeof buf) return false;
  *out = buf;
  return true;
}

void Parse() {
  if (g_parsed) return;
  g_parsed = true;
  std::string v, b;
  if (!Env("SS_SHADOW_RESOLUTION", &v)) v = ss_conf("shadow_resolution");
  if (v == "2048") g_k = 2;
  else if (v == "4096") g_k = 4;
  else if (!v.empty() && v != "console" && v != "1024") ss_log("[shadowres] shadow_resolution = %s is not known; console (1024)", v.c_str());
  if (g_k == 1) return;
  if (Env("SS_SHADOW_BIAS_SCALE", &b)) {
    const float f = static_cast<float>(atof(b.c_str()));
    if (f > 0.0f && f <= 16.0f && f != 1.0f) g_bias_scale = f;
  }
}

}  // namespace

bool ss_shadowres_wanted() {
  Parse();
  return g_k > 1;
}

// display.cpp, inside the game's CreateDevice, before the game has the device (the atlas is made in a race load).
void ss_shadowres_device(void* device) {
  if (!ss_shadowres_wanted() || !device) return;
  void** vt = *static_cast<void***>(device);
  if (vt == g_vt) return;
  if (g_vt) {
    ss_log("[shadowres] a device with another vtable (%p, first %p): left at the console size", vt, g_vt);
    return;
  }
  struct { int slot; void* mine; void** orig; } slots[] = {
      {kSlotCreateTexture, reinterpret_cast<void*>(&CreateTextureHook), reinterpret_cast<void**>(&o_create_texture)},
      {kSlotSetRenderTarget, reinterpret_cast<void*>(&SetRenderTargetHook), reinterpret_cast<void**>(&o_set_rt)},
      {kSlotSetDepthStencil, reinterpret_cast<void*>(&SetDepthStencilHook), reinterpret_cast<void**>(&o_set_ds)},
      {kSlotSetViewport, reinterpret_cast<void*>(&SetViewportHook), reinterpret_cast<void**>(&o_set_viewport)},
      {kSlotSetScissorRect, reinterpret_cast<void*>(&SetScissorRectHook), reinterpret_cast<void**>(&o_set_scissor)},
      {kSlotSetRenderState, reinterpret_cast<void*>(&SetRenderStateHook), reinterpret_cast<void**>(&o_set_rs)},
  };
  const int count = g_bias_scale != 1.0f ? 6 : 5;
  for (int i = 0; i < count; ++i) *slots[i].orig = vt[slots[i].slot];  // set before a slot points at us
  // The pass-through slots first, CreateTexture last: until it is in, no atlas is enlarged and the rest is a no-op.
  for (int i = count - 1; i >= 0; --i) {
    if (ss_write_word(&vt[slots[i].slot], reinterpret_cast<uintptr_t>(slots[i].mine))) continue;
    ss_log("[shadowres] cannot write device vtable slot %d (error %lu); the shadow atlas stays 1024x3072", slots[i].slot, GetLastError());
    return;  // CreateTexture is not in: nothing is enlarged, the slots already written pass their calls on
  }
  g_vt = vt;
  ss_log("[shadowres] shadow_resolution %d: the 1024x3072 sun shadow atlas is made %ux%u, its viewports scaled%s", 1024 * g_k,
         kAtlasW * g_k, kAtlasH * g_k, g_bias_scale != 1.0f ? "; slope bias scaled (SS_SHADOW_BIAS_SCALE)" : "");
  if (g_bias_scale != 1.0f) ss_log("[shadowres] test aid: slope-scaled depth bias x %.2f while the atlas is bound", g_bias_scale);
}

// display.cpp, after a Reset: the device's targets are the back buffer and its depth again.
void ss_shadowres_after_reset() {
  if (!g_vt) return;
  g_rt_atlas = g_ds_atlas = false;
  g_bias_applied = false;  // a Reset sets every render state to its default
}
