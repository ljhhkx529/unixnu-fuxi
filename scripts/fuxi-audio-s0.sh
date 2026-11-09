#!/usr/bin/env bash
# One stable boot of the frozen default image, verified and recorded.
#
# S0 asks for three of these. The script does one, because the phone has to go back to fastboot between boots and only the
# owner can do that; run it again for the second and third and the three records together are the gate.
#
# What it proves: the image on disk carries the kernel that was just built, it starts userland, the control channel
# answers, and no audio step ran. What it cannot prove: that the owner pressed the keys three times in a row without
# changing the image, which is why the record keeps the hashes.
set -euo pipefail

DEVICE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WX=$(dirname "$DEVICE")
UNIXNU="$WX/UniXNU"
PROFILE=out/fuxi/ipados-17.7.11-21H461/fuxi
SEED="${FUXI_DATA_SEED:-$UNIXNU/out/fuxi/build/owner-data-fixture/owner-data-16g.dbr}"
IMAGE="${FUXI_S0_IMAGE:-/tmp/fuxi-s0.img}"
RECORD="${FUXI_S0_RECORD:-$DEVICE/facts/audio/s0-boots.txt}"
WAIT="${FUXI_S0_WAIT:-90}"

# The probe exits non-zero for every state but fastboot, and that is information rather than a failure.
state=$("$DEVICE/scripts/fuxi-audio-device.sh" 2>/dev/null | head -1 || true)
case "$state" in
state=fastboot*) ;;
*) echo "the phone is not in fastboot ($state)"; exit 1 ;;
esac

[ -f "$SEED" ] || { echo "no data seed at $SEED"; exit 1; }

cd "$UNIXNU"
./unixnu build payload --uefi ../uefi-fuxi --kernel "$PROFILE/kernel/kernel.macho" \
    --unique-chip-id 0x0123456789abcdef --data-ramdisk "$SEED" > /tmp/fuxi-s0-payload.log 2>&1
./unixnu bootimg --uefi ../uefi-fuxi --target DEBUG \
    --dtb ../uefi-fuxi/Platforms/Xiaomi/fuxiPkg/FdtBlob/sm8550-xiaomi-fuxi.dtb -o "$IMAGE" > /tmp/fuxi-s0-bootimg.log 2>&1

kernel_sha=$(shasum -a 256 "$PROFILE/kernel/kernel.macho" | cut -d' ' -f1)
payload_kernel=$(shasum -a 256 "$PROFILE/payload/stage/EFI/UniXNU/Payloads/kernel.macho" | cut -d' ' -f1)
[ "$kernel_sha" = "$payload_kernel" ] || { echo "the payload does not carry the kernel that was built"; exit 1; }
image_sha=$(shasum -a 256 "$IMAGE" | cut -d' ' -f1)
if grep -q 'fuxi-audio-step\|fuxi-audio-probe' "$DEVICE/board.toml"; then
    echo "board.toml asks for an audio step; this is not a default image"; exit 1
fi

cd "$DEVICE"
./scripts/fuxi-usb-debug.sh start >/dev/null 2>&1 || true
fastboot boot "$IMAGE" > /tmp/fuxi-s0-fastboot.log 2>&1

answer=""
for _ in $(seq 1 "$WAIT"); do
    answer=$(./scripts/fuxi-usb-debug.sh ctl ping 2>&1 | tail -1)
    case "$answer" in *'"ok": true'*) break ;; esac
    answer=""
    sleep 1
done
capture=$(ls -t /tmp/fuxi-usb/run-*/xnu-usb.log 2>/dev/null | head -1)
audio_lines=0
[ -n "$capture" ] && audio_lines=$(grep -c 'AUDIO step\|fuxi-audio-probe' "$capture" 2>/dev/null || true)

mkdir -p "$(dirname "$RECORD")"
{
    echo "boot $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "  device_repo=$(git -C "$DEVICE" rev-parse --short HEAD) unixnu=$(git -C "$UNIXNU" rev-parse --short HEAD)"
    echo "  kernel_sha256=$kernel_sha"
    echo "  image_sha256=$image_sha"
    echo "  seed=$SEED"
    echo "  control=$( [ -n "$answer" ] && printf '%s' "$answer" | sed 's/^ *//' || echo 'no answer' )"
    echo "  capture=$capture audio_lines=$audio_lines"
} | tee -a "$RECORD"

if [ -z "$answer" ]; then
    echo "FAIL the control channel did not answer within ${WAIT}s"; exit 1
fi
if [ "$audio_lines" != "0" ]; then
    echo "FAIL an audio step ran in a default image"; exit 1
fi
echo "PASS one stable boot recorded; press the keys to return to fastboot and run again"
