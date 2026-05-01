from __future__ import annotations

import argparse
import math
import struct
import subprocess
import sys
import threading
import zlib
from collections import deque
from dataclasses import dataclass
from typing import BinaryIO

import numpy as np


AUDIO_SAMPLE_RATE = 48_000
CHANNELS = 2
FRAME_MS = 20
SAMPLES_PER_FRAME = AUDIO_SAMPLE_RATE * FRAME_MS // 1000
PCM_BYTES_PER_FRAME = SAMPLES_PER_FRAME * CHANNELS * 2
MAGIC = b"OQ"
VERSION = 1
HEADER = struct.Struct(">2sBHI")


@dataclass(frozen=True)
class OpusFrame:
    sequence: int
    payload: bytes


class ToneSource:
    def __init__(self, hz: float, seconds: float | None):
        self.hz = hz
        self.remaining = None if seconds is None else int(seconds * AUDIO_SAMPLE_RATE)
        self.phase = 0

    def read(self, size: int) -> bytes:
        frames = size // (CHANNELS * 2)
        if self.remaining is not None:
            frames = min(frames, self.remaining)
            self.remaining -= frames
        if frames <= 0:
            return b""

        n = np.arange(frames, dtype=np.float32) + self.phase
        self.phase += frames
        left = 0.35 * np.sin(2 * math.pi * self.hz * n / AUDIO_SAMPLE_RATE)
        right = 0.35 * np.sin(2 * math.pi * (self.hz * 1.5) * n / AUDIO_SAMPLE_RATE)
        stereo = np.column_stack([left, right])
        return np.rint(stereo.reshape(-1) * 32767).astype(np.int16).tobytes()

    def close(self) -> None:
        pass


class BufferedPCMSource:
    def __init__(
        self,
        raw: BinaryIO,
        max_bytes: int,
        read_timeout_s: float,
        fill_silence: bool,
    ):
        self._raw = raw
        self._max_bytes = max(PCM_BYTES_PER_FRAME, int(max_bytes))
        self._read_timeout_s = max(0.0, float(read_timeout_s))
        self._fill_silence = bool(fill_silence)
        self._chunks: deque[bytes] = deque()
        self._queued = 0
        self._eof = False
        self._closed = False
        self._lock = threading.Lock()
        self._cv = threading.Condition(self._lock)
        self._reader = threading.Thread(target=self._reader_loop, name="pcm-reader", daemon=True)
        self._reader.start()

    def _reader_loop(self) -> None:
        try:
            while True:
                if self._closed:
                    return
                chunk = self._raw.read(8192)
                if not chunk:
                    return
                with self._cv:
                    while (self._queued + len(chunk) > self._max_bytes) and not self._closed:
                        self._cv.wait(timeout=0.2)
                    if self._closed:
                        return
                    self._chunks.append(chunk)
                    self._queued += len(chunk)
                    self._cv.notify_all()
        finally:
            with self._cv:
                self._eof = True
                self._cv.notify_all()

    def read(self, size: int) -> bytes:
        if size <= 0:
            return b""
        out = bytearray()
        with self._cv:
            while len(out) < size:
                while self._chunks and len(out) < size:
                    chunk = self._chunks.popleft()
                    self._queued -= len(chunk)
                    need = size - len(out)
                    if len(chunk) <= need:
                        out.extend(chunk)
                    else:
                        out.extend(chunk[:need])
                        rest = chunk[need:]
                        self._chunks.appendleft(rest)
                        self._queued += len(rest)
                    self._cv.notify_all()
                if len(out) >= size:
                    break
                if self._eof:
                    break
                notified = self._cv.wait(timeout=self._read_timeout_s)
                if not notified and self._fill_silence:
                    out.extend(b"\x00" * (size - len(out)))
                    break
        return bytes(out)

    def close(self) -> None:
        with self._cv:
            self._closed = True
            self._cv.notify_all()
        try:
            self._raw.close()
        except Exception:
            pass


def open_pcm_source(args: argparse.Namespace) -> tuple[BinaryIO, subprocess.Popen[bytes] | None]:
    if args.tone:
        return ToneSource(args.tone_hz, args.seconds), None
    if args.audio_in == "-":
        return sys.stdin.buffer, None

    source = args.stream_url or args.audio_file
    if source is None:
        raise SystemExit("Gebruik --stream-url, --audio-file, --audio-in - of --tone")

    cmd = ["ffmpeg", "-hide_banner", "-loglevel", args.ffmpeg_loglevel]
    if args.stream_url and args.ffmpeg_reconnect:
        cmd.extend(
            [
                "-reconnect",
                "1",
                "-reconnect_streamed",
                "1",
                "-reconnect_delay_max",
                "5",
            ]
        )
    cmd.extend(
        [
            "-i",
            source,
            "-f",
            "s16le",
            "-ac",
            str(CHANNELS),
            "-ar",
            str(AUDIO_SAMPLE_RATE),
            "-",
        ]
    )
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=sys.stderr)
    if proc.stdout is None:
        raise RuntimeError("ffmpeg stdout is not available")
    buffer_bytes = int(args.pcm_buffer_ms * AUDIO_SAMPLE_RATE * CHANNELS * 2 / 1000)
    buffered = BufferedPCMSource(
        raw=proc.stdout,
        max_bytes=buffer_bytes,
        read_timeout_s=args.pcm_read_timeout_ms / 1000.0,
        fill_silence=bool(args.pcm_fill_silence),
    )
    return buffered, proc


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


def make_encoder(bitrate: int):
    try:
        import opuslib
    except ImportError as exc:
        raise SystemExit("Installeer opuslib: pip install --user opuslib") from exc

    enc = opuslib.Encoder(AUDIO_SAMPLE_RATE, CHANNELS, opuslib.APPLICATION_AUDIO)
    enc.bitrate = int(bitrate)
    enc.vbr = False
    enc.complexity = 7
    return enc


def make_decoder():
    try:
        import opuslib
    except ImportError as exc:
        raise SystemExit("Installeer opuslib: pip install --user opuslib") from exc

    return opuslib.Decoder(AUDIO_SAMPLE_RATE, CHANNELS)


def encode_packet(sequence: int, payload: bytes) -> bytes:
    if len(payload) > 4095:
        raise ValueError("Opus payload is too large")
    head = HEADER.pack(MAGIC, VERSION, len(payload), sequence & 0xFFFFFFFF)
    crc = zlib.crc32(head + payload) & 0xFFFFFFFF
    return head + payload + struct.pack(">I", crc)


def try_decode_packet(data: bytes) -> OpusFrame | None:
    if len(data) < HEADER.size + 4:
        return None
    magic, version, payload_len, sequence = HEADER.unpack_from(data)
    if magic != MAGIC or version != VERSION:
        return None
    total = HEADER.size + payload_len + 4
    if len(data) != total:
        return None
    expected = struct.unpack(">I", data[-4:])[0]
    actual = zlib.crc32(data[:-4]) & 0xFFFFFFFF
    if actual != expected:
        return None
    return OpusFrame(sequence=sequence, payload=data[HEADER.size:-4])


def packet_total_len_from_header(data: bytes) -> int | None:
    if len(data) < HEADER.size:
        return None
    magic, version, payload_len, _sequence = HEADER.unpack_from(data)
    if magic != MAGIC or version != VERSION or payload_len > 4095:
        return None
    return HEADER.size + payload_len + 4
