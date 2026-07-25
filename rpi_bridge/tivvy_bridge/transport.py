"""Links that carry command strings from the Pi to the Qualia.

``BleLink`` is the drop-in replacement for the Nicla: the Qualia keeps acting as
a Nordic UART Service peripheral and the Pi becomes the central that writes to
the RX characteristic. No firmware protocol change is required for this path.

``SerialLink`` is the wired alternative over the Qualia's USB CDC port. It needs
a small firmware change (route serial input through ``parseCommand``); see the
audit notes in the README.
"""

from __future__ import annotations

import asyncio
import logging
import threading
import time
from typing import Callable, Optional

from .config import LinkConfig
from .protocol import (
    NUS_RX_CHAR_UUID,
    NUS_SERVICE_UUID,
    NUS_TX_CHAR_UUID,
)

log = logging.getLogger(__name__)

NotifyHandler = Callable[[str], None]


class LinkError(RuntimeError):
    pass


class Link:
    """Interface for a command sink."""

    name = "link"

    def start(self) -> None:  # pragma: no cover - interface
        raise NotImplementedError

    def send(self, payload: str) -> bool:  # pragma: no cover - interface
        raise NotImplementedError

    def close(self) -> None:  # pragma: no cover - interface
        raise NotImplementedError

    @property
    def connected(self) -> bool:  # pragma: no cover - interface
        return False

    def wait_ready(self, timeout: float) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.connected:
                return True
            time.sleep(0.2)
        return self.connected


# --------------------------------------------------------------------------
# Dry run
# --------------------------------------------------------------------------


class StdoutLink(Link):
    name = "stdout"

    def __init__(self) -> None:
        self.sent: list[str] = []

    def start(self) -> None:
        log.info("dry run: commands will be printed, not transmitted")

    def send(self, payload: str) -> bool:
        self.sent.append(payload)
        print(f"[TX] {payload}", flush=True)
        return True

    def close(self) -> None:
        pass

    @property
    def connected(self) -> bool:
        return True


# --------------------------------------------------------------------------
# BLE
# --------------------------------------------------------------------------


