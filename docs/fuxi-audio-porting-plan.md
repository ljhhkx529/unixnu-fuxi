# Xiaomi 13（fuxi / SM8550）音频移植方案

状态：实施中；已完成静态证据审计和板级无关 GPR 主机模型，尚未接入内核或进行设备测试

目标系统：UniXNU + iPadOS 17.7.11（21H461）

参考实现：`UniXNU-sheng` 的 ADSP、GPR、AudioReach、音频服务和 CoreAudio HAL

目标硬件：Xiaomi 13（fuxi，Qualcomm SM8550）

## 1. 结论与推荐路线

sheng 的方案对 fuxi 很有价值，但只能复用“分层架构和 SM8550 通用机制”，不能复制板级音频参数。

推荐链路是：

```text
fuxi ADSP firmware
  -> SMEM / SMP2P / IPCC / GLINK
  -> GPR channel adsp_apps
  -> AudioReach / APM graph
  -> PRM + LPASS macro clocks
  -> secondary TDM/MI2S -> 2 x CS35L41 -> speaker/receiver
  -> TX SoundWire -> WCD938x -> microphones
  -> kernel audio service + shared rings
  -> fuxi CoreAudio HAL plug-in
  -> iPadOS applications
```

与 sheng 相比，最重要的修正是：

1. fuxi 的公开 Android 下游 DTS 线索是 **2 颗 CS35L41**，不是 sheng 的 6 颗 CS35L43。CS35L43 的寄存器表、reset 拓扑和 slot 分配均不得照抄。
2. 两台设备都使用 GPIO121--124 一组 secondary 音频引脚，但 fuxi DTS 把它声明为最多 4 slot 的 TDM 接口。实际 frame sync、slot width、slot mask、左右声道映射必须由 UniXNU 观测或波形确认。
3. 两台设备都处于 WCD938x + TX SoundWire 的 SM8550 音频体系中，但这并不能证明主麦路径相同。sheng 已确认的 AMIC3/ADC2/port 2/TX3 只能作为查找模板。
4. sheng 的 AudioReach、共享环、19.2 MHz timeline 和 CoreAudio object model 可以作为移植起点；sheng 的 iPadOS 26/27 arm64e 细节不能直接当作 fuxi 的 21H461 ABI 事实。
5. fuxi stock DTS 存在 CS35L41 `fast-switch` profile。首声可暂时采用低功率、短时的安全固定配置，但日用音频不能以“永久绕过功放 DSP/保护”为完成状态。

因此，实施顺序应是“先控制面、再静音数据面、再低音量扬声器、最后 CoreAudio 和麦克风”，而不是先复制 sheng 功放驱动。

## 2. 证据等级与当前基线

所有音频值必须带证据等级：

| 等级 | 含义 | 可否进入 fuxi 板级配置 |
| --- | --- | --- |
| `confirmed-device` | 来自 fuxi 在 UniXNU 下的总线读回、波形或其他直接测量 | 可以 |
| `confirmed-source` | 来自固定 commit 的 fuxi 源码，且与目标硬件明确匹配 | 可以，但关键电气值仍应真机复核 |
| `reference-sheng` | 来自 sheng 的已验证实现 | 只能指导架构和调查方法 |
| `hypothesis` | 来自同 SoC、其他 Xiaomi 机型或未验证静态 DTS | 不可以，只能生成下一项测量 |

当前 fuxi 项目事实：

- `porting.toml` 中 `audio` 仍是 `planned`，没有选择音频组件。
- `dts/fuxi.dts` 尚未描述 ADSP 或板级音频设备。
- 当前没有 fuxi 真机音频事实集，也没有 fuxi 音频设备测试结果；项目没有可供采集的 Android boot/runtime。
- `docs/dtb_diff_report.txt` 中的 `audio_cma_region`、ADSP 和 SMMU 差异只能提示调查方向，不能据此确定地址、IRQ 或路由。

当前 fuxi 硬件线索来自 LineageOS `android_kernel_xiaomi_sm8550-devicetrees` commit
`43b5ada5654ed84c25f74789bc8f6326b7bacd20`。这些值在 UniXNU 只读探针或直接测量复核前仍按
`confirmed-source` 或 `hypothesis` 管理：

- 扬声器功放：2 x CS35L41，I2C 地址 `0x40` 和 `0x42`。
- 控制总线：`qupv3_se8_i2c`；SM8550 公共定义的 MMIO base 是 `0x00880000`。
- amp 0：reset GPIO183、IRQ GPIO99、prefix `B`、标记 `right-channel-amp`。
- amp 1：reset GPIO182、IRQ GPIO98、prefix `T`，另有 GPIO68 的 receiver high/low pinctrl。
- boost 参数线索：4000 mA、15 uF、1000 nH；这些是芯片配置输入，不是允许直接满功率运行的授权。
- stock profile 名线索：`B-music.txt`、`B-handfree.txt`、`T-music.txt`、`T-handset.txt`、`T-handfree.txt`。
- speaker 数字链路：secondary TDM/MI2S，GPIO121 clock、GPIO123 word select、GPIO122 data input、GPIO124 data output，最多 4 slots。
- WSA macro 和 WSA SoundWire 功放被禁用，`qcom,wsa-max-devs = 0`。
- 麦克风 codec 线索是 WCD938x，TX SoundWire master MMIO `0x06d30000`；micbias1--4 均声明 2750 mV。
- DTS 声明 AMIC1--5、DMIC0--3 等候选 routing，但没有证明用户实际使用的主麦、听筒麦或降噪麦路径。

## 3. sheng 与 fuxi 的差异

| 层 | sheng 已验证状态 | fuxi 当前结论 | 处理方式 |
| --- | --- | --- | --- |
| SoC | SM8550 | SM8550 | 通用 transport/协议优先抽到公共层 |
| ADSP 固件 | 从 `modem_a` FAT `/image` 读取 `adsp`、`adsp_dtb` | 来源、版本、分段和 carveout 未确认；不得假设存在 Android boot | 复用 loader 结构，不复制 manifest |
| IPC | SMEM/SMP2P/IPCC/GLINK 已工作 | 理论上同族，所有 ID/IRQ/地址待确认 | 参数化公共实现 |
| GPR | `adsp_apps` 已工作 | channel 和 intent 行为待确认 | 先做只读 discovery/echo |
| AudioReach | APM graph、PRM vote、共享内存已工作 | 模块/实例/endpoint ID 待确认 | 复用 builder，板级 graph 由事实生成 |
| 扬声器功放 | 6 x CS35L43 | 2 x CS35L41 | 新建 CS35L41 实现，禁止复用寄存器表 |
| 功放总线 | 两条 I2C，总地址 0x40--0x42 | QUP2 SE0，0x40/0x42 | 用 fuxi 总线、GPIO 和时序 |
| reset | 6 颗共享 GPIO90 | GPIO183、GPIO182 独立 | 独立状态机，单颗故障可隔离 |
| IRQ | sheng 板级配置 | GPIO99、GPIO98 线索 | 确认极性/触发方式并读取 sticky fault |
| 数字音频 | secondary MI2S，48 kHz/16-bit，SoC master | secondary TDM/MI2S，最多 4 slots | 实测 BCLK/FS/slot/word width |
| slot | top 三颗 slot 0、bottom 三颗 slot 1 | B/T 映射未知 | 分别单开功放并验证声道 |
| 功放 DSP | bypass，无功放 firmware | stock 有 fast-switch profile | bypass 只允许低功率 bring-up；日用前补保护/校准 |
| 麦克风 codec | WCD938x | WCD938x 线索 | codec transport 可参考，route 必须由 UniXNU 探针或测量确定 |
| 主麦 route | AMIC3 -> bias3 -> ADC2 -> SWR -> TX3 | 未知 | 从 mixer diff、DAPM 和 capture 实测获得 |
| 音频 DMA | 低于 4 GiB，sheng 默认 `0xd0000000` | 可用低地址空洞未知 | 重新做内存地图和所有权审计 |
| SMMU | translating context 会 reset，使用 pass-through | 未验证 | 从主线语义、固定源码和 UniXNU 观测确定 ownership，不能假定与 sheng 相同 |
| CoreAudio HAL | stereo output + mono input | 可复用 ABI 设计 | 重审 21H461 ABI、bundle identity 和签名 |
| timeline | 19.2 MHz host timer，修正 CoreAudio 24 MHz 假设 | SM8550 很可能相同，仍需测量 | 在 fuxi 上用真实计数器验证 |

