#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build the owner trust bundle for the board: the Metal plugin's hashes, the audio server
plug-in's hash, and the two unchanged original Apple trust modules.

  make-fuxi-owner-trust.py --tree DIR [--metal DIR] [--audio DIR] [--output FILE]

The owner module is the leading version-2 module of the bundle: the boot loader
(drivers/DarwinBoot/DarwinBootPkg/Driver/DarwinBoot.c, ValidTrustCaches) and the kernel's
binary search both need the hashes of a module in strictly ascending order, so all the
owner's hashes share one module.

The two original modules (the target's static/System trust caches) are copied byte for byte
and pinned by their SHA-256, so nothing of Apple's own trust chain changes: the bundle only
adds the owner's own code to it.

This is UniXNU's scripts/make-owner-trust-bundle.py extended with the audio server plug-in's
CDHash (UniXNU-sheng trusts its ShengAudio.driver the same way).
"""
import argparse
import hashlib
import json
import os
import struct
import sys
import uuid
from pathlib import Path

ORIGINAL_SHA256 = '7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7'


def modules_of(data):
    count = struct.unpack_from('<I', data)[0]
    starts = list(struct.unpack_from(f'<{count}I', data, 4))
    return [data[starts[index]:starts[index + 1] if index + 1 < count else len(data)]
            for index in range(count)]


def valid_like_the_loader(data):
    """The same structural checks as drivers/DarwinBoot/DarwinBootPkg/Driver/DarwinBoot.c
    ValidTrustCaches: the loader refuses the whole bundle if one of them fails."""
    count = struct.unpack_from('<I', data)[0]
    if not 0 < count <= 128:
        return False
    for index in range(count):
        start = struct.unpack_from('<I', data, 4 + 4 * index)[0]
        end = struct.unpack_from('<I', data, 8 + 4 * index)[0] if index + 1 < count else len(data)
        if (start < 4 + 4 * count or start & 3 or end > len(data) or end - start < 24 or
                struct.unpack_from('<I', data, start)[0] != 2):
            return False
        entries = struct.unpack_from('<I', data, start + 20)[0]
        if end - start != 24 + entries * 24:
            return False
        for entry in range(entries):
            at = start + 24 + entry * 24
            if not 1 <= data[at + 20] <= 4 or (entry and data[at - 24:at - 4] >= data[at:at + 20]):
                return False
    return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--tree', type=Path,
                        default=Path(os.environ.get('UNIXNU_TREE', ROOT / '.cache/unixnu')),
                        help='the UniXNU tree that holds build/original-static-system-trust')
    parser.add_argument('--metal', type=Path, default=None,
                        help='the Metal plugin build directory (build/adreno-metal-plugin)')
    parser.add_argument('--audio', type=Path, default=None,
                        help='the audio plug-in build directory (build/fuxi-audio-hal)')
    parser.add_argument('--output', type=Path, default=None)
    args = parser.parse_args()

    metal = args.metal or (args.tree / 'build/adreno-metal-plugin')
    audio = args.audio or (args.tree / 'build/fuxi-audio-hal')
    output = args.output or (args.tree / 'build/fuxi-audio-owner-trust/owner-trust.bundle')

    original_path = args.tree / 'build/original-static-system-trust/original-static-system.bundle'
    if not original_path.is_file():
        sys.exit(f'{original_path} missing: the tree has no original trust input')
    original = original_path.read_bytes()
    if hashlib.sha256(original).hexdigest() != ORIGINAL_SHA256:
        sys.exit('The original static/System trust bundle differs from the unchanged target input')
    originals = modules_of(original)
    if len(originals) != 2:
        sys.exit(f'The original trust bundle carries {len(originals)} modules, not the two of the target')

    hashes = []
    metal_trust = metal / 'trust.json'
    if metal_trust.is_file():
        trust = json.loads(metal_trust.read_text())
        for key in ('cdhash', 'daemon_cdhash', 'turnip_cdhash'):
            if trust.get(key):
                hashes.append(bytes.fromhex(trust[key]))
    audio_trust = audio / 'trust.json'
    if not audio_trust.is_file():
        sys.exit(f'{audio_trust} missing: build the plug-in with scripts/build-fuxi-audio-hal.py')
    audio_hash = bytes.fromhex(json.loads(audio_trust.read_text())['cdhash'])
    hashes.append(audio_hash)
    if len(set(hashes)) != len(hashes):
        sys.exit('Duplicate CDHash')
    hashes.sort()

    identifier = uuid.uuid5(uuid.NAMESPACE_URL, 'org.unixnu.fuxi.owner:' + ''.join(h.hex() for h in hashes))
    owner = struct.pack('<I16sI', 2, identifier.bytes, len(hashes)) + \
        b''.join(h + bytes([2, 0, 0, 0]) for h in hashes)
    modules = [owner, *originals]
    cursor, starts = 16, []
    for module in modules:
        starts.append(cursor)
        cursor += len(module)
    bundle = struct.pack('<4I', 3, *starts) + b''.join(modules)
    if not valid_like_the_loader(bundle):
        sys.exit('The trust bundle would be refused by the boot loader')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(bundle)
    (output.parent / 'owner-trust.json').write_text(json.dumps(dict(
        bundle_sha256=hashlib.sha256(bundle).hexdigest(),
        own_cdhashes=[h.hex() for h in hashes],
        audio_cdhash=audio_hash.hex(),
        original_modules_unchanged=True,
        purpose='owner trust for the Adreno Metal plugin and the audio server plug-in'),
        indent=2) + '\n')
    print(f'PASS: owner trust bundle ({len(hashes)} owner hashes + both original modules) '
          f'{hashlib.sha256(bundle).hexdigest()} -> {output}')


ROOT = Path(__file__).resolve().parents[1]


if __name__ == '__main__':
    main()
