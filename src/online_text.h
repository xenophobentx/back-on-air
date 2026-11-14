// online_text.h: the automatic sign-in's name rule and the log's personal-data filter. No Windows calls, so
// test/onlinetest.cpp checks them on any machine (`make onlinetest`). online_login.cpp and patch.cpp use them.
#pragma once
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

// Latin letters with accents (U+00C0..U+017F) as plain ones: "Árpád" -> "Arpad", "Ő" -> "O", "ß" -> "ss".
inline const char* ss_fold_latin(uint32_t cp) {
  static const char* const kLatin[192] = {
      "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",     // U+00C0
      "D", "N", "O", "O", "O", "O", "O", "", "O", "U", "U", "U", "U", "Y", "Th", "ss",     // U+00D0
      "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",     // U+00E0
      "d", "n", "o", "o", "o", "o", "o", "", "o", "u", "u", "u", "u", "y", "th", "y",      // U+00F0
      "A", "a", "A", "a", "A", "a", "C", "c", "C", "c", "C", "c", "C", "c", "D", "d",      // U+0100
      "D", "d", "E", "e", "E", "e", "E", "e", "E", "e", "E", "e", "G", "g", "G", "g",      // U+0110
      "G", "g", "G", "g", "H", "h", "H", "h", "I", "i", "I", "i", "I", "i", "I", "i",      // U+0120
      "I", "i", "IJ", "ij", "J", "j", "K", "k", "k", "L", "l", "L", "l", "L", "l", "L",    // U+0130
      "l", "L", "l", "N", "n", "N", "n", "N", "n", "n", "N", "n", "O", "o", "O", "o",      // U+0140
      "O", "o", "OE", "oe", "R", "r", "R", "r", "R", "r", "S", "s", "S", "s", "S", "s",    // U+0150
      "S", "s", "T", "t", "T", "t", "T", "t", "U", "u", "U", "u", "U", "u", "U", "u",      // U+0160
      "U", "u", "U", "u", "W", "w", "Y", "y", "Y", "Z", "z", "Z", "z", "Z", "z", "s",      // U+0170
  };
  return cp >= 0xC0 && cp < 0x180 ? kLatin[cp - 0xC0] : nullptr;
}

// Cyrillic (U+0400..U+045F, Ґ ґ) in Latin letters, the common Russian / Ukrainian way: "Дмитрий" -> "Dmitriy".
inline const char* ss_fold_cyrillic(uint32_t cp) {
  static const char* const kUpper[48] = {
      "E", "Yo", "Dj", "G", "Ye", "Dz", "I", "Yi", "J", "Lj", "Nj", "C", "K", "I", "U", "Dz",          // U+0400
      "A", "B", "V", "G", "D", "E", "Zh", "Z", "I", "Y", "K", "L", "M", "N", "O", "P",                 // U+0410
      "R", "S", "T", "U", "F", "Kh", "Ts", "Ch", "Sh", "Shch", "", "Y", "", "E", "Yu", "Ya",          // U+0420
  };
  static const char* const kLower[48] = {
      "a", "b", "v", "g", "d", "e", "zh", "z", "i", "y", "k", "l", "m", "n", "o", "p",                 // U+0430
      "r", "s", "t", "u", "f", "kh", "ts", "ch", "sh", "shch", "", "y", "", "e", "yu", "ya",          // U+0440
      "e", "yo", "dj", "g", "ye", "dz", "i", "yi", "j", "lj", "nj", "c", "k", "i", "u", "dz",          // U+0450
  };
  if (cp >= 0x400 && cp < 0x430) return kUpper[cp - 0x400];
  if (cp >= 0x430 && cp < 0x460) return kLower[cp - 0x430];
  if (cp == 0x490) return "G";
  if (cp == 0x491) return "g";
  return nullptr;
}

