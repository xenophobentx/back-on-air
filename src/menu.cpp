// The in-game settings overlay: "PC SETTINGS".
//
// The PC front end is Scaleform GFx movies driven by the game's code; new rows there would mean new movie art and
// code, so the page is drawn by this DLL instead, over the finished picture, from the device's Present hook (slot 17
// of the D3D9 device vtable, in d3d9.dll's memory; patch.cpp calls ss_menu_present first thing). Nothing of the game
// is written: the drawing uses the device the game hands to Present, d3dx9_42.dll's font (the game loads that DLL
// itself) and a state block that puts every device state back afterwards; Reset (slot 16, same vtable) is wrapped
// only to free this page's objects before the device is reset.
//
// Pages: DISPLAY, HDR, GRAPHICS, AUDIO, CONTROLLER, GAME, ADVANCED, and DEVELOPER only with `menu_developer = true`.
// Rows are a registry (menu.h); other parts of the DLL register theirs. Which page a row is on, its label and its
// help line come from the key table below (kKeys); a row whose key is not in it goes to DEVELOPER. A restart-only
// row says RESTART, and RESTART NEEDED once its saved value differs from the running one.
//
// Open / close: F1, or hold a pad's Back (View) button for 0.7 s. While open: up/down rows, left/right change the
// value and save it to splitsecond.conf at once (live keys take effect at once), A/Enter opens an action row, B/Esc
// closes, LB/RB or Q/E turn the page. Input reaches the menu from the pads the game reads (pad.cpp hands every state read to
// ss_menu_pad_state) and from the keyboard state (GetAsyncKeyState, only while the game's window is in front). While
// it is open, and after it closes until the buttons that worked it are let go, the game reads centred pads (pad.cpp)
// and a keyboard and mouse with nothing pressed: the mouse through the wrapper below (put around the game's DirectInput
// devices by pad.cpp's IDirectInput8A wrapper), the keyboard through the game's import of user32!GetKeyboardState (the
// IAT word 0xB9B2A8, read-only data, swapped at the first present): the game polls its keyboard with
// GetKeyboardState (0x40D2B0, observed in the code; it opens no DirectInput keyboard), so while the menu captures the
// state it gets has nothing pressed.
//
// splitsecond.conf is rewritten in place: the key's line(s) replaced, or the key appended; comments and every other
// line stay as they are.
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <dinput.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "menu.h"
#include "ss_shared.h"

// The mixer (src/mixer.h, another part of the DLL) gives the AUDIO page its volume rows when it is built in.
// Assumed interface: int ss_mixer_count(); const char* ss_mixer_key(int i) ("volume_master", ...);
// ss_mixer_get(i) -> percent 0..100; ss_mixer_set(i, percent).
#if __has_include("mixer.h")
#include "mixer.h"
#define SS_MENU_MIXER 1
#endif

// While this page or the anti-aliasing passes (postfx.cpp) draw on the game's device, the other device hooks of the
// DLL (postfx's EndScene, aspect's BeginScene, quality's SetSamplerState) pass the calls straight on: our own
// BeginScene / EndScene / sampler states are not the game's frame. Render thread only.
volatile long ss_own_drawing = 0;

// ------------------------------------------------------------------------------------------- registry

MenuRow MenuChoiceRow(const char* key, const char* label, int page, MenuActs acts, const char* fallback,
                      std::initializer_list<MenuChoice> choices, MenuApplyFn apply) {
  MenuRow r;
  memset(&r, 0, sizeof r);
  r.key = key;
  r.label = label;
  r.page = page;
  r.acts = acts;
  r.fallback = fallback;
  int n = 0;
  for (const MenuChoice& c : choices)
    if (n < kMenuMaxChoices) r.choices[n++] = c;
  r.unit = "";
  r.apply = apply;
  return r;
}

MenuRow MenuNumberRow(const char* key, const char* label, int page, MenuActs acts, const char* fallback, int min, int max,
                      int step, const char* unit, MenuApplyFn apply) {
  MenuRow r;
  memset(&r, 0, sizeof r);
  r.key = key;
  r.label = label;
  r.page = page;
  r.acts = acts;
  r.fallback = fallback;
  r.min = min;
  r.max = max;
  r.step = step > 0 ? step : 1;
  r.unit = unit ? unit : "";
  r.apply = apply;
  return r;
}

MenuRow MenuActionRow(const char* key, const char* label, int page, MenuApplyFn action, const char* (*status)()) {
  MenuRow r;
  memset(&r, 0, sizeof r);
  r.key = key;
  r.label = label;
  r.page = page;
  r.acts = MENU_ACTION;
  r.fallback = "";
  r.unit = "";
  r.apply = action;
  r.status = status;
  return r;
}

namespace {

SRWLOCK g_reg_lock = SRWLOCK_INIT;
volatile LONG g_reg_version = 0;

std::vector<MenuRow>& Registry() {
  static std::vector<MenuRow> rows;
  return rows;
}

void Register(const MenuRow& row, bool replace) {
  if (!row.key || !row.label) return;
  AcquireSRWLockExclusive(&g_reg_lock);
  std::vector<MenuRow>& rows = Registry();
  bool found = false;
  for (MenuRow& r : rows) {
    if (strcmp(r.key, row.key) == 0) {
      if (replace) r = row;
      found = true;
      break;
    }
  }
  if (!found) rows.push_back(row);
  ReleaseSRWLockExclusive(&g_reg_lock);
  InterlockedIncrement(&g_reg_version);
}

}  // namespace

void ss_menu_register(const MenuRow& row) { Register(row, true); }

