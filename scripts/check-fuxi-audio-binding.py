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
    "QcomAudioPath.h": ("QcomAudioPathConfig", ("path", "path.references", "path.graph", "path.topology"),
                        ("graph", "endpoint", "endpoint_format", "stream_format")),
    "CirrusCs35l41.h": ("Cs35l41Binding", ("amp.registers", "amp.b", "amp.t"), ()),
    "QcomAudioRing.h": ("QcomAudioRingConfig", ("path.ring", "path"), ()),
    "QcomAudioAbi.h": ("QcomAudioAbiConfig", ("abi",), ()),
    "QcomAudioCapture.h": ("QcomAudioCaptureConfig", ("capture",), ()),
    "QcomAudioGraph.h": ("QcomArGraphSpec", ("path.graph",), ()),
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


def manifest_measured(manifest: dict, key: str) -> bool:
    """Whether the fact behind a key was measured rather than read off a source."""
    match = re.fullmatch(r"amplifier\[(\d+)\]\.(\w+)", key)
    if match:
        amplifiers = manifest.get("amplifier", [])
        index = int(match.group(1))
        if index < len(amplifiers):
            return bool(amplifiers[index].get("runtime_verified", False))
        return False
    section, _, _field = key.partition(".")
    table = manifest.get(section)
    if isinstance(table, dict):
        return bool(table.get("runtime_verified", False))
    return False
    section, _, field = key.partition(".")
    table = manifest.get(section)
    if not isinstance(table, dict):
        return None
    return table.get(field)


def struct_fields(header: str, name: str) -> list[str]:
    text = (PACKAGE / header).read_text()
    # A file may hold several typedefs; take the one whose name matches, not the span up to the first match.
    found = [m for m in re.finditer(r"typedef struct \{(.*?)\} (\w+);", text, re.S) if m.group(2) == name]
    if not found:
        problems.append(f"{header}: {name} not found")
        return []
    body = re.sub(r"/\*.*?\*/", "", found[0].group(1), flags=re.S)
    body = re.sub(r"//[^\n]*", "", body)
    fields = []
    for statement in body.split(";"):
        statement = " ".join(statement.split())
        if not statement:
            continue
        for part in statement.split(","):
            # A pointer is runtime wiring (a shared buffer, an index), not a board fact, so it is not a binding
            # field; every other declarator is.
            if "*" in part:
                continue
            # Each piece ends in the declarator's name, with the type in front of it.
            match = re.search(r"([A-Za-z_]\w*)\s*(\[[^\]]*\])?$", part.strip())
            if match:
                fields.append(match.group(1))
    return fields


def main() -> int:
    arguments = sys.argv[1:]
    require_verified = "--require-verified" in arguments
    skip_coverage = "--skip-coverage" in arguments
    binding_path = BINDING
    if "--binding" in arguments:
        # The gate's own test needs to point it at a one-entry binding without a second copy of the tree.
        binding_path = Path(arguments[arguments.index("--binding") + 1]).resolve()

    binding = tomllib.loads(binding_path.read_text())
    manifest = tomllib.loads(MANIFEST.read_text())
    entries = flatten(binding)
    if not entries:
        problems.append("the binding has no entries")

    verified = 0
    chosen = 0
    for path, entry in entries:
        state = entry.get("state")
        evidence = entry.get("evidence")
        if state not in ("unverified", "verified", "chosen"):
            problems.append(f"{path}: state {state!r} is neither unverified, verified nor chosen")
            continue
        if state == "chosen":
            chosen += 1
            # A choice has no source; it has a reason, and the reason has to exist somewhere a person can read it.
            if not evidence or not evidence.startswith("design:"):
                problems.append(f"{path}: chosen but names no design anchor")
                continue
            target = evidence[len("design:"):]
            document_name, _, anchor = target.partition("#")
            document = ROOT / document_name
            if not document.is_file():
                problems.append(f"{path}: design anchor {document_name} does not exist")
            elif not anchor:
                problems.append(f"{path}: design anchor names no section")
            elif anchor not in document.read_text():
                problems.append(f"{path}: design anchor {anchor!r} is not in {document_name}")
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
            elif not manifest_measured(manifest, evidence):
                problems.append(f"{path}: verified on {evidence!r}, which was read from a source, not measured")
        else:
            if evidence not in (None, "", "none"):
                value = manifest_value(manifest, evidence)
                if value is None:
                    problems.append(f"{path}: evidence {evidence!r} is not in the manifest")
                elif value not in ("unverified", False) and value is not None and value != entry.get("value"):
                    problems.append(f"{path}: carries {entry.get('value')!r} but its evidence {evidence!r} says {value!r}")

    # Every configuration field of every layer must appear in the binding.
    covered = 0
    for header, (struct, sections, references) in ({} if skip_coverage else COVERED).items():
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

    if require_verified and verified + chosen != len(entries):
        problems.append(f"--require-verified: {len(entries) - verified - chosen} of {len(entries)} entries are neither verified nor a recorded choice")

    for problem in problems:
        print(f"FAIL {problem}")
    if problems:
        print(f"{len(problems)} problem(s)")
        return 1
    def is_fact(evidence):
        return (evidence not in (None, "", "none") and evidence != "sheng.sm8550"
                and not str(evidence).startswith("design:"))
    cited = sum(1 for _p, e in entries if is_fact(e.get("evidence")))
    predicted = sum(1 for _p, e in entries if e.get("evidence") == "sheng.sm8550")
    unsourced = len(entries) - cited - predicted - chosen
    print(f"PASS fuxi audio binding: {len(entries)} fields ({covered} layer fields covered), "
          f"{verified} verified, {chosen} chosen, {len(entries) - verified - chosen} unverified")
    print(f"     {cited} cite a recorded fact, {predicted} are predictions from sheng, "
          f"{chosen} are recorded choices, {unsourced} have no source yet")
    return 0


if __name__ == "__main__":
    sys.exit(main())
