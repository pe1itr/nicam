from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

from .audio_payload import PCM_VALUES_PER_NICAM_FRAME
from .dqpsk import bits_to_symbols, final_phase_quarter, shape_symbols, upsample_symbols
from .frame import build_frame
from .iq import complex_to_u8_iq, u8_iq_to_complex
from .nicam728 import (
    J17Deemphasis,
    J17Preemphasis,
    nicam_payload_to_pcm16,
    pcm16_to_nicam_payload_j17,
)
from .constants import DEFAULT_SAMPLE_RATE


DECODED_RE = re.compile(r"decoded_frames=(\d+)")
STATS_RE = re.compile(r"audio_stats: samples=(\d+) peak=(\d+) rms=([0-9.]+)")


def parse_snr_list(value: str) -> list[float | None]:
    out: list[float | None] = []
    for item in value.split(","):
        item = item.strip().lower()
        if not item:
            continue
        if item in {"clean", "none", "inf"}:
            out.append(None)
        else:
            out.append(float(item))
    if not out:
        raise argparse.ArgumentTypeError("SNR-lijst is leeg")
    return out


def add_awgn_iq(in_path: Path, out_path: Path, snr_db: float, seed: int) -> None:
    raw = in_path.read_bytes()
    iq = u8_iq_to_complex(raw)
    if not iq.size:
        raise RuntimeError(f"geen IQ samples in {in_path}")

    signal_power = float(np.mean(np.abs(iq) ** 2))
    noise_power = signal_power / (10 ** (snr_db / 10))
    rng = np.random.default_rng(seed)
    sigma = (noise_power / 2.0) ** 0.5
    noise = (
        rng.normal(0.0, sigma, iq.size) + 1j * rng.normal(0.0, sigma, iq.size)
    ).astype(np.complex64)
    out_path.write_bytes(complex_to_u8_iq(iq + noise, amplitude=1.0))


def prbs_pcm_block(frame_index: int, state: int) -> tuple[np.ndarray, int]:
    values = np.empty(PCM_VALUES_PER_NICAM_FRAME, dtype=np.uint16)
    for sample_index in range(PCM_VALUES_PER_NICAM_FRAME):
        bit = ((state >> 0) ^ (state >> 2) ^ (state >> 3) ^ (state >> 5)) & 1
        state = ((state >> 1) | (bit << 15)) & 0xFFFF
        values[sample_index] = (
            state ^ (frame_index * 257 + sample_index * 73)
        ) & 0xFFFF
    return values.view(np.int16), state


def generate_prbs_iq(args: argparse.Namespace, iq_path: Path, expected_path: Path) -> None:
    frames = int(args.seconds * 1000)
    expected = np.empty(frames * PCM_VALUES_PER_NICAM_FRAME, dtype=np.int16)
    state = 0xACE1
    phase_quarter = 0
    j17_pre = J17Preemphasis()
    j17_de = J17Deemphasis()
    amplitude = (
        args.amplitude
        if args.amplitude is not None
        else (10 ** (args.nicam_level_db / 20.0) if args.nicam_level_db is not None else args.nicam_rf_level / 1023.0)
    )

    with iq_path.open("wb") as out:
        for frame_index in range(frames):
            pcm, state = prbs_pcm_block(frame_index, state)
            payload = pcm16_to_nicam_payload_j17(pcm, j17_pre)
            start = frame_index * PCM_VALUES_PER_NICAM_FRAME
            decoded_preemphasized = nicam_payload_to_pcm16(payload)
            expected[start : start + PCM_VALUES_PER_NICAM_FRAME] = j17_de.process(decoded_preemphasized)
            bits = build_frame(payload, frame_index=frame_index, mode=0, fallback=0)
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
            out.write(complex_to_u8_iq(iq, amplitude=amplitude))
    np.save(expected_path, expected)


