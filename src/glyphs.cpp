// Button prompts for the pad in use (`button_prompts`), like the console port's.
//
// Sets: auto (default), xbox360, xbox (One / Series), playstation, nintendo, steam, keyboard.
//
// Where the prompts are (reverse engineered from the PC build and its archives):
//  * The prompt bar (UI/CommonUI/Prompt_Bar.gfx: the bar at the foot of every front-end screen, the popups and the
//    pause boxes) has an icon clip with the frames Xenon_A/B/X/Y/Start, PS3_Cross/Circle/Square/Triangle/Start,
//    PC_GenKey, PC_GenKeyLong and PC_KeyBlank. On every prompt change 0x4DADE0 calls the movie's
//    Invoke(path, result, args, 1) for the five icons ("main.bar_mc.button?_mc.icon_mc.gotoAndStop") with the one
//    shared argument "PC_GenKeyLong" (0xBBACA8): the PC build always shows key caps. Invoke is slot +0x48 of the
//    GFxMovieRoot vtable (0xC41D3C, read-only data, holds 0x98D2E0); the slot is pointed at a wrapper that swaps the
//    argument for the button's Xenon_* frame (every pad set) or PS3_* frame (playstation). One aligned 4-byte write
//    in read-only data; keyboard writes nothing.
//  * The Xenon frames draw the external images circle_buttons.tga (128x64: a row of the Xbox 360 discs Y B A X in
//    32-pixel cells over a row of the PS3 symbols) and the Start button (64x64), from the texture set
//    UI/CommonUI/Textures/CommonUI.textures (DDS, A8R8G8B8). For xbox, nintendo and steam those textures are drawn
//    again, as the game uploads them, by this DLL's own vector drawing (glyph_canvas.cpp; the same art as the console
//    port, no picture file anywhere). A texture is found by the FNV-1a hash of its first level's pixels; the
//    texture's LockRect / UnlockRect (the IDirect3DTexture9 and IDirect3DSurface9 vtables, in d3d9.dll's memory) and
//    the device's UpdateTexture (a staging upload) are wrapped. A set change re-writes the found textures at once
//    (from the Present hook); xbox360 puts the game's own pixels back.
//  * playstation uses the game's own PS3 frames; keyboard the game's own key caps (the bound key's name). Neither
//    swaps a texture.
//  * The Instant Replay banner (UI/HUD/Hud.gfx, mc_replayIcon.mc_replayiconAll.mc_buttonPrompt and its shadow
//    mc_buttonPromptShadow) shows on PC only the text "Instant Replay" (txt_buttonPrompt): the console's LB picture is
//    in the file but no frame draws it. Hud::cReplayIconElement (ctor 0x570460) shows the banner through the same
//    Invoke slot with the method name "CameraIconShow" (0xBD27AC; 0x45AB30 -> 0x536A70 -> vtable +0x48). After that
//    call the wrapper puts the bound input in front of both texts with the movie's own GetVariable / SetVariable
//    (vtable 0xC41CF4, +0x30 / +0x2C): "LB  Instant Replay" (pad: the set's name for the button), "[P]  Instant
//    Replay" (keyboard). The binding is the live one: the input manager *(0xD646C8) keeps one entry per device
//    (count +0xCF6C, array +0xCF70, 0x584 bytes each; {type, index} at +0x564, type 1 = keyboard; in use: byte
//    +0x56C) of 0x45 rows {action, key, pad control, pad kind, hat angle}; Instant Replay is action 51 (the Controls
//    screen's 12th row, row 38). The device is the menus' player's (*(*(0xD64E08) + 0x64), {type, index}) when it
//    is of the sort wanted, else the first of that sort. Pad kind 0 = button (index of the Xbox 360 pad layout, the
//    virtual pad's), kind 2 = the hat (control >= 128, angle in hundredths of a degree), 3 = none. Defaults: LB on an
//    Xbox 360 pad, P (arrow keys layout) or R (WASD layout) [0xCFABF8 / 0xCFB06C, row 38]. An axis binding or none:
//    the game's own text.
//
// auto: the last input decides keyboard or pad (a pad press or stick push -> pad; a key while the pad rests ->
// keyboard; at start the pad when one is connected). For a pad, the kind of the first pad: Valve (28DE, the Steam
// Deck's controls, a Steam Controller) -> steam; Sony (054C) -> playstation; Nintendo (057E) -> nintendo; an Xbox 360
// pad -> xbox360 (steam on a handheld); any other Xbox or pad -> xbox (steam on a handheld). Steam Input shows every
// pad as its virtual pad 28DE:11FF; when Steam names the real pad behind it (the file the SteamVirtualGamepadInfo
// variable points to: "[slot N]" sections with VID / PID / type), that one counts. Handheld = SteamDeck=1 in the
// environment or SS_HANDHELD=1. The pads come from the Windows device list (raw input HID game pads), looked at every
// two seconds, with the game's own DirectInput list as the fallback.
//
// Nothing of the game's code is written: one .rdata vtable slot, slots of d3d9.dll's vtables, and the banner's text
// through the movie's own calls.

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "glyph_art.h"
#include "glyph_canvas.h"
#include "menu.h"
#include "ss_shared.h"

using namespace ss_glyph;
using ss_glyph::Set;