namespace {

// --------------------------------------------------------------------------------- the key table

// Every row a player or a developer may see: its level, its page, the label shown and the help line drawn under the
// rows while it is focused. The table's order is the order rows are shown in, page by page. A registered row whose
// key is not here is shown on DEVELOPER (with its own label), so a test row added by a branch reaches players only
// when someone lists it here on purpose. Help lines are at most two lines of the help band (about 180 characters).
// Words used throughout: PC ORIGINAL = what the Steam release does; XBOX 360 = the console's value; RESTART = read at
// start only; RESTART NEEDED = changed, takes effect at the next start.
enum { LEVEL_PLAYER, LEVEL_ADVANCED, LEVEL_DEVELOPER };

struct KeyInfo {
  const char* key;
  int level;  // LEVEL_PLAYER rows are on `page`; LEVEL_ADVANCED on ADVANCED, LEVEL_DEVELOPER on DEVELOPER
  int page;   // the player page; for the other levels the page the row belongs to (ADVANCED keeps that order)
  const char* label;
  const char* help;
};

const KeyInfo kKeys[] = {
    // DISPLAY
    {"fps", LEVEL_PLAYER, MENU_PAGE_DISPLAY, "FRAME RATE",
     "How many pictures a second the game runs at. 30 is what the PC release does. 120 needs a screen of 120 Hz or more."},
    {"frame_pacing", LEVEL_PLAYER, MENU_PAGE_DISPLAY, "SCREEN SYNC", "How finished frames are handed to the screen."},
    {"window_mode", LEVEL_PLAYER, MENU_PAGE_DISPLAY, "WINDOW MODE",
     "Fullscreen, a borderless window that fills the screen, or a normal window. Borderless makes Alt+Tab quick."},
    // Registered by renderer.cpp only on Windows with dxvk\d3d9.dll there and no d3d9.dll beside the exe.
    {"renderer", LEVEL_PLAYER, MENU_PAGE_DISPLAY, "RENDERER",
     "Which Direct3D 9 the game draws with. Takes effect at the next start."},
    {"pause_on_focus_loss", LEVEL_PLAYER, MENU_PAGE_DISPLAY, "PAUSE IN BACKGROUND",
     "Pauses the game when you switch to another window."},
    {"aspect", LEVEL_PLAYER, MENU_PAGE_DISPLAY, "ASPECT RATIO",
     "Fits the picture to wide screens. Auto uses your screen's shape. 16:9 is what the PC release does: wide screens "
     "lose the top and bottom."},
    {"hud_width", LEVEL_PLAYER, MENU_PAGE_DISPLAY, "HUD WIDTH",
     "How wide the HUD and menus are drawn on a wide screen. 16:9 keeps them undistorted in the middle. "
     "16:9 also on 16:10 adds bars above and below on a 16:10 screen."},
    {"fov", LEVEL_PLAYER, MENU_PAGE_DISPLAY, "FIELD OF VIEW", "How much of the track you see. 100% is the game's own view."},
    {"slow_half_steps", LEVEL_PLAYER, MENU_PAGE_DISPLAY, "SLOW MOTION",
     "How crash and takedown slow motion is drawn at 60 fps. Smooth shows 60 different pictures a second."},
    // HDR (registered by hdr.cpp only where the swap chain can do HDR10)
    {"hdr", LEVEL_PLAYER, MENU_PAGE_HDR, "HDR",
     "Shows sun glare, fire and lamps brighter than white on an HDR screen. Switch HDR on for the screen first."},
    {"hdr_calibrate", LEVEL_PLAYER, MENU_PAGE_HDR, "CALIBRATE HDR",
     "Opens two test patterns to set peak brightness and game brightness for your screen. Needs HDR on."},
    {"hdr_peak", LEVEL_PLAYER, MENU_PAGE_HDR, "PEAK BRIGHTNESS", "The brightest your screen can show. Auto asks the screen."},
    {"hdr_paper_white", LEVEL_PLAYER, MENU_PAGE_HDR, "GAME BRIGHTNESS", "How bright the normal picture is. Raise it in a bright room."},
    {"hdr_hud", LEVEL_PLAYER, MENU_PAGE_HDR, "HUD BRIGHTNESS", "How bright the HUD and menus are. Auto is a little under game brightness."},
    {"hdr_highlights", LEVEL_PLAYER, MENU_PAGE_HDR, "HIGHLIGHTS", "How far bright things go above white."},
    {"hdr_lift", LEVEL_PLAYER, MENU_PAGE_HDR, "LIGHT SOURCES", "Extra brightness for lamps, signs, fire and sparks."},
    {"hdr_shadows", LEVEL_PLAYER, MENU_PAGE_HDR, "DEEPER DARKS", "Makes dark areas darker. Off is the game's own look."},
    // GRAPHICS
    {"aa", LEVEL_PLAYER, MENU_PAGE_GRAPHICS, "ANTI-ALIASING", "Smooths jagged edges. SMAA is the safe choice."},
    {"sharpen", LEVEL_PLAYER, MENU_PAGE_GRAPHICS, "SHARPENING", "Sharpens the picture. 25% goes well with anti-aliasing."},
    {"anisotropy", LEVEL_PLAYER, MENU_PAGE_GRAPHICS, "TEXTURE FILTERING",
     "Keeps the road and walls sharp at a distance. 16x costs almost nothing."},
    {"draw_distance", LEVEL_PLAYER, MENU_PAGE_GRAPHICS, "DRAW DISTANCE", "How far away scenery and other cars are drawn."},
    {"shadow_distance", LEVEL_PLAYER, MENU_PAGE_GRAPHICS, "SHADOW DISTANCE", "How far away shadows are drawn."},
    {"shadow_smoothing", LEVEL_PLAYER, MENU_PAGE_GRAPHICS, "SHADOW BLENDING",
     "Hides the line in front of the car where sharp shadows turn into soft ones."},
    {"lod_smoothing", LEVEL_PLAYER, MENU_PAGE_GRAPHICS, "POP-IN FADE", "Fades scenery in and out instead of letting it pop."},
    {"opponent_cars", LEVEL_PLAYER, MENU_PAGE_GRAPHICS, "OPPONENT CAR DETAIL", "How detailed the other cars are at a distance."},
    {"deband", LEVEL_PLAYER, MENU_PAGE_GRAPHICS, "SMOOTH GRADIENTS", "Removes the visible steps in dark skies, fog and glow."},
    // pipewarm.cpp registers these two only with DXVK and the record files
    {"pipeline_warmup", LEVEL_PLAYER, MENU_PAGE_GRAPHICS, "PRE-COMPILE",
     "Builds the track's graphics while it loads: no stutter in the first race. The first load of each track takes "
     "about 30 s. Fewer compiler threads with it on: changes to that wait for a restart."},
    {"pipeline_compile_all", LEVEL_PLAYER, MENU_PAGE_GRAPHICS, "PRE-COMPILE ALL TRACKS",
     "Builds every track's graphics now, from the main menu, so no track's first load is long. Takes a few minutes. "
     "Press again to stop."},
    // AUDIO
    {"volume_master", LEVEL_PLAYER, MENU_PAGE_AUDIO, "MASTER VOLUME", "Everything. 100% is the game's own level."},
    {"volume_music", LEVEL_PLAYER, MENU_PAGE_AUDIO, "MUSIC VOLUME", "The music."},
    {"volume_sfx", LEVEL_PLAYER, MENU_PAGE_AUDIO, "EFFECTS VOLUME", "All sounds except music: engines, crashes, voice."},
    {"volume_engines", LEVEL_PLAYER, MENU_PAGE_AUDIO, "ENGINES VOLUME", "Your engine and the other cars' engines."},
    {"volume_voice", LEVEL_PLAYER, MENU_PAGE_AUDIO, "VOICE VOLUME", "The announcer."},
    {"volume_ambience", LEVEL_PLAYER, MENU_PAGE_AUDIO, "AMBIENCE VOLUME", "Background sound of the tracks."},
    {"reverb", LEVEL_PLAYER, MENU_PAGE_AUDIO, "TRACK REVERB",
     "Echo in tunnels, hangars and halls, as on the console. The PC release has one small room everywhere."},
    {"reverb_level", LEVEL_PLAYER, MENU_PAGE_AUDIO, "REVERB AMOUNT", "How strong the echo is. 100% is as authored."},
    {"console_mix", LEVEL_PLAYER, MENU_PAGE_AUDIO, "SOUND MIX",
     "Uses the console's levels for tyres, crashes and HUD sounds. Changes with each new sound."},
    // CONTROLLER
    {"pad_rumble", LEVEL_PLAYER, MENU_PAGE_CONTROLLER, "VIBRATION", "Controller vibration."},
    {"pad_rumble_gain", LEVEL_PLAYER, MENU_PAGE_CONTROLLER, "VIBRATION STRENGTH", "100% is what the game asks for."},
    {"pad_deadzone_left", LEVEL_PLAYER, MENU_PAGE_CONTROLLER, "LEFT STICK DEAD ZONE",
     "How far the stick moves before the game reacts. Raise it if the car steers by itself."},
    {"pad_deadzone_right", LEVEL_PLAYER, MENU_PAGE_CONTROLLER, "RIGHT STICK DEAD ZONE", "The same for the camera stick."},
    {"pad_trigger_threshold", LEVEL_PLAYER, MENU_PAGE_CONTROLLER, "TRIGGER DEAD ZONE",
     "How far a trigger is pressed before the game reacts."},
    {"button_prompts", LEVEL_PLAYER, MENU_PAGE_CONTROLLER, "BUTTON ICONS",
     "Which controller's buttons the game shows. Auto follows the controller in use."},
    {"pad_hotplug", LEVEL_PLAYER, MENU_PAGE_CONTROLLER, "CONTROLLERS CONNECTED LATER",
     "On: a controller switched on after the game started works. The game then lists four controllers even when "
     "fewer are there."},
    // GAME
    {"skip_intro", LEVEL_PLAYER, MENU_PAGE_GAME, "SKIP INTRO MOVIES", "Skips the logos and the intro film when the game starts."},
    {"boot_to_menu", LEVEL_PLAYER, MENU_PAGE_GAME, "SKIP PRESS START", "Goes straight to the main menu when the game starts."},
    {"save_backups", LEVEL_PLAYER, MENU_PAGE_GAME, "SAVE BACKUPS",
     "Copies your save at every start into the save-backups folder beside the game. This many copies are kept."},
    {"online_autologin", LEVEL_PLAYER, MENU_PAGE_GAME, "ONLINE SIGN-IN",
     "Automatic: online play signs in by itself with your Steam name. Nothing to type."},
    {"online_name", LEVEL_PLAYER, MENU_PAGE_GAME, "ONLINE NAME",
     "The name other players see. To use another name, write online_name = <name> in splitsecond.conf."},
    {"options_prompt", LEVEL_PLAYER, MENU_PAGE_GAME, "PC SETTINGS BUTTON IN OPTIONS",
     "Shows a Y prompt on the game's Options screen that opens this menu."},
    // ADVANCED
    {"smaa_predication", LEVEL_ADVANCED, MENU_PAGE_GRAPHICS, "SMAA: USE DEPTH",
     "SMAA finds edges from depth too: cleaner car and fence edges. Only matters with SMAA. Leave on."},
    {"shadow_softness", LEVEL_ADVANCED, MENU_PAGE_GRAPHICS, "SHADOW SOFTNESS",
     "How soft the edges of sun shadows are. Softest can show stripes on slopes."},
    {"slipstream_fix", LEVEL_ADVANCED, MENU_PAGE_GRAPHICS, "SLIPSTREAM EFFECT",
     "The white streaks at the car's front while you draft another car. Missing in the PC release."},
    {"audio_voice_dsp", LEVEL_ADVANCED, MENU_PAGE_AUDIO, "3D SOUND POSITIONS", "Places sounds around you as the console does. Leave on."},
    {"audio_speaker_fix", LEVEL_ADVANCED, MENU_PAGE_AUDIO, "SURROUND LEVELS FIX",
     "Stops menu and HUD sounds playing at full level from every speaker. Leave on."},
    {"audio_sidechain", LEVEL_ADVANCED, MENU_PAGE_AUDIO, "AUTO DUCKING",
     "Lowers music and engines for a moment under crashes and power plays, as on the console."},
    {"volume_player_engine", LEVEL_ADVANCED, MENU_PAGE_AUDIO, "YOUR ENGINE", nullptr},
    {"volume_opponent_engines", LEVEL_ADVANCED, MENU_PAGE_AUDIO, "OPPONENT ENGINES", nullptr},
    {"volume_collisions", LEVEL_ADVANCED, MENU_PAGE_AUDIO, "CRASHES", nullptr},
    {"volume_powerplays", LEVEL_ADVANCED, MENU_PAGE_AUDIO, "POWER PLAYS", nullptr},
    {"volume_swooshes", LEVEL_ADVANCED, MENU_PAGE_AUDIO, "PASS-BY WHOOSHES", nullptr},
    {"volume_surfaces", LEVEL_ADVANCED, MENU_PAGE_AUDIO, "TYRES AND ROAD", nullptr},
    {"volume_particles", LEVEL_ADVANCED, MENU_PAGE_AUDIO, "DEBRIS", nullptr},
    {"volume_hyperreal", LEVEL_ADVANCED, MENU_PAGE_AUDIO, "SLOW-MOTION EFFECTS", nullptr},
    {"volume_camera", LEVEL_ADVANCED, MENU_PAGE_AUDIO, "CAMERA EFFECTS", nullptr},
    {"volume_hud", LEVEL_ADVANCED, MENU_PAGE_AUDIO, "MENU AND HUD SOUNDS", nullptr},
    {"volume_general_ambience", LEVEL_ADVANCED, MENU_PAGE_AUDIO, "TRACK AMBIENCE", nullptr},
    {"volume_spot_ambience", LEVEL_ADVANCED, MENU_PAGE_AUDIO, "SPOT AMBIENCE", nullptr},
    {"pad_support", LEVEL_ADVANCED, MENU_PAGE_CONTROLLER, "CONTROLLER LAYER",
     "Off: the game reads controllers as the PC release does, and this menu opens with F1 only. Leave on."},
    {"pad_translate_dinput", LEVEL_ADVANCED, MENU_PAGE_CONTROLLER, "OLDER CONTROLLERS AS XBOX",
     "Makes controllers without an Xbox mode (older Sony, Nintendo, generic) work as an Xbox controller."},
    {"pad_foreground_only", LEVEL_ADVANCED, MENU_PAGE_CONTROLLER, "IGNORE CONTROLLER IN BACKGROUND",
     "The game ignores the controller while another window is in front."},
    // DEVELOPER (menu_developer = true): logs, tests and the 120 fps work's measuring switches
    {"frame_log", LEVEL_DEVELOPER, MENU_PAGE_DISPLAY, "FRAME LOG", "frame_log: a [frames] line every 10 s in splitsecond-patch.log."},
    {"display_log", LEVEL_DEVELOPER, MENU_PAGE_DISPLAY, "DISPLAY LOG", "display_log: window and device events in the log."},
    {"audio_diag", LEVEL_DEVELOPER, MENU_PAGE_AUDIO, "AUDIO LOG", "audio_diag: the audio part's diagnostic lines."},
    {"pad_log", LEVEL_DEVELOPER, MENU_PAGE_CONTROLLER, "LOG BUTTONS", "pad_log: button changes in the log (capped)."},
    {"takedown_realtime", LEVEL_DEVELOPER, MENU_PAGE_DISPLAY, "SLOW MOTION AT 60",
     "takedown_realtime: double speed is the PC release's bug at 60 fps, for comparing only."},
    {"hfr_ticks", LEVEL_DEVELOPER, MENU_PAGE_DISPLAY, "SCRIPTED SEQUENCES", "hfr_ticks: above 60 fps. Per tick is the fault being fixed."},
    {"hfr_rewind", LEVEL_DEVELOPER, MENU_PAGE_DISPLAY, "REWIND ABOVE 60", "hfr_rewind: the rewind rings above 60 fps."},
    {"hfr_steering", LEVEL_DEVELOPER, MENU_PAGE_DISPLAY, "STEERING ABOVE 60", "hfr_steering: steering response above 60 fps."},
    {"hdr_lift_test", LEVEL_DEVELOPER, MENU_PAGE_HDR, "LIGHT SOURCES TEST", "hdr_lift_test: which light sources the lift reaches."},
};

const KeyInfo* FindKey(const char* key) {
  for (const KeyInfo& k : kKeys)
    if (!strcmp(k.key, key)) return &k;
  return nullptr;
}

// Values with a name or a help line of their own: per-value help (shown while that value is on the row), the name of
// a value that is set in splitsecond.conf but is not one the row steps through (label set, it is not offered), and
// number rows' special values. Null label: the row's own name for it.
struct ValueInfo {
  const char* key;
  const char* value;
  const char* label;
  const char* help;
};

const ValueInfo kValues[] = {
    {"fps", "native", "NATIVE (CONF)", "Set in splitsecond.conf: the screen's own refresh rate."},
    {"renderer", "dxvk", nullptr,
     "DXVK (Vulkan): holds 120 fps and gives HDR. Each track stutters a little the first time it is driven."},
    {"renderer", "native", nullptr,
     "Windows' own Direct3D 9: no stutter in the race, a short pause at the start. About 110 fps at 120, no HDR."},
    {"frame_pacing", "auto", nullptr, "Picks the best choice for this machine: Smooth for now, on every screen."},
    {"frame_pacing", "smooth", nullptr,
     "Every frame waits for the screen: no tearing, about one frame of delay. On a 90 or 144 Hz screen set it to 60 or 120 Hz."},
    {"frame_pacing", "fastest", nullptr, "Does not wait for the screen. Lowest input delay, but the picture can tear."},
    {"frame_pacing", "vrr", "VARIABLE REFRESH (CONF)",
     "Set in splitsecond.conf: for G-Sync and FreeSync screens. Frames are shown as they are ready, a little under the screen's top rate."},
    {"frame_pacing", "present", "EVEN GRID (CONF)",
     "Set in splitsecond.conf: frames on an even grid; the game's own vsync option decides about tearing."},
    {"frame_pacing", "step", "STEP ONLY (CONF)", "Set in splitsecond.conf: the old frame wait only. Uneven frame times."},
    {"aa", "off", nullptr, "No smoothing of jagged edges: the PC release's look."},
    {"aa", "fxaa", nullptr, "Smooths edges cheaply, a little soft."},
    {"aa", "smaa", nullptr, "Sharp, clean edges. The safe choice."},
    {"aa", "taa", nullptr, "Also calms shimmer on fences and wires, but can smear in motion."},
    {"aa", "taa_high", nullptr, "TAA after SMAA: cleaner where cars pass and places come into view. Can smear in motion."},
    {"lod_smoothing", "high", nullptr, "Fades scenery in and out over a longer time instead of letting it pop."},
    {"save_backups", "0", "OFF", "No copies of your save are made."},
};

const ValueInfo* FindValue(const char* key, const std::string& value) {
  for (const ValueInfo& v : kValues)
    if (!strcmp(v.key, key) && value == v.value) return &v;
  return nullptr;
}

// Older spellings of a value, read as the value the row offers (written back only when the row is changed).
struct ValueAlias {
  const char* key;
  const char* from;
  const char* to;
};
const ValueAlias kAliases[] = {
    {"frame_pacing", "vsync", "smooth"},
    {"frame_pacing", "fast", "fastest"},
    {"pipeline_warmup", "replay", "on"},
    {"pipeline_warmup", "auto", "on"},
    {"pipeline_warmup", "true", "on"},
    {"pipeline_warmup", "false", "off"},
};

const char* const kPageNames[MENU_PAGE_COUNT] = {"DISPLAY", "HDR", "GRAPHICS", "AUDIO", "CONTROLLER", "GAME", "ADVANCED", "DEVELOPER"};

// --------------------------------------------------------------------------------- the built-in rows

// The rows' values and how they act; labels, pages and help lines are the key table's (above).
void RegisterBuiltins() {
  const MenuChoice off_on[] = {{"false", "OFF"}, {"true", "ON"}};
  auto onoff = [&](const char* key, const char* label, int page, MenuActs acts, const char* fallback, MenuApplyFn fn) {
    Register(MenuChoiceRow(key, label, page, acts, fallback, {off_on[0], off_on[1]}, fn), false);
  };
  // DISPLAY
  // Read at start (the rewind rings hold ticks of one length per race). fps = native and 61..240 are conf-only.
  Register(MenuChoiceRow("fps", "FRAME RATE", MENU_PAGE_DISPLAY, MENU_RESTART, "30",
                         {{"30", "30 FPS (PC ORIGINAL)"}, {"60", "60 FPS"}, {"120", "120 FPS"}}, nullptr),
           false);
  // The device's presentation interval is made at start: every change waits for the next start. vrr, present and
  // step are conf-only (kValues names them when set).
  Register(MenuChoiceRow("frame_pacing", "SCREEN SYNC", MENU_PAGE_DISPLAY, MENU_RESTART, "auto",
                         {{"auto", "AUTO (SMOOTH)"}, {"smooth", "SMOOTH"}, {"fastest", "FASTEST"}}, nullptr),
           false);
  Register(MenuChoiceRow("window_mode", "WINDOW MODE", MENU_PAGE_DISPLAY, MENU_RESTART, "fullscreen",
                         {{"fullscreen", "FULLSCREEN"}, {"borderless", "BORDERLESS"}, {"windowed", "WINDOWED"}}, nullptr),
           false);
  // Live only while display.cpp's window subclass is in (the key was false at start); else RESTART NEEDED.
  onoff("pause_on_focus_loss", "PAUSE IN BACKGROUND", MENU_PAGE_DISPLAY, MENU_LIVE, "true", ss_display_apply);
  // aspect.cpp puts its hooks in only when the screen's shape asks for it, so these are read at start only.
  Register(MenuChoiceRow("aspect", "ASPECT RATIO", MENU_PAGE_DISPLAY, MENU_RESTART, "auto",
                         {{"auto", "AUTO"}, {"16:9", "16:9 (PC ORIGINAL)"}, {"16:10", "16:10"}, {"21:9", "21:9"}, {"32:9", "32:9"}}, nullptr),
           false);
  Register(MenuChoiceRow("hud_width", "HUD WIDTH", MENU_PAGE_DISPLAY, MENU_RESTART, "16:9",
                         {{"16:9", "16:9"}, {"21:9", "21:9"}, {"full", "STRETCHED"}, {"letterbox", "16:9 ALSO ON 16:10"}}, nullptr),
           false);
  Register(MenuNumberRow("fov", "FIELD OF VIEW", MENU_PAGE_DISPLAY, MENU_RESTART, "100", 50, 150, 5, "%", nullptr), false);
  Register(MenuChoiceRow("slow_half_steps", "SLOW MOTION", MENU_PAGE_DISPLAY, MENU_LIVE, "true",
                         {{"true", "SMOOTH"}, {"playback", "SMOOTH, ALSO REPLAYS"}, {"false", "30 FPS"}}, ss_patch_apply),
           false);
  // GRAPHICS
  // Live only while postfx.cpp's hooks are in (aa or sharpen on at start, or the HDR hooks: hdr_ready); else RESTART NEEDED.
  Register(MenuChoiceRow("aa", "ANTI-ALIASING", MENU_PAGE_GRAPHICS, MENU_LIVE, "smaa",
                         {{"off", "OFF"}, {"fxaa", "FXAA (FAST)"}, {"smaa", "SMAA (SHARP)"}, {"taa", "TAA"},
                          {"taa_high", "TAA + SMAA"}},
                         ss_postfx_apply),
           false);
  Register(MenuChoiceRow("sharpen", "SHARPENING", MENU_PAGE_GRAPHICS, MENU_LIVE, "0",
                         {{"0", "OFF"}, {"0.1", "10%"}, {"0.2", "20%"}, {"0.25", "25%"}, {"0.3", "30%"}, {"0.4", "40%"},
                          {"0.5", "50%"}, {"0.6", "60%"}, {"0.75", "75%"}, {"1", "100%"}}, ss_postfx_apply),
           false);
  // Live only while quality.cpp's SetSamplerState hook is in (not game at start); else RESTART NEEDED.
  Register(MenuChoiceRow("anisotropy", "TEXTURE FILTERING", MENU_PAGE_GRAPHICS, MENU_LIVE, "game",
                         {{"game", "PC ORIGINAL"}, {"off", "OFF"}, {"2", "2X"}, {"4", "4X"}, {"8", "8X"}, {"16", "16X"}}, ss_quality_apply),
           false);
  Register(MenuChoiceRow("draw_distance", "DRAW DISTANCE", MENU_PAGE_GRAPHICS, MENU_RESTART, "console",
                         {{"console", "NORMAL"}, {"1.5", "FAR (1.5X)"}, {"2", "VERY FAR (2X)"}}, nullptr),
           false);
  Register(MenuChoiceRow("shadow_distance", "SHADOW DISTANCE", MENU_PAGE_GRAPHICS, MENU_RESTART, "console",
                         {{"console", "NORMAL"}, {"1.5", "FAR (1.5X)"}, {"2", "VERY FAR (2X)"}}, nullptr),
           false);
  // Live only while shadowsmooth.cpp's SetPixelShader hook is in (on or high at start); else RESTART NEEDED.
  Register(MenuChoiceRow("shadow_smoothing", "SHADOW BLENDING", MENU_PAGE_GRAPHICS, MENU_LIVE, "high",
                         {{"off", "OFF"}, {"on", "ON"}, {"high", "WIDE"}}, ss_shadowsmooth_apply),
           false);
  // Live only while lodfade.cpp's device hooks are in (on or high at start); else RESTART NEEDED.
  Register(MenuChoiceRow("lod_smoothing", "POP-IN FADE", MENU_PAGE_GRAPHICS, MENU_LIVE, "on",
                         {{"off", "OFF"}, {"on", "ON"}, {"high", "LONG"}}, ss_lodfade_apply),
           false);
  Register(MenuChoiceRow("opponent_cars", "OPPONENT CAR DETAIL", MENU_PAGE_GRAPHICS, MENU_RESTART, "console",
                         {{"pc", "LOW (PC ORIGINAL)"}, {"console", "NORMAL (XBOX 360)"}, {"high", "HIGH"}}, nullptr),
           false);
  // Live only while deband.cpp's SetPixelShader hook is in (on at start); else RESTART NEEDED.
  Register(MenuChoiceRow("deband", "SMOOTH GRADIENTS", MENU_PAGE_GRAPHICS, MENU_LIVE, "on", {{"off", "OFF"}, {"on", "ON"}}, ss_deband_apply),
           false);
  // SMAA's edge pass also reads the game's depth (postfx.cpp).
  onoff("smaa_predication", "SMAA: USE DEPTH", MENU_PAGE_ADVANCED, MENU_LIVE, "true", ss_postfx_apply);
  // The sun shadow filter's radius (shadowsmooth.cpp, a constant of the same variants): live while that hook is in.
  Register(MenuChoiceRow("shadow_softness", "SHADOW SOFTNESS", MENU_PAGE_ADVANCED, MENU_LIVE, "console",
                         {{"0.75", "SHARPER"}, {"console", "NORMAL"}, {"1.25", "SOFTER"}, {"1.5", "SOFTEST"}}, ss_shadowsmooth_apply),
           false);
  // AUDIO (the volume rows come from the mixer, RegisterMixerRows)
  Register(MenuChoiceRow("reverb", "TRACK REVERB", MENU_PAGE_AUDIO, MENU_RESTART, "true",
                         {{"true", "XBOX 360"}, {"false", "PC ORIGINAL"}}, nullptr),
           false);
  // A trim on the reverb's level (the presets' Room value); 100 = as authored (audio.cpp takes 0..100).
  Register(MenuNumberRow("reverb_level", "REVERB AMOUNT", MENU_PAGE_AUDIO, MENU_LIVE, "100", 0, 100, 5, "%", ss_audio_apply), false);
  // PC ORIGINAL: the PC release's event values; XBOX 360: the console's (console_mix_table.h). Takes effect as events
  // are next fetched (each new sound), so a sound already playing keeps its level.
  Register(MenuChoiceRow("console_mix", "SOUND MIX", MENU_PAGE_AUDIO, MENU_LIVE, "true",
                         {{"false", "PC ORIGINAL"}, {"true", "XBOX 360"}}, ss_audio_apply),
           false);
  onoff("audio_voice_dsp", "3D SOUND POSITIONS", MENU_PAGE_ADVANCED, MENU_RESTART, "true", nullptr);
  onoff("audio_speaker_fix", "SURROUND LEVELS FIX", MENU_PAGE_ADVANCED, MENU_RESTART, "true", nullptr);
  onoff("audio_sidechain", "AUTO DUCKING", MENU_PAGE_ADVANCED, MENU_RESTART, "true", nullptr);
  // CONTROLLER
  onoff("pad_rumble", "VIBRATION", MENU_PAGE_CONTROLLER, MENU_LIVE, "true", ss_pad_apply);
  Register(MenuNumberRow("pad_rumble_gain", "VIBRATION STRENGTH", MENU_PAGE_CONTROLLER, MENU_LIVE, "100", 0, 200, 10, "%", ss_pad_apply),
           false);
  Register(MenuNumberRow("pad_deadzone_left", "LEFT STICK DEAD ZONE", MENU_PAGE_CONTROLLER, MENU_LIVE, "6", 0, 50, 1, "%", ss_pad_apply),
           false);
  Register(MenuNumberRow("pad_deadzone_right", "RIGHT STICK DEAD ZONE", MENU_PAGE_CONTROLLER, MENU_LIVE, "6", 0, 50, 1, "%", ss_pad_apply),
           false);
  Register(MenuNumberRow("pad_trigger_threshold", "TRIGGER DEAD ZONE", MENU_PAGE_CONTROLLER, MENU_LIVE, "5", 0, 50, 1, "%", ss_pad_apply),
           false);
  Register(MenuChoiceRow("button_prompts", "BUTTON ICONS", MENU_PAGE_CONTROLLER, MENU_LIVE, "auto",
                         {{"auto", "AUTO"}, {"xbox360", "XBOX 360"}, {"xbox", "XBOX"}, {"playstation", "PLAYSTATION"},
                          {"nintendo", "NINTENDO"}, {"steam", "STEAM"}, {"keyboard", "KEYBOARD"}},
                         ss_glyphs_apply),
           false);
  onoff("pad_hotplug", "CONTROLLERS CONNECTED LATER", MENU_PAGE_CONTROLLER, MENU_RESTART, "true", nullptr);
  onoff("pad_support", "CONTROLLER LAYER", MENU_PAGE_ADVANCED, MENU_RESTART, "true", nullptr);
  onoff("pad_translate_dinput", "OLDER CONTROLLERS AS XBOX", MENU_PAGE_ADVANCED, MENU_RESTART, "true", nullptr);
  onoff("pad_foreground_only", "IGNORE CONTROLLER IN BACKGROUND", MENU_PAGE_ADVANCED, MENU_LIVE, "false", ss_pad_apply);
  // GAME
  onoff("skip_intro", "SKIP INTRO MOVIES", MENU_PAGE_GAME, MENU_RESTART, "true", nullptr);
  onoff("boot_to_menu", "SKIP PRESS START", MENU_PAGE_GAME, MENU_RESTART, "true", nullptr);
  // saves.cpp: how many dated copies of the save folder are kept beside the game (0 = off); the copy is made at start.
  Register(MenuNumberRow("save_backups", "SAVE BACKUPS", MENU_PAGE_GAME, MENU_RESTART, "10", 0, 50, 1, "", nullptr), false);
  // DEVELOPER: logs, the 120 fps work's switches (docs/plan-120fps.md), the PC release's slow-motion bug.
  onoff("frame_log", "FRAME LOG", MENU_PAGE_DEVELOPER, MENU_LIVE, "false", ss_patch_apply);
  onoff("display_log", "DISPLAY LOG", MENU_PAGE_DEVELOPER, MENU_RESTART, "false", nullptr);
  onoff("audio_diag", "AUDIO LOG", MENU_PAGE_DEVELOPER, MENU_RESTART, "false", nullptr);
  onoff("pad_log", "LOG BUTTONS", MENU_PAGE_DEVELOPER, MENU_LIVE, "false", ss_pad_apply);
  Register(MenuChoiceRow("takedown_realtime", "SLOW MOTION AT 60", MENU_PAGE_DEVELOPER, MENU_LIVE, "true",
                         {{"true", "REAL SPEED"}, {"false", "DOUBLE SPEED"}}, ss_patch_apply),
           false);
  Register(MenuChoiceRow("hfr_ticks", "SCRIPTED SEQUENCES", MENU_PAGE_DEVELOPER, MENU_RESTART, "sixty",
                         {{"sixty", "BY GAME TIME"}, {"raw", "PER TICK"}}, nullptr),
           false);
  Register(MenuChoiceRow("hfr_rewind", "REWIND ABOVE 60", MENU_PAGE_DEVELOPER, MENU_RESTART, "capped",
                         {{"capped", "SAME LENGTH"}, {"off", "SHORTER"}}, nullptr),
           false);
  Register(MenuChoiceRow("hfr_steering", "STEERING ABOVE 60", MENU_PAGE_DEVELOPER, MENU_RESTART, "scaled",
                         {{"scaled", "AS AT 60"}, {"off", "WEAKER"}}, nullptr),
           false);
}

#ifdef SS_MENU_MIXER
std::vector<std::string> g_mixer_keys, g_mixer_labels;

bool MixerApply(const char* key, const char* value) {
  for (int i = 0, n = static_cast<int>(ss_mixer_count()); i < n; ++i) {
    const char* k = ss_mixer_key(i);
    if (k && strcmp(k, key) == 0) {
      ss_mixer_set(i, atoi(value));
      return true;
    }
  }
  return false;
}

// One row per mixer group; the key table names and places them. The menu offers 0..120 % (above 100 % the sound may
// clip, HelpText says so); splitsecond.conf still takes 0..200 % (mixer.h), and a conf value above 120 is shown as
// it is and kept until stepped down (Step).
void RegisterMixerRows() {
  const int n = static_cast<int>(ss_mixer_count());
  if (n <= 0 || !g_mixer_keys.empty()) return;
  g_mixer_keys.reserve(n);  // the rows keep pointers into these strings
  g_mixer_labels.reserve(n);
  for (int i = 0; i < n; ++i) {
    const char* k = ss_mixer_key(i);
    if (!k) continue;
    std::string label = k;
    if (label.compare(0, 7, "volume_") == 0) label = label.substr(7);
    for (char& c : label) c = (c == '_') ? ' ' : static_cast<char>(toupper(static_cast<unsigned char>(c)));
    label += " VOLUME";
    g_mixer_keys.push_back(k);
    g_mixer_labels.push_back(label);
  }
  for (size_t i = 0; i < g_mixer_keys.size(); ++i)
    Register(MenuNumberRow(g_mixer_keys[i].c_str(), g_mixer_labels[i].c_str(), MENU_PAGE_AUDIO, MENU_LIVE, "100", 0, 120, 5, "%",
                           MixerApply),
             false);
}
#endif

// ------------------------------------------------------------------------------------------- conf file

std::string ConfPath() {
  HMODULE self = nullptr;
  char path[MAX_PATH] = "";
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<const char*>(&ConfPath), &self);
  GetModuleFileNameA(self, path, MAX_PATH);
  char* slash = strrchr(path, '\\');
  if (slash) slash[1] = 0;
  return std::string(path) + "splitsecond.conf";
}

