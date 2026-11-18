// tunnel.cpp: the peer tunnel (docs/plan-online.md 4.2, milestone M3, SSPC-41): players behind different home routers
// race through our server's relay when they cannot reach each other directly. Takedawn's relay model; the wire format
// and the route policy are in tunnel_core.h (unit-tested), the server side in tools/online/rdvd/relay.py.
//
//   online_relay = auto    (default) direct first, the relay when no direct path answered within 2 s
//   online_relay = always  every peer datagram through the relay (test switch; also hides one's address from players)
//   online_relay = off     no tunnel: the game's own direct peer traffic, as before this file
// SS_ONLINE_RELAY overrides the conf line.
//
// How the game is kept unaware (its code and Net-Z's logic are untouched; only import slots of ws2_32 are written):
//   * The DLL picks a virtual address 10.254.x.y and the game believes it is its own: the interface list
//     (WSAIoctl SIO_GET_INTERFACE_LIST, 0x5E1AD8) and the own host name lookup answer it, so the station URLs the game
//     registers carry it. The server ties it to the Rendez-Vous session (relay.py bind) and tells the other players
//     only that address.
//   * The game's datagrams to a virtual address (WSASendTo at 0x5D380E, sendto at 0x5DC6AE) are redirected to a local
//     "proxy" socket on 127.0.0.1, one per peer address and port; the tunnel thread reads them there and sends them
//     on, directly or through the relay. Datagrams for the game arrive on the tunnel socket and are handed to the game
//     from that proxy over loopback; the receive hooks (WSARecvFrom 0x5D3961 with its overlapped completion
//     WSAGetOverlappedResult 0x5D39D9, recvfrom 0x5DC58C) put the peer's virtual address and port in the source. The
//     payload is never changed, so the game's own sends and receives complete as they always do.
//   * The game's datagrams to the server (sign-in, matchmaking) are not touched.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "online_conf.h"
#include "ss_shared.h"
#include "tunnel_core.h"

uint32_t ss_online_server_addr();  // online.cpp
void ss_tunnel_go(uint32_t server_addr_net);

