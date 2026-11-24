#!/usr/bin/env python3
"""Add one job to a stock launchd boot manifest without rewriting the file.

The manifest is the binary plist launchd itself reads after its first exec; its loader keeps a native
cache keyed by the manifest's object graph, so a re-serialised copy (plistlib round trip) is silently
ignored -- every entry, new and original, stops being honoured. The job is therefore appended at the
binary-plist level: the original object bytes stay untouched and only a new key string, a new
dictionary and the root's reference array are added.

Adapted from Trogdor-ipadOS tools/coachz_apps.py append_launchd_job (MIT).

usage: make-fuxi-manifest.py STOCK_MANIFEST OUT_MANIFEST
"""
import plistlib
import struct
import subprocess
import sys
from pathlib import Path

JOB_PATH = '/System/Library/LaunchDaemons/org.unixnu.fuxi.logdump.plist'
JOB = {
    'Label': 'org.unixnu.fuxi.logdump',
    'ProgramArguments': ['/System/Library/Frameworks/CoreTelephony.framework/Support/fuxi-logdump'],
    'RunAtLoad': True,
    'KeepAlive': False,
    'UserName': 'root',
    'ProcessType': 'Background',
    # The spelling the stock manifest itself uses on com.apple.replayd: a job property, not an entitlement.
    'com.apple.private.security.no-sandbox': True,
    # The console is not a line this port carries for a user process; the daemon writes a file the kernel reads.
    'StandardOutPath': '/private/var/log/fuxi-logdump.log',
    'StandardErrorPath': '/private/var/log/fuxi-logdump.log',
}


ABM_PATH = '/System/Library/LaunchDaemons/org.unixnu.fuxi.abm.plist'
ABM = {
    'Label': 'org.unixnu.fuxi.abm',
    'Program': '/System/Library/PrivateFrameworks/ABMHelper.framework/Support/abm-helper',
    'RunAtLoad': True,
    'KeepAlive': False,
    'UserName': 'root',
    'ProcessType': 'Standard',
    'com.apple.private.security.no-sandbox': True,
    'StandardOutPath': '/private/var/db/abm.log',
    'StandardErrorPath': '/private/var/db/abm.log',
}


ABMLITE_PATH = '/System/Library/LaunchDaemons/org.unixnu.fuxi.abmlite.plist'
ABMLITE = {
    'Label': 'org.unixnu.fuxi.abmlite',
    # The ABM command tool: it answers only when CommCenter runs the cellular branch and has started its
    # abmtool server, so whichever line it writes tells us which branch CommCenter took.
    'ProgramArguments': ['/usr/bin/abmlite', 'status'],
    'RunAtLoad': True,
    'KeepAlive': False,
    'UserName': 'root',
    'ProcessType': 'Standard',
    'StandardOutPath': '/private/var/db/abmlite.log',
    'StandardErrorPath': '/private/var/db/abmlite.log',
}


def append_launchd_job(stock, path, job):
    """Keep the native cache's original objects and reference identities intact."""
    assert stock.startswith(b'bplist00') and len(stock) >= 40
    _, offset_size, reference_size, count, root, table = struct.unpack('>6sBBQQQ', stock[-32:])
    assert offset_size in (1, 2, 4, 8) and reference_size in (1, 2, 4, 8)
    assert count > root and 8 <= table and table + count * offset_size == len(stock) - 32
    offsets = [int.from_bytes(stock[table + i * offset_size:table + (i + 1) * offset_size], 'big')
               for i in range(count)]
    assert all(8 <= offset < table for offset in offsets)
    body = bytearray(stock[:table])

    def read_count(offset, kind):
        value = stock[offset]
        assert value >> 4 == kind
        length = value & 15
        offset += 1
        if length == 15:
            assert stock[offset] >> 4 == 1 and stock[offset] & 15 <= 3
            size = 1 << (stock[offset] & 15)
            offset += 1
            length = int.from_bytes(stock[offset:offset + size], 'big')
            offset += size
        assert offset <= table
        return length, offset

    def dictionary_refs(index):
        length, offset = read_count(offsets[index], 13)
        assert offset + 2 * length * reference_size <= table
        refs = [int.from_bytes(stock[offset + i * reference_size:offset + (i + 1) * reference_size], 'big')
                for i in range(2 * length)]
        assert all(ref < count for ref in refs)
        return refs[:length], refs[length:]

    keys, values = dictionary_refs(root)
    jobs_index = None
    for key, value in zip(keys, values):
        length, offset = read_count(offsets[key], 5)
        if stock[offset:offset + length] == b'LaunchDaemons':
            jobs_index = value
    assert jobs_index is not None
    keys, values = dictionary_refs(jobs_index)

    def marker(kind, length):
        if length < 15:
            return bytes((kind | length,))
        width = next(width for width in (1, 2, 4, 8) if length < 1 << (8 * width))
        return bytes((kind | 15, 16 | (width.bit_length() - 1))) + length.to_bytes(width, 'big')

    def references(indices):
        return b''.join(index.to_bytes(reference_size, 'big') for index in indices)

    def append(value):
        if isinstance(value, bool):
            encoded = b'\x09' if value else b'\x08'
        elif isinstance(value, str):
            encoded = value.encode('ascii')
            encoded = marker(80, len(encoded)) + encoded
        elif isinstance(value, int):
            assert value >= 0
            width = next(width for width in (1, 2, 4, 8) if value < 1 << (8 * width))
            encoded = bytes((16 | (width.bit_length() - 1),)) + value.to_bytes(width, 'big')
        elif isinstance(value, list):
            encoded = marker(160, len(value)) + references([append(item) for item in value])
        elif isinstance(value, dict):
            key_refs = [append(key) for key in value]
            value_refs = [append(item) for item in value.values()]
            encoded = marker(208, len(value)) + references(key_refs + value_refs)
        else:
            raise TypeError('unsupported new launchd job value')
        index = len(offsets)
        assert index < 1 << (8 * reference_size), 'native object-reference width exhausted'
        offsets.append(len(body))
        body.extend(encoded)
        return index

    keys.append(append(path))
    values.append(append(job))
    offsets[jobs_index] = len(body)
    body.extend(marker(208, len(keys)) + references(keys + values))
    new_table = len(body)
    assert new_table < 1 << (8 * offset_size), 'native offset width exhausted'
    assert body[:table] == stock[:table], 'native object data changed'
    body.extend(b''.join(offset.to_bytes(offset_size, 'big') for offset in offsets))
    body.extend(struct.pack('>6sBBQQQ', bytes(6), offset_size, reference_size, len(offsets), root, new_table))
    return bytes(body)



