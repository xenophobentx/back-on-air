// online.cpp: measurement spike for "real online" (docs/plan-online.md).
//
// The game's Quazal Rendez-Vous backend (splitsecpclive.quazal.net, UDP 31120) is gone. With
//   online_server = host[:port]
// in splitsecond.conf (no line or an empty value: the project's server, kOnlineDefaultServer; off: this file does nothing at
// all, no hook, no thread) the name lookup of every *.quazal.net host is answered with the given address, so the game's
// own online menu talks to a server of ours.
//
// How: a 5-byte jump on the entry of ws2_32.dll's exported gethostbyname (the only resolver the game imports), in
// the system DLL's own memory, put in once at start-up. The game's code and its import table are never written.
// If the entry does not start with instructions this file knows how to move, the fallback is a data write: the
// host name string in the game's read-only data (0xC4BD24, "splitsecpclive.quazal.net", 28 bytes of room) is
// replaced by the dotted address through ss_write_word (gethostbyname takes a dotted address as it is).
//
// The port is an immediate in the game's code (31120) and is not redirected: the server must listen on UDP 31120.
// A host name in online_server (the default is one) is resolved once at start on the worker thread (ResolveServer,
// getaddrinfo, IPv4 only, logged). If that fails the hook asks the system again at each game lookup, so a network that
// comes up late still works; nothing falls back to a built-in address.
// Every lookup is logged: "[online] gethostbyname(name) -> a.b.c.d" and whether it was redirected.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "online_conf.h"
#include "ss_shared.h"

