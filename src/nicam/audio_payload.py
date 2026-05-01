from __future__ import annotations

import numpy as np

from .constants import PAYLOAD_BITS
from .frame import deinterleave_payload, interleave_payload

AUDIO_SAMPLE_RATE = 32_000
CHANNELS = 2
STEREO_FRAMES_PER_NICAM_FRAME = 32
PCM_VALUES_PER_NICAM_FRAME = STEREO_FRAMES_PER_NICAM_FRAME * CHANNELS


def pcm16_to_payload(pcm: np.ndarray) -> np.ndarray:
    """Pack 32 stereo PCM16 samples into a simple 10-bit payload plus parity.

    This is a lab payload for end-to-end SDR testing. It uses the NICAM frame,
    scrambler, interleaver and DQPSK transport, but not the official NICAM
    near-instantaneous companding.
    """
    samples = np.asarray(pcm, dtype=np.int16).reshape(-1)
    if samples.size != PCM_VALUES_PER_NICAM_FRAME:
        raise ValueError(f"need {PCM_VALUES_PER_NICAM_FRAME} interleaved PCM values")

    q10 = np.clip(np.rint(samples.astype(np.float32) / 64.0), -512, 511).astype(np.int16)
    words = (q10.astype(np.int32) & 0x3FF).astype(np.uint16)
    shifts = np.arange(9, -1, -1, dtype=np.uint16)
    value_bits = ((words[:, None] >> shifts) & 1).astype(np.uint8)
    parity = (np.sum(value_bits, axis=1, dtype=np.uint8) & 1).reshape(-1, 1)
    return interleave_payload(np.column_stack([value_bits, parity]).reshape(-1))


def payload_to_pcm16(transmitted_payload: np.ndarray) -> np.ndarray:
    bits = deinterleave_payload(transmitted_payload)
    words = bits.reshape(-1, 11)[:, :10].astype(np.int16)
    weights = (1 << np.arange(9, -1, -1)).astype(np.int16)
    values = np.sum(words * weights, axis=1, dtype=np.int16)
    values = np.where(values & 0x200, values - 0x400, values)
    return np.clip(values.astype(np.int32) * 64, -32768, 32767).astype(np.int16)


def payload_parity_errors(transmitted_payload: np.ndarray) -> int:
    bits = deinterleave_payload(transmitted_payload).reshape(-1, 11)
    expected = np.sum(bits[:, :10], axis=1, dtype=np.uint8) & 1
    return int(np.count_nonzero(expected ^ bits[:, 10]))
