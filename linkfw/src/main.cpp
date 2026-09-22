// swlink firmware v0.2 — M5Stack StopWatch as a host peripheral.
// Transports: USB CDC (always) + TCP over Wi-Fi (net.cpp). Settings in NVS
// (settings.cpp). Power tiers: screen off after idle, light sleep on battery.
// Protocol: docs/design-docs/swlink-architecture.md
#include <M5Unified.h>
#include <cstring>
#include <ctime>
#include <cmath>
#include "link.hpp"
#include "settings.hpp"
#include "net.hpp"
#include "ble.hpp"
#include "keymap.hpp"
#include "adpcm.hpp"
#include "face.hpp"

static constexpr const char* kFwVersion = "swlink/0.2";

// ---------------------------------------------------------------------------
// USB / stdio stream per target
// ---------------------------------------------------------------------------
#if defined(ARDUINO)
#include <esp_sleep.h>
struct UsbStream : swl::Stream {
    int available() override { return Serial.available(); }
    int read() override { return Serial.read(); }
    size_t readBytes(uint8_t* dst, size_t n, uint32_t timeout_ms) override {
        Serial.setTimeout(timeout_ms);
        return Serial.readBytes(reinterpret_cast<char*>(dst), n);
    }
    size_t write(const uint8_t* src, size_t n) override { return Serial.write(src, n); }
};
static UsbStream s_usb;
static bool usbHostPresent() { return Serial.isConnected(); }   // DTR asserted by a host program
uint32_t swl::nowMs() { return millis(); }
#else
#include <cstdio>
#include <string>
#include <sys/select.h>
#include <unistd.h>
using lgfx::millis;
struct UsbStream : swl::Stream {
    std::string pending;
    void pump(uint32_t wait_ms) {
        fd_set r; FD_ZERO(&r); FD_SET(0, &r);
        timeval tv{ (time_t)(wait_ms / 1000), (suseconds_t)((wait_ms % 1000) * 1000) };
        if (select(1, &r, nullptr, nullptr, &tv) > 0) {
            char buf[1024]; ssize_t n = ::read(0, buf, sizeof buf);
            if (n > 0) pending.append(buf, (size_t)n);
        }
    }
    int available() override { pump(0); return (int)pending.size(); }
    int read() override { if (pending.empty()) return -1; int c = (unsigned char)pending[0]; pending.erase(0, 1); return c; }
    size_t readBytes(uint8_t* dst, size_t n, uint32_t timeout_ms) override {
        uint32_t end = millis() + timeout_ms;
        while (pending.size() < n && millis() < end) pump(10);
        size_t got = pending.size() < n ? pending.size() : n;
        memcpy(dst, pending.data(), got); pending.erase(0, got);
        return got;
    }
    size_t write(const uint8_t* src, size_t n) override { size_t w = fwrite(src, 1, n, stdout); fflush(stdout); return w; }
};
static UsbStream s_usb;
static bool usbHostPresent() { return true; }
uint32_t swl::nowMs() { return millis(); }
#endif

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static swl::LineReader s_usbReader(&s_usb);
static swl::LineReader* s_tcpReader = nullptr;
static swl::LineReader* s_bleReader = nullptr;
static bool     s_bleWasConnected = false;
static bool     s_hostSeen = false;
static uint32_t s_imuNext = 0, s_powerNext = 0, s_hapticOff = 0;
static bool     s_micOn = false;
static uint32_t s_micRate = 16000, s_micSeq = 0;
static int16_t  s_micBuf[2][1024];
static uint8_t  s_adpcmBuf[2][516];
static int      s_micCur = 0;
static uint8_t  s_spkBuf[2][4096];
static int      s_spkCur = 0;
static int      s_touchX = -1, s_touchY = -1;
static char     s_line[3][40] = { "", "", "" };
static char     s_lastEvent[40] = "-";
static bool     s_dirty = true;
static uint32_t s_drawNext = 0;
static int      s_bat = -1; static bool s_chg = false;
static uint32_t s_lastActivity = 0;
static bool     s_screenOn = true;

static void touchActivity() { s_lastActivity = swl::nowMs(); }

