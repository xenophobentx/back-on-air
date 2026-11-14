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
void ss_renderer_start();                 // renderer.cpp: renderer = dxvk | native (Windows), before anything makes an IDirect3D9
bool ss_renderer_dxvk();                  // renderer.cpp: the game draws with DXVK on Windows (dxvk folder, or a d3d9.dll beside the exe)
void ss_dxvkwin_start();                  // dxvkwin.cpp: dxvk_fit, DXVK on Windows inside the exe's 2 GB
void ss_dxvkwin_hold();                   // dxvkwin.cpp: the kept IDirect3D9 is made (display.cpp, before its own object)
void ss_audio_start();                    // audio.cpp: starts its own thread
void ss_saves_start(void* self);
void ss_overlay_start();                  // overlay.cpp: overlay_dir, dlc_tracks, dlc_save_profile (import-slot hooks; its own thread)          // saves.cpp: save_backups, the save folder copied beside the DLL (its own thread)
// Writes one aligned pointer-sized word in read-only memory (vtable slots in d3d9 / fmod / the game's .rdata).
// Every module goes through this one lock, so two threads never lift and restore the same page's protection over
// each other (the second write would hit a page already made read-only again). Keeps an executable page executable.
// Refuses (false, logged once) any word in the game's code: 0x401000..0xB9B000 at image base 0x400000, and inside
// the exe anything that is not in a section without IMAGE_SCN_MEM_EXECUTE (.rdata, .data, .idata, ...).
bool ss_write_word(void* where, uintptr_t value, uintptr_t* previous = nullptr);
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
void ss_slipstream_start();               // slipstream.cpp: slipstream_fix, the draft streaks' texture and definition (SSPC-31)
void* ss_shadowsmooth_original(void* ps);  // shadowsmooth.cpp: the game's shader behind one of our variants, else ps
void* ss_lodfade_original(void* ps);       // lodfade.cpp: the game's shader behind one of its dither variants, else ps
void ss_deband_start();                   // deband.cpp: the tone mapper's and grade's dither (its own thread when wanted)
void* ss_deband_original(void* ps);       // deband.cpp: the game's shader behind one of its dither variants, else ps
bool ss_deband_on();                      // deband.cpp: postfx.cpp's 8-bit writes add the same dither
float ss_lodfade_drawing(int* old);       // lodfade.cpp: the share of the masked draw under way (-1: none), *old = the old mesh
void ss_quality_present();                // quality.cpp: from the present hook: the props' draw distance
void ss_quality_scene_begin(void* device, bool jittered);  // quality.cpp: TAA's texture LOD bias from the update ...
void ss_quality_scene_end(void* device);                   // ... until the scene/UI split (postfx.cpp)
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
void ss_hdr_scene(void* device, void* final_surface);      // from postfx.cpp at the scene/UI split: the scene pass
void ss_hdr_frame_end(void* device, void* final_surface);  // from postfx.cpp at the game's EndScene: the mask pass
void ss_hdr_menu_rows(void* device);      // from menu.cpp when the overlay opens: registers the HDR rows where HDR can be had
bool ss_hdr_active();                     // the output is HDR10 now (or forced by the test aid)
void ss_hdr_calibrating(bool on);         // menu.cpp: the calibration screen is up (the output pass draws its patterns)
void ss_hdr_levels(float* peak, float* paper, float* hud);  // the levels in use, in nits
bool ss_hdr_apply(const char* key, const char* value);      // a key's value, live (hdr, hdr_peak, hdr_paper_white, ...)
bool ss_hdr_encoded(float* paper_white);  // testshot.cpp: the frame being presented holds PQ; the paper white it was made with
bool ss_hdr_test_lost();                  // test aid (SS_HDR_TEST_RESET): patch.cpp's Present hook answers D3DERR_DEVICELOST
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
// pipewarm.cpp: pipeline_warmup (SSPC-35): DXVK's graphics pipelines of a level made at its load screen.
bool ss_pipewarm_wanted();                // display.cpp hooks CreateDevice for it
void ss_pipewarm_start();                 // patch.cpp at start, after env.*: DXVK_CONFIG numCompilerThreads with pre-compile on
void ss_pipewarm_device(void* device);    // from display.cpp's CreateDevice wrap, before every other device hook
void ss_pipewarm_present(void* device);   // from patch.cpp's Present hook, before the overlay
// A pixel shader variant one of our parts made of a game shader (maker 1 lodfade, 2 shadowsmooth, 3 deband): named to
// pipewarm.cpp after the create, so a record can say which game shader it comes from and the replay can make it again.
void ss_pipewarm_derived(void* variant, void* original, int maker);
// The variant the current settings draw for a game pixel shader, made now if it was not (render thread; null: none,
// the game's own shader is drawn). For the replay of variants that are otherwise made only in the race.
void* ss_lodfade_variant(void* device, void* ps);
void* ss_shadowsmooth_variant(void* device, void* ps);
void* ss_deband_variant(void* device, void* ps);
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
