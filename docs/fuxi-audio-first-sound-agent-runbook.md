# Fuxi 音频首次出声 Agent 操作指南

更新时间：2026-11-09  
目标设备：Xiaomi 13（fuxi / SM8550）  
目标系统：UniXNU + iPadOS 17.7.11（21H461）  
唯一验收目标：**内置扬声器可重复播放短时、低音量、可辨识测试音**

本文是当前音频开发的执行文档。`fuxi-audio-porting-plan.md` 解释长期架构，
`fuxi-audio-probe-debug-runbook.md` 记录早期 SMEM 故障隔离方法；两者中“默认入口不可达”“禁止设备操作”
等历史状态不再描述当前分支。当前分支已经通过 `fuxi-adsp=1` 运行只读固件 inventory，并已获设备所有者授权使用
UniXNU、CDC 和 Fastboot 继续测试。仍然禁止 Android boot 和 ADB。

## 1. Agent 的任务边界

Agent 不是重新设计一套音频栈。任务是把 Sheng 已验证的 SM8550 音频机制接到 Fuxi 已存在的公共层和板级 binding，
尽快形成下面这条最短闭环：

```text
modem_a/modem_b 的 adsp_dtb + adsp
  -> MDT metadata / PAS / carveout
  -> SMP2P ready + IPCC + QMP + GLINK
  -> GPR adsp_apps + SPF/APM ready
  -> speaker AudioReach graph + shared PCM buffer
  -> secondary MI2S/TDM
  -> 两颗 CS35L41 muted-ready
  -> 短时低音量 test tone
```

首次出声不要求麦克风、电话、蓝牙、完整 CoreAudio、休眠恢复、SSR 长稳或日用音质。不要把这些工作混入本轮。

### 1.1 一天内的建议节奏

| 时间盒 | 工作 | 必须得到的证据 |
| --- | --- | --- |
| 0–2 小时 | 将 Sheng ADSP owner 机制接入 Fuxi hooks | mode 2 不再报 `hook mask != 0x7f`；PAS 各阶段有精确状态 |
| 2–4 小时 | 固件 placement、PAS 启动、SMP2P ready | `stage=11`，无 fatal、panic、SMMU fault |
| 4–6 小时 | GLINK、`adsp_apps`、GPR/SPF ready | `stage=12/13`，有 RX/TX、channel up、SPF ready 回包 |
| 6–8 小时 | speaker graph、静音 period | graph open/config/start 成功，period submitted=completed |
| 8–10 小时 | CS35L41 muted-ready、短 tone | 两颗 ID/PLL/fault PASS；单颗再双颗短音测试 |
| 10–12 小时 | 重复性和回归 | 5 次播放/停止，GPU/Metal、CDC、Data mount 无回退 |

如果某一阶段超过两个连续刷机轮次没有增加新的可观察证据，停止盲试，先扩充精确状态字段或日志。

## 2. 当前真实基线

### 2.1 已完成

- `FuxiAudioBinding` 已从 `facts/audio/binding.toml` 生成；当前为 131 个字段：73 unverified、58 chosen、0 verified。
- `libFuxiAudioRuntime.a` 已独立构建并 force-load。
- `FuxiAdspFirmware`、`FatVolume` 和 NativeUfs 只读分区接口已接通。
- 真机已经从设备本地 `modem_a` 或 `modem_b` FAT `/image` 成功打开并解析：
  - `adsp_dtb.b00...`
  - `adsp.b00...`
- 第一次 inventory 曾因 `native_ufs_part_find()` 在内核栈分配约 20 KiB GPT scratch 而触发
  `__chkstk_darwin_probe`；该缓冲已移到 static storage，函数栈帧约 128 bytes。
- inventory 已改成独立、有限期 worker：等待 NativeUfs ready，最多 30 秒，不阻塞 `bsd_init`、root mount 或 CDC。
- 真机 CDC session `3390f937e8ccc86b` 的 `DIAGSTATUS resources=0x1b1f00` 证明：
  - 解析到 `fuxi-adsp=1`；
  - runtime 进入并返回；
  - stats ABI/result 已发布且 `last_error=0`；
  - `adsp_dtb` 和 `adsp` 两个 MDT 都解析成功。
- 同一轮 NativeUfs 发布 SystemOS/OS，launchd 报告 Early boot complete；GPU 启动成功，初始 fence `9/9`。

