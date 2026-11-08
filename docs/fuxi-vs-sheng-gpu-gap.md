# fuxi 与 sheng 的 GPU 差异清单（2026-11-08）

这份文件只记录**可以查证的事实**和**已经排除的假设**，不写推断当结论。
每条事实都给出文件与行号，或给出日志读数。

背景：fuxi 上 Metal 已经能用（`fence submitted 48 retired 48`、`gop offers 16 done 16`），
但为此启用了四个本该不必要的补偿。sheng 在同一条驱动上、不启用任何一个，也能正常工作。
本文件回答"差在哪、哪些已排除、还剩什么"。

---

## 1. 已验证：sheng 一个补偿都不需要

sheng 的板级探针 `package/GpuPowerProbe.cpp` 对这些全局开关的引用次数：

| 全局开关 | sheng | fuxi | fuxi 设置位置 |
| --- | --- | --- | --- |
| `native_adreno_no_thread_sync` | 0 | 1 | `package/FuxiGpuPowerProbe.cpp:1702` |
| `native_adreno_bv_to_br` | 0 | 1 | `:1703` |
| `native_adreno_drop_wait_mem` | 0 | 1 | `:1704` |
| `native_adreno_plain_submit` | 0 | 1 | `:1711` |
| `native_adreno_bv_ring` | 0 | 0（已确认有害，关闭） | — |
| `native_adreno_single_table` | 0 | 由启动参数 `sheng-gpu-single-table=1` 打开 | `board.toml` |
| `native_adreno_wc_enabled` | 1（保持开启） | 0（`sheng-gpu-wc=0`） | `package/FuxiGpuPowerProbe.cpp:1342` |

**结论**：差异不在驱动行为，而在板级环境。驱动是同一份代码。

## 2. 已验证：两边启动参数的差异

| | sheng | fuxi |
| --- | --- | --- |
| GPU 参数 | `sheng-gpu-power=9`、`sheng-gpu-metal=1`、`sheng-fb-gpu=1`、`sheng-fb-bgra=1`、`sheng-dpu-scanout=2`、`sheng-gpu-dcvs=1`、`sheng-gpu-addon=1` | 上述的 `power/metal/fb-gpu/dcvs/addon` 相同，另有 `sheng-gpu-zap=4`、`sheng-gpu-single-table=1`、`sheng-gpu-wc=0`、`sheng-tu-debug=sysmem,nocb,nobin`；**没有** `sheng-fb-bgra`、`sheng-dpu-scanout` |
| 生效的 `TU_DEBUG` | `sysmem,nocb`（由插件默认值 + `sheng-fb-bgra=1` 得出） | `sysmem,nocb,nobin`（被 `sheng-tu-debug` 覆盖） |
| 页表 | 每进程表 | 单表（内核表） |
| 客户端 BO 的 CPU 映射 | 可写合并（write-combined） | 非写合并 |

关键差别是 **`nobin`**：sheng 跑 binning（BV 线程工作），fuxi 关掉了它。

## 3. 已排除的假设

每一条都是"看起来最像答案"的那种，逐条核查后不成立。

| 假设 | 核查方法 | 结果 |
| --- | --- | --- |
| `prepare-sheng-gpu-smmu.py` 生成的 SID/bank 常量 fuxi 没有 → 内存路径错 | 从 fuxi DTB 取出实际绑定，与 `NativeAdrenoGpuContextBringup` 实参对照 | **一致**。DT：`kgsl-smmu@3da0000`、`#iommu-cells=2`、TBU `stream-id-range=<0x00 0x400>`；两边调用均为 `(map, win, bank=0, asid=2, slot0=0, sid0=0, slot1=1, sid1=1)` → **bank 0、SIDs {0,1}**，与 sheng 的 `gpu_context_bank=0, gpu_sids=[0,1]` 相同 |
| `owned_coherent` 未设 → 自有内存被当写合并 → 写入不落地 | 查两边板级参数与引用 | **两边都默认 0**：任何 `board.toml` 都没有 `sheng-gpu-owned-coherent`，所以 sheng 也走写合并。**不是差异** |
| `ACTLR` 未编程 → SMMU 内存属性错 | 查两边探针 | 两边都实现并调用 `gpuSetActlr()`（sheng `:431/:1670`，fuxi `:401/:1613`）。**不是差异** |
| 探针写的 `CP_*` 寄存器集合不同 | 提取两边 `gpuWr(A740_CP_*)` 集合并做差 | 各 10 个，**完全相同**，差集为空 |
| `walk_coherent` 未证明 → 表遍历读到脏描述符 | 查引用 | fuxi=0、sheng=5。fuxi 不设 → 驱动**每次都清表页**，比 sheng 更保守，只会更慢，不会更错。**不是故障源** |
| BV 环没编程（历史上把偏移写成 `0xa80..0xa87`） | 写探测 | 那段读 `0xdeafbead`、拒写，等于从未编程；但按真实偏移（BR + `0x260`）编程会让 bring-up 直接停死（`fence live 0`）。**该路已放弃** |

