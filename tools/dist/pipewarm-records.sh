#!/bin/sh
# pipewarm-records.sh STAGE - puts the shipped pre-compile records (records/pipeline-warmup/<level>.bin) into
# STAGE/pipeline-warmup/, the folder beside dinput8.dll that the patch reads (src/pipewarm.cpp). The files hold shader
# hashes and pipeline state only, never the game's byte code; merge.py --check refuses anything else.
set -eu
cd "$(dirname "$0")/../.."
STAGE=$1
python3 -I tools/pipewarm/merge.py --check records/pipeline-warmup >/dev/null || {
  echo "records/pipeline-warmup: not clean version 3 records"; exit 1; }
mkdir -p "$STAGE/pipeline-warmup"
cp records/pipeline-warmup/*.bin "$STAGE/pipeline-warmup/"
