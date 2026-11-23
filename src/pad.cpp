// Controller layer of the dinput8 proxy.
//
// What the game does (reverse engineered from the PC build, see README):
//  * it reads every pad through DirectInput 8 (ANSI): DirectInput8Create -> EnumDevices(GAMECTRL, attached only)
//    once at start (never again: no hot-plug) -> CreateDevice -> SetDataFormat(c_dfDIJoystick2) -> SetProperty
//    (range +-32767 on the six axes) -> SetCooperativeLevel -> Acquire; every frame Poll + GetDeviceState(0x110).
//  * the bindings (action -> button / axis half / hat angle) come from one built-in table per *product id*
//    (the first dword of the product GUID, i.e. PID<<16|VID). Only 15 products are known (Xbox 360 pad,
//    a few Logitech / Saitek / Thrustmaster pads and wheels). Any other product gets a table whose every
//    entry is "unbound": the pad does nothing, in the menus as in the race.
//  * rumble goes through XInputSetState(i, ...) with i = the pad's position in the game's own device list,
//    which is the DirectInput enumeration order and not the XInput slot.
//
// What this layer does: it wraps IDirectInput8A. The game's pad list is rebuilt: every connected XInput pad
// becomes a virtual DirectInput device that looks exactly like a wired Xbox 360 pad (product 045E:028E,
// DIJOYSTATE2 in the layout the game's table expects), in XInput slot order. Real DirectInput game
// controllers are dropped when XInput already covers them, translated to the same Xbox layout when nothing
// else does (Sony / Nintendo / generic pads), or passed through untouched (wheels, the pads the game knows).
// The game's XInputSetState is routed from the device-list index to the right XInput slot.
//
// Nothing of the game's code is written: only this DLL's own objects, the entry code of xinput1_3.dll in memory, and
// (split triggers, pad_split_triggers) three slots of the input manager's .rdata vtable 0xBA2904.

#define DIRECTINPUT_VERSION 0x0800
#include <initguid.h>  // first: defines the GUIDs of this translation unit
#include <windows.h>
#include <dinput.h>
#include <xinput.h>

#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "menu.h"
#include "padsplit_core.h"
#include "ss_common.h"

bool ss_write_word(void* where, uintptr_t value, uintptr_t* previous = nullptr);  // testshot.cpp: one lock for all slot writes
bool ss_intro_tap(int pad_index);  // intro.cpp: skip_intro taps A on pad #0 while the boot movies run
void ss_intro_user_input();        // intro.cpp: a real button press was seen (the taps stop)
void ss_boot_user_input();         // boot.cpp: the same for boot_to_menu (it hands off)
bool ss_conf_present();            // patch.cpp: splitsecond.conf exists next to the DLL
void ss_glyphs_input(bool pad, const char* why);         // glyphs.cpp: the last input came from a pad / the keyboard
void ss_glyphs_directinput_pad(unsigned long product);  // glyphs.cpp: the first real pad's product (PID<<16|VID)
void ss_glyphs_pad_layout(bool xbox_layout);           // glyphs.cpp: every pad in the game's list is in the Xbox 360 layout

namespace {

// ------------------------------------------------------------------------------------- configuration

// splitsecond.conf keys of this layer. The port (recomp) has no controller rows in its settings catalog, so
// these are new; names and values follow its style (snake_case, true/false, percentages as plain numbers).
struct Config {
  bool on = true;             // pad_support: the whole layer
  bool rumble = true;         // pad_rumble: vibration through the routing hook
  int rumble_gain = 100;      // pad_rumble_gain: percent of what the game asks (100 = as asked)
  int deadzone_left = 6;      // pad_deadzone_left: percent of the stick range, radial (the game adds its own)
  int deadzone_right = 6;     // pad_deadzone_right
  int trigger_threshold = 5;  // pad_trigger_threshold: percent of the trigger travel
  bool hotplug = true;        // pad_hotplug: offer all four XInput slots from the start so pads can be
                              //   connected after the game has started (the game enumerates only once)
  bool translate = true;      // pad_translate_dinput: DirectInput-only pads (Sony, Nintendo, generic) -> Xbox layout
  bool log_input = false;     // pad_log: log button changes (the owner's evidence), capped
  bool foreground_only = false;  // pad_foreground_only: report nothing while another application's window is in front
  bool split_triggers = true;    // pad_split_triggers: throttle and brake read RT and LT apart (read once at start)
} g_cfg;

bool ParseBool(const std::string& v, bool def) {
  if (v.empty()) return def;
  if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
  if (v == "0" || v == "false" || v == "no" || v == "off") return false;
  return def;
}

int ParseInt(const std::string& v, int def, int lo, int hi) {
  if (v.empty()) return def;
  char* end = nullptr;
  long n = strtol(v.c_str(), &end, 10);
  if (end == v.c_str()) return def;
  if (n < lo) n = lo;
  if (n > hi) n = hi;
  return static_cast<int>(n);
}

void LoadConfig() {
  Config c;
  const char* env = getenv("SS_PAD");  // environment switch beats the file (test aid)
  const char* fake = getenv("SS_PAD_FAKE");
  const bool env_on = env || (fake && *fake == '1');  // an environment switch counts as asking for the layer
  c.on = env ? ParseBool(env, true) : ParseBool(ss_conf("pad_support"), true);
  c.rumble = ParseBool(ss_conf("pad_rumble"), true);
  c.rumble_gain = ParseInt(ss_conf("pad_rumble_gain"), 100, 0, 200);
  c.deadzone_left = ParseInt(ss_conf("pad_deadzone_left"), 6, 0, 50);
  c.deadzone_right = ParseInt(ss_conf("pad_deadzone_right"), 6, 0, 50);
  c.trigger_threshold = ParseInt(ss_conf("pad_trigger_threshold"), 5, 0, 50);
  c.hotplug = ParseBool(ss_conf("pad_hotplug"), true);
  c.translate = ParseBool(ss_conf("pad_translate_dinput"), true);
  c.log_input = ParseBool(ss_conf("pad_log"), false);
  c.foreground_only = ParseBool(ss_conf("pad_foreground_only"), false);
  c.split_triggers = ParseBool(ss_conf("pad_split_triggers"), true);
  if (!env_on && !ss_conf_present()) c.on = false;  // no conf file and no switch: the DLL only forwards DirectInput
  g_cfg = c;
}

// ------------------------------------------------------------------------------------------- state

CRITICAL_SECTION g_cs;

// The Xbox 360 pad as the game's table knows it: product GUID {028E045E-0000-0000-0000-504944564944}.
const GUID kProductX360 = {0x028E045E, 0x0000, 0x0000, {0x00, 0x00, 0x50, 0x49, 0x44, 0x56, 0x49, 0x44}};
const char kNameX360[] = "Controller (XBOX 360 For Windows)";
constexpr DWORD kDevTypeGamepad = 0x00010215;  // HID | standard gamepad subtype | DI8DEVTYPE_GAMEPAD

// The 15 product ids the game has a table for (first dword of the product GUID, PID<<16|VID).
// Index 0 and 1 are the Xbox 360 pad (wired, wireless); the rest are older pads and wheels.
const DWORD kGameKnownProducts[15] = {
    0x028E045E, 0x02A1045E, 0xCA04046D, 0xC218046D, 0xC299046D, 0xC29B046D, 0xC294046D, 0xC216046D,
    0x40010428, 0xFF0D06A3, 0x040C06A3, 0xB321044F, 0x990307B5, 0x00050C12, 0xC293046D};

bool GameKnows(DWORD p) {
  for (DWORD k : kGameKnownProducts)
    if (k == p) return true;
  return false;
}

DWORD Vid(DWORD p) { return p & 0xFFFF; }

// -------------------------------------------------------------------------------------- pad state

// One pad in XInput terms (all sticks up = +).
struct Pad {
  float lx = 0, ly = 0, rx = 0, ry = 0;  // -1..1
  float lt = 0, rt = 0;                  // 0..1
  WORD buttons = 0;                      // XINPUT_GAMEPAD_* bits
};

bool g_fake = false;

struct XInputApi {
  HMODULE mod = nullptr;
  DWORD(WINAPI* get)(DWORD, XINPUT_STATE*) = nullptr;
  DWORD(WINAPI* set)(DWORD, XINPUT_VIBRATION*) = nullptr;
  bool set_is_separate = false;  // set belongs to a module other than xinput1_3 (so the 1_3 hook can call it)
} g_xi;

bool LoadXInput() {
  const char* names[] = {"xinput1_4.dll", "xinput9_1_0.dll", "xinput1_3.dll"};
  for (const char* n : names) {
    HMODULE m = LoadLibraryA(n);
    if (!m) continue;
    auto get = reinterpret_cast<DWORD(WINAPI*)(DWORD, XINPUT_STATE*)>(GetProcAddress(m, "XInputGetState"));
    auto set = reinterpret_cast<DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*)>(GetProcAddress(m, "XInputSetState"));
    if (!get || !set) continue;
    g_xi.mod = m;
    g_xi.get = get;
    g_xi.set = set;
    g_xi.set_is_separate = strcmp(n, "xinput1_3.dll") != 0;
    ss_log("[pad] XInput: %s", n);
    return true;
  }
  ss_log("[pad] XInput: no module found");
  return false;
}

