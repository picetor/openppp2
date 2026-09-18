#!/usr/bin/env python3
"""Zero-dependency pcapng analyzer for openppp2 PktMon acceptance evidence."""

from __future__ import annotations

import argparse
import hashlib
import ipaddress
import json
import pathlib
import re
import struct
from collections import Counter
from dataclasses import dataclass
from typing import Iterable


def checksum_valid(data: bytes) -> bool:
    if len(data) & 1:
        data += b"\0"
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return total == 0xFFFF


def transport_checksum_valid(packet: bytes, offset: int, protocol: int) -> bool | None:
    segment = packet[offset:]
    if protocol == 1:
        return checksum_valid(segment)
    if protocol not in (6, 17):
        return None
    if protocol == 17 and len(segment) >= 8 and segment[6:8] == b"\0\0":
        return True
    pseudo = packet[12:20] + bytes((0, protocol)) + struct.pack("!H", len(segment))
    return checksum_valid(pseudo + segment)


def transport_checksum_valid_v6(packet: bytes, offset: int, protocol: int) -> bool | None:
    segment = packet[offset:]
    if protocol not in (6, 17, 58):
        return None
    if protocol == 17 and len(segment) >= 8 and segment[6:8] == b"\0\0":
        return False
    pseudo = packet[8:40] + struct.pack("!I", len(segment)) + bytes((0, 0, 0, protocol))
    return checksum_valid(pseudo + segment)


@dataclass(frozen=True)
class PacketRecord:
    timestamp_us: float
    packet: bytes


def iter_pcapng_packets(path: pathlib.Path) -> Iterable[PacketRecord]:
    blob = path.read_bytes()
    offset = 0
    endian = "<"
    timestamp_resolution: dict[int, float] = {}
    interfaces = 0
    while offset + 12 <= len(blob):
        raw_type = blob[offset : offset + 4]
        if raw_type == b"\x0a\x0d\x0d\x0a":
            if offset + 12 > len(blob):
                raise ValueError(f"truncated section header at {offset}")
            bom = blob[offset + 8 : offset + 12]
            if bom == b"\x4d\x3c\x2b\x1a":
                endian = "<"
            elif bom == b"\x1a\x2b\x3c\x4d":
                endian = ">"
            else:
                raise ValueError(f"invalid byte-order magic at {offset}")
        block_type, block_length = struct.unpack_from(endian + "II", blob, offset)
        if block_length < 12 or offset + block_length > len(blob):
            raise ValueError(f"invalid block length {block_length} at {offset}")
        if struct.unpack_from(endian + "I", blob, offset + block_length - 4)[0] != block_length:
            raise ValueError(f"block length trailer mismatch at {offset}")
        if block_type == 1:
            resolution = 1e-6
            options_offset = offset + 16
            options_end = offset + block_length - 4
            while options_offset + 4 <= options_end:
                code, length = struct.unpack_from(endian + "HH", blob, options_offset)
                options_offset += 4
                if code == 0:
                    break
                value = blob[options_offset : options_offset + length]
                if code == 9 and value:
                    raw = value[0]
                    resolution = (2.0 ** -(raw & 0x7F)) if raw & 0x80 else (10.0 ** -raw)
                options_offset += (length + 3) & ~3
            timestamp_resolution[interfaces] = resolution
            interfaces += 1
        elif block_type == 6:
            interface_id, timestamp_high, timestamp_low, captured_length = struct.unpack_from(
                endian + "IIII", blob, offset + 8
            )
            packet_start = offset + 28
            packet_end = packet_start + captured_length
            if packet_end > offset + block_length - 4:
                raise ValueError(f"truncated enhanced packet at {offset}")
            ticks = (timestamp_high << 32) | timestamp_low
            resolution = timestamp_resolution.get(interface_id, 1e-6)
            yield PacketRecord(ticks * resolution * 1_000_000.0, blob[packet_start:packet_end])
        offset += block_length
    if offset != len(blob):
        raise ValueError(f"trailing {len(blob) - offset} bytes after final block")


