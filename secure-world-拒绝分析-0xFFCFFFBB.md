# 0xFFCFFFBB：secure world 拒绝 modem blob 的对照分析

日期：2026-11-20。对象：fuxi（Xiaomi 13 / SM8550）集成的 MPSS 基带，PAS `INIT_IMAGE` 被拒。

**范围说明**：这是一份对照分析，不是新代码。结论来自三处一手源码的逐点比对：
本仓 `package/FuxiModemProbe.cpp` / `package/QcomMdt.c` / `package/QcomScm.h`；
上游 Linux 7.4-rc3（`/private/tmp/x13-dbg/tree/linux-7.4-rc3`）；
hakios-xnu-kernel-umi-fixes（`/tmp/hakios-work/`）。
设备侧结论引自本仓 `evidence/2026-11-19-modem-*.md` 与 `PORTING_MAP.md`，未新增真机实验。

---

## 1. 结论

**0xFFCFFFBB 是 secure world 对「这份固件的签名我不认」的回答，不是调用形状的问题。**
本仓的 PAS 调用形状已经被同一份代码里被接受的 ADSP blob 证明是对的（PAS 矩阵，2026-11-19）。

对照上游 Linux 7.4-rc3 与 hakios 的真机成功配方，fuxi 移植在 **PAS 序列的顺序上有一处确定的错误**，外加**两处上游做了、fuxi 没做的前置动作**。这三项都是「secure world 在认证前需要的前置状态」，与「拒绝镜像」的错误码形状一致。

下面逐项给出证据。

---

## 2. 上游的 PAS 序列（sm8550 MPSS）

### 2.1 顺序（`drivers/remoteproc/qcom_q6v5_pas.c`）

```c
qcom_pas_start(rproc):
    qcom_q6v5_prepare()              /* 中断、TCM */
    qcom_pas_pds_enable()             /* proxy pd: cx, mss */
    clk_prepare_enable(xo)
    clk_prepare_enable(aggre2_clk)
    regulator_enable(cx_supply)
    regulator_enable(px_supply)

    if (pas->dtb_pas_id) {
        qcom_pas_map_carveout(dtb_mem_phys, dtb_mem_size)     /* IOMMU map */
        qcom_pas_prepare_and_auth_reset(pas->dtb_pas_ctx)      /* DTB 的 AUTH_AND_RESET */
    }

    qcom_mdt_pas_load(pas->pas_ctx, pas->firmware, ...)        /* 主镜像的 INIT_IMAGE + MEM_SETUP + 搬分段 */
    qcom_pil_info_store(...)
    qcom_pas_map_carveout(mem_phys, mem_size)
    qcom_pas_prepare_and_auth_reset(pas->pas_ctx)              /* 主镜像的 AUTH_AND_RESET */
    qcom_q6v5_wait_for_start(5000 ms)
    qcom_pas_metadata_release(pas->pas_ctx)
    qcom_pas_metadata_release(pas->dtb_pas_ctx)
```

**DTB 的 AUTH_AND_RESET 在主镜像的 INIT_IMAGE 之前**，这是上游的确定顺序。

### 2.2 `qcom_mdt_pas_load` 只做两件事（`drivers/soc/qcom/mdt_loader.c:486`）

```c
int qcom_mdt_pas_load(ctx, fw, firmware, reloc_base)
{
    ret = __qcom_mdt_pas_init(ctx->dev, fw, firmware, ctx->pas_id, ctx->mem_phys, ctx);
    mem_region = qcom_pas_ctx_map(ctx);
    ret = qcom_mdt_load_no_init(ctx->dev, fw, firmware, mem_region,
                                ctx->mem_phys, ctx->mem_size, reloc_base);
}
```

`__qcom_mdt_pas_init`（:231）做的是：

