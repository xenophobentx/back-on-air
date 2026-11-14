// Test aid: SS_SHOT_AT="30,60,95" saves the back buffer at those seconds after the first present as
// shot-<seconds>.bmp beside the DLL (for checking the picture in headless test runs). Unset: nothing happens.
// A 10-bit back buffer holding PQ (HDR on) is decoded: see Save10.
#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "ss_shared.h"

namespace {

std::vector<int> g_times;
size_t g_next = 0;
DWORD g_t0 = 0;
bool g_init = false;

// One 24-bit BMP of w x h from a row source (bottom-up in the file); `px(x, y, bgr)` fills one pixel.
template <typename F>
void WriteBmp(const char* path, int w, int h, F px) {
  FILE* f = fopen(path, "wb");
  if (!f) return;
  const int row = w * 3, pad = (4 - row % 4) % 4, size = (row + pad) * h;
  BITMAPFILEHEADER fh = {};
  BITMAPINFOHEADER ih = {};
  fh.bfType = 0x4D42;
  fh.bfOffBits = sizeof fh + sizeof ih;
  fh.bfSize = fh.bfOffBits + size;
  ih.biSize = sizeof ih;
  ih.biWidth = w;
  ih.biHeight = h;
  ih.biPlanes = 1;
  ih.biBitCount = 24;
  ih.biSizeImage = size;
  fwrite(&fh, sizeof fh, 1, f);
  fwrite(&ih, sizeof ih, 1, f);
  std::vector<unsigned char> line(row + pad, 0);
  for (int y = h - 1; y >= 0; --y) {
    for (int x = 0; x < w; ++x) px(x, y, &line[x * 3]);
    fwrite(line.data(), line.size(), 1, f);
  }
  fclose(f);
  ss_log("[shot] %s (%dx%d)", path, w, h);
}

// A 10-bit back buffer. As sRGB codes (HDR off, or the swap chain is not in HDR10): the upper 8 bits. As PQ in
// BT.2020 (hdr.cpp's output pass ran): decoded to light and saved twice, shot-N.bmp as an SDR display with its white
// at paper white would show it (everything above clips), shot-N-hi.bmp only the light above paper white, 0 to 4
// paper whites over it (black where the picture is at or under paper white: where the extra light went).
void Save10(const D3DLOCKED_RECT& r, int w, int h, const char* dir, int sec) {
  char path[MAX_PATH + 40];
  float paper = 200.0f;
  auto at = [&](int x, int y) { return reinterpret_cast<const uint32_t*>(static_cast<const unsigned char*>(r.pBits) + y * r.Pitch)[x]; };
  if (!ss_hdr_encoded(&paper)) {
    snprintf(path, sizeof path, "%sshot-%d.bmp", dir, sec);
    WriteBmp(path, w, h, [&](int x, int y, unsigned char* bgr) {
      const uint32_t v = at(x, y);
      bgr[0] = static_cast<unsigned char>((v & 1023) >> 2), bgr[1] = static_cast<unsigned char>(((v >> 10) & 1023) >> 2),
      bgr[2] = static_cast<unsigned char>(((v >> 20) & 1023) >> 2);
    });
    return;
  }
  static float lut[1024];  // PQ code -> nits
  static bool have = false;
  if (!have) {
    for (int i = 0; i < 1024; ++i) {
      const double p = pow(i / 1023.0, 1.0 / 78.84375), n = (p > 0.8359375 ? p - 0.8359375 : 0.0) / (18.8515625 - 18.6875 * p);
      lut[i] = static_cast<float>(10000.0 * pow(n, 1.0 / 0.1593017578125));
    }
    have = true;
  }
  auto light = [&](int x, int y, float* rgb) {  // BT.709 light in nits
    const uint32_t v = at(x, y);
    const float R = lut[(v >> 20) & 1023], G = lut[(v >> 10) & 1023], B = lut[v & 1023];
    rgb[0] = 1.6605f * R - 0.5876f * G - 0.0728f * B;
    rgb[1] = -0.1246f * R + 1.1329f * G - 0.0083f * B;
    rgb[2] = -0.0182f * R - 0.1006f * G + 1.1187f * B;
  };
  auto code = [](float v) { return static_cast<unsigned char>(255.0f * powf(v < 0 ? 0 : v > 1 ? 1 : v, 1 / 2.2f) + 0.5f); };
  snprintf(path, sizeof path, "%sshot-%d.bmp", dir, sec);
  WriteBmp(path, w, h, [&](int x, int y, unsigned char* bgr) {
    float c[3];
    light(x, y, c);
    for (int k = 0; k < 3; ++k) bgr[2 - k] = code(c[k] / paper);
  });
  snprintf(path, sizeof path, "%sshot-%d-hi.bmp", dir, sec);
  WriteBmp(path, w, h, [&](int x, int y, unsigned char* bgr) {
    float c[3];
    light(x, y, c);
    const float m = c[0] > c[1] ? (c[0] > c[2] ? c[0] : c[2]) : (c[1] > c[2] ? c[1] : c[2]);
    const float over = m > paper * 1.01f ? (m - paper) / (4.0f * paper) : 0.0f, s = m > 0 ? over / (m / paper) : 0.0f;
    for (int k = 0; k < 3; ++k) bgr[2 - k] = code(c[k] / paper * s);
  });
}

void SaveBmp(IDirect3DDevice9* dev, int sec) {
  IDirect3DSurface9* bb = nullptr;
  if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
  D3DSURFACE_DESC d;
  bb->GetDesc(&d);
  IDirect3DSurface9* sys = nullptr;
  if (d.Format != D3DFMT_X8R8G8B8 && d.Format != D3DFMT_A8R8G8B8 && d.Format != D3DFMT_A2R10G10B10) {
    ss_log("[shot] back buffer format %u not saved", (unsigned)d.Format);
    bb->Release();
    return;
  }
  if (SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
      SUCCEEDED(dev->GetRenderTargetData(bb, sys))) {
    D3DLOCKED_RECT r;
    if (SUCCEEDED(sys->LockRect(&r, nullptr, D3DLOCK_READONLY))) {
      char dir[MAX_PATH];
      HMODULE me = nullptr;
      GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<const char*>(&SaveBmp), &me);
      GetModuleFileNameA(me, dir, sizeof dir);
      char* slash = strrchr(dir, '\\');
      if (slash) slash[1] = 0;
      if (d.Format == D3DFMT_A2R10G10B10) {
        Save10(r, d.Width, d.Height, dir, sec);
      } else {
        char path[MAX_PATH + 32];
        snprintf(path, sizeof path, "%sshot-%d.bmp", dir, sec);
        WriteBmp(path, d.Width, d.Height, [&](int x, int y, unsigned char* bgr) {
          memcpy(bgr, static_cast<const unsigned char*>(r.pBits) + y * r.Pitch + x * 4, 3);
        });
      }
      sys->UnlockRect();
    }
  }
  if (sys) sys->Release();
  bb->Release();
}

}  // namespace

