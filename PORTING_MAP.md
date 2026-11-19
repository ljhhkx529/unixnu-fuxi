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
| 分段 `modem.b00`–`modem.b31`，缺 b14、b21、b30，共 29 段 | **真机 2026-11-19**，固件分区 `/image` 目录清点；两个槽都存固件且构建不同 |
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

**本板匹配的是 `qcom,sm8550-mpss-pas`，由 `qcom_q6v5_pas.c` 处理**（不是旧的
`qcom_q6v5_mss.c` 那条 RMB 路径；那条路径的 PAS id 是 5，本板不是）。上游
`qcom_pas_start()` 的八步与本仓 `stepPas()` 的对照：

| 上游 `qcom_pas_start()` | 本仓 | 说明 |
| --- | --- | --- |
| `request_firmware(dtb_firmware_name)` | `openImage("modem:modem_dtb")` | 上游取 `modem_dtb.mdt` |
| `qcom_q6v5_prepare()` | `stepIpcc()` 之前的准备 | 中断与 TCM 保持 |
| `qcom_pas_pds_enable()` + `clk_prepare_enable(xo/aggre2)` + `regulator_enable(cx)` | rails 投票（模式 3/4） | `cx` 与 `mss` 两个 proxy pd |
| `qcom_pas_prepare_and_auth_reset(dtb_pas_ctx)` | DTB 的 AUTH_AND_RESET | **在主镜像之前** |
| `qcom_mdt_pas_load(pas_ctx, firmware)` | `authenticateHeader()` + `loadSegments()` | INIT_IMAGE / MEM_SETUP / 搬分段 |
| `qcom_pas_prepare_and_auth_reset(pas_ctx)` | 镜像的 AUTH_AND_RESET | 分段进洞之后 |
| `qcom_q6v5_wait_for_start()` | `stepQmp()` 前的等待 | AOP 握手 |
| `qcom_pas_metadata_release()` | `releaseImage()` | metadata 在认证后才释放 |

**DTB 那一步到底搬不搬分段（2026-11-20 在上游 7.4-rc3 里核对过）**：
上游的 DTB **也搬分段**。`qcom_pas_load`（`qcom_q6v5_pas.c:280`）对 DTB 调的是
`qcom_mdt_pas_load(pas->dtb_pas_ctx, ...)`，而 `qcom_mdt_pas_load`（`mdt_loader.c:486`）
在 `__qcom_mdt_pas_init`（INIT_IMAGE + MEM_SETUP）之后**继续调 `qcom_mdt_load_no_init`**，
那个函数就是把可加载分段 `memcpy` 进 carveout（分裂的 `.bNN` 走 `mdt_load_split_segment`）。
所以上游一个 image 的完整顺序是 **INIT_IMAGE →（MEM_SETUP，仅可重定位）→ 搬分段 →
AUTH_AND_RESET**，DTB 与主镜像同形。
**本仓的 `stepPas()` 本来就是这个顺序**，因此"DTB 改成上游顺序"这条改动**不需要做**；
真正与上游不一致的是 **metadata 的释放时机**：上游把两块 metadata 都留到
`qcom_q6v5_wait_for_start()` 之后才放（`qcom_pas_start` 的 379-381 行），因为 SCM 调用期间
安全世界会为那块页开内存保护；本仓原先在 DTB 认证完就放掉了它，2026-11-20 起改为
等 modem 起来之后两块一起放。

> 分析文档 `secure-world-拒绝分析-0xFFCFFFBB.md` 的 §2.2 与 §3.1 说"上游的 DTB 不走
> `qcom_mdt_load_no_init`，分段不进 carveout"——**这是错的**，上游搬。若照它改，方向反而
> 背离上游。该文档 §7 说 region assign 的参数块"需要安全内存"——**也是错的**，见上文。

**metadata 的来源是 `.mdt` 文件，不是拼起来的镜像**（Linux
`drivers/soc/qcom/mdt_loader.c` `qcom_mdt_read_metadata`：先 `memcpy(data, fw->data, ehdr_size)`
把 ELF 头抄进缓冲区，再按 `ehdr_size + hash_size == fw->size` 这个**打包分支**把 hash 段
从 `fw->data + ehdr_size` 抄过来；上游的 `fw` 就是 `.mdt` 文件本身）。本仓原先是拿
`.bNN` 拼出来的 joined image 去凑 metadata，走的是第二个分支（hash 段在文件里的 `p_offset`），
于是取 hash 的位置与上游不同。上游的哈希段在搬分段时又是从 `.bNN`
里取的（`mdt_load_split_segment`），所以两个来源都要有，不能混。

