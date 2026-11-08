# 触控接入系统层：现状与验证计划

目标：FTM5 的触点在 SpringBoard / backboardd 里生效（系统层可用），必要时借鉴
`SadNutWall/unixnu-fuxi`。

## 这条链路在本仓库已经完整

| 环节 | 位置 | 状态 |
| --- | --- | --- |
| FTM5 协议（0xA0 扫描、0x87 FIFO、0xFA/0xFB 寄存器） | `drivers/FuxiFtsTouch/FtsProtocol.c` | 已实现，stage 3 在真机读出 chip id `36 40` |
| 探针分阶段启动、排空 FIFO、发布触点 | `package/FuxiTouchProbe.cpp` | 已实现（`fuxi-touch=1..4`） |
| 报文描述符 | `package/NativeHidInput.cpp` Report 3 | 十指 Touch Screen 集合（usage page 0x0D / usage 0x04） |
| 数字化器属性 | 同上 line 420 | `Built-In`、`DisplayIntegrated`、`DigitizerCollectionDispatch` |
| 挂接系统层 | `package/FuxiHidProbe.cpp` | 载入 IOHIDFamily + IOHIDEventDriver → 等 IOHIDResource → 注册 metaclass → allocate → attach → start |
| backboardd 过滤器 | 同上 line 74-81 | 在 **IOHIDEventDriver 与它的 interface** 两个平面发布 `Built-In`/`DisplayIntegrated`/`PrimaryUsagePage 0x0d`/`PrimaryUsage 4` |

## 从对方仓库反解出的关键约束

`SadNutWall/unixnu-fuxi` 的 `package/HidInputProbe.cpp` 里记录了一条从 target 反解来的行为：

> `-[BKHIDDirectTouchEventProcessor processEvent:sender:dispatcher:]` 会**丢弃**发送服务的
> `PrimaryUsagePage` 不是 `0x0D`、`PrimaryUsage` 不是 1 / 4 / 0x20 的数字化器事件
> （"ignoring digitizer event … from unsupported service"）。合并设备的第一个集合是键盘，服务会发布成
> (1, 6)，所以要在服务 **matched 之后**把 Digitizer/Touch Screen 写到服务上（写设备会改变驱动匹配）。

我们的 `FuxiHidProbe.cpp` 已经在 driver 与 interface 两个平面设了这四个属性，**与这条要求一致**。

## 对方仓库实际能借鉴的东西

| 项 | 结论 |
| --- | --- |
| `drivers/NativeHidInput/NativeHidInput.cpp` | 他**三个分支都没有这个路径**（构建脚本引用了它，仓库里不存在），所以 HID 实现无从借鉴 |
| `package/HidInputProbe.cpp` | 设计与我们相同（同一套 backboardd 约束、同样用合并设备），另有 `sheng-touch-split` 独立数字化器分支——但他自己的板级并没有打开 |
| `package/HidInputStub.c` | **死文件**，全仓库没有构建脚本引用它，不参与链接 |
| `package/FuxiButtons.cpp` | **真正可借鉴**：电源/音量键 → HID，含"短按短于唤醒延迟会丢失"的锁存修复。与触控屏无关，属另一条线 |
| Novatek（`TouchProbe.cpp` + 生成固件） | 与我们的机器无关：我们是 ST FingerTip FTM5 |

## 还没验证的

**没有任何证据表明触点真的到了 backboardd。** 真机日志里的

```
FTS rd0 ev0 in0 mv0 up0 er0/00 rdy0 rearm0 c0 -1,-1 hid0 irq0/0 xf0
```

含义是"数据线从未变低、一次读都没发生"——**没人碰屏时本来就是这个值**，不能据此判断坏了。
`porting.toml` 记录 2026-11-02 的 stage 4 曾报出过真实触点（轮询与中断两种方式），说明面板那条路至少跑通过一次。

## 验证计划（各一次启动，不新增热路径打印）

1. `fuxi-touch=4 fuxi-touch-hid=0 fuxi-touch-panic=2`，划一次屏：
   - 出现 `in/mv/up` 计数 → 面板到内核这段是好的，问题在系统层；
   - `rd` 增长但 `er` 增长 → SPI 读失败；
   - `rd` 仍 0 → stage 4 的 sensing 没有真正进去。
   panic 会把四行报告打出来，配合 `fuxi-panic-fastboot=30` 自动回 fastboot。
