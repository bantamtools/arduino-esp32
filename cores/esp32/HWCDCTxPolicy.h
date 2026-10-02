// Copyright 2026 Bantam Tools
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// What the HWCDC IN_EMPTY interrupt does once the TX FIFO is writable.
//
// The USB-Serial-JTAG IN endpoint uses 64-byte bulk packets. The host ends a bulk
// transfer only on a short packet, so a full 64-byte packet is held on the host side
// until more data, or a zero-length packet (ZLP), follows. The hardware flushes a full
// FIFO by itself, so the flush after a 64-byte write sends nothing extra. A ZLP needs a
// second flush once the FIFO is writable again (see usb_serial_jtag_ll_txfifo_flush()
// in hal/usb_serial_jtag_ll.h).
//
// Without the ZLP, a reply whose last packet is exactly 64 bytes sits undelivered
// until the device sends anything else. IN_EMPTY fires after the host has taken the
// last packet, so when the ring is empty and the last packet was full, the ISR sends
// the ZLP there.
//
// Kept free of hardware access so it can be tested on a host.

#pragma once

#include <stdbool.h>
#include <stddef.h>

#define HWCDC_TX_PACKET_SIZE 64

typedef enum {
    HWCDC_TX_SEND,  // write the queued bytes to the FIFO and flush
    HWCDC_TX_ZLP,   // flush the empty FIFO: a zero-length packet ends the transfer
    HWCDC_TX_IDLE,  // nothing to send
} hwcdc_tx_action_t;

// have_data: the ring returned bytes. queued_size: how many (at most 64).
// *last_full: whether the last packet sent was a full 64 bytes. Updated in place.
static inline hwcdc_tx_action_t hwcdc_tx_next(bool have_data, size_t queued_size, bool* last_full) {
    if (have_data) {
        *last_full = (queued_size == HWCDC_TX_PACKET_SIZE);
        return HWCDC_TX_SEND;
    }
    if (*last_full) {
        *last_full = false;
        return HWCDC_TX_ZLP;
    }
    return HWCDC_TX_IDLE;
}
