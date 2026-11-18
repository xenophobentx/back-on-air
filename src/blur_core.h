// blur_core.h: the pure parts of motion_blur_quality = low | high (mblur.cpp, SSPC-62). Portable C++ without Windows
// headers, so test/blurtest.cpp runs on the build machine.
//
// The game's screen-space motion blur programs (cSSMotionBlur, pass 0x49B490) are ps_3_0 programs whose constant table
// (CTAB) names MB_matMotion360 (up to 32 float4x4, one per motion ID: current clip -> last frame's clip),
// MB_motionBlurParams (x = the camera's blur amount, y = a speed / camera-cut factor), MB_viewportParams (zw: the
// texture coordinate -> 0..1 viewport scale, xy its inverse), the colour MB_motionBlurSourceImage (the tone-mapped
// 8-bit picture, alpha = motion ID / 255) and the G-buffer depth (deferred_lighting_DepthTex360). In the Steam build's
// resident archive they sit at c96 (128 registers, row-major), c94, c95, s10 and s3; the register numbers are read
// from each program's own table at run time and never assumed.
//
// What the game's program does (read from the Steam build's byte code, run-time behaviour only; no byte code of it is
// kept here): id = floor(alpha * 255 + 0.25); rows 0, 1 and 3 of matrix [id]; ndc = uv * vp.zw * (2, -2) + (-1, 1);
// prev = M * (ndc, depth, 1); v = (ndc - prev.xy / prev.w) * params.y; d = v * params.x * vp.xy * (0.5, -0.5) in
// texture units; 9 taps at uv + t * d, t = -1, -0.75 .. +1; a tap whose alpha differs from the centre's is replaced
// by the centre colour; the mean of the nine, alpha 1.
//
// Ours (Source below): the same matrix, velocity and span, with N = clamp(streak px / 4, min, cap) taps (TapsFor) jittered by
// interleaved gradient noise; a tap with another motion ID that is nearer than the centre (soft compare of
// -log2(1 - depth)) is left out instead of being replaced (ID 0 counts as the centre); every tap clamped to the
// viewport and the texture; tex2Dlod in the loop; the average stays in the texture's own (sRGB-encoded) values.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace mblur {

struct CtabConst {
  char name[64];
  int set;       // 0 bool, 1 int, 2 float, 3 sampler
  int reg;       // first register
  int count;     // registers
  int cls;       // 0 scalar, 1 vector, 2 matrix rows, 3 matrix columns, 4 object
  int elements;  // array length
};

// The constant table of a shader model 3 program: entries into out (at most max), their number. Only the leading
// comment tokens are walked, each by its own length; every offset is checked against the table's size.
inline int ParseCtab(const uint32_t* code, size_t dwords, CtabConst* out, int max) {
  if (!code || dwords < 2 || (code[0] & 0xFFFF0000u) != 0xFFFF0000u) return 0;  // not a pixel shader
  for (size_t i = 1; i < dwords;) {
    const uint32_t tok = code[i];
    if ((tok & 0xFFFF) != 0xFFFE) return 0;  // comments come first
    const size_t len = (tok >> 16) & 0x7FFF;
    if (i + 1 + len > dwords) return 0;
    if (len >= 8 && code[i + 1] == 0x42415443u) {  // 'CTAB'
      const uint8_t* t = reinterpret_cast<const uint8_t*>(&code[i + 2]);
      const size_t size = (len - 1) * 4;
      uint32_t h[7];
      memcpy(h, t, sizeof h);  // size, creator, version, constants, constant info, flags, target
      if (h[0] != 28) return 0;
      int n = 0;
      for (uint32_t k = 0; k < h[3] && n < max; ++k) {
        const size_t e = static_cast<size_t>(h[4]) + k * 20;
        if (e + 20 > size) break;
        uint32_t name, type;
        uint16_t w[4];
        memcpy(&name, t + e, 4);
        memcpy(w, t + e + 4, 8);
        memcpy(&type, t + e + 12, 4);
        if (name >= size || !memchr(t + name, 0, size - name)) continue;
        CtabConst& c = out[n];
        snprintf(c.name, sizeof c.name, "%s", reinterpret_cast<const char*>(t + name));
        c.set = w[0];
        c.reg = w[1];
        c.count = w[2];
        c.cls = -1;
        c.elements = 0;
        if (type + 16 <= size) {
          uint16_t ti[8];
          memcpy(ti, t + type, 16);  // class, type, rows, columns, elements, struct members, member info
          c.cls = ti[0];
          c.elements = ti[4];
        }
        ++n;
      }
      return n;
    }
    i += 1 + len;
  }
  return 0;
}

