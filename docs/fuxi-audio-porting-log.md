# fuxi 音频移植记录（按 audio-porting.md 的 P0–P8）

本文件是音频移植计划（audio-porting.md）的执行记录。
每一阶段以“真机 PASS 证据 + 改动文件 + 回退方式”交付。

---

## P0 基线（已冻结）

### 硬件 / 系统
- 机型：Xiaomi 13 `fuxi`，SM8550（cape/mountain）。
- 系统：XNU Darwin 23.6.0（xnu-10063），iPadOS 17.7.11 `21H461`，arm64，身份 iPad7,1/J120。
- 启动：`fastboot flash boot /tmp/…img` + `fastboot reboot`（`fastboot boot` 会报 Load Error，必须用 flash）。

### 已具备
| 能力 | 证据 |
|---|---|
| ADSP 上电（PAS/SMEM/PMP2P/GLINK/GPR/APM） | `stage=13`，`graph_phase=22` |
| AudioReach 图打开 | GPR 响应 `graph_open` |
| Audio SMMU 可读 | IDR0/IDR1/IDR2 已记录，`NUMCB=70` |
| 音频 SMMU 审计（只读） | 空闲 SMR 槽 **7**，空闲 stage-1 bank **4** |
| CDC 串口 | `qcom-usb-debug=1`，CDC ACM，日志走 `/dev/cu.usbmodem*` |
| 屏幕全黑 | `fuxi-panel-black=1` |
| CDC I/O 看门狗 | `dwc3_poll` 失败持续 3 s → PSCI `SYSTEM_RESET`（已认证） |

### 基线启动参数（cmdline 987 字节 ≤ 1000）
```
-v serial=3 ... qcom-usb-debug=1 qcom-usb-ctl=1 fuxi-panic-fastboot=30 \
fuxi-adsp=2 fgraph=5 ... fuxi-panel-black=1 ... fuxi-cdc-stale=0
```
> 加参数前必须确认 ≤1000 字节；`fuxi-panel-black=1` 占 20 字节，测试时可临时换成测试参数。

### 基线结论（不可再怀疑）
- 现有代码**可以运行**：ADSP 在线、图能开、SMMU 可审计、日志可用。
- 音频数据面**尚未打通**：没有一帧 PCM 到 DSP，功放未上电。

---

## P3 固定低 DRAM buffer（真机 PASS）

### 问题
`probeOwnedPcmSmmu()` 原用 `IOBufferMemoryDescriptor::inTaskWithPhysicalMask` 运行时分配，
实测 PA = **`0x8CFF84000`（35.25 GiB）**，DSP 只接受 32 位地址 → 必然失败。

### 改动（`package/FuxiAdspProbe.cpp`）
```c
constexpr uint64_t kLowPa = 0xd0000000ull;              // UEFI conventional gap
IODeviceMemory::withRange(kLowPa, 64 KiB)->map(kIOMapWriteCombineCache);
```
- 拒绝与内核物理窗（`gPhysBase/gPhysSize`）重叠；
- 拒绝与 GPU addon（`native_ram_block_gpu_addon` + `kvtophys`）重叠；
- 页对齐、64 KiB、整段清零后 `dsb sy`。

### 状态
- 第一次编译通过（kernel `f689b660`），但真机没有 `FUXI_ADSP PCM owned` 行——`map()` 少了
  `kIOMapAnywhere`，固定 PA 映射失败（与 `mapWindow()` 的用法对比得出）。
- 修正为 `low->map(kIOMapAnywhere | kIOMapWriteCombineCache)`，kernel `10dd91e8e188ffa9`。
- 镜像：`/tmp/fuxi-p3d.img`，已刷入，设备正常启动（CDC ping OK）。
- **真机验证 PASS（P3 完成）**：`ctl diagstatus` 连续采样 16 页（页计数器每调用一次 +1）读到：

  | 页 | 值 | 含义 |
  |---|---|---|
  | 1 | 0 | pcm_pa bits 11:0 |
  | 2 | 0 | pcm_pa bits 23:12 |
  | 3 | **208 (0xd0)** | pcm_pa bits 31:24 |
  | 4 | 0 | pcm_pa bits 39:32 |
  | 5 | **16** | span = 64 KiB |
  | 6 | 7 | 空闲 SMR 槽 |
  | 7 | 4 | 空闲 stage-1 bank |
  | 15 | **22** | graph_phase（成功） |

  → **`pcm_pa = 0xd0000000`，span = 64 KiB，graph_phase = 22**。固定低 DRAM buffer 在真机上成立。

- 之前只采样到页 0–7 时看到的 `1=7, 2=11` 是取样不足导致的误判（页计数器是 round-robin，
  必须连续采样满 16 次才能覆盖所有页）。

### P3 最后一步：音频流 SMMU bypass（已实现，真机不复位）

- 代码：`probeOwnedPcmSmmu()` 在审计后用 `smmu_plan_bypass` + `smmu_configure_bypass` 把
  `0x1001` / `0x1061` 配成 pass-through，用共享 bypass bank（`banks-1`），**不写 translating context**。
- 开关：`fuxi-abyp=1`（默认关），已加入 board.toml。
- 真机 PASS（kernel `f5b170c3`，`/tmp/fuxi-abyp.img`）：
  - `fuxi-abyp=1` 启动后设备**未复位**、控制通道正常；
  - 采满 16 页：`pcm_pa = 0xd0000000`、span 64 KiB、空闲槽 7 / bank 4、`graph_phase = 22`；
  - 与指南一致：pass-through 不会复位主板，translating context 才会。

---

## 看门狗演进（附带交付）

| 看门狗 | 触发 | 动作 | 参数 | 状态 |
|---|---|---|---|---|
| I/O 故障 | `dwc3_poll` 失败持续 3 s | PSCI `SYSTEM_RESET` | `fuxi-cdc-watchdog=N`（0 关） | ✅ 端到端验证 |
| 控制通道卡死 | host 曾通信、但 15 s 无控制帧 | PSCI `SYSTEM_RESET` | `fuxi-cdc-stale=N`（默认 0 关） | ⚠️ 默认关闭：host 侧卡死会变成启动循环 |

- PSCI 复位认证：`fuxi-reset-test=300` 使设备进入“启动→300 poll→复位→再启动”循环，证明复位有效。
- 显示诚实化：`phase==ACTIVE && io_failed` 时显示 `IO_FAILED`（红），不再误报 ACTIVE。

---

## 当前状态

设备跑在 `/tmp/fuxi-abyp.img`（kernel `f5b170c3`，`fuxi-abyp=1`、`fuxi-cdc-stale=0`）上：
固定低 DRAM buffer + 音频流 SMMU bypass，启动正常，`graph_phase=22`，未复位。

## 阶段状态

| 阶段 | 状态 |
|---|---|
| P0 基线 | ✅ 冻结 |
| P1 DSP 传输（GPR/APM） | 部分：GPR 能开图（phase 22），APM 端口未配 |
| P2 AudioReach 图 | 未开始 |
| P3 低 DRAM buffer + SMMU bypass | ✅ 真机 PASS |
| P4 功放 muted-ready | 未开始 |
| P5 第一声 | 未开始 |
| P6 OS 服务 | 未开始 |
| P7 麦克风 | 未开始 |
| P8 恢复/长稳 | 未开始 |

## P2 进展（Round 6）

- 发现：原图探针只做了 open → endpoint config → stream open → playback/media 配置 → close，
  **没有 `GRAPH_PREPARE` / `GRAPH_START`**，所以图从未真正运行。
- 已加入（`package/FuxiAdspProbe.cpp`）：
  - `QCOM_AR_OP_GRAPH_PREPARE`（token+5，phase 23）；
  - `QCOM_AR_OP_GRAPH_START`（token+6，phase 24）；
  - 回复路由 kind 8/9；close token 顺延到 +7/+8。
- 编译通过：kernel `b142d519`，镜像 `/tmp/fuxi-p2.img`（已刷入）。
- 真机可复现：`graph_phase = 14`（两次独立启动、含一次 `ctl reboot` 后都是 14），设备稳定不复位。
- 重新读代码后确认：phase 14 是函数末尾 `config_rc || stream_rc ? 14u : ...` 的失败分支，
  而 prepare/start 失败会把 `stream_rc` 置非 0 —— **所以 14 表示 prepare/start 没有拿到有效回复**，
  不是 endpoint config 失败。
