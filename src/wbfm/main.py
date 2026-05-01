from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path
from typing import BinaryIO

import numpy as np
import yaml

from .audio_source import device_blocks, list_audio_devices, read_wav, stream_blocks, wav_blocks
from .filters import StreamingIntegerUpsampler, resample_audio
from .fm_modulator import FmModulator
from .mpx_encoder import StereoMpxEncoder
from .pluto_tx import PlutoTx


DEFAULT_CONFIG = Path(__file__).resolve().parents[2] / "config" / "config.yaml"


def load_config(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as handle:
        return yaml.safe_load(handle)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Stereo WBFM transmitter voor ADALM-Pluto SDR.")
    parser.add_argument("--config", default=str(DEFAULT_CONFIG), help="Pad naar config.yaml.")
    parser.add_argument("--freq", type=float, help="RF-frequentie in MHz, bijvoorbeeld --freq 2323.7.")
    parser.add_argument("--uri", help="Pluto IIO URI, bijvoorbeeld ip:192.168.2.1.")
    parser.add_argument("--gain", type=float, help="Pluto TX gain in dB. Begin laag, bijvoorbeeld -30.")
    parser.add_argument("--source", choices=("wav", "device", "stream", "silence"), help="Audiobron.")
    parser.add_argument("--input", help="WAV-bestand voor --source wav.")
    parser.add_argument("--device", help="Audio input device naam of index voor --source device.")
    parser.add_argument("--stream-url", help="Stream-URL voor --source stream.")
    parser.add_argument("--audio-gain", type=float, help="Audio gain vóór MPX-encoding, bijvoorbeeld 0.5.")
    parser.add_argument("--mpx-gain", type=float, help="Composite MPX drive vóór FM-modulatie, bijvoorbeeld 1.15.")
    parser.add_argument("--cyclic", action="store_true", help="Gebruik Pluto cyclic buffer. Alleen zinvol voor WAV.")
    parser.add_argument("--deviation", type=float, help="FM-deviatie in Hz.")
    parser.add_argument("--preemphasis-us", type=float, help="Pre-emphasis in microseconden.")
    parser.add_argument("--pilot-level", type=float, help="19 kHz pilotniveau in de MPX, bijvoorbeeld 0.10.")
    parser.add_argument("--seconds", type=float, help="Stop automatisch na dit aantal seconden.")
    parser.add_argument("--list-devices", action="store_true", help="Toon audio input/output devices en stop.")
    parser.add_argument(
        "--iq-out",
        help="Schrijf complex64 baseband IQ naar dit bestand of '-' in plaats van Pluto TX.",
    )
    return parser


def make_chain(config: dict) -> tuple[StereoMpxEncoder, FmModulator]:
    audio_cfg = config["audio"]
    pluto_cfg = config["pluto"]
    mpx_cfg = config["mpx"]
    fm_cfg = config["fm"]
    encoder = StereoMpxEncoder(
        sample_rate=int(audio_cfg["mpx_rate_hz"]),
        audio_lowpass_hz=float(mpx_cfg["audio_lowpass_hz"]),
        preemphasis_us=float(mpx_cfg["preemphasis_us"]),
        pilot_hz=float(mpx_cfg["pilot_hz"]),
        pilot_level=float(mpx_cfg["pilot_level"]),
        subcarrier_hz=float(mpx_cfg["subcarrier_hz"]),
        sum_level=float(mpx_cfg["sum_level"]),
        diff_level=float(mpx_cfg["diff_level"]),
    )
    modulator = FmModulator(
        sample_rate=int(pluto_cfg["sample_rate_hz"]),
        deviation_hz=float(fm_cfg["deviation_hz"]),
        iq_level=float(fm_cfg["iq_level"]),
    )
    return encoder, modulator


def audio_to_iq(audio: np.ndarray, config: dict, encoder: StereoMpxEncoder, modulator: FmModulator) -> np.ndarray:
    audio_rate = int(config["audio"]["audio_rate_hz"])
    mpx_rate = int(config["audio"]["mpx_rate_hz"])
    pluto_rate = int(config["pluto"]["sample_rate_hz"])
    mpx_gain = float(config["mpx"].get("gain", 1.0))
    at_mpx_rate = resample_audio(audio, audio_rate, mpx_rate)
    mpx = encoder.encode(at_mpx_rate)
    if mpx_gain != 1.0:
        mpx = np.clip(mpx * mpx_gain, -1.0, 1.0).astype(np.float32, copy=False)
    mpx_at_pluto_rate = resample_audio(mpx, mpx_rate, pluto_rate)
    return modulator.modulate(mpx_at_pluto_rate)


class StreamingAudioToIq:
    def __init__(self, config: dict, encoder: StereoMpxEncoder, modulator: FmModulator) -> None:
        audio_rate = int(config["audio"]["audio_rate_hz"])
        mpx_rate = int(config["audio"]["mpx_rate_hz"])
        pluto_rate = int(config["pluto"]["sample_rate_hz"])
        if mpx_rate % audio_rate != 0:
            raise ValueError("Streaming pad verwacht dat mpx_rate_hz een veelvoud is van audio_rate_hz.")
        if pluto_rate % mpx_rate != 0:
            raise ValueError("Streaming pad verwacht dat sample_rate_hz een veelvoud is van mpx_rate_hz.")
        self.encoder = encoder
        self.modulator = modulator
        self.mpx_gain = float(config["mpx"].get("gain", 1.0))
        self.audio_to_mpx = StreamingIntegerUpsampler(mpx_rate // audio_rate, channels=2)
        self.mpx_to_pluto = StreamingIntegerUpsampler(pluto_rate // mpx_rate)

    def process(self, audio: np.ndarray) -> np.ndarray:
        at_mpx_rate = self.audio_to_mpx.process(audio)
        mpx = self.encoder.encode(at_mpx_rate)
        if self.mpx_gain != 1.0:
            mpx = np.clip(mpx * self.mpx_gain, -1.0, 1.0).astype(np.float32, copy=False)
        mpx_at_pluto_rate = self.mpx_to_pluto.process(mpx)
        return self.modulator.modulate(mpx_at_pluto_rate)


def apply_cli_overrides(config: dict, args: argparse.Namespace) -> dict:
    if args.freq is not None:
        config["pluto"]["tx_lo_hz"] = int(round(args.freq * 1_000_000))
    if args.uri:
        config["pluto"]["uri"] = args.uri
    if args.gain is not None:
        config["pluto"]["tx_gain_db"] = args.gain
    if args.source:
        config["audio"]["source"] = args.source
    if args.input:
        config["audio"]["input_file"] = args.input
    if args.device is not None:
        config["audio"]["device"] = args.device
    if args.stream_url:
        config["audio"]["stream_url"] = args.stream_url
    if args.audio_gain is not None:
        config["audio"]["gain"] = args.audio_gain
    if args.mpx_gain is not None:
        config["mpx"]["gain"] = args.mpx_gain
    if args.cyclic:
        config["pluto"]["cyclic_buffer"] = True
    if args.deviation is not None:
        config["fm"]["deviation_hz"] = args.deviation
    if args.preemphasis_us is not None:
        config["mpx"]["preemphasis_us"] = args.preemphasis_us
    if args.pilot_level is not None:
        config["mpx"]["pilot_level"] = args.pilot_level
    return config


def make_source(config: dict):
    audio_cfg = config["audio"]
    source = audio_cfg["source"]
    audio_rate = int(audio_cfg["audio_rate_hz"])
    block_size = int(audio_cfg["block_size"])
    if source == "wav":
        return wav_blocks(str(audio_cfg["input_file"]), audio_rate, block_size, loop=True)
    if source == "device":
        return device_blocks(audio_cfg.get("device"), audio_rate, block_size)
    if source == "stream":
        stream_url = audio_cfg.get("stream_url")
        if not stream_url:
            raise ValueError("--stream-url is verplicht bij --source stream.")
        return stream_blocks(str(stream_url), audio_rate, block_size)
    if source == "silence":
        return silence_blocks(block_size)
    raise ValueError(f"Onbekende audiobron: {source}")


def silence_blocks(block_size: int):
    block = np.zeros((block_size, 2), dtype=np.float32)
    while True:
        yield block


def apply_audio_gain(audio: np.ndarray, config: dict) -> np.ndarray:
    gain = float(config["audio"].get("gain", 1.0))
    if gain == 1.0:
        return audio
    return np.clip(audio * gain, -1.0, 1.0).astype(np.float32, copy=False)


def suppress_tx_warnings(config: dict) -> bool:
    operator_cfg = config.get("operator", {})
    return bool(operator_cfg.get("suppress_tx_warnings", False))


def run_cyclic_wav(
    config: dict,
    tx: PlutoTx,
    encoder: StereoMpxEncoder,
    modulator: FmModulator,
    seconds: float | None,
) -> None:
    path = str(config["audio"]["input_file"])
    audio = read_wav(path, int(config["audio"]["audio_rate_hz"]))
    audio = apply_audio_gain(audio, config)
    iq = audio_to_iq(audio, config, encoder, modulator)
    tx.send(iq)
    print(f"Cyclic WAV-buffer actief: {Path(path)} ({len(iq)} IQ samples). Ctrl-C stopt.", flush=True)
    deadline = time.monotonic() + seconds if seconds is not None else None
    while True:
        if deadline is not None and time.monotonic() >= deadline:
            print("Automatische testduur bereikt.", flush=True)
            return
        time.sleep(1.0)


def open_iq_output(path: str) -> tuple[BinaryIO, BinaryIO | None]:
    if path == "-":
        return sys.stdout.buffer, None
    handle = open(path, "wb")
    return handle, handle


def run_iq_output(
    config: dict,
    encoder: StereoMpxEncoder,
    modulator: FmModulator,
    iq_out_path: str,
    seconds: float | None,
) -> None:
    out, closeable = open_iq_output(iq_out_path)
    try:
        if config["audio"]["source"] == "wav" and seconds is None:
            audio = read_wav(str(config["audio"]["input_file"]), int(config["audio"]["audio_rate_hz"]))
            audio = apply_audio_gain(audio, config)
            out.write(audio_to_iq(audio, config, encoder, modulator).astype(np.complex64).tobytes())
            return

        streaming_chain = StreamingAudioToIq(config, encoder, modulator)
        deadline = time.monotonic() + seconds if seconds is not None else None
        for block in make_source(config):
            if deadline is not None and time.monotonic() >= deadline:
                return
            iq = streaming_chain.process(apply_audio_gain(block, config))
            out.write(iq.astype(np.complex64, copy=False).tobytes())
    finally:
        if closeable is not None:
            closeable.close()


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    if args.list_devices:
        print(list_audio_devices())
        return 0

    config = apply_cli_overrides(load_config(args.config), args)
    pluto_cfg = config["pluto"]
    tx_hz = int(pluto_cfg["tx_lo_hz"])
    gain = float(pluto_cfg["tx_gain_db"])
    source = config["audio"]["source"]

    encoder, modulator = make_chain(config)
    if args.iq_out:
        print(
            f"Start WBFM IQ-output: source={source}, "
            f"sample_rate={pluto_cfg['sample_rate_hz']} Hz, out={args.iq_out}",
            flush=True,
        )
        run_iq_output(config, encoder, modulator, args.iq_out, args.seconds)
        return 0

    print(
        f"Start Pluto WBFM TX: {tx_hz / 1_000_000:.6f} MHz, "
        f"source={source}, sample_rate={pluto_cfg['sample_rate_hz']} Hz, gain={gain:.1f} dB",
        flush=True,
    )
    if gain > -10 and not suppress_tx_warnings(config):
        print("Waarschuwing: TX gain is relatief hoog.", flush=True)

    tx = PlutoTx(
        uri=str(pluto_cfg["uri"]),
        tx_lo_hz=tx_hz,
        sample_rate_hz=int(pluto_cfg["sample_rate_hz"]),
        rf_bandwidth_hz=int(pluto_cfg["rf_bandwidth_hz"]),
        tx_gain_db=gain,
        buffer_size=int(pluto_cfg["buffer_size"]),
        cyclic_buffer=bool(pluto_cfg["cyclic_buffer"]),
    )

    try:
        if source == "wav" and bool(pluto_cfg["cyclic_buffer"]):
            run_cyclic_wav(config, tx, encoder, modulator, args.seconds)
            return 0
        streaming_chain = StreamingAudioToIq(config, encoder, modulator)
        deadline = time.monotonic() + args.seconds if args.seconds is not None else None
        for block in make_source(config):
            if deadline is not None and time.monotonic() >= deadline:
                print("Automatische testduur bereikt.", flush=True)
                break
            tx.send(streaming_chain.process(apply_audio_gain(block, config)))
    except KeyboardInterrupt:
        print("\nGestopt.", flush=True)
    finally:
        tx.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
