#!/bin/sh
# test-install.sh WORKDIR - runs install.sh and uninstall.sh against a fake Steam tree (HOME=WORKDIR/home) and checks the results.
# Nothing outside WORKDIR is touched. Exit status 0 = all checks passed.
set -u
SRC=$(cd "$(dirname "$0")" && pwd)
W=${1:?usage: test-install.sh WORKDIR}
SHELL_UNDER_TEST=${SH:-sh}
rm -rf "$W"; mkdir -p "$W"
W=$(cd "$W" && pwd -P)
H="$W/home"; ROOT="$H/.local/share/Steam"; LIB="$W/media/SD Card"
GAME="$LIB/steamapps/common/SplitSecond"; PFX="$LIB/steamapps/compatdata/297860/pfx"
PKG="$W/pkg"
FAIL=0
ok()   { printf 'ok    %s\n' "$*"; }
bad()  { printf 'FAIL  %s\n' "$*"; FAIL=1; }
check() { d=$1; shift; if "$@"; then ok "$d"; else bad "$d"; fi; }
run()  { (cd "$W" && HOME="$H" $SHELL_UNDER_TEST "$PKG/install.sh" "$@") > "$W/out.txt" 2>&1; echo $? > "$W/rc.txt"; }
# unrun: uninstall.sh by a relative path from another folder (WORKDIR), started with sh like the README says
unrun() { (cd "$W" && HOME="$H" $SHELL_UNDER_TEST pkg/uninstall.sh "$@") > "$W/out.txt" 2>&1; echo $? > "$W/rc.txt"; }
# state: the game folder (without patch-backup/) and user.reg, section time stamps left out
state() { (cd "$GAME" && find . -path ./patch-backup -prune -o -type f -print | LC_ALL=C sort | while read -r f; do cksum "$f"; done
          sed -e 's/\] [0-9][0-9]*$/]/' -e '/^#time=/d' "$PFX/user.reg"); }
rc()   { [ "$(cat "$W/rc.txt")" = "$1" ]; }
said() { grep -q -- "$1" "$W/out.txt"; }
nbk()  { ls -1 "$GAME/patch-backup" 2>/dev/null | wc -l | tr -d ' '; }

mkdir -p "$ROOT/steamapps" "$GAME" "$PKG"
cat > "$ROOT/steamapps/libraryfolders.vdf" <<EOF
"libraryfolders"
{
	"0"
	{
		"path"		"$ROOT"
	}
	"1"
	{
		"path"		"$LIB"
		"apps"
		{
			"297860"		"123"
		}
	}
}
EOF
printf '"AppState"\n{\n\t"appid"\t\t"297860"\n\t"name"\t\t"Split/Second"\n\t"installdir"\t\t"SplitSecond"\n}\n' > "$LIB/steamapps/appmanifest_297860.acf"
echo exe > "$GAME/SplitSecond.exe"
echo game-data > "$GAME/fmodex.dll"
cp "$SRC/install.sh" "$SRC/uninstall.sh" "$PKG/"
printf 'MZ patch v1 splitsecond.conf\n' > "$PKG/dinput8.dll"
printf 'fps = 60\n' > "$PKG/splitsecond.conf"
mkdir -p "$PKG/pipeline-warmup"; printf 'rec a\n' > "$PKG/pipeline-warmup/docks.bin"

echo "== 1. fresh install, no Proton prefix yet"
run
check "exit 0" rc 0
check "dinput8.dll copied" cmp -s "$PKG/dinput8.dll" "$GAME/dinput8.dll"
check "conf copied" cmp -s "$PKG/splitsecond.conf" "$GAME/splitsecond.conf"
check "records copied" cmp -s "$PKG/pipeline-warmup/docks.bin" "$GAME/pipeline-warmup/docks.bin"
check "asks to start the game once" said "has never been started"
check "prints the launch option" said 'WINEDLLOVERRIDES="dinput8=n,b" %command%'
check "no backup folder" [ "$(nbk)" = 0 ]

echo "== 2. prefix exists now: the override is written"
mkdir -p "$PFX"
cat > "$PFX/user.reg" <<'EOF'
WINE REGISTRY Version 2
;; All keys relative to REGISTRY\\User\\S-1-5-21-0-0-1000-1000

