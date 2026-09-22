// Kiro Buddy for M5Stack StopWatch.
// Port of amoled-apps/components/app_buddy (ESP-IDF + LVGL) to M5Unified/M5GFX.
// The white Kiro ghost body is a baked RGB565 bitmap (kiro_body.c); the eyes,
// blink, gaze, mood-specific motion and decorations are drawn per frame into a
// sprite and pushed to the round display.
//
// Moods cycle with BtnA / BtnB / touch, or are set from the host over USB CDC
// with one JSON object per line:  {"type":"agent_state","state":"working"}
// Accepted states: sleep idle thinking working waiting question error celebrate
// (aliases: sleeping, running, stuck, hmm, done).
#include <M5Unified.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include "kiro_body.h"

#if !defined(ARDUINO)
#include <cstdio>
#include <string>
#include <sys/select.h>
#include <unistd.h>
// Desktop stand-in for Arduino Serial: prints to stdout, reads lines from stdin.
struct HostSerial {
    std::string pending;
    void println(const char* s) { std::puts(s); }
    template <typename... A> void printf(const char* f, A... a) { std::printf(f, a...); std::fflush(stdout); }
    int available() {
        fd_set r; FD_ZERO(&r); FD_SET(0, &r);
        timeval tv{0, 0};
        if (select(1, &r, nullptr, nullptr, &tv) > 0) {
            char buf[256]; ssize_t n = ::read(0, buf, sizeof buf);
            if (n > 0) pending.append(buf, (size_t)n);
        }
        return (int)pending.size();
    }
    int read() { if (pending.empty()) return -1; int c = (unsigned char)pending[0]; pending.erase(0, 1); return c; }
};
static HostSerial Serial;
using lgfx::millis;
#endif

// ---- screen geometry (physical, see AGENTS.md) ------------------------------
static constexpr int kCenterX = 233, kCenterY = 233;

// ---- sprite the character is composed in -----------------------------------
static constexpr int CANW = 320, CANH = 372;
static constexpr int BX = (CANW - KIRO_BODY_W) / 2;   // body origin inside sprite
static constexpr int BY = 40;
static M5Canvas canvas(&M5.Display);

// ---- moods -----------------------------------------------------------------
enum Mood { BS_SLEEP, BS_IDLE, BS_THINKING, BS_WORKING, BS_WAITING, BS_QUESTION, BS_ERROR, BS_CELEBRATE, BS_N };
static const char* kMoodName[BS_N] = { "sleeping", "idle", "thinking", "working", "waiting", "hmm?", "error", "celebrate!" };
static const char* kMoodKey[BS_N]  = { "sleep", "idle", "thinking", "working", "waiting", "question", "error", "celebrate" };

static int      s_state = BS_IDLE;
static uint32_t s_t0;
static uint32_t s_blink_start, s_next_blink, s_look_next;
static float    s_lx, s_ly, s_ltx, s_lty;      // gaze offset, smoothed towards target

static constexpr uint16_t C_EYE = TFT_BLACK;
static constexpr uint16_t C_BG  = TFT_BLACK;

static int moodFromKey(const char* k)
{
    for (int i = 0; i < BS_N; i++) if (!strcmp(k, kMoodKey[i])) return i;
    if (!strcmp(k, "sleeping")) return BS_SLEEP;
    if (!strcmp(k, "running"))  return BS_WORKING;
    if (!strcmp(k, "stuck"))    return BS_ERROR;
    if (!strcmp(k, "hmm"))      return BS_QUESTION;
    if (!strcmp(k, "done"))     return BS_CELEBRATE;
    return -1;
}

static void setState(int st)
{
    if (st < 0 || st >= BS_N || st == s_state) return;
    s_state = st;
    Serial.printf("[buddy] state=%s\n", kMoodKey[st]);
}

// ---- drawing helpers -------------------------------------------------------
static void seg(int x1, int y1, int x2, int y2, int w, uint16_t col)
{
    canvas.drawWideLine(x1, y1, x2, y2, w, col);
}

