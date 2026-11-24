# 蜂窝为什么还不可用：基带代码对照与根因排序

2026-11-24。上一篇 `abm-boot-comparison.md` 讲的是“ABM 启动链各阶段长什么样”；这一篇只做一件事：
**把 fuxi、Trogdor、umi 三条基带代码放在一起逐文件对照，指出哪几处代码差异足以造成“蜂窝不可用”，
并给出判决实验。**

先给结论，后面是证据。

---

## 0. 结论

### 0.1 桥的 ABM 面没有缺口

- `coachz_baseband.cpp` 的 diff 是 **817 行**，但删掉/改坏的 ABM 面代码是 **0**：
  新增的是 SSCTL/UIM/NAS 探针、低功耗 DMS 握手、`/baseband` 属性复制与 `setName`、发送失败重试；
  `AppleBaseband`、`AppleBasebandPCIControl`、18 个通道、`AppleIPAppender`、
  `AppleBasebandPCIMAVPDPManager`、`matchPropertyTable`、SAHARA 设备端、QMUX/CTL 全在。
- **6 个头文件全部与 Trogdor 逐字节相同**（channels/control/data/qmux/sahara/pdp.h），
  `coachz_bb_pdp.cpp` 只差一个 include。**桥不是缺一块。**
- 真正缺的是 **平台胶水**和 **观测点**，不是 ABM 要的 IOKit 契约。

### 0.2 三个代码级缺口

| # | 缺口 | 代码证据 | 后果 |
|---|---|---|---|
| 1 | **ABM 的宿主（`basebandd` 或 CommCenter）没有被观测** | `fuxi_commcenter_report` 只扫 `CommCenter/logd/launchd/abm-helper/CommCenterRootHelper`（`fuxi_modem.c:1079-1090`）；`fuxi-logdump` 只 `bootstrap_look_up("com.apple.basebandd.xpc")`，不查进程（`fuxi-logdump.m:192-204`） | 无法区分“宿主没起”与“宿主起了但 ABM 没初始化”；`REGISTERED` 被误当成“在跑” |
| 2 | **桥的发布被绑在 modem 的 channel open 上，且有 10 秒放弃** | `fuxi_modem.c:844-847`：`if (link->ready && !link->published)` 才 publish；`FuxiModemProbe.cpp:1628-1650`：`waitLink` 10 s 超时后 `return -6`，`ownerThread` 走 `goto failed`，**不再 poll**（若 `ready` 从未置位，则桥也从未 publish） | 参考在 bring-up 前或 GLINK start 就发布；fuxi 可能晚于 ABM 的一次性查找；慢启动直接没有桥 |
| 3 | **ABM 之后：umi 的 pending/pool/DMS/WDS 修补没有，IPA 完全没起** | umi 的 `umi_bb_pending`、64 槽池化、WDS 失败指示、DMS 4/5→1；fuxi 无。`coachz_modem_data_send` 返回 `ENETDOWN`（`fuxi_modem.c:1255-1259`）；IPA 寄存器读同步外部中止（`FuxiModemProbe.cpp:3042-3049`） | 即使 ABM 打开，也只能到“有卡/有注册、无数据”；当前连 ABM 都没打开 |

### 0.3 直接原因排序

| 排序 | 假设 | 为什么排这里 | 判决实验 |
|---|---|---|---|
| **1** | **CommCenter 仍走 NoBBRadioModule** | 它是参考实现的关键开关；fuxi 的 `compatible=J121AP` 已设，但属性是 **31 字节多串**（参考是 **7 字节单串**，§2.3），`HWModelStr` 若按整段读就不匹配。没有一行日志证明 `C2KRadioModule` 被实例化；`abmtool` 不在、0 user client 与“模块没选”完全一致 | 先做单串实验（§2.3），再拿 `Starting C2KRadioModule` vs `Initializing NoBBRadioModule` |
| **2** | **ABM 宿主/helper 的按需 job 没 spawn（最可能是 `_wireless` 的 home 不存在）** | 如果 17.7.11 的 ABM 宿主是独立 `basebandd`，它和 `abm-helper` 都可能是 `_wireless`；home 缺失会让两个 job 同时 spawn 失败。代码里恰好没扫 `basebandd`、也没 `stat` home | 进程扫描 + manifest job 解析 + `/private/var/wireless` 的 stat/mkdir |
| **3** | **桥发布晚于 ABM 的一次性查找** | 代码上确实比两条参考都晚；但 modem 若在 20 s 前 publish，就不成立。需要时间戳 | 完整日志里 `22/22 published` 与 `CommCenter running at 30 s` 的先后 |
| **4** | **`waitLink` 10 s 放弃** | 是确定缺陷，但你这轮 150 s 日志 `link up`，本轮不是它；是同类症状的间歇原因 | 看 `the link did not come up within 10 s` |
| **5** | **ABM 之后的桥/IPA 缺口** | 不解释“入口点不进”，只解释“没数据” | ABM 打开后才验收 |

