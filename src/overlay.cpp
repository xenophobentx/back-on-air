// overlay.cpp: the file overlay (overlay_dir), the extra track ids (dlc_tracks) and the separate save profile
// (dlc_save_profile). SSPC-12; docs/dlc-overlay.md.
//
// overlay_dir = <folder> (default empty = off; env SS_OVERLAY_DIR), relative to the game folder or absolute. When the
// game opens a file for reading and a file with the same path relative to the game folder exists under the overlay
// folder, the open goes to the overlay copy. The stock files are never written. The lever is the game's import table
// (.rdata, reverse engineering of the PC image):
//   0xB9B1A8 CreateFileA       every game file open: the file layer's open with retries (0x5733A0, call at 0x573427),
//                              the save file read (0xA12E08), the C runtime's fopen (0x76E3F7, 0x76E748), 0x7704EE
//   0xB9B100 FindFirstFileA    the data root check at 0x4011E0 ("Data/resident.ark"), the file layer's folder walk
//                              (0x573550); a name without wildcards is redirected, a wildcard walk is not merged
//   0xB9B1F0 GetFileAttributesA  0x6715F0
//   0xB9B214 SHGetFolderPathA  the save folder (0x5906A0, 0x5907C0, 0xA12D50: CSIDL_PERSONAL +
//                              "Disney Interactive Studios\Split Second"); hooked only for dlc_save_profile
// Archives (data\resident.ark, data\seclusion.ark, level, power-play and sector archives) are opened by path through
// CreateFileA and read with SetFilePointer/ReadFile, so a rebuilt archive in the overlay replaces the stock one as a
// whole. Sound banks (fmodex) and movies (binkw32) open their files through their own imports: not redirected.
// The overlay folder is listed once at start (at most kMaxFiles files); each redirected path is logged once.
//
// dlc_tracks = true (default false; env SS_DLC_TRACKS) with <overlay>\dlc-tracks.txt (lines "<id> <level> <subtrack>
// <movie_from>", e.g. "15 nem_quarry A nem_storm_A"): the front end's track tables get the new ids. Four {id, pointer}
// tables in .data (six with the two subtrack tables below), each read through an array view {pointer, count} that a static initializer fills at start:
//   0xD0F9F8 / view 0xD6FB84  "<level>_<subtrack>" -> id (searched by name at 0x47B9BD; not found -> id 0)
//   0xD0FA70 / view 0xD6FB90  id -> event-select movie (16 entries: id 15 is EpisodeStatic.bik, so not extended)
//   0xD10188 / view 0xD6FC40  id -> quick-play tile movie (0x485190: id >= count -> NULL)
//   0xD368F8 / view 0xD7E874  id -> level name (0x78E600)
//   0xD36970 / view 0xD7E880  id -> subtrack letter (0x78E630)
//   0xD369E8 / view 0xD7E88C  id -> "Environments/Levels/<level>/Subtracks/<sub>" (0x78E660); without these two the
//                             race load passes a NULL to the game's sprintf (crash at 0x530647, airport_clone test)
// After the initializers ran, the views are pointed at longer copies in this DLL (plain .data writes). Two wrappers
// in .text bound the id themselves: 0x4751E0 returns NULL for id >= 15 and 0x479680 (6 + id) returns the locked
// movie for 21 (id 15) and NULL above. Whether their callers take a NULL is what the clone-track test shows; the
// fallback is to leave the new id out of the first table (the track then resolves to id 0 and shows Airport's
// movies). Nothing in .text is changed.
//
// dlc_save_profile = auto | on | off (default auto = on while dlc_tracks extends the tables; env
// SS_DLC_SAVE_PROFILE): SHGetFolderPathA(CSIDL_PERSONAL) answers <Documents>\Split Second DLC profile, so the game
// keeps its save under <Documents>\Split Second DLC profile\Disney Interactive Studios\Split Second\ and never loads
// or writes the stock save while the track list is longer. save_backups still copies the stock folder.
//
// Car list log (always on while the overlay is on; read only): the car select does not list the rows of
// Vehicles/VehicleDatabase.xml. It asks the unlock manager [0xD64E48] for its car list (0x47355B rebuild 0xA05FE0,
// 0x473563 list 0xA05360: +0x3C or +0x0C by [[0xD64E40]+8]). A rebuild starts from <InitialCars> of
// UI/Logic/Season.xml (+0x18 count +0x1C data +0x20, loaded by 0xA05820) and adds the credit (+0x6C), elite (+0xBC)
// and bonus unlocks (0xA05E30). The row's Locked (+0x1A0) is read only on the debug all-cars path (0x4735DA, flags
// 0xD6F8F0 / 0xD71A34). So a new car must be in a Season.xml list. The log names InitialCars once and both car-select
// lists whenever they change, with the database rows ([0xD64E4C]: count +4, rows +8, 0x1BC bytes each, name at +0).
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ss_shared.h"

