#!/usr/bin/env bash
# Keep the CDC control path alive for the device-side stale watchdog.
#
# The device resets when no control frame has arrived for `fuxi-cdc-stale`
# seconds (default 15). This loop sends a PING every 5 s so a wedged control
# path — not an idle host — is what triggers the reset. Run it in the same
# shell session as the collector:
#
#   ./scripts/fuxi-usb-heartbeat.sh
set -u
cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 1
while true; do
    ./scripts/fuxi-usb-debug.sh ctl ping >/dev/null 2>&1 || true
    sleep 5
done
