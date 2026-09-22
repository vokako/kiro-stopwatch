#include "link.hpp"
#include <cstring>

namespace swl {

static Stream* s_sinks[kMaxSinks];
static int     s_nsinks = 0;

void addSink(Stream* s) { if (s_nsinks < kMaxSinks) s_sinks[s_nsinks++] = s; }
Stream* sink(int i) { return i < s_nsinks ? s_sinks[i] : nullptr; }

static void writeAll(const uint8_t* p, size_t n, Stream* only)
{
    if (only) { if (only->active()) only->write(p, n); return; }
    for (int i = 0; i < s_nsinks; i++) if (s_sinks[i]->active()) s_sinks[i]->write(p, n);
}

void sendDoc(const JsonDocument& doc, Stream* only)
{
    char out[512];
    size_t n = serializeJson(doc, out, sizeof out - 1);
    out[n++] = '\n';
    writeAll(reinterpret_cast<const uint8_t*>(out), n, only);
}

void sendAck(const char* of, const char* msg, Stream* only)
{
    JsonDocument d; d["t"] = "ack"; d["of"] = of; if (msg) d["msg"] = msg;
    sendDoc(d, only);
}

void sendErr(const char* of, const char* msg, Stream* only)
{
    JsonDocument d; d["t"] = "err"; d["of"] = of; d["msg"] = msg;
    sendDoc(d, only);
}

void sendLog(const char* msg)
{
    JsonDocument d; d["t"] = "log"; d["msg"] = msg;
    sendDoc(d);
}

void sendChunk(const JsonDocument& header, const uint8_t* data, size_t n)
{
    // header + payload must not interleave with other writers on the same sink;
    // the firmware is single-threaded so this ordering is enough.
    sendDoc(header);
    writeAll(data, n, nullptr);
}

bool LineReader::poll(JsonDocument& doc)
{
    if (!_s->active()) { _len = 0; return false; }
    while (_s->available() > 0) {
        int c = _s->read();
        if (c < 0) break;
        if (c == '\n' || c == '\r') {
            if (_len == 0) continue;
            _buf[_len] = 0;
            size_t len = _len; _len = 0;
            doc.clear();
            DeserializationError e = deserializeJson(doc, _buf, len);
            if (e) { sendErr("?", e.c_str(), _s); continue; }
            if (!doc["t"].is<const char*>()) { sendErr("?", "missing t", _s); continue; }
            return true;
        }
        if (_len < kMax - 1) _buf[_len++] = (char)c;
        else _len = 0;
    }
    return false;
}

} // namespace swl
