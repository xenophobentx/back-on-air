// vm.cpp: the address-space log (vm_log; a diagnostic, off by default).
//
// SplitSecond.exe is a 32-bit image without the large-address flag: 2 GB of address space. vm_log = N walks that
// space with VirtualQuery and writes [vm] lines into splitsecond-patch.log:
//   * at start, every N seconds (vm_log = true: 10), whenever the free space has dropped 96 MB below the last
//     report, when an allocation of the process fails, and from the crash line's handler;
//   * totals by kind (image / mapped / private, committed and reserved), the free space and its largest blocks
//     (fragmentation), and the address space by owner.
// Owners: an image or a mapped file by its file name; a private or pagefile-backed allocation by the module that
// asked for it; an NT heap segment or large heap block by its heap (and who created the heap); a thread stack by the
// module the thread started in. "who asked" comes from four slots of kernelbase.dll's import table (the ntdll calls
// behind VirtualAlloc, VirtualFree, MapViewOfFile, UnmapViewOfFile and HeapCreate), redirected only while vm_log is
// on: system DLL memory, nothing of the game. Memory the kernel maps for a driver (GPU allocations made visible to
// the CPU) has no caller in the process: it shows as "no caller seen".
// The report makes no heap allocation and takes no loader lock, so it can run inside a failing allocation or a crash.
#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

#include "ss_shared.h"