namespace {

constexpr uintptr_t kIatCreateFileA = 0xB9B1A8;
constexpr uintptr_t kIatFindFirstFileA = 0xB9B100;
constexpr uintptr_t kIatGetFileAttributesA = 0xB9B1F0;
constexpr uintptr_t kIatSHGetFolderPathA = 0xB9B214;
constexpr uintptr_t kImageSectionsEnd = 0x118E000;  // the end of .rsrc: the image's own sections lie below

constexpr uintptr_t kTrackIdTable = 0xD0F9F8, kTrackIdView = 0xD6FB84;  // "<level>_<sub>" -> id
constexpr uintptr_t kEvsTable = 0xD0FA70, kEvsView = 0xD6FB90;          // id -> event-select movie
constexpr uintptr_t kQpTable = 0xD10188, kQpView = 0xD6FC40;            // id -> quick-play movie
constexpr uintptr_t kLevelTable = 0xD368F8, kLevelView = 0xD7E874;      // id -> level name
constexpr uintptr_t kSubTable = 0xD36970, kSubView = 0xD7E880;          // id -> subtrack letter
constexpr uintptr_t kPathTable = 0xD369E8, kPathView = 0xD7E88C;        // id -> subtrack folder
constexpr uint32_t kStockTracks = 15;
constexpr int kMaxNewTracks = 8;
constexpr size_t kMaxFiles = 20000;
constexpr int kMaxDepth = 16;

std::string Setting(const char* env, const char* key) {
  const char* e = getenv(env);
  return e ? e : ss_conf(key);
}

bool IsTrue(const std::string& v) { return v == "1" || v == "true" || v == "on" || v == "yes"; }

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string g_game;     // the game folder, with a trailing backslash
std::string g_root;     // the overlay folder, with a trailing backslash
std::map<std::string, std::string> g_files;  // lower-case path relative to the game folder (backslashes) -> overlay path
bool g_profile = false;

CRITICAL_SECTION g_cs;
std::set<std::string> g_logged;
volatile LONG g_redirects = 0;
bool g_trace = false;  // test aid SS_OVERLAY_TRACE=1: every CreateFileA of the game into the log (the first 300)
volatile LONG g_traced = 0;

using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using FindFirstFileAFn = HANDLE(WINAPI*)(LPCSTR, LPWIN32_FIND_DATAA);
using GetFileAttributesAFn = DWORD(WINAPI*)(LPCSTR);
using SHGetFolderPathAFn = HRESULT(WINAPI*)(HWND, int, HANDLE, DWORD, LPSTR);
CreateFileAFn o_create = nullptr;
FindFirstFileAFn o_find = nullptr;
GetFileAttributesAFn o_attr = nullptr;
SHGetFolderPathAFn o_folder = nullptr;

// The path relative to the game folder (lower case, backslashes), or "" when the path is not inside it.
std::string Relative(const char* path) {
  if (!path || !*path) return "";
  std::string p(path);
  std::replace(p.begin(), p.end(), '/', '\\');
  const bool absolute = (p.size() > 1 && p[1] == ':') || p[0] == '\\';
  if (absolute) {
    if (p.size() <= g_game.size() || Lower(p.substr(0, g_game.size())) != Lower(g_game)) return "";
    p = p.substr(g_game.size());
  }
  while (p.size() > 2 && p[0] == '.' && p[1] == '\\') p = p.substr(2);
  if (p.find("..") != std::string::npos || p.find(':') != std::string::npos) return "";
  std::string q;  // collapse doubled separators
  for (char c : p)
    if (!(c == '\\' && !q.empty() && q.back() == '\\')) q += c;
  return Lower(q);
}

const std::string* Target(const char* path) {
  if (g_files.empty()) return nullptr;
  const std::string rel = Relative(path);
  if (rel.empty()) return nullptr;
  auto it = g_files.find(rel);
  return it == g_files.end() ? nullptr : &it->second;
}

void Note(const char* what, const char* path, const std::string& to) {
  InterlockedIncrement(&g_redirects);
  EnterCriticalSection(&g_cs);
  const bool first = g_logged.insert(std::string(what) + " " + Relative(path)).second;
  LeaveCriticalSection(&g_cs);
  if (first) ss_log("[overlay] %s %s -> %s", what, path, to.c_str());
}

HANDLE WINAPI HookCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags,
                              HANDLE tmpl) {
  const DWORD writes = GENERIC_WRITE | GENERIC_ALL | DELETE | FILE_WRITE_DATA | FILE_APPEND_DATA;
  if (g_trace && InterlockedIncrement(&g_traced) <= 300)
    ss_log("[overlay] trace CreateFileA %s access %08lX disposition %lu flags %08lX", name ? name : "(null)", access, disp, flags);
  if (disp == OPEN_EXISTING && !(access & writes)) {
    if (const std::string* to = Target(name)) {
      HANDLE h = o_create(to->c_str(), access, share, sa, disp, flags, tmpl);
      if (h != INVALID_HANDLE_VALUE) {
        Note("open", name, *to);
        return h;
      }
      ss_log("[overlay] open %s: the overlay copy failed (error %lu); the stock file is used", name, GetLastError());
    }
  }
  return o_create(name, access, share, sa, disp, flags, tmpl);
}

