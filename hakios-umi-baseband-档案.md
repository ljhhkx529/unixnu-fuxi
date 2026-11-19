# hakios-xnu-kernel-umi-fixes 基带代码档案

对象：`xm6661885/hakios-xnu-kernel-umi-fixes`（本地副本 `hakios-xnu-kernel-umi-fixes-main.zip`）
基带范围：Mi 10（SM8250 / Kona）上的**外置 SDX55（X55）基带**，走 PCIe2 + MHI。
撰写日期：2026-11-19。全部结论来自源码与仓库内文档阅读，未在设备上运行任何测试。

---

## 0. 一句话概括

这套基带代码做的是一件很特殊的事：**把 iOS 自己的 CommCenter / ABM / BBUpdater 原封不动搬到一台 Android 手机上，让它们去驱动一台 Qualcomm 外置基带**。

难点不在驱动基带（那部分是标准 MHI/PCIe/QMI），而在**协议适配**：

- Apple 的 `AppleBaseband` IOKit 类族在真机上对应 Mav17 基带的特定私有语义，X55 上没有；
- Apple 的 QMUX 通道名（`pbl`/`sahara`/`qmux1`/`qmux2`…）与 X55 的 MHI 通道号完全不是一回事；
- Apple 私有 QMI 服务（`0xe1..0xea`）X55 一个都没有；
- BBUpdater 要求基带跑一遍 Sahara 握手，但 X55 自己带固件。

所以整个基带栈是**三层**，中间那层专门做"翻译 + 作假"：

```
iOS 用户态        CommCenter + ABM(basebandd) + BBUpdater + libPCITransport/libIOABP/libQMIParser
     │  IOKit user-client，类名必须是 Apple 原名
     ▼
IOKit 桥接层      hakios.umi_baseband.cpp（1865 行）+ coachz_bb_*.h（6 个纯逻辑头文件）
     │  umi_baseband.h 里的 C API
     ▼
kernel 传输层     umi_modem.c + umi_modem_{mhi,pcie,dma,data,net,power,selftest}.inc
     │  MHI / QRTR / TFTP / EFS / QMAP
     ▼
硬件               PCIe2 上的 SDX55 (X55)
```

---

## 1. 文件地图

### 1.1 kernel 传输层

| 文件 | 行数 | 职责 |
|---|---|---|
| `devices/xiaomi-mi-10/overlays/xnu/files/bsd/dev/umi_modem.c` | 356 | 主状态机：boot-arg 解析、线程、bring-up、故障重启 |
| `.../bsd/dev/umi_modem_mhi.inc` | 1314 | MHI 上下文/事件环、Sahara、QRTR、TFTP、EFS |
| `.../bsd/dev/umi_modem_pcie.inc` | 202 | PCIe2 枚举、GPIO、时钟、PHY |
| `.../bsd/dev/umi_modem_dma.inc` | 233 | SMMU 私有上下文、DMA 池、MSI 别名 |
| `.../bsd/dev/umi_modem_data.inc` | 126 | QMAP 数据面泵 |
| `.../bsd/dev/umi_modem_net.inc` | 132 | kernel 自有 `pdp_ip0` ifnet（仅 `umi_modem=1`） |
| `.../bsd/dev/umi_modem_selftest.inc` | 137 | 开机一次 ICMP/DNS 自检（不占 socket、不装路由） |
| `.../bsd/dev/umi_modem_power.inc` | 27 | RPMh 投票（被 `umi_rpmh.c` include） |
| `.../bsd/dev/umi_modem_qmi.h` | 223 | 无传输依赖的 QMI/QMAP 编解码（kernel 与 host 测试共用） |
| `.../bsd/dev/umi_modem_tftp.h` | 93 | TFTP 服务端 |
| `.../bsd/dev/umi_modem_phy.h` | 137 | PHY 寄存器/数值事实表（QMP v4 Gen3x2） |
| `.../bsd/dev/umi_baseband.h` | 56 | **两层之间唯一的接口面** |

### 1.2 IOKit 桥接层

| 文件 | 行数 | 职责 |
|---|---|---|
| `devices/xiaomi-mi-10/drivers/umi/hakios.umi_baseband.cpp` | 1865 | AppleBaseband 类族 + 桥接逻辑 |
| `.../bsd/dev/coachz_bb_qmux.h` | 20643 B | QMUX 帧 ↔ QMI/QRTR 桥、CTL 服务 |
| `.../bsd/dev/coachz_bb_sahara.h` | 12960 B | Sahara 设备端（对 BBUpdater 作假） |
| `.../bsd/dev/coachz_bb_data.h` | 22501 B | Apple 私有 QMI 应答（激活/carrier/chip info） |
| `.../bsd/dev/coachz_bb_pdp.h` | 5670 B | WDS 观察 + `pdp_ip0..3` |
| `.../bsd/dev/coachz_bb_channels.h` | 3587 B | Apple 通道名表 |
| `.../bsd/dev/coachz_bb_control.h` | 5389 B | 板级方法（selector）表 |
| `.../bsd/dev/umi_bb_pending.h` | 146 | 未决请求表（每个请求都必须有答复） |

### 1.3 周边（同一基带栈但独立关注点）

| 文件 | 行数 | 职责 |
|---|---|---|
| `.../bsd/dev/umi_ims.inc` | 2138 | IMS/VoLTE 胶水层 |
| `.../bsd/dev/umi_cellinfo.inc` | 197 | 驻留小区信息 |
| `.../bsd/dev/umi_gnss.inc` | 162 | GNSS（专用 QRTR 端点，不进 ABM 客户端池） |
| `.../bsd/dev/umi_voice*.inc` | 多个 | 语音 / IMSA 音频 |
| `xnu/files/bsd/sys/hakios.qmi.h` + `drivers/modem/hakios.qmi.c` | 221 + ~800 | **Wi-Fi 用**的通用 QMI 助手，非基带专用 |

---

## 2. 两种运行模式

`umi_modem` boot-arg 决定走哪条路（`umi_modem.c:339-355`）：

| boot-arg | 模式 | kernel 做什么 | CommCenter |
|---|---|---|---|
| `umi_modem=1` | kernel 自拨号 | 完整 QMI 客户端栈（UIM/DMS/NAS/WDA/WDS），自建 `pdp_ip0`，APN 硬编码 `cmnet` | 不加载 |
| `umi_modem=2` | 桥接 | 只有传输：boot、QRTR NS、servreg、TFTP、EFS、IP_HW0/QMAP 泵。**不主动问基带任何事** | 加载，拨号/地址/APN 全归它 |
| `umi_modem_probe=1` | 探测 | rails/GPIO/时钟/PHY/link/枚举/BHI EE 读，**bus mastering 保持关**，不需要固件 | 不加载 |

`umi_modem` 与 `umi_modem_probe` 互斥，同时给会报 `EBUSY`。

模式差异在代码里统一由 `md_bridge` 布尔量分叉。

`umi_modem=2` 时 loader（`loader/loader.c:5414`）会合成一个 DT 节点：

```
/baseband   device_type=baseband, compatible=baseband,n41, class=3, config=<10×u32>
/product/baseband-chipset = "mav17"
```

launchd 的 CommCenter 作业带 `_LimitLoadToDeviceTree=baseband`，所以这个节点是 cellular CommCenter 被加载的**唯一开关**。`/baseband` 节点还必须带 `config` 属性（至少 24 字节），否则 `libTelephonyBaseband` 的 `_TelephonyBasebandControllerLoadConfigOnce` 会报 "no class property"。

---

## 3. kernel 传输层

### 3.1 硬件绑定方式（移植要点）

**这一层完全不做 DT 匹配，也不读 DT 属性。** 所有硬件地址是编译期常量（`umi_modem.c:47-57`）：

