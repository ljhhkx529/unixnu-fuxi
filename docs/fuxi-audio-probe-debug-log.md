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
