#!/usr/bin/env python3
"""Add a wallpaper overlay to an already packed GPU addon, keeping every other file's content and mode.

  add-fuxi-wallpaper-to-addon.py ADDON_IN OVERLAY_DIR ADDON_OUT

The canonical path is the core scripts/make-gpu-addon.py --wallpaper, which packs the plugin bundle, the daemons, the client list and the
overlay together from source; it needs the tree's build/adreno-metal-plugin directory. This script covers the case where that directory is
gone but the bytes it once produced are on hand: an image under test then keeps exactly the plugin it was booted with and gains only the
wallpaper files, which is the single variable the boot is meant to show. The layout and the path guards are make-gpu-addon.py's NGPUADD2.
"""
import struct
import sys
import zlib
from pathlib import Path


def read_addon(blob):
    if blob[:8] != b'NGPUADD2':
        sys.exit('not an NGPUADD2 addon')
    count, _zero, total = struct.unpack_from('<IIQ', blob, 8)
    if total != len(blob):
        sys.exit(f'addon header says {total} bytes, the file is {len(blob)}')
    files, at = [], 24
    for _ in range(count):
        name_bytes, mode, size, packed = struct.unpack_from('<IIQQ', blob, at)
        at += 24
        name = blob[at:at + name_bytes - 1].decode()
        at += (name_bytes + 7) // 8 * 8
        data = zlib.decompress(blob[at:at + packed])
        at += (packed + 7) // 8 * 8
        if len(data) != size:
            sys.exit(f'{name}: header says {size} bytes, the stream holds {len(data)}')
        files.append((name, mode, data))
    if at != len(blob):
        sys.exit(f'{len(blob) - at} bytes of trailing data after {count} files')
    return files


def write_addon(files):
    body = b''
    for name, mode, data in files:
        encoded = name.encode() + b'\0'
        if len(encoded) > 160 or '..' in name.split('/') or name.startswith('/'):
            sys.exit('Bad addon path ' + name)
        packed = zlib.compress(data, 9)
        body += struct.pack('<IIQQ', len(encoded), mode, len(data), len(packed))
        body += encoded + bytes(-len(encoded) % 8) + packed + bytes(-len(packed) % 8)
    return b'NGPUADD2' + struct.pack('<IIQ', len(files), 0, 24 + len(body)) + body


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    source, overlay, target = (Path(arg) for arg in sys.argv[1:])
    if not source.is_file():
        sys.exit(f'no addon at {source}')
    if not overlay.is_dir():
        sys.exit(f'{overlay} is not the wallpaper overlay directory make-fuxi-wallpaper-overlay.py writes')
    if target.exists():
        sys.exit('Refuse to overwrite ' + str(target))
    files = read_addon(source.read_bytes())
    if any(name.startswith('wallpaper/') for name, _, _ in files):
        sys.exit(f'{source} already carries a wallpaper overlay')
    images = sorted(p for p in overlay.rglob('*') if p.is_file())
    if not images:
        sys.exit(f'{overlay} holds no files')
    for path in images:
        files.append(('wallpaper/' + path.relative_to(overlay).as_posix(), 0o644, path.read_bytes()))
    blob = write_addon(files)
    target.write_bytes(blob)
    if read_addon(blob) != files:
        target.unlink()
        sys.exit('the packed addon does not read back as written')
    added = sum(len(data) for _, _, data in files[len(files) - len(images):])
    print(f'{len(files) - len(images)} files kept, {len(images)} wallpaper files added ({added} bytes)')
    for name, mode, data in files[len(files) - len(images):]:
        print(f'{mode:o} {len(data):>10} {name}')
    print(f'{target}: {len(files)} files, {len(blob)} bytes (was {source.stat().st_size})')


if __name__ == '__main__':
    main()
