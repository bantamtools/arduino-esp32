#!/bin/bash
# Host test: an fd accepted by WiFiServer is closed on every way out, also when the
# WiFiClient that should own it cannot be built (no hardware, no test framework).
#
#   tests/host_wificlient_fd/run.sh
#
# Compiles the real libraries/WiFi/src/WiFiClient.cpp and WiFiServer.cpp (copied next to
# the stubs, as in tests/host_webserver_multipart), the real
# WString/Stream/Print/IPAddress sources from cores/esp32, and the host stubs in stubs/
# (BSD sockets under the lwip names). Uses a socketpair and a loopback-only listener.
#
# Exits 0 and prints "ALL PASS" when every check passes; non-zero otherwise.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$(mktemp -d "${TMPDIR:-/tmp}/host_wificlient_fd.XXXXXX")"
trap 'rm -rf "$BUILD"' EXIT

cp -R "$HERE"/stubs/* "$BUILD"/
for f in WString.cpp WString.h Stream.cpp Stream.h Print.cpp Print.h Printable.h \
         IPAddress.cpp IPAddress.h Client.h Server.h stdlib_noniso.c stdlib_noniso.h pgmspace.h; do
    cp "$ROOT/cores/esp32/$f" "$BUILD"/
done
cp "$ROOT/tests/host_webserver_multipart/stubs/host_newlib_extras.c" "$BUILD"/
# The WiFi sources too, so their #include "WiFi.h" finds the stub, not the real WiFi.h.
for f in WiFiClient.cpp WiFiClient.h WiFiServer.cpp WiFiServer.h; do
    cp "$ROOT/libraries/WiFi/src/$f" "$BUILD"/
done

CXX="${CXX:-c++}"
CC="${CC:-cc}"
CXXFLAGS=(-std=gnu++17 -g -O1 -w -I"$BUILD")
"$CC" -c -w -I"$BUILD" "$BUILD/stdlib_noniso.c" -o "$BUILD/stdlib_noniso.o"
"$CC" -c -w -I"$BUILD" "$BUILD/host_newlib_extras.c" -o "$BUILD/host_newlib_extras.o"
"$CXX" "${CXXFLAGS[@]}" \
    "$HERE/wificlient_fd_test.cpp" \
    "$BUILD/WiFiClient.cpp" "$BUILD/WiFiServer.cpp" \
    "$BUILD/WString.cpp" "$BUILD/Stream.cpp" "$BUILD/Print.cpp" "$BUILD/IPAddress.cpp" \
    "$BUILD/stdlib_noniso.o" "$BUILD/host_newlib_extras.o" -o "$BUILD/t"
"$BUILD/t"