namespace {

using SendToFn = int(WINAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
using RecvFromFn = int(WINAPI*)(SOCKET, char*, int, int, sockaddr*, int*);
using WSASendToFn = int(WINAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const sockaddr*, int, LPWSAOVERLAPPED,
                                 LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using WSARecvFromFn = int(WINAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, sockaddr*, LPINT, LPWSAOVERLAPPED,
                                   LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using WSAGetOverlappedResultFn = BOOL(WINAPI*)(SOCKET, LPWSAOVERLAPPED, LPDWORD, BOOL, LPDWORD);
using WSAIoctlFn = int(WINAPI*)(SOCKET, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPWSAOVERLAPPED,
                                LPWSAOVERLAPPED_COMPLETION_ROUTINE);

// Import slots of the unpacked image (each is checked to hold ws2_32's own function before it is written).
constexpr uintptr_t kSlotWSASendTo = 0xB9B348, kSlotWSARecvFrom = 0xB9B350, kSlotWSAGetOverlapped = 0xB9B354,
                    kSlotRecvFrom = 0xB9B358, kSlotSendTo = 0xB9B35C, kSlotWSAIoctl = 0xB9B368;
constexpr uint16_t kRelayPort = kOnlineGamePort + 2;  // 31122, the server's relay port
constexpr int kMaxProxies = 32, kMaxPeers = 32, kMaxPending = 64;

SendToFn r_sendto;
RecvFromFn r_recvfrom;
WSASendToFn r_WSASendTo;
WSARecvFromFn r_WSARecvFrom;
WSAGetOverlappedResultFn r_WSAGetOverlappedResult;
WSAIoctlFn r_WSAIoctl;

tun::Policy g_policy = tun::Policy::kAuto;
volatile bool g_hooked = false;
volatile long g_started = 0;
uint32_t g_vip = 0;              // host order
uint32_t g_server = 0;           // network order
SOCKET g_ts = INVALID_SOCKET;    // the tunnel socket
volatile bool g_welcomed = false;
uint8_t g_key[16];
uint32_t g_my_pub = 0;           // host order, as the relay saw us
HANDLE g_welcome_event = nullptr;
CRITICAL_SECTION g_lock;

// The game's sockets that talked to the server. Quazal uses one socket for the server and its peers, but it can make a
// new one later: with its fixed port 9103 taken (a second game on the same machine) every new socket gets a random
// port (seen: auth on 48374, then the secure connection and the peers on 52137, the port in its station URL).
SOCKET g_game_sock = INVALID_SOCKET;  // the newest of them
sockaddr_in g_game_addr{};            // its bound address (127.0.0.1 for 0.0.0.0): proxies deliver here by default
// Net-Z can also send to a peer from a socket that never talked to the server (seen on a second game on one machine:
// port 49805 beside the server socket 52137): every socket that sends to a virtual address counts too.
constexpr int kMaxGamePorts = 16;
uint16_t g_game_ports[kMaxGamePorts];  // network order: the bound ports of all of them (a datagram for one goes there)
volatile long g_ngp = 0;
SOCKET g_peer_socks[kMaxGamePorts];   // sockets already looked at by NotePeerSocket
volatile long g_nps = 0;
bool IsGamePort(uint16_t port_n) {
  for (long i = 0; i < g_ngp; i++)
    if (g_game_ports[i] == port_n) return true;
  return false;
}

struct Proxy {
  uint32_t vip;    // host order
  uint16_t port;   // the peer's game port
  SOCKET s;
  sockaddr_in local;
  DWORD used;      // GetTickCount of the last datagram either way (a full table reuses the longest idle)
};
Proxy g_px[kMaxProxies];
int g_npx = 0;
tun::Peer g_peers[kMaxPeers];
int g_npeers = 0;
struct Pending {
  LPWSAOVERLAPPED ov;
  sockaddr* from;
};
Pending g_pend[kMaxPending];  // overlapped receives not completed yet: a slot is freed at its completion
int g_pend_next = 0;          // when every slot is taken, the next to overwrite (the oldest)
void PendClear(LPWSAOVERLAPPED ov) {  // under g_lock
  for (auto& p : g_pend)
    if (p.ov == ov) p.ov = nullptr, p.from = nullptr;
}

struct Counts {
  volatile long relay_out, relay_in, direct_out, direct_in, outside, probes, refused;
} g_n;

bool IsServer(const sockaddr_in* a) { return a->sin_addr.s_addr == g_server; }
uint32_t HostIp(const sockaddr* a) { return ntohl(reinterpret_cast<const sockaddr_in*>(a)->sin_addr.s_addr); }
bool IsVipAddr(const sockaddr* a, int len) {
  return a && len >= int(sizeof(sockaddr_in)) && a->sa_family == AF_INET && tun::is_vip(HostIp(a)) && HostIp(a) != g_vip;
}
sockaddr_in Sin(uint32_t ip_host, uint16_t port) {
  sockaddr_in s{};
  s.sin_family = AF_INET;
  s.sin_addr.s_addr = htonl(ip_host);
  s.sin_port = htons(port);
  return s;
}
const char* VipText(uint32_t v, char* b) {
  wsprintfA(b, "%u.%u.%u.%u", v >> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255);
  return b;
}

// Under g_lock.
Proxy* ProxyFor(uint32_t vip, uint16_t port, bool make) {
  for (int i = 0; i < g_npx; i++)
    if (g_px[i].vip == vip && g_px[i].port == port) return g_px[i].used = GetTickCount(), &g_px[i];
  if (!make) return nullptr;
  int slot = g_npx;
  if (slot == kMaxProxies) {  // full (every game start gives each player a new virtual address): the longest idle goes
    slot = 0;
    for (int i = 1; i < g_npx; i++)
      if (GetTickCount() - g_px[i].used > GetTickCount() - g_px[slot].used) slot = i;
    char b[20];
    ss_log("[relay] proxy table full (%d): the one for %s:%u, idle %lu s, is reused", kMaxProxies,
           VipText(g_px[slot].vip, b), g_px[slot].port, static_cast<unsigned long>((GetTickCount() - g_px[slot].used) / 1000));
    closesocket(g_px[slot].s);
    g_px[slot].vip = 0, g_px[slot].port = 0, g_px[slot].s = INVALID_SOCKET, g_px[slot].local = {};
  }
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) return nullptr;
  sockaddr_in b = Sin(0x7F000001, 0);
  int bl = sizeof b;
  u_long nb = 1;
  if (bind(s, reinterpret_cast<sockaddr*>(&b), sizeof b) || getsockname(s, reinterpret_cast<sockaddr*>(&b), &bl) ||
      ioctlsocket(s, FIONBIO, &nb)) {
    closesocket(s);
    return nullptr;
  }
  if (slot == g_npx) g_npx++;
  {
    char t[20];
    ss_log("[relay] proxy %d for %s:%u on local port %u", slot, VipText(vip, t), port, ntohs(b.sin_port));
  }
  Proxy& p = g_px[slot];
  p.vip = vip, p.port = port, p.s = s, p.local = b, p.used = GetTickCount();
  return &p;
}
Proxy* ProxyByLocal(const sockaddr* a) {
  const sockaddr_in* in = reinterpret_cast<const sockaddr_in*>(a);
  for (int i = 0; i < g_npx; i++)
    if (g_px[i].local.sin_port == in->sin_port && g_px[i].local.sin_addr.s_addr == in->sin_addr.s_addr) return &g_px[i];
  return nullptr;
}
DWORD g_peer_used[kMaxPeers];
tun::Peer* PeerFor(uint32_t vip, bool make) {
  for (int i = 0; i < g_npeers; i++)
    if (g_peers[i].vip == vip) return g_peer_used[i] = GetTickCount(), &g_peers[i];
  if (!make) return nullptr;
  int slot = g_npeers;
  if (slot == kMaxPeers) {  // full: the longest idle peer is forgotten (it would start with probes again)
    slot = 0;
    for (int i = 1; i < g_npeers; i++)
      if (GetTickCount() - g_peer_used[i] > GetTickCount() - g_peer_used[slot]) slot = i;
    char b[20];
    ss_log("[relay] peer table full (%d): %s, idle %lu s, is forgotten", kMaxPeers, VipText(g_peers[slot].vip, b),
           static_cast<unsigned long>((GetTickCount() - g_peer_used[slot]) / 1000));
  } else {
    g_npeers++;
  }
  g_peer_used[slot] = GetTickCount();
  tun::Peer& p = g_peers[slot];
  p = tun::Peer{};
  p.vip = vip;
  return &p;
}

// The game's socket: learnt from its datagrams to the server; a new socket that talks to the server takes over.
void NoteGameSocket(SOCKET s, const sockaddr* to) {
  if (s == g_game_sock || !to || to->sa_family != AF_INET || !IsServer(reinterpret_cast<const sockaddr_in*>(to))) return;
  sockaddr_in me{};
  int ml = sizeof me;
  if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &ml)) return;
  if (me.sin_addr.s_addr == INADDR_ANY) me.sin_addr.s_addr = htonl(0x7F000001);
  EnterCriticalSection(&g_lock);
  const bool known = IsGamePort(me.sin_port);
  if (!known && g_ngp < kMaxGamePorts) g_game_ports[g_ngp] = me.sin_port, InterlockedIncrement(&g_ngp);
  g_game_addr = me;
  g_game_sock = s;
  LeaveCriticalSection(&g_lock);
  if (!known) ss_log("[relay] the game's socket uses port %u", ntohs(me.sin_port));
}

// A socket of the game that sends to a peer: its port is a game port (its datagrams are carried, replies go to it).
void NotePeerSocket(SOCKET s) {
  for (long i = 0; i < g_nps; i++)
    if (g_peer_socks[i] == s) return;
  sockaddr_in me{};
  int ml = sizeof me;
  if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &ml)) return;
  EnterCriticalSection(&g_lock);
  if (g_nps < kMaxGamePorts) g_peer_socks[g_nps] = s, InterlockedIncrement(&g_nps);
  const bool known = IsGamePort(me.sin_port);
  if (!known && g_ngp < kMaxGamePorts) g_game_ports[g_ngp] = me.sin_port, InterlockedIncrement(&g_ngp);
  LeaveCriticalSection(&g_lock);
  if (!known) ss_log("[relay] the game also sends to peers from port %u", ntohs(me.sin_port));
}