HANDLE WINAPI HookFindFirstFileA(LPCSTR name, LPWIN32_FIND_DATAA fd) {
  if (name && !strpbrk(name, "*?"))
    if (const std::string* to = Target(name)) {
      HANDLE h = o_find(to->c_str(), fd);
      if (h != INVALID_HANDLE_VALUE) {
        Note("find", name, *to);
        return h;
      }
    }
  return o_find(name, fd);
}

DWORD WINAPI HookGetFileAttributesA(LPCSTR name) {
  if (const std::string* to = Target(name)) {
    const DWORD a = o_attr(to->c_str());
    if (a != INVALID_FILE_ATTRIBUTES) {
      Note("attributes", name, *to);
      return a;
    }
  }
  return o_attr(name);
}

HRESULT WINAPI HookSHGetFolderPathA(HWND w, int csidl, HANDLE token, DWORD flags, LPSTR out) {
  const HRESULT r = o_folder(w, csidl, token, flags, out);
  if (SUCCEEDED(r) && out && (csidl & 0xFF) == CSIDL_PERSONAL) {
    static const char kSub[] = "\\Split Second DLC profile";
    const size_t n = strlen(out);
    if (n + sizeof kSub < MAX_PATH) {
      memcpy(out + n, kSub, sizeof kSub);
      static volatile LONG said = 0;
      if (!InterlockedExchange(&said, 1)) {
        CreateDirectoryA(out, nullptr);
        ss_log("[overlay] save profile: the game's Documents folder is %s (its save goes below it)", out);
      }
    }
  }
  return r;
}

