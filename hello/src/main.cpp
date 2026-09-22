// Hello World for M5Stack StopWatch.
// Screen: greeting, uptime counter, button/touch feedback.
// Serial (USB CDC, 115200): one heartbeat line per second plus input events,
// so the host can watch the device without looking at the display.
#include <M5Unified.h>

#if !defined(ARDUINO)
// Desktop simulator has no Arduino Serial; route it to stdout.
#include <cstdio>
struct HostSerial {
    void println() { std::puts(""); }
    void println(const char* s) { std::puts(s); }
    template <typename... A> void printf(const char* f, A... a) { std::printf(f, a...); std::fflush(stdout); }
};
static HostSerial Serial;
using lgfx::millis;
#endif

static uint32_t presses = 0;
static uint32_t lastHeartbeat = 0;

// Physical visible area of the round AMOLED (CO5300, 1.75"). M5GFX reports
// 468x468 on the device (even-aligned frame buffer, offset_x = 6) but 466x466
// in the SDL simulator; deriving the centre from width()/2 therefore drifts by
// 1 px and pushes the outline off the visible circle on hardware. Anchor to
// the panel geometry instead so both targets draw the same picture.
static constexpr int kScreenDiameter = 466;
static constexpr int kCenterX = kScreenDiameter / 2;   // 233
static constexpr int kCenterY = kScreenDiameter / 2;   // 233
static constexpr int kOutlineRadius = kCenterX - 3;    // 230, 3 px inside the glass edge

static void drawScreen()
{
    auto& d = M5.Display;
    const int cx = kCenterX;
    const int cy = kCenterY;

    d.startWrite();
    d.fillScreen(TFT_BLACK);
    d.drawCircle(cx, cy, kOutlineRadius, TFT_DARKGREY);

    d.setTextDatum(middle_center);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setFont(&fonts::FreeSansBold24pt7b);
    d.drawString("Hello, World!", cx, cy - 70);

    d.setFont(&fonts::FreeSans12pt7b);
    d.setTextColor(TFT_CYAN, TFT_BLACK);
    d.drawString("M5Stack StopWatch", cx, cy - 20);

    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    char line[48];
    snprintf(line, sizeof line, "uptime %lus", (unsigned long)(millis() / 1000));
    d.drawString(line, cx, cy + 30);
    snprintf(line, sizeof line, "presses %lu", (unsigned long)presses);
    d.drawString(line, cx, cy + 65);

    d.setFont(&fonts::Font0);
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.drawString("A/B or touch to count", cx, cy + 120);
    d.endWrite();
}

void setup()
{
    auto cfg = M5.config();
#if defined(ARDUINO)
    cfg.serial_baudrate = 115200;   // 0 (default) means M5Unified never calls Serial.begin()
#endif
    M5.begin(cfg);
    M5.Display.setBrightness(128);

    Serial.println();
    Serial.println("[hello] boot");
    Serial.printf("[hello] board=%d display=%dx%d\n",
                  (int)M5.getBoard(), M5.Display.width(), M5.Display.height());
    drawScreen();
}

void loop()
{
    M5.update();
    bool changed = false;

    if (M5.BtnA.wasPressed()) { presses++; changed = true; Serial.println("[hello] BtnA pressed"); }
    if (M5.BtnB.wasPressed()) { presses++; changed = true; Serial.println("[hello] BtnB pressed"); }
    auto t = M5.Touch.getDetail();
    if (t.wasPressed()) {
        presses++; changed = true;
        Serial.printf("[hello] touch x=%d y=%d\n", t.x, t.y);
    }

    uint32_t now = millis();
    if (now - lastHeartbeat >= 1000) {
        lastHeartbeat = now;
        changed = true;
        Serial.printf("[hello] heartbeat uptime=%lus presses=%lu heap=%u\n",
                      (unsigned long)(now / 1000), (unsigned long)presses,
#if defined(ESP_PLATFORM)
                      (unsigned)ESP.getFreeHeap()
#else
                      0u
#endif
        );
    }

    if (changed) drawScreen();
    M5.delay(10);
}

#if defined(SDL_h_)
// Host entry point for the M5GFX SDL backend (desktop simulation).
__attribute__((weak)) int user_func(bool* running)
{
    setup();
    do { loop(); } while (*running);
    return 0;
}

int main(int, char**)
{
    return lgfx::Panel_sdl::main(user_func, 128);
}
#endif
