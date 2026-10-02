// Host test for cores/esp32/HWCDCTxPolicy.h (no hardware, no test framework).
//
//   c++ -std=c++17 -Wall -Wextra -Werror -I../../cores/esp32 hwcdc_tx_policy_test.cpp -o /tmp/t && /tmp/t
//
// Exits 0 and prints "ALL PASS" when every check passes; prints each failure and exits 1
// otherwise.

#include "HWCDCTxPolicy.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            ++failures;                                        \
            std::printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
            std::printf(__VA_ARGS__);                          \
            std::printf("\n");                                 \
        }                                                      \
    } while (0)

// ---- Direct cases ----------------------------------------------------------------

static void direct_cases() {
    bool lf = false;
    CHECK(hwcdc_tx_next(true, 64, &lf) == HWCDC_TX_SEND, "full packet is sent");
    CHECK(lf, "a 64-byte packet must be remembered as full");

    lf = false;
    CHECK(hwcdc_tx_next(true, 63, &lf) == HWCDC_TX_SEND, "short packet is sent");
    CHECK(!lf, "a 63-byte packet is short: no ZLP owed");

    lf = true;
    CHECK(hwcdc_tx_next(true, 1, &lf) == HWCDC_TX_SEND, "data after a full packet is sent");
    CHECK(!lf, "a short packet after a full one ends the transfer: no ZLP owed");

    lf = true;
    CHECK(hwcdc_tx_next(false, 0, &lf) == HWCDC_TX_ZLP, "empty ring after a full packet sends a ZLP");
    CHECK(!lf, "the ZLP settles the debt");
    CHECK(hwcdc_tx_next(false, 0, &lf) == HWCDC_TX_IDLE, "only one ZLP per full packet");

    lf = false;
    CHECK(hwcdc_tx_next(false, 0, &lf) == HWCDC_TX_IDLE, "empty ring after a short packet is idle");
    CHECK(!lf, "idle leaves the flag clear");
}

// ---- Model: ring + ISR + host ----------------------------------------------------
//
// Byte ring like RINGBUF_TYPE_BYTEBUF: ReceiveUpTo returns at most 64 contiguous bytes
// and stops at the wrap point. The FIFO holds one packet; the ISR runs when IN_EMPTY is
// enabled and the host has taken the last packet, as the real ISR does. The host ends a
// transfer (hands bytes to the application) only on a packet shorter than 64 bytes.

struct Model {
    std::vector<unsigned char> ring;
    size_t                     head = 0, count = 0;  // read position, bytes queued
    bool                       in_empty_ena = true;
    bool                       last_full    = false;
    std::string                host_partial;   // bytes in a transfer not yet ended
    std::string                host_app;       // bytes the application has received
    int                        zlps = 0;

    explicit Model(size_t cap) : ring(cap) {}

    void write(const std::string& s) {  // HWCDC::write: ring send, then enable IN_EMPTY
        for (char c : s) {
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
            n = count < 64 ? count : 64;
            if (head + n > ring.size()) {
                n = ring.size() - head;  // contiguous only, like the byte ring
            }
        }
        hwcdc_tx_action_t a = hwcdc_tx_next(n != 0, n, &last_full);
        if (a == HWCDC_TX_SEND) {
            std::string pkt;
            for (size_t i = 0; i < n; ++i) {
                pkt += (char)ring[(head + i) % ring.size()];
            }
            head = (head + n) % ring.size();
            count -= n;
            in_empty_ena = true;
            host_take(pkt);
        } else if (a == HWCDC_TX_ZLP) {
            ++zlps;
            host_take(std::string());
        }
        return true;
    }

    void host_take(const std::string& pkt) {
        host_partial += pkt;
        if (pkt.size() < HWCDC_TX_PACKET_SIZE) {  // short packet or ZLP ends the transfer
            host_app += host_partial;
            host_partial.clear();
        }
    }

    void run() {
        for (int guard = 0; guard < 100000 && isr(); ++guard) {}
    }
};

static std::string payload(size_t n, char seed) {
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s += (char)('A' + (seed + i) % 26);
    }
    s[n - 1] = '\n';
    return s;
}

// Every reply written at idle must reach the application with no further traffic,
// at every ring offset and for every length, including multiples of 64 (the 832-byte
// $JobRec/Stats reply that stalled on the bench is 13 x 64).
static void model_delivers_everything() {
    const size_t cap = 2048;
    for (size_t offset = 0; offset < 130; ++offset) {
        for (size_t len : {1u, 63u, 64u, 65u, 127u, 128u, 129u, 832u, 1024u, 2048u - 64u}) {
            Model m(cap);
            if (offset) {  // advance the ring position with a short, delivered reply
                m.write(payload(offset, 7));
                m.run();
            }
            std::string before = m.host_app;
            std::string reply  = payload(len, (char)len);
            m.write(reply);
            m.run();
            CHECK(m.host_app == before + reply, "offset %zu len %zu: host got %zu of %zu bytes", offset, len,
                  m.host_app.size() - before.size(), reply.size());
            CHECK(m.host_partial.empty(), "offset %zu len %zu: %zu bytes held on the host", offset, len,
                  m.host_partial.size());
        }
    }
}

// The aligned 832-byte reply ends on a full packet and must take exactly one ZLP; a
// reply ending on a short packet must take none.
static void model_zlp_count() {
    Model a(2048);
    a.write(payload(832, 1));
    a.run();
    CHECK(a.zlps == 1, "aligned 832-byte reply: %d ZLPs, want 1", a.zlps);

    Model b(2048);
    b.write(payload(833, 1));
    b.run();
    CHECK(b.zlps == 0, "833-byte reply ends short: %d ZLPs, want 0", b.zlps);

    // Back-to-back: reply, ok, reply, ok. Each idle point after a full packet gets one.
    Model c(2048);
    c.write(payload(64, 2));
    c.run();
    c.write(payload(64, 3));
    c.run();
    CHECK(c.zlps == 2, "two 64-byte replies at idle: %d ZLPs, want 2", c.zlps);
    CHECK(c.host_app.size() == 128, "both delivered: got %zu", c.host_app.size());
}

int main() {
    direct_cases();
    model_delivers_everything();
    model_zlp_count();
    if (failures) {
        std::printf("%d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("ALL PASS\n");
    return 0;
}