namespace {

using GetHostByNameFn = hostent*(WINAPI*)(const char*);

constexpr uintptr_t kLiveHostString = 0xC4BD24;  // .rdata: "splitsecpclive.quazal.net\0" + padding up to 0xC4BD40
constexpr char kLiveHost[] = "splitsecpclive.quazal.net";
constexpr int kGamePort = kOnlineGamePort;

std::string g_host;            // online_server without the port
int g_port = kGamePort;
volatile uint32_t g_addr = INADDR_NONE;  // network order; INADDR_NONE = g_host is a name not resolved yet
GetHostByNameFn g_real = nullptr;
volatile long g_logged = 0;

// The answer handed to the game for a redirected name (the game copies the address at once).
hostent g_he;
char g_he_name[64];
char* g_he_aliases[1] = {nullptr};
uint32_t g_he_addr;
char* g_he_list[2] = {reinterpret_cast<char*>(&g_he_addr), nullptr};

bool IsBackendName(const char* name) {
  const size_t n = strlen(name);
  static const char kSuffix[] = ".quazal.net";
  const size_t s = sizeof kSuffix - 1;
  return n > s && _stricmp(name + n - s, kSuffix) == 0;
}

// What kind of address, for the log: the address itself, like the PC's own name, does not go into a bug report.
const char* AddressKind(const uint8_t* a) {
  if (a[0] == 127) return "a loopback address";
  if (a[0] == 10 || (a[0] == 172 && (a[1] & 0xF0) == 16) || (a[0] == 192 && a[1] == 168)) return "a LAN address";
  if (a[0] == 169 && a[1] == 254) return "a link-local address";
  if (a[0] == 100 && (a[1] & 0xC0) == 64) return "a carrier-NAT address";
  if (a[0] == 0) return "0.0.0.0";
  return "a public address";
}

// The game's server names are logged as they are, with the address they get; any other name (the PC's own, at the
// game's start) only as what it is.
void LogAnswer(const char* name, const hostent* he, const char* how) {
  if (InterlockedIncrement(&g_logged) > 200) return;
  const bool ours = IsBackendName(name);
  const char* shown = name;
  if (!ours) {
    bool is_own = false;
    for (COMPUTER_NAME_FORMAT f : {ComputerNameDnsHostname, ComputerNameNetBIOS, ComputerNameDnsFullyQualified}) {
      char own[256] = {0};
      DWORD n = sizeof own;
      if (GetComputerNameExA(f, own, &n) && _stricmp(own, name) == 0) is_own = true;
    }
    shown = is_own ? "<this PC's name>" : "<another name>";
  }
  if (he && he->h_addrtype == AF_INET && he->h_addr_list && he->h_addr_list[0]) {
    const uint8_t* a = reinterpret_cast<const uint8_t*>(he->h_addr_list[0]);
    if (ours)
      ss_log("[online] gethostbyname(%s) -> %u.%u.%u.%u%s", shown, a[0], a[1], a[2], a[3], how);
    else
      ss_log("[online] gethostbyname(%s) -> %s%s", shown, AddressKind(a), how);
  } else {
    ss_log("[online] gethostbyname(%s) -> no address (error %d)%s", shown, WSAGetLastError(), how);
  }
}

hostent* WINAPI HookGetHostByName(const char* name) {
  if (!name || !IsBackendName(name)) {
    hostent* he = g_real(name);
    if (name) LogAnswer(name, he, "");
    return he;
  }
  uint32_t addr = g_addr;
  if (addr == INADDR_NONE) {  // online_server is a name: ask the system for it, in the game's place
    hostent* he = g_real(g_host.c_str());
    if (!he || he->h_addrtype != AF_INET || !he->h_addr_list || !he->h_addr_list[0]) {
      LogAnswer(name, nullptr, " (online_server did not resolve; the lookup fails)");
      return nullptr;
    }
    memcpy(&addr, he->h_addr_list[0], 4);
  }
  g_he_addr = addr;
  lstrcpynA(g_he_name, name, sizeof g_he_name);
  g_he.h_name = g_he_name;
  g_he.h_aliases = g_he_aliases;
  g_he.h_addrtype = AF_INET;
  g_he.h_length = 4;
  g_he.h_addr_list = g_he_list;
  LogAnswer(name, &g_he, " (redirected to online_server)");
  return &g_he;
}

// Length of one instruction at p if it is one of the position-independent ones a function entry is made of
// (MSVC hot-patch and frame prologues, the ones mingw/clang emit for Wine's PE builtins); 0 = not known.
size_t KnownLength(const uint8_t* p) {
  const uint8_t a = p[0], b = p[1];
  if (a >= 0x50 && a <= 0x57) return 1;                                  // push reg
  if (a == 0x90) return 1;                                               // nop
  if (a == 0x8B && (b == 0xFF || b == 0xEC)) return 2;                   // mov edi,edi / mov ebp,esp
  if (a == 0x89 && b == 0xE5) return 2;                                  // mov ebp,esp
  if (a == 0x83 && (b == 0xEC || b == 0xE4)) return 3;                   // sub esp,ib / and esp,ib
  if (a == 0x81 && b == 0xEC) return 6;                                  // sub esp,id
  if (a == 0x6A) return 2;                                               // push ib
  if (a == 0x68) return 5;                                               // push id
  if (a >= 0xB8 && a <= 0xBF) return 5;                                  // mov reg,id
  if ((a == 0x31 || a == 0x33) && b >= 0xC0) return 2;                   // xor reg,reg
  if ((a == 0x8B || a == 0x8D) && (b & 0xC7) == 0x44 && p[2] == 0x24) return 4;  // mov/lea reg,[esp+ib]
  if (a == 0x8B && (b & 0xC7) == 0x45) return 3;                         // mov reg,[ebp+ib]
  return 0;
}

bool Detour(uint8_t* t) {
  size_t n = 0;
  while (n < 5) {
    const size_t k = KnownLength(t + n);
    if (!k) {
      ss_log("[online] ws2_32!gethostbyname starts %02X %02X %02X %02X %02X %02X %02X %02X: not an entry this build "
             "knows how to move",
             t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7]);
      return false;
    }
    n += k;
  }
  auto tr = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!tr) return false;
  memcpy(tr, t, n);
  tr[n] = 0xE9;
  const int32_t back = static_cast<int32_t>((t + n) - (tr + n + 5));
  memcpy(tr + n + 1, &back, 4);
  FlushInstructionCache(GetCurrentProcess(), tr, 32);
  g_real = reinterpret_cast<GetHostByNameFn>(tr);  // the original is reachable before the jump goes in
  // One locked 8-byte write, as audio.cpp does for fmodex.dll: the aligned qword when it holds all five bytes.
  const uintptr_t ta = reinterpret_cast<uintptr_t>(t);
  uint8_t* q = (ta & 7) <= 3 ? reinterpret_cast<uint8_t*>(ta & ~static_cast<uintptr_t>(7)) : t;
  const size_t at = static_cast<size_t>(t - q);
  DWORD old;
  if (!VirtualProtect(q, 8, PAGE_EXECUTE_READWRITE, &old)) {
    ss_log("[online] cannot unprotect ws2_32!gethostbyname (error %lu)", GetLastError());
    return false;
  }
  LONGLONG cur, now;
  memcpy(&cur, q, 8);
  uint8_t nb[8];
  memcpy(nb, &cur, 8);
  nb[at] = 0xE9;
  const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint8_t*>(&HookGetHostByName) - (t + 5));
  memcpy(nb + at + 1, &rel, 4);
  memcpy(&now, nb, 8);
  const bool ok = InterlockedCompareExchange64(reinterpret_cast<volatile LONGLONG*>(q), now, cur) == cur;
  FlushInstructionCache(GetCurrentProcess(), q, 8);
  DWORD dummy;
  VirtualProtect(q, 8, old, &dummy);
  if (!ok) ss_log("[online] ws2_32!gethostbyname changed while the jump was going in");
  return ok;
}

