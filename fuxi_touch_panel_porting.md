# 小米 13（fuxi）触控与面板驱动资料（面向 UniXNU 移植）

> 整理时间：2026-10-31。所有数据都来自官方固件、设备树、真机实测或开源驱动，每一项都注明出处。只是没有经过 UniXNU 上机验证。
> 配套文件：`drivers/FuxiFtsTouch/`（触控驱动，第 7 节）、`package/FuxiTouchProbe.cpp`（内核探针）、`package/Fuxi*Board.h`（板级常量，已通过主机测试）、`facts/`（机器可读事实与 73 组面板命令序列）、`dtb/`（合并后的完整设备树）、`firmware/`（触控固件）、`dts/fuxi.dts`（UniXNU 设备树草稿）。

## 0. 结论

| 项目 | 与 Pad 6S Pro（sheng，UniXNU 已跑通）的关系 | 工作量 |
|---|---|---|
| 触控 SPI 控制器、时钟、引脚、中断 | **完全相同**：SPI4 @0xa90000，wrapper 0xac0000，SID 0xa3，GPIO44-47，中断 GPIO25（TLMM 汇总中断 SPI 208） | 直接复用 `QcomGeniSpi`、`TouchBoard.h` 里的时钟和引脚代码 |
| 触控芯片 | **不同**：fuxi 是 ST FingerTip（FTM5，芯片 ID 36 40），sheng 是 Novatek | 要写新的协议层（第 2 节的协议已经整理完整） |
| 触控固件 | **更简单**：FTS 芯片自带 flash，开机不用像 Novatek 那样由主机下载固件 | — |
| 触控电源 | **已实测（第 6 节）**：两路只供触控，安卓里由触控驱动打开后常开；按"XNU 接管时是关的"处理。RPMh 地址已确定：vdd `ldob12` = 0x40900，avdd `ldom6` = 0x41b00 | 加 5 条 RPMh 投票（`FuxiTouchBoard.h` 的 `ftb_power_on_cmds`），之后等 3 ms |
| 第一阶段出图 | **相同**：splash 帧缓冲 0xB8000000 / 0x2B00000，1080x2400x32bpp | 走 `GopFramebuffer` 路线，不用写 DSI 驱动 |
| 原生显示驱动 | 三星 DDIC + DSC 命令模式；命令序列、时序、电源时序都已解码 | 第二阶段才需要 |

## 1. 数据来源与可信度
1. **设备树**：官方 OS3.0.309 线刷包（md5 已校验）里 vendor_boot 的 DTB[2]（"Kalama v2 SoC"，对应本机 `ro.boot.dtb_idx=2`、soc_id 519 rev 2.0），加 dtbo[4]（"8550 fuxi"，msm-id 0x207）。用 `fdtoverlay` 合并，得到与 ABL 启动时一致的完整设备树（`dtb/fuxi_merged.dtb`），所有 phandle 都已解析（`tools/dt_facts.py`）。
2. **触控驱动**：LineageOS `android_kernel_xiaomi_sm8550` 里的 `drivers/input/touchscreen/fts_spi`（GPL，最近一次提交 2025-01-23）。compatible `"st,spi"`、模块名 `fts_touch_spi`、描述 "STMicroelectronics MultiTouch IC Driver" 三项都与 fuxi 的 DT 和 TWRP ramdisk 里的模块一致，可以确认是 fuxi 实际在用的小米版驱动。
   - Qualcomm 的参考驱动 `touch-drivers/st`（compatible `"st,fts"`）**不是**这个，不要拿错。
   - 源码放在 `ref_src/fts_spi/`，仅供查阅。UniXNU 是 BSD-2-Clause-Patent 许可，**不要把 GPL 代码直接贴进去**，按本文的事实重新实现。
