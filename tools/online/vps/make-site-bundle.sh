#!/bin/sh
# Builds the deploy bundle for the VPS (rdvd with the web pages, the unit, the Caddy snippet, the deploy note).
# Usage: sh tools/online/vps/make-site-bundle.sh OUT.tar.gz      (run from anywhere; touches nothing but OUT)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ONLINE=$(dirname "$HERE")
OUT=$1
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/backonair/opt/splitsecond-rdv" "$STAGE/backonair/caddy"
cp "$ONLINE/rdvd_server.py" "$STAGE/backonair/opt/splitsecond-rdv/"
(cd "$ONLINE" && tar --exclude __pycache__ -cf - rdvd) | tar -C "$STAGE/backonair/opt/splitsecond-rdv" -xf -
cp "$HERE/splitsecond-rdv.service" "$HERE/site-deploy.md" "$STAGE/backonair/"
cp "$HERE/caddy/backonair.caddy" "$STAGE/backonair/caddy/"
tar -C "$STAGE" -czf "$OUT" backonair
echo "$OUT"