namespace {

// ------------------------------------------------------------------------------------------ the sets


bool ParseSet(std::string v, Set* out) {
  for (char& c : v) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  for (int i = 0; i < kSetCount; ++i)
    if (v == kSetNames[i]) {
      *out = static_cast<Set>(i);
      return true;
    }
  // pad_glyphs, the key this row had before: pad = the Xbox 360 icons
  if (v == "pad" || v == "1") { *out = kXbox360; return true; }
  if (v == "0") { *out = kKeyboard; return true; }
  if (v == "2") { *out = kAuto; return true; }
  return false;
}

bool g_on = false;               // the feature is wanted at all (a conf or an SS_* switch exists)
volatile LONG g_chosen = kAuto;  // the setting
volatile LONG g_pad_in_use = 0;  // auto: the last input came from a pad
volatile LONG g_detected = kXbox360;  // auto: what the first pad stands for
volatile LONG g_effective = kKeyboard;
volatile LONG g_version = 1;     // goes up when the effective set changes (the textures are re-written)
DWORD g_di_product = 0;          // pad.cpp: the first real pad of the game's DirectInput list (PID<<16|VID)
CRITICAL_SECTION g_cs;

Set Resolve() {
  const Set chosen = static_cast<Set>(g_chosen);
  if (chosen != kAuto) return chosen;
  return g_pad_in_use ? static_cast<Set>(g_detected) : kKeyboard;
}

void Update(const char* why) {
  const Set now = Resolve();
  if (InterlockedExchange(&g_effective, now) != now) {
    InterlockedIncrement(&g_version);
    ss_log("[glyphs] button prompts: %s (setting %s; %s)", kSetNames[now], kSetNames[g_chosen], why);
  }
}

// ------------------------------------------------------------------------------- the prompt bar's frames

constexpr uintptr_t kInvokeSlot = 0xC41D3C;
constexpr uint32_t kInvokeOrig = 0x98D2E0;
constexpr uintptr_t kGenKeyLong = 0xBBACA8;
struct FrameSite {
  uintptr_t path;
  const char* text;
  const char* xenon;
  const char* ps3;
};
const FrameSite kFrameSites[5] = {
    {0xBBAC7C, "main.bar_mc.buttonA_mc.icon_mc.gotoAndStop", "Xenon_A", "PS3_Cross"},
    {0xBBAC50, "main.bar_mc.buttonB_mc.icon_mc.gotoAndStop", "Xenon_B", "PS3_Circle"},
    {0xBBAC24, "main.bar_mc.buttonX_mc.icon_mc.gotoAndStop", "Xenon_X", "PS3_Square"},
    {0xBBABF8, "main.bar_mc.buttonY_mc.icon_mc.gotoAndStop", "Xenon_Y", "PS3_Triangle"},
    {0xBBABC8, "main.bar_mc.buttonSTART_mc.icon_mc.gotoAndStop", "Xenon_Start", "PS3_Start"},
};
bool g_frames_checked = false, g_frames_ok = false, g_frames_hooked = false;
uint32_t g_frame_swaps = 0;

using InvokeFn = uint32_t(__attribute__((thiscall)) *)(void*, const char*, void*, const void*, uint32_t);

// ------------------------------------------------------------------------- the Instant Replay banner's label

constexpr uintptr_t kCameraIconShow = 0xBD27AC;  // "CameraIconShow", the replay element's method name
constexpr uint32_t kMovieVt = 0xC41CF4;          // GFxMovieRoot: +0x2C SetVariable, +0x30 GetVariable, +0x48 Invoke
constexpr uint32_t kSetVarOrig = 0x991010, kGetVarOrig = 0x98A970;
constexpr uintptr_t kInputManager = 0xD646C8;
constexpr uintptr_t kFrontEnd = 0xD64E08;
constexpr uint32_t kReplayAction = 51;
bool g_replay_ok = false;  // the banner's data and the movie vtable are the known build's (CheckFrameSites)

using SetVarFn = uint32_t(__attribute__((thiscall)) *)(void*, const char*, const uint32_t*, int);
using GetVarFn = uint32_t(__attribute__((thiscall)) *)(void*, uint32_t*, const char*);
const char* const kReplayFields[2] = {"mc_replayIcon.mc_replayiconAll.mc_buttonPrompt.txt_buttonPrompt.text",
                                      "mc_replayIcon.mc_replayiconAll.mc_buttonPromptShadow.txt_buttonPrompt.text"};
std::string g_replay_orig[2], g_replay_shown[2], g_replay_logged = "-";

// The device the menus' player uses ({type, index}; type 0 pad, 1 keyboard): the front end's input block
// (0x4D60F0, object *(0xD64E08)) passes *(+0x64) to every action query.
bool ActiveDevice(uint32_t* type, uint32_t* index) {
  const uintptr_t fe = *reinterpret_cast<const uintptr_t*>(kFrontEnd);
  if (!fe || IsBadReadPtr(reinterpret_cast<const void*>(fe + 0x64), 4)) return false;
  const uint32_t* d = *reinterpret_cast<const uint32_t* const*>(fe + 0x64);
  if (!d || IsBadReadPtr(d, 8) || d[0] > 1) return false;
  *type = d[0];
  *index = d[1];
  return true;
}

// The Instant Replay row {key, control, kind, angle} of the player's device when it is of that sort (keyboard or
// pad), else of the first device in use of that sort.
bool ReplayBinding(bool keyboard, uint32_t out[4]) {
  uint32_t type = 0, index = 0;
  const bool exact = ActiveDevice(&type, &index) && (type == 1) == keyboard;
  const uintptr_t mgr = *reinterpret_cast<const uintptr_t*>(kInputManager);
  if (!mgr || IsBadReadPtr(reinterpret_cast<const void*>(mgr + 0xCF6C), 8)) return false;
  const uint32_t count = *reinterpret_cast<const uint32_t*>(mgr + 0xCF6C);
  const uintptr_t arr = *reinterpret_cast<const uintptr_t*>(mgr + 0xCF70);
  if (!arr || !count || count > 16 || IsBadReadPtr(reinterpret_cast<const void*>(arr), count * 0x584)) return false;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* e = reinterpret_cast<const uint8_t*>(arr + i * 0x584);
    if (!e[0x56C] || (*reinterpret_cast<const uint32_t*>(e + 0x564) == 1) != keyboard) continue;
    if (exact && *reinterpret_cast<const uint32_t*>(e + 0x568) != index) continue;
    for (int r = 0; r < 0x45; ++r) {
      const uint32_t* row = reinterpret_cast<const uint32_t*>(e + r * 0x14);
      if (row[0] != kReplayAction) continue;
      memcpy(out, row + 1, 16);
      return keyboard ? out[0] != 0 : out[2] == 0 || out[2] == 2;
    }
  }
  return false;
}

