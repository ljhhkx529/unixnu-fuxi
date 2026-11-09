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

---

## S10 文件审计（第一部分）：独立 fuxi Linux 端口的板级事实

来源：`https://github.com/ilovecnxx/xiaomi13-fuxi-linux-ubuntu26.10-kernel7.3` @ `7f12106`，文件
`work/patches/audio-bringup-20261029/audio.dtsi.before` 与 `audio-fixes.patch`。

这个端口**在这块板上真的把扬声器跑起来了**，所以它比推断强，但仍然不是对我们自己路径的测量。

新记录的事实（manifest `[linux_port]`）：

```text
I2C:   i2c8 = QUP2 SE0 @ 0x00880000, 400 kHz, GPIO0/GPIO1（mirb 组，不是默认的 56/57）
I2S:   TLMM I2S1 —— SCK GPIO121, WS GPIO123, DATA1(出) GPIO124, DATA0(入) GPIO122；LPASS 为 BCLK/WS 主
DAI:   dai@18 { reg = <SECONDARY_MI2S_RX>; } → 端口号 0x18
时钟:  LPASS_CLK_ID_SEC_MI2S_IBIT + COUPLE_NO
声卡:  xiaomi,fuxi-sndcard / model "Xiaomi-13" / 拓扑 qcom/sm8550/Xiaomi-13-tplg.bin
功放:  2× CS35L41，@40="B"（右声道，兼下扬）、@42="T"（左声道，兼听筒）；复位 183/182、IRQ 99/98 低电平
功放参数: boost 4000 mA / ind 1000 nH / cap 15 µF / asp-sdout-hiz 3 / gpio2-src-select 4 / gpio2-output-enable
固件:  cirrus/cs35l41-dsp1-spk-prot-xiaomi-fuxi-B.{wmfw,bin}（T 同理）
听筒:  GPIO68（simple-audio-mux，Speaker/Earpiece）
```

与已有记录的关系：**功放地址、复位、IRQ、boost 参数与厂商 DTS 完全一致**（互相印证）；新增的是 DAI 端口号、IBIT 时钟、
I2C 频率与 GPIO、GPIO2 配置、固件名与听筒 mux。

写入 binding 的新段（全部 `unverified`，因为还没有我们自己的设备测量）：

```text
[interface.mi2s]  port_id 0x18, sck 121, ws 123, data_out 124, data_in 122
[interface.i2c8]  mmio_base 0x00880000, frequency_hz 400000, sda 0, scl 1
[amp.gpio2]       src_select 4, output_enable 1
```

binding 现状：**129 字段，0 verified，30 项引用已记录事实**（本轮 +11），8 项 sheng 预测，91 项仍无来源。

91 项里最大的一块是 `[amp.registers]` 的 28 个 `reg_*`（CS35L41 寄存器地址）。下一轮用同样的办法从主线
`sound/soc/codecs/cs35l41.h` 取名字与地址——那是 A3/S14 的真实依据，不是猜。

---

## S10 文件审计（第二部分）：CS35L41 的寄存器地址有出处了

来源：主线 `include/sound/cs35l41.h` 与 `sound/soc/codecs/cs35l41-lib.c`
（`torvalds/linux@7475ea8b58616b33c04afb976cf0fbaad9dd63fb`，shell 里 curl 可取）。

**只填了名字与用法都能对上的**：

| binding 字段 | 主线定义 | 地址 |
| --- | --- | --- |
| `reg_device_id` | `CS35L41_DEVID` | 0x00000000 |
| `reg_revision` | `CS35L41_REVID` | 0x00000004 |
| `reg_global_enables` | `CS35L41_PWR_CTRL1`（bit0 = `GLOBAL_EN_MASK`，写入处已核对） | 0x00002014 |
| `reg_block_enables` | `CS35L41_PWR_CTRL2`（boost/监视器/主功放的位都在这） | 0x00002018 |
| `reg_mute` | `CS35L41_AMP_OUT_MUTE` | 0x00002024 |
| `reg_pll_ctrl` | `CS35L41_PLL_CLK_CTRL` | 0x00002C04 |
| `reg_boost_peak` | `CS35L41_BSTCVRT_PEAK_CUR` | 0x00003808 |
| `reg_irq1_mask1` | `CS35L41_IRQ1_MASK1` | 0x00010110 |

