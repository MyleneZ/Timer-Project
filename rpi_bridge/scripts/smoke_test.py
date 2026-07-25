#!/usr/bin/env python3
"""End-to-end smoke test using synthesized speech instead of a live microphone.

Drives real audio through the real pipeline - VAD segmentation, Vosk, the NLU,
and the link - so a laptop can prove the chain works before anything is flashed
or deployed to the Pi. Speech comes from macOS `say`; on Linux it falls back to
`espeak-ng`.

    python scripts/smoke_test.py --config bridge.mac.toml
    python scripts/smoke_test.py --config bridge.mac.toml --chatter
    python scripts/smoke_test.py --config bridge.mac.toml --transport ble

`--chatter` speaks ordinary conversation instead of commands and reports how
much of it gets forced into a command by the grammar-constrained decoder. Use
it when tuning [asr] min_confidence or deciding whether to enable the wake word.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from tivvy_bridge.app import Bridge  # noqa: E402
from tivvy_bridge.asr import build_recognizer  # noqa: E402
from tivvy_bridge.audio import Segmenter  # noqa: E402
from tivvy_bridge.config import BridgeConfig  # noqa: E402

COMMANDS = [
    "set a timer called baking for twenty minutes",
    "start a fifteen minute homework timer",
    "add five more minutes to the baking timer",
    "take ten minutes off baking",
    "cancel the baking timer",
    "stop",
]

CHATTER = [
    "what is the weather like today",
    "did you see the game last night",
    "i think we should order pizza for dinner",
    "can you pass me the salt please",
    "the dog needs to go outside",
    "i left my keys on the kitchen counter",
    "she said the meeting got moved to thursday",
    "that movie was way too long honestly",
    "remind me to call the dentist",
    "how much does the bus fare cost now",
]


def synthesize(phrase: str, path: Path, voice: str) -> None:
    """Render `phrase` to a 16 kHz mono WAV at `path`."""
    if shutil.which("say"):
        subprocess.run(
            ["say", "-v", voice, "--data-format=LEI16@16000",
             "--file-format=WAVE", "-o", str(path), phrase],
            check=True,
        )
        return
    if shutil.which("espeak-ng"):
        subprocess.run(
            ["espeak-ng", "-s", "150", "-w", str(path), phrase],
            check=True, stdout=subprocess.DEVNULL,
        )
        _force_16k_mono(path)
        return
    sys.exit("need `say` (macOS) or `espeak-ng` (Linux) to synthesize speech")


def _force_16k_mono(path: Path) -> None:
    """espeak-ng writes 22.05 kHz; decimate to the 16 kHz the pipeline wants."""
    with wave.open(str(path)) as handle:
        rate, channels, width = handle.getframerate(), handle.getnchannels(), handle.getsampwidth()
        pcm = handle.readframes(handle.getnframes())
    if (rate, channels, width) == (16000, 1, 2):
        return
    if width != 2:
        sys.exit(f"unexpected sample width {width} from espeak-ng")

    import array
    samples = array.array("h")
    samples.frombytes(pcm)
    if channels > 1:
        samples = samples[::channels]
    # Linear resample to 16 kHz. Crude, but this is a smoke test, not a benchmark.
    out = array.array("h")
    step = rate / 16000.0
    pos = 0.0
    while int(pos) < len(samples):
        out.append(samples[int(pos)])
        pos += step
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(16000)
        handle.writeframes(out.tobytes())


def segment(pcm: bytes, cfg: BridgeConfig) -> list[bytes]:
    """Run one phrase through the real VAD segmenter, padded with silence."""
    frame_bytes = int(cfg.audio.samplerate * cfg.audio.frame_ms / 1000) * 2
    silence = b"\x00" * frame_bytes
    stream = silence * 15 + pcm + silence * 60

    segmenter = Segmenter(cfg.vad, cfg.audio.samplerate, cfg.audio.frame_ms)
    utterances = []
    for offset in range(0, len(stream) - frame_bytes + 1, frame_bytes):
        utterance = segmenter.push(stream[offset:offset + frame_bytes])
        if utterance is not None:
            utterances.append(utterance)
    return utterances


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-c", "--config")
    parser.add_argument("--transport", choices=("ble", "serial", "stdout"), default="stdout")
    parser.add_argument("--voice", default="Samantha", help="macOS `say` voice")
    parser.add_argument("--chatter", action="store_true",
                        help="speak conversation, not commands, and count false positives")
    args = parser.parse_args()

    cfg = BridgeConfig.load(args.config)
    cfg.link.transport = args.transport
    cfg.transcript_log = None

    phrases = CHATTER if args.chatter else COMMANDS
    recognizer = build_recognizer(cfg.asr, cfg.audio.samplerate)
    bridge = Bridge(cfg, recognizer=recognizer)
    bridge._link.start()  # noqa: SLF001 - no mic here, we feed audio in directly
    if cfg.link.transport != "stdout":
        bridge._link.wait_ready(timeout=cfg.link.scan_timeout_s + 5)  # noqa: SLF001

    fired = 0
    missed = []
    tmp = Path(tempfile.mkdtemp(prefix="tivvy-smoke-"))
    try:
        for index, phrase in enumerate(phrases):
            wav = tmp / f"{index}.wav"
            synthesize(phrase, wav, args.voice)
            with wave.open(str(wav)) as handle:
                pcm = handle.readframes(handle.getnframes())

            utterances = segment(pcm, cfg)
            print(f"\n--- said {phrase!r}  ({len(utterances)} utterance(s))")
            if not utterances:
                missed.append(phrase)
                continue

            for chunk in utterances:
                transcript = recognizer.transcribe(chunk)
                print(f"    heard {transcript.text!r}  conf={transcript.confidence:.2f}")
                if bridge.handle_text(transcript.text, transcript.confidence) is not None:
                    fired += 1
    finally:
        bridge.shutdown()
        shutil.rmtree(tmp, ignore_errors=True)

    print()
    if args.chatter:
        print(f"{fired}/{len(phrases)} conversational phrases were misread as commands "
              f"(min_confidence={cfg.asr.min_confidence}). Lower is better; if this is "
              f"not 0, raise min_confidence or enable [wake].")
        return 0 if fired == 0 else 1

    print(f"{fired}/{len(phrases)} commands recognized and sent.")
    for phrase in missed:
        print(f"  no audio segmented for {phrase!r} - check the VAD settings")
    return 0 if fired == len(phrases) else 1


if __name__ == "__main__":
    raise SystemExit(main())