```c
metadata = qcom_mdt_read_metadata(fw, &metadata_len, fw_name, dev);
ret = qcom_pas_init_image(pas_id, metadata, metadata_len, ctx);   /* INIT_IMAGE */
if (relocate)
    ret = qcom_pas_mem_setup(pas_id, mem_phys, max_addr - min_addr);  /* MEM_SETUP */
```

**没有 AUTH_AND_RESET。** 上游的注释写得很直白：

> Loads and authenticates the metadata of the firmware (typically contained in the .mdt file), followed by loading the actual firmware segments (e.g., .bXX files). **Authentication of the segments done by a separate call.**

所以上游一个 image 的完整序列是：

```
INIT_IMAGE → (MEM_SETUP，仅 relocatable) → 搬分段进 carveout → AUTH_AND_RESET
```

而 **DTB 走的是另一条**：上游的 DTB **不走 `qcom_mdt_load_no_init`**（分段不进 carveout），只有 INIT_IMAGE（+ MEM_SETUP），然后立刻 AUTH_AND_RESET。DTB 的 carveout 由 `qcom_pas_map_carveout` 映射给 IOMMU，不搬数据。

### 2.3 pas id（`qcom_q6v5_pas.c:1740`）

```c
static const struct qcom_pas_data sm8550_mpss_resource = {
	.firmware_name = "modem.mdt",
	.dtb_firmware_name = "modem_dtb.mdt",
	.pas_id = 4,
	.dtb_pas_id = 0x26,          /* 38 */
	.crash_reason_smem = 421,
	.proxy_pd_names = {"cx", "mss", NULL},
	.region_assign_idx = 2,
	.region_assign_count = 1,
	.region_assign_vmid = QCOM_SCM_VMID_MSS_MSA,
};
```

**fuxi 用的 pas id 4 / 0x26 与上游一致**，这一点 PORTING_MAP 已经确认，无误。

### 2.4 carveout（`arch/arm64/boot/dts/qcom/sm8550.dtsi`）

```
mpss_mem:       mpss-region@8a800000       reg = 0x8a800000 0x10800000
q6_mpss_dtb_mem: q6-mpss-dtb-region@9b000000 reg = 0x9b000000 0x80000
mpss_dsm_mem:    mpss-dsm-region@d4d00000   reg = 0xd4d00000 0x3300000
```

**fuxi 的 `kModemCarveoutBase/Size` 与 `kModemDtbCarveoutBase/Size` 与上游逐字节一致**，无误。

---

## 3. fuxi 移植与上游的三处差异

### 3.1 差异一：DTB 的 AUTH_AND_RESET 位置错了（顺序问题）

**fuxi**（`package/FuxiModemProbe.cpp:768`）：

```c
int rc = authenticateHeader(gOwner.dtb, dtb_pas_id, kModemDtbCarveoutBase);  /* INIT_IMAGE (+MEM_SETUP) */
if (!rc) rc = loadSegments(gOwner.dtb, kModemDtbCarveoutBase, kModemDtbCarveoutSize);
if (!rc) {
    const ScmResult auth = secureCall(AUTH_AND_RESET, ..., dtb_pas_id, 0, 0, 0);
}
```

**上游**：`INIT_IMAGE` → `AUTH_AND_RESET`，DTB 的分段**不搬**。

两处不同：
- fuxi 把 DTB 的 AUTH_AND_RESET 放在搬分段之后；上游放在 INIT_IMAGE 之后立刻做。
- fuxi 给 DTB 搬了分段；上游不搬（DTB 的 carveout 走 `qcom_pas_map_carveout`）。

**为什么这可能是拒绝的原因**：DTB 是 **fixed** 镜像（段地址 0x9b000000），`qcom_mdt_place` 对 fixed 镜像按绝对地址放置。上游对 fixed 镜像**根本不做 MEM_SETUP**（`if (relocate)` 才做），而 fuxi 的 `fuxi_modem_pas_header_steps(relocatable ? 1 : 0, ...)` 同样只对 relocatable 做 MEM_SETUP——这一点 fuxi 是对的。