namespace {

using NTSTATUS_ = LONG;
using NtAllocFn = NTSTATUS_(NTAPI*)(HANDLE, PVOID*, ULONG_PTR, SIZE_T*, ULONG, ULONG);
using NtFreeFn = NTSTATUS_(NTAPI*)(HANDLE, PVOID*, SIZE_T*, ULONG);
using NtMapFn = NTSTATUS_(NTAPI*)(HANDLE, HANDLE, PVOID*, ULONG_PTR, SIZE_T, PLARGE_INTEGER, SIZE_T*, DWORD, ULONG, ULONG);
using NtUnmapFn = NTSTATUS_(NTAPI*)(HANDLE, PVOID);
using RtlCreateHeapFn = PVOID(NTAPI*)(ULONG, PVOID, SIZE_T, SIZE_T, PVOID, PVOID);
using BackTraceFn = USHORT(NTAPI*)(ULONG, ULONG, PVOID*, PULONG);
using MappedNameFn = DWORD(WINAPI*)(HANDLE, LPVOID, LPSTR, DWORD);
using NtQueryThreadFn = NTSTATUS_(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

NtAllocFn g_alloc = nullptr;
NtFreeFn g_free = nullptr;
NtMapFn g_map = nullptr;
NtUnmapFn g_unmap = nullptr;
RtlCreateHeapFn g_create_heap = nullptr;
BackTraceFn g_backtrace = nullptr;
MappedNameFn g_mapped_name = nullptr;
NtQueryThreadFn g_query_thread = nullptr;

int g_period = 0;  // seconds between reports; 0: off
uintptr_t g_self_lo = 0, g_self_hi = 0, g_ntdll = 0, g_kbase = 0, g_k32 = 0;
volatile LONG g_busy = 0;       // one report at a time (static buffers)
volatile LONG g_fail_dumps = 0, g_crash_dumps = 0;
uintptr_t g_max_addr = 0x7FFEFFFF;

// ---- who asked: allocation base -> the calling module's base (open addressing, never resized, no lock: a lost
// or stale entry only costs a label).
constexpr unsigned kCallers = 1u << 15;
struct Caller { volatile uintptr_t base; volatile uintptr_t module; };
Caller g_callers[kCallers];
constexpr unsigned kHeaps = 256;
struct HeapBy { volatile uintptr_t heap; volatile uintptr_t module; };
HeapBy g_heaps[kHeaps];
volatile LONG g_heap_n = 0;

uintptr_t ModuleOf(uintptr_t a) {
  MEMORY_BASIC_INFORMATION mbi;
  if (a < 0x10000 || !VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) || mbi.Type != MEM_IMAGE) return 0;
  return reinterpret_cast<uintptr_t>(mbi.AllocationBase);
}

// The module that asked: the first word on the stack above the hook that is a return address (a call instruction
// ends just before it) into a module other than ntdll, kernelbase, kernel32 and this DLL. The frame chain is no
// use here: DXVK and this DLL are built without frame pointers. 0: none found.
uintptr_t CallerModule(const uintptr_t* sp) {
  uintptr_t last = 0;
  const uintptr_t lo = reinterpret_cast<uintptr_t>(sp);
  const uintptr_t hi = (lo | 0xFFF) + 1 + 0x1000;  // this page and the next: both of the stack in practice
  MEMORY_BASIC_INFORMATION mbi;
  uintptr_t end = lo;
  if (VirtualQuery(sp, &mbi, sizeof mbi)) end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
  if (end > lo + 0x800) end = lo + 0x800;
  (void)hi;
  for (const uintptr_t* p = sp; reinterpret_cast<uintptr_t>(p) + 4 <= end; ++p) {
    const uintptr_t v = *p;
    if (v < 0x10000 || (v >= lo - 0x100000 && v < lo + 0x100000)) continue;
    if (!VirtualQuery(reinterpret_cast<void*>(v), &mbi, sizeof mbi) || mbi.Type != MEM_IMAGE || mbi.State != MEM_COMMIT ||
        !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
      continue;
    if (v - reinterpret_cast<uintptr_t>(mbi.BaseAddress) < 8) continue;
    const uint8_t* c = reinterpret_cast<const uint8_t*>(v);
    const bool call = c[-5] == 0xE8 || (c[-6] == 0xFF && (c[-5] & 0x38) == 0x10) || (c[-2] == 0xFF && (c[-1] & 0x38) == 0x10) ||
                      (c[-3] == 0xFF && (c[-2] & 0x38) == 0x10) || (c[-7] == 0xFF && (c[-6] & 0x38) == 0x10);
    if (!call) continue;
    const uintptr_t m = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
    last = m;
    if (m == g_ntdll || m == g_kbase || m == g_k32 || (m >= g_self_lo && m < g_self_hi)) continue;
    return m;
  }
  return last;  // only system frames: name the system DLL
}
#define SS_CALLER() CallerModule(reinterpret_cast<const uintptr_t*>(__builtin_frame_address(0)))

void NoteCaller(uintptr_t base, uintptr_t module) {
  unsigned i = (base >> 12) * 2654435761u >> 17;
  for (unsigned k = 0; k < 64; ++k, i = (i + 1) & (kCallers - 1)) {
    const uintptr_t b = g_callers[i].base;
    if (b == base || b == 0) {
      g_callers[i].module = module;
      g_callers[i].base = base;
      return;
    }
  }
}
uintptr_t CallerOf(uintptr_t base) {
  unsigned i = (base >> 12) * 2654435761u >> 17;
  for (unsigned k = 0; k < 64; ++k, i = (i + 1) & (kCallers - 1)) {
    const uintptr_t b = g_callers[i].base;
    if (b == base) return g_callers[i].module;
    if (b == 0) return 0;
  }
  return 0;
}

uint32_t Report(const char* why);

void Failed(const char* what, SIZE_T size, NTSTATUS_ st) {
  if (InterlockedIncrement(&g_fail_dumps) > 4) return;
  char why[160];
  snprintf(why, sizeof why, "%s of %lu kB failed (status %08lx)", what, static_cast<unsigned long>(size >> 10),
           static_cast<unsigned long>(st));
  Report(why);
}

NTSTATUS_ NTAPI HookAlloc(HANDLE p, PVOID* base, ULONG_PTR zero, SIZE_T* size, ULONG type, ULONG protect) {
  const SIZE_T asked = size ? *size : 0;
  const bool fresh = base && !*base;  // no address given: a failure is a lack of space, not a taken address
  const NTSTATUS_ st = g_alloc(p, base, zero, size, type, protect);
  if (p == GetCurrentProcess()) {
    if (st >= 0 && (type & MEM_RESERVE) && base && *base)
      NoteCaller(reinterpret_cast<uintptr_t>(*base) & ~uintptr_t(0xFFFF), SS_CALLER());
    else if (st < 0 && fresh)
      Failed("VirtualAlloc", asked, st);
  }
  return st;
}
NTSTATUS_ NTAPI HookFree(HANDLE p, PVOID* base, SIZE_T* size, ULONG type) {
  const uintptr_t b = base ? reinterpret_cast<uintptr_t>(*base) : 0;
  const NTSTATUS_ st = g_free(p, base, size, type);
  if (st >= 0 && p == GetCurrentProcess() && (type & MEM_RELEASE) && b && CallerOf(b)) NoteCaller(b, 0);
  return st;
}
NTSTATUS_ NTAPI HookMap(HANDLE sec, HANDLE p, PVOID* base, ULONG_PTR zero, SIZE_T commit, PLARGE_INTEGER off,
                        SIZE_T* view, DWORD inherit, ULONG type, ULONG protect) {
  const SIZE_T asked = view ? *view : 0;
  const NTSTATUS_ st = g_map(sec, p, base, zero, commit, off, view, inherit, type, protect);
  if (p == GetCurrentProcess()) {
    if (st >= 0 && base && *base)
      NoteCaller(reinterpret_cast<uintptr_t>(*base), SS_CALLER());
    else if (st < 0)
      Failed("MapViewOfFile", asked, st);
  }
  return st;
}
NTSTATUS_ NTAPI HookUnmap(HANDLE p, PVOID base) {
  const NTSTATUS_ st = g_unmap(p, base);
  const uintptr_t b = reinterpret_cast<uintptr_t>(base) & ~uintptr_t(0xFFFF);
  if (st >= 0 && p == GetCurrentProcess() && b && CallerOf(b)) NoteCaller(b, 0);
  return st;
}
PVOID NTAPI HookCreateHeap(ULONG flags, PVOID base, SIZE_T reserve, SIZE_T commit, PVOID lock, PVOID params) {
  const PVOID h = g_create_heap(flags, base, reserve, commit, lock, params);
  if (h) {
    const LONG k = InterlockedIncrement(&g_heap_n) - 1;
    if (k < static_cast<LONG>(kHeaps)) {
      g_heaps[k].module = SS_CALLER();
      g_heaps[k].heap = reinterpret_cast<uintptr_t>(h);
    }
  }
  return h;
}

// Redirects kernelbase.dll's imports of the five ntdll functions. Returns how many slots were taken.
int HookImports() {
  struct Want { const char* name; uintptr_t hook; uintptr_t* orig; } want[] = {
      {"NtAllocateVirtualMemory", reinterpret_cast<uintptr_t>(HookAlloc), reinterpret_cast<uintptr_t*>(&g_alloc)},
      {"NtFreeVirtualMemory", reinterpret_cast<uintptr_t>(HookFree), reinterpret_cast<uintptr_t*>(&g_free)},
      {"NtMapViewOfSection", reinterpret_cast<uintptr_t>(HookMap), reinterpret_cast<uintptr_t*>(&g_map)},
      {"NtUnmapViewOfSection", reinterpret_cast<uintptr_t>(HookUnmap), reinterpret_cast<uintptr_t*>(&g_unmap)},
      {"RtlCreateHeap", reinterpret_cast<uintptr_t>(HookCreateHeap), reinterpret_cast<uintptr_t*>(&g_create_heap)},
  };
  int taken = 0;
  for (Want& w : want)
    if (ss_hook_import("kernelbase.dll", "ntdll.dll", w.name, w.hook, w.orig)) ++taken;
  return taken;
}

// ---- the report
struct Alloc {
  uintptr_t base;
  uintptr_t first;    // its first committed address
  uint32_t size, commit;
  uint32_t type;      // MEM_IMAGE / MEM_MAPPED / MEM_PRIVATE
  uint32_t prot;      // of the largest committed region
  uint32_t guard;     // has a guard page (a stack)
  uintptr_t heap;     // the NT heap it belongs to (0: none), kHeapUnknown: a large block whose heap was not found
  int owner;
};
constexpr uintptr_t kHeapUnknown = 1, kHeap64 = 2;
constexpr int kAllocs = 24000;
Alloc g_a[kAllocs];
int g_an = 0;
struct Owner { char name[72]; uint32_t commit, reserve, n; };
constexpr int kOwners = 600;
Owner g_o[kOwners];
int g_on = 0;
struct Stack { uintptr_t base, module; };
constexpr int kStacks = 512;
Stack g_stacks[kStacks];
volatile LONG g_stack_n = 0;
int g_threads = 0;
char g_out[24000];
int g_len = 0;

void Out(const char* fmt, ...) {
  if (g_len > static_cast<int>(sizeof g_out) - 300) return;
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(g_out + g_len, sizeof g_out - g_len - 2, fmt, ap);
  va_end(ap);
  if (n > 0) g_len += n < static_cast<int>(sizeof g_out) - g_len - 2 ? n : static_cast<int>(sizeof g_out) - g_len - 2;
  g_out[g_len++] = '\n';
  g_out[g_len] = 0;
}

bool Peek(uintptr_t a, void* out, size_t n) {
  SIZE_T got = 0;
  return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(a), out, n, &got) && got == n;
}

// File name (no folder) of the image or mapped file at a; "" when it has none.
void FileAt(uintptr_t a, char* out, size_t n) {
  out[0] = 0;
  char path[520];
  if (!g_mapped_name) return;
  const DWORD len = g_mapped_name(GetCurrentProcess(), reinterpret_cast<void*>(a), path, sizeof path - 1);
  if (!len) return;
  path[len] = 0;
  const char* s = strrchr(path, '\\');
  snprintf(out, n, "%s", s ? s + 1 : path);
}

int OwnerId(const char* name) {
  for (int i = 0; i < g_on; ++i)
    if (!strcmp(g_o[i].name, name)) return i;
  if (g_on >= kOwners) return kOwners - 1;
  snprintf(g_o[g_on].name, sizeof g_o[g_on].name, "%s", name);
  g_o[g_on].commit = g_o[g_on].reserve = g_o[g_on].n = 0;
  return g_on++;
}

int FindAlloc(uintptr_t base) {
  int lo = 0, hi = g_an - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    if (g_a[mid].base == base) return mid;
    if (g_a[mid].base < base) lo = mid + 1; else hi = mid - 1;
  }
  return -1;
}

