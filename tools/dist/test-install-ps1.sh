#!/bin/sh
# test-install-ps1.sh WORKDIR - runs install.ps1 under pwsh (macOS/Linux) against a fake Steam tree and checks the
# results. The registry part (HKCU SteamPath) is replaced by SPLITSECOND_STEAM_PATH. Exit status 0 = all passed.
set -u
SRC=$(cd "$(dirname "$0")" && pwd)
W=${1:?usage: test-install-ps1.sh WORKDIR}
rm -rf "$W"; mkdir -p "$W"; W=$(cd "$W" && pwd -P)
ROOT="$W/Steam"; LIB="$W/Library [2]"; GAME="$LIB/steamapps/common/SplitSecond"; PKG="$W/pkg"
FAIL=0
ok()   { printf 'ok    %s\n' "$*"; }
bad()  { printf 'FAIL  %s\n' "$*"; FAIL=1; }
check() { d=$1; shift; if "$@"; then ok "$d"; else bad "$d"; fi; }
run()  { (cd "$W" && SPLITSECOND_STEAM_PATH="$ROOT" pwsh -NoProfile -NonInteractive -File "$PKG/install.ps1" "$@") > "$W/out.txt" 2>&1; echo $? > "$W/rc.txt"; }
rc()   { [ "$(cat "$W/rc.txt")" = "$1" ]; }
said() { grep -q -- "$1" "$W/out.txt"; }
nbk()  { ls -1 "$GAME/patch-backup" 2>/dev/null | wc -l | tr -d ' '; }

mkdir -p "$ROOT/steamapps" "$GAME" "$PKG/dxvk"
printf '"libraryfolders"\n{\n\t"0"\n\t{\n\t\t"path"\t\t"%s"\n\t}\n\t"1"\n\t{\n\t\t"path"\t\t"%s"\n\t}\n}\n' "$ROOT" "$LIB" > "$ROOT/steamapps/libraryfolders.vdf"
printf '"AppState"\n{\n\t"appid"\t\t"297860"\n\t"installdir"\t\t"SplitSecond"\n}\n' > "$LIB/steamapps/appmanifest_297860.acf"
echo exe > "$GAME/SplitSecond.exe"; echo game-data > "$GAME/fmodex.dll"
cp "$SRC/install.ps1" "$PKG/"
printf 'MZ patch v1 splitsecond.conf\n' > "$PKG/dinput8.dll"
printf 'fps = 60\n' > "$PKG/splitsecond.conf"
printf 'DXVK 2.7.1 x32\n' > "$PKG/dxvk/d3d9.dll"; printf 'zlib\n' > "$PKG/dxvk/LICENSE"
printf 'Split/Second PC patch: DXVK\n' > "$PKG/dxvk/README.txt"
mkdir -p "$PKG/pipeline-warmup"; printf 'rec a v1\n' > "$PKG/pipeline-warmup/docks.bin"; printf 'rec b\n' > "$PKG/pipeline-warmup/airport_test_03.bin"

echo "== 1. fresh install (library with brackets and a space in its path)"
run
check "exit 0" rc 0
check "dinput8.dll copied" cmp -s "$PKG/dinput8.dll" "$GAME/dinput8.dll"
check "conf copied" cmp -s "$PKG/splitsecond.conf" "$GAME/splitsecond.conf"
check "dxvk copied" cmp -s "$PKG/dxvk/d3d9.dll" "$GAME/dxvk/d3d9.dll"
check "dxvk licence copied" [ -f "$GAME/dxvk/LICENSE" ]
check "records copied" cmp -s "$PKG/pipeline-warmup/docks.bin" "$GAME/pipeline-warmup/docks.bin"
check "no backup folder" [ "$(nbk)" = 0 ]

echo "== 2. run twice: nothing changes"
run
check "exit 0" rc 0
check "says already" said "already this version"
check "no backup folder" [ "$(nbk)" = 0 ]

