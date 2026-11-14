// The patch's shader compiles on a worker thread, and their byte code cached on disk. See shadercache.h.
//
// Key `shader_cache = true | false` (default true; switch SS_SHADER_CACHE): the byte code is kept in
// <the DLL's folder>\shader-cache\<32 hex digits>.cso. The name is two 64-bit FNV-1a hashes of: this file's format
// number, the DLL's build (its compile date and time), the profile, the entry point, the compile flags and the whole
// source (defines included, they are part of it). A file is used only when its header, size, key and checksum match
// and the byte code starts with a vs_3_0 / ps_3_0 version token and ends with the end token; anything else is
// deleted and compiled again. At most kMaxFiles files and kMaxBytes bytes: past that the oldest go (last written
// first; a hit touches its file).
//
// One worker, one compile at a time (D3DX is not asked to compile on two threads at once). Below normal priority:
// the game's own threads come first.
#include "shadercache.h"
#include "ss_shared.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>

// ID3DXBuffer as d3dx9 returns it. Outside the anonymous namespace on purpose: inside it, GCC knows no class of ours
// implements the interface and turns every call through it into a pure-virtual call (as postfx.cpp notes).
struct IShaderCacheBuf {
  virtual HRESULT __stdcall QueryInterface(const IID&, void**) = 0;
  virtual ULONG __stdcall AddRef() = 0;
  virtual ULONG __stdcall Release() = 0;
  virtual void* __stdcall GetBufferPointer() = 0;
  virtual DWORD __stdcall GetBufferSize() = 0;
};

