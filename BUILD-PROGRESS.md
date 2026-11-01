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

## 十一、union SystemOS / 图形 / USB 后续 bring-up（2026-10-25 ~ 2026-10-27）

按提交顺序补记后续遇到的现象、根因和修复。提交号可在本仓库用 `git log` 对应。

### 11.1 让 owner SystemOS 对 launchd 可用

**现象**

owner SystemOS 挂起来之后，用户态仍找不到 dyld 共享缓存：`DYLD_SHARED_CACHE_DIR`
路径被拼接错、release 的 `/sbin/launchd` 没有被选中，shared region 还要求 cache
必须放在 root 或 cryptex 挂载点，而不是 UUID 选中的 SystemOS 卷。

**修复**（`958dd51 xnu: make owner SystemOS mount usable by launchd`）

- 保留 `DYLD_SHARED_CACHE_DIR` 的尾部 `/`，让 dyld 追加 `arm64` cache 文件名而不是把路径拼坏。
- owner cache mount 生效时选择 release 的 `/sbin/launchd`。
- 允许 shared region 直接使用挂在 `/private/preboot` 的 UUID 选定 SystemOS 卷。
- 在应用其他 xnu patch 前先反向撤掉过期的 shared-region patch，避免和 owner cache carrier hunk 重叠。
- 把 `XNU_OWNER_CACHE_MOUNT` 纳入 `vm_unix` 重建 stamp，避免缓存命中旧对象。

### 11.2 PMGR 与 boot-manifest-hash

**现象**

- 启动早期报 `AMFI: NO PMGR?`，ApplePMGR 没有以 root-device 期望的形态发布。
- launchd 挂载阶段 `/sbin/mount -P 1` 返回 `EX_NOINPUT (66)`，触发 critical-process panic。

**修复**（`5d3a148 fuxi: enable PMGR and provide boot-manifest-hash`）

- 打开 `CONFIG_XNU_SHENG_PMGR=y`，并在 `board.toml` 增加 `sheng-pmgr=2`。
- `scripts/make-fuxi-adt.py` 向 `/chosen` 写入 `boot-manifest-hash`，先让 `mount -P 1`
  有属性可读；后续 owner/verified 链中这一合成属性被真实 `/chosen/text-fixups` 替代。

### 11.3 从 raw UFS 窗口切到 owner-root + verified-root

**现象**

早期 `fuxi_full_defconfig` 只挂 raw UFS 开发窗口，没有 audited owner/verified 链；
同时 DarwinBoot 默认 256 MiB 窗口太小，进入 launchd 后仅剩约 32 MiB，jetsam
不断杀掉 SpringBoard/powerd。

**修复**

- `f627c4c fuxi: enable owner-root and verified-root union SystemOS bring-up`
  - `build-arm-driver.py` 支持 `NativeUfs --verified-root --verified-root-bundle`，
    并让 `NativeVirtioBlock` 用 verified multi-policy bundle，两个块传输共用
    fuxi 固定下来的原始 `IOBlockStorageDevice` bindings。
  - 新增 `scripts/build-fuxi-verified-root-policy.py`，从固定的 21H461 OS/SystemOS
    DMG 生成签名 VerifiedRoot manifest。
  - 更新 owner cache-carrier、root-binding-audit、systemos-mount patch，删除过期
    shared-region patch。
  - `make-fuxi-adt.py` 改为输出 `/chosen/text-fixups`，不再使用合成
    `boot-manifest-hash`；`board.toml` 把 `owned_memory_size` 提到 sheng 的
    `0x130000000`（4.75 GiB）。
- `9b88908 fuxi: switch full defconfig to union SystemOS bring-up`
  - 在 `fuxi_full_defconfig` 打开 NativeVirtioBlock、VerifiedRoot、owner root
    audit/trust、boot manifest、union SystemOS，以及 NativeAdreno、NativeLegacyDisplay、
    boot-progress；并链接对应 archive。
- `f041a17 fuxi: raise kern.maxvnodes in the generated ADT`
  - 在生成 ADT 的 `/defaults` 中写 `kern.maxvnodes=66560`，解决首次启动 mount 和
    文件查找阶段 vnode 池不足导致的停顿。
- `81b4cc5 fuxi: replace legacy-stub with Sheng platform-power archive`
  - 内核 archive 列表从 `build-legacy-stub.py` 换成 `build-sheng-platform-power.py`，
    full union SystemOS boot 需要真正的 Sheng PMGR/power archive，而不是旧的显示 stub。

