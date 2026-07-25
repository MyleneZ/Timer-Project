"""Microphone capture and utterance segmentation.

Capture is 16 kHz mono int16 (what both Vosk and Whisper want). Frames are cut
into utterances by a voice-activity detector so the recognizer only ever sees
speech, which keeps CPU use on a Pi 4 low enough to leave headroom.
"""

from __future__ import annotations

import array
import logging
import math
import queue
import threading
from collections import deque
from typing import Iterator, Optional

from .config import AudioConfig, VadConfig

log = logging.getLogger(__name__)


class AudioError(RuntimeError):
    pass


# --------------------------------------------------------------------------
# Small PCM helpers
#
# `audioop` would cover these, but it was removed from the stdlib in Python
# 3.13, so we do the arithmetic ourselves. At 20 ms frames this is 320 samples
# per call, which is nothing on a Pi 4.
# --------------------------------------------------------------------------

_INT16_MIN = -32768
_INT16_MAX = 32767


def pcm_rms(frame: bytes) -> float:
    samples = array.array("h")
    samples.frombytes(frame)
    if not samples:
        return 0.0
    total = 0
    for sample in samples:
        total += sample * sample
    return math.sqrt(total / len(samples))


def pcm_scale(frame: bytes, gain: float) -> bytes:
    samples = array.array("h")
    samples.frombytes(frame)
    for i, sample in enumerate(samples):
        scaled = int(sample * gain)
        samples[i] = _INT16_MIN if scaled < _INT16_MIN else (
            _INT16_MAX if scaled > _INT16_MAX else scaled
        )
    return samples.tobytes()


