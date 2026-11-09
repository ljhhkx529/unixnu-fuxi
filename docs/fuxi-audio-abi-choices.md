# fuxi 音频：我们自己选的数值，以及为什么

binding 里的数值有三种性质，原来只有两种被表达出来（`verified` / `unverified`）：

1. **板上事实**——可以去查、去量（功放地址、寄存器、GPIO、DAI 端口）。出处是文档或设备。
2. **运行时读数**——只能在真机上读出来（芯片 DEVID、REVID）。
3. **我们自己的选择**——ABI 的帧长、通道数、超时、静音极性。它们没有"出处"，只有**理由**；
   一旦某阶段验证过，就可以升级为 `verified`。

这份文档就是第 3 类的理由锚点。binding 里 `state = "chosen"` 的条目，`evidence` 指向本文的某一节
（`design:docs/fuxi-audio-abi-choices.md#<anchor>`），门禁会检查锚点确实存在。

**这些值都是暂定的**：它们让 host 侧的层可以带着真实尺寸运行，并且每一条都会在它所属的阶段被真机检验。
任何一条被真机推翻的，就改这一条，而不是改理由。

## abi-rate

48000 Hz（暂定）。理由：两颗功放与 DSP 的扬声器通路都以 48 kHz 为主；Linux 端口的拓扑也是 48 kHz。
先只支持一个采样率；需要 44.1 kHz 时做重采样，而不是让 ABI 变复杂。

## abi-channels

输出 2 声道、输入 2 声道（暂定）。两颗 CS35L41 构成左右（B 右、T 左）；WCD938x 的采集最少两声道。
单声道路径（第一声）用同一对声道，另一路留静音。

## abi-period

`period_frames = 480`（48 kHz 下 10 ms），`ring_frames = 4800`（100 ms，10 个 period），
`lead_frames = 960`（20 ms 提前量）。理由：10 ms 周期是 DSP 的常规粒度，A1 的 ring 测试已经证明
480 帧周期在 65536 帧环里是合法的；20 ms 提前量让第一次写在读指针之前落地。

## abi-host-clock

`host_clock_hz = 24000000`、`anchor_host = 0`（暂定）。24 MHz 是 Apple 平台 `mach_absolute_time` 的常用时基，
0 表示"在第一次写入时确定锚点"而不是写死一个绝对时间。A4 会用真实时间戳检验这个映射，届时若时基不是 24 MHz，
改这一条即可。

## capture

`channels = 2`、`bit_width = 16`、`period_frames = 480`、`sample_rate = 48000`、`expected_periods = 200`（暂定）。
理由：200 个 period 是计划里给第一支麦克风的验收数字；16 位是 WCD938x 采集的最小公共格式，
24 位打包在板上还没确认，所以 `QcomAudioCapture` 明确拒绝它而不是猜。

## path-periods-in-flight

`periods_in_flight = 2`（暂定）。双缓冲让写入与读取不撞在同一 period 上；A2 用 1000 个 period 检验它。

## amp-mute-polarity

`mute_on = 1`、`mute_off = 0`、`clock_settle_ms = 10`（暂定）。按"写 1 置静音、写 0 解除"的常见极性选，
10 ms 让 PLL/时钟在解除静音前稳定。S14 会先做到"muted-ready"（保持静音），因此这个极性在出声之前就会被验证。

## timeouts

`adsp.timeouts`：邮箱 1000 ms、ready 2000 ms、link 5000 ms，`path.timeout_steps = 100`（暂定）。
理由：DSP 冷启动到 ready 通常在一秒量级，链路建立更慢；三个值分开是为了让失败点自己说话——
"卡在邮箱"和"卡在 link"是不同的问题。全部有上界，等待循环不允许无界。

## path-ring

`path.ring`：`frames = 65536`、`period_frames = 480`、`channels = 2`、`bytes_per_sample = 2`、
`sample_rate = 48000`、`host_clock_hz = 24000000`（暂定）。理由：65536 帧环是 A1 主机测试已经覆盖的形状，
480 帧周期（10 ms）与 ABI 一致，16 位样本对应计划里的第一声；环与 ABI 用同一套数字，避免两侧对不上。

## path-format

`path.format`：`media = 0`（PCM）、`bit_width = 16`、`channels = 2`、`interleaving = 0`（交织）、
`bytes_per_frame = 4`（暂定）。理由：先只做一种格式，A2 用 1000 个 period 验证；24 位打包仍未确立，不做。

## path-buffer

`path.buffer_bytes = 0x10000`（64 KiB，暂定）。理由：两个 period 的 48 kHz 立体声 16 位数据不到 4 KiB，
64 KiB 留足了拓扑与控制结构的余量。`buffer_address` **不在这里**——那是运行时由映射决定的 IOVA，不是常量。

## graph-performance

`path.graph.performance = 0`（默认档）、`container_stack_bytes = 0x4000`（暂定）。理由：先按默认性能档开通路，
容器栈大小是我们在 DSP 侧申请的字节数，16 KiB 足够容纳一条 MI2S 通路的模块。

## amp-write-values

`amp.registers` 里"我们要写进去的值"：`enable_value = 1`、`pll_value = 1`、`irq_mask_value = 0`、
`asp_slot_value = 0`（暂定）。理由：1 表示使能/置位，0 表示解除屏蔽，这些都是常见极性；
**S14 会在保持静音的前提下逐项核对**，所以极性在出声之前就会被验证，而不是靠猜。

（`device_id_value`、`revision_value` 不在此列：那是芯片读回来的值，只能由 A3 的真机读取确立。）

## ar-ids

AudioReach 的图、子图、容器与模块实例编号**由客户端分配**，不是板上常量——主线头文件只规定取值范围
（`AR_SG_INSTANCE_ID_START = 0x4000`、`AR_CONT_INSTANCE_ID_START = 0x5000`、
`AR_MODULE_INSTANCE_ID_START = 0x6000`，各占 0x1000）。因此这些数字是我们的选择：

```text
graph_id = 1 · subgraph_id = 1 · container_id = 1 · container_position = 0 · container_domain = 0（ADSP）
module_count = 3 · link_count = 2     （写共享内存端点 → 增益 → I2S 汇）
stream_instance = 0x6001 · endpoint_instance = 0x6002 · shmem_instance = 0x6003
```

理由：第一条通路只求最短可达——一个流端点、一个增益、一个 I2S 输出，两个连接；实例号落在头文件给模块保留的
`0x6000` 段内且互不重复。**A1 会用"图被 DSP 接受并进入 prepared 状态"来验证这些编号可行**；
若 DSP 拒绝，改的是这里，而不是去猜一个"正确"的常量。

（`graph.direction`、`graph.scenario` 不在此列：它们是协议枚举，需要头文件里的定义，不是可自由分配的数字。）
