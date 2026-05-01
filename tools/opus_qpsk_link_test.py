#!/usr/bin/env python3
from __future__ import annotations

import argparse
import math
from pathlib import Path

import numpy as np

from nicam.dqpsk import (
    adaptive_symbols_from_samples,
    bits_to_symbols,
    coarse_symbols_from_samples,
    final_phase_quarter,
    symbols_to_bits,
    upsample_symbols,
)
from nicam.iq import complex_to_u8_iq, u8_iq_to_complex
from opus.frames import encode_packet, try_decode_packet
from opus.qpsk_rx import PREAMBLE_BITS, find_packet, score_phase
from opus.qpsk_tx import PREAMBLE, bytes_to_bits


QPSK_POINTS = np.array([1 + 0j, 0 + 1j, -1 + 0j, 0 - 1j], dtype=np.complex64)


def parse_offsets(text: str) -> list[float]:
    if ":" not in text:
        return [float(part) for part in text.split(",") if part]

    start_text, stop_text, step_text = text.split(":", 2)
    start = float(start_text)
    stop = float(stop_text)
    step = float(step_text)
    if step == 0:
        raise argparse.ArgumentTypeError("offset step cannot be 0")

    values: list[float] = []
    value = start
    if step > 0:
        while value <= stop + abs(step) * 1e-9:
            values.append(value)
            value += step
    else:
        while value >= stop - abs(step) * 1e-9:
            values.append(value)
            value += step
    return values


def expected_payload(sequence: int, payload_bytes: int) -> bytes:
    return bytes([sequence & 0xFF]) * payload_bytes


def generate(args: argparse.Namespace) -> int:
    phase_quarter = 0
    out = Path(args.out).open("wb")
    try:
        for sequence in range(args.packets):
            payload = expected_payload(sequence, args.payload_bytes)
            packet = PREAMBLE + encode_packet(sequence, payload)
            bits = bytes_to_bits(packet)
            if sequence == 0:
                bits = np.concatenate([np.zeros(2, dtype=np.uint8), bits])

            symbols = bits_to_symbols(bits, phase_quarter)
            phase_quarter = final_phase_quarter(bits, phase_quarter)
            iq = upsample_symbols(symbols, args.sample_rate)
            out.write(complex_to_u8_iq(iq, args.amplitude))
    finally:
        out.close()
    return 0


def symbol_quality(symbols: np.ndarray) -> tuple[float, float, float]:
    if symbols.size == 0:
        return math.nan, math.nan, math.nan

    centered = symbols.astype(np.complex64)
    power = float(np.mean(np.abs(centered) ** 2))
    if power <= 0:
        return math.nan, math.nan, math.nan

    normalized = centered / math.sqrt(power)
    distances = np.abs(normalized[:, None] - QPSK_POINTS[None, :])
    nearest = QPSK_POINTS[np.argmin(distances, axis=1)]
    error = normalized - nearest
    evm_rms = float(np.sqrt(np.mean(np.abs(error) ** 2)))
    snr_db = -20.0 * math.log10(evm_rms) if evm_rms > 0 else math.inf
    mean_amp = float(np.mean(np.abs(symbols)))
    return evm_rms, snr_db, mean_amp


def decode_packets(bits: np.ndarray, payload_bytes: int) -> tuple[int, int, int, int | None, int | None]:
    pos = 0
    decoded = 0
    payload_ok = 0
    lost = 0
    first_sequence: int | None = None
    last_sequence: int | None = None
    expected_sequence: int | None = None

    while pos + PREAMBLE_BITS < bits.size:
        found = find_packet(bits[pos:])
        if found is None:
            break

        _start_bit, stop_bit, packet = found
        frame = try_decode_packet(packet)
        if frame is None:
            pos += max(stop_bit, 1)
            continue

        decoded += 1
        if first_sequence is None:
            first_sequence = frame.sequence
        if expected_sequence is not None and frame.sequence != expected_sequence:
            lost += (frame.sequence - expected_sequence) & 0xFFFFFFFF
        expected_sequence = (frame.sequence + 1) & 0xFFFFFFFF
        last_sequence = frame.sequence

        if frame.payload == expected_payload(frame.sequence, payload_bytes):
            payload_ok += 1

        pos += max(stop_bit, 1)

    return decoded, payload_ok, lost, first_sequence, last_sequence