> **一句话**：不是桥缺 ABM 的接口，而是 **ABM 的宿主/模块选择没有被观测**，以及 **桥的发布时机比
> 参考晚且可能直接放弃**。H1（NoBB）是最可能的根因，H2（宿主进程）是最便宜的判决实验：
> **先做 H2 的进程扫描，再做 H1 的模块日志**。

---

## 1. 对比对象与证据

| 代号 | 代码 | 形态 | 用途 |
|---|---|---|---|
| **fuxi** | `UniXNU/drivers/QcomBaseband/baseband/*` + `platform/fuxi/fuxi_modem.c` + `unixnu-fuxi/package/FuxiModemProbe.cpp` | SM8550 内置 MPSS，PAS+GLINK/QRTR | 当前端口 |
| **Trogdor** | `Trogdor-ipadOS/platform/coachz-prelink/pending/baseband/*` + `patches/xnu/coachz-prelink-baseband.patch` | SC7180 内置 MPSS，Wi-Fi 线程兼 QRTR | 同形态参考，ABM 已实机打开 |
| **umi** | `hakios-xnu-kernel-umi-fixes/devices/xiaomi-mi-10/drivers/umi/hakios.umi_baseband.cpp` + `umi_modem.c` | 外置 SDX55，PCIe/MHI | ABM 已完整驱动、有数据 |
| **Apple** | `/tmp/compat/CommCenter` + `CommCenter` 的框架/字符串 | 21H461 用户态 | 判定模块选择和宿主 |

证据级别同上一篇：**【代码】** 读源码、**【实测】** 设备日志、**【文档】** 参考记录、**【推断】** 分析。

---

## 2. 先看桥本身：ABM 面没有缺口

### 2.1 `coachz_baseband.cpp` 的 diff

fuxi 1666 行，Trogdor 1430 行，diff 817 行。按 hunk 分类：

| 类别 | 内容 | 对 ABM 的影响 |
|---|---|---|
| 新增 | SSCTL 失败原因、UIM slot status、NAS/DMS 只读探针 | 不影响；ABM 起来后才用 |
| 新增 | 低功耗 DMS 0x2d/0x2e（从 Trogdor 移植） | 正向；ABM 打开 sahara 后才触发 |
| 新增 | `bb_publish(..., node)` 复制 `/baseband` 属性 + `setName("baseband")` | **正向**；`libTelephonyBaseband` 按名字/属性找板子 |
| 新增 | `bb_send` 返回 int、失败重排队 | 正向；QRTR 没 credit 时不丢请求 |
| 新增 | `AppleBaseband`/`PCIControl` 的 `bb_note_open` 打印 | 正向；这次就是靠它确认“0 次打开” |
| 删除/移动 | 低功耗块从 QMUX 路径移到 sahara 打开处 | 行为等价，位置更对 |
| 未动 | Sahar/control/qmux/data 头文件 | **ABI 未变** |

**结论**：fuxi 的桥在 **ABM 面**上是 Trogdor 的超集（加上 umi 风格的 `/baseband` 命名），不存在“ABM 要的类/方法
被删掉”这种缺口；eSIM/双卡面比 Trogdor 旧，但那是 ABM 之后的事。`AppleBaseband` 的 6 个属性、`config` present、name `baseband` 也已经实测打印。

### 2.2 头文件逐字节对比