但上游的 DTB AUTH_AND_RESET 之后**没有搬分段**这一步；fuxi 在 INIT_IMAGE 与 AUTH_AND_RESET 之间插了一次分段写入。这意味着 fuxi 在 secure world 还没认证 DTB 时，就把 512 字节写进了 0x9b000000。这块内存在 `no-map` 的保留区里，但 secure world 对这块区域的视图此时未必是 AP 的视图。

**判定**：顺序差异是**代码事实**，是否就是拒绝的原因**未在设备上验证**。这是本轮最该先试的一项，因为改动小、语义明确。

### 3.2 差异二：`qcom_pas_assign_memory_region` 没做

上游 `sm8550_mpss_resource` 有 `region_assign_idx = 2, region_assign_count = 1, region_assign_vmid = QCOM_SCM_VMID_MSS_MSA`。
`qcom_pas_assign_memory_region`（`qcom_q6v5_pas.c:776`）在**任何 PAS 调用之前**把第三个保留内存区（`mpss_dsm_mem`，0xd4d00000，0x3300000）用 `QCOM_SCM_ASSIGN_MEM` 交给 modem 的 VMID（0xF）。

```c
static int qcom_pas_assign_memory_region(struct qcom_pas *pas)
{
	if (!pas->region_assign_idx) return 0;
	for (offset = 0; offset < pas->region_assign_count; ++offset) {
		ret = of_reserved_mem_region_to_resource(pas->dev->of_node,
							 pas->region_assign_idx + offset, &res);
		...
		perm[0].vmid = pas->region_assign_vmid;   /* MSS_MSA */
		...
		ret = qcom_scm_assign_memory_region(phys, size, &perm);
	}
}
```

**fuxi 没有这一段。** PORTING_MAP 已经把它列为「上游两步本仓没做」的第一条。

**为什么这可能是拒绝的原因**：secure world 在 `INIT_IMAGE` 时要认证 hash 段并与安全启动链上的信任根比对。这条信任链上，modem 的内存归属（DSM 区归 MSS_MSA）通常是在认证**之前**建立的。如果归属没建立，secure world 可能认为这份固件没有被授权到这个 VMID，直接拒。

**注意 ADSP 没有这一步**（`sm8550_adsp_resource` 无 `region_assign_idx`），这解释了为什么 ADSP 的 blob 被接受而 modem 的被拒——**两者唯一的结构性差别就是 modem 有 region assign 而 ADSP 没有**。这个对应关系目前只是巧合般的相关，不是因果证明，但它是目前唯一能解释「ADSP 接受、modem 拒绝」而其它全部维度都相同的差别。

**判定**：**代码事实**，且**是当前最能解释 PAS 矩阵结果的假设**。

### 3.3 差异三：`qcom_scm_clk_enable` / `qcom_scm_bw_enable` 没做

上游每一个 PAS 调用前后都包着：

```c
ret = qcom_scm_clk_enable();     /* SCM core + interface clocks */
ret = qcom_scm_bw_enable();      /* SCM bus bandwidth */
...
qcom_scm_bw_disable();
qcom_scm_clk_disable();
```

`INIT_IMAGE` / `MEM_SETUP` / `AUTH_AND_RESET` 三处都有（`drivers/firmware/qcom/qcom_scm.c`）。

**fuxi 没有。** PORTING_MAP 也记了这条。

**判定**：代码事实。可能性低一些——上游的 clk/bw 是为了让 SCM 访问内存带宽够用，不是认证的前提；而且 fuxi 的 ADSP blob 在**没有** clk/bw 的情况下被接受了，说明 clk/bw 不是「能不能认证」的门槛。但它可能是「认证到哪一步」的差别（比如读 hash 段时数据没到位）。

