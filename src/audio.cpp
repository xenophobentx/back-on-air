// Split/Second (PC, Steam build 385319): audio. Two optional parts, both done in the middleware's own DLLs
// (fmod_event.dll, FMOD Designer 4.28.x runtime, and fmodex.dll, FMOD Ex 4.28.09), never in the game's code:
//
//  1. reverb = true  The PC release ships its event files with the authored reverb presets overwritten by the
//     generic default preset (one small room everywhere). The game reads the presets through the event system
//     (EventSystem::getReverbPresetByIndex / getReverbPreset, virtual slots 0x70 and 0x6C of the object
//     FMOD_EventSystem_Create returns; calls at 0x8669DF, 0x866A5C and 0x866AB9 in the Steam image) and hands what it
//     got to System::setReverbProperties. We swap the two slots in fmod_event.dll's own vtable (its .rdata, not
//     the game's) for wrappers which call the original and then replace the returned parameters, by preset name,
//     with the authored ones (reverb_presets.h). The game's own blending and reverb calls carry them from there.
//
//  2. The mixer: volume_master / volume_music / volume_sfx / volume_engines / volume_voice / volume_ambience
//     (percent, default 100 = as the game plays it), one more gain per category on top (volume_player_engine,
//     volume_opponent_engines, volume_collisions, volume_powerplays, volume_swooshes, volume_surfaces,
//     volume_particles, volume_hyperreal, volume_camera, volume_hud, volume_general_ambience, volume_spot_ambience),
//     and the older sfx_trim (dB). Observed in the Steam image: at
//     audio init (0x868930..0x8689E5) the game takes the event system's "master" category and every top-level
//     category (EventSystem vtable +0x3C getCategory, +0x40 getCategoryByIndex), asks each for its FMOD channel group
//     (EventCategory vtable +0x40 getChannelGroup) and builds its own mix tree from them (0x868090, node constructor
//     0x86C930: name, channel group at node+0x8C). Every audio update the node (0x86C600) writes its fade / slider /
//     side-chain ducking value straight into that channel group with ChannelGroup::setVolume, imported from
//     fmodex.dll (calls at 0x86C6E1 and 0x86C7A4). EventCategory::setVolume is not on that path (it would land in the
//     same channel group and be overwritten on the next update), so the gain has to sit on ChannelGroup::setVolume.
//     That function is a plain export of fmodex.dll (no vtable): we put a 5-byte jump on its first instruction, in
//     fmodex.dll's memory (a third-party library beside the game), to a wrapper that multiplies the value by the
//     gain of the category's group when the channel group belongs to a known category, and passes everything else
//     through unchanged. ChannelGroup::getVolume is wrapped the same way so the game reads back its own value. The
//     category tree is walked through fmod_event.dll's exported methods from inside EventSystem::update (vtable slot
//     swap, as before), logged, and mapped to the groups by name (mixer_map overrides the mapping).
//     FMOD clamps every channel group volume to 1.0, so a gain above 100 only gives back what the game itself takes
//     away (its sliders below the top, fades, ducking); it cannot make a category louder than full scale.
//     The hooks go in once, at start-up, and only when needed: a group not at 100, sfx_trim, mixer = true, or the
//     in-game overlay wanted (so it can move a volume later). They are never put in late, while the game plays.
//
// Nothing is written into SplitSecond.exe's image: no code, no read-only data, no import entry. The vtable slots
// and the jump that are changed live in fmod_event.dll and fmodex.dll, third-party libraries shipped next to the game.
#include <windows.h>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "menu.h"
#include "mixer.h"
#include "console_mix_table.h"
#include "reverb_presets.h"
#include "ss_shared.h"

// Called by code built with another compiler: do not trust the stack alignment.
#define HOOK __attribute__((stdcall, force_align_arg_pointer))

namespace {

using ssrev::ReverbProps;

// ------------------------------------------------------------------ config

bool ParseBool(const std::string& v, bool dflt) {
  if (v.empty()) return dflt;
  return v == "1" || v == "true" || v == "yes" || v == "on";
}

bool WantConsoleMix() {
  if (const char* e = getenv("SS_CONSOLE_MIX")) return ParseBool(e, true);
  if (!ss_conf_present()) return false;  // no conf file: the DLL does nothing
  return ParseBool(ss_conf("console_mix"), true);  // default true (XBOX 360) since 2026-11-13, a missing key included
}

bool WantSidechain() {
  // Default on: measured on a race, it ducks exactly as DynamicMixer.xml sets out (the shipped design).
  if (const char* e = getenv("SS_AUDIO_SIDECHAIN")) return ParseBool(e, true);
  if (!ss_conf_present()) return false;
  return ParseBool(ss_conf("audio_sidechain"), true);
}

bool WantSoftwareMix() {
  // Default off: measured, the speaker spread is the same with and without hardware voices (not Wine's doing).
  if (const char* e = getenv("SS_AUDIO_SOFTWARE_MIX")) return ParseBool(e, false);
  if (!ss_conf_present()) return false;
  return ParseBool(ss_conf("audio_software_mix"), false);
}

bool WantVoiceDsp() {
  if (const char* e = getenv("SS_AUDIO_VOICE_DSP")) return ParseBool(e, true);
  if (!ss_conf_present()) return false;
  return ParseBool(ss_conf("audio_voice_dsp"), true);
}

bool WantSpeakerFix() {
  if (const char* e = getenv("SS_AUDIO_SPEAKER_FIX")) return ParseBool(e, true);
  if (!ss_conf_present()) return false;
  return ParseBool(ss_conf("audio_speaker_fix"), true);
}

bool WantReverb() {
  if (const char* e = getenv("SS_REVERB")) return ParseBool(e, true);
  if (!ss_conf_present()) return false;  // no conf file: the DLL does nothing, as for the frame rate
  return ParseBool(ss_conf("reverb"), true);
}

bool WantFsbFix() {
  if (const char* e = getenv("SS_AUDIO_FSB_FIX")) return ParseBool(e, true);
  if (!ss_conf_present()) return false;
  return ParseBool(ss_conf("audio_fsb_fix"), true);
}

double WantSfxTrimDb() {
  std::string v;
  if (const char* e = getenv("SS_SFX_TRIM")) v = e; else v = ss_conf("sfx_trim");
  if (v.empty()) return 0.0;
  double db = atof(v.c_str());
  if (db > 0.0) db = 0.0;      // a trim only lowers
  if (db < -24.0) db = -24.0;
  return db;
}

// ------------------------------------------------------------ vtable hooks

typedef long(__stdcall* PfnGetPreset)(void* self, const char* name, ReverbProps* props, int* index);
typedef long(__stdcall* PfnGetPresetByIndex)(void* self, int index, ReverbProps* props, char** name);
typedef long(__stdcall* PfnUpdate)(void* self);
typedef long(__stdcall* PfnGetCategory)(void* self, const char* name, void** cat);
typedef long(__stdcall* PfnSetVolume)(void* self, float v);
typedef long(__stdcall* PfnGetVolume)(void* self, float* v);
typedef long(__stdcall* PfnCatInfo)(void* self, int* index, char** name);
typedef long(__stdcall* PfnCatNum)(void* self, int* n);
typedef long(__stdcall* PfnCatByIndex)(void* self, int i, void** cat);
typedef long(__stdcall* PfnCatParent)(void* self, void** cat);
typedef long(__stdcall* PfnCatGroup)(void* self, void** channel_group);

PfnGetPreset g_orig_get_preset = nullptr;
PfnGetPresetByIndex g_orig_by_index = nullptr;
PfnUpdate g_orig_update = nullptr;
PfnGetCategory g_orig_get_category = nullptr;
PfnSetVolume g_cg_set = nullptr;  // fmodex.dll ChannelGroup::setVolume / getVolume, through the trampolines
PfnGetVolume g_cg_get = nullptr;
PfnCatInfo g_cat_info = nullptr;
PfnCatNum g_cat_num = nullptr;
PfnCatByIndex g_cat_by_index = nullptr;
PfnCatParent g_cat_parent = nullptr;
PfnCatGroup g_cat_group = nullptr;

// Finds the vtable slot of fmod_event.dll that holds `fn` (a pointer into the DLL's own code, taken from its
// export table). The slot must sit in a non-executable section and, when given, its neighbours must be the
// expected functions. Returns null unless exactly one such slot exists.
void** FindSlot(HMODULE mod, void* fn, void* prev, void* next) {
  auto base = reinterpret_cast<uint8_t*>(mod);
  auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
  auto sec = IMAGE_FIRST_SECTION(nt);
  void** found = nullptr;
  int hits = 0;
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
    if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
    if (!memcmp(sec[i].Name, ".reloc", 6) || !memcmp(sec[i].Name, ".rsrc", 5)) continue;
    auto p = reinterpret_cast<void**>(base + sec[i].VirtualAddress);
    size_t n = sec[i].Misc.VirtualSize / sizeof(void*);
    for (size_t k = 1; k + 1 < n; ++k) {
      if (p[k] != fn) continue;
      if (prev && p[k - 1] != prev) continue;
      if (next && p[k + 1] != next) continue;
      found = &p[k];
      ++hits;
    }
  }
  return hits == 1 ? found : nullptr;
}

bool SwapSlot(void** slot, void* hook, void** orig) {
  *orig = *slot;  // set before the slot points at us
  return ss_write_word(slot, reinterpret_cast<uintptr_t>(hook));
}

// -------------------------------------------------------------- reverb part

bool g_reverb_on = false;
char g_logged[48][40];
int g_nlogged = 0;

bool FirstTime(const char* name) {
  for (int i = 0; i < g_nlogged; ++i)
    if (!strncmp(g_logged[i], name, 39)) return false;
  if (g_nlogged < 48) { strncpy(g_logged[g_nlogged], name, 39); g_logged[g_nlogged][39] = 0; ++g_nlogged; }
  return true;
}

void Substitute(const char* name, ReverbProps* props, const char* how) {
  const ReverbProps* t = ssrev::FindReverbPreset(name);
  if (!t) {
    if (FirstTime(name))
      ss_log("[reverb] preset '%s' (%s) has no authored data here: kept (Room %d RoomHF %d decay %.2f)", name, how,
             props->Room, props->RoomHF, props->DecayTime);
    return;
  }
  if (FirstTime(name))
    ss_log("[reverb] preset '%s' (%s): Room %d RoomHF %d decay %.2f HFratio %.2f refl %d -> Room %d RoomHF %d decay %.2f "
           "HFratio %.2f refl %d%s",
           name, how, props->Room, props->RoomHF, props->DecayTime, props->DecayHFRatio, props->Reflections, t->Room,
           t->RoomHF, t->DecayTime, t->DecayHFRatio, t->Reflections, memcmp(props, t, sizeof *t) ? "" : " (already authored)");
  memcpy(props, t, sizeof *t);
}

HOOK long HkGetPresetByIndex(void* self, int index, ReverbProps* props, char** name) {
  long r = g_orig_by_index(self, index, props, name);
  if (r == 0 && g_reverb_on && props && name && *name) Substitute(*name, props, "by index");
  return r;
}

HOOK long HkGetPreset(void* self, const char* name, ReverbProps* props, int* index) {
  long r = g_orig_get_preset(self, name, props, index);
  if (r == 0 && g_reverb_on && props && name) Substitute(name, props, "by name");
  return r;
}

// ------------------------------------------------------------------ the mixer

// A group is one conf key / one overlay row; its categories are matched by name (any case). "*" takes every
// top-level category (direct child of "master") that no other group names, i.e. the game's own SFX slider set.
// Category tree of the PC event files (ReverbAndGroups bank, a flat tree, every per-level bank a subset of it):
// master{music, PlayerEngine, CPUEngine, Swooshes, Surfaces, Powerplays, Collisions, HUD, Ambience, Particles,
// Spot_Ambience, HyperReal, Speech, Camera} (+ Rumble in one bank). The startup log shows the tree really loaded.
//
// The first six are the coarse groups. The rest are one per category ("detail" groups): a second gain on that one
// category's channel group, on top of its coarse group (engines x player engine, ...). The game's tree has no level
// below these categories, so this is as fine as the mix goes without touching single events.
struct Group {
  const char* key;    // conf key
  const char* name;   // name in mixer_map
  const char* label;  // overlay label
  const char* dflt;   // default categories
  volatile LONG pct;
  bool detail;        // a per-category gain, multiplied with the coarse group's
};
Group g_groups[] = {
    {"volume_master", "master", "Master", "master", 100, false},
    {"volume_music", "music", "Music", "music", 100, false},
    {"volume_sfx", "sfx", "Effects", "*", 100, false},
    {"volume_engines", "engines", "Engines", "PlayerEngine,CPUEngine", 100, false},
    {"volume_voice", "voice", "Voice", "Speech", 100, false},
    {"volume_ambience", "ambience", "Ambience", "Ambience,Spot_Ambience", 100, false},
    {"volume_player_engine", "player_engine", "Player engine", "PlayerEngine", 100, true},
    {"volume_opponent_engines", "opponent_engines", "Opponent engines", "CPUEngine", 100, true},
    {"volume_collisions", "collisions", "Collisions", "Collisions", 100, true},
    {"volume_powerplays", "powerplays", "Power Plays", "Powerplays", 100, true},
    {"volume_swooshes", "swooshes", "Swooshes", "Swooshes", 100, true},
    {"volume_surfaces", "surfaces", "Surfaces", "Surfaces", 100, true},
    {"volume_particles", "particles", "Particles", "Particles", 100, true},
    {"volume_hyperreal", "hyperreal", "HyperReal", "HyperReal", 100, true},
    {"volume_camera", "camera", "Camera", "Camera", 100, true},
    {"volume_hud", "hud", "HUD", "HUD", 100, true},
    {"volume_general_ambience", "general_ambience", "General ambience", "Ambience", 100, true},
    {"volume_spot_ambience", "spot_ambience", "Spot ambience", "Spot_Ambience", 100, true},
};
constexpr int kGroups = sizeof(g_groups) / sizeof(g_groups[0]);
std::string g_map[kGroups];  // per group ",name,name," in lower case

double g_trim_gain = 1.0;  // sfx_trim, kept as it was: a flat trim on the old selection of top-level categories
double g_trim_db = 0.0;
std::string g_trim_list;   // sfx_trim_categories: comma separated names; empty = every top-level category but the excluded

std::string Lower(std::string s) {
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  return s;
}

std::string NoSpaces(const std::string& s) {
  std::string o;
  for (char c : s)
    if (c != ' ' && c != '\t' && c != '\r') o += c;
  return o;
}

bool IsTrimmed(const std::string& name_in) {
  std::string name = Lower(name_in);
  if (!g_trim_list.empty()) {
    std::string list = "," + Lower(NoSpaces(g_trim_list)) + ",";
    return list.find("," + name + ",") != std::string::npos;
  }
  static const char* const kNot[] = {"master", "music", "speech", "voice", "vo", "hud", "frontend", "menu", "ui",
                                     "loading", "dialog", "commentary"};
  for (const char* n : kNot)
    if (name.find(n) != std::string::npos) return false;
  return true;
}

int ParsePct(const char* env, const char* key) {
  std::string v;
  if (const char* e = getenv(env)) v = e; else v = ss_conf(key);
  v = NoSpaces(v);
  if (v.empty()) return 100;
  if (v.back() == '%') v.pop_back();
  int p = atoi(v.c_str());
  return p < 0 ? 0 : p > 200 ? 200 : p;
}

// mixer_map = group:cat1,cat2;group:cat3   replaces the categories of the groups it names.
void ParseMap(const std::string& spec) {
  for (int g = 0; g < kGroups; ++g) g_map[g] = "," + Lower(g_groups[g].dflt) + ",";
  std::string s = NoSpaces(spec);
  size_t p = 0;
  while (p < s.size()) {
    size_t e = s.find(';', p);
    if (e == std::string::npos) e = s.size();
    std::string item = s.substr(p, e - p);
    p = e + 1;
    if (item.empty()) continue;
    size_t colon = item.find(':');
    std::string g = Lower(item.substr(0, colon));
    if (g.compare(0, 7, "volume_") == 0) g = g.substr(7);
    int gi = -1;
    for (int k = 0; k < kGroups; ++k)
      if (g == g_groups[k].name) gi = k;
    if (gi < 0 || colon == std::string::npos) {
      ss_log("[mixer] mixer_map: '%s' not understood (group:category,category); ignored", item.c_str());
      continue;
    }
    g_map[gi] = "," + Lower(item.substr(colon + 1)) + ",";
  }
}

// One category whose channel group carries a gain. `req` is what the game (or fmod_event.dll itself, through
// EventCategory::setVolume) last asked of that channel group.
struct Cat {
  void* cg;
  int group;   // -1: only the sfx_trim / detail gain applies
  int detail;  // detail group of this category, -1 none
  bool trim;
  bool have_req;
  float req;
  char name[32];
};
constexpr int kMaxCats = 64;
Cat g_cats[kMaxCats];
int g_ncats = 0;
CRITICAL_SECTION g_cs;

void* g_event_system = nullptr;
volatile LONG g_update_count = 0;
volatile LONG g_dirty = 0;         // a gain changed: walk and apply on the next update
volatile LONG g_update_state = 0;  // EventSystem::update slot: 0 untouched, 1 installing, 2 wrapped, -1 failed
volatile LONG g_detour_state = 0;  // ChannelGroup set/getVolume: 0 untouched, 1 installing, 2 wrapped, -1 failed
volatile LONG g_mixer_ready = 0;   // exports resolved, the hooks can go in
void** g_update_slot = nullptr;
void* g_fx_set = nullptr;  // fmodex.dll ChannelGroup::setVolume / getVolume (the exported code)
void* g_fx_get = nullptr;

Cat* FindCat(void* cg) {
  for (int i = 0; i < g_ncats; ++i)
    if (g_cats[i].cg == cg) return &g_cats[i];
  return nullptr;
}

double CatGain(const Cat& c) {
  double g = 1.0;
  if (c.group >= 0) g *= g_groups[c.group].pct / 100.0;
  if (c.detail >= 0) g *= g_groups[c.detail].pct / 100.0;
  if (c.trim) g *= g_trim_gain;
  return g;
}

float Scaled(float v, double gain) {
  if (gain == 1.0) return v;
  double o = v * gain;
  if (o > 1.0) o = 1.0;  // FMOD clamps a channel group volume to 0..1 anyway
  if (o < 0.0) o = 0.0;
  return static_cast<float>(o);
}

volatile LONG g_music_log_cg = 0;  // audio_music_log: the categories' requests are logged
void MlGroupVolume(void* cg, const char* cname, float v);

HOOK long HkCgSetVolume(void* cg, float v) {
  float out = v;
  char cname[32];
  cname[0] = 0;
  EnterCriticalSection(&g_cs);
  if (Cat* c = FindCat(cg)) {
    c->req = v;
    c->have_req = true;
    out = Scaled(v, CatGain(*c));
    if (g_music_log_cg) memcpy(cname, c->name, sizeof cname);
  }
  LeaveCriticalSection(&g_cs);
  if (cname[0]) MlGroupVolume(cg, cname, v);
  return g_cg_set(cg, out);
}

HOOK long HkCgGetVolume(void* cg, float* v) {
  long r = g_cg_get(cg, v);
  if (r == 0 && v) {
    EnterCriticalSection(&g_cs);
    Cat* c = FindCat(cg);
    if (c && c->have_req) *v = c->req;  // the game reads back its own value, never the scaled one
    LeaveCriticalSection(&g_cs);
  }
  return r;
}

int GroupOf(const std::string& lname, bool top, bool detail = false) {
  for (int g = 0; g < kGroups; ++g)
    if (g_groups[g].detail == detail && g_map[g].find("," + lname + ",") != std::string::npos) return g;
  if (top && !detail)
    for (int g = 0; g < kGroups; ++g)
      if (!g_groups[g].detail && g_map[g].find(",*,") != std::string::npos) return g;
  return -1;
}

// Depth first through the category tree. A category gets its own entry when it maps to a group other than the one
// it already inherits (children multiply with their parents in FMOD, so a gain is never counted twice) or when the
// sfx_trim selects it (top level only, as before).
void Visit(void* cat, int depth, int inherited, bool top, std::string& tree, Cat* out, int& n) {
  char* nm = nullptr;
  int idx = 0;
  if (g_cat_info(cat, &idx, &nm) != 0 || !nm) nm = const_cast<char*>("?");
  tree += nm;
  int g = GroupOf(Lower(nm), top);
  int own = (g >= 0 && g != inherited) ? g : -1;
  bool trim = top && g_trim_gain != 1.0 && IsTrimmed(nm);
  const int det = GroupOf(Lower(nm), top, true);
  void* cg = nullptr;
  if ((own >= 0 || trim || det >= 0) && n < kMaxCats && g_cat_group(cat, &cg) == 0 && cg) {
    Cat& c = out[n++];
    memset(&c, 0, sizeof c);
    c.cg = cg;
    c.group = own;
    c.detail = det;
    c.trim = trim;
    strncpy(c.name, nm, sizeof c.name - 1);
  }
  int kids = 0;
  if (depth < 6 && g_cat_num(cat, &kids) == 0 && kids > 0) {
    tree += "{";
    for (int i = 0; i < kids; ++i) {
      void* c = nullptr;
      if (g_cat_by_index(cat, i, &c) != 0 || !c) continue;
      if (i) tree += ", ";
      Visit(c, depth + 1, own >= 0 ? own : inherited, depth == 0, tree, out, n);
    }
    tree += "}";
  }
}