std::string KeyName(uint32_t vk) {
  UINT sc = MapVirtualKeyA(vk, 0);  // MAPVK_VK_TO_VSC
  switch (vk) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
    case VK_PRIOR: case VK_NEXT: case VK_RCONTROL: case VK_RMENU: case VK_DIVIDE: case VK_NUMLOCK:
      sc |= 0x100;
  }
  char buf[48] = {};
  if (sc && GetKeyNameTextA(static_cast<LONG>(sc << 16), buf, sizeof buf) > 0) return buf;
  if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, static_cast<char>(vk));
  return "";
}

// What goes in front of the banner's text for the set in force ("" = the game's own text).
std::string ReplayLabel(Set set) {
  uint32_t b[4];
  if (set == kKeyboard) {
    if (!ReplayBinding(true, b)) return "";
    const std::string k = KeyName(b[0]);
    return k.empty() ? "" : "[" + k + "]";
  }
  if (!ReplayBinding(false, b)) return "";
  if (b[2] == 2) {  // the hat
    static const char* const dir[4] = {"Up", "Right", "Down", "Left"};
    return b[3] % 9000 || b[3] >= 36000 ? "" : std::string("D-pad ") + dir[b[3] / 9000];
  }
  // Buttons of the virtual pad (an Xbox 360 pad): A B X Y LB RB Back Start LS RS.
  static const char* const names[5][10] = {
      {"A", "B", "X", "Y", "LB", "RB", "Back", "Start", "LS", "RS"},                              // xbox360
      {"A", "B", "X", "Y", "LB", "RB", "View", "Menu", "LS", "RS"},                               // xbox
      {"Cross", "Circle", "Square", "Triangle", "L1", "R1", "Share", "Options", "L3", "R3"},      // playstation
      {"B", "A", "Y", "X", "L", "R", "-", "+", "LS", "RS"},                                       // nintendo (by position)
      {"A", "B", "X", "Y", "L1", "R1", "View", "Menu", "L3", "R3"},                               // steam
  };
  const int s = set - kXbox360;
  return s >= 0 && s < 5 && b[1] < 10 ? names[s][b[1]] : "";
}

// After the game's CameraIconShow: the bound input in front of both texts. A text that is still ours is not
// written again (the call may come every frame while the banner is up).
void LabelReplayBanner(void* movie) {
  const uint32_t* vt = *static_cast<const uint32_t* const*>(movie);
  if (reinterpret_cast<uintptr_t>(vt) != kMovieVt || vt[0x2C / 4] != kSetVarOrig || vt[0x30 / 4] != kGetVarOrig) return;
  const SetVarFn setv = reinterpret_cast<SetVarFn>(vt[0x2C / 4]);
  const GetVarFn getv = reinterpret_cast<GetVarFn>(vt[0x30 / 4]);
  const Set set = static_cast<Set>(g_effective);
  const std::string label = ReplayLabel(set);
  for (int i = 0; i < 2; ++i) {
    uint32_t v[4] = {};
    if (!(getv(movie, v, kReplayFields[i]) & 0xff) || v[0] != 4 || !v[2] || IsBadReadPtr(reinterpret_cast<const void*>(v[2]), 1)) continue;
    char cur[160] = {};
    lstrcpynA(cur, reinterpret_cast<const char*>(v[2]), sizeof cur);
    if (cur != g_replay_shown[i]) g_replay_orig[i] = cur;  // the movie's own text (a new banner or a new movie)
    const std::string want = label.empty() ? g_replay_orig[i] : label + "  " + g_replay_orig[i];
    g_replay_shown[i] = want;
    if (want == cur) continue;
    const uint32_t val[4] = {4, 0, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(want.c_str())), 0};
    setv(movie, kReplayFields[i], val, 0);
  }
  const std::string note = label + "|" + kSetNames[set];
  if (note != g_replay_logged) {
    g_replay_logged = note;
    ss_log("[glyphs] Instant Replay banner: '%s' (%s)", g_replay_shown[0].c_str(), kSetNames[set]);
  }
}

uint32_t __attribute__((thiscall)) InvokeHook(void* movie, const char* path, void* result, const void* args, uint32_t n) {
  const InvokeFn orig = reinterpret_cast<InvokeFn>(kInvokeOrig);
  if (reinterpret_cast<uintptr_t>(path) == kCameraIconShow && g_replay_ok && movie) {
    const uint32_t r = orig(movie, path, result, args, n);
    if (r & 0xff) LabelReplayBanner(movie);
    return r;
  }
  const Set set = static_cast<Set>(g_effective);
  if (set != kKeyboard && n == 1 && args) {
    const uint32_t* v = static_cast<const uint32_t*>(args);
    if (v[0] == 4 && v[2] == kGenKeyLong) {
      for (const FrameSite& g : kFrameSites) {
        if (reinterpret_cast<uintptr_t>(path) != g.path) continue;
        uint32_t copy[4];
        memcpy(copy, args, sizeof copy);
        copy[2] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(set == kPlayStation ? g.ps3 : g.xenon));
        if (g_frame_swaps++ == 0) ss_log("[glyphs] first prompt bar drawn with controller icons (%s)", kSetNames[set]);
        return orig(movie, path, result, copy, n);
      }
    }
  }
  return orig(movie, path, result, args, n);
}

