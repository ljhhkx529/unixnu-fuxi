# fuxi 构建进度 & UFS 验证经验

本文记录 fuxi（Xiaomi 13 / SM8550）UFS 存储链路的验证过程、关键事实和踩坑经验。

## 一、结论（2026-10-25）

UFS 全链路已在真机验证通过：UFS 传输、LUN0 GPT、SHNGROOT 窗口、APFS 容器/卷解析、
按 UUID 命中 root / SystemOS / Data 三个卷全部正确。

`probe=12` 真机输出：

```text
NATIVE_UFS media report: published=2 reads=829 failures=0 media=6
```

- `published=2`：SystemOS + OS 两个 SHNGROOT 窗口成功发布。
- `reads=829 failures=0`：829 次块读取零失败，UFS 传输稳定。
- `media=6`：IOStorageFamily 枚举到 6 个 IOMedia。

三个关键卷 UUID 全部命中配置：

| IOMedia | 内容 | UUID | 配置项 |
| --- | --- | --- | --- |
| `disk5s1` | OS 根卷 | `04769D26-2CA0-4825-B443-E2E5F88300F2` | `[payload.adt] root_match_volume_uuid` |
| `disk3s1` | SystemOS / Cache | `8846FE78-2910-4297-AF97-D59BB6F8FC16` | `CONFIG_XNU_OWNER_CACHE_VOLUME_UUID` |
| `disk1s1` | Data | `3E81D4A6-8D16-496B-B890-E680C6E6997C` | `CONFIG_XNU_OWNER_DATA_VOLUME_UUID` |

GUID 对照：`EF57347C-...` = APFS 容器，`41504653-...` = APFS 卷，
`7C3457EF-...` = Apple_APFS 分区，`GUID_partition_scheme` = OS dmg 内的 GPT。

## 二、关键硬件事实（来自 fuxi.dtb 与真机 GPT）

| 项 | 值 | 来源 |
| --- | --- | --- |
| UFS 主机控制器 | `0x1d84000` / `0x3000` | `drivers/NativeUfs/GccUfs.h`、fuxi.dtb |
| GCC（时钟） | `0x100000` | `GccUfs.h` |
| apps_smmu | `0x15000000` | `NativeUfs.cpp` |
| UFS stream id | `0x60` | fuxi.dtb `iommus = <&apps_smmu 0x60>` |
| compatible | `qcom,sm8550-ufshc` | `scripts/make-fuxi-adt.py` |
| userdata 真实 GPT | `first=0x2f7840 last=0x3afc3fa` | `probe=10` 真机 |
| unique-chip-id | `0xce854c9ae043320b` | 从旧 payload 的 ADT 提取 |
| chip-id | `0x207` | 同上 |
| root 卷 UUID | `04769D26-2CA0-4825-B443-E2E5F88300F2` | `probe=12` 真机 |

### 已知几何错误（待修）

- `board.toml [userdata] last_lba = 0x76923FA` 是错的，真机为 `0x3afc3fa`。
  - 运行时驱动读设备真实 GPT（`findRootLayout`），所以不影响本次启动。
- `scripts/build-userdata-root-image.py` 仍硬编码 sheng 几何
  `USERDATA_BYTES = (0x3d0abfa - 0x2f3840 + 1) * 4096`。
  - 只用于“镜像是否超过分区”的尺寸校验，当前已刷镜像有效，但重新生成 userdata 前必须改成 fuxi 的 `0x2f7840..0x3afc3fa`。

## 三、无串口时如何验证 UFS（核心经验）

fuxi 没有可用的真串口（`/dev/cu.debug-console` 是旧节点，抓取 0 字节），所以用驱动内置的
`native-ufs-probe=N` 探测档位 + `panic_screen` 把结果直接画到 framebuffer。

各档位（见 `drivers/NativeUfs/NativeUfs.cpp` 的 `NativeUfs::start`）：

| probe | 作用 | 结果形式 |
| --- | --- | --- |
| `2` | 只读 GCC/UFS 寄存器 | panic 屏 |
| `7` | 只读 SMMU stream `0x60` 映射 | panic 屏 |
| `10` | LUN0 GPT → userdata → 首块 + manifest | panic 屏 |
| `11` | 逐窗口读扇区 1 的 `EFI PART` 签名 | panic 屏 |
| `12` | 完整 `startRoot()` + media 报告 | panic 屏（成功即闭环） |
| `13/14` | 内存快照 | panic 屏 |

判断标准：

- `probe=10`：`manifest status=0 windows=2` 且首块 magic 为 `SHNGROOT`
  （`474e4853 544f4f52`）→ UFS 能读盘、root 镜像已刷入。
