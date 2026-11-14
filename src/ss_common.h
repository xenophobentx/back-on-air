// Shared between patch.cpp (log, conf file) and pad.cpp (controller layer).
#pragma once
#include <string>

void ss_log(const char* fmt, ...);
std::string ss_conf(const char* key);  // value of a key in splitsecond.conf, "" when absent

// pad.cpp: the controller layer. proxy.cpp hands the real DirectInput8Create result to it.
long ss_pad_wrap_directinput(const void* riid, void** inout);
