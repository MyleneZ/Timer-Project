"""Turn a dictated sentence into a Tivvy command.

This is the piece that replaces the Nicla's token-sequence parser. Because the
Pi produces free-form text instead of a stream of keyword-spotting tokens, the
parser has to be tolerant of filler words, ASR slips, and re-orderings:

    "set a timer called baking for twenty minutes"  -> SET  Baking 1200
    "start a 15 minute homework timer"              -> SET  Homework 900
    "add five more minutes to the baking timer"     -> ADD  Baking 300
    "take ten minutes off exercise"                 -> MINUS Exercise 600
    "cancel the break timer"                        -> CANCEL Break
    "stop"                                          -> STOP

The parser never guesses a destructive target: if a command needs a timer name
and the utterance does not identify one, it is rejected rather than applied to
an arbitrary timer.
"""

from __future__ import annotations

import difflib
import re
from dataclasses import dataclass
from typing import Callable, Iterable, Optional, Sequence

from .protocol import Command, CommandKind, ProtocolError, sanitize_name

# --------------------------------------------------------------------------
# Vocabulary
# --------------------------------------------------------------------------

_ONES = {
    "zero": 0, "one": 1, "two": 2, "three": 3, "four": 4, "five": 5,
    "six": 6, "seven": 7, "eight": 8, "nine": 9,
}
_TEENS = {
    "ten": 10, "eleven": 11, "twelve": 12, "thirteen": 13, "fourteen": 14,
    "fifteen": 15, "sixteen": 16, "seventeen": 17, "eighteen": 18, "nineteen": 19,
}
_TENS = {
    "twenty": 20, "thirty": 30, "forty": 40, "fifty": 50,
    "sixty": 60, "seventy": 70, "eighty": 80, "ninety": 90,
}

_UNITS = {
    "second": 1, "seconds": 1, "sec": 1, "secs": 1,
    "minute": 60, "minutes": 60, "min": 60, "mins": 60,
    "hour": 3600, "hours": 3600, "hr": 3600, "hrs": 3600,
}

#: Words allowed to sit between a number and its unit ("5 more minutes").
_NUMBER_UNIT_FILLER = {"more", "extra", "additional", "whole", "full", "another"}

#: Canonical timer name -> spoken aliases. The canonical spellings are the ones
#: the firmware's detect_theme_id() recognises, so themed art keeps working.
NAME_ALIASES = {
    "Baking": ["baking", "bake", "baked", "oven", "cake", "cookies"],
    "Cooking": ["cooking", "cook", "cooked", "kitchen", "food", "dinner", "lunch"],
    "Break": ["break", "breaks", "rest", "resting", "recess", "pause"],
    "Homework": ["homework", "home work", "study", "studying", "school", "reading"],
    "Exercise": ["exercise", "exercising", "run", "running", "cardio", "stretch"],
    "Workout": ["workout", "work out", "gym", "lifting", "weights"],
}

_ALIAS_TO_NAME = {
    alias: canonical
    for canonical, aliases in NAME_ALIASES.items()
    for alias in aliases
}

# Command trigger words, checked by earliest position in the utterance.
_TRIGGERS: Sequence[tuple[CommandKind, tuple[str, ...]]] = (
    (CommandKind.STOP, ("stop", "quiet", "silence", "enough", "hush", "dismiss")),
    (CommandKind.CANCEL, ("cancel", "delete", "clear", "abort", "scrap")),
    (CommandKind.ADD, ("add", "extend", "increase", "lengthen", "plus")),
    (CommandKind.MINUS, ("minus", "subtract", "reduce", "shorten", "shave", "trim")),
    (CommandKind.SET, ("set", "start", "create", "make", "begin", "new", "launch")),
)

#: Triggers whose meaning depends on whether a duration was spoken.
#: "remove the baking timer" is a CANCEL, "remove five minutes" is a MINUS.
_AMBIGUOUS_TRIGGERS = {
    "remove": (CommandKind.CANCEL, CommandKind.MINUS),
    "take": (CommandKind.CANCEL, CommandKind.MINUS),
    "cut": (CommandKind.CANCEL, CommandKind.MINUS),
    "kill": (CommandKind.CANCEL, CommandKind.CANCEL),
}

#: Explicit "the timer is called X" markers.
_NAME_MARKERS = ("called", "named", "name")