**明确没填的**（写进 manifest 的 `unnamed` 字段）：`reg_asp_enables`、`reg_asp_ctrl`、`reg_boost_cap`、
`reg_irq1_eint1`、`reg_irq1_eint2`。原因各不相同——块使能是 `PWR_CTRL2/3` 的位而不是独立寄存器；ASP 使能掩码属于
头文件里没按这个名字命名的控制寄存器；boost 电容是系数而不是单独寄存器；两个 EINT 字段在头文件里叫
`IRQ1_STATUS1/STATUS2`，与本项目的命名不一致。**名字对不上就不写**，这比填一个看起来合理的数字强。

binding 现状：**129 字段 · 0 verified · 37 项引用已记录事实（本轮 +8）· 8 项 sheng 预测 · 84 项无来源**（上一轮 91）。

---

## 证据模型补上"第三种数值"：我们自己选的

原来 binding 只有 `verified` / `unverified`，于是"我们还没决定 ABI 的帧长"和"我们不知道功放地址"被混成同一个
"无来源"数字。现在分三类：

| 状态 | 含义 | 出处形式 |
| --- | --- | --- |
| `verified` | 真机测量过 | manifest 里 `runtime_verified = true` 的事实 |
| `unverified` | 有来源但没测量过 / 还没有来源 | manifest 键，或 `none` |
| **`chosen`** | **我们自己的设计选择** | `design:docs/fuxi-audio-abi-choices.md#<anchor>` |

理由文档：`docs/fuxi-audio-abi-choices.md`，逐条给出 `abi-rate`、`abi-channels`、`abi-period`、`abi-host-clock`、
`capture`、`path-periods-in-flight`、`amp-mute-polarity` 七节，并写明**这些值都是暂定的、各自由哪个阶段检验**。

标记为 `chosen` 的 17 项：`abi.*` 8 项、`capture.*` 5 项、`path.periods_in_flight`、`amp.registers` 的
`mute_on/mute_off/clock_settle_ms`。

门禁因此也改了，并且**门禁本身现在有测试**：

```text
PASS QcomAudioBindingGate: 6 cases
  a choice with an existing anchor passes
  a choice with no reason is refused
  a choice pointing at a missing document is refused
  a choice whose anchor is not in the document is refused
  a choice with no anchor at all is refused
  a fact claimed verified on an unmeasured source is refused
```

（上一轮的门禁支持 `--require-verified` 却没有参数解析，是直接读 `sys.argv`；这次补 `--binding` 与
`--skip-coverage` 才让它可被单独测试。）

binding 现状：**129 字段 · 0 verified · 17 chosen · 112 unverified**；
**38 项引用已记录事实 · 8 项 sheng 预测 · 17 项已记录选择 · 66 项仍无来源**（"无来源"从 83 降到 66，
而且剩下的 66 项现在真的都是"我们还不知道"）。

---

## G1.3 的 host 半：小消息与控制回复在回压下

```text
test_id: G1.3-host
result: PASS
device_commit: 8156808（CDC 子仓库） / 设备仓库 V2
classification: 200 条 32/64 字节固定消息 + 1 条控制回复，写进 16 KiB ring，再以 64 字节为单位排空：
               accepted 与写入总量精确相等；overflow = 0；high_water = 写入总量；
               控制回复完整出现在流尾、未被饿死也未乱序；两段消息的边界（第 100 条 32B 后转到 64B）逐字节可查
next_single_measurement: G1.3 真机（日志在写的同时，控制回复仍能回来）
```

子仓库门禁：`python3 -B tests/check.py --target-clang <clang> --output <dir>` → **31 项全部通过**。

**没做的事**：没有在真机上跑。G1.3 的验收要求"日志写入期间控制回复不被饿死"，那只能在手机上观察；host 侧能证明的
是队列语义与计数，不能替代它。

---

## S10 文件审计（第三部分）：拓扑与路由，以及一处我自己的错

来源：`work/patches/audio-bringup-20261029/` 的 `audio-fixes.patch`、`stereo-routing.patch`、
`fuxi-audio-tplg.sh`（同一仓库 `7f12106`）。