class BleLink(Link):
    """BLE central that keeps a connection to the Qualia's NUS peripheral.

    Bleak is asyncio-only, so the event loop lives in its own daemon thread and
    the synchronous ``send()`` hands work to it via ``run_coroutine_threadsafe``.
    """

    name = "ble"

    def __init__(self, cfg: LinkConfig, on_notify: Optional[NotifyHandler] = None) -> None:
        self._cfg = cfg
        self._on_notify = on_notify
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._thread: Optional[threading.Thread] = None
        self._client = None
        self._connected = threading.Event()
        self._stopping = threading.Event()
        self._address: Optional[str] = cfg.device_address

    # -- lifecycle ---------------------------------------------------------

    def start(self) -> None:
        try:
            import bleak  # type: ignore  # noqa: F401
        except ModuleNotFoundError as exc:  # pragma: no cover - env dependent
            raise LinkError("bleak is not installed (`pip install bleak`)") from exc

        self._loop = asyncio.new_event_loop()
        self._thread = threading.Thread(target=self._run_loop, name="ble", daemon=True)
        self._thread.start()
        asyncio.run_coroutine_threadsafe(self._maintain(), self._loop)

    def _run_loop(self) -> None:  # pragma: no cover - thread body
        assert self._loop is not None
        asyncio.set_event_loop(self._loop)
        self._loop.run_forever()

    def close(self) -> None:
        self._stopping.set()
        loop = self._loop
        if loop is None:
            return
        future = asyncio.run_coroutine_threadsafe(self._disconnect(), loop)
        try:
            future.result(timeout=5)
        except Exception:  # pragma: no cover - teardown best effort
            log.debug("BLE disconnect failed", exc_info=True)
        loop.call_soon_threadsafe(loop.stop)
        if self._thread is not None:
            self._thread.join(timeout=5)

    @property
    def connected(self) -> bool:
        return self._connected.is_set()

    # -- sending -----------------------------------------------------------

    def send(self, payload: str) -> bool:
        if self._loop is None:
            raise LinkError("BleLink.start() was not called")
        if not self._connected.is_set():
            log.warning("BLE link is down; dropping %s", payload)
            return False
        future = asyncio.run_coroutine_threadsafe(self._write(payload), self._loop)
        try:
            return bool(future.result(timeout=self._cfg.send_timeout_s))
        except Exception as exc:
            log.warning("BLE write failed for %s: %s", payload, exc)
            self._connected.clear()
            return False

    async def _write(self, payload: str) -> bool:
        client = self._client
        if client is None or not client.is_connected:
            return False
        await client.write_gatt_char(
            NUS_RX_CHAR_UUID,
            payload.encode("ascii"),
            response=self._cfg.write_with_response,
        )
        return True

    # -- connection management --------------------------------------------

    async def _disconnect(self) -> None:
        client, self._client = self._client, None
        self._connected.clear()
        if client is not None and client.is_connected:
            await client.disconnect()

    async def _maintain(self) -> None:  # pragma: no cover - needs hardware
        backoff = self._cfg.reconnect_min_s
        while not self._stopping.is_set():
            try:
                connected = await self._connect_once()
            except Exception as exc:
                log.warning("BLE connect attempt failed: %s", exc)
                connected = False

            if connected:
                backoff = self._cfg.reconnect_min_s
                # Poll until the link drops.
                while not self._stopping.is_set():
                    client = self._client
                    if client is None or not client.is_connected:
                        break
                    await asyncio.sleep(1.0)
                if not self._stopping.is_set():
                    log.warning("BLE link lost; reconnecting")
                self._connected.clear()
                self._client = None
                continue

            await asyncio.sleep(backoff)
            backoff = min(self._cfg.reconnect_max_s, backoff * 1.7)

    async def _connect_once(self) -> bool:  # pragma: no cover - needs hardware
        from bleak import BleakClient, BleakScanner  # type: ignore

        scanner_kwargs = {}
        client_kwargs = {}
        if self._cfg.adapter:
            scanner_kwargs["adapter"] = self._cfg.adapter
            client_kwargs["adapter"] = self._cfg.adapter

        target = self._address
        if target is None:
            target = await self._discover(BleakScanner, scanner_kwargs)
            if target is None:
                return False

        log.info("connecting to %s", target)
        client = BleakClient(target, timeout=self._cfg.scan_timeout_s, **client_kwargs)
        await client.connect()

        if not _has_characteristic(client, NUS_RX_CHAR_UUID):
            log.error(
                "peer %s does not expose the NUS RX characteristic; is this the Qualia?",
                target,
            )
            await client.disconnect()
            self._address = None if self._cfg.device_address is None else self._address
            return False

        if self._cfg.subscribe_notifications and _has_characteristic(client, NUS_TX_CHAR_UUID):
            try:
                await client.start_notify(NUS_TX_CHAR_UUID, self._handle_notify)
            except Exception as exc:
                log.debug("could not subscribe to TX notifications: %s", exc)

        self._client = client
        self._address = target
        self._connected.set()
        log.info("BLE link up (%s)", target)
        return True

    async def _discover(self, BleakScanner, scanner_kwargs) -> Optional[str]:  # pragma: no cover
        """Find the Qualia by name, falling back to the NUS service UUID.

        The name fallback matters: NimBLE only advertises the device name when
        it fits in the 31-byte advertisement alongside the 128-bit service UUID,
        so name-only discovery is not guaranteed to work.
        """
        log.info("scanning for %r (%.0fs)", self._cfg.device_name, self._cfg.scan_timeout_s)
        devices = await BleakScanner.discover(
            timeout=self._cfg.scan_timeout_s,
            return_adv=True,
            **scanner_kwargs,
        )

        wanted_name = (self._cfg.device_name or "").lower()
        by_uuid = None
        for device, adv in devices.values():
            name = (adv.local_name or device.name or "").lower()
            if wanted_name and name == wanted_name:
                return device.address
            uuids = [u.lower() for u in (adv.service_uuids or [])]
            if NUS_SERVICE_UUID in uuids and by_uuid is None:
                by_uuid = device.address

        if by_uuid is not None:
            log.info("matched the Qualia by NUS service UUID at %s", by_uuid)
            return by_uuid

        log.warning("no Tivvy display found in scan results")
        return None

    def _handle_notify(self, _characteristic, data: bytearray) -> None:  # pragma: no cover
        try:
            text = bytes(data).decode("utf-8", errors="replace").strip()
        except Exception:
            return
        if not text:
            return
        log.info("[RX] %s", text)
        if self._on_notify is not None:
            try:
                self._on_notify(text)
            except Exception:
                log.debug("notification handler raised", exc_info=True)


