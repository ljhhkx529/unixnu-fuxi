# 已排除清单 / EXCLUDED

**每轮开工的第一件事是读这个文件。**

这个文件**不是权威**，它是一份带出处的备忘录。它错过，而且不止一次（见 §F）。所以它分三档，**质疑权按档不同**：

| 档 | 内容 | 你的权利 |
|---|---|---|
| **C（禁止）** | 不可逆的危险动作 | **不许质疑**。写坏分区、重启 modem 子系统，这些没有"试一试"的选项 |
| **A / B（实测事实）** | 在设备上测出来的结论 | **可以质疑，但不能靠重跑**。指出现文哪一行与你手上的日志矛盾，并贴出那段日志。重跑一次实验证明别人错，成本是别人的 10 倍 |
| **D / E（判断与任务）** | 对事实的推论、本轮任务 | **随时可以推翻**。这是判断，不是测量。有更简单的解释就直接说，不要客气 |

**每条必须带出处**：evidence 文件 + 行号，或 run id，或 sha256。没有出处的条目**不算数**，可以当它不存在。

**发现错误怎么办**：不要默默绕过，也不要重跑实验。写一行 `CONTRADICTION: <条目号> — <文件:行> 说 X，但 <日志片段> 显示 Y`，贴在当轮 evidence 的开头，然后按手上的证据走。**这个文件会跟着改。**

---

## F. 这个文件自己错过的记录（保留，不要删）

留着这一节，是为了让下一个人知道：**这里的结论都是临时的。**

| 日期 | 错在哪 | 后果 |
|---|---|---|
| 11-21 | 把 "RMTFS 管宣布服务" 和 "TFTP 管转 ONLINE" 混成一句 | 让下一轮先去做 TFTP，顺序反了 |
| 11-21 | 说 "UMI 的 assign_mem 可直接借用" | UMI 那份只传 4 个寄存器参数、arginfo 却声明 7 个，是坏的；照抄会静默失效 |
| 11-21 | 提示词/文件里的参考坐标漏了 `:3765` 的服务端 | Claude 花了 50 分钟去 cgit 反推线格式 |
| 11-20 | 说 "DTB 的分段不该搬" | 上游 `qcom_mdt_pas_load` **确实搬**分段；照做会把正确的顺序改错 |
| 11-20 | 说 "掩码 API 能给 4 GiB 上限" | 实际 `contiguous` 在 `IOLib.cpp:758` 被重算为 false，两个掩码都失效 |

**结论：A/B 档的每一条都可能像上面这样错。** 质疑的方式是读它引的那一行，不是重跑实验。

---

## A. secure world 与 PAS（已全部关闭）

| # | 已排除的假设 | 来源（文件:行） | 日期 |
|---|---|---|---|
| A1 | metadata 的来源（.mdt 本体 vs 拼装 .bNN） | `evidence/2026-11-19-modem-mdt-metadata.md` 全文；packed 与 split 实测 0/4044 字节差异 | 11-19 |
| A2 | metadata 的地址宽度（>4 GiB 不是拒绝原因） | `evidence/2026-11-19-modem-pas-matrix.md`：adsp@0x8d0b1f000 被接受、modem@相邻地址被拒 | 11-19 |
| A3 | 固件来自哪个槽（modem_a / modem_b） | `evidence/2026-11-19-modem-slot-survey.md:50,67-68`：两块都 0xFFCFFFBB，槽问题关闭 | 11-19 |
| A4 | PAS 调用形状 / arginfo / 命令常量 | 同上矩阵：同一调用接受 adsp_dtb | 11-19 |
| A5 | mdt 全 21 个被接受的 pas id 逐一试过 | `evidence/2026-11-19-modem-pas-matrix.md` 的 mode 9 sweep | 11-19 |
| A6 | `region_assign`（mpss_dsm_region 交给 VMID 0xF）是拒绝原因 | `evidence/2026-11-20-modem-region-assign.md`：assign 应答 `0/0`，DTB 仍 0xFFCFFFBB | 11-20 |
| **A7** | **PAS 本身已通过，不再有任何 PAS 阻塞** | `evidence/2026-11-20-modem-low-metadata.md`（DTB 首次被接受）+ `evidence/2026-11-20-modem-up.md`（stage 9，31 段拷完，modem 起来） | **11-20** |

