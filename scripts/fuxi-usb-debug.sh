#!/usr/bin/env bash
# Keep the fuxi XNU CDC debug port attached and make the control channel one command.
#
#   fuxi-usb-debug.sh start          attach and stay attached across reboots
#   fuxi-usb-debug.sh stop           detach
#   fuxi-usb-debug.sh status         supervisor, port and capture state
#   fuxi-usb-debug.sh log [lines]    follow the newest capture (default 40 lines then follow)
#   fuxi-usb-debug.sh ctl <verb>     hello | status | ping | reboot (hello is implicit)
#   fuxi-usb-debug.sh ls             list capture runs
#
# The collector owns the serial port while it runs, so every control command goes through its unix
# socket instead of opening the port a second time. Each attach gets a fresh run directory because
# the collector refuses to reuse one, and the supervisor restarts it when the phone reboots and the
# port comes back.
set -euo pipefail

DEVICE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
MODULE="$DEVICE/drivers/QcomXnuUsbDebug/scripts"
# macOS caps a unix socket path at 104 bytes, and the control socket lives inside the run
# directory, so the state directory has to stay short: the workspace path alone is already ~80.
STATE="${FUXI_USB_STATE:-/tmp/fuxi-usb}"
SERIAL="${FUXI_USB_SERIAL:-FuxiXnuUsbDebug}"
PYTHON="${PYTHON:-python3}"

mkdir -p "$STATE"
pidfile="$STATE/supervisor.pid"
stopfile="$STATE/stop"

port() { compgen -G "/dev/cu.usbmodem*" >/dev/null 2>&1 && ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true; }
current() { readlink -f "$STATE/current" 2>/dev/null || true; }
alive() { [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null; }

supervise() {
  rm -f "$stopfile"
  while [ ! -f "$stopfile" ]; do
    if [ -z "$(port)" ]; then sleep 1; continue; fi
    run="$STATE/run-$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$run"
    ln -sfn "$run" "$STATE/current"
    echo "fuxi-usb: attached $(port) -> $run"
    "$PYTHON" -B "$MODULE/collect-xnu-usb.py" --serial "$SERIAL" --output "$run" \
      --wait 600 --seconds 0 --no-marker --control-socket || true
    if [ -f "$stopfile" ]; then break; fi
    echo "fuxi-usb: collector exited, waiting for the port"
    sleep 1
  done
  rm -f "$pidfile"
}

case "${1:-}" in
  start)
    if alive; then echo "fuxi-usb: already attached (pid $(cat "$pidfile"))"; exit 0; fi
    pkill -f 'collect-xnu-usb.py' 2>/dev/null || true
    sleep 1
    nohup "$0" __supervise >"$STATE/supervisor.log" 2>&1 &
    echo $! >"$pidfile"
    sleep 2
    echo "fuxi-usb: supervisor pid $(cat "$pidfile"); log: $STATE/supervisor.log"
    ;;
  __supervise)
    supervise
    ;;
  stop)
    touch "$stopfile"
    pkill -f 'collect-xnu-usb.py' 2>/dev/null || true
    if alive; then kill "$(cat "$pidfile")" 2>/dev/null || true; fi
    sleep 1
    echo "fuxi-usb: detached"
    ;;
  status)
    if alive; then echo "supervisor: running (pid $(cat "$pidfile"))"; else echo "supervisor: stopped"; fi
    p="$(port)"; echo "port: ${p:-absent}"
    run="$(current)"; echo "run: ${run:-none}"
    if [ -S "$run/usb-control.sock" ]; then echo "control: ready"; else echo "control: not ready"; fi
    if [ -f "$run/capture-progress.json" ]; then
      "$PYTHON" - "$run" <<'PY'
import json, sys, pathlib
run = pathlib.Path(sys.argv[1])
log = run / 'xnu-usb.log'
print('captured: %d bytes' % (log.stat().st_size if log.exists() else 0))
PY
    fi
    ;;
  log)
    run="$(current)"
    if [ -z "$run" ] || [ ! -f "$run/xnu-usb.log" ]; then echo "fuxi-usb: no capture yet" >&2; exit 1; fi
    tail -n "${2:-40}" -f "$run/xnu-usb.log"
    ;;
  ctl)
    shift
    if [ $# -eq 0 ]; then echo "usage: $0 ctl <hello|status|ping|reboot>" >&2; exit 2; fi
    run="$(current)"
    if [ -z "$run" ] || [ ! -S "$run/usb-control.sock" ]; then
      echo "fuxi-usb: control channel not ready; is the phone booted with qcom-usb-ctl=1?" >&2
      exit 1
    fi
    if [ "${1}" != hello ]; then
      "$PYTHON" -B "$MODULE/xnu_usb_control.py" --socket "$run/usb-control.sock" hello >/dev/null || true
    fi
    "$PYTHON" -B "$MODULE/xnu_usb_control.py" --socket "$run/usb-control.sock" "$@"
    ;;
  ls)
    ls -1dt "$STATE"/run-* 2>/dev/null | head -20 || echo "none"
    ;;
  *)
    sed -n '2,16p' "$0"
    exit 2
    ;;
esac