def analyze_one(
    iq: np.ndarray,
    sample_rate: int,
    freq_offset: float,
    phase: int,
    payload_bytes: int,
    adaptive_demod: bool,
) -> dict[str, float | int | None]:
    if adaptive_demod:
        symbols = adaptive_symbols_from_samples(iq, sample_rate, phase, freq_offset)
    else:
        symbols = coarse_symbols_from_samples(iq, sample_rate, phase, freq_offset)
    bits = symbols_to_bits(symbols)
    preambles = score_phase(bits)
    decoded, payload_ok, lost, first_sequence, last_sequence = decode_packets(bits, payload_bytes)
    evm_rms, snr_db, mean_amp = symbol_quality(symbols)
    return {
        "offset": freq_offset,
        "phase": phase,
        "adaptive": int(adaptive_demod),
        "symbols": int(symbols.size),
        "preambles": int(preambles),
        "decoded": int(decoded),
        "payload_ok": int(payload_ok),
        "lost": int(lost),
        "first": first_sequence,
        "last": last_sequence,
        "evm": evm_rms,
        "snr": snr_db,
        "mean_amp": mean_amp,
    }


def print_result(result: dict[str, float | int | None]) -> None:
    first = "-" if result["first"] is None else str(result["first"])
    last = "-" if result["last"] is None else str(result["last"])
    printable = dict(result)
    printable["first"] = first
    printable["last"] = last
    print(
        "offset={offset:9.0f} phase={phase} symbols={symbols} "
        "preambles={preambles:5d} decoded={decoded:5d} payload_ok={payload_ok:5d} "
        "lost={lost:5d} first={first} last={last} evm={evm:.4f} snr={snr:.1f}dB "
        "mean_amp={mean_amp:.4f}".format(**printable)
    )


def analyze(args: argparse.Namespace) -> int:
    raw = Path(args.iq_in).read_bytes()
    iq = u8_iq_to_complex(raw)
    if args.max_samples is not None:
        iq = iq[: args.max_samples]

    sps = args.sample_rate / args.symbol_rate
    if abs(sps - round(sps)) > 1e-9:
        raise SystemExit("--sample-rate moet een geheel aantal samples per symbool zijn")
    phase_values = range(int(round(sps))) if args.timing_phase is None else [args.timing_phase]

    results = []
    for offset in args.offsets:
        for phase in phase_values:
            result = analyze_one(
                iq,
                args.sample_rate,
                offset,
                phase,
                args.payload_bytes,
                args.adaptive_demod,
            )
            results.append(result)
            if not args.best_only:
                print_result(result)

    if args.best_only:
        best = max(
            results,
            key=lambda item: (
                int(item["decoded"]),
                int(item["payload_ok"]),
                int(item["preambles"]),
                float(item["snr"]),
            ),
        )
        print_result(best)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate/analyze known Opus-QPSK modem test packets")
    subparsers = parser.add_subparsers(dest="command", required=True)

    gen = subparsers.add_parser("generate", help="generate rtl_sdr-style uint8 IQ with known packets")
    gen.add_argument("--packets", type=int, default=10_000)
    gen.add_argument("--payload-bytes", type=int, default=32)
    gen.add_argument("--sample-rate", type=int, default=2_184_000)
    gen.add_argument("--amplitude", type=float, default=0.7)
    gen.add_argument("--out", required=True)
    gen.set_defaults(func=generate)

    ana = subparsers.add_parser("analyze", help="analyze a received rtl_sdr-style uint8 IQ capture")
    ana.add_argument("--iq-in", required=True)
    ana.add_argument("--sample-rate", type=int, default=2_184_000)
    ana.add_argument("--symbol-rate", type=int, default=364_000)
    ana.add_argument("--payload-bytes", type=int, default=32)
    ana.add_argument("--offsets", type=parse_offsets, default=[0.0], help="comma list or start:stop:step")
    ana.add_argument("--timing-phase", type=int)
    ana.add_argument("--max-samples", type=int)
    ana.add_argument("--best-only", action="store_true")
    ana.add_argument("--adaptive-demod", action="store_true")
    ana.set_defaults(func=analyze)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
