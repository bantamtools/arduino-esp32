#pragma once
// Host stub: WiFiClient.cpp needs only the DNS lookup from WiFi.h.
#include "Arduino.h"
#include "WiFiClient.h"
class WiFiGenericClass {
public:
    static int hostByName(const char*, IPAddress&) { return 0; }
};
