# Fuxi 音频线完整实施路线（从安全探针到日用级）

更新日期：2026-11-09  
适用对象：Xiaomi 13 fuxi / SM8550、UniXNU、iPadOS 17.7.11（21H461）  
状态词只允许：**PASS / FAIL / INCONCLUSIVE**。Host PASS、build PASS 和 device PASS 必须分别记录。

本文是后续执行清单，不替代以下资料：

- `docs/fuxi-audio-porting-plan.md`：架构、模块和长期设计；
- `docs/fuxi-audio-probe-debug-runbook.md`：危险真机步骤的隔离规则；
- `docs/fuxi-audio-probe-debug-log.md`：已经执行的逐轮证据；
- `facts/audio/manifest.toml` 与 `facts/audio/binding.toml`：事实和板级参数的唯一数据源。

## 1. 当前进度

### 1.1 已经完成并有证据的部分

| 项目 | 状态 | 当前证据 |
| --- | --- | --- |
| 默认启动不调用 audio probe | PASS | H0；`native_audio_probe()` 没有启动路径调用者，当前真机日志无 `AUDIO step=` |
| CDC 固定诊断协议 | PASS | H1–H4；generation、唯一 ticket、重复 claim/stale completion 拒绝、250 ms deadline |
| 一次性内核 worker | PASS | G1.1–G1.3；三步真机 `claims=3 finishes=3 active=0 timeouts=0`，之后 PING 正常 |
| Descriptor 生命周期 | PASS（host/device） | 固定 `0x81d00000+0x200000`；create 后 resources=1，release 后 0；无 mapping/read |
| SMEM 只读 parser | PASS（host） | H5；15 项 corpus/fuzz，ASan/UBSan，无越界、原 image 不变 |
| SMEM shadow allocator | PASS（host） | H6；16 项精确 byte diff，payload 不清零，free offset 最后且唯一发布 |
| 真机 SMEM 只读事实 | PASS（device/read-only） | `0x81d00000 + 0x200000`；APPS/ADSP partition `+0x19f000/0x60000`、cacheline 32；item 478/479 已存在，429/443/423/480 不存在 |
| 板无关音频模型 | PASS（host） | 16 套、20,592 项、0 失败；含 MDT、SMEM/SMP2P/GLINK、ADSP、GPR/APM、period、CS35L41、HAL ABI 模型和 fuzz |
| GPU/Metal 平台基线 | PASS（device） | G2.1 后 client calls 1007、fence 73/73、GOP 24/24、CP/SMMU fault 0 |

当前已刷镜像的关键身份：

```text
kernel.macho sha256 7db8751ceed8a2f1742803fd721da15bf6ea7c9606b965814ab50320d4301e58
boot.img    sha256 adae45c83112b2c625411c7d9185bc9db2817242f022c79973c4103681db2656
GPU addon   sha256 e4b4b7062c03eea7301f9f830ad45d2d5e25c80875dfc3efdd0848c0ec639288
Metal trust sha256 8c2b907f4ef4a813a4092c0b4e724a05d90051772fe13db2082301bd4929d913
Data seed   sha256 efcde232f6563faf955631a3a81f4ba4a16202ff821dc3ea32ea5ae88ada7e40
Data UUID          B2A023C5-CC73-45C1-A9E6-26AC8BDD2822
```

### 1.2 尚未完成的部分

| 层级 | 状态 | 仍缺少的第一性证据 |
| --- | --- | --- |
| H7/H8 | PASS（host） | H7 32 项、TCSR 编码 23 项；H8 mapping helper 31 项，均经 ASan/UBSan |
| G1.3 | PASS（host/device） | 真机精确 100 条 32-byte + 100 条 64-byte；PING 成功，bp/bo 维持 0/0 |
| G2 | G2.1 PASS；其余未完成 | descriptor 生命周期已关闭；仍缺 map/单 word read/unmap 与 cache parity |
| G3 | 未完成 | TCSR hwspinlock 3 的真机识别、bounded acquire/release |
| G4 | 未完成 | 从真实只读 snapshot 复制到普通 RAM 后的 shadow allocation |
| G5/G6 | 未完成 | 单一真实 SMEM allocation 与 SMP2P 最小初始化 |
| A0 | 未完成 | fuxi ADSP 固件、PAS/DTB、carveout、ready/fatal/handover、stop/SSR |
| A1 | 未完成 | 真机 GLINK/GPR/APM/SPF ready 和事务恢复 |
| A2 | 未完成 | 真机静音 buffer map、period 和 teardown |
| A3 | 未完成 | CS35L41 真机身份/PLL/fault、speaker graph 和第一声 |
| A4 | 未完成 | 21H461 CoreAudio ABI 与 10 分钟稳定输出 |
| A5 | 未完成 | WCD938x/SoundWire、mic route 和真实采集 |
| A6 | 未完成 | 保护、校准、SSR、睡眠唤醒和长稳 |

