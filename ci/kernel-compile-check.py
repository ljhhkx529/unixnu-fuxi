#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Compile the fuxi touch probe and driver with the flags of a real XNU build, then resolve their imports against it.

UniXNU's kernel step starts with an XNU pass that leaves clang compile records (-MJ) in the build tree, and the device
archive scripts take their flags from those records. UniXNU-sheng builds its touch probe from the record of
IOPlatformExpert.cpp and its touch C sources from the record of startup.c: XNU's own -W flags are dropped and
-Wall -Wextra -Werror with a few exceptions are added (scripts/build-gop-legacy-display.py). This script does the same for

  control  sheng's own TouchProbe.cpp, which shows that the harness reproduces sheng's build
  fuxi     package/FuxiTouchProbe.cpp, drivers/FuxiFtsTouch/*.c and the sheng sources they call (GeniSpi.c, QcomRpmh.c)

and checks that every symbol the fuxi objects import is defined by one of them or by an object of the XNU build, or is a
weak import, and that they define no symbol an XNU object also defines.

usage: kernel-compile-check.py <UniXNU checkout> <this repository> <output directory>
"""
import json
import os
import subprocess
import sys
from pathlib import Path

OBJ = 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
CXX_RECORD = OBJ + '/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
C_RECORD = OBJ + '/osfmk/DEVELOPMENT/startup.o.json'
# scripts/build-gop-legacy-display.py of UniXNU-sheng: the flags its probes and touch sources are built with.
CXX_FLAGS = ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unnecessary-virtual-specifier',
             '-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion']
C_FLAGS = ['-Wall', '-Wextra', '-Werror', '-mgeneral-regs-only']


def find_tree(unixnu):
    for directory, names, _ in os.walk(unixnu / 'out'):
        path = Path(directory)
        if path.name == 'tree':
            names[:] = []
            if (path / C_RECORD).is_file() and (path / CXX_RECORD).is_file():
                return path
    sys.exit(f'no build tree with XNU compile records under {unixnu / "out"}')


def record(tree, name):
    return json.loads((tree / name).read_text().strip().rstrip(','))


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


def parse_nm(text, sets):
    """Adds the names of nm -m output to (strong definitions, weak definitions, undefined, weak undefined)."""
    strong, weak, undefined, weak_undefined = sets
    for line in text.splitlines():
        line = line.strip()
        if not line or line.endswith(':') or 'non-external' in line:
            continue
        name = line.split()[-1]
        if '(undefined)' in line:
            (weak_undefined if ' weak ' in line else undefined).add(name)
        elif ' external ' in line:
            (weak if ' weak ' in line else strong).add(name)
    return sets


def symbols(objects):
    sets = (set(), set(), set(), set())
    for start in range(0, len(objects), 400):
        result = subprocess.run(['xcrun', 'nm', '-m', *map(str, objects[start:start + 400])], capture_output=True, text=True)
        parse_nm(result.stdout, sets)
    return sets


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    unixnu, repo, out = (Path(a).resolve() for a in sys.argv[1:])
    out.mkdir(parents=True, exist_ok=True)
    tree = find_tree(unixnu)
    cxx, c = record(tree, CXX_RECORD), record(tree, C_RECORD)
    osfmk = ['-I', tree / '.cache/xnu-work/osfmk']
    sheng = ['-I', tree / 'kits/sheng', '-I', tree / 'kits/Sm8550Platform', '-I', tree / 'drivers/QcomGeniSpi',
             '-I', tree / 'drivers/ShengNovatekTouch', '-I', tree / 'drivers/ShengUfs']
    fuxi = ['-I', repo / 'drivers/FuxiFtsTouch', '-I', repo / 'include']
    jobs = [('control', 'sheng TouchProbe.cpp', command(cxx, tree / 'kits/sheng/TouchProbe.cpp', out / 'ShengTouchProbe.o',
                                                        CXX_FLAGS + osfmk + sheng), cxx['directory'])]
    objects = []
    for rec, source, flags in [(cxx, repo / 'package/FuxiTouchProbe.cpp', CXX_FLAGS + osfmk + fuxi + sheng),
                               (c, repo / 'drivers/FuxiFtsTouch/FtsProtocol.c', C_FLAGS + osfmk),
                               (c, repo / 'drivers/FuxiFtsTouch/FtsTouchCore.c', C_FLAGS + osfmk),
                               (c, tree / 'drivers/QcomGeniSpi/GeniSpi.c', C_FLAGS + osfmk),
                               (c, tree / 'kits/sheng/QcomRpmh.c', C_FLAGS + osfmk)]:
        obj = out / (source.stem + '.o')
        objects.append(obj)
        jobs.append(('fuxi', source.name, command(rec, source, obj, flags), rec['directory']))

    report = ['## Kernel compile check', '', f'Build tree: `{tree.relative_to(unixnu)}`; compiler: `{cxx["arguments"][0]}`', '',
              '| Set | Source | Result |', '|---|---|---|']
    failed = 0
    with open(out / 'compile.log', 'w') as log:
        for group, name, cmd, cwd in jobs:
            result = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
            log.write(f'== {group} {name}\n$ {" ".join(cmd)}\n{result.stdout}{result.stderr}\n')
            ok = result.returncode == 0
            failed += not ok
            report.append(f'| {group} | `{name}` | {"ok" if ok else "**failed**"} |')
            print(f'{group} {name}: {"ok" if ok else "FAILED"}')
            if not ok:
                print(result.stderr[-8000:])

    if not failed:
        xnu = sorted((tree / OBJ).rglob('*.o'))
        xnu_strong, xnu_weak, _, _ = symbols(xnu)
        own_strong, own_weak, undefined, weak_undefined = symbols(objects)
        known = own_strong | own_weak | xnu_strong | xnu_weak
        unresolved = sorted(undefined - known)
        weak_missing = sorted(weak_undefined - known)
        duplicates = sorted(own_strong & xnu_strong)
        report += ['', f'XNU objects: {len(xnu)}, their external definitions: {len(xnu_strong) + len(xnu_weak)}.',
                   f'fuxi objects import {len(undefined)} strong and {len(weak_undefined)} weak symbols.', '',
                   f'- unresolved strong imports: {", ".join(f"`{s}`" for s in unresolved) or "none"}',
                   f'- weak imports the XNU objects do not define (bound at boot when another archive provides them): '
                   f'{", ".join(f"`{s}`" for s in weak_missing) or "none"}',
                   f'- definitions that an XNU object also has: {", ".join(f"`{s}`" for s in duplicates) or "none"}']
        failed += bool(unresolved) + bool(duplicates)
    report += ['', 'Result: ' + ('**failed**' if failed else 'passed')]
    text = '\n'.join(report) + '\n'
    (out / 'summary.md').write_text(text)
    print(text)
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as summary:
            summary.write(text)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
