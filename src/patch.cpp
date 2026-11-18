// Split/Second (PC, Steam build 385319): 60 ticks a second without writing a single byte of the game's code.
//
// The executable's code and its read-only data are never modified, protected or read for patching: the
// copy protection that wraps the exe reacted to code writes (threads spinning in its exception handler,
// frame rate collapsing minutes into a race). This file only
//   * copies the vtables of the eight "game state" objects into this DLL and points the objects' vptr (a
//     field in the game's own writable data) at the copies; slot 2 of the copy is our frame step. The
//     step calls the game's own update/render virtuals through the original vtable;
//   * gives every SSAI::cAIPlayer instance a private vtable copy whose update slot is our gate, so the
//     steering controller (a fixed 1/30 s step) runs every second tick;
//   * replaces the two rewind rings (180 entries) by 360 entry rings in the heap, using the game's own
//     allocator and array helpers, before anything has been recorded.
// Every address is a run-time VA of the Steam build 385319 (image base 0x400000). Before anything is done
// the vtables and the objects are compared with the known values; on any mismatch the DLL does nothing.
#include <windows.h>
#include <mmsystem.h>
#include <tlhelp32.h>
#include <io.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include "menu.h"
#include "online_text.h"
#include "ss_common.h"
#include "ss_shared.h"

#define THISCALL __attribute__((thiscall))
#define ENTRY __attribute__((thiscall, force_align_arg_pointer))

volatile unsigned long ss_vq_calls = 0;  // ss_shared.h: cost counters for the [frames] split line
volatile unsigned long ss_vp_calls = 0;
volatile unsigned long ss_shader_map_gen = 0;  // ss_shared.h
volatile int ss_game_state = -1;  // the state whose frame step ran last (kStates index; ss_shared.h)

// ss_shared.h: the readable-region cache of one phase of a frame (main thread only). 64 regions (16 were too few: a
// race update follows pointers into more regions than that, and the oldest were pushed out and queried again on every
// update, about 10 VirtualQuery calls a present; SSPC-17 round 2), and 8 regions found NOT readable in the phase
// (a stale pointer into freed or reserved memory was queried again on every call).
constexpr int kRegions = 64, kBadRegions = 8;
static struct {
  uintptr_t lo[kRegions], hi[kRegions];
  uintptr_t bad_lo[kBadRegions], bad_hi[kBadRegions];
  int n = 0, next = 0, nbad = 0, next_bad = 0;
  volatile DWORD tid = 0;  // the game's main thread (set at every step); 0: no step yet, nothing is kept
  bool on = true;
} g_rc;

static void RegionsForget(bool main_thread) {
  if (main_thread) g_rc.tid = GetCurrentThreadId();
  g_rc.n = g_rc.next = 0;
  g_rc.nbad = g_rc.next_bad = 0;
}

bool ss_region_known(uintptr_t p, size_t n) {
  if (!g_rc.n || GetCurrentThreadId() != g_rc.tid) return false;
  const uintptr_t e = p + n;
  if (e < p) return false;
  for (int i = 0; i < g_rc.n; ++i)
    if (p >= g_rc.lo[i] && e <= g_rc.hi[i]) return true;
  return false;
}

bool ss_region_bad(uintptr_t p) {
  // Above the highest user address: VirtualQuery fails there (the answer was "not readable" after a system call;
  // a float or a tag read as a pointer, e.g. 0xBF800000, is such an address), on any thread, in any phase.
  static const uintptr_t top = [] {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
  }();
  if (p > top) return true;
  if (!g_rc.nbad || GetCurrentThreadId() != g_rc.tid) return false;
  for (int i = 0; i < g_rc.nbad; ++i)
    if (p >= g_rc.bad_lo[i] && p < g_rc.bad_hi[i]) return true;
  return false;
}

void ss_region_note(uintptr_t base, size_t size, unsigned long type) {
  if (!g_rc.on || type == MEM_MAPPED || !g_rc.tid || GetCurrentThreadId() != g_rc.tid) return;
  int i;
  if (g_rc.n < kRegions) {
    i = g_rc.n++;
  } else {
    i = g_rc.next;
    g_rc.next = (g_rc.next + 1) % kRegions;
  }
  g_rc.lo[i] = base;
  g_rc.hi[i] = base + size;
}

void ss_region_note_bad(uintptr_t base, size_t size) {
  if (!g_rc.on || !g_rc.tid || GetCurrentThreadId() != g_rc.tid || base + size < base) return;
  int i;
  if (g_rc.nbad < kBadRegions) {
    i = g_rc.nbad++;
  } else {
    i = g_rc.next_bad;
    g_rc.next_bad = (g_rc.next_bad + 1) % kBadRegions;
  }
  g_rc.bad_lo[i] = base;
  g_rc.bad_hi[i] = base + size;
}

namespace {

// ---------------------------------------------------------------- log, conf

char g_dir[MAX_PATH];
FILE* g_log = nullptr;
bool g_stats = false;
volatile LONG g_crash_armed = 0;  // CrashLog logs (from the first step, or at once with SS_PATCH_LOG=1)

void LogV(const char* fmt, va_list ap) {
  if (!g_log) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  char buf[768];
  int n = snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
  vsnprintf(buf + n, sizeof buf - n - 2, fmt, ap);
  // The log goes into public bug reports: no user name from a path (online_text.h).
  char line[sizeof buf];
  size_t len = ss_redact_user_folders(buf, line, sizeof line - 2);
  line[len] = '\n';
  line[len + 1] = 0;
  fputs(line, g_log);
  fflush(g_log);
}

void Log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  LogV(fmt, ap);
  va_end(ap);
}

std::string ConfValue(const char* key) {
  std::string path = std::string(g_dir) + "splitsecond.conf";
  FILE* f = fopen(path.c_str(), "r");
  if (!f) return "";
  std::string result;
  char line[512];
  while (fgets(line, sizeof line, f)) {
    char* p = line;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '#' || *p == ';' || *p == '\n' || *p == 0) continue;
    char* eq = strchr(p, '=');
    if (!eq) continue;
    std::string k(p, eq);
    std::string v(eq + 1);
    auto trim = [](std::string& s) {
      while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
      size_t i = 0;
      while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
      s.erase(0, i);
    };
    trim(k);
    trim(v);
    if (k == key) result = v;  // the last of a repeated key wins
  }
  fclose(f);
  return result;
}

// env.NAME = value lines of splitsecond.conf: environment variables of this process, set at start (before the
// game makes its Direct3D object, so DXVK and the Vulkan loader read them: DXVK_CONFIG, VK_LOADER_LAYERS_DISABLE,
// ...). The exe restarts itself on Windows and the second process does not get the launcher's environment; this
// way it does. An empty value removes the variable.
void ConfEnvironment() {
  std::string path = std::string(g_dir) + "splitsecond.conf";
  FILE* f = fopen(path.c_str(), "r");
  if (!f) return;
  char line[1024];
  while (fgets(line, sizeof line, f)) {
    char* p = line;
    while (*p == ' ' || *p == '\t') ++p;
    if (strncmp(p, "env.", 4)) continue;
    char* eq = strchr(p, '=');
    if (!eq) continue;
    std::string k(p + 4, eq), v(eq + 1);
    while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' || v.back() == '\n')) v.pop_back();
    v.erase(0, v.find_first_not_of(" \t") == std::string::npos ? v.size() : v.find_first_not_of(" \t"));
    if (k.empty()) continue;
    const BOOL ok = SetEnvironmentVariableA(k.c_str(), v.empty() ? nullptr : v.c_str());
    Log("[env] %s = %s%s", k.c_str(), v.empty() ? "(removed)" : v.c_str(), ok ? "" : " (not set: the call failed)");
  }
  fclose(f);
}

// The catalog's frame-rate row: fps = 30 | 60 (switch SS_HFR = 0 | 1), and above it fps = 120 | native | <61..240>
// (SS_HFR = a number | native; docs/plan-120fps.md). 0: the game as it ships (30); -1: native, the display's rate.
double RateOf(const std::string& v) {
  if (v.empty()) return 0;
  if (v == "60" || v == "1" || v == "true" || v == "yes" || v == "on") return 60;
  if (v == "native") return -1;
  char* end = nullptr;
  const double n = strtod(v.c_str(), &end);
  if (end && end != v.c_str() && *end == 0 && n > 60 && n <= 240) return n;
  return 0;
}

double WantedRate() {
  const char* env = getenv("SS_HFR");
  return RateOf(env ? env : ConfValue("fps"));
}

// fps = native: the display mode's refresh rate now (under Proton / gamescope the nested output's), 60..240; within
// 1 Hz of 60, or unknown, is 60.
double NativeRate() {
  DEVMODEA dm = {};
  dm.dmSize = sizeof dm;
  const DWORD hz = EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm) ? dm.dmDisplayFrequency : 0;
  return hz <= 61 ? 60.0 : hz > 240 ? 240.0 : double(hz);
}

bool Off(const char* what) {  // SS_PATCH_OFF=ai,rings,timer : leave a part out (bisecting)
  const char* e = getenv("SS_PATCH_OFF");
  return e && strstr(e, what);
}

// ----------------------------------------------------------------- helpers

inline uint32_t Rd(uintptr_t va) { return *reinterpret_cast<const volatile uint32_t*>(va); }
inline void Wr(uintptr_t va, uint32_t v) { *reinterpret_cast<volatile uint32_t*>(va) = v; }

// True when [p, p+n) is committed, readable memory (VirtualQuery only; nothing is touched).
bool Readable(uintptr_t p, size_t n) {
  if (p < 0x10000 || p + n < p) return false;
  if (ss_region_known(p, n)) return true;  // checked in this phase of the frame (ss_shared.h)
  if (ss_region_bad(p)) return false;
  while (n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!SS_VQ(reinterpret_cast<void*>(p), &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT || mbi.Protect == 0 || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
      ss_region_note_bad(reinterpret_cast<uintptr_t>(mbi.BaseAddress), mbi.RegionSize);
      return false;
    }
    ss_region_note(reinterpret_cast<uintptr_t>(mbi.BaseAddress), mbi.RegionSize, mbi.Type);
    uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    size_t avail = end - p;
    if (avail >= n) return true;
    n -= avail;
    p = end;
  }
  return true;
}

// ---------------------------------------------------------- build constants

// The eight "game state" objects (writable data) and their vtables (read-only data, 7 entries, RTTI locator at -4).
// Slot 2 is the frame step (a thunk to 0x558880), slot 5 update(ticks) -> next state, slot 6 render.
struct StateDef {
  uint32_t obj, vt, update, render;
};
const StateDef kStates[8] = {
    {0xD64E88, 0xBA2AA4, 0x411A30, 0x7D8120},  // cBootupState
    {0xD66018, 0xBA3110, 0x413E10, 0x413B00},  // cInGameLoadState
    {0xD6606C, 0xBA32DC, 0x414EB0, 0x414D00},  // cIntroState
    {0xD660A4, 0xBA3398, 0x4151F0, 0x4152D0},  // cFrontendLoadState
    {0xD66ADC, 0xBA3518, 0x417310, 0x417640},  // cInGameMovieState
    {0xD66B14, 0xBA36A4, 0x417940, 0x417E70},  // cInGameRunState
    {0xD66B48, 0xBA3894, 0x418BF0, 0x7D8120},  // cLoadDemoState
    {0xD67524, 0xBA38C0, 0x419000, 0x4194A0},  // cFrontendRunState
};
constexpr uint32_t kStepThunk = 0x413530;  // slot 2 of every state vtable (jmp 0x558880)
constexpr uint32_t kStepReal = 0x558880;

constexpr uintptr_t kFrameNo = 0xD72330;     // present counter (one per 1/60 s of wall time)
constexpr uintptr_t kDtOut = 0xD72320;       // float, published by kDtFn
constexpr uintptr_t kAcc = 0xD747F4;         // float, the original limiter's accumulator
constexpr uintptr_t kBehind = 0xD747F0;      // int, the original limiter's "periods behind"
constexpr uintptr_t kMaxTicks = 0xD199D8;    // int, 10 (the console clamps to 4)
constexpr uintptr_t kPeriod = 0xBA0C14;      // float 1/30 (read-only data, shared by 28 users; only read)
constexpr uintptr_t kAppSingleton = 0xD646C8;
constexpr uintptr_t kDtFn = 0x50CF60;        // cdecl: QPC delta -> *kDtOut = min(dt, 0.2 s)
constexpr uintptr_t kPumpFn = 0x410380;      // thiscall(app): message pump + input poll

constexpr uintptr_t kRaceRun = 0xD66B28, kRaceLoad = 0xD66034;  // race object (run state +0x14, load state +0x1C)
constexpr uint32_t kAiMgrVt = 0xBC0A84, kAiPlayerVt = 0xBC0BB4;
constexpr int kAiPlayerSlots = 18;
constexpr uint32_t kAiUpdate = 0x51A8B0;  // cAIPlayer vtable slot 3 (this, input, output), ret 8

constexpr uintptr_t kRewindMgr = 0xD71264;
// Rewind manager fields read by the slow-motion replay hold: +0x100 a playback is running, +0x108 it is a slow-motion one
// (copied from +0x10C when the playback starts). The manager's update (0x7BDEA0, called directly by the race update 0x4B2670)
// then returns ONE tick whatever the record holds, and a record is left after as many updates as it holds ticks.
constexpr uint32_t kRewindPlaying = 0x100, kRewindSlow = 0x108;
constexpr uint32_t kInputRingVt = 0xC199DC;
constexpr uintptr_t kAlloc = 0x530290, kFree = 0x5302D0, kVecCtor = 0x7566F7, kVecDtor = 0x756694;
constexpr uint32_t kElemCtor = 0x7C9B40, kElemDtor = 0x7C9B60;

// -------------------------------------------------------------------- state

enum Mode { MODE_OFF = 0, MODE_PASS = 1, MODE_OWN30 = 2, MODE_SIXTY = 3 };
volatile int g_mode = MODE_OFF;
volatile bool g_ai_gate = true;
bool g_rings_on = true;
bool g_ai_on = true;
bool g_takedown_realtime = true;  // takedown_realtime / SS_TAKEDOWN_REALTIME: hold every second slowed update at 60
bool g_timer_on = true;
uint32_t g_maxticks60 = 4;
// The rate (docs/plan-120fps.md M1). g_rate is what fps asks for: 60, or more. Everything paces on the rate of the
// cadence now running: 60 (BASE: every state but a race whose simulation runs in 1/g_rate steps) or g_rate (RACE).
// With g_rate = 60 there is one cadence and every number below is the one the 60 fps build had.
double g_rate = 60.0;
volatile int g_cad = 0;       // 0 BASE (60), 1 RACE (g_rate)
bool g_fe_test = false;       // SS_PATCH_MODE=present120: the front end at 120, one tick an update (test aid)
volatile bool g_force_base = false;  // key-file command cad60: no RACE cadence and no time scale from now on (test aid)
// The AI gate's time base while a race runs in 1/g_rate steps: the game time of the update now running, in ticks
// of 1/60 s (the gate then counts time, not ticks).
volatile bool g_ai_frac = false;
double g_ai_units = 1.0;
inline double CadRate() { return g_cad ? g_rate : 60.0; }
double g_spin = 0.0015;  // seconds of the wait that are spun on QPC instead of slept
int64_t g_freq = 0;
volatile uint32_t g_ticks = 1;  // ticks of the update now running (for the AI gate)

alignas(16) uint32_t g_vt[8][16];  // private vtable copies: [0] = RTTI locator, [1..] = slots
bool g_vt_ready[8];
bool g_sig_checked = false;
bool g_code_ok = true;   // the called code looks like the expected build
bool g_alloc_ok = true;

alignas(16) uint32_t g_aivt[24];
bool g_ai_vt_ready = false;

// ------------------------------------------------------------- statistics

struct Stats {
  uint64_t steps = 0, ticks = 0, t1 = 0, t2 = 0, t3p = 0, ai_run = 0, ai_hold = 0, ai_flips = 0, ai_calls = 0;
  double ai_abs = 0, upd_s = 0, out_s = 0, wait_s = 0;
  int64_t t0 = 0, t_after = 0;
} g_st;

double Secs(int64_t d) { return double(d) / double(g_freq); }
int64_t Now() {
  LARGE_INTEGER n;
  QueryPerformanceCounter(&n);
  return n.QuadPart;
}

// -------------------------------------------------------------- AI gate

using AiFn = void(THISCALL*)(void*, void*, void*);
AiFn g_ai_orig = nullptr;

struct AiState {
  void* self = nullptr;
  uint32_t acc = 2;
  double facc = 2.0;
  uint8_t head[0x10] = {};
  uint8_t flag = 0;
  uint8_t tail[0x28] = {};
  float last_steer = 0;
  float pos[3] = {};
  bool has_pos = false;
  double dist = 0;
  float speed = 0;  // m/s over the last step
  // how much the car turns for how much steering: heading change over 2 m segments of its path, and |steer| x path
  double seg_x = 0, seg_z = 0, seg_len = 0, seg_head = 0, turn = 0, turn_signed = 0, steer_path = 0;
  bool has_head = false;
};
AiState g_ai[64];

// Measurement (SS_PATCH_LOG=1 only): the path each AI car has driven. The AI input block ([[in+0xC]], 0x519B50) holds
// the step time at +4 and the car's position at +8 (x, y, z; found by dumping it in a race). Only reads.
float g_ai_in_dt = 0;

void AiTrack(AiState* s, void* in, float steer) {
  const uintptr_t h = reinterpret_cast<uintptr_t>(in);
  if (!Readable(h + 0xC, 4)) return;
  const uintptr_t pp = Rd(h + 0xC);
  if (!pp || !Readable(pp, 4)) return;
  const uintptr_t p = Rd(pp);
  if (!p || !Readable(p, 0x14)) return;
  float v[4];
  memcpy(v, reinterpret_cast<const void*>(p + 4), sizeof v);
  g_ai_in_dt = v[0];
  if (s->has_pos) {
    const double dx = v[1] - s->pos[0], dy = v[2] - s->pos[1], dz = v[3] - s->pos[2];
    const double d = sqrt(dx * dx + dy * dy + dz * dz);
    if (d < 30.0) s->dist += d;  // a reset to the track is not driven
    if (d < 30.0 && v[0] > 1e-4f) s->speed = static_cast<float>(d / v[0]);
    if (d < 30.0) {
      s->steer_path += (steer < 0 ? -steer : steer) * d;
      s->seg_x += dx;
      s->seg_z += dz;
      s->seg_len += d;
      if (s->seg_len >= 2.0) {
        const double head = atan2(s->seg_x, s->seg_z);
        if (s->has_head) {
          double dh = head - s->seg_head;
          while (dh > 3.14159265358979) dh -= 6.28318530717959;
          while (dh < -3.14159265358979) dh += 6.28318530717959;
          if (dh > -1.0 && dh < 1.0) {  // a spin or a hit is not steering
            s->turn += dh < 0 ? -dh : dh;
            s->turn_signed += dh;
          }
        }
        s->seg_head = head;
        s->has_head = true;
        s->seg_x = s->seg_z = s->seg_len = 0;
      }
    } else {
      s->has_head = false;
      s->seg_x = s->seg_z = s->seg_len = 0;
    }
  }
  memcpy(s->pos, v + 1, sizeof s->pos);
  s->has_pos = true;
}

// Measurement (SS_PATCH_LOG=1 only): which bytes of the AI output block the game changes between a run of the AI
// and the next held call (the gate writes back +0x10..0x1F, +0xCD and +0xD0..0xF7; anything else that is reset
// every tick would only be set on the ticks the AI runs).
constexpr int kAiOutCensus = 0x100;
uint8_t g_ai_snap[64][kAiOutCensus];
bool g_ai_snap_ok[64];
uint32_t g_ai_diff[kAiOutCensus], g_ai_diff_calls = 0;

// The player's steering controller steps with a fixed 1/30 s: with one tick an update it runs when two ticks
// have gathered; on the other updates the outputs of the last run are written back.
void ENTRY AiGate(void* self, void* in, void* out) {
  const bool gated = g_mode == MODE_SIXTY && g_ai_gate;
  if (!gated && !(g_stats && g_mode == MODE_SIXTY)) {
    g_ai_orig(self, in, out);
    return;
  }
  AiState* s = nullptr;
  for (auto& e : g_ai) {
    if (e.self == self) { s = &e; break; }
    if (!e.self && !s) s = &e;
  }
  if (!s) { g_ai_orig(self, in, out); return; }
  if (s->self != self) { s->self = self; s->acc = 2; s->facc = 2.0; }
  uint8_t* o = static_cast<uint8_t*>(out);
  bool run;
  if (!gated) {  // gate off (key-file command ai0) with SS_PATCH_LOG=1: every call runs, and is measured
    run = true;
  } else if (g_ai_frac) {  // a race in 1/g_rate steps: run when 1/30 s of game time has gathered
    s->facc += g_ai_units;
    run = s->facc >= 2.0 - 1e-6;
    if (run) s->facc = s->facc >= 4.0 ? 0.0 : s->facc - 2.0;
  } else {
    s->acc += g_ticks;
    run = s->acc >= 2;
    if (run) s->acc %= 2;
  }
  if (run) {
    g_ai_orig(self, in, out);
    memcpy(s->head, o + 0x10, 0x10);
    s->flag = o[0xCD];
    memcpy(s->tail, o + 0xD0, 0x28);
    ++g_st.ai_run;
    if (g_stats && Readable(reinterpret_cast<uintptr_t>(o), kAiOutCensus)) {
      memcpy(g_ai_snap[s - g_ai], o, kAiOutCensus);
      g_ai_snap_ok[s - g_ai] = true;
    }
  } else {
    if (g_stats && g_ai_snap_ok[s - g_ai] && Readable(reinterpret_cast<uintptr_t>(o), kAiOutCensus)) {
      const uint8_t* was = g_ai_snap[s - g_ai];
      for (int i = 0; i < kAiOutCensus; ++i)
        if (o[i] != was[i]) ++g_ai_diff[i];
      ++g_ai_diff_calls;
    }
    memcpy(o + 0x10, s->head, 0x10);
    o[0xCD] = s->flag;
    memcpy(o + 0xD0, s->tail, 0x28);
    ++g_st.ai_hold;
  }
  if (g_stats) {
    float st;
    memcpy(&st, o + 0x1C, 4);
    ++g_st.ai_calls;
    g_st.ai_abs += st < 0 ? -st : st;
    if ((st < 0) != (s->last_steer < 0) && (st != 0) && (s->last_steer != 0)) ++g_st.ai_flips;
    s->last_steer = st;
    AiTrack(s, in, st);
  }
}

// Slot 17 (0x51A550, thiscall (this, a, b), ret 8, nothing returned) reads the car's curve through
// [[[[this+0x18]]+0x28]+0x54] (0xA463B6 in the call at 0x51A5FF). On a race restart at 60 Hz the AI manager's loop
// (0x518E60) can reach it on an update before that pointer is set: a read of [0+4] and the game ends. The console port
// froze at the same place at 60 fps. While the chain is not there yet, the call is skipped, as the function itself
// skips when it has nothing to send (0x51A58E); from the update the car is complete on, it runs as before.
constexpr uint32_t kAiSlot17 = 0x51A550;
uint32_t g_ai17_skipped = 0;
void ENTRY AiSlot17Guard(void* self, void* a, void* b) {
  const uintptr_t t = reinterpret_cast<uintptr_t>(self);
  uintptr_t p = Readable(t + 0x18, 4) ? Rd(t + 0x18) : 0;
  p = p && Readable(p, 4) ? Rd(p) : 0;
  p = p && Readable(p + 0x28, 4) ? Rd(p + 0x28) : 0;
  p = p && Readable(p + 0x54, 4) ? Rd(p + 0x54) : 0;
  if (!p || !Readable(p + 4, 4)) {
    if (g_ai17_skipped++ < 8) Log("[ai] player %p: its car is not set up yet (restart); this update's report call skipped", self);
    return;
  }
  reinterpret_cast<AiFn>(kAiSlot17)(self, a, b);
}

// Builds the private cAIPlayer vtable (slot 3 = AiGate) once. Needs the read-only data to be readable and sane.
bool PrepareAiVtable() {
  if (g_ai_vt_ready) return true;
  if (!Readable(kAiPlayerVt - 4, (kAiPlayerSlots + 1) * 4)) return false;
  if (Rd(kAiPlayerVt + 12) != kAiUpdate) return false;
  for (int i = 0; i < kAiPlayerSlots + 1; ++i) g_aivt[i] = Rd(kAiPlayerVt - 4 + 4 * i);
  g_ai_orig = reinterpret_cast<AiFn>(kAiUpdate);
  g_aivt[1 + 3] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&AiGate));
  if (g_aivt[1 + 17] == kAiSlot17) g_aivt[1 + 17] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&AiSlot17Guard));
  g_ai_vt_ready = true;
  return true;
}

// Every cAIPlayer the race's AI manager holds gets the private vtable (a vptr store into a heap object).
// race -> +0xBC SSAI::cAIManager (vptr 0xBC0A84) -> +0x14 count, +0x18 pointer array. Neither global is ever cleared
// by the game, so both are tried and every pointer is validated (readable, manager vptr) before it is followed.
uintptr_t g_ai_mgr = 0, g_seen_load = 0, g_seen_run = 0;
int g_ai_swapped_total = 0;

