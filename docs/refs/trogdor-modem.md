# LTE 基带（蜂窝数据）

2026-11-02：已写好六块，放在 [pending/modem](../../platform/coachz-prelink/pending/modem/README.md)，由 drivers/RamBlock/coachz_modem.c 就地编译接入，真机已跑通到握手完成（见“真机结果”），rmnet_ipa0 也已在真机上打开（见“数据通路接入”）：IPA 与 modem 的 QMI 握手层；IPA 探测期配置（主线 `ipa_config` 全部步骤）；GSI setup（主线 `gsi_setup`）；端点编程和命令通道（`ipa_setup` 里的 `ipa_endpoint_setup`、命令通道启动、`ipa_mem_setup`、`ipa_table_setup`）；LAN RX 和默认路由（`ipa_setup` 的最后一段）；`ipa_modem_start` 和数据通路（rmnet_ipa0 与 rmnet 的 QMAP）。主线 `ipa_setup` 到 `ipa_qmi_setup` 之前已全部写完。寄存器值已和 Debian 上主线驱动配好后的真值对照。

## 路线（照主线 Linux）

MPSS 已经在跑（Wi-Fi 补丁：MBA/PIL、SMEM、SMP2P、GLINK、QRTR、rmtfs，IPA 时钟已投、SMP2P 的 IPA 时钟查询已应答）。主线里蜂窝数据走 IPA：`drivers/net/ipa`，LTE 机型 `qcom,gsi-loader = "modem"`（`sc7180-trogdor-lte-sku.dtsi`），之上是 `rmnet_ipa0` 和 rmnet；联网控制（UIM、DMS、NAS、WDS、WDA）在 Linux 上是 user space 的 libqmi/ModemManager。主线顺序：

1. **IPA 探测配置（已写）**：寄存器、内存布局和 `mem_offset`、中断、uC、端点、资源组、SMP2P。
2. modem 加载 GSI 固件后经 SMP2P 发 `ipa-setup-ready` → `ipa_setup`：**GSI、端点、命令通道、内存和路由/过滤表、LAN RX、默认路由（已写）**。
3. **QMI 握手（已写）**：AP 开 0x31/实例 1 服务、找 modem 的 0x31/实例 2，发 INIT_DRIVER（带 v4.2 内存布局），应答 DRIVER_INIT_COMPLETE 和 INDICATION_REGISTER，发 INIT_COMPLETE。
4. **`ipa_modem_start`（已写）**：modem 端点、`rmnet_ipa0`（QMAP）、rmnet。
5. QMI 控制：卡状态、上线、注册、WDS 拨号拿 IP、WDA 数据格式；再往上才是 iPadOS 的网络接口和蜂窝界面。

## 写了什么

IPA 配置：`gen_ipa_data.py` 在主机上编译主线的 `ipa_reg-v4.2.c`、`gsi_reg-v4.0.c`、`ipa_data-v4.2.c`，按主线枚举把真实的表导出成 `coachz_ipa_v42.h`（寄存器偏移、步长、字段掩码，端点表，资源限额），没有手抄的数字。`coachz_ipa_proto.h` 是可主机测试的判定：硬件寄存器值、`SHARED_MEM_SIZE` 解码与区域校验、canary 位置、FLAVOR_0 端点检查、资源限额编码、uC 应答规则。`coachz_ipa_config.c` 按主线顺序做 hardware → mem → interrupt → uc → endpoint → resource（v4.2 的 table 配置是空操作），另有中断处理（`ipa_isr_thread` 的主体，uC 报 INIT_COMPLETED 时置 `uc_loaded` 并经 AOSS QMP 发 `ipa_pc` 保持请求）、modem 启动前的 uC 供电标记、panic 时通知 uC。

GSI：`coachz_gsi_proto.h` 是主线 `gsi_channel_init` 建出的通道表（AP 通道按端点名顺序分事件环：命令 TX 通道 1 用环 0、LAN RX 通道 2 用环 1、modem TX 通道 0 用环 2、modem RX 通道 3 用环 3；v4.2 要 AP 替 modem 分配通道 0–3）、通道数据校验、上下文寄存器值、命令字、通用命令结果、环在一块 DMA 内存里的摆放。`coachz_gsi.c` 按 `gsi_setup` 做：查 GSI_STATUS、中断初始化、读通道和事件环数量、清错误日志、每个 AP 通道先分配并编程事件环再分配并编程通道，最后用通用命令替 modem 分配通道，失败时照主线回退。主线发命令后睡在 completion 上等 ISR，这里改为轮询同一个 ISR 主体，最长 50 ms。

命令通道：`coachz_ipa_cmd_proto.h` 是可主机测试的部分：`ipa_endpoint_program` 对每个 AP 端点写的寄存器值、`gsi_trans.c` 的 TRE 和事件格式、`ipa_cmd.c` 全部立即命令的载荷、`ipa_cmd_init` 的检查、`ipa_table_init` 建的表区，以及 `ipa_mem_setup`、`ipa_table_setup` 的命令清单（v4.2 没有哈希表：4 条表初始化加 2 条过滤表清零）。`coachz_ipa_cmd.c` 依次做端点编程、启动命令通道（通道 1）、发命令事务：每条命令占一个 TRE，载荷放在按 TRE 环下标对应的 DMA 槽里，最后一个 TRE 带 IEOT 和 BEI；完成判定照 `gsi_channel_update`，读事件环的 `EV_CH_E_CNTXT_4`，用事件里的 TRE 指针对上最后一个 TRE，再敲事件环门铃。主线等完成没有超时，这里给 1 秒并报告超时。`coachz_ipa_kernel.h` 放三个内核文件共用的结构。

LAN RX：`coachz_ipa_rx_proto.h` 是可主机测试的部分：缓冲和 TRE 布局（每个 8 KiB，数据放在 NET_SKB_PAD 之后）、最多挂多少（`gsi_channel_tre_max`，这里是 251）、补充时的门铃规则（每 16 个一次，或者 TRE 用完时）、包状态结构和 `ipa_endpoint_status_parse` 的跳过/标签/丢弃/上交规则、默认路由值。`coachz_ipa_rx.c` 做 RX 端点启用（启动通道、打开该端点的挂起中断、补充缓冲）、轮询完成（从事件取收到的长度、解析状态、缓冲重新挂回、确认事件、重新打开 IEOB），以及 `ipa_setup` 的收尾：LAN RX 启用后写 ROUTE。主线里 LAN RX 没有网络设备，上交的包只计数丢弃；命令端点发来的带标签状态表示管道清空完成。

数据通路：`coachz_ipa_modem_proto.h` 是可主机测试的部分。数据格式由 IPA 端点决定：AP_MODEM_TX 开了校验和、头长 8，AP_MODEM_RX 开了校验和，对应 rmnet 的 MAPv4（拆聚合 + 上下行 v4 校验和，flags 0x0d）。上行每个包前加 QMAP 头和全零的 UL 校验和头（即 rmnet 的软件校验和路径）；下行拆聚合、去掉 QMAP 头和 8 字节 DL 校验和尾（相当于 rmnet 没开 RXCSUM，由协议栈自己校验）。rmnet 数据设备 MTU 是 1500 − 8 = 1492。`coachz_ipa_modem.c` 做 `ipa_modem_start`、打开 rmnet_ipa0（启动 AP_MODEM_TX 的通道 0，AP_MODEM_RX 经 `coachz_ipa_rx` 挂缓冲）、按 mux id 建数据设备、发送（每帧拷进 DMA 中转槽、一个 TRE，主线在分片过多时也会线性化成一段）、轮询 TX 完成，以及按 mux id 上交 IP 包的下行回调。AP_MODEM_RX 没开状态元素，`coachz_ipa_rx.c` 按主线 `ipa_endpoint_skb_build` 把整个聚合缓冲交给 rmnet。还没有 XNU 的网络接口；已打开设备的停止（通道 stop/reset）也没移植。

