# fuxi GPU 上游干净化方案

## 结论

可以做到 **Adreno740、AdrenoMetal 和 GopFramebuffer 三个组件始终 checkout 官方提交，fuxi 不再维护它们的私有分支**。

但当前不能直接把 Adreno740 切到现有官方 `main`。fuxi 仍依赖共享地址空间和无 framing 提交；官方驱动没有这两个通用策略入口。正确的迁移方法是：

1. 删除只用于观察的 AdrenoMetal/GOP 私有提交，立即回到官方版本；
2. 把 Adreno740 里真正通用的修复合入上游；
3. 给 Adreno740 上游增加一个稳定、通用的 board policy 接口，fuxi 只传策略数据；
4. 把客户端命令流的改写移到 Turnip 的 Darwin 后端，内核驱动不再扫描和修改 IB；
5. 最后修复 fuxi 的页表切换、BV 和 posted-write 根因，再撤掉 policy 本身。

这样短期达到的是“**零私有 GPU 组件提交**”；长期根因修好后达到“**完全使用默认驱动路径**”。这两件事不能混为一谈。

## 当前边界和证据

2026-11-08 检查到的官方 `main`：

| 组件 | fuxi 当前提交 | 官方 `main` | 当前私有差异 |
| --- | --- | --- | --- |
| Adreno740 | `6106a9553682272287c5e6035a17fd650034f7b5` | `6fa7bae2a5b3b523e790eb3e0348af4d98969e4d` | 6 个提交，433 行增加、48 行删除 |
| AdrenoMetal | `90c60b8442d027b489521de2de5289ad190dc389` | `6d7f73437087c57ee54a41adceb1735daaea405e` | 设备打开路径的诊断 marker |
| GopFramebuffer | `af8b7da041ee61e0047ee5038ee39b045807fb45` | `e47c1b8de169cd2bd0b885c7d85e433010e3cdf2` | presenter tunnel 诊断计数 |

现有成功基线是 `submitted 48 / retired 48`、GOP `16/16`。它依赖：

- 共享内核页表；
- plain submission；
- 客户 IB 中 `CP_THREAD_CONTROL` 改成 BR-only 并去掉 `SYNC_THREADS`；
- 将 `CP_WAIT_MEM_WRITES` 改成 NOP；
- write-back CPU 映射；
- `TU_DEBUG=sysmem,nocb,nobin`。

因此，现在只改三个 revision 到官方 SHA 会导致功能回退，而不是一次安全更新。

## 目标所有权

| 内容 | 归属 | fuxi 是否维护驱动 fork |
| --- | --- | --- |
| GPU 电源、寄存器、固件、板级策略值 | `unixnu-fuxi/package/FuxiGpuPowerProbe.cpp` | 否 |
| `sysmem,nocb,nobin` 等运行策略 | `unixnu-fuxi/board.toml` | 否 |
| 通用 publish/submit policy ABI | 官方 Adreno740 | 否；一次合入上游后只跟随版本 |
| Darwin Turnip 对 capability 的实现 | UniXNU 的 Mesa patch 队列 | 否；不在 Adreno740/AdrenoMetal/GOP 分支中 |
| PM4/页表的通用正确性修复 | 对应官方组件 | 否；必须 upstream |
| fuxi bring-up 诊断 | `unixnu-fuxi` | 否；不得留在共享组件中 |

不要把 Adreno740 patch 放进 XNU 的 `[kernel].patch_series`。现有 series 只应用到 XNU 工作树，不会应用到组件源码。

## Adreno740 上游接口

### 1. 先合入通用正确性修复

将本地 `b1bd99d` 拆成独立上游提交：

- `CP_SMMU_TABLE_UPDATE` payload 从 4 dword 修正为 2 dword；
- `RingTableWords` 从 7 改为 5；
- 提交只包含编码修复和 host test，不包含 fuxi 日志数组。

这是包格式修复，不是 fuxi quirk，不应留在设备分支。

### 2. 新增通用 publish policy，不保留 boot-arg 判断

建议在 Adreno740 新增 `NativeAdrenoPublishPolicy.h`：

```cpp
enum : uint32_t {
    NativeAdrenoPublishSharedAddressSpace = 1u << 0,
    NativeAdrenoPublishPlainSubmission    = 1u << 1,
};

struct NativeAdrenoPublishPolicy {
    uint32_t version;   // 1
    uint32_t bytes;     // sizeof(NativeAdrenoPublishPolicy)
    uint32_t flags;
    uint32_t reserved;  // 0
};
```

保留现有 `native_adreno_publish(...)`，再增加：

```cpp
IOReturn native_adreno_publish_with_policy(
    NativeAdrenoVm *, NativeAdrenoSubmit *, NativeAdrenoRing *,
    const NativeAdrenoRingClock *, const NativeAdrenoTimer *,
    const NativeAdrenoAbiDeviceInfo *,
    const NativeAdrenoPublishPolicy *,
    uint32_t input_bits, uint32_t output_bits);
```

旧入口用全零 policy 调新入口，保证 sheng 和现有调用者不变。

具体实现：

- `NativeAdrenoPublish.cpp::AGXDeviceUserClient::mount()`：
  - 默认仍 `createVm()`，行为与官方当前版本一致；
  - `SharedAddressSpace` 时使用 `g.vm`，不创建 process VM；
  - 每个用户连接仍分配唯一 queue range；
  - 共享表模式调用 `setScope(base, 0, 0)`。
- `NativeAdrenoSubmit`：
  - 增加 `setSubmissionMode()`，只允许 attach 后、尚无 outstanding job 时调用；
  - 默认用 `submitFramed()`；
  - `PlainSubmission` 用 `ring->submit()`，仍保留 job、BO lifetime 和 fence accounting；
  - 不在这里解析或改写任何客户端 PM4。
- policy 严格校验 `version`、`bytes`、未知 flag 和 `reserved`；默认全零必须与当前官方路径逐字节等价。