void SwapAiPlayersOf(uintptr_t race) {
  if (!race || !Readable(race, 0xC0)) return;
  uintptr_t mgr = Rd(race + 0xBC);
  if (!mgr || !Readable(mgr, 0x20) || Rd(mgr) != kAiMgrVt) return;
  if (mgr != g_ai_mgr) {
    g_ai_mgr = mgr;
    for (auto& e : g_ai) e = AiState();
    Log("[ai] race object %p, AI manager %p", reinterpret_cast<void*>(race), reinterpret_cast<void*>(mgr));
  }
  uint32_t count = Rd(mgr + 0x14);
  uintptr_t arr = Rd(mgr + 0x18);
  if (count == 0 || count > 64 || !arr || !Readable(arr, count * 4)) return;
  const uint32_t mine = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_aivt[1]));
  for (uint32_t i = 0; i < count; ++i) {
    uintptr_t p = Rd(arr + 4 * i);
    if (!p || !Readable(p, 4)) continue;
    if (Rd(p) == kAiPlayerVt) {
      Wr(p, mine);
      ++g_ai_swapped_total;
      Log("[ai] player %u at %p gated (%d so far)", i, reinterpret_cast<void*>(p), g_ai_swapped_total);
    }
  }
}

// True when one of the race pointers changed since the last call (a new race: look at once).
bool RaceChanged() {
  uintptr_t l = Rd(kRaceLoad), r = Rd(kRaceRun);
  bool ch = l != g_seen_load || r != g_seen_run;
  g_seen_load = l;
  g_seen_run = r;
  return ch;
}

void SwapAiPlayers() {
  uintptr_t l = Rd(kRaceLoad), r = Rd(kRaceRun);
  SwapAiPlayersOf(l);
  if (r != l) SwapAiPlayersOf(r);
}

// ---------------------------------------------------------- rewind rings

using AllocFn = void*(__cdecl*)(uint32_t);
using FreeFn = void(__cdecl*)(void*);
using VecCtorFn = void(__stdcall*)(void*, uint32_t, uint32_t, void*, void*);
using VecDtorFn = void(__stdcall*)(void*, uint32_t, uint32_t, void*);

uintptr_t g_ring_mgr = 0;
bool g_ring_mgr_done = false, g_ring_in_done = false, g_ring_gave_up = false;
int g_ring_tries = 0;

void FixRings() {
  uintptr_t m = Rd(kRewindMgr);
  if (m != g_ring_mgr) {
    g_ring_mgr = m;
    g_ring_mgr_done = g_ring_in_done = g_ring_gave_up = false;
    g_ring_tries = 0;
  }
  if (!m || g_ring_gave_up || (g_ring_mgr_done && g_ring_in_done) || !Readable(m, 0x138)) return;
  auto alloc = reinterpret_cast<AllocFn>(kAlloc);
  auto release = reinterpret_cast<FreeFn>(kFree);

  // Nothing may have been recorded yet: the rings are built while loading, records are written per update.
  if (!g_ring_mgr_done) {
    uintptr_t ring = Rd(m + 0x11C);
    uint32_t cap = Rd(m + 0x120), first = Rd(m + 0x124), count = Rd(m + 0x128);
    if (!ring || cap == 0) return;  // still being built
    if (cap != 180 || first != 0 || count != 0) {
      Log("[rewind] manager %p not replaced: capacity %u first %u count %u", reinterpret_cast<void*>(m), cap, first, count);
      g_ring_gave_up = true;
      return;
    }
    void* n = alloc(360 * 12);
    if (!n) { g_ring_gave_up = true; return; }
    memset(n, 0, 360 * 12);
    Wr(m + 0x11C, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(n)));
    Wr(m + 0x120, 360);
    release(reinterpret_cast<void*>(ring));
    g_ring_mgr_done = true;
    Log("[rewind] manager %p ring 180 -> 360 records", reinterpret_cast<void*>(m));
  }
  if (!g_ring_in_done) {
    uintptr_t s = 0;
    for (int i = 0; i < 64; ++i) {
      uintptr_t c = Rd(m + 4 * i);
      if (c && Readable(c, 0x20) && Rd(c) == kInputRingVt) { s = c; break; }
    }
    if (!s) {
      if (++g_ring_tries > 300) {
        Log("[rewind] player input ring not found; left at 180");
        g_ring_gave_up = true;
      }
      return;
    }
    uintptr_t old = Rd(s + 0xC);
    uint32_t cap = Rd(s + 0x10), i14 = Rd(s + 0x14), i18 = Rd(s + 0x18);
    if (!old || cap != 180 || i14 != 0 || i18 != 0) {
      Log("[rewind] player input ring %p not replaced: array %p capacity %u indices %u %u", reinterpret_cast<void*>(s),
          reinterpret_cast<void*>(old), cap, i14, i18);
      g_ring_gave_up = true;
      return;
    }
    uint8_t* n = static_cast<uint8_t*>(alloc(360 * 0x600 + 4));
    if (!n) { g_ring_gave_up = true; return; }
    *reinterpret_cast<uint32_t*>(n) = 360;
    reinterpret_cast<VecCtorFn>(kVecCtor)(n + 4, 0x600, 360, reinterpret_cast<void*>(kElemCtor), reinterpret_cast<void*>(kElemDtor));
    Wr(s + 0xC, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(n + 4)));
    Wr(s + 0x10, 360);
    reinterpret_cast<VecDtorFn>(kVecDtor)(reinterpret_cast<void*>(old), 0x600, 180, reinterpret_cast<void*>(kElemDtor));
    release(reinterpret_cast<void*>(old - 4));
    g_ring_in_done = true;
    Log("[rewind] player input ring %p 180 -> 360 entries", reinterpret_cast<void*>(s));
  }
}

// ------------------------------------------------------------ the step

using StepFn = uint32_t(THISCALL*)(void*);
using UpdateFn = uint32_t(THISCALL*)(void*, uint32_t);
using RenderFn = void(THISCALL*)(void*);
using PumpFn = void(THISCALL*)(void*);
using DtFn = void(__cdecl*)();

// Waits `sec` seconds: Sleep for the whole milliseconds that leave `g_spin` over, then spin on QPC.
void WaitSeconds(double sec) {
  if (sec <= 0) return;
  int64_t target = Now() + int64_t(sec * double(g_freq));
  for (;;) {
    double rem = Secs(target - Now());
    if (rem <= 0) break;
    double coarse = rem - g_spin;
    if (coarse >= 0.001)
      Sleep(static_cast<DWORD>(coarse * 1000.0));
    else
      YieldProcessor();
  }
}

void WaitUntil(int64_t target) {
  if (target > Now()) WaitSeconds(Secs(target - Now()));
}

// ---------------------------------------------------------- frame pacing at the present
//
// The step's wait sits before the input poll, the update and the render, so the time between two presents
// varied by as much as the update and render times vary (no vsync, tearing allowed: every present is shown at
// once). Now the presents are put on a fixed 1/60 s grid: the step waits until the next grid point minus the
// work it expects (input to present), and the present hook spins out the rest (normally a fraction of a
// millisecond). The input is still read just before the work, so the added latency is that small margin.
//
// Above 60 (docs/plan-120fps.md) the grid's period is the running cadence's: 1/60 s, or 1/g_rate s in a race.
//
// The hook is slot 17 (Present) of the D3D9 device's vtable, which lives in d3d9.dll (DXVK or wined3d), not in
// the game. The device is renderer+0x578, renderer = *0xD64708 (the present function 0x50E400 reads it there).
constexpr uintptr_t kRenderer = 0xD64708;
constexpr uint32_t kRendererDevice = 0x578;
constexpr int kPresentSlot = 17;

using PresentFn = long(__stdcall*)(void*, const void*, const void*, void*, const void*);
PresentFn g_present_orig = nullptr;  // the original of the vtable hooked last (also "the hook is in")
uint32_t* g_present_vt = nullptr;
// Every vtable we hooked, with the Present it had then. The original of a vtable is taken once, when we hook it, and
// never again: if another module later wraps the slot over ours, a re-capture would take its wrapper (which ends in
// us) as our original and form a cycle. PresentHook calls the original of the device's own vtable.
struct PresentVt {
  uint32_t* vt;
  PresentFn orig;
};
constexpr int kMaxPresentVts = 8;
PresentVt g_present_vts[kMaxPresentVts];
volatile LONG g_present_nvts = 0;

PresentFn PresentOrigOf(void* dev) {
  const uint32_t* vt = *static_cast<uint32_t* const*>(dev);
  const LONG n = g_present_nvts;
  for (LONG i = 0; i < n; ++i)
    if (g_present_vts[i].vt == vt && g_present_vts[i].orig) return g_present_vts[i].orig;
  return g_present_orig;
}
bool g_pace_present = true;  // frame_pacing = auto (default) | smooth | fastest | vrr | vsync | present | step (the wait before the work only, as before)
bool g_frame_log = false;    // frame_log: a [frames] line every 10 s (default off since 2026-11-13)

// Vsync (frame_pacing = smooth / vsync, or auto). With the game's vsync off the presents are
// shown as they come (IMMEDIATE), and under gamescope (Steam Deck, Bazzite game mode) the display's own clock picks
// which present it scans out. The grid above is QPC time, which is not the display's clock: the two drift by some
// 0.01 to 0.1 %, so the frames' finishing times slide slowly through the display's latch point. While they sit near
// it, the frame-to-frame jitter of the GPU's finish puts two frames into one refresh and none into the next: the
// counter still says 60, but for seconds at a time about 30 different pictures a second reach the screen, until
// the phase has drifted past (on a 90 Hz Deck OLED the 60 frames are also shown for 1 and 2 refreshes in turn).
// With vsync the device presents on the vblank (FIFO: no frame is dropped or torn) and:
//   * the grid runs 0.1 % faster than the cadence, so the swap chain, not QPC, is the clock: when Present blocks
//     (a full queue) the grid goes on from its return;
//   * DXVK keeps at most one frame queued (DXVK_CONFIG d3d9.maxFrameLatency = 1, set at start unless the user set
//     it), so the full queue costs one frame of latency, not three;
//   * the step's tick accumulator leaks 2 % a frame: a display a little slower or faster than 60 Hz gives one tick
//     per present (the game runs at the display's rate, 0.1 % off at most) instead of a double tick every few
//     seconds; a frame really missed still gets its catch-up tick.
volatile unsigned g_dev_interval = 0xFFFFFFFFu;  // the interval the device was created / reset with; all ones: not known

bool Vsynced() {
  const unsigned i = g_dev_interval;
  return i == 0xFFFFFFFFu ? ss_frame_vsync() : i != 0x80000000u;  // D3DPRESENT_INTERVAL_IMMEDIATE
}

// Under gamescope (Steam Deck game mode, Bazzite game mode, a nested gamescope): the compositor owns the display.
bool UnderGamescope() {
  const char* d = getenv("SteamDeck");
  return (d && !strcmp(d, "1")) || getenv("GAMESCOPE_WAYLAND_DISPLAY") || getenv("SteamGamepadUI");
}

// frame_pacing (the overlay's SCREEN SYNC): smooth (= vsync), vrr -> yes; fastest, present, step -> no; auto (or none,
// or a value not known) -> smooth, on every screen and at every frame rate (owner's choice 2026-11-13, until SSPC-32
// can tell a variable-refresh screen; then auto may pick vrr there). The test aid SS_PATCH_MODE keeps the old no.
bool VsyncFor(const std::string& v) {
  if (v == "smooth" || v == "vsync" || v == "vrr") return true;
  if (v == "fastest" || v == "fast" || v == "present" || v == "step") return false;
  return !getenv("SS_PATCH_MODE");
}
// frame_pacing = fastest: the grid of present, with the device made with interval IMMEDIATE whatever the game's own
// vsync option says (display.cpp), so a frame is shown the moment it is ready: no wait, may tear.
bool FastestFor(const std::string& v) { return v == "fastest" || v == "fast"; }
bool g_fastest = false;
// frame_pacing = vrr (SSPC-32): for a display with variable refresh (G-Sync, FreeSync, VESA adaptive sync) that is
// active for the game. The device presents on the vblank as with vsync (FIFO: no tearing), but a VRR display starts
// its refresh when a frame arrives, so the vblank is wherever the frame is: a frame late by 1 ms is shown 1 ms late,
// where a fixed 120 Hz display shows the previous picture for a whole extra refresh (8.3 ms). For that the presents
// must stay below the display's top rate (else FIFO waits for the fixed vblank again, with a frame of latency): the
// race rate is capped under the display mode's refresh (VrrCap: 116 at 120 Hz), and the grid runs on QPC time at exactly
// the cadence (no 0.1 % fast grid, no accumulator leak: the display follows the frames, not the other way round).
// Late frames are presented at once and the next one keeps the grid, as in every mode, so the game's 1/fps ticks
// stay in step with the wall clock. On a display without VRR (or VRR not active) this judders: 116 frames on 120
// refreshes repeat one picture four times a second. Decided once, at start (ss_frame_vsync).
bool g_vrr = false;
volatile int64_t g_next_present = 0;  // the grid point the next present aims at (QPC)
volatile int64_t g_last_present = 0;  // when the hook last ran (QPC)
double g_work_est = 0.004;            // expected seconds from the step's wait to the present
int64_t g_work_t0 = 0;                // when the step's wait ended (QPC)

struct FrameStats {
  int64_t t0 = 0, prev = 0;
  uint32_t n = 0, late = 0, missed = 0, held = 0;
  double sum = 0, sum2 = 0, mn = 1e9, mx = 0, spin = 0;
  uint32_t blocked = 0;  // vsync: presents that waited over 1 ms in the real Present (a full queue)
  double block = 0;      // vsync: seconds spent in the real Present
  double in_base = 0, in_race = 0;  // seconds of the window in each cadence
  // Buckets of the present interval, for the 1% low: 0.5 ms at 60, the same fraction of the period at g_rate
  // (0.25 ms at 120). 64 of them at 60 (32 ms); twice as many above, so a frame of the 60 cadence still has a bucket.
  uint32_t hist[128] = {};
  // The frame split (SSPC-17), sums and maxima in seconds. Per step: the step's wait, the game's update, the game's
  // render without the present hook in it. Per present: our present hook's own work without the pacing spin, the
  // real Present, and the work time the estimate is made from, before its cap.
  struct Part {
    double sum = 0, mx = 0;
    void Add(double v) {
      sum += v;
      if (v > mx) mx = v;
    }
  };
  Part wait, upd, rnd, hook, pres, work;
  // Per step: the time between the last step's return and this step (the game's main loop: its per-frame call
  // 0xA15020 and its Sleep(1)), and the part of it spent in that Sleep(1); how many of those sleeps were left out.
  Part out, loop_sleep;
  Part net;  // the game's InternetGetConnectedState calls that reached the real function on the calling thread
  uint32_t net_calls = 0, net_cached = 0;
  uint32_t sleep_skipped = 0;
  uint32_t steps = 0, presents = 0;
  unsigned long vq0 = 0, vp0 = 0;  // ss_vq_calls / ss_vp_calls when the window began
} g_fs;
double g_present_hook_s = 0;  // seconds the present hook took in all (spin and real Present included) since the step read it

// The [frames] split line, beside the pacing line: where a frame's time goes (avg / max in ms), and the system calls
// per present that cost the most for their size (VirtualQuery, VirtualProtect).
void FrameSplitLog() {
  const double ns = g_fs.steps ? 1000.0 / g_fs.steps : 0, np = g_fs.presents ? 1000.0 / g_fs.presents : 0;
  const double per = g_fs.presents ? 1.0 / g_fs.presents : 0;
  Log("[frames] split (avg / max ms): step wait %.2f / %.2f, update %.2f / %.2f, render %.2f / %.2f, present hook %.2f / %.2f, "
      "real Present %.2f / %.2f; work before the cap %.2f / %.2f; outside the step %.2f / %.2f, of it the game loop's "
      "Sleep(1) %.2f / %.2f (%u left out: loop_sleep) and its InternetGetConnectedState %.2f / %.2f (%u real calls, %u answered "
      "from the last poll: net_poll); VirtualQuery %.1f a present, VirtualProtect %.1f a present "
      "(%u steps, %u presents)",
      g_fs.wait.sum * ns, g_fs.wait.mx * 1000, g_fs.upd.sum * ns, g_fs.upd.mx * 1000, g_fs.rnd.sum * ns, g_fs.rnd.mx * 1000,
      g_fs.hook.sum * np, g_fs.hook.mx * 1000, g_fs.pres.sum * np, g_fs.pres.mx * 1000, g_fs.work.sum * np, g_fs.work.mx * 1000,
      g_fs.out.sum * ns, g_fs.out.mx * 1000, g_fs.loop_sleep.sum * ns, g_fs.loop_sleep.mx * 1000, g_fs.sleep_skipped,
      g_fs.net.sum * ns, g_fs.net.mx * 1000, g_fs.net_calls, g_fs.net_cached,
      double(ss_vq_calls - g_fs.vq0) * per, double(ss_vp_calls - g_fs.vp0) * per, g_fs.steps, g_fs.presents);
}

// `rate`: the cadence the interval that ends now was paced at (the late / missed thresholds are in its periods).
void FrameStatsAdd(int64_t t, double spun, double rate) {
  if (!g_frame_log) return;
  if (!g_fs.t0) { g_fs.t0 = g_fs.prev = t; g_fs.vq0 = ss_vq_calls; g_fs.vp0 = ss_vp_calls; return; }
  const double d = Secs(t - g_fs.prev);
  g_fs.prev = t;
  ++g_fs.n;
  g_fs.sum += d;
  g_fs.sum2 += d * d;
  if (d < g_fs.mn) g_fs.mn = d;
  if (d > g_fs.mx) g_fs.mx = d;
  g_fs.spin += spun;
  if (d > 1.25 / rate) ++g_fs.late;
  if (d > 1.75 / rate) ++g_fs.missed;
  (rate == 60.0 ? g_fs.in_base : g_fs.in_race) += d;
  const double scale = g_rate / 60.0;  // 1 at 60
  const int top = g_rate == 60.0 ? 63 : 127;
  int b = int(d * 2000.0 * scale);
  g_fs.hist[b < 0 ? 0 : b > top ? top : b]++;
  const double s = Secs(t - g_fs.t0);
  if (s < 10.0) return;
  const double avg = g_fs.sum / g_fs.n;
  double var = g_fs.sum2 / g_fs.n - avg * avg;
  if (var < 0) var = 0;
  uint32_t want = g_fs.n / 100 + 1, acc = 0;  // the slowest 1 % of the intervals
  int b1 = top;
  for (; b1 > 0; --b1) {
    acc += g_fs.hist[b1];
    if (acc >= want) break;
  }
  char more[200] = "", vs[120] = "";
  if (Vsynced())
    snprintf(vs, sizeof vs, "; vsync: in the real Present %.2f ms avg, %u presents waited for a free slot", g_fs.block / g_fs.n * 1000,
             g_fs.blocked);
  if (g_rate != 60.0)
    snprintf(more, sizeof more, "; target %.4g fps: %.1f s at %.4g, %.1f s at 60 (late frames counted in periods of the cadence)",
             g_rate, g_fs.in_race, g_rate, g_fs.in_base);
  Log("[frames] %s pacing: %.1f fps, interval %.2f ms avg, %.2f sd, %.2f min, %.2f max; 1%% low %.1f fps; "
      ">1.25 frames %u, >1.75 frames %u; slow-motion frames kept %u; spun at the present %.2f ms avg, work estimate %.2f ms%s%s",
      !g_pace_present ? "step" : g_vrr ? "vrr" : Vsynced() ? "vsync" : "present", g_fs.n / s, avg * 1000, sqrt(var) * 1000, g_fs.mn * 1000, g_fs.mx * 1000,
      1000.0 / ((b1 * 0.5 + 0.25) / scale), g_fs.late, g_fs.missed, g_fs.held, g_fs.spin / g_fs.n * 1000, g_work_est * 1000, vs, more);
  FrameSplitLog();
  g_fs = FrameStats();
  g_fs.t0 = g_fs.prev = t;
  g_fs.vq0 = ss_vq_calls;
  g_fs.vp0 = ss_vp_calls;
}

// A held slow-motion frame keeps the last picture for one more period: counted as a frame on time.
void FrameStatsHeld(int64_t t, double rate) {
  if (!g_frame_log) return;
  ++g_fs.held;
  FrameStatsAdd(t, 0, rate);
}

// One period of the cadence now running, in QPC units.
int64_t PeriodQpc() {
  const double rate = CadRate();
  return rate == 60.0 ? g_freq / 60 : int64_t(double(g_freq) / rate);
}

// True while the present hook is running the presents (it ran in the last 100 ms).
bool PresentPaced() {
  return g_pace_present && g_present_orig && g_freq && g_last_present && Secs(Now() - g_last_present) < 0.1;
}

// ---------------------------------------------------------- the game loop's Sleep(1)
//
// The game's main loop (0x558760) is `do { next = step(state); Sleep(1); } while (...)`: after every frame the main
// thread sleeps "1 ms" through the import slot 0xB9B120 (kernel32 Sleep; the wrapper 0x5A4A40 calls it). The game's
// own step had no other wait when it was behind, so this was its yield. With our step the wait before the work
// (WaitUntil, itself a Sleep for the whole milliseconds) is the frame's rest, and this second sleep only takes time
// from the frame: on the Steam Deck (Proton) some 5 ms of every 16.7 went outside the step, so a frame whose work
// passed about 11 ms was late although neither the CPU nor the GPU was busy. loop_sleep = auto (the default) leaves
// that sleep out while the present hook paces the frames; game keeps it. Either way the [frames] split line says
// how long the main loop took outside the step and how much of it was this sleep.
// The slot is in the import table (.rdata, filled by the loader/protector), not in the code; it is redirected the
// way the other import and vtable slots are (ss_write_word), only when it holds kernel32's Sleep.
constexpr uintptr_t kIatSleep = 0xB9B120;
using SleepFn = void(WINAPI*)(DWORD);
SleepFn g_sleep_orig = nullptr;
volatile DWORD g_main_tid = 0;       // the thread our step runs on
bool g_loop_sleep_skip = true;       // loop_sleep = auto (true) | game (false)
volatile DWORD g_loop_tid = 0;       // the main thread, while it is between two of our steps (else 0)
int64_t g_step_exit = 0;             // when our step last returned (QPC; main thread only)

void WINAPI __attribute__((force_align_arg_pointer)) LoopSleepHook(DWORD ms) {
  if (ms != 1 || !g_loop_tid || GetCurrentThreadId() != g_loop_tid) {
    g_sleep_orig(ms);
    return;
  }
  g_loop_tid = 0;  // the first Sleep(1) of the main thread after the step: the main loop's
  if (g_loop_sleep_skip && PresentPaced()) {
    ++g_fs.sleep_skipped;
    return;
  }
  const int64_t t0 = Now();
  g_sleep_orig(ms);
  if (g_frame_log) g_fs.loop_sleep.Add(Secs(Now() - t0));
}

// ---------------------------------------------------------- the game loop's network state poll
//
// Before every step the main loop's dispatcher (0x558810) calls 0xA15020 -> 0xA12480 -> 0xA12360, which asks
// wininet's InternetGetConnectedState(&flags, 0) (import slot 0xB9B2CC) and keeps three bytes of the answer
// (connected, and modem / proxy / RAS bits) at the app's +0x1F9..+0x1FB: once a frame, on the main thread. On
// Windows that is a cheap read; under Wine / Proton the function walks the network adapters on every call, and on
// the Steam Deck it took about 4.3 ms of every 16.7 ms frame (the "outside the step" time that stayed when the
// loop's Sleep(1) was left out). net_poll = auto (the default): a thread of ours asks the real function every 10 s
// and the game's call gets the last answer (same values, at most 2 s old); game: the game's call goes through.
// The slot is in the import table (.rdata), redirected only when it holds wininet's function.
constexpr uintptr_t kIatNetState = 0xB9B2CC;
using NetStateFn = BOOL(WINAPI*)(DWORD*, DWORD);
NetStateFn g_net_orig = nullptr;
bool g_net_cache = true;  // net_poll = auto (true) | game (false)
volatile LONG g_net_have = 0, g_net_ok = 0, g_net_flags = 0;

DWORD WINAPI NetPollThread(LPVOID) {
  for (uint32_t n = 0;; ++n) {
    DWORD flags = 0;
    const int64_t t0 = Now();
    const BOOL ok = g_net_orig(&flags, 0);
    const double ms = g_freq ? Secs(Now() - t0) * 1000 : 0;
    InterlockedExchange(&g_net_flags, static_cast<LONG>(flags));
    InterlockedExchange(&g_net_ok, ok ? 1 : 0);
    InterlockedExchange(&g_net_have, 1);
    if (n < 3) Log("[frames] net_poll: InternetGetConnectedState took %.2f ms on our thread (answer %d, flags 0x%lX)", ms, ok ? 1 : 0, flags);
    Sleep(10000);
  }
  return 0;
}

BOOL WINAPI __attribute__((force_align_arg_pointer)) NetStateHook(DWORD* flags, DWORD reserved) {
  const bool main = g_frame_log && g_mode == MODE_SIXTY && GetCurrentThreadId() == g_main_tid;
  if (g_net_cache && reserved == 0 && g_net_have) {
    if (flags) *flags = static_cast<DWORD>(g_net_flags);
    if (main) ++g_fs.net_cached;
    return g_net_ok ? TRUE : FALSE;
  }
  if (!main) return g_net_orig(flags, reserved);
  const int64_t t0 = Now();
  const BOOL r = g_net_orig(flags, reserved);
  g_fs.net.Add(Secs(Now() - t0));
  ++g_fs.net_calls;
  return r;
}

void InstallNetStateHook() {
  const HMODULE wi = GetModuleHandleA("wininet.dll");
  const uintptr_t real = wi ? reinterpret_cast<uintptr_t>(GetProcAddress(wi, "InternetGetConnectedState")) : 0;
  if (!real || !Readable(kIatNetState, 4) || Rd(kIatNetState) != real) {
    Log("[frames] the game's InternetGetConnectedState import (0x%X) does not hold wininet's function: left alone (net_poll has no effect)",
        static_cast<unsigned>(kIatNetState));
    return;
  }
  g_net_orig = reinterpret_cast<NetStateFn>(real);
  g_main_tid = GetCurrentThreadId();
  if (!ss_write_word(reinterpret_cast<void*>(kIatNetState), reinterpret_cast<uintptr_t>(&NetStateHook))) {
    Log("[frames] the game's InternetGetConnectedState import could not be redirected: left alone");
    return;
  }
  if (g_net_cache) {
    const HANDLE th = CreateThread(nullptr, 0, NetPollThread, nullptr, 0, nullptr);
    if (th) CloseHandle(th);
    else g_net_cache = false;
  }
  Log("[frames] the game's InternetGetConnectedState call before every step: %s (net_poll)",
      g_net_cache ? "answered from our thread's poll every 10 s" : "kept (timed in the split line)");
}

