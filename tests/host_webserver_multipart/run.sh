#!/bin/bash
# Host test for the WebServer multipart parser exits (no hardware, no test framework).
#
#   tests/host_webserver_multipart/run.sh
#
# Compiles the real libraries/WebServer/src/Parsing.cpp and mimetable.cpp, the real
# WString/Stream/Print sources from cores/esp32, and the host stubs in stubs/ (a scripted
# WiFiClient with a simulated clock). The core sources are copied next to the stubs
# first, because their #include "Arduino.h" would otherwise find cores/esp32/Arduino.h.
#
# Exits 0 and prints "ALL PASS" when every check passes; non-zero otherwise.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$(mktemp -d "${TMPDIR:-/tmp}/host_webserver_multipart.XXXXXX")"
trap 'rm -rf "$BUILD"' EXIT

cp "$HERE"/stubs/* "$BUILD"/
for f in WString.cpp WString.h Stream.cpp Stream.h Print.cpp Print.h Printable.h \
         stdlib_noniso.c stdlib_noniso.h pgmspace.h; do
    cp "$ROOT/cores/esp32/$f" "$BUILD"/
done
HTTP_PARSER_DIR="$ROOT/tools/sdk/esp32s3/include/nghttp/port/include"

CXX="${CXX:-c++}"
CC="${CC:-cc}"
CXXFLAGS=(-std=gnu++17 -g -O1 -w -I"$BUILD" -I"$HTTP_PARSER_DIR" -I"$ROOT/libraries/WebServer/src")
"$CC" -c -w -I"$BUILD" "$BUILD/stdlib_noniso.c" -o "$BUILD/stdlib_noniso.o"
"$CC" -c -w -I"$BUILD" "$BUILD/host_newlib_extras.c" -o "$BUILD/host_newlib_extras.o"
"$CXX" "${CXXFLAGS[@]}" \
    "$HERE/webserver_multipart_test.cpp" \
    "$ROOT/libraries/WebServer/src/Parsing.cpp" \
    "$ROOT/libraries/WebServer/src/detail/mimetable.cpp" \
    "$BUILD/WString.cpp" "$BUILD/Stream.cpp" "$BUILD/Print.cpp" \
    "$BUILD/stdlib_noniso.o" "$BUILD/host_newlib_extras.o" -o "$BUILD/t"
"$BUILD/t"
