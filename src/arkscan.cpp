// arkscan.cpp: see arkscan.h.
#include "arkscan.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace arkscan {

size_t Lzss(const uint8_t* src, size_t n, uint8_t* dst, size_t cap) {
  uint8_t ring[4096];
  memset(ring, 0, sizeof ring);
  unsigned r = 0xFEE, flags = 0;
  size_t i = 0, out = 0;
  while (out < cap) {
    if (((flags >>= 1) & 0x100) == 0) {
      if (i >= n) break;
      flags = src[i++] | 0xFF00u;
    }
    if (flags & 1) {
      if (i >= n) break;
      const uint8_t c = src[i++];
      dst[out++] = c;
      ring[r] = c;
      r = (r + 1) & 0xFFF;
    } else {
      if (i + 1 >= n) break;
      const unsigned b0 = src[i], b1 = src[i + 1];
      i += 2;
      const unsigned pos = b0 | ((b1 & 0xF0u) << 4), len = (b1 & 15u) + 3;
      for (unsigned k = 0; k < len && out < cap; ++k) {
        const uint8_t c = ring[(pos + k) & 0xFFF];
        dst[out++] = c;
        ring[r] = c;
        r = (r + 1) & 0xFFF;
      }
    }
  }
  return out;
}

namespace {

constexpr uint32_t kVersion = 0x00010000;
constexpr size_t kWindow = 256 * 1024;

bool IsFxc(const uint8_t* p) { return !memcmp(p, "CXF\0", 4) || !memcmp(p, "\0FXC", 4); }

// Reads through a window: members are small and stored back to back, so most come from one read.
struct Window {
  const ReadFn& read;
  uint64_t size;
  std::vector<uint8_t> buf;
  uint64_t at = 0;
  bool Get(uint64_t off, size_t n, uint8_t* dst) {
    if (off > size || n > size - off) return false;
    if (!buf.empty() && off >= at && off + n <= at + buf.size()) {
      memcpy(dst, &buf[off - at], n);
      return true;
    }
    if (n > kWindow / 2) return read(off, dst, n);
    const size_t len = static_cast<size_t>(std::min<uint64_t>(kWindow, size - off));
    buf.resize(len);
    if (!read(off, buf.data(), len)) {
      buf.clear();
      return false;
    }
    at = off;
    memcpy(dst, buf.data(), n);
    return true;
  }
};

uint32_t Rd32(const uint8_t* p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}

// The program starting at p (its version token): its length in dwords, 0 when the tokens do not walk to an end token.
size_t ProgramDwords(const uint8_t* p, size_t avail_dwords) {
  size_t k = 1;
  while (k < avail_dwords && k < 65536) {
    const uint32_t t = Rd32(p + 4 * k);
    if (t == 0x0000FFFFu) return k + 1;
    if ((t & 0xFFFFu) == 0xFFFEu) {  // comment: its length in dwords in bits 16..30
      k += 1 + ((t >> 16) & 0x7FFFu);
      continue;
    }
    const uint32_t op = t & 0xFFFFu;
    if (op > 0x60u || (t & 0x80000000u)) return 0;  // not an SM2/SM3 instruction token
    k += 1 + ((t >> 24) & 0x0Fu);
  }
  return 0;
}

}  // namespace

