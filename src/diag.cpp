// diag.cpp: what a player's log needs to say why an HDR picture looks wrong, and the log's [summary] block (GitHub
// #2: an RTX 3080 showed a grey picture, and the log could not tell whether Windows HDR was on). Nothing here changes
// how the game runs: user32 and kernel32 reads on a thread of its own, started at the game's CreateDevice.
//   * Windows HDR on the game window's monitor (QueryDisplayConfig + DisplayConfigGetDeviceInfo: advanced colour, the
//     SDR white level, the monitor's name and connector, never its serial): at the device, at "HDR10 on", and when it
//     changes (looked at every 3 s). Under Wine / Proton the calls fail or are not there: "not known".
//   * DXVK's swap chain as DXVK made it: a second after "HDR10 on" / off / a Reset, the last "Actual swapchain
//     properties" block of SplitSecond_d3d9.log and its new err: lines.
//   * The window against its monitor and the display mode at "HDR10 on"; focus and display changes while HDR is on
//     (looked at every 250 ms / 3 s, 20 lines at most a run).
//   * Overlays and dxgi / d3d11 / d3d12 10 s after the device; the Proton HDR switches.
//   * [summary]: a second after the first frame (with hdr = true: at HDR10 on instead, or 10 s on when it does not
//     come) and after every HDR on / off, with the swap chain read.
// The log goes into public bug reports: no paths outside the game folder, no serials, no names but the monitor's.
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "ss_common.h"
#include "ss_shared.h"

#ifndef SS_RELEASE
#define SS_RELEASE "?"
#endif
#ifndef SS_COMMIT
#define SS_COMMIT "?"
#endif