def pcm_downmix(frame: bytes, channels: int, mode: str = "first") -> bytes:
    """Reduce an interleaved multi-channel frame to mono."""
    if channels <= 1:
        return frame
    samples = array.array("h")
    samples.frombytes(frame)
    if mode == "average":
        out = array.array("h", [0]) * (len(samples) // channels)
        for i in range(len(out)):
            block = samples[i * channels:(i + 1) * channels]
            out[i] = int(sum(block) / channels)
        return out.tobytes()
    return samples[::channels].tobytes()


# --------------------------------------------------------------------------
# Voice activity detection
# --------------------------------------------------------------------------


class VoiceDetector:
    """Interface: ``is_speech(frame_bytes) -> bool`` for one fixed-size frame."""

    def is_speech(self, frame: bytes) -> bool:  # pragma: no cover - interface
        raise NotImplementedError


class WebRtcDetector(VoiceDetector):
    def __init__(self, aggressiveness: int, samplerate: int, frame_ms: int) -> None:
        try:
            import webrtcvad  # type: ignore
        except ModuleNotFoundError as exc:  # pragma: no cover - env dependent
            raise AudioError(
                "webrtcvad is not installed. `pip install webrtcvad-wheels` or set "
                "[vad] backend = \"energy\"."
            ) from exc
        if frame_ms not in (10, 20, 30):
            raise AudioError(f"webrtcvad needs a 10/20/30 ms frame, got {frame_ms}")
        if samplerate not in (8000, 16000, 32000, 48000):
            raise AudioError(f"webrtcvad does not support {samplerate} Hz")
        self._vad = webrtcvad.Vad(max(0, min(3, aggressiveness)))
        self._samplerate = samplerate

    def is_speech(self, frame: bytes) -> bool:
        return self._vad.is_speech(frame, self._samplerate)


class EnergyDetector(VoiceDetector):
    """Dependency-free fallback: RMS against a slowly-adapting noise floor."""

    def __init__(self, margin_db: float = 9.0) -> None:
        self._margin_db = margin_db
        self._noise_rms = 0.0
        self._primed = False

    def is_speech(self, frame: bytes) -> bool:
        rms = pcm_rms(frame)
        if not self._primed:
            self._noise_rms = max(rms, 1.0)
            self._primed = True
            return False

        floor = max(self._noise_rms, 1.0)
        db_over = 20.0 * math.log10(max(rms, 1.0) / floor)
        speech = db_over >= self._margin_db

        # Adapt the floor on non-speech only, quickly downward and slowly upward.
        if not speech:
            alpha = 0.15 if rms < floor else 0.02
            self._noise_rms = (1 - alpha) * floor + alpha * max(rms, 1.0)
        return speech


def make_detector(cfg: VadConfig, samplerate: int, frame_ms: int) -> VoiceDetector:
    backend = (cfg.backend or "webrtc").lower()
    if backend == "energy":
        return EnergyDetector(cfg.energy_margin_db)
    if backend == "webrtc":
        try:
            return WebRtcDetector(cfg.aggressiveness, samplerate, frame_ms)
        except AudioError as exc:
            log.warning("%s Falling back to the energy detector.", exc)
            return EnergyDetector(cfg.energy_margin_db)
    raise AudioError(f"unknown VAD backend: {cfg.backend!r}")


# --------------------------------------------------------------------------
# Segmentation
# --------------------------------------------------------------------------


class Segmenter:
    """Accumulate frames into utterances using a VAD with hysteresis."""

    def __init__(self, cfg: VadConfig, samplerate: int, frame_ms: int,
                 detector: Optional[VoiceDetector] = None) -> None:
        self._cfg = cfg
        self._frame_ms = frame_ms
        self._detector = detector or make_detector(cfg, samplerate, frame_ms)

        self._preroll = deque(maxlen=max(1, cfg.preroll_ms // frame_ms))
        self._frames: list[bytes] = []
        self._triggered = False
        self._speech_run = 0
        self._silence_ms = 0

        self._end_silence_frames = max(1, cfg.end_silence_ms // frame_ms)
        self._min_frames = max(1, cfg.min_utterance_ms // frame_ms)
        self._max_frames = max(self._min_frames, cfg.max_utterance_ms // frame_ms)

    @property
    def triggered(self) -> bool:
        return self._triggered

    def reset(self) -> None:
        self._frames.clear()
        self._preroll.clear()
        self._triggered = False
        self._speech_run = 0
        self._silence_ms = 0

    def push(self, frame: bytes) -> Optional[bytes]:
        """Feed one frame; returns a complete utterance's PCM when ready."""
        speech = self._detector.is_speech(frame)

        if not self._triggered:
            self._preroll.append(frame)
            self._speech_run = self._speech_run + 1 if speech else 0
            if self._speech_run >= self._cfg.start_frames:
                self._triggered = True
                self._frames = list(self._preroll)
                self._preroll.clear()
                self._silence_ms = 0
            return None

        self._frames.append(frame)
        if speech:
            self._silence_ms = 0
        else:
            self._silence_ms += self._frame_ms

        silence_frames = self._silence_ms // self._frame_ms
        finished = silence_frames >= self._end_silence_frames
        overlong = len(self._frames) >= self._max_frames

        if not (finished or overlong):
            return None

        frames = self._frames
        self.reset()

        if overlong and not finished:
            log.debug("utterance hit the %d ms cap", self._cfg.max_utterance_ms)
        if len(frames) < self._min_frames:
            return None
        return b"".join(frames)


# --------------------------------------------------------------------------
# Microphone
# --------------------------------------------------------------------------


class Microphone:
    """A 16-bit mono PCM frame source backed by PortAudio (``sounddevice``)."""

    def __init__(self, cfg: AudioConfig) -> None:
        self._cfg = cfg
        self._frame_samples = int(cfg.samplerate * cfg.frame_ms / 1000)
        self._queue: "queue.Queue[bytes]" = queue.Queue(maxsize=200)
        self._stream = None
        self._stop = threading.Event()
        self._overflows = 0

    # -- lifecycle ---------------------------------------------------------

    def __enter__(self) -> "Microphone":
        self.start()
        return self

    def __exit__(self, *exc_info) -> None:
        self.stop()

    def start(self) -> None:
        try:
            import sounddevice as sd  # type: ignore
        except (ModuleNotFoundError, OSError) as exc:  # pragma: no cover
            raise AudioError(
                "sounddevice/PortAudio unavailable. Install with "
                "`sudo apt install libportaudio2 && pip install sounddevice`."
            ) from exc

        device = _resolve_device(sd, self._cfg.device)
        log.info(
            "opening mic device=%s rate=%d frame=%dms channels=%d",
            device if device is not None else "default",
            self._cfg.samplerate, self._cfg.frame_ms, self._cfg.channels,
        )

        def callback(indata, frames, time_info, status) -> None:  # pragma: no cover
            if status:
                self._overflows += 1
                if self._overflows % 50 == 1:
                    log.warning("audio input status: %s", status)
            try:
                self._queue.put_nowait(bytes(indata))
            except queue.Full:
                # Drop the oldest frame instead of blocking the audio thread.
                try:
                    self._queue.get_nowait()
                    self._queue.put_nowait(bytes(indata))
                except queue.Empty:
                    pass

        self._stream = sd.RawInputStream(
            samplerate=self._cfg.samplerate,
            blocksize=self._frame_samples,
            device=device,
            dtype="int16",
            channels=self._cfg.channels,
            callback=callback,
        )
        self._stream.start()

    def stop(self) -> None:
        self._stop.set()
        if self._stream is not None:
            try:
                self._stream.stop()
                self._stream.close()
            except Exception:  # pragma: no cover - teardown best effort
                log.debug("error closing audio stream", exc_info=True)
            self._stream = None

    # -- frames ------------------------------------------------------------

    def frames(self) -> Iterator[bytes]:
        """Yield mono int16 frames of ``frame_ms`` duration, forever."""
        while not self._stop.is_set():
            try:
                raw = self._queue.get(timeout=0.5)
            except queue.Empty:
                continue
            yield self._to_mono(raw)

    def _to_mono(self, raw: bytes) -> bytes:
        if self._cfg.channels > 1:
            raw = pcm_downmix(raw, self._cfg.channels)
        if self._cfg.gain != 1.0:
            raw = pcm_scale(raw, self._cfg.gain)
        return raw


def _resolve_device(sd, device: Optional[str]):
    """Accept an index, a name substring, or None for the PortAudio default."""
    if device in (None, "", "default"):
        return None
    if isinstance(device, int):
        return device
    text = str(device)
    if text.isdigit():
        return int(text)
    needle = text.lower()
    for index, info in enumerate(sd.query_devices()):
        if info.get("max_input_channels", 0) > 0 and needle in info["name"].lower():
            return index
    raise AudioError(f"no input device matching {device!r}")


def list_devices() -> str:
    """Human-readable input device listing for ``--list-audio``."""
    try:
        import sounddevice as sd  # type: ignore
    except (ModuleNotFoundError, OSError) as exc:  # pragma: no cover
        return f"sounddevice unavailable: {exc}"
    lines = ["index  channels  name"]
    for index, info in enumerate(sd.query_devices()):
        if info.get("max_input_channels", 0) <= 0:
            continue
        lines.append(f"{index:>5}  {info['max_input_channels']:>8}  {info['name']}")
    return "\n".join(lines)
