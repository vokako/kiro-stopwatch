#include "net.hpp"
#include "settings.hpp"
#include <M5Unified.h>
#include <cstring>

#if defined(ESP_PLATFORM)
#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_random.h>
#include <time.h>
#endif

namespace net {

static char s_mac[18] = "00:00:00:00:00:00";
const char* mac() { return s_mac; }

#if defined(ESP_PLATFORM)

static WiFiServer s_server(kTcpPort);
static WiFiClient s_client;
static bool       s_authed = false;
static uint32_t   s_clientSince = 0;
static bool       s_mdnsUp = false;
static bool       s_wasConnected = false;
static bool       s_rtcSynced = false;
static uint32_t   s_reconnectAt = 0;

struct TcpStream : swl::Stream {
    int available() override { return s_client ? s_client.available() : 0; }
    int read() override { return s_client ? s_client.read() : -1; }
    size_t readBytes(uint8_t* dst, size_t n, uint32_t timeout_ms) override {
        if (!s_client) return 0;
        s_client.setTimeout(timeout_ms);
        return s_client.readBytes(dst, n);
    }
    size_t write(const uint8_t* src, size_t n) override { return s_client ? s_client.write(src, n) : 0; }
    bool active() override { return (bool)s_client && s_client.connected(); }
    bool trusted() override { return s_authed; }
};
static TcpStream s_tcp;
swl::Stream* tcpSink() { return &s_tcp; }
bool tcpAuthed() { return s_authed && s_tcp.active(); }
bool tcpClientConnected() { return s_tcp.active(); }
bool connected() { return WiFi.status() == WL_CONNECTED; }

void tcpAuthenticate(const char* token)
{
    auto& st = settings::get();
    if (st.token[0] && token && !strcmp(token, st.token)) { s_authed = true; }
}

void generateToken()
{
    auto& st = settings::get();
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i += 8) {
        uint32_t r = esp_random();
        for (int j = 0; j < 8; j++) { st.token[i + j] = hex[r & 0xF]; r >>= 4; }
    }
    st.token[32] = 0;
    settings::save();
}

void reportStatus(swl::Stream* only)
{
    JsonDocument d; d["t"] = "wifi";
    auto& st = settings::get();
    if (!st.ssid[0]) d["ev"] = "off";
    else if (connected()) { d["ev"] = "connected"; d["ip"] = WiFi.localIP().toString(); d["rssi"] = WiFi.RSSI(); }
    else d["ev"] = s_reconnectAt ? "disconnected" : "connecting";
    d["ssid"] = st.ssid; d["mac"] = s_mac; d["port"] = kTcpPort; d["client"] = tcpClientConnected();
    swl::sendDoc(d, only);
}

static void connectSta()
{
    auto& st = settings::get();
    if (!st.ssid[0]) return;
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true);                     // modem sleep: keeps the link, saves ~60 mA
    WiFi.setHostname(st.name);
    WiFi.begin(st.ssid, st.psk);
    s_reconnectAt = 0;
    reportStatus();
}

void begin()
{
    uint8_t m[6]; WiFi.macAddress(m);
    snprintf(s_mac, sizeof s_mac, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    swl::addSink(&s_tcp);
    connectSta();
}

void setCredentials(const char* ssid, const char* psk)
{
    auto& st = settings::get();
    strncpy(st.ssid, ssid ? ssid : "", sizeof st.ssid - 1); st.ssid[sizeof st.ssid - 1] = 0;
    strncpy(st.psk, psk ? psk : "", sizeof st.psk - 1);     st.psk[sizeof st.psk - 1] = 0;
    settings::save();
    if (s_client) s_client.stop();
    WiFi.disconnect(true, true);
    s_wasConnected = false;
    connectSta();
}

void forget()
{
    auto& st = settings::get();
    st.ssid[0] = 0; st.psk[0] = 0; settings::save();
    if (s_client) s_client.stop();
    if (s_mdnsUp) { MDNS.end(); s_mdnsUp = false; }
    s_server.end();
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    s_wasConnected = false;
    reportStatus();
}

static void onConnected()
{
    s_server.begin(); s_server.setNoDelay(true);
    auto& st = settings::get();
    if (MDNS.begin(st.name)) {
        MDNS.addService("swlink", "tcp", kTcpPort);
        MDNS.addServiceTxt("swlink", "tcp", "mac", String(s_mac));
        MDNS.addServiceTxt("swlink", "tcp", "fw", String("swlink/0.2"));
        s_mdnsUp = true;
    }
    // SNTP -> RTC once per boot
    configTime(st.tz_min * 60, 0, "pool.ntp.org", "time.apple.com");
    s_rtcSynced = false;
    reportStatus();
}

static void onDisconnected()
{
    if (s_client) s_client.stop();
    if (s_mdnsUp) { MDNS.end(); s_mdnsUp = false; }
    s_server.end();
    s_reconnectAt = millis() + 5000;
    reportStatus();
}

void loop()
{
    auto& st = settings::get();
    if (!st.ssid[0]) return;
    bool now = connected();
    if (now && !s_wasConnected) onConnected();
    if (!now && s_wasConnected) onDisconnected();
    s_wasConnected = now;

    if (!now) {
        if (s_reconnectAt && (int32_t)(millis() - s_reconnectAt) >= 0) { WiFi.reconnect(); s_reconnectAt = millis() + 15000; }
        return;
    }

    if (!s_rtcSynced && M5.Rtc.isEnabled()) {
        struct tm tmv;
        if (getLocalTime(&tmv, 0)) { M5.Rtc.setDateTime(&tmv); s_rtcSynced = true; swl::sendLog("rtc synced from sntp"); }
    }

    // accept one client; a newcomer replaces a dead one
    if (s_server.hasClient()) {
        WiFiClient c = s_server.available();
        if (s_client && s_client.connected()) { c.stop(); }
        else { s_client = c; s_client.setNoDelay(true); s_authed = false; s_clientSince = millis(); }
    }
    if (s_client && s_client.connected() && !s_authed && millis() - s_clientSince > 3000) {
        swl::sendErr("hello", "auth timeout", &s_tcp);
        s_client.stop();
    }
}

#else  // ---------------------------------------------------------------- host stubs

void begin() {}
void loop() {}
void setCredentials(const char*, const char*) { swl::sendErr("wifi.set", "no wifi on host"); }
void forget() {}
void reportStatus(swl::Stream* only) { JsonDocument d; d["t"] = "wifi"; d["ev"] = "off"; d["mac"] = s_mac; swl::sendDoc(d, only); }
bool connected() { return false; }
bool tcpClientConnected() { return false; }
void generateToken() { strcpy(settings::get().token, "0123456789abcdef0123456789abcdef"); }
swl::Stream* tcpSink() { return nullptr; }
bool tcpAuthed() { return false; }
void tcpAuthenticate(const char*) {}

#endif

} // namespace net
