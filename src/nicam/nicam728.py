from __future__ import annotations

import math

import numpy as np

from .audio_payload import (
    AUDIO_SAMPLE_RATE,
    CHANNELS,
    PCM_VALUES_PER_NICAM_FRAME,
    STEREO_FRAMES_PER_NICAM_FRAME,
)
from .frame import deinterleave_payload, interleave_payload

try:
    from . import _nicam728 as _c_nicam728
except ImportError:
    _c_nicam728 = None

RANGE_TO_SHIFT = {
    0b111: 4,
    0b110: 3,
    0b101: 2,
    0b011: 1,
    0b100: 0,
    0b010: 0,
    0b001: 0,
    0b000: 0,
}

SIGNAL_GROUPS = (
    (0, 0b100),  # R2 left
    (1, 0b100),  # R2 right
    (2, 0b010),  # R1 left
    (3, 0b010),  # R1 right
    (4, 0b001),  # R0 left
    (5, 0b001),  # R0 right
)


class J17Preemphasis:
    """Stateful inverse of the NICAM receiver J.17 de-emphasis filter."""

    def __init__(self, sample_rate: int = AUDIO_SAMPLE_RATE) -> None:
        k = 2.0 * float(sample_rate)
        zero = 3000.0
        pole = 3000.0 * math.sqrt(75.0)
        gain = 75.0**0.25
        self.b0 = gain * (k + zero) / (k + pole)
        self.b1 = gain * (zero - k) / (k + pole)
        self.a1 = (pole - k) / (k + pole)
        self.x1 = np.zeros(CHANNELS, dtype=np.float64)
        self.y1 = np.zeros(CHANNELS, dtype=np.float64)

    def process(self, pcm: np.ndarray) -> np.ndarray:
        stereo = np.asarray(pcm, dtype=np.int16).reshape(-1, CHANNELS)
        out = np.empty(stereo.shape, dtype=np.int16)
        for idx, sample in enumerate(stereo.astype(np.float64)):
            y = self.b0 * sample + self.b1 * self.x1 - self.a1 * self.y1
            self.x1 = sample
            self.y1 = y
            out[idx] = np.clip(np.rint(y), -32768, 32767).astype(np.int16)
        return out.reshape(-1)


class J17Deemphasis:
    """Stateful NICAM receiver J.17 de-emphasis filter."""

    def __init__(self, sample_rate: int = AUDIO_SAMPLE_RATE) -> None:
        k = 2.0 * float(sample_rate)
        pole = 3000.0
        zero = 3000.0 * math.sqrt(75.0)
        gain = 75.0**-0.25
        self.b0 = gain * (k + zero) / (k + pole)
        self.b1 = gain * (zero - k) / (k + pole)
        self.a1 = (pole - k) / (k + pole)
        self.x1 = np.zeros(CHANNELS, dtype=np.float64)
        self.y1 = np.zeros(CHANNELS, dtype=np.float64)

    def process(self, pcm: np.ndarray) -> np.ndarray:
        stereo = np.asarray(pcm, dtype=np.int16).reshape(-1, CHANNELS)
        out = np.empty(stereo.shape, dtype=np.int16)
        for idx, sample in enumerate(stereo.astype(np.float64)):
            y = self.b0 * sample + self.b1 * self.x1 - self.a1 * self.y1
            self.x1 = sample
            self.y1 = y
            out[idx] = np.clip(np.rint(y), -32768, 32767).astype(np.int16)
        return out.reshape(-1)


def _range_word_and_shift(samples14: np.ndarray) -> tuple[int, int]:
    peak = int(np.max(np.abs(samples14.astype(np.int32)))) if samples14.size else 0
    if peak >= 4096:
        return 0b111, 4
    if peak >= 2048:
        return 0b110, 3
    if peak >= 1024:
        return 0b101, 2
    if peak >= 512:
        return 0b011, 1
    if peak >= 256:
        return 0b100, 0
    if peak >= 128:
        return 0b010, 0
    return 0b000, 0


def _signed10_to_bits(words: np.ndarray) -> np.ndarray:
    shifts = np.arange(10, dtype=np.uint16)
    return ((words[:, None].astype(np.uint16) >> shifts) & 1).astype(np.uint8)


def _bits_to_signed10(bits: np.ndarray) -> np.ndarray:
    weights = (1 << np.arange(10)).astype(np.int16)
    values = np.sum(bits.astype(np.int16) * weights, axis=1, dtype=np.int16)
    return np.where(values & 0x200, values - 0x400, values).astype(np.int16)


def _encode_channel(samples16: np.ndarray) -> tuple[np.ndarray, int]:
    samples14 = np.clip(samples16.astype(np.int32) >> 2, -8192, 8191).astype(np.int16)
    range_word, shift = _range_word_and_shift(samples14)
    compressed = (samples14.astype(np.int32) >> shift).astype(np.int16)
    words = (compressed.astype(np.int32) & 0x3FF).astype(np.uint16)
    value_bits = _signed10_to_bits(words)
    parity = (np.sum(value_bits[:, 4:10], axis=1, dtype=np.uint8) & 1).reshape(-1, 1)
    return np.column_stack([value_bits, parity]).astype(np.uint8), range_word


