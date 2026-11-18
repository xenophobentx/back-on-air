// Shared between patch.cpp (log, conf file) and pad.cpp (controller layer).
#pragma once
#include <string>

void ss_log(const char* fmt, ...);
std::string ss_conf(const char* key);  // value of a key in splitsecond.conf, "" when absent

// pad.cpp: the controller layer. proxy.cpp hands the real DirectInput8Create result to it.
long ss_pad_wrap_directinput(const void* riid, void** inout);

// srccheck.cpp: the start-up check (the exe, other mods, the patch's own files, overlay modules); patch.cpp runs it
// once the log is open. ss_exe_known: false when the exe is not the Steam build (nothing may be patched or hooked).
bool ss_srccheck_run(void* self, bool conf_present, bool boxes, bool test_unknown);
bool ss_exe_known();
std::string ss_srccheck_modules(const char* when);  // logs the known overlays (when: null, not logged); returns their short names
