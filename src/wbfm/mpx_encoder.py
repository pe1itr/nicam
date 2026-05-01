from __future__ import annotations

import numpy as np
from scipy import signal


class StereoMpxEncoder:
    def __init__(
        self,
        sample_rate: int,
        audio_lowpass_hz: float = 15000.0,
        preemphasis_us: float = 50.0,
        pilot_hz: float = 19000.0,
        pilot_level: float = 0.09,
        subcarrier_hz: float = 38000.0,
        sum_level: float = 0.9,
        diff_level: float = 0.9,
    ) -> None:
        self.sample_rate = int(sample_rate)
        self.audio_lowpass_hz = float(audio_lowpass_hz)
        self.preemphasis_us = float(preemphasis_us)
        self.pilot_hz = float(pilot_hz)
        self.pilot_level = float(pilot_level)
        self.subcarrier_hz = float(subcarrier_hz)
        self.sum_level = float(sum_level)
        self.diff_level = float(diff_level)
        self._phase = 0.0
        self._lp_taps = signal.firwin(129, self.audio_lowpass_hz / (self.sample_rate / 2.0))
        self._lp_zi = np.zeros((len(self._lp_taps) - 1, 2), dtype=np.float64)
        self._pre_prev = np.zeros(2, dtype=np.float64)

    def _apply_preemphasis(self, audio: np.ndarray) -> np.ndarray:
        if self.preemphasis_us <= 0:
            return audio
        tau = self.preemphasis_us * 1e-6
        previous = np.vstack((self._pre_prev, audio[:-1]))
        self._pre_prev = audio[-1].astype(np.float64, copy=True)
        return audio + (tau * self.sample_rate) * (audio - previous)

    def encode(self, stereo_audio: np.ndarray) -> np.ndarray:
        audio = np.asarray(stereo_audio, dtype=np.float32)
        if audio.ndim != 2 or audio.shape[1] != 2:
            raise ValueError("StereoMpxEncoder verwacht audio met vorm (samples, 2).")
        if len(audio) == 0:
            return np.zeros(0, dtype=np.float32)

        audio, self._lp_zi = signal.lfilter(self._lp_taps, [1.0], audio, axis=0, zi=self._lp_zi)
        audio = self._apply_preemphasis(audio)
        left = audio[:, 0]
        right = audio[:, 1]

        n = len(audio)
        t = (np.arange(n, dtype=np.float64) / self.sample_rate)
        pilot_phase = self._phase + 2.0 * np.pi * self.pilot_hz * t
        sub_phase = self._phase * 2.0 + 2.0 * np.pi * self.subcarrier_hz * t

        mono_sum = (left + right) * 0.5 * self.sum_level
        stereo_diff = (left - right) * 0.5 * self.diff_level
        pilot = self.pilot_level * np.sin(pilot_phase)
        dsb_sc = stereo_diff * np.sin(sub_phase)
        mpx = mono_sum + pilot + dsb_sc

        self._phase = (pilot_phase[-1] + 2.0 * np.pi * self.pilot_hz / self.sample_rate) % (2.0 * np.pi)
        peak = float(np.max(np.abs(mpx))) if mpx.size else 0.0
        if peak > 1.0:
            mpx = mpx / peak
        return mpx.astype(np.float32)
