// The vector canvas of the button-prompt artwork (glyph_canvas.h).
#include "glyph_canvas.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace ss_glyph {

namespace {

constexpr float kPi = 3.14159265358979f;

// --- the stroke alphabet: advance width, and the strokes on x 0..w, y 0..10 (y down) ----------
struct Letter {
  char c;
  float advance;
  const char* path;
};

// Capitals, two digits and a plus: lines and arcs, one weight. A letter is as wide as its
// strokes need; the gap between letters is added by the caller.
constexpr Letter kLetters[] = {
    {'A', 6.0f, "M0,10 L3,0 L6,10 M1.2,6.6 L4.8,6.6"},
    {'B', 5.6f, "M0,0 L0,10 M0,0 L3,0 A2.4,2.4 0 0 1 3,4.8 L0,4.8 M3,4.8 A2.6,2.6 0 0 1 3,10 L0,10"},
    {'C', 5.6f, "M5.4,2.3 Q4.9,0 2.8,0 Q0,0 0,2.8 L0,7.2 Q0,10 2.8,10 Q4.9,10 5.4,7.7"},
    {'D', 5.8f, "M0,0 L0,10 L2.6,10 Q5.8,10 5.8,6.8 L5.8,3.2 Q5.8,0 2.6,0 L0,0"},
    {'E', 4.8f, "M4.8,0 L0,0 L0,10 L4.8,10 M0,5 L3.9,5"},
    {'F', 4.8f, "M0,10 L0,0 L4.8,0 M0,4.8 L3.8,4.8"},
    {'G', 6.0f, "M5.8,2.3 Q5.3,0 3,0 Q0,0 0,2.8 L0,7.2 Q0,10 3,10 Q6,10 6,7.2 L6,5.4 L3.4,5.4"},
    {'H', 6.0f, "M0,0 L0,10 M6,0 L6,10 M0,5 L6,5"},
    {'I', 0.0f, "M0,0 L0,10"},
    {'K', 5.8f, "M0,0 L0,10 M5.6,0 L0,6 M2,4.4 L5.8,10"},
    {'L', 4.8f, "M0,0 L0,10 L4.8,10"},
    {'M', 7.2f, "M0,10 L0,0 L3.6,6 L7.2,0 L7.2,10"},
    {'N', 6.0f, "M0,10 L0,0 L6,10 L6,0"},
    {'O', 6.0f, "M2.6,0 L3.4,0 Q6,0 6,2.6 L6,7.4 Q6,10 3.4,10 L2.6,10 Q0,10 0,7.4 L0,2.6 Q0,0 2.6,0 Z"},
    {'P', 5.6f, "M0,10 L0,0 L3,0 A2.6,2.6 0 0 1 3,5.2 L0,5.2"},
    {'Q', 6.0f, "M2.6,0 L3.4,0 Q6,0 6,2.6 L6,7.4 Q6,10 3.4,10 L2.6,10 Q0,10 0,7.4 L0,2.6 Q0,0 2.6,0 Z M3.6,6.6 L6.4,10.8"},
    {'R', 5.6f, "M0,10 L0,0 L3,0 A2.5,2.5 0 0 1 3,5 L0,5 M2.6,5 L5.6,10"},
    {'S', 5.6f, "M5.6,1.9 Q5.4,0 2.8,0 Q0,0 0,2.5 Q0,5 2.8,5 Q5.6,5 5.6,7.5 Q5.6,10 2.8,10 Q0.2,10 0,8.1"},
    {'T', 6.0f, "M0,0 L6,0 M3,0 L3,10"},
    {'U', 6.0f, "M0,0 L0,7 Q0,10 3,10 Q6,10 6,7 L6,0"},
    {'V', 6.4f, "M0,0 L3.2,10 L6.4,0"},
    {'W', 8.0f, "M0,0 L2,10 L4,2.8 L6,10 L8,0"},
    {'X', 6.0f, "M0,0 L6,10 M6,0 L0,10"},
    {'Y', 6.0f, "M0,0 L3,5 L6,0 M3,5 L3,10"},
    {'Z', 5.6f, "M0,0 L5.6,0 L0,10 L5.6,10"},
    {'1', 2.6f, "M0,2.2 L2.4,0 L2.4,10"},
    {'2', 5.4f, "M0,2.6 A2.7,2.6 0 0 1 5.4,2.6 Q5.4,4.6 0,10 L5.4,10"},
    {'+', 6.0f, "M0,5 L6,5 M3,2 L3,8"},
};

const Letter* FindLetter(char c) {
  for (const Letter& l : kLetters) {
    if (l.c == c) return &l;
  }
  return nullptr;
}

// --- distances ---------------------------------------------------------------------------------
float DistanceToSegment(float px, float py, Point a, Point b) {
  const float dx = b.x - a.x, dy = b.y - a.y;
  const float len2 = dx * dx + dy * dy;
  float t = len2 > 0 ? ((px - a.x) * dx + (py - a.y) * dy) / len2 : 0.0f;
  t = std::clamp(t, 0.0f, 1.0f);
  const float qx = a.x + dx * t - px, qy = a.y + dy * t - py;
  return std::sqrt(qx * qx + qy * qy);
}

float SignedBox(float px, float py, float cx, float cy, float hx, float hy, float r) {
  const float qx = std::fabs(px - cx) - (hx - r);
  const float qy = std::fabs(py - cy) - (hy - r);
  const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
  return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

// --- the path string ---------------------------------------------------------------------------
struct Reader {
  const char* p;
  bool Skip() {
    while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n') ++p;
    return *p != 0;
  }
  bool Number(float* out) {
    if (!Skip()) return false;
    char* end = nullptr;
    const float v = std::strtof(p, &end);
    if (end == p) return false;
    p = end;
    *out = v;
    return true;
  }
};

// An SVG elliptical arc from (x0, y0) to (x1, y1), appended to `line` as short segments.
void FlattenArc(std::vector<Point>* line, Point from, float rx, float ry, float rotation_deg,
                bool large, bool sweep, Point to, float scale) {
  rx = std::fabs(rx);
  ry = std::fabs(ry);
  if (rx < 1e-6f || ry < 1e-6f || (from.x == to.x && from.y == to.y)) {
    line->push_back(to);
    return;
  }
  const float phi = rotation_deg * kPi / 180.0f;
  const float cp = std::cos(phi), sp = std::sin(phi);
  const float dx = (from.x - to.x) / 2, dy = (from.y - to.y) / 2;
  const float x1 = cp * dx + sp * dy, y1 = -sp * dx + cp * dy;
  const float lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
  if (lambda > 1) {
    const float s = std::sqrt(lambda);
    rx *= s;
    ry *= s;
  }
  const float rx2 = rx * rx, ry2 = ry * ry;
  float num = rx2 * ry2 - rx2 * y1 * y1 - ry2 * x1 * x1;
  const float den = rx2 * y1 * y1 + ry2 * x1 * x1;
  float coef = den > 0 ? std::sqrt(std::max(0.0f, num / den)) : 0.0f;
  if (large == sweep) coef = -coef;
  const float cx1 = coef * rx * y1 / ry, cy1 = -coef * ry * x1 / rx;
  const float cx = cp * cx1 - sp * cy1 + (from.x + to.x) / 2;
  const float cy = sp * cx1 + cp * cy1 + (from.y + to.y) / 2;
  const auto angle = [](float ux, float uy, float vx, float vy) {
    const float dot = ux * vx + uy * vy;
    const float len = std::sqrt(ux * ux + uy * uy) * std::sqrt(vx * vx + vy * vy);
    float a = std::acos(std::clamp(len > 0 ? dot / len : 1.0f, -1.0f, 1.0f));
    if (ux * vy - uy * vx < 0) a = -a;
    return a;
  };
  const float theta1 = angle(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry);
  float delta = angle((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry);
  if (!sweep && delta > 0) delta -= 2 * kPi;
  if (sweep && delta < 0) delta += 2 * kPi;
  const float length = std::fabs(delta) * std::max(rx, ry);
  const int steps = std::clamp(int(std::ceil(length * scale / 1.5f)), 4, 256);
  for (int i = 1; i <= steps; ++i) {
    const float a = theta1 + delta * float(i) / float(steps);
    const float ex = rx * std::cos(a), ey = ry * std::sin(a);
    line->push_back({cp * ex - sp * ey + cx, sp * ex + cp * ey + cy});
  }
  line->back() = to;
}

}  // namespace

// ================================================================================================
// Path

Path::Path(const std::string& commands, float scale) {
  Reader in{commands.c_str()};
  Point cur{}, start{};
  std::vector<Point> line;
  bool closed = false;
  const auto flush = [&] {
    if (line.size() >= 2 || (line.size() == 1 && closed)) {
      lines_.push_back(line);
      closed_.push_back(closed);
    }
    line.clear();
    closed = false;
  };
  char command = 0;
  while (in.Skip()) {
    if (std::isalpha(static_cast<unsigned char>(*in.p))) {
      command = *in.p++;
    }
    switch (command) {
      case 'M': {
        float x, y;
        if (!in.Number(&x) || !in.Number(&y)) return;
        flush();
        cur = start = {x, y};
        line.push_back(cur);
        command = 'L';  // further pairs are lines
        break;
      }
      case 'L': {
        float x, y;
        if (!in.Number(&x) || !in.Number(&y)) return;
        cur = {x, y};
        line.push_back(cur);
        break;
      }
      case 'H': {
        float x;
        if (!in.Number(&x)) return;
        cur.x = x;
        line.push_back(cur);
        break;
      }
      case 'V': {
        float y;
        if (!in.Number(&y)) return;
        cur.y = y;
        line.push_back(cur);
        break;
      }
      case 'Q': {
        float cx, cy, x, y;
        if (!in.Number(&cx) || !in.Number(&cy) || !in.Number(&x) || !in.Number(&y)) return;
        const float length = std::fabs(cx - cur.x) + std::fabs(cy - cur.y) + std::fabs(x - cx) +
                             std::fabs(y - cy);
        const int steps = std::clamp(int(std::ceil(length * scale / 1.5f)), 4, 128);
        for (int i = 1; i <= steps; ++i) {
          const float t = float(i) / float(steps), u = 1 - t;
          line.push_back({u * u * cur.x + 2 * u * t * cx + t * t * x,
                          u * u * cur.y + 2 * u * t * cy + t * t * y});
        }
        cur = {x, y};
        break;
      }
      case 'A': {
        float rx, ry, rot, large, sweep, x, y;
        if (!in.Number(&rx) || !in.Number(&ry) || !in.Number(&rot) || !in.Number(&large) ||
            !in.Number(&sweep) || !in.Number(&x) || !in.Number(&y)) {
          return;
        }
        FlattenArc(&line, cur, rx, ry, rot, large != 0, sweep != 0, {x, y}, scale);
        cur = {x, y};
        break;
      }
      case 'Z':
      case 'z':
        closed = true;
        cur = start;
        flush();
        line.push_back(cur);  // a following segment starts from the same point
        command = 'L';
        break;
      default:
        return;
    }
  }
  if (line.size() >= 2) {
    flush();
  }
}

Path Path::Transformed(float ox, float oy, float s) const {
  Path out;
  out.closed_ = closed_;
  out.lines_ = lines_;
  for (auto& line : out.lines_) {
    for (Point& p : line) {
      p = {ox + p.x * s, oy + p.y * s};
    }
  }
  return out;
}

// ================================================================================================
// Canvas

Canvas::Canvas(int design_w, int design_h, int scale)
    : design_w_(design_w),
      design_h_(design_h),
      scale_(std::max(1, scale)),
      width_(design_w * std::max(1, scale)),
      height_(design_h * std::max(1, scale)),
      premul_(size_t(width_) * size_t(height_) * 4, 0.0f) {}

Colour Canvas::PaintAt(const Paint& paint, float y) const {
  if (paint.y1 == paint.y0) return paint.top;
  const float t = std::clamp((y - paint.y0) / (paint.y1 - paint.y0), 0.0f, 1.0f);
  return {paint.top.r + (paint.bottom.r - paint.top.r) * t,
          paint.top.g + (paint.bottom.g - paint.top.g) * t,
          paint.top.b + (paint.bottom.b - paint.top.b) * t,
          paint.top.a + (paint.bottom.a - paint.top.a) * t};
}

void Canvas::Blend(int x, int y, Colour c, float coverage) {
  const float a = c.a * coverage;
  float* px = &premul_[(size_t(y) * size_t(width_) + size_t(x)) * 4];
  const float keep = 1.0f - a;
  px[0] = c.r * a + px[0] * keep;
  px[1] = c.g * a + px[1] * keep;
  px[2] = c.b * a + px[2] * keep;
  px[3] = a + px[3] * keep;
}

template <class Distance, class PaintFn>
void Canvas::Paint2D(float x0, float y0, float x1, float y1, const Distance& distance,
                     const PaintFn& paint) {
  const float s = float(scale_);
  const int px0 = std::max(0, int(std::floor(x0 * s)) - 1);
  const int py0 = std::max(0, int(std::floor(y0 * s)) - 1);
  const int px1 = std::min(width_ - 1, int(std::ceil(x1 * s)) + 1);
  const int py1 = std::min(height_ - 1, int(std::ceil(y1 * s)) + 1);
  for (int y = py0; y <= py1; ++y) {
    const float dy = (float(y) + 0.5f) / s;
    for (int x = px0; x <= px1; ++x) {
      const float dx = (float(x) + 0.5f) / s;
      const float d = distance(dx, dy);
      const float coverage = std::clamp(0.5f - d * s, 0.0f, 1.0f);
      if (coverage > 0.0f) {
        Blend(x, y, paint(dy), coverage);
      }
    }
  }
}

void Canvas::FillDisc(float cx, float cy, float r, const Paint& paint) {
  Paint2D(cx - r, cy - r, cx + r, cy + r,
          [&](float x, float y) { return std::hypot(x - cx, y - cy) - r; },
          [&](float y) { return PaintAt(paint, y); });
}

void Canvas::FillRoundRect(float x, float y, float w, float h, float radius, const Paint& paint) {
  const float r = std::min(radius, std::min(w, h) / 2);
  Paint2D(x, y, x + w, y + h,
          [&](float px, float py) {
            return SignedBox(px, py, x + w / 2, y + h / 2, w / 2, h / 2, r);
          },
          [&](float py) { return PaintAt(paint, py); });
}

void Canvas::StrokeCircle(float cx, float cy, float r, float width, const Paint& paint) {
  const float e = r + width / 2;
  Paint2D(cx - e, cy - e, cx + e, cy + e,
          [&](float x, float y) { return std::fabs(std::hypot(x - cx, y - cy) - r) - width / 2; },
          [&](float y) { return PaintAt(paint, y); });
}

void Canvas::StrokeRoundRect(float x, float y, float w, float h, float radius, float width,
                             const Paint& paint) {
  const float r = std::min(radius, std::min(w, h) / 2);
  const float e = width / 2;
  Paint2D(x - e, y - e, x + w + e, y + h + e,
          [&](float px, float py) {
            return std::fabs(SignedBox(px, py, x + w / 2, y + h / 2, w / 2, h / 2, r)) - width / 2;
          },
          [&](float py) { return PaintAt(paint, py); });
}

void Canvas::StrokePath(const Path& path, float width, const Paint& paint) {
  struct Segment {
    Point a, b;
  };
  std::vector<Segment> segments;
  float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
  for (size_t i = 0; i < path.Lines().size(); ++i) {
    const auto& line = path.Lines()[i];
    for (size_t k = 0; k + 1 < line.size(); ++k) {
      segments.push_back({line[k], line[k + 1]});
    }
    if (path.Closed(i) && line.size() >= 2) {
      segments.push_back({line.back(), line.front()});
    }
    for (const Point& p : line) {
      x0 = std::min(x0, p.x);
      y0 = std::min(y0, p.y);
      x1 = std::max(x1, p.x);
      y1 = std::max(y1, p.y);
    }
  }
  if (segments.empty()) return;
  const float e = width / 2;
  Paint2D(x0 - e, y0 - e, x1 + e, y1 + e,
          [&](float x, float y) {
            float best = 1e30f;
            for (const Segment& s : segments) {
              best = std::min(best, DistanceToSegment(x, y, s.a, s.b));
            }
            return best - e;
          },
          [&](float y) { return PaintAt(paint, y); });
}

void Canvas::FillPath(const Path& path, const Paint& paint) {
  struct Segment {
    Point a, b;
  };
  std::vector<Segment> segments;
  float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
  for (const auto& line : path.Lines()) {
    for (size_t k = 0; k < line.size(); ++k) {
      segments.push_back({line[k], line[(k + 1) % line.size()]});
      x0 = std::min(x0, line[k].x);
      y0 = std::min(y0, line[k].y);
      x1 = std::max(x1, line[k].x);
      y1 = std::max(y1, line[k].y);
    }
  }
  if (segments.empty()) return;
  Paint2D(x0, y0, x1, y1,
          [&](float x, float y) {
            float best = 1e30f;
            bool inside = false;
            for (const Segment& s : segments) {
              best = std::min(best, DistanceToSegment(x, y, s.a, s.b));
              if ((s.a.y > y) != (s.b.y > y) &&
                  x < (s.b.x - s.a.x) * (y - s.a.y) / (s.b.y - s.a.y) + s.a.x) {
                inside = !inside;
              }
            }
            return inside ? -best : best;
          },
          [&](float y) { return PaintAt(paint, y); });
}

float Canvas::TextWidth(const std::string& text, float height, float gap) {
  const float k = height / 10.0f;
  float total = 0;
  size_t n = 0;
  for (char c : text) {
    if (const Letter* l = FindLetter(c)) {
      total += l->advance * k;
      ++n;
    }
  }
  return n ? total + gap * height * float(n - 1) : 0.0f;
}

void Canvas::StrokeText(const std::string& text, float cx, float cy, float height, float width,
                        const Paint& paint, float gap) {
  const float k = height / 10.0f;
  float x = cx - TextWidth(text, height, gap) / 2.0f;
  for (char c : text) {
    const Letter* l = FindLetter(c);
    if (!l) continue;
    const Path glyph = Path(l->path, float(scale_) * k).Transformed(x, cy - height / 2.0f, k);
    StrokePath(glyph, width, paint);
    x += l->advance * k + gap * height;
  }
}

void Canvas::Finish(std::vector<uint8_t>* rgba, Colour background) const {
  rgba->assign(size_t(width_) * size_t(height_) * 4, 0);
  const auto byte = [](float v) { return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
  for (size_t i = 0; i < size_t(width_) * size_t(height_); ++i) {
    const float* px = &premul_[i * 4];
    uint8_t* out = &(*rgba)[i * 4];
    const float a = px[3];
    if (a <= 1.0f / 512.0f) {
      out[0] = byte(background.r);
      out[1] = byte(background.g);
      out[2] = byte(background.b);
      out[3] = 0;
      continue;
    }
    out[0] = byte(px[0] / a);
    out[1] = byte(px[1] / a);
    out[2] = byte(px[2] / a);
    out[3] = byte(a);
  }
}

// ================================================================================================
// Mips

std::vector<Level> MakeMipChain(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) {
  std::vector<Level> chain;
  Level top;
  top.width = width;
  top.height = height;
  top.rgba = rgba;
  chain.push_back(std::move(top));
  while (chain.back().width > 1 || chain.back().height > 1) {
    const Level& src = chain.back();
    Level dst;
    dst.width = std::max(1u, src.width / 2);
    dst.height = std::max(1u, src.height / 2);
    dst.rgba.assign(size_t(dst.width) * dst.height * 4, 0);
    for (uint32_t y = 0; y < dst.height; ++y) {
      for (uint32_t x = 0; x < dst.width; ++x) {
        float pm[3] = {0, 0, 0}, plain[3] = {0, 0, 0}, alpha = 0;
        int n = 0;
        for (uint32_t oy = 0; oy < 2; ++oy) {
          for (uint32_t ox = 0; ox < 2; ++ox) {
            const uint32_t sx = std::min(src.width - 1, x * 2 + ox);
            const uint32_t sy = std::min(src.height - 1, y * 2 + oy);
            const uint8_t* p = &src.rgba[(size_t(sy) * src.width + sx) * 4];
            const float a = float(p[3]) / 255.0f;
            for (int c = 0; c < 3; ++c) {
              pm[c] += float(p[c]) / 255.0f * a;
              plain[c] += float(p[c]) / 255.0f;
            }
            alpha += a;
            ++n;
          }
        }
        uint8_t* out = &dst.rgba[(size_t(y) * dst.width + x) * 4];
        for (int c = 0; c < 3; ++c) {
          const float v = alpha > 1e-4f ? pm[c] / alpha : plain[c] / float(n);
          out[c] = uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
        out[3] = uint8_t(std::clamp(alpha / float(n), 0.0f, 1.0f) * 255.0f + 0.5f);
      }
    }
    chain.push_back(std::move(dst));
  }
  return chain;
}

}  // namespace ss_glyph