3. **引脚和中断换算**：Linux `pinctrl-sm8550.c`（GPIO→PDC 映射）、LineageOS 下游 `pinctrl-kalama.c`（`REG_BASE 0x100000`），以及 DT 中的 `qcom,pdc-ranges`。
4. **交叉印证**：
   - 本机实测：`spi0.0` 绑定 fts，触控坐标范围 0..10799 × 0..23999，屏下指纹位置属性 `442,2052`、大小 196；
   - SurfaceFlinger 上报的 HDR 420 / 0.323 nit 与 DT 一致；
   - 社区 fuxi UEFI 移植（hanzi123-bilp/uefi-fuxi）里的显示保留区和帧缓冲参数与 DT 一致。

## 2. 触控（ST FTS，`/soc/qcom,qupv3_1_geni_se@ac0000/spi@a90000/fts@0`）

### 2.1 硬件连接（详见 `package/FuxiTouchBoard.h`）
| 项 | 值 | 与 sheng |
|---|---|---|
| 总线 | QUP1 SE4 SPI，SE 0xa90000（0x4000），wrapper 0xac0000，SE 中断 GIC SPI 357，SMMU SID 0xa3，时钟 `gcc_qupv3_wrap1_s4_clk` | 相同 |
| SPI | mode 0，**12 MHz**，4 线，拉高 CS 前等 10 µs | 频率不同（sheng 19.2 MHz） |
| 引脚 | MISO 44 / MOSI 45 / CLK 46 / CS 47，function `qup1_se4_l0..l3`，6 mA；CS 上拉，其余下拉 | 相同 |
| 中断 | GPIO25，低电平，上拉 8 mA；TLMM 汇总中断 GIC SPI 208（sheng 用 `32+208`）；走 PDC 唤醒时是 PDC pin 57 → GIC SPI 537 | 相同 |
| 复位 | GPIO24，输出，上拉 8 mA | — |
| 电源 | vdd = RPMh `ldob12`（pm_humu_l12）1.8 V；avdd = RPMh `ldom6`（**pm8010m_l6**，相机 PMIC）3.2 V。上电先 vdd 后 avdd | sheng 不操作电源 |
| TLMM 地址 | GPIO n 控制寄存器 = 0x0F100000 + 0x1000·n，+0x4 IO，+0xC 中断状态（bit0） | 相同 |
| 坐标 | 0..10800 × 0..24000（屏幕的 10 倍，`support-super-resolution`） | — |
| 屏下指纹区 | 中心 (4420, 20520)，边长 1960 | — |

### 2.2 SPI 帧格式
- **写**：一次传输，`[操作码][地址，大端][数据]`。硬件寄存器的地址是 4 字节。
- **读**：**同一次片选**内先发 `[操作码][地址]`，再收 `1 + n` 字节，**第 1 个字节是 dummy，丢掉**。
- 单次读写最多 1024 字节（READ_CHUNK / WRITE_CHUNK）。

| 操作码 | 含义 |
|---|---|
| `FA` / `FB` | 写 / 读硬件寄存器（SPI 形态；I2C 形态下读也是 FA） |
| `A6` / `A7` | 写 / 读 framebuffer |
| `A8` / `A9` | 写 / 读配置内存 |
| `87` | 读事件 FIFO（无地址） |
| `A0 mode settings` | 设置扫描模式 |
| `A2` / `A4` | 设置特性 / 系统命令 |

| 硬件寄存器 | 地址 | 说明 |
|---|---|---|
| 芯片 ID | 0x20000000 | 读 2 字节 = `36 40`（固件 `st_fts_m3.ftb` 头部 target 也是 0x4036） |
| 固件版本 | 0x20000004 | |
| 系统复位 | 0x20000024 | 写 0x81（没有复位脚时才用） |
| IER | 0x20000029 | 0x41 = 使能中断（驱动定义了，但 fuxi 的流程里没有直接写） |
| ICR | 0x2000002D | bit1 = 4 线 SPI（复位后收不到 ready 时设置） |
| CRC 状态 | 0x20000078 | 低 2 位非 0 表示 flash CRC 错误 |