握手：`coachz_ipa_qmi.h`：QMI 帧格式、IPA QMI 的消息号和 TLV（照 `ipa_qmi_msg.c` 的编码表）、按 v4.2 内存布局和运行时 `mem_offset` 构造 INIT_DRIVER、解析应答、服务端应答和 INIT_COMPLETE 指示，以及 `ipa_qmi_ready` 的就绪规则（首次启动要等指示发出；modem 重启时保留 uC 和首启状态）。

主机测试 `test_ipa_qmi.py` 9 项通过：数据通路模型 3 组（数据格式与端点一致、MTU、TX TRE，上行封帧，含 IPv4、带填充的 IPv6、MAP 命令和截断尾部的聚合缓冲拆帧）、RX 模型 3 组（缓冲 TRE、251 个缓冲的门铃规则、每条状态解析规则、默认路由与 Debian 的 ROUTE 0x01100050 一致）、命令模型 4 组（端点 1、6、8、9 的十个端点寄存器都和 Debian 真值一致，TRE 和事件，各条命令载荷逐字段，内存和表 setup 的命令清单，含 Debian 的 LOCAL_PKT_PROC_CNTXT 0x5f0）、GSI 模型 4 组（四个 AP 通道的通道和事件环上下文、QOS、scratch、HW_PARAM_2、通用命令结果都和 Debian 真值一致）、握手模型 3 组、配置模型 4 组（含实机的 SHARED_MEM_SIZE 0x415 和 Debian 的 FLAVOR_0），`coachz_ipa_config.c`、`coachz_gsi.c`、`coachz_ipa_cmd.c`、`coachz_ipa_rx.c`、`coachz_ipa_modem.c` 用 XNU 桩头文件做类型检查（后三个还按 b9 的方式合成一个编译单元检查）、生成头标明 Linux 提交、现有构建不引用它们。

## 接入方式

配置：Wi-Fi 补丁的 `wifi_ipa_power()` 之后调 `coachz_ipa_config(&ipa, zero_virt, zero_phys, wifi_qmp_send)`（8 KiB 设备可见内存；apps SMMU 处于旁路），主线是在 modem 启动之前的探测阶段做；MBA/PIL 开始启动 modem 时调 `coachz_ipa_modem_starting()` 并清掉 AP 的 SMP2P `ipa` 项（主线 `ipa_smp2p_notify_reset`）；GIC SPI 311 或轮询里调 `coachz_ipa_interrupt()`。另外 Wi-Fi 补丁打印 `ipa: setup ready` 时把 SHARED_MEM_SIZE 的高低 16 位读反了：主线 MEM_SIZE 在 15:0，MEM_BADDR 在 31:16，`mem_offset` = 8 × 高 16 位。

GSI：配置时一并调 `coachz_gsi_init(&gsi, ipa.gsi, ring_virt, ring_phys, bytes)`，给一块清零、64 KiB 对齐、写合并或非缓存、至少 40 KiB 的内存（主线也在 probe 时分配环）；收到 `ipa-setup-ready` 后，`ipa_setup` 第一步调 `coachz_gsi_setup()`。再调 `coachz_ipa_cmd_init(&cmd, &ipa, &gsi, virt, phys, bytes)`（至少 8448 字节、清零、128 字节对齐的 DMA 内存，64 KiB 环内存块从 0xc000 起还有空间）和 `coachz_ipa_setup_commands(&cmd)`。然后 `coachz_ipa_rx_init(&lan, &ipa, &gsi, CIPA_EP_AP_LAN_RX, virt, phys, bytes)`（128 字节对齐的写合并或非缓存内存；2 MiB 能挂满 251 个缓冲，和 Linux 一样，给得少就挂得少）和 `coachz_ipa_setup_lan_rx(&lan)`，`ipa_setup` 到此结束。`coachz_gsi_interrupt()` 在 `ieob_pending` 里标出 LAN RX 的事件环时，或者定时，调 `coachz_ipa_rx_poll(&lan)`。之后是 `ipa_qmi_setup`（握手），再往后是 `ipa_modem_start` 和 `rmnet_ipa0` 数据通路，还没写。

握手：在现有 QRTR 上：开 0x31 实例字 0x101 的服务；modem 通告 0x31/0x201 时发 INIT_DRIVER（需要第 1 步读到的真实 `mem_offset`）；收到 0x20、0x35 请求时应答；每个事件后跑 `ci_ready_step()`。这一层要等第 2 步 `ipa_setup` 做完才有意义。

## 已有的实机事实

XNU 下（还没接这些代码）：modem 发了 `ipa-setup-ready`（SMP2P `ipa` 项 0x2），当时 AP 还没做任何 IPA 配置；SHARED_MEM_SIZE 是 0x415，`mem_offset` 为 0，报告的 8360 字节超过 ipa-shared，按主线 `mem_size` 仍取 0x2000；QRTR 上有 0x31 版本 1 实例字 0x201 的服务。Debian 上（主线驱动 setup 完成，`rmnet_ipa0` 存在）把 IPA 设为常开，经 /dev/mem 只读读了寄存器：FLAVOR_0 0x08090811、HW_PARAM_2 0xe4046b59（11 个通道和事件环）、资源限额、COUNTER_CFG、IDLE_INDICATION 、全部 GSI 通道和事件环上下文、四个 AP 端点的十个端点寄存器、LOCAL_PKT_PROC_CNTXT（0x5f0，说明那边 `mem_offset` 也是 0）、ROUTE（0x01100050）、IRQ_SUSPEND_EN（0x100，`rmnet_ipa0` 没开时只有 LAN RX）都和这里写的一致。CLKON_CFG（0）、QSB_MAX_WRITES（0x78）、QSB_MAX_READS（0x7800000c）和 TX_CFG 的 PA_MASK_EN 与主线 probe 时写的不同；读之前设备处于运行时挂起，主线恢复时不重写它们，所以视为挂起后的值，代码保留主线 probe 的写法。

## 真机结果（XNU，一次启动，日志环）

b9 用 drivers/RamBlock/coachz_modem.c 按上面的顺序接入，在 Wi-Fi 线程的 1 ms 循环里轮询，没有接 SPI 311/432。预期日志全部一致：SHARED_MEM_SIZE 0x415（mem_offset 0，mem_size 0x2000）、FLAVOR_0 0x8090811、HW_PARAM_2 0xe4046b59、四个 AP 通道和事件环都已分配、modem 通道 0xf、两次命令事务完成（LOCAL_PKT_PROC_CNTXT 0x5f0，ENDP_INIT_MODE[6] 0x83）、LAN RX 挂了 251 个缓冲、ROUTE 0x1100050、IRQ_SUSPEND_EN 0x100；配置后 CLKON 读到 0x30000000、QSB 0x8/0xc，就是主线写的值。没有超时、状态错误、generic result 或 global error。握手走到了 INIT_COMPLETE：INIT_DRIVER（mem_offset 0，uc_loaded 0）得到成功回答，uC 报 INIT_COMPLETED 并发出保持请求，INDICATION_REGISTER 和 DRIVER_INIT_COMPLETE 都已应答。日志记录的 240 秒内状态一直保持，modem 在跑，Wi-Fi 不受影响；LAN RX 没收到包。

