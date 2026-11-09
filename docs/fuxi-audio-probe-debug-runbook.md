# Fuxi Audio Probe 故障隔离与调试 Runbook

## 1. 适用范围

本文用于定位 `FuxiAudioProbe` 访问 Qualcomm SMEM 时造成的 XNU 停机、USB CDC 控制通道失联，以及后续安全地恢复
SMP2P/GLINK 音频 bring-up。它是给后续 AI 和人工执行者的操作规范，不是“照着一次跑完”的脚本。

本轮审计基线：

- 设备仓库分支：`V2`
- 设备仓库提交：`e133ba144095d2f6a32085c969c12afdaa5e9065`
- Linux 语义参考：`fa4ad798f6f681031775266044ba1b0cf3c670bd`
- 禁止使用 Android boot、ADB、刷写或自动设备重启来完成本文的 host 阶段。
- 任何后续真机阶段都必须由设备所有者单独授权，并保留默认安全镜像。

本文区分三类结果：

- **PASS**：观察到了用例预先定义的全部证据。
- **FAIL**：观察到了与预期明确冲突的证据。
- **INCONCLUSIVE**：USB 断开、日志截断、旧镜像、超时或缺少前后快照使结论不唯一。不得把它写成 FAIL，更不得据此猜测 fault PC。

## 2. 当前结论

### 2.1 当前 HEAD 不会自动运行 audio probe

`package/FuxiTouchProbe.cpp` 的 `native_touch_probe()` 已不再调用 `native_audio_probe()`。`FuxiAudioProbe.cpp` 仍被
`scripts/build-fuxi-touch.py` 编进 archive，但当前没有运行入口。因此：

- 从静态调用链看，默认镜像不会因为 `FuxiAudioProbe` 自动阻塞启动；
- 仍需以三次稳定启动完成 G0 设备验收，才能把“代码上不可达”升级为“设备上无回退”；
- 不得为了继续实验而恢复 `native_touch_probe() -> native_audio_probe()`。

### 2.2 “内核线程 + 延迟 20 秒”不是故障隔离

`FuxiAudioProbe.cpp` 目前仍保留 `kernel_thread_start()` 和 `IOSleep(20000)` 的旧实现。它只能把危险访问移到较晚时间，
不能隔离 data abort、panic、死锁、错误 cache alias 或共享内存破坏。probe 线程、USB worker 和系统其余部分仍属于同一内核；
probe 线程出错时，USB 串口和系统会一起死亡。

同理，由 USB 命令触发也不等于 fault containment。USB 触发的价值仅在于：

1. 系统稳定后才允许运行；
2. 每次只运行一个小步骤；
3. host 可以先收到包含 step id 的 ACK；
4. 每步前后可做 PING/STATUS；
5. 默认启动不再自动执行实验。

如果内核中的危险访问 fault，内核仍然会死。需要真正隔离的写实验应优先放进独立诊断环境；若必须在 XNU 中验证页表和
cache 属性，则一次镜像只验证一个最小动作，并接受该实验镜像可能崩溃，但不得影响默认镜像。

### 2.3 已知 SMEM 事实

设备只读证据已经稳定地确认：

```text
SMEM:              0x81d00000 + 0x200000
APPS/ADSP partition: offset 0x19f000, size 0x60000, cacheline 32
item 478:          offset 0x19f030, size 32, first 32 bytes all zero
item 479:          offset 0x19f060, size 16384, first 32 bytes all zero
item 429/443/423/480: absent
partition contents: exactly item 478 and item 479
global partition:  offset 0x1000, size 0xb0000
```

这些事实可用于后续测试的 baseline，但不能推出 item 429/443/423/480 的 fuxi 尺寸、所有权或初始化时机。

### 2.4 现有失败不能归因于一个单一原因

历史样本包括：