void Walk(const std::string& dir, const std::string& rel, int depth) {
  if (depth > kMaxDepth || g_files.size() >= kMaxFiles) return;
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA((dir + "*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    const std::string n = fd.cFileName;
    if (n == "." || n == "..") continue;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      Walk(dir + n + "\\", rel + n + "\\", depth + 1);
    } else if (g_files.size() < kMaxFiles) {
      if (rel.empty() && Lower(n) == "dlc-tracks.txt") continue;  // our declaration, not a game file
      g_files[Lower(rel + n)] = dir + n;
    }
  } while (FindNextFileA(h, &fd));
  FindClose(h);
}

// The module that holds an address ("?" when none).
std::string ModuleOf(uintptr_t p) {
  HMODULE m = nullptr;
  char name[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(p), &m) && m) {
    GetModuleFileNameA(m, name, MAX_PATH);
    const char* s = strrchr(name, '\\');
    if (s) return s + 1;
  }
  return name;
}

struct Slot {
  const char* name;
  uintptr_t iat;
  void* hook;
  void** original;
  const char* dll;
  bool wanted;
  bool done;
};

// The slot holds the system function (the loader or the protection's start-up filled it): take it once.
// Not filled yet: false (try again). Filled with something else: hooked anyway when it is inside a system module.
bool TryHook(Slot& s) {
  const uintptr_t cur = *reinterpret_cast<volatile uintptr_t*>(s.iat);
  if (cur == reinterpret_cast<uintptr_t>(s.hook)) return true;
  // Empty, or still the import's name entry (inside the image's own sections, below 0x118E000): not filled yet.
  if (cur < 0x10000 || (cur >= 0x400000 && cur < kImageSectionsEnd)) return false;
  HMODULE m = GetModuleHandleA(s.dll);
  const uintptr_t want = m ? reinterpret_cast<uintptr_t>(GetProcAddress(m, s.name)) : 0;
  const std::string mod = Lower(ModuleOf(cur));
  // The start-up code of the Steam exe fills the kernel32 slots with its own forwarders (addresses above the image's
  // sections, in memory it adds to the exe); those are taken as they are and called as the original.
  const bool forwarder = cur >= kImageSectionsEnd && mod == "splitsecond.exe";
  if (cur != want && !forwarder && mod.find("kernel") == std::string::npos && mod.find("shell32") == std::string::npos) {
    static int said = 0;
    if (said++ < 8) ss_log("[overlay] the %s import slot 0x%X holds %p in %s (not the system function); left alone",
                           s.name, static_cast<unsigned>(s.iat), reinterpret_cast<void*>(cur), mod.c_str());
    return false;
  }
  *s.original = reinterpret_cast<void*>(cur);
  if (!ss_write_word(reinterpret_cast<void*>(s.iat), reinterpret_cast<uintptr_t>(s.hook))) {
    ss_log("[overlay] cannot write the %s import slot 0x%X (error %lu)", s.name, static_cast<unsigned>(s.iat), GetLastError());
    return false;
  }
  if (s.done) ss_log("[overlay] the %s import slot was written back by someone (%p, %s); hooked again", s.name,
                     reinterpret_cast<void*>(cur), mod.c_str());
  else ss_log("[overlay] %s hooked at import slot 0x%X (original %p in %s)", s.name, static_cast<unsigned>(s.iat),
              reinterpret_cast<void*>(cur), mod.c_str());
  s.done = true;
  return true;
}

// ------------------------------------------------------------------------------------------------- track tables

struct Pair {
  uint32_t id;
  uint32_t ptr;
};
struct NewTrack {
  uint32_t id;
  std::string key, level, sub, movie_from;
};
std::vector<NewTrack> g_tracks;
Pair g_ids[kStockTracks + kMaxNewTracks], g_qp[kStockTracks + kMaxNewTracks], g_levels[kStockTracks + kMaxNewTracks],
    g_subs[kStockTracks + kMaxNewTracks], g_paths[kStockTracks + kMaxNewTracks];
// "<level>_<sub>", "<level>", "<sub>", "Environments/Levels/<level>/Subtracks/<sub>" (the tables point at them for the
// life of the game)
char g_names[kMaxNewTracks][4][128];