这把本地 `07e815b` 和 `6106a95` 中唯一仍需留在内核的功能收敛成一个通用接口。不要把 `sheng-gpu-single-table`、`native_adreno_plain_submit` 等设备名称或全局变量合入上游。

### 3. 用 device capability 告诉 Turnip 命令限制

在现有 `NativeAdrenoAbiDeviceInfo.flags` 增加两个通用 bit：

```cpp
NativeAdrenoDeviceSingleRenderThread = 1u << 2; // CS 只能选择 BR
NativeAdrenoDeviceNoWaitMemWrites    = 1u << 3; // CS 不得发 CP_WAIT_MEM_WRITES
```

已有结构大小不变，旧客户端会忽略新 bit。命名描述能力，不出现 `fuxi`，也不描述实现方式。

## Turnip 修改

在 UniXNU 新增 `patches/mesa/turnip-darwin-command-policy.patch`，并放到 `scripts/data/adreno-compiler-inputs.json` 中 `turnip-darwin-kmd.patch` 之后。该 patch 只在 Darwin KMD 从 `NativeAdrenoAbiDeviceInfo.flags` 读到 capability 时生效；Linux/MSM 和 sheng 的输出必须不变。

### `CP_THREAD_CONTROL`

不能只修改一个调用点。固定 Mesa `85fda3b...` 在 `tu_cmd_buffer.cc`、`tu_lrz.cc`、`tu_query_pool.cc` 等位置直接发包。

建议在 `tu_cs` emission 层增加一个小状态：

- `tu_cs_emit_pkt7()` 看到 `CP_THREAD_CONTROL` 时标记下一 dword 是 thread-control payload；
- 下一次 `tu_cs_emit()` 在 `SingleRenderThread` 模式下：
  - 清除 thread selector；
  - 写入 `CP_SET_THREAD_BR`；
  - 清除 `CP_THREAD_CONTROL_0_SYNC_THREADS`；
- 默认模式不触碰 payload。

这等价替代当前内核递归扫描所有 IB 的 `native_adreno_bv_to_br` 和 `native_adreno_no_thread_sync`，但修改发生在命令生成时。

### `CP_WAIT_MEM_WRITES`

同样在 `tu_cs_emit_pkt7()` 层处理：

- `NoWaitMemWrites` 时把零 payload 的 `CP_WAIT_MEM_WRITES` header 编码为合法的零 payload `CP_NOP`；
- 其他设备原样输出；
- 对非零 payload 或未知包形状直接断言/报错，不静默改写。

这覆盖 `tu_cmd_buffer.cc`、`tu_queue.cc`、query 和 autotune 路径，等价替代内核当前记录到的 107 次改写。

### 测试

新增 host test，至少验证：

1. capability 为 0 时，生成的 PM4 与修改前 byte-for-byte 一致；
2. fuxi capability 下，嵌套/secondary CS 中没有 BV、BOTH、`SYNC_THREADS` 和 `CP_WAIT_MEM_WRITES`；
3. `CP_NOP` 是合法的 type-7 零 payload 编码；
4. iOS Turnip 和 host Turnip 均可编译；
5. `scripts/mesa_patch_identity.py` 能反向重建固定 Mesa 源码，没有未登记修改。

不要在 `NativeAdrenoSubmit.cpp` 再做第二次 PM4 改写。测试期可以保留只读审计，发现禁用包就拒绝 submission；稳定后删除审计。

## fuxi 板级修改

在 `FuxiGpuPowerProbe.cpp` 中定义静态 policy：

```cpp
static const NativeAdrenoPublishPolicy gGpuPolicy = {
    1, sizeof(NativeAdrenoPublishPolicy),
    NativeAdrenoPublishSharedAddressSpace |
    NativeAdrenoPublishPlainSubmission,
    0,
};
```

并设置：

```cpp
gDeviceInfo.flags = NativeAdrenoDeviceClientIova |
                    NativeAdrenoDeviceSingleRenderThread |
                    NativeAdrenoDeviceNoWaitMemWrites;
```

随后调用 `native_adreno_publish_with_policy()`。

删除：

- `native_adreno_no_thread_sync`；
- `native_adreno_bv_to_br`；
- `native_adreno_drop_wait_mem`；
- `native_adreno_plain_submit`；
- 对应赋值、日志和 boot page 字段；
- `sheng-gpu-single-table=1` boot arg。

`native_adreno_wc_enabled=0` 是官方已有的内存策略入口。第 5 轮已经证明当前 XNU WC 契约不能可靠发布客户端 IB，因此 fuxi 保留它作为正确性策略；这不属于提交 policy。`sheng-tu-debug=sysmem,nocb,nobin` 则分项测试，其中 `nobin`、`nocb` 已能从 sysmem 基线撤掉，`sysmem` 仍待 tiled 定位。

## 私有提交迁移表

| 私有提交 | 处理 |
| --- | --- |
| Adreno740 `414fa69` VM readback | 删除；bring-up 证据留在设备文档 |
| Adreno740 `07e815b` single table | 变成上游通用 `SharedAddressSpace` policy |
| Adreno740 `b1bd99d` 2-dword SMMU update | 作为通用正确性修复 upstream |
| Adreno740 `d7bfa23` reset/thread diagnostics | 删除 |
| Adreno740 `5c90626` no SYNC_THREADS | 移到 Turnip capability 路径 |
| Adreno740 `6106a95` command repair | plain-submit 收敛到通用 policy；IB rewrite 移到 Turnip；其余诊断删除 |
| AdrenoMetal `90c60b8` open markers | 删除，不是功能依赖 |
| GOP `af8b7da` tunnel counters | 删除，不是功能依赖 |

## 实施顺序

### Phase A：先去掉无功能依赖的两个 fork