bool CheckFrameSites() {
  if (g_frames_checked) return g_frames_ok;
  bool ok = GetModuleHandleA(nullptr) == reinterpret_cast<HMODULE>(0x400000) &&
            !IsBadReadPtr(reinterpret_cast<const void*>(kInvokeSlot), 4) && !IsBadReadPtr(reinterpret_cast<const void*>(kGenKeyLong), 14) &&
            memcmp(reinterpret_cast<const void*>(kGenKeyLong), "PC_GenKeyLong", 14) == 0;
  for (int i = 0; ok && i < 5; ++i) {
    size_t n = strlen(kFrameSites[i].text);
    ok = !IsBadReadPtr(reinterpret_cast<const void*>(kFrameSites[i].path), n + 1) &&
         memcmp(reinterpret_cast<const void*>(kFrameSites[i].path), kFrameSites[i].text, n + 1) == 0;
  }
  const uint32_t slot = ok ? *reinterpret_cast<const uint32_t*>(kInvokeSlot) : 0;
  const uint32_t mine = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&InvokeHook));
  if (ok && slot != kInvokeOrig && slot != mine) ok = false;
  g_frames_checked = true;
  g_frames_ok = ok;
  const uint32_t* mvt = reinterpret_cast<const uint32_t*>(kMovieVt);
  g_replay_ok = ok && !IsBadReadPtr(reinterpret_cast<const void*>(kCameraIconShow), 15) &&
                memcmp(reinterpret_cast<const void*>(kCameraIconShow), "CameraIconShow", 15) == 0 &&
                mvt[0x2C / 4] == kSetVarOrig && mvt[0x30 / 4] == kGetVarOrig && kMovieVt + 0x48 == kInvokeSlot &&
                !IsBadReadPtr(reinterpret_cast<const void*>(kInputManager), 4) &&
                !memcmp(reinterpret_cast<const void*>(0x4D6124), "\x8B\x46\x64", 3) &&     // mov eax, [esi+0x64]
                !memcmp(reinterpret_cast<const void*>(0x4D6166), "\x8B\x0D\xC8\x46\xD6\x00", 6);  // mov ecx, [0xD646C8]
  if (ok && !g_replay_ok) ss_log("[glyphs] the Instant Replay banner's data is not the known build; its text is left as the game's");
  if (!ok) ss_log("[glyphs] the prompt bar's data in the image is not the known build; its icons are left as the game draws them");
  return ok;
}

// Points the movie's Invoke slot at the wrapper (once; with the keyboard set too, for the replay banner's key).
void InstallFrameHook() {
  if (g_frames_hooked || !CheckFrameSites()) return;
  uint32_t* slot = reinterpret_cast<uint32_t*>(kInvokeSlot);
  const uint32_t mine = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&InvokeHook));
  if (*slot != mine && !ss_write_word(slot, mine)) {
    ss_log("[glyphs] cannot write the movie vtable slot at %p (error %lu)", slot, GetLastError());
    g_frames_ok = false;
    return;
  }
  g_frames_hooked = true;
  ss_log("[glyphs] Invoke slot %p wrapped (the prompt bar's icons follow the set at its next change)", slot);
}

// ------------------------------------------------------------------------------ the found textures

struct Found {
  IDirect3DBaseTexture9* tex = nullptr;  // an IDirect3DTexture9
  const Site* site = nullptr;
  int variant = 0;
  bool staging = false;   // a SYSTEMMEM texture the game uploads from (not kept, no reference held)
  bool held = false;      // a reference of ours is held (MANAGED / DEFAULT textures)
  D3DPOOL pool = D3DPOOL_MANAGED;
  D3DFORMAT fmt = D3DFMT_A8R8G8B8;
  std::vector<std::vector<uint8_t>> original;  // per level, tight rows, the game's own bytes
};
std::vector<Found> g_found;
std::map<std::pair<const Site*, int>, std::vector<Level>> g_pictures;  // (site, set) -> picture

struct PendingLock {
  void* obj;  // the texture (LockRect on the texture) or the surface
  IDirect3DTexture9* tex;
  UINT level;
  uint8_t* bits;
  int pitch;
  UINT w, h;
  DWORD when;  // GetTickCount at the lock
};
std::vector<PendingLock> g_pending;

thread_local int t_inside = 0;  // our own locks, and the surface calls made inside a texture's LockRect
bool g_survey = false;
int g_survey_budget = 400;

Found* FindFound(void* tex) {
  for (Found& f : g_found)
    if (f.tex == tex) return &f;
  return nullptr;
}

const std::vector<Level>* Picture(const Found& f, Set set) {
  auto key = std::make_pair(f.site, static_cast<int>(set));
  auto it = g_pictures.find(key);
  if (it != g_pictures.end()) return &it->second;
  if (f.original.empty() || f.original[0].size() < size_t(f.site->width) * f.site->height * 4) return nullptr;
  auto& p = g_pictures[key];
  p = MakePicture(*f.site, set, f.original[0], f.variant);
  return &p;
}

// Writes level `level` of what `f` should show now into a locked rectangle.
void WriteLevel(const Found& f, UINT level, uint8_t* bits, int pitch, UINT w, UINT h, Set set) {
  if (Draws(*f.site, set)) {
    const std::vector<Level>* pic = Picture(f, set);
    if (!pic || level >= pic->size() || (*pic)[level].width != w || (*pic)[level].height != h) return;
    const Level& L = (*pic)[level];
    for (UINT y = 0; y < h; ++y)
      for (UINT x = 0; x < w; ++x) FromRgba(&L.rgba[(size_t(y) * w + x) * 4], bits + static_cast<ptrdiff_t>(y) * pitch + x * 4, f.variant);
  } else {
    if (level >= f.original.size() || f.original[level].size() != size_t(w) * h * 4) return;
    for (UINT y = 0; y < h; ++y) memcpy(bits + static_cast<ptrdiff_t>(y) * pitch, &f.original[level][size_t(y) * w * 4], size_t(w) * 4);
  }
}