// An NT heap segment starts with a block header, then the signature 0xFFEEFFEE at +8 and its heap at +0x18.
uintptr_t SegmentHeap(uintptr_t base) {
  uint32_t w[8];
  if (!Peek(base, w, sizeof w)) return 0;
  if (w[2] == 0xFFEEFFEE) return w[6];
  if (w[4] == 0xFFEEFFEE) return kHeap64;  // the 64-bit layout: a heap of the WoW64 side of the process
  return 0;
}

// A large heap block is its own allocation. Its header lies at the first committed address (Windows leaves a
// reserved gap of random size before it): a list entry, 8 bytes, then its commit and reserve sizes. The list leads
// to its head inside the heap's first segment; every block passed on the way gets the same heap.
void LargeBlockHeap(int i) {
  Alloc& a = g_a[i];
  uint32_t w[6];
  if (!a.first || !Peek(a.first, w, sizeof w) || w[5] > a.size || w[5] < a.commit || w[4] > w[5] || w[4] + 0x2000 < a.commit ||
      w[0] < 0x10000 || w[1] < 0x10000)
    return;
  int seen[256], ns = 0;
  seen[ns++] = i;
  uintptr_t heap = kHeapUnknown, p = w[0];
  for (int step = 0; step < 20000; ++step) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
    const uintptr_t ab = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
    const int j = FindAlloc(ab);
    if (j < 0) break;
    if (g_a[j].first != p) {  // not a block's header: the list head, inside a segment of the heap
      const uintptr_t h = g_a[j].heap ? g_a[j].heap : SegmentHeap(ab);
      if (h) heap = h;
      break;
    }
    if (g_a[j].heap) { heap = g_a[j].heap; break; }
    if (ns < 256) seen[ns++] = j;
    uint32_t next;
    if (!Peek(p, &next, 4) || next < 0x10000) break;
    p = next;
  }
  for (int k = 0; k < ns; ++k) g_a[seen[k]].heap = heap;
}

