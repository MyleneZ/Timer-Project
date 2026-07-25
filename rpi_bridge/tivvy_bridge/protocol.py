"""Wire protocol shared with the Qualia firmware.

Everything in here mirrors constraints that live in ``device_code/production.ino``
and ``device_code/command_protocol.h``. If the firmware changes, change these
constants with it.
"""

from __future__ import annotations

import re
import string
from dataclasses import dataclass
from enum import Enum

# Nordic UART Service, as advertised by NimBLEDevice::init("TimerDevice").
NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX_CHAR_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # we write here
NUS_TX_CHAR_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # firmware notifies here
DEFAULT_DEVICE_NAME = "TimerDevice"

# struct ParsedCommand { ... char name[16]; ... } -> 15 usable characters.
MAX_NAME_LEN = 15

# #define MAX_TIMERS 3
MAX_TIMERS = 3

# The firmware never rings a timer whose seconds_left is already 0, so a
# zero-duration SET wedges a slot forever. Never emit one.
MIN_DURATION_SECONDS = 1
MAX_DURATION_SECONDS = 24 * 3600

# Names the firmware maps to a themed panel (detect_theme_id in production.ino).
# Anything else renders with THEME_DEFAULT, which is legal but plain.
THEMED_NAMES = ("Baking", "Cooking", "Break", "Homework", "Exercise", "Workout")

_ALLOWED_NAME_CHARS = set(string.ascii_letters + string.digits + " -_")


class CommandKind(str, Enum):
    SET = "SET"
    CANCEL = "CANCEL"
    ADD = "ADD"
    MINUS = "MINUS"
    STOP = "STOP"


#: Commands that carry NAME + DURATION fields.
_NEEDS_DURATION = {CommandKind.SET, CommandKind.ADD, CommandKind.MINUS}
#: Commands that carry a NAME field.
_NEEDS_NAME = {CommandKind.SET, CommandKind.CANCEL, CommandKind.ADD, CommandKind.MINUS}


class ProtocolError(ValueError):
    """Raised when a command cannot be represented on the wire."""


@dataclass(frozen=True)
class Command:
    """A single command destined for the Qualia."""

    kind: CommandKind
    name: str = ""
    seconds: int = 0

    def __str__(self) -> str:  # pragma: no cover - debug helper
        if self.kind is CommandKind.STOP:
            return "STOP"
        if self.kind is CommandKind.CANCEL:
            return f"CANCEL {self.name}"
        return f"{self.kind.value} {self.name} {self.seconds}s"


def sanitize_name(raw: str) -> str:
    """Coerce a spoken name into something the firmware can store and match.

    The firmware splits NAME on the first comma and truncates at 15 characters,
    and its ``findTimerByName`` is a case-sensitive ``strcmp``. So we normalise
    aggressively here and always emit the same spelling for the same timer.
    """
    name = re.sub(r"\s+", " ", (raw or "").strip())
    name = "".join(ch for ch in name if ch in _ALLOWED_NAME_CHARS)
    name = name.strip()
    if not name:
        raise ProtocolError("empty timer name")

    # Title-case so "baking" and "Baking" never coexist as two firmware slots.
    name = " ".join(word[:1].upper() + word[1:].lower() for word in name.split(" "))

    # Prefer the canonical spelling of a themed name when we land on one.
    for themed in THEMED_NAMES:
        if name.lower() == themed.lower():
            name = themed
            break

    if len(name) > MAX_NAME_LEN:
        name = name[:MAX_NAME_LEN].rstrip()
    if not name:
        raise ProtocolError("timer name became empty after sanitising")
    return name


def clamp_duration(seconds: int) -> int:
    """Clamp a duration into the range the firmware can actually count down."""
    seconds = int(seconds)
    if seconds < MIN_DURATION_SECONDS:
        raise ProtocolError(f"duration {seconds}s is below the {MIN_DURATION_SECONDS}s minimum")
    return min(seconds, MAX_DURATION_SECONDS)


def encode(command: Command) -> str:
    """Render a command as the exact string the firmware's parseCommand expects."""
    kind = command.kind
    if kind is CommandKind.STOP:
        return "CMD:STOP"

    name = sanitize_name(command.name) if kind in _NEEDS_NAME else ""

    if kind is CommandKind.CANCEL:
        return f"CMD:CANCEL,NAME:{name}"

    if kind in _NEEDS_DURATION:
        seconds = clamp_duration(command.seconds)
        return f"CMD:{kind.value},NAME:{name},DURATION:{seconds}"

    raise ProtocolError(f"unsupported command kind: {kind!r}")


def normalized(command: Command) -> Command:
    """Return the command as it will actually be sent (name/duration applied)."""
    if command.kind is CommandKind.STOP:
        return Command(CommandKind.STOP)
    name = sanitize_name(command.name)
    if command.kind is CommandKind.CANCEL:
        return Command(CommandKind.CANCEL, name=name)
    return Command(command.kind, name=name, seconds=clamp_duration(command.seconds))


def format_hhmmss(seconds: int) -> str:
    """Match the firmware's fmt_hhmmss for log readability."""
    seconds = max(0, int(seconds))
    return f"{seconds // 3600:02d}:{seconds % 3600 // 60:02d}:{seconds % 60:02d}"
