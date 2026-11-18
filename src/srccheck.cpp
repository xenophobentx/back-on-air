// srccheck.cpp: the start-up check of the game's files, so a report of "it does not start" explains itself.
//
// Run once from ss_patch_start (DllMain), right after splitsecond-patch.log is opened and before anything is patched:
//   * the exe: its size and SHA-256 (one sequential read of the file, Windows' CNG hash) against the Steam build
//     385319 every fixed address of this patch belongs to. Any other exe (retail DVD, another store, a cracked or
//     changed one): nothing at all is patched or hooked (proxy.cpp starts no module, pad.cpp wraps nothing); the DLL
//     only forwards DirectInput, and one message box says so (its own thread: the game is not held up).
//   * the game folder: one listing of it, and of scripts\ and plugins\, against a short table of other wrappers and
//     mods (proxy DLLs, ASI loaders and plugins, ReShade, Special K, the old HFR mod), and the d3d9.dll beside the
//     exe against the DXVK this patch ships. Found ones are logged and named in one message box, shown again only
//     when the list changes (the list last shown is kept in splitsecond-patch.warned). Nothing is disabled.
//   * the patch's own files: splitsecond.conf, and on Windows dxvk\d3d9.dll.
//   * overlay and injector modules already in the process (GetModuleHandle on a short list); logged again after
//     the game's CreateDevice (display.cpp). Log only: the Steam overlay is normal.
// Without splitsecond.conf (the patch switched off) everything is logged and no box is shown.
// Paths in the log: the game folder only (the log line redaction turns a user folder name into <user>).
#include <windows.h>
#include <bcrypt.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "ss_common.h"

namespace {

// The exe every address of this patch belongs to: SplitSecond.exe of the Steam depot, build 385319.
constexpr ULONGLONG kSteamExeSize = 23359488;
constexpr char kSteamExeSha[] = "82b3b577b40b7703ac7bd95d40a67521f43ad27ef84c3fa14bc4c089235c4453";
// The other two untouched Steam language builds (depots 297867 and 297866), told apart by size and PE time stamp.
// They stay unsupported (the patch is off, as for any other exe); only the message says how to get the supported one.
const char* OtherSteamLanguage(ULONGLONG size, unsigned long stamp) {
  if (size == 23392256 && stamp == 0x4BE3EA74) return "Russian";
  if (size == 23379968 && stamp == 0x4BE13CC6) return "Polish";
  return nullptr;
}
// DXVK 2.7.1's 32-bit d3d9.dll, as the package ships it in dxvk\ (tools/dist/make-dist.sh, DXVK_D3D9_SHA).
constexpr ULONGLONG kDxvkSize = 4493326;
constexpr char kDxvkSha[] = "00ecd422b3b12e9d3b309785f3f19431405389d80eb4a37018ebc63e2d87d900";

bool g_known = true;  // false: the exe is not the Steam build, nothing is patched

// Other wrappers and mods in the game folder. Pattern: a name, or prefix*suffix; case does not matter.
struct Known {
  const wchar_t* pattern;
  const char* what;
};
const Known kFolder[] = {
    {L"dxgi.dll", "dxgi.dll wrapper (ReShade, Special K or similar)"},
    {L"d3d8.dll", "d3d8.dll wrapper"},
    {L"d3d11.dll", "d3d11.dll wrapper"},
    {L"winmm.dll", "winmm.dll proxy (often an ASI loader)"},
    {L"version.dll", "version.dll proxy (often an ASI loader)"},
    {L"dsound.dll", "dsound.dll proxy (often an ASI loader)"},
    {L"dinput.dll", "dinput.dll proxy"},
    {L"xinput*.dll", "XInput proxy (a controller wrapper)"},
    {L"*.asi", "ASI plugin"},
    {L"reshade*.ini", "ReShade settings"},
    {L"specialk*.dll", "Special K"},
    {L"specialk*.ini", "Special K settings"},
    {L"DXFreezerServer.dll", "the old HFR mod: it freezes the game at start"},
};
const wchar_t* const kSubfolders[] = {L"scripts", L"plugins"};  // *.asi only

// Overlay and injector modules (a 32-bit process: the 32-bit names).
struct Module {
  const char* name;
  const char* what;
};
const Module kModules[] = {
    {"GameOverlayRenderer.dll", "Steam overlay"},
    {"RTSSHooks.dll", "RivaTuner Statistics Server (MSI Afterburner, Armoury Crate)"},
    {"RTSSHooks64.dll", "RivaTuner Statistics Server, 64-bit hooks"},
    {"amdihk32.dll", "AMD Software overlay hotkeys"},
    {"DiscordHook.dll", "Discord overlay"},
    {"nvspcap.dll", "NVIDIA overlay / ShadowPlay capture"},
    {"graphics-hook32.dll", "OBS game capture"},
    {"fraps32.dll", "Fraps"},
    {"NahimicOSD.dll", "Nahimic audio overlay"},
    {"ReShade32.dll", "ReShade"},
    {"SpecialK32.dll", "Special K"},
};

std::string Utf8(const std::wstring& w) {
  if (w.empty()) return "";
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n : 0, 0);
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
  return s;
}

