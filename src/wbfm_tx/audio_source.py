from __future__ import annotations

import queue
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import Iterator, Optional, Union

import numpy as np
import soundfile as sf

from .filters import as_float32_stereo, resample_audio
from .tx_audio import PCMFormat, float32_stereo_blocks, open_constant_pcm_source


@dataclass(frozen=True)
class AudioBlock:
    samples: np.ndarray
    sample_rate: int


def read_wav(path: str, target_rate: int) -> np.ndarray:
    audio, src_rate = sf.read(path, always_2d=True, dtype="float32")
    stereo = as_float32_stereo(audio)
    return resample_audio(stereo, int(src_rate), target_rate)


def wav_blocks(path: str, target_rate: int, block_size: int, loop: bool) -> Iterator[np.ndarray]:
    data = read_wav(path, target_rate)
    if len(data) == 0:
        raise ValueError(f"WAV-bestand bevat geen audio: {path}")

    while True:
        for start in range(0, len(data), block_size):
            block = data[start : start + block_size]
            if len(block) < block_size:
                pad = np.zeros((block_size - len(block), 2), dtype=np.float32)
                block = np.vstack((block, pad))
            yield block
        if not loop:
            break


def device_blocks(device: str | int | None, sample_rate: int, block_size: int) -> Iterator[np.ndarray]:
    import sounddevice as sd

    audio_queue: queue.Queue[np.ndarray] = queue.Queue(maxsize=12)

    def callback(indata, _frames, _time, status):
        if status:
            print(f"Audio-device status: {status}", flush=True)
        try:
            audio_queue.put_nowait(indata.copy())
        except queue.Full:
            pass

    with sd.InputStream(
        samplerate=sample_rate,
        channels=2,
        dtype="float32",
        blocksize=block_size,
        device=device,
        callback=callback,
    ):
        while True:
            yield as_float32_stereo(audio_queue.get())


def stream_blocks(
    stream_url: str,
    sample_rate: int,
    block_size: int,
    reconnect: bool = True,
    silence_on_stall: bool = True,
    reconnect_delay_s: float = 2.0,
) -> Iterator[np.ndarray]:
    if shutil.which("ffmpeg") is None:
        raise RuntimeError("ffmpeg is nodig voor --source stream, maar staat niet in PATH.")

    bytes_per_block = block_size * 2 * 4
    silence = np.zeros((block_size, 2), dtype=np.float32)

    while True:
        cmd = ["ffmpeg", "-hide_banner", "-loglevel", "warning"]
        if reconnect:
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
                stream_url,
                "-vn",
                "-ac",
                "2",
                "-ar",
                str(sample_rate),
                "-f",
                "f32le",
                "pipe:1",
            ]
        )
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
        if proc.stdout is None:
            raise RuntimeError("Kon ffmpeg stdout niet openen.")
        try:
            while True:
                raw = proc.stdout.read(bytes_per_block)
                if not raw:
                    print("WBFM stream decoder stopte; herstart ffmpeg.", file=sys.stderr)
                    break
                samples = np.frombuffer(raw, dtype="<f4")
                frames = len(samples) // 2
                if frames == 0:
                    if silence_on_stall:
                        yield silence
                    continue
                block = samples[: frames * 2].reshape(frames, 2).astype(np.float32, copy=False)
                if len(block) < block_size:
                    pad = np.zeros((block_size - len(block), 2), dtype=np.float32)
                    block = np.vstack((block, pad))
                yield as_float32_stereo(block)
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=2)
        if silence_on_stall:
            yield silence
        time.sleep(reconnect_delay_s)


def constant_audio_blocks(
    source: str,
    sample_rate: int,
    block_size: int,
    stream_url: Optional[str] = None,
    audio_file: Optional[str] = None,
    device: Optional[Union[str, int]] = None,
    udp_url: Optional[str] = None,
    reconnect: bool = True,
    reconnect_delay_s: float = 2.0,
    read_timeout_s: float = 0.12,
    buffer_ms: int = 1200,
) -> Iterator[np.ndarray]:
    fmt = PCMFormat(sample_rate=sample_rate, channels=2)
    pcm = open_constant_pcm_source(
        source=source,
        fmt=fmt,
        block_frames=block_size,
        stream_url=stream_url,
        audio_file=audio_file,
        device=device,
        udp_url=udp_url,
        reconnect=reconnect,
        reconnect_delay_s=reconnect_delay_s,
        read_timeout_s=read_timeout_s,
        buffer_ms=buffer_ms,
    )
    try:
        yield from float32_stereo_blocks(pcm, fmt, block_size)
    finally:
        pcm.close()


def list_audio_devices() -> str:
    import sounddevice as sd

    return str(sd.query_devices())
