#!/bin/sh
set -eu

PF_CONF="${PF_CONF:-/etc/pf.conf}"
ANCHOR_LINE='anchor "openppp2/nat66"'

if [ "$(id -u)" -ne 0 ]; then
    echo "run as root" >&2
    exit 1
fi
if [ ! -f "$PF_CONF" ]; then
    echo "PF configuration not found: $PF_CONF" >&2
    exit 1
fi

if grep -Fqx "$ANCHOR_LINE" "$PF_CONF"; then
    pfctl -f "$PF_CONF"
    echo "OpenPPP2 PF anchor already installed and ruleset reloaded"
    exit 0
fi

backup="${PF_CONF}.openppp2.$(date +%Y%m%d%H%M%S).bak"
cp -p "$PF_CONF" "$backup"
printf '\n# OpenPPP2 managed NAT66 anchor\n%s\n' "$ANCHOR_LINE" >> "$PF_CONF"

if ! pfctl -nf "$PF_CONF"; then
    cp -p "$backup" "$PF_CONF"
    echo "PF syntax validation failed; restored $PF_CONF" >&2
    exit 1
fi

if ! pfctl -f "$PF_CONF"; then
    cp -p "$backup" "$PF_CONF"
    pfctl -f "$PF_CONF" >/dev/null 2>&1 || true
    echo "PF reload failed; restored $PF_CONF" >&2
    exit 1
fi

echo "Installed OpenPPP2 PF anchor in $PF_CONF (backup: $backup)"
