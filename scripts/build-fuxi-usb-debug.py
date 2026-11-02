#!/usr/bin/env python3
"""Build and qualify the selected fuxi USB archive using actual XNU records."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
module = root / '.cache/fuxi-qud-module'
out = root / '.cache/fuxi-qud-build'
records = root / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
llvm = Path('/opt/homebrew/opt/llvm/bin')
if not llvm.is_dir():
    # Linux host: the framework's toolchain provides llvm-config. Its clang is the host
    # compiler the kit's tests must use, and /usr/bin has to come before the cross tools
    # so the host link gets the ELF linker rather than cctools' Mach-O ld.
    llvm = Path(subprocess.check_output(['llvm-config', '--bindir'], text=True).strip())
    os.environ['PATH'] = (str(llvm) + os.pathsep + '/usr/bin' + os.pathsep
                          + os.environ.get('PATH', ''))
    # check.py builds its host tests with -std=c11, which hides posix_memalign behind
    # glibc's feature-test macros; Apple's headers always declare it.
    staged_check = module / 'tests/check.py'
    if staged_check.is_file():
        source = staged_check.read_text()
        if '-D_POSIX_C_SOURCE' not in source and "'-std=c11'," in source:
            staged_check.write_text(source.replace("'-std=c11',",
                                                 "'-std=c11', '-D_POSIX_C_SOURCE=200809L',", 1))
def run(arguments):
    command = [str(v) for v in arguments]
    result = subprocess.run(command, cwd=root, capture_output=True, text=True)
    print(result.stdout, end='')
    if result.returncode:
        print(result.stderr, end='')
        for base in (out, root / '.cache/fuxi-qud-xnu-check'):
            if base.is_dir():
                for log in base.glob('*.log'):
                    content = log.read_text(errors='replace')
                    if 'error:' in content:
                        print(log.name, content[-6500:])
        raise SystemExit(result.returncode)

run([sys.executable, module / 'tests/check.py', '--target-clang', llvm / 'clang',
     '--output', root / '.cache/fuxi-qud-model-check'])
matches = list(records.glob('**/pe_kprintf.o.json'))
if len(matches) != 1:
    raise SystemExit('missing or ambiguous actual PE compile record')
run([sys.executable, module / 'scripts/build.py', '--compile-record', matches[0],
     '--soc', 'soc/FuxiSm8550.h', '--ar', llvm / 'llvm-ar', '--nm', llvm / 'llvm-nm', '--output', out])
run([sys.executable, module / 'tests/check_xnu.py', '--xnu', root / '.cache/xnu-work',
     '--records', records, '--archive', out / 'libQcomXnuUsbDebug.a', '--nm', llvm / 'llvm-nm',
     '--output', root / '.cache/fuxi-qud-xnu-check'])
for name in ('check-report.json', 'xnu-check-report.json'):
    directory = root / ('.cache/fuxi-qud-model-check' if name == 'check-report.json' else '.cache/fuxi-qud-xnu-check')
    print((directory / name).read_text())
print((out / 'build-report.json').read_text())
print('PASS: fuxi QcomXnuUsbDebug archive, models and six-unit enabled/disabled XNU integration')