| 捕获时间 | 最后可见内容 | 可得结论 |
| --- | --- | --- |
| 00:43、01:02、01:22 | 完整只读报告，后续继续启动；01:22 还有 PING | 只读解析至少在若干运行中成功 |
| 00:58 | 报告停在 item 478 | 只说明最后收到的 USB 文本，不是 fault PC |
| 01:31 | stage 4 首次写入，停在 item 479 | 只读 mapping 上 store fault 是该次事故的合理解释 |
| 06:57 | 可写 mapping、零写，停在 item 479 | 不能由“第一次 store fault”解释 |
| 07:04 | 报告到 global partition，之后失联 | 读取已经完成，仍可能死在 unmap、输出或线程退出附近 |
| 07:13、07:15 | 延迟线程，报告停在 item 479 | 证明线程化没有保护内核；不能证明 item 479 的读本身 fault |

`run()` 先拼接一个约 1.6 KiB 的全局报告，随后 `map->release()`，最后由线程一次性 `IOLog("%s")`。因此日志截断可能来自：

- 一个具体的 SMEM load；
- mapping 生命周期或 cache alias；
- `map->release()`；
- 大块 `IOLog` 和 USB backlog；
- 内核线程返回路径；
- 同时发生的其他启动活动。

在加入逐 checkpoint 之前，不得把“最后一行是 item 479”写成“CPU fault 在 item 479”。

## 3. 与 Linux 主线必须对齐的语义

主线 `drivers/soc/qcom/smem.c` 和 kalama DTS 给出两个当前实现尚未满足的协议要求。

### 3.1 映射属性

Linux 对 SMEM region、partition 和 table 使用 `devm_ioremap_wc()`。当前 probe 使用：

```cpp
IOMemoryDescriptor::withPhysicalAddress(...)
createMappingInTask(..., kIOMapAnywhere | kIOMapReadOnly)
```

这没有证明最终页表属性与 Linux 的 WC 语义相同。仓库内 GPU 代码还记录了一个重要风险：
`IOMemoryDescriptor::map` 的 kernel 路径可能改变物理页的 cache 属性，并影响该页的其他映射。

后续实现不得直接宣称 `VM_WIMG_IO` 等于 Linux WC。必须先做只读属性审计：

1. 查清 Darwin 22 在本树中 `VM_WIMG_WCOMB`、`VM_WIMG_IO`、`ml_io_map_unmappable()` 的实际 PTE/MAIR 语义；
2. 选择不会全局修改同一物理页 cache 属性的 mapping API；
3. 检查同一 SMEM PA 是否已经有另一种属性的 VA alias；
4. 用同一组只读 word 和 checksum 比较 candidate mapping；
5. 在只读 parity 通过前不得写。

### 3.2 hardware spinlock

kalama DTS 明确给出：

```text
TCSR mutex block: 0x1f40000 + 0x20000
SMEM hwlock:      <&tcsr_mutex 3>
```

Linux `qcom_smem_alloc()` 使用 `hwspin_lock_timeout_irqsave()`，超时后返回，并在所有路径释放 lock。当前
`QcomSmem.h` 也明确写着“caller holds the hardware mutex”，但历史 probe 曾在不持锁时调用 `qcom_smem_alloc()`。
“ADSP 尚未运行，所以不存在竞争”不是合法豁免：SMEM 是跨处理器协议，lock 是发布协议的一部分，访问者也不只 ADSP。

在真实 SMEM 上第一次写之前，必须存在并通过测试的 bounded hwspinlock 3：

- acquire 有固定 deadline；
- busy/timeout 立即返回，不得无限轮询；
- 所有错误路径 release；
- 持锁期间不 `IOLog`、不 sleep、不等待 USB；
- 不允许递归 acquire 或 double release；
- lock 的 MMIO mapping 也必须使用明确的 device 属性。

### 3.3 发布顺序

Linux private allocator 的关键顺序是：

1. 填 entry header；
2. `wmb()`；
3. 最后推进 `offset_free_uncached`。

H6 已把 `qcom_smem_alloc()` 收敛为主线 private allocator 的发布形状：allocator 只写四个 entry word，不清零 payload；
完成 entry 后执行一次 release barrier，最后推进 `offset_free_uncached`。协议 payload 的初始化属于后续独立 transaction，
不能隐含在 allocation 中。H6 的 host trace 只证明源代码写集合与顺序，不能把 host array test 的成功当作
interconnect/cache/order 的真机证明。

