"""BLE transport: the swlink GATT service (RX write / TX notify) over bleak.

Same surface as SerialLink so the Hub can swap transports: open(), send(),
close(), .connected (Event), .port (address). bleak is asyncio-based, so this
runs its own event loop on a background thread; on_message is called from
that thread like SerialLink's reader thread.

macOS: the first use prompts for Bluetooth permission for the app that runs the
daemon (Terminal, etc.). Without it CoreBluetooth reports "Bluetooth device is
turned off" even when Bluetooth is on.
"""
from __future__ import annotations

import asyncio
import logging
import threading
from collections.abc import Callable

from .protocol import Decoder, Message, encode

log = logging.getLogger(__name__)

SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"


class BleLink:
    def __init__(self, address: str | None = None, name: str | None = None,
                 on_message: Callable[[Message], None] | None = None, scan_timeout: float = 8.0):
        self.address = address
        self.name = name
        self.on_message = on_message or (lambda m: None)
        self.scan_timeout = scan_timeout
        self.port: str | None = None
        self.connected = threading.Event()
        self._loop = asyncio.new_event_loop()
        self._thread = threading.Thread(target=self._loop.run_forever, name="swlink-ble", daemon=True)
        self._client = None
        self._dec = Decoder()
        self._mtu = 20

    # -- lifecycle -----------------------------------------------------------
    def open(self) -> str:
        self._thread.start()
        fut = asyncio.run_coroutine_threadsafe(self._connect(), self._loop)
        return fut.result(timeout=self.scan_timeout + 15)

    def close(self) -> None:
        if self._client is not None:
            try:
                asyncio.run_coroutine_threadsafe(self._client.disconnect(), self._loop).result(timeout=5)
            except Exception:
                pass
        self.connected.clear()
        self._loop.call_soon_threadsafe(self._loop.stop)

    def send(self, obj: dict, data: bytes = b"") -> None:
        if self._client is None or not self.connected.is_set():
            raise RuntimeError("ble link not open")
        frame = encode(obj, data)
        asyncio.run_coroutine_threadsafe(self._write(frame), self._loop).result(timeout=5)

    def reset_device(self) -> None:  # no control lines over BLE
        pass

    # -- asyncio side --------------------------------------------------------
    async def _connect(self) -> str:
        from bleak import BleakClient, BleakScanner

        target = None
        if self.address:
            target = self.address
        else:
            def match(d, adv):
                return SERVICE_UUID in [u.lower() for u in adv.service_uuids] or \
                       (self.name and adv.local_name == self.name)
            dev = await BleakScanner.find_device_by_filter(match, timeout=self.scan_timeout)
            if dev is None:
                raise RuntimeError("no swlink device found over BLE (is ble enabled on the watch and advertising?)")
            target = dev.address
            log.info("ble: found %s (%s)", dev.name, dev.address)

        client = BleakClient(target, disconnected_callback=self._on_disconnect)
        await client.connect()
        self._client = client
        self._mtu = max(20, client.mtu_size - 3) if client.mtu_size else 20
        await client.start_notify(TX_UUID, self._on_notify)
        self.port = f"ble:{target}"
        self.connected.set()
        log.info("ble: connected %s (mtu %d)", target, self._mtu)
        return self.port

    async def _write(self, frame: bytes) -> None:
        for off in range(0, len(frame), self._mtu):
            await self._client.write_gatt_char(RX_UUID, frame[off:off + self._mtu], response=False)

    def _on_notify(self, _char, data: bytearray) -> None:
        for msg in self._dec.feed(bytes(data)):
            try:
                self.on_message(msg)
            except Exception:
                log.exception("on_message handler failed")

    def _on_disconnect(self, _client) -> None:
        log.warning("ble: disconnected")
        self.connected.clear()
        self.on_message(Message({"t": "_link", "ev": "lost", "msg": "ble disconnected"}))