新记录的事实：

```text
AR DAI 索引   = 18        （拓扑 token1 = AR_TKN_DAI_INDEX，16 PRIMARY → 18 SECONDARY）
HW_IF_IDX     = 1         （I2S_INTF_TYPE_SECONDARY，i2s 模块 token250）
SD_LINE_IDX   = 2         （SD1 = GPIO124 i2s1_data1，token256）
设备号 device16           （logger1 → i2s_rx1 是被改道的播放通路）
声道顺序      = 设备树里第一个 codec 是左、第二个是右 → **T 左（0）、B 右（1）**
音量上限      = 816（0 dB），因为保护固件在跑，机器驱动把两路 PCM 音量封在 0 dB
```

### 我写错的一个数（已纠正）

上一轮我把 `SECONDARY_MI2S_RX` 记成 **0x18**，是从设备树里的节点名 `dai@18` 推的。这次读
`audio-fixes.patch` 才看清：它把节点名从 `dai@18` 改成 `dai@12`，而 `reg = <SECONDARY_MI2S_RX>` 不变——
**节点名是十六进制，0x12 = 十进制 18**，与拓扑脚本里那个十进制的 18 完全一致，本来没有矛盾。

正确值是 **0x12**，binding 的 `[interface.mi2s] port_id` 已改成 0x12，manifest 里也写下了这个十六进制/十进制的坑，
避免下次再读错。

binding 现状：**135 字段 · 0 verified · 17 chosen · 118 unverified**；
**44 项引用已记录事实（+6）· 8 项 sheng 预测 · 17 项已记录选择 · 66 项无来源**。

---

## 证据模型再清理："无来源"从 66 降到 37

三件事：

**1. 删掉四个不是数值的条目。** `path.references.graph/endpoint/endpoint_format/stream_format` 原本是
`{ value = "", evidence = "none" }`——它们是**结构体指针**，不是配置值。binding 只应该记录值；门禁本来就把引用字段
排除在覆盖之外，所以删掉不损失任何覆盖。

**2. 22 项"我们的选择"改成 `chosen`**，理由写进 `docs/fuxi-audio-abi-choices.md` 的新章节：

| 锚点 | 内容 |
| --- | --- |
| `timeouts` | 邮箱 1000 ms / ready 2000 ms / link 5000 ms、`timeout_steps = 100`（分开是为了让失败点自己说话） |
| `path-ring` | 65536 帧环、480 帧周期、2 声道、2 字节样本、48 kHz、24 MHz 时基（与 ABI 同一套数字） |
| `path-format` | PCM、16 位、2 声道、交织、4 字节/帧（先只做一种格式） |
| `path-buffer` | 64 KiB 缓冲；`buffer_address` **不在此列**（那是运行时映射决定的 IOVA） |
| `graph-performance` | 默认性能档、16 KiB 容器栈 |
| `amp-write-values` | 我们要写进寄存器的值（enable=1、pll=1、irq_mask=0、asp_slot=0），**S14 保持静音时逐项核对** |

**3. 3 项协议常量改标为 sheng 预测**（它们不是 SoC 事实，是 GPR/APM 协议参数）：
`gpr_port = 0x2001`（我们的客户端端口）、`token_seed = 1`、`graph.apm_iid = 1`（APM 模块实例）。
出处是 sheng 的 `ShengGpr.h` 与 `AudioProbe.cpp`，标注为**预测**而不是事实——它跑在另一块 SM8550 板上。

结果：

```text
131 字段 · 0 verified · 39 chosen · 92 unverified
44 项有已记录事实 · 11 项 sheng 预测 · 39 项已记录选择 · 37 项无来源
```

剩下 37 项的构成（都在等具体阶段的输入，不是含糊的"不知道"）：

- **29 项 AudioReach 拓扑/端点的标识**（graph_id、subgraph_id、container_*、module/link 计数、endpoint 的
  module/instance/line/slot_mask、topology 的模块 id 与 slot 掩码）——需要反编译 `Xiaomi-13-tplg.bin` 或
  AudioReach 的头文件才能确立；
