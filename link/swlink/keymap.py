"""Map device button events to host keyboard actions.

Config (TOML) example::

    [keys]
    "A.single" = "space"         # tap space
    "A.double" = "cmd+shift+4"   # chord
    "A.hold"   = "media_next"
    "B.down"   = "press:right"   # hold while the button is held ...
    "B.up"     = "release:right" # ... release on button up
    "imu.shake" = "media_play_pause"   # IMU gestures, see triggers.py

Trigger names: see swlink.triggers.ALL_TRIGGERS.

Action grammar: ``[tap:|hold:|press:|release:]KEY[+KEY...]``. Default verb is ``tap``.
``hold:`` binds to ``<btn>.hold`` (preferred) or ``<btn>.down``: the keys are
pressed when the event arrives, auto-repeated like a physical key (500 ms delay,
then every 33 ms, modifiers excluded), and released when that button comes up.
Binding it to ``.hold`` keeps it clear of single/double-click arbitration, since
a held button never produces a click. ``press:`` / ``release:`` are manual.
Key names: single characters, or pynput names (space, enter, esc, tab, up, down,
left, right, f1..f20, cmd, ctrl, alt, shift, media_play_pause, ...).
"""
from __future__ import annotations

import logging
import threading
import time
from collections.abc import Callable
from dataclasses import dataclass

log = logging.getLogger(__name__)

ALIASES = {
    "cmd": "cmd", "command": "cmd", "meta": "cmd", "super": "cmd",
    "ctrl": "ctrl", "control": "ctrl",
    "alt": "alt", "option": "alt", "opt": "alt",
    "shift": "shift",
    "esc": "esc", "escape": "esc",
    "return": "enter", "enter": "enter",
    "del": "delete", "delete": "delete", "backspace": "backspace",
    "pgup": "page_up", "pgdn": "page_down",
    # left/right specific modifiers (pynput names: cmd_l, cmd_r, ...)
    "lcmd": "cmd_l", "rcmd": "cmd_r", "lctrl": "ctrl_l", "rctrl": "ctrl_r",
    "lalt": "alt_l", "ralt": "alt_r", "lshift": "shift_l", "rshift": "shift_r",
    "loption": "alt_l", "roption": "alt_r", "lcommand": "cmd_l", "rcommand": "cmd_r",
}


@dataclass(frozen=True)
class Action:
    verb: str           # tap | press | release
    keys: tuple[str, ...]


def parse_action(spec: str) -> Action:
    spec = spec.strip()
    verb = "tap"
    if ":" in spec:
        v, spec = spec.split(":", 1)
        v = v.strip().lower()
        if v not in ("tap", "hold", "press", "release"):
            raise ValueError(f"unknown verb {v!r}")
        verb = v
    keys = tuple(ALIASES.get(k.strip().lower(), k.strip()) for k in spec.split("+") if k.strip())
    if not keys:
        raise ValueError(f"empty key spec {spec!r}")
    return Action(verb, keys)


class KeyBackend:
    """Something that can press keys. Real one wraps pynput; tests inject a fake."""

    def press(self, key: str) -> None: ...
    def release(self, key: str) -> None: ...
    def repeat(self, key: str) -> None:
        """A key-down carrying the OS auto-repeat flag. Default: plain press."""
        self.press(key)