#: Never treat these as a timer name.
_NAME_STOPWORDS = (
    set(_ONES) | set(_TEENS) | set(_TENS) | set(_UNITS) | set(_NUMBER_UNIT_FILLER)
    | {word for _, words in _TRIGGERS for word in words}
    | set(_AMBIGUOUS_TRIGGERS)
    | {
        "a", "an", "the", "and", "to", "for", "from", "of", "on", "off", "my", "please",
        "timer", "timers", "half", "hundred", "up", "down", "it", "that", "this",
        "there", "then", "is", "was", "be", "hey", "ok", "okay", "tivvy", "with",
        "all", "any", "one's", "s", "i", "want", "need", "let", "lets", "us",
    }
)

_WORD_RE = re.compile(r"[a-z0-9']+")

#: Words the ASR commonly returns that carry no meaning for us.
_UNKNOWN_TOKEN = "[unk]"

def recognizer_vocabulary() -> list[str]:
    """Every word the decoder needs, used to build the Vosk grammar.

    Constraining the decoder to this list is the biggest accuracy lever on a
    Pi 4. Note that every word here must exist in the acoustic model's lexicon;
    invented words (like a "Tivvy" wake word) are out-of-vocabulary and are
    dropped from the grammar, so they can never be recognised.
    """
    words: set[str] = set()
    words.update(_ONES, _TEENS, _TENS, _UNITS)
    words.update(_NUMBER_UNIT_FILLER)
    for _, triggers in _TRIGGERS:
        words.update(triggers)
    words.update(_AMBIGUOUS_TRIGGERS)
    words.update(_NAME_MARKERS)
    for aliases in NAME_ALIASES.values():
        for alias in aliases:
            words.update(alias.split())
    words.update({
        "a", "an", "the", "and", "to", "for", "from", "of", "on", "off", "my",
        "please", "timer", "timers", "half", "hundred", "hey", "all",
    })
    return sorted(w for w in words if w)


# --------------------------------------------------------------------------
# Configuration knobs the app layer passes down
# --------------------------------------------------------------------------


@dataclass
class NluOptions:
    #: Seconds to use for a SET with no spoken duration. 0 rejects the command,
    #: which is the safe default: a misheard duration is worse than no timer.
    set_without_duration_seconds: int = 0
    #: A bare number with no unit ("set baking for twenty") is read as minutes.
    bare_number_unit_seconds: int = 60
    #: Default ADD/MINUS amount when only a bare "add time to baking" is heard.
    adjust_without_duration_seconds: int = 60
    #: Fuzzy-match cutoff for recovering a timer name from an ASR slip.
    name_fuzzy_cutoff: float = 0.82
    #: Longest utterance (in words) we will attempt to parse.
    max_words: int = 24


@dataclass
class ParseResult:
    """Outcome of parsing one utterance."""

    command: Optional[Command] = None
    reason: str = ""
    text: str = ""

    def __bool__(self) -> bool:
        return self.command is not None


# --------------------------------------------------------------------------
# Tokenisation and numbers
# --------------------------------------------------------------------------


def tokenize(text: str) -> list[str]:
    text = (text or "").lower().replace("-", " ").replace("_", " ")
    text = text.replace(_UNKNOWN_TOKEN, " ")
    return _WORD_RE.findall(text)


def _parse_number(tokens: Sequence[str], i: int) -> tuple[Optional[int], int]:
    """Parse an English number starting at ``tokens[i]``.

    Returns ``(value, next_index)``; ``value`` is None when no number is there.
    Handles ``5``, ``five``, ``fifteen``, ``twenty five``, ``two hundred``.
    """
    n = len(tokens)
    if i >= n:
        return None, i

    if tokens[i].isdigit():
        return int(tokens[i]), i + 1

    total = 0
    found = False

    # [ones] hundred [and]
    if tokens[i] in _ONES and i + 1 < n and tokens[i + 1] == "hundred":
        total += _ONES[tokens[i]] * 100
        i += 2
        found = True
    elif tokens[i] == "hundred":
        total += 100
        i += 1
        found = True
    if found and i < n and tokens[i] == "and":
        i += 1

    if i < n and tokens[i] in _TENS:
        total += _TENS[tokens[i]]
        i += 1
        found = True
        if i < n and tokens[i] in _ONES and _ONES[tokens[i]] > 0:
            total += _ONES[tokens[i]]
            i += 1
    elif i < n and tokens[i] in _TEENS:
        total += _TEENS[tokens[i]]
        i += 1
        found = True
    elif i < n and tokens[i] in _ONES:
        total += _ONES[tokens[i]]
        i += 1
        found = True
    elif i < n and tokens[i].isdigit():
        total += int(tokens[i])
        i += 1
        found = True

    return (total, i) if found else (None, i)


