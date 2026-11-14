// The art of the button prompts (glyphs.cpp): the sets, the game's textures that hold prompts (the sites), and
// their pictures drawn with glyph_canvas.h. No Windows types: test/glyphtest.cpp builds it for the host.
#pragma once
#include <cstdint>
#include <vector>

#include "glyph_canvas.h"

namespace ss_glyph {

enum Set : int { kAuto, kXbox360, kXbox, kPlayStation, kNintendo, kSteam, kKeyboard, kSetCount };
const char* const kSetNames[kSetCount] = {"auto", "xbox360", "xbox", "playstation", "nintendo", "steam", "keyboard"};

enum class Kind : uint8_t { kA, kB, kX, kY, kStart, kLeftTrigger, kRightTrigger };

struct Slot {
  Kind kind;
  float x, y, w, h;
};

struct Box {
  int left, top, right, bottom;
};

struct Site {
  const char* name;
  uint32_t width, height;
  uint64_t hash[3];  // FNV-1a 64 of the first level as the archive holds it: BGRA (A8R8G8B8), RGBA, premultiplied BGRA
  const Slot* slots;
  int slot_count;
  Box clear;         // what is made transparent before the drawing (the old pictures); the rest stays the game's
  bool playstation;  // drawn for the playstation set too (the prompt bar's own textures are not: it shows the PS3 frames)
};

// The sites, and how many.
const Site* Sites(int* count);
bool SiteSize(uint32_t w, uint32_t h);
// FNV-1a 64 of a level's pixels, row by row (the pitch skipped).
uint64_t Fnv(const uint8_t* bits, int pitch, uint32_t w, uint32_t h);
// Texture bytes <-> straight RGBA (variant: 0 BGRA, 1 RGBA, 2 premultiplied BGRA).
void ToRgba(const uint8_t* t, uint8_t* o, int variant);
void FromRgba(const uint8_t* s, uint8_t* t, int variant);
// The picture of a site in a set, every level (straight RGBA), drawn over the game's own first level (texture bytes).
std::vector<Level> MakePicture(const Site& site, Set set, const std::vector<uint8_t>& original, int variant);
// Whether a set draws its own pictures (the others show the game's).
inline bool Draws(const Site& site, Set s) {
  return s == kXbox || s == kNintendo || s == kSteam || (s == kPlayStation && site.playstation);
}

}  // namespace ss_glyph
