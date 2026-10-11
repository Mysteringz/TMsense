#pragma once
#include <cstdio>
class IPAddress {
public:
    bool fromString(const char* s) {
        unsigned a, b, c, d; char extra;
        return sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) == 4 && a < 256 && b < 256 && c < 256 && d < 256;
    }
};