> **A7 是最重要的一条。** `0xFFCFFFBB` 从 2026-11-20 起就不再是阻塞。**任何把讨论拉回 PAS / secure world / metadata 地址的，先看 A7。**

---

## B. 链路与流量（已关闭）

| # | 已排除的假设 | 来源（文件:行） | 日期 |
|---|---|---|---|
| B1 | GLINK 链路没起来 | `evidence/2026-11-20-modem-link-up.md`：IPCRTR 打开，glink_up=1 | 11-20 |
| B2 | **GLINK credit（intent）耗尽** | `evidence/2026-11-21-modem-service-lookup.md:214`：`no room 0, too small 0, asked 0, done 52`，30 个 intent 从未同时被占 | 11-21 |
| B3 | **QRTR credit 耗尽** | 同文件 `:217`：6 个 RESUME_TX 的 body 全是 `node 0, port 4`，双向对得上上游 | 11-21 |
| B4 | 我们这侧丢包 | 同文件 `:223`：`rx 131 tx 117 data 41 dropped 0` | 11-21 |
| B5 | **credit 冻结在 10 pending 是"原因"** | 同文件 `:227`：**是症状**。modem 先停，credit 才不归还 | 11-21 |
| B6 | **SMP2P `fatal` 位 = modem 死亡** | 同文件 `:383,392`：安全运行里 fatal 第 42 s 升起，**modem 仍应答 413 s** | 11-21 |
| B7 | `fatal` 位与我们的包到达有关 | 同文件 `:286`：是 modem 自己的定时 handover 状态字，最后答复时间从 53.4 s 变到 61.5 s 而 fatal 不动 | 11-21 |
| B8 | **servreg locator 未发布是阻塞原因** | `evidence/2026-11-21-modem-servreg-locator.md`：NEW_SERVER 到 node 0 后 modem **立刻**来问 GET_DOMAIN_LIST，两个名字都答完，**modem 之后 764 s 再没问过，也仍只发布 2 个服务**。locator 必要但不充分 | 11-21 |
| B9 | **`tms/pddump_disabled` 是"表里缺了一条"** | 它是 PDR 的**策略查询**，不是任何域"提供"的服务：应答 **0 个域 = PD dump 未禁用 = 正确值**。参考实现对任何未知名字都只回 0 域，从不特判（`hakios.mpss.c:4634-4636`）。**任何 `.jsn` 里都不会有这个名字，不要去找** | 11-21 |
| B10 | **modem 的 failure reason 94 是"我们被拒"** | `evidence/2026-11-22-modem-radio-probe.md`：94 = `QMI_PROTOCOL_ERROR_NOT_SUPPORTED`（libqmi `qmi-errors.h`），是 modem 自己说"SSCTL Get Failure Reason (0x0022) 我不实现"。UIM 的卡状态同一轮答的是 **error 0**，栈是通的 | 11-22 |
| B11 | **NAS 的 error 74 = 我们发错了** | 同上：74 = `INFORMATION_UNAVAILABLE`，是"现在还没有这些信息"，不是拒绝。Get Signal Info 的回答里**只有 result 一个 TLV**，没有信号 TLV——radio 确实没有信号可报 | 11-22 |
| B12 | **RMTFS 通了 radio 就该自动注册** | 同上：DMS Get Operating Mode 答 error 0、模式 **5 = PERSIST**，注册状态 0 = not registered。参考实现的顺序是 **UIM→DMS online→NAS 注册**（`hakios-umi-baseband-档案.md:911`），我们从没过 DMS online 这一步。**卡通了不等于 radio 通了** | 11-22 |
| B13 | **QRTR 流控表 8 个槽够用** | 同上：`the qrtr flow has 17 packets pending over 12 destination(s)`。modem 每开一条流换一个源端口，目的地只增不减；上一轮的 8 槽表会丢第 9 个回答。现在是 32 槽（`UniXNU/.../qcom_qrtr.h:83`）。modem 依然一次 RESUME_TX 都不发，"信用还清就能复用"这条补救在真机上没用 | 11-22 |

> **B2–B8 是事后补的排除，而且曾被重新提出过多次。** 若再提，先引对应行。

---

## C. 危险动作（不是"已排除"，是"禁止"）

