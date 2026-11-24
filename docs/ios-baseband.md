# 把基带接进 iPadOS（对照 Trogdor-ipadOS）

2026-11-23。参考实现：https://github.com/chiba233/Trogdor-ipadOS（HP Chromebook x2，Coachz/SC7180）。

## 参考实现发布的东西

`AppleBaseband`、`AppleBasebandPCIControl`、每个通道一个 `AppleBasebandPCIInterface`（按 `ABPInterfaceProtocol` 匹配：
pbl、sahara、qmux1、qmux2）、`AppleIPAppender`、`AppleBasebandPCIMAVPDPManager`，加四个 `pdp_ip` 接口。
README 明说：少了 `AppleIPAppender` 和 `AppleBasebandPCIMAVPDPManager`，CommCenter 会崩。

## 我们的状态

- **服务已发布**：`COACHZ baseband: 22 of 22 services published (AppleBaseband, AppleBasebandPCIControl, 18 channels,
  the PDP manager, AppleIPAppender)`。
- 我们的 `coachz_baseband.cpp` 73,827 字节（参考 67,390），是更新的分支。
- `/chosen/coachz-baseband` 由 `board.toml [adt.chosen.u32]` 注入，**已在生效**（否则不会发布）。
- 机型身份 `iPhone15,2` / `D72`（iPhone 14 Pro），**本来就是蜂窝机型**。
- **但没有任何通道被打开**：桥对每个通道打开都打印（`coachz_baseband.cpp:1079`），日志里一次都没有 →
  **userspace 里没有东西在驱动基带 → CommCenter 没有连上来。**

## 参考实现里我们缺的两块

### 1. ABM 的低功耗握手（已移植）

参考的 `bb_lpm_*`（`coachz_baseband.cpp:296-340`）说明：

> ABM 每次启动都"上电"基带，桥替它扮演这次启动（`bb_sahara_restart`），而 modem 一直在跑、保持着原来的模式。
> 刚启动的基带应当是**低功耗**：CommCenter 记 "Baseband booted in airplane mode"，它的订阅开关等
> `/cc/props/radio_state` 到 Offline，而**只有 modem 的低功耗报告才会给**。参考板上 modem 一直停在 CommCenter
> 读作 Shutdown 的模式，开关从不翻转、SIM 一直 unknown、没有服务。所以每次这样的启动之后，桥用自己的 DMS 客户端
> 读模式并设成低功耗（DMS `0x002d`/`0x002e`，TLV `0x01` u8：0 online、1 low power），**然后由 CommCenter 带到 online**。

2026-11-23 已移植进 `UniXNU/drivers/QcomBaseband/baseband/coachz_baseband.cpp`：状态、`bb_lpm_send`/`bb_lpm_answer`、
在 `bb_sahara_restart` 里触发、每次 poll 重试一次、在 DMS 回答分支前先判 txn。`kbuild=0`，实机无中止。
**它只在 sahara 通道被打开后才跑**，所以现在还没触发过。

### 2. 打过补丁的 CommCenter（未做）

参考 README（`:73-78`）:

```sh
python3 tools/coachz_cellular_compat.py --commcenter ORIGINAL_COMM_CENTER --output build/coachz-cellular-compat
# 打包命令中加 --baseband-commcenter build/coachz-cellular-compat/CommCenter
```

> **内核归档携带签名副本，ADT 携带其 SHA-256，信任缓存携带其 cdhash。启动时核对摘要，把副本安装到 Data，
> 再用只读 bindfs 联合挂载到 CoreTelephony Support 目录。**

**原版 CommCenter 认不出这颗基带，必须用打过补丁的副本。** 工具我们树里就有
（`UniXNU/drivers/QcomBaseband/platform/coachz/tools/coachz_cellular_compat.py`，还有 `--esim` 变体），
但**打包流程与内核侧的安装/挂载都没有**。参考的内核侧在 `coachz_prelink_baseband.patch` 的
`coachz_baseband_install()` / `coachz_baseband_point()`（约 200 行），还包括：

- `/private/var/coachz/baseband/Mav16` 固件目录，
- 两个统一日志子系统 plist（`com.apple.telephony.abm`、`.bb`）让 ABM/BBUpdater 的失败原因可见，
- 只读 bindfs 联合挂载到 `/private/preboot/<HASH>/usr/local/standalone/firmware/Baseband/Mav16`。

## 下一步顺序

1. **用现有工具生成兼容副本**：`coachz_cellular_compat.py --commcenter <IPSW 里的 CommCenter>`；
2. **打包**：把副本、SHA-256（ADT）、cdhash（信任缓存）一起装进镜像；
3. **内核安装**：照参考的 `coachz_baseband_install`/`point` 把副本装到 Data 并 bindfs 挂到 CoreTelephony；
4. 之后 sahara 通道会被打开 → 移植好的低功耗握手触发 → CommCenter 把 modem 带到 online。

**判据**：日志出现 `COACHZ baseband: <通道> opened`，随后 `operating mode after the boot: … setting low power`。

## 实机验证（2026-11-23 09:28）

ABM 不在时低功耗握手永远不会触发（它挂在 sahara 通道打开上），所以加了 `sheng-bb-lpm=1` 启动参数，
让桥在**自己第一个 DMS 回答**之后手动跑一次。实机结果：

```
1367: exercising the boot operating-mode handshake (no ABM is driving the baseband)
1370: operating mode after the boot: 0 (error 0x4a), setting low power
1373: operating mode set to low power: error 0x0
```

**Get Operating Mode 读到 mode 0（online），Set Operating Mode 低功耗返回 error 0x0（成功）。**
之后 modem 报 `the radio is offline` —— 正是"刚启动的基带"该有的状态。

### 移植时抓到并修掉的一个真 bug

参考实现里 `bb_lpm_answer` 对"不是我的 txn"返回 **1**（吞掉），因为**它的 DMS 客户端是 lpm 独占的**。
但我们这边的同一个客户端**和无线电探针共用**（`bb_radio_step` 用 txn 0x31 问同一服务），照抄会把探针的回答吞掉 ——
实测现象就是 `the DMS answer` 一条都不出现。改成 txn 不匹配时返回 **0**（交还原分支）后，探针恢复、探针与握手并存。

### 低功耗不影响载波配置

设置低功耗之后 TFTP 仍是 **178 请求 / 174 应答**，`mcfg_sw.mbn`（CMCC）仍被请求 3 次，`rf_static` 仍被请求。
所以 ABM 预期的低功耗状态不会妨碍现有这条链。

启动参数默认不加（不加时行为与之前完全一致）。

## 2026-11-23 09:46 实测：CommCenter **在跑**，但不碰基带

在 `fuxi_modem_report` 里加了一次进程表扫描（30 s 与 120 s 各一次，`sys/proc_internal.h` 的
`allproc` + `proc_list_lock`），实机输出：

```
4040: CommCenter is running at 30 s (pid 79)
```

**所以"设置里没有蜂窝入口 / 没有 SIM / 没有 IMEI"不是因为它没启动**，而是它**启动后没有连到我们的基带**：
桥对每个通道打开都会打印，而日志里一次都没有。

这也排除了两件事：

- `sheng-disable-jobs` / `disabled.plist` 没有把 CommCenter 禁用；
- 机型身份（ADT 里的 `iPhone15,2`）不是它不启动的原因。

### 与参考实现的对照

`coachz_cellular_compat.py` 的说明是：

> Prepare the pinned CommCenter for a modem **without Apple postponement tickets**.

也就是说：**原版 CommCenter 会拒绝驱动一颗没有苹果"推迟票据"的基带**，补丁把那条守卫改掉。
我们用的是 J120（iPad Pro 12.9" Wi-Fi，`iPad7,1`）的 iPadOS 17.7.11 21H461 —— 一台 **Wi-Fi 机型**的
CommCenter，本来走 `NoBBRadioModule`，更不会去驱动基带。

### 因此的下一步

下载 `iPad7,1` 的 17.7.11/21H461 IPSW（6.8 GB，`updates.cdn-apple.com`），取出
`System/Library/Frameworks/CoreTelephony.framework/Support/CommCenter`，跑
`coachz_cellular_compat.py --commcenter` 生成兼容副本 + trustcache，再装进 payload。

## 2026-11-23 10:15 两件待办的状态

### RF 静态表：三个来源都没有，已确认是死路

- OTA `modem.img`：FAT16，`image/` 下 169 个条目全是固件段，**没有 `modem_pr`**；之前命中的
  `MODEM_PR`/`MCFG` 逐字节看是固件里的符号/格式串（`.qmi_modem_init`、`.MCS/.MCFG/.V2XL1`）。
- `modem_a`：`configs/` 下只有 `mbn_build_utc.txt`、`mcfg_hw`、`mcfg_sw`，`mcfg_ddr` → ENOENT。
- `modem_b`：同一棵树，同样没有。
- **modem 从不回写它**：整轮只发了 1 次 `server_check.txt` 与 3 次 `mcfg.tmp` 的 WRQ。
- modem 自己的代码把这一步叫 "DDR MBN discovery"，失败只打 `Err`。

**结论：这张表在我们能拿到的任何地方都不存在，而且不是阻塞点。**

### RMTFS 的写入其实保留在内存里

`FuxiModemRmtfs.cpp:227-229`：

> The write goes into the copy in memory and no further: the storage is not written, so the
> calibration and the IMEI cannot be damaged by the modem.

**所以一次启动之内 modem 写进去的东西它自己读得到**（只是不落盘、不跨重启）。这条不是当前的阻塞原因。

### 内核安装 / bindfs：整套机器已经在仓库里，只是没给 fuxi 应用

`UniXNU/drivers/QcomBaseband/platform/coachz/patches/coachz-prelink-baseband.patch`（452 行）里就有
`coachz_baseband_archive` / `_unpack` / `_install` / `_prepare` / `_point` 全部实现。
而它依赖的 `native_gpu_addon_write`、`native_gpu_addon_parents`、`native_gpu_addon_mount`、`bindfs`
**我们的 XNU 补丁里已经有**。

**编译产物里查不到 `coachz_baseband_install`** → 这份补丁**没有应用到 fuxi**。
它挂的钩子在 `bsd/dev/coachz_wifi.c`（coachz 的 GLINK 启动处），要改成 fuxi 的 `fuxi_modem.c`。

