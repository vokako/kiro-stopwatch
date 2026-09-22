// Key names (same vocabulary as the host's swlink/keymap.py) -> USB HID usages.
// Keyboard page usages go in the boot keyboard report; consumer-page usages
// (media keys) go in a separate consumer report.
#pragma once
#include <cstdint>
#include <cstring>

namespace hidkeys {

enum Kind : uint8_t { none = 0, key = 1, modifier = 2, consumer = 3 };

struct Usage { Kind kind; uint16_t code; };   // modifier: bitmask; key: keyboard usage; consumer: usage id

// Modifier bitmask in the boot keyboard report byte 0.
enum Mod : uint8_t { ctrl_l = 0x01, shift_l = 0x02, alt_l = 0x04, cmd_l = 0x08,
                     ctrl_r = 0x10, shift_r = 0x20, alt_r = 0x40, cmd_r = 0x80 };

inline Usage lookup(const char* name)
{
    if (!name || !*name) return { none, 0 };
    if (!name[1]) {                              // single character
        char c = name[0];
        if (c >= 'a' && c <= 'z') return { key, (uint16_t)(0x04 + (c - 'a')) };
        if (c >= 'A' && c <= 'Z') return { key, (uint16_t)(0x04 + (c - 'A')) };
        if (c >= '1' && c <= '9') return { key, (uint16_t)(0x1E + (c - '1')) };
        if (c == '0') return { key, 0x27 };
        switch (c) {
        case '-': return { key, 0x2D }; case '=': return { key, 0x2E }; case '[': return { key, 0x2F };
        case ']': return { key, 0x30 }; case '\\': return { key, 0x31 }; case ';': return { key, 0x33 };
        case '\'': return { key, 0x34 }; case '`': return { key, 0x35 }; case ',': return { key, 0x36 };
        case '.': return { key, 0x37 }; case '/': return { key, 0x38 }; case ' ': return { key, 0x2C };
        }
        return { none, 0 };
    }
    struct E { const char* n; Usage u; };
    static const E table[] = {
        { "space", { key, 0x2C } }, { "enter", { key, 0x28 } }, { "tab", { key, 0x2B } },
        { "backspace", { key, 0x2A } }, { "delete", { key, 0x4C } }, { "esc", { key, 0x29 } },
        { "up", { key, 0x52 } }, { "down", { key, 0x51 } }, { "left", { key, 0x50 } }, { "right", { key, 0x4F } },
        { "home", { key, 0x4A } }, { "end", { key, 0x4D } }, { "page_up", { key, 0x4B } }, { "page_down", { key, 0x4E } },
        { "caps_lock", { key, 0x39 } }, { "insert", { key, 0x49 } },
        { "cmd", { modifier, cmd_l } }, { "cmd_l", { modifier, cmd_l } }, { "cmd_r", { modifier, cmd_r } },
        { "ctrl", { modifier, ctrl_l } }, { "ctrl_l", { modifier, ctrl_l } }, { "ctrl_r", { modifier, ctrl_r } },
        { "alt", { modifier, alt_l } }, { "alt_l", { modifier, alt_l } }, { "alt_r", { modifier, alt_r } },
        { "shift", { modifier, shift_l } }, { "shift_l", { modifier, shift_l } }, { "shift_r", { modifier, shift_r } },
        { "media_play_pause", { consumer, 0xCD } }, { "media_next", { consumer, 0xB5 } },
        { "media_previous", { consumer, 0xB6 } }, { "media_volume_up", { consumer, 0xE9 } },
        { "media_volume_down", { consumer, 0xEA } }, { "media_volume_mute", { consumer, 0xE2 } },
    };
    for (auto& e : table) if (!strcmp(e.n, name)) return e.u;
    if (name[0] == 'f' && name[1] >= '1' && name[1] <= '9') {  // f1..f20
        int n = atoi(name + 1);
        if (n >= 1 && n <= 12) return { key, (uint16_t)(0x3A + n - 1) };
        if (n >= 13 && n <= 20) return { key, (uint16_t)(0x68 + n - 13) };
    }
    return { none, 0 };
}

// Parsed "[verb:]k1+k2+..." action.
struct Action {
    enum Verb : uint8_t { tap, hold, press, release } verb = tap;
    uint8_t  mods = 0;          // modifier bitmask
    uint16_t keys[6] = { 0 };   // keyboard usages
    uint8_t  nkeys = 0;
    uint16_t consumer = 0;      // at most one consumer usage per action
    bool     valid = false;
};

inline Action parse(const char* spec)
{
    Action a;
    if (!spec) return a;
    char buf[96]; strncpy(buf, spec, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    char* rest = buf;
    if (char* colon = strchr(buf, ':')) {
        *colon = 0;
        if (!strcmp(buf, "tap")) a.verb = Action::tap;
        else if (!strcmp(buf, "hold")) a.verb = Action::hold;
        else if (!strcmp(buf, "press")) a.verb = Action::press;
        else if (!strcmp(buf, "release")) a.verb = Action::release;
        else return a;
        rest = colon + 1;
    }
    bool any = false;
    for (char* tok = strtok(rest, "+"); tok; tok = strtok(nullptr, "+")) {
        while (*tok == ' ') tok++;
        Usage u = lookup(tok);
        if (u.kind == modifier) a.mods |= (uint8_t)u.code;
        else if (u.kind == key) { if (a.nkeys < 6) a.keys[a.nkeys++] = u.code; }
        else if (u.kind == consumer) a.consumer = u.code;
        else return a;                 // unknown name: whole action invalid
        any = true;
    }
    a.valid = any;
    return a;
}

} // namespace hidkeys
