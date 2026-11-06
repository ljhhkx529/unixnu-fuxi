#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Publish the board's GPU-role headers into the XNU export tree before the archives build.

Adreno740 reads PRIO_DARWIN_GPU_BACKGROUND and task_get_gpu_role, which patches/xnu/fuxi-gpu-role.patch
adds to sys/resource_private.h and osfmk/kern/task.h. The archives build before the XNU pass, and they
include the headers from build/xnu/ios-obj/EXPORT_HDRS, which the previous XNU pass wrote: on a clean
tree the archive would be compiled against the unpatched copies and fail on those two names. This copies
the patched headers across first, so the archive sees what the kernel will.

The XNU pass rewrites the export tree from the same sources afterwards, so the copy is only a bridge.
"""
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / '.cache/xnu-work'
EXPORTS = [ROOT / 'build/xnu/ios-obj/EXPORT_HDRS', ROOT / 'build/xnu/obj/EXPORT_HDRS']
# Only the header whose change is a pair of added macros. osfmk/kern/task.h is exported through
# unifdef, so copying the source over the export would undo that pass and hide kernel imports (libpthread
# reported _current_task as unavailable when it did). The GPU role declaration reaches Adreno740 through
# the force-included kits/fuxi/FuxiGpuRole.h instead.
HEADERS = ('bsd/sys/resource_private.h',)


def main():
    copied = 0
    for name in HEADERS:
        source = WORK / name
        if not source.is_file():
            sys.exit(f'{source} missing: the XNU work tree has not been prepared')
        for export_root in EXPORTS:
            target = export_root / name
            if not target.is_file():
                continue
            if target.read_bytes() != source.read_bytes():
                # The XNU export step writes these read-only; the bridge has to rewrite them.
                target.chmod(0o644)
                shutil.copy2(source, target)
                copied += 1
    print(f'gpu-role headers: {copied} exported copies updated')


if __name__ == '__main__':
    main()
