"""Wi-Fi TCP transport and mDNS discovery for swlink.

The watch advertises ``_swlink._tcp.local.`` on port 9899 once configured with
Wi-Fi. TCP carries exactly the same JSON-lines + chunk framing as USB, but the
first ``hello`` includes the USB-provisioned pairing token.
"""
from __future__ import annotations

import ipaddress
import logging
import socket
import threading
from collections.abc import Callable

from .protocol import Decoder, Message, encode

log = logging.getLogger(__name__)

SERVICE_TYPE = "_swlink._tcp.local."
DEFAULT_PORT = 9899


def discover(timeout: float = 3.0, mac: str | None = None) -> list[dict]:
    """Return mDNS-discovered swlink devices.

    Each result: ``name, host, port, mac, fw``. ``mac`` filters exact hardware
    identity when it is known from a prior USB pairing.
    """
    from zeroconf import ServiceBrowser, ServiceListener, Zeroconf

    found: dict[str, dict] = {}
    done = threading.Event()

    class Listener(ServiceListener):
        def add_service(self, zc, type_, name):
            info = zc.get_service_info(type_, name, timeout=int(timeout * 1000))
            if not info:
                return
            addresses = [str(ipaddress.ip_address(a)) for a in info.addresses]
            props = {k.decode(errors="replace"): v.decode(errors="replace")
                     for k, v in info.properties.items()}
            if not addresses:
                return
            value = {"name": name.removesuffix("."), "host": addresses[0], "port": info.port,
                     "mac": props.get("mac"), "fw": props.get("fw")}
            if not mac or value["mac"] == mac.lower():
                found[name] = value
                done.set()

        def update_service(self, zc, type_, name):
            self.add_service(zc, type_, name)

        def remove_service(self, zc, type_, name):
            found.pop(name, None)

    zc = Zeroconf()
    try:
        browser = ServiceBrowser(zc, SERVICE_TYPE, Listener())
        done.wait(timeout)
        browser.cancel()
        return list(found.values())
    finally:
        zc.close()


class TcpLink:
    """Threaded TCP stream with the same surface as SerialLink / BleLink."""

    def __init__(self, host: str, port: int = DEFAULT_PORT, token: str | None = None,
                 on_message: Callable[[Message], None] | None = None, timeout: float = 5.0):
        self.host, self.tcp_port, self.token = host, int(port), token or ""
        self.on_message = on_message or (lambda m: None)
        self.timeout = timeout
        self.port = f"tcp:{host}:{port}"
        self.connected = threading.Event()
        self._sock: socket.socket | None = None
        self._thread: threading.Thread | None = None
        self._stop = threading.Event()
        self._wlock = threading.Lock()

    def open(self) -> str:
        sock = socket.create_connection((self.host, self.tcp_port), self.timeout)
        sock.settimeout(0.25)
        self._sock = sock
        self._stop.clear()
        self.connected.set()
        self._thread = threading.Thread(target=self._reader, name="swlink-tcp", daemon=True)
        self._thread.start()
        # Authenticate before any other command. Device responds with hello(auth=true).
        self.send({"t": "hello", "token": self.token})
        log.info("tcp: connected %s", self.port)
        return self.port

    def close(self) -> None:
        self._stop.set()
        self.connected.clear()
        if self._sock:
            try:
                self._sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self._sock.close()
            self._sock = None
        if self._thread and self._thread is not threading.current_thread():
            self._thread.join(timeout=1)

    def reset_device(self) -> None:
        pass

    def send(self, obj: dict, data: bytes = b"") -> None:
        if not self._sock or not self.connected.is_set():
            raise RuntimeError("tcp link not open")
        frame = encode(obj, data)
        with self._wlock:
            self._sock.sendall(frame)

    def _reader(self) -> None:
        dec = Decoder()
        try:
            assert self._sock
            while not self._stop.is_set():
                try:
                    chunk = self._sock.recv(65536)
                except TimeoutError:
                    continue
                if not chunk:
                    raise ConnectionError("remote closed")
                for msg in dec.feed(chunk):
                    self.on_message(msg)
        except (OSError, ConnectionError) as e:
            if not self._stop.is_set():
                log.warning("tcp read failed: %s", e)
                self.connected.clear()
                self.on_message(Message({"t": "_link", "ev": "lost", "msg": f"tcp: {e}"}))
