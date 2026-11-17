# fuxi 音频移植 · 交接指南

> 面向下一个接手这个项目的人。目标：让你在 30 分钟内知道**项目在哪、怎么编译、怎么上真机、哪里会踩坑**。
> 配套文档：`docs/fuxi-audio-porting-log.md`（逐阶段真机证据与失败记录）。

---

## 1. 项目是什么

- 机器：**Xiaomi 13 `fuxi`（SM8550）**，跑 **XNU（Darwin 23.6.0）**，用户态是 **iPadOS 17.7.11 21H461**（arm64，iPad7,1 身份）。
- 目标：把音频打通（音频移植计划的 P0–P8 阶段）。
- 代码仓：
  - 设备仓：`/Users/mac/Documents/deepseek-harness/default-workspace/unixnu-fuxi`
  - 构建器：`/Users/mac/Documents/deepseek-harness/default-workspace/UniXNU`（fork `OddlyLabs/UniXNU` branch `fuxi`）
  - UEFI：`/Users/mac/Documents/deepseek-harness/default-workspace/uefi-fuxi`
  - 参考实现：`/tmp/kerfand-fuxi`（Kerfand 的 iOS17 移植，**同机型**，参数可信）

## 2. 现在到哪了

| 阶段 | 状态 | 证据 |
|---|---|---|
| P0 基线 | ✅ | ADSP `stage=13`、SMMU 审计 slot 7 / bank 4 |
| P1 DSP 传输 | ✅ | GPR/GLINK 通、图能 open |
| P2 AudioReach 图 | ✅ | `GRAPH_PREPARE`、`GRAPH_START` 回复 status 都是 **0** |
| P3 低 DRAM buffer + SMMU bypass | ✅ | `pcm_pa = 0xd0000000`、span 64 KiB、bypass 已配、不复位 |
| P4 CS35L41 功放 | 🔄 进行中 | 探针只读版安全；QUP2 SE0 寄存器全 0 → 未上时钟 |
| P5 第一声 | ⏸ | 依赖 P4 |
| P6–P8 | ⏸ | — |

### P4 当前的确切卡点

`probeOwnedAmp()`（`package/FuxiAdspProbe.cpp`）只读访问 QUP2 SE0（`0x00880000`）：

- `mapWindow` 成功（标记 `0xCC`）、**不复位**；
- 但 `GENI_FW_REVISION` / `GENI_CLK_CFG` / `GENI_SCL_COUNTERS` **全读 0**；
- 说明 SE 块**没上时钟**，所以 `geni_i2c_prepare()` 必然失败（`proto=0 ≠ I2C`）。

**下一步就是给它上时钟**（GCC），参数见第 7 节。

## 3. 编译流程

全部在 `UniXNU` 目录下执行，`-D ../unixnu-fuxi` 指向设备仓。

```sh
cd /Users/mac/Documents/deepseek-harness/default-workspace/UniXNU

# 1) 生成配置（只在 board.toml/components 改动后需要）
./unixnu defconfig fuxi_defconfig -D ../unixnu-fuxi

# 2) 内核（改了 package/*.c/*.cpp/*.h、drivers、补丁时跑；约 2–5 分钟）
./unixnu build kernel -D ../unixnu-fuxi

# 3) payload（改了 board.toml 的 kernel_args / 组件时跑；约 1–2 分钟）
./unixnu build payload -D ../unixnu-fuxi --uefi ../uefi-fuxi \
  --unique-chip-id 0x0123456789abcdef \
  --owner-trust-cache out/fuxi/ipados-17.7.11-21H461/fuxi/tree/build/adreno-metal-plugin/owner-trust.bundle \
  --data-ramdisk out/fuxi/build/owner-data-fixture/owner-data-16g.dbr \
  --gpu-addon out/fuxi/ipados-17.7.11-21H461/fuxi/tree/build/fuxi-gpu-addon.ngpu

# 4) 打包 boot 镜像（约 10 秒）
./unixnu bootimg -D ../unixnu-fuxi --uefi ../uefi-fuxi --target DEBUG \
  --dtb ../uefi-fuxi/Platforms/Xiaomi/fuxiPkg/FdtBlob/sm8550-xiaomi-fuxi.dtb \
  -o /tmp/fuxi-<tag>.img
```

