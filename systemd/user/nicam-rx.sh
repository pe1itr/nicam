#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"

DEVICE_INDEX="${DEVICE_INDEX:-2}"
FREQ_HZ="${FREQ_HZ:-2324000000}"
SAMPLE_RATE="${SAMPLE_RATE:-1456000}"
PPM="${PPM:-0}"
GAIN="${GAIN:-auto}"
AUDIO_OUT="${AUDIO_OUT:--}"
EXTRA_RX_ARGS="${EXTRA_RX_ARGS:-}"

cd "${REPO_DIR}"

RTL_CMD=(rtl_sdr -d "${DEVICE_INDEX}" -f "${FREQ_HZ}" -s "${SAMPLE_RATE}" -p "${PPM}")
if [[ "${GAIN}" != "auto" ]]; then
  RTL_CMD+=(-g "${GAIN}")
fi
RTL_CMD+=(-)

exec "${RTL_CMD[@]}" \
  | PYTHONPATH=src python -m nicam.stream_rx \
      --iq-in - \
      --sample-rate "${SAMPLE_RATE}" \
      --audio-out "${AUDIO_OUT}" \
      ${EXTRA_RX_ARGS}