## 4. 完成等级

报告必须区分以下等级。读取到 I2C 芯片 ID 不等于音频可用；听到一次 test tone 也不等于 CoreAudio 可用。

| 等级 | 定义 | 最低证据 |
| --- | --- | --- |
| A0：ADSP 在线 | ADSP 认证、启动、ready、stop/SSR 可控 | 20 次启动/停止、crash reason、无 AP panic |
| A1：控制面在线 | GLINK、GPR、APM/PRM 事务可用 | channel/intent、事务统计、超时恢复 |
| A2：静音数据面 | shared memory map/unmap 和 DSP period 完整兑现，不开功放 | period fence、buffer hash/offset、无越界和 fault |
| A3：安全扬声器 | 两颗功放分别可控并播放短、低音量 test tone | PLL/fault、声道、波形、温度、100 次启停 |
| A4：iPadOS 输出 | CoreAudio stereo output 可被系统应用稳定使用 | 系统音效/音乐/暂停恢复、10 分钟无 underrun |
| A5：麦克风输入 | 目标麦克风 route 和 CoreAudio input 可用 | 200/200 period、非零率/peak、Voice Memos 启停 |
| A6：日用级 | 保护、校准、路由、休眠、SSR 和长稳通过 | 1 小时播放、30 分钟录音、1000 次启停 |

## 5. 安全边界

1. 第一声只允许短时、低音量单频 tone；默认功放保持 reset 或 global mute。
2. 禁止把 sheng 的 CS35L43 register table 写入 CS35L41。
3. 未确认 boost、speaker impedance、thermal/protection 和 calibration 前，不做高增益、长时间或低频满幅测试。
4. 两颗功放必须可独立 reset/mute。任意 I2C、PLL、over-temperature、boost 或 short fault 立即关闭对应功放。
5. receiver GPIO68 的含义未由真机确认前不切换；它可能改变听筒/上扬声器电气路径。
6. 麦克风 bring-up 先只输出 period 数、非零率、RMS/peak，不保存私人录音；需要保存时必须由设备所有者明确选择。
7. 音频 carveout、DMA 和 SMMU 配置必须先与 GPU、显示、UFS、USB 的地址窗口核对，不能借用 sheng 的低地址。
8. 当前禁止一切 boot、fastboot、ADB、刷写和其他设备操作；只有设备所有者后续重新明确授权，才可设计独立的 UniXNU 测试步骤。

## 6. 代码所有权与上游更新策略

为了避免今后跟随 UniXNU 上游时反复解决板级冲突，建议按以下边界拆分。

应进入 UniXNU/SM8550 公共层：

- MDT/PAS ADSP loader 的通用状态机和边界检查。
- SMEM、SMP2P、IPCC、GLINK transport。
- GPR 编解码、事务表、超时/取消和 SSR 代际管理。
- AudioReach/APM graph builder、PRM 命令、共享内存 mapper。
- 通用的音频服务、共享 ring 和 CoreAudio HAL core。
- 通用 GENI I2C transport。
- 通用 CS35L41 register/状态机驱动；具体 wiring 和 profile 不进入通用驱动。

只留在 `unixnu-fuxi`：

- ADSP firmware manifest/hash 和提取来源。
- carveout、MMIO、IRQ、GPIO、clock、rail、SMMU SID 和 memory attribute。
- CS35L41 数量、地址、reset/IRQ、B/T 角色、slot 和 profile 选择。
- TDM pins、frame format、slot mask 和 master/slave 策略。
- WCD938x 的 fuxi mic routing、bias、ADC、SoundWire port/channel、DMA endpoint。
- HAL 的 product identity、允许的最大音量和设备策略。
- `facts/audio/`、采集脚本、脱敏日志和验收记录。

sheng 中以 `Sheng*` 命名且包含板级常量的文件不应整体复制。先把其中 transport/协议逻辑抽成无板级值的接口，再由 fuxi binding 提供参数。若暂时不能上游化，可保持小型适配层，但不得在公共协议代码里加入 `if (fuxi)` 分支。

## 7. 分阶段实施计划

### Phase 0：冻结范围、事实格式和故障回退

目标：首期只承诺 stereo speaker output 和一个主麦输入，不承诺听筒、通话、USB-C 模拟耳机、蓝牙或多麦算法。

工作项：

- 建立 `facts/audio/manifest.toml`，每项记录 value、unit、source、stock build、采集日期、hash、可信等级。
- 建立 ADSP firmware manifest，只记录原分区/路径、大小、版本和 SHA-256，不提交不可分发固件。
- 建立日志 tag：`[adsp]`、`[audio-glink]`、`[gpr]`、`[apm]`、`[audio-buffer]`、`[amps]`、`[mic]`、`[audio-service]`、`[audio-io]`、`[audio-clock]`。
- 所有阶段有 boot arg 或配置开关，默认关闭。控制面、speaker、mic、HAL 应可分别禁用。
- 规定失败回退：停止新 graph、mute amp、unmap buffer、关闭 clocks、释放 channel；失败不能阻塞启动。

交付物：事实模板、firmware manifest、风险登记表、日志 schema、回退检查单。

验收：任何拟写入代码的地址、GPIO、IRQ、SID、clock ID 或 route 都能追溯到 fuxi 证据；未知值保持未知。

### Phase 1：建立无 Android runtime 的证据基线

目标：只用固定 fuxi 源码、许可证兼容的芯片资料、Linux 主线语义和可合法取得的离线固件清单建立边界，不凭 sheng 猜参数，也不假设存在 Android boot。

#### 1.1 当前可执行的静态审计

- 固定并逐项引用 fuxi DTS/DTSI commit，记录 I2C、GPIO、TDM、WCD/SoundWire 和候选 route；源码声明与运行事实分栏保存。
- 阅读 Linux 主线的 remoteproc、GLINK、GPR/APR、APM/PRM、WCD938x 和 CS35L41 状态机，只提取协议语义和安全约束，不复制 GPL 实现、OTP map 或 revision patch table。
- 对用户以后提供的、可合法使用的离线固件或配置只记录文件来源、大小、版本、SHA-256 和许可证；没有文件就保持 `unverified`。
- 对 carveout、SMEM/SMP2P、IRQ、SID、endpoint、clock 和 graph ID 建立待证表；不得从 sheng 或其他 SM8550 设备回填。
- 为 GPR、AudioReach builder/parser、超时和 SSR generation 建立 host sanitizer 测试，这些测试只证明编码和状态机，不证明 fuxi runtime。

#### 1.2 后续设备测量入口（当前不执行）

若设备所有者以后明确授权设备测试，另行编写 UniXNU 只读探针；不得依赖 Android、ADB 或所谓 stock boot。探针应按风险递增采集：

1. I2C 只读 chip ID/revision/fault；功放保持 reset 或 global mute。
2. ADSP/SMEM/SMP2P/GLINK discovery 与错误码，不创建 speaker graph。
3. SoundWire enumeration、clock/rail 状态和共享内存边界。
4. 逻辑分析仪观察 GPIO121/123/122/124 的 BCLK/FS/DATA，确认 slot/word width/master。
5. 只有安全门全部满足后，才进入后续低音量扬声器和麦克风阶段。

#### 1.3 产出

建议目录：

```text
facts/audio/
  manifest.toml
  source-revisions.toml
  adsp-firmware.sha256
  topology.md
  unverified-fields.toml
  mainline-audit.md
  unixnu-probes/
  tdm-waveform/
```

验收：每个已经使用的板级值都有 fuxi 来源；每个缺失的 runtime 值仍明确标为 `unverified`；主机测试与设备事实严格分开。

停止条件：固定源码互相冲突但未解释；固件/profile/calibration 的来源或许可证不清楚；实现需要未经确认的 endpoint/clock/slot/SMMU 常量。

### Phase 2：ADSP 固件、SMEM 和 GLINK（A0）

目标：只让 ADSP 可控地在线，不创建音频 graph、不碰功放。

实施包：

