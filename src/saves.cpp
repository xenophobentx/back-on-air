// saves.cpp: automatic save backups (save_backups; SSPC-21).
//
// At every start the files of the game's save folder, <Documents>\Disney Interactive Studios\Split Second\ (*.sav,
// config_ss.cfg, ...; under Proton inside the prefix), are copied into a new dated folder
//   <the folder of this DLL>\save-backups\YYYY-MM-DD_HHMMSS\ (one folder a start)
// beside the game, not in Documents: Steam Auto-Cloud syncs the save folder. steam_autocloud.vdf is not copied.
// When the newest backup holds the same files with the same bytes, no new folder is made. The newest
// save_backups folders (default 10, 0..50; 0 = off) are kept; older folders are deleted only when their name is
// exactly our pattern and they hold only plain files.
//
// It runs on its own thread, started from ss_patch_start (DllMain): the game reads its profile much later (after the
// boot movies), and a few small files take milliseconds. Bounded: at most kMaxFiles files of at most kMaxFileBytes
// each. Every failure is logged and the game goes on. Paths are wide (a user name outside the code page).
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

#include "ss_shared.h"

namespace {

constexpr int kDefaultKeep = 10;
constexpr int kMaxKeep = 50;
constexpr size_t kMaxFiles = 64;
constexpr DWORD kMaxFileBytes = 16u << 20;  // a save is a few hundred KB; anything bigger is not the game's
constexpr size_t kMaxTotalBytes = 64u << 20;
constexpr wchar_t kSaveSub[] = L"\\Disney Interactive Studios\\Split Second\\";

wchar_t g_self[MAX_PATH];

std::string Utf8(const std::wstring& w) {
  if (w.empty()) return "";
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (n <= 0) return "?";
  std::string s(n - 1, 0);
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
  return s;
}

struct SaveFile {
  std::wstring name;
  std::vector<char> bytes;
  FILETIME written;
};

bool ReadAll(const std::wstring& path, std::vector<char>* out, FILETIME* written) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size;
  bool ok = GetFileSizeEx(h, &size) && size.QuadPart <= kMaxFileBytes;
  if (ok) {
    out->resize(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    ok = out->empty() || (ReadFile(h, out->data(), static_cast<DWORD>(out->size()), &got, nullptr) && got == out->size());
  }
  if (ok && written) ok = GetFileTime(h, nullptr, nullptr, written) != 0;
  CloseHandle(h);
  return ok;
}

bool WriteAll(const std::wstring& path, const SaveFile& f) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD put = 0;
  bool ok = f.bytes.empty() || (WriteFile(h, f.bytes.data(), static_cast<DWORD>(f.bytes.size()), &put, nullptr) &&
                                put == f.bytes.size());
  if (ok) SetFileTime(h, nullptr, nullptr, &f.written);  // the save's own time, for the one who restores it
  CloseHandle(h);
  return ok;
}

// The plain files of a folder (no subfolders), names only.
std::vector<std::wstring> FilesIn(const std::wstring& dir, bool* has_dirs = nullptr) {
  std::vector<std::wstring> names;
  if (has_dirs) *has_dirs = false;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return names;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (has_dirs && wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) *has_dirs = true;
      continue;
    }
    names.push_back(fd.cFileName);
  } while (FindNextFileW(h, &fd) && names.size() < 4 * kMaxFiles);
  FindClose(h);
  return names;
}

// Exactly YYYY-MM-DD_HHMMSS: the only folder names this file ever makes or deletes.
bool IsOurName(const wchar_t* n) {
  static const wchar_t kForm[] = L"dddd-dd-dd_dddddd";
  for (int i = 0; kForm[i]; ++i) {
    if (!n[i]) return false;
    if (kForm[i] == L'd' ? !(n[i] >= L'0' && n[i] <= L'9') : n[i] != kForm[i]) return false;
  }
  return n[17] == 0;
}