// A send of the game: to a virtual address -> its proxy (the tunnel thread carries it on). -> the address to use.
const sockaddr* MapSend(SOCKET s, const sockaddr* to, int len, sockaddr_in* tmp) {
  if (!to || len < int(sizeof(sockaddr_in)) || to->sa_family != AF_INET) return to;
  NoteGameSocket(s, to);
  const sockaddr_in* in = reinterpret_cast<const sockaddr_in*>(to);
  if (IsVipAddr(to, len) && g_welcomed) {
    NotePeerSocket(s);
    EnterCriticalSection(&g_lock);
    Proxy* p = ProxyFor(HostIp(to), ntohs(in->sin_port), true);
    if (p) *tmp = p->local;
    LeaveCriticalSection(&g_lock);
    return p ? reinterpret_cast<sockaddr*>(tmp) : to;
  }
  if (s == g_game_sock && !IsServer(in) && in->sin_addr.s_addr != htonl(0x7F000001)) InterlockedIncrement(&g_n.outside);
  return to;
}
// A completed receive of the game: a datagram from one of our proxies -> from the peer's virtual address and port.
void MapFrom(sockaddr* from) {
  if (!from || from->sa_family != AF_INET) return;
  EnterCriticalSection(&g_lock);
  if (Proxy* p = ProxyByLocal(from)) *reinterpret_cast<sockaddr_in*>(from) = Sin(p->vip, p->port);
  LeaveCriticalSection(&g_lock);
}