namespace {

// The exe's data sections (no IMAGE_SCN_MEM_EXECUTE), read once from its PE headers.
struct ImageMap {
  uintptr_t base = 0, end = 0;
  int n = 0;
  uintptr_t lo[32], hi[32];
};

bool HeaderReadable(uintptr_t p, size_t n) {
  MEMORY_BASIC_INFORMATION mi;
  if (!SS_VQ(reinterpret_cast<void*>(p), &mi, sizeof mi) || mi.State != MEM_COMMIT ||
      (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || mi.Protect == 0)
    return false;
  return p + n <= reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize;
}

void MapImage(ImageMap& m) {
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  m.base = base;
  m.end = base + 1;  // headers unreadable: the whole first page counts as the image, nothing in it is allowed
  if (!base || !HeaderReadable(base, sizeof(IMAGE_DOS_HEADER))) return;
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000) return;
  const uintptr_t nt_at = base + dos->e_lfanew;
  if (!HeaderReadable(nt_at, sizeof(IMAGE_NT_HEADERS32))) return;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(nt_at);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return;
  m.end = base + nt->OptionalHeader.SizeOfImage;
  const auto* sec = IMAGE_FIRST_SECTION(nt);
  const int count = nt->FileHeader.NumberOfSections;
  if (!HeaderReadable(reinterpret_cast<uintptr_t>(sec), sizeof(IMAGE_SECTION_HEADER) * count)) return;
  for (int i = 0; i < count && m.n < 32; ++i) {
    if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
    const DWORD size = sec[i].Misc.VirtualSize > sec[i].SizeOfRawData ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
    m.lo[m.n] = base + sec[i].VirtualAddress;
    m.hi[m.n] = base + sec[i].VirtualAddress + size;
    ++m.n;
  }
}

// False for a word that may be the game's code (the copy protection watches it): the known .text range at the
// known base, and inside the exe everything outside its non-executable sections.
bool WordAllowed(uintptr_t at) {
  static ImageMap map;
  static bool mapped = false;
  if (!mapped) {
    MapImage(map);
    mapped = true;
  }
  const uintptr_t end = at + sizeof(void*);
  if (end < at) return false;
  if (map.base == 0x400000 && at < 0xB9B000 && end > 0x401000) return false;
  if (end <= map.base || at >= map.end) return true;  // not the exe (d3d9, fmod, a heap block, this DLL)
  for (int i = 0; i < map.n; ++i)
    if (at >= map.lo[i] && end <= map.hi[i]) return true;
  return false;
}

}  // namespace