float Norm16(int v) {
  float f = static_cast<float>(v) / 32767.0f;
  return f < -1.0f ? -1.0f : (f > 1.0f ? 1.0f : f);
}

bool ReadXInput(int slot, Pad& out) {
  out = Pad();
  if (g_fake) {  // SS_PAD_FAKE=1 (test aid): XInput slot 0 holds A and D-pad up, left stick half right / quarter up, left trigger full
    if (slot != 0) return false;
    out.lx = 0.5f;
    out.ly = 0.25f;
    out.lt = 1.0f;
    out.buttons = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_DPAD_UP;
    return true;
  }
  if (!g_xi.get) return false;
  // An empty slot is asked again only every 100 ms. XInputGetState on a slot with no pad costs about 30 us on Windows
  // (measured, xinput1_3 and 1_4), and with pad_hotplug the game polls all four slots on every update and the settings
  // menu's chord check reads all four on every present: with no pad that was about 0.25 ms a frame at 120 (SSPC-17
  // round 2). A pad plugged in is seen up to 0.1 s later; a connected slot is read live every time, as before.
  static volatile DWORD empty_since[4];
  const DWORD now = GetTickCount();
  if (slot >= 0 && slot < 4 && empty_since[slot] && now - empty_since[slot] < 100) return false;
  XINPUT_STATE st;
  memset(&st, 0, sizeof st);
  const DWORD r = g_xi.get(static_cast<DWORD>(slot), &st);
  if (slot >= 0 && slot < 4) empty_since[slot] = r == ERROR_DEVICE_NOT_CONNECTED ? (now ? now : 1) : 0;
  if (r != ERROR_SUCCESS) return false;
  out.lx = Norm16(st.Gamepad.sThumbLX);
  out.ly = Norm16(st.Gamepad.sThumbLY);
  out.rx = Norm16(st.Gamepad.sThumbRX);
  out.ry = Norm16(st.Gamepad.sThumbRY);
  out.lt = st.Gamepad.bLeftTrigger / 255.0f;
  out.rt = st.Gamepad.bRightTrigger / 255.0f;
  out.buttons = st.Gamepad.wButtons;
  return true;
}


// ------------------------------------------------------------------------------------ prompt glyphs
//
// The button prompts themselves are glyphs.cpp's (button_prompts). This layer tells it which device was used last:
// a pad press or a stick pushed past half -> pad; a key while the pad has rested for 300 ms -> keyboard.
DWORD g_glyph_last_kb = 0;
DWORD g_glyph_last_pad_activity = 0;
bool g_glyph_pad = false;

bool AnyKeyboardKey() {
  for (int vk = 8; vk < 0xFF; ++vk)
    if (vk != VK_LBUTTON && (GetAsyncKeyState(vk) & 0x8000) && vk >= 8) return true;
  return false;
}

// Called for every state read of a presented pad.
void GlyphWatch(const Pad& p) {
  DWORD now = GetTickCount();
  bool active = p.buttons != 0 || fabsf(p.lx) > 0.5f || fabsf(p.ly) > 0.5f || fabsf(p.rx) > 0.5f || fabsf(p.ry) > 0.5f ||
                p.lt > 0.5f || p.rt > 0.5f;
  if (active) {
    g_glyph_last_pad_activity = now;
    if (!g_glyph_pad) {
      g_glyph_pad = true;
      ss_glyphs_input(true, "pad used");
    }
  } else if (g_glyph_pad && now - g_glyph_last_kb > 150) {
    g_glyph_last_kb = now;
    if (now - g_glyph_last_pad_activity > 300 && AnyKeyboardKey()) {
      g_glyph_pad = false;
      ss_glyphs_input(false, "keyboard used");
    }
  }
}

// ---------------------------------------------------------------------------- the virtual DirectInput pad

// DIJOYSTATE2 as the game's data format describes it (its size is 0x110; the first 0x50 bytes are DIJOYSTATE).
constexpr DWORD kStateSize2 = 0x110;
constexpr DWORD kStateSize1 = 0x50;

struct PadObject {
  const GUID* type;
  DWORD ofs;
  DWORD dwtype;
  DWORD flags;
  const char* name;
};

constexpr int kButtons = 10;
std::vector<PadObject> g_objects;  // X Y Z Rx Ry, POV 0, buttons 0..9, at their DIJOYSTATE2 offsets

void BuildObjects() {
  static const struct {
    const GUID* g;
    const char* n;
  } axes[5] = {{&GUID_XAxis, "X Axis"}, {&GUID_YAxis, "Y Axis"}, {&GUID_ZAxis, "Z Axis"},
               {&GUID_RxAxis, "X Rotation"}, {&GUID_RyAxis, "Y Rotation"}};
  for (int i = 0; i < 5; ++i)
    g_objects.push_back({axes[i].g, static_cast<DWORD>(i * 4), static_cast<DWORD>(DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(i)), DIDOI_ASPECTPOSITION,
                         axes[i].n});
  g_objects.push_back({&GUID_POV, 0x20, DIDFT_POV | DIDFT_MAKEINSTANCE(0), 0, "Hat Switch"});
  static char names[kButtons][12];
  for (int i = 0; i < kButtons; ++i) {
    snprintf(names[i], sizeof names[i], "Button %d", i);
    g_objects.push_back({&GUID_Button, static_cast<DWORD>(0x30 + i), static_cast<DWORD>(DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(i)), 0, names[i]});
  }
}

bool ObjectMatches(const PadObject& o, DWORD flags) {
  if (flags & DIDFT_FFACTUATOR) return false;  // no force-feedback objects here
  if (flags & DIDFT_FFEFFECTTRIGGER) return false;
  DWORD kinds = flags & 0xFF;
  if (kinds == DIDFT_ALL) return true;
  return (o.dwtype & kinds & 0x1F) != 0;
}

enum class Kind { XInputSlot, DirectInputPad };

// How a real DirectInput pad's layout is read as an Xbox pad.
enum class Profile { Identity, Sony, Nintendo };

struct Entry {  // one device the game was offered in its last EnumDevices
  Kind kind = Kind::XInputSlot;
  int slot = -1;  // XInput slot
  GUID instance{};
  GUID real_instance{};  // the real device's instance (translated or passed through)
  DWORD product = 0;
  std::string name;
  Profile profile = Profile::Identity;
  bool virt = true;     // false: a real device passed through untouched
  int list_index = -1;  // position in the game's own list (order of CreateDevice), for the rumble routing
};

std::vector<Entry> g_entries;
int g_created = 0;

int g_log_budget_input = 400;  // button-change lines per run

void DeadzoneStick(float x, float y, float dz, float& ox, float& oy) {
  float m = sqrtf(x * x + y * y);
  if (m <= dz || m <= 0.0f) {
    ox = oy = 0.0f;
    return;
  }
  float s = m > 1.0f ? 1.0f : m;
  float k = (s - dz) / (1.0f - dz) / m;
  ox = x * k;
  oy = y * k;
  if (ox > 1) ox = 1;
  if (ox < -1) ox = -1;
  if (oy > 1) oy = 1;
  if (oy < -1) oy = -1;
}

float DeadzoneTrigger(float t, float th) {
  if (t <= th) return 0.0f;
  float v = (t - th) / (1.0f - th);
  return v > 1.0f ? 1.0f : v;
}

LONG ScaleAxis(float n, LONG lo, LONG hi) {
  float c = (static_cast<float>(lo) + static_cast<float>(hi)) * 0.5f;
  float h = (static_cast<float>(hi) - static_cast<float>(lo)) * 0.5f;
  return static_cast<LONG>(lroundf(c + n * h));
}

DWORD PovFromDpad(WORD b) {
  bool u = b & XINPUT_GAMEPAD_DPAD_UP, d = b & XINPUT_GAMEPAD_DPAD_DOWN, l = b & XINPUT_GAMEPAD_DPAD_LEFT,
       r = b & XINPUT_GAMEPAD_DPAD_RIGHT;
  if (u && r) return 4500;
  if (r && d) return 13500;
  if (d && l) return 22500;
  if (l && u) return 31500;
  if (u) return 0;
  if (r) return 9000;
  if (d) return 18000;
  if (l) return 27000;
  return 0xFFFFFFFF;
}

