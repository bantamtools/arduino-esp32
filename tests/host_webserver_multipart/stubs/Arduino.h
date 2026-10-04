// Host stub for the WebServer multipart test: just enough of Arduino.h for Parsing.cpp
// and the real WString/Stream/Print sources. Time is simulated (fake_millis).
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <algorithm>
#include "pgmspace.h"
#include "esp32-hal.h"
#include "esp32-hal-log.h"
#include "WString.h"
#include "Stream.h"

typedef bool boolean;
typedef uint8_t byte;

extern unsigned long fake_millis;
inline unsigned long millis() { return fake_millis; }
inline void delay(uint32_t ms) { fake_millis += ms; }
inline void yield() {}

class IPAddress {
public:
    IPAddress() {}
    String toString() const { return String("0.0.0.0"); }
};
