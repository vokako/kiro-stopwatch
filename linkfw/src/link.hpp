// swlink framing: JSON lines both ways, plus raw byte chunks announced by a
// JSON header. Events fan out to every attached transport; each transport has
// its own line reader so partial lines from USB and TCP never mix.
#pragma once
#include <ArduinoJson.h>
#include <cstdint>
#include <cstddef>

namespace swl {

// Byte stream the protocol runs over.
struct Stream {
    virtual int  available() = 0;
    virtual int  read() = 0;
    virtual size_t readBytes(uint8_t* dst, size_t n, uint32_t timeout_ms) = 0;
    virtual size_t write(const uint8_t* src, size_t n) = 0;
    virtual bool active() { return true; }          // false = skip on fan-out
    virtual bool trusted() { return true; }         // USB is physically trusted; TCP must authenticate
    virtual ~Stream() = default;
};

static constexpr int kMaxSinks = 3;
void addSink(Stream* s);
Stream* sink(int i);                                 // nullptr past the end
uint32_t nowMs();

// Send one JSON object as a line to every active sink (or one specific sink).
void sendDoc(const JsonDocument& doc, Stream* only = nullptr);

void sendAck(const char* of, const char* msg = nullptr, Stream* only = nullptr);
void sendErr(const char* of, const char* msg, Stream* only = nullptr);
void sendLog(const char* msg);

// JSON header line followed by n raw bytes, to every active sink.
void sendChunk(const JsonDocument& header, const uint8_t* data, size_t n);

// Accumulates bytes from one stream into lines; true when a JSON object with
// a "t" field has been parsed into doc. Bad lines produce an err on that stream.
class LineReader {
public:
    explicit LineReader(Stream* s) : _s(s) {}
    bool poll(JsonDocument& doc);
    Stream* stream() const { return _s; }
private:
    static constexpr size_t kMax = 512;
    Stream* _s;
    char    _buf[kMax];
    size_t  _len = 0;
};

} // namespace swl