`facts/audio/binding.toml` 当前为 **107 unverified / 0 verified**：8 项引用已有资料，8 项是 Sheng 的 SM8550 预测，91 项没有来源。
引用资料不等于真机验证；任何消费板级 binding 的构建必须通过 `--require-verified`，不能用默认值绕过。

### 1.3 工程距离估算

以下是剩余工程量估算，不是功能完成率，也不能作为验收：

- 安全 bring-up 工具链：约 55%；S0–S3 已关闭，下一项为 S4/G2.2 mapping create/getVA；
- A0（ADSP 在线）：约 10–15%；
- A3（第一声）：约 5–10%；
- A4（可用 CoreAudio 输出）：低于 5%；
- A5/A6：尚无真机功能完成度。

## 2. 不可破坏的执行规则

1. 不恢复 `FuxiTouchProbe -> native_audio_probe()`，audio provider 永远不在默认 boot path。
2. 不使用 Android boot、ADB 或 Android runtime。需要的固件和事实必须来自可复现文件、主线源码、固定设备树或 UniXNU 真机只读测量。
3. 真机实验只能通过固定 operation ID 的 CDC provider 推进；不得接受主机传入任意 PA、VA、寄存器或长度。
4. 一次 `DIAGNEXT` 只做一个不可分割动作；BEGIN、END、generation、step、deadline、资源位必须可审计。
5. timeout 不强杀正在访问 MMIO 的线程；活动 ticket 返回前拒绝 RESET。
6. G2 前不读取 SMEM；G5 前不写真实 SMEM；A3 前功放保持 reset 或 mute。
7. hwspinlock 临界区内禁止日志、sleep、USB 等待、内存分配和无界轮询。
8. 每个真机 gate 失败后回到当前 GPU/Metal 安全基线，不把多个动作合并后“再试一次”。
9. GPU/Metal、触摸、Data mount、CDC 是每轮实验的共存回归项，不允许为了音频临时删掉。
10. 不把 Sheng 的地址、bit、通道名或时序从 `unverified` 改为 `verified`，除非 fuxi 自己的证据支持。

## 3. 所有实验镜像的强制打包契约

每轮音频实验必须保留正确 Data、GPU addon 和 Metal owner trust。基础命令为：

```sh
cd /Users/mac/Documents/deepseek-harness/default-workspace/UniXNU

./unixnu build kernel -D ../unixnu-fuxi

./unixnu build payload -D ../unixnu-fuxi --uefi ../uefi-fuxi \
  --unique-chip-id 0x0123456789abcdef \
  --owner-trust-cache out/fuxi/ipados-17.7.11-21H461/fuxi/tree/build/adreno-metal-plugin/owner-trust.bundle \
  --data-ramdisk out/fuxi/build/owner-data-fixture/owner-data-16g.dbr \
  --gpu-addon out/fuxi/ipados-17.7.11-21H461/fuxi/tree/build/fuxi-gpu-addon.ngpu

./unixnu bootimg -D ../unixnu-fuxi --uefi ../uefi-fuxi --target DEBUG \
  --dtb ../uefi-fuxi/Platforms/Xiaomi/fuxiPkg/FdtBlob/sm8550-xiaomi-fuxi.dtb
```

构建后必须读取 `payload.json`，确认：

- `gpu_addon_sha256` 非空；
- `trust_cache_sha256` 等于本轮指定 owner bundle；
- `data_volume_uuid` 为 `B2A023C5-CC73-45C1-A9E6-26AC8BDD2822`；
- `kernel_args` 不含 `fuxi-audio-probe` 或其他自动音频启动参数；
- 记录 kernel、payload、boot.img 三个哈希。

