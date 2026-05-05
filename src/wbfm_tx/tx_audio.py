from __future__ import annotations

import math
import queue
import shutil
import socket
import subprocess
import sys
import threading
import time
from collections import deque
from dataclasses import dataclass
from urllib.parse import parse_qs, urlparse
from typing import BinaryIO, Callable, Deque, Optional, Union

import numpy as np


@dataclass(frozen=True)
class PCMFormat:
    sample_rate: int
    channels: int = 2
    sample_width: int = 2

    @property
    def bytes_per_frame(self) -> int:
        return self.channels * self.sample_width

    def bytes_for_frames(self, frames: int) -> int:
        return int(frames) * self.bytes_per_frame


class SilenceSource:
    def read(self, size: int) -> bytes:
        return b"\x00" * max(0, int(size))

    def close(self) -> None:
        pass


class RawUDPPCMSource:
    """Raw s16le UDP input on top of a permanent silent audio bus."""

    def __init__(
        self,
        url: str,
        fmt: PCMFormat,
        max_bytes: int,
        read_timeout_s: float,
        packet_bytes: int,
        prebuffer_ms: int = 750,
    ) -> None:
        parsed = urlparse(url)
        if parsed.scheme != "udp" or parsed.port is None:
            raise ValueError("UDP audio URL moet de vorm udp://host:port hebben")
        bind_host = parsed.hostname or "0.0.0.0"
        params = parse_qs(parsed.query)
        bind_host = params.get("localaddr", [bind_host])[0]
        self.fmt = fmt
        requested_prebuffer = int(prebuffer_ms * fmt.sample_rate * fmt.bytes_per_frame / 1000)
        self._max_bytes = max(fmt.bytes_per_frame, int(max_bytes), requested_prebuffer * 2)
        self._read_timeout_s = max(0.0, float(read_timeout_s))
        self._packet_bytes = max(fmt.bytes_per_frame, int(packet_bytes))
        self._prebuffer_bytes = max(fmt.bytes_per_frame, min(self._max_bytes, requested_prebuffer))
        self._chunks: Deque[bytes] = deque()
        self._queued = 0
        self._active = False
        self._underflows = 0
        self._stats_packets = 0
        self._stats_dropped_bytes = 0
        self._stats_underflow_reads = 0
        self._stats_rebuffers = 0
        self._stats_started = 0
        self._last_stats = time.monotonic()
        self._closed = False
        self._lock = threading.Lock()
        self._cv = threading.Condition(self._lock)
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.settimeout(0.2)
        self._sock.bind((bind_host, int(parsed.port)))
        self._thread = threading.Thread(target=self._reader_loop, name="tx-udp-pcm-reader", daemon=True)
        self._thread.start()

    def _reader_loop(self) -> None:
        while True:
            with self._lock:
                if self._closed:
                    return
            try:
                chunk, _addr = self._sock.recvfrom(self._packet_bytes)
            except socket.timeout:
                continue
            except OSError:
                return
            if not chunk:
                continue
            frames = len(chunk) // self.fmt.bytes_per_frame
            if frames <= 0:
                continue
            chunk = chunk[: frames * self.fmt.bytes_per_frame]
            with self._cv:
                if self._queued + len(chunk) > self._max_bytes:
                    drop = (self._queued + len(chunk)) - self._prebuffer_bytes
                    while self._chunks and drop > 0:
                        old = self._chunks.popleft()
                        self._queued -= len(old)
                        drop -= len(old)
                        self._stats_dropped_bytes += len(old)
                if self._closed:
                    return
                self._chunks.append(chunk)
                self._queued += len(chunk)
                self._stats_packets += 1
                self._cv.notify_all()
            self._maybe_log_stats()

    def _maybe_log_stats(self) -> None:
        now = time.monotonic()
        if now - self._last_stats < 2.0:
            return
        self._last_stats = now
        queued_ms = self._queued / self.fmt.bytes_per_frame / self.fmt.sample_rate * 1000.0
        print(
            "tx_udp_audio: "
            f"queued_ms={queued_ms:.0f} active={int(self._active)} "
            f"packets={self._stats_packets} underflows={self._stats_underflow_reads} "
            f"rebuffers={self._stats_rebuffers} dropped_ms="
            f"{self._stats_dropped_bytes / self.fmt.bytes_per_frame / self.fmt.sample_rate * 1000.0:.0f}",
            file=sys.stderr,
        )

    def read(self, size: int) -> bytes:
        size = max(0, int(size))
        if size == 0:
            return b""
        out = bytearray()
        with self._cv:
            if not self._active:
                if self._queued < self._prebuffer_bytes:
                    return b"\x00" * size
                self._active = True
                self._stats_started += 1
                self._underflows = 0
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
        if len(out) < size:
            self._underflows += 1
            self._stats_underflow_reads += 1
            if self._underflows >= 50:
                with self._cv:
                    if self._queued < self._prebuffer_bytes:
                        self._active = False
                        self._stats_rebuffers += 1
                        self._underflows = 0
            out.extend(b"\x00" * (size - len(out)))
        else:
            self._underflows = 0
        return bytes(out)

    def close(self) -> None:
        with self._cv:
            self._closed = True
            self._cv.notify_all()
        self._sock.close()


