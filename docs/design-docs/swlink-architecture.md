# swlink — StopWatch as a host peripheral

Firmware `swlink/0.2` + host package `swlink 0.1.0`. Last verified on hardware 2026-09-16.
Read `../../AGENTS.md` for the traps, `../unresolved.md` for what is still open.

## Goal

Use the M5Stack StopWatch (C152) as an input/output peripheral for a Mac: its buttons act as real
keyboard keys, its sensors and actuators (touch, IMU, battery, RTC, vibration, speaker, microphone,
display) are reachable through one local API, and its screen shows the Kiro ghost reacting to what
happens. New applications should need no firmware change.

## Shape

```
apps (any language) ─── WebSocket ws://127.0.0.1:9898 ───┐
swlink CLI (monitor/send/mic/ws) ────────────────────────┤
browser dashboard (served at http:// on the same port) ──┤
                                                         ▼
                                        swlink daemon (host, Python)
                                        ├─ transport: USB CDC → Wi-Fi TCP → BLE GATT (in that order)
                                        ├─ triggers: btn + IMU messages → named gestures
                                        ├─ keymap:   gesture → key action (fallback path, see below)
                                        ├─ audio:    mic PCM → loopback device (virtual microphone)
                                        └─ history:  SQLite power/connection log, low-battery alert
                                                         │  JSON lines + length-prefixed binary chunks
                                                         ▼
                                        linkfw (device, Arduino + M5Unified/M5GFX)
                                        ├─ transports: USB CDC · TCP :9899 (token) · BLE GATT
                                        ├─ BLE HID keyboard  ← button mappings execute HERE
                                        ├─ settings + key/mood tables in NVS
                                        ├─ Kiro face renderer
                                        └─ power tiers: screen off → light sleep
```

### Decisions that shaped this

1. **Button mappings execute on the device over BLE HID, not by injecting keys on the host.**
   The host-injection path (`pynput` → CGEvent) was built first and works for ordinary apps, but
   macOS input methods and anything reading the HID layer filter synthetic events — they check the
   posting process (`kCGEventSourceUnixProcessID`, which cannot be forged) or bypass the event tap
   entirely. Verified on 2026-09-15: WeChat input method ignored synthetic `alt+O` but accepted the
   same chord from the watch as a paired BLE keyboard. So the mapping table is pushed to the device
   (`keys.set`, stored in NVS) and the watch emits HID reports; the OS also handles key auto-repeat.
   Host injection stays as the fallback when no BLE keyboard connection exists, and remains the only
   path for IMU gestures (those are computed on the host).
2. **One process owns the device link.** USB CDC tolerates a single reader and interleaves writers;
   BLE and the TCP server likewise accept one client. The daemon holds it and multiplexes for
   everyone else over a loopback WebSocket.
3. **Text control plane, binary data plane, one stream.** Every message is a JSON object on its own
   line; bulk audio is a JSON header carrying `n` followed by exactly `n` raw bytes. Readable in a
   serial monitor, no framing library, works identically on USB, TCP and BLE.
4. **Same wire protocol on all three transports.** Only the byte pipe differs, so the dashboard and
   apps are transport-agnostic. USB is trusted by physical connection; TCP needs a pairing token
   minted over USB; a bonded BLE link is trusted like USB.

## Wire protocol

UTF-8, one JSON object per `\n`-terminated line, ≤ 512 bytes. Every object has `"t"`. Device time
`ms` is `millis()`. Chunk types (`mic`, `spk.pcm`) carry `n` and are followed by `n` raw bytes.

### Device → host

| `t` | fields | when |
|---|---|---|
| `hello` | `fw` `board` `name` `mac` `w` `h` `auth` `hid` `caps[]` | boot, and in reply to `hello`. `hid` = the device executes button mappings itself (BLE keyboard connected) |
| `btn` | `id` (`A`/`B`/`PWR`), `ev` (`down`/`up`/`click`/`hold`/`single`/`double`/`triple`), `ms` | see "Click arbitration" |
| `touch` | `ev` (`down`/`move`/`up`), `x`, `y`, `ms` | 466-space coordinates |
| `imu` | `ax ay az` (g), `gx gy gz` (deg/s), `ms` | at `cfg.imu_hz`, default 20, 0 = off |
| `power` | `bat` (%), `chg`, `mv`, `usb`, `ms` | on change, every 5 s awake / 30 s with the screen off, and on request |
| `rtc` | `iso` | on request, and after `rtc.set` |
| `cfg` | see settings table | boot, `cfg.get`, after `cfg.set`. Never contains secrets |
| `keys` | `keys{}` `moods{}` `hid` | `keys.get`, and after `keys.set` |
| `wifi` | `ev` (`off`/`connecting`/`connected`/`disconnected`), `ssid` `ip` `rssi` `mac` `port` `client` | state changes, `wifi.status` |
| `ble` | `enabled` `connected` `name` `mac` | state changes, `ble.status` |
| `screen` | `on` | screen sleeps/wakes (power tier 1) |
| `pair` | `token` | reply to `net.pair` (USB only) |
| `mic` | `seq` `n` `rate` `fmt` (`s16le` or `ima-adpcm`) `samples` — **+ `n` raw bytes** | while streaming; ADPCM only when the link is BLE |
| `keyevent` | `via`=`hid` `ev` `key` `ms` | a mapping fired on the device |
| `ack` / `err` | `of`, `msg` | every accepted / rejected command |
| `log` | `msg` | firmware diagnostics |