UniXNU 已增加打包门禁：命令行含 `sheng-gpu-addon=1` 时，缺 `--gpu-addon` 或 `--owner-trust-cache` 必须构建失败。

## 4. 阶段总图

| 阶段 | 目标 | 写硬件/共享内存 | 入口条件 | 出口 |
| --- | --- | --- | --- | --- |
| S0 | 冻结安全基线 | 否 | 当前镜像 | 3/3 稳定启动与回归记录 |
| S1 | H7 hwspinlock 模型 | 否，host | H6 PASS | **PASS：32 项策略 + 23 项 TCSR 编码** |
| S2 | H8 mapping 合约 + G1.3 | 仅 host/NOOP | S1 PASS | **PASS：H8 host 与 G1.3 host/device 均关闭** |
| S3 | G2.1 descriptor 生命周期 | 否 | S2 PASS | **PASS：create/release 分步、资源归零、map 仍拒绝** |
| S4 | G2.2–G2.5 最小只读 mapping | 只读 | S3 PASS | map/read/unmap 分步 PASS |
| S5 | G2.6 cache 属性 parity | 只读 | S4 PASS | 属性/alias 风险关闭 |
| S6 | G3 hwspinlock 真机 | lock MMIO | S5 PASS | bounded acquire/release PASS |
| S7 | G4 真实 snapshot shadow | 真实 SMEM 只读；普通 RAM 写 | S6 PASS | 真实布局 H6 parity |
| S8 | G5 单一真实 allocation | 是，单 item | 评审批准 | item 429 allocation PASS |
| S9 | G6 SMP2P 最小初始化 | 是，单 transaction | S8 PASS | outbound item 重读稳定 |
| S10 | 固件与板级事实闭环 | 默认只读 | S9 或可并行的文件审计 | 固件/hash/PAS/carveout verified |
| S11 | A0 ADSP 在线 | 是 | S9+S10 PASS | 20 次 start/stop，ready/SSR 可控 |
| S12 | A1 控制面 | 是 | A0 PASS | GLINK/GPR/APM 稳定 |
| S13 | A2 静音数据面 | 是，不开功放 | A1 PASS | 1000 period、完整 teardown |
| S14 | 功放 muted-ready | I2C/GPIO，保持 mute | A2 PASS | 两颗 CS35L41 身份/PLL/fault PASS |
| S15 | A3 第一声 | 是，低能量输出 | S13+S14 PASS | 短低音量 tone 与 100 次启停 |
| S16 | A4 CoreAudio output | 是 | A3 PASS | 系统输出 10 分钟稳定 |
| S17 | A5 microphone | 是 | A2 PASS，可与 S15/S16 调研并行 | 200/200 真实采集 period |
| S18 | A6 日用级 | 是 | A4+A5 PASS | SSR/睡眠/长稳/保护全部通过 |

任何阶段只有在证据写入调试日志后才算完成；代码存在、编译成功或 host 测试通过都不能替代 device PASS。

## 5. S0：冻结平台安全基线

### 任务

1. 保存当前 kernel、payload、boot.img、GPU addon、trust cache、Data seed 哈希。
2. 连续执行三次独立启动；每次等待 Data mount、launchd、SpringBoard 和 Metal 活动。
3. 每次记录 CDC PING/STATUS、Metal opens/client calls、fence submitted/retired、GOP offer/done、CP/SMMU fault。
4. 确认日志无 `AUDIO step=`、panic、reset、Data UUID mismatch。
5. 保存可恢复 boot.img；不得覆盖唯一副本。

### PASS

- 3/3 Data mount 和 launchd；
- 3/3 CDC PING；
- 每次 fence submitted=retired，GOP offer=done，fault=0；
- 无 audio 自动入口。

### 失败与回退

任一轮失败则结论为 FAIL 或 INCONCLUSIVE，先恢复本节记录的 boot.img。S1 可以继续做 host 工作，但禁止进入 S3 真机实验。

## 6. S1：H7 bounded hwspinlock host model

### 实现任务