| 文件 | Trogdor vs fuxi | 含义 |
|---|---|---|
| `coachz_bb_channels.h` | **SAME** | 通道名、selector、`ABPInterfaceProtocol` 一致 |
| `coachz_bb_control.h` | **SAME** | `AppleBaseband` 方法表一致（6/15/22/23/26/27…） |
| `coachz_bb_data.h` | **SAME** | 数据口/激活/载波逻辑一致 |
| `coachz_bb_qmux.h` | **SAME** | QMUX↔QRTR、CTL 一致 |
| `coachz_bb_sahara.h` | **SAME** | SAHARA 设备端一致（含 `0x15` SBL1） |
| `coachz_bb_pdp.h` | **SAME** | PDP 接口一致 |
| `coachz_bb_pdp.cpp` | 只差 `sys/mbuf.h` → `sys/kpi_mbuf.h` | XNU 版本 include，行为一致 |
| `coachz_bb_esim.h` / `coachz_bb_slots.h` | fuxi 826 行，Trogdor 1337 行；fuxi **没有** `coachz_bb_slots.h` | eSIM/双卡映射较旧；物理单卡不受影响，eSIM 是后续缺口 |

**结论**：ABM 面（`AppleBaseband`/通道/SAHARA/QMUX/control）逐字节同源。**不要再去桥里找“ABM 为什么不打开”**，
缺口在桥之外。

### 2.3 最具体的代码差异：根 `compatible` 是 31 字节多串，参考是 7 字节单串

这是目前找到的、**唯一直接落在“模块选择输入”上的代码差异**。

fuxi 的 ADT 构建器（`UniXNU/scripts/unixnu/devicetree.py:381-385`）：

```python
# A configured root 'compatible' goes before the hardware DTS's own entries: an OS identity's board name comes first (the OS
# reads the first entry as the hardware model) and the board's compatible strings stay matchable after it.
configured = dict(typed_properties(platform.get('root', {})))
compatible = configured.pop('compatible', b'') + root.properties['compatible']
root_properties = [('compatible', compatible), ...]
```

`board.toml` 配 `compatible = "J121AP"`，硬件 DT 的根是 `xiaomi,fuxi\0qcom,sm8550\0`，于是实际属性是：

```text
J121AP\0xiaomi,fuxi\0qcom,sm8550\0        ← 31 字节；内核日志 "root compatible = J121AP (31 bytes)"
```

参考实现（`Trogdor-ipadOS/tools/xnu_screen.py:141-149`）：

```python
def baseband_tree(root):
    ...
    root.set('compatible', b'J121AP\0')      ← 7 字节，单串，替换而不是前缀
```

参考的注释直接写着：

> **HWModelStr comes from the root compatible**，libTelephonyUtil's table of hardware models has no
> J120 (Wi-Fi), so CommCenter would run NoBBRadioModule: J121 is Qualcomm Mav16.

**所以两边给 `HWModelStr` 的输入不是同一个形状**：参考是 `J121AP` 单串；fuxi 是 `J121AP` 开头的
三串。如果 `libTelephonyUtil` 用 `CFStringCreateWithBytes`/`OSData` 的**整段长度**构造字符串，
31 字节的串不等于 `J121AP`，`__TelephonyRadiosDetermineRadio` 找不到 J121，CommCenter 就回到
`NoBBRadioModule`——**ABM 根本不会被拉起**。如果它只用 `strcmp`/`%s` 读首串，31 字节没问题。

**这是“代码差异”里最值得先测的一条**：一次单串实验就能定论，而且不需要改桥、不需要日志。
风险是根 `compatible` 同时被 XNU 平台代码用来匹配板子；如果替换后系统起不来，就回退，改用
“保留板级串、另造一个只给 CommCenter 看的属性”的方案（但 `libTelephonyUtil` 读的是
`compatible`，没有第二个键，所以最终可能只能替换 + 在别处补板级匹配）。

---

## 3. 缺口一：ABM 的宿主（`basebandd` 或 CommCenter）没有被看

### 3.1 代码事实

内核的进程扫描（`fuxi_modem.c:1079-1090`）：

```c
LIST_FOREACH(p, &allproc, p_list) {
    if (!found && strncmp(p->p_comm, "CommCenter", ...) == 0) found = p->p_pid;
    if (!logger && strncmp(p->p_comm, "logd", ...) == 0) logger = p->p_pid;
    if (!launchd && strncmp(p->p_comm, "launchd", ...) == 0) launchd = p->p_pid;
    if (!abm && strncmp(p->p_comm, "abm-helper", ...) == 0) abm = p->p_pid;
    if (!helper && strncmp(p->p_comm, "CommCenterRoot", ...) == 0) helper = p->p_pid;
}
```

