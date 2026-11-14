# tools/audio

Measuring the PC release's sound against the console version, per speaker, without anybody listening.

| file | what |
|---|---|
| `ab_capture.sh` | on the Linux test machine: one headless run of the PC release (with a `dinput8.dll` of this repository) or of the console port, the sound into a PipeWire null sink of its own (5.1, 7.1 or stereo), that sink's monitor recorded |
| `ab_analyze.py` | on any machine with Python and numpy: two recordings side by side, PC minus console, per channel group |
| `fev_compare.py` | the event files of both releases compared (event values, mixer data, sample levels); writes `src/console_mix_table.h` |

## Recording

`ab_capture.sh` runs on the test machine itself (gamescope, `pactl`, `parecord`, Proton). It starts the game in a
nested headless gamescope, so nothing appears on the machine's screen, and it refuses every sink that is not a null
sink (`ab_null*`, `harness_null*`), so nothing is played on the machine's speakers. The null sink is made when it is
missing. The game is the only thing playing into it, so the recording of its monitor is the game's own stream, in the
layout the game was offered: `LAYOUT=51` (default), `71` or `20`.

    # the PC release: a private copy of the install and a Proton prefix of its own, never the Steam install
    PC_MIRROR=/data/ss/mirror/SplitSecond PC_COMPAT=/data/ss/compat/297860 \
    PC_DLL=/data/ss/dinput8.dll PROTON="$HOME/.local/share/Steam/steamapps/common/Proton 11.0/proton" \
    KEYS='25:Return,28:Right,30:Return,38:Return,46:Return,56:Right,58:Return,68:Return,98:Return,113:Return,163:+Up' \
    OUT=/data/ss/ab sh ab_capture.sh pc pc51 265 /data/ss/diag.conf

    # the console port (its own tools/linux/run-nested.sh and a pad recording that drives the menus)
    PORT_ROOT=/data/port OUT=/data/ss/ab sh ab_capture.sh console rx51 265 /data/port/tools/reach/menu-to-race.pad

    # the same on a 7.1 or a stereo device
    LAYOUT=71 ... sh ab_capture.sh pc pc71 265 /data/ss/diag.conf

`KEYS` is the patch's own key route (`SS_PATCH_KEYS`, seconds:key) from the title screen into a race; the conf file
is an ordinary `splitsecond.conf` (with `audio_diag = true` the log carries the `[start]` lines the analysis sorts by
category). Further `VAR=value` arguments reach the game's environment (`SS_AUDIO_SPEAKER_FIX=0`, `SS_REVERB=0`,
`SS_AUDIO_REVERB_TEST=mute` ...), which is how one switch is measured against another.

Results in `$OUT/<tag>/`: `cap.wav` (float32, 48 kHz), `cap.start` (wall clock of the first sample),
`splitsecond-patch.log` (PC), `inputs.txt` (which stream sat on which sink 25 s into the recording). Look at
`inputs.txt` once per set-up: the game's stream has to be on the null sink, with the channel count asked for. A
recording of digital silence means the game never opened a stream (the console port prints its audio device in its
log at debug level).

On a machine shared with other work, take that machine's lock around the run and say how long it takes.

Recordings made on a real system work as well: `pw-record --target <stream serial> --channels 6 --format f32
--rate 48000 out.wav` on the game's output stream (the serial from `pw-dump`, `media.class` `Stream/Output/Audio`).

## Comparing

    python3 ab_analyze.py --pc ab/pc51/cap.wav --pc-log ab/pc51/splitsecond-patch.log --pc-start ab/pc51/cap.start \
                          --console ab/rx51/cap.wav [--from 60 --to 200]

It prints, for each recording and as PC minus console:

* **levels**: RMS per channel group (front pair, centre, LFE, rears, sides) in dBFS and against the front pair;
* **floor**: the 1 %, 5 % and median of each group against the front pair over the 100 ms windows in which the front
  pair is loud. Whatever is always in a speaker (a reverb return in all five, an engine send) is a floor there;
* **transients**: the windows that jump 9 dB over the last two seconds (impacts, power plays): count, mean jump, and
  where they sit (rear, centre, LFE against front at the peak);
* **tails**: the 0.2-0.8 s and 0.8-2.0 s after each transient against its peak, per group: the reverb energy and
  where it comes out, with the correlation of centre and rear-left there (a mono return in every speaker is near 1);
* **categories** (PC, with the log of an `audio_diag = true` run): per event category, how many sounds started in
  the recording and where the 0.3 s after a start sits.

The two recordings need not be the same drive; the figures are statistics. They do need the same kind of content:
cut both to the race with `--from` / `--to` (menus and loading screens have their own reverb and no engines), use
the same track, and for one particular place (a tunnel, the airport's hangar) record both there.

What the figures mean for the patch's switches is in the main README ("Audio").