// Once, from the first step at 60 and above (the game runs, so its imports are filled).
void InstallLoopSleepHook() {
  static bool tried = false;
  if (tried) return;
  tried = true;
  InstallNetStateHook();
  const HMODULE k32 = GetModuleHandleA("kernel32.dll");
  const uintptr_t real = k32 ? reinterpret_cast<uintptr_t>(GetProcAddress(k32, "Sleep")) : 0;
  if (!real || !Readable(kIatSleep, 4) || Rd(kIatSleep) != real) {
    Log("[frames] the game loop's Sleep import (0x%X) does not hold kernel32's Sleep: left alone (loop_sleep has no effect, its time is not split out)",
        static_cast<unsigned>(kIatSleep));
    return;
  }
  g_sleep_orig = reinterpret_cast<SleepFn>(real);
  if (!ss_write_word(reinterpret_cast<void*>(kIatSleep), reinterpret_cast<uintptr_t>(&LoopSleepHook))) {
    g_sleep_orig = nullptr;
    Log("[frames] the game loop's Sleep import could not be redirected: left alone");
    return;
  }
  Log("[frames] the game loop's Sleep(1) after every step: %s (loop_sleep)",
      g_loop_sleep_skip ? "left out while the present hook paces the frames" : "kept (timed in the split line)");
}

long __stdcall PresentHook(void* dev, const void* a, const void* b, void* c, const void* d) {
  const int64_t t_entry = g_frame_log && g_mode == MODE_SIXTY ? Now() : 0;  // the [frames] split only
  RegionsForget(false);  // the readable-region cache: the present phase checks again
  ss_quality_present();  // quality.cpp: the knock-over props' draw distance for the next frame
  ss_lodfade_present();  // lodfade.cpp: the frame's render is over, the scenery objects get their own meshes back
  ss_ppicon_present();  // ppicon.cpp: the Power Play icons' scales for this back buffer (each race's HUD once)
  ss_keyrepeat_present();  // keyrepeat.cpp: the arrow keys on the menus' repeat rows (the keyboard's live entry)
  ss_glyphs_present(dev);  // glyphs.cpp: a button_prompts change re-writes the prompt textures
  ss_pipewarm_present(dev);  // pipewarm.cpp: pipeline_warmup's replay at the level load (before the overlay)
  ss_streampool_present();  // streampool.cpp: stream_pool's limit at a level load, the hold at 480 nodes
  ss_extras_present();  // extras.cpp: the unlock switches' bytes and the decal requests (data only)
  ss_menu_present(dev);  // the settings overlay (menu.cpp): drawn over the finished picture while open
  ss_hdr_present(dev);   // HDR's output pass (hdr.cpp; hdr only), after the overlay; and the probe (hdr_probe only)
  ss_diag_present();     // diag.cpp: the first frame starts the [summary] block (one flag test after that)
  ss_testshot(dev);      // test aid, SS_SHOT_AT only: after the overlay, so a capture shows what the screen shows
  const bool sixty = g_mode == MODE_SIXTY && g_freq;
  double spun = 0;
  if (sixty) {
    const double rate = CadRate();
    const int64_t P = PeriodQpc();
    int64_t now = Now();
    if (g_pace_present) {
      if (g_work_t0) {  // how long the work took this frame: the estimate follows the slow frames quickly, recovers slowly
        const double w = Secs(now - g_work_t0);
        if (g_frame_log) g_fs.work.Add(w);
        // The estimate is the 98th percentile of the last 256 frames' work (about 4 s), recomputed every 32
        // frames, so a rare slow frame costs one late present instead of a millisecond of latency on every frame.
        static float ring[256];
        static uint32_t n = 0;
        ring[n++ & 255] = static_cast<float>(w);
        if (w > g_work_est) g_work_est = w;  // a slower stretch starts: follow at once
        if ((n & 31) == 0 && n >= 64) {
          float tmp[256];
          const uint32_t cnt = n < 256 ? n : 256;
          std::copy(ring, ring + cnt, tmp);
          const uint32_t k = cnt * 98 / 100;
          std::nth_element(tmp, tmp + k, tmp + cnt);
          g_work_est = tmp[k];
        }
        const double cap = std::min(0.012, 0.75 / rate);  // 12 ms at 60
        if (g_work_est > cap) g_work_est = cap;
        g_work_t0 = 0;
      }
      int64_t target = g_next_present;
      if (!target || now - target > P) target = now;  // first frame, or more than a frame late: start a new grid
      if (target > now) {
        WaitUntil(target);
        spun = Secs(Now() - now);
      }
      g_next_present = target + (Vsynced() && !g_vrr ? P - P / 1000 : P);  // vsync: a little fast, the swap chain holds it back
      now = Now();
    }
    g_last_present = now;
    FrameStatsAdd(now, spun, rate);
  } else {
    g_next_present = 0;
    g_last_present = 0;
  }
  if (ss_hdr_test_lost()) return static_cast<long>(0x88760868L);  // test aid (SS_HDR_TEST_RESET): D3DERR_DEVICELOST
  const PresentFn orig = PresentOrigOf(dev);
  if (!orig) return static_cast<long>(0x8876086CL);  // D3DERR_INVALIDCALL (cannot happen: reached only through a hooked vtable)
  if (!sixty) return orig(dev, a, b, c, d);
  const bool vsynced = Vsynced();
  if (!vsynced && !g_frame_log) return orig(dev, a, b, c, d);
  const int64_t t0 = Now();
  const long hr = orig(dev, a, b, c, d);
  const int64_t t1 = Now();
  const double held = Secs(t1 - t0);
  if (g_frame_log && held > 0.025) {
    // A single long block inside d3d9.dll's Present (DXVK: its CS thread or the frame-latency wait held up, e.g. by a
    // pipeline compile); timed so it can be matched with DXVK's own log. At most 300 lines a run.
    static int long_presents = 0;
    if (++long_presents <= 300) Log("[frames] real Present blocked %.1f ms", held * 1000);
  }
  if (g_frame_log && t_entry) {
    // The split (every pacing mode): our hook's work without the spin, and the real Present.
    ++g_fs.presents;
    g_fs.hook.Add(Secs(t0 - t_entry) - spun);
    g_fs.pres.Add(held);
    g_present_hook_s += Secs(t1 - t_entry);
  }
  if (!vsynced) return hr;
  if (g_frame_log) {
    g_fs.block += held;
    if (held > 0.001) ++g_fs.blocked;
  }
  // The queue was full: the vblank freed the slot just now. The next present aims one period after it.
  if (held > 0.001 && g_pace_present) {
    const int64_t n = t1 + PeriodQpc() - (g_vrr ? 0 : PeriodQpc() / 1000);
    if (n > g_next_present) g_next_present = n;
  }
  return hr;
}

// Puts the hook into the device's vtable (in d3d9.dll's memory) once the device exists; again if the vtable changes.
void InstallPresentHook() {
  uintptr_t r = Rd(kRenderer);
  if (!r || !Readable(r + kRendererDevice, 4)) return;
  uintptr_t dev = Rd(r + kRendererDevice);
  if (!dev || !Readable(dev, 4)) return;
  uint32_t* vt = reinterpret_cast<uint32_t*>(Rd(dev));
  if (!vt || !Readable(reinterpret_cast<uintptr_t>(vt), (kPresentSlot + 1) * 4)) return;
  const uint32_t mine = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&PresentHook));
  if (vt[kPresentSlot] == mine) return;
  for (LONG i = 0; i < g_present_nvts; ++i) {
    if (g_present_vts[i].vt != vt) continue;
    const uintptr_t cur = vt[kPresentSlot];
    if (cur == reinterpret_cast<uintptr_t>(g_present_vts[i].orig)) {
      // The slot holds the original again: our hook was taken out (Windows' d3d9 keeps the device's function table
      // in the device and can write it back whole). Put it in again.
      static int put = 0;
      const bool ok = ss_write_word(&vt[kPresentSlot], mine);
      if (put++ < 4) Log("[frames] the device's Present slot (vtable %p) held the original again: %s", vt, ok ? "hooked again" : "cannot write it");
      return;
    }
    // Hooked by us before and wrapped since by someone else: their wrapper chains to us; leave it.
    static bool said = false;
    if (!said) {
      HMODULE by = nullptr;
      char name[MAX_PATH] = "?";
      if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<const char*>(cur), &by) && by)
        GetModuleFileNameA(by, name, sizeof name);
      Log("[frames] the device's Present (vtable %p) was wrapped over our hook by another module (%p in %s); left as it is",
          vt, reinterpret_cast<void*>(cur), name);
    }
    said = true;
    return;
  }
  if (g_present_nvts >= kMaxPresentVts) {
    static bool said = false;
    if (!said) Log("[frames] more than %d device vtables seen; the new one is not hooked", kMaxPresentVts);
    said = true;
    return;
  }
  HMODULE owner = nullptr, game = GetModuleHandleA(nullptr);
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<const char*>(vt[kPresentSlot]), &owner) || !owner || owner == game) {
    static bool said = false;
    if (!said) Log("[frames] the device's Present is not in a d3d9 module; present pacing left off");
    said = true;
    return;
  }
  if (g_present_vt && g_present_vt != vt) Log("[frames] the device's vtable changed (%p -> %p): hooked again", g_present_vt, vt);
  // The entry is in place before the slot points at us (a present on another thread finds its original at once).
  const LONG i = g_present_nvts;
  g_present_vts[i].vt = vt;
  g_present_vts[i].orig = reinterpret_cast<PresentFn>(vt[kPresentSlot]);
  MemoryBarrier();
  g_present_nvts = i + 1;
  uintptr_t prev = 0;
  if (!ss_write_word(&vt[kPresentSlot], mine, &prev)) {
    // Not hooked: drop the entry. The previous original stays (an older vtable may still point at PresentHook).
    const DWORD err = GetLastError();
    g_present_nvts = i;
    g_present_vts[i].vt = nullptr;
    static bool said = false;
    if (!said) Log("[frames] cannot write the device vtable (error %lu); present pacing left off", err);
    said = true;
    return;
  }
  if (prev != mine && prev != reinterpret_cast<uintptr_t>(g_present_vts[i].orig))
    g_present_vts[i].orig = reinterpret_cast<PresentFn>(prev);  // changed between the read and the swap: the real one
  g_present_orig = g_present_vts[i].orig;
  g_present_vt = vt;
  char name[MAX_PATH] = "?";
  GetModuleFileNameA(owner, name, sizeof name);
  Log("[frames] Present hooked in %s (device %p, vtable %p); pacing %s, log %s", name, reinterpret_cast<void*>(dev), vt,
      !g_pace_present ? "in the step (as before)"
      : g_vrr         ? "at the present, on a variable-refresh vblank (vrr)"
      : Vsynced()     ? "at the present, on the vblank (vsync)"
                      : "at the present",
      g_frame_log ? "on" : "off");
  DEVMODEA dm = {};
  dm.dmSize = sizeof dm;
  const DWORD hz = EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm) ? dm.dmDisplayFrequency : 0;
  const bool odd = hz > 1 && (hz % 60 > 1 && hz % 60 < 59);
  Log("[frames] the display mode says %lu Hz%s", static_cast<unsigned long>(hz),
      odd ? ": not a multiple of 60, so 60 frames a second cannot be shown evenly (each frame stays 1 or 2 refreshes); "
            "set the display to 60 Hz (Steam Deck: Performance, refresh rate)" : "");
}

// Called once from the first step (the game is running, so its code is decrypted): the first bytes of every
// function we call must be the ones of the known build. Only reads.
bool Sig(uintptr_t va, std::initializer_list<uint8_t> bytes) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(va);
  size_t i = 0;
  for (uint8_t b : bytes)
    if (p[i++] != b) return false;
  return true;
}

void CheckCode() {
  g_sig_checked = true;
  bool core = Sig(kDtFn, {0x83, 0xEC, 0x08, 0x56, 0x68}) && Sig(kPumpFn, {0x83, 0xEC, 0x20, 0x53, 0x8B}) &&
              Sig(kStepReal, {0x83, 0xEC, 0x10, 0x53, 0x55, 0x56, 0x57});
  g_alloc_ok = Sig(kAlloc, {0x56, 0xE8}) && Sig(kFree, {0x57, 0x8B, 0x7C, 0x24, 0x08}) && Sig(kVecCtor, {0x6A, 0x10}) &&
               Sig(kVecDtor, {0x6A, 0x0C}) && Sig(kElemCtor, {0x56, 0x68}) && Sig(kElemDtor, {0x68});
  bool ai = Sig(kAiUpdate, {0x83, 0xEC, 0x10, 0x53, 0x8B, 0x5C, 0x24, 0x18});
  if (!core) {
    g_code_ok = false;
    Log("code signature of the step helpers differs from the known build: pass-through only");
  }
  if (!g_alloc_ok) Log("code signature of the allocator helpers differs: rewind rings left at 180");
  if (!ai) {
    g_ai_on = false;
    Log("code signature of the AI update differs: AI gate off");
  }
  if (Rd(kMaxTicks) != 10) Log("note: max ticks per update is %u (expected 10)", Rd(kMaxTicks));
  Log("code check: core %s, allocator %s, ai %s", core ? "ok" : "BAD", g_alloc_ok ? "ok" : "BAD", ai ? "ok" : "BAD");
}

// The limiter's state (the original keeps it in two globals that only the step reads).
double g_acc = 0;
uint32_t g_behind = 0;
int g_acc_mode = -1;

void StatsReport(uint32_t ticks) {
  g_st.ticks += ticks;
  (ticks == 1 ? g_st.t1 : ticks == 2 ? g_st.t2 : g_st.t3p)++;
  ++g_st.steps;
  int64_t now = Now();
  double s = Secs(now - g_st.t0);
  if (s >= 2.0) {
    Log("[step] mode %d: %.1f updates/s, %.1f ticks/s (60 = real time); ticks 1:%llu 2:%llu 3+:%llu; AI run %.0f/s held %.0f/s; "
        "steer |mean| %.3f, sign flips %.1f/s, call rate %.0f/s; update %.2f ms, outside %.2f ms (waited %.2f ms)",
        g_mode, g_st.steps / s, g_st.ticks / s, (unsigned long long)g_st.t1, (unsigned long long)g_st.t2,
        (unsigned long long)g_st.t3p, g_st.ai_run / s, g_st.ai_hold / s, g_st.ai_calls ? g_st.ai_abs / g_st.ai_calls : 0.0,
        g_st.ai_flips / s, g_st.ai_calls / s, 1000.0 * g_st.upd_s / g_st.steps, 1000.0 * g_st.out_s / g_st.steps,
        1000.0 * g_st.wait_s / g_st.steps);
    uintptr_t m = Rd(kRewindMgr);
    if (m && Readable(m, 0x138))
      Log("[rewind] manager %p ring %p capacity %u first %u count %u", reinterpret_cast<void*>(m),
          reinterpret_cast<void*>(Rd(m + 0x11C)), Rd(m + 0x120), Rd(m + 0x124), Rd(m + 0x128));
    g_st = Stats();
    g_st.t0 = now;
  }
}


// The two slow motions at sixty (takedown_realtime).
//
// 1. The cameras' slow motion. The in-game controller's update(ticks) 0x4B4F90 (InGame::cInGameController, vptr
//    0xBB642C, the run state's +0x10 = 0xD66B24; called by the run state's update 0x417940 at 0x4179B5) decides at
//    0x4B4FE7..0x4B5043, before anything else uses the count, to run the update with ONE tick whatever the step
//    counted. Then it passes that count to the race update 0x4B2670 (0x4B5575). It does so when, with race =
//    controller+0x14 (the object at 0xD66B28):
//      * the race's settings (race+0x14) say one local player (+0x9C == 1), and
//      * the mode word of the object at 0xD64E18 (+8) is neither 2 nor 3, and
//      * the byte +0x2C of the camera output (race+0x30, the output array the camera manager 0xA7EB00 is updated
//        with at 0x4C22CB) is set, or the byte +0x2A0 of the object at race+0xCC is.
//    The crash and takedown shots set the output's +0x2C from their "Slow_Motion_Criteria" window (0xA9B78D /
//    0xA9B795, entry 0xA9B686, exit 0xA9B514). At thirty an update holds two ticks, so one tick is half speed; at
//    sixty an update holds one tick already and the slow motion vanished.
// 2. The replay's slow motion: a slow-motion rewind playback (manager +0x100 and +0x108). The manager's update gives
//    the loop ONE tick per update whatever the record holds, so at sixty the replay ran at double speed.
//
// One decision per update, as the port's SS_REPLAY_SLOW_HOLD: in a playback only the replay's slow motion counts
// (the rewind update ignores the update's ticks there), outside one only the cameras' one-tick request. A slowed
// update is held when the one before it was not, so never two in a row: one tick per 1/30 s, as at thirty. A held
// update does not call the game's update at all (the frame is rendered again), so nothing is ticked and the rewind
// recorder records nothing for it (no record of zero ticks). Everything here only reads the game's data, every
// pointer is checked, and the inlined decision's code bytes are compared with the known build first: on any mismatch
// the answer is "not slowed", i.e. no hold.

constexpr uintptr_t kController = 0xD66B24;  // cInGameRunState+0x10: InGame::cInGameController
constexpr uint32_t kControllerVt = 0xBB642C;
constexpr uintptr_t kModeObj = 0xD64E18;
constexpr uintptr_t kOneTickCode = 0x4B4FE7;  // the inlined decision in 0x4B4F90
const uint8_t kOneTickSig[] = {
    0x8B, 0x0D, 0x18, 0x4E, 0xD6, 0x00, 0x8B, 0x41, 0x08, 0x8B, 0x7C, 0x24, 0x40, 0x83, 0xC4, 0x04, 0x83, 0xF8, 0x03,
    0x89, 0x7C, 0x24, 0x18, 0x74, 0x09, 0x83, 0xF8, 0x02, 0x74, 0x04, 0x32, 0xD2, 0xEB, 0x02, 0xB2, 0x01, 0x8B, 0x46,
    0x14, 0x8B, 0x48, 0x14, 0x39, 0xA9, 0x9C, 0x00, 0x00, 0x00, 0x75, 0x2A, 0x84, 0xD2, 0x75, 0x26, 0x8B, 0x50, 0x30,
    0x80, 0x7A, 0x2C, 0x00, 0x75, 0x17, 0x39, 0xA9, 0x9C, 0x00, 0x00, 0x00, 0x75, 0x15, 0x8B, 0x88, 0xCC, 0x00, 0x00,
    0x00, 0x80, 0xB9, 0xA0, 0x02, 0x00, 0x00, 0x00, 0x74, 0x06, 0x8B, 0xFD, 0x89, 0x6C, 0x24, 0x18};
// The run state's update calls the controller's update: mov ecx,[esi+0x10]; call 0x4B4F90.
const uint8_t kOneTickCallSig[] = {0x8B, 0x4E, 0x10, 0xE8, 0xD6, 0xD5, 0x09, 0x00};
constexpr uintptr_t kOneTickCall = 0x4179B2;

int g_one_tick_ok = -1;  // -1 not checked yet, 0 the code differs (never slowed), 1 known build

bool SigBytes(uintptr_t va, const uint8_t* b, size_t n) {
  return Readable(va, n) && memcmp(reinterpret_cast<const void*>(va), b, n) == 0;
}

// The controller update's own question, asked first from the same data (only reads). False whenever anything does
// not look like the known build.
bool OneTickRequested() {
  if (g_one_tick_ok < 0) {
    g_one_tick_ok = SigBytes(kOneTickCode, kOneTickSig, sizeof kOneTickSig) &&
                    SigBytes(kOneTickCall, kOneTickCallSig, sizeof kOneTickCallSig);
    Log("[takedown] one-tick request code %s", g_one_tick_ok ? "ok" : "differs from the known build: camera slow motion not followed");
  }
  if (!g_one_tick_ok) return false;
  const uintptr_t c = Rd(kController);
  if (!c || !Readable(c, 0x18) || Rd(c) != kControllerVt) return false;
  const uintptr_t race = Rd(c + 0x14);
  if (!race || !Readable(race, 0xD0)) return false;
  const uintptr_t ai = Rd(race + 0xBC);  // the race's AI manager: proves this is the race object
  if (!ai || !Readable(ai, 4) || Rd(ai) != kAiMgrVt) return false;
  const uintptr_t settings = Rd(race + 0x14);
  if (!settings || !Readable(settings + 0x9C, 4) || Rd(settings + 0x9C) != 1) return false;
  const uintptr_t mode = Rd(kModeObj);
  if (!mode || !Readable(mode + 8, 4)) return false;
  const uint32_t m = Rd(mode + 8);
  if (m == 2 || m == 3) return false;
  const uintptr_t out = Rd(race + 0x30);
  if (out && Readable(out + 0x2C, 1) && *reinterpret_cast<const volatile uint8_t*>(out + 0x2C)) return true;
  const uintptr_t o = Rd(race + 0xCC);
  return o && Readable(o + 0x2A0, 1) && *reinterpret_cast<const volatile uint8_t*>(o + 0x2A0);
}

// ------------------------------------------------------------ half steps
//
// Holding every second slowed update keeps the speed, but the world then moves in 30 Hz steps, each picture shown
// for two periods. Observed in the decrypted image: the race update 0x4B2670 (this = the in-game controller) passes
// the tick's time (1/60 s) and the update's (ticks x 1/60 s) through 0x4AF810 (called once, at 0x4B2730) before
// anything uses them, and that function multiplies both by a factor (0x4AF921 / 0x4AF931): 1.0, or, while the
// controller's step byte +0x1A745 is clear, the debug tweakables "Physics/Change Phys dt/Slow motion activated"
// (byte 0xD71208) -> "Slow Motion cheat speed" (float 0xD113B8, 0.05) and "Fast motion activated" (0xD71210) ->
// 0xD113C0. Everything in the tick loop takes the scaled time: the physics world (0x53A390 -> 0x673E80 stores it),
// the camera director ([ctrl+0x1A680]+0x14 at 0x4B2DA3), the camera controller (ctrl+0x370, +0x14 per tick at
// 0x4B31B5), the race timers (0x4B06F0) and the systems after the loop. So at sixty a slowed update is run, not held,
// with its one tick at half the time: for that one update the slow-motion tweakable is switched on with a factor of
// 0.5 (two .data values, put back right after the update). Sixty different pictures a second, each half a tick on,
// the same game time a wall second as at thirty. The game clock ctrl+0x1A740 (+= ticks x 1/60 at 0x4B5083, before
// the race update, not scaled) is put back by half a tick first.
//
// A slowed playback (the takedown's replay) gives the tick loop one tick an update and a record is left when the
// manager's ticks consumed (+0x130, +1 a tick at 0x4B2E2D -> 0x7BD820) equal the record's ticks (+8 of the 12 byte
// entry): checked at the start of the next update (0x7BDBC0). At sixty the records hold one tick. Half steps play
// each such record over two updates, as a record of two ticks plays at thirty:
//   * the first ("keep"): before the update the record about to be played gets 2 ticks instead of 1, so the update
//     leaves it under way (the post-loop query 0x7BDD90 at 0x4B31C8 answers 0 = not complete) and the next update
//     does not leave it. The two playback players that read a ring of their own, one entry a call (moving objects:
//     race+0x1AC -> 0x7CCDC0, cursor +0x24; Power Play states: race+0x208 -> 0x7C9760 -> 0x7C8810, cursor +0x40),
//     must not move on in this half: race+0x208 is set to 0 for the update (the call at 0x4B2F1D is skipped on 0) and
//     race+0x1AC points at a zeroed stand-in (0x7CCDC0 then reads it, finds nothing to play and writes nothing).
//     The other players follow the manager's tick position (0x7BDCE0), not a cursor of their own.
//   * the second ("play"): the update runs as it is (the players set the record's entry; the post-loop query answers
//     2 complete ticks, x the halved tick time = the record's 1/60 s). After it the record gets its 1 tick back and
//     the ticks consumed 1, so the next update leaves it as the game does.
// Every pointer is checked, every byte of the code the reasoning depends on is compared with the known build first,
// and on anything unexpected the update falls back to the hold above. Only .data and heap values are written.

constexpr uintptr_t kSlowCheatOn = 0xD71208;     // byte: "Slow motion activated"
constexpr uintptr_t kSlowCheatSpeed = 0xD113B8;  // float: "Slow Motion cheat speed" (0.05)
constexpr uintptr_t kFastCheatOn = 0xD71210;     // byte: "Fast motion activated"
constexpr uintptr_t kTicksOverride = 0xD71238;   // byte: a debug tick count override (0x4B5043)
constexpr uintptr_t kRewindDebug = 0xD82664;     // byte: "RewindTime/ManualToggleEnabled" (0x7BDF30 rewrites +0x10C)
constexpr uint32_t kCtrlStepByte = 0x1A745, kCtrlClock = 0x1A740;
constexpr uint32_t kRaceRewind = 0x1A8, kRaceMovers = 0x1AC, kRacePowerPlays = 0x208;

