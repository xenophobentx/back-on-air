// reverb_presets.h - the game's own authored reverb parameters (restored at runtime).
//
// Split/Second authors its reverb presets in the FMOD Designer event files. The PC release
// ships the same event files with seven of the eight presets overwritten by the generic
// default preset (Room -1000, RoomHF -100, DecayTime 1.49, ... see PC column in the notes),
// (LARGE_TANK carries the stock bathroom preset instead), so nearly every environment
// sounds like the same small room. The values below are the game's
// own authored reverb parameters (a handful of numbers per preset), restored at runtime
// through the engine's own reverb call. No game media, no sample data, no file contents
// other than these parameters are included.
//
// Layout: ReverbProps mirrors FMOD Ex 4.28 FMOD_REVERB_PROPERTIES field for field
// (31 dwords, 124 bytes), so a preset can be copied straight over the structure the
// game passes to System::setReverbProperties (or returned by the event system's
// getReverbPreset / getReverbPresetByIndex).
//
// How the game picks a preset (verified in the unpacked 1.0 image):
//  * At start-up the audio manager enumerates the event system's presets
//    (EventSystem getNumReverbPresets + getReverbPresetByIndex) and caches one
//    preset object per name.
//  * An AudioTrigger's `reverb_type` string and the audio-mixer XML's
//    <ReverbPreset Name="..."/> (TakeDownCamera, LoadingAndFrontendScreenReverb) are looked
//    up in that cache by exact, case-sensitive name. Preset values are then blended and
//    handed to System::setReverbProperties.
//  * Names that no event file defines (UnderBridge, SMALL_TANK, GENERIC, Tunnel) get a
//    freshly created default preset; there is no authored data for them (not in this table).
//  => Substitute by name when the preset is read from the event system (or by matching
//     the generic block + the name in the manager's cache), then the game's own
//     blending and System::setReverbProperties calls carry the authored values.
//
// Source: the console release's event files (identical copies in every project file that
// carries the preset). All 31 fields of every preset were readable from the data; none
// are defaulted. DefaultAmbient is used by the game's ambient/default environment.
#pragma once
#include <stdint.h>
#include <string.h>

