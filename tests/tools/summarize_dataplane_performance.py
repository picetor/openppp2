#!/usr/bin/env python3
"""Summarize acceptance JSONL latency and download throughput without dependencies."""

from __future__ import annotations

import argparse
import json
import math
from collections import defaultdict
from pathlib import Path


def nearest_rank(values: list[float], percentile: int) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    index = max(0, math.ceil(len(ordered) * percentile / 100) - 1)
    return round(ordered[index], 3)


def summarize_records(records: list[dict[str, object]]) -> dict[str, object]:
    groups: dict[str, list[dict[str, object]]] = defaultdict(list)
    for record in records:
        if record.get("stage") == "SUMMARY":
            continue
        name = str(record.get("gate") or record.get("protocol") or "unknown")
        if name == "dns" and record.get("query_type"):
            name = f"dns_{str(record['query_type']).lower()}"
        groups[name].append(record)

    result: dict[str, object] = {}
    for name, items in sorted(groups.items()):
        success = [item for item in items if bool(item.get("ok"))]
        elapsed = [float(item["elapsed_ms"]) for item in success if item.get("elapsed_ms") is not None]
        summary: dict[str, object] = {
            "attempts": len(items),
            "succeeded": len(success),
            "failed": len(items) - len(success),
            "elapsed_ms": {
                "p50": nearest_rank(elapsed, 50),
                "p95": nearest_rank(elapsed, 95),
                "p99": nearest_rank(elapsed, 99),
                "max": round(max(elapsed), 3) if elapsed else None,
            },
        }
        throughputs = [
            float(item["bytes"]) * 8.0 / (float(item["elapsed_ms"]) / 1000.0) / 1_000_000.0
            for item in success
            if float(item.get("elapsed_ms") or 0) > 0 and float(item.get("bytes") or 0) > 0
        ]
        if throughputs:
            summary["throughput_mbps"] = {
                "p50": nearest_rank(throughputs, 50),
                "p95": nearest_rank(throughputs, 95),
                "p99": nearest_rank(throughputs, 99),
                "min": round(min(throughputs), 3),
                "max": round(max(throughputs), 3),
            }
        result[name] = summary
    return result


def load_jsonl(path: Path) -> list[dict[str, object]]:
    records: list[dict[str, object]] = []
    with path.open("r", encoding="utf-8-sig") as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            value = json.loads(line)
            if not isinstance(value, dict):
                raise ValueError(f"{path}:{line_number}: JSON value must be an object")
            records.append(value)
    return records


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    records: list[dict[str, object]] = []
    sources: list[str] = []
    for path in args.inputs:
        if path.is_file():
            records.extend(load_jsonl(path))
            sources.append(str(path.resolve()))
    if not records:
        raise SystemExit("no JSONL evidence records found")
    result = {"sources": sources, "groups": summarize_records(records)}
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