bool ss_write_word(void* where, uintptr_t value, uintptr_t* previous) {
  static SRWLOCK lock = SRWLOCK_INIT;
  AcquireSRWLockExclusive(&lock);
  if (!WordAllowed(reinterpret_cast<uintptr_t>(where))) {
    static bool said = false;
    if (!said) ss_log("[write] refused a write at %p: the game's code (or not a data section of the exe)", where);
    said = true;
    ReleaseSRWLockExclusive(&lock);
    SetLastError(ERROR_ACCESS_DENIED);
    return false;
  }
  bool ok = false;
  MEMORY_BASIC_INFORMATION mi;
  const DWORD odd = PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE;  // such pages keep the old path
  const bool queried = SS_VQ(where, &mi, sizeof mi) != 0;
  const DWORD prot = queried ? (mi.Protect & 0xFF) : 0;
  if (queried && mi.State == MEM_COMMIT && !(mi.Protect & odd) &&
      (prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_WRITECOPY)) {
    // Already writable (heap words, .data): the protection is not lifted and put back, which cost two system calls
    // per word on the frame's paths (SSPC-17, perf-plan A3). The query just made says the page is committed and
    // writable now, as the lift said before; the code-range refusal above is unchanged. Write-copy pages take writes
    // as they are (the system makes the page private on the first one): Windows reports a written .data page of an
    // image as PAGE_READWRITE, Wine keeps reporting PAGE_WRITECOPY, so under Wine the exe's .data words (the props'
    // force tweak, written twice a frame) took the lift and put-back pair on every write (SSPC-17 round 2).
    const uintptr_t prev = static_cast<uintptr_t>(
        InterlockedExchange(static_cast<volatile LONG*>(where), static_cast<LONG>(value)));
    if (previous) *previous = prev;
    ok = true;
  } else if (queried) {
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    DWORD old = 0;
    ++ss_vp_calls;
    if (VirtualProtect(where, sizeof(void*), (mi.Protect & exec) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &old)) {
      const uintptr_t prev = static_cast<uintptr_t>(
          InterlockedExchange(static_cast<volatile LONG*>(where), static_cast<LONG>(value)));
      if (previous) *previous = prev;
      ++ss_vp_calls;
      VirtualProtect(where, sizeof(void*), old, &old);
      ok = true;
    }
  }
  ReleaseSRWLockExclusive(&lock);
  return ok;
}

// Test aid for postfx.cpp (SS_SMAA_AB_AT): one 8-bit render-target surface saved as <name>.bmp beside the DLL.
void ss_testshot_surface(void* device, void* surface, const char* name) {
  IDirect3DDevice9* dev = static_cast<IDirect3DDevice9*>(device);
  IDirect3DSurface9* s = static_cast<IDirect3DSurface9*>(surface);
  D3DSURFACE_DESC d;
  if (!dev || !s || FAILED(s->GetDesc(&d)) || (d.Format != D3DFMT_X8R8G8B8 && d.Format != D3DFMT_A8R8G8B8)) return;
  IDirect3DSurface9* sys = nullptr;
  if (SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
      SUCCEEDED(dev->GetRenderTargetData(s, sys))) {
    D3DLOCKED_RECT r;
    if (SUCCEEDED(sys->LockRect(&r, nullptr, D3DLOCK_READONLY))) {
      char dir[MAX_PATH];
      HMODULE me = nullptr;
      GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<const char*>(&ss_testshot_surface), &me);
      GetModuleFileNameA(me, dir, sizeof dir);
      char* slash = strrchr(dir, '\\');
      if (slash) slash[1] = 0;
      char path[MAX_PATH + 64];
      snprintf(path, sizeof path, "%s%s.bmp", dir, name);
      WriteBmp(path, d.Width, d.Height, [&](int x, int y, unsigned char* bgr) {
        memcpy(bgr, static_cast<const unsigned char*>(r.pBits) + y * r.Pitch + x * 4, 3);
      });
      sys->UnlockRect();
    }
  }
  if (sys) sys->Release();
}

volatile LONG g_asked = 0;  // ss_testshot_ask: a capture at the next present, named shot-<n>.bmp (n = 1000 + count)
LONG g_asked_n = 0;

void ss_testshot_ask() { InterlockedIncrement(&g_asked); }

void ss_testshot(void* device) {
  if (g_asked && InterlockedExchange(&g_asked, 0)) SaveBmp(static_cast<IDirect3DDevice9*>(device), 1000 + ++g_asked_n);
  if (!g_init) {
    g_init = true;
    const char* env = getenv("SS_SHOT_AT");
    const std::string conf = env ? "" : ss_conf("test_shots");  // the conf key for a game started by Steam
    const char* e = env ? env : (conf.empty() ? nullptr : conf.c_str());
    if (e)
      for (const char* p = e; *p;) {
        g_times.push_back(atoi(p));
        const char* c = strchr(p, ',');
        if (!c) break;
        p = c + 1;
      }
    g_t0 = GetTickCount();
  }
  if (g_next >= g_times.size()) return;
  const int sec = static_cast<int>((GetTickCount() - g_t0) / 1000);
  if (sec < g_times[g_next]) return;
  SaveBmp(static_cast<IDirect3DDevice9*>(device), g_times[g_next]);
  ++g_next;
}