**字节上差多少，已经测出来了（2026-11-19 真机，模式 11）：** 紧凑形式与分片形式
**4044 字节里 0 字节不同**，modem_dtb 与 adsp_dtb 都是。所以这台机器上"从 `.mdt` 取"与
"从拼起来的镜像取"给安全世界的是**同一批字节**——这个改动与上游一致，但在这块板上是字节上的
空操作，**不是被拒的原因**。之前的"两者相同"这条是用两个分支一样的 `memcmp` 写的，
不可能失败、什么也没证明，已撤回；现在这条是数差异字节数得出的，能失败。

**2026-11-19 真机：改了来源，拒绝不变。** 日志写明 `from the mdt file`，错误码仍是
`0xFFCFFFBB`。所以 metadata 的来源不是安全世界拒绝的理由。

**metadata 缓冲区的地址宽度：查过了，四项都查完了，结论是宽度不是拒绝的理由。**

| 检查项 | 结论 | 出处 |
| --- | --- | --- |
| `0x8d5acf000` 是什么地址 | **物理地址**。`getPhysicalSegment(0, nullptr, kIOMemoryMapperNone)` 走 `getPhysicalSegment64` 的分支，返回的是页帧号换算出来的物理地址，不是虚拟地址，也不是 IOMMU 后的 DMA 地址 | `package/FuxiModemProbe.cpp` 的 `authenticateHeader`；XNU `IOMemoryDescriptor.cpp` |
| 分配它的设备的 `dma_mask` 与 `coherent_dma_mask` | 上游的 SCM 平台设备**默认就是 32 位**：`setup_pdev_dma_masks` 把 `coherent_dma_mask` 设成 `DMA_BIT_MASK(32)`，`dma_mask` 指向同样是 `DMA_BIT_MASK(32)` 的 `platform_dma_mask`；`qcom_scm_probe` 自己一个掩码都没设 | Linux `drivers/base/platform.c:571`、`drivers/firmware/qcom/qcom_scm.c` |
| `dma_alloc_coherent()` 的返回值与地址宽度 | 上游拿到的是 `dma_addr_t`（64 位类型），但被设备的 32 位掩码约束，所以**要么落在 4 GiB 以下，要么分配失败**。本仓的地址是 64 位值，通过 SMC64 的 x3 寄存器完整传下去，**SMC 编码这一层不截断** | `qcom_scm_pas_init_image` 的 `desc.args[1]`（`u64 args[]`）；`package/QcomScm.h` |
| 上游用什么分配机制 | **`dma_alloc_coherent`**，就是 coherent DMA API。**不是 CMA，不是 reserved-memory**——保留内存是固件自己的洞（`qcom_pas_alloc_memory_region` 用 `of_reserved_mem_region_to_resource` 取得），装的是分段，不是 metadata | Linux `drivers/firmware/qcom/qcom_scm.c`、`drivers/remoteproc/qcom_q6v5_pas.c` |

**上游还有一条分支本仓用不上**：`use_tzmem` 为真时走 `qcom_tzmem_alloc`（TrustZone 内存池）。
它是 `desc->needs_tzmem || rproc->has_iommu` 的结果，而 `remoteproc_mpss` 节点**没有 `iommus`
属性**（`sm8550.dtsi`），`sm8550_mpss_resource` 也没设 `needs_tzmem`，所以本板是 `false`，
上游在这块板上也用 `dma_alloc_coherent`。

**这台板上没有 4 GiB 以下的 DRAM 可分配**（`../uefi-fuxi` 的 `MemoryMapLib.c`）：
最低的 AddMem DRAM 行是 `DRAM 0x811d0000`（0x30000 字节），4 GiB 以下其余的全是保留区
（`mpss_region` 0x8A800000、`q6_mpss_dtb_region` 0x9B000000、`mpss_dsm_region` 0xD4D00000）。
所以主线那个 fuxi 移植必须开 `CONFIG_ZONE_DMA`/`CONFIG_ZONE_DMA32` 才分配得出 metadata，
这是硬件事实，本仓绕不开。

**但宽度不是 secure world 拒绝 modem blob 的理由**（PAS 矩阵，`fuxi-modem=5`）：

| blob | metadata 地址 | PAS | 结果 |
| --- | --- | --- | --- |
| `adsp_dtb` | 0x8d0b1f000 | 36 | **0/0 接受** |
| `modem_dtb` | 0x8d0b1e000 | 38 | 0xFFCFFFBB 拒绝 |
| `modem_dtb` | 0x8d0b1e000 | 36 | 0xFFCFFFBB 拒绝 |

两个地址相邻，**都在 4 GiB 以上**。如果 secure world 只能访问 32 位地址，两个会一起失败；
事实是一个被接受。所以**地址宽度区分不了被接受的 blob 与被拒绝的 blob**，区分它们的是镜像内容
（adsp 的是 relocatable，modem 的是 fixed）。低地址分配的改动**不会修好 modem 的拒绝**。