// Fallback without any hook: the host name in the game's read-only data becomes the dotted address.
bool RewriteHostString() {
  if (g_addr == INADDR_NONE) {
    ss_log("[online] the data fallback needs an address and online_server %s did not resolve", g_host.c_str());
    return false;
  }
  MEMORY_BASIC_INFORMATION mbi{};
  auto s = reinterpret_cast<const char*>(kLiveHostString);
  if (!SS_VQ(s, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || memcmp(s, kLiveHost, sizeof kLiveHost)) {
    ss_log("[online] 0x%X does not hold \"%s\": not the known build, nothing written", static_cast<unsigned>(kLiveHostString),
           kLiveHost);
    return false;
  }
  char buf[28] = {0};
  const uint32_t addr = g_addr;
  const uint8_t* a = reinterpret_cast<const uint8_t*>(&addr);
  wsprintfA(buf, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
  for (int i = 0; i < 28; i += 4) {
    uint32_t w;
    memcpy(&w, buf + i, 4);
    if (!ss_write_word(reinterpret_cast<void*>(kLiveHostString + i), w)) return false;
  }
  ss_log("[online] data fallback: the host name at 0x%X now reads \"%s\" (lookups are not logged in this mode)",
         static_cast<unsigned>(kLiveHostString), buf);
  return true;
}

// online_server is a host name: resolve it here, once, off the game's thread. Success: the hook answers from memory
// and the data fallback has an address to write. Failure is logged and g_addr stays INADDR_NONE (the hook then asks
// the system at each lookup). Never a built-in address in its place.
void ResolveServer() {
  if (g_addr != INADDR_NONE) return;  // online_server is a dotted address already
  WSADATA wd;
  const int ws = WSAStartup(MAKEWORD(2, 2), &wd);
  if (ws) {
    ss_log("[online] WSAStartup failed (error %d): %s is looked up when the game asks", ws, g_host.c_str());
    return;
  }
  addrinfo hints{};
  hints.ai_family = AF_INET;  // the game's lookup answers IPv4 only
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* res = nullptr;
  const DWORD t0 = GetTickCount();
  const int rc = getaddrinfo(g_host.c_str(), nullptr, &hints, &res);
  const unsigned ms = static_cast<unsigned>(GetTickCount() - t0);
  if (rc || !res || !res->ai_addr || res->ai_addrlen < sizeof(sockaddr_in)) {
    ss_log("[online] online_server %s does not resolve to an IPv4 address (getaddrinfo error %d, %u ms): check the name "
           "and the network connection; the game's own lookup tries again when the online menu opens",
           g_host.c_str(), rc, ms);
  } else {
    const uint32_t addr = reinterpret_cast<const sockaddr_in*>(res->ai_addr)->sin_addr.s_addr;
    g_addr = addr;
    const uint8_t* a = reinterpret_cast<const uint8_t*>(&addr);
    ss_log("[online] online_server %s resolves to %u.%u.%u.%u (%u ms)", g_host.c_str(), a[0], a[1], a[2], a[3], ms);
  }
  if (res) freeaddrinfo(res);
  WSACleanup();  // the count WSAStartup took; the game's own WSAStartup is separate
}

DWORD WINAPI Worker(LPVOID) {
  HMODULE ws = LoadLibraryA("ws2_32.dll");  // the game imports it: this only takes a reference
  auto target = ws ? reinterpret_cast<uint8_t*>(GetProcAddress(ws, "gethostbyname")) : nullptr;
  HMODULE owner = nullptr;
  if (target) {
    // A forwarder stub or an entry already redirected by somebody else: follow nothing, hook only ws2_32's own code.
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(target), &owner) ||
        owner != ws) {
      ss_log("[online] gethostbyname at %p is not inside ws2_32.dll", static_cast<void*>(target));
      target = nullptr;
    }
  }
  if (target && Detour(target)) {
    ss_log("[online] ws2_32!gethostbyname at %p redirected: *.quazal.net -> %s (the game dials UDP %d)",
           static_cast<void*>(target), g_host.c_str(), kGamePort);
    ResolveServer();  // after the jump: a slow name lookup does not hold the redirection back
    return 0;
  }
  ResolveServer();
  if (!RewriteHostString()) ss_log("[online] no redirection in place: the online menu will look for the original server");
  return 0;
}