### 2.3 启动流程（与驱动的 `fts_probe` → `fts_init` → `fts_init_sensing` 一致）
1. 配置引脚（2.1），确认 vdd 1.8 V、avdd 3.2 V 已上电（不确定就先投票打开，见 2.6）。
2. **复位**：GPIO24 拉低，等 10 ms，拉高。
3. **等待就绪**：每 2 ms 读一次 FIFO（`87` → 1 + 8 字节），直到某个事件的第 0 字节是 `0x03`（CONTROLLER_READY），超时 100 ms。复位加等待最多重试 3 次。
4. **收不到 ready**：写 ICR（0x2000002D）的 bit1 切到 4 线 SPI，等 1 ms，然后每隔 10 ms 读一次芯片 ID，最多 3 次，应为 `36 40`。读到说明芯片在，但 flash 可能没有固件。
5. （可选）读 System Info（framebuffer 里的系统信息区），拿固件版本和配置 ID。
6. **开始扫描**：`A0 00 00` → 等 50 ms → `A0 00 01`；唤醒或恢复路径用 `A0 00 FF`。
7. 开中断（GPIO25 低电平）。

### 2.4 中断与事件
- 中断到来时，先读 1 个事件（`87` → 1 + 8 字节）。**第 7 字节低 5 位**是 FIFO 里剩下的事件数 N（最多 31），N 大于 0 时再一次性读 `8·N` 字节。每个事件按第 0 字节分派。
- 触摸事件（`0x13` 按下 / `0x23` 移动 / `0x33` 抬起）的解码，fuxi 用的是超分辨率格式：
  - `type = ev[1] & 0x0F`（1 手指、2 手套、3 触控笔、4 手掌、5 悬停）
  - `id = ev[1] >> 4`（0–9）
  - `x = ev[3] << 8 | ev[2]`，`y = ev[5] << 8 | ev[4]`；**除以 10** 就是屏幕像素
  - `major = ((ev[0] & 0x0C) << 2) | (ev[6] >> 4)`，`minor = ((ev[7] & 0xC0) >> 2) | (ev[6] & 0x0F)`
- 其它事件：`0x43` 状态（`ev[1]=0x01` 是命令回显），`0x53` 用户事件（手势、接近等），`0xF3` 错误（`ev[1]`：0x06 看门狗，0x20–0x23 / 0xA0–0xA6 是 CRC 错误，0xF0 是 ESD）。
- **按第 0 字节的高 4 位分派**（驱动 `eventId = data[0] >> 4`）：第 0 字节的 bit2–3 还带着接触面积 major 的高位，所以 0x13、0x17、0x1B、0x1F 都是按下事件。只认整字节 0x13 会漏事件（早期版本的头文件就犯了这个错误，已改正）。
- 现成实现：`drivers/FuxiFtsTouch/FtsTouchCore.c`（`fts_decode_point`、`fts_touch_apply`），测试在 `drivers/FuxiFtsTouch/Test/`。

### 2.5 固件
- `/odm/firmware/st_fts_m3.ftb`：139728 字节，头部 `55 AA 55 AA`，ftb v1，target 0x4036，fw_id 0x6335，fw_ver 0x53，cfg_ver 0x42。已从官方 odm 取出，放在 `firmware/`；产线测试阈值是 `stm_fts_production_limits.csv`。
- 芯片自带 flash，**正常开机不需要下载固件**。安卓驱动开了 `FW_UPDATE_ON_PROBE`，只在文件版本更新时才写 flash。UniXNU 前期**不要实现写 flash**，以免把触控刷坏。

### 2.6 风险与待实测项
1. **电源**（已实测，见第 6 节）：两路都只供触控，DT 里没有 boot-on/always-on，安卓里由触控驱动打开。所以 XNU 接管时**应按关着处理**：访问芯片前，先向 RPMh 投 `ftb_power_on_cmds` 里的 5 条命令，等 3 ms，再配 GPIO、复位。即使电源其实已经开着，重复投票也无害。sheng 包里的 `QcomRpmh.c` 可以拿来发命令。
2. 坐标轴方向：DT 没有 x/y 翻转属性，驱动默认不翻转，以实机为准。
3. DT 里有 21 个小米私有调参属性（边缘抑制、点击灵敏度、跟手性等），LineageOS 驱动没有解析，基础触控不需要。
4. 备选料 GT9916（I2C 0x5d）的节点在这份 DT 里是 `disabled`，这台机器用的是 FTS（`tp-vendor = 0x38`）。如果遇到 GT9916 的机器，需要另做适配。