```
MD_GCC  0x00100000   MD_GPIO 0x0f500000   MD_PARF 0x01c10000   MD_PHY 0x01c16000
MD_DBI  0x64000000   MD_ATU  0x64001000   MD_CONF 0x64100000   MD_BAR  0x64300000
MD_SMMU 0x15000000   MD_IOVA 0xb0000000   MD_DMA_SIZE 16 MiB
```

`md_reg()` 是白名单区间表，越界返回 0，读写静默丢弃。映射用 `ml_io_map()`，访问带 `dmb oshld`/`dsb oshst` 屏障。

**换板子必须逐个核对这组地址、TLMM pin 号、PCIe controller/PHY 位置**，不能靠 DT 描述符改。

关键 GPIO（SOUTH tile，物理 `0x0f500000 + pin*0x1000`）：

| Pin | 方向 | 含义 |
|---|---|---|
| 56 | 输出高 | AP2MDM_STATUS |
| 57 | 输出低 | AP2MDM_ERRFATAL |
| 3 | 输入 | MDM2AP_STATUS |
| 1 | 输入 | MDM2AP_ERRFATAL |
| 85 | 输出低 | PERST#（2 mA / 下拉） |
| 86 | — | CLKREQ#（`pci_e2` mux1，2 mA / 上拉） |
| 87 | 输入 | WAKE#（2 mA / 上拉） |

PCI ID 硬编码校验 `0x030617cb`（vendor 17cb / device 0306），不匹配直接 `ENODEV`（`umi_modem_pcie.inc:184`）。

### 3.2 线程模型

**只有一个线程**：`md_thread`（`umi_modem.c:285`）。头注释是硬约束：

> Every function here runs on the modem's service thread unless it says otherwise: that thread alone owns MHI and sends QRTR.

上层用户线程只做「排队 + 唤醒」，不碰硬件。桥接层每轮通过 `umi_bb_poll(send, release)` 把攒下的 QMI 交给 kernel 线程发。

唯一跨线程唤醒点是 `umi_modem_wake()` → `md_idle()` 里的 `msleep(&md_wake_channel, ...)`（`umi_modem.c:138-142`）。节流策略：有流量或 2 秒内有活动时 1 ms 一轮，安静后 100 ms 一轮。

> 这个 100 ms 就是 ABM 的 QMI 延迟上界。`COMMCENTER-umi.md:263` 明确指出并要求改成可唤醒等待。

### 3.3 固件加载完整时序

```
md_thread
 └ md_bringup(probe)                          umi_modem.c:262
    ├ md_pcie()                                umi_modem_pcie.inc:49
    │   GPIO → rails → clocks → PHY → link → 枚举
    ├ 读 BHI：EE 值判定（0=PBL 1=SBL 2=AMSS 3=RDDM 4=WFW 6=EDL）
    └ md_mhi_boot()                            umi_modem_mhi.inc:1124
       ├ md_image_map()                        解析 /fw/sdx55/sahara.map
       ├ md_dma_install()                      SMMU SID 0x1d01 + 16 MiB DMA 池
       ├ 读 sbl1.mbn → 4 MiB buffer
       ├ md_enable_dma()                       MSI 别名映射，bus mastering 开
       ├ BHI 直送 SBL
       ├ md_contexts()                         READY→M0，上下文 + 4 个软件事件环
       ├ md_sahara()                           Sahara 协议，推 17 个镜像
       ├ 等 EE==2 (AMSS)
       ├ 等 M0 + EE change event
       ├ md_enable_hw_events()                 ER4/ER5 硬件事件环
       ├ 开 IPCR 20/21 (ER0)、EFS 10/11 (ER1)
       ├ QRTR HELLO + NEW_LOOKUP
       └ 等 QRTR discovery (NAS/UIM)
```

**SBL 走 BHI 寄存器直送**，不是 BHIE、不是 Firehose、不是 EDL：

```c
md_write(MD_BAR+md_bhi+0x20, 0);
md_address(MD_BAR+md_bhi+8, sbl.a);       // 地址
md_write(MD_BAR+md_bhi+0x10, size);       // 长度
md_publish();
md_write(MD_BAR+md_bhi+0x18, 1);          // 开始传输
md_wait(MD_BAR+md_bhi+0x2c, 0xc0000000, 0x80000000, 20000000);
```

SBL buffer 是**一次性 4 MiB 静态分配**，永不回收（`md_link_reset` 不重置池）。

#### Sahara 协议

MHI 通道 2/3（ER1）。Sahara 是字节流，不与 MHI completion 对齐，所以有独立的 32 KiB 流缓冲和 `md_sahara_pos/size`。

| cmd | 含义 | 处理 |
|---|---|---|
| 1 | HELLO | 长度必须 48，回显 image transfer pending/complete |
| 3 | READ_DATA (32 位 id/offset/length) | `md_sahara_read()` |
| 0x12 | READ_DATA (64 位) | 同上，长度必须 32 |
| 4 | image complete | 长度必须 16，status 非 0 → EIO，成功回 DONE |
| 6 | done | 长度必须 12，mode==1 才算完成 |
| 其他 | memory-debug / RDDM / 所有写模式 | **一律 ENOTSUP** |

循环上限 16384 次请求，超出 `ELOOP`。

**关键约束**（`umi_modem_mhi.inc:298-301`）：

> Removal of SBL is not proof of success. The caller must verify live AMSS and open mission IPCR.

所以 Sahara 传输结束（哪怕 `EAGAIN`/`ETIMEDOUT`）不构成成功。只有当错误是 `EAGAIN`/`ETIMEDOUT` **且** EE 已变成 2 时，才当作「传输终止但 AMSS 已起」继续；其他错误一律致命。

#### 固件镜像清单

内置 17 条（`umi_modem_mhi.inc:552-557`）：

| Sahara ID | 文件 |
|---:|---|
| 6 | apps.mbn |
| 8 | qdsp6sw.mbn |
| 16 / 17 / 20 | mdm1m9kefs1/2/3.img |
| 21 | sbl1.mbn |
| 23 | aop.mbn |
| 25 | tz.mbn |
| 29 | acdb.mbn |
| 33 | hyp.mbn |
| 34 | mdmddr.img |
| 36 / 37 | multi_image_qti.mbn / multi_image.mbn |
| 38 | xbl_cfg.elf |
| 40 | apdp.mbn |
| 41 | devcfg.mbn |
| 42 | sec.elf |

每个镜像字节有三个来源，优先级由 `md_image_prepare()` 决定：

1. payload：`/fw/sdx55/<name>`（VFS vnode 读）
2. UFS 分区：ID 16/17/20/34 → `mdm1m9kefs1/2/3`、`mdmddr`
3. modem FAT 分区：`image/sdx55m/<name>`

分块固定 32768 字节，每 8 MiB 打一次进度+速率。

#### 超时汇总

| 阶段 | 超时 |
|---|---|
| root mount 就绪 | 300 ms × 循环 |
| MHI READY / MHI 状态 | 20 s / 20 s |
| BHI 接受 SBL | 20 s |
| M0 上下文就绪 | 20 s |
| START_CHAN 确认 | 5 s |
| 单次 TX 完成 | 5 s |
| Sahara 单次收包 | 20 s |
| AMSS 出现 | 20 s |
| AMSS 后 M0/EE 事件同步 | 20 s |
| QRTR discovery | 120 s |
| **整个 bring-up 总预算** | **360 s** |

### 3.4 MHI 传输层

**上下文布局**

- 通道上下文：128 × 44 字节（11 个 u32）
- 事件环上下文：6 × 44 字节
- 命令环：44 字节上下文 + 2048 字节环

通道上下文字段：`c[0]`=chcfg（软件通道 `0x201`，硬件通道 id≥100 用 `0x301`）、`c[1]`=方向（偶数通道=1 host→device，奇数=2 device→host）、`c[2]`=事件环号、`c[3-4]`=环基址、`c[5-6]`=环长度、`c[7-8]`=生产者、`c[9-10]`=消费者。

**事件环大小**（`umi_modem_mhi.inc:44`）：

