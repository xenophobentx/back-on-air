// A small vector canvas for the button-prompt artwork (glyphs.cpp). Ported from the console port's renderer
// (the same author's code): discs, rounded rectangles, rings and strokes of lines and arcs; a letter is a stroke
// path in a 10-unit cap height, no font is used. Drawn in "design units" (the pixels of the game's own texture),
// rasterised at any whole-number scale with coverage from the distance to the edge. No raster file is kept
// anywhere: the art is code. Standard library only.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ss_glyph {

struct Colour {
  float r = 0, g = 0, b = 0, a = 1;  // straight (not premultiplied), 0..1
};

// 0xRRGGBB, with an alpha.
inline Colour Rgb(uint32_t rgb, float alpha = 1.0f) {
  return {float((rgb >> 16) & 255u) / 255.0f, float((rgb >> 8) & 255u) / 255.0f,
          float(rgb & 255u) / 255.0f, alpha};
}

// A colour, or a vertical gradient from `top` at y0 to `bottom` at y1 (design units).
struct Paint {
  Colour top, bottom;
  float y0 = 0, y1 = 1;
  Paint() = default;
  Paint(Colour solid) : top(solid), bottom(solid) {}
  Paint(Colour t, Colour b, float from, float to) : top(t), bottom(b), y0(from), y1(to) {}
};

struct Point {
  float x = 0, y = 0;
};

// An outline: one or more polylines made from an SVG-like command string ("M0,0 L5,5 Q.. A..").
// Curves are flattened when it is built, finely enough for `scale`.
class Path {
 public:
  Path() = default;
  // Commands: M L H V Q A Z, absolute, comma or space separated. `scale` is the rasteriser's
  // pixels per design unit (a curve gets a segment for about every pixel).
  Path(const std::string& commands, float scale);
  // The path moved and scaled: x' = ox + x * s, y' = oy + y * s (the letters are drawn on a
  // 10-unit cap height and placed by this).
  Path Transformed(float ox, float oy, float s) const;
  const std::vector<std::vector<Point>>& Lines() const { return lines_; }
  bool Closed(size_t i) const { return closed_[i]; }

 private:
  std::vector<std::vector<Point>> lines_;
  std::vector<bool> closed_;
};

class Canvas {
 public:
  // The picture is design_w x design_h units, scale pixels to each. It starts transparent.
  Canvas(int design_w, int design_h, int scale);

  int Width() const { return width_; }
  int Height() const { return height_; }
  int Scale() const { return scale_; }

  void FillDisc(float cx, float cy, float r, const Paint& paint);
  void FillRoundRect(float x, float y, float w, float h, float radius, const Paint& paint);
  // A ring: the circle's line `width` thick, centred on radius r.
  void StrokeCircle(float cx, float cy, float r, float width, const Paint& paint);
  void StrokeRoundRect(float x, float y, float w, float h, float radius, float width,
                       const Paint& paint);
  // The path's lines `width` thick, with round caps and joins; closed paths join their ends.
  void StrokePath(const Path& path, float width, const Paint& paint);
  // The path's lines as a polygon (even-odd): for the triangle of a d-pad key, and the like.
  void FillPath(const Path& path, const Paint& paint);
  // A text of the stroke alphabet, centred on (cx, cy), `height` units tall (capitals only).
  void StrokeText(const std::string& text, float cx, float cy, float height, float width,
                  const Paint& paint, float gap = 0.28f);
  // The width of `text` at `height`, in design units.
  static float TextWidth(const std::string& text, float height, float gap = 0.28f);

  // The finished picture: width*height straight RGBA8, rows top to bottom. Texels the picture
  // does not cover take `background`'s colour at alpha 0, so a bilinear sample at the edge of a
  // shape does not pull in a stray colour.
  void Finish(std::vector<uint8_t>* rgba, Colour background) const;

 private:
  template <class Distance, class PaintFn>
  void Paint2D(float x0, float y0, float x1, float y1, const Distance& distance,
               const PaintFn& paint);
  Colour PaintAt(const Paint& paint, float y) const;
  void Blend(int x, int y, Colour c, float coverage);

  int design_w_, design_h_, scale_, width_, height_;
  std::vector<float> premul_;  // r, g, b, a premultiplied, width*height*4
};

// One level of a full chain: a width x height picture of straight RGBA8.
struct Level {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba;
};

// Every level of the picture, from its own size down to 1x1, by a box filter on premultiplied
// colour (so a transparent texel does not darken an edge).
std::vector<Level> MakeMipChain(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height);

}  // namespace ss_glyph