当前已验证镜像身份：

```text
kernel.macho sha256 118693cb37ab2b5e504e074c998f68fc271d6ab203a0146c368d6df5807aab77
boot.img    sha256 fe385baa95bb353c93c4c975708f34f0552f70cbe3d0992c88f7b464e001052a
CDC session        3390f937e8ccc86b
```

### 2.2 尚未完成

`FuxiAdspProbe.cpp` 的 mode 2 已完成七类 hook 的真机 bring-up：

```text
firmware_source, map_carveout, PAS, SMP2P, IPCC, QMP, GLINK
```

最新 `R-GRAPH-02` 证明严格 PAS 0/0、ADSP ready、GLINK，以及静音设备子图 OPEN、48 kHz/16-bit/stereo endpoint SET_CFG、CLOSE 均返回 0；CDC、Metal、GOP 仍正常。`GET_SPF_STATE` 依旧返回 `ADSP_EFAILED`，但 graph 命令已由实际返回码验证。尚无静音 PCM period、CS35L41 真机身份/PLL/fault 或扬声器声音证据。graph 试验须显式使用 `fuxi-graph-probe=1`；默认启动不执行 graph 探测。

## 3. 工作区和参考提交

```text
设备仓库  /Users/mac/Documents/deepseek-harness/default-workspace/unixnu-fuxi
构建仓库  /Users/mac/Documents/deepseek-harness/default-workspace/UniXNU
UEFI      /Users/mac/Documents/deepseek-harness/default-workspace/uefi-fuxi
Sheng     /Users/mac/Documents/deepseek-harness/compare/UniXNU-sheng
```

设备仓库使用 `V2` 分支，当前存在大量未提交音频改动；`drivers/NativeUfs` 子仓库也有本轮修改。Agent 必须保留这些改动，
不得 `reset --hard`、`checkout --`、清理未跟踪文件或替换整个文件。编辑使用 `apply_patch`，先查看 diff，再做局部修改。

Sheng 音频线共有 19 个不重复的功能/修复提交；本轮最相关的是：

| 提交 | 用途 |
| --- | --- |
| `5823baf` | ADSP/SMEM/SMP2P/GLINK/QMP/RPMh 协议核心和 host tests |
| `34c8b2f` | Sheng ADSP 启动与 GLINK owner 实现 |
| `a3a6e1d`、`cd0248f` | GPR codec 与 SPF readiness |
| `2fa7290` | 从 firmware partition 启动 DSP、QRTR/IPC router |
| `cc92795` | speaker graph、功放、共享音频服务和 HAL 的首次完整输出路径 |
| `d5a7d84` | GLINK 大 intent 修复 |

重点阅读：

```text
UniXNU-sheng/package/AdspProbe.cpp
UniXNU-sheng/package/AudioProbe.cpp
UniXNU-sheng/package/AdspFirmware.c
UniXNU-sheng/package/ShengAudioReach.c
UniXNU-sheng/package/ShengCs35l43.c
```

不要整体复制 `AdspProbe.cpp` 或 `AudioProbe.cpp`。公共协议文件多数已经存在，板级差异必须留在 Fuxi owner glue。

## 4. Sheng 到 Fuxi 的代码映射

| Sheng | Fuxi 目标 | 处理方式 |
| --- | --- | --- |
| `AdspFirmware.c`、`FatVolume.*` | `FuxiAdspFirmware.*`、`FatVolume.*` | 已完成；继续使用设备本地 split MDT |
| `QcomMdt.*` | `QcomMdt.*` | 字节一致；不要重写 |
| `QcomSmem.*` | `QcomSmem.*` | Fuxi 有加固版；不要用 Sheng 覆盖 |
| `QcomSmp2p.*` | `QcomSmp2p.*` | 字节一致；直接使用 |
| `QcomGlink.*` | `QcomGlink.*` | 字节一致；直接使用 |
| `QcomQmp.*`、`QcomRpmh.*` | 同名文件 | 字节一致；直接使用 |
| `ShengGpr.*` | `ShengGpr.*` | 字节一致；直接使用 |
| `AdspProbe.cpp` owner 部分 | `FuxiAdspProbe.cpp` | 移植 mapping、SCM、SMP2P/IPCC、QMP、GLINK 和事件线程 |
| `ShengAudioReach.*` | `QcomAudioGraph.*`、`QcomAudioPath.*` | 已参数化；不要再增加第二套 builder |
| `ShengAudioService.cpp` | `QcomAudioService.*` | 使用现有服务状态机 |
| `ShengCs35l43.*` | `CirrusCs35l41.*` + `Cs35Run.*` | 只复用 runner 形状；严禁复制 CS35L43 寄存器 |
| `ShengAudioPlugIn.c` | `hal/FuxiAudioPlugIn.c` | 已有 Fuxi 身份；首次出声阶段可暂不依赖 HAL |