// A name as plain ASCII: UTF-8 (loginusers.vdf) decoded, a byte that is not UTF-8 read as Latin-1 (the registry's
// ANSI LastGameNameUsed, an ANSI conf file); accents and Cyrillic folded as above; anything else (CJK, symbols) left
// out.
inline std::string ss_fold_name(const std::string& in) {
  std::string out;
  const unsigned char* s = reinterpret_cast<const unsigned char*>(in.data());
  const size_t n = in.size();
  for (size_t i = 0; i < n;) {
    uint32_t cp = s[i];
    size_t len = 1;
    if (cp >= 0xC0) {
      const size_t want = cp >= 0xF0 ? 4 : cp >= 0xE0 ? 3 : 2;
      bool ok = cp < 0xF8 && i + want <= n;
      uint32_t v = cp & (0x3F >> (want - 1));
      for (size_t k = 1; ok && k < want; ++k) {
        if ((s[i + k] & 0xC0) != 0x80) ok = false;
        v = (v << 6) | (s[i + k] & 0x3F);
      }
      if (ok && v >= 0x80) {
        cp = v;
        len = want;
      }
    }
    i += len;
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (const char* f = ss_fold_latin(cp)) {
      out += f;
    } else if (const char* f2 = ss_fold_cyrillic(cp)) {
      out += f2;
    }
  }
  return out;
}

// The server's name rule (tools/online/rdvd/accounts.py NAME_RE): letters, digits, space . _ -; first one a letter or
// digit; no two spaces in a row. The base is cut to 12 so that "-" and the tag fit in 16. Folded first (above).
inline std::string ss_online_base(const std::string& in) {
  std::string s;
  for (unsigned char c : ss_fold_name(in)) {
    const bool ok = isalnum(c) || c == ' ' || c == '_' || c == '.' || c == '-';
    if (c >= 0x80 || !ok) continue;
    if (s.empty() && !isalnum(c)) continue;
    if (c == ' ' && !s.empty() && s.back() == ' ') continue;
    s += static_cast<char>(c);
  }
  if (s.size() > 12) s.resize(12);
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

// The base when a name was found but nothing of it is left (a name in Chinese, Japanese, Korean, Arabic, only
// symbols): the tag after it keeps it apart from other players'.
constexpr char kOnlineFallbackBase[] = "Racer";

// The log goes into public bug reports: the folder name after \Users\, /Users/, \home\ or /home/ (the Windows or
// Linux user name inside a Steam, game or save path) becomes <user>. -> the length written (out is always ended).
inline size_t ss_redact_user_folders(const char* in, char* out, size_t cap) {
  if (!cap) return 0;
  size_t o = 0;
  auto put = [&](char c) {
    if (o + 1 < cap) out[o++] = c;
  };
  static const char* const kMarks[] = {"users", "home"};
  for (size_t i = 0; in[i];) {
    const char c = in[i];
    size_t skip = 0;
    if (c == '\\' || c == '/') {
      for (const char* m : kMarks) {
        const size_t ml = strlen(m);
        size_t k = 0;
        while (k < ml && in[i + 1 + k] && tolower(static_cast<unsigned char>(in[i + 1 + k])) == m[k]) ++k;
        if (k == ml && in[i + 1 + ml] == c && in[i + 2 + ml] && in[i + 2 + ml] != c) {
          skip = 2 + ml;
          break;
        }
      }
    }
    if (!skip) {
      put(c);
      ++i;
      continue;
    }
    for (size_t k = 0; k < skip; ++k) put(in[i + k]);
    i += skip;
    // The user folder ends at the next separator; a path that ends with it, at a space, quote, comma or bracket.
    size_t e = i;
    while (in[e] && in[e] != '\\' && in[e] != '/') ++e;
    if (!in[e]) {
      e = i;
      while (in[e] && !strchr(" \"',)]", in[e])) ++e;
    }
    for (const char* r = "<user>"; *r; ++r) put(*r);
    i = e;
  }
  out[o] = 0;
  return o;
}