class ToneSource:
    def __init__(self, fmt: PCMFormat, hz: float, seconds: Optional[float]) -> None:
        self.fmt = fmt
        self.hz = float(hz)
        self.remaining = None if seconds is None else int(seconds * fmt.sample_rate)
        self.phase = 0

    def read(self, size: int) -> bytes:
        frames = max(0, int(size) // self.fmt.bytes_per_frame)
        if self.remaining is not None:
            frames = min(frames, self.remaining)
            self.remaining -= frames
        if frames <= 0:
            return b"\x00" * max(0, int(size))

        n = np.arange(frames, dtype=np.float32) + self.phase
        self.phase += frames
        wave = 0.35 * np.sin(2 * math.pi * self.hz * n / self.fmt.sample_rate)
        channels = [wave for _ in range(self.fmt.channels)]
        pcm = np.column_stack(channels)
        raw = np.rint(pcm.reshape(-1) * 32767).astype("<i2").tobytes()
        if len(raw) < size:
            raw += b"\x00" * (size - len(raw))
        return raw[:size]

    def close(self) -> None:
        pass


class BufferedPCMSource:
    """Background PCM reader that turns source stalls into silence."""

    def __init__(
        self,
        reader: Callable[[], BinaryIO],
        max_bytes: int,
        read_timeout_s: float,
        restart_delay_s: float,
        restart: bool,
        chunk_bytes: int = 8192,
    ) -> None:
        self._reader_factory = reader
        self._max_bytes = max(chunk_bytes, int(max_bytes))
        self._read_timeout_s = max(0.0, float(read_timeout_s))
        self._restart_delay_s = max(0.0, float(restart_delay_s))
        self._restart = bool(restart)
        self._chunk_bytes = max(1, int(chunk_bytes))
        self._chunks: Deque[bytes] = deque()
        self._queued = 0
        self._closed = False
        self._lock = threading.Lock()
        self._cv = threading.Condition(self._lock)
        self._reader: Optional[BinaryIO] = None
        self._thread = threading.Thread(target=self._reader_loop, name="tx-pcm-reader", daemon=True)
        self._thread.start()

    def _reader_loop(self) -> None:
        while True:
            with self._lock:
                if self._closed:
                    return
            try:
                reader = self._reader_factory()
                with self._lock:
                    self._reader = reader
                while True:
                    chunk = reader.read(self._chunk_bytes)
                    if not chunk:
                        break
                    with self._cv:
                        while self._queued + len(chunk) > self._max_bytes and not self._closed:
                            self._cv.wait(timeout=0.2)
                        if self._closed:
                            return
                        self._chunks.append(chunk)
                        self._queued += len(chunk)
                        self._cv.notify_all()
            except Exception as exc:
                if not self._closed:
                    print(f"TX audio input stopte: {exc}", file=sys.stderr)
            finally:
                with self._lock:
                    reader = self._reader
                    self._reader = None
                if reader is not None:
                    try:
                        reader.close()
                    except Exception:
                        pass

            if not self._restart:
                return
            time.sleep(self._restart_delay_s)

    def read(self, size: int) -> bytes:
        size = max(0, int(size))
        out = bytearray()
        with self._cv:
            while len(out) < size and not self._closed:
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
                self._cv.wait(timeout=self._read_timeout_s)
                if len(out) < size:
                    out.extend(b"\x00" * (size - len(out)))
                    break
        if len(out) < size:
            out.extend(b"\x00" * (size - len(out)))
        return bytes(out)

    def close(self) -> None:
        with self._cv:
            self._closed = True
            self._cv.notify_all()
        with self._lock:
            reader = self._reader
        if reader is not None:
            try:
                reader.close()
            except Exception:
                pass


class _ProcessStdout:
    def __init__(self, proc: subprocess.Popen[bytes]) -> None:
        self.proc = proc
        if proc.stdout is None:
            raise RuntimeError("ffmpeg stdout is niet beschikbaar")
        self.stdout = proc.stdout

    def read(self, size: int) -> bytes:
        return self.stdout.read(size)

    def close(self) -> None:
        try:
            self.stdout.close()
        except Exception:
            pass
        if self.proc.poll() is None:
            self.proc.terminate()
        try:
            self.proc.wait(timeout=2)
        except Exception:
            self.proc.kill()
            self.proc.wait(timeout=2)


class ProcessPCMSource(BufferedPCMSource):
    def __init__(
        self,
        command_factory: Callable[[], list[str]],
        max_bytes: int,
        read_timeout_s: float,
        restart_delay_s: float,
        restart: bool,
    ) -> None:
        self._command_factory = command_factory
        self._proc: Optional[subprocess.Popen[bytes]] = None
        super().__init__(
            reader=self._open_process_stdout,
            max_bytes=max_bytes,
            read_timeout_s=read_timeout_s,
            restart_delay_s=restart_delay_s,
            restart=restart,
        )

    def _open_process_stdout(self) -> BinaryIO:
        cmd = self._command_factory()
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=sys.stderr)
        self._proc = proc
        return _ProcessStdout(proc)  # type: ignore[return-value]

    def close(self) -> None:
        super().close()
        proc = self._proc
        if proc is not None:
            if proc.poll() is None:
                proc.terminate()
            try:
                proc.wait(timeout=2)
            except Exception:
                proc.kill()
                proc.wait(timeout=2)