1. 从 fuxi manifest 读取 `adsp`/`adsp_dtb`，校验 hash、分段范围、对齐和 carveout。
2. 按许可证兼容资料和 fuxi 实测确认的顺序完成 PAS authenticate/load/start；保留每一个真实错误码。
3. 建立 SMEM、SMP2P 和 IPCC；确认 ready、stop-ack、fatal、watchdog 和 crash reason。
4. 建立 GLINK edge，观察 channel negotiation；对 `adsp_apps` 的 channel open、intent request/grant、close 做统计。
5. ADSP SSR 时使旧 channel、旧 transaction 和旧 buffer generation 全部失效，然后重建。

sheng 可以复用的结构：`AdspProbe.cpp`、`QcomMdt.*`、`QcomSmem.*`、`QcomSmp2p.*`、`QcomGlink.*` 的状态机和测试思路。

不得复制：sheng carveout、IPCC/IRQ/SMEM ID、固件路径假设、RPMh vote、错误超时常量，除非 fuxi 证据逐项吻合。

验收：连续 20 次 start/stop 成功；ready/SSR 可观测；无 AP panic、内存越界、UFS/USB/GPU/显示回退。

### Phase 3：GPR、APM 和静音数据面（A1--A2）

目标：先证明 AudioReach 控制面和 period 数据面可靠，功放始终 mute/reset。

实施顺序：

1. 在 `adsp_apps` 上完成 GPR service/port discovery，验证 request/response、token、timeout、cancel 和乱序响应。
2. 查询 APM framework 状态，解析 capability/version；未知版本不得静默套用 sheng graph。
3. 从 stock 证据建立 PRM clock、LPASS macro/digital codec vote，逐项记录 vote 前后可访问寄存器。
4. 实现 graph 的 open -> prepare -> start -> stop -> close 状态机；每一步可重复且失败可逆。
5. 找到 fuxi 可用的低于 4 GiB 共享内存区域，确认所有权、cache attribute、DMA 地址宽度和 SMMU 模式。
6. 先发送静音或 DSP 内部 loopback period；校验每个 buffer 的 PA/offset/length、提交序号、返回字节数和内容 hash。
7. 做 map/unmap、ring wrap、短 period、timeout、SSR 中断提交和 client crash 的错误注入。

可暂时使用 sheng 的 65536-frame ring、480-frame DSP period、4 period in flight 作为实验起点，但它们不是 fuxi 最终事实。`0xd0000000` 也不是 fuxi 默认地址。

验收：1000 个静音 periods 全部有明确完成或明确取消；无 stale completion、越界、SMMU fault 和不可恢复超时；SSR 后可重新建立 graph。

### Phase 4：CS35L41 transport 和安全状态机

目标：只确认两颗功放可以安全识别、复位、配置和 mute，不播放声音。

必须新建 CS35L41 实现，不在 `ShengCs35l43.*` 中加入 fuxi 分支。推荐状态机：

```text
OFF
 -> rail/pinctrl ready
 -> reset asserted
 -> reset released + datasheet delay
 -> ID/revision verified
 -> sticky faults read/cleared
 -> ASP/TDM + PLL configured
 -> MUTED_READY
 -> UNMUTED
 -> MUTED_READY
 -> reset/OFF
```

fuxi binding 的候选输入为：

- controller `qupv3_se8_i2c`，MMIO `0x00880000` 来自固定 fuxi/SM8550 源码，仍待 UniXNU 只读探针确认。
- amp B：I2C `0x40`、reset GPIO183、IRQ GPIO99。
- amp T：I2C `0x42`、reset GPIO182、IRQ GPIO98。
- GPIO68：只在 stock route 差分明确它与 `T-handset`/receiver 的关系后使用。

实现要求：

- 按 CS35L41 datasheet/Linux driver 和真机读回确认 register width、value width、wire endian、unlock/lock 和 reset delay。
- 识别 chip ID/revision 后才允许写配置；不匹配立即停止。
- 每个 write 后对关键寄存器 readback；IRQ handler 读取并分类 boost、PLL、thermal、short 和 brownout fault。
- PLL/ASP/TDM 参数从 Phase 1 的 frame format 生成，不手填 sheng 的 48 kHz/16-bit/slot 表。
- 初始 gain、boost current 和音量上限取保守值；global mute 是默认状态。
- profile/firmware 初期不加载，但要保留明确的“低功率 bring-up only”标志，防止被误认为日用配置。

验收：两颗功放分别 100 次 reset/identify/configure/mute；chip ID/revision 一致；PLL 在输入 clock 存在时锁定；无 fault IRQ；任何故障可单颗关闭。此阶段仍不能宣称“音频可用”。

### Phase 4 完成情况（主机部分）

`package/CirrusCs35l41.{h,c}`：状态机只负责**顺序与条件**，寄存器映、要写入的值、revision 掩码、boost 上下限与轮询预算
全部由 `Cs35l41Binding` 传入；两条线与复位由 hook 提供。因此本文件里没有任何 fuxi 地址，寄存器事实仍留在
`facts/audio/manifest.toml` 并保持 `unverified`。

保证的性质：**识别先于配置**（ID/revision 不符立即停止，且不写任何配置）、**故障先清后配**（清不掉即停）、
**每个配置写都读回比对**、**PLL 锁定后才进入 MUTED_READY**、**进入 MUTED_READY 前一定已 mute**、
**只有 verified + configured + locked + 无故障才允许解除 mute**。

`scripts/check-cirrus-cs35l41-host.py` 在 ASan/UBSan 下跑 **29 项**：完整路径、解除/恢复 mute、shutdown（先 mute 再复位）、
以及逐项拒绝——未就绪就解除 mute、错误 chip id（且一个配置字节都没写）、revision 越界、boot 永不完成、写读回不符、
写失败、boost 峰值超上限 / 电容低于下限（都在写之前拒绝）、PLL 永不锁定、上电即有黏性故障、解除前出现故障、
以及计划的验收项"100 次 reset/identify/configure/mute 全部达到 MUTED_READY 且 chip id 一致"。

真机部分（两颗功放的真实寄存器值与 revision、100 次读写循环、IRQ 分类）仍待设备授权。

### period 窗口与计数（完成于本轮）

`QcomAudioPath` 现在允许**最多 4 个 period 同时在 DSP 手里**（板上选 1..4，默认 1）。每个 period 用**写入的缓冲地址**识别，
应答按地址匹配：完成后 `completed++`、地址不在飞则计 `late`、重启时在飞的计 `cancelled`。游标按字节数推进并在缓冲末尾回绕，
因此两个 period 永不携带相同地址——这正是匹配能成立的前提。

这一轮测试抓出**两个真缺陷**与**两个测试缺陷**：

1. `submit()` 把 GPR 包头编码进 `p->tx`，而载荷就在同一个缓冲区里——**自我覆盖**，载荷前 16 字节被包头毁掉。
   之前"固定地址"的模拟 DSP 掩盖了它；改成载荷用 `p->apm`、编码包用 `p->tx`。
2. period 的偏移量从未推进（`period_bytes` 恒为 0），四次写的是同一个地址，应答只能匹配一次。改成游标推进。
3. ASan 抓到测试自身的**悬垂指针**：给 GLINK 通道传的是复合字面量 `(uint8_t[4*512]){0}`，生存期在 `rig_init` 返回时结束；
   `qcom_adsp_host.c` 里同样的写法也一并改成静态存储。
4. 绑定门禁立刻发现新增的 `periods_in_flight` 字段未被覆盖——即它的设计目的。

### A3 的参数化 speaker 拓扑（完成于本轮）

`package/QcomAudioTopology.{h,c}`：从**显式输入**装配 speaker 的 graph/endpoint/format —— 模块 instance 与 type、
接口类型与索引、采样格式、帧长、以及**每颗功放占用哪些 slot**。不填任何 sheng 的 48 kHz/16-bit/slot 表。

它拒绝"不可能对"的映射：slot 越界于帧长、两颗功放抢同一 slot、通道数与 mask 不一致、端点吃不了的位宽、
零采样率/零帧长、两个模块共用同一 instance、未知接口。装配结果由**调用方持有**，graph 直接指向它，不使用任何静态或线程局部存储。