int WINAPI HkSendTo(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen) {
  sockaddr_in tmp;
  const sockaddr* t = MapSend(s, to, tolen, &tmp);
  return r_sendto(s, buf, len, flags, t, t == to ? tolen : int(sizeof tmp));
}
int WINAPI HkWSASendTo(SOCKET s, LPWSABUF b, DWORD n, LPDWORD sent, DWORD flags, const sockaddr* to, int tolen,
                       LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr) {
  sockaddr_in tmp;
  const sockaddr* t = MapSend(s, to, tolen, &tmp);
  return r_WSASendTo(s, b, n, sent, flags, t, t == to ? tolen : int(sizeof tmp), ov, cr);
}
int WINAPI HkRecvFrom(SOCKET s, char* buf, int len, int flags, sockaddr* from, int* fromlen) {
  const int r = r_recvfrom(s, buf, len, flags, from, fromlen);
  if (r >= 0 && g_npx) MapFrom(from);
  return r;
}
int WINAPI HkWSARecvFrom(SOCKET s, LPWSABUF b, DWORD n, LPDWORD got, LPDWORD flags, sockaddr* from, LPINT fromlen,
                         LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr) {
  if (ov && from) {  // noted before the call: the completion can come before the call returns
    EnterCriticalSection(&g_lock);
    Pending* slot = nullptr;
    for (auto& p : g_pend)
      if (p.ov == ov) slot = &p;
    for (int i = 0; !slot && i < kMaxPending; i++)
      if (!g_pend[i].ov) slot = &g_pend[i];
    if (!slot) slot = &g_pend[g_pend_next], g_pend_next = (g_pend_next + 1) % kMaxPending;
    slot->ov = ov, slot->from = from;
    LeaveCriticalSection(&g_lock);
  }
  const int r = r_WSARecvFrom(s, b, n, got, flags, from, fromlen, ov, cr);
  if (r == 0 && g_npx) MapFrom(from);
  if (ov && from && (r == 0 || WSAGetLastError() != WSA_IO_PENDING)) {  // done at once (or failed): nothing pending
    const int e = WSAGetLastError();
    EnterCriticalSection(&g_lock);
    PendClear(ov);
    LeaveCriticalSection(&g_lock);
    WSASetLastError(e);
  }
  return r;
}
BOOL WINAPI HkWSAGetOverlappedResult(SOCKET s, LPWSAOVERLAPPED ov, LPDWORD got, BOOL wait, LPDWORD flags) {
  const BOOL r = r_WSAGetOverlappedResult(s, ov, got, wait, flags);
  if (r && ov) {
    sockaddr* from = nullptr;
    EnterCriticalSection(&g_lock);
    for (auto& p : g_pend)
      if (p.ov == ov) from = p.from;
    PendClear(ov);
    LeaveCriticalSection(&g_lock);
    if (g_npx) MapFrom(from);
  }
  return r;
}

// The game is told its addresses once (station URLs, own name). If the relay did not answer by then, it got its real
// addresses: the tunnel stays off for this session (a late WELCOME is ignored), and later questions do not wait.
volatile long g_gave_up = 0;
bool WaitWelcome(DWORD ms) {
  if (g_welcomed) return true;
  if (!g_started || !g_welcome_event || g_gave_up) return false;
  WaitForSingleObject(g_welcome_event, ms);
  if (!g_welcomed && !InterlockedExchange(&g_gave_up, 1))
    ss_log("[relay] no answer from the server's relay port in %lu ms: the game keeps its real addresses, no tunnel this session",
           static_cast<unsigned long>(ms));
  return g_welcomed;
}

// The adapter list the game builds its station URLs from: only the virtual address while the tunnel is up.
int WINAPI HkWSAIoctl(SOCKET s, DWORD code, LPVOID in, DWORD inlen, LPVOID out, DWORD outlen, LPDWORD ret,
                      LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr) {
  const int r = r_WSAIoctl(s, code, in, inlen, out, outlen, ret, ov, cr);
  if (code != SIO_GET_INTERFACE_LIST || r != 0 || !out || outlen < sizeof(INTERFACE_INFO) || !ret) return r;
  // The game asks for its adapters just before it looks up the server when it goes ONLINE, and builds the station URL
  // of its sign-in from this answer: the tunnel must start here (seen 2026-11-14: started only at the lookup, the game
  // registered its real address). Known limit (review G2): LAN mode asks too and then also gets the virtual address
  // while online_relay is auto or always; telling the two apart needs the game's online state.
  if (!g_started) ss_tunnel_go(ss_online_server_addr());
  const bool up = WaitWelcome(1500);
  static volatile long logged = 0;
  if (InterlockedIncrement(&logged) <= 5)
    ss_log("[relay] the game asks for its network adapters (%lu found): %s", static_cast<unsigned long>(*ret / sizeof(INTERFACE_INFO)),
           up ? "it is told only the virtual address" : "the tunnel is not up, the real list stays");
  if (!up) return r;
  INTERFACE_INFO ii{};
  ii.iiFlags = IFF_UP | IFF_BROADCAST;
  ii.iiAddress.AddressIn = Sin(g_vip, 0);
  ii.iiBroadcastAddress.AddressIn = Sin(g_vip | 0xFFFF, 0);
  ii.iiNetmask.AddressIn = Sin(0xFFFF0000, 0);
  memcpy(out, &ii, sizeof ii);
  *ret = sizeof ii;
  return r;
}