class DevicePCMSource:
    def __init__(
        self,
        fmt: PCMFormat,
        device: Optional[Union[str, int]],
        block_frames: int,
        read_timeout_s: float,
    ) -> None:
        import sounddevice as sd

        self.fmt = fmt
        self._block_bytes = fmt.bytes_for_frames(block_frames)
        self._timeout_s = max(0.0, float(read_timeout_s))
        self._queue: queue.Queue[bytes] = queue.Queue(maxsize=12)
        self._stream = sd.InputStream(
            samplerate=fmt.sample_rate,
            channels=fmt.channels,
            dtype="int16",
            blocksize=block_frames,
            device=device,
            callback=self._callback,
        )
        self._stream.start()

    def _callback(self, indata, _frames, _time, status) -> None:
        if status:
            print(f"Audio-device status: {status}", file=sys.stderr, flush=True)
        try:
            self._queue.put_nowait(np.asarray(indata, dtype="<i2").tobytes())
        except queue.Full:
            pass

    def read(self, size: int) -> bytes:
        out = bytearray()
        while len(out) < size:
            try:
                chunk = self._queue.get(timeout=self._timeout_s)
            except queue.Empty:
                out.extend(b"\x00" * (size - len(out)))
                break
            need = size - len(out)
            out.extend(chunk[:need])
            if len(chunk) > need:
                try:
                    self._queue.put_nowait(chunk[need:])
                except queue.Full:
                    pass
        return bytes(out)

    def close(self) -> None:
        self._stream.stop()
        self._stream.close()


