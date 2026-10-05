// Host test for cores/esp32/HWCDCTxPolicy.h (no hardware, no test framework).
//
//   c++ -std=c++17 -O2 -Wall -Wextra -Werror -I../../cores/esp32 hwcdc_tx_policy_test.cpp -o /tmp/t && /tmp/t
//
// Exits 0 and prints "ALL PASS" when every check passes; prints the first failures and
// exits 1 otherwise.

#include "HWCDCTxPolicy.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>
#include <vector>

static int failures = 0;
#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            if (++failures <= 20) {                                         \
                std::printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
                std::printf(__VA_ARGS__);                                   \
                std::printf("\n");                                          \
            }                                                               \
        }                                                                   \
    } while (0)

// The USB full-speed bulk max packet size, independent of the header under test.
static const size_t kUsbMps = 64;

// ---- No ZLP state ----------------------------------------------------------------
//
// The decision takes only "the ring returned bytes": no flag in, no flag out, so nothing
// can carry a pending ZLP from one interrupt to the next. The enum has only SEND and IDLE.

static_assert(std::is_same<decltype(&hwcdc_tx_next), hwcdc_tx_action_t (*)(bool)>::value,
              "hwcdc_tx_next must be stateless: hwcdc_tx_action_t(bool)");
static_assert(HWCDC_TX_SEND == 0 && HWCDC_TX_IDLE == 1, "only SEND and IDLE");
static_assert(HWCDC_TX_PACKET_SIZE == kUsbMps, "the IN endpoint's max packet size is 64");

static void direct_cases() {
    CHECK(HWCDC_TX_MAX_PACKET < kUsbMps, "cap %d must be below the 64-byte max packet size", HWCDC_TX_MAX_PACKET);
    CHECK(HWCDC_TX_MAX_PACKET == kUsbMps - 1, "cap %d should be 63 (largest short packet)", HWCDC_TX_MAX_PACKET);
    CHECK(hwcdc_tx_next(true) == HWCDC_TX_SEND, "queued bytes are sent");
    CHECK(hwcdc_tx_next(false) == HWCDC_TX_IDLE, "empty ring is idle");
}

// ---- Model: ring + ISR + host ----------------------------------------------------
//
// Byte ring like RINGBUF_TYPE_BYTEBUF: ReceiveUpTo returns at most the requested number
// of contiguous bytes and stops at the wrap point. The ISR requests HWCDC_TX_MAX_PACKET,
// as hw_cdc_isr_handler does, and runs when IN_EMPTY is enabled and the host has taken the
// last packet. write() fills the ring and enables IN_EMPTY; when the ring is full it waits
// for the ISR to drain, as the blocking xRingbufferSend does. The host ends a transfer
// (hands bytes to the application) only on a packet shorter than 64 bytes.

struct Model {
    std::vector<unsigned char> ring;
    size_t                     head = 0, count = 0;  // read position, bytes queued
    bool                       in_empty_ena = true;
    std::string                host_partial;  // bytes in a transfer not yet ended
    std::string                host_app;      // bytes the application has received
    std::vector<size_t>        packets;       // size of every IN packet, in order
    int                        empty_flushes = 0;

    explicit Model(size_t cap) : ring(cap) {}

    void write(const std::string& s) {
        for (char c : s) {
            if (count == ring.size()) {  // ring full: the ISR drains while write() blocks
                in_empty_ena = true;
                isr();
            }
            ring[(head + count) % ring.size()] = (unsigned char)c;
            ++count;
        }
        in_empty_ena = true;
    }

