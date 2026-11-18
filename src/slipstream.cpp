// slipstream.cpp: the draft streaks at the front of the player's car (slipstream_fix; SSPC-31).
//
//   slipstream_fix = true | false   (true)   SS_SLIPSTREAM_FIX; read at start
//
// On the Xbox 360 translucent white streaks run beside the car's front corners while it drafts another car; the PC
// release shows at most a faint flicker at the nose. The game's code is the same; two things in the PC release's data
// are wrong. Reverse engineering of the Steam build (run-time VAs):
//
// 1. The streak texture. slip_stream's material (shader instance 0 of VehicleParticles, technique 0x7ec6e87c,
//    volume_alphablend_rg) samples the volume texture 0x03D53EF3 (256x256x16 DXT1, one mip) at w = animation frame /
//    16 (uvslideshow_volume: 16 frames, 10 a second, a random start per particle); the pixel shader keeps red as the
//    colour, green as the alpha and kills every pixel with green < 1/255. In the PC resident.ark (texture set
//    a2a0cd1d) only slices 0-3 of it hold data and slices 4-15 are zero bytes, and those 4 are not frames 0-3 either:
//    the PC conversion of the tiled volume went wrong, slice s holds every other block row (0, 2, .. 62) of frames 4s
//    (block rows 0-31) and 4s+1 (32-63), two half-height frames per slice; frames 2, 3, 6, 7, 10, 11, 14, 15 are in no
//    PC file. So 12 of the 16 frames are killed outright and the other 4 are squashed. The game loads the texture
//    through its d3dx9 import D3DXCreateVolumeTextureFromFileInMemory (IAT word 0xB9B3C4, called from 0x511546).
//    The fix takes that import slot and, only for a 256x256x16 DXT1 volume with one mip whose slices from 4 on are
//    all zero and whose 4 filled slices show that interleave (blank seam rows, streaks in both halves), hands d3dx9 16
//    frames rebuilt from those slices (streak_rebuild.cpp): the 8 present frames at full height (their own blocks
//    kept, the missing block rows interpolated and re-encoded), the 8 absent ones as left-right mirror images of the
//    present frame six later. Built from the user's own PC data only; every other texture passes through untouched.
//
// 2. The emitter definition (0x380 bytes; VehicleParticles.data, slip_stream is its first record, name at +0x2C0)
//    holds at +0x158 one byte "the particles move with their emitter". The three particle-system updates (0x7DF890,
//    0x7DD220, 0x7B6BE0) read it as a byte and, when it is set, add the emitter's movement since the last update
//    (instance position +0x40 minus the one before, +0x80; 0x7DF510) to every live particle; the emission (0x7BA8F0,
//    its copy at block+0x148) then hands a new particle a zero emitter velocity. The console data has 01 00 00 00 at
//    +0x158; the PC data has 00 00 00 01 (the four bytes turned round as if they were one 32-bit number). So on PC
//    the byte reads 0 and the streaks are not carried along with the car. The fix writes 1 into that byte of the
//    loaded definition (heap data), found through the draft effect's slots once a race runs: race [0xD66B28] with a
//    vehicle manager (+0x50), vehicle particle managers [[0xD825EC] + 4 + 4 * i]; slots +0x3770 (kind 4, slip_stream)
//    and +0x4400 (kind 5): +0x10 instance count, +0x20 the instance pointers; instance +0x174 (or +8 while on) its
//    definition. Only a definition named slip_stream whose four bytes are 00 00 00 01 is written. The same misplaced
//    byte is in six more records (DEBRIS_COLLISION, CRASH_SPARKS_CRAZY, SPARKS_CRAZY, Drift_blue_light,
//    SMOKE_SLIDE_att, SMOKE_SLIDE_brown_att); they are left as they are.
//
// Nothing in the game's code is written: the import slot is a word of .rdata (as overlay.cpp takes its file imports)
// and every read of the game's data goes through ReadProcessMemory on the own process, so a wrong pointer cannot
// fault. Every step and every refusal is a [slip] line; on any refusal the game keeps its own behaviour.
#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "menu.h"
#include "ss_shared.h"
#include "streak_rebuild.h"