对照主线：INIT_COMPLETE 之后 modem 发来的 0x23 和 0x27 请求，在 `ipa_server_msg_handlers` 里没有处理函数（只有 0x20 和 0x35），而 `qmi_invoke_handler`（drivers/soc/qcom/qmi_interface.c）对没有处理函数的消息直接返回、不回应，所以只记日志不应答就是主线的行为。第一次 INIT_DRIVER 里 uc_loaded 为 0 是预期的：modem 首次启动时负责加载 uC，只有 modem 之后重启时 `skip_uc_load` 才为 1。INIT_DRIVER 回答里的 `modem_driver_init_pending` 为 1，表示还有第二次握手（DRIVER_INIT_COMPLETE），它也确实到了；主线解码回答，但只用其中的结果字段。

## 数据通路接入

握手完成（`ci_ready_step` 要求启动 modem）后：`coachz_ipa_rx_init(&modem_rx, &ipa, &gsi, CIPA_EP_AP_MODEM_RX, virt, phys, 2 MiB)`、`coachz_ipa_modem_init(&m, &ipa, &gsi, &modem_rx, tx_virt, tx_phys, bytes)`（每个 TX 槽 2 KiB，1 MiB 正好是通道上限 505 个），再 `coachz_ipa_modem_start(&m)`、`coachz_ipa_modem_open(&m)`。事件环 3 调 `coachz_ipa_rx_poll(&modem_rx)`，事件环 2 调 `coachz_ipa_modem_tx_poll(&m)`。没有数据呼叫也先打开，这和 Linux 拉起 rmnet_ipa0 时一样；要等 QMI 控制（WDA 数据格式、WDS 绑定 mux id、拨号）建好呼叫才会有数据，这部分还没写。预期日志："rmnet_ipa0 up: TX channel 0 (505 slots), RX endpoint 9 with 251 buffers, IRQ_SUSPEND_EN 0x300"。

真机已通过：rmnet_ipa0 两行日志符合预期，打开后 GSI 通道 0 和 3 的状态都是 2（已启动），没有数据呼叫时 modem RX 计数保持 0。还没有走过任何数据。

## iPadOS 蜂窝界面和 eSIM（调研，2026-11-02）

验收标准：蜂窝要在 iPadOS 界面里显示和配置（状态栏信号和运营商，设置里的蜂窝网络、APN、SIM 和 eSIM），并且要接 eSIM。只做内核拨号加一个裸网卡不算完成。

先例：UniXNU 的移植文档只有 Wi-Fi；ios-sweet、UniXNU/sheng、Droid-VM 都禁用 CommCenter，没有人接过蜂窝界面。

以下来自本地 21H461 IPSW 的内核缓存（prelink 信息）和共享缓存里库的名字、字符串，没有反汇编验证。

**苹果的栈就是“高通 modem 走 PCIe”。** 内核缓存里有：`AppleBasebandN71`（板级，供电和复位，按 `baseband,n41` 匹配，`AppleBasebandUserClient`）、`AppleBasebandPCI`（匹配 `baseband-pcie` 的 PCIe 设备，MHI/BHI）、`AppleBasebandPCIMAVControl`（控制面，`AppleBasebandPCIControlUserClient`，按 mav16 到 mav30 各代配置）、`AppleBasebandPCIMAVPDP`（数据面，`AppleBasebandPCIPDPQMAPSkywalk` 等在 Skywalk 上建 `pdp_ip`）。它的 MHI 通道表是高通标准的：2/3 SAHARA、4/5 DIAG、8/9 QDSS、10/11 远程 EFS（refs）、14/15 和 16/17 两路 QMUX、32/33 AT、100/101 QMAP 数据、109/110 QMAP 控制、105/106 低时延、104 RSC、92/93 语音。我们的 modem 是同一类芯片，只是控制走片上 QRTR、数据走 IPA，没有 PCIe/MHI。

**系统服务。** ABM（`libBasebandManager`）经 `libTelephonyBasebandDynamic`（`TelephonyBaseband*`：上电、复位、状态，对应 `AppleBaseband` 的 UserClient）和 `libPCITransport`（`TelephonyBasebandPCITransport*`：按编号打开管道读写、时间同步、共享内存映射，对应 `AppleBasebandPCIControl` 的 UserClient）接内核；在 QMUX 管道上建 QMI 客户端，再经 XPC 把 QMI 服务交给 CommCenter（`QMITransportService`、`QMuxServerAccepter`）。ABM 还自带远程文件系统（`RFS/FSModule`、`BBUFSServer`，相当于 rmtfs）和固件刷写（`BBUpdater`，经 SAHARA）。另有 `libmav_ipc_router_dynamic`（高通 IPC Router，即 QRTR），用途待查。

**QMI 服务。** 标准：CTL、WDS、DMS、NAS、QOS、WMS、UIM、PDC、WDA、DSD、DFS、SSCTL 等。苹果私有（只在苹果定制固件里有，我们的固件没有）：BSP（Board Support Package，ABM 用得最多）、AWD、VINYL、MFSE、MAVIMS、ELQM、Mav P2P、BSP APPS、Stewie。

**eSIM。** VINYL 的 “Tape” 操作（读 EID、eUICC 挑战值和信息、列出 profile、装载绑定 profile 包、启用/停用/删除 profile）就是 SGP.22 的 ES10 函数。标准做法是经 QMI UIM 打开到 ISD-R 的逻辑通道发 APDU（Linux 上 lpac、ModemManager 这么做），所以 VINYL 可以由桥接层用 UIM 实现。机器上 eUICC 的卡槽和 EID 还没读：要在 Debian 上用 QMI UIM 读卡槽状态。

**路线。** 内核里扮演苹果的基带接口：发布 `AppleBaseband` 和 `AppleBasebandPCIControl` 的 UserClient，让 ABM 以为有一个已经启动好的 PCIe 高通 modem；QMUX 管道转到 QRTR 上的 QMI（QMUX 的服务类型和客户端号对 QRTR 的服务和端口，CTL 由桥自己实现，libqmi 的 QRTR 后端就是这么做的）；EFS 继续由内核的 rmtfs 服务；数据面把 IPA 的 rmnet 接成 `pdp_ip`；苹果私有服务由桥应答，VINYL 用 UIM APDU 实现；启用 CommCenter。机型身份不换（换成别的屏幕尺寸，屏幕比例会坏）：保持 iPad14,1/J310，只把 IPSW 里蜂窝版 J121AP 设备树的 `/baseband`（`compatible = baseband,n41`、`euicc-chip-id`、GPIO 功能和 `config`）、`/arm-io/apcie/.../baseband-pcie` 和 `/product` 的 `baseband-chipset = mav16` 嫁接进来，并在 MobileGestalt 缓存里补蜂窝能力；确实需要蜂窝型号时用同屏幕的 iPad14,2/J311。已经真机通过的 IPA、GSI、握手、rmnet_ipa0 都还用得上。

**还要查（按顺序）：** ABM 启动基带的状态机，怎样跳过 SAHARA 刷写（字符串里有 “Transport bypass”“skipping establishing transport”）；两个 UserClient 的方法号和参数；CommCenter 启动和注册时必需哪些 BSP、AWD 等私有消息，哪些可以回“不支持”；`pdp_ip` 由谁建、CommCenter 怎么配置它；在不换机型身份的前提下，要嫁接哪些 ADT 节点、补哪些 MobileGestalt 项；Debian 上读 eUICC 的卡槽状态和 EID。

### 第二轮调研（反汇编 ABM、IOABP、TelephonyBaseband，2026-11-02）

都来自 21H461 的 IPSW：用 Xcode 自带的 dsc_extractor 从共享缓存解出库，用 otool 反汇编（库里带符号），内核 kext 信息来自内核缓存的 prelink 信息。苹果二进制只留在本机临时目录，不进仓库。

