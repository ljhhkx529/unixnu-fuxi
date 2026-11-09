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
