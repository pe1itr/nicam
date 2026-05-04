from __future__ import annotations

import argparse
import queue
import sys
import threading
import time
from typing import BinaryIO

import numpy as np

from .constants import DEFAULT_SAMPLE_RATE, SYMBOL_RATE
from .iq import u8_iq_to_complex


def init_pluto(
    uri: str,
    sample_rate: int,
    lo_hz: int,
    tx_gain_db: int,
    rf_bandwidth: int | None = None,
    cyclic: bool = False,
    buffer_samples: int | None = None,
):
    """Create and configure a PlutoSDR TX object via pyadi-iio."""
    try:
        import adi
    except ImportError as exc:
        raise SystemExit("Installeer pyadi-iio: pip install pyadi-iio") from exc

    sdr = adi.Pluto(uri)
    sdr.sample_rate = int(sample_rate)
    sdr.tx_lo = int(lo_hz)
    if rf_bandwidth is not None:
        sdr.tx_rf_bandwidth = int(rf_bandwidth)
    sdr.tx_hardwaregain_chan0 = int(tx_gain_db)
    sdr.tx_cyclic_buffer = bool(cyclic)
    if buffer_samples is not None:
        sdr.tx_buffer_size = int(buffer_samples)
    return sdr


def scale_for_pluto(iq: np.ndarray, amplitude: float = 0.8) -> np.ndarray:
    scaled = np.asarray(iq, dtype=np.complex64) * float(amplitude)
    peak = float(np.max(np.abs(scaled))) if scaled.size else 1.0
    if peak > 1.0:
        scaled = scaled / peak
    return (scaled * (2**14)).astype(np.complex64)


def transmit_iq(
    uri: str,
    sample_rate: int,
    lo_hz: int,
    tx_gain_db: int,
    iq: np.ndarray,
    rf_bandwidth: int | None = None,
    cyclic: bool = True,
) -> None:
    sdr = init_pluto(uri, sample_rate, lo_hz, tx_gain_db, rf_bandwidth, cyclic)
    scaled = scale_for_pluto(iq)
    sdr.tx(scaled.astype(np.complex64))


def open_iq_input(path: str) -> BinaryIO:
    if path == "-":
        return sys.stdin.buffer
    return open(path, "rb")


def read_exact_or_partial(source: BinaryIO, byte_count: int) -> bytes:
    chunks: list[bytes] = []
    remaining = byte_count
    while remaining > 0:
        chunk = source.read(remaining)
        if not chunk:
            break
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def stream_noncyclic_u8_iq_to_pluto(
    sdr,
    source: BinaryIO,
    args: argparse.Namespace,
    chunk_bytes: int,
) -> None:
    buffers: "queue.Queue[np.ndarray | None]" = queue.Queue(maxsize=args.queue_buffers)
    stop_event = threading.Event()
    stats = {"input_buffers": 0, "tx_buffers": 0, "underruns": 0}
    buffer_duration_ms = 1000.0 * float(args.buffer_samples) / float(args.sample_rate)
    buffer_duration_s = buffer_duration_ms / 1000.0
    underrun_timeout_s = args.underrun_timeout_ms / 1000.0
    if underrun_timeout_s <= 0.0:
        underrun_timeout_s = max(0.05, buffer_duration_ms * 1.5 / 1000.0)

    def producer() -> None:
        try:
            while not stop_event.is_set():
                raw = read_exact_or_partial(source, chunk_bytes)
                if len(raw) < 2:
                    break
                iq = u8_iq_to_complex(raw)
                if iq.size:
                    buffers.put(scale_for_pluto(iq, args.amplitude))
                    stats["input_buffers"] += 1
                if len(raw) < chunk_bytes:
                    break
        finally:
            buffers.put(None)

    thread = threading.Thread(target=producer, daemon=True)
    thread.start()

    last_buffer: np.ndarray | None = None
    initial_buffers: list[np.ndarray] = []
    prebuffered = 0
    prebuffer_target = min(args.prebuffer_buffers, args.queue_buffers)
    while prebuffered < prebuffer_target:
        item = buffers.get()
        if item is None:
            return
        initial_buffers.append(item)
        prebuffered += 1

    next_report = time.monotonic() + args.status_interval
    next_tx_time = time.monotonic()
    try:
        for item in initial_buffers:
            if args.realtime_pace:
                sleep_s = next_tx_time - time.monotonic()
                if sleep_s > 0:
                    time.sleep(sleep_s)
            sdr.tx(item)
            stats["tx_buffers"] += 1
            last_buffer = item
            if args.realtime_pace:
                next_tx_time += buffer_duration_s

        while True:
            try:
                item = buffers.get(timeout=underrun_timeout_s)
            except queue.Empty:
                if last_buffer is None or not args.repeat_on_underrun:
                    continue
                sdr.tx(last_buffer)
                stats["tx_buffers"] += 1
                stats["underruns"] += 1
                item = None

            if item is None:
                if stop_event.is_set():
                    break
                if not thread.is_alive() and buffers.empty():
                    break
            else:
                if args.realtime_pace:
                    sleep_s = next_tx_time - time.monotonic()
                    if sleep_s > 0:
                        time.sleep(sleep_s)
                sdr.tx(item)
                stats["tx_buffers"] += 1
                last_buffer = item
                if args.realtime_pace:
                    next_tx_time += buffer_duration_s
                    now = time.monotonic()
                    if next_tx_time < now - buffer_duration_s:
                        next_tx_time = now

            if args.status_interval > 0 and time.monotonic() >= next_report:
                print(
                    "pluto_tx: "
                    f"input_buffers={stats['input_buffers']} "
                    f"tx_buffers={stats['tx_buffers']} "
                    f"queue={buffers.qsize()} "
                    f"underruns={stats['underruns']} "
                    f"pace={'on' if args.realtime_pace else 'off'} "
                    f"timeout_ms={underrun_timeout_s * 1000.0:.0f}",
                    file=sys.stderr,
                )
                next_report = time.monotonic() + args.status_interval
    finally:
        stop_event.set()
        thread.join(timeout=1.0)