**本仓与上游的差别，以及要不要改**：本仓的掩码是 `0xfffffffffffff000`（不约束），上游是 32 位。
这是真实差别，但按上面那条，它不是拒绝的原因。**本轮不改分配**：理由是改它需要新开一个
低 DRAM 窗口（音频链占着 0xd0000000 那 64 KiB），而它换不来任何被拒绝的 blob 的改善。
若将来要用，正确的做法是走 XNU 的掩码机制（`NativeAdrenoMemory.cpp` 的写法：把掩码限制到
4 GiB 以下，让分配器自己选），**不是硬编码一个低地址覆盖现有分配**。

**之前记的"metadata 地址在这块板的 DRAM 之外"是错的，已撤回**：UEFI 的内存图
（`../uefi-fuxi/Platforms/Xiaomi/fuxiPkg/Library/MemoryMapLib/MemoryMapLib.c`）有
`{"DRAM 0x8c0000000", 0x8C0000000, 0x140000000, ...}` 一行，覆盖 0x8C0000000..0xA00000000，
0x8d5acf000 就在里面，是普通内存。真正为真的那句话更窄：**这块板没有 4 GiB 以下的 DRAM 可分配**。

**主线 Linux 的 fuxi 移植在同一块板上撞到过一模一样的问题并修好了**：它的
`modem_dtb` PAS 初始化返回 **-12**，原因是 `dma_alloc_coherent` 分配那 4044 字节返回 NULL——
系统内存超过 4 GiB，而 `CONFIG_ZONE_DMA`/`CONFIG_ZONE_DMA32` 关着，没有 32 位的
coherent 区可退；打开这两项后 modem 跑起来了（主线树 `/tmp/x13` 的
`docs/交接-新会话-20261030-2110.md`）。那个移植在这块板上能起 modem，
所以硬件侧在那边已经定了。

本仓该走自己的、已经验证过的配方：**固定低 DRAM 映射**（音频链已证明可用：
`0xd0000000`、`kIOMapAnywhere` 必需）。metadata 是 4044 字节，且上游要求 4 KiB 对齐，
放得下。**未实现**：那个低 DRAM 窗口是 64 KiB 且已被音频链占用，需要另开一个窗口。

`modem_dtb` 的实测形状：`.mdt` 4044 字节（148 字节头 + 3896 字节 hash），joined blob
7992 字节、hash 在 4096（`.b02`）。两个 `.mdt` 都满足上游的打包分支，所以上游拿到的
就是 `.mdt` 里的 hash。

**上游与本仓的差别，2026-11-20 逐项核对后的状态**（`qcom_q6v5_pas.c` 与 `qcom_scm.c`）：

| 上游的做法 | 本仓 | 为什么要紧 |
| --- | --- | --- |
| `qcom_pas_assign_memory_region()`：把设备树里第 3 块保留内存（`mpss_dsm_mem@0xd4d00000`，`0x3300000`，DSM 区）从 HLOS（vmid 3）交给 modem 的 MSA（vmid 0xF），**在任何 PAS 调用之前** | **已做**（`package/FuxiModemAssign.h` + `stepAssign()`，2026-11-20，真机 `scm 0/0` 接受） | 这是 **modem 独有**的：ADSP 的 `sm8550_adsp_resource` 没有 `region_assign_idx`。所以"ADSP 接受、modem 拒绝"不能用 ADSP 的成功来论证这一步可省。**做完并被接受后，拒绝没有变，所以这一步不是原因** |
| 每个 PAS 调用前后的 `qcom_scm_clk_enable()`（SCM 的 core/iface/bus 三个时钟）与 `qcom_scm_bw_enable()`（总线带宽 `icc_set_bw`） | 没有 | 调用本身在 2026-11-19 真机上成功过（ADSP 接受），说明这一步不是 modem 被拒的原因，但它仍是上游的规矩。**这是本仓与上游的序列之间最后一处差别** |
| PAS 三个步骤的参数类型字（`0x82` / `0x03` / `0x01`） | **一致**（`package/FuxiModemPas.h` 的 `fuxi_modem_pas_arginfo()`） | 2026-11-20 逐字核对 |
| metadata 从 `.mdt` 文件取（上游的打包分支） | **一致**（`package/QcomMdt.c` 的 `qcom_mdt_metadata_from_mdt()`） | 2026-11-19 核对 |
| 探针顺序：分配洞 → region assign → 时钟 → proxy pd → `rproc_add` | **一致**（`stepAssign()` → `stepRails()` → `stepPas()`） | 2026-11-20 核对。DTB 的 INIT_IMAGE 在上游属于**启动阶段**（rails 之后），本仓同 |
| fixed（不可重定位）镜像：少发 MEM_SETUP，分段落点用洞的物理基址 | **一致**（`fuxi_modem_pas_header_steps()`、`qcom_mdt_place()`） | 2026-11-20 核对 |
| `qcom_pas_shutdown(lite_pas_id)` / `lite_dtb_pas_id` | 不需要 | `sm8550_mpss_resource` 没有这两个字段，上游自己也不发 |

