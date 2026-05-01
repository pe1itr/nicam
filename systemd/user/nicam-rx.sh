#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"

DEVICE_INDEX="${DEVICE_INDEX:-2}"
FREQ_HZ="${FREQ_HZ:-436000000}"
SAMPLE_RATE="${SAMPLE_RATE:-1456000}"
PPM="${PPM:-0}"
GAIN="${GAIN:-auto}"
EXTRA_RX_ARGS="${EXTRA_RX_ARGS:-}"

cd "${REPO_DIR}"

exec "${REPO_DIR}/tools/nicam-run" nicam-rx \
  --device-index "${DEVICE_INDEX}" \
  --freq "${FREQ_HZ}" \
  --sample-rate "${SAMPLE_RATE}" \
  --ppm "${PPM}" \
  --gain "${GAIN}" \
  ${EXTRA_RX_ARGS}