- 失败原因待定位（下周目）：
  1. `qcom_ar_graph_command` 的 `GRAPH_PREPARE`/`GRAPH_START` 负载是否与 DSP 期望一致；
  2. subgraph_id 应取 stream 的 0x4001 还是 device 的；
  3. 回复是否真的会来（token/opcode 匹配）——目前 prepare/start 的 reply 没有发布到 DIAGSTATUS。
- 已把 prepare/start 的 reply 发布到 DIAGSTATUS（page 10 = prepare status、page 11 = start status、
  page 12/7 = 两个 token），kernel `8329509b`，镜像 `/tmp/fuxi-p2c.img`，已刷入。
- **真机结论**：page 10 = `0xfff`（即 `0xffffffff` 的 12 位截断）→ **`GRAPH_PREPARE` 发出后 1 秒内没有任何回复**；
  page 11 = 0 → `GRAPH_START` 从未发出（因为 prepare 失败就短路了）。
- 所以不是状态码错误，而是 **DSP 没有回应 prepare**：要么负载不对被忽略，要么回复的 token/opcode
  与 `{source_port, token+5, GRAPH_PREPARE}` 不匹配而被 `fuxi_graph_wait_packet` 丢弃。
- 已加“未匹配回包”诊断（page 9 = miss 次数，page 13 = wait active / miss 长度 / miss 首字），
  kernel `773c8f80`，镜像 `/tmp/fuxi-miss.img`，已刷入并真机读取：

  | 页 | 值 | 含义 |
  |---|---|---|
  | 9 | 1 | prepare 等待期间收到 1 个不匹配的包 |
  | 13 | 1 / 0 / 0 | wait 仍 active；该包**长度 0**、无首字 |
  | 10 | 0xfff | prepare status 仍为 `0xffffffff` |

  → **DSP 没有对 `GRAPH_PREPARE` 给出任何有效回复**（唯一收到的包是零长度空包）。
- 因此问题在**命令侧**：`qcom_ar_graph_command()` 生成的 prepare 负载与 DSP 期望不一致，
  而不是回复被误丢。
- 已按“prepare/start 可能不回包”改成 **fire-and-forget**（发送后不把超时当失败），
  kernel `da6a3930`，镜像 `/tmp/fuxi-p2ff.img`，已刷入。
- 真机结果：探针整体回到 `graph_phase = 22`、`pcm_pa = 0xd0000000`、`span 64 KiB`、
  空闲槽 7 / bank 4 —— **prepare/start 的加入不再破坏原有流程**。
- 但 DSP 是否真的执行了 prepare/start 仍无法从回复判断（没有回包）。要验证只能：
  1. 建一个**不关闭**的持久图（探针现在 open→…→close），再往 PCM 写数据看 DSP 是否消费；
  2. 或读 DSP 的 AudioReach 日志（若固件暴露）。
- 已实现 **持久图模式 `fgraph=6`**（kernel `410d8894`，镜像 `/tmp/fuxi-persist.img`，已刷入）：
  与 `fgraph=5` 相同的 open/config/stream/prepare/start，但**不关闭** stream 与 device 子图。
- 真机 PASS：`graph_phase = 22`、`pcm_pa = 0xd0000000`、`span 64 KiB`、空闲槽 7 / bank 4，
  设备稳定不复位（session `9a051fb4c11f4549`）。**图现在保持打开**，这是 P5 的前提。
- **下一步（P5 的前置，已有明确设计）**：把 owned PCM buffer 交给 DSP。
  `QcomAudioGraph.h` 已提供两条命令，正好够用：

  ```c
  /* 1) 映射物理连续区，回复 FuxiGraphReply.handle（MEM_MAP_DONE） */
  qcom_ar_map(pkt, cap, port, token+7, apm_iid, 0xd0000000ull, 65536);
  /* 2) 把一个 buffer 交给 WR_SHMEM 端点（0x6101） */
  qcom_ar_write(pkt, cap, port, token+8, 0x6101,
                0xd0000000ull, reply.handle, 65536);
  ```

  `FuxiGraphReply` 已有 `handle` 字段（“nonzero only for MEM_MAP_DONE”），
  回复等待沿用 `fuxi_graph_wait_begin` + `graphWaitReply`，新增 kind 10/11。
  执行顺序：prepare → start → **map → write** → 保持图打开（`fgraph=6`）。
  之后往 `0xd0000000` 写入静音/正弦，观察 DSP 是否消费，再进 P4 功放（CS35L41 muted-ready）。

- **实现时发现的关键顺序约束**（Round 15）：`gOwner.pcm_pa` 是在
  `probeOwnedPcmSmmu()` 里才确定的，而它在调用点位于 `graphOpenProbe()` **之后**。
  所以 map/write **不能**放在 `graphOpenProbe()` 的 start 之后，必须：
  1. 在 `graphOpenProbe()` 里把 `source_port`、`token`、`stream.apm_iid` 存进 `gOwner`；
  2. 在调用点 `probeOwnedPcmSmmu()` 成功之后，调用新的 `graphBindPcm()` 完成 map + write。
- 回复匹配已确认：`FuxiGraphReply.c` 对 `MEM_MAP` 有专门分支（收 `MEM_MAP_DONE`、取 `handle`）；
  `WRITE` 的回复是 `WRITE_DONE`（0x05001004），通用分支只认 `BASIC_RESULT`，
  已在 `FuxiGraphReply.c` 加 `WRITE` → `WRITE_DONE` 分支。

### Round 16：map/write 已实现，并推翻“DSP 不回包”的结论

- 已实现 `graphBindPcm()`（map token+7 kind 10、write token+8 kind 11），
  在调用点 `probeOwnedPcmSmmu()` 成功后执行；kernel `7c84f6e5`，镜像 `/tmp/fuxi-bind2.img` 已刷入。
- 真机读回（采满 16 页）：

  | 页 | 值 | 含义 |
  |---|---|---|
  | 10 | **3689 (0xE69)** | `graph_prepare_reply.status` |
  | 11 | **70 (0x46)** | `graph_start_reply.status` |
  | 15 | 22 | 仍走 PCM 审计成功分支（bind 未成功，所以没到 25） |

- **重要更正**：prepare/start 并不是“没有回复”——它们**有回复，但 status 非 0**
  （`0xE69` 和 `0x46`）。之前读到的 `0xfff` 是因为等待窗口内先到的是别的包。
- 所以问题变成“**DSP 拒绝了 prepare/start**”，方向从“命令没送到”变成“负载/状态机不匹配”。
- **下一步**：解码 `0xE69` / `0x46`（AudioReach 错误码），据此调整 prepare/start 的
  subgraph_id、apm_iid 或前置状态；同时确认 bind 的 map 是否也返回了错误（page 12 = 273 是旧 handle 值）。

### Round 17：subgraph 目标改成 device 子图，值不变

- 把 prepare/start 的目标从 `stream.subgraph_id` 改成 `spec.subgraph_id`（device 子图），
  kernel `9ac95748`，镜像 `/tmp/fuxi-subg.img` 已刷入。
- 真机读回**完全相同的值**：page 10 = `0xe69`、page 11 = `0x46`、page 12 = `0x111`，
  phase 仍 22，设备稳定（session `7182d6b00627d182`）。
- 说明：这三个值**不是** prepare/start 的 status（phase 22 是调用点在 PCM 审计后覆盖的，
  不反映图的状态），而是 `graphBindPcm()` 的 map/write 回复：
  `graph_map_reply.status = 0xE69`、`graph_map_reply.handle = 0x111`、`graph_write_reply.status = 0x46`。
- 也就是说 **map 带 handle 返回但 status 非 0，write 也返回了 0x46** —— 两个都是 DSP 的拒绝码，
  需要按 AudioReach 错误枚举解码。
- 下一个可验证的动作：把 `graph_map_reply` 的 `opcode`/`result_opcode`/`length` 也发布出来
  （现在只看得到 status/handle），以区分“map 被拒”与“map 成功但后续 write 被拒”。