所以这一步是"把现成的 250 行接到 fuxi 的胶水上 + 打包"，等兼容 CommCenter 到了再一起做（它的放置方式要和打包一致）。

## 2026-11-23 10:20 x13 的射频校准走的是 EFS，不是 TFTP

`x13/蜂窝调研.md:21-23`：

> ## ⚠️ 基带数据分区（IMEI、射频校准）
> `modemst1`、`modemst2`、`fsg`、`fsc` 存着 IMEI 和射频校准，基带通过 **rmtfs 会写入**它们。

**所以主线的射频校准来自 EFS（四个分区），经 rmtfs —— 不是 tqftpserv。**

我们的 rmtfs 服务的正是这四个（`FuxiModemRmtfs.h:64-69`）：

```
/boot/modem_fs1 -> modemst1   (8 MiB)
/boot/modem_fs2 -> modemst2   (8 MiB)
/boot/modem_fsc -> fsc        (1 MiB)
/boot/modem_fsg -> fsg        (8 MiB)
```

插卡那轮（`run-20261123-083749`，776 s）的计数器：

```
opened 2 closed 0, 5120 reads and 5120 writes of 5120 and 5120 sectors
```

**modem 从这四个分区读走了 5120 个扇区、写回了 5120 个扇区** —— 射频/IMEI 这条路径是活的，
和主线的做法一致。

### 因此 `mcfg_ddr/rf/rf_static_*.mbn` 是另一条可选的发现路径

主线那套 tqftpserv 服务的也是同一棵 `modem_pr` 树（我们验证过它没有 `mcfg_ddr`），所以**主线同样不靠它拿校准**。
modem 固件自己把这一步叫 "DDR MBN discovery"，失败只打 `Err`。

### 唯一与主线不同的地方

我们的 rmtfs **写入只落在内存里**（`FuxiModemRmtfs.cpp:227-229`），主线写的是真实分区。
一次启动之内没问题（modem 写进去自己读得到），但 **modem 自己重启（SSR）后这些写入会丢**。
如果它的射频初始化是"写 NV → 重启 → 再读"，就会在这里打转 —— **这是接下来值得补的一处**。

## 2026-11-23 10:50 两件都落地了

### 1. RMTFS 写入持久化（已实机验证）

`FuxiModemRmtfs.cpp` 新增：写入时标脏（`gDirty`），每秒由 `fuxi_modem_report` 调一次
`fuxi_rmtfs_flush()`，把脏文件写到 Data 卷的 `/private/var/db/coachz-rmtfs-<partition>`
（用 `vnode_open` / `vn_rdwr`，1 MiB 一段；父目录是已有的 `/private/var/db`，因为建目录需要进程上下文）。

实机（`run-20261123-104917`）：

```
9053: FUXI_RMTFS: modemst2: 8388608 bytes of the modem writes saved to /private/var/db/coachz-rmtfs-modemst2
9672: the rmtfs was announced 1 times, took 7 requests, answered 7; opened 2, 5120 reads and 5120 writes
```

**modem 对 modemst2 的 8 MiB 写入整份落到了 Data 卷。** 无中止。

**还没做的一半**：开机时优先读回该副本。原因是 **RMTFS 的文件加载发生在 probe 早期
（the modem files 那一步），那时 Data 卷还没挂载**（Data 挂载在 `load_init_program`）。
要读回必须把加载推迟到 Data 挂载之后。

### 2. 兼容 CommCenter（已生成并校验）

下载并校验了 `iPad7,1` 的 iPadOS 17.7.11 / 21H461 IPSW（6,835,124,840 字节，
sha256 `c84ad24b…3e519b1`，与 Apple 公布值一致），取出系统卷 `044-76040-038.dmg`（5.2 GB，
sha256 `3f99b859…1028d`）里的
`System/Library/Frameworks/CoreTelephony.framework/Support/CommCenter`：

- sha256 `383779eda4bbad539f8809d39b109ffd5a003335824c170f0a81cd8073e519b1` —— **与工具的 SOURCE_SHA256 一致**
- LC_UUID `F1D178CF-7F4C-3576-B9D7-0DC14AC6F82D` —— **与工具的 SOURCE_UUID 一致**

跑 `coachz_cellular_compat.py --commcenter` 之后：

```
Prepared CommCenter compatibility copy, sha256 6bf1e5f9c0fed71622be39bcaa60e6c41d2893c14a56dc76809d35a5388121f4
/tmp/compat/CommCenter        26,254,032
/tmp/compat/CommCenter.zlib    9,951,161
/tmp/compat/trustcache.bin          48     （v2 模块，cdhash b1ad1752…8f087766，与副本一致）
```

**剩下的是把它装进系统**：参考实现用"内核归档携带签名副本 + ADT 携带 SHA-256 + 信任缓存携带 cdhash +
开机装到 Data 并 bindfs 联合挂载到 CoreTelephony Support"。这套代码在本仓库里**已有**
（`platform/coachz/patches/coachz-prelink-baseband.patch`，452 行），**但没有应用到 fuxi** ——
它挂的钩子在 `coachz_wifi.c`，要改成 fuxi 的 `fuxi_modem.c`。

## 2026-11-23 11:30 兼容 CommCenter 装进去并**跑起来了**

### 打包

- **GPU addon 复用**：`scripts/addon-add-dir.py` 把兼容副本作为 `baseband/CommCenter` 加进 `NGPUADD2`
  容器（9 → 10 个文件，12.8 MB → 22.7 MB）。内核本来就把它装到 `/private/var/ngpu/`。
- **信任缓存**：`scripts/make-fuxi-trust-bundle.py`（新增）按 `make-owner-trust-bundle.py` 的格式重建 bundle
  （v3 头 + v2 模块，hash 严格升序），把 cdhash `b1ad1752…8f087766` 并进 owner 模块。
- **挂载**：新增 `patches/xnu/fuxi-baseband-addon-mount.patch`（照 `fuxi-audio-addon-mount.patch`），
  第五个 bindfs 挂载：`/private/var/ngpu/baseband` → `/System/Library/Frameworks/CoreTelephony.framework/Support`。

### 两次实机结果

**第一次（`run-20261123-112156`）** —— 装好了但被 AMFI 拒绝：

```
5126: fuxi-gpu-addon: handover 1, bytes 22755232, install 10/10/0 err 0, mount 5/0 5/0 1/2 1/2
3650: AMFI: Launch Constraint Violation (enforcing), ... launching proc: .../CoreTelephony.framework/Support/CommCenter
4051: FUXI_MODEM: CommCenter is NOT running at 30 s (pid 0)
```

**不是信任缓存问题，是 launch constraint**（`Constraint not matched`）。
而我们内核里本来就有 `zzyc-owner-skip-launch-constraints.patch`，它的开关是启动参数 **`sheng-no-lc=1`**。

**第二次（`run-20261123-112701`，加上 `sheng-no-lc=1`）**：

```
4042: FUXI_MODEM: CommCenter is running at 30 s (pid 79)
8995: COACHZ baseband: 94 s: 49 QMI services, 6 clients, ... sahara state 0, 0 executes, 0 requests
9124: COACHZ baseband: 95 s: the radio: offline not registered
```

**兼容 CommCenter 已装、已挂、已可信、正在运行。** 不再有 Constraint Violation。

### 但基带通道仍未被打开

`sahara state 0, 0 executes, 0 requests` —— **没有任何东西打开 sahara**，即 ABM 还没开始给基带上电。

### 下一个缺口：ABM 的固件目录

参考实现的 `coachz_baseband_prepare()` 除了装 CommCenter，还把基带固件放到两个位置并 bindfs 挂上：

- `/private/var/coachz/baseband/Mav16` → `/private/preboot/<HASH>/usr/local/standalone/firmware/Baseband/Mav16`
- 同一个目录 → `/usr/standalone/firmware/Baseband/Mav16`

ABM 给基带上电前会先找这个固件目录；没有它就停在"没有固件"，不会去 open `AppleBaseband` 的通道。
对我们来说 Mav16 的内容就是设备上 `/image/` 那套基带固件（和 TFTP 服务的是同一批文件）。

## 2026-11-23 11:42 ABM **根本没有打开基带**（不是固件目录的问题）

之前"零通道打开"这条证据有洞：`AppleBaseband` **顶层**的 user client 打开时**不打印**（只有 `AppleBasebandPCIControl`
和每个通道会打）。所以补了一个探针（`coachz_baseband.cpp` 的 `AppleBaseband::newUserClient`）。

实机（`run-20261123-114151`）：

```
4034: FUXI_MODEM: CommCenter is running at 30 s (pid 79)
10260: COACHZ baseband: 109 s: 49 QMI services, 6 clients, ... sahara state 0, 0 executes, 0 requests
=== first user client on ... ===
(none)
```

**`AppleBaseband`、`AppleBasebandPCIControl`、任何通道，全都没有 user client。**

### 结论（回答"是不是因为固件目录没做"）

**不是。** 固件目录是 ABM 决定给基带上电之后才会用到的；现在的观测是 **ABM 连基带服务都没打开**，
在这之前就停下了。所以固件目录**不是当前阻塞点**。

### 下一步要查的方向

1. **CommCenter 的基带模块选择**：这套系统镜像来自 **J120（Wi-Fi iPad）**。参考实现的说明是
   "libTelephonyUtil 的机型表里没有 J120（Wi-Fi），所以 CommCenter 会跑 NoBBRadioModule；J121 才是高通 Mav16"。
   兼容工具只改了一条指令（推迟票据那条守卫），**没有**动机型表。
2. **身份不一致**：ADT 里是 `iPhone15,2`（蜂窝 iPhone），系统镜像是 J120 的。ABM 可能按 iPhone 的型号去找
   对应固件/Mav 版本，和桥发布的东西对不上。
3. 要真正分辨，需要 **ABM 自己的日志**（`com.apple.telephony.abm` 子系统，走统一日志，不在串口上）。
   参考实现为此写了 logging 子系统 plist；我们还没有读统一日志的手段。

## 2026-11-23 13:00 自己的服务：manifest 已签名、已挂载、不再 panic

### 根因（新发现，比之前所有假设都靠前）

系统镜像里 **CommCenter 有两份 job**，launchd 按设备树二选一：