// Called on the thread that runs EventSystem::update. Rebuilds the table from the categories that exist now
// (banks loaded later add some), keeping the game's last request per channel group.
bool Walk() {
  void* master = nullptr;
  if (!g_event_system || g_orig_get_category(g_event_system, "master", &master) != 0 || !master) return false;
  static Cat found[kMaxCats];
  int n = 0;
  std::string tree;
  Visit(master, 0, -1, false, tree, found, n);
  for (int i = 0; i < n; ++i) {
    EnterCriticalSection(&g_cs);
    Cat* old = FindCat(found[i].cg);
    if (old && old->have_req) { found[i].have_req = true; found[i].req = old->req; }
    LeaveCriticalSection(&g_cs);
    float v = 1.0f;  // never scaled by us yet: what the channel group holds is the game's own value
    if (!found[i].have_req && g_cg_get(found[i].cg, &v) == 0) { found[i].have_req = true; found[i].req = v; }
  }
  EnterCriticalSection(&g_cs);
  memcpy(g_cats, found, sizeof(Cat) * n);
  g_ncats = n;
  LeaveCriticalSection(&g_cs);

  static std::string last_tree;
  static int ntree_logs = 0;
  if (tree != last_tree && ntree_logs < 8) {
    last_tree = tree;
    ++ntree_logs;
    ss_log("[mixer] category tree: %s", tree.c_str());
    std::string m;
    for (int i = 0; i < n; ++i) {
      char b[96];
      snprintf(b, sizeof b, "%s%s -> %s%s%s%s", i ? ", " : "", found[i].name,
               found[i].group >= 0 ? g_groups[found[i].group].name : "-", found[i].detail >= 0 ? " x " : "",
               found[i].detail >= 0 ? g_groups[found[i].detail].name : "", found[i].trim ? " +sfx_trim" : "");
      m += b;
    }
    ss_log("[mixer] mapping: %s", n ? m.c_str() : "(no category carries a gain)");
  }
  return true;
}

void ApplyAll() {
  EnterCriticalSection(&g_cs);
  for (int i = 0; i < g_ncats; ++i)
    if (g_cats[i].have_req) g_cg_set(g_cats[i].cg, Scaled(g_cats[i].req, CatGain(g_cats[i])));
  LeaveCriticalSection(&g_cs);
}

// A 5-byte jump on the first instruction of an fmodex.dll export, with a trampoline for the original. Only for the
// prologue this build has (push ebp / mov ebp,esp / lea eax,[ebp+8]: 6 bytes, nothing position dependent).
bool Detour(void* target, void* hook, void** tramp_out, const char* what) {
  static const uint8_t kPrologue[6] = {0x55, 0x8B, 0xEC, 0x8D, 0x45, 0x08};
  auto t = static_cast<uint8_t*>(target);
  HMODULE owner = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(t), &owner) ||
      !owner || owner != GetModuleHandleA("fmodex.dll")) {
    ss_log("[mixer] %s at %p is not in fmodex.dll's own code: nothing done", what, target);
    return false;
  }
  if (memcmp(t, kPrologue, sizeof kPrologue)) {
    ss_log("[mixer] fmodex.dll %s starts %02X %02X %02X %02X %02X %02X, not the known prologue: nothing done", what,
           t[0], t[1], t[2], t[3], t[4], t[5]);
    return false;
  }
  auto tr = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!tr) return false;
  memcpy(tr, t, 6);
  tr[6] = 0xE9;
  int32_t back = static_cast<int32_t>((t + 6) - (tr + 11));
  memcpy(tr + 7, &back, 4);
  FlushInstructionCache(GetCurrentProcess(), tr, 16);
  *tramp_out = tr;  // the original is reachable before the jump goes in
  // The qword the 5 bytes sit in: the aligned one when it holds them all, else the one starting at the entry. The
  // other 3 bytes are written back with their own value, in the same locked compare-exchange.
  const uintptr_t ta = reinterpret_cast<uintptr_t>(t);
  uint8_t* q = (ta & 7) <= 3 ? reinterpret_cast<uint8_t*>(ta & ~static_cast<uintptr_t>(7)) : t;
  const size_t at = static_cast<size_t>(t - q);
  DWORD old;
  if (!VirtualProtect(q, 8, PAGE_EXECUTE_READWRITE, &old)) {
    ss_log("[mixer] fmodex.dll %s: cannot unprotect its entry (error %lu): nothing done", what, GetLastError());
    VirtualFree(tr, 0, MEM_RELEASE);
    *tramp_out = nullptr;
    return false;
  }
  LONGLONG cur, now;
  memcpy(&cur, q, 8);
  uint8_t nb[8];
  memcpy(nb, &cur, 8);
  nb[at] = 0xE9;
  int32_t rel = static_cast<int32_t>(static_cast<uint8_t*>(hook) - (t + 5));
  memcpy(nb + at + 1, &rel, 4);
  memcpy(&now, nb, 8);
  const bool ok = InterlockedCompareExchange64(reinterpret_cast<volatile LONGLONG*>(q), now, cur) == cur;  // one locked write
  FlushInstructionCache(GetCurrentProcess(), q, 8);
  DWORD dummy;
  VirtualProtect(q, 8, old, &dummy);
  if (!ok) {
    ss_log("[mixer] fmodex.dll %s changed while the jump was going in: nothing done", what);
    VirtualFree(tr, 0, MEM_RELEASE);
    *tramp_out = nullptr;
    return false;
  }
  return true;
}

void InstallDetours() {
  if (!g_mixer_ready || InterlockedCompareExchange(&g_detour_state, 1, 0) != 0) return;
  bool ok = Detour(g_fx_get, (void*)HkCgGetVolume, (void**)&g_cg_get, "ChannelGroup::getVolume") &&
            Detour(g_fx_set, (void*)HkCgSetVolume, (void**)&g_cg_set, "ChannelGroup::setVolume");
  g_detour_state = ok ? 2 : -1;
  ss_log(ok ? "[mixer] fmodex.dll ChannelGroup::setVolume/getVolume wrapped"
            : "[mixer] the channel group wrappers could not go in: gains stay off");
  if (ok) InterlockedExchange(&g_dirty, 1);
}

// ------------------------------------------------------------- diagnostics (audio_diag = true)

// What fmodex.dll's DSP network holds (every unit once: type, name, active/bypass, parameters), walked from the system's
// DSP head through FMOD's own read-only exports, and every change of what the game asks of the categories' channel
// groups (its fades, presets and side-chain ducking), in dB. Read only; on the thread that runs EventSystem::update.
volatile LONG g_diag = 0;

template <typename F>
F FxSym(const char* sym) {
  static HMODULE fx = GetModuleHandleA("fmodex.dll");
  return fx ? reinterpret_cast<F>(GetProcAddress(fx, sym)) : nullptr;
}

// Who creates and inserts DSP units (the game's meters for side-chain ducking are its own plugin DSPs).
typedef long(__stdcall* PfnCreateDsp)(void* sys, void* desc, void** dsp);
typedef long(__stdcall* PfnCreateDspByType)(void* sys, int type, void** dsp);
typedef long(__stdcall* PfnAddDsp)(void* self, void* dsp, void** conn);
PfnCreateDsp g_orig_create_dsp = nullptr;
PfnCreateDspByType g_orig_create_dsp_type = nullptr;
PfnAddDsp g_orig_add_dsp[3] = {nullptr, nullptr, nullptr};
volatile LONG g_dsp_logged = 0;

constexpr int kVoiceDsps = 96;
void* g_voice_dsp[kVoiceDsps];
volatile LONG g_nvoice_dsp = 0;

// Names and ranges of a DSP unit's parameters (FMOD's own read-only export).
void DiagDspParams(void* dsp, const char* what) {
  static auto npar = FxSym<long(__stdcall*)(void*, int*)>("_FMOD_DSP_GetNumParameters@8");
  static auto pinfo = FxSym<long(__stdcall*)(void*, int, char*, char*, char*, int, float*, float*)>("_FMOD_DSP_GetParameterInfo@32");
  int n = 0;
  if (!npar || !pinfo || npar(dsp, &n) != 0) return;
  std::string t;
  for (int k = 0; k < n && k < 24; ++k) {
    char name[32] = "", label[32] = "", desc[96] = "";
    float lo = 0, hi = 0;
    if (pinfo(dsp, k, name, label, desc, sizeof desc, &lo, &hi) != 0) continue;
    name[16] = 0; label[16] = 0;
    char b[160];
    snprintf(b, sizeof b, " | %d '%s' %g..%g %s", k, name, lo, hi, label);
    t += b;
  }
  ss_log("[vdsp] %s: %d parameters%s", what, n, t.c_str());
}

const char* DspName(void* dsp, char* buf) {
  static auto info = FxSym<long(__stdcall*)(void*, char*, unsigned*, int*, int*, int*)>("_FMOD_DSP_GetInfo@24");
  buf[0] = 0;
  if (info && dsp) info(dsp, buf, nullptr, nullptr, nullptr, nullptr);
  buf[31] = 0;
  return buf;
}

// audio_voice_dsp, the unit's sound path. Reverse engineering of the PC image: the "BlackRock VoiceDSP" description
// (built at 0x8A5860: channels 0 = any, read callback 0x8A5470) hands each block to 0x8A8520, which works on exactly
// 256 sample frames (0x100 pushed as the length to every helper, 4 KB stack buffers), reads only input channels 0 and
// 1, and writes output channels 0..5 (L R C LFE RL RR) with the INPUT channel count as the stride (the output count is
// never looked at). That is the console's mixer: 256-frame blocks, always 5.1. FMOD Ex on PC reads 1024-frame blocks
// by default, so the unit filled a quarter of each block and left the rest as whatever the buffer held (the noise),
// and on a stereo mix it would write 6 channels into a 2-channel buffer. So the read callback is swapped (in our copy
// of the description, at System::createDSP) for one that feeds the game's callback 256 frames at a time, and gives
// it a 5.1 buffer when the mix is not 5.1 (stereo: folded back to 2 channels). Only units created that way are put in
// the network (kept in g_vread_dsp).
typedef long(__stdcall* PfnDspRead)(void* state, float* in, float* out, unsigned len, int inch, int outch);
constexpr uintptr_t kVoiceDspRead = 0x8A5470;
constexpr unsigned kVoiceBlock = 256;
PfnDspRead g_voice_read = nullptr;     // the game's callback (0x8A5470), set when our copy is made
volatile LONG g_voice_read_wrap = 0;   // 1 when audio_voice_dsp is in: createDSP swaps the callback
constexpr int kVReadMax = 256;
void* volatile g_vread_dsp[kVReadMax];  // units created with our callback
volatile LONG g_nvread = 0;
volatile LONG g_vread_odd = 0;  // reads with a layout we pass through untouched (logged a few times)
float g_vin6[kVoiceBlock * 8], g_vout6[kVoiceBlock * 8];  // FMOD's mixer thread only

bool VoiceReadWrapped(void* dsp) {
  const LONG n = g_nvread < kVReadMax ? g_nvread : kVReadMax;
  for (LONG k = 0; k < n; ++k)
    if (g_vread_dsp[k] == dsp) return true;
  return false;
}

// What the units do, every 5 s on FMOD's mixer thread: loudest input and output sample, and the largest speaker gain
// target the game has given any unit (state object = FMOD_DSP_STATE plugin data, targets at +0x118..+0x12C, current
// gains at +0x100..+0x114; all zero after the unit's reset 0x8A7EB0 until the game sends parameters 8..13).
float g_vpk_in = 0, g_vpk_out = 0, g_vpk_tgt = 0, g_vpk_cur = 0;
unsigned g_vreads = 0;
DWORD g_vlog_t = 0;
int g_vlogs = 0;
float PeakAbs(const float* p, size_t n) {
  float m = 0;
  for (size_t i = 0; i < n; ++i) m = fabsf(p[i]) > m ? fabsf(p[i]) : m;
  return m;
}
void VoiceReadStats(void* state, const float* in, const float* out, unsigned len, int inch, int outch) {
  ++g_vreads;
  g_vpk_in = fmaxf(g_vpk_in, PeakAbs(in, (size_t)len * inch));
  g_vpk_out = fmaxf(g_vpk_out, PeakAbs(out, (size_t)len * outch));
  const uint8_t* st = state ? *reinterpret_cast<uint8_t* const*>(static_cast<uint8_t*>(state) + 4) : nullptr;
  if (st) {
    g_vpk_tgt = fmaxf(g_vpk_tgt, PeakAbs(reinterpret_cast<const float*>(st + 0x118), 6));
    g_vpk_cur = fmaxf(g_vpk_cur, PeakAbs(reinterpret_cast<const float*>(st + 0x100), 6));
  }
  const DWORD now = GetTickCount();
  if (!g_vlog_t) g_vlog_t = now;
  if (now - g_vlog_t >= 5000 && g_vlogs < 60) {
    ++g_vlogs;
    ss_log("[voice_dsp] last 5 s: %u unit reads (%d in / %d out ch), peak in %.4f out %.4f, gain target max %.3f "
           "current max %.3f", g_vreads, inch, outch, g_vpk_in, g_vpk_out, g_vpk_tgt, g_vpk_cur);
    g_vlog_t = now;
    g_vreads = 0;
    g_vpk_in = g_vpk_out = g_vpk_tgt = g_vpk_cur = 0;
  }
}

HOOK long HkVoiceDspRead(void* state, float* in, float* out, unsigned len, int inch, int outch) {
  if (!g_voice_read || !in || !out || inch <= 0 || outch <= 0) return 0;
  struct Stats {
    void* s; const float* i; const float* o; unsigned l; int ic, oc;
    ~Stats() { VoiceReadStats(s, i, o, l, ic, oc); }
  } stats{state, in, out, len, inch, outch};
  if (inch == outch && inch >= 6 && inch <= 8) {  // 5.1 / 7.1: the game's layout, 256 frames at a time
    unsigned off = 0;
    for (; off + kVoiceBlock <= len; off += kVoiceBlock)
      g_voice_read(state, in + off * inch, out + off * inch, kVoiceBlock, inch, outch);
    if (off < len) {  // a short tail (not seen with FMOD's fixed block sizes): padded with silence
      const unsigned n = len - off;
      memset(g_vin6, 0, sizeof g_vin6);
      memcpy(g_vin6, in + off * inch, n * inch * sizeof(float));
      g_voice_read(state, g_vin6, g_vout6, kVoiceBlock, inch, inch);
      memcpy(out + off * inch, g_vout6, n * inch * sizeof(float));
    }
    return 0;
  }
  if (inch == outch && (inch == 2 || inch == 1)) {  // stereo / mono mix: placed in 5.1, folded back
    for (unsigned off = 0; off < len; off += kVoiceBlock) {
      const unsigned n = len - off < kVoiceBlock ? len - off : kVoiceBlock;
      memset(g_vin6, 0, kVoiceBlock * 6 * sizeof(float));
      for (unsigned i = 0; i < n; ++i) {
        const float* s = in + (off + i) * inch;
        g_vin6[i * 6 + 0] = s[0];
        g_vin6[i * 6 + 1] = inch == 2 ? s[1] : s[0];
      }
      g_voice_read(state, g_vin6, g_vout6, kVoiceBlock, 6, 6);
      for (unsigned i = 0; i < n; ++i) {
        const float* o = g_vout6 + i * 6;
        const float c = 0.70710678f * o[2];
        float* d = out + (off + i) * outch;
        if (outch == 2) {
          d[0] = o[0] + c + o[4];
          d[1] = o[1] + c + o[5];
        } else {
          d[0] = 0.5f * (o[0] + o[1] + o[4] + o[5]) + c;
        }
      }
    }
    return 0;
  }
  // any other layout: the sound passes as it came (as shipped: no placement)
  if (InterlockedIncrement(&g_vread_odd) <= 4)
    ss_log("[voice_dsp] a voice unit is read with %d in / %d out channels: passed through unplaced", inch, outch);
  const int m = inch < outch ? inch : outch;
  for (unsigned i = 0; i < len; ++i)
    for (int c = 0; c < outch; ++c) out[i * outch + c] = c < m ? in[i * inch + c] : 0.0f;
  return 0;
}

// audio_sidechain: the game builds each audio group of DynamicMixer.xml (<AudioGroups>, in file order) slot by slot:
// FMOD's own filters are created and put on the group's channel group with ChannelGroup::addDSP, the game's plugin
// DSPs (BlackRock_VolumeMeter in slot 7 of every group, the Powerplays flanger) are created but on PC never put in
// the network, so the meters the side chains read (SideChainCollisions, Powerplays, HUD, Speech) stay at silence and
// no ducking ever happens. Here the k-th meter created is the k-th group's; its channel group is the one that group's
// filters went to, or the category of that name (groups without filters). The meters go in on the next
// EventSystem::update, on the game's audio thread.
const char* const kMeterGroups[] = {"master", "music", "PlayerEngine", "Powerplays", "HUD", "CPUEngine", "Swooshes",
                                    "Collisions", "Ambience", "Surfaces", "Particles", "ReverbSubmix", "Spot_Ambience",
                                    "HyperReal", "Speech", "Camera"};
constexpr int kMeters = sizeof kMeterGroups / sizeof kMeterGroups[0];
struct Meter { void* dsp; void* cg_seen; void* cg; bool in; };
Meter g_meters[kMeters];
volatile LONG g_nmeters = 0;      // meters created so far
volatile LONG g_meters_done = 0;  // 1 once all are in (or given up)
void* volatile g_last_cg = nullptr;  // the channel group the current group's filters went to
volatile LONG g_sidechain = 0;
volatile LONG g_own_add = 0;      // > 0 while we call addDSP ourselves
void* volatile g_last_bytype = nullptr;  // the last DSP created by type (a group's FMOD filter)

HOOK long HkCreateDsp(void* sys, void* desc, void** dsp) {
  // FMOD_DSP_DESCRIPTION (FMOD Ex, 0x5C bytes): name[32], version, channels, create, release, reset, read (+0x34), ...
  constexpr size_t kDescSize = 0x5C, kReadOff = 0x34;
  uint8_t mine[kDescSize];
  bool voice_read = false;
  if (g_voice_read_wrap && desc && dsp && !strncmp(static_cast<const char*>(desc), "BlackRock VoiceDSP", 18)) {
    uintptr_t rd = 0;
    memcpy(&rd, static_cast<uint8_t*>(desc) + kReadOff, 4);
    if (rd == kVoiceDspRead) {
      g_voice_read = reinterpret_cast<PfnDspRead>(rd);
      memcpy(mine, desc, kDescSize);
      const uintptr_t ours = reinterpret_cast<uintptr_t>(&HkVoiceDspRead);
      memcpy(mine + kReadOff, &ours, 4);
      desc = mine;  // FMOD keeps its own copy of the description
      voice_read = true;
    }
  }
  long r = g_orig_create_dsp(sys, desc, dsp);
  if (voice_read && r == 0 && *dsp) {
    const LONG k = InterlockedIncrement(&g_nvread) - 1;
    if (k < kVReadMax) g_vread_dsp[k] = *dsp;
    if (k == 0) {
      static auto bufsz = FxSym<long(__stdcall*)(void*, unsigned*, int*)>(
          "?getDSPBufferSize@System@FMOD@@QAG?AW4FMOD_RESULT@@PAIPAH@Z");
      unsigned bl = 0;
      int nb = 0;
      if (bufsz) bufsz(sys, &bl, &nb);
      ss_log("[voice_dsp] first voice unit %p created with our read callback (FMOD reads %u frames x %d buffers; the "
             "unit is fed %u at a time)", *dsp, bl, nb, kVoiceBlock);
    } else if (k == kVReadMax) {
      ss_log("[voice_dsp] more than %d voice units created: the rest stay out of the DSP network", kVReadMax);
    }
  }
  if (r == 0 && desc && dsp && *dsp && !strncmp(static_cast<const char*>(desc), "BlackRock VolumeMeter", 21)) {
    LONG k = InterlockedIncrement(&g_nmeters) - 1;
    if (k < kMeters) g_meters[k] = {*dsp, InterlockedExchangePointer((void* volatile*)&g_last_cg, nullptr), nullptr, false};
  }
  if (g_diag && r == 0 && desc && dsp && *dsp && !strncmp(static_cast<const char*>(desc), "BlackRock VoiceDSP", 18)) {
    LONG k = InterlockedIncrement(&g_nvoice_dsp) - 1;
    if (k < kVoiceDsps) g_voice_dsp[k] = *dsp;
    if (k == 0) DiagDspParams(*dsp, "BlackRock VoiceDSP");
  }
  if (g_diag && InterlockedIncrement(&g_dsp_logged) <= 400) {
    char n[32] = "";
    if (desc) memcpy(n, desc, 31);
    ss_log("[dsp] t=%lu createDSP '%s' -> %ld %p", (unsigned long)GetTickCount(), n, r, dsp ? *dsp : nullptr);
  }
  return r;
}

