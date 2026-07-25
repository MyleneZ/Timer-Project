"""Offline speech recognition backends.

Both backends run entirely on the Pi with no network access, which keeps the
privacy property the project is built around.

* ``vosk``   - default. Small Kaldi model, ~50 MB, real-time on one Pi 4 core.
               With ``use_grammar`` the decoder is restricted to the Tivvy
               vocabulary, which is the single biggest accuracy lever available.
* ``whisper`` - faster-whisper ``tiny.en``. Better on unusual phrasings and
               accents, roughly 1-3 s per utterance on a Pi 4. Use it if Vosk
               is not accurate enough for a speaker.
"""

from __future__ import annotations

import json
import logging
import math
import os
from dataclasses import dataclass
from typing import Sequence

from .config import AsrConfig
from .nlu import recognizer_vocabulary

log = logging.getLogger(__name__)


class AsrError(RuntimeError):
    pass


@dataclass
class Transcript:
    text: str
    confidence: float = 1.0
    backend: str = ""

    def __bool__(self) -> bool:
        return bool(self.text.strip())


class Recognizer:
    """Interface: PCM in, text out."""

    name = "base"

    def transcribe(self, pcm: bytes) -> Transcript:  # pragma: no cover - interface
        raise NotImplementedError

    def close(self) -> None:  # pragma: no cover - optional
        pass


# --------------------------------------------------------------------------
# Vosk
# --------------------------------------------------------------------------


class VoskRecognizer(Recognizer):
    name = "vosk"

    def __init__(self, cfg: AsrConfig, samplerate: int,
                 extra_words: Sequence[str] = ()) -> None:
        try:
            from vosk import KaldiRecognizer, Model, SetLogLevel  # type: ignore
        except ModuleNotFoundError as exc:  # pragma: no cover - env dependent
            raise AsrError("vosk is not installed (`pip install vosk`)") from exc

        model_path = os.path.expanduser(cfg.model_path)
        if not os.path.isdir(model_path):
            raise AsrError(
                f"Vosk model directory not found: {model_path}\n"
                "Run scripts/download_vosk_model.sh or fix [asr] model_path."
            )

        SetLogLevel(-1)  # Vosk is extremely chatty at default verbosity.
        log.info("loading Vosk model from %s", model_path)
        self._model = Model(model_path)
        self._samplerate = samplerate
        self._cfg = cfg

        if cfg.use_grammar:
            vocabulary = sorted(set(recognizer_vocabulary()) | {w.lower() for w in extra_words if w})
            grammar = json.dumps([" ".join(vocabulary), "[unk]"])
            self._recognizer = KaldiRecognizer(self._model, samplerate, grammar)
            log.info("Vosk grammar restricted to %d words", len(vocabulary))
        else:
            self._recognizer = KaldiRecognizer(self._model, samplerate)
        self._recognizer.SetWords(True)

    def transcribe(self, pcm: bytes) -> Transcript:
        self._recognizer.Reset()
        self._recognizer.AcceptWaveform(pcm)
        raw = self._recognizer.FinalResult()
        try:
            result = json.loads(raw)
        except json.JSONDecodeError:  # pragma: no cover - vosk always returns JSON
            log.warning("could not decode Vosk result: %r", raw)
            return Transcript("", 0.0, self.name)

        text = (result.get("text") or "").strip()
        words: Sequence[dict] = result.get("result") or []
        if words:
            confidence = sum(float(w.get("conf", 0.0)) for w in words) / len(words)
        else:
            confidence = 0.0 if text else 1.0
        return Transcript(text=text, confidence=confidence, backend=self.name)


# --------------------------------------------------------------------------
# faster-whisper
# --------------------------------------------------------------------------


class WhisperRecognizer(Recognizer):
    name = "whisper"

    def __init__(self, cfg: AsrConfig, samplerate: int) -> None:
        try:
            from faster_whisper import WhisperModel  # type: ignore
        except ModuleNotFoundError as exc:  # pragma: no cover - env dependent
            raise AsrError(
                "faster-whisper is not installed (`pip install faster-whisper`)"
            ) from exc
        try:
            import numpy  # type: ignore  # noqa: F401
        except ModuleNotFoundError as exc:  # pragma: no cover
            raise AsrError("numpy is required by the whisper backend") from exc

        if samplerate != 16000:
            raise AsrError("the whisper backend requires a 16 kHz capture rate")

        log.info("loading faster-whisper model %s", cfg.whisper_model)
        self._model = WhisperModel(
            cfg.whisper_model,
            device="cpu",
            compute_type=cfg.whisper_compute_type,
        )
        self._cfg = cfg
        # Bias the decoder toward our vocabulary without hard-constraining it.
        self._prompt = (
            "Timer commands: set, cancel, add, minus, stop. "
            "Timer names: Baking, Cooking, Break, Homework, Exercise, Workout."
        )

    def transcribe(self, pcm: bytes) -> Transcript:
        import numpy as np  # type: ignore

        audio = np.frombuffer(pcm, dtype=np.int16).astype(np.float32) / 32768.0
        segments, _info = self._model.transcribe(
            audio,
            language="en",
            beam_size=self._cfg.whisper_beam_size,
            vad_filter=False,
            condition_on_previous_text=False,
            initial_prompt=self._prompt,
        )

        parts: list[str] = []
        logprobs: list[float] = []
        for segment in segments:
            parts.append(segment.text.strip())
            logprobs.append(float(getattr(segment, "avg_logprob", -1.0)))

        text = " ".join(p for p in parts if p).strip()
        if logprobs:
            # avg_logprob is roughly -0.1 (great) .. -1.5 (bad); map to 0..1.
            confidence = float(min(1.0, max(0.0, math.exp(sum(logprobs) / len(logprobs)))))
        else:
            confidence = 0.0
        return Transcript(text=text, confidence=confidence, backend=self.name)


# --------------------------------------------------------------------------
# Test double
# --------------------------------------------------------------------------


class ScriptedRecognizer(Recognizer):
    """Returns canned transcripts; used by ``--say`` and the tests."""

    name = "scripted"

    def __init__(self, lines: Sequence[str]) -> None:
        self._lines = list(lines)

    def transcribe(self, pcm: bytes) -> Transcript:
        if not self._lines:
            return Transcript("", 0.0, self.name)
        return Transcript(self._lines.pop(0), 1.0, self.name)


def build_recognizer(cfg: AsrConfig, samplerate: int,
                     extra_words: Sequence[str] = ()) -> Recognizer:
    """Construct the configured backend.

    ``extra_words`` adds vocabulary beyond the built-in command grammar (the
    bridge uses it for wake phrases). Words the acoustic model has never seen
    are silently dropped by Vosk, so keep wake phrases to real English words.
    """
    backend = (cfg.backend or "vosk").lower()
    if backend == "vosk":
        return VoskRecognizer(cfg, samplerate, extra_words)
    if backend in ("whisper", "faster-whisper"):
        return WhisperRecognizer(cfg, samplerate)
    raise AsrError(f"unknown ASR backend: {cfg.backend!r}")