**没有 `basebandd`。**

用户态日志守护进程（`fuxi-logdump.m:192-204`）只查服务名：

```c
static const char *const probes[] = {
    "com.apple.basebandd.xpc", "com.apple.commcenter.xpc",
    "com.apple.commcenter.carrierspace.xpc", "abmtool",
};
...
bootstrap_look_up(bootstrap_port, probes[i], &port);
fprintf(file, "# %s %s\n", probes[i], found == KERN_SUCCESS ? "REGISTERED" : "absent");
```

**`REGISTERED` 是 bootstrap 名字，不是进程。** 名字可以由 launchd 为一条按需 job 预先注册，
进程完全没 spawn；也可以在进程退出后仍然保留一个 dead name。你们自己的注释
（`fuxi-logdump.m:192`）写的是 “basebandd.xpc is the service ABM lives behind”，但代码没有
验证“behind”是谁、在不在。

### 3.2 17.7.11 的 ABM 宿主：两种可能

17.7.11 的 ABM 宿主有两种可能，代码没有区分：

| 可能 | 含义 | 观测上的差别 |
|---|---|---|
| **A. CommCenter 进程内**（umi 的 16.5 记录） | ABM 由 `CommCenter` 里的 `abm::InitServer` 托管 | CommCenter 在跑，ABM 可能已初始化；`abmtool` 不在说明初始化停在更早 |
| **B. 独立 `basebandd`**（17.x 的 `.allow` 线索） | CommCenter 只是客户端，通过 `com.apple.basebandd.xpc` 连它 | `basebandd` 没进程 = 没有 ABM，`abmtool` 必然不在 |

支持 B 的线索：

- `CommCenter` 里有 `com.apple.basebandd.xpc.allow`，这是 sandbox 里“允许连接某个 MachService”
  的规则名；如果 CommCenter 自己托管它，通常不需要这条 allow。**【代码】**
- `CommCenter` 同时导入 `abm::HelperClient`（连 `abm-helper`）和 `abm::InitServer`
  （服务端）；这不能证明它在同进程调用 `InitServer`，只能证明两套符号都在。**【代码】**
- fuxi 自己把 `abm-helper` 强行 `RunAtLoad` 后，ABM 仍然不打开基带；这说明 `abm-helper`
  不是触发者。**【实测】**（`docs/ios-baseband.md:1052-1062`）

支持 A 的线索：

- umi 的 16.5 记录明确写“ABM = CommCenter 进程内的 basebandd”；17.7.11 是否搬出去没有记录。
- `CommCenter` 二进制里确实有 `abm::InitServer` / `StartServer`。

**无论 A 还是 B，现在的代码都没有把“宿主在不在”测出来。** 这是第一个要补的洞。

### 3.3 如果宿主是 `basebandd`：`_wireless` 的 home 不存在

`basebandd` 和 `abm-helper` 都可能是 `UserName = _wireless` 的 launchd job。`_wireless` 的 passwd
项是 `_wireless:*:25:25:...:/var/wireless:/usr/bin/false`；launchd 对带 `UserName` 的 job 会
chdir 到 home。**如果 Data seed 里没有 `/private/var/wireless`，两个 job 都会在 `posix_spawn`
阶段失败**，而 launchd 仍然会为它们注册 MachService。

**如果 B 成立**，这一条能同时解释日志里的四件事：

| 日志 | 用 home 缺失解释 |
|---|---|
| `# com.apple.basebandd.xpc REGISTERED` | job 被加载、服务名被注册；**但进程没 spawn** |
| `# abmtool absent` | 宿主没起来，当然没有命令服务 |
| `no /private/var/db/abm-helper.log (2)` | job 没 spawn，launchd 不会创建 `StandardOutPath` |
| `# demand com.apple.abm.helper.wireless started` | `bootstrap_look_up` 返回服务名，launchd 接受请求；spawn 失败不回传给调用者 |

而 `CommCenter`（不同 user）和 `org.unixnu.fuxi.abm`（root，`/var/root` 在）都能跑，正好对上。
**这是目前最省事的统一解释**，而且一次 `stat` 就能排除。