namespace {

using IBuf = IShaderCacheBuf;

constexpr uint32_t kMagic = 0x43535353;  // "SSSC"
constexpr uint32_t kFormat = 1;
constexpr size_t kMaxFiles = 128;
constexpr uint64_t kMaxBytes = 16ull << 20;
constexpr uint32_t kMaxCode = 1u << 20;  // a single shader's byte code: far more than any of ours
const char kBuild[] = __DATE__ " " __TIME__;

struct Header {
  uint32_t magic, format;
  uint64_t key[2];
  uint32_t size;  // bytes of byte code after the header
  uint32_t pad;
  uint64_t sum;   // FNV-1a 64 of the byte code
};

using CompileFn = HRESULT(WINAPI*)(const char*, UINT, const void*, void*, const char*, const char*, DWORD, IBuf**, IBuf**, void**);

CRITICAL_SECTION g_cs;
HANDLE g_wake = nullptr;
std::deque<std::shared_ptr<SsShaderJob>> g_queue;
LONG g_started = 0;
bool g_cache = true;
std::wstring g_dir;  // ...\shader-cache\ ("" when there is no cache)
int g_said_bad = 0;

uint64_t Fnv(const void* p, size_t n, uint64_t h) {
  const unsigned char* b = static_cast<const unsigned char*>(p);
  for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 0x100000001b3ull;
  return h;
}

void Key(const SsShaderJob::Item& it, uint64_t key[2]) {
  for (int k = 0; k < 2; ++k) {
    uint64_t h = k ? 0x84222325cbf29ce4ull : 0xcbf29ce484222325ull;
    const uint32_t fmt = kFormat;
    h = Fnv(&fmt, sizeof fmt, h);
    h = Fnv(kBuild, sizeof kBuild, h);
    h = Fnv(it.profile.c_str(), it.profile.size() + 1, h);
    h = Fnv(it.entry.c_str(), it.entry.size() + 1, h);
    h = Fnv(&it.flags, sizeof it.flags, h);
    h = Fnv(it.src.data(), it.src.size(), h);
    key[k] = h;
  }
}

std::wstring PathOf(const uint64_t key[2]) {
  wchar_t name[40];
  swprintf(name, 40, L"%016llx%016llx.cso", static_cast<unsigned long long>(key[0]), static_cast<unsigned long long>(key[1]));
  return g_dir + name;
}

bool CodeShapeOk(const std::vector<uint32_t>& c, const std::string& profile) {
  if (c.size() < 2) return false;
  const uint32_t want = profile == "vs_3_0" ? 0xFFFE0300u : profile == "ps_3_0" ? 0xFFFF0300u : 0;
  return (!want || c.front() == want) && c.back() == 0x0000FFFFu;
}

bool Load(const SsShaderJob::Item& it, const uint64_t key[2], std::vector<uint32_t>* out) {
  const std::wstring path = PathOf(key);
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  Header h = {};
  DWORD got = 0;
  LARGE_INTEGER size = {};
  bool ok = GetFileSizeEx(f, &size) && ReadFile(f, &h, sizeof h, &got, nullptr) && got == sizeof h && h.magic == kMagic &&
            h.format == kFormat && h.key[0] == key[0] && h.key[1] == key[1] && h.size % 4 == 0 && h.size <= kMaxCode &&
            static_cast<uint64_t>(size.QuadPart) == sizeof h + h.size;
  if (ok) {
    out->assign(h.size / 4, 0);
    ok = ReadFile(f, out->data(), h.size, &got, nullptr) && got == h.size && Fnv(out->data(), h.size, 0xcbf29ce484222325ull) == h.sum &&
         CodeShapeOk(*out, it.profile);
  }
  if (ok) {  // the newest use: kept longest
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    SetFileTime(f, nullptr, nullptr, &now);
  }
  CloseHandle(f);
  if (!ok) {
    out->clear();
    if (g_said_bad++ < 8) ss_log("[shaders] cache entry %ls is damaged or of another format; deleted, compiled again", path.c_str());
    DeleteFileW(path.c_str());
  }
  return ok;
}

// Oldest first out until at most kMaxFiles files and kMaxBytes bytes are left.
void Trim() {
  struct Entry {
    std::wstring name;
    uint64_t time, size;
  };
  std::vector<Entry> all;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((g_dir + L"*.cso").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    all.push_back({fd.cFileName, (uint64_t(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime,
                   (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow});
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  uint64_t bytes = 0;
  for (const Entry& e : all) bytes += e.size;
  if (all.size() <= kMaxFiles && bytes <= kMaxBytes) return;
  std::sort(all.begin(), all.end(), [](const Entry& a, const Entry& b) { return a.time < b.time; });
  size_t n = all.size(), gone = 0;
  for (const Entry& e : all) {
    if (n <= kMaxFiles && bytes <= kMaxBytes) break;
    if (DeleteFileW((g_dir + e.name).c_str())) ++gone;
    --n;
    bytes -= e.size;
  }
  ss_log("[shaders] cache over %zu files or %llu MB: %zu oldest deleted", kMaxFiles, static_cast<unsigned long long>(kMaxBytes >> 20), gone);
}

void Store(const SsShaderJob::Item& it, const uint64_t key[2]) {
  const std::wstring path = PathOf(key), tmp = path + L".tmp";
  Header h = {kMagic, kFormat, {key[0], key[1]}, static_cast<uint32_t>(it.code.size() * 4), 0, 0};
  h.sum = Fnv(it.code.data(), h.size, 0xcbf29ce484222325ull);
  HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    static int said = 0;
    if (said++ < 3) ss_log("[shaders] cannot write the cache (%ls, error %lu)", tmp.c_str(), GetLastError());
    return;
  }
  DWORD put = 0, put2 = 0;
  const bool ok = WriteFile(f, &h, sizeof h, &put, nullptr) && put == sizeof h && WriteFile(f, it.code.data(), h.size, &put2, nullptr) && put2 == h.size;
  CloseHandle(f);
  if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
}

bool Compile(CompileFn compile, const char* tag, SsShaderJob::Item& it) {
  IBuf *code = nullptr, *errors = nullptr;
  const HRESULT hr = compile(it.src.data(), static_cast<UINT>(it.src.size()), nullptr, nullptr, it.entry.c_str(), it.profile.c_str(),
                             it.flags, &code, &errors, nullptr);
  bool ok = SUCCEEDED(hr) && code && code->GetBufferSize() >= 8 && code->GetBufferSize() % 4 == 0 && code->GetBufferSize() <= kMaxCode;
  if (ok) {
    it.code.assign(code->GetBufferSize() / 4, 0);
    memcpy(it.code.data(), code->GetBufferPointer(), code->GetBufferSize());
  } else {
    std::string msg = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "";
    if (msg.size() > 600) msg.resize(600);
    ss_log("%s shader %s (%s) did not compile (0x%08lx): %s", tag, it.entry.c_str(), it.profile.c_str(), static_cast<unsigned long>(hr), msg.c_str());
  }
  if (errors) errors->Release();
  if (code) code->Release();
  return ok;
}

void Run(SsShaderJob& job) {
  static CompileFn compile = [] {
    HMODULE m = GetModuleHandleA("d3dx9_42.dll");
    return m ? reinterpret_cast<CompileFn>(GetProcAddress(m, "D3DXCompileShader")) : nullptr;
  }();
  const DWORD t0 = GetTickCount();
  bool stored = false;
  for (SsShaderJob::Item& it : job.items) {
    uint64_t key[2];
    Key(it, key);
    if (g_cache && !g_dir.empty() && Load(it, key, &it.code)) {
      ++job.hits;
      continue;
    }
    if (!compile) {
      static int said = 0;
      if (said++ < 3) ss_log("%s d3dx9_42.dll!D3DXCompileShader not found", job.tag);
      ++job.failed;
      continue;
    }
    if (Compile(compile, job.tag, it)) {
      ++job.compiled;
      if (g_cache && !g_dir.empty()) {
        Store(it, key);
        stored = true;
      }
    } else {
      ++job.failed;
    }
  }
  if (stored) Trim();
  job.worker_ms = GetTickCount() - t0;
  ss_log("%s shaders for %s on the worker: %zu, %d from the cache (hit), %d compiled (miss), %d failed, %lu ms", job.tag,
         job.what.c_str(), job.items.size(), job.hits, job.compiled, job.failed, static_cast<unsigned long>(job.worker_ms));
}

DWORD WINAPI Worker(LPVOID) {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  for (;;) {
    WaitForSingleObject(g_wake, INFINITE);
    for (;;) {
      std::shared_ptr<SsShaderJob> job;
      EnterCriticalSection(&g_cs);
      if (!g_queue.empty()) {
        job = g_queue.front();
        g_queue.pop_front();
      }
      LeaveCriticalSection(&g_cs);
      if (!job) break;
      if (!job->cancelled) Run(*job);
      InterlockedExchange(&job->state, 1);
    }
  }
  return 0;
}

void Start() {
  InitializeCriticalSection(&g_cs);
  g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  const char* e = getenv("SS_SHADER_CACHE");
  const std::string v = e ? std::string(e) : ss_conf("shader_cache");
  g_cache = !(v == "false" || v == "0" || v == "off" || v == "no");
  if (g_cache) {
    HMODULE self = nullptr;
    wchar_t path[MAX_PATH * 2] = {};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&Start), &self) &&
        GetModuleFileNameW(self, path, MAX_PATH * 2) && wcsrchr(path, L'\\')) {
      wcsrchr(path, L'\\')[1] = 0;
      g_dir = std::wstring(path) + L"shader-cache\\";
      if (!CreateDirectoryW(g_dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        ss_log("[shaders] cannot make %ls (error %lu); compiled every time", g_dir.c_str(), GetLastError());
        g_dir.clear();
      }
    }
  }
  ss_log("[shaders] compiles on a worker thread; disk cache %s%ls (build %s)", g_cache && !g_dir.empty() ? "on: " : "off",
         g_cache ? g_dir.c_str() : L"", kBuild);
  CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
}

}  // namespace

std::shared_ptr<SsShaderJob> ss_shader_submit(std::shared_ptr<SsShaderJob> job) {
  // The first caller makes the worker; any later one waits until it is made.
  if (InterlockedCompareExchange(&g_started, 1, 0) == 0) {
    Start();
    InterlockedExchange(&g_started, 2);
  }
  while (g_started != 2) Sleep(0);
  job->submitted = GetTickCount();
  EnterCriticalSection(&g_cs);
  g_queue.push_back(job);
  LeaveCriticalSection(&g_cs);
  SetEvent(g_wake);
  return job;
}