// Where the blur program's inputs are, for our shader.
struct Layout {
  int mat_reg = -1, mats = 0;  // first register of MB_matMotion360, matrices in it (1..32)
  bool row_major = true;       // its class (2 rows, 3 columns): ours is declared the same way
  int params_reg = -1, vp_reg = -1;
  int src_sampler = -1, depth_sampler = -1;
  int own_reg = -1;  // a register no entry uses: our noise offset and tap cap
  bool operator==(const Layout& o) const {
    return mat_reg == o.mat_reg && mats == o.mats && row_major == o.row_major && params_reg == o.params_reg &&
           vp_reg == o.vp_reg && src_sampler == o.src_sampler && depth_sampler == o.depth_sampler && own_reg == o.own_reg;
  }
};

inline const CtabConst* Find(const CtabConst* c, int n, const char* name, int set) {
  for (int i = 0; i < n; ++i)
    if (c[i].set == set && !strcmp(c[i].name, name)) return &c[i];
  return nullptr;
}

// Both of the blur's own names: the program is one of the game's motion blur programs.
inline bool IsBlurProgram(const CtabConst* c, int n) {
  bool mat = false, src = false;
  for (int i = 0; i < n; ++i) {
    if (!strcmp(c[i].name, "MB_matMotion360")) mat = true;
    if (!strcmp(c[i].name, "MB_motionBlurSourceImage")) src = true;
  }
  return mat && src;
}

// The layout from a blur program's table; false (and why) when something ours needs is missing or odd.
inline bool LayoutOf(const CtabConst* c, int n, Layout* out, const char** why) {
  Layout l;
  const CtabConst* m = Find(c, n, "MB_matMotion360", 2);
  const CtabConst* p = Find(c, n, "MB_motionBlurParams", 2);
  const CtabConst* v = Find(c, n, "MB_viewportParams", 2);
  const CtabConst* s = Find(c, n, "MB_motionBlurSourceImage", 3);
  const CtabConst* d = nullptr;
  for (int i = 0; i < n && !d; ++i)
    if (c[i].set == 3 && strstr(c[i].name, "DepthTex")) d = &c[i];
  *why = !m ? "no float MB_matMotion360" : !p ? "no float MB_motionBlurParams" : !v ? "no float MB_viewportParams"
         : !s ? "no sampler MB_motionBlurSourceImage" : !d ? "no depth sampler (*DepthTex*)" : nullptr;
  if (*why) return false;
  if (m->count < 4 || m->count % 4 || (m->cls != 2 && m->cls != 3)) {
    *why = "MB_matMotion360 is not an array of 4x4 matrices";
    return false;
  }
  l.mat_reg = m->reg;
  l.mats = m->count / 4 > 32 ? 32 : m->count / 4;
  l.row_major = m->cls == 2;
  l.params_reg = p->reg;
  l.vp_reg = v->reg;
  l.src_sampler = s->reg;
  l.depth_sampler = d->reg;
  if (l.mat_reg + l.mats * 4 > 224 || p->reg >= 224 || v->reg >= 224 || s->reg > 15 || d->reg > 15 || s->reg == d->reg) {
    *why = "a register out of the ps_3_0 range";
    return false;
  }
  // A float register no entry of the table uses (just below the lowest one, else the highest free one).
  bool used[224] = {};
  int lowest = 224;
  for (int i = 0; i < n; ++i) {
    if (c[i].set != 2) continue;
    for (int r = c[i].reg; r < c[i].reg + (c[i].count > 0 ? c[i].count : 1) && r < 224; ++r) used[r] = true;
    if (c[i].reg < lowest) lowest = c[i].reg;
  }
  if (lowest > 0 && lowest < 224 && !used[lowest - 1]) l.own_reg = lowest - 1;
  for (int r = 223; r >= 0 && l.own_reg < 0; --r)
    if (!used[r]) l.own_reg = r;
  if (l.own_reg < 0) {
    *why = "no free float register for our constant";
    return false;
  }
  *out = l;
  *why = "";
  return true;
}