## 5. 实施顺序

### 阶段 A：先完成 ADSP owner hooks

只修改 Fuxi owner glue 和必要的构建脚本，不改变公共协议层。

#### A1. 保留并扩展现有 runtime 状态

使用 `native_fuxi_adsp_stats[16]` 记录可通过 CDC 查询的稳定字段。至少能够区分：

```text
ABI / mode / overall status / exact errno
hook mask / current stage / failed stage
DTB/ADSP image size and segment count
last SCM x0/x1 and PAS operation
SMP2P inbound bits
IPCC interrupt count
GLINK version, RX/TX, channel-up and unknown-open count
GPR/APM last opcode and status
```

不要只打印早期 `kprintf`。CDC 晚于早期启动，关键结果必须保存在 stats 中。

#### A2. 从 Sheng 移植 mapping 和事件所有权

在 `FuxiAdspProbe.cpp` 建立一个 owner context，持有所有 map、Qcom 对象、image 和 worker 状态。遵守：

- 一个 kernel thread 拥有 ADSP 状态机；IRQ 只 ack、置位 event、唤醒线程。
- 不在 IRQ 中分配、解析、等待、发大日志或执行 SCM。
- 所有等待有 deadline；不得无限轮询。
- cleanup 逆序执行，失败时保留精确 stage/error。
- `mapWindow()`、cache attribute、carveout base/size 从 Fuxi binding/事实取得，不把值重新散落到代码里。

当前候选 carveout 与 Sheng 同为：

```text
ADSP     0x9ea00000 + 0x04680000
ADSP DTB 0x9e980000 + 0x00080000
PAS IDs  ADSP=1, ADSP_DTB=36
```

这些值目前是 `sheng.sm8550` prediction。首轮可作为明确标注的候选值测试，但不得在文档中提前改成 verified；只有 PAS、
placement 和 ready 证据吻合后才能升级。

#### A3. PAS 顺序必须与 Sheng 一致

分别对 DTB 和 ADSP 执行，错误不得折叠：

1. `qcom_mdt_parse()`；
2. 分配物理连续 metadata；
3. `qcom_mdt_metadata()`；
4. clean metadata to PoC；
5. `QCOM_SCM_PAS_INIT_IMAGE`；
6. relocatable image 执行 `QCOM_SCM_PAS_MEM_SETUP`；
7. 将 loadable segments 放入各自 carveout，BSS 清零并完成必要 cache maintenance；
8. DTB `AUTH_AND_RESET`；
9. ADSP `AUTH_AND_RESET`；
10. 等待 SMP2P ready，而不是用固定 sleep 宣称成功。

沿用 Sheng 的 interrupted/resume 和 `QCOM_SCM_V2_EBUSY` 有界重试形状。每次 SCM 保存 `x0/x1`。任何 SCM 错误停止本轮，
不得继续发 QMP 或打开 graph。

#### A4. SMP2P、IPCC、QMP、GLINK

先对照 Fuxi DTB 和已有 binding，再用 Sheng 值作为同 SoC 候选：

```text
SMEM                 0x81d00000 + 0x200000
TCSR mutex slot 3    0x1f40000 + 3 * 0x1000
remote pid           2
SMP2P in/out          item 443 / 429
crash reason          item 423
GLINK descriptor/TX/RX item 478 / 479 / 480
GLINK FIFO            0x4000
IPCC                  0x408000, SPI 229
AOSS message RAM      0x0c300000 + 0x400
apps RSC              0x17a20000, TCS block +0xd00, SPI 5
command DB            0x81c60000 + 0x20000
```

已知真机只读事实：APPS/ADSP partition 存在，item 478 和 479 已存在；429、443、423、480 在早期只读快照中未分配。
启动路径需要在 TCSR mutex 下按现有 `QcomSmem` 协议分配缺失项，然后建立 SMP2P 和 GLINK。不要用无锁写，也不要清零
allocator 未授权的 payload。

