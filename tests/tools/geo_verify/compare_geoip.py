#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Compare MetaCubeX vs v2fly GeoIP.dat cn coverage on common domestic IPs."""
import sys
import ipaddress

sys.path.insert(0, '.')
from verify_geo_dat import parse_geoip


def build_cn_nets(path):
    cats = parse_geoip(path)
    cn = cats.get('cn')
    if not cn:
        return None, None
    nets = []
    for raw, prefix in cn:
        ip = ipaddress.ip_address(bytes(raw))
        if ip.version == 4:
            nets.append(ipaddress.ip_network(str(ip) + '/' + str(prefix), strict=False))
    return len(cn), nets


def check(ip_str, nets):
    ip = ipaddress.ip_address(ip_str)
    for n in nets:
        if ip in n:
            return True
    return False


# Common domestic sites' IPs (from logs / well-known)
ips = [
    '180.76.11.230',   # baidu official
    '180.76.3.19',     # baidu
    '182.61.200.65',   # baidu
    '39.156.66.10',    # baidu www
    '110.242.68.66',   # baidu www
    '220.181.38.148',  # baidu
    '119.63.197.151',  # taobao
    '140.205.94.188',  # taobao alibaba
    '203.119.174.67',  # qq / tencent?
    '123.151.76.80',   # sina?
    '202.108.22.5',    # sina
    '106.3.16.104',    # mobile
    '36.110.181.102',  # telecom
    '111.13.29.102',   # mobile
    '101.246.176.240', # ?
    '112.53.55.211',   # ?
    '120.52.86.97',    # tencent
    '111.206.229.54',  # mobile
    '203.119.174.67',  # ?
    '182.254.4.111',   # tencent
    '223.5.5.5',       # alidns (domestic DNS)
    '119.29.29.29',    # dnspod
    '114.114.114.114', # 114 dns
]

for path, label in [('GeoIP.dat', 'MetaCubeX (current)'), ('geoip_v2fly.dat', 'v2fly (official)')]:
    count, nets = build_cn_nets(path)
    if nets is None:
        print(f'{label}: parse failed')
        continue
    hits = sum(1 for ip in ips if check(ip, nets))
    print(f'\n{label}: cn networks={count}, hit={hits}/{len(ips)}')
    for ip in ips:
        ok = check(ip, nets)
        mark = 'OK ' if ok else 'MISS'
        print(f'  [{mark}] {ip}')
