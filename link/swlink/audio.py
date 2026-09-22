"""Virtual microphone: play the watch's mic stream into a loopback audio device.

macOS apps cannot record from a serial port, but they can record from any
CoreAudio *input*. BlackHole (brew install --cask blackhole-2ch) is a loopback
device: whatever is written to its output appears on its input. So the daemon
writes the watch PCM to "BlackHole 2ch", and Zoom/QuickTime/anything select
"BlackHole 2ch" as their microphone.

Pipeline: s16le mono @16 kHz chunks -> linear resample to the device rate ->
duplicate to N channels -> jitter buffer (~120 ms target) -> PortAudio output
callback. Underruns play silence; a buffer beyond 400 ms drops the oldest audio
so latency never grows without bound.
"""
from __future__ import annotations

import logging
import threading
import time
from collections import deque

import numpy as np

log = logging.getLogger(__name__)

TARGET_BUFFER_MS = 120
MAX_BUFFER_MS = 400


_refresh_lock = threading.Lock()


def refresh_devices() -> None:
    """PortAudio snapshots the device list at init; re-init to see devices added since
    (e.g. BlackHole after `sudo killall coreaudiod`). Never call while a stream is open."""
    import sounddevice as sd

    with _refresh_lock:
        sd._terminate()
        sd._initialize()


def list_output_devices(refresh: bool = False) -> list[dict]:
    import sounddevice as sd

    if refresh:
        refresh_devices()
    out = []
    for i, d in enumerate(sd.query_devices()):
        if d["max_output_channels"] > 0:
            out.append({"index": i, "name": d["name"], "channels": d["max_output_channels"],
                        "rate": int(d["default_samplerate"]),
                        "loopback": "blackhole" in d["name"].lower() or "loopback" in d["name"].lower()})
    return out


def pick_device(hint: str | None) -> dict | None:
    devs = list_output_devices()
    if hint:
        for d in devs:
            if hint.lower() in d["name"].lower():
                return d
        return None
    for d in devs:
        if d["loopback"]:
            return d
    return None


class AudioBridge:
    def __init__(self, device_hint: str | None = None):
        self.device_hint = device_hint
        self.device: dict | None = None
        self._stream = None
        self._q: deque[np.ndarray] = deque()     # int16 frames, shape (n, channels)
        self._lock = threading.Lock()
        self._queued = 0                          # frames in the queue
        self.underruns = 0
        self.dropped_frames = 0
        self.level = 0.0                          # last chunk RMS, 0..1
        self.started_at = 0.0

    # -- lifecycle -----------------------------------------------------------
    @property
    def running(self) -> bool:
        return self._stream is not None

    def start(self) -> dict:
        import sounddevice as sd

        if self._stream:
            return self.status()
        refresh_devices()
        dev = pick_device(self.device_hint)
        if dev is None:
            raise RuntimeError("no loopback output device found (install BlackHole, then run: sudo killall coreaudiod)")
        self.device = dev
        self._q.clear(); self._queued = 0; self.underruns = 0; self.dropped_frames = 0
        self._stream = sd.OutputStream(device=dev["index"], samplerate=dev["rate"],
                                       channels=min(2, dev["channels"]), dtype="int16",
                                       blocksize=0, latency="low", callback=self._callback)
        self._stream.start()
        self.started_at = time.time()
        log.info("audio bridge -> %s @ %d Hz", dev["name"], dev["rate"])
        return self.status()

    def stop(self) -> None:
        if self._stream:
            self._stream.stop(); self._stream.close(); self._stream = None
            log.info("audio bridge stopped")
        self.device = None

    # -- data ----------------------------------------------------------------
    def push(self, pcm: bytes, rate: int) -> None:
        """Accept one s16le mono chunk from the watch."""
        if not self._stream or not pcm:
            return
        mono = np.frombuffer(pcm, dtype=np.int16)
        self.level = float(np.sqrt(np.mean(mono.astype(np.float32) ** 2)) / 32768.0)
        out_rate = self.device["rate"]
        if out_rate != rate:
            n_out = int(round(len(mono) * out_rate / rate))
            x_old = np.linspace(0.0, 1.0, num=len(mono), endpoint=False)
            x_new = np.linspace(0.0, 1.0, num=n_out, endpoint=False)
            mono = np.interp(x_new, x_old, mono.astype(np.float32)).astype(np.int16)
        frames = np.repeat(mono[:, None], self._stream.channels, axis=1)
        max_frames = int(MAX_BUFFER_MS / 1000 * out_rate)
        with self._lock:
            self._q.append(frames); self._queued += len(frames)
            while self._queued > max_frames and len(self._q) > 1:
                old = self._q.popleft(); self._queued -= len(old); self.dropped_frames += len(old)

    def _callback(self, outdata, frames, time_info, status):  # PortAudio thread
        if status.output_underflow:
            self.underruns += 1
        filled = 0
        with self._lock:
            # hold back until the jitter buffer has reached its target once
            if self._queued < int(TARGET_BUFFER_MS / 1000 * self.device["rate"]) and not self._q:
                pass
            while filled < frames and self._q:
                chunk = self._q[0]
                take = min(frames - filled, len(chunk))
                outdata[filled:filled + take] = chunk[:take]
                filled += take
                if take == len(chunk):
                    self._q.popleft()
                else:
                    self._q[0] = chunk[take:]
                self._queued -= take
        if filled < frames:
            outdata[filled:] = 0
            if self._q is not None and filled == 0:
                self.underruns += 1

    def status(self) -> dict:
        rate = self.device["rate"] if self.device else 0
        return {"running": self.running, "device": self.device["name"] if self.device else None,
                "rate": rate, "buffer_ms": int(self._queued / rate * 1000) if rate else 0,
                "underruns": self.underruns, "dropped_ms": int(self.dropped_frames / rate * 1000) if rate else 0,
                "level": round(self.level, 3)}