ADSP 阶段验收：

```text
stage >= 11 (ready)
fatal bit = 0
last SCM x0/x1 = 0/0
crash reason empty
CDC PING still works
```

GLINK/GPR 阶段验收：

```text
GLINK version agreed
adsp_apps channel up
RX > 0 and TX > 0
SPF/APM framework ready reply received
stage >= 13
```

### 阶段 B：静音 speaker graph

不要先解 mute。先证明 DSP 控制面和 buffer 闭环。

1. 用现有 `QcomAudioTopology` 从 binding 生成 speaker graph。
2. 用 `QcomAudioGraph` 执行 open、endpoint config、PCM format、prepare、start。
3. 使用低于 4 GiB、物理连续的共享 buffer；保存 PA、DSP address、map handle 和实际返回码。
4. 先提交全零 PCM period；至少完成 32 个 period。
5. stop、close、unmap 必须全部返回成功，资源计数归零。

首轮候选格式：

```text
48 kHz, signed PCM, 16-bit, stereo
secondary MI2S/TDM
slot B = mask 0x1
slot T = mask 0x2
2 slots
```

这些是设计选择/源码候选，不是最终 verified。若 graph 接受但无 TDM clock，应先检查 PRM clock、endpoint instance、接口号和
pinmux，不要通过随机更换 module ID 试错。

静音数据面 PASS：

```text
graph open/config/prepare/start = 0
buffer map handle != 0
submitted == completed >= 32
no timeout, stale completion or SMMU fault
stop/close/unmap = 0
```

### 阶段 C：两颗 CS35L41 达到 muted-ready

使用 `GeniI2c`、`CirrusCs35l41` 和 `Cs35Run`。禁止把 Sheng 的 CS35L43 表写到 Fuxi。

板级候选：

```text
I2C controller  0x00880000, 400 kHz
B amp           address 0x40, reset GPIO183, IRQ GPIO99
T amp           address 0x42, reset GPIO182, IRQ GPIO98
MI2S pins       SCK 121, WS 123, DATA_IN 122, DATA_OUT 124
```

每颗功放分别执行：reset -> ID/revision -> sticky fault -> ASP/TDM -> PLL -> mute。只有满足下列条件才允许 unmute：

```text
I2C ACK
device ID and revision match binding
critical EINT bits clear
PLL lock observed
state == Cs35l41MutedReady
```

任意一颗失败时保持两颗 mute，本轮不得“先听另一颗”。先保存 address、ID、revision、EINT1、EINT2 和失败 step。

### 阶段 D：首次出声

首次出声不依赖 CoreAudio HAL。由内核音频服务生成确定性 PCM，避免同时调试 HAL。

测试音约束：

- 1 kHz 正弦波；
- 48 kHz / 16-bit；
- 初始幅度不高于 full scale 的 1/32（约 -30 dBFS）；
- 每次 1 秒，最多 3 次；
- 先 B 单颗、再 T 单颗，确认无异常后才双颗；
- 每次开始前读取 fault，每次结束立即 mute 并再次读取 fault；
- 不使用低频、满幅、扫频、长时间循环或系统最大音量。

推荐状态顺序：

```text
graph running with zero periods
 -> both amps muted-ready
 -> select one amp and unmute
 -> queue bounded tone periods
 -> wait for all completions
 -> mute amp
 -> stop/close/unmap graph
 -> read final amp faults
```

首次出声 PASS 不是“日志看起来正常”，而是同时满足：

1. 设备所有者实际听到可辨识短 tone；
2. B、T 至少分别完成一次单独 tone；
3. tone period 全部完成，submitted=completed；
4. 两颗功放最终均为 muted，无 critical fault；
5. 无 panic、reset、SMMU fault、UFS/Data mount 回退；
6. CDC PING 正常；
7. Metal fence submitted=retired，GOP offer=done。

完成后重复完整 start/tone/stop 5 次。5/5 成功才将“扬声器出声”记录为 PASS；单次声音只记为 provisional。

## 6. Host 检查

在设备仓库运行：