// The key of a `key = value` line, "" for comments and other lines (the same reading as patch.cpp's ConfValue).
std::string LineKey(const std::string& line) {
  size_t p = 0;
  while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
  if (p >= line.size() || line[p] == '#' || line[p] == ';') return "";
  size_t eq = line.find('=', p);
  if (eq == std::string::npos) return "";
  std::string k = line.substr(p, eq - p);
  while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
  return k;
}

// Sets `key = value` in splitsecond.conf: every line of the key is replaced, or the key is appended; the rest of
// the file is kept byte for byte. Written to a temporary file next to it, then moved over it.
bool WriteConf(const char* key, const std::string& value) {
  const std::string path = ConfPath();
  std::string text;
  if (FILE* f = fopen(path.c_str(), "rb")) {
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
  }
  const bool crlf = text.find("\r\n") != std::string::npos;
  const std::string eol = crlf ? "\r\n" : "\n";
  const std::string line_new = std::string(key) + " = " + value;
  std::string out;
  bool done = false;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    size_t next = end == std::string::npos ? text.size() : end + 1;
    std::string line = text.substr(pos, (end == std::string::npos ? text.size() : end) - pos);
    std::string term = text.substr(pos + line.size(), next - pos - line.size());  // "\n" or "" (the CR stays in line)
    bool cr = !line.empty() && line.back() == '\r';
    if (cr) line.pop_back();
    if (LineKey(line) == key) {
      out += line_new + (cr ? "\r" : "") + term;
      done = true;
    } else {
      out += line + (cr ? "\r" : "") + term;
    }
    pos = next;
  }
  if (!done) {
    if (!out.empty() && out.back() != '\n') out += eol;
    out += line_new + eol;
  }
  const std::string tmp = path + ".tmp";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) return false;
  bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
  ok = (fclose(f) == 0) && ok;
  if (!ok) {
    DeleteFileA(tmp.c_str());
    return false;
  }
  if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileA(tmp.c_str());
    return false;
  }
  return true;
}