// The time scale's factor choice and multiplications (0x4AF8ED..0x4AF939).
const uint8_t kScaleSig[] = {
    0x80, 0x3D, 0x08, 0x12, 0xD7, 0x00, 0x00, 0x74, 0x0A, 0xF3, 0x0F, 0x10, 0x05, 0xB8, 0x13, 0xD1, 0x00, 0xEB, 0x19,
    0x80, 0x3D, 0x10, 0x12, 0xD7, 0x00, 0x00, 0x74, 0x0A, 0xF3, 0x0F, 0x10, 0x05, 0xC0, 0x13, 0xD1, 0x00, 0xEB, 0x06,
    0xF3, 0x0F, 0x10, 0x44, 0x24, 0x08, 0x8B, 0x44, 0x24, 0x14, 0xF3, 0x0F, 0x10, 0x08, 0xF3, 0x0F, 0x59, 0xC8, 0xF3,
    0x0F, 0x11, 0x08, 0x8B, 0x44, 0x24, 0x18, 0xF3, 0x0F, 0x10, 0x08, 0xF3, 0x0F, 0x59, 0xC8, 0xF3, 0x0F, 0x11, 0x08};
// Its one call in the race update, with the two times it scales (0x4B2700..0x4B2735).
const uint8_t kScaleCallSig[] = {
    0xF3, 0x0F, 0x10, 0x45, 0x0C, 0x8B, 0xF0, 0x8D, 0x44, 0x24, 0x10, 0x50, 0x8D, 0x4C, 0x24, 0x40, 0x51, 0x8D,
    0x54, 0x24, 0x50, 0xF3, 0x0F, 0x2A, 0xCE, 0xF3, 0x0F, 0x59, 0xC8, 0x52, 0x8B, 0xCB, 0x89, 0x74, 0x24, 0x54,
    0xF3, 0x0F, 0x11, 0x44, 0x24, 0x48, 0xF3, 0x0F, 0x11, 0x4C, 0x24, 0x1C, 0xE8, 0xDB, 0xD0, 0xFF, 0xFF};
// The game clock: ctrl+0x1A740 += ticks x 1/60 (0x4B5065..0x4B5097).
const uint8_t kClockSig[] = {
    0x8A, 0x15, 0x5C, 0x13, 0xD1, 0x00, 0x8B, 0x8E, 0x54, 0xA7, 0x01, 0x00, 0xF3, 0x0F, 0x2A, 0xC7, 0xF3,
    0x0F, 0x59, 0x05, 0x20, 0x0D, 0xBA, 0x00, 0xF3, 0x0F, 0x11, 0x44, 0x24, 0x14, 0xF3, 0x0F, 0x58, 0x86,
    0x40, 0xA7, 0x01, 0x00, 0x88, 0x54, 0x24, 0x11, 0xF3, 0x0F, 0x11, 0x86, 0x40, 0xA7, 0x01, 0x00};
// The playback players in the tick loop (0x4B2EBA..0x4B2F22) and the ticks-consumed increment (0x4B2E24).
const uint8_t kPlayersSig[] = {
    0x8B, 0x88, 0xA8, 0x01, 0x00, 0x00, 0xE8, 0xFB, 0xA8, 0x30, 0x00, 0x83, 0xF8, 0x01, 0x75, 0x58, 0x8B, 0x53,
    0x14, 0x8B, 0x8A, 0xAC, 0x01, 0x00, 0x00, 0x8D, 0x44, 0x24, 0x44, 0x50, 0xE8, 0xE3, 0x9E, 0x31, 0x00, 0x8B,
    0x4B, 0x14, 0x8B, 0x89, 0xB0, 0x01, 0x00, 0x00, 0xE8, 0x15, 0x9B, 0x31, 0x00, 0x8B, 0x53, 0x14, 0x8B, 0x8A,
    0xB4, 0x01, 0x00, 0x00, 0xE8, 0x77, 0x92, 0x31, 0x00, 0x8B, 0x43, 0x14, 0x8B, 0x90, 0x58, 0x01, 0x00, 0x00,
    0x8B, 0x80, 0xBC, 0x01, 0x00, 0x00, 0x52, 0x8B, 0xC8, 0xE8, 0xA0, 0x79, 0x31, 0x00, 0x8B, 0x43, 0x14, 0x8B,
    0x88, 0x08, 0x02, 0x00, 0x00, 0x85, 0xC9, 0x74, 0x05, 0xE8, 0x3E, 0x68, 0x31, 0x00};
const uint8_t kConsumeSig[] = {0x8B, 0x4B, 0x14, 0x8B, 0x89, 0xA8, 0x01, 0x00, 0x00, 0xE8, 0xEE, 0xA9, 0x30, 0x00};
const uint8_t kConsumeFnSig[] = {0x83, 0x81, 0x30, 0x01, 0x00, 0x00, 0x01, 0xC3};  // 0x7BD820
// The rewind manager's update (0x7BDEA0..0x7BDF26) and the playing record's step (0x7BDBC0..0x7BDC24).
const uint8_t kRewindUpdateSig[] = {
    0x56, 0x8B, 0xF1, 0x80, 0xBE, 0x10, 0x01, 0x00, 0x00, 0x00, 0x57, 0x8B, 0x7C, 0x24, 0x0C, 0x74, 0x2F, 0x8B, 0x8E,
    0x00, 0x01, 0x00, 0x00, 0x8B, 0x86, 0x14, 0x01, 0x00, 0x00, 0x85, 0xC0, 0x51, 0x89, 0x86, 0x00, 0x01, 0x00, 0x00,
    0xC6, 0x86, 0x10, 0x01, 0x00, 0x00, 0x00, 0x8B, 0xCE, 0x57, 0x75, 0x07, 0xE8, 0x59, 0xFA, 0xFF, 0xFF, 0xEB, 0x1D,
    0xE8, 0x42, 0xFB, 0xFF, 0xFF, 0xEB, 0x16, 0x83, 0xBE, 0x00, 0x01, 0x00, 0x00, 0x00, 0x57, 0x75, 0x07, 0xE8, 0x21,
    0xFC, 0xFF, 0xFF, 0xEB, 0x05, 0xE8, 0xCA, 0xFC, 0xFF, 0xFF, 0x83, 0xBE, 0x00, 0x01, 0x00, 0x00, 0x00, 0x75, 0x07,
    0x8B, 0xC7, 0x5F, 0x5E, 0xC2, 0x04, 0x00, 0x83, 0xBE, 0x08, 0x01, 0x00, 0x00, 0x00, 0x75, 0x0F, 0x8B, 0xCE, 0xE8,
    0x6A, 0xFD, 0xFF, 0xFF, 0x8B, 0x40, 0x08, 0x5F, 0x5E, 0xC2, 0x04, 0x00, 0x5F, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x5E,
    0xC2};
const uint8_t kRewindStepSig[] = {
    0x56, 0x8B, 0xF1, 0x8B, 0x8E, 0x2C, 0x01, 0x00, 0x00, 0x8B, 0x86, 0x24, 0x01, 0x00, 0x00, 0x03, 0xC1, 0x99, 0xF7,
    0xBE, 0x20, 0x01, 0x00, 0x00, 0x57, 0x8B, 0xBE, 0x30, 0x01, 0x00, 0x00, 0x8D, 0x04, 0x52, 0x8B, 0x96, 0x1C, 0x01,
    0x00, 0x00, 0x3B, 0x7C, 0x82, 0x08, 0x0F, 0x85, 0x85, 0x00, 0x00, 0x00, 0x8B, 0x86, 0x0C, 0x01, 0x00, 0x00, 0xC7,
    0x86, 0x30, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x89, 0x86, 0x08, 0x01, 0x00, 0x00, 0x8B, 0x96, 0x28, 0x01,
    0x00, 0x00, 0x83, 0xEA, 0x01, 0x3B, 0xCA, 0x74, 0x0E, 0x83, 0xC1, 0x01, 0x5F, 0x89, 0x8E, 0x2C, 0x01, 0x00, 0x00,
    0x5E, 0xC2, 0x04, 0x00, 0x83};
// The "record complete" query (0x7BDD90) and its post-loop call (0x4B31C4..0x4B31FF: the answer x the tick time).
const uint8_t kCompleteSig[] = {
    0x56, 0x8B, 0xF1, 0x83, 0xBE, 0x00, 0x01, 0x00, 0x00, 0x00, 0x74, 0x22, 0x83, 0xBE, 0x08, 0x01,
    0x00, 0x00, 0x00, 0x74, 0x19, 0x83, 0xBE, 0x30, 0x01, 0x00, 0x00, 0x00, 0x74, 0x10, 0xE8, 0xCD,
    0xFE, 0xFF, 0xFF, 0x8B, 0x8E, 0x30, 0x01, 0x00, 0x00, 0x3B, 0x48, 0x08, 0x75, 0x0C, 0x8B, 0xCE,
    0xE8, 0xBB, 0xFE, 0xFF, 0xFF, 0x8B, 0x40, 0x08, 0x5E, 0xC3, 0x33, 0xC0, 0x5E, 0xC3};
const uint8_t kCompleteCallSig[] = {
    0x8B, 0x4C, 0x24, 0x24, 0xE8, 0xC3, 0xAB, 0x30, 0x00, 0x85, 0xC0, 0x8B, 0x53, 0x14, 0x8B, 0x52, 0x50, 0x8B, 0x0D, 0x58,
    0xC2, 0xD6, 0x00, 0x89, 0x54, 0x24, 0x24, 0x8A, 0x51, 0x50, 0x8B, 0x74, 0x24, 0x24, 0x88, 0x96, 0x88, 0x00, 0x00, 0x00,
    0x8B, 0x53, 0x14, 0x8B, 0x52, 0x50, 0x8D, 0x71, 0x30, 0xF3, 0x0F, 0x2A, 0xC0, 0xF3, 0x0F, 0x59, 0x44, 0x24, 0x3C};
// The moving objects' player 0x7CCDC0: its head, the branch the stand-in takes (0x7CCF0F) and its cursor (0x7CD02D);
// the event part it calls (0x7CCB20) returns at once when +0x28 >= +0x40.
const uint8_t kMoversSig[] = {
    0x83, 0xEC, 0x28, 0x53, 0x55, 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x1C, 0x03, 0x46, 0x24, 0x57, 0x99, 0xF7,
    0x7E, 0x18, 0x33, 0xED, 0x8D, 0x44, 0x24, 0x10, 0x55, 0x50, 0x89, 0x6C, 0x24, 0x18, 0x8B, 0xFA, 0xC1,
    0xE7, 0x0A, 0x03, 0x7E, 0x14, 0xE8, 0x74, 0x09, 0xD7, 0xFF, 0x8B, 0x4C, 0x24, 0x18, 0x83, 0xC4, 0x08,
    0x80, 0x7E, 0x11, 0x00, 0x89, 0x4C, 0x24, 0x14, 0x0F, 0x84, 0x17, 0x01, 0x00, 0x00};
const uint8_t kMoversMidSig[] = {0xC6, 0x46, 0x11, 0x00, 0xE9, 0x15, 0x01, 0x00, 0x00, 0x8B,
                                 0xCE, 0xE8, 0x01, 0xFC, 0xFF, 0xFF, 0x39, 0x6E, 0x0C};
const uint8_t kMoversTailSig[] = {0x8D, 0x54, 0x24, 0x10, 0x6A, 0x00, 0x52, 0xE8, 0x47, 0x07, 0xD7, 0xFF, 0x8B, 0x4E,
                                  0x20, 0x8B, 0x46, 0x24, 0x83, 0xE9, 0x01, 0x83, 0xC4, 0x08, 0x3B, 0xC1, 0x7D, 0x06,
                                  0x83, 0xC0, 0x01, 0x89, 0x46, 0x24, 0x5F, 0x5E, 0x5D, 0x5B, 0x83, 0xC4, 0x28, 0xC2};
const uint8_t kMoverEventsSig[] = {0x53, 0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x0D, 0x64, 0x12, 0xD7, 0x00, 0xE8, 0xB0, 0x11, 0xFF,
                                   0xFF, 0x8B, 0xD8, 0x8B, 0x4F, 0x28, 0x3B, 0x4F, 0x40, 0x0F, 0x8D, 0xA4, 0x00, 0x00, 0x00};
// The Power Plays' player (0x7C9760 -> 0x7C8810) and its cursor at the end of 0x7C8810 (0x7C8D53).
const uint8_t kPowerPlaysSig[] = {0x80, 0x79, 0x3C, 0x00, 0x74, 0x05, 0xE9, 0x25, 0xE7, 0xFF, 0xFF, 0xE9, 0xA0, 0xF0, 0xFF, 0xFF};
const uint8_t kPowerPlaysTailSig[] = {0x8B, 0x46, 0x40, 0x83, 0xE9, 0x01, 0x83, 0xC4, 0x08, 0x3B, 0xC1, 0xC6, 0x46, 0x4C, 0x00,
                                      0x7D, 0x06, 0x83, 0xC0, 0x01, 0x89, 0x46, 0x40, 0x5F, 0x5E, 0x5B, 0x8B, 0xE5, 0x5D, 0xC3};

// slow_half_steps / SS_SLOW_HALF_STEP: 0 hold, 1 live slow motion in half steps (the default; slowed playback keeps the
// tested hold), 2 slowed playback in half steps too (slow_half_steps = playback; not yet tested in the game).
volatile int g_half_mode = 1;  // also set live by the settings menu (ss_patch_apply)
// Test aid (key-file command slowtest<seconds>): until then every update of the race is treated as if the cameras
// had asked for one tick, so a slow motion can be measured at a chosen moment.
volatile int64_t g_slow_test_until = 0;
int g_half_live_ok = -1, g_half_play_ok = -1;  // code check: -1 not yet, 0 differs (hold), 1 known build

// The moving objects' player stand-in for a "keep" half: +0x18 (a divisor) 1, everything else 0 (no objects, no
// events, cursor 0 of 0 entries). 0x7CCDC0 only reads it.
alignas(16) uint32_t g_movers_standin[64] = {0, 0, 0, 0, 0, 0, 1};

bool HalfLiveCode() {
  if (g_half_live_ok < 0) {
    g_half_live_ok = SigBytes(0x4AF8ED, kScaleSig, sizeof kScaleSig) && SigBytes(0x4B2700, kScaleCallSig, sizeof kScaleCallSig) &&
                     SigBytes(0x4B5065, kClockSig, sizeof kClockSig);
    Log("[takedown] half step code (time scale 0x4AF810, game clock) %s", g_half_live_ok ? "ok" : "differs from the known build: slow motion held");
  }
  return g_half_live_ok == 1;
}

bool HalfPlayCode() {
  if (g_half_play_ok < 0) {
    g_half_play_ok =
        SigBytes(0x4B2EBA, kPlayersSig, sizeof kPlayersSig) && SigBytes(0x4B2E24, kConsumeSig, sizeof kConsumeSig) &&
        SigBytes(0x7BD820, kConsumeFnSig, sizeof kConsumeFnSig) && SigBytes(0x7BDEA0, kRewindUpdateSig, sizeof kRewindUpdateSig) &&
        SigBytes(0x7BDBC0, kRewindStepSig, sizeof kRewindStepSig) && SigBytes(0x7BDD90, kCompleteSig, sizeof kCompleteSig) &&
        SigBytes(0x4B31C4, kCompleteCallSig, sizeof kCompleteCallSig) && SigBytes(0x7CCDC0, kMoversSig, sizeof kMoversSig) &&
        SigBytes(0x7CCF0F, kMoversMidSig, sizeof kMoversMidSig) && SigBytes(0x7CD02D, kMoversTailSig, sizeof kMoversTailSig) &&
        SigBytes(0x7CCB20, kMoverEventsSig, sizeof kMoverEventsSig) && SigBytes(0x7C9760, kPowerPlaysSig, sizeof kPowerPlaysSig) &&
        SigBytes(0x7C8D53, kPowerPlaysTailSig, sizeof kPowerPlaysTailSig);
    Log("[takedown] half step playback code (rewind manager, playback players) %s",
        g_half_play_ok ? "ok" : "differs from the known build: slowed playback held");
  }
  return g_half_play_ok == 1;
}

inline uint8_t Rb(uintptr_t va) { return *reinterpret_cast<const volatile uint8_t*>(va); }

// The in-game controller and its race object when both look like the known build (else 0).
uintptr_t HalfController(uintptr_t* race_out) {
  const uintptr_t c = Rd(kController);
  if (!c || !Readable(c, kCtrlStepByte + 1) || Rd(c) != kControllerVt) return 0;
  const uintptr_t race = Rd(c + 0x14);
  if (!race || !Readable(race, kRacePowerPlays + 4)) return 0;
  const uintptr_t ai = Rd(race + 0xBC);
  if (!ai || !Readable(ai, 4) || Rd(ai) != kAiMgrVt) return 0;
  if (race_out) *race_out = race;
  return c;
}

// A half step can run: the code is the known build and none of the debug switches that change the factor's choice or
// the tick count is on (the time scale then takes the slow-motion tweakable, 0x4AF8ED).
bool HalfLiveReady(uintptr_t* ctrl, uintptr_t* race) {
  if (!HalfLiveCode()) return false;
  *ctrl = HalfController(race);
  if (!*ctrl) return false;
  return Rb(*ctrl + kCtrlStepByte) == 0 && Rb(kSlowCheatOn) == 0 && Rb(kFastCheatOn) == 0 && Rb(kTicksOverride) == 0;
}

// The ticks field of a record of the playing ring (index counted from the oldest), or 0.
uintptr_t RecordTicks(uintptr_t m, uint32_t index) {
  const uintptr_t ring = Rd(m + 0x11C);
  const uint32_t cap = Rd(m + 0x120), first = Rd(m + 0x124);
  if (!ring || cap == 0 || cap > 4096 || index >= cap) return 0;
  const uintptr_t at = ring + ((first + index) % cap) * 12 + 8;
  return Readable(at, 4) ? at : 0;
}

// The record raised to 2 ticks for its two half steps (between the "keep" and the "play" update).
struct HalfPlay {
  bool raised = false;
  uintptr_t mgr = 0, ticks_at = 0;
  uint32_t index = 0;
} g_hp;

void HalfPlayRestore(const char* why) {
  if (!g_hp.raised) return;
  g_hp.raised = false;
  if (g_hp.ticks_at && Readable(g_hp.ticks_at, 4) && Rd(g_hp.ticks_at) == 2) Wr(g_hp.ticks_at, 1);
  if (why) Log("[takedown] half step playback: record %u given back its tick (%s)", g_hp.index, why);
}

enum SlowAction { SLOW_RUN = 0, SLOW_HOLD, SLOW_HALF, SLOW_HALF_KEEP, SLOW_HALF_PLAY };

struct SlowState {
  bool last_held = false;  // the previous update was held
  bool on = false;         // inside a slowed stretch (for the log)
  bool replay = false, camera = false;
  uint32_t updates = 0, held = 0, half = 0;
  double game_s = 0;
  int64_t start = 0;
  uint32_t last_next = 0;
  void* last_self = nullptr;
} g_slow;

struct SlowPlan {
  SlowAction action = SLOW_RUN;
  uintptr_t ctrl = 0, race = 0;
  uint32_t clock_ticks = 1;  // the ticks the game clock adds this update (one when the cameras ask for one)
};

// One decision per update (see above). The hold is the fallback whenever a half step cannot be vouched for.
// `base`: the time scale the race's updates run at anyway (1, or 60/g_rate in a race in 1/g_rate steps: `fast`).
SlowPlan SlowDecide(void* self, bool sixty, uint32_t ticks, double base, bool fast) {
  SlowPlan plan;
  bool playing = false, replay_slow = false;
  const uintptr_t m = Rd(kRewindMgr);
  if (m && Readable(m, 0x138)) {
    playing = Rd(m + kRewindPlaying) != 0;
    replay_slow = playing && Rd(m + kRewindSlow) != 0;
  }
  const bool in_race = self == reinterpret_cast<void*>(static_cast<uintptr_t>(kStates[5].obj));  // cInGameRunState
  const int64_t test_until = g_slow_test_until;
  const bool one_tick = in_race && (OneTickRequested() || (test_until && Now() < test_until));
  bool slowed = playing ? replay_slow : one_tick;
  plan.clock_ticks = one_tick ? 1 : ticks;
  const bool want_half = sixty && g_takedown_realtime && g_half_mode >= 1 && in_race;

  // The playback: which record this update plays and whether it is slow (a record left at the start of the update
  // takes its slowness from +0x10C, 0x7BDBF2).
  bool play_half = false;
  // (A race in 1/g_rate steps holds every second update of a slowed playback: the tested path; plan M5.)
  if (playing && want_half && !fast && g_half_mode >= 2 && HalfPlayCode() && HalfLiveReady(&plan.ctrl, &plan.race) &&
      Rd(plan.race + kRaceRewind) == m && Rb(kRewindDebug) == 0 && Rb(m + 0x110) == 0) {
    const uint32_t cur = Rd(m + 0x12C), cnt = Rd(m + 0x128), consumed = Rd(m + 0x130);
    const uintptr_t cur_at = RecordTicks(m, cur);
    if (g_hp.raised) {
      if (g_hp.mgr == m && g_hp.index == cur && cur_at == g_hp.ticks_at && consumed == 1 && Rd(cur_at) == 2) {
        plan.action = SLOW_HALF_PLAY;
        play_half = true;
        slowed = true;
      } else {
        HalfPlayRestore("the playback moved otherwise");
      }
    }
    if (!play_half && cur_at && cur < cnt) {
      const bool leaves = consumed == Rd(cur_at);
      const uint32_t r = leaves ? cur + 1 : cur;
      const uint32_t r_consumed = leaves ? 0 : consumed;
      const bool r_slow = (leaves ? Rd(m + 0x10C) : Rd(m + kRewindSlow)) != 0;
      const uintptr_t r_at = r < cnt ? RecordTicks(m, r) : 0;
      if (r < cnt) slowed = r_slow;  // the record this update plays decides (a full update when it is not slow)
      if (r_slow && r_at && r_consumed == 0 && Rd(r_at) == 1) {
        plan.action = SLOW_HALF_KEEP;
        play_half = true;
        Wr(r_at, 2);  // two ticks: this update leaves it under way, the next one completes it (see above)
        g_hp.raised = true;
        g_hp.mgr = m;
        g_hp.ticks_at = r_at;
        g_hp.index = r;
      }
    }
  } else if (g_hp.raised) {
    HalfPlayRestore(playing ? "half steps no longer possible" : "the playback ended");
  }
  if (playing && !play_half && g_hp.raised) HalfPlayRestore("not a half step");

  SlowState& s = g_slow;
  if (!slowed) {
    if (s.on) {
      Log("[takedown] slow motion end: %u updates, %u half steps, %u held, %.2f s wall, %.2f s game (x%.2f) (%s%s%s)",
          s.updates, s.half, s.held, Secs(Now() - s.start), s.game_s, Secs(Now() - s.start) > 0 ? s.game_s / Secs(Now() - s.start) : 0.0,
          s.camera ? "camera one-tick request" : "", s.camera && s.replay ? " + " : "", s.replay ? "slow-motion replay" : "");
      s.on = false;
    }
    s.last_held = false;
    plan.action = SLOW_RUN;
    return plan;
  }
  if (!play_half && !playing && want_half && HalfLiveReady(&plan.ctrl, &plan.race)) plan.action = SLOW_HALF;
  if (!s.on) {
    s = SlowState{false, true, false, false, 0, 0, 0, 0, Now(), s.last_next, s.last_self};
    const bool half = plan.action != SLOW_RUN;
    Log("[takedown] slow motion start (%s, %s)", playing ? "slow-motion replay" : "camera one-tick request",
        half ? "half steps: every update runs at half the tick time" : sixty && g_takedown_realtime ? "every second update held" : "not held");
  }
  (playing ? s.replay : s.camera) = true;
  ++s.updates;
  if (plan.action != SLOW_RUN) {
    s.last_held = false;
    ++s.half;
    s.game_s += 0.5 * base * (playing ? 1 : plan.clock_ticks) / 60.0;
    return plan;
  }
  // Never two in a row, and never before this state has run once (the held step returns its last result).
  const bool hold = sixty && g_takedown_realtime && !s.last_held && self == s.last_self;
  s.last_held = hold;
  if (hold) {
    ++s.held;
    plan.action = SLOW_HOLD;
  } else {
    s.game_s += base * double(playing ? 1 : plan.clock_ticks) / 60.0;
  }
  return plan;
}

// The two .data tweakables of a half step, put back however the update is left.
struct SlowCheatScope {
  volatile uint8_t* on = reinterpret_cast<volatile uint8_t*>(kSlowCheatOn);
  volatile float* speed = reinterpret_cast<volatile float*>(kSlowCheatSpeed);
  const uint8_t on_was = *on;
  const float speed_was = *speed;
  bool set = false;
  void Enable(float factor) {
    *speed = factor;
    *on = 1;
    set = true;
  }
  void Restore() {
    if (!set) return;
    *on = on_was;
    *speed = speed_was;
    set = false;
  }
  ~SlowCheatScope() { Restore(); }
};

// The controller and race the plan was made for are still the live ones (the update did not tear the race down).
bool HalfRaceStillLive(const SlowPlan& plan) {
  if (Rd(kController) != plan.ctrl || !Readable(plan.ctrl, 0x18) || Rd(plan.ctrl) != kControllerVt) return false;
  if (Rd(plan.ctrl + 0x14) != plan.race || !Readable(plan.race, kRacePowerPlays + 4)) return false;
  const uintptr_t ai = Rd(plan.race + 0xBC);
  return ai && Readable(ai, 4) && Rd(ai) == kAiMgrVt;
}

