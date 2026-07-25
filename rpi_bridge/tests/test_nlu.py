"""Parser tests. These run anywhere - no microphone, model or BLE needed."""

from __future__ import annotations

import pytest

from tivvy_bridge.nlu import NluOptions, parse, parse_duration, tokenize
from tivvy_bridge.protocol import CommandKind, encode
from tivvy_bridge.state import TimerShadow


OPTS = NluOptions()


def cmd(text, shadow=None, options=OPTS):
    resolver = shadow.resolve_name if shadow is not None else None
    return parse(text, options=options, resolve_name=resolver).command


# --------------------------------------------------------------------------
# Durations
# --------------------------------------------------------------------------

@pytest.mark.parametrize("text,seconds", [
    ("twenty minutes", 1200),
    ("20 minutes", 1200),
    ("twenty five minutes", 1500),
    ("fifteen minutes", 900),
    ("one hour", 3600),
    ("an hour", 3600),
    ("two hours", 7200),
    ("half an hour", 1800),
    ("a minute", 60),
    ("thirty seconds", 30),
    ("one hour and thirty minutes", 5400),
    ("an hour and a half", 5400),
    ("five more minutes", 300),
    ("ninety seconds", 90),
    ("a hundred minutes", 6000),
])
def test_parse_duration(text, seconds):
    assert parse_duration(tokenize(text), OPTS) == seconds


def test_bare_number_defaults_to_minutes():
    assert parse_duration(tokenize("for twenty"), OPTS) == 1200


def test_no_duration_returns_none():
    assert parse_duration(tokenize("cancel the baking timer"), OPTS) is None


# --------------------------------------------------------------------------
# SET
# --------------------------------------------------------------------------

@pytest.mark.parametrize("text,name,seconds", [
    ("set a timer called baking for twenty minutes", "Baking", 1200),
    ("set baking for twenty minutes", "Baking", 1200),
    ("start a fifteen minute homework timer", "Homework", 900),
    ("create an exercise timer for half an hour", "Exercise", 1800),
    ("set a break timer for five minutes", "Break", 300),
    ("make a workout timer for forty five minutes", "Workout", 2700),
    ("set the cooking timer for one hour", "Cooking", 3600),
    ("new laundry timer for ten minutes", "Laundry", 600),
])
def test_set_commands(text, name, seconds):
    command = cmd(text)
    assert command is not None, text
    assert command.kind is CommandKind.SET
    assert command.name == name
    assert command.seconds == seconds


def test_set_without_name_uses_next_default_slot():
    shadow = TimerShadow()
    command = cmd("set a timer for ten minutes", shadow)
    assert command.name == "Timer 1"
    shadow.apply(command)
    assert cmd("set a timer for five minutes", shadow).name == "Timer 2"


def test_set_without_duration_is_rejected_by_default():
    result = parse("set a baking timer")
    assert result.command is None
    assert "duration" in result.reason


def test_set_without_duration_can_be_given_a_fallback():
    options = NluOptions(set_without_duration_seconds=300)
    command = cmd("set a baking timer", options=options)
    assert command.seconds == 300


# --------------------------------------------------------------------------
# ADD / MINUS
# --------------------------------------------------------------------------

@pytest.mark.parametrize("text,kind,name,seconds", [
    ("add five minutes to the baking timer", CommandKind.ADD, "Baking", 300),
    ("add five more minutes to baking", CommandKind.ADD, "Baking", 300),
    ("extend homework by ten minutes", CommandKind.ADD, "Homework", 600),
    ("minus five minutes from baking", CommandKind.MINUS, "Baking", 300),
    ("subtract ten minutes from exercise", CommandKind.MINUS, "Exercise", 600),
    ("take ten minutes off exercise", CommandKind.MINUS, "Exercise", 600),
    ("remove five minutes from break", CommandKind.MINUS, "Break", 300),
])
def test_adjust_commands(text, kind, name, seconds):
    command = cmd(text)
    assert command is not None, text
    assert command.kind is kind
    assert command.name == name
    assert command.seconds == seconds


# --------------------------------------------------------------------------
# CANCEL / STOP
# --------------------------------------------------------------------------

@pytest.mark.parametrize("text,name", [
    ("cancel the baking timer", "Baking"),
    ("cancel the timer called homework", "Homework"),
    ("delete the break timer", "Break"),
    ("remove the workout timer", "Workout"),
])
def test_cancel_commands(text, name):
    command = cmd(text)
    assert command is not None, text
    assert command.kind is CommandKind.CANCEL
    assert command.name == name
    assert command.seconds == 0


@pytest.mark.parametrize("text", ["stop", "stop!", "hey stop", "quiet", "silence"])
def test_stop_commands(text):
    command = cmd(text)
    assert command is not None, text
    assert command.kind is CommandKind.STOP


# --------------------------------------------------------------------------
# Ambiguity and rejection
# --------------------------------------------------------------------------

def test_cancel_without_name_refuses_to_guess_when_ambiguous():
    shadow = TimerShadow()
    shadow.apply(cmd("set a baking timer for ten minutes", shadow))
    shadow.apply(cmd("set a break timer for five minutes", shadow))
    assert cmd("cancel the timer", shadow) is None


def test_cancel_without_name_targets_the_only_running_timer():
    shadow = TimerShadow()
    shadow.apply(cmd("set a baking timer for ten minutes", shadow))
    command = cmd("cancel the timer", shadow)
    assert command.kind is CommandKind.CANCEL
    assert command.name == "Baking"


def test_ordinal_reference_resolves_by_slot():
    shadow = TimerShadow()
    shadow.apply(cmd("set a baking timer for ten minutes", shadow))
    shadow.apply(cmd("set a break timer for five minutes", shadow))
    assert cmd("cancel timer two", shadow).name == "Break"


@pytest.mark.parametrize("text", [
    "",
    "what time is it",
    "the weather looks nice today",
    "i think we should go to the store later",
])
def test_non_commands_are_rejected(text):
    assert cmd(text) is None


def test_fuzzy_name_recovery():
    # ASR slips that should still land on a themed name.
    assert cmd("cancel the bakeing timer").name == "Baking"
    assert cmd("cancel the exercize timer").name == "Exercise"


# --------------------------------------------------------------------------
# Wire format
# --------------------------------------------------------------------------

def test_encoded_wire_format_matches_firmware():
    assert encode(cmd("set a baking timer for twenty minutes")) == \
        "CMD:SET,NAME:Baking,DURATION:1200"
    assert encode(cmd("cancel the baking timer")) == "CMD:CANCEL,NAME:Baking"
    assert encode(cmd("add one minute to baking")) == \
        "CMD:ADD,NAME:Baking,DURATION:60"
    assert encode(cmd("minus one minute from baking")) == \
        "CMD:MINUS,NAME:Baking,DURATION:60"
    assert encode(cmd("stop")) == "CMD:STOP"