// A level was just written by the game (before its UnlockRect): find the site, keep the original, draw ours.
void Unlocked(const PendingLock& p) {
  Found* f = FindFound(p.tex);
  if (p.level == 0) {
    int variant = -1;
    const Site* site = nullptr;
    if (SiteSize(p.w, p.h) || g_survey) {
      uint64_t hv = Fnv(p.bits, p.pitch, p.w, p.h);
      int n = 0;
      const Site* sites = Sites(&n);
      for (int i = 0; i < n; ++i)
        for (int v = 0; v < 3; ++v)
          if (sites[i].width == p.w && sites[i].height == p.h && sites[i].hash[v] == hv) site = &sites[i], variant = v;
      if (g_survey && g_survey_budget > 0) {
        --g_survey_budget;
        ss_log("[glyphs] survey: texture %p %ux%u level 0 hash %016llX%s%s", p.tex, p.w, p.h, static_cast<unsigned long long>(hv),
               site ? " = " : "", site ? site->name : "");
        // The pixels too (glyph-survey\<hash>_<w>x<h>.raw beside the game, BGRA rows), once a hash: to find new sites.
        char name[96];
        snprintf(name, sizeof name, "glyph-survey\\%016llX_%ux%u.raw", static_cast<unsigned long long>(hv), p.w, p.h);
        CreateDirectoryA("glyph-survey", nullptr);
        if (p.w <= 128 && p.h <= 128 && GetFileAttributesA(name) == INVALID_FILE_ATTRIBUTES) {
          if (FILE* f = fopen(name, "wb")) {
            for (UINT y = 0; y < p.h; ++y) fwrite(p.bits + static_cast<ptrdiff_t>(y) * p.pitch, 4, p.w, f);
            fclose(f);
          }
        }
      }
    }
    if (!site) {
      if (f) {  // the game wrote other pixels into a texture we knew: it is no longer a site
        if (f->held) f->tex->Release();
        g_found.erase(g_found.begin() + (f - g_found.data()));
      }
      return;
    }
    if (!f) {
      D3DSURFACE_DESC d{};
      p.tex->GetLevelDesc(0, &d);
      Found n;
      n.tex = p.tex;
      n.pool = d.Pool;
      n.fmt = d.Format;
      n.staging = d.Pool == D3DPOOL_SYSTEMMEM || d.Pool == D3DPOOL_SCRATCH;
      if (!n.staging) {
        p.tex->AddRef();
        n.held = true;
      }
      g_found.push_back(n);
      f = &g_found.back();
      ss_log("[glyphs] found %s: texture %p (%s pool, %s)", site->name, p.tex,
             d.Pool == D3DPOOL_MANAGED ? "managed" : d.Pool == D3DPOOL_DEFAULT ? "default" : "system-memory",
             variant == 0 ? "BGRA" : variant == 1 ? "RGBA" : "premultiplied");
    }
    f->site = site;
    f->variant = variant;
    f->original.assign(1, {});
  }
  if (!f) return;
  if (f->original.size() <= p.level) f->original.resize(p.level + 1);
  std::vector<uint8_t>& o = f->original[p.level];
  o.resize(size_t(p.w) * p.h * 4);
  for (UINT y = 0; y < p.h; ++y) memcpy(&o[size_t(y) * p.w * 4], p.bits + static_cast<ptrdiff_t>(y) * p.pitch, size_t(p.w) * 4);
  WriteLevel(*f, p.level, p.bits, p.pitch, p.w, p.h, static_cast<Set>(g_effective));
}

bool Interesting(D3DFORMAT fmt, UINT w, UINT h, UINT level, void* tex) {
  if (fmt != D3DFMT_A8R8G8B8 && fmt != D3DFMT_A8B8G8R8) return false;
  if (level == 0) return SiteSize(w, h) || (g_survey && w <= 256 && h <= 256);
  return FindFound(tex) != nullptr;
}

// -------------------------------------------------------------------------------- d3d9 hooks

