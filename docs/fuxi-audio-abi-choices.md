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
