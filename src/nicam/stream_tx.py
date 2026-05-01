from __future__ import annotations

import argparse
import math
import subprocess
import sys
from typing import BinaryIO

import numpy as np

from .audio_payload import (
    AUDIO_SAMPLE_RATE,
    CHANNELS,
    PCM_VALUES_PER_NICAM_FRAME,
    pcm16_to_payload,
)
from .dqpsk import bits_to_symbols, final_phase_quarter, upsample_symbols
from .frame import build_frame
from .iq import complex_to_u8_iq
from .pipe import suppress_stdout_broken_pipe
from .rtlsdr_rx import DEFAULT_SAMPLE_RATE


def open_audio_source(args: argparse.Namespace) -> tuple[BinaryIO, subprocess.Popen[bytes] | None]:
    if args.tone:
        return ToneSource(args.tone_hz, args.seconds), None

    source = args.stream_url or args.audio_file
    if source is None:
        raise SystemExit("Gebruik --stream-url, --audio-file of --tone")

    cmd = [
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "error",
        "-i",
        source,
        "-f",
        "s16le",
        "-ac",
        str(CHANNELS),
        "-ar",
        str(AUDIO_SAMPLE_RATE),
        "-",
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=sys.stderr)
    if proc.stdout is None:
        raise RuntimeError("ffmpeg stdout is not available")
    return proc.stdout, proc


class ToneSource:
    def __init__(self, hz: float, seconds: float | None):
        self.hz = hz
        self.remaining = None if seconds is None else int(seconds * AUDIO_SAMPLE_RATE)
        self.phase = 0

    def read(self, size: int) -> bytes:
        frames = size // (2 * CHANNELS)
        if self.remaining is not None:
            frames = min(frames, self.remaining)
            self.remaining -= frames
        if frames <= 0:
            return b""

        n = np.arange(frames, dtype=np.float32) + self.phase
        self.phase += frames
        wave = 0.35 * np.sin(2 * math.pi * self.hz * n / AUDIO_SAMPLE_RATE)
        stereo = np.column_stack([wave, wave])
        return np.rint(stereo.reshape(-1) * 32767).astype(np.int16).tobytes()

    def close(self) -> None:
        pass


def run(args: argparse.Namespace) -> int:
    source, proc = open_audio_source(args)
    out = sys.stdout.buffer if args.out == "-" else open(args.out, "wb")
    frame_bytes = PCM_VALUES_PER_NICAM_FRAME * 2
    frame_index = 0
    max_frames = None if args.seconds is None else int(args.seconds * 1000)
    pending = bytearray()
    phase_quarter = 0

    try:
        while True:
            if max_frames is not None and frame_index >= max_frames:
                break
            raw = source.read(frame_bytes)
            if not raw:
                break
            if len(raw) < frame_bytes:
                raw += b"\x00" * (frame_bytes - len(raw))

            pcm = np.frombuffer(raw, dtype=np.int16)
            payload = pcm16_to_payload(pcm)
            bits = build_frame(payload, frame_index=frame_index, mode=0, fallback=0)
            symbols = bits_to_symbols(bits, phase_quarter)
            phase_quarter = final_phase_quarter(bits, phase_quarter)
            iq = upsample_symbols(symbols, args.sample_rate)
            pending.extend(complex_to_u8_iq(iq, args.amplitude))
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
        if proc is not None:
            proc.terminate()
            proc.wait(timeout=2)

    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Modulate stream/audio to direct NICAM-like DQPSK IQ")
    parser.add_argument("--stream-url", help="audio stream URL decoded by ffmpeg")
    parser.add_argument("--audio-file", help="audio file decoded by ffmpeg")
    parser.add_argument("--tone", action="store_true", help="generate an internal sine tone")
    parser.add_argument("--tone-hz", type=float, default=1000.0)
    parser.add_argument("--seconds", type=float)
    parser.add_argument("--sample-rate", type=int, default=DEFAULT_SAMPLE_RATE)
    parser.add_argument("--amplitude", type=float, default=0.7)
    parser.add_argument("--out", default="-", help="IQ output file, or - for stdout")
    parser.add_argument("--flush-frames", type=int, default=20)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    return run(parse_args(sys.argv[1:] if argv is None else argv))


if __name__ == "__main__":
    raise SystemExit(main())