```
com.apple.CommCenter.plist              _LimitLoadToDeviceTree:   baseband
com.apple.CommCenter-noncellular.plist  _LimitLoadFromDeviceTree: baseband
```

两份跑同一个二进制，但 cellular 那份多一个 `com.apple.commcenter.carrierspace.xpc` MachService。
**我们的 DTB 没有 `/baseband` 节点**，所以生效的是 noncellular —— CommCenter 以"无基带"模式运行，
ABM 自然不看 `AppleBaseband`。这解释了：CommCenter 在跑、桥 22/22、零 user client、sahara state 0。

Trogdor 的 `docs/plans/modem.md:116` 也印证："launchd 按 `/baseband` 选 `com.apple.CommCenter.plist`"。

### 自有服务的实现（Trogdor 路线）

1. **内核写 `/Library/LaunchDaemons` 失败**：`open` 返回 30（EROFS）—— 该目录在只读 sealed 根上。
2. **改用 manifest**：`/System/Library/xpc/launchd.plist` 是 1.6 MB 的 bplist，键为
   `"/System/Library/LaunchDaemons/<name>.plist"`，共 561 项。我们生成新 manifest：
   - 删掉 noncellular 那一条；
   - 去掉 cellular 的 `_LimitLoadToDeviceTree`；
   - **加入自己的 job** `org.unixnu.fuxi.logdump`。
3. **签名**：参考实现的关键（`apps/README.md`）：`launchd_cache_loader` 读同目录 `launchd.plist.sig`，
   取 CodeDirectory 问 AMFI，再 `SecCodeMapMemory` 逐页校验。所以：
   `codesign --sign - --identifier … --digest-algorithm sha256 --pagesize 4096 --detached` 给 manifest 签一份，
   再把 **CodeDirectory 的 cdhash** 加进信任缓存。

   我们的 manifest cdhash = `ee7c935c38a15cad1e4b9a5f88b5412c017ad653`（现已加入 owner trust module）。

4. **挂载**：第六个（`/private/var/ngpu/daemons` → `/Library/LaunchDaemons`）与第七个
   （`/private/var/ngpu/owner-apps/xpc` → `/System/Library/xpc`）都已接上。

### 实机结果（`run-20261123-130045`）

```
306: FUXI addon mounts: commcenter 5/0, jobs 5/0, manifest 5/0
CS_KILLED: 0
4042: CommCenter is running at 30 s (pid 80)
```

**三个挂载全部 stage 5 / status 0，没有 CS_KILLED，launchd 正常。** 用原版侧车时那一轮是
`CS_KILLED launchd_cache_loader` panic —— 所以**签名侧车的配方在这台设备上是对的**。

### 仍待确认

我们的 job 没有输出（`FUXILOG` 零行），CommCenter 也仍然没有打开基带。两种可能：

1. launchd 虽然接受了 manifest，但没有装载我们新增的条目（例如它按 `SystemLibraryTreeState` 校验）；
2. job 跑了，但守护进程的 stdout 指向的 `/dev/console` 在本端口没有接到串口。

要分辨，最直接的是让守护进程把结果**写成一个文件**，再由内核读回来打印（内核写 Data 卷已经验证过，读同理）。

## 2026-11-23 13:29 自己的服务跑起来了（关键：程序必须在 sealed 路径上）

### 实测

```
306:  FUXI addon mounts: commcenter 5/0, jobs 5/0, manifest 5/0
13431: FUXI_LOGDUMP 150 s: FUXILOG: the job ran
```

**我们自己的 launchd 任务（`org.unixnu.fuxi.logdump`）真的被执行了** —— 守护进程写了文件，
内核在 150 s 读回来并打印。

### 三处必要的修正（缺一不可）

1. **manifest 必须字节级追加**，不能整份重写。参考的 `append_launchd_job()` 注释说得直接：
   "Keep the native cache's original objects and reference identities intact"。
   我们搬了这份实现到 `scripts/make-fuxi-manifest.py`（新 manifest = 原件 1,640,626 + 2,737 字节）。
   整份重写（plistlib）时 launchd **静默回退到 sealed 原件**，连原有任务的改动都不生效（已实测）。
2. **签名侧车**：`codesign --sign - --digest-algorithm sha256 --pagesize 4096 --detached`，
   并把 CodeDirectory 的 cdhash 加进信任缓存（我们用的是 owner trust module）。
   不做这一步会 panic：`CS_KILLED launchd_cache_loader`。
3. **程序路径必须在 sealed 根上**：launchd **不会**执行 `/private/var/ngpu/bin/...` 这种 Data 卷上的程序。
   把守护进程改放到 `CoreTelephony.framework/Support/`（我们本来就有基带的 union 挂载在那里），
   **任务立刻跑起来了**。这是最后一块拼图。

### 另外补上的两个 job 属性

- `com.apple.private.security.no-sandbox: True`（原版 manifest 自己对 `com.apple.replayd` 就是这个拼法）
- 输出写文件（`/private/var/log/fuxi-logdump.log`），而不是 `/dev/console`。

### 下一步

manifest 现在**确实被 launchd 采用**了。所以让 CommCenter 走蜂窝分支有两条路：

1. **给设备树加 `/baseband` 节点**（`device_type = "baseband"`、`compatible = "baseband,n41"`，
   并设 `/product/baseband-chipset = "mav16"`）—— 这是 Trogdor 的实际做法，原版 manifest 的 cellular 条目就会生效；
2. 或在 manifest 里字节级改写 CommCenter 那条（去掉 `_LimitLoadToDeviceTree`），比删除条目更麻烦。

**建议走 1**：它同时给 CommCenter 一个 `compatible` 值，参考实现的实测结论是
"`compatible` 用 J121AP 后 CommCenter 走高通分支并启动 ABM"。

## 2026-11-23 13:52 设置里出现了"蜂窝网络"入口 —— CommCenter 已经是蜂窝模式

实测（用户观察，`run-20261123-134705`，已带 `/baseband` 节点）：

- 设置里**出现了"蜂窝网络"入口**（之前完全没有）
- 点不进去；"关于本机"里 **Carrier: No available**

**这说明两次修改都生效了**：manifest（字节级追加 + 签名侧车）被 launchd 采用，`/baseband` 节点让
launchd 选了 cellular 那份 CommCenter job。ABM 因此在跑蜂窝分支。

但基带仍未被驱动：

```
COACHZ baseband: 252 s: 49 QMI services, 6 clients ... sahara state 0, 0 executes, 0 requests
```

**零 user client** —— ABM 还没给基带上电。

### 剩下的缺口：ABM 的基带固件目录

参考实现的 `coachz_baseband_prepare()` 在装 CommCenter 之外，还把 Mav16 固件放到两处并 bind 挂载：

```
/private/var/coachz/baseband/Mav16 -> /private/preboot/<HASH>/usr/local/standalone/firmware/Baseband/Mav16
                                -> /usr/standalone/firmware/Baseband/Mav16
```

ABM 上电前会先找这个目录；没有就停在"没有固件"，不会去 open `AppleBaseband` 的通道。
（这也是为什么现在设置里能进"蜂窝"入口但不能用、Carrier 显示 No available：CommCenter 有了蜂窝身份，
但没有任何基带状态可读。）

### 固件不需要塞进 payload

设备上 `modem_a` 的 `/image/` 就是这套固件（我们的 TFTP 服务的正是它）。内核已经能**读** FAT 卷
（`fuxi_adsp_fw_read_file`）也能**写** Data 卷（`fuxi_rmtfs_flush` 已验证），所以可以在开机时
**把固件从 modem 分区复制到 `Mav16` 目录**，不必占用 192 MiB 的 payload。

这是下一步。

## 2026-11-23 14:38 固件目录就位，且**透过 union 可见**

### 实测（`run-20261123-143512`）

```
13432: FUXI_LOGDUMP 150 s: FUXILOG: the job ran, 17 firmware files unpacked
13433: FUXI_LOGDUMP: the firmware is visible through the union at 150 s
```

也就是说 `/usr/standalone/firmware/Baseband/Mav16/Info.plist` 在设备上**确实可解析** ——

- 我们的自有服务把 IPSW 里的 `vinyl_02_Mav16-11.00.01.Release.bbfw`（33,837,189 字节，22 条，64 MiB）
  解到了 `/private/var/ngpu/owner-apps/firmware/Baseband/Mav16/`（17 个文件）
- 内核的 union（`/private/var/ngpu/owner-apps/firmware` → `/usr/standalone/firmware`）**能看到后写入的文件**

### 内核侧解压这条路为什么走不通（结论）

内核在 `native_gpu_addon_prepare` 里解压时，`mkdir` 返回 **30 (EROFS)**：

```
308: sheng baseband firmware: mkdir /private/var/ngpu/owner-apps/firmware/Baseband/Mav16 says 30
```

**因为那一刻 `/private/var`（Data）还是只读的** —— Data 卷挂载得比这一步晚。
（这也解释了同一步里 addon 安装器为什么总是 `install N/N/0`：内容都已相同，没有一次实际写入。）
所以固件由**晚些启动的用户态守护进程**解压是正确的，内核那段留作记录但没有用上。

### 仍然：ABM 不打开基带

```
first user client on ... : (none)
sahara state 0, 0 executes, 0 requests
```

固件已经在了、可见了，ABM 却仍然没有上电。剩下的候选：

1. **时机**：ABM 在 ~20–40 s 就检查过固件，而我们的守护进程 ~45 s 才解压完；
   它可能缓存了"没有固件"的结论。需要让解压**更早**（或在解压后让 CommCenter 重新尝试）。
2. **第二个路径**：参考实现同时挂 `/private/preboot/<HASH>/usr/local/standalone/firmware/Baseband/Mav16`，
   我们只做了 `/usr/standalone/firmware`。
3. **ABM 需要的不只是文件存在**（例如 `Info.plist` 的版本匹配、或 BBUpdater 的其它前置条件）。

第 1 条最值得先排：把解压提前 + 让 CommCenter 在文件就绪后重启。

## 2026-11-23 14:59 按参考补了 preboot 那条路；固件可见，但 ABM 仍不动

### 参考实现的关键（`coachz-prelink-baseband.patch:387-398`）

