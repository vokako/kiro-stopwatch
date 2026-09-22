"""Turn raw device messages into named triggers that mappings bind to.

Button triggers come straight from the firmware's decided events::

    A.down A.up A.single A.double A.triple A.hold      (same for B)
    PWR.single PWR.double PWR.hold                     (PMIC key: no down/up)

IMU triggers are computed here from the accel/gyro stream::

    imu.shake                      sharp back-and-forth motion (gyro energy burst)
    imu.tilt_left / imu.tilt_right   left / right edge dipped past ~35° from flat
    imu.tilt_toward / imu.tilt_away  screen raised to face you / tipped away from you
    imu.face_down / imu.face_up      turned over (fires on the transition)
    imu.tap                          a short knock on the body (accel spike, little rotation)

Axis orientation, measured on the device 2026-09-14 (gravity component in g):
    ay = +1  left edge down        ay = -1  right edge down
    ax = -1  upright, screen facing you (bottom edge down)     ax = +1  top edge down
    az = +1  screen up (flat)      az = -1  screen down

Thresholds are conservative so idle noise (|g| ~0.02) never fires anything.
"""
from __future__ import annotations

import math
import time
from collections.abc import Callable
from dataclasses import dataclass, field

BUTTON_EVENTS = ("down", "up", "single", "double", "triple", "hold")
BUTTON_TRIGGERS = [f"{b}.{e}" for b in ("A", "B") for e in BUTTON_EVENTS] + \
                  [f"PWR.{e}" for e in ("single", "double", "hold")]
IMU_TRIGGERS = ["imu.shake", "imu.tap", "imu.tilt_left", "imu.tilt_right",
                "imu.tilt_toward", "imu.tilt_away", "imu.face_down", "imu.face_up"]
ALL_TRIGGERS = BUTTON_TRIGGERS + IMU_TRIGGERS


@dataclass
class ImuGestures:
    """Stateful gesture detector over imu messages. Units: g and deg/s."""

    shake_gyro_dps: float = 250.0     # |gyro| above this counts as a swing
    shake_swings: int = 3             # swings within the window => shake
    shake_window_s: float = 0.8
    tap_g: float = 0.9                # |accel - 1g| jump above this with calm gyro => tap
    tap_gyro_dps: float = 120.0
    tilt_on: float = 0.55             # component of gravity (sin 33°) to enter a tilt
    tilt_off: float = 0.35            # ... and to leave it (hysteresis)
    cooldown_s: float = 0.6           # min spacing between IMU triggers of the same name

    _swings: list[float] = field(default_factory=list)
    _last_gyro_sign: int = 0
    _tilt: str | None = None
    _face_down: bool | None = None
    _last_fire: dict[str, float] = field(default_factory=dict)
    _now: Callable[[], float] = time.monotonic

    def feed(self, m: dict) -> list[str]:
        ax, ay, az = float(m.get("ax", 0)), float(m.get("ay", 0)), float(m.get("az", 1))
        gx, gy, gz = float(m.get("gx", 0)), float(m.get("gy", 0)), float(m.get("gz", 0))
        now = self._now()
        out: list[str] = []

        # shake: count gyro swings that change direction
        gmag = math.sqrt(gx * gx + gy * gy + gz * gz)
        if gmag > self.shake_gyro_dps:
            dominant = max((gx, gy, gz), key=abs)
            sign = 1 if dominant > 0 else -1
            if sign != self._last_gyro_sign:
                self._swings.append(now)
                self._last_gyro_sign = sign
        self._swings = [t for t in self._swings if now - t <= self.shake_window_s]
        if len(self._swings) >= self.shake_swings:
            self._swings.clear()
            out += self._fire("imu.shake", now)

        # tap: acceleration spike without much rotation
        amag = math.sqrt(ax * ax + ay * ay + az * az)
        if abs(amag - 1.0) > self.tap_g and gmag < self.tap_gyro_dps:
            out += self._fire("imu.tap", now)

        # tilt: which way is gravity leaning (only when not being shaken)
        if gmag < self.shake_gyro_dps:
            tilt = None
            if ay > self.tilt_on: tilt = "imu.tilt_left"        # left edge down
            elif ay < -self.tilt_on: tilt = "imu.tilt_right"    # right edge down
            elif ax < -self.tilt_on: tilt = "imu.tilt_toward"   # upright, facing you
            elif ax > self.tilt_on: tilt = "imu.tilt_away"
            if self._tilt and abs(ax) < self.tilt_off and abs(ay) < self.tilt_off:
                self._tilt = None                       # back to flat: re-arm
            elif tilt and tilt != self._tilt:
                self._tilt = tilt
                out += self._fire(tilt, now)

        # face down / up transitions
        face_down = az < -0.6 if az < -0.6 or az > 0.6 else self._face_down
        if face_down is not None and face_down != self._face_down:
            if self._face_down is not None:            # skip the very first sample
                out += self._fire("imu.face_down" if face_down else "imu.face_up", now)
            self._face_down = face_down
        return out

    def _fire(self, name: str, now: float) -> list[str]:
        if now - self._last_fire.get(name, -1e9) < self.cooldown_s:
            return []
        self._last_fire[name] = now
        return [name]


class TriggerEngine:
    """Feed every device message; get back the trigger names it produced."""

    def __init__(self, imu: ImuGestures | None = None):
        self.imu = imu or ImuGestures()

    def feed(self, m: dict) -> list[str]:
        t = m.get("t")
        if t == "btn":
            name = f"{m.get('id')}.{m.get('ev')}"
            # 'click' is the immediate, count-agnostic event; mappings bind to the
            # decided single/double/triple instead, so it is not a trigger.
            return [name] if m.get("ev") != "click" else []
        if t == "imu":
            return self.imu.feed(m)
        return []
