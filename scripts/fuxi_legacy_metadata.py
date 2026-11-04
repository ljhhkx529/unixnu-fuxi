#!/usr/bin/env python3
"""Static class ABI proof for the unchanged 21H461 IOMobileGraphicsFamily.

This is a compile-time check of real original initializer instructions, not a
replacement QEMU trace or a claim that a Legacy instance has run on a phone.
The caller must first verify the complete original family executable hash.
"""
import hashlib
import struct


def verify_static_metadata(read, names):
    chunks = []

    def checked_read(pc, size):
        value = read(pc, size)
        if len(value) != size:
            raise ValueError('Truncated original class metadata code')
        chunks.append(value)
        return value

    def require(value, message):
        if not value:
            raise ValueError(message)

    def adrp(pc, word, reg):
        require(word & 0x9f00001f == 0x90000000 | reg, 'Expected original ADRP register')
        imm = ((word >> 5) & 0x7ffff) * 4 + ((word >> 29) & 3)
        if imm & (1 << 20):
            imm -= 1 << 21
        return (pc & ~4095) + (imm << 12)

    def pair(pc, reg):
        a, b = struct.unpack('<2I', checked_read(pc, 8))
        require(b & 0xffc003ff == 0x91000000 | (reg << 5) | reg,
                'Expected unshifted original ADD address')
        return adrp(pc, a, reg) + ((b >> 10) & 4095)

    def branch(pc):
        word = struct.unpack('<I', checked_read(pc, 4))[0]
        require(word & 0xfc000000 == 0x94000000, 'Expected original BL')
        imm = word & 0x3ffffff
        return pc + 4 * (imm - (1 << 26) if imm & (1 << 25) else imm)

    # The initializer passes this exact singleton through two constructor
    # wrappers. Both wrappers preserve x0 through the observed stack slots.
    require(pair(0xfffffff005786558, 0) == 0xfffffff006b35940,
            'Original Legacy metaclass singleton changed')
    require(branch(0xfffffff005786560) == 0xfffffff00578656c and
            checked_read(0xfffffff005786578, 12) == bytes.fromhex('e00700f9e00740f9e00300f9') and
            branch(0xfffffff005786584) == 0xfffffff00578667c,
            'Original Legacy metaclass constructor wrappers changed')
    require(checked_read(0xfffffff005786698, 12) == bytes.fromhex('a0831ff8a0835ff8e00700f9'),
            'Original OSMetaClass constructor self argument changed')
    name = pair(0xfffffff0057866a4, 1)
    require(checked_read(name, len(b'IOMobileFramebufferLegacy\0')) == b'IOMobileFramebufferLegacy\0',
            'Original Legacy class name changed')
    parent = pair(0xfffffff0057866ac, 2)
    require(parent == 0xfffffff006b31400, 'Original Legacy parent metaclass changed')
    require(checked_read(0xfffffff0057866b4, 4) == bytes.fromhex('03148352'),
            'Original Legacy instance size is not 0x18a0')
    stub = branch(0xfffffff0057866b8)
    require(stub == 0xfffffff0057a2818, 'Original OSMetaClass constructor stub changed')
    a, b, c = struct.unpack('<3I', checked_read(stub, 12))
    require(b & 0xffc003ff == 0xf9400210 and c == 0xd61f0200,
            'Original metaclass stub is not ADRP/LDR/BR x16')
    slot = adrp(stub, a, 16) + ((b >> 10) & 4095) * 8
    target = struct.unpack('<Q', checked_read(slot, 8))[0]
    ctor = '__ZN11OSMetaClassC2EPKcPKS_j'
    require(ctor in names and target == names[ctor],
            'Original metaclass stub does not bind to the actual OSMetaClass constructor')
    return dict(kind='original-static-initializer', class_name='IOMobileFramebufferLegacy',
                metaclass='0xfffffff006b35940', parent_metaclass=hex(parent),
                instance_bytes=0x18a0, constructor=ctor,
                checked_bytes_sha256=hashlib.sha256(b''.join(chunks)).hexdigest(),
                runtime_observed=False, original_instructions_modified=False)
