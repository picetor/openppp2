#!/usr/bin/env python3
"""Active IPv4 ICMP/DNS probe with machine-readable APP_COMPLETED evidence.

The probe intentionally uses only the Python standard library. Run it from an
elevated shell on Windows because the ICMP probe opens a raw socket. Every
successful response carries identifiers that can be joined to core dataplane
logs and PktMon records without relying on timing or localized command output.
"""

from __future__ import annotations

import argparse
import datetime as dt
import ipaddress
import json
import os
import secrets
import socket
import struct
import time
from pathlib import Path


def checksum(data: bytes) -> int:
    if len(data) & 1:
        data += b"\0"
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    total = (total & 0xFFFF) + (total >> 16)
    total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def encode_dns_name(name: str) -> bytes:
    labels = name.rstrip(".").split(".")
    encoded = bytearray()
    for label in labels:
        raw = label.encode("idna")
        if not raw or len(raw) > 63:
            raise ValueError(f"invalid DNS label: {label!r}")
        encoded.append(len(raw))
        encoded.extend(raw)
    encoded.append(0)
    return bytes(encoded)


def ptr_query_name(value: str) -> str:
    address = ipaddress.ip_address(value)
    return address.reverse_pointer


class EvidenceWriter:
    def __init__(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        self._stream = path.open("x", encoding="utf-8", newline="\n")

    def close(self) -> None:
        self._stream.close()

    def write(self, record: dict[str, object]) -> None:
        record = {
            "wall_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
            "monotonic_ns": time.monotonic_ns(),
            **record,
        }
        self._stream.write(json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n")
        self._stream.flush()


def icmp_probe(
    writer: EvidenceWriter,
    tun_address: str,
    target: str,
    count: int,
    timeout: float,
) -> tuple[int, int]:
    identifier = (os.getpid() ^ secrets.randbits(16)) & 0xFFFF
    succeeded = 0
    failed = 0
    with socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_ICMP) as probe:
        probe.bind((tun_address, 0))
        for sequence in range(1, count + 1):
            payload = b"OPENPPP2-PROBE" + struct.pack("!Q", time.monotonic_ns())
            header = struct.pack("!BBHHH", 8, 0, 0, identifier, sequence & 0xFFFF)
            packet = header[:2] + struct.pack("!H", checksum(header + payload)) + header[4:] + payload
            started = time.monotonic_ns()
            error: str | None = None
            ok = False
            try:
                probe.sendto(packet, (target, 0))
                deadline = time.monotonic() + timeout
                while True:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError("ICMP response timeout")
                    probe.settimeout(remaining)
                    response, source = probe.recvfrom(65535)
                    offset = (response[0] & 0x0F) * 4 if response and response[0] >> 4 == 4 else 0
                    if len(response) < offset + 8:
                        continue
                    icmp_type, icmp_code, _, reply_id, reply_sequence = struct.unpack_from(
                        "!BBHHH", response, offset
                    )
                    if (
                        icmp_type == 0
                        and icmp_code == 0
                        and reply_id == identifier
                        and reply_sequence == (sequence & 0xFFFF)
                        and source[0] == target
                    ):
                        ok = True
                        break
            except (OSError, TimeoutError) as exc:
                error = str(exc)
            elapsed_ms = round((time.monotonic_ns() - started) / 1_000_000, 3)
            succeeded += int(ok)
            failed += int(not ok)
            writer.write(
                {
                    "stage": "APP_COMPLETED" if ok else "APP_TIMEOUT",
                    "kind": "icmp",
                    "ok": ok,
                    "attempt": sequence,
                    "elapsed_ms": elapsed_ms,
                    "address_family": 4,
                    "protocol": socket.IPPROTO_ICMP,
                    "src_ip": target,
                    "src_port": 0,
                    "dst_ip": tun_address,
                    "dst_port": 0,
                    "icmp_type": 0,
                    "icmp_code": 0,
                    "icmp_identifier": identifier,
                    "icmp_sequence": sequence & 0xFFFF,
                    "dns_transaction_id": -1,
                    "error": error,
                }
            )
    return succeeded, failed


def dns_probe(
    writer: EvidenceWriter,
    tun_address: str,
    server: str,
    name: str,
    query_type: int,
    query_type_name: str,
    count: int,
    timeout: float,
) -> tuple[int, int]:
    succeeded = 0
    failed = 0
    for attempt in range(1, count + 1):
        transaction_id = secrets.randbelow(65536)
        query = (
            struct.pack("!HHHHHH", transaction_id, 0x0100, 1, 0, 0, 0)
            + encode_dns_name(name)
            + struct.pack("!HH", query_type, 1)
        )
        started = time.monotonic_ns()
        ok = False
        error: str | None = None
        response_count = 0
        local_port = 0
        response_source = server
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP) as probe:
                probe.bind((tun_address, 0))
                local_port = probe.getsockname()[1]
                probe.settimeout(timeout)
                probe.sendto(query, (server, 53))
                while True:
                    response, source = probe.recvfrom(65535)
                    if len(response) < 12:
                        continue
                    reply_id, flags, _, response_count, _, _ = struct.unpack_from("!HHHHHH", response)
                    if reply_id != transaction_id:
                        continue
                    if not (flags & 0x8000):
                        continue
                    response_source = source[0]
                    rcode = flags & 0x000F
                    if rcode != 0:
                        raise RuntimeError(f"DNS rcode={rcode}")
                    ok = True
                    break
        except (OSError, RuntimeError) as exc:
            error = str(exc)
        elapsed_ms = round((time.monotonic_ns() - started) / 1_000_000, 3)
        succeeded += int(ok)
        failed += int(not ok)
        writer.write(
            {
                "stage": "APP_COMPLETED" if ok else "APP_TIMEOUT",
                "kind": "dns",
                "ok": ok,
                "attempt": attempt,
                "query_type": query_type_name,
                "query_name": name,
                "answer_count": response_count,
                "elapsed_ms": elapsed_ms,
                "address_family": 4,
                "protocol": socket.IPPROTO_UDP,
                "src_ip": response_source,
                "src_port": 53,
                "dst_ip": tun_address,
                "dst_port": local_port,
                "dns_transaction_id": transaction_id,
                "icmp_type": -1,
                "icmp_code": -1,
                "icmp_identifier": -1,
                "icmp_sequence": -1,
                "error": error,
            }
        )
    return succeeded, failed


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tun-address", required=True)
    parser.add_argument("--ping-target", default="1.1.1.1")
    parser.add_argument("--dns-server", required=True)
    parser.add_argument("--dns-name", default="example.com")
    parser.add_argument("--dns-ptr-address", default="1.1.1.1")
    parser.add_argument("--icmp-count", type=int, default=100)
    parser.add_argument("--dns-count", type=int, default=100)
    parser.add_argument("--timeout", type=float, default=2.0)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if not 1 <= args.icmp_count <= 1000 or not 1 <= args.dns_count <= 1000:
        raise SystemExit("--icmp-count and --dns-count must be in 1..1000")
    if not 0.05 <= args.timeout <= 60:
        raise SystemExit("--timeout must be in 0.05..60 seconds")
    for value, label in ((args.tun_address, "tun"), (args.ping_target, "ping"), (args.dns_server, "dns")):
        if ipaddress.ip_address(value).version != 4:
            raise SystemExit(f"{label} address must be IPv4: {value}")

    writer = EvidenceWriter(args.output)
    totals: dict[str, dict[str, int]] = {}
    try:
        success, failure = icmp_probe(
            writer, args.tun_address, args.ping_target, args.icmp_count, args.timeout
        )
        totals["icmp"] = {"succeeded": success, "failed": failure}
        for query_type, query_type_name, query_name in (
            (1, "A", args.dns_name),
            (28, "AAAA", args.dns_name),
            (12, "PTR", ptr_query_name(args.dns_ptr_address)),
        ):
            success, failure = dns_probe(
                writer,
                args.tun_address,
                args.dns_server,
                query_name,
                query_type,
                query_type_name,
                args.dns_count,
                args.timeout,
            )
            totals[f"dns_{query_type_name.lower()}"] = {
                "succeeded": success,
                "failed": failure,
            }
        writer.write({"stage": "SUMMARY", "totals": totals})
    finally:
        writer.close()

    print(json.dumps({"output": str(args.output.resolve()), "totals": totals}, indent=2))
    allowed_icmp_failures = args.icmp_count // 100
    return 0 if totals["icmp"]["failed"] <= allowed_icmp_failures and all(
        totals[name]["failed"] == 0 for name in ("dns_a", "dns_aaaa", "dns_ptr")
    ) else 1


if __name__ == "__main__":
    raise SystemExit(main())