def _and_a_half(tokens: Sequence[str], i: int, unit_seconds: int) -> tuple[int, int]:
    """Consume a trailing "and a half" / "and half"; returns (extra_seconds, i)."""
    n = len(tokens)
    if i >= n or tokens[i] != "and":
        return 0, i
    j = i + 1
    if j < n and tokens[j] in ("a", "an"):
        j += 1
    if j < n and tokens[j] == "half":
        return unit_seconds // 2, j + 1
    return 0, i


def parse_duration(tokens: Sequence[str], options: NluOptions) -> Optional[int]:
    """Sum every ``<number> <unit>`` pair in the utterance into seconds."""
    n = len(tokens)
    total = 0
    found = False
    pending_bare: Optional[int] = None
    i = 0

    while i < n:
        token = tokens[i]

        # "half an hour", "half hour"
        if token == "half":
            j = i + 1
            if j < n and tokens[j] in ("a", "an"):
                j += 1
            if j < n and tokens[j] in _UNITS:
                total += _UNITS[tokens[j]] // 2
                found = True
                i = j + 1
                continue
            i += 1
            continue

        # "a minute", "an hour", "an hour and a half"
        if token in ("a", "an"):
            j = i + 1
            if j < n and tokens[j] in _UNITS:
                unit_seconds = _UNITS[tokens[j]]
                total += unit_seconds
                found = True
                extra, i = _and_a_half(tokens, j + 1, unit_seconds)
                total += extra
                continue
            i += 1
            continue

        value, j = _parse_number(tokens, i)
        if value is None:
            i += 1
            continue

        k = j
        while k < n and tokens[k] in _NUMBER_UNIT_FILLER:
            k += 1

        if k < n and tokens[k] in _UNITS:
            unit_seconds = _UNITS[tokens[k]]
            total += value * unit_seconds
            found = True
            # "one hour and a half"
            extra, i = _and_a_half(tokens, k + 1, unit_seconds)
            total += extra
            continue

        # Number with no unit: remember it in case no unit shows up at all.
        if pending_bare is None:
            pending_bare = value
        i = j

    if not found and pending_bare is not None:
        total = pending_bare * options.bare_number_unit_seconds
        found = True

    return total if found else None


# --------------------------------------------------------------------------
# Name extraction
# --------------------------------------------------------------------------


def _canonical_from_alias(phrase: str) -> Optional[str]:
    return _ALIAS_TO_NAME.get(phrase)


def _fuzzy_name(token: str, cutoff: float) -> Optional[str]:
    matches = difflib.get_close_matches(token, list(_ALIAS_TO_NAME), n=1, cutoff=cutoff)
    if matches:
        return _ALIAS_TO_NAME[matches[0]]
    return None


def extract_name(tokens: Sequence[str], options: NluOptions) -> Optional[str]:
    """Find the timer name in an utterance, or None if it does not name one."""
    n = len(tokens)

    # 1. Explicit marker: "called baking", "named my timer".
    for i, token in enumerate(tokens):
        if token in _NAME_MARKERS and i + 1 < n:
            # Take up to two following words that are not stopwords.
            words: list[str] = []
            j = i + 1
            while j < n and len(words) < 2:
                nxt = tokens[j]
                if nxt in ("the", "a", "an", "my"):
                    j += 1
                    continue
                if nxt in _NAME_STOPWORDS:
                    break
                words.append(nxt)
                j += 1
            if words:
                phrase = " ".join(words)
                return _canonical_from_alias(phrase) or _canonical_from_alias(words[0]) \
                    or _title(phrase)

    # 2. Two-word known aliases ("work out", "home work").
    for i in range(n - 1):
        canonical = _canonical_from_alias(f"{tokens[i]} {tokens[i + 1]}")
        if canonical:
            return canonical

    # 3. Single known alias anywhere.
    for token in tokens:
        canonical = _canonical_from_alias(token)
        if canonical:
            return canonical

    # 4. "timer one" / "timer 2" -> ordinal reference, resolved later.
    for i, token in enumerate(tokens):
        if token in ("timer", "timers") and i + 1 < n:
            value, _ = _parse_number(tokens, i + 1)
            if value is not None and 1 <= value <= 9:
                return f"#{value}"

    # 5. Fuzzy recovery for ASR slips ("bakin", "homewor", "exercize").
    for token in tokens:
        if token in _NAME_STOPWORDS or len(token) < 4:
            continue
        canonical = _fuzzy_name(token, options.name_fuzzy_cutoff)
        if canonical:
            return canonical

    # 6. "<word> timer" where <word> is unknown but clearly a label.
    for i in range(1, n):
        if tokens[i] in ("timer", "timers"):
            candidate = tokens[i - 1]
            if candidate not in _NAME_STOPWORDS and len(candidate) >= 3:
                return _title(candidate)

    return None