bool Match(const wchar_t* name, const wchar_t* pattern) {
  const wchar_t* star = wcschr(pattern, L'*');
  if (!star) return !_wcsicmp(name, pattern);
  const size_t pre = star - pattern, suf = wcslen(star + 1), len = wcslen(name);
  return len >= pre + suf && !_wcsnicmp(name, pattern, pre) && !_wcsicmp(name + len - suf, star + 1);
}

// Size and SHA-256 (lower-case hex, "" when the hash could not be made) of one file. false: it cannot be opened.
bool HashFile(const std::wstring& path, ULONGLONG* size, std::string* hex) {
  hex->clear();
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER sz{};
  GetFileSizeEx(f, &sz);
  *size = static_cast<ULONGLONG>(sz.QuadPart);
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE h = nullptr;
  const DWORD kBuf = 1 << 20;
  unsigned char* buf = static_cast<unsigned char*>(VirtualAlloc(nullptr, kBuf, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  bool ok = buf && BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
            BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) >= 0;
  DWORD got = 0;
  while (ok && ReadFile(f, buf, kBuf, &got, nullptr) && got) ok = BCryptHashData(h, buf, got, 0) >= 0;
  unsigned char digest[32];
  if (ok && BCryptFinishHash(h, digest, sizeof digest, 0) >= 0) {
    char t[3];
    for (unsigned char b : digest) snprintf(t, sizeof t, "%02x", b), *hex += t;
  }
  if (h) BCryptDestroyHash(h);
  if (alg) BCryptCloseAlgorithmProvider(alg, 0);
  if (buf) VirtualFree(buf, 0, MEM_RELEASE);
  CloseHandle(f);
  return true;
}

bool UnderWine() {
  HMODULE nt = GetModuleHandleA("ntdll.dll");
  return nt && GetProcAddress(nt, "wine_get_version");
}

std::string g_system = "Windows";  // for the [summary] block (diag.cpp)

void LogSystem() {
  HMODULE nt = GetModuleHandleA("ntdll.dll");
  using WineVer = const char*(CDECL*)();
  if (auto wv = nt ? reinterpret_cast<WineVer>(GetProcAddress(nt, "wine_get_version")) : nullptr) {
    ss_log("[srccheck] system: Wine / Proton %s", wv());
    g_system = std::string("Wine / Proton ") + wv();
    return;
  }
  using RtlVer = LONG(WINAPI*)(OSVERSIONINFOW*);
  OSVERSIONINFOW v{};
  v.dwOSVersionInfoSize = sizeof v;
  if (auto rv = nt ? reinterpret_cast<RtlVer>(GetProcAddress(nt, "RtlGetVersion")) : nullptr) rv(&v);
  // The registry has the real build; the game's own view differs under a compatibility mode (which shims the call).
  DWORD major = 0, minor = 0, sz = sizeof major;
  char build[32] = "?";
  const char* key = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
  RegGetValueA(HKEY_LOCAL_MACHINE, key, "CurrentMajorVersionNumber", RRF_RT_REG_DWORD, nullptr, &major, &sz);
  sz = sizeof minor;
  RegGetValueA(HKEY_LOCAL_MACHINE, key, "CurrentMinorVersionNumber", RRF_RT_REG_DWORD, nullptr, &minor, &sz);
  sz = sizeof build;
  RegGetValueA(HKEY_LOCAL_MACHINE, key, "CurrentBuildNumber", RRF_RT_REG_SZ, nullptr, build, &sz);
  const bool shim = major && (v.dwMajorVersion != major || v.dwMinorVersion != minor);
  g_system = std::string("Windows build ") + build;
  ss_log("[srccheck] system: Windows %lu.%lu build %s%s%lu.%lu build %lu%s", major, minor, build,
         shim ? " (the game is told " : " (", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber,
         shim ? ": a compatibility shim, from Windows or a compatibility mode)" : ")");
}

DWORD WINAPI BoxThread(void* p) {
  std::wstring* text = static_cast<std::wstring*>(p);
  MessageBoxW(nullptr, text->c_str(), L"Back on Air", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
  delete text;
  return 0;
}

void Box(const std::string& utf8) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
  if (n <= 0) return;
  std::wstring* w = new std::wstring(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &(*w)[0], n);
  w->resize(n - 1);
  // A thread of its own: DllMain holds the loader lock, and the game goes on starting while the box is up.
  if (HANDLE t = CreateThread(nullptr, 0, BoxThread, w, 0, nullptr)) CloseHandle(t);
  else delete w;
}

// The mods box is shown again only when its list differs from the one last shown.
bool SeenBefore(const std::wstring& dir, const std::string& list) {
  const std::wstring path = dir + L"splitsecond-patch.warned";
  std::string old;
  if (FILE* f = _wfopen(path.c_str(), L"rb")) {
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) old.append(b, n);
    fclose(f);
  }
  if (old == list) return true;
  if (FILE* f = _wfopen(path.c_str(), L"wb")) {
    fwrite(list.data(), 1, list.size(), f);
    fclose(f);
  }
  return false;
}

