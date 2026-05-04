from __future__ import annotations

import numpy as np


def pn9(length: int) -> np.ndarray:
    """Return standard NICAM PN9 bits after the FAW.

    EN 300 163 defines x^9 + x^4 + 1 with an all-ones initialization, but the
    first scrambled bit after the FAW is the sequence starting
    0000 0111 1011 1110 0010. The raw shift-register output has nine leading
    ones before that point, so skip those legacy warm-up bits here.
    """
    reg = np.ones(9, dtype=np.uint8)
    out = np.empty(length + 9, dtype=np.uint8)

    for i in range(length + 9):
        out[i] = reg[8]
        feedback = reg[8] ^ reg[4]
        reg[1:] = reg[:-1]
        reg[0] = feedback

    return out[9:]
