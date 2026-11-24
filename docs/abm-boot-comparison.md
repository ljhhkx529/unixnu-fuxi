# ABM 启动链对照：Apple 原生 / Trogdor 参考 / fuxi 现状

2026-11-24。对象是 **iPadOS 17.7.11（21H461）** 上的 `CommCenter` / ABM / `abmtool` / `abm-helper`，
以及 `unixnu-fuxi` 在小米 13（fuxi，SM8550，内置 MPSS）上已经把 Qualcomm 侧跑通的那座桥。

本文回答三件事：

1. **ABM 启动时，Apple 原生、Trogdor 参考、fuxi 这三条路各自发生什么**；
2. **fuxi 还差哪些东西**，按“挡住 ABM”“ABM 起来后才会遇到”“数据面”“用户态”分层；
3. **你贴的那 14 行日志到底说明了什么**，以及下一步用哪几个实验把剩下的假设分开。

> 代码级对照与根因排序见 [cellular-unavailable-analysis.md](cellular-unavailable-analysis.md)。

证据标记：

| 标记 | 含义 |
|---|---|
| **【代码】** | 读本机源码得到，给出 `path:line`，未上机 |
| **【实测】** | 设备日志 / `evidence/` / 本文引用的那次 run 直接支持 |
| **【文档】** | 仓库文档或参考实现的记录，本文未重新上机复核 |
| **【推断】** | 本文的分析，明确标出，不作为事实 |

> 先把边界说清：`uefi-fuxi` 是 SEC/DXE/BootShim 那一层，负责把 XNU 送进 EL2/EL1；ABM 在 XNU 的
> 用户态里，和 UEFI 没有调用关系。**ABM 起不来不是固件层的问题**，所以下面不再提 `uefi-fuxi`。

| 仓库 | 负责 | 与 ABM 的关系 |
|---|---|---|
| `uefi-fuxi` | SEC/DXE/BootShim、内存图、payload 卷、把 XNU 送进 EL2/EL1 | 无。交棒后不再参与，ABM 还没开始 |
| `unixnu-fuxi`（本文所在） | 板级 DTS/ADT、`board.toml`、内核补丁、manifest、`fuxi-logdump`、PAS 探针 | 提供 ABM 要的 `/baseband`、固件、服务发布和可观测性 |
| `UniXNU` 的 `QcomBaseband` | `coachz_baseband.cpp`、QMUX/SAHARA/control/PDP | ABM 直接面对的 IOKit 桥 |


---

## 0. 结论摘要

1. **ABM 不是 `abm-helper` 的同义词。** 这份 21H461 的 `CommCenter` 二进制弱链接
   `AppleBasebandManager` 与 `ABMHelper`，导入 `abm::InitServer` / `abm::StartServer` /
   `abm::HelperClient`；ABM 的主宿主是 `CommCenter`，`abm-helper` 是它按需拉起的辅助 XPC
   进程。**【代码】**（`/tmp/compat/CommCenter` 的 `nm -m` 与 `otool -L`）
   所以“`abm-helper` 进程在不在”不能当作“ABM 接管了基带”。

2. **三条路的 Qualcomm 侧和 Apple 侧是分开的两件事。** fuxi 的 Qualcomm 侧已经完整：
   PAS stage 9、GLINK `IPCRTR`、QRTR 52 个服务、servreg locator 4/4、RMTFS/TFTP、SIM 卡状态
   都通。**【实测】**（你贴的 150 s 行、`docs/ios-baseband.md:1298-1350`）
   Apple 侧则停在 **`CommCenter` 决定走蜂窝分支之后、ABM 第一次打开 `AppleBaseband` 之前**。

3. **桥已经“ABM-ready”，但从未被 ABM 碰过。** `AppleBaseband` 已按参考做法从 `/baseband`
   继承 6 个属性、命名为 `baseband`；`AppleIPAppender`、`AppleBasebandPCIMAVPDPManager`、
   18 个通道、`matchPropertyTable`、1024 帧队列、SAHARA 设备端、BHI 寄存器、低功耗 DMS
   握手全在。**【代码】**（`coachz_baseband.cpp:1388-1445`、`coachz_bb_sahara.h`、
   `coachz_bb_control.h`）但日志里 **0 个 user client、sahara 0 次请求**，说明 ABM 连门都没敲。

4. **已经排除的不是“少一个属性”。** 固件两处、`/baseband` 的 `class`/`config`/`euicc-chip-id`、
   `baseband-chipset=mav16`、根 `compatible=J121AP`、机型 `iPad14,1`、`AppleBaseband` 的命名和
   属性、兼容 CommCenter、`sheng-no-lc` 放行、manifest 签名，都已经上机验证过。**【实测】**
   （`docs/ios-baseband.md:1298-1350`）仍不动，说明剩下的差异在 **模块选择** 或 **ABM 决定
   启动基带之前的某个前置条件**，不在桥的 ABI 里。

5. **你的日志里最容易被误读的两行**：
   - `# com.apple.basebandd.xpc REGISTERED` 只证明这个名字在 bootstrap 里，不证明托管它的进程活着；
   - `# demand com.apple.abm.helper.wireless started` 只证明 `bootstrap_look_up` 成功、launchd
     接受了按需启动请求，不证明 job spawn 成功；`no /private/var/db/abm-helper.log` 恰好说明
     那个被加了日志的 job 没有真正跑起来（或者日志加在了另一条 plist 上）。

6. **`abmlite` 那句不是“这台产品不支持 ABM”**，是“bootstrap 里找不到 `abmtool` 服务”。
   字符串在 `abmlite` 自己里，`abmtool` 由 `CommCenter` 的 ABM 命令接口运行时创建。它证明的
   是 **ABM 的命令面还没起来**，不是硬件/产品不支持。**【实测】**+**【文档】**
   （`docs/ios-baseband.md:1123-1134`）

---

## 1. ABM 是什么，谁托管，几个名字分别指谁

这一节先把名词钉死，否则后面每一步都会被 `abm-helper` / `basebandd` / `abmtool` / `abmlite`
四个名字绕进去。

| 名字 | 是什么 | 谁创建 | 在本端口怎么观测 |
|---|---|---|---|
| **ABM** | AppleBasebandManager。负责“给基带上电、跑 BBUpdater、把基带带到 booted、再把 QMI 交给 CommCenter”的状态机 | `CommCenter` 里的 `abm::InitServer`（弱链接 `AppleBasebandManager`） | 只能通过它打开 `AppleBaseband` / `pbl` / `sahara` / `qmux` 的 user client 间接看 |
| **`abm-helper`** | `ABMHelper.framework/Support/abm-helper`，ABM 的辅助 XPC 进程；`CommCenter` 通过 `abm::HelperClient` 使用它 | launchd，按 MachService 按需启动 | `abm_helper` 进程、它的日志文件、`com.apple.abm.helper.*` 服务 |
| **`com.apple.basebandd.xpc`** | ABM 对外的 MachService 名（umi 的记录把它描述为 CommCenter 内的 `basebandd`） | ABM / CommCenter | `bootstrap_look_up` 是否成功；**不证明进程在跑** |
| **`abmtool`** | ABM 的命令服务，`abmlite` 连它就是连这个 | ABM 的命令接口，运行时注册 | `bootstrap_look_up("abmtool")`；`abmlite status` 的输出 |
| **`abmlite`** | `/usr/bin/abmlite`，命令行客户端；查 `abmtool`，查不到就打印 `'abmtool' is not supported on this product.` | 系统自带 | 我们自己的 launchd job 跑它，stdout 落到 `/private/var/db/abmlite.log` |