### 3.4 判决实验

在 `fuxi_commcenter_report` 里加：

```c
pid_t baseband = 0;
if (!baseband && strncmp(p->p_comm, "basebandd", sizeof p->p_comm) == 0) baseband = p->p_pid;
```

在 `fuxi-logdump.m` 里加：

1. `stat("/usr/libexec/basebandd")`、`/usr/sbin/basebandd`、`/System/Library/.../basebandd`；
2. 解析 `launchd.plist`，打印所有 `MachServices` 含 `com.apple.basebandd.xpc` 的 job，
   以及它的 `UserName`/`RunAtLoad`/`KeepAlive`/`Program`；
3. `stat("/private/var/wireless")`（`_wireless` 的 home）和 `/var/root`；
4. 60/120/150 s 的进程列表。

**判据**：

- `basebandd` 进程在、`abmtool` 在 → ABM 已起，问题在桥的第一次 open；
- `basebandd` 进程在、`abmtool` 不在 → ABM 初始化被卡（固件/票据/entitlement）；
- `basebandd` 进程不在 → 查它的 job 为什么没 spawn（home/UserName/设备树条件），
  或者 CommCenter 根本没连它（回到模块选择）。

---

## 4. 缺口二：桥的发布顺序与 10 秒门

### 4.1 三条代码的发布时机

| 代码 | 发布点 | 代码 |
|---|---|---|
| **umi** | **modem bring-up 之前**，只要 `md_bridge` | `umi_modem.c:296-298`：`/* umi_modem=2: CommCenter must find its baseband services whatever the modem does. */ if (md_bridge) umi_bb_publish();` |
| **Trogdor** | `wg_start()` 成功之后（GLINK start，channel 还没开） | `coachz-prelink-baseband.patch:129-133`：`const int link = wg_start(); ... if (!link) coachz_baseband_configure(...)` |
| **fuxi** | **modem 打开 IPCRTR 通道之后**，`link->ready` 才 publish | `fuxi_modem.c:844-847`：`if (link->ready && !link->published) { link->published = 1; fuxi_modem_publish(); }` |

fuxi 的注释还专门论证了“必须之后”：

```c
/* The bridge is published once the channel is up. It has to be published after, not before: the bridge's clients
   are bound to the services the modem announces, and it needs this side's send hook to reach them. Until the
   channel is up there is no route to the modem at all, so a bridge published earlier could only fail. */
```

**这条论证与 umi 的注释正好相反**：umi 说“无论 modem 怎样，CommCenter 都必须能找到基带服务”。
桥的 `AppleBaseband`/`pbl`/`sahara` 并不需要 modem 已经起来；它们只需要 IOKit 服务先存在。
QMUX 客户端可以等 `NEW_SERVER` 到了再绑定（`coachz_baseband_new_server` 就是增量绑定）。
所以“必须之后”是一个 **设计假设，不是约束**。

### 4.2 10 秒门

`FuxiModemProbe.cpp:1628-1650`：

```c
clock_interval_to_deadline(10000, kMillisecondScale, &deadline);
while (mach_absolute_time() < deadline) {
    ...
    fuxi_modem_poll();
    ...
    if (status.ready && status.hello_sent) return 0;
}
...
printf("FUXI_MODEM: the link did not come up within 10 s ...");
return -6;
```

`ownerThread`（`:2681-2688`）：

```c
rc = waitLink();
if (rc) goto failed;
...
serviceLink();
```

`waitLink` 失败 → `goto failed` → **不进入 `serviceLink`** → 不再 poll GLINK。分两种：

- `ready` 从未置位：`fuxi_modem_poll` 里的 publish 从未执行，**桥根本不出现**；
- `ready` 置位但 `hello_sent` 没成：publish 可能已经执行（publish 只看 `ready`），但线程随即
  failed，**不再 poll，QMUX 永远不通**。

两种都不可用。参考实现没有这个 10 秒放弃：`wg_start` 之后是一个无限循环；umi 的 `md_thread`
是无限重试。**【代码】**

**这一轮 150 s 的日志 `link up`，所以本轮不是它。** 但它是同类“蜂窝不可用”的确定原因：
冷启动慢、modem 第一次没在 10 s 内开通道，就直接没有桥。

