#!/usr/bin/env python3
"""Check the fuxi audio binding against the facts it rests on.

The binding names a value, the state of that value and the fact behind it. This refuses the two ways an
unverified value could reach a build: a "verified" entry whose evidence is missing or still unverified, and a
layer field the binding does not cover at all. With --require-verified it refuses every unverified entry, which
is what a build that consumes the binding has to do.
"""

from __future__ import annotations

import re
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINDING = ROOT / "facts/audio/binding.toml"
MANIFEST = ROOT / "facts/audio/manifest.toml"
PACKAGE = ROOT / "package"

# The layer headers whose configuration structs the binding has to cover, and the binding sections that cover
# them. A pointer to another struct counts as covered by that struct's own section.
COVERED = {
    "QcomAdsp.h": ("QcomAdspConfig", ("adsp.carveout", "adsp.entries", "adsp.values", "adsp.timeouts"), ()),
    "QcomAudioPath.h": ("QcomAudioPathConfig", ("path", "path.references"),
                        ("graph", "endpoint", "endpoint_format", "stream_format")),
    "CirrusCs35l41.h": ("Cs35l41Binding", ("amp.registers", "amp.b", "amp.t"), ()),
    "QcomAudioTopology.h": ("QcomAudioTopologyInputs",
                            ("path", "path.references", "path.topology", "path.graph", "path.endpoint", "path.format"),
                            ()),
}

problems: list[str] = []


def flatten(table: dict, prefix: str = "") -> list[tuple[str, dict]]:
    """Every leaf entry, as (dotted path, {value, state, evidence})."""
    entries = []
    for key, value in table.items():
        name = f"{prefix}{key}"
        if isinstance(value, dict):
            if {"value", "state", "evidence"} <= set(value):
                entries.append((name, value))
            else:
                entries.extend(flatten(value, name + "."))
    return entries


def manifest_value(manifest: dict, key: str):
    """Resolves "section.key" or "amplifier[N].key"; None when it is not there."""
    match = re.fullmatch(r"amplifier\[(\d+)\]\.(\w+)", key)
    if match:
        index, field = int(match.group(1)), match.group(2)
        amplifiers = manifest.get("amplifier", [])
        if index >= len(amplifiers):
            return None
        return amplifiers[index].get(field)
    section, _, field = key.partition(".")
    table = manifest.get(section)
    if not isinstance(table, dict):
        return None
    return table.get(field)


def struct_fields(header: str, name: str) -> list[str]:
    text = (PACKAGE / header).read_text()
    match = re.search(r"typedef struct \{(.*?)\} " + name + r";", text, re.S)
    if not match:
        problems.append(f"{header}: {name} not found")
        return []
    body = re.sub(r"/\*.*?\*/", "", match.group(1), flags=re.S)
    body = re.sub(r"//[^\n]*", "", body)
    fields = []
    for statement in body.split(";"):
        statement = " ".join(statement.split())
        if not statement:
            continue
        for part in statement.split(","):
            # Each piece ends in the declarator's name, with the type (and any pointer) in front of it.
            match = re.search(r"([A-Za-z_]\w*)\s*(\[[^\]]*\])?$", part.strip())
            if match:
                fields.append(match.group(1))
    return fields


def main() -> int:
    require_verified = "--require-verified" in sys.argv[1:]

    binding = tomllib.loads(BINDING.read_text())
    manifest = tomllib.loads(MANIFEST.read_text())
    entries = flatten(binding)
    if not entries:
        problems.append("the binding has no entries")

    verified = 0
    for path, entry in entries:
        state = entry.get("state")
        evidence = entry.get("evidence")
        if state not in ("unverified", "verified"):
            problems.append(f"{path}: state {state!r} is neither unverified nor verified")
            continue
        if state == "verified":
            verified += 1
            if evidence in (None, "", "none"):
                problems.append(f"{path}: verified but names no evidence")
                continue
            value = manifest_value(manifest, evidence)
            if value is None:
                problems.append(f"{path}: evidence {evidence!r} is not in the manifest")
                continue
            if value in ("unverified", False) or value is None:
                problems.append(f"{path}: verified on {evidence!r}, which is still unverified")
        else:
            if evidence not in (None, "", "none"):
                value = manifest_value(manifest, evidence)
                if value is None:
                    problems.append(f"{path}: evidence {evidence!r} is not in the manifest")

    # Every configuration field of every layer must appear in the binding.
    covered = 0
    for header, (struct, sections, references) in COVERED.items():
        present = set()
        for path, _ in entries:
            for section in sections:
                if path == section or path.startswith(section + "."):
                    present.add(path[len(section) + 1:] if path.startswith(section + ".") else path)
        for field in struct_fields(header, struct):
            if field in references:
                continue
            if field not in present:
                problems.append(f"{header}: {struct}.{field} is not in the binding")
            else:
                covered += 1

    if require_verified and verified != len(entries):
        problems.append(f"--require-verified: {len(entries) - verified} of {len(entries)} entries are unverified")

    for problem in problems:
        print(f"FAIL {problem}")
    if problems:
        print(f"{len(problems)} problem(s)")
        return 1
    print(f"PASS fuxi audio binding: {len(entries)} fields ({covered} layer fields covered), "
          f"{verified} verified, {len(entries) - verified} unverified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
