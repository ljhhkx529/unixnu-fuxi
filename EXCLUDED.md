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

> **B2–B7 是 2026-11-21 事后才补的排除，而且曾被重新提出过多次。** 若再提，先引这两行。

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

| # | 问题 | 当前知道什么 | 下一步的唯一判据 |
|---|---|---|---|
| **D1** | **modem 只发布 2 个服务（0x42、0x2b），UIM/NAS/WDS/DMS 一个都没有** | 685 s 内只发 4 个包（HELLO + 2×NEW_SERVER + 1×DATA）；84 次 lookup 0 应答 | 见下 |
| **D2** | **servreg locator 已发布并被 modem 问到，但 modem 仍未多发服务** | `evidence/2026-11-21-modem-servreg-locator.md`：NEW_SERVER 发到 node 0，modem 立刻来问 GET_DOMAIN_LIST（tms/servreg 和 tms/pddump_disabled），两个都答了。**modem 仍然只发布 2 个服务，UIM 仍然没发布** | 见 E |
| D3 | TFTP（QRTR 4096）未实现 | 全仓 grep 无 tftp | D2 已证伪"发布 locator 就够"，下一轮做 |

**D1 的参考实现（只读，勿抄 GPL 部分）**：小米 10 的 `hakios.mpss.c` —— locator 在 `:4492-4600`，TFTP 在 `umi_modem_mhi.inc:684-840`。
其文档原话：modem **先宣布 88 个服务，再停在 OFFLINE，等 AP 的 locator 与 TFTP 供给后才转 ONLINE**（`docs/ru/cellular.md` 的「23 октября 2026, вечер」一节）。

---

## E. 本轮（2026-11-21 locator 之后）的唯一任务

> 读出 `tms/pddump_disabled` 是在哪张 `.jsn` 表里。
> modem 问了 locator 两个名字：`tms/servreg`（在 `modemr.jsn` 里，已答）和
> `tms/pddump_disabled`（**不在** `modemr.jsn` 里）。后者决定了该读哪个文件。
> 先找到它，再决定做什么；**不要一次读完六个 `.jsn`**。

判据：日志或 dump 里出现 `pddump` 这个服务名的出处，且能说出它属于哪张表。

**Do not**：
- 不要通读 `package/FuxiModemProbe.cpp`（2201 行）。模式分派在文件末尾，用 `grep -n "fuxi-modem="` 定位。
- 不要同时改 metadata、DTB 顺序或任何无关代码。
- 不要重新讨论 A 组或 B 组的任何一条。
- 不要重新验证 D2（locator 已经发布并被 modem 问到了，见上）。