bool Truthy(const std::string& v) { return v == "1" || v == "true" || v == "yes" || v == "on"; }
bool Falsy(const std::string& v) { return v == "0" || v == "false" || v == "no" || v == "off"; }

// ------------------------------------------------------------------------------------------- the items

struct Item {
  MenuRow row;
  int page = MENU_PAGE_DEVELOPER;  // where the key table puts it
  const char* help = nullptr;      // the key table's help line (null: none)
  std::string value;    // saved in the file (or the fallback)
  std::string running;  // what the game runs with (the value when the menu first saw the row)
};

std::vector<Item> g_items;
std::map<std::string, std::string> g_running;  // key -> value at the menu's first sight of it
LONG g_items_version = -1;
bool g_developer = false;  // menu_developer: the DEVELOPER page is shown

bool IsAction(const MenuRow& r) { return r.acts == MENU_ACTION; }
bool IsNumber(const MenuRow& r) { return !r.choices[0].value && !IsAction(r); }

int ChoiceCount(const MenuRow& r) {
  int n = 0;
  while (n < kMenuMaxChoices && r.choices[n].value) ++n;
  return n;
}

// The choice a value stands for (true/1/yes/on all stand for "true"), -1 when none.
int ChoiceIndex(const MenuRow& r, const std::string& v) {
  const int n = ChoiceCount(r);
  for (int i = 0; i < n; ++i)
    if (v == r.choices[i].value) return i;
  for (int i = 0; i < n; ++i) {
    if (Truthy(v) && !strcmp(r.choices[i].value, "true")) return i;
    if (Falsy(v) && !strcmp(r.choices[i].value, "false")) return i;
  }
  return -1;
}

std::string Canonical(const MenuRow& r, const std::string& v) {
  for (const ValueAlias& a : kAliases)
    if (!strcmp(a.key, r.key) && v == a.from) return a.to;
  if (IsNumber(r)) return v;
  int i = ChoiceIndex(r, v);
  return i >= 0 ? r.choices[i].value : v;
}

std::string ReadValue(const MenuRow& r) {
  if (IsAction(r)) return "";
  std::string v = ss_conf(r.key);
  if (v.empty()) v = r.fallback ? r.fallback : "";
  return Canonical(r, v);
}

// The key table's index of a key; keys not in it come after every listed one (in the order they were registered).
int OrderOf(const char* key) {
  const int n = static_cast<int>(sizeof kKeys / sizeof kKeys[0]);
  for (int i = 0; i < n; ++i)
    if (!strcmp(kKeys[i].key, key)) return i;
  return n;
}

// The rows as the registry has them now, sorted by the key table, placed on its pages, with their values read from
// the file.
void BuildItems() {
  std::vector<MenuRow> rows;
  AcquireSRWLockShared(&g_reg_lock);
  rows = Registry();
  ReleaseSRWLockShared(&g_reg_lock);
  g_items_version = g_reg_version;
  std::stable_sort(rows.begin(), rows.end(), [](const MenuRow& a, const MenuRow& b) { return OrderOf(a.key) < OrderOf(b.key); });
  g_items.clear();
  for (const MenuRow& r : rows) {
    Item it;
    it.row = r;
    if (const KeyInfo* k = FindKey(r.key)) {
      it.row.label = k->label;
      it.help = k->help;
      it.page = k->level == LEVEL_PLAYER ? k->page : k->level == LEVEL_ADVANCED ? MENU_PAGE_ADVANCED : MENU_PAGE_DEVELOPER;
    } else {
      it.page = MENU_PAGE_DEVELOPER;  // not in the key table: never on a player page
    }
    if (it.page == MENU_PAGE_DEVELOPER && !g_developer) continue;
    it.value = ReadValue(r);
    auto run = g_running.find(r.key);
    if (run == g_running.end()) run = g_running.emplace(r.key, it.value).first;
    it.running = run->second;
    g_items.push_back(it);
  }
}

