# Fuxi audio probe 调试记录

按 `docs/fuxi-audio-probe-debug-runbook.md` 的模板记录每一轮。结果只有 **PASS / FAIL / INCONCLUSIVE** 三种，
`host PASS` 不等于设备 PASS。

---

## H0 — 默认调用链不可达

```text
test_id: H0
result: PASS
device_commit: e133ba1 (设备仓库 V2)
kernel_commit: 未参与（纯静态检查）
image_sha256: 未参与
control_protocol_revision: 未参与
boot_args: board.toml 的 kernel_args 不含 fuxi-audio-probe
generation: 未参与
step_id: 未参与
mapping_api: 未参与
requested_cache_attribute: 未参与
observed_cache_attribute: 未参与
smem_before_checksum: 未参与
smem_after_checksum: 未参与
hwlock_acquire_rc: 未参与
hwlock_release_seen: 未参与
begin_seen: 未参与
end_seen: 未参与
ping_before: 未参与
ping_after: 未参与
deadline_ms: 未参与
last_small_checkpoint: 未参与
panic_or_reset_evidence: 无（未接触设备）
unexpected_diff: 无
classification: 默认构建中 native_audio_probe 只有一处声明与一处定义，没有任何调用点；
                board.toml 不传 fuxi-audio-probe；因此默认镜像不可自动执行音频探针
next_single_measurement: H1（控制协议 codec 与拒绝策略）
```

门禁：`python3 scripts/check-fuxi-audio-probe-h0.py`

它做三件事：全树搜索 `native_audio_probe` 并把命中分类为声明 / 定义 / 调用；把任何非声明非定义的命中当作调用点并
判 FAIL；检查 `board.toml` 的 `kernel_args` 是否带 `fuxi-audio-probe`。检查脚本自身与 `scripts/report-*` 不是构建输入，
已排除，否则它们提到函数名就会被误判成调用点。

**不做的事**：不据此宣称设备上没有回归——runbook 要求三次稳定启动（G0）才能把"代码上不可达"升级为"设备上无回退"。

---

## H5 — SMEM 解析器只读模糊测试

```text
test_id: H5
result: PASS
device_commit: e133ba1（设备仓库 V2）
kernel_commit: 未参与（host 测试）
image_sha256: 未参与
boot_args: 未参与
smem_before_checksum: 每个变异体在解析前记录
smem_after_checksum: 只读调用后必须与 before 逐字节相同
unexpected_diff: 无
classification: 14 个单点变异（坏 magic、超大 entry count、未知 version、零/越界 partition offset、
               溢出 size、缺 partition magic、header 与 entry 不一致、free 指针逆序、坏 canary、
               item size 越界、padding 溢出、未对齐 offset、非 2 的幂 cacheline）+ 4000 轮噪声，
               全部被拒或停留在界内；只读调用未改变任何字节
next_single_measurement: H6（shadow allocation 的 byte diff）
```

门禁：`python3 scripts/check-fuxi-audio-probe-h5.py`

它复用 A0 夹具构造一张**合法**镜像，然后每次只改一个字段，要求解析器要么拒绝、要么在界内；随后用 memcmp 证明只读
调用（`qcom_smem_partition` / `find` / `item_count` / `item_at`）**一个字节都没改**。噪声部分要求 4000 轮随机镜像
**没有任何一次**被当成合法分区表。

**不做的事**：不据此宣称真实 SMEM 的页表属性或 cache 语义正确——那是 G2.6 的设备义务。

---

## H6 — shadow allocation byte-diff

```text
test_id: H6
result: PASS
device_commit: 工作区未提交（host 测试）
kernel_commit: 未参与
image_sha256: 未参与
boot_args: 未参与
smem_before_checksum: ba14d21544394d58
smem_after_checksum: 108a80e8bc8c8a6b
unexpected_diff: 无
classification: 等价构造 SMEM image 的未使用 partition 区域预填 0xcc；一次 13-byte item allocation
                只修改 16-byte entry header 和 offset_free_uncached。payload 16 bytes 保持 0xcc；
                write trace 为四个非发布 entry writes，最后且唯一一次 publication 是 free offset。
                重复 allocation 返回 Exists 且零写；空间不足返回 NoSpace 且零写。
next_single_measurement: H7（bounded hwspinlock host model）
```

