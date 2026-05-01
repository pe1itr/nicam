from __future__ import annotations

import argparse
import subprocess
import sys
import wave
from collections import deque
from typing import BinaryIO

import numpy as np

from nicam.dqpsk import adaptive_symbols_from_samples, coarse_symbols_from_samples
from nicam.iq import u8_iq_to_complex
from nicam.pipe import suppress_stdout_broken_pipe
from nicam.qpsk_dsp import demap_symbols_to_bits
from nicam.rtlsdr_rx import DEFAULT_SAMPLE_RATE

from .frames import (
    AUDIO_SAMPLE_RATE,
    CHANNELS,
    HEADER,
    make_decoder,
    packet_total_len_from_header,
    try_decode_packet,
)
from .qpsk_tx import PREAMBLE


PREAMBLE_BITS = len(PREAMBLE) * 8
FEC_MAGIC = b"F3"
FEC_DATA_FRAMES = 3
FEC_PARITY_INDEX = 3

try:
    from nicam import _adaptive_demod
except ImportError:
    _adaptive_demod = None


def open_iq_input(args: argparse.Namespace) -> tuple[BinaryIO, subprocess.Popen[bytes] | None]:
    if args.freq is not None:
        cmd = [
            "rtl_sdr",
            "-f",
            str(args.freq),
            "-s",
            str(args.sample_rate),
            "-p",
            str(args.ppm),
            "-",
        ]
        if args.gain != "auto":
            cmd[1:1] = ["-g", str(args.gain)]
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=sys.stderr)
        if proc.stdout is None:
            raise RuntimeError("rtl_sdr stdout is not available")
        return proc.stdout, proc

    if args.iq_in == "-":
        return sys.stdin.buffer, None
    return open(args.iq_in, "rb"), None


def open_audio_output(path: str):
    if path == "-":
        return sys.stdout.buffer, None
    if path.lower().endswith(".wav"):
        wav = wave.open(path, "wb")
        wav.setnchannels(CHANNELS)
        wav.setsampwidth(2)
        wav.setframerate(AUDIO_SAMPLE_RATE)
        return wav, wav
    return open(path, "wb"), None


def write_pcm(out, pcm: bytes, flush: bool = False) -> None:
    if isinstance(out, wave.Wave_write):
        out.writeframesraw(pcm)
    else:
        out.write(pcm)
        if flush:
            out.flush()