【代码】`/tmp/compat/CommCenter`：

```text
$ nm -m CommCenter | grep -i abm
(undefined) weak external automatically hidden __ZN3abm10InitServerEPKcRKNS_9OverridesE (from AppleBasebandManager)
(undefined) weak external automatically hidden __ZN3abm11StartServerEP16dispatch_group_s (from AppleBasebandManager)
(undefined) weak external automatically hidden __ZN3abm12HelperClient6createEPKcNSt3__110shared_ptrIN3ctu9LogServerEEE (from ABMHelper)
(undefined) weak external automatically hidden __ZN3abm12HelperClient7performENS_6helper6TaskIDEPKcN3xpc4dictERS6_ (from ABMHelper)
...
$ otool -L CommCenter | grep -i abm
/System/Library/PrivateFrameworks/ABMHelper.framework/ABMHelper (compatibility version 1.0.0, current version 0.0.0, weak)
```

`strings` 里同时有 `#I ----------------- Starting C2KRadioModule ----------------` 与
`#I ----------------- Initializing NoBBRadioModule ----------------`，以及
`#I Cannot host ABMHelper`、`#I ABM Server is not available. So, dump CommCenter state alone and proceed.`
这四句就是判定模块选择和 ABM 托管失败的四个观测点。**【代码】**

> **结论**：判断“ABM 起没起”要看 **它有没有打开 `AppleBaseband`**，而不是看 `abm-helper` 的
> 进程号、也不是看 `basebandd.xpc` 的注册位。参考实现在 ABM 第一次打开 `AppleBaseband`
> 之后才崩溃（`ApplePDPHelperInterface::enableMetadataTrailer`，`this == null`），那时
> `abm-helper` 根本不需要存在。**【文档】**（`Trogdor-ipadOS/docs/hardware-tests.md:1629-1637`）

### 1.1 ABM 与 ABMLite 不是两个基带管理器

这是你的日志里最容易混淆的一对。两者不是“完整版/精简版”的关系：

| | **ABM**（AppleBasebandManager） | **ABMLite / `abmlite`** |
|---|---|---|
| 形态 | `CommCenter` 进程内的库/状态机 | `/usr/bin/abmlite`，独立命令行程序 |
| 作用 | 给基带上电、跑 BBUpdater、开 `AppleBaseband`/`pbl`/`sahara`/`boot`/`qmux`、把 QMI 交给 CommCenter | 通过 `abmtool` 查询/控制 ABM 的命令接口；**自己不碰内核、不碰基带** |
| 依赖 | 固件、`AppleBaseband` 服务、SAHARA/BHI/QMUX 桥 | `abmtool` MachService（ABM 运行时创建） |
| 失败表现 | 0 个 `AppleBaseband` user client、sahara 0 | `'abmtool' is not supported on this product.` |
| 能否证明“ABM 起了” | 能（打开 user client 是硬证据） | 只能证明“命令面起了/没起” |

所以 `FUXI_ABMLITE` 那一行是**下游观测**：它说明 `abmtool` 不在；`abmtool` 不在又说明 ABM 的
命令接口没起来。它既不能证明“产品不支持 ABM”，也不能单独证明“ABM 完全没启动”——ABM 可能
已经初始化但停在打开基带之前。要区分这两种，仍然要看 `AppleBaseband` 的 user client。

---

## 2. 原生 Apple 的 ABM 启动序列

这一节是“正常蜂窝机型上应该发生什么”，来源是 Trogdor 对 21H461 的三轮反汇编与实机记录，
不是本文的推断。**【文档】**（`docs/refs/trogdor-modem.md:79-107`、
`Trogdor-ipadOS/docs/hardware-tests.md:1629-1656`）

### 2.1 启动前的四个条件

| # | 条件 | 消费者 | 失败时的样子 |
|---|---|---|---|
| 0 | 设备树有 `/baseband` 节点（`_LimitLoadToDeviceTree: baseband`） | launchd | 加载 `com.apple.CommCenter-noncellular.plist`，CommCenter 以无基带模式运行 |
| 1 | 根 `compatible` 是蜂窝机型名（J121AP） | `libTelephonyUtil` 的 `HWModelStr` → `__TelephonyRadiosDetermineRadio` | `No CommandDriver dylib was loaded`、`'NullFactory' instantiated`、`kNoBaseband` |
| 2 | `/usr/standalone/firmware/Baseband/` 与 `<preboot>/<hash>/...` 有固件 | `BootControllerLegacy::prepare` | `BBUpdater error 2 - Assertion failure ( fPhases.size() )`、`Miss images for bootupInternal` |
| 3 | `AppleBaseband` 服务可被 `IOServiceNameMatching("baseband")` 找到，且带 `config` | `libTelephonyBaseband` 的 `_TelephonyBasebandControllerLoadConfigOnce` | `no class property of IODeviceTree:/baseband`、ABM 不打开 |

第 1 条是参考实现里“根 compatible 换 J121AP 后 CommCenter 不再报 No CommandDriver、走
C2KRadioModule、ABM 启动”的那一步。**【文档】**（`docs/refs/trogdor-modem.md:116-118`）

### 2.2 启动序列（正常蜂窝机型）

| 步 | 谁做 | 做什么 | 内核/桥要回答什么 | 日志里的样子 |
|---|---|---|---|---|
| 1 | launchd | 因 `/baseband` 选 cellular job，启动 CommCenter | — | `Starting C2KRadioModule` |
| 2 | CommCenter | 初始化 ABM（`abm::InitServer`），注册 `basebandd.xpc`、`abmtool` | — | `ABM Server ...` |
| 3 | ABM | `BootControllerLegacy::prepare` 检查固件、`baseband-chipset` | 无 | `FW base path ... exists` |
| 4 | ABM | 打开 `AppleBaseband`（方法 6 断电重启、15 self-identify），再打开 `AppleIPAppender`、`AppleBasebandPCIMAVPDPManager` | `AppleBaseband` 的 selector 表、appender 的 0–20 号方法 | `AppleBaseband user client`、`AppleIPAppender selector ...` |
| 5 | ABM → BBUpdater | 打开 `pbl`，`readRegister` 0/3/4（序列号、MSM ID、PK hash），`SendImage` SBL | 方法 5 的寄存器内容、方法 4 收下即丢 | `CHIPID=0x9540e1`、`SendImage ...` |
| 6 | BBUpdater | 打开 `sahara`，等 HELLO，回 mode 3/0，按设备请求传镜像，DONE | SAHARA 设备端状态机 | `Hello received boot mode`、`request image ID`、`All images transferred` |
| 7 | BBUpdater | 打开 `boot`，`Connecting to Loader`、`Getting Version`；基带进入 booted | `boot`/`refs` 通道（参考实现在这一步通过） | `Baseband state: booted`、`IPC state ... kEngaged`、`engage 24 ms` |
| 8 | ABM | 打开 `qmux1`/`qmux2`，QMI CTL 建客户端 | QMUX↔QRTR、CTL GetClientId/Release/Sync | `qmux1 opened`、QMI 首帧 |
| 9 | ABM/CommCenter | 读工作模式；刚启动的基带应为低功耗；CommCenter 再带到 online | DMS 0x2d/0x2e | `Baseband booted in airplane mode`、`operating mode ...` |
| 10 | CommCenter | UIM 卡状态、NAS 注册、BSP 激活、WDS 数据口 | QMI 各服务 | `kSimStateReady`、`kRegistered`、`kActivated` |