def compare_expected_audio(expected_path: Path, pcm_path: Path) -> tuple[int, int, int, int]:
    expected = np.load(expected_path)
    got = np.frombuffer(pcm_path.read_bytes(), dtype=np.int16).copy()

    expected_frames = expected.size // PCM_VALUES_PER_NICAM_FRAME
    got_frames = got.size // PCM_VALUES_PER_NICAM_FRAME
    if expected_frames <= 0 or got_frames <= 0:
        return 0, 0, 0, 0

    expected_blocks = expected[: expected_frames * PCM_VALUES_PER_NICAM_FRAME].reshape(
        expected_frames, PCM_VALUES_PER_NICAM_FRAME
    )
    got_blocks = got[: got_frames * PCM_VALUES_PER_NICAM_FRAME].reshape(
        got_frames, PCM_VALUES_PER_NICAM_FRAME
    )

    errors = 0
    max_abs = 0
    skipped = 0
    expected_index = 0
    search_ahead = 12
    for block in got_blocks:
        if expected_index >= expected_frames:
            errors += PCM_VALUES_PER_NICAM_FRAME
            continue

        stop = min(expected_frames, expected_index + search_ahead + 1)
        best_index = expected_index
        best_errors = PCM_VALUES_PER_NICAM_FRAME + 1
        best_max_abs = 0
        for candidate_index in range(expected_index, stop):
            diff = block.astype(np.int32) - expected_blocks[candidate_index].astype(np.int32)
            candidate_errors = int(np.count_nonzero(diff))
            candidate_max_abs = int(np.max(np.abs(diff))) if diff.size else 0
            if (candidate_errors, candidate_max_abs) < (best_errors, best_max_abs):
                best_index = candidate_index
                best_errors = candidate_errors
                best_max_abs = candidate_max_abs
                if best_errors == 0:
                    break

        skipped += best_index - expected_index
        errors += best_errors
        max_abs = max(max_abs, best_max_abs)
        expected_index = best_index + 1

    compared = got_frames * PCM_VALUES_PER_NICAM_FRAME
    return errors, max_abs, skipped, compared