// XInput button bit of each DirectInput button index, in the order a wired Xbox 360 pad reports them.
const WORD kButtonBits[kButtons] = {XINPUT_GAMEPAD_A,           XINPUT_GAMEPAD_B,            XINPUT_GAMEPAD_X,
                                    XINPUT_GAMEPAD_Y,           XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER,
                                    XINPUT_GAMEPAD_BACK,        XINPUT_GAMEPAD_START,         XINPUT_GAMEPAD_LEFT_THUMB,
                                    XINPUT_GAMEPAD_RIGHT_THUMB};
const char* const kButtonNames[kButtons] = {"A", "B", "X", "Y", "LB", "RB", "Back", "Start", "LS", "RS"};

// Real DirectInput device behind a translated virtual pad: a fixed private data format.
struct RawJoy {
  LONG a[8];  // X Y Z Rx Ry Rz slider0 slider1
  DWORD pov[4];
  BYTE b[32];
};

DIOBJECTDATAFORMAT g_rawfmt_objs[8 + 4 + 32];
DIDATAFORMAT g_rawfmt;

void BuildRawFormat() {
  const GUID* ax[8] = {&GUID_XAxis, &GUID_YAxis, &GUID_ZAxis, &GUID_RxAxis, &GUID_RyAxis, &GUID_RzAxis, &GUID_Slider, &GUID_Slider};
  int n = 0;
  for (int i = 0; i < 8; ++i)
    g_rawfmt_objs[n++] = {ax[i], static_cast<DWORD>(offsetof(RawJoy, a) + i * 4), DIDFT_OPTIONAL | DIDFT_AXIS | DIDFT_ANYINSTANCE, 0};
  for (int i = 0; i < 4; ++i)
    g_rawfmt_objs[n++] = {&GUID_POV, static_cast<DWORD>(offsetof(RawJoy, pov) + i * 4), DIDFT_OPTIONAL | DIDFT_POV | DIDFT_ANYINSTANCE, 0};
  for (int i = 0; i < 32; ++i)
    g_rawfmt_objs[n++] = {nullptr, static_cast<DWORD>(offsetof(RawJoy, b) + i), DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0};
  g_rawfmt.dwSize = sizeof(DIDATAFORMAT);
  g_rawfmt.dwObjSize = sizeof(DIOBJECTDATAFORMAT);
  g_rawfmt.dwFlags = DIDF_ABSAXIS;
  g_rawfmt.dwDataSize = sizeof(RawJoy);
  g_rawfmt.dwNumObjs = n;
  g_rawfmt.rgodf = g_rawfmt_objs;
}

float Axis01(LONG v) { return static_cast<float>(v) / 65535.0f; }
float AxisSigned(LONG v) {
  float f = (static_cast<float>(v) - 32767.5f) / 32767.5f;
  return f < -1.0f ? -1.0f : (f > 1.0f ? 1.0f : f);
}

WORD DpadFromPov(DWORD pov) {
  if (LOWORD(pov) == 0xFFFF) return 0;
  DWORD a = pov % 36000;
  WORD d = 0;
  if (a >= 31500 || a <= 4500) d |= XINPUT_GAMEPAD_DPAD_UP;
  if (a >= 4500 && a <= 13500) d |= XINPUT_GAMEPAD_DPAD_RIGHT;
  if (a >= 13500 && a <= 22500) d |= XINPUT_GAMEPAD_DPAD_DOWN;
  if (a >= 22500 && a <= 31500) d |= XINPUT_GAMEPAD_DPAD_LEFT;
  return d;
}

void RawToPad(const RawJoy& r, Profile prof, Pad& p) {
  p = Pad();
  p.buttons = DpadFromPov(r.pov[0]);
  auto btn = [&](int i) { return (r.b[i] & 0x80) != 0; };
  auto set = [&](bool on, WORD bit) {
    if (on) p.buttons |= bit;
  };
  p.lx = AxisSigned(r.a[0]);
  p.ly = -AxisSigned(r.a[1]);
  switch (prof) {
    case Profile::Identity:  // an Xbox-like pad: X Y left stick, Rx Ry right stick, Z both triggers
      p.rx = AxisSigned(r.a[3]);
      p.ry = -AxisSigned(r.a[4]);
      {
        float z = AxisSigned(r.a[2]);
        p.lt = z > 0 ? z : 0;
        p.rt = z < 0 ? -z : 0;
      }
      for (int i = 0; i < kButtons; ++i) set(btn(i), kButtonBits[i]);
      break;
    case Profile::Sony:  // DualShock 4 / DualSense as Windows reports them
      p.rx = AxisSigned(r.a[2]);
      p.ry = -AxisSigned(r.a[5]);
      p.lt = Axis01(r.a[3]);
      p.rt = Axis01(r.a[4]);
      set(btn(1), XINPUT_GAMEPAD_A);               // cross
      set(btn(2), XINPUT_GAMEPAD_B);               // circle
      set(btn(0), XINPUT_GAMEPAD_X);               // square
      set(btn(3), XINPUT_GAMEPAD_Y);               // triangle
      set(btn(4), XINPUT_GAMEPAD_LEFT_SHOULDER);   // L1
      set(btn(5), XINPUT_GAMEPAD_RIGHT_SHOULDER);  // R1
      set(btn(8), XINPUT_GAMEPAD_BACK);            // share / create
      set(btn(9), XINPUT_GAMEPAD_START);           // options
      set(btn(10), XINPUT_GAMEPAD_LEFT_THUMB);
      set(btn(11), XINPUT_GAMEPAD_RIGHT_THUMB);
      break;
    case Profile::Nintendo:  // Switch Pro: buttons in positional order (B A Y X), digital ZL / ZR
      p.rx = AxisSigned(r.a[2]);
      p.ry = -AxisSigned(r.a[5]);
      p.lt = btn(6) ? 1.0f : 0.0f;
      p.rt = btn(7) ? 1.0f : 0.0f;
      set(btn(0), XINPUT_GAMEPAD_A);
      set(btn(1), XINPUT_GAMEPAD_B);
      set(btn(2), XINPUT_GAMEPAD_X);
      set(btn(3), XINPUT_GAMEPAD_Y);
      set(btn(4), XINPUT_GAMEPAD_LEFT_SHOULDER);
      set(btn(5), XINPUT_GAMEPAD_RIGHT_SHOULDER);
      set(btn(8), XINPUT_GAMEPAD_BACK);
      set(btn(9), XINPUT_GAMEPAD_START);
      set(btn(10), XINPUT_GAMEPAD_LEFT_THUMB);
      set(btn(11), XINPUT_GAMEPAD_RIGHT_THUMB);
      break;
  }
}

void FillInstance(const Entry& en, LPDIDEVICEINSTANCEA i) {
  DWORD sz = i->dwSize ? i->dwSize : sizeof(DIDEVICEINSTANCEA);
  if (sz > sizeof(DIDEVICEINSTANCEA)) sz = sizeof(DIDEVICEINSTANCEA);
  memset(i, 0, sz);
  i->dwSize = sz;
  i->guidInstance = en.instance;
  i->guidProduct = kProductX360;
  i->dwDevType = kDevTypeGamepad;
  snprintf(i->tszInstanceName, sizeof i->tszInstanceName, "%s", en.name.c_str());
  snprintf(i->tszProductName, sizeof i->tszProductName, "%s", kNameX360);
  i->wUsagePage = 1;
  i->wUsage = 5;
}

// True when the foreground window belongs to another process (then the pad reports nothing, like a
// foreground-level DirectInput device). No foreground window at all counts as "ours".
bool OtherAppInFront() {
  HWND fg = GetForegroundWindow();
  if (!fg) return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  return pid != 0 && pid != GetCurrentProcessId();
}

// ------------------------------------------------------------------------------ split triggers (issue #5)
//
// The virtual pad puts both triggers on Z (LT +, RT -) like a wired Xbox 360 pad, so the game's own Xbox 360 table
// (brake = Z+, throttle = Z-) works, but both held at once cancel: a brake press on full throttle reads as nothing.
// The game asks for an action's value through the input manager's vtable 0xBA2904 (.rdata):
//   slot 0 0x40F0F0  float (devid*, action*): the value now (throttle / brake: 41 / 42 swapped unless +0xCF80 is 11)
//   slot 1 0x40EE10  bool (devid*, action*): value and press edges from both snapshots (0x410770)
//   slot 2 0x40E510  bool (devid*, action*): the same from the fixed raw-control table 0xCFA780 (49 / 50 = Z+ / Z-)
// devid = {type, index} (type 1: keyboard), the index picks the pad block (mgr + 0xCF94 + index * 0x560, count
// +0xCF90), whose device is at +0x244 and whose two DIJOYSTATE2 snapshots are at +0x294 / +0x3A4 (GetDeviceState
// fills one per poll, 0x40D9C1). Every Z read (0x40DCE0, controls 4 / 5) is inside these three methods.
// The three slots point at wrappers: they find the row the original will read, the same way it does (data reads
// only); when it is Z+ or Z- and the block's device is one of our virtual pads, the snapshots' lZ hold LT or RT alone
// for that one call (padsplit_core.h). Bindings, the Controls screen and the save are untouched; nothing of the game's
// code is written.