```c
SecureDTGetProperty(chosen, "boot-manifest-hash", &value, &value_size)   // 48 字节
snprintf(hash + 2*i, 3, "%02X", value[i]);                              // → 96 个十六进制字符
snprintf(entry, "%s/%s/usr/local/standalone/firmware/Baseband/Mav16", preboot_source, hash);
→ 把 /private/var/coachz/baseband/preboot  union 挂到 /private/preboot
```

也就是 **ABM 读的是 `<preboot>/<boot-manifest-hash>/usr/local/standalone/firmware/Baseband/Mav16`**，
`/usr/standalone/firmware` 是第二条。

### 我们的实现

- 内核补了**第九个挂载**：`/private/var/ngpu/owner-apps/preboot` → `/private/preboot`（union），实测 `5/0`。
  （第一次用 `0xf00` 偏移导致 EFAULT —— `data` 指针落到 4 KiB 暂存页边界外，改成 `0x200` 后成功。）
- 守护进程解压后**镜像到 preboot 树**（硬链接，省 64 MB 复制）。

### 实测（`run-20261123-145525`）

```
311: FUXI addon mounts: commcenter 5/0, jobs 5/0, manifest 5/0, firmware 4/2, preboot 5/0
13436: FUXI_LOGDUMP 150 s: FUXILOG: the job ran, 17 firmware files unpacked, preboot mirror 0
13437: FUXI_LOGDUMP: the firmware is visible through the union at 150 s
```

- `/usr/standalone/firmware/Baseband/Mav16` **可见** ✓
- **preboot mirror = 0**：守护进程在 `/private/preboot` 里**没找到 96 字符的目录名**（或列不出该目录），所以没镜像
- 仍然 `first user client on ... : (none)`

### 结构性的差异（与参考不同的一点）

参考在**内核**里解压（`coachz_baseband_unpack`），因为他们的 Data 在 `load_init_program` 时**可写**；
我们这边的 Data 在那一刻仍然是 **EROFS**（`mkdir` 返回 30），所以只能在**用户态晚些**解压 ——
**这带来一个时机差**：ABM 在 ~20–40 s 就检查过固件，而固件 ~45 s 才出现。

### 两个可选做法

1. **把固件预先放进 Data seed**（构建期）：`owner-data-16g.dbr` 里就有 `ngpu/owner-apps/firmware/...`，
   于是**从第一刻起就存在**，彻底消除时机差。seed 是 16 GB，加 64 MB 没问题。
2. **修好 preboot mirror**（先把 `/private/preboot` 列不出来的原因查清），再配合"解压后让 CommCenter 重新初始化"。

**建议 1**：它是参考实现"内核先解压、再挂载"在我们这个端口上的正确等价物。

## 2026-11-23 15:36 内核按参考布局写固件成功（挂载前，17 个文件）

### 实测（`run-20261123-152942`）

```
sheng baseband firmware: the standalone tree answers 0
sheng baseband firmware: zip 33837189 bytes, eocd at 33837167, 22 entries
sheng baseband firmware: 17 of the package's images written, status 0
FUXI addon mounts: commcenter 5/0, jobs 5/0, manifest 5/0, firmware 5/0, preboot 1/2
```

**内核在挂载之前把 17 个固件映像写进 `/private/var/coachz/baseband/standalone/Baseband/Mav16`，
随后 firmware union 达到 5/0。** 这正是参考实现的顺序与成果。

### 这一路排掉的四个坑（每个都有实测）

1. **`mkdir("/private")` 返回 30 (EROFS)** —— sealed 根的写权限检查**先于**存在性检查。
   修法：`EROFS` 视同"已存在"跳过，真正的缺失由后续 `open()` 报出。
2. **`mkdir` 把文件名本身也建成了目录** → 写文件返回 **21 (EISDIR)**。
   修法：只为**父目录**建目录；写前先 `rmdir` 掉上一轮误建的同名目录。
3. **`0xf00` 的挂载暂存偏移**让 mount 参数里的 `data` 指针落到 4 KiB 页外 → **EFAULT**（改 `0x200`）。
4. 早前误判"Data 在开机早期只读"—— 实测 `mkdir /private/var/coachz-probe` 等**都返回 0**；
   只有 `/private/var/ngpu/owner-apps/firmware` 那条路径是 EROFS（与 `/private` 同理）。

### 仍然：ABM 不打开基带，而且 preboot 那条路是空的

```
COACHZ baseband: 278 s: 49 QMI services, 6 clients ... sahara state 0, 0 executes, 0 requests
first user client on ... : (none)
preboot 1/2        ← 源目录不存在
sheng baseband firmware: no boot-manifest hash
```

**我们的设备树里没有 `/chosen/boot-manifest-hash`**（参考实现从它取 48 字节 → 96 个十六进制字符，
用来定位 `/private/preboot/<HASH>/usr/local/standalone/firmware/Baseband/Mav16`）。所以 preboot 树没写、
那个 union 也没内容。**ABM 很可能先看 preboot 这条。**

### 下一步

把 `boot-manifest-hash` 补进 `/chosen`：端口的 ADT 构建器支持 `[adt.chosen.bytes]`，
值就是我们启动日志里出现过的那个 96 位十六进制串（48 字节）：

```
BC54BE6681F5959F9AB9401DCD1883CB21ED185937FD7E02134BDA743DC529E798D84BA08AC69B0ABB5CBED0E6FBDAFA
```

放进 `/chosen` 后，内核那段就会把 preboot 树也写出来并挂上 5/0。

## 2026-11-23 16:12 去掉 owner SystemOS union 后仍然能启动

### 改动

- `configs/fuxi_defconfig`：`CONFIG_XNU_OWNER_UNION_SYSTEMOS=y` → **not set**
  （效果：SystemOS 缓存从挂 `"/"` 改为挂 `"/private/preboot"`，且不再要求 `native_union`）
- `board.toml`：撤掉 `/chosen/boot-manifest-hash`（那是端口自己的镜像集身份）
- 补丁：撤掉 preboot union 挂载（第九个）

### 实测（`run-20261123-161010`）

```
FUXI addon mounts: commcenter 5/0, jobs 5/0, manifest 5/0, firmware 5/0
sheng baseband firmware: 17 of the package's images written, status 0
sheng baseband firmware: no boot-manifest hash
FUXI_MODEM: CommCenter is running at 120 s (pid 80)
COACHZ baseband: the card's slot status: error 0, 5 TLVs
COACHZ baseband: 177 s: 49 QMI services, 6 clients ... sahara state 0, 0 executes, 0 requests
```

**没有任何 panic 或 CS_KILLED，系统正常启动，modem 链路、TFTP(178)、CommCenter 都在。**
也就是说这个端口在**非 union 模式**下同样能跑 —— 这是这一轮最有价值的新事实。

### Trogdor 的 1–4 我们对照的现状

| # | 参考做法 | 我们 |
|---|---|---|
| 1 | `/chosen/coachz-baseband`、`/baseband` 节点、`baseband-chipset` | ✅ 全有 |
| 2 | IOKit 服务与 UserClient（`coachz_baseband.cpp`） | ✅ 22/22 |
| 3 | kern_exec：固件写入 + 联合挂载 + 放行 CommCenter | ✅ 固件 17 文件、四处挂载 5/0、`sheng-no-lc` |
| 4 | 打包：`/baseband` + Mav16 固件 | ✅ `/baseband` + standalone 固件 5/0 |

**唯一还差的是 preboot 那条树**（`<preboot>/<hash>/usr/local/standalone/firmware/Baseband/Mav16`）。

### 为什么 preboot 这条在我们这里不能照抄

- 参考端口**没有** owner 缓存挂载，所以 `/private/preboot` 上再加 union 无人竞争。
- 我们的端口在**非 union 模式**下把 SystemOS 缓存**挂在 `/private/preboot` 本身**；
  在它上面再叠一个 union 会把 dyld 缓存挡掉。
- 在 **union 模式**下挂载点是 `"/"`，但授权检查要求 `native_union`，我们那次 preboot union 与它冲突 → EAUTH。

**所以 preboot 固件只能"直接写进真实的 preboot 树"，不能靠挂载。** 下一步是探测那条路径是否可写。

## 2026-11-23 16:31 参考的"两级 bind"在我们端口复现不了（实测）

### 参考真正的交付方式（读全了）

```c
// 文件只写一处
snprintf(path, "/private/var/coachz/baseband/Mav16/%s", name);      // line 268
// 造挂载点 + 两级挂载
coachz_baseband_point(preboot_source "/<hash>/usr/local/standalone/firmware/Baseband/Mav16");  // 393
native_gpu_addon_mount(folder, preboot_point "/<hash>/...");        // 400  ← bind 固件目录
native_gpu_addon_mount(preboot_source, "/private/preboot");        // 398
coachz_baseband_point(standalone_source "/Baseband/Mav16");        // 404
native_gpu_addon_mount(folder, "/usr/standalone/firmware/Baseband/Mav16");  // 409
native_gpu_addon_mount(standalone_source, "/usr/standalone/firmware");      // 407
```

### 我们照做之后的实测（`run-20261123-162905`）

```
sheng baseband firmware: the folder answers 0            ← 单一 Mav16 目录建好
sheng baseband firmware: 17 of the package's images written, status 0
sheng baseband firmware: the port published no boot manifest hash   ← 非 union 模式下端口不发布
FUXI addon mounts: ... fw preboot 0/2 0/-1, fw standalone 4/2 4/2
FUXI_LOGDUMP: the firmware is not visible through the union at 150 s (2)   ← 不是假阴性
```

**两级 bind 在我们这里不成立**：单挂 `standalone_source → /usr/standalone/firmware` 时
（文件直接写在那棵树里）是 **5/0**；改成"先 bind 固件目录到树内的挂载点、再挂整棵树"就变成 **4/2**，
而且 150 s 的独立检查也确认**看不到** `Info.plist`。我们的 bindfs 组合不了这种嵌套 bind。

### 因此的正确等价做法

- **直接写进每棵树**（standalone 已验证 5/0）—— 效果与参考的 bind 相同，文件多一份而已
- preboot 需要真实目录名；**非 union 模式下端口不发布 `boot-manifest-hash`**，
  所以要用 `readdir` 读 `/private/preboot` 里的 96 字符目录名

