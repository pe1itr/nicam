from __future__ import annotations

import numpy as np


def pn9(length: int) -> np.ndarray:
    """Return NICAM PN9 bits for x^9 + x^4 + 1, initialized to all ones."""
    reg = np.ones(9, dtype=np.uint8)
    out = np.empty(length, dtype=np.uint8)

    for i in range(length):
        out[i] = reg[8]
        feedback = reg[8] ^ reg[4]
        reg[1:] = reg[:-1]
        reg[0] = feedback

    return out
