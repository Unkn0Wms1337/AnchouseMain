#!/usr/bin/env python3
"""Refresh src/Misc/Offsets.h from a dota2-offset-dumper output folder.

Usage:
    py -3 scripts/update_offsets.py <dump_dir> [offsets_header] [--dry-run]

Only values the dumper can actually supply are touched:

    Offsets::Engine            <- offsets.json ("engine") / engine_offsets.hpp
    Offsets::C_EconItemView    <- client.dll.hpp (fields + class size)
    Offsets::C_BaseEntity      <- client.dll.hpp (m_Particles -> ParticleProperty)

Everything else in Offsets.h is manual layout knowledge and stays untouched.
The file is rewritten in place and only when a value really changed, so the
build always compiles against the newest dump without losing local edits.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

VALUE_RE = re.compile(
    r"^(\s*inline constexpr\s+[^=]+?\b(\w+)\s*=\s*)(0[xX][0-9A-Fa-f]+|\d+)(\s*;.*)$"
)
NAMESPACE_RE = re.compile(r"^\s*namespace\s+(Offsets::[A-Za-z0-9_]+)\s*$")

ENGINE_KEYS = ("dwEntityList", "dwViewMatrix", "dwLocalPlayerController")

ECON_VIEW_FIELDS = {
    "ItemDefinitionIndex": "m_iItemDefinitionIndex",
    "ItemId": "m_iItemID",
    "AccountId": "m_iAccountID",
    "Style": "m_nOverrideStyle",
}


def log(message: str) -> None:
    print(message, flush=True)


def load_engine(dump: Path) -> dict[str, int]:
    values: dict[str, int] = {}
    offsets_json = dump / "offsets.json"
    if offsets_json.is_file():
        try:
            data = json.loads(offsets_json.read_text(encoding="utf-8-sig", errors="replace"))
        except Exception as exc:  # pragma: no cover - unreadable dump
            log(f"offsets: offsets.json unreadable ({exc})")
            data = {}
        for key, value in (data.get("engine") or {}).items():
            if isinstance(value, int) and value > 0:
                values[key] = value
        build = data.get("build")
        if build:
            log(f"offsets: dump build {build}")
    if not values:
        header = dump / "engine_offsets.hpp"
        if header.is_file():
            text = header.read_text(encoding="utf-8", errors="replace")
            for key, value in re.findall(r"auto\s+(\w+)\s*=\s*(0[xX][0-9A-Fa-f]+|\d+)", text):
                values[key] = int(value, 0)
    return values


def class_block(text: str, name: str) -> dict[str, int] | None:
    match = re.search(r"namespace\s+" + re.escape(name) + r"\s*\{(.*?)\n\s*\}", text, re.S)
    if not match:
        return None
    fields: dict[str, int] = {}
    for key, value in re.findall(
        r"constexpr\s+auto\s+(\w+)\s*=\s*(0[xX][0-9A-Fa-f]+|\d+)\s*;", match.group(1)
    ):
        fields[key] = int(value, 0)
    return fields


def class_size(text: str, name: str) -> int | None:
    match = re.search(
        r"//\s*" + re.escape(name) + r"[^\n]*?\bsize\s+(0[xX][0-9A-Fa-f]+|\d+)", text
    )
    return int(match.group(1), 0) if match else None


def collect_updates(dump: Path) -> dict[tuple[str, str], int]:
    updates: dict[tuple[str, str], int] = {}

    def add(namespace: str, key: str, value: int) -> None:
        # A zero here always means "the dumper could not resolve it", never a
        # real offset for the fields we map, so it is safer to keep the header.
        if value > 0:
            updates[(namespace, key)] = value

    engine = load_engine(dump)
    for key in ENGINE_KEYS:
        if key in engine:
            add("Offsets::Engine", key, engine[key])

    client_header = dump / "client.dll.hpp"
    if not client_header.is_file():
        return updates

    text = client_header.read_text(encoding="utf-8", errors="replace")

    econ = class_block(text, "C_EconItemView") or {}
    for ours, theirs in ECON_VIEW_FIELDS.items():
        if theirs in econ:
            add("Offsets::C_EconItemView", ours, econ[theirs])
    size = class_size(text, "C_EconItemView")
    if size:
        add("Offsets::C_EconItemView", "Size", size)

    base_entity = class_block(text, "C_BaseEntity") or {}
    if "m_Particles" in base_entity:
        add("Offsets::C_BaseEntity", "ParticleProperty", base_entity["m_Particles"])

    return updates


def patch_header(path: Path, updates: dict[tuple[str, str], int], dry_run: bool) -> list[str]:
    with open(path, "r", encoding="utf-8", newline="") as handle:
        lines = handle.readlines()

    namespace: str | None = None
    changes: list[str] = []

    for index, raw in enumerate(lines):
        body = raw.rstrip("\r\n")
        ending = raw[len(body):]

        namespace_match = NAMESPACE_RE.match(body)
        if namespace_match:
            namespace = namespace_match.group(1)
            continue

        if namespace is None:
            continue

        value_match = VALUE_RE.match(body)
        if not value_match:
            continue

        head, key, current, tail = value_match.groups()
        wanted = updates.get((namespace, key))
        if wanted is None or int(current, 0) == wanted:
            continue

        lines[index] = f"{head}0x{wanted:X}{tail}{ending}"
        changes.append(f"~ Offsets::{namespace.split('::', 1)[1]}::{key} {current} -> 0x{wanted:X}")

    if changes and not dry_run:
        with open(path, "w", encoding="utf-8", newline="") as handle:
            handle.writelines(lines)

    return changes


def main(argv: list[str]) -> int:
    args = [arg for arg in argv[1:] if not arg.startswith("--")]
    dry_run = "--dry-run" in argv[1:]

    if not args:
        log(__doc__.strip().splitlines()[2].strip())
        return 64

    dump = Path(args[0])
    header = Path(args[1]) if len(args) > 1 else Path("src/Misc/Offsets.h")

    if not header.is_file():
        log(f"offsets: header not found: {header}")
        return 3

    updates = collect_updates(dump)
    if not updates:
        log(f"offsets: no usable values in {dump}, keeping the current header")
        return 2

    changes = patch_header(header, updates, dry_run)

    if not changes:
        log(f"offsets: up to date ({len(updates)} values checked)")
        return 0

    for change in changes:
        log(change)
    suffix = " (dry run)" if dry_run else ""
    log(f"offsets: {len(changes)} value(s) updated in {header}{suffix}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
