#!/bin/sh
# install.sh - installs, updates or removes Back on Air, the Split/Second PC patch, on Linux and the Steam Deck (Steam + Proton).
#
#   sh install.sh                     install or update
#   sh install.sh --uninstall         remove the patch, put back what the install had moved away (= sh uninstall.sh)
#   sh install.sh --game-dir DIR      use this game folder (the one with SplitSecond.exe) instead of searching
#
# What it does: copies dinput8.dll, splitsecond.conf and pipeline-warmup/ next to SplitSecond.exe and tells Wine (in the game's
# Proton prefix) to load that dinput8.dll, so no launch option is needed. Anything it replaces goes into
# <game folder>/patch-backup/YYYY-MM-DD_HHMMSS first. It never touches the game's own files or the saves.
set -u

APPID=297860
EXE=SplitSecond.exe
MARK='splitsecond.conf'   # a string every build of the patch's dinput8.dll holds
# The prefix key: an override for SplitSecond.exe only (Wine reads WINEDLLOVERRIDES first, then this key, then the
# prefix-wide [Software\\Wine\\DllOverrides]; Proton's WINEDLLOVERRIDES does not name dinput8).
REGKEY='[Software\\Wine\\AppDefaults\\SplitSecond.exe\\DllOverrides]'
REGVAL='"dinput8"="native,builtin"'
LAUNCH='WINEDLLOVERRIDES="dinput8=n,b" %command%'

HERE=$(cd "$(dirname "$0")" && pwd)
MODE=install
GAME=""
while [ $# -gt 0 ]; do
  case "$1" in
    --uninstall|-u|uninstall) MODE=uninstall ;;
    --game-dir) shift; GAME=${1:-} ;;
    -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
    *) echo "Unknown option: $1 (use --uninstall or --game-dir DIR)"; exit 2 ;;
  esac
  shift
done

STAMP=$(date +%Y-%m-%d_%H%M%S)
BK=""
say() { printf '%s\n' "$*"; }
die() { printf 'STOPPED: %s\n' "$*"; exit 1; }

# ---------------------------------------------------------------- Steam libraries and the game folder
LIBS=""
add_lib() {
  [ -d "$1/steamapps" ] || return 0
  p=$(cd "$1" 2>/dev/null && pwd -P) || return 0
  case "
$LIBS
" in *"
$p
"*) return 0 ;; esac
  LIBS="$LIBS
$p"
}
for root in "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.steam/root" \
            "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam" "$HOME/snap/steam/common/.local/share/Steam"; do
  [ -d "$root/steamapps" ] || continue
  add_lib "$root"
  vdf="$root/steamapps/libraryfolders.vdf"
  [ -f "$vdf" ] || continue
  # lines like:   "path"   "/run/media/mmcblk0p1"   (a backslash is written as \\)
  sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$vdf" | sed 's/\\\\/\\/g' > "${TMPDIR:-/tmp}/ss-libs.$$"
  while IFS= read -r lib; do add_lib "$lib"; done < "${TMPDIR:-/tmp}/ss-libs.$$"
  rm -f "${TMPDIR:-/tmp}/ss-libs.$$"
done

