# Environment profiles

`tools/nicam-run` loads one profile before starting receivers or transmitters.

Default profile lookup:

```sh
config/environments/$(hostname -s).env
```

Override profile lookup:

```sh
NICAM_ENV=websdr tools/nicam-run wbfm-rx ...
NICAM_ENV_FILE=/opt/nicam/local.env tools/nicam-run nicam-rx ...
```

Profiles are shell env files. Keep machine-specific device names, Python mode
and audio player choices here instead of putting them in systemd units or long
manual commands.

For `PYTHON_MODE=venv`, prefer `VENV_PATH=${REPO_DIR}/.venv`. If a profile still
contains an old absolute venv path and the checkout-local `.venv` exists,
`tools/nicam-run` falls back to that checkout-local venv automatically.

## NICAM TX audio bus

For live NICAM TX, prefer a constant UDP audio bus instead of feeding the
internet stream directly into the transmitter:

```sh
NICAM_TX_SOURCE=udp
NICAM_TX_UDP_URL=udp://0.0.0.0:7355
```

Send raw stereo `s16le` at `32 kHz` to that UDP port. With `ffmpeg`, pace the
source in real time:

```sh
ffmpeg -hide_banner -loglevel error -re -i input \
  -vn -ac 2 -ar 32000 -f s16le \
  'udp://tim:7355?pkt_size=1024'
```

The current NICAM TX spectrum-shaping defaults follow the NICAM 728
transmitter-side data-shaping filter for systems B/G/H/K1/L:

```sh
NICAM_TX_PULSE_SHAPE=1
NICAM_TX_PULSE_ROLLOFF=0.4
NICAM_TX_PULSE_SPAN_SYMBOLS=6
```

This uses root-raised-cosine pulse shaping in the transmitter. An ideal
receiver using the same filter gives the 40 % cosine roll-off response described
by ETSI EN 300 163.

The receiver can apply the matching root-raised-cosine FIR before symbol
timing:

```sh
NICAM_RX_MATCHED_FILTER=1
NICAM_RX_MATCHED_ROLLOFF=0.4
NICAM_RX_MATCHED_SPAN_SYMBOLS=6
```
