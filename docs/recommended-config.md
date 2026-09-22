# Recommended configuration

A starting mapping that covers the everyday uses of the watch — voice input, confirm, cancel, and
switching sessions — plus how to wire the voice buttons to whatever actually does the listening
(macOS Dictation, the WeChat input method, or a push-to-talk app).

Everything here is editable in the dashboard at [http://127.0.0.1:9898/](http://127.0.0.1:9898/);
the TOML below is what it saves to `~/.config/swlink/swlink.toml`.

## The default mapping

| Trigger | Sends | For |
|---|---|---|
| Right button, single click | `f17` | toggle real-time voice on/off |
| Right button, hold | `f18` held down | push-to-talk: speak while held |
| Right button, double click | `enter` | confirm / send |
| Left button, double click | `esc` | cancel |
| Left button, single click | `cmd+]` | next session |
| Left button, triple click | `cmd+[` | previous session |
| Shake | *(no key)* | `dizzy` face, as a "no" gesture |

One click sends one chord, so `cmd+[` and `cmd+]` need two different triggers; the triple click is
the least-used one left on that button.

`f17` and `f18` are placeholders for "a hotkey nothing else claims" — see
[Choosing the hotkeys](#choosing-the-hotkeys). Swap in whatever your voice tool is set to.

```toml
[keys]
"A.single" = "f17"          # real-time voice: toggle on, toggle off
"A.hold"   = "hold:f18"     # push-to-talk: pressed on hold, released when the button comes up
"A.double" = "enter"
"B.single" = "cmd+]"
"B.double" = "esc"
"B.triple" = "cmd+["

# Kiro's face reacts, so you can see what the watch just did without looking at the Mac.
[moods]
"A.down"    = "listening"   # held while the right button is down (both voice actions)
"A.double"  = "working:800"
"B.single"  = "thinking:800"
"B.double"  = "error:800"
"imu.shake" = "dizzy:1500"

[daemon]
key_repeat = true           # a held key auto-repeats, as a physical one does
```

`imu.shake` appears only under `[moods]`: a row with a face and no key is fine, and leaving it out
of `[keys]` is what keeps it from also sending something.

## Which button is A and which is B

The firmware names the two programmable buttons `A` and `B` (plus `PWR`, the power key). Which one
is on which side is not written down anywhere, and this table assumes **A is the right-hand button**.
Check yours in one second: open the dashboard, watch the **Touch · buttons** card, and press each
button — the card lights up the one that fired. If yours is mirrored, swap the `A` and `B` rows.

## Choosing the hotkeys

Two constraints come from how the watch sends keys:

- **One click is one chord.** Shortcuts of the "press Control twice" / "press Fn twice" family —
  which includes the macOS Dictation default — cannot be produced by a single click. Pick a shortcut
  that is a single chord, or a single key.
- **Nothing else may claim it.** The watch is a real keyboard, so a chord that already means
  something in the front app will do that instead.

`F13`–`F19` are the safest single keys: macOS assigns them nothing by default and few apps use them.
If you would rather use a chord, the right-hand modifiers are good, because macOS tells them apart
from the left ones and almost nothing binds them: `cmd_r+alt_r+d`, `ctrl_r+shift_r+v`, and so on.
Every key name swlink accepts is listed in `link/swlink/swlink.example.toml`.

**The reliable way to set any of these**: pair the watch first and save the mapping, then open the
shortcut recorder in the app you are configuring and *press the watch button*. Whatever the app
records is exactly what the watch sends, so there is no key-name guessing.

## Wiring the voice buttons

### macOS Dictation (系统设置 → 键盘 → 听写)

1. **System Settings → Keyboard → Dictation** (设置 → 键盘 → 听写), turn it on.
2. Click the **Shortcut** (快捷键) pop-up → **Customize** (自定义), then press the keys you want —
   or press the watch's right button, per the trick above. Apple's own guide documents Customize as
   "press the keys you want to use".
3. Dictation is toggle-style: the shortcut starts it, the same shortcut or `Esc` stops it. That is
   exactly the single-click row (`A.single`), and it pairs well with `esc` on the left button's
   double click as the stop.

macOS Dictation has no built-in push-to-talk, so the hold row (`A.hold`) needs a tool that offers a
"hold to talk" hotkey. Point `hold:` at that tool's hotkey and the watch behaves like its foot pedal.

### WeChat input method (微信输入法)

Its voice input is driven by a hotkey you set in the input method itself: open its settings from the
menu-bar icon and look for the voice-input shortcut (语音输入快捷键). Set it with the watch button
pressed into the recorder, then leave `[keys]` pointing at the same key.

I could not verify the exact menu path or the default hotkey for the Mac build, and both have moved
between versions — treat the above as "where to look", not a quoted setting.

**This one needs Bluetooth.** Input methods read the HID layer and ignore synthetic key events, which
was measured on this project: the watch's own BLE HID keys reach the WeChat input method, while the
daemon's injected keys never do. So pair the watch as a Bluetooth keyboard (dashboard → *Bluetooth
keyboard* → *Enable Bluetooth*) before expecting voice input to fire. Without pairing, the same
mapping still works in ordinary text fields through the daemon, provided it has Accessibility
permission.

### Anything else

Any app with a global hotkey works the same way: a toggle feature goes on `A.single`, a
hold-to-speak feature on `A.hold` with the `hold:` verb.

## What to expect when pressing the buttons

- **Click counting costs about half a second.** The watch cannot know a single click is not the start
  of a double one, so it waits out the multi-click window — M5Unified's default, 500 ms after the
  last release — and only then sends `single`. `Enter` and `Esc` therefore land half a second after
  you let go. If you want one of them instant, move it to `.down`, and accept that the button can no
  longer carry click counts.
- **Hold fires at 500 ms** of continuous press, and never produces a click, so the three right-button
  rows never collide.
- **Put hold actions on `.hold`, not `.down`.** `.down` fires on every press, including both presses
  of a double click.
- The mapping lives in the watch's NVS, so the buttons keep working with the Mac asleep or the daemon
  stopped. The shake gesture is computed on the host and needs the daemon running.

## Not verified yet

The mapping grammar, the arbitration timings and the BLE HID path are all exercised; this particular
table is a recommendation rather than a measured setup. The voice integrations depend on third-party
settings that change between versions — if a row does not fire, check it in the dashboard's event log
first (`⚡` trigger matched, `⌨` key sent), which tells you whether the watch or the receiving app is
the problem.