namespace {

volatile LONG g_fix_on = 0, g_started = 0;

bool ParseBool(std::string v) {
  for (char& c : v) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return v == "1" || v == "true" || v == "yes" || v == "on";
}

// ------------------------------------------------------------------------------------------- the streak texture

constexpr uintptr_t kIatCreateVolumeFromMemory = 0xB9B3C4;  // d3dx9_42!D3DXCreateVolumeTextureFromFileInMemory
constexpr uintptr_t kImageSectionsEnd = 0x118E000;
constexpr uint32_t kSteamDataHash = 0x2bf240b7;  // StreakHash of the Steam release's 4 filled slices
using CreateVolumeFn = long(__stdcall*)(void* device, const void* data, unsigned size, void** texture);
CreateVolumeFn g_orig_create_volume = nullptr;

bool AllZero(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i)
    if (p[i]) return false;
  return true;
}

// A DDS file in memory with the streak texture's shape: a 256x256x16 DXT1 volume with one mip.
bool StreakHeader(const uint8_t* d, unsigned size) {
  if (!d || size < 128 || memcmp(d, "DDS ", 4) != 0) return false;
  uint32_t f[31];
  memcpy(f, d + 4, sizeof f);  // f[i] = dword at 4 + 4 i
  const uint32_t height = f[2], width = f[3], depth = f[5], mips = f[6], fourcc = f[20], caps2 = f[27];
  return (caps2 & 0x200000) && width == kStreakW && height == kStreakH && depth == kStreakFrames && mips <= 1 &&
         fourcc == 0x31545844;  // DXT1
}

long __stdcall HkCreateVolume(void* device, const void* data, unsigned size, void** texture) {
  const uint8_t* d = static_cast<const uint8_t*>(data);
  if (!g_fix_on || !StreakHeader(d, size)) return g_orig_create_volume(device, data, size, texture);
  if (size < 128 + kStreakFrames * kStreakSliceBytes) {
    ss_log("[slip] texture: a 256x256x16 DXT1 volume of only %u bytes; left as it is", size);
    return g_orig_create_volume(device, data, size, texture);
  }
  const uint8_t* px = d + 128;
  if (AllZero(px, 4 * kStreakSliceBytes) || !AllZero(px + 4 * kStreakSliceBytes, (kStreakFrames - 4) * kStreakSliceBytes)) {
    ss_log("[slip] texture: a 256x256x16 DXT1 volume without the PC release's empty slices 4-15; left as it is");
    return g_orig_create_volume(device, data, size, texture);
  }
  const uint32_t hash = StreakHash(px);
  if (!StreakInterleaved(px)) {
    ss_log("[slip] texture: 256x256x16 DXT1 volume (data %08x) with slices 4-15 empty, but not the streak texture's "
           "interleaved layout; left as it is", hash);
    return g_orig_create_volume(device, data, size, texture);
  }
  uint8_t* copy = static_cast<uint8_t*>(malloc(size));
  if (!copy) {
    ss_log("[slip] texture: no memory for a copy (%u bytes); the streak texture is left as it is", size);
    return g_orig_create_volume(device, data, size, texture);
  }
  memcpy(copy, d, size);
  const DWORD t0 = GetTickCount();
  StreakRebuild(px, copy + 128);
  const DWORD ms = GetTickCount() - t0;
  const long hr = g_orig_create_volume(device, copy, size, texture);
  free(copy);  // d3dx9 has copied the bytes into the texture by now
  if (hr < 0) {
    ss_log("[slip] texture: d3dx9 refused the rebuilt streak texture (%08lx); the game's own data is loaded instead",
           static_cast<unsigned long>(hr));
    return g_orig_create_volume(device, data, size, texture);
  }
  ss_log("[slip] texture rebuilt: the streak texture 256x256x16 (PC data %08x, %s): 16 frames from the 8 half-height "
         "ones in %lu ms (rows interpolated, the 8 absent frames mirrored)",
         hash, hash == kSteamDataHash ? "the Steam release's" : "not the Steam release's 2bf240b7, same layout",
         static_cast<unsigned long>(ms));
  return hr;
}

