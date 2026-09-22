"""Local device-management history (SQLite, no cloud/network dependency).

One database is shared by all watches but keyed by immutable hardware MAC. Power
samples are downsampled to one per minute; connection and configuration events
are retained as an audit trail. The dashboard can ask `_history` for a compact
range rather than streaming the whole database.
"""
from __future__ import annotations

import sqlite3
import threading
import time
from pathlib import Path

DEFAULT_PATH = Path.home() / ".config" / "swlink" / "history.db"


class History:
    def __init__(self, path: Path = DEFAULT_PATH):
        self.path = path
        self._lock = threading.Lock()
        path.parent.mkdir(parents=True, exist_ok=True)
        self._db = sqlite3.connect(path, check_same_thread=False)
        self._db.execute("PRAGMA journal_mode=WAL")
        self._db.execute("PRAGMA busy_timeout=2000")
        self._db.executescript("""
            CREATE TABLE IF NOT EXISTS power_samples (
              mac TEXT NOT NULL, ts INTEGER NOT NULL, battery INTEGER, mv INTEGER,
              charging INTEGER NOT NULL, transport TEXT, screen_on INTEGER,
              PRIMARY KEY(mac, ts)
            );
            CREATE INDEX IF NOT EXISTS power_samples_mac_ts ON power_samples(mac, ts);
            CREATE TABLE IF NOT EXISTS events (
              mac TEXT, ts INTEGER NOT NULL, kind TEXT NOT NULL, detail TEXT
            );
            CREATE INDEX IF NOT EXISTS events_mac_ts ON events(mac, ts);
        """)
        self._db.commit()
        self._last_power: dict[str, int] = {}

    def record_power(self, mac: str, msg: dict, transport: str, screen_on: bool | None = None) -> None:
        """Store no more than one unchanged power sample per 60 seconds per device."""
        now = int(time.time())
        if now - self._last_power.get(mac, 0) < 60:
            return
        self._last_power[mac] = now
        with self._lock:
            self._db.execute(
                "INSERT OR REPLACE INTO power_samples VALUES(?,?,?,?,?,?,?)",
                (mac, now, msg.get("bat"), msg.get("mv"), int(bool(msg.get("chg"))),
                 transport, None if screen_on is None else int(screen_on)),
            )
            self._db.commit()

    def event(self, mac: str | None, kind: str, detail: str = "") -> None:
        with self._lock:
            self._db.execute("INSERT INTO events VALUES(?,?,?,?)", (mac, int(time.time()), kind, detail))
            self._db.commit()

    def query(self, mac: str, hours: int = 24, limit: int = 720) -> dict:
        hours = max(1, min(int(hours), 24 * 30))
        since = int(time.time()) - hours * 3600
        with self._lock:
            samples = self._db.execute(
                "SELECT ts,battery,mv,charging,transport,screen_on FROM power_samples "
                "WHERE mac=? AND ts>=? ORDER BY ts DESC LIMIT ?", (mac, since, limit)
            ).fetchall()
            events = self._db.execute(
                "SELECT ts,kind,detail FROM events WHERE mac=? AND ts>=? ORDER BY ts DESC LIMIT 100",
                (mac, since),
            ).fetchall()
        return {"hours": hours, "samples": [list(x) for x in reversed(samples)],
                "events": [list(x) for x in reversed(events)]}

    def close(self) -> None:
        with self._lock:
            self._db.close()