1. 把 AdrenoMetal pin 改成 `6d7f73437087c57ee54a41adceb1735daaea405e`；
2. 把 GopFramebuffer pin 改成 `e47c1b8de169cd2bd0b885c7d85e433010e3cdf2`；
3. 编译 kernel、payload、boot image；
4. 真机确认 Metal device、fence 和 GOP 数量未回退。

### Phase B：先合上游接口，再切 Adreno740

1. 向 Adreno740 提交 2-dword 修复；
2. 单独提交 publish policy 和 host tests；
3. 向 UniXNU 提交 capability bits、Turnip policy patch 和 host tests；
4. 上述提交进入官方仓库后，将 `components.toml` 的 Adreno740 revision 更新到合入后的官方 SHA；
5. 同时修改 fuxi probe 使用 policy，删除四个全局 workaround。

在 upstream commit 存在之前，不要把 Adreno740 pin 改成当前 `6fa7bae...`。如果需要立即让组件 checkout 干净，可临时维护一张设备拥有的 build patch，但它必须只应用到输出目录的副本，不能修改 symlink 指向的 `drivers/Adreno740`；当前构建器没有这个 component-source patch 机制，所以推荐先上游接口，而不是增加一个脆弱的预构建脚本。

### Phase C：逐项恢复默认路径

每次只改一个变量并完整启动：

1. 恢复 per-process VM，要求 `checkCpSwitch` 成功、table switch 计数增长且无 SMMU fault，然后撤 `SharedAddressSpace`；
2. 修复 BV ring/线程初始化，恢复 framed submission，要求 framed prologue/fence 单测和真实客户端都退休，然后撤 `PlainSubmission`；
3. 恢复 `CP_WAIT_MEM_WRITES`，定位并修复 posted write/PTE 属性根因后撤 `NoWaitMemWrites`；
4. 评估 write-combining；只有 CPU/DRAM/GPU readback 与映射属性契约全部一致才撤 `sheng-gpu-wc=0`，否则把它固定为板级内存策略；
5. 在 sysmem/tiled 两条路径分别测试 `nobin`、`nocb`，最后撤 `sysmem`，每次记录帧正确性和 fence 延迟。

Phase C 完成后，fuxi 的 **publish/submission policy** 为全零，内核不改写客户端 PM4；板级内存策略可保留 `sheng-gpu-wc=0`。tiled 是否能完全使用官方默认算法，取决于第 6 轮停点和 `tiled + nobin/nocb` 矩阵，不能预先写成结论。

## CI 和验收

### 源码洁净检查

CI 必须检查三个组件：

```sh
git -C drivers/Adreno740 diff --quiet HEAD
git -C drivers/AdrenoMetal diff --quiet HEAD
git -C drivers/GopFramebuffer diff --quiet HEAD
```

并核对各自 `HEAD` 等于 `components.toml` 的 revision，且 revision 能从官方 remote 获取。禁止“目录是 fuxi 分支但 catalog 写官方 SHA”的假洁净状态。

### Host 验收

```sh
python3 scripts/check-turnip-fuxi-command-policy.py
python3 scripts/build-turnip-darwin.py --target host
python3 scripts/check-turnip-darwin-host.py
python3 scripts/build-turnip-darwin.py --target ios
./unixnu build kernel -D ../unixnu-fuxi
```

还要运行 Adreno740 的 host tests，并验证默认 policy 对现有 sheng fixture 没有输出差异。

### 真机验收

一次候选镜像至少满足：

- Metal plugin 成功返回 native device；
- 连续提交和退休计数相等，最低不低于当前 `48/48` 基线；
- GOP present/tunnel 成功数不低于当前 `16/16`；
- 无 SMMU fault、GPU reset、context loss；
- 抓取的客户端 PM4 不含 BV/BOTH、`SYNC_THREADS` 和 `CP_WAIT_MEM_WRITES`；
- 内核不再打印或累计 `bv patched`、`wait_mem dropped`，证明改写已经离开内核；
- 至少保持桌面合成运行 10 分钟，并记录 commit、boot image hash 和串口证据。

只有 host build 通过不能证明 Metal 可用；只有画面出现也不能证明已经脱离内核 rewrite。两组证据都必须保存。

## 上游更新流程

以后更新只做：

1. 获取三个官方 `main` 的目标 SHA；
2. 更新 `components.toml` revision；
3. 运行源码洁净检查、Mesa policy 测试和完整构建；
4. 真机跑固定验收；
5. 记录新 SHA、镜像 hash 和 `submitted/retired`、GOP、fault/reset 数据。

不再 rebase `fuxi` GPU driver 分支，也不把诊断 marker 当成功依赖保留下来。


---

## 执行记录

### Phase A —— 完成并真机验证

- `components.toml`：AdrenoMetal → `6d7f734`，GopFramebuffer → `e47c1b8`；两个子模块 checkout 到同一提交。
- 包侧摘掉了对私有符号 `native_gop_handoff_stat` 的引用（官方版本没有它）。
- 洁净检查：三个组件 `git diff --quiet HEAD` 全部通过。
- 镜像 `sha256 282aeecd…`，刷入 `boot_a`。

真机结果（无回退，且高于 48/48、16/16 基线）：

```
fence live 1 submitted 85 retired 85
gop offers 37 done 37 fail 0 abandon 0
fuxi-metal: opens 9 busy 0 bad 0
cp status 1 ... fault 0 status1 0 protect 0
sheng-fb: copy 0
```

SMMU fault 0、GPU reset 0、无 panic（日志里的 3 处 "panic" 是 `gPanicBase` 初始化提示与 boot-args 文本）。

### Phase B 前半 —— 完成并真机验证

Adreno740 `2398bd2`：

- 新增 `NativeAdrenoPublishPolicy.h`（version 1、定长、未知 flag 与非零 reserved 一律拒绝）；
- `native_adreno_publish_with_policy()`，旧入口以空 policy 转发 —— **默认路径逐字节不变**；
- `NativeAdrenoSubmit::setSubmissionMode()`，在已有在飞任务时拒绝切换；
- `NativeAdrenoAbiDeviceInfo.flags` 新增 `SingleRenderThread`(1<<2)、`NoWaitMemWrites`(1<<3)，结构长度不变。