// Runs the game's update as a half step: the slow-motion tweakable on at 0.5 for this one update, the game clock put
// back by half the ticks it adds, and for a "keep" half the two cursor players kept from moving on.
// In a race in 1/g_rate steps every update runs through here: `factor` is 60/g_rate (half of it in a half step), and
// the clock is put back by the part of the ticks the simulation does not run (`fix_clock`: the update is expected
// to go through the race update; the other paths ignore the scale and are run at 60 a second).
uint32_t HalfStepUpdate(UpdateFn update, void* self, uint32_t ticks, const SlowPlan& plan, float factor, bool fix_clock) {
  SlowCheatScope cheat;
  volatile float* clock = reinterpret_cast<volatile float*>(plan.ctrl + kCtrlClock);
  if (fix_clock) *clock = *clock - float(plan.clock_ticks) * ((1.0f - factor) / 60.0f);
  cheat.Enable(factor);
  uint32_t movers = 0, powerplays = 0;
  const uint32_t standin = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_movers_standin));
  const bool keep = plan.action == SLOW_HALF_KEEP;
  if (keep) {
    movers = Rd(plan.race + kRaceMovers);
    powerplays = Rd(plan.race + kRacePowerPlays);
    Wr(plan.race + kRaceMovers, standin);
    Wr(plan.race + kRacePowerPlays, 0);
  }
  const uint32_t next = update(self, ticks);
  cheat.Restore();  // the .data bytes first, whatever happened to the race
  if (!HalfRaceStillLive(plan)) {
    // The race was torn down (or replaced) inside the update: its memory is not ours to touch any more. The raised
    // record went with it.
    if (g_hp.raised) {
      g_hp.raised = false;
      Log("[takedown] half step: the race changed during the update; record %u left as it is", g_hp.index);
    }
    return next;
  }
  if (keep) {
    if (Rd(plan.race + kRaceMovers) == standin) Wr(plan.race + kRaceMovers, movers);
    if (Rd(plan.race + kRacePowerPlays) == 0) Wr(plan.race + kRacePowerPlays, powerplays);
  }
  if (keep && g_hp.raised) {
    // The record must now be under way: the one this update played, one tick of its two consumed.
    const uintptr_t m = g_hp.mgr;
    if (!(Readable(m, 0x138) && Rd(m + kRewindPlaying) && Rd(m + 0x12C) == g_hp.index && Rd(m + 0x130) == 1))
      HalfPlayRestore("the update did not start the record");
  } else if (plan.action == SLOW_HALF_PLAY && g_hp.raised) {
    // Its second half played: one tick again, consumed, so the next update leaves it as the game does.
    const uintptr_t m = g_hp.mgr;
    const bool as_expected = Readable(m, 0x138) && Rd(m + 0x12C) == g_hp.index && Rd(m + 0x130) == 2;
    HalfPlayRestore(as_expected ? nullptr : "the update did not complete the record");
    if (as_expected) Wr(m + 0x130, 1);
  }
  return next;
}

// ---------------------------------------------------------- 120 fps groundwork: instrumentation
//
// docs/plan-120fps.md, M0. Everything here only reads the game, except the two vptr stores below, and none of it
// runs unless SS_PATCH_LOG=1 is set (or a rate above 60 is asked for). SS_PATCH_OFF=hfrhook leaves the vptr stores
// out.
//
// The in-game controller holds embedded objects with a vptr of their own. The race update 0x4B2670 calls
//   * the camera director's (ctrl+0x1A680, vtable 0xBB8D38) slot 5 once, right before the tick loop (0x4B2DA3), and
//   * the camera controller's (ctrl+0x370, vtable 0xBB7EDC) slot 5 at the end of every tick of the loop (0x4B31B5),
// both thiscall with one stack argument. Each object gets a private copy of its vtable (as the AI players do) whose
// slot 5 counts the call and runs the original: "this update went through the race update" and "ticks the loop ran",
// seen from inside the update without touching code.
constexpr uint32_t kCtrlDirector = 0x1A680, kCtrlCamCtl = 0x370, kCtrlTickCount = 0x1A748;
constexpr uint32_t kDirectorVt = 0xBB8D38, kCamCtlVt = 0xBB7EDC, kDirectorFn = 0x4CCFA0, kCamCtlFn = 0x4C2490;
constexpr int kEmbSlots = 12;  // both vtables have twelve entries
constexpr uint32_t kRaceTimelines = 0x164;
// The race's timeline managers that get the integer "completed ticks" of the update through their slot 1
// ([race+0x150] at 0x4B3253, [race+0x15C] at 0x4B326D, [race+0x158] at 0x4B3513, [race+0x160] at 0x4B353D; each
// call sits behind a "completed > 0" test), by class (SplitSecondTimeline::c...TimelineManager):
constexpr uint32_t kRaceTimelineMgrs[] = {0x150, 0x158, 0x15C, 0x160};
constexpr uint32_t kStandardTlVt = 0xBD3F70;   // +0xC count, +0x10 array of timelines (0x57DBE0)
constexpr uint32_t kCollisionTlVt = 0xBD4238;  // +0x514D0 count (32 at most), +0x51450 array of entries, [entry] = timeline (0x5800A0)
constexpr uint32_t kLevelLoadTlVt = 0xBD44EC;  // +0x10 count, +0x20 timelines in place (0x581F40)
constexpr uint32_t kVehicleTlVt = 0xBD4714;    // +0x68 vehicles, +0x10 + 12 i -> 27 timelines each (0x582D20)
const uint8_t kDirectorCallSig[] = {0x8B, 0x40, 0x14, 0x8D, 0x93, 0x8C, 0x03, 0x00, 0x00, 0x8D, 0x8B,
                                    0x80, 0xA6, 0x01, 0x00, 0x89, 0x54, 0x24, 0x54, 0x8D, 0x54, 0x24,
                                    0x50, 0x52, 0xF3, 0x0F, 0x11, 0x44, 0x24, 0x54, 0xFF, 0xD0};  // 0x4B2D85
const uint8_t kCamCtlCallSig[] = {0x8B, 0x93, 0x70, 0x03, 0x00, 0x00, 0xF3, 0x0F, 0x10, 0x44, 0x24, 0x3C, 0x8B, 0x52, 0x14,
                                  0x8D, 0x8B, 0x70, 0x03, 0x00, 0x00, 0x88, 0x44, 0x24, 0x55, 0x8D, 0x44, 0x24, 0x50, 0x50,
                                  0xF3, 0x0F, 0x11, 0x44, 0x24, 0x54, 0xC6, 0x44, 0x24, 0x58, 0x00, 0xFF, 0xD2};  // 0x4B318C
const uint8_t kDirectorFnSig[] = {0x83, 0xEC, 0x1C, 0x57, 0x8B, 0xF9, 0xE8, 0xD5, 0x2A, 0x0C, 0x00, 0x84, 0xC0, 0x0F, 0x84, 0xF3};
const uint8_t kCamCtlFnSig[] = {0x53, 0x56, 0x33, 0xDB, 0x38, 0x1D, 0x98, 0x19, 0xD7, 0x00, 0x57, 0x8B, 0xF9, 0x74, 0x19, 0xA1};
// The timelines: the race's manager (race+0x164; +0xC count, +0x10 array of 8 byte entries, the timeline at +4) is
// advanced by 0x582200(ticks, ..) -> 0xA1FC00 per timeline: while +0xC == 1 (playing), frame (+0) += ticks; the
// tracks get frame x 1/60 s; the end is frame >= +4.
const uint8_t kTimelineMgrSig[] = {0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x77, 0x10, 0x8B,
                                   0x47, 0x0C, 0x8B, 0xCE, 0x8D, 0x14, 0xC1, 0x3B};  // 0x582200
const uint8_t kTimelineSig[] = {0x83, 0xEC, 0x08, 0x53, 0x56, 0x8B, 0xF1, 0xBB, 0x01, 0x00, 0x00, 0x00,
                                0x39, 0x5E, 0x0C, 0x0F, 0x85, 0xB5, 0x00, 0x00, 0x00, 0x8B, 0x06, 0x3B};  // 0xA1FC00

uint32_t g_cad_to_race = 0, g_cad_to_base = 0;  // cadence transitions (for the log)

using EmbFn = uint32_t(THISCALL*)(void*, void*);
alignas(16) uint32_t g_dirvt[kEmbSlots + 1], g_camvt[kEmbSlots + 1];  // [0] = RTTI locator, [1..] = slots

struct HfrClass {  // one kind of update: how far the playing timelines and the game clock moved
  uint32_t updates = 0, samples = 0, frames = 0;
  double clock_s = 0;
};

struct Hfr {
  bool on = false;        // the instrumentation runs
  bool hooks = true;      // the two vptr stores are allowed (SS_PATCH_OFF=hfrhook: not)
  int code_ok = -1;       // -1 not checked, 0 differs from the known build, 1 known build
  bool timelines_ok = false;
  volatile uint32_t dir_calls = 0, tick_calls = 0;  // counted by the wrappers
  uintptr_t ctrl_logged = 0, race_logged = 0;
  float clock_now = 0;
  // the 10 s window
  int64_t t0 = 0, prev_step = 0;
  double wall_s = 0, clock_s = 0;
  uint32_t race_updates = 0, other_updates = 0, held = 0, loop_ticks = 0, hook_ticks = 0, playing_sum = 0;
  uint32_t playing_by[5] = {};  // playing timelines seen, per manager (race+0x150, +0x158, +0x15C, +0x160, +0x164)
  HfrClass normal, playback;
  HfrClass slow;          // the slow motion under way (reported when it ends)
  // the race's class and the cadence (M2)
  uintptr_t class_mgr = 0;
  bool fast = false;            // the race's simulation runs in 1/g_rate steps (fixed while anything is recorded)
  bool last_race_path = false;  // the last update of the race state went through the race update
  uint32_t wrong_guess = 0, lever_updates = 0, dropouts = 0;
  double race_s = 0, base_s = 0;
  // rewind census
  bool req_seen = false, play_on = false;
  uint32_t play_updates = 0, play_held = 0, play_slow = 0;
  int64_t play_t0 = 0;
} g_hfr;

uint32_t ENTRY DirectorWrap(void* self, void* arg) {
  g_hfr.dir_calls = g_hfr.dir_calls + 1;
  return reinterpret_cast<EmbFn>(kDirectorFn)(self, arg);
}

uint32_t ENTRY CamCtlWrap(void* self, void* arg) {
  g_hfr.tick_calls = g_hfr.tick_calls + 1;
  return reinterpret_cast<EmbFn>(kCamCtlFn)(self, arg);
}

bool HfrCode() {
  if (g_hfr.code_ok < 0) {
    const bool ok =
        SigBytes(0x4B2D85, kDirectorCallSig, sizeof kDirectorCallSig) && SigBytes(0x4B318C, kCamCtlCallSig, sizeof kCamCtlCallSig) &&
        SigBytes(kDirectorFn, kDirectorFnSig, sizeof kDirectorFnSig) && SigBytes(kCamCtlFn, kCamCtlFnSig, sizeof kCamCtlFnSig) &&
        Readable(kDirectorVt - 4, (kEmbSlots + 1) * 4) && Readable(kCamCtlVt - 4, (kEmbSlots + 1) * 4) &&
        Rd(kDirectorVt + 0x14) == kDirectorFn && Rd(kCamCtlVt + 0x14) == kCamCtlFn;
    if (ok) {
      for (int i = 0; i < kEmbSlots + 1; ++i) {
        g_dirvt[i] = Rd(kDirectorVt - 4 + 4 * i);
        g_camvt[i] = Rd(kCamCtlVt - 4 + 4 * i);
      }
      g_dirvt[1 + 5] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&DirectorWrap));
      g_camvt[1 + 5] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&CamCtlWrap));
    }
    g_hfr.timelines_ok = SigBytes(0x582200, kTimelineMgrSig, sizeof kTimelineMgrSig) && SigBytes(0xA1FC00, kTimelineSig, sizeof kTimelineSig);
    g_hfr.code_ok = ok;
    Log("[hfr] in-update hook code (camera director 0x4B2DA3, camera controller 0x4B31B5) %s; timeline code %s",
        ok ? "ok" : "differs from the known build: not hooked", g_hfr.timelines_ok ? "ok" : "differs: not sampled");
  }
  return g_hfr.code_ok == 1;
}

// The controller with everything the instrumentation reads (else 0).
uintptr_t HfrController(uintptr_t* race) {
  const uintptr_t c = HalfController(race);
  return c && Readable(c, kCtrlTickCount + 4) ? c : 0;
}

// The two embedded objects of this controller get the private vtables (two vptr stores into the controller object).
// True when both are in place.
bool HfrSwap(uintptr_t c) {
  if (!HfrCode() || !g_hfr.hooks) return false;
  const uint32_t dir = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_dirvt[1]));
  const uint32_t cam = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_camvt[1]));
  if (g_hfr.ctrl_logged != c) {
    g_hfr.ctrl_logged = c;
    Log("[hfr] controller %p embedded vptrs: +0x330 %08x, +0x340 %08x, +0x358 %08x, +0x364 %08x, +0x370 %08x, +0xDA4 %08x, +0x1A680 %08x",
        reinterpret_cast<void*>(c), Rd(c + 0x330), Rd(c + 0x340), Rd(c + 0x358), Rd(c + 0x364), Rd(c + kCtrlCamCtl), Rd(c + 0xDA4),
        Rd(c + kCtrlDirector));
  }
  if (Rd(c + kCtrlDirector) == kDirectorVt) {
    Wr(c + kCtrlDirector, dir);
    Log("[hfr] controller %p: camera director wrapped (vptr %08x -> %08x)", reinterpret_cast<void*>(c), kDirectorVt, dir);
  }
  if (Rd(c + kCtrlCamCtl) == kCamCtlVt) {
    Wr(c + kCtrlCamCtl, cam);
    Log("[hfr] controller %p: camera controller wrapped (vptr %08x -> %08x)", reinterpret_cast<void*>(c), kCamCtlVt, cam);
  }
  return Rd(c + kCtrlDirector) == dir && Rd(c + kCtrlCamCtl) == cam;
}

// ---------------------------------------------------------- timeline ticks (docs/plan-120fps.md M3, step 1)
//
// The race update hands the integer "completed ticks" of the update to slot 1 `update(ticks, a, b)` of four
// timeline managers ([race+0x150], +0x158, +0x15C, +0x160), and a timeline moves one frame of 1/60 s per tick.
// Whenever an update runs with the game's time scale below 1 (a half step at 60; every update of a race in 1/fps
// steps) the tick is shorter than 1/60 s and the timelines ran too fast against the cars: 2.00 frames per 1/60 s of
// game time in a half step at 60 and at 120, 4.00 in a slow motion at 120 (measured, plan section 11).
// Each manager object gets a private copy of its class's vtable (a vptr store into a heap object, as for the AI
// players) whose slot 1 hands on floor(acc += ticks x scale) ticks and does not call the manager at all when that is
// zero: the game's own case, every call site sits behind "completed > 0". With the scale at 1 the wrapper passes
// the call through unchanged. hfr_ticks = raw (or SS_PATCH_OFF=hfrticks) leaves the managers alone.
constexpr int kTlClasses = 4;
const uint32_t kTlVts[kTlClasses] = {kStandardTlVt, kCollisionTlVt, kLevelLoadTlVt, kVehicleTlVt};
const uint32_t kTlFns[kTlClasses] = {0x57DBE0, 0x581DE0, 0x581F40, 0x582E60};  // slot 1 of each, thiscall, ret 0xC
const uint8_t kTlFnSigs[kTlClasses][12] = {
    {0x53, 0x8B, 0x5C, 0x24, 0x0C, 0x56, 0x57, 0x8B, 0xF9, 0x8B, 0xCB, 0xE8},
    {0x56, 0x8B, 0xF1, 0x83, 0xBE, 0x30, 0x14, 0x05, 0x00, 0x00, 0x76, 0x0A},
    {0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x10, 0x57, 0x8D, 0x7E, 0x20, 0x8D, 0x4C},
    {0x6A, 0xFF, 0x68, 0x90, 0x1A, 0xB3, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00}};
// The four call sites: the tick count is the first argument (pushed last) and the result is not used.
const uint8_t kTlCallSigA[] = {0x8B, 0x11, 0x6A, 0x00, 0x50, 0x8B, 0x42, 0x04, 0x56, 0xFF, 0xD0};  // 0x4B324A, 0x4B3264
const uint8_t kTlCallSigB[] = {0x8B, 0x31, 0x52, 0x8B, 0x56, 0x04, 0x50, 0x57, 0xFF, 0xD2};        // 0x4B350B, 0x4B3535
constexpr int kTlSlots = 12;  // copied words after the RTTI locator (the classes have 8 or 9 slots)
alignas(16) uint32_t g_tlvt[kTlClasses][kTlSlots + 1];
using TlFn = uint32_t(THISCALL*)(void*, uint32_t, void*, void*);

bool g_tl_on = true;             // hfr_ticks = sixty (default) | raw
int g_tl_code = -1;              // -1 not checked, 0 differs from the known build, 1 known build
volatile double g_tl_units = 1;  // the time scale of the update now running (1: pass through)
struct TlAcc {
  void* self = nullptr;
  double acc = 0;
  uint32_t used = 0;  // g_tl_use at this manager's last call: the entry not called for the longest time is given away
} g_tl_acc[8];
uint32_t g_tl_use = 0;
struct TlStats {
  uint32_t calls = 0, skipped = 0, in = 0, out = 0, swapped = 0;
} g_tl_st;

uint32_t TlRun(int k, void* self, uint32_t ticks, void* a, void* b) {
  const TlFn orig = reinterpret_cast<TlFn>(kTlFns[k]);
  // One accumulator per manager object. A new race has new managers and the table keeps the old races' entries,
  // so a manager that is not in it takes the entry that was not called for the longest time. (It used to take
  // entry 0 when the table was full: from the third race of a session on, all the managers shared that entry, each
  // call cleared the fraction the one before had left, and a timeline moved only in an update of two ticks or
  // more: 4528 of 4664 calls left out in a race at 120, a Power Play gate that took the energy and stood still.)
  TlAcc* e = nullptr;
  for (auto& x : g_tl_acc) {
    if (x.self == self) { e = &x; break; }
    if (!e || x.used < e->used) e = &x;
  }
  if (e->self != self) { e->self = self; e->acc = 0; }
  e->used = ++g_tl_use;
  const double units = g_tl_units;
  if (units == 1.0 || ticks == 0 || ticks > 64) {  // a full tick (or nothing we know): as the game calls it
    e->acc = 0;
    return orig(self, ticks, a, b);
  }
  e->acc += double(ticks) * units;
  const uint32_t n = static_cast<uint32_t>(e->acc + 1e-6);
  e->acc -= n;
  if (e->acc < 0) e->acc = 0;
  ++g_tl_st.calls;
  g_tl_st.in += ticks;
  g_tl_st.out += n;
  if (!n) {
    ++g_tl_st.skipped;
    return 0;
  }
  return orig(self, n, a, b);
}

uint32_t ENTRY TlWrap0(void* self, uint32_t ticks, void* a, void* b) { return TlRun(0, self, ticks, a, b); }
uint32_t ENTRY TlWrap1(void* self, uint32_t ticks, void* a, void* b) { return TlRun(1, self, ticks, a, b); }
uint32_t ENTRY TlWrap2(void* self, uint32_t ticks, void* a, void* b) { return TlRun(2, self, ticks, a, b); }
uint32_t ENTRY TlWrap3(void* self, uint32_t ticks, void* a, void* b) { return TlRun(3, self, ticks, a, b); }

bool TlCode() {
  if (g_tl_code < 0) {
    bool ok = SigBytes(0x4B324A, kTlCallSigA, sizeof kTlCallSigA) && SigBytes(0x4B3264, kTlCallSigA, sizeof kTlCallSigA) &&
              SigBytes(0x4B350B, kTlCallSigB, sizeof kTlCallSigB) && SigBytes(0x4B3535, kTlCallSigB, sizeof kTlCallSigB);
    for (int k = 0; ok && k < kTlClasses; ++k)
      ok = Readable(kTlVts[k] - 4, (kTlSlots + 1) * 4) && Rd(kTlVts[k] + 4) == kTlFns[k] &&
           SigBytes(kTlFns[k], kTlFnSigs[k], sizeof kTlFnSigs[k]);
    if (ok) {
      const TlFn wraps[kTlClasses] = {&TlWrap0, &TlWrap1, &TlWrap2, &TlWrap3};
      for (int k = 0; k < kTlClasses; ++k) {
        for (int i = 0; i < kTlSlots + 1; ++i) g_tlvt[k][i] = Rd(kTlVts[k] - 4 + 4 * i);
        g_tlvt[k][1 + 1] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(wraps[k]));
      }
    }
    g_tl_code = ok;
    Log("[hfr] timeline manager code (slot 1 of four classes, four call sites) %s",
        ok ? "ok" : "differs from the known build: timelines get the raw tick count");
  }
  return g_tl_code == 1;
}

// The class index of a timeline manager's vptr (the game's vtable or our copy of it), or -1.
int TlClassOf(uint32_t vt) {
  for (int k = 0; k < kTlClasses; ++k)
    if (vt == kTlVts[k] || vt == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_tlvt[k][1]))) return k;
  return -1;
}

// The race's four managers get the private vtables (before the first update that runs with a scale below 1).
void TlSwap(uintptr_t race) {
  if (!g_tl_on || !race || !Readable(race, 0x168) || !TlCode()) return;
  for (const uint32_t off : kRaceTimelineMgrs) {
    const uintptr_t mgr = Rd(race + off);
    if (!mgr || !Readable(mgr, 4)) continue;
    const uint32_t vt = Rd(mgr);
    for (int k = 0; k < kTlClasses; ++k) {
      if (vt != kTlVts[k]) continue;
      Wr(mgr, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_tlvt[k][1])));
      ++g_tl_st.swapped;
      Log("[hfr] timeline manager race+0x%X (%p, vtable %08x): ticks by game time from now on", off, reinterpret_cast<void*>(mgr), vt);
    }
  }
}

// What one in-race update is measured against.
struct HfrSnap {
  uintptr_t ctrl = 0, race = 0;
  float clock = 0;
  uint32_t ticks = 0, dir = 0, tick = 0;
  int ntl = 0;
  bool playing = false, hooked = false;
};
constexpr int kMaxTimelines = 512;
uintptr_t g_tl_at[kMaxTimelines];
uint32_t g_tl_frame[kMaxTimelines];

float Rf(uintptr_t va) {
  float f;
  memcpy(&f, reinterpret_cast<const void*>(va), 4);
  return f;
}

void HfrAddTimeline(HfrSnap& s, uintptr_t t) {
  if (s.ntl >= kMaxTimelines || !t || !Readable(t, 0x10) || Rd(t + 0xC) != 1) return;  // not playing
  g_tl_at[s.ntl] = t;
  g_tl_frame[s.ntl] = Rd(t);
  ++s.ntl;
}

void HfrManagerTimelines(HfrSnap& s, uintptr_t mgr) {
  if (!mgr || !Readable(mgr, 0x70)) return;
  const int k = TlClassOf(Rd(mgr));
  const uint32_t vt = k < 0 ? 0 : kTlVts[k];  // the class, whether or not the manager has our vtable copy
  if (vt == kStandardTlVt) {
    const uint32_t count = Rd(mgr + 0xC);
    const uintptr_t arr = Rd(mgr + 0x10);
    if (count == 0 || count > 4096 || !arr || !Readable(arr, count * 4)) return;
    for (uint32_t i = 0; i < count; ++i) HfrAddTimeline(s, Rd(arr + 4 * i));
  } else if (vt == kLevelLoadTlVt) {
    const uint32_t count = Rd(mgr + 0x10);
    for (uint32_t i = 0; i < count && i < 20; ++i) HfrAddTimeline(s, Rd(mgr + 0x20 + 4 * i));
  } else if (vt == kCollisionTlVt) {
    if (!Readable(mgr + 0x51450, 0x84)) return;
    const uint32_t count = Rd(mgr + 0x514D0);
    for (uint32_t i = 0; i < count && i < 32; ++i) {
      const uintptr_t e = Rd(mgr + 0x51450 + 4 * i);
      if (e && Readable(e, 4)) HfrAddTimeline(s, Rd(e));
    }
  } else if (vt == kVehicleTlVt) {
    const uint32_t count = Rd(mgr + 0x68);
    for (uint32_t i = 0; i < count && i < 7; ++i) {
      const uintptr_t arr = Rd(mgr + 0x10 + 12 * i);
      if (!arr || !Readable(arr, 27 * 4)) continue;
      for (uint32_t j = 0; j < 27; ++j) HfrAddTimeline(s, Rd(arr + 4 * j));
    }
  }
}

void HfrBefore(HfrSnap& s) {
  s.ctrl = HfrController(&s.race);
  if (!s.ctrl) return;
  s.hooked = HfrSwap(s.ctrl);
  s.clock = Rf(s.ctrl + kCtrlClock);
  s.ticks = Rd(s.ctrl + kCtrlTickCount);
  s.dir = g_hfr.dir_calls;
  s.tick = g_hfr.tick_calls;
  const uintptr_t m = Rd(kRewindMgr);
  s.playing = m && Readable(m, 0x138) && Rd(m + kRewindPlaying) != 0;
  if (!g_hfr.timelines_ok || !Readable(s.race, 0x168)) return;
  const uintptr_t tm = Rd(s.race + kRaceTimelines);
  const bool say = g_hfr.race_logged != s.race;
  g_hfr.race_logged = s.race;
  int k = 0;
  for (const uint32_t off : kRaceTimelineMgrs) {
    const uintptr_t mgr = Rd(s.race + off);
    if (say)
      Log("[hfr] race %p timeline manager +0x%X: %p (vptr %08x)", reinterpret_cast<void*>(s.race), off, reinterpret_cast<void*>(mgr),
          mgr && Readable(mgr, 4) ? Rd(mgr) : 0);
    const int before = s.ntl;
    HfrManagerTimelines(s, mgr);
    g_hfr.playing_by[k++] += s.ntl - before;
  }
  const int before = s.ntl;
  struct Tail {  // whatever the position-keyed manager adds below
    const HfrSnap& s;
    int before;
    ~Tail() { g_hfr.playing_by[4] += s.ntl - before; }
  } tail{s, before};
  if (!tm || !Readable(tm, 0x14)) return;  // the one played by position (0x582400 / 0x582200)
  const uint32_t count = Rd(tm + 0xC);
  const uintptr_t arr = Rd(tm + 0x10);
  if (count == 0 || count > 4096 || !arr || !Readable(arr, count * 8)) return;
  for (uint32_t i = 0; i < count; ++i) HfrAddTimeline(s, Rd(arr + 8 * i + 4));
}

