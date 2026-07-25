"""A best-effort mirror of the timers the Qualia is running.

The firmware currently sends nothing back over the NUS TX characteristic, so the
Pi cannot *know* the device state. This module keeps an optimistic shadow copy
built from the commands we sent, which is enough to:

  * resolve "cancel the timer" when exactly one timer is running,
  * resolve "cancel timer two" (ordinal references),
  * name a new timer when the user did not say one ("Timer 1", "Timer 2", ...),
  * warn when a command is very likely to be a no-op on the device.

The shadow is advisory. When it disagrees with a plausible command we still send
the command and log the disagreement rather than silently dropping it. If the
firmware is later extended to emit ``STATE:`` notifications (see the audit
notes), :meth:`TimerShadow.apply_device_state` folds real state back in.
"""

from __future__ import annotations

import logging
import threading
import time
from dataclasses import dataclass, field
from typing import Optional

from .protocol import MAX_TIMERS, Command, CommandKind, format_hhmmss

log = logging.getLogger(__name__)

#: production.ino clears a ringing timer ALARM_DURATION_MS after it fires.
ALARM_DURATION_SECONDS = 8.0


@dataclass
class ShadowTimer:
    name: str
    total_seconds: int
    ends_at: float
    created_at: float = field(default_factory=time.monotonic)

    def remaining(self, now: Optional[float] = None) -> float:
        now = time.monotonic() if now is None else now
        return max(0.0, self.ends_at - now)

    def is_ringing(self, now: Optional[float] = None) -> bool:
        return self.remaining(now) <= 0.0


