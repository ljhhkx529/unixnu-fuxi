#!/usr/bin/env python3
"""Keep board progress output while initializing the USB transport exactly once."""
import argparse
from pathlib import Path

OLD = '\tif (boot_progress_active()) { PE_kputc = boot_progress_putc; return; }'
NEW = '''\tif (boot_progress_active()) {
#if XNU_QCOM_USB_DEBUG
\t\t(void)qcom_usb_debug_init();
#endif
\t\tPE_kputc = boot_progress_putc;
\t\treturn;
\t}'''

def update(path, reverse=False):
    text = path.read_text()
    old, new = (NEW, OLD) if reverse else (OLD, NEW)
    if text.count(old) == 1 and new not in text:
        path.write_text(text.replace(old, new))
    elif text.count(new) != 1 or old in text:
        raise SystemExit('Unexpected boot-progress/USB initialization context')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('xnu', type=Path)
    parser.add_argument('--reverse', action='store_true')
    args = parser.parse_args()
    path = args.xnu / 'pexpert/arm/pe_kprintf.c'
    # Fresh trees have neither board hook yet; reversal is only preparation.
    if not args.reverse or (path.exists() and NEW in path.read_text()):
        update(path, args.reverse)
