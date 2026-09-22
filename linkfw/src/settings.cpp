#include "settings.hpp"
#include <cstring>

#if defined(ESP_PLATFORM)
#include <Preferences.h>
static Preferences s_prefs;
static constexpr const char* kNs = "swlink";
#endif

namespace settings {

static Data s_data;
Data& get() { return s_data; }

void load()
{
#if defined(ESP_PLATFORM)
    s_prefs.begin(kNs, true);
    s_prefs.getString("name", s_data.name, sizeof s_data.name);
    s_data.brightness    = s_prefs.getUChar("bright", s_data.brightness);
    s_data.imu_hz        = s_prefs.getUShort("imu_hz", s_data.imu_hz);
    s_data.screen_off_s  = s_prefs.getUShort("scr_off", s_data.screen_off_s);
    s_data.sleep_after_s = s_prefs.getUShort("slp_aft", s_data.sleep_after_s);
    s_data.allow_sleep   = s_prefs.getBool("allow_slp", s_data.allow_sleep);
    s_data.tz_min        = s_prefs.getShort("tz_min", s_data.tz_min);
    s_data.ble           = s_prefs.getBool("ble", s_data.ble);
    s_prefs.getString("ssid", s_data.ssid, sizeof s_data.ssid);
    s_prefs.getString("psk", s_data.psk, sizeof s_data.psk);
    s_prefs.getString("token", s_data.token, sizeof s_data.token);
    s_prefs.end();
#endif
}

void save()
{
#if defined(ESP_PLATFORM)
    s_prefs.begin(kNs, false);
    s_prefs.putString("name", s_data.name);
    s_prefs.putUChar("bright", s_data.brightness);
    s_prefs.putUShort("imu_hz", s_data.imu_hz);
    s_prefs.putUShort("scr_off", s_data.screen_off_s);
    s_prefs.putUShort("slp_aft", s_data.sleep_after_s);
    s_prefs.putBool("allow_slp", s_data.allow_sleep);
    s_prefs.putShort("tz_min", s_data.tz_min);
    s_prefs.putBool("ble", s_data.ble);
    s_prefs.putString("ssid", s_data.ssid);
    s_prefs.putString("psk", s_data.psk);
    s_prefs.putString("token", s_data.token);
    s_prefs.end();
#endif
}

template <typename T>
static bool rangeInt(JsonVariantConst v, long lo, long hi, T& out, const char** err, const char* what)
{
    if (v.isNull()) return true;
    if (!v.is<long>()) { *err = what; return false; }
    long x = v.as<long>();
    if (x < lo || x > hi) { *err = what; return false; }
    out = (T)x;
    return true;
}

bool applyPatch(JsonVariantConst p, const char** err)
{
    Data d = s_data;    // validate into a copy, commit at the end
    if (!p["name"].isNull()) {
        const char* n = p["name"];
        if (!n || !*n || strlen(n) >= sizeof d.name) { *err = "name 1-23 chars"; return false; }
        strcpy(d.name, n);
    }
    if (!rangeInt(p["brightness"], 1, 255, d.brightness, err, "brightness 1-255")) return false;
    if (!rangeInt(p["imu_hz"], 0, 100, d.imu_hz, err, "imu_hz 0-100")) return false;
    if (!rangeInt(p["screen_off_s"], 0, 3600, d.screen_off_s, err, "screen_off_s 0-3600")) return false;
    if (!rangeInt(p["sleep_after_s"], 0, 86400, d.sleep_after_s, err, "sleep_after_s 0-86400")) return false;
    if (!rangeInt(p["tz_min"], -720, 840, d.tz_min, err, "tz_min -720..840")) return false;
    if (!p["allow_sleep"].isNull()) { if (!p["allow_sleep"].is<bool>()) { *err = "allow_sleep bool"; return false; } d.allow_sleep = p["allow_sleep"]; }
    if (!p["ble"].isNull())         { if (!p["ble"].is<bool>())         { *err = "ble bool"; return false; }         d.ble = p["ble"]; }
    s_data = d;
    save();
    return true;
}

void toJson(JsonDocument& d)
{
    d["t"] = "cfg";
    d["name"] = s_data.name;
    d["brightness"] = s_data.brightness;
    d["imu_hz"] = s_data.imu_hz;
    d["screen_off_s"] = s_data.screen_off_s;
    d["sleep_after_s"] = s_data.sleep_after_s;
    d["allow_sleep"] = s_data.allow_sleep;
    d["tz_min"] = s_data.tz_min;
    d["ble"] = s_data.ble;
    d["wifi_ssid"] = s_data.ssid;
    d["wifi_set"] = s_data.ssid[0] != 0;
    d["token_set"] = s_data.token[0] != 0;
}

} // namespace settings
