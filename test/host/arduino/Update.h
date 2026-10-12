#pragma once
#include "Arduino.h"
#include "esp_ota_ops.h"
inline bool host_update_end_called = false;
inline bool host_update_end_saw_probation = false;
struct HostUpdate {
    std::vector<uint8_t> written;
    bool begin(uint32_t size) { written.clear(); return size > 0; }
    size_t write(const uint8_t* b, size_t n) { written.insert(written.end(), b, b + n); return n; }
    const char* errorString() { return "host failure"; }
    void abort() { written.clear(); }
    bool end(bool);
};
inline HostUpdate Update;
