// The art of the button prompts (glyph_art.h): the console port's drawing (the same author's code), the parts the PC
// sets use - the face buttons as dark discs (xbox: a coloured ring and letter; nintendo: letters by position; steam:
// light letters) and the menu button - and the game's textures that hold prompts.
#include "glyph_art.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace ss_glyph {

namespace {

const Colour kInk = Rgb(0x07080b);
const Colour kWhite = Rgb(0xf4f5f7);
const Colour kFaceTop = Rgb(0x3a3e49), kFaceBottom = Rgb(0x15171d);
const Colour kRim = Rgb(0xc9ccd6);

// A dark trigger key: dark face, a light rim, light letters.
void DarkPill(Canvas& c, float x, float y, float w, float h, float rx) {
  const float e = std::min(w, h) * 0.06f;
  c.FillRoundRect(x, y, w, h, rx, Paint(kRim));
  c.FillRoundRect(x + e, y + e, w - 2 * e, h - 2 * e, std::max(rx - e, 0.5f), Paint(kFaceTop, kFaceBottom, y, y + h));
}

Colour XboxColour(Kind k) {
  switch (k) {
    case Kind::kA: return Rgb(0x7ccf3e);
    case Kind::kB: return Rgb(0xf0564a);
    case Kind::kX: return Rgb(0x4a9af0);
    default: return Rgb(0xf7d843);
  }
}

std::string F(float v) {
  char b[32];
  snprintf(b, sizeof b, "%.3f", v);
  return b;
}

void DarkDisc(Canvas& c, float cx, float cy, float r) {
  c.FillDisc(cx, cy, r, Paint(kInk));
  c.FillDisc(cx, cy, r - r * 0.114f, Paint(kFaceTop, kFaceBottom, cy - r, cy + r));
  const float rr = r * 0.78f;
  Path rim("M" + F(cx - rr * 0.87f) + "," + F(cy - rr * 0.5f) + " A" + F(rr) + "," + F(rr) + " 0 0 1 " + F(cx + rr * 0.87f) + "," +
               F(cy - rr * 0.5f),
           float(c.Scale()));
  c.StrokePath(rim, std::max(0.5f, r * 0.07f), Paint(Rgb(0xffffff, 0.22f)));
}

void FitText(Canvas& c, const std::string& text, float cx, float cy, float max_w, float max_h, Colour colour) {
  float height = max_h;
  const float w10 = Canvas::TextWidth(text, 10.0f);
  if (w10 > 0) height = std::min(height, max_w / w10 * 10.0f);
  c.StrokeText(text, cx, cy, height, std::max(0.6f, height * 0.17f), Paint(colour), 0.22f);
}

// Nintendo prints by position: the game's A is the bottom button, which a Nintendo pad calls B.
const char* FaceLetter(Set set, Kind k) {
  static const char* const plain[] = {"A", "B", "X", "Y"};
  static const char* const swapped[] = {"B", "A", "Y", "X"};
  const int i = static_cast<int>(k);
  if (i < 0 || i > 3) return "";
  return (set == kNintendo ? swapped : plain)[i];
}

// The PlayStation symbols as plain strokes: A a cross, B a circle, X a square, Y a triangle.
void ShapeSymbol(Canvas& c, Kind k, float cx, float cy, float r) {
  const float lw = r * 0.24f;
  const float sc = float(c.Scale());
  switch (k) {
    case Kind::kA: {
      const float d = r * 0.40f;
      c.StrokePath(Path("M" + F(cx - d) + "," + F(cy - d) + " L" + F(cx + d) + "," + F(cy + d) + " M" + F(cx + d) + "," + F(cy - d) +
                            " L" + F(cx - d) + "," + F(cy + d),
                        sc),
                   lw, Paint(Rgb(0x8fb2ff)));
      break;
    }
    case Kind::kB:
      c.StrokeCircle(cx, cy, r * 0.46f, lw, Paint(Rgb(0xff5f5a)));
      break;
    case Kind::kX: {
      const float d = r * 0.40f;
      c.StrokeRoundRect(cx - d, cy - d, 2 * d, 2 * d, lw * 0.3f, lw, Paint(Rgb(0xff8fe0)));
      break;
    }
    default: {
      const float t = r * 0.50f;
      c.StrokePath(Path("M" + F(cx) + "," + F(cy - t) + " L" + F(cx + t) + "," + F(cy + t * 0.7f) + " L" + F(cx - t) + "," +
                            F(cy + t * 0.7f) + " Z",
                        sc),
                   lw, Paint(Rgb(0x45e6b0)));
      break;
    }
  }
}

void MenuIcon(Canvas& c, float cx, float cy, float r) {
  const float lw = r * 0.16f;
  for (int i = -1; i <= 1; ++i) {
    const float y = cy + float(i) * r * 0.30f;
    c.StrokePath(Path("M" + F(cx - r * 0.38f) + "," + F(y) + " L" + F(cx + r * 0.38f) + "," + F(y), float(c.Scale())), lw, Paint(kWhite));
  }
}

void PlusIcon(Canvas& c, float cx, float cy, float r) {
  const float lw = r * 0.17f;
  c.StrokePath(Path("M" + F(cx - r * 0.40f) + "," + F(cy) + " L" + F(cx + r * 0.40f) + "," + F(cy) + " M" + F(cx) + "," +
                        F(cy - r * 0.40f) + " L" + F(cx) + "," + F(cy + r * 0.40f),
                    float(c.Scale())),
               lw, Paint(kWhite));
}

void DrawSlot(Canvas& c, Set set, const Slot& s) {
  const float cx = s.x + s.w / 2, cy = s.y + s.h / 2;
  const float r = std::min(s.w, s.h) / 2;
  if (s.kind == Kind::kLeftTrigger || s.kind == Kind::kRightTrigger) {
    const bool left = s.kind == Kind::kLeftTrigger;
    const char* text = set == kSteam || set == kPlayStation ? (left ? "L2" : "R2") : set == kNintendo ? (left ? "ZL" : "ZR") : (left ? "LT" : "RT");
    DarkPill(c, s.x, s.y, s.w, s.h, std::min(s.w, s.h) * 0.30f);
    FitText(c, text, cx, cy, s.w * 0.80f, std::min(s.w, s.h) * 0.52f, kWhite);
    return;
  }
  if (s.kind == Kind::kStart) {
    c.FillDisc(cx, cy, r, Paint(kWhite));
    c.FillDisc(cx, cy, r - r * 0.16f, Paint(Rgb(0x101421)));
    if (set == kNintendo) PlusIcon(c, cx, cy, r);
    else MenuIcon(c, cx, cy, r);
    return;
  }
  DarkDisc(c, cx, cy, r);
  if (set == kPlayStation) {
    ShapeSymbol(c, s.kind, cx, cy, r);
    return;
  }
  if (set == kXbox) c.StrokeCircle(cx, cy, r * 0.78f, r * 0.15f, Paint(XboxColour(s.kind)));
  FitText(c, FaceLetter(set, s.kind), cx, cy + r * 0.01f, r * 1.0f, r * 0.92f, set == kXbox ? XboxColour(s.kind) : kWhite);
}

}  // namespace

