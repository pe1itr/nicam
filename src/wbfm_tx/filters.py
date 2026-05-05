from __future__ import annotations

from math import gcd

import numpy as np
from scipy import signal


class StreamingIntegerUpsampler:
    def __init__(self, factor: int, channels: int | None = None, taps: int = 129) -> None:
        if factor < 1:
            raise ValueError("Upsample-factor moet minimaal 1 zijn.")
        self.factor = int(factor)
        self.channels = channels
        self.taps = signal.firwin(taps, 1.0 / self.factor).astype(np.float64) * self.factor
        if channels is None:
            self._zi = np.zeros(len(self.taps) - 1, dtype=np.float64)
        else:
            self._zi = np.zeros((len(self.taps) - 1, channels), dtype=np.float64)

    def process(self, audio: np.ndarray) -> np.ndarray:
        data = np.asarray(audio, dtype=np.float32)
        if self.factor == 1:
            return data
        if self.channels is None:
            upsampled = np.zeros(len(data) * self.factor, dtype=np.float32)
            upsampled[:: self.factor] = data
        else:
            if data.ndim != 2 or data.shape[1] != self.channels:
                raise ValueError(f"Verwacht audio met vorm (samples, {self.channels}).")
            upsampled = np.zeros((len(data) * self.factor, self.channels), dtype=np.float32)
            upsampled[:: self.factor, :] = data
        filtered, self._zi = signal.lfilter(self.taps, [1.0], upsampled, axis=0, zi=self._zi)
        return filtered.astype(np.float32, copy=False)


def as_float32_stereo(audio: np.ndarray) -> np.ndarray:
    data = np.asarray(audio, dtype=np.float32)
    if data.ndim == 1:
        data = np.column_stack((data, data))
    if data.ndim != 2:
        raise ValueError("Audio moet mono of stereo zijn.")
    if data.shape[1] == 1:
        data = np.repeat(data, 2, axis=1)
    if data.shape[1] > 2:
        data = data[:, :2]
    peak = float(np.max(np.abs(data))) if data.size else 0.0
    if peak > 1.0:
        data = data / peak
    return data.astype(np.float32, copy=False)


def resample_audio(audio: np.ndarray, src_rate: int, dst_rate: int) -> np.ndarray:
    if src_rate == dst_rate:
        return np.asarray(audio, dtype=np.float32)
    factor = gcd(src_rate, dst_rate)
    up = dst_rate // factor
    down = src_rate // factor
    return signal.resample_poly(audio, up, down, axis=0).astype(np.float32)


def lowpass_audio(audio: np.ndarray, sample_rate: int, cutoff_hz: float, taps: int = 129) -> np.ndarray:
    nyquist = sample_rate / 2.0
    if cutoff_hz >= nyquist:
        return np.asarray(audio, dtype=np.float32)
    coeff = signal.firwin(taps, cutoff_hz / nyquist)
    return signal.lfilter(coeff, [1.0], audio, axis=0).astype(np.float32)


def preemphasis(audio: np.ndarray, sample_rate: int, tau_us: float) -> np.ndarray:
    if tau_us <= 0:
        return np.asarray(audio, dtype=np.float32)
    tau = tau_us * 1e-6
    data = np.asarray(audio, dtype=np.float32)
    if data.ndim == 1:
        previous = np.concatenate((np.zeros(1, dtype=np.float32), data[:-1]))
    else:
        previous = np.vstack((np.zeros((1, data.shape[1]), dtype=np.float32), data[:-1]))
    emphasized = data + (tau * sample_rate) * (data - previous)
    peak = float(np.max(np.abs(emphasized))) if emphasized.size else 0.0
    if peak > 1.0:
        emphasized = emphasized / peak
    return emphasized.astype(np.float32)
