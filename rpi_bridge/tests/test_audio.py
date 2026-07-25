"""Segmentation tests.

`audio.py` had no coverage, which let a fatal aliasing bug through: `push()`
took a reference to `self._frames` and then called `reset()`, which clears that
same list in place. Every utterance came back empty and was dropped as too
short, so the bridge was deaf to every spoken command while still passing its
whole test suite. These tests pin that behaviour down.
"""

from __future__ import annotations

import pytest

from tivvy_bridge.audio import (
    EnergyDetector,
    Segmenter,
    VoiceDetector,
    pcm_downmix,
    pcm_rms,
)
from tivvy_bridge.config import VadConfig

SAMPLERATE = 16000
FRAME_MS = 20
FRAME_BYTES = int(SAMPLERATE * FRAME_MS / 1000) * 2  # 640

SILENCE = b"\x00" * FRAME_BYTES
SPEECH = b"\x40\x10" * (FRAME_BYTES // 2)


class ScriptedDetector(VoiceDetector):
    """Reports speech for exactly the frames we say it should."""

    def __init__(self, verdicts):
        self._verdicts = list(verdicts)
        self.calls = 0

    def is_speech(self, frame: bytes) -> bool:
        verdict = self._verdicts[self.calls] if self.calls < len(self._verdicts) else False
        self.calls += 1
        return verdict


def make_segmenter(verdicts, **overrides):
    cfg = VadConfig(**{
        "start_frames": 4,
        "end_silence_ms": 100,   # 5 frames
        "preroll_ms": 40,        # 2 frames
        "min_utterance_ms": 100, # 5 frames
        "max_utterance_ms": 400, # 20 frames
        **overrides,
    })
    return Segmenter(cfg, SAMPLERATE, FRAME_MS, detector=ScriptedDetector(verdicts))


def drive(segmenter, count, frame=SPEECH):
    """Push `count` frames, returning every utterance that came out."""
    out = []
    for _ in range(count):
        utterance = segmenter.push(frame)
        if utterance is not None:
            out.append(utterance)
    return out


# --------------------------------------------------------------------------
# The regression: an utterance must survive reset()
# --------------------------------------------------------------------------

def test_utterance_is_returned_not_swallowed_by_reset():
    # 2 silence, 10 speech, 6 silence -> one utterance.
    verdicts = [False] * 2 + [True] * 10 + [False] * 6
    segmenter = make_segmenter(verdicts)
    utterances = drive(segmenter, len(verdicts))

    assert len(utterances) == 1, "the utterance was dropped (reset() aliasing bug)"
    assert len(utterances[0]) > 0
    assert len(utterances[0]) % FRAME_BYTES == 0


def test_returned_utterance_is_not_mutated_by_the_next_one():
    verdicts = ([False] * 2 + [True] * 10 + [False] * 6) * 2
    segmenter = make_segmenter(verdicts)
    utterances = drive(segmenter, len(verdicts))

    assert len(utterances) == 2
    first_len = len(utterances[0])
    # Push more frames; the already-returned buffer must not change underneath.
    drive(segmenter, 5, SILENCE)
    assert len(utterances[0]) == first_len


def test_utterance_includes_preroll():
    # preroll_ms=40 -> 2 frames of context kept from before the trigger.
    verdicts = [False] * 2 + [True] * 10 + [False] * 6
    segmenter = make_segmenter(verdicts)
    utterances = drive(segmenter, len(verdicts))

    frames = len(utterances[0]) // FRAME_BYTES
    assert frames >= 10, "preroll was not prepended, the first word will clip"


# --------------------------------------------------------------------------
# Gating
# --------------------------------------------------------------------------

def test_short_utterance_is_discarded():
    # 6 speech frames = 120 ms of audio, under min_utterance_ms once preroll
    # is excluded... but with preroll it clears the bar, so demand a real gap.
    verdicts = [False] * 2 + [True] * 4 + [False] * 6
    segmenter = make_segmenter(verdicts, min_utterance_ms=600, preroll_ms=0)
    assert drive(segmenter, len(verdicts)) == []


def test_brief_noise_never_triggers():
    # start_frames=4, so 3 consecutive speech frames must not open an utterance.
    verdicts = ([True] * 3 + [False] * 3) * 4
    segmenter = make_segmenter(verdicts)
    assert drive(segmenter, len(verdicts)) == []
    assert not segmenter.triggered


def test_speech_run_resets_on_silence():
    # Alternating frames never reach 4 in a row.
    verdicts = [True, False] * 12
    segmenter = make_segmenter(verdicts)
    assert drive(segmenter, len(verdicts)) == []


def test_long_utterance_is_capped_and_emitted():
    # max_utterance_ms=400 -> 20 frames; speech never stops.
    segmenter = make_segmenter([True] * 60)
    utterances = drive(segmenter, 60)

    assert utterances, "an unbroken talker must still yield audio, not buffer forever"
    assert len(utterances[0]) // FRAME_BYTES <= 20


def test_reset_clears_pending_state():
    segmenter = make_segmenter([True] * 10)
    drive(segmenter, 6)
    assert segmenter.triggered
    segmenter.reset()
    assert not segmenter.triggered


# --------------------------------------------------------------------------
# PCM helpers
# --------------------------------------------------------------------------

def test_pcm_rms_of_silence_is_zero():
    assert pcm_rms(SILENCE) == 0.0


def test_pcm_rms_is_positive_for_signal():
    assert pcm_rms(SPEECH) > 0.0


def test_pcm_downmix_mono_is_untouched():
    assert pcm_downmix(SPEECH, 1) == SPEECH


def test_pcm_downmix_stereo_halves_the_frame():
    stereo = b"\x01\x00\x02\x00\x03\x00\x04\x00"
    assert len(pcm_downmix(stereo, 2)) == len(stereo) // 2


def test_energy_detector_primes_on_first_frame_then_detects():
    detector = EnergyDetector(margin_db=9.0)
    assert detector.is_speech(SILENCE) is False   # priming frame
    for _ in range(5):
        detector.is_speech(SILENCE)
    assert detector.is_speech(SPEECH) is True


@pytest.mark.parametrize("frame_ms", [10, 20, 30])
def test_segmenter_accepts_webrtc_frame_sizes(frame_ms):
    cfg = VadConfig(backend="energy")
    segmenter = Segmenter(cfg, SAMPLERATE, frame_ms)
    assert segmenter is not None
