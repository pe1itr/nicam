from __future__ import annotations

import numpy as np


PLUTO_TX_FULL_SCALE = 2**14


class PlutoTx:
    def __init__(
        self,
        uri: str,
        tx_lo_hz: int,
        sample_rate_hz: int,
        rf_bandwidth_hz: int,
        tx_gain_db: float,
        buffer_size: int = 16384,
        cyclic_buffer: bool = False,
    ) -> None:
        try:
            import adi
        except ImportError as exc:
            raise RuntimeError("pyadi-iio ontbreekt. Installeer dependencies met pip install -r requirements.txt.") from exc

        try:
            self.sdr = adi.Pluto(uri)
            self.sdr.sample_rate = int(sample_rate_hz)
            self.sdr.tx_lo = int(tx_lo_hz)
            self.sdr.tx_rf_bandwidth = int(rf_bandwidth_hz)
            self.sdr.tx_hardwaregain_chan0 = float(tx_gain_db)
            self.sdr.tx_cyclic_buffer = bool(cyclic_buffer)
            self.sdr.tx_buffer_size = int(buffer_size)
        except Exception as exc:
            raise RuntimeError(f"Kan Pluto niet initialiseren via URI {uri!r}: {exc}") from exc

    def send(self, iq: np.ndarray) -> None:
        samples = np.asarray(iq, dtype=np.complex64)
        scaled = np.clip(samples.real, -1.0, 1.0) * PLUTO_TX_FULL_SCALE
        scaled = scaled + 1j * (np.clip(samples.imag, -1.0, 1.0) * PLUTO_TX_FULL_SCALE)
        self.sdr.tx(scaled.astype(np.complex64, copy=False))

    def stop(self) -> None:
        try:
            self.sdr.tx_destroy_buffer()
        except Exception:
            pass