**region assign 的形状（2026-11-20 在上游 7.4-rc3 里逐行读出来的，`package/FuxiModemAssign.h` 是它的逐字实现）**：

- 调用走的是**内存保护服务**（svc `0x0c`）的 assign 命令（cmd `0x16`），不是 PAS 服务。
- **七个参数**（`__qcom_scm_assign_mem` 的 `desc.args[]`）：
  `args[0]=mem_to_map 的物理地址`、`args[1]=16`、`args[2]=参数块的物理地址`、
  `args[3]=src 的大小(4)`、`args[4]=dest 的物理地址`、`args[5]=dest 的大小(24)`、`args[6]=0`。
- **参数类型的字**是 `QCOM_SCM_ARGS(7, RO, VAL, RO, VAL, RO, VAL, VAL)` = **0x1117**
  （计数在 bits 3:0，参数 i 的类型在 bits 5+2i:4+2i，`VAL=0`、`RO=1`）。
  本仓 `QcomScm.h` 的 `qcom_scm_args` 只编码 3 个类型，所以这个字单独算在 `FuxiModemAssign.h` 里。
- **七个参数怎么进寄存器**（`qcom_scm-smc.c`）：`x0` 是函数 id、`x1` 是类型字，
  `desc.args[0..3]` 进 `x2..x5`。**参数多于四个时**，`desc.args[3..9]` 被复制成 7 个 64 位
  小端值放进一块内存，**这块内存的物理地址覆盖 x5**（x5 原本的 `args[3]` 被冲掉）。
  所以本调用是 `x2=mem_to_map 地址`、`x3=16`、`x4=参数块地址`、`x5=外接参数块地址`，
  外接块里依次是 `src 大小`、`dest 地址`、`dest 大小`、`0`、`0`、`0`、`0`。
  quirk 的会话寄存器是 x6，所以外接块的地址**不能**放 x6——上游放 x5，本仓也一样。
- **参数块的三段**（`qcom_scm_assign_mem`）：
  源 owner 表（每个 4 字节 vmid）在 0；`mem_map_info`（`le64 mem_addr` + `le64 mem_size`，共 16 字节）
  在 `ALIGN(src_sz,64)`=64；`current_perm_info`（`le32 vmid` + `le32 perm` + `le64 ctx` +
  `le32 ctx_size` + `le32 unused`，共 24 字节）在 `ALIGN(16,64)+ALIGN(4,64)`=128。
  整块 `ALIGN(src,64)+ALIGN(16,64)+ALIGN(24,64)` = **192 字节**。
- **本板的取值**：源 vmid 3（HLOS）、目的 vmid 0xF（MSS_MSA）、权限 0x6（读 0x4 + 写 0x2）、
  区域 `0xd4d00000`/`0x3300000`。`sm8550_mpss_resource` **没有** `region_assign_shared`，
  所以目的 owner **只有一个**（ADSP 那边根本没有 `region_assign_idx`）。

**参数块不需要"安全内存"**——这一点纠正了此前分析文档的说法。上游把它放在 SCM 的
tzmem 池里（`qcom_tzmem_alloc`），但 `qcom_tzmem_pool_add_memory`（`qcom_tzmem.c:207`）
底层就是 `dma_alloc_coherent`，而 `qcom_tzmem_init_area` 只有在
`CONFIG_QCOM_TZMEM_MODE_SHMBRIDGE` 打开时才做事（该选项默认关，fuxi 参考内核配置里也是关的）。
所以一块普通的物理连续分配就是上游在这块板上实际用的东西。

**真机结果（2026-11-20，证据 `evidence/2026-11-20-modem-region-assign.md`）：
assign 被安全世界接受了，但拒绝没有变。** 日志：

```
FUXI_MODEM: the assign hands d4d00000 of 53477376 bytes from vmid 3 to vmid 15, the block is at 8d53f5000
FUXI_MODEM: the assign's answer is 0/0
FUXI_MODEM: the pas 38 step 0 answers 4291821499/0
FUXI_MODEM: the modem device tree blob was refused (scm 4291821499/0)
```

`scm 0/0` 就是**接受**：DSM 区（`0xd4d00000`/`0x3300000`）按上游 `sm8550_mpss_resource`
的要求，从 HLOS（vmid 3）交给了 modem 的 MSA（vmid 0xF），读写权限。
**所以 region assign 不是拒绝的理由**——分析文档第 1 项的假设被真机否掉了。
DTB 的 INIT_IMAGE 仍然回 `0xFFCFFFBB`，与 2026-11-19 完全一样。