fuxi 板级：

- 探针改为构造 policy（`SharedAddressSpace|PlainSubmission`）并调用新入口；
- 设备信息声明两个能力位；
- 删除 `sheng-gpu-single-table=1` 启动参数；
- 删除 `native_adreno_plain_submit` 全局。

镜像 `sha256 8563893a…`，真机结果同上表（85/85、37/37、CPU 拷贝 0），说明改走 policy 后功能未回退。

### 当时仍待完成（历史记录）

1. **Phase B 后半**：`patches/mesa/turnip-darwin-command-policy.patch` —— 在 `tu_cs_emit_pkt7()` 层按 capability 处理 `CP_THREAD_CONTROL` 与 `CP_WAIT_MEM_WRITES`，并加 host test（capability=0 时输出逐字节不变）。完成后才能删除内核侧 `bv_to_br`、`no_thread_sync`、`drop_wait_mem`（本次启动仍在工作：`wait_mem dropped 188`、`bv_patch ok 76`）。
2. **Phase C**：逐项恢复 per-process VM、framed submission、`CP_WAIT_MEM_WRITES`，评估 write-combining，再撤 `nobin`/`nocb`/`sysmem`。后续实测结果见下文；这不是当前待办清单。


### Phase B 后半 —— 完成（Turnip 侧），内核侧改写尚未移除

UniXNU `166a1fa`：

- 新增 `patches/mesa/turnip-darwin-command-policy.patch`，并在 `scripts/data/adreno-compiler-inputs.json` 中紧随 `turnip-darwin-kmd.patch` 登记；
- 在 `tu_cs.h` 新增 `tu_thread_control_payload()` —— 所有 `CP_THREAD_CONTROL` 载荷的唯一构造点；设备报告"仅 BR 线程"时强制选 BR 且不置 `SYNC_THREADS`；
- `tu_cs_emit_pkt7()` 对零载荷 `CP_WAIT_MEM_WRITES` 换成零载荷 `CP_NOP`（同一 header 编码，替换合法）；
- `tu7_set_thread_both_patchpoint()` 在该设备上发 BR 形式且**不记录 CB 控制点** —— 否则后续 patch pass 会把 BOTH 写回；
- 新增 `scripts/check-turnip-fuxi-command-policy.py`，15 项 host 检查全部通过（补丁序列可逐字节还原 pinned 源码、每个改写都受能力位约束、CP_NOP 编码合法且与 CP_WAIT_MEM_WRITES 仅 opcode 不同、补丁中不出现板名）。

真机（镜像 `sha256 7471aca5…`）：

```
fence live 1 submitted 51 retired 51
gop offers 18 done 18 fail 0 abandon 0
fuxi-metal: wait_mem dropped 0 last_at 0 seen 0
fuxi-metal: bv_patch ok 42 last_bytes 1088
```

- **`CP_WAIT_MEM_WRITES` 已从流中消失**（`seen 0`，此前 188）——Turnip 侧生效；
- fence 与 GOP **优于基线**（51/51 与 18/18，基线 48/48 与 16/16）；
- 但内核 `bv_patch` 仍改写 42 次，说明 Turnip 仍有路径发出 BV/BOTH 选择。因此**内核侧三个改写暂不能删除**，Phase C 的前置条件未满足。

### Phase B 收尾 —— 完成

定位过程：内核改写用的是 `(payload&~3u)|1u|(1u<<27)` —— 它不只要 BR，还要**置 bit27（禁 concurrent binning）**。所以即使 Turnip 选了 BR，只要没有 bit27 就仍被改写。直方图（`bv_seen`）确认了这一点：改写前的载荷是 `0x00000001`（BR 但无 bit27）。

修复：`tu_thread_control_payload()` 在能力成立时直接返回 `CP_THREAD_CONTROL_0_THREAD(CP_SET_THREAD_BR) | CP_THREAD_CONTROL_0_CONCURRENT_BIN_DISABLE`。

随后把内核侧改写降级为**只读审计**（Adreno740 `1307c7e`）：删除 `native_adreno_bv_to_br`、`native_adreno_drop_wait_mem`、`native_adreno_drop_empty_ib` 三个全局与全部 poke 路径，保留遍历计数。

真机（镜像 `sha256 7c8e7915…`）：

```
audit wait_mem seen 0
bv_seen sel3 0 sel2 0 sel1_nobin 0 other 0 last 8000001 8000001 8000001 8000001
bv_patch ok 0
fence live 1 submitted 59 retired 59
gop offers 22 done 22 fail 0 abandon 0
```

**内核不再修改客户端命令流**，流本身已带 `0x08000001`。Phase B 完成。

### Phase C 第 1 步 —— 完成：每进程页表恢复

`gGpuPublishPolicy` 只保留 `PlainSubmission`（镜像 `sha256 56fddc6c…`）。真机：

```
fence live 1 submitted 61 retired 61
gop offers 18 done 18 fail 0 abandon 0
fuxi-metal: opens 7 busy 0 bad 0
cp status 1 0 0 0 fault 0 status1 0 protect 0
```

**每个连接重新拥有自己的页表，且无 SMMU fault、无 context loss。** 这推翻了此前"本板 command processor 不能从 ring 切 TTBR0"的结论：那次失败是流本身不合法（含 BV/BOTH 选择与 `CP_WAIT_MEM_WRITES`），切表指令因此从未被正常执行。流修好之后，`checkCpSwitch` 通过。

### Phase C 第 2、3 步 —— 完成