def _decode_channel(words: np.ndarray, range_word: int) -> np.ndarray:
    shift = RANGE_TO_SHIFT.get(int(range_word), 0)
    signed10 = _bits_to_signed10(words[:, :10])
    samples14 = np.clip(signed10.astype(np.int32) << shift, -8192, 8191)
    return np.clip(samples14 << 2, -32768, 32767).astype(np.int16)


def _parity_syndrome(words: np.ndarray) -> np.ndarray:
    return (np.sum(words[:, 4:10], axis=1, dtype=np.uint8) + words[:, 10]) & 1


def _apply_signalling_in_parity(words: np.ndarray, left_range: int, right_range: int) -> None:
    ranges = (left_range, right_range)
    for start, mask in SIGNAL_GROUPS:
        channel_range = ranges[start & 1]
        if channel_range & mask:
            words[start:54:6, 10] ^= 1


def _decode_ranges_and_errors(words: np.ndarray) -> tuple[int, int, int]:
    syndrome = _parity_syndrome(words)
    range_bits = [0, 0]
    errors = 0

    for start, mask in SIGNAL_GROUPS:
        group = syndrome[start:54:6]
        bit = int(np.count_nonzero(group) >= 5)
        range_bits[start & 1] |= bit * mask
        errors += int(np.count_nonzero(group != bit))

    for start in (54, 59):
        group = syndrome[start : start + 5]
        bit = int(np.count_nonzero(group) >= 3)
        errors += int(np.count_nonzero(group != bit))

    return range_bits[0], range_bits[1], errors


def pcm16_to_nicam_payload(pcm: np.ndarray) -> np.ndarray:
    """Pack one 1 ms stereo PCM block as a NICAM 728 audio payload."""
    samples = np.asarray(pcm, dtype=np.int16).reshape(-1)
    if samples.size != PCM_VALUES_PER_NICAM_FRAME:
        raise ValueError(f"need {PCM_VALUES_PER_NICAM_FRAME} interleaved PCM values")

    stereo = samples.reshape(STEREO_FRAMES_PER_NICAM_FRAME, CHANNELS)
    left_words, left_range = _encode_channel(stereo[:, 0])
    right_words, right_range = _encode_channel(stereo[:, 1])
    words = np.empty((PCM_VALUES_PER_NICAM_FRAME, 11), dtype=np.uint8)
    words[0::2] = left_words
    words[1::2] = right_words
    _apply_signalling_in_parity(words, left_range, right_range)
    return interleave_payload(words.reshape(-1))


def pcm16_to_nicam_payload_j17(pcm: np.ndarray, j17: J17Preemphasis) -> np.ndarray:
    """Apply J.17 pre-emphasis and pack one 1 ms stereo block."""
    return pcm16_to_nicam_payload(j17.process(pcm))


def nicam_payload_to_pcm16(transmitted_payload: np.ndarray) -> np.ndarray:
    if _c_nicam728 is not None:
        payload = np.asarray(transmitted_payload, dtype=np.uint8)
        pcm_bytes, _errors = _c_nicam728.decode_payload(payload.tobytes())
        return np.frombuffer(pcm_bytes, dtype=np.int16)

    bits = deinterleave_payload(transmitted_payload)
    words = bits.reshape(-1, 11).astype(np.uint8)
    left_range, right_range, _errors = _decode_ranges_and_errors(words)
    stereo = np.empty((STEREO_FRAMES_PER_NICAM_FRAME, CHANNELS), dtype=np.int16)
    stereo[:, 0] = _decode_channel(words[0::2], left_range)
    stereo[:, 1] = _decode_channel(words[1::2], right_range)
    return stereo.reshape(-1)


def nicam_payload_parity_errors(transmitted_payload: np.ndarray) -> int:
    if _c_nicam728 is not None:
        payload = np.asarray(transmitted_payload, dtype=np.uint8)
        return int(_c_nicam728.payload_parity_errors(payload.tobytes()))

    bits = deinterleave_payload(transmitted_payload)
    words = bits.reshape(-1, 11).astype(np.uint8)
    _left_range, _right_range, errors = _decode_ranges_and_errors(words)
    return errors


__all__ = [
    "AUDIO_SAMPLE_RATE",
    "CHANNELS",
    "PCM_VALUES_PER_NICAM_FRAME",
    "J17Deemphasis",
    "J17Preemphasis",
    "pcm16_to_nicam_payload",
    "pcm16_to_nicam_payload_j17",
    "nicam_payload_to_pcm16",
    "nicam_payload_parity_errors",
]
