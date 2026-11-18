// tunnel_core.h: the peer tunnel's wire format and route policy (docs/plan-online.md 4.2, SSPC-41). No Windows calls:
// test/tunneltest.cpp checks it on any machine (`make tunneltest`); tunnel.cpp does the sockets and the hooks.
//
// The model is Takedawn's relay (xboxrecomp src/kernel/xnet_route.c, burnout3 tools/lobby/aries_server.c):
//   * every player has a virtual address (10.254.x.y); the game sees only virtual addresses for tunnelled players;
//   * per pair: direct first (signed probes to the peer's public and LAN endpoints), the relay when no direct path
//     answered within 2 s; back to direct only when it works both ways; a direct path quiet for 30 s goes back to the
//     relay. `online_relay = always` never tries direct (test switch and privacy), `off` = no tunnel at all.
// Differences, forced by the game: the game's own server protocol is encrypted PRUDP the DLL does not speak, so the
// tunnel has its own small channel to the server's relay port (HELLO/WELCOME) instead of fields in the lobby records;
// each registration has its own key and the server re-signs for the receiver (Takedawn: one key per game, forwarded
// unchanged), so one member cannot sign as another.
//
// Wire format "SR" version 2, one UDP port on the server (31122), ports and lengths big-endian:
//   HELLO    "SR" 2 1 nonce[8] vip[4] lan_ip[4] lan_port[2] proof[8] flags[1] pad -> 48 bytes  tunnel -> server, every 20 s
//            flags bit 0 = relay only (always): the server gives nobody this player's endpoints
//            proof = HMAC(old key, bytes 0..21) when re-registering from a new address (NAT rebinding), else zeros
//   WELCOME  "SR" 2 2 nonce[8] vip[4] status[1] key[16] pub_ip[4] pub_port[2] -> 39   server -> the HELLO's source
//   SEND     "SR" 2 3 vip[4] sport[2] dport[2] mac[8] payload    tunnel -> server, vip = destination
//   DELIVER  "SR" 2 4 vip[4] sport[2] dport[2] mac[8] payload    server -> tunnel, vip = source, re-signed
//            mac = HMAC(key, bytes 0..11 + payload)[:8], with the sender's key (SEND) or the receiver's (DELIVER)
//   PEER     "SR" 2 5 vip[4] nonce[4] mac[8] pad -> 52           tunnel -> server: the endpoints of a co-player
//   PEERINFO "SR" 2 6 vip[4] nonce[4] pub_ip[4] pub_port[2] lan_ip[4] lan_port[2] pair_key[16] mac[8] -> 48
//   PROBE    "SR" 2 7 from[4] to[4] flags[1] pad[3] mac[8] -> 24  tunnel <-> tunnel, mac under the pair key;
//            flags bit 0 = "your probes reach me" (Takedawn's hello bit 1)
//   DATA     "SR" 2 8 from[4] sport[2] dport[2] payload          tunnel <-> tunnel, accepted only from the learned
//            direct endpoint of `from`
// mac = HMAC-SHA256(key, ...)[:8]. Nothing answers a datagram that fails a check; no answer is larger than its
// request (HELLO 48 -> WELCOME 39, PEER 52 -> PEERINFO 48, SEND -> DELIVER the same size).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace tun {

