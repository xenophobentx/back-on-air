// onlinetest: src/online_conf.h (the online_server value of splitsecond.conf) and src/online_text.h (the sign-in
// name rule, the log filter). `make onlinetest`.
#include "../src/online_conf.h"
#include "../src/online_text.h"

#include <cstdio>
#include <cstring>

static int g_fail = 0;
#define CHECK(c)                                                  \
  do {                                                            \
    if (!(c)) {                                                   \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);    \
      ++g_fail;                                                   \
    }                                                             \
  } while (0)

int main() {
  // The built-in default: the project server by host name, the game's port.
  OnlineServer s = ss_parse_online_server("");
  CHECK(!s.off && s.by_default && s.host == kOnlineDefaultServer && s.port == kOnlineGamePort);
  s = ss_parse_online_server(" \t\r");
  CHECK(s.by_default && s.host == kOnlineDefaultServer);

  s = ss_parse_online_server("rdv.example.org");
  CHECK(!s.off && !s.by_default && s.host == "rdv.example.org" && s.port == kOnlineGamePort);
  s = ss_parse_online_server("rdv.example.org:31120\r");
  CHECK(s.host == "rdv.example.org" && s.port == 31120);
  s = ss_parse_online_server("localhost:4000");
  CHECK(s.host == "localhost" && s.port == 4000);

  for (const char* off : {"off", "false", "0", "no", "none", "off  "}) {
    s = ss_parse_online_server(off);
    CHECK(s.off && s.host.empty());
  }
  s = ss_parse_online_server("Off");  // case matters, as before: a name, looked up (and failing) like any other
  CHECK(!s.off && s.host == "Off");

  // Sign-in names: accents become plain letters, Cyrillic Latin letters; nothing usable left -> the caller's "Racer".
  CHECK(ss_online_base("\xC3\x81rp\xC3\xA1" "d") == "Arpad");                // UTF-8, as in loginusers.vdf
  CHECK(ss_online_base("\xC1rp\xE1" "d") == "Arpad");                         // Latin-1 / ANSI, as in the registry
  CHECK(ss_online_base("\xC5\x90rs \xC3\x9Cr\xC5\xB1") == "Ors Uru");          // Őrs Ürű
  CHECK(ss_online_base("Stra\xC3\x9F" "e") == "Strasse");
  CHECK(ss_online_base("\xC5\x81\xC3\xB3" "d\xC5\xBA") == "Lodz");             // Łódź
  CHECK(ss_online_base("\xD0\x94\xD0\xBC\xD0\xB8\xD1\x82\xD1\x80\xD0\xB8\xD0\xB9") == "Dmitriy");   // Дмитрий
  CHECK(ss_online_base("\xD0\xA9\xD1\x83\xD0\xBA\xD0\xB0") == "Shchuka");  // Щука
  CHECK(ss_online_base("\xE5\xB1\xB1\xE7\x94\xB0").empty());                 // 山田
  CHECK(ss_online_base("\xEA\xB9\x80 \xE2\x98\x85").empty());                // 김 ★
  CHECK(ss_online_base("\xE5\xB1\xB1\xE7\x94\xB0 Taro") == "Taro");
  CHECK(ss_online_base("  --Racer  One!!") == "Racer One");
  CHECK(ss_online_base("abcdefghijklmnopq") == "abcdefghijkl");
  CHECK(ss_online_base("\xF0\x9F\x8F\x8E Speed") == "Speed");               // 🏎 Speed
  CHECK(ss_online_base("\xC3") == "A");                                       // a cut sequence: read as Latin-1
  CHECK(std::string(kOnlineFallbackBase) == "Racer");

  // The log filter: the user folder of a path becomes <user>; nothing else changes.
  char out[256];
  auto redact = [&](const char* in) {
    ss_redact_user_folders(in, out, sizeof out);
    return std::string(out);
  };
  CHECK(redact("loginusers.vdf in Z:\\home\\deck\\.local\\share\\Steam, user by MostRecent") ==
        "loginusers.vdf in Z:\\home\\<user>\\.local\\share\\Steam, user by MostRecent");
  CHECK(redact("C:\\Users\\John Smith\\Documents\\x") == "C:\\Users\\<user>\\Documents\\x");
  CHECK(redact("c:/users/racer/steam") == "c:/users/<user>/steam");
  CHECK(redact("/home/deck/.steam/steam") == "/home/<user>/.steam/steam");
  CHECK(redact("folder C:\\Users\\racer (its save)") == "folder C:\\Users\\<user> (its save)");
  CHECK(redact("C:\\Program Files (x86)\\Steam") == "C:\\Program Files (x86)\\Steam");
  CHECK(redact("homework\\users and /home") == "homework\\users and /home");
  CHECK(redact("[online] sign-in name \"Racer-7QK\"") == "[online] sign-in name \"Racer-7QK\"");
  char small[8];
  CHECK(ss_redact_user_folders("/home/deck/x", small, sizeof small) == 7 && std::string(small) == "/home/<");

  std::printf(g_fail ? "onlinetest: %d FAILED\n" : "onlinetest: ok\n", g_fail);
  return g_fail ? 1 : 0;
}
