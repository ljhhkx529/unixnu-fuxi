#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build build/virtio-mmio-block-verified-bundle/libVirtioMmioBlock.a for fuxi.

The sheng path builds this via build-virtio-mmio-bindings.py, which reads the
offline binary audit build/storage-abi/interface-audit.json (GDB live capture of
the original 21H461 kernelcache). fuxi has no such capture; instead it links the
pinned IOBlockStorageDevice addresses already transcribed in
scripts/data/native-storage-original-bindings.S, exactly as unixnu-fuxi-old did.

Only VirtioMmioBlock carries the absolute-symbol object; NativeUfs and RamBlock
resolve those imports from the force-loaded Virtio archive, avoiding duplicate
absolute symbols."""
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/virtio-mmio-block-verified-bundle'


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    obj = ROOT / 'build/virtio-mmio-block/libVirtioMmioBlock.a'
    if not obj.is_file():
        # component:VirtioMmioBlock builds the plain archive; it must run first.
        sys.exit(f'{obj} missing: keep component:VirtioMmioBlock before this script in [kernel].archives')

    source = ROOT / 'scripts/data/native-storage-original-bindings.S'
    text = source.read_text()
    bindings = {}
    for line in text.splitlines():
        match = re.match(r'\.set\s+(\S+),\s*(0x[0-9a-fA-F]+)', line)
        if match:
            bindings[match.group(1)] = match.group(2)
    if not bindings:
        sys.exit('Pinned original storage bindings are empty')

    bindings_object = OUT / 'storage-original-bindings.o'
    subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0',
                    '-Werror', '-Wno-incompatible-sysroot', '-c', source,
                    '-o', bindings_object], check=True)

    # Copy the plain archive and append the absolute-symbol object.
    archive = OUT / 'libVirtioMmioBlock.a'
    archive.unlink(missing_ok=True)
    shutil.copyfile(obj, archive)
    subprocess.run(['xcrun', 'ar', 'q', str(archive), str(bindings_object)], check=True)

    (OUT / 'bindings.json').write_text(json.dumps(dict(
        compiled_object_sha256=hashlib.sha256(obj.read_bytes()).hexdigest(),
        bindings=bindings, original_code_modified=False,
        live_capture=False), indent=2) + chr(10))
    print(f'fuxi virtio verified-bundle archive: {archive.relative_to(ROOT)} ({len(bindings)} storage bindings)')


if __name__ == '__main__':
    main()

