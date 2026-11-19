# 基带移植映射表

参考机型、主板线内核与 Fuxi device repo 之间的文件级映射。每行给出参考侧的来源、目标侧落点和改写等级。

改写等级：**A** 直接复用 | **B** UniXNU API 适配 | **C** SM8550/Fuxi 硬件适配 | **D** 重写。

## 放置规则

组件 `UniXNU/drivers/QcomBaseband/AGENTS.md` 规定：

> Maintain the baseband IOKit bridge, QMUX/QRTR protocols and IPA data path.
> **Board power, modem boot and firmware packaging belong in the device or SoC repository.**

所以：

| 归属 | 内容 |
| --- | --- |
| `UniXNU/drivers/QcomBaseband/` | IOKit 桥、QRTR/QMUX 协议、IPA 数据面 |
| `unixnu-fuxi/package/` | 板上电、modem PAS 启动、固件打包、SMEM/IPCC/RPMh 实参 |
| `UniXNU-sm8550-common/` | SoC 共用资源（当前无基带内容） |

## 传输层

| 参考 | 来源 | 目标 | 等级 | 说明 |
| --- | --- | --- | --- | --- |
| GLINK-SMEM 传输 | Linux `drivers/rpmsg/qcom_glink_smem.c` | `unixnu-fuxi/package/QcomGlink.c` | A | 描述符四字（tx_tail/tx_head/rx_tail/rx_head）与 32 字节大小已核对 |
| SMEM 查表/分配 | Linux `drivers/soc/qcom/qcom_smem.c` | `unixnu-fuxi/package/QcomSmem.c` | A | |
| MDT 解析 | Linux `drivers/soc/qcom/mdt_loader.c` | `unixnu-fuxi/package/QcomMdt.c` | A | |
| RPMh ARC 资源 | Linux `drivers/soc/qcom/cmd_db.c` + `rpmh.c` | `unixnu-fuxi/package/QcomRpmh.c` | A | |
| RPMh 寄存器与 TCS | Linux `drivers/soc/qcom/rpmh-rsc.c` | `unixnu-fuxi/package/QcomRpmh.c` | A | 两个 RSC 寄存器布局、TCS 的选择与触发顺序 |
| AOSS QMP | Linux `drivers/mailbox/qcom_aoss.c` | `unixnu-fuxi/package/QcomQmp.c` | A | |
| SMP2P | Linux `drivers/soc/qcom/smp2p.c` | `unixnu-fuxi/package/QcomSmp2p.c` | A | |
| **QRTR 协议** | Linux `net/qrtr/af_qrtr.c` | `UniXNU/drivers/QcomBaseband/platform/fuxi/qcom_qrtr.c` | **D** | XNU 无 QRTR，按线格式重写（32 字节 `qrtr_hdr_v1`） |
| **GLINK 适配 + 桥接** | Trogdor `bsd/dev/coachz_wifi.c` | `UniXNU/drivers/QcomBaseband/platform/fuxi/fuxi_modem.c` | **D** | fuxi 树无该文件；Trogdor 把 QRTR 与 IOKit 桥写在一起，fuxi 拆成两层 |
| rpmsg 端点注册 | Linux `net/qrtr/smd.c` | `platform/fuxi/fuxi_modem.c` | B | Linux 用 rpmsg 匹配 "IPCRTR"，fuxi 直接按通道名开通道 |

## 启动序列

| 参考 | 来源 | 目标 | 等级 |
| --- | --- | --- | --- |
| PAS 引导 | Linux `drivers/remoteproc/qcom_q6v5_pas.c` (`qcom_pas_start`) | `unixnu-fuxi/package/FuxiModemProbe.cpp` | **C** |
| SCM64 SiP 调用 | 本仓 `package/QcomScm.h` + ADSP 已验证实现 | `package/FuxiModemProbe.cpp` | A |
| RPMh 投票 | 本仓 `package/QcomRpmh.h` | `package/FuxiModemProbe.cpp` | A |
| RPMh corner 语义 | Linux `drivers/pmdomain/qcom/rpmhpd.c` | `package/FuxiModemProbe.cpp` | A（见下） |
| ADSP PAS 模板 | 本仓 `package/FuxiAdspProbe.cpp` | `package/FuxiModemProbe.cpp` | B（换实参） |