- **3 项运行时值**（`amp.registers.device_id_value`、`revision_value`、`path.buffer_address`）——只能由真机给出；
- **5 项功放寄存器**（ASP 使能/控制、boost 电容、两个 EINT）——头文件里没有对应名字，等 A3 读芯片或换一种命名方式。

---

## S10 文件审计（第四部分）：AudioReach 常量与"由客户端分配"的编号

来源：主线 `sound/soc/qcom/qdsp6/audioreach.h`（`torvalds/linux@7475ea8`，curl 可取）。

**协议常量（有出处）**：

```text
MODULE_ID_WR_SHARED_MEM_EP = 0x07001000   播放流端点
MODULE_ID_RD_SHARED_MEM_EP = 0x07001001   采集流端点
MODULE_ID_I2S_SINK         = 0x0700100A   MI2S 输出端点
MODULE_ID_I2S_SOURCE       = 0x0700100B
MODULE_ID_GAIN             = 0x07001002
I2S_INTF_TYPE_PRIMARY      = 0        I2S_INTF_TYPE_SECONDARY = 1
PARAM_ID_I2S_INTF_CFG      = 0x08001019
AR_SG_INSTANCE_ID_START    = 0x4000   AR_CONT_INSTANCE_ID_START = 0x5000   AR_MODULE_INSTANCE_ID_START = 0x6000
```

**互相印证**：`I2S_INTF_TYPE_SECONDARY = 1` 正是 Linux 端口拓扑脚本里的 `token250（HW_IF_IDX）0 → 1`——
两条独立来源对上了，这条通路确实是 Secondary MI2S。

由此 `path.endpoint.module = 0x0700100A`、`path.endpoint.interface_type = 1`、
`path.topology.endpoint_module = 0x0700100A`、`path.topology.stream_module = 0x07001000` 四项有了出处；
`path.endpoint.line = 2` 来自拓扑脚本的 `SD_LINE_IDX`。

**12 项"由客户端分配"的编号改标 `chosen`**（新锚点 `ar-ids`）：graph/subgraph/container id、container 位置与域、
模块数与连接数、三个实例号（0x6001/0x6002/0x6003，落在头文件为模块保留的 0x6000 段内）。理由写在文档里：
第一条通路只求最短可达（流端点 → 增益 → I2S 汇），**A1 用"图被 DSP 接受并进入 prepared"来验证**，
被拒就改这里，而不是去猜一个"正确"常量。

结果：

```text
131 字段 · 0 verified · 51 chosen · 80 unverified
49 项有已记录事实 · 11 项 sheng 预测 · 51 项已记录选择 · 20 项无来源
```

**剩下 20 项，每一项都对应一个具体输入**：

| 项 | 数量 | 等什么 |
| --- | --- | --- |
| 协议枚举（`graph.direction`、`graph.scenario`、`topology.interface_word`、`slot_mask*`、`slot_count`、`endpoint.secondary/ws_source/interface_index`、`topology.shmem_module`） | 10 | AudioReach 枚举/拓扑反编译 |
| 运行时值（`device_id_value`、`revision_value`、`buffer_address`） | 3 | 真机 |
| 功放寄存器（ASP 使能/控制、boost 电容、两个 EINT） | 5 | A3 读芯片 / 换命名 |
| 待定 | 2 | — |

---

## H6 — 一次分配允许改动的字节

```text
test_id: H6
result: PASS
device_commit: 1dd259d（设备仓库 V2）
kernel_commit: 未参与（host 测试）
smem_before_checksum: 每个用例前逐字节快照
smem_after_checksum: 分配后逐字节比较，记录改动偏移的集合
unexpected_diff: 无（改动集合 = 条目头 16 字节 + 已发布 free 指针的 1 个非零字节）
classification: 22 项断言。分配一个 32 字节条目：只有条目头四个字与 free 指针变化，
               **载荷 32 字节原样（0x5a 未被清零）**；解析器立刻能找到它且 size 正确；
               5 字节的非对齐请求按 8 对齐、find 仍返回 5，且下一个条目落在补齐之后；
               exists / 零长度 / 超长三种拒绝之后**镜像逐字节不变**
next_single_measurement: G4（同一套断言，跑在从真机读回来的真实 snapshot 上）
```

门禁：`python3 scripts/check-qcom-smem-alloc-host.py`（ASan/UBSan）。