### 顺带确认：非 union 模式能正常启动

`run-20261123-161010` 与这一轮都正常启动（无 panic、CommCenter 在跑、TFTP 178、modem 链路在）。

## 2026-11-23 17:12 参考的 1–4 全部落地并验证（含 preboot）

### 最终做法（与参考一致，只把 hash 换成端口自己发布的那个）

```
sheng baseband firmware: the port's published boot manifest hash is read through the IORegistry
sheng baseband firmware: the preboot tree answers 0 (BC54BE66…FBDAFA)
sheng baseband firmware: 34 of the package's images written, status 0
FUXI addon mounts: commcenter 5/0, jobs 5/0, manifest 5/0, firmware 5/0, preboot 5/0
```

**五个 union 全部 stage 5 / status 0，无 panic，系统正常。**

### 关键的两个认知（都是实测换来的）

1. **不能往 `/chosen` 写自己的 `boot-manifest-hash`**：union 模式下端口用它表达镜像集身份，外来值会让
   SystemOS 挂载报 **EAUTH(80)**（两次实测，且第二次已无 preboot 挂载，故与挂载无关）。
2. **端口的 hash 发布在 IORegistry，不在 FDT**（`IORegistryEntry::fromPath("/chosen", gIODTPlane)`），
   所以必须用 IOKit 读（`FuxiModemRmtfs.cpp` 里的 `fuxi_boot_manifest_hash()`），
   `SecureDTGetProperty` 永远读不到。

### 固件怎么送（与参考的差异）

参考：文件一份 + 两级 bind（folder → 树内挂载点 → 整树 union）。
**我们端口的 bindfs 组合不了这种嵌套 bind**（实测 `4/2`，150 s 独立检查也看不到）。
所以改成**两棵树各写一份**（34 个文件），再各自 union：效果相同，机制更简单。

### Trogdor 1–4 对照（全部完成）

| # | 参考 | 我们 |
|---|---|---|
| 1 | `/chosen/coachz-baseband`、`/baseband`、`baseband-chipset` | ✅ `baseband,n41` / `mav16` |
| 2 | IOKit 服务与 UserClient | ✅ 22/22 |
| 3 | kern_exec 固件写入 + 联合挂载 + 放行 CommCenter | ✅ 34 文件、五处 5/0、`sheng-no-lc` |
| 4 | 打包 `/baseband` + Mav16 固件 | ✅ 两条路径都有 |

### 仍未解决

ABM **仍然不打开 `AppleBaseband`**（`sahara state 0`、零 user client），而固件与路径都已就位。
剩下要看的是 **ABM 自己的日志**：我们的自有服务已经在跑、文件通道也验证可用，
但 `OSLogStore` 的过滤目前拿到 0 条 telephony 记录，需要放宽过滤并做多轮采集。

## 2026-11-23 17:54 对照 Trogdor-ipadOS 的完整提交历史

那份仓库（`Trogdor-ipadOS`，187 个提交）的蜂窝接入顺序如下，附我们的状态：

| 提交 | 内容 | 我们 |
|---|---|---|
| `0beb19c` | 在基带桥上起**蜂窝** CommCenter | ✅ |
| `2844cb0` | 发布 `AppleIPAppender` 与 PDP manager，CommCenter 不再崩 | ✅ |
| `66e7e69` | 固件挂到 ABM 读的位置、打开 ABM 私有日志、**sahara command mode** | ✅ 固件两处 5/0 |
| `5bcd725` | 把 BHI 寄存器当"未熔断 modem"应答 | ❓ 未验证（ABM 还没走到） |
| `1203ad9` | 按协议匹配通道、经 QMUX 接到 modem | ✅ `matchPropertyTable` |
| `87522ac` | 从 eMMC boot 分区取 factory data → IMEI/ICCID | ❓ 未验证 |
| `7cc7f77` | 应答 activation、按 ModemManager 建 IPA 数据口、LTE 注册 | ❓ 未验证 |
| `ec50b12` | activation 指示里报告"已开通 + 双槽接受" | ❓ 未验证 |
| `c679400` | ICCID 读序 + activation 指示里带 IMSI | ❓ 未验证 |
| `934fcc3` | BSP `0xb001` 回 chip id/serial | ❓ 未验证 |
| `0b2f663` | 按 Apple modem 的方式接收载体包 PRI/GRI | ❓ 未验证 |
| `62dadba` | **固定版 CommCenter 兼容副本** → 原生 PDP | ✅ 已装（同一工具） |
| 之后 eSIM/PDP 若干 | 不在当前目标 | — |

## 文档里的"启动门槛"（`hardware-tests.md:1629-1659`）

```
1629: 根节点 compatible 换成 J121AP 后，CommCenter 不再报 No CommandDriver dylib，走 C2KRadioModule，ABM 启动
1630: 启动 ABM 后立即 SIGSEGV（ApplePDPHelperInterface::enableMetadataTrailer，this 为空）
1637: 发布 AppleIPAppender 和 AppleBasebandPCIMAVPDPManager、应答方法 6 和 15 后不再崩溃
1639: ABM 启动基带失败 15 次后报 Baseband is declared dead（BBUpdater error 2）
1644: 固件目录 bind 挂到 /private/preboot/<hash>/... 后 BBU 能读到
1649: 方法 5 应答后 BBU 报 CHIPID=0x9540e1
1655: ABPInterfaceProtocol 比较后，pbl/sahara/qmux1/qmux2/refs 各开各的
1656: Sending Image SBL 100%，Baseband booted，engage 24 ms
```

**这条链的入口就是"根 compatible = J121AP"**（我们刚从 `xiaomi,fuxi` 改过来，并已实测：
`FUXI_DT: root compatible = J121AP (31 bytes)`）。

## 但 ABM 仍不打开 —— 结论与下一步

我们卡在这条链的**入口之后、第一次 open 之前**：桥的 22 个服务在、`/baseband` 带 `class=3`、
根 compatible = J121AP、固件两处就位、五处 union 全 5/0，**但没有任何 user client**。

参考文档说明这个阶段 CommCenter 会打印 **`No CommandDriver dylib`** 之类的原因 —— **我们还没看到它自己的日志**。
我们的日志工具已能运行（`FUXILOG: the job ran`），但它用 `OSLogStore` **从开机全量枚举**，一次要很久，
150 s 时文件里只有表头。

**下一步**：把枚举限制成**最近窗口**（例如最近 40 秒），只取 `CommCenter`/`telephony` 的行，
这样才拿得到它的真实抱怨。

## 2026-11-23 18:01 机型身份这条怀疑：已测，**排除**

把根 `model` 从 `iPhone15,2` 改成 `J121AP`（与根 `compatible` 一致），实机确认：

```
AMFI: queried model name from device tree: J121AP      ← 之前是 iPhone15,2
FUXI_DT: root compatible = J121AP (31 bytes)
FUXI_DT: /baseband compatible = baseband,n41 (13 bytes)
**ABM: 仍然没有任何 user client（sahara state 0）**
```

所以 **"机型是 iPhone 14 Pro"不是当前阻塞点** —— 身份已经和参考实现一样是 J121AP 了，ABM 仍不启动。

（`product` 节点仍是 iPhone 14 Pro：`product-name`/`product-id` 未动，界面身份不受影响。）

### 现在唯一的信息缺口

参考文档写这一步 CommCenter 会打印 **`No CommandDriver dylib`** 或类似原因（`hardware-tests.md:1629`），
而**我们看不到它的日志**。

我们的自有服务能跑、文件通道能用（`FUXILOG: the job ran`），但 `OSLogStore` **整段开机枚举**太慢，
150 s 时文件里只有表头。**下一步是把枚举改成最近窗口**（或让它跑得更久再由内核兜底读取）。

## 2026-11-23 18:17 型号选择：模块都在 CommCenter 里，表在 cryptex 里

### 主机侧查证（未上机）

```
CommCenter 内字符串：NoBBRadioModule ×4、C2KRadioModule ×4、CommandDriver ×43
（两个模块都在；没有任何"模块缺失"的痕迹）
libTelephony*.dylib：OS 卷里没有  → 在 Cryptex1,SystemOS（044-76034-038.dmg）里
```

**所以"选哪个模块"由一张表决定，而那张表不在我们已经挂载的 OS 卷里，在 cryptex 里。**

### 已排除的

- 根 `model` + 根 `compatible` 都已是 `J121AP`，AMFI 实测确认；**ABM 仍不启动** → 机型名不是当前阻塞。
- 固件两处（standalone + preboot）就位、五处 union 全 5/0 → 不是固件。
- 窗口化日志采集仍为空 → 这套系统里 `CommCenter` **不往统一日志写 telephony 条目**（或有别的去路），
  所以"看它自己的抱怨"这条路要换成别的手段。

### 下一步（主机侧，一次就能定论）

解出 `044-76034-038.dmg`（Cryptex1,SystemOS，3.3 GB），在里面的 `libTelephonyUtil` 上查：

- 表里有没有 `J121`（应当有）
- 有没有 `iPhone15,2`（应当没有）
- 选择键到底是什么（`HWModelStr` / MobileGestalt product type / `/baseband` 的 `compatible`）

这决定我们是"只差一个键"还是"表里根本没有这颗 SoC 的基带"——后者就只能走参考那种
**pinned CommCenter 兼容副本 + 自定义模块表**的路子。

## 2026-11-23 18:31 对照 Trogdor 的 `coachz_cellular_compat.py`：我们那份**补丁本身是对的**

```
ours (UniXNU)   4811 字节：只有基础补丁  0x100fa5a54 守卫 / 0x100fa5a78 B 0x1400000a
theirs          7536 字节：同一基础补丁 + 4 个 eSIM 补丁（0x10000e3ec、0x10000e5ec、0x10027dba4、0x1009aaa00）
```

**基础补丁完全相同**，而它的注释正是 **`B 0x100fa5aa0: continue normal context selection`** ——
也就是"继续正常的 context 选择"，这正是让 CommCenter 不走 NoBB 的那一处。
他们的 4 个额外补丁是 **eSIM** 用的，与 ABM 启动无关。

### `libTelephonyUtil` 不在 IPSW 里（两个卷都查过）

