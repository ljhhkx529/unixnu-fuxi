#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build build/fuxi-keystore/libFuxiKeyStore.a for the fuxi kernel.

The archive holds the board glue for the owned AppleKeyStore (kits/fuxi/FuxiKeyStoreProbe.cpp), which
UniXNU-sheng builds the same way from package/KeyStoreProbe.cpp: the AppleKeyStoreState component
calls five entry points it defines, and its own probe instantiates that service once. It is compiled
with the flags of the XNU build's own compile record for iokit IOPlatformExpert.cpp, with XNU's
warning flags replaced by -Wall -Wextra -Werror and the same few exceptions.

bsd_init calls native_keystore_probe() (patches/xnu/fuxi-keystore-probe.patch) and fuxi_defconfig
passes the archive to the link in CONFIG_XNU_EXTRA_LD_ARCHIVES.
"""
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]   # the assembled build tree: scripts are copied into it
OUT = ROOT / 'build/fuxi-keystore'
OBJ = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
KIT = ROOT / 'kits/fuxi'
CXX_FLAGS = ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unnecessary-virtual-specifier',
             '-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion']


def record(path):
    if not path.is_file():
        sys.exit(f'{path} missing: the XNU pass of the kernel step has not left its compile records')
    return json.loads(path.read_text().strip().rstrip(','))


def command(rec, source, obj, extra):
    """The record's command for another source and object, without XNU's warning flags and dependency outputs."""
    args, out, i = rec['arguments'], [], 0
    while i < len(args):
        arg = args[i]
        if arg == rec['file']:
            out.append(str(source))
        elif arg in ('-o', '-MF', '-MT', '-MJ'):
            if arg == '-o':
                out += ['-o', str(obj)]
            i += 1
        elif arg == '-Xclang' and i + 1 < len(args) and args[i + 1].startswith('-W'):
            i += 1
        elif not arg.startswith('-W'):
            out.append(arg)
        i += 1
    return out + [str(x) for x in extra]


KEYEVENTS = ROOT / 'build/apple-key-store-state'


def objcopy(*arguments):
    """llvm-objcopy from the same LLVM the archive builders use."""
    for candidate in ('/opt/homebrew/opt/llvm/bin/llvm-objcopy', '/usr/local/opt/llvm/bin/llvm-objcopy',
                      shutil.which('llvm-objcopy')):
        if candidate and Path(candidate).is_file():
            subprocess.run([candidate, *map(str, arguments)], check=True, timeout=120)
            return
    sys.exit('llvm-objcopy missing: brew install llvm')


def keystore_object():
    """The component's object, prepared the way UniXNU-sheng links it.

    NativeKeyStoreEvents.cpp carries a static constructor in __DATA,__mod_init_func, and its symbol is
    local. The probe runs that constructor itself (native_keyevents_static_init) after OSMetaClass
    preModLoad, so the section is dropped and the symbol globalised; otherwise the kernel's own
    init-function table would run it before the class is registered.
    """
    source = KEYEVENTS / 'NativeKeyStoreEvents.o'
    if not source.is_file():
        sys.exit(f'{source} missing: component:AppleKeyStoreState has to build first')
    late = KEYEVENTS / 'NativeKeyStoreEvents.late.o'
    objcopy('--remove-section=__DATA,__mod_init_func',
            '--globalize-symbol=__GLOBAL__sub_I_NativeKeyStoreEvents.cpp', source, late)
    return late


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    cxx = record(OBJ / 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')
    source = KIT / 'FuxiKeyStoreProbe.cpp'
    if not source.is_file():
        sys.exit(f'{source} missing: the fuxi device repository owns this board glue')
    obj = OUT / 'FuxiKeyStoreProbe.o'
    subprocess.run(command(cxx, source, obj, CXX_FLAGS), cwd=cxx['directory'], check=True, timeout=180)
    print(f'compiled {source.relative_to(ROOT)}')
    late = keystore_object()
    archive = OUT / 'libFuxiKeyStore.a'
    archive.unlink(missing_ok=True)
    ar = ['xcrun', 'ar'] if shutil.which('xcrun') else [shutil.which('llvm-ar') or 'ar']
    subprocess.run([*ar, 'rcs', str(archive), str(obj), str(late)], check=True, timeout=60)
    print(f'keystore archive: {archive.relative_to(ROOT)}')


if __name__ == '__main__':
    main()