def stream_u8_iq_to_pluto(args: argparse.Namespace) -> int:
    if args.sample_rate % SYMBOL_RATE:
        raise SystemExit("--sample-rate moet een veelvoud van 364000 zijn")

    sdr = init_pluto(
        args.uri,
        args.sample_rate,
        args.lo,
        args.tx_gain,
        args.rf_bandwidth,
        args.cyclic,
        args.buffer_samples,
    )
    source = open_iq_input(args.iq_in)
    raw_chunks: list[bytes] = []
    chunk_bytes = args.buffer_samples * 2

    try:
        if not args.cyclic:
            stream_noncyclic_u8_iq_to_pluto(sdr, source, args, chunk_bytes)
            return 0

        while True:
            raw = source.read(chunk_bytes)
            if not raw:
                break
            if len(raw) < 2:
                break

            if args.cyclic:
                raw_chunks.append(raw)
                continue

            iq = u8_iq_to_complex(raw)
            if iq.size:
                sdr.tx(scale_for_pluto(iq, args.amplitude))

        if args.cyclic:
            raw = b"".join(raw_chunks)
            iq = u8_iq_to_complex(raw)
            if not iq.size:
                raise SystemExit("Geen IQ-samples gelezen voor cyclic TX")
            sdr.tx(scale_for_pluto(iq, args.amplitude))
            print("Cyclic TX draait. Stop met Ctrl-C.", file=sys.stderr)
            try:
                while True:
                    time.sleep(1)
            except KeyboardInterrupt:
                pass
    finally:
        source.close()
        try:
            sdr.tx_destroy_buffer()
        except Exception:
            pass

    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Transmit rtl_sdr-style uint8 IQ via PlutoSDR")
    parser.add_argument("--uri", default="ip:192.168.2.1")
    parser.add_argument("--iq-in", default="-", help="rtl_sdr-style uint8 IQ input, or - for stdin")
    parser.add_argument("--sample-rate", type=int, default=DEFAULT_SAMPLE_RATE)
    parser.add_argument("--lo", type=int, required=True)
    parser.add_argument("--tx-gain", type=int, default=-30)
    parser.add_argument("--rf-bandwidth", type=int, default=750_000)
    parser.add_argument("--buffer-samples", type=int, default=262_144)
    parser.add_argument("--amplitude", type=float, default=0.8)
    parser.add_argument(
        "--queue-buffers",
        type=int,
        default=12,
        help="number of converted TX buffers to keep queued for non-cyclic streaming",
    )
    parser.add_argument(
        "--prebuffer-buffers",
        type=int,
        default=8,
        help="queue this many TX buffers before starting non-cyclic Pluto output",
    )
    parser.add_argument(
        "--underrun-timeout-ms",
        type=int,
        default=0,
        help="wait this long for input before repeating the last TX buffer; 0 derives it from --buffer-samples",
    )
    parser.add_argument(
        "--no-repeat-on-underrun",
        dest="repeat_on_underrun",
        action="store_false",
        help="leave Pluto input gaps visible instead of repeating the last buffer",
    )
    parser.set_defaults(repeat_on_underrun=False)
    parser.add_argument(
        "--no-realtime-pace",
        dest="realtime_pace",
        action="store_false",
        help="do not pace non-cyclic writes to the configured sample rate",
    )
    parser.set_defaults(realtime_pace=True)
    parser.add_argument(
        "--status-interval",
        type=float,
        default=5.0,
        help="print non-cyclic queue/underrun status every N seconds; 0 disables",
    )
    parser.add_argument(
        "--cyclic",
        action="store_true",
        help="read all input first and repeat it in the Pluto cyclic TX buffer",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    return stream_u8_iq_to_pluto(parse_args(sys.argv[1:] if argv is None else argv))


if __name__ == "__main__":
    raise SystemExit(main())