注意第 4 步：参考实现第一次跑时 ABM **立即 SIGSEGV**，因为 `AppleIPAppender` 没发布，
`ApplePDPHelperInterface::getInstance()` 返回空，`enableMetadataTrailer` 直接解引用。
这发生在 ABM 已经打开 `AppleBaseband` 之后。**【文档】**
（`Trogdor-ipadOS/docs/hardware-tests.md:1630-1637`）

第 5 步的 BHI 寄存器（`coachz_bb_sahara.h:93-105`）：

| 寄存器 | 内容 | 值 |
|---|---|---|
| 0 | serial number | 固定 `0x0c0ac42b`，不输出真实序列号 |
| 3 | MSM hardware id | offset 4 放 chip id `0x009540e1`（Mav16） |
| 4 | PK hash | 默认未熔断值，ABM 才会接受非个性化镜像 |

第 6 步的 SAHARA 报文（`coachz_bb_sahara.h:21-47`）：HELLO 1、HELLO_RESP 2、READ_DATA 3、
END_IMAGE_TX 4、DONE 5、DONE_RESP 6、COMMAND_READY 0xb、SWITCH_MODE 0xc、EXECUTE 0xd。
BBUpdater 是**被动**的：它等设备 HELLO，按设备的 READ_DATA 发数据。所以桥要扮演的是
“已经启动的基带”，而不是“下载器”。

---

## 3. Trogdor 参考实现：同一座桥，怎么被 ABM 接受

Trogdor（Coachz/SC7180）的基带是 SoC 内 MPSS，和 fuxi 同类；它没有 PCIe/MHI，也没有真的
让 Apple 固件跑起来，而是用 **同一个 `coachz_baseband.cpp` 思路** 演一台“已经 booted 的
Apple PCIe 基带”。它的实机记录是 fuxi 最好的对照，因为它把“ABM 从拒绝到接受”的每一步都
踩过一遍。**【文档】**（`Trogdor-ipadOS/docs/hardware-tests.md:1629-1656`、
`platform/coachz-prelink/pending/baseband/README.md`）

### 3.1 参考实现踩过的六道门（按时间顺序）

| # | 现象 | 根因 | 修法 | fuxi 对应 |
|---|---|---|---|---|
| 1 | `No CommandDriver dylib`，CommCenter 走 NoBB | 根 `compatible` 不是蜂窝机型 | 根 `compatible = J121AP` | ✅ 已做（31 字节，首串 J121AP） |
| 2 | ABM 启动即 SIGSEGV `far 8` | 缺 `AppleIPAppender`，`ApplePDPHelperInterface` 为空 | 发布 `AppleIPAppender` + `AppleBasebandPCIMAVPDPManager`，应答方法 6/15 | ✅ 类已发布（`coachz_baseband.cpp:1314-1381`） |
| 3 | `Baseband is declared dead`，`BBUpdater error 2 fPhases.size()` | 固件目录不在 ABM 读的 personalized 路径 | 把 Mav16 写到 `<preboot>/<hash>/usr/local/standalone/firmware/Baseband/Mav16` 并 bind | ✅ 34 文件、两树 5/0（`fuxi-baseband-addon-mount.patch`） |
| 4 | `kBBUReturnGetChipIDFailure` | BHI 方法 5 未实现 | 方法 5 回 serial/chip id/PK hash | ✅ `cbs_read_register` 已实现 |
| 5 | `failed receiving sahara hello` 40 s | `pbl` 的 `SendImage` 后没有在 `sahara` 发 HELLO | sahara 打开时发 HELLO | ✅ `bb_sahara_restart`（`coachz_baseband.cpp:324-334`） |
| 6 | 第二路 qmux 打开报独占，`Engage failure` | `ABPInterfaceProtocol` 是 IOKit 不比较的键 | `matchPropertyTable` 比较它 | ✅ `coachz_baseband.cpp:1149-1161` |

第 6 条之后，参考实现才走到 `Sending Image 'SBL' 100%`、`Baseband state: booted`、
`IPC ... kEngaged`、`engage 24 ms`、`qmux1 收发 262/270 帧`。**【文档】**

### 3.2 参考实现和 fuxi 的机制差异

| 机制 | Trogdor | fuxi | 影响 |
|---|---|---|---|
| 固件投递 | 内核在挂载前写一份，bind 挂到两处（folder→树内挂载点→整树） | 内核写两份（standalone + preboot），各自 union；用户态守护进程再解 17 个并硬链接到 preboot | fuxi 更简单，但**多了一次重复写入**；`preboot mirror 1` 说明用户态也成功了 |
| bind 嵌套 | 支持两级 bind | 不支持（实测 4/2），改为两棵树各写一份 | 已绕开（`docs/ios-baseband.md:639-704`） |
| 发布时机 | GLINK 起来后由 Wi-Fi 线程发布 | QRTR 链路 up + remote node known 后由 `fuxi_modem_poll` 发布（`fuxi_modem.c:972-978`） | **fuxi 的发布可能晚于 CommCenter 的 30 s 检查**，这是本文要测的假设之一 |
| `boot` 通道 | 实机通过 | 只有 pbl/sahara/qmux1/qmux2 有实现，`boot`/`refs` 是“能打开、读不到数据”的桩（`coachz_baseband.cpp:778`） | ABM 打开之后可能撞上的下一堵墙 |
| 固件数量 | 17 个 | 17 个（bbfw）+ 内核写的 34 个 | 内容同源 |

### 3.3 参考实现给 fuxi 的最重要一句话

> ABM 是 **CommCenter 拉起的**，不是 launchd 直接起的；根 `compatible` 只决定“CommCenter 走
> 不走蜂窝分支”，走进去之后 ABM 自己会去 open `AppleBaseband`。**【文档】**
> （`Trogdor-ipadOS/docs/hardware-tests.md:1629`）

所以 fuxi 现在的观测（CommCenter 在跑、`basebandd.xpc` 注册、`abmtool` 不在、0 user client）
指向的不是“桥缺东西”，而是 **CommCenter 还没走进 C2KRadioModule，或者走进了但在 ABM 启动
前停住**。

---

## 4. fuxi 的实现：Qualcomm 侧已经到哪，Apple 侧到哪

### 4.1 分层

```text
用户态
  launchd
    └─ com.apple.CommCenter（cellular job，因 /baseband 选中）
         ├─ abm::InitServer（ABM 主宿主）
         │    ├─ AppleBasebandManager → AppleBaseband user client
         │    ├─ libPCITransport → pbl / sahara / boot / qmux1 / qmux2
         │    └─ BBUpdater → BHI 寄存器 / SAHARA 镜像 / DONE
         ├─ abmtool（命令服务）
         └─ QMI 客户端（CTL/UIM/NAS/DMS/WDS/BSP/VINYL…）
  abm-helper（按需，ABMHelper.framework）
  fuxi-logdump（我们自己的 job，读统一日志 + 解固件 + 查服务）
内核
  coachz_baseband.cpp（AppleBaseband / PCIControl / 18 通道 / appender / PDP manager）
  fuxi_modem.c（GLINK/QRTR/locator/RMTFS/TFTP/IPA host，向桥喂 NEW_SERVER、收发 QMI）
  FuxiModemProbe.cpp（PAS 启动 MPSS、rails、SMEM、SMP2P、IPCC）
硬件
  SM8550 MPSS（PAS，内置），QRTR over GLINK，IPA v5.5（未启动）
```

