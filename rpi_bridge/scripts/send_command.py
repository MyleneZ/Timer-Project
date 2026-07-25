#!/usr/bin/env python3
"""Drive the Qualia directly from a laptop - no microphone, no speech.

Sends the same `CMD:` wire protocol the voice bridge sends, and prints the
`ACK:` / `STATE:` notifications the firmware sends back, so you can see whether
the display actually accepted each command rather than guessing from across the
room.

    ./scripts/send_command.py --demo                    # scripted bring-up run
    ./scripts/send_command.py                           # interactive prompt
    ./scripts/send_command.py "CMD:SET,NAME:Baking,DURATION:120"
    ./scripts/send_command.py --say "set a baking timer for two minutes"
    ./scripts/send_command.py --transport stdout --demo # rehearse with no hardware

Re-executes itself under ./.venv, so there is nothing to activate first.
Requires the Qualia powered on and running device_code/production.ino.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VENV_DIR = ROOT / ".venv"
VENV_PY = VENV_DIR / "bin" / "python"


def _reexec_under_venv() -> None:
    if os.environ.get("TIVVY_SEND_REEXEC") == "1" or not VENV_PY.exists():
        return
    try:
        if Path(sys.prefix).resolve() == VENV_DIR.resolve():
            return
    except OSError:
        pass
    os.environ["TIVVY_SEND_REEXEC"] = "1"
    os.execv(str(VENV_PY), [str(VENV_PY), str(Path(__file__).resolve()), *sys.argv[1:]])


_reexec_under_venv()
sys.path.insert(0, str(ROOT))

import argparse  # noqa: E402
import logging  # noqa: E402
import queue  # noqa: E402
import time  # noqa: E402

from tivvy_bridge.config import BridgeConfig  # noqa: E402
from tivvy_bridge.nlu import NluOptions, parse  # noqa: E402
from tivvy_bridge.protocol import encode  # noqa: E402
from tivvy_bridge.transport import build_link  # noqa: E402

# (label, command, expect_ok, pause_after)
#
# MAX_TIMERS is 3, so step 7 is expected to be REFUSED - that is the point of
# it. Same for step 8: MINUS past zero must not wedge the slot.
DEMO = [
    ("create a timer",            "CMD:SET,NAME:Baking,DURATION:120",      True,  2.0),
    ("create a second",           "CMD:SET,NAME:Homework,DURATION:300",    True,  2.0),
    ("add a minute",              "CMD:ADD,NAME:Baking,DURATION:60",       True,  1.5),
    ("take 30s off",              "CMD:MINUS,NAME:Baking,DURATION:30",     True,  1.5),
    ("lowercase name matches",    "CMD:ADD,NAME:baking,DURATION:15",       True,  1.5),
    ("create a third",            "CMD:SET,NAME:Break,DURATION:240",       True,  2.0),
    ("fourth is refused",         "CMD:SET,NAME:Exercise,DURATION:60",     False, 1.5),
    ("minus past zero is safe",   "CMD:MINUS,NAME:Break,DURATION:99999",   True,  1.5),
    ("duplicate name",            "CMD:SET,NAME:Baking,DURATION:90",       None,  1.5),
    ("cancel one",                "CMD:CANCEL,NAME:Homework",              True,  1.5),
    ("cancel unknown is refused", "CMD:CANCEL,NAME:Nonexistent",           False, 1.5),
    ("stop any alarm",            "CMD:STOP",                              None,  1.0),
    ("clean up",                  "CMD:CANCEL,NAME:Baking",                True,  0.8),
    ("clean up",                  "CMD:CANCEL,NAME:Break",                 None,  0.5),
]


def describe_state(payload: str) -> str:
    """Render a STATE: notification as something readable."""
    marker = payload.find("STATE:")
    if marker < 0:
        return payload
    chunks = payload[marker:].split(";")
    timers = []
    for chunk in chunks[1:]:
        chunk = chunk.strip()
        if not chunk.startswith("T:"):
            continue
        name, remain = "?", "?"
        for part in chunk.split(","):
            if part.startswith("T:"):
                name = part[2:]
            elif part.startswith("REMAIN:"):
                remain = part[7:]
        timers.append(f"{name} {int(remain) // 60:02d}:{int(remain) % 60:02d}"
                      if remain.isdigit() else f"{name} {remain}")
    return ", ".join(timers) if timers else "(no timers)"


class Device:
    """A link plus the notifications coming back off it."""

    def __init__(self, cfg: BridgeConfig) -> None:
        self.inbox: "queue.Queue[str]" = queue.Queue()
        self.link = build_link(cfg.link, self.inbox.put)
        self.cfg = cfg

    def connect(self) -> bool:
        self.link.start()
        if self.cfg.link.transport == "stdout":
            return True
        print(f"connecting over {self.cfg.link.transport} ...")
        ok = self.link.wait_ready(timeout=self.cfg.link.scan_timeout_s + 5)
        print("connected\n" if ok else "NOT CONNECTED\n")
        return ok

    def drain(self, seconds: float) -> list[str]:
        """Collect notifications for `seconds`, printing each as it lands."""
        lines = []
        deadline = time.monotonic() + seconds
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return lines
            try:
                line = self.inbox.get(timeout=remaining)
            except queue.Empty:
                return lines
            lines.append(line)
            if "STATE:" in line:
                print(f"      <- timers: {describe_state(line)}")
            else:
                print(f"      <- {line}")

    def send(self, payload: str, wait: float = 1.5) -> list[str]:
        print(f"   -> {payload}")
        if not self.link.send(payload):
            print("      !! send failed (link down?)")
            return []
        return self.drain(wait)

    def close(self) -> None:
        self.link.close()


def ack_ok(lines: list[str]) -> bool | None:
    """True/False from an ACK:...,OK:n line, None if the device did not ack."""
    for line in lines:
        marker = line.find("ACK:")
        if marker < 0:
            continue
        tail = line[marker:]
        if "OK:1" in tail:
            return True
        if "OK:0" in tail:
            return False
    return None


def run_demo(device: Device) -> int:
    print("Scripted bring-up. Watch the display as each step runs.\n")
    failures = 0
    for index, (label, payload, expect, pause) in enumerate(DEMO, 1):
        print(f"{index:2d}. {label}")
        lines = device.send(payload, wait=pause)
        got = ack_ok(lines)

        if device.cfg.link.transport == "stdout":
            continue
        if got is None:
            print("      ?? no ACK - firmware may predate the ACK/STATE notifications")
        elif expect is None:
            pass
        elif got != expect:
            wanted = "accept" if expect else "refuse"
            print(f"      ** MISMATCH: expected the firmware to {wanted} this")
            failures += 1
    print()
    if device.cfg.link.transport == "stdout":
        print("Dry run finished - nothing was transmitted.")
        return 0
    print(f"{len(DEMO)} steps, {failures} mismatch(es).")
    return 1 if failures else 0


def to_payload(text: str, cfg: BridgeConfig) -> str | None:
    """Accept either a raw CMD: string or natural language."""
    text = text.strip()
    if text.upper().startswith("CMD:"):
        return text
    result = parse(text, options=NluOptions(
        set_without_duration_seconds=cfg.nlu.set_without_duration_seconds,
        bare_number_unit_seconds=cfg.nlu.bare_number_unit_seconds,
        adjust_without_duration_seconds=cfg.nlu.adjust_without_duration_seconds,
        name_fuzzy_cutoff=cfg.nlu.name_fuzzy_cutoff,
        max_words=cfg.nlu.max_words,
    ))
    if not result.command:
        print(f"   no command in {text!r} ({result.reason})")
        return None
    return encode(result.command)


def run_interactive(device: Device) -> int:
    print("Type a command and press Enter. Examples:")
    print("  CMD:SET,NAME:Baking,DURATION:120")
    print("  set a baking timer for two minutes")
    print("  cancel baking")
    print("Ctrl-D or 'quit' to exit.\n")
    while True:
        try:
            text = input("tivvy> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return 0
        if not text:
            device.drain(0.2)
            continue
        if text.lower() in ("quit", "exit", "q"):
            return 0
        payload = to_payload(text, device.cfg)
        if payload:
            device.send(payload, wait=1.5)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("commands", nargs="*", help="raw CMD: strings to send")
    parser.add_argument("-c", "--config")
    parser.add_argument("--transport", choices=("ble", "serial", "stdout"), default="ble")
    parser.add_argument("--address", help="pin the BLE address (a CoreBluetooth UUID on macOS)")
    parser.add_argument("--serial-port")
    parser.add_argument("--say", action="append", metavar="TEXT",
                        help="natural language, run through the NLU (repeatable)")
    parser.add_argument("--demo", action="store_true", help="scripted bring-up sequence")
    parser.add_argument("--listen", type=float, metavar="SECONDS",
                        help="just connect and print notifications for N seconds")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.INFO if args.verbose else logging.WARNING,
        format="%(levelname)-7s %(name)-20s %(message)s",
    )

    config_path = args.config
    if not config_path:
        for candidate in ("bridge.mac.toml", "bridge.toml", "/etc/tivvy/bridge.toml"):
            path = Path(candidate)
            if not path.is_absolute():
                path = ROOT / candidate
            if path.exists():
                config_path = str(path)
                break

    cfg = BridgeConfig.load(config_path)
    cfg.link.transport = args.transport
    if args.address:
        cfg.link.device_address = args.address
    if args.serial_port:
        cfg.link.serial_port = args.serial_port

    print(f"config    : {config_path or '(built-in defaults)'}")
    print(f"transport : {cfg.link.transport}")
    if cfg.link.transport == "ble":
        print(f"target    : {cfg.link.device_address or cfg.link.device_name + ' (by name/UUID)'}")

    device = Device(cfg)
    if not device.connect():
        device.close()
        print("\nThe display did not connect. Check that it is powered on, that "
              "production.ino is flashed, and that\n  ./scripts/send_command.py "
              "--transport ble --listen 5\nsees it. `python -m tivvy_bridge "
              "--scan-ble` lists everything advertising nearby.")
        return 1

    try:
        if args.listen is not None:
            print(f"listening for {args.listen:.0f}s ...")
            device.drain(args.listen)
            return 0
        if args.demo:
            return run_demo(device)

        payloads = [p for p in (to_payload(c, cfg) for c in args.commands) if p]
        payloads += [p for p in (to_payload(s, cfg) for s in (args.say or [])) if p]
        if payloads:
            for payload in payloads:
                device.send(payload, wait=1.5)
            return 0
        return run_interactive(device)
    finally:
        device.close()


if __name__ == "__main__":
    raise SystemExit(main())
