# swlink requirements

What the user asked for, in the order it was asked, and where each item stands. This is the
"what/why"; `../design-docs/swlink-architecture.md` is the "how".

| # | Requirement | Status |
|---|---|---|
| 1 | A host driver so the watch works as a computer peripheral | done — `link/` (daemon, CLI, dashboard) + `linkfw/` |
| 2 | Map watch buttons to computer keys | done — executed on the device as BLE HID; host injection is the fallback |
| 3 | Expose every sensor: mic, speaker, IMU, battery, RTC, touch, haptics | done — all in the protocol and on the dashboard |
| 4 | Fast local iteration, ideally a simulator / digital twin | done — every firmware project builds for `native` (SDL) as well as the device |
| 5 | A UI to see and configure things | done — dashboard served by the daemon at `http://127.0.0.1:9898/` |
| 6 | Configure mappings in the browser, not a text file | done — grouped pickers, saved to TOML and pushed to the device |
| 7 | Distinguish left/right modifiers | done — `cmd_l`/`cmd_r`/… selectable per side |
| 8 | Recognise the power key | done — `PWR.single/double/hold` via the M5PM1 PMIC |
| 9 | Richer gestures: single/double/triple/hold plus IMU gestures | done — plus arbitration so a double click never fires single |
| 10 | Hold a watch button = hold a keyboard key, with auto-repeat | done — `hold:` on a `.hold` trigger; the OS repeats when the watch is the keyboard |
| 11 | Behave like a real keyboard, including for input methods | done — BLE HID; the synthetic-event path could not satisfy this |
| 12 | Use the watch as a microphone device for any app | done — virtual microphone via BlackHole loopback |
| 13 | Bluetooth pairing plus Wi-Fi provisioning from the host, keep working unplugged | device side done and verified for BLE; Wi-Fi untested with real credentials (`../unresolved.md`) |
| 14 | Remember settings on the device (clock, name, timers, mappings) | done — NVS, verified across resets |
| 15 | Act as a device manager: battery history and health | done — SQLite log, 24 h curve, low-battery notification |
| 16 | Low-power standby with wake on button | implemented, not measured on battery (`../unresolved.md`) |
| 17 | Kiro ghost faces bound to triggers, incl. a voice-input face | done — 10 moods, per-trigger table, `listening` automatic while the mic streams |
| 18 | Preview faces in the browser | done — `link/swlink/ui/face.js` |
| 19 | Fix jagged edges on the ghost | done — regenerated bitmap with a wider AA ramp, supersampled eyes |

## Non-goals (decided during the work)

- Reimplementing the UI logic separately for the simulator: one source, two build environments.
- Storing the user's Wi-Fi password anywhere on the host: it goes over the live link into device NVS.
- Making the WebSocket reachable off-machine: loopback only, no auth (see `../unresolved.md`).
- Running the microphone over BLE without compression: the link cannot carry 32 KB/s.