std::string Short(const std::string& sha) { return sha.empty() ? "(no hash)" : sha.substr(0, 16); }

}  // namespace

bool ss_exe_known() { return g_known; }

std::string ss_srccheck_modules(const char* when) {
  std::string found, names;
  auto add = [&](const std::string& line, std::string name) {
    // The short name is cut at the first "(" or ",": drop the space before it, or the list reads "Server , ...".
    while (!name.empty() && name.back() == ' ') name.pop_back();
    found += (found.empty() ? "" : ", ") + line;
    names += (names.empty() ? "" : ", ") + name;
  };
  for (const Module& m : kModules)
    if (GetModuleHandleA(m.name)) add(std::string(m.name) + " (" + m.what + ")", std::string(m.what, strcspn(m.what, "(,")));
  // ReShade or Special K loaded under a system DLL's name: known by an export of theirs.
  for (const char* dll : {"d3d9.dll", "dxgi.dll", "dinput8.dll", "winmm.dll", "version.dll", "dsound.dll"}) {
    HMODULE m = GetModuleHandleA(dll);
    if (!m) continue;
    if (GetProcAddress(m, "ReShadeRegisterAddon")) add(std::string(dll) + " is ReShade", "ReShade");
    if (GetProcAddress(m, "SK_GetDLLVersionStr")) add(std::string(dll) + " is Special K", "Special K");
  }
  if (when) ss_log("[srccheck] overlay / injector modules %s: %s", when, found.empty() ? "none of the known ones" : found.c_str());
  return names;  // the short names ("Steam overlay, Discord overlay"), "" when none
}

std::string ss_srccheck_system() { return g_system; }