- **framed submission 恢复**（policy 只剩 0）：`62/62` fence、`19/19` GOP。
- **`native_adreno_no_thread_sync` 删除**（Adreno740 `fbb2ddc`）：prologue 重新发带 `SYNC_THREADS` 的出厂载荷，仍然 `73/73`、`24/24`。
- **`CP_WAIT_MEM_WRITES` 恢复**（撤 `NoWaitMemWrites`）：流里重新出现等待（`audit wait_mem seen 121`），fence 仍全部兑现。

**两个旧结论被推翻**：`SYNC_THREADS` 与 `CP_WAIT_MEM_WRITES` 都不会让本板 command processor 停住——它们当年看起来致命，是因为所在的流本身不合法。

### Phase C 第 4 步 —— WC 恢复失败，转为板级内存策略

撤掉 `sheng-gpu-wc=0`（恢复 write-combining）后：

```
fence live 1 submitted 10 retired 9
gop offers 0 done 0
cp status 800005 0 0 0 fault 0 status1 8001060e
```

客户端 IB 在 write-combined 映射下读回全零，第一条提交永不兑现。这是**唯一不能靠"流修好"消除的项**，已恢复该参数并在 `board.toml` 注明原因。

### Phase C 第 4 步的取证（本轮）

打开 write-combining 并打开驱动的审计计数器：

```
fuxi-metal: wc audit n2 lines16 dirty8 stale0 last0 rewc0 distrusted2
fence live 1 submitted 10 retired 9
gop offers 0 done 0
```

审计**确实发现 8 条脏行**，并让驱动放弃 WC 信任（`distrusted 2`），随后提交进入包含 WC 范围的物理页维护路径——**但提交仍然卡住**。所以问题不在“没有检测到”。现有证据能证明的是：当前 XNU 映射、别名和 cache-maintenance 契约没有把该 WC 客户端写入可靠地发布给 command processor；它还不能区分是页面属性传播、别名、TLB 中的 memory attribute，还是 WC store buffer/维护原语本身的问题。

这条把缺陷从“驱动没做维护”收窄成“当前 WC 映射/别名/维护契约不能可靠发布 IB”。`NativeAdrenoMemory::publishForDevice` 的路径已经在第 5 轮读完；更精确的结论和重开条件见下文第 6 轮方案。

### Phase C 第 5 步 —— 部分完成

| 变量 | 结果 |
| --- | --- |
| `nobin` | ✅ 撤掉，`87/87` fence、`32/32` GOP（反而是最好的一次） |
| `nocb` | ✅ 撤掉，`63/63`、`19/19` |
| `sysmem` | ⏳ **当前必需**：补齐所有 BV/BOTH 发射点后 `bv_seen` 已全零，但 tiled 的首个受影响提交仍不退休，根因待停点快照定位 |

### 当前终态

```
policy          全零（默认路径：每进程页表 + framed 提交）
device flags    ClientIova | SingleRenderThread（NoWaitMemWrites 已撤）
保留的补偿      sheng-gpu-wc=0（当前端口的发布策略）、sheng-tu-debug=sysmem（tiled 根因未定位）
真机            63/63 fence、19/19 GOP、bv_seen 全零、无 panic/reset/SMMU fault
```

计划的目标终态“policy 全零”已达成。Phase C 现在是 3/5：每进程页表、framed submission 和 `CP_WAIT_MEM_WRITES` 已恢复；WC 已形成有证据支持的发布策略，tiled 仍是待定位缺陷。`nobin`、`nocb` 已分别在 sysmem 基线下撤掉，但这不等于它们在 tiled 路径中已经被单独排除。


### write-combining 的结论（第 5 轮）

代码路径已经读到底。`NativeAdrenoMemory::publishForDevice` 对 write-combined 缓冲是：

```cpp
if(write_combined_) {
    if(!include_write_combined)return kIOReturnSuccess;         // 信任时：完全不清理
    return maintainImported(offset,bytes,CleanInvalidate);      // 不信任时：按物理地址清理
}
```

设备实测（第 4 轮）显示：审计**确实**发现了 8 条脏行、驱动**确实**转入不信任（`distrusted 2`），此后每次提交都走物理地址清理——**而提交仍然失败**。

也就是说：当前的物理别名维护路径没有令 WC 客户端写入对 GPU 可靠可见。`sys_*cache_flush` 在该 Apple 平台上只是 DSB，说明用户态接口不能补足缺失的维护；但内核实际路径使用 `dc cvac/civac`，所以不能再把失败简单归因为“函数只有 DSB”。仍可能存在客户端映射未同步改成 WC、同一物理页的 cacheable/WC 别名、pmap 属性更新后 GPU/CPU TLB 中的旧 memory attribute，或 WC store 与物理别名 clean 之间缺少平台契约。

**工程结论不变，但结论边界需要收紧**：`sheng-gpu-wc=0` 是当前 fuxi + 当前 XNU 内存契约下正确的发布终态，不是上线欠账；现有证据尚不足以宣称 SM8550/A740 硬件本身绝对不支持 WC。只有出现一个能证明所有 CPU 映射属性一致、属性更新完成 shootdown、同一 IB 在 client/cacheable/uncached/GPU 四个观察点一致的受支持平台原语，才值得重新开启 WC。否则默认 cacheable + 每次提交显式 clean 是正确性路径。

### 验收证据（第 5 轮）

| 计划要求 | 实测 |
| --- | --- |
| Metal plugin 返回 native device | ✅ `opens 7 busy 0 bad 0`，marklog 到 6803 |
| 提交与退休计数相等，≥48/48 | ✅ **63/63**（另有 81/81、95/95 的启动） |
| GOP ≥16/16 | ✅ **19/19**（另有 29/29、38/38） |
| 无 SMMU fault / reset / context loss | ✅ 全部 0 |
| 客户端 PM4 不含 BV/BOTH/SYNC | ✅ `bv_seen` 全零，载荷 `0x08000001` |
| 内核不再 `bv patched`/`wait_mem dropped` | ✅ 改写路径已删除，只剩审计计数 |
| `CP_WAIT_MEM_WRITES` | 按 Phase C 第 3 步**已恢复**（审计计数即证据） |
| 10 分钟桌面合成 | 串口只在报告点输出，无法直接计时；本会话多次启动均无 panic/reset |