def set_job_logging(stock, path, out_path, err_path):
    """Give one existing system job stdout/stderr files, at the binary-plist level.

    The entry cannot be rewritten in place, and launchd keeps the object graph it read: the job dictionary is
    appended again with the two keys added and the offset table entry of the old object is pointed at the new
    one, so every reference to it (the LaunchDaemons dictionary's) resolves to the updated copy.
    """
    _, offset_size, reference_size, count, root, table = struct.unpack('>6sBBQQQ', stock[-32:])
    offsets = [int.from_bytes(stock[table + i * offset_size:table + (i + 1) * offset_size], 'big')
               for i in range(count)]
    body = bytearray(stock[:table])

    def read_count(offset, kind):
        marker = stock[offset]
        assert marker >> 4 == kind, 'unexpected object kind'
        length = marker & 15
        offset += 1
        if length == 15:
            size = 1 << (stock[offset] & 15)
            offset += 1
            length = int.from_bytes(stock[offset:offset + size], 'big')
            offset += size
        return length, offset

    def dictionary_refs(index):
        length, offset = read_count(offsets[index], 13)
        refs = [int.from_bytes(stock[offset + i * reference_size:offset + (i + 1) * reference_size], 'big')
                for i in range(2 * length)]
        return refs[:length], refs[length:]

    def string_at(index):
        length, offset = read_count(offsets[index], 5)
        return stock[offset:offset + length].decode()

    def marker(kind, length):
        if length < 15:
            return bytes((kind | length,))
        width = next(width for width in (1, 2, 4, 8) if length < 1 << (8 * width))
        return bytes((kind | 15, 16 | (width.bit_length() - 1))) + length.to_bytes(width, 'big')

    def references(indices):
        return b''.join(index.to_bytes(reference_size, 'big') for index in indices)

    def append(encoded):
        index = len(offsets)
        offsets.append(len(body))
        body.extend(encoded)
        return index

    def append_string(text):
        encoded = text.encode('ascii')
        return append(marker(80, len(encoded)) + encoded)

    keys, values = dictionary_refs(root)
    jobs_index = None
    for key, value in zip(keys, values):
        if string_at(key) == 'LaunchDaemons':
            jobs_index = value
    assert jobs_index is not None
    job_keys, job_values = dictionary_refs(jobs_index)
    slot = [i for i, key in enumerate(job_keys) if string_at(key) == path]
    assert len(slot) == 1, 'the job to change is not in the manifest'
    slot = slot[0]
    job = job_values[slot]
    inner_keys, inner_values = dictionary_refs(job)
    out_ref = append_string(out_path)
    err_ref = append_string(err_path)
    new_keys = inner_keys + [append_string('StandardOutPath'), append_string('StandardErrorPath')]
    new_values = inner_values + [out_ref, err_ref]
    replacement = append(marker(208, len(new_keys)) + references(new_keys + new_values))
    # Point the old dictionary's slot at the new object: every reference resolves through the offset table.
    offsets[job] = offsets[replacement]
    assert body[:table] == stock[:table], 'native object data changed'
    new_table = len(body)
    body.extend(b''.join(offset.to_bytes(offset_size, 'big') for offset in offsets))
    body.extend(struct.pack('>6sBBQQQ', bytes(6), offset_size, reference_size, len(offsets), root, new_table))
    return bytes(body)