### 4.2 Qualcomm 侧的状态

| 项 | 状态 | 证据 |
|---|---|---|
| PAS 启动 MPSS | ✅ stage 9，31 段，SMP2P ready | `evidence/2026-11-20-modem-up.md` |
| GLINK `IPCRTR` | ✅ link up，intent 4096/8 | 你贴的 `link up, QRTR tx 392 rx 200 ... glink tx 724 rx 872` |
| QRTR | ✅ 52 个服务 | 同上 `servers 52` |
| servreg locator | ✅ 1 次公告，4 请求 / 4 应答 | 同上最后一行 |
| RMTFS（service 14） | ✅ 四个分区，modem 5120 读 / 5120 写 | `docs/ios-baseband.md:179-186` |
| TFTP（service 4096） | ✅ 读 178 / 应答 174，`mcfg_sw.mbn` 被请求 | `docs/ios-baseband.md:87-90` |
| 卡状态 / 信号 | ✅ UIM slot status、DMS PERSIST、NAS error 74 | `evidence/2026-11-22-modem-radio-probe.md` |
| IPA v5.5 | ❌ 寄存器读同步外部中止；时钟/互联/固件都投了也不行 | `FuxiModemProbe.cpp:3042-3049` 的 mode 18 注释 |
| IPA 数据面 | ❌ `coachz_modem_data_send` 返回 `ENETDOWN` | `fuxi_modem.c:1255-1259` |

### 4.3 Apple 侧的状态

| 项 | 状态 | 证据 |
|---|---|---|
| `/baseband` 节点 | ✅ `device_type=baseband`、`compatible=baseband,n41`、`class=3`、`euicc-chip-id=2`、`config` 10×u32 | `board.toml:453-470`；`FUXI_DT: /baseband compatible = baseband,n41 (13 bytes)` |
| 根身份 | ✅ `model=iPad14,1`、`target-type=J310`、`compatible=J121AP`（首串） | `board.toml:371-380`；AMFI `queried model name ... iPad14,1` |
| `/product` | ✅ `baseband-chipset=mav16`、iPad mini 6 身份、sRGB | `board.toml:383-400` |
| 桥服务 | ✅ 22/22（AppleBaseband、PCIControl、18 通道、PDP manager、IPAppender） | `docs/ios-baseband.md:1298-1331` |
| AppleBaseband 命名/属性 | ✅ 6 个属性，`config` present，name `baseband` | 同上；`coachz_baseband.cpp:1388-1405` |
| 固件 | ✅ 内核写 17+17；用户态 17 + preboot mirror 1；`Info.plist` 可见。五处 union 5/0 见 `run-20261123-152942`，本轮未重打那一行 | 你的 `FUXILOG` 行；`fuxi-baseband-addon-mount.patch` |
| 兼容 CommCenter | ✅ 已装、已挂、已可信、在跑（`sheng-no-lc=1`） | `docs/ios-baseband.md:244-280` |
| manifest / 签名 / 自有 job | ✅ 字节级追加、签名侧车、cdhash 进 owner trust；`fuxi-logdump`、`abmlite` 都跑了 | 你的 `FUXILOG`、`FUXI_ABMLITE` 行 |
| **ABM 第一次 open** | ❌ **0 个 user client，sahara 0** | 你的日志；`coachz_baseband.cpp:916-921` 的 `bb_note_open` 一次都没打 |

### 4.4 fuxi 与参考的“形”已经一致

`docs/ios-baseband.md:706-713` 的对照表已经说明：参考的 1–4（`/chosen`+`/baseband`+chipset、
IOKit 服务与 UserClient、kern_exec 固件+联合挂载+放行 CommCenter、打包）全部对上。**唯独
参考的“第 0 步”——CommCenter 选 C2K——没有可观测证据。** 这正是本文要补的。

---

## 5. 逐阶段对照表

图例：✅ 已实测 / ⚠️ 已实现未触发 / ❌ 未做 / — 不适用 / ❓ 无法判定

| 阶段 | Apple 原生 | Trogdor 参考 | fuxi 代码 | fuxi 实测 | 判定 |
|---|---|---|---|---|---|
| S0 launchd 选 job | `/baseband` 存在 → cellular | 嫁接 `/baseband` | `[adt.baseband]` | 设置里出现“蜂窝网络” | ✅ |
| S1 radio module | `HWModelStr`→C2K | 根 compatible J121AP → C2K | 根 compatible J121AP | 无日志可证 | ❓ **头号假设** |
| S2 ABM 托管 | `abm::InitServer`，注册 basebandd/abmtool | ABM 启动，首次 open 后崩溃 | 类/框架都在 | `basebandd.xpc REGISTERED`、`abmtool absent` | ❓ 半 |
| S3 固件检查 | `/usr/standalone` + preboot | 两处 bind，BBU 读到全部 | 两树 34 文件 5/0；Info.plist 可见 | `the firmware is visible ...` | ✅ |
| S4 首次 open | AppleBaseband 6/15 + appender/PDP | 缺 appender 时 SIGSEGV | 方法表、appender、PDP 类全 | **0 行** | ❌ **断点** |
| S5 pbl / BHI | 0/3/4 + SendImage | 方法 5 应答后 CHIPID | `cbs_read_register`、sendImage | 未发生 | ⚠️ |
| S6 sahara | HELLO→镜像→DONE | SBL 100%，booted | `{0x15}`、HELLO/DONE | 未发生 | ⚠️ |
| S7 boot/engage | boot 通道、Getting Version | 实机通过 | `boot` 是桩，读不到数据 | 未发生 | ⚠️/风险 |
| S8 qmux/QMI | qmux1/2 + CTL | 53 客户端，262/270 帧 | QMUX↔QRTR + CTL | 0 客户端 | ⚠️ |
| S9 radio online | DMS 低功耗→online | 低功耗握手成功 | 已移植 DMS 0x2d/0x2e | 未触发 | ⚠️ |
| S10 数据面 | PDP manager + pdp_ip | IPA QMAP，网页通 | appender/PDP 类在；数据发 `ENETDOWN` | IPA 未启动 | ❌ |

**这张表的读法**：S0/S3 已过，S1/S2 无法判定，S4 是断点；S5–S9 的代码都在，但**从未被
ABM 触发过**；S10 是另一座山。所以现在不是“桥缺一块”，而是“ABM 没进门”。

---

## 6. 你贴的 14 行，逐行读

先说你给的位置：`bugines: 17661 | dump at 13444`。按仓库里 `docs/ios-baseband.md` 的
编号习惯（`13431:` 起是 150 s 的 `FUXI_LOGDUMP`），这 14 行正好落在 **13431–13444** 附近；
**“dump at 13444” 是抓取位置，不是一个新的 panic**。如果 `bugines:` 是另一条独立的崩溃行，
请把它的完整原文给我——仓库源码与日志里没有这个字符串，无法从现有证据判断。

下面逐行：

```text
FUXI_LOGDUMP 150 s: FUXILOG: the job ran, 17 firmware files unpacked, preboot mirror 1
```