// ---------------------------------------------------------------------------
// Emitters
// ---------------------------------------------------------------------------
static void sendHello(swl::Stream* only = nullptr)
{
    JsonDocument d;
    d["t"] = "hello"; d["fw"] = kFwVersion; d["board"] = "M5StopWatch";
    d["name"] = settings::get().name; d["mac"] = net::mac();
    d["w"] = 466; d["h"] = 466;
    d["auth"] = only ? only->trusted() : true;
    JsonArray caps = d["caps"].to<JsonArray>();
    caps.add("btn"); caps.add("btn_pwr"); caps.add("touch"); caps.add("display"); caps.add("haptic");
    caps.add("cfg"); caps.add("power_tiers"); caps.add("keys");
    d["hid"] = ble::hidReady();                 // true => device executes button mappings itself
    if (M5.Imu.isEnabled())     caps.add("imu");
    if (M5.Rtc.isEnabled())     caps.add("rtc");
    caps.add("power");
    if (M5.Speaker.isEnabled()) { caps.add("tone"); caps.add("spk"); }
    if (M5.Mic.isEnabled())     caps.add("mic");
#if defined(ESP_PLATFORM)
    caps.add("wifi"); caps.add("ble");
#endif
    swl::sendDoc(d, only);
}

static void sendCfg(swl::Stream* only = nullptr) { JsonDocument d; settings::toJson(d); swl::sendDoc(d, only); }

static void sendBtn(const char* id, const char* ev)
{
    JsonDocument d; d["t"] = "btn"; d["id"] = id; d["ev"] = ev; d["ms"] = swl::nowMs();
    swl::sendDoc(d);
    if (strcmp(ev, "click")) {                       // decided events only, same rule as the host
        char trig[16]; snprintf(trig, sizeof trig, "%s.%s", id, ev);
        keymap::onTrigger(trig);
    }
    snprintf(s_lastEvent, sizeof s_lastEvent, "btn %s %s", id, ev); s_dirty = true;
    touchActivity();
}

static void sendTouch(const char* ev, int x, int y)
{
    JsonDocument d; d["t"] = "touch"; d["ev"] = ev; d["x"] = x; d["y"] = y; d["ms"] = swl::nowMs();
    swl::sendDoc(d);
    if (strcmp(ev, "move")) { snprintf(s_lastEvent, sizeof s_lastEvent, "touch %s %d,%d", ev, x, y); s_dirty = true; }
    touchActivity();
}

static void sendPower(swl::Stream* only = nullptr)
{
    int bat = M5.Power.getBatteryLevel();
    bool chg = (M5.Power.isCharging() == m5::Power_Class::is_charging);
    if (bat != s_bat || chg != s_chg) s_dirty = true;
    s_bat = bat; s_chg = chg;
    if (bat >= 0) ble::setBattery((uint8_t)bat);
    JsonDocument d; d["t"] = "power"; d["bat"] = s_bat; d["chg"] = s_chg;
    d["mv"] = M5.Power.getBatteryVoltage(); d["usb"] = usbHostPresent(); d["ms"] = swl::nowMs();
    swl::sendDoc(d, only);
}

static void sendImu()
{
    float ax, ay, az, gx, gy, gz;
    if (!M5.Imu.getAccel(&ax, &ay, &az)) return;
    M5.Imu.getGyro(&gx, &gy, &gz);
    JsonDocument d; d["t"] = "imu";
    d["ax"] = ax; d["ay"] = ay; d["az"] = az; d["gx"] = gx; d["gy"] = gy; d["gz"] = gz;
    d["ms"] = swl::nowMs();
    swl::sendDoc(d);
}

static void sendRtc(swl::Stream* only)
{
    if (!M5.Rtc.isEnabled()) { swl::sendErr("rtc.get", "no rtc", only); return; }
    m5::rtc_datetime_t dt; M5.Rtc.getDateTime(&dt);
    char iso[24];
    snprintf(iso, sizeof iso, "%04d-%02d-%02dT%02d:%02d:%02d",
             dt.date.year, dt.date.month, dt.date.date, dt.time.hours, dt.time.minutes, dt.time.seconds);
    JsonDocument d; d["t"] = "rtc"; d["iso"] = iso;
    swl::sendDoc(d, only);
}

static void sendScreen() { JsonDocument d; d["t"] = "screen"; d["on"] = s_screenOn; swl::sendDoc(d); }

