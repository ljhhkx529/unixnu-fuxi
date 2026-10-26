<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/logo-dark.svg">
    <source media="(prefers-color-scheme: light)" srcset="assets/logo-light.svg">
    <img alt="UniXNU" src="assets/logo-light.svg" width="200">
  </picture>
</p>

# UniXNU-fuxi

Device repository for the Xiaomi 13 (fuxi, SM8550). It holds the board file, kernel defconfigs, device scripts, drivers and package.

- [Documentation](https://unixnu.com)
- [Porting guide](https://unixnu.com/porting/)
- [Build](https://unixnu.com/building)

Build with the `unixnu` driver of [UniXNU](https://github.com/UniXNU/UniXNU).

## State

The fuxi port (`Platforms/Xiaomi/fuxiPkg`) starts through `fastboot boot` and reaches its boot manager with the firmware framebuffer. The board file and the ADT generator carry the values read from the stock firmware and the stock device tree.

On top of that the tree boots iPadOS 17.7.11 (iPad7,1 userland): NativeUfs mounts the OS/SystemOS windows from userdata, `sheng-data-persist=2` keeps a persistent Data-store window at the end of userdata, and the Adreno 740 presents the UI through NativeAdreno/NativeMetal. Everything is RAM boots (`fastboot boot`); the platform seed is not stored in the repository (`unique_chip_id = "required"`).

## Known-good boot arguments

Beyond the arguments the board generates, these are the fuxi-specific switches and why they are there:

| Argument | Why |
| --- | --- |
| `native-fuxi-ufs-root=1` | start NativeUfs from the root-device personality: publish the OS/SystemOS windows, authentication callback and root read path |
| `offline-legacy-*` + `offline-mobile-graphics-*` | offline legacy platform/power/display bring-up and the mobile graphics client/load path |
| `sheng-gpu-metal=1 sheng-fb-gpu=1 sheng-backlight=1 sheng-vsync=120` | Adreno 740 Metal, GPU scan-out hand-off, firmware backlight and the 120 Hz work-loop timer |
| `sheng-pmgr=2` | publish the original PMGR nub (mode 2) |
| `sheng-log=1` | write the boot/UFS log to the 32 MiB window reserved at the end of userdata |
| `sheng-data-persist=2` | enable the read/write persistent Data-store window (`NativeRamBlock` overlay) |
| `armcpu-spi-ipi=1` | start secondary CPUs using software-SPI IPIs |
| `sheng-disable-watchdogd=1 sheng-disable-jobs=... sheng-lite=1` | disable watchdogd and noisy/network-only daemons so launchd and SpringBoard can make progress |
| `qcom-usb-debug=1` | enable the fuxi USB debug adapter (`FuxiUsbCompat`) on top of `QcomXnuUsbDebug` |
| `-no_slto_panic` | avoid spinlock-timeout panics during early bring-up |

## Board values

| Item | Value | Source |
| --- | --- | --- |
| SoC | SM8550, Xiaomi 13 (`fuxi`) | `board.toml` |
| Target OS | iPad7,1 iPadOS 17.7.11, build 21H461 | `board.toml` |
| Defconfig | `fuxi_full_defconfig` | `board.toml` |
| CPU registers | `0,0x100,0x200,0x300,0x400,0x500,0x600,0x700` | `board.toml` |
| Root volume UUID | `04769D26-2CA0-4825-B443-E2E5F88300F2` | `board.toml` |
| UFS windows | OS + SystemOS from GPT `userdata` on LUN 0 | `board.toml` |
| UFS host controller | `0x1d84000`, apps SMMU `0x15000000`, stream id `0x60` | `BUILD-PROGRESS.md`, stock DTB |
| Chip id | `0x207` | `board.toml` |
| Platform seed | `unique-chip-id` is required and never committed | `board.toml` |
| Payload volume | `FUXIESP`, 176 MiB | `board.toml` |
| Owned memory | `0x130000000` | `board.toml` |
| Firmware | `Platforms/Xiaomi/fuxiPkg/fuxi.dsc`, config `fuxi` | `board.toml` |
| Boot image | header v3, packed by `uefi-fuxi/scripts/bootimage.py` | `BUILD-PROGRESS.md` |

## Scripts

| Script | Purpose |
| --- | --- |
| `scripts/make-fuxi-adt.py` | Apple-format device tree for the first boot |
| `scripts/build-fuxi-verified-root-policy.py` | signed OS/SystemOS policy bundle for VerifiedRootProbe |
| `scripts/build-arm-driver.py` | compiles a generic driver against the XNU compile records |
| `scripts/prepare-fuxi-gmu.py` + `prepare-fuxi-gmu-votes.py` + `prepare-fuxi-command-db-oracle.py` | prepare the Adreno GMU, votes and command-db inputs for NativeAdreno |
| `scripts/build-native-legacy-display.py` | build the `NativeLegacyDisplay` archive |
| `scripts/build-fuxi-usb-debug.py` | build the `QcomXnuUsbDebug` archive |
| `scripts/read-sheng-log.py` | decode the UFS log window pulled from userdata |

## Device support

The original iPadOS userland runs from the IPSW; the home screen and Settings are reached. This table records user-facing device support.

| Feature | State | Notes |
| --- | --- | --- |
| UEFI | ⚠️ | Mu-Silicium boot manager on the firmware framebuffer; no general UEFI storage or input stack |
| Touch | ✅ | Novatek NT36532E |
| Display | ⚠️ | Firmware GOP framebuffer handed off to `NativeLegacyDisplay`; the panel is not re-initialised and refresh is a work-loop timer |
| UFS | ⚠️ | XNU `NativeUfs` root read path and OS/SystemOS windows are verified; UEFI side has no general UFS access |
| Wi-Fi | ❌ | |
| Bluetooth | ❌ | |
| Camera | ❌ | |
| GPU | ⚠️ | Adreno 740 Metal through `NativeAdreno`; compositing works, but there is no panel-vblank hand-off or DCVS |
| USB | ⚠️ | fuxi `QcomXnuUsbDebug` debug backend; no general USB host/peripheral stack |
| Audio | ❌ | |
| Sensors | ❌ | |
| Battery driver | ❌ | |
| MiPPS | ❌ | |
| Suspend and resume | ❌ | |

✅ available, ⚠️ partial, ❌ not available.

## License

BSD-2-Clause-Patent. See [LICENSE](LICENSE).