**metadata 缓冲区的缓存属性**：上游用 `dma_alloc_coherent` 分配它，
`qcom_scm_pas_init_image` 的注释写明它必须"physically contiguous, 4K aligned and
**non-cachable** to avoid XPU violations"。本仓 2026-11-20 起改成**整页**分配
（`allocSecurePage()`：4096 字节、物理连续、页内其余清零、`cleanToPoc` 整页），
与上游的规矩同形。

**但"整页分配"不等于"映射成不可缓存"——这一点是 2026-11-20 真机测出来的。**
第一版照字面做了 `kIOMapInhibitCache`，**modem 的线程卡死在里面**：分配函数本身没返回，
连 `stepAssign()` 的第一行 `printf` 都没到。它变成 `MAP_MEM_IO`（`iomd_mem_types[]`），
`vmProtForCacheMode` 还要额外给 `VM_PROT_WRITE`（VM 改缓存属性需要写权限）。
上游自己的 `dma_alloc_coherent` 在 ARM64 上是**可缓存但一致**的，它不要求不可缓存的映射，
也不做任何清缓存——一致——那是 DMA API 的职责；本仓里与之等价的是调用者那次 `cleanToPoc`。
所以现在**整页 + 物理连续 + 页对齐 + 页内清零 + `cleanToPoc` 全部保留**，
但**不再强制不可缓存的映射**。改完这一步，线程就过了分配，assign 被安全世界接受（0/0）。

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

**没有待查的上游源码。** 2026-11-20 读完上游并写回下面的答案：region assign 的参数块结构、
七个参数的逐个含义、七参数怎么进 SMC64 的寄存器、DTB 那一步搬不搬分段、
PAS 三个步骤的参数类型字、fixed 镜像走哪条路、探针顺序与 DTB 的 INIT_IMAGE 属于哪个阶段。

**2026-11-20 第三次读上游：探针顺序与 DTB 的 INIT_IMAGE 属于哪个阶段（已核对，本仓一致）**

上游探针（`qcom_q6v5_pas.c` 的 `qcom_pas_probe`）的行号顺序：

| 行 | 上游做什么 | 本仓对应 |
| --- | --- | --- |
| 976 | `qcom_pas_alloc_memory_region()`：解析设备树的保留内存（DTB 的洞是**第 2 块**，modem 的洞是第 1 块） | `kModemDtbCarveoutBase` / `kModemCarveoutBase`（与厂商 DTS 一致） |
| 980 | `qcom_pas_assign_memory_region()`：**在任何 PAS 调用之前**把 DSM 区从 HLOS 交给 modem 的 MSA | `stepAssign()`（本仓也在 PAS 之前） |
| 984 | `qcom_pas_init_clock()` | （无 SCM 时钟，见"上游还有两步本仓没做"那张表） |
| 988 | `qcom_pas_init_regulator()` | — |
| 992 | `qcom_pas_pds_attach()`：拿 `cx` 与 `mss` 两个 proxy pd | `stepRails()` |
| 1022 | `pas->pas_ctx->use_tzmem = desc->needs_tzmem \|\| rproc->has_iommu` | — |
| 1033 | `rproc_add(rproc)` | — |

**DTB 的 INIT_IMAGE 属于启动阶段，不属于探针阶段。** 上游 `qcom_pas_load()`（`.load` 回调，
`qcom_q6v5_pas.c:259`）是由 remoteproc 核心在 **`rproc_start()` 里**调的
（`remoteproc_core.c:1274` → `rproc_load_segments()` → `rproc->ops->load()`，
`remoteproc_internal.h:222-228`），而 `rproc_start()` 是启动阶段。
所以上游的 DTB 的 INIT_IMAGE 与搬分段都发生在 **`qcom_pas_start()` 里、在 rails 投票之后**，
与本仓 `stepRails()` → `stepPas()` 的顺序一致。
**本仓的 PAS 顺序与上游一致。**

上游 `qcom_pas_load()` 里另外两件事，本仓不需要做：
`qcom_pas_shutdown(lite_pas_id)` 与 `qcom_pas_shutdown(lite_dtb_pas_id)`——
`sm8550_mpss_resource` **没有**这两个字段（只有 `pas_id = 4` 与 `dtb_pas_id = 0x26`），
所以上游自己也不发这两个关闭调用。

**2026-11-20 第二次读上游：PAS 三个步骤的参数类型字与 fixed 镜像的路径（已核对，本仓一致）**

上游三个描述符的 `.arginfo`（`drivers/firmware/qcom/qcom_scm.c`）：