门禁：`python3 scripts/check-fuxi-audio-probe-h6.py`

该门禁同时在 ASan/UBSan 下运行 16 项检查。allocator 不再隐式清零 payload；协议数据初始化必须在后续单独、可审计的
transaction 中完成。测试 trace 由 `QCOM_SMEM_TEST_TRACE_WRITES` 仅在 host 测试构建启用，生产构建没有 trace 分支。

**不做的事**：它没有映射真实 SMEM、没有取得 hwspinlock，也没有证明 Darwin mapping 属性、互连可见性或真实
`dmb oshst` 效果。真实 snapshot 上的普通 RAM shadow 验证仍属于 G4；第一次真实写仍须等待 G2-G4 全部通过。

---

## H3 — 并发请求下的唯一 owner

```text
test_id: H3
result: PASS
device_commit: 51ec7d4（CDC 子仓库） / 设备仓库 V2
kernel_commit: 未参与（host 测试）
image_sha256: 未参与
control_protocol_revision: DIAGSTATUS/DIAGARM/DIAGNEXT/DIAGCANCEL/DIAGRESET
generation: 覆盖 1..3
step_id: 覆盖 1
hwlock_acquire_rc: 未参与
begin_seen: 未参与
end_seen: 未参与
ping_before: 未参与
ping_after: 未参与
deadline_ms: 25（tick 单位）
panic_or_reset_evidence: 无
unexpected_diff: 无
classification: 两次 NEXT 只有一次入队；两次 claim 只有一次成功；状态洪泛不改变任何字段；
               RUNNING 时 RESET 被拒；NEXT 后 CANCEL 使该步不可再被 claim；旧 generation 的 ticket 无法完成新步
next_single_measurement: H4
```

发现并修掉一个真实语义缺陷：**ticket 原来是值语义**——把赢家的 ticket 复制一份就能"再次完成"同一步。
现在 ticket 带单调 id，状态在步结束（完成 / 超时 / 取消 / 断连）时失效该 id，因此**"精确一次"是被强制的**，
不再是假设。

## H4 — deadline 与超时上界

```text
test_id: H4
result: PASS
device_commit: 51ec7d4（CDC 子仓库）
kernel_commit: 未参与（host 测试）
deadline_ms: 25 / 10（tick 单位）
panic_or_reset_evidence: 无
classification: budget=0 被拒（不存在无界步）；deadline-1 仍 RUNNING；到达 deadline 记 TIMEOUT；
               已超时后再完成返回 STALE，不能把超时改写成成功；超时后 state 仍可审计（step/generation/last_rc）；
               reset 是唯一恢复路径，且下次 arm 是新 generation
next_single_measurement: H5（已完成，见上）；G1.2 安全空 worker
```

门禁：`python3 -B drivers/QcomXnuUsbDebug/tests/check.py --target-clang <clang> --output <dir>` → **31 项全部通过**（ASan/UBSan）。

---

## 基线冻结（2026-11-09）

| 仓库 | 分支 | 提交 |
| --- | --- | --- |
| UniXNU | `main` | `b973dae`（payload 校验 Data seed 的 APFS UUID） |
| 设备仓库 | `V2` | `50027b2` |
| CDC 子仓库 | `main` | `51ec7d4` |

**已验收镜像**（G1 NOOP 真机 PASS，来自 runbook 第 13 节）：

```text
kernel  sha256 c793f715fa99ba6da2d8f8005c8981b939a74d54952048e384af1580c2bd8ca6
boot.img sha256 5e068a4b525f4c86f29bdb6aa1ab62b3f0653bc846f6fb8c85cf27e9ccd14d24
```

**当前工作区产物不是已验收镜像**：`out/fuxi/.../kernel/kernel.macho` 为单步探针实验所构建，
sha256 `5917b722da70c9cef75c127387afa4d1f9ec7dd6be17f2718443093bf1aa4065`。
任何 G2 实验必须记录它实际运行的内核哈希，不得把两者混为一谈。

