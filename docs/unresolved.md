# Unresolved

Open issues, unverified claims and deliberate gaps. Keep this honest: an item here is worth more
than an optimistic sentence in the design doc. Date every entry.

## Not verified on hardware

- **Wi-Fi transport end to end** (2026-09-15). Device side is implemented and reports correctly
  (`wifi.set`, mDNS `_swlink._tcp`, TCP :9899, token auth, SNTP → RTC), and the daemon has the
  matching `TcpLink` + mDNS discovery with tests. Never run with real credentials, because the
  agent must not read or invent the user's Wi-Fi password. Needs: enter SSID/PSK in the dashboard,
  unplug USB, confirm the daemon reconnects over TCP and the dashboard keeps working.
- **BLE GATT from the daemon** (2026-09-15). The watch's BLE HID keyboard is verified. The daemon's
  `BleLink` (bleak) has never connected, because CoreBluetooth refuses with "Bluetooth device is
  turned off" unless the process that runs the daemon holds macOS Bluetooth permission — the agent's
  execution context does not. Needs: run `uv run swlink daemon` from the user's own terminal once and
  allow the prompt.
- **Power measurements.** All the current figures in conversation were ESP32-S3/BMI270 typicals, not
  measured on this board. The power tiers are implemented but nobody has watched the battery curve
  across tier 1 → tier 2 on battery. Needs: unplug with `allow_sleep = true`, leave it overnight,
  compare `~/.config/swlink/history.db` samples.
- **`spk.pcm` latency and quality.** Protocol path works; only smoke-tested with a generated
  sine. No latency numbers, no test with real audio.
- **Wi-Fi + BLE together.** Static RAM after both stacks link is 27.4 %, but the two share the
  2.4 GHz radio and neither concurrency nor heap pressure has been stress-tested.

## Known limitations accepted for now

- **WebSocket has no authentication** (loopback only). Anything that can reach 127.0.0.1 can press
  keys through the keymap and read sensors. Fine for a personal machine, not beyond it.
- **Virtual microphone drift.** The watch sample clock runs slightly fast; the jitter buffer grows
  about 20 ms/s until the 400 ms cap drops the oldest audio, which is an occasional small skip on
  long sessions. A proper fix resamples adaptively instead of trimming.
- **Virtual microphone is named "BlackHole 2ch"** in app menus, not "StopWatch". Renaming means
  building a customised BlackHole.
- **No system-native Bluetooth microphone.** ESP32-S3 has BLE only; macOS accepts Bluetooth mics
  over classic HFP/SCO, and its LE Audio support is closed to third-party devices. Audio therefore
  always goes through the daemon (USB/Wi-Fi raw, BLE via ADPCM).
- **Disabling BLE needs a reboot.** Bluedroid cannot be de-initialised and re-initialised cleanly in
  place, so `cfg.set {ble:false}` only takes effect after a restart.
- **Factory firmware was never backed up.** `scripts/backup-factory.sh` exists (chunked,
  `--no-stub`) but has not completed a full run; the user accepted restoring via M5Burner instead.
- **The repository is not under version control.** No `git init` yet, so there is no history to
  bisect and no way to see what a previous agent changed.

## Ideas not started

- Adaptive resampling for the audio bridge (fixes the drift above).
- `ArduinoOTA`: flash over Wi-Fi instead of entering download mode.
- USB composite device (HID keyboard + CDC) so a plugged watch is a real keyboard without BLE;
  needs the Arduino core's TinyUSB HID, and USB *audio* would additionally need ESP-IDF.
- Multi-device support: the history schema is keyed by MAC already, the daemon is not.
- Wire the daemon into pm2 (`~/work/system/pm2/`) so it survives reboots.
