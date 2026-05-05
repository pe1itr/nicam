# NICAM DQPSK breakthrough, 2026-05-04

## Context

Capture under test:

- File: `nicam.iq`
- Format: complex interleaved `s16le` IQ
- Sample rate: `1456000` samples/s
- Symbol rate: `364000` symbols/s
- Samples per symbol: `4`

Earlier fixed-phase DQPSK slicing produced a bitstream, but not a usable NICAM
frame stream. The raw FAW was too weak, so descrambling, parity and audio were
not the primary problem yet.

## Breakthrough

The new adaptive DQPSK path in `src/nicam_rx/nicam_cli.c` can recover correct raw NICAM
FAW data from `nicam.iq`.

New options:

- `--adaptive-demod`: searches adaptive demod hypotheses.
- `--adaptive-fixed`: uses the working hypothesis found for `nicam.iq`.

The adaptive path uses:

- AGC
- matched filter
- QPSK slicer
- Costas-like carrier loop
- Mueller and Muller timing recovery
- NICAM/ETSI differential DQPSK demapping

Working hypothesis for `nicam.iq`:

```text
conj=0
reverse=0
invert=0
rot=0
swap=0
carrier_sign=-1
timing_sign=1
initial_mu=0.0000
initial_freq_hz=0.0
```

## Reproduction

Build:

```sh
tools/build-nicam
```

Old fixed slicer on first 1 MiB:

```sh
dd if=nicam.iq of=/tmp/nicam-s16-head1m.iq bs=1048576 count=1 status=none
./nicam-rx --iq-format s16 --sample-rate 1456000 --bitstream-quality \
  < /tmp/nicam-s16-head1m.iq \
  > /tmp/nicam-old-head1m.pcm \
  2> /tmp/nicam-old-head1m.err
```

Observed old result:

```text
quality: selected_hyp=838 raw_faw_best_offset=70 raw_faw_best_frames=23 raw_faw_hits=1 raw_faw_error_sum=63
quality: q_hist=2021/2140/1981/2594 delta_hist=2374/2151/2005/2205
```

New adaptive demod on first 1 MiB:

```sh
./nicam-rx --iq-format s16 --sample-rate 1456000 --adaptive-demod --bitstream-quality \
  < /tmp/nicam-s16-head1m.iq \
  > /tmp/nicam-adapt-head1m.pcm \
  2> /tmp/nicam-adapt-head1m.err
```

Observed adaptive result:

```text
adaptive_quality: selected_hyp=128 raw_faw_best_offset=60 raw_faw_best_frames=32 raw_faw_hits=32 raw_faw_error_sum=0
adaptive_quality: conj=0 reverse=0 invert=0 rot=0 swap=0 carrier_sign=-1 timing_sign=1 initial_mu=0.0000 initial_freq_hz=0.0 symbols=64608 bits=69888 q_hist=16256/16010/16624/15717 omega=4.020000 carrier_hz=-196.8
```

The same result was confirmed on later chunks:

```text
mid chunk:  raw_faw_hits=32/32 raw_faw_error_sum=0
late chunk: raw_faw_hits=32/32 raw_faw_error_sum=0
```

Fast fixed adaptive mode over the full file:

```sh
./nicam-rx --iq-format s16 --sample-rate 1456000 --adaptive-fixed --timing-search-steps 1 --bitstream-quality \
  < nicam.iq \
  > /tmp/nicam-adapt-fixed-full.pcm \
  2> /tmp/nicam-adapt-fixed-full.err
```

Observed final-buffer FAW result:

```text
adaptive_quality: selected_hyp=0 raw_faw_best_offset=424 raw_faw_best_frames=32 raw_faw_hits=32 raw_faw_error_sum=0
adaptive_quality: conj=0 reverse=0 invert=0 rot=0 swap=0 carrier_sign=-1 timing_sign=1 initial_mu=0.0000 initial_freq_hz=0.0 symbols=9602171 bits=69888 q_hist=2436255/2343366/2507550/2314999 omega=4.020000 carrier_hz=-257.7
```

## Current Status

Correct raw FAW data is now recovered from the DQPSK layer. The previous blocker
was the fixed DQPSK slicer without carrier/timing recovery.

The next decoder step after FAW has also been checked: body descrambling and
control-bit interpretation now pass on the first MiB of `nicam.iq`.

Command:

```sh
./nicam-rx --iq-format s16 --sample-rate 1456000 --adaptive-fixed --timing-search-steps 1 --bitstream-quality \
  < /tmp/nicam-s16-head1m.iq \
  > /tmp/nicam-body-step-head2.pcm \
  2> /tmp/nicam-body-step-head2.err
```

Observed body/control result:

```text
quality: body_descramble offset=60 frames=32 phase=9 raw16=0000011110111110 ctrl16=0000000000000000 C=00000 AD=00000000000 c0_phase=11 c0_errors=0
quality: body_descramble mode_hist=32/0/0/0/0/0/0/0
```

This means:

- the standard descrambler phase is `9` in the current C PN9 table;
- `C0` follows the expected 8-on/8-off 16-frame pattern with zero errors;
- `C1 C2 C3 = 000`, so the capture announces stereo audio frames;
- `AD` is zero for the checked frames.