## 3. 面板（三星 DDIC，`qcom,mdss_dsi_m3_38_0c_0a_dsc_cmd`；详见 `package/FuxiPanelBoard.h`）

### 3.1 基本参数
| 项 | 值 |
|---|---|
| 面板 ID | `mi,panel-id = <0x4d33 0x380c00>`（"M3"） |
| DDIC | 命令集是三星显示的惯例：`F0 5A 5A` / `F0 A5 A5` Level-2 解锁/上锁，`B0` 全局参数偏移，`F7 0F` 更新。加上面板名里的 "sec"，判断为**三星显示**（具体型号未核实） |
| 分辨率 / 尺寸 | 1080×2400，66.3×147.3 mm（6.36"） |
| 链路 | DSI0 单路，4 lane，**命令模式**，TE 引脚同步，burst，1.1 GHz，RGB888 |
| DSC | 8 bpp / 8 bpc，切片 540×40，每包 2 切片，block prediction 打开 |
| 时序 | 3 套：60 / 120 / 90 Hz。h 前后沿 124/124、同步 32；v 前沿 128、后沿 132、同步 24 |
| 亮度 | DCS `0x51`，12 bit（4–4095，开机初值 307）。`bl-inverted-dbv`，两个字节按大端发送（按驱动逻辑推断）。DC 调光阈值 450 |
| HDR | 峰值 420 nit，黑电平 0.323 nit（与本机 HWC 上报一致） |

### 3.2 控制器与 GPIO
- MDSS/MDP 0xae00000（GIC SPI 83），DSI0 控制器 0xae94000（v2.7），DSI0 PHY 0xae95000、PLL 0xae95500（v5.2），DISP_CC 0xaf0f000，MDP INTF 0xae36000。
- GPIO：复位 133（低 1 ms → 高 10 ms），TE 86（function `mdp_vsync`），ESD 中断 49（低电平）。
- 电源上电顺序：vddio 1.8 V（GPIO126 使能，之后等 2 ms）→ vddd 1.5 V（GPIO42）→ vci 3.0 V（GPIO127，之后等 10 ms）→ 复位 → on-command。

### 3.3 第一阶段：沿用 UEFI 留下的帧缓冲
- splash 区 **0xB8000000 / 0x2B00000，与 sheng 完全相同**，UEFI 移植的 "Display Reserved" 也是这个值。格式 1080×2400，32 bpp，stride 1080 像素（uefi-fuxi 的 `PcdFrameBufferWidth`）。所以 `board.toml` 里 `bootimg.dtb_node` 的 splash_region 可以照抄 sheng。
- 另有 Demura 保留区 0xA2A80000（0x2B00000），不要占用。
- 这个阶段不需要碰 DSI，画面由 ABL/UEFI 点亮的面板持续扫描输出。命令模式面板需要有人触发刷新，这一点要看 UniXNU `GopFramebuffer` 在 sheng 上怎么处理（sheng 的面板是视频模式还是命令模式，**未核实**）。

### 3.4 第二阶段：原生 DSI 驱动需要的数据（`facts/`）
- 73 组命令序列已逐条解码成 `facts/panel_cmds_*.txt`。主要的几组：
  - `on-command`（63 条）：先发 PPS（`9E`，90 字节）→ `9D 01` → `11` 退出睡眠，等 120 ms → `35` 开 TE → `2A`/`2B` 设 1080×2400 窗口 → 一系列三星私有寄存器 → `29` 开显示；
  - `off-command`（9 条）：`28` 关显示，等 20 ms → … → `10` 进入睡眠，等 120 ms；
  - `timing-switch`（8 条，切换刷新率）；`nolp`；HBM、FOD-HBM、local-HBM（屏下指纹点亮）、DOZE（息屏）、flat-mode、dimming。