constexpr uintptr_t kQueryVt = 0xBA2904;
constexpr uint32_t kQueryOrig[3] = {0x40F0F0, 0x40EE10, 0x40E510};
constexpr uintptr_t kFixedRows = 0xCFA780;  // slot 2's table, rows {action, vk, ctl, kind, angle}, ends at action 0x45
constexpr uint32_t kActionEnd = 0x45, kRows = 0x45, kRowSize = 0x14, kKindAxis = 1, kTypeKeyboard = 1;
constexpr uint32_t kEntryCount = 0xCF6C, kEntries = 0xCF70, kEntrySize = 0x584, kEntryId = 0x564, kEntryValid = 0x56C;
constexpr uint32_t kSwapMode = 0xCF80, kBlockCount = 0xCF90, kBlocks = 0xCF94, kBlockSize = 0x560, kMaxBlocks = 4;
constexpr uint32_t kBlockDevice = 0x244, kBlockSnap = 0x294, kSnapSize = 0x110;

padsplit::Dev g_split[8];   // the virtual pads the game holds (game thread only)
bool g_split_on = false;    // the wrappers are in

uint32_t Rd(uintptr_t a) { return *reinterpret_cast<const uint32_t*>(a); }

padsplit::Dev* SplitFind(const void* dev) {
  if (!dev) return nullptr;
  for (auto& d : g_split)
    if (d.dev == dev) return &d;
  return nullptr;
}

void SplitRegister(const void* dev, int list_index) {
  if (SplitFind(dev)) return;
  for (auto& d : g_split)
    if (!d.dev) {
      d = padsplit::Dev();
      d.dev = dev;
      d.list_index = list_index;
      return;
    }
}

void SplitForget(const void* dev) {
  if (padsplit::Dev* d = SplitFind(dev)) *d = padsplit::Dev();
}

// The control the original method `slot` reads for this query when it is an axis half of a pad, and that pad's block.
int SplitControl(int slot, uintptr_t mgr, const uint32_t* devid, const uint32_t* action, uintptr_t* block) {
  if (!devid || !action || devid[0] == kTypeKeyboard) return -1;
  uintptr_t row = 0;
  if (slot == 2) {
    for (uint32_t i = 0; i < 0x100 && !row; ++i) {
      const uintptr_t r = kFixedRows + i * kRowSize;
      if (Rd(r) == kActionEnd) break;
      if (Rd(r) == *action) row = r;
    }
  } else {
    const uintptr_t entries = Rd(mgr + kEntries);
    const uint32_t count = Rd(mgr + kEntryCount);
    uintptr_t e = 0;
    for (uint32_t i = 0; i < count && i < 16 && !e; ++i) {
      const uintptr_t c = entries + i * kEntrySize;
      if (*reinterpret_cast<const uint8_t*>(c + kEntryValid) && Rd(c + kEntryId) == devid[0] && Rd(c + kEntryId + 4) == devid[1]) e = c;
    }
    if (!e) return -1;
    uint32_t act = *action;
    if (slot == 0 && Rd(mgr + kSwapMode) != 11) act = act == 41 ? 42 : 41;  // as 0x40F146 does
    for (uint32_t i = 0; i < kRows && !row; ++i)
      if (Rd(e + i * kRowSize) == act) row = e + i * kRowSize;
  }
  if (!row || Rd(row + 12) != kKindAxis) return -1;
  const uint32_t index = devid[1];
  if (index >= Rd(mgr + kBlockCount) || index >= kMaxBlocks) return -1;
  *block = mgr + kBlocks + index * kBlockSize;
  return static_cast<int>(Rd(row + 8));
}

// Before the original: the swaps for this query (0 when none).
int SplitBefore(int slot, void* self, const uint32_t* devid, const uint32_t* action, padsplit::Swap sw[2]) {
  if (!g_split_on) return 0;
  uintptr_t block = 0;
  const int ctl = SplitControl(slot, reinterpret_cast<uintptr_t>(self), devid, action, &block);
  if (ctl != padsplit::kCtlZPos && ctl != padsplit::kCtlZNeg) return 0;
  padsplit::Dev* d = SplitFind(*reinterpret_cast<void* const*>(block + kBlockDevice));
  if (!d) return 0;  // a passed-through device: its own axes as they are
  const uintptr_t bufs[2] = {block + kBlockSnap, block + kBlockSnap + kSnapSize};
  const int n = padsplit::Apply(*d, bufs, ctl, sw);
  if (n && !d->logged) {
    d->logged = true;
    ss_log("[pad] device #%d: first split trigger read (action %u reads %s alone, query slot %d)", d->list_index,
           static_cast<unsigned>(*action), ctl == padsplit::kCtlZPos ? "LT (Z+)" : "RT (Z-)", slot);
  }
  return n;
}

#define SPLIT_THIS __attribute__((thiscall, force_align_arg_pointer))
using QueryValueFn = float(__attribute__((thiscall)) *)(void*, const uint32_t*, const uint32_t*);
using QueryEdgeFn = uint32_t(__attribute__((thiscall)) *)(void*, const uint32_t*, const uint32_t*);

float SPLIT_THIS SplitQuery0(void* self, const uint32_t* devid, const uint32_t* action) {
  padsplit::Swap sw[2];
  const int n = SplitBefore(0, self, devid, action, sw);
  const float v = reinterpret_cast<QueryValueFn>(kQueryOrig[0])(self, devid, action);
  padsplit::Restore(sw, n);
  return v;
}
template <int kSlot>
uint32_t SPLIT_THIS SplitQueryEdge(void* self, const uint32_t* devid, const uint32_t* action) {
  padsplit::Swap sw[2];
  const int n = SplitBefore(kSlot, self, devid, action, sw);
  const uint32_t v = reinterpret_cast<QueryEdgeFn>(kQueryOrig[kSlot])(self, devid, action);
  padsplit::Restore(sw, n);
  return v;
}

// Once, when the layer starts (the game has called DirectInput8Create: its image is unpacked).
void SplitInstall() {
  if (!g_cfg.split_triggers) {
    ss_log("[pad] split triggers off (pad_split_triggers = false): LT and RT share the Z axis, as on a stock Xbox 360 pad");
    return;
  }
  const uintptr_t mine[3] = {reinterpret_cast<uintptr_t>(&SplitQuery0), reinterpret_cast<uintptr_t>(&SplitQueryEdge<1>),
                             reinterpret_cast<uintptr_t>(&SplitQueryEdge<2>)};
  uint32_t* vt = reinterpret_cast<uint32_t*>(kQueryVt);
  bool known = !IsBadReadPtr(vt, 12);
  for (int i = 0; known && i < 3; ++i) known = vt[i] == kQueryOrig[i] || vt[i] == mine[i];
  if (!known) {
    ss_log("[pad] split triggers not in: the input query table at 0x%08X is not the known build (LT and RT share Z)",
           static_cast<unsigned>(kQueryVt));
    return;
  }
  for (int i = 0; i < 3; ++i)
    if (vt[i] != mine[i] && !ss_write_word(&vt[i], mine[i])) {
      ss_log("[pad] split triggers not in: cannot write query slot %d (error %lu)", i, GetLastError());
      for (int j = 0; j < i; ++j) ss_write_word(&vt[j], kQueryOrig[j]);
      return;
    }
  g_split_on = true;
  ss_log("[pad] split triggers on: an action bound to Z+ / Z- reads LT / RT alone on the virtual pads (query slots 0-2 of 0x%08X)",
         static_cast<unsigned>(kQueryVt));
}

struct VPad final : IDirectInputDevice8A {
  LONG refs = 1;
  Entry e;
  IDirectInputDevice8A* inner = nullptr;  // DirectInputPad: the real device
  LONG rmin[5] = {0, 0, 0, 0, 0};
  LONG rmax[5] = {65535, 65535, 65535, 65535, 65535};  // DirectInput's default range until the game sets its own
  WORD last_buttons = 0;
  WORD real_last = 0;  // the pad's own buttons at the last read (before the menu, the taps): skip_intro's user check
  bool first_state = true;

  LONG z_plus = 0, z_minus = 0;  // the last Fill's lZ with LT alone / RT alone (split triggers)

  VPad(const Entry& en, IDirectInputDevice8A* in) : e(en), inner(in) {
    SplitRegister(static_cast<IDirectInputDevice8A*>(this), e.list_index);
  }

