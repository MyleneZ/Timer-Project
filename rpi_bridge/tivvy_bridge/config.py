"""Configuration for the bridge.

Values come from a TOML file (see ``config.example.toml``); anything omitted
falls back to the defaults below. Requires Python 3.11+ for ``tomllib``, which
is what Raspberry Pi OS Bookworm and later ship.
"""

from __future__ import annotations

import dataclasses
import logging
from dataclasses import dataclass, field, fields, is_dataclass
from pathlib import Path
from typing import Any, Mapping, Optional, get_args, get_origin

try:  # Python 3.11+
    import tomllib
except ModuleNotFoundError:  # pragma: no cover - older Pi images
    try:
        import tomli as tomllib  # type: ignore[no-redef]
    except ModuleNotFoundError:  # pragma: no cover
        tomllib = None  # type: ignore[assignment]

from .protocol import DEFAULT_DEVICE_NAME

log = logging.getLogger(__name__)


@dataclass
class AudioConfig:
    #: PortAudio device index or a substring of the device name. None = default.
    device: Optional[str] = None
    samplerate: int = 16000
    #: 10, 20 or 30 ms - webrtcvad only accepts these.
    frame_ms: int = 20
    channels: int = 1
    #: Multiply captured samples before recognition (1.0 = untouched).
    gain: float = 1.0


@dataclass
class VadConfig:
    #: "webrtc" (needs webrtcvad) or "energy" (pure Python fallback).
    backend: str = "webrtc"
    #: webrtcvad aggressiveness, 0 (permissive) .. 3 (aggressive).
    aggressiveness: int = 2
    #: Consecutive speech frames required to open an utterance.
    start_frames: int = 4
    #: Silence that closes an utterance.
    end_silence_ms: int = 700
    #: Audio kept from before the trigger so the first word is not clipped.
    preroll_ms: int = 320
    #: Utterances shorter than this are discarded as noise.
    min_utterance_ms: int = 400
    #: Hard cap so a noisy room cannot buffer forever.
    max_utterance_ms: int = 8000
    #: Energy-VAD only: dB above the rolling noise floor that counts as speech.
    energy_margin_db: float = 9.0


@dataclass
class AsrConfig:
    #: "vosk" or "whisper".
    backend: str = "vosk"
    #: Vosk model directory (see scripts/download_vosk_model.sh).
    model_path: str = "/opt/tivvy/models/vosk-model-small-en-us-0.15"
    #: Restrict the decoder to the Tivvy vocabulary. Large accuracy win; turn
    #: off only if you want to experiment with free-form dictation.
    use_grammar: bool = True
    #: Reject an utterance whose mean word confidence is below this (0..1).
    min_confidence: float = 0.55
    #: faster-whisper settings, used when backend = "whisper".
    whisper_model: str = "tiny.en"
    whisper_compute_type: str = "int8"
    whisper_beam_size: int = 1


@dataclass
class WakeConfig:
    #: When enabled, an utterance must contain a wake phrase to be acted on.
    enabled: bool = False
    phrases: list[str] = field(default_factory=lambda: ["tivvy", "hey tivvy"])
    #: Seconds a bare wake word keeps the mic "armed" for a follow-up command.
    follow_up_window_s: float = 6.0


@dataclass
class NluConfig:
    set_without_duration_seconds: int = 0
    bare_number_unit_seconds: int = 60
    adjust_without_duration_seconds: int = 60
    name_fuzzy_cutoff: float = 0.82
    max_words: int = 24


@dataclass
class LinkConfig:
    #: "ble", "serial", or "stdout" (dry run).
    transport: str = "ble"

    # -- BLE ---------------------------------------------------------------
    device_name: str = DEFAULT_DEVICE_NAME
    #: Pin the Qualia's BLE address once you know it. Far more reliable than
    #: scanning by name; see the audit note about advertised names.
    device_address: Optional[str] = None
    adapter: Optional[str] = None  # e.g. "hci0"
    scan_timeout_s: float = 12.0
    #: Write with response. Keep True: BlueZ then handles long writes for us,
    #: so commands survive a small negotiated ATT MTU.
    write_with_response: bool = True
    #: Listen on the TX characteristic for firmware acknowledgements.
    subscribe_notifications: bool = True

    # -- Serial ------------------------------------------------------------
    serial_port: str = "/dev/ttyACM0"
    serial_baud: int = 115200

    # -- Shared ------------------------------------------------------------
    reconnect_min_s: float = 1.0
    reconnect_max_s: float = 20.0
    send_timeout_s: float = 5.0
    #: Drop a command rather than queue it when the link has been down this long.
    stale_command_s: float = 8.0


@dataclass
class BridgeConfig:
    audio: AudioConfig = field(default_factory=AudioConfig)
    vad: VadConfig = field(default_factory=VadConfig)
    asr: AsrConfig = field(default_factory=AsrConfig)
    wake: WakeConfig = field(default_factory=WakeConfig)
    nlu: NluConfig = field(default_factory=NluConfig)
    link: LinkConfig = field(default_factory=LinkConfig)

    log_level: str = "INFO"
    #: Ignore an identical command repeated within this many seconds. Guards
    #: against the Qualia's own alarm audio being re-recognised as a command.
    duplicate_window_s: float = 2.5
    #: Write every accepted utterance to this file for later tuning.
    transcript_log: Optional[str] = None

    @classmethod
    def load(cls, path: Optional[str | Path]) -> "BridgeConfig":
        if not path:
            return cls()
        file_path = Path(path).expanduser()
        if not file_path.exists():
            raise FileNotFoundError(f"config file not found: {file_path}")
        if tomllib is None:  # pragma: no cover
            raise RuntimeError(
                "TOML support is unavailable. Use Python 3.11+ or `pip install tomli`."
            )
        with file_path.open("rb") as handle:
            data = tomllib.load(handle)
        return _from_mapping(cls, data, path=file_path.name)


def _from_mapping(cls: type, data: Mapping[str, Any], path: str = "config") -> Any:
    """Build a (possibly nested) dataclass from a mapping, warning on typos."""
    if not is_dataclass(cls):
        return data

    known = {f.name for f in fields(cls)}
    kwargs: dict[str, Any] = {}

    for key, value in data.items():
        if key not in known:
            log.warning("%s: ignoring unknown setting %r", path, key)
            continue
        # `from __future__ import annotations` makes field types strings, so the
        # nested dataclass is recovered from the field's default_factory.
        nested = _nested_dataclass(cls, key) if isinstance(value, Mapping) else None
        if nested is not None:
            kwargs[key] = _from_mapping(nested, value, path=f"{path}.{key}")
        else:
            kwargs[key] = value

    return cls(**kwargs)


def _nested_dataclass(cls: type, name: str) -> Optional[type]:
    """Resolve the dataclass type of a field, tolerating string annotations."""
    for f in fields(cls):
        if f.name != name:
            continue
        if is_dataclass(f.type):
            return f.type  # type: ignore[return-value]
        # Fall back to the default_factory's product for string annotations.
        if f.default_factory is not dataclasses.MISSING:  # type: ignore[misc]
            produced = f.default_factory()  # type: ignore[misc]
            if is_dataclass(produced):
                return type(produced)
        origin = get_origin(f.type)
        if origin is not None:
            for arg in get_args(f.type):
                if is_dataclass(arg):
                    return arg  # type: ignore[return-value]
    return None
