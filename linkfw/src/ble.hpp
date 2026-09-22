// BLE: a real HID keyboard (+ consumer/media keys) and battery service for the
// OS, plus a custom "swlink" GATT service carrying the same JSON-lines protocol
// as USB and TCP (RX = host writes commands, TX = device notifies events).
// Device build only; host stubs report ble unavailable.
#pragma once
#include "link.hpp"

namespace ble {

// 128-bit UUIDs for the swlink service.
static constexpr const char* kServiceUuid = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
static constexpr const char* kRxUuid      = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";   // write
static constexpr const char* kTxUuid      = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";   // notify

void begin(const char* name);     // init stack, start advertising (idempotent)
void end();                       // stop (frees the stack; requires reboot to restart cleanly on Bluedroid)
void loop();
bool enabled();
bool connected();                 // any central connected
bool hidReady();                  // HID input reports subscribed by the OS (keys will be seen)
void setBattery(uint8_t pct);
void reportStatus(swl::Stream* only = nullptr);   // emit a "ble" message

// Keyboard: modifiers bitmask + up to 6 keys; call with zeros to release.
void keyReport(uint8_t mods, const uint16_t* keys, uint8_t nkeys);
void consumerReport(uint16_t usage);              // 0 = release

swl::Stream* sink();              // the GATT transport (nullptr when disabled)

} // namespace ble
