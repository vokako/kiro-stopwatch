#include "keymap.hpp"
#include "ble.hpp"
#include "face.hpp"
#include "link.hpp"
#include <M5Unified.h>
#include <cstring>

#if defined(ESP_PLATFORM)
#include <Preferences.h>
#endif

namespace keymap {

static Entry s_entries[kMax];
static int   s_count = 0;
static MoodEntry s_moods[kMax];
static int   s_moodCount = 0;
// currently held action per button (index by button id A/B/PWR)
static const hidkeys::Action* s_held[3] = { nullptr, nullptr, nullptr };

static int buttonIndex(const char* trigger)
{
    if (trigger[0] == 'A' && trigger[1] == '.') return 0;
    if (trigger[0] == 'B' && trigger[1] == '.') return 1;
    if (!strncmp(trigger, "PWR.", 4)) return 2;
    return -1;
}

static void persist(const char* key, const char* json)
{
#if defined(ESP_PLATFORM)
    Preferences p; p.begin("swlink", false); p.putString(key, json); p.end();
#endif
}

static bool applyMoods(JsonVariantConst moods, const char** err)
{
    MoodEntry tmp[kMax]; int n = 0;
    for (JsonPairConst kv : moods.as<JsonObjectConst>()) {
        if (n >= kMax) { *err = "too many moods"; return false; }
        const char* k = kv.key().c_str(); const char* v = kv.value().as<const char*>();
        if (!v || strlen(k) >= sizeof tmp[n].trigger) { *err = "mood entry too long"; return false; }
        char spec[32]; strncpy(spec, v, sizeof spec - 1); spec[sizeof spec - 1] = 0;
        uint16_t ms = 1500;
        if (char* c = strchr(spec, ':')) { *c = 0; ms = (uint16_t)atoi(c + 1); }
        int m = face::fromName(spec);
        if (m < 0) { *err = "unknown mood"; return false; }
        const char* ev = strchr(k, '.'); 
        if (ev && !strcmp(ev, ".down")) ms = 0;            // held while the button is down
        strcpy(tmp[n].trigger, k); tmp[n].mood = (uint8_t)m; tmp[n].ms = ms; n++;
    }
    memcpy(s_moods, tmp, sizeof tmp); s_moodCount = n;
    return true;
}

static bool apply(JsonVariantConst keys, const char** err)
{
    Entry tmp[kMax]; int n = 0;
    for (JsonPairConst kv : keys.as<JsonObjectConst>()) {
        if (n >= kMax) { *err = "too many mappings"; return false; }
        const char* k = kv.key().c_str(); const char* v = kv.value().as<const char*>();
        if (!v || strlen(k) >= sizeof tmp[n].trigger || strlen(v) >= sizeof tmp[n].spec) { *err = "mapping too long"; return false; }
        if (strncmp(k, "imu.", 4) == 0) continue;            // host-side gesture: not ours
        hidkeys::Action a = hidkeys::parse(v);
        if (!a.valid) { *err = "unknown key name"; return false; }
        strcpy(tmp[n].trigger, k); strcpy(tmp[n].spec, v); tmp[n].action = a; n++;
    }
    memcpy(s_entries, tmp, sizeof tmp); s_count = n;
    return true;
}

void load()
{
#if defined(ESP_PLATFORM)
    Preferences p; p.begin("swlink", true);
    char json[1536]; p.getString("keys", json, sizeof json); p.end();
    if (!json[0]) return;
    JsonDocument d;
    if (!deserializeJson(d, json)) { const char* e; apply(d.as<JsonVariantConst>(), &e); }
    Preferences q; q.begin("swlink", true);
    char mj[1024]; q.getString("moods", mj, sizeof mj); q.end();
    if (mj[0]) { JsonDocument md; if (!deserializeJson(md, mj)) { const char* e; applyMoods(md.as<JsonVariantConst>(), &e); } }
#endif
}

bool setMoods(JsonVariantConst moods, const char** err)
{
    if (!moods.is<JsonObjectConst>()) { *err = "moods must be an object"; return false; }
    if (!applyMoods(moods, err)) return false;
    char json[1024];
    size_t n = serializeJson(moods, json, sizeof json - 1); json[n] = 0;
    persist("moods", json);
    return true;
}

bool set(JsonVariantConst keys, const char** err)
{
    if (!keys.is<JsonObjectConst>()) { *err = "keys must be an object"; return false; }
    if (!apply(keys, err)) return false;
    char json[1536];
    size_t n = serializeJson(keys, json, sizeof json - 1); json[n] = 0;
    persist("keys", json);
    return true;
}

void toJson(JsonDocument& d)
{
    d["t"] = "keys";
    JsonObject o = d["keys"].to<JsonObject>();
    for (int i = 0; i < s_count; i++) o[s_entries[i].trigger] = s_entries[i].spec;
    JsonObject mo = d["moods"].to<JsonObject>();
    for (int i = 0; i < s_moodCount; i++) {
        if (s_moods[i].ms) { char v[32]; snprintf(v, sizeof v, "%s:%u", face::name((face::Mood)s_moods[i].mood), s_moods[i].ms); mo[s_moods[i].trigger] = v; }
        else mo[s_moods[i].trigger] = face::name((face::Mood)s_moods[i].mood);
    }
    d["hid"] = ble::hidReady();
}

int count() { return s_count; }

static void emit(const char* ev, const Entry& e)
{
    JsonDocument d; d["t"] = "keyevent"; d["via"] = "hid"; d["ev"] = ev; d["key"] = e.spec; d["ms"] = swl::nowMs();
    swl::sendDoc(d);
}

static void down(const hidkeys::Action& a)
{
    ble::keyReport(a.mods, a.keys, a.nkeys);
    if (a.consumer) ble::consumerReport(a.consumer);
}

static void up(const hidkeys::Action& a)
{
    if (a.consumer) ble::consumerReport(0);
    ble::keyReport(0, nullptr, 0);
}

void applyMood(const char* name)
{
    const char* ev = strchr(name, '.'); ev = ev ? ev + 1 : "";
    char tag[8]; tag[0] = name[0]; tag[1] = (name[1] == '.') ? 0 : name[1]; tag[2] = 0;   // "A", "B", "PW": per-button hold tag
    if (!strcmp(ev, "up")) { face::release(tag); }
    for (int i = 0; i < s_moodCount; i++) {
        if (strcmp(s_moods[i].trigger, name)) continue;
        face::set((face::Mood)s_moods[i].mood, s_moods[i].ms, tag);
        return;
    }
}

bool onTrigger(const char* name)
{
    applyMood(name);
    if (!ble::hidReady()) return false;
    int btn = buttonIndex(name);
    const char* ev = strchr(name, '.'); ev = ev ? ev + 1 : "";

    // a button coming up ends whatever it holds, regardless of mappings
    if (!strcmp(ev, "up") && btn >= 0 && s_held[btn]) {
        up(*s_held[btn]); s_held[btn] = nullptr;
        return true;
    }
    for (int i = 0; i < s_count; i++) {
        Entry& e = s_entries[i];
        if (strcmp(e.trigger, name)) continue;
        switch (e.action.verb) {
        case hidkeys::Action::tap:
            down(e.action); M5.delay(40); up(e.action); emit("tap", e); return true;
        case hidkeys::Action::hold:
            // "hold" fires from the hold event (button held past the threshold) or from a
            // raw .down. Using .hold keeps it clear of single/double click arbitration.
            if (strcmp(ev, "hold") && strcmp(ev, "down")) return false;
            if (btn >= 0) { if (s_held[btn]) up(*s_held[btn]); s_held[btn] = &e.action; }
            down(e.action); emit("hold", e); return true;  // OS auto-repeats a held HID key itself
        case hidkeys::Action::press:
            down(e.action); emit("press", e); return true;
        case hidkeys::Action::release:
            up(e.action); emit("release", e); return true;
        }
    }
    return false;
}

void releaseAll()
{
    for (auto& h : s_held) { if (h) { up(*h); h = nullptr; } }
}

} // namespace keymap