// audio_diag: what the game asks of its per-voice DSP units (BlackRock VoiceDSP: one per voice, the game sets their
// parameters every time a sound starts and while it plays). Per parameter: calls, lowest and highest value.
typedef long(__stdcall* PfnDspSetParam)(void* dsp, int index, float value);
PfnDspSetParam g_orig_dsp_set_param = nullptr;
struct ParStat { long n; float lo, hi; };
ParStat g_vpar[24];
volatile LONG g_vpar_logged = 0;
HOOK long HkDspSetParam(void* dsp, int index, float value) {
  bool voice = false;
  const LONG nv = g_nvoice_dsp < kVoiceDsps ? g_nvoice_dsp : kVoiceDsps;
  for (LONG k = 0; k < nv && !voice; ++k) voice = g_voice_dsp[k] == dsp;
  if (voice && index >= 0 && index < 24) {
    EnterCriticalSection(&g_cs);
    ParStat& ps = g_vpar[index];
    if (!ps.n) ps.lo = ps.hi = value;
    ++ps.n;
    if (value < ps.lo) ps.lo = value;
    if (value > ps.hi) ps.hi = value;
    LeaveCriticalSection(&g_cs);
    if (InterlockedIncrement(&g_vpar_logged) <= 600)
      ss_log("[vdsp] t=%lu voice DSP %p parameter %d = %g", (unsigned long)GetTickCount(), dsp, index, value);
  }
  return g_orig_dsp_set_param(dsp, index, value);
}

void DiagVoiceDsp(const char* when) {
  std::string t;
  for (int i = 0; i < 24; ++i) {
    EnterCriticalSection(&g_cs);
    const ParStat ps = g_vpar[i];
    LeaveCriticalSection(&g_cs);
    if (!ps.n) continue;
    char b[96];
    snprintf(b, sizeof b, " | %d: %ld calls, %g..%g", i, ps.n, ps.lo, ps.hi);
    t += b;
  }
  // is any of them in the DSP network (has an output)?
  static auto nout = FxSym<long(__stdcall*)(void*, int*)>("?getNumOutputs@DSP@FMOD@@QAG?AW4FMOD_RESULT@@PAH@Z");
  int wired = 0;
  const LONG nv = g_nvoice_dsp < kVoiceDsps ? g_nvoice_dsp : kVoiceDsps;
  for (LONG k = 0; k < nv && nout; ++k) {
    int n = 0;
    if (nout(g_voice_dsp[k], &n) == 0 && n > 0) ++wired;
  }
  ss_log("[vdsp] %s %ld voice DSP units created, %d with an output in the DSP network; parameters set by the game%s", when,
         (long)g_nvoice_dsp, wired, t.empty() ? ": none" : t.c_str());
}
HOOK long HkCreateDspByType(void* sys, int type, void** dsp) {
  long r = g_orig_create_dsp_type(sys, type, dsp);
  if (r == 0 && dsp) g_last_bytype = *dsp;
  if (g_diag && InterlockedIncrement(&g_dsp_logged) <= 400)
    ss_log("[dsp] t=%lu createDSPByType %d -> %ld %p", (unsigned long)GetTickCount(), type, r, dsp ? *dsp : nullptr);
  return r;
}
template <int K>
HOOK long HkAddDsp(void* self, void* dsp, void** conn) {
  long r = g_orig_add_dsp[K](self, dsp, conn);
  if (K == 1 && r == 0 && !g_own_add && dsp && dsp == g_last_bytype) g_last_cg = self;
  if (g_diag && InterlockedIncrement(&g_dsp_logged) <= 400) {
    static const char* const kWho[3] = {"Channel", "ChannelGroup", "System"};
    char n[32];
    ss_log("[dsp] t=%lu %s::addDSP(%p '%s') on %p -> %ld", (unsigned long)GetTickCount(), kWho[K], dsp, DspName(dsp, n),
           self, r);
  }
  return r;
}

void InstallDspHooks() {
  static bool done = false;  // from audio_voice_dsp early, and again for audio_diag / audio_sidechain
  if (done) return;
  HMODULE fx = GetModuleHandleA("fmodex.dll");
  if (!fx) return;
  done = true;
  void* f;
  if ((f = (void*)GetProcAddress(fx, "?createDSP@System@FMOD@@QAG?AW4FMOD_RESULT@@PAUFMOD_DSP_DESCRIPTION@@PAPAVDSP@2@@Z")))
    Detour(f, (void*)HkCreateDsp, (void**)&g_orig_create_dsp, "System::createDSP");
  if ((f = (void*)GetProcAddress(fx, "?createDSPByType@System@FMOD@@QAG?AW4FMOD_RESULT@@W4FMOD_DSP_TYPE@@PAPAVDSP@2@@Z")))
    Detour(f, (void*)HkCreateDspByType, (void**)&g_orig_create_dsp_type, "System::createDSPByType");
  if ((f = (void*)GetProcAddress(fx, "?addDSP@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PAVDSP@2@PAPAVDSPConnection@2@@Z")))
    Detour(f, (void*)HkAddDsp<0>, (void**)&g_orig_add_dsp[0], "Channel::addDSP");
  if ((f = (void*)GetProcAddress(fx, "?addDSP@ChannelGroup@FMOD@@QAG?AW4FMOD_RESULT@@PAVDSP@2@PAPAVDSPConnection@2@@Z")))
    Detour(f, (void*)HkAddDsp<1>, (void**)&g_orig_add_dsp[1], "ChannelGroup::addDSP");
  if ((f = (void*)GetProcAddress(fx, "?addDSP@System@FMOD@@QAG?AW4FMOD_RESULT@@PAVDSP@2@PAPAVDSPConnection@2@@Z")))
    Detour(f, (void*)HkAddDsp<2>, (void**)&g_orig_add_dsp[2], "System::addDSP");
  if (g_diag && (f = (void*)GetProcAddress(fx, "?setParameter@DSP@FMOD@@QAG?AW4FMOD_RESULT@@HM@Z")))
    Detour(f, (void*)HkDspSetParam, (void**)&g_orig_dsp_set_param, "DSP::setParameter");
  ss_log("[dsp] DSP creation wrappers: createDSP %s, byType %s, addDSP %s/%s/%s", g_orig_create_dsp ? "in" : "-",
         g_orig_create_dsp_type ? "in" : "-", g_orig_add_dsp[0] ? "in" : "-", g_orig_add_dsp[1] ? "in" : "-",
         g_orig_add_dsp[2] ? "in" : "-");
}

void DiagDsp(void* event_system, const char* when, bool detail) {
  HMODULE ev = GetModuleHandleA("fmod_event.dll");
  auto get_sys = ev ? reinterpret_cast<long(__stdcall*)(void*, void**)>(GetProcAddress(ev, "_FMOD_EventSystem_GetSystemObject@8"))
                    : nullptr;
  auto head = FxSym<long(__stdcall*)(void*, void**)>("_FMOD_System_GetDSPHead@8");
  auto nin = FxSym<long(__stdcall*)(void*, int*)>("_FMOD_DSP_GetNumInputs@8");
  auto input = FxSym<long(__stdcall*)(void*, int, void**, void**)>("_FMOD_DSP_GetInput@16");
  auto type = FxSym<long(__stdcall*)(void*, int*)>("_FMOD_DSP_GetType@8");
  auto info = FxSym<long(__stdcall*)(void*, char*, unsigned*, int*, int*, int*)>("_FMOD_DSP_GetInfo@24");
  auto npar = FxSym<long(__stdcall*)(void*, int*)>("_FMOD_DSP_GetNumParameters@8");
  auto par = FxSym<long(__stdcall*)(void*, int, float*, char*, int)>("_FMOD_DSP_GetParameter@20");
  auto active = FxSym<long(__stdcall*)(void*, int*)>("_FMOD_DSP_GetActive@8");
  auto bypass = FxSym<long(__stdcall*)(void*, int*)>("_FMOD_DSP_GetBypass@8");
  void* sys = nullptr;
  void* root = nullptr;
  if (!get_sys || !head || !nin || !input || !type || !info || get_sys(event_system, &sys) != 0 || !sys ||
      head(sys, &root) != 0 || !root) {
    ss_log("[diag] DSP network not readable (%p %p)", (void*)get_sys, sys);
    return;
  }
  static void* seen[512];
  int nseen = 0, ntypes[32] = {0};
  struct Item { void* dsp; int depth; };
  static Item stack[1024];
  int sp = 0;
  stack[sp++] = {root, 0};
  int total = 0, unusual = 0;
  while (sp > 0) {
    Item it = stack[--sp];
    bool dup = false;
    for (int i = 0; i < nseen; ++i) dup |= seen[i] == it.dsp;
    if (dup || nseen >= 512) continue;
    seen[nseen++] = it.dsp;
    ++total;
    int t = -1, n = 0, a = -1, b = -1, np = 0;
    char name[40] = "";
    type(it.dsp, &t);
    info(it.dsp, name, nullptr, nullptr, nullptr, nullptr);
    nin(it.dsp, &n);
    if (active) active(it.dsp, &a);
    if (bypass) bypass(it.dsp, &b);
    if (npar) npar(it.dsp, &np);
    if (t >= 0 && t < 32) ++ntypes[t];
    // FMOD Ex 4.28 FMOD_DSP_TYPE: 0 unknown/plugin, 1 mixer, 2 oscillator, 3 lowpass, 4 itlowpass, 5 highpass,
    // 6 echo, 7 flange, 8 distortion, 9 normalize, 10 parameq, 11 pitchshift, 12 chorus, 13 reverb, 16 itecho,
    // 17 compressor, 18 sfxreverb, 19 lowpass simple
    if (t != 1 || it.depth < 2) {
      std::string ps;
      for (int k = 0; k < np && k < (t == 18 ? 15 : 8) && par; ++k) {
        float v = 0;
        char vs[24] = "";
        par(it.dsp, k, &v, vs, sizeof vs);
        char b2[40];
        snprintf(b2, sizeof b2, "%s%g", k ? "," : "", v);
        ps += b2;
      }
      const bool plain = !strncmp(name, "ChannelGroup", 12) || !strcmp(name, "FMOD Channel DSPHead Unit") ||
                         !strcmp(name, "FMOD DSP Codec") || !strcmp(name, "EmulatedChannel DSPHead Unit");
      if ((detail && total <= 150) || t == 17 || (!plain && ++unusual <= 40))
        ss_log("[diag] %s dsp depth %d type %d '%s' inputs %d active %d bypass %d params [%s]", when, it.depth, t, name, n,
               a, b, ps.c_str());
    }
    for (int i = 0; i < n && sp < 1024; ++i) {
      void* in = nullptr;
      if (input(it.dsp, i, &in, nullptr) == 0 && in) stack[sp++] = {in, it.depth + 1};
    }
  }
  std::string c;
  for (int t = 0; t < 32; ++t)
    if (ntypes[t]) { char b2[24]; snprintf(b2, sizeof b2, " %d:%d", t, ntypes[t]); c += b2; }
  ss_log("[diag] %s DSP network: %d units, by type%s (17 = compressor)", when, total, c.c_str());
}

void DiagReverb(const char* when);  // further down, with the reverb wrappers
void ReapplyReverbLevel();
extern volatile LONG g_reverb_level_dirty;
void DiagVoiceDsp(const char* when);

// Called from HkUpdate when audio_diag is on: logs the game's requests per category whenever one moved by 0.5 dB.
void DiagDuck() {
  static float last[kMaxCats];
  static bool have = false;
  static int lines = 0;
  if (lines > 1500) return;
  char buf[900];
  int len = 0;
  bool moved = false;
  EnterCriticalSection(&g_cs);
  for (int i = 0; i < g_ncats && i < kMaxCats; ++i) {
    if (!g_cats[i].have_req) continue;
    float v = g_cats[i].req;
    double dbv = v > 0.000001f ? 20.0 * log10(v) : -120.0;
    double dl = last[i] > 0.000001f ? 20.0 * log10(last[i]) : -120.0;
    if (!have || fabs(dbv - dl) >= 0.5) moved = true;
    last[i] = v;
    len += snprintf(buf + len, sizeof buf - len, "%s %.1f ", g_cats[i].name, dbv);
    if (len >= (int)sizeof buf - 40) break;
  }
  LeaveCriticalSection(&g_cs);
  have = true;
  if (moved && len) { ++lines; ss_log("[duck] t=%lu %s", (unsigned long)GetTickCount(), buf); }
}

// ---------------------------------------------------------------- audio_software_mix
// FMOD Ex on its DirectSound output plays a sound on a "hardware" voice (a DirectSound buffer of its own) whenever the
// driver reports hardware mixing buffers; Wine's dsound reports 32 2D + 32 3D. audio_software_mix = true wraps
// System::init to set the hardware channels to none first, so FMOD mixes everything in software, as on the console.
// Measured on a race (6-channel capture): the speaker spread per category is the same either way, so it is off by
// default; the wrapper stays for the [fmod] log of the output setup.
typedef long(__stdcall* PfnSysInit)(void* sys, int channels, unsigned flags, void* extra);
PfnSysInit g_orig_sys_init = nullptr;
volatile LONG g_software_mix = 0;

void LogFmodOutput(void* sys, const char* when) {
  auto get_out = FxSym<long(__stdcall*)(void*, int*)>("?getOutput@System@FMOD@@QAG?AW4FMOD_RESULT@@PAW4FMOD_OUTPUTTYPE@@@Z");
  auto get_spk = FxSym<long(__stdcall*)(void*, int*)>("?getSpeakerMode@System@FMOD@@QAG?AW4FMOD_RESULT@@PAW4FMOD_SPEAKERMODE@@@Z");
  auto get_hw = FxSym<long(__stdcall*)(void*, int*, int*, int*)>("?getHardwareChannels@System@FMOD@@QAG?AW4FMOD_RESULT@@PAH00@Z");
  auto get_caps = FxSym<long(__stdcall*)(void*, int, unsigned*, int*, int*, int*)>(
      "?getDriverCaps@System@FMOD@@QAG?AW4FMOD_RESULT@@HPAIPAH1PAW4FMOD_SPEAKERMODE@@@Z");
  auto get_fmt = FxSym<long(__stdcall*)(void*, int*, int*, int*, int*, int*, int*)>(
      "?getSoftwareFormat@System@FMOD@@QAG?AW4FMOD_RESULT@@PAHPAW4FMOD_SOUND_FORMAT@@00PAW4FMOD_DSP_RESAMPLER@@0@Z");
  int out = -1, spk = -1, hw2 = -1, hw3 = -1, hwt = -1, minf = -1, maxf = -1, cpl = -1;
  unsigned caps = 0;
  int rate = -1, fmt = -1, outch = -1, inch = -1, res = -1, bits = -1;
  long rc = -1;
  if (get_out) get_out(sys, &out);
  if (get_spk) get_spk(sys, &spk);
  if (get_hw) get_hw(sys, &hw2, &hw3, &hwt);
  if (get_caps) rc = get_caps(sys, 0, &caps, &minf, &maxf, &cpl);
  if (get_fmt) get_fmt(sys, &rate, &fmt, &outch, &inch, &res, &bits);
  ss_log("[fmod] %s: output %d, speaker mode %d, hardware channels 2D %d 3D %d total %d, driver 0 caps %#x (rc %ld) "
         "freq %d..%d control panel speaker mode %d, software format %d Hz fmt %d out-ch %d in-ch %d",
         when, out, spk, hw2, hw3, hwt, caps, rc, minf, maxf, cpl, rate, fmt, outch, inch);
}

HOOK long HkSysInit(void* sys, int channels, unsigned flags, void* extra) {
  static auto set_hw = FxSym<long(__stdcall*)(void*, int, int, int, int)>(
      "?setHardwareChannels@System@FMOD@@QAG?AW4FMOD_RESULT@@HHHH@Z");
  LogFmodOutput(sys, "before init");
  long hr = -1;
  if (g_software_mix && set_hw) hr = set_hw(sys, 0, 0, 0, 0);
  long r = g_orig_sys_init(sys, channels, flags, extra);
  ss_log("[fmod] System::init(%d channels, flags %#x) -> %ld; hardware channels %s (%ld)", channels, flags, r,
         g_software_mix ? "set to none: everything in FMOD's software 5.1 mix" : "left as the driver reports them", hr);
  LogFmodOutput(sys, "after init");
  return r;
}

void InstallSoftwareMix() {
  HMODULE fx = GetModuleHandleA("fmodex.dll");
  void* f = fx ? (void*)GetProcAddress(fx, "?init@System@FMOD@@QAG?AW4FMOD_RESULT@@HIPAX@Z") : nullptr;
  if (!f || !Detour(f, (void*)HkSysInit, (void**)&g_orig_sys_init, "System::init"))
    ss_log("[fmod] System::init not wrapped: hardware voices stay as the driver reports them");
}

// On the update thread: every meter created gets its channel group (seen, or by category name), then goes in.
void AttachMeters(void* event_system) {
  static auto set_active = FxSym<long(__stdcall*)(void*, int)>("_FMOD_DSP_SetActive@8");
  const LONG n = g_nmeters;
  if (n < kMeters) {
    if (n > 0 && g_update_count > 3000) {  // a minute of updates and not all meters: the build differs
      ss_log("[sidechain] only %ld of %d volume meters created: none put in", (long)n, kMeters);
      g_meters_done = 1;
    }
    return;
  }
  if (n > kMeters) ss_log("[sidechain] %ld volume meters created, %d expected: the first %d used", (long)n, kMeters, kMeters);
  int ok = 0;
  for (int k = 0; k < kMeters; ++k) {
    Meter& m = g_meters[k];
    void* cat = nullptr;
    void* by_name = nullptr;
    if (g_orig_get_category(event_system, kMeterGroups[k], &cat) == 0 && cat) g_cat_group(cat, &by_name);
    m.cg = m.cg_seen ? m.cg_seen : by_name;
    long r = -1, ra = -1;
    if (m.cg && m.dsp) {
      void* conn = nullptr;
      InterlockedIncrement(&g_own_add);
      r = g_orig_add_dsp[1](m.cg, m.dsp, &conn);
      InterlockedDecrement(&g_own_add);
      if (r == 0 && set_active) ra = set_active(m.dsp, 1);
    }
    m.in = r == 0;
    ok += m.in;
    ss_log("[sidechain] %-13s meter %p -> channel group %p (%s%s) addDSP %ld setActive %ld", kMeterGroups[k], m.dsp,
           m.cg, m.cg_seen ? "its filters'" : "category", m.cg_seen && by_name && by_name != m.cg_seen ? ", category differs" : "",
           r, ra);
  }
  ss_log("[sidechain] %d of %d volume meters in the DSP network: the side chains of DynamicMixer.xml can duck", ok, kMeters);
  g_meters_done = 1;
}

void MlPoll();  // audio_music_log: the event started last is watched

HOOK long HkUpdate(void* self) {
  g_event_system = self;
  long r = g_orig_update(self);
  if (g_music_log_cg) MlPoll();
  LONG n = InterlockedIncrement(&g_update_count);
  if (g_sidechain && !g_meters_done && g_orig_add_dsp[1] && g_orig_get_category && g_cat_group) AttachMeters(self);
  if (InterlockedExchange(&g_reverb_level_dirty, 0)) ReapplyReverbLevel();
  if (g_detour_state == 2) {
    bool dirty = InterlockedExchange(&g_dirty, 0) != 0;
    if ((dirty || (n % 120) == 1) && Walk()) ApplyAll();
    if (g_diag && (n % 6) == 0) DiagDuck();
  }
  if (g_diag && (n % 600) == 300 && n < 600 * 40) {
    char when[32];
    snprintf(when, sizeof when, "u%ld/t%lu", (long)n, (unsigned long)GetTickCount());
    DiagDsp(self, when, n == 300);
    DiagReverb(when);
    DiagVoiceDsp(when);
  }
  return r;
}

void EnsureUpdateHook() {
  if (!g_mixer_ready || InterlockedCompareExchange(&g_update_state, 1, 0) != 0) return;
  bool ok = SwapSlot(g_update_slot, (void*)HkUpdate, (void**)&g_orig_update);
  g_update_state = ok ? 2 : -1;
  ss_log(ok ? "[mixer] EventSystem::update wrapped" : "[mixer] could not make the update slot writable: gains stay off");
}

// ----------------------------------------------------------------- the worker

template <typename F>
F Export(HMODULE m, const char* sym) {
  return reinterpret_cast<F>(GetProcAddress(m, sym));
}

#define SYM_ES(name, args) "?" name "@EventSystemI@FMOD@@UAG?AW4FMOD_RESULT@@" args
#define SYM_EC(name, args) "?" name "@EventCategoryI@FMOD@@UAG?AW4FMOD_RESULT@@" args

