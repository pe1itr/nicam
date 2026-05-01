from __future__ import annotations

import argparse
import subprocess
import sys
import time
import wave
from collections import deque
from typing import BinaryIO

import numpy as np

from .audio_payload import AUDIO_SAMPLE_RATE, CHANNELS, payload_parity_errors, payload_to_pcm16
from .constants import FRAME_BITS, SYMBOL_RATE
from .dqpsk import symbols_to_bits
from .frame import FAW, descramble_frame, find_frame_offsets
from .iq import u8_iq_to_complex
from .pipe import suppress_stdout_broken_pipe
from .qpsk_dsp import demap_symbols_to_bits
from .rtlsdr_rx import DEFAULT_SAMPLE_RATE


def open_iq_input(args: argparse.Namespace) -> tuple[BinaryIO, subprocess.Popen[bytes] | None]:
    if args.freq is not None:
        if args.gain == "0":
            print(
                "Waarschuwing: rtl_sdr behandelt -g 0 als auto-gain. "
                "Gebruik bijvoorbeeld --gain 0.9 voor lage handmatige gain.",
                file=sys.stderr,
            )
        cmd = [
            "rtl_sdr",
            "-d",
            str(args.device_index),
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

    if args.iq_in == "-":
        return sys.stdin.buffer, None
    return open(args.iq_in, "rb"), None


def open_audio_output(path: str):
    if path == "-":
        return sys.stdout.buffer, None

    if path.lower().endswith(".wav"):
        wav = wave.open(path, "wb")
        wav.setnchannels(CHANNELS)
        wav.setsampwidth(2)
        wav.setframerate(AUDIO_SAMPLE_RATE)
        return wav, wav

    return open(path, "wb"), None


def write_audio(out, pcm: np.ndarray, flush: bool = False) -> None:
    data = np.asarray(pcm, dtype=np.int16).tobytes()
    if isinstance(out, wave.Wave_write):
        out.writeframesraw(data)
    else:
        out.write(data)
        if flush:
            out.flush()


def read_exact(source: BinaryIO, size: int) -> bytes:
    chunks: list[bytes] = []
    remaining = size
    while remaining > 0:
        chunk = source.read(remaining)
        if not chunk:
            break
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def estimate_rf_metrics(iq: np.ndarray) -> tuple[float, float] | None:
    data = np.asarray(iq, dtype=np.complex64)
    if data.size < 1024:
        return None

    n = 1024
    while (n * 2) <= data.size and n < 16384:
        n *= 2

    seg = data[:n]
    power = float(np.mean(np.abs(seg) ** 2))
    signal_dbfs = 10.0 * np.log10(max(power, 1e-12))

    window = np.hanning(n).astype(np.float32)
    spectrum = np.abs(np.fft.fftshift(np.fft.fft(seg * window))) ** 2
    if spectrum.size < 16:
        return None
    peak_idx = int(np.argmax(spectrum))
    guard = max(4, n // 256)
    lo = max(0, peak_idx - guard)
    hi = min(spectrum.size, peak_idx + guard + 1)
    mask = np.ones(spectrum.size, dtype=bool)
    mask[lo:hi] = False
    if not np.any(mask):
        return None
    noise = float(np.median(spectrum[mask]))
    peak = float(spectrum[peak_idx])
    snr_db = 10.0 * np.log10(max(peak - noise, 1e-12) / max(noise, 1e-12))
    return signal_dbfs, snr_db


def maybe_print_rf_metrics(iq: np.ndarray, next_report_at: float, interval_s: float) -> float:
    now = time.monotonic()
    if now < next_report_at:
        return next_report_at
    metrics = estimate_rf_metrics(iq)
    if metrics is not None:
        signal_dbfs, snr_db = metrics
        print(
            f"rf_stats: level={signal_dbfs:.1f} dBFS snr_est={snr_db:.1f} dB",
            file=sys.stderr,
        )
    return now + interval_s


def decode_frame(raw_bits: np.ndarray, max_faw_errors: int):
    raw = np.asarray(raw_bits, dtype=np.uint8)
    if max_faw_errors > 0:
        faw_errors = int(np.count_nonzero(raw[: FAW.size] ^ FAW))
        if faw_errors <= max_faw_errors:
            raw = raw.copy()
            raw[: FAW.size] = FAW
    return descramble_frame(raw)


def symbols_to_bits_with_carrier_tracking(
    symbols: np.ndarray,
    previous_symbol: complex | np.complex64 | None,
) -> tuple[np.ndarray, float]:
    z = np.asarray(symbols, dtype=np.complex64)
    if previous_symbol is not None:
        z = np.concatenate([np.array([previous_symbol], dtype=np.complex64), z])
    if z.size < 2:
        return np.empty(0, dtype=np.uint8), 0.0

    phase_delta = np.angle(z[1:] * np.conj(z[:-1]))
    nearest_quarter = np.rint(phase_delta / (np.pi / 2)) * (np.pi / 2)
    residual = np.angle(np.exp(1j * (phase_delta - nearest_quarter)))
    mean = np.mean(np.exp(1j * residual))
    carrier_step = float(np.angle(mean)) if abs(mean) > 1e-6 else 0.0

    q = np.mod(np.rint((phase_delta - carrier_step) / (np.pi / 2)).astype(np.int8), 4)
    pairs = np.empty((q.size, 2), dtype=np.uint8)
    pairs[q == 0] = (0, 0)
    pairs[q == 3] = (0, 1)
    pairs[q == 2] = (1, 1)
    pairs[q == 1] = (1, 0)
    return pairs.reshape(-1), carrier_step


def demod_symbols_to_bits(
    symbols: np.ndarray,
    previous_symbol: complex | np.complex64 | None,
    carrier_tracking: bool,
    dsp_backend: str,
) -> tuple[np.ndarray, float]:
    if carrier_tracking:
        return symbols_to_bits_with_carrier_tracking(symbols, previous_symbol)
    bits, _ = demap_symbols_to_bits(symbols, previous_symbol, dsp_backend)
    return bits, 0.0


class PhaseDemodState:
    def __init__(self, phase: int, sample_rate: int, freq_offset: float):
        self.phase = phase
        self.sample_rate = sample_rate
        self.freq_offset = freq_offset
        self.sps = int(round(sample_rate / SYMBOL_RATE))
        self.raw_tail = np.empty(0, dtype=np.complex64)
        self.started = False
        self.sample_index = 0
        self.previous_symbol: complex | np.complex64 | None = None

    def demod(
        self,
        iq: np.ndarray,
        carrier_tracking: bool,
        dsp_backend: str,
    ) -> tuple[np.ndarray, float]:
        raw = np.asarray(iq, dtype=np.complex64)
        data = np.concatenate([self.raw_tail, raw]) if self.raw_tail.size else raw
        if data.size == 0:
            return np.empty(0, dtype=np.uint8), 0.0

        corrected = data
        if self.freq_offset:
            n = np.arange(data.size, dtype=np.float64) + self.sample_index
            corrected = data * np.exp(-2j * np.pi * self.freq_offset * n / self.sample_rate)

        start = 0 if self.started else self.phase
        usable = ((corrected.size - start) // self.sps) * self.sps
        if usable <= 0:
            self.raw_tail = data
            return np.empty(0, dtype=np.uint8), 0.0

        symbols = corrected[start : start + usable].reshape(-1, self.sps).mean(axis=1)
        bits, carrier_step = demod_symbols_to_bits(
            symbols.astype(np.complex64),
            self.previous_symbol,
            carrier_tracking=carrier_tracking,
            dsp_backend=dsp_backend,
        )
        if symbols.size:
            self.previous_symbol = symbols[-1]
        self.raw_tail = data[start + usable :].copy()
        self.sample_index += start + usable
        self.started = True
        return bits, carrier_step


class FractionalDemodState:
    def __init__(
        self,
        phase: float,
        sample_rate: int,
        freq_offset: float,
        symbol_ppm: float,
    ):
        self.phase = float(phase)
        self.sample_rate = sample_rate
        self.freq_offset = freq_offset
        self.omega = (sample_rate / SYMBOL_RATE) * (1.0 + symbol_ppm / 1_000_000.0)
        self.timing_gain = 0.002
        self.raw = np.empty(0, dtype=np.complex64)
        self.base_index = 0
        self.next_pos = self.phase
        self.previous_symbol: complex | np.complex64 | None = None

    def clone(self) -> "FractionalDemodState":
        other = FractionalDemodState(self.phase, self.sample_rate, self.freq_offset, 0.0)
        other.omega = self.omega
        other.timing_gain = self.timing_gain
        other.raw = self.raw.copy()
        other.base_index = self.base_index
        other.next_pos = self.next_pos
        other.previous_symbol = self.previous_symbol
        return other

    def demod(
        self,
        iq: np.ndarray,
        carrier_tracking: bool,
        dsp_backend: str,
    ) -> tuple[np.ndarray, float]:
        new = np.asarray(iq, dtype=np.complex64)
        if new.size:
            self.raw = np.concatenate([self.raw, new]) if self.raw.size else new
        if self.raw.size < 2 or self.next_pos > self.raw.size - 2:
            return np.empty(0, dtype=np.uint8), 0.0

        positions = np.arange(self.next_pos, self.raw.size - 1, self.omega, dtype=np.float64)
        if positions.size == 0:
            return np.empty(0, dtype=np.uint8), 0.0

        idx = np.floor(positions).astype(np.int64)
        frac = (positions - idx).astype(np.float32)
        samples = self.raw[idx] * (1.0 - frac) + self.raw[idx + 1] * frac
        early_pos = np.maximum(positions - self.omega * 0.25, 0.0)
        late_pos = np.minimum(positions + self.omega * 0.25, self.raw.size - 2.0)
        early_idx = np.floor(early_pos).astype(np.int64)
        late_idx = np.floor(late_pos).astype(np.int64)
        early_frac = (early_pos - early_idx).astype(np.float32)
        late_frac = (late_pos - late_idx).astype(np.float32)
        early = self.raw[early_idx] * (1.0 - early_frac) + self.raw[early_idx + 1] * early_frac
        late = self.raw[late_idx] * (1.0 - late_frac) + self.raw[late_idx + 1] * late_frac
        timing_error = float(np.mean(np.abs(late) ** 2 - np.abs(early) ** 2))
        if np.isfinite(timing_error):
            self.omega = float(np.clip(self.omega - self.timing_gain * timing_error, 4.0, 16.0))
        if self.freq_offset:
            absolute = positions + self.base_index
            samples = samples * np.exp(-2j * np.pi * self.freq_offset * absolute / self.sample_rate)

        bits, carrier_step = demod_symbols_to_bits(
            samples.astype(np.complex64),
            self.previous_symbol,
            carrier_tracking=carrier_tracking,
            dsp_backend=dsp_backend,
        )
        if samples.size:
            self.previous_symbol = samples[-1]

        self.next_pos = float(positions[-1] + self.omega)
        drop = max(0, int(np.floor(self.next_pos)) - 2)
        if drop:
            self.raw = self.raw[drop:].copy()
            self.base_index += drop
            self.next_pos -= drop
        return bits, carrier_step


def parse_ppm_candidates(spec: str) -> list[float]:
    return [float(part.strip()) for part in spec.split(",") if part.strip()]


def make_stable_states(args: argparse.Namespace, sps: int):
    if not args.fractional_timing:
        return [
            (phase, 0.0, PhaseDemodState(phase, args.sample_rate, args.freq_offset))
            for phase in range(sps)
        ]

    ppms = parse_ppm_candidates(args.symbol_ppm_candidates)
    return [
        (
            phase,
            ppm,
            FractionalDemodState(phase, args.sample_rate, args.freq_offset, ppm),
        )
        for ppm in ppms
        for phase in range(sps)
    ]


def score_candidate_stream(
    initial_bits: np.ndarray,
    offset: int,
    state,
    lookahead_iq: list[np.ndarray],
    args: argparse.Namespace,
) -> tuple[int, int, int, object]:
    trial = state.clone() if hasattr(state, "clone") else state
    chunks = [np.asarray(initial_bits[offset:], dtype=np.uint8)]
    score = 0
    good = 0
    decoded = 0

    for iq in lookahead_iq:
        bits, _ = trial.demod(
            iq,
            carrier_tracking=not args.no_carrier_tracking,
            dsp_backend=args.dsp_backend,
        )
        chunks.append(np.asarray(bits, dtype=np.uint8))

    data = np.concatenate(chunks) if len(chunks) > 1 else chunks[0]
    pos = 0
    while pos + FRAME_BITS <= data.size:
        try:
            frame = decode_frame(data[pos : pos + FRAME_BITS], max_faw_errors=args.faw_errors)
        except ValueError:
            return score, good, decoded, trial
        pe = payload_parity_errors(frame.payload)
        score += max(0, 64 - pe)
        good += int(pe <= args.lock_max_parity_errors)
        decoded += 1
        pos += FRAME_BITS

    return score, good, decoded, trial


def run_stable(args: argparse.Namespace) -> int:
    if args.sample_rate % SYMBOL_RATE:
        raise SystemExit("--sample-rate moet een veelvoud van 364000 zijn")

    source, proc = open_iq_input(args)
    audio_out, closeable_audio = open_audio_output(args.audio_out)
    sps = int(round(args.sample_rate / SYMBOL_RATE))
    chunk_samples = args.chunk_symbols * sps
    phase_states = make_stable_states(args, sps)
    active = None
    bit_buffer: deque[int] = deque(maxlen=FRAME_BITS * 64)
    locked = False
    total_frames = 0
    bad_frames = 0
    last_pcm = np.zeros(64, dtype=np.int16)
    warmup_pcm: list[np.ndarray] = []
    audio_ready = False
    stats_samples = 0
    stats_sum_squares = 0.0
    stats_peak = 0

    pending_iq: deque[np.ndarray] = deque()
    next_rf_report_at = 0.0

    try:
        while True:
            if pending_iq:
                iq = pending_iq.popleft()
            else:
                raw = read_exact(source, chunk_samples * 2)
                if not raw:
                    break
                iq = u8_iq_to_complex(raw)
            if args.rf_snr:
                next_rf_report_at = maybe_print_rf_metrics(
                    iq,
                    next_rf_report_at,
                    args.rf_snr_interval,
                )

            if not locked:
                lookahead_iq: list[np.ndarray] = []
                for _ in range(args.acquire_lookahead):
                    raw_next = read_exact(source, chunk_samples * 2)
                    if not raw_next:
                        break
                    lookahead_iq.append(u8_iq_to_complex(raw_next))
                pending_iq.extend(lookahead_iq)

                phase_bits: list[np.ndarray] = []
                scores: list[int] = []
                locks: list[tuple[int, int] | None] = []
                stream_scores: list[tuple[int, int, int, object]] = []
                for _phase, _ppm, state in phase_states:
                    bits, _ = state.demod(
                        iq,
                        carrier_tracking=not args.no_carrier_tracking,
                        dsp_backend=args.dsp_backend,
                    )
                    phase_bits.append(bits)
                    lock = select_payload_lock_offset(
                        bits,
                        max_faw_errors=args.faw_errors,
                        max_parity_errors=args.lock_max_parity_errors,
                        min_repeats=args.min_repeats,
                        flag_frames=args.flag_frames,
                    )
                    locks.append(lock)
                    if lock is not None and lookahead_iq:
                        stream_score = score_candidate_stream(
                            bits,
                            lock[0],
                            state,
                            lookahead_iq,
                            args,
                        )
                    else:
                        stream_score = (
                            lock[1],
                            0,
                            0,
                            state.clone() if hasattr(state, "clone") else state,
                        ) if lock is not None else (0, 0, 0, state)
                    stream_scores.append(stream_score)
                    if stream_score[2] < args.min_acquire_frames:
                        scores.append(0)
                    else:
                        scores.append(stream_score[0])

                best_phase = max(
                    range(len(scores)),
                    key=lambda idx: (
                        scores[idx],
                        stream_scores[idx][1],
                        stream_scores[idx][2],
                        -abs(phase_states[idx][1]),
                        -phase_states[idx][0],
                    ),
                )
                lock = locks[best_phase]
                if lock is None:
                    if args.verbose:
                        print(f"no lock scores={scores}", file=sys.stderr)
                    continue

                offset, lock_score = lock
                active_phase, active_ppm, _ = phase_states[best_phase]
                active = stream_scores[best_phase][3]
                locked = True
                bad_frames = 0
                audio_ready = False
                warmup_pcm.clear()
                bit_buffer.clear()
                bit_buffer.extend(int(x) for x in phase_bits[best_phase][offset:])
                if args.verbose:
                    print(
                        f"locked: timing_phase={active_phase} ppm={active_ppm} "
                        f"score={lock_score} stream_score={stream_scores[best_phase][:3]} "
                        f"scores={scores}",
                        file=sys.stderr,
                    )
            else:
                assert active is not None
                bits, carrier_step = active.demod(
                    iq,
                    carrier_tracking=not args.no_carrier_tracking,
                    dsp_backend=args.dsp_backend,
                )
                if args.verbose and abs(carrier_step) > 0.05:
                    carrier_hz = carrier_step * SYMBOL_RATE / (2 * np.pi)
                    print(f"carrier_step_hz={carrier_hz:.0f}", file=sys.stderr)
                bit_buffer.extend(int(x) for x in bits)

            while len(bit_buffer) >= FRAME_BITS:
                data = np.fromiter((bit_buffer[idx] for idx in range(FRAME_BITS)), dtype=np.uint8)
                try:
                    frame = decode_frame(data, max_faw_errors=args.faw_errors)
                except ValueError:
                    slip = find_forward_frame_slip(
                        bit_buffer,
                        max_search=args.tracking_search,
                        max_faw_errors=args.faw_errors,
                        min_score=args.min_track_score,
                    )
                    if slip is not None:
                        for _ in range(slip):
                            bit_buffer.popleft()
                        if args.verbose:
                            print(f"frame_slip: +{slip}", file=sys.stderr)
                        continue
                    locked = False
                    active = None
                    bit_buffer.clear()
                    warmup_pcm.clear()
                    audio_ready = False
                    phase_states = make_stable_states(args, sps)
                    if args.verbose:
                        print("lost lock: sync", file=sys.stderr)
                    break

                for _ in range(FRAME_BITS):
                    bit_buffer.popleft()

                parity_errors = payload_parity_errors(frame.payload)
                pcm = payload_to_pcm16(frame.payload)
                if parity_errors > args.max_parity_errors:
                    bad_frames += 1
                    if args.verbose:
                        print(
                            f"bad frame: parity_errors={parity_errors} bad_frames={bad_frames}",
                            file=sys.stderr,
                        )
                    if bad_frames >= args.max_bad_frames:
                        locked = False
                        active = None
                        bit_buffer.clear()
                        phase_states = make_stable_states(args, sps)
                        warmup_pcm.clear()
                        audio_ready = False
                        if args.verbose:
                            print("lost lock: parity", file=sys.stderr)
                        break
                    if args.conceal_bad_frames:
                        pcm = last_pcm
                else:
                    bad_frames = 0
                    last_pcm = pcm

                if not audio_ready:
                    warmup_pcm.append(pcm)
                    if len(warmup_pcm) < args.lock_warmup_frames:
                        continue
                    audio_ready = True
                    for buffered_pcm in warmup_pcm:
                        if args.stats:
                            pcm64 = buffered_pcm.astype(np.float64)
                            stats_samples += buffered_pcm.size
                            stats_sum_squares += float(np.sum(pcm64 * pcm64))
                            stats_peak = max(stats_peak, int(np.max(np.abs(buffered_pcm))))
                        try:
                            write_audio(
                                audio_out,
                                buffered_pcm,
                                flush=False,
                            )
                        except BrokenPipeError:
                            suppress_stdout_broken_pipe()
                            return 0
                        total_frames += 1
                    warmup_pcm.clear()
                    continue

                if args.stats:
                    pcm64 = pcm.astype(np.float64)
                    stats_samples += pcm.size
                    stats_sum_squares += float(np.sum(pcm64 * pcm64))
                    stats_peak = max(stats_peak, int(np.max(np.abs(pcm))))
                try:
                    write_audio(
                        audio_out,
                        pcm,
                        flush=total_frames % args.flush_frames == args.flush_frames - 1,
                    )
                except BrokenPipeError:
                    suppress_stdout_broken_pipe()
                    return 0
                total_frames += 1
    finally:
        source.close()
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=2)
        if closeable_audio is not None:
            closeable_audio.close()

    if args.stats and stats_samples:
        rms = (stats_sum_squares / stats_samples) ** 0.5
        print(
            f"audio_stats: samples={stats_samples} peak={stats_peak} rms={rms:.1f}",
            file=sys.stderr,
        )
    if args.verbose:
        print(f"decoded_frames={total_frames}", file=sys.stderr)
    return 0


def count_decodable_frames(
    data: np.ndarray,
    max_faw_errors: int,
    max_parity_errors: int,
    max_frames: int = 4,
) -> int:
    score = 0
    offset = 0
    count = 0
    while count < max_frames and offset + FRAME_BITS <= data.size:
        try:
            frame = decode_frame(data[offset : offset + FRAME_BITS], max_faw_errors)
        except ValueError:
            break
        parity_errors = payload_parity_errors(frame.payload)
        if parity_errors > max_parity_errors:
            break
        score += 1 + max(0, 64 - parity_errors)
        count += 1
        offset += FRAME_BITS
    return score


def select_payload_lock_offset(
    bits: np.ndarray,
    max_faw_errors: int,
    max_parity_errors: int,
    min_repeats: int,
    flag_frames: int,
) -> tuple[int, int] | None:
    data = np.asarray(bits, dtype=np.uint8)
    offsets = find_frame_offsets(data, max_errors=max_faw_errors, min_repeats=min_repeats)
    if not offsets:
        return None

    best_offset: int | None = None
    best_score = -1
    for offset in offsets:
        score = 0
        good_frames = 0
        for idx in range(flag_frames):
            start = offset + idx * FRAME_BITS
            stop = start + FRAME_BITS
            if stop > data.size:
                break
            try:
                frame = decode_frame(data[start:stop], max_faw_errors)
            except ValueError:
                break
            parity_errors = payload_parity_errors(frame.payload)
            if parity_errors > max_parity_errors:
                break
            good_frames += 1
            score += 1 + max(0, 64 - parity_errors)

        if good_frames >= min_repeats and score > best_score:
            best_offset = offset
            best_score = score

    if best_offset is None:
        return None
    return best_offset, best_score


def score_frame_at(data: np.ndarray, offset: int, max_faw_errors: int) -> tuple[int, object | None]:
    if offset < 0 or offset + FRAME_BITS > data.size:
        return -1, None
    try:
        frame = decode_frame(data[offset : offset + FRAME_BITS], max_faw_errors)
    except ValueError:
        return -1, None
    parity_errors = payload_parity_errors(frame.payload)
    return max(0, 64 - parity_errors), frame


def select_tracking_offset(
    data: np.ndarray,
    expected_offset: int,
    search_bits: int,
    max_faw_errors: int,
    min_score: int,
) -> tuple[int, object, int] | None:
    if search_bits <= 0:
        return None
    best: tuple[int, object, int] | None = None
    best_score = -1
    start = max(0, expected_offset - search_bits)
    stop = min(data.size - FRAME_BITS, expected_offset + search_bits)
    for offset in range(start, stop + 1):
        score, frame = score_frame_at(data, offset, max_faw_errors)
        if frame is not None and score > best_score:
            best = (offset, frame, score)
            best_score = score
    if best is None or best_score < min_score:
        return None
    return best


def find_forward_frame_slip(
    bits: deque[int],
    max_search: int,
    max_faw_errors: int,
    min_score: int,
) -> int | None:
    if len(bits) < FRAME_BITS + 1:
        return None
    data = np.fromiter(bits, dtype=np.uint8)
    max_offset = min(max_search, data.size - FRAME_BITS)
    for offset in range(1, max_offset + 1):
        score, _frame = score_frame_at(data, offset, max_faw_errors)
        if score >= min_score:
            return offset
    return None


def coarse_stream_symbols(
    iq: np.ndarray,
    sample_rate: int,
    timing_phase: int,
    freq_offset: float,
    tail: np.ndarray,
    started: bool,
    freq_index: int,
) -> tuple[np.ndarray, np.ndarray, bool, int]:
    samples_per_symbol = sample_rate / SYMBOL_RATE
    if abs(samples_per_symbol - round(samples_per_symbol)) > 1e-9:
        raise ValueError("sample_rate must be an integer multiple of 364000")
    sps = int(round(samples_per_symbol))

    raw_data = (
        np.concatenate([tail, np.asarray(iq, dtype=np.complex64)])
        if tail.size
        else np.asarray(iq, dtype=np.complex64)
    )
    data = raw_data
    if freq_offset:
        n = np.arange(data.size, dtype=np.float64) + freq_index
        data = data * np.exp(-2j * np.pi * freq_offset * n / sample_rate)

    start = 0 if started else timing_phase
    usable = ((data.size - start) // sps) * sps
    if usable <= 0:
        return np.empty(0, dtype=np.complex64), raw_data, started, freq_index

    sliced = data[start : start + usable].reshape(-1, sps)
    next_index = freq_index + start + usable
    return (
        sliced.mean(axis=1).astype(np.complex64),
        raw_data[start + usable :].copy(),
        True,
        next_index,
    )


def run(args: argparse.Namespace) -> int:
    if args.sample_rate % SYMBOL_RATE:
        raise SystemExit("--sample-rate moet een veelvoud van 364000 zijn")

    source, proc = open_iq_input(args)
    audio_out, closeable_audio = open_audio_output(args.audio_out)
    sps = int(round(args.sample_rate / SYMBOL_RATE))
    chunk_samples = args.chunk_symbols * sps
    previous_symbols: list[complex | np.complex64 | None] = [None] * sps
    sample_tails = [np.empty(0, dtype=np.complex64) for _ in range(sps)]
    sample_started = [False] * sps
    freq_indices = [0] * sps
    bit_buffer: deque[int] = deque(maxlen=FRAME_BITS * 24)
    total_frames = 0
    locked = False
    locked_phase: int | None = None
    chunk_index = 0
    bad_frames = 0
    last_pcm = np.zeros(64, dtype=np.int16)
    stats_samples = 0
    stats_sum_squares = 0.0
    stats_peak = 0
    next_rf_report_at = 0.0

    try:
        while True:
            raw = read_exact(source, chunk_samples * 2)
            if not raw:
                break
            if len(raw) < chunk_samples * 2:
                if args.verbose:
                    print("short final IQ read", file=sys.stderr)
            chunk_index += 1

            iq = u8_iq_to_complex(raw)
            if args.rf_snr:
                next_rf_report_at = maybe_print_rf_metrics(
                    iq,
                    next_rf_report_at,
                    args.rf_snr_interval,
                )
            if args.timing_phase is None:
                if locked and locked_phase is not None:
                    candidates = [
                        coarse_stream_symbols(
                            iq,
                            args.sample_rate,
                            phase,
                            args.freq_offset,
                            sample_tails[phase],
                            sample_started[phase],
                            freq_indices[phase],
                        )
                        for phase in range(sps)
                    ]
                    symbols_by_phase = [candidate[0] for candidate in candidates]
                    demod_by_phase = [
                        demod_symbols_to_bits(
                            symbols,
                            previous_symbols[phase],
                            carrier_tracking=not args.no_carrier_tracking,
                            dsp_backend=args.dsp_backend,
                        )
                        for phase, symbols in enumerate(symbols_by_phase)
                    ]
                    bits_by_phase = [demod[0] for demod in demod_by_phase]
                    prefix = np.fromiter(bit_buffer, dtype=np.uint8)
                    score_prefix = (
                        prefix[args.tracking_search :]
                        if prefix.size > args.tracking_search
                        else prefix
                    )
                    scores = [
                        count_decodable_frames(
                            np.concatenate([score_prefix, bits]),
                            max_faw_errors=args.faw_errors,
                            max_parity_errors=args.max_parity_errors,
                        )
                        for bits in bits_by_phase
                    ]
                    best_score = max(scores)
                    if best_score > 0:
                        current_score = scores[locked_phase]
                        best_phase = int(np.argmax(scores))
                        if (
                            current_score > 0
                            and best_phase != locked_phase
                            and best_score < current_score + args.timing_switch_margin
                        ):
                            phase = locked_phase
                        else:
                            phase = best_phase
                        bits = bits_by_phase[phase]
                        if phase != locked_phase and args.verbose:
                            print(f"timing_phase={phase} scores={scores}", file=sys.stderr)
                        if args.verbose and abs(demod_by_phase[phase][1]) > 0.05:
                            carrier_hz = (
                                demod_by_phase[phase][1] * SYMBOL_RATE / (2 * np.pi)
                            )
                            print(f"carrier_step_hz={carrier_hz:.0f}", file=sys.stderr)
                        locked_phase = phase
                    else:
                        phase = locked_phase
                        bits = bits_by_phase[phase]
                        if args.verbose:
                            print(
                                f"timing_miss: chunk={chunk_index} phase={phase} scores={scores}",
                                file=sys.stderr,
                            )
                    for idx, symbols in enumerate(symbols_by_phase):
                        if symbols.size:
                            previous_symbols[idx] = symbols[-1]
                        sample_tails[idx] = candidates[idx][1]
                        sample_started[idx] = candidates[idx][2]
                        freq_indices[idx] = candidates[idx][3]
                else:
                    candidates = [
                        coarse_stream_symbols(
                            iq,
                            args.sample_rate,
                            phase,
                            args.freq_offset,
                            sample_tails[phase],
                            sample_started[phase],
                            freq_indices[phase],
                        )
                        for phase in range(sps)
                    ]
                    symbols_by_phase = [candidate[0] for candidate in candidates]
                    demod_by_phase = [
                        demod_symbols_to_bits(
                            symbols,
                            previous_symbols[phase],
                            carrier_tracking=not args.no_carrier_tracking,
                            dsp_backend=args.dsp_backend,
                        )
                        for phase, symbols in enumerate(symbols_by_phase)
                    ]
                    bits_by_phase = [demod[0] for demod in demod_by_phase]
                    scores = [
                        (
                            lock[1]
                            if (
                                lock := select_payload_lock_offset(
                                    bits,
                                    max_faw_errors=args.faw_errors,
                                    max_parity_errors=args.max_parity_errors,
                                    min_repeats=args.min_repeats,
                                    flag_frames=args.flag_frames,
                                )
                            )
                            is not None
                            else 0
                        )
                        for bits in bits_by_phase
                    ]
                    phase = int(np.argmax(scores))
                    bits = bits_by_phase[phase]
                    for idx, symbols in enumerate(symbols_by_phase):
                        if symbols.size:
                            previous_symbols[idx] = symbols[-1]
                        sample_tails[idx] = candidates[idx][1]
                        sample_started[idx] = candidates[idx][2]
                        freq_indices[idx] = candidates[idx][3]
            else:
                phase = args.timing_phase
                if phase < 0 or phase >= sps:
                    raise SystemExit(f"--timing-phase moet tussen 0 en {sps - 1} liggen")
                (
                    symbols,
                    sample_tails[phase],
                    sample_started[phase],
                    freq_indices[phase],
                ) = coarse_stream_symbols(
                    iq,
                    args.sample_rate,
                    phase,
                    args.freq_offset,
                    sample_tails[phase],
                    sample_started[phase],
                    freq_indices[phase],
                )
                bits, carrier_step = demod_symbols_to_bits(
                    symbols,
                    previous_symbols[phase],
                    carrier_tracking=not args.no_carrier_tracking,
                    dsp_backend=args.dsp_backend,
                )
                if args.verbose and abs(carrier_step) > 0.05:
                    carrier_hz = carrier_step * SYMBOL_RATE / (2 * np.pi)
                    print(f"carrier_step_hz={carrier_hz:.0f}", file=sys.stderr)
                if symbols.size:
                    previous_symbols[phase] = symbols[-1]

            bit_buffer.extend(int(x) for x in bits)
            data = np.fromiter(bit_buffer, dtype=np.uint8)
            if locked:
                offset = min(args.tracking_search, max(0, data.size - FRAME_BITS))
            else:
                lock = select_payload_lock_offset(
                    data,
                    max_faw_errors=args.faw_errors,
                    max_parity_errors=args.lock_max_parity_errors,
                    min_repeats=args.min_repeats,
                    flag_frames=args.flag_frames,
                )
                if lock is None:
                    if args.verbose:
                        print("no lock", file=sys.stderr)
                    continue
                offset, lock_score = lock

                if args.verbose:
                    print(f"locked: timing_phase={phase} score={lock_score}", file=sys.stderr)
                locked = True
                locked_phase = phase
                bad_frames = 0

            while offset + FRAME_BITS <= data.size:
                tracked = (
                    select_tracking_offset(
                        data,
                        expected_offset=offset,
                        search_bits=args.tracking_search,
                        max_faw_errors=args.faw_errors,
                        min_score=args.min_track_score,
                    )
                    if locked
                    else None
                )
                if tracked is not None:
                    tracked_offset, frame, frame_score = tracked
                    if tracked_offset != offset and args.verbose:
                        print(
                            f"track_slip: {tracked_offset - offset:+d} score={frame_score}",
                            file=sys.stderr,
                        )
                    offset = tracked_offset
                else:
                    try:
                        frame = decode_frame(
                            data[offset : offset + FRAME_BITS],
                            max_faw_errors=args.faw_errors,
                        )
                    except ValueError:
                        if args.ignore_sync_errors:
                            raw_frame = data[offset : offset + FRAME_BITS].copy()
                            raw_frame[: FAW.size] = FAW
                            frame = descramble_frame(raw_frame)
                            if args.verbose:
                                print("bad frame: sync", file=sys.stderr)
                        else:
                            locked = False
                            locked_phase = None
                            if args.verbose:
                                print("lost lock", file=sys.stderr)
                            break
                pcm = payload_to_pcm16(frame.payload)
                parity_errors = payload_parity_errors(frame.payload)
                if parity_errors > args.max_parity_errors:
                    bad_frames += 1
                    if args.verbose:
                        print(
                            f"bad frame: parity_errors={parity_errors} "
                            f"bad_frames={bad_frames}",
                            file=sys.stderr,
                        )
                    if args.ignore_parity_errors:
                        bad_frames = 0
                    elif bad_frames >= args.max_bad_frames:
                        locked = False
                        locked_phase = None
                        if args.verbose:
                            print("lost lock", file=sys.stderr)
                        break
                    if args.conceal_bad_frames:
                        pcm = last_pcm
                else:
                    bad_frames = 0
                    last_pcm = pcm
                if args.stats:
                    pcm64 = pcm.astype(np.float64)
                    stats_samples += pcm.size
                    stats_sum_squares += float(np.sum(pcm64 * pcm64))
                    stats_peak = max(stats_peak, int(np.max(np.abs(pcm))))
                try:
                    write_audio(
                        audio_out,
                        pcm,
                        flush=total_frames % args.flush_frames == args.flush_frames - 1,
                    )
                except BrokenPipeError:
                    suppress_stdout_broken_pipe()
                    return 0
                total_frames += 1
                offset += FRAME_BITS

            keep_from = min(offset, data.size)
            if locked and keep_from > args.tracking_search:
                keep_from -= args.tracking_search
            bit_buffer = deque((int(x) for x in data[keep_from:]), maxlen=FRAME_BITS * 24)
    finally:
        source.close()
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=2)
        if closeable_audio is not None:
            closeable_audio.close()

    if args.stats and stats_samples:
        rms = (stats_sum_squares / stats_samples) ** 0.5
        print(
            f"audio_stats: samples={stats_samples} peak={stats_peak} rms={rms:.1f}",
            file=sys.stderr,
        )
    if args.verbose:
        print(f"decoded_frames={total_frames}", file=sys.stderr)
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Demodulate direct NICAM-like DQPSK IQ to PCM audio")
    parser.add_argument("--iq-in", default="-", help="rtl_sdr-style uint8 IQ input, or - for stdin")
    parser.add_argument("--audio-out", default="-", help="PCM/WAV output, or - for stdout")
    parser.add_argument("--freq", type=int, help="start rtl_sdr at this RF center frequency in Hz")
    parser.add_argument(
        "--device-index",
        type=int,
        default=0,
        help="rtl_sdr device index when --freq is used (default: 0)",
    )
    parser.add_argument("--sample-rate", type=int, default=DEFAULT_SAMPLE_RATE)
    parser.add_argument("--ppm", type=int, default=0)
    parser.add_argument("--gain", default="auto")
    parser.add_argument("--freq-offset", type=float, default=0.0)
    parser.add_argument("--chunk-symbols", type=int, default=8192)
    parser.add_argument("--timing-phase", type=int)
    parser.add_argument("--faw-errors", type=int, default=0)
    parser.add_argument("--lock-max-parity-errors", type=int, default=8)
    parser.add_argument("--max-parity-errors", type=int, default=16)
    parser.add_argument("--max-bad-frames", type=int, default=8)
    parser.add_argument("--conceal-bad-frames", action="store_true")
    parser.add_argument("--ignore-parity-errors", action="store_true")
    parser.add_argument("--ignore-sync-errors", action="store_true")
    parser.add_argument("--timing-switch-margin", type=int, default=64)
    parser.add_argument("--tracking-search", type=int, default=32)
    parser.add_argument("--min-track-score", type=int, default=56)
    parser.add_argument("--min-repeats", type=int, default=3)
    parser.add_argument("--flag-frames", type=int, default=16)
    parser.add_argument("--stats", action="store_true")
    parser.add_argument(
        "--rf-snr",
        action="store_true",
        help="print periodic RF level and peak-over-noise SNR estimate",
    )
    parser.add_argument(
        "--rf-snr-interval",
        type=float,
        default=1.0,
        help="seconds between RF stats prints (default: 1.0)",
    )
    parser.add_argument(
        "--dsp-backend",
        choices=["py", "c"],
        default="py",
        help="DSP backend for QPSK demapping",
    )
    parser.add_argument("--flush-frames", type=int, default=20)
    parser.add_argument("--no-carrier-tracking", action="store_true")
    parser.add_argument("--stable-demod", action="store_true")
    parser.add_argument("--fractional-timing", action="store_true")
    parser.add_argument("--symbol-ppm", type=float, default=0.0)
    parser.add_argument("--symbol-ppm-candidates", default="-100,-50,-25,0,25,50,100")
    parser.add_argument("--acquire-lookahead", type=int, default=1)
    parser.add_argument("--min-acquire-frames", type=int, default=8)
    parser.add_argument("--lock-warmup-frames", type=int, default=12)
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    if args.stable_demod:
        return run_stable(args)
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