**注意 hakios 也没有**——它的 MPSS/WPSS/ADSP/GPU 四个 DSP 都跑通了 PAS 且没有 clk/bw enable。所以这条可以排在最后试。

---

## 4. 已经排除的（不要重复试）

PORTING_MAP 与三份 evidence 已经把这些排干净了：

| 试过的 | 结果 |
|---|---|
| metadata 取自 `.mdt` 而非 joined image | 仍 0xFFCFFFBB；且两种形式 4044 字节中 0 字节不同（mode 11 实测） |
| metadata 地址宽度 / 高于 4 GiB | 不是理由：adsp_dtb 在相邻地址 0x8d0b1f000 被接受 0/0 |
| 21 个被 secure world 接受的 pas id | 全部拒绝 modem 的 blob，错误码相同 |
| 两个槽（modem_a / modem_b）的各自 blob | 都被拒；两个槽是不同的构建（95 字节不同） |
| 把 blob 的分段先搬进 0x9b000000 再 INIT_IMAGE（mode 7） | 仍 0xFFCFFFBB |
| INIT_IMAGE 的 pas id 换成 ADSP 的 36 | 仍拒绝；adsp_dtb + 36 接受 |
| blob 结构自洽性 | modem 512 = 452+60，adsp 8376 = 8316+60，都自洽；头都是 `43 46 47 4c` |

**关键的一点**：mode 7 已经试过「先搬分段再 INIT_IMAGE」，而 fuxi 现在的顺序是「INIT_IMAGE → 搬分段 → AUTH_AND_RESET」。也就是说 **fuxi 当前代码里 INIT_IMAGE 前面没有任何前置动作**——这正是差异二（region assign）和差异三（clk/bw）缺失的后果：secure world 在收到 INIT_IMAGE 时，它要的内存归属还没建立。

---

## 5. 对照 hakios：另一条真机跑通的路

hakios-xnu-kernel-umi-fixes 的 `drivers/modem/hakios.mpss.c`（6395 行，redwood / POCO X5 Pro）走的是**集成 MPSS + PAS** 路线，2026-10-21 在真机上跑通：

> шаги 1–10 проходят: прошивка `modem.*` из `/fw` системного образа (85 МБ, 28 сегментов) записана в `0x8b800000` и прочитана обратно без расхождений; **TrustZone принял подпись (образ 4)** и область `0xf600000` байт

它的 PAS 序列（`hakios.mpss.c:2415` `step_start`）：

```
step_firmware   解析 modem.mdt + 28 个 .bNN，校验 header/hash/段范围
step_header     INIT_IMAGE（metadata 放在自己的一页里，dc civac 清到 PoC）
                → MEM_SETUP（base, size）
step_image      把分段写进 modem 的内存，读回核对
step_start      rails(384) → AOP mailbox "modem: on" → AUTH_AND_RESET
```

**与上游、与 fuxi 都一致**：INIT_IMAGE → MEM_SETUP → 搬分段 → AUTH_AND_RESET。
**与 fuxi 不同**：hakios 的 **INIT_IMAGE 在搬分段之前**，而且它**没有** region assign（它的板子不是 sm8550）。

hakios 也没有 `qcom_scm_clk_enable` / `bw_enable`——这削弱了差异三的权重。

### 5.1 一个值得注意的差别：metadata 的放置

hakios 的 `step_header`：

```c
announce("hand the image's header to TrustZone to check its signature (image 4), in a page of ours");
if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) ...
memcpy((void *)page, image.header, image.header_size);
for (vm_offset_t line = page; line < page + PAGE_SIZE; line += 64)
	__asm__ volatile ("dc civac, %0" : : "r" (line));
settle();
result = scm("to take the modem's header", SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, pas_mpss, kvtophys(page), 0, 0, &answer);
```

- metadata 放在**整页**（PAGE_SIZE）里，只拷贝 `image.header_size` 字节，其余为零
- `dc civac` 逐 64 字节清到 PoC
- `settle()`（应该是 dsb）

