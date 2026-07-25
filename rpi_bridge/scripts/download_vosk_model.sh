#!/usr/bin/env bash
# Fetch the small English Vosk model used by the bridge.
#
#   sudo ./scripts/download_vosk_model.sh            # -> /opt/tivvy/models
#   ./scripts/download_vosk_model.sh ~/tivvy/models  # -> custom location
#
# The small model is ~40 MB and decodes faster than real time on one Pi 4 core.
# Swap MODEL for vosk-model-en-us-0.22 (1.8 GB) only if you have the RAM and
# are willing to trade latency for accuracy.
set -euo pipefail

MODEL="${MODEL:-vosk-model-small-en-us-0.15}"
DEST="${1:-/opt/tivvy/models}"
URL="https://alphacephei.com/vosk/models/${MODEL}.zip"

if [[ -d "${DEST}/${MODEL}" ]]; then
  echo "Model already present at ${DEST}/${MODEL}"
  exit 0
fi

command -v unzip >/dev/null || { echo "unzip missing: sudo apt install unzip" >&2; exit 1; }

mkdir -p "${DEST}"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

echo "Downloading ${MODEL} ..."
curl -fL --progress-bar -o "${TMP}/model.zip" "${URL}"

echo "Extracting to ${DEST} ..."
unzip -q "${TMP}/model.zip" -d "${DEST}"

echo "Done: ${DEST}/${MODEL}"
echo "Set [asr] model_path = \"${DEST}/${MODEL}\" in your config."