def run_cmd(cmd: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def run_case(args: argparse.Namespace, iq_path: Path, pcm_path: Path) -> tuple[int, str]:
    if args.payload_format != "nicam728":
        raise SystemExit("./nicam ondersteunt alleen --payload-format nicam728")
    rx_cmd = [
        "./nicam",
        "--sample-rate",
        str(args.sample_rate),
        "--adaptive-fixed",
        "--timing-search-steps",
        "1",
        "--chunk-bytes",
        "65536",
        "--verbose",
    ]
    if args.rx_matched_filter:
        rx_cmd.extend(
            [
                "--matched-filter",
                "--matched-rolloff",
                str(args.rx_matched_rolloff),
                "--matched-span-symbols",
                str(args.rx_matched_span_symbols),
            ]
        )
    with iq_path.open("rb") as source, pcm_path.open("wb") as out:
        proc = subprocess.run(
            rx_cmd,
            stdin=source,
            stdout=out,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
    return proc.returncode, proc.stderr


def summarize_stderr(stderr: str) -> tuple[int, str, str]:
    decoded_match = DECODED_RE.search(stderr)
    stats_match = STATS_RE.search(stderr)
    lost_lock = "yes" if "lost lock" in stderr else "no"
    decoded = int(decoded_match.group(1)) if decoded_match else 0
    stats = (
        f"samples={stats_match.group(1)} peak={stats_match.group(2)} rms={stats_match.group(3)}"
        if stats_match
        else "samples=0 peak=0 rms=0"
    )
    return decoded, lost_lock, stats


def run(args: argparse.Namespace) -> int:
    snrs = parse_snr_list(args.snr_db)
    if (
        args.nicam_rf_level is None
        and args.nicam_level_db is None
        and args.amplitude is None
    ):
        args.nicam_rf_level = 200
    expected_frames = int(args.seconds * 1000)
    with tempfile.TemporaryDirectory(prefix="nicam-loop-") as tmp:
        tmp_path = Path(tmp)
        clean_iq = tmp_path / "clean.iq"
        expected_path = tmp_path / "expected.npy"
        if args.pattern == "prbs":
            if args.payload_format != "nicam728":
                raise SystemExit("--pattern prbs ondersteunt nu alleen --payload-format nicam728")
            generate_prbs_iq(args, clean_iq, expected_path)
        else:
            tx_cmd = [
                sys.executable,
                "-m",
                "nicam.stream_tx",
                "--tone",
                "--tone-hz",
                str(args.tone_hz),
                "--seconds",
                str(args.seconds),
                "--sample-rate",
                str(args.sample_rate),
                "--payload-format",
                args.payload_format,
                "--out",
                str(clean_iq),
            ]
            if args.pulse_shape:
                tx_cmd.extend(
                    [
                        "--pulse-shape",
                        "--pulse-rolloff",
                        str(args.pulse_rolloff),
                        "--pulse-span-symbols",
                        str(args.pulse_span_symbols),
                    ]
                )
            if args.nicam_level_db is not None:
                tx_cmd.extend(["--nicam-level-db", str(args.nicam_level_db)])
            elif args.nicam_rf_level is not None:
                tx_cmd.extend(["--nicam-rf-level", str(args.nicam_rf_level)])
            elif args.amplitude is not None:
                tx_cmd.extend(["--amplitude", str(args.amplitude)])
            tx = run_cmd(tx_cmd)
            if tx.returncode:
                sys.stderr.write(tx.stderr)
                return tx.returncode

        print(
            "case,snr_db,decoded_frames,expected_frames,lost_lock,compare_errors,compare_offset,audio_stats",
            flush=True,
        )
        failures = 0
        for index, snr in enumerate(snrs):
            if snr is None:
                iq_path = clean_iq
                label = "clean"
            else:
                iq_path = tmp_path / f"snr-{snr:g}.iq"
                add_awgn_iq(clean_iq, iq_path, snr, args.seed + index)
                label = f"{snr:g}"

            pcm_path = tmp_path / f"out-{label}.pcm"
            returncode, stderr = run_case(args, iq_path, pcm_path)
            decoded, lost_lock, stats = summarize_stderr(stderr)
            compare_errors = ""
            compare_offset = ""
            if args.compare and args.pattern == "prbs":
                errors, _max_abs, frame_offset, _compared = compare_expected_audio(
                    expected_path, pcm_path
                )
                compare_errors = str(errors)
                compare_offset = str(frame_offset)
            print(
                f"{index},{label},{decoded},{expected_frames},{lost_lock},{compare_errors},{compare_offset},{stats}",
                flush=True,
            )
            if args.verbose:
                sys.stderr.write(f"\n--- {label} dB ---\n{stderr}")
            if returncode or decoded < max(1, expected_frames - args.allowed_missing_frames):
                failures += 1
            if args.compare and args.pattern == "prbs" and compare_errors != "0":
                failures += 1

    return 1 if failures else 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run a closed NICAM TX/RX IQ baseband loop test")
    parser.add_argument("--seconds", type=float, default=1.0)
    parser.add_argument("--pattern", choices=["tone", "prbs"], default="tone")
    parser.add_argument("--compare", action="store_true")
    parser.add_argument("--tone-hz", type=float, default=1000.0)
    parser.add_argument("--sample-rate", type=int, default=DEFAULT_SAMPLE_RATE)
    parser.add_argument("--pulse-shape", action="store_true")
    parser.add_argument("--pulse-rolloff", type=float, default=0.4)
    parser.add_argument("--pulse-span-symbols", type=int, default=6)
    parser.add_argument("--rx-matched-filter", action="store_true")
    parser.add_argument("--rx-matched-rolloff", type=float, default=0.4)
    parser.add_argument("--rx-matched-span-symbols", type=int, default=6)
    parser.add_argument("--payload-format", choices=["nicam728", "lab"], default="nicam728")
    parser.add_argument("--nicam-rf-level", type=int)
    parser.add_argument("--nicam-level-db", type=float)
    parser.add_argument("--amplitude", type=float)
    parser.add_argument("--snr-db", default="clean,24,18,12,9,6")
    parser.add_argument("--seed", type=int, default=12345)
    parser.add_argument("--allowed-missing-frames", type=int, default=2)
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    return run(parse_args(sys.argv[1:] if argv is None else argv))


if __name__ == "__main__":
    raise SystemExit(main())