fuxi 的对应：

```c
image.meta = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, InOut | PhysicallyContiguous, bytes, 0xfffffffffffff000ull);
uint8_t *va = image.meta->getBytesNoCopy();
buildMetadata(image, va, bytes);
cleanToPoc(va, bytes);
const uint64_t phys = image.meta->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
```

- 分配 `bytes`（4044）字节，**不是整页**
- `cleanToPoc` 用 `ctr_el0` 算 cache line 尺寸逐行清

差异：fuxi 的缓冲是 4044 字节、非 4 KiB 对齐的**连续分配**（物理连续但不页对齐）。上游要求：

> During the scm call memory protection will be enabled for the meta data blob, so make sure it's physically contiguous, **4K aligned** and **non-cachable** to avoid XPU violations.

fuxi 的 `0xfffffffffffff000` 掩码**只保证页对齐的分配粒度，不保证返回地址页对齐**——地址 0x8d3a3b000 恰好是页对齐的（末尾 000），但这是运气。hakios 用整页分配保证了这一点。

**判定**：fuxi 的分配**可能**不满足 4 KiB 对齐（当前几次实测恰好对齐），并且是 cacheable 而非 non-cacheable（PORTING_MAP 已记）。这两点上游都明确要求。

---

## 6. 建议的验证顺序

按「改动小 → 改动大」排，每步一次真机：

| 顺序 | 动作 | 依据 | 改动量 |
|---|---|---|---|
| 1 | **加 region assign**（`QCOM_SCM_ASSIGN_MEM`，mpss_dsm_mem 0xd4d00000/0x3300000，VMID 0xF），在任何 PAS 调用之前 | 上游 `sm8550_mpss_resource` 明确有；ADSP 没有且被接受、modem 有且被拒——唯一能区分两者的结构性差别 | 中（要新的 SCM 命令 + 安全内存参数块） |
| 2 | **DTB 改成上游顺序**：INIT_IMAGE → AUTH_AND_RESET，不搬分段 | 上游 `qcom_pas_start` 的确定顺序 | 小 |
| 3 | **metadata 改成整页分配**（4 KiB 对齐、页内其余清零） | 上游「physically contiguous, 4K aligned and non-cachable」；hakios 的成功配方 | 小 |
| 4 | 加 `qcom_scm_clk_enable` / `bw_enable` 包住 PAS 调用 | 上游每个 PAS 调用都有 | 中（要 SCM 时钟/带宽寄存器） |

理由：1 和 2 都能解释「ADSP 接受、modem 拒绝」，而 3 和 4 与 ADSP/modem 的差别无关（ADSP 已经在同样的代码下被接受），放在后面。

**每一步都只发 `INIT_IMAGE`/`MEM_SETUP`/`AUTH_AND_RESET`，不释放任何处理器、不写 RF。**

---

## 7. 一个尚不能排除的假设

`region_assign` 那条有一个细节需要核对：`QCOM_SCM_ASSIGN_MEM` 的参数块本身需要**安全内存**。

上游 `qcom_pas_assign_memory_region` 把参数块放在 SCM 的 tzmem 池（`qcom_tzmem_alloc`），由 TrustZone 内存分配器支撑，平台支持时由 SHM bridge 承载。

fuxi 这边：
- `remoteproc_mpss` 节点**没有 `iommus` 属性**（`sm8550.dtsi`），`needs_tzmem` 未设 → 上游在这块板上也走 `dma_alloc_coherent`
- 但**这是 PAS metadata 的路径**。region assign 的参数块在上游代码里无条件走 tzmem

也就是说，做 region assign 需要 XNU 里拿到一块「安全内存」（能被 hypervisor 建 SHM bridge 的内存）。XNU 没有 tzmem 的等价物，`NativeAdrenoMemory.cpp` 的固定低 DRAM 映射是唯一已验证的可行机制，但它只有 64 KiB 且被音频链占用。

