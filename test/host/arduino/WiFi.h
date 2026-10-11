#pragma once
#include "Arduino.h"
#define WL_CONNECTED 3
struct HostWifi { int status() { return WL_CONNECTED; } };
inline HostWifi WiFi;
