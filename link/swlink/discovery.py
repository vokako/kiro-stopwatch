from __future__ import annotations

from .tcp import discover


def choose_wifi_target(device: dict, timeout: float = 0.5) -> dict | None:
    """Pick the paired watch advertised over mDNS, without raising on a quiet LAN."""
    token = device.get("token")
    if not token:
        return None
    mac = device.get("mac")
    hits = discover(timeout=timeout, mac=mac)
    return hits[0] if hits else None