// ------------------------------------------------------------------------------------------------ tunnel thread
sockaddr_in RelayAddr() {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = g_server;
  a.sin_port = htons(kRelayPort);
  return a;
}
void Out(const uint8_t* p, size_t n, const sockaddr_in& to) {
  r_sendto(g_ts, reinterpret_cast<const char*>(p), int(n), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
}

uint8_t g_nonce[8];
uint32_t g_lan_ip = 0;
uint16_t g_lan_port = 0;
void SendHello() {
  uint8_t h[tun::kHelloLen];
  // always: the server gives no co-player this game's addresses (and it sends no LAN address at all)
  const bool only = g_policy == tun::Policy::kAlways;
  tun::make_hello(h, g_nonce, g_vip, only ? 0 : g_lan_ip, only ? 0 : g_lan_port, g_welcomed ? g_key : nullptr,
                  only ? tun::kHelloRelayOnly : 0);
  Out(h, sizeof h, RelayAddr());
}

const char* ViaName(tun::Via v) { return v == tun::Via::kDirect ? "direct" : v == tun::Via::kRelay ? "through the relay" : "-"; }

// A datagram of the game (read from proxy p) goes to its peer. Under g_lock.
void Carry(Proxy& px, uint16_t myport, const uint8_t* d, size_t n, uint32_t now) {
  tun::Peer* p = PeerFor(px.vip, true);
  if (!p) return;
  if (!p->first_tx) p->first_tx = now ? now : 1;
  tun::Endpoint to;
  const tun::Via v = tun::route(*p, g_policy, now, g_my_pub, &to);
  if (v != p->via) {
    char b[20];
    ss_log("[relay] peer %s: %s", VipText(p->vip, b), ViaName(v));
    p->via = v;
  }
  uint8_t o[tun::kHdr + tun::kMaxPayload];
  if (v == tun::Via::kDirect) {
    const size_t m = tun::make_data(o, sizeof o, g_vip, myport, px.port, d, n);
    if (m) Out(o, m, Sin(to.ip, to.port)), InterlockedIncrement(&g_n.direct_out);
  } else {
    const size_t m = tun::make_send(o, sizeof o, g_key, px.vip, myport, px.port, d, n);
    if (m) Out(o, m, RelayAddr()), InterlockedIncrement(&g_n.relay_out);
  }
}
// A datagram for the game's port dport from peer vip:sport: to that socket of the game, else to the newest. Under g_lock.
void Inject(uint32_t vip, uint16_t sport, uint16_t dport, const uint8_t* d, size_t n) {
  if (g_game_sock == INVALID_SOCKET) return;  // the game has not talked to the server yet: nothing to deliver to
  Proxy* px = ProxyFor(vip, sport, true);
  if (!px) return;
  sockaddr_in to = g_game_addr;
  if (IsGamePort(htons(dport))) to.sin_port = htons(dport);
  r_sendto(px->s, reinterpret_cast<const char*>(d), int(n), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
}

void FromNet(const uint8_t* d, size_t n, const sockaddr_in& src, uint32_t now) {
  const int k = tun::kind_of(d, n);
  const bool from_relay = src.sin_addr.s_addr == g_server && src.sin_port == htons(kRelayPort);
  if (from_relay && k == tun::WELCOME) {
    tun::Welcome w;
    if (!tun::parse_welcome(d, n, g_nonce, &w) || w.vip != g_vip) return;
    if (w.status != 0) {
      if (!g_welcomed) ss_log("[relay] the server refused the virtual address (taken): no tunnel this session");
      return;
    }
    const bool first = !g_welcomed;
    if (first && g_gave_up) {
      static bool said = false;
      if (!said) said = true, ss_log("[relay] the relay's answer came after the game took its real addresses: ignored (no tunnel this session)");
      return;
    }
    memcpy(g_key, w.key, 16);
    g_my_pub = w.pub_ip;
    g_welcomed = true;
    SetEvent(g_welcome_event);
    char b[20];
    if (first) ss_log("[relay] the server's relay answered: virtual address %s, policy %s", VipText(g_vip, b), tun::policy_name(g_policy));
    return;
  }
  if (!g_welcomed) return;
  if (from_relay && k == tun::DELIVER) {
    tun::Deliver dv;
    if (!tun::parse_deliver(d, n, g_key, &dv)) return (void)InterlockedIncrement(&g_n.refused);
    if (tun::Peer* p = PeerFor(dv.src_vip, true)) p->relay_heard = true;
    InterlockedIncrement(&g_n.relay_in);
    Inject(dv.src_vip, dv.sport, dv.dport, dv.payload, dv.n);
    return;
  }
  if (from_relay && k == tun::PEERINFO) {
    tun::PeerInfo pi;
    tun::Peer* p;
    if (!tun::parse_peerinfo(d, n, g_key, &pi) || !(p = PeerFor(pi.vip, false))) return;
    p->have_info = true;
    p->pub = {pi.pub_ip, pi.pub_port};
    p->lan = {pi.lan_ip, pi.lan_port};
    memcpy(p->pair_key, pi.pair_key, 16);
    return;
  }
  if (from_relay || g_policy != tun::Policy::kAuto || n < 12) return InterlockedIncrement(&g_n.refused), void();
  const uint32_t from = tun::get32(d + 4);
  tun::Peer* p = PeerFor(from, false);
  const tun::Endpoint se{ntohl(src.sin_addr.s_addr), ntohs(src.sin_port)};
  if (k == tun::PROBE && p && p->have_info) {
    uint8_t fl;
    if (!tun::check_probe(d, n, p->pair_key, from, g_vip, &fl)) return (void)InterlockedIncrement(&g_n.refused);
    tun::probe_seen(*p, se, fl, now);
    return;
  }
  if (k == tun::DATA && p && tun::direct_fresh(*p, now) && p->direct == se && n > tun::kDataHdr) {
    InterlockedIncrement(&g_n.direct_in);
    Inject(from, tun::get16(d + 8), tun::get16(d + 10), d + tun::kDataHdr, n - tun::kDataHdr);
    return;
  }
  InterlockedIncrement(&g_n.refused);
}

void Timers(uint32_t now) {
  static uint32_t hello_at = 0;
  const uint32_t every = g_welcomed ? 20000 : 1000;
  if (!hello_at || now - hello_at >= every) hello_at = now, SendHello();
  if (!g_welcomed || g_policy != tun::Policy::kAuto) return;
  for (int i = 0; i < g_npeers; i++) {
    tun::Peer& p = g_peers[i];
    if (tun::ask_due(p, g_policy, now)) {
      uint8_t q[tun::kPeerLen];
      tun::make_peer(q, g_key, p.vip, now);
      Out(q, sizeof q, RelayAddr());
      p.asked = now;
    }
    if (tun::probe_due(p, g_policy, now)) {
      uint8_t pr[tun::kProbeLen];
      tun::make_probe(pr, p.pair_key, g_vip, p.vip, tun::probe_flags(p, now));
      if (p.pub.ok()) Out(pr, sizeof pr, Sin(p.pub.ip, p.pub.port));
      if (p.lan.ok() && !(p.lan == p.pub)) Out(pr, sizeof pr, Sin(p.lan.ip, p.lan.port));
      if (p.direct.ok() && !(p.direct == p.pub) && !(p.direct == p.lan)) Out(pr, sizeof pr, Sin(p.direct.ip, p.direct.port));
      p.probed = now;
      InterlockedIncrement(&g_n.probes);
    }
  }
}

void LogCounts(uint32_t now) {
  static uint32_t at = 0;
  static long last[7] = {0};
  if (now - at < 30000) return;
  at = now;
  const long v[7] = {g_n.relay_out, g_n.relay_in, g_n.direct_out, g_n.direct_in, g_n.outside, g_n.probes, g_n.refused};
  if (!memcmp(v, last, sizeof v)) return;
  memcpy(last, v, sizeof v);
  ss_log("[relay] datagrams: relay out %ld in %ld, direct out %ld in %ld, probes %ld, refused %ld; game socket to "
         "other addresses outside the tunnel %ld",
         v[0], v[1], v[2], v[3], v[5], v[6], v[4]);
}

DWORD WINAPI TunnelThread(LPVOID) {
  WSADATA wd;
  WSAStartup(MAKEWORD(2, 2), &wd);
  g_ts = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockaddr_in b = Sin(0, 0);
  int bl = sizeof b;
  u_long nb = 1;
  if (g_ts == INVALID_SOCKET || bind(g_ts, reinterpret_cast<sockaddr*>(&b), sizeof b) ||
      getsockname(g_ts, reinterpret_cast<sockaddr*>(&b), &bl) || ioctlsocket(g_ts, FIONBIO, &nb)) {
    ss_log("[relay] no tunnel socket (error %d): peers are reached directly only", WSAGetLastError());
    return 0;
  }
  g_lan_port = ntohs(b.sin_port);
  {  // the LAN address the route to the server leaves from (what a player in the same house can reach)
    SOCKET t = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in ra = RelayAddr(), me{};
    int ml = sizeof me;
    if (t != INVALID_SOCKET && !connect(t, reinterpret_cast<sockaddr*>(&ra), sizeof ra) &&
        !getsockname(t, reinterpret_cast<sockaddr*>(&me), &ml))
      g_lan_ip = ntohl(me.sin_addr.s_addr);
    if (t != INVALID_SOCKET) closesocket(t);
  }
  ss_log("[relay] tunnel started, policy %s: hello to the server's relay port %u", tun::policy_name(g_policy), kRelayPort);
  static uint8_t buf[2048];
  for (;;) {
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(g_ts, &rs);
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_npx; i++)
      if (g_px[i].s != INVALID_SOCKET) FD_SET(g_px[i].s, &rs);
    LeaveCriticalSection(&g_lock);
    timeval tv{0, 50000};
    const int r = select(0, &rs, nullptr, nullptr, &tv);
    const uint32_t now = GetTickCount();
    EnterCriticalSection(&g_lock);
    if (r > 0) {
      for (int round = 0; round < 64 && FD_ISSET(g_ts, &rs); round++) {
        sockaddr_in src{};
        int sl = sizeof src;
        const int n = r_recvfrom(g_ts, reinterpret_cast<char*>(buf), sizeof buf, 0, reinterpret_cast<sockaddr*>(&src), &sl);
        if (n <= 0) break;
        FromNet(buf, size_t(n), src, now);
      }
      for (int i = 0; i < g_npx; i++) {
        if (!FD_ISSET(g_px[i].s, &rs)) continue;
        for (int round = 0; round < 64; round++) {
          sockaddr_in src{};
          int sl = sizeof src;
          const int n = r_recvfrom(g_px[i].s, reinterpret_cast<char*>(buf), sizeof buf, 0, reinterpret_cast<sockaddr*>(&src), &sl);
          if (n <= 0) break;
          if (!IsGamePort(src.sin_port)) {  // only the game's own sockets send here
            static long said = 0;
            if (++said <= 3) {
              char b[20];
              ss_log("[relay] proxy for %s:%u: a datagram from local port %u, not a game socket that talked to the server: dropped",
                     VipText(g_px[i].vip, b), g_px[i].port, ntohs(src.sin_port));
            }
            continue;
          }
          if (g_welcomed) Carry(g_px[i], ntohs(src.sin_port), buf, size_t(n), now);
        }
      }
    }
    Timers(now);
    LeaveCriticalSection(&g_lock);
    LogCounts(now);
  }
}

constexpr uintptr_t kImageSectionsEnd = 0x118E000;  // the end of .rsrc (overlay.cpp): the image's own sections lie below

struct Slot {
  uintptr_t iat;
  const char* name;
  uintptr_t hook;
  void* original;  // where the original goes (a function pointer variable)
  bool done;
};
Slot g_slots[] = {
    {kSlotSendTo, "sendto", uintptr_t(&HkSendTo), &r_sendto, false},
    {kSlotRecvFrom, "recvfrom", uintptr_t(&HkRecvFrom), &r_recvfrom, false},
    {kSlotWSASendTo, "WSASendTo", uintptr_t(&HkWSASendTo), &r_WSASendTo, false},
    {kSlotWSARecvFrom, "WSARecvFrom", uintptr_t(&HkWSARecvFrom), &r_WSARecvFrom, false},
    {kSlotWSAGetOverlapped, "WSAGetOverlappedResult", uintptr_t(&HkWSAGetOverlappedResult), &r_WSAGetOverlappedResult, false},
    {kSlotWSAIoctl, "WSAIoctl", uintptr_t(&HkWSAIoctl), &r_WSAIoctl, false},
};

// The protection's start-up fills the import slots after DllMain (overlay.cpp has the same wait). A slot is taken
// when it holds ws2_32's function, or one of the exe's own forwarders above its sections; never anything else.
// -> 1 hooked, 0 not filled yet, -1 holds something unknown.
int TryHook(Slot& s) {
  const uintptr_t cur = *reinterpret_cast<volatile uintptr_t*>(s.iat);
  if (cur == s.hook) return 1;
  if (cur < 0x10000 || (cur >= 0x400000 && cur < kImageSectionsEnd)) return 0;
  if ((cur & 0xFFFF0000u) == 0x80000000u) return 0;  // still the ordinal entry (sendto and recvfrom are imported by ordinal)
  HMODULE ws = GetModuleHandleA("ws2_32.dll"), owner = nullptr;
  const uintptr_t want = ws ? reinterpret_cast<uintptr_t>(GetProcAddress(ws, s.name)) : 0;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCSTR>(cur), &owner);
  const bool forwarder = cur >= kImageSectionsEnd && owner == GetModuleHandleA(nullptr);
  if (cur != want && owner != ws && !forwarder) {
    ss_log("[relay] the %s import slot %08X holds %p (not ws2_32's): left alone, no tunnel", s.name, unsigned(s.iat),
           reinterpret_cast<void*>(cur));
    return -1;
  }
  *reinterpret_cast<uintptr_t*>(s.original) = cur;
  if (!ss_write_word(reinterpret_cast<void*>(s.iat), s.hook)) return -1;
  s.done = true;
  return 1;
}