namespace ssrev {

struct ReverbProps {                 // == FMOD_REVERB_PROPERTIES (FMOD Ex 4.28)
    int32_t  Instance;               // 0..3
    int32_t  Environment;            // -1..25 (EAX environment index, informational for the software mixer)
    float    EnvSize;                // 1..100
    float    EnvDiffusion;           // 0..1
    int32_t  Room;                   // -10000..0
    int32_t  RoomHF;                 // -10000..0
    int32_t  RoomLF;                 // -10000..0
    float    DecayTime;              // 0.1..20
    float    DecayHFRatio;           // 0.1..2
    float    DecayLFRatio;           // 0.1..2
    int32_t  Reflections;            // -10000..1000
    float    ReflectionsDelay;       // 0..0.3
    float    ReflectionsPan[3];
    int32_t  Reverb;                 // -10000..2000
    float    ReverbDelay;            // 0..0.1
    float    ReverbPan[3];
    float    EchoTime;               // 0.075..0.25
    float    EchoDepth;              // 0..1
    float    ModulationTime;         // 0.04..4
    float    ModulationDepth;        // 0..1
    float    AirAbsorptionHF;        // -100..0
    float    HFReference;            // 20..20000
    float    LFReference;            // 20..1000
    float    RoomRolloffFactor;      // 0..10
    float    Diffusion;              // 0..100
    float    Density;                // 0..100
    uint32_t Flags;                  // FMOD_REVERB_FLAGS
};
static_assert(sizeof(ReverbProps) == 31 * 4, "ReverbProps must match FMOD_REVERB_PROPERTIES");

struct ReverbPreset {
    const char* name;                // exact, case-sensitive preset name
    ReverbProps props;
};

static const ReverbPreset kReverbPresets[] = {
    { "TakeDownCamera", {
        /* Instance          */ 0,
        /* Environment       */ 13,
        /* EnvSize           */ 7.5f,
        /* EnvDiffusion      */ 1.0f,
        /* Room              */ 0,
        /* RoomHF            */ -237,
        /* RoomLF            */ 0,
        /* DecayTime         */ 1.0f,
        /* DecayHFRatio      */ 0.79f,
        /* DecayLFRatio      */ 1.0f,
        /* Reflections       */ -1214,
        /* ReflectionsDelay  */ 0.013f,
        /* ReflectionsPan    */ { 0.0f, 0.0f, 0.0f },
        /* Reverb            */ 395,
        /* ReverbDelay       */ 0.02f,
        /* ReverbPan         */ { 0.0f, 0.0f, 0.0f },
        /* EchoTime          */ 0.25f,
        /* EchoDepth         */ 0.0f,
        /* ModulationTime    */ 0.25f,
        /* ModulationDepth   */ 0.0f,
        /* AirAbsorptionHF   */ -5.0f,
        /* HFReference       */ 5000.0f,
        /* LFReference       */ 250.0f,
        /* RoomRolloffFactor */ 0.0f,
        /* Diffusion         */ 100.0f,
        /* Density           */ 100.0f,
        /* Flags             */ 0x3fu,
    } },
    { "LoadingAndFrontendScreenReverb", {
        /* Instance          */ 0,
        /* Environment       */ 3,
        /* EnvSize           */ 7.5f,
        /* EnvDiffusion      */ 1.0f,
        /* Room              */ -1500,
        /* RoomHF            */ 0,
        /* RoomLF            */ 0,
        /* DecayTime         */ 8.0f,
        /* DecayHFRatio      */ 1.0f,
        /* DecayLFRatio      */ 1.0f,
        /* Reflections       */ -370,
        /* ReflectionsDelay  */ 0.007f,
        /* ReflectionsPan    */ { 0.0f, 0.0f, 0.0f },
        /* Reverb            */ 1030,
        /* ReverbDelay       */ 0.011f,
        /* ReverbPan         */ { 0.0f, 0.0f, 0.0f },
        /* EchoTime          */ 0.25f,
        /* EchoDepth         */ 0.0f,
        /* ModulationTime    */ 0.25f,
        /* ModulationDepth   */ 0.0f,
        /* AirAbsorptionHF   */ -5.0f,
        /* HFReference       */ 5000.0f,
        /* LFReference       */ 50.0f,
        /* RoomRolloffFactor */ 0.0f,
        /* Diffusion         */ 100.0f,
        /* Density           */ 60.0f,
        /* Flags             */ 0x3fu,
    } },
    { "DefaultAmbient", {
        /* Instance          */ 0,
        /* Environment       */ 20,
        /* EnvSize           */ 7.5f,
        /* EnvDiffusion      */ 1.0f,
        /* Room              */ -1000,
        /* RoomHF            */ 0,
        /* RoomLF            */ 0,
        /* DecayTime         */ 1.65f,
        /* DecayHFRatio      */ 1.5f,
        /* DecayLFRatio      */ 1.0f,
        /* Reflections       */ -1363,
        /* ReflectionsDelay  */ 0.008f,
        /* ReflectionsPan    */ { 0.0f, 0.0f, 0.0f },
        /* Reverb            */ -1153,
        /* ReverbDelay       */ 0.012f,
        /* ReverbPan         */ { 0.0f, 0.0f, 0.0f },
        /* EchoTime          */ 0.25f,
        /* EchoDepth         */ 0.0f,
        /* ModulationTime    */ 0.25f,
        /* ModulationDepth   */ 0.0f,
        /* AirAbsorptionHF   */ -5.0f,
        /* HFReference       */ 5000.0f,
        /* LFReference       */ 250.0f,
        /* RoomRolloffFactor */ 0.0f,
        /* Diffusion         */ 100.0f,
        /* Density           */ 100.0f,
        /* Flags             */ 0x3fu,
    } },
    { "HALL", {
        /* Instance          */ 0,
        /* Environment       */ 13,
        /* EnvSize           */ 7.5f,
        /* EnvDiffusion      */ 1.0f,
        /* Room              */ 0,
        /* RoomHF            */ -300,
        /* RoomLF            */ 0,
        /* DecayTime         */ 2.7f,
        /* DecayHFRatio      */ 0.79f,
        /* DecayLFRatio      */ 1.0f,
        /* Reflections       */ -500,
        /* ReflectionsDelay  */ 0.013f,
        /* ReflectionsPan    */ { 0.0f, 0.0f, 0.0f },
        /* Reverb            */ 395,
        /* ReverbDelay       */ 0.02f,
        /* ReverbPan         */ { 0.0f, 0.0f, 0.0f },
        /* EchoTime          */ 0.25f,
        /* EchoDepth         */ 0.0f,
        /* ModulationTime    */ 0.25f,
        /* ModulationDepth   */ 0.0f,
        /* AirAbsorptionHF   */ -5.0f,
        /* HFReference       */ 5000.0f,
        /* LFReference       */ 250.0f,
        /* RoomRolloffFactor */ 0.0f,
        /* Diffusion         */ 100.0f,
        /* Density           */ 100.0f,
        /* Flags             */ 0x3fu,
    } },
    { "LARGE_HALL", {
        /* Instance          */ 0,
        /* Environment       */ 13,
        /* EnvSize           */ 7.5f,
        /* EnvDiffusion      */ 1.0f,
        /* Room              */ 0,
        /* RoomHF            */ -300,
        /* RoomLF            */ 0,
        /* DecayTime         */ 2.7f,
        /* DecayHFRatio      */ 0.79f,
        /* DecayLFRatio      */ 1.0f,
        /* Reflections       */ -500,
        /* ReflectionsDelay  */ 0.013f,
        /* ReflectionsPan    */ { 0.0f, 0.0f, 0.0f },
        /* Reverb            */ 395,
        /* ReverbDelay       */ 0.02f,
        /* ReverbPan         */ { 0.0f, 0.0f, 0.0f },
        /* EchoTime          */ 0.25f,
        /* EchoDepth         */ 0.0f,
        /* ModulationTime    */ 0.25f,
        /* ModulationDepth   */ 0.0f,
        /* AirAbsorptionHF   */ -5.0f,
        /* HFReference       */ 5000.0f,
        /* LFReference       */ 250.0f,
        /* RoomRolloffFactor */ 0.0f,
        /* Diffusion         */ 100.0f,
        /* Density           */ 100.0f,
        /* Flags             */ 0x3fu,
    } },
    { "LARGE_TANK", {
        /* Instance          */ 0,
        /* Environment       */ 0,
        /* EnvSize           */ 7.5f,
        /* EnvDiffusion      */ 1.0f,
        /* Room              */ 0,
        /* RoomHF            */ -100,
        /* RoomLF            */ 0,
        /* DecayTime         */ 1.49f,
        /* DecayHFRatio      */ 0.83f,
        /* DecayLFRatio      */ 1.0f,
        /* Reflections       */ -1000,
        /* ReflectionsDelay  */ 0.007f,
        /* ReflectionsPan    */ { 0.0f, 0.0f, 0.0f },
        /* Reverb            */ 1000,
        /* ReverbDelay       */ 0.011f,
        /* ReverbPan         */ { 0.0f, 0.0f, 0.0f },
        /* EchoTime          */ 0.25f,
        /* EchoDepth         */ 0.0f,
        /* ModulationTime    */ 0.25f,
        /* ModulationDepth   */ 0.0f,
        /* AirAbsorptionHF   */ -5.0f,
        /* HFReference       */ 5000.0f,
        /* LFReference       */ 250.0f,
        /* RoomRolloffFactor */ 0.0f,
        /* Diffusion         */ 100.0f,
        /* Density           */ 100.0f,
        /* Flags             */ 0x3fu,
    } },
    { "OVERHEAD", {
        /* Instance          */ 0,
        /* Environment       */ 10,
        /* EnvSize           */ 7.5f,
        /* EnvDiffusion      */ 1.0f,
        /* Room              */ 0,
        /* RoomHF            */ -300,
        /* RoomLF            */ 0,
        /* DecayTime         */ 4.0f,
        /* DecayHFRatio      */ 0.23f,
        /* DecayLFRatio      */ 1.0f,
        /* Reflections       */ -602,
        /* ReflectionsDelay  */ 0.02f,
        /* ReflectionsPan    */ { 0.0f, 0.0f, 0.0f },
        /* Reverb            */ 200,
        /* ReverbDelay       */ 0.03f,
        /* ReverbPan         */ { 0.0f, 0.0f, 0.0f },
        /* EchoTime          */ 0.25f,
        /* EchoDepth         */ 0.0f,
        /* ModulationTime    */ 0.25f,
        /* ModulationDepth   */ 0.0f,
        /* AirAbsorptionHF   */ -5.0f,
        /* HFReference       */ 5000.0f,
        /* LFReference       */ 250.0f,
        /* RoomRolloffFactor */ 0.0f,
        /* Diffusion         */ 100.0f,
        /* Density           */ 100.0f,
        /* Flags             */ 0x3fu,
    } },
    { "HANGER", {
        /* Instance          */ 0,
        /* Environment       */ 7,
        /* EnvSize           */ 7.5f,
        /* EnvDiffusion      */ 1.0f,
        /* Room              */ 0,
        /* RoomHF            */ 0,
        /* RoomLF            */ 0,
        /* DecayTime         */ 3.92f,
        /* DecayHFRatio      */ 0.7f,
        /* DecayLFRatio      */ 1.0f,
        /* Reflections       */ -1230,
        /* ReflectionsDelay  */ 0.02f,
        /* ReflectionsPan    */ { 0.0f, 0.0f, 0.0f },
        /* Reverb            */ 0,
        /* ReverbDelay       */ 0.029f,
        /* ReverbPan         */ { 0.0f, 0.0f, 0.0f },
        /* EchoTime          */ 0.25f,
        /* EchoDepth         */ 0.0f,
        /* ModulationTime    */ 0.25f,
        /* ModulationDepth   */ 0.0f,
        /* AirAbsorptionHF   */ -5.0f,
        /* HFReference       */ 5000.0f,
        /* LFReference       */ 250.0f,
        /* RoomRolloffFactor */ 0.0f,
        /* Diffusion         */ 100.0f,
        /* Density           */ 100.0f,
        /* Flags             */ 0x3fu,
    } },
};

static const int kReverbPresetCount = (int)(sizeof(kReverbPresets) / sizeof(kReverbPresets[0]));

// Returns the authored preset for `name` (exact match) or nullptr.
static inline const ReverbProps* FindReverbPreset(const char* name) {
    if (!name) return nullptr;
    for (int i = 0; i < kReverbPresetCount; i++)
        if (strcmp(kReverbPresets[i].name, name) == 0) return &kReverbPresets[i].props;
    return nullptr;
}

} // namespace ssrev