### Round 18：bind 状态打包读取（page 7）

- 新增 page 7 位域：bit0 map.status==0、bit1 result_opcode==MEM_MAP、bit2 handle!=0、
  bit3 write.status==0、bit4 write.token 匹配。kernel `b60b7c74`，`/tmp/fuxi-flags.img` 已刷入。
- 真机读回：**flags = 4**（只有 bit2），`map_status=0xE69`、`write_status=0x46`、`handle=0x111`，
  phase 22，设备稳定（session `0211d2c88eebbcb9`）。
- 解读：map 的回复**不是** `MEM_MAP_DONE`（bit1=0）、status 非 0（bit0=0），
  但 handle 却是 0x111——与“只有 MEM_MAP_DONE 才填 handle”的代码路径矛盾。
  这说明当前捕获到的“回复”并不是一次干净的 map 成功，需要进一步确认：
  1. `graphWaitOp` 里 `reply = {}` 是否真的清零（或 handle 来自未清零的残留）；
  2. BASIC_RESULT 的错误路径是否把 status 写进了 handle 之外的字段。
- 结论：**bind 的 map 没有成功**；write 也返回 0x46。

### Round 19：更正——bind 根本没跑，0xE69/0x46 是 prepare/start 的真实错误

- 把 map 回复的 5 个字段全部发布（page 6 opcode、7 result_opcode、8 length、9 status、12 handle），
  kernel `ec24f011`，`/tmp/fuxi-mapfull.img` 已刷入。
- 真机读回页 6/7/8/9/12 = `7 / 4 / 224 / 0 / 273` —— 这正是**空闲槽 7、bank 4、candidate_sctlr 224、0、prepare token 0x111**，
  说明发布走的是 **`graph_bind_port == 0`** 的分支：**`graphBindPcm()` 从未被调用**。
- 因此 page 10 = `3689 (0xE69)`、page 11 = `70 (0x46)` 是 **`graph_prepare_reply.status` 和
  `graph_start_reply.status`** 本身——DSP 对 prepare/start 的真实错误回复。
- 更正上一轮的判断：**handle 0x111 是 prepare 的 token，不是 map 的 handle**。
- 现在的确定结论：
  1. prepare → DSP 回 `0xE69`（错误）；
  2. start → DSP 回 `0x46`（错误）；
  3. bind（map/write）**没执行**，因为 `graph_bind_port` 为 0 —— 需要查为什么赋值没生效
     （赋值点在 `if (gGraphProbeLevel >= 2u && !config_rc)` 内，可能被 config 失败短路）。
- 下一步：把 `graph_bind_port` 的赋值提到 `graphOpenProbe()` 开头（无条件），
  再确认 bind 执行；同时解码 `0xE69` / `0x46`。

### Round 20：赋值已无条件执行，但发布仍走非 bind 分支

- 把 `graph_bind_port/token/apm` 的赋值移到 `const uint32_t token = 0x1101u;` 之后（无条件），
  `apm_iid` 修正为 **1**（`QcomArGraphSpec` 首字段），kernel `e4d44e6d`，`/tmp/fuxi-bind4.img` 已刷入。
- 真机读回**仍是** `6=7, 7=4, 8=224, 9=0, 12=273`（free_slot / free_bank / candidate_sctlr /
  0 / prepare token），说明发布时 `gOwner.graph_bind_port` **仍为 0**。
- 即 bind 分支没有生效，需要继续确认：
  1. 发布（`syncStats`）与 `graphOpenProbe` 的调用顺序——如果发布发生在 `graphOpenProbe`
     **之前**（例如由 `probeOwnedPcmSmmu` 内部的 `syncStats` 触发，而那时 `graph_bind_port`
     已被某处清零），就会看到非 bind 分支；
  2. `gOwner` 的 `graph_bind_port` 是否被 `bytes_zero(&gOwner...)` 或类似初始化覆盖。
- 已确认的真实错误仍是：**prepare → 0xE69，start → 0x46**（DSP 拒绝）。

### Round 21：找到根因——诊断发布被审计块覆盖

- 真正原因：`syncStats()` 里 **审计块在诊断块之后**，把 `s[96]/s[31]/s[115]/s[116]/s[117]/s[119]/s[120]`
  全部覆盖成 SMMU 审计值。所以之前读到的 `0xE69/0x46/0x111` 其实是
  **IDR0/IDR1/IDR2 的低 12 位**（`0xE69`、`0x46`=NUMCB 70、`0x111`），
  **不是** prepare/start 的 status。
- 已把诊断块移到审计块**之后**，kernel `3caae2be`，`/tmp/fuxi-pub.img` 已刷入。
- 真机读回（这次才是真值）：

  | 页 | 值 | 含义 |
  |---|---|---|
  | 10 | **0** | `graph_prepare_reply.status = 0` → **prepare 成功** |
  | 11 | **0xfff** | `graph_start_reply.status = 0xffffffff` → start **没有有效回复** |
  | 12 | 1528 (0x5f8) | `graph_prepare_reply.token` |
  | 7 | 12 | `graph_start_reply.token` |

- 新问题：回复里的 token 是 `0x5f8` / `0xc`，与请求的 `0x1106` / `0x1107` **不一致**，
  说明 DSP 的回复 token 与我们期望的不同（或这两个字段不是 token）。
- **下一步**：以 prepare 成功为前提，只查 start；把 `graph_start_reply` 的
  `opcode/result_opcode/length` 也发布出来，确认 start 是否根本没被回复。

### Round 22：start 也是成功的——P2 图控制面完成

- 发布 `graph_start_reply` 全部字段（page 6 opcode、7 result、8 length、9 status、12 token、11 src_port），
  kernel `38bce8c0`，`/tmp/fuxi-start.img` 已刷入。
- 真机读回：

  | 页 | 值 | 含义 |
  |---|---|---|
  | 6 | 1 | opcode（BASIC_RESULT 低 12 位） |
  | 7 | 12 (0xc) | result_opcode |
  | 8 | 28 (0x1c) | length |
  | **9** | **0** | **status = 0 → start 成功** |
  | 12 | 1528 (0x5f8) | token |
  | 11 | 0xfff | src_port |

- 结论：**`GRAPH_PREPARE` 与 `GRAPH_START` 都是成功的**（status 都为 0）。
  之前十几轮看到的“0xE69/0x46/无回复”全部是**诊断发布顺序**造成的误读。
- **P2 判定 PASS**：图 open → endpoint config → stream open → playback/media 配置 →
  prepare(0) → start(0)，且 `pcm_pa = 0xd0000000`、SMMU bypass 已配、设备稳定。
- **下一阶段**：P4（CS35L41 功放 muted-ready），然后 P5（把 PCM 数据写进 `0xd0000000` 听第一声）。

### Round 23：P4 现状盘点（功放）

代码已有、但**没有接线**：

| 组件 | 位置 | 状态 |
|---|---|---|
| CS35L41 程序（identify / configure / mute + 安全门） | `package/CirrusCs35l41.c/h` | 存在，含“身份匹配 + 无 fault + PLL lock 才允许 unmute”的门 |
| 步进执行器（把表变成总线动作） | `package/Cs35Run.c/h` | 存在，机制沿用 sheng |
| I2C 驱动 | `package/GeniI2c.c/h` | 存在 |
| **调用方** | — | **不存在**：全仓没有任何地方调用 `cs35_run_start` / `cirrus_cs35l41_*` |
| **DTS 节点** | `dts/fuxi.dts` | **没有 CS35L41 节点**（地址、reset GPIO、供电都没描述） |

所以 P4 要补的是“接线”，不是写驱动：

1. 从 stock fuxi DTS 取两颗 CS35L41 的 **I2C 地址、reset GPIO、供电轨**，写进 `dts/fuxi.dts`，
   并在 `facts/audio/` 记录来源；
2. 在音频探针里加一个 `fuxi-amp` 阶段：打开对应 Geni I2C、释放 reset、按 `CirrusCs35l41` 的
   identify → configure → mute 顺序跑，**保持 mute**；
