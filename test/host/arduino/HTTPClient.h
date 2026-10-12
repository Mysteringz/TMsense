#pragma once
#include "WiFi.h"
#define HTTP_CODE_OK 200
class HTTPClient {
    WiFiClient stream;
public:
    void setTimeout(int) {}
    void setConnectTimeout(int) {}
    bool begin(const char*) { return false; }
    int GET() { return 500; }
    int getSize() { return -1; }
    WiFiClient* getStreamPtr() { return &stream; }
    void end() {}
};