### 4.3 为什么“晚”会造成“永远不打开”

如果 ABM 在启动时做一次 `IOServiceMatching("baseband")` / `IOServiceNameMatching("baseband")`，
没找到就放弃、不重试，那么：

- 参考：桥在 ABM 之前就在 → 找到；
- fuxi：桥在 ABM 之后才在 → 永远找不到；
- 150 s 时看到 0 user client，正是“一次错过”的样子。

ABM 是否重试，代码里没有证据；但 **0 user client + `abmtool` 不在 + 桥 22/22 在** 这三件事同时成立，
最省事的解释就是“它找的时候没有”。判决只需一个时间戳。

### 4.4 低成本修法（不改 ABI）

1. 把 `coachz_baseband_configure` 从 `fuxi_modem_publish` 里拆出来，在 `stepAdapter()` 之前
   （甚至 `native_fuxi_modem_probe` 一开始）调用；`bb_send` 先给一个返回 `-1` 的桩，
   链路起来后再替换。
2. `coachz_baseband_configure` 现在有 `if (!coachz_baseband_enabled() || bb_published) return;`
   （`coachz_baseband.cpp:1427`），第二次调用不会替换 `bb_send`；需要拆成
   `coachz_baseband_publish()` + `coachz_baseband_set_send()`，或者让 configure 在已发布时只更新 hook。
3. `waitLink` 超时不要 `goto failed`；继续 `serviceLink()`，让桥仍然发布，modem 的延迟由日志说明。
4. `fuxi_modem_poll` 的 `if (!link->channel) return;`（`fuxi_modem.c:766-768`）意味着发布提前后，
   链路没起来时 `coachz_baseband_poll` 也不会跑；请求会留在 `bb_txq`（64 条），等链路起来再发。
   这是可接受的，但要在日志里报“队列里有多少”。

---

## 5. 缺口三：ABM 打开之后，为什么仍然不会有数据

这一层不解释“入口点不进”，但如果 ABM 明天打开，它会立刻成为下一个“蜂窝不可用”。

### 5.1 umi 有、fuxi 没有的桥内修补

| umi 的机制 | 作用 | fuxi |
|---|---|---|
| `umi_bb_pending`（`umi_bb_pending.h`） | 请求转给 X55 后未被回答时，8 s 兜底回答，避免 CommCenter 永久等待 | ❌ 无 |
| 64 槽客户端池化 / 端口归属 | 53 个客户端反复开关不耗尽 | ⚠️ `CBQ_MAX_CLIENTS=64`，无池化 |
| WDS 失败指示合成 | Context 失败时给 CommCenter 一个明确结果 | ❌ 只观察，不合成 |
| DMS 4/5→1（reset/shutdown 视为 low power） | X55 的 bring-up 语义 | ⚠️ 有 DMS 0x2d/0x2e，但不是同一套 rewrite |
| 每请求必有回答 | 防止 `ATCS_TIMEOUT` | ⚠️ 私有服务回 `NOT_SUPPORTED`，其余靠 modem |

### 5.2 数据面完全没起

- `coachz_modem_data_send` 返回 `ENETDOWN`（`fuxi_modem.c:1255-1259`），
  `coachz_modem_data_receiver` 只是一个空指针。
- IPA v5.5 的寄存器读在时钟、互联、固件都投了之后仍同步外部中止
  （`FuxiModemProbe.cpp:3042-3049`），所以 `fuxi_ipa.c` 只有 62 行，数据面没有。
- `coachz_bb_pdp.cpp` 的 `pdp_ip` 是壳：能发布接口，但没有 IPA 的 QMAP 收发。
- 结果：即使 ABM 走到 `qmux1`、UIM/NAS 能读卡和注册，`pdp_ip0` 也发不出包；
  Settings 里最多显示“有信号/有运营商”，不能上网。

### 5.3 这不是当前症状

当前症状是“设置里有蜂窝入口、点不进去、Carrier: No available”。这是 **CommCenter 没有基带**，
不是“有基带但没数据”。所以 IPA 不是第一优先级；先让 ABM 打开。

---

## 6. “设置里能进蜂窝”到底证明了什么

`Settings` 的蜂窝入口来自 **cellular CommCenter job 被加载**（`/baseband` 节点 +
`_LimitLoadToDeviceTree`），不是来自 ABM。它证明：

