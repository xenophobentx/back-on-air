// arkscantest: src/arkscan.cpp on a synthetic archive built here (no game data). `make arkscantest`.
// Members: a stored shader member, an LZSS shader member (made by a small encoder below, with matches), a member
// that is not a shader, and a shader member at the end. The programs are made up: a vs_3_0 with a comment block, a
// ps_3_0 without one, a ps_2_0 after junk bytes at an odd offset.
#include "../src/arkscan.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c)                                                  \
  do {                                                            \
    if (!(c)) {                                                   \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);    \
      ++g_fail;                                                   \
    }                                                             \
  } while (0)

static void Put32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}

// Greedy Okumura LZSS (ring 4096, start 0xFEE, zero fill, matches 3..18) — the inverse of arkscan::Lzss.
static std::vector<uint8_t> Encode(const std::vector<uint8_t>& in) {
  std::vector<uint8_t> out;
  uint8_t ring[4096] = {};
  unsigned r = 0xFEE;
  size_t i = 0;
  while (i < in.size()) {
    const size_t flag_at = out.size();
    out.push_back(0);
    for (int bit = 0; bit < 8 && i < in.size(); ++bit) {
      unsigned best_len = 0, best_pos = 0;
      for (unsigned p = 0; p < 4096; ++p) {
        if (p == r) continue;
        unsigned l = 0;
        // Copy semantics as the decoder: bytes written during the match are read back.
        uint8_t tmp[4096];
        memcpy(tmp, ring, sizeof ring);
        unsigned rr = r;
        while (l < 18 && i + l < in.size()) {
          const uint8_t c = tmp[(p + l) & 0xFFF];
          if (c != in[i + l]) break;
          tmp[rr] = c;
          rr = (rr + 1) & 0xFFF;
          ++l;
        }
        if (l > best_len) best_len = l, best_pos = p;
        if (best_len == 18) break;
      }
      if (best_len >= 3) {
        out.push_back(static_cast<uint8_t>(best_pos & 0xFF));
        out.push_back(static_cast<uint8_t>(((best_pos >> 4) & 0xF0) | (best_len - 3)));
        for (unsigned k = 0; k < best_len; ++k) {
          ring[r] = in[i + k];
          r = (r + 1) & 0xFFF;
        }
        i += best_len;
      } else {
        out[flag_at] |= static_cast<uint8_t>(1u << bit);
        out.push_back(in[i]);
        ring[r] = in[i];
        r = (r + 1) & 0xFFF;
        ++i;
      }
    }
  }
  return out;
}

static std::vector<uint32_t> Vs3() {
  // vs_3_0; comment of 3 dwords ("CTAB" + 2); dcl_position v0; mov o0, v0 (2 operands); end
  return {0xFFFE0300u, 0x0003FFFEu, 0x42415443u, 1, 2, 0x0200001Fu, 0x80000000u, 0x900F0000u,
          0x02000001u, 0xE00F0000u, 0x90E40000u, 0x0000FFFFu};
}
static std::vector<uint32_t> Ps3() {
  // ps_3_0; mov oC0, c0; end
  return {0xFFFF0300u, 0x02000001u, 0x800F0800u, 0xA0E40000u, 0x0000FFFFu};
}
static std::vector<uint32_t> Ps2() {
  // ps_2_0; mov oC0, c1; nop; end
  return {0xFFFF0200u, 0x02000001u, 0x800F0800u, 0xA0E40001u, 0x00000000u, 0x0000FFFFu};
}

static void Append(std::vector<uint8_t>& v, const std::vector<uint32_t>& code) {
  for (uint32_t t : code) Put32(v, t);
}

