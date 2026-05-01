from __future__ import annotations

import argparse
import subprocess
import sys
import time
import wave
from math import gcd
from typing import BinaryIO

import numpy as np
from scipy import signal

from nicam.iq import u8_iq_to_complex
from nicam.pipe import suppress_stdout_broken_pipe


DEFAULT_SAMPLE_RATE = 960_000
DEFAULT_MPX_RATE = 240_000
DEFAULT_AUDIO_RATE = 48_000


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


def open_iq_input(args: argparse.Namespace) -> tuple[BinaryIO, subprocess.Popen[bytes] | None]:
    if args.freq is not None:
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


def open_audio_output(path: str, audio_rate: int):
    if path == "-":
        return sys.stdout.buffer, None
    if path.lower().endswith(".wav"):
        wav = wave.open(path, "wb")
        wav.setnchannels(2)
        wav.setsampwidth(2)
        wav.setframerate(audio_rate)
        return wav, wav
    return open(path, "wb"), None


def write_pcm(out, audio: np.ndarray, gain: float, flush: bool = False) -> None:
    data = np.asarray(audio, dtype=np.float32)
    if gain != 1.0:
        data = data * float(gain)
    pcm = np.clip(np.rint(data * 32767.0), -32768, 32767).astype(np.int16)
    if isinstance(out, wave.Wave_write):
        out.writeframesraw(pcm.tobytes())
    else:
        out.write(pcm.tobytes())
        if flush:
            out.flush()


class FmDiscriminator:
    def __init__(self, sample_rate: int, deviation_hz: float) -> None:
        self.sample_rate = int(sample_rate)
        self.deviation_hz = float(deviation_hz)
        self.previous: complex | np.complex64 | None = None

    def demod(self, iq: np.ndarray) -> np.ndarray:
        data = np.asarray(iq, dtype=np.complex64)
        if data.size == 0:
            return np.empty(0, dtype=np.float32)
        if self.previous is not None:
            data = np.concatenate([np.array([self.previous], dtype=np.complex64), data])
        self.previous = data[-1]
        if data.size < 2:
            return np.empty(0, dtype=np.float32)
        phase_delta = np.angle(data[1:] * np.conj(data[:-1]))
        scale = self.sample_rate / (2.0 * np.pi * self.deviation_hz)
        return (phase_delta * scale).astype(np.float32)


class StreamingLowpass:
    def __init__(self, sample_rate: int, cutoff_hz: float, taps: int = 129) -> None:
        nyquist = sample_rate / 2.0
        cutoff = min(float(cutoff_hz), nyquist * 0.95)
        self.taps = signal.firwin(taps, cutoff / nyquist).astype(np.float64)
        self.zi = np.zeros(len(self.taps) - 1, dtype=np.float64)

    def process(self, data: np.ndarray) -> np.ndarray:
        filtered, self.zi = signal.lfilter(
            self.taps,
            [1.0],
            np.asarray(data, dtype=np.float32),
            zi=self.zi,
        )
        return filtered.astype(np.float32, copy=False)


class StreamingBandpass:
    def __init__(self, sample_rate: int, low_hz: float, high_hz: float, taps: int = 257) -> None:
        nyquist = sample_rate / 2.0
        low = max(float(low_hz), 1.0)
        high = min(float(high_hz), nyquist * 0.95)
        if low >= high:
            raise ValueError("bandpass low_hz must be below high_hz")
        self.taps = signal.firwin(taps, [low / nyquist, high / nyquist], pass_zero=False).astype(np.float64)
        self.zi = np.zeros(len(self.taps) - 1, dtype=np.float64)

    def process(self, data: np.ndarray) -> np.ndarray:
        filtered, self.zi = signal.lfilter(
            self.taps,
            [1.0],
            np.asarray(data, dtype=np.float32),
            zi=self.zi,
        )
        return filtered.astype(np.float32, copy=False)


