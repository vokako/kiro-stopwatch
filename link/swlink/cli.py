"""swlink command line.

  swlink ports                      list candidate serial ports
  swlink monitor [--port P]         print every device message (direct serial)
  swlink send '{"t":"tone"}' ...    send commands directly and print replies
  swlink mic out.wav --seconds 3    record from the device mic to a WAV file
  swlink daemon [--config F] [--open]  run the hub: key mapping, WebSocket API, dashboard at http://127.0.0.1:9898/

``monitor``, ``send`` and ``mic`` open the serial port themselves; do not run
them while the daemon holds it. To talk to a running daemon use ``swlink ws``.
"""
from __future__ import annotations

import argparse
import asyncio
import json
import logging
import sys
import time
import tomllib
import wave
from pathlib import Path

from .protocol import Message
from .transport import SerialLink, find_ports

DEFAULT_CONFIG = Path(__file__).with_name("swlink.example.toml")


def resolve_config(path: str | None) -> tuple[Path, Path]:
    """(file to read, file to persist to). Without --config: the user config if it
    exists, else the bundled example is read and saves go to the user config."""
    from .daemon import USER_CONFIG

    if path:
        p = Path(path).expanduser()
        return p, p
    if USER_CONFIG.exists():
        return USER_CONFIG, USER_CONFIG
    return DEFAULT_CONFIG, USER_CONFIG


def load_config(path: str | None) -> dict:
    p, _ = resolve_config(path)
    with p.open("rb") as f:
        return tomllib.load(f)


def _print(msg: Message) -> None:
    if msg.data:
        print(f"{json.dumps(msg.obj)}  +{len(msg.data)} bytes")
    else:
        print(json.dumps(msg.obj))


def cmd_ports(_: argparse.Namespace) -> int:
    for p in find_ports():
        print(p)
    return 0


def cmd_monitor(a: argparse.Namespace) -> int:
    link = SerialLink(a.port, _print)
    print("port:", link.open(), file=sys.stderr)
    link.send({"t": "hello"})
    try:
        while link.connected.is_set():
            time.sleep(0.2)
    except KeyboardInterrupt:
        pass
    finally:
        link.close()
    return 0


def cmd_send(a: argparse.Namespace) -> int:
    link = SerialLink(a.port, _print)
    link.open()
    time.sleep(0.2)
    for raw in a.json:
        link.send(json.loads(raw))
    time.sleep(a.wait)
    link.close()
    return 0


def cmd_mic(a: argparse.Namespace) -> int:
    frames: list[bytes] = []
    rate = {"v": a.rate}

    def on(msg: Message) -> None:
        if msg.t == "mic":
            data = msg.data
            if msg.obj.get("fmt") == "ima-adpcm":
                from .adpcm import decode_block
                data = decode_block(data, int(msg.obj.get("samples", 0)) or None)
            frames.append(data)
            rate["v"] = int(msg.obj.get("rate", a.rate))
        elif msg.t in ("ack", "err"):
            print(json.dumps(msg.obj), file=sys.stderr)

    link = SerialLink(a.port, on)
    link.open()
    time.sleep(0.2)
    link.send({"t": "mic.start", "rate": a.rate})
    time.sleep(a.seconds)
    link.send({"t": "mic.stop"})
    time.sleep(0.3)
    link.close()
    pcm = b"".join(frames)
    with wave.open(a.out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate["v"])
        w.writeframes(pcm)
    print(f"wrote {a.out}: {len(pcm)//2} samples, {len(pcm)/2/rate['v']:.2f} s at {rate['v']} Hz")
    return 0


def cmd_daemon(a: argparse.Namespace) -> int:
    from .daemon import Hub

    read_from, save_to = resolve_config(a.config)
    cfg = load_config(a.config)
    logging.getLogger(__name__).info("config: %s (saves go to %s)", read_from, save_to)
    backend = None
    if not a.no_keys:
        from .keymap import PynputBackend

        backend = PynputBackend()
    hub = Hub(cfg, backend, save_to)
    if a.port:
        hub.link.port = a.port
    if a.open:
        import threading
        import webbrowser

        threading.Timer(1.0, webbrowser.open, [f"http://{hub.host}:{hub.port}/"]).start()
    try:
        asyncio.run(hub.run())
    except KeyboardInterrupt:
        pass
    return 0


def cmd_ws(a: argparse.Namespace) -> int:
    """Talk to a running daemon: send JSON frames, print what comes back."""
    import websockets

    async def go() -> None:
        async with websockets.connect(a.url) as ws:
            for raw in a.json:
                await ws.send(raw)
            end = asyncio.get_running_loop().time() + a.wait
            while True:
                left = end - asyncio.get_running_loop().time()
                if left <= 0:
                    break
                try:
                    frame = await asyncio.wait_for(ws.recv(), timeout=left)
                except asyncio.TimeoutError:
                    break
                print(frame if isinstance(frame, str) else f"<{len(frame)} bytes>")

    asyncio.run(go())
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="swlink", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-v", "--verbose", action="store_true")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("ports").set_defaults(fn=cmd_ports)

    p = sub.add_parser("monitor"); p.add_argument("--port"); p.set_defaults(fn=cmd_monitor)

    p = sub.add_parser("send"); p.add_argument("json", nargs="+"); p.add_argument("--port")
    p.add_argument("--wait", type=float, default=1.0); p.set_defaults(fn=cmd_send)

    p = sub.add_parser("mic"); p.add_argument("out"); p.add_argument("--seconds", type=float, default=3.0)
    p.add_argument("--rate", type=int, default=16000); p.add_argument("--port"); p.set_defaults(fn=cmd_mic)

    p = sub.add_parser("daemon"); p.add_argument("--config"); p.add_argument("--port")
    p.add_argument("--no-keys", action="store_true", help="do not inject keys (no Accessibility needed)")
    p.add_argument("--open", action="store_true", help="open the dashboard in the default browser")
    p.set_defaults(fn=cmd_daemon)

    p = sub.add_parser("ws"); p.add_argument("json", nargs="*"); p.add_argument("--url", default="ws://127.0.0.1:9898")
    p.add_argument("--wait", type=float, default=2.0); p.set_defaults(fn=cmd_ws)

    a = ap.parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if a.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s", datefmt="%H:%M:%S")
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