### tiled 模式（sysmem 的必要性）——已排除线程选择

两处漏网的 BV 发射点（`tu_cmd_buffer` 的 binning 分支、`tu_lrz`）修复后，撤掉 `sysmem` 时：

```
bv_seen sel3 0 sel2 0 sel1_nobin 0 other 0 last 8000001 8000001 8000001 8000001
fence live 1 submitted 15 retired 10 ; gop offers 1 done 0 ; 之后 submitted 0
```

**流里没有任何 BV/BOTH**，但首条 tiled 提交仍不兑现。对比两种模式的 IB 包直方图，没有结构差异（tiled 只是停得更早，缺少后续的 `CP_DRAW_INDX_BIN`/`CP_INDIRECT_BUFFER`/`CP_EXEC_CS`）。所以 tiled 的失败**另有原因且尚未识别**，`sysmem` 的必要性已从"BV 未覆盖"重新归类为"tiled 模式下另一处未识别的问题"。

## 两个剩余问题的收敛方案（第 6 轮）

### A. write-combining：关闭问题，但保留可证伪的根因边界

#### 已证明

1. WC 下出现了真实可见性错误，不是 fence 计数误报：客户端 IB 曾从内核/DRAM 观察为全零，首个相关 fence 不退休。
2. 审计能够发现异常：`dirty8` 证明至少有 CPU cacheable 观察点持有和 DRAM 不同的 IB 行。
3. 驱动能够切入 distrust 路径：`distrusted2` 证明问题不是漏掉状态转换。
4. 进入物理页维护路径后仍不能让该提交完成，因此“再多调用一次同一 clean”不是解决方案。
5. 关闭 WC 后，当前发布镜像达到 `63/63` fence、`19/19` GOP，且无 panic/reset/SMMU fault。

#### 尚未证明

- 没有证明 A740 或 SM8550 硬件不支持 Normal-NC/WC；Android/Linux 的 msm 路径本身就是反例线索。
- 没有证明 `dc cvac/civac` 指令没有执行。它可能正确维护了物理别名对应的 cache line，但没有覆盖另一个仍以旧属性存在的映射或尚未完成的 WC store。
- 没有证明 page 的全局 WIMG 值等于每个用户 pmap 中已经生效的 PTE 属性，也没有证明 CPU/GPU TLB 都已经丢弃旧 memory attribute。
- 本次失败的第一条提交一旦卡住，后续提交不能作为独立证明；每个替代原语必须新启动、只看第一条受影响提交。

#### 发布方案

- 保留 `sheng-gpu-wc=0`，把它记录为 **fuxi 当前 XNU 内存契约的正确性策略**。
- WC 关闭时继续保留正常的 cacheable BO publication：提交前 clean，GPU 写回后 invalidate；不能因 WC 关闭而删掉维护。
- 不为追求配置“全零”引入私有 cache opcode、未审计的 pmap 调用或每帧全内存扫描。
- 性能优化应先缩小实际 dirty range、批量维护和减少不必要 readback，不能以重新打开 WC 换正确性。

#### 以后重新开启 WC 的必要条件

只有同时满足下列条件才重开该议题：

1. 能枚举同一 BO 页在 client pmap、kernel alias 和物理 aperture 中的实际属性，并证明没有 cacheable/WC 冲突别名。
2. 页面属性切换有明确的跨 CPU TLB shootdown 完成点，GPU 页表中的 memory attribute 也与设计一致。
3. 对同一组已知 IB dword，依次取得 client view、内核映射、fresh uncached/DRAM 观察和 GPU `CP_MEMCPY` 观察，四者一致。
4. 平台提供有契约的 WC publish/drain 原语；仅重复当前的物理别名 `dc cvac/civac` 不算新证据。
5. 新启动下至少 20 次首提交和 1000 次连续提交全部退休，无 dirty/stale audit、SMMU fault 或偶发零 IB。

在这些条件出现前，Phase C 第 4 项记为“**策略关闭 / 不再阻塞发布**”，而不是“硬件永久不支持”。

### B. tiled：停点快照的正确解释

刚加入的 `native_adreno_stall_mem` 会给出 BR IB1、BV IB1、BV IB2 的位置和其后 16 个 dword。它比包直方图强得多，但需要注意两个限制：

1. `IB_size - REM_SIZE` 是 CP 的**取流前沿**，不自动等于真正阻塞指令。CP 可以预取越过一个正在等待的包；必须同时看 SQE current opcode/header、RBBM 状态和等待目标。
2. 当前 50 ms 快照发生在一次 TLB invalidate + kick 之后；1 s 快照之前还会发生 200 ms bus drain。因此首轮镜像仍值得跑，但它记录的是“恢复动作后的稳定停点”，不是完全未扰动现场。如果结果含糊，下一版仪器应在任何 kick/TLBI/bus drain 之前先保存一份只读 snapshot，再执行现有恢复动作。

每次分析必须保存：

- stalled fence 编号，以及它是不是开启 tiled 后的第一个未退休 fence；
- ring RPTR/WPTR、IB1/IB2 base 和 REM_SIZE；
- `stall_mem` 的 dword index、16 个字和对应 map handle；
- BR/BV SQE PC + current header；
- SMMU FSR/FAR/FSYNR、CP fault/protect/status；
- 该 fence 的完整 root IB 与所有嵌套 IB 图，使用同一构建的 `adreno_pm4.xml` 离线解码。

不能只拿 16 个字和“最像的 opcode”对号入座。应把这 16 个字在该 fence 的完整 IB 图中做唯一匹配，确认父子层级、包头边界、payload 长度和目标地址，再给停点命名。

### C. tiled 的优先假设：不是 BV 选择，而是 binning 生命周期

