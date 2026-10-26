#!/usr/bin/env python3
"""Build the fuxi XNU CDC-ACM debug archive from QcomXnuUsbDebug.

The generic DWC3/SMMU/CDC implementation lives in
drivers/QcomXnuUsbDebug. package/FuxiUsbCompat.c keeps the existing
fuxi_usb_* hook names used by zzzd-fuxi-usb-debug.patch and runs the
generic poll worker. The objects are compiled with the real ARM64 XNU
startup compile record and force-linked by the kernel link step.
"""
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / 'drivers/QcomXnuUsbDebug'
OUT = ROOT / 'build/fuxi-usb-debug'
OUT.mkdir(parents=True, exist_ok=True)

RECORD = json.loads(
    (ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json')
    .read_text().strip().rstrip(',')
)

MODULE_UNITS = [
    MODULE / 'kernel/QcomXnuUsbDebug.c',
    MODULE / 'kernel/QcomUsbScreenshot.c',
    MODULE / 'usb/Dwc3.c',
    MODULE / 'soc/QcomUsbQualify.c',
    MODULE / 'soc/QcomUsbDmaDomain.c',
]

DEFINES = [
    '-I', str(MODULE),
    '-I', str(MODULE / 'include'),
    '-I', str(MODULE / 'kernel'),
    '-DQUD_SOC_HEADER="soc/Sm8550.h"',
    '-Wno-reserved-identifier',
    '-Wno-pre-c11-compat',
    '-Wno-switch-default',
]


def compile_like(source, obj):
    command = []
    args = RECORD['arguments']
    i = 0
    while i < len(args):
        arg = args[i]
        if arg == RECORD['file']:
            command.append(str(source))
        elif arg in ('-o', '-MF', '-MT', '-MJ'):
            if arg == '-o':
                command += ['-o', str(obj)]
            i += 1
        elif arg == '-Xclang' and i + 1 < len(args) and args[i + 1].startswith('-W'):
            i += 1
        elif not arg.startswith('-W'):
            command.append(arg)
        i += 1
    command += ['-Wall', '-Wextra', '-Werror', '-mgeneral-regs-only']
    command += DEFINES
    subprocess.run(command, cwd=RECORD['directory'], check=True, timeout=180)
    return obj


objects = []
for unit in MODULE_UNITS:
    obj = OUT / (unit.stem + '.o')
    compile_like(unit, obj)
    objects.append(obj)

compat = compile_like(ROOT / 'kits/sheng/FuxiUsbCompat.c', OUT / 'FuxiUsbCompat.o')
objects.append(compat)

archive = OUT / 'libFuxiUsbDebug.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True, timeout=60)
print(f'fuxi QcomXnuUsbDebug archive: {archive}')