bool ss_srccheck_run(void* self_module, bool conf_present, bool boxes, bool test_unknown) {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetModuleFileNameW(static_cast<HMODULE>(self_module), buf, MAX_PATH * 2);
  std::wstring dir(buf, n < MAX_PATH * 2 ? n : 0);
  dir.erase(dir.find_last_of(L'\\') + 1);
  n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
  const std::wstring exe(buf, n < MAX_PATH * 2 ? n : 0);
  const std::wstring exe_name = exe.substr(exe.find_last_of(L'\\') + 1);
  ss_log("[srccheck] game folder %s", Utf8(dir).c_str());
  LogSystem();

  // ---- the exe
  LARGE_INTEGER t0, t1, fq;
  QueryPerformanceCounter(&t0);
  ULONGLONG size = 0;
  std::string sha;
  const bool opened = HashFile(exe, &size, &sha);
  QueryPerformanceCounter(&t1);
  QueryPerformanceFrequency(&fq);
  const double ms = 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(fq.QuadPart);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
      reinterpret_cast<const char*>(GetModuleHandleW(nullptr)) + reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandleW(nullptr))->e_lfanew);
  // A size match with no hash (CNG failed) counts as known: a failing hash must not switch a good install off.
  g_known = opened && size == kSteamExeSize && (sha.empty() || sha == kSteamExeSha);
  if (test_unknown) g_known = false;
  ss_log("[srccheck] exe %s: %llu bytes, SHA-256 %s, PE time stamp 0x%08lx (%.0f ms): %s%s", Utf8(exe_name).c_str(), size,
         opened ? (sha.empty() ? "(could not be made)" : sha.c_str()) : "(the file could not be read)",
         static_cast<unsigned long>(nt->FileHeader.TimeDateStamp), ms,
         g_known ? "known Steam build 385319" : "NOT the Steam build this patch supports; the patch is off",
         test_unknown ? " (srccheck_test = exe)" : "");
  const char* lang = g_known || !opened ? nullptr : OtherSteamLanguage(size, nt->FileHeader.TimeDateStamp);
  if (lang)
    ss_log("[srccheck] this is the %s Steam version of the exe; the supported one is the English / German Steam exe: in "
           "Steam set the game's language to English (Properties > General > Language) and let Steam update it", lang);

  // ---- the patch's own files
  if (!conf_present) ss_log("[srccheck] splitsecond.conf is not beside the DLL");
  if (!UnderWine()) {
    const std::wstring dxvk = dir + L"dxvk\\d3d9.dll";
    if (GetFileAttributesW(dxvk.c_str()) == INVALID_FILE_ATTRIBUTES)
      ss_log("[srccheck] dxvk\\d3d9.dll is not there (renderer = dxvk needs it)");
    else if (HANDLE f = CreateFileW(dxvk.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
             f != INVALID_HANDLE_VALUE) {
      LARGE_INTEGER sz{};
      GetFileSizeEx(f, &sz);
      CloseHandle(f);
      const ULONGLONG ds = static_cast<ULONGLONG>(sz.QuadPart);
      ss_log("[srccheck] dxvk\\d3d9.dll: %llu bytes%s", ds, ds == kDxvkSize ? " (the size of the DXVK 2.7.1 this patch ships)" : ", not the DXVK 2.7.1 this patch ships");
    }
  }

  // ---- other wrappers and mods
  std::string list;  // "- name (what)\n" lines, for the box
  auto add = [&](const std::wstring& name, const char* what) {
    ss_log("[srccheck] found %s: %s", Utf8(name).c_str(), what);
    list += "- " + Utf8(name) + " (" + what + ")\n";
  };
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      if (!_wcsicmp(fd.cFileName, L"d3d9.dll")) {
        const ULONGLONG s = (static_cast<ULONGLONG>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        std::string dh;
        ULONGLONG ds = 0;
        if (s == kDxvkSize) HashFile(dir + fd.cFileName, &ds, &dh);  // the same size as ours: hashed to be sure
        if (dh == kDxvkSha)
          ss_log("[srccheck] d3d9.dll beside the exe is the DXVK 2.7.1 this patch ships (the older layout: it is used whatever renderer says)");
        else
          add(fd.cFileName, (std::string("a d3d9.dll that is not the DXVK this patch ships, ") + std::to_string(s) + " bytes").c_str());
        continue;
      }
      for (const Known& k : kFolder)
        if (Match(fd.cFileName, k.pattern)) {
          add(fd.cFileName, k.what);
          break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  for (const wchar_t* sub : kSubfolders) {
    h = FindFirstFileW((dir + sub + L"\\*.asi").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) continue;
    do {
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) add(std::wstring(sub) + L"\\" + fd.cFileName, "ASI plugin");
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  if (list.empty()) ss_log("[srccheck] no other wrappers or mods of the known kinds in the game folder");
  ss_srccheck_modules("at start");

  // ---- the boxes (not when the patch is switched off by having no splitsecond.conf)
  if (!boxes) return g_known;
  if (!g_known && lang) {
    Box(std::string("Back on Air: this is the ") + lang + " Steam version of SplitSecond.exe.\n"
        "Back on Air supports only the English / German Steam exe.\n\n"
        "In Steam, right-click Split/Second > Properties > General > Language, choose English, "
        "let Steam download the update, and the patch will work.\n\n"
        "The patch is off for now. The game runs without it.\n"
        "Details are in splitsecond-patch.log.");
  } else if (!g_known) {
    char t[160];
    snprintf(t, sizeof t, "%llu bytes, SHA-256 %s...", size, Short(sha).c_str());
    Box("Back on Air: " + Utf8(exe_name) +
        " is not the Steam version of Split/Second that this patch supports.\n\n"
        "Found: " + (opened ? std::string(t) : std::string("the file could not be read")) + "\n"
        "Expected: 23359488 bytes, SHA-256 82b3b577b40b7703...\n\n"
        "The patch is off. The game runs without it.\n"
        "Details are in splitsecond-patch.log.");
  } else if (!list.empty() && !SeenBefore(dir, list)) {
    Box("Back on Air found other mods or wrappers in the game folder:\n\n" + list +
        "\nThey can conflict with this patch. The patch still runs.\n"
        "If the game crashes or does not start, move them out of the game folder and try again.\n\n"
        "This message shows again only if the list changes. Details are in splitsecond-patch.log.");
  }
  return g_known;
}
