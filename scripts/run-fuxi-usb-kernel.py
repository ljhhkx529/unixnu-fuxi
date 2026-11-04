#!/usr/bin/env python3
"""Wrap the existing kernel recipe with the pinned fuxi USB source profile."""
from pathlib import Path
import os
import shlex
import subprocess

root = Path(__file__).resolve().parents[1]
original = root / 'scripts/fuxi-kernel-baseline.sh'
if os.environ.get('FUXI_USB_REFERENCE_ONLY') == '1':
    # This link-only reference is never a phone boot artifact.
    raise SystemExit(subprocess.run(['bash', str(original)], cwd=root).returncode)
text = original.read_text()
if 'run-fuxi-usb-kernel.py' in text:
    raise SystemExit('wrapper cannot recursively invoke itself')
anchor = 'ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)'
if text.count(anchor) != 1:
    raise SystemExit('kernel recipe root anchor changed')
text = text.replace(anchor, 'ROOT=' + shlex.quote(str(root)))
anchor = '# The optional observation hooks overlap existing root/exec patch context.'
prepare = '''python3 "$ROOT/scripts/fuxi-usb-boot-progress.py" "$ROOT/.cache/xnu-work" --reverse
python3 "$ROOT/scripts/prepare-fuxi-usb-debug.py" \
  --module "$ROOT/drivers/QcomXnuUsbDebug" --xnu "$ROOT/refer/xnu" \
  --output "$ROOT/.cache/fuxi-qud-module"
QUD_MODULE="$ROOT/.cache/fuxi-qud-module"
QUD_PATCHER="$QUD_MODULE/scripts/xnu_patches.py"
QUD_STATE=$(python3 "$QUD_PATCHER" status --xnu "$ROOT/.cache/xnu-work" --profile darwin23-fuxi)
# Only when the tree is still unpatched: after the framework series is in place, reversing
# here would move bsd_init.c away from the context the owner patches were applied against.
if [[ "$QUD_STATE" == *'"state_before": "applied"'* ]] && \
   ! grep -q native_owner_cache_prepare "$ROOT/.cache/xnu-work/bsd/kern/kern_exec.c" 2>/dev/null; then
  python3 "$QUD_PATCHER" reverse --xnu "$ROOT/.cache/xnu-work" --profile darwin23-fuxi
fi
'''
if text.count(anchor) != 1:
    raise SystemExit('kernel patch preparation anchor changed')
text = text.replace(anchor, prepare + '\n' + anchor)
anchor = 'export XNU_AVAILABILITY_PL=$ROOT/.cache/fakeroot/usr/local/libexec/availability.pl'
apply = '''QUD_STATE2=$(python3 "$QUD_PATCHER" status --xnu "$ROOT/.cache/xnu-work" --profile darwin23-fuxi)
if [[ "$QUD_STATE2" == *'"state_before": "ready"'* ]]; then
  python3 "$QUD_PATCHER" check --xnu "$ROOT/.cache/xnu-work" --profile darwin23-fuxi
  python3 "$QUD_PATCHER" apply --xnu "$ROOT/.cache/xnu-work" --profile darwin23-fuxi
fi
python3 "$ROOT/scripts/fuxi-usb-boot-progress.py" "$ROOT/.cache/xnu-work"
'''
if text.count(anchor) != 1:
    raise SystemExit('kernel patch completion anchor changed')
text = text.replace(anchor, apply + anchor)
anchor = 'make -C .cache/xnu-work "${XNU_BUILD_TARGET:-build}"'
flags = '''# Preserve all previously selected per-object options (PMGR, root probes,
# native console and VM flags); append USB only to its six translation units.
for qud_object in pe_kprintf.o debug.o printf.o serial_console.o serial_general.o bsd_init.o; do
  qud_key="${qud_object}_CFLAGS_ADD="
  qud_found=0
  for qud_index in "${!EXTRA_MAKE_ARGS[@]}"; do
    if [[ "${EXTRA_MAKE_ARGS[$qud_index]}" == "$qud_key"* ]]; then
      EXTRA_MAKE_ARGS[$qud_index]="${EXTRA_MAKE_ARGS[$qud_index]} -DXNU_QCOM_USB_DEBUG=1 -I$QUD_MODULE/include"
      qud_found=1
    fi
  done
  if [ "$qud_found" = 0 ]; then
    EXTRA_MAKE_ARGS+=("${qud_key}-DXNU_QCOM_USB_DEBUG=1 -I$QUD_MODULE/include")
  fi
done
'''
if text.count(anchor) != 1:
    raise SystemExit('kernel make anchor changed')
text = text.replace(anchor, flags + anchor)
out = root / '.cache/fuxi-usb-kernel-recipe.sh'
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(text)
marker = root / '.cache/fuxi-usb-bootstrap'
first_pass = os.environ.get('XNU_BOOTSTRAP') == '1'
result = subprocess.run(['bash', str(out)], cwd=root)
if first_pass:
    # The driver runs this recipe twice. The first pass only has to produce the
    # compile records, and it always stops at the kernel link because no archive
    # step has run yet (libQcomXnuUsbDebug.a, the UFS kit, the stub). Tolerating
    # that lets the driver reach those steps; the second invocation is the real
    # link and keeps its exit status.
    records = sorted(root.glob('build/xnu/**/link.filelist'))
    if not records:
        # A broken patch or a compile error fails here too, and the tail of the log is
        # then just the usual link failure. Without this check the driver carried on to
        # the archives and corecrypto stopped at 'No XNU symbol names'.
        print('fuxi USB bootstrap pass produced no compile records: the recipe failed before the link')
        raise SystemExit(result.returncode or 1)
    marker.parent.mkdir(parents=True, exist_ok=True)
    marker.write_text(str(result.returncode) + '\n')
    print('fuxi USB bootstrap pass wrote %d compile record file(s) and stopped with status %d; the final link runs after the archives'
          % (len(records), result.returncode))
    raise SystemExit(0)
raise SystemExit(result.returncode)
