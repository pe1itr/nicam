# NICAM Reference Path

This document records the current operational reference path for live NICAM
receive and for future decoder cleanup work.

## AFEDRI Live Receive Baseline

Run from the AFEDRI pipeline checkout:

```sh
cd /home/rhardenb/repos/sdr-dvb-pipelines

./afedri-udp.py | /home/rhardenb/repo-prop/nicam-transmitter/nicam-rx \
  --iq-format s16 \
  --sample-rate 1456000 \
  --adaptive-fixed \
  --timing-search-steps 1 \
  --chunk-bytes 8192 \
  --conceal-mode bridge \
  --stats-every 1000 \
| aplay -f S16_LE -r 32000 -c 2 --buffer-time=2000000 --period-time=200000
```

For normal listening, `--stats-every 1000` may be omitted. It only writes
diagnostics to stderr and does not affect the PCM stream.

## Fixed Assumptions

- Input SDR stream: AFEDRI UDP helper output.
- Input IQ format: complex interleaved `s16le`, `I,Q,I,Q,...`.
- Input sample rate: `1456000` samples/s.
- NICAM symbol rate: `364000` symbols/s.
- Samples per symbol: `4`.
- Decoder path: C decoder `./nicam-rx`.
- Demod mode: `--adaptive-fixed`.
- Timing search: `--timing-search-steps 1`.
- Live chunk size: `--chunk-bytes 8192`.
- Bad frame handling: `--conceal-mode bridge`.
- Audio output: stereo `s16le`, `32000` Hz.
- ALSA buffering: `--buffer-time=2000000 --period-time=200000`.

## Why This Is The Reference

`--adaptive-fixed` uses the known-good DQPSK hypothesis for the current live
signal. It avoids the expensive multi-hypothesis path and keeps live behavior
predictable.

`--chunk-bytes 8192` is the best-sounding AFEDRI live value found so far. Larger
chunks such as `32768` can show lower-looking startup behavior in short tests,
but live listening has repeatedly sounded more broken with larger chunks. The
reference therefore prioritizes audible continuity on the AFEDRI stream.

`--conceal-mode bridge` keeps audio duration closer to the IQ stream when frames
are rejected or skipped during short resynchronization events.

## Health Indicators

Useful stats fields:

- `bad`: rejected or missing frames after lock.
- `align_drop_bits`: frame alignment drops; bursts usually indicate input
  discontinuities.
- `sync_drop_bits`: bits dropped while searching for the next FAW.
- `faw_errsum`: should normally stay `0` when FAWs are found.
- `hyp`: should stay `0` with `--adaptive-fixed`.
- `pending`: bridge concealment backlog; should usually return to `0`.

Typical dropout signature:

- `faw_errsum=0` remains stable.
- `hyp=0` remains stable.
- `bad`, `align_drop_bits`, and sometimes `sync_drop_bits` rise in bursts.

This points more strongly to AFEDRI UDP/socket/input discontinuities than to a
continuous RF demodulation problem.

## Change Policy

Do not change the reference path while cleaning code unless the new path is
tested against this baseline.

Before changing decoder behavior, run at least:

```sh
PYTHONPATH=src tools/nicam-loop-test --seconds 1 --pattern prbs --snr-db clean --allowed-missing-frames 10
```

For live changes, compare against the AFEDRI baseline above for:

- audible continuity,
- `bad` growth rate,
- burst behavior in `align_drop_bits` and `sync_drop_bits`,
- station-ID output when a transmitting source provides it.

Any cleanup should preserve the baseline command shape unless there is a clear
measured improvement.