std::string ValueText(const Item& it, const std::string& v) {
  const MenuRow& r = it.row;
  char buf[64];
  if (IsAction(r)) return r.status ? r.status() : "OPEN";
  if (const ValueInfo* vi = FindValue(r.key, v))
    if (vi->label) return vi->label;
  if (IsNumber(r)) {
    snprintf(buf, sizeof buf, "%d%s", atoi(v.c_str()), r.unit ? r.unit : "");
    return buf;
  }
  int i = ChoiceIndex(r, v);
  if (i >= 0) return r.choices[i].label;
  std::string s = v.empty() ? "-" : v;  // set in the file, not one the row offers
  for (char& c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
  return s + " (CONF)";
}

// The help band's text for a row: the value's own line when it has one, else the row's; on DEVELOPER the conf key.
std::string HelpText(const Item& it) {
  if (const ValueInfo* vi = FindValue(it.row.key, it.value))
    if (vi->help) return vi->help;
  if (!strncmp(it.row.key, "volume_", 7)) {
    std::string h = it.help ? it.help : "Fine volume for one kind of sound, on top of its main volume. 100% is the game's own level.";
    if (atoi(it.value.c_str()) > 100) h += " Above 100% the sound may distort (clip).";
    return h;
  }
  if (it.help) return it.help;
  return std::string("splitsecond.conf: ") + it.row.key;
}

// A restart row whose saved value differs from the running one, or a live row whose saved value did not take (the
// part could not apply it now, e.g. its hook was not put in at start): both wait for the next start.
bool NeedsRestart(const Item& it) { return !IsAction(it.row) && it.value != it.running; }

// Left / right: the next value is saved to splitsecond.conf at once and, on a live row, applied.
void Apply(Item& it, const std::string& v) {
  const char* key = it.row.key;
  if (!WriteConf(key, v)) {
    ss_log("[menu] %s = %s: could not write splitsecond.conf (error %lu); not applied", key, v.c_str(), GetLastError());
    return;
  }
  it.value = v;
  bool applied = false;
  if (it.row.acts == MENU_LIVE && it.row.apply) applied = it.row.apply(key, v.c_str());
  if (applied) it.running = g_running[key] = v;
  const char* how = it.row.acts == MENU_RESTART ? (v == it.running ? "saved, as running" : "saved, restart needed")
                    : applied                   ? "applied"
                    : v == it.running           ? "saved, as running"
                                                : "saved, not applied now: restart needed";
  ss_log("[menu] %s = %s (%s)", key, v.c_str(), how);
}

void Step(Item& it, int dir) {
  const MenuRow& r = it.row;
  if (IsAction(r)) return;
  const std::string& cur = it.value;
  std::string next;
  if (IsNumber(r)) {
    // A value set in the file outside the row's range (a volume of 150 where the menu offers 0..120) is kept: a step
    // never moves it further out, so the outward direction leaves it as it is and the inward one moves it one step
    // toward the range (150 -> 145 -> ... -> 120).
    const int c = atoi(cur.c_str());
    const int lo = std::min(r.min, c), hi = std::max(r.max, c);
    int v = c + dir * r.step;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    next = std::to_string(v);
  } else {
    const int n = ChoiceCount(r);
    if (n == 0) return;
    int i = ChoiceIndex(r, cur);
    i = i < 0 ? 0 : (i + dir + n) % n;  // a value the row does not offer (conf-only): the first choice
    next = r.choices[i].value;
  }
  if (next != cur) Apply(it, next);
}

// ------------------------------------------------------------------------------------------- input

constexpr unsigned short kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008, kStart = 0x0010, kBack = 0x0020,
                         kLB = 0x0100, kRB = 0x0200, kA = 0x1000, kB = 0x2000, kY = 0x8000;

struct PadSeen {
  volatile unsigned short buttons;
  volatile float lx, ly;
  volatile DWORD when;
};
PadSeen g_pads[4];

volatile bool g_open = false;
volatile bool g_latch = false;  // closed, but a button or key that worked the menu is still held
void* volatile g_request = nullptr;  // ss_menu_request_open: open at the next present (the literal is the reason)
int g_enabled = -1;
bool g_failed = false;          // no font: the menu cannot be drawn
bool g_used_pad = false;        // the last input was a pad (prompts show pad buttons)

bool KeyHeld(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

bool GameInFront() {
  HWND fg = GetForegroundWindow();
  if (!fg) return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  return pid == GetCurrentProcessId();
}

struct PadNow {
  unsigned short buttons = 0;
  float lx = 0, ly = 0;
};

PadNow ReadPads() {
  PadNow p;
  const DWORD now = GetTickCount();
  for (const PadSeen& s : g_pads) {
    if (!s.when || now - s.when > 500) continue;  // a pad the game has stopped reading
    p.buttons |= s.buttons;
    if (fabsf(s.lx) > fabsf(p.lx)) p.lx = s.lx;
    if (fabsf(s.ly) > fabsf(p.ly)) p.ly = s.ly;
  }
  // Also the XInput pads read here: the game does not poll its pads on every screen (paused menus), and a press it
  // never read must still open and work the menu.
  unsigned short b = 0;
  float x = 0, y = 0;
  if (ss_pad_direct(&b, &x, &y)) {
    p.buttons |= b;
    if (fabsf(x) > fabsf(p.lx)) p.lx = x;
    if (fabsf(y) > fabsf(p.ly)) p.ly = y;
  }
  return p;
}

// VK_TAB: the game's key for Y (the Options screen's PC SETTINGS prompt); held, it keeps the game's input quiet after a close.
const int kMenuKeys[] = {VK_F1, VK_ESCAPE, VK_RETURN, VK_BACK, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, 'Q', 'E', VK_SPACE, VK_TAB};

// Direction / button state of one frame, with edges and auto-repeat for the directions.
struct Input {
  bool up, down, left, right, apply, back, prev, next, close;
};

struct Repeat {
  bool held = false;
  DWORD t0 = 0, last = 0;
  bool Fire(bool down, DWORD now) {
    if (!down) { held = false; return false; }
    if (!held) { held = true; t0 = last = now; return true; }
    if (now - t0 >= 380 && now - last >= 90) { last = now; return true; }
    return false;
  }
};

Repeat g_rep[4];
unsigned short g_prev_buttons = 0;
bool g_prev_keys[256];

void ResetEdges() {
  g_prev_buttons = ReadPads().buttons;
  for (int vk : kMenuKeys) g_prev_keys[vk] = KeyHeld(vk);
  for (Repeat& r : g_rep) r.held = true, r.t0 = r.last = GetTickCount();  // a held direction does not move at once
}

Input ReadInput() {
  Input in = {};
  const DWORD now = GetTickCount();
  const bool front = GameInFront();
  bool keys[256] = {};
  for (int vk : kMenuKeys) keys[vk] = front && KeyHeld(vk);
  auto kedge = [&](int vk) { return keys[vk] && !g_prev_keys[vk]; };
  const PadNow pad = ReadPads();
  const unsigned short b = pad.buttons;
  auto pedge = [&](unsigned short bit) { return (b & bit) && !(g_prev_buttons & bit); };

  const bool pad_dir[4] = {(b & kUp) || pad.ly > 0.6f, (b & kDown) || pad.ly < -0.6f, (b & kLeft) || pad.lx < -0.6f,
                           (b & kRight) || pad.lx > 0.6f};
  const bool key_dir[4] = {keys[VK_UP], keys[VK_DOWN], keys[VK_LEFT], keys[VK_RIGHT]};
  bool fire[4];
  for (int i = 0; i < 4; ++i) fire[i] = g_rep[i].Fire(pad_dir[i] || key_dir[i], now);
  in.up = fire[0];
  in.down = fire[1];
  in.left = fire[2];
  in.right = fire[3];
  in.apply = pedge(kA) || kedge(VK_RETURN) || kedge(VK_SPACE);
  in.back = pedge(kB) || kedge(VK_ESCAPE) || kedge(VK_BACK);
  in.prev = pedge(kLB) || kedge('Q');
  in.next = pedge(kRB) || kedge('E');
  in.close = kedge(VK_F1) || pedge(kBack) || pedge(kStart);

  bool any_key = false;
  for (int vk : kMenuKeys) any_key |= keys[vk] && !g_prev_keys[vk];
  if (any_key) g_used_pad = false;  // the prompts follow the last device pressed
  else if (b & ~g_prev_buttons) g_used_pad = true;

  g_prev_buttons = b;
  for (int vk : kMenuKeys) g_prev_keys[vk] = keys[vk];
  return in;
}

bool AnythingHeld() {
  if (ReadPads().buttons) return true;
  for (int vk : kMenuKeys)
    if (KeyHeld(vk)) return true;
  return false;
}

// ------------------------------------------------------------------------------------------- the page

int g_page = 0;
int g_sel = 0;     // row on the page
int g_scroll = 0;  // first row shown
constexpr int kRowsShown = 10;

std::vector<int> RowsOn(int page) {
  std::vector<int> v;
  for (size_t i = 0; i < g_items.size(); ++i)
    if (g_items[i].page == page) v.push_back(static_cast<int>(i));
  return v;
}

std::vector<int> Pages() {
  std::vector<int> v;
  for (int p = 0; p < MENU_PAGE_COUNT; ++p)
    if (!RowsOn(p).empty()) v.push_back(p);
  return v;
}

void TurnPage(int dir) {
  std::vector<int> pages = Pages();
  if (pages.empty()) return;
  int at = 0;
  for (size_t i = 0; i < pages.size(); ++i)
    if (pages[i] == g_page) at = static_cast<int>(i);
  const int n = static_cast<int>(pages.size());
  g_page = pages[(at + dir + n) % n];
  g_sel = g_scroll = 0;
}

void CalClose();

void Close(const char* why) {
  CalClose();
  g_open = false;
  g_latch = true;
  ss_log("[menu] closed (%s)", why);
}

void Open(const char* how) {
  static bool first = true;
  if (first) {
    first = false;
    RegisterBuiltins();
  }
#ifdef SS_MENU_MIXER
  RegisterMixerRows();
#endif
  // menu_developer = true (or SS_MENU_DEVELOPER=1): the DEVELOPER page; read at every open.
  {
    std::string v = ss_conf("menu_developer");
    if (const char* e = getenv("SS_MENU_DEVELOPER")) v = e;
    g_developer = Truthy(v);
  }
  BuildItems();
  std::vector<int> pages = Pages();
  if (pages.empty()) return;
  if (std::find(pages.begin(), pages.end(), g_page) == pages.end()) g_page = pages[0], g_sel = g_scroll = 0;
  ResetEdges();
  g_open = true;
  ss_log("[menu] opened (%s), page %s", how, kPageNames[g_page]);
}

// The HDR calibration screen (the port's): a black screen with words; hdr.cpp's output pass draws the two patterns
// into the boxes left free (the same fractions of the screen there). Left / right change the focused value live;
// leaving it writes what was changed to splitsecond.conf.
bool g_cal = false;
int g_cal_sel = 0;  // 0 peak, 1 paper white
int g_cal_value[2] = {1000, 240};
bool g_cal_touched[2] = {false, false};
constexpr int kCalMin[2] = {400, 120}, kCalMax[2] = {4000, 400}, kCalStep[2] = {50, 10};
const char* const kCalKey[2] = {"hdr_peak", "hdr_paper_white"};

void CalClose() {
  if (!g_cal) return;
  g_cal = false;
  ss_hdr_calibrating(false);
  for (int i = 0; i < 2; ++i) {
    if (!g_cal_touched[i]) continue;
    const std::string v = std::to_string(g_cal_value[i]);
    if (!WriteConf(kCalKey[i], v)) {
      ss_log("[menu] %s = %s: could not write splitsecond.conf (error %lu)", kCalKey[i], v.c_str(), GetLastError());
      continue;
    }
    g_running[kCalKey[i]] = v;  // applied live on the screen
    ss_log("[menu] %s = %s (from the calibration screen)", kCalKey[i], v.c_str());
  }
  BuildItems();
  ss_log("[menu] HDR calibration closed");
}

void NavigateCal(const Input& in) {
  if (!ss_hdr_active() || in.back || in.apply || in.close) {
    CalClose();
    return;
  }
  if (in.up || in.down) g_cal_sel ^= 1;
  const int dir = in.right ? 1 : in.left ? -1 : 0;
  if (dir) {
    const int i = g_cal_sel;
    int v = (g_cal_value[i] / kCalStep[i]) * kCalStep[i] + dir * kCalStep[i];
    v = v < kCalMin[i] ? kCalMin[i] : v > kCalMax[i] ? kCalMax[i] : v;
    g_cal_value[i] = v;
    g_cal_touched[i] = true;
    ss_hdr_apply(kCalKey[i], std::to_string(v).c_str());
  }
}

void Navigate(const Input& in) {
  if (g_cal) {
    NavigateCal(in);
    return;
  }
  if (g_items_version != g_reg_version) {  // rows registered while open: keep the edits out, rebuild
    BuildItems();
    std::vector<int> pages = Pages();
    if (std::find(pages.begin(), pages.end(), g_page) == pages.end() && !pages.empty()) g_page = pages[0], g_sel = g_scroll = 0;
  }
  if (in.close) { Close("closed by the toggle"); return; }
  if (in.back) { Close("back"); return; }
  if (in.prev) TurnPage(-1);
  if (in.next) TurnPage(1);
  std::vector<int> rows = RowsOn(g_page);
  if (rows.empty()) return;
  const int n = static_cast<int>(rows.size());
  if (g_sel >= n) g_sel = n - 1;
  if (in.up || in.down) g_sel = (g_sel + (in.down ? 1 : -1) + n) % n;
  Item& it = g_items[rows[g_sel]];
  // Left / right save (and apply) at once; A / Enter works action rows only.
  if (in.left) Step(it, -1);
  if (in.right) Step(it, 1);
  if (in.apply && IsAction(it.row) && it.row.apply) it.row.apply(it.row.key, "");
  if (g_sel < g_scroll) g_scroll = g_sel;
  if (g_sel >= g_scroll + kRowsShown) g_scroll = g_sel - kRowsShown + 1;
}

// ------------------------------------------------------------------------------------------- drawing

using CreateFontFn = HRESULT(WINAPI*)(IDirect3DDevice9*, INT, UINT, UINT, UINT, BOOL, DWORD, DWORD, DWORD, DWORD, LPCSTR, ID3DXFont**);
using CreateSpriteFn = HRESULT(WINAPI*)(IDirect3DDevice9*, ID3DXSprite**);
using ResetFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

struct Gfx {
  IDirect3DDevice9* dev = nullptr;
  IDirect3DStateBlock9* sb = nullptr;
  ID3DXSprite* sprite = nullptr;
  ID3DXFont* big = nullptr;
  ID3DXFont* body = nullptr;
  ID3DXFont* small = nullptr;
  UINT h = 0;
} g_gfx;

CreateFontFn g_create_font = nullptr;
CreateSpriteFn g_create_sprite = nullptr;
ResetFn g_reset_orig = nullptr;

template <typename T>
void SafeRelease(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

void ReleaseGfx() {
  SafeRelease(g_gfx.big);
  SafeRelease(g_gfx.body);
  SafeRelease(g_gfx.small);
  SafeRelease(g_gfx.sprite);
  SafeRelease(g_gfx.sb);
  g_gfx.dev = nullptr;
  g_gfx.h = 0;
}

HRESULT WINAPI ResetHook(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
  ReleaseGfx();  // a reset fails while state blocks or default-pool objects of ours are alive
  return g_reset_orig(dev, pp);
}

// Reset is slot 16 of the same vtable as Present (d3d9.dll's memory, never the game's). Once per vtable: postfx.cpp
// and display.cpp wrap the same slot and one of them may sit over ours; taking the slot's word again as our original
// would make a loop (menu -> postfx -> menu ...) that overflows the stack at the next Reset.
void** g_reset_vt = nullptr;

void HookReset(IDirect3DDevice9* dev) {
  void** vt = *reinterpret_cast<void***>(dev);
  void* mine = reinterpret_cast<void*>(&ResetHook);
  if (vt == g_reset_vt || vt[16] == mine) return;
  HMODULE owner = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<const char*>(vt[16]), &owner) ||
      !owner || owner == GetModuleHandleA(nullptr))
    return;
  g_reset_orig = reinterpret_cast<ResetFn>(vt[16]);  // set before the slot points at us
  if (!ss_write_word(&vt[16], reinterpret_cast<uintptr_t>(mine))) return;
  g_reset_vt = vt;
  ss_log("[menu] device Reset wrapped (the menu's objects are freed before a reset)");
}

bool LoadD3dx() {
  if (g_create_font && g_create_sprite) return true;
  HMODULE m = GetModuleHandleA("d3dx9_42.dll");
  if (!m) m = LoadLibraryA("d3dx9_42.dll");
  if (!m) m = LoadLibraryA("d3dx9_43.dll");
  if (!m) return false;
  g_create_font = reinterpret_cast<CreateFontFn>(GetProcAddress(m, "D3DXCreateFontA"));
  g_create_sprite = reinterpret_cast<CreateSpriteFn>(GetProcAddress(m, "D3DXCreateSprite"));
  return g_create_font && g_create_sprite;
}

bool EnsureGfx(IDirect3DDevice9* dev, UINT h) {
  if (g_gfx.dev == dev && g_gfx.h == h && g_gfx.sb) return true;
  ReleaseGfx();
  if (!LoadD3dx()) return false;
  HookReset(dev);
  const float u = h / 720.0f;
  auto font = [&](int px, UINT weight, ID3DXFont** out) {
    return SUCCEEDED(g_create_font(dev, static_cast<INT>(px * u + 0.5f), 0, weight, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                   ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial", out));
  };
  if (!font(34, FW_HEAVY, &g_gfx.big) || !font(21, FW_BOLD, &g_gfx.body) || !font(15, FW_BOLD, &g_gfx.small) ||
      FAILED(g_create_sprite(dev, &g_gfx.sprite)) || FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_gfx.sb))) {
    ReleaseGfx();
    return false;
  }
  g_gfx.dev = dev;
  g_gfx.h = h;
  return true;
}

// Colours (ARGB): the game's menus are white capitals on dark panels with an orange highlight.
constexpr DWORD kWhite = 0xFFFFFFFF, kGrey = 0xFFA8ACB0, kDim = 0xFF6E7378, kOrange = 0xFFFF8C1A, kShadow = 0xC0000000;

struct Quad {
  float x, y, w, h;
  DWORD c;
};
struct Text {
  ID3DXFont* font;
  std::string s;
  RECT r;
  DWORD fmt, c;
};

std::vector<Quad> g_quads;
std::vector<Text> g_texts;

void AddQuad(float x, float y, float w, float h, DWORD c) { g_quads.push_back({x, y, w, h, c}); }

void AddText(ID3DXFont* f, const std::string& s, float x, float y, float w, float h, DWORD fmt, DWORD c, bool shadow = true) {
  RECT r = {static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x + w), static_cast<LONG>(y + h)};
  if (shadow) {
    RECT rs = r;
    OffsetRect(&rs, 1, 2);
    g_texts.push_back({f, s, rs, fmt, kShadow});
  }
  g_texts.push_back({f, s, r, fmt, c});
}