class PynputBackend(KeyBackend):
    """Real key injection. Every event goes through ``on_event(kind, key, vk)``
    (kind = press | release | repeat) so the daemon can log and broadcast it."""

    def __init__(self, on_event: Callable[[str, str, int | None], None] | None = None) -> None:
        import sys
        from pynput.keyboard import Controller, Key

        self._Key = Key
        self.on_event = on_event or (lambda kind, key, vk: None)
        if sys.platform == "darwin":
            self._kb = _DarwinRepeatController()
        else:
            self._kb = Controller()

    def _vk(self, resolved) -> int | None:
        try:
            k = resolved.value if hasattr(resolved, "value") else resolved
            if getattr(k, "vk", None) is not None:
                return int(k.vk)
            mapping = getattr(self._kb, "_mapping", None)
            if mapping and getattr(k, "char", None) is not None:
                return int(mapping.get(k.char)) if k.char in mapping else None
            if isinstance(k, str) and mapping and k in mapping:
                return int(mapping[k])
        except Exception:
            pass
        return None

    def press(self, key: str) -> None:
        r = self._resolve(key); self._kb.press(r); self.on_event("press", key, self._vk(r))

    def release(self, key: str) -> None:
        r = self._resolve(key); self._kb.release(r); self.on_event("release", key, self._vk(r))

    def repeat(self, key: str) -> None:
        r = self._resolve(key)
        if hasattr(self._kb, "press_repeat"):
            self._kb.press_repeat(r)
        else:
            self._kb.press(r)
        self.on_event("repeat", key, self._vk(r))

    def _resolve(self, name: str):
        if len(name) == 1:
            return name
        try:
            return getattr(self._Key, name)
        except AttributeError as e:
            raise ValueError(f"unknown key name {name!r}") from e



MODIFIER_KEYS = frozenset({"cmd", "cmd_l", "cmd_r", "ctrl", "ctrl_l", "ctrl_r",
                           "alt", "alt_l", "alt_r", "shift", "shift_l", "shift_r"})
REPEAT_DELAY_S = 0.5
TAP_DWELL_S = 0.08
MOD_STAGGER_S = 0.02      # modifiers go down this long before the main key, as fingers do
REPEAT_INTERVAL_S = 0.033


def _make_darwin_repeat_controller():
    """pynput's macOS Controller posts fresh key-downs; a physical keyboard held down
    produces key-downs flagged kCGKeyboardEventAutorepeat. Long-press detectors rely on
    that flag, so repeats are posted through this subclass with the flag set."""
    import sys
    if sys.platform != "darwin":
        return None
    from pynput.keyboard import Controller, Key
    from Quartz import (CGEventPost, CGEventSetIntegerValueField, CGEventSetSource, CGEventSourceCreate,
                        kCGHIDEventTap, kCGKeyboardEventAutorepeat, kCGEventSourceStateHIDSystemState)

    # Mark our events as coming from the HID system state, like a physical keyboard's.
    # The posting PID (kCGEventSourceUnixProcessID) is still stamped by the OS and cannot be hidden.
    HID_SOURCE = CGEventSourceCreate(kCGEventSourceStateHIDSystemState)

    class DarwinRepeatController(Controller):
        def _handle(self, key, is_press):
            with self.modifiers as modifiers:
                ev = (key if key not in (k for k in Key) else key.value)._event(modifiers, self._mapping, is_press)
                CGEventSetSource(ev, HID_SOURCE)
                CGEventPost(kCGHIDEventTap, ev)

        def press_repeat(self, key):
            resolved = self._KeyCode.from_char(key) if isinstance(key, str) else key
            with self.modifiers as modifiers:
                ev = (resolved if resolved not in (k for k in Key) else resolved.value)._event(
                    modifiers, self._mapping, True)
                CGEventSetIntegerValueField(ev, kCGKeyboardEventAutorepeat, 1)
                CGEventSetSource(ev, HID_SOURCE)
                CGEventPost(kCGHIDEventTap, ev)

    return DarwinRepeatController


_DarwinRepeatController = _make_darwin_repeat_controller()


def _press_chord(backend: KeyBackend, keys: tuple[str, ...]) -> None:
    """Press modifiers first, pause, then the main key(s)."""
    mods = [k for k in keys if k in MODIFIER_KEYS]
    rest = [k for k in keys if k not in MODIFIER_KEYS]
    for k in mods:
        backend.press(k)
    if mods and rest:
        time.sleep(MOD_STAGGER_S)
    for k in rest:
        backend.press(k)


