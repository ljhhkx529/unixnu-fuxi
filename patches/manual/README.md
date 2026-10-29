# Manual merge patches

These patches are for maintainers who merge the UniXNU working-tree changes by hand.

- `0001-AdvancedLogDxe-ufs-speed-gear.patch` applies to the `UniXNU` main repository.
  ```sh
  cd UniXNU
  git am ../unixnu-fuxi/patches/manual/0001-AdvancedLogDxe-ufs-speed-gear.patch
  ```
- `0002-Adreno740-NativeAdrenoRing-stall-regs.patch` applies to the `UniXNU/drivers/Adreno740` submodule.
  ```sh
  cd UniXNU/drivers/Adreno740
  git am ../../../../unixnu-fuxi/patches/manual/0002-Adreno740-NativeAdrenoRing-stall-regs.patch
  ```
