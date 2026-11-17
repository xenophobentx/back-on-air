// streampool.cpp: the sector streaming pool's size (SSPC-51). Overlay row ADVANCED > STREAMING MEMORY.
//   stream_pool = game | <MB> : game (default) leaves the game's 57 MB (the Xbox 360's size); a number (the row offers
//                         70) is set as the live pool limit at every level load. Once the pool's nodes in use reach
//                         480 the limit is held at the bytes then in use for the rest of that level (the node cap).
//   stream_pool_log = 1 : one "[pool]" line a second: limit, used, nodes, evictions and quick reloads, free address space.
// Found in the decrypted image: the tweakable "Sectors/Streaming/d; Memory Pool MB" is the int 0xD389E0 (57); the
// sector manager's constructor reads it once (0x7C654F, MB << 20). manager = [[[0xD714D0]+0x28]+0x34], its items in
// three lists (0x7C3180): +0x18/+0x1C (item = [[arr[i]+4]]), +8/+0xC and +0x10/+0x14 (item = arr[i]); item state +4
// (3 resident). pool = [[manager+0x54]+0xC]: limit +4, used +0x250 (the request check 0xABEF90), nodes in use +0x40.
// The pool has 512 nodes at most (an immediate in .text, not changed): at the cap 0xABEC00 gets no node but 0xABEF90
// still counts the bytes, and that item cannot be evicted until the next level; the hold stops short of that.
// Only pool+4 (heap data) is written, from the Present hook; the game's code is never touched.
#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "menu.h"
#include "ss_shared.h"

namespace {

constexpr uintptr_t kGame = 0xD714D0;
const uint8_t kCtorSig[] = {0xA1, 0xE0, 0x89, 0xD3, 0x00};        // 0x7C654F: mov eax, [0xD389E0]
const uint8_t kCheckSig[] = {0x8B, 0x8F, 0x50, 0x02, 0x00, 0x00};  // 0xABEF9E: mov ecx, [edi+0x250]
constexpr uint32_t kHoldNodes = 480;

bool Get(uintptr_t at, void* out, size_t n) {
  SIZE_T got = 0;
  return at >= 0x10000 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(at), out, n, &got) && got == n;
}
uint32_t U32(uintptr_t at) {
  uint32_t v = 0;
  return Get(at, &v, 4) ? v : 0;
}
double Mb(uint64_t b) { return b / 1048576.0; }
int ParseMb(const std::string& v) {
  const int mb = (v.empty() || v == "game") ? 0 : atoi(v.c_str());
  return mb <= 0 ? 0 : mb < 16 ? 16 : mb > 512 ? 512 : mb;
}
// The pool and its live limit, or 0 while there is none (menus, loading).
uintptr_t Pool(uintptr_t* mgr_out = nullptr) {
  const uintptr_t env = U32(U32(kGame) + 0x28), mgr = env ? U32(env + 0x34) : 0;
  const uintptr_t pool = mgr ? U32(U32(mgr + 0x54) + 0xC) : 0;
  const uint32_t limit = pool ? U32(pool + 4) : 0;
  if (mgr_out) *mgr_out = mgr;
  return limit >= (8u << 20) && limit <= (1024u << 20) ? pool : 0;
}

volatile LONG g_want_mb;  // the row's / the file's value; 0 = game
int g_ok;                 // 0 not yet checked, 1 the known code, -1 off
DWORD g_first;
uintptr_t g_pool;         // the pool of this level (render thread)
uint32_t g_game_limit, g_set;  // the game's limit at the load; what we wrote (0: nothing)
volatile LONG g_held, g_peak, g_max;  // held at 480; the nodes' highest after the hold; and over the level
unsigned g_skip;

void LevelEnd() {
  if (g_pool && g_held) ss_log("[pool] after the hold the nodes peaked at %ld (limit %.1f MB)", g_peak, Mb(g_set));
  g_pool = 0, g_set = 0, g_held = 0, g_peak = 0, g_max = 0;
}

