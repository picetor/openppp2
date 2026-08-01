#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Verify GeoSite.dat / GeoIP.dat contents on the OpenPPP2 project.

Usage:
    python verify_geo_dat.py <GeoSite.dat> [cn,google,...]
    python verify_geo_dat.py --ip <GeoIP.dat> [cn,private,...]

Parses the v2ray protobuf wire format (GeoSiteList / GeoIPList) without
external dependencies and prints category summaries / membership checks.
"""
import sys

# ---------- minimal protobuf wire reader ----------

class Reader:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def eof(self):
        return self.pos >= len(self.data)

    def read_varint(self):
        result = 0
        shift = 0
        while True:
            if self.pos >= len(self.data):
                raise ValueError("truncated varint")
            b = self.data[self.pos]
            self.pos += 1
            result |= (b & 0x7F) << shift
            if not (b & 0x80):
                return result
            shift += 7

    def read_tag(self):
        tag = self.read_varint()
        return tag >> 3, tag & 7

    def read_bytes(self):
        length = self.read_varint()
        if self.pos + length > len(self.data):
            raise ValueError("truncated bytes")
        value = self.data[self.pos:self.pos + length]
        self.pos += length
        return value

    def skip(self, wire):
        if wire == 0:
            self.read_varint()
        elif wire == 1:
            self.pos += 8
        elif wire == 2:
            self.read_bytes()
        elif wire == 5:
            self.pos += 4
        else:
            raise ValueError(f"unsupported wire type {wire}")

    def sub(self, length):
        return Reader(self.read_bytes())


# ---------- geosite ----------

def parse_geosite(path):
    """returns {category: [ (domain_type, value) ]}"""
    with open(path, "rb") as f:
        data = f.read()
    root = Reader(data)
    categories = {}
    while not root.eof():
        field, wire = root.read_tag()
        if field == 1 and wire == 2:
            entry = root.sub(0) if False else Reader(root.read_bytes())
            # entry: GeoSite { string country_code = 1; repeated Domain domain = 2; }
            category = None
            domains = []
            scan = entry
            # first pass: country code
            s2 = Reader(entry.data)
            while not s2.eof():
                f2, w2 = s2.read_tag()
                if f2 == 1 and w2 == 2:
                    category = s2.read_bytes().decode("utf-8", "ignore").strip().lower()
                    break
                s2.skip(w2)
            # second pass: domains
            s3 = Reader(entry.data)
            while not s3.eof():
                f3, w3 = s3.read_tag()
                if f3 == 2 and w3 == 2:
                    dom = parse_site_domain(s3.read_bytes())
                    if dom is not None:
                        domains.append(dom)
                else:
                    s3.skip(w3)
            if category:
                categories[category] = domains
        else:
            root.skip(wire)
    return categories


def parse_site_domain(raw):
    """Domain { Type type = 1; string value = 2; repeated Attribute attribute = 3; }"""
    r = Reader(raw)
    dtype = 0
    value = None
    while not r.eof():
        f, w = r.read_tag()
        if f == 1 and w == 0:
            dtype = r.read_varint()
        elif f == 2 and w == 2:
            value = r.read_bytes().decode("utf-8", "ignore")
        else:
            r.skip(w)
    if value is None:
        return None
    return (dtype, value)


# ---------- geoip ----------

def parse_geoip(path):
    """returns {category: [ (ip_bytes, prefix) ]}"""
    with open(path, "rb") as f:
        data = f.read()
    root = Reader(data)
    categories = {}
    while not root.eof():
        field, wire = root.read_tag()
        if field == 1 and wire == 2:
            raw = root.read_bytes()
            r = Reader(raw)
            category = None
            cidrs = []
            while not r.eof():
                f2, w2 = r.read_tag()
                if f2 == 1 and w2 == 2:
                    category = r.read_bytes().decode("utf-8", "ignore").strip().lower()
                elif f2 == 2 and w2 == 2:
                    c = parse_cidr(r.read_bytes())
                    if c is not None:
                        cidrs.append(c)
                else:
                    r.skip(w2)
            if category:
                categories[category] = cidrs
        else:
            root.skip(wire)
    return categories


def parse_cidr(raw):
    """CIDR { bytes ip = 1; uint32 prefix = 2; }"""
    r = Reader(raw)
    ip = None
    prefix = None
    while not r.eof():
        f, w = r.read_tag()
        if f == 1 and w == 2:
            ip = r.read_bytes()
        elif f == 2 and w == 0:
            prefix = r.read_varint()
        else:
            r.skip(w)
    if ip is None or prefix is None:
        return None
    return (ip, prefix)


# ---------- helpers ----------

def domain_matches(host, dtype, value):
    """same semantics as GeoRuleEngine::MatchDomainRule"""
    host = host.lower()
    if dtype == 0:      # Plain: substring
        return value in host
    if dtype == 2:      # Domain: suffix
        if host == value:
            return True
        if host.endswith("." + value):
            return True
        return False
    if dtype == 3:      # Full: exact
        return host == value
    return False  # regex not checked here


def main():
    args = sys.argv[1:]
    ip_mode = False
    if args and args[0] == "--ip":
        ip_mode = True
        args = args[1:]
    if not args:
        print(__doc__)
        sys.exit(1)

    path = args[0]
    wanted = [a.lower() for a in args[1:]] or None

    if ip_mode:
        cats = parse_geoip(path)
    else:
        cats = parse_geosite(path)

    print(f"parsed {path}: {len(cats)} categories")

    if wanted:
        for w in wanted:
            if w not in cats:
                print(f"  category '{w}': NOT FOUND")
                continue
            entries = cats[w]
            print(f"  category '{w}': {len(entries)} entries")
            if ip_mode:
                for ip, prefix in entries[:10]:
                    print(f"    {'.'.join(str(b) for b in ip)}/{prefix}")
            else:
                names = {"0": "plain", "1": "regex", "2": "domain", "3": "full"}
                for dtype, value in entries[:20]:
                    print(f"    [{names.get(str(dtype), str(dtype))}] {value}")
                # membership checks
                for host in ["baidu.com", "www.baidu.com", "qq.com", "taobao.com",
                             "jd.com", "360.cn", "weibo.com", "zhihu.com",
                             "bilibili.com", "douyin.com", "163.com", "aliyun.com",
                             "mi.com", "meituan.com", "dianping.com", "google.com",
                             "youtube.com", "facebook.com"]:
                    hit = any(domain_matches(host, d, v) for d, v in entries if d != 1)
                    print(f"    match '{host}': {'YES' if hit else 'no'}")
    else:
        for cat, entries in sorted(cats.items()):
            print(f"  {cat}: {len(entries)} entries")


if __name__ == "__main__":
    main()
