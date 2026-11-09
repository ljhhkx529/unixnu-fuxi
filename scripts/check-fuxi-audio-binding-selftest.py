#!/usr/bin/env python3
"""The binding gate's own test: a gate that is never tested is a gate that can quietly stop gating.

Each case below writes a one-entry binding and asks the gate about it, so the three rules that matter are exercised the
way a mistake would exercise them: a choice without a reason, a reason that points nowhere, and a fact claimed as verified
while its source says it was never measured.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GATE = ROOT / "scripts/check-fuxi-audio-binding.py"

CASES = [
    ("a choice with an existing anchor passes",
     '[section]\nfield = { value = 1, state = "chosen", evidence = "design:docs/fuxi-audio-abi-choices.md#abi-rate" }\n',
     True, ""),
    ("a choice with no reason is refused",
     '[section]\nfield = { value = 1, state = "chosen", evidence = "none" }\n',
     False, "names no design anchor"),
    ("a choice pointing at a missing document is refused",
     '[section]\nfield = { value = 1, state = "chosen", evidence = "design:docs/not-here.md#x" }\n',
     False, "does not exist"),
    ("a choice whose anchor is not in the document is refused",
     '[section]\nfield = { value = 1, state = "chosen", evidence = "design:docs/fuxi-audio-abi-choices.md#no-such-anchor" }\n',
     False, "is not in"),
    ("a choice with no anchor at all is refused",
     '[section]\nfield = { value = 1, state = "chosen", evidence = "design:docs/fuxi-audio-abi-choices.md" }\n',
     False, "names no section"),
    ("a fact claimed verified on an unmeasured source is refused",
     '[section]\nfield = { value = 1, state = "verified", evidence = "amplifier[0].reset_gpio" }\n',
     False, "not measured"),
]

failures = 0
for name, body, should_pass, expect in CASES:
    with tempfile.TemporaryDirectory() as temporary:
        binding = Path(temporary) / "binding.toml"
        binding.write_text(body)
        result = subprocess.run(
            [sys.executable, str(GATE), "--binding", str(binding), "--skip-coverage"],
            capture_output=True, text=True)
        passed = result.returncode == 0
        text = result.stdout + result.stderr
        if passed != should_pass or (expect and expect not in text):
            failures += 1
            print(f"FAIL {name}")
            print(f"     exit={result.returncode} expected_pass={should_pass}")
            print("     " + text.strip().replace("\n", "\n     ")[:400])
        else:
            print(f"PASS {name}")

print(f"PASS QcomAudioBindingGate: {len(CASES)} cases" if not failures else f"{failures} case(s) failed")
sys.exit(0 if not failures else 1)