## 实参来源

实参的唯一入口是 `facts/baseband/binding.toml`：每个值都带状态（confirmed / inferred / chosen / unverified）
与出处，出处本身在 `facts/baseband/manifest.toml` 里逐条说明"能证明什么、不能证明什么"。下表是同一份
内容的摘要。

| 值 | 来源 |
| --- | --- |
| `pas_id = 4`（modem 镜像） | Linux `qcom_q6v5_pas.c` `sm8550_mpss_resource` |
| `dtb_pas_id = 0x26`（modem DTB） | 同上 |
| 固件文件名 `modem.mdt` / `modem_dtb.mdt` | Linux 同上（secure world 要什么）；**真机 2026-11-19 确认板上就是这两个名字** |
| 分段 `modem.b00`–`modem.b31`，缺 b14、b21、b30，共 29 段 | **真机 2026-11-19**，`modem_a` 分区 `/image` 目录清点 |
| `proxy_pd_names = {"cx","mss"}` | Linux `sm8550.dtsi` `power-domain-names` |
| 内存洞 `0x8a800000/0x10800000` | Linux `sm8550.dtsi` `mpss_region@8a800000`；CAF live dts 同值 |
| 内存洞 `0x9b000000/0x80000` | Linux `sm8550.dtsi` `q6_mpss_dtb_region@9b000000`；CAF live dts 同值 |
| `IPCC_CLIENT_MPSS = 2` | Linux `include/dt-bindings/mailbox/qcom-ipcc.h` |
| GLINK 通道 `IPCRTR` | CAF live dts `qcom,modem_qrtr` |
| 意图 `0x800 0x05 0x2000 0x03 0x4400 0x02` | CAF live dts `qcom,intents` |
| SMP2P 项 `0x1b3/0x1ac` | CAF live dts `qcom,smp2p-modem` |
| `remote-pid = 1` | CAF live dts `glink-edge` |
| 轨 `cx.lvl` / `mss.lvl` | CAF live dts `rpmh-regulator-cxlvl` / `rpmh-regulator-msslvl` |
| ready 位 = bit 1、fatal 位 = bit 0 | Linux `sm8550.dtsi` `remoteproc_mpss` 的 `interrupts-extended` 顺序（wdog, fatal, ready, handover, stop-ack, shutdown-ack） |
| GLINK 项 478/479/480 与描述符四字顺序 | Linux `drivers/rpmsg/qcom_glink_smem.c` |
| QRTR 帧格式（32 字节 `qrtr_hdr_v1`） | Linux `net/qrtr/af_qrtr.c` |
| 本地节点 id 1、控制端口 `0xfffffffe`、广播节点 `0xffffffff` | 同上 |

**中断位的顺序**：CAF 的设备树比 Linux 多一个 handover（bit 2）。CAF 的 `interrupts-extended` 是
`0x77 0x00`、`0x77 0x02`、`0x77 0x01`、`0x77 0x03`、`0x77 0x07`，`interrupt-names` 依次是
wdog、fatal、handover、ready、stop-ack、shutdown-ack，所以第二个数才是位号：`0x77 0x00` 是 fatal，
`0x77 0x01` 是 handover，不是 ready。Linux 的 `sm8550.dtsi` 少一个 handover，因此 fatal 是 bit 0、
ready 是 bit 1。**`package/FuxiModemProbe.cpp` 用的是 Linux 的编号（fatal 0x1、ready 0x2），是对的。**