// The game's own import table: 0xB9B000-0xB9B56C in .rdata, one page, every import of the game (the six ws2_32 slots
// among them). The protection's start-up fills it about 1 s after the DLL loads, and ws2_32's group is the third of 17:
// when our six slots hold their functions it is still writing the rest of that page. A slot write of ours lifts the
// page's protection and puts it back (ss_write_word); put back between the protection's own lift and write, its write
// faults in its code. Seen 2026-11-17 (rb1, Windows test PC): an access violation in the protection's Stext section
// (SplitSecond.exe+0xE38CBF) 0-0.8 s after the six writes; the +0x131BB02 boot crash (also Stext) is a write of the
// protection into D3DCompiler_42's import table that found the page read-only. So the six slots are written only once
// the whole table is filled and has not changed for 500 ms; the game's first socket call comes far later (the ONLINE
// menu).
constexpr uintptr_t kIatBegin = 0xB9B000, kIatEnd = 0xB9B56C;
constexpr int kIatQuietMs = 500, kIatWaitMs = 30000;

// -> true when no entry still holds its import's name (an address in the image's sections) or ordinal; *hash then
// sums the table so a change can be seen.
bool IatFilled(uint32_t* hash) {
  uint32_t h = 2166136261u;
  for (uintptr_t a = kIatBegin; a < kIatEnd; a += 4) {
    const uintptr_t v = *reinterpret_cast<volatile uintptr_t*>(a);
    if (v && (v < 0x10000 || (v >= 0x400000 && v < kImageSectionsEnd) || (v & 0xFFFF0000u) == 0x80000000u))
      return false;
    h = (h ^ uint32_t(v)) * 16777619u;
  }
  *hash = h;
  return true;
}

