#!/usr/bin/env python3
"""Fail closed when runtime logs contain common credential forms.

The report intentionally contains only rule names and line numbers; matching log
text is never echoed, so the scanner itself cannot leak the detected secret.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


RULES = (
    ("guid", re.compile(
        r"(?i)\b[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\b"
    )),
    ("uri_userinfo", re.compile(r"(?i)\b[a-z][a-z0-9+.-]*://[^\s/@:]+:[^\s/@]+@")),
    ("authorization_header", re.compile(r"(?i)\bauthorization\s*[:=]\s*(?:basic|bearer)\s+\S+")),
    ("proxy_authorization_header", re.compile(r"(?i)\bproxy-authorization\s*[:=]\s*\S+")),
    ("credential_assignment", re.compile(
        r"(?i)(?:password|passwd|pwd|token|secret|api[_-]?key|private[_-]?key|debug[_-]?key)"
        r"\s*(?:=|:)\s*['\"]?[^\s,'\"}\]]+"
    )),
    ("credential_argument", re.compile(
        r"(?i)--(?:password|passwd|token|secret|api-key|private-key|debug-key)(?:=|\s+)\S+"
    )),
)


def scan(path: Path, start_byte: int = 0) -> dict[str, object]:
    findings: list[dict[str, object]] = []
    with path.open("rb") as stream:
        size = stream.seek(0, 2)
        offset = min(max(0, start_byte), size)
        stream.seek(offset)
        text = stream.read().decode("utf-8", errors="replace")
    for line_number, line in enumerate(text.splitlines(), 1):
        for rule_name, pattern in RULES:
            if pattern.search(line):
                findings.append({"rule": rule_name, "relative_line": line_number})
    return {
        "path": str(path.resolve()),
        "start_byte": offset,
        "bytes_scanned": size - offset,
        "finding_count": len(findings),
        "findings": findings,
        "passed": not findings,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--start-byte", type=int, default=0)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not args.log.is_file():
        parser.error(f"log file does not exist: {args.log}")
    result = scan(args.log, args.start_byte)
    rendered = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
