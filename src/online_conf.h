// online_conf.h: what the online_server value of splitsecond.conf means. No Windows calls, so test/onlinetest.cpp
// checks it on any machine (`make onlinetest`). online.cpp uses it at start.
#pragma once
#include <cstdlib>
#include <string>

// The project's replacement server: no line or an empty value = this. A host name, never a literal address: the DLL
// resolves it at start (online.cpp, ResolveServer) and again at each game lookup if that first try failed.
constexpr char kOnlineDefaultServer[] = "vps4954.rackhostvps.com";
constexpr int kOnlineGamePort = 31120;  // mov word [esi+8], 0x7990 at 0xA25999: fixed in the game's code

struct OnlineServer {
  bool off = false;         // off / false / 0 / no / none: nothing is hooked
  bool by_default = false;  // no line or an empty value: kOnlineDefaultServer
  std::string host;         // a host name or a dotted IPv4 address, without the port
  int port = kOnlineGamePort;
};

// "host[:port]" with trailing blanks; the port, when given, is only reported (the game's port is not redirected).
inline OnlineServer ss_parse_online_server(std::string v) {
  OnlineServer s;
  while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.pop_back();
  if (v == "off" || v == "false" || v == "0" || v == "no" || v == "none") {
    s.off = true;
    return s;
  }
  s.by_default = v.empty();
  if (s.by_default) v = kOnlineDefaultServer;
  const size_t colon = v.rfind(':');
  if (colon != std::string::npos) {
    s.port = std::atoi(v.c_str() + colon + 1);
    v.resize(colon);
  }
  s.host = v;
  return s;
}