**`mss.lvl` 在本板**：2026-11-19 真机读到（`fuxi-modem=3`，只读命令数据库，不投票）。
`cx.lvl` 在 `0x30000`，10 个档；`mss.lvl` 在 `0x30060`，7 个档。之前"只有 gfx/mx/cx/gmxc"的说法是从
sheng 板推断的，对本板不成立。

**RPMh 投票的 corner 语义**（Linux `drivers/pmdomain/qcom/rpmhpd.c` + `drivers/soc/qcom/rpmh.c`）：

1. `sm8550_mpss_resource` **没有** `proxy_pd_performance_states`，所以 `qcom_pas_pds_enable` 传的
   performance state 是 `INT_MAX`。
2. `rpmhpd_set_performance_state` 把 `INT_MAX` 钳到 **最高档**：`level_count - 1`。
   于是 `pd->corner` = 最高档。
3. `rpmhpd_power_on` 投 `max(pd->corner, pd->enable_corner)`，enable corner 是**第一个非零档位**
   （跳过 Retention），它不会比最高档大，所以**投出去的是最高档**。
4. `state_synced` 假不改变结果：`rpmhpd_aggregate_corner` 在未同步时直接用
   `level_count - 1`，同步后用 `to_active_sleep(pd, corner)` 也是同一个值。两种情况相同。
5. 投到线上的是 **ARC corner 下标**：下标就是命令数据库 aux 数据里的**位置**。命令数据库给出一串
   硬件档位（vlvl），下标 0 是 Retention（`RPMH_REGULATOR_LEVEL_RETENTION = 16`），它在下标空间里
   **占着自己的位置**，只是它的档位值不被用来算 enable corner。
6. `cx` 有 peer `cx_ao`，`mss` 没有 peer。Linux 因此给 `cx` 记三种态（ACTIVE_ONLY、WAKE_ONLY、
   SLEEP），给 `mss` 只记一种。
7. **但只有 ACTIVE_ONLY 会上线**。`__rpmh_write` 只在 state 是 `RPMH_ACTIVE_ONLY_STATE` 时调
   `rpmh_rsc_send_data`；WAKE_ONLY 和 SLEEP 只进 `rpmh.c` 的缓存，等 CPU 进 idle 或挂起时
   `rpmh_flush()` 才写进 WAKE/SLEEP 的 TCS。modem 启动时 AP 还醒着，那两种票还没上线。
8. 顺序：Linux 先给 `cx` 投票、再给 `mss` 投票，各是一次独立的请求。**本仓把两条轨放在同一次
   请求里**，命令顺序也是 cx 在前。touch 探针在真机上已经证明这台机器的 RSC 能接受一条请求里的
   两条命令。

**结论：本仓 `voteRails()` 投的就是最高档，与 Linux 一致，不需要改。**
`qcom_rpmh_level_index(&cx, 0xffffffffu, &index)` 在档位都小于 `0xffffffff` 时返回
`level_count - 1`，正是最高档。

**尚未做的**：本仓没有 `rpmh.c` 的缓存，也没有在 idle/挂起时把 WAKE/SLEEP 票刷进 TCS 的地方。
这不影响 modem 启动（那时那两种票还没上线），但 AP 挂起再回来之后 modem 的 `cx` 域会停在
硬件当前记着的档位上。这属于 modem 起来以后的事。

**RSC 布局的版本必须强制为 major 3**（2026-11-19）：DRV2 的 ID 寄存器（`drv->base + 0`）在本板
读出**零**，于是按 ID 选布局会落到 v2_7 的偏移上。v2_7 与 v3_0 的 TCS 寄存器偏移不同
（`CMD_OFFSET` 20 vs 24，命令寄存器块 0x30/0x34/0x38 vs 0x34/0x38/0x3C，
wait/control/status/enable 0x10/0x14/0x18/0x1C vs 0x20/0x24/0x28/0x2C），选错就写不到真的
TCS 请求寄存器上。ADSP 探针早已因此加了强制回落（`gOwner.rsc_drv_id = gOwner.rsc_id2 ? gOwner.rsc_id2 : (3u << 16);`），
并在真机上证明这是能用的那一种：设备记录 `RSC: forced v3 layout after DRV2 ID read 0; active TCS 2; vote done`。
**没有强制回落的那一方，投票永远收不到 done 位**——touch 探针的 `tcs2 rc0 w-1` 就是这个现象
（票写进去了，done 位 10 ms 内没来）。modem 探针原先也缺这个回落，现已补上，与 ADSP 同形。