void HfrClassLine(const char* what, const HfrClass& c) {
  if (!c.updates) return;
  const double per_update = c.samples ? double(c.frames) / c.samples : 0.0;
  const double clock_update = c.clock_s / c.updates;
  Log("[hfr] timeline (%s): %u updates, game clock %.5f s per update; playing timelines moved %.3f frames per update "
      "(%u samples) = %.2f frames per 1/60 s of game time (1.00 = in step with the simulation)",
      what, c.updates, clock_update, per_update, c.samples, clock_update > 0 ? per_update / (clock_update * 60.0) : 0.0);
}

// The rewind manager after an update: a request made during it (0x7BDE10 sets +0x110 pending and +0x118, the record
// the playback starts at; the next race update consumes it), and the playbacks' lengths.
void HfrRewind(bool held) {
  const uintptr_t m = Rd(kRewindMgr);
  if (!m || !Readable(m, 0x138)) return;
  const bool pending = Rb(m + 0x110) != 0;
  if (pending && !g_hfr.req_seen) {
    const uint32_t start = Rd(m + 0x118), count = Rd(m + 0x128), cap = Rd(m + 0x120);
    uint32_t reached = 0;
    for (uint32_t i = start; i < count && i < 4096; ++i) {
      const uintptr_t at = RecordTicks(m, i);
      if (at) reached += Rd(at);
    }
    Log("[hfr] rewind request: playback from record %u of %u (ring of %u): %u ticks back%s; slow-motion flag %u", start, count,
        cap, reached, start == 0 ? " (the whole ring: the request may have asked for more)" : "", Rd(m + 0x10C));
  }
  g_hfr.req_seen = pending;
  const bool playing = Rd(m + kRewindPlaying) != 0;
  if (playing && !g_hfr.play_on) {
    g_hfr.play_on = true;
    g_hfr.play_updates = g_hfr.play_held = g_hfr.play_slow = 0;
    g_hfr.play_t0 = Now();
    Log("[hfr] rewind playback start: record %u of %u, slow %u", Rd(m + 0x12C), Rd(m + 0x128), Rd(m + kRewindSlow));
  }
  if (g_hfr.play_on) {
    ++(held ? g_hfr.play_held : g_hfr.play_updates);
    if (playing && Rd(m + kRewindSlow)) ++g_hfr.play_slow;
    if (!playing) {
      g_hfr.play_on = false;
      Log("[hfr] rewind playback end: %u updates run, %u held (%u of both while slow), %.2f s wall", g_hfr.play_updates,
          g_hfr.play_held, g_hfr.play_slow, Secs(Now() - g_hfr.play_t0));
    }
  }
}

// Each car's standing start as a speed trace (SS_PATCH_LOG=1): from the update a standing car first moves, its speed
// and the path it has driven at fixed game times after it. For comparing the launch at 60 and above; the player's
// car (throttle held by the test's key route) is one of them.
void HfrLaunchTrace() {
  static const float kAt[] = {0.5f, 1, 1.5f, 2, 3, 4, 5, 6, 8, 10, 15, 20};
  constexpr int kN = sizeof kAt / sizeof kAt[0];
  struct Car {
    int state = 0;  // 0 not seen standing, 1 standing, 2 tracing, 3 done
    int next = 0;
    float t0 = 0, v[kN] = {};
    double d0 = 0, h0 = 0, d[kN] = {}, h[kN] = {};
  };
  static Car cars[64];
  static uintptr_t mgr = 0;
  if (!g_stats) return;
  if (g_ai_mgr != mgr) {
    mgr = g_ai_mgr;
    for (auto& c : cars) c = Car();
  }
  const float clock = g_hfr.clock_now;
  for (int i = 0; i < 64; ++i) {
    const AiState& e = g_ai[i];
    Car& c = cars[i];
    if (!e.self || !e.has_pos) continue;
    if (c.state == 3) {  // traced once: again when it stands again (the player's car after the rolling start)
      if (e.speed < 0.2f) c = Car();
      continue;
    }
    if (c.state == 0) {
      if (e.speed < 0.2f) c.state = 1;
      continue;
    }
    if (c.state == 1) {
      if (e.speed < 1.0f) continue;
      c.state = 2;
      c.t0 = clock;
      c.d0 = e.dist;
      c.h0 = e.turn_signed;
      continue;
    }
    if (clock - c.t0 < kAt[c.next]) continue;
    c.v[c.next] = e.speed;
    c.d[c.next] = e.dist - c.d0;
    c.h[c.next] = (e.turn_signed - c.h0) * 57.29578;
    if (++c.next < kN) continue;
    c.state = 3;
    char line[700];
    int n = snprintf(line, sizeof line, "[hfr] launch car %d from game clock %.2f s, speed m/s / path m / heading deg at", i, static_cast<double>(c.t0));
    for (int k = 0; k < kN; ++k)
      n += snprintf(line + n, sizeof line - n, " %gs %.1f/%.1f/%.1f", static_cast<double>(kAt[k]), static_cast<double>(c.v[k]), c.d[k], c.h[k]);
    Log("%s", line);
  }
}

void SteerWindowLog();
void HfrWindow(int64_t now) {
  if (Secs(now - g_hfr.t0) < 10.0) return;
  const uint32_t ran = g_hfr.race_updates + g_hfr.other_updates;
  if (ran + g_hfr.held) {
    Log("[hfr] clock: %.4f game s per wall s over %.2f s in the race state; updates: race path %u, other paths %u, held %u",
        g_hfr.wall_s > 0 ? g_hfr.clock_s / g_hfr.wall_s : 0.0, g_hfr.wall_s, g_hfr.race_updates, g_hfr.other_updates, g_hfr.held);
    Log("[hfr] ticks: %.1f loop ticks per wall s, %.3f per race-path update (tick counter %u, tick hook calls %u); "
        "%.1f timelines playing on average (seen per manager +0x150 / +0x158 / +0x15C / +0x160 / +0x164: %u / %u / %u / %u / %u)",
        g_hfr.wall_s > 0 ? g_hfr.loop_ticks / g_hfr.wall_s : 0.0, g_hfr.race_updates ? double(g_hfr.loop_ticks) / g_hfr.race_updates : 0.0,
        g_hfr.loop_ticks, g_hfr.hook_ticks, ran ? double(g_hfr.playing_sum) / ran : 0.0, g_hfr.playing_by[0], g_hfr.playing_by[1],
        g_hfr.playing_by[2], g_hfr.playing_by[3], g_hfr.playing_by[4]);
    HfrClassLine("normal updates", g_hfr.normal);
    HfrClassLine("rewind playback", g_hfr.playback);
    if (g_tl_st.calls)
      Log("[hfr] timeline ticks: %u scaled manager calls got %u ticks, %u handed on (%u calls left out)", g_tl_st.calls,
          g_tl_st.in, g_tl_st.out, g_tl_st.skipped);
    g_tl_st.calls = g_tl_st.skipped = g_tl_st.in = g_tl_st.out = 0;
    int cars = 0;
    double sum = 0, mn = 1e30, mx = 0, turn = 0, steer = 0;
    for (const auto& e : g_ai) {
      if (!e.self || !e.has_pos) continue;
      ++cars;
      turn += e.turn;
      steer += e.steer_path;
      sum += e.dist;
      mn = std::min(mn, e.dist);
      mx = std::max(mx, e.dist);
    }
    if (cars)
      Log("[hfr] cars: game clock %.2f s; %d AI cars have driven %.1f m on average (%.1f to %.1f); AI input step %.5f s",
          static_cast<double>(g_hfr.clock_now), cars, sum / cars, mn, mx, static_cast<double>(g_ai_in_dt));
    if (g_ai_diff_calls) {
      char line[700];
      int n = snprintf(line, sizeof line, "[hfr] AI output bytes changed by the game before a held call (%u held calls), offset:count", g_ai_diff_calls);
      for (int i = 0; i < kAiOutCensus && n < 660; ++i)
        if (g_ai_diff[i]) n += snprintf(line + n, sizeof line - n, " %02x:%u", i, g_ai_diff[i]);
      Log("%s", line);
      memset(g_ai_diff, 0, sizeof g_ai_diff);
      g_ai_diff_calls = 0;
    }
    if (cars && sum > 1.0)
      Log("[hfr] steering: the cars turned %.1f degrees per km so far, |steer| %.4f averaged over the path: %.1f degrees "
          "per km and unit of steering",
          turn * 57.29578 / (sum / 1000.0), steer / sum, steer > 0 ? turn * 57.29578 / (steer / 1000.0) : 0.0);
    SteerWindowLog();
    if (g_rate != 60.0)
      Log("[hfr] cadence: %.2f s at %.4g, %.2f s at 60 in the race state; to race %u, to base %u; updates through the time "
          "scale %u, wrong path guesses %u, time scale dropouts %u",
          g_hfr.race_s, g_rate, g_hfr.base_s, g_cad_to_race, g_cad_to_base, g_hfr.lever_updates, g_hfr.wrong_guess, g_hfr.dropouts);
  }
  g_hfr.race_s = g_hfr.base_s = 0;
  g_hfr.lever_updates = g_hfr.wrong_guess = g_hfr.dropouts = 0;
  g_cad_to_race = g_cad_to_base = 0;
  g_hfr.t0 = now;
  g_hfr.wall_s = g_hfr.clock_s = 0;
  g_hfr.race_updates = g_hfr.other_updates = g_hfr.held = g_hfr.loop_ticks = g_hfr.hook_ticks = g_hfr.playing_sum = 0;
  g_hfr.normal = g_hfr.playback = HfrClass();
  for (uint32_t& n : g_hfr.playing_by) n = 0;
}

// After an in-race update (or instead of one, when it was held). True when the update went through the race update
// (always true when the wrappers are not in: unknown).
bool HfrAfter(const HfrSnap& s, bool held, bool slowed) {
  const int64_t now = Now();
  if (!g_hfr.t0) g_hfr.t0 = now;
  if (g_hfr.prev_step && Secs(now - g_hfr.prev_step) < 0.25) {
    g_hfr.wall_s += Secs(now - g_hfr.prev_step);
    (g_cad ? g_hfr.race_s : g_hfr.base_s) += Secs(now - g_hfr.prev_step);
  }
  g_hfr.prev_step = now;
  HfrRewind(held);
  uintptr_t race = 0;
  bool race_path = false;
  if (held) {
    ++g_hfr.held;
  } else if (s.ctrl && HfrController(&race) == s.ctrl && race == s.race) {
    race_path = s.hooked ? g_hfr.dir_calls != s.dir : true;
    ++(race_path ? g_hfr.race_updates : g_hfr.other_updates);
    g_hfr.clock_now = Rf(s.ctrl + kCtrlClock);
    if (race_path) HfrLaunchTrace();
    const double clock = double(g_hfr.clock_now) - double(s.clock);
    g_hfr.clock_s += clock;
    g_hfr.loop_ticks += Rd(s.ctrl + kCtrlTickCount) - s.ticks;
    g_hfr.hook_ticks += g_hfr.tick_calls - s.tick;
    g_hfr.playing_sum += s.ntl;
    if (race_path) {
      HfrClass& c = s.playing ? g_hfr.playback : slowed ? g_hfr.slow : g_hfr.normal;
      ++c.updates;
      c.clock_s += clock;
      for (int i = 0; i < s.ntl; ++i) {
        const uintptr_t t = g_tl_at[i];
        if (!Readable(t, 0x10) || Rd(t + 0xC) != 1) continue;  // ended (or gone) during the update
        const uint32_t d = Rd(t) - g_tl_frame[i];
        if (d > 16) continue;  // looped back to 0, or not the same object any more
        ++c.samples;
        c.frames += d;
      }
    }
  }
  if (!slowed && g_hfr.slow.updates) {
    HfrClassLine("slow motion", g_hfr.slow);
    g_hfr.slow = HfrClass();
  }
  HfrWindow(now);
  return race_path;
}

// ---------------------------------------------------------- cadence
//
// Which cadence the step about to run uses (only asked when g_rate is not 60): 1 = RACE (g_rate), 0 = BASE (60).
//
// docs/plan-120fps.md 3.4 (M2). A race whose rewind rings are empty and that the conditions below hold for becomes a
// "fast" race: from then on every update of the race state runs with the game's time scale (0x4AF810: byte 0xD71208,
// float 0xD113B8) at 60/g_rate, so every tick the race update runs, and every rewind record, is 1/g_rate s long. The
// class stays while anything is recorded (the rings never hold ticks of two lengths).
//   RACE cadence: the previous update of the race state went through the race update (the camera director's wrapper
//     was called): one update and one render per 1/g_rate s.
//   BASE cadence: everything else, also inside a fast race (the pause path 0x4B2120, the phase 0x4B0E50 and the
//     controller's own clock take 1/60 s unscaled, so they are right at 60 updates a second).
// A wrong guess costs one update: a paused-path update 1/g_rate s after the last one, or a race update of 1/g_rate s
// after 1/60 s.
constexpr uintptr_t kStateTicks = 0xD66B38;  // a tweakable that overrides the run state's tick count when > 0 (0x417940)

// The rewind requests' seconds-to-ticks factor (docs/plan-120fps.md M4). The crash and takedown shots ask the
// rewind manager for `seconds x [0xC5C124]` ticks (62.5; its three readers are 0xA9B56E, 0xA9B595 and 0xAB2343, all
// of them such requests). In a race in 1/g_rate steps a tick is 60/g_rate of that, so the factor is multiplied by
// g_rate/60 while such a race is the current one and put back for any other: one word in the exe's read-only data,
// through ss_write_word. The requests' caps (360 and 180 ticks, immediates in code) stay: a rewind reaches
// 360/g_rate s. hfr_rewind = off (or SS_PATCH_OFF=hfrrewind) leaves the constant alone.
constexpr uintptr_t kRewindTicksPerSec = 0xC5C124;
constexpr uint32_t kRewindTicksPerSecBits = 0x4279FFFF;  // 62.499996
bool g_rw_on = true;
bool g_rw_scaled = false;

void RewindScale(bool fast) {
  if (!g_rw_on || fast == g_rw_scaled || !Readable(kRewindTicksPerSec, 4)) return;
  float orig;
  memcpy(&orig, &kRewindTicksPerSecBits, 4);
  const float scaled = orig * static_cast<float>(g_rate / 60.0);
  uint32_t want;
  memcpy(&want, fast ? &scaled : &orig, 4);
  const uint32_t have = Rd(kRewindTicksPerSec);
  if (fast && have != kRewindTicksPerSecBits) {
    static bool said = false;
    if (!said) Log("[hfr] rewind: the seconds-to-ticks constant is %08x, not the known 62.5: left alone", have);
    said = true;
    return;
  }
  if (!ss_write_word(reinterpret_cast<void*>(kRewindTicksPerSec), want)) {
    static bool said = false;
    if (!said) Log("[hfr] rewind: cannot write the seconds-to-ticks constant (error %lu)", GetLastError());
    said = true;
    return;
  }
  g_rw_scaled = fast;
  Log("[hfr] rewind: seconds-to-ticks constant %.4f (%s)", static_cast<double>(fast ? scaled : orig),
      fast ? "ticks of this race are 1/fps s" : "back to the game's value");
}

// The steering lock (docs/plan-120fps.md 12.4). The car's steering part (vehicle+0x550, 0x8D38E0, once a tick with
// the physics step dt) turns the wheels by steer x lock degrees, lock = 0x8D3830(..):
//   lock = curve[cfg+0](a4) + atan(min(1, a1 x mass / max(mass x |forward speed| / dt, 0.1))) x 57.3
//        = curve(..) + atan(a1 x dt / |v|),   a1 = curve[cfg+0x90 | +0xD8](yaw rate) x [cfg+0x20C] + [vehicle+0x124]
// (cfg = [vehicle+0x550] = driving model config + 0xEA0, shared by the cars of a model). The step is in the lock
// itself: at 1/120 s the wheels turn half as far for the same steer, which is the 0.49..0.56 of section 12.3. While
// an update runs in 1/g_rate steps, both terms of a1 are multiplied by g_rate/60 (a1 x dt is then what it is at 60),
// and put back for every other update and race. Heap words of the game's data, written through ss_write_word.
// hfr_steering = off (or SS_PATCH_OFF=hfrsteer) leaves them alone.
constexpr uintptr_t kRaceVehicleMgr = 0x50;  // race -> vehicle manager (0x8C4340): +4 .. +0x80 = 32 vehicle pointers
constexpr uintptr_t kVehSteerPart = 0x550, kVehLockAdd = 0x124, kVehBody = 0x584;
constexpr uintptr_t kSteerCfgOff = 0xEA0, kCfgLockGain = 0x20C;
bool g_steer_on = true;
struct SteerWord {
  uintptr_t at = 0;
  uint32_t orig = 0, made = 0;
  bool per_race = false;  // a vehicle field (the vehicle goes with its race); config words are kept
};
constexpr int kSteerWords = 192;
SteerWord g_sw[kSteerWords];
int g_sw_n = 0;
struct SteerStats {
  uintptr_t race = 0;
  uint32_t writes = 0, adopted = 0, cars = 0, toggles = 0;
  bool scaled = false, said_full = false;
} g_ss;

uint32_t FloatBits(float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  return u;
}
float BitsFloat(uint32_t u) {
  float f;
  memcpy(&f, &u, 4);
  return f;
}

void SteerWordSet(uintptr_t at, bool per_race, bool want, float s) {
  if (!Readable(at, 4)) return;
  const uint32_t cur = Rd(at);
  // Not a number of the game yet (the vehicle's 0xDEADBEEF fill before the race zeroes it): left alone.
  const float f = BitsFloat(cur);
  if (!(f > -1e6f && f < 1e6f)) return;
  SteerWord* e = nullptr;
  for (int i = 0; i < g_sw_n; ++i)
    if (g_sw[i].at == at) { e = &g_sw[i]; break; }
  if (!e) {
    if (!want) return;  // never touched: the game's value
    if (g_sw_n == kSteerWords) {
      if (!g_ss.said_full) Log("[hfr] steering: table full, further words left alone");
      g_ss.said_full = true;
      return;
    }
    e = &g_sw[g_sw_n++];
    e->at = at;
    e->orig = cur;
    e->made = FloatBits(BitsFloat(cur) * s);
    e->per_race = per_race;
  } else if (cur != e->orig && cur != e->made) {  // the game wrote it since (a config reload): its new value
    e->orig = cur;
    e->made = FloatBits(BitsFloat(cur) * s);
    ++g_ss.adopted;
  }
  const uint32_t to = want ? e->made : e->orig;
  if (cur != to && ss_write_word(reinterpret_cast<void*>(at), to)) ++g_ss.writes;
}

// SS_PATCH_LOG=1: each window, the lock's two words of the cars as the game holds them now (at 60 as well: read
// only), and how often a word the patch had written was found rewritten by the game.
void SteerWindowLog() {
  const uintptr_t race = Rd(kRaceRun);
  if (!race || !Readable(race + kRaceVehicleMgr, 4)) return;
  const uintptr_t mgr = Rd(race + kRaceVehicleMgr);
  if (!mgr || !Readable(mgr, 0x84)) return;
  char line[600];
  int n = snprintf(line, sizeof line, "[hfr] steering words (lock add / gain) now:");
  for (int i = 0; i < 32 && n < 520; ++i) {
    const uintptr_t v = Rd(mgr + 4 + 4 * i);
    if (!v || !Readable(v, kVehBody + 4)) continue;
    const uintptr_t model = Rd(v), cfg = Rd(v + kVehSteerPart);
    if (!model || cfg != model + kSteerCfgOff || !Readable(cfg, 0x268)) continue;
    n += snprintf(line + n, sizeof line - n, " %d:%.4g/%.4g", i, static_cast<double>(Rf(v + kVehLockAdd)),
                  static_cast<double>(Rf(cfg + kCfgLockGain)));
  }
  Log("%s; %u words written, %u found rewritten by the game", line, g_ss.writes, g_ss.adopted);
}

// want: this update runs in 1/g_rate steps. Called before every update of the race state above 60.
void SteerScale(uintptr_t race, bool want) {
  if (!g_steer_on || !race || !Readable(race + kRaceVehicleMgr, 4)) return;
  const uintptr_t mgr = Rd(race + kRaceVehicleMgr);
  if (!mgr || !Readable(mgr, 0x84)) return;
  const bool fresh = race != g_ss.race;
  if (fresh) {  // the old race's vehicles are gone: forget their words, without writing
    int k = 0;
    for (int i = 0; i < g_sw_n; ++i)
      if (!g_sw[i].per_race) g_sw[k++] = g_sw[i];
    g_sw_n = k;
    g_ss.race = race;
    g_ss.cars = 0;
    g_ss.toggles = 0;
  }
  const float s = static_cast<float>(g_rate / 60.0);
  uint32_t cars = 0;
  for (int i = 0; i < 32; ++i) {
    const uintptr_t v = Rd(mgr + 4 + 4 * i);
    if (!v || !Readable(v, kVehBody + 4)) continue;
    const uintptr_t model = Rd(v), cfg = Rd(v + kVehSteerPart);
    if (!model || cfg != model + kSteerCfgOff || !Rd(v + kVehBody) || !Readable(cfg, 0x268)) continue;  // not a car
    ++cars;
    SteerWordSet(cfg + kCfgLockGain, false, want, s);
    SteerWordSet(v + kVehLockAdd, true, want, s);
  }
  if (cars != g_ss.cars || (want != g_ss.scaled && g_ss.toggles < 8)) {
    if (want != g_ss.scaled) ++g_ss.toggles;
    Log("[hfr] steering: race %p, %u cars: lock gain %s (%u words written so far, %u game rewrites adopted)",
        reinterpret_cast<void*>(race), cars, want ? "x fps/60 (updates in 1/fps steps)" : "the game's", g_ss.writes, g_ss.adopted);
  }
  g_ss.scaled = want;
  if (cars != g_ss.cars) {
    g_ss.cars = cars;
    {  // what the lock is made of, per car (the game's values, before scaling)
      for (int i = 0; i < 32; ++i) {
        const uintptr_t v = Rd(mgr + 4 + 4 * i);
        if (!v || !Readable(v, kVehBody + 4)) continue;
        const uintptr_t model = Rd(v), cfg = Rd(v + kVehSteerPart);
        if (!model || cfg != model + kSteerCfgOff || !Readable(cfg, 0x268)) continue;
        auto orig = [](uintptr_t at) {
          for (int k = 0; k < g_sw_n; ++k)
            if (g_sw[k].at == at) return BitsFloat(g_sw[k].orig);
          return Rf(at);
        };
        const uintptr_t c0 = cfg, c90 = cfg + 0x90, cd8 = cfg + 0xD8;
        Log("[hfr] steering car %d: vehicle %p model %p: lock add %.4g, gain %.4g; rate %.4g/%.4g, max %.4g; curve scales "
            "base %.4g, yaw %.4g/%.4g (now %.4g, %.4g)",
            i, reinterpret_cast<void*>(v), reinterpret_cast<void*>(model), static_cast<double>(orig(v + kVehLockAdd)),
            static_cast<double>(orig(cfg + kCfgLockGain)), static_cast<double>(Rf(cfg + 0x200)),
            static_cast<double>(Rf(cfg + 0x204)), static_cast<double>(Rf(cfg + 0x1F8)), static_cast<double>(Rf(c0 + 0x1C)),
            static_cast<double>(Rf(c90 + 0x1C)), static_cast<double>(Rf(cd8 + 0x1C)), static_cast<double>(Rf(v + kVehLockAdd)),
            static_cast<double>(Rf(cfg + kCfgLockGain)));
      }
    }
  }
}

struct HfrStep {
  int cad = 0;
  bool lever = false;  // run the update with the time scale at 60/g_rate
  uintptr_t ctrl = 0, race = 0;
};

HfrStep HfrStepFor(void* self) {
  HfrStep hs;
  // Test aid SS_PATCH_MODE=present120: presents and updates at g_rate in the front end only, one tick each, nothing
  // else changed: does the harness reach the rate, and what do the menus do with twice the ticks.
  if (g_fe_test) {
    hs.cad = self == reinterpret_cast<void*>(static_cast<uintptr_t>(kStates[7].obj)) ? 1 : 0;
    return hs;
  }
  if (self != reinterpret_cast<void*>(static_cast<uintptr_t>(kStates[5].obj))) {  // not cInGameRunState
    g_hfr.last_race_path = false;
    return hs;
  }
  const uintptr_t m = Rd(kRewindMgr);
  if (m != g_hfr.class_mgr) {
    g_hfr.class_mgr = m;
    g_hfr.fast = false;
  }
  uintptr_t ctrl = 0, race = 0;
  bool ready = !g_force_base && m && Readable(m, 0x138) && HalfLiveReady(&ctrl, &race) && Readable(ctrl, kCtrlTickCount + 4) &&
               Rd(race + kRaceRewind) == m && static_cast<int32_t>(Rd(kStateTicks)) <= 0;
  if (ready) {  // one local player, not a network mode (the reads of the controller's one-tick decision)
    const uintptr_t settings = Rd(race + 0x14), mode = Rd(kModeObj);
    ready = settings && Readable(settings + 0x9C, 4) && Rd(settings + 0x9C) == 1 && mode && Readable(mode + 8, 4) &&
            Rd(mode + 8) != 2 && Rd(mode + 8) != 3;
  }
  ready = ready && HfrSwap(ctrl);
  if (m && Readable(m, 0x138) && Rd(m + 0x11C) && Rd(m + 0x120) && Rd(m + 0x128) == 0 && Rd(m + kRewindPlaying) == 0 &&
      g_hfr.fast != ready) {
    g_hfr.fast = ready;  // nothing recorded yet: the class can still be chosen
    Log("[hfr] race (rewind manager %p): %s", reinterpret_cast<void*>(m),
        ready ? "simulation in 1/fps steps through the game's time scale" : "left at 60 (not a single-player race, or not the known build)");
  }
  RewindScale(g_hfr.fast);
  SteerScale(race ? race : Rd(kRaceRun), g_hfr.fast && ready);
  if (!g_hfr.fast) {
    g_hfr.last_race_path = false;
    return hs;
  }
  if (!ready) {  // a fast race, but the scale cannot be vouched for now: 60, unscaled (logged in the [hfr] cadence line)
    ++g_hfr.dropouts;
    g_hfr.last_race_path = false;
    return hs;
  }
  hs.lever = true;
  hs.ctrl = ctrl;
  hs.race = race;
  hs.cad = g_hfr.last_race_path ? 1 : 0;
  return hs;
}