- `probe=11`：`w0/w1 read=1`，OS 窗口 `gpt=45464920 50415254`（`EFI PART`）；
  SystemOS 窗口 `gpt=00000000` 是正常的（cryptex 是裸 APFS 卷，无 GPT）。
- `probe=12`：`published=2 failures=0` 且三个卷 UUID 命中 → UFS 完全闭环。

注入 probe 参数用 `--kernel-arg`：

```sh
./unixnu build payload --uefi ../uefi-fuxi \
  --unique-chip-id 0xce854c9ae043320b \
  --trust-cache <trustcache.bundle> \
  --data-ramdisk <scratch-data.img> \
  --kernel-arg native-ufs-probe=12
```

### panic_screen 的边界

- 早于 framebuffer 被回收的 panic（probe 模式）能干净渲染。
- 内核跑到后面把 framebuffer 当普通内存回收后，panic 屏无法渲染 → 花屏，真实错误被盖住。

## 四、花屏卡死 / 重启的解释

`fuxi_full_defconfig` 中 `CONFIG_XNU_NATIVE_LEGACY_DISPLAY=n`、
`CONFIG_XNU_NATIVE_ADRENO=n`，即没有显示驱动。内核启动后期会回收 framebuffer，
屏幕花屏；此时真正的 panic 也画不出来。因此花屏不是 UFS 的问题，而是
「UFS 之后（挂载 / launchd / 显示 / 内存）阶段」的问题，需要后期内核日志才能定位。

两条后续路线：

1. 移植 sheng 的显示驱动（`NativeLegacyDisplay` / `NativeAdreno`），让屏幕可显示。
2. 打通真串口或 USB diag 口。

## 五、从旧 payload 提取 platform seed（无 ADB 时）

`unique_chip_id = "required"` 不落盘，无 ADB 时可以从已构建的 `payload.img` 还原：

```sh
MTOOLS_SKIP_CHECK=1 mcopy -i out/fuxi/payload/payload.img \
  ::/EFI/UNIXNU/PAYLOADS/KERNEL~1.ADT kernel.macho.adt
```

该 ADT 是自定义二进制格式（非标准 DTB），用 Python 解析 `/chosen` 的
`unique-chip-id`（8 字节）和 `chip-id`（4 字节）即可。

## 六、完整构建命令（存储链版本）

```sh
cd UniXNU
./unixnu defconfig fuxi_full_defconfig -D ../unixnu-fuxi
./unixnu fetch
./unixnu build kernel
./unixnu build package
./unixnu build firmware --uefi ../uefi-fuxi
./unixnu build payload --uefi ../uefi-fuxi \
  --unique-chip-id 0xce854c9ae043320b \
  --trust-cache <trustcache.bundle> \
  --data-ramdisk <scratch-data.img>
python3 ../uefi-fuxi/scripts/bootimage.py \
  --ramdisk out/fuxi/payload/payload.img -o out/fuxi/boot/boot.img
```

注意：

- 打包 boot.img 用 `uefi-fuxi/scripts/bootimage.py`（header v3），不要用旧
  `unixnu bootimg`（会在 `splash_region` 上 `FDT_ERR_EXISTS`）。
- fuxi ABL 不加载大 ramdisk，payload 是追加到 gzip kernel 字段后一起打包的。

## 七、当前产物

| 文件 | 说明 |
| --- | --- |
| `UniXNU/out/fuxi/kernel/kernel.macho` | 含 NativeUfs/NativeRamBlock，sha256 `32e81abe…` |
| `UniXNU/out/fuxi/payload/payload.img` | 176 MiB，含新内核 |
| `UniXNU/out/fuxi/boot/boot.img` | 默认挂载版（`native-fuxi-ufs-root=1`） |
| `UniXNU/out/fuxi/boot/boot-probe12.img` | UFS 闭环验证版 |

## 八、待办

- [ ] 修正 `board.toml` 的 `last_lba`（`0x76923FA` → `0x3afc3fa`）。
- [ ] 修正 `build-userdata-root-image.py` 的 sheng 几何硬编码。
- [ ] 决定显示路线：移植 sheng `NativeLegacyDisplay` / `NativeAdreno`，或打通串口。
- [ ] 定位默认挂载版花屏卡死的后期原因（需要内核日志）。

## 九、从 UFS 取日志（标准流程）

### 写端（已完成）

内核在 `osfmk/kern/startup.c` 的 `boot_progress_start()` 之后调用 `boot_log_tick(0)`，
`BootLog.c` 只有在 boot-arg `sheng-log=1` 时才启动写线程。写线程等 NativeUfs 读到
root layout 后，把日志写到 **userdata 分区最后 8192 个 4K 块（32 MiB）**：

