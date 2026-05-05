# AGENTS.md

## Project Context

This repository contains SDR experiments for:

- Direct NICAM-like DQPSK TX/RX.
- Stereo WBFM TX/RX.
- PlutoSDR transmit paths.
- RTL-SDR receive paths.

The project is used on a small fixed set of machines. Prefer preserving the
existing launchers and profile structure over adding ad-hoc commands.

## Hosts

- `tim`: PlutoSDR TX host. Uses a checkout-local `.venv`. Main TX frequency is
  `2324 MHz`.
- `websdr`: Odroid/RTL-SDR RX host. Often uses Python 3.8/user install. Main RX
  IF is `436 MHz`.

## Launchers

Use these host-specific entry points for normal operation:

- `tools/tim-wbfm-tx`
- `tools/websdr-wbfm-rx`
- `tools/tim-nicam-tx`
- `tools/websdr-nicam-rx`

Use `tools/nicam-run` for lower-level module starts and for profile-aware audio
routing.

## Environment Profiles

Profile examples live in `config/environments/*.env.example`.

Machine-local profiles should be copied from examples:

```sh
cp config/environments/tim.env.example config/environments/tim.env
cp config/environments/websdr.env.example config/environments/websdr.env
```

Do not commit machine-local `config/environments/*.env` files. Commit only
`.env.example` templates.

Prefer checkout-relative venv paths:

```sh
: "${VENV_PATH:=${REPO_DIR}/.venv}"
```

## Operational Defaults

- WBFM TX on `tim`: `2324 MHz`.
- WBFM TX Pluto gain currently works well around `TX_GAIN_DB=-3`.
- NICAM TX on `tim`: `2324 MHz`.
- NICAM TX should start more conservatively than WBFM, around
  `NICAM_TX_GAIN_DB=-8` to `-6`.
- RTL-SDR RX on `websdr`: `436000000` Hz IF.

## Useful Debug Commands

NICAM RX status without audio playback:

```sh
AUDIO_BACKEND=none NICAM_RX_GAIN=29.7 NICAM_RX_MATCHED_FILTER=1 tools/websdr-nicam-rx --verbose
```

WBFM RX squelch/status:

```sh
tools/websdr-wbfm-rx --verbose
tools/websdr-wbfm-rx --mono
tools/websdr-wbfm-rx --stereo-blend 0.35
```

WBFM TX with temporary gain override:

```sh
TX_GAIN_DB=-3 tools/tim-wbfm-tx
```

NICAM TX with temporary gain override:

```sh
NICAM_TX_GAIN_DB=-8 tools/tim-nicam-tx
```

## Coding Notes

- Keep Python 3.8 compatibility for `websdr`.
- Keep generated files out of git: `__pycache__`, `*.pyc`, `*.so`, `*.iq`, and
  local machine `.env` files.
- For receiver changes, keep audio output compatible with both `ffplay` and
  `aplay` through `tools/nicam-run`.
- For WBFM stream TX, preserve reconnect and silence fallback behavior so a
  short internet stream outage does not stop RF output.