#arch=win32

[Control Panel\\Desktop] 1765484782
#time=1dc6adc6a5a406c
"FontSmoothing"="2"

[Software\\Wine\\DllOverrides] 1765484782
#time=1dc6adc6a5a406c
"api-ms-win-crt-conio-l1-1-0"="native,builtin"
"d3d9"="native"
EOF
cp "$PFX/user.reg" "$W/user.reg.orig"
run
check "exit 0" rc 0
check "override line present" grep -q -x '"dinput8"="native,builtin"' "$PFX/user.reg"
check "section header present" grep -q '^\[Software\\\\Wine\\\\AppDefaults\\\\SplitSecond.exe\\\\DllOverrides\] [0-9][0-9]*$' "$PFX/user.reg"
check "#time line follows the header" sh -c "grep -A1 'AppDefaults' '$PFX/user.reg' | grep -q '^#time=[0-9a-f]*\$'"
check "original text unchanged before the new section" sh -c "head -n $(wc -l < "$W/user.reg.orig") '$PFX/user.reg' | cmp -s - '$W/user.reg.orig'"
check "user.reg backed up" sh -c "ls '$GAME'/patch-backup/*/user.reg >/dev/null 2>&1"
check "dll already this version" said "already this version"
check "one backup folder (the user.reg copy)" [ "$(nbk)" = 1 ]

echo "== 3. run twice: nothing changes"
cp "$PFX/user.reg" "$W/user.reg.2"
run
check "exit 0" rc 0
check "user.reg untouched" cmp -s "$PFX/user.reg" "$W/user.reg.2"
check "says already set" said "already set"
check "records already this version" said "pre-compile records are already this version"
check "still one backup folder" [ "$(nbk)" = 1 ]
check "exactly one dinput8 line" [ "$(grep -c '^"dinput8"=' "$PFX/user.reg")" = 1 ]

echo "== 4. update: new dll, the player's own conf is kept"
printf 'MZ patch v2 splitsecond.conf\n' > "$PKG/dinput8.dll"
printf 'fps = 120\n' > "$GAME/splitsecond.conf"
sleep 1
run
check "exit 0" rc 0
check "new dll in place" cmp -s "$PKG/dinput8.dll" "$GAME/dinput8.dll"
check "old dll in backup as patch" sh -c "grep -l -x 'patch dinput8.dll' '$GAME'/patch-backup/*/backup.txt >/dev/null"
check "player conf kept" grep -q 'fps = 120' "$GAME/splitsecond.conf"
check "conf.new written" cmp -s "$PKG/splitsecond.conf" "$GAME/splitsecond.conf.new"
check "says so" said "splitsecond.conf.new"

echo "== 5. old layout: d3d9.dll beside the exe and a foreign dinput8.dll"
printf 'DXVK d3d9\n' > "$GAME/d3d9.dll"
printf 'MZ some other dinput8\n' > "$GAME/dinput8.dll"
sleep 1
run
check "exit 0" rc 0
check "d3d9.dll moved away" [ ! -e "$GAME/d3d9.dll" ]
check "d3d9.dll in backup as foreign" sh -c "grep -l -x 'foreign d3d9.dll' '$GAME'/patch-backup/*/backup.txt >/dev/null"
check "foreign dinput8.dll in backup as foreign" sh -c "grep -l -x 'foreign dinput8.dll' '$GAME'/patch-backup/*/backup.txt >/dev/null"
check "patch dll installed" cmp -s "$PKG/dinput8.dll" "$GAME/dinput8.dll"

echo "== 6. the old HFR mod is refused"
echo x > "$GAME/DXFreezerServer.dll"
run
check "exit 1" rc 1
check "names the HFR mod" said "HFR mod"
rm "$GAME/DXFreezerServer.dll"

echo "== 7. an existing override with another value is replaced"
sed 's/^"dinput8"="native,builtin"$/"dinput8"="builtin"/' "$PFX/user.reg" > "$W/t" && cat "$W/t" > "$PFX/user.reg"
run
check "exit 0" rc 0
check "value is native,builtin again" grep -q -x '"dinput8"="native,builtin"' "$PFX/user.reg"
check "exactly one dinput8 line" [ "$(grep -c '^"dinput8"=' "$PFX/user.reg")" = 1 ]
check "one AppDefaults section" [ "$(grep -c 'AppDefaults' "$PFX/user.reg")" = 1 ]