| 步骤 | 上游的 `.arginfo` | 算出来的值 | 本仓 `fuxi_modem_pas_arginfo()` | 一致 |
| --- | --- | --- | --- | --- |
| INIT_IMAGE | `QCOM_SCM_ARGS(2, QCOM_SCM_VAL, QCOM_SCM_RW)` | `2 \| VAL<<4 \| RW<<6` = **0x82** | `qcom_scm_args(2, QCOM_SCM_VAL, QCOM_SCM_RW, 0)` | 是 |
| MEM_SETUP | `QCOM_SCM_ARGS(3)` | **0x03** | `qcom_scm_args(3, 0, 0, 0)` | 是 |
| AUTH_AND_RESET | `QCOM_SCM_ARGS(1)` | **0x01** | `qcom_scm_args(1, 0, 0, 0)` | 是 |

（`QCOM_SCM_ARGS` 的编码：`a<<4 \| b<<6 \| c<<8 \| ...`，计数在 bits 3:0。）

**fixed 镜像在上游走的是哪条路**（`drivers/soc/qcom/mdt_loader.c` `__qcom_mdt_pas_init` + `qcom_mdt_load_no_init`）：

- `relocate` 只在**可加载**的程序头上判断：`mdt_phdr_loadable()` 要求 `p_type == PT_LOAD`、
  不是 hash 类型、`p_memsz != 0`；`relocate = true` 当且仅当某个可加载段的 `p_flags` 带
  `QCOM_MDT_RELOCATABLE`（`BIT(27)`）。
- `relocate` 为真才调 `qcom_pas_mem_setup(pas_id, mem_phys, max_addr - min_addr)`。
  **fixed 镜像根本不调 MEM_SETUP**——与本仓 `fuxi_modem_pas_header_steps()` 的做法一致。
- 分段的落点（`qcom_mdt_load_no_init`）：
  - relocatable：`mem_reloc = min_addr`，落点 = `p_paddr - min_addr`；
  - **fixed**：`mem_reloc = mem_phys`（**洞的物理基址**），落点 = `p_paddr - mem_phys`。
  本仓 `qcom_mdt_place()` 的 fixed 分支是 `s->address - region_base`，与此一致。
- `min_addr` / `max_addr` 都用 **`p_paddr`**（不是 `p_vaddr`），`max_addr` 按 `ALIGN(p_paddr + p_memsz, SZ_4K)`
  取整。本仓 `QcomMdt.c` 读的也是 `p_paddr`（ELF32 的偏移 12、ELF64 的偏移 24），一致。
- `p_filesz > p_memsz` 的段上游直接拒绝（`-EINVAL`），本仓 `qcom_mdt_parse()` 也返回 `QcomMdtBadHeaders`。

**结论：fixed 与 relocatable 的差别在参数层不改变 INIT_IMAGE。** 两者发出去的 INIT_IMAGE
完全一样（`pas_id` + metadata 地址），只有 fixed 少了 MEM_SETUP 这一步、且分段落点用洞的基址。
所以"被接受的 ADSP 是 relocatable、被拒的 modem 是 fixed"这个差别，**不能解释 INIT_IMAGE 被拒**：
DTB 的 INIT_IMAGE 在 fixed 路径下发的两个参数与 relocatable 路径逐字相同。
**这条线索到此为止**，与"两个槽都被拒"那条线索合在一起：拒绝的原因在两个构建共有的东西上。


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
固件分区的 `/image` 目录由内核的 `fuxi-modem=1` 只读清点列出（`fuxi-modem=12` 的槽清点证明两个槽都存固件，
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

**拒绝的原因已经定位在镜像内容上，不在调用上**（2026-11-19 第二次真机，PAS 矩阵，
`fuxi-modem=5`，证据 `evidence/2026-11-19-modem-pas-matrix.md`）：

| 组合 | INIT_IMAGE |
| --- | --- |
| `adsp_dtb` + PAS 36（ADSP 的那一对） | **0/0 接受** |
| `adsp_dtb` + PAS 38 | 0xFFCFFFBB 拒绝 |
| `modem_dtb` + PAS 38（modem 的那一对） | 0xFFCFFFBB 拒绝 |
| `modem_dtb` + PAS 36 | 0xFFCFFFBB 拒绝 |

四组用的是**同一条调用**：同一个命令、同一个参数描述符、同一个 4044 字节的 metadata 缓冲区、
相邻的物理地址（0x8d0b1e000 与 0x8d0b1f000）。其中一组被接受，所以调用形状、命令常量、参数顺序、
缓冲区大小与对齐、metadata 的拼装方式全都是对的。拒绝只跟着镜像走。

**仪器先被验证过，才用它下的结论**（`fuxi-modem=6`）：

| 查询 | 结果 |
| --- | --- |
| 支持查询，PAS 36 | 0/0 |
| 支持查询，PAS 1 | 0/0 |
| 支持查询，PAS 0xffff | 0xFFCFFFC0 |
| 支持查询，PAS 0x1234 | 0xFFCFFFC0 |
| INIT_IMAGE，modem 的 blob，PAS 0xffff | 0xFFCFFFE5 |
| INIT_IMAGE，modem 的 blob，PAS 38 | 0xFFCFFFBB |