void InstallReverb(HMODULE ev) {
  void* fn_props = (void*)GetProcAddress(ev, SYM_ES("getReverbProperties", "PAUFMOD_REVERB_PROPERTIES@@@Z"));
  void* fn_get = (void*)GetProcAddress(ev, SYM_ES("getReverbPreset", "PBDPAUFMOD_REVERB_PROPERTIES@@PAH@Z"));
  void* fn_idx = (void*)GetProcAddress(ev, SYM_ES("getReverbPresetByIndex", "HPAUFMOD_REVERB_PROPERTIES@@PAPAD@Z"));
  void* fn_num = (void*)GetProcAddress(ev, SYM_ES("getNumReverbPresets", "PAH@Z"));
  if (!fn_props || !fn_get || !fn_idx || !fn_num) { ss_log("[reverb] the reverb preset exports are not in fmod_event.dll: nothing done"); return; }
  void** s_get = FindSlot(ev, fn_get, fn_props, fn_idx);
  void** s_idx = FindSlot(ev, fn_idx, fn_get, fn_num);
  if (!s_get || !s_idx || s_idx != s_get + 1) {
    ss_log("[reverb] EventSystem vtable layout not recognised (get %p, by index %p): nothing done", (void*)s_get, (void*)s_idx);
    return;
  }
  g_reverb_on = true;
  bool ok = SwapSlot(s_idx, (void*)HkGetPresetByIndex, (void**)&g_orig_by_index) &&
            SwapSlot(s_get, (void*)HkGetPreset, (void**)&g_orig_get_preset);
  if (!ok) { g_reverb_on = false; ss_log("[reverb] could not make the vtable slots writable: nothing done"); return; }
  ss_log("[reverb] fmod_event.dll at %p, EventSystem vtable slots getReverbPreset %p / getReverbPresetByIndex %p wrapped; "
         "%d authored presets loaded", (void*)ev, (void*)s_get, (void*)s_idx, ssrev::kReverbPresetCount);
}

// ---------------------------------------------------------------- console_mix

// The PC release's event files carry a later revision of some event properties than the Xbox 360 release (volume of
// about 20 events, pitch of 4), and some re-recorded samples play at a different level than the console's. The table
// (console_mix_table.h, generated by tools/audio/fev_compare.py from both releases' files) gives, per project + event
// name + property + the PC value, the console value; the volume includes a gain for re-recorded samples where every
// sample the event plays was measured. Whenever the game takes an event from the event system (the getEvent family,
// vtable slots of fmod_event.dll swapped for wrappers), the wrapper looks the event up and, when its property still
// holds the PC value, sets the console value, on the event's template (all instances; this_instance = false) and on
// the instance handed out. Events and values not in the table are never touched, nor is an event the game already
// changed itself (its value no longer equals the PC one).
typedef long(__stdcall* PfnGetEvent)(void* self, uintptr_t a, unsigned mode, void** out);
typedef long(__stdcall* PfnEvInfo)(void* ev, int* index, char** name, void* info);
typedef long(__stdcall* PfnEvGroup)(void* ev, void** group);
typedef long(__stdcall* PfnGroupProject)(void* group, void** project);
typedef long(__stdcall* PfnProjectInfo)(void* project, int* index, char** name);
typedef long(__stdcall* PfnEvProp)(void* ev, int index, void* value, int this_instance);

constexpr int kGetEventSlots = 9;
PfnGetEvent g_orig_get_event[kGetEventSlots];
PfnEvInfo g_ev_info = nullptr;
PfnEvGroup g_ev_group = nullptr;
PfnGroupProject g_group_project = nullptr;
PfnProjectInfo g_project_info = nullptr;
PfnEvProp g_ev_get_prop = nullptr;
PfnEvProp g_ev_set_prop = nullptr;
volatile LONG g_cm_hits[sscm::kCorrectionCount];  // per table row: templates corrected
volatile LONG g_cm_instances = 0;                  // instances corrected
volatile LONG g_cm_logged = 0;
volatile LONG g_cm_on = 0;         // 1: CONSOLE (PC values -> console), 0: NATIVE (console values put in -> PC again)
volatile LONG g_cm_installed = 0;  // the getEvent wrappers are in

uint32_t Fnv1a(const char* s) {
  uint32_t h = 0x811C9DC5u;
  for (; *s; ++s) h = (h ^ static_cast<uint8_t>(*s)) * 0x01000193u;
  return h;
}

bool Near(float a, float b) { return fabsf(a - b) <= 1e-5f + 1e-4f * fabsf(b); }

typedef long(__stdcall* PfnEvCategory)(void* ev, void** category);
PfnEvCategory g_ev_category = nullptr;
volatile LONG g_ev_logged = 0;

// audio_diag: one line per event fetched (capped): what plays, in which category, how loud and how it is placed.
void DiagEvent(void* ev, const char* name, unsigned mode) {
  if (InterlockedIncrement(&g_ev_logged) > 6000) return;
  char* cname = nullptr;
  void* cat = nullptr;
  int ci = 0;
  if (g_ev_category && g_cat_info && g_ev_category(ev, &cat) == 0 && cat) g_cat_info(cat, &ci, &cname);
  float vol = -1, mind = -1, maxd = -1, spread = -1, pan = -1;
  int emode = -1, maxpb = -1;
  g_ev_get_prop(ev, 1, &vol, 1);
  g_ev_get_prop(ev, 14, &emode, 1);
  g_ev_get_prop(ev, 12, &maxpb, 1);
  g_ev_get_prop(ev, 17, &mind, 1);
  g_ev_get_prop(ev, 18, &maxd, 1);
  g_ev_get_prop(ev, 24, &spread, 1);
  g_ev_get_prop(ev, 25, &pan, 1);
  ss_log("[event] t=%lu %s cat=%s fetch=%#x vol=%.3f mode=%#x maxpb=%d dist=%.1f..%.1f spread=%.1f pan=%.2f",
         (unsigned long)GetTickCount(), name, cname ? cname : "?", mode, vol, emode, maxpb, mind, maxd, spread, pan);
  float sp[8];
  for (int k = 0; k < 8; ++k) { sp[k] = -1; g_ev_get_prop(ev, 26 + k, &sp[k], 0); }
  ss_log("[tmpl] %s cat=%s speakers L %.2f C %.2f R %.2f LS %.2f RS %.2f LR %.2f RR %.2f LFE %.2f", name,
         cname ? cname : "?", sp[0], sp[1], sp[2], sp[3], sp[4], sp[5], sp[6], sp[7]);
}

// audio_diag: EventI::start (vtable slot; `this` is the EventI object, the public wrapper validated the handle).
// One line per start: event, category, instance volume, every parameter's value; shows what a crash plays and how
// hard the game drives it.
typedef long(__stdcall* PfnEvIStart)(void* evi);
typedef long(__stdcall* PfnEvIInfo)(void* evi, int* index, char** name, void* info);
typedef long(__stdcall* PfnEvINum)(void* evi, int* n);
typedef long(__stdcall* PfnEvIParam)(void* evi, int i, void** param);
typedef long(__stdcall* PfnParInfo)(void* par, int* index, char** name);
typedef long(__stdcall* PfnParValue)(void* par, float* v);
typedef long(__stdcall* PfnParRange)(void* par, float* lo, float* hi);
PfnEvIStart g_orig_evi_start = nullptr;
PfnEvIInfo g_evi_info = nullptr;
PfnEvCategory g_evi_category = nullptr;
PfnEvINum g_evi_num_params = nullptr;
PfnEvIParam g_evi_param = nullptr;
PfnEvProp g_evi_get_prop = nullptr;
PfnParInfo g_par_info = nullptr;
PfnParValue g_par_value = nullptr;
volatile LONG g_start_logged = 0;

HOOK long HkEvIStart(void* evi) {
  if (g_diag && InterlockedIncrement(&g_start_logged) <= 8000) {
    char* name = nullptr;
    int idx = 0;
    char line[640];
    int len = 0;
    if (g_evi_info(evi, &idx, &name, nullptr) == 0 && name) {
      char* cname = nullptr;
      void* cat = nullptr;
      int ci = 0;
      if (g_cat_info && g_evi_category(evi, &cat) == 0 && cat) g_cat_info(cat, &ci, &cname);
      float vol = -1;
      g_evi_get_prop(evi, 1, &vol, 1);
      len = snprintf(line, sizeof line, "[start] t=%lu %s cat=%s vol=%.3f", (unsigned long)GetTickCount(), name,
                     cname ? cname : "?", vol);
      int np = 0;
      if (g_evi_num_params(evi, &np) == 0)
        for (int i = 0; i < np && i < 8 && len > 0 && len < (int)sizeof line - 48; ++i) {
          void* par = nullptr;
          char* pn = nullptr;
          int pi = 0;
          float v = 0;
          if (g_evi_param(evi, i, &par) == 0 && par && g_par_info(par, &pi, &pn) == 0 && pn && g_par_value(par, &v) == 0)
            len += snprintf(line + len, sizeof line - len, " %.24s=%.3f", pn, v);
        }
      ss_log("%s", line);
    }
  }
  return g_orig_evi_start(evi);
}

// audio_diag: which event properties the game itself sets (EventI::setPropertyByIndex slot), and the speaker levels it
// puts on channels (fmodex Channel::setSpeakerLevels): the places a port could spread a sound over the speakers.
typedef long(__stdcall* PfnEvISetProp)(void* evi, int index, void* value, int this_instance);
PfnEvISetProp g_orig_evi_set_prop = nullptr;
volatile LONG g_prop_count[64];
volatile LONG g_prop_logged = 0;
struct PropStat { long n, differ, one; float lo, hi, tlo, thi; };
PropStat g_prop_stat[48];

// audio_speaker_fix: the PC game sets the 2D speaker levels of almost every event it plays (FMOD_EVENTPROPERTY 27 C,
// 31 LR, 32 RR, 33 LFE) to exactly 1.0, where the event files (the same as the console's) route a 2D sound to the front
// pair (L 1, R 1, C/rears 0, LFE 0 or a small send). So collisions, swooshes, tyres, power plays all came out of all six
// speakers at full level. The wrapper keeps the event file's level for such a write: a write of exactly 1.0 to 26..33
// where the event's own (template) value is not 1.0. The engine's moving speaker levels (never exactly 1.0) pass.
volatile LONG g_speaker_fix = 0;
volatile LONG g_voice_dsp_on = 0;  // audio_voice_dsp is in: the game's voice units place the sounds, the 1.0 is theirs
volatile LONG g_speaker_dropped = 0;
PfnEvProp g_evi_template_prop = nullptr;  // EventI::getPropertyByIndex, this_instance false: the event file's value
HOOK long HkEvISetProp(void* evi, int index, void* value, int this_instance) {
  const bool speaker = index >= 26 && index <= 33;
  bool drop = false;
  if (speaker && g_speaker_fix && !g_voice_dsp_on && value && g_evi_template_prop) {
    float f = 0, t = 1.0f;
    memcpy(&f, value, 4);
    if (f == 1.0f && g_evi_template_prop(evi, index, &t, 0) == 0 && t != 1.0f) drop = true;
  }
  if (drop && InterlockedIncrement(&g_speaker_dropped) == 1)
    ss_log("[speaker_fix] first game write of a 2D speaker level 1.0 over the event file's level kept out (property %d)", index);
  if (g_diag && index >= 0 && index < 64) {
    LONG c = InterlockedIncrement(&g_prop_count[index]);
    if ((speaker ? c <= 150 : (c <= 40 || (c & (c - 1)) == 0)) && InterlockedIncrement(&g_prop_logged) <= 2500) {
      char* name = nullptr;
      int idx = 0;
      if (g_evi_info) g_evi_info(evi, &idx, &name, nullptr);
      float f = 0, tf = 0;
      int i = 0;
      if (value) memcpy(&f, value, 4), memcpy(&i, value, 4);
      const long rt = g_evi_template_prop ? g_evi_template_prop(evi, index, &tf, 0) : -1;
      char* cname = nullptr;
      void* cat = nullptr;
      int ci = 0;
      if (g_cat_info && g_evi_category && g_evi_category(evi, &cat) == 0 && cat) g_cat_info(cat, &ci, &cname);
      ss_log("[prop] t=%lu %s cat=%s set property %d = %g / %d (instance %d), event file %g (%ld), #%ld%s",
             (unsigned long)GetTickCount(), name ? name : "?", cname ? cname : "?", index, f, i, this_instance, tf, rt, (long)c,
             drop ? " (dropped)" : "");
    }
    if (value && g_evi_template_prop && index < 48) {  // the table [props]: writes, and how many differ from the file
      float f = 0, tf = 0;
      memcpy(&f, value, 4);
      PropStat& ps = g_prop_stat[index];
      EnterCriticalSection(&g_cs);
      if (g_evi_template_prop(evi, index, &tf, 0) == 0) {
        if (!ps.n) ps.lo = ps.hi = f, ps.tlo = ps.thi = tf;
        ++ps.n;
        if (fabsf(f - tf) > 0.001f) ++ps.differ;
        if (f == 1.0f && tf != 1.0f) ++ps.one;
        if (f < ps.lo) ps.lo = f;
        if (f > ps.hi) ps.hi = f;
        if (tf < ps.tlo) ps.tlo = tf;
        if (tf > ps.thi) ps.thi = tf;
      }
      LeaveCriticalSection(&g_cs);
    }
  }
  if (drop) return 0;
  return g_orig_evi_set_prop(evi, index, value, this_instance);
}

void InstallSetPropWrapper(HMODULE ev) {
  g_evi_template_prop = Export<PfnEvProp>(ev, "?getPropertyByIndex@EventI@FMOD@@UAG?AW4FMOD_RESULT@@HPAX_N@Z");
  if (!g_evi_info) g_evi_info = Export<PfnEvIInfo>(ev, "?getInfo@EventI@FMOD@@UAG?AW4FMOD_RESULT@@PAHPAPADPAUFMOD_EVENT_INFO@@@Z");
  void* fn = (void*)GetProcAddress(ev, "?setPropertyByIndex@EventI@FMOD@@UAG?AW4FMOD_RESULT@@HPAX_N@Z");
  void** slot = fn ? FindSlot(ev, fn, nullptr, nullptr) : nullptr;
  const bool ok = slot && g_evi_template_prop && SwapSlot(slot, (void*)HkEvISetProp, (void**)&g_orig_evi_set_prop);
  ss_log("[speaker_fix] %s; EventI::setPropertyByIndex slot %p %s", g_speaker_fix ? "on: 2D sounds keep the event files' speaker levels" : "off (diag log only)",
         (void*)slot, ok ? "wrapped" : "not wrapped");
}

// audio_voice_dsp: put each 3D voice's own DSP unit (BlackRock VoiceDSP) in the DSP network, as the console does.
// Reverse engineering of the PC image: a 3D voice (Audio::Voice::cVoice3D, vtable 0xC32A2C) whose sound property
// "IsUsingVoiceDSP" is on (default 1.0, table at 0x8BA250) gets a VoiceDSP on start (0x8A634F..0x8A63A0), sets its
// event's six speaker levels to 1.0 (0x8A5F20) and from then on places the sound only through the unit's parameters
// 8..13 (L R C LFE RL RR, 0x8A6779.., computed by the panner 0x8BA3B0 from the listener: front/back/side, distance
// filter). The sound wrapper hands the unit the event's channel group through its "attach" virtual (0x86BAB7 ->
// 0x8FA820 -> slot 9), and on PC that slot of the VoiceDSP instance vtable (0xC32848 -> 0x8A5B40) is only
// "mov eax,[ecx+0x138]; ret 4": it returns the FMOD DSP and never calls ChannelGroup::addDSP. So no voice unit ever
// sees a sample: on PC every such sound (opponent cars among them) plays on all speakers alike, with or without
// audio_speaker_fix, and is never moved front/back/side. The slot is in .rdata (a data write, no code is touched);
// our function does what the slot is for: the DSP goes on the channel group and is made active.
// Lifetime: the units come from a pool of 64 (registered at 0x8A56E6); a voice that ends only drops its reference, the
// unit is not disconnected (its destructor, 0x8A5B50, runs only at shutdown), and the event instance (with its channel
// group) is reused by later voices. So when a unit is put on a channel group, any other voice unit still sitting on
// that group is the leftover of an ended voice and is taken out first (DSP::remove), and a unit that moves to another
// group leaves its old one. One unit per event channel group, never a chain.
constexpr uintptr_t kVoiceDspAttachSlot = 0xC32848;
constexpr uintptr_t kVoiceDspAttachStub = 0x8A5B40;
constexpr uintptr_t kVoiceDspDspOffset = 0x138;
volatile LONG g_voice_attached = 0;
volatile LONG g_voice_attach_failed = 0;
volatile LONG g_voice_unwrapped = 0;
volatile LONG g_voice_stale = 0;
struct VoicePlace { void* dsp; void* cg; };
constexpr int kVoicePlaces = 256;
VoicePlace g_voice_place[kVoicePlaces];  // where each unit was put; the game's audio thread only
int g_nvoice_place = 0;
__attribute__((thiscall, force_align_arg_pointer)) void* HkVoiceDspAttach(void* self, void* cg) {
  static auto add = FxSym<long(__stdcall*)(void*, void*, void**)>(
      "?addDSP@ChannelGroup@FMOD@@QAG?AW4FMOD_RESULT@@PAVDSP@2@PAPAVDSPConnection@2@@Z");
  static auto active = FxSym<long(__stdcall*)(void*, int)>("?setActive@DSP@FMOD@@QAG?AW4FMOD_RESULT@@_N@Z");
  static auto nout = FxSym<long(__stdcall*)(void*, int*)>("?getNumOutputs@DSP@FMOD@@QAG?AW4FMOD_RESULT@@PAH@Z");
  static auto remove = FxSym<long(__stdcall*)(void*)>("?remove@DSP@FMOD@@QAG?AW4FMOD_RESULT@@XZ");
  void* dsp = self ? *reinterpret_cast<void**>(static_cast<char*>(self) + kVoiceDspDspOffset) : nullptr;
  if (!dsp || !cg || !add || !nout || !remove) return dsp;
  if (!VoiceReadWrapped(dsp)) {  // created before our createDSP hook: its read would garble the sound, stay out
    if (InterlockedIncrement(&g_voice_unwrapped) <= 4)
      ss_log("[voice_dsp] voice unit %p was created without our read callback: left out (that voice is not placed)", dsp);
    return dsp;
  }
  int slot = -1, outs = 0;
  for (int k = 0; k < g_nvoice_place; ++k) {
    VoicePlace& p = g_voice_place[k];
    if (p.dsp == dsp) { slot = k; continue; }
    if (p.cg == cg) {  // an ended voice's unit still on this event's group
      if (nout(p.dsp, &outs) == 0 && outs > 0) remove(p.dsp);
      p.cg = nullptr;
      const LONG n = InterlockedIncrement(&g_voice_stale);
      if (n == 1 || (n & (n - 1)) == 0)
        ss_log("[voice_dsp] old voice unit %p taken off channel group %p before %p goes on, %ld so far", p.dsp, cg, dsp,
               (long)n);
    }
  }
  outs = 0;
  const bool wired = nout(dsp, &outs) == 0 && outs > 0;
  if (wired && slot >= 0 && g_voice_place[slot].cg == cg) return dsp;  // already there: left as it is (no click)
  if (wired) remove(dsp);  // moved to another event: out of the old one first
  if (slot < 0 && g_nvoice_place < kVoicePlaces) slot = g_nvoice_place++;
  const long r = add(cg, dsp, nullptr);
  const long ra = r == 0 && active ? active(dsp, 1) : -1;
  if (slot >= 0) g_voice_place[slot] = {dsp, r == 0 ? cg : nullptr};
  if (r == 0) {
    const LONG n = InterlockedIncrement(&g_voice_attached);
    if (n == 1 || (n & (n - 1)) == 0)
      ss_log("[voice_dsp] voice unit %p on channel group %p (setActive rc %ld), %ld so far, %d units known", dsp, cg, ra,
             (long)n, g_nvoice_place);
  } else if (InterlockedIncrement(&g_voice_attach_failed) <= 8) {
    ss_log("[voice_dsp] ChannelGroup::addDSP(%p, voice unit %p) -> %ld: that voice is not placed", cg, dsp, r);
  }
  return dsp;
}

void InstallVoiceDsp() {
  void** slot = reinterpret_cast<void**>(kVoiceDspAttachSlot);
  MEMORY_BASIC_INFORMATION mi{};
  const bool readable = VirtualQuery(slot, &mi, sizeof mi) == sizeof mi && mi.State == MEM_COMMIT;
  const uintptr_t cur = readable ? reinterpret_cast<uintptr_t>(*slot) : 0;
  if (cur != kVoiceDspAttachStub) {
    ss_log("[voice_dsp] VoiceDSP attach slot %p holds %#lx, not the stub %#lx (another build?): nothing done", (void*)slot,
           (unsigned long)cur, (unsigned long)kVoiceDspAttachStub);
    return;
  }
  g_voice_read_wrap = 1;  // the units must get our read callback when they are created: hook createDSP first
  InstallDspHooks();
  if (!g_orig_create_dsp) {
    g_voice_read_wrap = 0;
    ss_log("[voice_dsp] System::createDSP not hooked: the units would garble their sound, nothing done");
    return;
  }
  const bool ok = ss_write_word(slot, reinterpret_cast<uintptr_t>(&HkVoiceDspAttach));
  if (ok) g_voice_dsp_on = 1;
  ss_log("[voice_dsp] %s: 3D voices that use the game's voice unit (opponent cars, positional effects) %s",
         ok ? "on" : "slot write refused", ok ? "get it in the DSP network, so they are placed front/back/side as on the console"
                                             : "stay as shipped (on every speaker)");
}

