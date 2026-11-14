# split-second-pcpatch: technical notes

The player's guide is `README.md` (install, settings, known limits, release notes). This file is the long
technical description of every part: how it works, what it touches, the measurements and the test aids.

A `dinput8.dll` proxy for the PC release of Split/Second (Steam build 385319). It forwards DirectInput
to the system DLL and lets the game run its logic at 60 ticks a second.

**It never writes the game's code.** No byte of `.text` changes (SecuROM watches it). What it may touch, and only
for the features switched on in `splitsecond.conf`: values in the game's data and heap, single vtable words in the
game's read-only data (through one locked writer that refuses any address in code), and functions or vtables inside
third-party / system DLLs (d3d9, fmod_event, fmodex, xinput). With no `splitsecond.conf` the DLL only forwards
DirectInput. The base (60 fps):

* the eight game-state objects (global data) get their vtable pointer redirected to a private copy of the vtable
  that lives in this DLL; slot 2 of the copy is our frame step, which calls the game's own `update`/`render`
  through the original vtable (one tick per 1/60 s, a precise wait, the console's catch-up clamp of 4);
* every `cAIPlayer` instance gets a private vtable copy whose update slot is a gate (the steering controller
  steps with a fixed 1/30 s, so it runs every second tick);
* the two rewind rings (180 entries) are replaced by 360 entry rings in the heap, with the game's own allocator,
  before anything is recorded.

Before anything is done, the vtables and objects are compared with the known build; on any mismatch the DLL does nothing.

* `src/proxy.cpp` the dinput8 forwarder, starts the patcher.
* `src/audio.cpp` the audio part (below), `src/reverb_presets.h` the authored reverb parameters, `src/mixer.h` the
  mixer's live API (for the overlay).
* `src/patch.cpp` the takeover, the step, the AI gate, the rings.
* Install and uninstall: see "Install" below (`fps = 30` or no `splitsecond.conf`: the DLL does nothing).
* Config: `splitsecond.conf` next to the DLL (`fps = 30|60`; experimental: `120`, `native`, a number up to 240,
  see "Above 60"); the switch `SS_HFR` overrides it.
  Log: `splitsecond-patch.log` next to the DLL (a few lines; `SS_PATCH_LOG=1` adds a statistics line every 2 s
  and a thread census every 10 s). Each line passes `ss_redact_user_folders` (`src/online_text.h`) on its way in:
  the folder after `\Users\`, `/Users/`, `\home\` or `/home/` reads `<user>`, so a Steam, game or save path in a
  bug report carries no user name.
* Test aids (environment only, not for play): `SS_PATCH_MODE=off|pass|own30|60` (pass: states taken over, original
  step called through; own30: our step with the game's own 1/30 arithmetic), `SS_PATCH_OFF=ai,rings,timer`,
  `SS_MAXTICKS`, `SS_SPIN_US`, `SS_PATCH_KEYS` / `SS_PATCH_TEST` (key injection and live commands `pass`, `fps30`,
  `fps60`, `ai0`, `ai1` from `splitsecond-keys.txt`).
* Build: `make` (mingw-w64, i686 target). Player zip: `make dist` (`tools/dist/make-dist.sh`, into `build/dist/`);
  `make installtest` runs the install scripts against a fake Steam folder.

## Install

The zip holds `dinput8.dll` (the patch), `splitsecond.conf` (the settings), `dxvk\d3d9.dll` (DXVK 2.7.1, used on
Windows only), the install scripts and this README. Unpack the whole zip into any folder first. Quit the game before
you install, update or uninstall.

**Windows, with the script.** Double-click `install.bat`. It finds the game through Steam (all library folders); if it
cannot, it asks for the game folder (the folder with `SplitSecond.exe`). The window lists each step and waits for a key
at the end. If it says it cannot write into the game folder, right-click `install.bat` and choose "Run as administrator".

**Steam Deck and Linux, with the script.** On the Deck switch to Desktop Mode. Open the unpacked folder in Dolphin,
right-click an empty place > "Open Terminal Here" (or open Konsole and `cd` there) and type `sh install.sh`. It finds
the game in every Steam library (SD card too; also the Flatpak Steam) and writes the `dinput8` override into the
game's Proton prefix (`steamapps/compatdata/297860/pfx/user.reg`, key
`[Software\\Wine\\AppDefaults\\SplitSecond.exe\\DllOverrides]`, `"dinput8"="native,builtin"`), so no launch option
is needed. If the game was never started, the prefix does not exist yet: start the game once, quit, and run the script
again. If Steam resets the prefix (another Proton version, "delete compatibility data"), run the script again.

What both scripts do:

* Anything they would replace goes first into `patch-backup/YYYY-MM-DD_HHMMSS` inside the game folder: an older
  `dinput8.dll`, a `d3d9.dll` beside `SplitSecond.exe` (an older layout; on Windows it would override the RENDERER
  setting), an older `dxvk` folder (Windows), and on Linux a copy of `user.reg` before the edit.
* Update: run the script of the new zip. Your `splitsecond.conf` stays as it is; the new version's defaults are written
  beside it as `splitsecond.conf.new` (compare the two, take what you want).
* They stop if the game is running, and if the old Split/Second HFR mod is installed (`DXFreezerServer.dll`; it
  conflicts with this patch and the game freezes): move that DLL and `SplitSecondHFR.ini` out first.
* They never change the game's own files or the saves. Running them twice changes nothing.

**Windows, by hand.**

1. Find the game folder: Steam > Split/Second > Properties > Installed Files > Browse.
2. Copy `dinput8.dll`, `splitsecond.conf` and the `dxvk` folder into it (next to `SplitSecond.exe`).
3. If a `d3d9.dll` is next to `SplitSecond.exe`, move it away.

**Steam Deck and Linux, by hand.**

1. Find the game folder: Steam > Split/Second > Properties > Installed Files > Browse.
2. Copy `dinput8.dll` and `splitsecond.conf` into it (the `dxvk` folder is not needed; Proton has its own DXVK).
3. Steam > Split/Second > Properties > General > Launch options: `WINEDLLOVERRIDES="dinput8=n,b" %command%`
   (this tells Proton to load the patch's `dinput8.dll`; the script does the same in the prefix instead).

## Uninstall

* Windows: double-click `uninstall.bat` (or run `install.bat /uninstall`).
* Steam Deck / Linux: `sh uninstall.sh` (or `sh install.sh --uninstall`).

It removes `dinput8.dll` (only when it is the patch's), the `dxvk` folder (Windows, only the patch's), and moves
`splitsecond.conf`, `splitsecond.conf.new` and `splitsecond-patch.log` into `patch-backup/<date>-uninstall`. It puts
back the newest copy of anything the install had moved away that was not the patch's (another `dinput8.dll`, a
`d3d9.dll`, another `dxvk` folder), and on Linux removes the `dinput8` override from the prefix. Left alone: the saves,
`save-backups` (copies of the saves the patch made), `shader-cache` (can be deleted) and `patch-backup`. If you set
the launch option, remove it in Steam.

By hand: delete `dinput8.dll`, `splitsecond.conf`, `splitsecond-patch.log` and (Windows) the `dxvk` folder from the
game folder; remove the launch option.

## Controllers (`src/pad.cpp`)

How the game reads pads (from the PC image): `DirectInput8Create` (ANSI, 0x800) -> one `EnumDevices(GAMECTRL, attached only)` at start
(no hot-plug) -> `CreateDevice` -> `SetDataFormat(c_dfDIJoystick2)`, axis range +-32767, dead zone 0 -> `SetCooperativeLevel(hwnd, 5)` -> `Acquire`;
each frame `Poll` + `GetDeviceState(0x110)`. Bindings come from a built-in table chosen by the **product id** of the device (15 known products:
the Xbox 360 pad, a few Logitech / Saitek / Thrustmaster pads and wheels); every other product gets a table with all entries unbound, so a pad of
any other kind does nothing, in the menus as in the race. Rumble is `XInputSetState(i, ...)` with `i` = the pad's position in the game's own list
(the DirectInput enumeration order), not its XInput slot.

What the DLL does with `pad_support = true` (the default): `IDirectInput8A` is wrapped; every connected XInput pad is offered as a wired Xbox 360 pad
(product 045E:028E, the layout the game's table expects: X/Y left stick, Rx/Ry right stick, Z = left minus right trigger, 10 buttons A B X Y LB RB Back
Start LS RS, hat = D-pad) in XInput slot order. Real DirectInput pads that XInput already carries are hidden; DirectInput-only pads (Sony, Nintendo,
generic) are read and presented the same way; wheels and the pads the game has its own table for are passed through. `xinput1_3!XInputSetState` (the
system DLL's entry in memory, not the game) maps the game's list index to the XInput slot. Keys: `splitsecond.conf.example`. Log lines `[pad]` in
`splitsecond-patch.log` list what was found, what the game was offered, button changes and rumble routing.

* Test aid: `make test` builds `build/padtest.exe`, which plays the game's calls against `./dinput8.dll`; `SS_PAD_FAKE=1` makes XInput slot 0 a scripted pad;
  `SS_PAD=0` switches the layer off.

## Audio

The PC release ships the event files with the per-environment reverb presets overwritten by one generic small room
(hall, large hall, tank, overhead, hangar, take-down camera, loading screen, default ambient), so explosions and
collisions are dry. `reverb = true` (the default whenever `splitsecond.conf` exists; switch `SS_REVERB=0|1`) gives the
game the authored parameters back. Still nothing is written into the game: `fmod_event.dll` is a separate library
beside the exe, and the DLL swaps two slots of its `EventSystem` vtable (`getReverbPreset`, `getReverbPresetByIndex`)
for wrappers that call the original and then replace the returned parameters, by preset name. The game's own preset
cache, blending and `System::setReverbProperties` calls carry them from there. The log lists every preset: before
and after.

`reverb_level = <percent>` (default 100 = the presets as authored; overlay AUDIO > REVERB AMOUNT, live;
`SS_REVERB_LEVEL`) is a trim for the ear: it lowers the Room level of whatever preset the game hands to
`System::setReverbProperties` (50 = 6 dB less reverb, 0 = none), in a wrapper on that export of `fmodex.dll`.
Nothing measured calls for a value below 100: the reverb path of the PC release is the console's (same code in both
images: the reverb unit is moved to a channel group of its own, `ReverbSubmix`, behind a 150 Hz high-pass, and its
output connection carries the reverb as one mono signal at full level to the five main speakers and not to the LFE;
every voice gets its send from the event's reverb wet level; the preset is blended between the trigger zones). What
the restored presets change is how much there is of it: the open track (`DefaultAmbient`) has Room -1000 mB and
1.65 s, the halls and the overhead zones Room 0 and 2.7 to 4.0 s, the airport's hangar (`HANGER`) Room 0 and 3.92 s,
so the hangar is 10 dB wetter than the open track by design, and that reverb comes from all five speakers while the
dry sound sits on the front pair. Whether the console sounds the same there is a matter of one recording of each in
the hangar (`tools/audio`).

### Mix preset (`console_mix`)

The PC event files are a later revision than the Xbox 360 ones for about 20 events (volume of the tarmac and water
tyre events, overhead/tunnel swooshes, HyperReal helicopter/roll/squeal, nemesis missile beeps, final-lap HUD,
downtown collapse; pitch of four tyre events), and some samples were re-recorded at other levels (HUD ticks and
toggles up to 12 dB, a number of power play rolls, debris and drops 1.5 to 9 dB). Category volumes, the game's
DynamicMixer.xml (presets, side-chain ducking) and the side-chain tunables are the same on both. The overlay's
AUDIO > SOUND MIX row (`console_mix`, default `true` = XBOX 360 since 2026-11-13, a missing key included; `false` = PC ORIGINAL, the PC's own values; `SS_CONSOLE_MIX=0|1`)
gives those events the console values, live, for the sounds started after the switch (switching back to PC ORIGINAL puts
the PC values back the same way). The getEvent family of `fmod_event.dll` (nine vtable slots: EventSystem,
EventGroup, EventProject, EventCategory) is wrapped, and an event the table names (FNV-1a of project + event name,
FMOD property index, the PC value) gets the console value on its template and on the instance, through
`Event::setPropertyByIndex`, only while it still holds the PC value. That covers 78 values: 73 volumes, 4 pitches
and TRUCK_IMPACT's max playbacks (PC 4, console 1). Five sound instance volumes in the RubberNeck (crash camera) loop
of CollisionEffects are 5 to 11 dB louder on PC (Car_take_out 0.41 against 0.20, Car_in_air 0.41 against 0.11,
two Glass 0.17/0.16 against 0.07, Swooshes 0.16 against 0.09); they are no run-time property, so the game's file
callbacks (`System::setFileSystem`) are wrapped and the five floats replaced as FMOD reads CollisionEffects.fev
(by file name, size and offset, only while they hold the PC value; CONSOLE when the file is opened, i.e. at start).
Table: `src/console_mix_table.h`, generated by `tools/audio/fev_compare.py table` (event values from both releases'
event files; the sample gain only where every sample an event plays was measured, within 3 dB of each other, and at
least 1.5 dB off; event volumes are capped at 1.0, so samples that got quieter on PC are recovered only up to that).
Not covered: the Nemesis-helicopter-level music (-2 dB on PC, a DynamicMixer preset), events sharing name and value
with an unchanged event (DETONATE, SLIDE). Log lines `[console_mix]`.

`audio_diag = true` (`SS_AUDIO_DIAG=1`) is a diagnostic: it logs the FMOD DSP network every 10 s (`[diag]`, units by
type, a compressor would be type 17, and every unit that is not a channel group or voice), every DSP the game creates
or inserts (`[dsp]`), every event fetched (`[event]`: category, volume, mode, 3D distances, spread, pan level) and
started (`[start]`: with its parameter values), and every category level move of 0.5 dB or more (`[duck]`: fades,
presets, side-chain ducking). It also logs the reverb path (`[revdiag]`: every preset the game hands to
`System::setReverbProperties`, each voice's reverb send, the speaker levels on the reverb submix), the event
properties the game writes with the event file's value beside them (`[prop]`, and the table `[props]` every 10 s)
and the game's voice DSP units (`[vdsp]`). `SS_AUDIO_REVERB_TEST=mute` (with `audio_diag`) hands FMOD every preset
with Room -10000, to see in a recording what the reverb return carries. Recording and comparing: `tools/audio`.

### Sound bank offsets (`audio_fsb_fix`)

Every PC sound bank (`data/audio/release/**/*.fsb`, FSB4, MPEG) starts each sound's data on a 32-byte boundary, with
zero bytes after the sound before it; the header's data size counts them. FMOD on PC (fmodex.dll 4.28) finds a
sound by adding up the compressed lengths of the sounds before it without those bytes, so it starts reading up to
2 KB too early: the last MPEG frames of the previous sound play first and as many frames are missing at the end.
In `music.fsb` 112 of 149 sounds start with 1 to 4 frames (26..105 ms) of the track before them. The result screen
music loops over the whole file, so the tail of another track is heard at every loop (SS04: the last 78 ms of SS03,
found in a recording on the Deck, and SS04 cut 80 ms before its end; the console version's audio has neither).
`audio_fsb_fix = true` (default; `SS_AUDIO_FSB_FIX=0|1`, next start) wraps the game's file callbacks
(`System::setFileSystem`) and, for a bank whose own header shows exactly that layout (FSB4, every sound MPEG, the
lengths rounded up to 32 adding up to the data size and the plain ones not), rounds each sound's compressed length
up to 32 as FMOD reads the header. No file is changed and the sound data passes as it is. Log line `[fsb]`, one per
bank.

### Music time line (`audio_music_log`)

`audio_music_log = true` (`SS_AUDIO_MUSIC_LOG=1`, default false) changes no sound; it writes `[music_log]` lines for
a report of music that should not play (short pieces of music at the end of a race). The game's music manager
(update 0x83DC60) plays race sections, the finish stingers and the result screen music as FMOD events; its race
update (0x83D6C0) stops the playing section when the fade of the game's "music" mix group is under 0.05 and starts a
section anew when the fade is back. With `audio_sidechain` that group really ducks (-60 dB under speech), which the
PC release never did. Logged, with the log's millisecond times: `Event::start`, `stop`, `setPaused` and larger
`setVolume` moves of every event in the music or Speech category (the game's imports), each with the event's volume
and what the game last asked of the music, Speech and master channel groups; and every move of those three groups
across 0.05 or by 3 dB. 6000 lines at most.

### Side-chain ducking (`audio_sidechain`)

The game ducks some audio groups while others are loud, as `DynamicMixer.xml` sets out (`SideChain*` presets, the
same on PC and Xbox 360): collisions duck the music -2 dB and both engine groups -5 dB (fade in 10-22 ms, out 200 ms),
power plays duck collisions -6, surfaces -12 and the engines -6 dB, speech ducks music and swooshes -60 and the player
engine -18 dB, the HUD ducks the music -3 dB. The level each side chain listens to comes from a `BlackRock_VolumeMeter`
the file puts in slot 7 of every audio group. The PC release creates the 16 meters (and the Powerplays flanger) but
never puts them in FMOD's DSP network: only FMOD's own filters reach `ChannelGroup::addDSP`. A meter outside the
network never sees a sample, every side chain reads silence, and nothing ever ducks (measured: no category moved by
0.5 dB during a dozen impacts). `audio_sidechain = true` (the default: measured on a race, collisions duck the
music -2.0 dB and both engine groups -5.0 dB within 0.1-0.2 s, as the file sets out; `SS_AUDIO_SIDECHAIN=0|1`;
overlay ADVANCED > AUTO DUCKING, restart) wraps `System::createDSP` and
`ChannelGroup::addDSP` in `fmodex.dll`, notes each meter as the game creates it (the k-th meter is the k-th group of
the file's `<AudioGroups>`; its channel group is the one that group's filters were just put on, else the category
of that name) and puts it in with `ChannelGroup::addDSP` on the next `EventSystem::update`, on the game's audio
thread. Log lines `[sidechain]`; with `audio_diag` the ducking shows as `[duck]` lines.

Why the meters are missing: every DSP unit of the game's own (the meters, the Powerplays flanger, one
`BlackRock VoiceDSP` per voice) is an object whose "attach to this channel group" method calls
`ChannelGroup::addDSP` in the Xbox 360 image (13 instances of it), and in the PC image only returns the unit
(`mov eax,[ecx+0x19c]; ret 4`): the PC build creates them all and inserts none. The meters are the part of that
this switch gives back. The flanger stays out (no measurement of it against the console yet).

### 3D placement of the game's own voices (`audio_voice_dsp`)

The game places most positional sounds itself, opponent cars among them: a 3D voice whose sound has
"IsUsingVoiceDSP" on (the default) gets its own `BlackRock VoiceDSP` unit when it starts, puts its event's six
speaker levels at 1.0, and from then on only moves the unit's parameters 8..13 (L, R, C, LFE, rear L, rear R),
which the game computes every update from the listener (front, back, side, distance). The unit reaches its sound
through one virtual, "attach to this channel group". In the PC image that slot of the unit's vtable
(0xC32848 -> 0x8A5B40) is `mov eax,[ecx+0x138]; ret 4`: it hands back the FMOD DSP and never calls
`ChannelGroup::addDSP`. So on the stock PC release no voice unit ever sees a sample: those sounds play on all
speakers alike and are never moved, whatever the device or Wine does (FMOD's own 3D panning is not used for them).
`audio_voice_dsp = true` (default; `SS_AUDIO_VOICE_DSP=0|1`; overlay ADVANCED > 3D SOUND POSITIONS,
restart) points that slot (in `.rdata`, a data write) at a function of ours that does what it is for:
`DSP::remove` if the unit already sits elsewhere, `ChannelGroup::addDSP` on the channel group the game passes,
`DSP::setActive(true)`, and returns the DSP as before. Two more things make the units sound right on PC:
- The unit's read callback (0x8A5470 -> 0x8A8520) processes exactly 256 frames per call, the console mixer's block,
  and writes six channels (L R C LFE RL RR) using the input channel count as the stride. FMOD Ex on PC reads 1024
  frames per call, so the unit filled a quarter of each block and the rest was noise. At `System::createDSP` the
  unit gets our read callback instead (in FMOD's copy of the description; the game's code is untouched), which feeds
  the game's one 256 frames at a time, and on a stereo (or mono) mix places the sound in a 5.1 buffer and folds it
  back (L = FL + 0.71 C + RL, R likewise; LFE dropped). Other layouts pass through unplaced.
- The units come from a pool of 64 and are not disconnected when their voice ends, while the event instance and
  its channel group are reused by later voices. When a unit goes on a channel group, any other voice unit still on
  that group is taken off first, so each event group has one unit, never a chain.

Only units created with our read callback are connected. While it is on, `audio_speaker_fix` is not applied: the
1.0 levels it removes are the units' input. Log lines `[voice_dsp]`.

### Opponent engines (`audio_cpu_engines`)

In the PC release the opponents' engines are silent (the stock game too, checked under Proton). The cause, from
the PC exe: each opponent's engine is its own event project (`Audio/Release/Vehicles/CPU/<car>.fev` + `.fsb`),
loaded into one of 7 "CPUCar" slots. Each slot has a fixed sample-bank buffer of `[0xD3DBC0]` bytes, and the loader
(`0x8695D0`) refuses a bank larger than the buffer, then unloads the project without a word. The size is `0x19999`
(104,857 bytes), the Xbox 360 value: the Xbox car banks are 57..94 KB (XMA), the PC ones 148..203 KB (MP2), so on
PC every car bank is refused, the engine events are never registered and the game gets no sound object for them.
`audio_cpu_engines = true` (default; `SS_AUDIO_CPU_ENGINES=0|1`) writes that one `.data` value to 256 KB before
the game makes the slots (7 x 256 KB out of a 36.5 MB sound-bank heap). Log line
`[cpu_engine] car-bank buffer 104857 -> 262144 bytes`.

The same key also keeps an older guard on the play itself: each frame the car-audio update drives an opponent's engine through a pooled 3D sound object; until
a "started" byte is set it calls the object's play (vtable `0xC32A2C` slot +4, `0x8A6F20`, from `0x85C15E`) and then
sets the byte whatever happened, so the play is asked for once per race. The play has a range check: when the
object's flag +0x382 is set, it starts only if a listener is within the object's radius (+0x29C) at that moment
(`0x8A63F0`); otherwise it returns without starting, and that engine stays silent all race (no `Event::start` is
ever made for a `CPUEngine` event). `audio_cpu_engines = true` (default; `SS_AUDIO_CPU_ENGINES=0|1`) swaps that
vtable slot (an `.rdata` value) and, for the call from the opponent-engine branch only, makes the real start
(`0x8A7700`) when the check would refuse; FMOD's own distance roll-off still sets the level. Every other caller
gets the game's play unchanged. Log lines `[cpu_engine]` (each opponent play: the check's inputs, listeners, the
outcome) and `[events] Event::start` (every start FMOD refuses, with its error, and the CPUEngine starts).

### 5.1 speaker levels (`audio_speaker_fix`)

On a 5.1 system the PC release plays its 2D sounds (collisions, swooshes, tyres, power plays, the crash camera)
out of all six speakers at nearly the same level, centre and LFE included; the console puts them on the front pair.
The event files are not the cause: their speaker levels are the console's (2D: L 1, R 1, centre and rears 0, LFE 0
or a small send). The PC game itself sets four of them, `FMOD_EVENTPROPERTY` 27 (centre), 31 and 32 (rears) and
33 (LFE), to 1.0 on almost every event instance it plays, through `EventI::setPropertyByIndex`. Wine is not the
cause either: with FMOD mixing everything in software (`audio_software_mix`, no hardware voices) the spread is the
same. `audio_speaker_fix = true` (default; `SS_AUDIO_SPEAKER_FIX=0|1`; overlay ADVANCED > SURROUND LEVELS FIX,
restart) wraps that virtual of `fmod_event.dll` and keeps the event file's level wherever the game writes exactly
1.0 over a level the file does not have at 1.0. The engine's moving speaker levels pass unchanged. The Tonic
engine gains (`Audio/Tonic/MultiChannel/...`) are not involved: they have the same defaults on PC and Xbox 360
and only feed the engine voices. Measured on a race (6-channel capture), collisions moved from rears -0.8 dB,
centre -1.3, LFE -1.2 against the front pair to rears -10.9, centre -7.8, LFE -18.9; between sounds the LFE is silent.
Log line `[speaker_fix]`.

The cause, from both images: a sound the game places itself gets a `BlackRock VoiceDSP` on its event's channel group,
the event's six speaker levels are set to 1.0 (L, R, centre, LFE, both rears; `FMOD_EVENTPROPERTY` 26, 28, 27, 33,
31, 32) and the placement is done by that unit. The Xbox 360 build inserts the unit; the PC build never does (see
side-chain ducking: 64 voice units created in a race, none in the DSP network), so the 1.0 is all that is left and
the sound is on every speaker. Keeping the event file's levels is what the unit does for a sound that is not moved
(the file's levels times the game's speaker gains, which are 1.0). A sound the unit would move between the speakers
is not moved on PC by this switch; `audio_voice_dsp` (above) connects the units and gives the movement back. The sounds the game pans through the event properties themselves (the engine event: all
six levels move, the 3D pan level goes to 0) are untouched. The flush that writes the properties writes nothing
else of weight: over a race, pitch (properties 3, 5, 9), the 3D pan level (25) and the speaker levels (26, 27, 28,
31, 32, 33); the reverb wet and dry levels (34, 38) are held back in the game's code and go out as the voice's
reverb send. The side speakers (29, 30) are never written and are not used in 5.1.

### Output devices: 5.1, 7.1, stereo

The game sets FMOD's speaker mode to 5.1 itself and never asks the device, so FMOD always mixes six channels (FL FR
FC LFE BL BR) whatever is connected; the switches above act before that and are the same for every device. Measured
under Proton with null sinks (`tools/audio/ab_capture.sh`):

* 5.1 device: the six channels arrive as they are.
* 7.1 device: Wine opens an eight-channel stream and puts the six channels on front, centre, LFE and the rear pair;
  the side pair stays silent (digital silence over a whole race). Nothing is lost, the surrounds play on the rear
  speakers only. To have them on the sides as well, give the game a 5.1 device (the card's 5.1 profile, or a
  PipeWire 5.1 virtual sink in front of the 7.1 one, which can copy the rears to the sides).
* stereo device: Wine folds the six channels (in its dsound source: front + 0.7 centre + 0.24 rear, LFE dropped). With the speaker fix the
  2D sounds are front-pair only, so they pass the fold at their own level; no clipping over a race (peak 0.78).

### Mixer (`volume_*`)

`volume_master`, `volume_music`, `volume_sfx`, `volume_engines`, `volume_voice`, `volume_ambience`: percentages
(0-200, default 100 = as the game plays it) that multiply the game's own volume for each group of FMOD event
categories. The PC event files have one flat category tree (the log prints the one actually loaded, as
`[mixer] category tree: ...`):

    master{music, PlayerEngine, CPUEngine, Swooshes, Surfaces, Powerplays, Collisions, HUD, Ambience,
           Particles, Spot_Ambience, HyperReal, Speech, Camera}

Default mapping: master -> `master`; music -> `music` (the in-race and menu music are events of this category, not a
separate stream); engines -> `PlayerEngine`, `CPUEngine`; voice -> `Speech`; ambience -> `Ambience`,
`Spot_Ambience`; sfx -> every other top-level category (the set the game's own effects slider covers).
`mixer_map = group:cat,cat;group:cat` replaces the categories of the groups it names. Bink movie sound (intros) does
not go through FMOD and is not affected.

How: at audio init the game takes each top-level category's FMOD channel group and from then on writes its fades,
sliders and ducking straight into those channel groups with `ChannelGroup::setVolume`, every update, so a gain on
`EventCategory::setVolume` would be overwritten. The DLL therefore puts a 5-byte jump on `fmodex.dll`'s exported
`ChannelGroup::setVolume` / `getVolume` (memory of the third-party library, never the game's) to wrappers that
multiply the value for the channel groups of known categories and pass every other call through unchanged; the game
reads back its own value. The category tree is walked from inside `EventSystem::update` (a slot swap in
`fmod_event.dll`'s vtable, like the reverb). FMOD caps every channel group at full scale, so more than 100 only gives
back what the game itself turns down. With every group at 100 and `sfx_trim = 0` nothing is hooked; `mixer = true`
hooks at start anyway (to log the tree), and a change from the in-game overlay (`src/mixer.h`: `ss_mixer_count`,
`ss_mixer_key`, `ss_mixer_label`, `ss_mixer_get`, `ss_mixer_set`, live) puts the hooks in when it first moves a
group. Env overrides `SS_VOLUME_MASTER` ... `SS_VOLUME_AMBIENCE`, `SS_MIXER_MAP`, `SS_MIXER`.

`sfx_trim = <dB>` (default 0 = off) is the older flat trim: every top-level category except music, speech and HUD,
or the ones `sfx_trim_categories` names. It now goes through the same channel group wrapper and multiplies with the
groups above.

Per-category gains on top of the groups: `volume_player_engine` (PlayerEngine), `volume_opponent_engines`
(CPUEngine), `volume_collisions`, `volume_powerplays`, `volume_swooshes`, `volume_surfaces`, `volume_particles`,
`volume_hyperreal`, `volume_camera`, `volume_hud`, `volume_general_ambience` (Ambience), `volume_spot_ambience`.
Each multiplies with its group (engines x player engine). The tree has no level below these categories, so this is
the finest the mix goes without touching single events. The six main volumes (master, music, effects, engines, voice,
ambience) are rows on the overlay's AUDIO page, the twelve group volumes on ADVANCED. Every volume row goes from 0 to
120 % in steps of 5; above 100 % the sound can distort (clip), and the row's help line says so. The file takes up to
200 %: a value above 120 set there is shown as it is and kept; right leaves it, left moves it down one step at a time
(150, 145, ... 120). This holds for every number row: a step never moves a value further outside the row's range.

### Button prompts (`button_prompts`)

The pictures of the prompt bar (`A SELECT`, `B BACK`, `Y QUIT`, the popups and pause boxes, Start) follow the pad in
use, like the console port's. Sets: `auto` (default), `xbox360`, `xbox`, `playstation`, `nintendo`, `steam`, `keyboard`;
a row on the overlay's CONTROLLER page (BUTTON PROMPTS), live.

- **Where the game decides.** The prompt bar movie (`UI/CommonUI/Prompt_Bar.gfx`) has the frames `Xenon_A/B/X/Y/Start`,
  `PS3_Cross/Circle/Square/Triangle/Start` and the PC key caps, but on every prompt change `0x4DADE0` calls the movie's
  `Invoke(path, result, args, 1)` for the five icons (`main.bar_mc.button?_mc.icon_mc.gotoAndStop`, 0xBBABC8..0xBBAC7C)
  with the one argument `"PC_GenKeyLong"` (0xBBACA8): the PC build always shows key caps. The `GFxMovieRoot` vtable
  slot +0x48 (0xC41D3C, holds 0x98D2E0) is pointed at a wrapper that swaps the argument for the button's `Xenon_*`
  frame, or its `PS3_*` frame with `playstation`. One aligned 4-byte write in read-only data; `keyboard` writes nothing.
  The icons change at the bar's next prompt change.
- **The pictures.** The Xenon frames draw the external images `circle_buttons.tga` (128x64: the discs Y B A X in
  32-pixel cells over the PS3 symbols) and the Start button (64x64), textures of `UI/CommonUI/Textures/CommonUI.textures`
  (A8R8G8B8). For `xbox`, `nintendo` and `steam` the DLL draws them again as the game uploads them: the texture's
  `LockRect`/`UnlockRect` (texture and surface vtables) and the device's `UpdateTexture`, all in d3d9.dll's memory, are
  wrapped; a texture is known by the FNV-1a hash of its first level's pixels (the game's archive, not this repository,
  is where the hashes come from); the drawing goes over the game's pixels before `UnlockRect`, every mip level too,
  and the PS3 row is kept. A set change re-writes the found textures at once from the Present hook; `xbox360` puts the
  game's own pixels back. The drawing is the console port's vector art (`src/glyph_canvas.cpp`, `src/glyph_art.cpp`:
  discs, rings and a stroke alphabet, no font and no picture file): the PC draws it at the game's texture size (the
  console port draws it larger; here the game's texture keeps its size). `playstation` and `xbox360` show the game's
  own art in the prompt bar, `keyboard` the game's key caps. `test/glyphtest.cpp` (`make glyphtest`) draws the pictures on the host.
- **auto.** The last input picks keyboard or pad (a button or a stick pushed past half -> pad; a key while the pad
  rests -> keyboard; at start the pad when one is connected). The pad's kind comes from the Windows device list (raw
  input HID game pads, every 2 s), the game's DirectInput list as the fallback: Valve 28DE -> `steam`; Sony 054C ->
  `playstation`; Nintendo 057E -> `nintendo`; Xbox 360 (045E:028E and kin) -> `xbox360`; any other pad -> `xbox`. On a
  handheld (`SteamDeck=1` in the environment, or `SS_HANDHELD=1`) the Xbox kinds count as `steam`. Under Steam Input
  every pad is Steam's virtual pad 28DE:11FF; when Steam names the real pad behind it (the file
  `SteamVirtualGamepadInfo` points to: `[slot N]` sections with `VID`, `PID`, `type`), that one counts. The log has
  `[glyphs] pad: ...` and `[glyphs] button prompts: ...` lines.
- **The race HUD.** Its own pictures (the large A and B of `PRESS A TO FIRE ... POWER PLAYS`, the race tips' LT and
  RT, 64x64 each) are streamed textures, not in `resident.ark`: their hashes were taken from what the game uploads on the
  test install (`SS_GLYPH_SURVEY=1` logs every small texture's hash and writes its pixels to `glyph-survey\` beside the
  game). They are drawn for `xbox`, `nintendo`, `steam` and also `playstation` (the HUD has no PS frames to pick).
  Whether the PC HUD shows them at all with a pad was not seen in a headless run (no pad there).
- **Checked** on the Bazzite (headless, no pad, the set forced): `nintendo` and `xbox` in the track select's bar
  (`Y DIFFICULTY`, `B BACK`, `A SELECT` as X / A / B and as coloured rings), the pause box and the unlock popup; the
  three CommonUI textures are found at load and again when the front end reloads them.

### Frame pacing (`frame_pacing`, `frame_log`)

For players this is one row, DISPLAY > SCREEN SYNC (restart):

| Row value | `frame_pacing` | What it does |
|---|---|---|
| AUTO (SMOOTH) | `auto` (default) | Picks for the machine. Today that is SMOOTH everywhere: Windows, desktop Linux, the Steam Deck, at every frame rate. When variable refresh can be told (SSPC-32) AUTO may pick VARIABLE REFRESH there. |
| SMOOTH | `smooth` (old spelling `vsync`) | Every frame waits for the screen's refresh: no tearing, about one frame of delay. On a screen whose rate is not a multiple of the frame rate (90 Hz Deck OLED, 144 Hz) set the screen to 60 or 120 Hz. |
| FASTEST | `fastest` | The even grid of `present`, with the device made with interval IMMEDIATE whatever the game's own vsync option says: lowest delay, the picture can tear. |

Kept for measuring, set in the file only (the row names them when set): `vrr` (VARIABLE REFRESH, below), `present`
(the even grid, the game's own vsync option decides about tearing), `step` (the old wait only). `game_vsync`
(display.cpp) is conf-only too; `frame_pacing` is applied after it and wins.

The step's wait sits before the input read, the update and the render, so with no vsync the time between presents
varied as much as the work did. With `frame_pacing = present` the device's `Present` (slot 17 of the D3D9 device
vtable, in d3d9.dll's memory; device = `[[0xD64708]+0x578]`) is hooked: the step waits until the next 1/60 s grid
point minus the work it expects (a fast-rising, slowly decaying estimate), and the hook spins out the rest. The input
is still read just before the work, so the added latency is the ~0.5 ms margin. `frame_log` (default false since
2026-11-13; DEVELOPER page) writes a `[frames]` line every 10 s (fps, interval avg/sd/min/max, 1% low, frames over 1.25 and 1.75 periods, time spun at the present).

`loop_sleep = auto | game`: the game's main loop (`0x558760`) calls `Sleep(1)` after every step (import slot
`0xB9B120`, kernel32 `Sleep`). With the paced step that sleep is not the frame's rest any more (the step's own wait
is), it only shortens the time left for the work: on the Steam Deck about 5 ms of every 16.7 ms went outside the
step, so frames with more than ~11 ms of work were late while CPU and GPU were not busy. `auto` (default) redirects
the import slot and leaves the main loop's `Sleep(1)` out while the present hook paces the frames; `game` keeps it.
The `[frames] split` line shows `outside the step` and the sleep's part of it in both modes.

`net_poll = auto | game`: before every step the loop's dispatcher (`0x558810` -> `0xA15020` -> `0xA12480` ->
`0xA12360`) calls wininet's `InternetGetConnectedState(&flags, 0)` (import slot `0xB9B2CC`) and stores the answer's
bits at the app object's `+0x1F9..+0x1FB`. Under Wine / Proton that call walks the network adapters every time
(about 4.3 ms a frame on the Steam Deck: the `outside the step` time left after `loop_sleep`). `auto` (default)
redirects the import slot: a thread of the patch calls the real function every 10 s and the game's call returns the
last answer; `game` lets the game's call through. The split line shows the real calls' time and how many calls were
answered from the poll.

`frame_pacing = smooth` / `vsync` (and `auto`, the default, everywhere since 2026-11-13; before, only under gamescope at
`fps = 60`): without vsync the grid is QPC time, and under gamescope the display's clock picks which present is scanned out. The two clocks drift by hundredths of a percent, so
the frames' finishing times slide through the display's latch point; while they sit near it the GPU's frame-to-frame
jitter puts two frames into one refresh and none into the next. The counter stays at 60, yet for seconds at a time
about 30 different pictures a second reach the screen (Steam Deck, owner report; its log showed a perfect grid:
interval sd 0.03..0.14 ms, no late frames, no held frames). With vsync, `CreateDevice` / `Reset` get
`PresentationInterval = ONE` when the game asked `IMMEDIATE` (display.cpp; the game is told it got its own value), so
every frame gets its own vblank. The grid then runs 0.1 % fast and, when the real `Present` blocks (a full queue),
goes on from its return: the display is the clock. `DXVK_CONFIG` gets `d3d9.maxFrameLatency = 1` (unless it already
names a frame latency), so the full queue is one frame of latency. The step's tick accumulator leaks 2 % a frame, so
a display a little off 60 Hz gives one tick per present (the game runs at the display's rate) instead of a double
tick every few seconds; a frame really missed still gets its catch-up tick. The `[frames]` line adds the time spent
in the real `Present` and how many presents waited for a free slot. At start the display mode's rate is logged, with
a warning when it is not a multiple of 60: a 90 Hz Steam Deck OLED shows 60 frames for 1 and 2 refreshes in turn
whatever the pacing; set its refresh rate to 60 Hz in the Performance menu. A change between vsync and no vsync
takes effect at the next start (the menu row says RESTART NEEDED).

`frame_pacing = vrr` is for a display with variable refresh (G-Sync, FreeSync, VESA adaptive sync) that is active
for the game. The device presents on the vblank as with `vsync` (FIFO: no tearing), but a VRR display starts its
refresh when a frame arrives, so a frame late by 1 ms is shown 1 ms late; on a fixed-rate display the previous
picture stays for a whole extra refresh (8.3 ms at 120 Hz) and the frame after it waits. The presents must stay
below the display's top rate, else FIFO waits for the fixed vblank again: a race rate (`fps`) above the cap
min(refresh - 3, refresh - refresh²/3600), rounded down, is lowered to it at start (116 on a 120 Hz display, 138 at
144 Hz, 224 at 240 Hz; `fps = native` gives the cap; the `[frames] vrr:` line says which). That is the same cap
NVIDIA's driver takes with G-Sync, V-Sync and Low Latency Mode Ultra or Reflex, so the driver does not hold frames
back on top of ours. The grid then runs on the PC's clock at exactly the cadence (no 0.1 % fast grid and
no accumulator leak: the display follows the frames). Late frames are presented at once and the next one keeps the
grid, as in every mode, so the game's 1/fps steps stay in step with the clock. On a display without VRR, or with VRR
not active for the game, this judders (116 frames on 120 refreshes repeat a picture four times a second); use
`vsync` there. Whether VRR is active shows on the display's own refresh-rate readout (OSD) or the GPU driver's
overlay: in a race it reads the capped rate, not the maximum. Menus stay at 60 (inside the range of most VRR
displays). A Moonlight / Sunshine stream does not carry the variable timing: the client shows the stream at its
own rate. Changes to or from `vrr` take effect at the next start. Under gamescope (Steam Deck, Bazzite game mode)
VRR is gamescope's own setting; the Steam Deck's panel has none.

### Above 60 (`fps = 120 | native | 61..240`)

`docs/plan-120fps.md`, milestones M0 to M4 (its sections 11 and 12 have the measurements). Off unless `fps` asks for
it; with `fps = 60` only the timeline item below acts, and only in a slow motion's half steps.

* A single-player race whose rewind rings are still empty becomes a "fast" race: every update of the race state
  runs with the game's own time scale (`0x4AF810`: byte `0xD71208`, float `0xD113B8`, the lever of the half steps)
  at `60/fps`, set for the one update and put back after it, so a tick of the race update is `1/fps` s. The game
  clock (`ctrl+0x1A740`, not scaled by the game) is put back by the rest. The AI gate runs the steering when 1/30 s
  of game time has gathered. Split screen, network modes and anything that does not look like the known build stay
  at 60.
* Two cadences. RACE (`fps` updates and presents a second) while the previous update went through the race update;
  BASE (60) for everything else: menus, loading, movies, and the paths of the race state that ignore the scale.
  The camera director (`ctrl+0x1A680`) and the camera controller (`ctrl+0x370`) of the in-game controller get
  private vtable copies whose slot 5 counts the call and runs the original; the director's is called once per
  race update, which is how the step knows the path (`SS_PATCH_OFF=hfrhook` leaves them out, and with them the
  feature).
* The camera slow motion runs at half of the race's scale; a slowed rewind playback holds every second update.
* Timelines (`hfr_ticks = sixty`, the default; `raw` or `SS_PATCH_OFF=hfrticks` turns it off). The race update
  hands the update's integer tick count to slot 1 of four timeline managers (`[race+0x150]`, `+0x158`, `+0x15C`,
  `+0x160`: standard, collision, level load, vehicle), and a timeline moves one frame of 1/60 s per tick whatever
  the time scale. Each manager gets a private vtable copy whose slot 1 hands on `floor(acc += ticks x scale)`
  ticks and leaves the call out when that is zero (every call site sits behind "ticks > 0": the game's own case).
  With the scale at 1 the call passes through unchanged. **This also acts at `fps = 60`**, from the first half step
  of a race on: a camera slow motion in half steps ran the scripted sequences at double pace against the slowed
  cars (measured 2.00 frames per 1/60 s of game time, now 1.00).
* Rewind (`hfr_rewind = capped`, the default above 60; `off` or `SS_PATCH_OFF=hfrrewind`). The crash and takedown
  shots ask for `seconds x 62.5` ticks (`0xC5C124`, read-only data, three readers, all such requests). While a
  fast race is the current one the constant is multiplied by `fps/60` (one word through the locked writer) and put
  back for any other race. The requests' caps (360 / 180 ticks) are code: a rewind reaches `360/fps` s.
* Steering (`hfr_steering = scaled`, the default above 60; `off` or `SS_PATCH_OFF=hfrsteer`). The car's steering
  part (`vehicle+0x550`, `0x8D38E0`) turns the wheels by `steer x lock` degrees, and the lock (`0x8D3830`) is
  `curve(..) + atan(a1 x dt / |forward speed|)` with the physics step `dt` in it, `a1 = curve(yaw rate) x
  [cfg+0x20C] + [vehicle+0x124]` (`cfg = [vehicle+0x550]`, the driving model config + `0xEA0`). At 120 the wheels
  turned half as far for the same steering. While an update runs in 1/fps steps both terms of `a1` are multiplied
  by `fps/60` (heap words of the game's data, through the locked writer) and put back for every other update and
  race. Nothing is touched at `fps = 60`.
* Still wrong above 60: the other users of the integer tick count (not classified yet) (plan section 12). `fps_scope = all` is read and reported as not in this build.
* `native` takes the display mode's refresh rate at start (60..240). Numbers that are not multiples of 60 are
  accepted but not tested.
* Log (`SS_PATCH_LOG=1`, or always above 60): `[hfr] clock` (game seconds per wall second), `ticks`, `timeline`
  (frames the playing timelines move per 1/60 s of game time, for normal updates, slow motion and playback),
  `cars` (path the AI cars have driven), `rewind request` / `playback`, `cadence`.
* Test aids: `SS_PATCH_MODE=120|native|present120` (present120: only the front end at 120, one tick an update),
  key-file commands `cad60` / `cadrace`, and `slowtest<seconds>` (the cameras' one-tick request is assumed for that
  long: a slow motion at a chosen moment; also at 60).

### Takedown and crash slow motion (`takedown_realtime`)

Two slow motions are lost at 60, because both give the update one tick and at 60 an update holds one tick already:
- the cameras' slow motion: the in-game controller's update (0x4B4F90) runs with one tick when, in a one-player race,
  the camera output's `+0x2C` (race `+0x30`) or the byte `+0x2A0` of the object at race `+0xCC` is set (the crash and
  takedown shots set it from their `Slow_Motion_Criteria`);
- the takedown replay: a slow-motion rewind playback (manager `[0xD71264]`: `+0x100` playing, `+0x108` slow).

One decision per step, as the port's `SS_REPLAY_SLOW_HOLD`: in a playback only the replay's flag counts, otherwise the
cameras' request (read from the game's data, never written).

**Half steps** (`slow_half_steps = true`, the default, = live camera slow motion only; `slow_half_steps = playback` adds
the takedown replay; `false` holds; `SS_SLOW_HALF_STEP` = 0 hold, 1 live only, 2 live + playback, as the port's values): a slowed step runs the game's update with its one tick at half the time, so the slow motion shows 60
different pictures a second instead of 30 shown twice. Reverse engineering of the decrypted image: the race update
0x4B2670 passes the tick time and the update time through 0x4AF810 (called once, at 0x4B2730), which multiplies both by
the debug tweakable "Physics/Change Phys dt/Slow Motion cheat speed" (float `0xD113B8`) while "Slow motion activated"
(byte `0xD71208`) is set; physics, the camera director and controller, the race timers and the post-loop systems take
the scaled time. For one update the byte is set and the factor is 0.5 (both `.data`, put back right after); the game
clock `ctrl+0x1A740` (added unscaled before the race update, 0x4B5083) is put back by half a tick.
A slowed playback plays each one-tick record over two half steps, as a two-tick record plays at 30: before the first
the record gets 2 ticks (so it stays under way; the post-loop "complete" query 0x7BDD90 answers 0), and the two
playback players that read a ring of their own one entry a call (race `+0x1AC` -> 0x7CCDC0, race `+0x208` -> 0x7C8810)
are kept from moving on in that half (`+0x208` = 0 skips its call; `+0x1AC` points at a zeroed stand-in that 0x7CCDC0
reads and finds empty). After the second the record gets its tick back and the ticks consumed (`+0x130`) 1, so the
next update leaves it as the game does. All heap / `.data` writes; every piece of code the reasoning depends on is
byte-compared with the known build first, and on any mismatch or anything unexpected the update falls back to the hold.

**Hold** (`slow_half_steps = false`, or the fallback): a slowed step skips the game's update (render only) when the
step before it was not skipped, so a slowed stretch runs one tick per 1/30 s, as at 30.

Log: `[takedown] slow motion start/end: N updates, H half steps, M held, W s wall, G s game (xR) (source)`; at the
right speed R is 0.50, and with half steps M is 0 (the `[frames]` line's "slow-motion frames kept" stays 0).


### Boot movies (`skip_intro`)

At start `cIntroState` runs the play list of `UI/Bootup/Bootup.gfx`: a disclaimer page (not skippable), the third-party
logo page, the autosave notice, then the Bink movies `Movies/BootUp/DISBR.bik` (studio logos) and `Movies/BootUp/Intro.bik`
(the intro film); the movies are marked skippable. With `skip_intro = true` (the default) pad #0 (the virtual pad, so it
needs `pad_support`) taps A while the intro runs, and the game skips each movie along its own path; the taps stop when
the game marks the intro over (`0xD660C4`). Nothing in the game is written. Tested on the Bazzite copy: the title screen
in about 14 s instead of 75. Two earlier ways were dropped after tests: cutting the movies to one frame in `binkw32`
(the game's Bink player read freed memory) and the game's own `-skipintro` byte (the front end read a table the skipped
intro path builds, 0xD96ACC, and crashed).

### Boot to the main menu (`boot_to_menu`; `src/boot.cpp`)

After the boot movies the title screen waits for Press Start. With `boot_to_menu = true` (the default follows
`skip_intro`) the patch passes it for you: once the title screen is shown with its Press Start overlay up, it does
what the game itself does on a Start press of pad 0 (the overlay, layer 8, is popped and the title screen's own
start routine `0x44DBC0` runs). The game then loads the profile in use (the save layer, `CreateSave.gfx`) and opens
the main menu by itself. The step is made from the title screen's own update (its vtable slot 15, word `0xBAA338` in
read-only data, swapped only while it holds the known `0x44DDA0`), so it can only happen on that screen, and only
once. Nothing is tapped and nothing in the game's code is written. It works the same with a pad, with Steam Input
or with the keyboard alone (no input is involved).

Your own first button press (any pad) or key press (while the game is in front) hands control back at once. A first
start without a profile stops at the profile creation (a name to type); a save error or the first-start training
question is left to you. The disclaimer and the autosave notice belong to the boot play list and keep their few
seconds.

Log: `[boot]` lines, one per front-end step (screen, top layer, save step), `title screen: Press Start passed ...`
and `main menu reached after N s with no button pressed`, or why it handed off.

### Texture filtering and detail (`anisotropy`, `draw_distance`, `shadow_distance`, `opponent_cars`; `src/quality.cpp`)

Keys, values and defaults as the port's settings rows. `anisotropy` takes slot 69 (`SetSamplerState`) of the same
device vtable as the present hook (in d3d9.dll's memory): with N, a sampler whose min and mag filters are linear and
which has a mip filter gets `MINFILTER = ANISOTROPIC`, `MAXANISOTROPY = N`; others are capped at N; `off` removes it;
`game` leaves the hook out (the game's own rule, 0x516A80: anisotropic at the device maximum at HIGH / VERY HIGH only).
`draw_distance` and `opponent_cars` write tunables in the game's data (never its code), re-applied whenever the game
loads them again (a level's `CityLOD.params`, `PlayerLOD_ps3.params`): the scenery LOD distances 0xD38A90.. (high, low,
fades, split-screen set; mid 0xD38A98 and stream radius 0xD389F0 kept), the visibility-map switch 0xD82788 above x1
and, times the factor, the opponents' off distance `AIFarToOff` 0xD37C98 (200 m as loaded) and `NemTruckFarToOff` 0xD37CC8;
PlayerLOD `MaxAIMidLODs` 0xD37C80 = 31 and `AIMidToFar` 0xD37C90 = 200 m (the Xbox file's) for `console`, plus
`MaxAINearLODs` 0xD37C78 >= 7 and `textureLodOverride` 0xD37CF0 = 0 for `high`. Log lines `[aniso]`, `[detail]`.
`shadow_distance = console | 1.5 | 2` scales the sun shadow's range and fadeout (300 m and 100 m at HIGH / VERY HIGH,
the `Lighting/CSM_Max/Settings` tweaks; the level's own numbers at LOW / MEDIUM): the light pass fills a settings record
every frame (0x88A3E0) and hands it to the shadow maps' compare and set-up methods; those two vtable slots of both shadow
classes (0xC314DC, 0xC31944, in the game's read-only data) point at wrappers that pass on a copy with the two numbers
times the factor. The cascade fit's slot gets a wrapper that gives the fit, for the call only, the split weight at
which the first cascade ends where the game has it: the added distance goes to the last cascade. Log lines `[aniso]`,
`[detail]`.
Not done: shadow map resolution (the 1024x3072 atlas and 1024 viewports are immediates in code, 0x88BDDE / 0x88BE19 /
0x88A5C6; the game's QUALITY option changes none of them). Findings and measurements: `docs/lod-shadow.md`.

### Shadow smoothing (`shadow_smoothing`; `src/shadowsmooth.cpp`)

`shadow_smoothing = off | on | high` (default high since 2026-11-13, a missing key included; the port's key and values). The game's sun shadow maps are stable
(the fit keeps each cascade's first radius and moves the light's projection to whole texels), but the line where the
sharp first cascade (1024 texels over about 25 to 35 m) gives way to the coarser second one is a hard switch in the
shaders, 25 to 35 m ahead of the camera: it runs along in front of the car and every shadow it crosses changes its
look at once. With on / high, every pixel shader whose constant table has `ShadowMap_SplitDists` (18 of the PC's,
ps_3_0) is replaced at `SetPixelShader` (slot 107, in d3d9.dll's memory) by a variant made on first use: the first
two splits read from a temporary, moved towards the camera by up to 25 % / 40 % of themselves by an interleaved
gradient noise of the pixel, so over that band the coarser cascade takes a growing share of the pixels (a dithered
hand-over, as the port's). Off: nothing is hooked. The menu changes it at once when it was on at start. Log lines
`[shadow]`. Findings: `docs/lod-shadow.md` section 6.

### Shadow softness and TAA tap rotation (`shadow_softness`; `src/shadowsmooth.cpp`)

`shadow_softness = 0.75 | console | 1.25 | 1.5` (default console; `SS_SHADOW_SOFTNESS`; ADVANCED row SHADOW SOFTNESS).
Every sun-shadow reader takes 8 Poisson taps in the shadow map, turned per pixel by a fixed noise texture
(`ShadowMap_texRotate`), and counts the lit ones: 9 shades. Their radius is `ShadowMap_FilterScale`, sent by the game as
1.56 / 1024 x (1, 2/3, 1/3) per cascade. The same variants as shadow smoothing read that constant times a factor (a
free constant the hook sets at every bind), so the penumbra widens by 1.25 / 1.5 or narrows to 0.75. The game's depth
bias is not scaled with it: at 1.5 slopes may show stripes and contact shadows thin a little.
With `aa = taa` / `taa_high` at start the variant also turns the taps' angle by a new amount every frame TAA draws
jittered (a golden-ratio sequence; one `mad` / `frc` / `mad` before the reader's `sincos`), so over TAA's history a
pixel sees 8 taps at many angles and the 9 shades blend into a smooth edge at no GPU cost. Frames TAA does not
jitter, and every frame without TAA, keep the game's fixed angle (a new pattern each frame would shimmer).
`SS_SHADOW_ROTATE=0` turns the rotation off for a comparison. The hook goes in when shadow smoothing is on, the
softness is not console, or TAA is on at start; the row then acts at once, else it says RESTART NEEDED. Log lines `[shadow] variant`.

### Debanding (`deband`; `src/deband.cpp`)

`deband = off | on` (default on; this patch's own key; `SS_DEBAND`). The game cuts its picture to 8 bits twice
without a dither: at the tone mapper (float light to an 8-bit target) and again at the colour grade (the 32x32x32
colour cube and the vignette). Dark gradients, fog, bloom halos and the vignette show steps; the grade's second rounding
widens them. With on, every pixel shader whose constant table names `SSFilmicToneMapper_Consts` /
`SSToneMapper_Consts` (the tone mappers) or `color_cube_color_cube_texture` (the grade; not the cube build) is
replaced at `SetPixelShader` (slot 107, in d3d9.dll's memory) by a variant made on first use: its `oC0` writes go to a
temporary, and at the end half an 8-bit step up or down is added to the colour by a fixed interleaved gradient noise
of the pixel position (no flicker; nothing for TAA's history to fight); alpha is kept. The tone mapper that writes
linear light with sRGB encoding on the draw gets the step through the sRGB curve's slope at the pixel's value; the one
that encodes itself and the grade get 1/255. A value on an exact code stays on it. TAA's copy of its 16-bit history to
the final target and the sharpening pass add the same noise (`postfx.cpp`); FXAA and SMAA pass most pixels through
and are left alone. HDR finds the tone mapper and the grade behind the variants. The 10-bit HDR output pass is not
dithered. Off: nothing is hooked. The menu (GRAPHICS, SMOOTH GRADIENTS) changes it at once when it was on at start. Log lines
`[deband]`.

### The game's QUALITY option on the Steam Deck

Keep the game's post-processing (QUALITY) on High on the Deck; Very High draws at 4x the pixels. The game's Very High is
2x2 supersampling: the race is drawn at twice the width and height and scaled down to the screen, about 32 fps on the
Deck instead of 60. The patch leaves this option to the game; an old `post_quality` key is ignored with one log line.

### LOD smoothing (`lod_smoothing`; `src/lodfade.cpp`)

`lod_smoothing = off | on | high` (default on since 2026-11-13, a missing key included; overlay GRAPHICS > POP-IN FADE;
the port's key and values) turns the scenery's one-frame mesh switches
(the low mesh and the streamed full mesh trading places at the mid LOD distance, and when the full mesh has streamed
in) into a dithered cross-fade of half a second / a second, and fades in the objects of a sector the visibility map
has just let through (at console draw distance its LOD caps switch whole sectors on when the camera changes cell).
It also fades the power-play system's objects (the level's moving assets, such as the airport's turning radars, and
the power-play set pieces) in and out over a band in front of their own draw distance, where the game switched them
on and off in one frame: their renderable's model pointer points, for the frame's render, at a copy of the shared
model whose mesh is a marked copy, so only that instance fades.
The sun shadow fades with all of these (the caster passes take the same mask, in the shadow atlas's texels), and a
shadow whose caster switches mesh in the cascades' own lists fades on its own.
With it on, or `draw_distance` above console, the knock-over props' draw distance (one value for the view, 200 m or
the camera's box's) is also taken in hand through its force tweak (0xD40C00): times the `draw_distance` factor, and
moved at 100 m/s at most, so a box change no longer shows every prop in between at once; with `lod_smoothing` on,
a prop also fades out over the last 30 m / 60 m before that distance (the same mask, its draws told apart in the
device hook by the game's stack; the title's own prop fade parameters are read by no PC shader); `draw_distance` above console
also takes the idle power-play objects off the visibility map (0xD8B368), like the scenery. The game's per-object draw is reached by direct calls only,
so nothing of it is wrapped: for a fading object a combined mesh (the game's own mesh table form: per pass the new
mesh's batches, then the old one's) is built in the DLL's memory and the object's mesh pointer (a word in the game's
heap) points at it for the frame's render only; the copies' vertex strides carry a mark that the device's
`SetStreamSource` hook reads and removes, and `DrawIndexedPrimitive` for a marked stream uses a variant of the game's
pixel shader that begins with an 8x8 ordered-dither `texkill` (shader model 3 bytecode, made on first use) with
complementary masks for the two meshes. Device slots 37, 82, 100, 107, 109 in d3d9.dll's memory; with `off` nothing is
hooked. Mechanism, limits and measurements: `docs/lod-shadow.md`. Log lines `[lod]` (`SS_LOD_LOG=1` for a line every
ten seconds).

### Slipstream streaks (`slipstream_fix`; `src/slipstream.cpp`, `src/streak_rebuild.cpp`)

`slipstream_fix = true | false` (default true; read at start, overlay ADVANCED > SLIPSTREAM EFFECT). On the Xbox 360,
translucent white streaks run beside the car's front corners while it drafts another car. The PC release shows at
most a faint flicker at the nose, because two things in its data were converted wrongly:

* The streaks' animated texture (16 frames) holds only 8 of its frames, at half height; the other 8 are missing from
  the PC files. As the game loads it, the patch rebuilds all 16 frames from that PC data: the 8 present ones at full
  height, the 8 missing ones as mirror images of present ones. The files on disk are not changed and no console data
  is used. Only a texture with exactly that broken layout is touched.
* One byte of the streaks' particle definition, "the particles move with the car", is in the wrong place in the PC
  data, so the streaks were left behind. The patch sets it in the loaded data once a race runs.

The log shows `[slip] texture rebuilt: ...` at start-up and `[slip] attach byte set: ...` in the first race with a
draft. Anything unexpected is logged and left as the game has it. `false` changes nothing.

### Window mode and alt-tab (`window_mode`, `pause_on_focus_loss`, `display_log`; `src/display.cpp`)

The game always creates an exclusive-fullscreen device: its present parameters (renderer `+0x57C`, filled by
0x50DED0) say windowed only when the window's style kind (`+0x154`) is 1, which 0xA139FA sets only when
`IsDebuggerPresent()`; otherwise the window is `WS_POPUP` and moved to `(0, 0, w, h)` (0xA12AF0). With
`window_mode = borderless` or `windowed` the DLL takes slot 16 (`CreateDevice`) of the `IDirect3D9` vtable (in
d3d9.dll's memory, found through an `IDirect3D9` of its own made at load, long before the game creates its device) and,
on the device, slot 16 (`Reset`). Both pass a copy of the game's parameters with `Windowed = TRUE` (back buffer at the
game's resolution, scaled to the window by `Present`; intervals above one become one) and give the game its own
values back. After the device is created or reset (the resolution menu restyles the window and then resets,
0x470C28 / 0x470C4D), the window gets `WS_POPUP` over the whole monitor (borderless) or a caption with a client area
of the back buffer's size, centred (windowed). For a sharp picture set the game's resolution to the monitor's.

Alt-tab: a windowed device is not lost, so in borderless and windowed alt-tab out and back needs no `Reset` at all.
In fullscreen the game has a complete-looking lost-device path (Present -> `+0xB48`, `TestCooperativeLevel`,
0x50E620 releases its DEFAULT-pool objects through its resource registry, `Reset`, re-creates them; a failed Reset is
retried every frame). `display_log = true` checks it at run time: every D3DPOOL_DEFAULT object gets a private-data
marker, every `Reset` is logged, and a failed one lists the DEFAULT objects still alive with the game code that
created them. The game's window procedure (0xA12BA0) clears the "active" byte 0xD57108 on `WM_ACTIVATE`, which mutes
the sound and pauses a race; `pause_on_focus_loss = false` keeps that deactivation from the game's procedure (a
subclass, `SetWindowLongA(GWL_WNDPROC)`). Defaults (`fullscreen`, `true`, `false`) do nothing at all: no thread, no
Direct3D call, no hook (under gamescope the window mode is moot).

`extra_resolution = <w>x<h>` (borderless / windowed only) adds one mode to the list the game reads (`IDirect3D9`
slots 6 `GetAdapterModeCount` and 7 `EnumAdapterModes`, appended after the driver's modes of each format, with the
last real mode's refresh rate). The game keeps the resolution of `config_ss.cfg` only when the list has it: on a
2560x1440 monitor a 2560x1080 setting was replaced by 2048x1536 without the extra mode (a Windows PC, 2026-11-13). With it
the game's RESOLUTION option offers the size and a window of that client size shows a 21:9 or 32:9 picture on a 16:9
monitor (how the ultrawide tests were run).

### Anti-aliasing and sharpening (`aa`, `sharpen`, `src/postfx.cpp`)

The PC release has no anti-aliasing at any quality level. `aa = off | fxaa | smaa | taa | taa_high` (default smaa since 2026-11-13, a missing key included) and `sharpen = 0..1` (default 0)
are the port's own keys and values (`SS_AA`, `SS_SHARPEN`). The passes run on the finished 3D picture before the HUD and menus
are drawn on it: the game's Scaleform renderer (`AppGfx::cRenderer`, vtable 0xBC93DC) begins every movie with `BeginDisplay`
(slot 5, 0xBC93F0, holds 0x535080); that slot is pointed at a wrapper which, at the first display of a frame whose render target
is the frame's final target (`[[[0xD64708]+4]+0x64]+0x5C`, the surface the present function 0x50E400 copies to the back buffer),
copies the target and runs FXAA (or SMAA 1x: edges, weights, blending) and then the sharpening back into it. A frame with no
Scaleform display at all is filtered whole at the device's `EndScene` (no UI on it). The device's `EndScene` and `Reset` (slots
42 and 16 of d3d9.dll's vtable) are hooked for that and to free the passes' targets before a reset. All device state is put back
(a state block, plus the render and depth targets). The shaders are HLSL compiled at run time with the game's own
`d3dx9_42.dll`. With both keys off nothing is hooked. Notices: `THIRD_PARTY_NOTICES.md`. Log: `[postfx]`.

`aa = taa` and `aa = taa_high` are temporal anti-aliasing. Jitter: every frame after a resolved one gets a
sub-pixel offset (Halton 2,3, 8 positions), chosen right after the game's update. Most of the scene is drawn with a
world x view x projection matrix per object that the game uploads as a vertex shader constant (the matrix's columns in the
registers at c33, its rows at c64; the plain view-projection and projection at c8, c4), so an offset written into the
viewport manager's camera (`*0xD66000`, cameras of 0x2F4 bytes at +0x68: view matrix +0xD8, projection +0x158,
view-projection +0x198) after the update reaches only the few draws that take the camera's own matrices (the first version
did that: the picture did not move, the resolve took the offset out anyway, and a standing picture wobbled). The offset
now goes in at the device: `SetVertexShaderConstantF` (slot 94 of d3d9.dll's vtable) is wrapped, and from the update to
the scene/UI split every upload that starts with a clip matrix of the scene camera (z column = P[2][2] x w column, plus
P[3][2] in the translation row, with a perspective w column) is passed on as a copy with x += jx * w, y += jy * w. The
game's memory and camera are not written. The resolve takes out exactly the offset the frame's uploads got (none when no
upload was moved). Resolve, at the same place as the other passes (before the UI): the previous result is reprojected
through the G-buffer depth (the INTZ texture SMAA's predication reads; the nearest of the neighbouring depths) and
inverse(view-projection now) x view-projection last, checked against the last frame's depth (the same pixel is tried where
the reprojection does not match: the player's car under the chase camera, a car driving along), fetched with a
Catmull-Rom filter (the history is the picture without jitter: a standing camera reads it unfiltered), clipped to the
current 3x3 neighbourhood's colour range (YCoCg, mean +- 1.5 sigma where the picture stands, 1 sigma where it moves or
the history is the same pixel) and blended 10 % current (more where the same pixel's history is far outside the range:
a car turning in front of the camera). A place that was outside the last picture, or matches neither way, keeps the
current picture. `taa_high` first runs SMAA 1x (with its depth predication) on the jittered picture and resolves that, so
the places where the history is rejected (cars passing, disocclusions) are anti-aliased too, and looks for the nearest
depth in all 9 neighbours instead of 5; it costs about SMAA's time more. The history is A16B16G16R16F where the device
can filter it as a render target, copied out into the final target's format by a pass, or read straight by the
sharpening; where the device has two render targets of independent bit depths the resolve also writes the depth for the
next frame's match (else a pass of its own). While the scene is jittered, `taa_lod_bias` (default -0.5, 0 = off) is added
to the mip LOD bias of the game's mip-mapped pixel samplers from the update to the scene/UI split (quality.cpp's
SetSamplerState wrap; the HUD, the menus and the DLL's own passes keep the game's bias; aa = taa or taa_high at start puts
the wrap in even with `anisotropy = game`). The history is dropped on a camera cut (more than 5 m or 15 degrees in a
frame), a Reset or size change, a frame not resolved, or no depth; split screen is left alone. `sharpen = 0.25` suits
both. Test aids: `SS_TAA_JITTER=0` resolves without jitter; `SS_TAA_AB_AT=<s>,...` saves 4 consecutive frames (input and
result, and the result sharpened at 25 % and 50 %) before the UI; `SS_TAA_PROBE_AT=<s>,...` compares one frame's vertex
shader constant uploads with the camera's matrices and logs the tally per register; `SS_TAA_JITTER_CAMERA_UNTIL=<s>` puts
the offset into the camera instead (the first way) until that second; `SS_TAA_HISTORY8=1` keeps the history in the final
target's format, `SS_TAA_MRT=0` the depth copy as its own pass.

### Shader compiles off the render thread (`shader_cache`, `src/shadercache.cpp`)

The patch's HLSL (anti-aliasing, sharpening, HDR) is compiled on a worker thread of its own, below normal priority, one
compile at a time; only the shader objects are made on the render thread once the byte code is there (milliseconds). A mode
picked in PC SETTINGS keeps the old mode on screen while its shaders compile (on the Steam Deck up to about 4 s for
`taa_high`), then takes over in one frame; at start the passes begin a little after the first frames. HDR's shaders are
asked for at the first frame whenever `hdr` or `hdr_ready` is on, and the swap chain goes to HDR10 only once they are made.
`shader_cache = true` (default; `SS_SHADER_CACHE`) keeps the compiled byte code in `shader-cache\` beside the DLL, one
`.cso` file per shader, named by a hash of the source (defines included), the entry point, the profile, the compile flags
and the DLL's build; a damaged or foreign file is deleted and compiled again, a new DLL build compiles once. At most 128
files and 16 MB (the least recently used go first). The folder can be deleted at any time. Log: `[shaders]`, and per mode
`[postfx] shaders for ... on the worker: N, H from the cache (hit), C compiled (miss), F failed, T ms` and `shaders ready
for ...: the passes run from frame N`. Test aid: `SS_AA_SWITCH=30=taa_high,45=taa,60=off` changes the aa row at those
seconds after start, as the menu does.

### In-game settings (`menu`, `menu_developer`, `src/menu.cpp`)

The PC menus are Scaleform GFx movies with hard-coded rows, so the settings page is an overlay this DLL draws over the
finished picture, from the device's `Present` hook (also wrapped at `fps = 30` when `splitsecond.conf` exists; only
d3d9.dll's memory is touched): "PC SETTINGS", d3dx9_42's font (the game loads that DLL), a state block around the
drawing, `Reset` wrapped to free the overlay's objects first. F1 or holding Back/View for 0.7 s opens it (or Y on the
game's Options screen, `options_prompt`); while it is open (and until the buttons that worked it are released) the
game's pads read centred and its DirectInput keyboard and mouse report nothing pressed (the keyboard and mouse devices
are wrapped by the `IDirectInput8A` wrapper, so this needs `pad_support = true`).

Pages and rows (reviewed for players, SSPC-33):

| Page | Rows |
|---|---|
| DISPLAY | FRAME RATE (30 / 60 / 120), SCREEN SYNC, WINDOW MODE, RENDERER (Windows, only with the `dxvk` folder), PAUSE IN BACKGROUND, ASPECT RATIO, HUD WIDTH, FIELD OF VIEW, SLOW MOTION |
| HDR (only where the swap chain can do HDR10) | HDR, CALIBRATE HDR, PEAK BRIGHTNESS, GAME BRIGHTNESS (`hdr_paper_white`), HUD BRIGHTNESS, HIGHLIGHTS, LIGHT SOURCES, DEEPER DARKS |
| GRAPHICS | ANTI-ALIASING, SHARPENING, TEXTURE FILTERING, DRAW DISTANCE, SHADOW DISTANCE, SHADOW BLENDING (`shadow_smoothing`), POP-IN FADE (`lod_smoothing`), OPPONENT CAR DETAIL, SMOOTH GRADIENTS (`deband`), PRE-COMPILE and PRE-COMPILE ALL TRACKS (`pipeline_warmup`; only under DXVK with the record files) |
| AUDIO | MASTER, MUSIC, EFFECTS, ENGINES, VOICE, AMBIENCE VOLUME, TRACK REVERB, REVERB AMOUNT, SOUND MIX |
| CONTROLLER | VIBRATION, VIBRATION STRENGTH, LEFT / RIGHT STICK DEAD ZONE, TRIGGER DEAD ZONE, BUTTON ICONS, CONTROLLERS CONNECTED LATER (`pad_hotplug`) |
| GAME | SKIP INTRO MOVIES, SKIP PRESS START (`boot_to_menu`), SAVE BACKUPS, ONLINE SIGN-IN and ONLINE NAME (unless `online_server = off`), PC SETTINGS BUTTON IN OPTIONS (`options_prompt`) |
| ADVANCED | SMAA: USE DEPTH, SHADOW SOFTNESS, SLIPSTREAM EFFECT, 3D SOUND POSITIONS, SURROUND LEVELS FIX, AUTO DUCKING, the twelve group volumes, CONTROLLER LAYER (`pad_support`), OLDER CONTROLLERS AS XBOX, IGNORE CONTROLLER IN BACKGROUND |
| DEVELOPER (only with `menu_developer = true`) | FRAME LOG, DISPLAY LOG, AUDIO LOG, LOG BUTTONS, SLOW MOTION AT 60 (`takedown_realtime`), SCRIPTED SEQUENCES, REWIND ABOVE 60, STEERING ABOVE 60, LIGHT SOURCES TEST, and every row whose key is not in the menu's key table |

Words used throughout: PC ORIGINAL = what the Steam release does; XBOX 360 = the console's value. Under the rows a help
line says what the focused row (or its value, for SCREEN SYNC and ANTI-ALIASING) does. `menu_developer = true | false`
(default false; `SS_MENU_DEVELOPER`, read each time the overlay opens) shows the DEVELOPER page. Rows hidden from the
players' pages keep working from `splitsecond.conf` and their `SS_*` variables; no key was removed. Values set in the
file that a row does not step through (`fps = native`, `frame_pacing = vrr | present | step`) are shown by name with
`(CONF)`; left/right then moves to the row's own values.

Left/right changes the value and writes the key into `splitsecond.conf` at once (the key's line replaced or appended,
everything else kept; no A press); A/Enter only opens an action row (CALIBRATE HDR); B/Esc or F1 closes; LB/RB or Q/E
turn the page. Live keys (`slow_half_steps`, `takedown_realtime`, `frame_log`, `pad_rumble`, `pad_rumble_gain`, the
dead zones, `button_prompts`, the volumes, `reverb_level`, `console_mix`, ...) act at once. A row that is read at start
only carries a grey RESTART tag and says RESTART NEEDED once its saved value differs from the running one. Some keys
act at once only when the part's hook was put in at start, since the menu never adds a hook: `aa` and `sharpen` when
either was on at start or the HDR hooks are in (`hdr_ready`, the default), `anisotropy` when it was not `game`,
`pause_on_focus_loss` when it was `false`, `hdr` unless `hdr_ready = false` and `hdr = false` at start,
`lod_smoothing` and `shadow_smoothing` when they were `on` or `high` at start (`shadow_softness` too, or when it was
not `console` or `aa` was a TAA mode), and `deband` when it was `on`; otherwise
they say RESTART NEEDED too. `frame_pacing` (SCREEN SYNC), `fps`, `aspect`, `hud_width`, `fov`, `window_mode`,
`draw_distance`, `shadow_distance`, `opponent_cars`, `skip_intro` and the other start-up keys are read at start only.
Rows come from a registry (`src/menu.h`: `ss_menu_register`) other parts add to; the key table in `src/menu.cpp`
(`kKeys`: key, level, page, label, help) decides where and how each is shown. Log lines `[menu]`.

### Picture shape and field of view (`aspect`, `hud_width`, `fov`; `src/aspect.cpp`)

What the PC build does: for every view the in-game camera setup (0x4B8DD0) makes the view window width
`2 tan(fov/2) * 16/9` (0x794370 with its force flag returns the constant 16/9 at 0xBAA9DC) and the height width / back
buffer shape (renderer `+0x30 / +0x34`): the horizontal field of view is a 16:9 screen's on every screen, so 16:10 gets more
above and below and 21:9 / 32:9 are cut at the top and the bottom. Every Scaleform movie is made with
`SetViewScaleMode(ExactFit)` (0x536920) and given the whole screen, so the interface is stretched on any shape but 16:9.

With `aspect = auto` (default) on a screen wider than 16:9 the scene is Hor+: the 16:9 picture's vertical field, more at
the sides. The game is made to build it itself: the constant 16/9 that 0x794370 returns to the camera setup (0xBAA9DC,
read-only data) is set to the screen's shape, so 0x4B8DD0 makes the window `2 tan(fov/2) * A` wide and `2 tan(fov/2)` high,
and everything the update derives from the camera sees the wide one: the six frustum planes 0x4B1C20 copies into the
frame's visibility data for the culling job (+0x1A770, and the far set +0x1A9B0), the streaming views (0x4B45EA,
0x4B4C29), the chase cameras' collision box (0xAAAFD0), the camera zones (0xA9F0F0). Before (2026-11-10 .. 13) the camera
was widened only after the update, so the planes copied during the update were the 16:9 ones and objects only in the
side strips could be culled. `aspect_culling = false` (test switch) brings that back for comparisons. After the update
(the frame step, and the device's `BeginScene` as a fallback that also covers `fps = 30`) every perspective camera of
the viewport manager (`*0xD66000 + 0x68`, 0x2F4 each) that the game made is checked and, where its window is not the
final one (a fixed `aspect` shape, `fov` other than 100), rebuilt with the game's own matrix build and publish (0x50C0D0,
0x50C250). With `fov` the constant also carries the fov factor of the last frame (+1 %), so the culling frustum holds
the final one. On 16:9, and on 16:10 at fov 100, the constant stays 16/9 and nothing is touched.

`hud_width` (16:9 default, 21:9, full, letterbox) is the interface box's shape on a screen wider than 16:9. A 16:10 screen
(the Steam Deck) keeps the game's full-screen interface with every value but `letterbox`, which draws the 16:9 box with
bars above and below there (before 2026-11-13 any set value did that, and the example conf set it, so a Deck with a
conf copied from the example was letterboxed).

The interface: `GFxMovieRoot::SetViewport` (vtable slot 0xC41D44, holds 0x98CDF0) is wrapped and makes the viewport the
middle 16:9 box of what the game asked for, scissored to it; `Display` (0xC41D70) is wrapped to catch movies set up
before the wrapper. Movies the game draws into a target of their own (the route-change letterbox, the transitions,
the HUD cards) are left alone and keep the full width. Menus that sit over the stopped race with bars or a backdrop of their own (file names containing PauseMenu,
IG_GameModeInstructions, EventObjectives, RaceEnd, RaceResults, RaceStats, Rewards) get black side strips after they are
drawn (`Clear` with the viewport and scissor opened to the back buffer), so they are pillarboxed like the loading screens
instead of showing the race beside their bars and tints (seen at 2560x1080 and 2560x720: the pause menu's bars and the
track intro's tint ended at the box). Videos (Bink) play inside the menu movies and are pillarboxed with them.
Words in read-only data (one constant, two vtable slots) and one slot of the device vtable in d3d9.dll; no code is
written. Log lines `[aspect]` (each movie file once, the first 100).

Markers: the Power Play icons over cars and hazards (0x4C2B20) and the online name tags (0x4C6F90) are projected during
the update with the manager camera's matrix (cam+0x198) and drawn by the engine from textures of their own, so they
follow the widened camera now that it is wide during the update. Not seen in a test (the harness never earns a Power
Play). Not covered: screen-space effects keep their pixel sizes (the vignette stretches with the screen); split screen
is widened like one view.

### HDR output (`hdr`, `hdr_ready`, `hdr_peak`, `hdr_paper_white`, `hdr_hud`, `hdr_shadows`, `hdr_highlights`, `hdr_lift`; `src/hdr.cpp`)

`hdr = true` (default false; the port's keys, values and defaults; the menu's HDR row switches it live) shows the light the game's tone mapper clips at
white above paper white on an HDR10 display. The PC renderer is the console's: the race is lit into a 16-bit float
target and one full-screen draw (`SSFilmicToneMapper` + `SSBloomAdd`) tone-maps it into 8 bits. The game's chain is
left as it is; per frame:

* the tone mapper's draw is noted (its lit-scene, adaptation and bloom textures and its constants c100..c102), and
  the particle composite's (the particles' own light);
* at the scene/UI split, after `aa` / `sharpen`, a scene pass works out the extra light over the SDR picture from
  those inputs with the port's math (the curve's upper half, a logarithmic extension, the hue past white, the
  grade's response, the particle tag) into a float target;
* at `EndScene` a mask pass keeps it where the finished frame still shows the graded scene (what the HUD or a menu
  covered or brightened goes out at the HUD's white, `hdr_hud`, 0 = 0.8 x paper white);
* in the `Present` hook, after the settings overlay, an output pass re-encodes the back buffer: SDR picture + extra,
  deeper darks, highlight gain, a shoulder to the peak, paper white, BT.2020, PQ.

With `hdr_ready = true` (the default) or `hdr = true` the device is created (and reset) with an `A2R10G10B10` back
buffer and the hooks are in from the start, so HDR can be switched on and off live; if the runtime refuses the
format, the game's own is used and the picture stays SDR. With HDR off no pass runs: the game's 8-bit picture is
copied into the 10-bit back buffer by its own `StretchRect` and goes out as sRGB, the same codes as on the game's own
back buffer; `aa` / `sharpen` keep their own split, and the HUD and the settings overlay are drawn as without HDR. DXVK's `ID3D9VkExtSwapchain` (queried from the implicit swap chain) switches the
swap chain to HDR10 ST 2084 when the surface offers it: Steam Deck OLED / Bazzite in Game Mode with HDR on for the
display. Not offered (HDR off, an SDR display, not DXVK): no pass runs, the 10-bit back buffer goes out as sRGB, and
it is asked again every two seconds. The rows (HDR, HDR CALIBRATION, PEAK BRIGHTNESS, PAPER WHITE, HUD BRIGHTNESS,
DEEPER DARKS, HIGHLIGHT STRENGTH, LIGHT SOURCES) are on the DISPLAY page of the in-game settings where the swap chain offers HDR10
or `hdr` is on; they act at once (with `hdr_ready = false` and `hdr = false` at start, `hdr` says RESTART NEEDED: the
hooks and the 10-bit back buffer come with the start). The calibration screen shows a disc at the peak setting on a
10000-nit surround and a paper white card beside an 18 % grey one. With `hdr_ready = false` and `hdr = false` nothing
is hooked and the back buffer is the game's own. Since the frame hooks are then always in, `aa` and `sharpen` act at
once as well.

LIGHT SOURCES (`hdr_lift`, 0 to 4, default 3; the port's row): what the game itself knows to be a light goes above
the sunlit whites. Additive draws into the lit scene by shaders that do not read the G-buffer (coronas, glow cards)
are drawn once more into a float target of their own; G-buffer draws of the self-lit materials (a car's lamp lens,
`Emissive_n`, `Emissive_Alpha_n`, `Diffuse_Emissive_n`, `Emissive_Pulse_n` boards, signs and bulbs; found by their
constant names) once more into a mask with a depth buffer of its own, which holds each pixel's own light as the
material writes it into the G-buffer (an emissive texture's alpha included), so a sign keeps the same lift whichever
of its meshes or materials draws it, and a surface the material does not light gets none; the scene pass lifts the
pixels they made, in their own hue. The particles' light (fire, sparks) is lifted as well.
Both tone mappers the game has are covered (`SSFilmicToneMapper`, `SSToneMapper`).

Levers: device vtable slots 37, 81..84, 106 and 107 (`SetRenderTarget`, the draw calls, `CreatePixelShader`,
`SetPixelShader`) in the d3d9 module, the frame hooks `postfx.cpp` already has, `display.cpp`'s `CreateDevice` /
`Reset` wraps; nothing of the game is written. Windows: only with DXVK (`renderer = dxvk`). Plan, measurements, the owner's test steps: `docs/plan-hdr.md`. Log lines `[hdr]`.

`hdr_probe = true` (`src/hdr_probe.cpp`) is the test aid the plan stands on: it lists the device, the swap chain
extension and chosen frames in `splitsecond-hdr-probe.txt`. Off (the default): nothing is started.

### Renderer on Windows (`renderer`; `src/renderer.cpp`)

`renderer = dxvk | native` (default `dxvk`, read at start, overlay row DISPLAY > RENDERER) picks the Direct3D 9 the
game draws with on Windows.

* `dxvk`: DXVK 2.7.1 (Direct3D 9 on Vulkan), shipped in the `dxvk` folder beside the exe. It is the default because
  it holds 120 fps (race average about 119 fps, 1% low about 99 on a Radeon RX 9060 XT) and it is the only way to HDR
  on Windows. The cost: the first time a track is driven, DXVK builds its Vulkan pipelines when a new object or
  effect first appears, and the picture stops while it does. Measured on a first race with an empty driver cache:
  about 25 stops over 40 ms in the first minute, 16 of them 100 ms or longer, the longest about 0.8 s. The graphics
  driver keeps the built pipelines on disk, so the next race on that track is smooth. AMD's cache holds about 1 GB:
  a track not driven for a long time, or a new driver, can stutter once again.
* `native`: Windows' own Direct3D 9. No stops in the race; only a short pause as the race starts (about 0.45 s the
  first time, 0.12 s after that). It does not hold 120: about 110 fps when aiming for 120, 1% low about 70. At
  60 fps it is fine. No HDR.

How: the exe imports `d3d9.dll`, which is now Windows' own. With `dxvk` a 5-byte jump on Windows'
`d3d9!Direct3DCreate9` (system DLL memory, nothing of the game) sends the call to `dxvk\d3d9.dll`; if that DLL is
missing or does not load, Windows' own is used. DXVK still reads `dxvk.conf` from the game folder. A `d3d9.dll` beside
the exe (the older layout) is always used and the key does nothing (the row is then not shown). Under Wine / Proton
nothing changes: Proton's DXVK is always used and the row is not shown. One log line `[renderer]` says which d3d9 is
used and why. Without `splitsecond.conf` the DLL changes nothing, so the game uses Windows' own d3d9.

On Windows' own d3d9 the device's function table lives inside the device, and d3dx9's font and sprite work (the
overlay's first frame) writes it back whole: every slot hook of the patch was lost, so the overlay never showed and
the frame log stopped. The overlay now reads the table before it draws and writes back every slot that changed
(log: `[menu] N device vtable slots were reset ...`, 16 on the first frame).

### DXVK on Windows inside 2 GB (`dxvk_fit`, `vm_log`, `env.NAME`; `src/dxvkwin.cpp`, `src/vm.cpp`)

The exe is 32-bit without the large-address flag: 2 GB of address space on Windows. With DXVK's `d3d9.dll` beside
the exe the first race load ran out of it (DXVK: `Memory allocation failed`, `VK_ERROR_OUT_OF_HOST_MEMORY`, then a
crash or a stuck picture). Measured causes and the fix (`docs/plan-hdr.md` section 17.6):

* Every `IDirect3D9` of DXVK is a Vulkan instance. When the last one is released the Vulkan driver DLL unloads;
  AMD's driver leaves a 128 MB reservation behind each time and reserves 128 MB again at the next
  `Direct3DCreate9`. The start-up did that about three times. `dxvk_fit` makes one `IDirect3D9` at start and keeps
  it, so the driver is loaded once.
* The exe's header gives every thread 5 MB of stack reservation, about 50 threads. With `dxvk_fit` a thread that
  does not start in the game's own code gets 1 MB (one import slot of `kernelbase.dll`, `NtCreateThreadEx`).

`dxvk_fit = auto` (default): on Windows only, and only when the game draws with DXVK (`renderer = dxvk`, or a
`d3d9.dll` beside the exe). `false`: off. Log
lines `[dxvk_fit]`. With it a race runs under DXVK with about 600 MB free (Microsoft's d3d9: about 450 MB).

`vm_log = N` (off by default) writes the address space into the log every N seconds, when the free space drops,
when an allocation fails and at a crash: free space and its largest blocks, totals by kind, and the owners (an
image by file name, a private allocation by the module that asked, heaps, thread stacks). While it is on, five
import slots of `kernelbase.dll` are redirected to learn who asks. `env.NAME = value` lines set environment
variables of the game process before Direct3D starts (`DXVK_HUD`, `VK_LOADER_LAYERS_DISABLE`, ...).

### NVIDIA GPUs: untested

Every run so far was on AMD (Steam Deck, a Bazzite desktop, a Windows PC with a Radeon RX 9060 XT). Nothing in the
patch or in the game is tied to AMD: the hooks sit in the d3d9 module (Windows' own or DXVK's, the same files for every
vendor), the game takes the same depth path on NVIDIA (INTZ), and every picture feature checks what it needs and
switches itself off with a log line when the driver refuses. But no NVIDIA GPU has run the patch yet.

If you try it on NVIDIA, please send `splitsecond-patch.log` (beside the DLL) from one session with a race in it,
with `frame_log = true` and `vm_log = 30` in `splitsecond.conf`. The line `[display] adapter 0: NVIDIA ...` says
which GPU, driver and d3d9 ran. Lines to look for: `could not be made`, `took no variant`, `no readable depth`,
`refused`, `EXCEPTION`.

Known differences and settings:

* DXVK (the default `renderer`) on NVIDIA under Windows: the 2 GB address space with NVIDIA's 32-bit Vulkan driver is
  not measured (AMD ends a race with about 600 MB free). If a race load crashes or the picture sticks, send the log
  with `vm_log = 30` and use `renderer = native` meanwhile.
* `renderer = native` (Windows' own d3d9 on NVIDIA's D3D9 driver): no HDR (as on every vendor); 120 fps not measured.
* NVIDIA Control Panel / NVIDIA App: leave the game's 3D settings at their defaults. Forced anti-aliasing, FXAA,
  forced V-Sync (Off, Fast, Adaptive) or a Max Frame Rate for the game work against `aa` and `frame_pacing`. With
  `hdr = true` (DXVK) turn RTX HDR and RTX Dynamic Vibrance off for the game.
* `frame_pacing = vrr` with G-Sync: the race rate cap is the same as NVIDIA's own (116 at 120 Hz), so Low Latency Mode
  Ultra or Reflex does not hold frames back. In a borderless or windowed game, G-Sync works only with "Enable G-SYNC
  for windowed and full screen mode" in the control panel; with DXVK also leave "Vulkan/OpenGL present method" at Auto
  or "Prefer layered on DXGI swapchain". Check the monitor's own refresh readout: in a race it should show the capped
  rate, not the maximum.
* Linux (Proton) on NVIDIA's proprietary driver: needs the 32-bit Vulkan driver and a driver recent enough for DXVK 2.x.

### Pre-compile (`pipeline_warmup`; `src/pipewarm.cpp`)

Under DXVK the first race on a track stutters. When a shader meets a new set of states (blend, vertex layout, target
formats, ...) for the first time, DXVK builds a Vulkan pipeline for it and the frame waits (`[frames] real Present
blocked N ms` with `frame_log = true`; 40-1700 ms each). The graphics driver keeps the built pipelines in its own
disk cache (AMD on Windows: `%LOCALAPPDATA%\AMD\VkCache`), so later races on that track are smooth until the cache
drops them.

Pre-compile builds those pipelines while the track loads, so the first race does not stutter.

**In the menu** (GRAPHICS page, shown only under DXVK and when the record files are there):

* **PRE-COMPILE** `OFF / ON`. ON (the default when the record files are there): at each track's load the patch draws
  every recorded state once, off screen. The first load of a track takes about 30 s instead of 5 s, once per
  driver cache; later loads take about 5 s. Takes effect at the next track load, no restart.
* **PRE-COMPILE ALL TRACKS**: in the main menu, press A / Enter to build every track now (a few minutes; press again
  to stop). The row shows the progress: `READING GAME FILES`, `MAKING SHADERS`, `TRACK 2/5 431/1124`, `DONE`. It
  works on a fresh save with locked tracks: no track is loaded, nothing is unlocked, the save is not touched. It reads
  the shaders from your own game files (`data\*.ark`). It stops by itself
  when you leave the main menu or when free memory (32-bit address space) runs low.

**Compiler threads.** With pre-compile on, the patch tells DXVK to use 4 compiler threads
(`DXVK_CONFIG = dxvk.numCompilerThreads = 4`, added to any `DXVK_CONFIG` you have). With DXVK's default (one per CPU
thread) a cold pre-compile left only about 250 MB of the game's 32-bit address space free; with 4 about 440 MB, at the
same load time. If you set `numCompilerThreads` yourself (in `DXVK_CONFIG`, `DXVK_CONFIG_FILE` or a `dxvk.conf` beside
the game), the patch keeps yours. DXVK reads this once at start: switching PRE-COMPILE on in the menu gets the 4
threads only from the next start. The log says what was applied (`[pipewarm] pre-compile on: DXVK_CONFIG = ...`).

**Where the records are.** Beside `dinput8.dll`:

* `pipeline-warmup\<level>.bin`: the records that ship with the patch (the 5 levels of the 11 tracks:
  airport_test_03 902, docks 1124, downtown 1058, graveyard 1015, powerplant 950 combinations, 2.4 MB). An update of
  the patch replaces them.
* `pipeline-warmup\local\<level>.bin`: your own records, written only in `record` mode. The patch reads both and
  uses each combination once; `record` writes only what neither file has.

A record holds shader hashes and pipeline states only (about 0.5 KB a combination), never the game's shader code.
`make dist` checks this (`tools/pipewarm/merge.py --check`) and puts the shipped set into the zip
(`tools/dist/pipewarm-records.sh`, from `records/pipeline-warmup/` in this repository).

**`pipeline_warmup` in splitsecond.conf** (`SS_PIPELINE_WARMUP`): empty or `auto` = on when the record files are
there; `on` (`replay`) = pre-compile at each load; `off`; for developers: `record` = append every new combination
drawn in a level to `pipeline-warmup\local\<level>.bin`, `both` = pre-compile and record what it missed.
`SS_PIPEWARM_SLICE_MS` (default 40) is the time per frame the replay may use.

How the replay works: the game's own shaders that a record names are kept when the game makes them during the load;
once 95% are there (or 1.5 s after the last one, at most 20 s; at the latest when the load ends) every recorded
combination is drawn once into 16x16 targets, in slices of 40 ms per frame. Variants of game shaders that this patch
makes (`lod_smoothing`, `shadow_smoothing`, `deband`) are named in the record by the game shader they come from and
are made with the settings of the day. PRE-COMPILE ALL TRACKS does the same for every level at once: it finds the
shaders the records name in the game's archives (`src/arkscan.cpp`: the archive table, LZSS, the D3D9 programs in
the `.fxc` shader members, hashed as the records hash them), makes them on the device, and replays each level. Log
lines `[pipewarm] pre-compile all: ...` give the archives read, the shaders found and made, each track and the free
address space.

Measured on the Windows PC (AMD GPU, DXVK 2.7.1, the game's own AI driving with `autopilot = on`; cold = empty AMD
driver cache):

| Airport Terminal | load | Present blocks > 40 ms | >= 100 ms | worst | lowest free address space |
|---|---|---|---|---|---|
| cold, off | 5.3 s | 28 | 16 | 1377 ms | 395 MB |
| cold, pre-compile | 27.1 s | 2 | 0 | 69 ms | 251 MB |
| cold, pre-compile, `numCompilerThreads = 2` | 28.1 s | 3 | 0 | 61 ms | 440 MB |
| cold, pre-compile, `numCompilerThreads = 4` | 28.6 s | 2 | 0 | 62 ms | 437 MB |
| warm (second start), pre-compile | 4.7 s | 0 | 0 | - | 452 MB |

Ferry Wharf, cold: off 15 blocks > 40 ms, 10 >= 100 ms, worst 1709 ms (load 5.6 s); pre-compile 3 > 40 ms, none
>= 100 ms (load 33.4 s); warm: load 4.9 s, no blocks. Caveat found later: in both "off" runs the autopilot never got
past the pre-race card (the Return keys came before the card took input), so their blocks are from the race intro
and the card, not from a driven race; the off runs need repeating. In-race frame rate with pre-compile, from the
autopilot taking over to the finish: 119.4-119.6 fps at the 120 cap on both tracks, cold and warm (the lower
"average fps" in earlier notes mixed in the results screens after the finish).

Not yet measured: PRE-COMPILE ALL TRACKS on the real game files (how many of the needed shaders are found, the time,
the free address space after all 5 tracks).

`tools/pipewarm/merge.py OUT IN...` merges the files of several folders per level; `merge.py --list DIR` lists them.

### Save backups (`save_backups`; `src/saves.cpp`)

At every start (on its own thread, before the game reads its profile) the DLL copies the files of the game's save
folder, `Documents\Disney Interactive Studios\Split Second\` (the `*.sav` profile, `config_ss.cfg`; not
`steam_autocloud.vdf`), into a new folder `save-backups\YYYY-MM-DD_HHMMSS\` in the game's folder, next to
`dinput8.dll`. They are not put into Documents, because Steam Auto-Cloud syncs that folder. When the saves are the
same bytes as in the newest backup, no new folder is made. The newest `save_backups` folders are kept (default 10,
0-50; overlay GAME > SAVE BACKUPS, read at start); older ones are deleted, and only folders with exactly that
name pattern. `save_backups = 0` turns it off. Log lines `[saves]`: `backed up N files to ...`, `N files unchanged
since ..., no new backup`, `save_backups = 0: off`, or why nothing was copied (no save folder yet on a first start).

Where the save folder is: on Windows `C:\Users\<you>\Documents\Disney Interactive Studios\Split Second\`; under
Proton (Linux, Steam Deck) `~/.local/share/Steam/steamapps/compatdata/297860/pfx/drive_c/users/steamuser/Documents/Disney Interactive Studios/Split Second/`.

To restore a backup: close the game, copy the files from the chosen `save-backups\<date>\` folder back into the
save folder (replace the files there), and start the game. If Steam then asks about a cloud conflict, choose the
local files. The next start makes a new backup of the restored files.

### DLC test overlay (`overlay_dir`, `dlc_tracks`, `dlc_save_profile`; `src/overlay.cpp`): experimental

For the DLC tests of SSPC-12 only (`docs/dlc-overlay.md`). `overlay_dir = <folder>` makes the game read a file from
that folder when the folder holds a file with the same path relative to the game folder (for example a rebuilt
`data\resident.ark`). The game's own files are never changed; remove the key and the stock game is back. The test
folders are built from your own install by `tools/dlc/overlay_car_gate1.py` and `tools/dlc/overlay_clone_track.py`
(no game data ships with the patch). `dlc_tracks = true` adds the track ids that the overlay's `dlc-tracks.txt`
declares, and then the game keeps its save in a separate folder (`<Documents>\Split Second DLC profile\`) unless
`dlc_save_profile = off`. Log lines `[overlay]`: the files the folder holds, the hooked import slots, each redirected
file once, the track ids added.

## Online (`src/online.cpp`, `tools/online/`)

The game's ONLINE menu is a Quazal Rendez-Vous client (Net-Z / Rendez-Vous 2008.9 SP1) for a backend that is gone
(`splitsecpclive.quazal.net`, UDP 31120). `docs/plan-online.md` is the plan for our own server, `tools/online/rdvd`. The
parts:

* `online_server = host[:port]` (no line or an empty value: the project's server `vps4954.rackhostvps.com`, so an
  updater's older conf is online too; `off`/`false`/`0`/`no`/`none`: nothing is hooked, no thread is started). Every
  `*.quazal.net` lookup is answered with that server's address. A host name (the default is one) is resolved once at
  start on the worker thread (`getaddrinfo`, IPv4 only) and the result is logged
  (`[online] online_server name resolves to address`). If it does not resolve, the log line says so and the hook asks
  the system again at each game lookup. There is no built-in address to fall back to. The redirection is a 5-byte jump
  on the entry of `ws2_32.dll`'s exported `gethostbyname` (the system DLL's memory; the game's code and import table
  are not written), put in once at start. If the entry is not one the DLL knows how to move, the fallback replaces the
  host name string in the game's read-only data (0xC4BD24) with the dotted address through `ss_write_word`; this needs
  the start-time resolution to have worked. Each lookup of a game server name is logged
  (`[online] gethostbyname(name) -> address`); any other name (the game asks for the PC's own at start) is logged as
  `<this PC's name>` or `<another name>` with the kind of address only (loopback, LAN, link-local, carrier-NAT,
  public). The port is an immediate in the game's code and stays 31120. The shipped `splitsecond.conf` has
  `online_server =` (empty), so the server's host name lives only in `kOnlineDefaultServer` (`src/online_conf.h`)
  and in the unit's `--public-address`.