```c
static const unsigned md_event_counts[MD_NER] = {64, 128, 64, 64, 1024, 2048};
```

ER0=64、ER1=128、ER2=64、ER3=64、ER4=1024、ER5=2048 项，每项 16 字节。

> ⚠️ `MODEM-MAINLINE.md:108` 的表格写 IP_HW0 ER4/ER5 各 128 项，与代码的 1024/2048 不一致。**以代码为准**。

对齐是硬要求：MHI 要求环基址对齐到**整个环长度**，页对齐不够（ER4 是 16 KiB，ER5 是 32 KiB）。`md_alloc_aligned(size, alignment)` 用 `alignment=ring_bytes` 分配。

通道环固定 128 项 × 16 B = 2048 B（`MD_TRES=128`），单包缓冲 `MD_PACKET=32768`。

**通道 → 事件环映射**（`umi_modem_mhi.inc:340-345`）：

```c
static const unsigned channels[] = {2,3,4,5,10,11,14,15,20,21,100,101};
c[0] = (ch>=100 ? 3u<<8 : 2u<<8);
c[1] = (ch&1) ? 2 : 1;
c[2] = ch>=100 ? ch-96 : (ch<12 ? 1 : 0);
```

| 通道 | ER | 用途 | 实际开启 |
|---|---|---|---|
| 2 / 3 | 1 | Sahara | 是 |
| 4 / 5 | 1 | DIAG | **否**，只配置不用 |
| 10 / 11 | 1 | mission EFS | 是 |
| 14 / 15 | 0 | QMI WWAN | **否**，Linux 成功配方不用它 |
| 20 / 21 | 0 | IPCR / QRTR 控制 | 是 |
| 100 / 101 | 4 / 5 | IP_HW0 数据 | 是 |

**门铃**

| 用途 | 地址 |
|---|---|
| 通道门铃 | `BAR + md_chdb + id*8`（`md_chdb` 从 `BAR+0x18` 读） |
| 事件环门铃 | `BAR + md_erdb + e*8`（`md_erdb` 从 `BAR+0x20` 读） |
| 命令门铃 | `BAR + 0x70` |

**db_mode 机制**（`umi_modem_mhi.inc:28-33`）：硬件通道（id≥100）默认 `db_mode=true`，第一次敲门铃后置 false——**IP_HW0 默认用普通门铃而非硬件 burst doorbell**。只有固件回报 OOB/DB_MODE 事件时才切 burst：

- 事件 type `0x20` cc==2（doorbell mode switch）→ 对通道 6/7 恢复 burst 并重发未完成 TRE
- 事件 type `0x22` 且 ch≥100 且 cc==5 或 6 → 置 `db_mode=true` 并重发

**事件解析**（`md_poll()`，`umi_modem_mhi.inc:114-176`）

- 读 producer（ctx+7），用 `md_ringptr()` 校验（必须在环内且 16 B 对齐），否则 `EPROTO`
- 事件类型 `event[3]>>16 & 255`，通道 `event[3]>>24`，completion code `event[2]>>24`，字节数 `event[2]&65535`
- type `0x40` 且在 ER0 → EE change 事件
- type `0x21` 且 pointer==`md_command_wait` → 命令完成
- type `0x22` → transfer 完成：偶数通道清 `tx_pending` 置 `tx_done`；奇数通道置 `rx_ready`
- **转移事件的 pointer 必须等于最后一个 TRE 的地址**，否则 `EPROTO`；RX 长度超 `MD_PACKET` 直接 `EPROTO`

**IRQ：完全不用。** MSI 配好但**所有向量保持 masked**，只做轮询：

```
UMI MODEM: protected DMA enabled; MSI alias=0x17a10040 INTID=864 masked polling
```

GICv3 MBI alias `0x17a10000`，SETSPI `0x17a10040`，INTID 864。

### 3.5 DMA 与 SMMU

**私有 DMA 池**：16 MiB，`kmem_alloc_contig` + `KMA_ZERO`，用 `ml_io_map_unmappable()` 映射——**不可映射**，防 CPU 侧误访问设备 DMA 目标。IOVA 基址 `0xb0000000`。

`md_used` **永不回卷**（`md_link_reset` 不重置）：voice mailbox 把 AP 拥有的页映射进池子，重卷会破坏映射。

缓存维护用 `dc civac` + `dsb sy`。

**SMMU 配置**（`md_dma_install()`，`umi_modem_dma.inc:74-190`）

- 全程持 `hakios_smmu_lock`（与 Wi-Fi、camera 共享）
- 扫描所有 SMR，**若 SID 0x1d01 已被占用直接 EBUSY 退出**
- 先写 S2CR 指向一个 faulting context 占位（SMR bit31 置位 + `0x80001d01`），**即使后续失败也保留所有权**
- 装 context 后**校验回读**：SMR==0x80001d01、S2CR==cb、SCTLR&1、CBAR/CBA2R、TTBR0 高低位全对，否则 EIO 并回滚

端点 SID = **0x1d01**，root SID 基址 0x1d00。**不得与 Wi-Fi 的 SID 0x1c01 混用。**

**bus mastering 开启顺序是硬约束**（`umi_modem_dma.inc:225-231`）：

1. 先清 `PARF+0x2c00` bypass
2. 写 `DBI+4=6`（command 寄存器，值从 2 变 6，值不对直接 EIO）
3. 最后才 `CONF+4` 置 bit2

**恢复路径** `md_dma_recover()` 的顺序（`umi_modem_dma.inc:39-73`）：

> acknowledge the context and global faults (both write-1-to-clear), drop the dead link's translations, re-assert SMR/S2CR, and only then let translation and finally bus mastering come back.

残留的 translation fault 会让新链路第一个传输就再次挂掉，所以必须再清一次 FSR。

**MSI / 端点路由**（`md_enable_dma()`）：清 DBI-to-SID bypass 表（`PARF+0x2000+4*i`），然后 CRC8/0x07 over BDF `0x0100` 得 hash，写 `PARF+0x2000+4*hash = 0x01000100`（BDF 0x0100 + SID delta 1）。

### 3.6 数据面（QMAP）

**链路建立**（`md_data_link()`，`umi_modem_data.inc:69-92`）：

1. `md_enable_hw_events()`（ER4/ER5）
2. 开通道 100（ER4, slot 6）、101（ER5, slot 7）
3. WDA Set Data Format，TLV `0x11`=2（raw IP）、`0x12`/`0x13`=5（QMAP）、`0x15`=32、`0x16`=32768、`0x17`=endpoint `{3,0,0,0,4,0,0,0}`
4. **回读校验**：0x11 必须是 2、0x12/0x13 必须是 5，否则 `EPROTONOSUPPORT`；0x16 是下行聚合上限，必须 ≤32768

注释说明为什么这么严（`umi_modem_data.inc:79-80`）：

> WDA can succeed while selecting a different format. Accept only the framing we implement, never silently interpret raw IP/QMAPv5 as v1.

**帧格式**

上行：`{0x00, mux, len_hi, len_lo}` + 裸 IP 包（`umi_modem_data.inc:43-45`）
下行：`pad = frame[p] & 0x3f`，`n = frame[p+2]<<8 | frame[p+3]`，payload 在 `p+4`，长度 `n - pad`（`umi_modem_qmi.h:178-186`）

**下行严格校验**（`umi_modem_data.inc:25-28`）：

```c
if ((rx->buffer.p[start]&0xc0) || mux!=1 || n<20 || (ip[0]>>4)!=4 ||
    (ip[0]&15)<5 || (size_t)(ip[0]&15)*4>n || ((size_t)ip[2]<<8|ip[3])!=n)
    { md_data_drop++; continue; }
```

`&0xc0` 是 QMAP 的 command bit 和 next-header bit——那不是 IP 包。

桥接模式下走不同分支：所有 mux、所有 IP 版本都转给 `umi_bb_data_input`，由桥接层决定归属哪个 `pdp_ip`。