| # | 禁止 | 为什么 | 来源 |
|---|---|---|---|
| C1 | 向 SSCTL (0x2b) 发 `0x0020` | 那是 **SSCTL RESTART**，会让 modem 重启子系统 | `evidence/2026-11-21-modem-service-lookup.md:262-294` |
| C2 | 向 SSCTL (0x2b) 发 `0x0021` | 那是 **SSCTL SHUTDOWN** | 同上 |
| C3 | 向 SERVREG_NOTIF (0x42) 发 `0x0024` | 那是 **RESTART_PD**，重启保护域 | 同文件 `:533-535` |
| C4 | 对 SSCTL 做任何 id sweep | sweep 本身就是 C1/C2 的载体 | 同文件 `:293-294` |

---

## D. 当前真正未解决的问题（**唯一允许继续的方向**）

| # | 问题 | 当前知道什么 |
|---|---|---|
| **D1** | ~~**modem 只发布 2 个服务（0x42 SERVREG_NOTIF、0x2b SSCTL），UIM/NAS/WDS/DMS 一个都没有**~~ **← 11-22 已解** | 764 s 内只发 4 个包（HELLO + 2×NEW_SERVER + 1×DATA）；84 次 lookup 0 应答；locator 已答、modem 不再问。**RMTFS 的 assign 被接受之后 modem 发布 48 个服务，UIM 在里面**（见 A 组 A10） |

### D1 的两道闸门（UMI 原文，必须分清 —— 2026-11-21 更正）

| 闸门 | 控制什么 | fuxi 状态 |
|---|---|---|
| **RMTFS**（QRTR service 14 + 4 个分区） | **modem 是否宣布协议栈服务** | ❌ 全仓无 rmtfs，内存图无 `rmtfs_mem` |
| **TFTP**（QRTR 4096 + `modem_pr`） | **modem 是否从 OFFLINE 转 ONLINE** | ❌ 全仓无 tftp |

UMI 的原话是两句，不是一句：

- `hakios.mpss.c:1251-1252` —— "WITHOUT THEM ALL THE MODEM OFFERS **NO SERVICES BUT ITS OWN** (DMS, NAS... stay away)" → **RMTFS 管"是否宣布服务"**
- `docs/ru/cellular.md`「23 октября 2026, вечер」—— 服务完 `modem_pr` 之后 modem **同意转 ONLINE** → **TFTP 管"是否 ONLINE"**

**fuxi 的症状是"只宣布 2 个服务"，命中的是 RMTFS 这道闸，不是 TFTP。**
（本文件此前的 D3 把两件事混成一件，2026-11-21 更正：先做 RMTFS。）

### 参考实现的坐标（只读，勿抄 GPL 部分）

- RMTFS 常量与分区：`hakios.mpss.c:3533-3600`（`RMTFS_SERVICE 14`、5 个消息 id、4 个分区路径、`RMTFS_SECTOR 512`）
- RMTFS 共享缓冲区：`:3632-3660`（`rmtfs_buffer_make`）、`:3658-3736`（`rmtfs_share` = `assign_mem` 给 **HLOS + MSS_MSA + NAV 三方**）
- **RMTFS 服务端（完整实现，400 行，上过真机）：`:3765-3925` `rmtfs_receive`**
  - `OPEN`（:3785）：**TLV 1 = 路径字符串本身，前面没有长度字节**
  - `ALLOC_BUFF`（:3818）：请求 TLV 2 = u32 大小；应答 TLV **0x10 = u64 小端物理地址**
  - `RW_IOVEC`（:3837）：TLV 1 = 文件号 u32(1..4)；TLV 2 = 方向 u8（非 0 = 写）；TLV 3 = `count:u8` + count×12 字节
  - `CLOSE`（:3810）、`GET_DEV_ERROR`（:3922）
  - **写请求只写内存副本**（:3870 之后）
- TFTP：`umi_modem_mhi.inc:684-840`
- 主线的 `rmtfs_mem` = **0xd4a80000，2.5 MB**，文件模式只写副本（`/tmp/x13/docs/蜂窝调研.md:11,26,47`）

### 一条必须记住的对照（防止把 RMTFS 当充分条件）