**启动。** `BootModule::boot_sync` 先问 `capabilities::abs::shouldBoot()`，返回 0 才走“Faking bootup”；这份正式版里它写死为 1，所以 ABM 每次都真启动：`BootControllerLegacy::prepare` 检查 `/usr/standalone/firmware/Baseband/` 里有固件，`bootModem` 交给 BBUpdater。正常开机（非恢复模式）不做个性化（“Not in restore mode - skipping gathering Personalization parameters”），只是把恢复时写好的镜像经 SAHARA 推给基带，等它进入运行态；还支持 BypassManifestStatusCheck、BypassNVMDownload/Sync 等选项。所以桥接层要在 sahara 通道上当一个接收端，收下镜像丢弃，然后报告运行态；磁盘上要放好固件文件。IPSW 里自带 `Firmware/vinyl_02_Mav16-11.00.01.Release.bbfw`（33 MB，含 SBL1、RPM、TZ、MBA、QDSP6SW、APPS、ACDB、BBCFG 和 eUICC 签名 profile），镜像打包时解到该目录即可，这些镜像永远不会真的在我们的 modem 上运行。

**设备树。** IPSW 里有蜂窝版 J121AP、J208AP 的设备树。J121AP 有 `/device-tree/baseband`（`compatible = baseband,n41`，`AppleBasebandN71` 按它匹配；`euicc-chip-id = 2`；bb_on、bb_rst、radio_on、coredump、reset_det 等 GPIO 功能；`config`）、`/arm-io/apcie/pci-bridge3/baseband-pcie`，`/product` 有 `baseband-chipset = mav16`。机型身份不换（iPad14,1/J310），只嫁接这几处。

**AppleBaseband UserClient（`libTelephonyBasebandDynamic` → `__TelephonyBasebandControllerCallMethod`）方法号：** 0 复位，1 上电/断电，2 复用开关，3 读复位，6 断电重启，8 VBUS，11 读 PMU EXTON，16 读 DIAG 就绪，17 读 EFS 擦除完成，18 读复位请求，19 coredump GPIO，20 时间同步，22/23 设置/读取管理器状态，25 充电器去敏，26/27 读写基带状态，28/29 读写基带属性，32/34 频点表，35–37/41/45 SPMI 和 GNSS，42 coredump 就绪，43 经 SPMI 让基带崩溃。都是 GPIO 和状态，内核里直接应答即可。

**通道 UserClient（`libIOABP`）。** 每条通道一个 `AppleBasebandPCIInterface` 服务，`IOABPClass::start_nl` 按名字匹配（名字应是 MHI 通道表的协议名：qmux1、sahara、refs、diag……）。方法号：0 open（一个标量），1 close，2 write，3 read（各有同步 `IOConnectCallMethod` 和异步 `IOConnectCallAsyncScalarMethod` 版本），4 sendImage，5 readRegister，6 中止通道，7 启动通道，8 时间同步，9/10 映射/解除映射共享内存，11/12 注册/注销时间事件。控制连接 `AppleBasebandPCIControl`（`IOABPControlClass`）：0 open，1 close，2 读日志，3 日志器调参；日志另有 `AppleBasebandPCITraceBackendRaw`。上层 `libPCITransport` 只是包装这些。

**私有 QMI。** CommCenter 的高通命令驱动里能看到 BSP（ActivationRegister、SetAntennaPreference、EnableAQMReport、GetCDRXInfo、SetAudioLoggingEnabled……）、AWD（指标上报）、ELQM、P2P、VINYL（Info、TapeInfo、VSimEnable、EosStatus）、AUDIO（语音，iPad 用不上）。静态看不出哪些是注册的硬依赖：桥接层对没实现的私有消息先回“不支持”，上机读 CommCenter 和 ABM 的统一日志再补。

**数据面。** 高通这条路的 `pdp_ip%1u` 由内核 `AppleBasebandPCIMAVPDP` 在 QMAP 上建（`pdp_ip0`…），CommCenter 拨号后配置它；`ApplePDPHelper` 属于另一条数据面（ACDP/ACIPC），这里用不到。QMAP mux id 来自 CommCenter 发给基带的 WDS 绑定 mux 请求，桥接层能直接看到，按它把 IPA rmnet 的 mux 和 `pdp_ip` 对上。

**MobileGestalt。** 蜂窝能力主要从 IORegistry（`AppleBaseband` 服务、`/baseband` 节点）推出；身份不变，让 MobileGestalt 在有基带节点的 ADT 上重建缓存（`coachz-reset-mgcache` 已有）。

**桥接层的组成（据此）：** 内核里发布 `AppleBaseband`（上面的方法表，状态由我们的 MBA/PIL 和 SMP2P 驱动）、`AppleBasebandPCIControl`、各通道的 `AppleBasebandPCIInterface`（qmux1/qmux2 ↔ QRTR 上的 QMI，自带 CTL；sahara 当接收端；refs 由已有 rmtfs 处理或回空；diag/at 等先不开）、`pdp_ip` 网卡接 IPA rmnet；私有服务和 VINYL（UIM APDU）由桥应答。

**还没查的：** `AppleBasebandPCIInterface` 用什么属性区分通道（名字放在哪个键里）；ABM 读基带状态的取值含义（26/27 号方法的状态码）；BBUpdater 在 sahara 上实际收发的报文（hello、读芯片信息、image transfer、done），决定接收端要回什么；QMUX 帧格式和 CTL 客户端号分配（可照 libqmi 的 QRTR 后端）；`pdp_ip` 是 Skywalk netif，普通 ifnet 能否被 CommCenter 接受。

### 第三轮调研（通道匹配、状态、SAHARA、QMUX，2026-11-02）

**通道服务怎么被找到。** `IOABPClass::start_nl` 用 `IOServiceMatching("AppleBasebandPCIInterface")`，再往字典里放 `ABPInterfaceProtocol = <名字>`（ASCII 的 CFString），等首次匹配和终止通知。名字来自 `libPCITransport` 的 `{编号, 名字}` 表：0 pbl、1 sahara、2 diag、3 refs、4 qmux1、5 qmux2、6 rom、7 boot、8 ari、9 trace、10 at、11 dpl、12 mediactl、13 vcom、14 gnss、15 qdss、17 virtual_timesync、20 boot_logger。所以内核里每条通道发布一个类名为 `AppleBasebandPCIInterface` 的服务，属性 `ABPInterfaceProtocol` 写通道名。打开：`IOServiceOpen` 类型 0，然后方法 0 带一个标量 0；服务上的 `IOGeneralInterest` 消息转给 EL0（错误和状态通知，消息码还没查）。

**通道方法的参数（IOConnectCall* 前的寄存器）。** 2 write、3 read：同步版两个标量加输入结构；异步版两个标量（EL0 缓冲地址、长度），引用数组 3 项，第 1 项是 EL0 回调、第 2 项是 refcon，IOKit 按参数个数调用回调，所以完成时回一个参数（字节数）。4 sendImage 两个标量，异步完成回两个参数。5 readRegister 一个标量。6 中止、7 启动通道各一个标量（方向），完成不带参数。8 时间同步三个标量，回一个参数。9 映射共享内存三个标量（区域号、地址、长度），10 解除映射一个标量。11/12 时间事件各一个标量。控制服务 `AppleBasebandPCIControl`：0 open（一个标量），1 close，2 读日志（异步，三个标量），3 日志器调参（四个标量）。