```
OS 卷：没有 libTelephony*.dylib
Cryptex1,SystemOS（044-76034-038.dmg）：也没有，只有 libCoreLSKD / libLogRedirect 等几个
CommCenter 内：NoBBRadioModule ×4、C2KRadioModule ×4、CommandDriver ×43、HWModelStr ×1，但**没有任何机型名**
```

**结论：没有"机型 → 模块"的静态表**；选择是运行时按 `HWModelStr` 之类的输入做的，
两个模块都在同一个二进制里。

### 另外一条：`model = J121AP` 会把用户态弄挂（已回退）

```
那一轮：CommCenter is NOT running; logd NOT running; launchd NOT running   ← 用户态没起
回退后：CommCenter running (pid 80); logd running (pid 25); launchd running (pid 1)
        根 compatible = J121AP（保留）
```

参考只改根 `compatible`、**从不改 `model`** —— 与他们的做法一致了。

### 下一步：不依赖 OSLogStore 的日志通道

`OSLogStore` 在我们这里一直拿到 0 条（连 `logd` 在跑也一样）。
下次可以**在我们的 manifest 里给 `com.apple.CommCenter` 加 `StandardErrorPath`/`StandardOutPath`**，
指向 Data 上的一个文件 —— CommCenter 的任何 `fprintf`/stderr 都会落到那里，再由内核读回来。
这条路完全绕开 `OSLogStore`，而且 manifest 我们已经能改（字节级追加）。

## 2026-11-23 18:50 给 CommCenter 加了 stderr 文件：文件建了，但它什么都不写

### 做法（字节级改 manifest，已验证）

`make-fuxi-manifest.py` 新增 `set_job_logging()`：追加一个带 `StandardOutPath`/`StandardErrorPath` 的
**新 job 字典**，把偏移表里旧对象的位置指向它（与参考"追加对象 + 改偏移"同一手法）。
主机侧验证：`com.apple.CommCenter` 条目带上了两个键，其余 561 条一字未动。

### 实测（`run-20261123-184201`）

```
FUXI_LOGDUMP: CommCenter wrote nothing at 150 s
FUXI_CC lines: 0
```

**文件被创建了（没有"no CommCenter log"报错），但 CommCenter 一个字节都没写。**
也就是说它**只用 os_log**，不走 stderr。

### 而 os_log 这条路在我们端口里是断的

- `OSLogStore` 的**无过滤**枚举同样 0 条（不只是 telephony 没写）
- `logd` 在跑（pid 25）
- 结论：**这个端口的 os_log 投递链本身没通**（`libsystem_trace` → `logd` 这条），
  这与基带无关，是端口层面的缺口。

**所以"读 CommCenter 自己的抱怨"这条路，在修好日志之前走不通。**

### 剩下的实验路径：把身份做成**完整的 J121**

参考的 `HWModelStr` 来自设备树，且他们的整棵树就是 **J121AP**（root compatible + product + product-id）。
我们目前是"`compatible=J121AP` + `model=iPhone15,2` + product-id 取 D72"的**混合身份**。

`model=J121AP` 单独改会把用户态弄挂（已实测），说明 **model 与 product-id 必须一致**。
所以要试就得**整套**：

```
root model      = J121AP
root compatible = J121AP
product-id      = J121AP 的 20 字节（来自 DeviceTree.j121ap.im4p）
```

**我们手上只有 J120（Wi-Fi）的 IPSW**，它的设备树里没有 J121 的身份。
`iPad7,2`（J121AP，iPad Pro 12.9" Cellular）的 17.7.11/21H461 IPSW 能提供它。

## 2026-11-23 19:00 机型名的两种取值：只有 **J120AP** 能起用户态

| 根 `model` | 用户态 | ABM |
|---|---|---|
| `iPhone15,2` | ✅ launchd/logd/CommCenter 都在 | ❌ |
| **`J120AP`** | ✅ **都在**（`launchd pid 1, logd pid 25, CommCenter pid 80`） | ❌ |
| `J121AP` | ❌ **用户态根本没起**（launchd 都没有） | — |

**这解释了我们之前的困惑**：`J121AP` 作为 `model` 会让系统起不来，但作为 `compatible` 没问题。
**现在身份与参考完全一致**：

```
model      = J120AP     （AMFI 实测确认）
compatible = J121AP
/baseband  = baseband,n41 + class=3 + euicc-chip-id=2
product.baseband-chipset = mav16
```

**但 ABM 仍然零 user client。**

### 现在的处境

参考的每一步我们都对上了，剩下的唯一差异只在 **CommCenter 内部的选择**，而它在我们的端口里**没有可观测通道**：

- `OSLogStore` 无过滤枚举：**0 条**（`logd` 在跑也没用，可能是 ad-hoc 签名的 entitlements 不被授予）
- `StandardErrorPath`：**文件建了但 CommCenter 一个字节都不写**（它只用 os_log）

### 可选的两条路

1. **修通日志**：查清 `OSLogStore` 为什么空（entitlement 是否生效、`logd` 的数据存到哪）；
   或让内核去读 `/private/var/db/diagnostics/**.tracev3`（内容是 LZ4，内核只有 zlib，需另找手段）。
2. **不靠日志的实验**：例如把兼容 CommCenter 的**eSIM 补丁也打上**（虽然与 ABM 无关），
   或试 `product-id` 换成 J121 的 20 字节（需要解 `bvx2` 压缩的 `DeviceTree.j121ap.im4p`）。

**建议先做 1**：没有 CommCenter 的自述，第 2 条只能继续盲试。

## 2026-11-23 19:10 机型/product-id 这条也查清了：**参考根本不设 product-id**

### 关键发现（`xnu_screen.py:108-125, 248`）

```python
FULL_SCREEN_IDENTITY = {
  "root":    {"model": "iPad14,1", "target-type": "J310"},      # iPad mini 6（Wi-Fi，无 Home 键）
  "product": {"product-name": "iPad mini (6th generation)",
              "compatible-device-fallback": "iPad7,3", ...},
}
# --baseband 时只额外做一件事：root compatible = J121AP
# product 节点从设备树导入，**没有 product-id 这个字段**
```

在整个参考仓库里搜 `product-id` / `product_id`：**零命中**。
也就是说他们的 `MGGetProductType` 大概率就是 `0xffffffff`（查不到），**而 ABM 仍然能启动**。

**结论：机型名 / product-id 都不是阻塞点。** 我们为此做的两次实验（`model=J120AP` 能起用户态、
`model=J121AP` 起不来）反而排除了这一整类变量。

### 顺带确认：参考也没有解 `bvx2` 的工具

`xnu_ipsw.py` 的 `im4p_payload()` 只切出第 4 段（对称于 trust cache/ramdisk，**不解压**）；
仓库里搜 `lzfse`/`bvx2`/`compression_decode` 全是零命中。所以我这边用 `libcompression` 解
`DeviceTree.j121ap.im4p` 失败，也不影响结论 —— **他们不需要那个值**。

### 今天这一轮真正验证过的（都已上机）

| 项 | 状态 |
|---|---|
| 自有 launchd 服务（新标签、manifest 字节级追加、签名侧车、信任缓存） | ✅ 跑起来了 |
| 固件两处（standalone + `<preboot>/<hash>`），34 个文件，挂载前写入 | ✅ 五处 union 全 5/0 |
| 端口自己的 `boot-manifest-hash`（IORegistry 读取，不写外来值） | ✅ |
| 根 `compatible = J121AP`、`/baseband` 带 `class=3`/`euicc-chip-id=2` | ✅ 实测 |
| 兼容 CommCenter（与参考同一基础补丁） | ✅ |
| `model` 取值 | `iPhone15,2` ✅能起 / **`J120AP` ✅能起** / `J121AP` ❌用户态不起 |
| `OSLogStore` 日志 | ❌ 全空 |
| CommCenter `StandardErrorPath` | ❌ 文件建了但零字节 |

**ABM 仍不启动**，而两条日志通道都不通 —— 这是现在唯一挡在面前的东西。

## 2026-11-23 19:14 身份整套移植成参考的 iPad mini 6：ABM 仍不启动

### 现在的身份（与 `xnu_screen.py:108-121` 的 FULL_SCREEN_IDENTITY 逐项一致）

```toml
[adt.root.string]
model = "iPad14,1"          # 参考值
target-type = "J310"
compatible = "J121AP"       # 参考在 --baseband 时设的值

[adt.product.string]
product-name = "iPad mini (6th generation)"
product-description = "iPad mini (6th generation)"
compatible-device-fallback = "iPad7,3"
artwork-display-gamut = "sRGB"
baseband-chipset = "mav16"

[adt.product.u32]
artwork-device-subtype = 2266   # 参考值
thin-bezel = 1
# product-id 已删除（参考不设）
```

`[os_overrides]` 里那条删除 `compatible-device-fallback` 的规则也去掉了（身份自带该值）。

### 实测（`run-20261123-191236`）

```
AMFI: queried model name from device tree: iPad14,1
FUXI_MODEM: CommCenter is running at 120 s (pid 80); logd running (pid 25); launchd running (pid 1)
COACHZ baseband: 124 s: 49 QMI services ... sahara state 0, 0 executes, 0 requests
**baseband user clients: 0**
```

**用户态正常、身份完全一致，ABM 仍不启动。**

### 因此可以确定：阻塞点**不在身份**

已经排除的整类变量：

| 变量 | 结论 |
|---|---|
| 根 `model`（iPhone15,2 / J120AP / J121AP / iPad14,1） | 四种都试过；`J121AP` 会弄挂用户态，其余三种用户态正常 |
| 根 `compatible` | J121AP，已实测进树 |
| `product` 身份（iPhone / iPad）与 `product-id` | 整套换成参考的 iPad 身份后仍无效 |
| `/baseband` 的 `class`/`euicc-chip-id` | 已加 |
| 固件两处、五处 union | 全 5/0 |
| 兼容 CommCenter | 与参考同一基础补丁 |

**剩下的差异只可能在"参考做了、我们没做"的其它环节**（兼容副本的校验、`/chosen` 的其它属性、
或 bridge 的行为差异），而它自己的日志通道在我们端口是断的（`OSLogStore` 全空、stderr 零字节）。

## 2026-11-23 19:37 ABM 的启动链：`abm-helper` 默认**根本不起**

### 从参考的里程碑总结里抽出的关键一句