int main() {
  // Shader member contents.
  std::vector<uint8_t> fxc1 = {'C', 'X', 'F', 0};
  const char id[] = "0x1234abcd";
  fxc1.insert(fxc1.end(), id, id + sizeof id);  // id with its NUL: odd length, so the programs are unaligned
  Append(fxc1, Vs3());
  Append(fxc1, Ps3());
  std::vector<uint8_t> fxc2 = {0, 'F', 'X', 'C', 0xFF, 0xFF, 0x03};  // junk that looks like the start of a token
  for (int k = 0; k < 40; ++k) fxc2.push_back(static_cast<uint8_t>(k % 5));  // repeats: LZSS matches
  Append(fxc2, Ps2());
  Append(fxc2, Ps3());  // the same program again (as in different combos)
  std::vector<uint8_t> other = {'n', 'o', 't', ' ', 'a', ' ', 's', 'h', 'a', 'd', 'e', 'r'};
  Append(other, Ps3());  // a program in a member that is not a shader member: not reported

  // LZSS round trip.
  {
    std::vector<uint8_t> enc = Encode(fxc2);
    std::vector<uint8_t> dec(fxc2.size());
    CHECK(arkscan::Lzss(enc.data(), enc.size(), dec.data(), dec.size()) == fxc2.size());
    CHECK(dec == fxc2);
    CHECK(enc.size() < fxc2.size());  // there were matches
  }

  // Archive: header, TOC, data (members in a different order from the TOC).
  struct M {
    std::vector<uint8_t> stored;
    uint32_t flags, usize;
  };
  std::vector<M> ms;
  ms.push_back({fxc1, 0, static_cast<uint32_t>(fxc1.size())});
  {
    std::vector<uint8_t> s;
    Put32(s, static_cast<uint32_t>(fxc2.size()));
    std::vector<uint8_t> enc = Encode(fxc2);
    s.insert(s.end(), enc.begin(), enc.end());
    ms.push_back({s, 1, static_cast<uint32_t>(fxc2.size())});
  }
  ms.push_back({other, 0, static_cast<uint32_t>(other.size())});
  const uint32_t count = static_cast<uint32_t>(ms.size());
  std::vector<uint8_t> ark;
  Put32(ark, 0x00010000u);
  Put32(ark, 0x10 + 20 * count);
  Put32(ark, count);
  Put32(ark, 0x10);
  Put32(ark, 0);
  Put32(ark, 0x10 + 20 * count);
  uint32_t off = 0x18 + 20 * count;
  std::vector<uint32_t> offs(count);
  // data order: member 2, 0, 1
  const int order[] = {2, 0, 1};
  for (int k : order) {
    offs[k] = off;
    off += static_cast<uint32_t>(ms[k].stored.size());
  }
  for (uint32_t k = 0; k < count; ++k) {
    Put32(ark, ms[k].flags);
    Put32(ark, offs[k]);
    Put32(ark, static_cast<uint32_t>(ms[k].stored.size()));
    Put32(ark, ms[k].usize);
    Put32(ark, 0x1000 + k);
  }
  for (int k : order) ark.insert(ark.end(), ms[k].stored.begin(), ms[k].stored.end());

  arkscan::ReadFn rd = [&](uint64_t o, void* dst, size_t n) {
    if (o > ark.size() || n > ark.size() - o) return false;
    memcpy(dst, ark.data() + o, n);
    return true;
  };
  std::set<uint64_t> found;
  int programs = 0;
  arkscan::Stats st;
  const bool ok = arkscan::ScanArchive(
      rd, ark.size(),
      [&](const uint8_t* p, size_t n) {
        arkscan::ScanPrograms(p, n, [&](const uint32_t* c, size_t dw, bool vs) {
          ++programs;
          found.insert(arkscan::ProgramHash(c, dw * 4, vs));
        });
      },
      &st);
  CHECK(ok);
  CHECK(st.members == 3);
  CHECK(st.fxc == 2);
  CHECK(st.bad == 0);
  auto hash = [](const std::vector<uint32_t>& c, bool vs) { return arkscan::ProgramHash(c.data(), c.size() * 4, vs); };
  std::vector<uint32_t> vs_bare = Vs3();
  vs_bare.erase(vs_bare.begin() + 1, vs_bare.begin() + 5);  // the comment block left out
  CHECK(found.count(hash(Vs3(), true)));
  CHECK(found.count(hash(vs_bare, true)));
  CHECK(found.count(hash(Ps3(), false)));
  CHECK(found.count(hash(Ps2(), false)));
  CHECK(!found.count(hash(Ps3(), true)));  // the stage is part of the hash
  CHECK(found.size() == 4);
  CHECK(programs == 5);  // vs (full + bare), ps3, ps2, ps3 again

  // Same hash as pipewarm.cpp's ShaderHash (Fnv seeded 0x5653 / 0x5053).
  {
    const uint8_t b[] = {1, 2, 3};
    uint64_t h = 0x5653;
    for (uint8_t c : b) h = (h ^ c) * 0x100000001b3ull;
    CHECK(arkscan::ProgramHash(b, 3, true) == h);
  }

  // Not an archive; a truncated one.
  {
    std::vector<uint8_t> junk(100, 7);
    arkscan::ReadFn rj = [&](uint64_t o, void* dst, size_t n) {
      if (o > junk.size() || n > junk.size() - o) return false;
      memcpy(dst, junk.data() + o, n);
      return true;
    };
    CHECK(!arkscan::ScanArchive(rj, junk.size(), [](const uint8_t*, size_t) {}, nullptr));
    CHECK(!arkscan::ScanArchive(rd, 0x18 + 10, [](const uint8_t*, size_t) {}, nullptr));
  }

  std::printf(g_fail ? "arkscantest: %d FAILED\n" : "arkscantest: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