**基带状态。** `AppleBaseband` 的 26/27 号（读写基带状态）和 22/23 号（管理器状态）只是 ABM 自己写、自己读回：`BootController::setBasebandState` 先问 `capabilities::radio::supportsBasebandStateController()`（无线电类型在 9 到 15 之间才为真），再调 26/27 号；读回失败只记日志。内核不需要理解这些值，存住、原样返回即可。`AppleBaseband` 由 `libTelephonyBaseband` 按服务名 `AppleBaseband` 找。

**SAHARA。** `libETLSAH` 的报文号：1 HELLO、2 HELLO 应答、3 READ_DATA、4 镜像传完、5 DONE、6 DONE 应答、7/8 复位、9 内存调试、0xb 命令就绪、0xc 切换模式；苹果扩展：0xa0/0xa1 擦除查询和应答、0xa2 根清单查询、0xa3/0xa4 根清单应答、0xa5 哈希查询、0xa9 配置查询。模式：0 待传、1 完成、2 内存调试、3 命令、0x100 Maverick（苹果的清单和随机数流程）。Mav16 用 `BBUEUR16UpdateSource` 和 `BBUEUR10Programmer`：主机先等设备的 HELLO（应答里的保留字放产品类型和硬件配置），模式 0 或 1 进入镜像传输，按设备的 READ_DATA（镜像号、偏移、长度）发数据，镜像传完后发 DONE，设备回 DONE 应答（0 还有下一个，1 全部完成）。也就是说主机是被动的：发什么镜像、发多少完全由设备的请求决定。镜像号按 `EUR10Item` 的表：0x15 SBL1、0x17、0x19、0x1c、0x1d、0x1e、0x1f、0x06、0x08、0x05、0x10/0x11/0x14（EFS）、0x21、0x22、0x24、0x25、0x26、0x28、0x29、0x2a、0x30，以及苹果自己的 0x4300、0x4700、0x4c00、0x4f56、0x5400。桥接层在 sahara 通道上扮演设备：发 HELLO，按 ELF 头请求一个镜像的各段（像高通的 PBL 那样），收下丢弃，报告传完，DONE 应答报完成。要请求哪些镜像号、主机是否接受只请求一部分，由 BBUpdater 自己的日志（“request image ID”“item not found”“All images transferred”）在真机上确认。

**QMUX。** 苹果的 `QMuxHeader` 前 6 字节就是标准 QMUX 头：I/F 类型 1、长度（不含 I/F 字节）、控制标志（主机发 0x00，服务发 0x80）、服务类型、客户端号，后面是 QMI SDU。普通服务的 SDU 头（标志、2 字节事务号、消息号、长度）和 QRTR 上 QMI 的头完全一样，所以转发只是去掉或加上这 6 字节；CTL 的事务号是 1 字节，由桥接层自己应答。苹果的解析库里 CTL 只有 GetClientId（0x22）、ReleaseClientId（0x23）、Sync（0x27，含指示）、ConfigPowerSaveMode，以及 RevokeClientId 指示。QMUX 服务类型号：标准服务和 QRTR 服务号相同（WDS 1、DMS 2、NAS 3、UIM 11、WDA 0x1a、COEX 0x22、PDC 0x24、DSD 0x2a、SSCTL 0x2b、DFS 0x30 等）；苹果私有服务 AUDIO 0xe1、BSP 0xe2、CIQ 0xe3、AWD 0xe4、VINYL 0xe5、MAVIMS 0xe6、ELQM 0xe7、P2P 0xe8、APPS 0xe9、SFT 0xea，modem 上没有，由桥接层分配客户端号并逐条应答。每个 QMUX 客户端在 QRTR 上占一个本地端口（libqmi 的 QRTR 后端也是每个客户端一个套接字），释放时发 DEL_CLIENT。

## iPadOS 蜂窝接入

桥接层放在 [pending/baseband](../../platform/coachz-prelink/pending/baseband/README.md)，已接入内核：
1. 可移植部分：QMUX 与 QRTR 互转和 CTL、SAHARA 设备端、`AppleBaseband` 方法表。
2. 内核 IOKit 服务和 UserClient（`coachz_baseband.cpp`）。
3. Wi-Fi 线程的 QRTR 挂钩，以及 kern_exec.c 的固件写入、联合挂载和放行 CommCenter（`coachz-prelink-baseband.patch`）。

打包是 `tools/xnu_screen.py --baseband`（设 `/chosen/coachz-baseband`、嫁接 J121AP 的 `/baseband` 节点和 `baseband-chipset`）加 `tools/linux_log.py --baseband`（把 Mav16 固件放到内核分区里 modem 固件之后）。launchd 按 `/baseband` 选 `com.apple.CommCenter.plist`（`_LimitLoadToDeviceTree`），没有这个节点时用 `-noncellular` 那份。

真机上服务、固件和两处挂载都通过。`compatible` 用 J121AP 后 CommCenter 走高通分支并启动 ABM，状态栏显示搜信号。发布 `AppleIPAppender` 和 `AppleBasebandPCIMAVPDPManager` 后 CommCenter 不再崩溃，“蜂窝网络”页面能打开，固件目录 bind 挂载到 ABM 要的路径后 BBUpdater 能加载全部镜像，走完 `pbl`（读寄存器、`SendImage`）和 `sahara`，ABM 报告启动成功并 engage，CommCenter 经 qmux 接到真实的 modem，识别到 SIM 卡。modem 的工厂数据（FSG：IMEI、射频校准）在 eMMC 硬件分区 `mmcblk1boot0`，ChromeOS 的 `verify_fsg.sh` 这样读它：头 `FSG`，偏移 4 处 8 字节 ASCII 大小，数据从偏移 512 开始，SHA-256 烧在 qfprom nvmem 偏移 0x750。Debian 的全 0 文件加 `rmtfs -r` 让 modem 停在 `factory-test`，没有 IMEI，能力也是空的。把它只读读出、对上熔丝、补零到 2 MiB，放到被忽略的 `.tools/modem/rmtfs/modem_fsg`，`linux_log.py --modem-firmware` 会把它打进固件块，所以内核分区和镜像都含设备数据，不能外传；不写 boot0，不改 IMEI。内核的 rmtfs 不加载长度为 0 的文件，读写超出文件末尾时报错，这两点和 Linux 的 rmtfs 不同，所以四个文件都要有内容。

有了它，CommCenter 不再报 `num of IMEIs (0)`，`kSimStateReady`，“关于本机”显示 IMEI 和 ICCID，QMI 服务从 37 个变成 40 个。所用测试卡没有服务，modem 在 LTE 上搜网，CS、PS 都没有附着。有效 SIM 的注册和附着见下面的第五、第六次上机。

测试卡下 modem 收到 LTE 小区，CommCenter 报 `kEmergencyOnly`（仅限紧急呼叫）。

### 第四轮调研（数据口、激活、eSIM 能力）

**数据口。** CommCenter 的 `QMIDataContextIPBase` 按能力二选一：WDS Bind Data Port（0x89，TLV 0x01 SIO 口）或 WDS Bind Mux Data Port（0xa2，TLV 0x11 mux id，有端点时另加 TLV 0x10）。它填的端点是常量 `{类型 3（PCIe）, 接口 4}`；休眠驱动的那次绑定只带 mux id 1。QOS Bind Data Port（0x2b）带 TLV 0x11 mux id 或 TLV 0x12 数据口，不带端点。这台 modem 的数据走 IPA，ModemManager 对 `ipa` 驱动用 `{类型 4（embedded）, 接口 1}`，并且在第一次绑定前先发 DPM Open Port（硬件口 `{4, 1, rx = AP_MODEM_TX 的端点号, tx = AP_MODEM_RX 的端点号}`，即 IPA v4.2 的 1 和 9）和 WDA Set Data Format（raw IP，上下行 QMAP，下行最多 32 个包、32768 字节，带端点；QMAP 版本依次试 v5、v4、QMAP，以 modem 回报的为准）。modem 公布了 WDA（26）和 DPM（47）。CommCenter 两样都不发。桥接层（`coachz_bb_data.h`）在 CommCenter 第一次要 WDS 客户端时用自己的客户端照这个顺序做一遍（IPA v4.2 是 MAPv4，所以从 QMAPv4 开始），并把 WDS 0xa2、QOS 0x2b、WDA 0x20/0x21 里的端点换成 `{4, 1}`，只有 mux id 时补上。

