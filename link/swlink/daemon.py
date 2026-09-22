"""swlink daemon: the one process that owns the serial port.

- applies the key mapping to button events
- broadcasts every device message to WebSocket clients (ws://127.0.0.1:9898)
- forwards any JSON text frame a client sends to the device

Chunk messages (``mic``) are delivered to clients as a JSON text frame followed
by one binary frame with the payload.
"""
from __future__ import annotations

import asyncio
import json
import logging
import time
from datetime import datetime
from typing import Any

import websockets
from websockets.asyncio.server import ServerConnection, serve
from websockets.http11 import Response
from websockets.datastructures import Headers
from pathlib import Path

UI_DIR = Path(__file__).with_name("ui")

from .keymap import KeyBackend, KeyMapper, parse_action
from .protocol import Message
from .transport import SerialLink, find_device_ports
from .tcp import TcpLink
from .discovery import choose_wifi_target
from .ble import BleLink
from .triggers import ALL_TRIGGERS, TriggerEngine
from .audio import AudioBridge, list_output_devices
from .history import History
from . import accessibility

log = logging.getLogger(__name__)

USER_CONFIG = Path.home() / ".config" / "swlink" / "swlink.toml"

MOODS = ["sleep", "idle", "thinking", "working", "waiting", "question", "error", "celebrate", "listening", "dizzy"]

# Key picker groups for the dashboard. Names are pynput Key attributes or single characters.
KEY_NAMES: dict[str, list[str]] = {
    "Letters": [chr(c) for c in range(ord("a"), ord("z") + 1)],
    "Digits": [str(d) for d in range(10)],
    "Editing": ["space", "enter", "tab", "backspace", "delete", "esc"],
    "Navigation": ["up", "down", "left", "right", "home", "end", "page_up", "page_down"],
    "Function": [f"f{i}" for i in range(1, 21)],
    "Media": ["media_play_pause", "media_next", "media_previous", "media_volume_up",
              "media_volume_down", "media_volume_mute"],
    "Modifiers": ["cmd", "cmd_l", "cmd_r", "ctrl", "ctrl_l", "ctrl_r",
                  "alt", "alt_l", "alt_r", "shift", "shift_l", "shift_r"],
    "Symbols": list("-=[]\\;',./`"),
}


def _toml_str(v: str) -> str:
    return json.dumps(v)  # JSON string escapes are valid TOML basic-string escapes


def dump_config(cfg: dict[str, Any]) -> str:
    """Minimal TOML writer for our flat config: [device], [daemon], [keys]."""
    out = ["# swlink configuration (written by the swlink daemon)", ""]
    for section in ("device", "daemon"):
        vals = cfg.get(section) or {}
        out.append(f"[{section}]")
        for k, v in vals.items():
            if isinstance(v, bool): out.append(f"{k} = {'true' if v else 'false'}")
            elif isinstance(v, (int, float)): out.append(f"{k} = {v}")
            elif v is not None: out.append(f"{k} = {_toml_str(str(v))}")
        out.append("")
    out.append("[keys]")
    for k, v in (cfg.get("keys") or {}).items():
        out.append(f"{_toml_str(k)} = {_toml_str(v)}")
    out.append("")
    if cfg.get("moods"):
        out.append("[moods]")
        for k, v in cfg["moods"].items():
            out.append(f"{_toml_str(k)} = {_toml_str(v)}")
        out.append("")
    return "\n".join(out)