### 11.4 boot progress、boot log 与 CrashReporter 导出

**现象**

旧的 stage-A storage/probe stub 没有 watchdog policy 和 boot-progress 后端；
启动早期也拿不到 crash 文件，只能依赖 panic 屏或外部抓 UFS。

**修复**

- `10ad4d8 fuxi: rebuild boot-progress archive from Sheng sources`
  - `build-boot-progress.py` 改从 `BootWatchdogPolicy`、`BootProgress`、
    `BootFaultProbe` 编译 `libBootProgress.a`。
  - 新增 `package/BootProgressShim.c`，boot-progress 关闭时保持 no-op，并在
    IOKit / NativeUfs 就绪后重试 `boot_log_tick()`。
  - `zzz-boot-progress.patch` 扩展 SystemOS mount panic，输出 owner-cache
    stage/device/authorization 诊断；`board.toml` 增加 `boot-progress=2`。
- `77b1e05 fuxi: persist CrashReporter snapshots in the UFS boot log`
  - SHLOG001 窗口升级到 version 4。
  - `BootCrashExport` 遍历 `/private/var/mobile/Library/Logs/CrashReporter`，
    把最新 `.ips` 交给 boot-log writer，作为 `SHCRASH01` 保存。
  - `BootLogStubs` 弱化 Adreno dashboard 符号，`BootLogTouchStats` 提供 touch
    计数器，使 boot-log archive 在完整 GPU archive 之前也能链接。
  - `build-boot-log.py` 编译 CrashExport 单元；`board.toml` 打开 `sheng-log=1`。

### 11.5 legacy display 与 Adreno GMU/command-db

**现象**

之前的显示路径还是 no-op stub；NativeAdreno 的 GMU include 指向 sheng 输入，
fuxi 的 GMU 寄存器、clock/vote 和 command-db fixture 没有真实来源。

**修复**

- `aca19ab fuxi: bring up the legacy display and vsync/refresh path`
  - `LegacyDisplayProbe` 记录 6 个 vsync 事件并写进 prepare report。
  - `LegacyDisplayStub` 增加 `native_gop_handoff_call/memory` no-op，让内核在完整
    presenter archive 之前也能链接。
  - `build-native-legacy-display.py` 绑定原始 vsync sender、slot 301 refresh
    attributes 和 `setDisplayRefreshProperties`。
  - `NativeHidInput` 先用 link stub 构建，等 v9 IOHIDFamily prelink candidate 就绪。
  - ADT 的 artwork display gamut 改为 sRGB；board/defconfig 打开 legacy display
    archive 与相关 boot-args。
- `db0d411 fuxi: generate pinned Adreno GMU and command-db inputs`
  - 新增 `prepare-fuxi-gmu.py`、`prepare-fuxi-gmu-votes.py`、
    `prepare-fuxi-command-db-oracle.py`，从 `fuxi.dtb` 和固定 Linux/firmware 源
    生成 fuxi 的 GMU/clock/vote 和 command-db fixture。
  - `fuxi_adreno_common.py` 统一 DT 解析、来源锁定和缓存读取；
    `fuxi-adreno-inputs.json` 固定固件 blob 与校验和。
  - `build-native-adreno-kits.py` 嵌入 fuxi GMU/SQE/ZAP 固件；
    `build-arm-driver.py` 把 NativeAdreno 指到 `build/fuxi-gmu`。

### 11.6 QcomXnuUsbDebug CDC-ACM

**现象**

没有串口时，早期 XNU 日志只能靠 UFS log 窗口或 panic 屏，交互和实时采集很受限。

**修复**

- `354e4e7 fuxi: add QcomXnuUsbDebug CDC-ACM transport`
  - 将 `drivers/QcomXnuUsbDebug` 作为 submodule 引入，并新增设备适配层
    `package/FuxiUsbCompat.c`。
  - `zzzd-fuxi-usb-debug.patch` 把 kprintf/serial 输出镜像到 USB writer。
  - `build-fuxi-usb-debug.py` 用真实 XNU compile record 编译 DWC3/SMMU/CDC 源；
    `board.toml` 打开 archive 和 `qcom-usb-debug=1`。
- `e620bbb fuxi: update QcomXnuUsbDebug submodule`
  - 将 submodule 固定到 `a5f7629`：在 `acquire()` 前映射 GCC/DWC3/SMMU IO，
    并增加 USB 初始化/错误诊断。