def _has_characteristic(client, uuid: str) -> bool:  # pragma: no cover - needs hardware
    try:
        services = client.services
    except Exception:
        return False
    if services is None:
        return False
    for service in services:
        for char in service.characteristics:
            if char.uuid.lower() == uuid.lower():
                return True
    return False


# --------------------------------------------------------------------------
# Serial
# --------------------------------------------------------------------------


class SerialLink(Link):
    """USB CDC link to the Qualia.

    Sends the same ``CMD:...`` payload terminated with a newline. The stock
    firmware's serial handler does NOT understand this format - apply the
    firmware change described in the audit before relying on this transport.
    """

    name = "serial"

    def __init__(self, cfg: LinkConfig, on_notify: Optional[NotifyHandler] = None) -> None:
        self._cfg = cfg
        self._on_notify = on_notify
        self._serial = None
        self._lock = threading.Lock()
        self._stopping = threading.Event()
        self._reader: Optional[threading.Thread] = None

    def start(self) -> None:
        try:
            import serial  # type: ignore  # noqa: F401
        except ModuleNotFoundError as exc:  # pragma: no cover - env dependent
            raise LinkError("pyserial is not installed (`pip install pyserial`)") from exc
        self._open()
        self._reader = threading.Thread(target=self._read_loop, name="serial-rx", daemon=True)
        self._reader.start()

    def _open(self) -> bool:
        import serial  # type: ignore

        try:
            self._serial = serial.Serial(
                self._cfg.serial_port,
                self._cfg.serial_baud,
                timeout=0.5,
                write_timeout=self._cfg.send_timeout_s,
            )
            log.info("serial link up on %s", self._cfg.serial_port)
            return True
        except Exception as exc:
            log.warning("could not open %s: %s", self._cfg.serial_port, exc)
            self._serial = None
            return False

    @property
    def connected(self) -> bool:
        return self._serial is not None and self._serial.is_open

    def send(self, payload: str) -> bool:
        with self._lock:
            if not self.connected and not self._open():
                return False
            try:
                self._serial.write((payload + "\n").encode("ascii"))
                self._serial.flush()
                return True
            except Exception as exc:
                log.warning("serial write failed: %s", exc)
                try:
                    self._serial.close()
                except Exception:
                    pass
                self._serial = None
                return False

    def _read_loop(self) -> None:  # pragma: no cover - needs hardware
        backoff = self._cfg.reconnect_min_s
        while not self._stopping.is_set():
            if not self.connected:
                if not self._open():
                    time.sleep(backoff)
                    backoff = min(self._cfg.reconnect_max_s, backoff * 1.7)
                    continue
                backoff = self._cfg.reconnect_min_s
            try:
                raw = self._serial.readline()
            except Exception:
                self._serial = None
                continue
            if not raw:
                continue
            text = raw.decode("utf-8", errors="replace").strip()
            if not text:
                continue
            log.debug("[RX] %s", text)
            if self._on_notify is not None:
                try:
                    self._on_notify(text)
                except Exception:
                    log.debug("notification handler raised", exc_info=True)

    def close(self) -> None:
        self._stopping.set()
        if self._reader is not None:
            self._reader.join(timeout=2)
        with self._lock:
            if self._serial is not None:
                try:
                    self._serial.close()
                except Exception:  # pragma: no cover
                    pass
                self._serial = None


def build_link(cfg: LinkConfig, on_notify: Optional[NotifyHandler] = None) -> Link:
    transport = (cfg.transport or "ble").lower()
    if transport == "ble":
        return BleLink(cfg, on_notify)
    if transport == "serial":
        return SerialLink(cfg, on_notify)
    if transport in ("stdout", "none", "dry"):
        return StdoutLink()
    raise LinkError(f"unknown transport: {cfg.transport!r}")
