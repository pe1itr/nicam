from __future__ import annotations

import argparse
import sys

import numpy as np

from nicam.dqpsk import upsample_symbols
from nicam.iq import complex_to_u8_iq
from nicam.pipe import suppress_stdout_broken_pipe
from nicam.qpsk_dsp import map_bits_to_symbols
from nicam.rtlsdr_rx import DEFAULT_SAMPLE_RATE

from .frames import (
    PCM_BYTES_PER_FRAME,
    SAMPLES_PER_FRAME,
    encode_packet,
    make_encoder,
    open_pcm_source,
    read_exact,
)


PREAMBLE = b"\x55" * 64
FEC_MAGIC = b"F3"
FEC_DATA_FRAMES = 3
FEC_BLOCK_FRAMES = 4


def bytes_to_bits(data: bytes) -> np.ndarray:
    return np.unpackbits(np.frombuffer(data, dtype=np.uint8))


def wrap_fec_data(block_id: int, data_index: int, payload: bytes) -> bytes:
    return (
        FEC_MAGIC
        + block_id.to_bytes(4, "big")
        + bytes([data_index & 0xFF])
        + len(payload).to_bytes(2, "big")
        + payload
    )


def wrap_fec_parity(block_id: int, parity_payload: bytes) -> bytes:
    return (
        FEC_MAGIC
        + block_id.to_bytes(4, "big")
        + bytes([3])
        + len(parity_payload).to_bytes(2, "big")
        + parity_payload
    )


def xor_parity_payload(payloads: list[bytes]) -> bytes:
    max_len = max((len(p) for p in payloads), default=0)
    if max_len == 0:
        return b""
    parity = bytearray(max_len)
    for payload in payloads:
        for idx, value in enumerate(payload):
            parity[idx] ^= value
    return bytes(parity)


def run(args: argparse.Namespace) -> int:
    source, proc = open_pcm_source(args)
    encoder = make_encoder(args.bitrate)
    out = sys.stdout.buffer if args.out == "-" else open(args.out, "wb")
    sequence = 0
    max_frames = None if args.seconds is None else int(args.seconds * 1000 / 20)
    phase_quarter = 0
    pending = bytearray()
    stream_started = False
    fec_block_id = 0
    fec_data: list[bytes] = []

    try:
        while max_frames is None or sequence < max_frames:
            pcm = read_exact(source, PCM_BYTES_PER_FRAME)
            if not pcm:
                break
            if len(pcm) < PCM_BYTES_PER_FRAME:
                pcm += b"\x00" * (PCM_BYTES_PER_FRAME - len(pcm))

            opus = encoder.encode(pcm, SAMPLES_PER_FRAME)
            tx_payloads: list[bytes]
            if args.fec == "3/4":
                fec_data.append(opus)
                tx_payloads = [wrap_fec_data(fec_block_id, len(fec_data) - 1, opus)]
                if len(fec_data) == FEC_DATA_FRAMES:
                    tx_payloads.append(wrap_fec_parity(fec_block_id, xor_parity_payload(fec_data)))
                    fec_data.clear()
                    fec_block_id = (fec_block_id + 1) & 0xFFFFFFFF
            else:
                tx_payloads = [opus]

            for payload in tx_payloads:
                packet = PREAMBLE + encode_packet(sequence, payload)
                bits = bytes_to_bits(packet)
                if not stream_started:
                    bits = np.concatenate([np.zeros(2, dtype=np.uint8), bits])
                    stream_started = True
                symbols, phase_quarter = map_bits_to_symbols(bits, phase_quarter, args.dsp_backend)
                iq = upsample_symbols(symbols, args.sample_rate)
                pending.extend(complex_to_u8_iq(iq, args.amplitude))

                if sequence % args.flush_frames == args.flush_frames - 1:
                    try:
                        out.write(pending)
                        out.flush()
                        pending.clear()
                    except BrokenPipeError:
                        suppress_stdout_broken_pipe()
                        return 0
                sequence += 1

        if pending:
            try:
                out.write(pending)
                out.flush()
            except BrokenPipeError:
                suppress_stdout_broken_pipe()
                return 0
    finally:
        source.close()
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except Exception:
                proc.kill()
        if out is not sys.stdout.buffer:
            out.close()

    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Encode stereo audio as Opus over direct DQPSK/QPSK IQ")
    parser.add_argument("--stream-url", help="audio stream URL decoded by ffmpeg")
    parser.add_argument("--audio-file", help="audio file decoded by ffmpeg")
    parser.add_argument(
        "--audio-in",
        choices=["-"],
        help="read raw s16le stereo 48k PCM from stdin (use -)",
    )
    parser.add_argument("--tone", action="store_true", help="generate an internal stereo test tone")
    parser.add_argument("--tone-hz", type=float, default=1000.0)
    parser.add_argument("--seconds", type=float)
    parser.add_argument("--bitrate", type=int, default=128_000, help="Opus bitrate in bit/s")
    parser.add_argument("--sample-rate", type=int, default=DEFAULT_SAMPLE_RATE)
    parser.add_argument("--amplitude", type=float, default=0.7)
    parser.add_argument("--out", default="-", help="rtl_sdr-style uint8 IQ output, or - for stdout")
    parser.add_argument("--flush-frames", type=int, default=10)
    parser.add_argument("--fec", choices=["off", "3/4"], default="off", help="FEC mode")
    parser.add_argument("--dsp-backend", choices=["py", "c"], default="py", help="DSP backend for QPSK mapping")
    parser.add_argument(
        "--ffmpeg-reconnect",
        action="store_true",
        help="enable ffmpeg reconnect flags when using --stream-url",
    )
    parser.add_argument(
        "--ffmpeg-loglevel",
        default="error",
        help="ffmpeg loglevel for --stream-url/--audio-file (default: error)",
    )
    parser.add_argument(
        "--pcm-buffer-ms",
        type=int,
        default=1200,
        help="PCM jitter buffer size in ms for ffmpeg inputs (default: 1200)",
    )
    parser.add_argument(
        "--pcm-read-timeout-ms",
        type=int,
        default=120,
        help="wait time before underrun handling in ms (default: 120)",
    )
    parser.add_argument(
        "--pcm-fill-silence",
        action="store_true",
        dest="pcm_fill_silence",
        help="on short input stalls, fill missing PCM with silence",
    )
    parser.add_argument(
        "--no-pcm-fill-silence",
        action="store_false",
        dest="pcm_fill_silence",
        help="disable silence fill on short input stalls",
    )
    parser.set_defaults(pcm_fill_silence=False)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    return run(parse_args(sys.argv[1:] if argv is None else argv))


if __name__ == "__main__":
    raise SystemExit(main())