### Host → device

| `t` | fields | effect |
|---|---|---|
| `hello` | `token` (TCP only) | re-send `hello`; on TCP this authenticates the connection (3 s or the device closes it) |
| `cfg.get` / `cfg.set` | subset of settings | read / persist+apply settings |
| `keys.get` / `keys.set` | `keys{}` `moods{}` | read / replace both tables, persisted in NVS |
| `imu.rate` | `hz` 0–100 | transient IMU rate (use `cfg.set imu_hz` to persist) |
| `power.get` / `rtc.get` | — | one-shot report |
| `rtc.set` | `iso` (`YYYY-MM-DDThh:mm:ss`) | set the RTC |
| `haptic` | `level` 0–255, `ms` | vibration pulse |
| `tone` | `hz`, `ms` | speaker beep (refused while the mic streams) |
| `mic.start` / `mic.stop` | `rate` 8000/16000 | start/stop `mic` chunks (1024 samples each) |
| `spk.pcm` | `n`, `rate` — **+ `n` raw s16le bytes** (≤ 4096) | play a chunk (refused while the mic streams) |
| `display.text` | `text`, `line` 0–2 | line 1 replaces the face's status line |
| `display.brightness` | `v` 1–255 | transient brightness |
| `mood.set` | `mood`, `ms` (0 = new base mood) | show a Kiro face |
| `trigger` | `name` | apply the mood bound to a host-derived trigger (`imu.*`) |
| `wifi.set` / `wifi.forget` / `wifi.status` | `ssid`, `psk` | provision / erase / report Wi-Fi |
| `net.pair` | — | mint a new 128-bit TCP token (USB only) |
| `ble.status` | — | one `ble` report |

Unknown `t` → `err`. Malformed JSON → `err` with `of:"?"`.

### Daemon-only messages (never reach the device)

`_status` (full daemon + device state, mood/trigger/key catalogues), `_keys.set {keys, moods}`
(validate, apply, persist to TOML, push to device), `_action.test {action}` (fire a key action on
the host), `_history {hours}` (SQLite power log), `_audio.start|stop|status` (virtual microphone),
`_network.pair` (ask the device for a TCP token, USB only), `_ble.pair` (open macOS Bluetooth
settings).

## Settings (device NVS, namespace `swlink`)

| field | default | meaning |
|---|---|---|
| `name` | `StopWatch` | device name; also the BLE and mDNS name |
| `brightness` | 128 | AMOLED brightness 1–255 |
| `imu_hz` | 20 | IMU stream rate, 0 = off |
| `screen_off_s` | 30 | idle seconds before the screen sleeps, 0 = never |
| `sleep_after_s` | 120 | further idle seconds before light sleep, 0 = never |
| `allow_sleep` | true | permit light sleep at all |
| `tz_min` | 480 | UTC offset in minutes, used by SNTP |
| `ble` | false | run the BLE HID keyboard + GATT service (disabling needs a reboot) |
| `wifi_ssid` / `wifi_psk` | — | Wi-Fi credentials; the PSK is never echoed |
| `token` | — | TCP pairing token; only `token_set` is echoed |

Separate NVS keys hold the mapping tables: `keys` and `moods` (JSON, ≤ 24 entries each).

## Transports and pairing

| transport | when the daemon picks it | trust |
|---|---|---|
| USB CDC | an Espressif USB-Serial/JTAG port exists (VID 0x303A, PID 0x1001) | physical connection |
| Wi-Fi TCP :9899 | no USB, a pairing token is stored, and mDNS `_swlink._tcp` finds the device (matched by MAC) | token in `hello` |
| BLE GATT | no USB and no Wi-Fi, `daemon.ble = true` | bonded pairing |

