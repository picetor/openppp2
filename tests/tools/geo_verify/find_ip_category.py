#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Find which categories an IP belongs to in GeoIP.dat."""
import sys
import ipaddress

sys.path.insert(0, '.')
from verify_geo_dat import parse_geoip

path = sys.argv[1] if len(sys.argv) > 1 else 'GeoIP.dat'
ips = sys.argv[2:] if len(sys.argv) > 2 else ['180.76.11.230']

cats = parse_geoip(path)
print('parsed', path, ':', len(cats), 'categories')

for ip_str in ips:
    ip = ipaddress.ip_address(ip_str)
    found = []
    for cat, nets in cats.items():
        for raw, prefix in nets:
            net_ip = ipaddress.ip_address(bytes(raw))
            if ip.version != net_ip.version:
                continue
            net = ipaddress.ip_network(str(net_ip) + '/' + str(prefix), strict=False)
            if ip in net:
                found.append((cat, str(net_ip) + '/' + str(prefix)))
                break
    if found:
        for cat, net in found:
            print(ip_str, '->', cat, net)
    else:
        print(ip_str, '-> NO CATEGORY FOUND')