```sh
cd /Users/mac/Documents/deepseek-harness/default-workspace/unixnu-fuxi

python3 scripts/check-qcom-mdt-host.py
python3 scripts/check-qcom-a0-host.py
python3 scripts/check-qcom-loader-host.py
python3 scripts/check-qcom-adsp-host.py
python3 scripts/check-qcom-adsp-client-host.py
python3 scripts/check-gpr-host.py
python3 scripts/check-qcom-audio-graph-host.py
python3 scripts/check-qcom-audio-path-host.py
python3 scripts/check-qcom-audio-topology-host.py
python3 scripts/check-qcom-audio-service-host.py
python3 scripts/check-geni-i2c-host.py
python3 scripts/check-cirrus-cs35l41-host.py
python3 scripts/check-fuxi-audio-runtime-binding.py
python3 scripts/check-fuxi-audio-records.py
```

需要全量汇总时：

```sh
python3 scripts/report-fuxi-audio-status.py
```

任何 host FAIL 先修复再构建。Host PASS 只证明状态机/报文形状，不等于真机 PASS。

## 7. 编译和打包

只使用下面这组已验证命令。不要使用 `out/ipsw/21H461/data.dbr`，它的 Data UUID 与内核配置不匹配。不要省略
GPU addon 或 owner trust cache，否则会破坏已经稳定的 GPU/Metal。

```sh
cd /Users/mac/Documents/deepseek-harness/default-workspace/UniXNU

./unixnu defconfig fuxi_defconfig -D ../unixnu-fuxi

./unixnu build kernel -D ../unixnu-fuxi \
  > /tmp/fuxi-audio-kernel.log 2>&1
echo "kernel=$?"
grep -n ' error:' /tmp/fuxi-audio-kernel.log | head -20

./unixnu build payload -D ../unixnu-fuxi --uefi ../uefi-fuxi \
  --unique-chip-id 0x0123456789abcdef \
  --owner-trust-cache out/fuxi/ipados-17.7.11-21H461/fuxi/tree/build/adreno-metal-plugin/owner-trust.bundle \
  --data-ramdisk out/fuxi/build/owner-data-fixture/owner-data-16g.dbr \
  --gpu-addon out/fuxi/ipados-17.7.11-21H461/fuxi/tree/build/fuxi-gpu-addon.ngpu \
  > /tmp/fuxi-audio-payload.log 2>&1
echo "payload=$?"

./unixnu bootimg -D ../unixnu-fuxi --uefi ../uefi-fuxi --target DEBUG \
  --dtb ../uefi-fuxi/Platforms/Xiaomi/fuxiPkg/FdtBlob/sm8550-xiaomi-fuxi.dtb \
  > /tmp/fuxi-audio-bootimg.log 2>&1
echo "bootimg=$?"
```

打包后检查：

```sh
cd /Users/mac/Documents/deepseek-harness/default-workspace/UniXNU

jq '{kernel_args, data_volume_uuid, gpu_addon_sha256, trust_cache_sha256}' \
  out/fuxi/ipados-17.7.11-21H461/fuxi/payload/payload.json

shasum -a 256 \
  out/fuxi/ipados-17.7.11-21H461/fuxi/kernel/kernel.macho \
  out/fuxi/ipados-17.7.11-21H461/fuxi/payload/payload.img \
  out/fuxi/ipados-17.7.11-21H461/fuxi/boot/boot.img
```

必须确认：

- `data_volume_uuid = B2A023C5-CC73-45C1-A9E6-26AC8BDD2822`；
- `gpu_addon_sha256` 和 `trust_cache_sha256` 非空；
- inventory 轮使用 `fuxi-adsp=1`；
- 真正 ADSP bring-up 轮只有在 7 个 hooks 全部安装后才切到 `fuxi-adsp=2`；
- 首声开关应独立于 `fuxi-adsp`，建议 `fuxi-audio=1`，且默认关闭自动循环播放。

## 8. Fastboot 和 CDC 真机流程

### 8.1 先判断设备在哪个模式

```sh
ls /dev/cu.usbmodem* 2>/dev/null
fastboot devices -l
```

- 出现 `/dev/cu.usbmodem*`：设备在 XNU CDC。
- `fastboot devices -l` 有序列号：设备在 Fastboot。
- 两者都没有：先检查 USB 枚举和线缆，不要继续发送命令。

### 8.2 从 XNU CDC 进入 Fastboot

```sh
cd /Users/mac/Documents/deepseek-harness/default-workspace/unixnu-fuxi

./scripts/fuxi-usb-debug.sh stop || true
./scripts/fuxi-usb-debug.sh start
./scripts/fuxi-usb-debug.sh ctl ping
./scripts/fuxi-usb-debug.sh ctl reboot-fastboot
```