// IID_IDirect3DTexture9 (d3d9.h declares it; no import library here defines it).
const GUID kIidTexture = {0x85c31227, 0x3de5, 0x4f00, {0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5}};
using TexLockFn = HRESULT(__stdcall*)(IDirect3DTexture9*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
using TexUnlockFn = HRESULT(__stdcall*)(IDirect3DTexture9*, UINT);
using SurfLockFn = HRESULT(__stdcall*)(IDirect3DSurface9*, D3DLOCKED_RECT*, const RECT*, DWORD);
using SurfUnlockFn = HRESULT(__stdcall*)(IDirect3DSurface9*);
using UpdateTextureFn = HRESULT(__stdcall*)(IDirect3DDevice9*, IDirect3DBaseTexture9*, IDirect3DBaseTexture9*);
TexLockFn o_tex_lock = nullptr;
TexUnlockFn o_tex_unlock = nullptr;
SurfLockFn o_surf_lock = nullptr;
SurfUnlockFn o_surf_unlock = nullptr;
UpdateTextureFn o_update = nullptr;
constexpr int kTexLockRect = 19, kTexUnlockRect = 20, kSurfLockRect = 13, kSurfUnlockRect = 14, kDevUpdateTexture = 31;
bool g_tex_hooked = false;

HRESULT __stdcall TexLockHook(IDirect3DTexture9* t, UINT level, D3DLOCKED_RECT* lr, const RECT* r, DWORD flags) {
  ++t_inside;
  HRESULT hr = o_tex_lock(t, level, lr, r, flags);
  --t_inside;
  if (FAILED(hr) || t_inside || r || !lr || (flags & D3DLOCK_READONLY)) return hr;
  D3DSURFACE_DESC d{};
  if (FAILED(t->GetLevelDesc(level, &d))) return hr;
  EnterCriticalSection(&g_cs);
  if (Interesting(d.Format, d.Width, d.Height, level, t))
    g_pending.push_back({t, t, level, static_cast<uint8_t*>(lr->pBits), lr->Pitch, d.Width, d.Height, GetTickCount()});
  LeaveCriticalSection(&g_cs);
  return hr;
}

HRESULT __stdcall TexUnlockHook(IDirect3DTexture9* t, UINT level) {
  if (!t_inside) {
    EnterCriticalSection(&g_cs);
    for (size_t i = 0; i < g_pending.size(); ++i) {
      if (g_pending[i].obj != t || g_pending[i].level != level) continue;
      const PendingLock p = g_pending[i];
      g_pending.erase(g_pending.begin() + i);
      Unlocked(p);
      break;
    }
    LeaveCriticalSection(&g_cs);
  }
  ++t_inside;
  HRESULT hr = o_tex_unlock(t, level);
  --t_inside;
  return hr;
}

// A level locked through its surface (GetSurfaceLevel + LockRect): the texture and level are looked up.
HRESULT __stdcall SurfLockHook(IDirect3DSurface9* s, D3DLOCKED_RECT* lr, const RECT* r, DWORD flags) {
  HRESULT hr = o_surf_lock(s, lr, r, flags);
  if (FAILED(hr) || t_inside || r || !lr || (flags & D3DLOCK_READONLY)) return hr;
  D3DSURFACE_DESC d{};
  if (FAILED(s->GetDesc(&d)) || (d.Format != D3DFMT_A8R8G8B8 && d.Format != D3DFMT_A8B8G8R8)) return hr;
  if (d.Width > 256 || d.Height > 256) return hr;
  ++t_inside;
  IDirect3DTexture9* tex = nullptr;
  if (SUCCEEDED(s->GetContainer(kIidTexture, reinterpret_cast<void**>(&tex))) && tex) {
    const UINT n = tex->GetLevelCount();
    for (UINT i = 0; i < n; ++i) {
      IDirect3DSurface9* li = nullptr;
      if (FAILED(tex->GetSurfaceLevel(i, &li)) || !li) continue;
      const bool same = li == s;
      li->Release();
      if (!same) continue;
      EnterCriticalSection(&g_cs);
      if (Interesting(d.Format, d.Width, d.Height, i, tex))
        g_pending.push_back({s, tex, i, static_cast<uint8_t*>(lr->pBits), lr->Pitch, d.Width, d.Height, GetTickCount()});
      LeaveCriticalSection(&g_cs);
      break;
    }
    tex->Release();
  }
  --t_inside;
  return hr;
}

HRESULT __stdcall SurfUnlockHook(IDirect3DSurface9* s) {
  if (!t_inside) {
    EnterCriticalSection(&g_cs);
    for (size_t i = 0; i < g_pending.size(); ++i) {
      if (g_pending[i].obj != s) continue;
      const PendingLock p = g_pending[i];
      g_pending.erase(g_pending.begin() + i);
      Unlocked(p);
      break;
    }
    LeaveCriticalSection(&g_cs);
  }
  return o_surf_unlock(s);
}

// A staging texture of a site is uploaded: the destination is the texture the game draws with.
HRESULT __stdcall UpdateTextureHook(IDirect3DDevice9* d, IDirect3DBaseTexture9* src, IDirect3DBaseTexture9* dst) {
  if (src && dst && !t_inside) {
    EnterCriticalSection(&g_cs);
    Found* f = FindFound(src);
    if (f && f->staging && !FindFound(dst)) {
      Found n = *f;
      n.tex = dst;
      n.staging = false;
      n.pool = D3DPOOL_DEFAULT;
      dst->AddRef();
      n.held = true;
      g_found.push_back(n);
      ss_log("[glyphs] %s uploaded from %p into %p (default pool)", n.site->name, src, dst);
    }
    LeaveCriticalSection(&g_cs);
  }
  return o_update(d, src, dst);
}

bool Hook(void** vt, int slot, void* fn, void** orig) {
  if (vt[slot] == fn) return true;
  void* prev = vt[slot];
  *orig = prev;
  if (!ss_write_word(&vt[slot], reinterpret_cast<uintptr_t>(fn))) {
    ss_log("[glyphs] cannot write vtable slot %d at %p (error %lu)", slot, &vt[slot], GetLastError());
    return false;
  }
  return true;
}

void HookTextures(IDirect3DDevice9* dev, const char* when) {
  if (g_tex_hooked || !dev) return;
  g_tex_hooked = true;
  IDirect3DTexture9* probe = nullptr;
  HRESULT hr = dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &probe, nullptr);
  if (FAILED(hr) || !probe) hr = dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &probe, nullptr);
  if (FAILED(hr) || !probe) {
    ss_log("[glyphs] cannot make a probe texture (0x%08lx): the prompt textures are left as the game draws them", static_cast<unsigned long>(hr));
    return;
  }
  IDirect3DSurface9* surf = nullptr;
  probe->GetSurfaceLevel(0, &surf);
  void** tvt = *reinterpret_cast<void***>(probe);
  void** svt = surf ? *reinterpret_cast<void***>(surf) : nullptr;
  void** dvt = *reinterpret_cast<void***>(dev);
  bool ok = Hook(tvt, kTexLockRect, reinterpret_cast<void*>(&TexLockHook), reinterpret_cast<void**>(&o_tex_lock)) &&
            Hook(tvt, kTexUnlockRect, reinterpret_cast<void*>(&TexUnlockHook), reinterpret_cast<void**>(&o_tex_unlock));
  if (ok && svt)
    ok = Hook(svt, kSurfLockRect, reinterpret_cast<void*>(&SurfLockHook), reinterpret_cast<void**>(&o_surf_lock)) &&
         Hook(svt, kSurfUnlockRect, reinterpret_cast<void*>(&SurfUnlockHook), reinterpret_cast<void**>(&o_surf_unlock));
  if (ok) ok = Hook(dvt, kDevUpdateTexture, reinterpret_cast<void*>(&UpdateTextureHook), reinterpret_cast<void**>(&o_update));
  if (surf) surf->Release();
  probe->Release();
  ss_log("[glyphs] texture hooks %s %s (texture vtable %p, surface vtable %p)", ok ? "in" : "FAILED", when, tvt, svt);
}

