# fuxi 基带实施方案（困难项与阶段目标）

状态：设计输入已齐；传输层（QRTR + GLINK 适配）已实现并链接进内核，modem 的 PAS 启动尚未做
依据：`docs/baseband-porting-plan.md`（通用分层与安全规则）、`/tmp/x13/work/device-info/fuxi-live.dts`（CAF 运行中 DTB，37744 行）、`uefi-fuxi` 的 `sm8550-xiaomi-fuxi.dtb`、UniXNU 上游 `components.toml` 与 `docs/porting/baseband.md`、`UniXNU/QcomBaseband` @ `dd0199c`、`chiba233/Trogdor-ipadOS`
目标系统：UniXNU + iPadOS 17.7.11（21H461）
目标硬件：Xiaomi 13（fuxi，SM8550，Qualcomm MPSS over GLINK/QRTR + QMI）

本文件回答两个问题：哪些地方一定会卡住，以及每阶段做到什么算完成。通用分层与安全红线仍以 `baseband-porting-plan.md` 为准，本文不重复。

## 1. 先看结论

fuxi 的基带不是 Apple PCIe 基带，是 SoC 内集成的 Qualcomm MPSS。`UniXNU/QcomBaseband` 的协议方向（QMUX↔QRTR、QMI、SAHARA、Apple 类名门面）对得上，但它的硬件代际是 SC7180 / IPA v4.2，fuxi 是 SM8550 / IPA v5.5，所以**组件可以复用，板级适配层必须重写**。

**组件里有三块是 Apple Mav16 专用，fuxi 不需要**：SAHARA 设备端（`coachz_bb_sahara.h` 是为了让 Apple 的 BBUpdater 走一遍 Mav16 启动而做的假端点——它按 bootloader 的样子逐个要镜像、把字节丢掉、只回报「传输完成」）、Mav16 的 `CZBBFW01` 固件归档解包与挂载、以及 `baseband-chipset = mav16` 这套身份。fuxi 的基带由 PAS 启动自己的固件，不经过 Apple 的 baseband firmware 路径。这三块在 S2 就要从「引入组件」里隔出去，否则会带着一批对不上 fuxi 的假设进代码。

`unixnu-fuxi` 已经有大部分低层基座：SMEM（按 item id）、SMP2P、通用 GLINK-SMEM、MDT 解析与 PAS metadata、FAT 只读分区读固件（已能读 `modem_a`/`modem_b`）、以及 FuxiAdspProbe 里已跑通的 PAS 启动时序。缺的正是四块：**SCM/PAS 调用胶水（可从 ADSP 那份抄）、QRTR 本体、IPCC 驱动、IPA v5.5 数据面**。

最终目标「正常读取卡 1 并收集卡 1 的信号」落在 BB1 内侧，不依赖 CommCenter 集成，可以先用独立工具闭环。

**这个目标在主线 Linux 上已经达成过一次，同一台 fuxi**（`/tmp/x13/docs/目标-M5.md` 的实测记录）：UIM 读到两个卡槽都 ACTIVE，卡 1 注册到联通 46001 5G NR、PS attached、信号 50；LTE 时 RSRP -82 dBm / SNR 21.8 dB。所以硬件与基带侧没有未解的问题——**XNU 侧的工作是把 Linux 的用户态服务栈换成 XNU 内核里的 QMUX/QMI 桥接**，这也是 S8 先于 S9 的理由。

## 2. 困难项清单

按「一定会卡」的程度排序。每项写清事实、影响和绕法。

### D1 UniXNU 上游合并（所有工作的前置）

事实：`UniXNU` 本地分支相对 `origin/main` **落后 611、领先 161**，合并基点是 `dc8e5deb`。`git merge-tree` 给出 **98 处冲突**，按路径分：

| 类别 | 数量 | 路径 |
| --- | --- | --- |
| 文档 | 73 | `docs/`、`docs/zh/`、`docs/.vitepress/`（其中 `docs/.vitepress/device-records/elden.toml` 是 modify/delete） |
| `scripts/unixnu/` | 6 | `bootimg.py`、`cli.py`、`ipsw.py`、`kernel.py`、`payload.py`、`runtime.py`（后者是 add/add） |
| 其他脚本与数据 | 11 | `scripts/build-adreno-metal-bridge.py`、`build-adreno-metal-plugin.py`、`build-boot-runtime.py`、`build-fileset-release-reference.py`、`seed-data-grow.py`、`test-runtime-inputs.py`、`fetch-adreno-inputs.py`、`patch-xnu-entry-probe.py`、`prepare-xnu-source.py`、`scripts/data/adreno-compiler-inputs.json`、`scripts/data/gpu-metal-vulkan-inputs.json` |
| 仓库根 | 5 | `.gitmodules`、`README.md`、`README.zh-CN.md`、`components.toml`、`devices.toml` |
| submodule 指针 | 3 | `drivers/Adreno740`、`drivers/AdrenoMetal`、`drivers/DarwinBoot` |

`components.toml` 的冲突要手工合。本地 26 个组件、上游 28 个，两边不是包含关系：上游独有 `QcomBaseband`、`QcaBluetooth`、`QcomDwc3`，本地独有 `FuxiFtsTouch`。合并结果要是 29 个的并集。

**光并集不够**：25 个共有组件里有 **9 个 revision 分叉**（`ArmCpu`、`Adreno740`、`AdrenoMetal`、`ShengUfs`、`ShengNovatekTouch`、`AppleKeyStoreState`、`RamBlock`、`XnuPrelink`、`Ath12kWifi`），上游普遍推前了。其中**只有 `Adreno740` 一条带本地注释的 overlay**（「fuxi bring-up overlay on upstream 6fa7bae2a5b3b523e790eb3e0348af4d98969e4d」，保住双 dword 的 `CP_SMMU_TABLE_UPDATE`、进程表回读与挂载策略），这一条必须保本地 revision，否则 overlay 丢掉。其余 8 条要看上游那侧的 revision 有没有带 fuxi 需要的改动，逐条比。**`Ath12kWifi` 两边都有但 revision 分叉**，Wi-Fi 这条线要单独比。

影响：`scripts/unixnu/` 是本 fork 自研的 bootimg/kernel/payload/runtime 链，上游同期重写了同样这几个文件。合并后 `build-qcom-baseband.py` 依赖的 XNU compile record 路径可能变。`components.toml` 冲突若解错，会把本地板级条目或上游新组件丢掉。

绕法：先单独做一次「只合上游、不加基带」的合并并跑通 `./unixnu build kernel`，把合并成本与基带成本分开计。基带组件在合并之后单独引入。

### D2 IPA v5.5 与 QcomBaseband 的 v4.2 表不匹配

事实：`QcomBaseband` 的 `ipa/coachz_ipa_v42.h` 由 `ipa/gen_ipa_data.py` 从 Linux commit `6628e7d` 的 `ipa_reg-v4.2.c` / `gsi_reg-v4.0.c` / `ipa_data-v4.2.c` 生成。fuxi 的运行中 DTB 写的是 `qcom,ipa-hw-ver = <0x18>`，主线对应 `drivers/net/ipa/data/ipa_data-v5.5.c`。

主线在 fuxi 上踩过并已修的点（`/tmp/x13/kernel/patches/0006-ipa.patch`）：

| 项 | 主线原值 | fuxi 实测值 | 后果 |
| --- | --- | --- | --- |
| `IPA_ENDPOINT_AP_MODEM_RX` | endpoint 23 | 24 | 端点不匹配 |
| `IPA_ENDPOINT_MODEM_AP_RX` | endpoint 21 | 22 | 端点不匹配 |
| `modem-tables` | 0x14688000/0x2000（sm8550.dtsi 的 `modem-tables@8000`） | **0x146a8000/0x2000**（厂商 `ipa_smmu_ap` 的 `qcom,additional-mapping`） | SMMU context fault（SID 0x4a0）→ 基带 `IPA_IRQ_STTS 0x1` BAD_SNOC_ACCESS |
| `INIT_DRIVER` | 无可选 TLV | 需 TLV `0x23` hw_filter_stats_info；`0x24` smem_info 与 `0x25` per_stats_smem_info 在 fuxi 上**不填**（见下） | 基带不回包，`error -110` |
| SMEM item 497 | AP 预分配 | **归基带所有**，AP 预分配会被基带 assert | 需整段 SMEM 一对一映射，不调 `qcom_smem_alloc` |
| GSI channel halt | 50 ms 超时 | 需 5000 ms，且要在 SSR shutdown 之后 | 基带重启后 channel 0 起不来 |

六条各自的落点：

- 端点号在 `ipa_data-v5.5.c` 的 `ipa_gsi_endpoint_data[]`，是 v5.5 表自己的数据，不是代码里写死的常量——所以换成 v5.5 的表就已经对了。
- `modem-tables` 是 **DTS 侧**的 `sram` 引用：主线 `sm8550.dtsi` 指向 `modem-tables@8000`（0x14688000），fuxi 要新建 `modem-tables@28000` 指向 0x146a8000（`/tmp/x13/kernel/patches/0013-board.patch:1654`）。
- `INIT_DRIVER` 的三个 TLV 有条件（`init_modem_driver_req()`，主线补丁 `0006-ipa.patch` 的加法段）：外层 `if (ipa->version >= IPA_VERSION_5_0)` 里，先取 `ipa_mem_find(ipa, IPA_MEM_STATS_FILTER_ROUTE)`，非空则填 `hw_filter_stats_info`（start = mem->offset，size = `IPA_QMI_MODEM_FNR_COUNT * 16`，start_index = `IPA_QMI_FNR_START`，end_index = start + count - 1）；再 `if (ipa->smem_size && !ipa->smem_modem_owned)` 才填 `smem_info`（size = smem_size）与 `per_stats_smem_info`（start = smem_iova，size = `IPA_QMI_PER_STATS_SZ` = 2 KB）。**fuxi 走的是整段 SMEM 映射那条路**（`ipa_smem_init()` 里 `if (of_machine_is_compatible("xiaomi,fuxi")) return ipa_smem_map_region(ipa);` 提前返回，该函数末尾置 `smem_modem_owned = true`），所以 `smem_size` 非零但 `smem_modem_owned` 为真 → **`0x24` 与 `0x25` 这两个 TLV 不会填，只有 `0x23` 会填**。写代码时按这个条件走，不要三个 TLV 无条件带。
- SMEM 整段映射用 `of_find_compatible_node(NULL, NULL, "qcom,smem")` 取节点、`of_address_to_resource()` 取 `res.start`/`resource_size`，再 `iommu_map(domain, phys, phys, size, IOMMU_READ|IOMMU_WRITE)` 一对一映射，置 `smem_modem_owned = true`。fuxi 的 SMEM region 是 `0x81d00000/0x200000`。
- GSI 的 `GSI_GENERIC_CMD_TIMEOUT 5000` 是新增的独立常量，`GSI_CMD_TIMEOUT 50` 保留给普通命令；`gsi_modem_channel_halt()` 从 static 改成导出，并在 `ipa_endpoint_modem_halt_all()` 里对每个 modem 端点调用，且该函数在 modem offline 之后才跑。

