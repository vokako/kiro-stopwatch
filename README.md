# M5Stack StopWatch workspace

Development workspace for the M5Stack StopWatch (SKU C152) — a 1.75" round AMOLED touch device on an
ESP32-S3. The main result here is **swlink**: firmware plus a Mac driver that turns the watch into a
keyboard, a sensor hub, a microphone and a little Kiro ghost that reacts to what you do.

- Users start here.
- Agents: read `AGENTS.md` first, then `docs/design-docs/swlink-architecture.md` and
  `docs/unresolved.md`.

## Quick start

The host driver needs nothing but `uv`:

```sh
cd link
uv sync                       # first time only
uv run swlink daemon --open   # starts the driver and opens the dashboard
```

Building firmware additionally needs PlatformIO, SDL2 for the simulator, and the
upstream M5Stack libraries, which are **not** committed (they are ~1.2 GB with the
vendor PDFs). Fetch them at the pinned revisions once per clone:

```sh
brew install platformio sdl2      # once per machine
./scripts/fetch-references.sh     # ~60 MB: M5Unified, M5GFX + the SDL patch
./scripts/fetch-references.sh --all   # plus the optional reference projects
```

The dashboard is at [http://127.0.0.1:9898/](http://127.0.0.1:9898/). It is served *by* the daemon —
when the daemon stops, the page stops working. Everything below can be done from that page.

To flash the firmware:

```sh
cd linkfw
pio run -e stopwatch -t upload --upload-port /dev/cu.usbmodemXXXX   # stop the daemon first
```

`ls /dev/cu.usbmodem*` finds the port; the number changes between plugs.

## What the dashboard gives you

| Card | What it does |
|---|---|
| Device | firmware, capabilities, battery, RTC, uptime; sync the clock |
| IMU | live tilt bubble and six-axis bars; change the sample rate |
| Touch · buttons | touch position on the round screen, button state |
| Key mapping | the mapping table: trigger → key action → Kiro face, with a test button |
| Kiro face | preview all ten moods in the browser, send one to the watch |
| Actuators | vibrate, beep, brightness, write text on the screen |
| Bluetooth keyboard | enable BLE, open macOS Bluetooth settings, see mapping state |
| Virtual microphone | play the watch mic into a loopback device so any app can record it |
| Microphone | record straight to a WAV file |
| Network · Wi-Fi | provision Wi-Fi, mint the pairing token for unplugged use |
| Power policy | screen-off and sleep timers |
| Battery history | 24 h curve from the local SQLite log |
| Event log | three layers: `⚡` trigger matched · `⌨` key sent · everything else |

## Using the watch as a keyboard

1. Plug in USB, start the daemon.
2. In **Bluetooth keyboard**, click *Enable Bluetooth*, then *Open Bluetooth settings…* and connect
   the watch (it appears under its device name, pairs as a keyboard, no passkey).
3. Edit the **Key mapping** table and press *Save*. The table is pushed to the watch and stored
   there, so it keeps working unplugged.

Once paired, the watch itself emits the key events, so input methods, games and anything reading the
HID layer see them — a synthetic-key approach does not survive those. Without a Bluetooth
connection the daemon injects keys instead, which needs macOS *Accessibility* permission for the
terminal running it (the dashboard header shows whether that is granted).

Mapping rules worth knowing:

- A ready-made starting point — voice, confirm, cancel, session switching, and how to wire the voice
  buttons to macOS Dictation or the WeChat input method — is in
  [docs/recommended-config.md](docs/recommended-config.md).
- `single` / `double` / `triple` are decided after the multi-click window, so a double click never
  also fires a single. A held button fires `hold` and no click at all.
- For "hold the watch button = hold a keyboard key", use the `.hold` trigger with the *hold while
  pressed* action. The raw `.down`/`.up` edges fire on every press (twice during a double click) and
  the dashboard warns if you mix them with click counting.
- Modifiers can be left- or right-specific (`cmd_l`, `alt_r`, …); right-side keys are distinct to
  macOS, which makes them good dedicated hotkeys.
- IMU gestures (`shake`, `tap`, tilt directions, face up/down) are computed by the daemon, so they
  need it running even when the watch is a Bluetooth keyboard.

## Using the watch as a microphone

```sh
brew install --cask blackhole-2ch
sudo killall coreaudiod          # once, so the new device appears
```

Then in **Virtual microphone** pick `● BlackHole 2ch`, press *Start*, and select BlackHole as the
microphone in Zoom / QuickTime / anything. Set `audio_autostart = true` in the config to start it
whenever the watch connects. The watch shows the `listening` face with a live level meter while it
streams.

## Kiro faces

The screen shows the Kiro ghost: `idle` while a host is connected, `sleep` otherwise. Ten moods:
`sleep idle thinking working waiting question error celebrate listening dizzy`. Each mapping row can
bind a face — it flashes for 1.5 s on a tap, or is held while a `.down` button is pressed. A row may
have a face and no key (shake the watch → `dizzy`). Any program can drive it:

```sh
uv run swlink ws '{"t":"mood.set","mood":"working","ms":0}'
```

## Command line

```sh
uv run swlink ports                          # candidate serial ports
uv run swlink monitor                        # print every device message (needs the daemon stopped)
uv run swlink send '{"t":"haptic","level":150,"ms":100}'
uv run swlink mic out.wav --seconds 3        # record to WAV
uv run swlink daemon --open                  # driver + dashboard
uv run swlink daemon --no-keys               # sensors only, no key injection
uv run swlink ws '{"t":"tone","hz":880,"ms":150}'   # talk to a running daemon
uv run pytest                                # 39 host tests, no hardware needed
```

Only one process may hold the device link: stop the daemon before `monitor`, `send` or `mic`.

## Config

`~/.config/swlink/swlink.toml` (created on first save, mode 0600) holds the key mapping, the mood
table, daemon options and the Wi-Fi pairing token. `link/swlink/swlink.example.toml` documents every
option. Device-side settings (name, brightness, timers, Wi-Fi credentials, mapping tables) live in
the watch's NVS and survive resets and reflashes of unrelated code.

Battery and connection history: `~/.config/swlink/history.db` (SQLite).

## Running it on another computer

The dashboard is not standalone — copy the whole `link/` directory:

1. `brew install uv`, copy `link/`, run `uv sync`.
2. `uv run swlink daemon --open`.
3. Optional: install BlackHole for the virtual microphone; grant Accessibility if you want host key
   injection; copy `~/.config/swlink/swlink.toml` to reuse your mappings.
4. Pair the watch over Bluetooth again (pairing is per computer). Mappings already stored on the
   watch keep working immediately.

## Hardware notes

Verified with read-only probes on 2026-09-14:

- ESP32-S3 (QFN56) rev v0.2, 16 MB flash, 8 MB PSRAM, a stable MAC reported by `esptool chip-id`.
- Espressif USB-Serial/JTAG (VID `0x303A`, PID `0x1001`); no external UART adapter needed.
- Download mode: hold the power button ~2 s until the green LED lights, then release.
- `M5.Display.width()` reports **468** on the device but 466 in the simulator — use the physical
  geometry (centre 233,233) instead of deriving from `width()`.

**Hardware revision warning**: C152 exists as v1.0 and v1.0.1. On some v1.0 units the rear pin
printed `BAT` is actually **5V IN** — never connect a battery there.

## Other projects in this workspace

| Directory | What it is |
|---|---|
| `linkfw/` | swlink firmware (device + SDL simulator builds) |
| `link/` | swlink host package: daemon, CLI, dashboard |
| `hello/` | minimal verified Hello World, the template for new firmware |
| `buddy/` | the original Kiro mood display, driven by buttons or USB JSON |
| `sim-smoke/` | smallest possible SDL smoke test |
| `references/` | official PDFs, schematics and library clones (read-only) |
| `scripts/` | device utilities, e.g. the chunked factory-flash backup |

Every firmware project builds twice from one source: `pio run -e native` runs it on the Mac against
SDL (mouse = touch, keyboard = buttons), `pio run -e stopwatch` builds for the device. Iterate on the
Mac, flash only for what the host cannot model.