namespace {

bool UnderWine() {
  const HMODULE nt = GetModuleHandleA("ntdll.dll");
  return nt && GetProcAddress(nt, "wine_get_version");
}

// ------------------------------------------------------------------------------------------- Windows HDR

struct WinHdr {
  bool known = false;       // the calls answered
  bool on = false, supported = false, wcg = false, limited = false;
  unsigned bits = 0;
  float white = 0;          // the SDR white level in nits (0: not known)
  char name[64] = "";       // the monitor's friendly name, ASCII
  const char* connector = "";
  bool operator!=(const WinHdr& o) const {
    return known != o.known || on != o.on || supported != o.supported || wcg != o.wcg || limited != o.limited ||
           bits != o.bits || static_cast<int>(white) != static_cast<int>(o.white);
  }
};

// DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 (Windows 11 24H2, type 15), spelt out: the headers have it only for Win11.
struct AdvColor2 {
  DISPLAYCONFIG_DEVICE_INFO_HEADER header;
  UINT32 value;  // bit 0 supported, 1 active, 3 limited by policy, 4 HDR supported, 5 HDR on, 6 WCG supported
  UINT32 encoding, bits;
  UINT32 mode;   // 0 SDR, 1 WCG (automatic colour management), 2 HDR
};

const char* Connector(int t) {
  switch (t) {
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI: return "HDMI";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL: return "DisplayPort";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_USB_TUNNEL: return "USB-C DisplayPort";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DVI: return "DVI";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HD15: return "VGA";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL: return "built-in panel";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_MIRACAST: return "Miracast";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_VIRTUAL: return "indirect (a dock, capture or virtual display)";
    default: return "other connector";
  }
}

// The monitor's path in the display configuration, matched by its GDI name (\\.\DISPLAY1).
WinHdr QueryWinHdr(const wchar_t* gdi_name) {
  WinHdr w;
  const HMODULE u = GetModuleHandleA("user32.dll");
  using Sizes = LONG(WINAPI*)(UINT32, UINT32*, UINT32*);
  using Query = LONG(WINAPI*)(UINT32, UINT32*, DISPLAYCONFIG_PATH_INFO*, UINT32*, DISPLAYCONFIG_MODE_INFO*, DISPLAYCONFIG_TOPOLOGY_ID*);
  using Info = LONG(WINAPI*)(DISPLAYCONFIG_DEVICE_INFO_HEADER*);
  const auto sizes = u ? reinterpret_cast<Sizes>(GetProcAddress(u, "GetDisplayConfigBufferSizes")) : nullptr;
  const auto query = u ? reinterpret_cast<Query>(GetProcAddress(u, "QueryDisplayConfig")) : nullptr;
  const auto info = u ? reinterpret_cast<Info>(GetProcAddress(u, "DisplayConfigGetDeviceInfo")) : nullptr;
  if (!sizes || !query || !info) return w;
  std::vector<DISPLAYCONFIG_PATH_INFO> paths;
  std::vector<DISPLAYCONFIG_MODE_INFO> modes;
  LONG r = ERROR_INSUFFICIENT_BUFFER;
  for (int i = 0; i < 3 && r == ERROR_INSUFFICIENT_BUFFER; ++i) {  // the configuration can change in between
    UINT32 np = 0, nm = 0;
    if (sizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return w;
    paths.resize(np);
    modes.resize(nm);
    r = query(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr);
    paths.resize(np);
  }
  if (r != ERROR_SUCCESS) return w;
  for (const DISPLAYCONFIG_PATH_INFO& p : paths) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
    src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    src.header.size = sizeof src;
    src.header.adapterId = p.sourceInfo.adapterId;
    src.header.id = p.sourceInfo.id;
    if (info(&src.header) != ERROR_SUCCESS || wcscmp(src.viewGdiDeviceName, gdi_name) != 0) continue;
    auto ask = [&](DISPLAYCONFIG_DEVICE_INFO_HEADER& h, int type, UINT32 size) {
      h.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(type);
      h.size = size;
      h.adapterId = p.targetInfo.adapterId;
      h.id = p.targetInfo.id;
      return info(&h) == ERROR_SUCCESS;
    };
    AdvColor2 a2 = {};
    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO a = {};
    if (ask(a2.header, 15, sizeof a2)) {
      w.known = true;
      w.on = a2.mode == 2;
      w.wcg = a2.mode == 1;
      w.supported = (a2.value >> 4) & 1;
      w.limited = (a2.value >> 3) & 1;
      w.bits = a2.bits;
    } else if (ask(a.header, DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO, sizeof a)) {
      w.known = true;
      w.on = a.advancedColorEnabled && !a.wideColorEnforced;  // enforced: SDR with automatic colour management
      w.wcg = a.wideColorEnforced;
      w.supported = a.advancedColorSupported;
      w.limited = a.advancedColorForceDisabled;
      w.bits = a.bitsPerColorChannel;
    } else {
      return w;
    }
    DISPLAYCONFIG_SDR_WHITE_LEVEL wl = {};
    if (ask(wl.header, DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL, sizeof wl) && wl.SDRWhiteLevel)
      w.white = wl.SDRWhiteLevel * 80.0f / 1000.0f;
    DISPLAYCONFIG_TARGET_DEVICE_NAME tn = {};
    if (ask(tn.header, DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof tn)) {
      size_t n = 0;
      for (const wchar_t* c = tn.monitorFriendlyDeviceName; *c && n + 1 < sizeof w.name; ++c)
        w.name[n++] = *c >= 32 && *c < 127 && *c != '"' ? static_cast<char>(*c) : '?';
      w.name[n] = 0;
      w.connector = Connector(tn.outputTechnology);
    }
    return w;
  }
  return w;
}

std::string HdrText(const WinHdr& w) {
  if (!w.known) return UnderWine() ? "not known (Wine / Proton does not say)" : "not known (Windows did not answer)";
  char b[240];
  const char* state = w.on        ? "ON"
                      : !w.supported ? "OFF (this monitor or its connection cannot do HDR)"
                      : w.limited    ? "OFF (limited by Windows: a policy, or the connection's bandwidth)"
                      : w.wcg        ? "OFF (SDR with automatic colour management): switch Use HDR on in Windows display settings"
                                     : "OFF: switch Use HDR on in Windows display settings";
  if (w.bits) snprintf(b, sizeof b, "%s (%s%u bit)", state, w.on ? "supported, " : "", w.bits);
  else snprintf(b, sizeof b, "%s", state);
  std::string s = b;
  if (w.on && w.white > 0) {  // the SDR content brightness slider: only in HDR mode
    snprintf(b, sizeof b, ", SDR white %.0f nits", w.white);
    s += b;
  }
  return s;
}

// --------------------------------------------------------------------------- the window and its monitor

HWND g_hwnd = nullptr;

struct Snap {
  int index = 0, count = 0;  // the monitor's number (1..) among count
  RECT wr = {}, mr = {};
  bool fg = false, covers = false, have_window = false;
  DWORD w = 0, h = 0, hz = 0;
  WinHdr hdr;
};

struct MonFind {
  HMONITOR want;
  int n, index;
};
BOOL CALLBACK CountMonitor(HMONITOR m, HDC, LPRECT, LPARAM p) {
  MonFind* f = reinterpret_cast<MonFind*>(p);
  ++f->n;
  if (m == f->want) f->index = f->n;
  return TRUE;
}

bool Foreground() {
  DWORD pid = 0;
  const HWND f = GetForegroundWindow();
  if (f) GetWindowThreadProcessId(f, &pid);
  return f && pid == GetCurrentProcessId();
}

Snap Take() {
  Snap s;
  const HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY);
  MonFind f = {mon, 0, 0};
  EnumDisplayMonitors(nullptr, nullptr, CountMonitor, reinterpret_cast<LPARAM>(&f));
  s.index = f.index;
  s.count = f.n;
  MONITORINFOEXW mi = {};
  mi.cbSize = sizeof mi;
  if (!mon || !GetMonitorInfoW(mon, &mi)) return s;
  s.mr = mi.rcMonitor;
  s.have_window = g_hwnd && IsWindow(g_hwnd) && GetWindowRect(g_hwnd, &s.wr);
  s.covers = s.have_window && EqualRect(&s.wr, &s.mr);
  s.fg = Foreground();
  DEVMODEW dm = {};
  dm.dmSize = sizeof dm;
  if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) s.w = dm.dmPelsWidth, s.h = dm.dmPelsHeight, s.hz = dm.dmDisplayFrequency;
  s.hdr = QueryWinHdr(mi.szDevice);
  return s;
}