影响：数据面不能直接拿 `coachz_ipa_v42.h` 用。寄存器偏移、字段掩码、端点表、资源组全部错位。

绕法：`gen_ipa_data.py` 的版本常量、文件名、`ipa_data` 符号名是写死的 v4.2，需要改成参数化（`ipa_reg-v5.5.c` / `gsi_reg-v5.0.c` / `ipa_data-v5.5.c`）后重新生成头文件——所有数字仍由编译出的表得出，不手抄。上面六条主线路由补丁要逐条移进 XNU 侧的实现；其中 `modem-tables` 是 DTS 侧的事，不在生成表里。

### D3 板级适配层是重写，不是移植

事实：`platform/coachz/coachz_modem.c` 把 SC7180 的 DMA 物理地址写死，并假设 apps SMMU 全 bypass：

```
CM_RX_PA 0x81400000   CM_RING_PA 0x81700000   CM_CMD 0x81710000
CM_MODEM_RX_PA 0x82000000   CM_MODEM_TX_PA 0x82200000
```

fuxi 需要的是自己的绑定（`fuxi-live.dts` 已给出）：

| 资源 | fuxi 值 | 来源 |
| --- | --- | --- |
| IPA MMIO | `0x3e00000/0x84000` + GSI `0x3e04000/0xfc000` | `qcom,ipa@3e00000` |
| IPA IRQ | GIC SPI `0x28e`(ipa) / `0x1b0`(gsi) | 同上 |
| IPA core clock | `clocks = <0x3b 0x0c>` | 同上 |
| IPA GSI carveout | `0x9b090000/0xa000` (40 KB) | `ipa_gsi_region` |
| IPA fw carveout | `0x9b080000/0x10000` (64 KB) | `ipa_fw_region` |
| IPA cfg offset | `0x140000`（2 MB block 内） | `qcom,ipa-cfg-offset` |
| SMMU CB | AP `0x4a0`、wlan `0x4a1`、uc `0x4a2`、11ad `0x4a4`（`0x4a3` 库存未分配） | `ipa_smmu_*` |
| MPSS carveout | `0x8a800000/0x10800000` (264 MB) | `mpss_region` |
| MPSS DTB carveout | `0x9b000000/0x80000` (512 KB) | `q6_mpss_dtb_region` |
| MPSS DSM | `0xd4d00000/0x3300000` (51 MB) | `mpss_dsm_region` |
| SMEM region | `0x81d00000/0x200000` | `smem_region` |
| IPA fw PAS id | `0x0f` | `qcom,ipa_fws` 的 `qcom,pas-id` |
| modem remoteproc | `0x4080000/0x10000`，`qcom,kalama-modem-pas` | `remoteproc-mss@04080000` |
| modem SMP2P | SMEM `0x1b3`/`0x1ac`，local pid 0，remote pid 1 | `qcom,smp2p-modem` |
| modem GLINK | remote pid 1，transport smem，channel `IPCRTR`（intent `0x800 0x05 0x2000 0x03 0x4400 0x02`）+ `DS`（intent `0x4000 0x02`） | `glink-edge` |
| AOP QMP | `power-controller@c300000`，`qcom,kalama-aoss-qmp` | `qcom,qmp = <0x43>` |

另外两点结构性差异：

- coachz 把所有轮询挂在 **Wi-Fi 线程**的毫秒循环上，因为 coachz 的 WCN3990 与 modem 共用；fuxi 的 Wi-Fi 是独立的（WCN7850，走 PCIe），所以 fuxi 需要**独立的 modem owner 线程**，这反而更干净，但要自己处理线程与生命周期。
- coachz 的 6148 行 Wi-Fi 补丁里纠缠着 MPSS/GLINK/QRTR/rmtfs 的一半，fuxi 不需要背这个包袱。

绕法：新建 `platform/fuxi/` 适配层，接口沿用 `QcomBaseband.h` 的六个入口（`coachz_baseband_configure` / `new_server` / `del_server` / `qrtr` / `poll` / `report` 与 `coachz_modem_configure` / `data_send` / `data_receiver`），实现另起。`coachz` 前缀在 fuxi 上是误导，合并进上游组件时应一并改名（见 D9）。

`coachz_bb_control.h` 的做法可以直接沿用到 fuxi。Apple `AppleBasebandN71` 的 user client 方法是一组 selector（`CBC_RESET_MODEM 0`、`CBC_POWER 1`、`CBC_MUX 2`、`CBC_GET_RESET 3`、`CBC_POWERCYCLE 6`、`CBC_VBUS 8`、`CBC_GET_PMU_EXTON 11`、`CBC_SELF_IDENTIFY 15`、`CBC_GET_DIAG_READY 16`、`CBC_GET_EFS_ERASE_DONE 17`、`CBC_GET_RESET_REQUEST 18`、`CBC_COREDUMP_GPIO 19`、`CBC_TIMESYNC 20`、`CBC_SET_MANAGER_STATE 22`、`CBC_GET_MANAGER_STATE 23`、`CBC_CHARGER_DESENSE 25`、`CBC_GET_BASEBAND_STATE 26`、`CBC_SET_BASEBAND_STATE 27`、`CBC_GET_PROPERTY 28`、`CBC_SET_PROPERTY 29`、`CBC_CLEAR_FREQ_LIST 32`、`CBC_REMOVE_FREQ_LIST 34`、`CBC_RESET_SPMI 35`、`CBC_SPMI_RAW_WRITE 36`、`CBC_SPMI_RAW_READ 37`、`CBC_SPMI_GET_EVENTS 41`、`CBC_GET_COREDUMP_READY 42`、`CBC_SPMI_REG0_WRITE 45`）。

fuxi 上的处理：**不碰硬件**——电源、复位、mux、VBUS 请求照收并记进 `s->resets` / `s->powered` / `s->mux` / `s->vbus`（fuxi 的基带在 SoC 内、由 PAS 启动，没有 AP 侧可操作的复位线）；`CBC_GET_RESET` 恒回 0（不在复位中）；manager/baseband 状态（22/23、26/27）是 ABM 自己写回读的 `BootController` 值，原样存回；`CBC_SELF_IDENTIFY` 回成功但不据此判身份；`CBC_GET_PROPERTY`/`SET_PROPERTY` 是 16 组 key/value 的表；未实现的 selector 回 `CBC_UNSUPPORTED`（`0xe00002c7`）。这套契约审计在 S9 做，代码形态在 S2/S3 就要能用。

### D4 传输层缺件：QRTR、IPCC、rmtfs

事实：`unixnu-fuxi/package/` 里没有 QRTR、没有 IPCC 驱动、没有 rmtfs。`QcomScm.h` 只有编码器（`qcom_scm_fn` / `qcom_scm_args` / `qcom_ipcc_id`），注释明说「SMC 本身在 kernel glue 里，这些只是 host test 检查的编码器」。

`QcomGlink` 是通用 GLINK-SMEM 传输（按名字开通道，`QCOM_GLINK_MAX_CHANNELS 6`），目前只开了 `adsp_apps`。基带要开的是 `IPCRTR`（QRTR 承载）和 `DS`。

主线在 fuxi 上踩过的两个 QRTR/传输坑：

- **QRTR HELLO 竞态**（`/tmp/x13/kernel/patches/0005-qrtr.patch`）：主线在 `hello_sent` 之前收到对端 HELLO 的回复会直接丢包。基带正是靠 HELLO 之后的 NEW_SERVER 公告自己的服务，丢了就一个 QMI 服务都看不到。必须移植这个 flush_delayed_work 修法。
- **挂起时 MPSS 是唤醒源**（`TASKS.md`）：恢复后第一个到达的是 MPSS IPCRTR 92 字节数据。XNU 侧的挂起/唤醒设计必须把基带算进去。

rmtfs 方面：上游组件的 README 与 coachz README 都把 rmtfs 列为板级要提供的东西（基带要通过 QMI 找用户态取文件，路径如 `/readonly/firmware/image`、`/readonly/firmware/modem_pr`、`/readonly/vendor/firmware_mnt/image`）。**fuxi 上这条不需要实现**：固件不落文件系统，`package/FuxiAdspFirmware.c` 已经支持 `"modem:NAME"` 形式从 `modem_a`/`modem_b` 的 FAT 分区 `/image` 里只读 `NAME.b00`…（FAT 分区就地读，不安装、不写、镜像不变大），ADSP 已经用它取 `modem:adsp` 与 `modem:adsp_dtb`。

**但有一条要查**：rmtfs 除了取固件，还用于基带运行时读取 NV/校准数据（modem_pr 等）。fuxi 的 NV 在 `fsg`/`fsc` 分区，基带自己会通过 QMI/文件系统去读，AP 侧要不要提供一条通路取决于基带固件的请求方式。**这一条在 S1 查清楚**：如果基带发起的是 QMI 文件读取请求（要 rmtfs 或等价的服务端），那 S6 就要补一个服务端；如果基带自己读 UFS，那完全不需要。标成待验证。