**激活。** `/cc/props/baseband_activated` 为假时，`RegistrationModel` 不更新运营商名（`Operator name change aborted: not activated`），`DataServiceController` 也不激活数据上下文；`RegistrationController`、`TravelHandler` 同样等它。CommCenter 用 `QMIMav13ActivationCommandDriver`：BSP（0xe2）的 ActivationRegister 0xb00c 请求，之后读同号指示的 TLV 0x01（一字节 `bsp::ActivationState`：0 kActivated、1 kReady、2 kNotReady、3 kError；2 到 4 之间的值会等下一条指示）。苹果的基带在激活票据被接受后报 kActivated。这台 modem 没有激活锁，桥接层回成功并发 kActivated。指示里还有 IMEI、各槽 ICCID、IMSI、`Slot1IsEmbedded`（0x58）、`Slot2IsEmbedded`（0x59）、`DsdsAllowed`（0x53）等可选 TLV。

**eSIM 能力。** `EURSimCommandDriver` 从 UIM Get Card Status（0x2f）应答的苹果私有 TLV 0x21（旧版 0x20）读每个卡槽的能力，每槽一字节：1 kVinylCapable、2 kTapeCapable、3 kHydraCapable、4 kVirtualHydraCapable，其它值和缺省都是 kNotCapable，CommCenter 期望两个元素。VINYL 服务的请求里有 Tape 系列（`TapeReqGetEid`、`TapeReqGetDeviceCapabilities`、`ReqTapeReqCommand` 和 `TapeRequestPayloadData`），即经基带转发给 eUICC 的 GSMA 指令。哪个槽是 eUICC 还没在这台机器上读到。

### 第五次上机（数据口、激活、注册）

- **数据口**：CommCenter 第一次要 WDS 客户端时，桥接层发的 DPM Open Port 返回成功；WDA Set Data Format 返回成功，link 2（raw IP），上下行都是 8（QMAPv4）。WDS 0xa2 和 QOS 0x2b 的请求都只带 mux id，桥接层补上 `{4, 1}`。绑定本身的结果内核没有记录。
- **激活**：ActivationRegister 回 kActivated 后，CommCenter 记 `Activation state changing from kNotProvisioned to kActivated`、`Baseband activated, initiating network attach`。
- **工作模式**：这次开机 ABM 第一次读到的工作模式是 Shutdown，CommCenter 把射频留在低功耗；约 40 s 时基带重启一次，之后请求 Online 成功。
- **注册**：插入有效 SIM 后，modem 在 LTE 上报 `kRegisteredRoaming`，CS 和 PS 都附着上，信号格（3 到 4 格）传到 SpringBoard。界面仍是无服务：CommCenter 的基带服务开通状态停在 `kUnknown`，每次评估运营商名都记 `Operator name change aborted: not provisioned`，给界面的显示状态一直是 `kNotRegistered`。开通状态最早由 `set.cdma.qmi` 报 `kUnavailable`。测试卡仍只有紧急服务。

### 第六次上机（开通状态）

- 开通状态来自 ActivationRegister 指示：TLV 0x4f（一字节，`parseLegacyTlvs`：2 为 kProvisioned，3 为 kNotProvisioned），0x50、0x51（各卡槽，`parseMutableTlvs`：2 为 kCardProvisionedStateProvisioned）。指示带上 0x4f = 2、0x50 = 2 后，CommCenter 记 `Service provisioned changing from kUnknown to kProvisioned`。
- 但 `pp` 每次查询激活状态都判 `Baseband activation result is unexpected!`、`BB activated, but there is no SIM`，又把开通状态改回 kUnknown。判断看的是 0x56 `Slot1IsAccepted`（一字节，非零为真，即 SIM 锁的结论）；0x57 是卡槽二。现在两项都发 1。
- 数据口：所有 WDS 0xa2 和 QOS 0x2b 绑定都返回成功。
- 有效 SIM：开机后插拔一次 SIM，再开关飞行模式，状态栏才出运营商名和信号格；0x56、0x57 加上以后仍要插拔一次。“关于本机”的运营商是未知，CommCenter 记 `Missing carrier preferences path`。
- 开关飞行模式后 Wi-Fi 重新加入，驱动报的信号正常，SpringBoard 收到的却是 0 格，图标变空心。

### 第七次上机（开机即显示）

- 桥接层给每个通道最多排 64 帧等 ABM 读。开机时 53 个客户端的注册和指示一齐到达，每次开机都溢出，丢 5 到 22 帧。丢的是哪一帧决定症状：一次丢了 VOICE（服务 9）0x3 的两条应答，CommCenter 25 s 后记 `ATCS_TIMEOUT on message for svc=9` 并硬重启基带，这就是开机约 40 s 的那次重启；丢了 UIM 和 NAS 的帧，SIM 和注册状态停在旧值，插拔一次 SIM 才重新拉取。队列改为 1024 帧并记录每次丢帧后，开机没有丢帧、没有重启，状态栏直接显示运营商名和信号格。
- 运营商设置包：CommCenter 只查国家包和服务网络的运营商包，从不为 SIM 查运营商包，日志是 `No PersonalityID while CB changed`；没有运营商包就没有 APN，数据上下文记 `kDataNotSupported{context is not assigned}`，一次 Start Network 也不发。
- 激活指示每个卡槽的信息只在带了该槽的 ICCID、IMSI 一组 TLV（卡槽一从 0x42 起，卡槽二从 0x46 起）时才会填上；pp 判断有没有 SIM 看的就是这两项，现在一项都没有，所以每次都记 `BB activated, but there is no SIM`。
- 失效的卡在任何网络上都只得到紧急服务，拒绝原因 15（EMM #15）；同一张卡在 Debian 下也注册不上。

接下来的卡点：
- **蜂窝数据**：数据口设置已被接受；要看 `BindMuxDataPort` 的应答，之后才是 `pdp_ip` 接 `rmnet_ipa0`。蜂窝数据和数据漫游默认关闭，开数据通路前先确认。
- **eSIM**：在 UIM 0x2f 应答里补 TLV 0x21，VINYL 0xe5 的 Tape 请求转成 UIM 逻辑通道上对 ISD-R 的 APDU。
- **EFS 持久化**：modem 的写入只留在内核内存里，重启后从 FSG 重建。

### 第八次上机（ICCID、IMSI 和运营商设置包）

- k150 的激活指示带上 ICCID 后，`BB activated, but there is no SIM` 只在读到 ICCID 之前出现，CommCenter 第一次为这张 SIM 查运营商包，也在 iPad 的运营商包里匹配到了对应的包；但匹配 SIM 信息时记 `There is no CarrierID or SIM Info present`，技术类型是 kUnknown，`Ignoring bundle setup for unknown technology type`。
- 反汇编确认两条原因：匹配要用 IMSI，指示里只有 ICCID；`createActivationStringFromData` 把 ICCID 字节按十六进制原样转成字符串、去掉末尾的 F，要的是阅读顺序的 BCD，而 EF_ICCID 每两位是对调存的。
- k151：ICCID 对调回阅读顺序；CommCenter 开机自己读 EF_IMSI（9 字节），桥接层把应答接下来，解出 15 位数字，按阅读顺序打成 8 字节、末尾补 F，作为 TLV 0x43 放进激活指示。两个值都取自卡本身，日志里不打印。
- 上机：状态栏显示运营商名、信号格和 LTE 标志，控制中心的蜂窝数据开关打开。LTE 反复掉线，过一会儿又回到 LTE，原因在查。APN 设置和实际数据收发还没确认。