Provisioning order for a fresh device: connect USB → dashboard "Create/rotate pairing token"
(`net.pair`, saved to `~/.config/swlink/swlink.toml`, file mode 0600) → enter Wi-Fi credentials
(`wifi.set`, they travel only over the current link and land in NVS) → unplug. For keyboard use:
enable `ble` over USB, then pair "the device name" in macOS Bluetooth settings (Just Works, no
passkey); it appears as a HID keyboard with media keys and a battery service.

BLE GATT service `6e400001-b5a3-f393-e0a9-e50e24dcca9e`, RX (write) `…0002`, TX (notify) `…0003`.
HID report IDs: 1 = boot keyboard (8 bytes), 2 = consumer/media (16-bit usage).

## Triggers, keys and moods

Mappings bind to *trigger names*, not raw events.

- Button triggers pass straight through: `A.single` `A.double` `A.triple` `A.hold` `A.down` `A.up`
  (same for `B`), `PWR.single` `PWR.double` `PWR.hold`. The immediate `click` is never a trigger.
- IMU triggers are computed on the host from the 20 Hz stream (`swlink/triggers.py`): `imu.shake`
  `imu.tap` `imu.tilt_left` `imu.tilt_right` `imu.tilt_toward` `imu.tilt_away` `imu.face_down`
  `imu.face_up`, with hysteresis and a 0.6 s per-name cooldown.
- Key actions are `[tap:|hold:|press:|release:]KEY[+KEY…]`. `hold:` belongs on a `.hold` trigger:
  it presses on the event and releases on that button's `.up`. Modifiers can name a side
  (`cmd_l`, `alt_r`, …). `tap` holds for 80 ms and staggers modifiers 20 ms ahead of the main key,
  because instant press/release pairs get filtered by apps that watch for "modifier alone".
- Mood values are `"name"` or `"name:ms"` (default 1500 ms); on a `.down` trigger the mood is held
  until `.up`. Button moods are applied on the device; for `imu.*` the daemon forwards
  `{"t":"trigger","name":…}` and the device looks the mood up.

IMU axis orientation, measured on hardware: `ay=+1` left edge down · `ay=-1` right edge down ·
`ax=-1` upright with the screen facing you · `ax=+1` top edge down · `az=+1` screen up ·
`az=-1` screen down.

### Click arbitration

M5Unified decides the click count roughly one hold-threshold after the last release, so the firmware
emits `single`/`double`/`triple` only once the outcome is known: a double click never also fires
`single`, and a button held past the threshold produces no click at all (it fires `hold`). Raw
`.down`/`.up` bypass that arbitration and fire on every press — twice during a double click — so the
dashboard groups them under "Raw edges (advanced)" and warns when one button mixes raw edges with
count triggers.

## Kiro face

`linkfw/src/face.*` renders the ghost into a 320×400 PSRAM sprite (body bitmap 213×258 at `BY=40`).
Base mood: `idle` while any host is connected, `sleep` otherwise. Moods: `sleep idle thinking
working waiting question error celebrate listening dizzy`. `listening` is set automatically for the
duration of mic streaming and animates rings plus a live level meter (block RMS computed in the
firmware); `dizzy` has counter-rotating spiral eyes, a wobble and orbiting stars.

The body bitmap is generated by `linkfw/tools/gen_kiro_body.py` from the official
`kiro-ghost.svg` (rsvg at 2000 px → LANCZOS downscale → sub-pixel blur on alpha → alpha floor →
neutral grey → RGB565). Two constraints learned on hardware: the panel is locked to 16-bit RGB565
by M5GFX, and antialiasing pixels dimmer than ~24/255 read as coloured specks on an emissive AMOLED,
so the ramp must start at a substantial value (`--floor`). Eyes use a 4×4 supersampled ellipse
(`fillEllipseAA`); M5GFX's `fillEllipse` is hard-edged.

The dashboard previews the same animation in the browser (`link/swlink/ui/face.js`), drawing the
ghost as a path rather than shipping the 107 KiB bitmap.

## Power tiers

| tier | entered when | effect | wake |
|---|---|---|---|
| 0 active | any input or host command | screen on, 5 ms loop | — |
| 1 screen off | idle > `screen_off_s` | `Display.sleep()`, links stay up, power reports slow to 30 s | button, touch, host command |
| 2 light sleep | tier 1 plus idle > `sleep_after_s`, `allow_sleep`, **and** no USB host, no Wi-Fi, no BLE, no mic | `esp_light_sleep_start()` | A/B/PWR via ext1 (active-low GPIO 1/2/12) or a 60 s timer |

