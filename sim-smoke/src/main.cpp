// Smoke test: same sketch shape as device firmware (setup/loop, M5.update,
// BtnA/touch), rendered through M5GFX's SDL backend on the host.
#include <M5Unified.h>

static uint32_t taps = 0;

static void draw()
{
    auto& d = M5.Display;
    d.startWrite();
    d.fillScreen(TFT_BLACK);
    const int cx = d.width() / 2;
    const int cy = d.height() / 2;
    d.drawCircle(cx, cy, std::min(cx, cy) - 2, TFT_DARKGREY);
    d.setTextDatum(middle_center);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setTextSize(2);
    d.drawString("StopWatch sim", cx, cy - 20);
    d.setTextSize(1);
    d.drawString(("taps: " + std::to_string(taps)).c_str(), cx, cy + 20);
    d.endWrite();
}

void setup()
{
    auto cfg = M5.config();
    M5.begin(cfg);
    draw();
}

void loop()
{
    M5.update();
    bool changed = false;
    if (M5.BtnA.wasPressed()) { taps++; changed = true; }
    auto t = M5.Touch.getDetail();
    if (t.wasPressed()) { taps++; changed = true; }
    if (changed) draw();
    M5.delay(16);
}

#if defined(SDL_h_)
// Host entry point used by M5GFX's SDL backend (same shape as the official
// M5Unified PlatformIO_SDL example).
__attribute__((weak)) int user_func(bool* running)
{
    setup();
    do {
        loop();
    } while (*running);
    return 0;
}

int main(int, char**)
{
    return lgfx::Panel_sdl::main(user_func, 128);
}
#endif
