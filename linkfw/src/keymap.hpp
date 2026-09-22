// On-device key mapping, executed over BLE HID so the OS sees a real keyboard.
// The table is the same {"A.single": "cmd+space", ...} the host edits; the
// daemon pushes it with keys.set and it persists in NVS. Only button triggers
// are handled here; IMU gestures stay on the host (they need the daemon anyway).
#pragma once
#include <ArduinoJson.h>
#include "hidkeys.hpp"

namespace keymap {

static constexpr int kMax = 24;

struct Entry { char trigger[16]; char spec[64]; hidkeys::Action action; };
struct MoodEntry { char trigger[16]; uint8_t mood; uint16_t ms; };   // ms 0 = hold while button down

void load();                                   // from NVS
bool set(JsonVariantConst keys, const char** err);   // validate, replace, persist
bool setMoods(JsonVariantConst moods, const char** err);  // {"A.single":"celebrate", "B.down":"working"}
void toJson(JsonDocument& d);                  // {"t":"keys","keys":{...}}
int  count();

// Called for every button trigger name ("A.down", "A.single", "B.hold", ...).
// Returns true if a HID action was executed. Emits a keyevent message for the log.
// Also applies the mood bound to the trigger (independent of HID state).
bool onTrigger(const char* name);
void applyMood(const char* name);              // mood part only (used for host-fed triggers too)
void releaseAll();                             // on BLE disconnect

} // namespace keymap