def signature_directories(blob):
    assert len(blob) >= 12
    magic, length, count = struct.unpack_from('>III', blob, 0)
    assert magic == 0xfade0cc0 and 12 + 8 * count <= len(blob)
    total, = struct.unpack_from('>I', blob, 4)
    assert total == len(blob)
    found = []
    for i in range(count):
        _, at = struct.unpack_from('>II', blob, 12 + 8 * i)
        kind, size = struct.unpack_from('>II', blob, at)
        assert 8 <= size <= len(blob) - at
        if kind == 0xfade0c02:
            found.append(blob[at:at + size])
    assert found, 'signature has no CodeDirectory'
    return found


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    stock_path, out_path = Path(sys.argv[1]), Path(sys.argv[2])
    stock = stock_path.read_bytes()
    packed = append_launchd_job(stock, JOB_PATH, JOB)
    packed = append_launchd_job(packed, ABM_PATH, ABM)
    packed = append_launchd_job(packed, ABMLITE_PATH, ABMLITE)
    packed = set_job_logging(packed, '/System/Library/LaunchDaemons/com.apple.CommCenter.plist',
                             '/private/var/db/commcenter.log', '/private/var/db/commcenter.log')
    # ABM's helper exits as soon as it decides it cannot serve, and its reason is the one line this port has
    # never been able to read: give the stock job the same two paths.
    packed = set_job_logging(packed, '/System/Library/LaunchDaemons/com.apple.abm-helper.plist',
                             '/private/var/db/abm-helper.log', '/private/var/db/abm-helper.log')
    # The original entries must be exactly what they were.
    check = plistlib.loads(packed)
    assert check['LaunchDaemons'].pop(JOB_PATH) == JOB, 'the added job does not read back'
    assert check['LaunchDaemons'].pop(ABM_PATH) == ABM, 'the ABM job does not read back'
    assert check['LaunchDaemons'].pop(ABMLITE_PATH) == ABMLITE, 'the abmlite job does not read back'
    cc = check['LaunchDaemons']['/System/Library/LaunchDaemons/com.apple.CommCenter.plist']
    assert cc.get('StandardOutPath') == '/private/var/db/commcenter.log', 'CommCenter logging was not set'
    assert cc.get('StandardErrorPath') == '/private/var/db/commcenter.log', 'CommCenter error logging was not set'
    check['LaunchDaemons']['/System/Library/LaunchDaemons/com.apple.CommCenter.plist'] = {
        k: v for k, v in cc.items() if k not in ('StandardOutPath', 'StandardErrorPath')}
    ab = check['LaunchDaemons']['/System/Library/LaunchDaemons/com.apple.abm-helper.plist']
    assert ab.get('StandardErrorPath') == '/private/var/db/abm-helper.log', 'abm-helper logging was not set'
    check['LaunchDaemons']['/System/Library/LaunchDaemons/com.apple.abm-helper.plist'] = {
        k: v for k, v in ab.items() if k not in ('StandardOutPath', 'StandardErrorPath')}
    assert check == plistlib.loads(stock), 'an original launchd entry changed'
    out_path.write_bytes(packed)
    signature = out_path.with_name(out_path.name + '.sig')
    # SecCodeMapMemory checks 4 KiB pages, which is what this OS's own manifest signature covers.
    subprocess.run(['codesign', '--force', '--sign', '-', '--identifier', 'com.fuxi.launchd.manifest',
                    '--digest-algorithm', 'sha256', '--pagesize', '4096', '--detached', str(signature),
                    str(out_path)], check=True)
    blob = signature.read_bytes()
    for cd in signature_directories(blob):
        offset, = struct.unpack_from('>I', cd, 16)
        slots, limit = struct.unpack_from('>II', cd, 28)
        size, _, _, bits = cd[36:40]
        assert bits == 12 and limit == len(packed), 'detached signature covers the wrong length'
        assert slots == (len(packed) + 4095) // 4096
        data = packed
        for i in range(slots):
            import hashlib
            expected = hashlib.sha256(data[i * 4096:(i + 1) * 4096]).digest()[:size]
            assert cd[offset + i * size:offset + (i + 1) * size] == expected, 'page mismatch'
        import hashlib
        print('manifest', len(packed), 'cdhash', hashlib.sha256(cd).hexdigest()[:40])
    subprocess.run(['codesign', '--verify', '--strict', '--detached', str(signature), str(out_path)], check=True)
    print('signed', signature, signature.stat().st_size, 'bytes')


if __name__ == '__main__':
    main()