void ModuleName(uintptr_t m, char* out, size_t n) {
  if (!m) { snprintf(out, n, "?"); return; }
  if (m == 0x400000) { snprintf(out, n, "SplitSecond.exe"); return; }
  FileAt(m, out, n);
  if (!out[0]) snprintf(out, n, "module %08lx", static_cast<unsigned long>(m));
}

void Label(Alloc& a, char* out, size_t n) {
  char f[96];
  if (a.type == MEM_IMAGE) {
    FileAt(a.base, f, sizeof f);
    snprintf(out, n, "image %s", f[0] ? f : "?");
    return;
  }
  const uintptr_t who = CallerOf(a.base);
  if (a.type == MEM_MAPPED) {
    FileAt(a.base, f, sizeof f);
    char m[64];
    ModuleName(who, m, sizeof m);
    if (f[0] && who) snprintf(out, n, "mapped file by %s", m);
    else if (f[0]) snprintf(out, n, "mapped file, no caller seen (%s)", strlen(f) > 4 ? f + strlen(f) - 4 : f);
    else if (who) snprintf(out, n, "mapped pagefile by %s", m);
    else snprintf(out, n, "mapped pagefile, no caller seen");
    return;
  }
  if (a.heap) {
    char m[64] = "";
    const uintptr_t ph = reinterpret_cast<uintptr_t>(GetProcessHeap());
    if (a.heap == kHeapUnknown) { snprintf(out, n, "heap large blocks (heap not found)"); return; }
    if (a.heap == kHeap64) { snprintf(out, n, "heaps of the 64-bit side (WoW64)"); return; }
    if (a.heap == ph) snprintf(m, sizeof m, "the process heap");
    for (LONG k = 0, e = g_heap_n < static_cast<LONG>(kHeaps) ? g_heap_n : kHeaps; k < e && !m[0]; ++k)
      if (g_heaps[k].heap == a.heap) {
        char mm[48];
        ModuleName(g_heaps[k].module, mm, sizeof mm);
        snprintf(m, sizeof m, "made by %s", mm);
      }
    if (!m[0]) snprintf(m, sizeof m, "made before the log");
    snprintf(out, n, "heap %08lx (%s)", static_cast<unsigned long>(a.heap), m);
    return;
  }
  for (LONG k = 0, e = g_stack_n; k < e; ++k)
    if (g_stacks[k].base == a.base) {
      char m[64];
      ModuleName(g_stacks[k].module, m, sizeof m);
      snprintf(out, n, "thread stacks (%s)", m);
      return;
    }
  if (a.guard) { snprintf(out, n, "thread stacks (64-bit side or thread not listed)"); return; }
  if (who) {
    char m[64];
    ModuleName(who, m, sizeof m);
    snprintf(out, n, "private by %s", m);
    return;
  }
  snprintf(out, n, "private, no caller seen");
}