`reboot-fastboot` 成功时 USB 会立即断开，因此“control socket closed before a complete reply”可以是正常重启表现；随后必须以
`fastboot devices -l` 确认，不能仅凭 socket 断开判断成功。

### 8.3 一次性启动测试镜像

遵守设备仓库规则，开发阶段使用 `fastboot boot`，不要写 Android 分区：

```sh
cd /Users/mac/Documents/deepseek-harness/default-workspace/UniXNU

fastboot devices -l
fastboot boot out/fuxi/ipados-17.7.11-21H461/fuxi/boot/boot.img
```

不要使用 Android boot、ADB 或 vendor ramdisk；不要把音频实验写入其他 boot slot。

### 8.4 收集 XNU 日志

等待 `/dev/cu.usbmodem*` 重新出现，然后：

```sh
cd /Users/mac/Documents/deepseek-harness/default-workspace/unixnu-fuxi

./scripts/fuxi-usb-debug.sh stop || true
./scripts/fuxi-usb-debug.sh start
./scripts/fuxi-usb-debug.sh ctl ping
./scripts/fuxi-usb-debug.sh ctl diagstatus
./scripts/fuxi-usb-debug.sh status
```

若 supervisor 报 `Connection refused`，重新执行一次 `stop`、`start`，不要因此重刷镜像。

定位当前捕获：

```sh
run_dir=$(./scripts/fuxi-usb-debug.sh status 2>&1 | sed -n 's/^run: //p')

rg -n -i \
  'FUXI_ADSP|audio|glink|gpr|apm|cs35|i2c|fuxi-metal|fuxi-display|panic|data abort|smmu.*fault|gpu.*reset|launchd' \
  "$run_dir/xnu-usb.log"
```

每轮至少保存：boot.img SHA、kernel SHA、CDC session、boot args、ADSP/audio stats、最后一个成功 stage、精确 errno/SCM
结果、GPU fence、GOP、panic/fault 状态和捕获目录。

## 9. 分阶段测试矩阵

| 镜像 | boot args | 测试内容 | PASS |
| --- | --- | --- | --- |
| I1 | `fuxi-adsp=1` | 只读 firmware inventory | 两个 MDT parsed、error=0、系统正常启动 |
| A1 | `fuxi-adsp=2`, audio off | mapping + metadata + PAS | SCM=0、stage 至少 10，无 AP panic |
| A2 | 同上 | SMP2P ready | stage=11、fatal=0 |
| A3 | 同上 | GLINK + `adsp_apps` | version agreed、channel up、stage>=13 |
| C1 | ADSP up，speaker graph off | 两颗功放 muted-ready | ID/revision/PLL/fault PASS，仍无声音 |
| D1 | ADSP up，amps mute | graph + zero PCM | 32/32 periods，无 fault |
| T1 | `fuxi-audio=1`，单 B | 1 秒 -30 dBFS tone | 可闻、完成、立即 mute、无 fault |
| T2 | 单 T | 同上 | 可闻、完成、立即 mute、无 fault |
| T3 | B+T | 同上 | 两路可闻、完成、无 fault |
| R5 | B+T | 5 次 start/tone/stop | 5/5，资源归零，GPU/CDC/Data 正常 |

每个镜像只扩大一个危险动作。不要在同一轮首次启用 PAS、GLINK、graph 和 unmute，否则失败后无法归因。

## 10. 失败分流