**PAS 的三个步骤与它们的先后**（Linux `drivers/remoteproc/qcom_q6v5_pas.c` `qcom_pas_start`
+ `drivers/soc/qcom/mdt_loader.c` `qcom_mdt_pas_load`）：

| 步骤 | SCM 命令 | 参数 | 什么时候 |
| --- | --- | --- | --- |
| INIT_IMAGE | `QCOM_SCM_PAS_INIT_IMAGE` | metadata 的物理地址 | 票之前 |
| MEM_SETUP | `QCOM_SCM_PAS_MEM_SETUP` | 镜像的基址与跨度 | **只在可重定位镜像时**；固定地址的镜像本来就知道自己在哪里，再问一次是错的 |
| AUTH_AND_RESET | `QCOM_SCM_PAS_AUTH_AND_RESET` | 无 | **分段搬进洞之后**，每个镜像一次 |

关键点：`qcom_mdt_pas_load` 的注释写明"Authentication of the segments done by a separate call"——
加载分段与认证复位是**两件事**，复位在分段进洞之后。Linux 对每个镜像的顺序是
INIT_IMAGE → MEM_SETUP → 搬分段 → AUTH_AND_RESET，**两个镜像各走一遍，先 DTB 后镜像**
（DTB 告诉 modem 自己的内存在哪里，必须先就位）。modem 探针原先把 AUTH_AND_RESET 放在
`authenticateHeader` 里，也就是**分段进洞之前就复位**，随后又在外层复位一次；现已改为
只在分段进洞之后复位一次，与 Linux 和 ADSP 已验证的路径同形。

**ADSP 与 modem 争同一条中断线和同一个 TCS**（2026-11-19）：两边的 bring-up 都要

| 资源 | ADSP | modem | 能否共享 |
| --- | --- | --- | --- |
| IPCC 中断（SPI 229 → INTID 261） | attach | attach | **不能**：中断线一次 boot 只能 attach 一次，`armcpu_spi_attach` 对已用的 INTID 返回 `kIOReturnExclusiveAccess`，且**没有 detach** |
| RSC 中断（SPI 5 → INTID 37） | attach | attach | **不能**，同上 |
| RSC 的活动 TCS（DRV2 的 TCS 2） | 投票 | 投票 | **不能**：一个 TCS 就是一个请求槽，两个主人在上面写请求会互相破坏 |
| AOSS QMP 邮箱（0xc300000，phandle 0x43） | 用 | 用 | 能：它是一个 0x400 字节的共享 RAM，不是独占资源 |
| GLINK 的 SMEM 项 478/479/480 | remote pid 2 | remote pid 1 | 能：`qcom_smem_alloc` 按 **remote pid** 分项，同一个 item id 对两个 remote pid 是两条 |

`armcpu_spi_attach` 在 `package/ArmCpu.cpp:836`，`ARMCPU_SPI_SLOTS` 是 16。
已核对过不冲突的占用者：touch 占 240（32+208）、按键占 513（32+481）、GPU 占 332，
都和 261 与 37 无关；基带组件 `UniXNU/drivers/QcomBaseband` 不 attach 任何中断，它是轮询的。