// Non-open eye shapes: 1 = X, 2 = ^ (happy), 3 = closed arc (sleep)
static void drawEyeShape(int cx, int cy, int style)
{
    switch (style) {
    case 1: seg(cx-12, cy-12, cx+12, cy+12, 6, C_EYE); seg(cx-12, cy+12, cx+12, cy-12, 6, C_EYE); break;
    case 2: seg(cx-13, cy+7,  cx,    cy-9,  6, C_EYE); seg(cx,    cy-9,  cx+13, cy+7,  6, C_EYE); break;
    case 3: seg(cx-13, cy-4,  cx,    cy+5,  6, C_EYE); seg(cx,    cy+5,  cx+13, cy-4,  6, C_EYE); break;
    }
}

static void drawFrame(float t)
{
    float bob = sinf(t * 2.0f) * 6.0f;
    int   jitter = 0;
    float wsc = 1.0f, hsc = 1.0f, bias_x = 0, bias_y = 0;
    int   estyle = 0;        // 0 open ellipse, 1 X, 2 ^, 3 closed
    bool  wander = false, frown = false;

    switch (s_state) {
    case BS_SLEEP:     bob = sinf(t*0.9f)*4.0f; estyle = 3; break;
    case BS_IDLE:      wander = true; break;
    case BS_THINKING:  bob = sinf(t*1.3f)*4.0f; hsc = 0.9f; bias_x = -6; bias_y = -8; break;
    case BS_WORKING:   bob = sinf(t*3.0f)*4.0f; hsc = 0.8f; wsc = 1.02f; bias_y = 4; frown = true; break;
    case BS_WAITING:   bob = -fabsf(sinf(t*4.0f))*12.0f; hsc = 1.14f; wsc = 1.08f; wander = true; break;
    case BS_QUESTION:  bob = sinf(t*1.6f)*5.0f; hsc = 1.05f; bias_x = -7; bias_y = -6; break;
    case BS_ERROR:     jitter = (((int)(t*34)) & 1) ? 3 : -3; estyle = 1; break;
    case BS_CELEBRATE: bob = -fabsf(sinf(t*5.0f))*18.0f; estyle = 2; break;
    }

    const int dx = BX + jitter;
    const int dy = BY + (int)bob;

    canvas.fillScreen(C_BG);
    canvas.pushImage(dx, dy, KIRO_BODY_W, KIRO_BODY_H, kiro_body);

    // blink: 150 ms close/open
    uint32_t ms = millis();
    float open = 1.0f;
    if (ms - s_blink_start < 150) { float p = (ms - s_blink_start) / 150.0f; open = fabsf(p - 0.5f) * 2.0f; }

    float lookx = (wander ? s_lx : 0) + bias_x;
    float looky = (wander ? s_ly : 0) + bias_y;
    int lx = dx + KIRO_EYE_L_X + (int)lookx;
    int rx = dx + KIRO_EYE_R_X + (int)lookx;
    int ey = dy + KIRO_EYE_L_Y + (int)looky;

    if (estyle == 0) {
        int ew = (int)(KIRO_EYE_W * wsc);
        int eh = (int)(KIRO_EYE_H * hsc * open);
        if (eh < 3) eh = 3;
        canvas.fillEllipse(lx, ey, ew / 2, eh / 2, C_EYE);
        canvas.fillEllipse(rx, ey, ew / 2, eh / 2, C_EYE);
    } else {
        drawEyeShape(lx, ey, estyle);
        drawEyeShape(rx, ey, estyle);
    }
    if (frown) {   // inner ends low, outer ends high
        seg(lx-15, ey-20, lx+11, ey-11, 5, C_EYE);
        seg(rx-11, ey-11, rx+15, ey-20, 5, C_EYE);
    }
    if (s_state == BS_CELEBRATE) {   // orbiting stars
        int gcx = dx + KIRO_BODY_W/2, gcy = dy + KIRO_BODY_H/2;
        for (int i = 0; i < 7; i++) {
            float a = t*3.0f + i*0.9f;
            int sx = gcx + (int)(cosf(a)*(KIRO_BODY_W/2 + 12));
            int sy = gcy + (int)(sinf(a)*(KIRO_BODY_H/2 + 8));
            canvas.fillCircle(sx, sy, ((i + (int)(t*4)) & 1) ? 5 : 3, (i & 1) ? 0xFED9 : 0x67F8);
        }
    }

    // mood label at the bottom of the sprite
    canvas.setTextDatum(middle_center);
    canvas.setFont(&fonts::FreeSans12pt7b);
    canvas.setTextColor(TFT_LIGHTGREY, C_BG);
    canvas.drawString(kMoodName[s_state], CANW / 2, CANH - 22);

    canvas.pushSprite(kCenterX - CANW / 2, kCenterY - CANH / 2);
}