`scripts/check-qcom-audio-topology-host.py` 在 ASan/UBSan 下跑 **31 项**：一条立体声映射的完整装配（含
`slot_owner` 的逐槽归属）、以及上述每一项拒绝；最后把装配结果**直接喂给 AudioReach 构造器**，验证图与端点命令里确实带着
graph id、两个模块 instance 与 slot 掩码——即"模板产出的东西可以原样使用"。

三个 A3 专属输入（`slot_mask_a/b`、`slot_count`、`sample_rate` 等 8 项）也已纳入 `facts/audio/binding.toml`
与门禁（现共 **86 项**，全部 `unverified`）。

### 板级绑定字段表与门禁（完成于本轮）

`facts/audio/binding.toml`：把三层配置的**每个字段**列成一条记录——值、状态、所依据的事实：

| 段 | 覆盖 |
| --- | --- |
| `adsp.*` | carveout、SMP2P entry 名与 ready/fatal 位、load-state 文本、三个超时（11 项） |
| `path.*` | GPR port、token seed、超时、stream instance、缓冲地址/大小，以及 graph/endpoint/format 三张子表（28 项） |
| `amp.registers` + `amp.b`/`amp.t` | 28 个寄存器映与限值字段，加两颗功放的 I2C 地址、reset/IRQ GPIO（34 项） |

`scripts/check-fuxi-audio-binding.py` 检查三件事，并逐项反向验证过：

1. `state = "verified"` 但证据写成 `none` → 拒（`verified but names no evidence`）；
2. `state = "verified"` 而证据在 manifest 里仍是 `unverified` → 拒（`verified on 'adsp.carveout', which is still unverified`）；
3. 某个层字段没被绑定覆盖 → 拒（`QcomAudioPathConfig.gpr_port is not in the binding`，字段名直接从 C 结构体里解析，不靠手写清单）。

`--require-verified` 会拒绝任何未验证项——这就是"未验证值不得进入构建"的可执行形式。当前 **78 项全部 unverified**，构建也尚未消费该表。

### Phase 5：speaker AudioReach graph 和第一声（A3）

目标：建立 fuxi 的真实 speaker endpoint，并安全证明左右/上下声道。

需要由 Phase 1 确定的 graph 输入：

- I2S sink 还是 TDM endpoint，以及 secondary interface ID。
- sample rate、sample width、slot width、slot count、BCLK/FS 极性和 master/slave。
- data line、B/T 的 slot mask 和左右声道映射。
- AudioReach module/instance/subgraph ID、hardware endpoint ID、PRM clock ID。
- 两颗 CS35L41 的 ASP RX source 和所需 feedback/SDOUT 状态。

测试顺序：

1. 功放保持 mute，graph 发送全零，确认 BCLK/FS/DATA 波形和 PLL lock。
2. 只启 amp B，播放 750 Hz、100 ms、强衰减 tone；确认声音来自预期位置。
3. mute B，只启 amp T，重复测试；GPIO68 保持 stock 默认安全状态。
4. 双 amp stereo，逐步延长至 500 ms；每步检查 DC offset、pop/click、PLL、IRQ、boost 和 temperature。
5. 重复 start/stop，验证 stop 顺序是先 mute、再停 graph/clock，start 顺序相反。

验收：两路映射正确；100 次 start/stop 无 pop、reset、fault IRQ、SMMU fault；每次 period 完成可追踪。出现异常立即降回 `MUTED_READY`，不得靠提高 gain 掩盖路由错误。

### Phase 6：CoreAudio output（A4）

目标：让 iPadOS 17.7.11 的系统音频通过共享 ring 驱动 A3 链路。

可从 sheng 复用：

- kernel audio service 与用户态 HAL 的 shared control/output/input mapping。
- 生产者/消费者 ring、period 调度、underrun/late/silent 统计。
- 统一的 zero timestamp 和 host timer timeline。
- audio server IO thread 的 time-constraint 思路。
- VirtualAudio/Thunderbolt transport 风格的 CoreAudio object model。

必须针对 21H461 重审：

- HAL ABI、arm64e signing、原始函数/对象布局、加载路径和 owner trust cache。
- `AudioGetHostClockFrequency` 的实际实现和 host timer 频率，不能直接套用 sheng 在 iPadOS 26/27 的指令定位。
- bundle ID、plugin filename、device UID、manufacturer/name 均改成 fuxi 身份，通用 ABI 结构尽量保持兼容。
- 第一版只发布 stereo output，不发布未完成的 input port。
- 音量曲线和最大增益受 CS35L41 安全策略限制，系统 100% 不等于硬件最大增益。

19.2 MHz、65536-frame ring、480-frame period 和 4 periods in flight 可作为初值；用 fuxi 的 IO cycle、lead、skew 和 underrun 数据调优。

验收：系统音效、音乐、暂停/恢复、音量调节、锁屏和 app 切换均可用；连续 10 分钟无可闻断续、无 underrun/late period、无 HAL/audio server crash；停止后功放可靠 mute。

### A4 的共享环（主机部分，完成于本轮）

`package/QcomAudioRing.{h,c}`：音频服务与 DSP 之间的共享输出环——两个索引跨一块缓冲、**永远留一帧空闲**（这样"满"与"空"可区分），
以及验收记录需要的全部计数：完成周期、**超期周期（late，并记最差延迟）**、**消费者填不满的周期（underrun，并记缺帧数）**、
**刻意写静音的周期**、**写入已满环的 overrun**。环大小、周期帧数、格式与 host clock 频率全部来自配置——
计划里提到的 19.2 MHz 与 65536 帧因此是**值**，不是常量。

`scripts/check-qcom-audio-ring-host.py` 在 ASan/UBSan 下跑 **31 项**：配置拒绝（帧数非周期整数倍、零周期/零率/零时钟/零通道/无缓冲、
在飞周期为 0 或超出环容量）、一次完整周期（数据原样落地、级别随生产者上升、消费者读完归零）、写满后 overrun 计数、
消费者跑在生产者前面时的 underrun 与缺帧数、静音周期确实是零且单独计数、**恰好压线不算 late 而超 5 tick 算 late 且记下延迟**、
以及**跨环尾写入**后数据仍按索引正确可读。

这 7 个新字段（`frames`/`period_frames`/`channels`/`bytes_per_sample`/`sample_rate`/`host_clock_hz`）
已进绑定，门禁现覆盖 **93 项 / 74 个层字段**；校验器同时改为**跳过指针字段**（共享缓冲与索引属运行时接线，不是板级事实）。

### A6 的恢复顺序（主机部分，完成于本轮）

`package/QcomAudioService.{h,c}`：把 ADSP 与数据面组成一个会话，并规定**重启的顺序**——

1. **先 mute 功放**（hook）：mute 失败则**重启根本不发生**，会话标记失败、代次不变，绝不让功放在重启中保持未静音；
2. 代次 +1；
3. 重建 ADSP 层与数据面（数据面的事务 token 高 8 位即代次，因此旧代次应答永远匹配不上）；
4. 清空共享环的两个索引。

`scripts/check-qcom-audio-service-host.py` 在 ASan/UBSan 下跑 **29 项**，**底层是完整栈**（Mdt / Qmp / Smp2p / Glink / Gpr /
AudioReach / Adsp / Path），AOP 与 DSP 均为模拟：端到端起来、跑起来、重启（mute 一次、代次推进、在飞周期取消、环复位）、
**再次起来**、旧代次应答只计 `late` 不套用、mute 失败时重启被拒且代次不变、carveout 装不下镜像时 bring-up 失败。

### Phase 7：WCD938x 和麦克风（A5）

目标：先完成一个明确的主麦 mono 48 kHz 路径，再增加其他麦克风。

不能直接复制 sheng 的 AMIC3 route。必须从 Phase 1 获得以下一整条链：

```text
physical mic
 -> AMIC/DMIC number
 -> micbias and voltage
 -> WCD ADC/input
 -> WCD SoundWire slave data port/channel
 -> TX SoundWire master port
 -> TX decimator/source
 -> codec DMA endpoint
 -> AudioReach capture graph
```

实施顺序：

