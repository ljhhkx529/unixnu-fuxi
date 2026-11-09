#!/usr/bin/env bash
# Where is the phone, and what can be done with it right now?
#
# The device is reachable two ways and the owner decides which by pressing keys, so every round has to ask rather than
# assume. This prints one of three states and exits with a code that matches:
#
#   fastboot  the boot loader is listening; images can be booted          exit 0
#   serial    a running kernel's USB control channel answers              exit 1
#   absent    neither answers; the phone is off or in another system      exit 2
#
# "serial" means a kernel with qcom-usb-debug is running: the control channel answers PING and the capture file grows. It
# does not mean the device can be flashed, and "absent" is not a failure of the device - it is usually just switched off.
set -uo pipefail

DEVICE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

fastboot_state() {
    local devices
    devices=$(fastboot devices 2>/dev/null | awk 'NF {print $1; exit}')
    [ -n "$devices" ] || return 1
    echo "state=fastboot serial=$devices"
    return 0
}

serial_state() {
    local ping status
    ping=$("$DEVICE/scripts/fuxi-usb-debug.sh" ctl ping 2>&1 | tail -1)
    case "$ping" in
        *'"ok": true'*) ;;
        *) return 1 ;;
    esac
    status=$("$DEVICE/scripts/fuxi-usb-debug.sh" status 2>&1 | tr '\n' ' ')
    echo "state=serial $status"
    echo "ping=$(echo "$ping" | sed 's/^ *//')"
    return 0
}

if fastboot_state; then
    exit 0
fi
if serial_state; then
    exit 1
fi

last=$(ls -t /tmp/fuxi-usb/run-*/xnu-usb.log 2>/dev/null | head -1)
if [ -n "$last" ]; then
    echo "state=absent last_capture=$last last_bytes=$(wc -c < "$last" | tr -d ' ')"
else
    echo "state=absent last_capture=none"
fi
exit 2