## 4. 仍然开放的问题

**一条提交永不兑现的根因尚未定位。** 已确认的停点特征：

- `CP_IB1_BASE` 指向客户端 IB，`CP_IB1_REM_SIZE = 0` → CP 把 1000 字节**全部取完**；
- ring 的 `wptr 79 / rptr 72` → CP **没有回到 ring 继续**；
- IB2 / SDS / MRB / VSD 的剩余量都是 0 → 不在 draw-state、可见性流、memory-read 路径里；
- `CP_PROTECT` 关掉后计数不变 → 不是寄存器保护丢写；
- 流末尾是 `CP_EVENT_WRITE {0x21}{0x20}{0x31}` + `CP_WAIT_MEM_WRITES`。

把这些等待改写为 `CP_NOP` 后一切正常（单次启动改写 107 个），所以**停点就在这个写屏障**。
但**为什么它等的写永不完成，没有定位**。

可疑点已收敛到同一族的两件事，都与"客户端内存如何被描述给 GPU"有关：

1. 单表模式下客户端 BO 落在**内核表**里，属性由内核表决定；sheng 是每进程表，属性由客户端自己的表决定。
2. **写合并映射在本板不落地**——这正是被迫 `wc=0` 的原因。

"写不落地"与"写完成等不到"很可能是同一属性问题的两个表现。**这是假设，未经证实。**

## 5. 四个补偿的依赖关系与撤回判据

它们目前互相掩盖，不能一次全撤。

| 撤回项 | 前提 | 判据 |
| --- | --- | --- |
| `sheng-gpu-single-table=1` → 每进程表 | 切表能成功 | `tbl_switches` 增长，客户端能用自己表 |
| `sheng-gpu-wc=0` → 恢复写合并 | 客户端写入能到 CP 读到的内存 | `peekWords` 读 IB 不再是全零 |
| `drop_wait_mem` → 恢复等待 | GPU 的 posted write 能完成 | `CP_IB1_REM_SIZE` 到 0 **且** `rptr` 继续前进 |
| `bv_to_br` / `nobin` | BV 线程可运行 | 不改写线程控制也能兑现 fence |

## 6. 关于 `prepare-sheng-gpu-smmu.py`：不需要补

它做的事：从 pinned 的 Linux `arm-smmu-qcom.c` 与 DT 平台 JSON **断言策略**并**生成常量头**，
内容为 SMMU 寄存器范围、`GPU_STREAMS=2`、`GMU_SID=5`、`sids={0,1}`，报告里明确写着
`hardware_observed=False`（描述策略，不观测硬件）。

**该头文件在 sheng 树里只被一个宿主测试引用**：

```
package/ShengGpuSmmuHostTests.c:3:  #include "ShengGpuSmmuSource.h"
```

设备端探针不引用它；SMMU 基址/窗口在设备上来自 loader 交接的 `gSmmuMap`。

所以：**补它不会改变任何运行行为，也不会免掉任何驱动修改。** 它的价值只是防止回归
（pinned 源码或 DT 一变就 fail）。要做可以做，但不要期待它修问题。

## 7. 建议的下一步

按代价从低到高：

1. **host 侧、不刷机**：把 `kits/ArmSmmu4K` 给客户端 BO 建的 PTE（属性位、AP、SH、AF）与
   每进程表路径下的取值逐项对比。既然 SID/bank/ACTLR 都已排除，剩下的就是页表项属性。
2. **单变量复测**：只关 `drop_wait_mem`，读 `CP_IB1_REM_SIZE`，确认停点仍是该屏障。
3. **恢复每进程表**：环境已与当初失败时大不相同，值得重试；成功则 `single-table` 可撤。
