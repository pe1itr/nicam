from __future__ import annotations

import argparse
import queue
import sys
import threading
import time

import numpy as np

from .audio_payload import PCM_VALUES_PER_NICAM_FRAME, pcm16_to_payload
from .constants import DEFAULT_SAMPLE_RATE, SYMBOL_RATE
from .dqpsk import bits_to_symbols, filter_baseband, final_phase_quarter, shape_symbols, upsample_symbols
from .frame import build_frame
from .nicam728 import J17Preemphasis, pcm16_to_nicam_payload_j17
from .pluto_tx import init_pluto, scale_for_pluto
from .stream_tx import nicam_level_to_amplitude, open_audio_source


def build_iq_frame(
    args: argparse.Namespace,
    pcm: np.ndarray,
    j17: J17Preemphasis,
    frame_index: int,
    phase_quarter: int,
    station_id: str | None,
) -> tuple[np.ndarray, int]:
    payload = (
        pcm16_to_payload(pcm)
        if args.payload_format == "lab"
        else pcm16_to_nicam_payload_j17(pcm, j17)
    )
    bits = build_frame(
        payload,
        frame_index=frame_index,
        mode=0,
        fallback=0,
        station_id=station_id,
    )
    symbols = bits_to_symbols(bits, phase_quarter)
    phase_quarter = final_phase_quarter(bits, phase_quarter)
    if args.pulse_shape:
        iq = shape_symbols(
            symbols,
            args.sample_rate,
            rolloff=args.pulse_rolloff,
            span_symbols=args.pulse_span_symbols,
        )
    else:
        iq = upsample_symbols(symbols, args.sample_rate)
    if args.baseband_filter:
        iq = filter_baseband(
            iq,
            args.sample_rate,
            cutoff_hz=args.baseband_filter_cutoff,
            taps=args.baseband_filter_taps,
        )
    return iq, phase_quarter


def iq_producer(
    args: argparse.Namespace,
    buffers: "queue.Queue[np.ndarray | None]",
    stop_event: threading.Event,
) -> None:
    source = open_audio_source(args)
    j17 = J17Preemphasis()
    frame_bytes = PCM_VALUES_PER_NICAM_FRAME * 2
    frame_index = 0
    phase_quarter = 0
    max_frames = None if args.seconds is None else int(args.seconds * 1000)
    station_id = args.station_id[:8] if args.station_id is not None else None
    baseband_amplitude = nicam_level_to_amplitude(args)

    try:
        while not stop_event.is_set():
            if max_frames is not None and frame_index >= max_frames:
                break
            chunks: list[np.ndarray] = []
            for _ in range(args.frames_per_buffer):
                if max_frames is not None and frame_index >= max_frames:
                    break
                raw = source.read(frame_bytes)
                if len(raw) < frame_bytes:
                    raw += b"\x00" * (frame_bytes - len(raw))
                pcm = np.frombuffer(raw, dtype=np.int16)
                iq, phase_quarter = build_iq_frame(
                    args,
                    pcm,
                    j17,
                    frame_index,
                    phase_quarter,
                    station_id,
                )
                chunks.append(iq)
                frame_index += 1
            if not chunks:
                break
            iq_buffer = np.concatenate(chunks).astype(np.complex64, copy=False)
            scaled = scale_for_pluto(iq_buffer * baseband_amplitude, args.pluto_amplitude)
            buffers.put(scaled)
    finally:
        source.close()
        buffers.put(None)