int TextWidth(ID3DXFont* f, const std::string& s) {
  RECT r = {0, 0, 0, 0};
  f->DrawTextA(nullptr, s.c_str(), -1, &r, DT_CALCRECT | DT_SINGLELINE | DT_NOCLIP, 0);
  return r.right - r.left;
}

// What Draw put over the frame (ss_menu_cover): the panel's rectangle, and whether anything was drawn at all.
float g_cover_panel[4] = {0, 0, 0, 0};
bool g_cover_drawn = false, g_cover_cal = false;

// The calibration screen: words only, on black; the boxes (0.06, 0.24)-(0.46, 0.70) and (0.54, 0.24)-(0.94, 0.70)
// of the screen are left to the output pass's patterns (hdr.cpp, kPeakPanel / kPaperPanel).
void LayoutCal(float W, float H) {
  const float u = H / 720.0f;
  const DWORD mid = DT_SINGLELINE | DT_VCENTER | DT_NOCLIP;
  AddQuad(0, 0, W, H, 0xFF000000);
  AddText(g_gfx.big, "HDR CALIBRATION", 0, 0.04f * H, W, 44 * u, mid | DT_CENTER, kWhite, false);
  const float x[2] = {0.06f * W, 0.54f * W}, w = 0.40f * W;
  const char* const title[2] = {"PEAK PATTERN", "PAPER WHITE CARD"};
  const char* const help[2] = {"RAISE PEAK BRIGHTNESS UNTIL THE DISC JUST DISAPPEARS",
                               "SET IT SO THIS LOOKS LIKE A SHEET OF PAPER UNDER ROOM LIGHT"};
  const char* const name[2] = {"PEAK BRIGHTNESS", "GAME BRIGHTNESS"};
  for (int i = 0; i < 2; ++i) {
    const bool focus = g_cal_sel == i;
    AddText(g_gfx.body, title[i], x[i], 0.17f * H, w, 30 * u, mid | DT_CENTER, focus ? kOrange : kGrey, false);
    AddText(g_gfx.small, help[i], x[i], 0.72f * H, w, 22 * u, mid | DT_CENTER, kGrey, false);
    char v[64];
    snprintf(v, sizeof v, focus ? "%s   <<  %d NITS  >>" : "%s   %d NITS", name[i], g_cal_value[i]);
    AddText(g_gfx.body, v, x[i], 0.78f * H, w, 34 * u, mid | DT_CENTER, focus ? kWhite : kGrey, false);
  }
  AddText(g_gfx.small, g_used_pad ? "UP, DOWN: CHOOSE      LEFT, RIGHT: CHANGE      B: DONE" : "UP, DOWN: CHOOSE      LEFT, RIGHT: CHANGE      ESC: DONE", 0,
          0.90f * H, W, 22 * u, mid | DT_CENTER, kGrey, false);
}