// Our backup folders under root, oldest first (the name sorts as the time).
std::vector<std::wstring> Backups(const std::wstring& root) {
  std::vector<std::wstring> v;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((root + L"*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return v;
  do {
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        IsOurName(fd.cFileName))
      v.push_back(fd.cFileName);
  } while (FindNextFileW(h, &fd) && v.size() < 1000);
  FindClose(h);
  std::sort(v.begin(), v.end());
  return v;
}

bool SameNameLess(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; }

// The backup folder holds exactly these files with these bytes.
bool Identical(const std::wstring& dir, const std::vector<SaveFile>& files) {
  std::vector<std::wstring> names = FilesIn(dir);
  if (names.size() != files.size()) return false;
  std::sort(names.begin(), names.end(), SameNameLess);
  std::vector<char> bytes;
  for (size_t i = 0; i < files.size(); ++i) {  // files is sorted the same way
    if (_wcsicmp(names[i].c_str(), files[i].name.c_str())) return false;
    if (!ReadAll(dir + names[i], &bytes, nullptr) || bytes != files[i].bytes) return false;
  }
  return true;
}

// Deletes one of our backup folders: its plain files, then the folder. A folder with a subfolder is left alone
// (not made by this file).
bool RemoveBackup(const std::wstring& dir) {
  bool has_dirs = false;
  std::vector<std::wstring> names = FilesIn(dir, &has_dirs);
  if (has_dirs) {
    SetLastError(ERROR_DIR_NOT_EMPTY);
    return false;
  }
  for (const std::wstring& n : names) {
    const std::wstring p = dir + n;
    SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(p.c_str());
  }
  return RemoveDirectoryW(dir.c_str()) != 0;
}

int Prune(const std::wstring& root, int keep) {
  std::vector<std::wstring> v = Backups(root);
  int removed = 0;
  for (size_t i = 0; i + keep < v.size(); ++i) {
    if (RemoveBackup(root + v[i] + L"\\")) {
      ++removed;
    } else {
      ss_log("[saves] could not delete the old backup %s (error %lu)", Utf8(v[i]).c_str(), GetLastError());
    }
  }
  return removed;
}

int Keep() {
  const char* e = getenv("SS_SAVE_BACKUPS");
  const std::string v = e ? e : ss_conf("save_backups");
  if (v.empty()) return kDefaultKeep;
  if (v == "false" || v == "off" || v == "no") return 0;
  char* end = nullptr;
  const long n = strtol(v.c_str(), &end, 10);
  if (!end || end == v.c_str() || *end) return kDefaultKeep;
  return static_cast<int>(std::max(0L, std::min(static_cast<long>(kMaxKeep), n)));
}

void Run() {
  const int keep = Keep();
  if (keep == 0) {
    ss_log("[saves] save_backups = 0: off, no backup");
    return;
  }
  // The folder of this DLL (the game's folder), wide.
  std::wstring root = g_self;
  const size_t slash = root.find_last_of(L'\\');
  if (slash == std::wstring::npos) {
    ss_log("[saves] the DLL's folder is not known: no backup");
    return;
  }
  root.resize(slash + 1);
  root += L"save-backups\\";

  // Documents (shell32 loaded here, on this thread, not in DllMain).
  HMODULE shell = LoadLibraryW(L"shell32.dll");
  using GetFolderFn = HRESULT(WINAPI*)(HWND, int, HANDLE, DWORD, LPWSTR);
  GetFolderFn get_folder = shell ? reinterpret_cast<GetFolderFn>(GetProcAddress(shell, "SHGetFolderPathW")) : nullptr;
  wchar_t docs[MAX_PATH] = L"";
  if (!get_folder || FAILED(get_folder(nullptr, CSIDL_PERSONAL, nullptr, 0, docs)) || !docs[0]) {
    ss_log("[saves] the Documents folder is not known: no backup");
    return;
  }
  const std::wstring src = std::wstring(docs) + kSaveSub;
  const DWORD attr = GetFileAttributesW(src.c_str());
  if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
    ss_log("[saves] no save folder yet (%s): nothing to back up", Utf8(src).c_str());
    return;
  }

  std::vector<SaveFile> files;
  size_t total = 0;
  for (const std::wstring& n : FilesIn(src)) {
    if (!_wcsicmp(n.c_str(), L"steam_autocloud.vdf")) continue;
    if (files.size() >= kMaxFiles) {
      ss_log("[saves] more than %u files in the save folder: the rest are not copied", static_cast<unsigned>(kMaxFiles));
      break;
    }
    SaveFile f;
    f.name = n;
    if (!ReadAll(src + n, &f.bytes, &f.written)) {
      ss_log("[saves] could not read %s (error %lu, or larger than %lu bytes): not copied", Utf8(n).c_str(),
             GetLastError(), static_cast<unsigned long>(kMaxFileBytes));
      continue;
    }
    total += f.bytes.size();
    if (total > kMaxTotalBytes) {
      ss_log("[saves] the save folder holds more than %u bytes: no backup", static_cast<unsigned>(kMaxTotalBytes));
      return;
    }
    files.push_back(std::move(f));
  }
  if (files.empty()) {
    ss_log("[saves] the save folder %s has no files: nothing to back up", Utf8(src).c_str());
    return;
  }
  std::sort(files.begin(), files.end(), [](const SaveFile& a, const SaveFile& b) { return SameNameLess(a.name, b.name); });

  std::vector<std::wstring> existing = Backups(root);
  if (!existing.empty() && Identical(root + existing.back() + L"\\", files)) {
    const int removed = Prune(root, keep);
    ss_log("[saves] %u files unchanged since %s%s, no new backup (keeping %d; %d old removed)",
           static_cast<unsigned>(files.size()), Utf8(root).c_str(), Utf8(existing.back()).c_str(), keep, removed);
    return;
  }

  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t name[32];
  swprintf(name, 32, L"%04u-%02u-%02u_%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  const std::wstring dst = root + name + L"\\";
  CreateDirectoryW(root.c_str(), nullptr);
  if (!CreateDirectoryW(dst.c_str(), nullptr)) {
    ss_log("[saves] could not make %s (error %lu): no backup", Utf8(dst).c_str(), GetLastError());
    return;
  }
  for (const SaveFile& f : files) {
    if (!WriteAll(dst + f.name, f)) {
      const DWORD err = GetLastError();
      RemoveBackup(dst);  // a partial backup is not left as the newest one
      ss_log("[saves] could not write %s%s (error %lu): no backup", Utf8(dst).c_str(), Utf8(f.name).c_str(), err);
      return;
    }
  }
  const int removed = Prune(root, keep);
  ss_log("[saves] backed up %u files to %s (keeping %d; %d old removed)", static_cast<unsigned>(files.size()),
         Utf8(dst).c_str(), keep, removed);
}

DWORD WINAPI Thread(void*) {
  const DWORD t0 = GetTickCount();
  try {
    Run();
  } catch (...) {  // out of memory: no backup, the game goes on
    ss_log("[saves] failed (out of memory?): no backup");
  }
  ss_log("[saves] done in %lu ms", GetTickCount() - t0);
  return 0;
}

}  // namespace

// patch.cpp, from ss_patch_start (only when the patch is active, after the log is open).
void ss_saves_start(void* self) {
  if (!GetModuleFileNameW(static_cast<HMODULE>(self), g_self, MAX_PATH)) return;
  HANDLE h = CreateThread(nullptr, 0, Thread, nullptr, 0, nullptr);
  if (h) {
    SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL);
    CloseHandle(h);
  }
}