// ---------------------------------------------------------------------------------------- the sites

namespace {
// circle_buttons: the discs Y, B, A, X in 32-pixel cells (each about 24 pixels across); the PS3 row below is kept.
const Slot kCircleButtons[] = {
    {Kind::kY, 3, 3, 26, 26}, {Kind::kB, 35, 3, 26, 26}, {Kind::kA, 67, 3, 26, 26}, {Kind::kX, 99, 3, 26, 26}};
// The two 64x64 Start pictures of the set (the Xbox 360's round one and the PS3's; which is which is not recorded,
// both are drawn: the PS3 one only shows with the playstation set, which swaps no texture).
const Slot kStartA[] = {{Kind::kStart, 17, 16, 34, 32}};
const Slot kStartB[] = {{Kind::kStart, 17, 15, 30, 30}};
// The race HUD's pictures (Hud.gfx; streamed textures, hashed as the game uploads them on the test install): the large
// A and B of its messages (PRESS A TO FIRE ... POWER PLAYS) and the race tips' triggers, 64x64 each, nothing kept.
const Slot kHudA[] = {{Kind::kA, 1, 1, 62, 62}};
const Slot kHudB[] = {{Kind::kB, 1, 1, 62, 62}};
const Slot kHudLT[] = {{Kind::kLeftTrigger, 15, 1, 33, 61}};
const Slot kHudRT[] = {{Kind::kRightTrigger, 15, 1, 33, 61}};

const Site kSites[] = {
    {"circle_buttons", 128, 64, {0x77842A3DF03FD9DEull, 0x265C98E89DA2C95Aull, 0xF60776BDA4D29CEFull}, kCircleButtons, 4, {0, 0, 128, 32}, false},
    {"Start (round)", 64, 64, {0xD7E4EE4FBCF6B15Full, 0xB099BDAAE139E16Full, 0xC4AFCE889140D010ull}, kStartA, 1, {0, 0, 64, 64}, false},
    {"Start (pill)", 64, 64, {0x85288147CABA683Bull, 0x95AE2BE158F0B03Bull, 0x735F59C8D9A06B5Dull}, kStartB, 1, {0, 0, 64, 64}, false},
    {"HUD message A", 64, 64, {0x505BC026F5F4633Eull, 0, 0}, kHudA, 1, {0, 0, 64, 64}, true},
    {"HUD message B", 64, 64, {0xA231B5CFCD0791DAull, 0, 0}, kHudB, 1, {0, 0, 64, 64}, true},
    {"HUD trigger LT", 64, 64, {0xC42F68496C0FBF0Dull, 0, 0}, kHudLT, 1, {0, 0, 64, 64}, true},
    {"HUD trigger RT", 64, 64, {0xD88D4F2C726F58F2ull, 0, 0}, kHudRT, 1, {0, 0, 64, 64}, true},
};

}  // namespace

