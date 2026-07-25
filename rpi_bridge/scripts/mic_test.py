#!/usr/bin/env python3
"""Talk to your laptop and watch what the bridge hears.

This is the step where you test *yourself* rather than the code: the mic, the
room, your phrasing. It shows a live input-level meter while idle, so a dead
mic looks obviously different from a working mic that is failing to recognize
you - those two look identical if all you have is silence on stdout.

    ./scripts/mic_test.py                 # print commands, no hardware needed
    ./scripts/mic_test.py --list          # which input devices exist
    ./scripts/mic_test.py --device "USB"  # pick one by name substring or index
    ./scripts/mic_test.py --transport ble # actually drive the Qualia
    ./scripts/mic_test.py --wake          # require "hey timer" before a command

Re-executes itself under ./.venv if it is not already running there, so there
is nothing to activate first. Ctrl-C to stop and print a summary.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VENV_DIR = ROOT / ".venv"
VENV_PY = VENV_DIR / "bin" / "python"


def _reexec_under_venv() -> None:
    """Hand off to ./.venv/bin/python unless we are already running under it."""
    if os.environ.get("TIVVY_MIC_TEST_REEXEC") == "1":
        return
    if not VENV_PY.exists():
        return
    # In a venv, sys.prefix IS the venv directory; base_prefix is the real one.
    try:
        already = Path(sys.prefix).resolve() == VENV_DIR.resolve()
    except OSError:
        already = False
    if already:
        return
    os.environ["TIVVY_MIC_TEST_REEXEC"] = "1"
    os.execv(str(VENV_PY), [str(VENV_PY), str(Path(__file__).resolve()), *sys.argv[1:]])


_reexec_under_venv()
sys.path.insert(0, str(ROOT))

import argparse  # noqa: E402
import logging  # noqa: E402
import math  # noqa: E402
import time  # noqa: E402

try:
    from tivvy_bridge.app import Bridge
    from tivvy_bridge.asr import build_recognizer
    from tivvy_bridge.audio import Microphone, Segmenter, pcm_rms
    from tivvy_bridge.config import BridgeConfig
    from tivvy_bridge.nlu import parse
    from tivvy_bridge.protocol import encode
except ModuleNotFoundError as exc:  # pragma: no cover
    sys.exit(
        f"{exc}\n\nDependencies are missing. From {ROOT}:\n"
        f"  python3.12 -m venv .venv && .venv/bin/pip install -r requirements.txt"
    )

BAR_WIDTH = 30
TTY = sys.stdout.isatty()


def clear_meter() -> None:
    if TTY:
        sys.stdout.write("\r" + " " * (BAR_WIDTH + 46) + "\r")


def draw_meter(rms: float, peak: float, speech: bool, armed: bool) -> None:
    if not TTY:
        return
    dbfs = 20 * math.log10(max(rms, 1.0) / 32768.0)
    filled = max(0, min(BAR_WIDTH, int((dbfs + 60) / 60 * BAR_WIDTH)))
    bar = "#" * filled + "-" * (BAR_WIDTH - filled)
    state = "SPEECH" if speech else "  ..  "
    if armed:
        state = "ARMED " if not speech else "SPEECH"
    sys.stdout.write(f"\r  [{bar}] {dbfs:6.1f} dBFS  peak {peak:6.1f}  {state}  ")
    sys.stdout.flush()


def find_config(explicit: str | None) -> str | None:
    if explicit:
        return explicit
    for candidate in ("bridge.mac.toml", "bridge.toml", "/etc/tivvy/bridge.toml"):
        path = Path(candidate)
        if not path.is_absolute():
            path = ROOT / candidate
        if path.exists():
            return str(path)
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-c", "--config", help="TOML config (auto-detected if omitted)")
    parser.add_argument("-d", "--device", help="input device index or name substring")
    parser.add_argument("--transport", choices=("stdout", "ble", "serial"), default="stdout")
    parser.add_argument("--wake", action="store_true", help="require a wake phrase")
    parser.add_argument("--list", action="store_true", help="list input devices and exit")
    parser.add_argument("--verbose", action="store_true", help="show bridge INFO logging")
    parser.add_argument("--no-meter", action="store_true", help="suppress the level meter")
    args = parser.parse_args()

    if args.list:
        from tivvy_bridge.audio import list_devices
        print(list_devices())
        return 0

    logging.basicConfig(
        level=logging.INFO if args.verbose else logging.WARNING,
        format="%(levelname)-7s %(name)-20s %(message)s",
    )

    config_path = find_config(args.config)
    cfg = BridgeConfig.load(config_path)
    cfg.link.transport = args.transport
    cfg.transcript_log = None
    if args.device:
        cfg.audio.device = args.device
    if args.wake:
        cfg.wake.enabled = True
        cfg.wake.phrases = ["hey timer", "timer"]

    global TTY
    if args.no_meter:
        TTY = False

    print(f"config     : {config_path or '(built-in defaults)'}")
    print(f"model      : {cfg.asr.model_path}")
    print(f"transport  : {cfg.link.transport}")
    print(f"wake word  : {'on -> ' + ', '.join(cfg.wake.phrases) if cfg.wake.enabled else 'off'}")

    recognizer = build_recognizer(
        cfg.asr, cfg.audio.samplerate,
        [w for p in cfg.wake.phrases for w in p.split()] if cfg.wake.enabled else (),
    )
    bridge = Bridge(cfg, recognizer=recognizer)
    bridge._link.start()  # noqa: SLF001 - we drive the mic loop ourselves
    if cfg.link.transport != "stdout":
        print(f"link       : connecting ...")
        if not bridge._link.wait_ready(timeout=cfg.link.scan_timeout_s + 5):  # noqa: SLF001
            print("link       : NOT CONNECTED - commands will be dropped")

    segmenter = Segmenter(cfg.vad, cfg.audio.samplerate, cfg.audio.frame_ms)
    heard = sent = rejected = 0
    peak_db = -99.0
    started = time.monotonic()
    warned_silent = False

    print("\nSpeak now. Try: \"set a baking timer for twenty minutes\"")
    print("Ctrl-C to stop.\n")

    try:
        with Microphone(cfg.audio) as mic:
            for frame in mic.frames():
                rms = pcm_rms(frame)
                dbfs = 20 * math.log10(max(rms, 1.0) / 32768.0)
                peak_db = max(peak_db, dbfs)

                utterance = segmenter.push(frame)
                if utterance is None:
                    draw_meter(rms, peak_db, segmenter.triggered, False)
                    if not warned_silent and peak_db < -55.0 and time.monotonic() - started > 8:
                        clear_meter()
                        print("  (no input detected yet - check System Settings > Privacy "
                              "& Security > Microphone for your terminal)")
                        warned_silent = True
                    continue

                clear_meter()
                seconds = len(utterance) / 2 / cfg.audio.samplerate
                transcript = recognizer.transcribe(utterance)
                heard += 1

                if not transcript.text.strip():
                    print(f"  {seconds:4.1f}s  (nothing recognized)")
                    rejected += 1
                    continue

                result = parse(
                    transcript.text,
                    options=bridge.nlu_options,
                    resolve_name=bridge.shadow.resolve_name,
                    wake_words=cfg.wake.phrases if cfg.wake.enabled else (),
                )
                print(f"  {seconds:4.1f}s  conf {transcript.confidence:4.2f}  "
                      f"{transcript.text!r}")

                if not result.command:
                    print(f"          -> no command ({result.reason})")
                    rejected += 1
                    continue

                command = bridge.handle_text(transcript.text, transcript.confidence)
                if command is not None:
                    print(f"          -> {encode(command)}")
                    print(f"             timers: {bridge.shadow.describe() or '(none)'}")
                    sent += 1
                else:
                    print(f"          -> {encode(result.command)} "
                          f"NOT SENT (duplicate, low confidence, or link down)")
                    rejected += 1
    except KeyboardInterrupt:
        pass
    finally:
        clear_meter()
        bridge.shutdown()

    print(f"\n{heard} utterance(s), {sent} command(s) sent, {rejected} rejected.")
    if heard == 0:
        print("Nothing was ever segmented. Check --list and pass --device, or confirm "
              "microphone permission for your terminal.")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