2. 若第 1 步通过，改 `fuxi-touch-hid=1 fuxi-touch-panic=0`，读已有的 `fuxi-display: hid` 行
   （`fuxi_hid_row()`，每 500 ms 轮询至 20 s）看探针走到哪一步、event driver 是否 attached，再判断是否需要动
   `NativeHidInput` 的报文派发。

**不要再往触控/显示线程里加 printf**：2026-11-08 曾在轮询循环里每 4 秒打印一次，直接把 USB CDC 打崩
（镜像已回退到 `sha256 9aff0c67…`，与出问题前逐字节相同）。

## 解决（2026-11-08 18:25）：一个参数名

根因是**参数名不一致**，不是驱动缺陷。`NativeHidInput::start()`：

```c
touchEnabled = PE_parse_boot_argn("sheng-touch-hid", &touch, sizeof(touch)) && touch == 1;
...
if (touchOnly || touchEnabled) {
    if (&native_touch_set_sink) native_touch_set_sink(&NativeHidInput::touchNotify, this);
}
```

板级一直设的是 `fuxi-touch-hid=1`，而这个标志只在 `sheng-touch-hid=1` 时才成立。`touchEnabled` 恒假带来四个连带后果：

1. 描述符里不追加数字化器集合；
2. 不设 `Built-In` / `DisplayIntegrated` / `DigitizerCollectionDispatch`；
3. **不注册 sink**，触控线程发布帧后无人通知 HID；
4. 不调用 `edge::install`。

加上 `sheng-touch-hid=1` 后一切就位。两个参数各管一件事，都要留：`fuxi-touch-hid=1` 让探针启动 HID 设备，
`sheng-touch-hid=1` 让设备把数字化器打开。

### 真机证据（镜像 `sha256 974c9c95…`，run 182127）

```
FTS rd4471 ev4661 in237 mv4185 up237 er0/00 rdy0 rearm0 c0 1785,422 hid2936 irq4470/0
HID st6 f0 fam0 evd0 sz208 res1 dev4 drv1 up1 rep…（drv1 = IOHIDEventDriver 已 attach）
fuxi-metal: submitted 1322 retired 1322   gop offers 530 done 530
panic 0   smmu fault 0
```

`hid` 从恒 0 变成 2936，237 次按下/237 次抬起/4185 次移动全部被读取且无错；GOP 帧数从界面静止时的约 23 涨到 530，
与“界面开始响应触摸”一致。触摸点、移动、多指抬起都到达了系统层。

### 为定位留下的东西

`fuxi-display` 的周期上报里多了两行：`fuxi-touch: loop/step/err/irq` 和探针自己的 `FTS …` 行。它们挂在**已有**的
上报点（45 s 起，每 30 s 一次，共 6 次），不在任何热路径上——上一轮把 printf 放进触控循环里曾直接打崩 USB CDC。

## 按键（电源/音量）—— 移植对方的驱动与其短按修复

从 `SadNutWall/unixnu-fuxi` 的 `package/FuxiButtons.cpp`（259 行）整份移植，**包括** `b529c30` 的锁存修复：
中断处理里锁存自己那几个键的边沿；线程发现当前无键按下时，把锁存的边沿报成一对完整的按下/松开，并撑到
`kMinPressMs = 60 ms`。没有这个锁存，短于唤醒延迟的一次轻点永远到不了 HID——用户必须按住约两秒。

接线（四处）：

| 位置 | 改动 |
| --- | --- |
| `package/FuxiButtons.cpp` | 新文件，定义 `native_buttons_set_sink`（真正注册 sink 的地方） |
| `package/FuxiHidProbe.cpp` | 删掉原来的空桩，那个桩让 `NativeHidInput` 的呼叫落空 |
| `package/BootLogExtraStubs.c` | 删掉 `fuxi_buttons_stats[10]` 的占位定义（否则重复符号） |
| `package/FuxiLegacyDisplayProbe.cpp` | late probe 里调用 `fuxi_buttons_probe()`，板级 `fuxi-buttons=1` 才启动 |

归档加入 `scripts/build-fuxi-touch.py`（与 `FuxiTouchProbe.cpp`、`FuxiHidProbe.cpp` 同一个 `libFuxiTouch.a`）。