void Layout(float W, float H) {
  g_quads.clear();
  g_texts.clear();
  g_cover_cal = g_cal;
  if (g_cal) {
    LayoutCal(W, H);
    return;
  }
  const float u = H / 720.0f;
  const DWORD mid = DT_SINGLELINE | DT_VCENTER | DT_NOCLIP;
  std::vector<int> rows = RowsOn(g_page);
  const int shown = std::min(static_cast<int>(rows.size()) - g_scroll, kRowsShown);
  // 880 units wide: eight page names fit at the body font (measured below; smaller where the screen is narrow).
  const float pw = std::min(W - 32.0f, 880 * u);
  const float row_h = 38 * u, head = 112 * u, help_h = 48 * u, foot = 44 * u;
  const float ph = head + shown * row_h + 16 * u + help_h + foot;
  const float x0 = (W - pw) * 0.5f, y0 = std::max(16.0f, (H - ph) * 0.5f);
  const float pad = 26 * u;
  const float inner = pw - 2 * pad;

  g_cover_panel[0] = x0, g_cover_panel[1] = y0, g_cover_panel[2] = x0 + pw, g_cover_panel[3] = y0 + ph;
  AddQuad(0, 0, W, H, 0x70000000);                          // the game behind, dimmed (hdr.cpp knows this alpha)
  AddQuad(x0, y0, pw, ph, 0xE0101317);                      // panel
  AddQuad(x0, y0, pw, 4 * u, kOrange);                      // accent line
  AddText(g_gfx.big, "PC SETTINGS", x0 + pad, y0 + 14 * u, inner, 44 * u, mid | DT_LEFT, kWhite);
  AddText(g_gfx.body, "BACK ON AIR", x0 + pad, y0 + 14 * u, inner, 44 * u, mid | DT_RIGHT, kOrange);  // the patch's name

  // Page tabs, measured: the body font with a 28-unit gap, or a narrower gap down to 12 units; when even that does not
  // fit the panel, the small font with the gap that fits.
  std::vector<int> pages = Pages();
  ID3DXFont* tab_font = g_gfx.body;
  float gap = 28 * u;
  {
    int sum = 0;
    for (int p : pages) sum += TextWidth(g_gfx.body, kPageNames[p]);
    const int n = static_cast<int>(pages.size());
    if (n > 1 && sum + (n - 1) * gap > inner) {
      gap = (inner - sum) / (n - 1);
      if (gap < 12 * u) {
        tab_font = g_gfx.small;
        sum = 0;
        for (int p : pages) sum += TextWidth(g_gfx.small, kPageNames[p]);
        gap = std::max(4 * u, std::min(16 * u, (inner - sum) / (n - 1)));
      }
    }
  }
  float tx = x0 + pad;
  const float ty = y0 + 66 * u;
  for (int p : pages) {
    const std::string name = kPageNames[p];
    const int w = TextWidth(tab_font, name);
    const bool on = p == g_page;
    AddText(tab_font, name, tx, ty, static_cast<float>(w) + 4, 28 * u, mid | DT_LEFT, on ? kOrange : kDim);
    if (on) AddQuad(tx, ty + 29 * u, static_cast<float>(w), 3 * u, kOrange);
    tx += w + gap;
  }
  AddQuad(x0 + pad, y0 + head - 8 * u, inner, 1 * u + 0.5f, 0x40FFFFFF);

  // Rows. Every change is saved at once; a restart row says RESTART (dim) while it runs as saved and RESTART NEEDED
  // (orange) once changed; a live row that could not take its value now says RESTART NEEDED too.
  float y = y0 + head;
  for (int k = 0; k < shown; ++k) {
    const int idx = g_scroll + k;
    const Item& it = g_items[rows[idx]];
    const bool focus = idx == g_sel;
    if (focus) {
      AddQuad(x0 + pad - 10 * u, y, inner + 20 * u, row_h - 2 * u, 0x38FF8C1A);
      AddQuad(x0 + pad - 10 * u, y, 4 * u, row_h - 2 * u, kOrange);
    }
    const std::string label = it.row.label;
    AddText(g_gfx.body, label, x0 + pad + 4 * u, y, pw * 0.6f, row_h, mid | DT_LEFT, focus ? kWhite : kGrey);
    const float lw = static_cast<float>(TextWidth(g_gfx.body, label));
    if (NeedsRestart(it)) {
      AddText(g_gfx.small, "RESTART NEEDED", x0 + pad + 4 * u + lw + 12 * u, y, 200 * u, row_h, mid | DT_LEFT, kOrange);
    } else if (it.row.acts == MENU_RESTART) {
      AddText(g_gfx.small, "RESTART", x0 + pad + 4 * u + lw + 12 * u, y, 200 * u, row_h, mid | DT_LEFT, kDim);
    }
    std::string v = ValueText(it, it.value);
    if (focus) v = "<<  " + v + "  >>";
    AddText(g_gfx.body, v, x0 + pw * 0.45f, y, pw * 0.55f - pad, row_h, mid | DT_RIGHT, focus ? kWhite : kGrey);
    y += row_h;
  }
  if (g_scroll > 0) AddText(g_gfx.small, "MORE", x0 + pad, y0 + head - 26 * u, inner, 18 * u, mid | DT_RIGHT, kDim);
  if (g_scroll + shown < static_cast<int>(rows.size()))
    AddText(g_gfx.small, "MORE", x0 + pad, y, inner, 18 * u, mid | DT_RIGHT, kDim);

  // Help band: what the focused row (or its value) does, two lines at most.
  const float hy = y0 + ph - foot - help_h;
  AddQuad(x0 + pad, hy, inner, 1 * u + 0.5f, 0x40FFFFFF);
  if (g_sel >= 0 && g_sel < static_cast<int>(rows.size())) {
    const Item& it = g_items[rows[g_sel]];
    AddText(g_gfx.small, HelpText(it), x0 + pad, hy + 6 * u, inner, help_h - 8 * u, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOCLIP,
            kWhite);
  }

  // Prompts.
  const float fy = y0 + ph - foot;
  AddQuad(x0 + pad, fy, inner, 1 * u + 0.5f, 0x40FFFFFF);
  const bool action = g_sel >= 0 && g_sel < static_cast<int>(rows.size()) && IsAction(g_items[rows[g_sel]].row);
  std::string prompts;
  if (g_used_pad)
    prompts = action ? "A  OPEN      B  CLOSE      LB/RB  PAGE" : "LEFT/RIGHT  CHANGE (SAVED AT ONCE)      B  CLOSE      LB/RB  PAGE";
  else
    prompts = action ? "ENTER  OPEN      ESC  CLOSE      Q/E  PAGE" : "LEFT/RIGHT  CHANGE (SAVED AT ONCE)      ESC  CLOSE      Q/E  PAGE";
  AddText(g_gfx.small, prompts, x0 + pad, fy + 4 * u, inner, foot - 8 * u, mid | DT_LEFT, kGrey);
}

struct Vtx {
  float x, y, z, rhw;
  DWORD c;
};

// Windows' own d3d9 keeps the device's function table inside the device and writes it back whole during d3dx9's font
// and sprite work (the overlay's first frame on native D3D9, SSPC-37): every slot hook of this patch (Present,
// SetSamplerState, SetPixelShader, the draw calls, Reset, EndScene, ...) was gone after it. The table is read before
// the overlay draws and every slot that changed is written back after it. DXVK's table never changes: nothing written.
struct SlotGuard {
  static constexpr int kSlots = 119;  // IDirect3DDevice9's methods
  void** vt;
  void* saved[kSlots];
  explicit SlotGuard(IDirect3DDevice9* dev) : vt(*reinterpret_cast<void***>(dev)) { memcpy(saved, vt, sizeof saved); }
  ~SlotGuard() {
    int n = 0;
    for (int i = 0; i < kSlots; ++i)
      if (vt[i] != saved[i] && ss_write_word(&vt[i], reinterpret_cast<uintptr_t>(saved[i]))) ++n;
    static int said = 0;
    if (n && said++ < 3) ss_log("[menu] %d device vtable slots were reset by the d3d9 runtime while the overlay drew; written back", n);
  }
};

void Draw(IDirect3DDevice9* dev) {
  HookReset(dev);  // before the guard: its own slot write is not one to undo
  SlotGuard guard(dev);
  IDirect3DSurface9* bb = nullptr;
  if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
  D3DSURFACE_DESC d;
  bb->GetDesc(&d);
  if (!EnsureGfx(dev, d.Height)) {
    bb->Release();
    if (!g_failed) ss_log("[menu] d3dx9 font or state block unavailable; the menu cannot be drawn and stays closed");
    g_failed = true;
    return;
  }
  Layout(static_cast<float>(d.Width), static_cast<float>(d.Height));

  ++ss_own_drawing;  // our BeginScene / EndScene / states: the other device hooks pass them on
  g_gfx.sb->Capture();
  IDirect3DSurface9* old_rt = nullptr;
  IDirect3DSurface9* old_ds = nullptr;
  dev->GetRenderTarget(0, &old_rt);
  dev->GetDepthStencilSurface(&old_ds);
  dev->SetRenderTarget(0, bb);  // also sets the viewport to the whole back buffer
  dev->SetDepthStencilSurface(nullptr);
  const bool began = SUCCEEDED(dev->BeginScene());

  dev->SetVertexShader(nullptr);
  dev->SetPixelShader(nullptr);
  dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
  dev->SetTexture(0, nullptr);
  dev->SetRenderState(D3DRS_ZENABLE, FALSE);
  dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
  dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
  dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
  dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
  dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
  dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
  dev->SetRenderState(D3DRS_LIGHTING, FALSE);
  dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
  dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
  dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
  dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
  dev->SetRenderState(D3DRS_CLIPPING, TRUE);
  dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
  dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
  dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

  for (const Quad& q : g_quads) {
    const float x = q.x - 0.5f, y = q.y - 0.5f;
    const Vtx v[4] = {{x, y, 0, 1, q.c}, {x + q.w, y, 0, 1, q.c}, {x, y + q.h, 0, 1, q.c}, {x + q.w, y + q.h, 0, 1, q.c}};
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vtx));
  }
  if (SUCCEEDED(g_gfx.sprite->Begin(D3DXSPRITE_ALPHABLEND))) {
    for (Text& t : g_texts) t.font->DrawTextA(g_gfx.sprite, t.s.c_str(), -1, &t.r, t.fmt, t.c);
    g_gfx.sprite->End();
  }

  if (began) dev->EndScene();
  g_cover_drawn = began;
  dev->SetRenderTarget(0, old_rt);
  dev->SetDepthStencilSurface(old_ds);
  if (old_rt) old_rt->Release();
  if (old_ds) old_ds->Release();
  bb->Release();
  g_gfx.sb->Apply();  // every state as the game left it (viewport included)
  --ss_own_drawing;
}

// ------------------------------------------------------------------- keyboard and mouse while captured

