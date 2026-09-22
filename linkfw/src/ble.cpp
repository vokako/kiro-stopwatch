#include "ble.hpp"
#include <cstring>

#if defined(ESP_PLATFORM)
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEHIDDevice.h>
#include <BLE2902.h>
#include <BLESecurity.h>
#include <BLEUtils.h>
#include <esp_bt_device.h>

namespace ble {

// Report IDs
static constexpr uint8_t kKbdReport = 1, kConsumerReport = 2;

// Boot-protocol keyboard (8 bytes) + 16-bit consumer usage report.
static const uint8_t kReportMap[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, kKbdReport,
      0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,   // modifiers
      0x95, 0x01, 0x75, 0x08, 0x81, 0x01,                                                              // reserved
      0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,                          // LEDs
      0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
      0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x05, 0x07, 0x19, 0x00, 0x2A, 0xFF, 0x00, 0x81, 0x00, // 6 keys
    0xC0,
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, kConsumerReport,
      0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xC0,
};

static BLEServer*         s_server = nullptr;
static BLEHIDDevice*      s_hid = nullptr;
static BLECharacteristic* s_kbd = nullptr;
static BLECharacteristic* s_consumer = nullptr;
static BLECharacteristic* s_tx = nullptr;
static BLECharacteristic* s_rx = nullptr;
static bool     s_enabled = false, s_connected = false;
static uint32_t s_connectedAt = 0;
static char     s_name[24] = "StopWatch";
static uint8_t  s_battery = 100;
static bool     s_statusDirty = false;

// Incoming bytes from RX writes, drained by the LineReader on the main loop.
static uint8_t  s_rxBuf[1024];
static volatile size_t s_rxHead = 0, s_rxTail = 0;
static portMUX_TYPE s_rxMux = portMUX_INITIALIZER_UNLOCKED;

struct GattStream : swl::Stream {
    int available() override { return (int)((s_rxHead - s_rxTail) % sizeof s_rxBuf); }
    int read() override {
        if (s_rxHead == s_rxTail) return -1;
        portENTER_CRITICAL(&s_rxMux);
        uint8_t c = s_rxBuf[s_rxTail]; s_rxTail = (s_rxTail + 1) % sizeof s_rxBuf;
        portEXIT_CRITICAL(&s_rxMux);
        return c;
    }
    size_t readBytes(uint8_t* dst, size_t n, uint32_t timeout_ms) override {
        uint32_t end = millis() + timeout_ms; size_t got = 0;
        while (got < n && millis() < end) { int c = read(); if (c < 0) { delay(1); continue; } dst[got++] = (uint8_t)c; }
        return got;
    }
    size_t write(const uint8_t* src, size_t n) override {
        if (!s_connected || !s_tx) return 0;
        size_t mtu = BLEDevice::getMTU(); size_t chunk = mtu > 23 ? mtu - 3 : 20; if (chunk > 244) chunk = 244;
        for (size_t off = 0; off < n; off += chunk) {
            size_t len = n - off < chunk ? n - off : chunk;
            s_tx->setValue(const_cast<uint8_t*>(src) + off, len);
            s_tx->notify();
            if (off + len < n) delay(3);          // let the controller drain; keeps order on macOS
        }
        return n;
    }
    bool active() override { return s_connected; }
    bool trusted() override { return true; }      // bonded link: treat like USB
};
static GattStream s_stream;

struct ServerCb : BLEServerCallbacks {
    void onConnect(BLEServer*) override { s_connected = true; s_connectedAt = millis(); s_statusDirty = true; }
    void onDisconnect(BLEServer*) override {
        s_connected = false; s_statusDirty = true;
        BLEDevice::startAdvertising();
    }
};

struct RxCb : BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* c) override {
        std::string v = c->getValue();
        portENTER_CRITICAL(&s_rxMux);
        for (unsigned char ch : v) {
            size_t next = (s_rxHead + 1) % sizeof s_rxBuf;
            if (next == s_rxTail) break;          // full: drop
            s_rxBuf[s_rxHead] = ch; s_rxHead = next;
        }
        portEXIT_CRITICAL(&s_rxMux);
    }
};