支持查询确实在回答"这个 id 认不认识"，不是有问必答。secure world 至少给三种不同的错误码：
不知道的 id（0xFFCFFFE5）、认识但不接受的 id（0xFFCFFFC0）、拒绝的镜像（0xFFCFFFBB）。

**扫遍全部被接受的 id**（`fuxi-modem=9`）：secure world 接受 21 个 id
（1 4 9 12 13 14 15 18 19 20 21 23 26 27 28 33 34 35 36 37 38），
**每一个都拒绝 modem 的 blob**，错误码都是同一个 0xFFCFFFBB。所以没有任何 id 能收这个 blob，
问题一定在 blob 本身。

**两条还没排除的路径都试过了**：

| 试的东西 | 结果 |
| --- | --- |
| 把 blob 的分段先搬进它自己的洞（0x9b000000）再 INIT_IMAGE（`fuxi-modem=7`） | 仍然 0xFFCFFFBB |
| 把 metadata 换成紧凑形式（头后面直接跟 hash 段，`fuxi-modem=11`） | 仍然 0xFFCFFFBB。**"紧凑形式与分片形式逐字节相同"这条已撤回**：那次比较写成了两个分支一样的 `memcmp(...) ? "the same" : "the same"`，两个缓冲区又都来自同一次读，所以它不可能失败，什么也没证明。模式 11 现在改为统计差异字节数，且紧凑形式改从 `.mdt` 文件读 |

**两个 blob 本身都是完好的**：modem 的 512 字节段声明 452+60=512，adsp 的 8376 字节段声明
8316+60=8376，都自洽；头都是 `43 46 47 4c`（CFGL）。modem 的 blob 是 **fixed**（段地址 0x9b000000），
adsp 的是 **relocatable**。hash 段两者都是 3896 字节，其中 193 字节不同——这是不同的固件，
本来就该不同。

**结论**：modem 的 DTB blob 在这台机器上不被 secure world 接受，而 adsp 的 blob 被接受。
modem 的**主镜像**（149 MB，32 段，PAS 4）同样被拒，错误码同一个 0xFFCFFFBB。
**这不是本仓代码的问题**——本仓的 PAS 调用形状已经被 ADSP 那组（adsp_dtb + 36）证明是对的。

**槽位清点**（2026-11-19 第三次真机，`fuxi-modem=12`，
证据 `evidence/2026-11-19-modem-slot-survey.md`）：

| 槽 | LUN | 首块 | 块数 | `/image` 条目 | `modem.mdt` | `modem_dtb.mdt` |
| --- | --- | --- | --- | --- | --- | --- |
| `modem_a` | 4 | 5126 | 114688 | 443 | 6364 | 4044 |
| `modem_b` | 4 | 283283 | 114688 | 443 | 6364 | 4044 |

两个槽是**同一 LUN 上两个不同的分区**（GPT 名字匹配是全等的），**两个槽都有基带固件**，
而且**两个槽的固件不一样**：两个 `modem_dtb` blob 都是 7992 字节，逐字节比较有 **95 字节不同**，
第一处在偏移 4533——落在 hash 段里（头 148 字节，hash 段 4096 到 7992）。所以两个槽是**两个不同的构建**。

**同一件事在 `.mdt` 文件上也量到了（2026-11-19，模式 12）：** 两个槽的 `modem_dtb.mdt` 都是
4044 字节，逐字节比较也是 **95 字节不同**，第一处在偏移 **585**（`.mdt` 的布局是 148 字节头
接 3896 字节 hash，585 落在 hash 段里）。同一批差异在两种文件布局上各出现一次，
这是"两个槽是两个不同构建"的字节级证据，而且是在 metadata 真正被取用的那个文件上。

**这条线索变了**：之前记录写"固件来自 `modem_a`"是**推断**——`mountPartition()` 先试 `modem_b`，
清点读到的其实是 `modem_b`。清点也证明**槽不是拒绝的原因**：被测的那个槽（`modem_b`）确实有固件。
两个槽是两个不同的构建，其中**一个可能就是 secure world 接受的那一个**。

**两个槽各自的 blob 分别递上去**（`fuxi-modem=13`，同一份证据）：

| 槽 | blob | metadata | PAS 38 |
| --- | --- | --- | --- |
| `modem_a` | 7992 字节，3 段，fixed，hash 2 | 4044 字节 @ 0x8d0b3f000 | 0xFFCFFFBB 拒绝 |
| `modem_b` | 7992 字节，3 段，fixed，hash 2 | 4044 字节 @ 0x8d0b60000 | 0xFFCFFFBB 拒绝 |

**两个都被拒，错误码相同**。槽这条线索到此为止：**没有一个构建是被接受的**，
所以拒绝的原因在**两个构建共有的东西**上。

