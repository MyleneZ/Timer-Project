"""End-to-end smoke test: synthesized speech -> VAD -> Vosk -> NLU -> [TX].

Uses macOS `say` to generate audio, so it exercises the real audio pipeline
without anyone having to talk to the laptop.

    .venv/bin/python speak_test.py bridge.mac.toml
"""
import subprocess
import sys
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from tivvy_bridge.app import Bridge
from tivvy_bridge.asr import build_recognizer
from tivvy_bridge.audio import Segmenter
from tivvy_bridge.config import BridgeConfig

PHRASES = [
    "set a timer called baking for twenty minutes",
    "start a fifteen minute homework timer",
    "add five more minutes to the baking timer",
    "take ten minutes off baking",
    "cancel the baking timer",
    "stop",
    "what is the weather like today",          # should NOT produce a command
]

VOICE = sys.argv[2] if len(sys.argv) > 2 else "Samantha"
cfg = BridgeConfig.load(sys.argv[1] if len(sys.argv) > 1 else None)
cfg.transcript_log = None
cfg.link.transport = "stdout"

tmp = Path("/tmp/tivvy_say")
tmp.mkdir(exist_ok=True)

recognizer = build_recognizer(cfg.asr, cfg.audio.samplerate)
bridge = Bridge(cfg, recognizer=recognizer)
bridge._link.start()

frame_bytes = int(cfg.audio.samplerate * cfg.audio.frame_ms / 1000) * 2
silence = b"\x00" * frame_bytes

ok = 0
for i, phrase in enumerate(PHRASES):
    wav = tmp / f"{i}.wav"
    subprocess.run(
        ["say", "-v", VOICE, "--data-format=LEI16@16000", "--file-format=WAVE",
         "-o", str(wav), phrase],
        check=True,
    )
    with wave.open(str(wav)) as handle:
        assert handle.getframerate() == 16000 and handle.getnchannels() == 1
        pcm = handle.readframes(handle.getnframes())

    # Fresh segmenter per phrase, padded with silence so the VAD closes it.
    seg = Segmenter(cfg.vad, cfg.audio.samplerate, cfg.audio.frame_ms)
    stream = silence * 15 + pcm + silence * 60
    utterances = []
    for off in range(0, len(stream) - frame_bytes + 1, frame_bytes):
        got = seg.push(stream[off:off + frame_bytes])
        if got:
            utterances.append(got)

    print(f"\n--- said: {phrase!r}  ({len(utterances)} utterance(s) segmented)")
    for pcm_utt in utterances:
        transcript = recognizer.transcribe(pcm_utt)
        print(f"    heard: {transcript.text!r}  conf={transcript.confidence:.2f}")
        cmd = bridge.handle_text(transcript.text, transcript.confidence)
        if cmd:
            ok += 1

print(f"\n{ok} commands produced from {len(PHRASES)} phrases")
bridge.shutdown()
