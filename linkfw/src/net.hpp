// Wi-Fi station + mDNS + one-client TCP transport + SNTP. Device build only;
// the simulator gets no-op stubs so the rest of the firmware compiles.
#pragma once
#include "link.hpp"

namespace net {

static constexpr uint16_t kTcpPort = 9899;

void begin();                    // connect if credentials are stored
void loop();                     // drive state machine; call every loop()
void setCredentials(const char* ssid, const char* psk);   // persist + reconnect
void forget();                   // erase credentials, disconnect
void reportStatus(swl::Stream* only = nullptr);           // emit a "wifi" message
bool connected();
bool tcpClientConnected();
const char* mac();               // "aa:bb:cc:dd:ee:ff"
void generateToken();            // new pairing token, persisted

// The TCP sink registered with swl::addSink(); authenticated per connection.
swl::Stream* tcpSink();
bool tcpAuthed();
void tcpAuthenticate(const char* token);   // called on hello{token}

} // namespace net