std::string MonitorText(const Snap& s) {
  char b[200];
  snprintf(b, sizeof b, "monitor %d of %d", s.index, s.count);
  std::string t = b;
  if (s.hdr.name[0]) t += std::string(" \"") + s.hdr.name + "\"";
  if (*s.hdr.connector) t += std::string(" ") + s.hdr.connector;
  snprintf(b, sizeof b, ", %lux%lu @ %lu Hz", static_cast<unsigned long>(s.w), static_cast<unsigned long>(s.h), static_cast<unsigned long>(s.hz));
  return t + b;
}

std::string WindowText(const Snap& s) {
  if (!s.have_window) return "the game window is not known";
  char b[200];
  snprintf(b, sizeof b, "%ldx%ld at (%ld,%ld): %s, %s", s.wr.right - s.wr.left, s.wr.bottom - s.wr.top, s.wr.left, s.wr.top,
           s.covers ? "covers the monitor" : "does not cover the monitor exactly", s.fg ? "foreground" : "not in the foreground");
  return b;
}

// ------------------------------------------------------------------------------------ DXVK's own log

struct Chain {
  bool file = false, block = false;
  std::string format, cs, mode, images, version, driver;
  int errors = 0;          // err: lines in the part read
  std::string last_error;
};

std::string ExeBase() {
  char p[MAX_PATH] = "";
  const DWORD n = GetModuleFileNameA(nullptr, p, sizeof p);
  if (!n || n >= sizeof p) return "SplitSecond";
  std::string s = p;
  s.erase(0, s.find_last_of("\\/") + 1);
  return s.substr(0, s.find_last_of('.'));
}

// DXVK writes <exe>_d3d9.log into DXVK_LOG_PATH, or the current folder (Windows; under Wine only with DXVK_LOG_PATH).
// A file older than this process is an earlier run's: not used.
HANDLE OpenDxvkLog() {
  const std::string name = ExeBase() + "_d3d9.log";
  std::vector<std::string> tries;
  if (const char* e = getenv("DXVK_LOG_PATH")) {
    std::string p = e;
    if (p == "none") return INVALID_HANDLE_VALUE;
    if (!p.empty() && p[0] == '/') {  // a Unix path under Proton: Wine's Z: drive is the root
      for (char& c : p)
        if (c == '/') c = '\\';
      p = "Z:" + p;
    }
    if (!p.empty()) tries.push_back(p + "\\" + name);
  }
  tries.push_back(name);  // the current folder: the game folder
  FILETIME created, x1, x2, x3;
  const bool have_start = GetProcessTimes(GetCurrentProcess(), &created, &x1, &x2, &x3);
  for (const std::string& t : tries) {
    const HANDLE h = CreateFileA(t.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) continue;
    FILETIME wrote;
    if (have_start && GetFileTime(h, nullptr, nullptr, &wrote) && CompareFileTime(&wrote, &created) < 0) {
      CloseHandle(h);
      continue;
    }
    return h;
  }
  return INVALID_HANDLE_VALUE;
}