def find_ipv4(packet_data: bytes) -> bytes | None:
    candidates: list[tuple[int, int, bytes]] = []
    for offset in range(max(0, len(packet_data) - 19)):
        if packet_data[offset] >> 4 != 4:
            continue
        header_length = (packet_data[offset] & 0x0F) * 4
        if header_length < 20 or offset + header_length > len(packet_data):
            continue
        total_length = struct.unpack_from("!H", packet_data, offset + 2)[0]
        if total_length < header_length or offset + total_length > len(packet_data):
            continue
        header = packet_data[offset : offset + header_length]
        if not checksum_valid(header):
            continue
        trailing = len(packet_data) - offset - total_length
        candidates.append((trailing, offset, packet_data[offset : offset + total_length]))
    if not candidates:
        return None
    candidates.sort(key=lambda item: (item[0], item[1]))
    return candidates[0][2]


def find_ipv6(packet_data: bytes) -> bytes | None:
    candidates: list[tuple[int, int, bytes]] = []
    for offset in range(max(0, len(packet_data) - 39)):
        if packet_data[offset] >> 4 != 6:
            continue
        payload_length = struct.unpack_from("!H", packet_data, offset + 4)[0]
        total_length = 40 + payload_length
        if offset + total_length > len(packet_data):
            continue
        candidate = packet_data[offset : offset + total_length]
        try:
            locate_ipv6_transport(candidate)
        except ValueError:
            continue
        trailing = len(packet_data) - offset - total_length
        candidates.append((trailing, offset, candidate))
    if not candidates:
        return None
    candidates.sort(key=lambda item: (item[0], item[1]))
    return candidates[0][2]


def locate_ipv6_transport(packet: bytes) -> tuple[int, int, bool]:
    if len(packet) < 40 or packet[0] >> 4 != 6 or 40 + struct.unpack_from("!H", packet, 4)[0] != len(packet):
        raise ValueError("invalid IPv6 base header or payload length")
    cursor = 40
    next_header = packet[6]
    extension_count = 0
    fragment_seen = False
    fragmented = False
    while True:
        if next_header in (0, 43, 60, 51):
            extension_count += 1
            if extension_count > 8 or cursor + 2 > len(packet):
                raise ValueError("invalid IPv6 extension chain")
            extension_length = (
                (packet[cursor + 1] + 2) * 4
                if next_header == 51
                else (packet[cursor + 1] + 1) * 8
            )
            if extension_length < 8 or cursor + extension_length > len(packet):
                raise ValueError("invalid IPv6 extension length")
            next_header = packet[cursor]
            cursor += extension_length
            continue
        if next_header == 44:
            extension_count += 1
            if fragment_seen or extension_count > 8 or cursor + 8 > len(packet):
                raise ValueError("invalid or duplicate IPv6 fragment header")
            fragment_seen = True
            fragment_flags = struct.unpack_from("!H", packet, cursor + 2)[0]
            if fragment_flags & 0x0006:
                raise ValueError("IPv6 fragment reserved bits are set")
            fragmented = bool(fragment_flags & 0xFFF9)
            next_header = packet[cursor]
            cursor += 8
            continue
        break
    return cursor, next_header, not fragmented and next_header in (6, 17, 58)


def tcp_mss(segment: bytes) -> int | None:
    if len(segment) < 20 or not (segment[13] & 0x02):
        return None
    header_length = (segment[12] >> 4) * 4
    if header_length < 20 or header_length > len(segment):
        return None
    cursor = 20
    while cursor < header_length:
        kind = segment[cursor]
        if kind == 0:
            break
        if kind == 1:
            cursor += 1
            continue
        if cursor + 2 > header_length:
            return None
        length = segment[cursor + 1]
        if length < 2 or cursor + length > header_length:
            return None
        if kind == 2 and length == 4:
            return struct.unpack_from("!H", segment, cursor + 2)[0]
        cursor += length
    return None


def trace_key(fields: dict[str, str]) -> str | None:
    try:
        address_family = int(fields["address_family"])
        protocol = int(fields["protocol"])
        source = fields["src_ip"]
        destination = fields["dst_ip"]
        if (address_family == 4 and protocol == 1) or (address_family == 6 and protocol == 58):
            icmp_type = int(fields["icmp_type"])
            identifier = int(fields["icmp_identifier"])
            sequence = int(fields["icmp_sequence"])
            if icmp_type < 0 or identifier < 0 or sequence < 0:
                return None
            return f"icmp|{address_family}|{protocol}|{source}|{destination}|{icmp_type}|{identifier}|{sequence}"
        if address_family in (4, 6) and protocol == 17:
            transaction_id = int(fields["dns_transaction_id"])
            if transaction_id < 0:
                return None
            return (
                f"dns|{address_family}|{source}|{int(fields['src_port'])}|{destination}|"
                f"{int(fields['dst_port'])}|{transaction_id}"
            )
    except (KeyError, ValueError):
        return None
    return None


