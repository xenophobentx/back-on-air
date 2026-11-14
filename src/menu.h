// The in-game settings overlay (menu.cpp): "PC SETTINGS", drawn by this DLL over the game's picture in the
// device's Present hook, opened with F1 or by holding a pad's Back/View button. Its texts, capitals and pages
// follow the port's own PC settings page; its rows are a registry other parts of the DLL add to.
#pragma once
#include <initializer_list>

// ------------------------------------------------------------------------------------------- the registry

// Pages in the order they are shown; a page without rows is left out. Which page a row is on, its label, its level
// and its help line come from the key table in menu.cpp (kKeys), not from the registration: a key missing from that
// table lands on DEVELOPER, which is shown only with `menu_developer = true`, so a test row never reaches players.
enum MenuPage {
  MENU_PAGE_DISPLAY,     // frame rate, screen sync, window, picture shape, slow motion
  MENU_PAGE_HDR,         // the hdr_* rows (only where the swap chain can do HDR10)
  MENU_PAGE_GRAPHICS,    // anti-aliasing, sharpening, texture filtering, distances, shadows
  MENU_PAGE_AUDIO,       // the main volumes, reverb, sound mix
  MENU_PAGE_CONTROLLER,  // vibration, dead zones, button icons
  MENU_PAGE_GAME,        // intro, main menu, save backups, the Options-screen prompt
  MENU_PAGE_ADVANCED,    // level 1: works, but a player rarely needs it
  MENU_PAGE_DEVELOPER,   // level 2: logs, tests, measuring switches (menu_developer = true)
  MENU_PAGE_COUNT
};

enum MenuActs {
  MENU_LIVE,     // the apply callback puts the value to work at once
  MENU_RESTART,  // saved only; read at the next start (the row says RESTART NEEDED while it differs from the running one)
  MENU_ACTION,   // no value: A / Enter calls `apply` (with an empty value); nothing is written to the file
};

constexpr int kMenuMaxChoices = 16;

struct MenuChoice {
  const char* value;  // the text in splitsecond.conf
  const char* label;  // what the row shows (capitals)
};

// Live rows: called after the value has been written to splitsecond.conf. True when it took effect.
using MenuApplyFn = bool (*)(const char* key, const char* value);

struct MenuRow {
  const char* key;       // splitsecond.conf key
  const char* label;     // capitals, e.g. "FRAME RATE" (menu.cpp's key table has the label shown)
  int page;              // MenuPage asked for (menu.cpp's key table decides; a key not there goes to DEVELOPER)
  MenuActs acts;
  const char* fallback;  // the value when the file has none (the key's default)
  MenuChoice choices[kMenuMaxChoices];  // up to the first null value; none at all: a number row
  int min, max, step;    // number rows
  const char* unit;      // number rows: shown after the number ("%"), or ""
  MenuApplyFn apply;     // live rows; null for restart rows
  const char* (*status)();  // action rows: the text shown in place of OPEN (progress, result), or null
};

// Adds a row, or replaces the row with the same key. Safe from static initialisers of other files
// (`static MenuRegistrar r(MenuChoiceRow(...));`) and at any time later; the menu shows rows in the order of its key
// table (menu.cpp, kKeys), then the others (on DEVELOPER) in the order they were registered.
void ss_menu_register(const MenuRow& row);

// A row with named choices.
MenuRow MenuChoiceRow(const char* key, const char* label, int page, MenuActs acts, const char* fallback,
                      std::initializer_list<MenuChoice> choices, MenuApplyFn apply);
// A row with a number from min to max in steps.
MenuRow MenuNumberRow(const char* key, const char* label, int page, MenuActs acts, const char* fallback, int min, int max,
                      int step, const char* unit, MenuApplyFn apply);

// A row without a value that does something when A / Enter is pressed on it. `status` (optional) gives the text on
// the right side, read every frame the row is drawn (e.g. a job's progress); without it the row says OPEN.
MenuRow MenuActionRow(const char* key, const char* label, int page, MenuApplyFn action, const char* (*status)() = nullptr);

struct MenuRegistrar {
  explicit MenuRegistrar(const MenuRow& row) { ss_menu_register(row); }
};

// ------------------------------------------------------------------------------------------- the hook-ups

// patch.cpp: the device's Present hook calls this first, before the real Present. Cheap while closed (one key and
// one pad check); draws the overlay while open.
void ss_menu_present(void* device);

// patch.cpp: whether the menu is wanted at all (splitsecond.conf exists and `menu` is not false).
bool ss_menu_wanted();

// pad.cpp: every state read of a presented pad (game list index 0..3, XInput button bits, left stick -1..1, up +).
void ss_menu_pad_state(int index, unsigned short buttons, float lx, float ly);
// pad.cpp: the XInput pads read directly (false: no pad layer / no XInput).
bool ss_pad_direct(unsigned short* buttons, float* lx, float* ly);
// pad.cpp: true while the game must see neutral input (menu open, or a button that worked it still held).
bool ss_menu_capturing();
// options_entry.cpp: asks for the overlay to open (the Y prompt on the game's Options screen). Any thread: it only
// sets a flag; the Present hook opens the overlay on its next call. `how` is a literal for the log line.
void ss_menu_request_open(const char* how);
// pad.cpp: every other device the game opens through the wrapper (keyboard, mouse, ...); returns the device the
// game gets: keyboards and mice wrapped so they report nothing while the menu captures, the rest unchanged.
void* ss_menu_wrap_device(void* device);

// hdr.cpp: what the overlay drew over the frame being presented. False: nothing. `panel`: its panel's rectangle in
// back buffer pixels (x0, y0, x1, y1); around it the picture is dimmed. `calibrating`: the HDR calibration screen
// (a black screen with words; the output pass draws the patterns).
bool ss_menu_cover(float panel[4], bool* calibrating);
// The CALIBRATE HDR row's action (hdr.cpp registers the row): opens the calibration screen while HDR is in use.
bool ss_menu_calibration(const char* key, const char* value);

// Live settings of the other parts (true when the key is theirs and was applied; false when the part cannot take it
// now, e.g. its hook was not put in at start: the row then says RESTART NEEDED).
// patch.cpp: takedown_realtime, slow_half_steps, frame_pacing, frame_log
bool ss_patch_apply(const char* key, const char* value);
// pad.cpp: pad_rumble, pad_rumble_gain, dead zones
bool ss_pad_apply(const char* key, const char* value);
// glyphs.cpp: button_prompts
bool ss_glyphs_apply(const char* key, const char* value);
// postfx.cpp: aa, sharpen (while the passes' hooks are in: either key on at start)
bool ss_postfx_apply(const char* key, const char* value);
// quality.cpp: anisotropy (while SetSamplerState is hooked: not game at start)
bool ss_quality_apply(const char* key, const char* value);
// audio.cpp: console_mix (while the getEvent wrappers are in: the key true or the overlay wanted at start)
bool ss_audio_apply(const char* key, const char* value);
// lodfade.cpp: lod_smoothing (while its device hooks are in: on at start)
bool ss_lodfade_apply(const char* key, const char* value);
// shadowsmooth.cpp: shadow_smoothing (while its SetPixelShader hook is in: on or high at start)
bool ss_shadowsmooth_apply(const char* key, const char* value);
// deband.cpp: deband (while its SetPixelShader hook is in: on at start)
bool ss_deband_apply(const char* key, const char* value);
// display.cpp: pause_on_focus_loss (while the window is subclassed: false at start)
bool ss_display_apply(const char* key, const char* value);