// ---------------------------------------------------------------------------------------------- SHA-256 / HMAC
struct Sha256 {
  uint32_t h[8];
  uint8_t buf[64];
  uint64_t len = 0;
  size_t n = 0;
  Sha256() {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(h, iv, sizeof h);
  }
  static uint32_t R(uint32_t x, int k) { return (x >> k) | (x << (32 - k)); }
  void block(const uint8_t* p) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
        0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
        0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = uint32_t(p[4 * i]) << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
      const uint32_t s0 = R(w[i - 15], 7) ^ R(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = R(w[i - 2], 17) ^ R(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++) {
      const uint32_t t1 = k + (R(e, 6) ^ R(e, 11) ^ R(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
      const uint32_t t2 = (R(a, 2) ^ R(a, 13) ^ R(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      k = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += k;
  }
  void add(const void* data, size_t m) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    len += m;
    while (m) {
      const size_t t = (64 - n) < m ? 64 - n : m;
      memcpy(buf + n, p, t);
      n += t, p += t, m -= t;
      if (n == 64) block(buf), n = 0;
    }
  }
  void done(uint8_t out[32]) {
    const uint64_t bits = len * 8;
    const uint8_t one = 0x80, zero = 0;
    add(&one, 1);
    while (n != 56) add(&zero, 1);
    uint8_t l[8];
    for (int i = 0; i < 8; i++) l[i] = uint8_t(bits >> (56 - 8 * i));
    add(l, 8);
    for (int i = 0; i < 8; i++)
      out[4 * i] = uint8_t(h[i] >> 24), out[4 * i + 1] = uint8_t(h[i] >> 16), out[4 * i + 2] = uint8_t(h[i] >> 8),
                out[4 * i + 3] = uint8_t(h[i]);
  }
};

// HMAC-SHA256 over two parts (header, payload), truncated to `outlen` bytes. Keys here are 16 bytes.
inline void hmac(const uint8_t* key, size_t klen, const void* a, size_t alen, const void* b, size_t blen, uint8_t* out,
                 size_t outlen) {
  uint8_t k[64] = {0}, ip[64], op[64], inner[32], full[32];
  memcpy(k, key, klen < 64 ? klen : 64);
  for (int i = 0; i < 64; i++) ip[i] = k[i] ^ 0x36, op[i] = k[i] ^ 0x5c;
  Sha256 s;
  s.add(ip, 64);
  s.add(a, alen);
  if (blen) s.add(b, blen);
  s.done(inner);
  Sha256 o;
  o.add(op, 64);
  o.add(inner, 32);
  o.done(full);
  memcpy(out, full, outlen);
}

inline bool same(const uint8_t* a, const uint8_t* b, size_t n) {  // constant time
  uint8_t d = 0;
  for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
  return d == 0;
}

// ---------------------------------------------------------------------------------------------- wire format
enum Kind : uint8_t { HELLO = 1, WELCOME, SEND, DELIVER, PEER, PEERINFO, PROBE, DATA };
constexpr uint8_t kVersion = 2;
constexpr size_t kHelloLen = 48, kWelcomeLen = 39, kPeerLen = 52, kPeerInfoLen = 48, kProbeLen = 24;
constexpr size_t kHdr = 20;       // SEND / DELIVER header (with the mac)
constexpr size_t kDataHdr = 12;   // DATA header
constexpr size_t kMaxPayload = 1400;
constexpr uint32_t kNet = 0x0AFE0000;  // 10.254.0.0/16, host order

inline bool is_vip(uint32_t ip_host) { return (ip_host & 0xFFFF0000u) == kNet && (ip_host & 0xFF) != 0 && (ip_host & 0xFF) != 255; }
inline void put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }
inline void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8), p[1] = uint8_t(v); }
inline uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
inline uint16_t get16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline void head(uint8_t* p, Kind k) { p[0] = 'S', p[1] = 'R', p[2] = kVersion, p[3] = k; }
inline int kind_of(const uint8_t* p, size_t n) {
  return (n >= 4 && p[0] == 'S' && p[1] == 'R' && p[2] == kVersion) ? p[3] : 0;
}

// Addresses are host order (a.b.c.d = a<<24 ...) everywhere in this file.
// flags (byte 30, in the pad): kHelloRelayOnly = never hand this player's endpoints to a co-player (always).
constexpr uint8_t kHelloRelayOnly = 1;
inline size_t make_hello(uint8_t* o, const uint8_t nonce[8], uint32_t vip, uint32_t lan_ip, uint16_t lan_port,
                         const uint8_t* old_key, uint8_t flags = 0) {
  memset(o, 0, kHelloLen);
  head(o, HELLO);
  memcpy(o + 4, nonce, 8);
  put32(o + 12, vip);
  put32(o + 16, lan_ip);
  put16(o + 20, lan_port);
  if (old_key) hmac(old_key, 16, o, 22, nullptr, 0, o + 22, 8);
  o[30] = flags;
  return kHelloLen;
}

struct Welcome {
  uint32_t vip = 0, pub_ip = 0;
  uint16_t pub_port = 0;
  uint8_t status = 0xFF, key[16] = {0};
};
inline bool parse_welcome(const uint8_t* p, size_t n, const uint8_t nonce[8], Welcome* w) {
  if (n != kWelcomeLen || kind_of(p, n) != WELCOME || !same(p + 4, nonce, 8)) return false;
  w->vip = get32(p + 12);
  w->status = p[16];
  memcpy(w->key, p + 17, 16);
  w->pub_ip = get32(p + 33);
  w->pub_port = get16(p + 37);
  return true;
}

// SEND (tunnel -> server) and DELIVER (server -> tunnel) share one layout.
inline size_t make_send(uint8_t* o, size_t cap, const uint8_t key[16], uint32_t dst_vip, uint16_t sport, uint16_t dport,
                        const uint8_t* payload, size_t n) {
  if (n == 0 || n > kMaxPayload || cap < kHdr + n) return 0;
  head(o, SEND);
  put32(o + 4, dst_vip);
  put16(o + 8, sport);
  put16(o + 10, dport);
  hmac(key, 16, o, 12, payload, n, o + 12, 8);
  memcpy(o + kHdr, payload, n);
  return kHdr + n;
}
struct Deliver {
  uint32_t src_vip = 0;
  uint16_t sport = 0, dport = 0;
  const uint8_t* payload = nullptr;
  size_t n = 0;
};
inline bool parse_deliver(const uint8_t* p, size_t n, const uint8_t key[16], Deliver* d) {
  if (n <= kHdr || n > kHdr + kMaxPayload || kind_of(p, n) != DELIVER) return false;
  uint8_t m[8];
  hmac(key, 16, p, 12, p + kHdr, n - kHdr, m, 8);
  if (!same(m, p + 12, 8)) return false;
  d->src_vip = get32(p + 4);
  d->sport = get16(p + 8);
  d->dport = get16(p + 10);
  d->payload = p + kHdr;
  d->n = n - kHdr;
  return is_vip(d->src_vip);
}

inline size_t make_peer(uint8_t* o, const uint8_t key[16], uint32_t vip, uint32_t nonce) {
  memset(o, 0, kPeerLen);
  head(o, PEER);
  put32(o + 4, vip);
  put32(o + 8, nonce);
  hmac(key, 16, o, 12, nullptr, 0, o + 12, 8);
  return kPeerLen;
}
struct PeerInfo {
  uint32_t vip = 0, nonce = 0, pub_ip = 0, lan_ip = 0;
  uint16_t pub_port = 0, lan_port = 0;
  uint8_t pair_key[16] = {0};
};
inline bool parse_peerinfo(const uint8_t* p, size_t n, const uint8_t key[16], PeerInfo* pi) {
  if (n != kPeerInfoLen || kind_of(p, n) != PEERINFO) return false;
  uint8_t m[8];
  hmac(key, 16, p, 40, nullptr, 0, m, 8);
  if (!same(m, p + 40, 8)) return false;
  pi->vip = get32(p + 4);
  pi->nonce = get32(p + 8);
  pi->pub_ip = get32(p + 12);
  pi->pub_port = get16(p + 16);
  pi->lan_ip = get32(p + 18);
  pi->lan_port = get16(p + 22);
  memcpy(pi->pair_key, p + 24, 16);
  return true;
}

inline size_t make_probe(uint8_t* o, const uint8_t pair_key[16], uint32_t from, uint32_t to, uint8_t flags) {
  memset(o, 0, kProbeLen);
  head(o, PROBE);
  put32(o + 4, from);
  put32(o + 8, to);
  o[12] = flags;
  hmac(pair_key, 16, o, 16, nullptr, 0, o + 16, 8);
  return kProbeLen;
}
// -> true when it is a probe from `from` to `me` signed with the pair key; *flags gets its flags.
inline bool check_probe(const uint8_t* p, size_t n, const uint8_t pair_key[16], uint32_t from, uint32_t me, uint8_t* flags) {
  if (n != kProbeLen || kind_of(p, n) != PROBE || get32(p + 4) != from || get32(p + 8) != me) return false;
  uint8_t m[8];
  hmac(pair_key, 16, p, 16, nullptr, 0, m, 8);
  if (!same(m, p + 16, 8)) return false;
  *flags = p[12];
  return true;
}

inline size_t make_data(uint8_t* o, size_t cap, uint32_t from, uint16_t sport, uint16_t dport, const uint8_t* payload, size_t n) {
  if (n == 0 || n > kMaxPayload || cap < kDataHdr + n) return 0;
  head(o, DATA);
  put32(o + 4, from);
  put16(o + 8, sport);
  put16(o + 10, dport);
  memcpy(o + kDataHdr, payload, n);
  return kDataHdr + n;
}

// ---------------------------------------------------------------------------------------------- route policy
enum class Policy { kOff, kAuto, kAlways };
inline Policy parse_policy(const char* v) {
  // Default auto (1.0.5, SSPC-41): no line, an empty value or a misspelt one is auto; only an explicit off is off.
  if (!v || !*v) return Policy::kAuto;
  if (!strcmp(v, "off") || !strcmp(v, "false") || !strcmp(v, "0") || !strcmp(v, "direct")) return Policy::kOff;
  if (!strcmp(v, "always") || !strcmp(v, "only") || !strcmp(v, "force")) return Policy::kAlways;
  return Policy::kAuto;
}
inline const char* policy_name(Policy p) { return p == Policy::kOff ? "off" : p == Policy::kAlways ? "always" : "auto"; }

constexpr uint32_t kRelayAfterMs = 2000;    // Takedawn XR_RELAY_AFTER_MS
constexpr uint32_t kProbeFastMs = 500;      // HELLO_FAST_MS
constexpr uint32_t kProbeKeepMs = 15000;    // HELLO_KEEP_MS
constexpr uint32_t kProbeFastForMs = 20000; // XR_RELAY_DIRECT_FAST_MS
constexpr uint32_t kDirectStaleMs = 30000;  // LEARNED_STALE_MS
constexpr uint32_t kPeerAskMs = 1000;       // PEER query repeat until PEERINFO

struct Endpoint {
  uint32_t ip = 0;  // host order
  uint16_t port = 0;
  bool operator==(const Endpoint& o) const { return ip == o.ip && port == o.port; }
  bool ok() const { return ip && port; }
};

enum class Via { kNone, kRelay, kDirect };

// One co-player, keyed by its virtual address. All times are a millisecond clock (GetTickCount on Windows).
struct Peer {
  uint32_t vip = 0;
  uint32_t first_tx = 0;   // first datagram of the game to this peer (Takedawn's member_at); 0 = none yet
  bool have_info = false;  // PEERINFO came: endpoints and the pair key
  Endpoint pub, lan;
  uint8_t pair_key[16] = {0};
  uint32_t asked = 0;        // last PEER query
  uint32_t probed = 0;       // last probe round
  Endpoint direct;           // where its valid probes come from (learned)
  uint32_t direct_heard = 0; // last valid probe from it
  bool hears_us = false;     // its last probe had bit 0
  bool relay_heard = false;  // it reached us through the relay
  Via via = Via::kNone;      // last route used (for the change log)
};

// The endpoint a direct guess goes to before anything is learned: LAN when both share a public address, else public.
inline Endpoint guess(const Peer& p, uint32_t my_pub_ip) {
  if (p.lan.ok() && p.pub.ip == my_pub_ip) return p.lan;
  return p.pub.ok() ? p.pub : p.lan;
}
inline bool direct_fresh(const Peer& p, uint32_t now) { return p.direct.ok() && now - p.direct_heard < kDirectStaleMs; }
inline bool direct_both_ways(const Peer& p, uint32_t now) { return direct_fresh(p, now) && p.hears_us; }

// Where the game's next datagram to this peer goes. *to = the direct endpoint for kDirect.
inline Via route(const Peer& p, Policy pol, uint32_t now, uint32_t my_pub_ip, Endpoint* to) {
  if (pol == Policy::kAlways) return Via::kRelay;
  if (direct_both_ways(p, now)) return *to = p.direct, Via::kDirect;
  if (p.relay_heard) return Via::kRelay;  // answer in kind
  if (p.have_info && p.first_tx && now - p.first_tx < kRelayAfterMs) {
    *to = direct_fresh(p, now) ? p.direct : guess(p, my_pub_ip);
    if (to->ok()) return Via::kDirect;
  }
  return Via::kRelay;
}

// Probe flags we put in our probes to this peer: bit 0 when its probes reach us (both-ways rule, Takedawn BO-381).
inline uint8_t probe_flags(const Peer& p, uint32_t now) { return direct_fresh(p, now) ? 1 : 0; }
// A probe round is due (auto only): fast for 20 s after the first datagram, then every 15 s.
inline bool probe_due(const Peer& p, Policy pol, uint32_t now) {
  if (pol != Policy::kAuto || !p.have_info || !p.first_tx) return false;
  const uint32_t every = (now - p.first_tx < kProbeFastForMs || !direct_both_ways(p, now)) ? kProbeFastMs : kProbeKeepMs;
  return !p.probed || now - p.probed >= every;
}
inline bool ask_due(const Peer& p, Policy pol, uint32_t now) {
  return pol == Policy::kAuto && !p.have_info && p.first_tx && (!p.asked || now - p.asked >= kPeerAskMs);
}
// A valid probe from `src`: learn the endpoint.
inline void probe_seen(Peer& p, Endpoint src, uint8_t flags, uint32_t now) {
  p.direct = src;
  p.direct_heard = now;
  p.hears_us = flags & 1;
}

}  // namespace tun