```text
kLogReserveBlocks = 8192
log_first = userdata_last - 8192 + 1 = 0x3afc3fa - 0x1fff = 0x3afa3fb
```

已确认带 `sheng-log=1` 的 payload 内核 `kernel.macho` 含 `boot_log_tick` / `native_ufs_log_open`
符号，`boot.img` 已按 header v3 打包（见第六节）。

### 读端 A：Android dd（立即可用）

UniXNU 跑完一轮后回 Android（有 root），把 userdata 尾部 32 MiB 拉出来解码：

```sh
SKIP=58731451   # 0x3afa3fb - 0x2f7840，相对 userdata 分区头
adb shell "su -c 'dd if=/dev/block/by-name/userdata of=/data/local/tmp/fuxi-log.bin bs=4096 skip=$SKIP count=8192'"
adb pull /data/local/tmp/fuxi-log.bin
python3 scripts/read-sheng-log.py --decode fuxi-log.bin --out fuxi-logs
```

或直接跑 `outputs/extract-fuxi-log-android.sh`（本机 Codex outputs 目录）。

注意：Android 挂载 userdata 时可能 fsck 尾部，所以从 UniXNU 回到 Android 后尽快 dd，
不要在 Android 里做大量写盘操作。

### 读端 B：Linux reader（标准，缺 fuxi mainline DTB）

sheng 的标准读法是用 `package/linux-log/`：mainline 内核 + initramfs（busybox/init/rebootbl/
`nvmem-reboot-mode.ko`），只读不挂载，通过 USB CDC ACM 把 32 MiB 窗口吐给
`read-sheng-log.py --port ...`。fuxi 目前缺一块板级输入：

- `ianchb/sm8550-mainline` v7.3.0 的资产只有 `sm8550-xiaomi-sheng.dtb`，没有
  `sm8550-xiaomi-fuxi.dtb`；同一 fork 源码分支也没有 fuxi 的 DTS。
- 因此 fuxi 的 Linux reader 需要先拿到/编译 `sm8550-xiaomi-fuxi.dtb`（匹配该 7.3.0 内核），
  再把 mainline `Image.gz` + fuxi DTB 拼成 header-v0 boot 镜像喂给
  `build-linux-log-boot.py --boot ... --module nvmem-reboot-mode.ko`。

在此之前，fuxi 用读端 A（Android dd）验证写链路；读端 B 仅差 fuxi DTB 这一块输入。

## 十、union SystemOS 启动根因（SpringBoard 崩溃循环）

### 结论

最新一轮日志（`fuxi-20190100-000033 (2).log`）已越过 UFS 阶段：union SystemOS / OwnerData
挂载成功，`load_init_program` 拉起 `/sbin/launchd`，用户态大量起来，但 **SpringBoard 每约 3 秒
被拉起一次立即崩溃，触发 `Too many corpses being created`，始终到不了主屏**。

这是「dyld 共享缓存 slide 未修正」导致的用户态崩溃循环，不是 UFS 问题。

### 两条日志要分开看

`shared_region: [1(launchd)] check_np(...) vm_shared_region_start_address() failed` 只在 launchd
早期出现 2 次，是 **良性握手**，不是致命点：

- dyld 每次进程启动先调 `shared_region_check_np()`（`bsd/vm/vm_unix.c:2093`）问「共享区是否已映射」。
- 共享区还是空时，`vm_shared_region_start_address()`（`osfmk/vm/vm_shared_region.c:999`）发现
  `sr_first_mapping == -1`，返回 `KERN_INVALID_ADDRESS`，内核打印该行并回 `ENOMEM`。
- dyld 收到 `ENOMEM` 后自己去映射 dyld 共享缓存。**这一行 sheng 也必然出现，无需修。**

真正致命的是后续的 SpringBoard 崩溃循环：

```text
SpringBoard[107] Corpse failure, too many 6
Process[107] crashed: SpringBoard. Too many corpses being created.
```

### 根因：dyld 共享缓存 slide 未生效

日志反复出现 `unnest ... DYLD shared region 0x1fdc00000-0x203000000`（非标准基址，slide 偏约
1.98GB），并且 `[payload] text_fixups` 里 3 条 `dyld_shared_cache_arm64.*` fixup **一条都没 applied**，
只有 `BTAudioHALPlugin` 那条 applied：

```text
text-fixup: BTAudioHALPlugin+0x38bec (8 bytes) applied
```