两个构建共有的、而 secure world 要看的，是 metadata 的结构、程序头、段的地址
（0x9b000000，就是板上自己的 `q6_mpss_dtb_region`），以及**这个镜像是 fixed 而不是 relocatable**。
被接受的 adsp 那个 blob 是 **relocatable**。

**2026-11-20 查完了：fixed 与 relocatable 的差别不能解释 INIT_IMAGE 被拒。**
上游（`drivers/soc/qcom/mdt_loader.c`）的 `relocate` 只决定两件事：
fixed 少发一次 `MEM_SETUP`，以及分段的落点用洞的物理基址而不是最小地址。
**INIT_IMAGE 发出去的参数逐字相同**（`pas_id` 与 metadata 的地址），
所以两种镜像在 INIT_IMAGE 这一步没有任何差别。
本仓 `package/FuxiModemPas.h` 与 `package/QcomMdt.c` 已经与上游一致（详见"待查的上游源码"下面
2026-11-20 第二次读上游的那张表）。这条线索到此为止。

**所以拒绝的原因只能在下面这些共有事实上**：metadata 的**字节**、它的程序头、
段的 `p_paddr`（0x9b000000）、以及 PAS id 38 本身。

**2026-11-20 找到最重的一条线索：主线 Linux 在同一块板上、用同一份固件，把基带跑起来了。**
主线端口的 `/tmp/x13/docs/交接-新会话-20261030-2110.md` 记着：modem12 首次让基带稳定运行
4584 s 并注册上 CHN-UNICOM 460-01。它用的固件就是本板 `modem_a`/`modem_b` 分区里的
`modem.mdt` 与 `modem_dtb.mdt`（主线 DTS：`firmware-name = "qcom/sm8550/xiaomi/fuxi/modem.mdt",
"qcom/sm8550/xiaomi/fuxi/modem_dtb.mdt"`），设备树用的也是同一套 `sm8550.dtsi` 的
`remoteproc_mpss`。**所以这份固件不是被 secure world 拒绝的**：同一块板、同一份 blob、
同一个 pas id 38，主线把它启动了。

主线那边真正卡住的是 **metadata 的地址宽度**：主线 `qcom_scm_pas_init_image` 用
`dma_alloc_coherent` 分配 4044 字节，而 SCM 平台设备的 mask 默认 32 位
（`dma_mask = DMA_BIT_MASK(32)`），主板上 4 GiB 以下没有可用的 coherent 区，
`dma_alloc_coherent` 返回 NULL，modem_dtb 的 PAS init 报 **-12**。
主线的修法是 **`CONFIG_ZONE_DMA=y` + `CONFIG_ZONE_DMA32=y`**（`abl-fuxi-modem12-imem.config`
相对基线只多了这两项），打开之后 3 次崩溃恢复全部成功。
**打开 DMA32 的作用只有一个：让 metadata 落到 4 GiB 以下。**

本仓的 metadata 落在 **0x8d39ab000**（2026-11-20 真机实测），**在 4 GiB 以上**。
本仓的 mask 是 `0xfffffffffffff000`，不限制地址宽度，所以分配成功、地址却在 4 GiB 以上。
**这条线索与主线那边修好的那个 bug 是同一个形状**，而且它是目前唯一一条能同时解释
"ADSP 的 blob 接受、modem 的 blob 拒绝"的线索吗？——不是：ADSP 的 blob 也是从 4 GiB 以上的
地址交的（0x8d0b1f000），却被接受。所以**地址宽度本身不足以解释 ADSP 与 modem 的差别**，
但它仍然可能是 modem 被拒的**原因之一**：主线那条路上 metadata 从来没有在 4 GiB 以上被交过。

下一步（本轮要做的）：把 metadata 的分配限制在 4 GiB 以下再试一次。
XNU 的机制是 `IOBufferMemoryDescriptor::inTaskWithPhysicalMask` 的 mask 参数
（`IOBufferMemoryDescriptor.h` 的文档："physical addresses will only have bits set present
in physicalMask ... pass 0x00000000FFFFFFFFULL for a buffer to be accessed by hardware that has
32 address bits"），本仓 `package/FuxiAdspProbe.cpp` 的 `probeOwnedLowPcm()` 已经有这个用法
（mask `0x00000000fffff000`），但那条路在真机上测出来的 PA 是 **0x8CFF84000（35.25 GiB）**，
**掩码没有生效**（`docs/fuxi-audio-porting-log.md` §P3）。所以掩码这条路要先弄明白为什么没生效，
或者改走固定低 DRAM 映射（音频链已经验证过 `IODeviceMemory::withRange(0xd0000000, 64 KiB)
-> map(kIOMapAnywhere | kIOMapWriteCombineCache)` 这条配方）。

