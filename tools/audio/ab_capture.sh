#!/bin/sh
# ab_capture.sh: one headless run of the PC release (with this patch) or of the console port, its sound sent to a
# PipeWire null sink of its own and that sink's monitor recorded. Runs ON the Linux test machine (gamescope, pactl,
# parecord, Proton). Nothing is shown on the machine's screen and nothing is played on its speakers: the script
# refuses any sink whose name does not start with "ab_null" or "harness_null".
#
#   ab_capture.sh pc      <tag> <seconds> <conf file> [VAR=value ...]   the PC release under Proton
#   ab_capture.sh console <tag> <seconds> <pad file>  [VAR=value ...]   the console port (its run-nested.sh)
#
# Environment:
#   LAYOUT=51|71|20      channel layout of the null sink and of the recording (default 51)
#   OUT=<dir>            results go to $OUT/<tag>/ (default ./ab-out): cap.wav (float32, 48 kHz), cap.start (wall
#                        clock of the first recorded sample, to line the recording up with splitsecond-patch.log),
#                        inputs.txt (what was connected to which sink while the game ran), the game's logs
#   REC_FROM=<s>         start recording this many seconds after the start (default 15)
#   pc:      PC_MIRROR=<dir with SplitSecond.exe; a private copy, never the Steam install>
#            PC_COMPAT=<a Proton prefix directory for it (STEAM_COMPAT_DATA_PATH)>
#            PC_DLL=<the dinput8.dll to test>  PROTON=<path of the proton script>
#            KEYS=<SS_PATCH_KEYS route: "seconds:Key,..."> (the menu route to a race)
#   console: PORT_ROOT=<the console port's checkout with tools/linux/run-nested.sh and a build>
#
# The analysis: tools/audio/ab_analyze.py (tools/audio/README.md).
set -eu
mode=${1:?usage: ab_capture.sh pc|console <tag> <seconds> <conf|pad> [VAR=value ...]}
tag=${2:?tag}; secs=${3:?seconds}; arg=${4:?conf file (pc) or pad file (console)}
shift 4
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
case "${LAYOUT:-51}" in
    20) nch=2; map=front-left,front-right ;;
    51) nch=6; map=front-left,front-right,front-center,lfe,rear-left,rear-right ;;
    71) nch=8; map=front-left,front-right,front-center,lfe,rear-left,rear-right,side-left,side-right ;;
    *) echo "LAYOUT must be 20, 51 or 71" >&2; exit 2 ;;
esac
sink=${SINK:-ab_null${LAYOUT:-51}}
case "$sink" in ab_null*|harness_null*) ;; *) echo "refusing sink $sink: only a null sink" >&2; exit 2 ;; esac
pactl list short sinks | grep -q "	$sink	" ||
    pactl load-module module-null-sink sink_name="$sink" channels=$nch channel_map=$map >/dev/null
pactl list short sinks | grep -q "	$sink	" || { echo "no sink $sink" >&2; exit 1; }
out=${OUT:-$(pwd)/ab-out}/$tag
rm -rf "$out"; mkdir -p "$out"
from=${REC_FROM:-15}

( sleep "$from"; date +%H:%M:%S.%N > "$out/cap.start"
  timeout $((secs - from)) parecord --device="$sink.monitor" --channels=$nch --channel-map=$map \
      --format=float32le --rate=48000 --file-format=wav "$out/cap.wav" ) &
( sleep $((from + 25)); { pactl list sink-inputs | grep -E 'Sink Input|Sink:|Sample Spec|Channel Map|application.name'
  pactl list short sinks; } > "$out/inputs.txt" ) &

case "$mode" in
pc)
    : "${PC_MIRROR:?PC_MIRROR}" "${PC_COMPAT:?PC_COMPAT}" "${PC_DLL:?PC_DLL}" "${PROTON:?PROTON}"
    case "$PC_MIRROR" in *steamapps/common*) echo "PC_MIRROR must be a private copy, not the Steam install" >&2; exit 2 ;; esac
    cp "$PC_DLL" "$PC_MIRROR/dinput8.dll"
    cp "$arg" "$PC_MIRROR/splitsecond.conf"
    rm -f "$PC_MIRROR/splitsecond-patch.log"
    cat > "$out/inner.sh" <<IN
#!/bin/sh
export STEAM_COMPAT_DATA_PATH="$PC_COMPAT" STEAM_COMPAT_CLIENT_INSTALL_PATH="\$HOME/.local/share/Steam"
export PULSE_SINK=$sink SteamAppId=297860 SteamGameId=297860 ENABLE_GAMESCOPE_WSI=0 DXVK_HDR=0 DISABLE_LSFG=1
export PROTON_LOG=1 PROTON_LOG_DIR="$out" DXVK_LOG_PATH="$out" WINEDEBUG=\${WINEDEBUG:--all}
export WINEDLLOVERRIDES="dinput8=n,b" SS_PATCH_KEYS="${KEYS:-}" SS_PATCH_LOG=1 $*
cd "$PC_MIRROR"
timeout -k 10 $secs "$PROTON" run "$PC_MIRROR/SplitSecond.exe" > "$out/proton.out" 2>&1
echo "exit=\$?" >> "$out/proton.out"
WINEPREFIX="$PC_COMPAT/pfx" "\$(dirname "$PROTON")/files/bin/wineserver" -k
IN
    chmod +x "$out/inner.sh"
    gamescope --backend headless -W 1280 -H 800 -w 1280 -h 800 -- "$out/inner.sh" > "$out/gamescope.out" 2>&1 &
    g=$!
    ( sleep $((secs + 40)); kill $g 2>/dev/null ) & kw=$!
    wait $g || true
    kill $kw 2>/dev/null || true
    cp "$PC_MIRROR/splitsecond-patch.log" "$out/" 2>/dev/null || true
    ;;
console)
    : "${PORT_ROOT:?PORT_ROOT}"
    # SDL has to go through PulseAudio, where PULSE_SINK is honoured; PIPEWIRE_NODE covers its native PipeWire driver.
    export PULSE_SINK=$sink PIPEWIRE_NODE=$sink SDL_AUDIODRIVER=pulseaudio SDL_AUDIO_DRIVER=pulseaudio
    for kv in "$@"; do export "$kv"; done
    ( cd "$PORT_ROOT" && RUNDIR="$out" GPU=${GPU:-plume} PAD="$arg" LOGFLAGS=${LOGFLAGS:---log_level=info} \
        timeout $((secs + 60)) sh tools/linux/run-nested.sh "$tag" "$secs" > "$out/run.out" 2>&1 ) || true
    ;;
*) echo "mode must be pc or console" >&2; exit 2 ;;
esac
wait
echo "== $out"; ls -la "$out"
grep -B1 -A3 "Sink: " "$out/inputs.txt" 2>/dev/null | head -40 || true
echo "check inputs.txt: the game's stream has to be on $sink (and on nothing else)"
