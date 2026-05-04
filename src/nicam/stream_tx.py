from __future__ import annotations

import argparse
import sys
import time
from typing import BinaryIO

import numpy as np

from .audio_payload import (
    AUDIO_SAMPLE_RATE,
    CHANNELS,
    PCM_VALUES_PER_NICAM_FRAME,
    pcm16_to_payload,
)
from .nicam728 import J17Preemphasis, pcm16_to_nicam_payload, pcm16_to_nicam_payload_j17
from .dqpsk import bits_to_symbols, filter_baseband, final_phase_quarter, shape_symbols, upsample_symbols
from .frame import build_frame
from .iq import complex_to_u8_iq
from .pipe import suppress_stdout_broken_pipe
from .constants import DEFAULT_SAMPLE_RATE
from .tx_audio import PCMFormat, open_constant_pcm_source


def nicam_level_to_amplitude(args: argparse.Namespace) -> float:
    selected = [
        args.amplitude is not None,
        args.nicam_rf_level is not None,
        args.nicam_level_db is not None,
    ]
    if sum(selected) > 1:
        raise SystemExit("Gebruik maar een van --amplitude, --nicam-rf-level of --nicam-level-db")

    if args.nicam_rf_level is not None:
        if args.nicam_rf_level < 0 or args.nicam_rf_level > 1023:
            raise SystemExit("--nicam-rf-level moet tussen 0 en 1023 liggen")
        return args.nicam_rf_level / 1023.0

    if args.nicam_level_db is not None:
        if args.nicam_level_db > 0:
            raise SystemExit("--nicam-level-db moet 0 dB of lager zijn")
        return 10 ** (args.nicam_level_db / 20.0)

    return 0.7


def open_audio_source(args: argparse.Namespace) -> BinaryIO:
    fmt = PCMFormat(sample_rate=AUDIO_SAMPLE_RATE, channels=CHANNELS)
    if args.tone:
        source = "tone"
    elif args.silence:
        source = "silence"
    elif args.udp_url:
        source = "udp"
    elif args.audio_device is not None:
        source = "device"
    elif args.audio_file:
        source = "file"
    elif args.stream_url:
        source = "stream"
    else:
        raise SystemExit("Gebruik --stream-url, --audio-file, --udp-url, --audio-device, --silence of --tone")

    return open_constant_pcm_source(
        source=source,
        fmt=fmt,
        block_frames=PCM_VALUES_PER_NICAM_FRAME // CHANNELS,
        stream_url=args.stream_url,
        audio_file=args.audio_file,
        device=args.audio_device,
        udp_url=args.udp_url,
        tone_hz=args.tone_hz,
        seconds=args.seconds,
        reconnect=args.ffmpeg_reconnect,
        reconnect_delay_s=args.input_reconnect_delay,
        read_timeout_s=args.pcm_read_timeout_ms / 1000.0,
        buffer_ms=args.pcm_buffer_ms,
        ffmpeg_loglevel=args.ffmpeg_loglevel,
    )