// Thread stacks: each thread's stack allocation (TEB + 0xE0C) and the module it started in. Makes heap
// allocations (the snapshot): only from the log's own thread.
void Stacks() {
  if (!g_query_thread) return;
  const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te;
  te.dwSize = sizeof te;
  const DWORD pid = GetCurrentProcessId();
  LONG n = 0;
  int threads = 0;
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid) continue;
    ++threads;
    const HANDLE t = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
    if (!t) continue;
    uint32_t tbi[7] = {};
    uintptr_t start = 0, stack = 0;
    if (g_query_thread(t, 0, tbi, sizeof tbi, nullptr) >= 0 && tbi[1]) Peek(tbi[1] + 0xE0C, &stack, 4);
    g_query_thread(t, 9, &start, sizeof start, nullptr);
    CloseHandle(t);
    if (stack && n < kStacks) {
      g_stacks[n].module = ModuleOf(start);
      g_stacks[n].base = stack;
      ++n;
    }
  }
  CloseHandle(snap);
  g_stack_n = n;
  g_threads = threads;
}

// Walks the address space into g_a; returns the free total, fills the largest free blocks.
uint32_t Walk(uint32_t* free_top, int ntop, int* free_blocks) {
  g_an = 0;
  for (int i = 0; i < ntop; ++i) free_top[i] = 0;
  *free_blocks = 0;
  uint32_t free_total = 0;
  uintptr_t a = 0x10000;
  MEMORY_BASIC_INFORMATION mbi;
  Alloc* cur = nullptr;
  uint32_t best = 0;
  while (a < g_max_addr && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi)) {
    const uint32_t sz = static_cast<uint32_t>(mbi.RegionSize);
    if (mbi.State == MEM_FREE) {
      free_total += sz;
      ++*free_blocks;
      uint32_t v = sz;
      for (int i = 0; i < ntop; ++i)
        if (v > free_top[i]) { const uint32_t t = free_top[i]; free_top[i] = v; v = t; }
      cur = nullptr;
    } else {
      const uintptr_t ab = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
      if (!cur || cur->base != ab) {
        if (g_an >= kAllocs) break;
        cur = &g_a[g_an++];
        *cur = Alloc{ab, 0, 0, 0, static_cast<uint32_t>(mbi.Type), 0, 0, 0, -1};
        best = 0;
      }
      cur->size += sz;
      if (mbi.State == MEM_COMMIT) {
        if (!cur->commit) cur->first = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        cur->commit += sz;
        if (mbi.Protect & PAGE_GUARD) cur->guard = 1;
        if (sz > best) { best = sz; cur->prot = mbi.Protect; }
      }
    }
    a = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + sz;
    if (!sz) break;
  }
  return free_total;
}