主线那边 rmtfs 是**用户态服务**（Ubuntu 的 `rmtfs` 包），而且只用文件模式 `-s`——EFS 副本写在 `/var/lib/fuxi-rmtfs`，**不写真分区**（主线记录明确说 Ubuntu 默认的 `-r -P -s` 会写真分区，drop-in 必须先在位）。这条正好对上本仓库的红线「不写 `fsg`/`fsc`」。fuxi 若要 rmtfs，等价物必须是「只读 + 内存副本」语义。

主线还有一个已知限制要记住：**同一次开机内 stop/start rmtfs 无法重试基带**（rproc 的 power 引用不归零），每次试验都得整机重启。XNU 侧的 SSR 路径（S5/S6）不能假设「重启 rmtfs 就能让基带重来」。

影响：QRTR 是 QMI 的全部前提，工作量集中在 QRTR node/port 状态机、HELLO/NEW_SERVER/DEL_SERVER、名字服务与 lookup。

补充：`coachz_baseband.cpp` 本身不含传输层，它假定「某个线程拥有 QRTR」，把基带消息交给 `coachz_baseband_qrtr()`、把 `coachz_baseband_poll()` 的结果发出去。coachz 里这个线程是 Wi-Fi 线程，fuxi 上要换成独立的 modem owner 线程（见 D3）。桥接层的锁模型现成：用户线程只入队，异步读用 `thread_call` 完成，用户内存拷贝在锁外做。

#### D4 的当前实现状态（2026-11-18）

QRTR 协议层与 fuxi 板级适配层已写好并接进内核，但**没有在真机上跑过一次**：

- 协议层在 `UniXNU/drivers/QcomBaseband/platform/fuxi/qcom_qrtr.{h,c}`，是**可移植的纯 C**（只用定宽整数），所以能在 host 上跑模型测试。线格式照 Linux `net/qrtr/af_qrtr.c` 与 `include/uapi/linux/qrtr.h`：32 字节小端头（version / type / src_node / src_port / confirm_rx / size / dst_node / dst_port），version 1，载荷按 4 字节补齐，**头和载荷在同一个连续 buffer 里**（Linux 是 `skb_push` 把头放到已有载荷前面）。
- 适配层在 `platform/fuxi/fuxi_modem.c`：复用设备仓库的 `package/QcomGlink.*` 与 `package/QcomSmem.*`，只自己管「怎么找到 modem 的 link」「怎么把一个 QRTR 包送过去」「收到了交给谁」。GLINK item 与 Linux 一致：描述符 478（32 字节，四个词依次是 tx tail / tx head / rx tail / rx head）、FIFO_0 479、FIFO_1 480；分区是 remote pid 1（modem），ADSP 那条边是 pid 2、另一个分区。通道名 `IPCRTR`，intent 4 KiB × 8。
- **HELLO 竞态按主线的做法处理**：HELLO 之前发出去的 DATA 会被丢弃并计入 `hello_dropped`，因为 modem 的名字服务会忽略它。这是主线 `0005-qrtr.patch` 修的那个坑的等价实现。
- host 模型测试 `tests/fuxi_qrtr_model_test.c` 覆盖头布局、单 buffer 分帧与补齐、HELLO 门控、服务器表（含 DEL_SERVER 之后留洞的查找）、畸形包计数。**它抓到一个真 bug**：`qrtr_lookup()` 与 `server_find()` 原来只遍历前 `servers` 个条目，被删的服务在数组中间留洞后就查不到了。
- 接入方式：`unixnu-fuxi/board.toml` 的 `[kernel] archives` 加 `component:QcomBaseband`，`configs/fuxi_defconfig` 的 `CONFIG_XNU_EXTRA_LD_ARCHIVES` 加 `-Wl,-force_load,{tree}/build/qcom-baseband/libQcomBaseband.a`。链接后的 `kernel.macho` 里能看到 `_fuxi_modem_start` / `_fuxi_modem_poll` / `_fuxi_modem_report` 与 `_qrtr_*`。
- **IPA 数据面（`coachz_modem_data_send` / `coachz_modem_data_receiver`）在 fuxi 上是空实现**，发送返回 `ENETDOWN`。它不在最终目标的路径上：卡状态与信号走 QRTR 控制面，不走 IPA raw-IP。

还差的（进 S4/S5）：modem 的 PAS 启动（`fuxi_modem_start()` 需要调用方先把 SMEM 窗口映射好并给出 doorbell 的 kick 回调）、IPCC doorbell、owner 线程把 `fuxi_modem_poll()` 跑起来。

### D5 MPSS 启动链：SCM 胶水缺失，但 ADSP 那份可抄

事实：`package/FuxiAdspProbe.cpp` 里已有一套完整的 PAS 时序：`secureCall()`（SMC64，x0=fn、x1=arginfo、x2=pas、x3..=参数，处理 `QCOM_SCM_INTERRUPTED` 与 `QCOM_SCM_V2_EBUSY`）→ `authenticateHeader()`（PAS_INIT_IMAGE + PAS_MEM_SETUP）→ `loadSegments()`（按 `qcom_mdt_place` 把段拷进 carveout）→ PAS_AUTH_AND_RESET → AOP 释放 DSP。这套在真机上跑通过（`docs/fuxi-audio-porting-log.md` 记录 ADSP 上电为 `stage=13`、`graph_phase=22`）。

但 ADSP 用的是 `pas_adsp = 1`、`pas_dtb = 36`（`facts/audio/binding.toml`，标 `unverified`，来源 sheng）。modem 需要自己的 PAS id 与 carveout：

- **modem DTB 段的 PAS id = 0x26（38）是推断，不是事实**。推断链有两条：主线内核日志里 `PAS init image 38`（`qcom_scm.c` 的 `__qcom_scm_pas_init_image` 打印 pas_id），以及 CAF 侧的十六进制惯例 `0x26 = 38`。但主线配方里**没有**任何地方明写这个值：`0013-board.patch` 的 `&remoteproc_mpss` 段只给 `firmware-name`（`modem.mdt` 与 `modem_dtb.mdt`），没有 `qcom,pas-id`；`qcom_q6v5_pas` 驱动自己按固件顺序给 pas_id。所以「modem 段是 38、modem_dtb 段是另一个值」这个对应关系目前**没有一手出处**，S1 必须从 CAF 的 `remoteproc-mss` 节点或主线 `qcom_q6v5_pas` 源码确认。**S4 之前确认不了就不能启动**——传错的 pas id 会让 SCM 静默失败或加载错误的段。
- modem 段的 carveout：`mpss_region`（`0x8a800000/0x10800000`）+ `mpss_dsm_region`（`0xd4d00000/0x3300000`）；modem_dtb 段的 carveout：`q6_mpss_dtb_region`（`0x9b000000/0x80000`）。
- **基带上下电与复位没有 AP 侧 GPIO**：`cx-supply = <0x26>` / `mx-supply = <0x72>` 指向 `rpmh-regulator-cxlvl`（`qcom,resource-name = "cx.lvl"`）下的 `regulator-pm-v6e-s6-level`（`qcom,init-voltage-level = <0x180>`、`qcom,set = <0x03>`）与 `rpmh-regulator-msslvl`（`qcom,resource-name = "mss.lvl"`）下的 `regulator-pm-v8-s7-level`（`qcom,init-voltage-level = <0x10>`、`qcom,set = <0x03>`）。注意 `cx.lvl` 是 RPMh 的 ARC 资源名、level 是资源类型字节，不是电压微伏——真值在 `/tmp/x13/device-info/cmd-db-fuxi.txt` 的 Command DB dump 里（`0x30000: cx.lvl`、`0x30010: mx.lvl`、`0x30060: mss.lvl`，各带一组 level 字节；`mss.lvl` 的 `0x38` 是 modem 相关的资源类型）。上下电由 AOP 经 SCM 投票驱动，AP 侧不直接写寄存器，上电顺序必须照 stock。

绕法：把 ADSP 那套抽成通用的 `QcomPas`，参数化 pas id、carveout、regulator 序列与 QMP 释放调用，ADSP 与 modem 共用。

### D6 DMA32 / 低 DRAM

事实：主线在 fuxi 上最惨的一次失败是**崩溃恢复时 modem_dtb 的 PAS 返回 -12**（`QCOM_SCM_V2_EBUSY`），根因是 `qcom_scm` 里的 `dma_alloc_coherent` 申请 4044 字节返回 NULL——所有保存的构建配置都关了 `CONFIG_ZONE_DMA`/`CONFIG_ZONE_DMA32`（`CONFIG_EXPERT=y` 使其可关），手机约 9.9 GiB 内存且 4 GiB 以上无低端区，32 位 coherent 掩码没有可退的低端区。打开这两个选项后 3 次崩溃全部恢复到 running（主线记录 modem10）。

UniXNU 侧没有 Linux 的 DMA zone 概念，等价问题是「PAS metadata 与段缓冲必须落在低 DRAM」。fuxi 的音频链已经解过这个问题，有现成解法。

音频那次的根因值得记下来：原来用 `IOBufferMemoryDescriptor::inTaskWithPhysicalMask` 运行时分配，实测拿到 PA = **`0x8CFF84000`（35.25 GiB）**，而 DSP 只接受 32 位地址，必然失败。改成**固定低 DRAM 映射**：

```c
constexpr uint64_t kLowPa = 0xd0000000ull;              // UEFI conventional gap
IODeviceMemory::withRange(kLowPa, 64 KiB)->map(kIOMapAnywhere | kIOMapWriteCombineCache);
```

