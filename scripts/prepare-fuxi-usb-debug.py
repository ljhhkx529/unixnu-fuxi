#!/usr/bin/env python3
"""Stage pinned QcomXnuUsbDebug with a separate Darwin23/fuxi integration profile."""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

QUD_REVISION = '83be5782ad7227d7d245862ce1ae3272cae14242'
XNU_REVISION = 'd8b80295118ef25ac3a784134bcf95cd8e88109f'
PROFILE = 'darwin23-fuxi'


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError('ambiguous source adaptation anchor: ' + old[:120])
    return text.replace(old, new)


def stage(module, xnu, output):
    module, xnu, output = module.resolve(), xnu.resolve(), output.resolve()
    def git(path, *args):
        return subprocess.check_output(['git', '-C', str(path), *args], text=True).strip()
    if git(module, 'rev-parse', 'HEAD') != QUD_REVISION or git(xnu, 'rev-parse', 'HEAD') != XNU_REVISION:
        raise ValueError('unexpected module/XNU source revision')
    takeover_patch = Path(__file__).with_name('fuxi-usb-exact-stream.patch')
    signature = hashlib.sha256(Path(__file__).read_bytes() + takeover_patch.read_bytes()).hexdigest()
    record = output / 'fuxi-stage.json'
    if record.is_file():
        if json.loads(record.read_text())['adapter_sha256'] != signature:
            raise ValueError('staged adapter changed; use a fresh build tree')
        return
    if output.exists():
        raise FileExistsError(output)
    shutil.copytree(module, output, ignore=shutil.ignore_patterns('.git', '__pycache__'))
    (output / 'soc/FuxiSm8550.h').write_text('''/* fuxi board policy; hardware register layout is the existing SM8550 profile.
 * PHY, power and device routing must be inherited from the supplied firmware.
 * No fuxi PSCI reset qualification or display provider is claimed here. */
#include "Sm8550.h"
#undef QUD_SOC_NAME
#define QUD_SOC_NAME "fuxi-sm8550-inherited"
#undef QUD_USB_SERIAL
#define QUD_USB_SERIAL "FuxiXnuUsbDebug"
#undef QUD_USB_PRODUCT
#define QUD_USB_PRODUCT "Xiaomi 13 XNU Debug"
#undef QUD_PSCI_SMC_RESET
#define QUD_PSCI_SMC_RESET 0
#define QUD_ALLOW_EXACT_STREAM_TAKEOVER 1
''')
    subprocess.run(['git', 'apply', '--check', str(takeover_patch)], cwd=output, check=True)
    subprocess.run(['git', 'apply', str(takeover_patch)], cwd=output, check=True)
    original_patch = (module / 'patches/darwin22/0001-qcom-usb-debug-console.patch').read_text()
    # Three files retain their reviewed upstream hooks verbatim. Darwin23 moved
    # disableConsoleOutput checks into the central console accessors; tee raw
    # bytes before those checks and remove only the now-redundant PE USB route.
    pieces = ['--- a/' + part for part in original_patch.split('--- a/')[1:]]
    kept = [part for part in pieces if not part.startswith(('--- a/osfmk/kern/printf.c\n', '--- a/osfmk/console/serial_console.c\n'))]
    custom = []
    for name in ('osfmk/kern/printf.c', 'osfmk/console/serial_console.c'):
        old = subprocess.check_output(['git', '-C', str(xnu), 'show', XNU_REVISION + ':' + name], text=True)
        new = old
        if name.endswith('/printf.c'):
            new = replace_once(new, 'void\nconsdebug_putc(char c)\n', '#if XNU_QCOM_USB_DEBUG\n#include <QcomXnuUsbDebug.h>\n#endif\n\nvoid\nconsdebug_putc(char c)\n')
            for func, sink in (('consdebug_putc', 'console_write_char'), ('consdebug_putc_unbuffered', 'console_write_unbuffered')):
                begin = 'void\n' + func + '(char c)\n{\n\t' + sink + '(c);'
                patched = 'void\n' + func + '''(char c)
{
#if XNU_QCOM_USB_DEBUG
\tconst bool usb_via_pe_console = !console_is_serial() && !disable_serial_output &&
\t    PE_kputc == console_write_unbuffered;
\tif (!usb_via_pe_console)
#endif
\t''' + sink + '(c);'
                new = replace_once(new, begin, patched)
            # Exactly two debug output routes need this guard.
            needle = '\tif (!console_is_serial() && !disable_serial_output) {\n\t\tPE_kputc(c);\n\t}'
            if new.count(needle) != 2:
                raise ValueError('unexpected Darwin23 debug output routes')
            new = new.replace(needle, '\tif (!console_is_serial() && !disable_serial_output) {\n#if XNU_QCOM_USB_DEBUG\n\t\tif (PE_kputc && PE_kputc != qcom_usb_debug_putc)\n#endif\n\t\tPE_kputc(c);\n\t}')
        else:
            new = replace_once(new, '#include <libkern/section_keywords.h>\n', '#include <libkern/section_keywords.h>\n#if XNU_QCOM_USB_DEBUG\n#include <QcomXnuUsbDebug.h>\n#endif\n')
            anchor = 'console_write(char *str, int size)\n{\n'
            insertion = '''#if XNU_QCOM_USB_DEBUG
\tif (str != NULL) for (int offset = 0; offset < size;) {
\t\tint bytes = size - offset;
\t\tif (bytes > 16384) bytes = 16384;
\t\t(void)qcom_usb_debug_write(str + offset, (size_t)bytes);
\t\toffset += bytes;
\t}
#endif
'''
            new = replace_once(new, anchor, anchor + insertion)
            anchor = 'console_write_unbuffered(char c)\n{\n'
            new = replace_once(new, anchor, anchor + '#if XNU_QCOM_USB_DEBUG\n\tqcom_usb_debug_putc(c);\n#endif\n')
        custom.append(''.join(difflib.unified_diff(old.splitlines(True), new.splitlines(True), fromfile='a/' + name, tofile='b/' + name)))
    directory = output / 'patches' / PROFILE
    directory.mkdir()
    (directory / '0001-qcom-usb-debug-console.patch').write_text(''.join(kept + custom))
    shutil.copyfile(module / 'patches/darwin22/0002-qcom-usb-debug-worker.patch', directory / '0002-qcom-usb-debug-worker.patch')
    path = output / 'scripts/xnu_patches.py'
    text = path.read_text()
    text += '''\n# Consumer-specific profile; retain the shipped Darwin22 pin unchanged.\nXNU_PROFILES['darwin23-fuxi'] = dict(XNU_PROFILES['darwin22'],\n    xnu_version='10063.141.1', xnu_revision='d8b80295118ef25ac3a784134bcf95cd8e88109f')\n'''
    # The new entry must exist before argparse main executes.
    suffix = text[text.index('\n# Consumer-specific profile;'):]
    text = text[:text.index('\n# Consumer-specific profile;')]
    text = replace_once(text, "if __name__ == '__main__':", suffix + "\nif __name__ == '__main__':")
    path.write_text(text)
    path = output / 'tests/check_xnu.py'
    path.write_text(path.read_text().replace("'darwin22'", "'darwin23-fuxi'"))
    path = output / 'tests/check.py'
    text = path.read_text()
    text = replace_once(text, "profiles = {'sm8550': ROOT / 'soc/Sm8550.h'}", "profiles = {'sm8550': ROOT / 'soc/Sm8550.h', 'fuxi': ROOT / 'soc/FuxiSm8550.h'}")
    path.write_text(text)
    record.write_text(json.dumps(dict(module_revision=QUD_REVISION, xnu_revision=XNU_REVISION,
                     adapter_sha256=signature, profile=PROFILE, reset_qualified=False,
                     cold_initialization=False, hardware_verified=False,
                     exact_stream_takeover=True,
                     takeover_patch_sha256=hashlib.sha256(takeover_patch.read_bytes()).hexdigest()), indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--module', type=Path, required=True)
    parser.add_argument('--xnu', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    stage(args.module, args.xnu, args.output)
    print(args.output)
