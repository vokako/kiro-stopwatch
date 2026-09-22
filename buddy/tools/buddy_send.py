#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyserial==3.5"]
# ///
"""Send one Kiro Buddy mood to the StopWatch over USB CDC and echo its reply.

Usage:
  uv run buddy/tools/buddy_send.py working
  uv run buddy/tools/buddy_send.py celebrate --port /dev/cu.usbmodem31301

States: sleep idle thinking working waiting question error celebrate
(aliases: sleeping running stuck hmm done)
"""
import argparse, glob, json, sys, time
import serial

p = argparse.ArgumentParser()
p.add_argument("state")
p.add_argument("--port")
a = p.parse_args()

ports = [a.port] if a.port else sorted(glob.glob("/dev/cu.usbmodem*"))
if len(ports) != 1:
    sys.exit(f"buddy_send: expected exactly one port, found {ports}; pass --port")

with serial.Serial(ports[0], 115200, timeout=0.2) as s:
    s.rts = False; s.dtr = True           # DTR asserted or the device drops CDC output
    s.write((json.dumps({"type": "agent_state", "state": a.state}) + "\n").encode())
    end = time.time() + 1.0; buf = b""
    while time.time() < end:
        buf += s.read(4096)
reply = [l for l in buf.decode("utf-8", "replace").splitlines() if "[buddy]" in l]
print(reply[-1] if reply else f"sent {a.state} -> {ports[0]} (no reply within 1 s)")
