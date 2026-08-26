#!/bin/sh
set -eu

PF_CONF="${PF_CONF:-/etc/pf.conf}"
ANCHOR_LINE='anchor "openppp2/nat66"'

if [ "$(id -u)" -ne 0 ]; then
    echo "run as root" >&2
    exit 1
fi
if [ ! -f "$PF_CONF" ]; then
    exit 0
fi

tmp="$(mktemp "${TMPDIR:-/tmp}/openppp2-pf.XXXXXX")"
trap 'rm -f "$tmp"' EXIT INT TERM
awk -v anchor="$ANCHOR_LINE" '$0 != anchor { print }' "$PF_CONF" > "$tmp"
backup="${PF_CONF}.openppp2.$(date +%Y%m%d%H%M%S).bak"
cp -p "$PF_CONF" "$backup"
cp "$tmp" "$PF_CONF"
if ! pfctl -nf "$PF_CONF"; then
    cp -p "$backup" "$PF_CONF" 2>/dev/null || true
    echo "PF syntax validation failed; manual restoration may be required" >&2
    exit 1
fi
if ! pfctl -f "$PF_CONF"; then
    cp -p "$backup" "$PF_CONF" 2>/dev/null || true
    pfctl -f "$PF_CONF" >/dev/null 2>&1 || true
    echo "PF reload failed; restored $PF_CONF" >&2
    exit 1
fi
echo "Removed OpenPPP2 PF anchor declaration from $PF_CONF"