// ---------------------------------------------------------------------------
// Mic / speaker (share the ES8311 codec: one at a time)
// ---------------------------------------------------------------------------
#if !defined(ESP_PLATFORM)
static void micStart(uint32_t, swl::Stream* o) { swl::sendErr("mic.start", "no mic on host", o); }
static void micStop(bool ack, swl::Stream* o) { if (ack) swl::sendAck("mic.stop", "already", o); }
static void micPump() {}
#else
static void micStart(uint32_t rate, swl::Stream* o)
{
    if (!M5.Mic.isEnabled()) { swl::sendErr("mic.start", "no mic", o); return; }
    if (s_micOn) { swl::sendAck("mic.start", "already", o); return; }
    M5.Speaker.end();
    auto cfg = M5.Mic.config(); cfg.sample_rate = rate; M5.Mic.config(cfg);
    if (!M5.Mic.begin()) { M5.Speaker.begin(); swl::sendErr("mic.start", "begin failed", o); return; }
    s_micRate = rate; s_micSeq = 0; s_micCur = 0; s_micOn = true;
    M5.Mic.record(s_micBuf[0], 1024, rate);
    M5.Mic.record(s_micBuf[1], 1024, rate);
    face::set(face::LISTENING, 0, "mic");            // held until mic.stop
    swl::sendAck("mic.start", nullptr, o);
    strcpy(s_lastEvent, "mic streaming"); s_dirty = true;
}

static void micStop(bool ack, swl::Stream* o)
{
    if (!s_micOn) { if (ack) swl::sendAck("mic.stop", "already", o); return; }
    s_micOn = false;
    M5.Mic.end();
    M5.Speaker.begin();
    face::release("mic");
    if (ack) swl::sendAck("mic.stop", nullptr, o);
    strcpy(s_lastEvent, "mic stopped"); s_dirty = true;
}

static void micPump()
{
    if (!s_micOn) return;
    while (M5.Mic.isRecording() < 2) {
        {   // level for the listening animation: RMS of this block, ~-40 dBFS..0 mapped to 0..1
            uint64_t acc = 0; for (int i = 0; i < 1024; i += 4) { int v = s_micBuf[s_micCur][i]; acc += (uint64_t)(v * v); }
            float rms = sqrtf((float)acc / 256.0f) / 32768.0f;
            float db = rms > 1e-5f ? 20.0f * log10f(rms) : -100.0f;
            face::setLevel((db + 40.0f) / 40.0f);
        }
        JsonDocument h; h["t"] = "mic"; h["seq"] = s_micSeq++; h["rate"] = s_micRate;
        if (ble::connected()) {
            size_t n = adpcm::encode(s_micBuf[s_micCur], 1024, s_adpcmBuf[s_micCur]);
            h["n"] = n; h["samples"] = 1024; h["fmt"] = "ima-adpcm";
            swl::sendChunk(h, s_adpcmBuf[s_micCur], n);
        } else {
            h["n"] = 2048; h["fmt"] = "s16le";
            swl::sendChunk(h, reinterpret_cast<const uint8_t*>(s_micBuf[s_micCur]), 2048);
        }
        M5.Mic.record(s_micBuf[s_micCur], 1024, s_micRate);
        s_micCur ^= 1;
    }
}
#endif

static void spkPcm(size_t n, uint32_t rate, swl::Stream* src)
{
    if (s_micOn) { swl::sendErr("spk.pcm", "mic active", src); return; }
    if (n == 0 || n > sizeof s_spkBuf[0] || (n & 1)) { swl::sendErr("spk.pcm", "bad n", src); return; }
    uint8_t* buf = s_spkBuf[s_spkCur];
    size_t got = src->readBytes(buf, n, 1000);
    if (got != n) { swl::sendErr("spk.pcm", "short read", src); return; }
    uint32_t end = swl::nowMs() + 500;
    while (M5.Speaker.isPlaying(0) >= 2 && swl::nowMs() < end) M5.delay(1);
    M5.Speaker.playRaw(reinterpret_cast<const int16_t*>(buf), n / 2, rate, false, 1, 0, false);
    s_spkCur ^= 1;
    swl::sendAck("spk.pcm", nullptr, src);
}

// ---------------------------------------------------------------------------
// Power tiers
// ---------------------------------------------------------------------------
static void screenSet(bool on)
{
    if (on == s_screenOn) return;
    s_screenOn = on;
    if (on) { M5.Display.wakeup(); M5.Display.setBrightness(settings::get().brightness); s_dirty = true; }
    else    { M5.Display.sleep(); }
    sendScreen();
}

