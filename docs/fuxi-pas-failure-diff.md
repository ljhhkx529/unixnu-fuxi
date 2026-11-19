# PAS 失败差异分析：UMI HakiOS 与 fuxi

日期 2026-11-20。只回答一个问题：**UMI 走 PAS 且成功，fuxi 为什么被拒。**
已有证据（`evidence/2026-11-19-modem-*.md`、`facts/baseband/{binding,manifest}.toml`）视为已建立，不重复。
本文补三项：UMI 的真实调用流程、逐项差异、哪些差异能解释 0xFFCFFFBB。

---

## 0. 结论摘要

1. **UMI 不是 SM8550 双镜像 PAS 的参考案例。** 它跑在 SM8250 的 `qcom,mss` 上：**一个镜像、一个 PAS id（4）、没有 DTB 阶段**。
   SM8550 是 `qcom_q6v5_pas`：两个镜像、两个 id（4 与 0x26），DTB 先走。fuxi 失败的地方**恰好在 UMI 没有的那个阶段**。
2. **UMI 证明了一件有用的事**：fixed 镜像**可以**接受 `PAS_MEM_SETUP`，而且递的是**整段** `MPSS_MEMORY_SIZE`（246 MiB），
   不是镜像大小。（上一轮我说它递镜像大小，错。）
3. **UMI 的 assign_mem 不能当证据**：它的 `scm()` 只把 4 个参数放进 x2..x5，arginfo 却声明 7 个，x6 还被 session 覆盖。
   这个调用是坏的，且全仓没有任何一条它成功或失败的日志。上一轮 "借用 rmtfs_share()" 的建议**撤回**。
4. **矩阵里真正的信号不是"镜像被拒"，而是只有 "(adsp_dtb, 36)" 被接受。** 同一个 ADSP 镜像交给 38 也被拒。
   也就是说：**36 是唯一接受任何东西的 id，而 36 恰好是本次启动里真正跑完整条 PAS 序列的那个 id。**
5. 由此得到一个可证伪的假设：**modem 的 `INIT_IMAGE` 依赖一个只有"已完整走完序列"的 id 才具备的 secure-world 状态**，
   而这正是本移植缺少的上游步骤 `qcom_pas_assign_memory_region()`（把 `mpss_dsm_region` 交给 VMID 0xF）的位置。
   见 §4。**这是当前最值得验证的一项。**

---

## 1. 三条此前判断的更正

| 上一轮的说法 | 核对结果 | 依据 |
| --- | --- | --- |
| UMI 的 `PAS_MEM_SETUP` 递镜像大小 | **错**，递整段 `MPSS_MEMORY_SIZE` | mpss.c:1509 |
| UMI 的 `rmtfs_share()` 是可直接借用的 assign_mem | **错**，只传 4 个参数 | mpss.c:3718 vs 1297-1328 |
| 失败日志可能被 `shutdownModem` 覆盖 | **错**，printf 在 shutdown 之前 | Probe:779 在 780 之前 |

第三条的核对结果还有一个副产品，很重要：

```
774:  auth = secureCall(AUTH_AND_RESET, ..., dtb_pas_id, 0,0,0);
775:  if (!scm_ok) rc = -3;
778:  if (rc) {
779:      printf("... the modem device tree blob was refused (scm %u/%u)", gOwner.scm_x0, gOwner.scm_x1);
780:      shutdownModem(true, false);      // 在 printf 之后，不污染上面的值
```

而 `authenticateHeader` 对 fixed 镜像只可能返回 -1（INIT 失败），因为 MEM_SETUP 被跳过（Probe:781-789）。
**所以 DTB 打印出来的 0xFFCFFFBB 确定来自 `PAS_INIT_IMAGE`，不是别的调用。这一步不需要再查。**

---

## 2. UMI 的真实流程（可逐行核对）

源码 `drivers/modem/hakios.mpss.c`。**目标机型写在文件第一行：POCO X5 Pro（redwood）**。

| 常量 | 值 | 行 |
| --- | --- | --- |
| `PAS_MPSS_DEFAULT` | 4（`dt: qcom,pas-id of qcom,mss`） | 1050 |
| `MPSS_MEMORY_DEFAULT` | `0x8b800000` | 1047 |
| `MPSS_MEMORY_SIZE` | `0x0f600000` = 246 MiB | 1049 |

步骤表（6182）：`step_survey, step_firmware, step_mailbox, step_header, step_image, step_our_item, step_start, ...`

