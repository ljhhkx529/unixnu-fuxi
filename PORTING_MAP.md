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