def run(args: argparse.Namespace) -> int:
    source = open_audio_source(args)
    out = sys.stdout.buffer if args.out == "-" else open(args.out, "wb")
    amplitude = nicam_level_to_amplitude(args)
    station_id = args.station_id
    if station_id is not None:
        station_id = station_id[:8]
    j17 = J17Preemphasis()
    encode_payload = pcm16_to_payload if args.payload_format == "lab" else None
    frame_bytes = PCM_VALUES_PER_NICAM_FRAME * 2
    frame_index = 0
    max_frames = None if args.seconds is None else int(args.seconds * 1000)
    pending = bytearray()
    phase_quarter = 0
    realtime_start = time.monotonic()

    try:
        while True:
            if max_frames is not None and frame_index >= max_frames:
                break
            if args.realtime:
                target_time = realtime_start + (frame_index / 1000.0)
                sleep_s = target_time - time.monotonic()
                if sleep_s > 0:
                    time.sleep(sleep_s)
            raw = source.read(frame_bytes)
            if len(raw) < frame_bytes:
                raw += b"\x00" * (frame_bytes - len(raw))

            pcm = np.frombuffer(raw, dtype=np.int16)
            payload = (
                encode_payload(pcm)
                if encode_payload is not None
                else pcm16_to_nicam_payload_j17(pcm, j17)
            )
            bits = build_frame(
                payload,
                frame_index=frame_index,
                mode=0,
                fallback=0,
                station_id=station_id,
            )
            symbols = bits_to_symbols(bits, phase_quarter)
            phase_quarter = final_phase_quarter(bits, phase_quarter)
            if args.pulse_shape:
                iq = shape_symbols(
                    symbols,
                    args.sample_rate,
                    rolloff=args.pulse_rolloff,
                    span_symbols=args.pulse_span_symbols,
                )
            else:
                iq = upsample_symbols(symbols, args.sample_rate)
            if args.baseband_filter:
                iq = filter_baseband(
                    iq,
                    args.sample_rate,
                    cutoff_hz=args.baseband_filter_cutoff,
                    taps=args.baseband_filter_taps,
                )
            pending.extend(complex_to_u8_iq(iq, amplitude))
            try:
                if frame_index % args.flush_frames == args.flush_frames - 1:
                    out.write(pending)
                    pending.clear()
                    out.flush()
            except BrokenPipeError:
                suppress_stdout_broken_pipe()
                return 0
            frame_index += 1
        if pending:
            try:
                out.write(pending)
                out.flush()
            except BrokenPipeError:
                suppress_stdout_broken_pipe()
                return 0
    finally:
        source.close()
        if out is not sys.stdout.buffer:
            out.close()

    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Modulate stream/audio to direct NICAM-like DQPSK IQ")
    parser.add_argument("--stream-url", help="audio stream URL decoded by ffmpeg")
    parser.add_argument("--audio-file", help="audio file decoded by ffmpeg")
    parser.add_argument("--udp-url", help="UDP input URL decoded by ffmpeg, bijvoorbeeld udp://0.0.0.0:7355")
    parser.add_argument("--audio-device", help="Audio input device naam of index voor sounddevice")
    parser.add_argument("--silence", action="store_true", help="generate continuous digital silence")
    parser.add_argument("--tone", action="store_true", help="generate an internal sine tone")
    parser.add_argument("--tone-hz", type=float, default=1000.0)
    parser.add_argument("--seconds", type=float)
    parser.add_argument("--realtime", action="store_true", help="pace output at the NICAM frame rate")
    parser.add_argument("--sample-rate", type=int, default=DEFAULT_SAMPLE_RATE)
    parser.add_argument(
        "--pulse-shape",
        action="store_true",
        help="apply root-raised-cosine FIR pulse shaping after DQPSK symbols",
    )
    parser.add_argument("--pulse-rolloff", type=float, default=0.4)
    parser.add_argument("--pulse-span-symbols", type=int, default=6)
    parser.add_argument(
        "--baseband-filter",
        action="store_true",
        help="apply a short low-pass FIR to the generated baseband IQ",
    )
    parser.add_argument("--baseband-filter-cutoff", type=float, default=500000.0)
    parser.add_argument("--baseband-filter-taps", type=int, default=17)
    parser.add_argument(
        "--amplitude",
        type=float,
        help="legacy IQ amplitude scale; default is 0.7 when no NICAM level is set",
    )
    parser.add_argument(
        "--nicam-rf-level",
        type=int,
        help="Digital Baseband compatible NICAM carrier level, 0..1023",
    )
    parser.add_argument(
        "--nicam-level-db",
        type=float,
        help="NICAM carrier level in dBFS, e.g. -14.2 is about rf_level 200",
    )
    parser.add_argument(
        "--payload-format",
        choices=["nicam728", "lab"],
        default="nicam728",
        help="audio payload codec (default: nicam728)",
    )
    parser.add_argument(
        "--station-id",
        help="send first 8 ASCII characters in NICAM additional data bits, compatible with Digital Baseband V1.4",
    )
    parser.add_argument("--out", default="-", help="IQ output file, or - for stdout")
    parser.add_argument("--flush-frames", type=int, default=20)
    parser.add_argument(
        "--ffmpeg-reconnect",
        action="store_true",
        help="enable ffmpeg reconnect flags when using --stream-url",
    )
    parser.add_argument(
        "--input-reconnect-delay",
        type=float,
        default=2.0,
        help="wait time in seconds before restarting a stopped input decoder",
    )
    parser.add_argument(
        "--ffmpeg-loglevel",
        default="error",
        help="ffmpeg loglevel for --stream-url/--audio-file (default: error)",
    )
    parser.add_argument(
        "--pcm-buffer-ms",
        type=int,
        default=1200,
        help="PCM jitter buffer size in ms for external inputs (default: 1200)",
    )
    parser.add_argument(
        "--pcm-read-timeout-ms",
        type=int,
        default=120,
        help="wait time before filling missing TX audio with silence (default: 120)",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    return run(parse_args(sys.argv[1:] if argv is None else argv))


if __name__ == "__main__":
    raise SystemExit(main())