static void applySettings()
{
    auto& st = settings::get();
    if (s_screenOn) M5.Display.setBrightness(st.brightness);
}

static void powerTiers()
{
    auto& st = settings::get();
    uint32_t idle = swl::nowMs() - s_lastActivity;
    if (st.screen_off_s && s_screenOn && idle > st.screen_off_s * 1000UL) screenSet(false);

#if defined(ESP_PLATFORM)
    // Tier 2: light sleep only when nobody could notice the link dropping.
    if (!s_screenOn && st.allow_sleep && st.sleep_after_s && !usbHostPresent() && !net::connected() && !ble::connected() && !s_micOn
        && idle > (st.screen_off_s + st.sleep_after_s) * 1000UL) {
        // A = GPIO2, B = GPIO1 (active low), PWR = M5PM1 IRQ on GPIO12 (active low)
        esp_sleep_enable_ext1_wakeup((1ULL << 1) | (1ULL << 2) | (1ULL << 12), ESP_EXT1_WAKEUP_ANY_LOW);
        esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
        esp_light_sleep_start();
        esp_sleep_wakeup_cause_t why = esp_sleep_get_wakeup_cause();
        if (why == ESP_SLEEP_WAKEUP_EXT1) { touchActivity(); screenSet(true); }
        else { s_powerNext = 0; }             // timer: report power, loop decides whether to sleep again
    }
#endif
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
static void handle(JsonDocument& d, swl::Stream* src)
{
    const char* t = d["t"];
    // Unauthenticated TCP clients may only say hello.
    if (!strcmp(t, "hello")) {
        if (!src->trusted()) net::tcpAuthenticate(d["token"] | "");
        s_hostSeen = true; s_dirty = true; touchActivity();
        sendHello(src);
        if (src->trusted()) { sendCfg(src); sendPower(src); }
        return;
    }
    if (!src->trusted()) { swl::sendErr(t, "not authenticated", src); return; }
    touchActivity();
    if (s_hostSeen && !s_screenOn && strcmp(t, "power.get") && strcmp(t, "cfg.get") && strcmp(t, "wifi.status")) screenSet(true);

    if (!strcmp(t, "cfg.get"))  { sendCfg(src); return; }
    if (!strcmp(t, "keys.get")) { JsonDocument k; keymap::toJson(k); swl::sendDoc(k, src); return; }
    if (!strcmp(t, "keys.set")) {
        const char* err = nullptr;
        if (!keymap::set(d["keys"], &err)) { swl::sendErr("keys.set", err, src); return; }
        if (!d["moods"].isNull() && !keymap::setMoods(d["moods"], &err)) { swl::sendErr("keys.set", err, src); return; }
        char msg[32]; snprintf(msg, sizeof msg, "%d mappings", keymap::count());
        swl::sendAck("keys.set", msg, src); JsonDocument k; keymap::toJson(k); swl::sendDoc(k); return;
    }
    if (!strcmp(t, "ble.status")) { ble::reportStatus(src); return; }
    if (!strcmp(t, "mood.set")) {
        int m = face::fromName(d["mood"] | "");
        if (m < 0) { swl::sendErr("mood.set", "unknown mood", src); return; }
        int ms = d["ms"] | 0;                       // 0 = stay until the next mood.set / base change
        if (ms > 0) face::set((face::Mood)m, (uint32_t)ms); else face::setBase((face::Mood)m);
        swl::sendAck("mood.set", face::name((face::Mood)m), src); return;
    }
    if (!strcmp(t, "trigger")) {                     // host-derived trigger (imu.*): apply its mood
        keymap::applyMood(d["name"] | ""); return;
    }
    if (!strcmp(t, "cfg.set")) {
        const char* err = nullptr;
        if (!settings::applyPatch(d.as<JsonVariantConst>(), &err)) { swl::sendErr("cfg.set", err, src); return; }
        applySettings(); swl::sendAck("cfg.set", nullptr, src); sendCfg();
        if (settings::get().ble && !ble::enabled()) ble::begin(settings::get().name);
        if (!settings::get().ble && ble::enabled()) swl::sendLog("ble disabled: takes effect after reboot");
        return;
    }
    if (!strcmp(t, "imu.rate")) {          // legacy alias for cfg.set imu_hz (not persisted)
        int hz = d["hz"] | 20; if (hz < 0) hz = 0; if (hz > 100) hz = 100;
        settings::get().imu_hz = hz; swl::sendAck("imu.rate", nullptr, src); return;
    }
    if (!strcmp(t, "power.get")) { sendPower(src); swl::sendAck("power.get", nullptr, src); return; }
    if (!strcmp(t, "rtc.get"))   { sendRtc(src); return; }
    if (!strcmp(t, "rtc.set")) {
        if (!M5.Rtc.isEnabled()) { swl::sendErr("rtc.set", "no rtc", src); return; }
        const char* iso = d["iso"] | "";
        int Y, M, D, h, m, sec;
        if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6) { swl::sendErr("rtc.set", "iso YYYY-MM-DDThh:mm:ss", src); return; }
        m5::rtc_datetime_t dt;
        dt.date.year = Y; dt.date.month = M; dt.date.date = D;
        dt.time.hours = h; dt.time.minutes = m; dt.time.seconds = sec;
        M5.Rtc.setDateTime(&dt); swl::sendAck("rtc.set", nullptr, src); sendRtc(src); return;
    }
    if (!strcmp(t, "haptic")) {
        int level = d["level"] | 128; int ms = d["ms"] | 80;
        M5.Power.setVibration((uint8_t)level); s_hapticOff = swl::nowMs() + ms;
        swl::sendAck("haptic", nullptr, src); return;
    }
    if (!strcmp(t, "tone")) {
        if (s_micOn) { swl::sendErr("tone", "mic active", src); return; }
        if (!M5.Speaker.isEnabled()) { swl::sendErr("tone", "no speaker", src); return; }
        float hz = d["hz"] | 880.0f; int ms = d["ms"] | 120;
        M5.Speaker.tone(hz, ms); swl::sendAck("tone", nullptr, src); return;
    }
    if (!strcmp(t, "mic.start")) {
        micStart(d["rate"] | 16000, src); return;
    }
    if (!strcmp(t, "mic.stop"))  { micStop(true, src); return; }
    if (!strcmp(t, "spk.pcm"))   { spkPcm(d["n"] | 0, d["rate"] | 16000, src); return; }
    if (!strcmp(t, "display.text")) {
        int line = d["line"] | 0; if (line < 0 || line > 2) { swl::sendErr("display.text", "line 0-2", src); return; }
        const char* text = d["text"] | "";
        strncpy(s_line[line], text, sizeof s_line[line] - 1); s_line[line][sizeof s_line[line] - 1] = 0;
        s_dirty = true; swl::sendAck("display.text", nullptr, src); return;
    }
    if (!strcmp(t, "display.brightness")) {   // transient; cfg.set brightness persists
        int v = d["v"] | 128; M5.Display.setBrightness((uint8_t)v); swl::sendAck("display.brightness", nullptr, src); return;
    }
    if (!strcmp(t, "wifi.set")) {
        const char* ssid = d["ssid"] | "";
        if (!*ssid) { swl::sendErr("wifi.set", "ssid required", src); return; }
        swl::sendAck("wifi.set", nullptr, src);
        net::setCredentials(ssid, d["psk"] | ""); sendCfg(); return;
    }
    if (!strcmp(t, "wifi.forget")) { net::forget(); swl::sendAck("wifi.forget", nullptr, src); sendCfg(); return; }
    if (!strcmp(t, "wifi.status")) { net::reportStatus(src); return; }
    if (!strcmp(t, "net.pair")) {
        if (src != &s_usb) { swl::sendErr("net.pair", "usb only", src); return; }
        net::generateToken();
        JsonDocument r; r["t"] = "pair"; r["token"] = settings::get().token; swl::sendDoc(r, src);
        sendCfg(); return;
    }
    swl::sendErr(t, "unknown command", src);
}