```
1466 step_header:
 1501   scm(PAS_INIT_IMAGE, arginfo=0x82, pas=4, kvtophys(page), 0, 0)
 1509   scm(PAS_MEM_SETUP,  arginfo=3,    pas=4, mpss_memory, MPSS_MEMORY_SIZE, 0)

1573 step_image:
 1591   segment_put(...)        // 分段写进 0x8b800000
 1596   segment_check(...)      // 回读校验

2415 step_start:
 2436   IPCC_ENABLE (2<<16 | 2) // 门铃打开
 2448   rmtfs_share(true)       // assign_mem（坏的，见 §1）
 2451   rails(384)
 2456   mailbox_send("load_state modem on")
 2462   scm(PAS_AUTH_AND_RESET, arginfo=1, pas=4, 0,0,0)
```

**关键**：1236 行 `if (... || lowest != mpss_memory || ...)` 要求最低段地址**精确等于** `mpss_memory`。
所以 UMI 的镜像是 **fixed**（不是 relocatable）——而它**照样**调了 `PAS_MEM_SETUP`，并且成功了。

这与本移植 `FuxiModemPas.h` 的规则（fixed 不调 MEM_SETUP）**相反**。但两者来自不同的上游路径：
UMI 对应 `qcom_mdt_load()`（无条件 MEM_SETUP），fuxi 对应 `qcom_q6v5_pas`（按 relocate 决定）。
**本移植跟的是后者，对 SM8550 是对的。UMI 在这里只能证明 fixed + MEM_SETUP 在 SM8250 的 secure world 上不违法。**

---

## 3. 逐项差异（只列可能影响 PAS 的）

| 维度 | UMI（SM8250, qcom,mss） | fuxi（SM8550, qcom_q6v5_pas） | 能否解释拒绝 |
| --- | --- | --- | --- |
| 镜像数 / id 数 | 1 / 1 | 2 / 2（4 与 0x26） | 结构不同，非直接原因 |
| DTB 阶段 | **不存在** | 先走，且**正是它被拒** | **fuxi 独有的阶段** |
| metadata 来源 | `.mdt` header | `.mdt` header（已改） | 否（实测 0/4044 差异） |
| metadata 分配 | `kmem_alloc` + `dc civac` | `inTaskWithPhysicalMask` + `dc civac` | 否（ADSP 同法被接受） |
| metadata 地址宽度 | 未约束 | 未约束（>4 GiB） | 否（矩阵：相邻地址一对一拒） |
| `PAS_INIT_IMAGE` arginfo | `0x82` | `qcom_scm_args(2,VAL,RW)` = `0x82` | 否（同形） |
| `PAS_MEM_SETUP` size | 整段 246 MiB | 主镜像 span **==** 整段 0x10800000 | 否（主镜像两者相等） |
| fixed 是否调 MEM_SETUP | **调** | **不调** | **假设来源，见 §4** |
| **启动前 assign_mem** | 有（形状错、未验证） | **无** | **假设来源，见 §4** |
| carveout UEFI 属性 | AP 侧无 no-map | `HobOnlyNoCacheSetting`+`UNCACHEABLE` | **否**（ADSP 同属性却成功） |
| IPCC / AOP / SMP2P 时机 | 全在 AUTH 前 | 全在 PAS 后 | 否（只影响 reset 之后） |

主镜像 span 的核对（这是上一轮可能被误判的地方）：
`evidence/2026-11-19-modem-mdt-metadata.md` 记录主镜像 `276824064 byte span`，而 `0x10800000 = 276824064`。
**两者完全相等**——所以对主镜像，`qcom_mdt_span()` 恰等于整段 carveout。MEM_SETUP 的范围**不是**差异。

---

## 4. 矩阵里被忽略的信号：只有 "(adsp_dtb, 36)" 被接受

`evidence/2026-11-19-modem-pas-matrix.md` 的表：

| image | pas 38 | pas 36 |
| --- | --- | --- |
| modem_dtb | 0xFFCFFFBB | 0xFFCFFFBB |
| adsp_dtb | **0xFFCFFFBB** | **0/0 接受** |

之前的记录把它读成"拒绝跟着镜像走"。但表里还有一行没被用上：**同一个 ADSP 镜像交给 38 也被拒**。
所以严格地说，被接受的不是"adsp 镜像"，而是**唯一那一个 (镜像, id) 对**。

加上两次 sweep 的结论（全部 21 个 id 都拒 modem 的 blob），可以得到一个更紧的陈述：

> **36 是唯一接受过任何东西的 id；而那一次启动里，36 正是真正跑完了整条 PAS 序列的那个 id**（`fuxi-adsp=1`）。

如果是这样，`INIT_IMAGE` 就不是"纯静态校验 metadata"——它依赖某个只有走完整条序列的 id 才具备的 secure-world 状态。
而本移植与上游在这一点上的差异，正好记录在自己的 manifest 里：

