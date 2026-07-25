"""The bridge itself: mic -> ASR -> NLU -> link."""

from __future__ import annotations

import logging
import signal
import threading
import time
from pathlib import Path
from typing import Optional

from .asr import Recognizer, Transcript, build_recognizer
from .audio import Microphone, Segmenter
from .config import BridgeConfig
from .nlu import NluOptions, ParseResult, parse
from .protocol import Command, encode
from .state import TimerShadow
from .transport import Link, build_link

log = logging.getLogger(__name__)


class Bridge:
    def __init__(self, cfg: BridgeConfig, link: Optional[Link] = None,
                 recognizer: Optional[Recognizer] = None) -> None:
        self.cfg = cfg
        self.shadow = TimerShadow()
        self.nlu_options = NluOptions(
            set_without_duration_seconds=cfg.nlu.set_without_duration_seconds,
            bare_number_unit_seconds=cfg.nlu.bare_number_unit_seconds,
            adjust_without_duration_seconds=cfg.nlu.adjust_without_duration_seconds,
            name_fuzzy_cutoff=cfg.nlu.name_fuzzy_cutoff,
            max_words=cfg.nlu.max_words,
        )

        self._link = link if link is not None else build_link(cfg.link, self._on_device_message)
        self._recognizer = recognizer
        self._stop = threading.Event()
        self._last_command: Optional[tuple[str, float]] = None
        self._armed_until = 0.0
        self._transcript_file = None

        self.stats = {"utterances": 0, "recognized": 0, "commands": 0, "rejected": 0}

    # ----------------------------------------------------------------- setup

    def _open_transcript_log(self) -> None:
        if not self.cfg.transcript_log:
            return
        path = Path(self.cfg.transcript_log).expanduser()
        path.parent.mkdir(parents=True, exist_ok=True)
        self._transcript_file = path.open("a", encoding="utf-8")
        log.info("logging transcripts to %s", path)

    def _log_transcript(self, transcript: Transcript, result: ParseResult) -> None:
        if self._transcript_file is None:
            return
        stamp = time.strftime("%Y-%m-%dT%H:%M:%S")
        outcome = str(result.command) if result.command else f"REJECT ({result.reason})"
        self._transcript_file.write(
            f"{stamp}\tconf={transcript.confidence:.2f}\t{transcript.text!r}\t{outcome}\n"
        )
        self._transcript_file.flush()

    # ------------------------------------------------------------- device I/O

    def _on_device_message(self, text: str) -> None:
        """Handle anything the firmware notifies back on the TX characteristic."""
        if self.shadow.apply_device_state(text):
            log.info("device state: %s", self.shadow.describe())
            return

        # ACK:SET,NAME:Baking,OK:0 - the device heard us and refused.
        marker = text.find("ACK:")
        if marker >= 0 and "OK:0" in text[marker:]:
            log.warning("device refused: %s", text[marker:].strip())

    # -------------------------------------------------------------- utterance

    def handle_text(self, text: str, confidence: float = 1.0) -> Optional[Command]:
        """Parse one utterance and, if it is a command, send it. Returns it."""
        transcript = Transcript(text=text, confidence=confidence)
        self.stats["utterances"] += 1

        if not transcript.text.strip():
            return None
        self.stats["recognized"] += 1

        if confidence < self.cfg.asr.min_confidence:
            log.info("low confidence %.2f, ignoring: %r", confidence, text)
            self.stats["rejected"] += 1
            return None

        if not self._passes_wake_gate(text):
            return None

        result = parse(
            text,
            options=self.nlu_options,
            resolve_name=self.shadow.resolve_name,
            wake_words=self.cfg.wake.phrases if self.cfg.wake.enabled else (),
        )
        self._log_transcript(transcript, result)

        if not result.command:
            log.info("heard %r -> no command (%s)", text, result.reason)
            self.stats["rejected"] += 1
            return None

        command = result.command
        if self._is_duplicate(command):
            log.info("ignoring repeat of %s within %.1fs", command, self.cfg.duplicate_window_s)
            return None

        warning = self.shadow.plausibility(command)
        if warning:
            log.warning("%s: %s", command, warning)

        payload = encode(command)
        log.info("heard %r -> %s", text, payload)

        if self._link.send(payload):
            self.shadow.apply(command)
            self.stats["commands"] += 1
            self._last_command = (payload, time.monotonic())
            log.info("timers now: %s", self.shadow.describe())
            return command

        log.error("failed to deliver %s", payload)
        return None

    def _is_duplicate(self, command: Command) -> bool:
        if self._last_command is None:
            return False
        payload, when = self._last_command
        if payload != encode(command):
            return False
        return (time.monotonic() - when) < self.cfg.duplicate_window_s

    def _passes_wake_gate(self, text: str) -> bool:
        if not self.cfg.wake.enabled:
            return True

        lowered = f" {text.lower()} "
        heard_wake = any(f" {phrase.lower()} " in lowered for phrase in self.cfg.wake.phrases)
        now = time.monotonic()

        if heard_wake:
            self._armed_until = now + self.cfg.wake.follow_up_window_s
            return True
        if now < self._armed_until:
            # Follow-up utterance inside the window; consume the arming.
            self._armed_until = 0.0
            return True

        log.debug("no wake phrase in %r; ignoring", text)
        return False

    # ------------------------------------------------------------------- run

    def run(self) -> int:
        self._open_transcript_log()
        self._install_signal_handlers()

        log.info("starting link: %s", self._link.name)
        self._link.start()
        if not self._link.wait_ready(timeout=self.cfg.link.scan_timeout_s + 5):
            log.warning("link is not up yet; continuing and retrying in the background")

        if self._recognizer is None:
            extra_words: list[str] = []
            if self.cfg.wake.enabled:
                for phrase in self.cfg.wake.phrases:
                    extra_words.extend(phrase.split())
            self._recognizer = build_recognizer(
                self.cfg.asr, self.cfg.audio.samplerate, extra_words
            )
        log.info("ASR backend: %s", self._recognizer.name)

        segmenter = Segmenter(self.cfg.vad, self.cfg.audio.samplerate, self.cfg.audio.frame_ms)

        try:
            with Microphone(self.cfg.audio) as mic:
                log.info("listening (say e.g. \"set a baking timer for twenty minutes\")")
                for frame in mic.frames():
                    if self._stop.is_set():
                        break
                    utterance = segmenter.push(frame)
                    if utterance is None:
                        continue
                    self._process_utterance(utterance)
        except KeyboardInterrupt:  # pragma: no cover
            pass
        finally:
            self.shutdown()

        log.info(
            "stats: %d utterances, %d recognized, %d commands sent, %d rejected",
            self.stats["utterances"], self.stats["recognized"],
            self.stats["commands"], self.stats["rejected"],
        )
        return 0

    def _process_utterance(self, pcm: bytes) -> None:
        assert self._recognizer is not None
        started = time.monotonic()
        try:
            transcript = self._recognizer.transcribe(pcm)
        except Exception:
            log.exception("recognition failed")
            return
        elapsed = time.monotonic() - started

        seconds = len(pcm) / 2 / self.cfg.audio.samplerate
        log.debug("%.2fs audio decoded in %.2fs -> %r (conf %.2f)",
                  seconds, elapsed, transcript.text, transcript.confidence)

        self.handle_text(transcript.text, transcript.confidence)

    def _install_signal_handlers(self) -> None:
        def handler(signum, _frame):  # pragma: no cover - signal path
            log.info("received signal %s, shutting down", signum)
            self._stop.set()

        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                signal.signal(sig, handler)
            except ValueError:  # pragma: no cover - not on the main thread
                pass

    def shutdown(self) -> None:
        self._stop.set()
        try:
            self._link.close()
        except Exception:  # pragma: no cover
            log.debug("link close failed", exc_info=True)
        if self._recognizer is not None:
            self._recognizer.close()
        if self._transcript_file is not None:
            self._transcript_file.close()
            self._transcript_file = None