const Site* Sites(int* count) {
  *count = static_cast<int>(sizeof(kSites) / sizeof(kSites[0]));
  return kSites;
}

bool SiteSize(uint32_t w, uint32_t h) {
  for (const Site& s : kSites)
    if (s.width == w && s.height == h) return true;
  return false;
}

// FNV-1a 64 of a level's pixels, row by row (the pitch skipped).
uint64_t Fnv(const uint8_t* bits, int pitch, uint32_t w, uint32_t h) {
  uint64_t x = 0xcbf29ce484222325ull;
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* row = bits + static_cast<ptrdiff_t>(y) * pitch;
    for (uint32_t i = 0; i < w * 4; ++i) {
      x ^= row[i];
      x *= 0x100000001b3ull;
    }
  }
  return x;
}

// Texture bytes <-> straight RGBA of the canvas.
void ToRgba(const uint8_t* t, uint8_t* o, int variant) {
  if (variant == 1) {
    memcpy(o, t, 4);
    return;
  }
  o[0] = t[2], o[1] = t[1], o[2] = t[0], o[3] = t[3];
  if (variant == 2 && o[3]) {
    for (int c = 0; c < 3; ++c) o[c] = static_cast<uint8_t>(std::min(255, (o[c] * 255 + o[3] / 2) / o[3]));
  }
}
void FromRgba(const uint8_t* s, uint8_t* t, int variant) {
  if (variant == 1) {
    memcpy(t, s, 4);
    return;
  }
  uint8_t r = s[0], g = s[1], b = s[2];
  if (variant == 2) {
    r = static_cast<uint8_t>((r * s[3] + 127) / 255);
    g = static_cast<uint8_t>((g * s[3] + 127) / 255);
    b = static_cast<uint8_t>((b * s[3] + 127) / 255);
  }
  t[0] = b, t[1] = g, t[2] = r, t[3] = s[3];
}

// The picture of a site in a set, every level (straight RGBA), drawn over the game's own first level.
std::vector<Level> MakePicture(const Site& site, Set set, const std::vector<uint8_t>& original, int variant) {
  const uint32_t w = site.width, h = site.height;
  std::vector<uint8_t> base(size_t(w) * h * 4, 0);
  for (size_t i = 0; i < size_t(w) * h; ++i) ToRgba(&original[i * 4], &base[i * 4], variant);
  for (int y = site.clear.top; y < site.clear.bottom; ++y)
    for (int x = site.clear.left; x < site.clear.right; ++x) memset(&base[(size_t(y) * w + x) * 4], 0, 4);
  Canvas canvas(static_cast<int>(w), static_cast<int>(h), 1);
  for (int i = 0; i < site.slot_count; ++i) DrawSlot(canvas, set, site.slots[i]);
  std::vector<uint8_t> drawn;
  canvas.Finish(&drawn, kInk);
  for (size_t i = 0; i < drawn.size(); i += 4) {  // the drawing over what is kept, by the drawing's alpha
    const float a = drawn[i + 3] / 255.0f;
    if (a <= 0.0f) continue;
    const float ba = base[i + 3] / 255.0f;
    const float oa = a + ba * (1.0f - a);
    for (int ch = 0; ch < 3; ++ch) {
      const float v = (drawn[i + ch] * a + base[i + ch] * ba * (1.0f - a)) / std::max(oa, 1e-5f);
      base[i + ch] = static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f);
    }
    base[i + 3] = static_cast<uint8_t>(std::clamp(oa, 0.0f, 1.0f) * 255.0f + 0.5f);
  }
  return MakeMipChain(base, w, h);
}


}  // namespace ss_glyph