1. 确认 WCD rails、reset GPIO、上电/掉电时序和 chip ID。
2. 完成 PRM macro/digital codec vote，使 VA/TX macro registers 可访问。
3. 配置 LPI SoundWire pins，依次打开 VA gate 和 TX macro clocks。
4. 启动 `0x06d30000` TX SoundWire master，枚举 WCD938x，记录 manufacturer/part/device number。
5. 只启目标 micbias 和 ADC，配置已确认的 slave data port/channel 与 TX decimator。
6. 建立 codec-DMA capture graph，先采集 2 秒到内存统计，不落盘。
7. 验证 200 个 periods 的 returned bytes/offset，统计非零率、RMS、peak、clipping 和 DC bias。
8. 通过后在 HAL 增加 mono input port，再验证 Voice Memos start/stop、权限和 route 切换。

验收：200/200 periods 完成，数值符合安静/说话场景变化且无 clipping；无 SoundWire/SSR/SMMU fault；Voice Memos 可重复启停。不得用“有非零数据”单独证明 route 正确，需通过遮挡/轻触目标麦位置或受控声源做空间确认。

### Phase 8：CS35L41 profile、保护和音质

目标：把 A3/A4 的低功率 bring-up 配置升级到安全日用配置。

工作项：

- 确认每个 `fast-switch` 文件的 stock 来源、适用 amp/route、版本、hash 和许可证；不可随意重分发 proprietary 内容。
- 确认是否还需要 WMFW、calibration、speaker impedance、temperature coefficient、boost limit 或 persistent calibration 数据。
- 不得复制其他设备或另一颗扬声器的 calibration。
- 明确 `music`、`handfree`、`handset` 的切换时序和 GPIO68 关系；切 profile 前先 mute。
- 建立 fault telemetry：temperature、boost、brownout、short、PLL unlock、DSP watchdog。
- 做低电池、充电、CPU/GPU 高负载、屏幕亮灭下的供电和失真测试。

验收：日用音量范围内无过热、削波、明显失真或保护风暴；profile 切换无 pop；30 分钟播放后温度和 fault 状态正常。未完成本阶段前，文档必须明确标注“仅低功率测试，不是日用音频”。

### Phase 9：路由、恢复和长稳（A6）

目标：补齐系统生命周期，而不只是一条能响的路径。

测试矩阵：

- output/input 快速 start/stop 1000 次。
- 连续音乐播放 1 小时；录音 30 分钟。
- suspend/resume、屏幕锁定/解锁、HAL reload、audio server restart、client crash。
- ADSP SSR 发生在 idle、graph open、period in flight、HAL IO 中的恢复。
- top/bottom/receiver route 切换；USB audio 和 Bluetooth 后续独立阶段加入。
- GPU 高负载、UFS IO、USB 调试同时运行时的 period deadline 和电源 vote。
- 充电、低电池和 thermal throttling 下的行为。

电话/IMS 音频依赖基带控制面、voice session 和系统 telephony 集成，不应混入首期媒体音频验收；在基带达到相应阶段后另立 route 和隐私/紧急呼叫测试。

验收：所有声明 route 在格式、声道、start/stop、错误恢复方面都有证据；无资源泄漏、stale graph、永久 mute 失效或影响其他外设的回归。

## 8. 推荐的模块拆分

建议最终结构如下，名称可随公共层现状调整：

```text
UniXNU / SM8550 common
  QcomMdt / QcomAdsp
  QcomSmem / QcomSmp2p / QcomGlink
  QcomGpr
  QcomAudioReach
  QcomAudioSharedRing
  QcomGeniI2c
  CirrusCs35l41
  QcomWcd938xTransport
  CoreAudio shared HAL core

unixnu-fuxi
  FuxiAudioBoard.{h,cpp}       # board binding only
  FuxiAudioProbe.cpp           # staged bring-up and logging
  FuxiAudioService.cpp         # product policy / publication
  FuxiAudioAbi.h               # stable shared ABI version
  hal/FuxiAudioPlugIn.c        # thin product adapter
  facts/audio/*
  scripts/check-fuxi-audio-*.py
```

板级 binding 应是普通数据，不应把地址和 GPIO 散落在协议实现中。每项 binding 最好同时保存 evidence key，使构建或审计脚本能追到 `facts/audio/manifest.toml`。

## 9. 验证与证据矩阵

### 9.1 当前状态（主机侧已完成，真机侧全部空缺）

下表由 `scripts/report-fuxi-audio-status.py` 生成：它实际运行每个套件并汇总，所以数字不会随文档漂移。

| 对象 | Host 套件 | 项数 | Device 证据 | 等级 |
| --- | --- | --- | --- | --- |
| 安全固件容器 | `check-qcom-mdt-host.py` | 43 | 无 | A0 未完成 |
| SMEM / SMP2P / GLINK | `check-qcom-a0-host.py` | 43 | 无 | A0 未完成 |
| QMP / RPMh | `check-qcom-loader-host.py` | 52 | 无 | A0 未完成 |
| ADSP 加载顺序（sheng 阶段梯） | `check-qcom-adsp-host.py` | 43 | 无 | A0 未完成 |
| ADSP 客户端层（sheng 形状） | `check-qcom-adsp-client-host.py` | 30 | 无 | A0 未完成 |
| GPR（sheng 原样） | `check-gpr-host.py` | 37 | 无 | A1–A2 未完成 |
| AudioReach 构造（sheng 结构） | `check-qcom-audio-graph-host.py` | 67 | 无 | A3 未完成 |
| 静音数据面（含 period 窗口） | `check-qcom-audio-path-host.py` | 28 | 无 | A1–A2 未完成 |
| speaker 拓扑 | `check-qcom-audio-topology-host.py` | 37 | 无 | A3 未完成 |
| 共享输出环 | `check-qcom-audio-ring-host.py` | 32 | 无 | A4 未完成 |
| CS35L41 步骤表（sheng 运行器） | `check-cirrus-cs35l41-host.py` | 38 | 无 | M3 未完成 |
| 共享音频页（sheng 形状） | `check-qcom-audio-abi-host.py` | 37 | 无 | A4 未完成 |
| GENI I2C（sheng 原样） | `check-geni-i2c-host.py` | 2169 | 无 | M3 未完成 |
| 采集统计 | `check-qcom-audio-capture-host.py` | 30 | 无 | A5 未完成 |
| 恢复顺序（端到端） | `check-qcom-audio-service-host.py` | 17 | 无 | A6 未完成 |

合计 **2710 项**主机检查，全部在 ASan/UBSan 下运行，且不依赖任何 fuxi 数值。

| 门禁 | 覆盖 | 状态 |
| --- | --- | --- |
| `check-fuxi-audio-binding.py` | 108 个字段 / 89 个层字段 | 0 verified，108 unverified |
| `check-fuxi-audio-facts.py` | 两颗 CS35L41、TDM/WCD 接线 | source-confirmed；runtime unverified |

### 9.1b HAL 插件（构建证据，不是运行证据）

`hal/FuxiAudioPlugIn.c` 是 sheng 的 CoreAudio 插件换 fuxi 身份：bundle id `org.unixnu.fuxi.audio`、IOKit 类 `FuxiAudio`、
设备/型号 UID、以及本板的共享页。机制未变（AudioServerPlugIn 驱动接口、三种映射内存、属性表、搬运帧的 IO 线程）。
它发布的数值**从映射后的页里读**，头文件里的宏只在页映射之前作兜底（流描述必须先答）。

`scripts/build-fuxi-audio-hal.py` 用 Xcode 的 iPhoneOS sysroot 加 macOS 的 CoreAudio 头编译并 ad-hoc 签名，本机实测产出
`FuxiAudio.driver`（CDHash `8c96f3ca41761825098d1d9f82d7ad0bc1ca1159`）。**这只是"能编译"，不是"能工作"**：21H461 的 HAL ABI
审计与真机验证都没做，插件从未在手机上运行过。


### 9.1c HAL 插件与 21H461 审计表

`hal/FuxiAudioPlugIn.c` 是 sheng 的插件换 fuxi 身份（bundle id、IOKit 类、设备/型号 UID、共享页），机制未变。
`scripts/report-fuxi-hal-audit.py` 从源码里提取它实现的**17 个驱动函数、87 个属性选择器、2 个 IO 操作**，并把其中
**24 项属于音频服务器语义**的列为审计对象——这些的"意思"不由我们决定，必须在 21H461 上实测确认。表由源码生成，
所以它不会与插件脱节（某个选择器从代码消失时脚本会直接失败）。