**只改 cmdline（`board.toml` 的字符串参数）时**：可以跳过 `build kernel`，只跑 3+4。

**构建失败先看**：
- 内核编译错误：`out/fuxi/.../logs/archive-20-build-fuxi-audio-runtime.log`
- 链接错误：`out/fuxi/.../logs/kernel.log`

## 4. 刷机与启动

```sh
# 设备进 fastboot 后
fastboot -s b4c62a1e flash boot /tmp/fuxi-<tag>.img
fastboot -s b4c62a1e reboot
```

- **不要用 `fastboot boot`**：这台机器会报 `Failed to load/authenticate boot image`，必须 `flash boot`。
- 设备序列号 `b4c62a1e`；fastboot 产品名显示 `Android`。
- 从 XNU 里进 fastboot：`./scripts/fuxi-usb-debug.sh ctl reboot-fastboot`（控制通道活着时才有效）。

## 5. 串口 / 控制通道

```sh
cd unixnu-fuxi
./scripts/fuxi-usb-debug.sh start     # 启动 supervisor（自动跨重启重连）
./scripts/fuxi-usb-debug.sh status
./scripts/fuxi-usb-debug.sh ctl ping
./scripts/fuxi-usb-debug.sh ctl diagstatus   # 读 16 页诊断
```

- 日志：`/tmp/fuxi-usb/run-<时间>/xnu-usb.log`
- **CDC 控制通道经常卡死**（QCOM USB LOSS）：`ping` 超时、`diagstatus` 返回空。
  处理：`stop` → `start` 重启 supervisor；再不行就等/重启设备。
- **早期日志会丢**：`bsd_init` 阶段的 `kprintf` 发生在 CDC 枚举之前（约 30 s），
  collector 接得晚就看不到。所以**调试信息要写进 DIAGSTATUS**，不要只靠 kprintf。

### DIAGSTATUS 的读法（非常重要）

`ctl diagstatus` 的 `resources` 字段是 **round-robin** 的：每调用一次页号 +1（0..15）。
所以**必须连续采样 20 次**才能覆盖全部 16 页：

```python
rows = {}
for _ in range(20):
    r = ctl('diagstatus')            # 解析 JSON 的 fields.resources
    rows.setdefault((resources >> 16) & 0xf, (resources >> 20) & 0xfff)
```

页的分配见 `package/FuxiAudioProbe.cpp` 的 `fuxi_audio_diag_resources()`。

## 6. 必踩的坑（每条都真实发生过）

1. **cmdline 上限 1000 字节**。超了 `build payload` 直接报错。加参数前先算长度；可删冗余参数（例如 `fuxi-cdc-stale=0` 就是默认值）。
2. **诊断发布顺序**：`syncStats()` 里审计块如果排在诊断块之后，会把你的状态页覆盖成 SMMU 的 IDR0/IDR1/IDR2。
   我因此连续十几轮误判“prepare 被拒绝”。**新加的诊断块一定放在所有会写同一页的块之后。**
3. **`Cs35l41 amp;` 必须 `= {}`**：未初始化栈会读出“device_id=0x5f8”这种垃圾，误导排查方向。
4. **`IODeviceMemory::map()` 要带 `kIOMapAnywhere`**：只写 `kIOMapWriteCombineCache` 会映射失败。
5. **SMMU 只用 bypass，不要写 translating context**：后者会复位主板（指南明确）。
6. **对 QUP SE 寄存器写入会导致复位**（实测）：先只读验证，再逐步加写。
7. **`fuxi-panel-black=1` 会让整块屏黑**（含 UI、日志、诊断、pulse）。要恢复界面就删掉它。
8. **看门狗**：`fuxi-cdc-watchdog=N`（默认 3 s，IO 故障复位）；`fuxi-cdc-stale=N`（控制卡死，默认关，**开着会把 host 侧卡死变成启动循环**）。

## 7. 可复用的 Kerfand 参数（同机型 SM8550）