* Automatic sign-in name (`src/online_login.cpp`, `src/online_text.h`): `online_name`, else the Steam persona name,
  folded to ASCII (UTF-8, or Latin-1 for a byte that is not UTF-8; U+00C0..U+017F to the plain letter, Cyrillic
  to Latin letters, anything else dropped), then cut to the server's rule (letters, digits, space `.` `_` `-`, 12
  characters) and given the 3-character tag. A name with nothing left signs in as `Racer` plus the tag, and the log
  line names the fallback.
* `tools/online/rdv_stub.py` (Python, standard library): logs every datagram, decodes PRUDP v0 / RMC, proves the
  checksum against the access key, acknowledges; with `--answer --unknown retval --secure-port 31121` it plays
  the login half of the backend (tickets, secure connect, Register, account creation through the guest login), far
  enough for the game to create an account, sign in and open its Online menu. It keeps nothing on disk, has no
  limits of any kind and is not a server: never expose it to a network.
* `tools/online/rdvd` is the server built from that (see `tools/online/README.md`). It runs on
  `vps4954.rackhostvps.com` since 2026-11-11. Tested on 2026-11-14 with three games on the 1.0 DLL: sign-in and
  account creation, a public (ranked) lobby created and joined, a race started and finished by all players, the
  results and credits in the lobby afterwards. rdvd also writes the public statistics page at `/backonair/`.
  Limits of 1.0: races have run only between PCs on one home network (peer to peer; between different home routers
  untested, and the DLL has no tunnel to the relay port yet, plan section 4); private games with invitations are
  refused and the friends list is empty, so a race needs 3 players in a public lobby; no leaderboards.
