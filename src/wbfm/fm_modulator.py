from __future__ import annotations

import numpy as np


class FmModulator:
    def __init__(self, sample_rate: int, deviation_hz: float, iq_level: float = 0.65) -> None:
        self.sample_rate = int(sample_rate)
        self.deviation_hz = float(deviation_hz)
        self.iq_level = float(iq_level)
        self._phase = 0.0

    def modulate(self, mpx: np.ndarray) -> np.ndarray:
        x = np.asarray(mpx, dtype=np.float32)
        phase_step = 2.0 * np.pi * self.deviation_hz * x / self.sample_rate
        phase = self._phase + np.cumsum(phase_step, dtype=np.float64)
        if len(phase):
            self._phase = float(phase[-1] % (2.0 * np.pi))
        iq = self.iq_level * np.exp(1j * phase)
        return iq.astype(np.complex64)
