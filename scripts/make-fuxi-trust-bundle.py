#!/usr/bin/env python3
"""The boot trust bundle with the cellular bridge's CommCenter added to the owner module.

The same shape as UniXNU's scripts/make-owner-trust-bundle.py: a version-3 header carrying the offset of
each module, then the owner module (version 2, hashes in strictly ascending order, which is what DarwinBoot's
and the kernel's binary search need) followed by both original Apple modules byte for byte. The only
difference is one more cdhash: the compatibility copy of CommCenter that the GPU addon carries, so AMFI lets
it run when the bridge mounts it over the sealed one.

usage: make-fuxi-trust-bundle.py OUT --cdhash HEX [--cdhash HEX ...]
"""
import argparse
import hashlib
from pathlib import Path
import struct
import uuid

ROOT = Path('out/fuxi/ipados-17.7.11-21H461/fuxi/tree')  # the prepared tree, relative to the UniXNU checkout
ORIGINAL_SHA256 = '7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--cdhash', action='append', default=[])
    parser.add_argument('--tree', type=Path, default=None, help='the prepared tree (default: the current build tree)')
    args = parser.parse_args()
    global ROOT
    if args.tree:
        ROOT = args.tree

    original = (ROOT / 'build/original-static-system-trust/original-static-system.bundle').read_bytes()
    if hashlib.sha256(original).hexdigest() != ORIGINAL_SHA256:
        raise SystemExit('The original static/System trust bundle differs from the unchanged target input')
    count = struct.unpack_from('<I', original)[0]
    if count != 2:
        raise SystemExit('the original bundle is not the two modules this port expects')
    offsets = struct.unpack_from('<2I', original, 4)
    originals = [original[offsets[0]:offsets[1]], original[offsets[1]:]]

    trust = (ROOT / 'build/adreno-metal-plugin/trust.json').read_text()
    import json
    trust = json.loads(trust)
    hashes = {bytes.fromhex(trust['cdhash']), bytes.fromhex(trust['daemon_cdhash']),
              bytes.fromhex(trust['turnip_cdhash'])}
    cryptex = ROOT / 'build/app-cryptex/app-trustcache.bin'
    if cryptex.is_file():
        module = cryptex.read_bytes()
        entries = struct.unpack_from('<I', module, 20)[0]
        if struct.unpack_from('<I', module)[0] != 2 or len(module) != 24 + entries * 24:
            raise SystemExit('The App cryptex trust module is not a complete version-2 module')
        hashes |= {module[24 + 24 * e:24 + 24 * e + 20] for e in range(entries)}
    for text in args.cdhash:
        digest = bytes.fromhex(text)
        if len(digest) != 20:
            raise SystemExit(f'not a cdhash: {text}')
        hashes.add(digest)

    ordered = sorted(hashes)
    identifier = uuid.uuid5(uuid.NAMESPACE_URL, 'org.blu-sharky.adreno-metal:' + ''.join(h.hex() for h in ordered))
    own = struct.pack('<I16sI', 2, identifier.bytes, len(ordered)) + b''.join(
        h + bytes([2, 0, 0, 0]) for h in ordered)
    modules = [own, *originals]
    cursor, starts = 16, []
    for module in modules:
        starts.append(cursor)
        cursor += len(module)
    bundle = struct.pack('<4I', 3, *starts) + b''.join(modules)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(bundle)
    print('wrote %s (%d bytes, %d hashes)' % (args.output, len(bundle), len(ordered)))


if __name__ == '__main__':
    main()