> 根节点 `compatible` 用 J121AP 后 CommCenter **走高通基带分支，加载命令驱动并启动 ABM**

即：**ABM 是 CommCenter 拉起的**，不是 launchd 直接起的。

### 实测证据（`run-20261123-192519`）

```
FUXI_MODEM: CommCenter running (pid 80); logd running (pid 25); launchd running (pid 1);
            abm-helper NOT running (pid 0); CommCenterRootHelper NOT running (pid 0)
```

封存 manifest 里 `com.apple.abm-helper` 是 `KeepAlive={Crashed}`、**无 RunAtLoad**、**无设备树条件** ——
这种配置是**按需启动**：没人连它的 MachService，它就永远不跑。**这就是 ABM 不存在的直接原因。**

### 我们强行起它的实验结果（`run-20261123-192930`）

在 manifest 里加了一条 `org.unixnu.fuxi.abm`（`RunAtLoad`，直接跑 `abm-helper`）：

```
FUXI_MODEM: ... abm-helper running (pid 44); CommCenterRootHelper NOT running (pid 0)
**baseband user clients: 0**
```

**进程起来了，但它仍然不打开基带** —— 说明 `abm-helper` 只是前端，
真正的"启动基带"指令来自 **CommCenter 的命令驱动**。

### `CommandDriver` 在镜像里不存在

```
find OS 卷 -iname "*CommandDriver*"      → 0 个文件
grep -alr CommandDriver /usr/lib          → 0 个文件
CommCenter 内字符串 CommandDriver ×43     → 它是内置的类/错误文本，不是独立 dylib
```

所以"命令驱动"是 CommCenter **内部实现**，参考那句"加载命令驱动并启动 ABM"指的就是
**CommCenter 走高通分支后自己实例化它、再启动 ABM**。

### 结论

**ABM 的启动完全取决于 CommCenter 是否走高通分支** —— 而它自己的日志在我们端口不可见。
下一步必须把日志弄通（或找一个不依赖日志的观测点），否则只能继续盲试。

## 2026-11-23 20:18 读完 `docs/plans/modem.md`（保存为 `docs/refs/trogdor-modem.md`，51,798 字节）

### 文档里与"基带为什么没启动"直接相关的三条

**① ABM 的启动条件（:79）**

> `BootModule::boot_sync` 先问 `capabilities::abs::shouldBoot()`，返回 0 才走 "Faking bootup"；
> **这份正式版里它写死为 1**，所以 ABM 每次都真启动：`BootControllerLegacy::prepare` **检查
> `/usr/standalone/firmware/Baseband/` 里有固件**，`bootModem` 交给 BBUpdater。

→ 固件这条我们**已经满足**（`/usr/standalone/firmware/Baseband/Mav16` 5/0）。

**② J121AP 的 `/baseband` 节点内容（:81）**

> J121AP 有 `/device-tree/baseband`（`compatible = baseband,n41`，`AppleBasebandN71` 按它匹配；
> `euicc-chip-id = 2`；**bb_on、bb_rst、radio_on、coredump、reset_det 等 GPIO 功能；`config`**）、
> **`/arm-io/apcie/pci-bridge3/baseband-pcie`**，`/product` 有 `baseband-chipset = mav16`

→ **我们只有 `device_type`/`compatible`/`class`/`euicc-chip-id`；GPIO 功能、`config`、`baseband-pcie` 节点都没有。**

**③ MobileGestalt（:91）**

> 蜂窝能力主要从 IORegistry（`AppleBaseband` 服务、`/baseband` 节点）推出；身份不变，
> 让 MobileGestalt 在有基带节点的 ADT 上**重建缓存**（`coachz-reset-mgcache` 已有）。

→ 参考的做法是每次带基带启动时删掉缓存文件：
```
/private/var/containers/Shared/SystemGroup/
  systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist
```

### 我们把这条也实测了（`run-20261123-201355`）：**我们这里根本没有那个缓存**

```
/private/var/containers/Shared/SystemGroup                      answers 0   ← 存在
/private/var/containers/Shared/SystemGroup/...mobilegestaltcache answers 2   ← ENOENT
/private/var/containers/Shared/SystemGroup/.../Library/Caches     answers 2   ← ENOENT
/private/var/mobile/Library/Caches/com.apple.MobileGestalt.plist  answers 2   ← ENOENT
```

**所以"陈旧 MG 缓存"这条假设被排除**：容器不存在，MG 每次从 IORegistry 现算，而 `/baseband` 与
`AppleBaseband` 服务都在。

### 关于 `abmlite` 那句 `'abmtool' is not supported on this product.`

字符串在 **`abmlite` 自己**里（CommCenter 里没有），附近是：

```
abmtool.queue / abmtool / CommCenter / basebandd
''abmtool' is not supported on this product."
CommCenter process is not running. Please run/load CommCenter (or basebandd) and try again.
```

**所以它的含义是"bootstrap 里查不到 `abmtool` 服务"** —— 那个服务由 CommCenter 运行时创建，
说明 **CommCenter 还没走到创建它的那条分支**。

### 结论：还缺什么

| 缺的东西 | 依据 |
|---|---|
| `/baseband` 的 **GPIO 功能**属性（`bb_on`/`bb_rst`/`radio_on`/`coredump`/`reset_det`） | modem.md:81 |
| `/baseband` 的 **`config`** 属性 | modem.md:81 |
| **`/arm-io/apcie/pci-bridge3/baseband-pcie`** 节点 | modem.md:81 |

（固件、服务、身份、MG 缓存都不缺。）

## 2026-11-23 20:25 通读 `modem.md` 全文（238 行）后的结论：缺 `config` 和 GPIO 功能

### 文档给出的"ABM 能启动"的完整前提

| # | 前提 | 出处 | 我们 |
|---|---|---|---|
| 1 | `/baseband` 节点存在，launchd 才会选 `com.apple.CommCenter.plist`（`_LimitLoadToDeviceTree`） | :116 | ✅ |
| 2 | 该节点带 `compatible=baseband,n41`、`euicc-chip-id=2`、**GPIO 功能**、**`config`** | :81 | ⚠ **只有前两项** |
| 3 | 根 `compatible = J121AP` → CommCenter 走高通分支 | :118 | ✅ 实测 |
| 4 | `/usr/standalone/firmware/Baseband/` 里有固件（`BootControllerLegacy::prepare` 的检查） | :79 | ✅ 5/0 |
| 5 | `AppleBaseband`、`AppleBasebandPCIControl`、通道服务按 `ABPInterfaceProtocol` 匹配 | :99/103 | ✅ 22/22 |
| 6 | 每次启动后桥用 DMS 0x002d/0x002e 把基带设成低功耗，CommCenter 才把它带到在线 | :232-238 | ✅ 已移植 |
| 7 | MobileGestalt 缓存里要有蜂窝能力 | :91 | ✅ 实测：我们**根本没有** MG 缓存，现算 |

**1、3–7 全部满足。唯一缺的是 2 里的 `GPIO 功能` 与 `config`。**

### 我们自己的档案里也写着同一件事

`hakios-umi-baseband-档案.md:719`：

> **`/baseband` 节点需要 `class` 属性和 ≥24 字节的 `config`**，否则 lockdownd 报
> "no class property of IODeviceTree:/baseband"

`class` 我们上一轮已加，**`config` 一直没加**。

### 实测侧的证据也指向这里

```
FUXI_ABMLITE 150 s: 'abmtool' is not supported on this product.
```

字符串在 `abmlite` 里，含义是 **bootstrap 查不到 `abmtool` 服务** —— 该服务由 CommCenter 运行时创建。
也就是说 **CommCenter 没有走到"基带管理器"那一步**，而它是唯一还没被满足的结构性前提（`config`/GPIO）。

### 下一步

给 `/baseband` 加 **`config`**（先按 ≥24 字节试），再上机看 `abmtool` 与 ABM 是否出现。
GPIO 功能需要 GPIO 控制器 phandle，我们 ADT 里没有对应节点，先不动。

## 2026-11-23 20:33 给 `/baseband` 加 `config`（24 字节零值）——**没有变化**

```
FUXI_ABMLITE 150 s: 'abmtool' is not supported on this product.
FUXI_ABMLITE 210 s: 'abmtool' is not supported on this product.
baseband user clients: 0
```

所以要么 `config` 的内容有语义（零值不算数），要么它根本不是那道门。
**这条实验只有"负结果"，但把 `config` 这个变量消掉了。**

### 现在真正能推进的两条路

**① 反汇编 CommCenter 的 radio 模块选择**（推荐）

我们手上就是参考固定过的那颗二进制（sha256 `383779ed…`，与其 `SOURCE_SHA256` 完全一致）。
`NoBBRadioModule` / `C2KRadioModule` 各出现 4 次，`HWModelStr` 只被调用 1 次 ——
只要找到**实例化 C2KRadioModule 的那个分支**，就能读出它的条件（是 MobileGestalt 的哪个键、
还是 `/baseband` 的哪个属性），不必再猜。参考当年就是这么干的（`modem.md:75` 第二轮调研整节都是反汇编结论）。

**② 修通日志**

`OSLogStore` 全空、`StandardErrorPath` 零字节 —— 端口层面的 os_log 投递链没通，
没有它就只能靠 ① 这种静态手段。

## 2026-11-23 20:47 照抄 J121AP 的 GPIO 功能 → **卡死总线**（已回退）

### 事故

把 J121AP 的真实 `function-*` 属性原样写进 `/baseband` 后，设备**卡死总线**。

### 原因（正是"必须用小米 13 自己的 GPIO"）

解出来的属性值形如：

```
function-bb_on   = 56000000 4f495047 05000000 01010000   ← phandle 0x56, "OIPG", 引脚 5, 标志
function-bb_rst  = 24000000 4f495047 15000000 00000000   ← phandle 0x24
function-radio_on= 24000000 4f495047 10000000 00000000   ← phandle 0x24
```

**这些 phandle 指向苹果板子的 GPIO 控制器节点**（0x24/0x55/0x56/0xcb/0xd0…），
而我们的 ADT 里**连一个 GPIO 控制器节点都没有**（整个硬件设备树只有 0x01、0x02 两个 phandle，
没有 gpio/tlmm/pinctrl 节点）。于是这些属性成了**悬空引用**，有代码去解引用就**卡死总线**。