| 现象 | 首查 | 禁止做法 |
| --- | --- | --- |
| build link error | archive 是否包含新增对象、符号是否强定义、force-load 顺序 | 用 weak stub 掩盖缺失实现 |
| mode 2 立即 `BadConfig` | hook mask 是否为 `0x7f`、binding 字段是否传入 | 删除 gate 强行继续 |
| SCM `INIT_IMAGE` 失败 | metadata size/PA、cache clean、PAS id、x0/x1 | 继续写 carveout 或 auth reset |
| `MEM_SETUP` 失败 | relocatable、base/span、carveout overlap | 放宽边界检查 |
| PAS 后无 ready | SMP2P item/entry、IPCC ack、QMP load-state、crash item | 增加无界 sleep |
| GLINK 无 version | item 478/479/480、FIFO size、IPCC client/signal、cache/barrier | 随机改 channel name |
| GPR 无 SPF reply | `adsp_apps` channel、intent、src/dst domain/port | 直接开始 graph |
| graph basic result 非零 | 保存 opcode/token/status；查 graph/instance/endpoint/PRM | 猜 module ID 并批量改动 |
| zero periods 不完成 | DSP address、map handle、32-bit address、cache、period offset | 解 mute 判断“是否有声音” |
| I2C NACK | controller/pinmux/clock/reset/address | 对未知地址广播写 |
| wrong chip/revision | 保存读值，检查 wire endian/register width | 绕过身份 gate |
| PLL unlocked | 先确认 TDM BCLK/FS 和 ASP 配置 | 解除 mute |
| graph/period 正常但无声 | amp mute、PLL、slot mask、MI2S pins、endpoint interface | 提高增益或使用满幅 tone |
| CDC 消失 | 先查 Fastboot，再查 USB；保留最后捕获 | 反复刷写或假定是音频 fault |
| GPU fence 不退休/GOP 不完成 | 立即回退到上一镜像，比较唯一增量 | 把 GPU 回退当作可接受副作用 |

## 11. 必须停止本轮的条件

遇到以下任一项，立即停止扩大功能，保留日志并回到最后一个 PASS 镜像：

- AP panic、data abort、watchdog reset 或设备反复进入 Fastboot；
- SCM 返回无法解释的非零值；
- ADSP fatal、crash reason、ready 后立即 SSR；
- SMMU fault 或 carveout 越界；
- NativeUfs/Data mount 失败；
- CDC 无法恢复；
- CS35L41 wrong ID/revision、boost short、over-temperature、DC watchdog、amp fault 或 PLL unlock；
- tone 结束后无法重新 mute；
- GPU submitted != retired 或 GOP offers != done。

## 12. Agent 的工作方式

Machine-readable test results are in `docs/fuxi-audio-test-results.json`;
`python3 scripts/show-fuxi-audio-test-results.py` prints the latest run. Add a
new object for every booted image and never rewrite an older one. `PASS_READY`
means ADSP ready, `PASS_GLINK` means the transport is up, and `PASS_GRAPH`
means only the bounded muted graph command succeeded. None means speaker sound.

1. **先读现状再改。** 每轮先看 `git status`、相关 diff、上一轮日志和真实镜像 hash。
2. **一次只关闭一个未知量。** 代码变化和 boot-arg 变化要能对应到唯一预期证据。
3. **证据优先于推断。** “函数存在”“编译通过”“Sheng 相同”不等于设备 PASS。
4. **状态必须可远程读取。** 早期日志可能丢失；重要值放入 CDC 可读 stats。
5. **不复制第二套公共栈。** 优先接现有 `Qcom*`/`Fuxi*`，只移植 Sheng owner glue。
6. **不破坏现有功能。** GPU/Metal、GOP、触摸、Data mount、CDC 是每轮强制回归项。
7. **失败不叠补丁。** 两轮失败无新证据时，先加仪器；不要连续猜常量。
8. **保留可恢复镜像。** 每个 PASS 镜像记录 hash；下一轮只用 `fastboot boot` 测试。
9. **完成后整理记录。** 更新 `porting.toml` 和本 runbook 的执行记录，再整理独立提交；不要把调试日志和无关改动混在一起。

## 13. 最终交付报告模板

```text
Result: PASS / FAIL / INCONCLUSIVE
Kernel SHA:
Boot image SHA:
CDC session / capture:
Boot args:

ADSP:
  firmware inventory:
  PAS DTB / ADSP:
  ready/fatal/stage:
  GLINK version, rx/tx, channel:
  GPR/SPF:

Speaker graph:
  open/config/start:
  buffer PA/DSP address/map handle:
  submitted/completed/timeouts:
  stop/close/unmap:

Amplifiers:
  B address/id/revision/PLL/fault/final mute:
  T address/id/revision/PLL/fault/final mute:

Audible test:
  frequency/format/amplitude/duration:
  B heard:
  T heard:
  combined heard:
  repeat count:

Regression:
  Data mount/launchd:
  CDC PING:
  Metal submitted/retired:
  GOP offer/done:
  panic/reset/SMMU fault:

Next exact blocker:
```

只有当“可闻证据 + period 完成 + 最终 mute + 平台回归”同时成立，Agent 才能报告“扬声器出声 PASS”。
