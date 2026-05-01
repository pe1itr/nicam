from __future__ import annotations

import numpy as np


def complex_to_u8_iq(iq: np.ndarray, amplitude: float = 0.7) -> bytes:
    data = np.asarray(iq, dtype=np.complex64) * amplitude
    interleaved = np.empty(data.size * 2, dtype=np.float32)
    interleaved[0::2] = data.real
    interleaved[1::2] = data.imag
    return np.clip(np.rint(interleaved * 127.5 + 127.5), 0, 255).astype(np.uint8).tobytes()


def u8_iq_to_complex(raw: bytes) -> np.ndarray:
    u8 = np.frombuffer(raw, dtype=np.uint8)
    if u8.size < 2:
        return np.empty(0, dtype=np.complex64)
    if u8.size % 2:
        u8 = u8[:-1]
    f = (u8.astype(np.float32) - 127.5) / 127.5
    return (f[0::2] + 1j * f[1::2]).astype(np.complex64)