## 4. 推荐调试架构

### 4.1 两层隔离

**安全层（默认镜像）**：

- audio probe 永远不在 boot path；
- 未显式 ARM 时不创建 worker、不映射 SMEM；
- 只提供状态查询和受限诊断入口；
- 普通系统功能和 USB PING 不依赖 audio state。

**实验层（单步状态机）**：

- 只能在 USB 控制通道已经稳定后 ARM；
- 一个请求最多推进一个不可分割 step；
- 固定 operation id，不接受任意 PA、VA、寄存器或长度；
- 同一时刻最多一个请求；
- 每步有 deadline 和明确的 terminal state；
- 写步骤必须经过更高一级 build-time gate，默认构建不包含写能力。

### 4.2 不要把任意调试能力塞进 USB 协议

建议给 `QcomXnuUsbDebug` 增加一个受限 diagnostic-provider 接口，而不是加入“读任意地址/写任意寄存器”。provider 只暴露：

```text
STATUS       返回当前 state、step、last_rc、generation
ARM          从 IDLE 进入 ARMED；不访问 SMEM
NEXT         ACK 当前 step id，然后排队执行一个 step
CANCEL       只取消尚未开始的 step；不强杀正在访问 MMIO 的线程
RESET        只在 DONE/FAILED 时清理状态；不重启设备
```

如果现有 verb grammar 不带参数，就为上述操作使用固定 verb，或让单一 `DIAGNEXT` 驱动预编译的 audio provider。不得引入地址、
长度、cache mode 等来自 host 的自由参数。

状态机至少包含：

```text
DISABLED -> IDLE -> ARMED -> QUEUED -> RUNNING -> DONE
                                      |          -> FAILED
                     CANCEL ----------+
```

每次 `NEXT` 的同步回复只表示“已接受 step N”，不表示 step 已成功。host 随后用 STATUS/PING 观察 `DONE step=N`。这样即使内核
在 step N 中死亡，host 也已经知道最后被接受的精确 step。不要在危险动作完成后才发唯一一条回复。

### 4.3 checkpoint 规范

每个 step 只允许固定、小尺寸 checkpoint，推荐不超过 64 bytes：

```text
AUDIO gen=3 step=MAP_CREATE phase=BEGIN
AUDIO gen=3 step=MAP_CREATE phase=END rc=0 value=...
```

规则：

- BEGIN 必须在危险动作前被 host 确认收到；
- END 只在动作完成后产生；
- 不在持有 hwspinlock、关闭中断或访问 SMEM 的临界区里输出；
- 不再一次 `IOLog` 1600/4096 bytes；
- 报告正文由 host 根据多个小回复组装；
- 每条记录带 image commit、generation、step id；
- 缺 END 时结果是 FAIL 或 INCONCLUSIVE，取决于是否还能 PING；不是自动等于“该 load fault”。

### 4.4 每步资源所有权

资源必须由一个 context 持有，并显式记录状态：

```text
descriptor_created
mapping_created
va_obtained
hwlock_held
worker_active
last_completed_step
```

每个 step 只改变其中一项。cleanup 也作为独立 step 验证，避免把 create、read、unmap、thread return 混在一个事故窗口中。

## 5. 测试门禁总览

| Gate | 范围 | 是否碰真机 SMEM | 通过后允许 |
| --- | --- | --- | --- |
| H0-H8 | host / 静态 / sanitizer | 否 | 构建只读实验镜像 |
| G0 | 默认镜像基线 | 否 | 启用 NOOP provider |
| G1 | USB/provider 骨架 | 否 | 启用 mapping 生命周期步骤 |
| G2 | 只读 mapping | 只读 | 做 cache 属性 parity |
| G3 | hwspinlock | 只访问 lock MMIO | 做 shadow allocation |
| G4 | shadow allocator | 读取后在普通 RAM 修改 | 申请真实写评审 |
| G5 | 单一真实 allocation | 是，持锁、单 item | 初始化一个 SMP2P item |
| G6 | SMP2P | 是，最小字段写入 | 进入 GLINK/ADSP bring-up |