**kernel 自有拨号**（仅 `umi_modem=1`）：WDS bind（endpoint pcie/3 iface 4, mux 1）→ set IP family ipv4 → start network（**APN 硬编码 `cmnet`**）→ get runtime settings → 校验地址/网关/掩码/MTU → `md_net_attach`。掩码连续性校验：`inverse = ~mask; if (inverse & (inverse+1)) return EPROTO;`

### 3.7 故障模型与重启

**所有故障收敛到 `md_fault()` 一个检查点**（`umi_modem_mhi.inc:96-113`）：

- `md_now_us() > md_deadline` → `ETIMEDOUT`
- MHI status（`BAR+0x48`）== 0xffffffff，或 bit2 置位，或 EE==3/EE==6/EE>7，或 SMMU context FSR & 0xc00001fe → `EIO`

**主循环**（`umi_modem.c:299-332`）：

```
for (round = 0; ; round++) {
    if (round) md_link_reset();
    md_deadline = now + 360 s;
    error = md_bringup(probe);
    if (!error && !probe) { error = md_service(); md_net_down(); }
    if (error) { md_stop(true); if (bridge) umi_bb_modem_lost(); }
    if (!md_restart_allowed()) break;
    md_restart_log(n, "begin: PERST cycle, PCIe retrain, MHI/Sahara, QRTR discovery");
    md_restart_pause(n);
}
```

**重启策略**

| 项 | 值 |
|---|---|
| 开关 | sysctl `kern.hakios.umi_mdm_restart`，**默认 0（关）** |
| 每 boot 上限 | 3（`MD_RESTARTS_MAX`） |
| 最小间隔 | 5000 ms |
| 退避 | `5000 << (n-1)`，封顶 30000 ms |

默认为 0 有历史原因（`umi_modem.c:66`）：

> v29w: off by default -- the v29v restart hung the phone (16:06:48)

**状态三态**（`umi_modem_state()`，`umi_modem.c:211-215`）：0 = 没有 service 线程；1 = 链路活着；2 = 线程活着但链路不在（bring-up 中、故障中、或两次重启之间）。

`umi_baseband.h:40-42` 的警告：

> The bridge must never report a reset as harmless while this is not 1.

---

## 4. IOKit 桥接层

### 4.1 AppleBaseband 类族

| IOKit 类 | user-client | 作用 |
|---|---|---|
| `AppleBaseband` | `AppleBasebandUserClient` | 板级方法：复位、电源、mux、VBUS、manager/baseband state、property 读写 |
| `AppleBasebandPCIControl` | `AppleBasebandPCIControlUserClient` | open/close/readLogs/loggerTune |
| `AppleBasebandPCIInterface` | `AppleBasebandPCIUserClient` | 每通道一个实例，13 个 selector |
| `AppleBasebandPCIPDPManagerBase` → `…PCIPDPManager` → `…PCIMAVPDPManager` | — | 三级继承只为匹配 `libPCITransport` 的基类匹配 |
| `AppleIPAppender` | `AppleIPAppenderUserClient` | 数据面内核服务；selector 16 = Block PDP traffic |

**类名必须是 Apple 原名**，因为 CommCenter 的沙箱白名单按 user-client 类名放行（`COMMCENTER-umi.md:97`）。这是硬约束，不是命名偏好。

真机 bundle 有 `AppleBasebandPCIMAVControl`，本实现没有这个类——这是一个已知偏离。

**`ABPInterfaceProtocol` 属性**：光 `setProperty` 不够，IOKit 不比较自己不知道的 key，必须覆写 `matchPropertyTable` 走 `matchProtocol`，否则每个 client 都会拿到第一个通道。

### 4.2 通道映射（最容易搞错的地方）

Apple 的 `libPCITransport` 通道表在 `libPCITransport.dylib` @ `0x20c64c430`，与 `coachz_bb_channels.h` 逐字节一致：

```
0 pbl   1 sahara   2 diag   3 refs   4 qmux1   5 qmux2   6 rom   7 boot
8 ari   9 trace   10 at    11 dpl   12 mediactl   13 vcom   14 gnss   15 qdss
17 virtual_timesync   20 boot_logger
```

全部 18 个都发布，但**只有 pbl、sahara、qmux1、qmux2 背后有东西**，其余"能开、能记账、永远读不到东西"——目的是让日志显示 ABM 到底用了哪些。

**Apple 的 `qmux1`/`qmux2` 不是 MHI 通道 4/5。** 它们是 ABM 通过 `AppleBasebandPCIInterface` 写入的**逻辑通道**，桥内部再映射到 X55 的 QRTR 传输。MHI 4/5 是 DIAG，与 QMUX 无关。

| Apple 通道名 | X55 MHI 通道 | 用途 |
|---|---|---|
| `pbl` / `sahara` | 2 / 3（ER=1） | Sahara 镜像传输 |
| `diag` | 4 / 5（ER=1） | 64 项，本项目未用 |
| （EFS） | 10 / 11（ER=1） | mission EFS |
| （WWAN） | 14 / 15（ER=0） | 存在但被忽略 |
| `qmux1` / `qmux2` | **20 / 21（IPCR, ER=0）** | QMI over QRTR |
| — | **100 / 101（IP_HW0, ER=4/5）** | QMAP 数据面 |

### 4.3 QMUX ↔ QMI/QRTR 桥

**帧格式**

QMUX 头 6 字节：I/F type(1) + length(2 LE, 不含 I/F 字节) + control flags(1) + service type(1) + client id(1)
QMI SDU 头 7 字节：flags(1) + transaction(2) + message id(2) + TLV length(2)

除 CTL 外 SDU 头与 QRTR 上的 QMI 完全一致，所以**上行请求 = QMUX 帧去 6 字节头，下行应答 = 加回 6 字节**。

**三元组分配**（`cbq_client_alloc`）

1. **池化优先**：非私有服务时先找 `pooled` 的同服务 slot
2. **新 slot 轮转**：从 `next_slot` 环形找空 slot
3. **cid 分配**：每 (channel, service) 一个 1..254 循环 cid，0 留给 CTL，255 广播
4. `local_port = 0x4100 + slot`，slot 上限 96（`0x4100..0x415f`）

池化的原因（v25 实测）：基带复位后 X55 的 NAS 回 `QMI_ERR_CLIENT_IDS_EXHAUSTED` 然后不再响应——说明 DEL_CLIENT 并没有真正释放 X55 的 client。池化的 client 不接流量。

**CTL 本地应答**

| 消息 | ID | 行为 |
|---|---|---|
| GetClientId | 0x0022 | 分配 cid；服务不存在 → `QMI_ERR_INVALID_SERVICE_TYPE`(31)；slot 耗尽 → `CLIENT_IDS_EXHAUSTED`(5) |
| ReleaseClientId | 0x0023 | 池化该 client |
| Sync | 0x0027 | 释放该通道所有 client |
| GetVersionInfo | 0x0021 | 列服务，最多 32 个 |

### 4.4 Sahara 设备端（角色是反的）

真机上 BBUpdater 是 host、基带是 device。HakiOS 上 X55 自己带固件（由 `umi_modem.c` 加载），所以桥在 `sahara` 通道上扮演 **device 端**，假装成一个还没被加载固件的基带，把 BBUpdater 送来的镜像收下扔掉。

状态机：`CBS_IDLE → CBS_WAIT_HELLO_RESP → CBS_WAIT_ELF → CBS_WAIT_PHDRS → CBS_WAIT_SEGMENT → CBS_WAIT_DONE → CBS_FINISHED`，旁路 `CBS_FAILED`、`CBS_COMMAND`。

**HELLO 与 mode**：设备端发 mode 0（image pending）。BBUpdater 的 `HELLO_RESP` 带 host_status / host_mode：

