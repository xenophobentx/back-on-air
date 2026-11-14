#!/bin/sh
# make-dist.sh - builds the player zip: build/dist/back-on-air-<release>-<date>-<commit>.zip (run by `make dist`).
#
# Contents: dinput8.dll, splitsecond.conf (= splitsecond.conf.example, which holds the release defaults), README.md,
# THIRD_PARTY_NOTICES.md, install.bat + install.ps1 + uninstall.bat (Windows), install.sh + uninstall.sh (Linux / Steam Deck),
# and dxvk/ = DXVK 2.7.1's 32-bit d3d9.dll exactly as released, its licence and a README (Windows: renderer = dxvk),
# pipeline-warmup/ = the pre-compile records of the 5 levels (shader hashes and pipeline state; tools/dist/pipewarm-records.sh).
# Nothing from the game. The DXVK release tarball is downloaded once into build/cache and checked by SHA-256.
set -eu
cd "$(dirname "$0")/../.."
REPO=$(pwd)

DXVK_VER=2.7.1
DXVK_URL="https://github.com/doitsujin/dxvk/releases/download/v$DXVK_VER/dxvk-$DXVK_VER.tar.gz"
DXVK_TGZ_SHA=d85ce7c79f57ecd765aaa1b9e7007cb875e6fde9f6d331df799bce73d513ce87
DXVK_D3D9_SHA=00ecd422b3b12e9d3b309785f3f19431405389d80eb4a37018ebc63e2d87d900

sha256() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }

make build/dinput8.dll

mkdir -p build/cache
TGZ="build/cache/dxvk-$DXVK_VER.tar.gz"
if [ ! -f "$TGZ" ] || [ "$(sha256 "$TGZ")" != "$DXVK_TGZ_SHA" ]; then
  echo "downloading $DXVK_URL"
  curl -fsSL -o "$TGZ.part" "$DXVK_URL"
  mv "$TGZ.part" "$TGZ"
fi
[ "$(sha256 "$TGZ")" = "$DXVK_TGZ_SHA" ] || { echo "dxvk tarball: SHA-256 mismatch"; exit 1; }

RELEASE=1.0   # Back on Air release number, in the zip name
VER="$(date +%Y%m%d)-$(git rev-parse --short HEAD)"
git diff --quiet HEAD -- src third_party Makefile splitsecond.conf.example tools/dist records 2>/dev/null || VER="$VER-dirty"
NAME="back-on-air-$RELEASE-$VER"
STAGE="build/dist/$NAME"
rm -rf "$STAGE" "build/dist/$NAME.zip"
mkdir -p "$STAGE/dxvk"

cp build/dinput8.dll "$STAGE/dinput8.dll"
cp splitsecond.conf.example "$STAGE/splitsecond.conf"
cp README.md THIRD_PARTY_NOTICES.md "$STAGE/"
cp tools/dist/install.ps1 tools/dist/install.bat tools/dist/uninstall.bat tools/dist/install.sh tools/dist/uninstall.sh "$STAGE/"
chmod 755 "$STAGE/install.sh" "$STAGE/uninstall.sh"
sh tools/dist/pipewarm-records.sh "$STAGE"

tmp="build/cache/dxvk-x"; rm -rf "$tmp"; mkdir -p "$tmp"
tar -xzf "$TGZ" -C "$tmp" "dxvk-$DXVK_VER/x32/d3d9.dll"
cp "$tmp/dxvk-$DXVK_VER/x32/d3d9.dll" "$STAGE/dxvk/d3d9.dll"
rm -rf "$tmp"
[ "$(sha256 "$STAGE/dxvk/d3d9.dll")" = "$DXVK_D3D9_SHA" ] || { echo "dxvk d3d9.dll: SHA-256 mismatch"; exit 1; }
cp third_party/dxvk/LICENSE "$STAGE/dxvk/LICENSE"
cat > "$STAGE/dxvk/README.txt" <<EOF
Back on Air (Split/Second PC patch): DXVK $DXVK_VER (32-bit d3d9.dll), unmodified, from $DXVK_URL
DXVK is Direct3D 9 on Vulkan, by Philip Rebohle and contributors: https://github.com/doitsujin/dxvk
Licence: zlib/libpng, in LICENSE next to this file.
On Windows the patch loads this d3d9.dll when splitsecond.conf says renderer = dxvk (the default).
Under Wine / Proton this folder is not used.
EOF

(cd build/dist && rm -f "$NAME.zip" && zip -q -r -X "$NAME.zip" "$NAME")
echo "build/dist/$NAME.zip"
(cd build/dist && unzip -l "$NAME.zip")