任何 gate 失败，后续 gate 全部停止。不得“顺手多测一步”。

## 6. Host 与静态测试用例

### H0：默认调用链不可达

- **目标**：证明默认启动路径没有 audio probe 调用者。
- **检查**：全树搜索 `native_audio_probe`；验证只有声明/定义，无调用；检查 boot args 不会被其他入口消费。
- **PASS**：没有调用点；默认配置不注册自动 worker。
- **FAIL**：任何 boot/bsd/touch/display 路径可调用它。
- **下一步**：FAIL 时先删除调用，禁止继续设备测试。

### H1：控制协议 codec 和拒绝策略

- **目标**：证明新增 verb 不破坏 session、sequence、CRC 和 128-byte frame ceiling。
- **输入**：合法命令；CRC 错；未知 verb；空行；分片；拼包；超长；sequence=0；重复 sequence；旧 session；最大 sequence 后回绕。
- **PASS**：合法请求只接受一次；非法请求无副作用；原 PING/STATUS/REBOOT/SCREENSHOT 测试不回退。
- **FAIL**：解析失败后仍推进 audio state，或 replay 能重复执行 step。
- **下一步**：只修 codec/state，不接 provider。

### H2：provider 状态机

- **目标**：验证 DISABLED/IDLE/ARMED/QUEUED/RUNNING/DONE/FAILED 的所有合法与非法迁移。
- **唯一变量**：状态迁移，不包含线程、mapping 或 MMIO。
- **PASS**：非法迁移返回固定错误；每个 generation 的 step 只执行一次；RESET 不会掩盖 RUNNING。
- **FAIL**：NEXT 可并发、可 replay，或 CANCEL 能释放正在使用的资源。

### H3：并发与单请求约束

- **目标**：两个 host 请求、USB retry 和 worker completion 竞争时仍只有一个 owner。
- **输入**：双 NEXT、NEXT+CANCEL、STATUS flood、completion 与 RESET 同时发生。
- **PASS**：精确一次执行；其余返回 BUSY/STALE；无 use-after-free，TSAN/ASan/UBSan 可用检查全部通过。

### H4：deadline 和超时

- **目标**：任何 polling 都有上限。
- **输入**：永远 busy 的模拟 hwlock、永远不完成的 worker、延迟 completion、completion 恰好在 deadline 边界。
- **PASS**：到期进入 FAILED/TIMEOUT；无无限循环；timeout 后资源状态可审计。
- **注意**：不能通过强杀正在 MMIO 临界区的线程实现 timeout。

### H5：SMEM parser 只读 fuzz

- **目标**：不可信 TOC/partition/entry 不得越界。
- **输入**：坏 magic、超大 entry count、offset+size overflow、free pointers 逆序、坏 canary、padding overflow、cached/uncached 相撞、未对齐字段。
- **PASS**：全部返回错误，无 OOB/UB；原始 image 未改变。
- **下一步**：将当前 `qcom_a0_host` 的数组测试扩为 property/fuzz corpus。

### H6：shadow allocation byte-diff

- **目标**：在普通 RAM 上证明 allocator 只修改预期字节。
- **前置**：使用真实只读 snapshot 的脱敏副本或等价构造 image。
- **步骤**：记录 before checksum；只申请一个 item；生成 byte diff；重新解析；再申请同 item。
- **PASS**：diff 只含新 entry header 和最后发布的 free offset；existing 返回 Exists；no-space 完全不改 image。
- **重点**：分别验证“是否清零 payload”的设计决定，不要沿用当前实现而不解释。
- **当前结果**：PASS（`scripts/check-fuxi-audio-probe-h6.py`）。等价构造 image 的未用区域填充 `0xcc`；16 项检查证明
  payload 保持不变、diff 仅落在新 entry header 与 free offset、重复 item/空间不足均为零写入，且 trace 中四个 entry word
  后只有一次最终 publication。它不替代 G4 的真实脱敏 snapshot，也不授权真实 SMEM 写入。

### H7：hwspinlock 模型