  // ---- IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** o) override {
    if (IsEqualIID(r, IID_IUnknown) || IsEqualIID(r, IID_IDirectInputDevice8A) || IsEqualIID(r, IID_IDirectInputDevice2A) ||
        IsEqualIID(r, IID_IDirectInputDeviceA)) {
      *o = static_cast<IDirectInputDevice8A*>(this);
      AddRef();
      return S_OK;
    }
    *o = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
  ULONG STDMETHODCALLTYPE Release() override {
    LONG n = InterlockedDecrement(&refs);
    if (n == 0) {
      if (e.kind == Kind::XInputSlot && g_xi.set && g_xi.set_is_separate) {
        XINPUT_VIBRATION z = {0, 0};
        g_xi.set(static_cast<DWORD>(e.slot), &z);
      }
      if (inner) inner->Release();
      SplitForget(static_cast<IDirectInputDevice8A*>(this));
      delete this;
    }
    return n;
  }

  // ---- the state
  bool ReadPad(Pad& p) {
    if (e.kind == Kind::XInputSlot) return ReadXInput(e.slot, p);
    if (!inner) return false;
    RawJoy raw;
    for (int attempt = 0; attempt < 2; ++attempt) {
      inner->Poll();
      memset(&raw, 0, sizeof raw);
      HRESULT hr = inner->GetDeviceState(sizeof raw, &raw);
      if (SUCCEEDED(hr)) {
        RawToPad(raw, e.profile, p);
        return true;
      }
      if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
        if (FAILED(inner->Acquire())) break;
        continue;
      }
      break;
    }
    return false;
  }

  void Fill(const Pad& in, void* data, DWORD cb) {
    memset(data, 0, cb);
    LONG* ax = static_cast<LONG*>(data);
    DWORD* pov = reinterpret_cast<DWORD*>(static_cast<BYTE*>(data) + 0x20);
    BYTE* btn = static_cast<BYTE*>(data) + 0x30;
    for (int i = 0; i < 4; ++i) pov[i] = 0xFFFFFFFF;
    float lx, ly, rx, ry;
    DeadzoneStick(in.lx, in.ly, g_cfg.deadzone_left / 100.0f, lx, ly);
    DeadzoneStick(in.rx, in.ry, g_cfg.deadzone_right / 100.0f, rx, ry);
    float th = g_cfg.trigger_threshold / 100.0f;
    float lt = DeadzoneTrigger(in.lt, th), rt = DeadzoneTrigger(in.rt, th);
    // DirectInput: Y grows downwards; a wired Xbox 360 pad puts both triggers on Z (left +, right -).
    ax[0] = ScaleAxis(lx, rmin[0], rmax[0]);
    ax[1] = ScaleAxis(-ly, rmin[1], rmax[1]);
    ax[2] = ScaleAxis(lt - rt, rmin[2], rmax[2]);
    z_plus = ScaleAxis(lt, rmin[2], rmax[2]);  // the halves alone, for the game's Z+ / Z- reads (split triggers)
    z_minus = ScaleAxis(-rt, rmin[2], rmax[2]);
    ax[3] = ScaleAxis(rx, rmin[3], rmax[3]);
    ax[4] = ScaleAxis(-ry, rmin[4], rmax[4]);
    ax[5] = ScaleAxis(0, rmin[0], rmax[0]);  // Rz: a pad that has none, centred
    ax[6] = 0;                               // sliders: none
    ax[7] = 0;
    pov[0] = PovFromDpad(in.buttons);
    for (int i = 0; i < kButtons; ++i)
      if (in.buttons & kButtonBits[i]) btn[i] = 0x80;
  }

  // ---- IDirectInputDevice8A
  HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS c) override {
    if (!c || c->dwSize < 24) return DIERR_INVALIDPARAM;
    DWORD sz = c->dwSize;
    memset(c, 0, sz);
    c->dwSize = sz;
    c->dwFlags = DIDC_ATTACHED;
    c->dwDevType = kDevTypeGamepad;
    c->dwAxes = 5;
    c->dwButtons = kButtons;
    c->dwPOVs = 1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID ref, DWORD flags) override {
    if (!cb) return DIERR_INVALIDPARAM;
    for (const PadObject& o : g_objects) {
      if (!ObjectMatches(o, flags)) continue;
      DIDEVICEOBJECTINSTANCEA d;
      memset(&d, 0, sizeof d);
      d.dwSize = sizeof d;
      d.guidType = *o.type;
      d.dwOfs = o.ofs;
      d.dwType = o.dwtype;
      d.dwFlags = o.flags;
      snprintf(d.tszName, sizeof d.tszName, "%s", o.name);
      if (cb(&d, ref) == DIENUM_STOP) break;
    }
    return S_OK;
  }
  static int AxisOfProp(const DIPROPHEADER* h) {
    if (h->dwHow == DIPH_BYOFFSET) {
      if (h->dwObj < 20 && (h->dwObj % 4) == 0) return static_cast<int>(h->dwObj / 4);
      return -1;
    }
    if (h->dwHow == DIPH_DEVICE) return 0;
    return -1;
  }
  HRESULT STDMETHODCALLTYPE GetProperty(REFGUID prop, LPDIPROPHEADER h) override {
    uintptr_t id = reinterpret_cast<uintptr_t>(&prop);  // the property ids are small integers passed as GUID references
    if (!h) return DIERR_INVALIDPARAM;
    if (id == 4) {  // DIPROP_RANGE
      auto* r = reinterpret_cast<DIPROPRANGE*>(h);
      int a = AxisOfProp(h);
      if (a < 0) return DIERR_OBJECTNOTFOUND;
      r->lMin = rmin[a];
      r->lMax = rmax[a];
      return S_OK;
    }
    if (id == 5 || id == 6 || id == 3 || id == 1 || id == 2 || id == 9) {  // dead zone, saturation, granularity ...
      reinterpret_cast<DIPROPDWORD*>(h)->dwData = 0;
      return S_OK;
    }
    return DIERR_UNSUPPORTED;
  }
  HRESULT STDMETHODCALLTYPE SetProperty(REFGUID prop, LPCDIPROPHEADER h) override {
    uintptr_t id = reinterpret_cast<uintptr_t>(&prop);
    if (!h) return DIERR_INVALIDPARAM;
    if (id == 4) {  // DIPROP_RANGE
      auto* r = reinterpret_cast<const DIPROPRANGE*>(h);
      if (h->dwHow == DIPH_DEVICE) {
        for (int i = 0; i < 5; ++i) {
          rmin[i] = r->lMin;
          rmax[i] = r->lMax;
        }
        return S_OK;
      }
      int a = AxisOfProp(h);
      if (a < 0) return DIERR_OBJECTNOTFOUND;
      rmin[a] = r->lMin;
      rmax[a] = r->lMax;
      return S_OK;
    }
    // Dead zone, saturation, axis mode, auto-centre, buffer size, FF gain ...: accepted, nothing to do
    // (the layer applies its own dead zone; the data is absolute and unbuffered).
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Acquire() override {
    if (inner) inner->Acquire();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Unacquire() override {
    if (inner) inner->Unacquire();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD cb, LPVOID data) override {
    if (!data) return DIERR_INVALIDPARAM;
    if (cb != kStateSize2 && cb != kStateSize1) return DIERR_INVALIDPARAM;
    Pad p;
    bool ok = ReadPad(p);
    if (ok && g_cfg.foreground_only && OtherAppInFront()) ok = false;
    if (ok) GlyphWatch(p);
    if (ok) {
      if (!first_state && (p.buttons & ~real_last)) {  // a new press by the player
        ss_intro_user_input();
        ss_boot_user_input();
      }
      real_last = p.buttons;
    }
    if (ok) ss_menu_pad_state(e.list_index, p.buttons, p.lx, p.ly);
    if (!ok || ss_menu_capturing()) p = Pad();  // the settings menu has the pads: the game sees them centred
    if (ss_intro_tap(e.list_index)) p.buttons |= XINPUT_GAMEPAD_A;  // skip_intro: taps while the boot movies run
    Fill(p, data, cb);
    if (padsplit::Dev* d = SplitFind(static_cast<IDirectInputDevice8A*>(this)))
      padsplit::Record(*d, reinterpret_cast<uintptr_t>(data), z_plus, z_minus, static_cast<LONG*>(data)[2]);
    if (first_state) {
      first_state = false;
      ss_log("[pad] device #%d (%s): first state read, pad %s", e.list_index, e.name.c_str(), ok ? "reports" : "silent");
    }
    if (g_cfg.log_input && p.buttons != last_buttons) {
      last_buttons = p.buttons;
      if (g_log_budget_input > 0) {
        --g_log_budget_input;
        char names[96] = "";
        for (int i = 0; i < kButtons; ++i)
          if (p.buttons & kButtonBits[i]) {
            strncat(names, kButtonNames[i], sizeof names - strlen(names) - 2);
            strncat(names, " ", sizeof names - strlen(names) - 1);
          }
        if (p.buttons & 0x000F) strncat(names, "dpad", sizeof names - strlen(names) - 1);
        ss_log("[pad] device #%d buttons 0x%04x  %s", e.list_index, p.buttons, names);
      }
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD, LPDIDEVICEOBJECTDATA, LPDWORD n, DWORD) override {
    if (n) *n = 0;
    return S_OK;  // unbuffered device: nothing queued
  }
  HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT) override { return S_OK; }  // the game's c_dfDIJoystick2 is what we fill
  HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND h, DWORD) override {
    // The real device (translated pads) is read non-exclusively and in the background; the foreground rule
    // is applied in GetDeviceState.
    if (inner) inner->SetCooperativeLevel(h, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetObjectInfo(LPDIDEVICEOBJECTINSTANCEA o, DWORD obj, DWORD how) override {
    if (!o) return DIERR_INVALIDPARAM;
    for (const PadObject& po : g_objects) {
      bool hit = (how == DIPH_BYOFFSET && po.ofs == obj) || (how == DIPH_BYID && po.dwtype == obj);
      if (!hit) continue;
      DWORD sz = o->dwSize;
      memset(o, 0, sz);
      o->dwSize = sz;
      o->guidType = *po.type;
      o->dwOfs = po.ofs;
      o->dwType = po.dwtype;
      o->dwFlags = po.flags;
      snprintf(o->tszName, sizeof o->tszName, "%s", po.name);
      return S_OK;
    }
    return DIERR_OBJECTNOTFOUND;
  }
  HRESULT STDMETHODCALLTYPE GetDeviceInfo(LPDIDEVICEINSTANCEA i) override {
    if (!i) return DIERR_INVALIDPARAM;
    FillInstance(e, i);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE RunControlPanel(HWND, DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD, REFGUID) override { return DIERR_ALREADYINITIALIZED; }
  HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID, LPCDIEFFECT, LPDIRECTINPUTEFFECT*, LPUNKNOWN) override { return DIERR_UNSUPPORTED; }
  HRESULT STDMETHODCALLTYPE EnumEffects(LPDIENUMEFFECTSCALLBACKA, LPVOID, DWORD) override { return S_OK; }  // none: rumble goes through XInput
  HRESULT STDMETHODCALLTYPE GetEffectInfo(LPDIEFFECTINFOA, REFGUID) override { return DIERR_UNSUPPORTED; }
  HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD) override { return DIERR_UNSUPPORTED; }
  HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD) override { return DIERR_UNSUPPORTED; }
  HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK, LPVOID, DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE) override { return DIERR_UNSUPPORTED; }
  HRESULT STDMETHODCALLTYPE Poll() override { return S_OK; }  // the state is read when asked for
  HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD, LPCDIDEVICEOBJECTDATA, LPDWORD, DWORD) override { return DIERR_UNSUPPORTED; }
  HRESULT STDMETHODCALLTYPE EnumEffectsInFile(LPCSTR, LPDIENUMEFFECTSINFILECALLBACK, LPVOID, DWORD) override { return DIERR_UNSUPPORTED; }
  HRESULT STDMETHODCALLTYPE WriteEffectToFile(LPCSTR, DWORD, LPDIFILEEFFECT, DWORD) override { return DIERR_UNSUPPORTED; }
  HRESULT STDMETHODCALLTYPE BuildActionMap(LPDIACTIONFORMATA, LPCSTR, DWORD) override { return DIERR_UNSUPPORTED; }
  HRESULT STDMETHODCALLTYPE SetActionMap(LPDIACTIONFORMATA, LPCSTR, DWORD) override { return DIERR_UNSUPPORTED; }
  HRESULT STDMETHODCALLTYPE GetImageInfo(LPDIDEVICEIMAGEINFOHEADERA) override { return DIERR_UNSUPPORTED; }
};