inline uint32_t Rd(uintptr_t va) { return *reinterpret_cast<const volatile uint32_t*>(va); }

bool ReadTracks() {
  const std::string path = g_root + "dlc-tracks.txt";
  FILE* f = fopen(path.c_str(), "r");
  if (!f) {
    ss_log("[overlay] dlc_tracks = true but %s does not exist: no track is added", path.c_str());
    return false;
  }
  char line[256];
  while (fgets(line, sizeof line, f)) {
    char lvl[64], sub[16], from[64];
    unsigned id;
    if (line[0] == '#' || sscanf(line, "%u %63s %15s %63s", &id, lvl, sub, from) != 4) continue;
    if (id != kStockTracks + g_tracks.size() || g_tracks.size() >= static_cast<size_t>(kMaxNewTracks)) {
      ss_log("[overlay] dlc-tracks.txt: id %u for %s: ids must follow 15, 16, ... (at most %d); line left out", id, lvl,
             kMaxNewTracks);
      continue;
    }
    g_tracks.push_back({id, std::string(lvl) + "_" + sub, lvl, sub, from});
  }
  fclose(f);
  return !g_tracks.empty();
}

bool KnownTrackCode() {
  // 0x4751E0: mov eax,[esp+4]; test eax,eax; jl; cmp eax,0Fh. 0x485190: mov eax,[esp+4]; cmp eax,[0xD6FC44].
  static const uint8_t a[] = {0x8B, 0x44, 0x24, 0x04, 0x85, 0xC0, 0x7C, 0x11, 0x83, 0xF8, 0x0F};
  static const uint8_t b[] = {0x8B, 0x44, 0x24, 0x04, 0x3B, 0x05, 0x44, 0xFC, 0xD6, 0x00};
  return !memcmp(reinterpret_cast<const void*>(0x4751E0), a, sizeof a) &&
         !memcmp(reinterpret_cast<const void*>(0x485190), b, sizeof b);
}

