# 已排除清单 / EXCLUDED

**规则：每轮开工的第一件事是读这个文件。这里列出的任何一条，都不要再验证、不要再作为阻塞理由提出。**

每条必须带出处：evidence 文件 + 行号，或 run id，或 sha256。没有出处的"已排除"不算数。

若你认为某一条被排除错了，**不要重跑实验**——先指出原文哪一行与你观察到的事实矛盾，等批准。

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
| **D1** | **modem 只发布 2 个服务（0x42 SERVREG_NOTIF、0x2b SSCTL），UIM/NAS/WDS/DMS 一个都没有** | 764 s 内只发 4 个包（HELLO + 2×NEW_SERVER + 1×DATA）；84 次 lookup 0 应答；locator 已答、modem 不再问 |

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

## E. 本轮（2026-11-21 之后）的唯一任务：RMTFS（QRTR service 14）

> 实现一个**只读**的 RMTFS 服务端，让 modem 能读它的 4 个文件系统。
> 判据只有一条：**modem 是否开始发 `OPEN /boot/modem_fs1`**（或 `modem_fs2` / `modem_fsc` / `modem_fsg`）。

### 第 0 步（不改代码）：清点

在设备上确认这四个分区**存在且可读**：`modemst1`、`modemst2`、`fsc`、`fsg`（GPT 名字精确匹配）。
若某个名字在这块板上不同或不存在，**停下来报名字**，不要猜。

### 第 1 步：读进内存

4 个分区各读一份只读副本到内核内存（UMI 的 `rmtfs_load()`，`hakios.mpss.c:3586-3620`）。
**分区一个字节都不许写。**

### 第 2 步：共享缓冲区

一块物理连续的内存作为 RMTFS 缓冲区（主线用 **0xd4a80000 / 2.5 MB**，见 `/tmp/x13/docs/蜂窝调研.md:11`），
并用 `assign_mem` 交给 **HLOS + MSS_MSA + NAV** 三方（UMI 的 `rmtfs_share(true)`，`hakios.mpss.c:3658-3736`；
你自己的 `FuxiModemAssign.h` 已经是这个形状，改参数即可）。
注意：fuxi 的内存图里**没有**这个区，先在 UEFI 内存图里加一行，或在已有保留区内选出不重叠的一段。

### 第 3 步：QRTR service 14

发布 service **14**，instance 0，应答 5 个消息：
`OPEN 1`、`CLOSE 2`、`RW_IOVEC 3`、`ALLOC_BUFF 4`、`GET_DEV_ERROR 5`（`hakios.mpss.c:3553-3557`）。

- `OPEN` 按路径打开：`/boot/modem_fs1`…`/boot/modem_fsg`（`rmtfs_files[4]`，`:3575-3578`）
- `ALLOC_BUFF` 返回缓冲区的地址与大小
- `RW_IOVEC` 读副本；**写请求只写内存副本，绝不落分区**
- `CLOSE` 释放

### Do not

- **不要写 `modemst1`/`modemst2`/`fsc`/`fsg`。** 这四个分区存 IMEI 与射频校准，写坏是变砖级别。
- 不要同时做 TFTP、IPA 或任何其它闸门。
- 不要通读 `package/FuxiModemProbe.cpp`。模式分派在 `:2559`，模式 `1`–`14` 在 `:2569-2641` 附近，加新模式在旁边。
- 不要重新讨论 A/B/C 组，也不要重测 B8（locator）。

### 交付

1. `evidence/2026-11-2X-modem-rmtfs.md`：分区清点结果、缓冲区地址与 `assign_mem` 的应答、每个 RMTFS 请求的原始字节、run id、kernel sha256、判据结论。
2. `porting.toml` 一条 finding（findings 数组，带 commit）。
3. `python3 scripts/porting-record.py check --device ../unixnu-fuxi` 通过（需 Python ≥ 3.11）。

**若判据是否**（modem 从未来要文件）：把 RMTFS 也写进 A/B 组的排除，下一轮转 IPA（`目标-M5.md:509-512` 的证据更强），**不要**先做 TFTP。
