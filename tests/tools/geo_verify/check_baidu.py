#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Check 180.76.0.0/20 coverage in a GeoIP.dat file."""
import sys
import ipaddress

sys.path.insert(0, '.')
from verify_geo_dat import parse_geoip

path = sys.argv[1] if len(sys.argv) > 1 else 'GeoIP.dat'
cats = parse_geoip(path)
print('categories:', len(cats))
cn = cats.get('cn')
print('cn networks:', len(cn) if cn else 'NOT FOUND')

print('=== 180.76.x in cn ===')
for raw, prefix in cn:
    net_ip = ipaddress.ip_address(bytes(raw))
    if net_ip.version == 4 and str(net_ip).startswith('180.76.'):
        print(net_ip, '/', prefix)

print()
checks = ['180.76.11.230', '182.61.200.65', '180.76.0.1']
for ip_str in checks:
    ip = ipaddress.ip_address(ip_str)
    hit = False
    for raw, prefix in cn:
        net_ip = ipaddress.ip_address(bytes(raw))
        if ip.version != net_ip.version:
            continue
        net = ipaddress.ip_network(str(net_ip) + '/' + str(prefix), strict=False)
        if ip in net:
            hit = True
            break
    print(ip_str, ':', 'YES' if hit else 'NO')