**顺带确认**：runbook §3.3 提到"当前 `qcom_smem_alloc()` 还会清零整个 payload"——读代码后确认**这一点已经不成立**：
分配器只写条目元数据，注释也写明"The allocator owns the entry metadata, not the payload"。这次用字节差异把它钉住，
而不是只在注释里声明。**发布顺序**（先写条目头、再发布 free 指针）由"拒绝时指针不动"与"分配后指针等于条目末尾"两条间接证明。

---

## G2–G5 的单步目录，以及"写"的构建期门闸

```text
test_id: G2-ladder-catalogue
result: PASS
device_commit: ac117b9（CDC 子仓库）
classification: 13 个固定 step id（1..13）。目录被断言为：连续、名字唯一、恰好两步标为写入
               （hwlock-cycle 与 smem-allocate）；只读阶梯（read-toc / read-partition / read-item / unmap /
               hwlock-identify）全部不写；未 ARM 时不能入队；已入队时不能再入队另一个。
               同一份测试**编译两次**：未定义 SXNU_DIAG_ALLOW_WRITE_STEPS 时两个写步骤返回 REFUSED、
               只读步骤仍可用；定义了则该写步骤可入队
next_single_measurement: G2.1（真机，descriptor-create → descriptor-release）
```

子仓库门禁：**35 项全部通过**（此前 31 项；新增两种 SoC profile × 带/不带写门闸）。

**要说清楚的一点**：目录已经列出了 G2–G5 的步骤，但**传输层目前仍然只执行 NOOP**。也就是说这些 step id 现在
是"被登记、被测试、但还没有接入真机动作"。这是有意的顺序：先让"能请求什么"和"什么需要更高授权"变成编译期事实，
再逐步把每个 step 接到映射/读取/锁上。**在 S0 与 G2.1 之前，没有任何一个 step 会在真机上跑。**

写门闸的形式：`#if !defined(SXNU_DIAG_ALLOW_WRITE_STEPS) → 写入步骤一律 SXNU_DIAG_REFUSED`。
默认镜像里 `hwlock-cycle` 与 `smem-allocate` **永远拿不到**，不是靠 host 自觉。

---

## 目录的诚实规则：未实现的步骤不能"假成功"

上一轮目录列出了 13 个步骤，而 worker 只实现 NOOP。两者之间有一条会说谎的路径：**阶梯推进到未实现的步骤时，
worker 仍跑 NOOP 并回报成功**——host 于是会记录一次从未发生的 descriptor 创建。

修法：每个步骤增加 `implemented` 标志（当前只有 NOOP 为真），`sxnu_diag_next_id()` 对未实现的步骤返回
`SXNU_DIAG_REFUSED`——**与写门闸无关，开了写门闸也照样拒绝**。

```text
test_id: G2-ladder-honesty
result: PASS
device_commit: 77d8d3a / bec8d12（CDC 子仓库）
classification: 恰好 1 个步骤 implemented、恰好 2 个步骤 writes；未实现的只读步骤被拒（且状态留在 ARMED）；
               开了 SXNU_DIAG_ALLOW_WRITE_STEPS 之后，hwlock-cycle 依旧因未实现被拒；
               阶梯函数从起点走遍全部编译步骤各一次并在末尾停住、不回绕（999 → NONE）
next_single_measurement: 把阶梯接进传输层（需要设备确认第 2 步的行为）
```

**阶梯尚未接入传输层**，这是刻意的：接入后第一个会被拒绝的是第 2 步，在没有设备确认的情况下接入，等于凭空改变
手机上会发生的事。文档（`drivers/QcomXnuUsbDebug/docs/control-and-screenshot.md`）已写明这条理由。

子仓库门禁：**35 项全部通过**。

---

## 阶梯接进传输层，并用真实内核构建验证

`DIAGNEXT` 现在驱动阶梯：每次请求入队**当前**这一步；**完成**才推进游标；**失败或超时**不推进（下次仍请求同一步，
不会悄悄跳过失败）；`DIAGCANCEL` 与 `DIAGRESET` 把游标退回第一步（重置是重新开始，不是继续）。

