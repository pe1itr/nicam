from __future__ import annotations

import argparse
import subprocess
import sys
from collections import deque
from typing import BinaryIO

import numpy as np

from .constants import FRAME_BITS, SYMBOL_RATE
from .dqpsk import coarse_symbols_from_samples, symbols_to_bits
from .frame import descramble_frame, find_frame_offsets, select_lock_offset
from .iq import u8_iq_to_complex


DEFAULT_SAMPLE_RATE = SYMBOL_RATE * 4


def iq_u8_to_complex(raw: bytes) -> np.ndarray:
    return u8_iq_to_complex(raw)


def open_iq_source(args: argparse.Namespace) -> tuple[BinaryIO, subprocess.Popen[bytes] | None]:
    if args.iq_file:
        return open(args.iq_file, "rb"), None

    cmd = [
        "rtl_sdr",
        "-f",
        str(args.freq),
        "-s",
        str(args.sample_rate),
        "-p",
        str(args.ppm),
        "-",
    ]
    if args.gain != "auto":
        cmd[1:1] = ["-g", str(args.gain)]

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=sys.stderr)
    if proc.stdout is None:
        raise RuntimeError("rtl_sdr stdout is not available")
    return proc.stdout, proc


def best_bits_for_chunk(
    iq: np.ndarray,
    sample_rate: int,
    freq_offset: float,
    previous_symbols: list[complex | np.complex64 | None],
) -> tuple[np.ndarray, int, int]:
    sps = int(round(sample_rate / SYMBOL_RATE))
    best_bits = np.empty(0, dtype=np.uint8)
    best_phase = 0
    best_score = -1

    for phase in range(sps):
        symbols = coarse_symbols_from_samples(iq, sample_rate, phase, freq_offset)
        bits = symbols_to_bits(symbols, previous_symbols[phase])
        score = len(find_frame_offsets(bits, max_errors=0, min_repeats=3))
        if score > best_score:
            best_bits = bits
            best_phase = phase
            best_score = score

    return best_bits, best_phase, best_score


def run(args: argparse.Namespace) -> int:
    if args.sample_rate % SYMBOL_RATE:
        raise SystemExit("--sample-rate moet een veelvoud van 364000 zijn")

    source, proc = open_iq_source(args)
    bit_buffer: deque[int] = deque(maxlen=FRAME_BITS * 24)
    total_frames = 0
    chunk_samples = args.chunk_symbols * int(args.sample_rate / SYMBOL_RATE)
    sps = int(round(args.sample_rate / SYMBOL_RATE))
    previous_symbols: list[complex | np.complex64 | None] = [None] * sps

    try:
        while True:
            raw = source.read(chunk_samples * 2)
            if not raw:
                break

            iq = iq_u8_to_complex(raw)
            symbols_by_phase = [
                coarse_symbols_from_samples(iq, args.sample_rate, phase, args.freq_offset)
                for phase in range(sps)
            ]
            bits_by_phase = [
                symbols_to_bits(symbols, previous_symbols[phase])
                for phase, symbols in enumerate(symbols_by_phase)
            ]
            scores = [
                len(find_frame_offsets(bits, max_errors=0, min_repeats=3))
                for bits in bits_by_phase
            ]
            phase = int(np.argmax(scores))
            score = scores[phase]
            bits = bits_by_phase[phase]
            for idx, symbols in enumerate(symbols_by_phase):
                if symbols.size:
                    previous_symbols[idx] = symbols[-1]
            bit_buffer.extend(int(x) for x in bits)
            data = np.fromiter(bit_buffer, dtype=np.uint8)
            offset = select_lock_offset(
                data,
                max_errors=args.faw_errors,
                min_repeats=args.min_repeats,
                flag_frames=args.flag_frames,
            )
            if offset is None:
                if args.verbose:
                    print(f"no lock: timing_phase={phase} faw_pairs={score}")
                continue

            while offset + FRAME_BITS <= data.size:
                raw_frame = data[offset : offset + FRAME_BITS]
                try:
                    frame = descramble_frame(raw_frame)
                except ValueError:
                    break

                total_frames += 1
                print(
                    "frame "
                    f"{total_frames:06d}: flag={frame.frame_flag} "
                    f"mode={frame.mode:03b} fallback={frame.fallback} "
                    f"timing_phase={phase}"
                )
                offset += FRAME_BITS

            keep_from = min(offset, data.size)
            bit_buffer = deque((int(x) for x in data[keep_from:]), maxlen=FRAME_BITS * 24)
    finally:
        source.close()
        if proc is not None:
            proc.terminate()
            proc.wait(timeout=2)

    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Direct NICAM DQPSK RTL-SDR receiver")
    parser.add_argument("--freq", type=int, default=100_000_000, help="RF center frequency in Hz")
    parser.add_argument("--sample-rate", type=int, default=DEFAULT_SAMPLE_RATE)
    parser.add_argument("--ppm", type=int, default=0)
    parser.add_argument("--gain", default="auto")
    parser.add_argument("--freq-offset", type=float, default=0.0, help="baseband correction in Hz")
    parser.add_argument("--iq-file", help="read rtl_sdr-style uint8 IQ from a file")
    parser.add_argument("--chunk-symbols", type=int, default=8192)
    parser.add_argument("--faw-errors", type=int, default=0)
    parser.add_argument("--min-repeats", type=int, default=3)
    parser.add_argument("--flag-frames", type=int, default=16)
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    return run(parse_args(sys.argv[1:] if argv is None else argv))


if __name__ == "__main__":
    raise SystemExit(main())