// Takes the import slot as soon as the protection's start-up has filled it with d3dx9's function, then watches it
// for a minute and takes it again if it is filled once more (overlay.cpp does the same with its slots).
DWORD WINAPI TextureThread(LPVOID) {
  // The slot is filled about 1 s in while the protection still writes the rest of the import page: a write of ours
  // then (its protection lifted and put back) can fault the protection's next write (Windows test PC 2026-11-17, H5: the boot
  // crash SplitSecond.exe+0xE38B5D right after this slot's write, the relay's writes not yet made).
  ss_import_table_wait("slip");
  const DWORD t0 = GetTickCount();
  volatile uintptr_t* slot = reinterpret_cast<volatile uintptr_t*>(kIatCreateVolumeFromMemory);
  bool done = false;
  while (GetTickCount() - t0 < 60000) {
    const uintptr_t cur = *slot;
    if (cur != reinterpret_cast<uintptr_t>(&HkCreateVolume) && cur >= 0x10000 && !(cur >= 0x400000 && cur < kImageSectionsEnd)) {
      HMODULE m = GetModuleHandleA("d3dx9_42.dll");
      const uintptr_t want = m ? reinterpret_cast<uintptr_t>(GetProcAddress(m, "D3DXCreateVolumeTextureFromFileInMemory")) : 0;
      if (cur != want) {
        ss_log("[slip] texture: the import slot 0x%X holds %p, not d3dx9_42's function (%p); left alone, the streak "
               "texture stays as it is", static_cast<unsigned>(kIatCreateVolumeFromMemory), reinterpret_cast<void*>(cur),
               reinterpret_cast<void*>(want));
        return 0;
      }
      g_orig_create_volume = reinterpret_cast<CreateVolumeFn>(cur);
      if (!ss_write_word(const_cast<uintptr_t*>(slot), reinterpret_cast<uintptr_t>(&HkCreateVolume))) {
        ss_log("[slip] texture: cannot write the import slot 0x%X; the streak texture stays as it is",
               static_cast<unsigned>(kIatCreateVolumeFromMemory));
        return 0;
      }
      ss_log("[slip] texture: D3DXCreateVolumeTextureFromFileInMemory %s at import slot 0x%X after %lu ms",
             done ? "hooked again" : "hooked", static_cast<unsigned>(kIatCreateVolumeFromMemory), GetTickCount() - t0);
      done = true;
    }
    Sleep(done ? 50 : 1);
  }
  if (!done) ss_log("[slip] texture: the import slot was never filled; the streak texture stays as it is");
  return 0;
}

// ------------------------------------------------------------------------------------------- the emitter definition

constexpr uintptr_t kRaceRun = 0xD66B28;     // *: the running race
constexpr uintptr_t kRaceVehicleMgr = 0x50;  // race -> vehicle manager
constexpr uintptr_t kPartSet = 0xD825EC;     // *: +4 + 4i = vehicle i's particle manager
constexpr uintptr_t kSlotA = 0x3770, kSlotB = 0x4400;  // the draft effect's slots, kinds 4 (slip_stream) and 5
constexpr uintptr_t kSlotCount = 0x10, kSlotInstances = 0x20;
constexpr uintptr_t kInstDef = 0x174, kInstDefOn = 8;  // instance -> its emitter definition (0x84B640 copies it to +8)
constexpr uintptr_t kDefAttach = 0x158, kDefName = 0x2C0;
constexpr uint32_t kMaxInstances = 8;