1. 新建独立的 `QcomHwspinlock` board-independent helper，不把 TCSR 地址写进通用层。
2. API 明确区分 `acquire(deadline)`、`release()`、`owned()`；错误 lock id 在任何 MMIO 前失败。
3. acquire 使用固定尝试预算或绝对 deadline；不得无界循环。
4. owner token 防止递归 acquire、非 owner release 和 double release。
5. SMEM transaction wrapper 确保所有 parser/space/error 路径都 release。

### 测试矩阵

- 初始 free；
- busy 若干轮后释放；
- 永远 busy；
- deadline 前一 tick 与边界 tick；
- 错误 lock id；
- recursive acquire；
- double release；
- 两线程竞争；
- acquire 成功后 SMEM parser/NoSpace/Exists/Corrupt 各自退出；
- timeout 时 SMEM byte diff 必须为零。

### PASS/产出

- ASan/UBSan 和可用的线程 sanitizer 全通过；
- 成功路径恰好一次 acquire/release；所有失败路径无锁泄漏；
- `facts/audio` 仍不新增真机 verified 字段；
- H7 记录进入 `fuxi-audio-probe-debug-log.md`。

## 7. S2：H8 mapping helper 与 G1.3 日志门禁

### H8 任务

1. helper 输入只能来自编译期 operation 表；禁止控制协议传 PA/length/attribute。
2. 对 PA 向下页对齐、末端溢出检查、长度向上对齐，返回 caller-visible VA offset。
3. create descriptor、create map、get VA、unmap、release 分别有资源位。
4. 失败时逆序清理；重复 unmap/release 返回固定错误且无副作用。
5. cache attribute 作为显式 enum，不把 `VM_WIMG_IO` 自动等同 Linux WC。

### H8 host 用例

- 零长度、非页对齐、跨页、`PA+length` 溢出；
- descriptor/map/getVA 任一点失败；
- 重复 map/unmap；
- VA offset 计算；
- cleanup 顺序和资源计数。

### G1.3 真机用例

固定 NOOP worker 分别生成 32 和 64 bytes 消息；总次数和总字节有上限。检查 CDC overflow/backpressure、控制回复延迟和 PING。

### PASS

H8 sanitizer 全通过；G1.3 无 overflow/backpressure 增长，控制通道不被日志饿死。失败时不得进入 descriptor 阶段。

## 8. S3–S5：G2 只读 SMEM mapping

每个子步骤使用新的 generation；上一步 PASS 才能执行下一步。

### G2.1 descriptor create/release

- 固定范围：`0x81d00000 + 0x200000`；
- 只创建 read-only descriptor，不创建 mapping、不 dereference；
- create 和 release 分成两个可审计动作；
- PASS：descriptor 非空、资源位归零、随后 PING。

### G2.2 mapping create/get VA

- 只创建 mapping 并验证非零 VA，不读取；
- 记录 API、请求属性、对齐后的范围和 VA offset；
- mapping 保持到下一固定 cleanup step，不让局部变量隐式释放。

### G2.3 单 word read

- 只读一个已知、对齐的 TOC magic word；
- 不循环、不解析、不打印大报告；
- PASS：值与已知 snapshot 一致、END 和 PING 均到达。

### G2.4 分段 snapshot

每个 NEXT 最多读取一个 cacheline，依次覆盖 TOC、partition header、item 478 header、item 479 header。每步保存 offset、word、checksum。

### G2.5 unmap/release

unmap 和 descriptor release 分成两个 NEXT；任何 END 后失联都归因于对应 cleanup 窗口。

### G2.6 cache/alias parity

1. 审计 Darwin 22 的 mapping 实现、PTE/MAIR 和物理页 cache 属性改变行为。
2. 候选 API 在独立启动中读取同一组固定 word/checksum；禁止同一时刻创建冲突属性 alias。
3. 每个候选重复 map/read/unmap 100 次。
4. PASS：读值一致、无漂移、无全局属性改变、无 cleanup 失联。

任何值漂移、alias 或 release 后失联都停止所有 SMEM 写测试。

## 9. S6：G3 TCSR hwspinlock 3

### G3.1 只读识别