// ------------------------------------------------------------------------------ the DirectInput wrapper

GUID SyntheticInstance(int slot) {
  // {53535850-4144-000N-5353-504144303030}: ASCII "SSXP", "AD", slot, "SSPAD000"
  GUID g = {0x53535850, 0x4144, static_cast<WORD>(slot), {'S', 'S', 'P', 'A', 'D', '0', '0', '0'}};
  return g;
}

GUID TranslatedInstance(const GUID& real) {
  GUID g = real;
  g.Data1 ^= 0x53534D50;  // stays unique, never collides with a real instance id
  return g;
}

struct Candidate {  // a real DirectInput game controller
  DIDEVICEINSTANCEA inst;
};

BOOL CALLBACK CollectReal(const DIDEVICEINSTANCEA* d, LPVOID ref) {
  static_cast<std::vector<Candidate>*>(ref)->push_back({*d});
  return DIENUM_CONTINUE;
}

// The file name of the module that holds an address ("?" when none does).
std::string ModuleNameOf(const void* addr) {
  HMODULE m = nullptr;
  char path[MAX_PATH] = "";
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<LPCSTR>(addr), &m) ||
      !GetModuleFileNameA(m, path, sizeof path))
    return "?";
  const char* slash = strrchr(path, '\\');
  return slash ? slash + 1 : path;
}

bool IsPadClass(DWORD devtype) {
  BYTE t = static_cast<BYTE>(devtype & 0xFF);
  return t == DI8DEVTYPE_GAMEPAD || t == DI8DEVTYPE_JOYSTICK;
}

Profile ProfileFor(DWORD product) {
  switch (Vid(product)) {
    case 0x054C: return Profile::Sony;
    case 0x057E: return Profile::Nintendo;
    default: return Profile::Identity;  // Xbox family and anything else: the Xbox order is assumed
  }
}