其中 `kIOMapAnywhere` 是必须的——第一次漏了它，固定 PA 映射静默失败（`map()` 没报错，但真机没有 `FUXI_ADSP PCM owned` 日志行），跟 `mapWindow()` 的用法对比才定位到（kernel `f689b660` → `10dd91e8e188ffa9`）。映射前要拒绝与内核物理窗（`gPhysBase`/`gPhysSize`）和 GPU addon（`native_ram_block_gpu_addon` + `kvtophys`）重叠，页对齐、整段清零后 `dsb sy`。真机已 PASS（`pcm_pa = 0xd0000000`、span 64 KiB、`graph_phase = 22`）。

绕法：modem PAS 复用音频那条固定低 DRAM 映射路径，不另造机制。modem 的段缓冲可能比 64 KiB 大很多（`mpss_region` 有 264 MB），要按实际段大小分段映射或改用别的低 DRAM 区间——**这一条要在 S1 定，标成待验证**。

### D7 AP 不能预分配 IPA 的 SMEM item

事实：fuxi 的 modem 固件自己分配 `SMEM_IPA_FILTER_TABLE`（item 497，大小 `0xb000`，与 `qcom,ipa-q6-smem-size = <0xb000>` 一致），并在发现 AP 已预分配时 assert。主线的解法是 `ipa_smem_map_region()`：用 `of_find_compatible_node(NULL,NULL,"qcom,smem")` 取整个 SMEM region，一对一 `iommu_map` 整段 2 MB，并置 `smem_modem_owned`，跳过 `qcom_smem_alloc()`。仅在 `compatible = "xiaomi,fuxi"` 时走这条路径。

影响：照抄主线的 `ipa_smem_init()` 会让基带启动即崩。

### D8 SIM / RF 事实缺口

事实：

- 运行中 DTB 里**没有** SIM 槽位节点、没有蜂窝 RF 前端节点、没有 eSIM/SAID。全局 grep `sim|uim|esim|euicc` 只命中 27 处，全部是 DSI 面板的 "Simulator" 模式（`qcom,mdss_dsi_sim_cmd`、`qcom,mdss_dsi_sim_video`、`qcom,mdss_dsi_dual_sim_*`）与 `simple-bus` 的巧合匹配，没有一处是 SIM 卡槽。
- **GPIO 32–39 归 TrustZone**。运行中 DTB 里写的是逐个引脚号：`qcom,gpios-reserved = <0x20 0x21 0x22 0x23 0x24 0x25 0x26 0x27>`（即 32 到 39，8 个），主线把它写成区间形式 `gpio-reserved-ranges = <32 8>`。缺这一句时 TLMM probe 读这些引脚的寄存器会触发 XPU 违规：黑屏 + 整机卡死 + 看门狗复位。AP 侧不可访问。
- 蜂窝 RF 走 Q6 内部 RF + 外部 PMIC/瞬态开关，不在 DT 里以节点暴露（这是「没找到」，不等于「确认没有」）。
- 蜂窝侧的「射频供电」范式可以照抄 Wi-Fi 的写法，但要抄对对象：fuxi 的 Wi-Fi 是 WCN7850（Kiwi），走 PCIe0，设备 ID `pci17cb,1107`；主线 fuxi 配方 `0013-board.patch` 给它加了 `wcn7850-pmu` 节点（PMU 节点 + `wlan-enable-gpios` GPIO80、`bt-enable-gpios` GPIO81 + 六路 RFA 电压 `vddrfa1p2/1p8` 等 + `vreg_pmu_rfa_cmn`）。这是 Wi-Fi 的，**不是蜂窝的**；蜂窝 RF 没有对应的 DT 节点。

影响：卡 1 是物理 SIM（中国移动），UIM 走 QMI 读卡，不需要 RF 前端节点。但需要回答两个问题：SIM 卡槽在 AP 侧有没有任何可读的东西（例如卡检测 GPIO），以及基带侧的 RF 通路是否需要 AP 侧参与（库存证据说不需要）。

风险：无线卡的检测与热插拔完全在基带内，AP 侧只通过 UIM 的 card status 得知。

**S7 的目标在主线 Linux 上已经达成过一次，同一台 fuxi**（`/tmp/x13/docs/目标-M5.md`）。这不是推测，是已实测的记录：

- UIM 读到两个物理卡槽都是 ACTIVE，provisioning 映射为 Primary GW = 卡槽 1 应用 1、Secondary GW = 卡槽 2 应用 2、Secondary 1X = 卡槽 2 应用 1（主会话记录 10-31/11-01）。
- 卡 1 首次注册到 CHN-UNICOM 460-01（home），LTE `limited`，RSRP -82 dBm、SNR 21.8 dB，附着 APN 3GNET ipv4v6（记录 10-30 首注那次）。
- 13:26 卡 1 在联通 46001 **5G NR** home、PS attached、**信号 50**（记录 11-01 13:26）。
- 主卡 5G NR 460-01 时 MM connected、IMS registered、SMS/voice available（11-01 恢复记录）。
- 卡 2（副卡，电信）实测：PLMN 460-11、TAC 62072、RSSI -52、RSRQ -11、RSRP -81、SNR 9.2 dB，且主卡 5G 不受影响。

所以 S7 要做的事在硬件与基带侧已经验证过。XNU 侧要做的是**换一套宿主**：主线是 Linux 用户态（ModemManager + qmicli + qrtr-ns + 81voltd + rmtfs），XNU 侧是内核里的 QMUX/QMI 桥接。**这一步的难度不在「能不能读到卡」，而在「能不能把 Linux 那套用户态服务在 XNU 上重新搭起来或替换掉」**——这直接支持 S8（独立工具闭环）先于 S9（CommCenter 集成）的排序。

主线踩过的两个坑要一并继承：

- **ModemManager 的双卡 slot 循环有 bug**：MM 1.25.95 的 `mm-shared-qmi.c` 把最后一个 ACTIVE 卡槽当作 active slot，双卡都 ACTIVE 时与主会话读到的 ICCID 不一致，报 "SIM identifier changed" 后反复 reprobe。补丁 `work/patches/mm-simslot-20261030/mm-shared-qmi-active-slot.diff` 改成优先 logical slot 1。XNU 侧没有 MM，但「双卡都 ACTIVE 时如何判定当前卡」这个问题同样存在，S7 的单一 radio state owner 要正面处理。
- **`mmcli --set-primary-sim-slot` 是个空操作**，双卡都 ACTIVE 时它以为已经是目标卡槽。XNU 侧不要设计成「切当前卡槽」这种语义。

**另一条主线证据：基带在 QRTR 上注册的服务很少**。崩溃前基带（QRTR 节点 0/1）只见到 RFS(14)、TFTP(4096)、servreg locator(64)、IPA 控制(49)；ADSP（节点 5）服务正常。这说明**服务发现要走 QRTR NEW_SERVER，不能假设服务列表固定**——S5 的完成标志「能看到基带宣告的服务节点」是对的，但不能预先知道会有哪些服务。

### D9 iPadOS 侧的接入与版本锁定

事实：

- 上游 `QcomBaseband` 组件块（`components.toml` 上游）声明 `os_abi = ["ipados-17.7.11-21H461 AppleBaseband IOUserClient"]`，`build_stage = "kernel"`，`build = ["build-qcom-baseband.py"]`，输出 `build/qcom-baseband/libQcomBaseband.a`，运行时标识含 `AppleBaseband`、`AppleBasebandPCIControl`、`AppleBasebandPCIInterface`、`AppleIPAppender`、`AppleBasebandPCIMAVPDPManager`、`pdp_ip0..3`。
- 组件 `dependencies = []`——不依赖别的组件，但依赖板级提供 modem 启动、GLINK/QRTR、rmtfs。
- `platform/coachz/patches/coachz-prelink-baseband.patch` 作用在 XNU 10063，打的是三件事：① **接 QRTR 钩子**——在 XNU 的 QRTR 收包处加 `coachz_baseband_qrtr(dst_port, src_node, src_port, body, size)`，在 QRTR 的 NEW_SERVER/DEL_SERVER 控制消息处加 `coachz_baseband_new_server()`/`del_server()`，GLINK 起来之后调 `coachz_baseband_configure(wb_send, wb_del_client)`，并在轮询循环里调 `coachz_baseband_poll()`；② **启用 cellular CommCenter job**（靠 `/chosen/coachz-baseband` 决定要不要跑）；③ **在 launchd 之前挂载 Mav16 固件目录**（解 `CZBBFW01` 归档，写到 `/private/var/coachz/baseband/Mav16`，再用只读 union 挂成 `/private/preboot/<HASH>/usr/local/standalone/firmware/Baseband/Mav16` 与 `/usr/standalone/firmware/Baseband/Mav16`）。
- `platform/coachz/tools/coachz_cellular_compat.py` 是 CommCenter 二进制补丁：校验 SHA-256 与 Mach-O UUID，改一个条件跳转分支，重新 ad-hoc 签名并写 trust cache。**绑定特定 iPadOS 构建**。
- 启用方式是 ADT 属性，不是启动参数（内核命令行有 1000 字节上限，pending 的 `scripts/unixnu/payload.py` 改动把它放宽到 1023，仍然不够放基带参数）：`/chosen/coachz-baseband`、`/baseband`、`/product/baseband-chipset = mav16`、根 `compatible = J121AP`。
- **fuxi 当前的 ADT 机制只差一个顶层节点名**：`scripts/unixnu/devicetree.py` 只把 `defaults`、`options`、`product`、`buttons` 四个顶层节点写进 ADT（`for name in ('defaults', 'options', 'product', 'buttons')`）。`/chosen` 已经在里面（`chosen += typed_properties(platform.get('chosen', {}))`，`board.toml` 已有 `[adt.chosen.u32]`、`[adt.chosen.string]`、`[adt.chosen.bytes]` 三张表），`/product` 也在里面（`[adt.product.*]`）。**只有顶层 `/baseband` 节点没有出口**——`platform_node()` 本身支持任意子节点，扩展点是给 `devicetree.py` 加一个顶层节点名，再在 `board.toml` 加 `[adt.baseband.*]`。
- 属性类型也要对上：桥接层用 `OSData` 读 `/chosen/coachz-baseband`（`bb_chosen("/chosen", "coachz-baseband")`，长度必须是 4 字节的 u32，见 `baseband/coachz_baseband.cpp:112`），所以它必须写成 `[adt.chosen.u32]`，写成 string 或 bool 都不生效。`/product/baseband-chipset` 是字符串，走 `[adt.product.string]`。
- fuxi 的 `board.toml` 现在把 `com.apple.CommCenter` 放在 `sheng-disable-jobs` 里（连同 identityservicesd、locationd、bluetoothd、liveactivitiesd）。

