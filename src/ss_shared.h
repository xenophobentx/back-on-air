// Shared between patch.cpp (log, conf) and audio.cpp (reverb restore, SFX trim).
#pragma once
#include <cstdint>
#include <string>

void ss_log(const char* fmt, ...);        // one line into splitsecond-patch.log
std::string ss_conf(const char* key);     // value of a key in splitsecond.conf ("" when absent)
bool ss_conf_present();                   // splitsecond.conf exists next to the DLL
bool ss_patch_active();                   // a conf or an SS_* switch exists: false = pure forwarding, nothing started
void ss_log_raw(const char* text);        // lines already formatted ('\n' after each), written without stdio or heap (vm.cpp)
void ss_vm_start(void* self);             // vm.cpp: vm_log, the address-space log (a diagnostic, off by default)
void ss_vm_crash();                       // vm.cpp: from the crash line's handler
bool ss_hook_import(const char* module, const char* from, const char* name, uintptr_t hook, uintptr_t* original);  // vm.cpp
bool ss_hook_import_at(void* module, const char* from, const char* name, uintptr_t hook, uintptr_t* original);  // vm.cpp: by handle
void ss_renderer_start();                 // renderer.cpp: renderer = dxvk | native (Windows), before anything makes an IDirect3D9
bool ss_renderer_dxvk();                  // renderer.cpp: the game draws with DXVK on Windows (dxvk folder, or a d3d9.dll beside the exe)
void ss_dxvkwin_start();                  // dxvkwin.cpp: dxvk_fit, DXVK on Windows inside the exe's 2 GB
void ss_dxvkwin_hold();                   // dxvkwin.cpp: the kept IDirect3D9 is made (display.cpp, before its own object)
void ss_audio_start();                    // audio.cpp: starts its own thread
void ss_saves_start(void* self);
// saves.cpp: a backup kept for good before a save-changing switch (extras.cpp; see saves.cpp), and whether the save
// still holds its bytes (1 same, 0 changed, -1 not known).
enum { SAVES_KEPT_FAILED, SAVES_KEPT_MADE, SAVES_KEPT_SAME, SAVES_KEPT_NO_SAVE };
int ss_saves_backup_kept(const char* prefix, std::string* where);
int ss_saves_same_as_kept(const char* prefix);
void ss_extras_start();                   // extras.cpp: unlock_all, bonus_content, pure_decal, the decal actions (SSPC-55)
void ss_extras_present();                 // extras.cpp: from the present hook: the switches' bytes, the decal requests
const char* ss_extras_note(const char* key);  // extras.cpp: the overlay's help line for a row while it is held off
void ss_overlay_start();                  // overlay.cpp: overlay_dir, dlc_tracks, dlc_save_profile (import-slot hooks; its own thread)          // saves.cpp: save_backups, the save folder copied beside the DLL (its own thread)
// Writes one aligned pointer-sized word in read-only memory (vtable slots in d3d9 / fmod / the game's .rdata).
// Every module goes through this one lock, so two threads never lift and restore the same page's protection over
// each other (the second write would hit a page already made read-only again). Keeps an executable page executable.
// Refuses (false, logged once) any word in the game's code: 0x401000..0xB9B000 at image base 0x400000, and inside
// the exe anything that is not in a section without IMAGE_SCN_MEM_EXECUTE (.rdata, .data, .idata, ...).
bool ss_write_word(void* where, uintptr_t value, uintptr_t* previous = nullptr);
// tunnel.cpp: blocks until the game's import table (0xB9B000-0xB9B56C) is filled and quiet for 500 ms (30 s cap,
// then false, logged as [who]); every import slot written in the first seconds waits for it.
bool ss_import_table_wait(const char* who);
// patch.cpp: frame_pacing = smooth (vsync), vrr or auto: display.cpp makes and resets the device with
// PresentationInterval ONE, so every present gets its own vblank (decided once, at start).
bool ss_frame_vsync();
// patch.cpp: frame_pacing = fastest: display.cpp makes and resets the device with PresentationInterval IMMEDIATE.
bool ss_frame_immediate();
// display.cpp -> patch.cpp: the presentation interval the device was created / reset with (D3DPRESENT_INTERVAL_*).
void ss_frame_interval(unsigned interval);
void ss_testshot_surface(void* device, void* surface, const char* name);  // test aid: one 8-bit target saved as <name>.bmp
void ss_testshot_ask();                   // test aid: a back-buffer capture at the next present (key item "shot")
void ss_testshot(void* device);           // test aid: SS_SHOT_AT back-buffer captures (testshot.cpp)
void ss_quality_start();                  // quality.cpp: anisotropy and detail rows, its own thread
void ss_aspect_start();                   // aspect.cpp: picture shape and fov, starts its own thread
void ss_options_entry_start();            // options_entry.cpp: the PC SETTINGS prompt on the game's Options screen (its own thread)
void ss_lodfade_start();                  // lodfade.cpp: LOD smoothing (its own thread when wanted)
void ss_shadowsmooth_start();             // shadowsmooth.cpp: shadow smoothing (its own thread when wanted)
void ss_autopilot_start();                // autopilot.cpp: autopilot (test aid): the game's AI drives player one, lap / finish log
void ss_streampool_start();              // streampool.cpp: stream_pool / stream_pool_log, the sector streaming pool's size (SSPC-51)
void ss_streampool_present();            // streampool.cpp: from the present hook: the pool limit at a level load, the 480-node hold
void ss_slipstream_start();               // slipstream.cpp: slipstream_fix, the draft streaks' texture and definition (SSPC-31)
void ss_picture_start();                  // picture.cpp: motion_blur, vignette, screen_gradient, lens_flare, explosion_shake (SSPC-54)
bool ss_picture_clamp_wanted();           // picture.cpp: quality.cpp puts its SetSamplerState hook in for the blur's edge clamp
// mblur.cpp: motion_blur_quality = low | high, our own motion blur shader in the game's blur pass (SSPC-62)
bool ss_mblur_deck();                     // SteamDeck=1 in the environment
const char* ss_mblur_default();           // motion_blur_quality's default: "low" on the Steam Deck, else "high"
void ss_mblur_start();                    // picture.cpp's start: the setting and the SetPixelShader hook's thread
void ss_mblur_pass_begin();               // picture.cpp's BlurHook, around the game's pass (render thread)
void ss_mblur_pass_end();
bool ss_mblur_apply(const char* value);   // the settings menu (live); false while the hook is not in
void* ss_blur_original(void* ps);         // hdr.cpp: the game's blur program behind ours, else ps
bool ss_quality_edge_clamp(bool on);      // quality.cpp: render thread; samplers 0-7 clamped to the edge until called with false
double ss_frame_update_rate();            // patch.cpp: the game's update rate now (30, 60 or the race's fps)
bool ss_frame_wanted_above_30();          // patch.cpp: fps asks for 60 or more (read at start)
void* ss_shadowsmooth_original(void* ps);  // shadowsmooth.cpp: the game's shader behind one of our variants, else ps
void* ss_lodfade_original(void* ps);       // lodfade.cpp: the game's shader behind one of its dither variants, else ps
void ss_deband_start();                   // deband.cpp: the tone mapper's and grade's dither (its own thread when wanted)
void* ss_deband_original(void* ps);       // deband.cpp: the game's shader behind one of its dither variants, else ps
bool ss_deband_on();                      // deband.cpp: postfx.cpp's 8-bit writes add the same dither
float ss_lodfade_drawing(int* old);       // lodfade.cpp: the share of the masked draw under way (-1: none), *old = the old mesh
void ss_quality_present();                // quality.cpp: from the present hook: the props' draw distance
void ss_quality_scene_begin(void* device, bool jittered);  // quality.cpp: TAA's texture LOD bias from the update ...
void ss_quality_scene_end(void* device);                   // ... until the scene/UI split (postfx.cpp)
void ss_ppicon_start();                  // ppicon.cpp: the Power Play icons' size (GitHub #3)
void ss_ppicon_present();                // ppicon.cpp: from the present hook: the icons' scales for the back buffer
void ss_keyrepeat_start();               // keyrepeat.cpp: menu_key_repeat, a held arrow key repeats in the menus (SSPC-47)
void ss_keyrepeat_present();             // keyrepeat.cpp: from the present hook: the arrow keys on the menu repeat rows
void ss_keyrepeat_keys(unsigned char* state);  // keyrepeat.cpp: from the game's GetKeyboardState: arrows mirrored
bool ss_game_keyboard_hook();            // menu.cpp: the game's GetKeyboardState import taken (once); true when ours
void ss_kbhook_start();                  // kbhook.cpp: game_key_hook, the game's system-wide keyboard hook left out (#7)
void ss_lodfade_present();                // lodfade.cpp: from the present hook, before the frame is shown
void ss_aspect_after_update();            // aspect.cpp: widens the cameras the update just made
void ss_postfx_after_update();            // postfx.cpp: after the update (and aspect's), TAA's camera jitter
int ss_postfx_taa_frame();                // postfx.cpp: this frame is drawn jittered for TAA: the jitter's frame number, else -1
// hdr.cpp: HDR output (hdr, hdr_ready; nothing at all when both are off). docs/plan-hdr.md.
void ss_hdr_start();                      // reads the keys; also starts the probe (hdr_probe.cpp)
bool ss_hdr_wanted();                     // hdr or hdr_ready at start: postfx.cpp's frame hooks and display.cpp's wraps are needed
void ss_hdr_present(void* device);        // from patch.cpp's Present hook, after the overlay: the output pass
unsigned ss_hdr_backbuffer_format();      // the back buffer format display.cpp creates / resets the device with (0: the game's)
void ss_hdr_before_create(void* d3d9);    // from display.cpp's CreateDevice wrap, before the device is made
void ss_hdr_device_created(void* device, unsigned backbuffer_format);  // ... and after, before the game has the device
void ss_hdr_before_reset(void* device);   // from the Reset wraps: the passes' default-pool objects are released
void ss_hdr_after_reset(void* device);    // from display.cpp, after a Reset that succeeded
void ss_postfx_split(void* device, void* final_surface);   // postfx.cpp: hdr.cpp found the scene/UI split (earlier than postfx's own)
bool ss_camera_depth(float* a, float* b);  // postfx.cpp: the scene camera's depth = a + b / distance (false: no single perspective camera)
void ss_hdr_scene(void* device, void* final_surface);      // from postfx.cpp at the scene/UI split: the scene pass
void ss_hdr_frame_end(void* device, void* final_surface);  // from postfx.cpp at the game's EndScene: the mask pass
void ss_hdr_menu_rows(void* device);      // from menu.cpp when the overlay opens: registers the HDR rows where HDR can be had
bool ss_hdr_active();                     // the output is HDR10 now (or forced by the test aid)
void ss_hdr_calibrating(bool on);         // menu.cpp: the calibration screen is up (the output pass draws its patterns)
void ss_hdr_levels(float* peak, float* paper, float* hud);  // the levels in use, in nits
float ss_hdr_display_peak();              // the peak the display reports of itself, in nits (0: not known)
bool ss_hdr_apply(const char* key, const char* value);      // a key's value, live (hdr, hdr_peak, hdr_paper_white, ...)
bool ss_hdr_encoded(float* paper_white);  // testshot.cpp: the frame being presented holds PQ; the paper white it was made with
bool ss_hdr_test_lost();                  // test aid (SS_HDR_TEST_RESET): patch.cpp's Present hook answers D3DERR_DEVICELOST
// hdr.cpp -> diag.cpp: the patch's HDR state for the log's [summary] block (read from diag.cpp's thread; a value
// a frame old is fine there) and dxvk.allowFse as the start found it.
struct SsHdrState {
  bool wanted, on, ten_bit, described, ext, offered, active, broken;
  float peak, paper, display_peak;
};
void ss_hdr_state(SsHdrState* out);
const char* ss_hdr_allowfse();            // "True (set by the patch)", "set by the player", "not set", "not used (...)"
// diag.cpp: what a player's log needs to say why HDR looks wrong (Windows HDR, the window, DXVK's swap chain, the
// present request, overlays) and the [summary] block. Its own thread; nothing here changes how the game runs.
void ss_diag_device(void* hwnd);          // display.cpp, after the game's CreateDevice: starts the thread
void ss_diag_gpu(const char* name, unsigned vendor, const char* driver);  // display.cpp: the adapter, the real driver
void ss_diag_present();                   // patch.cpp's Present hook: the first frame (one flag test after that)
void ss_diag_hdr_event(bool on);          // hdr.cpp: HDR10 on / off (and set again after a Reset)
std::string ss_diag_present_text();       // frame_pacing, interval, window_mode, allowFse (hdr.cpp's "HDR10 on" line)
bool ss_diag_windows_hdr_off();           // Windows (not Proton) says for sure that HDR is off on the game's monitor
bool ss_file_version(void* module, unsigned v[4]);  // a loaded module's file version (its version resource)
const char* ss_display_window_mode();     // display.cpp: "fullscreen", "borderless" or "windowed"
const char* ss_frame_pacing_name();       // patch.cpp: frame_pacing as decided at start ("auto", "fastest", ...)
unsigned ss_frame_dev_interval();         // patch.cpp: the device's presentation interval (all ones: not known yet)
std::string ss_srccheck_system();         // srccheck.cpp: "Windows build 26300" or "Wine / Proton <version>"
// hdr_probe.cpp: the HDR feasibility probe (hdr_probe; nothing at all when off)
void ss_hdr_probe_start();
void ss_hdr_probe_present(void* device);
unsigned ss_hdr_probe_backbuffer_format();
void ss_hdr_probe_before_create(void* d3d9);
// glyphs.cpp: button_prompts, the button prompts for the pad in use.
bool ss_glyphs_wanted();                  // display.cpp hooks CreateDevice for it
void ss_glyphs_device(void* device);      // from display.cpp's CreateDevice wrap, before the game has the device
void ss_glyphs_present(void* device);     // from patch.cpp's Present hook
void ss_glyphs_before_reset();            // from display.cpp's Reset wrap: the default-pool textures it holds are let go
// shadowres.cpp: shadow_resolution (SSPC-50), the sun shadow atlas at 2x / 4x through the device's vtable.
bool ss_shadowres_wanted();               // display.cpp hooks CreateDevice for it
void ss_shadowres_device(void* device);   // from display.cpp's CreateDevice wrap, before the game has the device
void ss_shadowres_after_reset();          // from display.cpp's Reset wrap, after a Reset that worked
// pipewarm.cpp: pipeline_warmup (SSPC-35): DXVK's graphics pipelines of a level made at its load screen.
bool ss_pipewarm_wanted();                // display.cpp hooks CreateDevice for it
void ss_pipewarm_start();                 // patch.cpp at start, after env.*: DXVK_CONFIG numCompilerThreads with pre-compile on
void ss_pipewarm_device(void* device);    // from display.cpp's CreateDevice wrap, before every other device hook
void ss_pipewarm_present(void* device);   // from patch.cpp's Present hook, before the overlay
// A pixel shader variant one of our parts made of a game shader (maker 1 lodfade, 2 shadowsmooth, 3 deband, 4 mblur): named to
// pipewarm.cpp after the create, so a record can say which game shader it comes from and the replay can make it again.
void ss_pipewarm_derived(void* variant, void* original, int maker);
// The variant the current settings draw for a game pixel shader, made now if it was not (render thread; null: none,
// the game's own shader is drawn). For the replay of variants that are otherwise made only in the race.
void* ss_lodfade_variant(void* device, void* ps);
void* ss_shadowsmooth_variant(void* device, void* ps);
void* ss_deband_variant(void* device, void* ps);
void* ss_blur_variant(void* device, void* ps);   // maker 4 (mblur.cpp)
extern volatile int ss_game_state;        // patch.cpp: kStates index of the last frame step (1 level load, 5 race, 3/7 front end)
extern volatile long ss_own_drawing;      // menu.cpp: > 0 while menu.cpp / postfx.cpp draw on the game's device; other device hooks pass through
// Cost counters for the [frames] split line (patch.cpp; SSPC-17): every VirtualQuery and every VirtualProtect made on
// the frame's paths. Bumped without a lock (a plain add; another thread's bump can be lost now and then): they are
// a rate for the log, nothing reads them as exact. Defined in patch.cpp.
extern volatile unsigned long ss_vq_calls;
extern volatile unsigned long ss_vp_calls;
#define SS_VQ(...) (++ss_vq_calls, VirtualQuery(__VA_ARGS__))
// The readable-region cache of the frame (patch.cpp; SSPC-17, perf-plan A2), for the Readable() checks on the
// per-update and per-frame paths. Only the game's main thread uses it (any other thread: never known, nothing kept),
// and it is emptied at the start of every step, after the update and at every present, so a region is trusted
// only inside one phase of one frame. Regions of mapped files are never kept. SS_READ_CACHE=0 turns it off.
// ss_region_known: [p, p + n) lies inside one region a VirtualQuery found committed and readable in this phase.
// ss_region_note: after such a query (base, size and type as it returned them; the caller checked the protection).
bool ss_region_known(uintptr_t p, size_t n);
void ss_region_note(uintptr_t base, size_t size, unsigned long type);
// ss_region_bad: p lies in a region a VirtualQuery found not committed or not readable in this phase (the answer is
// "not readable" again, as a query a moment earlier gave). ss_region_note_bad: after such a query.
bool ss_region_bad(uintptr_t p);
void ss_region_note_bad(uintptr_t base, size_t size);
// Bumped whenever shadowsmooth.cpp, lodfade.cpp or deband.cpp maps a new variant to the game's shader behind it (the maps
// ss_*_original read): hdr.cpp keeps a cache of those answers and empties it on a change. Defined in patch.cpp.
extern volatile unsigned long ss_shader_map_gen;