重建已验收镜像的做法：用上表三个提交，`CONFIG_XNU_OWNER_DATA_MOUNT=y`、Data seed 用
`out/fuxi/build/owner-data-fixture/owner-data-16g.dbr`（UUID `B2A023C5-CC73-45C1-A9E6-26AC8BDD2822`），
且 `board.toml` 不含 `fuxi-audio-step`。

---

## H7 — 有界硬件自旋锁的策略层

```text
test_id: H7
result: PASS
device_commit: 503866a（设备仓库 V2）
kernel_commit: 未参与（host 测试）
hwlock_acquire_rc: 覆盖 Ok / Busy / Recursive / NotHeld / NoDevice / DeviceError
hwlock_release_seen: 成功路径恰好一次；超时路径零次；双重释放被拒且不下发
deadline_ms: 5 / 10（tick）
panic_or_reset_evidence: 无
unexpected_diff: 无
classification: 32 项断言。free 路径一次获取一次释放；忙后放行在 deadline 内成功且尝试次数精确；
               永不释放 → 到期 Busy、无释放、轮询有界；budget=0 只尝试一次；无 device 时零动作；
               递归获取被拒且不下发；板级硬错误 → DeviceError 且不持有；释放失败仍保持状态可审计；
               两个调用方竞争只有持有者成功、释放后另一方才获取；deadline 跨 UINT32 回绕仍以 Busy 结束；
               budget 无时钟被拒而不是变成自旋
next_single_measurement: H8（mapping helper 合约）；G3 需先有可引用的寄存器编码
```

门禁：`python3 scripts/check-qcom-hwspinlock-host.py`（ASan/UBSan）。

**分工说明**：`package/QcomHwspinlock.c` 只拥有 runbook 要求的**策略**——带 deadline 的获取、忙则返回、递归即拒、
错误路径不泄漏、不持锁输出。**寄存器编码属于 SoC**，通过 `struct QcomHwlockDevice` 由板级注入，host 测试用脚本化
设备代替它。这样"策略可证、编码不臆造"。

## H7 的板级事实（来自 fuxi 自己的 DTB）

反编译 `uefi-fuxi/Platforms/Xiaomi/fuxiPkg/FdtBlob/sm8550-xiaomi-fuxi.dtb`：

```text
tcsr_mutex_block: syscon@1f40000   reg = <0x1f40000 0x20000>
tcsr_mutex:       hwlock          compatible = "qcom,tcsr-mutex", syscon = <&tcsr_mutex_block 0x0 0x1000>
smem:             qcom,smem       hwlocks = <&tcsr_mutex 3>
```

已写入 `facts/audio/manifest.toml` 的 `[hwlock]`（`runtime_verified = false`——设备树说明接线，不说明运行时状态）
与 `facts/audio/binding.toml` 的 `[hwlock]`（5 个字段全部 `unverified`）。

**G3 仍然不能开始**：锁寄存器的具体编码（哪一位获取、哪一位释放、owner 字段位置）必须由主线
`drivers/hwspinlock/qcom_hwspinlock.c` 一类可引用来源确定。本轮尝试取原文失败——
`git.kernel.org` 与 `raw.githubusercontent.com` 被 DNS 策略拒绝，`web_search` 鉴权失败（HTTP 401）——
因此**不写入任何编码**，G3 挂起等待来源。

binding 现状：112 字段，0 verified，**13 项现在引用已记录事实**（本轮新增 5 项 DTB 事实）。

---

## H8 — mapping helper 合约（S2 前半）

```text
test_id: H8
result: PASS
device_commit: b7e123f（设备仓库 V2）
kernel_commit: 未参与（host 测试）
mapping_api: package/QcomMapping.c + 注入式 backend（host 用 mock）
requested_cache_attribute: 未参与（本层不做属性）
observed_cache_attribute: 未参与
unexpected_diff: 无
classification: 31 项断言。非页对齐 PA → backend 收到页对齐基址；跨页范围 → 窗口扩到两页；
               偏移与页倍数长度不多要一页；零长度、回绕溢出、无 backend 三种拒绝都不触达 backend；
               backend 失败不留任何状态且其后 unmap 返回 NotMapped；重复 map 被拒（backend 只获取一次）；
               重复 unmap 被拒（release 只发生一次）；release 收到与 acquire 完全相同的 VA；
               4 KiB 与 16 KiB 页对同一请求给出不同窗口与相同偏移；page_size=0 取 16 KiB 默认
next_single_measurement: G1.3 的 host 部分（32/64 字节固定日志与回压计数）
```