### 所以正确做法

1. **不能用苹果板子的 GPIO 值**（会崩）——这就是你指出的点。
2. **要用也只能用 fuxi 自己的**：需要在 ADT 里先加 **TLMM 控制器节点 + 合法 phandle**，
   再填 SM8550 的真实引脚号。**我们今天两者都没有。**
3. **而且在我们的架构里这些属性没有消费者**：fuxi 的基带供电/复位走 **PAS/PIL + PMIC 稳压器**，
   由**桥**负责（modem 已在跑：TFTP 178、RMTFS 5120 读写、QRTR 服务 49 个），
   不经过这些引脚。

**结论：`function-*` 一律不加；`config`（真实 10 个 u32）保留。**

### 回退后的状态（`run-20261123-204235`）

```
FUXI_MODEM: CommCenter running (pid 82); logd running (pid 25); launchd running (pid 1); abm-helper running (pid 45)
COACHZ baseband: 105 s: the card's slot status: error 0, 5 TLVs
```

设备恢复正常启动，`/baseband` 只剩：`device_type`、`compatible`、`class=3`、`euicc-chip-id=2`、`config`（真实值）。

## 2026-11-23 20:55 对照 UMI 的做法：**GPIO 根本不该进 DT**

### UMI 合成的节点（`hakios-umi-baseband-档案.md:95-102`）

```
/baseband   device_type=baseband, compatible=baseband,n41, class=3, config=<10×u32>
/product/baseband-chipset = "mav17"
```

**没有 `function-*`、没有任何 GPIO 属性。**

> launchd 的 CommCenter 作业带 `_LimitLoadToDeviceTree=baseband`，所以这个节点是 cellular CommCenter 被加载的
> **唯一开关**。`/baseband` 节点还必须带 `config` 属性（至少 24 字节），否则
> `libTelephonyBaseband` 的 **`_TelephonyBasebandControllerLoadConfigOnce`** 会报 "no class property"。

**这就把 `config` 的消费者指名道姓地说出来了** —— 我们保留真实 `config`（10×u32）是对的。

### UMI 的 GPIO 怎么用（:110-133）

> **这一层完全不做 DT 匹配，也不读 DT 属性。** 所有硬件地址是编译期常量 ……
> **换板子必须逐个核对这组地址、TLMM pin 号、PCIe controller/PHY 位置，不能靠 DT 描述符改。**

```
| Pin | 方向   | 含义              |
| 56  | 输出高 | AP2MDM_STATUS    |
| 57  | 输出低 | AP2MDM_ERRFATAL  |
| 3   | 输入   | MDM2AP_STATUS    |
| 1   | 输入   | MDM2AP_ERRFATAL  |
| 85  | 输出低 | PERST#           |
```

**GPIO 是驱动按引脚号直接操作，不写进设备树。**

### 所以昨天那次卡死是可以解释的

我们照抄 J121AP 的 `function-*= <phandle, "OIPG", pin, flags>`：那些 **phandle 指向苹果板子的控制器**，
我们 ADT 里没有 —— 而 fuxi 的正确做法（UMI 的做法）本来就不该有这些属性。

### 我们现在的节点与 UMI 完全同形

```
/baseband   device_type=baseband, compatible=baseband,n41, class=3, euicc-chip-id=2,
            config=<10×u32 真实值>
/product/baseband-chipset = "mav16"
```

（`euicc-chip-id=2` 取自 J121AP；UMI 那颗没有，保留无害。）

## 2026-11-24 07:05 移植 UMI 的"把 /baseband 节点复制到服务上"——**完成并上机验证**

### 参考做法（`hakios.umi_baseband.cpp:1445-1461, 1510`）

```cpp
template <class Service>
static Service *bb_publish(IOService *provider, uint32_t channel = ~0u, const char *node = nullptr)
{
  ...
  if (ok && node) {
    IORegistryEntry *dt = IORegistryEntry::fromPath(node, gIODTPlane);
    OSDictionary *props = dt ? dt->dictionaryWithProperties() : nullptr;
    ...每个属性 setProperty 到服务上...
    service->setName(dt ? dt->getName() : "baseband");
  }
  ...
}
made += provider && bb_publish<AppleBaseband>(provider, ~0u, "/baseband");
```

> libTelephonyBaseband 用 **`IOServiceNameMatching("baseband")`** 找板子，并读它的 **`config`**
> （`_TelephonyBasebandControllerLoadConfigOnce`）

### 我们的移植（`coachz_baseband.cpp`）

`bb_publish` 增加 `node` 参数，按参考逐属性复制并 `setName`；`AppleBaseband` 以 `"/baseband"` 发布。

### 上机验证（`run-20261124-065446`）

```
COACHZ baseband: AppleBaseband took 6 properties from IODeviceTree:/baseband (config present), name baseband
```

**6 个属性复制成功、`config` 在、服务名 = `baseband`** —— 这正是 `IOServiceNameMatching("baseband")` 要找的。

### 但 ABM 仍未打开服务

```
# com.apple.basebandd.xpc REGISTERED      ← launchd 静态注册，不代表进程
# abmtool absent
# demand com.apple.abm.helper.wireless started
# demand com.apple.abm.helper.root started
FUXI_LOGDUMP: no /private/var/db/abm-helper.log at 150 s (2)   ← 该 job 从未 spawn
FUXI_LOGDUMP: /private/var/db/abm.log wrote nothing at 150 s   ← 直接跑也零输出
baseband user clients: 0
```

### 两条新线索

1. **`abm-helper` 这个 job 根本没被 spawn**（连日志文件都没创建）。已排除：`_wireless` 用户在 sealed passwd 里**存在**
   （`_wireless:*:25:25:Wireless Services:/var/wireless:/usr/bin/false`）。
   下一个怀疑点：它的 home `/private/var/wireless`（Data 上）是否存在、以及 launchd 的 spawn 失败原因。
2. **直接跑 `abm-helper` 也零输出** —— 它静默退出。

## 2026-11-24 08:40 两个实验的结果

### 实验一：根 compatible 改成"替换"（只留 `J121AP\0`，7 字节）

```
FUXI_DT: root compatible = J121AP (7 bytes)
FUXI_MODEM: CommCenter running (pid 82); logd running (pid 25); launchd running (pid 1)
**baseband user clients: 0**
```

**系统照常启动**（用户态全在）→ 替换是安全的，不需要回退。
**但 ABM 仍不打开服务 → 按判据：31 字节不是问题，转下一层。**

### 实验二：以 `_wireless` 身份 fork+exec `abm-helper`，记录退出码

```
# user _wireless    present
# user _assetcache  MISSING        ← 镜像里本来就没有（sealed passwd 也没有），与本次无关
# abm-helper exited: exit 0 signal 0
# /private/var/wireless mkdir -1 chown 0    ← 目录本来就存在
FUXI_LOGDUMP: /private/var/db/abm-helper.log wrote nothing at 150 s   ← 零输出
```

**结论：不是二进制/权限问题。** `abm-helper` 能被 exec、以 `_wireless` 运行、**退出码 0、零输出**。
它是 XPC 服务，离开 launchd 的 job 上下文就无法注册 `com.apple.abm.helper.wireless` 监听器，于是立刻退出。

### 仍卡在同一处：**launchd 不 spawn 这条 stock job**

```
# demand com.apple.abm.helper.wireless started     ← lookup 成功
FUXI_LOGDUMP: no /private/var/db/abm-helper.log at 150 s (2)   ← 但连日志文件都没建 → 没有 spawn
```

已排除：`_wireless` 用户存在、`/private/var/wireless` 存在。

### 下一步：在核心里给 execve/posix_spawn 加打印

唯一还没看过的是**launchd 到底有没有尝试 exec**。内核的 `posix_spawn`/`execve` 路径加一行打印
（只打印路径里含 `abm-helper`/`CommCenter` 的），就能区分：

- **有打印、exec 失败** → launchd 尝试了，失败在内核侧（权限/签名/路径）
- **完全没有打印** → launchd 自己就没走到 spawn（job 配置/沙箱/条件判断）

## 2026-11-24 09:55 证书（`.cer`）调查：**整个系统在启动路径上不开任何 `.cer`**

### 做法：在内核 `vnode_open` 里给 `.cer` 打开加打印（`patches/xnu/fuxi-cer-probe.patch`）

```c
static uint32_t cer_opens;
if (path && strnstr(path, ".cer", strlen(path)) && __atomic_fetch_add(&cer_opens, 1, __ATOMIC_RELAXED) < 20) {
    printf("FUXI_CER: open %s\n", path);
}
```

### 上机结果（`run-20261124-095155`，用户态全起：CommCenter pid 82、abm-helper pid 89）

```
FUXI_CER: 0        ← 一次都没有
```

### 之前的静态搜索也全是空

| 查的位置 | 结果 |
|---|---|
| 内核 kernelcache（16 MB） | 无 `.cer`/certificate/credential 字符串 |
| `CommCenter` / `abm-helper` / `CommCenterRootHelper` | 无 `.cer` 路径（只有 `ps.cer` 片段） |
| dyld 共享缓存（cryptex 里各分片） | 只有 `certs.apple.com/appleistca2g1_*.cer` 这种**下载 URL** |
| OS 卷里的 `.cer` 文件 | **0 个**（`/System/Library/PrivateFrameworks` 全扫） |
| Mav16 固件包（`.bbfw`，22 项） | 只有 `vinyl/update/{main,gold}/{firmware.der,signedprofile.der}`（已随固件写入） |

**结论：这份镜像的启动路径上没有任何组件打开 `.cer`。**

### 与基带证书相关的、真实存在的两项（已有/可补）

1. **`firmware.der` / `signedprofile.der`**：在 `.bbfw` 里，我们的解包器**全部 22 项都写**到固件目录 ✓
2. **`apticket.der`**（个性化票据）：`/usr/standalone/firmware/apticket.der` 在镜像里**缺失**（OS 卷只有悬空符号链接），
   如果确实有组件要它，可以按需生成/放置

**下一步需要的信息**：那个"读本机 `.cer`"的**确切路径**（或来源项目/日志）。有了路径，
探针可以直接验证"放了文件之后是否真的有人打开它"，再决定伪造内容。