def read_exact(source: BinaryIO, size: int) -> bytes:
    chunks: list[bytes] = []
    remaining = size
    while remaining > 0:
        chunk = source.read(remaining)
        if not chunk:
            break
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def bits_to_bytes(bits: np.ndarray) -> bytes:
    usable = (bits.size // 8) * 8
    if usable <= 0:
        return b""
    return np.packbits(bits[:usable]).tobytes()


def find_packet(bits: np.ndarray) -> tuple[int, int, bytes] | None:
    for shift in range(8):
        packed = bits_to_bytes(bits[shift:])
        preamble_at = packed.find(PREAMBLE)
        if preamble_at < 0:
            continue

        packet_byte_at = preamble_at + len(PREAMBLE)
        if len(packed) < packet_byte_at + HEADER.size:
            return None

        total_len = packet_total_len_from_header(packed[packet_byte_at : packet_byte_at + HEADER.size])
        if total_len is None:
            continue
        if len(packed) < packet_byte_at + total_len:
            return None

        packet = packed[packet_byte_at : packet_byte_at + total_len]
        decoded = try_decode_packet(packet)
        if decoded is None:
            continue

        start_bit = shift + preamble_at * 8
        stop_bit = shift + (packet_byte_at + total_len) * 8
        return start_bit, stop_bit, packet
    return None


def score_phase(bits: np.ndarray) -> int:
    score = 0
    for shift in range(8):
        score += bits_to_bytes(bits[shift:]).count(PREAMBLE)
    return score


def parse_fec_payload(payload: bytes) -> tuple[int, int, int, bytes] | None:
    if len(payload) < 9 or payload[:2] != FEC_MAGIC:
        return None
    block_id = int.from_bytes(payload[2:6], "big")
    index = payload[6]
    length = int.from_bytes(payload[7:9], "big")
    data = payload[9:]
    if length > len(data):
        return None
    return block_id, index, length, data[:length]


def xor_recover_missing(parts: list[bytes | None], parity: bytes, missing_index: int) -> bytes:
    recovered = bytearray(parity)
    for idx, part in enumerate(parts):
        if idx == missing_index or part is None:
            continue
        limit = min(len(recovered), len(part))
        for j in range(limit):
            recovered[j] ^= part[j]
    return bytes(recovered)


def run(args: argparse.Namespace) -> int:
    if args.fec == "3/4" and not args.adaptive_demod:
        raise SystemExit("--fec 3/4 vereist --adaptive-demod")

    source, proc = open_iq_input(args)
    decoder = make_decoder()
    audio_out, closeable_audio = open_audio_output(args.audio_out)
    sps = int(round(args.sample_rate / args.symbol_rate))
    if abs(args.sample_rate / args.symbol_rate - sps) > 1e-9:
        raise SystemExit("--sample-rate moet een geheel aantal samples per symbool zijn")

    previous_symbols: list[complex | np.complex64 | None] = [None] * sps
    def make_adaptive_states():
        if not args.adaptive_demod or _adaptive_demod is None:
            return None
        return [
            _adaptive_demod.create(
                args.sample_rate,
                args.symbol_rate,
                phase,
                args.freq_offset,
            )
            for phase in range(sps)
        ]

    adaptive_states = make_adaptive_states()
    locked_phase = args.timing_phase
    bit_buffer: deque[int] = deque(maxlen=args.max_buffer_bits)
    chunk_samples = args.chunk_symbols * sps
    decoded_frames = 0
    lost_packets = 0
    reordered_packets = 0
    recovered_frames = 0
    lost_audio_frames = 0
    expected_sequence: int | None = None
    fec_block_id: int | None = None
    fec_parts: list[bytes | None] = [None, None, None]
    fec_parity: bytes | None = None
    chunks_without_frame = 0

    def write_plc_frame() -> bool:
        nonlocal lost_audio_frames
        plc_pcm = decoder.decode(b"", frame_size=960, decode_fec=False)
        try:
            write_pcm(audio_out, plc_pcm, flush=False)
        except BrokenPipeError:
            suppress_stdout_broken_pipe()
            return False
        lost_audio_frames += 1
        return True

    def flush_fec_block() -> bool:
        nonlocal decoded_frames, recovered_frames
        parts = list(fec_parts)
        missing = [i for i, part in enumerate(parts) if part is None]
        if len(missing) == 1 and fec_parity is not None:
            parts[missing[0]] = xor_recover_missing(parts, fec_parity, missing[0])
            recovered_frames += 1

        for part in parts:
            if part is None:
                if not write_plc_frame():
                    return False
                continue
            pcm = decoder.decode(part, frame_size=960, decode_fec=False)
            try:
                write_pcm(audio_out, pcm, flush=decoded_frames % args.flush_frames == 0)
            except BrokenPipeError:
                suppress_stdout_broken_pipe()
                return False
            decoded_frames += 1
        return True

    try:
        while True:
            raw = read_exact(source, chunk_samples * 2)
            if not raw:
                break
            iq = u8_iq_to_complex(raw)
            if not iq.size:
                continue
            decoded_before_chunk = decoded_frames

            if locked_phase is None:
                phase_bits: list[np.ndarray] = []
                scores: list[int] = []
                for phase in range(sps):
                    if adaptive_states is not None:
                        bits_raw, _symbols_count, _evm, _snr, _tracked_freq = _adaptive_demod.process(
                            adaptive_states[phase],
                            raw,
                            args.pll_adjustment,
                        )
                        bits = np.frombuffer(bits_raw, dtype=np.uint8)
                    else:
                        if args.adaptive_demod:
                            symbols = adaptive_symbols_from_samples(
                                iq,
                                args.sample_rate,
                                phase,
                                args.freq_offset,
                                args.symbol_rate,
                                args.pll_adjustment,
                            )
                        else:
                            symbols = coarse_symbols_from_samples(
                                iq,
                                args.sample_rate,
                                phase,
                                args.freq_offset,
                            )
                        bits, previous_symbols[phase] = demap_symbols_to_bits(
                            symbols, previous_symbols[phase], args.dsp_backend
                        )
                    phase_bits.append(bits)
                    scores.append(score_phase(bits))
                best = int(np.argmax(scores))
                if scores[best] <= 0:
                    continue
                locked_phase = best
                bit_buffer.extend(int(x) for x in phase_bits[best])
                if args.verbose:
                    print(f"locked timing_phase={locked_phase} score={scores[best]}", file=sys.stderr)
            else:
                if adaptive_states is not None:
                    bits_raw, _symbols_count, _evm, _snr, _tracked_freq = _adaptive_demod.process(
                        adaptive_states[locked_phase],
                        raw,
                        args.pll_adjustment,
                    )
                    bits = np.frombuffer(bits_raw, dtype=np.uint8)
                else:
                    if args.adaptive_demod:
                        symbols = adaptive_symbols_from_samples(
                            iq,
                            args.sample_rate,
                            locked_phase,
                            args.freq_offset,
                            args.symbol_rate,
                            args.pll_adjustment,
                        )
                    else:
                        symbols = coarse_symbols_from_samples(
                            iq,
                            args.sample_rate,
                            locked_phase,
                            args.freq_offset,
                        )
                    bits, previous_symbols[locked_phase] = demap_symbols_to_bits(
                        symbols, previous_symbols[locked_phase], args.dsp_backend
                    )
                bit_buffer.extend(int(x) for x in bits)

            while len(bit_buffer) >= PREAMBLE_BITS + HEADER.size * 8:
                data = np.fromiter(bit_buffer, dtype=np.uint8)
                found = find_packet(data)
                if found is None:
                    if len(bit_buffer) > PREAMBLE_BITS * 3:
                        for _ in range(len(bit_buffer) - PREAMBLE_BITS * 2):
                            bit_buffer.popleft()
                    break

                _start_bit, stop_bit, packet = found
                frame = try_decode_packet(packet)
                if frame is None:
                    break

                for _ in range(min(stop_bit, len(bit_buffer))):
                    bit_buffer.popleft()

                if expected_sequence is not None and frame.sequence != expected_sequence:
                    # Distinguish forward gaps (loss) from backward jumps (reorder/wrap).
                    delta = (frame.sequence - expected_sequence) & 0xFFFFFFFF
                    if delta < 0x80000000:
                        lost_packets += delta
                    else:
                        reordered_packets += 1
                    if args.verbose:
                        print(
                            f"sequence jump: expected={expected_sequence} got={frame.sequence}",
                            file=sys.stderr,
                        )
                expected_sequence = (frame.sequence + 1) & 0xFFFFFFFF

                if args.fec == "3/4":
                    fec = parse_fec_payload(frame.payload)
                    if fec is None:
                        if not write_plc_frame():
                            return 0
                        continue
                    block_id, index, _length, payload = fec
                    if index > FEC_PARITY_INDEX:
                        if not write_plc_frame():
                            return 0
                        continue

                    if fec_block_id is None:
                        fec_block_id = block_id
                    elif block_id != fec_block_id:
                        if block_id > fec_block_id:
                            if not flush_fec_block():
                                return 0
                            for _ in range(block_id - fec_block_id - 1):
                                for _slot in range(FEC_DATA_FRAMES):
                                    if not write_plc_frame():
                                        return 0
                        fec_block_id = block_id
                        fec_parts = [None, None, None]
                        fec_parity = None

                    if index == FEC_PARITY_INDEX:
                        fec_parity = payload
                        if not flush_fec_block():
                            return 0
                        fec_block_id = (fec_block_id + 1) & 0xFFFFFFFF
                        fec_parts = [None, None, None]
                        fec_parity = None
                    else:
                        fec_parts[index] = payload
                else:
                    pcm = decoder.decode(frame.payload, frame_size=960, decode_fec=False)
                    try:
                        write_pcm(audio_out, pcm, flush=decoded_frames % args.flush_frames == 0)
                    except BrokenPipeError:
                        suppress_stdout_broken_pipe()
                        return 0
                    decoded_frames += 1

                if args.verbose and decoded_frames % 50 == 0:
                    print(
                        f"decoded={decoded_frames} lost={lost_packets} reordered={reordered_packets} recovered={recovered_frames} plc={lost_audio_frames} timing_phase={locked_phase}",
                        file=sys.stderr,
                    )

            if decoded_frames == decoded_before_chunk:
                chunks_without_frame += 1
            else:
                chunks_without_frame = 0

            if locked_phase is not None and chunks_without_frame >= args.relock_chunks:
                locked_phase = None
                chunks_without_frame = 0
                bit_buffer.clear()
                previous_symbols = [None] * sps
                adaptive_states = make_adaptive_states()
                expected_sequence = None
                fec_block_id = None
                fec_parts = [None, None, None]
                fec_parity = None
                if args.verbose:
                    print("relock: no valid frames, resetting timing lock", file=sys.stderr)
    finally:
        source.close()
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
        if closeable_audio is not None:
            closeable_audio.close()

    if args.verbose:
        print(
            f"decoded={decoded_frames} lost={lost_packets} reordered={reordered_packets} recovered={recovered_frames} plc={lost_audio_frames}",
            file=sys.stderr,
        )
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Receive Opus over direct DQPSK/QPSK IQ")
    parser.add_argument("--freq", type=int, help="RF center frequency in Hz; omit for stdin/file input")
    parser.add_argument("--iq-in", default="-", help="rtl_sdr-style uint8 IQ input, or - for stdin")
    parser.add_argument("--audio-out", default="-", help="PCM/WAV output; .wav writes a WAV header")
    parser.add_argument("--sample-rate", type=int, default=DEFAULT_SAMPLE_RATE)
    parser.add_argument("--symbol-rate", type=int, default=364_000)
    parser.add_argument("--ppm", type=int, default=0)
    parser.add_argument("--gain", default="auto")
    parser.add_argument("--freq-offset", type=float, default=0.0, help="baseband correction in Hz")
    parser.add_argument("--timing-phase", type=int)
    parser.add_argument("--chunk-symbols", type=int, default=16_384)
    parser.add_argument("--relock-chunks", type=int, default=20, help="relock after N chunks without valid frames")
    parser.add_argument("--max-buffer-bits", type=int, default=1_000_000)
    parser.add_argument("--flush-frames", type=int, default=10)
    parser.add_argument(
        "--adaptive-demod",
        dest="adaptive_demod",
        action="store_true",
        default=True,
        help="enable adaptive demodulation (default: on)",
    )
    parser.add_argument(
        "--no-adaptive-demod",
        dest="adaptive_demod",
        action="store_false",
        help="disable adaptive demodulation",
    )
    parser.add_argument("--pll-adjustment", type=float, default=1.0)
    parser.add_argument("--fec", choices=["off", "3/4"], default="off", help="FEC mode")
    parser.add_argument("--dsp-backend", choices=["py", "c"], default="py", help="DSP backend for QPSK demapping")
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    return run(parse_args(sys.argv[1:] if argv is None else argv))


if __name__ == "__main__":
    raise SystemExit(main())