class StreamingDeemphasis:
    def __init__(self, sample_rate: int, tau_us: float) -> None:
        self.enabled = tau_us > 0
        if not self.enabled:
            self.b = np.array([1.0], dtype=np.float64)
            self.a = np.array([1.0], dtype=np.float64)
            self.zi = np.zeros((0, 2), dtype=np.float64)
            return
        tau = float(tau_us) * 1e-6
        dt = 1.0 / float(sample_rate)
        alpha = dt / (tau + dt)
        self.b = np.array([alpha], dtype=np.float64)
        self.a = np.array([1.0, -(1.0 - alpha)], dtype=np.float64)
        self.zi = np.zeros((1, 2), dtype=np.float64)

    def process(self, stereo: np.ndarray) -> np.ndarray:
        data = np.asarray(stereo, dtype=np.float32)
        if not self.enabled:
            return data
        filtered, self.zi = signal.lfilter(self.b, self.a, data, axis=0, zi=self.zi)
        return filtered.astype(np.float32, copy=False)


class StereoMpxDecoder:
    def __init__(
        self,
        mpx_rate: int,
        audio_rate: int,
        pilot_hz: float = 19_000.0,
        sum_level: float = 0.9,
        diff_level: float = 0.9,
        deemphasis_us: float = 50.0,
        stereo: bool = True,
        stereo_blend: float = 1.0,
    ) -> None:
        self.mpx_rate = int(mpx_rate)
        self.audio_rate = int(audio_rate)
        self.pilot_hz = float(pilot_hz)
        self.sum_level = float(sum_level)
        self.diff_level = float(diff_level)
        self.stereo = bool(stereo)
        self.stereo_blend = float(np.clip(stereo_blend, 0.0, 1.0))
        self.sample_index = 0
        self.mono_lpf = StreamingLowpass(mpx_rate, 15_000.0)
        self.diff_bpf = StreamingBandpass(mpx_rate, 23_000.0, 53_000.0)
        self.diff_lpf = StreamingLowpass(mpx_rate, 15_000.0)
        self.deemphasis = StreamingDeemphasis(audio_rate, deemphasis_us)

    def _pilot_phase(self, mpx: np.ndarray, t: np.ndarray) -> float:
        carrier = np.exp(-2j * np.pi * self.pilot_hz * t)
        estimate = np.mean(np.asarray(mpx, dtype=np.float32) * carrier)
        if abs(estimate) < 1e-6:
            return 0.0
        return float(np.angle(estimate) + np.pi / 2.0)

    def decode(self, mpx: np.ndarray) -> np.ndarray:
        data = np.asarray(mpx, dtype=np.float32)
        if data.size == 0:
            return np.empty((0, 2), dtype=np.float32)

        n = data.size
        t = (np.arange(n, dtype=np.float64) + self.sample_index) / self.mpx_rate
        self.sample_index += n

        mono = self.mono_lpf.process(data) / max(self.sum_level, 1e-6)
        if self.stereo and self.stereo_blend > 0.0:
            phase = self._pilot_phase(data, t)
            subcarrier = np.sin(2.0 * (2.0 * np.pi * self.pilot_hz * t + phase))
            stereo_subband = self.diff_bpf.process(data)
            diff = self.diff_lpf.process(2.0 * stereo_subband * subcarrier) / max(self.diff_level, 1e-6)
            if self.stereo_blend != 1.0:
                diff = diff * self.stereo_blend
            stereo = np.column_stack((mono + diff, mono - diff))
        else:
            stereo = np.column_stack((mono, mono))

        audio = resample(stereo, self.mpx_rate, self.audio_rate)
        return np.clip(self.deemphasis.process(audio), -1.0, 1.0)


def resample(data: np.ndarray, src_rate: int, dst_rate: int) -> np.ndarray:
    if src_rate == dst_rate:
        return np.asarray(data, dtype=np.float32)
    factor = gcd(src_rate, dst_rate)
    up = dst_rate // factor
    down = src_rate // factor
    return signal.resample_poly(data, up, down, axis=0).astype(np.float32, copy=False)


def parse_iq(raw: bytes, iq_format: str) -> np.ndarray:
    if iq_format == "rtl_u8":
        return u8_iq_to_complex(raw)
    if iq_format == "complex64":
        data = np.frombuffer(raw, dtype=np.complex64)
        return data.astype(np.complex64, copy=False)
    raise ValueError(f"unsupported IQ format: {iq_format}")