def run(args: argparse.Namespace) -> int:
    if args.sample_rate % SYMBOL_RATE:
        raise SystemExit("--sample-rate moet een veelvoud van 364000 zijn")
    if args.frames_per_buffer <= 0:
        raise SystemExit("--frames-per-buffer moet positief zijn")

    buffers: "queue.Queue[np.ndarray | None]" = queue.Queue(maxsize=args.queue_buffers)
    stop_event = threading.Event()
    producer = threading.Thread(
        target=iq_producer,
        args=(args, buffers, stop_event),
        name="nicam-pluto-iq-producer",
        daemon=True,
    )
    producer.start()

    sdr = None
    if not args.dry_run:
        tx_buffer_samples = args.frames_per_buffer * args.sample_rate // 1000
        sdr = init_pluto(
            args.uri,
            args.sample_rate,
            args.lo,
            args.tx_gain,
            args.rf_bandwidth,
            cyclic=False,
            buffer_samples=tx_buffer_samples,
        )

    tx_buffers = 0
    generated_buffers = 0
    next_report = time.monotonic() + args.status_interval
    started = False

    try:
        while True:
            item = buffers.get()
            if item is None:
                break
            generated_buffers += 1
            if not started:
                while buffers.qsize() < args.prebuffer_buffers and producer.is_alive():
                    time.sleep(0.005)
                started = True
            if sdr is not None:
                sdr.tx(item)
            tx_buffers += 1

            if args.status_interval > 0 and time.monotonic() >= next_report:
                print(
                    "pluto_stream_tx: "
                    f"generated_buffers={generated_buffers} "
                    f"tx_buffers={tx_buffers} "
                    f"queue={buffers.qsize()} "
                    f"frames_per_buffer={args.frames_per_buffer}",
                    file=sys.stderr,
                )
                next_report = time.monotonic() + args.status_interval
    finally:
        stop_event.set()
        producer.join(timeout=1.0)
        if sdr is not None:
            try:
                sdr.tx_destroy_buffer()
            except Exception:
                pass

    return 0


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Direct NICAM audio/stream TX via PlutoSDR")
    parser.add_argument("--stream-url", help="audio stream URL decoded by ffmpeg")
    parser.add_argument("--audio-file", help="audio file decoded by ffmpeg")
    parser.add_argument("--udp-url", help="UDP input URL decoded by ffmpeg")
    parser.add_argument("--audio-device", help="Audio input device name or index")
    parser.add_argument("--silence", action="store_true")
    parser.add_argument("--tone", action="store_true")
    parser.add_argument("--tone-hz", type=float, default=1000.0)
    parser.add_argument("--seconds", type=float)
    parser.add_argument("--sample-rate", type=int, default=DEFAULT_SAMPLE_RATE)
    parser.add_argument("--pulse-shape", action="store_true", default=True)
    parser.add_argument("--no-pulse-shape", dest="pulse_shape", action="store_false")
    parser.add_argument("--pulse-rolloff", type=float, default=0.4)
    parser.add_argument("--pulse-span-symbols", type=int, default=6)
    parser.add_argument("--baseband-filter", action="store_true")
    parser.add_argument("--baseband-filter-cutoff", type=float, default=500000.0)
    parser.add_argument("--baseband-filter-taps", type=int, default=17)
    parser.add_argument("--amplitude", type=float)
    parser.add_argument("--nicam-rf-level", type=int, default=200)
    parser.add_argument("--nicam-level-db", type=float)
    parser.add_argument("--payload-format", choices=["nicam728", "lab"], default="nicam728")
    parser.add_argument("--station-id")
    parser.add_argument("--ffmpeg-reconnect", action="store_true")
    parser.add_argument("--input-reconnect-delay", type=float, default=2.0)
    parser.add_argument("--ffmpeg-loglevel", default="error")
    parser.add_argument("--pcm-buffer-ms", type=int, default=1200)
    parser.add_argument("--pcm-read-timeout-ms", type=int, default=120)

    parser.add_argument("--uri", default="ip:192.168.2.1")
    parser.add_argument("--lo", type=int, required=True)
    parser.add_argument("--tx-gain", type=int, default=-10)
    parser.add_argument("--rf-bandwidth", type=int, default=1_750_000)
    parser.add_argument("--pluto-amplitude", type=float, default=3.0)
    parser.add_argument("--frames-per-buffer", type=int, default=50)
    parser.add_argument("--queue-buffers", type=int, default=12)
    parser.add_argument("--prebuffer-buffers", type=int, default=4)
    parser.add_argument("--status-interval", type=float, default=5.0)
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    return run(parse_args(argv))


if __name__ == "__main__":
    raise SystemExit(main())