阶梯逻辑收进两个小助手（`diag_queue_next` / `diag_ladder_restart`），因为 host 侧单独编译控制处理器时，
只需替这两个符号，而不是同时替目录、状态机与游标。

```text
test_id: G2-ladder-wired
result: PASS
device_commit: 4697b05（CDC 子仓库）
kernel_commit: 5347093661305e1c053ca06970034cac215a1da9301915576f0643123cb87c45
classification: 子仓库 35 项主机检查通过；内核完整构建通过（说明改动在真实 XNU 环境里成立，
              且模块适配器对钉住 revision 的适配锚点仍然匹配）
next_single_measurement: G2.1 真机（第一次 DIAGNEXT 走 NOOP，第二次应收到 REFUSED，因为 noop-thread 尚未实现）
```

**构建期间发现的一件事**：`scripts/prepare-fuxi-usb-debug.py` 把子仓库钉在**确切 revision**（`QUD_REVISION`），
所以任何子仓库提交都需要同步这个钉子。这次已更新为 `b20537b`。这个钉子是好设计：适配器只对已知源码生效，
换源码而不改钉子会被直接拒绝（`unexpected module/XNU source revision`），比默默打补丁安全。

---

## S10 收尾：无来源从 66 降到 11

**4 个功放寄存器找到出处**（主线 `cs35l41.h` / `cs35l41-lib.c`）：

| 字段 | 主线寄存器 | 地址 | 说明 |
| --- | --- | --- | --- |
| `reg_asp_ctrl` | `CS35L41_ASP_CONTROL4` | 0x00004854 | 头文件里唯一的 ASP 控制寄存器 |
| `reg_boost_cap` | `CS35L41_BSTCVRT_COEFF` | 0x00003810 | **boost 电容是通过变换器系数配置的**，没有独立寄存器（驱动默认 0x00002424） |
| `reg_irq1_eint1` | `CS35L41_IRQ1_STATUS1` | 0x00010010 | 驱动用它轮询上电/下电，并写它清事件 |
| `reg_irq1_eint2` | `CS35L41_IRQ1_STATUS2` | 0x00010014 | 第二个状态字 |

**1 个明确不存在**：`reg_asp_enables`——本项目设想的"独立 ASP 使能寄存器"在这颗芯片上并不存在，
使能位在 `ASP_CONTROL4` 内部。字段先保留，等使用它的代码决定是否合并/删除；理由写进 manifest 的 `unnamed`。

**5 项改标 `chosen`**（新锚点 `slot-and-rate`）：`topology.sample_rate = 48000`、`slot_count = 2`、
`slot_mask_a = 0x1`（T 左、时隙 0）、`slot_mask_b = 0x2`（B 右、时隙 1）、`endpoint.secondary = 1`
（接口类型已是 secondary，不是可选风格）。时隙顺序与 `stereo-routing.patch` 的声道顺序一致。

最终：

```text
131 字段 · 0 verified · 56 chosen · 75 unverified
53 项有已记录事实 · 11 项 sheng 预测 · 56 项已记录选择 · 11 项无来源
```

剩下 11 项，全部等具体输入：`graph.direction/scenario`、`topology.interface_word`、`endpoint.ws_source/
slot_mask/interface_index`、`topology.shmem_module`（协议枚举与拓扑细节）；`device_id_value`、`revision_value`、
`buffer_address`（只能由真机给出）；`reg_asp_enables`（该芯片没有这个寄存器）。

---

## S10 完成：无来源只剩 6 项

又确立 6 项：

| 项 | 值 | 出处 |
| --- | --- | --- |
| `path.graph.scenario` | 1 | `APM_SUB_GRAPH_SID_AUDIO_PLAYBACK = 0x1`（`audioreach.h`） |
| `path.graph.direction` | 0 | `q6apm.c` 用 `cfg->direction == SNDRV_PCM_STREAM_CAPTURE`(1) 判别，播放即 0 |
| `path.topology.shmem_module` | 0x07001000 | 播放流的共享内存端点就是 `MODULE_ID_WR_SHARED_MEM_EP` |
| `path.endpoint.interface_index` | 0 | chosen：该接口类型下的第 0 个实例 |
| `path.endpoint.slot_mask` | 0x3 | chosen：这条端点承载两个时隙（见 `slot-and-rate`） |
| （另）`apm_sg_sid_audio_record = 0x2` | — | 采集通路以后要用 |