### 真机证据（镜像 `sha256 e98a2bfe…`，run 183225）

```
fuxi-buttons: stage 3 irq 116 reports 98 mask 0 pon 560 gpio 583 clears 0 pdc 30000 err 0 spur 0
FTS rd566 ev567 in18 mv529 up18 er0/00 c0 10185,785 hid256 irq565/0 xf0
fuxi-metal: submitted 553 retired 553   gop offers 262 done 262
panic 0   smmu fault 0
```

`stage 3` = 已映射、已配置、已装载中断；`pon 560`/`gpio 583` 是两个键所在的 APID；**116 次中断产生 98 条 HID 报文**，
`err 0`、`spur 0`。同一镜像上触控仍在送报文（`hid256`），GPU 553/553 与 262/262 未受影响。

### 过程中踩到的两个坑

1. `fuxi_buttons_stats` 同时被 `BootLogExtraStubs.c` 定义 → 链接期 `duplicate symbol`；占位定义必须让位给真实驱动。
2. `ctl reboot-fastboot` 本来就以非零退出（控制 socket 先关闭），在 `set -e` 脚本里会**中断整条刷机链**；
   该调用必须与 `set -e` 分开。

## 按键：驱动通了，系统层还没接受（未完成）

移植后的驱动本身完全正常（镜像 `126b598d…`，run 185624）：

```
fuxi-buttons: stage 3 irq 442 reports 390 mask 0 pon 560 gpio 583 clears 0 pdc 30000 err 0 spur 0 submit 390/0
```

`submit 390/0` 表示 `buttonsAction` 提交了 390 次、`submitReport` **全部返回成功**（IOReturn 0）；`spur 0` 说明没有误中断。
也就是说：**驱动 → HID 设备这一段是通的，报文没有生效是在系统层。**

### 为什么被丢

为了触摸能被接受，我们把**同一个事件服务**标成了数字化器（`PrimaryUsagePage 0x0D`、`PrimaryUsage 4`，外加
`Built-In`/`DisplayIntegrated`）。backboardd 的数字化器过滤器正是读这几个字段——于是这个服务被当作触屏，
它发出的 Consumer（音量/电源）报文不再被当作按键处理。**触控与按键在单一服务上互斥。**

### 试过的两条路

1. **把数字化器拆成独立服务**（`sheng-touch-split=1`，镜像 `8f832257…`）：触控仍然正常（`hid856`→`hid1858`），
   但**桌面起不来**。日志给出原因：

   ```
   Driver com.apple.AppleUserHIDDrivers has crashed 0 time(s)
   DK: AppleUserHIDEventDriver-0x1000001dc waiting for server …-1000001dc
   DK: AppleUserHIDEventDriver-0x1000001dc user server timeout
   ```

   DriverKit 的 `AppleUserHIDEventDriver` 抢走了那个纯数字化器服务，然后等用户态 server 超时——这也解释了
   对方仓库里同一个分支为什么默认不打开。

2. **跳过数字化器标注**（`fuxi-hid-no-usage=1`，镜像 `c35d3cc7…`）：代码里保留了这个开关，**默认关闭**。
   不标注时按键这条路的语义正确，但触摸就失去 backboardd 的数字化器资格，触控会失效。

### 现在的状态

设备留在**已验证可用**的配置上（镜像 `f7c9b33e…`）：`sheng-touch-hid=1` + `fuxi-buttons=1`，桌面正常、触摸正常；
按键驱动已加载并 armed（`stage 3`、APID 0x560/0x583），报文能提交但系统不响应。

### 下一步的两条候选

1. **反向拆分**：触控留在一个设备上（现状），另建一个**只带键盘+Consumer** 的设备专门给按键。代码已实现于
   `fuxi-buttons-device=1`（`native_hid_buttons_allocate()`），**尚未上机验证**——它的风险低于正向拆分，因为
   第一个设备与现在完全一致，DriverKit 不会去抢键盘设备。
2. 找到满足 backboardd 数字化器过滤器、又不动 Consumer 服务分类的写法（例如只把 usage 写在 interface 上）。

两个都需要在设备上按音量键/划屏来判定，**必须在有人在设备旁时进行**。

### 顺带记下的限制