`bv_seen` 全零只证明所有 `CP_THREAD_CONTROL` 最终都选择 BR；它不证明 tiled 流已经变成合法的单线程算法。当前补丁会把 `tu6_tile_render_begin()`、LRZ 和 query 中原本选择 BV/BOTH 的控制字改成 BR，但 Turnip 仍可能：

- 进入 `use_binning` 分支；
- 等待 VSC/visibility stream、LRZ 或 on-chip reslist 状态；
- 执行假设 BV 阶段已经产生数据/事件/时间戳的 BR 消费阶段；
- 保留 concurrent-binning 的数据依赖，只是线程选择字已不再出现 BV。

因此“无 BV/BOTH”与“无需 BV 产物”不是一回事。包直方图也不足以排除它，因为相同 opcode 的 payload、地址、顺序和初始内存值都可能不同。

#### 最小实验矩阵（每项独立冷启动，只看第一条受影响提交）

| 轮次 | `sysmem` | `nobin` | `nocb` | 回答的问题 |
| --- | ---: | ---: | ---: | --- |
| T0 | 开 | 关 | 关 | 当前 `63/63`、`19/19` 发布基线 |
| T1 | 关 | **开** | 关 | tiled 本身是否可行；若通过，根因在硬件 binning/VSC 生命周期 |
| T2 | 关 | 关 | **开** | 是否仍有 concurrent-binning 控制/同步状态未被 capability 完整关闭 |
| T3 | 关 | **开** | **开** | 最保守的 BR-only tiled 路径是否可行 |
| T4 | 关 | 关 | 关 | 现有失败复现和停点取证 |

`nobin` 之前只是在 `sysmem` 基线下撤掉；sysmem 本身绕过 tiled render，所以那次成功不能排除 tiled 的 binning 路径。T1 是当前信息增益最高的测试，应排在继续枚举 opcode 之前。

实际执行顺序建议：先用已经编好的仪器跑一次 T4，保存当前失败的停点；随后立即跑 T1。若 T1 通过，再跑 T3 确认 `nocb` 是否还有独立影响；若 T1 失败，才跑 T2 并按停点分流。这样既不浪费已完成的仪器镜像，也能用最少启动次数检验 binning-lifecycle 假设。

#### 结果分流

| 观察 | 结论 | 解决方向 |
| --- | --- | --- |
| T1/T3 通过 | tiled 可用，硬件 binning 或它的生产者/消费者同步有问题 | capability 应在算法选择处令 `use_binning=false`；先发布“tiled + nobin”，再单独移植串行 binning |
| T2 通过而 T1 不通过 | concurrent-binning 控制点或同步仍有遗漏 | 审计 CB control point、timestamp/event 和 wait 的成对关系，不再搜索 BV selector |
| T1/T2/T3 全失败且停在同一包 | 问题属于 tiled 公共路径 | 按停点查 tile load/store、CCU/cache、GMEM/VSC/LRZ 资源和其映射/初始化 |
| map handle 为 0，或 handle 不早于 `vm_next` | IB/资源未映射、被释放重用或 job 没有持有正确 BO | 修正提交 BO 列表和 lifetime；无 SMMU fault 不能排除读到合法但错误的页 |
| ring RPTR 未进入该 IB，kick 后才改变 | 不是客户端 tiled 包停点 | 回到 prologue/WPTR/doorbell/可见性路径 |
| REM_SIZE 为 0，SQE opcode 是 wait/event | CP 已取完整个 IB，正在等数据或事件 | 解码 wait 地址/比较值，寻找唯一生产者；比较目标内存的 CPU/DRAM/GPU 值 |
| index 稳定落在 child IB 中某包，SQE opcode 与之相符 | 得到真实候选停点 | 只对该包做一次单变量禁用/替换实验，禁止整类 packet 批量 NOP |
| 50 ms 与 1 s 快照因 kick/drain 改变 | 现有仪器扰动现场 | 增加 pre-recovery 只读快照后再判断，不能用后一个位置宣称原始根因 |

### D. 得到具体停点后的处理顺序

1. **先验证包边界。** 用准确 IB 大小和嵌套关系计算 index，确认 16 字窗口中的 header/payload 没有错位。
2. **再验证资源。** 所有包引用的地址都查 VM mapping、handle、创建代际、CPU/DRAM 首字和 job hold；“没有 SMMU fault”只说明地址可能可翻译。
3. **若是 WAIT/COND。** 记录目标地址、mask、reference 和当前值，沿流查唯一 producer。重点检查 VSC、LRZ、visibility、timestamp/event 与 on-chip reslist。
4. **若是 cache/event。** 检查对应 event 的前置写、CCU 状态和所需 idle，而不是恢复已经证明无害的全局 `CP_WAIT_MEM_WRITES` 禁用。
5. **若是 draw/bin。** 先做 T1；若 `nobin` 通过，在 Turnip 算法选择处禁用不受支持的 binning，不能继续靠内核改写 PM4。
6. **最后才做定点实验。** 只替换一个已确认候选包，并用独立冷启动验证。替换后 fence 退休只能证明“该依赖阻塞”，还必须验证像素、GOP 完成和后续提交，不能把跳过工作当成修复。

### E. Phase C 的更新验收

Phase C 不再要求为了形式上的“全零”恢复 WC；它的发布验收改为：

- policy 全零、每进程页表、framed submission、`CP_WAIT_MEM_WRITES` 均保持官方语义；
- 内核只审计客户端 PM4，不做 BV/BOTH、SYNC 或 WAIT 改写；
- WC 以 `sheng-gpu-wc=0` 作为有证据的板级内存策略；
- `sysmem` 只有在 T1–T4 和停点取证完成后才能决定是发布策略还是仍需修复；
- tiled 若以 `nobin` 通过，可以先把“单线程设备不启用硬件 binning”做成通用 capability 行为，但必须证明非 fuxi、capability=0 的流逐字节不变；
- 最终设备门槛仍是提交=退休、GOP offer=done、像素正确，并且无 panic/reset/SMMU fault，不能只看 fence。