// Re-writes every found texture for the set in force (render thread, from the Present hook).
void Refresh(IDirect3DDevice9* dev) {
  const Set set = static_cast<Set>(g_effective);
  EnterCriticalSection(&g_cs);
  ++t_inside;
  int done = 0;
  for (Found& f : g_found) {
    if (f.staging) continue;
    IDirect3DTexture9* t = static_cast<IDirect3DTexture9*>(f.tex);
    const UINT levels = std::min<UINT>(t->GetLevelCount(), static_cast<UINT>(f.original.size()));
    IDirect3DTexture9* up = t;
    if (f.pool == D3DPOOL_DEFAULT) {  // through a staging texture of ours
      up = nullptr;
      if (FAILED(dev->CreateTexture(f.site->width, f.site->height, levels, 0, f.fmt, D3DPOOL_SYSTEMMEM, &up, nullptr)) || !up) continue;
    }
    for (UINT l = 0; l < levels; ++l) {
      D3DSURFACE_DESC d{};
      D3DLOCKED_RECT lr{};
      if (FAILED(up->GetLevelDesc(l, &d)) || FAILED(up->LockRect(l, &lr, nullptr, 0))) continue;
      WriteLevel(f, l, static_cast<uint8_t*>(lr.pBits), lr.Pitch, d.Width, d.Height, set);
      up->UnlockRect(l);
    }
    if (up != t) {
      (o_update ? o_update(dev, up, t) : dev->UpdateTexture(up, t));
      up->Release();
    }
    ++done;
  }
  --t_inside;
  LeaveCriticalSection(&g_cs);
  if (done) ss_log("[glyphs] %d prompt texture(s) re-written for %s", done, kSetNames[set]);
}

// Drops the textures the game has let go of (only our reference is left) and the staging ones.
void Sweep() {
  EnterCriticalSection(&g_cs);
  for (size_t i = 0; i < g_found.size();) {
    Found& f = g_found[i];
    bool drop = f.staging;
    if (f.held) {
      f.tex->AddRef();
      const ULONG n = f.tex->Release();
      if (n <= 1) {
        f.tex->Release();
        drop = true;
      }
    }
    if (drop) g_found.erase(g_found.begin() + i);
    else ++i;
  }
  const DWORD now = GetTickCount();  // a lock not unlocked within 10 s is not one the game finishes
  g_pending.erase(std::remove_if(g_pending.begin(), g_pending.end(), [now](const PendingLock& p) { return now - p.when > 10000; }),
                  g_pending.end());
  LeaveCriticalSection(&g_cs);
}

// ------------------------------------------------------------------------------------ pad kinds

struct PadId {
  uint16_t vid = 0, pid = 0;
  std::string type;  // Steam's name of the pad type, when it gave one
};

bool Handheld() {
  if (const char* e = getenv("SS_HANDHELD"); e && *e) return *e != '0';
  if (const char* e = getenv("SteamDeck"); e && *e && *e != '0') return true;
  return false;
}

bool Xbox360Pid(uint16_t pid) {
  return pid == 0x028E || pid == 0x028F || pid == 0x0291 || pid == 0x02A0 || pid == 0x02A1 || pid == 0x0719;
}

Set SetFor(const PadId& p, bool handheld) {
  std::string t = p.type;
  for (char& c : t) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  if (!t.empty()) {
    if (t.find("ps3") != std::string::npos || t.find("ps4") != std::string::npos || t.find("ps5") != std::string::npos) return kPlayStation;
    if (t.find("switch") != std::string::npos || t.find("joycon") != std::string::npos) return kNintendo;
    if (t.find("steam") != std::string::npos) return kSteam;
  }
  switch (p.vid) {
    case 0x28DE: return kSteam;
    case 0x054C: return kPlayStation;
    case 0x057E: return kNintendo;
    case 0x045E: return Xbox360Pid(p.pid) ? (handheld ? kSteam : kXbox360) : (handheld ? kSteam : kXbox);
    default: return handheld ? kSteam : kXbox;
  }
}

// Steam's description of the pads behind its virtual ones (SteamVirtualGamepadInfo: a file of "[slot N]"
// sections with VID, PID and type). The lowest slot.
bool SteamInfo(PadId* out) {
  const char* path = getenv("SteamVirtualGamepadInfo");
  if (!path || !*path) return false;
  std::string p = path;
  if (p[0] == '/') {  // a Unix path under Proton: Wine's Z: drive is the root
    for (char& c : p)
      if (c == '/') c = '\\';
    p = "Z:" + p;
  }
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return false;
  static bool logged = false;
  char line[256];
  int slot = -1, best = 1 << 30;
  PadId cur, found;
  bool have = false;
  auto take = [&] {
    if (slot >= 0 && slot < best && (cur.vid || !cur.type.empty())) best = slot, found = cur, have = true;
  };
  std::string all;  // the file in one log line (no handles), at most ~400 characters
  while (fgets(line, sizeof line, f)) {
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
    if (!logged && n && _strnicmp(line, "handle=", 7) != 0 && all.size() < 400) all += (all.empty() ? "" : " ") + std::string(line);
    if (line[0] == '[') {
      take();
      cur = PadId();
      slot = -1;
      const char* d = line;
      while (*d && !isdigit(static_cast<unsigned char>(*d))) ++d;
      if (*d) slot = atoi(d);
      continue;
    }
    const char* eq = strchr(line, '=');
    if (!eq) continue;
    std::string k(line, eq - line), v = eq + 1;
    for (char& c : k) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (k == "vid") cur.vid = static_cast<uint16_t>(strtoul(v.c_str(), nullptr, 0));
    else if (k == "pid") cur.pid = static_cast<uint16_t>(strtoul(v.c_str(), nullptr, 0));
    else if (k == "type") cur.type = v;
  }
  take();
  fclose(f);
  if (!logged) ss_log("[glyphs] SteamVirtualGamepadInfo: %s", all.empty() ? "(empty)" : all.c_str());
  logged = true;
  if (have) *out = found;
  return have;
}

// The game pads of the Windows device list (raw input: HID, generic desktop page, joystick or game pad).
std::vector<PadId> RawPads() {
  std::vector<PadId> out;
  UINT n = 0;
  if (GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST)) != 0 || n == 0) return out;
  std::vector<RAWINPUTDEVICELIST> list(n);
  UINT got = GetRawInputDeviceList(list.data(), &n, sizeof(RAWINPUTDEVICELIST));
  if (got == static_cast<UINT>(-1)) return out;
  for (UINT i = 0; i < got; ++i) {
    if (list[i].dwType != RIM_TYPEHID) continue;
    RID_DEVICE_INFO info{};
    info.cbSize = sizeof info;
    UINT sz = sizeof info;
    if (GetRawInputDeviceInfoA(list[i].hDevice, RIDI_DEVICEINFO, &info, &sz) == static_cast<UINT>(-1)) continue;
    if (info.hid.usUsagePage != 1 || (info.hid.usUsage != 4 && info.hid.usUsage != 5)) continue;
    PadId p;
    p.vid = static_cast<uint16_t>(info.hid.dwVendorId);
    p.pid = static_cast<uint16_t>(info.hid.dwProductId);
    out.push_back(p);
  }
  return out;
}