影响：

- 目标系统是同一个 21H461，`coachz_cellular_compat.py` 的 SHA-256 可能直接可用——但必须校验，不能假定。
- ADT 加 `baseband` 节点会触发 cellular CommCenter 的 KeepAlive 反复启动，而不是自动可用。必须三道闸门：先不带节点手动起 CommCenter 看缺什么契约 → 契约齐了再发布节点 → 稳定后才从 `sheng-disable-jobs` 里放出 CommCenter。
- 组件名与代码里的 `coachz_*` 前缀在 fuxi 上是误导。上游仓库是 `UniXNU/QcomBaseband`，fuxi 适配层要么在 fork 里另起名字，要么接受这个前缀。
- `devicetree.py` 的顶层节点白名单是本 fork 的自研改动（相对合并基点本地改了 40 行，上游没动这个文件，所以它**不在** 98 处冲突里）。加 `/baseband` 节点时要改 `scripts/unixnu/devicetree.py`，落在 S0 合并之后做最省事——但因为上游没碰它，合并时可以直接保留本地版，不必参与解冲突。

### D10 构建与工具链的隐性依赖

事实：`scripts/build-qcom-baseband.py` 只是转调 `drivers/QcomBaseband/scripts/build.py --tree .`，而后者用「所选 kernel 的 compile record」编译。两个 record：`build/xnu/ios-obj/**/IOPlatformExpert.cpo.json`（C++，编 `baseband/coachz_baseband.cpp`）与 `build/xnu/ios-obj/**/vfs_conf.o.json`（C，只在 `--coachz` 时要）。脚本从 record 的 `arguments` 里剔掉 `-o`/`-MF`/`-MT`/`-MJ`/`-MD`/`-MP` 与 `-W*` 选项，把 record 里指向原文件名的参数换成组件自己的源文件，然后加上 `-I <SOURCE>/baseband -I <SOURCE>/ipa -mgeneral-regs-only -Wall -Wextra`。它要求 record 目录里能跑 `cc`，最后 `xcrun ar rcs` 打包。

影响：D1 的合并结果直接决定 D2–D6 的编译输入是否可用——compile record 的路径与形状都写在 `scripts/unixnu/kernel.py` 里，而那个文件正是 98 处冲突之一。所以这两项必须绑在一起验证：合并完先确认 `build/xnu/ios-obj/**/IOPlatformExpert.cpo.json` 还在、形状没变，再谈编译组件。

### D11 事实来源的缺口

| 缺什么 | 现状 | 影响 |
| --- | --- | --- |
| LineageOS fuxi 板级 devicetree overlay | 本机没有（`facts/audio/manifest.toml` 引用的 `android_kernel_xiaomi_sm8550-devicetrees` @ `43b5ada5` 不在本地） | 板级 RF/SIM wiring 仍缺 |
| modem 固件本体 | 设备自己的 `modem_a`/`modem_b` 分区，FAT 只读可取 | 不缺（已有读取通道） |
| modem 段与 modem_dtb 段的 PAS id | 推断 0x26（38），无一手出处 | 需确认，S4 的前置 |
| IPA v5.5 Linux 源 | 本机没有 Linux 树（`/tmp/x13` 只是配方，`manifest.json` 指向 `linux-7.4-rc3` tarball） | `gen_ipa_data.py` 需要一份可编译的 v5.5 树 |
| 基带芯片型号 | 仓库里无出处（X65 只是推断） | 不影响实施 |

## 3. 阶段目标

每阶段一个可观测结果和一个停止条件。阶段之间串行，前一阶段的停止条件触发就停下，不往后推。

通用安全红线（`baseband-porting-plan.md` §3 的七条）全部适用：① 设备事实只以本机 stock live DT、Android 实测、分区元数据或固定版本的 fuxi 源码为依据，其他 SM8550 设备只作线索；② 只读取用 `modem` 固件输入，每个文件记来源、分区、版本与 SHA-256，**不得拿 Apple baseband firmware 替代 Qualcomm MPSS 固件**；③ 不写 `modemst1`/`modemst2`/`fsg`/`fsc`/`persist`，不复制其他设备的 EFS/NV，不改 IMEI、射频校准与运营商安全材料；④ 未完成控制面校验前保持 RF 离线；⑤ 基带/IPA/兼容层失败必须可关闭，不能拖死 AP、UFS、USB、显示与 GPU，每阶段都保留「不发布 baseband 节点、继续禁用 CommCenter」的回退路径；⑥ 设备启动测试只有所有者明确要求时才做，优先可恢复的临时启动；⑦ 日志默认脱敏（IMEI、IMSI、ICCID、号码、基站标识、鉴权材料不得完整落盘）。

本文件额外加两条本机专用红线（`BUILD-AND-TEST.md` §0）：**绝不刷分区**，真机启动只用 `fastboot boot`（唯一例外是 `scripts/fuxi-audio-s0.sh` 的 `fastboot flash boot`，且只用于 S0 基线冻结）；**一次只扩大一个危险动作**——同一轮里不要同时首次启用 PAS、GLINK、QRTR 和发射，否则失败无法归因。也不用 Android boot / ADB / vendor ramdisk。

### S0 合并上游，不带基带

目标：把 D1 的成本与基带成本分开。

- 合并 `origin/main`，解 98 处冲突：73 处文档取上游（只保留 fuxi 自己那条记录）、5 处仓库根文件手工合、11 处其他脚本与数据手工合、6 处 `scripts/unixnu/*.py` 手工合、3 处 submodule 指针取上游。`components.toml` 手工合出 29 个组件的并集（上游加 `QcomBaseband`/`QcaBluetooth`/`QcomDwc3`，本地保 `FuxiFtsTouch`），**其中 9 个共有组件的 revision 分叉要逐条定**（只有 `Adreno740` 带本地 overlay 必须保本地）。
- 保留本地 `scripts/unixnu` 的自研改动，语义不回退。本地改过而上游没动的文件（如 `devicetree.py`）直接保本地版，不参与解冲突。

完成标志：`./unixnu defconfig fuxi_defconfig -D ../unixnu-fuxi` → `./unixnu status --json` → `./unixnu build kernel` 在合并后的树上通过，产物与合并前功能等价（触摸、显示、音频、存储基线不回退）。

停止条件：合并需要改动 `patches/xnu/series` 的语义，或 `build kernel` 失败且根因不在合并冲突本身。

### S1 冻结 fuxi 基带事实

目标：把 D11 的缺口补上，使后续每个地址、IRQ、SID、PAS id 都有 fuxi 出处。

- 从 `fuxi-live.dts`（CAF 运行中 DTB）与 UEFI 的 `sm8550-xiaomi-fuxi.dtb` 抽出并交叉核对：MPSS remoteproc 节点、PAS id、carveout、regulator level、SMP2P、GLINK channel、IPCC mailbox、SMEM item、IPA MMIO/IRQ/clock/SMMU CB、rmtfs 共享内存。
- 事实分级沿用 `baseband-porting-plan.md` §5 的三类：`confirmed-device` / `confirmed-source` / `hypothesis`，只有前两类可进板级配置。
- 产出 `facts/baseband/` 证据集与 `facts/baseband/manifest.toml`（含固件来源：分区、路径、大小、SHA-256）。
- 确认 modem 段与 modem_dtb 段的 PAS id（0x26 只是日志推断，待证）与 carveout 归属。

完成标志：能画出从 AP 启动 MPSS 到 modem 进入 QRTR 服务的完整 stock 时序；每个将来要用的地址、IRQ、SID、PAS id 都有一手出处。

停止条件：关键 carveout 与 AP 内存布局冲突；stock 证据对 PAS id、SID、IRQ 存在未解释的矛盾。

### S2 引入 QcomBaseband 组件（不带板级）

目标：打通 D10 的构建链，先证明组件能在本 fork 编译链接。

- `components.toml` 加 `QcomBaseband`（按上游块，revision 固定到上游 pin），`.gitmodules` 加 `drivers/QcomBaseband`。
- `drivers/QcomBaseband` 引入 fuxi 需要的部分。`scripts/build.py` 的开关是 `--coachz`：不带它时只编 `baseband/coachz_baseband.cpp`（C++ 记录），带它时再加 `ipa/coachz_ipa_config.c`、`ipa/coachz_gsi.c`、`platform/coachz/coachz_modem.c`（C 记录）。fuxi 需要自己的第三个开关（`--fuxi`），带上 `platform/fuxi/fuxi_modem.c` 与 v5.5 的 IPA 表——**这是对上游构建脚本的改动，要提出到上游组件，不要在 fork 里私改**。
- 不带 `--coachz` 的构建里，`coachz_baseband.cpp` 以头文件方式包含 `coachz_bb_qmux.h`、`coachz_bb_sahara.h`、`coachz_bb_control.h`、`coachz_bb_channels.h`、`coachz_bb_data.h`、`coachz_bb_pdp.h`、`coachz_bb_esim.h`，所以 **QMUX、SAHARA、用户态契约、数据口、PDP 接口与 eSIM 桥都会一起进库**。fuxi 无 eSIM（见 D8），`coachz_bb_esim.h` 的 VINYL/UIM/ISD-R 桥要显式关闭；`coachz_bb_data.h` 是纯协议层（DPM 硬件口、WDA raw IP/QMAPv4 协商、PCIe `{3,4}` → embedded `{4,1}` 端点翻译），协议方向可用，fuxi 的实际端点号要等 S10 的 v5.5 表确认后再对齐。
- **明确关掉三块 Mav16 专用物**（见 §1）：SAHARA 设备端（`coachz_bb_sahara.h` 的整个 `sahara` 通道与 `CBS_*` 消息）、Mav16 固件归档（`CZBBFW01` 解包与挂载，XNU 侧那部分属于 prelink 补丁）、`baseband-chipset = mav16` 身份。这三块的上游形态都要动，**改动提出到上游组件**，不要在 fork 里私改。
- `board.toml` 与 `configs/fuxi_defconfig` 接入 `libQcomBaseband.a`（`CONFIG_XNU_EXTRA_LD_ARCHIVES` + force_load；fuxi 现在这行的写法是 `"{tree}/build/fuxi-touch/libFuxiTouch.a -Wl,-force_load,{tree}/build/fuxi-audio-runtime/libFuxiAudioRuntime.a -Wl,-force_load,{tree}/build/sheng-hid-input/libShengHidInput.a {tree}/build/boot-log/libBootLog.a -Wl,-force_load,{tree}/.cache/fuxi-qud-build/libQcomXnuUsbDebug.a -Wl,-force_load,{tree}/build/fuxi-keystore/libFuxiKeyStore.a"`），并按 `platform/coachz/patches/coachz-prelink-baseband.patch` 的形态做 XNU 侧钩子：IOKit 类注册、`/chosen/coachz-baseband` 读取路径、`coachz_baseband_configure` 的调用点。

