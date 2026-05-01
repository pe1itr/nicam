#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np

from nicam.dqpsk import bits_to_symbols, upsample_symbols
from nicam.frame import build_test_frame
from nicam.iq import complex_to_u8_iq


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate synthetic rtl_sdr-style NICAM IQ")
    parser.add_argument("--frames", type=int, default=200)
    parser.add_argument("--sample-rate", type=int, default=1_456_000)
    parser.add_argument("--amplitude", type=float, default=0.7)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    bits = np.concatenate([build_test_frame(i) for i in range(args.frames)])
    symbols = bits_to_symbols(bits)
    iq = upsample_symbols(symbols, args.sample_rate)
    Path(args.out).write_bytes(complex_to_u8_iq(iq, args.amplitude))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
