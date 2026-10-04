#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

inline uint32_t host_now = 0;
inline uint32_t millis() { return host_now; }
inline void delay(uint32_t ms) { host_now += ms; }
struct HostSerial {
    std::string input, output;
    size_t at = 0;
    int available() { return (int) (input.size() - at); }
    int read() { return available() ? input[at++] : -1; }
    void println(const char* s) { output += std::string(s) + "\n"; }
    void printf(const char* format, ...) {
        char text[1024]; va_list args; va_start(args, format);
        vsnprintf(text, sizeof(text), format, args); va_end(args); output += text;
    }
};
inline HostSerial Serial;
struct HostRestart {};
struct HostEsp {
    int restarts = 0;
    void restart() { ++restarts; throw HostRestart(); }
    uint32_t getMaxAllocHeap() { return 100000; }
};
inline HostEsp ESP;
inline uint32_t host_stream_read_delay = 0;
inline size_t host_stream_read_cap = SIZE_MAX;

class Stream {
public:
    std::vector<uint8_t> bytes;
    size_t at = 0;
    virtual ~Stream() = default;
    int available() { return (int) (bytes.size() - at); }
    int read() { return available() ? bytes[at++] : -1; }
    size_t readBytes(uint8_t* out, size_t n) {
        if (n > host_stream_read_cap) n = host_stream_read_cap;
        if (n > bytes.size() - at) n = bytes.size() - at;
        memcpy(out, bytes.data() + at, n); at += n; host_now += host_stream_read_delay; return n;
    }
};
class Client : public Stream {
public:
    bool connected() { return available() > 0; }
    void stop() {}
    size_t write(const uint8_t*, size_t n) { return n; }
};
class WiFiClient : public Client {};
class WiFiClientSecure : public Client {};