std::string ReadAt(HANDLE h, LONGLONG at, DWORD n) {
  std::string s(n, '\0');
  LARGE_INTEGER li;
  li.QuadPart = at;
  DWORD got = 0;
  if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN) || !ReadFile(h, &s[0], n, &got, nullptr)) got = 0;
  s.resize(got);
  return s;
}

std::string After(const std::string& line, const char* key) {
  const size_t k = line.find(key);
  if (k == std::string::npos) return "";
  size_t v = k + strlen(key);
  while (v < line.size() && (line[v] == ' ' || line[v] == ':')) ++v;
  return line.substr(v);
}

LONGLONG g_err_seen = 0;  // the file offset up to which err: lines were copied into our log
int g_err_lines = 0;

Chain ReadDxvk(bool copy_errors) {
  Chain c;
  const HANDLE h = OpenDxvkLog();
  if (h == INVALID_HANDLE_VALUE) return c;
  c.file = true;
  LARGE_INTEGER size = {};
  GetFileSizeEx(h, &size);
  const std::string head = ReadAt(h, 0, 16384);  // "DXVK: v2.7.1", the adapter's "Driver : NVIDIA 621.24.0"
  const LONGLONG from = size.QuadPart > 512 * 1024 ? size.QuadPart - 512 * 1024 : 0;
  const std::string tail = ReadAt(h, from, static_cast<DWORD>(size.QuadPart - from));
  CloseHandle(h);
  for (size_t at = 0; at < head.size();) {
    size_t e = head.find('\n', at);
    if (e == std::string::npos) break;
    std::string line = head.substr(at, e - at);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (c.version.empty() && line.find("DXVK: v") != std::string::npos) c.version = After(line, "DXVK: v");
    if (c.driver.empty() && line.find("Driver ") != std::string::npos && line.find(" : ") != std::string::npos) c.driver = After(line, " : ");
    at = e + 1;
  }
  int copied = 0, block_lines = 0;  // the block's own lines: the 6 after its heading
  for (size_t at = 0; at < tail.size();) {
    size_t e = tail.find('\n', at);
    if (e == std::string::npos) break;  // a line DXVK is still writing
    std::string line = tail.substr(at, e - at);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const LONGLONG offset = from + static_cast<LONGLONG>(at);
    at = e + 1;
    if (line.find("Actual swapchain properties") != std::string::npos) {
      c.block = true;
      block_lines = 6;
      c.format = c.cs = c.mode = c.images = "";
    } else if (line.compare(0, 4, "err:") == 0) {
      ++c.errors;
      c.last_error = After(line, "err:");
      if (copy_errors && offset >= g_err_seen && g_err_lines < 30 && copied < 10) {
        ss_log("[diag] DXVK err: %s", c.last_error.c_str());
        ++g_err_lines, ++copied;
      }
    } else if (block_lines > 0) {
      --block_lines;
      if (line.find("Format:") != std::string::npos) c.format = After(line, "Format:");
      else if (line.find("Color space:") != std::string::npos) c.cs = After(line, "Color space:");
      else if (line.find("Present mode:") != std::string::npos) c.mode = After(line, "Present mode:");
      else if (line.find("Image count:") != std::string::npos) c.images = After(line, "Image count:");
    }
  }
  if (copy_errors) g_err_seen = from + static_cast<LONGLONG>(tail.size());
  return c;
}

// VK_FORMAT_A2B10G10R10_UNORM_PACK32 -> A2B10G10R10, VK_COLOR_SPACE_HDR10_ST2084_EXT -> HDR10_ST2084, ...
std::string Short(std::string s) {
  s = s.substr(0, s.find(' '));
  for (const char* p : {"VK_FORMAT_", "VK_COLOR_SPACE_", "VK_PRESENT_MODE_"})
    if (s.compare(0, strlen(p), p) == 0) s.erase(0, strlen(p));
  for (const char* q : {"_UNORM_PACK32", "_UNORM", "_KHR", "_EXT"}) {
    const size_t n = strlen(q);
    if (s.size() > n && s.compare(s.size() - n, n, q) == 0) s.erase(s.size() - n);
  }
  return s.empty() ? "?" : s;
}

