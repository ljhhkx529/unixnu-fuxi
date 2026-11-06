#!/usr/bin/env python3
"""Stage the prelinked kernel as a display-audit candidate in this device repository.

scripts/audit-mobile-graphics-legacy.py needs a prelink candidate to read the original
IOMobileGraphicsFamily, and it looks for one under the device repository first, because the tree is
rebuilt from scratch and build products do not survive. The candidate is the kernel step's own output
(emit_prelink writes the Mach-O plus a plan with the per-component executable hashes), so this script
only copies the pair and checks that the audit's two invariants hold before it does.

The pair is about 140 MB, so build/xnu-prelink/ is ignored: regenerate it whenever the kernelcache pin
or the root list of [kernel.prelink] changes.

  scripts/stage-prelink-candidate.py --tree ../UniXNU/out/fuxi/ipados-17.7.11-21H461/fuxi/tree
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys

# The audit compares the original executable of this family against exactly this digest, so a
# candidate built from a different IPSW is refused here rather than in the middle of an audit.
FAMILY = 'com.apple.iokit.IOMobileGraphicsFamily'
FAMILY_TEXT_SHA256 = '2e044ee68c1650397e1435234ca6459206262e4c7ed709cb7131c6f664f9bd9b'
NAME = 'native-owner-ufs-cpu-desktop-1-candidate'


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, required=True,
                        help='the assembled build tree that holds kernel/kernel.macho')
    parser.add_argument('--output', type=Path, default=root / 'build/xnu-prelink')
    args = parser.parse_args()

    kernel = args.tree / 'kernel/kernel.macho'
    plan = args.tree / 'kernel/kernel.json'
    for path in (kernel, plan):
        if not path.is_file():
            sys.exit(f'{path} missing: run "unixnu build kernel" first')
    report = json.loads(plan.read_text())
    if FAMILY not in report['components']:
        sys.exit(f'{FAMILY} is not a component of this prelink; check [kernel.prelink] roots')
    if report['original_executable_sha256'].get(FAMILY) != FAMILY_TEXT_SHA256:
        sys.exit('the original ' + FAMILY + ' of this kernelcache is not the one the audit qualifies')
    if sha256(kernel) != report['candidate_sha256']:
        sys.exit('kernel.macho does not match its own plan; rebuild the kernel step')

    args.output.mkdir(parents=True, exist_ok=True)
    target = args.output / (NAME + '.macho')
    shutil.copy2(kernel, target)
    shutil.copy2(plan, args.output / (NAME + '.json'))
    print(f'staged {target.relative_to(root)} ({target.stat().st_size} bytes)')
    print(f'candidate sha256 {report["candidate_sha256"]}')
    print(f'original {FAMILY} {report["original_executable_sha256"][FAMILY]}')


if __name__ == '__main__':
    main()
