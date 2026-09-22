import sqlite3
from pathlib import Path

from swlink.history import History


def test_power_samples_are_minutely_downsampled_and_queryable(tmp_path: Path):
    h = History(tmp_path / "history.db")
    h.record_power("aa", {"bat": 80, "mv": 4000, "chg": False}, "usb", True)
    h.record_power("aa", {"bat": 79, "mv": 3990, "chg": False}, "usb", False)
    result = h.query("aa", 24)
    assert len(result["samples"]) == 1
    assert result["samples"][0][1:4] == [80, 4000, 0]
    assert result["samples"][0][4:] == ["usb", 1]
    h.close()


def test_events_are_keyed_by_device_and_time_range(tmp_path: Path):
    h = History(tmp_path / "history.db")
    h.event("aa", "connected", "usb")
    h.event("bb", "connected", "ble")
    assert h.query("aa")["events"][0][1:] == ["connected", "usb"]
    assert h.query("bb")["events"][0][1:] == ["connected", "ble"]
    h.close()