    // One IN_EMPTY interrupt with the FIFO writable. Mirrors hw_cdc_isr_handler.
    // Returns false when the interrupt is disabled (nothing more will happen).
    bool isr() {
        if (!in_empty_ena) {
            return false;
        }
        in_empty_ena = false;
        size_t n     = 0;
        if (count) {
            n = count < (size_t)HWCDC_TX_MAX_PACKET ? count : (size_t)HWCDC_TX_MAX_PACKET;
            if (head + n > ring.size()) {
                n = ring.size() - head;  // contiguous only, like the byte ring
            }
        }
        if (hwcdc_tx_next(n != 0) == HWCDC_TX_SEND) {
            std::string pkt;
            for (size_t i = 0; i < n; ++i) {
                pkt += (char)ring[(head + i) % ring.size()];
            }
            head = (head + n) % ring.size();
            count -= n;
            in_empty_ena = true;
            host_take(pkt);
        }
        return true;
    }

    void host_take(const std::string& pkt) {
        packets.push_back(pkt.size());
        if (pkt.empty()) {
            ++empty_flushes;
        }
        host_partial += pkt;
        if (pkt.size() < kUsbMps) {  // short packet ends the transfer
            host_app += host_partial;
            host_partial.clear();
        }
    }

    void run() {
        for (int guard = 0; guard < 100000 && isr(); ++guard) {}
    }
};

static std::string payload(size_t n, size_t seed) {
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s += (char)('A' + (seed + i) % 26);
    }
    if (n) {
        s[n - 1] = '\n';
    }
    return s;
}

// Every burst of 0-1024 bytes, written at idle at every ring offset, reaches the
// application with no further traffic: no packet is 64 bytes, the burst ends on a short
// packet, nothing is held on the host, and no empty (ZLP) flush ever happens. The ring is
// the HWCDC default of 256 bytes, so long bursts wrap and block in write().
static void model_every_burst_every_offset() {
    const size_t cap = 256;
    for (size_t offset = 0; offset < cap; ++offset) {
        for (size_t len = 0; len <= 1024; ++len) {
            Model m(cap);
            if (offset) {  // advance the ring position with a delivered reply
                m.write(payload(offset, 7));
                m.run();
            }
            std::string before = m.host_app;
            m.packets.clear();
            std::string reply = payload(len, len);
            m.write(reply);
            m.run();
            for (size_t p : m.packets) {
                CHECK(p != kUsbMps && p <= (size_t)HWCDC_TX_MAX_PACKET, "offset %zu len %zu: %zu-byte packet", offset,
                      len, p);
            }
            if (len) {
                CHECK(!m.packets.empty() && m.packets.back() < kUsbMps,
                      "offset %zu len %zu: burst does not end on a short packet", offset, len);
            } else {
                CHECK(m.packets.empty(), "offset %zu len 0: %zu packets sent", offset, m.packets.size());
            }
            CHECK(m.host_app == before + reply, "offset %zu len %zu: host got %zu of %zu bytes", offset, len,
                  m.host_app.size() - before.size(), reply.size());
            CHECK(m.host_partial.empty(), "offset %zu len %zu: %zu bytes held on the host", offset, len,
                  m.host_partial.size());
            CHECK(m.empty_flushes == 0, "offset %zu len %zu: %d ZLPs", offset, len, m.empty_flushes);
        }
    }
}

// The 64-byte line followed by a separate 2-byte CRLF write that stalled on the HW85:
// both arrive, as two complete transfers, with no ZLP.
static void model_line_then_crlf() {
    for (size_t offset = 0; offset < 256; ++offset) {
        Model m(256);
        if (offset) {
            m.write(payload(offset, 3));
            m.run();
        }
        std::string before = m.host_app;
        std::string line   = payload(64, 1);
        line[63]           = 'x';
        m.write(line);
        m.run();
        CHECK(m.host_app == before + line, "offset %zu: 64-byte line not delivered on its own", offset);
        m.write("\r\n");
        m.run();
        CHECK(m.host_app == before + line + "\r\n", "offset %zu: CRLF after the 64-byte line not delivered", offset);
        CHECK(m.empty_flushes == 0, "offset %zu: %d ZLPs", offset, m.empty_flushes);
    }
}

int main() {
    direct_cases();
    model_every_burst_every_offset();
    model_line_then_crlf();
    if (failures) {
        std::printf("%d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("ALL PASS\n");
    return 0;
}