- **待核实**：on-command 里的 PPS 写的 `pic_height = 0x09D8 = 2520`，与 timing 和 `2B` 页地址的 2400 不一致。要么面板忽略这个值，要么驱动另外发了一份 PPS。写原生驱动前需要对照 Qualcomm 显示驱动源码或实机确认。

## 4. 建议的验证顺序
1. 先用社区 fuxi 的 UEFI 镜像，通过 `fastboot boot` 确认能进 UniXNU 菜单。只是临时启动，不写分区。
2. 打通 GENI SPI，读芯片 ID `36 40`。读不到就先查电源（2.6 第 1 条）。
3. 复位，等 `0x03`，然后 `A0 00 01`，接中断，验证事件解码。可以先在屏幕上打点，看坐标是否除以 10 后与屏幕对齐。
4. 出图走 splash 帧缓冲。原生 DSI 驱动放到最后。

## 5. 安卓侧还能补采的数据（需要 root）
- ~~触控和面板电源的实时状态~~：已完成，见第 6 节。
- 面板实际收到的命令：开启 `msm_drm` 的 DSI 调试日志后抓 `dmesg`，可以用来核对 PPS 的高度问题。
- 开机时电源何时打开：手机已连续运行 4 天，开机日志已被覆盖；下次重启后马上抓 `dmesg`，可以看到触控驱动的上电时间点。

## 6. 安卓侧实测记录（root，2026-10-31，原始数据在 `power/`）
| 项目 | 结果 | 结论 |
|---|---|---|
| 触控 vdd `pm_humu_l12` | 已开，1800 mV，模式 fast；**唯一消费者 `spi0.0-vdd`** | 只有触控用 |
| 触控 avdd `pm8010m_l6` | 已开，3200 mV；**唯一消费者 `spi0.0-avdd`** | 相机并不用它，只有触控用 |
| 息屏（Dozing）时的触控电源 | 两路都保持开（use 1） | 驱动加载后常开（屏下指纹、双击唤醒需要） |
| 面板 vci / vddio / vddd | 亮屏 3000 / 1800 / 1500 mV；**息屏全部关闭**（use 0） | 面板每次亮屏都会重新走一遍上电时序 |
| RPMh 资源地址（`/sys/kernel/debug/cmd-db`） | `ldob12` = **0x40900**，`ldom6` = **0x41b00** | 写进 `FuxiTouchBoard.h` 的投票表 |
| VRM 寄存器（Linux `qcom-rpmh-regulator.c`） | +0x0 电压（mV），+0x4 使能，+0x8 模式（PMIC5 LDO HPM = 7） | 同上 |
| 芯片里运行的固件 | `/proc/tp_fw_version` = **53.42** | 与 `st_fts_m3.ftb` 头部（fw 0x53 / cfg 0x42）一致，不需要刷写 |
| 锁定信息 | `48 38 32 30 4D 33 31` = **"H820M31"** | 第 2 字节 0x38 = DT 的 `tp-vendor` |
| 真实触摸事件（dmesg） | `Event 0x13 - Press ID[0] type = 1`、`Event 0x33 - release` | 印证了第 2.4 节的事件 ID 和类型编码 |
| 驱动上电后的等待 | `fts_probe`：`fts_enable_reg` 之后 `mdelay(3)` 再配 GPIO | 投票后等 3 ms |

**仍然无法在安卓下确认的**：XNU 接管那一刻电源的真实状态。RPMh 汇总了多个主控的投票，Linux 只能看到自己这一份，而且开机日志已经被覆盖。现在的做法（总是先投票）不依赖这个答案。

## 7. 触控驱动与 CI（2026-10-31）