- **目标**：证明 bounded acquire/timeout/release 和错误清理。
- **输入**：free、busy 后释放、永远 busy、错误 lock id、double release、第二线程竞争。
- **PASS**：成功路径恰好 acquire/release 一次；timeout 不写 SMEM；所有错误路径 lock 未泄漏。

### H8：mapping helper 合约

- **目标**：在编译和 mock 层固定 mapping API、对齐、长度、属性与 unmap 对称性。
- **输入**：非页对齐 PA、跨页范围、零长度、溢出、重复 map/unmap、create 失败。
- **PASS**：内部 round-down/round-up 正确，返回 VA offset 正确，失败不遗留 partial mapping。
- **限制**：host PASS 不能证明真机 PTE/MAIR 或 cache alias，设备 G2 仍不可省略。

## 7. 后续真机调试用例

以下用例只是执行规范，本轮审计不运行。每项都需要设备所有者单独授权。不得使用 Android boot/ADB；不得自动刷写或自动重启。

### G0：默认镜像安全基线

- **镜像**：当前默认配置，audio provider 不运行，`fuxi-audio-probe` 不作为 boot arg。
- **执行**：三次独立稳定启动；每次在系统稳定后连续 PING/STATUS；记录 image/kernel/device commit。
- **PASS**：3/3 启动完成，USB 控制持续可用，日志无 audio BEGIN。
- **FAIL**：出现 audio 调用证据，或同一版本稳定复现启动失联。
- **INCONCLUSIVE**：镜像 hash 不一致、捕获缺失、设备电源问题。

### G1.1：NOOP 命令

- **目标**：验证 USB -> provider -> worker -> reply，不碰 descriptor/mapping/MMIO。
- **step**：worker 只返回常量和 generation。
- **PASS**：BEGIN、END、DONE 齐全，之后连续 PING 正常。
- **FAIL**：NOOP 本身能拖死 USB，先修控制/线程生命周期。

### G1.2：worker 正常返回

- **目标**：单独验证 kernel thread continuation/return 路径。
- **step**：创建 worker，worker 不 sleep、不映射，立即完成并返回。
- **PASS**：completion 可见，资源计数回到零，PING 正常。
- **说明**：这一步把“线程返回”从 SMEM read/unmap 中拆出。

### G1.3：小日志与回压

- **目标**：排除 1.6 KiB 单次 IOLog 的干扰。
- **输入**：32、64 bytes 固定消息；受控地重复，但保持总量上限。
- **PASS**：无 overflow/backpressure 增长，控制回复不被日志饿死。
- **FAIL**：先修日志/transport，不能进入 mapping 阶段。

### G2.1：descriptor create

- **唯一动作**：为 `0x81d00000 + 0x200000` 创建只读 descriptor，不创建 mapping。
- **checkpoint**：DESCRIPTOR_BEGIN；DESCRIPTOR_END rc；随后单独 cleanup。
- **PASS**：descriptor 非空，release 完成，PING 正常。
- **FAIL**：不要把 descriptor 与 mapping 合并重试。

### G2.2：mapping create/get VA

- **前置**：G2.1 PASS。
- **唯一动作**：create mapping、验证非零 VA；不 dereference。
- **PASS**：MAP_CREATE END 后 PING；随后 G2.5 单独 unmap。
- **FAIL**：记录 candidate API、属性请求、VA；停止。

### G2.3：单 word 读取

- **前置**：已有 mapping；先从已知 TOC magic 或 partition magic 中选择一个 32-bit word，不做循环。
- **唯一动作**：一次 aligned volatile load。
- **PASS**：值与 baseline 一致，END 到达，PING 正常。
- **FAIL**：若 BEGIN 后 USB 死且无 END，可定位到该单 load 窗口；仍需 panic/reset 证据才能称 data abort。

### G2.4：分段只读快照

- **步骤**：每个 USB NEXT 只读一个固定 word 或最多一个 cacheline，顺序覆盖 TOC、partition header、item 478 header、item 479 header。
- **PASS**：所有 word 与已知 snapshot 一致；每步后 PING。
- **禁止**：一次遍历全部 item；一次输出完整报告；在一个 step 内 read+unmap。

