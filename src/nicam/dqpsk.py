from __future__ import annotations

import numpy as np

from .constants import SYMBOL_RATE


PHASE_STEPS = {
    (0, 0): 0.0,
    (0, 1): -np.pi / 2,
    (1, 1): np.pi,
    (1, 0): np.pi / 2,
}


def bits_to_symbols(bits: np.ndarray, initial_phase_quarter: int = 0) -> np.ndarray:
    data = np.asarray(bits, dtype=np.uint8)
    if data.size % 2:
        raise ValueError("DQPSK input bit count must be even")

    codes = data[0::2] * 2 + data[1::2]
    quarter_steps = np.array([0, -1, 1, 2], dtype=np.int16)[codes]
    phases = np.mod(initial_phase_quarter + np.cumsum(quarter_steps, dtype=np.int32), 4)
    constellation = np.array([1 + 0j, 0 + 1j, -1 + 0j, 0 - 1j], dtype=np.complex64)
    return constellation[phases]


def final_phase_quarter(bits: np.ndarray, initial_phase_quarter: int = 0) -> int:
    data = np.asarray(bits, dtype=np.uint8)
    if data.size % 2:
        raise ValueError("DQPSK input bit count must be even")
    codes = data[0::2] * 2 + data[1::2]
    quarter_steps = np.array([0, -1, 1, 2], dtype=np.int16)[codes]
    return int((initial_phase_quarter + int(np.sum(quarter_steps, dtype=np.int32))) % 4)


def symbols_to_bits(
    symbols: np.ndarray, previous_symbol: complex | np.complex64 | None = None
) -> np.ndarray:
    z = np.asarray(symbols, dtype=np.complex64)
    if previous_symbol is not None:
        z = np.concatenate([np.array([previous_symbol], dtype=np.complex64), z])
    if z.size < 2:
        return np.empty(0, dtype=np.uint8)

    phase_delta = np.angle(z[1:] * np.conj(z[:-1]))
    q = np.mod(np.rint(phase_delta / (np.pi / 2)).astype(np.int8), 4)

    pairs = np.empty((q.size, 2), dtype=np.uint8)
    pairs[q == 0] = (0, 0)
    pairs[q == 3] = (0, 1)
    pairs[q == 2] = (1, 1)
    pairs[q == 1] = (1, 0)
    return pairs.reshape(-1)


def upsample_symbols(symbols: np.ndarray, sample_rate: int) -> np.ndarray:
    samples_per_symbol = sample_rate / SYMBOL_RATE
    if abs(samples_per_symbol - round(samples_per_symbol)) > 1e-9:
        raise ValueError("sample_rate must be an integer multiple of 364000")
    sps = int(round(samples_per_symbol))
    return np.repeat(np.asarray(symbols, dtype=np.complex64), sps)


def coarse_symbols_from_samples(
    iq: np.ndarray,
    sample_rate: int,
    timing_phase: int,
    freq_offset: float = 0.0,
) -> np.ndarray:
    samples_per_symbol = sample_rate / SYMBOL_RATE
    if abs(samples_per_symbol - round(samples_per_symbol)) > 1e-9:
        raise ValueError("sample_rate must be an integer multiple of 364000")
    sps = int(round(samples_per_symbol))
    data = np.asarray(iq, dtype=np.complex64)

    if freq_offset:
        n = np.arange(data.size, dtype=np.float32)
        data = data * np.exp(-2j * np.pi * freq_offset * n / sample_rate)

    usable = ((data.size - timing_phase) // sps) * sps
    if usable <= 0:
        return np.empty(0, dtype=np.complex64)

    sliced = data[timing_phase : timing_phase + usable].reshape(-1, sps)
    return sliced.mean(axis=1).astype(np.complex64)


def adaptive_symbols_from_samples(
    iq: np.ndarray,
    sample_rate: int,
    timing_phase: float = 0.0,
    freq_offset: float = 0.0,
    symbol_rate: int = SYMBOL_RATE,
    pll_adjustment: float = 1.0,
) -> np.ndarray:
    """Decision-directed QPSK sampler with carrier and timing tracking.

    This is intentionally compact, but follows the same broad receiver shape as
    mature QPSK demodulators: linear interpolation, a decision-directed carrier
    PLL, and Mueller and Muller timing feedback.
    """
    samples_per_symbol = sample_rate / symbol_rate
    data = np.asarray(iq, dtype=np.complex64)
    if data.size < 2:
        return np.empty(0, dtype=np.complex64)

    omega = float(samples_per_symbol)
    min_omega = omega * (1.0 - 100e-6)
    max_omega = omega * (1.0 + 100e-6)

    phase = 0.0
    freqw = 2.0 * np.pi * float(freq_offset) / float(sample_rate)
    max_freqw = freqw + 2.0 * np.pi * symbol_rate / 8.0 / sample_rate
    min_freqw = freqw - 2.0 * np.pi * symbol_rate / 8.0 / sample_rate

    freq_alpha = 0.04 * pll_adjustment
    freq_beta = 0.0012 / omega * pll_adjustment
    gain_mu = 0.02
    max_mucorr = 0.1

    # Start close to the requested timing phase. The loop keeps mu as time of
    # the next symbol relative to the current integer sample.
    pin = 0
    mu = float(timing_phase)
    while mu >= 1.0 and pin < data.size - 2:
        pin += 1
        mu -= 1.0

    est_power = float(np.mean(np.abs(data[: min(data.size, 4096)]) ** 2))
    agc_gain = 1.0 / np.sqrt(est_power) if est_power > 0 else 1.0
    kest = 0.01

    points = np.array([1 + 0j, 0 + 1j, -1 + 0j, 0 - 1j], dtype=np.complex64)
    hist_p = [0j, 0j, 0j]
    hist_c = [0j, 0j, 0j]
    out: list[complex] = []

    while pin < data.size - 1:
        if mu < 1.0:
            s0 = data[pin] * np.exp(-1j * phase)
            s1 = data[pin + 1] * np.exp(-1j * (phase + freqw))
            raw_symbol = s0 * (1.0 - mu) + s1 * mu
            symbol = raw_symbol * agc_gain
            out.append(symbol)

            nearest = points[int(np.argmin(np.abs(symbol - points)))]
            phase_error = float(np.angle(symbol * np.conj(nearest)))
            phase += phase_error * freq_alpha
            freqw += phase_error * freq_beta
            if freqw < min_freqw:
                freqw = min_freqw
            elif freqw > max_freqw:
                freqw = max_freqw

            hist_p = [symbol, hist_p[0], hist_p[1]]
            hist_c = [nearest, hist_c[0], hist_c[1]]
            muerr = float(
                np.real(
                    (hist_p[0] - hist_p[2]) * np.conj(hist_c[1])
                    - (hist_c[0] - hist_c[2]) * np.conj(hist_p[1])
                )
            )
            mucorr = float(np.clip(muerr * gain_mu, -max_mucorr, max_mucorr))
            mu += omega + mucorr

            insp = float(np.abs(raw_symbol) ** 2)
            est_power = insp * kest + est_power * (1.0 - kest)
            if est_power > 0:
                agc_gain = 1.0 / np.sqrt(est_power)

        pin += 1
        mu -= 1.0
        phase += freqw

        # Keep phase bounded to avoid long-capture precision loss.
        if abs(phase) > 1e6:
            phase = float(np.mod(phase + np.pi, 2.0 * np.pi) - np.pi)

    return np.asarray(out, dtype=np.complex64)