**因此本轮只跑 modem**：`board.toml` 把 `fuxi-adsp` 从 2 改成 1（模式 1 只盘点 DSP 的
固件，只读，不 attach 中断、不映射窗口），把 `fuxi-modem` 从 3 改成 4（运行时路径）。
`bsd_init` 里 `native_audio_probe()` 排在 `native_fuxi_modem_probe()` 之前，所以只要
ADSP 还开着运行时，它会先占住那两条线，modem 的 `stepIpcc` 就会以 -74 失败。
ADSP 的运行时重新打开之前，modem 和 ADSP 必须共用一个主控。

## 待查的上游源码

<!-- 列出要先读上游才能定下来的问题。读完把答案写回上面的表或 facts/baseband/，然后把这一条删掉。 -->


## 桥接与 IPA

| 参考 | 来源 | 目标 | 等级 |
| --- | --- | --- | --- |
| Apple baseband 桥 | Trogdor `bsd/dev/coachz_baseband.c` | `UniXNU/drivers/QcomBaseband/baseband/coachz_baseband.cpp` | A（组件自带） |
| QRTR 节点发现 | Trogdor `coachz_wifi.c` `wq_send`/`wq_ctrl` | `platform/fuxi/qcom_qrtr.c` | D |
| IPA v5.5 数据面 | 上游组件 `ipa/`（SC7180 v4.2 表） | 待重生成 | D（未做） |

## 已在真机证明过的地方

只列真机结果，标"主机"的不算。

| 事实 | 出处 |
| --- | --- |
| SMC 成功判据必须严格 `x0 == 0 && x1 == 0` | ADSP 的 R-SCM-BUG-01 / R-SCM-STRICT-01（`docs/fuxi-audio-test-results.md`） |
| `0x00cfffe5` 这类正数 cookie 不是成功值 | 同上：无效 PAS id 返回同一个 cookie |
| 底低内存必须固定映射 | 音频链上 0xd0000000/64 KiB，运行时分配落到 0x8CFF84000 后失败 |
| 载带 `libQcomBaseband.a` 的内核能启动到 launchd | `evidence/2026-11-18-baseband-first-boot.md` |

尚未在真机证明的：modem 的 PAS id 4 与 DTB id 0x26、GLINK 项 478/479/480、SMP2P 项 0x1b3/0x1ac、
IPCC client 2。这些都已从一手出处确认来源，但没有跑过真机。

已经在真机证明的（2026-11-19）：固件文件名 `modem.mdt` 与 `modem_dtb.mdt` 以及分段布局，
`modem_a` 分区的 `/image` 目录由内核的 `fuxi-modem=1` 只读清点列出，
证据 `evidence/2026-11-19-modem-firmware-inventory.log`。

**运行时路径第一次跑通到第 3 阶段**（2026-11-19，`fuxi-adsp=1 fuxi-modem=4`，
证据 `evidence/2026-11-19-modem-runtime-first-boot.md`）：

| 事实 | 怎么证的 |
| --- | --- |
| RSC 强制 major 3 布局在真机上是对的 | 阶段 3（rails-voted）是第一个需要投票完成的阶段，投票在强制 major-3 的布局上写出并完成 |
| ADSP 关掉之后 modem 能独占中断线 | `stepIpcc` attach 了 IPCC 261 与 RSC 37，没有 `kIOReturnExclusiveAccess` |
| DTB 的 INIT_IMAGE 被 secure world 拒绝 | `scm 4291821499/0` = `0xFFCFFFBB`，探针发布的低半是 4027（0xFBB） |
| 没有动 RF、没有写分段 | `loaded_segments=0`，SMP2P 的 ready 与 fatal 位都是 0 |

调用形状与 ADSP 已验证的那次相同：同一条命令、同一个参数描述符（两个参数，第二个读写）、
同样的参数顺序，metadata 缓冲区也在高 DRAM 的连续段里。两次 bring-up 的差别只有 PAS id
（DTB 是 0x26）和镜像本身。当前探针在第一次拒绝处就停下，不说 secure world 觉得哪个参数不对。
