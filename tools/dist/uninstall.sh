#!/bin/sh
# uninstall.sh - removes Back on Air, the Split/Second PC patch, on Linux and the Steam Deck (same as "sh install.sh --uninstall").
#
#   sh uninstall.sh                   remove the patch, put back what the install had moved away
#   sh uninstall.sh --game-dir DIR    use this game folder (the one with SplitSecond.exe) instead of searching
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
case "${1:-}" in
  -h|--help) sed -n '2,5p' "$0"; exit 0 ;;
esac
if [ ! -f "$HERE/install.sh" ]; then
  echo "Unpack the whole zip first, then run uninstall.sh from the unpacked folder."
  exit 1
fi
exec sh "$HERE/install.sh" --uninstall "$@"
