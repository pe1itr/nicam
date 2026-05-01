from __future__ import annotations

import argparse
import subprocess
import sys
from typing import BinaryIO

import numpy as np

from .constants import SYMBOL_RATE
from .pipe import suppress_stdout_broken_pipe


DEFAULT_HACKRF_SAMPLE_RATE = SYMBOL_RATE * 11


def open_iq_input(path: str) -> BinaryIO:
    if path == "-":
        return sys.stdin.buffer
    return open(path, "rb")


def u8_iq_to_s8(raw: bytes) -> bytes:
    u8 = np.frombuffer(raw, dtype=np.uint8)
    if u8.size % 2:
        u8 = u8[:-1]
    return (u8.astype(np.int16) - 128).astype(np.int8).tobytes()


def build_hackrf_command(args: argparse.Namespace) -> list[str]:
    cmd = [
        "hackrf_transfer",
        "-t",
        "-",
        "-f",
        str(args.lo),
        "-s",
        str(args.sample_rate),
        "-x",
        str(args.tx_gain),
        "-a",
        "1" if args.amp else "0",
    ]
    if args.serial:
        cmd.extend(["-d", args.serial])
    if args.rf_bandwidth:
        cmd.extend(["-b", str(args.rf_bandwidth)])
    return cmd


def run(args: argparse.Namespace) -> int:
    if args.sample_rate % SYMBOL_RATE:
        raise SystemExit("--sample-rate moet een veelvoud van 364000 zijn")
    if args.sample_rate < 2_000_000:
        raise SystemExit("HackRF sample-rate moet minimaal ongeveer 2000000 zijn")

    source = open_iq_input(args.iq_in)
    proc = subprocess.Popen(build_hackrf_command(args), stdin=subprocess.PIPE)
    if proc.stdin is None:
        raise RuntimeError("hackrf_transfer stdin is not available")

    try:
        while True:
            raw = source.read(args.buffer_bytes)
            if not raw:
                break
            converted = u8_iq_to_s8(raw)
            if not converted:
                continue
            try:
                proc.stdin.write(converted)
            except BrokenPipeError:
                suppress_stdout_broken_pipe()
                return proc.wait()
    finally:
        source.close()
        try:
            proc.stdin.close()
        except BrokenPipeError:
            pass

    return proc.wait()


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Transmit rtl_sdr-style uint8 IQ via HackRF")
    parser.add_argument("--iq-in", default="-", help="rtl_sdr-style uint8 IQ input, or - for stdin")
    parser.add_argument("--lo", type=int, required=True, help="RF carrier in Hz")
    parser.add_argument("--sample-rate", type=int, default=DEFAULT_HACKRF_SAMPLE_RATE)
    parser.add_argument("--tx-gain", type=int, default=0, help="HackRF TX VGA gain, 0-47 dB")
    parser.add_argument("--amp", action="store_true", help="enable HackRF RF amplifier")
    parser.add_argument("--rf-bandwidth", type=int, help="baseband filter bandwidth in Hz")
    parser.add_argument("--serial", help="HackRF serial number")
    parser.add_argument("--buffer-bytes", type=int, default=262_144)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    return run(parse_args(sys.argv[1:] if argv is None else argv))


if __name__ == "__main__":
    raise SystemExit(main())
