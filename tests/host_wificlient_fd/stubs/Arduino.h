// Host stub for the WiFiClient/WiFiServer fd test: just enough of Arduino.h for the real
// WiFiClient.cpp, WiFiServer.cpp and the core WString/Stream/Print/IPAddress sources.
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
#include "IPAddress.h"

#ifndef ESP_IDF_VERSION_MAJOR
#define ESP_IDF_VERSION_MAJOR 4
#endif
#define ESP_OK 0

typedef bool boolean;
typedef uint8_t byte;

unsigned long millis();
void delay(uint32_t ms);
inline void yield() {}
