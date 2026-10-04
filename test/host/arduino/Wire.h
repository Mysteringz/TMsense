#pragma once
#include "Arduino.h"
struct HostWire {
    int transmissions = 0;
    bool fail_verify = false;
    void beginTransmission(uint8_t) { ++transmissions; }
    void write(unsigned) {}
    int endTransmission(bool = true) { return fail_verify && transmissions > 1 ? 1 : 0; }
    int requestFrom(uint8_t, uint8_t size) { return size; }
    int read() { return 0; }
    void setClock(long) {}
};
inline HostWire Wire;
