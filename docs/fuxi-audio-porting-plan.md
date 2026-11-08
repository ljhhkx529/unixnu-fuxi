# Xiaomi 13（fuxi / SM8550）音频移植方案

状态：设计方案，尚未实施或进行设备测试

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
2. 两台设备都使用 GPIO121--124 一组 secondary 音频引脚，但 fuxi DTS 把它声明为最多 4 slot 的 TDM 接口。实际 frame sync、slot width、slot mask、左右声道映射必须由 fuxi stock runtime 或波形确认。
3. 两台设备都处于 WCD938x + TX SoundWire 的 SM8550 音频体系中，但这并不能证明主麦路径相同。sheng 已确认的 AMIC3/ADC2/port 2/TX3 只能作为查找模板。
4. sheng 的 AudioReach、共享环、19.2 MHz timeline 和 CoreAudio object model 可以作为移植起点；sheng 的 iPadOS 26/27 arm64e 细节不能直接当作 fuxi 的 21H461 ABI 事实。
5. fuxi stock DTS 存在 CS35L41 `fast-switch` profile。首声可暂时采用低功率、短时的安全固定配置，但日用音频不能以“永久绕过功放 DSP/保护”为完成状态。

因此，实施顺序应是“先控制面、再静音数据面、再低音量扬声器、最后 CoreAudio 和麦克风”，而不是先复制 sheng 功放驱动。

## 2. 证据等级与当前基线

所有音频值必须带证据等级：

| 等级 | 含义 | 可否进入 fuxi 板级配置 |
| --- | --- | --- |
| `confirmed-device` | 来自 fuxi stock live DT、Android 真机运行状态、总线读回或测量 | 可以 |
| `confirmed-source` | 来自固定 commit 的 fuxi 源码，且与目标硬件明确匹配 | 可以，但关键电气值仍应真机复核 |
| `reference-sheng` | 来自 sheng 的已验证实现 | 只能指导架构和调查方法 |
| `hypothesis` | 来自同 SoC、其他 Xiaomi 机型或未验证静态 DTS | 不可以，只能生成下一项测量 |

当前 fuxi 项目事实：

- `porting.toml` 中 `audio` 仍是 `planned`，没有选择音频组件。
- `dts/fuxi.dts` 尚未描述 ADSP 或板级音频设备。
- 当前没有 fuxi 真机音频事实集，也没有 fuxi 音频设备测试结果。
- `docs/dtb_diff_report.txt` 中的 `audio_cma_region`、ADSP 和 SMMU 差异只能提示调查方向，不能据此确定地址、IRQ 或路由。

当前 fuxi 硬件线索来自 LineageOS `android_kernel_xiaomi_sm8550-devicetrees` commit
`43b5ada5654ed84c25f74789bc8f6326b7bacd20`。这些值在 stock live DT/Android runtime 复核前仍按
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
| ADSP 固件 | 从 `modem_a` FAT `/image` 读取 `adsp`、`adsp_dtb` | 路径、版本、分段和 carveout 待真机确认 | 复用 loader 结构，不复制 manifest |
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
| 麦克风 codec | WCD938x | WCD938x 线索 | codec transport 可参考，route 必须重测 |
| 主麦 route | AMIC3 -> bias3 -> ADC2 -> SWR -> TX3 | 未知 | 从 mixer diff、DAPM 和 capture 实测获得 |
| 音频 DMA | 低于 4 GiB，sheng 默认 `0xd0000000` | 可用低地址空洞未知 | 重新做内存地图和所有权审计 |
| SMMU | translating context 会 reset，使用 pass-through | 未验证 | 先复现 stock ownership，不能假定相同 |
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
8. 遵循设备仓库规则：设备测试只在所有者明确要求后进行，并优先使用可恢复的临时启动；本方案本身不授权刷写。

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

### Phase 1：采集 fuxi stock 黄金基线

目标：得到可重放的 Android 正常路径和完整硬件拓扑，不凭 sheng 猜参数。

#### 1.1 静态输入

- `/sys/firmware/fdt` 和最终应用后的 overlay；同时记录 stock build fingerprint。
- `/proc/iomem`、reserved-memory、ADSP/ADSP-DTB carveout、audio CMA、SMEM 和 SMMU 节点。
- `/vendor/etc/mixer_paths*.xml`、`audio_policy_configuration.xml` 及其 include。
- `/vendor/etc/acdb*`、`/vendor/etc/audio*`、Cirrus firmware/profile 文件名、大小和 hash。
- `/proc/asound/cards`、`/proc/asound/pcm`、codec/component/DAI link 列表。
- remoteproc/PAS/ADSP firmware 名、分区来源、启动时序和 crash/SSR 节点。
- I2C、SoundWire、regulator、clock、pinctrl、interconnect 和 SMMU 的最终节点。

#### 1.2 动态差分

对以下每个场景分别保存开始前、运行中和停止后的 `tinymix`、DAPM、clock/regulator、I2C/SoundWire 状态差分：

1. bottom speaker 单独播放。
2. top speaker 单独播放。
3. receiver/handset 播放。
4. speakerphone 播放。
5. 主麦录音。
6. 副麦/降噪麦录音。
7. 视频录音或 camcorder route。

同时采集：