std::string ChainText(const Chain& c) {
  if (!c.file) return "no DXVK log";
  if (!c.block) return "not in DXVK's log yet";
  std::string t = Short(c.format) + " / " + Short(c.cs) + " / " + Short(c.mode) + ", " + (c.images.empty() ? "?" : c.images) + " images; errors: ";
  if (!c.errors) return t + "none";
  char b[32];
  snprintf(b, sizeof b, "%d, the last ", c.errors);
  return t + b + c.last_error.substr(0, 100);
}

// ------------------------------------------------------------------------------------------- the thread

char g_gpu[128] = "";
unsigned g_vendor = 0;
char g_driver[64] = "";
std::string g_overlays;        // the short names of the overlays the last check found
bool g_overlays_late = false;  // ... at the check 10 s after the device
volatile LONG g_win_hdr_off = 0;
volatile LONG g_due = 0;       // GetTickCount() when the report is due (0: none)
const char* volatile g_why = "";
WinHdr g_hdr;                  // the thread's last Windows HDR answer
bool g_hdr_was_on = false;     // ... any answer this run said ON (on NVIDIA: Windows turns it off for the game's screen)
int g_changes = 0;             // focus / display / Windows HDR change lines (20 at most)

void Change(const char* fmt, ...) {
  if (g_changes >= 20) return;
  char b[400];
  va_list a;
  va_start(a, fmt);
  vsnprintf(b, sizeof b, fmt, a);
  va_end(a);
  ss_log("[diag] %s%s", b, ++g_changes == 20 ? " (the last of 20 change lines this run)" : "");
}

void NoteHdr(const WinHdr& w) {
  g_hdr = w;
  if (w.known && w.on) g_hdr_was_on = true;
  InterlockedExchange(&g_win_hdr_off, w.known && !w.on && !UnderWine() ? 1 : 0);
}

const char* IntervalName(unsigned i) {
  switch (i) {
    case 0xFFFFFFFFu: return "not known yet";
    case 0x80000000u: return "IMMEDIATE";
    case 0: return "DEFAULT (one vblank)";
    case 1: return "ONE";
    case 2: return "TWO";
    default: return "other";
  }
}

const char* AllowFse() {
  if (!UnderWine() && !ss_renderer_dxvk()) return "not used (Windows' own d3d9)";
  return ss_hdr_allowfse();
}

std::string PatchHdrText(const SsHdrState& h) {
  if (!h.wanted) return "off (hdr and hdr_ready false: nothing of HDR loaded)";
  char b[160];
  std::string t = h.on ? "on" : "off (the HDR row)";
  if (!h.ten_bit) t += ", the back buffer is not 10-bit";
  if (!h.described) t += ", the swap chain not asked yet";
  else if (!h.ext) t += ", no HDR in this d3d9 (not DXVK, or older than 2.3)";
  else t += h.offered ? ", HDR10 offered" : ", HDR10 not offered";
  t += h.active ? ", colour space set" : ", colour space sRGB";
  if (h.broken) t += ", the HDR passes failed";
  snprintf(b, sizeof b, "; peak %.0f, paper white %.0f; display peak %s", h.peak, h.paper, h.display_peak > 0 ? "" : "not known");
  t += b;
  if (h.display_peak > 0) {
    snprintf(b, sizeof b, "%.0f", h.display_peak);
    t += b;
  }
  return t;
}

DWORD g_wait_from = 0;  // the first frame's [summary] has waited for HDR10 since (the diag thread only)
bool g_late = false;    // it waited 10 s and HDR10 did not come

// HDR is asked for and nothing has said it cannot come, but the swap chain is not in HDR10 yet (Verdict's "not out yet").
bool HdrPending(const SsHdrState& h) {
  return h.wanted && h.on && !h.broken && h.ten_bit && !(h.described && (!h.ext || !h.offered)) && !h.active;
}