void begin(const char* name)
{
    if (s_enabled) return;
    strncpy(s_name, name, sizeof s_name - 1);
    BLEDevice::init(s_name);
    BLEDevice::setMTU(247);
    s_server = BLEDevice::createServer();
    s_server->setCallbacks(new ServerCb());

    s_hid = new BLEHIDDevice(s_server);
    s_kbd = s_hid->inputReport(kKbdReport);
    s_hid->outputReport(kKbdReport);              // LED state from the host (ignored)
    s_consumer = s_hid->inputReport(kConsumerReport);
    s_hid->manufacturer()->setValue("M5Stack / swlink");
    s_hid->pnp(0x02, 0x303A, 0x4001, 0x0002);     // USB-IF sig, Espressif VID, our PID
    s_hid->hidInfo(0x00, 0x01);
    s_hid->reportMap(const_cast<uint8_t*>(kReportMap), sizeof kReportMap);
    s_hid->startServices();
    s_hid->setBatteryLevel(s_battery);

    BLEService* svc = s_server->createService(kServiceUuid);
    s_tx = svc->createCharacteristic(kTxUuid, BLECharacteristic::PROPERTY_NOTIFY);
    s_tx->addDescriptor(new BLE2902());
    s_rx = svc->createCharacteristic(kRxUuid, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    s_rx->setCallbacks(new RxCb());
    svc->start();

    // Bonding with Just Works: macOS pairs a keyboard without a passkey this way.
    BLESecurity* sec = new BLESecurity();
    sec->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
    sec->setCapability(ESP_IO_CAP_NONE);
    sec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

    BLEAdvertising* adv = BLEDevice::getAdvertising();
    adv->setAppearance(0x03C1);                   // HID keyboard
    adv->addServiceUUID(s_hid->hidService()->getUUID());
    adv->addServiceUUID(kServiceUuid);
    adv->setScanResponse(true);
    adv->start();
    s_enabled = true; s_statusDirty = true;
}

void end()
{
    if (!s_enabled) return;
    BLEDevice::deinit(true);
    s_enabled = false; s_connected = false; s_statusDirty = true;
}

void loop()
{
    if (s_statusDirty) { s_statusDirty = false; reportStatus(); }
}

bool enabled() { return s_enabled; }
bool connected() { return s_enabled && s_connected; }
bool hidReady() { return connected() && millis() - s_connectedAt > 500; }

void setBattery(uint8_t pct)
{
    if (pct == s_battery) return;
    s_battery = pct;
    if (s_hid) s_hid->setBatteryLevel(pct);
}

void reportStatus(swl::Stream* only)
{
    JsonDocument d; d["t"] = "ble";
    d["enabled"] = s_enabled; d["connected"] = connected(); d["name"] = s_name;
    if (s_enabled) {
        const uint8_t* m = esp_bt_dev_get_address();
        char mac[18]; snprintf(mac, sizeof mac, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
        d["mac"] = mac;
    }
    swl::sendDoc(d, only);
}

void keyReport(uint8_t mods, const uint16_t* keys, uint8_t nkeys)
{
    if (!connected() || !s_kbd) return;
    uint8_t r[8] = { mods, 0, 0, 0, 0, 0, 0, 0 };
    for (uint8_t i = 0; i < nkeys && i < 6; i++) r[2 + i] = (uint8_t)keys[i];
    s_kbd->setValue(r, sizeof r); s_kbd->notify();
}

void consumerReport(uint16_t usage)
{
    if (!connected() || !s_consumer) return;
    uint8_t r[2] = { (uint8_t)(usage & 0xFF), (uint8_t)(usage >> 8) };
    s_consumer->setValue(r, sizeof r); s_consumer->notify();
}

swl::Stream* sink() { return &s_stream; }

} // namespace ble

#else  // -------------------------------------------------------------- host stubs
namespace ble {
void begin(const char*) {}
void end() {}
void loop() {}
bool enabled() { return false; }
bool connected() { return false; }
bool hidReady() { return false; }
void setBattery(uint8_t) {}
void reportStatus(swl::Stream* only) { JsonDocument d; d["t"] = "ble"; d["enabled"] = false; d["connected"] = false; swl::sendDoc(d, only); }
void keyReport(uint8_t, const uint16_t*, uint8_t) {}
void consumerReport(uint16_t) {}
swl::Stream* sink() { return nullptr; }
}
#endif