echo "== 3. update: new dll and dxvk, the player's conf is kept"
printf 'MZ patch v2 splitsecond.conf\n' > "$PKG/dinput8.dll"; printf 'DXVK 2.7.2 x32\n' > "$PKG/dxvk/d3d9.dll"
printf 'fps = 120\n' > "$GAME/splitsecond.conf"
run
check "exit 0" rc 0
check "new dll" cmp -s "$PKG/dinput8.dll" "$GAME/dinput8.dll"
check "new dxvk" cmp -s "$PKG/dxvk/d3d9.dll" "$GAME/dxvk/d3d9.dll"
check "old ones backed up as patch" sh -c "grep -q -x 'patch dxvk' '$GAME'/patch-backup/*/backup.txt && grep -q -x 'patch dinput8.dll' '$GAME'/patch-backup/*/backup.txt"
check "player conf kept" grep -q 'fps = 120' "$GAME/splitsecond.conf"
check "conf.new written" cmp -s "$PKG/splitsecond.conf" "$GAME/splitsecond.conf.new"

echo "== 3b. new records; the player's own records in pipeline-warmup/local stay"
mkdir -p "$GAME/pipeline-warmup/local"; printf 'mine\n' > "$GAME/pipeline-warmup/local/docks.bin"
printf 'rec a v2\n' > "$PKG/pipeline-warmup/docks.bin"
run
check "exit 0" rc 0
check "new record" cmp -s "$PKG/pipeline-warmup/docks.bin" "$GAME/pipeline-warmup/docks.bin"
check "says 1 file" said "pipeline-warmup, 1 files"
check "local record kept" grep -q mine "$GAME/pipeline-warmup/local/docks.bin"

echo "== 4. old layout d3d9.dll, a foreign dxvk folder and a foreign dinput8.dll"
sleep 1
printf 'old DXVK\n' > "$GAME/d3d9.dll"; printf 'MZ other\n' > "$GAME/dinput8.dll"
rm -rf "$GAME/dxvk"; mkdir "$GAME/dxvk"; printf 'mine\n' > "$GAME/dxvk/d3d9.dll"
run
check "exit 0" rc 0
check "d3d9.dll moved away" [ ! -e "$GAME/d3d9.dll" ]
check "all three foreign" sh -c "for n in d3d9.dll dxvk dinput8.dll; do grep -q -x \"foreign \$n\" '$GAME'/patch-backup/*/backup.txt || exit 1; done"
check "our dxvk in place" cmp -s "$PKG/dxvk/d3d9.dll" "$GAME/dxvk/d3d9.dll"

echo "== 5. HFR mod refused"
echo x > "$GAME/DXFreezerServer.dll"; run
check "exit 1" rc 1
check "names it" said "HFR mod"
rm "$GAME/DXFreezerServer.dll"

echo "== 6. uninstall"
mkdir -p "$GAME/save-backups"; echo s > "$GAME/save-backups/x"
sleep 1
run /uninstall
check "exit 0" rc 0
check "foreign dinput8.dll back" grep -q 'MZ other' "$GAME/dinput8.dll"
check "old d3d9.dll back" grep -q 'old DXVK' "$GAME/d3d9.dll"
check "foreign dxvk back" grep -q mine "$GAME/dxvk/d3d9.dll"
check "conf moved out" [ ! -e "$GAME/splitsecond.conf" ]
check "records moved out" sh -c "[ ! -e '$GAME/pipeline-warmup' ] && ls '$GAME'/patch-backup/*-uninstall/pipeline-warmup/docks.bin >/dev/null"
check "save-backups untouched" [ -f "$GAME/save-backups/x" ]
check "game file untouched" grep -q game-data "$GAME/fmodex.dll"

echo "== 7. game not found, no folder given (non-interactive)"
run /gamedir "$W/nowhere"
check "exit 1" rc 1
check "says exe missing" said "SplitSecond.exe is not in"

if [ $FAIL = 0 ]; then echo "ALL PASSED"; else echo "SOME CHECKS FAILED (last output: $W/out.txt)"; fi
exit $FAIL