### G2.5：unmap/release

- **唯一动作**：先单独 unmap，再由另一 step release descriptor/清 context；不读 SMEM。
- **PASS**：两个 END 均到达，资源位清零，PING 正常。
- **价值**：直接验证 07:04 样本可能涉及的 release 生命周期。

### G2.6：mapping 属性 parity

- **候选**：当前 IOMemoryDescriptor 路径与审计后的 WC/device helper；必须避免同时存在冲突属性 alias。
- **步骤**：每种 mapping 在独立运行中读取同一小型 word set 和 checksum；记录实际 cache attribute（如平台 API 可读）。
- **PASS**：值一致、重复 100 次稳定、unmap 后 PING；没有改变已存在页的全局属性。
- **FAIL**：出现值漂移、alias、属性被全局改写或 release 后失联，停止所有写测试。

### G3.1：TCSR hwspinlock 3 只读/识别

- **目标**：只验证 MMIO mapping 和 lock slot 地址计算，不改变 lock。
- **PASS**：固定寄存器读稳定，mapping 生命周期通过。
- **限制**：地址/slot 计算必须同时由 kalama DTS 和主线 qcom hwspinlock 语义支持。

### G3.2：bounded acquire/release

- **唯一动作**：acquire lock 3；立即 release；临界区不访问 SMEM、不输出、不 sleep。
- **checkpoint**：BEGIN 在 acquire 前；END 在 release 后。
- **PASS**：成功或明确 TIMEOUT；两者之后 PING 正常；成功时 lock 未遗留。
- **FAIL**：失联或 lock 不能恢复，停止。

### G4：真实 snapshot 的 shadow allocation

- **步骤**：通过已验证只读 mapping 把必要 partition 范围复制到普通 kernel buffer；立刻 unmap；在普通 buffer 上申请 item；返回摘要 diff。
- **PASS**：与 H6 相同，且真实 header/free pointers 能被 parser 接受。
- **注意**：所有修改都发生在普通 RAM，不写真实 SMEM。

### G5：第一次真实写入评审与执行

只有 G0-G4 全部 PASS，且代码评审确认 mapping、hwlock、barrier 后才能批准。

- **目标**：只创建一个新 item；不能一次创建 429/443/423/480。
- **item 选择**：建议先评审 429，但其尺寸和生命周期必须由 fuxi 协议证据确认，不能只复制 sheng。
- **前置快照**：partition header、free pointers、item list、目标区域 checksum。
- **临界区**：acquire hwlock 3；重新验证 free pointers；写 entry header；barrier；最后发布 free offset；读回；release。
- **日志**：锁外只返回 before/after 摘要和 rc。
- **PASS**：只出现预期 byte diff；parser 能找到 item；第二次 allocation 返回 Exists；系统和 PING 保持稳定。
- **FAIL**：任何非预期 diff、timeout、失联、lock 泄漏。不得继续下一个 item。
- **恢复**：不得在现场猜测并“写回 free pointer”；使用预先定义的恢复方案或回到安全镜像。

### G6：SMP2P 最小初始化

- **前置**：G5 的 item 429 已稳定存在，尺寸已证实。
- **步骤**：先在 shadow item 上逐 word 验证预期布局；真实 item 的初始化作为一个有界 transaction，在锁内按协议顺序完成，
  锁外只发 BEGIN/END，不把一个可被远端观察的半初始化 header 跨多个 USB 命令暴露。
- **顺序**：magic/version/pids/entry count、`master-kernel` 内容及最终可见性发布必须与协议实现一致；写完后在锁内读回，
  释放锁后再报告摘要。
- **PASS**：重读一致，下次稳定启动仍一致，无 ADSP start、无 GLINK traffic。
- **FAIL**：停止，不创建 443，不启动 ADSP。

## 8. 故障分类规则

