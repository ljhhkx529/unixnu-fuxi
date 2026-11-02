# First device boots, 2026-11-02

Device: Xiaomi 13 (fuxi), 12 GB; fastboot reports `product: fuxi`, `unlocked: yes`, `current-slot: a`; Android
OS4.0.0.32.XPCCNXM. Every image was started with `fastboot boot`; nothing was written to the device. The images and
kernels derive from Apple software and are not published: the SHA-256 values below identify them, and the decrypted
CI artifacts can be rebuilt from the listed commits. Panel photographs are kept by the owner; the rows below are
transcribed from them.

## Inputs

| Build | Device commit | Kernel (prelinked, unpatched) | Firmware volume |
| --- | --- | --- | --- |
| [run 41273003819](https://github.com/OddlyLabs/unixnu-fuxi/actions/runs/41273003819) | 2bbd3993e97081db16d8ecc00c58c78f53200b8e | not uploaded | 627fe52a |
| [run 41363175206](https://github.com/OddlyLabs/unixnu-fuxi/actions/runs/41363175206) | 12f9f38d63e4642d335583e159603cfcea2c3744 | 9ac9e37a88df44c4 | 627fe52a |
| [run 41372182658](https://github.com/OddlyLabs/unixnu-fuxi/actions/runs/41372182658) | 65c4f4e4c59f08e81b6790fe75d4c3eaaf04e794 (AMP patch) | d7a84922df8c60c7 | 627fe52a |
| [run 41378843724](https://github.com/OddlyLabs/unixnu-fuxi/actions/runs/41378843724) | e90113974b1186120e4eb4c3f77bfb8d909934bf (`CONFIG_XNU_DT_PANIC_REGION`) | e088da4d4b1c663d | d6edb042 |

The two firmware volumes hold the same code: of their 54 modules only PcdDxe differs, in 8 bytes of its CodeView
record (the PDB GUID).

Probe images were packed locally from the kernel of the run before them (`./unixnu build payload --kernel`, then
`./unixnu bootimg --fd` with the firmware volume of the same run), with `entry_probe_reset_delay = 6`,
`entry_probe_reset_after` and `panic_screen = false` in a scratch copy of `board.toml`. Such an image waits 6 s after
the probed call and resets with PSCI SYSTEM_RESET; a phone that comes back to Android reached the call.

## First image

`fastboot boot` of the run 41273003819 image (SHA-256 `931ac958…`): `Sending OKAY`, `Booting OKAY`. The panel shows the
DEBUG firmware log, the UniXNU menu starting its entry, DarwinBoot's hand-off page, then DarwinBoot's green band and the
entry probe's blue (XNU entry, MMU off) and magenta (before the MMU enable) bands; no kernel text and no panic screen
follow. The hand-off page of that day's boots reads:

```
kernel   va 0xFFFFFFF00427C000  pa 0x8C007C000
args     ...  window 0x130000000 bytes
video    base 0xB8000000  1080x2400  row 4320 bytes  32 bpp
CurrentEL 1   ...   (PARange 2, 4 KiB granule supported)
```

## Reset probes

Times are local (UTC+8). "Watched" is how long the runner saw no USB device.

| Image | Started | Probe | SHA-256 | Result |
| --- | --- | --- | --- | --- |
| 1-mmu | 08:03:15 | stage 3: 6 s after the MMU enable | `a9b83b627e94877e` | reset: Android up after 124s |
| 5-vm | 08:05:50 | after call #1 to `_arm_vm_init` in `_arm_init` (0xfffffff0079b8a68) | `cf1202539f832085` | no reset (332s watched) |
| 9-vm-ttb | 08:29:11 | after call #1 to `_set_mmu_ttb` in `_arm_vm_init` (0xfffffff0073ea120) | `00a45d7c6dbe7003` | no reset (331s watched) |
| 4-bootstrap | 08:44:37 | after call #1 to `_processor_bootstrap` in `_arm_init` (0xfffffff0079b89dc) | `aa235b8c205c1f77` | no reset (201s watched) |
| 16-tbi | 08:58:20 | after call #1 to `_arm_set_kernel_tbi` in `_arm_init` (0xfffffff0079b8330) | `33bccb7f278cd0dd` | reset: Android up after 69s |
| 17-cpudata | 08:59:41 | after call #1 to `_cpu_data_init` in `_arm_init` (0xfffffff0079b8898) | `b9fa3ad9bf5088e6` | reset: Android up after 68s |
| 2-platform | 09:01:03 | after call #1 to `_PE_init_platform` in `_arm_init` (0xfffffff0079b88a4) | `94a0bd8910958489` | reset: Android up after 68s |
| 18-appleregs | 09:02:25 | after call #1 to `_configure_misc_apple_regs` in `_arm_init` (0xfffffff0079b88bc) | `eea263ba58537539` | reset: Android up after 69s |
| 3-topology | 09:03:47 | after call #1 to `_ml_parse_cpu_topology` in `_arm_init` (0xfffffff0079b893c) | `dfb081851d3b1592` | no reset (201s watched) |
| a1-topology | 09:35:16 | after call #1 to `_ml_parse_cpu_topology` in `_arm_init` (0xfffffff0079beaf4) | `b65f1f956a6c5eea` | reset: Android up after 69s |
| a2-console | 09:36:38 | after call #1 to `_PE_init_printf` in `_arm_init` (0xfffffff0079bed70) | `c62851cf475985f6` | no reset (201s watched) |
| b01-thread | 09:46:09 | after call #1 to `_thread_bootstrap` in `_arm_init` (0xfffffff0079beb60) | `af626ef7147da9da` | reset: Android up after 69s |
| b02-cpuboot | 09:47:31 | after call #1 to `_cpu_bootstrap` in `_arm_init` (0xfffffff0079beb74) | `134582fc9c66ebbe` | reset: Android up after 69s |
| b03-rtclock | 09:48:53 | after call #1 to `_rtclock_early_init` in `_arm_init` (0xfffffff0079beb78) | `863b699accf927a3` | reset: Android up after 68s |
| b04-startup | 09:50:15 | after call #1 to `_kernel_startup_bootstrap` in `_arm_init` (0xfffffff0079beb88) | `869bfa05daf2348c` | reset: Android up after 68s |
| b05-timercall | 09:51:37 | after call #1 to `_timer_call_init` in `_arm_init` (0xfffffff0079beb8c) | `349d2774eebbf473` | reset: Android up after 69s |
| b06-cpuinit | 09:53:00 | after call #1 to `_cpu_init` in `_arm_init` (0xfffffff0079beb90) | `52b207e2f12496e0` | reset: Android up after 70s |
| b07-procboot | 09:54:23 | after call #1 to `_processor_bootstrap` in `_arm_init` (0xfffffff0079beb94) | `803556600fc44c3b` | reset: Android up after 69s |
| b08-vm | 09:55:45 | after call #1 to `_arm_vm_init` in `_arm_init` (0xfffffff0079bec20) | `f621ceb0cba26bc4` | reset: Android up after 69s |
| b09-serial | 09:57:07 | after call #1 to `_serial_init` in `_arm_init` (0xfffffff0079bec3c) | `8b797d0be2139380` | reset: Android up after 66s |
| b10-upto | 09:58:27 | after call #1 to `_kernel_startup_initialize_upto` in `_arm_init` (0xfffffff0079bec48) | `12dd09f223a87a70` | reset: Android up after 69s |
| b11-createcons | 09:59:49 | after call #1 to `_PE_create_console` in `_arm_init` (0xfffffff0079bed68) | `bfb7c108f774a1a3` | reset: Android up after 66s |
| c1-official | 10:12:51 | CI image of run 41372182658 (panic screen, no reset probe) | `bd8a55066c196feb` | no reset; panel: the three bands and the end of the firmware log |
| d1-official | 11:07:05 | CI image of run 41378843724 (panic screen, no reset probe) | `e360c41aa4057d27` | no reset; panel: the touch report below |

Images 1-mmu to 3-topology use the run 41363175206 kernel, a1 to c1 the run 41372182658 kernel (with the AMP patch).

Reading:

1. Without the AMP patch the kernel never returns from `ml_parse_cpu_topology`: the ADT gives two cluster types and
   the BCM2837 target allows one cluster. With the patch the same probe resets.
2. With the AMP patch every probe up to `PE_create_console` resets, the one after `PE_init_printf` does not, and the
   official image shows nothing after the bands. Without `CONFIG_XNU_DT_PANIC_REGION`, `PE_create_console` maps the
   Raspberry Pi panic log address 0x3C0FC000, reads it and zeroes 16 KiB there; no RAM or device tree node covers that
   address on this phone. With the option set (and no other kernel change) the boot reaches `bsd_init`. How the access
   fails was not measured; the two probes around it disagree, which fits an asynchronous fault.

## Touch report (image d1-official)

The kernel reaches `bsd_init`; `native_touch_probe` (`fuxi-touch=3 fuxi-touch-panic=1`) runs stage 3 and panics with its
rows, drawn by the panic screen below DarwinBoot's green band and the probe's blue band:

```
fuxi-touch report (stage 3 of 3)
FTS s3/3 st7 e0 vote443c0200 h00000000 80000002 00004000 00004000 00004000 gpe0000005
FTS p00000085 00000085 00000085 00000087 rst000302c3 irq000000c3 hw30090000 L2/4 12000000Hz
FTS pwr vdd40900 avdd41b00 tcs2 rc0 w-1 id3640 rc0 rst0/1 sp40 ev030000 sense99
FTS rd0 ev0 in0 mv0 up0 er0/00 rdy0 rearm0 c0 -1,-1 hid0 irq0/0 xf0 @FuxiTouchProbe.cpp:530
frames fffffff007a1f030 fffffff00797be74 fffffff0073f74a4
pages=0x0000000000127068
free=0x0000000000125ccd
```

Decoded with `package/FuxiTouchProbe.cpp` at e901139 (`rows()`, `enable()`, `touchThread()`):

| Field | Value | Meaning |
| --- | --- | --- |
| `s3/3 st7 e0` | stage 3 of 3, step 7, error 0 | stages 1 to 3 completed; step 7 is the controller reset and chip id |
| `vote` | 0x443c0200 | GCC vote register after the QUP votes |
| `h` | M_AHB 0x00000000, S_AHB 0x80000002, CORE, CORE_2X, S4 0x00004000 | every branch running; S_AHB shows CLK_OFF with hardware gating (bit 1), which the probe accepts as on sheng |
| `gp` | 0xe0000005 | GPLL0 mode, lock bit 31 set |
| `p` | 0x85, 0x85, 0x85, 0x87 | GPIO 44 to 47: function 1 (qup1_se4), 6 mA, pull-down on 44 to 46 and pull-up on 47 (CS), as the stock DT |
| `rst` | cfg 0x2c3, in/out 0x3 | GPIO 24: GPIO, 8 mA, pull-up, output enabled, driven high (reset released) |
| `irq` | cfg 0xc3, in/out 0x0 | GPIO 25: GPIO input, 8 mA, pull-up; the line read low at stage 2, before the reset |
| `hw` | 0x30090000 | QUP hardware version 3.9.0 |
| `L2/4 12000000Hz` | DFS level 2, divider 4 | SPI clock exactly 12 MHz |
| `pwr vdd40900 avdd41b00` | command database addresses | `ldob12` and `ldom6` found by name, the values measured on Android |
| `tcs2 rc0 w-1` | TCS 2, request status 0, no completion seen | the votes were written; the done bit did not show within the 10 ms wait. Whether it latches without the TCS interrupt is unknown, so the probe records -1 and goes on |
| `id3640 rc0` | chip id 36 40 | ST FTM5, the expected id |
| `rst0/1 sp40` | reset status 0 after 1 attempt, no 4-wire fallback | the controller answered the reset pulse within 100 ms |
| `ev030000` | last event 03 00 00 | CONTROLLER_READY |
| `sense99` | not run | sensing starts in stage 4 |
| row 4 | all zero, `xf0` | stage 4 counters; no SPI transfer failed |
| `frames` | `touchThread` (cold), `touchThread`, `Call_continuation` | the panic is the probe's own report in its kernel thread |
| `pages`, `free` | 0x127068 (4.61 GiB), 0x125ccd (4.59 GiB) | `vm_page_pages` and `vm_page_free_count`: the kernel manages the 4.75 GiB window minus its image |

Not shown by this report: whether the RPMh votes changed anything (the bootloader may have left both rails on;
`fuxi-touch-power=0` would tell), and stage 4 (sensing, interrupt, touch events).

## Stage 4 attempt (image t4-touch)

Packed locally from the run 41378843724 kernel and firmware with `fuxi-touch=4 fuxi-touch-panic=2` (SHA-256
`a43145873cbdfd7e`), started at 12:03:36. The panel shows a panic screen that is not the touch report:

```
"AMFI: No PMGR?\n" @ConfigurationSettings.cpp:235
frames fffffff004ac9420 fffffff004aaed08 fffffff004aaeaac fffffff004ab865c fffffff0072d8608 fffffff0072d9be8 fffffff0073f74a4
pages=0x0000000000127069
free=0x0000000000125cce
```

The first four frames are in the prelinked kext text (`__PLK_TEXT_EXEC`); two of them lie within 0x200 bytes of AMFI's PMGR
call site at 0xfffffff004aaeb40, the address UniXNU's `scripts/audit-amfi-pmgr-contract.py` pins. The last three are
`thread_call_invoke`, `thread_call_thread` and `Call_continuation` in the kernel: AMFI checks for the PMGR from a thread
call. In the stage 3 runs the touch probe panicked first; in stage 4 it waits for a touch, so AMFI's panic comes before
any touch could be reported. The fix is UniXNU-sheng's PMGR bridge (`CONFIG_XNU_SHENG_PMGR`, `sheng-pmgr=2`; see the power
area of `porting.toml`).

## With the PMGR bridge (run 41390823512, device commit 484a650)

Kernel `99e19285024252de`, firmware volume `dd72bd0c` (the same code as `627fe52a`: only PcdDxe's PDB GUID differs). The
post-link check `ci/check-pmgr-bridge.py` passes on this kernel. Each image below changes only the boot arguments, on top
of `-v serial=3 debug=0x14e kt_fixed_zones=120 sheng-pmgr=2`.

| Image | Started | Added arguments | SHA-256 | Result |
| --- | --- | --- | --- | --- |
| t4b-touch | 13:10:36 | `fuxi-touch=4 fuxi-touch-panic=2` | `ca080270f58e179c` | the phone reset by itself: the owner saw the bands, then a restart; Android up 65 s after the boot command |
| p1-pmgr | 13:25:38 | none | `cba346b63087500c` | no reset in the 150 s watched; panel: first the bands, then `Failed to mount root device` |
| p3-poll | 13:38:54 | `fuxi-touch=4 fuxi-touch-poll=1 fuxi-touch-panic=2` | `d17cc5498a404abd` | no reset; panel: the stage 4 touch report below |

Reading: the bridge satisfies AMFI, and with it the kernel runs on to the root device wait. The reset appeared once, with
the touch probe's interrupt armed; the polling image differs from it only by `fuxi-touch-poll=1` and does not reset. One run
each, so the cause is not settled.

p1-pmgr panel:

```
Failed to mount root device @IOKitBSDInit.cpp:984
frames fffffff007a1c978 fffffff007929898 fffffff0076b5c44 fffffff00799620c fffffff0073f74a4
pages=0x000000000012705c
free=0x0000000000125c61
```

The frames are `IOFindBSDRoot` (cold path and body), `bsd_init`, `kernel_bootstrap_thread` and `Call_continuation`: the main
boot path, at the end of the wait for a root device (ROOTDEVICETIMEOUT is 60 s in this XNU; the time was not measured). The payload has none, so this panic is the expected end of the boot.

p3-poll panel, after one tap near the lower left of the panel:

```
fuxi-touch report (stage 4 of 4)
FTS s4/4 st10 e0 vote443c0200 h00000000 80000002 00004000 00004000 00004000 gpe0000005
FTS p00000085 00000085 00000085 00000087 rst000302c3 irq000000c3 hw30090000 L2/4 12000000Hz
FTS pwr vdd40900 avdd41b00 tcs2 rc0 w-1 id3640 rc0 rst0/1 sp40 ev030000 sense0
FTS rd3531 ev12 in1 mv8 up1 er0/00 rdy0 rearm0 c0 2885,18595 hid0 irq0/0 xf0 @FuxiTouchProbe.cpp:530
frames fffffff007a20434 fffffff00797c758 fffffff0073f74a4
pages=0x000000000012705d
free=0x0000000000125c42
```

| Field | Value | Meaning |
| --- | --- | --- |
| `s4/4 st10 e0` | stage 4 of 4, step 10, error 0 | sensing started and the event loop ran |
| `sense0` | status 0 | `A0 00 00`, 50 ms, `A0 00 01` succeeded |
| `rd3531` | 3531 FIFO reads | polled every 8 ms or more: at least 28 s between the start of sensing and the end of the tap |
| `ev12` | 12 events | 10 touch events, 2 others (not errors and not ready events) |
| `in1 mv8 up1` | 1 enter, 8 moves, 1 leave | one complete touch, which is what `fuxi-touch-panic=2` waits for |
| `er0/00 rdy0 rearm0` | no error event, no unexpected ready, no re-arm | the controller stayed up |
| `c0` | no contact | the finger had lifted |
| `2885,18595` | last position, raw | range 0..10800 x 0..24000: pixel (288, 1859), 17.7 mm from the left and 33 mm from the bottom edge |
| `hid0` | no HID report | no HID layer is linked yet |
| `irq0/0` | no interrupt | polling mode |
| `xf0` | no failed transfer | all 3531 reads and the commands before them |
| `frames` | `touchThread` (cold), `touchThread`, `Call_continuation` | the probe's own report |

The position fits the tap the owner was asked to make (lower left, about 2 cm from both edges): `y` is above the short-axis
range, so the axes are not swapped, and small `x` with large `y` means neither is mirrored. One tap, not a calibration.
