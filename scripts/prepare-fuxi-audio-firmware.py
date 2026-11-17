#!/usr/bin/env python3
"""Inventory a fuxi-firmware checkout for the native audio runtime.

This never copies proprietary bytes into Git. It verifies the pinned amplifier
assets in place and reports separately whether the two PAS images needed by the
ADSP owner glue are present. --require-adsp turns the partial inventory into a
hard gate for a runtime image build.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PINNED_REVISION = "8a97136d96df7f7baadcb83dcf9e30495ea61a17"
ASSETS = {
    "odm/firmware/B-cs35l41-dsp1-spk-cali.bin": "5b832c76dd4ce9e497e36e00cb21922b00267cfca7ddb043a79ff112a74b81ab",
    "odm/firmware/B-cs35l41-dsp1-spk-prot.bin": "67a3023c8c616f828961b0a649d39c7923d36029ffdf5e98dcc85c891189ded7",
    "odm/firmware/T-cs35l41-dsp1-spk-cali.bin": "7ed4abcfd84e30bb80ffa258452dabc7c605f592b3743ecd5253106b7a8f8b9d",
    "odm/firmware/T-cs35l41-dsp1-spk-prot.bin": "1a19f41d89428d19eae6ab44ce79dc0a9cdd73d276344d38a9f2503ced27b576",
    "odm/firmware/cs35l41-dsp1-spk-cali.wmfw": "c11cb3e25825c673a955a3519370ad77fbcd8bcf68808e8e34140a70caefcec4",
    "odm/firmware/cs35l41-dsp1-spk-prot.wmfw": "c11cb3e25825c673a955a3519370ad77fbcd8bcf68808e8e34140a70caefcec4",
    "odm/firmware/B-handfree.txt": "38c334efb8227484e230cb13ee3a52cbf26c7c1adc5b5b8c9b030e18973aa240",
    "odm/firmware/B-music.txt": "708b54d9b7069973c88d58eb771c82a46e2477bb3561d82f491f7ad5697ef99d",
    "odm/firmware/T-handfree.txt": "d5933b00c250e62a095b8f48b5aceda838b19d378aa30b20bf820ccc792f1b18",
    "odm/firmware/T-handset.txt": "126f487f5650d7f328667a7fef775fee5fccda7074ef3f079b223817797811c7",
    "odm/firmware/T-music.txt": "bb4c7c68afd71f1686615fd98743cbbf61561d03c5f9a8b92cd7d867f2f1cbc0",
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_revision(source: Path) -> str | None:
    result = subprocess.run(["git", "-C", source, "rev-parse", "HEAD"], capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else None


def tree_names(source: Path) -> set[str]:
    result = subprocess.run(["git", "-C", source, "ls-tree", "-r", "--name-only", "HEAD"],
                            capture_output=True, text=True)
    if result.returncode == 0:
        return set(result.stdout.splitlines())
    return {str(path.relative_to(source)) for path in source.rglob("*") if path.is_file()}


def image_pieces(names: set[str], base: str) -> list[str]:
    candidates = []
    for name in sorted(names):
        leaf = Path(name).name.lower()
        if leaf in (f"{base}.mbn", f"{base}.mdt", f"{base}.elf") or (
                leaf.startswith(f"{base}.b") and len(leaf.rsplit(".b", 1)[-1]) == 2):
            candidates.append(name)
    return candidates


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path, nargs="?", default=ROOT / ".cache/fuxi-firmware")
    parser.add_argument("--report", type=Path, default=ROOT / "build/fuxi-audio-firmware/report.json")
    parser.add_argument("--require-adsp", action="store_true")
    args = parser.parse_args()
    source = args.source.resolve()
    revision = git_revision(source)
    names = tree_names(source) if source.is_dir() else set()

    assets = []
    bad = []
    for relative, expected in ASSETS.items():
        path = source / relative
        actual = sha256(path) if path.is_file() else None
        ok = actual == expected
        assets.append({"path": relative, "sha256": actual, "expected_sha256": expected, "ok": ok})
        if not ok:
            bad.append(relative)

    adsp = image_pieces(names, "adsp")
    adsp_dtb = image_pieces(names, "adsp_dtb")
    complete = bool(adsp and adsp_dtb)
    report = {
        "schema_version": 1,
        "repository": "https://github.com/ljhhkx529/fuxi-firmware",
        "expected_revision": PINNED_REVISION,
        "revision": revision,
        "revision_matches": revision == PINNED_REVISION,
        "amplifier_assets": assets,
        "amplifier_ready": not bad,
        "adsp_pieces": adsp,
        "adsp_dtb_pieces": adsp_dtb,
        "adsp_pas_ready": complete,
        "runtime_ready": not bad and complete,
        "missing": (["adsp PAS image"] if not adsp else []) + (["adsp_dtb PAS image"] if not adsp_dtb else []),
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")

    if revision != PINNED_REVISION:
        print(f"FAIL firmware revision: expected {PINNED_REVISION}, got {revision or 'not a Git checkout'}")
        return 1
    if bad:
        print("FAIL amplifier firmware: " + ", ".join(bad))
        return 1
    print(f"PASS Fuxi amplifier firmware: {len(ASSETS)} pinned files at {revision[:12]}")
    if complete:
        print(f"PASS Fuxi ADSP PAS inputs: {len(adsp)} adsp files, {len(adsp_dtb)} adsp_dtb files")
        return 0
    print("PARTIAL Fuxi ADSP PAS inputs: " + ", ".join(report["missing"]))
    print(f"report: {args.report}")
    return 1 if args.require_adsp else 0


if __name__ == "__main__":
    raise SystemExit(main())