GAMELIB=""
if [ -z "$GAME" ]; then
  while IFS= read -r lib; do
    [ -n "$lib" ] || continue
    acf="$lib/steamapps/appmanifest_$APPID.acf"
    [ -f "$acf" ] || continue
    dir=$(sed -n 's/^[[:space:]]*"installdir"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$acf" | head -n 1)
    [ -n "$dir" ] && [ -f "$lib/steamapps/common/$dir/$EXE" ] || continue
    GAME="$lib/steamapps/common/$dir"; GAMELIB="$lib"; break
  done <<EOF
$LIBS
EOF
fi
if [ -z "$GAME" ]; then
  say "Split/Second was not found in the Steam libraries."
  if [ -t 0 ]; then
    printf 'Type the game folder (the folder with %s) and press Enter: ' "$EXE"
    IFS= read -r GAME || GAME=""
  fi
  [ -n "$GAME" ] || die "no game folder. Run again with: sh install.sh --game-dir \"/path/to/SplitSecond\""
fi
GAME=${GAME%/}
[ -f "$GAME/$EXE" ] || die "$EXE is not in $GAME"
say "Back on Air, a PC patch for Split/Second"
say "Game folder: $GAME"

# The Proton prefix: compatdata/<appid> in the game's library first, then in the other libraries.
REG=""
while IFS= read -r lib; do
  [ -n "$lib" ] || continue
  if [ -f "$lib/steamapps/compatdata/$APPID/pfx/user.reg" ]; then REG="$lib/steamapps/compatdata/$APPID/pfx/user.reg"; break; fi
done <<EOF
$GAMELIB
$LIBS
EOF

# ---------------------------------------------------------------- the game must not run
running() {
  if command -v pgrep >/dev/null 2>&1; then
    # Wine names the game's process SplitSecond.exe; its command line holds ...\SplitSecond.exe
    pgrep -i -x 'splitsecond\.exe' >/dev/null 2>&1 && return 0
    pgrep -f -i '[\\/]splitsecond\.exe' >/dev/null 2>&1 && return 0
  else
    ps -e -o args= 2>/dev/null | grep -i -q '[\\/][s]plitsecond\.exe' && return 0
  fi
  # The prefix's wineserver keeps the registry in memory and writes it back when it exits. Its working folder is
  # /tmp/.wine-<uid>/server-<device>-<inode> of the prefix folder.
  [ -n "$REG" ] && [ -d /proc ] || return 1
  di=$(stat -L -c '%d %i' "$(dirname "$REG")" 2>/dev/null) || return 1
  sdir=$(printf '/tmp/.wine-%s/server-%x-%x' "$(id -u)" $di)
  for pid in $(pgrep -x wineserver 2>/dev/null); do
    [ "$(readlink "/proc/$pid/cwd" 2>/dev/null)" = "$sdir" ] && return 0
  done
  return 1
}
if running; then
  die "Split/Second (or its Proton prefix) is still running. Quit the game, wait a few seconds and run this again."
fi

# ---------------------------------------------------------------- helpers
is_ours() { grep -a -q "$MARK" "$1" 2>/dev/null; }
backup_dir() {
  [ -n "$BK" ] && return 0
  mkdir -p "$GAME/patch-backup" || die "cannot make $GAME/patch-backup"
  b="$GAME/patch-backup/$STAMP"; n=2
  while ! mkdir "$b" 2>/dev/null; do b="$GAME/patch-backup/$STAMP-$n"; n=$((n + 1)); [ $n -lt 50 ] || die "cannot make a backup folder"; done
  BK="$b"
}
# move_away NAME KIND: moves <game>/NAME into the backup folder; KIND is patch (ours) or foreign (to restore on uninstall)
move_away() {
  backup_dir
  mv "$GAME/$1" "$BK/$1" || die "cannot move $1 into $BK"
  printf '%s %s\n' "$2" "$1" >> "$BK/backup.txt"
  say "  $1 moved to patch-backup/$(basename "$BK")/"
}
copy_in() { cp "$HERE/$1" "$GAME/$2.ss-new" && mv -f "$GAME/$2.ss-new" "$GAME/$2" || die "cannot write $GAME/$2 (is the folder writable?)"; }

# user.reg: has_override / edit_reg set|unset
has_override() {
  SSKEY=$REGKEY awk 'BEGIN { k = tolower(ENVIRON["SSKEY"]); f = 1 }
    /^\[/ { s = (index(tolower($0), k) == 1); next }
    s && tolower($0) == "\"dinput8\"=\"native,builtin\"" { f = 0 }
    END { exit f }' "$REG"
}
edit_reg() {
  now=$(date +%s)
  ft=$(printf '%x' $(( (now + 11644473600) * 10000000 )))
  SSKEY=$REGKEY SSVAL=$REGVAL SSMODE=$1 SSNOW=$now SSFT=$ft awk '
    BEGIN { k = tolower(ENVIRON["SSKEY"]); v = ENVIRON["SSVAL"]; m = ENVIRON["SSMODE"]; seen = 0; s = 0; pend = 0 }
    function put() { if (pend) { if (m == "set") print v; pend = 0 } }
    /^\[/ { put(); s = (index(tolower($0), k) == 1); if (s) { seen = 1; pend = 1 } print; next }
    pend && /^#time=/ { print; put(); next }
    { put() }
    s && tolower($0) ~ /^"dinput8"=/ { next }
    { print }
    END { put(); if (m == "set" && !seen) { print ""; print ENVIRON["SSKEY"] " " ENVIRON["SSNOW"]; print "#time=" ENVIRON["SSFT"]; print v } }
  ' "$REG" > "$REG.ss-new" || { rm -f "$REG.ss-new"; die "could not edit $REG"; }
  # keep the file's owner and mode: write the new text into the existing file
  cat "$REG.ss-new" > "$REG" && rm -f "$REG.ss-new" || die "could not write $REG"
}
backup_reg() {
  backup_dir
  cp -p "$REG" "$BK/user.reg" || die "cannot back up $REG"
  printf 'copy user.reg (from %s)\n' "$REG" >> "$BK/backup.txt"
}

# ---------------------------------------------------------------- uninstall
if [ "$MODE" = uninstall ]; then
  STAMP="$STAMP-uninstall"
  if [ -f "$GAME/dinput8.dll" ]; then
    if is_ours "$GAME/dinput8.dll"; then move_away dinput8.dll patch
    else say "  dinput8.dll is not the patch's; left alone"; fi
  fi
  for f in splitsecond.conf splitsecond.conf.new splitsecond-patch.log splitsecond-patch.prev.log splitsecond-patch.warned; do
    [ -f "$GAME/$f" ] && move_away "$f" patch
  done
  [ -d "$GAME/pipeline-warmup" ] && move_away pipeline-warmup patch
  # put back what an install moved away (the newest copy of each), unless something is there again
  for f in dinput8.dll d3d9.dll; do
    [ -e "$GAME/$f" ] && continue
    for d in $(ls -1 "$GAME/patch-backup" 2>/dev/null | sort -r); do
      if grep -q -x "foreign $f" "$GAME/patch-backup/$d/backup.txt" 2>/dev/null && [ -f "$GAME/patch-backup/$d/$f" ]; then
        cp -p "$GAME/patch-backup/$d/$f" "$GAME/$f" && say "  $f put back from patch-backup/$d/"
        break
      fi
    done
  done
  if [ -n "$REG" ]; then
    if grep -i -q '^"dinput8"=' "$REG" && SSKEY=$REGKEY awk 'BEGIN { k = tolower(ENVIRON["SSKEY"]); f = 1 }
         /^\[/ { s = (index(tolower($0), k) == 1); next } s && tolower($0) ~ /^"dinput8"=/ { f = 0 } END { exit f }' "$REG"; then
      backup_reg; edit_reg unset; say "  dinput8 override removed from the Proton prefix"
    fi
  fi
  say "Removed. Left alone: your saves, save-backups/ (copies of your saves), shader-cache/ (can be deleted),"
  say "patch-backup/ (what the patch replaced; your settings file is in the newest folder there)."
  [ -f "$GAME/splitsecond-online.key" ] && say "splitsecond-online.key is kept: it is your online account key. Keep it if you want the same account and name later."
  say "If you had set the launch option $LAUNCH, remove it in Steam (game > Properties > General)."
  exit 0
fi

# ---------------------------------------------------------------- install / update
for f in dinput8.dll splitsecond.conf; do
  [ -f "$HERE/$f" ] || die "$f is missing next to install.sh (unpack the whole zip and run install.sh from there)"
done

if [ -e "$GAME/DXFreezerServer.dll" ]; then
  die "the old Split/Second HFR mod is installed (DXFreezerServer.dll). It conflicts with this patch (the game freezes).
Move DXFreezerServer.dll and SplitSecondHFR.ini out of the game folder, then run this again."
fi
[ -e "$GAME/SplitSecondHFR.ini" ] && say "  note: SplitSecondHFR.ini (old HFR mod) is in the game folder; it does nothing without DXFreezerServer.dll"

# dinput8.dll
if [ -f "$GAME/dinput8.dll" ] && cmp -s "$HERE/dinput8.dll" "$GAME/dinput8.dll"; then
  say "  dinput8.dll is already this version"
else
  if [ -e "$GAME/dinput8.dll" ]; then
    if is_ours "$GAME/dinput8.dll"; then move_away dinput8.dll patch; else move_away dinput8.dll foreign; fi
  fi
  copy_in dinput8.dll dinput8.dll; say "  dinput8.dll installed"
fi

# a d3d9.dll beside the exe (an older layout of this patch: DXVK copied there) would replace Proton's own DXVK
if [ -e "$GAME/d3d9.dll" ]; then
  say "  d3d9.dll beside the game (older layout) is not needed:"; move_away d3d9.dll foreign
fi

# the settings file: an existing one is the player's and stays
if [ ! -f "$GAME/splitsecond.conf" ]; then
  copy_in splitsecond.conf splitsecond.conf; say "  splitsecond.conf installed (default settings)"
elif cmp -s "$HERE/splitsecond.conf" "$GAME/splitsecond.conf"; then
  say "  splitsecond.conf is already the default one"
else
  copy_in splitsecond.conf splitsecond.conf.new
  say "  your splitsecond.conf is kept; this version's defaults are in splitsecond.conf.new"
fi

# pre-compile records (pipeline-warmup/<level>.bin); the player's own records in pipeline-warmup/local/ stay
if [ -d "$HERE/pipeline-warmup" ]; then
  mkdir -p "$GAME/pipeline-warmup" || die "cannot make $GAME/pipeline-warmup"
  n=0
  for f in "$HERE"/pipeline-warmup/*.bin; do
    [ -f "$f" ] || continue
    b=$(basename "$f")
    cmp -s "$f" "$GAME/pipeline-warmup/$b" && continue
    copy_in "pipeline-warmup/$b" "pipeline-warmup/$b"; n=$((n + 1))
  done
  if [ $n -gt 0 ]; then say "  pre-compile records installed (pipeline-warmup/, $n files)"
  else say "  pre-compile records are already this version"; fi
fi

# the dinput8 override in the Proton prefix
if [ -z "$REG" ]; then
  say ""
  say "The game's Proton prefix does not exist yet (the game has never been started)."
  say "Start Split/Second once from Steam, quit it, and run this script again."
  say "Or set this launch option instead (Steam: game > Properties > General > Launch options):"
  say "  $LAUNCH"
  exit 0
fi
if has_override; then
  say "  dinput8 override is already set in the Proton prefix"
else
  backup_reg; edit_reg set
  if has_override; then say "  dinput8 override set in the Proton prefix ($REG)"
  else die "the override could not be written into $REG; use the launch option: $LAUNCH"; fi
fi
[ -n "$BK" ] && say "Backup of what was replaced: $BK"
say ""
say "Done. Start Split/Second from Steam; no launch option is needed."
say "PC SETTINGS in the game: F1, hold Back (View), or Y on the Options screen."
say "If Steam resets the prefix (another Proton version, or \"delete compatibility data\"), run this again or set:"
say "  $LAUNCH"
say "To remove the patch later: sh uninstall.sh"