// Our replacement for App::cUpdateRenderState's frame step (slot 2 of the eight state vtables). thiscall, no stack
// arguments, returns the next state like the original.
uint32_t ENTRY StepReplacement(void* self) {
  uint32_t* vt = *reinterpret_cast<uint32_t**>(self);
  const uint32_t* orig = nullptr;
  for (int k = 0; k < 8; ++k)
    if (vt == &g_vt[k][1]) {
      orig = reinterpret_cast<const uint32_t*>(kStates[k].vt);
      ss_game_state = k;  // pipewarm.cpp: the level load (1) and the race (5)
      break;
    }
  if (!orig) return reinterpret_cast<StepFn>(kStepReal)(self);  // cannot happen: we were reached through our own vtable

  if (!g_sig_checked) {
    if (!g_freq) {
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      g_freq = f.QuadPart;
    }
    CheckCode();
  }
  const int mode = g_mode;
  if (mode != MODE_SIXTY || !g_code_ok) g_rc.tid = 0;  // the readable-region cache only runs with our 60-and-above step
  if (mode == MODE_PASS || mode == MODE_OFF || !g_code_ok) return reinterpret_cast<StepFn>(orig[2])(self);

  const bool sixty = mode == MODE_SIXTY;
  if (sixty) RegionsForget(true);  // the readable-region cache: a new frame (main thread, 60 and above only)
  // The cadence of this step. A change starts a new grid one period of the new cadence after the last present and
  // gives this update one tick.
  bool cad_changed = false;
  HfrStep hs;
  if (sixty && g_rate != 60.0) {
    hs = HfrStepFor(self);
    const int cad = hs.cad;
    if (cad != g_cad) {
      g_cad = cad;
      cad_changed = true;
      const int64_t last = g_last_present;
      if (last) g_next_present = last + PeriodQpc();
      ++(cad ? g_cad_to_race : g_cad_to_base);
    }
  } else if (g_cad) {
    g_cad = 0;
  }
  if (g_acc_mode != mode) {  // (re)start from the limiter state the previous mode left in the game's globals
    float a;
    memcpy(&a, reinterpret_cast<const void*>(kAcc), 4);
    g_acc = a;
    g_behind = 0;
    g_acc_mode = mode;
    if (sixty && g_timer_on) timeBeginPeriod(1);
    Log("step: mode %d (%s)", mode, sixty ? "60 Hz, one tick a period" : "game arithmetic, 1/30 s, two ticks a period");
    if (!g_crash_armed) {
      g_crash_armed = 1;  // the game's frames run: from here an access violation and the like is logged (CrashLog)
      Log("crash log armed (an exception that ends the game leaves an EXCEPTION line here)");
    }
    if (sixty && g_rate != 60.0) Log("step: target %.4g Hz where the cadence allows it, 60 Hz elsewhere; one tick a period", g_rate);
  }

  int64_t t_step = Now();
  g_loop_tid = 0;
  if (sixty) {
    InstallLoopSleepHook();
    if (g_frame_log && g_step_exit) g_fs.out.Add(Secs(t_step - g_step_exit));
  }
  g_step_exit = 0;
  if (g_stats) {
    if (!g_st.t0) g_st.t0 = t_step;
    if (g_st.t_after) g_st.out_s += Secs(t_step - g_st.t_after);
  }

  // 1. the frame number, with the original's single-frame rule
  uint32_t frame = Rd(kFrameNo);
  uint32_t last = reinterpret_cast<uint32_t*>(self)[2];
  if (frame == last + 1) frame += 1;
  const uint32_t maxticks = Rd(kMaxTicks);

  // 2. publish dt (12 other readers of kDtOut)
  reinterpret_cast<DtFn>(kDtFn)();
  float dtf;
  memcpy(&dtf, reinterpret_cast<const void*>(kDtOut), 4);
  float pf;
  memcpy(&pf, reinterpret_cast<const void*>(kPeriod), 4);
  const double P = sixty ? 1.0 / CadRate() : double(pf);
  const uint32_t per = sixty ? 1 : 2;

  // 3. the limiter
  double acc = g_acc + double(dtf);
  if (sixty && g_pace_present && Vsynced() && !g_vrr) acc -= 0.02 * g_acc;  // vsync: the accumulator leaks (see Vsynced)
  if (cad_changed) acc = 0.5 * P;  // one tick now, whatever the last frame of the other cadence took
  uint32_t ticks = 0;
  uint32_t behind = g_behind;
  double waited = 0;  // the [frames] split: this step's wait
  if (sixty && PresentPaced()) {
    // The presents run on the 1/60 s grid, so a frame's dt is one period give or take the jitter: count ticks at
    // half a period (acc stays within +-P/2, one tick a paced frame, catch-up ticks after a slow one) and wait
    // only until the next present's grid point minus the expected work.
    while (acc >= 0.5 * P) {
      ++ticks;
      acc -= P;
      ++behind;
    }
    if (ticks == 0) {
      ticks = 1;
      acc -= P;
      if (acc < -P) acc = -P;
    }
    const int64_t next = g_next_present;
    if (next) {
      const int64_t w0 = Now();
      const int64_t margin = int64_t((g_work_est + 0.0003) * double(g_freq));
      WaitUntil(next - margin);
      waited = Secs(Now() - w0);
      if (g_stats) g_st.wait_s += waited;
    }
    g_work_t0 = Now();
    if (ticks == 1) behind = 0;
  }
  while (acc >= P) {
    ticks += per;
    acc -= P;
    ++behind;
  }
  if (ticks == 0) {
    int64_t w0 = Now();
    if (sixty) {
      WaitSeconds(P - acc);
    } else {
      Sleep(static_cast<DWORD>((P - acc) * 1000.0));  // truncated, as the game does
    }
    waited = Secs(Now() - w0);
    if (g_stats) g_st.wait_s += waited;
    acc -= P;  // the period is credited; the next dt measures what really passed
    ticks = per;
    behind = 0;
  }
  if (!sixty && behind >= 5) Sleep(5);  // the game's own "behind" yield (dropped at 60)
  g_acc = acc;
  g_behind = behind;
  {
    float a = float(acc);
    memcpy(reinterpret_cast<void*>(kAcc), &a, 4);
    Wr(kBehind, behind);
  }
  uint32_t cap = maxticks;
  if (sixty && g_maxticks60 < cap) cap = g_maxticks60;
  // The catch-up clamp is a time (4 ticks of 1/60 s): as many ticks of the cadence as cover it.
  if (sixty && g_cad) cap = std::min(maxticks, static_cast<uint32_t>(ceil(g_maxticks60 * g_rate / 60.0)));
  if (ticks < 1) ticks = 1;
  if (ticks > cap) ticks = cap;

  // 4. frame number, message pump and input exactly as the original does
  reinterpret_cast<uint32_t*>(self)[2] = frame;
  reinterpret_cast<PumpFn>(kPumpFn)(reinterpret_cast<void*>(Rd(kAppSingleton)));

  // 5. heap fix-ups on the main thread, before the update
  g_ticks = ticks;
  if (sixty) {
    static uint32_t n = 0;
    if (g_ai_on && g_ai_vt_ready && (RaceChanged() || ++n % 6 == 0)) SwapAiPlayers();
    if (g_rings_on && g_alloc_ok) FixRings();
  }

  // 6. the game's own update and render through the original vtable
  int64_t t_upd = Now();
  uint32_t next;
  bool held = false;
  const float scale = hs.lever ? static_cast<float>(60.0 / g_rate) : 1.0f;
  SlowPlan plan = SlowDecide(self, sixty, ticks, hs.lever ? 60.0 / g_rate : 1.0, hs.lever);
  const bool measured = g_hfr.on && sixty && self == reinterpret_cast<void*>(static_cast<uintptr_t>(kStates[5].obj));
  HfrSnap snap;
  if (measured) HfrBefore(snap);
  if (plan.action == SLOW_HOLD) {
    next = g_slow.last_next;  // held: the world stands still for this frame, the state does not change
    held = true;
  } else {
    const UpdateFn update = reinterpret_cast<UpdateFn>(orig[5]);
    if (plan.action != SLOW_RUN) {
      // a half step: half the tick time (of the race's own scale)
      TlSwap(plan.race);
      g_tl_units = 0.5 * scale;
      g_ai_frac = hs.lever;
      g_ai_units = double(plan.clock_ticks) * 0.5 * scale;
      next = HalfStepUpdate(update, self, ticks, plan, 0.5f * scale, true);
    } else if (hs.lever) {
      // a fast race: the tick is 1/g_rate s; the clock is put back when the race update is expected to run
      plan.ctrl = hs.ctrl;
      plan.race = hs.race;
      TlSwap(plan.race);
      g_tl_units = scale;
      g_ai_frac = true;
      g_ai_units = double(plan.clock_ticks) * scale;
      ++g_hfr.lever_updates;
      next = HalfStepUpdate(update, self, ticks, plan, scale, hs.cad == 1);
    } else {
      next = update(self, ticks);
    }
    g_ai_frac = false;
    g_tl_units = 1.0;
    ss_aspect_after_update();
    ss_postfx_after_update();
    g_slow.last_next = next;
    g_slow.last_self = self;
  }
  if (measured) {
    const bool race_path = HfrAfter(snap, held, g_slow.on);
    if (hs.lever && !held) {
      g_hfr.last_race_path = race_path;
      const bool expected = hs.cad == 1 || plan.action != SLOW_RUN;
      if (race_path != expected) {
        // The other path ran: the clock gets (or loses) what the guess took from it (or left on it).
        ++g_hfr.wrong_guess;
        if (HalfRaceStillLive(plan)) {
          volatile float* clock = reinterpret_cast<volatile float*>(plan.ctrl + kCtrlClock);
          const float used = plan.action != SLOW_RUN ? 0.5f * scale : scale;
          const float part = float(plan.clock_ticks) * ((1.0f - used) / 60.0f);
          *clock = race_path ? *clock - part : *clock + part;
        }
      }
    }
  }
  int64_t t_updated = Now();
  if (sixty) RegionsForget(true);  // the update may have freed memory: the render phase checks again
  double rendered = 0;  // the [frames] split: the render without the present hook in it
  static const bool render_held = [] { const char* e = getenv("SS_HOLD_RENDER"); return e && *e == '1'; }();
  if (held && !render_held) {
    // A held frame is not rendered again: a render with no update in between drew a slightly different picture
    // (motion blur and particles with no movement), every second frame, which flickered. The last picture stays
    // on the screen for one more period instead, as a slowed frame does at thirty.
    if (PresentPaced()) {
      const int64_t target = g_next_present;
      if (target) WaitUntil(target);
      g_next_present = (target ? target : Now()) + PeriodQpc();
      g_last_present = Now();
      g_work_t0 = 0;
      FrameStatsHeld(g_last_present, CadRate());
    }
  } else {
    const bool split = sixty && g_frame_log;
    const int64_t t_r0 = split ? Now() : 0;
    g_present_hook_s = 0;
    reinterpret_cast<RenderFn>(orig[6])(self);
    if (split) rendered = Secs(Now() - t_r0) - g_present_hook_s;
  }
  if (sixty && g_frame_log) {
    ++g_fs.steps;
    g_fs.wait.Add(waited);
    g_fs.upd.Add(Secs(t_updated - t_upd));
    g_fs.rnd.Add(rendered);
  }

  if (g_stats) {
    g_st.upd_s += Secs(t_updated - t_upd);
    g_st.t_after = Now();
    StatsReport(ticks);
  }
  if (sixty) {
    g_step_exit = Now();
    g_loop_tid = GetCurrentThreadId();
  }
  return next;
}

// ---------------------------------------------------------- installation

// Points the state object's vptr at its private vtable copy. Returns 0 (not yet), 1 (already ours), 2 (done now),
// -1 (the vtable in the read-only data is not the known one).
int InstallState(int k) {
  const StateDef& d = kStates[k];
  const uint32_t mine = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_vt[k][1]));
  uint32_t cur = Rd(d.obj);
  if (cur == mine) return 1;
  if (cur != d.vt) return 0;  // not constructed yet
  if (!g_vt_ready[k]) {
    if (!Readable(d.vt - 4, 16 * 4) || Rd(d.vt + 8) != kStepThunk || Rd(d.vt + 0x14) != d.update || Rd(d.vt + 0x18) != d.render)
      return -1;
    for (int i = 0; i < 16; ++i) g_vt[k][i] = Rd(d.vt - 4 + 4 * i);
    g_vt[k][1 + 2] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&StepReplacement));
    g_vt_ready[k] = true;
  }
  Wr(d.obj, mine);  // one aligned dword store into the game's writable data
  return 2;
}

// ------------------------------------------------------ thread monitor (tests)

struct ThreadTimes {
  DWORD id;
  ULONGLONG user;
};

int g_threads = 0, g_spinners = 0;

void Monitor() {
  static ThreadTimes prev[1024];
  static int nprev = 0;
  static ULONGLONG prev_t = 0;
  static ThreadTimes cur[1024];
  int n = 0;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te;
  te.dwSize = sizeof te;
  const DWORD pid = GetCurrentProcessId();
  if (Thread32First(snap, &te)) {
    do {
      if (te.th32OwnerProcessID != pid || n >= 1024) continue;
      HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
      FILETIME c, e, k, u;
      ULONGLONG user = 0;
      if (h) {
        if (GetThreadTimes(h, &c, &e, &k, &u)) user = (ULONGLONG(u.dwHighDateTime) << 32) | u.dwLowDateTime;
        CloseHandle(h);
      }
      cur[n].id = te.th32ThreadID;
      cur[n].user = user;
      ++n;
    } while (Thread32Next(snap, &te));
  }
  CloseHandle(snap);
  FILETIME now_ft;
  GetSystemTimeAsFileTime(&now_ft);
  ULONGLONG now = (ULONGLONG(now_ft.dwHighDateTime) << 32) | now_ft.dwLowDateTime;
  int spin = 0;
  if (prev_t) {
    double wall = double(now - prev_t);
    for (int i = 0; i < n; ++i)
      for (int j = 0; j < nprev; ++j)
        if (prev[j].id == cur[i].id) {
          if (wall > 0 && double(cur[i].user - prev[j].user) / wall > 0.8) ++spin;
          break;
        }
  }
  memcpy(prev, cur, sizeof(ThreadTimes) * n);
  nprev = n;
  prev_t = now;
  g_threads = n;
  g_spinners = spin;
  Log("[threads] %d threads in the process, %d spinning (>80%% of a core over the last interval)", n, spin);
}

// ---- test keys: SS_PATCH_KEYS="75:Return,80:Return,90:Right" (seconds since start : key), sent with SendInput
struct KeyDef { const char* name; WORD scan; bool ext; };
const KeyDef kKeys[] = {{"Return", 0x1C, false}, {"Escape", 0x01, false}, {"space", 0x39, false}, {"Up", 0x48, true},
                        {"Down", 0x50, true},   {"Left", 0x4B, true},    {"Right", 0x4D, true},  {"Tab", 0x0F, false},
                        {"y", 0x15, false},     {"n", 0x31, false},      {"w", 0x11, false},     {"s", 0x1F, false},
                        {"F1", 0x3B, false},    {"q", 0x10, false},      {"e", 0x12, false},     {"Back", 0x0E, false},
                        {"Delete", 0x53, true},  {"End", 0x4F, true},     {"Home", 0x47, true}};

// "type=<letters and digits>": each character as its key's scan code (US layout), for the game's text fields (the
// online login dialog). Other characters are skipped.
WORD TypeScan(char c) {
  static const char* rows[] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
  static const WORD first[] = {0x02, 0x10, 0x1E, 0x2C};
  if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  for (int r = 0; r < 4; ++r)
    if (const char* q = strchr(rows[r], c)) return static_cast<WORD>(first[r] + (q - rows[r]));
  return 0;
}

// Test aid: the keys go to the foreground window; when that is not the game's (a console or another window took the
// focus), the game's own top-level window is brought forward first (logged).
BOOL CALLBACK FindOwnWindow(HWND h, LPARAM out) {
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
  *reinterpret_cast<HWND*>(out) = h;
  return FALSE;
}

void EnsureFocus() {
  DWORD pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &pid);
  if (pid == GetCurrentProcessId()) return;
  HWND own = nullptr;
  EnumWindows(FindOwnWindow, reinterpret_cast<LPARAM>(&own));
  if (!own) { Log("[keys] the foreground window is process %lu's and the game has no visible window", pid); return; }
  // A process may take the foreground only in some cases; a synthetic Alt press makes it the last input's owner.
  INPUT alt[2] = {};
  alt[0].type = alt[1].type = INPUT_KEYBOARD;
  alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
  alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
  SendInput(2, alt, sizeof(INPUT));
  const BOOL ok = SetForegroundWindow(own);
  Sleep(300);
  DWORD now = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &now);
  Log("[keys] the foreground window was process %lu's: game window brought forward (%s)", pid,
      ok && now == GetCurrentProcessId() ? "done" : "FAILED");
}

void SendKey(const KeyDef& k, DWORD hold_ms) {
  EnsureFocus();
  INPUT in[2] = {};
  in[0].type = INPUT_KEYBOARD;
  in[0].ki.wScan = k.scan;
  in[0].ki.dwFlags = KEYEVENTF_SCANCODE | (k.ext ? KEYEVENTF_EXTENDEDKEY : 0);
  SendInput(1, in, sizeof(INPUT));
  Sleep(hold_ms);
  in[1] = in[0];
  in[1].ki.dwFlags |= KEYEVENTF_KEYUP;
  SendInput(1, in + 1, sizeof(INPUT));
}

void SetMode(int m, const char* why) {
  g_mode = m;
  Log("[live] mode %d (%s)", m, why);
}

void PressByName(std::string name) {
  DWORD hold = 150;
  size_t star = name.find('*');
  if (star != std::string::npos) { hold = atoi(name.c_str() + star + 1); name.resize(star); }
  while (!name.empty() && (name.back() == '\r' || name.back() == '\n' || name.back() == ' ')) name.pop_back();
  if (name.size() > 1 && (name[0] == '+' || name[0] == '-')) {  // +Up: key down, -Up: key up
    for (const auto& k : kKeys) {
      if (name.compare(1, std::string::npos, k.name) == 0) {
        if (name[0] == '+') EnsureFocus();
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wScan = k.scan;
        in.ki.dwFlags = KEYEVENTF_SCANCODE | (k.ext ? KEYEVENTF_EXTENDEDKEY : 0) | (name[0] == '-' ? KEYEVENTF_KEYUP : 0);
        SendInput(1, &in, sizeof(INPUT));
        Log("[keys] %s", name.c_str());
      }
    }
    return;
  }
  // live mode switch (the states must have been taken over: start with SS_PATCH_MODE=pass or fps = 60)
  if (name == "pass") { SetMode(MODE_PASS, "pass-through"); return; }
  if (name == "fps30") { SetMode(MODE_OWN30, "game arithmetic at 30"); return; }
  if (name == "fps60") { SetMode(MODE_SIXTY, "60 Hz"); return; }
  if (name == "ai0") { g_ai_gate = false; Log("[live] AI gate off"); return; }
  if (name == "ai1") { g_ai_gate = true; Log("[live] AI gate on"); return; }
  if (name.compare(0, 8, "slowtest") == 0) {
    g_slow_test_until = Now() + int64_t(atof(name.c_str() + 8) * double(g_freq));
    Log("[live] slow motion test: the cameras' one-tick request assumed for %.1f s", atof(name.c_str() + 8));
    return;
  }
  if (name == "cad60") { g_force_base = true; Log("[live] cadence forced to 60, time scale off (races started from now on stay at 60)"); return; }
  if (name == "cadrace") { g_force_base = false; Log("[live] cadence by the race's class again"); return; }
  if (name == "shot") { ss_testshot_ask(); Log("[keys] shot asked"); return; }
  if (name.compare(0, 4, "wait") == 0) { Sleep(static_cast<DWORD>(atof(name.c_str() + 4) * 1000)); return; }
  if (name.compare(0, 5, "type=") == 0) {
    int n = 0;
    for (size_t i = 5; i < name.size(); ++i) {
      const WORD sc = TypeScan(name[i]);
      if (!sc || !name[i]) continue;
      SendKey(KeyDef{"", sc, false}, 80);
      Sleep(80);
      ++n;
    }
    Log("[keys] typed %d characters", n);
    return;
  }
  for (const auto& k : kKeys) {
    if (name == k.name) {
      SendKey(k, hold);
      Log("[keys] %s (held %lu ms)", k.name, (unsigned long)hold);
      return;
    }
  }
}

// Keys and live commands come from SS_PATCH_KEYS (a schedule) and from the file splitsecond-keys.txt beside the DLL,
// one name per line, read while the game runs (a test aid: `echo fps30 >> splitsecond-keys.txt`).
// An item "@N" waits (at most 10 minutes) until the game state N (kStates index: 7 front end, 1 level load, 5 race)
// runs a frame step, and the times of the items after it count from that moment; "@N+S" also waits S seconds more.
// "<t>:?N<key>" sends the key only while the game state is N.
// With env.SS_PATCH_KEYS = ... in splitsecond.conf the schedule reaches the restarted Windows process.
DWORD WINAPI KeyThread(LPVOID) {
  std::string path = std::string(g_dir) + "splitsecond-keys.txt";
  long done = 0;
  // GetEnvironmentVariable, not getenv: env.SS_PATCH_KEYS from the conf is set after the C runtime copied the
  // environment, and only the Win32 block has it. Else the conf key test_keys (a game started by Steam has no
  // environment).
  static char keys_buf[8192];
  const DWORD kn = GetEnvironmentVariableA("SS_PATCH_KEYS", keys_buf, sizeof keys_buf);
  std::string list = kn && kn < sizeof keys_buf ? std::string(keys_buf) : ConfValue("test_keys");
  DWORD t0 = GetTickCount();
  size_t pos = 0;
  for (;;) {
    if (FILE* f = fopen(path.c_str(), "r")) {
      fseek(f, done, SEEK_SET);
      char line[128];
      while (fgets(line, sizeof line, f)) {
        if (!strchr(line, '\n')) break;  // a half-written line: later
        done = ftell(f);
        PressByName(line);
        Sleep(120);
      }
      fclose(f);
    }
    if (pos < list.size()) {
      size_t comma = list.find(',', pos);
      std::string item = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
      if (!item.empty() && item[0] == '@') {  // an anchor: wait for the game state, then count from there
        const int want = atoi(item.c_str() + 1);
        const size_t plus = item.find('+');
        const DWORD extra = plus == std::string::npos ? 0 : static_cast<DWORD>(atof(item.c_str() + plus + 1) * 1000.0);
        const DWORD w0 = GetTickCount();
        while (ss_game_state != want && GetTickCount() - w0 < 600000) Sleep(20);
        Log("[keys] state %d %s after %.1f s (anchor %s)", want, ss_game_state == want ? "reached" : "NOT reached",
            (GetTickCount() - w0) / 1000.0, item.c_str());
        Sleep(extra);
        t0 = GetTickCount();
        pos = comma == std::string::npos ? list.size() : comma + 1;
        continue;
      }
      size_t colon = item.find(':');
      DWORD at = colon == std::string::npos ? 0 : static_cast<DWORD>(atof(item.substr(0, colon).c_str()) * 1000.0);
      if (GetTickCount() - t0 >= at) {
        pos = comma == std::string::npos ? list.size() : comma + 1;
        std::string what = colon == std::string::npos ? std::string() : item.substr(colon + 1);
        if (what.size() > 1 && what[0] == '?') {  // "?N<key>": only while the game state is N (else skipped, logged)
          const int want = atoi(what.c_str() + 1);
          size_t k = 1;
          while (k < what.size() && isdigit(static_cast<unsigned char>(what[k]))) ++k;
          if (ss_game_state != want) {
            Log("[keys] %s skipped: game state %d", item.c_str(), static_cast<int>(ss_game_state));
            what.clear();
          } else {
            what = what.substr(k);
          }
        }
        if (!what.empty()) PressByName(what);
      }
    }
    Sleep(100);
  }
  return 0;
}

// ------------------------------------------------------------------- worker

// The crash line. Always on (no environment variable): armed once the game's frames run (the first step), so the
// protector's and Wine's first-chance exceptions while the game starts are not seen; with SS_PATCH_LOG=1 armed at
// once, as before. Only the codes that end a game (access violation, stack overflow, privileged instruction,
// integer division by zero; not the illegal instructions the protector raises on purpose), each address once, at most
// kCrashLines exceptions a run. It only observes (EXCEPTION_CONTINUE_SEARCH). Safe inside a crash: no stdio, no heap, no loader lock. The line is formatted on the
// stack and written with WriteFile to the log's own handle (each line of the log is flushed when written), so a
// crash while another thread holds the log's FILE lock, the heap lock or the loader lock does not wait for it; a
// module is named from its PE export directory, found with VirtualQuery.
constexpr LONG kCrashLines = 6;
volatile LONG g_crash_n = 0;
HANDLE g_log_handle = INVALID_HANDLE_VALUE;
uintptr_t g_crash_seen[kCrashLines];