**这是这一步的主要成本所在，不是逻辑难点。**

---

## 8. 与 hakios 基带档案的关系

`hakios-umi-baseband-档案.md` 讲的是 **外置 SDX55（PCIe）** 路线，与本文件讲的 **集成 MPSS（PAS）** 路线是两回事，不能互相套用。但有一处共通，值得记下：

hakios 的 `hakios.mpss.c` 里，PAS 与 SCM 的实现是从 `hakios.wifi.c`（WPSS）和 `hakios.adsp.c`（ADSP）**逐行抄来再换数字**的，两个 DSP 都跑通了。这说明 **PAS + SCM 的调用形状本身在 XNU 上是可以做对的**——hakios 的 WPSS `SCM_PAS_INIT_IMAGE` 用 `SCM_VALUE_AND_BUFFER`、PAS id 6，ADSP 用 PAS id 1，都在真机上被 secure world 接受。

fuxi 与 hakios 的 SCM 编码逐字节一致：

| | hakios | fuxi |
|---|---|---|
| SMC64 | `1ull << 30` | `1u << 30` |
| SiP owner | `2ull << 24` | `2u << 24` |
| service | `2`（PIL） | `QCOM_SCM_SVC_PIL 0x02u` |
| INIT_IMAGE arginfo | `2 \| 2<<6` = 130 | `qcom_scm_args(2, VAL, RW, 0)` = 130 |
| MEM_SETUP arginfo | `3` | `qcom_scm_args(3,0,0,0)` = 3 |
| AUTH_AND_RESET arginfo | `1` | `qcom_scm_args(1,0,0,0)` = 1 |

所以 **fuxi 的 SCM 编码层没有嫌疑**——与一个真机跑通的实现逐位相同。

---

## 9. 引用

**上游（Linux 7.4-rc3）**
- `drivers/remoteproc/qcom_q6v5_pas.c:1740` `sm8550_mpss_resource`
- `drivers/remoteproc/qcom_q6v5_pas.c:265-400` `qcom_pas_start`
- `drivers/remoteproc/qcom_q6v5_pas.c:776` `qcom_pas_assign_memory_region`
- `drivers/soc/qcom/mdt_loader.c:161` `qcom_mdt_read_metadata`
- `drivers/soc/qcom/mdt_loader.c:231` `__qcom_mdt_pas_init`
- `drivers/soc/qcom/mdt_loader.c:486` `qcom_mdt_pas_load`
- `drivers/firmware/qcom/qcom_scm.c` `qcom_scm_pas_init_image` / `mem_setup` / `auth_and_reset`
- `arch/arm64/boot/dts/qcom/sm8550.dtsi:911,916,1001,3118` 保留内存与 remoteproc_mpss

**本仓**
- `package/FuxiModemProbe.cpp:449-497` `authenticateHeader`
- `package/FuxiModemProbe.cpp:763-800` `stepPas`
- `package/FuxiModemProbe.cpp:87-90` carveout 常量
- `package/QcomMdt.c:13-70` `qcom_mdt_parse`
- `package/QcomMdt.c:76-101` `qcom_mdt_metadata_from_mdt`
- `package/QcomScm.h:26` `qcom_scm_args`
- `package/FuxiModemPas.h` PAS 序列
- `evidence/2026-11-19-modem-pas-matrix.md`
- `evidence/2026-11-19-modem-mdt-metadata.md`
- `evidence/2026-11-19-modem-slot-survey.md`
- `PORTING_MAP.md:140-200, 300-360`

**hakios-xnu-kernel-umi-fixes**
- `drivers/modem/hakios.mpss.c:1465-1520` `step_header`
- `drivers/modem/hakios.mpss.c:2415` `step_start`
- `drivers/modem/hakios.mpss.c:1298` `scm()`
- `docs/ru/cellular.md` 2026-10-21 真机记录