- 地址必须同时由 kalama DTS 与主线 qcom hwspinlock 语义支持；当前参考是 TCSR block `0x1f40000 + 0x20000`、slot 3，执行前重新审计；
- 只 mapping 和读取固定寄存器，不写 lock；
- 记录 slot stride、owner/free 编码和重复读稳定性。

### G3.2 bounded acquire/release

- BEGIN 在 acquire 前，END 在 release 后；
- acquire 后立即 release，临界区不访问 SMEM；
- busy 必须在 deadline 返回 TIMEOUT；
- 成功和 TIMEOUT 后都必须 PING；
- 下一轮只读确认 lock 未遗留。

失联或 lock 不能恢复时停止，不尝试“写 0 修复”未知状态。

## 10. S7：G4 真实 snapshot 上的 shadow allocation

1. 使用已经验证的只读 mapping，只复制必要 partition 范围到普通 kernel RAM。
2. 立即 unmap/release；后续 allocator 只能操作普通 RAM 副本。
3. 对副本执行与 H6 相同的单 item allocation。
4. 返回摘要：before/after checksum、diff ranges、旧/新 free offset、item id/size、unexpected byte count。
5. 再执行 Exists 和 NoSpace，要求零写。

PASS 要求真实 header/free pointer 被 parser 接受，diff 与 H6 完全同形。此阶段仍然不写真实 SMEM。

## 11. S8–S9：第一次真实 SMEM transaction 与 SMP2P

### S8/G5 评审前置

必须同时满足：S0–S7 PASS；mapping/cache/hwlock/barrier 代码审查完成；保存真实 before snapshot 和恢复镜像；明确 item 429 的尺寸来自 fuxi 证据而不是只来自 Sheng。

### G5 唯一 transaction

1. acquire hwspinlock 3；
2. 锁内重读 free pointers 和目标 item 是否存在；
3. 写四个 entry header word；
4. release barrier；
5. 最后推进 free offset；
6. 锁内读回；release lock；
7. 锁外报告摘要。

只允许创建一个 item。不得同时创建 429/443/423/480。第二次请求必须返回 Exists 且零写。

### G6 SMP2P outbound 初始化

- 在普通 RAM shadow 上先形成完整 layout；
- 真实初始化必须是一个持锁 transaction，不跨多个 USB 命令暴露半初始化 header；
- 只初始化 outbound item 和 `master-kernel` 所需最小字段；
- 不创建 inbound 443、不启动 ADSP、不发送 IPCC；
- 重启后只读确认布局仍一致。

任何 unexpected diff、lock 泄漏、失联或跨重启不一致都停止，禁止继续创建其他 item。

## 12. S10：固件和 107 个 binding 字段的闭环

### 12.1 固件资产

1. 确定 `adsp.mbn`、`adsp_dtb.mbn` 或 split `.bNN` 的可复现来源；不从 Android runtime 临时抓取。
2. 保存来源、版本、完整 SHA-256、文件大小和许可边界，不把 vendor blob 提交进源码仓库。
3. 用 `QcomMdt` 只读解析 program headers、hash segment、loadable extent 和 entry。
4. 与 fuxi 固定 reserved-memory/DT 交叉验证 carveout，不允许只采用 Sheng 数值。
5. 确认 PAS ID、DTB PAS ID、remote PID、load-state 名称。

### 12.2 binding 分批关闭

| 批次 | 字段类别 | 允许的验证来源 | 完成条件 |
| --- | --- | --- | --- |
| B0 | SMEM、item、cacheline、remote PID | G2/G4 真机只读 + DTS | 字段逐项附 capture/hash |
| B1 | carveout、PAS、固件 hash、ready/fatal/handover | 固件解析、DTS、A0 真机 | 不再依赖 Sheng prediction |
| B2 | GLINK channel、GPR/APM port/module/interface | 固件应答与真机 trace | 每个常量有实际应答证据 |
| B3 | I2C/GPIO/TDM/slot/clock/boost | 固定 DTS/原理资料 + 真机读回/波形 | 两颗 amp 分别确认 |
| B4 | ring、period、host clock、21H461 HAL ABI | ABI 审计 + 真机时间线 | 24 项 HAL 审计关闭 |
| B5 | WCD/SWR/micbias/ADC/mic route | SoundWire 枚举与真实采集 | 目标麦 route 有证据 |