- host_status != 0 → FAILED
- host_mode == 3（COMMAND）→ 回 COMMAND_READY，进 command 状态
- host_mode 是 0 或 1 才继续；**其它 mode（包括 2）一律 FAILED**
- 然后发 READ_DATA(offset=0, len=64) 探 ELF 头（覆盖 ELF32 的 52 字节和 ELF64 的 64 字节）

command 模式下：execute 2 = MSM hardware id（chip id 在 offset 4），execute 1 = serial number。

**镜像 id 固定 0x15**（SBL1，与真实 Sahara 的 `md_image_index(21)` 是同一个 id）。

**BHI 寄存器读取**

| 寄存器 | 返回 |
|---|---|
| 0 | serial，`0x0c0ac42b`（假值，注释说 "no real serial number leaves this machine"） |
| 3 | MSM hardware id，chip id 在 offset 4 = `0x0003b0e1`（Mav17） |
| 4 | PK hash，**固定默认值** |

固定 PK hash 是必须的：任何别的 hash 都意味着"已 fuse"，BBUpdater 会拒绝加载未个性化的镜像（`kBBUReturnUnPersonalizedImageError`）。

### 4.5 Apple 私有 QMI 应答

Apple 私有服务区间 `0xe1..0xea`（libQMIParser 的 serviceTypeFromString 表），外加 IMSRTP `0x28`。X55 没有这些服务，桥自己实现；未实现的走 `QMI_ERR_NOT_SUPPORTED`(94)，按 (service, message) 去重记录一次。

**激活（ActivationRegister 0xb00c）** 构造的 TLV：

| TLV | 值 | 语义 |
|---|---|---|
| 0x01 | 0 | ActivationState = kActivated |
| 0x4f | 2 | service provisioned = kProvisioned |
| 0x50 | 2 | slot 一的 provisioned 状态 |
| 0x56 / 0x57 | 1 / 1 | Slot1/2IsAccepted |
| 0x15 | 1 | ActivationMode = kDisabled |
| 0x42 | ICCID（10 B） | ActivationIccidSlot1 |
| 0x43 | IMSI（8 B） | slot 一的 IMSI |

每个字段都有强制理由（`coachz_bb_data.h:246-274`）：

- 没有 0x4f → `baseband_service_provisioned_states` 停在 kUnknown，运营商名字拿不到
- 没有 0x56/0x57 → CommCenter 打 "Baseband activation result is unexpected!" 和 "BB activated, but there is no SIM"
- 没有 0x42 → 落到 "BB activated, but there is no SIM"；`pp` 随后用 ICCID/IMSI 匹配 carrier bundle（`kCTPostponementInfoICCID`/`kCTPostponementInfoIMSI`）。**只有 ICCID 匹配而 IMSI 不匹配时，carrier bundle 根本建立不起来**（没有 APN、没有数据上下文）。所以两个都要
- 0x15 = kDisabled：iOS 16.5 读成 `fActivationEnabled`，激活启用且无 postponement ticket 时，Internet 上下文选择会拒绝启动数据

**ICCID / IMSI 来源**：CommCenter 自己读卡（UIM `0x0b` Read Transparent `0x0020`，TLV 0x02 = {file id, path length, path}，回答 TLV 0x11）。桥挂接 `bb_sim_request` / `bb_sim_answer`，解出值存 `bb_iccid`/`bb_imsi`，并**重发一次激活 indication**。注释明写 "Never logged"。

**Carrier bundle 文件**（BSP `0xa000` SendFile）

- TLV 0x01 = {u16 file type, u16 segment, u16 segment count, data}，每请求 1024 B 一段
- 回答**必须带 TLV 0x40 ExpectedSegmentInfo**（下一个段号），否则 CommCenter 报 "Next segment numbers are not matching"
- 最后一个段答完**必须发 FileTransferStatus indication**（BSP `0xa001`），否则 CommCenter 等 22 秒判失败、配置不缓存

代价记录在 `coachz_bb_data.h:306-308`：不这样应答的话，CommCenter 每次开机都推、复位基带、再推，大约每 12 秒一次。

这些文件是 Apple Mav16 的 carrier 配置，本 modem 有自己的，**接收后丢弃**。

**厂商上报消息**：NAS `0x03` 和 WDS 的 `0x555b`（频率上报、链路统计）是 Mav16 私有扩展，本固件回 `QMI_ERR_INVALID_QMI_CMD`。桥接受但不做任何事。

### 4.6 PDP / 数据面归属

**归属划分很明确**：WDS 完全属于 CommCenter，桥只观察生命周期，不碰凭据和地址。`coachz_bb_pdp.h:3-5`："No addresses or APNs are retained here."

**端点改写**：`cbd_endpoint_fix` —— 请求里端点 TLV 缺失且请求带 mux id（0x11）时，插入 `{3, 4}`；端点存在但值不同则改写。

> 原因：v23 实测 WDS `0xa2`（Bind Mux Data Port）和 QOS `0x2b`（Bind Data Port）只给 mux 时，X55 回 `QMI_ERR_INVALID_ARG`(0x30)。

**WDS 观察**（`cbp_observe`）跟踪：Bind Mux Data Port `0x00a2`、Set IP Family `0x4d`、Start Network `0x20`、Stop `0x21`、Get Runtime Settings `0x2d`、Packet Service Status `0x22`。

**同一 mux 上 IPv4 和 IPv6 是两个独立的 WDS client**，一个断不能拆掉另一个。

**`pdp_ip0..3`**：4 个接口（`IFNET_FAMILY_CELLULAR` / `IFT_CELLULAR`），unit i 对应 **QMAP mux i+1**。

- `bb_pdp_output` 是上行：家族不在 `p->families`、接口被 blocked、或队列满（64）→ 丢包回 ENETDOWN
- `umi_bb_data_input` 是下行：mux → `bb_pdp_ifaces[mux-1]`，家族匹配且未 blocked 且 IFF_UP → 分配 mbuf 交 `ifnet_input`
- `bb_pdp_update` 由 WDS 状态算出每个 mux 承载哪些家族，据此 set/clear `IFF_RUNNING`

`AppleIPAppender` 只有 selector 16（Block PDP traffic）是真功能，其余全是"成功并清零输出"的占位——因为 `ApplePDPHelperInterface::getInstance()->enableMetadataTrailer()` 无空指针检查，没有这个服务就直接崩。

### 4.7 状态机

| 变量 | 含义 | 置位 | 清除 |
|---|---|---|---|
| `bb_published` | 桥已发布服务 | `umi_bb_publish` | 永不清 |
| `bb_modem_ready` | X55 的 QMI 服务已可用 | `umi_bb_modem_ready` | `umi_bb_modem_lost` |
| `bb_transport_lost` | 传输层死了且未恢复 | `umi_bb_modem_lost` | `umi_bb_modem_ready` |
| `bb_boot_image_sent` | BBUpdater 已送过镜像 | `externalMethod` case 4 且通道是 PBL | 复位/power 时清 |
| `bb_hello_wanted` | sahara 的 HELLO 要等基带起来 | sahara 通道 open 且 `!bb_modem_ready` | `umi_bb_modem_ready` 或复位 |
| `bb_link_ok` | IP_HW0 是 raw IP over QMAP | `umi_bb_link` / `bb_wda_answer` | 同 |

**`bb_transport_lost` 与 `bb_modem_ready` 的区别是关键**：`bb_modem_ready` 在每次开机过程中都是 false，不能从它推导"传输丢失"；只有故障才置 `bb_transport_lost`。

**故障后的行为链**（`umi_bb_modem_lost`）：

1. 清 `bb_modem_ready`，置 `bb_transport_lost`，清 `bb_link_ok`，清激活状态
2. 飞行中的请求就地回 `QMI_ERR_INTERNAL`
3. 丢弃排队的 TX 帧
4. `cbq_reset_host_clients` 池化 ABM 的 client，**清空 server 表**（让重启后 modem 自己发的 NEW_SERVER 成为唯一来源）
5. 清 IMS audit / cellinfo / voice 的端口绑定

