"""macOS Accessibility (TCC) check for synthetic keyboard events.

Without this permission CGEventPost silently drops key events: pynput reports
success and nothing happens. The permission is granted per *responsible
process* (usually the terminal app that launched the daemon), so check from
inside the daemon, not from a helper.
"""
from __future__ import annotations

import ctypes
import ctypes.util
import sys


def is_trusted(prompt: bool = False) -> bool | None:
    """True/False on macOS; None on other platforms or if the probe is unavailable."""
    if sys.platform != "darwin":
        return None
    try:
        lib = ctypes.cdll.LoadLibrary(ctypes.util.find_library("ApplicationServices"))
        cf = ctypes.cdll.LoadLibrary(ctypes.util.find_library("CoreFoundation"))
    except (OSError, TypeError):
        return None
    lib.AXIsProcessTrusted.restype = ctypes.c_bool
    if not prompt:
        return bool(lib.AXIsProcessTrusted())
    # AXIsProcessTrustedWithOptions({kAXTrustedCheckOptionPrompt: true}) opens the system dialog.
    cf.CFStringCreateWithCString.restype = ctypes.c_void_p
    cf.CFStringCreateWithCString.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32]
    cf.CFDictionaryCreate.restype = ctypes.c_void_p
    cf.CFDictionaryCreate.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p),
                                      ctypes.c_long, ctypes.c_void_p, ctypes.c_void_p]
    lib.AXIsProcessTrustedWithOptions.restype = ctypes.c_bool
    lib.AXIsProcessTrustedWithOptions.argtypes = [ctypes.c_void_p]
    key = cf.CFStringCreateWithCString(None, b"AXTrustedCheckOptionPrompt", 0x08000100)
    k_true = ctypes.c_void_p.in_dll(cf, "kCFBooleanTrue")
    keys = (ctypes.c_void_p * 1)(key)
    vals = (ctypes.c_void_p * 1)(k_true.value)
    opts = cf.CFDictionaryCreate(None, keys, vals, 1, None, None)
    return bool(lib.AXIsProcessTrustedWithOptions(opts))


HOWTO = ("macOS is dropping synthetic key events: grant Accessibility to the app that runs the daemon "
         "(System Settings → Privacy & Security → Accessibility → add/enable your Terminal, then restart the daemon).")
