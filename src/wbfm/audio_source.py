from __future__ import annotations

import queue
import shutil
import subprocess
from dataclasses import dataclass
from typing import Iterator

import numpy as np
import soundfile as sf

from .filters import as_float32_stereo, resample_audio


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


def stream_blocks(stream_url: str, sample_rate: int, block_size: int) -> Iterator[np.ndarray]:
    if shutil.which("ffmpeg") is None:
        raise RuntimeError("ffmpeg is nodig voor --source stream, maar staat niet in PATH.")

    cmd = [
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "warning",
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
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    bytes_per_block = block_size * 2 * 4
    try:
        if proc.stdout is None:
            raise RuntimeError("Kon ffmpeg stdout niet openen.")
        while True:
            raw = proc.stdout.read(bytes_per_block)
            if not raw:
                raise RuntimeError("Stream decoder stopte of gaf geen data meer.")
            samples = np.frombuffer(raw, dtype="<f4")
            frames = len(samples) // 2
            if frames == 0:
                continue
            block = samples[: frames * 2].reshape(frames, 2).astype(np.float32, copy=False)
            if len(block) < block_size:
                pad = np.zeros((block_size - len(block), 2), dtype=np.float32)
                block = np.vstack((block, pad))
            yield as_float32_stereo(block)
    finally:
        proc.terminate()


def list_audio_devices() -> str:
    import sounddevice as sd

    return str(sd.query_devices())