// ---------------------------------------------------------------------------
// Display: Kiro face + footer
// ---------------------------------------------------------------------------
static void draw()
{
    char link[64]; link[0] = 0;
    if (usbHostPresent()) strlcat(link, "usb ", sizeof link);
    if (ble::connected()) strlcat(link, "ble ", sizeof link); else if (ble::enabled()) strlcat(link, "ble-adv ", sizeof link);
    if (net::connected()) strlcat(link, "wifi ", sizeof link);
    if (!link[0]) strcpy(link, "no host");
    char foot[64];
    if (s_bat >= 0) snprintf(foot, sizeof foot, "%s| bat %d%%%s", link, s_bat, s_chg ? "+" : "");
    else            snprintf(foot, sizeof foot, "%s", link);
    // display.text line 1 (if any) replaces the event line; line 0/2 are reserved for apps later
    const char* top = s_line[1][0] ? s_line[1] : s_lastEvent;
    face::tick(top, foot, s_dirty);
    s_dirty = false;
}

// ---------------------------------------------------------------------------
void setup()
{
    auto cfg = M5.config();
#if defined(ARDUINO)
    cfg.serial_baudrate = 115200;
#endif
    M5.begin(cfg);
    settings::load();
    M5.Display.setBrightness(settings::get().brightness);
    M5.Speaker.setVolume(96);
    swl::addSink(&s_usb);
    net::begin();
    if (net::tcpSink()) s_tcpReader = new swl::LineReader(net::tcpSink());
    keymap::load();
    if (settings::get().ble) ble::begin(settings::get().name);
    if (ble::sink()) { swl::addSink(ble::sink()); s_bleReader = new swl::LineReader(ble::sink()); }
    face::begin();
    s_lastActivity = swl::nowMs();
    sendHello();
    sendCfg();
    sendPower();
    s_powerNext = swl::nowMs() + 5000;
}