- `/baseband` 节点在设备树里；
- launchd 选了 `com.apple.CommCenter.plist`（不是 `-noncellular`）；
- CommCenter 进程在跑。

它 **不证明**：

- CommCenter 选了 `C2KRadioModule`（而不是 `NoBBRadioModule`）；
- ABM 初始化了；
- `abmtool` 注册了；
- `AppleBaseband` 被打开。

`Carrier: No available` 是“没有任何基带状态可读”的 UI 表现，正好对应“0 user client”。
所以 **UI 症状和代码断点是同一件事**，不是两个问题。

---

## 7. 根因排序与判决实验（汇总）

### H1（最可能）CommCenter 仍走 NoBBRadioModule

- **证据**：参考实现里根 `compatible=J121AP` 是模块选择的关键开关；fuxi 已设，但属性是 31 字节
  多串（`J121AP\0xiaomi,fuxi\0qcom,sm8550\0`），参考是 7 字节单串（`J121AP\0`）。如果
  `HWModelStr` 按整段构造字符串，31 字节不等于 `J121AP`。没有 C2K/NoBB 日志；`abmtool` 不在、
  0 user client 与“模块没选”完全一致。
- **反证**：`carrierspace.xpc` 已注册、Settings 有蜂窝入口；但这只证明 job 加载。
- **实验**：拿到 `#I ----------------- Starting C2KRadioModule ----------------` 或
  `#I ----------------- Initializing NoBBRadioModule ----------------`。
- **若 NoBB**：先做 §2.3 的单串实验——把 ADT 构建器从“前缀”改成“替换”，让根
  `compatible` 只有 `J121AP\0`（7 字节），再开机看模块是否变成 C2K。若系统因板级匹配起不来，
  回退并考虑“只给 CommCenter 看的属性”方案。

### H2 `basebandd`/`abm-helper` 的按需 job 没 spawn（`_wireless` home 缺失）

- **证据**：代码不扫 `basebandd`；`REGISTERED` 不是进程；`abmtool` 不在；`abm-helper.log` ENOENT。
  `_wireless` 的 home 是 `/var/wireless`，Data seed 很可能没有这个空目录。
- **实验**：进程扫描 + manifest job 解析 + `stat("/private/var/wireless")`；
  若不存在，`mkdir` + `chown(25,25)`，**在 demand lookup 之前**，再跑一轮。
- **判据**：home 补上后 `basebandd`/`abm-helper` 进程出现、`abmtool` 出现 → H2 成立。
- **若仍不在**：查 job 的 `Program`/`UserName`/设备树条件；必要时临时加一条 root 的
  `basebandd` job（不是 `abm-helper`）验证。

### H3 桥发布晚于 ABM 的查找

- **证据**：三条代码的发布顺序不同；fuxi 最晚；ABM 若一次性查找就永远错过。
- **反证**：modem 若在 20 s 前 publish，就不成立；本轮 150 s 已 link up，但 publish 的具体时间未知。
- **实验**：完整日志里 `COACHZ baseband: 22 of 22 services published` 与
  `FUXI_MODEM: CommCenter is running at 30 s` 的先后；或加 `waitLink` 时间戳。
- **修法**：提前 publish + 拆分 send hook（§4.4）。

### H4 `waitLink` 10 s 放弃

- **证据**：代码确定；本轮日志 link up，所以本轮不是。
- **实验**：看 `the link did not come up within 10 s`。
- **修法**：超时后继续 `serviceLink`，不要 `goto failed`。

### H5 ABM 之后的桥/IPA

- **证据**：umi 的 pending/pool/WDS 没有；`ENETDOWN`；IPA abort。
- **实验**：只有 H1–H4 解决、`AppleBaseband user client` 出现后才做。

---

## 8. 具体改动轮廓（按顺序，先观测后改行为）

### 8.1 只加观测（不改桥）

0. **先做 §2.3 的单串实验**：ADT 的根 `compatible` 只写 `J121AP\0`，看 `C2KRadioModule` 是否出现。
   这是唯一能直接改“模块选择输入”的实验，优先级高于其他观测。