// Diagnostics only (stream_pool_log): every 100 ms the items' states, once a second the line.
DWORD WINAPI LogThread(void*) {
  constexpr int kMax = 4096;
  static uintptr_t item[kMax];
  static uint8_t prev[kMax];
  static DWORD evicted_at[kMax];
  uintptr_t mgr_seen = 0;
  long ev = 0, rl = 0;
  for (int tick = 0;; Sleep(100), ++tick) {
    uintptr_t mgr = 0;
    const uintptr_t pool = Pool(&mgr);
    if (!pool) { mgr_seen = 0; continue; }
    if (mgr != mgr_seen) mgr_seen = mgr, memset(item, 0, sizeof item), memset(prev, 0, sizeof prev), ev = rl = 0;
    int n = 0;
    for (int l = 0; l < 3; ++l) {
      const uint32_t cnt = U32(mgr + (l == 0 ? 0x18 : l == 1 ? 8 : 0x10)), arr = U32(mgr + (l == 0 ? 0x1C : l == 1 ? 0xC : 0x14));
      for (uint32_t i = 0; i < cnt && cnt <= 4096 && n < kMax; ++i, ++n) {
        if (tick % 10) continue;  // the pointers once a second
        const uintptr_t e = U32(arr + 4 * i), it = l == 0 ? U32(U32(e + 4)) : e;
        if (it != item[n]) item[n] = it, prev[n] = 0, evicted_at[n] = 0;
      }
    }
    const DWORD now = GetTickCount() | 1;
    for (int i = 0; i < n; ++i) {
      const uint32_t st = item[i] ? U32(item[i] + 4) : 0;
      if (prev[i] == 3 && st != 3) ++ev, evicted_at[i] = now;
      if (prev[i] != 3 && st == 3 && evicted_at[i] && now - evicted_at[i] < 10000) ++rl;
      prev[i] = static_cast<uint8_t>(st > 9 ? 9 : st);
    }
    if (tick % 10) continue;
    uint64_t free_b = 0, big = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t a = 0x10000; a < 0x7FFF0000 && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi);) {
      if (mbi.State == MEM_FREE) free_b += mbi.RegionSize, big = mbi.RegionSize > big ? mbi.RegionSize : big;
      const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
      if (next <= a) break;
      a = next;
    }
    ss_log("[pool] limit %.1f used %.1f MB nodes %u (max %ld) | evict %ld reload10 %ld | free %u MB largest %u MB%s",
           Mb(U32(pool + 4)), Mb(U32(pool + 0x250)), U32(pool + 0x40), g_max, ev, rl, static_cast<unsigned>(free_b >> 20),
           static_cast<unsigned>(big >> 20), g_held ? " | held" : "");
    ev = rl = 0;
  }
}

bool Apply(const char* key, const char* value) {
  if (strcmp(key, "stream_pool") || g_ok < 0) return false;
  InterlockedExchange(&g_want_mb, ParseMb(value ? value : ""));
  ss_log("[pool] stream_pool = %s: from the next level load", value ? value : "");
  return true;
}

}  // namespace

static MenuRegistrar g_pool_row(MenuChoiceRow("stream_pool", "STREAMING MEMORY", MENU_PAGE_ADVANCED, MENU_LIVE, "game",
                                              {{"game", "GAME (57 MB)"}, {"70", "LARGER (70 MB)"}}, Apply));

void ss_streampool_start() {
  const std::string l = ss_conf("stream_pool_log");
  g_want_mb = ParseMb(ss_conf("stream_pool"));
  if (!l.empty() && l != "0" && l != "off" && l != "false") CreateThread(nullptr, 0, LogThread, nullptr, 0, nullptr);
}

// From patch.cpp's Present hook. At a new pool (a level load; a restart at the same address shows as a limit that
// is not the one written) the chosen limit is set; then the nodes are checked every frame.
void ss_streampool_present() {
  if (g_ok <= 0) {
    if (g_ok < 0) return;
    const DWORD now = GetTickCount();
    if (!g_first) g_first = now;
    uint8_t a[sizeof kCtorSig], b[sizeof kCheckSig];
    if (Get(0x7C654F, a, sizeof a) && !memcmp(a, kCtorSig, sizeof a) && Get(0xABEF9E, b, sizeof b) && !memcmp(b, kCheckSig, sizeof b)) {
      g_ok = 1, ss_log("[pool] stream_pool %ld MB (0 = game), hold at %u nodes", g_want_mb, kHoldNodes);
    } else {
      if (now - g_first > 120000) g_ok = -1, ss_log("[pool] not the known build (the pool's code differs): off");
      return;
    }
  }
  const uint32_t want = static_cast<uint32_t>(g_want_mb) << 20;
  if (!want && !g_set && ++g_skip % 30) return;  // the game's size: only the level changes are followed
  const uintptr_t pool = Pool();
  if (!pool) { LevelEnd(); return; }
  const uint32_t limit = U32(pool + 4);
  if (pool != g_pool || (g_set && limit != g_set)) {
    LevelEnd();
    g_pool = pool, g_game_limit = limit;
    if (want && want != limit) {
      SIZE_T put = 0;
      if (WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(pool + 4), &want, 4, &put) && put == 4) g_set = want;
    }
    ss_log("[pool] level load: pool %p, game's limit %.1f MB, limit now %.1f MB", reinterpret_cast<void*>(pool), Mb(limit),
           Mb(g_set ? g_set : limit));
  }
  const LONG nodes = static_cast<LONG>(U32(pool + 0x40));
  if (nodes > g_max) g_max = nodes;
  if (g_held && nodes > g_peak) g_peak = nodes;
  if (!g_set || g_held || nodes < static_cast<LONG>(kHoldNodes)) return;
  const uint32_t used = U32(pool + 0x250), keep = used > g_game_limit ? used : g_game_limit;
  SIZE_T put = 0;
  if (keep < g_set && WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(pool + 4), &keep, 4, &put) && put == 4) g_set = keep;
  g_held = 1, g_peak = nodes;
  ss_log("[pool] nodes %ld of 512: limit held at %.1f MB (used %.1f MB) for this level", nodes, Mb(g_set), Mb(used));
}
