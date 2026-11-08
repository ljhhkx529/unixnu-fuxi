#!/usr/bin/env python3
"""Cross-check the predictions the binding takes from sheng against sheng's own source.

The binding carries SM8550 platform values that this board has not measured yet: the ADSP carveout, the SMP2P entry
names and their bits, the load-state request. They are predictions, and a prediction typed by hand is a prediction that
can be mis-typed - so they are read back out of sheng's AdspProbe.cpp and compared.

  check-fuxi-audio-sheng-refs.py [SHENG_CHECKOUT]

SHENG_CHECKOUT defaults to $UNIXNU_SHENG, then to ../UniXNU-sheng. Without it the check reports that it skipped, because
a tree without the reference cannot answer the question.
"""

from __future__ import annotations

import os
import re
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINDING = ROOT / "facts/audio/binding.toml"


def find_reference() -> Path | None:
    for candidate in (sys.argv[1] if len(sys.argv) > 1 else None, os.environ.get("UNIXNU_SHENG"),
                      str(ROOT.parent / "UniXNU-sheng")):
        if candidate and (Path(candidate) / "package/AdspProbe.cpp").is_file():
            return Path(candidate)
    return None


def constants(text: str) -> dict[str, int]:
    """The constexpr block, as name -> integer."""
    values = {}
    for line in text.splitlines():
        if "constexpr" not in line:
            continue
        # A constant may be a plain number or a shift, as kReady = 1u << 1 is.
        for name, shifted, base, raw in re.findall(
                r"(k[A-Za-z0-9]+)\s*=\s*(?:(\d+)\s*u?\s*<<\s*(\d+)|(0x[0-9a-fA-F]+|\d+))", line):
            values[name] = (int(shifted) << int(base)) if shifted else int(raw, 0)
    return values


def main() -> int:
    reference = find_reference()
    if not reference:
        print("SKIP fuxi audio sheng references: no sheng checkout found (pass it as an argument)")
        return 0

    source = (reference / "package/AdspProbe.cpp").read_text()
    known = constants(source)
    binding = tomllib.loads(BINDING.read_text())
    adsp = binding["adsp"]

    problems = []
    expected = {
        ("carveout", "carveout_base"): ("kAdspBase", "the ADSP carveout base"),
        ("carveout", "carveout_size"): ("kAdspSize", "the ADSP carveout size"),
        ("values", "ready_value"): ("kReady", "the ready bit"),
        ("values", "fatal_value"): ("kFatal", "the fatal bit"),
    }
    for (section, field), (constant, what) in expected.items():
        want = known.get(constant)
        got = adsp[section][field]["value"]
        if want is None:
            problems.append(f"{constant} is not in sheng's source any more")
        elif want != got:
            problems.append(f"{what}: binding {hex(got)} but sheng has {constant} {hex(want)}")

    names = {
        "state_entry": ("master-kernel", "the outbound entry"),
        "ready_entry": ("slave-kernel", "the inbound entry"),
        "fatal_entry": ("slave-kernel", "the entry the fatal bit lives in"),
    }
    for field, (name, what) in names.items():
        got = adsp["entries"][field]["value"]
        if got != name:
            problems.append(f"{what}: binding says {got!r}")
        if f'"{name}"' not in source:
            problems.append(f"sheng's source no longer names {name!r}")

    message = adsp["entries"]["load_state_message"]["value"]
    if "load_state" not in source or message not in source:
        problems.append("the load-state request the binding carries is not the one sheng sends")

    for problem in problems:
        print(f"FAIL {problem}")
    if problems:
        print(f"{len(problems)} problem(s)")
        return 1
    print(f"PASS fuxi audio sheng references: {len(expected)} values and {len(names) + 1} strings agree with "
          f"{reference.name}/package/AdspProbe.cpp")
    return 0


if __name__ == "__main__":
    sys.exit(main())