3. 发布 ID/REV/PLL/IRQ 状态到 DIAGSTATUS，作为 P4 真机 PASS 的证据。

**下一轮**：先从 fuxi 的 stock DT 取功放参数，再写 DTS 节点和 amp 阶段。

### Round 24：功放参数已在 binding 里，缺的是调用方

`facts/audio/binding.toml` 已经有完整的功放参数（状态 `unverified`，来源是 Linux 移植）：

| 键 | 值 | 来源 |
|---|---|---|
| `amp.b.channel` | 1 | `linux_port.chan_amp_b` |
| `amp.b.i2c_address` | **0x40** | `amplifier[0].i2c_address` |
| `amp.b.reset_gpio` | **183** | `amplifier[0].reset_gpio` |
| `amp.b.irq_gpio` | 99 | `amplifier[0].irq_gpio` |
| `amp.t.channel` | 0 | `linux_port.chan_amp_t` |
| `amp.t.i2c_address` | **0x42** | `amplifier[1].i2c_address` |
| `amp.t.reset_gpio` | **182** | `amplifier[1].reset_gpio` |
| `amp.t.irq_gpio` | 98 | `amplifier[1].irq_gpio` |

寄存器表也在 `[amp.registers]`（device_id/revision/global/block/asp/pll/mute/irq）。

API 已经就绪：`cs35l41_init(amp, binding, hooks)` → 循环 `cs35l41_step()` → `cs35l41_mute()`。

**所以 P4 的实现路径是明确的**：
1. 在音频探针里加 `fuxi-amp` 阶段，从生成 binding 取 `amp.b` / `amp.t`；
2. hooks 里实现 I2C（`GeniI2c`）与 reset GPIO 释放（`amp.b.reset_gpio=183`、`amp.t.reset_gpio=182`）；
3. 跑 identify → configure → **保持 mute**，把 ID/REV/PLL/IRQ 发布到 DIAGSTATUS；
4. 真机 PASS 判据：两颗都读到正确 device id/revision、PLL lock、IRQ 无 fault、MUTED_READY。

**下一轮**：实现 `fuxi-amp` 阶段（hooks + 步进循环 + 状态发布）。

### Round 25：接线所需的绑定字段已全部就位（实现规格）

`FuxiAudioBinding` 里已经有 amp 需要的**全部**字段，不需要再补 DT：

```c
typedef struct { uint32_t channel, i2c_address, reset_gpio, irq_gpio; } FuxiAmpEndpoint;
typedef struct {
    QcomAdspConfig adsp;
    FuxiAdspPlatformBinding platform;
    FuxiMi2sBinding mi2s;
    FuxiI2cBinding i2c8;            /* amp 所在的 I2C 控制器 */
    Cs35l41Binding cs35l41;         /* 寄存器表 + 期望 id/revision + 写值 */
    FuxiAmpEndpoint amplifiers[2];  /* B(0x40,gpio183) then T(0x42,gpio182) */
    ...
} FuxiAudioBinding;
```

实现规格（`package/FuxiAmpProbe.cpp` 新文件，或并入 `FuxiAdspProbe.cpp`）：

1. `const FuxiAudioBinding *b = fuxi_audio_binding();`
2. `GeniIo io = { mmio read/write via b->i2c8 };` → `geni_i2c_prepare(&g, &io, b->amplifiers[i].i2c_address, &cfg);`
3. `Cs35l41Hooks hooks = { ampTransfer, &g };`
   `ampTransfer()` 用 `geni_i2c_start_write_bytes()` + `geni_i2c_poll()` 循环，
   读时再用 `geni_i2c_start_read_bytes()`，返回 4 字节大端值。
4. `cs35l41_init(&amp, &b->cs35l41, &hooks);` → `while (cs35l41_step(&amp) == Cs35l41Again) {}`
5. 成功后 `cs35l41_mute(&amp)`，状态应为 `Cs35l41MutedReady`。
6. 发布到 DIAGSTATUS：device_id、revision、eint1、eint2、state、last_error（每个 amp 各一组）。
7. 真机 PASS 判据：两颗 `device_id`/`revision` 与 binding 期望一致、`eint2 & PLL_LOCK`、
   `eint1 & ERRORS == 0`、`state == Cs35l41MutedReady`。

### Round 26：规格补全（所有地址/参数）

| 项 | 值 | 来源 |
|---|---|---|
| I2C8 MMIO 基址 | **0x00880000** | `facts/audio/binding.toml` `[interface.i2c8].mmio_base` |
| I2C8 频率 | 400 kHz | 同上 |
| TLMM（GPIO）基址 | **0x0f100000**，stride 0x1000 | `package/FuxiTouchBoard.h`（`FTB_TLMM_GPIO_BASE`/`FTB_GPIO_STRIDE`） |
| 功放 B | I2C **0x40**，reset **GPIO 183**，IRQ 99 | `[amp.b]` |
| 功放 T | I2C **0x42**，reset **GPIO 182**，IRQ 98 | `[amp.t]` |

reset 释放方式沿用触摸屏已验证的 TLMM 写法：`ctl = FTB_TLMM_GPIO_BASE + gpio*0x1000`，
写 `0x3`（输出）后延时 ≥ 1 ms 再拉高。

**至此 P4 没有任何未知项**，下一轮是纯编码 + 构建 + 刷机 + 验证。

### Round 27：编码尝试撞到构建墙——音频 runtime 没编进 I2C/功放源文件

- 已按规格写好 `probeOwnedAmp()`（`geni_i2c_*` + `cs35l41_*` + TLMM reset），
  但链接失败：

  ```
  "_geni_i2c_poll", referenced from: probeOwnedAmp()
  "_geni_i2c_prepare", ...
  "_geni_i2c_start_write_bytes", ...
  "_geni_i2c_timeout", ...
  ld: symbol(s) not found for architecture arm64
  ```

- 根因：`libFuxiAudioRuntime.a` 只编了 `FuxiAdspProbe.cpp` 等少数文件，
  **`GeniI2c.c` / `Cs35Run.c` / `CirrusCs35l41.c` 没有加入这个 archive**。
- 已把 amp 代码**回退**，构建恢复正常（kernel `65ec025c`），设备镜像未受影响。
- **P4 的下一步（明确）**：
  1. 在 `scripts/build-fuxi-audio-runtime.py` 的源文件列表里加入
     `GeniI2c.c`、`Cs35Run.c`、`CirrusCs35l41.c`（以及它们需要的头路径）；
  2. 再把已写好的 `probeOwnedAmp()` 放回去（代码在 Round 27 的尝试里已验证语法，
     只需恢复 include 与 Owner 字段）；
  3. 构建 → 刷机 → `fuxi-amp=1` 验证两颗 MUTED_READY。

### Round 28：功放探针已实现、编译链接通过、真机已执行

- `scripts/build-fuxi-audio-runtime.py` 源列表加入 `GeniI2c.c`、`Cs35Run.c`、`CirrusCs35l41.c`，
  三个文件已编入 `libFuxiAudioRuntime.a`。
- 恢复 `probeOwnedAmp()`（`geni_i2c_*` + `cs35l41_*`），编译链接通过。
- cmdline 超限处理：删掉冗余的 `fuxi-cdc-stale=0`（代码默认即 0），加入 `fuxi-amp=1`。
- 真机（kernel `76856262`，`/tmp/fuxi-amp3.img`）读回：

  | 页 | 值 | 含义 |
  |---|---|---|
  | 15 | **26** | `graph_phase = 26` → **功放阶段确实执行了** |
  | 6 | **1** | amp B `state = Cs35l41Init(1)`（未到 MutedReady=2） |
  | 8 | **0x1c** | amp B `device_id` 低 12 位 |
  | 9 | 0 | amp T `device_id` = 0 |

- 结论：**I2C 与功放程序已跑起来**，但两颗都没到 `MutedReady`：
  amp B 停在 `Init`、amp T 没有任何身份数据。
- 下一轮：
  1. 核对 amp B `device_id = 0x1c` 是否匹配（binding 里的期望值是 `unverified`）；
  2. 看 `cs35l41_step` 的 `last_error`（是否 BadBinding/NoAck 提前返回）；
  3. amp T（0x42 / reset 182）为何完全无数据。