unsigned Mb(uint64_t b) { return static_cast<unsigned>((b + (1u << 19)) >> 20); }

// Returns the free space it found (0: another report was under way).
uint32_t Report(const char* why) {
  for (int i = 0; InterlockedCompareExchange(&g_busy, 1, 0) != 0; ++i) {
    if (i > 300) return 0;
    Sleep(1);
  }
  g_len = 0;
  uint32_t top[6];
  int blocks = 0;
  const uint32_t free_total = Walk(top, 6, &blocks);
  uint64_t ic = 0, mc = 0, mr = 0, pc = 0, pr = 0;
  g_on = 0;
  for (int i = 0; i < g_an; ++i) {
    Alloc& a = g_a[i];
    if (a.type != MEM_PRIVATE) continue;
    const uintptr_t h = SegmentHeap(a.base);
    if (h) a.heap = h;
  }
  for (int i = 0; i < g_an; ++i)
    if (g_a[i].type == MEM_PRIVATE && !g_a[i].heap && !g_a[i].guard) LargeBlockHeap(i);
  for (int i = 0; i < g_an; ++i) {
    Alloc& a = g_a[i];
    const uint32_t res = a.size - a.commit;
    if (a.type == MEM_IMAGE) ic += a.size;
    else if (a.type == MEM_MAPPED) { mc += a.commit; mr += res; }
    else { pc += a.commit; pr += res; }
    char name[96];
    Label(a, name, sizeof name);
    a.owner = OwnerId(name);
    Owner& o = g_o[a.owner];
    o.commit += a.commit;
    o.reserve += res;
    ++o.n;
  }
  Out("[vm] %s: free %u MB in %d blocks, largest %u MB (then %u, %u, %u, %u, %u); image %u MB; mapped %u MB + %u reserved; "
      "private %u MB + %u reserved; %d allocations, %d threads",
      why, Mb(free_total), blocks, Mb(top[0]), Mb(top[1]), Mb(top[2]), Mb(top[3]), Mb(top[4]), Mb(top[5]), Mb(ic), Mb(mc),
      Mb(mr), Mb(pc), Mb(pr), g_an, g_threads);
  // The two modules with the most reserved space: their allocations one by one, largest first (size, of which
  // committed), so a reservation that is never used shows.
  for (int pass = 0, done = -1; pass < 2; ++pass) {
    int b = -1;
    for (int i = 0; i < g_on; ++i)
      if (i != done && !strncmp(g_o[i].name, "private by ", 11) && g_o[i].reserve >= (16u << 20) && (b < 0 || g_o[i].reserve > g_o[b].reserve)) b = i;
    if (b < 0) break;
    done = b;
    char line[700];
    int ll = 0;
    uint32_t below = 0xFFFFFFFF;
    uintptr_t below_base = 0;
    for (int k = 0; k < 14 && ll < 640; ++k) {
      int best = -1;
      for (int i = 0; i < g_an; ++i) {
        const Alloc& x = g_a[i];
        if (x.owner != b || x.size > below || (x.size == below && x.base <= below_base)) continue;
        if (best < 0 || x.size > g_a[best].size || (x.size == g_a[best].size && x.base < g_a[best].base)) best = i;
      }
      if (best < 0) break;
      below = g_a[best].size;
      below_base = g_a[best].base;
      ll += snprintf(line + ll, sizeof line - ll, " %ukB(%u)", g_a[best].size >> 10, g_a[best].commit >> 10);
    }
    Out("[vm]   %s, its largest (size kB (committed kB)):%s", g_o[b].name, line);
  }
  // owners, largest first; those under 2 MB summed
  uint64_t rest = 0;
  int nrest = 0;
  for (int shown = 0;; ++shown) {
    int b = -1;
    uint64_t bv = 0;
    for (int i = 0; i < g_on; ++i) {
      const uint64_t v = static_cast<uint64_t>(g_o[i].commit) + g_o[i].reserve;
      if (g_o[i].n && v >= bv) { bv = v; b = i; }
    }
    if (b < 0) break;
    Owner& o = g_o[b];
    if (bv < (2u << 20) || shown >= 70) { rest += bv; nrest += o.n; }
    else Out("[vm]   %5u MB + %4u reserved  %5u x  %s", Mb(o.commit), Mb(o.reserve), o.n, o.name);
    o.n = 0;
  }
  Out("[vm]   %5u MB in %d allocations of smaller owners", Mb(rest), nrest);
  // the largest allocations nobody in the process asked for, and their sizes
  char seen_line[900];
  int sl = 0, cnt = 0;
  for (int k = 0; k < 14; ++k) {
    int b = -1;
    for (int i = 0; i < g_an; ++i) {
      const Alloc& a = g_a[i];
      if (a.owner < 0 || !strstr(g_o[a.owner].name, "no caller seen")) continue;
      if (a.size && (b < 0 || a.size > g_a[b].size)) b = i;
    }
    if (b < 0) break;
    Alloc& a = g_a[b];
    sl += snprintf(seen_line + sl, sizeof seen_line - sl, " %08lx %s %ukB/%ukB prot %x;", static_cast<unsigned long>(a.base),
                   a.type == MEM_MAPPED ? "map" : "prv", a.commit >> 10, a.size >> 10, a.prot);
    if (getenv("SS_VM_DEBUG")) {
      uint32_t w[8] = {};
      Peek(a.first, w, sizeof w);
      sl += snprintf(seen_line + sl, sizeof seen_line - sl, " [%x %x %x %x %x %x %x %x]", w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    }
    a.size = 0;  // taken (the table is rebuilt by the next report)
    ++cnt;
    if (sl > 800) break;
  }
  if (cnt) Out("[vm]   largest with no caller (base kind committed/size):%s", seen_line);
  ss_log_raw(g_out);
  InterlockedExchange(&g_busy, 0);
  return free_total;
}

DWORD WINAPI Thread(LPVOID) {
  DWORD last_report = 0;
  uint32_t last_free = 0xFFFFFFFF;
  bool first = true, low64 = false, low16 = false, free64 = false;
  for (;;) {
    // a light look twice a second: only free space and the largest block
    uint32_t free_total = 0, largest = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t a = 0x10000; a < g_max_addr && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi);
         a = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
      if (mbi.State == MEM_FREE) {
        free_total += mbi.RegionSize;
        if (mbi.RegionSize > largest) largest = static_cast<uint32_t>(mbi.RegionSize);
      }
      if (!mbi.RegionSize) break;
    }
    const DWORD now = GetTickCount();
    const char* why = nullptr;
    if (first) why = "start";
    else if (now - last_report >= static_cast<DWORD>(g_period) * 1000) why = "periodic";
    else if (free_total + (96u << 20) < last_free) why = "free space fell";
    else if (!free64 && free_total < (64u << 20)) { why = "free space under 64 MB"; free64 = true; }
    else if (!low64 && largest < (64u << 20)) { why = "largest free block under 64 MB"; low64 = true; }
    else if (!low16 && largest < (16u << 20)) { why = "largest free block under 16 MB"; low16 = true; }
    if (why) {
      Stacks();
      const uint32_t f = Report(why);
      first = false;
      last_report = now;
      last_free = f ? f : free_total;
    }
    Sleep(500);
  }
  return 0;
}

}  // namespace