`/tmp/x13/docs/目标-M5.md:169-170`：**rmtfs 正常运行时**，modem 仍只发 `RFS(14)/LSM(64)/IPA控制(49)/TFTP(4096)`，无 DMS/NAS；
同文件 `:509-512`：修好 **IPA**（SMEM 497 不预分配、IMEM `modem-tables` 改到 0x146a8000）之后，服务才全出、才有首次 LTE 注册。
**所以 RMTFS 不是充分条件 —— 但它在前、且 fuxi 完全没有。**

---
## E. 本轮（2026-11-22 之后）的唯一任务：TFTP（QRTR service 4096）

> 实现一个**只读**的 TFTP 服务端，让 modem 能读到它的 carrier 配置。
> 判据只有一条：**modem 的 operating mode 从 PERSIST 变掉**，或者 NAS Get Signal Info 的回答里**出现信号 TLV 0x14**。

### 为什么是 TFTP（不是 DMS Set Operating Mode）

2026-11-22 实测：卡读出来了（slot 1 present + active），但 radio 是 **PERSIST**（模式 5），
注册状态 0，信号答 error 74 = `INFORMATION_UNAVAILABLE`。

参考实现 `umi_modem_mhi.inc:685-687` 一句话说清了：

> The modem reads its carrier configuration (modem_pr/mcfg) here and stays offline
> (DMS DeviceNotReady) without it.

**D1 的两道闸是分开的**（本文件 §D 的表）：RMTFS 管"modem 是否宣布服务"（**已通**），
TFTP 管"modem 是否从 OFFLINE 转 ONLINE"（**没做**）。PERSIST 正是 carrier 配置缺失时 modem 的样子。

**不要用 DMS Set Operating Mode (0x2e) 代替 TFTP。** 参考实现的顺序是
UIM→DMS online→NAS 注册，但它的 DMS online 是在 TFTP 之后；先发 0x2e 是催 modem，
不是给它缺的那份文件。这条命令本身也不在 C 组（不是禁止），但它是**绕开**闸门而不是过闸门。

### 协议（参考实现是只读的，不要抄 GPL 部分，只用它定线格式）

`umi_modem_tftp.h` 与 `umi_modem_mhi.inc:684-850`：

- **服务 4096**，发布在 **version 1**（port `0x4002`）和 **version 3**（port `0x4003`）两个版本上，
  用 type-4 的 NEW_SERVER 包，host node = 1。
- **消息码**：RRQ 1、WRQ 2、DATA 3、ACK 4、ERROR 5、OACK 6。
- **请求格式**：第 1 字节恒为 0，第 2 字节是操作码；然后是 NUL 结尾的路径、
  NUL 结尾的 mode（`octet`）、再是成对的 NUL 结尾选项名和值。
- **选项**：`blksize`(8..65464)、`timeoutms`(1..255000)、`tsize`、`rsize`(1..100 MiB)、
  `wsize`(1..65535)、`seek`。modem 带选项来的时候**答 OACK**（不是 DATA）。
- **读**：路径前缀 `/readonly/firmware/image/`，映射到只读分区里的 `image/<X>`。
- **写**：`/readwrite/...` —— **确认后丢弃，一个字节都不许落盘**。
- **结束**：最后一个 ACK 之后的短 DATA 块（不足一个 blksize）结束传输。
- **ERROR**：opcode 5，第 3 字节是错误码，第 4 字节起是 NUL 结尾的文字。

### Do not

- **不要写任何 modem 分区**。TFTP 是只读的：写请求照参考实现那样确认后丢弃。
- 不要同时做 IPA 或任何别的闸门。
- 不要重测 B8 / B10 / B11 / B12 / B13。
- 不要把 carrier 配置从别的设备拷过来（IMEI / 射频校准）。

### 交付

1. `evidence/2026-11-2X-modem-tftp.md`：carrier 配置文件的清点结果、每个 TFTP 请求的原始字节、
   run id、kernel sha256、operating mode 的变化、判据结论。
2. `porting.toml` 一条 finding（findings 数组，带 commit）。
3. `python3 scripts/porting-record.py check --device ../unixnu-fuxi` 通过（需 Python ≥ 3.11）。

**若判据成立**（radio 变 online 或信号 TLV 出现）：把 TFTP 写进 A/B 组的排除，
下一轮转**读 IMSI**（UIM Read Transparent 0x0020，EF_IMSI 0x6f07）——
那是参考实现激活 TLV 里 0x43 的那一个。