- `dumpsys media.audio_flinger` 和 `dumpsys media.audio_policy`。
- `dmesg` 中 ADSP、GPR、APM、LPASS、CS35L41、WCD938x、SWR、SSR、SMMU 事件。
- `/sys/bus/i2c/devices` 和允许读取的 regmap/debugfs，只读记录 chip ID、revision、PLL/fault 状态。
- SoundWire slave enumeration、manufacturer/part ID、logical device number、data port 和 channel mask。
- TDM/MI2S sample rate、BCLK、FS、word length、slot count、slot mask、SoC/codec master 关系。

若软件状态无法唯一确定 frame format，使用逻辑分析仪观察 GPIO121/123/122/124；先不接触扬声器输出电气端。

#### 1.3 产出

建议目录：

```text
facts/audio/
  manifest.toml
  stock-build.txt
  live.dtb
  adsp-firmware.sha256
  topology.md
  speaker-bottom/
  speaker-top/
  receiver/
  mic-main/
  mic-secondary/
  tdm-waveform/
```

验收：能画出 stock 中从 AudioFlinger 到两个功放、从主麦到录音 PCM 的完整 route；每个板级值都有来源。

停止条件：live DT 与固定源码冲突但未解释；关键 GPIO/rail 被其他设备复用；无法确认哪个 profile/calibration 属于本机。

### Phase 2：ADSP 固件、SMEM 和 GLINK（A0）

目标：只让 ADSP 可控地在线，不创建音频 graph、不碰功放。

实施包：

1. 从 fuxi manifest 读取 `adsp`/`adsp_dtb`，校验 hash、分段范围、对齐和 carveout。
2. 按 stock 顺序完成 PAS authenticate/load/start；保留每一个真实错误码。
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

- controller `qupv3_se8_i2c`，MMIO `0x00880000` 待 live DT 确认。
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

应从 sheng 移植并参数化相应 host-test 思路：

- `check-adsp-host.py`
- `check-modem-fw-host.py`
- `check-qrtr-host.py`（仅在 audio 路径共享 QRTR 代码时）
- `check-gpr-host.py`
- `check-audioreach-host.py`
- `check-geni-i2c-host.py`
- CS35L41 新测试；不能把 `check-cs35l43-host.py` 改名后直接当作通过。

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
| M0 事实集 | stock Android 可访问 | live DT、mixer/route diff、firmware/profile hash | 立即 |
| M1 A0 | M0 的 ADSP 事实 | 通用 ADSP + fuxi binding | 高 |
| M2 A1/A2 | M1 | GPR/APM + 静音 periods | 高 |
| M3 CS35L41 安全状态机 | M0 的总线和电气事实 | 通用 CS35L41 + fuxi binding | 高，可与 M1/M2 分支并行设计 |
| M4 A3 第一声 | M2 + M3 | speaker graph 和低音量证据 | 高 |
| M5 A4 CoreAudio | M4 | output-only HAL | 中高 |
| M6 A5 麦克风 | M2 + M0 mic route | WCD/SWR capture + HAL input | 中 |
| M7 A6 日用级 | M5 + M6 | profile/protection/recovery/long run | 必须完成后才标记 usable |

最短关键路径是：

```text
stock 事实集
 -> ADSP/GLINK
 -> GPR/APM 静音数据面
 -> CS35L41 mute-ready
 -> TDM 第一声
 -> CoreAudio output
```

麦克风可以在 A2 后与 speaker/CoreAudio output 并行调查，但在主麦 route 未由 fuxi runtime 确认前不应写板级配置。

## 12. 第一轮实际执行清单

在开始写驱动前，先完成以下清单：

- [ ] 保存 fuxi stock live DT 和 build fingerprint。
- [ ] 确认 ADSP/adsp_dtb 来源、版本、hash、carveout 和启动状态。
- [ ] 确认 `adsp_apps` channel、intent 行为和 GPR/APM version。
- [ ] 保存音频 policy、mixer path、ALSA card/PCM 和 DAPM 状态。
- [ ] 分别对 bottom、top、receiver、main mic 做 runtime mixer diff。
- [ ] 在 live DT 确认 CS35L41 的 bus/address/reset/IRQ 和 GPIO68。
- [ ] 只读确认两颗 CS35L41 chip ID/revision 和 stock profile hash。
- [ ] 确认 TDM BCLK/FS/SD、slot count/width/mask 和 master/slave。
- [ ] 确认 WCD938x reset/rails、TX SWR enumeration 和主麦完整 route。
- [ ] 找到低于 4 GiB、与现有外设不冲突的音频 buffer 区，并确认 SMMU ownership。
- [ ] 为所有值写入 `facts/audio/manifest.toml`；未确认值不进入代码。

完成以上清单后，才适合决定 sheng 文件中哪些逻辑应先抽到 UniXNU 公共层，以及 fuxi 的最小 binding 具体包含哪些字段。

## 13. 当前未执行事项

本方案只完成了源码/设备树对比和移植设计，没有：

- 修改音频源码、`dts/fuxi.dts`、`board.toml` 或 `porting.toml`；
- 编译 kernel/payload；
- 启动、刷写或测试设备；
- 读取 fuxi 的功放寄存器或播放声音；
- 证明任何 fuxi 音频等级 A0--A6 已完成。

因此当前准确状态仍是：**sheng 提供了经过验证的参考架构；fuxi 的音频硬件线索已经足以规划工作，但真机事实采集尚未开始。**