class KeyMapper:
    """Applies ``{"A.single": "space", "A.down": "hold:left", ...}`` to triggers."""

    def __init__(self, mapping: dict[str, str], backend: KeyBackend | None = None,
                 on_fire: Callable[[str, Action], None] | None = None, key_repeat: bool = True):
        self.actions = {k: parse_action(v) for k, v in mapping.items()}
        self.backend = backend
        self.on_fire = on_fire or (lambda ev, a: None)
        self.key_repeat = key_repeat
        self._held: dict[str, Action] = {}            # button id -> action currently held
        self._repeat: dict[str, threading.Timer] = {}
        self._lock = threading.Lock()

    @classmethod
    def from_config(cls, cfg: dict, backend: KeyBackend | None = None) -> "KeyMapper":
        return cls(dict(cfg.get("keys", {})), backend)

    def handle(self, msg: dict) -> Action | None:
        """Legacy entry: a raw btn message. Prefer trigger()."""
        if msg.get("t") != "btn":
            return None
        return self.trigger(f"{msg.get('id')}.{msg.get('ev')}")

    def trigger(self, name: str) -> Action | None:
        """Fire the action bound to a trigger name (e.g. 'A.double', 'imu.shake')."""
        src, _, ev = name.rpartition(".")
        if ev == "up" and self._release_held(src):
            return None                               # hold action ended; nothing else bound to up
        action = self.actions.get(name)
        if action is None:
            return None
        if action.verb == "hold":
            if ev not in ("hold", "down"):
                log.warning("hold action on %s ignored: bind hold: to a .hold or .down trigger", name)
                return None
            self._begin_hold(src, action)
        else:
            self.fire(action)
        self.on_fire(name, action)
        return action

    # -- hold / auto-repeat -----------------------------------------------------
    def _begin_hold(self, src: str, action: Action) -> None:
        with self._lock:
            self._release_held_locked(src)
            self._held[src] = action
            if self.backend:
                _press_chord(self.backend, action.keys)
            if self.key_repeat and any(k not in MODIFIER_KEYS for k in action.keys):
                self._schedule_repeat(src, REPEAT_DELAY_S)

    def _schedule_repeat(self, src: str, delay: float) -> None:
        t = threading.Timer(delay, self._repeat_tick, [src])
        t.daemon = True
        self._repeat[src] = t
        t.start()

    def _repeat_tick(self, src: str) -> None:
        with self._lock:
            action = self._held.get(src)
            if action is None:
                return
            if self.backend:
                for k in action.keys:
                    if k not in MODIFIER_KEYS:
                        self.backend.repeat(k)     # key-down flagged as OS auto-repeat
            self._schedule_repeat(src, REPEAT_INTERVAL_S)

    def _release_held(self, src: str) -> bool:
        with self._lock:
            return self._release_held_locked(src)

    def _release_held_locked(self, src: str) -> bool:
        t = self._repeat.pop(src, None)
        if t:
            t.cancel()
        action = self._held.pop(src, None)
        if action is None:
            return False
        if self.backend:
            for k in reversed(action.keys):
                self.backend.release(k)
        return True

    def release_all(self) -> None:
        """Safety: release anything held (call on device disconnect / shutdown)."""
        with self._lock:
            for src in list(self._held):
                self._release_held_locked(src)

    def fire(self, action: Action) -> None:
        if self.backend is None:
            return
        if action.verb == "hold":                  # fired without a button (e.g. UI test): tap once
            _press_chord(self.backend, action.keys)
            time.sleep(TAP_DWELL_S)
            for k in reversed(action.keys):
                self.backend.release(k)
        elif action.verb == "press":
            for k in action.keys:
                self.backend.press(k)
        elif action.verb == "release":
            for k in reversed(action.keys):
                self.backend.release(k)
        else:  # tap: modifiers, stagger, main key; release in reverse
            _press_chord(self.backend, action.keys)
            # A physical tap lasts tens of ms; apps that act on "Option pressed alone" or
            # debounce input ignore down/up pairs shorter than that.
            time.sleep(TAP_DWELL_S)
            for k in reversed(action.keys):
                self.backend.release(k)