### 9.2 主机检查能证明什么、不能证明什么

### 9.1d 绑定字段的三类来源

绑定里的 107 个字段按来源分三类，门禁每次都会打印这个分布：

| 来源 | 数量 | 含义 |
| --- | --- | --- |
| 引用已记录事实 | 8 | 值直接来自 `manifest.toml` 里 source-confirmed 的字段（两颗功放的 I2C 地址、reset/IRQ GPIO、boost 峰值与电容） |
| sheng 的同 SoC 预测 | 8 | ADSP carveout、SMP2P entry 名与 ready/fatal 位、load-state 请求；由 `check-fuxi-audio-sheng-refs.py` 从 sheng 源码读回比对 |
| 尚无来源 | 91 | 拓扑实例号、环与页的几何、寄存器映、超时等 |

门禁对第一类还会**核对值与出处是否一致**——加上这条规则时立刻抓出 6 个字段写着 0、而出处写着 0x40/183/99 的不一致。

另有一项不按等级归类的加固：`check-qcom-audio-fuzz-host.py` 用固定种子向**固件容器解析器、GPR 解码器、APM 应答解析器和
GLINK 线路**各投 4000 轮输入（一半纯噪声、一半"合法头部 + 噪声载荷"），在 ASan/UBSan 下断言不越界、解码出的长度自洽、
链路把垃圾输入归类而不是静默失败。它证明的是**解析器不会因为怪字节而崩**，不是"解析结果正确"。

主机侧证明的是**顺序、条件与计数**：识别先于配置、故障先清后配、配置写回读、PLL 锁定后才算 ready、mute 先于重启、
事务按 token 代次隔离、period 按地址匹配并有 late/underrun/cancel 计数、环的满/空可区分。

主机侧**不能**证明：ADSP 是否真的启动、PLL 是否真的锁定、功放是否真的识别、声音是否可闻、CoreAudio 是否真的工作。
每一条都必须用真机证据补齐，这也是 9.1 里 Device 列全部为"无"的原因。

### 9.3 原始验收矩阵（设计意图，未变）

| 对象 | Host 验证 | Device 验证 | 不能据此宣称 |
| --- | --- | --- | --- |
| ADSP loader | 分段、边界、错误注入、状态机 | authenticate/ready/SSR 20 次 | 扬声器可用 |
| GLINK/GPR | 编解码、intent、token、timeout、乱序 | `adsp_apps` 事务和恢复 | AudioReach graph 正确 |
| AudioReach | graph 序列、map/unmap、period 模型 | 静音 1000 periods、SSR | 功放安全 |
| GENI I2C | endian、长度、timeout、NACK | 两颗 chip ID/revision/readback | 能播放声音 |
| CS35L41 | register model、状态机、fault injection | PLL/mute/fault/100 次复位 | 保护和音质完成 |
| speaker | format/slot 配置模型 | 波形、位置、低音量 tone、100 次启停 | CoreAudio 可用 |
| HAL output | ABI/object/ring/timestamp tests | 系统应用、10 分钟播放 | 麦克风可用 |
| WCD/SoundWire | port/channel/period parser | 枚举、route、200 periods | 所有麦都正确 |
| protection | profile parser/policy | 温度、失真、低电池、长播 | 通话音频完成 |

### 9.4 每格还缺什么

| 缺的东西 | 影响的行 | 需要谁来提供 |
| --- | --- | --- |
| fuxi 的 `adsp.mbn` / `adsp_dtb.mbn` 或来源说明 | ADSP loader、GLINK/GPR、AudioReach | 用户 |
| 设备授权（只读探针：不写寄存器、不 unmute） | ADSP loader、CS35L41、speaker | 用户 |
| 两颗功放的真实寄存器映与 revision | GENI I2C、CS35L41 | 设备读回 |
| TDM/BCLK/FS 极性、slot 与左右声道映射 | speaker、HAL output | 设备测量 |
| 21H461 的 HAL ABI 与 host timer 实现 | HAL output | 反解 + 设备 |
| WCD938x 的 SoundWire 枚举与主麦 route | WCD/SoundWire | 设备枚举 |


### 9.5 按等级的收束：每级还缺哪一条真机证据

主机侧 2723 项检查按等级归属如下。**同一套件可能横跨两级**，下表按它主要服务的那一级归类。

| 等级 | 套件（项数） | 主机侧证明了 | 还缺的真机证据 |
| --- | --- | --- | --- |
| A0 | `qcom-mdt` 43、`qcom-a0` 43、`qcom-loader` 52、`qcom-adsp` 43、`qcom-adsp-client` 30 | 容器解析、SMEM/SMP2P/GLINK、QMP/RPMh、加载顺序与阶段梯、客户端注册与排队 | `adsp.mbn`/`adsp_dtb.mbn` 能解析；carveout 与 PAS id；SMP2P entry 名与 ready/fatal 位；GLINK 通道名；QMP 描述符位置 |
| A1 | `gpr` 37 | 24 字节头、编解码、basic result、SPF readiness | GPR 端口号、APM 实例号、固件版本与 SPF 就绪时序 |
| A2 | `qcom-audio-graph` 67、`qcom-audio-path` 28 | 开图/config/命令/map/write/read/PRM 的报文形状；事务与 period 窗口 | graph/endpoint/转换器实例号；缓冲地址与 map handle 的真实回值；1000 个静音 period |
| A3 | `qcom-audio-topology` 37、服务套件的 **100 次启停** | 拓扑装配与一致性拒绝、slot 归属；**100 次 start/stop 全部完成，无超时、无残留映射、period 收支平衡** | TDM/BCLK/FS 极性、slot 与左右声道实测、两颗功放的地址与 revision |
| M3 | `geni-i2c` 2169、`cirrus-cs35l41` 38 | 传输层运行器与状态分类；功放步骤表、身份/故障/PLL 门 | 引擎寄存器实测、两颗功放 chip id/revision/OTP、100 次复位循环、IRQ 分类 |
| A4 | `qcom-audio-ring` 31、`qcom-audio-abi` 36、`qcom-audio-service` 39（含 A5/A6） | 环的计数与满空区分；共享页布局与时间轴；服务的 IO 周期与发布 | **21H461 的 HAL ABI 审计（24 项，见 9.1c）**、host clock 实测、10 分钟无 underrun |
| A5 | `qcom-audio-capture` 30 | 200 周期的验收形状：非零率、RMS、peak、削波、DC 偏置 | WCD938x 枚举、micbias/ADC、目标麦的 slot、200 个真实 period |
| A6 | 服务套件（含 20 次重启） | mute→代次→重建的顺序；在飞取消；旧代次应答被忽略；**20 次重启在主机模型里跑完，每次 mute 恰好一次、代次逐一推进** | 真机 SSR 20 次、长稳运行 |

**一句话**：主机侧能证明的都已经证明了；上表右列每一项都需要设备或固件，没有一条能靠继续写主机测试推进。


### 9.6 真机会话的读数清单

91 个"尚无来源"的字段不是散落的，它们按**能定案它们的那一次读**分组。`scripts/report-fuxi-audio-probe-reads.py` 直接
从 `binding.toml` 生成这张清单（所以字段填实后它会自动变短），并把"只读就能拿到"与"需要先 bring-up"分开：

| 组 | 读什么 | 是否需要先 bring-up |
| --- | --- | --- |
| amplifier | 两颗器件的 chip id / revision / OTP trim / 中断初值（I2C 只读） | 否 |
| adsp | SMEM 项是否可读、SMP2P 名与位、AOSS 邮箱描述符、carveout 是否落在可用 DRAM、GLINK 通道名 | 否（只读 SMEM 与窗口） |
| path | GPR 端口与 APM 实例号、拓扑实例号、map handle 真值、格式是否被端点接受 | 是（需要 ADSP 起来并开图） |
| abi | host clock 频率 | 是（需要内核运行） |
| capture | WCD938x 枚举与目标麦 slot | 是（需要 TX macro 上电） |
| — | 环与页的几何 | 无需读：**是选择，不是测量** |

当前状态：107 个字段分布在 16 张表，15 张被读数覆盖，`path.references` 的 4 个字段是**结构字段**（由代码填充，不是读数）。