struct WrapDI final : IDirectInput8A {
  LONG refs = 1;
  IDirectInput8A* real;
  explicit WrapDI(IDirectInput8A* r) : real(r) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** o) override {
    if (IsEqualIID(r, IID_IUnknown) || IsEqualIID(r, IID_IDirectInput8A)) {
      *o = static_cast<IDirectInput8A*>(this);
      AddRef();
      return S_OK;
    }
    return real->QueryInterface(r, o);
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
  ULONG STDMETHODCALLTYPE Release() override {
    LONG n = InterlockedDecrement(&refs);
    if (n == 0) {
      real->Release();
      delete this;
    }
    return n;
  }

  HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID guid, LPDIRECTINPUTDEVICE8A* out, LPUNKNOWN outer) override {
    Entry found;
    bool have = false;
    EnterCriticalSection(&g_cs);
    for (Entry& en : g_entries) {
      if (IsEqualGUID(en.instance, guid)) {
        en.list_index = g_created++;
        found = en;
        have = true;
        break;
      }
    }
    LeaveCriticalSection(&g_cs);
    if (!have) {  // the keyboard, the mouse: the settings menu (when it is wanted) keeps them quiet while it is open
      HRESULT hr = real->CreateDevice(guid, out, outer);
      if (SUCCEEDED(hr) && out && *out) {  // a game controller this layer did not offer: say which (diagnosis)
        DIDEVICEINSTANCEA di;
        memset(&di, 0, sizeof di);
        di.dwSize = sizeof di;
        if (SUCCEEDED((*out)->GetDeviceInfo(&di)) && IsPadClass(di.dwDevType))
          ss_log("[pad] the game opens a controller this layer did not offer: '%s' product %04X:%04X (its own table decides)",
                 di.tszInstanceName, (unsigned)Vid(di.guidProduct.Data1), (unsigned)(di.guidProduct.Data1 >> 16));
      }
      if (SUCCEEDED(hr) && out && *out && !outer && ss_menu_wanted()) *out = static_cast<LPDIRECTINPUTDEVICE8A>(ss_menu_wrap_device(*out));
      return hr;
    }
    ss_log("[pad] game opens device #%d: %s%s", found.list_index, found.name.c_str(), found.virt ? "" : " (passed through)");
    if (!found.virt) return real->CreateDevice(found.real_instance, out, outer);
    if (found.kind == Kind::XInputSlot) {
      *out = new VPad(found, nullptr);
      return S_OK;
    }
    IDirectInputDevice8A* dev = nullptr;
    HRESULT hr = real->CreateDevice(found.real_instance, &dev, nullptr);
    if (FAILED(hr)) return hr;
    dev->SetDataFormat(&g_rawfmt);
    for (DWORD i = 0; i < 6; ++i) {
      DIPROPRANGE r;
      r.diph.dwSize = sizeof r;
      r.diph.dwHeaderSize = sizeof(DIPROPHEADER);
      r.diph.dwObj = i * 4;
      r.diph.dwHow = DIPH_BYOFFSET;
      r.lMin = 0;
      r.lMax = 65535;
      dev->SetProperty(DIPROP_RANGE, &r.diph);
      DIPROPDWORD dz;
      dz.diph.dwSize = sizeof dz;
      dz.diph.dwHeaderSize = sizeof(DIPROPHEADER);
      dz.diph.dwObj = i * 4;
      dz.diph.dwHow = DIPH_BYOFFSET;
      dz.dwData = 0;
      dev->SetProperty(DIPROP_DEADZONE, &dz.diph);
    }
    *out = new VPad(found, dev);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE EnumDevices(DWORD type, LPDIENUMDEVICESCALLBACKA cb, LPVOID ref, DWORD flags) override {
    if (type != DI8DEVCLASS_GAMECTRL || !cb) return real->EnumDevices(type, cb, ref, flags);

    std::vector<Candidate> reals;
    real->EnumDevices(DI8DEVCLASS_GAMECTRL, CollectReal, &reals, flags);

    // XInput pads, in slot order. With pad_hotplug all four slots are offered, connected or not.
    std::vector<int> xslots;
    int connected = 0;
    for (int s = 0; s < 4; ++s) {
      Pad p;
      bool on = ReadXInput(s, p);
      if (on) ++connected;
      if (on || g_cfg.hotplug) xslots.push_back(s);
    }

    std::vector<Entry> list;
    for (int s : xslots) {
      Entry en;
      en.kind = Kind::XInputSlot;
      en.slot = s;
      en.instance = SyntheticInstance(s);
      en.product = kProductX360.Data1;
      char nm[96];
      snprintf(nm, sizeof nm, "%s (XInput slot %d)", kNameX360, s);
      en.name = nm;
      list.push_back(en);
    }
    // Real pads the game would not know and XInput might already carry.
    int real_pads = 0;
    for (const Candidate& c : reals) {
      DWORD prod = c.inst.guidProduct.Data1;
      bool known_other = GameKnows(prod) && Vid(prod) != 0x045E;
      if (IsPadClass(c.inst.dwDevType) && !known_other) ++real_pads;
    }
    for (const Candidate& c : reals) {
      const DIDEVICEINSTANCEA& d = c.inst;
      DWORD prod = d.guidProduct.Data1;
      const char* verdict;
      Entry en;
      en.real_instance = d.guidInstance;
      en.product = prod;
      en.name = d.tszInstanceName;
      bool xbox_vid = Vid(prod) == 0x045E;
      bool known_other = GameKnows(prod) && !xbox_vid;  // the game has its own table (older pads, wheels)
      if (known_other || !IsPadClass(d.dwDevType)) {
        verdict = known_other ? "passed through (the game has a table for this product)" : "passed through (not a pad)";
        en.virt = false;
        en.instance = d.guidInstance;
        list.push_back(en);
      } else if (connected > 0 && connected >= real_pads) {
        verdict = "hidden (XInput carries it)";
      } else if (!g_cfg.translate) {
        verdict = "passed through (pad_translate_dinput off)";
        en.virt = false;
        en.instance = d.guidInstance;
        list.push_back(en);
      } else {
        en.kind = Kind::DirectInputPad;
        en.virt = true;
        en.instance = TranslatedInstance(d.guidInstance);
        en.profile = xbox_vid ? Profile::Identity : ProfileFor(prod);
        char nm[200];
        snprintf(nm, sizeof nm, "%s (as Xbox 360 pad)", d.tszInstanceName);
        en.name = nm;
        verdict = en.profile == Profile::Sony       ? "translated (Sony layout)"
                  : en.profile == Profile::Nintendo ? "translated (Nintendo layout)"
                                                    : "translated (Xbox layout assumed)";
        list.push_back(en);
      }
      ss_log("[pad] real device '%s' product %04X:%04X type 0x%lx: %s", d.tszInstanceName, (unsigned)Vid(prod), (unsigned)(prod >> 16),
             static_cast<unsigned long>(d.dwDevType), verdict);
    }
    if (list.size() > 4) {
      ss_log("[pad] %u devices found, the game takes four: the rest is left out", (unsigned)list.size());
      list.resize(4);
    }

    EnterCriticalSection(&g_cs);
    g_entries = list;
    g_created = 0;
    LeaveCriticalSection(&g_cs);
    {
      bool have_pad = false;
      for (const Entry& en : list)
        if (en.virt) have_pad = true;
      if (connected > 0 && have_pad) {
        g_glyph_pad = true;
        ss_glyphs_input(true, "pad present at start");
      }
      // The pad names (glyphs.cpp) are the virtual pad's: they hold while no device of another layout is passed through.
      bool xbox_layout = true;
      for (const Entry& en : list)
        if (!en.virt && Vid(en.product) != 0x045E) xbox_layout = false;
      ss_glyphs_pad_layout(xbox_layout);
      for (const Candidate& c : reals)  // the kind of pad, for button_prompts = auto (glyphs.cpp's fallback)
        if (IsPadClass(c.inst.dwDevType)) {
          ss_glyphs_directinput_pad(c.inst.guidProduct.Data1);
          break;
        }
    }

    ss_log("[pad] the game is offered %u controller(s)%s:", (unsigned)list.size(), g_cfg.hotplug ? " (hot-plug: all four XInput slots)" : "");
    for (size_t i = 0; i < list.size(); ++i) {
      const Entry& en = list[i];
      if (en.virt && en.kind == Kind::XInputSlot)
        ss_log("[pad]   #%u XInput slot %d as Xbox 360 pad (045E:028E), rumble -> slot %d%s", (unsigned)i, en.slot, en.slot,
               g_cfg.rumble ? "" : " (off)");
      else if (en.virt)
        ss_log("[pad]   #%u DirectInput '%s' as Xbox 360 pad, no rumble", (unsigned)i, en.name.c_str());
      else
        ss_log("[pad]   #%u '%s' unchanged", (unsigned)i, en.name.c_str());
    }

    // The game's callback opens each device it is given right away (CreateDevice inside the callback), so a
    // callback from another module that opens none of them is a filter in between: Steam's overlay
    // (GameOverlayRenderer.dll) does this while Steam Input is on for the game, and the game then never sees these pads.
    const std::string cb_module = ModuleNameOf(reinterpret_cast<const void*>(cb));
    const bool cb_is_game = GetModuleHandleA(nullptr) == [&] {
      HMODULE m = nullptr;
      GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(cb), &m);
      return m;
    }();
    if (!cb_is_game)
      ss_log("[pad] the device list goes to a callback in %s, not the game's own: the offered pads go without a HID usage "
             "(Steam Input hides Xbox-type HID pads from the game)", cb_module.c_str());
    for (const Entry& en : list) {
      DIDEVICEINSTANCEA d;
      memset(&d, 0, sizeof d);
      d.dwSize = sizeof d;
      if (en.virt) {
        FillInstance(en, &d);
        // Steam's overlay (Steam Input on) wraps this interface and drops, from what it passes on to the game, every
        // device whose HID usage is joystick/gamepad (page 1, usage 4 or 5) and whose product GUID is a VID/PID
        // ("...PIDVID") of a controller it knows, 045E:028E among them; then it appends its own emulated pads.
        // The game never reads the usage fields (only the product GUID picks its table), so a filter in between
        // gets them blank: the pads pass, the game sees the same Xbox 360 pad, and takes them first (four at most).
        if (!cb_is_game) {
          d.wUsagePage = 0;
          d.wUsage = 0;
        }
      } else {
        for (const Candidate& c : reals)
          if (IsEqualGUID(c.inst.guidInstance, en.instance)) d = c.inst;
      }
      if (cb(&d, ref) == DIENUM_STOP) break;
    }
    EnterCriticalSection(&g_cs);
    const int opened = g_created;
    LeaveCriticalSection(&g_cs);
    if (!list.empty() && opened == 0 && !cb_is_game)
      ss_log("[pad] none of the %u offered controllers was opened: %s hid them from the game (Steam Input on for this game?); "
             "the game reads only the pads that module gives it, not this layer's",
             (unsigned)list.size(), cb_module.c_str());
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID g) override { return real->GetDeviceStatus(g); }
  HRESULT STDMETHODCALLTYPE RunControlPanel(HWND h, DWORD f) override { return real->RunControlPanel(h, f); }
  HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE h, DWORD v) override { return real->Initialize(h, v); }
  HRESULT STDMETHODCALLTYPE FindDevice(REFGUID g, LPCSTR n, LPGUID o) override { return real->FindDevice(g, n, o); }
  HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(LPCSTR u, LPDIACTIONFORMATA a, LPDIENUMDEVICESBYSEMANTICSCBA c, LPVOID r,
                                                   DWORD f) override {
    return real->EnumDevicesBySemantics(u, a, c, r, f);
  }
  HRESULT STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK c, LPDICONFIGUREDEVICESPARAMSA p, DWORD f, LPVOID r) override {
    return real->ConfigureDevices(c, p, f, r);
  }
};

// ----------------------------------------------------------------------------------- the rumble routing

int g_rumble_log = 30;