DWORD WINAPI HookThread(LPVOID) {
  const DWORD t0 = GetTickCount();
  ss_import_table_wait("relay");
  for (int t = 0; t < 120000; t += 5) {
    int ready = 0, bad = 0;
    for (auto& s : g_slots) {
      const int r = s.done ? 1 : TryHook(s);
      ready += r == 1, bad += r < 0;
    }
    if (bad) return 0;
    if (ready == int(sizeof g_slots / sizeof g_slots[0])) {
      g_hooked = true;
      ss_log("[relay] the game's socket calls are hooked (6 import slots of ws2_32, %lu ms after the start, the import "
             "table filled and quiet)", static_cast<unsigned long>(GetTickCount() - t0));
      return 0;
    }
    Sleep(5);
  }
  ss_log("[relay] the ws2_32 import slots were never filled: no tunnel");
  return 0;
}

}  // namespace

// Blocks until the game's import table is filled and has not changed for 500 ms (30 s at most), so a slot write of
// ours never lifts and restores the page's protection while the protection's start-up is still writing it. Every
// import slot written in the first seconds goes through this (the relay's six ws2_32 slots, slipstream's d3dx9 slot,
// audio.cpp's Event::start slot); the slot writes from the first frame on come long after. -> false after the 30 s cap (logged with who).
bool ss_import_table_wait(const char* who) {
  const DWORD t0 = GetTickCount();
  uint32_t last = 0;
  DWORD quiet_from = t0;
  for (bool first = true;; first = false) {
    uint32_t h = 0;
    const bool full = IatFilled(&h);
    const DWORD now = GetTickCount();
    if (!full || h != last || first) last = h, quiet_from = now;
    if (full && now - quiet_from >= DWORD(kIatQuietMs)) return true;
    if (now - t0 >= DWORD(kIatWaitMs)) {
      ss_log("[%s] the game's import table %s after %d s: its slots are taken as they are", who,
             full ? "kept changing" : "was not filled", kIatWaitMs / 1000);
      return false;
    }
    Sleep(5);
  }
}