def parse_core_submissions(path: pathlib.Path) -> list[tuple[int, str | None]]:
    submissions: list[tuple[int, str | None]] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "DATAPLANE WINTUN_SUBMITTED" not in line:
            continue
        fields = dict(re.findall(r"([a-z_]+)=([^\s]+)", line))
        try:
            flow_id = int(fields["flow_id"])
        except (KeyError, ValueError):
            continue
        submissions.append((flow_id, trace_key(fields)))
    return submissions


def parse_core_stage_flow_ids(path: pathlib.Path, marker: str) -> set[int]:
    flow_ids: set[int] = set()
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if marker not in line:
            continue
        match = re.search(r"\bflow_id=(\d+)\b", line)
        if match:
            flow_ids.add(int(match.group(1)))
    return flow_ids


def parse_app_completions(path: pathlib.Path) -> set[str]:
    keys: set[str] = set()
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            record = json.loads(line)
        except json.JSONDecodeError:
            continue
        if record.get("stage") != "APP_COMPLETED" or not record.get("ok"):
            continue
        key = trace_key({name: str(value) for name, value in record.items()})
        if key is not None:
            keys.add(key)
    return keys


def analyze(
    paths: Iterable[pathlib.Path],
    core_log: pathlib.Path | None = None,
    app_log: pathlib.Path | None = None,
) -> dict[str, object]:
    occurrence_count = 0
    ipv4_occurrences = 0
    ipv6_occurrences = 0
    invalid_transport: Counter[int] = Counter()
    unique_packets: set[str] = set()
    icmp_requests: dict[tuple[str, str, int, int], float] = {}
    icmp_replies: dict[tuple[str, str, int, int], float] = {}
    dns_queries: dict[tuple[str, str, int, int, int], float] = {}
    dns_responses: dict[tuple[str, str, int, int, int], float] = {}
    mss_values: Counter[int] = Counter()
    observed_trace_keys: set[str] = set()

    for path in paths:
        for record in iter_pcapng_packets(path):
            occurrence_count += 1
            packet = find_ipv4(record.packet)
            address_family = 4
            if packet is None:
                packet = find_ipv6(record.packet)
                address_family = 6
            if packet is None:
                continue
            if address_family == 4:
                ipv4_occurrences += 1
            else:
                ipv6_occurrences += 1
            unique_packets.add(hashlib.sha256(packet).hexdigest())
            if address_family == 4:
                header_length = (packet[0] & 0x0F) * 4
                protocol = packet[9]
                source = str(ipaddress.IPv4Address(packet[12:16]))
                destination = str(ipaddress.IPv4Address(packet[16:20]))
                fragment = struct.unpack_from("!H", packet, 6)[0]
                verifiable = (fragment & 0x3FFF) == 0
                valid = transport_checksum_valid(packet, header_length, protocol) if verifiable else None
            else:
                header_length, protocol, verifiable = locate_ipv6_transport(packet)
                source = str(ipaddress.IPv6Address(packet[8:24]))
                destination = str(ipaddress.IPv6Address(packet[24:40]))
                valid = transport_checksum_valid_v6(packet, header_length, protocol) if verifiable else None
            if valid is False:
                invalid_transport[address_family] += 1
            payload = packet[header_length:]
            echo_types = (0, 8) if address_family == 4 else (128, 129)
            if protocol in (1, 58) and len(payload) >= 8 and payload[0] in echo_types:
                identifier, sequence = struct.unpack_from("!HH", payload, 4)
                request_type = 8 if address_family == 4 else 128
                if payload[0] == request_type:
                    key = (source, destination, identifier, sequence)
                    icmp_requests.setdefault(key, record.timestamp_us)
                else:
                    key = (destination, source, identifier, sequence)
                    icmp_replies.setdefault(key, record.timestamp_us)
                observed_trace_keys.add(
                    f"icmp|{address_family}|{protocol}|{source}|{destination}|{payload[0]}|{identifier}|{sequence}"
                )
            elif protocol == 17 and len(payload) >= 20:
                source_port, destination_port, udp_length = struct.unpack_from("!HHH", payload, 0)
                if udp_length >= 20 and udp_length <= len(payload) and (source_port == 53 or destination_port == 53):
                    dns_id, dns_flags = struct.unpack_from("!HH", payload, 8)
                    if dns_flags & 0x8000:
                        key = (destination, source, destination_port, source_port, dns_id)
                        dns_responses.setdefault(key, record.timestamp_us)
                    else:
                        key = (source, destination, source_port, destination_port, dns_id)
                        dns_queries.setdefault(key, record.timestamp_us)
                    observed_trace_keys.add(
                        f"dns|{address_family}|{source}|{source_port}|{destination}|{destination_port}|{dns_id}"
                    )
            elif protocol == 6 and len(payload) >= 20:
                value = tcp_mss(payload)
                if value is not None:
                    mss_values[value] += 1

    matched_icmp = sorted(set(icmp_requests) & set(icmp_replies))
    matched_dns = sorted(set(dns_queries) & set(dns_responses))
    icmp_latency = [icmp_replies[key] - icmp_requests[key] for key in matched_icmp]
    dns_latency = [dns_responses[key] - dns_queries[key] for key in matched_dns]

    def latency(values: list[float]) -> dict[str, float | None]:
        if not values:
            return {"min_ms": None, "max_ms": None, "mean_ms": None}
        values_ms = [value / 1000.0 for value in values]
        return {
            "min_ms": round(min(values_ms), 3),
            "max_ms": round(max(values_ms), 3),
            "mean_ms": round(sum(values_ms) / len(values_ms), 3),
        }

    correlation: dict[str, object] = {"available": False}
    if core_log is not None:
        submissions = parse_core_submissions(core_log)
        tun_rx_flow_ids = parse_core_stage_flow_ids(core_log, "DATAPLANE TUN_RX")
        policy_flow_ids = parse_core_stage_flow_ids(core_log, "DATAPLANE POLICY_SELECTED")
        remote_tx_flow_ids = parse_core_stage_flow_ids(core_log, "DATAPLANE REMOTE_TX")
        remote_rx_flow_ids = parse_core_stage_flow_ids(core_log, "DATAPLANE REMOTE_RX")
        local_rx_flow_ids = parse_core_stage_flow_ids(core_log, "DATAPLANE LOCAL_RX")
        built_flow_ids = parse_core_stage_flow_ids(core_log, "DATAPLANE PACKET_BUILT")
        allocated_flow_ids = parse_core_stage_flow_ids(core_log, "DATAPLANE WINTUN_ALLOCATED")
        submitted_flow_id_set = parse_core_stage_flow_ids(core_log, "DATAPLANE WINTUN_SUBMITTED")
        correlatable = [(flow_id, key) for flow_id, key in submissions if key is not None]
        matched_flow_ids = sorted(flow_id for flow_id, key in correlatable if key in observed_trace_keys)
        missing_flow_ids = sorted(flow_id for flow_id, key in correlatable if key not in observed_trace_keys)
        flow_ids = [flow_id for flow_id, _ in submissions]
        request_correlated_flow_ids = sorted(set(flow_ids) & tun_rx_flow_ids)
        end_to_end_flow_ids = sorted(set(matched_flow_ids) & tun_rx_flow_ids)
        request_path_complete = sorted(tun_rx_flow_ids & policy_flow_ids & remote_tx_flow_ids)
        remote_roundtrip_complete = sorted(
            tun_rx_flow_ids & policy_flow_ids & remote_tx_flow_ids & remote_rx_flow_ids &
            built_flow_ids & allocated_flow_ids & submitted_flow_id_set
        )
        local_roundtrip_complete = sorted(
            tun_rx_flow_ids & policy_flow_ids & local_rx_flow_ids &
            built_flow_ids & allocated_flow_ids & submitted_flow_id_set
        )
        remote_os_observed_complete = sorted(set(remote_roundtrip_complete) & set(matched_flow_ids))
        local_os_observed_complete = sorted(set(local_roundtrip_complete) & set(matched_flow_ids))
        injection_sources = remote_rx_flow_ids | local_rx_flow_ids
        app_trace_keys = parse_app_completions(app_log) if app_log is not None else set()
        app_completed_flow_ids = sorted(
            flow_id for flow_id, key in correlatable if key in app_trace_keys
        )
        fully_observed_flow_ids = sorted(
            (set(remote_os_observed_complete) | set(local_os_observed_complete)) &
            set(app_completed_flow_ids)
        )
        correlation = {
            "available": True,
            "core_log": str(core_log.resolve()),
            "submitted_traces": len(submissions),
            "unique_flow_ids": len(set(flow_ids)),
            "duplicate_flow_ids": len(flow_ids) - len(set(flow_ids)),
            "tun_rx_flow_ids": len(tun_rx_flow_ids),
            "policy_selected_flow_ids": len(policy_flow_ids),
            "remote_tx_flow_ids": len(remote_tx_flow_ids),
            "remote_rx_flow_ids": len(remote_rx_flow_ids),
            "local_rx_flow_ids": len(local_rx_flow_ids),
            "request_correlated_submissions": len(request_correlated_flow_ids),
            "correlatable_icmp_or_dns": len(correlatable),
            "os_observed_matches": len(matched_flow_ids),
            "os_observed_missing": len(missing_flow_ids),
            "matched_flow_ids": matched_flow_ids,
            "missing_flow_ids": missing_flow_ids,
            "tun_rx_to_os_observed_matches": len(end_to_end_flow_ids),
            "tun_rx_to_os_observed_flow_ids": end_to_end_flow_ids,
            "request_path_complete": len(request_path_complete),
            "request_path_complete_flow_ids": request_path_complete,
            "tun_rx_missing_policy_flow_ids": sorted(tun_rx_flow_ids - policy_flow_ids),
            "policy_missing_remote_tx_flow_ids": sorted(policy_flow_ids - remote_tx_flow_ids),
            "remote_roundtrip_complete": len(remote_roundtrip_complete),
            "remote_roundtrip_complete_flow_ids": remote_roundtrip_complete,
            "local_roundtrip_complete": len(local_roundtrip_complete),
            "local_roundtrip_complete_flow_ids": local_roundtrip_complete,
            "remote_os_observed_complete": len(remote_os_observed_complete),
            "remote_os_observed_complete_flow_ids": remote_os_observed_complete,
            "local_os_observed_complete": len(local_os_observed_complete),
            "local_os_observed_complete_flow_ids": local_os_observed_complete,
            "app_log_available": app_log is not None,
            "app_log": str(app_log.resolve()) if app_log is not None else None,
            "app_completed_matches": len(app_completed_flow_ids),
            "app_completed_flow_ids": app_completed_flow_ids,
            "fully_observed_matches": len(fully_observed_flow_ids),
            "fully_observed_flow_ids": fully_observed_flow_ids,
            "injection_stage_sets_match": (
                injection_sources == built_flow_ids == allocated_flow_ids == submitted_flow_id_set
            ),
            "injection_missing_built_flow_ids": sorted(injection_sources - built_flow_ids),
            "built_missing_allocated_flow_ids": sorted(built_flow_ids - allocated_flow_ids),
            "allocated_missing_submitted_flow_ids": sorted(allocated_flow_ids - submitted_flow_id_set),
        }

    return {
        "capture_files": [str(path.resolve()) for path in paths],
        "packet_occurrences": occurrence_count,
        "ipv4_occurrences": ipv4_occurrences,
        "ipv6_occurrences": ipv6_occurrences,
        "unique_ip_packets": len(unique_packets),
        "checksum_validation": {
            "ipv4_failed_occurrences": invalid_transport[4],
            "ipv6_failed_occurrences": invalid_transport[6],
            "capture_stage_caveat": (
                "PktMon can include checksum-offload stages; a failed captured checksum is not "
                "an on-wire defect until correlated with component/direction evidence."
            ),
        },
        "icmp": {
            "unique_requests": len(icmp_requests),
            "unique_replies": len(icmp_replies),
            "matched": len(matched_icmp),
            "requests_without_reply": len(set(icmp_requests) - set(icmp_replies)),
            "replies_without_observed_request": len(set(icmp_replies) - set(icmp_requests)),
            "latency": latency(icmp_latency),
        },
        "dns": {
            "unique_queries": len(dns_queries),
            "unique_responses": len(dns_responses),
            "matched": len(matched_dns),
            "queries_without_response": len(set(dns_queries) - set(dns_responses)),
            "responses_without_observed_query": len(set(dns_responses) - set(dns_queries)),
            "latency": latency(dns_latency),
        },
        "tcp_syn_mss_occurrences": {str(key): value for key, value in sorted(mss_values.items())},
        "core_os_correlation": correlation,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pcapng", nargs="+", type=pathlib.Path)
    parser.add_argument("--core-log", type=pathlib.Path)
    parser.add_argument("--app-log", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    result = analyze(args.pcapng, args.core_log, args.app_log)
    rendered = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