std::string g_last_detect;

void Detect() {
  const bool handheld = Handheld();
  std::vector<PadId> pads = RawPads();
  PadId use;
  const char* from = "none";
  bool have = false;
  for (const PadId& p : pads) {
    if (p.vid == 0x28DE && p.pid == 0x11FF) {  // Steam Input's virtual pad: the real one behind it, when Steam names it
      PadId s;
      if (SteamInfo(&s)) {
        use = s, have = true, from = "Steam's virtual pad info";
        break;
      }
    }
    if (!have) use = p, have = true, from = "device list";
    break;
  }
  if (!have && g_di_product) {
    use.vid = static_cast<uint16_t>(g_di_product & 0xFFFF);
    use.pid = static_cast<uint16_t>(g_di_product >> 16);
    have = true;
    from = "DirectInput list";
  }
  const Set set = have ? SetFor(use, handheld) : (handheld ? kSteam : kXbox360);
  char desc[160];
  snprintf(desc, sizeof desc, "%04X:%04X%s%s from the %s, %u pad(s) listed%s -> %s", use.vid, use.pid, use.type.empty() ? "" : " type ",
           use.type.c_str(), from, static_cast<unsigned>(pads.size()), handheld ? ", handheld" : "", kSetNames[set]);
  if (desc != g_last_detect) {
    g_last_detect = desc;
    ss_log("[glyphs] pad: %s", desc);
  }
  if (InterlockedExchange(&g_detected, set) != set) Update("pad kind");
}

DWORD WINAPI DetectThread(LPVOID) {
  for (;;) {
    if (static_cast<Set>(g_chosen) == kAuto) Detect();
    Sleep(2000);
  }
}

LONG g_tex_version = 1;
bool g_late_checked = false;

}  // namespace

// ------------------------------------------------------------------------------------------- the entries

void ss_glyphs_start() {
  g_on = ss_patch_active();
  if (!g_on) return;
  InitializeCriticalSection(&g_cs);
  std::string v = ss_conf("button_prompts");
  const char* why = "button_prompts";
  if (v.empty()) v = ss_conf("pad_glyphs"), why = "pad_glyphs (the older key)";
  if (const char* e = getenv("SS_PAD_GLYPHS")) v = e, why = "SS_PAD_GLYPHS";
  if (const char* e = getenv("SS_BUTTON_PROMPTS")) v = e, why = "SS_BUTTON_PROMPTS";
  Set s = kAuto;
  if (!v.empty() && !ParseSet(v, &s)) ss_log("[glyphs] %s = %s is not known; auto", why, v.c_str());
  g_chosen = s;
  if (const char* e = getenv("SS_GLYPH_SURVEY")) g_survey = *e == '1';
  ss_log("[glyphs] button prompts: setting %s%s", kSetNames[s], g_survey ? " (survey: small textures logged)" : "");
  Update("start");
  CreateThread(nullptr, 0, DetectThread, nullptr, 0, nullptr);
}

bool ss_glyphs_wanted() { return g_on; }

// display.cpp, inside the game's CreateDevice: before any texture of the game exists.
void ss_glyphs_device(void* device) {
  if (!g_on) return;
  HookTextures(static_cast<IDirect3DDevice9*>(device), "at device creation");
}

// patch.cpp's Present hook: a set change re-writes the found textures.
void ss_glyphs_present(void* device) {
  if (!g_on) return;
  IDirect3DDevice9* dev = static_cast<IDirect3DDevice9*>(device);
  if (!g_tex_hooked && !g_late_checked) {
    g_late_checked = true;
    HookTextures(dev, "late (the prompt textures loaded before keep the game's pictures until they are loaded again)");
  }
  InstallFrameHook();
  static DWORD last_sweep = 0;
  const DWORD now = GetTickCount();
  if (now - last_sweep > 5000) {
    last_sweep = now;
    Sweep();
  }
  const LONG v = g_version;
  if (v != g_tex_version && g_tex_hooked) {
    g_tex_version = v;
    Refresh(dev);
  }
}

// display.cpp, before the device's Reset: a default-pool texture must have no reference of ours left.
void ss_glyphs_before_reset() {
  if (!g_on) return;
  EnterCriticalSection(&g_cs);
  for (size_t i = 0; i < g_found.size();) {
    if (g_found[i].pool != D3DPOOL_DEFAULT) {
      ++i;
      continue;
    }
    if (g_found[i].held) g_found[i].tex->Release();
    g_found.erase(g_found.begin() + i);
  }
  LeaveCriticalSection(&g_cs);
}

// pad.cpp: the last input came from a pad (true) or the keyboard (false); auto follows it.
void ss_glyphs_input(bool pad, const char* why) {
  if (!g_on) return;
  if (InterlockedExchange(&g_pad_in_use, pad ? 1 : 0) != (pad ? 1 : 0)) Update(why);
}

// pad.cpp: the first real pad of the game's DirectInput list (PID<<16|VID), the fallback for the device list.
void ss_glyphs_directinput_pad(unsigned long product) { g_di_product = product; }

// The settings menu's BUTTON PROMPTS row.
bool ss_glyphs_apply(const char* key, const char* value) {
  if (!g_on || strcmp(key, "button_prompts")) return false;
  Set s = kAuto;
  if (!ParseSet(value ? value : "", &s)) return false;
  g_chosen = s;  // auto: the pad kind is looked at again within two seconds (the detection thread)
  Update("settings menu");
  return true;
}