// Wraps the game's keyboard or mouse device: everything passes through, except that while the menu captures the
// state reads report nothing pressed and the buffered data keeps only releases (so the game never holds a key the
// menu swallowed the release of).
struct QuietDevice final : IDirectInputDevice8A {
  LONG refs = 1;
  IDirectInputDevice8A* in;
  bool keyboard;
  QuietDevice(IDirectInputDevice8A* d, bool kb) : in(d), keyboard(kb) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** o) override {
    if (IsEqualIID(r, IID_IUnknown) || IsEqualIID(r, IID_IDirectInputDevice8A)) {
      *o = static_cast<IDirectInputDevice8A*>(this);
      AddRef();
      return S_OK;
    }
    return in->QueryInterface(r, o);
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
  ULONG STDMETHODCALLTYPE Release() override {
    LONG n = InterlockedDecrement(&refs);
    if (n == 0) {
      in->Release();
      delete this;
    }
    return n;
  }
  HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD cb, LPVOID data) override {
    HRESULT hr = in->GetDeviceState(cb, data);
    if (SUCCEEDED(hr) && data && ss_menu_capturing()) memset(data, 0, cb);
    return hr;
  }
  HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD cb, LPDIDEVICEOBJECTDATA od, LPDWORD n, DWORD flags) override {
    HRESULT hr = in->GetDeviceData(cb, od, n, flags);
    if (SUCCEEDED(hr) && od && n && cb >= sizeof(DWORD) * 2 && ss_menu_capturing()) {
      BYTE* base = reinterpret_cast<BYTE*>(od);
      DWORD kept = 0;
      for (DWORD i = 0; i < *n; ++i) {
        const DIDEVICEOBJECTDATA* e = reinterpret_cast<const DIDEVICEOBJECTDATA*>(base + i * cb);
        const bool button = keyboard || e->dwOfs >= DIMOFS_BUTTON0;
        if (button && !(e->dwData & 0x80)) {
          if (kept != i) memmove(base + kept * cb, e, cb);
          ++kept;
        }
      }
      *n = kept;
    }
    return hr;
  }
  HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS c) override { return in->GetCapabilities(c); }
  HRESULT STDMETHODCALLTYPE EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID r, DWORD f) override { return in->EnumObjects(cb, r, f); }
  HRESULT STDMETHODCALLTYPE GetProperty(REFGUID p, LPDIPROPHEADER h) override { return in->GetProperty(p, h); }
  HRESULT STDMETHODCALLTYPE SetProperty(REFGUID p, LPCDIPROPHEADER h) override { return in->SetProperty(p, h); }
  HRESULT STDMETHODCALLTYPE Acquire() override { return in->Acquire(); }
  HRESULT STDMETHODCALLTYPE Unacquire() override { return in->Unacquire(); }
  HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT f) override { return in->SetDataFormat(f); }
  HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE e) override { return in->SetEventNotification(e); }
  HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND h, DWORD f) override { return in->SetCooperativeLevel(h, f); }
  HRESULT STDMETHODCALLTYPE GetObjectInfo(LPDIDEVICEOBJECTINSTANCEA o, DWORD obj, DWORD how) override { return in->GetObjectInfo(o, obj, how); }
  HRESULT STDMETHODCALLTYPE GetDeviceInfo(LPDIDEVICEINSTANCEA i) override { return in->GetDeviceInfo(i); }
  HRESULT STDMETHODCALLTYPE RunControlPanel(HWND h, DWORD f) override { return in->RunControlPanel(h, f); }
  HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE h, DWORD v, REFGUID g) override { return in->Initialize(h, v, g); }
  HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID g, LPCDIEFFECT e, LPDIRECTINPUTEFFECT* o, LPUNKNOWN u) override {
    return in->CreateEffect(g, e, o, u);
  }
  HRESULT STDMETHODCALLTYPE EnumEffects(LPDIENUMEFFECTSCALLBACKA cb, LPVOID r, DWORD t) override { return in->EnumEffects(cb, r, t); }
  HRESULT STDMETHODCALLTYPE GetEffectInfo(LPDIEFFECTINFOA i, REFGUID g) override { return in->GetEffectInfo(i, g); }
  HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD s) override { return in->GetForceFeedbackState(s); }
  HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD c) override { return in->SendForceFeedbackCommand(c); }
  HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, LPVOID r, DWORD f) override {
    return in->EnumCreatedEffectObjects(cb, r, f);
  }
  HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE e) override { return in->Escape(e); }
  HRESULT STDMETHODCALLTYPE Poll() override { return in->Poll(); }
  HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD cb, LPCDIDEVICEOBJECTDATA d, LPDWORD n, DWORD f) override {
    return in->SendDeviceData(cb, d, n, f);
  }
  HRESULT STDMETHODCALLTYPE EnumEffectsInFile(LPCSTR f, LPDIENUMEFFECTSINFILECALLBACK cb, LPVOID r, DWORD fl) override {
    return in->EnumEffectsInFile(f, cb, r, fl);
  }
  HRESULT STDMETHODCALLTYPE WriteEffectToFile(LPCSTR f, DWORD n, LPDIFILEEFFECT e, DWORD fl) override {
    return in->WriteEffectToFile(f, n, e, fl);
  }
  HRESULT STDMETHODCALLTYPE BuildActionMap(LPDIACTIONFORMATA a, LPCSTR u, DWORD f) override { return in->BuildActionMap(a, u, f); }
  HRESULT STDMETHODCALLTYPE SetActionMap(LPDIACTIONFORMATA a, LPCSTR u, DWORD f) override { return in->SetActionMap(a, u, f); }
  HRESULT STDMETHODCALLTYPE GetImageInfo(LPDIDEVICEIMAGEINFOHEADERA h) override { return in->GetImageInfo(h); }
};

}  // namespace

// ------------------------------------------------------------------------------------------- hook-ups

bool ss_menu_cover(float panel[4], bool* calibrating) {
  const bool drawn = g_cover_drawn;
  g_cover_drawn = false;  // one frame's
  if (!drawn) return false;
  for (int i = 0; i < 4; ++i) panel[i] = g_cover_panel[i];
  *calibrating = g_cover_cal;
  return true;
}

bool ss_menu_calibration(const char*, const char*) {
  if (!ss_hdr_active()) {
    ss_log("[menu] HDR calibration: HDR is not in use now; nothing to calibrate");
    return false;
  }
  float peak = 1000, paper = 240, hud = 0;
  ss_hdr_levels(&peak, &paper, &hud);
  g_cal_value[0] = static_cast<int>(peak + 0.5f);
  g_cal_value[1] = static_cast<int>(paper + 0.5f);
  g_cal_touched[0] = g_cal_touched[1] = false;
  g_cal_sel = 0;
  g_cal = true;
  ss_hdr_calibrating(true);
  ss_log("[menu] HDR calibration opened (peak %d, paper white %d nits)", g_cal_value[0], g_cal_value[1]);
  return true;
}

bool ss_menu_wanted() {
  if (g_enabled < 0) {
    std::string v = ss_conf("menu");
    if (const char* e = getenv("SS_MENU")) v = e;
    g_enabled = ss_conf_present() && !Falsy(v) ? 1 : 0;
    ss_log("[menu] in-game settings %s (F1, or hold Back/View for 0.7 s)", g_enabled ? "on" : "off (menu = false)");
  }
  return g_enabled == 1;
}

namespace {

// Test aid, SS_MENU_TEST_CAL=<second> only (headless runs cannot work the overlay precisely with injected keys): at
// that second after the first present the overlay opens on the calibration screen, 3 s later PEAK goes up two steps,
// at 8 s the screen is left (the change saved), at 11 s the overlay closes.
void TestCalibration(void* device) {
  static int at = -2, stage = 0;
  static DWORD t0 = 0;
  if (at == -2) {
    const char* e = getenv("SS_MENU_TEST_CAL");
    at = e ? atoi(e) : -1;
    t0 = GetTickCount();
  }
  if (at < 0 || stage > 3) return;
  const int secs = static_cast<int>((GetTickCount() - t0) / 1000);
  if (stage == 0 && secs >= at) {
    stage = 1;
    ss_hdr_menu_rows(device);
    if (!g_open) Open("test aid");
    if (g_open) ss_menu_calibration("hdr_calibrate", "");
  } else if (stage == 1 && secs >= at + 3) {
    stage = 2;
    Input in = {};
    in.right = true;
    g_cal_sel = 0;
    if (g_cal) NavigateCal(in), NavigateCal(in);
  } else if (stage == 2 && secs >= at + 8) {
    stage = 3;
    CalClose();
  } else if (stage == 3 && secs >= at + 11) {
    stage = 4;
    if (g_open) Close("test aid");
  }
}

}  // namespace

namespace {

// The game's keyboard: its import of GetKeyboardState (see the top of the file).
constexpr uintptr_t kIatGetKeyboardState = 0xB9B2A8;
using GetKeyboardStateFn = BOOL(WINAPI*)(PBYTE);
GetKeyboardStateFn g_game_gks = nullptr;

BOOL WINAPI QuietGetKeyboardState(PBYTE state) {
  const BOOL r = g_game_gks(state);
  if (r && state && ss_menu_capturing()) memset(state, 0, 256);
  return r;
}

void HookGameKeyboard() {
  if (reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)) != 0x400000) return;
  HMODULE u = GetModuleHandleA("user32.dll");
  const uintptr_t real = u ? reinterpret_cast<uintptr_t>(GetProcAddress(u, "GetKeyboardState")) : 0;
  const uintptr_t cur = *reinterpret_cast<const volatile uintptr_t*>(kIatGetKeyboardState);
  if (!real || cur != real) {
    ss_log("[menu] the game's GetKeyboardState import holds %08lx, not user32's %08lx; its keyboard is not quieted while the menu is open",
           static_cast<unsigned long>(cur), static_cast<unsigned long>(real));
    return;
  }
  g_game_gks = reinterpret_cast<GetKeyboardStateFn>(real);
  if (!ss_write_word(reinterpret_cast<void*>(kIatGetKeyboardState), reinterpret_cast<uintptr_t>(&QuietGetKeyboardState))) {
    ss_log("[menu] cannot write the game's GetKeyboardState import; its keyboard is not quieted while the menu is open");
    return;
  }
  ss_log("[menu] the game's keyboard (GetKeyboardState import %p) reads nothing pressed while the menu is open",
         reinterpret_cast<void*>(kIatGetKeyboardState));
}

}  // namespace

void ss_menu_present(void* device) {
  if (g_enabled != 1 && !ss_menu_wanted()) return;
  static bool kb_tried = false;
  if (!kb_tried) {
    kb_tried = true;
    HookGameKeyboard();
  }
  TestCalibration(device);
  if (!g_open) {
    if (g_latch && !AnythingHeld()) g_latch = false;
    if (g_failed) return;
    // Closed: one key and one pad check.
    static bool f1_prev = false;
    static DWORD back_t0 = 0;
    const bool f1 = KeyHeld(VK_F1) && GameInFront();
    const bool f1_edge = f1 && !f1_prev;
    f1_prev = f1;
    const unsigned short b = ReadPads().buttons;
    bool chord = false;
    if (b == kBack) {
      const DWORD now = GetTickCount();
      if (!back_t0) back_t0 = now;
      else if (now - back_t0 >= 700) chord = true;
    } else {
      back_t0 = 0;
    }
    const char* asked = static_cast<const char*>(InterlockedExchangePointer(&g_request, nullptr));
    if (!f1_edge && !chord && !asked) return;
    back_t0 = 0;
    g_used_pad = asked ? (b & kY) != 0 : chord;  // the Options screen's Y: a pad when a pad's Y is down, else its key
    ss_hdr_menu_rows(device);  // the HDR rows, where the swap chain can do HDR10 (looked at when the menu first opens)
    Open(asked ? asked : chord ? "Back held" : "F1");
    if (!g_open) return;
  } else {
    Navigate(ReadInput());
  }
  if (!g_open) {
    ReleaseGfx();  // nothing of the menu stays on the device while it is closed
    return;
  }
  IDirect3DDevice9* dev = static_cast<IDirect3DDevice9*>(device);
  if (dev->TestCooperativeLevel() != D3D_OK) return;  // lost: draw again once the game has reset it
  Draw(dev);
  if (g_failed) {
    g_open = false;
    g_latch = true;
  }
}

void ss_menu_pad_state(int index, unsigned short buttons, float lx, float ly) {
  if (index < 0 || index > 3) return;
  PadSeen& s = g_pads[index];
  s.buttons = buttons;
  s.lx = lx;
  s.ly = ly;
  s.when = GetTickCount() | 1;
}

bool ss_menu_capturing() { return g_open || g_latch; }

void ss_menu_request_open(const char* how) {
  if (g_open || g_failed) return;
  InterlockedExchangePointer(&g_request, const_cast<char*>(how ? how : "asked"));
}

void* ss_menu_wrap_device(void* device) {
  if (!device) return device;
  auto* d = static_cast<IDirectInputDevice8A*>(device);
  DIDEVICEINSTANCEA info;
  memset(&info, 0, sizeof info);
  info.dwSize = sizeof info;
  if (FAILED(d->GetDeviceInfo(&info))) return device;
  const BYTE type = GET_DIDEVICE_TYPE(info.dwDevType);
  if (type != DI8DEVTYPE_KEYBOARD && type != DI8DEVTYPE_MOUSE) return device;
  ss_log("[menu] game's %s '%s' wrapped (quiet while the menu is open)", type == DI8DEVTYPE_KEYBOARD ? "keyboard" : "mouse",
         info.tszInstanceName);
  return static_cast<IDirectInputDevice8A*>(new QuietDevice(d, type == DI8DEVTYPE_KEYBOARD));
}