完成标志：镜像带上了 `libQcomBaseband.a`，内核起来不 panic，`/chosen/coachz-baseband` 未设置时组件保持静默（不发布任何服务）。

停止条件：XNU 侧钩子导致内核 panic 或链接缺符号无法在不改动 `patches/xnu/series` 语义的前提下解决。

### S3 fuxi 板级适配层（transport 之前）

目标：完成 D3 的绑定，先不启动 modem。

- 新建 `platform/fuxi/`：`fuxi_modem.c`（owner 线程 + D2 的 IPA v5.5 表 + D3 的地址表 + D6 的低 DRAM 缓冲 + D7 的整段 SMEM 映射）。
- 组件里 `coachz_*` 前缀改为中性命名（`fuxi_*` 或 `qcom_*`），同时保留 `QcomBaseband.h` 的对外接口形状。
- IPA 表用改造后的 `gen_ipa_data.py` 从 Linux v5.5 源重新生成。需要的 v5.5 源在主线配方里：`python3 scripts/fuxi-kernel-source.py prepare --tree <tree>` 会下载 `linux-7.4-rc3.tar.gz`（校验 `archive_sha256`），按 `kernel/manifest.json` 逐个 apply 14 个 patch，落到 `<tree>`。**本机现在没有这棵树**（`/tmp/x13/build/linux-standard` 不存在），要跑一次 `prepare`——下载 ~250 MB，是这一步的前置。
- `gen_ipa_data.py` 现在写死六处：commit `6628e7d4c6b25c8d18f1345e3a20d21829aae7e4`、三个文件名 `ipa_reg-v4.2.c` / `gsi_reg-v4.0.c` / `ipa_data-v4.2.c`、两个表符号 `ipa_regs_v4_2` / `gsi_regs_v4_0`、数据符号 `ipa_data_v4_2`、include guard `COACHZ_IPA_V42_H`、输出文件名 `coachz_ipa_v42.h`。参数化这些后，v4.2 与 v5.5 两张表都能从各自的 Linux 树生成，数字仍全部由编译出的表得出。
- **生成器对 v5.5 的结构差异要单独确认**：它读 `ipa_data-v*.c` 的 `ipa_data` 结构（`endpoint_data`、`resource_data`、`mem_data`、`power_data`）与 `enum ipa_endpoint_name`。v5.5 的端点数比 v4.2 多（`IPA_ENDPOINT_COUNT` 变了），所以 `ipa_mem.h` 里的 `IPA_MEM_STATS_FILTER_ROUTE`、FNR 常量（`IPA_QMI_MODEM_FNR_COUNT` 等）也要一起对齐。这些常量在生成器里是硬编码引用还是从表里取，参数化时要一并处理。

完成标志：host 侧模型测试通过（`python3 scripts/check.py` 加新的 v5.5 端点表断言）；静态检查确认适配层里没有 SC7180 的任何地址。

停止条件：v5.5 的寄存器/字段模型与 `QcomBaseband` 现有代码结构冲突到需要改 `QcomBaseband` 公共部分——那时先把改动提出到上游组件，不要在 fork 里私改。

**进度（2026-11-18）**：传输部分（S3 的一半）已完成并接进内核。`platform/fuxi/qcom_qrtr.{h,c}` 是可移植的 QRTR 协议层，`platform/fuxi/fuxi_modem.c` 是 GLINK/SMEM 上的 fuxi 适配层，host 模型测试通过，`libQcomBaseband.a` 已 force-load 进 fuxi 内核。**未完成**：IPA v5.5 的表（`gen_ipa_data.py` 参数化、v5.5 源树准备）、D3 的地址表、D6 的低 DRAM 缓冲、D7 的整段 SMEM 映射、owner 线程。注意 IPA 那部分属于 S10 的数据面，不在卡 1 目标的路径上，可以后置。

### S4 启动 MPSS（BB0）

目标：把 modem 作为可控 remote processor 启动起来，RF 保持受控关闭。

- 抽出通用 `QcomPas`（从 FuxiAdspProbe 抄）：`PAS_INIT_IMAGE` → `PAS_MEM_SETUP` → 段拷贝（`qcom_mdt_place`）→ `PAS_AUTH_AND_RESET` → AOP 释放。
- modem 参数：PAS id、carveout（`mpss_region` / `q6_mpss_dtb_region` / `mpss_dsm_region`）、AOP QMP `0x43`（`qcom,qmp = <0x43>`，node `power-controller@c300000`，`qcom,kalama-aoss-qmp`）、`clocks = <0x3b 0x00>`（xo）。
- 固件来源走已有的 `"modem:NAME"` 只读通道：`package/FuxiAdspFirmware.c` 已经实现了完整的分段读取——`openModem()` 打开 `/image/NAME.b00`（ELF 头，≤64 KB），用 `qcom_mdt_split()` 解析出段表，再逐段打开 `/image/NAME.b01`…`NAME.b%02u`；`qcom_mdt_split_read()` 按虚拟偏移把段拼回来。ADSP 已经在用这条通道取 `modem:adsp` 与 `modem:adsp_dtb`（`package/FuxiAdspProbe.cpp:1120`），modem 侧对应 `modem:modem` 与 `modem:modem_dtb`。
- **关键点**：`qcom_mdt_split()` 要求 piece 0 就是 ELF 头本身（`split->piece[0].offset` 必须为 0），所以这不是「把若干段拼成一个 .mbn」，而是「按 MDT 段表把散落的 .b%02u 拼回原始 MDT 镜像」。PAS 的 `INIT_IMAGE` 需要的正是这个还原后的镜像，**不需要另写拼接代码**，也不落盘、不写分区。分区本身只读（`fat_mount` 走 UFS 分区读接口），符合「不写 `modemst1`/`modemst2`」的红线。
- 段表解析用的是 CAF 的 MDT ELF 头（`e[0..3] == 0x7f 'E' 'L' 'F'`、32/64 位 ELF 均支持），**与 AP 侧的 QMI/QRTR 无关**，所以这套解析对 modem 镜像同样有效——但前提是 fuxi 的 `modem.b00` 头确实是 CAF 的 MDT ELF 格式，这一点要在 S1 用真机分区验证，不能照搬 ADSP 的结论。
- 状态通道：ready、stop-ack、fatal、watchdog、shutdown-ack（`interrupts-extended` 里 6 个信号）。

完成标志：连续 20 次启动/停止无 AP panic、无 carveout 污染、UFS/USB/GPU/显示不回退；失败时留下明确阶段与 SMC 返回码。

停止条件：固件来源或 PAS id 不确认；PAS 返回非零且与 stock 行为不一致；无法保证失败时 AP 不受影响。

### S5 打通 GLINK + QRTR（BB0+）

目标：AP 与 MPSS 的可靠双向 IPC，并能枚举基带服务。

- GLINK：开 `IPCRTR` 与 `DS` 两个通道（`QcomGlink` 现有能力，`QCOM_GLINK_MAX_CHANNELS 6` 有余量）。intent **两个通道不一样**：`qcom,modem_qrtr`（`qcom,glink-channels = "IPCRTR"`，带 `qcom,low-latency`）是 `qcom,intents = <0x800 0x05 0x2000 0x03 0x4400 0x02>`；`qcom,modem_ds`（`qcom,glink-channels = "DS"`）是 `qcom,intents = <0x4000 0x02>`。edge 的 `qcom,remote-pid = <0x01>`、`transport = "smem"`、mbox `mpss_smem`。
- IPCC：实现 doorbell 路由与中断收发（`QcomScm.h` 的 `qcom_ipcc_id` 与 `QCOM_IPCC_CLIENT_*`/`QCOM_IPCC_SIGNAL_*` 已有编码）。
- QRTR：node/port、HELLO、NEW_SERVER/DEL_SERVER、lookup。**必须带上主线的 HELLO 竞态修法**，否则基带服务一个都看不到。
- modem 服务：modem 的 node/port 不写死，由基带通过 QRTR NEW_SERVER 公告时学到（`coachz_modem.c` 的 `cm_modem_node`/`cm_modem_port` 就是这样填的）。

完成标志：QRTR 名字服务里能看到基带宣告的服务节点；双向小包、大包、ring wraparound、backpressure 正确；空闲时无持续 CPU 占用；触发 modem restart 后旧请求正确失败、新通道自动恢复，连续 50 次无泄漏。

停止条件：GLINK 打开后 modem 立刻 fatal（说明 S4 的资源序列不完整，回 S4）。