```
facts/baseband/manifest.toml:47  [linux.qcom_q6v5_pas.memory_assignment]
  sm8550_mpss_resource 有 region_assign_idx 2, region_assign_count 1,
  region_assign_vmid QCOM_SCM_VMID_MSS_MSA (0xF)，无 region_assign_shared。
  -> qcom_pas_assign_memory_region 把第 3 个 reserved-memory（mpss_dsm_mem, 0xd4d00000, 0x3300000）
     交给 0xF。它在 probe 里跑，after qcom_pas_alloc_memory_region，before qcom_pas_init_clock，
     也就是 before any PAS call。
  ADSP 的 entry 没有 region_assign_idx。
```

**这个假设解释了目前所有观测：**

| 观测 | 假设下的解释 |
| --- | --- |
| modem blob 在**全部** 21 个 id 下被拒 | 没有任何 id 建立过 modem 的内存归属；归属是 per-remote 的，不是 per-id |
| adsp blob 在 36 下被接受 | 36 是本次启动里被真正建立过归属的那个 remote |
| adsp blob 在 38 下被拒 | 38 没有归属状态 |
| 两块板（modem_a / modem_b）都被拒 | 归属问题与固件内容无关 |
| 错误码全是同一个 0xFFCFFFBB | 同一处检查在失败 |
| ADSP 的成功不能证明这一步可省 | ADSP 本来就没有这一步（manifest 已记录） |

**这不是已证实的结论，是可证伪的假设。** 判据很干脆：

> 若把 `mpss_dsm_region` 正确交给 VMID 0xF 之后，`INIT_IMAGE(modem_dtb, 38)` 从 0xFFCFFFBB 变成 0/0，假设成立；
> 若仍是 0xFFCFFFBB，则"缺 assign_mem" 被彻底排除，剩下的唯一结构差异（fixed vs relocatable）上位。

---

## 5. 为什么这一轮没有直接改代码

`qcom_scm_assign_mem` 的正确形状在 manifest 里已经记录（`assign_mem_argument_shape`：7 个参数、RO/VAL 交替、
SVC_MP 0x0c / MP_ASSIGN 0x16、参数块必须是 secure world 能读的内存）。但实现它需要：

1. 一块 4 KiB、物理连续、可被 secure world 读的缓冲区（端口已有 `inTaskWithPhysicalMask` + `cleanToPoc` 的机制）；
2. 七个寄存器参数的**正确**排布——**UMI 那份是错的，不能抄**；
3. 正确的 VMID。`0xF` 来自 manifest（`QCOM_SCM_VMID_MSS_MSA`），与 UMI 的 `VMID_MSS_MSA 15`（=0xF）一致，可交叉确认。

这三点都够清楚，但**本机无法编译也无法上机**，而写进内核的 SMC 调用一旦参数错位，后果和 UMI 那份一样是
静默无效果（甚至可能把 secure world 的状态弄得更脏）。按"不凭猜测修改代码"的原则，本轮**不改代码**。

---

## 6. 建议的下一项验证（只做这一项）

**V1：实现 `mpss_dsm_region` 的 `assign_mem`，然后只测 `INIT_IMAGE(modem_dtb, 38)`。**

| 项 | 值 | 出处 |
| --- | --- | --- |
| region | `0xd4d00000`, size `0x3300000` | `MemoryMapLib.c:65` `mpss_dsm_region` |
| 源 vmid | `3`（HLOS） | manifest `QCOM_SCM_VMID_HLOS` |
| 目的 vmid | `0xF`（MSS_MSA） | manifest `memory_assignment` |
| 权限 | `QCOM_SCM_PERM_RW` | 同上 |
| 调用 | SVC_MP `0x0c` / cmd `0x16`，x1 arginfo 七个 RO/VAL 交替 | manifest `assign_mem_argument_shape` |
| 时机 | **任何 PAS 调用之前** | 同上 |

新增一个 mode（例如 `fuxi-modem=14`）只做：assign -> `INIT_IMAGE(modem_dtb, 38)` -> 打印 x0/x1。
**不 MEM_SETUP、不写段、不 AUTH_AND_RESET、不碰 RF。** 一次上机就能判定 §4 的假设。

若 V1 被否（仍是 0xFFCFFFBB），下一个才是 fixed-vs-relocatable：对 fixed DTB 也发 `PAS_MEM_SETUP`，
size 用 `kModemDtbCarveoutSize`（0x80000），与 UMI 的"整段"做法对齐。

---

## 7. 本轮产出

- 只做阅读与对照，**未修改任何源码、未构建、未上机**。
- 更正了上一轮的三条判断（§1）。
- 撤回上一轮"UMI 的 assign_mem 可直接借用"的建议。
- 新增一条可证伪的假设与它的判据（§4、§6）。