1. `fuxi_commcenter_report`：加 `basebandd`；把 30/120 s 改成 30/60/120/150 s。
2. `fuxi-logdump.m`：
   - `stat` `/private/var/wireless`、`/var/root`、`/usr/libexec/basebandd`；
   - 解析 `launchd.plist`，打印 `com.apple.basebandd.xpc` / `com.apple.abm.helper.*` 的 job；
   - 60/120/150 s 的 `sysctl(KERN_PROC_ALL)` 摘要；
   - `fuxi_logdump_read` 读 `va_modtime`，证明文件是本轮。
3. `fuxi_modem_report`：打印 `link->published` 的时间（`waitLink` 成功那一刻）和
   `coachz_baseband_configure` 的调用点；现在 publish 行没有秒数。

### 8.2 再改发布时机

1. `coachz_baseband.cpp`：拆 `coachz_baseband_publish()` 与 `coachz_baseband_set_send()`；
   发布不依赖 `bb_send`。
2. `FuxiModemProbe.cpp`：`stepAdapter` 之前发布；`waitLink` 超时不再 `goto failed`，
   继续 `serviceLink`。
3. `fuxi_modem.c`：`fuxi_modem_publish` 只设置 send hook；`published` 由发布函数管。

### 8.3 最后才补 ABM 之后的桥 / IPA

1. `umi_bb_pending` 等价物（每请求兜底回答）。
2. WDS 失败指示、DMS 模式 rewrite（按 umi 的实机结论）。
3. IPA v5.5 表、端点 24/22、TLV 0x23、GSI、QMAP、`pdp_ip`。

---

## 9. 验收判据

**“ABM 起了”**（全部出现，按顺序）：

```text
FUXI_MODEM: ... basebandd running (pid ...)
# abmtool REGISTERED
COACHZ baseband: AppleBaseband user client 1, type ...
COACHZ baseband: AppleBaseband selector 6 ...
COACHZ baseband: pbl register 3 read, ...
COACHZ baseband: pbl sendImage ... bytes dropped
COACHZ baseband: sahara hello
COACHZ baseband: qmux1 opened
```

**“蜂窝控制面可用”**：

```text
COACHZ baseband: the DMS answer: error 0, ... TLVs
COACHZ baseband: the radio is ... (online/low power)
COACHZ baseband: the UIM answer: error 0, ... TLVs
COACHZ baseband: ... clients ... (>0)
```

**“蜂窝数据可用”**（现在不投）：

```text
COACHZ pdp: WDS ... active 1 ...
COACHZ baseband: ... pdp_ip0 up ...
```

---

## 10. 一句话

> 代码对比的结论不是“桥缺一块”，而是 **桥一直在等一个永远不会来的 ABM，而 ABM 的宿主和模块选择
> 这个端口从来没有真正测过**。先把 `basebandd`、`C2KRadioModule`、publish 时间戳三件事测出来；
> 在这之前补 IPA 或补 umi 的 pending，都是在给一个还没开门的屋子装家具。

---

## 附录：代码位置

| 内容 | 位置 |
|---|---|
| 进程扫描（无 `basebandd`） | `UniXNU/drivers/QcomBaseband/platform/fuxi/fuxi_modem.c:1076-1096` |
| 服务探测（`REGISTERED`） | `package/fuxi-logdump.m:191-204` |
| publish 门 | `fuxi_modem.c:844-847` |
| poll 早退 | `fuxi_modem.c:766-768` |
| `waitLink` 10 s | `package/FuxiModemProbe.cpp:1628-1650` |
| `ownerThread` 失败路径 | `package/FuxiModemProbe.cpp:2681-2688` |
| configure / `bb_published` | `UniXNU/drivers/QcomBaseband/baseband/coachz_baseband.cpp:1425-1445` |
| `/baseband` 属性复制 | `coachz_baseband.cpp:1388-1422` |
| umi 早发布 | `hakios-xnu-kernel-umi-fixes/.../umi_modem.c:296-298` |
| Trogdor GLINK start 发布 | `Trogdor-ipadOS/.../coachz-prelink-baseband.patch:129-133` |
| umi pending/pool/DMS | `hakios.umi_baseband.cpp:118, 280-286, 1099-1100` |
| fuxi 数据面 `ENETDOWN` | `fuxi_modem.c:1255-1259` |
| IPA mode 18 说明 | `package/FuxiModemProbe.cpp:3042-3049` |