最终：

```text
131 字段 · 0 verified · 58 chosen · 73 unverified
56 项有已记录事实 · 11 项 sheng 预测 · 58 项已记录选择 · 6 项无来源
```

**剩下 6 项，全部要么是"还没确立的编码"，要么是"只能由真机给出"，要么是"这颗芯片没有"**：

| 项 | 为什么还没有 |
| --- | --- |
| `endpoint.ws_source`、`topology.interface_word` | 编码方式未确立（需要 I2S 配置结构或拓扑反编译） |
| `device_id_value`、`revision_value` | 只能读芯片（A3） |
| `buffer_address` | 运行时映射决定的 IOVA（A2） |
| `reg_asp_enables` | 该芯片没有这个寄存器，等代码决定是否删 |

本机没有 `alsatplg`，所以拓扑反编译这条暂时走不通——这也意味着那 7 个协议枚举里剩下的 2 个编码，
短期内只能靠 AudioReach 的其它头文件，而不是靠反编译产物。


### 追加：`ws_source` 的来源，以及一次独立印证

`audioreach.h` 里就有 I2S 接口配置的完整定义：

```c
struct param_id_i2s_intf_cfg { uint32_t lpaif_type; uint32_t intf_idx; uint16_t sd_line_idx; uint16_t ws_src; };
#define CONFIG_I2S_WS_SRC_EXTERNAL 0x0
#define CONFIG_I2S_WS_SRC_INTERNAL 0x1
#define I2S_SD0 1     #define I2S_SD1 2
```

- `ws_source = 1`（INTERNAL）：Linux 端口的注释写明"LPASS 做主，BCLK/WS 输出"，功放跟随 BCLK，所以字选择由接口自己产生；
- **`I2S_SD1 = 2` 与拓扑脚本的 `SD_LINE_IDX = 2` 完全一致**——这是本轮第二次"两个独立来源对上"（上一次是
  `I2S_INTF_TYPE_SECONDARY = 1` 与 `HW_IF_IDX 0 → 1`）。同一个数字被两个不同来源确认，比任一单独来源可信。

`endpoint.interface_type/index/line/ws_source` 四项正好对应这个结构的 `lpaif_type/intf_idx/sd_line_idx/ws_src`——
我们的绑定结构与协议结构是一一对应的，这一点以前只是"看起来像"，现在有定义了。

无来源：**5 项**（`interface_word`、`reg_asp_enables`、`device_id_value`、`revision_value`、`buffer_address`）。

---

## 设备状态探测与 S0 的单次启动脚本

设备有**两条**被访问的通道，由所有者的按键决定走哪条，所以每轮都应该先问而不是假设：

```bash
scripts/fuxi-audio-device.sh
#   state=fastboot serial=<id>        退出码 0
#   state=serial run=... captured=... 退出码 1（能 PING 通，说明有 qcom-usb-debug 的内核在跑）
#   state=absent last_capture=...     退出码 2（关机、或在别的系统里）
```

本轮实测：**state=absent**（不在 fastboot，串口控制通道也拒绝连接，最新捕获是旧的）。

同时把 **S0 的一次启动**做成可重复执行的脚本：`scripts/fuxi-audio-s0.sh`。它做一次启动并验证：

1. 设备必须在 fastboot，否则直接拒绝（退出码 1，带当前状态）；
2. 用**正确的 Data seed**（`out/fuxi/build/owner-data-fixture/owner-data-16g.dbr`）构建 payload 与镜像；
3. **校验 payload 里装的就是刚构建的内核**（sha256 相等），并校验 `board.toml` 不含任何 `fuxi-audio-step`/`fuxi-audio-probe`；
4. 启动后**等控制通道应答**（最多 90 s，不是固定 sleep）；
5. 记录 kernel sha、image sha、seed、控制通道应答、捕获路径与"音频行数"到 `facts/audio/s0-boots.txt`；
6. 若控制通道无应答或捕获里出现任何音频行 → **FAIL**。

S0 的验收是**三次**这样的记录，中间需要所有者按键让手机回到 fastboot——脚本只做一次，因为这个动作我不能替你做。