typedef long(__stdcall* PfnChSpeakerLevels)(void* ch, int speaker, float* levels, int n);
PfnChSpeakerLevels g_orig_ch_levels = nullptr;
volatile LONG g_levels_logged = 0;
HOOK long HkChSpeakerLevels(void* ch, int speaker, float* levels, int n) {
  if (InterlockedIncrement(&g_levels_logged) <= 120)
    ss_log("[levels] t=%lu Channel %p setSpeakerLevels speaker %d n %d first %g", (unsigned long)GetTickCount(), ch,
           speaker, n, levels && n > 0 ? levels[0] : -1.0f);
  return g_orig_ch_levels(ch, speaker, levels, n);
}

// audio_diag: the reverb path. What the game hands to System::setReverbProperties (its blended preset), the reverb send
// it puts on every voice (Event / Channel setReverbProperties: Direct and Room, in mB) and the speaker levels it puts
// on the reverb submix's output connection (DSPConnection::setLevels). Read only, except SS_AUDIO_REVERB_TEST=mute (a
// test switch of the diagnostic: FMOD gets the preset with Room -10000, to see in a capture what the return carries).
typedef long(__stdcall* PfnConnLevels)(void* conn, int speaker, float* levels, int n);
typedef long(__stdcall* PfnSysReverb)(void* sys, const ReverbProps* p);
struct ChanReverb {  // == FMOD_REVERB_CHANNELPROPERTIES (FMOD Ex 4.28), 19 dwords
  int Direct, DirectHF, Room, RoomHF, Obstruction;
  float ObstructionLFRatio;
  int Occlusion;
  float OcclusionLFRatio, OcclusionRoomRatio, OcclusionDirectRatio;
  int Exclusion;
  float ExclusionLFRatio;
  int OutsideVolumeHF;
  float DopplerFactor, RolloffFactor, RoomRolloffFactor, AirAbsorptionFactor;
  unsigned Flags;
  void* ConnectionPoint;
};
typedef long(__stdcall* PfnChanReverb)(void* self, const ChanReverb* p);
PfnConnLevels g_orig_conn_levels = nullptr;
PfnSysReverb g_orig_sys_reverb = nullptr;
PfnChanReverb g_orig_chan_reverb = nullptr;
PfnChanReverb g_orig_evi_reverb = nullptr;
volatile LONG g_conn_logged = 0, g_sysrev_logged = 0, g_chrev_logged = 0, g_evrev_logged = 0;
volatile LONG g_reverb_mute = 0;
struct RevStat { long n; int dlo, dhi, rlo, rhi; };
RevStat g_rev_stat[2];  // 0 events, 1 channels

void RevStatAdd(int k, const ChanReverb* p) {
  RevStat& r = g_rev_stat[k];
  EnterCriticalSection(&g_cs);
  if (!r.n) r.dlo = r.dhi = p->Direct, r.rlo = r.rhi = p->Room;
  ++r.n;
  if (p->Direct < r.dlo) r.dlo = p->Direct;
  if (p->Direct > r.dhi) r.dhi = p->Direct;
  if (p->Room < r.rlo) r.rlo = p->Room;
  if (p->Room > r.rhi) r.rhi = p->Room;
  LeaveCriticalSection(&g_cs);
}

HOOK long HkConnSetLevels(void* conn, int speaker, float* levels, int n) {
  if (InterlockedIncrement(&g_conn_logged) <= 80) {
    char b[160];
    int len = 0;
    b[0] = 0;
    for (int k = 0; k < n && k < 8 && levels; ++k) len += snprintf(b + len, sizeof b - len, "%s%g", k ? "," : "", levels[k]);
    ss_log("[revdiag] t=%lu DSPConnection %p setLevels speaker %d n %d [%s]", (unsigned long)GetTickCount(), conn, speaker, n, b);
  }
  return g_orig_conn_levels(conn, speaker, levels, n);
}

// reverb_level (percent, default 100 = the presets as authored): a trim on the reverb's Room level, in the preset the
// game hands to System::setReverbProperties (-6 dB at 50, off at 0). The return's place in the speakers and every
// other parameter stay the game's. The last preset is kept, so a change from the overlay is handed on again from
// the update thread without waiting for the game's next call.
volatile LONG g_reverb_level = 100;
volatile LONG g_reverb_level_dirty = 0;
void* volatile g_reverb_sys = nullptr;
ReverbProps g_reverb_last;  // what the game asked last (untrimmed); under g_cs
bool g_reverb_have = false;

int TrimmedRoom(int room) {
  const LONG pct = g_reverb_level;
  if (pct >= 100) return room;
  if (pct <= 0) return -10000;
  const int out = room + static_cast<int>(lround(2000.0 * log10(pct / 100.0)));
  return out < -10000 ? -10000 : out;
}

HOOK long HkSysSetReverb(void* sys, const ReverbProps* p) {
  static ReverbProps last;
  ReverbProps q;
  if (p) {
    EnterCriticalSection(&g_cs);
    g_reverb_last = *p;
    g_reverb_have = true;
    g_reverb_sys = sys;
    LeaveCriticalSection(&g_cs);
    if (g_diag && (p->Room != last.Room || p->Reverb != last.Reverb || p->Reflections != last.Reflections ||
         fabsf(p->DecayTime - last.DecayTime) > 0.01f) &&
        InterlockedIncrement(&g_sysrev_logged) <= 600)
      ss_log("[revdiag] t=%lu System::setReverbProperties inst %d env %d Room %d RoomHF %d RoomLF %d decay %.2f HFratio %.2f "
             "refl %d (%.3f s) reverb %d (%.3f s) diffusion %.0f density %.0f HFref %.0f flags %#x",
             (unsigned long)GetTickCount(), p->Instance, p->Environment, p->Room, p->RoomHF, p->RoomLF, p->DecayTime,
             p->DecayHFRatio, p->Reflections, p->ReflectionsDelay, p->Reverb, p->ReverbDelay, p->Diffusion, p->Density,
             p->HFReference, p->Flags);
    last = *p;
    if (g_reverb_mute || g_reverb_level < 100) { q = *p; q.Room = g_reverb_mute ? -10000 : TrimmedRoom(p->Room); p = &q; }
  }
  return g_orig_sys_reverb(sys, p);
}

// On the update thread, after the overlay moved reverb_level: the game's last preset again, with the new trim.
void ReapplyReverbLevel() {
  ReverbProps q;
  EnterCriticalSection(&g_cs);
  const bool have = g_reverb_have;
  q = g_reverb_last;
  LeaveCriticalSection(&g_cs);
  void* sys = g_reverb_sys;
  if (!have || !sys || !g_orig_sys_reverb) return;
  q.Room = g_reverb_mute ? -10000 : TrimmedRoom(q.Room);
  g_orig_sys_reverb(sys, &q);
}

void InstallReverbLevel() {
  HMODULE fx = GetModuleHandleA("fmodex.dll");
  void* f = fx ? (void*)GetProcAddress(fx, "?setReverbProperties@System@FMOD@@QAG?AW4FMOD_RESULT@@PBUFMOD_REVERB_PROPERTIES@@@Z") : nullptr;
  const bool ok = f && Detour(f, (void*)HkSysSetReverb, (void**)&g_orig_sys_reverb, "System::setReverbProperties");
  ss_log("[reverb] reverb_level %ld%% (%s); System::setReverbProperties %s", (long)g_reverb_level,
         g_reverb_level >= 100 ? "the presets' own Room level" : "Room level trimmed", ok ? "wrapped" : "not wrapped: no trim");
}

HOOK long HkChanSetReverb(void* ch, const ChanReverb* p) {
  if (p) {
    RevStatAdd(1, p);
    if (InterlockedIncrement(&g_chrev_logged) <= 120)
      ss_log("[revdiag] t=%lu Channel %p setReverbProperties Direct %d Room %d flags %#x conn %p", (unsigned long)GetTickCount(),
             ch, p->Direct, p->Room, p->Flags, p->ConnectionPoint);
  }
  return g_orig_chan_reverb(ch, p);
}

HOOK long HkEvISetReverb(void* evi, const ChanReverb* p) {
  if (p) {
    RevStatAdd(0, p);
    const LONG c = InterlockedIncrement(&g_evrev_logged);
    if (c <= 400 || (c & (c - 1)) == 0) {
      char* name = nullptr;
      int idx = 0;
      if (g_evi_info) g_evi_info(evi, &idx, &name, nullptr);
      float wet = -1, dry = -1;
      if (g_evi_template_prop) g_evi_template_prop(evi, 34, &wet, 0), g_evi_template_prop(evi, 38, &dry, 0);
      ss_log("[revdiag] t=%lu event %s setReverbProperties Direct %d Room %d flags %#x (event file wet %g dry %g), #%ld",
             (unsigned long)GetTickCount(), name ? name : "?", p->Direct, p->Room, p->Flags, wet, dry, (long)c);
    }
  }
  return g_orig_evi_reverb(evi, p);
}

// On the update thread: the reverb sends so far and the property write table. (The levels on the reverb submix's
// output connection are logged as the game sets them; reading them back with DSPConnection::getLevels faults in
// this fmodex.dll, so that is not done.)
void DiagReverb(const char* when) {
  EnterCriticalSection(&g_cs);
  const RevStat e = g_rev_stat[0], c = g_rev_stat[1];
  LeaveCriticalSection(&g_cs);
  ss_log("[revdiag] %s reverb sends so far: events %ld (Direct %d..%d mB, Room %d..%d mB), channels %ld (Direct %d..%d, Room %d..%d)",
         when, e.n, e.dlo, e.dhi, e.rlo, e.rhi, c.n, c.dlo, c.dhi, c.rlo, c.rhi);
  std::string t;
  for (int i = 0; i < 48; ++i) {
    EnterCriticalSection(&g_cs);
    const PropStat ps = g_prop_stat[i];
    LeaveCriticalSection(&g_cs);
    if (!ps.n) continue;
    char b[200];
    snprintf(b, sizeof b, " | %d: %ld writes, %ld differ from the file, %ld are 1.0 over another level, game %g..%g, file %g..%g", i,
             ps.n, ps.differ, ps.one, ps.lo, ps.hi, ps.tlo, ps.thi);
    t += b;
  }
  ss_log("[props] %s event property writes by index%s", when, t.empty() ? ": none" : t.c_str());
}

void InstallReverbDiag(HMODULE ev) {
  HMODULE fx = GetModuleHandleA("fmodex.dll");
  void* f;
  if (fx && (f = (void*)GetProcAddress(fx, "?setLevels@DSPConnection@FMOD@@QAG?AW4FMOD_RESULT@@W4FMOD_SPEAKER@@PAMH@Z")))
    Detour(f, (void*)HkConnSetLevels, (void**)&g_orig_conn_levels, "DSPConnection::setLevels");
  if (fx && (f = (void*)GetProcAddress(fx, "?setReverbProperties@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PBUFMOD_REVERB_CHANNELPROPERTIES@@@Z")))
    Detour(f, (void*)HkChanSetReverb, (void**)&g_orig_chan_reverb, "Channel::setReverbProperties");
  void* fe = (void*)GetProcAddress(ev, "?setReverbProperties@EventI@FMOD@@UAG?AW4FMOD_RESULT@@PBUFMOD_REVERB_CHANNELPROPERTIES@@@Z");
  void** slot = fe ? FindSlot(ev, fe, nullptr, nullptr) : nullptr;
  const bool ok = slot && SwapSlot(slot, (void*)HkEvISetReverb, (void**)&g_orig_evi_reverb);
  const char* test = getenv("SS_AUDIO_REVERB_TEST");
  g_reverb_mute = test && !strcmp(test, "mute") ? 1 : 0;
  ss_log("[revdiag] wrappers: connection levels %s, system reverb %s, channel reverb %s, event reverb slot %p %s%s",
         g_orig_conn_levels ? "in" : "-", g_orig_sys_reverb ? "in" : "-", g_orig_chan_reverb ? "in" : "-", (void*)slot,
         ok ? "in" : "-", g_reverb_mute ? "; TEST: reverb muted (Room -10000)" : "");
}

void InstallStartDiag(HMODULE ev) {
  {
    HMODULE fx = GetModuleHandleA("fmodex.dll");
    void* f = fx ? (void*)GetProcAddress(fx, "?setSpeakerLevels@Channel@FMOD@@QAG?AW4FMOD_RESULT@@W4FMOD_SPEAKER@@PAMH@Z") : nullptr;
    if (f) Detour(f, (void*)HkChSpeakerLevels, (void**)&g_orig_ch_levels, "Channel::setSpeakerLevels");
  }
  void* fn = (void*)GetProcAddress(ev, "?start@EventI@FMOD@@UAG?AW4FMOD_RESULT@@XZ");
  g_evi_info = Export<PfnEvIInfo>(ev, "?getInfo@EventI@FMOD@@UAG?AW4FMOD_RESULT@@PAHPAPADPAUFMOD_EVENT_INFO@@@Z");
  g_evi_category = Export<PfnEvCategory>(ev, "?getCategory@EventI@FMOD@@UAG?AW4FMOD_RESULT@@PAPAVEventCategory@2@@Z");
  g_evi_num_params = Export<PfnEvINum>(ev, "?getNumParameters@EventI@FMOD@@UAG?AW4FMOD_RESULT@@PAH@Z");
  g_evi_param = Export<PfnEvIParam>(ev, "?getParameterByIndex@EventI@FMOD@@UAG?AW4FMOD_RESULT@@HPAPAVEventParameter@2@@Z");
  g_evi_get_prop = Export<PfnEvProp>(ev, "?getPropertyByIndex@EventI@FMOD@@UAG?AW4FMOD_RESULT@@HPAX_N@Z");
  g_par_info = Export<PfnParInfo>(ev, "?getInfo@EventParameter@FMOD@@QAG?AW4FMOD_RESULT@@PAHPAPAD@Z");
  g_par_value = Export<PfnParValue>(ev, "?getValue@EventParameter@FMOD@@QAG?AW4FMOD_RESULT@@PAM@Z");
  void** slot = fn ? FindSlot(ev, fn, nullptr, nullptr) : nullptr;
  if (!slot || !g_evi_info || !g_evi_category || !g_evi_num_params || !g_evi_param || !g_evi_get_prop || !g_par_info ||
      !g_par_value) {
    ss_log("[diag] EventI::start slot %p or exports not found: no start log", (void*)slot);
    return;
  }
  ss_log("[diag] EventI::start slot %p %s", (void*)slot,
         SwapSlot(slot, (void*)HkEvIStart, (void**)&g_orig_evi_start) ? "wrapped" : "not writable");
}

// FMOD event properties that are ints (priority, max playbacks); the table carries every value as a float.
bool IntProp(int prop) { return prop == 11 || prop == 12 || prop == 13 || prop == 14; }
long GetProp(void* ev, int prop, float* v, int inst) {
  if (!IntProp(prop)) return g_ev_get_prop(ev, prop, v, inst);
  int i = 0;
  long r = g_ev_get_prop(ev, prop, &i, inst);
  *v = static_cast<float>(i);
  return r;
}
long SetProp(void* ev, int prop, float v, int inst) {
  if (!IntProp(prop)) return g_ev_set_prop(ev, prop, &v, inst);
  int i = static_cast<int>(lroundf(v));
  return g_ev_set_prop(ev, prop, &i, inst);
}

void CorrectEvent(void* ev, unsigned mode) {
  char* name = nullptr;
  int idx = 0;
  if (g_ev_info(ev, &idx, &name, nullptr) != 0 || !name) return;
  if (g_diag) DiagEvent(ev, name, mode);
  const uint32_t nh = Fnv1a(name);
  int first = -1;
  for (int i = 0; i < sscm::kCorrectionCount && first < 0; ++i)
    if (sscm::kCorrections[i].name_hash == nh) first = i;
  if (first < 0) return;  // the usual case: not an event the table names
  void* group = nullptr;
  void* project = nullptr;
  char* pname = nullptr;
  if (g_ev_group(ev, &group) != 0 || !group || g_group_project(group, &project) != 0 || !project ||
      g_project_info(project, &idx, &pname) != 0 || !pname)
    return;
  const uint32_t ph = Fnv1a(pname);
  for (int i = first; i < sscm::kCorrectionCount; ++i) {
    const sscm::Correction& c = sscm::kCorrections[i];
    if (c.name_hash != nh || c.project_hash != ph) continue;
    float v = 0.0f;
    // CONSOLE: a PC value becomes the console one; NATIVE (switched back live): a console value we put in goes back
    const bool on = g_cm_on != 0;
    const float from = on ? c.pc : c.console;
    float want = on ? c.console : c.pc;
    // the template: every instance of the event
    long rs = -1;
    if (GetProp(ev, c.prop, &v, 0) == 0 && Near(v, from) && (rs = SetProp(ev, c.prop, want, 0)) == 0) {
      if (on) InterlockedIncrement(&g_cm_hits[i]);
      if (InterlockedIncrement(&g_cm_logged) <= 60)
        ss_log("[console_mix] %s/%s property %d: %.4f -> %.4f (event%s)", pname, name, c.prop, v, want, on ? "" : ", native again");
    } else if (rs != -1 && rs != 0) {
      ss_log("[console_mix] %s/%s property %d: FMOD refused %.4f (error %ld)", pname, name, c.prop, want, rs);
    }
    // the instance in hand
    if (GetProp(ev, c.prop, &v, 1) == 0 && Near(v, from) && SetProp(ev, c.prop, want, 1) == 0)
      InterlockedIncrement(&g_cm_instances);
  }
}

// Engine evidence (always on, capped): the opponents' engines are one event per CPU car, "<car>/Tonic/<car>" in the
// car's own project (audio/release/vehicles/cpu/<car>.fev in resident.ark, category CPUEngine; the game's car-audio
// init at 0x85C2DC fetches it by name and gives up quietly when the fetch fails). A refused fetch by name and the
// first CPUEngine fetches are logged, so one race shows whether the game asks for them and whether FMOD has them.
volatile LONG g_ev_fail_logged = 0, g_cpu_fetch_logged = 0;
void EngineEvidence(int k, uintptr_t a, unsigned mode, long r, void* ev) {
  const bool named = k == 0 || k == 2 || k == 6;  // the getEvent(const char*) slots
  if (r != 0) {
    if (named && a && InterlockedIncrement(&g_ev_fail_logged) <= 40)
      ss_log("[events] getEvent '%.96s' (mode %#x) refused: FMOD error %ld", reinterpret_cast<const char*>(a), mode, r);
    return;
  }
  if (!ev || !g_ev_category || !g_cat_info || g_cpu_fetch_logged >= 24) return;
  void* cat = nullptr;
  char* cname = nullptr;
  int ci = 0;
  if (g_ev_category(ev, &cat) != 0 || !cat || g_cat_info(cat, &ci, &cname) != 0 || !cname) return;
  if (_stricmp(cname, "CPUEngine") != 0) return;
  if (InterlockedIncrement(&g_cpu_fetch_logged) > 24) return;
  char* name = nullptr;
  int idx = 0;
  float vol = -1.0f;
  if (g_ev_info) g_ev_info(ev, &idx, &name, nullptr);
  if (g_ev_get_prop) g_ev_get_prop(ev, 1, &vol, 1);
  ss_log("[events] CPUEngine event fetched: '%s'%s%.96s%s (mode %#x), instance volume %.3f", name ? name : "?",
         named ? " as '" : "", named && a ? reinterpret_cast<const char*>(a) : "", named ? "'" : "", mode, vol);
}

#define SYM_EV(name, args) "?" name "@Event@FMOD@@QAG?AW4FMOD_RESULT@@" args

