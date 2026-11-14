// online_login.cpp: automatic online sign-in (SSPC-25). Only with online_server set (online.cpp) and
// online_autologin not false.
//
// The game's ONLINE entry signs in with the login name and password kept in its online setup object; only when the
// password is empty does it open its "Enter login credentials" dialog. This file puts a name and a password there
// itself, so nobody types anything, and answers the game's "ACCOUNT DOES NOT EXIST. CREATE?" question with yes.
//
// Identity (docs/plan-online.md section 5):
// * The name other players see: the Steam persona name (online_name in splitsecond.conf replaces it), cut down to the
//   server's name rule (letters, digits, space . _ -; at most 12 characters here), then "-" and a 3-character tag:
//   "Speedy Racer-7QK". The tag comes from the key file and the SteamID, so two players with the same Steam name
//   get two names, and it stays the same when a player renames himself on Steam (rdvd from 1ec4ef4 then renames the
//   account instead of making a new one: tools/online/rdvd/auth.py create_account; an older server makes a new one).
// * The password: 24 characters derived from a random 32-byte key (splitsecond-online.key beside the DLL, made on
//   first use) and the SteamID. It never leaves the PC (the game sends only its MD5 hash at account creation, and
//   uses it as the ticket key). Moving to another PC: copy the key file; deleting it makes a new account (a new key
//   gives a new tag, so a new name; the old account stays on the server, out of reach without the old key).
// * Where the Steam identity comes from (the game never loads steam_api.dll): Steam's config/loginusers.vdf (the
//   Windows Steam folder from the registry, or under Proton the Linux Steam folder through Wine's Z: drive), the
//   user picked by the running Steam's ActiveUser, the SteamUser variable, AutoLoginUser (registry / registry.vdf),
//   MostRecent, or the newest timestamp; HKCU\Software\Valve\Steam LastGameNameUsed as a last name source.
//
// Reverse engineering of the Steam build (run-time VAs, unpacked image):
// * online setup object *(0xD98614), made by its getter 0xA286E0 (cdecl, no arguments): +0x10 login (char*), +0x14
//   password (char*), set by 0xA25A10 / 0xA25A20 (thiscall (obj, const char*): the game's string assignment 0x5A2710);
//   +0x18 signed in (non-zero), +0x148 the last sign-in error (0x29A: the account does not exist), +0x21C 1 = busy.
//   The profile save keeps both strings (section ONLINE, RVLogin / RVPassword, 0x41A2F0); its save path copies the
//   password into a 28-byte stack buffer without a bound, so the password must stay under 28 characters.
// * The main menu (vtable slot 0xBB0F58 holds its update 0x47F460, thiscall (this, dt)): +0x90 its state, 2 = shown
//   and idle, 4 = signing in. ONLINE (0x47F05C) copies the password and opens the dialog when it is empty, else signs
//   in. When the sign-in ends with 0x29A it shows TEXT_ACCOUNT_DOES_NOT_EXIST (0x47F661) with the callback 0x47E770
//   (cdecl (flags); 0x10001 = continue: closes the box, state 6, CreateAccount 0xA26810 (login, password), a wait box;
//   the game then signs in with the new account by itself).
//
// What this file does (nothing in the game's code is written): the main menu's update word in the vtable is swapped
// (ss_write_word, only while it holds the known original). The wrapper, on the game thread: while the menu is idle and
// not signed in, puts our name and password into the online setup object when they differ (also after a profile
// load, which resets them); after the original update, when the "does not exist" box has just come up for our name,
// it calls the box's own callback with "continue" (at most 3 times per start).
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "menu.h"
#include "online_text.h"
#include "ss_shared.h"

#define ENTRY_THIS __attribute__((thiscall, force_align_arg_pointer))