// Reads n bytes of this process; false (and nothing used) when any of them is not readable.
bool Get(uintptr_t at, void* out, size_t n) {
  if (at < 0x10000) return false;
  SIZE_T got = 0;
  return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(at), out, n, &got) && got == n;
}
uint32_t U32(uintptr_t at) {
  uint32_t v = 0;
  return Get(at, &v, 4) ? v : 0;
}
bool Put(uintptr_t at, const void* in, size_t n) {
  SIZE_T put = 0;
  return at >= 0x10000 && WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(at), in, n, &put) && put == n;
}

// The loaded slip_stream definition, through the first cars' draft slots (every car shares the one record); 0 outside
// a race, while no slot has an instance, or when the record is not the known one (its name at +0x2C0).
uintptr_t StreamDef() {
  const uintptr_t race = U32(kRaceRun), set = U32(kPartSet);
  if (!race || !set || !U32(race + kRaceVehicleMgr)) return 0;
  for (int i = 0; i < 4; ++i) {
    const uintptr_t pm = U32(set + 4 + 4 * i);
    if (!pm) continue;
    for (const uintptr_t off : {kSlotA, kSlotB}) {
      const uint32_t n = U32(pm + off + kSlotCount);
      const uintptr_t inst = n >= 1 && n <= kMaxInstances ? U32(pm + off + kSlotInstances) : 0;
      if (!inst) continue;
      for (const uintptr_t at : {kInstDef, kInstDefOn}) {
        const uintptr_t def = U32(inst + at);
        char name[12];
        if (def && Get(def + kDefName, name, sizeof name) && memcmp(name, "slip_stream", 12) == 0) return def;
      }
    }
  }
  return 0;
}

DWORD WINAPI DefinitionThread(LPVOID) {
  uintptr_t told = 0;  // the definition a refusal was logged for (one line per loaded definition)
  for (;;) {
    const uintptr_t def = StreamDef();
    uint8_t b[4];
    if (def && Get(def + kDefAttach, b, 4)) {
      if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 1) {  // the PC data
        const uint8_t one = 1;
        if (Put(def + kDefAttach, &one, 1)) {
          ss_log("[slip] attach byte set: slip_stream definition %p +0x158 = 1 (the PC data has its 1 at +0x15B): the "
                 "streaks move with the car", reinterpret_cast<void*>(def));
        } else if (told != def) {
          ss_log("[slip] attach byte: cannot write slip_stream definition %p +0x158; left as it is", reinterpret_cast<void*>(def));
        }
        told = def;
      } else if (!(b[0] == 1 && b[1] == 0 && b[2] == 0 && b[3] == 1) && told != def) {  // 01 00 00 01: already set
        ss_log("[slip] attach byte: slip_stream definition %p +0x158 = %02x %02x %02x %02x, not the PC data's 00 00 00 01; "
               "left as it is", reinterpret_cast<void*>(def), b[0], b[1], b[2], b[3]);
        told = def;
      }
    }
    Sleep(100);
  }
  return 0;
}

}  // namespace

static MenuRegistrar g_slip_fix_row(MenuChoiceRow("slipstream_fix", "SLIPSTREAM EFFECT", MENU_PAGE_ADVANCED, MENU_RESTART, "true",
                                                  {{"false", "OFF"}, {"true", "ON"}}, nullptr));

void ss_slipstream_start() {
  if (InterlockedExchange(&g_started, 1)) return;
  const char* e = getenv("SS_SLIPSTREAM_FIX");
  const std::string v = e ? std::string(e) : ss_conf("slipstream_fix");  // absent = true
  g_fix_on = v.empty() || ParseBool(v);
  ss_log("[slip] slipstream_fix %s", g_fix_on ? "true" : "false");
  if (!g_fix_on) return;
  if (GetModuleHandleA(nullptr) != reinterpret_cast<HMODULE>(0x400000)) {
    ss_log("[slip] unexpected image base; nothing done");
    return;
  }
  CreateThread(nullptr, 0, TextureThread, nullptr, 0, nullptr);
  CreateThread(nullptr, 0, DefinitionThread, nullptr, 0, nullptr);
}