def run(args: argparse.Namespace) -> int:
    if args.mpx_rate > args.sample_rate:
        raise SystemExit("--mpx-rate moet kleiner dan of gelijk aan --sample-rate zijn")
    if args.channel_bandwidth >= args.sample_rate / 2.0:
        raise SystemExit("--channel-bandwidth moet kleiner zijn dan Nyquist")

    source, proc = open_iq_input(args)
    audio_out, closeable_audio = open_audio_output(args.audio_out, args.audio_rate)
    discriminator = FmDiscriminator(args.sample_rate, args.deviation)
    channel_lpf = StreamingLowpass(args.sample_rate, args.channel_bandwidth, taps=257)
    decoder = StereoMpxDecoder(
        mpx_rate=args.mpx_rate,
        audio_rate=args.audio_rate,
        pilot_hz=args.pilot_hz,
        sum_level=args.sum_level,
        diff_level=args.diff_level,
        deemphasis_us=args.deemphasis_us,
        stereo=not args.mono,
        stereo_blend=args.stereo_blend,
    )
    bytes_per_sample = 2 if args.iq_format == "rtl_u8" else 8
    read_size = args.block_samples * bytes_per_sample
    deadline = time.monotonic() + args.seconds if args.seconds is not None else None
    chunks = 0
    sample_index = 0

    try:
        while True:
            if deadline is not None and time.monotonic() >= deadline:
                break
            raw = read_exact(source, read_size)
            if not raw:
                break
            iq = parse_iq(raw, args.iq_format)
            if args.freq_offset:
                n = np.arange(iq.size, dtype=np.float64) + sample_index
                iq = iq * np.exp(-2j * np.pi * args.freq_offset * n / args.sample_rate)
            sample_index += iq.size
            mpx_rf_rate = channel_lpf.process(discriminator.demod(iq))
            mpx = resample(mpx_rf_rate, args.sample_rate, args.mpx_rate)
            audio = decoder.decode(mpx)
            if audio.size:
                try:
                    write_pcm(audio_out, audio, args.audio_gain, flush=chunks % args.flush_chunks == 0)
                except BrokenPipeError:
                    suppress_stdout_broken_pipe()
                    return 0
            chunks += 1
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

    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Decode WBFM broadcast audio from RTL-SDR IQ")
    parser.add_argument("--freq", type=int, help="start rtl_sdr at this RF frequency in Hz")
    parser.add_argument("--device-index", type=int, default=0)
    parser.add_argument("--sample-rate", type=int, default=DEFAULT_SAMPLE_RATE)
    parser.add_argument("--ppm", type=int, default=0)
    parser.add_argument("--gain", default="auto")
    parser.add_argument("--iq-in", default="-", help="IQ input file or - for stdin")
    parser.add_argument(
        "--iq-format",
        choices=["rtl_u8", "complex64"],
        default="rtl_u8",
        help="input IQ format; rtl_u8 matches rtl_sdr stdout",
    )
    parser.add_argument("--audio-out", default="-", help="WAV/s16le output, or - for stdout")
    parser.add_argument("--audio-rate", type=int, default=DEFAULT_AUDIO_RATE)
    parser.add_argument("--mpx-rate", type=int, default=DEFAULT_MPX_RATE)
    parser.add_argument("--deviation", type=float, default=75_000.0)
    parser.add_argument("--deemphasis-us", type=float, default=50.0)
    parser.add_argument("--pilot-hz", type=float, default=19_000.0)
    parser.add_argument("--sum-level", type=float, default=0.9)
    parser.add_argument("--diff-level", type=float, default=0.9)
    parser.add_argument(
        "--stereo-blend",
        type=float,
        default=0.65,
        help="L-R stereo amount, 0.0 is mono and 1.0 is full stereo (default: 0.65)",
    )
    parser.add_argument("--channel-bandwidth", type=float, default=100_000.0)
    parser.add_argument("--freq-offset", type=float, default=0.0, help="baseband correction in Hz")
    parser.add_argument("--audio-gain", type=float, default=0.8)
    parser.add_argument("--block-samples", type=int, default=65_536)
    parser.add_argument("--flush-chunks", type=int, default=4)
    parser.add_argument("--seconds", type=float)
    parser.add_argument("--mono", action="store_true", help="decode only L+R mono")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    return run(parse_args(sys.argv[1:] if argv is None else argv))


if __name__ == "__main__":
    raise SystemExit(main())