std::string Verdict(const SsHdrState& h, const Snap& s, const Chain& c) {
  const bool wine = UnderWine();
  const bool win_known = s.hdr.known && !wine;
  if (!h.wanted || !h.on) return win_known && s.hdr.on ? "the patch's HDR is off: an SDR picture, which Windows HDR shows as SDR" : "the patch's HDR is off: the SDR picture";
  if (h.broken) return "the patch's HDR passes failed: the SDR picture";
  if (!h.ten_bit) return "HDR is on in the patch but the back buffer is not 10-bit (another d3d9, or the device was refused it): the SDR picture";
  if (h.described && !h.ext) return "HDR is on in the patch but this d3d9 has no HDR output (Windows HDR needs renderer = dxvk): the SDR picture";
  if (h.described && !h.offered) {
    if (win_known && !s.hdr.on) return "HDR is on in the patch but Windows HDR is OFF, so the swap chain offers no HDR10: the SDR picture";
    return "HDR is on in the patch but the swap chain does not offer HDR10 (an SDR screen, or HDR is off for the screen): the SDR picture";
  }
  if (!h.active) return g_late ? "HDR is on in the patch but HDR10 is not out 10 s after the first frame" : "HDR is on in the patch but HDR10 is not out yet";
  // Windows HDR was ON earlier in the run (at CreateDevice, or in the back) and is OFF now: seen on NVIDIA with DXVK,
  // where Windows turns HDR off while the game has exclusive full screen (issue #2). Elsewhere the player may have
  // switched it off: no cause is named.
  if (win_known && !s.hdr.on && g_hdr_was_on)
    return g_vendor == 0x10DE ? "the patch sends HDR10 but Windows turned HDR OFF when the game took the screen (it was ON "
                                "before; on NVIDIA Windows does this while the game has exclusive full screen): the "
                                "picture will look grey; see the README's HDR section"
                              : "the patch sends HDR10 but Windows HDR went OFF during the run (it was ON before): the "
                                "picture will look grey";
  if (win_known && !s.hdr.on) return "the patch sends HDR10 but Windows HDR is OFF: the picture will look grey";
  if (c.block && c.cs.find("HDR10_ST2084") == std::string::npos)
    return "the patch set HDR10 but DXVK's swap chain is " + Short(c.cs) + ": the picture will look wrong";
  if (!wine && g_vendor == 0x10DE && s.have_window && !s.covers)
    return "HDR10 is going out, but the window does not cover the monitor: NVIDIA's driver may show it grey or frozen (window_mode borderless or fullscreen)";
  if (win_known) return "HDR10 is going out and Windows HDR is ON";
  return wine ? "HDR10 is going out (Proton: the compositor decides what the screen gets)" : "HDR10 is going out (Windows HDR not known)";
}

void Summary(const Snap& s, const Chain& c) {
  SsHdrState h;
  ss_hdr_state(&h);
  const bool wine = UnderWine();
  std::string d3d9 = wine ? "Proton's d3d9 (DXVK" : ss_renderer_dxvk() ? "DXVK" : "Windows' own d3d9 (renderer = native)";
  if (wine || ss_renderer_dxvk()) {
    if (!c.version.empty()) d3d9 += " " + c.version;
    d3d9 += wine ? ")" : " (renderer = dxvk)";
  }
  ss_log("[summary] Back on Air %s (%s); %s; %s", SS_RELEASE, SS_COMMIT, ss_srccheck_system().c_str(), d3d9.c_str());
  const std::string driver = g_driver[0] ? g_driver : !c.driver.empty() ? c.driver + " (DXVK's log)" : "not known";
  ss_log("[summary] GPU %s, driver %s; overlays%s: %s", g_gpu[0] ? g_gpu : "not known", driver.c_str(), g_overlays_late ? "" : " (so far)",
         g_overlays.empty() ? "none of the known ones" : g_overlays.c_str());
  ss_log("[summary] %s", MonitorText(s).c_str());
  ss_log("[summary] Windows HDR: %s", HdrText(s.hdr).c_str());
  ss_log("[summary] window %s %s", ss_display_window_mode(), WindowText(s).c_str());
  ss_log("[summary] present: %s", ss_diag_present_text().c_str());
  ss_log("[summary] patch HDR: %s", PatchHdrText(h).c_str());
  ss_log("[summary] DXVK swap chain: %s", ChainText(c).c_str());
  ss_log("[summary] verdict: %s", Verdict(h, s, c).c_str());
}

void LogHdr(const WinHdr& w, const Snap& s, const char* when) {
  ss_log("[diag] Windows HDR on the game's %s (%s): %s", MonitorText(s).c_str(), when, HdrText(w).c_str());
}

