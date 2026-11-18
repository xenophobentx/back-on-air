// tunneltest.cpp: the peer tunnel's wire format and route policy (src/tunnel_core.h). `make tunneltest`.
// The SEND vector is the one tools/online/tests/test_relay.py computes with Python's hmac, so both sides agree.
#include <cstdio>
#include <cstring>
#include <string>

#include "../src/tunnel_core.h"

using namespace tun;
static int fails = 0;
#define CHECK(c)                                                    \
  do {                                                              \
    if (!(c)) std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c), fails++; \
  } while (0)

static std::string hex(const uint8_t* p, size_t n) {
  std::string s;
  char b[3];
  for (size_t i = 0; i < n; i++) std::snprintf(b, 3, "%02x", p[i]), s += b;
  return s;
}

int main() {
  {  // SHA-256("abc"), HMAC-SHA256 RFC 4231 case 2
    Sha256 s;
    s.add("abc", 3);
    uint8_t d[32];
    s.done(d);
    CHECK(hex(d, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    uint8_t m[32];
    const char* msg = "what do ya want for nothing?";
    hmac(reinterpret_cast<const uint8_t*>("Jefe"), 4, msg, strlen(msg), nullptr, 0, m, 32);
    CHECK(hex(m, 32) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  }
  uint8_t key[16], other[16];
  for (int i = 0; i < 16; i++) key[i] = uint8_t(i), other[i] = uint8_t(i + 1);
  const uint32_t A = 0x0AFE0102, B = 0x0AFE0203;  // 10.254.1.2, 10.254.2.3
  const uint8_t pl[] = "hello quazal";
  const size_t pn = sizeof pl - 1;
  {  // SEND: layout and the Python vector; DELIVER parse; tamper and size checks
    uint8_t o[1500];
    const size_t n = make_send(o, sizeof o, key, A, 9103, 9104, pl, pn);
    CHECK(n == kHdr + pn);
    CHECK(o[0] == 'S' && o[1] == 'R' && o[2] == 2 && o[3] == SEND);
    CHECK(hex(o + 12, 8) == "017f0191645eb18f");
    o[3] = DELIVER;  // a DELIVER is signed the same way (the server uses the receiver's key)
    hmac(key, 16, o, 12, pl, pn, o + 12, 8);
    Deliver d;
    CHECK(parse_deliver(o, n, key, &d) && d.src_vip == A && d.sport == 9103 && d.dport == 9104 && d.n == pn &&
          !memcmp(d.payload, pl, pn));
    CHECK(!parse_deliver(o, n, other, &d));  // another key
    o[n - 1] ^= 1;
    CHECK(!parse_deliver(o, n, key, &d));  // payload changed
    o[n - 1] ^= 1;
    o[3] = SEND;
    CHECK(!parse_deliver(o, n, key, &d));  // a SEND is never taken as a DELIVER
    uint8_t big[1500] = {0};
    CHECK(make_send(o, sizeof o, key, A, 1, 1, big, kMaxPayload + 1) == 0);
    CHECK(make_send(o, 30, key, A, 1, 1, big, 20) == 0);  // buffer too small
    CHECK(make_send(o, sizeof o, key, A, 1, 1, big, 0) == 0);
  }
  {  // HELLO / WELCOME: sizes (no amplification), nonce check
    uint8_t h[kHelloLen], nonce[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(make_hello(h, nonce, A, 0xC0A80005, 9103, nullptr) == 48);
    CHECK(kWelcomeLen <= kHelloLen && kPeerInfoLen <= kPeerLen);
    uint8_t zero[8] = {0};
    CHECK(!memcmp(h + 22, zero, 8));
    make_hello(h, nonce, A, 0xC0A80005, 9103, key);
    uint8_t m[8];
    hmac(key, 16, h, 22, nullptr, 0, m, 8);
    CHECK(!memcmp(h + 22, m, 8));
    uint8_t w[kWelcomeLen] = {'S', 'R', 2, WELCOME};
    memcpy(w + 4, nonce, 8);
    put32(w + 12, A);
    w[16] = 0;
    memcpy(w + 17, key, 16);
    put32(w + 33, 0x4F00DE89);
    put16(w + 37, 50000);
    Welcome wl;
    CHECK(parse_welcome(w, sizeof w, nonce, &wl) && wl.vip == A && wl.status == 0 && wl.pub_port == 50000 &&
          !memcmp(wl.key, key, 16));
    uint8_t nonce2[8] = {9};
    CHECK(!parse_welcome(w, sizeof w, nonce2, &wl));
  }
  {  // PEER / PEERINFO
    uint8_t q[kPeerLen];
    make_peer(q, key, B, 77);
    uint8_t m[8];
    hmac(key, 16, q, 12, nullptr, 0, m, 8);
    CHECK(!memcmp(q + 12, m, 8) && get32(q + 4) == B && get32(q + 8) == 77);
    uint8_t r[kPeerInfoLen] = {'S', 'R', 2, PEERINFO};
    put32(r + 4, B), put32(r + 8, 77), put32(r + 12, 0x4F00DE89), put16(r + 16, 4000), put32(r + 18, 0xC0A80006),
        put16(r + 22, 9103);
    memcpy(r + 24, other, 16);
    hmac(key, 16, r, 40, nullptr, 0, r + 40, 8);
    PeerInfo pi;
    CHECK(parse_peerinfo(r, sizeof r, key, &pi) && pi.vip == B && pi.lan_port == 9103 && pi.pub_port == 4000 &&
          !memcmp(pi.pair_key, other, 16));
    r[20] ^= 1;
    CHECK(!parse_peerinfo(r, sizeof r, key, &pi));
  }
  {  // PROBE: from/to bound, the pair key
    uint8_t p[kProbeLen], fl = 0;
    make_probe(p, other, A, B, 1);
    CHECK(check_probe(p, sizeof p, other, A, B, &fl) && fl == 1);
    CHECK(!check_probe(p, sizeof p, key, A, B, &fl));
    CHECK(!check_probe(p, sizeof p, other, B, A, &fl));  // our own probe reflected back
    CHECK(!check_probe(p, sizeof p, other, A, 0x0AFE0909, &fl));
  }
  {  // DATA
    uint8_t o[64];
    CHECK(make_data(o, sizeof o, A, 9103, 9103, pl, pn) == kDataHdr + pn && get32(o + 4) == A && o[3] == DATA);
  }
  {  // vip range, policy parsing
    CHECK(is_vip(A) && !is_vip(0x0AFE0100) && !is_vip(0x0AFE01FF) && !is_vip(0xC0A80001));
    CHECK(parse_policy("always") == Policy::kAlways && parse_policy("off") == Policy::kOff &&
          parse_policy("") == Policy::kAuto && parse_policy(nullptr) == Policy::kAuto &&
          parse_policy("auto") == Policy::kAuto && parse_policy("bogus") == Policy::kAuto &&
          parse_policy("false") == Policy::kOff && parse_policy("0") == Policy::kOff);
  }
  {  // route policy, Takedawn's timeline
    const uint32_t mypub = 0x4F00DE89;
    Peer p;
    p.vip = B;
    Endpoint to;
    const uint32_t t0 = 100000;
    CHECK(route(p, Policy::kAuto, t0, mypub, &to) == Via::kRelay);  // nothing known: the relay
    p.first_tx = t0;
    CHECK(ask_due(p, Policy::kAuto, t0) && !ask_due(p, Policy::kAlways, t0));
    p.asked = t0;
    CHECK(!ask_due(p, Policy::kAuto, t0 + 500) && ask_due(p, Policy::kAuto, t0 + 1000));
    p.have_info = true;
    p.pub = {0x50000001, 4000};
    p.lan = {0xC0A80006, 9103};
    CHECK(route(p, Policy::kAuto, t0 + 10, mypub, &to) == Via::kDirect && to == p.pub);  // other house: public
    p.pub.ip = mypub;
    CHECK(route(p, Policy::kAuto, t0 + 10, mypub, &to) == Via::kDirect && to == p.lan);  // same house: LAN
    CHECK(route(p, Policy::kAuto, t0 + 1999, mypub, &to) == Via::kDirect);
    CHECK(route(p, Policy::kAuto, t0 + 2000, mypub, &to) == Via::kRelay);  // 2 s without a direct path
    CHECK(route(p, Policy::kAlways, t0 + 10, mypub, &to) == Via::kRelay);  // forced
    CHECK(probe_due(p, Policy::kAuto, t0) && !probe_due(p, Policy::kAlways, t0));
    p.probed = t0;
    CHECK(!probe_due(p, Policy::kAuto, t0 + 400) && probe_due(p, Policy::kAuto, t0 + 500));
    // its probe arrives, without "hears you": one-way, the relay stays
    probe_seen(p, p.lan, 0, t0 + 3000);
    CHECK(probe_flags(p, t0 + 3000) == 1);
    CHECK(route(p, Policy::kAuto, t0 + 3000, mypub, &to) == Via::kRelay);
    // both ways: direct
    probe_seen(p, p.lan, 1, t0 + 3500);
    CHECK(route(p, Policy::kAuto, t0 + 3500, mypub, &to) == Via::kDirect && to == p.lan);
    CHECK(route(p, Policy::kAlways, t0 + 3500, mypub, &to) == Via::kRelay);  // always never goes direct
    // probes slow down after 20 s once both ways work
    p.probed = t0 + 21000;
    probe_seen(p, p.lan, 1, t0 + 21000);
    CHECK(!probe_due(p, Policy::kAuto, t0 + 22000) && probe_due(p, Policy::kAuto, t0 + 36000));
    // direct quiet for 30 s: back to the relay
    CHECK(route(p, Policy::kAuto, t0 + 21000 + 29999, mypub, &to) == Via::kDirect);
    CHECK(route(p, Policy::kAuto, t0 + 21000 + 30000, mypub, &to) == Via::kRelay);
    // a peer that reached us through the relay is answered through it before the 2 s are up
    Peer q;
    q.first_tx = t0, q.have_info = true, q.pub = {0x50000001, 4000}, q.relay_heard = true;
    CHECK(route(q, Policy::kAuto, t0 + 1, mypub, &to) == Via::kRelay);
  }
  std::printf(fails ? "tunneltest: %d FAILED\n" : "tunneltest: all passed\n", fails);
  return fails != 0;
}