之后每个来自 ABM 的 QMUX 请求**被丢弃且不给任何回复**。理由：

> 必须让 ABM 自己的 ATCS 定时器超时，只有这个超时才会让 CommCenter 去复位基带；如果给一个合成的错误应答，ABM 不会超时，CommCenter 也就不会复位。

实测 v29t：ABM "Reset requested" 用了 25 秒。

**ABM 复位**（AppleBaseband 的 reset/power selector）：

对 ABM 来说基带重启了——清 `bb_boot_image_sent`、`bb_hello_wanted`，flush sahara 和两个 qmux 队列，清激活状态，`bb_reset_calls()` 停掉 ABM 起的数据 call，`ubp_forget` 清未决请求，池化 client。

但**真实基带不复位**：由 `umi_modem_state()` 决定。链路活着 → 只记录；链路没了 → `umi_modem_restart()` 请求真正的 bring-up。

`bb_reset_calls` 给每个活跃的 WDS client 发 Stop Network（用它自己的 handle），transaction 用 `0xff00 + slot`。原因（`hakios.umi_baseband.cpp:1050-1053`）：否则复位后 ABM 的 Start Network 会 join 旧 call，没有 Packet Service Status indication 跟随，上下文一直停在 activating，直到 CommCenter 再复位一次（v25：13:00:03 → 13:06:03）。

### 4.8 VoPS 覆盖（`umi_vops=1`）

CommCenter 的 "Ims Voice Support on LTE" 能力只有一个输入：NAS System Info indication（`0x4e`）或 Get System Info 响应（`0x4d`）的 TLV 0x21。X55 报 0，于是 CommCenter 把 IMS 语音排除在 Bambi 的服务掩码之外，只注册 SMS。

`umi_vops=1` 打开后，桥把那个字节改成 1。约束很严：

- 只在 NAS 服务、只在 `0x4e`/`0x4d` 消息上
- 只在消息带 LTE 系统/服务信息（TLV 0x14 或 0x19，即 LTE 驻留）时
- 只在 TLV 0x21 存在且值为 0 时
- 改的是**桥自己的副本**，不是 modem 的 SDU
- 长度、其它所有 TLV（含紧急相关的）都不动

---

## 5. 两层之间的接口面

`umi_baseband.h` 是 kernel 层与桥接层**唯一**的接口。

| 函数 | 语义 |
|---|---|
| `void umi_bb_publish(void)` | 发布 AppleBaseband 全套服务 + `pdp_ip0..3`。**必须在基带起来之前调用**，否则 CommCenter 找不到 |
| `void umi_bb_modem_ready(void)` | QRTR discovery 完成 |
| `void umi_bb_new_server(service, version_instance, node, port)` | QRTR NEW_SERVER |
| `void umi_bb_del_server(service, node, port)` | QRTR DEL_SERVER |
| `int umi_bb_qrtr(dst_port, src_node, src_port, body, size)` | 收 QRTR 数据包；dst_port 在 `0x4100..0x413f` 时返回 1 |
| `int umi_bb_announce(service, version, instance, port)` | 向基带 name server 发布 host 侧 QRTR 服务 |
| `unsigned umi_bb_poll(send, release)` | 桥交出攒下的 QMI，返回处理条数 |
| `void umi_bb_link(dl_max_size)` | kernel 已把 IP_HW0 设为 raw IP over QMAP(5)；0 = 无链路 |
| `int umi_bb_data_input(mux, ip, length)` | 一个下行 IP 包，返回是否被某个活跃 pdp_ip 接走 |
| `uint32_t umi_bb_data_take(mux, ip, room)` | 取下一个上行包及其 mux |
| `void umi_bb_report(second)` | 周期统计 |
| `void umi_bb_modem_lost(void)` | 传输层死了，桥要清理并结清欠账 |

kernel 层另外导出：`umi_modem_wake()`、`umi_modem_state()`、`umi_modem_restart()`、`umi_modem_restarts()`、`umi_modem_quiesce()`、`umi_modem_init()`、`umi_rpmh_modem_power()`。

**契约**：`umi_bb_*` 是跨线程的——kernel 线程调 `umi_bb_poll` 时 bridge 侧不能阻塞；`umi_bb_qrtr` 在 QRTR 接收路径上被调。

---

## 6. 安全与隐私边界

**日志策略**：只记 QMI 头字段和计数，**绝不记 TLV 内容**——没有 IMEI、IMSI、ICCID、号码、APN 或地址。

- `bb_trace` 打方向、通道、service、cid、type、message、txn、长度、error
- 采样：UIM(`0x0b`)、WDA(`0x1a`)、WDS(`0x01`) 每条都打；其它服务同种消息打前 6 条，之后每 2 的幂打一条
- 预算 `BB_TRACE_BUDGET 4000` 行，超出后只计数
- `bb_pdp_note` 只打 `address-fields %u`（settings 位标志数），不打地址
- ICCID/IMSI、v6cfg 注释明写 "Never logged"

**无写入路径**（硬性）：

- Sahara 收到任何写命令、memory-debug、RDDM 一律 `ENOTSUP`
- TFTP 的 WRQ 被 ACK 后丢弃
- EFS sync 只在 RAM 里读进来再丢掉
- UFS 分区访问有编译期护栏：
  ```c
  #define MD_UFS_READ_ONLY 1
  _Static_assert(MD_UFS_READ_ONLY == 1,
      "external modem UFS sources must remain read-only");
  ```
- 不写任何 NV / EFS / modemst / fsg / 校准分区
- 不碰 PMIC PON / soft-reset GPIO（UMI 的 `qcom,ext-sdx55m` 节点没有这两个属性，AP 侧唯一握手是 AP_STATUS）

---

## 7. 踩过的坑（实机验证过的）

这部分是整份档案最有价值的部分——每一条都对应一次真实失败。

### 7.1 会挂死 SoC 的操作（改动时必须保留）

1. **故障时不得访问 config space / DBI / BAR**。endpoint 掉线后这些寄存器在冻结的链路后面，一次读就能永久卡住线程（v29v 16:06:48）。

2. **故障时不得门控时钟**。时钟门掉后，另一线程遗留的 doorbell/BAR 访问会永不完成，整个 SoC 停摆（v29v 16:06:48、v29w 19:27）。

3. **`md_gate()` 不得清 `0x8d058 bit0`**。那是 **Wi-Fi 的 PCIe0 时钟分支**，清了会让 ath11k 的 ring pointer 写挂死。注释原文：

   > v29w: 0x8d058 bit0 is Wi-Fi's own PCIe0 clock branch -- umi_pcie.c:102 is the branch() that turns it on, and clearing it here (even on a clean stop) is what left ath11k's ring pointer write hanging on the SoC. Do not gate it from the modem.

4. **`md_gate()` 也不清 `0x52010 bit10-15` 和 `0x8c00c bit0`**——没有 gcc-sm8250.c/gcc-kona.c 无法确认它们是否也投票 PCIe0，留了疑问。

5. **未初始化 SMMU 就开 bus mastering** 是最脆弱的一环。

### 7.2 语义耦合坑

1. **DMS Set Operating Mode 到 4/5 会被改写成 1（low power）**。X55 收到 reset/shutting-down 会重启自己，之后再没有 QMI 回答，只有 `umi_modem.c` 能重新 boot 它。而 baseband reset 是 ABM 通过 AppleBaseband 主动发起的，桥接层要"演"这个角色——所以把模式改写成 low power。

2. **每个 QMI 请求都必须被回答**。答案丢了 CommCenter 会在 ATCS 超时后 reset 基带，而且会反复 reset。所以每个转发给 X55 的请求都记在 `ubp` 表里，8 秒（`UBP_ANSWER_MS`）没答复就用 `QMI_ERR_INTERNAL` 应答；表满 128 项时新请求当场被应答。

