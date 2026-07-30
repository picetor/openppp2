#!/bin/sh
set -eu

printf '127.0.0.1 openppp2-musl-hosts.test\n' >> /etc/hosts
g++ -std=c++17 -static -O2 tools/ci/musl-network-smoke.cpp \
    -o /tmp/musl-network-smoke -lssl -lcrypto -lz -ldl -latomic -pthread

if readelf -l /tmp/musl-network-smoke | grep -q 'INTERP' ||
   readelf -d /tmp/musl-network-smoke | grep -q 'NEEDED'; then
    echo "Network smoke test is not fully static" >&2
    exit 1
fi

echo "Running musl network smoke test (60s timeout)"
timeout 60s /tmp/musl-network-smoke