| 现象 | 分类 | 下一步 |
| --- | --- | --- |
| BEGIN/END 都到，PING 正常，值正确 | PASS | 进入下一 gate |
| BEGIN 到，END 不到，PING 仍正常 | FAIL（step 自身未完成） | 查 worker 状态/deadline，不重跑更大步骤 |
| BEGIN 到，END 不到，PING 也死 | FAIL（内核级故障窗口） | 收集 panic/reset cause；最小化该 step |
| BEGIN 都未到，USB 断 | INCONCLUSIVE | 先查 transport/host，不归因于 probe |
| END 到但随后 USB 死 | FAIL（cleanup/return/output 窗口） | 把 cleanup、worker return、日志分别拆步 |
| 只有旧式大块 IOLog 截断 | INCONCLUSIVE | 不得指定 fault PC |
| host test PASS、设备失败 | 设备语义 FAIL | 查 mapping/cache/MMIO/order，不能用 host test 覆盖 |

## 9. 明确禁区

后续 AI 不得：

1. 恢复 `native_touch_probe() -> native_audio_probe()`；
2. 用 `IOSleep(20000)` 或另起内核线程宣称已经隔离 fault；
3. 宣称 USB 触发能保护内核不被 probe fault 拖死；
4. 从日志最后一行反推 fault PC；
5. 在没有 hwspinlock 3 时调用真实 `qcom_smem_alloc()`；
6. 在持锁、关中断或 MMIO 临界区里 IOLog、sleep 或等待 USB；
7. 一次申请 429、443、423、480 四个 item；
8. 把 sheng 的 item size、初始化时机和生命周期当作 fuxi 事实；
9. 暴露任意地址/任意寄存器的 USB debug verb；
10. 使用 `scripts/fuxi-audio-probe.sh run` 的自动设备路径；
11. 使用 Android boot、ADB、自动刷写或自动重启；
12. 在默认镜像中保留可自动执行的写 probe。

## 10. 每轮记录模板

```text
test_id:
result: PASS | FAIL | INCONCLUSIVE
device_commit:
kernel_commit:
image_sha256:
control_protocol_revision:
boot_args:
generation:
step_id:
mapping_api:
requested_cache_attribute:
observed_cache_attribute:
smem_before_checksum:
smem_after_checksum:
hwlock_acquire_rc:
hwlock_release_seen:
begin_seen:
end_seen:
ping_before:
ping_after:
deadline_ms:
last_small_checkpoint:
panic_or_reset_evidence:
unexpected_diff:
classification:
next_single_measurement:
```

Host、build 和 device 结果必须分开写。`host PASS` 不能写成 `device PASS`。

## 11. 给后续 AI 的任务提示模板

```text
你正在处理 unixnu-fuxi 的 audio probe。先完整阅读：
1. docs/fuxi-audio-probe-debug-runbook.md
2. package/FuxiAudioProbe.cpp
3. package/QcomSmem.c / QcomSmem.h
4. drivers/QcomXnuUsbDebug/kernel/QcomUsbControl.h
5. Linux mainline drivers/soc/qcom/smem.c 与 kalama.dtsi

约束：
- 不恢复任何 boot-path audio probe 调用。
- 不执行 boot、fastboot、ADB、刷写或自动重启。
- 本次只完成测试 ID <ID>，不得顺手推进下一 gate。
- 先写/运行 host test，再改实现。
- 固定 operation id，不接受 host 提供任意地址或长度。
- 输出必须包含：改动文件、host 测试、未验证事项、PASS/FAIL/INCONCLUSIVE、下一次唯一测量。
- 若需要真机，停下来列出镜像 hash、精确 step、预期 checkpoint 和恢复方案，等待设备所有者授权。
```

## 12. 推荐执行顺序

最小成熟路线是：

```text
修正文档和静态门禁
  -> H1/H2 控制协议与状态机
  -> H3/H4 并发和 timeout
  -> H5/H6 SMEM parser 与 shadow diff
  -> H7 hwspinlock 模型
  -> H8 mapping helper 合约
  -> G0 默认启动基线
  -> G1 NOOP/线程/小日志
  -> G2 descriptor/map/read/unmap 分步
  -> G2.6 cache 属性 parity
  -> G3 hwspinlock acquire/release
  -> G4 真实 snapshot 上的 shadow allocation
  -> 评审
  -> G5 单一 item 的真实 allocation
  -> G6 SMP2P 最小初始化
```

