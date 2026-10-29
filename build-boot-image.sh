#!/usr/bin/env bash
# Build a bootable image with the Adreno Metal plugin correctly trusted.
#
# The payload's --owner-trust-cache must be the OWNER trust bundle produced by
# scripts/make-owner-trust-bundle.py: one owner module (NativeAdrenoMetal +
# adrenoprobed + Turnip CDHashes) plus the two unchanged Apple modules. Passing
# the original Apple-only bundle leaves the plugin untrusted, so AMFI rejects it
# at runtime ("adhoc signed" / "unsuitable CT policy 0") and boot stalls at the
# logo before SpringBoard/backboardd can open a Metal device.
#
# Device inputs (device repo, uefi, unique chip id, data ramdisk) come from
# unixnu.local.toml and are read by ./unixnu itself; this script hardcodes no
# device-specific values. Override the extra native-Metal clients with
# GPU_CLIENTS="name,..." (backboardd is always included by the plugin).
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

GPU_CLIENTS=${GPU_CLIENTS:-mediaserverd,SpringBoard}

json=$(./unixnu status --json)
read -r NAME OUT < <(printf '%s' "$json" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["name"], d["output"])')

TREE="$OUT/tree"
OWNER_TRUST="$TREE/build/adreno-metal-plugin/owner-trust.bundle"
GPU_ADDON=${GPU_ADDON:-"$OUT/build/$NAME-gpu-addon.ngpu"}

# The UEFI checkout is the only input ./unixnu status does not expose; read it
# from the same local config the build driver uses.
UEFI=$(python3 - "$ROOT/unixnu.local.toml" <<'PY'
import sys, tomllib
from pathlib import Path
cfg = Path(sys.argv[1])
build = tomllib.load(cfg.open('rb')).get('build', {})
uefi = build.get('uefi')
if not uefi:
    sys.exit('unixnu.local.toml [build] needs uefi')
print(str((cfg.parent / uefi).resolve()))
PY
)

./unixnu build kernel

# The GPU pieces build inside the assembled tree, where Turnip, the bridge and
# the reference inputs live. Their outputs are consumed by absolute path below.
cd "$TREE"
if [ ! -f build/original-static-system-trust/original-static-system.bundle ]; then
  python3 scripts/build-original-system-trust.py
fi
python3 scripts/build-adreno-metal-plugin.py
python3 scripts/make-owner-trust-bundle.py

# The addon carries the same plugin files the owner module trusts; rebuild it
# so the CDHash in the trust bundle and the bytes on disk can never drift.
rm -f "$GPU_ADDON"
python3 scripts/make-gpu-addon.py --publish --clients "$GPU_CLIENTS" "$GPU_ADDON"
cd "$ROOT"

./unixnu build payload \
  --owner-trust-cache "$OWNER_TRUST" \
  --gpu-addon "$GPU_ADDON" \
  --skip-setup

python3 "$UEFI/scripts/bootimage.py" \
  --ramdisk "$OUT/payload/payload.img" \
  -o "$OUT/boot/boot.img"

python3 - "$OUT/payload/payload.json" "$OWNER_TRUST" <<'PY'
import json, struct, sys
from pathlib import Path
report = json.loads(Path(sys.argv[1]).read_text())
owner = Path(sys.argv[2]).read_bytes()
modules = struct.unpack_from('<I', owner)[0]
assert report.get('trust_cache_sha256'), 'payload trust_cache_sha256 is empty'
assert report.get('gpu_addon_sha256'), 'payload gpu_addon_sha256 is empty'
assert modules == 3, f'owner trust bundle has {modules} modules, expected 3'
print(f'PASS: trust cache = {modules} modules (owner + 2 Apple), gpu addon present')
PY
