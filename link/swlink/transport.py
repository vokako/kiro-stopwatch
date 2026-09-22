"""Serial transport to the StopWatch over USB CDC.

Exactly one SerialLink should hold the port at a time (the daemon). Other
programs talk to the daemon, not to the port.
"""
from __future__ import annotations

import glob
import logging
import threading
import time
from collections.abc import Callable

import serial

from .protocol import Decoder, Message, encode

log = logging.getLogger(__name__)

# Espressif USB-Serial/JTAG. Used to pick the right port when several usbmodems exist.
ESPRESSIF_VID = 0x303A
USB_JTAG_PID = 0x1001


def find_device_ports() -> list[str]:
    """Only ESP32-S3 USB-Serial/JTAG ports, never unrelated usbmodems."""
    try:
        from serial.tools import list_ports

        return sorted(p.device for p in list_ports.comports()
                      if p.vid == ESPRESSIF_VID and p.pid == USB_JTAG_PID)
    except Exception:  # pragma: no cover - list_ports is best effort
        return []


def find_ports() -> list[str]:
    """Candidate ports for the CLI: prefer the known device, else list USB modems."""
    return find_device_ports() or sorted(glob.glob("/dev/cu.usbmodem*"))


class SerialLink:
    """Owns the serial port; decodes frames on a thread; delivers via callback."""

    def __init__(self, port: str | None = None, on_message: Callable[[Message], None] | None = None):
        self.port = port
        self.on_message = on_message or (lambda m: None)
        self._ser: serial.Serial | None = None
        self._thread: threading.Thread | None = None
        self._stop = threading.Event()
        self._wlock = threading.Lock()
        self.connected = threading.Event()

    # -- lifecycle -----------------------------------------------------------
    def open(self) -> str:
        port = self.port
        if port is None:
            ports = find_ports()
            if len(ports) != 1:
                raise RuntimeError(f"expected exactly one device port, found {ports}; pass --port")
            port = ports[0]
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = 115200
        ser.timeout = 0.05
        ser.open()
        # USB-Serial/JTAG: DTR asserted => firmware sees a host and emits CDC output.
        # RTS asserted with DTR low would reset the chip, so keep RTS low.
        ser.rts = False
        ser.dtr = True
        self._ser = ser
        self.port = port
        self._stop.clear()
        self._thread = threading.Thread(target=self._reader, name="swlink-reader", daemon=True)
        self._thread.start()
        self.connected.set()
        log.info("opened %s", port)
        return port

    def close(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=1)
        if self._ser:
            try:
                self._ser.close()
            finally:
                self._ser = None
        self.connected.clear()

    def reset_device(self) -> None:
        """Pulse EN via the USB-JTAG control lines; the device reboots and re-sends hello."""
        assert self._ser
        self._ser.dtr = False
        self._ser.rts = True
        time.sleep(0.1)
        self._ser.rts = False
        self._ser.dtr = True

    # -- io ------------------------------------------------------------------
    def send(self, obj: dict, data: bytes = b"") -> None:
        if not self._ser:
            raise RuntimeError("link not open")
        frame = encode(obj, data)
        with self._wlock:
            self._ser.write(frame)

    def _reader(self) -> None:
        dec = Decoder()
        assert self._ser
        while not self._stop.is_set():
            try:
                chunk = self._ser.read(65536)
            except (serial.SerialException, OSError) as e:
                log.warning("serial read failed: %s", e)
                self.connected.clear()
                self.on_message(Message({"t": "_link", "ev": "lost", "msg": str(e)}))
                return
            if not chunk:
                continue
            for msg in dec.feed(chunk):
                try:
                    self.on_message(msg)
                except Exception:  # keep the reader alive whatever a handler does
                    log.exception("on_message handler failed")