3. **WDS Start Network 失败要伪造 Packet Service Status indication**。CommCenter 的 `QMIDataContextIP::handleActivateResponse` 只在收到 WDS `0x0022` indication 时才知道该断开，别的失败类型它就干等。v25 实测：IMS IPv4 被拒 #51，context 等了 360 秒然后 CommCenter reset 了基带。所以要合成 TLV 0x01（连接状态）+ 0x10（call end reason）+ 0x11（verbose reason）+ 0x12（IP family）。

4. **QMUX 队列深度必须是 1024**。参考实现用 64 时会丢开机时的 indication，CommCenter 于是 reset 基带。

5. **X55 的 client 永远不 DEL_CLIENT，只池化**。v25 实测：DEL_CLIENT 没有真正释放 X55 的 client，NAS 回 `QMI_ERR_CLIENT_IDS_EXHAUSTED` 后不再响应。v24 实测：Apple 基带复位后 ABM 从不释放 client，桥如果保留全部，64 个槽全满 → `QMI_ERR_CLIENT_IDS_EXHAUSTED`，所有 command driver 停在 not ready。

6. **来自 X55 的消息只能到达该 client 所绑定服务的 node:port**。v25 实测：一条 WDS `0x22` indication 被 ABM 当成另一个服务的来解析，报 "QMI parsing error … msgId=0x0022" 然后 ABM ASSERT、CommCenter SIGTRAP。端口易主后这是真实风险。

7. **DMS reset 之后必须先停掉 ABM 已起的 call**。否则复位后 ABM 的 Start Network 会 join 旧 call，没有 0x22 indication 跟随。

8. **基带复位后必须释放 ABM 的 QMI client 并清 server 表**，否则重启后 ABM 看到的是陈旧的 NEW_SERVER。

9. **Mav17 BBUpdater 的固件路径查找走的是 personalized 路径**：`/private/preboot/<HASH>/usr/local/standalone/firmware/Baseband/Mav17`（HASH = loader 的 `boot-manifest-hash` 大写十六进制）。v22a 实测失败于此：`reset: reason=modem boot up failure [Baseband Firmware Path Not Found]`。

10. **`/baseband` 节点需要 `class` 属性和 ≥24 字节的 `config`**，否则 lockdownd 报 "no class property of IODeviceTree:/baseband"。

### 7.3 限制清单

1. **只能从 EE=0（PBL）冷启动**。已有固件占用的环无法安全接管：

   ```c
   if (ee != 0) {
       MD_LOG("EE=%u is not PBL: its rings are not ours; cold PBL required\n", ee);
       return EBUSY;
   }
   ```

   **软件没有冷复位能力**（没有 PMIC PON），全新 PBL boot 是前提。

2. **只能 basic QMAP (5)，无 checksum 扩展**，下行聚合 ≤32768，上行固定 32 包。WDA 回读不匹配就 `EPROTONOSUPPORT`。

3. **只用单个 IPv4 PDN**。kernel 自有路径固定 mux=1、APN=cmnet。

4. **不用 MSI 收事件**，全部轮询。

5. **EFS sync 只支持两种地址宽度**，单条 region ≤4096 B、总字节 ≤128 MiB、单段 ≤64 MiB。

6. **QRTR 只支持 v1（32 B 头）和 v3（16 B 头）**。

7. **TFTP 只支持 `/readonly/firmware/image/` 前缀**，其余路径 ENOENT。

8. **servreg locator 的域表是硬编码 6 条**，照抄 Linux SM8250 表。

9. **voice 通道 80 特殊**：vendor `mhi_init.c` 刻意跳过它的通道上下文，`voice_mhi.c` 从不准备 transfer、不敲通道门铃，MVM 拥有 mailbox 握手。实现上只把 mailbox 页映射进 DMA 池，**不准备 transfer、不敲门铃**。不要把它当普通 UL PCM 通道。

10. **MT SMS 会停在 MessagesBlastDoorService（SIGTRAP）**：BlastDoor 要求 `sandbox_check(getpid(), NULL, 0) == 1`，需要内核 Sandbox stand-in 记录带 `seatbelt-profiles` entitlement 的进程。

---

## 8. 关键常量表（移植时逐个核对）

### 8.1 kernel 层

| 常量 | 值 | 位置 |
|---|---|---|
| 通道环项数 | 128 | `MD_TRES` |
| 单包缓冲 | 32768 | `MD_PACKET` |
| 事件环数 | 6 | `MD_NER` |
| DMA 池 | 16 MiB | `MD_DMA_SIZE` |
| IOVA 基址 | 0xb0000000 | `MD_IOVA` |
| 端点 SID | 0x1d01 | `umi_modem_dma.inc:148` |
| MSI alias / SETSPI | 0x17a10000 / 0x17a10040 | `umi_modem_dma.inc:95,209` |
| MSI INTID | 864 | `umi_modem_dma.inc:210` |
| QRTR 主机 node | 1 | `MD_HOST_NODE` |
| QRTR 控制端口 | 0xfffffffe | `MD_QRTR_CTRL` |
| QMI 客户端端口 | 0x4000 / WDS 0x4004 / WDA 0x4005 / PDM 0x4001 | |
| TFTP v1/v3/数据 | 0x4002 / 0x4003 / 0x4010+slot | |
| 桥接层端口范围 | 0x4100..0x413f | `umi_baseband.h:20` |
| TFTP 只读前缀 | `/readonly/firmware/image/` | `MD_TFTP_PREFIX` |
| APN | cmnet | `umi_modem_data.inc:106` |
| 端点路由 | BDF 0x0100 / SID delta 1 → `0x01000100` | `umi_modem_dma.inc` |
| PCIe ID | 17cb:0306 | `umi_modem_pcie.inc:184` |
| BAR0 | 0x64300000 | `umi_modem.c:53` |
| 通道初始表 | {2,3,4,5,10,11,14,15,20,21,100,101} | `umi_modem_mhi.inc:340` |

### 8.2 桥接层

| 常量 | 值 | 位置 | 原因 |
|---|---|---|---|
| `BB_QUEUE` | 1024 | `hakios.umi_baseband.cpp:59` | 64 会丢开机 indication → CommCenter reset |
| `BB_READS` | 8 | `:60` | 每通道异步读等待数 |
| `BB_TX` | 64 | `:61` | 等 modem 线程的 QMI 队列 |
| `BB_FRAME_MAX` | 64 KiB | `:57` | 一次读/写上限 |
| `BB_PDP_QUEUE` | 64 | `:67` | 每接口上行队列 |
| `BB_SYNC_READ_SECONDS` | 30 | `:62` | 同步读超时 |
| `CBQ_MAX_CLIENTS` | 96 | `coachz_bb_qmux.h:58` | |
| `CBQ_MAX_SERVERS` | 64 | `:57` | |
| `UBP_MAX` / `UBP_ANSWER_MS` | 128 / 8000 | `umi_bb_pending.h:19-20` | |
| `CBS_MAX_IMAGES` / `CBS_MAX_PHDRS` | 32 / 64 | `coachz_bb_sahara.h:46-47` | |
| `CBS_PACKET_MAX` | 48 | `:48` | Sahara 包大小 |
| **PCIe endpoint** | **type 3 / interface 4** | `coachz_bb_data.h:37-38` | X55 的 IP_HW0 端点 |
| **WDS mux** | **1** | `umi_modem_qmi.h` | |
| **Sahara image id** | **0x15**（SBL1） | `hakios.umi_baseband.cpp:70` | |
| **chip id** | **0x0003b0e1**（Mav17） | `coachz_bb_sahara.h:40` | |
| **DAP 顺序** | QMAPv4(8) → QMAP(5) | `coachz_bb_data.h:47` | |
| **下行聚合** | 32 包 / 32768 字节 | `coachz_bb_data.h:44-45` | |

### 8.3 端口地址空间