内核在 150 s 读了 `/private/var/db/fuxi-logdump.txt`。这行是 job 自己写的，**如果文件是本轮
重写的**，它说明：
- 我们的 launchd job **被执行**（manifest 字节级追加 + 签名侧车 + 信任缓存这条路是通的）；
- bbfw 解出 **17 个文件**；
- preboot 镜像 **执行了**（在 `/private/preboot` 找到 96 字符目录，随后逐文件硬链接/复制）。
  注意 `mirrorPreboot()` 的返回值只表示“找到了目录并走完了循环”，**不回报单个文件失败**；
  真正证明文件在的是下一行 `Info.plist` 可见。

对比 `docs/ios-baseband.md:526` 的 `preboot mirror 0`，这次是进步；但它只证明**文件在**，
不证明 ABM 会读。**【实测】**

```text
# com.apple.basebandd.xpc REGISTERED
# com.apple.commcenter.xpc REGISTERED
# com.apple.commcenter.carrierspace.xpc REGISTERED
```

`fuxi-logdump.m:191-204` 依次 `bootstrap_look_up` 三个名字，成功就写 `REGISTERED`。
- `basebandd.xpc`：ABM 侧对外的 MachService 名（本端口只能从 bootstrap 看到它，不能区分
  托管它的是 CommCenter 还是别的 daemon）；**名字在，不等于托管进程在跑**。
- `commcenter.xpc`、`carrierspace.xpc`：CommCenter 的自身服务和蜂窝分支服务；`carrierspace`
  在 noncellular job 里没有，所以它的存在至少说明 **cellular job 被加载了**。这是本轮一个
  正面证据。**【实测】**

```text
# abmtool absent
```

`bootstrap_look_up("abmtool")` 失败。`abmtool` 是 ABM 的命令服务，由 CommCenter/ABM 运行时注册；
**它不在，说明 ABM 的命令面没起来**。这和 `abmlite` 的输出互相印证。**【实测】+【文档】**

```text
# demand com.apple.abm.helper.wireless started
# demand com.apple.abm.helper.root started
```

`fuxi-logdump.m:206-227` 对两个 MachService 做 `bootstrap_look_up`，并把返回的 port 一直
held 住（不释放），本意是让 launchd 按需把 `abm-helper` 拉起来。`started` 只表示
`KERN_SUCCESS`，**不表示 job 的 `posix_spawn` 成功、更不表示进程活着**。
一个旁证是下一行：`no /private/var/db/abm-helper.log` —— 如果 job 真的 spawn，launchd 会
按 `StandardErrorPath` 创建文件，不会 ENOENT。**【实测】**

```text
FUXI_LOGDUMP: /private/var/db/commcenter.log wrote nothing at 150 s
FUXI_LOGDUMP: no /private/var/db/abm-helper.log at 150 s (2)
FUXI_LOGDUMP: /private/var/db/abm.log wrote nothing at 150 s
```

三行要分开读：
- `commcenter.log` **文件存在但零字节**：CommCenter 的 job 被加了 `StandardOutPath`，
  launchd 创建了文件，但 CommCenter 只用 `os_log`，不走 stderr。**【实测】**
- `abm-helper.log` **ENOENT**：被加日志的那条 `com.apple.abm-helper.plist` 没被 spawn；
  可能是 `_wireless` 的 home 不存在、也可能是 MachService 属于另一条 plist（见 §7 H3）。
- `abm.log` **文件存在但零字节**：这是我们自己的 `org.unixnu.fuxi.abm`（`RunAtLoad`，直接跑
  `abm-helper`）的 `StandardOutPath`。文件存在只说明它**曾经被创建**（本轮或上一轮，Data 是
  持久的）；零字节说明 `abm-helper` 什么也没写。最可能是它作为前端，没有被 CommCenter 的
  命令驱动连上，静默退出。

> **一个必要的保留（适用于本节所有文件）**：这些文件都没有“本轮”的时间戳。
> `/private/var/db/fuxi-logdump.txt` 在 job 启动时用 `fopen(..., "w")` 重写，所以**如果 job
> 本轮跑了**，内容是本轮的；如果没跑，上一轮的文件会原样留下，内核读到的就是旧内容。
> `commcenter.log`、`abm.log`、`abmlite.log` 是 launchd 的 `StandardOutPath`，同理。
> 所以严格说，本节只能证明“这些文件里有过这些内容”，**不能单独证明是本轮写的**。
> 要把它变成硬证据：内核在 `fuxi_logdump_read` 里读 `va_modtime` 并与 boot time 比较，或者
> 让 daemon 第一行写 `uptime`/boot time，或者每次启动先删除这四个文件。

```text
FUXI_ABMLITE 150 s: 'abmtool' is not supported on this product.
```

内核读了 `/private/var/db/abmlite.log`，那是 `org.unixnu.fuxi.abmlite` 跑
`/usr/bin/abmlite status` 的输出。**这句话是 `abmlite` 自己说的**，含义是
“bootstrap 里查不到 `abmtool`”，不是“这台产品不支持 ABM”。在本轮文件的前提下，它说明：
- `/usr/bin/abmlite` 在这份 userspace 里存在且能跑；
- 我们的 job/文件通道可用；
- ABM 的命令服务没有注册。**【实测】+【文档】**（`docs/ios-baseband.md:1123-1134`）

```text
FUXI_LOGDUMP: the firmware is visible through the union at 150 s
```

`fuxi_modem.c:1170-1175` 用 `vnode_lookup("/usr/standalone/firmware/Baseband/Mav16/Info.plist")`
确认 union 可见。**固件这条已经彻底不是阻塞点**。**【实测】**

```text
FUXI modem: 150 s: link up, QRTR tx 392 rx 200 again 3, servers 52, glink tx 724 rx 872, held packets retried 2 and sent 2
FUXI modem: 150 s: the locator was announced 1 times, took 4 requests and answered 4
```

`fuxi_modem.c:1210-1246` 的周期报告：
- `link up`：GLINK `IPCRTR` 已通；
- `QRTR tx/rx`、`again`、`servers 52`：QRTR 层在跑，modem 公布了 52 个服务（之前记录是 49，
  多出的 3 个是哪三个要看日志里的 NEW_SERVER 行，不能只凭总数推断）；
- `glink tx/rx`、`held packets retried 2 and sent 2`：GLINK 的 intent 流控在工作，曾经被
  hold 的包被重试并成功发出；
- locator：公告 1 次，4 个请求全部应答。

**Qualcomm 侧没有异常。** 这一段和 ABM 无关，是“桥的下半身”健康证明。**【实测】**

### 6.1 这 14 行合起来说明什么

- **已经过的**：launchd job 机制、manifest/签名、固件、preboot、union、CommCenter cellular
  job、AppleBaseband 服务、Qualcomm 链路、locator。
- **被卡住的**：`abmtool` 不在、`abm-helper` 没 spawn、**0 个 `AppleBaseband` user client**、
  sahara 0。
- **不能从这 14 行判断的**：CommCenter 到底走了 C2K 还是 NoBB；ABM 是否初始化过又退出；
  `_wireless` 的 home 在不在；`abm-helper` 的 job 到底是哪条 plist。

---

## 7. 为什么 ABM 不打开：五个假设与判别实验

按“最可能、最便宜可排除”排序。每个假设都给“看什么行”“改什么”“判据”。

### H1（头号）CommCenter 仍走 NoBBRadioModule

**内容**：根 `compatible=J121AP` 是首串，但 `HWModelStr` 的读取路径可能不是首串，或
`libTelephonyUtil` 的表在本镜像里按别的键（MobileGestalt product type / `/product`）选，
于是 CommCenter 仍实例化 `NoBBRadioModule`，ABM 根本不启动。

