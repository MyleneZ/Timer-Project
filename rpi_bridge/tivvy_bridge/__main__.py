"""Command-line entry point: ``python -m tivvy_bridge``."""

from __future__ import annotations

import argparse
import asyncio
import logging
import sys
from typing import Optional

from . import __version__
from .app import Bridge
from .config import BridgeConfig


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="tivvy-bridge",
        description="Raspberry Pi voice bridge for the Tivvy timer display.",
    )
    parser.add_argument("-c", "--config", help="path to a TOML config file")
    parser.add_argument("--transport", choices=("ble", "serial", "stdout"),
                        help="override [link] transport")
    parser.add_argument("--address", help="override [link] device_address (BLE MAC)")
    parser.add_argument("--serial-port", help="override [link] serial_port")
    parser.add_argument("--asr", choices=("vosk", "whisper"), help="override [asr] backend")
    parser.add_argument("--model", help="override [asr] model_path")
    parser.add_argument("--log-level", help="DEBUG/INFO/WARNING/ERROR")
    parser.add_argument("--dry-run", action="store_true",
                        help="print commands instead of transmitting them")

    parser.add_argument("--say", action="append", metavar="TEXT",
                        help="parse TEXT as if it were dictated, then exit "
                             "(repeatable; no microphone needed)")
    parser.add_argument("--list-audio", action="store_true",
                        help="list input devices and exit")
    parser.add_argument("--scan-ble", action="store_true",
                        help="scan for BLE peripherals and exit")
    parser.add_argument("--check", action="store_true",
                        help="load config, model and link, report status, exit")
    parser.add_argument("--version", action="version", version=f"tivvy-bridge {__version__}")
    return parser


def apply_overrides(cfg: BridgeConfig, args: argparse.Namespace) -> BridgeConfig:
    if args.dry_run:
        cfg.link.transport = "stdout"
    if args.transport:
        cfg.link.transport = args.transport
    if args.address:
        cfg.link.device_address = args.address
    if args.serial_port:
        cfg.link.serial_port = args.serial_port
    if args.asr:
        cfg.asr.backend = args.asr
    if args.model:
        cfg.asr.model_path = args.model
    if args.log_level:
        cfg.log_level = args.log_level
    return cfg


def configure_logging(level: str) -> None:
    logging.basicConfig(
        level=getattr(logging, (level or "INFO").upper(), logging.INFO),
        format="%(asctime)s %(levelname)-7s %(name)-22s %(message)s",
        datefmt="%H:%M:%S",
    )


def scan_ble(cfg: BridgeConfig) -> int:
    from .protocol import NUS_SERVICE_UUID

    async def run() -> int:
        try:
            from bleak import BleakScanner  # type: ignore
        except ModuleNotFoundError:
            print("bleak is not installed (`pip install bleak`)", file=sys.stderr)
            return 1

        kwargs = {"adapter": cfg.link.adapter} if cfg.link.adapter else {}
        print(f"scanning for {cfg.link.scan_timeout_s:.0f}s ...")
        devices = await BleakScanner.discover(
            timeout=cfg.link.scan_timeout_s, return_adv=True, **kwargs
        )
        if not devices:
            print("no BLE peripherals found")
            return 1
        print(f"{'address':<20} {'rssi':>5}  name")
        for device, adv in sorted(devices.values(), key=lambda d: -(d[1].rssi or -999)):
            name = adv.local_name or device.name or "(no name)"
            marker = ""
            uuids = [u.lower() for u in (adv.service_uuids or [])]
            if NUS_SERVICE_UUID in uuids:
                marker = "  <-- Nordic UART Service (this is the Qualia)"
            print(f"{device.address:<20} {adv.rssi if adv.rssi is not None else 0:>5}  {name}{marker}")
        return 0

    return asyncio.run(run())


def run_say(cfg: BridgeConfig, phrases: list[str]) -> int:
    """Parse phrases without touching the microphone or the ASR model."""
    bridge = Bridge(cfg)
    bridge._link.start()  # noqa: SLF001 - deliberate: no mic, no ASR, link only
    if cfg.link.transport != "stdout":
        bridge._link.wait_ready(timeout=cfg.link.scan_timeout_s + 5)  # noqa: SLF001
    sent = 0
    try:
        for phrase in phrases:
            if bridge.handle_text(phrase) is not None:
                sent += 1
    finally:
        bridge.shutdown()
    return 0 if sent == len(phrases) else 2


def run_check(cfg: BridgeConfig) -> int:
    from .asr import build_recognizer
    from .audio import list_devices

    status = 0
    print(f"transport      : {cfg.link.transport}")
    print(f"asr backend    : {cfg.asr.backend}")
    print(f"asr model      : {cfg.asr.model_path}")
    print(f"vad backend    : {cfg.vad.backend}")
    print(f"wake word      : {'on ' + ', '.join(cfg.wake.phrases) if cfg.wake.enabled else 'off'}")
    print()
    print("input devices:")
    print(list_devices())
    print()

    try:
        recognizer = build_recognizer(cfg.asr, cfg.audio.samplerate)
        print(f"ASR            : OK ({recognizer.name})")
        recognizer.close()
    except Exception as exc:
        print(f"ASR            : FAILED - {exc}")
        status = 1

    from .transport import build_link

    link = build_link(cfg.link)
    try:
        link.start()
        ok = link.wait_ready(timeout=cfg.link.scan_timeout_s + 5)
        print(f"link           : {'OK' if ok else 'NOT CONNECTED'}")
        if not ok:
            status = 1
    except Exception as exc:
        print(f"link           : FAILED - {exc}")
        status = 1
    finally:
        link.close()

    return status


def main(argv: Optional[list[str]] = None) -> int:
    args = build_parser().parse_args(argv)

    try:
        cfg = BridgeConfig.load(args.config)
    except Exception as exc:
        print(f"config error: {exc}", file=sys.stderr)
        return 2

    cfg = apply_overrides(cfg, args)
    configure_logging(cfg.log_level)

    if args.list_audio:
        from .audio import list_devices
        print(list_devices())
        return 0

    if args.scan_ble:
        return scan_ble(cfg)

    if args.say:
        return run_say(cfg, args.say)

    if args.check:
        return run_check(cfg)

    try:
        return Bridge(cfg).run()
    except Exception as exc:
        logging.getLogger("tivvy_bridge").error("fatal: %s", exc, exc_info=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