class TimerShadow:
    """Thread-safe mirror of the device's timer slots."""

    def __init__(self, max_timers: int = MAX_TIMERS) -> None:
        self._max = max_timers
        self._timers: list[ShadowTimer] = []
        self._lock = threading.RLock()

    # -- housekeeping ------------------------------------------------------

    def _expire(self, now: Optional[float] = None) -> None:
        """Drop timers the firmware would already have cleared."""
        now = time.monotonic() if now is None else now
        self._timers = [
            t for t in self._timers
            if t.ends_at + ALARM_DURATION_SECONDS > now
        ]

    def snapshot(self) -> list[ShadowTimer]:
        with self._lock:
            self._expire()
            return list(self._timers)

    def active_count(self) -> int:
        return len(self.snapshot())

    def has_room(self) -> bool:
        return self.active_count() < self._max

    def find(self, name: str) -> Optional[ShadowTimer]:
        with self._lock:
            self._expire()
            for timer in self._timers:
                if timer.name.lower() == name.lower():
                    return timer
        return None

    # -- name resolution ---------------------------------------------------

    def next_default_name(self) -> str:
        """Pick an unused "Timer N" label, matching the firmware's convention."""
        with self._lock:
            self._expire()
            taken = {t.name.lower() for t in self._timers}
            for index in range(1, self._max + 1):
                candidate = f"Timer {index}"
                if candidate.lower() not in taken:
                    return candidate
        return "Timer"

    def resolve_name(self, hint: Optional[str], kind: CommandKind) -> Optional[str]:
        """Map an NLU name hint onto a concrete timer name.

        Passed to :func:`tivvy_bridge.nlu.parse` as ``resolve_name``.
        """
        with self._lock:
            self._expire()
            timers = list(self._timers)

        # Ordinal reference: "timer two" -> the second slot in creation order.
        if hint and hint.startswith("#"):
            try:
                index = int(hint[1:])
            except ValueError:
                return None
            if 1 <= index <= len(timers):
                return timers[index - 1].name
            if kind is CommandKind.SET:
                return f"Timer {index}"
            return None

        if hint:
            return hint

        # No name spoken.
        if kind is CommandKind.SET:
            return self.next_default_name()

        # Destructive/adjusting commands: only auto-target when unambiguous.
        if len(timers) == 1:
            return timers[0].name
        if not timers:
            log.info("no timer name spoken and no timers are running")
            return None
        log.info(
            "no timer name spoken and %d timers are running (%s) - refusing to guess",
            len(timers), ", ".join(t.name for t in timers),
        )
        return None

    # -- optimistic application -------------------------------------------

    def plausibility(self, command: Command) -> Optional[str]:
        """Return a warning string when the device will likely ignore this."""
        with self._lock:
            self._expire()
            timers = list(self._timers)
        names = {t.name.lower() for t in timers}

        if command.kind is CommandKind.SET:
            if len(timers) >= self._max:
                return f"device already has {self._max} timers; SET will be ignored"
            if command.name.lower() in names:
                return (
                    f"a timer named {command.name!r} already exists; the firmware "
                    "will create a duplicate that cannot be addressed by name"
                )
        elif command.kind in (CommandKind.CANCEL, CommandKind.ADD, CommandKind.MINUS):
            if command.name.lower() not in names:
                return f"no shadow timer named {command.name!r}; command may be a no-op"
        elif command.kind is CommandKind.STOP:
            if not any(t.is_ringing() for t in timers):
                return "nothing appears to be ringing; STOP will be a no-op"
        return None

    def apply(self, command: Command) -> None:
        """Fold a command we just sent into the shadow."""
        now = time.monotonic()
        with self._lock:
            self._expire(now)

            if command.kind is CommandKind.STOP:
                self._timers = [t for t in self._timers if not t.is_ringing(now)]
                return

            if command.kind is CommandKind.SET:
                if len(self._timers) >= self._max:
                    return
                self._timers.append(
                    ShadowTimer(
                        name=command.name,
                        total_seconds=command.seconds,
                        ends_at=now + command.seconds,
                    )
                )
                return

            target = None
            for timer in self._timers:
                if timer.name.lower() == command.name.lower():
                    target = timer
                    break
            if target is None:
                return

            if command.kind is CommandKind.CANCEL:
                self._timers.remove(target)
            elif command.kind is CommandKind.ADD:
                target.ends_at += command.seconds
                target.total_seconds += command.seconds
            elif command.kind is CommandKind.MINUS:
                target.ends_at = max(now, target.ends_at - command.seconds)

    # -- optional real state from the device -------------------------------

    def apply_device_state(self, payload: str) -> bool:
        """Parse a ``STATE:`` notification from the firmware.

        Expected shape (one timer per field, semicolon separated)::

            STATE:COUNT:2;T:Baking,REMAIN:540,TOTAL:1200;T:Break,REMAIN:60,TOTAL:300

        Over BLE the firmware notifies this verbatim. Over USB serial the same
        line arrives inside the firmware's log output (``[TX] STATE:...``), so
        we locate the marker rather than requiring it at position 0.

        Returns True when the payload was understood.
        """
        payload = (payload or "").strip()
        marker = payload.find("STATE:COUNT:")
        if marker < 0:
            return False
        payload = payload[marker:]

        now = time.monotonic()
        timers: list[ShadowTimer] = []
        for chunk in payload.split(";"):
            chunk = chunk.strip()
            if not chunk.startswith("T:"):
                continue
            fields = {}
            for part in chunk.split(","):
                key, _, value = part.partition(":")
                fields[key.strip().upper()] = value.strip()
            name = fields.get("T")
            if not name:
                continue
            try:
                remain = int(fields.get("REMAIN", "0"))
                total = int(fields.get("TOTAL", remain))
            except ValueError:
                continue
            timers.append(ShadowTimer(name=name, total_seconds=total, ends_at=now + remain))

        with self._lock:
            self._timers = timers[: self._max]
        return True

    def describe(self) -> str:
        timers = self.snapshot()
        if not timers:
            return "no timers"
        return ", ".join(
            f"{t.name} {format_hhmmss(int(t.remaining()))}" for t in timers
        )