void LateModules() {
  g_overlays = ss_srccheck_modules("10 s after CreateDevice");
  g_overlays_late = true;
  char exe_dir[MAX_PATH] = "";
  const DWORD n = GetModuleFileNameA(nullptr, exe_dir, sizeof exe_dir);
  if (char* sl = n && n < sizeof exe_dir ? strrchr(exe_dir, '\\') : nullptr) sl[1] = 0;
  std::string t;
  for (const char* dll : {"dxgi.dll", "d3d11.dll", "d3d12.dll"}) {
    const HMODULE m = GetModuleHandleA(dll);
    char p[MAX_PATH] = "";
    const bool here = m && GetModuleFileNameA(m, p, sizeof p) && *exe_dir && !_strnicmp(p, exe_dir, strlen(exe_dir));
    t += std::string(t.empty() ? "" : ", ") + dll + (!m ? " not loaded" : here ? " loaded (from the game folder)" : " loaded (system)");
  }
  ss_log("[diag] 10 s after CreateDevice: %s", t.c_str());
}

void LogProton() {
  if (!UnderWine()) return;
  std::string t;
  for (const char* k : {"DXVK_HDR", "PROTON_ENABLE_HDR", "ENABLE_GAMESCOPE_WSI", "SteamDeck"}) {
    const char* v = getenv(k);
    std::string val = v ? std::string(v).substr(0, 12) : "(not set)";
    for (char& ch : val)
      if (ch < 32 || ch > 126) ch = '?';
    t += std::string(t.empty() ? "" : ", ") + k + "=" + val;
  }
  const bool gamescope = getenv("GAMESCOPE_WAYLAND_DISPLAY") != nullptr;
  ss_log("[diag] Proton: %s; gamescope %s", t.c_str(), gamescope ? "yes (GAMESCOPE_WAYLAND_DISPLAY set)" : "not seen");
}

DWORD WINAPI Thread(LPVOID) {
  const DWORD t0 = GetTickCount();
  Snap s = Take();
  NoteHdr(s.hdr);
  LogHdr(s.hdr, s, "at CreateDevice");
  LogProton();
  g_overlays = ss_srccheck_modules(nullptr);
  bool late = false, fg = s.fg;
  DWORD w = s.w, h = s.h, hz = s.hz, next = t0 + 3000;
  for (;;) {
    Sleep(250);
    const DWORD now = GetTickCount();
    if (!late && now - t0 >= 10000) {
      late = true;
      LateModules();
    }
    SsHdrState hs;
    ss_hdr_state(&hs);
    const bool fg_now = Foreground();
    if (fg_now != fg && hs.active)
      Change("while HDR is on: the game window %s", fg_now ? "is in the foreground again" : "lost the foreground (another window is in front)");
    fg = fg_now;
    if (static_cast<LONG>(now - next) >= 0) {
      next = now + 3000;
      const Snap n = Take();
      if ((n.w != w || n.h != h || n.hz != hz) && hs.active)
        Change("while HDR is on: the display mode changed from %lux%lu @ %lu Hz to %lux%lu @ %lu Hz", static_cast<unsigned long>(w),
               static_cast<unsigned long>(h), static_cast<unsigned long>(hz), static_cast<unsigned long>(n.w),
               static_cast<unsigned long>(n.h), static_cast<unsigned long>(n.hz));
      w = n.w, h = n.h, hz = n.hz;
      if (n.hdr != g_hdr) {
        Change("Windows HDR changed: %s -> %s", HdrText(g_hdr).c_str(), HdrText(n.hdr).c_str());
        static bool said = false;  // back ON as the game goes to the back: Windows turned it off for the game's screen
        if (!said && g_vendor == 0x10DE && hs.active && !fg_now && n.hdr.known && n.hdr.on && g_hdr.known && !g_hdr.on &&
            !UnderWine()) {
          said = true;
          ss_log("[diag] Windows HDR is ON again with the game in the back: Windows turns it OFF while the game has "
                 "exclusive full screen (seen on NVIDIA), so the game's picture looks grey; see the README's HDR section");
        }
        NoteHdr(n.hdr);
      }
    }
    const LONG due = g_due;
    if (due && static_cast<LONG>(now - static_cast<DWORD>(due)) >= 0) {
      const char* why = g_why;
      // hdr = true: the first frame is SDR and HDR10 comes a few seconds later (the swap chain is remade). The first
      // [summary] waits for "HDR10 on" (that event's own summary replaces it) or, when HDR10 does not come, prints
      // after 10 s with that said; hdr = false prints at once.
      if (!strcmp(why, "the first frame") && HdrPending(hs)) {
        if (!g_wait_from) g_wait_from = now | 1;
        if (static_cast<LONG>(now - g_wait_from) < 10000) continue;  // signed: "| 1" can put it 1 ms after now
        g_late = true;
      }
      InterlockedCompareExchange(&g_due, 0, due);
      const Snap r = Take();
      const bool on_event = !strcmp(why, "HDR10 on");
      if (r.hdr != g_hdr && !on_event) Change("Windows HDR changed: %s -> %s", HdrText(g_hdr).c_str(), HdrText(r.hdr).c_str());
      NoteHdr(r.hdr);
      if (on_event) {
        LogHdr(r.hdr, r, "at HDR10 on");
        ss_log("[diag] at HDR10 on: window %s; monitor %ldx%ld at (%ld,%ld), %d monitor(s); mode %lux%lu @ %lu Hz", WindowText(r).c_str(),
               r.mr.right - r.mr.left, r.mr.bottom - r.mr.top, r.mr.left, r.mr.top, r.count, static_cast<unsigned long>(r.w),
               static_cast<unsigned long>(r.h), static_cast<unsigned long>(r.hz));
      }
      const Chain c = ReadDxvk(true);
      if (!c.file) {
        ss_log("[diag] DXVK's swap chain (%s): no DXVK log (%s_d3d9.log of this run is not in the game folder%s)", why, ExeBase().c_str(),
               UnderWine() ? "; under Proton DXVK writes one only with DXVK_LOG_PATH set" : "");
      } else if (c.block) {
        ss_log("[diag] DXVK's swap chain (%s, the last one in its log): %s, %s, %s, %s images", why, c.format.c_str(), c.cs.c_str(),
               c.mode.c_str(), c.images.c_str());
      } else {
        ss_log("[diag] DXVK's swap chain (%s): no \"Actual swapchain properties\" in its log yet", why);
      }
      Summary(r, c);
    }
  }
  return 0;
}