**支持**：`abmtool` 不在、0 user client、没有 ABM 的任何打开记录；这与“模块没选对”完全一致。
**反对**：`carrierspace.xpc` 已注册、设置里出现蜂窝入口，说明 cellular job 已加载；但
“cellular job 加载”与“C2K 模块实例化”是两件事。

**判别**：拿到 CommCenter 自己的日志，看是
`#I ----------------- Starting C2KRadioModule ----------------` 还是
`#I ----------------- Initializing NoBBRadioModule ----------------`。这两句都在
`CommCenter` 二进制里。**【代码】**（`strings /tmp/compat/CommCenter`）
- 看到 `NoBB`：问题在 S1，继续查 `HWModelStr` 的输入；
- 看到 `C2K`：问题在 S2/S3/S4，转 H2/H4。

**没有日志时的静态手段**：把根 `compatible` 的 31 字节按 NUL 拆开打印，确认首串。
按 `UniXNU/scripts/unixnu/devicetree.py:381-385`（配置的 root compatible **前缀**在硬件 DT 的
串之前）和 `hardware.dtb` 的 `xiaomi,fuxi`/`qcom,sm8550`，这 31 字节应当是
`J121AP\0xiaomi,fuxi\0qcom,sm8550\0`；而参考实现
（`Trogdor-ipadOS/tools/xnu_screen.py:145-149`）是 **`root.set('compatible', b'J121AP\0')`**，
**替换**成 7 字节单串。**一边是 31 字节三串，一边是 7 字节单串——这是最具体的代码差异。**
如果 `libTelephonyUtil` 按整段构造 `HWModelStr`，31 字节就不等于 `J121AP`。**【推断】**
判决实验：把 ADT 的根 compatible 改成单串 `J121AP\0`，看 `C2KRadioModule` 是否出现。

### H2 `AppleBaseband` 发布晚于 ABM 的第一次查找

**内容**：`coachz_baseband_configure` 只在 `link->ready && remote_node_known` 之后调用
（`fuxi_modem.c:972-978`），也就是 **QRTR 链路起来之后**。CommCenter 30 s 就在跑
（`docs/ios-baseband.md:100`），如果 ABM 在 30–40 s 做 `IOServiceMatching("baseband")`，而桥
到 45–60 s 才发布，ABM 可能已经放弃且不重试。

**支持**：publish 行只在发布那一刻打一次、**没有秒数**，仓库里也没有它的时间戳。
**反对**：参考实现也是 GLINK 起来后才发布，ABM 却能等到；但 SC7180 的 PAS/GLINK 可能比
SM8550 快。

**判别**：看 `COACHZ baseband: 22 of 22 services published` 这一行在完整日志里的**位置**：
- 如果它出现在 `FUXI_MODEM: CommCenter is running at 30 s` **之后**，说明发布晚于 30 s，
  H2 成立；
- 如果它在 30 s 行之前，或 30 s 有 `COACHZ baseband: 30 s: ...` 周期报告（该报告要求
  `bb_published`），说明发布 ≤30 s，H2 排除。

**低成本修法（无论 H2 是否成立都值得做）**：把 `coachz_baseband_configure` 提前到
`bsd_init`/probe 早期发布（`bb_send` 允许为空，等链路起来再赋值；`new_server` 在
`coachz_baseband_new_server` 里增量加入）。桥的队列和锁已经支持“先发布、后接服务”。
注意 `coachz_baseband_configure` 现在有 `bb_published` 守卫（`coachz_baseband.cpp:1427`），
重复调用不会替换 `bb_send`；要把“发布”和“挂 send hook”拆成两步，或者加一个
`coachz_baseband_set_send()`。风险：ABM 可能立刻打开并等数据；`bb_send` 为空时请求应留在
`bb_txq` 里，等链路起来再发，而不是丢。**【代码】**

### H3 `_wireless` 的 home 不存在，`abm-helper` job spawn 失败

**内容**：`_wireless` 的 passwd 项是 `_wireless:*:25:25:...:/var/wireless:/usr/bin/false`
（`docs/ios-baseband.md:1347-1348`）。launchd 对 `UserName` job 会 chdir 到 home；如果
`/private/var/wireless` 在 Data seed 里不存在，`posix_spawn` 可能失败，于是
`bootstrap_look_up` 返回了服务名，但进程从未创建，`abm-helper.log` 也就 ENOENT。

**支持**：`no abm-helper.log (2)` 是最直接的证据；`abm.log` 的 job 以 root 跑就成功创建了。
**反对**：ABM 主宿主在 CommCenter，不依赖 `abm-helper`；即使 helper 失败，CommCenter 仍应
打开 `AppleBaseband`。所以 H3 解释“helper 不在”，**不单独解释“0 user client”**。

**判别 + 修法**：
1. 在 `fuxi-logdump.m` 里 `stat("/private/var/wireless")`，打印存在/权限/owner；
2. 若不存在，`mkdir` + `chown(25,25)`（root 的 daemon 可以做），**在两次 demand lookup 之前**；
3. 同时把 manifest 里所有 `MachServices` 含 `com.apple.abm.helper.wireless` 的 job 路径打印出来，
   确认我们加日志的 `com.apple.abm-helper.plist` 是不是那条。

### H4 ABM 在 S2/S3 停住（托管失败/固件检查/激活票据）

**内容**：CommCenter 已走 C2K，但 `abm::InitServer` 失败（`Cannot host ABMHelper`），或
`BootControllerLegacy::prepare` 认为固件不合格，或兼容副本没有真正替换、票据守卫仍在。

**支持**：`abmtool` 不在可以发生在 ABM 初始化的任意早期。**反对**：固件已可见、兼容副本
已挂、`sheng-no-lc` 已放行；参考实现的 `fPhases.size()` 失败是**固件路径**，fuxi 已经满足。

**判别**：看 CommCenter 日志里的
`Cannot host ABMHelper`、`ABM Server is not available`、`FW base path`、`Miss images`、
`kBBUReturnFirmwareDataError`。这些字符串都在二进制里。**【代码】**

### H5 日志通道本身是坏的（观测问题，不是原因）

**内容**：`OSLogStore` 空、stderr 零字节，可能不是“CommCenter 不写”，而是我们的
`fuxi-logdump` **没有 `com.apple.private.logging.store` entitlement**，`storeWithScope:`
返回空集合但不报错。于是我们看不到 H1/H4 的判别行。

**支持**：`fuxi-logdump` 只有 `com.apple.private.security.no-sandbox`，没有日志 entitlement；
`OSLogStore` 无过滤枚举也是 0 条。**反对**：`logd` 在跑；但“logd 在跑”不等于“我们的 ad-hoc
进程有权限读”。

**判别/替代**：
1. 给 daemon 加 entitlement 并重新签名（如果 AMFI 允许 ad-hoc 的 private entitlement）；
2. 或者用 `/usr/bin/log show --last 120s --predicate 'subsystem == "com.apple.telephony.abm"'`
   由 root job 执行（若该二进制存在）；
3. 或者把 `/private/var/db/diagnostics` 的 `tracev3` 留在 Data，主机用 `log show` 读
   （参考实现就是这么做的）；
4. 最差也可以给 `com.apple.telephony.abm` 写 `Enable-Private-Data` 的 subsystem plist
   （参考做法），但**先要有读的手段**。