**进度（2026-11-18）**：QRTR 协议层与 GLINK 适配层写完并链接进内核（见 D4 的实现状态）。**未做**：IPCC doorbell、owner 线程、modem 侧 NEW_SERVER 的真机验证。代码里 `fuxi_modem_start()` 已经把 HELLO 发出去了，但还没有调用方——modem 的 PAS 启动（`fuxi_modem_start()` 的前置条件：SMEM 窗口映射、doorbell kick 回调）是 S4 的活。

### S6 只读 QMI 控制面（BB1 前半）

目标：服务发现和健壮事务层，不做会改变网络状态的请求。

- **QMUX 与 QMI 的分工要分清**（`baseband/coachz_bb_qmux.h` 的设计）：iPadOS 的 ABM 说的是 QMUX（iPadOS 侧 `qmux1`/`qmux2` 通道上的 Apple QMUX 帧头），而基带在 QRTR 上说的是 QMI。两者除 6 字节 QMUX 头外完全一致，所以桥接层负责拆头：发出去时剥掉 6 字节，答回来时加上。
- **CTL 由内核本地应答**，不发给基带（`cbq_ctl()` 的 `switch` 只有四个分支）：GetClientId `0x0022`（读 TLV `0x01` 取 service，按 channel+service 分配 client id 与本地 QRTR port `CBQ_PORT_BASE + slot`；服务不存在回 `CBQ_ERR_INVALID_SERVICE_TYPE`，client 用尽回 `CBQ_ERR_CLIENT_IDS_EXHAUSTED`）、ReleaseClientId `0x0023`（TLV `0x01` 2 字节 service+cid，释放后把该 slot 标 `release_pending`，等调用方发完 QRTR DEL_CLIENT 才回收）、Sync `0x0027`（释放该 channel 上全部 client）、GetVersionInfo `0x0021`（列当前在场的服务，每项 5 字节：service + version + instance，最多 32 个）。
- **RevokeClientId `0x0024` 落在 default 分支**：上游代码里它不是独立分支，而是跟 ConfigPowerSaveMode 之类一起「回成功、不做事」，并计入 `ctl_other`。基带重启后的 client id 回收靠的是 Sync 指示（`cbq_ctl_sync_indication()`）与 `cbq_client_free()` 的 `release_pending` 两级机制，不是 Revoke。
- client id 是**每 channel 每 service 各自 1..254**（0 留给 CTL，255 是广播），分配后从 `last_cid[channel][service]` 往后轮转。
- **Apple 私有服务 0xe1–0xea（AUDIO 到 SFT）基带上不存在**：`cbq_private_service()` 判 0xe1..0xea。给它们分配 client id 后，请求以 `CBQ_PRIVATE` 回到桥接层，由桥接层实现或用 `QMI_ERR_NOT_SUPPORTED`（94，`CBQ_ERR_NOT_SUPPORTED`）回绝。
- **服务版本取最小 instance**：`cbq_server_find()` 返回 instance 最小的那个（`qmicli` 的 QRTR 后端取它找到的第一个）。QRTR 的 version_instance 是低字节 version、高字节 instance（`cbq_server_new()` 里 `version_instance & 0xff` 与 `version_instance >> 8`）。
- QMUX 通道固定 2 个（`CBQ_CHANNELS 2`，qmux1/qmux2），服务上限 `CBQ_MAX_SERVERS 64`、client 上限 `CBQ_MAX_CLIENTS 64`，本地 QRTR port 从 `CBQ_PORT_BASE 0x4100` 起（`base + client slot`；Wi-Fi 与 IPA 占 0x4000..0x4003）。fuxi 要按需调这三个上限。
- QMI/QMUX 的 header/TLV 编解码、transaction id、request/response/indication、超时取消。
- 服务优先级：CTL → DMS → NAS → UIM。Voice/WMS/IMS 后置。
- 解析器拒绝越界长度、重复关键 TLV、未知版本、过大消息；未知非关键 TLV 跳过。

完成标志：服务枚举在每次 MPSS 启动后稳定；1000 次只读事务无错误累积；未知 TLV 与超时不破坏内存；SSR 后服务重新发现且旧 transaction 不会误配给新响应。

停止条件：出现无法归类的协议错误；QMI 层出现内存破坏迹象。

### S7 读取卡 1 与卡 1 的信号（最终目标的落点）

目标：**正常读取卡 1（物理 SIM，中国移动）并收集卡 1 的信号。**

这一步在主线 Linux 上已经实测达成过（见 D8 末的记录）：同一台 fuxi 上 UIM 读到卡 1，卡 1 注册到联通 46001 5G NR、PS attached、信号 50，LTE 时 RSRP -82 dBm / SNR 21.8 dB。**硬件与基带侧没有未解的问题**；XNU 侧要解决的是宿主替换——把 Linux 的用户态服务栈（ModemManager / qmicli / qrtr-ns / rmtfs / 81voltd）换成 XNU 内核里的 QMUX/QMI 桥接。

顺序严格按 `baseband-porting-plan.md` §5 阶段 5：

1. UIM 报物理卡存在与应用类型（`UIM Get Card Status`）。主线实测：两个物理卡槽都 ACTIVE，provisioning 映射为 Primary GW = 卡槽 1 应用 1、Secondary GW = 卡槽 2 应用 2、Secondary 1X = 卡槽 2 应用 1。
2. 读 ICCID / IMSI（TLV 0x42 / 0x43，reading-order BCD），日志只留不可逆摘要或末四位。
3. 查 PIN/PUK 状态；**测试期间禁止 PIN 尝试**，避免锁卡。
4. 单一 radio state owner（这一步是本阶段内建，不引入第二个切 RF 的进程）。**不要设计「切当前卡槽」这种语义**——主线实测 `mmcli --set-primary-sim-slot` 在双卡都 ACTIVE 时是空操作。
5. 查支持制式与频段，与 stock 基线对照。
6. PLMN 扫描 → 自动选择 → 注册 → 注销 → 飞行模式切换。
7. 读信号质量：RSSI / RSRQ / RSRP / SINR、驻留制式（LTE/NR）、服务状态。基站标识脱敏。

卡 1 与副卡的协议差异要在这一步之前确认（`/tmp/x13/docs/调研-双卡QMI-20261101.md` 有逐条出处）：

- 各服务的「第二张卡」取值不同——NAS/VOICE/WMS/IMSA/IMSS 从 0 开始（0 主、1 副），**WDS/DMS/DSD 从 1 开始**（0 默认、1 主、2 副）。
- 副卡查询法：新开一个 NAS client 发 Bind Subscription `0x0045` = 1，此后该 client 的所有类型化调用都作用于副卡。
- **DMS Bind Subscription 是 `0x0054`，4 字节**（1 主 / 2 副 / 3 第三）；WDS Bind 是 `0x00AF`，4 字节，`QmiSubscriptionType` 里 **2 = 副**。
- NAS Set Dual Standby Pref 是 `0x004B`，TLV `0x14` active_subs_mask 是 8 字节位掩码（bit0 主、bit1 副）；**TLV `0x15` dds_duration 已不支持，带上会报错**。
- eSIM 侧不适用：fuxi 无 eSIM 节点与 SAID（见 D8），本阶段只做物理卡 1。

完成标志（就是最终目标的判定）：

- 卡 1 可重复识别，读到卡状态、ICCID、IMSI。
- 能读到并持续刷新卡 1 的信号指标与注册状态，数值与手机原生界面的量级一致。
- RF off/on 与注册/注销各 50 次无卡锁死、无 MPSS 崩溃。
- 弱信号、无 SIM、PIN locked、无服务、漫游五种状态可区分。
- 飞行模式最终一定使 RF 离线。

停止条件：发现 NV/校准不一致；异常高频重启；不可控发射；SIM PIN 尝试计数异常。

安全约束：这是第一次真正发射的阶段。用合法 SIM、本机频率、遵守本地法规；日志脱敏 IMEI/IMSI/ICCID/号码/基站 ID；每轮只做一个危险动作。

### S8 独立工具闭环（把 S7 变成可用能力）

目标：不依赖 CommCenter，也能由用户态工具读卡 1 与信号。

- owner-signed 的 `fuxi-basebandd`（名字可调）+ 最小 IOKit UserClient，暴露版本化的高层能力：modem 状态与固件版本、SIM/PIN 只读状态、RF 与注册状态、信号指标、受控 SSR/诊断（仅开发 entitlement）。
- 不开放任意 QMI、任意物理地址映射或 DMA buffer。每个 selector 有 entitlement、长度、状态、调用者检查；API 带版本与 modem generation。
- CLI 能完成 S7 的全部读取。

完成标志：独立 CLI 完成 S7 全部操作；杀 broker 不让 RF/数据面失控；无权限进程打不开控制接口；旧 generation 句柄全部拒绝。

### S9 CommCenter 集成（BB3，后置）

目标：iPadOS UI/API 与真实基带状态一致。

- 契约审计：21H461 的 CommCenter 实际匹配哪个 IOService/class，读哪些 DT/property；开哪些 UserClient type/selector、共享内存、notification port、异步回调；`AppleBaseband` / `AppleBasebandPCIControl` / ARI 里哪些是必需契约、哪些只适用 Apple PCIe modem。
- shim 对外满足必要服务契约，对内调 S8 的 broker；不让 `AppleBasebandPCI` 直接映射 Qualcomm MMIO 或拥有 IPA。
- 三道闸门：① 设备树先不带 `baseband`，手动起 CommCenter 看缺什么；② 契约齐了再发布 `baseband` 节点；③ 稳定并能退回 noncellular 后，才从 `sheng-disable-jobs` 里放出 `com.apple.CommCenter`。
- `coachz_cellular_compat.py` 的 CommCenter 补丁：先校验 SHA-256 与 Mach-O UUID 是否匹配 21H461，不匹配就重做（版本锁定的重做成本在这一阶段一次性付清）。