def _title(phrase: str) -> str:
    return " ".join(word[:1].upper() + word[1:] for word in phrase.split())


# --------------------------------------------------------------------------
# Command detection
# --------------------------------------------------------------------------


def _detect_kind(tokens: Sequence[str], has_duration: bool) -> Optional[CommandKind]:
    """Pick the command by the earliest trigger word in the utterance."""
    best_index: Optional[int] = None
    best_kind: Optional[CommandKind] = None

    for index, token in enumerate(tokens):
        kind: Optional[CommandKind] = None
        if token in _AMBIGUOUS_TRIGGERS:
            without_dur, with_dur = _AMBIGUOUS_TRIGGERS[token]
            kind = with_dur if has_duration else without_dur
        else:
            for candidate_kind, triggers in _TRIGGERS:
                if token in triggers:
                    kind = candidate_kind
                    break
        if kind is None:
            continue
        if best_index is None or index < best_index:
            best_index = index
            best_kind = kind

    # "plus"/"minus" spoken alone with a duration but no explicit verb still works
    # through the trigger table above; nothing extra needed here.
    return best_kind


def _strip_wake_words(tokens: list[str], wake_phrases: Iterable[str]) -> list[str]:
    """Drop a leading wake phrase ("hey tivvy set a timer ..." -> "set a ...")."""
    phrases = sorted(
        (tokenize(p) for p in wake_phrases if p),
        key=len,
        reverse=True,  # longest match first, so "hey tivvy" beats "tivvy"
    )
    changed = True
    while changed and tokens:
        changed = False
        for phrase in phrases:
            if phrase and tokens[:len(phrase)] == phrase:
                del tokens[:len(phrase)]
                changed = True
                break
    return tokens


# --------------------------------------------------------------------------
# Public entry point
# --------------------------------------------------------------------------


def parse(
    text: str,
    options: Optional[NluOptions] = None,
    resolve_name: Optional[Callable[[Optional[str], CommandKind], Optional[str]]] = None,
    wake_words: Iterable[str] = (),
) -> ParseResult:
    """Parse one dictated utterance.

    ``resolve_name`` is an optional hook (see :mod:`tivvy_bridge.state`) that maps
    a possibly-missing or ordinal name onto a timer the device actually has. It
    receives the raw hint (``None``, ``"#2"``, or a name) plus the command kind
    and returns a concrete name or None.
    """
    options = options or NluOptions()
    tokens = tokenize(text)
    if not tokens:
        return ParseResult(reason="empty utterance", text=text)
    if len(tokens) > options.max_words:
        return ParseResult(reason=f"utterance too long ({len(tokens)} words)", text=text)

    tokens = _strip_wake_words(tokens, wake_words)
    if not tokens:
        return ParseResult(reason="wake word only", text=text)

    duration = parse_duration(tokens, options)
    kind = _detect_kind(tokens, has_duration=duration is not None)
    if kind is None:
        return ParseResult(reason="no command word recognised", text=text)

    if kind is CommandKind.STOP:
        return ParseResult(command=Command(CommandKind.STOP), text=text)

    name_hint = extract_name(tokens, options)

    if resolve_name is not None:
        name = resolve_name(name_hint, kind)
    else:
        name = None if name_hint is None or name_hint.startswith("#") else name_hint

    if not name:
        if name_hint and name_hint.startswith("#"):
            return ParseResult(reason=f"no timer in slot {name_hint[1:]}", text=text)
        return ParseResult(reason="command needs a timer name", text=text)

    if kind is CommandKind.CANCEL:
        try:
            return ParseResult(command=Command(CommandKind.CANCEL, name=sanitize_name(name)), text=text)
        except ProtocolError as exc:
            return ParseResult(reason=str(exc), text=text)

    if duration is None:
        if kind is CommandKind.SET:
            fallback = options.set_without_duration_seconds
        else:
            fallback = options.adjust_without_duration_seconds
        if fallback <= 0:
            return ParseResult(reason=f"{kind.value} needs a duration", text=text)
        duration = fallback

    if duration <= 0:
        return ParseResult(reason="duration parsed as zero", text=text)

    try:
        command = Command(kind, name=sanitize_name(name), seconds=int(duration))
    except ProtocolError as exc:
        return ParseResult(reason=str(exc), text=text)

    return ParseResult(command=command, text=text)