echo "== 8. uninstall"
echo "save" > "$GAME/save-keep.txt"; mkdir -p "$GAME/save-backups"; echo s > "$GAME/save-backups/x"
echo log > "$GAME/splitsecond-patch.log"
sleep 1
run --uninstall
check "exit 0" rc 0
check "foreign dinput8.dll put back" grep -q 'some other dinput8' "$GAME/dinput8.dll"
check "d3d9.dll put back" grep -q 'DXVK d3d9' "$GAME/d3d9.dll"
check "conf moved out" [ ! -e "$GAME/splitsecond.conf" ]
check "conf.new moved out" [ ! -e "$GAME/splitsecond.conf.new" ]
check "log moved out" [ ! -e "$GAME/splitsecond-patch.log" ]
check "records moved out" sh -c "[ ! -e '$GAME/pipeline-warmup' ] && ls '$GAME'/patch-backup/*-uninstall/pipeline-warmup/docks.bin >/dev/null"
check "conf kept in the uninstall backup" sh -c "grep -q 'fps = 120' '$GAME'/patch-backup/*-uninstall/splitsecond.conf"
check "no dinput8 line left in user.reg" sh -c "! grep -q '^\"dinput8\"=' '$PFX/user.reg'"
check "rest of user.reg as before" sh -c "head -n $(wc -l < "$W/user.reg.orig") '$PFX/user.reg' | cmp -s - '$W/user.reg.orig'"
check "save-backups untouched" [ -f "$GAME/save-backups/x" ]
check "game files untouched" grep -q game-data "$GAME/fmodex.dll"
state > "$W/state.8"

echo "== 9. uninstall again: the foreign dinput8.dll stays"
run --uninstall
check "exit 0" rc 0
check "foreign dinput8.dll still there" grep -q 'some other dinput8' "$GAME/dinput8.dll"
check "says not the patch's" said "not the patch's"

echo "== 10. --game-dir and a missing exe"
run --game-dir "$W/nowhere"
check "exit 1" rc 1
check "says the exe is missing" said "SplitSecond.exe is not in"

echo "== 11. install again, then uninstall.sh: the same result as install.sh --uninstall"
sleep 1
run
check "install: exit 0" rc 0
check "install: override set" grep -q -x '"dinput8"="native,builtin"' "$PFX/user.reg"
sleep 1
unrun
check "uninstall.sh: exit 0" rc 0
check "says removed" said "^Removed\."
check "foreign dinput8.dll put back" grep -q 'some other dinput8' "$GAME/dinput8.dll"
check "d3d9.dll put back" grep -q 'DXVK d3d9' "$GAME/d3d9.dll"
check "no dinput8 line left in user.reg" sh -c "! grep -q '^\"dinput8\"=' '$PFX/user.reg'"
state > "$W/state.11"
check "game folder and user.reg as after step 8" cmp -s "$W/state.8" "$W/state.11"
check "two uninstall backups" [ "$(ls -1d "$GAME"/patch-backup/*-uninstall* | wc -l | tr -d ' ')" = 2 ]

echo "== 12. uninstall.sh passes --game-dir on, knows --help, needs install.sh beside it"
unrun --game-dir "$W/nowhere"
check "exit 1" rc 1
check "says the exe is missing" said "SplitSecond.exe is not in"
unrun --help
check "--help: exit 0" rc 0
check "--help: shows its usage" said "sh uninstall.sh --game-dir DIR"
mkdir -p "$W/alone"; cp "$SRC/uninstall.sh" "$W/alone/"
(cd "$W/alone" && HOME="$H" $SHELL_UNDER_TEST uninstall.sh) > "$W/out.txt" 2>&1; echo $? > "$W/rc.txt"
check "alone: exit 1" rc 1
check "alone: asks to unpack the whole zip" said "Unpack the whole zip first"

if [ $FAIL = 0 ]; then echo "ALL PASSED"; else echo "SOME CHECKS FAILED (last output: $W/out.txt)"; fi
exit $FAIL
