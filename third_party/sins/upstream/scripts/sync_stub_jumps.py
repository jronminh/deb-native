#!/usr/bin/env python3
"""
Regenerate JUMP_TO(...) trampolines in pkg/libsystemd/stub.c from
pkg/libsystemd/libsystemd.map.

Does not require systemd or libsystemd.so on the build machine.
Run after editing the version script when elogind/systemd ABI changes.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAP = ROOT / "pkg/libsystemd/libsystemd.map"
STUB = ROOT / "pkg/libsystemd/stub.c"

BEGIN = "/* --- BEGIN GENERATED JUMP_TO (libsystemd.map) --- */\n"
END = "/* --- END GENERATED JUMP_TO --- */\n"

# Implemented in stub.c before the generated block (not forwarded).
SKIP = frozenset(
    {
        "sd_booted",
        "sd_bus_call_method",
        "sd_pid_get_session",
        "sd_session_is_active",
        "sd_session_get_seat",
    }
)


def symbols_from_map(path: Path) -> list[str]:
    out: set[str] = set()
    for line in path.read_text().splitlines():
        m = re.match(r"\s+(sd_[a-z0-9_]+);", line)
        if m:
            out.add(m.group(1))
    return sorted(out)


def jump_lines(symbols: list[str]) -> str:
    lines: list[str] = []
    for s in symbols:
        if s.startswith("sd_journal_") or s in SKIP:
            continue
        lines.append(f"JUMP_TO({s})\n")
    return "".join(lines)


def replace_generated(stub_text: str, generated_body: str) -> str:
    if BEGIN not in stub_text or END not in stub_text:
        sys.stderr.write(
            f"{STUB}: missing BEGIN/END markers for generated JUMP_TO block\n"
        )
        sys.exit(1)
    pre, rest = stub_text.split(BEGIN, 1)
    _old_body, post = rest.split(END, 1)
    return pre + BEGIN + generated_body + END + post


def main() -> None:
    syms = symbols_from_map(MAP)
    body = jump_lines(syms)
    text = STUB.read_text()
    new_text = replace_generated(text, body)
    if new_text == text:
        print("stub.c: JUMP_TO block already up to date")
        return
    STUB.write_text(new_text)
    print(f"stub.c: wrote {body.count('JUMP_TO')} JUMP_TO lines from {MAP.name}")


if __name__ == "__main__":
    main()