// ------------------------------------------------------------ opponent engines (audio_cpu_engines)
//
// Observed in the PC exe (reverse engineering), the same in the stock game: every frame the car-audio update 0x85BDA0
// drives a CPU car's engine sound (branch 0x85C0CB) through the game's 3D sound object kept at car-audio+0x1C (one of
// 64 pooled objects of the class with vtable 0xC32A2C, handed out by 0x8A4CD0). Until a "started" byte at
// car-audio+9 is set it calls the object's play slot (vtable+4, 0x8A6F20, call at 0x85C15E) and then sets the byte,
// whatever the play did: the play is asked for once, never again. The play slot has a range gate: when the object's
// byte +0x382 is set, it starts the sound only if some listener is within the object's radius (+0x29C) of the sound
// (0x8A63F0: listener count from 0x511410 on the audio manager 0xD8429C); otherwise it returns without starting.
// The real start (0x8A7700) asks the sound's game-side event object to start (0x86A920 sets its request word +0x638
// to 1; the audio layer's update 0x86B8B0 then calls Event::start). So an opponent engine refused once by the gate
// stays silent for the whole race, and no Event::start is ever made for a CPUEngine event.
//
// The fix swaps the play slot of that vtable (in .rdata, a value; no code is written): when the call comes from the
// CPU branch (return address 0x85C163) and the gate would refuse, the real start is made anyway. FMOD's own distance
// model (the event's 10..1000 m roll-off) and its maxplaybacks still decide what is heard. Every other caller goes
// to the game's own play slot unchanged. Each such play is logged (capped), with the gate's inputs and the result.
//
// Why no opponent engine ever got that far (found after the above, from the code): each CPU car's engine is its own
// event project (Audio/Release/Vehicles/CPU/<car>.fev + .fsb). At audio start 0x7F5900 makes two project groups:
// "Level" (1 slot, buffer [0xD3DBBC]) and "CPUCar" (7 slots, each with a sample-bank buffer of [0xD3DBC0] bytes,
// allocated once from the "SoundBank" heap by 0x868A10). Loading a project (0x8699C0) reads each in-memory bank
// whole into its slot's buffer (0x8695D0) and refuses when the .fsb is larger than the buffer (0x869712: cmp edi,
// ecx; ja 0x8696C0); 0x8699C0 then unloads the project (0x8697C0) before its events are registered (0x869090 ->
// 0x867CD0), all without a word. [0xD3DBC0] is 0x19999 (104,857 bytes) on PC, the Xbox 360 value, sized for the
// Xbox banks (XMA, 57..94 KB). The PC car banks are MP2, 148..203 KB: every one is refused, so "<car>/Tonic/<car>"
// is never registered, the sound handout 0x8A4CD0 returns null at 0x85C302, car-audio+0x1C stays null and the engine
// update 0x85BDA0 leaves at 0x85C0D0. The fix writes that one .data value (to 256 KB) before the groups are made;
// the "SoundBank" heap is 0x2480000 bytes on PC and holds about 24 MB in a race, so 7 x 256 KB fits.
constexpr uintptr_t kCarBankSize = 0xD3DBC0;    // .data: bytes of each CPUCar slot's sample-bank buffer
constexpr uint32_t kCarBankShipped = 0x19999;   // as shipped (the Xbox value)
constexpr uint32_t kCarBankWanted = 0x40000;    // 262,144: the largest PC car bank is 202,848 bytes
constexpr uintptr_t kProjectMgrPtr = 0xD91C58;  // the project-group manager; non-null once audio has started
constexpr uintptr_t kSndVtbl = 0xC32A2C;        // the 3D sound object's vtable (.rdata)
constexpr uintptr_t kSndPlay = 0x8A6F20;        // its slot +4: play (range gate, then the real start)
constexpr uintptr_t kSndInRange = 0x8A63F0;     // bool: a listener within the radius
constexpr uintptr_t kSndStart = 0x8A7700;       // bool: the real start
constexpr uintptr_t kCpuPlayRet = 0x85C163;     // return address of the play call in the CPU-engine branch
constexpr uintptr_t kAudioMgrPtr = 0xD8429C;    // the audio manager (listeners)
constexpr uintptr_t kListenerCount = 0x511410;  // unsigned (thiscall on the manager): listeners
typedef bool(__thiscall* PfnSndBool)(void* self);
typedef unsigned(__thiscall* PfnMgrCount)(void* mgr);
PfnSndBool g_orig_snd_play = nullptr;
volatile LONG g_cpu_engine_fix = 1;
volatile LONG g_cpu_play_logged = 0, g_cpu_bypassed = 0;

__attribute__((force_align_arg_pointer)) bool __fastcall HkSndPlay(void* self, void* /*edx*/) {
  if (reinterpret_cast<uintptr_t>(__builtin_return_address(0)) != kCpuPlayRet) return g_orig_snd_play(self);
  auto* o = static_cast<uint8_t*>(self);
  const bool gate = o[0x382] != 0;
  const bool in_range = !gate || reinterpret_cast<PfnSndBool>(kSndInRange)(self);
  const bool bypass = !in_range && g_cpu_engine_fix;
  uint8_t* evo = *reinterpret_cast<uint8_t**>(o + 8);  // the game-side event object (null: no event)
  const long req0 = evo ? *reinterpret_cast<long*>(evo + 0x638) : -1;
  bool started = false;
  if (in_range || bypass) started = reinterpret_cast<PfnSndBool>(kSndStart)(self);
  if (bypass) InterlockedIncrement(&g_cpu_bypassed);
  if (InterlockedIncrement(&g_cpu_play_logged) <= 64) {
    void* mgr = *reinterpret_cast<void**>(kAudioMgrPtr);
    const unsigned listeners = mgr ? reinterpret_cast<PfnMgrCount>(kListenerCount)(mgr) : 0;
    ss_log("[cpu_engine] opponent engine play (sound %p): range gate %s, radius %.1f, %u listener(s), in range %s -> "
           "%s; start %s; event %p (FMOD event %p) start request %ld -> %ld; skip flags %d/%d level %.4f; bypassed %ld",
           self, gate ? "on" : "off", *reinterpret_cast<float*>(o + 0x29C), listeners, in_range ? "yes" : "no",
           in_range ? "played as the game does" : bypass ? "refused by the gate, started anyway" : "refused (as shipped)",
           started ? "made" : "NOT made", evo, evo ? *reinterpret_cast<void**>(evo + 0xC) : nullptr, req0,
           evo ? *reinterpret_cast<long*>(evo + 0x638) : -1L, o[0x282], o[0x283], *reinterpret_cast<float*>(o + 0x510),
           (long)g_cpu_bypassed);
  }
  return started;
}

// audio_music_log = true (default false; SS_AUDIO_MUSIC_LOG): a time line of what the game does with its music, for a
// report of sounds that should not play. Music is played by the music manager (update 0x83DC60: race sections
// "Music/<track>/RaceSection0N" from 0x83D1E0, the finish stingers from 0x83D0A0, then "ResultScreen" after the post
// race timer) through the game's event objects, whose update 0x86B8B0 makes the FMOD calls: Event::setPaused(true),
// Event::start, the set-up, Event::setPaused(false); Event::stop when the voice is let go. The race update 0x83D6C0
// also reads the fade of the "music" mix node (0x7ECFB0 -> 0x86C2D0): under 0.05 it stops the playing race section,
// and once the fade is above 0.05 again it starts a section anew. So the log holds, with the log's millisecond
// times: every start / stop / pause of an event in the music or Speech category (the game's imports of Event::start,
// stop, setPaused, setVolume, values in .rdata), with the event's own volume and what the game last asked of the
// music, Speech and master channel groups; and every move of those three channel groups across 0.05 or by 3 dB
// (ChannelGroup::setVolume, the mixer's wrapper: the game's fades, mutes and side-chain ducking). Capped.
volatile LONG g_music_log = 0;
volatile LONG g_ml_lines = 0;
constexpr LONG kMusicLogCap = 6000;
constexpr uintptr_t kIatEventStop = 0xB9B424, kIatEventSetPaused = 0xB9B3F8, kIatEventSetVolume = 0xB9B434;
typedef long(__stdcall* PfnEvStop)(void* ev, bool immediate);
typedef long(__stdcall* PfnEvSetPaused)(void* ev, bool paused);
typedef long(__stdcall* PfnEvSetVolume)(void* ev, float v);
typedef long(__stdcall* PfnEvGetVolume)(void* ev, float* v);
typedef long(__stdcall* PfnEvGetPaused)(void* ev, bool* paused);
PfnEvStop g_orig_ev_stop = nullptr;
PfnEvSetPaused g_orig_ev_set_paused = nullptr;
PfnEvSetVolume g_orig_ev_set_volume = nullptr;
PfnEvGetVolume g_ml_get_volume = nullptr;
PfnEvGetPaused g_ml_get_paused = nullptr;
PfnEvInfo g_ml_info = nullptr;
PfnEvCategory g_ml_category = nullptr;
PfnCatInfo g_ml_cat_info = nullptr;

// The last value logged per event or channel group (round robin); under g_cs.
struct MlLast { void* key; float v; };
MlLast g_ml_last[64];
int g_ml_next = 0;

// True when `v` is worth a line against the last one logged for `key`: the first, a move across 0.05 (where the
// music manager stops and starts race sections), or 3 dB. `*from` gets the last logged value (-1: none).
bool MlMoved(void* key, float v, float* from) {
  bool moved = true;
  *from = -1.0f;
  EnterCriticalSection(&g_cs);
  MlLast* e = nullptr;
  for (auto& x : g_ml_last)
    if (x.key == key) e = &x;
  if (e) {
    const float a = e->v, lo = a < v ? a : v, hi = a < v ? v : a;
    *from = a;
    moved = (a < 0.05f) != (v < 0.05f) || (hi > 0.0f && lo < hi * 0.7079f && hi - lo > 0.0005f);
  } else {
    e = &g_ml_last[g_ml_next];
    g_ml_next = (g_ml_next + 1) % 64;
    e->key = key;
  }
  if (moved) e->v = v;
  LeaveCriticalSection(&g_cs);
  return moved;
}

// What the game last asked of a category's channel group (-1: not known).
float MlCatReq(const char* name) {
  float v = -1.0f;
  EnterCriticalSection(&g_cs);
  for (int i = 0; i < g_ncats; ++i)
    if (g_cats[i].have_req && !_stricmp(g_cats[i].name, name)) v = g_cats[i].req;
  LeaveCriticalSection(&g_cs);
  return v;
}

// Category of an event when it is one the log follows: "music" or "Speech"; else null. `*name` gets the event's name.
const char* MlEvent(void* ev, char** name) {
  char* cname = nullptr;
  void* cat = nullptr;
  int idx = 0, ci = 0;
  *name = nullptr;
  if (!g_ml_info || !g_ml_category || !g_ml_cat_info || g_ml_lines >= kMusicLogCap) return nullptr;
  if (g_ml_category(ev, &cat) != 0 || !cat || g_ml_cat_info(cat, &ci, &cname) != 0 || !cname) return nullptr;
  if (_stricmp(cname, "music") && _stricmp(cname, "Speech")) return nullptr;
  g_ml_info(ev, &idx, name, nullptr);
  return cname;
}

void MlEventLine(const char* what, void* ev, const char* name, const char* cname, long rc) {
  if (InterlockedIncrement(&g_ml_lines) > kMusicLogCap) return;
  float vol = -1.0f;
  bool paused = false;
  if (g_ml_get_volume) g_ml_get_volume(ev, &vol);
  const long rp = g_ml_get_paused ? g_ml_get_paused(ev, &paused) : -1;
  ss_log("[music_log] %s '%s' (cat %s, event %p) -> %ld; event volume %.3f, paused %d; groups asked: music %.3f, "
         "Speech %.3f, master %.3f", what, name ? name : "?", cname, ev, rc, vol, rp == 0 ? (int)paused : -1,
         MlCatReq("music"), MlCatReq("Speech"), MlCatReq("master"));
}

// At a start: the event's own channel group and every parent up to the top (name, the volume FMOD holds), so the
// log says through which groups the event really plays. Then the event is watched for 8 s of updates (MlPoll): each
// change of Event::getState and of its group chain's volumes product is a line, so the moment it falls silent or
// stops playing, and which group did it, is in the log.
void* volatile g_ml_watch = nullptr;
volatile LONG g_ml_watch_left = 0;
unsigned g_ml_watch_state = 0;
float g_ml_watch_gain = -1.0f;
float MlChain(void* ev, char* out, size_t cap) {
  static auto ev_cg = reinterpret_cast<long(__stdcall*)(void*, void**)>(
      GetProcAddress(GetModuleHandleA("fmod_event.dll"), SYM_EV("getChannelGroup", "PAPAVChannelGroup@2@@Z")));
  static auto parent = FxSym<long(__stdcall*)(void*, void**)>("?getParentGroup@ChannelGroup@FMOD@@QAG?AW4FMOD_RESULT@@PAPAV12@@Z");
  static auto cgname = FxSym<long(__stdcall*)(void*, char*, int)>("?getName@ChannelGroup@FMOD@@QAG?AW4FMOD_RESULT@@PADH@Z");
  void* cg = nullptr;
  float gain = 1.0f;
  size_t len = 0;
  if (out && cap) out[0] = 0;
  if (!ev_cg || !parent || !cgname || !g_cg_get || ev_cg(ev, &cg) != 0 || !cg) return -1.0f;
  for (int d = 0; d < 8 && cg; ++d) {
    char n[48] = "?";
    float v = -1.0f;
    cgname(cg, n, sizeof n);
    g_cg_get(cg, &v);  // the trampoline: FMOD's own value
    if (v >= 0.0f) gain *= v;
    if (out && len < cap) len += snprintf(out + len, cap - len, "%s'%s' %p %.4f", d ? " <- " : "", n, cg, v);
    void* up = nullptr;
    if (parent(cg, &up) != 0 || up == cg) break;
    cg = up;
  }
  return gain;
}

void MlPoll() {
  void* ev = g_ml_watch;
  if (!ev || g_ml_watch_left <= 0) return;
  static auto state = reinterpret_cast<long(__stdcall*)(void*, unsigned*)>(
      GetProcAddress(GetModuleHandleA("fmod_event.dll"), SYM_EV("getState", "PAI@Z")));
  unsigned st = 0;
  const long r = state ? state(ev, &st) : -1;
  char chain[400];
  const float gain = r == 0 ? MlChain(ev, chain, sizeof chain) : -1.0f;
  const float a = g_ml_watch_gain, lo = a < gain ? a : gain, hi = a < gain ? gain : a;
  const bool moved = (a < 0.0f) != (gain < 0.0f) || (hi > 0.0f && lo < hi * 0.7079f);
  if (r != 0 || st != g_ml_watch_state || moved) {
    g_ml_watch_state = st;
    g_ml_watch_gain = gain;
    if (InterlockedIncrement(&g_ml_lines) <= kMusicLogCap)
      ss_log("[music_log] watched event %p: getState rc %ld state %#x (0x8 playing, 0x10 channels active), groups' "
             "product %.4f: %s", ev, r, st, gain, r == 0 ? chain : "-");
  }
  if (r != 0 || InterlockedDecrement(&g_ml_watch_left) <= 0) g_ml_watch = nullptr;
}

void MlStarted(void* ev) {
  char chain[400];
  const float gain = MlChain(ev, chain, sizeof chain);
  if (InterlockedIncrement(&g_ml_lines) <= kMusicLogCap)
    ss_log("[music_log] event %p plays through (group, FMOD's volume): %s; product %.4f", ev, chain, gain);
  g_ml_watch_state = 0xFFFFFFFFu;
  g_ml_watch_gain = gain;
  g_ml_watch_left = 480;
  g_ml_watch = ev;
}

HOOK long HkEventStop(void* ev, bool immediate) {
  char* name = nullptr;
  const char* cname = g_music_log ? MlEvent(ev, &name) : nullptr;
  char keep[64] = "?";
  if (name) snprintf(keep, sizeof keep, "%s", name);  // the name may go with the instance
  const long r = g_orig_ev_stop(ev, immediate);
  if (cname) MlEventLine(immediate ? "stop (at once)" : "stop (fade)", ev, keep, cname, r);
  return r;
}

HOOK long HkEventSetPaused(void* ev, bool paused) {
  const long r = g_orig_ev_set_paused(ev, paused);
  char* name = nullptr;
  if (const char* cname = g_music_log ? MlEvent(ev, &name) : nullptr)
    MlEventLine(paused ? "pause on" : "pause off", ev, name, cname, r);
  return r;
}

HOOK long HkEventSetVolume(void* ev, float v) {
  const long r = g_orig_ev_set_volume(ev, v);
  char* name = nullptr;
  float from = -1.0f;
  if (const char* cname = g_music_log ? MlEvent(ev, &name) : nullptr)
    if (MlMoved(ev, v, &from) && InterlockedIncrement(&g_ml_lines) <= kMusicLogCap)
      ss_log("[music_log] event volume '%s' (cat %s, event %p) %.3f -> %.3f (rc %ld)", name ? name : "?", cname, ev,
             from, v, r);
  return r;
}

// From the ChannelGroup::setVolume wrapper: the game's request `v` for the category `cname`.
void MlGroupVolume(void* cg, const char* cname, float v) {
  if (_stricmp(cname, "music") && _stricmp(cname, "Speech") && _stricmp(cname, "master")) return;
  float from = -1.0f;
  if (!MlMoved(cg, v, &from) || InterlockedIncrement(&g_ml_lines) > kMusicLogCap) return;
  ss_log("[music_log] group %s: the game asks %.4f (%.1f dB), was %.4f", cname, v,
         v > 0.000001f ? 20.0 * log10(v) : -120.0, from);
}

// Event::start through the game's import (IAT 0xB9B43C in .rdata, a value): every refused start is logged with the
// event, its category and FMOD's error, and every CPUEngine start (capped).
typedef long(__stdcall* PfnEvStart)(void* ev);
constexpr uintptr_t kIatEventStart = 0xB9B43C;
PfnEvStart g_orig_ev_start = nullptr;
PfnEvInfo g_es_info = nullptr;
PfnEvCategory g_es_category = nullptr;
PfnCatInfo g_es_cat_info = nullptr;
volatile LONG g_es_fail_logged = 0, g_es_cpu_logged = 0;

HOOK long HkEventStart(void* ev) {
  const long r = g_orig_ev_start(ev);
  if (g_music_log) {
    char* mname = nullptr;
    if (const char* mcat = MlEvent(ev, &mname)) {
      MlEventLine("start", ev, mname, mcat, r);
      if (r == 0) MlStarted(ev);
    }
  }
  if (r == 0 && g_es_cpu_logged >= 40) return r;
  if (r != 0 && g_es_fail_logged >= 60) return r;
  char* name = nullptr;
  char* cname = nullptr;
  void* cat = nullptr;
  int idx = 0, ci = 0;
  if (g_es_info) g_es_info(ev, &idx, &name, nullptr);
  if (g_es_category && g_es_cat_info && g_es_category(ev, &cat) == 0 && cat) g_es_cat_info(cat, &ci, &cname);
  const bool cpu = cname && !_stricmp(cname, "CPUEngine");
  if (r != 0 && InterlockedIncrement(&g_es_fail_logged) <= 60)
    ss_log("[events] Event::start '%s' (cat %s) refused: FMOD error %ld", name ? name : "?", cname ? cname : "?", r);
  else if (r == 0 && cpu && InterlockedIncrement(&g_es_cpu_logged) <= 40)
    ss_log("[events] Event::start '%s' (cat CPUEngine): ok", name ? name : "?");
  return r;
}

bool WantCpuEngines() {
  if (const char* e = getenv("SS_AUDIO_CPU_ENGINES")) return ParseBool(e, true);
  if (!ss_conf_present()) return false;
  return ParseBool(ss_conf("audio_cpu_engines"), true);
}

// The pool of 3D sound objects (observed: 0x8A5370 fills it): manager at [0xD96AC0], 64 objects of 0x580 bytes from
// +0x10, the free ones listed at +0x16010 with their count at +0x16110. Read only, from our thread, for the log.
constexpr uintptr_t kSndPoolPtr = 0xD96AC0;
constexpr int kSndPoolN = 64;
constexpr uintptr_t kSndSize = 0x580;