constexpr int kMaxTaps = 24;  // the shader's loop bound (high's cap)

// motion_blur_quality: game = the game's own program; low and high = ours, differing only in the tap range (taps
// = clamp(streak px / 4, min, cap)): low 6 .. 12, high 8 .. 24. Both ranges go in our own register, so one compiled
// shader serves both. Default high; low on the Steam Deck (about 1.8 ms of GPU left at 60 fps, SSPC-38).
enum Level { kLevelGame = 0, kLevelLow = 1, kLevelHigh = 2 };
inline const char* LevelName(int level) { return level == kLevelGame ? "game" : level == kLevelLow ? "low" : "high"; }
inline int DefaultLevel(bool deck) { return deck ? kLevelLow : kLevelHigh; }
// The conf's or the menu's value; empty or not known: the default.
inline int ParseLevel(const char* v, bool deck) {
  char s[16] = {};
  for (int i = 0; v && v[i] && i < 15; ++i) s[i] = static_cast<char>(v[i] >= 'A' && v[i] <= 'Z' ? v[i] + 32 : v[i]);
  if (!strcmp(s, "game") || !strcmp(s, "off") || !strcmp(s, "0") || !strcmp(s, "false")) return kLevelGame;
  if (!strcmp(s, "low")) return kLevelLow;
  if (!strcmp(s, "high")) return kLevelHigh;
  return DefaultLevel(deck);
}

struct TapRange {
  int min, cap;
};
// The tap range of a level. At the game's QUALITY Very High (level 3, its 2x2 supersampling: 4x the pixels) high
// takes low's range. game: none (ours is not drawn).
inline TapRange TapsFor(int level, int quality_level) {
  if (level == kLevelGame) return {0, 0};
  if (level == kLevelLow || quality_level >= 3) return {6, 12};
  return {8, kMaxTaps};
}

// Taps for a streak of `px` pixels (the whole length, -d .. +d), as the shader takes them: one per 4 pixels, at
// least min, at most the cap; 0 below half a pixel (the centre is returned as it is).
inline int TapCount(float px, TapRange r) {
  if (!(px >= 0.5f)) return 0;
  float n = static_cast<float>(static_cast<int>(px * 0.25f));
  if (n < r.min) n = static_cast<float>(r.min);
  if (n > r.cap) n = static_cast<float>(r.cap);
  return static_cast<int>(n);
}

// The noise offset of a frame (in pixels, added to the pixel position): animated over 64 frames only while TAA
// jitters (its history averages the noise away); fixed otherwise (a still grain instead of a crawling one).
// own register: x the noise offset, y the tap cap, z the test view (0 the blur), w the least taps.
inline float NoiseOffset(int taa_frame) { return taa_frame < 0 ? 0.0f : static_cast<float>(taa_frame & 63) * 5.588238f; }