### 第九次上机（运营商配置文件推送）

- LTE 掉线来自一个循环：SIM 匹配到运营商包后，CommCenter（QMIBasebandSettingsDriver）把包里的 PRI（DER，9 段）和 GRI（XML，43 段）用 BSP 0xa000 推给基带，每段 1024 字节，TLV 0x01 是 {u16 文件类型，u16 段号，u16 总段数，数据}；再用 SetConfig 0xe00d 发 IMSI 切换和多 PDN 两项设置，然后重启基带让配置生效。桥接层回“不支持”，配置缓存不下来，每次上线都推送、重启，约 12 s 一轮。
- 每段的应答要带 TLV 0x40 ExpectedSegmentInfo，即下一段段号，否则记 `Next segment numbers are not matching`；最后一段之后要由基带发 FileTransferStatus 指示 0xa001（TLV 0x01 {u8 文件类型，u8 结果，0 为成功}，TLV 0x10 订阅号 1），否则等 22 s 判失败。桥接层照此收下文件后丢弃，跟 sahara 推来的镜像一样。
- 上机：推了两轮后 CommCenter 记 `Cached PRI file configuration is same as the one intended to be pushed, bailing out`，之后没有再重启，LTE 保持注册（只观察了约半分钟）。两轮的事务结果仍记 `kTransactionFailed`，原因没查。
- 软重启前那 12 s 是 ABM 发 BSP 0xe001 后等复位检测，跟 ABP 的 `bootstage` 属性无关；`bootstage` 只有 `SafeResetModemLegacy` 读，读不到时走最快的路径，发布一个在线值反而会轮询 reset 和 wake。
- BSP 0xb001（`queryActivationStatus`）的应答是 TLV 0x01 ChipInformation {u32 芯片 ID，u32 序列号}，CommCenter 放进激活附带信息（`kCTPostponementInfoBasebandChipID`、`...BasebandSerialNumber`），ABM 也发布这两项。桥接层回 BHI 和 sahara 阶段报过的同一组值（固定序列号，不是真实的）；上机后 `Unable to get chip id and serial number` 不再出现。eSIM 的 `Could not get cellular plan device Info` 仍在，跟这两项无关。
- 数据上下文仍然没有分配，CommCenter 记 `kDataNotSupported{context is not assigned}`。

### 第十次上机（eSIM 选卡）

开机时 eUICC 里的配置已启用，modem 却在实体卡上，CommCenter 的 sub.eval 在两个端点里找不到这张 eSIM 的 ICCID（“SIM … not found, leaving with no assignment”），不分配订阅，也就不会请求切卡。原因是卡信息列表里只有实体卡一项，而 CommCenter 按下标把列表对到端点上。另外，CommCenter 读这张列表时，Get Card Status 应答用 TLV 0x17，Card Status Change 指示用 TLV 0x14（指示里的 0x12、0x15 分别当作扩展卡状态和“卡短暂断电”）；这台 modem 两处都用 0x17，0x15 是 SIM busy。

桥接层现在在实体卡在用时给端点二补上 eUICC 一项（ICCID 取自卡槽状态，再加 EID），指示里把列表放到 0x14，并去掉 modem 的 0x12、0x14、0x15。CommCenter 选中 eSIM 后发苹果私有的 SelectUIMForSingleSubCfg（UIM 0x5557，TLV 0x01：0 槽一、1 槽二；另一种驱动发 MapProvisioningSessionToSlot 0x6e，TLV 0x10 为槽号），这台 modem 不认识，由桥接层执行：切槽、等 6 秒、重读卡槽、提交选卡、发布新卡状态，然后回应。实机上 eSIM 激活和两个方向的切换都成功，日志里每次请求都在新卡状态之后回应成功。切换耗时长，从 eSIM 切回实体卡时较长时间显示无 SIM，设置里的 EID 缺失，三者待查。

### eSIM 回切实体卡与切卡耗时

这台 modem 只有一个逻辑槽，访问 eUICC 就要把它切过去，每次切槽后按 Hermes 的 `kSimRefreshDelay` 等 6 秒（Hermes 的注释说，切槽后马上发 QMI 会出错）。所以切一次至少 6 秒，6 秒本身不缩短。上次上机慢，是因为切槽次数太多。从 eSIM 切回实体卡时，内核事件和 CommCenter 日志对得上的过程是：

- CommCenter 先 DisableProfile，再用 0x5557 选实体卡。桥切回实体卡、发布新卡状态，5 ms 后 CommCenter 再查 GetProfilesInfo（日志里的“Updating EID”）。这条查询的缓存已被 DisableProfile 清掉，桥只好又把槽切去 eUICC，实体卡随即消失。CommCenter 连续 12 秒报“SIM … not found”，退回去再 EnableProfile 一次。
- 那次 EnableProfile 在卡上成功（ES10 结果 0），之后关逻辑通道却失败了（QMI 错误 3）。原因是桥让 Enable/Disable 都带 refreshFlag，eUICC 做 REFRESH 时通道随之失效。桥把整条命令判成失败，CommCenter 又补发一次 DisableProfile。
- 之后每次取回和删除通知都要再切一次。批处理的空闲保持只有 3 秒，而取回通知、上传服务器、删除通知之间各隔约 4 秒，于是每条通知都要切走再切回。

改动：

- 关逻辑通道只算收尾。关闭失败或超时都不再改写已经由卡给出的结果；Hermes 也从不主动关 ISD-R 通道，下次重新打开即可。
- 改变卡片的命令只把缓存的查询标成过期，不再直接清掉。马上要从 eUICC 切去另一张卡时，先在仍然打开的通道上把过期查询重读一遍，再关通道、切槽。之后 CommCenter 的查询直接从缓存回答，不必再切走。重读失败就丢掉该条目，退回原来的行为。待发的应答先暂存，重读完再放回。
- 批处理空闲保持改为 12 秒（上限 30 秒）。放开 eUICC 再拿回来要付两次 6 秒，空闲短于 12 秒时不放手反而更省。

上机：在实体卡和 eSIM 之间来回切了 6 次，每次桥都切到选中的物理槽并发布新卡状态；从 eSIM 切回实体卡时只短暂显示无 SIM。

EID：MobileEquipmentInfo 的 CSN 取哪个槽，由 `capabilities::ct::defaultMobileEquipmentInfoCSNSlotID()` 按 radio 查表决定。radio 3（J121）对应槽二，正是 eUICC 所在的端点，所以槽号没选错。设置页确实拿到了 MobileEquipmentInfo，但内容在日志里是私有数据。`--baseband` 给 `com.apple.cellularsettings` 打开私有日志后，上机看到设置的 SubscriberCache 收到的 `CTMobileEquipmentInfo` 带着 CSN（eSIM 为当前卡时，挂在槽一）。“关于本机”的 `-[PSGAboutDataSource loadSpecifiers]` 先问 `PSUICellularPlanManagerCache isActivationCodeFlowSupported`，为 NO 时整行 EID 都不加；为 YES 时取订阅 0、1 的 MobileEquipmentInfo 中第一个非空 CSN，所以槽位不是问题。设置记录的正是 `isActivationCodeFlowSupported: NO`，CommCenter 同时报“No local cellular plan signup support, mark as not supported”。这条判断在原版 CommCenter 的本地办理检查里，兼容副本（`tools/coachz_cellular_compat.py --esim`）正是把它改成放行的。这次跑的却是原版：内核日志是 `firmware files -1 ... error 2`，baseband.arc 没有读到，兼容副本没有装上。固件包头里加进 venus.mbn 以后有 9 个文件，baseband.arc 排第 9，而内核遍历包头的两处循环都只看前 8 项。现在两处都遍历包头能放下的全部 63 项，`tools/linux_log.py` 超过 63 项时报错。Data 上已有的 Mav16 文件是更早的启动写的，所以基带照常工作。上机：内核读到全部 18 个固件文件，兼容副本挂上（`cellular compatibility ready 1`），CommCenter 报的 eSIM 流程从 0x0 变为 0x22，不再报“No local cellular plan signup support”；“关于本机”的 EID 还没有在屏幕上核对。

