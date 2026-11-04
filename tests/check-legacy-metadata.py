#!/usr/bin/env python3
"""Verify the static ABI proof against supplied original bytes and fault controls."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--kernelcache', required=True, type=Path)
parser.add_argument('--kit', required=True, type=Path)
args = parser.parse_args()
sys.path[:0] = [str(args.kit.resolve()), str(Path(__file__).resolve().parents[1] / 'scripts')]
from native_prelink import macho, backed_offset, exports
from fuxi_legacy_metadata import verify_static_metadata

blob = args.kernelcache.read_bytes()
assert hashlib.sha256(blob).hexdigest() == 'f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7'
image = macho(blob)
names = {name: address for address, group in exports(blob, image).items() for name in group}


def read(address, count):
    # The consumer uses slide=-0x1000000. Instructions are unchanged; the
    # original input's GOT still points to original native exports above.
    at = backed_offset(image['segments'], address + 0x1000000, count, len(blob))
    return blob[at:at + count]


print(json.dumps(verify_static_metadata(read, names), indent=2))
for label, site in [('instance size', 0xfffffff0057866b4), ('class name', 0xfffffff0045fbc47),
                    ('parent address', 0xfffffff0057866b0), ('constructor stub', 0xfffffff0057a2820),
                    ('singleton wrapper', 0xfffffff005786578)]:
    def fault(address, count):
        data = bytearray(read(address, count))
        if address <= site < address + count:
            data[site - address] ^= 1
        return bytes(data)
    try:
        verify_static_metadata(fault, names)
    except ValueError as error:
        print('PASS refusal:', label, '-', error)
    else:
        raise AssertionError('Accepted corrupted original ' + label)
print('PASS: exact 21H461 static metadata and five independent corruption refusals')