// "name+offset" for an address in an image (its PE export name), else "?".
void CrashWhere(uintptr_t a, char* out, size_t n) {
  out[0] = '?';
  out[1] = 0;
  MEMORY_BASIC_INFORMATION mbi;
  if (a < 0x10000 || !SS_VQ(reinterpret_cast<void*>(a), &mbi, sizeof mbi) || mbi.Type != MEM_IMAGE) return;
  const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
  if (!Readable(base, 0x40) || (Rd(base) & 0xFFFF) != 0x5A4D) return;
  const uintptr_t nt = base + Rd(base + 0x3C);
  const char* name = "image";
  if (Readable(nt, 0x80) && Rd(nt) == 0x4550) {
    const uint32_t exp = Rd(nt + 0x78);  // the export directory's RVA (IMAGE_OPTIONAL_HEADER32 + 0x60)
    if (exp && Readable(base + exp, 0x28)) {
      const uintptr_t nm = base + Rd(base + exp + 0xC);
      if (Readable(nm, 64)) name = reinterpret_cast<const char*>(nm);
    }
  }
  if (base == 0x400000) name = "SplitSecond.exe";
  snprintf(out, n, "%.40s+%lx", name, static_cast<unsigned long>(a - base));
}

void CrashWrite(const char* text) {
  if (g_log_handle == INVALID_HANDLE_VALUE || !g_log_handle) return;
  DWORD w = 0;
  WriteFile(g_log_handle, text, static_cast<DWORD>(strlen(text)), &w, nullptr);
}

LONG WINAPI CrashLog(EXCEPTION_POINTERS* e) {
  if (!g_crash_armed || !e || !e->ExceptionRecord || !e->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
  const DWORD c = e->ExceptionRecord->ExceptionCode;
  if (c != 0xC0000005 && c != 0xC0000096 && c != 0xC00000FD && c != 0xC0000094)
    return EXCEPTION_CONTINUE_SEARCH;
  const uintptr_t at = reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress);
  for (LONG i = 0, n = g_crash_n < kCrashLines ? g_crash_n : kCrashLines; i < n; ++i)
    if (g_crash_seen[i] == at) return EXCEPTION_CONTINUE_SEARCH;  // this place once
  const LONG k = InterlockedIncrement(&g_crash_n) - 1;
  if (k >= kCrashLines) return EXCEPTION_CONTINUE_SEARCH;
  g_crash_seen[k] = at;
  const CONTEXT* x = e->ContextRecord;
  SYSTEMTIME t;
  GetLocalTime(&t);
  char buf[900], w[96];
  CrashWhere(at, w, sizeof w);
  int len = snprintf(buf, sizeof buf,
                     "%02d:%02d:%02d.%03d EXCEPTION %08lx at %p (%s; thread %lu; access %p kind %lu%s) eax=%08lx ebx=%08lx "
                     "ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx\n",
                     t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, c, reinterpret_cast<void*>(at), w,
                     GetCurrentThreadId(),
                     e->ExceptionRecord->NumberParameters > 1 ? reinterpret_cast<void*>(e->ExceptionRecord->ExceptionInformation[1]) : nullptr,
                     e->ExceptionRecord->NumberParameters > 0 ? static_cast<unsigned long>(e->ExceptionRecord->ExceptionInformation[0]) : 0,
                     (e->ExceptionRecord->ExceptionFlags & EXCEPTION_NONCONTINUABLE) ? ", not continuable" : "", x->Eax, x->Ebx,
                     x->Ecx, x->Edx, x->Esi, x->Edi, x->Ebp, x->Esp);
  if (len < 0 || len > 600) len = 600;
  // The first stack words that point into an image (return addresses, most of them), as name + offset.
  len += snprintf(buf + len, sizeof buf - len, "  stack:");
  int shown = 0;
  for (int i = 0; i < 96 && shown < 10 && len < static_cast<int>(sizeof buf) - 80; ++i) {
    const uintptr_t sp = x->Esp + 4 * i;
    if (!Readable(sp, 4)) break;
    CrashWhere(Rd(sp), w, sizeof w);
    if (w[0] == '?') continue;
    len += snprintf(buf + len, sizeof buf - len, " [esp+%x] %s", 4 * i, w);
    ++shown;
  }
  if (len > static_cast<int>(sizeof buf) - 2) len = sizeof buf - 2;
  buf[len] = '\n';
  buf[len + 1] = 0;
  CrashWrite(buf);
  if (k == kCrashLines - 1) CrashWrite("  (no more exception lines this run)\n");
  ss_vm_crash();  // vm_log: the address space at this moment
  return EXCEPTION_CONTINUE_SEARCH;  // only observes; the protector's own handlers see everything first or after as before
}

// frame_pacing = vrr: a race rate above the cap is lowered to it, so every frame reaches the display inside its
// variable range. The cap is min(refresh - 3, refresh - refresh^2 / 3600), rounded down to a whole rate: 116 at
// 120 Hz, 138 at 144, 157 at 165, 224 at 240 (fps = native gives the cap). refresh - refresh^2 / 3600 is the cap
// NVIDIA's own limiter takes with G-Sync + V-Sync (Low Latency Mode Ultra, Reflex): a rate above it would be held
// back by the driver and every held frame counted late on our grid. It also keeps a margin of at least 0.2 ms
// between frames at high refresh rates, where refresh - 3 leaves only 0.05 ms at 240 Hz. 60 is left alone.
double VrrCapFor(DWORD hz) {
  const double h = static_cast<double>(hz);
  const double a = h - 3.0, b = floor(h - h * h / 3600.0);
  return a < b ? a : b;
}

void VrrCap() {
  if (!ss_frame_vsync() || !g_vrr || g_rate == 60.0) return;
  DEVMODEA dm = {};
  dm.dmSize = sizeof dm;
  const DWORD hz = EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm) ? dm.dmDisplayFrequency : 0;
  if (hz < 64) {
    Log("[frames] vrr: the display mode says %lu Hz: no room under it for a race above 60; fps %.4g kept",
        static_cast<unsigned long>(hz), g_rate);
    return;
  }
  const double cap = VrrCapFor(hz);
  if (g_rate > cap) {
    Log("[frames] vrr: fps %.4g lowered to %.4g (the display mode's %lu Hz: min(refresh - 3, refresh - refresh^2/3600), "
        "so every frame is inside the variable refresh range and under NVIDIA's own G-Sync cap)", g_rate, cap,
        static_cast<unsigned long>(hz));
    g_rate = cap;
  } else {
    Log("[frames] vrr: fps %.4g, under the cap %.4g of the display mode's %lu Hz", g_rate, cap, static_cast<unsigned long>(hz));
  }
}

int InitialMode() {
  const char* m = getenv("SS_PATCH_MODE");
  if (m && *m) {
    if (!strcmp(m, "off")) return MODE_OFF;
    if (!strcmp(m, "pass")) return MODE_PASS;
    if (!strcmp(m, "own30")) return MODE_OWN30;
    if (!strcmp(m, "60")) return MODE_SIXTY;
    if (!strcmp(m, "120") || !strcmp(m, "native")) {
      g_rate = m[0] == 'n' ? NativeRate() : 120.0;
      return MODE_SIXTY;
    }
    if (!strcmp(m, "present120")) {
      g_rate = 120.0;
      g_fe_test = true;
      return MODE_SIXTY;
    }
  }
  const double rate = WantedRate();
  if (rate == 0) return MODE_OFF;
  g_rate = rate < 0 ? NativeRate() : rate;
  VrrCap();
  return MODE_SIXTY;
}

DWORD WINAPI Worker(LPVOID) {
  const int mode = InitialMode();
  const char* sl = getenv("SS_PATCH_LOG");
  g_stats = sl && *sl == '1';
  if (const char* rc = getenv("SS_READ_CACHE")) g_rc.on = *rc != '0';  // test switch: 0 = every Readable queries
  g_rings_on = !Off("rings");
  g_ai_on = !Off("ai");
  g_timer_on = !Off("timer");
  g_hfr.hooks = !Off("hfrhook");
  {
    // hfr_ticks = sixty (default): the timeline managers get ticks by game time in every update that runs with the
    // time scale below 1 (the half steps at 60 too); raw: they get the update's tick count as it is.
    const char* e = getenv("SS_HFR_TICKS");
    const std::string v = e ? e : ConfValue("hfr_ticks");
    g_tl_on = v != "raw" && !Off("hfrticks");
    Log("hfr_ticks %s", g_tl_on ? "sixty (timelines advance by game time in slowed and 1/fps updates)" : "raw (timelines get one frame per tick)");
    // Test aid SS_TL_STALE=1: the accumulator table starts full of managers that no longer exist, as it is from
    // the third race of a session on.
    if (const char* st = getenv("SS_TL_STALE"); st && *st == '1') {
      for (auto& x : g_tl_acc) {
        x.self = reinterpret_cast<void*>(static_cast<uintptr_t>(0x10 + (&x - g_tl_acc)));
        x.used = ++g_tl_use;
      }
      Log("[hfr] SS_TL_STALE: the timeline accumulator table starts full (8 stale entries)");
    }
  }
  // docs/plan-120fps.md M0: the [hfr] lines and the two in-update vtable wrappers. Above 60 they are always in: the
  // cadence follows the camera director's wrapper.
  g_hfr.on = g_stats || (g_rate != 60.0 && !g_fe_test);
  if (g_rate != 60.0 && !g_fe_test) {
    // The keys of the plan's later milestones are read so a conf written for them says what this build does.
    const std::string scope = ConfValue("fps_scope");
    g_rw_on = ConfValue("hfr_rewind") != "off" && !Off("hfrrewind");
    g_steer_on = ConfValue("hfr_steering") != "off" && !Off("hfrsteer");
    Log("[hfr] fps %.4g: single-player races run their simulation in 1/%.4g s steps (the game's time scale at %.4f), "
        "everything else stays at 60", g_rate, g_rate, 60.0 / g_rate);
    Log("[hfr] fps_scope race%s; hfr_ticks %s; hfr_rewind %s; a rewind reaches %.1f s (360 ticks)",
        scope == "all" ? " (all is not in this build: plan M6)" : "",
        g_tl_on ? "sixty: the timelines advance by game time (the other integer tick users still get one tick per update)"
                : "raw: timelines and the other integer tick users run fps/60 times too fast in a race",
        g_rw_on ? "capped: the rewind requests' seconds-to-ticks constant follows the rate" : "off: a rewind request reaches 60/fps of its time",
        360.0 / g_rate);
    Log("[hfr] hfr_steering %s", g_steer_on ? "scaled: the steering lock's step term is multiplied by fps/60 in a race in 1/fps steps"
                                            : "off: the wheels turn 60/fps as far for the same steering");
    if (!g_hfr.hooks) Log("[hfr] SS_PATCH_OFF=hfrhook: without the in-update wrappers every race stays at 60");
  }
  {
    const char* e = getenv("SS_TAKEDOWN_REALTIME");
    std::string v = e ? e : ConfValue("takedown_realtime");
    if (!v.empty()) g_takedown_realtime = !(v == "0" || v == "false" || v == "no" || v == "off");
    Log("takedown_realtime %s (at 60 the camera and replay slow motion keep their speed)", g_takedown_realtime ? "on" : "off");
    // slow_half_steps = true | false | playback (SS_SLOW_HALF_STEP = 0 hold, 1 the cameras' live slow motion, the
    // default, 2 slowed playback too)
    e = getenv("SS_SLOW_HALF_STEP");
    if (e && *e) {
      g_half_mode = atoi(e) <= 0 ? 0 : atoi(e) >= 2 ? 2 : 1;
    } else {
      v = ConfValue("slow_half_steps");
      if (v == "0" || v == "false" || v == "no" || v == "off") g_half_mode = 0;
      else if (v == "2" || v == "playback") g_half_mode = 2;
      else if (!v.empty()) g_half_mode = 1;
    }
    Log("slow_half_steps %d (%s)", g_half_mode,
        g_half_mode == 0 ? "every second slowed update held" : g_half_mode == 1 ? "live slow motion in half steps, slowed playback held"
                                                                              : "live slow motion and slowed playback in half steps");
  }
  if (const char* e = getenv("SS_MAXTICKS")) g_maxticks60 = static_cast<uint32_t>(atoi(e)) ? static_cast<uint32_t>(atoi(e)) : 4;
  if (const char* e = getenv("SS_SPIN_US")) g_spin = atof(e) / 1e6;
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  g_freq = f.QuadPart;
  Log("patcher started; mode %d (0 off, 1 pass-through, 2 game arithmetic at 30, 3 sixty)", mode);
  if (mode == MODE_OFF) {  // nothing of the game is touched at all
    // The settings menu (menu.cpp) is still offered: only the device's Present (d3d9.dll's memory) is wrapped.
    if (ss_menu_wanted() && reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)) == 0x400000)
      for (;;) {
        Sleep(250);
        InstallPresentHook();
      }
    return 0;
  }
  // The crash line (CrashLog): appended after the other handlers, observes only. SS_PATCH_LOG=1 arms it at once.
  if (g_log) g_log_handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(g_log)));
  AddVectoredExceptionHandler(0, CrashLog);
  if (g_stats) g_crash_armed = 1;
  if (GetEnvironmentVariableA("SS_PATCH_KEYS", nullptr, 0) || getenv("SS_PATCH_TEST") || !ConfValue("test_keys").empty()) CreateThread(nullptr, 0, KeyThread, nullptr, 0, nullptr);  // test aid only
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  if (base != 0x400000) { Log("unexpected image base %p; nothing done", reinterpret_cast<void*>(base)); return 1; }

  // Wait until the eight state objects are constructed (static initialisers, after the protector has unpacked the
  // code) and every vtable matches the known build. Only reads until then.
  bool taken[8] = {};
  int ntaken = 0;
  DWORD t0 = GetTickCount();
  bool reported_wait = false;
  for (;;) {
    for (int k = 0; k < 8; ++k) {
      if (taken[k]) continue;
      int r = InstallState(k);
      if (r == -1) {
        Log("state %d: vtable differs from the known build; nothing more is done", k);
        if (ntaken) Log("(the states taken over before keep running through the original step arithmetic)");
        return 1;
      }
      if (r >= 1) {
        taken[k] = true;
        ++ntaken;
        Log("state %d (object %p, vtable %p -> copy %p) taken over", k, reinterpret_cast<void*>(kStates[k].obj),
            reinterpret_cast<void*>(kStates[k].vt), static_cast<void*>(&g_vt[k][1]));
        if (ntaken == 1) PrepareAiVtable();
      }
    }
    if (ntaken == 8) break;
    if (GetTickCount() - t0 > 180000) {
      Log("gave up waiting: %d of 8 states constructed", ntaken);
      return 1;
    }
    if (!reported_wait && GetTickCount() - t0 > 30000) {
      reported_wait = true;
      Log("still waiting for the state objects (%d of 8)", ntaken);
    }
    Sleep(25);
  }
  {
    const char* e = getenv("SS_FRAME_PACING");
    std::string v = e ? e : ConfValue("frame_pacing");
    g_pace_present = !(v == "step");
    ss_frame_vsync();  // decided (and logged) once
    e = getenv("SS_FRAME_LOG");
    v = e ? e : ConfValue("frame_log");
    if (!v.empty()) g_frame_log = !(v == "0" || v == "false" || v == "no" || v == "off");
    e = getenv("SS_NET_POLL");
    v = e ? e : ConfValue("net_poll");
    g_net_cache = !(v == "game");
    if (!v.empty() && v != "game" && v != "auto") Log("[frames] net_poll = %s is not auto or game: auto", v.c_str());
    e = getenv("SS_LOOP_SLEEP");
    v = e ? e : ConfValue("loop_sleep");
    g_loop_sleep_skip = !(v == "game");
    if (!v.empty() && v != "game" && v != "auto") Log("[frames] loop_sleep = %s is not auto or game: auto", v.c_str());
    Log("frame_pacing %s, frame_log %s",
        !g_pace_present ? "step (the wait before the work, as before)"
        : g_vrr         ? "vrr (the presents on a 1/60 s grid on the PC's clock, each on its own variable-refresh vblank)"
        : Vsynced()     ? "smooth / vsync (the presents on a 1/60 s grid held to the display's vblank)"
        : g_fastest     ? "fastest (the presents on a 1/60 s grid, shown at once)"
                        : "present (the presents on a 1/60 s grid)",
        g_frame_log ? "on" : "off");
    if (g_fe_test) Log("test aid present120: the front end runs at %.4g updates and presents a second, one tick each", g_rate);
  }
  if (!PrepareAiVtable()) { g_ai_on = false; Log("cAIPlayer vtable differs from the known build; AI gate off"); }
  g_mode = mode;
  Log("all eight states taken over; mode %d", mode);

  // Keep watching: if the game ever re-constructs a state object, take it over again. Test builds also sample threads.
  DWORD last_mon = GetTickCount();
  for (;;) {
    Sleep(250);
    InstallPresentHook();
    for (int k = 0; k < 8; ++k) {
      if (Rd(kStates[k].obj) == kStates[k].vt) {
        if (InstallState(k) == 2) Log("state %d was re-constructed by the game: taken over again", k);
      }
    }
    if (g_stats && GetTickCount() - last_mon >= 10000) {
      last_mon = GetTickCount();
      Monitor();
    }
  }
  return 0;
}

}  // namespace

// Shared with pad.cpp and audio.cpp: the log, the conf file and its presence.
void ss_log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  LogV(fmt, ap);
  va_end(ap);
}
std::string ss_conf(const char* key) { return ConfValue(key); }
// For vm.cpp's reports (inside a failing allocation or a crash): the time, then the text, straight to the log's
// handle. Every stdio line of the log is flushed when written, so the two ways of writing do not overlap.
void ss_log_raw(const char* text) {
  if (!g_log) return;
  if (g_log_handle == INVALID_HANDLE_VALUE || !g_log_handle) g_log_handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(g_log)));
  SYSTEMTIME t;
  GetLocalTime(&t);
  char stamp[24];
  snprintf(stamp, sizeof stamp, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
  CrashWrite(stamp);
  CrashWrite(text);
}

// The settings menu's live rows of this file (menu.cpp calls it after writing the value to splitsecond.conf).
bool ss_patch_apply(const char* key, const char* value) {
  const std::string v = value ? value : "";
  const bool on = !(v == "0" || v == "false" || v == "no" || v == "off");
  if (!strcmp(key, "takedown_realtime")) {
    g_takedown_realtime = on;
  } else if (!strcmp(key, "slow_half_steps")) {
    // Read once per update (SlowDecide); a playback half step under way is given back its tick there when the mode
    // no longer allows it, as when takedown_realtime goes off.
    g_half_mode = (v == "2" || v == "playback") ? 2 : on ? 1 : 0;
  } else if (!strcmp(key, "frame_pacing")) {
    // Vsync is the device's presentation interval: made at start, so a change of it waits for the next start.
    // vrr also sets the race rate (InitialMode): a change to or from it waits for the next start too.
    if (v != "step" && (VsyncFor(v) != ss_frame_vsync() || (v == "vrr") != g_vrr || FastestFor(v) != g_fastest)) return false;
    g_pace_present = v != "step";
  } else if (!strcmp(key, "frame_log")) {
    if (on && !g_frame_log) g_fs = FrameStats();  // a fresh 10 s window
    g_frame_log = on;
  } else {
    return false;
  }
  return true;
}
bool ss_conf_present() {
  std::string path = std::string(g_dir) + "splitsecond.conf";
  FILE* f = fopen(path.c_str(), "r");
  if (!f) return false;
  fclose(f);
  return true;
}

namespace {
bool g_active = false;

// True when an SS_* variable is in the environment (every switch and test aid of the DLL has that prefix).
bool AnySsEnv() {
  char* block = GetEnvironmentStringsA();
  if (!block) return false;
  bool found = false;
  for (const char* e = block; *e && !found; e += strlen(e) + 1) found = strncmp(e, "SS_", 3) == 0;
  FreeEnvironmentStringsA(block);
  return found;
}
}  // namespace

bool ss_patch_active() { return g_active; }


const char* volatile g_pacing_name = "auto";

bool ss_frame_vsync() {
  static int on = -1;
  if (on < 0) {
    const char* e = getenv("SS_FRAME_PACING");
    const std::string v = e ? e : ConfValue("frame_pacing");
    static std::string name;  // for diag.cpp's [summary] (ss_frame_pacing_name)
    name = v.empty() ? "auto" : v;
    g_pacing_name = name.c_str();
    g_vrr = v == "vrr";  // before `on`: a thread that finds the decision made also finds this
    g_fastest = FastestFor(v);
    on = VsyncFor(v) ? 1 : 0;
    Log("[frames] frame_pacing = %s%s: %s", v.empty() ? "auto" : v.c_str(),
        (v.empty() || v == "auto") ? (UnderGamescope() ? " (under gamescope)" : " (desktop)") : "",
        g_vrr       ? "vrr (the device presents on the vblank; a variable-refresh display shows each frame as it comes)"
        : on        ? "smooth / vsync (the device presents on the vblank)"
        : g_fastest ? "fastest (the device presents at once, interval IMMEDIATE: no wait for the vblank, may tear)"
                    : "no vsync from the patch (the game's own option decides)");
    if (on) {
      // DXVK (Proton): at most one frame queued. DXVK_CONFIG is read when Direct3DCreate9 runs, after this.
      char cur[1024] = "";
      const DWORD n = GetEnvironmentVariableA("DXVK_CONFIG", cur, sizeof cur);
      if (n < sizeof cur && !strstr(cur, "maxFrameLatency")) {
        std::string nv = cur;
        if (!nv.empty()) nv += ";";
        nv += "d3d9.maxFrameLatency = 1";
        SetEnvironmentVariableA("DXVK_CONFIG", nv.c_str());
        Log("[frames] DXVK_CONFIG = %s", nv.c_str());
      }
    }
  }
  return on > 0;
}

const char* ss_frame_pacing_name() {
  ss_frame_vsync();
  return g_pacing_name;
}

unsigned ss_frame_dev_interval() { return g_dev_interval; }

// picture.cpp: the game's update rate now (one render per update): 30 unless our step runs (fps 60 or more), else
// 60 or, in a race in 1/g_rate steps, g_rate.
double ss_frame_update_rate() { return g_mode == MODE_SIXTY && g_code_ok ? CadRate() : 30.0; }

// quality.cpp / picture.cpp at start, before the worker has chosen the mode: fps (or SS_PATCH_MODE) asks for 60 or more.
bool ss_frame_wanted_above_30() {
  const char* m = getenv("SS_PATCH_MODE");
  if (m && *m) return !strcmp(m, "60") || !strcmp(m, "120") || !strcmp(m, "native") || !strcmp(m, "present120");
  return WantedRate() != 0;
}

bool ss_frame_immediate() {
  ss_frame_vsync();  // decides g_fastest once
  return g_fastest;
}

void ss_frame_interval(unsigned interval) {
  g_dev_interval = interval;
  Log("[frames] the device presents with interval 0x%x (%s)", interval,
      interval == 0x80000000u ? "immediate: no vsync" : "on the vblank: vsync");
}

void ss_patch_start(HMODULE self) {
  GetModuleFileNameA(self, g_dir, MAX_PATH);
  char* slash = strrchr(g_dir, '\\');
  if (slash) slash[1] = 0;
  // The log first, before anything that can fail: even a silent exit of the game leaves the start-up check behind.
  std::string lp = std::string(g_dir) + "splitsecond-patch.log";
  // The last run's log is kept as splitsecond-patch.prev.log: after a crash and a restart it is still there.
  const std::string prev = std::string(g_dir) + "splitsecond-patch.prev.log";
  MoveFileExA(lp.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
  g_log = fopen(lp.c_str(), "w");
  Log("Back on Air, a PC patch for Split/Second");  // the patch's name, the log's first line
  // No splitsecond.conf and no SS_* switch: the patch is off; only the start-up check is logged (no message box).
  const bool conf = ss_conf_present();
  g_active = conf || AnySsEnv();
  // srccheck.cpp: the exe (not the Steam build: nothing below is started, nor anything in proxy.cpp), other mods.
  // srccheck_test = exe: a test aid, the exe is taken as unknown.
  const bool known = ss_srccheck_run(self, conf, g_active, conf && ConfValue("srccheck_test") == "exe");
  if (!g_active) {
    Log("no splitsecond.conf beside the DLL and no SS_* switch: the patch is off");
    return;
  }
  if (!known) {
    Log("the exe is not the known Steam build: nothing is patched or hooked; the DLL only forwards DirectInput");
    g_active = false;
    return;
  }
  ss_overlay_start();    // first: the import slots are taken before the game opens its archives
  ss_saves_start(self);  // done before the game reads its profile
  ConfEnvironment();
  ss_vm_start(self);
  ss_renderer_start();  // before anything makes an IDirect3D9 (dxvk_fit, display.cpp, the game)
  ss_pipewarm_start();  // after env.* and the renderer choice, before DXVK reads DXVK_CONFIG (Direct3DCreate9)
  ss_dxvkwin_start();
  CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
  ss_audio_start();
  ss_quality_start();
  ss_aspect_start();
  ss_ppicon_start();
  ss_extras_start();  // extras.cpp: unlock_all, bonus_content, pure_decal (memory only, off online)
  ss_keyrepeat_start();
  ss_options_entry_start();
  ss_lodfade_start();
  ss_shadowsmooth_start();
  ss_deband_start();
  ss_slipstream_start();
  ss_picture_start();
  ss_autopilot_start();
  ss_streampool_start();
}