void Ask(const char* why) {
  g_why = why;
  InterlockedExchange(&g_due, static_cast<LONG>((GetTickCount() + 1000) | 1));  // a second: DXVK has written its lines
}

}  // namespace

void ss_diag_device(void* hwnd) {
  static volatile LONG started = 0;
  g_hwnd = static_cast<HWND>(hwnd);
  if (InterlockedExchange(&started, 1)) return;
  if (HANDLE t = CreateThread(nullptr, 0, Thread, nullptr, 0, nullptr)) CloseHandle(t);
}

void ss_diag_gpu(const char* name, unsigned vendor, const char* driver) {
  snprintf(g_gpu, sizeof g_gpu, "%s", name ? name : "");
  g_vendor = vendor;
  snprintf(g_driver, sizeof g_driver, "%s", driver ? driver : "");
}

void ss_diag_present() {
  static bool done = false;  // the render thread only
  if (done) return;
  done = true;
  Ask("the first frame");
}

void ss_diag_hdr_event(bool on) { Ask(on ? "HDR10 on" : "HDR10 off"); }

std::string ss_diag_present_text() {
  char b[200];
  snprintf(b, sizeof b, "frame_pacing %s -> interval %s; window_mode %s; allowFse %s", ss_frame_pacing_name(),
           IntervalName(ss_frame_dev_interval()), ss_display_window_mode(), AllowFse());
  return b;
}

bool ss_diag_windows_hdr_off() { return g_win_hdr_off != 0; }

// The VS_FIXEDFILEINFO in a loaded module's version resource (kernel32 only, no version.dll).
bool ss_file_version(void* module, unsigned v[4]) {
  const HMODULE m = static_cast<HMODULE>(module);
  const HRSRC r = m ? FindResourceW(m, MAKEINTRESOURCEW(1), reinterpret_cast<LPCWSTR>(RT_VERSION)) : nullptr;
  const HGLOBAL g = r ? LoadResource(m, r) : nullptr;
  const uint8_t* p = g ? static_cast<const uint8_t*>(LockResource(g)) : nullptr;
  const DWORD n = r ? SizeofResource(m, r) : 0;
  for (DWORD i = 0; p && i + sizeof(VS_FIXEDFILEINFO) <= n; i += 4) {
    const VS_FIXEDFILEINFO* f = reinterpret_cast<const VS_FIXEDFILEINFO*>(p + i);
    if (f->dwSignature != 0xFEEF04BD) continue;
    v[0] = HIWORD(f->dwFileVersionMS), v[1] = LOWORD(f->dwFileVersionMS);
    v[2] = HIWORD(f->dwFileVersionLS), v[3] = LOWORD(f->dwFileVersionLS);
    return true;
  }
  return false;
}
