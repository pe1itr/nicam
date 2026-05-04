from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from .constants import BODY_BITS, FAW_BITS, FRAME_BITS, HEADER_BITS, PAYLOAD_BITS
from .prbs import pn9

FAW = np.array(FAW_BITS, dtype=np.uint8)
SCRAMBLE = pn9(BODY_BITS)


@dataclass(frozen=True)
class NicamFrame:
    bits: np.ndarray
    ci: tuple[int, int, int, int, int]
    ad: np.ndarray
    payload: np.ndarray
    raw_body: np.ndarray

    @property
    def frame_flag(self) -> int:
        return self.ci[0]

    @property
    def mode(self) -> int:
        return (self.ci[1] << 2) | (self.ci[2] << 1) | self.ci[3]

    @property
    def fallback(self) -> int:
        return self.ci[4]


def scramble_body(body_bits: np.ndarray) -> np.ndarray:
    body = np.asarray(body_bits, dtype=np.uint8)
    if body.size != BODY_BITS:
        raise ValueError(f"body must be {BODY_BITS} bits")
    return body ^ SCRAMBLE


def descramble_frame(raw_frame_bits: np.ndarray) -> NicamFrame:
    raw = np.asarray(raw_frame_bits, dtype=np.uint8)
    if raw.size != FRAME_BITS:
        raise ValueError(f"frame must be {FRAME_BITS} bits")
    if not np.array_equal(raw[: len(FAW)], FAW):
        raise ValueError("frame does not start with NICAM FAW")

    body = raw[len(FAW) :] ^ SCRAMBLE
    ci = tuple(int(x) for x in body[:5])
    ad = body[5:16].copy()
    payload = body[16:].copy()
    return NicamFrame(
        bits=np.concatenate([FAW, body]),
        ci=ci,
        ad=ad,
        payload=payload,
        raw_body=raw[len(FAW) :].copy(),
    )


def build_test_frame(frame_index: int = 0, mode: int = 0, fallback: int = 0) -> np.ndarray:
    """Build a valid scrambled NICAM frame with zero payload for RF tests."""
    return build_frame(np.zeros(PAYLOAD_BITS, dtype=np.uint8), frame_index, mode, fallback)


def build_frame(
    payload: np.ndarray, frame_index: int = 0, mode: int = 0, fallback: int = 0
) -> np.ndarray:
    """Build a scrambled NICAM frame from an already interleaved 704-bit payload."""
    payload_bits = np.asarray(payload, dtype=np.uint8)
    if payload_bits.size != PAYLOAD_BITS:
        raise ValueError(f"payload must be {PAYLOAD_BITS} bits")
    flag = int((frame_index % 16) < 8)
    ci = np.array(
        [
            flag,
            (mode >> 2) & 1,
            (mode >> 1) & 1,
            mode & 1,
            fallback & 1,
        ],
        dtype=np.uint8,
    )
    ad = np.zeros(11, dtype=np.uint8)
    body = np.concatenate([ci, ad, payload_bits])
    return np.concatenate([FAW, scramble_body(body)])


def find_frame_offsets(
    bits: np.ndarray, max_errors: int = 0, min_repeats: int = 3
) -> list[int]:
    """Return offsets where FAW is present and repeats one frame later."""
    data = np.asarray(bits, dtype=np.uint8)
    offsets: list[int] = []
    if min_repeats < 1:
        raise ValueError("min_repeats must be at least 1")
    if data.size < (min_repeats - 1) * FRAME_BITS + len(FAW):
        return offsets

    max_offset = data.size - (min_repeats - 1) * FRAME_BITS - len(FAW) + 1
    for offset in range(0, max_offset):
        matched = True
        for repeat in range(min_repeats):
            frame_offset = offset + repeat * FRAME_BITS
            faw_errors = int(
                np.count_nonzero(data[frame_offset : frame_offset + len(FAW)] ^ FAW)
            )
            if faw_errors > max_errors:
                matched = False
                break
        if matched:
            offsets.append(offset)
    return offsets


def select_lock_offset(
    bits: np.ndarray, max_errors: int = 0, min_repeats: int = 3, flag_frames: int = 16
) -> int | None:
    """Pick the most likely frame offset using FAW repeats and the CI frame flag."""
    data = np.asarray(bits, dtype=np.uint8)
    offsets = find_frame_offsets(data, max_errors=max_errors, min_repeats=min_repeats)
    if not offsets:
        return None

    best_offset = offsets[0]
    best_score = -1
    for offset in offsets:
        flags: list[int] = []
        modes: list[int] = []
        for idx in range(flag_frames):
            start = offset + idx * FRAME_BITS
            stop = start + FRAME_BITS
            if stop > data.size:
                break
            try:
                frame = descramble_frame(data[start:stop])
            except ValueError:
                break
            flags.append(frame.frame_flag)
            modes.append(frame.mode)

        if not flags:
            continue

        has_expected_toggle = int(0 in flags and 1 in flags)
        mode_stability = max(modes.count(mode) for mode in set(modes)) if modes else 0
        score = has_expected_toggle * 100 + len(flags) + mode_stability
        if score > best_score:
            best_score = score
            best_offset = offset

    return best_offset


def deinterleave_payload(transmitted_payload: np.ndarray) -> np.ndarray:
    """Reverse the NICAM 44 x 16 payload interleaver."""
    tx = np.asarray(transmitted_payload, dtype=np.uint8)
    if tx.size != PAYLOAD_BITS:
        raise ValueError(f"payload must be {PAYLOAD_BITS} bits")
    out = np.empty_like(tx)
    for raw_index in range(PAYLOAD_BITS):
        row = raw_index % 44
        col = raw_index // 44
        out[raw_index] = tx[row * 16 + col]
    return out


def interleave_payload(payload: np.ndarray) -> np.ndarray:
    payload = np.asarray(payload, dtype=np.uint8)
    if payload.size != PAYLOAD_BITS:
        raise ValueError(f"payload must be {PAYLOAD_BITS} bits")
    out = np.empty_like(payload)
    for raw_index in range(PAYLOAD_BITS):
        row = raw_index % 44
        col = raw_index // 44
        out[row * 16 + col] = payload[raw_index]
    return out