### 11.7 首启动减负与 persistent Data

**现象**

全新 Data 首次启动时，大量只会访问网络/iCloud/相册的 launchd job 和 SpringBoard
抢 CPU 与内存；Data 写入默认不持久，`datamigrator` 和系统 app staging 不完整。

**修复**（`8200896 fuxi: add sheng-lite policy and persistent Data seed`）

- `BootWatchdogPolicy.c` 通过 scratch page 分块写 `sheng-lite` disabled-job 列表。
- 新增 `scripts/seed-data-skip-setup.py`，把 SetupDone/mini-buddy preferences 和
  container 目录直接烘进 Data seed。
- `make-fuxi-adt.py` 增加 `--persistent-data`，让 datamigrator 和系统 app staging
  在 persistent 路径运行；`board.toml` 打开 `sheng-lite=1`、`sheng-data-persist=2`
  和 `persistent_data=true`。

## 十二、GPU fence 停摆诊断（2026-11-01）

### 现象

union SystemOS 起来后，SpringBoard / backboardd 渲染到第 17/18 帧停摆：

- `[fence wait]` 显示 `submitted=18 completed=17`（最后一次超时快照），`timeouts` 持续累积。
- `[scanout wait]` 每次 500ms 超时，显示没有新帧合成。
- `[clocks] gpu perf` 从 7（615 MHz）落回 3（348 MHz）——GPU 空闲。

早期日志没有 `[stall]` 行，无法直接看 CP 停在哪，诊断走了弯路。

### 诊断手段（三个改动，均只加日志/对齐 sheng，不动执行逻辑）

1. `unixnu-fuxi/package/BootLogStubs.c` 补 `native_adreno_stall[29]` /
   `native_adreno_stall_mem[59]` 定义。此前 BootLog 用 `weak_import` 引用、符号缺失时
   `&sym == NULL`，`[stall]` 整段被跳过；补上后 Adreno740 已有的 stall 快照能打出来。
2. `UniXNU/drivers/Adreno740/NativeAdrenoSubmit.cpp` 加 fence 事件环形日志
   `native_adreno_fence_events[32][4]`（type 1 submit / 2 wait-enter / 3 wait-complete /
   4 wait-timeout），`BootLog.c` 打印最近 16 条。
3. `GpuPowerProbe.cpp` 对齐 sheng 的两处移植遗漏（不是停摆根因，但补齐一致性）：
   - DCVS wake boost（idle→active 50ms 内拉到 ≥578 MHz）+ touch boost（触屏后顶档
     `sheng-gpu-dcvs-boost-ms`）。fuxi 之前只有基础 busy 升降频，GPU 长期停在 348 MHz 地板。
   - `sheng-gpu-pause-us`（fence waiter 的睡眠时长，默认 1000us；fuxi 之前硬编码 100us）。

### 关键结论：fence 全部完成，backboardd 之后停止提交

fence 事件日志（`fuxi-20190100-000035 (4).log`）给出确定性证据：

```text
91  submit f=17 sub=17 cmp=16
93  wait-timeout f=17        ← 第 1 次 poll 超时
95  wait-timeout f=17        ← 第 2 次 poll 超时
97  wait-complete f=17 cmp=17   ← fence 17 完成
98  submit f=18 sub=18 cmp=17   ← 提交下一帧
100/102/104 wait-timeout f=18   ← 3 次 poll 超时
106 wait-complete f=18 cmp=18   ← fence 18 完成
（之后无 fence 19，事件计数冻结在 106）
```

- fence 17、18 都正常完成（`wait-complete`，`cmp` 递增），不是 GPU hang。
- 每个 fence 都是「poll 超时几次 → 最终 complete」的正常模式，WaitFence 返回成功。
- 有 fence 18（说明 fence 17 完成后渲染循环还在跑），但没有 fence 19——backboardd
  在 fence 18 完成后停止提交。

符号化 backboardd 的等待通道 `0xfffffff00b8ba2ac`：

```text
_ipc_mqueue_receive_continue
```

backboardd 26 个线程全部阻塞在 `ipc_mqueue_receive`（Mach 消息接收），在等下一帧渲染消息；
SpringBoard 一个线程忙等（`sw/s` 高）。

### 排除的假设