内核命令行已到 1000 字节上限（1024 的限制，超了直接 `unixnu: the kernel command line is 1017 bytes` 失败）。
为塞入新参数，`fuxi-touch-panic=0`（本来就是默认值）已删除。再加参数前要先腾位置。

### 新证据：电源键有效，音量键无效（2026-11-08 20:25）

这推翻了我此前“数字化器分类会挡掉 Consumer 报文”的判断：**电源键就是 Consumer 0x30，它能生效**，说明报文通路是通的。
为此给两个 PMIC 外设加了按位累加的取证（`fuxi_buttons_edges`），一次按键都漏不掉：

```
fuxi-buttons: edges pon c0 last 40 gpio 1 last 1 bits e
fuxi-buttons: stage 3 irq 245 reports 232 ... submit 232/0
```

- `pon 0xc0` = bit7（电源）+ bit6（RESIN，音量减）；
- `gpio 0x1` = 音量加；
- `bits 0xe` = report 位 1/2/3 全部产生过；`submit 232/0` 全部提交成功。

**结论：驱动与 HID 设备对三个键都正确工作，音量键的报文确实发出去了。**

### 据此改的一处：Consumer 集合独立

原来的四个 Consumer 用法（Menu/Power/Vol+/Vol−）**嵌在键盘应用集合里面**，这不是键盘上报音量键的标准布局。
现在它们单独成为一个 `Consumer Control` 应用集合（`05 0C 09 01 A1 01` … `C0`），键盘报告 1 与触控报告 3 的布局不变。
镜像 `sha256 b199bcd7…`：触控仍正常（`hid49`），三个键位仍全部产出（`bits e`），无 panic。

### 如果音量仍无反应，剩下的解释在音频链路

iOS 上音量键由 SpringBoard 处理，但真正的“调音量”要经过 `AVSystemController`/音频路由；本端口没有音频输出，
音量变更很可能被记为空操作（HUD 也不出现）。这与“电源键（不经音频）有效、音量键无效”的现象一致。
判定方法：换上新描述符后再按音量键，若仍无变化而 `edges`/`bits` 依旧正常，则 HID 层已交付完成，剩余属音频链路缺失。

## sheng 是怎么实现按键的（答案）

对照 `UniXNU-sheng`：**它的实现和我们完全一样**，没有额外技巧。

| 环节 | sheng | fuxi |
| --- | --- | --- |
| 按键驱动 | `package/ButtonProbe.cpp` | `package/FuxiButtons.cpp`（同一份逻辑，含短按锁存） |
| 上报 | report 2 的 Consumer 位（Power / Vol+ / Vol−） | 同 |
| 描述符 | Consumer 键嵌在键盘应用集合内 | 同（已恢复为与 sheng 逐字节一致） |
| 服务分类 | `PrimaryUsagePage 0x0D` / `PrimaryUsage 4` 发在 event driver 与 interface 上 | 同 |
| **音频** | **`AdspFirmware.c`、`AdspProbe.cpp`、`AudioProbe.cpp`、`ShengAudioReach.c`，参数 `sheng-adsp=2`、`sheng-audio=12`，并有音频 HAL 构建脚本** | **完全没有音频栈** |

这就是“电源键有反应、音量键没有”的原因：电源键不经音频；音量键在 iOS 上由 SpringBoard 收，但要真正改变音量
必须经过 `AVSystemController` 与音频路由，fuxi 没有音频路由，于是变更被记为空操作。

结论：**按键在 HID 层已经接好**（三个键位都产出、提交成功、电源生效）；音量要有可观效果需要移植音频栈，
那是独立的一块工作，不是 HID 的缺陷。

顺带更正：2026-11-08 20:53 我把 Consumer 用法拆成独立集合，那一步**偏离了参考实现**（sheng 用嵌套布局且工作正常），
现已恢复，并用 `diff` 验证描述符与 sheng **逐字节一致**。

### 该配置的真机确认（镜像 `sha256 1812533c…`，run 211741）

```
FTS rd70 ev71 in3 mv63 up3 er0/00 c0 2495,17695 hid56 irq69/0 xf0
fuxi-buttons: edges pon c0 last 80 gpio 1 last 1 bits e
fuxi-buttons: stage 3 irq 17 reports 16 ... submit 16/0
gop offers 135 done 135      panic 0
```
