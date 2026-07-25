"""Transport dispatch and the bits of the BLE link that need no radio.

The BLE connect path itself needs hardware and stays uncovered, but the helpers
around it do not - and `_address_of` in particular exists because bleak accepts
either a BLEDevice or an address string, which is easy to get wrong silently.
"""

from __future__ import annotations

import os

import pytest

from tivvy_bridge.config import LinkConfig
from tivvy_bridge.transport import (
    BleLink,
    LinkError,
    SerialLink,
    StdoutLink,
    _address_of,
    build_link,
)


# --------------------------------------------------------------------------
# Dispatch
# --------------------------------------------------------------------------

@pytest.mark.parametrize("transport,expected", [
    ("ble", BleLink),
    ("BLE", BleLink),
    ("serial", SerialLink),
    ("stdout", StdoutLink),
    ("none", StdoutLink),
    ("dry", StdoutLink),
])
def test_build_link_dispatch(transport, expected):
    assert isinstance(build_link(LinkConfig(transport=transport)), expected)


def test_build_link_rejects_unknown_transport():
    with pytest.raises(LinkError, match="unknown transport"):
        build_link(LinkConfig(transport="carrier-pigeon"))


# --------------------------------------------------------------------------
# BLEDevice vs address string
# --------------------------------------------------------------------------

class FakeBleDevice:
    def __init__(self, address):
        self.address = address

    def __str__(self):
        return f"FakeBleDevice({self.address})"


def test_address_of_ble_device():
    assert _address_of(FakeBleDevice("AA:BB:CC:DD:EE:FF")) == "AA:BB:CC:DD:EE:FF"


def test_address_of_plain_string():
    # macOS hands out a CoreBluetooth UUID rather than a MAC; both are opaque.
    assert _address_of("85F9DC4D-13C4-831A-F32D-75945EC109B7") == \
        "85F9DC4D-13C4-831A-F32D-75945EC109B7"


def test_address_of_device_with_empty_address_falls_back_to_str():
    assert "FakeBleDevice" in _address_of(FakeBleDevice(""))


# --------------------------------------------------------------------------
# Stdout link
# --------------------------------------------------------------------------

def test_stdout_link_records_and_reports_connected(capsys):
    link = StdoutLink()
    link.start()
    assert link.connected is True
    assert link.send("CMD:STOP") is True
    assert link.sent == ["CMD:STOP"]
    assert "[TX] CMD:STOP" in capsys.readouterr().out
    link.close()


def test_stdout_link_wait_ready_is_immediate():
    link = StdoutLink()
    link.start()
    assert link.wait_ready(timeout=0.1) is True


# --------------------------------------------------------------------------
# BLE link lifecycle
# --------------------------------------------------------------------------

def test_ble_send_before_start_is_an_error():
    link = BleLink(LinkConfig(transport="ble"))
    with pytest.raises(LinkError, match="start"):
        link.send("CMD:STOP")


def test_ble_close_without_start_is_a_no_op():
    BleLink(LinkConfig(transport="ble")).close()


def test_ble_not_connected_before_start():
    assert BleLink(LinkConfig(transport="ble")).connected is False


@pytest.mark.skipif(
    os.environ.get("TIVVY_BLE_TESTS") != "1",
    reason="needs a real Bluetooth adapter; set TIVVY_BLE_TESTS=1 to run",
)
def test_ble_start_then_close_leaves_no_pending_task(caplog):
    """close() must cancel _maintain() rather than yank the loop out from under it."""
    link = BleLink(LinkConfig(transport="ble", scan_timeout_s=1.0))
    link.start()
    link.close()
    assert "Task was destroyed" not in caplog.text
    assert link._maintain_task is None  # noqa: SLF001