class Hub:
    def __init__(self, cfg: dict[str, Any], key_backend: KeyBackend | None, config_path: Path | None = None):
        self.cfg = cfg
        self.config_path = config_path          # where _keys.set persists; None = do not persist
        d = cfg.get("daemon", {})
        self.host = d.get("host", "127.0.0.1")
        self.port = int(d.get("port", 9898))
        self.imu_hz = int(d.get("imu_hz", 20))
        self.sync_rtc = bool(d.get("sync_rtc", True))
        # v0.1 configs bound to "<btn>.click"; triggers now use the decided ".single".
        keys = cfg.get("keys") or {}
        migrated = {(k[:-6] + ".single" if k.endswith(".click") else k): v for k, v in keys.items()}
        if migrated != keys:
            log.info("migrated %d '.click' mappings to '.single'", sum(k.endswith(".click") for k in keys))
            cfg["keys"] = migrated
        self.mapper = KeyMapper.from_config(cfg, key_backend)
        self.mapper.key_repeat = bool(d.get("key_repeat", True))
        if key_backend is not None and hasattr(key_backend, "on_event"):
            key_backend.on_event = self._on_key_event
        self.mapper.on_fire = lambda ev, a: log.info("key %s -> %s %s", ev, a.verb, "+".join(a.keys))
        self.triggers = TriggerEngine()
        self.audio = AudioBridge(d.get("virtual_mic"))
        self.audio_autostart = bool(d.get("audio_autostart", False))
        self.link = SerialLink(cfg.get("device", {}).get("port"), self._on_device)
        self.transport = "usb"                 # usb | tcp | ble
        self.ble_enabled = bool(d.get("ble", True))
        self.ble_address = cfg.get("device", {}).get("ble_address")
        self.wifi_timeout = float(d.get("wifi_discovery_timeout", 1.5))
        self.device_hid = False                # device executes button mappings itself over BLE HID
        self.clients: set[ServerConnection] = set()
        self.loop: asyncio.AbstractEventLoop | None = None
        self.last: dict[str, dict] = {}       # latest message per type (hello, power, imu ...)
        self.stats = {"rx": 0, "tx": 0, "since": time.time()}
        self.history = History()
        self.device_mac = str(cfg.get("device", {}).get("mac", ""))
        self.low_battery = int(d.get("low_battery", 20))
        self._low_notified = False
        self.screen_on: bool | None = None

    # -- device side (reader thread) -------------------------------------------
    def _on_device(self, msg: Message) -> None:
        self.stats["rx"] += 1
        if msg.t == "_bad":
            log.warning("bad frame from device: %s", msg.obj.get("raw"))
            return
        if msg.t == "_link":                        # transport lost: never leave a key stuck down
            self.mapper.release_all()
        if msg.t == "hello":
            self.device_mac = str(msg.obj.get("mac", self.device_mac))
            if self.device_mac:
                self.cfg.setdefault("device", {})["mac"] = self.device_mac
            self.history.event(self.device_mac or None, "connected", self.transport)
        if msg.t == "screen":
            self.screen_on = bool(msg.obj.get("on"))
        if msg.t == "power":
            self._record_power(msg.obj)
        if msg.t == "pair":
            token = str(msg.obj.get("token", ""))
            mac = str(self.last.get("hello", {}).get("mac", ""))
            if len(token) == 32:
                dev = self.cfg.setdefault("device", {})
                dev["token"] = token
                if mac:
                    dev["mac"] = mac
                self._persist_config()
                log.info("stored Wi-Fi pairing token for %s", mac or "device")
        if msg.t == "mic" and self.audio.running:
            data = msg.data
            if msg.obj.get("fmt") == "ima-adpcm":
                from .adpcm import decode_block
                data = decode_block(data, int(msg.obj.get("samples", 0)) or None)
            self.audio.push(data, int(msg.obj.get("rate", 16000)))
        if msg.t not in ("mic", "imu"):
            self.last[msg.t] = msg.obj
        if msg.t == "hello":
            log.info("device %s caps=%s hid=%s", msg.obj.get("fw"), ",".join(msg.obj.get("caps", [])), msg.obj.get("hid"))
            self.device_hid = bool(msg.obj.get("hid"))
            self._configure_device()
            if "keys" in msg.obj.get("caps", []):
                self.link.send({"t": "keys.set", "keys": dict(self.cfg.get("keys") or {}),
                                "moods": dict(self.cfg.get("moods") or {})})
        elif msg.t == "keys":
            self.device_hid = bool(msg.obj.get("hid"))
        elif msg.t == "ble":
            self.last["ble"] = msg.obj
            if self.audio_autostart and not self.audio.running:
                try:
                    self.audio.start(); self.link.send({"t": "mic.start", "rate": 16000})
                except Exception as e:
                    log.warning("audio autostart failed: %s", e)
        for name in self.triggers.feed(msg.obj):
            # Button triggers are executed by the watch itself when it is a connected BLE keyboard
            # (real HID events); the host only injects IMU gestures then.
            if self.device_hid and not name.startswith("imu."):
                action = self.mapper.actions.get(name)
            else:
                action = self.mapper.trigger(name)
            trig = Message({"t": "trigger", "name": name, "ms": msg.obj.get("ms"),
                            "action": f"{action.verb}:{'+'.join(action.keys)}" if action else None})
            if name.startswith("imu.") and name in (self.cfg.get("moods") or {}):
                try:                                # the watch applies the mood bound to this host-side trigger
                    self.link.send({"t": "trigger", "name": name})
                except Exception as e:
                    log.warning("could not forward trigger mood: %s", e)
            if self.loop:
                self.loop.call_soon_threadsafe(self._schedule_broadcast, trig)
        if self.loop:
            self.loop.call_soon_threadsafe(self._schedule_broadcast, msg)

    def _record_power(self, msg: dict) -> None:
        if not self.device_mac:
            return
        self.history.record_power(self.device_mac, msg, self.transport, self.screen_on)
        bat = msg.get("bat")
        charging = bool(msg.get("chg"))
        if charging or (isinstance(bat, int) and bat > self.low_battery):
            self._low_notified = False
        elif isinstance(bat, int) and bat <= self.low_battery and not self._low_notified:
            self._low_notified = True
            self.history.event(self.device_mac, "low_battery", f"{bat}%")
            self._notify_low_battery(bat)

    def _notify_low_battery(self, bat: int) -> None:
        """Best-effort local macOS notification; failure must not affect the link."""
        try:
            import subprocess
            name = self.last.get("hello", {}).get("name", "StopWatch")
            subprocess.Popen(["osascript", "-e", f'display notification "{bat}% remaining" with title "{name} battery low"'])
            log.warning("low battery: %d%%", bat)
        except Exception as e:
            log.warning("could not send low-battery notification: %s", e)

    def _on_key_event(self, kind: str, key: str, vk: int | None) -> None:
        """Every synthesized key event: log it and show it in the dashboard."""
        if kind != "repeat":
            log.info("keyevent %-7s %-12s vk=%s", kind, key, vk)
        if self.loop:
            m = Message({"t": "keyevent", "ev": kind, "key": key, "vk": vk, "ts": time.time()})
            self.loop.call_soon_threadsafe(self._schedule_broadcast, m)

    def _configure_device(self) -> None:
        self.link.send({"t": "imu.rate", "hz": self.imu_hz})
        if self.sync_rtc:
            self.link.send({"t": "rtc.set", "iso": datetime.now().strftime("%Y-%m-%dT%H:%M:%S")})

    def _persist_config(self) -> None:
        if self.config_path is None:
            return
        self.config_path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.config_path.with_suffix(".tmp")
        tmp.write_text(dump_config(self.cfg))
        tmp.chmod(0o600)
        tmp.replace(self.config_path)
        self.config_path.chmod(0o600)

    def set_keys(self, keys: dict[str, str], moods: dict[str, str] | None = None) -> str | None:
        """Replace the key mapping (and optionally the mood table), persist; returns an error or None."""
        if not isinstance(keys, dict):
            return "keys must be an object"
        try:
            actions = {str(k): parse_action(str(v)) for k, v in keys.items() if str(v).strip()}
        except ValueError as e:
            return str(e)
        if moods is not None:
            if not isinstance(moods, dict):
                return "moods must be an object"
            for k, v in moods.items():
                base = str(v).split(":", 1)[0]
                if base not in MOODS:
                    return f"unknown mood {v!r}"
        self.mapper.actions = actions
        self.cfg["keys"] = {str(k): str(v) for k, v in keys.items()}
        if moods is not None:
            self.cfg["moods"] = {str(k): str(v) for k, v in moods.items()}
        try:
            if self.link.connected.is_set():
                self.link.send({"t": "keys.set", "keys": self.cfg["keys"], "moods": dict(self.cfg.get("moods") or {})})
        except Exception as e:
            log.warning("could not push keys to device: %s", e)
        if self.config_path is not None:
            self._persist_config()
            log.info("saved %d key mappings to %s", len(actions), self.config_path)
        return None

    def _status_obj(self) -> dict[str, Any]:
        return {"t": "_status", "port": self.link.port, "connected": self.link.connected.is_set(),
                "clients": len(self.clients), "config": str(self.config_path) if self.config_path else None,
                "keys": dict(self.cfg.get("keys") or {}), "moods": dict(self.cfg.get("moods") or {}),
                "mood_names": MOODS, "triggers": ALL_TRIGGERS,
                "audio": self.audio.status(), "audio_devices": self._audio_devices(),
                "accessibility": accessibility.is_trusted(), "keys_enabled": self.mapper.backend is not None,
                "transport": self.transport, "device_hid": self.device_hid, "ble": self.last.get("ble"),
                "key_names": KEY_NAMES, "history": str(self.history.path), "low_battery": self.low_battery,
                "screen_on": self.screen_on, **self.stats}

    def _audio_devices(self) -> list[dict]:
        try:
            return list_output_devices(refresh=not self.audio.running)
        except Exception as e:  # PortAudio missing or broken
            log.warning("audio devices unavailable: %s", e)
            return []

    # -- websocket side ---------------------------------------------------------
    def _schedule_broadcast(self, msg: Message) -> None:
        assert self.loop
        self.loop.create_task(self._broadcast(msg))

    async def _broadcast(self, msg: Message) -> None:
        if not self.clients:
            return
        text = json.dumps(msg.obj, separators=(",", ":"))
        dead = []
        for ws in list(self.clients):
            try:
                await ws.send(text)
                if msg.data:
                    await ws.send(msg.data)
            except websockets.ConnectionClosed:
                dead.append(ws)
        for ws in dead:
            self.clients.discard(ws)

    async def _client(self, ws: ServerConnection) -> None:
        self.clients.add(ws)
        log.info("client connected (%d)", len(self.clients))
        try:
            # give a new client the current picture
            for t in ("hello", "power", "rtc"):
                if t in self.last:
                    await ws.send(json.dumps(self.last[t], separators=(",", ":")))
            pending_chunk: dict | None = None
            async for frame in ws:
                if isinstance(frame, bytes):
                    if pending_chunk is not None:
                        self.link.send(pending_chunk, frame)
                        self.stats["tx"] += 1
                        pending_chunk = None
                    continue
                try:
                    obj = json.loads(frame)
                except ValueError:
                    await ws.send(json.dumps({"t": "err", "of": "?", "msg": "client sent invalid JSON"}))
                    continue
                if not isinstance(obj, dict) or "t" not in obj:
                    await ws.send(json.dumps({"t": "err", "of": "?", "msg": "missing t"}))
                    continue
                if obj["t"] == "_status":
                    await ws.send(json.dumps(self._status_obj()))
                    continue
                if obj["t"] == "_history":
                    if not self.device_mac:
                        await ws.send(json.dumps({"t": "err", "of": "_history", "msg": "device identity not known yet"}))
                    else:
                        data = self.history.query(self.device_mac, int(obj.get("hours", 24)))
                        await ws.send(json.dumps({"t": "_history", **data}))
                    continue
                if obj["t"] == "_network.pair":
                    try:
                        if self.transport != "usb":
                            raise RuntimeError("connect by USB to create or rotate the Wi-Fi pairing token")
                        self.link.send({"t": "net.pair"})
                        await ws.send(json.dumps({"t": "ack", "of": "_network.pair", "msg": "token requested; saved when the watch replies"}))
                    except Exception as e:
                        await ws.send(json.dumps({"t": "err", "of": "_network.pair", "msg": str(e)}))
                    continue
                if obj["t"] == "_ble.pair":         # open the macOS Bluetooth pane so the user can click Connect
                    import subprocess
                    subprocess.Popen(["open", "x-apple.systempreferences:com.apple.BluetoothSettings"])
                    await ws.send(json.dumps({"t": "ack", "of": "_ble.pair", "msg": "Bluetooth settings opened; connect the watch there"}))
                    continue
                if obj["t"] == "_audio.start":
                    try:
                        if obj.get("device"): self.audio.device_hint = obj["device"]
                        st = self.audio.start()
                        self.link.send({"t": "mic.start", "rate": int(obj.get("rate", 16000))})
                        await ws.send(json.dumps({"t": "ack", "of": "_audio.start", "msg": f"virtual mic -> {st['device']}"}))
                    except Exception as e:
                        await ws.send(json.dumps({"t": "err", "of": "_audio.start", "msg": str(e)}))
                    continue
                if obj["t"] == "_audio.stop":
                    self.audio.stop()
                    self.link.send({"t": "mic.stop"})
                    await ws.send(json.dumps({"t": "ack", "of": "_audio.stop"}))
                    continue
                if obj["t"] == "_audio.status":
                    await ws.send(json.dumps({"t": "_audio", **self.audio.status()}))
                    continue
                if obj["t"] == "_action.test":       # fire an action from the UI without the device
                    try:
                        action = parse_action(str(obj.get("action", "")))
                        if self.mapper.backend is None:
                            raise RuntimeError("daemon started with --no-keys")
                        if accessibility.is_trusted(prompt=True) is False:
                            raise RuntimeError(accessibility.HOWTO)
                        self.mapper.fire(action)
                        await ws.send(json.dumps({"t": "ack", "of": "_action.test", "msg": f"fired {action.verb}:{'+'.join(action.keys)}"}))
                    except Exception as e:  # bad spec or key injection failure
                        await ws.send(json.dumps({"t": "err", "of": "_action.test", "msg": str(e)}))
                    continue
                if obj["t"] == "_keys.set":
                    err = self.set_keys(obj.get("keys"), obj.get("moods"))
                    if err:
                        await ws.send(json.dumps({"t": "err", "of": "_keys.set", "msg": err}))
                    else:
                        status = json.dumps(self._status_obj())
                        for c in list(self.clients):       # every dashboard sees the new mapping
                            try: await c.send(status)
                            except websockets.ConnectionClosed: pass
                        await ws.send(json.dumps({"t": "ack", "of": "_keys.set", "msg": f"{len(self.mapper.actions)} mappings saved"}))
                    continue
                if obj["t"] == "spk.pcm":      # binary payload arrives in the next frame
                    pending_chunk = obj
                    continue
                self.link.send(obj)
                self.stats["tx"] += 1
        except websockets.ConnectionClosed:
            pass
        finally:
            self.clients.discard(ws)
            log.info("client disconnected (%d)", len(self.clients))

    # Plain HTTP GET on the WebSocket port serves the dashboard; upgrades pass through.
    def _http(self, connection: ServerConnection, request):
        if "Upgrade" in request.headers.get("Connection", "") or request.headers.get("Upgrade"):
            return None
        path = request.path.split("?", 1)[0]
        name = "index.html" if path in ("/", "/index.html") else path.lstrip("/")
        types = {".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8",
                 ".css": "text/css; charset=utf-8"}
        target = (UI_DIR / name).resolve()
        # Serve only files that really live in the bundled UI directory.
        if target.suffix in types and target.is_file() and UI_DIR.resolve() in target.parents:
            body = target.read_bytes()
            return Response(200, "OK", Headers([("Content-Type", types[target.suffix]),
                                                ("Content-Length", str(len(body))),
                                                ("Cache-Control", "no-store")]), body)
        return Response(404, "Not Found", Headers([("Content-Length", "0")]), b"")

    def _open_any(self) -> str | None:
        """Transport priority: USB -> paired Wi-Fi TCP -> paired BLE."""
        forced = self.cfg.get("device", {}).get("port")
        if forced or find_device_ports():
            try:
                self.link = SerialLink(forced, self._on_device)
                self.transport = "usb"
                return self.link.open()
            except Exception as e:
                log.warning("usb open failed: %s", e)
        target = choose_wifi_target(self.cfg.get("device", {}), self.wifi_timeout)
        if target:
            try:
                self.link = TcpLink(target["host"], target["port"], self.cfg["device"]["token"], self._on_device)
                self.transport = "tcp"
                return self.link.open()
            except Exception as e:
                log.warning("tcp open failed for %s: %s", target["host"], e)
        if self.ble_enabled:
            try:
                self.link = BleLink(self.ble_address, None, self._on_device)
                self.transport = "ble"
                return self.link.open()
            except Exception as e:
                log.warning("ble open failed: %s", e)
        return None

    async def run(self) -> None:
        self.loop = asyncio.get_running_loop()
        async with serve(self._client, self.host, self.port, process_request=self._http):
            log.info("daemon on ws://%s:%d, dashboard http://%s:%d/, %d key mappings",
                     self.host, self.port, self.host, self.port, len(self.mapper.actions))
            if self.mapper.backend is not None and accessibility.is_trusted() is False:
                log.warning(accessibility.HOWTO)
            try:
                while True:
                    port = await asyncio.to_thread(self._open_any)
                    if port is None:
                        await asyncio.sleep(3)
                        continue
                    log.info("device link up via %s (%s)", self.transport, port)
                    self.link.send({"t": "hello"})
                    while self.link.connected.is_set():
                        await asyncio.sleep(1)
                    log.warning("device link lost (%s); retrying", self.transport)
                    self.history.event(self.device_mac or None, "disconnected", self.transport)
                    self.mapper.release_all()
                    self.device_hid = False
                    await asyncio.to_thread(self.link.close)
                    await asyncio.sleep(2)
            finally:
                self.mapper.release_all()
                self.link.close()