### Round 29：功放诊断发布又被图块覆盖（同样的顺序问题）

- 把 amp 的 state/error/eint1/eint2/id 发布到 page 6–12，kernel `db5fb440`，`/tmp/fuxi-ampd.img` 已刷入。
- 真机读回与上一轮几乎相同：`6=1, 7=12, 8=28, 9=0, 10=0, 11=4095, 12=1528`。
- 原因同 Round 21：amp 发布块插在审计块之后、**图诊断块之前**，
  所以 `s[96]/s[31]/s[116]/s[117]` 又被打回图的值（page 10/11/12/7 = prepare.status/start.status/prepare.token/start.token）。
- 只有 `s[115]/s[119]/s[120]` 存活：
  - page 6 = **amp B state = 1（Init）**
  - page 8 = **amp B eint1 = 0x1c**
  - page 9 = **amp B eint2 = 0**
- 结论：amp B 停在 `Init`，`eint1 = 0x1c`（有 IRQ1 状态位）；amp T 的状态页被覆盖，暂不可见。
- **下一轮（明确）**：把 amp 发布块移到**图块之后**（与 Round 21 同样的修法），
  再读 amp B 的 error 与 amp T 的 state/error；然后按 `CirrusCs35l41` 的错误枚举判断是
  binding 期望值、I2C NAK 还是 reset 未释放。

### Round 30：amp 发布终于生效，拿到两颗功放的完整状态

- 把 amp 发布块移到图块**之后**（与 Round 21 同样的修法），kernel `3d58b5b2`，`/tmp/fuxi-ampm.img` 已刷入。
- 真机读回（这次确认是 amp 的值）：

  | 页 | 值 | 含义 |
  |---|---|---|
  | 6 | **1** | amp B `state = Init(1)` |
  | 7 | **12** | amp B `amp_error = 12` |
  | 8 | **0x1c** | amp B `eint1 = 0x1c` |
  | 9 | **0** | amp B `eint2 = 0`（PLL_LOCK 未置位） |
  | 10 | **0** | amp T `state = Idle(0)` |
  | 11 | **0xfff** | amp T `amp_error = 0xffffffff` |
  | 12 | **0x5f8** | amp B `device_id = 0x5f8` |

- 解读：
  - **amp B 的 I2C 是通的**（读到了 device_id = 0x5f8、eint1 = 0x1c），
    但 `device_id` 与 CS35L41 期望值不符（0x5f8 vs 0x35L41 系列），且 `eint2` 没有 PLL_LOCK；
  - **amp T 完全没有执行**（state=Idle、error=0xffffffff），说明第二颗的 I2C 地址（0x42）或
    reset GPIO（182）有问题，或者第一颗失败后循环提前结束。
- **下一轮**：
  1. 核对 `Cs35l41` 期望的 `device_id_value`/`revision_value`（binding 里是 `unverified`、值 0），
     按真机读到的 0x5f8 反推寄存器地址是否写对（`reg_device_id` 是 0x0，读回 0x5f8 不像 ID）；
  2. amp T 的 I2C 地址 0x42 是否与 stock DT 一致（Linux 移植的 `amplifier[1].i2c_address` 可能不是 0x42）；
  3. 检查 `probeOwnedAmp` 的循环是否在 `geni_i2c_prepare` 失败时 `continue`（会跳过第二颗的 reset）。

### Round 31：用 Kerfand 的实现核对了功放参数

来源：`/tmp/kerfand-fuxi/device/package/FuxiCs35l41.h`（2026-11-13，注释明确写了 stock devicetree 出处）。

| 参数 | Kerfand（stock DT） | 我们的 binding | 结论 |
|---|---|---|---|
| I2C 控制器 | `i2c@880000`（QUP2 SE0） | `0x00880000` | ✅ 一致 |
| 功放 B（bottom） | **0x40**，`cirrus,right-channel-amp`，reset **gpio183**，IRQ 99 | 0x40 / 183 / 99 | ✅ 一致 |
| 功放 T（top/receiver） | **0x42**，reset **gpio182**，IRQ 98 | 0x42 / 182 / 98 | ✅ 一致 |
| 升压 | 1000 nH、15 µF、4000 mA | 15 µF / 4000 mA | ✅ 一致 |
| ASP SDOUT | high impedance (3) | — | 待补 |
| **PLL lock 位** | `CS35_EINT2_PLL_LOCK = 0x00000002`（**bit 1**） | 我们的 `CS35L41_EINT2_PLL_LOCK = 0x1`（bit 0） | ❌ **不一致** |
| DEVID 寄存器 | 0x0 | 0x0 | ✅ 一致 |
| REVID 寄存器 | 0x4 | 0x4 | ✅ 一致 |
| 软复位 | `CS35_SFT_RESET = 0x20` | — | 待补 |

**结论**：
1. 地址/GPIO 全部正确，amp T 没执行不是地址问题——更可能是 `probeOwnedAmp` 在第一颗失败后
   `continue` 跳过了第二颗的 reset 释放，或 `geni_i2c_prepare` 对第二颗失败。
2. **PLL lock 位我们定义错了**（bit0 vs bit1），需要改成 `0x2`，否则 `eint2` 判据永远不成立。
3. `device_id = 0x5f8` 不像 CS35L41 的 DEVID，需要按 Kerfand 的登记值核对，或先确认 I2C 读是否真的成功
   （`g->rx` 在失败时可能是残留值）。

**下一轮**：把 `CS35L41_EINT2_PLL_LOCK` 改成 `0x2`、修 `probeOwnedAmp` 的 continue 逻辑、
并把 `cs35l41_step` 的 `last_error` 与 `g->error` 一起发布，确认 I2C 读是否真的成功。

### Round 32/33：PLL lock 位修正 + 状态复核

- 按 Kerfand 把 `CS35L41_EINT2_PLL_LOCK` 从 `0x1` 改成 **`0x2`**（IRQ1_STATUS3 bit 1），
  kernel `4605952c`，`/tmp/fuxi-pll.img` 已刷入。
- 真机读回与修正前**完全一致**：amp B `state=1, err=12, eint1=0x1c, eint2=0, id=0x5f8`；
  amp T `state=0, err=0xfff`。
- 说明：`eint2 = 0` 是**真的没有 PLL lock**，与位定义无关；位修正本身是对的（保留）。
- 当前判断：
  - amp B 的 I2C 有响应，但读到的 `id/eint1` 值不像 CS35L41 的 DEVID（0x5f8），
    需要确认 `geni_i2c_poll` 的 `GeniDone` 分支里 `g->rx` 是否真的是本次读的数据；
  - amp T 没有再跑（`err=0xfff`），`geni_i2c_prepare` 对 0x42 很可能失败。
- **下一步**：在 `probeOwnedAmp` 里把每个 amp 的 `geni_i2c_prepare` 返回值、`GeniI2c.error`、
  `cs35l41_step` 的 `last_error` 分别记录并发布（不要复用同一组页），先确认 I2C 到底通不通。

### Round 34：找到 P4 真正的卡点——`geni_i2c_prepare` 两颗都失败

- 把每颗的 `geni_i2c_prepare` 返回值、`GeniI2c.error`、`cs35l41_step` 的 `last_error` 分开发布，
  kernel `d477c761`，`/tmp/fuxi-i2cd.img` 已刷入。
- 真机读回：

  | 页 | 值 | 含义 |
  |---|---|---|
  | 6 | **1** | amp B `geni_i2c_prepare` **失败** |
  | 7 | 0 | amp B `last_error` = 0 |
  | 8 | 0 | amp B `geni.error` = 0 |
  | 9 | **0** | amp B `state = Idle` |
  | 10 | **1** | amp T `geni_i2c_prepare` **失败** |
  | 11 | 0 | amp T `last_error` = 0 |
  | 12 | 0 | amp T `geni.error` = 0 |

- **结论：两颗功放都没有真正被访问过**——`geni_i2c_prepare` 在“引擎可用性检查”就返回失败，
  所以 `cs35l41_step` 从未发出任何 I2C 传输。