每次只能把有证据的字段改成 `verified`；运行 `check-fuxi-audio-binding.py --require-verified` 的最终 PASS 是 A4/A5 集成前置，不是早期阶段的伪目标。

## 13. S11：A0 ADSP 在线

### 子阶段

1. **A0.1 固件只读计划**：解析和 placement plan，不写 carveout。
2. **A0.2 carveout descriptor/map**：复用 G2 方法，先 create/map/unmap，不复制固件。
3. **A0.3 segment copy/readback**：DSP 保持 reset；逐 segment hash/readback。
4. **A0.4 PAS init/auth**：只执行 secure authentication，记录 SCM x0/x1；失败不 start。
5. **A0.5 DTB auth/load**：与主固件分开记录。
6. **A0.6 power/load-state**：QMP/RPMh 每个 vote 独立、有 before/after。
7. **A0.7 start + ready**：启动后只等待有界 SMP2P ready；fatal/crash item 单独读取。
8. **A0.8 stop**：请求 stop、等待 ack、shutdown/reclaim；资源计数回零。
9. **A0.9 SSR**：在基础 start/stop 稳定后才注入 restart。

### A0 PASS

- 20/20 start/stop；
- ready、fatal、handover、stop ack 的位和时序均有真机证据；
- 无 AP panic、SMEM corruption、lock 泄漏和 carveout 越界；
- crash reason 可读取；失败能回到可再次启动状态。

## 14. S12：A1 GLINK、GPR 与 APM 控制面

1. 初始化 478/479/480 descriptor/FIFO，先完成 GLINK version negotiation。
2. 打开固件实际公布的 channel；intent 数量和大小有上限。
3. 加入 IPCC doorbell，但 ISR 只记事件并唤醒 worker，不解析大包。
4. GPR 先做 basic result/错误请求，再做 SPF readiness。
5. APM 每次只允许一个 transaction；token 包含 generation，旧 generation reply 丢弃。
6. 对 malformed、timeout、remote close、SSR 分别测试。

PASS：channel/intent 计数稳定；100 次控制事务无超时；SSR 后旧 reply 不污染新 generation；所有 FIFO/map 可 teardown。

## 15. S13：A2 静音数据面

1. 功放保持 reset/mute，speaker GPIO 不解除。
2. 建 graph/open/config/map/start，但 buffer 全部写零。
3. period 地址、map handle、token、generation、deadline 分别记录。
4. 从单 in-flight period 增到最多 4 个；每次扩窗前上一档 100% 兑现。
5. 验证乱序 completion、late、timeout、cancel、stop/close/unmap。
6. 跑 1000 个 silent period 和 100 次 start/stop。

PASS：submitted=completed+cancelled；无 underrun/overrun/OOB；停止后无 mapping、无 outstanding period、DSP graph 关闭。

## 16. S14–S15：CS35L41 muted-ready 与 A3 第一声

### S14 功放安全 bring-up

对 B/T 两颗功放分别执行，不允许一次同时解除 reset：

1. 验证 GENI I2C firmware、clock 和 transfer timeout；
2. GPIO reset 时序；
3. 读 DEVID/REVID/OTP 状态；身份不符立即停止；
4. 应用对应 revision errata 和 OTP trim；
5. 校验 boost 参数来源和范围；
6. 每个关键写 readback；
7. 配 PLL/TDM 后等待 lock；
8. 清读 fault，保持 mute，进入 `MUTED_READY`；
9. 单颗 100 次 reset/identify/configure/mute。

### S15 第一声

1. A2 静音链稳定后，先只启用一颗功放、一个声道。
2. 固定 48 kHz、受验证 sample width/slot；不得用猜测 slot。
3. 生成不超过 500 ms、明显衰减的 tone；先最低安全增益。
4. unmute 前最后检查 PLL/fault/temperature/boost；异常则拒绝 unmute。
5. 播放后立即 mute，再 stop graph/unmap。
6. 分别验证 B/T，再验证 stereo 和声道方向。

A3 PASS：两颗身份和 revision 正确；无 critical fault/过温/削顶；声道正确；100 次启停无 pop、无残留 graph/map。

## 17. S16：A4 CoreAudio output