The payload ordering, de-interleaver and parity word bit order have now been
updated to match ETSI EN 300 163:

- The 704-bit interleaver follows the transmitted order
  `25,69,113,...,685`, then `26,70,114,...,686`, etc.
- For original payload bit `n`, the received interleaved index is
  `(n % 44) * 16 + (n / 44)`.
- The 11-bit sound words are `X0..X9,P`, where `X0` is LSB and `X9` is MSB.
- Parity is checked over `X4..X9` plus `P`, not over the first six array bits.

Observed after the spec payload fix on the first MiB:

```text
quality: payload-after-descramble offset=60 frames=32 parity_errors=96
quality: payload_variant=normal frames=8 parity_errors=24 left=4 right=4
quality: descramble_phase best_variant=normal phase=25 frames=8 parity_errors=24 left=4 right=4
```

Observed on the full-file final buffer:

```text
quality: body_descramble offset=424 frames=32 phase=9 raw16=1000011110111110 ctrl16=1000000000000000 C=10000 AD=00000000000 c0_phase=6 c0_errors=0
quality: payload-after-descramble offset=424 frames=32 parity_errors=96
quality: payload_variant=normal frames=8 parity_errors=24 left=1 right=1
quality: descramble_phase best_variant=normal phase=25 frames=8 parity_errors=24 left=1 right=1
```

Audio output is now integrated into the adaptive path. Without
`--bitstream-quality`, `--adaptive-fixed` decodes frames and writes stereo
`s16le` PCM at 32 kHz to stdout.

Command:

```sh
./nicam-rx --iq-format s16 --sample-rate 1456000 --adaptive-fixed --timing-search-steps 1 \
  < nicam.iq \
  > /tmp/nicam-adaptive-full.pcm \
  2> /tmp/nicam-adaptive-full.err
```

Observed PCM result:

```text
1794944 bytes PCM
14023 NICAM audio frames
14.023 seconds at stereo 32 kHz
stderr empty
```

Signal statistics from the decoded PCM:

```text
left  rms ~= 777, min=-24448, max=23424
right rms ~= 813, min=-31424, max=32512
```

## Live Real-Signal Decode

The current setup now decodes a real live NICAM signal from the AFEDRI UDP IQ
source.

Working source side, from the separate SDR pipeline checkout:

```sh
cd /home/rhardenb/repos/sdr-dvb-pipelines
./afedri-udp.py
```

The AFEDRI stream is expected to produce complex interleaved `s16le` IQ at
`1456000` samples/s.

Working live audio pipeline:

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

This command has been confirmed by listening. It produces recognizable real
NICAM audio. The audio is substantially more continuous with
`--chunk-bytes 32768` than with the original 1 MiB decoder chunks, because the
decoder no longer writes PCM in large bursts. The ALSA buffer settings also help
absorb the remaining burstiness and short packet-loss events.

J.17 de-emphasis is required for natural audio balance. It is now always applied
by the C decoder. Before this was added, the decoded sound was intelligible but
thin and lacked low frequency/midrange body.

The current live baseline options are:

- `--adaptive-fixed`: known-good DQPSK/carrier/timing hypothesis for this signal.
- `--timing-search-steps 1`: no live hypothesis search, lower CPU/latency.
- `--chunk-bytes 8192`: current best-sounding AFEDRI live chunking. The general
  C decoder default remains `32768`.
- `--conceal-mode bridge`: interpolate over rejected/bad frames.
- `aplay -f S16_LE -r 32000 -c 2`: output is raw stereo signed 16-bit PCM at
  32 kHz.

The C decoder now uses the standard PRBS descrambler alignment directly. The
standard sequence after FAW begins `0000 0111 1011 1110 0010`; internally this
means the normal descramble phase is now `0`. The old phase-9 interpretation is
kept only for comparison via `--legacy-descramble`.

Verification after the standard PRBS alignment change:

```text
adaptive_quality: selected_hyp=0 raw_faw_hits=32 raw_faw_error_sum=0
quality: body_descramble offset=424 frames=32 phase=0 raw16=1000011110111110 ctrl16=1000000000000000 C=10000 AD=00000000000 c0_errors=0
quality: payload-after-descramble offset=424 frames=32 parity_errors=0
quality: payload_variant=normal frames=8 parity_errors=0 left=1 right=1
quality: descramble_phase best_variant=normal phase=16 frames=8 parity_errors=0 left=1 right=1
```

Important operational note: AFEDRI UDP packet loss still directly affects audio
quality. Earlier live tests showed very high packet loss in `afedri-udp.py`; if
that counter rises quickly, remaining clicks, holes or short artifacts are
expected even when the NICAM decoder remains locked.

## Next Development Step

1. Keep the live pipeline above as the reference command for real NICAM receive.
2. Reduce AFEDRI UDP packet loss before spending more time on audio concealment.
3. Keep `--adaptive-fixed` as the DQPSK/FAW reference path.
4. Keep standard `phase=0` body descrambling as the reference for `nicam.iq`.
5. Use the spec payload/de-interleaver path as the audio decode path.
6. Keep J.17 de-emphasis enabled; it is mandatory in the C decoder.
7. Do not change the DQPSK mapping unless FAW regresses; the adaptive path proves
   the ETSI mapping and IQ orientation are correct for `nicam.iq`.