void loop()
{
    M5.update();
    net::loop();
    ble::loop();
    const uint32_t now = swl::nowMs();
    if (ble::connected() != s_bleWasConnected) {
        s_bleWasConnected = ble::connected();
        if (!s_bleWasConnected) keymap::releaseAll();
        touchActivity(); s_dirty = true;
    }

    // commands from any transport
    JsonDocument doc;
    while (s_usbReader.poll(doc)) handle(doc, &s_usb);
    if (s_tcpReader) while (s_tcpReader->poll(doc)) handle(doc, s_tcpReader->stream());
    if (s_bleReader) while (s_bleReader->poll(doc)) handle(doc, s_bleReader->stream());

    // buttons
    struct { m5::Button_Class& b; const char* id; } btns[] = { { M5.BtnA, "A" }, { M5.BtnB, "B" }, { M5.BtnPWR, "PWR" } };
    for (auto& e : btns) {
        if (e.b.wasPressed())  { if (!s_screenOn) screenSet(true); sendBtn(e.id, "down"); }
        if (e.b.wasReleased()) sendBtn(e.id, "up");
        if (e.b.wasClicked())  { if (!s_screenOn) screenSet(true); sendBtn(e.id, "click"); }
        if (e.b.wasHold())     sendBtn(e.id, "hold");
        if (e.b.wasDecideClickCount()) {
            int n = e.b.getClickCount();
            sendBtn(e.id, n == 1 ? "single" : n == 2 ? "double" : "triple");
        }
    }

    // touch
    auto t = M5.Touch.getDetail();
    if (t.wasPressed())       { s_touchX = t.x; s_touchY = t.y; if (!s_screenOn) screenSet(true); sendTouch("down", t.x, t.y); }
    else if (t.isPressed() && (t.x != s_touchX || t.y != s_touchY)) { s_touchX = t.x; s_touchY = t.y; sendTouch("move", t.x, t.y); }
    if (t.wasReleased())      { sendTouch("up", t.x, t.y); s_touchX = s_touchY = -1; }

    // periodic sensors
    auto& st = settings::get();
    if (st.imu_hz && M5.Imu.isEnabled() && (int32_t)(now - s_imuNext) >= 0) {
        s_imuNext = now + 1000 / st.imu_hz;
        if (M5.Imu.update()) sendImu();
    }
    if ((int32_t)(now - s_powerNext) >= 0) { s_powerNext = now + (s_screenOn ? 5000 : 30000); sendPower(); }
    if (s_hapticOff && (int32_t)(now - s_hapticOff) >= 0) { M5.Power.setVibration(0); s_hapticOff = 0; }
    micPump();

    // base mood follows connection: someone listening -> idle, nobody -> sleep
    face::setBase((s_hostSeen && (usbHostPresent() || ble::connected() || net::tcpClientConnected())) ? face::IDLE : face::SLEEP);
    if (s_screenOn) draw();

    powerTiers();
    M5.delay(s_micOn ? 1 : (s_screenOn ? 5 : 20));
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