### Phase C 第 5 步 —— 完成：nocb 才是元凶，三条全部撤掉

按补充方案跑 sysmem/tiled × nobin/nocb 矩阵：

| TU_DEBUG | 渲染模式 | 结果 |
| --- | --- | --- |
| sysmem,nocb,nobin | sysmem | 48/48、16/16（Phase B 基线） |
| sysmem,nocb | sysmem | 87/87、32/32 |
| sysmem | sysmem | 63/63、19/19 |
| nocb（插件默认值） | tiled | 15/10、GOP 0、之后 submitted 0 |
| （空） | tiled | **71/71、23/23** |

关键发现：nocb 在 Mesa 里就是 `TU_DEBUG_NO_CONCURRENT_BINNING` —— 正是当年“关掉并发 binning”的补偿。
Turnip 的 capability 现在已在命令流层保证只选 BR，nocb 因此**多余**，而且在 tiled 模式下**有害**。

此前“tiled 模式失败”的结论被更正：**tiled 模式本身是好的**，失败来自插件的默认 nocb；sysmem 从来不是
必要条件。板级现在只需把 TU_DEBUG 置空（`sheng-tu-debug=`）以抑制插件那个默认值。

### Phase C 完成

| 步骤 | 结果 |
| --- | --- |
| 1 每进程 VM | 完成：撤 `SharedAddressSpace`，无 SMMU fault |
| 2 framed submission | 完成：撤 `PlainSubmission`，并删除 `no_thread_sync` 全局 |
| 3 `CP_WAIT_MEM_WRITES` | 完成：撤 `NoWaitMemWrites`，等待恢复且全部兑现 |
| 4 write-combining | 按补充方案**评估后固定为板级内存策略**：审计发现 8 条脏行、驱动转入不信任、按物理地址清理仍无效，契约不成立，保留 `sheng-gpu-wc=0` |
| 5 `nobin`/`nocb`/`sysmem` | 完成：三条全部撤掉，tiled 默认模式 71/71、23/23 |

**终态：publish/submission policy 全零，内核不改写客户端 PM4，板级只保留 `sheng-gpu-wc=0` 这一条内存策略**
—— 与补充方案“板级内存策略可保留 sheng-gpu-wc=0”的完成判据一致。

### 最终验收（Phase C 完成态）

配置（镜像 `sha256 9aff0c67…`，刷入 `boot_a`）：

```
publish policy    0（默认路径：每进程页表 + framed 提交）
device flags      ClientIova | SingleRenderThread（NoWaitMemWrites 已撤）
board.toml        sheng-gpu-wc=0（板级内存策略）、sheng-tu-debug=（空，抑制插件默认的 nocb）
kernel            不改写客户端 PM4；NativeAdrenoSubmit 只剩只读审计
```

真机：

```
fence live 1 submitted 71 retired 71
gop offers 23 done 23 fail 0 abandon 0
bv_seen sel3 0 sel2 0 sel1_nobin 0 other 0 last 8000001 8000001 8000001 8000001
wc audit n16 lines0 dirty0 stale0 last0 rewc0 distrusted0
panic 0、gpu reset 0、smmu fault 0
```

Host：

```
check-turnip-fuxi-command-policy.py     18/18 PASS（含补丁序列逐字节还原）
build-turnip-darwin.py --target host     通过（22964744 字节）
check-turnip-darwin-host.py             smoke/pipeline-cache/device/fill-buffer/BO-cache 全部 PASS；
                                        末项 check-quartzcore-turnip-cache-host.py 需要未提供的
                                        Metal 桥与挂载目标，与本改动无关
源码洁净                                Adreno740 / AdrenoMetal / GopFramebuffer 均 clean，
                                        且 catalog revision == HEAD
```

### 与验收清单的两处偏差（有意为之）

1. 清单写“客户端 PM4 不含 `CP_WAIT_MEM_WRITES`”，而 Phase C 第 3 步**恢复**了它。清单该条是 Phase B 阶段的
   状态描述，已被 Phase C 取代：现在等待包在流里（`audit wait_mem seen 121..191`）且全部兑现。
2. 清单要求 Adreno740 的 revision 能从官方 remote 获取。目前 pin 到本仓 `fuxi` 分支的 `fbb2ddc`：两个通用修复
   （2-dword SMMU 更新、publish policy）尚未上游，而方案本身要求“在 upstream commit 存在之前不要伪装成官方 SHA”。
   这里按方案的 interim 规则诚实 pin 到分支提交，上游化属于“上游更新流程”一节的工作。

### 更正（2026-11-08 18:10）：Phase C 第 5 步的“tiled 可用”结论作废

发现一处流程错误，直接推翻了上面第 5 步的结论：

- `unixnu build payload` **拒绝空的内核参数**：`sheng-tu-debug=` 会报 `unsupported kernel argument` 并失败；
- 我把多条命令用 `;` 串起来又只看了最后一条的退出码，**失败被掩盖**；
- 结果是 16:30 之后所有镜像都复用了 16:30 那份 payload，而它里面是 `sheng-tu-debug=sysmem`。

因此“TU_DEBUG 置空 → tiled 模式 71/71、23/23”这一格**并没有被真正测到**，被刷进去的仍是带 `sysmem` 的镜像。
矩阵里 `sysmem`/`tiled` 的对比需要重做，`nocb` 那一行的因果也需重新确认。

**新增规则（已执行）**：每次构建用 `set -e` 逐步检查，并在刷机前核对
`strings payload/payload.img | grep '<期望的参数>'`，确认镜像里确实是这次改的配置。

当前有效状态：`sheng-tu-debug=sysmem`、`fuxi-touch=4 fuxi-touch-hid=1 fuxi-touch-panic=0`，
镜像 `sha256 7fb74523…`（payload 里已含新的 `fuxi-touch:` 触控上报行）。