// Our ps_3_0 shader for a layout. Its own names (MBQ_*) so nothing takes it for a game program.
inline std::string Source(const Layout& l) {
  char head[640];
  snprintf(head, sizeof head,
           "#define MBQ_N %d\n#define MBQ_TAPS %d\n"
           "%s float4x4 MBQ_mat[MBQ_N] : register(c%d);\n"
           "float4 MBQ_params : register(c%d);\n"
           "float4 MBQ_vp : register(c%d);\n"
           "float4 MBQ_own : register(c%d);\n"
           "sampler2D MBQ_src : register(s%d);\n"
           "sampler2D MBQ_depth : register(s%d);\n",
           l.mats, kMaxTaps, l.row_major ? "row_major" : "column_major", l.mat_reg, l.params_reg, l.vp_reg, l.own_reg,
           l.src_sampler, l.depth_sampler);
  return std::string(head) + R"(
float MBQ_LogD(float d) { return -log2(max(1.0 - d, 1e-7)); }
float MBQ_Id(float a) { return floor(a * 255.0 + 0.25); }
float4 main(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR {
  float2 texel = max(abs(float2(ddx(uv.x), ddy(uv.y))), 1e-6);
  float4 c0 = tex2Dlod(MBQ_src, float4(uv, 0.0, 0.0));
  float id = MBQ_Id(c0.a);
  float d0 = tex2Dlod(MBQ_depth, float4(uv, 0.0, 0.0)).r;
  // The centre's matrix, rows 0, 1 and 3 (as the game: none when the ID has no matrix).
  float4 m0 = 0.0, m1 = 0.0, m3 = 0.0;
  [unroll] for (int k = 0; k < MBQ_N; ++k) {
    bool s = abs(id - k) < 0.5;
    m0 = s ? MBQ_mat[k][0] : m0;
    m1 = s ? MBQ_mat[k][1] : m1;
    m3 = s ? MBQ_mat[k][3] : m3;
  }
  float2 ndc = uv * MBQ_vp.zw * float2(2.0, -2.0) + float2(-1.0, 1.0);
  float4 p = float4(ndc, d0, 1.0);
  float pw = dot(m3, p);
  float pr = abs(pw) < 1e-6 ? 0.0 : 1.0 / pw;
  float2 vel = (ndc - float2(dot(m0, p), dot(m1, p)) * pr) * MBQ_params.y;
  float2 d = vel * MBQ_params.x * MBQ_vp.xy * float2(0.5, -0.5);  // the span: uv - d .. uv + d
  float px = 2.0 * length(d / texel);
  // No early return: the texel size's gradients stay out of flow control (no taps: the centre comes back as it is).
  bool blur = pr != 0.0 && px >= 0.5 && px < 16384.0;
  d = blur ? d : 0.0;
  float n = blur ? clamp(floor(px * 0.25), MBQ_own.w, MBQ_own.y) : 0.0;
  float jit = frac(52.9829189 * frac(dot(vpos + MBQ_own.xx, float2(0.06711056, 0.00583715))));
  float2 lo = 0.5 * texel;
  float2 hi = min(1.0 / max(MBQ_vp.zw, 1e-6), 1.0) - 0.5 * texel;  // the viewport (ndc -1 .. 1) and the texture
  float ld0 = MBQ_LogD(d0);
  float3 sum = 0.0;
  float wsum = 0.0;
  float span = 2.0 / max(n, 1.0);
  [loop] for (int i = 0; i < MBQ_TAPS; ++i) {
    if (i >= n) break;
    float t = (i + jit) * span - 1.0;
    float2 q = clamp(uv + d * t, lo, hi);
    float4 c = tex2Dlod(MBQ_src, float4(q, 0.0, 0.0));
    float idt = MBQ_Id(c.a);
    float w = 1.0;
    [branch] if (abs(idt - id) > 0.5 && idt > 0.5) {
      // Another object: left out where it is nearer than the centre (a car's colour does not smear into the
      // background behind it); soft over about 4% of the distance.
      float dt = tex2Dlod(MBQ_depth, float4(q, 0.0, 0.0)).r;
      w = saturate(1.0 - (ld0 - MBQ_LogD(dt) - 0.01) * 20.0);
    }
    sum += c.rgb * w;
    wsum += w;
  }
  float3 rgb = (sum + c0.rgb * 1e-3) / (wsum + 1e-3);
  // Test aid (SS_MOTION_BLUR_VIEW): 1 the motion ID (0 black, others by hue), 2 the span d (red x, green y, 0.5 =
  // none; 64 px full scale), 3 the taps (grey n / 24; red: none).
  float view = MBQ_own.z;
  float h = frac(id * 0.618034);
  float3 idc = id < 0.5 ? 0.0 : saturate(abs(frac(h + float3(0.0, 0.667, 0.333)) * 6.0 - 3.0) - 1.0);
  float3 vc = float3(saturate(0.5 + d / (64.0 * texel)), 0.0);
  float3 tc = n < 0.5 ? float3(1.0, 0.0, 0.0) : n / MBQ_TAPS;
  rgb = view < 0.5 ? rgb : view < 1.5 ? idc : view < 2.5 ? vc : tc;
  return float4(rgb, 1.0);
}
)";
}

}  // namespace mblur
