// Ultrawide extras (aspect.cpp): the pure part, apart from the game so test/aspecttest.cpp can run it on any host.
//
// Everything here acts only on a screen wider than 16:9 (Wider). On 16:9 and 16:10 the callers do nothing at all.

#pragma once
#include <cmath>
#include <cstring>

namespace aspectcore {

constexpr double kConsole = 16.0 / 9.0;

// A screen of shape a is wider than 16:9 (a 0.1 % margin: 1920x1080, 2560x1440, 1280x800 are not).
inline bool Wider(double a) { return a > kConsole * 1.001; }

// The 3D player tags (UI/HUD/PlayerNames3D: name width, icon width, icon offset x) are sized as a share of the
// viewport's width, tuned for 16:9: on a wider screen they are stretched by a / (16/9). This is the factor that keeps
// their 16:9 shape, 1 when the screen is not wider.
inline float TagScale(double a) { return Wider(a) ? static_cast<float>(kConsole / a) : 1.0f; }

// The split-screen HUD movies: one 16:9 stage with player 1's HUD in its left half and player 2's in its right half,
// drawn over the whole screen. File name (after the last / or \) compared without case.
inline bool SplitHudName(const char* url) {
  if (!url) return false;
  const char* name = url;
  for (const char* p = url; *p; ++p)
    if (*p == '/' || *p == '\\') name = p + 1;
  static const char* const kNames[] = {"hud.gfx", "timeroverlay.gfx"};
  for (const char* n : kNames) {
    size_t i = 0;
    while (name[i] && n[i] && (name[i] | 0x20) == n[i]) ++i;
    if (!name[i] && !n[i]) return true;
  }
  return false;
}

struct Rect {
  int l, t, w, h;
};

// The split-screen HUD for player p (0 left, 1 right) on a full-screen viewport `full`: the movie's stage is drawn
// `t` wide (the interface box's shape: h * t pixels) and placed so that its p-th half sits in the middle of screen
// half p; the scissor keeps that half of the stage only. Returns false when the stage would not fit (t wider than
// the screen).
inline bool HalfView(const Rect& full, double t, int p, Rect& vp, Rect& scissor) {
  if (full.w <= 0 || full.h <= 0 || t <= 0) return false;
  const double s = full.h * t;           // the stage's width in pixels
  if (s > full.w + 0.5) return false;
  const int half = full.w / 2;
  const int hl = full.l + (p ? half : 0), hw = p ? full.w - half : half;  // screen half p
  const double centre = hl + hw / 2.0;
  const double stage_l = centre - s / 4.0 - p * s / 2.0;
  vp = {static_cast<int>(std::lround(stage_l)), full.t, static_cast<int>(std::lround(s)), full.h};
  int sl = static_cast<int>(std::lround(centre - s / 4.0)), sr = static_cast<int>(std::lround(centre + s / 4.0));
  if (sl < hl) sl = hl;
  if (sr > hl + hw) sr = hl + hw;
  scissor = {sl, full.t, sr > sl ? sr - sl : 0, full.h};
  return true;
}

}  // namespace aspectcore