### 7.1 结构（与 sheng 的 Novatek 驱动同样分层）
| 文件 | 作用 |
|---|---|
| `drivers/FuxiFtsTouch/FtsProtocol.c` | 通过 `struct fts_bus`（全双工收发、延时、复位脚三个回调）访问芯片：寄存器读写、读芯片 ID、读 FIFO（先读 1 个事件，再按它报告的剩余数一次读完）、复位并等待就绪（3 次重试，失败后按驱动的做法切 4 线 SPI 并读 ID，区分"没固件"和"没芯片"）、开始扫描 |
| `drivers/FuxiFtsTouch/FtsTouchCore.c` | 事件到触点：每个触点 ID（0–9）占一个槽位。手指、手套、手掌、触控笔算按下；悬停算抬起；坐标截断到 x/y 最大值。看门狗、ESD 错误，以及没有请求过的就绪事件，都会置 `reset_needed`。另有板级坐标映射（交换、镜像、缩放） |
| `package/FuxiTouchProbe.cpp` | 内核探针。时钟、引脚、DMA、SMMU 的代码沿用 sheng 的 `TouchProbe.cpp`（总线相同），芯片相关部分换成 FTS：RPMh 上电、复位脚、低电平中断加 FIFO。导出的 `native_touch_*` 符号与 sheng 版一致，sheng 的 HID 层（`ShengHidInput`）可以直接读 |

