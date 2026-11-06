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
# The module's host tests bind AF_UNIX sockets, which macOS caps near 104 bytes. The
# assembled tree path is far longer than that, so the check outputs live under /tmp.
CHECKS = Path('/tmp/fuxi-qud-checks')
CHECKS.mkdir(parents=True, exist_ok=True)
model_check = CHECKS / 'model-check'
xnu_check = CHECKS / 'xnu-check'
# check.py/build.py require a fresh output directory on every run (mkdir exist_ok=False).
for previous in (model_check, xnu_check, out):
    if previous.exists():
        shutil.rmtree(previous)


def resolve_qud_module():
    """QcomXnuUsbDebug is not a selected component, so tree assembly does not link its
    directory. Resolve the device checkout through the already-linked FuxiFtsTouch symlink
    and link drivers/QcomXnuUsbDebug into the tree when it is missing (the same pattern as
    build-fuxi-ufs.py's resolve_native_ufs)."""
    qud = root / 'drivers/QcomXnuUsbDebug'
    if (qud / 'tests/check.py').is_file():
        return qud
    fts = root / 'drivers/FuxiFtsTouch'
    if fts.is_symlink():
        candidate = fts.resolve().parent / 'QcomXnuUsbDebug'
        if (candidate / 'tests/check.py').is_file():
            if not qud.exists():
                qud.symlink_to(candidate, target_is_directory=True)
            return qud
    sys.exit(f'{qud}/tests/check.py missing: add the drivers/QcomXnuUsbDebug submodule first')


qud = resolve_qud_module()
# Stage the pinned QcomXnuUsbDebug and its Darwin23/fuxi integration profile on first use
# (prepare-fuxi-usb-debug.py is idempotent once the staged module record matches).
if not (module / 'tests/check.py').is_file():
    subprocess.run([sys.executable, root / 'scripts/prepare-fuxi-usb-debug.py',
                    '--module', qud, '--xnu', root / 'refer/xnu',
                    '--output', module], check=True)

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
        for base in (out, xnu_check):
            if base.is_dir():
                for log in base.glob('*.log'):
                    content = log.read_text(errors='replace')
                    if 'error:' in content:
                        print(log.name, content[-6500:])
        raise SystemExit(result.returncode)

run([sys.executable, module / 'tests/check.py', '--target-clang', llvm / 'clang',
     '--output', model_check])
matches = list(records.glob('**/pe_kprintf.o.json'))
if len(matches) != 1:
    raise SystemExit('missing or ambiguous actual PE compile record')
# Only the iOS 17 fuxi archive opts into the post-drain hook, and only once the module is staged:
# the header the module's panic path includes is copied next to it, and the request/commit calls stay
# thin because package/QcomReset.c owns every register and every firmware call.
shutil.copy2(root / 'kits/fuxi/BootResetCore.h', module / 'kernel/BootResetCore.h')
board_header = module / 'soc/FuxiSm8550.h'
if 'FUXI_PANIC_FASTBOOT' not in board_header.read_text():
    with board_header.open('a') as stream:
        stream.write('\n#define FUXI_PANIC_FASTBOOT 1\n')
if 'fuxi_panic_fastboot(void)' not in (module / 'kernel/QcomXnuUsbDebug.c').read_text():
    subprocess.run(['patch', '-p1', '--batch', '--forward', '-i',
                    str(Path(__file__).with_name('fuxi-usb-panic-fastboot.patch'))],
                   cwd=module, check=True)

run([sys.executable, module / 'scripts/build.py', '--compile-record', matches[0],
     '--soc', 'soc/FuxiSm8550.h', '--ar', llvm / 'llvm-ar', '--nm', llvm / 'llvm-nm', '--output', out])


def compile_compat(source, obj):
    """Compile package/FuxiUsbCompat.c with the XNU startup compile record and the staged USB headers.
    zzzd-fuxi-usb-debug.patch calls fuxi_usb_debug_start / fuxi_usb_append, which this adapter defines."""
    rec = json.loads((records / 'osfmk/DEVELOPMENT/startup.o.json').read_text().strip().rstrip(','))
    args, i = rec['arguments'], 0
    command = []
    while i < len(args):
        a = args[i]
        if a == rec['file']:
            command.append(str(source))
        elif a == '-o':
            command += ['-o', str(obj)]
            i += 1
        elif a in ('-MF', '-MT', '-MJ'):
            i += 1
        elif a == '-Xclang' and i + 1 < len(args) and args[i + 1].startswith('-W'):
            i += 1
        elif not a.startswith('-W'):
            command.append(a)
        i += 1
    command += ['-I', str(module / 'include'), '-I', str(source.parent)]
    subprocess.run(command, cwd=rec['directory'], check=True, timeout=90)
    return obj


compat_o = out / 'FuxiUsbCompat.o'
compile_compat(root / 'kits/fuxi/FuxiUsbCompat.c', compat_o)
subprocess.run([str(llvm / 'llvm-ar'), 'q', str(out / 'libQcomXnuUsbDebug.a'), str(compat_o)], check=True)
# The reset layer is separate from the transport on purpose: a control channel asks it for a mode and
# never touches a register, so a second entry point (UART, another client) needs no new backend.
reset_o = out / 'QcomReset.o'
compile_compat(root / 'kits/fuxi/QcomReset.c', reset_o)
subprocess.run([str(llvm / 'llvm-ar'), 'q', str(out / 'libQcomXnuUsbDebug.a'), str(reset_o)], check=True)
run([sys.executable, module / 'tests/check_xnu.py', '--xnu', root / '.cache/xnu-work',
     '--records', records, '--archive', out / 'libQcomXnuUsbDebug.a', '--nm', llvm / 'llvm-nm',
     '--output', xnu_check])
for name in ('check-report.json', 'xnu-check-report.json'):
    directory = model_check if name == 'check-report.json' else xnu_check
    print((directory / name).read_text())
print((out / 'build-report.json').read_text())
print('PASS: fuxi QcomXnuUsbDebug archive, models and six-unit enabled/disabled XNU integration')