**这只读不写**：清单里的第一类（I2C 与 SMEM 只读）不改变任何设备状态，是"只读探针"能直接拿到的部分。


### 9.7 探针镜像的打包（照 `docs/boot_track.md`）

音频探针是内核里的一段只读代码，要跑在真机上就必须打包成 boot 镜像。**不要把 `CONFIG_XNU_OWNER_DATA_MOUNT` 关掉**——
它是 Data 卷挂载的开关（当初特意打开，`77d0724`），关掉会让系统停在 `kern_exec.c`。正确做法是用树里已经生成的 Data seed：

```bash
# 内核用真实仓库的配置（副本里改 defconfig 会连带丢掉 display 归档，链接会失败）
cd ../UniXNU && ./unixnu defconfig fuxi_defconfig -D ../unixnu-fuxi && ./unixnu build kernel

# payload 与镜像用一份**只改 board.toml**（加 fuxi-audio-probe=1）的副本
./unixnu build payload -D /tmp/fuxi-probe --uefi ../uefi-fuxi \
    --kernel out/fuxi/ipados-17.7.11-21H461/fuxi/kernel/kernel.macho \
    --unique-chip-id 0x0123456789abcdef --data-ramdisk out/ipsw/21H461/data.dbr
./unixnu bootimg -D /tmp/fuxi-probe --uefi ../uefi-fuxi --target DEBUG \
    --dtb ../uefi-fuxi/Platforms/Xiaomi/fuxiPkg/FdtBlob/sm8550-xiaomi-fuxi.dtb -o /tmp/fuxi-probe.img
```

**写阶段先证明再上机。** 探针的映射方向必须与阶段一致：只读阶段用 `kIOMapReadOnly`，写阶段（`fuxi-audio-probe>=4`）用读写映射。
第一次尝试分配 SMP2P 项时正是因为映射是只读的，`qcom_smem_alloc` 的第一次写入就 fault，内核当场死掉。

这样镜像**正常启动到桌面**，探针在 `bsd_init` 跑完、报告走 USB CDC 通道（`scripts/fuxi-usb-debug.sh`），
不需要拍照，也不需要动手机的电源键。`scripts/fuxi-audio-probe.sh run` 把这几步串成一条命令，并校验
**payload 里的 kernel 就是刚构建的那个**（`bootimg` 曾把旧 payload 打进镜像，导致刷了错的内核）。

## 10. 日志和验收记录格式

每次设备测试至少记录：

```text
image_commit:
device_commit:
common_commit:
stock_evidence_revision:
os_build: 21H461
boot_args:
test_level: A0..A6
routes_enabled:
adsp_generation:
graph_id / endpoint_id:
buffer_pa / size / attributes:
period_submitted / completed / cancelled / late:
amp_id / revision / pll / fault / temperature:
mic_route / returned_bytes / nonzero / rms / peak:
other_regressions: UFS / USB / display / touch / GPU
result: PASS / FAIL / INCONCLUSIVE
next_measurement:
```

Host 编译通过、host test 通过和真机观察必须分开报告。对失败不得只写“没声音”；至少区分：ADSP 未在线、GPR timeout、graph 未 start、period 未兑现、TDM 无 clock、PLL 未锁、功放仍 mute、slot 错误、fault shutdown 或 HAL 未产生数据。

## 11. 里程碑和建议优先级

| 里程碑 | 前置 | 主要交付物 | 推荐优先级 |
| --- | --- | --- | --- |
| M0 事实集 | 固定 fuxi 源码与主线审计 | source manifest、未验证字段表、可用离线 firmware/profile hash | 立即 |
| M1 A0 | M0 的 ADSP 事实 | 通用 ADSP + fuxi binding | 高 |
| M2 A1/A2 | M1 | GPR/APM + 静音 periods | 高 |
| M3 CS35L41 安全状态机 | M0 的总线和电气事实 | 通用 CS35L41 + fuxi binding | 高，可与 M1/M2 分支并行设计 |
| M4 A3 第一声 | M2 + M3 | speaker graph 和低音量证据 | 高 |
| M5 A4 CoreAudio | M4 | output-only HAL | 中高 |
| M6 A5 麦克风 | M2 + M0 mic route | WCD/SWR capture + HAL input | 中 |
| M7 A6 日用级 | M5 + M6 | profile/protection/recovery/long run | 必须完成后才标记 usable |

最短关键路径是：

```text
静态证据边界
 -> ADSP/GLINK
 -> GPR/APM 静音数据面
 -> CS35L41 mute-ready
 -> TDM 第一声
 -> CoreAudio output
```

麦克风可以在 A2 后与 speaker/CoreAudio output 并行调查，但在主麦 route 未由 fuxi 的 UniXNU 探针或直接测量确认前不应写板级配置。

## 12. 第一轮实际执行清单

当前只执行主机和源码工作：

- [x] 固定 fuxi DTS/DTSI revision，并把两颗 CS35L41、总线、GPIO、TDM 和 WCD938x 声明写入 `facts/audio/manifest.toml`。
- [x] 阅读 Linux 主线 CS35L41、remoteproc、GLINK、GPR/APR 和 APM/PRM 路径，记录许可证与安全边界。
- [x] 实现无板级常量的 `QcomGpr` 编解码/SPF readiness 模型，并用 ASan/UBSan 主机测试覆盖。
- [x] 以显式 topology 参数实现 AudioReach builder/parser；不内置 sheng 的 graph/endpoint/clock/TDM 常量。
  `package/QcomAudioReach.{h,c}`：graph open（SG/container/module list/module prop/connection）、start/stop/close、
  I2S 与 codec-DMA 端点配置、stream 媒体格式与 PCM 输出、时钟投票、共享内存 map/unmap，以及 APM 应答解析。
  graph id、module instance、interface index、clock id、slot mask 全部来自调用方传入的 topology；
  规范常量（`PARAM_*`/`PROP_*`/`OP_*`）才写死。`scripts/check-qcom-audioreach-host.py` 在 ASan/UBSan 下跑 57 项检查，
  其中两项专门验证"同输入确定性"与"两个 topology 产生两个包、且输出里不出现 sheng 的 0x4005/0x4007/0x30c/0x400"。
- [ ] 对合法取得的 fuxi ADSP/adsp_dtb 离线文件记录来源、版本、hash 和 segment；在取得前全部保持 `unverified`。
- [ ] 设计但不运行 UniXNU 只读探针，以后只有用户重新明确授权设备操作时才验证 chip ID、ADSP channel、TDM 和 route。
- [ ] 找到低于 4 GiB、与现有外设不冲突的音频 buffer 区，并确认 SMMU ownership；在有证据前不借用 sheng 地址。
- [ ] 所有值继续写入 `facts/audio/manifest.toml`；未确认值不进入板级配置。

完成以上清单后，才适合决定 sheng 文件中哪些逻辑应先抽到 UniXNU 公共层，以及 fuxi 的最小 binding 具体包含哪些字段。

### A0 传输层（完成于 2026-11-08）

从 UniXNU-sheng 的公共线迁入三个板级无关层，并补上它们此前没有的主机测试：

| 文件 | 内容 |
| --- | --- |
| `package/QcomSmem.{h,c}` | SMEM 分区表（`$TOC`/`$PRT`）、按主机对查分区、未缓存条目分配与查找 |
| `package/QcomSmp2p.{h,c}` | 出站/入站 item 布局（magic/version/pids/计数/标志/条目）、位更新、doorbell 服务 |
| `package/QcomGlink.{h,c}` | 原生 GLINK（版本握手、channel open/close、intent、data、done）跑在一对 SMEM FIFO 上 |

`scripts/check-qcom-a0-host.py` 在 ASan/UBSan 下跑 **43 项**检查，其中包括**两个 GLINK 实例互为对端**：握手、
通道建立、intent 交换、一次 `hello` 消息的双向传输与计数、以及一条"不可能成立的消息"必须让链路停止而非继续猜测。

固件容器层（`package/QcomMdt.{h,c}`，同样来自 sheng 公共线）也一并迁入并补了主机测试：
它解析安全世界接受的 ELF 容器（ELF/程序头、hash 段、可加载范围、carveout 内落位）以及 `.bNN` 分片按整镜像读取。
`scripts/check-qcom-mdt-host.py` 在 ASan/UBSan 下跑 **43 项**，既构造了一份真实 ELF64 镜像，也逐条验证拒绝：
坏 magic、大端、无程序头、超表长、段文件大小超内存大小、无 hash 段、无可加载段、越界落位、分片读取越界。