`cs_text_fixup_page()`（`UniXNU/patches/xnu/native-text-fixup.patch:151`）只挂在
`vm_page_validate_cs_mapped_slow()`（同 patch 的 `osfmk/vm/vm_fault.c` 段）。dyld 共享缓存走的是
shared-region 专用映射路径，不经过这个 UBC 校验点，所以针对 `dyld_shared_cache_arm64.*` 的 fixup
永远打不上，slide 保持错误，dyld/SpringBoard 反复 crash → corpse loop。

### sheng 怎么处理（`no ignition` 模式）

sheng 不靠 `[payload] text_fixups` 去补 dyld cache（sheng 的 `board.toml` 没有这些条目），而是让
dyld 直接跳过 shared-region 点火，把镜像认证交给已验证的只读 owner 挂载：

- `CONFIG_XNU_OWNER_CACHE_MOUNT=y` + `CONFIG_XNU_OWNER_UNION_SYSTEMOS=y` 把 SystemOS cryptex
  union 挂到 `/`（`UniXNU/patches/xnu/offline-owner-systemos-mount.patch`）。
- owner cache 挂载成功后，`native_owner_cache_prepare()` 置 dyld 标志 bit 3
  （`dyld_flags |= UINT64_C(8)`，`offline-owner-systemos-mount.patch:168`）：

  ```c
  /* Original dyld bit 3 selects no ignition; the owner chain and actual
   * readonly mounts above supply image authentication in this mode. */
  dyld_flags |= UINT64_C(8);
  commpage_update_dyld_flags(dyld_flags);
  ```

- 于是 dyld 进入 `no ignition`：`check_np` 看到空共享区（正是那两行日志），**不再走
  `shared_region_map_and_slide_2_np`**，改用私有映射；认证责任转移到只读 owner 链。
- `UniXNU/patches/xnu/native-4k-user-abi.patch:66` 同时把 shared-region 子映射改为 4K page shift，
  与 4K 用户 ABI / dyld cache 对齐。

fuxi 已经继承 `OWNER_CACHE_MOUNT/STARTUP/UNION_SYSTEMOS=y` 和 `native-4k-user-abi.patch`，所以那两条
`check_np` 日志对 fuxi 同样是良性的；fuxi 的差异是额外带了 3 条 dyld `text_fixups`，而这些 fixup 打不进
shared-region 路径，等于白带，slide 依旧错误。

### 已实施的修复（2026-10-26）

对齐 sheng 的 slide 来源，而不是继续依赖 dyld `text_fixups`：

- 三个检查点与 sheng 对比结果：
  - `[kernel.prelink] slide = "-0x1000000"` → 与 sheng 一致，未改。
  - `owned_memory_size` → fuxi `0x100000000` 改为 sheng 的 `0x130000000`（4.75 GiB，
    fuxi 6 GiB conventional bank 放得下）。
  - DT 内存窗口 `dram-base`/`dram-size` = `0x80000000`/`0x80000000` → 与 sheng 一致，未改。
- 移除 `board.toml [payload] text_fixups` 中 3 条 `dyld_shared_cache_arm64.*`，
  只保留实际会应用的 `BTAudioHALPlugin`。

`owned_memory_size` 只进 ADT/payload，不改变 `kernel.macho`，因此只重建 payload + bootimage：

```sh
./unixnu build payload -D ../unixnu-fuxi --uefi ../uefi-fuxi \
  --unique-chip-id ce854c9ae043320b \
  --owner-trust-cache out/fuxi/payload/original-static-system.bundle \
  --data-ramdisk out/fuxi/build/owner-data-fixture/owner-data-16g.dbr
python3 ../uefi-fuxi/scripts/bootimage.py \
  --ramdisk out/fuxi/payload/payload.img -o out/fuxi/boot/boot.img
```

产物：

| 文件 | sha256 |
| --- | --- |
| `UniXNU/out/fuxi/payload/payload.img` | `e900e2fa013c1221937ca707316c0c6ac69510766de4334076dc14b4171fedbb` |
| `UniXNU/out/fuxi/boot/boot.img` | `f7d3e12cf2eb6786c4149393882bd4e5ef1e26447dc55550330c41fb666dcc65` |

### 待验证

- [ ] 真机刷入新 `boot.img`，确认 launchd 后正常拉起 SpringBoard，无
      `Too many corpses being created` / 高频 `caught waking the CPU`。
- [ ] 若仍出现 corpse loop，再检查 dyld shared cache 的错误 slide 是否还来自
      `owned_memory_size` 之外的因素（如 `fuxiPkg/MemoryMapLib` 的窗口基址 / 大小）。