---

## 8. 还没做的差异清单

### A. 挡住 ABM 的（P0）

| # | 差异 | 现状 | 判据 |
|---|---|---|---|
| A1 | CommCenter 模块选择不可观测 | 无 C2K/NoBB 行 | 日志出现 `Starting C2KRadioModule` |
| A2 | `abm-helper` job 的真实 plist / spawn 原因不明 | `no abm-helper.log (2)` | 进程表出现 `abm-helper`，或明确 spawn 错误 |
| A3 | `_wireless` home 未验证 | 未知 | `/private/var/wireless` 存在且 owner 25 |
| A4 | 桥发布时机 vs ABM 查找时机 | 无时间戳 | publish < 30 s，或 ABM 重试 |
| A5 | `AppleBaseband` 打开路径未验证 | 0 user client | `COACHZ baseband: AppleBaseband user client 1` |

### B. ABM 打开之后才会遇到的（P1）

| # | 差异 | 现状 | 风险 |
|---|---|---|---|
| B1 | `boot`/`refs` 通道是桩 | 能打开，读不到数据（`coachz_baseband.cpp:778`） | 参考实机的 `Connecting to Loader` / `Getting Version` 走 `boot`；fuxi 可能在这里失败 |
| B2 | SAHARA 只列了 `0x15` SBL1 | `bb_sahara_images[] = { 0x15 }`（`coachz_baseband.cpp:44`） | 参考的 SBL-only 能 booted；但若 ABM 请求后续镜像，`item not found` |
| B3 | 低功耗 DMS 握手从未触发 | 代码在，挂在 sahara 打开上（`coachz_baseband.cpp:324-334`） | 第一次 ABM 启动后才会验证 |
| B4 | 私有 QMI 只覆盖 BSP/VINYL/UIM/NAS/DMS/WDS 的子集 | 其余回 `NOT_SUPPORTED` | ABM 注册/激活/载波包可能要求更多 |
| B5 | 1024 帧队列、客户端池化未在 fuxi 压过 | 参考的 53 客户端/262 帧是 coachz 的数 | fuxi 的 52 服务 + ABM 可能更多 |
| B6 | `AppleIPAppender` 多数 selector 返回零 | 方法 16 有实现，其余占位 | ABM 的 data::TransportService 可能崩/走错分支 |

### C. 数据面（P2，独立的一座山）

| # | 差异 | 现状 | 依据 |
|---|---|---|---|
| C1 | IPA v5.5 寄存器读同步外部中止 | mode 18 只打印“disabled” | `FuxiModemProbe.cpp:3042-3049` |
| C2 | IPA 固件可加载但块不响应 | `sheng-ipa-fw=1` 时 PAS 0x0f 成功，读仍 abort | 同上 |
| C3 | IPA 端点表是 v4.2，fuxi 是 v5.5（24/22 vs 23/21） | 未生成 v5.5 表 | `docs/fuxi-baseband-plan.md` D2 |
| C4 | `INIT_MODEM_DRIVER` 的 TLV 0x23 条件、0x24/0x25 不填 | 未接 | 同上 |
| C5 | `coachz_modem_data_send` 返回 `ENETDOWN` | 明确未实现 | `fuxi_modem.c:1255-1259` |
| C6 | 没有 `pdp_ip` / QMAP / rmnet | appender/PDP 类只是壳 | `coachz_bb_pdp.cpp` |

### D. 用户态/持久化（P3）

| # | 差异 | 现状 |
|---|---|---|
| D1 | 兼容 CommCenter 是否真是运行中的那份 | 已挂载，但未读回运行映像的 cdhash |
| D2 | RMTFS 写入只在内存 + 每秒 flush；开机**读回**未做 | `docs/ios-baseband.md:199-218` |
| D3 | 激活/载波包/IMSI/ICCID 的真实值 | 桥有 BSP/VINYL 代码，ABM 从未调用 |
| D4 | eSIM | 参考已通；fuxi 未开始 |
| D5 | SSR/复位恢复 | 未做；modem 重启后 RMTFS 内存副本会丢 |
| D6 | 统一日志可读性 | `OSLogStore` 空、stderr 零字节 |

### E. 工程性（P3）

| # | 差异 | 说明 |
|---|---|---|
| E1 | 固件写两遍 | 内核 `sheng_boot_baseband_firmware` 写 34 个；用户态又解 17 个并镜像；可合并 |
| E2 | `fuxi-logdump.m` 的 `mirrorPreboot` 依赖 96 字符目录名 | 现在成功（`mirror 1`），但目录名来源应改为 IORegistry 的 hash，和内核一致 |
| E3 | `sheng-logdump` 只在 150/210 s 读 | 早于 ABM 决策的日志拿不到；应加 60/90 s |
| E4 | 内核报告只打首串 compatible | 应逐串打印，排除 H1 的 31 字节问题 |

---

## 9. 建议的下一步（按顺序，一次一个变量）

### 实验 1：把“不可观测”变成“可观测”（不改桥）

改 `fuxi-logdump.m`：
1. 在 demand lookup **之前** `stat("/private/var/wireless")`，打印存在/owner/mode；
2. 若不存在，`mkdir("/private/var/wireless", 0755)` + `chown(25,25)`；
3. 解析 `/System/Library/xpc/launchd.plist`（Foundation 的 `NSPropertyListSerialization`），
   打印所有 `MachServices` 含 `com.apple.abm.helper.wireless` / `.root` 的 job 路径、`UserName`、
   `RunAtLoad`、`KeepAlive`；
4. 用 `sysctl(KERN_PROC_ALL)` 在 60/120/150 s 打印 `CommCenter`、`basebandd`、`abm-helper`、
   `CommCenterRootHelper` 的 pid；
5. 打印 `/private/var/db/diagnostics` 是否存在、大小，判断 tracev3 是否可带回主机；
6. 在 `fuxi_logdump_read` 里对读到的文件做 `vnode_getattr`，把 `va_modtime` 和 boot time 比较，
   打印“本轮/旧文件”。这一行是后面所有结论的地基。

**判据**：
- 如果 `_wireless` home 不存在且补上后 `abm-helper` 进程出现，H3 成立；
- 如果 manifest 显示 MachService 属于另一条 plist，说明我们加日志加错了对象；
- 如果 `basebandd` 进程在、`abmtool` 仍不在，说明 ABM 托管了但命令面没起，转 H4。

### 实验 2：拿到 CommCenter 的模块选择行

优先顺序：
1. 给 `fuxi-logdump` 加 `com.apple.private.logging.store`（或 `...admin`）entitlement，重签，
   再跑 `OSLogStore`；如果 AMFI 拒绝，记录拒绝原因；
2. 若 `/usr/bin/log` 存在，job 里 `posix_spawn` 它，`--last 120s --predicate
   'subsystem == "com.apple.telephony.abm" OR process == "CommCenter"'`，输出到文件；
3. 若都不行，把 Data 的 `diagnostics` 带回主机 `log show`（参考做法）。

**判据**：看到 `Starting C2KRadioModule` 或 `Initializing NoBBRadioModule`，H1 立刻定论。

### 实验 3：逐串打印根 compatible + `/baseband` 属性

改 `fuxi_modem.c:1181-1207`，把 `compatible` 按 NUL 拆开逐串打印，并打印 `/baseband`
的每个属性名和长度。**判据**：确认 `HWModelStr` 读到的是 `J121AP`，不是 `xiaomi,fuxi`。

