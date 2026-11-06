# UniXNU patches kept outside the fork

UniXNU is used as a fork of its upstream, and the fuxi work does not belong in it: every change this
board needs on the driver side lives here instead, so the checkout can be updated from upstream
without carrying a merge.

These four patches come from the commits that were sitting on the fork's `main` tip above
`f28542d` ("Integrate successful fuxi Linux USB device build and compiler compatibility").
They are ordinary `git format-patch` output and apply in file-name order onto that commit.

| Patch | Why it exists |
| --- | --- |
| 0001 xnu: run QcomXnuUsbDebug from the kernel recipe | Applies the module's darwin23 profile after the board series, hands `qcom_usb_debug_init()` the console at the boot-progress branch, and adds `-DXNU_QCOM_USB_DEBUG=1` plus the staged headers to the six units the transport touches. |
| 0002 xnu: give the panic screen the CDC status colour | Resolves `_native_usb_panic_bg` and passes it as `QUD_BG`; removes the frame-pointer walk that competed with the colour for the same padding budget. |
| 0003 build-arm-driver: surface the compiler output on failure | Prints the head and tail of a failed archive build instead of the exit status alone. |
| 0004 prepare-xnu-source: keep the DriverKit headers locally | Caches the DriverKit headers under `.cache/sources/driverkit-headers` and falls back to curl, because this Python build cannot complete a TLS handshake with GitHub on some networks. |

## Applying

```sh
cd ../UniXNU
git am ../../unixnu-fuxi/patches/manual/*.patch
```

`git am` refuses a tree that already carries them, so a checkout that still has the original commits
(same content, same order) needs no action. Verify with `git log --oneline` before and after.