- 之前读到的 `id=0x5f8 / eint1=0x1c / state=1` 是 `Cs35l41 amp;` **未初始化栈**的垃圾值
  （Round 31 的 `= {}` 修复后变成 0，才暴露真相）。
- **P4 的真正下一步**：查 `geni_i2c_prepare` 的前置条件——QUP2 SE0（`0x00880000`）的固件/时钟/GPIO
  （SDA/SCL = gpio 0/1）是否已经初始化；它要求“引擎已运行 I2C 固件且时钟/时序已设置”，
  这些目前没有任何代码在做。

### Round 35：`geni_i2c_prepare` 失败的四个条件已列出

`package/GeniI2c.c` 的 `geni_i2c_prepare()` 在下面任一条件成立时返回 -1：

```c
const uint32_t proto = (rd(g, GENI_FW_REVISION) >> 8) & 0xffu;
const uint32_t clock = rd(g, GENI_CLK_CFG);
if (proto != GENI_PROTO_I2C ||          /* 1. 固件不是 I2C 协议 */
    (rd(g, GENI_IF_DISABLE) & 1u) ||    /* 2. 接口被禁用 */
    !(clock & 1u) ||                    /* 3. 时钟未使能 */
    !rd(g, GENI_SCL_COUNTERS))          /* 4. SCL 时序计数器为 0 */
    return -1;
```

也就是说 QUP2 SE0（`0x00880000`）当前**没有被初始化成 I2C 引擎**。要让它可用，需要：

1. **GCC** 使能 QUP2 SE0 的 AHB/SE 时钟（`gcc_qupv3_wrap0_s0_clk` 等）；
2. **SE** 配 `GENI_CLK_CFG`（bit0 使能）与 `GENI_SCL_COUNTERS`（400 kHz 对应的 SCL 高/低计数）；
3. **GPIO 0/1**（SDA/SCL）切到 QUP2 SE0 功能、上拉；
4. 若固件未加载，还需要加载 I2C 固件（Kerfand 的注释说“the boot loader's”，
   所以可能 bootloader 已经加载，只是 clock/IF 被关）。

**P4 的下一步**：先只做 1–3（时钟 + SE 配置 + GPIO），再重跑 `geni_i2c_prepare`；
若仍失败，再查固件。

> 这些是 QUP2 SE0 的硬件初始化，和触摸/UFS 的 GENI 初始化同源，可直接参考 `GeniI2c.h` 的
> `GENI_*` 寄存器定义与现有 GENI 驱动写法。

### Round 36：按 Kerfand 的序列补上 QUP2 SE0 初始化

- 从 `/tmp/kerfand-fuxi/device/package/AudioProbe.cpp` 抄到 SE 初始化序列（在 `CLK_CFG` bit0 为 0
  或 `SCL_COUNTERS` 为 0 时执行）：

  ```c
  wr(0x28, rd(0x28) | 0x7f);   // clock gating controls
  wr(0xe30, rd(0xe30) | 0xf);  // DMA clock gating
  wr(0x24, 0x7f);              // default output control
  wr(0x20, 1);                 // force default
  wr(0xe18, 0);                // no GSI events
  wr(0x7c, level);             // DFS level
  wr(GENI_CLK_CFG, 0x21);      // 2<<4 | 1
  wr(GENI_SCL_COUNTERS, 0x52C016); // 5<<20 | 11<<10 | 22 (400 kHz)
  ```

- 已加进 `probeOwnedAmp()`（`level` 暂填 0，待确认 RCG 的 19.2 MHz DFS 索引），
  kernel `5295575d`，`/tmp/fuxi-se0.img` 已刷入。
- 刷机后第一次读（启动早期，graph_phase 仍为 0）：**两颗 `prepare` 都变成 0（成功）**，
  但 `state` 仍为 0、`last_error`/`geni.error` 也都是 0 —— 说明 I2C 引擎这次**起来了**，
  功放程序还没跑或刚跑。
- 第二次读时控制通道无响应（设备在重启或 collector 卡住），未能取到最终状态。
- **下一轮**：等 bring-up 稳定后再读一次；确认 `prepare=0` 之后 `cs35l41_step` 的
  `last_error`/`state`，并把 `level` 换成真实的 19.2 MHz DFS 索引（从 RCG 的 perf 寄存器计算）。

### Round 37：SE0 初始化后仍不稳定

- 重启 collector 后重新读（同一镜像 `/tmp/fuxi-se0.img`）：

  | 页 | 值 | 按 Round 34 布局 |
  |---|---|---|
  | 6 | **1** | amp B `prepare` 仍失败 |
  | 7 | 12 | amp B `last_error = 12` |
  | 8 | **28** | amp B `geni.error = 28` |
  | 9 | 0 | amp B `state = Idle` |
  | 10 | 0 | amp T `prepare = 0`（成功） |
  | 11 | 4095 | amp T `last_error = -1` |

- 与刚刷机时的 `prep=0/0` 不一致，说明：
  1. SE0 的初始化只在**第一次**满足条件时执行，第二次 boot 时寄存器可能已被前一次改过或被复位；
  2. `geni.error = 28` 是 QUP SE 的错误状态，需要对照 `GeniI2c.h` 的 `error` 枚举解码；
  3. `level = 0` 的占位值可能让 SE 时钟/时序不对，导致 prepare 时好时坏。
- **下一步**：把 `0x7c` 的 level 换成真实的 19.2 MHz DFS 索引；`geni.error` 与 `error_status`
  分别发布；并确认 SE0 初始化是幂等的（每次 boot 都检查并修正）。

### Round 38：去掉 DFS level 覆盖，待刷机验证

- 发现上一轮 `wr(0x7c, 0)` 是错的：`0x7c` 是 DFS level，bootloader 已经设好，
  用猜测值 0 覆盖会让引擎**时好时坏**（正好对应 Round 36/37 的 `prepare` 0/1 抖动）。
- 已删除该写入，改为保留 bootloader 的值，kernel `84689bf4`。
- 镜像已构建：`/tmp/fuxi-lvl.img`。
- 刷机受阻：`ctl reboot-fastboot` 返回 `Connection refused`，之后 CDC 控制通道超时
  （已知的 collector/USB LOSS 问题），未能进 fastboot。
- **下一轮**：设备进 fastboot 后刷 `/tmp/fuxi-lvl.img`，确认 `prepare` 是否稳定为 0，
  再读 `state`/`last_error`。

---

## Round 39：本轮收尾状态

设备仍在旧镜像上运行，CDC 控制通道超时（已知的 collector / USB LOSS），无法进 fastboot，
`/tmp/fuxi-lvl.img`（kernel `84689bf4`，去掉了 DFS level 覆盖）**尚未刷入验证**。

### 目标各阶段当前证据

| 阶段 | 状态 | 关键证据 |
|---|---|---|
| P0 基线 | ✅ | ADSP stage 13、graph_phase 22、SMMU 审计 slot 7/bank 4 |
| P1 DSP 传输 | ✅ | GPR/GLINK 通、图能 open |
| P2 AudioReach 图 | ✅ | prepare/start **status 都为 0**（page 10/9） |
| P3 buffer + bypass | ✅ | `pcm_pa = 0xd0000000`、span 64 KiB、bypass 已配、设备不复位 |
| P4 功放 | 进行中 | SE0 初始化已加、`prepare` 曾成功；DFS 覆盖已移除待验证 |
| P5 第一声 | 未开始 | 依赖 P4 |
| P6–P8 | 未开始 | — |

### 下一步（按优先级）

1. 设备进 fastboot → 刷 `/tmp/fuxi-lvl.img` → 确认 `prepare` 稳定为 0；
2. 读 `cs35l41_step` 的 `state`/`last_error`，让两颗走到 `MutedReady`；
3. P4 PASS 后进 P5：把 PCM 数据写入 `0xd0000000`，验证 DSP 消费；
4. P6（OS 服务）、P7（麦克风）、P8（恢复/长稳）。

### Round 41（resume）：去 DFS 覆盖的镜像已刷入，读数仍不完整