bool Readable(const void* p, size_t n) {
  MEMORY_BASIC_INFORMATION mi;
  if (!p || !VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
    return false;
  return reinterpret_cast<uintptr_t>(p) + n <= reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize;
}

// Every 2 s for 15 min: a summary line when it changes (free objects, vptrs, gate set, started), and one line per
// object the first time it is seen in use and whenever its gate / started / request fields change (capped).
void DumpSoundPool() {
  static uint32_t last_key[kSndPoolN];
  static uint32_t last_summary = 0;
  static LONG lines = 0;
  auto* mgr = *reinterpret_cast<uint8_t* volatile*>(kSndPoolPtr);
  if (!mgr || !Readable(mgr, 0x16114)) return;
  const int nfree = *reinterpret_cast<volatile int*>(mgr + 0x16110);
  if (nfree < 0 || nfree > kSndPoolN) return;
  bool is_free[kSndPoolN] = {};
  for (int k = 0; k < nfree; ++k) {
    const uintptr_t f = *reinterpret_cast<volatile uintptr_t*>(mgr + 0x16010 + 4 * k);
    const uintptr_t i = (f - reinterpret_cast<uintptr_t>(mgr + 0x10)) / kSndSize;
    if (f >= reinterpret_cast<uintptr_t>(mgr + 0x10) && i < (uintptr_t)kSndPoolN) is_free[i] = true;
  }
  int used = 0, other_vptr = 0, gated = 0, started = 0;
  uintptr_t odd = 0;
  for (int i = 0; i < kSndPoolN; ++i) {
    if (is_free[i]) continue;
    uint8_t* o = mgr + 0x10 + i * kSndSize;
    ++used;
    const uintptr_t vp = *reinterpret_cast<volatile uintptr_t*>(o);
    if (vp != kSndVtbl) { ++other_vptr; odd = vp; }
    const uint8_t g = o[0x382], st = o[0x99];
    gated += g != 0;
    started += st != 0;
    uint8_t* evo = *reinterpret_cast<uint8_t* volatile*>(o + 8);
    long req = -1;
    void* fev = nullptr;
    if (evo && Readable(evo + 0xC, 0x630)) { req = *reinterpret_cast<volatile long*>(evo + 0x638); fev = *reinterpret_cast<void**>(evo + 0xC); }
    const uint32_t key = (uint32_t)(uintptr_t)evo ^ (g << 1) ^ (st << 2) ^ ((uint32_t)req << 3) ^ 1u;
    if (key != last_key[i] && lines < 300) {
      last_key[i] = key;
      ++lines;
      ss_log("[cpu_engine] pool #%d %p vptr %#lx: gate %d radius %.1f started %d skip %d/%d level %.4f; event %p (FMOD %p) "
             "request %ld", i, o, (unsigned long)vp, g, *reinterpret_cast<float*>(o + 0x29C), st, o[0x282], o[0x283],
             *reinterpret_cast<float*>(o + 0x510), evo, fev, req);
    }
  }
  for (int i = 0; i < kSndPoolN; ++i) if (is_free[i]) last_key[i] = 0;
  const uint32_t summary = (uint32_t)used | (uint32_t)other_vptr << 8 | (uint32_t)gated << 16 | (uint32_t)started << 24;
  if (summary != last_summary) {
    last_summary = summary;
    ss_log("[cpu_engine] sound pool: %d of %d in use, %d with another vptr%s%#lx, %d with the range gate set, %d started; "
           "opponent plays seen %ld, gate bypassed %ld", used, kSndPoolN, other_vptr, other_vptr ? " " : " ",
           (unsigned long)odd, gated, started, (long)g_cpu_play_logged, (long)g_cpu_bypassed);
  }
}

// The car-audio slots (observed: 0x8359F0 walks them): manager [0xD84298], 8 slots of 0x3F8 bytes from +0x180; +0x20C
// in use, +0x224 human car; the engine part at +0x35C (set up by 0x85C170, driven each frame by 0x85BDA0): +8 set up,
// +9 play asked, +0x14 the human engine's sound, +0x1C the opponent engine's sound (0x8A4CD0 by name; null when that
// failed), +0x20 the human engine's synth object (non-null sends the update down the human branch at 0x85BDC3).
constexpr uintptr_t kCarAudioPtr = 0xD84298;
void DumpCarAudio() {
  static uint32_t last[8];
  static LONG lines = 0;
  auto* mgr = *reinterpret_cast<uint8_t* volatile*>(kCarAudioPtr);
  if (!mgr || !Readable(mgr + 0x180, 8 * 0x3F8)) return;
  for (int i = 0; i < 8; ++i) {
    uint8_t* sl = mgr + 0x180 + i * 0x3F8;
    uint8_t* en = sl + 0x35C;
    const uint8_t act = sl[0x20C], human = sl[0x224], setup = en[8], asked = en[9];
    const uintptr_t s14 = *reinterpret_cast<volatile uintptr_t*>(en + 0x14), s1c = *reinterpret_cast<volatile uintptr_t*>(en + 0x1C),
                    o20 = *reinterpret_cast<volatile uintptr_t*>(en + 0x20);
    const uint32_t key = 1u ^ act ^ (human << 1) ^ (setup << 2) ^ (asked << 3) ^ (uint32_t)s14 * 3 ^ (uint32_t)s1c * 5 ^ (uint32_t)o20 * 7;
    if (key == last[i] || lines >= 200) continue;
    last[i] = key;
    ++lines;
    uintptr_t vp = 0;
    if (s1c && Readable(reinterpret_cast<void*>(s1c), 4)) vp = *reinterpret_cast<volatile uintptr_t*>(s1c);
    ss_log("[cpu_engine] car slot %d: in use %d, human %d; engine set up %d, play asked %d; human sound %#lx, synth %#lx; "
           "opponent sound %#lx (vptr %#lx)", i, act, human, setup, asked, (unsigned long)s14, (unsigned long)o20,
           (unsigned long)s1c, (unsigned long)vp);
  }
}

// The sound's set-up (vtable slot +0x24, 0x8A6310, called by the pool handout 0x8A4CD0 with the resolved event name):
// fails when the cull check 0x8A6260 says no listener is near (flags of the +0x270 block, +0x284), when the event
// lookup 0x8A7840 fails, or, with the voice unit wanted (+0x3B4), when the unit (+0x550) is not made. A failure hands
// the object back and the car keeps no sound. Logged: every failure and every Tonic set-up (capped).
constexpr uintptr_t kSndSetup = 0x8A6310, kSndCull = 0x8A6260;
typedef bool(__thiscall* PfnSndSetup)(void* self, const char* name, int a2);
PfnSndSetup g_orig_snd_setup = nullptr;
volatile LONG g_setup_logged = 0;

__attribute__((force_align_arg_pointer)) bool __fastcall HkSndSetup(void* self, void* /*edx*/, const char* name, int a2) {
  const bool culled = reinterpret_cast<PfnSndBool>(kSndCull)(self);
  const bool ok = g_orig_snd_setup(self, name, a2);
  const bool tonic = name && Readable(name, 1) && strstr(name, "Tonic");
  if ((!ok || tonic) && InterlockedIncrement(&g_setup_logged) <= 80) {
    auto* o = static_cast<uint8_t*>(self);
    void* mgr = *reinterpret_cast<void**>(kAudioMgrPtr);
    const unsigned listeners = mgr ? reinterpret_cast<PfnMgrCount>(kListenerCount)(mgr) : 0;
    ss_log("[cpu_engine] sound set-up '%.96s' %s: cull check %s (%u listener(s), radius %.1f, flag +0x284 %d), event %s, "
           "voice unit wanted %d made %d", name && Readable(name, 1) ? name : "?", ok ? "ok" : "FAILED",
           culled ? "says too far" : "passes", listeners, *reinterpret_cast<float*>(o + 0x29C), o[0x284],
           *reinterpret_cast<void**>(o + 8) ? "found" : "NOT found", o[0x3B4], *reinterpret_cast<void**>(o + 0x550) != nullptr);
  }
  return ok;
}

struct CpuEngineArgs { HMODULE ev; };

DWORD WINAPI CpuEngineWorker(LPVOID arg) {
  HMODULE ev = static_cast<CpuEngineArgs*>(arg)->ev;
  delete static_cast<CpuEngineArgs*>(arg);
  g_es_info = Export<PfnEvInfo>(ev, SYM_EV("getInfo", "PAHPAPADPAUFMOD_EVENT_INFO@@@Z"));
  g_es_category = Export<PfnEvCategory>(ev, SYM_EV("getCategory", "PAPAVEventCategory@2@@Z"));
  g_es_cat_info = Export<PfnCatInfo>(ev, SYM_EC("getInfo", "PAHPAPAD@Z"));
  void* start = (void*)GetProcAddress(ev, SYM_EV("start", "XZ"));
  void* volatile* iat = reinterpret_cast<void* volatile*>(kIatEventStart);
  void* volatile* slot = reinterpret_cast<void* volatile*>(kSndVtbl + 4);
  // The protector fills the game's import table once it has unpacked the image (at start-up the slot still holds
  // the name's offset): wait for that, so nothing we write is laid over afterwards.
  int i = 0;
  for (; i < 1200 && *iat != start; ++i) Sleep(100);
  if (*iat != start) {
    ss_log("[events] Event::start import never filled (holds %p, export %p): not logged", *iat, start);
  } else {
    g_orig_ev_start = reinterpret_cast<PfnEvStart>(start);
    ss_log("[events] Event::start import filled after %.1f s; %s", i / 10.0,
           ss_write_word((void*)iat, reinterpret_cast<uintptr_t>(HkEventStart))
               ? "wrapped (refusals and CPUEngine starts logged)" : "not writable: not logged");
  }
  if (g_music_log && *iat == reinterpret_cast<void*>(HkEventStart)) {
    g_ml_info = g_es_info;
    g_ml_category = g_es_category;
    g_ml_cat_info = g_es_cat_info;
    g_ml_get_volume = Export<PfnEvGetVolume>(ev, SYM_EV("getVolume", "PAM@Z"));
    g_ml_get_paused = Export<PfnEvGetPaused>(ev, SYM_EV("getPaused", "PA_N@Z"));
    struct Imp { uintptr_t iat; const char* sym; void* hook; void** orig; const char* what; };
    const Imp imps[] = {
        {kIatEventStop, SYM_EV("stop", "_N@Z"), (void*)HkEventStop, (void**)&g_orig_ev_stop, "stop"},
        {kIatEventSetPaused, SYM_EV("setPaused", "_N@Z"), (void*)HkEventSetPaused, (void**)&g_orig_ev_set_paused, "setPaused"},
        {kIatEventSetVolume, SYM_EV("setVolume", "M@Z"), (void*)HkEventSetVolume, (void**)&g_orig_ev_set_volume, "setVolume"},
    };
    std::string done;
    for (const Imp& m : imps) {
      void* exp = (void*)GetProcAddress(ev, m.sym);
      void* volatile* at = reinterpret_cast<void* volatile*>(m.iat);
      bool ok = false;
      if (exp && *at == exp) {  // the import holds this export (this exe): the original is set before the slot moves
        *m.orig = exp;
        ok = ss_write_word((void*)at, reinterpret_cast<uintptr_t>(m.hook));
      }
      done += std::string(" ") + m.what + (ok ? " wrapped" : " NOT wrapped");
    }
    ss_log("[music_log] on: music and Speech events (Event::start wrapped,%s) and the music / Speech / master groups' "
           "volumes are logged, %ld lines at most", done.c_str(), (long)kMusicLogCap);
  } else if (g_music_log) {
    ss_log("[music_log] Event::start is not wrapped: only the groups' volumes are logged");
  }
  // The car-bank buffer size: written once the image is unpacked and before the game makes the CPUCar group.
  {
    volatile uint32_t* size = reinterpret_cast<volatile uint32_t*>(kCarBankSize);
    void* volatile* mgr = reinterpret_cast<void* volatile*>(kProjectMgrPtr);
    // Up to 30 s more for the shipped value to show (the image's data, should the protector lay it down late).
    for (int j = 0; j < 300 && *size != kCarBankShipped && !*mgr; ++j) Sleep(100);
    const bool late = *mgr != nullptr;
    if (!g_cpu_engine_fix) {
      ss_log("[cpu_engine] car-bank buffer left at %lu bytes (as shipped: no PC car bank fits, opponents stay silent)",
             (unsigned long)*size);
    } else if (*size != kCarBankShipped) {
      ss_log("[cpu_engine] car-bank buffer size holds %#lx, not %#lx: not this exe, left alone", (unsigned long)*size,
             (unsigned long)kCarBankShipped);
    } else {
      const bool wrote = ss_write_word((void*)kCarBankSize, kCarBankWanted);
      ss_log("[cpu_engine] car-bank buffer %lu -> %lu bytes per opponent slot (.data %#lx): %s%s",
             (unsigned long)kCarBankShipped, (unsigned long)*size, (unsigned long)kCarBankSize,
             wrote ? "written" : "not writable",
             late ? "; the audio system was already up, so this run may still use the old size" : ", before audio start");
    }
  }
  if (reinterpret_cast<uintptr_t>(*slot) != kSndPlay) {
    ss_log("[cpu_engine] the 3D sound play slot holds %p, not %#x: not this exe, nothing done", *slot,
           (unsigned)kSndPlay);
    return 0;
  }
  g_orig_snd_play = reinterpret_cast<PfnSndBool>(kSndPlay);
  const bool ok = ss_write_word((void*)slot, reinterpret_cast<uintptr_t>(HkSndPlay));
  void* volatile* setup = reinterpret_cast<void* volatile*>(kSndVtbl + 0x24);
  if (reinterpret_cast<uintptr_t>(*setup) == kSndSetup) {
    g_orig_snd_setup = reinterpret_cast<PfnSndSetup>(kSndSetup);
    ss_log("[cpu_engine] sound set-up slot %s", ss_write_word((void*)setup, reinterpret_cast<uintptr_t>(HkSndSetup))
                                                     ? "wrapped (failures and Tonic set-ups logged)" : "not writable");
  } else {
    ss_log("[cpu_engine] sound set-up slot holds %p, not %#x: not wrapped", *setup, (unsigned)kSndSetup);
  }
  ss_log("[cpu_engine] opponent engines: %s; play slot %s", g_cpu_engine_fix ? "on (a play the range gate refuses is "
         "started anyway)" : "off (as shipped, logged only)", ok ? "wrapped" : "not writable: nothing done");
  if (!ok) return 0;
  // 15 min: the slot stays ours (put back once if something rewrote it), the pool is logged.
  for (int t = 0; t < 450; ++t) {
    Sleep(2000);
    if (*slot != reinterpret_cast<void*>(HkSndPlay)) {
      ss_log("[cpu_engine] the play slot was rewritten (holds %p): wrapped again", *slot);
      if (*slot == reinterpret_cast<void*>(kSndPlay)) ss_write_word((void*)slot, reinterpret_cast<uintptr_t>(HkSndPlay));
    }
    DumpSoundPool();
    DumpCarAudio();
  }
  return 0;
}

void InstallCpuEngines(HMODULE ev) {
  g_cpu_engine_fix = WantCpuEngines() ? 1 : 0;
  CreateThread(nullptr, 0, CpuEngineWorker, new CpuEngineArgs{ev}, 0, nullptr);
}

template <int K>
HOOK long HkGetEvent(void* self, uintptr_t a, unsigned mode, void** out) {
  long r = g_orig_get_event[K](self, a, mode, out);
  EngineEvidence(K, a, mode, r, out ? *out : nullptr);
  if (r == 0 && out && *out) CorrectEvent(*out, mode);
  return r;
}

// Sound instance volumes (kInstancePatches) are no run-time property, and the game has FMOD load its projects by
// file name through the game's own file callbacks (System::setFileSystem, the archive reader). So the read callback
// is wrapped: for a project file the table names (file name and size), the volume floats at the given offsets are
// replaced as they pass by, each only if it still holds the PC value. CONSOLE at the moment the file is opened only.
typedef long(__stdcall* PfnFsOpen)(const char* name, int unicode, unsigned* size, void** handle, void** user);
typedef long(__stdcall* PfnFsClose)(void* handle, void* user);
typedef long(__stdcall* PfnFsRead)(void* handle, void* buf, unsigned n, unsigned* got, void* user);
typedef long(__stdcall* PfnFsSeek)(void* handle, unsigned pos, void* user);
typedef long(__stdcall* PfnSetFs)(void* sys, PfnFsOpen, PfnFsClose, PfnFsRead, PfnFsSeek, int align);
PfnSetFs g_orig_set_fs = nullptr;
PfnFsOpen g_fs_open = nullptr;
PfnFsClose g_fs_close = nullptr;
PfnFsRead g_fs_read = nullptr;
PfnFsSeek g_fs_seek = nullptr;
CRITICAL_SECTION g_fs_cs;
struct Tracked { void* handle; uint32_t hash; unsigned pos; };
Tracked g_tracked[8];
volatile LONG g_inst_set = 0;

// audio_fsb_fix (default true; SS_AUDIO_FSB_FIX): where each sound of a sound bank starts. Every PC .fsb (FSB4,
// MPEG) stores its sounds one after the other, each one's data starting on a 32-byte boundary (zero bytes after
// a sound up to the next boundary; the header's data size counts them). fmodex.dll 4.28 finds a sound's data by
// adding up the compressed lengths of the sounds before it without those zero bytes, so it starts reading up to
// 2 KB too early: the last MPEG frames of the previous sound play first (music.fsb, from sound 33 on: 1 to 4
// frames, 26..105 ms) and as many frames are missing at the end. On a result screen, whose music loops over the
// whole file, that previous track's tail is heard at every loop (SS04_ResultScreen: 3 frames of SS03's ending;
// measured in a recording on the Deck: SS03's last 78 ms, then SS04 from its start, and SS04 cut 80 ms before its
// end). The fix is in the header as FMOD reads it through the game's file callbacks: each sound's compressed
// length is rounded up to 32, so FMOD's sum lands on the real start; the zero bytes after a sound are never
// decoded (FMOD stops at the sound's length in samples). Only for a file whose own header shows that layout: FSB4,
// every sound MPEG, data starting on a 32-byte boundary, the rounded-up lengths adding up to the header's data size
// and the plain ones not, and header plus data the size of the file. Nothing else is read differently.
constexpr int kFsbSlots = 32;
struct FsbFix { void* handle; unsigned pos, hdr_end; std::string* patch; };  // patch: (offset u32, value u32) pairs
FsbFix g_fsb[kFsbSlots];
bool g_fsb_on = false;
uint32_t g_fsb_logged[32];
volatile LONG g_fsb_nlogged = 0;

uint32_t Rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

// The file's own name (after the last slash), lower case, as 7-bit text; empty when it is not a .fsb.
std::string FsbBaseName(const void* name, int unicode) {
  std::string s;
  if (!name) return s;
  for (int i = 0; i < 1024; ++i) {
    const unsigned c = unicode ? static_cast<const wchar_t*>(name)[i] : static_cast<const unsigned char*>(name)[i];
    if (!c) break;
    if (c == '/' || c == '\\') { s.clear(); continue; }
    s += static_cast<char>(c < 128 ? tolower(static_cast<int>(c)) : '?');
  }
  return s.size() > 4 && !s.compare(s.size() - 4, 4, ".fsb") ? s : std::string();
}

void FsbLog(const std::string& base, const char* fmt, ...) {
  const uint32_t h = Fnv1a(base.c_str());
  const LONG n = g_fsb_nlogged < 32 ? g_fsb_nlogged : 32;
  for (LONG i = 0; i < n; ++i)
    if (g_fsb_logged[i] == h) return;  // once per file: the streams open their bank again and again
  const LONG k = InterlockedIncrement(&g_fsb_nlogged) - 1;
  if (k >= 32) return;
  g_fsb_logged[k] = h;
  char line[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  ss_log("[fsb] %s: %s", base.c_str(), line);
}

// Reads the header of a freshly opened .fsb through the game's callbacks (then back to position 0) and, when its
// layout is the one described above, returns the length fields to change; empty otherwise.
std::string FsbExamine(void* handle, void* user, unsigned size, const std::string& base) {
  std::string patch;
  uint8_t h[48];
  unsigned got = 0;
  if (g_fs_seek(handle, 0, user) != 0 || g_fs_read(handle, h, sizeof h, &got, user) != 0 || got != sizeof h ||
      memcmp(h, "FSB4", 4) != 0) {
    g_fs_seek(handle, 0, user);
    FsbLog(base, "not an FSB4 bank as expected: read as shipped");
    return patch;
  }
  const uint32_t n = Rd32(h + 4), shdr = Rd32(h + 8), ds = Rd32(h + 12);
  if (n == 0 || n > 4096 || shdr < 80 * n || shdr > (1u << 20) || 48ull + shdr + ds != size || (48 + shdr) % 32) {
    g_fs_seek(handle, 0, user);
    FsbLog(base, "%u sounds, header %u + data %u bytes in a file of %u: read as shipped", n, shdr, ds, size);
    return patch;
  }
  std::string hdr(shdr, '\0');
  got = 0;
  const long rr = g_fs_read(handle, &hdr[0], shdr, &got, user);
  g_fs_seek(handle, 0, user);
  if (rr != 0 || got != shdr) { FsbLog(base, "sample headers not readable: read as shipped"); return patch; }
  const uint8_t* p = reinterpret_cast<const uint8_t*>(hdr.data());
  uint64_t plain = 0, rounded = 0;
  uint32_t off = 0, mpeg = 0, moved = 0, last_shift = 0;
  for (uint32_t k = 0; k < n; ++k) {
    const uint32_t sz = off + 2 <= shdr ? (p[off] | (p[off + 1] << 8)) : 0;
    if (sz < 80 || off + sz > shdr) { FsbLog(base, "sample header %u out of place: read as shipped", k); return {}; }
    const uint32_t clen = Rd32(p + off + 36), mode = Rd32(p + off + 48);
    if (mode & 0x40000) ++mpeg;  // FSOUND_MPEG
    if (clen % 32) {
      const uint32_t at = 48 + off + 36, v = (clen + 31) & ~31u;
      patch.append(reinterpret_cast<const char*>(&at), 4).append(reinterpret_cast<const char*>(&v), 4);
    }
    if (rounded != plain) ++moved;
    last_shift = static_cast<uint32_t>(rounded - plain);
    plain += clen, rounded += (clen + 31) & ~31u;
    off += sz;
  }
  if (off != shdr || mpeg != n || rounded != ds || plain == ds) {
    FsbLog(base, "%u sounds (%u MPEG), data %u bytes, lengths add up to %llu (%llu rounded to 32): read as shipped",
           n, mpeg, ds, (unsigned long long)plain, (unsigned long long)rounded);
    return {};
  }
  FsbLog(base, "%u sounds, data on 32-byte boundaries that FMOD reads packed: %u lengths rounded up as the header is "
         "read, %u sounds start where their data really is (the last one %u bytes later than FMOD would read it)",
         n, static_cast<unsigned>(patch.size() / 8), moved, last_shift);
  return patch;
}

HOOK long HkFsOpen(const char* name, int unicode, unsigned* size, void** handle, void** user) {
  long r = g_fs_open(name, unicode, size, handle, user);
  if (r == 0 && g_fsb_on && size && handle) {
    const std::string base = FsbBaseName(name, unicode);
    if (!base.empty()) {
      std::string* patch = new std::string(FsbExamine(*handle, user ? *user : nullptr, *size, base));
      bool kept = false;
      if (!patch->empty()) {
        const unsigned end = Rd32(reinterpret_cast<const uint8_t*>(patch->data()) + patch->size() - 8) + 4;
        EnterCriticalSection(&g_fs_cs);
        for (FsbFix& f : g_fsb)
          if (!f.handle) {
            f = {*handle, 0, end, patch};
            kept = true;
            break;
          }
        LeaveCriticalSection(&g_fs_cs);
        if (!kept) FsbLog(base + "#full", "more than %d banks open at once: this one read as shipped", kFsbSlots);
      }
      if (!kept) delete patch;
    }
  }
  {  // engine evidence: every event project FMOD opens (capped), so a missing car project shows
    static volatile LONG logged = 0;
    const size_t ln = (!unicode && name) ? strlen(name) : 0;
    if (ln > 4 && !_stricmp(name + ln - 4, ".fev") && InterlockedIncrement(&logged) <= 80)
      ss_log("[events] FMOD opens '%.120s': %s (%u bytes)", name, r == 0 ? "ok" : "failed", (r == 0 && size) ? *size : 0u);
  }
  if (r != 0 || !g_cm_on || unicode || !name || !size || !handle) return r;
  const char* base = name;
  for (const char* c = name; *c; ++c)
    if (*c == '/' || *c == '\\') base = c + 1;
  char low[64];
  size_t n = 0;
  for (; base[n] && n < sizeof low - 1; ++n) low[n] = static_cast<char>(tolower(static_cast<unsigned char>(base[n])));
  low[n] = 0;
  if (n < 5 || strcmp(low + n - 4, ".fev") != 0) return r;
  low[n - 4] = 0;
  const uint32_t h = Fnv1a(low);
  for (int i = 0; i < sscm::kInstancePatchCount; ++i) {
    if (sscm::kInstancePatches[i].file_hash != h) continue;
    if (sscm::kInstancePatches[i].file_size != *size) {
      ss_log("[console_mix] %s: %u bytes, not the PC release's %u: sound instance volumes left", base, *size,
             sscm::kInstancePatches[i].file_size);
      return r;
    }
    EnterCriticalSection(&g_fs_cs);
    for (Tracked& t : g_tracked)
      if (!t.handle) { t = {*handle, h, 0}; break; }
    LeaveCriticalSection(&g_fs_cs);
    return r;
  }
  return r;
}

HOOK long HkFsClose(void* handle, void* user) {
  std::string* drop = nullptr;
  EnterCriticalSection(&g_fs_cs);
  for (Tracked& t : g_tracked)
    if (t.handle == handle) t.handle = nullptr;
  for (FsbFix& f : g_fsb)
    if (f.handle && f.handle == handle) drop = f.patch, f = {};
  LeaveCriticalSection(&g_fs_cs);
  delete drop;
  return g_fs_close(handle, user);
}

HOOK long HkFsSeek(void* handle, unsigned pos, void* user) {
  long r = g_fs_seek(handle, pos, user);
  EnterCriticalSection(&g_fs_cs);
  for (Tracked& t : g_tracked)
    if (t.handle && t.handle == handle) t.pos = pos;
  for (FsbFix& f : g_fsb)
    if (f.handle && f.handle == handle) f.pos = pos;
  LeaveCriticalSection(&g_fs_cs);
  return r;
}

HOOK long HkFsRead(void* handle, void* buf, unsigned n, unsigned* got, void* user) {
  long r = g_fs_read(handle, buf, n, got, user);
  EnterCriticalSection(&g_fs_cs);
  for (FsbFix& f : g_fsb) {
    if (!f.handle || f.handle != handle) continue;
    const unsigned done = got ? *got : 0, from = f.pos;
    f.pos += done;
    if (!buf || from >= f.hdr_end) break;  // past the sample headers: the sound data, as it is
    const uint8_t* pp = reinterpret_cast<const uint8_t*>(f.patch->data());
    for (size_t i = 0; i + 8 <= f.patch->size(); i += 8) {
      const unsigned at = Rd32(pp + i);
      for (unsigned b = 0; b < 4; ++b)  // byte by byte: a field may straddle two reads
        if (at + b >= from && at + b < from + done) static_cast<uint8_t*>(buf)[at + b - from] = pp[i + 4 + b];
    }
    break;
  }
  for (Tracked& t : g_tracked) {
    if (!t.handle || t.handle != handle) continue;
    const unsigned done = got ? *got : 0, from = t.pos;
    t.pos += done;
    for (int i = 0; i < sscm::kInstancePatchCount && buf; ++i) {
      const sscm::InstancePatch& ip = sscm::kInstancePatches[i];
      if (ip.file_hash != t.hash || ip.offset < from || ip.offset + 4 > from + done) continue;
      uint8_t* at = static_cast<uint8_t*>(buf) + (ip.offset - from);
      float v;
      memcpy(&v, at, 4);
      if (v != ip.pc) continue;
      memcpy(at, &ip.console, 4);
      if (InterlockedIncrement(&g_inst_set) <= 20)
        ss_log("[console_mix] sound instance volume at %u of the project file: %.3f -> %.3f", ip.offset, v, ip.console);
    }
  }
  LeaveCriticalSection(&g_fs_cs);
  return r;
}

HOOK long HkSetFs(void* sys, PfnFsOpen op, PfnFsClose cl, PfnFsRead rd, PfnFsSeek sk, int align) {
  if (!op || !cl || !rd || !sk || g_fs_open) {
    ss_log("[console_mix] System::setFileSystem(%p, %p, %p, %p): callbacks passed on as they are", (void*)op, (void*)cl,
           (void*)rd, (void*)sk);
    return g_orig_set_fs(sys, op, cl, rd, sk, align);
  }
  g_fs_open = op, g_fs_close = cl, g_fs_read = rd, g_fs_seek = sk;
  ss_log("[console_mix] System::setFileSystem: the game's file callbacks wrapped (sound instance volumes%s)",
         g_fsb_on ? ", sound bank headers" : "");
  return g_orig_set_fs(sys, HkFsOpen, HkFsClose, HkFsRead, HkFsSeek, align);
}

// The wrapper of System::setFileSystem, once: for the sound instance volumes and for audio_fsb_fix.
void InstallFileCallbacks() {
  static bool done = false;
  if (done) return;
  done = true;
  InitializeCriticalSection(&g_fs_cs);
  HMODULE fx = GetModuleHandleA("fmodex.dll");
  void* fn_fs = fx ? (void*)GetProcAddress(fx, "?setFileSystem@System@FMOD@@QAG?AW4FMOD_RESULT@@P6G?AW43@PBDHPAIPAPAX2@Z"
                                                 "P6G?AW43@PAX4@ZP6G?AW43@44I14@ZP6G?AW43@4I4@ZH@Z")
                   : nullptr;
  if (!fn_fs || !Detour(fn_fs, (void*)HkSetFs, (void**)&g_orig_set_fs, "System::setFileSystem"))
    ss_log("[console_mix] System::setFileSystem not wrapped: sound instance volumes and sound bank headers stay as "
           "shipped");
}

#define SYM_GET(cls, name, args) "?" name "@" cls "@FMOD@@UAG?AW4FMOD_RESULT@@" args
// The Event* the getEvent family hands out is a handle, not an EventI object: only the public Event:: members
// (non-virtual, they validate the handle) take it.

void InstallConsoleMix(HMODULE ev) {
  static const char* const kSyms[kGetEventSlots] = {
      SYM_GET("EventGroupI", "getEvent", "PBDIPAPAVEvent@2@@Z"),
      SYM_GET("EventGroupI", "getEventByIndex", "HIPAPAVEvent@2@@Z"),
      SYM_GET("EventSystemI", "getEvent", "PBDIPAPAVEvent@2@@Z"),
      SYM_GET("EventSystemI", "getEventBySystemID", "IIPAPAVEvent@2@@Z"),
      SYM_GET("EventSystemI", "getEventByGUID", "PBUFMOD_GUID@@IPAPAVEvent@2@@Z"),
      SYM_GET("EventSystemI", "getEventByGUIDString", "PBDIPAPAVEvent@2@@Z"),
      SYM_GET("EventProjectI", "getEvent", "PBDIPAPAVEvent@2@@Z"),
      SYM_GET("EventProjectI", "getEventByProjectID", "IIPAPAVEvent@2@@Z"),
      SYM_GET("EventCategoryI", "getEventByIndex", "HIPAPAVEvent@2@@Z"),
  };
  static void* const kHooks[kGetEventSlots] = {
      (void*)HkGetEvent<0>, (void*)HkGetEvent<1>, (void*)HkGetEvent<2>, (void*)HkGetEvent<3>, (void*)HkGetEvent<4>,
      (void*)HkGetEvent<5>, (void*)HkGetEvent<6>, (void*)HkGetEvent<7>, (void*)HkGetEvent<8>};
  g_ev_info = Export<PfnEvInfo>(ev, SYM_EV("getInfo", "PAHPAPADPAUFMOD_EVENT_INFO@@@Z"));
  g_ev_group = Export<PfnEvGroup>(ev, SYM_EV("getParentGroup", "PAPAVEventGroup@2@@Z"));
  g_group_project = Export<PfnGroupProject>(ev, SYM_GET("EventGroupI", "getParentProject", "PAPAVEventProject@2@@Z"));
  g_project_info = Export<PfnProjectInfo>(ev, SYM_GET("EventProjectI", "getInfo", "PAHPAPAD@Z"));
  g_ev_get_prop = Export<PfnEvProp>(ev, SYM_EV("getPropertyByIndex", "HPAX_N@Z"));
  g_ev_set_prop = Export<PfnEvProp>(ev, SYM_EV("setPropertyByIndex", "HPAX_N@Z"));
  g_ev_category = Export<PfnEvCategory>(ev, SYM_EV("getCategory", "PAPAVEventCategory@2@@Z"));
  if (!g_ev_info || !g_ev_group || !g_group_project || !g_project_info || !g_ev_get_prop || !g_ev_set_prop) {
    ss_log("[console_mix] event exports missing in fmod_event.dll: nothing done");
    return;
  }
  // Every slot first, then the swaps: all or nothing.
  void** slots[kGetEventSlots];
  for (int k = 0; k < kGetEventSlots; ++k) {
    void* fn = (void*)GetProcAddress(ev, kSyms[k]);
    slots[k] = fn ? FindSlot(ev, fn, nullptr, nullptr) : nullptr;
    if (!slots[k]) {
      ss_log("[console_mix] vtable slot of %s not found: nothing done", kSyms[k]);
      return;
    }
  }
  int ok = 0;
  for (int k = 0; k < kGetEventSlots; ++k) ok += SwapSlot(slots[k], kHooks[k], (void**)&g_orig_get_event[k]) ? 1 : 0;
  if (ok == kGetEventSlots) g_cm_installed = 1;
  InstallFileCallbacks();
  ss_log("[console_mix] %d of %d getEvent slots of fmod_event.dll wrapped; %d corrections in the table; preset %s", ok,
         kGetEventSlots, sscm::kCorrectionCount, g_cm_on ? "CONSOLE" : "NATIVE");
}

}  // namespace

// The overlay's MIX PRESET row: switches the preset for the events fetched from now on.
bool ss_audio_apply(const char* key, const char* value) {
  if (!strcmp(key, "reverb_level")) {
    if (!g_orig_sys_reverb) return false;  // the wrapper went in at start-up, or never
    int pct = atoi(value ? value : "100");
    pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    InterlockedExchange(&g_reverb_level, pct);
    InterlockedExchange(&g_reverb_level_dirty, 1);
    ss_log("[reverb] reverb_level %d%%", pct);
    return true;
  }
  if (strcmp(key, "console_mix") != 0 || !g_cm_installed) return false;
  const LONG on = ParseBool(value ? value : "", false) ? 1 : 0;
  InterlockedExchange(&g_cm_on, on);
  ss_log("[console_mix] preset %s from the next event fetched", on ? "CONSOLE" : "NATIVE");
  return true;
}

namespace {

// Resolves what the mixer needs (read only); the hooks themselves go in later, and only when a gain is not 100.
bool PrepareMixer(HMODULE ev) {
  HMODULE fx = GetModuleHandleA("fmodex.dll");
  void* fn_up = (void*)GetProcAddress(ev, SYM_ES("update", "XZ"));
  g_orig_get_category = Export<PfnGetCategory>(ev, SYM_ES("getCategory", "PBDPAPAVEventCategory@2@@Z"));
  g_cat_info = Export<PfnCatInfo>(ev, SYM_EC("getInfo", "PAHPAPAD@Z"));
  g_cat_num = Export<PfnCatNum>(ev, SYM_EC("getNumCategories", "PAH@Z"));
  g_cat_by_index = Export<PfnCatByIndex>(ev, SYM_EC("getCategoryByIndex", "HPAPAVEventCategory@2@@Z"));
  g_cat_parent = Export<PfnCatParent>(ev, SYM_EC("getParentCategory", "PAPAVEventCategory@2@@Z"));
  g_cat_group = Export<PfnCatGroup>(ev, SYM_EC("getChannelGroup", "PAPAVChannelGroup@2@@Z"));
  if (fx) {
    g_fx_set = (void*)GetProcAddress(fx, "?setVolume@ChannelGroup@FMOD@@QAG?AW4FMOD_RESULT@@M@Z");
    g_fx_get = (void*)GetProcAddress(fx, "?getVolume@ChannelGroup@FMOD@@QAG?AW4FMOD_RESULT@@PAM@Z");
  }
  if (!fn_up || !g_orig_get_category || !g_cat_info || !g_cat_num || !g_cat_by_index || !g_cat_parent || !g_cat_group ||
      !g_fx_set || !g_fx_get) {
    ss_log("[mixer] exports missing in fmod_event.dll / fmodex.dll (fmodex %p): the mixer stays off", (void*)fx);
    return false;
  }
  g_update_slot = FindSlot(ev, fn_up, nullptr, nullptr);
  if (!g_update_slot) {
    ss_log("[mixer] EventSystem::update vtable slot not found: the mixer stays off");
    return false;
  }
  g_mixer_ready = 1;
  return true;
}

DWORD WINAPI AudioWorker(LPVOID) {
  InitializeCriticalSection(&g_cs);
  const bool reverb = WantReverb();
  g_trim_db = WantSfxTrimDb();
  g_trim_gain = g_trim_db < 0.0 ? pow(10.0, g_trim_db / 20.0) : 1.0;
  g_trim_list = getenv("SS_SFX_TRIM_CATEGORIES") ? getenv("SS_SFX_TRIM_CATEGORIES") : ss_conf("sfx_trim_categories");
  std::string env_vol;  // SS_VOLUME_MASTER, SS_VOLUME_MUSIC, ...
  bool any_gain = g_trim_gain != 1.0;
  std::string vols;
  for (int g = 0; g < kGroups; ++g) {
    env_vol = "SS_" + std::string(g_groups[g].key);
    for (auto& c : env_vol) c = (char)toupper((unsigned char)c);
    g_groups[g].pct = ParsePct(env_vol.c_str(), g_groups[g].key);
    if (g_groups[g].pct != 100) any_gain = true;
    char b[48];
    snprintf(b, sizeof b, "%s%s %ld", g ? ", " : "", g_groups[g].name, (long)g_groups[g].pct);
    vols += b;
  }
  ParseMap(getenv("SS_MIXER_MAP") ? getenv("SS_MIXER_MAP") : ss_conf("mixer_map"));
  bool force = ParseBool(getenv("SS_MIXER") ? getenv("SS_MIXER") : ss_conf("mixer"), false);
  g_diag = ParseBool(getenv("SS_AUDIO_DIAG") ? getenv("SS_AUDIO_DIAG") : ss_conf("audio_diag"), false) ? 1 : 0;
  if (g_diag) force = true;  // the log needs the channel group wrappers and the update hook
  g_music_log = ParseBool(getenv("SS_AUDIO_MUSIC_LOG") ? getenv("SS_AUDIO_MUSIC_LOG") : ss_conf("audio_music_log"), false) ? 1 : 0;
  if (g_music_log) force = true, g_music_log_cg = 1;  // the groups' volumes come through the channel group wrapper
  g_sidechain = WantSidechain() ? 1 : 0;
  if (g_sidechain) force = true;  // the meters go in on the thread that runs EventSystem::update
  ss_log("[audio] reverb %s, mixer %s%%, sfx_trim %.1f dB%s", reverb ? "on" : "off", vols.c_str(), g_trim_db,
         any_gain || force ? "" : " (all at 100)");
  if (!reverb && !ss_conf_present() && !any_gain && !force) return 0;  // nothing is touched at all
  HMODULE ev = nullptr;
  for (int i = 0; i < 18000 && !ev; ++i) {  // the game links fmod_event.dll itself: normally mapped before we run
    ev = GetModuleHandleA("fmod_event.dll");
    if (!ev) Sleep(10);
  }
  if (!ev) { ss_log("[audio] fmod_event.dll never appeared in the process: nothing done"); return 1; }
  g_fsb_on = WantFsbFix();
  if (g_fsb_on) InstallFileCallbacks();
  else ss_log("[fsb] audio_fsb_fix off: FMOD reads the sound banks as shipped (sounds start up to 2 KB early)");
  g_software_mix = WantSoftwareMix() ? 1 : 0;
  if (WantVoiceDsp()) InstallVoiceDsp();
  else ss_log("[voice_dsp] off: the game's voice units stay out of the DSP network, as shipped");
  g_speaker_fix = WantSpeakerFix() ? 1 : 0;
  if (g_speaker_fix && g_voice_dsp_on)
    ss_log("[speaker_fix] not applied: with audio_voice_dsp the 1.0 levels are the voice units' input, as on the console");
  if (g_speaker_fix || g_diag) InstallSetPropWrapper(ev);
  InstallSoftwareMix();  // logs FMOD's output setup either way; sets no hardware voices when audio_software_mix
  if (reverb) InstallReverb(ev);
  // The wrappers go in when the preset is CONSOLE, or when the overlay may switch it to CONSOLE later.
  g_cm_on = WantConsoleMix() ? 1 : 0;
  if (g_cm_on || ss_menu_wanted() || g_diag) InstallConsoleMix(ev);
  else ss_log("[console_mix] NATIVE: the PC release's event values as shipped, getEvent left alone");
  if (g_diag) InstallStartDiag(ev);
  InstallCpuEngines(ev);
  {
    std::string v = getenv("SS_REVERB_LEVEL") ? getenv("SS_REVERB_LEVEL") : ss_conf("reverb_level");
    v = NoSpaces(v);
    if (!v.empty()) { int pct = atoi(v.c_str()); g_reverb_level = pct < 0 ? 0 : pct > 100 ? 100 : pct; }
  }
  // At start-up or never, like the other jumps: when the level is trimmed, the overlay may trim it later, or for the log.
  if (g_reverb_level < 100 || ss_menu_wanted() || g_diag) InstallReverbLevel();
  if (g_diag) InstallReverbDiag(ev);
  if (g_diag || g_sidechain) InstallDspHooks();
  for (int g = 0; g < kGroups; ++g)
    if (g_groups[g].pct != 100) any_gain = true;  // also a value the overlay set while we waited
  // The jumps go in here, at start-up, or never: also when every group is at 100 if the in-game overlay is wanted,
  // so it can move a volume later without patching fmodex.dll while the game's audio thread runs.
  const bool overlay = ss_menu_wanted();
  if (!any_gain && !force && !overlay) {
    ss_log("[mixer] every group at 100, mixer = false, no overlay: fmodex.dll left alone");
    return 0;
  }
  if (PrepareMixer(ev)) {
    InstallDetours();
    if (g_detour_state == 2) EnsureUpdateHook();
  }
  return 0;
}

}  // namespace

// ------------------------------------------------------------------ mixer.h

extern "C" int ss_mixer_count() { return kGroups; }

extern "C" const char* ss_mixer_key(int g) { return g >= 0 && g < kGroups ? g_groups[g].key : nullptr; }

extern "C" const char* ss_mixer_label(int g) { return g >= 0 && g < kGroups ? g_groups[g].label : nullptr; }

extern "C" int ss_mixer_get(int g) { return g >= 0 && g < kGroups ? (int)g_groups[g].pct : 100; }

extern "C" void ss_mixer_set(int g, int pct) {
  if (g < 0 || g >= kGroups) return;
  pct = pct < 0 ? 0 : pct > 200 ? 200 : pct;
  if (InterlockedExchange(&g_groups[g].pct, pct) == pct) return;
  InterlockedExchange(&g_dirty, 1);  // the hooks (put in at start-up, or never) apply it on the next update
}

void ss_audio_start() { CreateThread(nullptr, 0, AudioWorker, nullptr, 0, nullptr); }
