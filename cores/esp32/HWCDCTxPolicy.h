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

// What the HWCDC IN_EMPTY interrupt does once the TX FIFO is writable, and how large a
// packet it may send.
//
// The USB-Serial-JTAG IN endpoint uses 64-byte bulk packets. The host ends a bulk
// transfer only on a short packet (fewer than 64 bytes, or zero-length), so a transfer
// whose last packet is exactly 64 bytes is held on the host until more data follows
// (see usb_serial_jtag_ll_txfifo_flush() in hal/usb_serial_jtag_ll.h).
//
// The ISR therefore never sends a full packet: it takes at most HWCDC_TX_MAX_PACKET (63)
// bytes from the ring per IN packet. Every packet is short, so the host completes each
// transfer by itself and no zero-length packet (ZLP) is ever needed. The cost is one
// extra packet per 63 bytes of a long burst, about 1.6% of bulk throughput.
//
// An earlier version sent 64-byte packets and a ZLP once the ring drained. That ZLP was
// flushed with IN_EMPTY masked and its status still latched; a following write()
// re-enabled the mask, the ISR fired while the ZLP was still in flight, found the FIFO
// not writable and cleared the status. Unless IN_EMPTY was raised again on ZLP
// completion, nothing restarted TX and USB output stopped until reboot. With no ZLP
// there is no flush of an empty FIFO and no state carried between interrupts.
//
// Kept free of hardware access so it can be tested on a host.

#pragma once

#include <stdbool.h>
#include <stddef.h>

#define HWCDC_TX_PACKET_SIZE 64  // USB full-speed bulk max packet size of the IN endpoint
#define HWCDC_TX_MAX_PACKET  63  // most bytes the ISR puts in one IN packet: always short

typedef enum {
    HWCDC_TX_SEND,  // write the queued bytes (at most HWCDC_TX_MAX_PACKET) to the FIFO and flush
    HWCDC_TX_IDLE,  // nothing to send; IN_EMPTY stays disabled until the next write()
} hwcdc_tx_action_t;

// have_data: the ring returned bytes. No other input and no state: each packet is short,
// so nothing is owed to the host once the ring is empty.
static inline hwcdc_tx_action_t hwcdc_tx_next(bool have_data) {
    return have_data ? HWCDC_TX_SEND : HWCDC_TX_IDLE;
}