// Redirects one named import of a loaded module (a slot of its import table: `module` calls `name` of `from`
// through it). *original is set before the slot changes, because the hook may run at once. False: not found or
// not writable (*original is 0 again).
bool ss_hook_import(const char* module, const char* from, const char* name, uintptr_t hook, uintptr_t* original) {
  const uintptr_t m = reinterpret_cast<uintptr_t>(GetModuleHandleA(module));
  if (!m) return false;
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(m + dos->e_lfanew);
  const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return false;
  for (auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(m + dir.VirtualAddress); d->Name; ++d) {
    if (_stricmp(reinterpret_cast<const char*>(m + d->Name), from) || !d->OriginalFirstThunk) continue;
    auto* names = reinterpret_cast<const IMAGE_THUNK_DATA32*>(m + d->OriginalFirstThunk);
    auto* slots = reinterpret_cast<uintptr_t*>(m + d->FirstThunk);
    for (int i = 0; names[i].u1.AddressOfData; ++i) {
      if (names[i].u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
      if (strcmp(reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(m + names[i].u1.AddressOfData)->Name, name)) continue;
      *original = slots[i];
      if (ss_write_word(&slots[i], hook)) return true;
      *original = 0;
      return false;
    }
  }
  return false;
}

// From the crash line's handler (patch.cpp): the address space at the first exceptions.
void ss_vm_crash() {
  if (!g_period || InterlockedIncrement(&g_crash_dumps) > 2) return;
  Report("at the exception");
}

void ss_vm_start(void* self) {
  const char* e = getenv("SS_VM_LOG");
  const std::string v = e ? e : ss_conf("vm_log");
  if (v.empty() || v == "0" || v == "false" || v == "off" || v == "no") return;
  g_period = atoi(v.c_str());
  if (g_period < 2) g_period = 10;
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  g_max_addr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
  g_self_lo = reinterpret_cast<uintptr_t>(self);
  g_self_hi = g_self_lo + 1;
  MEMORY_BASIC_INFORMATION mbi;
  if (VirtualQuery(self, &mbi, sizeof mbi)) g_self_lo = g_self_hi = reinterpret_cast<uintptr_t>(mbi.AllocationBase), ++g_self_hi;
  const HMODULE nt = GetModuleHandleA("ntdll.dll"), kb = GetModuleHandleA("kernelbase.dll"), k32 = GetModuleHandleA("kernel32.dll");
  g_ntdll = reinterpret_cast<uintptr_t>(nt);
  g_kbase = reinterpret_cast<uintptr_t>(kb);
  g_k32 = reinterpret_cast<uintptr_t>(k32);
  if (nt) {
    g_backtrace = reinterpret_cast<BackTraceFn>(GetProcAddress(nt, "RtlCaptureStackBackTrace"));
    g_query_thread = reinterpret_cast<NtQueryThreadFn>(GetProcAddress(nt, "NtQueryInformationThread"));
  }
  if (k32) g_mapped_name = reinterpret_cast<MappedNameFn>(GetProcAddress(k32, "K32GetMappedFileNameA"));
  const int taken = HookImports();
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandleA(nullptr));
  const auto* h = reinterpret_cast<const IMAGE_NT_HEADERS32*>(reinterpret_cast<uintptr_t>(dos) + dos->e_lfanew);
  ss_log("[vm] vm_log on: a report every %d s; address space up to %p (the exe %s large-address-aware); %d of 5 "
         "kernelbase.dll imports redirected (who asked for an allocation)",
         g_period, reinterpret_cast<void*>(g_max_addr),
         (h->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) ? "is" : "is not", taken);
  CreateThread(nullptr, 0, Thread, nullptr, 0, nullptr);
}