// online.cpp, at start (online on): reads online_relay, picks the virtual address and puts the hooks in.
void ss_tunnel_init() {
  std::string v;
  if (const char* e = getenv("SS_ONLINE_RELAY")) v = e; else v = ss_conf("online_relay");
  while (!v.empty() && (v.back() == ' ' || v.back() == '\r')) v.pop_back();
  g_policy = tun::parse_policy(v.c_str());
  if (g_policy == tun::Policy::kOff) {
    ss_log("[relay] online_relay = off: no tunnel, the game reaches other players directly as before");
    return;
  }
  InitializeCriticalSection(&g_lock);
  g_welcome_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  HCRYPTPROV prov;
  uint8_t rnd[10] = {0};
  if (CryptAcquireContextA(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
    CryptGenRandom(prov, sizeof rnd, rnd);
    CryptReleaseContext(prov, 0);
  }
  memcpy(g_nonce, rnd + 2, 8);
  uint8_t last = rnd[1];
  if (last == 0 || last == 255) last = 1 + (rnd[0] % 254);
  g_vip = tun::kNet | uint32_t(rnd[0]) << 8 | last;
  CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
  char b[20];
  ss_log("[relay] online_relay = %s: virtual address %s; the tunnel starts when the game first looks up the server",
         tun::policy_name(g_policy), VipText(g_vip, b));
}

// online.cpp's gethostbyname hook: the game looks up the backend (it is going online) -> the tunnel starts.
void ss_tunnel_go(uint32_t server_addr_net) {
  if (!g_hooked || !server_addr_net || server_addr_net == INADDR_NONE) return;
  if (InterlockedExchange(&g_started, 1)) return;
  g_server = server_addr_net;
  CreateThread(nullptr, 0, TunnelThread, nullptr, 0, nullptr);
}

// online.cpp's gethostbyname hook for any other name: the machine's own name -> the virtual address while the tunnel
// is up. -> true and *addr (network order) when answered.
bool ss_tunnel_own_address(const char* name, uint32_t* addr_net) {
  if (!g_hooked || !g_started || !name) return false;
  char me[256];
  if (gethostname(me, sizeof me) || _stricmp(me, name)) return false;
  if (!WaitWelcome(1500)) return false;
  *addr_net = htonl(g_vip);
  return true;
}