namespace {

constexpr uintptr_t kOnlineSetup = 0xD98614;
constexpr uintptr_t kGetOnlineSetup = 0xA286E0;
constexpr uintptr_t kSetLogin = 0xA25A10, kSetPassword = 0xA25A20;
constexpr uintptr_t kMainSlotUpdate = 0xBB0F58;
constexpr uint32_t kOrigMainUpdate = 0x47F460;
constexpr uintptr_t kCreateCallback = 0x47E770;
constexpr uintptr_t kPopups = 0xD64E10;  // *: +0x4C its first word = the shown popup's level (-1: none)
constexpr uint32_t kErrNoAccount = 0x29A;
constexpr size_t kPasswordLen = 24;  // < 28: the save path's buffer (see above)

// ------------------------------------------------------------------------------------------- SHA-256 / HMAC

struct Sha256 {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint8_t buf[64];
  uint64_t len = 0;
  static uint32_t R(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
  void Block(const uint8_t* p) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
        0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
        0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = R(w[i - 15], 7) ^ R(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = R(w[i - 2], 17) ^ R(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t t1 = hh + (R(e, 6) ^ R(e, 11) ^ R(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
      const uint32_t t2 = (R(a, 2) ^ R(a, 13) ^ R(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  void Add(const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (n--) {
      buf[len++ % 64] = *p++;
      if (len % 64 == 0) Block(buf);
    }
  }
  void Done(uint8_t out[32]) {
    const uint64_t bits = len * 8;
    const uint8_t one = 0x80, zero = 0;
    Add(&one, 1);
    while (len % 64 != 56) Add(&zero, 1);
    for (int i = 7; i >= 0; --i) {
      const uint8_t b = static_cast<uint8_t>(bits >> (8 * i));
      Add(&b, 1);
    }
    for (int i = 0; i < 8; ++i)
      for (int j = 0; j < 4; ++j) out[4 * i + j] = static_cast<uint8_t>(h[i] >> (24 - 8 * j));
  }
};

void HmacSha256(const uint8_t* key, size_t kn, const std::string& msg, uint8_t out[32]) {
  uint8_t k[64] = {0}, ip[64], op[64], inner[32];
  if (kn > 64) {
    Sha256 s;
    s.Add(key, kn);
    s.Done(k);
  } else {
    memcpy(k, key, kn);
  }
  for (int i = 0; i < 64; ++i) {
    ip[i] = k[i] ^ 0x36;
    op[i] = k[i] ^ 0x5c;
  }
  Sha256 a;
  a.Add(ip, 64);
  a.Add(msg.data(), msg.size());
  a.Done(inner);
  Sha256 b;
  b.Add(op, 64);
  b.Add(inner, 32);
  b.Done(out);
}

// ------------------------------------------------------------------------------------------- state

struct Identity {
  std::string name;      // the sign-in name (sanitised base + "-" + tag); empty: no automatic sign-in
  std::string password;  // kPasswordLen characters
  std::string source;    // where the base name came from (log, overlay)
};

Identity g_id;
volatile LONG g_ready = 0;  // g_id is complete (the game-thread wrapper may use it)
std::string g_steam_name;   // the Steam persona name found (for the overlay row), may be empty
char g_label_steam[64] = "STEAM NAME NOT FOUND";
char g_label_own[64] = "";
std::string g_own;          // online_name from the conf (stored for the row's choice value)

std::wstring DllDir() {
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&DllDir), &self);
  wchar_t p[MAX_PATH] = {0};
  GetModuleFileNameW(self, p, MAX_PATH);
  std::wstring s(p);
  const size_t sl = s.find_last_of(L"\\/");
  return sl == std::wstring::npos ? std::wstring() : s.substr(0, sl + 1);
}

bool RandomBytes(uint8_t* p, ULONG n) {
  using Fn = BOOLEAN(WINAPI*)(PVOID, ULONG);
  HMODULE a = LoadLibraryA("advapi32.dll");
  auto f = a ? reinterpret_cast<Fn>(GetProcAddress(a, "SystemFunction036")) : nullptr;  // RtlGenRandom
  return f && f(p, n);
}

// The key file: 64 hex digits. Made when missing (logged); an unreadable one is left alone and nothing signs in.
bool LoadKey(uint8_t key[32], bool* made) {
  *made = false;
  const std::wstring path = DllDir() + L"splitsecond-online.key";
  if (FILE* f = _wfopen(path.c_str(), L"rb")) {
    char hex[80] = {0};
    const size_t n = fread(hex, 1, sizeof hex - 1, f);
    fclose(f);
    if (n < 64) return false;
    for (int i = 0; i < 32; ++i) {
      unsigned v = 0;
      if (sscanf(hex + 2 * i, "%2x", &v) != 1) return false;
      key[i] = static_cast<uint8_t>(v);
    }
    return true;
  }
  if (!RandomBytes(key, 32)) return false;
  char hex[66];
  for (int i = 0; i < 32; ++i) sprintf(hex + 2 * i, "%02x", key[i]);
  hex[64] = '\n';
  FILE* f = _wfopen(path.c_str(), L"wb");
  if (!f) return false;
  const bool ok = fwrite(hex, 1, 65, f) == 65;
  fclose(f);
  *made = ok;
  return ok;
}

// ------------------------------------------------------------------------------------------- Steam identity

std::string RegString(const char* sub, const char* value) {
  HKEY k;
  if (RegOpenKeyExA(HKEY_CURRENT_USER, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return std::string();
  char buf[512] = {0};
  DWORD type = 0, n = sizeof buf - 1;
  std::string out;
  if (RegQueryValueExA(k, value, nullptr, &type, reinterpret_cast<BYTE*>(buf), &n) == ERROR_SUCCESS && type == REG_SZ)
    out = buf;
  RegCloseKey(k);
  return out;
}

DWORD RegDword(const char* sub, const char* value) {
  HKEY k;
  if (RegOpenKeyExA(HKEY_CURRENT_USER, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return 0;
  DWORD v = 0, type = 0, n = sizeof v;
  if (RegQueryValueExA(k, value, nullptr, &type, reinterpret_cast<BYTE*>(&v), &n) != ERROR_SUCCESS || type != REG_DWORD) v = 0;
  RegCloseKey(k);
  return v;
}

std::string ReadFileA(const std::string& path) {
  std::string s;
  if (FILE* f = fopen(path.c_str(), "rb")) {
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0 && s.size() < (1u << 20)) s.append(b, n);
    fclose(f);
  }
  return s;
}

// The quoted strings and braces of a VDF text file, in order.
std::vector<std::string> VdfTokens(const std::string& s) {
  std::vector<std::string> t;
  for (size_t i = 0; i < s.size();) {
    const char c = s[i];
    if (c == '{' || c == '}') {
      t.push_back(std::string(1, c));
      ++i;
    } else if (c == '"') {
      std::string v;
      for (++i; i < s.size() && s[i] != '"'; ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) ++i;
        v += s[i];
      }
      ++i;
      t.push_back(v);
    } else {
      ++i;
    }
  }
  return t;
}

using Fields = std::map<std::string, std::string>;  // lower-case key -> value

// loginusers.vdf: "users" { "<id64>" { "AccountName" ".." "PersonaName" ".." ... } ... }
std::map<std::string, Fields> LoginUsers(const std::string& text) {
  std::map<std::string, Fields> users;
  const std::vector<std::string> t = VdfTokens(text);
  for (size_t i = 0; i + 1 < t.size(); ++i) {
    if (t[i].size() != 17 || t[i + 1] != "{" || t[i].find_first_not_of("0123456789") != std::string::npos) continue;
    Fields f;
    size_t j = i + 2;
    for (; j + 1 < t.size() && t[j] != "}"; j += 2) {
      std::string k = t[j];
      for (char& c : k) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
      f[k] = t[j + 1];
    }
    users[t[i]] = f;
    i = j;
  }
  return users;
}

// The value of the first "key" "value" pair with that key (case-insensitive) in a VDF text.
std::string VdfValue(const std::string& text, const char* key) {
  const std::vector<std::string> t = VdfTokens(text);
  for (size_t i = 0; i + 1 < t.size(); ++i)
    if (_stricmp(t[i].c_str(), key) == 0 && t[i + 1] != "{") return t[i + 1];
  return std::string();
}

std::string UnixToWine(const std::string& p) {
  if (p.empty() || p[0] != '/') return p;
  std::string w = "Z:" + p;
  for (char& c : w)
    if (c == '/') c = '\\';
  return w;
}

std::string Env(const char* n) {
  const char* v = getenv(n);
  return v ? v : "";
}

struct SteamFound {
  std::string persona, id64, how;
};

SteamFound FindSteam() {
  SteamFound out;
  // Steam folders that may hold config/loginusers.vdf, with the registry.vdf of a Linux Steam beside them.
  std::vector<std::string> roots;
  const std::string reg_path = RegString("Software\\Valve\\Steam", "SteamPath");
  if (!reg_path.empty()) roots.push_back(reg_path);
  const std::string compat = Env("STEAM_COMPAT_CLIENT_INSTALL_PATH");
  if (!compat.empty()) roots.push_back(UnixToWine(compat));
  std::vector<std::string> homes;
  if (!Env("HOME").empty() && Env("HOME")[0] == '/') homes.push_back(Env("HOME"));
  for (const char* u : {"USER", "LOGNAME"})
    if (!Env(u).empty() && Env(u) != "steamuser") homes.push_back("/home/" + Env(u));
  for (const std::string& h : homes) {
    roots.push_back(UnixToWine(h + "/.local/share/Steam"));
    roots.push_back(UnixToWine(h + "/.steam/steam"));
  }
  std::string text, file;
  for (const std::string& r : roots) {
    text = ReadFileA(r + "\\config\\loginusers.vdf");
    if (!text.empty()) {
      file = r;
      break;
    }
  }
  const std::map<std::string, Fields> users = LoginUsers(text);

  // Which user runs Steam now.
  const DWORD active = RegDword("Software\\Valve\\Steam\\ActiveProcess", "ActiveUser");
  std::string auto_login = RegString("Software\\Valve\\Steam", "AutoLoginUser");
  if (auto_login.empty()) {
    for (const std::string& h : homes) {
      auto_login = VdfValue(ReadFileA(UnixToWine(h + "/.steam/registry.vdf")), "AutoLoginUser");
      if (!auto_login.empty()) break;
    }
  }
  const std::string steam_user = Env("SteamUser");
  auto by_account = [&](const std::string& acct) -> std::string {
    for (const auto& u : users) {
      auto it = u.second.find("accountname");
      if (it != u.second.end() && !acct.empty() && _stricmp(it->second.c_str(), acct.c_str()) == 0) return u.first;
    }
    return std::string();
  };
  std::string id;
  if (active) {
    char b[24];
    sprintf(b, "%llu", 76561197960265728ULL + active);
    id = b;
    out.how = "the running Steam's ActiveUser";
  }
  if (id.empty() && !(id = by_account(steam_user)).empty()) out.how = "the SteamUser variable";
  if (id.empty() && !(id = by_account(auto_login)).empty()) out.how = "AutoLoginUser";
  if (id.empty())
    for (const auto& u : users) {
      auto it = u.second.find("mostrecent");
      if (it != u.second.end() && it->second == "1") {
        id = u.first;
        out.how = "MostRecent";
        break;
      }
    }
  if (id.empty() && users.size() == 1) {
    id = users.begin()->first;
    out.how = "the only user";
  }
  if (id.empty()) {
    long long best = -1;
    for (const auto& u : users) {
      auto it = u.second.find("timestamp");
      const long long ts = it == u.second.end() ? 0 : atoll(it->second.c_str());
      if (ts > best) {
        best = ts;
        id = u.first;
        out.how = "the newest timestamp";
      }
    }
  }
  out.id64 = id;
  auto it = users.find(id);
  if (it != users.end()) {
    auto p = it->second.find("personaname");
    if (p != it->second.end()) out.persona = p->second;
  }
  if (!out.persona.empty()) {
    out.how = "loginusers.vdf in " + file + ", user by " + out.how;
  } else {
    out.persona = RegString("Software\\Valve\\Steam", "LastGameNameUsed");
    if (!out.persona.empty()) out.how = "the registry's LastGameNameUsed" + (id.empty() ? std::string() : ", id by " + out.how);
  }
  return out;
}

std::string Upper(std::string s) {
  for (char& c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
  return s;
}

void RegisterRows(bool with_name) {
  // GAME page (menu.cpp's key table): the sign-in switch and the name in use. The overlay has no text entry: another
  // name is set as online_name in splitsecond.conf; the row then offers it and the Steam name.
  ss_menu_register(MenuChoiceRow("online_autologin", "ONLINE SIGN-IN", MENU_PAGE_GAME, MENU_RESTART, "true",
                                 {{"true", "AUTOMATIC"}, {"false", "TYPE NAME AND PASSWORD"}}, nullptr));
  if (!with_name) return;
  if (g_own.empty())
    ss_menu_register(MenuChoiceRow("online_name", "ONLINE NAME", MENU_PAGE_GAME, MENU_RESTART, "", {{"", g_label_steam}}, nullptr));
  else
    ss_menu_register(MenuChoiceRow("online_name", "ONLINE NAME", MENU_PAGE_GAME, MENU_RESTART, "",
                                   {{"", g_label_steam}, {g_own.c_str(), g_label_own}}, nullptr));
}

// ------------------------------------------------------------------------------------------- game thread

using UpdateFn = void(__attribute__((thiscall)) *)(void*, float);
using SetFn = void(__attribute__((thiscall)) *)(void*, const char*);
using GetFn = void* (*)();
using CallbackFn = void (*)(unsigned);

inline uint32_t Rd(uintptr_t va) { return *reinterpret_cast<const volatile uint32_t*>(va); }

bool SameString(uintptr_t p, const std::string& want) {
  if (!p) return want.empty();
  const char* s = reinterpret_cast<const char*>(p);
  return strncmp(s, want.c_str(), want.size() + 1) == 0;
}

DWORD g_confirm_at = 0;  // tick when the "does not exist" box came up for our name (0: not armed)
int g_confirms = 0;
int g_failed_logs = 0;  // "sign-in failed" lines written (at most 3 per start)
bool g_put_logged = false;

void ENTRY_THIS MainMenuUpdate(void* self, float dt) {
  const uintptr_t s = reinterpret_cast<uintptr_t>(self);
  const bool ready = g_ready && !g_id.name.empty();
  uint32_t before = s ? Rd(s + 0x90) : 0;
  if (ready && before == 2) {
    uintptr_t os = Rd(kOnlineSetup);
    if (!os) os = reinterpret_cast<uintptr_t>(reinterpret_cast<GetFn>(kGetOnlineSetup)());
    if (os && Rd(os + 0x21C) != 1 && Rd(os + 0x18) == 0 &&
        (!SameString(Rd(os + 0x10), g_id.name) || !SameString(Rd(os + 0x14), g_id.password))) {
      reinterpret_cast<SetFn>(kSetLogin)(reinterpret_cast<void*>(os), g_id.name.c_str());
      reinterpret_cast<SetFn>(kSetPassword)(reinterpret_cast<void*>(os), g_id.password.c_str());
      if (!g_put_logged) ss_log("[online] sign-in name and password put in place for the ONLINE menu");
      g_put_logged = true;
    }
  }
  reinterpret_cast<UpdateFn>(kOrigMainUpdate)(self, dt);
  if (!ready || !s) return;
  const uint32_t after = Rd(s + 0x90);
  const uintptr_t os = Rd(kOnlineSetup);
  if (!os) return;
  if (before == 4 && after == 2 && Rd(os + 0x148) == kErrNoAccount && Rd(os + 0x18) == 0 &&
      SameString(Rd(os + 0x10), g_id.name) && g_confirms < 3) {
    g_confirm_at = GetTickCount() | 1;
    ss_log("[online] the server does not know \"%s\" yet: the game's create-account question is answered with yes",
           g_id.name.c_str());
    return;
  }
  // Any other failure of a sign-in with our name. The game shows its own failure text and does not try again by
  // itself (the player picks ONLINE again). With the server up, the usual cause is a name that belongs to another
  // key: the server knows the name, the ticket it sends is sealed with that account's password, and the game cannot
  // open it. The tag comes from the key, so another key gives the same name only when its 3-character tag matches too
  // (1 in 32768); a lost or new key normally gives a new tag, so a new name and a new account.
  if (before == 4 && after == 2 && Rd(os + 0x18) == 0 && Rd(os + 0x148) != kErrNoAccount &&
      SameString(Rd(os + 0x10), g_id.name) && g_failed_logs < 3) {
    ++g_failed_logs;
    ss_log("[online] sign-in as \"%s\" failed (game error %08x). If the server is up, the name may belong to another "
           "key: put back the splitsecond-online.key that made it, or set online_name in splitsecond.conf",
           g_id.name.c_str(), Rd(os + 0x148));
  }
  if (!g_confirm_at) return;
  const uintptr_t pq = Rd(kPopups);
  const int level = pq ? static_cast<int>(Rd(pq + 0x4C)) : -1;
  if (after != 2 || level < 0) {  // the player answered it, or it went away
    g_confirm_at = 0;
    return;
  }
  if (GetTickCount() - g_confirm_at < 300) return;  // let the box come up first
  g_confirm_at = 0;
  ++g_confirms;
  reinterpret_cast<CallbackFn>(kCreateCallback)(0x10001);
  ss_log("[online] create-account question answered (continue): the game makes the account and signs in");
}

bool HookMainMenu() {
  for (int i = 0; i < 600; ++i) {
    MEMORY_BASIC_INFORMATION mi;
    if (SS_VQ(reinterpret_cast<const void*>(kMainSlotUpdate), &mi, sizeof mi) && mi.State == MEM_COMMIT &&
        !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && mi.Protect) {
      const uint32_t cur = Rd(kMainSlotUpdate);
      if (cur != kOrigMainUpdate) {
        if (i < 599) {  // the image may still be unpacking
          Sleep(100);
          continue;
        }
        ss_log("[online] the main menu's update slot holds %08x, not the known build's: automatic sign-in off", cur);
        return false;
      }
      const uint32_t hook = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&MainMenuUpdate));
      if (!ss_write_word(reinterpret_cast<void*>(kMainSlotUpdate), hook)) {
        ss_log("[online] cannot write the main menu's vtable (error %lu): automatic sign-in off", GetLastError());
        return false;
      }
      return true;
    }
    Sleep(100);
  }
  ss_log("[online] the main menu's vtable never became readable: automatic sign-in off");
  return false;
}

DWORD WINAPI Worker(LPVOID) {
  uint8_t key[32];
  bool made = false;
  if (!LoadKey(key, &made)) {
    ss_log("[online] splitsecond-online.key beside the DLL cannot be read or made: automatic sign-in off");
    return 0;
  }
  const SteamFound st = FindSteam();
  g_steam_name = st.persona;
  std::string base, source;
  const std::string own_base = ss_online_base(g_own);
  std::string steam_base = ss_online_base(st.persona);
  if (!g_own.empty() && !own_base.empty()) {
    base = own_base;
    source = "online_name in splitsecond.conf";
  } else if (!steam_base.empty()) {
    base = steam_base;
    source = "Steam (" + st.how + ")";
  } else if (!st.persona.empty() || !g_own.empty()) {
    // A name with no letter or digit the server takes (Chinese, Japanese, Korean, Arabic, only symbols).
    base = steam_base = kOnlineFallbackBase;
    source = std::string(g_own.empty() ? "Steam (" + st.how + ")" : "online_name in splitsecond.conf") +
             ", which has no letter or digit the server takes: \"" + kOnlineFallbackBase + "\" in its place";
  }
  const std::string id = st.id64.empty() ? "none" : st.id64;
  uint8_t mac[32];
  HmacSha256(key, 32, "split-second-online tag " + id, mac);
  static const char kTag[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
  std::string tag = "-";
  for (int i = 0; i < 3; ++i) tag += kTag[mac[i] & 31];
  HmacSha256(key, 32, "split-second-online password " + id, mac);
  static const char kPw[] = "abcdefghijkmnpqrstuvwxyz23456789";
  std::string pw;
  for (size_t i = 0; i < kPasswordLen; ++i) pw += kPw[mac[i] & 31];
  SecureZeroMemory(key, sizeof key);
  SecureZeroMemory(mac, sizeof mac);

  if (!st.persona.empty())
    snprintf(g_label_steam, sizeof g_label_steam, "%s (STEAM)",
             Upper((steam_base.empty() ? std::string(kOnlineFallbackBase) : steam_base) + tag).c_str());
  if (!g_own.empty())
    snprintf(g_label_own, sizeof g_label_own, "%s (SETTINGS FILE)",
             Upper((own_base.empty() ? base : own_base) + tag).c_str());
  RegisterRows(true);

  const std::string shown_id = st.id64.size() > 4 ? "..." + st.id64.substr(st.id64.size() - 4) : "not found";
  if (base.empty()) {
    ss_log("[online] no Steam name found (SteamID %s) and no online_name in splitsecond.conf: automatic sign-in off, "
           "the game's own dialog asks", shown_id.c_str());
    return 0;
  }
  g_id.name = base + tag;
  g_id.password = pw;
  g_id.source = source;
  ss_log("[online] sign-in name \"%s\" from %s; SteamID %s; key file %s", g_id.name.c_str(), source.c_str(),
         shown_id.c_str(), made ? "made now (a new account will be created)" : "found");
  if (!HookMainMenu()) return 0;
  MemoryBarrier();
  InterlockedExchange(&g_ready, 1);
  ss_log("[online] automatic sign-in on (main menu update %p wrapped)", reinterpret_cast<void*>(kMainSlotUpdate));
  return 0;
}

}  // namespace

// online.cpp, once online_server is set.
void ss_online_login_start() {
  std::string v = ss_conf("online_autologin");
  if (v == "false" || v == "0" || v == "off") {
    ss_log("[online] online_autologin = false: the game's own sign-in dialog");
    RegisterRows(false);
    return;
  }
  g_own = ss_conf("online_name");
  while (!g_own.empty() && (g_own.back() == ' ' || g_own.back() == '\t' || g_own.back() == '\r')) g_own.pop_back();
  CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
}
