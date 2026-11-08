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
