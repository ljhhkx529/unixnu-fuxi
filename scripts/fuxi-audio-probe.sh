#!/usr/bin/env bash
# Build the audio probe image and run it on the phone, then print what it reported.
#
#   fuxi-audio-probe.sh run          build (incrementally), boot, wait, print the report
#   fuxi-audio-probe.sh build        build only
#   fuxi-audio-probe.sh report       print the newest report from the USB capture
#   fuxi-audio-probe.sh status       where the phone is
#
# The probe reads SMEM and writes nothing. It runs from bsd_init, so its report is in the kernel log before anything else
# can fail; the USB CDC channel carries that log out, which is why no photograph is needed.
#
# One deliberate difference from a normal image, confined to a scratch copy of the device repository: the boot argument
# fuxi-audio-probe=1 asks the probe to log its report. Nothing else is changed - in particular the Data mount stays on and
# the payload is built with the Data seed the tree documents (out/ipsw/21H461/data.dbr), so the phone boots to its desktop
# as usual and the report is read over the USB CDC channel. An earlier version of this script turned the Data mount off to
# dodge the seed requirement, and every run then left the owner with a stuck screen.
set -euo pipefail

DEVICE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WX=$(dirname "$DEVICE")
SCRATCH="${FUXI_AUDIO_PROBE_SCRATCH:-/tmp/fuxi-probe2}"
IMAGE="${FUXI_AUDIO_PROBE_IMAGE:-/tmp/fuxi-probe.img}"
UNIXNU="$WX/UniXNU"
PROFILE=out/fuxi/ipados-17.7.11-21H461/fuxi
SEED="${FUXI_DATA_SEED:-$UNIXNU/out/ipsw/21H461/data.dbr}"

scratch() {
    rm -rf "$SCRATCH"
    mkdir -p "$SCRATCH"
    (cd "$DEVICE" && tar -cf - --exclude .git .) | (cd "$SCRATCH" && tar -xf -)
    (cd "$SCRATCH" && git init -q . && git add -A && git -c user.email=a@b -c user.name=c commit -qm probe)
    python3 - "$SCRATCH" <<'PY'
import pathlib, sys
root = pathlib.Path(sys.argv[1])
board = root / "board.toml"
text = board.read_text()
if "fuxi-audio-probe" not in text:
    board.write_text(text.replace('"fuxi-touch=4", "fuxi-touch-hid=1",',
                                  '"fuxi-touch=4", "fuxi-touch-hid=1", "fuxi-audio-probe=1",'))
# The defconfig is left alone: the data mount and its seed are how this board boots to its desktop.
PY
}

build() {
    [ -d "$SCRATCH" ] || scratch
    cd "$UNIXNU"
    # The kernel is configured from the real device repository. A scratch copy that changed the defconfig also dropped the
    # display archive the GPU addon symbols come from, so the link failed - one more reason the scratch copy only carries
    # board.toml, which is where the probe's boot argument belongs.
    ./unixnu defconfig fuxi_defconfig -D "$DEVICE" >/dev/null
    ./unixnu build kernel
    # The payload step reads the .config, so the scratch copy's defconfig has to be the one in effect here: running the
    # kernel's defconfig last would leave the data-mount requirement on and the payload step would fail, after which
    # bootimg would quietly pack the previous payload.
    ./unixnu build payload -D "$SCRATCH" --uefi ../uefi-fuxi --kernel "$PROFILE/kernel/kernel.macho" \
        --unique-chip-id 0x0123456789abcdef --data-ramdisk "$SEED"
    ./unixnu bootimg -D "$SCRATCH" --uefi ../uefi-fuxi --target DEBUG \
        --dtb ../uefi-fuxi/Platforms/Xiaomi/fuxiPkg/FdtBlob/sm8550-xiaomi-fuxi.dtb -o "$IMAGE"
    # The payload carries the kernel it was built with; a stale one has bitten twice, so it is checked rather than assumed.
    local want got
    want=$(shasum -a 256 "$PROFILE/kernel/kernel.macho" | cut -d' ' -f1)
    got=$(shasum -a 256 "$PROFILE/payload/stage/EFI/UniXNU/Payloads/kernel.macho" 2>/dev/null | cut -d' ' -f1)
    [ "$want" = "$got" ] || { echo "the payload does not carry the kernel that was just built"; exit 1; }
    echo "image: $IMAGE (kernel $want)"
}

report() {
    local file
    file=$(ls -t /tmp/fuxi-usb/run-*/xnu-usb.log 2>/dev/null | head -1)
    [ -n "$file" ] || { echo "no capture under /tmp/fuxi-usb"; return 1; }
    echo "capture: $file"
    awk '/fuxi-audio-probe:/{flag=1} flag && /^fuxi-audio|^audio probe|^partition|^item|^  words/{print} /^partition global/{if (flag) exit}' "$file" | tail -40
}

case "${1:-run}" in
build) build ;;
run)
    build
    "$DEVICE/scripts/fuxi-usb-debug.sh" start >/dev/null 2>&1 || true
    fastboot devices | grep -q . || { echo "the phone is not in fastboot"; exit 1; }
    fastboot boot "$IMAGE"
    sleep 60
    report
    ;;
report) report ;;
status) fastboot devices; "$DEVICE/scripts/fuxi-usb-debug.sh" status 2>&1 | tail -3 ;;
*) echo "usage: $0 run|build|report|status"; exit 2 ;;
esac