DWORD WINAPI HookedSetState(DWORD game_index, XINPUT_VIBRATION* v) {
  if (!v) return ERROR_BAD_ARGUMENTS;
  if (!g_xi.set) return ERROR_DEVICE_NOT_CONNECTED;
  int slot = -1;
  bool any_virtual = false;
  EnterCriticalSection(&g_cs);
  for (const Entry& en : g_entries) {
    if (en.virt) any_virtual = true;
    if (en.list_index == static_cast<int>(game_index) && en.virt && en.kind == Kind::XInputSlot) slot = en.slot;
  }
  LeaveCriticalSection(&g_cs);
  if (slot < 0) {
    if (!any_virtual) return g_xi.set(game_index, v);  // nothing of ours in the list: as the game asked
    return ERROR_DEVICE_NOT_CONNECTED;                  // a wheel, a translated pad or nothing: never someone else's pad
  }
  XINPUT_VIBRATION out = *v;
  if (!g_cfg.rumble) {
    out.wLeftMotorSpeed = out.wRightMotorSpeed = 0;
  } else if (g_cfg.rumble_gain != 100) {
    unsigned l = static_cast<unsigned>(out.wLeftMotorSpeed) * g_cfg.rumble_gain / 100;
    unsigned r = static_cast<unsigned>(out.wRightMotorSpeed) * g_cfg.rumble_gain / 100;
    out.wLeftMotorSpeed = static_cast<WORD>(l > 65535 ? 65535 : l);
    out.wRightMotorSpeed = static_cast<WORD>(r > 65535 ? 65535 : r);
  }
  if ((v->wLeftMotorSpeed || v->wRightMotorSpeed) && g_rumble_log > 0) {
    --g_rumble_log;
    ss_log("[pad] rumble: game device #%lu -> XInput slot %d, motors %u/%u", static_cast<unsigned long>(game_index), slot,
           out.wLeftMotorSpeed, out.wRightMotorSpeed);
  }
  return g_xi.set(static_cast<DWORD>(slot), &out);
}

// Redirect the entry of xinput1_3.dll's XInputSetState (the function the game's import resolves to) to ours.
// This is the system DLL's memory, not the game's; the game's image and import table are not touched.
// Put in once, at start-up (from the game's DirectInput8Create, before its pads exist and before it vibrates any),
// whatever pad_rumble says: the hook itself sends zero motor speeds while vibration is off, so the overlay can switch
// it on and off later without touching the code again. The 5 bytes of the jump are written with one locked 8-byte
// compare-exchange (the 3 bytes after them rewritten with their own value), so no thread can see half a jump.
bool HookXInputSetState() {
  HMODULE m = GetModuleHandleA("xinput1_3.dll");
  if (!m) {
    ss_log("[pad] rumble routing: xinput1_3.dll is not loaded, the game's rumble stays as it is");
    return false;
  }
  if (!g_xi.set_is_separate) {
    ss_log("[pad] rumble routing: no second XInput module to send through, the game's rumble stays as it is");
    return false;
  }
  auto target = reinterpret_cast<BYTE*>(GetProcAddress(m, "XInputSetState"));
  if (!target || target == reinterpret_cast<BYTE*>(g_xi.set)) {
    ss_log("[pad] rumble routing: XInputSetState not found in xinput1_3.dll");
    return false;
  }
  HMODULE owner = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(target), &owner) ||
      owner != m) {
    ss_log("[pad] rumble routing: XInputSetState at %p is not in xinput1_3.dll's own code (forwarded?): nothing done", target);
    return false;
  }
  // The qword the 5 bytes sit in: the aligned one when it holds them all, else the one starting at the entry.
  const uintptr_t t = reinterpret_cast<uintptr_t>(target);
  BYTE* q = (t & 7) <= 3 ? reinterpret_cast<BYTE*>(t & ~static_cast<uintptr_t>(7)) : target;
  const size_t at = static_cast<size_t>(target - q);
  DWORD old = 0;
  if (!VirtualProtect(q, 8, PAGE_EXECUTE_READWRITE, &old)) {
    ss_log("[pad] rumble routing: cannot unprotect XInputSetState (error %lu)", GetLastError());
    return false;
  }
  LONGLONG cur;
  memcpy(&cur, q, 8);
  BYTE nb[8];
  memcpy(nb, &cur, 8);
  nb[at] = 0xE9;
  int32_t rel = static_cast<int32_t>(reinterpret_cast<BYTE*>(&HookedSetState) - (target + 5));
  memcpy(nb + at + 1, &rel, 4);
  LONGLONG now;
  memcpy(&now, nb, 8);
  const bool ok = InterlockedCompareExchange64(reinterpret_cast<volatile LONGLONG*>(q), now, cur) == cur;
  FlushInstructionCache(GetCurrentProcess(), q, 8);
  DWORD dummy;
  VirtualProtect(q, 8, old, &dummy);
  if (!ok) {
    ss_log("[pad] rumble routing: XInputSetState at %p changed while it was being redirected: nothing done", target);
    return false;
  }
  ss_log("[pad] rumble routing: xinput1_3!XInputSetState at %p now maps the device-list index to the XInput slot", target);
  return true;
}

bool g_started = false;

void StartOnce() {
  if (g_started) return;
  g_started = true;
  InitializeCriticalSection(&g_cs);
  LoadConfig();
  { const char* f = getenv("SS_PAD_FAKE"); g_fake = f && *f == '1'; }
  ss_log("[pad] controller layer %s (rumble %s gain %d%%, dead zone %d/%d%%, trigger %d%%, hot-plug %s, translate %s, foreground only %s)",
         g_cfg.on ? "on" : "off", g_cfg.rumble ? "on" : "off", g_cfg.rumble_gain, g_cfg.deadzone_left, g_cfg.deadzone_right,
         g_cfg.trigger_threshold, g_cfg.hotplug ? "on" : "off", g_cfg.translate ? "on" : "off", g_cfg.foreground_only ? "on" : "off");
  if (!g_cfg.on) return;
  BuildObjects();
  BuildRawFormat();
  if (LoadXInput()) HookXInputSetState();  // also with pad_rumble off: the hook then sends the motors zero
}

}  // namespace

// proxy.cpp calls this after the real DirectInput8Create succeeded; *inout is the real interface and is
// replaced with the wrapper. Returns S_OK when wrapped, S_FALSE when left alone.
long ss_pad_wrap_directinput(const void* riid, void** inout) {
  if (!ss_exe_known()) return S_FALSE;  // srccheck.cpp: not the Steam build, the patch is off
  StartOnce();
  if (!g_cfg.on || !inout || !*inout) return S_FALSE;
  if (!IsEqualIID(*static_cast<const IID*>(riid), IID_IDirectInput8A)) return S_FALSE;
  static bool split_once = false;
  if (!split_once) {
    split_once = true;
    SplitInstall();
  }
  *inout = static_cast<IDirectInput8A*>(new WrapDI(static_cast<IDirectInput8A*>(*inout)));
  ss_log("[pad] IDirectInput8A wrapped");
  return S_OK;
}

// intro.cpp: skip_intro taps A on the virtual pad #0, so it needs the layer on. Before the layer started this
// gives what its start will decide (the same rule as LoadConfig).
bool ss_pad_support_on() {
  if (g_started) return g_cfg.on;
  const char* env = getenv("SS_PAD");
  const char* fake = getenv("SS_PAD_FAKE");
  if (env) return ParseBool(env, true);
  if (fake && *fake == '1') return ParseBool(ss_conf("pad_support"), true);
  return ss_conf_present() && ParseBool(ss_conf("pad_support"), true);
}

// The settings menu reads the XInput pads itself (render thread), so opening and working it does not depend on the
// game polling its pads (it reads them less, or not at all, on some paused screens). Buttons ORed over the four
// slots, the stick with the largest deflection. False when the layer is off or no XInput module was found.
bool ss_pad_direct(unsigned short* buttons, float* lx, float* ly) {
  if (!g_started || !g_cfg.on || (!g_xi.get && !g_fake)) return false;
  if (g_cfg.foreground_only && OtherAppInFront()) return false;
  unsigned short b = 0;
  float x = 0, y = 0;
  bool any = false;
  for (int slot = 0; slot < 4; ++slot) {
    Pad p;
    if (!ReadXInput(slot, p)) continue;
    any = true;
    b |= p.buttons;
    if (fabsf(p.lx) > fabsf(x)) x = p.lx;
    if (fabsf(p.ly) > fabsf(y)) y = p.ly;
  }
  *buttons = b;
  *lx = x;
  *ly = y;
  return any;
}

// The settings menu's live rows of this layer (menu.cpp calls it after writing the value to splitsecond.conf).
// The keys read once at start (pad_support, pad_hotplug, pad_translate_dinput) keep their running values.
bool ss_pad_apply(const char* key, const char* value) {
  if (!g_started || strncmp(key, "pad_", 4) != 0) return false;
  const Config was = g_cfg;
  LoadConfig();
  g_cfg.on = was.on;
  g_cfg.hotplug = was.hotplug;
  g_cfg.translate = was.translate;
  g_cfg.split_triggers = was.split_triggers;
  // pad_rumble / pad_rumble_gain: the routing hook went in at start-up and reads g_cfg on every call.
  (void)value;
  return true;
}