def ffmpeg_pcm_command(
    input_url: str,
    fmt: PCMFormat,
    reconnect: bool,
    loglevel: str,
    input_format: Optional[str] = None,
) -> list[str]:
    if shutil.which("ffmpeg") is None:
        raise RuntimeError("ffmpeg is nodig voor deze TX-audiobron, maar staat niet in PATH")

    cmd = ["ffmpeg", "-hide_banner", "-loglevel", loglevel]
    if reconnect:
        cmd.extend(["-reconnect", "1", "-reconnect_streamed", "1", "-reconnect_delay_max", "5"])
    if input_format:
        cmd.extend(["-f", input_format])
    cmd.extend(
        [
            "-i",
            input_url,
            "-vn",
            "-f",
            "s16le",
            "-ac",
            str(fmt.channels),
            "-ar",
            str(fmt.sample_rate),
            "-",
        ]
    )
    return cmd


def open_constant_pcm_source(
    source: str,
    fmt: PCMFormat,
    block_frames: int,
    stream_url: Optional[str] = None,
    audio_file: Optional[str] = None,
    device: Optional[Union[str, int]] = None,
    udp_url: Optional[str] = None,
    tone_hz: float = 1000.0,
    seconds: Optional[float] = None,
    reconnect: bool = True,
    reconnect_delay_s: float = 2.0,
    read_timeout_s: float = 0.12,
    buffer_ms: int = 1200,
    ffmpeg_loglevel: str = "error",
) -> BinaryIO:
    source = source.lower()
    if source == "silence":
        return SilenceSource()
    if source == "tone":
        return ToneSource(fmt, tone_hz, seconds)
    if source == "device":
        return DevicePCMSource(fmt, device, block_frames, read_timeout_s)

    max_bytes = max(fmt.bytes_for_frames(block_frames), int(buffer_ms * fmt.sample_rate * fmt.bytes_per_frame / 1000))
    if source == "stream":
        if not stream_url:
            raise ValueError("stream_url is verplicht bij TX-audiobron stream")
        return ProcessPCMSource(
            command_factory=lambda: ffmpeg_pcm_command(stream_url, fmt, reconnect, ffmpeg_loglevel),
            max_bytes=max_bytes,
            read_timeout_s=read_timeout_s,
            restart_delay_s=reconnect_delay_s,
            restart=True,
        )
    if source in ("file", "wav"):
        if not audio_file:
            raise ValueError("audio_file is verplicht bij TX-audiobron file")
        return ProcessPCMSource(
            command_factory=lambda: ffmpeg_pcm_command(audio_file, fmt, False, ffmpeg_loglevel),
            max_bytes=max_bytes,
            read_timeout_s=read_timeout_s,
            restart_delay_s=reconnect_delay_s,
            restart=False,
        )
    if source == "udp":
        if not udp_url:
            raise ValueError("udp_url is verplicht bij TX-audiobron udp")
        return RawUDPPCMSource(
            url=udp_url,
            fmt=fmt,
            max_bytes=max_bytes,
            read_timeout_s=read_timeout_s,
            packet_bytes=max(fmt.bytes_for_frames(block_frames), 8192),
        )
    raise ValueError(f"Onbekende TX-audiobron: {source}")


def pcm16_blocks(source: BinaryIO, fmt: PCMFormat, block_frames: int):
    block_bytes = fmt.bytes_for_frames(block_frames)
    while True:
        raw = source.read(block_bytes)
        if len(raw) < block_bytes:
            raw += b"\x00" * (block_bytes - len(raw))
        yield raw[:block_bytes]


def float32_stereo_blocks(source: BinaryIO, fmt: PCMFormat, block_frames: int):
    for raw in pcm16_blocks(source, fmt, block_frames):
        samples = np.frombuffer(raw, dtype="<i2").astype(np.float32)
        yield (samples.reshape(-1, fmt.channels) / 32768.0).astype(np.float32, copy=False)
