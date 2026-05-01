from __future__ import annotations

import argparse
import sys
import time
from typing import BinaryIO

import numpy as np

from .constants import SYMBOL_RATE
from .iq import u8_iq_to_complex
from .rtlsdr_rx import DEFAULT_SAMPLE_RATE


def init_pluto(
    uri: str,
    sample_rate: int,
    lo_hz: int,
    tx_gain_db: int,
    rf_bandwidth: int | None = None,
    cyclic: bool = False,
):
    """Create and configure a PlutoSDR TX object via pyadi-iio."""
    try:
        import adi
    except ImportError as exc:
        raise SystemExit("Installeer pyadi-iio: pip install pyadi-iio") from exc

    sdr = adi.Pluto(uri)
    sdr.sample_rate = int(sample_rate)
    sdr.tx_lo = int(lo_hz)
    if rf_bandwidth is not None:
        sdr.tx_rf_bandwidth = int(rf_bandwidth)
    sdr.tx_hardwaregain_chan0 = int(tx_gain_db)
    sdr.tx_cyclic_buffer = bool(cyclic)
    return sdr


def scale_for_pluto(iq: np.ndarray, amplitude: float = 0.8) -> np.ndarray:
    scaled = np.asarray(iq, dtype=np.complex64) * float(amplitude)
    peak = float(np.max(np.abs(scaled))) if scaled.size else 1.0
    if peak > 1.0:
        scaled = scaled / peak
    return (scaled * (2**14)).astype(np.complex64)


def transmit_iq(
    uri: str,
    sample_rate: int,
    lo_hz: int,
    tx_gain_db: int,
    iq: np.ndarray,
    rf_bandwidth: int | None = None,
    cyclic: bool = True,
) -> None:
    sdr = init_pluto(uri, sample_rate, lo_hz, tx_gain_db, rf_bandwidth, cyclic)
    scaled = scale_for_pluto(iq)
    sdr.tx(scaled.astype(np.complex64))


def open_iq_input(path: str) -> BinaryIO:
    if path == "-":
        return sys.stdin.buffer
    return open(path, "rb")


def stream_u8_iq_to_pluto(args: argparse.Namespace) -> int:
    if args.sample_rate % SYMBOL_RATE:
        raise SystemExit("--sample-rate moet een veelvoud van 364000 zijn")

    sdr = init_pluto(
        args.uri,
        args.sample_rate,
        args.lo,
        args.tx_gain,
        args.rf_bandwidth,
        args.cyclic,
    )
    source = open_iq_input(args.iq_in)
    raw_chunks: list[bytes] = []
    chunk_bytes = args.buffer_samples * 2

    try:
        while True:
            raw = source.read(chunk_bytes)
            if not raw:
                break
            if len(raw) < 2:
                break

            if args.cyclic:
                raw_chunks.append(raw)
                continue

            iq = u8_iq_to_complex(raw)
            if iq.size:
                sdr.tx(scale_for_pluto(iq, args.amplitude))

        if args.cyclic:
            raw = b"".join(raw_chunks)
            iq = u8_iq_to_complex(raw)
            if not iq.size:
                raise SystemExit("Geen IQ-samples gelezen voor cyclic TX")
            sdr.tx(scale_for_pluto(iq, args.amplitude))
            print("Cyclic TX draait. Stop met Ctrl-C.", file=sys.stderr)
            try:
                while True:
                    time.sleep(1)
            except KeyboardInterrupt:
                pass
    finally:
        source.close()
        try:
            sdr.tx_destroy_buffer()
        except Exception:
            pass

    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Transmit rtl_sdr-style uint8 IQ via PlutoSDR")
    parser.add_argument("--uri", default="ip:192.168.2.1")
    parser.add_argument("--iq-in", default="-", help="rtl_sdr-style uint8 IQ input, or - for stdin")
    parser.add_argument("--sample-rate", type=int, default=DEFAULT_SAMPLE_RATE)
    parser.add_argument("--lo", type=int, required=True)
    parser.add_argument("--tx-gain", type=int, default=-30)
    parser.add_argument("--rf-bandwidth", type=int, default=750_000)
    parser.add_argument("--buffer-samples", type=int, default=32_768)
    parser.add_argument("--amplitude", type=float, default=0.8)
    parser.add_argument(
        "--cyclic",
        action="store_true",
        help="read all input first and repeat it in the Pluto cyclic TX buffer",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    return stream_u8_iq_to_pluto(parse_args(sys.argv[1:] if argv is None else argv))


if __name__ == "__main__":
    raise SystemExit(main())
