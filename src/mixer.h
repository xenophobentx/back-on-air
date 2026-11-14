// The audio mixer (audio.cpp): per-group volume on top of what the game asks for, in percent (0..200, 100 = as
// the game plays it). Safe to call from any thread at any time; a change is applied on the game's next audio
// update (within a frame), provided the mixer's hooks went in at start-up (they do whenever the overlay is wanted;
// they are never put in later). The initial values come from splitsecond.conf (volume_master, volume_music, ...).
#pragma once

extern "C" {
int ss_mixer_count();                 // number of groups
const char* ss_mixer_key(int group);  // conf key of the group, e.g. "volume_music" (null when out of range)
const char* ss_mixer_label(int group);  // short display name, e.g. "Music" (null when out of range)
int ss_mixer_get(int group);          // current gain in percent (100 when out of range)
void ss_mixer_set(int group, int pct);  // clamped to 0..200; applied live
}