### 实验 4：把桥的发布提前（只改时机，不改 ABI）

在 `native_fuxi_modem_probe` 的 owner thread 启动后、`fuxi_modem_start` 之前调用
`coachz_baseband_configure`（`bb_send` 先给一个返回 `-1` 的桩，链路起来后替换）。
**判据**：如果 ABM 在 30 s 查找，日志出现 `22/22 published` 早于 30 s，且随后出现
`AppleBaseband user client`，H2 成立。

### 实验 5：ABM 打开之后的桥验证

只有实验 1–4 出现 `AppleBaseband user client` 才做：
1. 看 selector 6/15 是否被调用（`COACHZ baseband: AppleBaseband selector ...`）；
2. 看 `pbl` 的 register 0/3/4 和 `SendImage`；
3. 看 `sahara hello` / `sahara finished`；
4. 看 `qmux1 opened` 和 QMI 首帧。
任何一步缺失，就按 §3.1 的六道门逐条对。

---

## 10. 验收判据（严格）

不要把下面任何一条单独当成“ABM 起来了”：

| 不算 | 为什么 |
|---|---|
| `abm-helper` 进程在 | 它只是 helper；参考实机 ABM 崩溃时它也不需要存在 |
| `basebandd.xpc REGISTERED` | 只证明名字在 bootstrap，不证明托管进程在跑 |
| `Settings` 出现“蜂窝网络” | 只证明 cellular job 加载，不证明 C2K 模块 |
| `22/22 services published` | 只证明桥发布了，不证明 ABM 打开 |
| `abmlite` 能跑 | 只证明 job/文件通道，不证明 `abmtool` |

**算“ABM 接管基带”的最小证据链**（全部出现，按顺序）：

```text
COACHZ baseband: AppleBaseband user client 1, type ...
COACHZ baseband: AppleBaseband selector 6 ...        (或 15)
COACHZ baseband: pbl register 3 read, ...            (或 0/4)
COACHZ baseband: pbl sendImage ... bytes dropped
COACHZ baseband: sahara hello
COACHZ baseband: sahara finished, image ...          (或 image read)
COACHZ baseband: qmux1 opened
COACHZ qmi: to-host ch 1 svc 0x0 ...                 (QMI CTL 首帧)
```

**算“基带 booted”的最小证据链**：

```text
COACHZ baseband: the DMS answer: error 0, ... TLVs    (工作模式)
COACHZ baseband: the radio is ...                    (DMS 模式)
COACHZ baseband: the UIM answer: error 0, ... TLVs   (卡状态)
COACHZ baseband: ... clients ...                     (客户端数 > 0)
```

**算“用户态可用”**：Settings 的蜂窝页能进、`Carrier` 不是 `No available`、状态栏有信号格。
这些是 ABM 之后的事，现在不投。

---

## 11. 一句话总结

> fuxi 的 Qualcomm 侧已经把“基带能跑、服务能公告、文件能服务、卡能读”全部做完；Apple 侧
> 的桥也已经按参考实现做到“ABM 一敲门就能应”。**现在唯一没发生的事，是 ABM 敲门。**
> 门不在桥里，而在 `CommCenter` 选择 C2KRadioModule 并启动 ABM 的那一步；这一步在这个端口
> 没有日志可看，所以第一优先级不是改桥，而是**把 CommCenter 的模块选择行读出来**。

---

## 附录 A：代码地图（本文引用的位置）

| 主题 | 文件:行 |
|---|---|
| 桥的启用开关 `/chosen/coachz-baseband` | `UniXNU/.../coachz_baseband.cpp:143-154` |
| 22 个服务的发布、`/baseband` 属性复制、setName | `coachz_baseband.cpp:1383-1445` |
| `AppleBaseband` user client 与 selector 表 | `coachz_baseband.cpp:1267-1310`；`coachz_bb_control.h:22-87` |
| 通道 open/write/read/register/sendImage | `coachz_baseband.cpp:1083-1136` |
| 通道匹配 `ABPInterfaceProtocol` | `coachz_baseband.cpp:1147-1161` |
| SAHARA 设备端 | `coachz_bb_sahara.h:21-271`；`coachz_baseband.cpp:44, 266-334` |
| 低功耗 DMS 0x2d/0x2e | `coachz_baseband.cpp:270-334` |
| QMUX↔QRTR / CTL | `coachz_bb_qmux.h` |
| IPAppender / PDP manager | `coachz_baseband.cpp:1314-1381` |
| fuxi 的 QRTR/locator/RMTFS/TFTP/IPA host | `UniXNU/.../fuxi_modem.c:435-545, 660-950` |
| 桥发布钩子 | `fuxi_modem.c:972-978` |
| 周期报告 / 日志读取 | `fuxi_modem.c:1076-1246` |
| 内核侧固件写入、五处 union | `patches/xnu/fuxi-baseband-addon-mount.patch` |
| 自有 job / manifest 追加 / 签名 | `scripts/make-fuxi-manifest.py:20-62, 65-149, 248-297` |
| 日志 dump 守护进程 | `package/fuxi-logdump.m:179-293` |
| 板级身份 / `/baseband` / 启动参数 | `board.toml:239-241, 371-470` |
| PAS / IPA 模式与说明 | `package/FuxiModemProbe.cpp:2995-3130` |

## 附录 B：参考实现的证据位置

| 内容 | 位置 |
|---|---|
| ABM 启动、方法表、SAHARA、QMUX | `docs/refs/trogdor-modem.md:79-107` |
| 通道匹配、booted、engage | `Trogdor-ipadOS/docs/hardware-tests.md:1649-1656` |
| `No CommandDriver`、appender 崩溃、固件、chip id | 同上 `:1629-1649` |
| 低功耗握手 | `docs/refs/trogdor-modem.md:232-238` |
| 兼容 CommCenter 只改一条分支 | `UniXNU/.../coachz_cellular_compat.py:13-14, 73-79` |

## 附录 C：你贴的日志（原文）

```text
FUXI_LOGDUMP 150 s: FUXILOG: the job ran, 17 firmware files unpacked, preboot mirror 1
# com.apple.basebandd.xpc REGISTERED
# com.apple.commcenter.xpc REGISTERED
# com.apple.commcenter.carrierspace.xpc REGISTERED
# abmtool absent
# demand com.apple.abm.helper.wireless started
# demand com.apple.abm.helper.root started
FUXI_LOGDUMP: /private/var/db/commcenter.log wrote nothing at 150 s
FUXI_LOGDUMP: no /private/var/db/abm-helper.log at 150 s (2)
FUXI_LOGDUMP: /private/var/db/abm.log wrote nothing at 150 s
FUXI_ABMLITE 150 s: 'abmtool' is not supported on this product.
FUXI_LOGDUMP: the firmware is visible through the union at 150 s
FUXI modem: 150 s: link up, QRTR tx 392 rx 200 again 3, servers 52, glink tx 724 rx 872, held packets retried 2 and sent 2
FUXI modem: 150 s: the locator was announced 1 times, took 4 requests and answered 4
```

> 这 14 行里**没有** `# user ...` 四行（源码里在 `# abmtool absent` 之后、`# demand` 之前）。
> 如果日志是逐字连续的，说明装到设备上的 `fuxi-logdump` 比仓库源码旧；如果不是逐字连续的，
> 说明你做了摘录。两种情况都不影响上面的结论，但下一次请把 `# user` 行留下，它直接回答 H3。