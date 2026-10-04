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

// When the multipart parser gives up on a line-oriented loop.
//
// WebServer::_parseForm reads the part headers, and the value of a non-file part, one
// line at a time with Stream::readStringUntil. A read that times out, a read on a
// closed connection, and a line lost to a failed String allocation all return an empty
// string. The loops had no exit for that case: the part-header loop only acted on a
// line starting with "Content-Disposition", and the value loop only on the boundary,
// so a client that stopped sending (or a heap too low to build the line) kept the web
// task inside the parser for good, holding the request's memory.
//
// The guard is told about each line the loop reads and says when to give up:
//   - an empty line while the peer is gone (not connected, nothing buffered);
//   - more than MP_GUARD_MAX_EMPTY_RUN empty lines in a row;
//   - in the part-header loop only: MP_GUARD_MAX_OTHER_RUN lines in a row that are not
//     a Content-Disposition line (empty lines count too).
// A legitimate form has at most one empty line between parts, and its part headers
// start with Content-Disposition, or have one or two other headers first.
//
// Limitation, accepted: a non-file field whose value holds three or more blank lines in
// a row is refused. The FluidNC upload forms send one-line values.
//
// Kept free of Arduino types so it can be tested on a host.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MP_GUARD_MAX_EMPTY_RUN 2  // give up on the 3rd empty line in a row
#define MP_GUARD_MAX_OTHER_RUN 4  // give up on the 4th non-Content-Disposition line in a row

typedef struct {
    uint8_t empty_run;            // empty lines in a row
    uint8_t other_run;            // lines in a row that were not Content-Disposition
    bool    count_non_disposition;  // true in the part-header loop, false in the value loop
} mp_line_guard_t;

static inline mp_line_guard_t mp_line_guard_make(bool part_headers) {
    mp_line_guard_t g;
    g.empty_run             = 0;
    g.other_run             = 0;
    g.count_non_disposition = part_headers;
    return g;
}

// Feed one line just read. Returns true when the loop must give up.
//   line_empty      the line read was empty (a timeout or a closed peer looks the same)
//   is_disposition  the line starts with "Content-Disposition" (ignored in the value loop)
//   peer_gone       the client is not connected and has nothing left to read
static inline bool mp_line_guard_give_up(mp_line_guard_t* g, bool line_empty, bool is_disposition, bool peer_gone) {
    if (line_empty) {
        if (peer_gone) {
            return true;
        }
        if (g->empty_run < 255) {
            ++g->empty_run;
        }
        if (g->empty_run > MP_GUARD_MAX_EMPTY_RUN) {
            return true;
        }
    } else {
        g->empty_run = 0;
    }
    if (g->count_non_disposition) {
        if (!line_empty && is_disposition) {
            g->other_run = 0;
        } else {
            if (g->other_run < 255) {
                ++g->other_run;
            }
            if (g->other_run >= MP_GUARD_MAX_OTHER_RUN) {
                return true;
            }
        }
    }
    return false;
}