void ExtendTracks() {
  // The views are filled by static initializers at start: wait until they hold the stock tables.
  bool ready = false;
  for (int i = 0; i < 120000 && !ready; i += 5) {
    ready = Rd(kTrackIdView) == kTrackIdTable && Rd(kTrackIdView + 4) == kStockTracks && Rd(kQpView) == kQpTable &&
            Rd(kQpView + 4) == kStockTracks && Rd(kLevelView) == kLevelTable && Rd(kLevelView + 4) == kStockTracks &&
            Rd(kSubView) == kSubTable && Rd(kSubView + 4) == kStockTracks && Rd(kPathView) == kPathTable &&
            Rd(kPathView + 4) == kStockTracks && Rd(kEvsView) == kEvsTable;
    if (!ready) Sleep(5);
  }
  if (!ready) {
    ss_log("[overlay] dlc_tracks: the track tables never held the stock values (%08X %u, %08X %u, %08X %u); no track added",
           Rd(kTrackIdView), Rd(kTrackIdView + 4), Rd(kQpView), Rd(kQpView + 4), Rd(kLevelView), Rd(kLevelView + 4));
    return;
  }
  if (!KnownTrackCode()) {
    ss_log("[overlay] dlc_tracks: the track accessors differ from the known build; no track added");
    return;
  }
  const uint32_t n = kStockTracks;
  memcpy(g_ids, reinterpret_cast<const void*>(kTrackIdTable), n * sizeof(Pair));
  memcpy(g_qp, reinterpret_cast<const void*>(kQpTable), n * sizeof(Pair));
  memcpy(g_levels, reinterpret_cast<const void*>(kLevelTable), n * sizeof(Pair));
  memcpy(g_subs, reinterpret_cast<const void*>(kSubTable), n * sizeof(Pair));
  memcpy(g_paths, reinterpret_cast<const void*>(kPathTable), n * sizeof(Pair));
  uint32_t k = 0;
  for (const NewTrack& t : g_tracks) {
    uint32_t movie = 0;
    for (uint32_t i = 0; i < n; ++i)
      if (!_stricmp(reinterpret_cast<const char*>(static_cast<uintptr_t>(g_ids[i].ptr)), t.movie_from.c_str()))
        for (uint32_t j = 0; j < n; ++j)
          if (g_qp[j].id == g_ids[i].id) movie = g_qp[j].ptr;
    snprintf(g_names[k][0], sizeof g_names[k][0], "%s", t.key.c_str());
    snprintf(g_names[k][1], sizeof g_names[k][1], "%s", t.level.c_str());
    g_ids[n + k] = {t.id, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_names[k][0]))};
    g_qp[n + k] = {t.id, movie};  // 0 when movie_from is not a stock track: 0x485190 then answers NULL
    g_levels[n + k] = {t.id, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_names[k][1]))};
    snprintf(g_names[k][2], sizeof g_names[k][2], "%s", t.sub.c_str());
    snprintf(g_names[k][3], sizeof g_names[k][3], "Environments/Levels/%s/Subtracks/%s", t.level.c_str(), t.sub.c_str());
    g_subs[n + k] = {t.id, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_names[k][2]))};
    g_paths[n + k] = {t.id, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_names[k][3]))};
    ss_log("[overlay] dlc_tracks: id %u = %s (level %s), quick-play movie %s; the event-select table keeps id 15 = "
           "EpisodeStatic.bik", t.id, g_names[k][0], g_names[k][1],
           movie ? reinterpret_cast<const char*>(static_cast<uintptr_t>(movie)) : "none (no such track)");
    ++k;
  }
  // Pointer first, then the count: a reader between the two sees the stock count over the longer copy.
  struct View {
    uintptr_t view;
    Pair* table;
  } views[] = {{kTrackIdView, g_ids}, {kQpView, g_qp}, {kLevelView, g_levels}, {kSubView, g_subs}, {kPathView, g_paths}};
  for (const View& v : views) {
    ss_write_word(reinterpret_cast<void*>(v.view), reinterpret_cast<uintptr_t>(v.table));
    ss_write_word(reinterpret_cast<void*>(v.view + 4), n + k);
  }
  ss_log("[overlay] dlc_tracks: the track tables at 0x%X, 0x%X, 0x%X, 0x%X, 0x%X now hold %u ids (0x4751E0 and "
         "0x479680 still bound the id in code: id 15 gets no quick-play movie there)", static_cast<unsigned>(kTrackIdView),
         static_cast<unsigned>(kQpView), static_cast<unsigned>(kLevelView), static_cast<unsigned>(kSubView),
         static_cast<unsigned>(kPathView), n + k);
}

Slot g_slots[] = {
    {"CreateFileA", kIatCreateFileA, reinterpret_cast<void*>(&HookCreateFileA), reinterpret_cast<void**>(&o_create), "kernel32.dll", false, false},
    {"FindFirstFileA", kIatFindFirstFileA, reinterpret_cast<void*>(&HookFindFirstFileA), reinterpret_cast<void**>(&o_find), "kernel32.dll", false, false},
    {"GetFileAttributesA", kIatGetFileAttributesA, reinterpret_cast<void*>(&HookGetFileAttributesA), reinterpret_cast<void**>(&o_attr), "kernel32.dll", false, false},
    {"SHGetFolderPathA", kIatSHGetFolderPathA, reinterpret_cast<void*>(&HookSHGetFolderPathA), reinterpret_cast<void**>(&o_folder), "shell32.dll", false, false},
};