// ---- host link: one JSON object per line -----------------------------------
static void pollSerialLink()
{
    static char line[256];
    static size_t len = 0;
    while (Serial.available() > 0) {
        int c = Serial.read();
        if (c < 0) break;
        if (c == '\n' || c == '\r') {
            line[len] = 0;
            if (len) {
                const char* p = strstr(line, "\"state\"");
                if (p && (p = strchr(p + 7, '"'))) {
                    char key[32]; size_t n = 0; p++;
                    while (*p && *p != '"' && n < sizeof key - 1) key[n++] = *p++;
                    key[n] = 0;
                    int st = moodFromKey(key);
                    if (st < 0) Serial.printf("[buddy] unknown state '%s'\n", key);
                    else setState(st);
                }
            }
            len = 0;
        } else if (len < sizeof line - 1) {
            line[len++] = (char)c;
        } else {
            len = 0;   // overlong frame, drop it
        }
    }
}

void setup()
{
    auto cfg = M5.config();
#if defined(ARDUINO)
    cfg.serial_baudrate = 115200;
#endif
    M5.begin(cfg);
    M5.Display.setBrightness(128);
    M5.Display.fillScreen(C_BG);

    canvas.setColorDepth(16);
#if defined(ESP_PLATFORM)
    canvas.setPsram(true);                 // 320x372x2 = 238 KB, keep it out of internal RAM
#endif
    if (!canvas.createSprite(CANW, CANH)) {
        Serial.println("[buddy] sprite alloc failed");
    }

    s_t0 = millis();
    s_next_blink = s_t0 + 1400;
    s_look_next  = s_t0 + 900;
    Serial.println("[buddy] boot; send {\"type\":\"agent_state\",\"state\":\"working\"} to drive");
}

void loop()
{
    M5.update();
    pollSerialLink();

    if (M5.BtnA.wasPressed()) setState((s_state + 1) % BS_N);
    if (M5.BtnB.wasPressed()) setState((s_state + BS_N - 1) % BS_N);
    if (M5.Touch.getDetail().wasPressed()) setState((s_state + 1) % BS_N);

    uint32_t ms = millis();
    bool canBlink = (s_state == BS_IDLE || s_state == BS_WORKING || s_state == BS_WAITING ||
                     s_state == BS_THINKING || s_state == BS_QUESTION);
    if (canBlink && ms >= s_next_blink) { s_blink_start = ms; s_next_blink = ms + 2200 + (rand() % 2600); }

    if (ms >= s_look_next) {              // pick a new gaze target
        s_ltx = (float)(rand() % 21 - 10);
        s_lty = (float)(rand() % 13 - 6);
        s_look_next = ms + 900 + (rand() % 1600);
    }
    s_lx += (s_ltx - s_lx) * 0.15f;
    s_ly += (s_lty - s_ly) * 0.15f;

    drawFrame((ms - s_t0) / 1000.0f);
    M5.delay(33);
}

#if defined(SDL_h_)
__attribute__((weak)) int user_func(bool* running)
{
    setup();
    do { loop(); } while (*running);
    return 0;
}
int main(int, char**) { return lgfx::Panel_sdl::main(user_func, 128); }
#endif
