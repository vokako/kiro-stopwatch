// Persistent device settings. NVS (Preferences) on the device, plain memory in
// the simulator. Secrets (Wi-Fi PSK, pairing token) are stored here but never
// echoed in the cfg message.
#pragma once
#include <ArduinoJson.h>
#include <cstdint>

namespace settings {

struct Data {
    char     name[24]      = "StopWatch";
    uint8_t  brightness    = 128;
    uint16_t imu_hz        = 20;
    uint16_t screen_off_s  = 30;     // 0 = never
    uint16_t sleep_after_s = 120;    // after screen off; 0 = never
    bool     allow_sleep   = true;
    int16_t  tz_min        = 480;    // UTC offset in minutes (default Asia/Shanghai)
    bool     ble           = false;
    char     ssid[33]      = "";
    char     psk[65]       = "";
    char     token[33]     = "";     // 32 hex chars
};

Data& get();
void  load();
void  save();

// Apply a cfg.set payload; returns false with *err set when a field is invalid.
bool applyPatch(JsonVariantConst patch, const char** err);

// Fill a "cfg" document (no secrets).
void toJson(JsonDocument& d);

} // namespace settings