同轮迁入的还有加载器的两件状态机（`package/QcomQmp.{h,c}`、`package/QcomRpmh.{h,c}`）与 `QcomScm.h` 的调用形状：

| 文件 | 内容 |
| --- | --- |
| `QcomQmp` | AOSS 邮箱握手（描述符校验、link/channel 四步与 kick、一次一条文本消息、开机阶段遗留的"已开"通道直接用） |
| `QcomRpmh` | cmd-db 资源查找 + ARC 档位索引 + 一次 active-only 请求在 RSC TCS 里的寄存器写（v3 与 v2 两套布局） |
| `QcomScm.h` | 安全调用的形状声明（不含实现，故不参与主机测试） |

`scripts/check-qcom-loader-host.py` 在 ASan/UBSan 下跑 **52 项**：握手逐级推进与"停在等 AOP"的两个状态、消息长度字与文本、
	extbf{拒绝}（未对齐邮箱、过小邮箱、未开就发、超长消息、超过四条命令、写表太小），以及 cmd-db 的坏 magic/过短/越界 aux
与档位索引的四种取值、两套 RSC 布局的差异和一次请求的 12 次写入顺序。

### A0 加载顺序（完成于本轮）

`package/QcomAdsp.{h,c}` 把上面七件串成一条不会等待的状态机：镜像落位（每段过 `qcom_mdt_place`）→ 认证 hook →
内存 hook → QMP 握手与 load-state 请求 → 等 DSP 的 ready 位 → GLINK 与通道就绪。**板级值全部入参**
（carveout、entry 名、ready/fatal 位、load-state 文本、三个超时），需要硬件的两步是 hook。

`scripts/check-qcom-adsp-host.py` 在 ASan/UBSan 下跑 **33 项**：一条完整成功路径（真实 Mdt/Qmp/Smp2p/Glink 在下面，
AOP 逐寄存器模拟），以及逐项故障注入——carveout 小于镜像、小于取整后的 span、缺 hook、认证失败、认证需要再来一次、
内存失败、邮箱无描述符、邮箱永不开启、DSP 永不 ready、DSP 报 fatal、链路永不上、失败后再调用仍返回同一原因。

### A1–A2 静音数据面（完成于本轮）

`package/QcomAudioPath.{h,c}`：框架 readiness → 开图 → 端点配置 → 流配置 → 缓冲映射 → 图启动 → 写 period，
**一次只有一个未决事务**，token 的高 8 位是重启代次，所以 ADSP 重启后的旧应答永远匹配不上新事务。

`scripts/check-qcom-audio-path-host.py` 在 ASan/UBSan 下跑 **18 项**，底层是**真实的 GLINK、GPR、AudioReach**，
对端是一个逐条应答的模拟 DSP：完整启动路径、period 提交与完成计数、readiness 永不就绪、配置被拒、命令无应答超时、
无未决事务却收到应答（计 late 而不套用）、重启后旧代次应答被忽略、重启后能再次起来、缺 graph 拒绝、未运行就写 period 拒绝。

这一轮测试抓出**三个真实缺陷**（都在新写的数据面里，不是测试）：readiness 查询在"发送退避"时被丢弃（探针于是永远等一个
从未发出的查询）；框架应答没有转交给 readiness 探针（只当未知包计数）；token 比对忽略了高位的代次。

A0 仍未完成的部分是 **ADSP 加载器本身**（`adsp.mbn`/`adsp_dtb.mbn` 的段、内存窗口、代理电源域、SMEM 布局的实际取值），
它需要 fuxi 的固件与设备证据，按计划保持未验证。

## 13. 当前未执行事项

主机侧已经完整：11 个板级无关层与 2 道门禁（9.1），2710 项检查全部在 ASan/UBSan 下通过，且没有一条依赖 fuxi 的数值。
每个文件都把自己的板级值做成了入参，`facts/audio/binding.toml` 用 108 个字段记录它们，门禁拒绝未验证值进入构建。

仍然没有做、也无法在主机上做的：

- 把音频组件接入 `dts/fuxi.dts`、`board.toml` 或内核构建（`board.toml` 至今没有音频归档与启动参数）；
- 编译带音频的 kernel/payload；
- 启动、刷写或测试设备；
- 读取 fuxi 的 ADSP 状态、功放寄存器，或播放任何声音；
- 证明任何 fuxi 音频等级 A0--A6 已完成。

需要的外部输入见 9.4：ADSP 固件（或来源说明）与设备授权。在这两样到位之前，等级停在"A0–A6 均未完成"，
但**结构上已经没有任何未知**：剩下的是数值与证据，而不是设计。

## 14. 与 sheng 音频线的对齐

原则：**机制仿 sheng**。sheng 的音频线是 `package/` 下 13 个文件、3329 行；本轮之前我在同一件事上另立了一套划分
（11 个文件、1642 行），机制不同、命名不同，这不符合"机制仿 sheng"。下表是逐项对照与动作，顺序即实施顺序。

| sheng | 行数 | fuxi 现状 | 机制差异 | 动作 |
| --- | --- | --- | --- | --- |
| `GeniI2c.{c,h}` + `tests/geni_i2c_host.c` | 154+101+218 | **已原样移植**（sha256 相同） | 我曾自造"可配置寄存器/值宽度 + 两段式读"，已删除 | ✅ 完成 |
| `ShengGpr.{c,h}` | 106+60 | `QcomGpr` 188 行 | 机制相同（encode/decode/basic_result/spf_init/next/receive/timeout），但**符号与签名不同** | 按 sheng 的接口对齐，删除自造签名 |
| `ShengAudioReach.{c,h}` | 375+109 | `QcomAudioGraph` 已按 sheng 结构参数化：开图 + config（I2S 端点/转换器输出/端点媒体格式）+ 图命令 + map/write/read + PRM 时钟与投票 + 应答解析（67 项）。与 `QcomAudioReach` 的并立待合并 | sheng 每方向**两张图**（device graph 拥有硬件端点、stream graph 拥有共享内存端点，连接由 stream graph 的 open 携带） | 接入会话后按双图重写 config 构造 |
| `AdspFirmware.c` + `AdspProbe.cpp` + `ShengAdsp.h` | 182+690+30 | `QcomMdt` + `QcomAdsp`（含 **sheng 的 13 级阶段梯**，43 项）+ **`QcomAdspClient`（sheng 形状，30 项）** | 阶段梯与客户端层均已按 sheng 落地；本层只报告它自己判定的阶段，板级阶段由绑定上报 | ✅ 完成（板级阶段的执行序列仍属绑定） |
| `ShengCs35l43.{c,h}` | 141+92 | **运行器已原样移植**（`Cs35Run`），程序表由 binding 生成，安全门在门内（38 项） | 曾是相位状态机 + 32 位 hook | ✅ 完成（程序内容按 CS35L41 事实，不抄 CS35L43 的寄存器程序） |
| `ShengAudioAbi.h` | 88 | **已按 sheng 形状重做**（`QcomAudioAbi`，33 项）：control page + 双环语义 + `ticks_per_frame_q32` + 双 anchor + `out_seq/in_seq` + `lead_frames` + `*_late` + 方法号；诊断页与追踪页按 sheng 的 2048/2560 偏移预留 | 自造的 `QcomHalAbi` 已删除 | ✅ 完成（诊断/追踪页内容待 plug-in 落地时填） |
| `AudioProbe.cpp` + `ShengAudioService.cpp` + HAL | 1159+143 | `QcomAudioPath`/`QcomAudioRing`/`QcomAudioCapture`/`QcomAudioService` | 我自划分层，与 sheng 的服务/探针/HAL 三分不同 | 把已有能力（period 窗口、事务代次、采集统计、SSR 恢复）**搬进** sheng 的结构，而不是并列 |

保留的判断：上述能力本身仍然有用（period 窗口与 late/underrun 计数、采集统计、重启代次隔离），它们不删除，但要落在
sheng 的文件与机制里。删除的是**与 sheng 冲突的自造机制**（已删：`QcomGeniI2c`）。
