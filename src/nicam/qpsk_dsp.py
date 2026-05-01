from __future__ import annotations

import numpy as np

from .dqpsk import bits_to_symbols as py_bits_to_symbols
from .dqpsk import final_phase_quarter as py_final_phase_quarter
from .dqpsk import symbols_to_bits as py_symbols_to_bits

try:
    from . import _qpsk_dsp as _c_qpsk_dsp
except ImportError:
    _c_qpsk_dsp = None


def _require_c_backend() -> None:
    if _c_qpsk_dsp is None:
        raise SystemExit("C DSP backend niet beschikbaar; bouw extension eerst (python -m pip install -e .)")


def map_bits_to_symbols(bits: np.ndarray, initial_phase_quarter: int, backend: str) -> tuple[np.ndarray, int]:
    if backend == "c":
        _require_c_backend()
        data = np.asarray(bits, dtype=np.uint8)
        iq_bytes, final_phase = _c_qpsk_dsp.bits_to_symbols(data.tobytes(), int(initial_phase_quarter))
        symbols = np.frombuffer(iq_bytes, dtype=np.complex64)
        return symbols, int(final_phase)

    symbols = py_bits_to_symbols(bits, initial_phase_quarter)
    final_phase = py_final_phase_quarter(bits, initial_phase_quarter)
    return symbols, final_phase


def demap_symbols_to_bits(
    symbols: np.ndarray,
    previous_symbol: complex | np.complex64 | None,
    backend: str,
) -> tuple[np.ndarray, complex | np.complex64 | None]:
    if backend == "c":
        _require_c_backend()
        z = np.asarray(symbols, dtype=np.complex64)
        bits_bytes, last_symbol = _c_qpsk_dsp.symbols_to_bits(z.tobytes(), previous_symbol)
        bits = np.frombuffer(bits_bytes, dtype=np.uint8)
        return bits, last_symbol

    bits = py_symbols_to_bits(symbols, previous_symbol)
    last = None if np.asarray(symbols).size == 0 else np.asarray(symbols, dtype=np.complex64)[-1]
    return bits, last