Tier 2 deliberately never runs while a link is up: a paired Bluetooth keyboard that vanishes to save
power is worse than one that stays awake.

## Audio

Device → host is s16le mono at 8 or 16 kHz in 1024-sample blocks (~15 blocks/s at 16 kHz). Over BLE
the firmware compresses each block with IMA ADPCM (4:1, 516 bytes) because raw PCM at 32 KB/s does
not fit; the host decodes in `swlink/adpcm.py`.

The "virtual microphone" plays that stream into a loopback output device (BlackHole), so any app can
select it as an input: resample to the device rate, 120 ms jitter target, 400 ms cap that drops the
oldest audio. Measured drift: the watch clock runs slightly fast, so the buffer grows ~20 ms/s and
the cap trims it — audible as an occasional small skip on long sessions, not yet corrected.

Mic and speaker share the ES8311 codec and cannot run at the same time; the firmware refuses `tone`
and `spk.pcm` while streaming.

## Device management

The daemon keeps `~/.config/swlink/history.db` (SQLite): one power sample per minute per device MAC
(battery, mV, charging, transport, screen state) plus connection and low-battery events. `_history`
serves a range to the dashboard, which draws a 24 h battery curve. A local macOS notification fires
once per discharge cycle at or below `daemon.low_battery` (default 20 %).

## Repository layout

```
linkfw/                     device firmware (PlatformIO: envs `stopwatch` and `native`)
  src/main.cpp              loop, command dispatch, sensors, power tiers, status footer
  src/link.{hpp,cpp}        framing, multi-sink fan-out (namespace `swl`)
  src/settings.{hpp,cpp}    NVS-backed settings
  src/net.{hpp,cpp}         Wi-Fi station, mDNS, TCP server, SNTP (device build only)
  src/ble.{hpp,cpp}         BLE HID keyboard + battery + swlink GATT service
  src/keymap.{hpp,cpp}      on-device trigger → HID action and mood tables
  src/hidkeys.hpp           key-name → USB HID usage table, action parser
  src/face.{hpp,cpp}        Kiro face renderer
  src/adpcm.hpp             IMA ADPCM encoder
  src/kiro_body.{c,h}       generated body bitmap — do not hand-edit
  tools/gen_kiro_body.py    regenerates the bitmap from the SVG

link/                       host package (uv project, `swlink` CLI)
  swlink/protocol.py        JSON-line + chunk codec
  swlink/transport.py       USB CDC link and port discovery
  swlink/tcp.py             Wi-Fi TCP link and mDNS discovery
  swlink/ble.py             BLE GATT link (bleak)
  swlink/discovery.py       picks the paired device off mDNS
  swlink/triggers.py        IMU gesture detection, trigger catalogue
  swlink/keymap.py          key actions, auto-repeat, macOS event quirks
  swlink/accessibility.py   macOS Accessibility probe
  swlink/audio.py           virtual microphone bridge
  swlink/adpcm.py           IMA ADPCM decoder
  swlink/history.py         SQLite power/event log
  swlink/daemon.py          Hub: transports + WebSocket + dashboard + management
  swlink/cli.py             ports | monitor | send | mic | daemon | ws
  swlink/ui/                dashboard (index.html, face.js)
  tests/                    39 host tests, no hardware needed
```

Other directories: `hello/` (minimal verified template), `buddy/` (the original Kiro mood display),
`sim-smoke/` (SDL smoke test), `references/` (official PDFs and library clones), `scripts/`,
`patches/`.

## Extending this

- **New command**: add it to the tables above, then to `handle()` in `linkfw/src/main.cpp`, then
  expose it in the dashboard. Nothing else needs to know; the daemon forwards unknown JSON as-is.
- **New IMU gesture**: extend `swlink/triggers.py` (host side) — the firmware stays untouched.
  Add the name to `IMU_TRIGGERS` so it appears in the dashboard picker.
- **New mood**: add the enum + drawing in `linkfw/src/face.cpp`, the name in `MOODS`
  (`swlink/daemon.py`), and mirror the animation in `link/swlink/ui/face.js` for the preview.
- **New key name**: `hidkeys.hpp` (device, HID usage) and `KEY_NAMES` (`daemon.py`, picker groups);
  `swlink/keymap.py` resolves host-side names through pynput.
- **New transport**: implement `open/close/send/connected/port` like `swlink/tcp.py` and add it to
  `Hub._open_any()`; the device side needs a `swl::Stream` and `swl::addSink()`.