// ---- online_probe = true: a read-only look at the lobby's start conditions (a measurement aid, docs/plan-online.md
// section 12). Every 500 ms the fields below are read with ReadProcessMemory (never written) and a line is logged when
// one changes. Addresses are reverse engineering of the unpacked image:
//   net manager  [0xD64E18] (cATVNetManager): +0x30 invite manager (byte +4 = an invite is pending), +0xB8 room state
//                (flags +4 start countdown requested, +5 cancel), +0xBC peer state, +0xC0 game setup (all three are
//                created when the room is entered), +0x10 room manager (+4 = its task state, 6 = in a room)
//   Net-Z holder [0xD986DC]: +0x14 state (2 = Net-Z session created), byte +0x20 busy
//   game mode    [0xD6F830]: +0x5C 1 = ranked (a PUBLIC room: auto start by a timer once 3 players are in), 2 = player
//                match (the host's START GAME, 2 players)
//   online setup [0xD98614]: +0xB0 = a private game was chosen in the menu
uint32_t Peek(uintptr_t a) {
  uint32_t v = 0;
  SIZE_T got = 0;
  if (!a || !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(a), &v, sizeof v, &got) || got != sizeof v)
    return 0xDEADDEAD;
  return v;
}
bool Valid(uint32_t p) { return p && p != 0xDEADDEAD; }

DWORD WINAPI ProbeThread(LPVOID) {
  char last[400] = {0};
  for (;;) {
    Sleep(500);
    const uint32_t nm = Peek(0xD64E18), nz = Peek(0xD986DC), gm = Peek(0xD6F830), os = Peek(0xD98614);
    uint32_t inv = 0, inv4 = 0, rs = 0, rsf = 0, ps = 0, gs = 0, rm = 0, rmst = 0, nzs = 0, nzb = 0, rk = 0, pv = 0;
    if (Valid(nm)) {
      inv = Peek(nm + 0x30);
      if (Valid(inv)) inv4 = Peek(inv + 4) & 0xFF;
      rs = Peek(nm + 0xB8);
      if (Valid(rs)) rsf = Peek(rs + 4) & 0xFFFF;
      ps = Peek(nm + 0xBC);
      gs = Peek(nm + 0xC0);
      rm = Peek(nm + 0x10);
      if (Valid(rm)) rmst = Peek(rm + 4);
    }
    if (Valid(nz)) {
      nzs = Peek(nz + 0x14);
      nzb = Peek(nz + 0x20) & 0xFF;
    }
    if (Valid(gm)) rk = Peek(gm + 0x5C);
    if (Valid(os)) pv = Peek(os + 0xB0);
    char line[400];
    wsprintfA(line,
              "netmgr %08X invite %u roomstate %08X (flags %04X) peerstate %08X gamesetup %08X roommgr task %u | "
              "netz state %u busy %u | ranked-flag %u private %u",
              nm, inv4, rs, rsf, ps, gs, rmst, nzs, nzb, rk, pv);
    if (strcmp(line, last)) {
      strcpy(last, line);
      ss_log("[online-probe] %s", line);
    }
  }
  return 0;
}

}  // namespace

void ss_online_login_start();

void ss_online_start() {
  std::string v;
  if (const char* e = getenv("SS_ONLINE_SERVER")) v = e; else v = ss_conf("online_server");
  // No line, or an empty value (the conf of builds before the release had "online_server =" as its default line):
  // the project's server, so a player who updates and keeps an older splitsecond.conf is online like a new one.
  const OnlineServer srv = ss_parse_online_server(v);
  if (srv.off) {
    ss_log("[online] online_server = %s: online stays off (nothing is hooked)", v.c_str());
    return;
  }
  const bool by_default = srv.by_default;
  g_port = srv.port;
  g_host = srv.host;
  g_addr = inet_addr(g_host.c_str());  // INADDR_NONE when it is a name
  ss_log("[online] online_server = %s (port %d)%s%s", g_host.c_str(), g_port, by_default ? ", the built-in default" : "",
         g_port != kGamePort ? ": the port is fixed in the game's code at 31120 and is NOT redirected by this build" : "");
  CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
  ss_online_login_start();  // online_login.cpp: automatic sign-in (online_autologin, online_name)
  const std::string pr = ss_conf("online_probe");
  if (pr == "true" || pr == "1" || getenv("SS_ONLINE_PROBE")) {
    ss_log("[online-probe] on: the lobby's start fields are logged when they change");
    CreateThread(nullptr, 0, ProbeThread, nullptr, 0, nullptr);
  }
}