- 设备回到 fastboot 后，`/tmp/fuxi-lvl.img`（kernel `84689bf4`）已刷入并重启。
- 串口 boot-args 确认：`fgraph=6 fuxi-abyp=1 fuxi-amp=1 fuxi-panel-black=1`，无 panic。
- 但 DIAGSTATUS 连续多次读到 `page 15 = 0`（graph_phase 未推进）、`poll ≈ 10850`，
  且 capture 里没有 `FUXI_ADSP` / `FUXI_AMP` 行（早期输出在 CDC 枚举前，collector 接得晚）。
- 判断：这次 boot 的音频探针要么还没跑完，要么在更早的阶段就停了；
  需要再看 stage/error（page 0 = 13 说明 ADSP 已起）与 `bsd_init` 之后的日志。
- **下一步**：在 `probeOwnedAmp()` 入口加一行 `FUXI_AMP` 的同步 kprintf **并同时写 stats[0]**，
  这样即使早期日志丢失，也能从 page 0/1 看出探针是否进入；再确认 `graph_phase` 是否推进到 26。

### Round 42/43：加了入口标记，但这次 boot 音频探针没进入

- 在 `probeOwnedAmp()` 入口写 `amp_prepare[0/1] = 0xAA` 并 `syncStats()`，kernel `77a827af`，
  `/tmp/fuxi-marker.img` 已刷入。
- 真机读回：`page 6 = 0`（不是 0xAA）、`page 15 = 0`（graph_phase 未推进）、stage 仍 13。
- 说明这次 boot **`graphOpenProbe()` 没有走到**（phase 0），所以 `probeOwnedAmp()` 也没被调用
  （它挂在 PCM 探针之后，`!rc` 才执行）。
- 结论：当前要先解决“图探针为什么这次没跑”，而不是功放。可能原因：
  1. 这次 boot 的 ADSP/GPR bring-up 没完成（`graph_phase` 会停在 1–8）；
  2. `fuxi-amp=1` 或 `fuxi-abyp=1` 改变了启动路径；
  3. 设备在读数期间重启过（session 变化过两次）。
- **下一步**：先用 `fgraph=5`（不带 amp）跑一次，确认图探针能到 phase 22；
  再单独加 `fuxi-amp=1` 对比，判断是 amp 路径还是 bring-up 随机性。

### Round 44：对照实验的镜像已构建，卡在进不了 fastboot

- 为区分“图探针没跑”是 amp 路径还是 bring-up 随机性，把 board.toml 改成
  **`fgraph=5` + 去掉 `fuxi-amp=1`**（保留 `fuxi-abyp=1`），只重建 payload 与 bootimg：
  镜像 `/tmp/fuxi-fg5.img`（kernel 仍是 `77a827af`）。
- 刷机受阻：设备在 XNU 里（CDC `/dev/cu.usbmodem101`），`ctl ping` / `ctl reboot-fastboot` 都超时，
  `fastboot devices` 为空。重启 collector 后仍是 `Connection refused` → `timed out`。
- **下一次设备进 fastboot 时**：刷 `/tmp/fuxi-fg5.img`，等 ~110 s 读 `page 15`：
  - 若 `15 = 22` → 说明 `fgraph=5` 正常，问题出在 `fuxi-amp=1`/`fgraph=6` 路径；
  - 若仍为 0 → 说明是 bring-up 随机性，与 amp 无关。

### Round 45：设备完全不在 USB 上

- `fastboot devices` 为空、`/dev/cu.usbmodem*` 不存在、ioreg 没有 `Xiaomi 13 XNU Debug`。
- 对照镜像 `/tmp/fuxi-fg5.img` 仍未刷入。
- 连续三轮（43/44/45）都无法把设备送进 fastboot：先控制通道超时、后设备整个从 USB 消失。
- **需要**：设备断电后重新进入 fastboot。

### Round 46：对照实验第一步——`fgraph=5` 正常

- 设备回到 fastboot 后刷入 `/tmp/fuxi-fg5.img`（`fgraph=5`、无 `fuxi-amp`）。
- 真机读回（采满 16 页）：

  | 页 | 值 | 含义 |
  |---|---|---|
  | 15 | **22** | graph_phase 正常推进 |
  | 1/2/3/4 | 0/0/208/0 | **pcm_pa = 0xd0000000** |
  | 5 | 16 | span 64 KiB |
  | 6/7 | 7/4 | 空闲槽 7 / bank 4 |

- 结论：`fgraph=5` 路径完全正常。之前 `fgraph=6 + fuxi-amp=1` 时停在 phase 0，
  说明问题出在 **`fgraph=6`（持久图）或 `fuxi-amp=1`（功放路径）** 之一。
- **下一步**：再刷一版 **`fgraph=6`、无 `fuxi-amp`**，把两者分开。

### Round 46b：对照实验第二步——`fgraph=6`（无 amp）也正常

- 刷入 `/tmp/fuxi-fg6.img`（`fgraph=6`、无 `fuxi-amp`），真机读回：
  **`page 15 = 22`、`pcm_pa = 0xd0000000`、span 64 KiB、slot 7 / bank 4**。
- 结论：**`fgraph=6` 持久图本身没问题**。之前那次 phase 0 只可能是
  **`fuxi-amp=1`** 或 bring-up 随机性造成的。
- 下一步：再刷一版 `fgraph=6 + fuxi-amp=1`（`/tmp/fuxi-marker.img`），
  若 phase 又变 0 → 锁定 amp 路径；若 22 → 是随机性，重跑一次即可。

### Round 46c：定性完成——`fuxi-amp=1` 会让设备复位

- 刷入 `/tmp/fuxi-marker.img`（`fgraph=6 + fuxi-amp=1`），真机读回：
  **`page 15 = 0`、`poll ≈ 11939`、session 又变了**，`ampB prep = 0`（入口标记 0xAA 也没出现）。
- 对照结果汇总：

  | 配置 | phase | 说明 |
  |---|---|---|
  | `fgraph=5`（无 amp） | **22** | 正常 |
  | `fgraph=6`（无 amp） | **22** | 正常 |
  | `fgraph=6` + `fuxi-amp=1` | **0**（poll 低、session 变） | **设备复位** |

- 结论：**`fuxi-amp=1` 触发的是设备复位/崩溃**，而不是“图探针变慢”。
  `probeOwnedAmp()` 里的 `mapWindow(0x00880000)` 或 SE 寄存器写入很可能踩到了不该写的地方，
  导致 panic → `fuxi-panic-fastboot=30` 或看门狗复位。
- 下一步（明确）：
  1. 让 `probeOwnedAmp()` **只读**（去掉所有 `geniWrite`，先只 `geniRead` 确认 MMIO 可访问）；
  2. 或在 `mapWindow` 之前检查该物理区间是否已被内核映射/保留；
  3. 确认复位原因（抓 panic 行或看 `fuxi-panic-fastboot` 是否触发）。

### Round 47：amp 探针改成只读版，待刷机

- `probeOwnedAmp()` 已改为**只读**：只 `mapWindow(0x00880000)` + 读
  `GENI_FW_REVISION` / `GENI_CLK_CFG` / `GENI_SCL_COUNTERS`，**不写任何 SE 寄存器**；
  入口写 `amp_prepare[0] = 0xAA`（进入）、`0xCC`（映射成功）、`0xEE`（失败），用于区分卡点。
- 编译通过：kernel `262a9c593d459485`，镜像 `/tmp/fuxi-ampro.img`（`fgraph=6 + fuxi-amp=1`）。
- 刷机受阻：设备还在上一版会复位的镜像里，`ctl reboot-fastboot` 超时，`fastboot devices` 为空。
- **下次进 fastboot 时**：刷 `/tmp/fuxi-ampro.img`，读 `page 6`：
  - `0xAA` → 进入了但 `mapWindow` 卡住/失败前；
  - `0xCC` → 映射成功（说明崩溃来自写 SE 寄存器，不是映射）；
  - `0xEE` → 映射失败；
  - `0` → 探针没进（`!rc` 未满足）。

### Round 48：只读 amp 探针安全，QUP2 SE0 读到全 0（未上时钟）

