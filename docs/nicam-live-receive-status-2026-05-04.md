# NICAM live receive status, 2026-05-04

## Status

The repository now contains a C decoder path that can decode a real live NICAM
signal from AFEDRI UDP IQ and play recognizable audio.

This is no longer only an offline FAW or bitstream test: the current setup
recovers DQPSK, finds NICAM frames, descrambles the body, de-interleaves the
payload, decodes stereo PCM, applies J.17 de-emphasis and plays live audio.

## Working Pipeline

The current cleanup/reference baseline is recorded in
`docs/nicam-reference-path.md`. Keep that command shape as the regression target
while simplifying the decoder.

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
| aplay -f S16_LE -r 32000 -c 2 --buffer-time=2000000 --period-time=200000
```

## Signal Assumptions

- Input IQ format: complex interleaved `s16le`, `I,Q,I,Q,...`.
- Input sample rate: `1456000` samples/s.
- NICAM symbol rate: `364000` symbols/s.
- Samples per symbol: `4`.
- Output audio: stereo `s16le`, `32000` Hz.

## Decoder Options

- `--adaptive-fixed`: uses the known-good adaptive DQPSK hypothesis.
- `--timing-search-steps 1`: avoids live multi-hypothesis search.
- `--chunk-bytes 8192`: best-sounding AFEDRI live value found so far. The
  general C decoder default remains `32768`, but live AFEDRI receive currently
  benefits from the smaller chunk.
- `--conceal-mode bridge`: interpolates over rejected frames.
- J.17 de-emphasis is always enabled in the C decoder, as required for NICAM
  audio balance.

## Current Observations

- Audio is recognizable and live.
- J.17 de-emphasis audibly improves tonal balance; without it the sound was thin.
- The C decoder now uses the standard descrambler alignment directly: the PRBS
  sequence starts at the first bit after FAW, so the normal descramble phase is
  `0`. The old phase-9 behaviour is retained only as `--legacy-descramble` for
  comparison.
- With the standard PRBS alignment, the current test buffer reports
  `raw_faw_hits=32`, `raw_faw_error_sum=0`, and payload parity errors `0`.
- Smaller decoder chunks plus a larger ALSA buffer make playback much more
  continuous.
- Remaining artifacts are likely dominated by AFEDRI UDP packet loss and by
  short bad-frame bursts.
- Tone tests revealed very short regular cuts when FAW resynchronization skipped
  damaged frames. The C decoder now treats a jump to the next FAW as missing
  audio and schedules bridge concealment for those missing frames. On the
  offline `nicam.iq` test this moved decoded audio duration from `26.266 s`
  closer to the IQ duration, `26.638 s` versus `26.758 s`.
- A 10 second TX/RX tone loop was swept across decoder chunk sizes. Chunks from
  `8192` through `32768` bytes stayed continuous with no phase jumps. Live
  listening on the AFEDRI stream sounded best with `8192`. The older
  1 MiB chunk is not suitable for the adaptive live path because it exceeds the
  bitbuffer working window and drops audio. The C decoder default is now
  `32768` bytes.

## Diagnostics

For live debugging, add `--stats-every N`. Statistics are written on stderr and
do not corrupt the PCM stream:

```sh
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

Watch `bad`, `align_drop_bits` and `sync_drop_bits`. Rising counters during a
steady tone should correlate with short audible cuts or concealment events.

## Standard TX / Pluto Loop Baseline

The Python NICAM TX generator used before `nicam.pluto_tx` has been aligned with
the C decoder and EN 300 163:

- PN9 scrambling now starts with the standard post-FAW sequence
  `0000 0111 1011 1110 0010`.
- C0 frame flag now starts with eight `1` frames followed by eight `0` frames.
- `nicam728` payload TX applies J.17 pre-emphasis before near-instantaneous
  companding.
- The C decoder always applies J.17 de-emphasis on receive.

The Pluto launcher still uses the same operational shape:

```sh
tools/tim-nicam-tx
```

Internally this is:

```sh
tools/nicam-run module nicam.stream_tx ... \
| tools/nicam-run module nicam.pluto_tx ...
```

Useful baseband loop check:

```sh
PYTHONPATH=src python3 -m nicam.loop_test \
  --seconds 0.5 \
  --pattern tone \
  --snr-db clean \
  --pulse-shape \
  --rx-matched-filter \
  --allowed-missing-frames 20
```

Observed result after the TX standard alignment:

```text
decoded_frames=498
expected_frames=500
lost_lock=no
```

Direct TX bitstream-quality check on a generated tone IQ reports:

```text
raw_faw_hits=32
raw_faw_error_sum=0
body_descramble phase=0
c0_errors=0
payload-after-descramble parity_errors=0
```

## Next Steps

1. Reduce UDP packet loss in `afedri-udp.py`/network/socket buffering.
2. Keep this command as the reference live receive baseline.
3. Do not change the DQPSK mapping while FAW remains stable.
4. Further refine bad-frame concealment only after packet loss is under control.
