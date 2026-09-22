"""swlink wire protocol: JSON lines, plus raw byte chunks announced by a header.

Device -> host and host -> device use the same framing. A "chunk" type is a
message whose header carries ``n``; exactly ``n`` raw bytes follow the newline.
See docs/design-docs/swlink-architecture.md for the message table.
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from typing import Any

# Message types whose header is followed by ``n`` raw bytes.
CHUNK_TYPES = frozenset({"mic", "spk.pcm"})

MAX_LINE = 512


@dataclass
class Message:
    """One decoded frame. ``data`` is empty for plain JSON messages."""

    obj: dict[str, Any]
    data: bytes = b""

    @property
    def t(self) -> str:
        return str(self.obj.get("t", ""))


def encode(obj: dict[str, Any], data: bytes = b"") -> bytes:
    """Encode a message. For chunk types ``n`` is set from ``data``."""
    if obj.get("t") in CHUNK_TYPES:
        obj = {**obj, "n": len(data)}
    line = json.dumps(obj, separators=(",", ":")).encode() + b"\n"
    if len(line) > MAX_LINE:
        raise ValueError(f"line too long ({len(line)} > {MAX_LINE})")
    return line + data


@dataclass
class Decoder:
    """Incremental parser. Feed bytes, pull complete messages.

    Invalid JSON lines are reported as ``Message({"t": "_bad", "raw": ...})``
    so callers can log them without the stream desynchronising.
    """

    _buf: bytearray = field(default_factory=bytearray)
    _pending: dict[str, Any] | None = None  # chunk header awaiting its bytes

    def feed(self, data: bytes) -> list[Message]:
        self._buf.extend(data)
        out: list[Message] = []
        while True:
            if self._pending is not None:
                n = int(self._pending.get("n", 0))
                if len(self._buf) < n:
                    break
                out.append(Message(self._pending, bytes(self._buf[:n])))
                del self._buf[:n]
                self._pending = None
                continue
            nl = self._buf.find(b"\n")
            if nl < 0:
                if len(self._buf) > MAX_LINE * 4:  # runaway garbage, resync
                    self._buf.clear()
                break
            line = bytes(self._buf[:nl]).strip(b"\r")
            del self._buf[: nl + 1]
            if not line:
                continue
            try:
                obj = json.loads(line)
            except ValueError:
                out.append(Message({"t": "_bad", "raw": line.decode("utf-8", "replace")}))
                continue
            if not isinstance(obj, dict) or "t" not in obj:
                out.append(Message({"t": "_bad", "raw": line.decode("utf-8", "replace")}))
                continue
            if obj["t"] in CHUNK_TYPES and int(obj.get("n", 0)) > 0:
                self._pending = obj
                continue
            out.append(Message(obj))
        return out