DWORD WINAPI OverlayThread(LPVOID) {
  // Hook as soon as each slot is filled, then keep watching for a minute: the protection's start-up may fill the
  // table again after we took a slot.
  bool all = false;
  const DWORD t0 = GetTickCount();
  int next_dump = 0;
  static const DWORD kDumpAt[] = {0, 3000, 10000, 30000};
  while (GetTickCount() - t0 < 60000) {
    if (next_dump < 4 && GetTickCount() - t0 >= kDumpAt[next_dump]) {
      ++next_dump;
      for (const Slot& s : g_slots) {
        const uintptr_t v = *reinterpret_cast<volatile uintptr_t*>(s.iat);
        if (s.wanted && !s.done) ss_log("[overlay] t+%lus: the %s slot is not hooked yet, it holds %p (%s)", (GetTickCount() - t0) / 1000, s.name,
                            reinterpret_cast<void*>(v), ModuleOf(v).c_str());
      }
    }
    all = true;
    for (Slot& s : g_slots)
      if (s.wanted && !TryHook(s)) all = false;
    Sleep(all ? 50 : 1);
  }
  for (Slot& s : g_slots)
    if (s.wanted && !s.done) ss_log("[overlay] %s: the import slot was never hooked; the overlay does not see it", s.name);
  ss_log("[overlay] %ld redirected calls in the first minute", static_cast<long>(g_redirects));
  return 0;
}

// ------------------------------------------------------------------------------------------------ car list log
constexpr uintptr_t kUnlockMgr = 0xD64E48, kCarDb = 0xD64E4C;
constexpr uint32_t kCarRow = 0x1BC;

bool MemOk(uintptr_t p, size_t n) {
  if (p < 0x10000) return false;
  MEMORY_BASIC_INFORMATION mi;
  for (uintptr_t a = p, end = p + n; a < end; a = reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize) {
    if (!VirtualQuery(reinterpret_cast<const void*>(a), &mi, sizeof mi)) return false;
    if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
  }
  return true;
}

// The car name of database row i ("?" when out of range or unreadable).
std::string CarName(uint32_t i) {
  const uintptr_t db = Rd(kCarDb);
  if (!MemOk(db, 12)) return "?";
  const uint32_t n = Rd(db + 4);
  const uintptr_t rows = Rd(db + 8);
  if (i >= n || !MemOk(rows + i * kCarRow, kCarRow)) return "?";
  const uintptr_t name = Rd(rows + i * kCarRow);
  if (!MemOk(name, 1)) return "?";
  char buf[48];
  size_t k = 0;
  for (; k + 1 < sizeof buf && MemOk(name + k, 1); ++k) {
    const char c = *reinterpret_cast<const volatile char*>(name + k);
    if (!c) break;
    buf[k] = c >= 0x20 && c < 0x7F ? c : '?';
  }
  buf[k] = 0;
  return buf;
}

// "<count> cars: name name ..." of a {capacity, count, data} list of row indices.
std::string CarList(uintptr_t list, bool* has_placeholder) {
  if (!MemOk(list, 12)) return "(unreadable)";
  const uint32_t n = Rd(list + 4);
  const uintptr_t data = Rd(list + 8);
  if (n > 256 || (n && !MemOk(data, n * 4))) return "(unreadable)";
  std::string s = std::to_string(n) + " cars:";
  for (uint32_t i = 0; i < n; ++i) {
    const std::string name = CarName(Rd(data + i * 4));
    if (name.compare(0, 12, "Placeholder_") == 0 && has_placeholder) *has_placeholder = true;
    s += " " + name;
  }
  return s;
}

