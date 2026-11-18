# The baseband bridge's first boot, 2026-11-18

Device: Xiaomi 13 (fuxi), 12 GB. The kernel with the QcomBaseband archive force-loaded was flashed as
`/tmp/fuxi-baseband.img` (kernel.macho SHA-256
`fc04506a22c539daee40c0006d794223454e16c77cb9e6698453781de074bdd0`) and the phone rebooted into it. The log
below is the USB CDC capture of that boot, `/private/tmp/fuxi-usb/run-20261118-131152/xnu-usb.log`.

## What the boot shows

The boot arguments the phone was given:

```
com.apple.xpc.launchd|1970-01-01 00:00:34.471154 <Notice>: boot-args = -v serial=3 debug=0x14e boot-progress=2 kt_fixed_zones=120 sheng-pmgr=2 sheng-smp=1 armcpu-spi-ipi=1 armcpu-self-ipi=1 native-ufs-root=1 fuxi-touch=4 fuxi-touch-hid=1 sheng-touch-hid=1 fuxi-buttons=1 sheng-disable-watchdogd=1 qcom-usb-debug=1 qcom-usb-ctl=1 fuxi-panic-fastboot=30 fuxi-adsp=2 fgraph=6 sheng-aks=1 sheng-data-persist=2 sheng-data-reset=1 offline-key-events=1 offline-legacy-phase=4 offline-legacy-register=1 offline-legacy-gop=1 offline-legacy-pm=1 offline-legacy-validate=1 offline-legacy-transfer=1 offline-legacy-cpu-present=1 fuxi-abyp=1 fuxi-amp=7 offline-legacy-unit-scale=3 offline-mobile-graphics-load=1 offline-mobile-graphics-client=1 offline-apfs-load=1 sheng-gpu-power=9 sheng-gpu-zap=4 fuxi-probe-tctrl=0x08000001 fuxi-probe-prologue=1 fuxi-cp-protect=0 sheng-gpu-metal=1 sheng-gpu-wc=0 sheng-tu-debug=sysmem sheng-gpu-dcvs=1 sheng-fb-gpu=1 sheng-gpu-addon=1 sheng-disable-jobs=com.apple.identityservicesd,com.apple.locationd,com.apple.CommCenter,com.apple.bluetoothd,com.apple.liveactivitiesd
```

The APFS root volume came up from the UFS device and launchd reported the boot complete:

```
com.apple.xpc.launchd|1969-12-31 16:00:35.538163 (boot) <Notice>: Early boot complete. Continuing system boot.
[System Event] [682383704] [INFO] [Subsystem: launchd] [Event: boot] Early boot complete. Continuing system boot.
volume_needs_purgeable_fixup:525: disk1s1 volume Data needs purgeable record fixup.
finish_purgeable_fixup:7206: disk1s1 found 12 dups/clones on Data (num & max ids: 144 / 8192)
```

The baseband bridge itself is silent, as it must be: it publishes nothing until
`/chosen/coachz-baseband` is set in the device tree, and that node is not there yet. The log line below is
the board's own baseband presence, from the ADSP side, and it says nothing about the modem:

```
fuxi-audio: service published, state 1
```

## What it does not show

No modem was started. The `fuxi-modem` boot argument was not set, so the modem probe never ran and the
GLINK edge, the SMP2P and the QRTR link were never exercised. The modem PAS ids, the GLINK item ids and
the SMP2P items are confirmed from their sources but unverified at run time.