来源：`/tmp/kerfand-fuxi/device/package/{AudioProbe.cpp,FuxiCs35l41.h,TouchBoard.h}`

| 项 | 值 | 说明 |
|---|---|---|
| GCC 基址 | `0x00100000` | SoC 级，直接用 |
| GCC vote enable | `0x52008`，bits 18/19/20/21/26(27) | QUP wrap 的 AHB/SE 投票 |
| GPLL0 enable / mode | `0x52018` / `0x0`（bit31 = locked） | 先使能再等锁 |
| QUP halt 寄存器 | `0x2327c/0x23280/0x23284/0x23294/0x184e4` | 清 halt 位 |
| RCG（DFS） | `0x184f0`（S4）、`0x18628`/`0x1e010`（SE） | `PERF_CFG(l)=0x1c+4l`、`M=0x5c+4l`、`N=0x9c+4l` |
| TLMM 基址 | `0x0f100000`，stride `0x1000` | GPIO |
| I2C SE 初始化 | `0x28|=0x7f`、`0xe30|=0xf`、`0x24=0x7f`、`0x20=1`、`0xe18=0` | 再写 CLK_CFG/SCL |
| I2C CLK_CFG / SCL | `0x21` / `0x52C016`（5<<20\|11<<10\|22，400 kHz @19.2 MHz） | |
| 功放 B / T | I2C `0x40`/`0x42`，reset `183`/`182`，IRQ `99`/`98` | 与我们的 binding 一致 |
| PLL lock 位 | `EINT2 bit 1`（`0x2`） | 我们已修正 |
| I2C 引脚 | SDA/SCL = GPIO `0`/`1`，function 2、上拉 | |

**不可复用**：运行时寄存器值（必须本机读）、DFS level 索引（要从 RCG 现算）。**不要**覆盖 `0x7c`（DFS level），那是 bootloader 设好的。

## 8. 当前镜像与参数

`board.toml` 当前关键参数：

```
fgraph=6            # 持久图（不 close）
fuxi-abyp=1         # 音频流 SMMU bypass
fuxi-amp=7          # 功放阶段（非 0 即开；7 是历史值，等价于开）
fuxi-panel-black=1  # 全屏黑
```

**要恢复 iOS 界面**：删掉 `"fuxi-panel-black=1",` → `build payload` + `bootimg` → 刷机。
所有黑屏门控（`BootProgress.c` 的 `draw()`、`touch_overlay()`、`bp_paint_off`，
以及 `NativeLegacyDisplay.cpp` 的 `forceBlack`）都由这一个参数驱动，删掉即全部恢复。

## 9. 下一步该做什么（按顺序）

1. **给 QUP2 SE0 上时钟**：按第 7 节实现 GCC 的 GPLL0 → vote → halt 清零 → RCG，
   **分步上线**（先读、再清 halt、再投票），每步真机验证不复位。
2. 读到 `GENI_FW_REVISION != 0` 后，再启用第 7 节的 SE 初始化 + `geni_i2c_prepare`。
3. `cs35l41_step` 走完 → 两颗 `MutedReady` → **P4 PASS**。
4. P5：把 PCM 数据写进 `0xd0000000`（图已 start），验证 DSP 消费。

## 10. 交付纪律

- 每个阶段要有**真机证据**（DIAGSTATUS 页值 / 串口行 / 不复位），写进 `docs/fuxi-audio-porting-log.md`。
- 失败与反转也要写（这个项目里“误判”比“真 bug”多）。
- 改 `board.toml` 的参数要同步记录，否则下一个人不知道当前镜像是什么。
- **不做无谓的测试**。结果已知的检查、缺工具链/缺 fixture 而跑不起来的套件、owner 已在真机上验收过的东西的重复构建，
  都只会浪费时间，不产生新信息。改动只是搬移、拆分、指针更新或提交时，用**读代码树和历史**来验证，而不是靠跑一遍。
  只有当测试本身就是被改动的对象、当它从未跑过、或者 owner 明确要求时才跑。交付时要讲清楚：哪些检查跑了，哪些没跑。
