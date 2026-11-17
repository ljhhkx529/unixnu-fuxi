# UniXNU patches kept outside the fork

UniXNU is used as a fork of its upstream, and the fuxi work does not belong in it: every change this
board needs on the driver side lives here instead, so the checkout can be updated from upstream
without carrying a merge.

These patches are ordinary `git format-patch` output and apply in file-name order onto the
upstream `main` tip the fuxi work started from (`04573c5`). Patches 0001 to 0015 carry the UniXNU
tree; patch 0016 carries one component and is applied to that component's own checkout, not to the
UniXNU tree.

| Patch | Why it exists |
| --- | --- |
| 0001 components: add FuxiFtsTouch to the catalog | Registers the board's own touch driver as a component so the catalog can fetch and build it like the rest. |
| 0002 bootimg: text offset and payload-in-image options | Adds the two options the board's boot image needs: where the text goes, and whether the payload is carried inside the image. |
| 0003 probe: a yellow band that shows how far the boot got | Paints a yellow band at the entry probe so a stalled boot says how far it came instead of showing a blank screen. |
| 0004 seed-data-grow: match the APFS container by ContainerReference | Grows the data volume against the container the kernel was built for, matched by its reference rather than by name. |
| 0005 build: the Linux host recipe for the fuxi kernel | Makes a Linux host build the kernel as well; the kernel used to be built only on a Mac. Authored by Kerfand. |
| 0006 xnu: run QcomXnuUsbDebug from the kernel recipe | Applies the USB debug module's darwin23 profile after the board series, hands `qcom_usb_debug_init()` the console at the boot-progress branch, and adds `-DXNU_QCOM_USB_DEBUG=1` plus the staged headers to the six units the transport touches. |
| 0007 xnu: give the panic screen the CDC status colour | Resolves `_native_usb_panic_bg` and passes it as `QUD_BG`; removes the frame-pointer walk that competed with the colour for the same padding budget. |
| 0008 build: keep the compiler output and the DriverKit headers on the machine | Prints the head and tail of a failed archive build instead of the exit status alone, and caches the DriverKit headers locally with a curl fallback, because the build cannot complete a TLS handshake with GitHub on some networks. |
| 0009 build-adreno-metal-plugin: accept the arm64 build of the plugin source | Takes the arm64 build of the Metal plugin source instead of failing on the architecture. |
| 0010 components: AdrenoMetal and GopFramebuffer back to official | Puts both components back on their official commits; the bring-up forks were diagnostic only and are not a functional dependency. |
| 0011 Adreno740: board policy as data, and an audit-only command stream | Moves the board's GPU policy into the catalog's data files and leaves the command stream audit-only. |
| 0012 mesa: Turnip honours the kernel's command-stream capabilities | Carries the Turnip command-policy patch and the checker that keeps the Mesa build against the kernel's stated capabilities. |
| 0013 scripts: build the updated components against this machine | Points the component build scripts at the components this board now builds, and updates the recorded compiler inputs. |
| 0014 payload: refuse a Data seed whose volume UUID is not the one the kernel was built for | Checks the seed volume's UUID against the kernel's, so a mismatched data seed fails at build time instead of on the device. |
| 0015 payload: the kernel command line may use the whole DarwinBoot buffer | Raises the command-line limit to the size of the DarwinBoot buffer; the board's kernel arguments are longer than the old 1000-byte cap. |
| 0016 gopframebuffer: keep the fuxi panel black on request | Adds the `fuxi-panel-black` boot argument to the display component: the firmware splash is overwritten with black at start and every later swap is painted as the blank frame, so an unattended boot leaves an OLED panel dark instead of showing the splash. |

## Applying

Patches 0001 to 0015 apply to the UniXNU tree:

```sh
cd ../UniXNU
git am ../../unixnu-fuxi/patches/manual/0*.patch
```

`git am` refuses a tree that already carries them, so a checkout that still has the original commits
(same content, same order) needs no action. Verify with `git log --oneline` before and after.

Patch 0016 applies to the GopFramebuffer component's own checkout:

```sh
cd ../UniXNU/drivers/GopFramebuffer
git am ../../../../unixnu-fuxi/patches/manual/0016-*.patch
```