1. 完成 21H461 音频服务器 ABI 的 24 项审计，不直接采用 Sheng 的 iPadOS 26/27 arm64e 细节。
2. 固定 service/bundle identity、entitlement、签名和加载方式。
3. 共享页发布 magic/version/size/ring/timeline；consumer 必须先 validate 再读字段。
4. ring size 必须是 period size 的整数倍；禁止 `65536 frames / 480 frames` 组合。
5. 首先只公布 stereo output，不提前公布不可用 input。
6. 测试系统音效、音乐、暂停/恢复、客户端退出、route 重建。

A4 PASS：系统应用可选择设备；连续 10 分钟输出无 underrun/overrun；暂停恢复正常；服务退出或 SSR 时先 mute，再按 generation 重建。

## 18. S17：A5 microphone

1. 只读枚举 WCD938x 和 SoundWire devices/ports。
2. 验证 TX macro power/clock、micbias、ADC、decimator 和目标麦 slot。
3. 先 DSP 内部静音/loopback，再连接真实 microphone。
4. 记录每个 period 的 nonzero ratio、RMS、peak、clipping、DC offset。
5. HAL 只有在 200 个真实 period 通过后才公布 input stream。

A5 PASS：200/200 period；信号非零且不过载；Voice Memos 连续 start/stop；输出与输入并发不破坏各自 timeline。

## 19. S18：A6 日用级收束

### 必测矩阵

- 1000 次 output start/stop；
- 20 次 ADSP SSR；
- 20 次音频服务重启；
- 20 次睡眠/唤醒；
- 1 小时连续播放；
- 30 分钟连续录音；
- 输出/输入并发；
- underrun、remote close、功放 fault、PLL unlock、过温模拟；
- 每次恢复验证 mute → generation++ → teardown → rebuild → unmute 顺序。

### PASS

无 panic、SMMU fault、SMEM corruption、锁泄漏、旧 generation completion；保护事件一定先 mute；恢复后计数和资源回到定义状态。只有 S18 PASS 后才能把音频标为“日用可用”。

## 20. 每一阶段的交付物

每个阶段必须同时交付：

1. 最小代码 diff；
2. 对应 host 测试及原始输出；
3. kernel/payload/boot.img 哈希；
4. 捕获目录和关键日志行；
5. binding/manifest 的证据更新；
6. PASS/FAIL/INCONCLUSIVE 判断；
7. 未验证事项；
8. 下一次唯一测量；
9. 明确回退镜像；
10. GPU/Metal、Data、CDC、触摸共存回归结果。

建议记录模板：

```text
stage:
test_id:
result: PASS | FAIL | INCONCLUSIVE
device_commit:
cdc_commit:
kernel_sha256:
payload_sha256:
bootimg_sha256:
gpu_addon_sha256:
trust_cache_sha256:
data_uuid:
generation:
step_id:
deadline_ms:
begin_seen:
end_seen:
ping_before:
ping_after:
resource_bits_before:
resource_bits_after:
smem_before_checksum:
smem_after_checksum:
hwlock_acquire_rc:
hwlock_release_seen:
unexpected_diff:
gpu_fence:
gop:
panic_or_reset_evidence:
classification:
rollback_image:
next_single_measurement:
```

## 21. 推荐的实际执行顺序

当前立即执行顺序固定为：

```text
S0 三次平台基线
 -> S1 H7 hwspinlock host model
 -> S2 H8 mapping helper + G1.3
 -> S3 G2.1 descriptor
 -> S4 G2.2–G2.5 map/read/unmap
 -> S5 G2.6 cache parity
 -> S6 G3 hwspinlock device
 -> S7 G4 real-snapshot shadow
 -> G5 写入评审
 -> S8/G5 single allocation
 -> S9/G6 SMP2P
 -> S10 firmware/binding closure
 -> S11 A0
 -> S12 A1
 -> S13 A2
 -> S14 amplifier muted-ready
 -> S15 A3
 -> S16 A4
 -> S17 A5
 -> S18 A6
```

下一项具体任务是 **S0 的 3/3 基线冻结，然后实现 S1/H7**。在 H7/H8 和 G2–G4 完成前，不增加真实 SMEM 写入能力；在 A2 完成前，不解除任何功放 mute。