`drivers/FuxiFtsTouch` 是子模块，指向独立的组件仓库 [OddlyLabs/FuxiFtsTouch](https://github.com/OddlyLabs/FuxiFtsTouch)。

### 7.2 探针的启动参数
| 参数 | 默认 | 说明 |
|---|---|---|
| `fuxi-touch=1..4` | 关 | 1 快照（含在 cmd-db 里按名字查 `ldob12`、`ldom6`）；2 RPMh 投票上电，等 3 ms，打开 QUP 时钟，配引脚（数据线和时钟下拉、CS 上拉、复位脚输出高、中断脚上拉）；3 GENI SPI 12 MHz，复位，等 `0x03`，读 ID `36 40`；4 `A0 00 00` → 50 ms → `A0 00 01`，接中断，读事件，发布触点 |
| `fuxi-touch-power=0` | 1 | 跳过 RPMh 投票（确认 UEFI 已经上电时用） |
| `fuxi-touch-tcs` | 2 | apps RSC DRV2 的活动 TCS（DT `qcom,tcs-config`：TCS 0–2 是活动 TCS） |
| `fuxi-touch-hz` | 12000000 | SPI 速率（不超过 DT 的 12 MHz） |
| `fuxi-touch-poll=1` | 0 | 不用中断，每 8 ms 读一次 FIFO |
| `fuxi-touch-map` | 0 | 发布前的坐标变换：1 交换 x/y，2 镜像 x，4 镜像 y（坐标轴方向要上机确认，见 2.6） |
| `fuxi-touch-panic` | 0 | 1：请求的阶段结束或失败时 panic，panic 信息就是 4 行诊断，可以在 panic 屏上读结果（现在还没有显示驱动）；2：在阶段 4 里，第一次完整的按下、抬起之后也 panic |

- **启动入口**：`bsd_init` 在 `IOKitInitializeTime()` 之后、`vfsinit()` 之前调用 `native_touch_probe()`（`patches/xnu/fuxi-touch-probe.patch`）。探针在自己的内核线程里运行；没有 `fuxi-touch` 参数时立即返回。每个阶段的 4 行结果也会打印到内核日志。

- **中断**：TLMM 检测下降沿，经汇总中断 SPI 208 唤醒线程。线程一直读 FIFO，直到中断线变回高电平，所以读的过程中来的新事件不会丢。另外每 100 ms 检查一次线电平，防止漏掉边沿。
- **坐标**：`ShengHidInput` 把 x 按 0..30480、y 按 0..20320 换算到 HID 的逻辑范围，所以探针把 fuxi 的 0..10800 × 0..24000 线性映射到这两个范围再发布。最终 HID 坐标和屏幕成正比。
- **诊断页**：`native_touch_rows[0..3]` 以 "FTS" 开头，依次显示：
  - 阶段和时钟；
  - 引脚寄存器；
  - 电源（cmd-db 查到的地址、TCS 返回码）、芯片 ID、复位结果、最后一个事件；
  - 事件计数、触点和中断计数。

### 7.3 尚未验证（需要上机）
1. RPMh：TCS 的完成位不开中断时会不会置位不确定，所以等不到时只记录 `w-1`，不算失败。TCS 2 在 fuxi 的 UEFI 下是否空闲也没确认，`fuxi-touch-tcs` 可以换成别的 TCS。
2. GENI 不提供"拉高 CS 前等 10 µs"的设置（驱动的 `SPI_DELAY_CS`）。12 MHz 下应该问题不大，但没有实测。
3. 坐标轴方向、`ShengHidInput` 在 fuxi 竖屏下的方向处理。
4. 启动时机：探针在 `bsd_init` 里启动，这时 IOKit 已经可用。在真机上能否在 panic 屏出现前跑完、panic 屏能否显示出来，都还没验证。

### 7.4 CI
见 README 的"CI"一节。

**第一阶段**（提交 `920093c`，2026-11-01）：借用 sheng 的构建。先跑 sheng 配置的 XNU 构建拿到编译记录，再按 sheng 编译触控探针的参数（`-Wall -Wextra -Werror` 加几项例外）编译本项目的探针和驱动，最后检查符号解析。结果：
- sheng 原版探针（对照）、`FuxiTouchProbe.cpp`、`FtsProtocol.c`、`FtsTouchCore.c`、`GeniSpi.c`、`QcomRpmh.c` 全部编译通过；
- 本项目目标文件引用 39 个强符号，全部能在 1011 个 XNU 目标文件（`.o` 和 `.cpo`）里找到，没有与 XNU 重复定义的符号；
- 唯一的弱引用 `armcpu_spi_attach` 在同一次构建出来的 `build/armcpu/libArmCpu.a` 里。

**第二阶段**（2026-11-01 起）：本仓库改成 UniXNU 官方模板（`examples/device`）结构的 fuxi 设备仓库，CI 直接编译 fuxi 自己的内核。见第 8 节。

## 8. 设备仓库结构（2026-11-01）

按 UniXNU 的"Port a device"流程，以官方根项目为基础，社区的 fuxi 移植只作参考：

| 仓库 | 作用 |
|---|---|
| [OddlyLabs/UniXNU](https://github.com/OddlyLabs/UniXNU)（fork 自 UniXNU/UniXNU，分支 `fuxi-fts-touch`） | 构建工具和组件目录。`components.toml` 加了组件 `FuxiFtsTouch`（依赖 `QcomGeniSpi`），并有中英文文档页 |
| [OddlyLabs/FuxiFtsTouch](https://github.com/OddlyLabs/FuxiFtsTouch) | 组件仓库：FTM5 协议和触点跟踪，带主机测试 |
| 本仓库 | fuxi 设备仓库：`board.toml`、`feeds.toml`、`configs/fuxi_defconfig`、`dts/fuxi.dts`、`patches/xnu/`、`package/`、`scripts/build-fuxi-touch.py`、`porting.toml` |

- **内核配置**：实际生效的配置是 arch、SoC、OS 三个配置片段再加上设备的 defconfig。设备 defconfig 在模板基础上关掉了 Adreno（SoC 片段默认打开），并加上触控归档。归档按顺序构建：corecrypto（普通版和命名空间版）、ArmDTPlatform、ArmCpu、SoftwareAES 及其绑定、libpthread、mockfs 测试用文件系统（OS 片段打开了 `CONFIG_XNU_SOURCE_MOCKFS`）、`build-fuxi-touch.py`。
- **启动钩子补丁**：上下文只用 XNU 原有的行，所以上游改动它前后的共享补丁时，这个补丁不会失效。已确认它能在 arch、SoC、OS 三组补丁之后干净应用。
- **porting.toml**：UniXNU 的 `scripts/porting-record.py check` 通过（13 个区域）。Input 区域是 `implemented`，其余多数是 `planned`。上游的检查器还不认识 `FuxiFtsTouch` 组件，所以本仓库的记录检查用 fork 里的检查器。
- **镜像**：payload 需要这台手机的唯一芯片 ID（`--unique-chip-id`）和 iPadOS 17.7.11 恢复镜像；固件要从社区的 uefi-fuxi 里选一个版本。这几项都还没做。