在 G5 之前，目标不是“尽快写进 SMEM”，而是把所有可能让系统和串口同时死亡的动作缩成一个能够唯一归因的最小步骤。

## 13. 执行记录

### H1-H4 与 G1.1-G1.2：NOOP 控制通道和一次性 worker 已完成真机验收

- CDC 新增固定 verb：`DIAGSTATUS`、`DIAGARM`、`DIAGNEXT`、`DIAGCANCEL`、`DIAGRESET`。
- provider 固定为 `noop`；`DIAGNEXT` 只创建一次性空 worker，不创建音频线程、不映射 SMEM、不访问 MMIO。
- 只有 ACK 完整入队并提交 control sequence 后，状态才进入 queued；随后单步完成为 done。
- 回压不会消费 control sequence，也不会改变诊断状态。
- host 模型与 codec：31 项通过；独立 ARM64 模块归档构建通过。
- 真机镜像：kernel SHA-256 `c793f715fa99ba6da2d8f8005c8981b939a74d54952048e384af1580c2bd8ca6`；
  boot.img SHA-256 `5e068a4b525f4c86f29bdb6aa1ab62b3f0653bc846f6fb8c85cf27e9ccd14d24`。
- 真机 G1：`idle -> armed -> queued -> done -> idle`；每次回复均为 `provider=noop rc=0`，之后 PING 正常。
- H3/H4：单 ticket 独占、重复 ARM/NEXT、回压、旧 generation completion、deadline 边界、迟到 completion
  与 deadline 饱和均有主机测试；timeout 不强杀线程，活动 ticket 返回前拒绝 RESET。
- G1.2 最终安全镜像：kernel SHA-256 `60d4370f583b9b68b0f1fc57fc51118bc29191fe9da3ca8d38f104fbb5562120`；
  boot.img SHA-256 `c871ff6f18ff5ccc3f8750c53eedf3006dc65988718c6ef6c647bfaa64e07a8f`。
- 真机 worker 证据：`claims 0 -> 1`、`finishes 0 -> 1`、`active=0`、`timeouts=0`；第二次 ARM 返回
  `ERR rc=-1` 且 generation 不变；RESET 后 idle，PING 正常。捕获目录：
  `/tmp/fuxi-usb/g12-safe-final-20261109-091835`。
- H0 已恢复 PASS：默认 boot path 不再调用 `native_audio_probe()`；最终捕获无 `AUDIO step=`。
- 本轮仍未建立 descriptor、mapping 或 SMEM 访问；G2 及以后保持未开始。

### 构建与启动事故记录：Data seed UUID 和 CDC staging

- `kern_exec.c:8196` 的 `offline scratch Data mount failed` 不是 DIAG fault。失败镜像把
  UUID 为 `E095FBF5-B5BA-43CE-ABD7-BF2618B0158C` 的 `out/ipsw/21H461/data.dbr`
  与绑定 `B2A023C5-CC73-45C1-A9E6-26AC8BDD2822` 的内核组合，
  `native_ram_block_volume_seed()` 因 UUID 不匹配找不到卷。
- 改回 `out/fuxi/build/owner-data-fixture/owner-data-16g.dbr` 后，真机日志确认
  `disk1s1` UUID 为 `B2A023C5-CC73-45C1-A9E6-26AC8BDD2822`、Data mount-complete，随后执行 `/sbin/launchd`。
- UniXNU payload 构建现在会在 packed seed 的 APFS UUID 与内核配置不同时直接失败；正确 seed 的
  `payload.json` 记录 `data_volume_uuid=B2A023C5-CC73-45C1-A9E6-26AC8BDD2822`，错误 seed 不再能进入 boot.img。
- `build-fuxi-usb-debug.py` 原先只在 staging 目录不存在时复制独立 CDC 仓库；因此子仓库工作区的新 DIAG
  已独立编译，却没有进入完整内核。staging 记录现在包含模块工作区 SHA-256；源码或消费端适配器变化时只重建
  该生成目录，失败留下的半成品 staging 也不会被下一轮复用。