- SMMU 缺页/ACD 值错误：`[stall vm] fault=00000400` 无 fault 位；`enable_acd=0`（ACD 关闭）。
- GPU 频率低：DCVS boost 补齐后 `gpu perf` 已能到 7/8，但停摆不变。
- fence 超时预算固定 2ms：metal2vulkan 的 `WAIT_SLICE=100ms`，`longest=2040us` 是成功等待耗时，
  不是超时预算。
- 大 IB（256KB）异常：fence 17 的 256KB IB 是 SpringBoard 完整渲染帧，在 615 MHz 下 ~2ms 完成。

### 根因定位

GPU 驱动和 fence 同步路径正常。主链是 iOS 用户态停摆：

```text
fence 18 完成 → backboardd 进入 ipc_mqueue_receive 等 mach 消息
→ 下一帧渲染消息没来 → backboardd 不再提交 → GPU 空闲
→ scanout 500ms 超时 → SpringBoard 忙等
```

这是 CoreAnimation → backboardd 消息循环的问题，不是 UniXNU 内核/GPU 驱动的问题。

### 用户态符号化（000823，停摆 150 秒）

从 IPSW 21H461 的 SystemOS 提取 `dyld_shared_cache_arm64` + `.symbols`，对停摆栈顶
`1e4174b58` 做符号化。slide 取日志 `cache+189f4000`：

- `1e4174b58`（backboardd 与 SpringBoard 停摆后共同栈顶 pc）
  = `libsystem_kernel.dylib` + `0x1b58`，是 `mach_msg` 接收等待循环内部；
  内核侧 continuation = `ipc_mqueue_receive_continue`（即 PROC 行 `Cfffffff00b8ba2ac`）。
- `_mach_msg_trap` / `_mach_msg2_trap` / `_mach_msg_overwrite_trap` 落在 `/usr/lib/dyld`
  （dyld 的 trap stub），真正阻塞点在 `libsystem_kernel` 的接收路径。

进程 peer（`lookup_peer` 用 `ipc_port_get_receiver_task_locked` 反查）：

- SpringBoard 主线程停摆时 `peer=154`，日志 `chronod(154)` 确认是 `chronod`
  （ChronoCore，小组件/时间线服务）。
- backboardd 主线程停摆时 `peer=-1`（挂在 port set，无单一 receiver）。

`lookup_peer` 的语义：阻塞在 mach_msg 的线程，其上下文里一个 send right 指向的
receiver task。所以 `peer=154` 表示 SpringBoard 阻塞的这次 mach_msg 关联到一个
由 chronod 接收的端口，不是 SpringBoard 在等 chronod 发消息。

### 排除实验（受控单变量）

按「每次只改一个变量」的顺序跑，`sheng-disable-jobs` 是现成机制，只改 board.toml：

| 轮 | 变量 | 结果 |
| --- | --- | --- |
| 000027 (9) | 禁 chronod | 停摆依旧：swap 停 W3、fence 停 17，peer 变成别的 pid |
| — | 禁 liveactivitiesd | 成功，渲染恢复 |

- 禁 chronod 无效，且 SpringBoard 的 peer 从 chronod 变成别的 pid → 等 chronod 是
  停摆后的连锁表现，不是原因。
- 禁 liveactivitiesd 后渲染恢复。liveactivitiesd 在停摆前约 5 秒（56.3s vs swap 停 61.2s）
  反复 sig5 崩溃。

### liveactivitiesd 崩溃符号化

`liveactivitiesd sig5 PC1cbdce4f8 slide a4d8000`：

```text
unslid = 0x1c18f64f8 = SessionSyncEngine + 0x1d4f8
          = ___swift_memcpy17_8 (0x1c18f4f34) ~ _block_copy_helper (0x1c18f68e0) 之间
```

`sig5 = SIGTRAP`，落在 SessionSyncEngine（小组件时间线同步引擎）的 Swift 栈拷贝/析构
路径里，不是野指针。

### 根因

liveactivitiesd（SessionCore）在停摆前约 5 秒崩溃，SessionSyncEngine 的 Swift 运行时
trap 触发 SIGTRAP；它崩溃/重启循环卡住渲染服务（backboardd 停提交、scanout 500ms 超时）。
禁用 liveactivitiesd 后渲染循环恢复。UniXNU 内核、Adreno740、GopFramebuffer、DPU、SMMU
都不是根因。

### 待办

- [ ] 查 SessionSyncEngine + 0x1d4f8 的具体 trap 原因（Swift 越界/未实现路径），确认是
      数据迁移（persistent Data seed）还是 SessionCore 依赖缺了某个 XPC 服务。