DWORD WINAPI CarsThread(LPVOID) {
  // Wait for Season.xml (InitialCars), then follow the two car-select lists for the whole session (every 0.5 s,
  // reads only; at most 40 lines).
  bool initial = false;
  uint32_t last[2] = {~0u, ~0u};
  uintptr_t last_data[2] = {0, 0};
  int lines = 0;
  for (;;) {
    Sleep(500);
    const uintptr_t m = Rd(kUnlockMgr), db = Rd(kCarDb);
    if (!MemOk(m, 0x48) || !MemOk(db, 12) || !Rd(db + 4)) continue;
    if (!initial) {
      if (!Rd(m + 0x1C)) continue;
      initial = true;
      bool ph = false;
      const std::string s = CarList(m + 0x18, &ph);
      ss_log("[cars] vehicle database %u rows; Season.xml InitialCars %s%s", Rd(db + 4), s.c_str(),
             ph ? "" : " (no Placeholder car: a DLC slot never reaches the car select)");
      const uintptr_t rows = Rd(db + 8);
      for (uint32_t i = 0, n = Rd(db + 4); i < n && i < 512; ++i) {
        const uintptr_t r = rows + i * kCarRow;
        if (!MemOk(r, kCarRow) || CarName(i).compare(0, 12, "Placeholder_") != 0) continue;
        const uint8_t* b = reinterpret_cast<const uint8_t*>(r);
        if (!b[0x1A2]) continue;  // Player="false": a stock empty slot
        ss_log("[cars] row %u %s: Locked %u Player %u CarSelectOrder %d", i, CarName(i).c_str(), b[0x1A0], b[0x1A2],
               static_cast<int>(Rd(r + 0x1B8)));
      }
    }
    static const uint32_t kOff[2] = {0x0C, 0x3C};
    static const char* const kWhat[2] = {"list +0x0C", "list +0x3C"};
    for (int k = 0; k < 2 && lines < 40; ++k) {
      const uint32_t n = Rd(m + kOff[k] + 4);
      const uintptr_t data = Rd(m + kOff[k] + 8);
      if (!n || (n == last[k] && data == last_data[k])) continue;
      last[k] = n;
      last_data[k] = data;
      bool ph = false;
      const std::string s = CarList(m + kOff[k], &ph);
      ss_log("[cars] car-select %s %s%s", kWhat[k], s.c_str(), ph ? " (with a Placeholder car)" : "");
      ++lines;
    }
  }
  return 0;
}

DWORD WINAPI TracksThread(LPVOID) {
  ExtendTracks();
  return 0;
}

}  // namespace

void ss_overlay_start() {
  const std::string dir = Setting("SS_OVERLAY_DIR", "overlay_dir");
  if (dir.empty()) return;
  InitializeCriticalSection(&g_cs);
  if (const char* t = getenv("SS_OVERLAY_TRACE")) g_trace = *t == '1';
  char exe[MAX_PATH];
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
  g_game = exe;
  g_game = g_game.substr(0, g_game.find_last_of("\\/") + 1);
  std::string d = dir;
  std::replace(d.begin(), d.end(), '/', '\\');
  g_root = (d.size() > 1 && (d[1] == ':' || d[0] == '\\')) ? d : g_game + d;
  if (g_root.back() != '\\') g_root += '\\';
  const DWORD a = GetFileAttributesA(g_root.c_str());
  if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
    ss_log("[overlay] overlay_dir = %s: %s is not a folder; overlay off", dir.c_str(), g_root.c_str());
    return;
  }
  Walk(g_root, "", 0);
  ss_log("[overlay] overlay_dir %s: %u files%s", g_root.c_str(), static_cast<unsigned>(g_files.size()),
         g_files.size() >= kMaxFiles ? " (the limit: the rest is not seen)" : "");
  int shown = 0;
  for (const auto& f : g_files)
    if (shown++ < 24) ss_log("[overlay]   %s", f.first.c_str());
  const bool tracks = IsTrue(Setting("SS_DLC_TRACKS", "dlc_tracks")) && ReadTracks();
  const std::string prof = Setting("SS_DLC_SAVE_PROFILE", "dlc_save_profile");
  g_profile = prof == "on" || prof == "true" || ((prof.empty() || prof == "auto") && tracks);
  ss_log("[overlay] dlc_tracks %s; save profile %s", tracks ? "on" : "off",
         g_profile ? "separate (<Documents>\\Split Second DLC profile)" : "the stock one");
  for (Slot& s : g_slots) s.wanted = s.iat != kIatSHGetFolderPathA || g_profile;
  for (Slot& s : g_slots)
    if (s.wanted) TryHook(s);  // already filled when the DLL loads late
  CreateThread(nullptr, 0, OverlayThread, nullptr, 0, nullptr);
  CreateThread(nullptr, 0, CarsThread, nullptr, 0, nullptr);
  if (tracks) CreateThread(nullptr, 0, TracksThread, nullptr, 0, nullptr);
}