bool ScanArchive(const ReadFn& read, uint64_t file_size, const std::function<void(const uint8_t*, size_t)>& on_fxc,
                 Stats* stats, uint32_t max_member) {
  Stats local;
  Stats& st = stats ? *stats : local;
  uint8_t head[0x18];
  if (file_size < sizeof head || !read(0, head, sizeof head)) return false;
  const uint32_t ver = Rd32(head), f4 = Rd32(head + 4), count = Rd32(head + 8);
  if (ver != kVersion || Rd32(head + 12) != 0x10 || Rd32(head + 16) != 0 || count > 1000000 || f4 != 0x10 + 20 * count ||
      Rd32(head + 20) != f4 || 0x18ull + 20ull * count > file_size)
    return false;
  std::vector<uint8_t> toc(20 * static_cast<size_t>(count));
  if (count && !read(0x18, toc.data(), toc.size())) return false;
  struct Member {
    uint32_t flags, off, csize, usize;
  };
  std::vector<Member> ms(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* e = &toc[20 * static_cast<size_t>(i)];
    ms[i] = {Rd32(e), Rd32(e + 4), Rd32(e + 8), Rd32(e + 12)};
  }
  std::sort(ms.begin(), ms.end(), [](const Member& a, const Member& b) { return a.off < b.off; });
  Window w{read, file_size, {}, 0};
  std::vector<uint8_t> stored, content;
  for (const Member& m : ms) {
    ++st.members;
    if (m.usize < 8 || m.flags > 1 || (m.flags == 1 && m.csize < 5)) continue;
    // The first bytes of the content: the magic.
    uint8_t first[64];
    const size_t nf = std::min<size_t>(m.csize, sizeof first);
    if (!w.Get(m.off, nf, first)) {
      ++st.bad;
      continue;
    }
    uint8_t magic[4];
    if (m.flags == 0) memcpy(magic, first, 4);
    else if (Lzss(first + 4, nf - 4, magic, 4) < 4) continue;
    if (!IsFxc(magic)) continue;
    if (m.csize > max_member || m.usize > max_member) {
      ++st.bad;
      continue;
    }
    stored.resize(m.csize);
    if (!w.Get(m.off, m.csize, stored.data())) {
      ++st.bad;
      continue;
    }
    if (m.flags == 0) {
      content.assign(stored.begin(), stored.begin() + std::min(m.csize, m.usize));
    } else {
      content.resize(m.usize);
      if (Lzss(stored.data() + 4, stored.size() - 4, content.data(), content.size()) != m.usize) {
        ++st.bad;
        continue;
      }
    }
    ++st.fxc;
    st.fxc_bytes += content.size();
    on_fxc(content.data(), content.size());
  }
  return true;
}

void ScanPrograms(const uint8_t* p, size_t n, const std::function<void(const uint32_t*, size_t, bool)>& on_prog) {
  std::vector<uint32_t> code, bare;
  size_t i = 0;
  while (i + 8 <= n) {
    // Version token: minor, major (2 or 3), then FE FF (vertex) or FF FF (pixel).
    const uint8_t major = p[i + 1], kind = p[i + 2];
    if (p[i + 3] != 0xFF || (kind != 0xFE && kind != 0xFF) || (major != 2 && major != 3) || p[i] > 3) {
      ++i;
      continue;
    }
    const size_t dw = ProgramDwords(p + i, (n - i) / 4);
    if (!dw) {
      ++i;
      continue;
    }
    code.resize(dw);
    memcpy(code.data(), p + i, dw * 4);
    const bool vs = kind == 0xFE;
    on_prog(code.data(), dw, vs);
    bare.clear();
    bare.push_back(code[0]);
    for (size_t k = 1; k < dw;) {
      const uint32_t t = code[k];
      const size_t len = (t & 0xFFFFu) == 0xFFFEu ? 1 + ((t >> 16) & 0x7FFFu)
                         : t == 0x0000FFFFu      ? 1
                                                 : 1 + ((t >> 24) & 0x0Fu);
      if ((t & 0xFFFFu) != 0xFFFEu) bare.insert(bare.end(), code.begin() + k, code.begin() + std::min(dw, k + len));
      k += len;
    }
    if (bare.size() != dw) on_prog(bare.data(), bare.size(), vs);
    i += dw * 4;
  }
}

uint64_t ProgramHash(const void* code, size_t bytes, bool vs) {
  const auto* b = static_cast<const uint8_t*>(code);
  uint64_t h = vs ? 0x5653ull : 0x5053ull;
  for (size_t i = 0; i < bytes; ++i) h = (h ^ b[i]) * 0x100000001b3ull;
  return h;
}

}  // namespace arkscan
