"""Protocol encoding and shadow-state tests."""

from __future__ import annotations

import pytest

from tivvy_bridge.protocol import (
    MAX_NAME_LEN,
    Command,
    CommandKind,
    ProtocolError,
    clamp_duration,
    encode,
    sanitize_name,
)
from tivvy_bridge.state import TimerShadow


# --------------------------------------------------------------------------
# Names
# --------------------------------------------------------------------------

@pytest.mark.parametrize("raw,expected", [
    ("baking", "Baking"),
    ("BAKING", "Baking"),
    ("  baking  ", "Baking"),
    ("home work", "Home Work"),
    ("laundry", "Laundry"),
])
def test_sanitize_name(raw, expected):
    assert sanitize_name(raw) == expected


def test_name_is_truncated_to_the_firmware_buffer():
    name = sanitize_name("supercalifragilistic")
    assert len(name) <= MAX_NAME_LEN


def test_commas_are_stripped_because_they_terminate_the_name_field():
    # production.ino reads NAME up to the next comma; a comma would truncate it.
    assert "," not in sanitize_name("baking, please")


def test_empty_name_is_rejected():
    with pytest.raises(ProtocolError):
        sanitize_name("   ")


# --------------------------------------------------------------------------
# Durations
# --------------------------------------------------------------------------

def test_zero_duration_is_rejected():
    # A zero-second timer never rings in production.ino and wedges a slot.
    with pytest.raises(ProtocolError):
        clamp_duration(0)
    with pytest.raises(ProtocolError):
        encode(Command(CommandKind.SET, name="Baking", seconds=0))


def test_duration_is_capped():
    assert clamp_duration(10 ** 9) == 24 * 3600


# --------------------------------------------------------------------------
# Shadow state
# --------------------------------------------------------------------------

def test_shadow_tracks_lifecycle():
    shadow = TimerShadow()
    shadow.apply(Command(CommandKind.SET, "Baking", 600))
    assert shadow.active_count() == 1

    shadow.apply(Command(CommandKind.ADD, "Baking", 60))
    timer = shadow.find("Baking")
    assert timer is not None
    assert timer.total_seconds == 660

    shadow.apply(Command(CommandKind.CANCEL, "Baking"))
    assert shadow.active_count() == 0


def test_shadow_respects_the_three_timer_limit():
    shadow = TimerShadow()
    for i in range(4):
        shadow.apply(Command(CommandKind.SET, f"Timer {i + 1}", 600))
    assert shadow.active_count() == 3


def test_plausibility_flags_a_full_device():
    shadow = TimerShadow()
    for i in range(3):
        shadow.apply(Command(CommandKind.SET, f"Timer {i + 1}", 600))
    warning = shadow.plausibility(Command(CommandKind.SET, "Baking", 600))
    assert warning and "already has 3" in warning


def test_plausibility_flags_a_duplicate_name():
    shadow = TimerShadow()
    shadow.apply(Command(CommandKind.SET, "Baking", 600))
    warning = shadow.plausibility(Command(CommandKind.SET, "Baking", 600))
    assert warning and "already exists" in warning


def test_plausibility_flags_an_unknown_target():
    shadow = TimerShadow()
    warning = shadow.plausibility(Command(CommandKind.CANCEL, "Baking"))
    assert warning and "no shadow timer" in warning


def test_device_state_notification_replaces_the_shadow():
    shadow = TimerShadow()
    shadow.apply(Command(CommandKind.SET, "Baking", 600))
    ok = shadow.apply_device_state(
        "STATE:COUNT:2;T:Homework,REMAIN:540,TOTAL:900;T:Break,REMAIN:60,TOTAL:300"
    )
    assert ok
    names = [t.name for t in shadow.snapshot()]
    assert names == ["Homework", "Break"]


def test_device_state_survives_the_serial_log_prefix():
    # Over USB serial the firmware's notifyDevice() line arrives as log output.
    shadow = TimerShadow()
    ok = shadow.apply_device_state("[TX] STATE:COUNT:1;T:Baking,REMAIN:120,TOTAL:600")
    assert ok
    assert [t.name for t in shadow.snapshot()] == ["Baking"]


def test_device_state_reports_an_empty_device():
    shadow = TimerShadow()
    shadow.apply(Command(CommandKind.SET, "Baking", 600))
    assert shadow.apply_device_state("STATE:COUNT:0")
    assert shadow.active_count() == 0


@pytest.mark.parametrize("line", [
    "[BOOT] Ready!",
    "ACK:SET,NAME:Baking,OK:1",
    "[TIMER] Created: Baking for 600 seconds",
    "",
])
def test_device_state_ignores_other_traffic(line):
    assert TimerShadow().apply_device_state(line) is False