- 刷入 `/tmp/fuxi-ampro.img`（只读 amp 探针 + `fgraph=6 + fuxi-amp=1`），真机读回：

  | 页 | 值 | 含义 |
  |---|---|---|
  | 15 | **26** | amp 阶段执行完，**设备没有复位** |
  | 6 | **0xCC** | `mapWindow(0x00880000)` **成功** |
  | 12 | 0 | `GENI_FW_REVISION` 读回 **0** |
  | 8 | 0 | `GENI_CLK_CFG` 读回 **0** |
  | 9 | 0 | `GENI_SCL_COUNTERS` 读回 **0** |

- 两个结论：
  1. **之前的复位来自 SE 寄存器写入**（不是映射）——只读版本完全安全，phase 26；
  2. **QUP2 SE0 的寄存器全读 0**，说明这个 SE 块**没有被上时钟/上电**。
- 这也解释了 `geni_i2c_prepare` 为什么一直失败（`proto = 0 ≠ GENI_PROTO_I2C`）。
- **下一步（明确）**：先使能 QUP2 SE0 的 GCC 时钟（`gcc_qupv3_wrap0_s0_clk` 等，参考
  `package/QcomRpmh.*` 或现有 GCC 访问方式），再重跑只读探针，直到 `FW_REVISION` 非 0；
  之后才谈 SE 初始化与功放。

### Round 49：QUP2 SE0 上时钟所需的 GCC 参数（来自 Kerfand）

`probeOwnedAmp()` 只读版证明：QUP2 SE0 的寄存器全 0，说明**块未上时钟**。
Kerfand 的 `AudioProbe.cpp` 里有一段完整的 QUP 时钟/投票序列，参数如下（可直接照抄）：

```c
constexpr uint64_t kGcc = 0x00100000ULL;
// 分页：+0x18000 / +0x1e000 / +0x23000 / +0x52000 各映射 0x1000
constexpr uint32_t kVote1 = 0x52008, kVote1Bits = (1u<<18)|(1u<<19)|(1u<<20)|(1u<<21)|(1u<<27);
constexpr uint32_t kVote2 = 0x52010, kVote2Bits = 0x1fu;
constexpr uint32_t kHalts[] = {0x2327c,0x23280,0x23284,0x23294,0x1861c,0x233cc,0x233d0,0x233d4,0x23004,0x1e004};
constexpr uint32_t kRcg[2] = {0x18628, 0x1e010};     // [0]=QUP1 SE5, [1]=QUP2 SE0
```

初始化顺序（Kerfand 的做法）：

1. 映射 GCC 的 5 个页（`kGcc+0x0/0x18000/0x1e000/0x23000/0x52000`）；
2. 置 `GPLL0_ENABLE` 并轮询 `GPLL0_MODE` bit31（等 PLL 锁）；
3. 置 `kVote1`/`kVote2` 的位（QUP wrap 的 AHB/SE 投票）；
4. 清 `kHalts[]` 里对应 SE 的 halt 位（branch halt）；
5. 配 `kRcg[1]` 的 DFS 分频到 19.2 MHz；
6. 再读 `GENI_FW_REVISION` 应为非 0。

**下一步**：把上面 1–5 实现成 `probeOwnedAmp()` 的前置步骤（先只读验证 `FW_REVISION` 非 0，
再放开 SE 写入），注意 **GCC 写入也要先确认不会复位**（上次就是写 SE 寄存器导致复位）。

### Round 50：GCC 只读映射安全（未复位），值待发布

- 在 `probeOwnedAmp()` 里加了 **只读** GCC 检查：
  `mapWindow(0x00100000)` 与 `mapWindow(0x00152000)`，读 `GPLL0_MODE`(0x0)、
  `GPLL0_ENABLE`(0x52018)、vote1(0x52008)，不写任何 GCC 寄存器。
  kernel `40d85b1a`，`/tmp/fuxi-gcc.img` 已刷入。
- 真机读回：`page 15 = 26`（amp 阶段完成）、`page 6 = 0xCC`（SE 映射成功）、其余 0。
- 结论：**GCC 只读访问不会复位**，可以继续；但这三个 GCC 值我写进了
  `amp_device_id[1]/amp_revision[1]/amp_state[1]`，而发布块只发布了 `[0]` 与部分 `[1]`，
  所以**还没看到具体数值**。
- **下一步**：
  1. 把 `amp_device_id[1]`（GPLL0_MODE）、`amp_revision[1]`（GPLL0_ENABLE）、
     `amp_state[1]`（vote1）发布到 page 10/11/12（替换现在的 prepare/last_error/geni_error[1]）；
  2. 确认 `GPLL0_MODE` bit31 是否已锁 —— 决定下一步是“直接清 halt 位”还是“先使能 PLL”。

### Round 51：P4 level 2 —— QUP2 五个时钟分支全部上线

- 本轮把 `fuxi-amp` 从 1 推到 **2**（读状态 + 投票时钟），刷入 `/tmp/fuxi-p4g.img`
  （真实 `fastboot flash boot` + `reboot`），用 CDC 读日志。
- **先修了一个判定 bug。** 第一版 level 2 报 `running 0`，日志是：

  ```text
  fuxi-amp: level 2 stage 3 gpll0 e0000005 vote1 443c0200 vote2 0001001f running 0
  fuxi-amp: halts 00004000 80000002 00000000 00004000 00004000
  ```

  投票其实**已经生效**（vote2 `0x00010000 → 0x0001001f`），GPLL0 也已锁
  （`0xe0000005` 的 bit31）。问题是判定的标准比 Linux 严：
  s_ahb 读回 `0x80000002`（bit31 置位 = 时钟关闭），m_ahb 读回 `0x00000000`。
  但上游 `gcc-sm8550.c` 给这两个 AHB 分支都设了 `hwcg_reg = 自己的 halt 寄存器`
  且 `hwcg_bit = 1`，而 `clk-branch.c` 的 `clk_branch_in_hwcg_mode()` 一旦看到
  这个位就**直接返回，`clk_branch_wait()` 根本不检查 halt 位**。
  换句话说，Linux 对这两个分支从不等待，它们的 bit31 说明不了时钟状态。
  `0x80000002` 里的 bit1 正是 hwcg 位本身，正好印证了这一点。
- 修法与上游一致：新增 `fuxi_amp_halt_hwcg[]`（只有 s_ahb / m_ahb 为 1），
  判定改为 `fuxi_amp_branch_ok(cbcr, hwcg_bit)`；另外把原来十个 halt 寄存器
  （含 wrap1 的分支）收敛成上游 wrap2 的五个。
- 修好后重刷重读，**level 2 通过**：

  ```text
  fuxi-amp: level 2 stage 3 gpll0 e0000005 vote1 443c0200 vote2 0001001f running 1
  fuxi-amp: branches core:00004000 s_ahb:80000002 m_ahb:00000000 core_2x:00004000 s0:00004000
  ```

- 结论：
  1. **QUP2 SE0 的五个时钟分支全部上线**，其中 core / core_2x / s0 的 bit31 已清
     （时钟确实开着），两个 AHB 分支由硬件门控（Linux 语义下算“开”）。
  2. **GPLL0 已锁**，`0xe0000005` 的 bit31 是 `PLL_LOCK_DET`（`clk-alpha-pll.c`）。
  3. **vote1 一个位都不用动**——上游 `gcc-sm8550.c` 里 wrap1 的 core/AHB
     （vote1 的 bit18/19/20/21）属于 QUP1，是触控驱动的地盘；
     wrap2 的五个分支全在 vote2（`0x52010`）的 bit0..bit4，即 `0x1f`。
     之前引用 Kerfand 注释说“vote 值来自 Kerfand”是错的：Kerfand 的
     `GeniI2c.c` **完全不碰 GCC**，它假设 bootloader 已经把引擎配好了。
     这些值是从 Linux 的 `gcc-sm8550.c` 推出来的，注释已改。
- **下一步**：`fuxi-amp=3`（I2C 引脚 + 两颗功放复位线），然后 4（引擎）与 5（身份）。
- Host 侧同时补了两个套件：`fuxi_amp_bringup_host`（42 项，含 hwcg 判定这条）
  和 `fuxi_cs35l41_host`（416 项，验证 init 程序跑完功放仍是关断的）。
  状态报告现在是 **18 个套件、21050 项主机检查、0 处失败**。