```
0x4000..0x4003   kernel 内部（Wi-Fi / IPA / TFTP）
0x4004 / 0x4005  kernel WDS / WDA 客户端
0x4010+slot      TFTP 数据端口
0x4100..0x415f   桥接层 ABM 客户端 slot（0x4100 + slot，96 个）
0x4200           GNSS
0x4201           cell-info
0x4202           voice
0x4203 / 0x4204  IMS audit
0x4205           DCM
```

---

## 9. 可调参数

### 9.1 boot-arg

| boot-arg | 值 | 含义 |
|---|---|---|
| `umi_modem` | `1` | kernel 自拨号模式 |
| | `2` | CommCenter 桥接模式（触发 loader 合成 `/baseband`） |
| | 其它/缺省 | 不启动 |
| `umi_modem_probe` | `1` | 只探测，bus mastering 保持关 |
| `umi_vops` | `1` | LTE VoPS 覆盖（桥接层） |
| `umi_rtpok` | — | IMSRTP（桥接层） |

loader 白名单只放行 `umi_modem=1`、`umi_modem=2`、`umi_modem_probe=1`、`umi_vops=`、`umi_rtpok=`。`modem=1`、`modem_step=7`、`umi_modem=3` 之类一律拒绝。

### 9.2 sysctl

| 名称 | 类型 | 默认 | 含义 |
|---|---|---|---|
| `kern.hakios.umi_mdm_restart` | int | **0** | 故障后是否做完整链路重启 |

---

## 10. 设计文档与来源

### 10.1 仓库内文档

| 文档 | 内容 |
|---|---|
| `devices/xiaomi-mi-10/COMMCENTER-umi.md` | **最重要**。CommCenter 桥接的完整设计与 25 个版本的实机迭代记录（v22a…v29t），每节记录一次失败与修复 |
| `devices/xiaomi-mi-10/MODEM-MAINLINE.md` | 从 Linux mainline 移植到 HakiOS 的硬件事实：寄存器、通道表、镜像清单、启动顺序 |
| `devices/xiaomi-mi-10/COMMCENTER-ACTIVATION-umi.md` | 激活流程（carrier bundle / postponement ticket） |
| `devices/xiaomi-mi-10/VOLTE-umi.md` | VoLTE 可行性：AP SIP vs modem IMS 两条路线 |
| `devices/xiaomi-mi-10/VOICE-CS-umi.md` | 语音 CS 路径 |
| `devices/xiaomi-mi-10/STATUSBAR-SIGNAL-umi.md` | 状态栏信号格 |
| `devices/xiaomi-mi-10/LOCATION-umi.md` | 定位（GNSS / cellinfo） |

文档用了 `[E]`（本树/日志证据）、`[R]`（参考实现，未在 16.5 复核）、`[S]`（推测）三级标记，引用时要保留。

### 10.2 代码来源与许可

`hakios.umi_baseband.cpp:14-16`：

> Ported from UniXNU/QcomBaseband dd0199cf, baseband/coachz_baseband.cpp and coachz_bb_pdp.cpp: Copyright (c) 2026, Trogdor-ipadOS contributors (HoshinoYumeka, @chiba233), BSD-2-Clause.

UniXNU/QcomBaseband 是 HoshinoYumeka / chiba233 从 Trogdor-ipadOS 抽出的内核桥，BSD-2-Clause，在 Chromebook (SC7180, iPadOS 17.7.11) 上报告有运营商、信号格、SIM 身份、LTE 和 IPv4 上网。

各文件标注：

| 文件 | 标注 |
|---|---|
| `coachz_bb_control.h` | From UniXNU/QcomBaseband dd0199cf, **unchanged** |
| `coachz_bb_sahara.h` | From …, **unchanged**（chip id 改为 Mav17） |
| `coachz_bb_pdp.h` | From …, **unchanged** |
| `coachz_bb_channels.h` | From …, **unchanged** |
| `coachz_bb_qmux.h` | From …；Mi 10 改动：client 池化、轮转新 slot、96 slot |
| `coachz_bb_data.h` | From …；激活加 0x15，端点固定为 X55 的 PCIe IP_HW0 {3,4}（不是 SC7180 的 IPA {4,1}） |
| `umi_bb_pending.h` | HakiOS 自有 |
| `umi_baseband.h` | HakiOS 自有 |

### 10.3 host 侧测试

| 路径 | 内容 |
|---|---|
| `devices/xiaomi-mi-10/tools/modem-tests/mhi.c` | MHI 协议模型测试 |
| `devices/xiaomi-mi-10/tools/modem-tests/stage3.c` | stage3 控制面测试 |
| `devices/xiaomi-mi-10/tools/modem-tests/bb.c` | 桥接层模型测试（647 行）：`test_qmux`/`test_sahara`/`test_sahara_command`/`test_registers`/`test_control`/`test_channels`/`test_data`/`test_answers` |
| `devices/xiaomi-mi-10/tools/test-modem.py` | loader 参数白名单测试 + 假基带交互 |
| `devices/xiaomi-mi-10/tools/stage-modem-firmware.py` | 固件暂存 |
| `devices/xiaomi-mi-10/tools/stage-baseband-firmware.py` | 从自己的 IPSW 解包 Mav17 bbfw 到 `fw-baseband/Mav17`（本地，不进 git） |
| `devices/xiaomi-mi-10/tools/import-modem-firmware.py` | 导入并校验 17 个固件文件（0700/0600 权限 + SHA256SUMS） |
| `tools/modem-cycle.sh` | 全流程：等手机 → 进 loader → 传内核 → 跑 → 汇总基带日志 |
| `tools/qmi-test/` | QMI 助手测试（Wi-Fi 侧通用 QMI，非基带专用） |

---

## 11. 主线小结

**一次完整 bring-up 的因果链**：

```
GPIO/PERST + rails(ldoa5 880mV, ldoa9 1200mV) + clocks(GCC/REFGEN 100MHz)
  → PHY(QMP v4 Gen3x2) + PCIe link(LTSSM L0, DBI link+active)
  → 枚举(17cb:0306, BAR0 @0x64300000, bus mastering OFF)
  → SMMU(SID 0x1d01, CB, 16MiB pool @IOVA 0xb0000000)
  → MSI 别名映射(INTID 864, masked) + bus mastering ON
  → BHI 直送 SBL (4MiB buffer)
  → MHI READY→M0, 上下文 + 4 个软件事件环
  → Sahara: HELLO → 17 个镜像 READ_DATA → image complete → DONE
  → EE==2 (AMSS) + M0 同步
  → ER4/ER5 硬件事件环
  → IPCR 20/21 (ER0) 开，EFS 10/11 (ER1) 开
  → QRTR HELLO + NEW_LOOKUP → 等 NEW_SERVER (NAS/UIM)
  → (bridge) umi_bb_modem_ready()  /  (自有) UIM→DMS online→NAS 注册
  → md_service(): 事件环轮询 + 桥接 QMI 泵 + TFTP + EFS + voice mailbox
  → md_data_link(): WDA format → IP_HW0 100/101 → QMAP 泵
```

**故障模型**：所有故障收敛到 `md_fault()` 一个检查点；处理是 `md_stop(faulted=true)`（**只拉 PERST，不碰 config/DBI/时钟**）→ 通知桥接层 → 退避 → `md_link_reset()` 清状态 → 重跑同一条 bring-up。重启默认关闭，每 boot 上限 3 次。

**这条链上最脆弱的三个环节**（都已发生过实机挂死）：

1. 故障时访问 PCIe config/DBI/BAR
2. 故障时门控时钟（含误伤 Wi-Fi 的 `0x8d058 bit0`）
3. 未初始化 SMMU 就开 bus mastering

**架构上最重要的一条**：**kernel 层完全不做 DT 匹配，所有硬件地址是编译期常量。** 这套代码换板子的成本几乎全在"重新核对寄存器地址和 pin 号"上，而不是在改代码结构上。
