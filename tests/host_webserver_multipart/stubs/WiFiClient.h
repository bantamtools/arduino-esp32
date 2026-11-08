#pragma once
// Host stub of WiFiClient: plays a scripted byte stream, then either stays connected and
// silent or closes. A read with nothing to deliver advances the simulated clock by 1 ms,
// so Stream::timedRead and _uploadReadByte time out the way they do on the device.
#include "Arduino.h"
#include <memory>
#include <string>
#include <vector>
#include <stdexcept>

extern unsigned long fake_millis;
extern unsigned long fake_hang_limit_ms;  // a parse still running past this is a hang

struct FakeHang : std::runtime_error {
    FakeHang() : std::runtime_error("parser still running past the hang limit") {}
};

enum FakeTail { FAKE_SILENT, FAKE_CLOSE };

struct FakeScript {
    std::string data;
    size_t      pos  = 0;
    FakeTail    tail = FAKE_SILENT;
    // Optional pause: when the read position reaches gap_at, nothing arrives for gap_ms
    // of simulated time, then the rest of the script follows (a client that stalls and
    // then recovers).
    size_t        gap_at    = std::string::npos;
    unsigned long gap_ms    = 0;
    unsigned long gap_until = 0;
    bool          gap_open  = false;
    bool in_gap(unsigned long now) {
        if (pos != gap_at) return false;
        if (!gap_open) {
            gap_open  = true;
            gap_until = now + gap_ms;
        }
        if (now < gap_until) return true;
        gap_at = std::string::npos;  // served
        return false;
    }
};

class WiFiClient : public Stream {
public:
    std::shared_ptr<FakeScript> s;
    std::vector<uint32_t>       timeouts_set;  // seconds, every setTimeout call

    WiFiClient() : s(std::make_shared<FakeScript>()) {}
    WiFiClient(const std::string& d, FakeTail t) : s(std::make_shared<FakeScript>()) {
        s->data = d;
        s->tail = t;
    }

    int available() override { return s->in_gap(fake_millis) ? 0 : (int)(s->data.size() - s->pos); }
    int read() override {
        if (s->pos < s->data.size() && !s->in_gap(fake_millis)) {
            return (unsigned char)s->data[s->pos++];
        }
        fake_millis += 1;
        if (fake_millis > fake_hang_limit_ms) {
            throw FakeHang();
        }
        return -1;
    }
    int peek() override {
        return s->pos < s->data.size() && !s->in_gap(fake_millis) ? (unsigned char)s->data[s->pos] : -1;
    }
    // Pause for gap_ms before the first occurrence of `marker` + its length (just after it).
    void pause_after(const std::string& marker, unsigned long gap_ms) {
        size_t at = s->data.find(marker);
        if (at == std::string::npos) throw std::runtime_error("pause marker not in script: " + marker);
        s->gap_at = at + marker.size();
        s->gap_ms = gap_ms;
    }
    size_t write(uint8_t) override { return 1; }
    size_t write(const uint8_t*, size_t n) override { return n; }
    size_t write(const char*, size_t n) { return n; }
    size_t write_P(const char*, size_t n) { return n; }
    void flush() {}
    // Still connected while script bytes remain, including during a pause.
    uint8_t connected() { return s->pos < s->data.size() || s->tail == FAKE_SILENT; }
    int setTimeout(uint32_t seconds) {
        timeouts_set.push_back(seconds);
        Stream::setTimeout(seconds * 1000);
        return 0;
    }
    operator bool() { return true; }
};