门禁：`python3 scripts/check-qcom-mapping-host.py`（ASan/UBSan）。

**边界**：本层只证明算术与对称性。页表属性、MAIR 与 cache alias **不能**由 host 证明——那是 G2.6 的设备义务，
runbook 已把这一点写死。

---

## G3 前置解除了：TCSR 编码有可引用来源

原先 G3 卡在"锁寄存器编码不能臆造"。现在有了来源，而且来源是被核对过的代码而不是二手描述：

```text
来源: drivers/hwspinlock/qcom_hwspinlock.c @ torvalds/linux 7475ea8b58616b33c04afb976cf0fbaad9dd63fb
tcsr-mutex 匹配数据: offset 0, stride 0x1000, 每 bank 32 把锁, regmap reg_stride 4, max_register 0x20000
获取: 向字段写 QCOM_MUTEX_APPS_PROC_ID(1), 再读回, 读回等于 1 才算拿到
释放: 读 owner, 然后写 0（即使 owner 不是我们，主线也照写 0，只是先报错）
字段: REG_FIELD(base + id * stride, 0, 31) —— 整个 32 位寄存器就是一把锁
```

与 fuxi 自己的 DTB 对得上：`tcsr_mutex` 的 `syscon = <&tcsr_mutex_block 0x0 0x1000>`，SMEM 用 `hwlocks = <&tcsr_mutex 3>`，
因此 **锁 3 的寄存器是 0x1f40000 + 3×0x1000 = 0x1f43000**。

已实现 `package/QcomTcsrHwlock.c`（编码，参数化：base/stride/locks_per_bank/owner_id 都是参数），并与 H7 的
策略层通过 `QcomHwlockDevice` 对接。

```text
test_id: G3-host
result: PASS
mapping_api: package/QcomTcsrHwlock.c + 注入式寄存器访问（host 用数组模拟）
hwlock_acquire_rc: 覆盖 Ok / Busy（写丢失、读回不匹配）/ OwnerMismatch / BadId
classification: 23 项断言。偏移：锁 0=0x1f40000、锁 3=0x1f43000、锁 31=0x1f5f000，锁 32 与 UINT32_MAX 越界被拒；
               获取=一次写 owner id + 一次读回；写丢失（寄存器保留别人的 id）→ Busy；
               释放别人的锁 → OwnerMismatch 但仍写 0；越界 id 不触达寄存器；
               策略层通过编码适配器获取=一次写；重复获取被策略层拒（寄存器不再被写）；释放清 0
next_single_measurement: G3 真机（同一个适配器对着真实 MMIO）
```

binding：118 字段，0 verified，**19 项引用已记录事实**（本轮 +6）。仍然 0 verified 是对的——**没有一个值在真机上被测量过**。

## 另一个来源：fuxi 的 Linux 移植仓库

`https://github.com/ilovecnxx/xiaomi13-fuxi-linux-ubuntu26.10-kernel7.3`（`main` = `7f12106`）已在 `/tmp/fuxi-linux`
稀疏克隆。它与我们目标相关的部分：

- `kernel/patches/0004-audio.patch`、`work/patches/audio-bringup-20261029/`（`audio.dtsi`、`sc8280xp.c` 机器驱动、
  `stereo-routing.patch`、UCM 配置）——**S10/A3/A5 的板级事实来源**；
- `device-info/cmd-db-fuxi.txt`——从设备抓下来的 **ADSP 命令数据库**，正是 A1（GPR/APM）要用的；
- `research/01-mainline-drivers.md`——主线驱动调研。

下一轮起用它给 `facts/audio` 补来源（尤其是当前 91 项无来源字段里的板级接线与命令面）。