### eSIM Tape 实现（待实机）

补齐的 `coachz_bb_esim.h` 已接入现有 QMUX 收发，不改数据口和激活实现，不增编译参数：

- 独立 UIM 客户端用 Get Slot Status 识别物理 eUICC、当前映射及真实 EID；初次 Get Card Status 在发现期间排队，成功应答才补能力 TLV 0x21。未发现 eUICC 不宣称支持。
- VINYL 0x1002 收集 1500-byte Tape 分段，按 command、transaction、slot 保持归属；逐段应答只是接收确认，执行结果由 0x1003 指示返回。0x1004 返回实际 EID，0x1005 返回桥接声明的 radio releases。
- ISD-R 用 UIM Open Logical Channel，STORE DATA 用短 APDU 分片、P1 0x11/0x91 和分片序号，响应 61xx 续读；Bound Profile Package 按其 DER 结构拆分。每个命令有三分钟整体上限，每次 QMI 等待十秒，错误和 SW 状态返回失败，不伪造卡片成功。
- 暂时切到 eUICC 后等六秒、重新读卡槽再开通道；结束关闭逻辑通道，恢复原物理映射、等六秒并重新读取确认。普通 UIM 请求在此期间排队，临时 Card Status 指示不传给 CommCenter。恢复未确认时保留错误状态，不继续把原 SIM 请求发给另一张卡。
- 删除草稿中的每次启动十六次切槽总限制；标准 eSIM 交互可包含多条命令，该限制会截断流程。卡状态轮询只查询，不触发切槽。
- 日志只含阶段、命令号、长度、QMI 错误和 SW，不含 EID、ICCID、APDU 或 profile 内容。事件编号与字段见头文件 `cbe_event_kind`。

现有主机模型已覆盖卡槽发现和能力 TLV 替换、两段 Tape 合并、255-byte APDU 分片、61xx 续读、多段结果、BPP 结构分片、超时后的关闭/恢复、迟到应答及恢复失败；C/C++ 头文件检查与模型的 ASan/UBSan 检查通过。IOKit/QMUX 的实际并发、真实 eUICC 和 RSP 服务不在这个模型内。没有编译内核、写盘或实机验收。菜单是否出现、完整 RSP 会话中的逻辑通道生命周期、真实分片、profile 下载及启停、恢复后的 SIM 行为都需要实机确认，不能将上述源码覆盖当作完整 eSIM 验收。

下次只需正常构建内核，现有 baseband 路径会复制这些源码；无需重编 GPU addon或改变镜像参数。首先看 eSIM 事件的卡槽/eUICC/EID 长度与两个能力字节，然后看 CommCenter 的 VINYL 能力和实际 Tape 请求。保留首条失败前后的阶段日志；原始标识与 profile 材料留在本地。

参考：

- [Hermes modem_qrtr.cc](https://chromium.googlesource.com/chromiumos/platform2/+/main/hermes/modem_qrtr.cc)：切槽、ISD-R 通道、APDU 和恢复流程；[modem.h](https://chromium.googlesource.com/chromiumos/platform2/+/main/hermes/modem.h) 的 `kSimRefreshDelay` 为六秒。
- [Hermes apdu.cc](https://chromium.googlesource.com/chromiumos/platform2/+/main/hermes/apdu.cc)、[uim_cmd.qmi](https://chromium.googlesource.com/chromiumos/platform2/+/main/hermes/uim_cmd.qmi)、[sgp_22.h](https://chromium.googlesource.com/chromiumos/platform2/+/main/hermes/sgp_22.h)：短 APDU 分片、UIM wire schema 和 ISD-R AID。
- [libqmi UIM schema](https://github.com/linux-mobile-broadband/libqmi/blob/main/data/qmi-service-uim.json)：Logical Channel 0x003f 的 Slot 和 Channel ID TLV，用于关闭已打开的通道。
- VINYL 和能力字段沿用已核对的 `EURSimCommandDriver`、`VinylQMICommandDriver`、TLV writer/parser 结论；此处仅实现桥接，不复制原始二进制或设备标识。


## 原生 Internet 数据验收

CommCenter 的 Internet 上下文通过显式启用的固定版本兼容副本发起真实 WDS 激活，modem 返回 IPv4 地址和 DNS，`pdp_ip0` 经 IPA 收发。Safari 经蜂窝路径完成 DNS 解析和 HTTP 页面加载，PDP 重建后收发继续增长。准备和打包方式见 [基带桥接说明](../../platform/coachz-prelink/pending/baseband/README.md#原生-internet-数据通路)。

重复切槽来自只读查询缓存的遗漏。`GetProfilesInfo (BF2D)` 与 `ListNotification (BF28)` 交替执行时，原缓存把 BF28 当作可能改变卡片的命令并清空资料查询缓存，触发下一轮切槽。BF28 现已加入只读缓存，六种查询各有条目，保留每项 16 KiB；改变卡片或通知状态的命令仍清空缓存。交替查询的便携回归和实机缓存命中均已确认。ListNotification 的列表语义见 SGP.22 第 5.7.9 节，删除通知是第 5.7.11 节的独立操作。[GSMA SGP.22 v2.3](https://www.gsma.com/esim/wp-content/uploads/2021/07/SGP.22-v2.3.pdf)

PDP 重建后的持续加载来自 XNU 协议挂接入口缺失。内核记录 `in_domifattach: proto_plumb returned 6 if=pdp_ip0`，DNS 记录 `Device not configured`，发送请求计数停止增长，DMA 没有积压。原驱动只在开机直接挂接协议，CommCenter 清理旧上下文后无法重新挂回。现按同一 XNU 的 `utun_attach_proto` 约定注册蜂窝接口的 IPv4／IPv6 plumbers，复用初始挂接回调，保留 `EEXIST` 语义。

修复后的实机记录中，plumber 注册成功，`ENXIO` 消失，发送请求及 TX／RX 继续增长。统一日志有 112 条经 `pdp_ip0` 的有效 DNS 回复，WebKit 有蜂窝路径和 HTTP 200 响应。该结果覆盖 IPv4 网页加载及初始化中的 PDP 重建，IPv6 数据连接、APN 编辑入口和长时间连接继续验收。

## Operating mode after a baseband boot

ABM power-cycles the baseband at each start and after each reset, while the bridge stands for that boot and the modem keeps running,
so the modem keeps whatever operating mode it had. CommCenter expects a freshly booted baseband in low power ("Baseband booted in
airplane mode"); its subscription switch waits for the modem's low-power report, and without one it stayed in airplane mode with no
service. After each such boot the bridge's own DMS client reads the mode (QMI DMS 0x002d) and sets low power (0x002e, TLV 0x01 = 1),
as libqmi lays them out. On hardware both requests answer with error 0, and CommCenter then takes the modem online itself.