完成标志：CommCenter 单实例稳定、无 KeepAlive crash loop；CoreTelephony 能区分无 SIM/PIN locked/无服务/注册/漫游/数据连接；状态栏与设置 UI 只呈现真实底层状态；飞行模式、重启、CommCenter 重启、MPSS SSR 后状态最终一致；关掉测试开关能恢复 noncellular plist 且不影响启动。

停止条件：契约审计发现 21H461 与 coachz 记录的接口形状不同，且补齐成本超过 S9 的价值——此时停在 S8，把 S9 记录为独立项目。

### S10 数据面（BB2，独立项目）

目标：蜂窝数据可用。

- IPA firmware 也走 PAS（pas id `0x0f`）+ `ipa_fw_region`（64 KB），不是普通 request_firmware。
- GSI ring 与 event ring、TRE 描述符、endpoint 编程、filter/routing、checksum offload、聚合。
- QMAP/rmnet mux、WDS client endpoint、IPv4/IPv6 family、MTU。
- XNU 侧发 `pdp_ip0`–`pdp_ip3`（mux 1–4，MTU 1492），地址/网关/DNS/路由来自 modem 实际响应，不写死 Android 参数。
- 端点翻译：Apple PCIe `{3,4}` → embedded `{4,1}`（`coachz_bb_data.h` 的做法）。
- 六层验收：GSI ring 自检 → 收发计数增长 → 拿到地址 → ICMP/DNS/TCP/UDP 与双栈 → 大包/PMTU/并发/长流量 → 断网/切站/RF off/on/PDP stop/start/SSR 恢复。

完成标志：IPv4/IPv6、DNS、MTU 与 WDS 响应一致，接口 down 后无残留路由；10 分钟冒烟 + 1 小时持续流量无 DMA/SMMU fault、ring stall 或不可解释丢包；100 次 PDP 建立/断开无资源增长；USB/UFS/GPU/显示/触控基线不回退。

停止条件：IPA v5.5 的 GSI 行为与 v4.2 模型差距超出可移植范围（此时回到 S3 评估）。

### 依赖关系

```text
S0 合并 ──┐
S1 事实 ──┼─→ S2 组件 ─→ S3 适配层 ─→ S4 MPSS ─→ S5 QRTR ─→ S6 QMI ─→ S7 卡1+信号 ─→ S8 工具
           │                                                                    │
           └────────────────────────────────────────────────────────────────────┴─→ S9 CommCenter
                                                                                 └─→ S10 数据面
```

S0 与 S1 可并行。S9、S10 依赖 S8，但彼此独立。

## 4. 与通用方案的阶段对照

`docs/baseband-porting-plan.md` 的阶段 0–10 与本文 S0–S10 的对应：

| 通用阶段 | 本文阶段 | 说明 |
| --- | --- | --- |
| 0 冻结范围 | S1 | 本文把它落到具体的抽取清单与证据分级 |
| 1 黄金基线 | S1 | 同一件事，本文指明从 CAF 运行中 DTB 与 UEFI DTB 取哪几类事实 |
| 2 启动 MPSS | S4 | 本文补上 fuxi 的 PAS id、carveout、regulator level 与固件读取通道 |
| 3 IPC | S5 | 本文补上 fuxi 的 GLINK 通道名、intent、IPCC 与 QRTR HELLO 竞态 |
| 4 QRTR/QMI | S6 | 本文补上 QMUX/QMI 分工、CTL 本地应答与私有服务的处理 |
| 5 SIM/注册 | S7 | 本文把它收敛为最终目标，并补上双卡取值差异 |
| 6 IPA/数据面 | S10 | 本文补上 v5.5 与 v4.2 的六处差异 |
| 7 独立 broker | S8 | 直接对应 |
| 8 CommCenter | S9 | 本文补上 fuxi 现有的 `sheng-disable-jobs` 状态与三道闸门 |
| 9 短信/语音/IMS | 不在本轮 | 依赖音频栈，另立项目 |
| 10 电源/SSR/稳定性 | 分散在各阶段的停止条件里 | 长期门槛在 S8 之后另开一轮 |

通用里程碑 M0–M5 与本文 S 阶段的对应：M0 = S0+S1，M1 = S2–S5，M2 = S6+S7，M3 = S8，S4 = S9，S5 = S10 + 语音/IMS 项目。

## 5. 第一轮工作包

按 `baseband-porting-plan.md` §8 的精神，先做设计输入，不写驱动：

1. S1 的事实抽取（`facts/baseband/`）。
2. S0 的合并解冲突（不夹带基带改动）。
3. modem 段与 modem_dtb 段 PAS id 的确认（`hypothesis` → `confirmed`）。
4. 一份可编译的 Linux v5.5 IPA 树（`python3 /tmp/x13/scripts/fuxi-kernel-source.py prepare --tree /tmp/x13/build/linux-standard`）。
5. `gen_ipa_data.py` 的 v5.5 参数化方案。

### 5.1 下一轮工作包（2026-11-18 起，按依赖就绪顺序）

传输层已经就位，接下来按依赖往前推：

1. **S4 的 modem PAS 启动**：通用 `QcomPas`（`PAS_INIT_IMAGE` → `PAS_MEM_SETUP` → 段拷贝 → `PAS_AUTH_AND_RESET` → AOP 释放），参数取自 CAF DTB 的 modem remoteproc 节点与 carveout。`package/FuxiAdspFirmware.c` 的 `"modem:NAME"` 只读通道已经能取 `modem:modem` 与 `modem:modem_dtb`（待确认名字）。
2. **IPCC doorbell**：GLINK 的 kick 回调与 modem 的 doorbell。
3. **modem owner 线程**：跑 `fuxi_modem_poll()`，这是 `fuxi_modem_start()` 的调用点。
4. **真机验证**：QRTR HELLO 发出、收到 modem 的 NEW_SERVER、名字服务里出现 QMI 服务。这一步通了，S6 才有对象。
5. **S6 只读 QMI 控制面**：CTL 本地应答、GetClientId、UIM（卡状态）、NAS（注册状态与信号）。

第 4 步是这一步与下一步的分界：**在真机看到 QRTR 服务之前，S6 的代码没有可测的对象。**

## 6. 与通用方案的关系

`docs/baseband-porting-plan.md` 是通用分层（阶段 0–10、BB0–BB4、依赖图、安全规则、证据格式、里程碑 M0–M5）。本文件是它的 fuxi 具体化：把每个通用阶段绑到本机的实际困难项（D1–D11）与实际证据（`fuxi-live.dts`、UEFI DTB、上游组件块、`/tmp/x13` 主线配方），并把阶段目标收敛到「读取卡 1 并收集卡 1 的信号」。

两者冲突时以 `baseband-porting-plan.md` §3 的安全规则为准。

## 7. 本文的证据来源

| 事实 | 出处 |
| --- | --- |
| modem remoteproc 节点、PAS、regulator、SMP2P、GLINK channel 与 intent、carveout、IPA MMIO/IRQ/clock/SMMU CB、IPA hw ver 0x18、SMEM region | `/tmp/x13/work/device-info/fuxi-live.dts`（CAF 运行中 DTB，37744 行） |
| UEFI 侧的 stock DTB 基线 | `uefi-fuxi/Platforms/Xiaomi/fuxiPkg/FdtBlob/sm8550-xiaomi-fuxi.dtb`（已反编译为 `/tmp/fuxi_uefi.dts`） |
| IPA v5.5 的六处差异、QRTR HELLO 竞态、SMC 诊断日志、modem-tables 地址 | `/tmp/x13/kernel/patches/0005-qrtr.patch`、`0006-ipa.patch`、`0007-scm.patch`、`0013-board.patch` |
| 双卡 QMI 取值差异 | `/tmp/x13/docs/调研-双卡QMI-20261101.md` |
| **卡 1 已实测注册与信号**（UIM 卡槽 ACTIVE、46001 5G NR、信号 50、RSRP -82 dBm / SNR 21.8 dB、副卡 RSRQ -11 / SNR 9.2 dB）、ModemManager 双卡 slot 循环 bug、`--set-primary-sim-slot` 空操作、基带 QRTR 只注册 RFS/TFTP/servreg locator/IPA 控制、同一次开机内无法重试基带 | `/tmp/x13/docs/目标-M5.md`、`/tmp/x13/docs/交接-新会话-20261101-*.md`、`/tmp/x13/work/patches/mm-simslot-20261030/mm-shared-qmi-active-slot.diff` |
| 低 DRAM 映射的已验证配方（`0xd0000000`、64 KiB、`kIOMapAnywhere` 必需、`0x8CFF84000` 是失败值） | `docs/fuxi-audio-porting-log.md` §P3 |
| 挂起时 MPSS 是唤醒源 | `/tmp/x13/TASKS.md` |
| 上游组件定义与接入方式 | `UniXNU` 的 `origin/main:components.toml`、`scripts/build-qcom-baseband.py`、`docs/porting/baseband.md` |
| 桥接层的协议与契约 | `UniXNU/QcomBaseband` @ `dd0199c` 的 `baseband/coachz_bb_qmux.h`、`coachz_bb_control.h`、`coachz_bb_sahara.h`、`coachz_bb_data.h`、`baseband/coachz_baseband.cpp`、`platform/coachz/coachz_modem.c`、`platform/coachz/README.md`、`scripts/build.py`、`ipa/gen_ipa_data.py` |
| 现有基座 | `package/FuxiAdspProbe.cpp`（PAS 时序）、`package/QcomScm.h`、`package/QcomGlink.h`、`package/QcomSmem.h`、`package/QcomSmp2p.h`、`package/QcomMdt.h`、`package/FuxiAdspFirmware.c`、`facts/audio/binding.toml` |
| 组件接入与 ADT 现状 | `board.toml`、`configs/fuxi_defconfig`、`dts/fuxi.dts` |
| 合并冲突分类 | `git merge-tree --write-tree HEAD origin/main`（UniXNU 仓库，HEAD = `b973dae`） |
| 通用分层与安全规则 | `docs/baseband-porting-plan.md` |
