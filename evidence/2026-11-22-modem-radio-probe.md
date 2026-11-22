# 卡读出来了，信号没有：radio 在 persist，NAS 说 information unavailable

**日期：2026-11-22。** 本轮任务：给基带加三个只读请求，问 modem 三个问题——
radio 开着吗、注册上了吗、信号多强。

**结果：三个问题 modem 都答了，答案把下一步定死了。**
DMS 答 error 0，模式是 **5 = persist**（不是 online）；
NAS 的 Get Serving System 和 Get Signal Info 都答 **error 74 = INFORMATION_UNAVAILABLE**，
注册状态 0 = not registered，**没有信号 TLV**。

也就是说：**卡是好的，radio 却是关着的**，而且关的方式正是
`EXCLUDED.md` 里写着的第二道闸。

| 项 | 值 |
|---|---|
| run id | `run-20261122-090006`（`/private/tmp/fuxi-usb/run-20261122-090006/xnu-usb.log`） |
| kernel sha256 | `20aa9e86a5484095c40276378be74cb7babdbdd150a2eb3a55304f9f7d109dd6` |
| boot image | `/tmp/fuxi-marker.img`（94 633 984 字节） |
| boot image sha256 | `fe4ada6c7c825b2252011a4a989be72b57f4882105177eb833ced4abb6c68932` |
| 驱动仓 commit | `ce16697`（UniXNU `main`，"baseband: ask the modem whether the radio is on and what the signal is"） |
| 设备仓 commit | `4527dd0`（unixnu-fuxi `V2`，无改动） |
| 启动参数 | `fuxi-modem=15`，其余不变 |

镜像只刷了 `boot` 分区（`fastboot flash boot`，serial `b4c62a1e`），没有刷过任何其它分区。

## 判据：三个只读请求都拿到答案

**成立。** 三个请求都发出去、都有回答，而且回答不是"没有回答"：

```
COACHZ baseband: the DMS answer: error 0, 2 TLVs
COACHZ baseband: the serving system answer: error 74, 2 TLVs
COACHZ baseband: the signal answer: error 74, 1 TLVs
```

端口自己的报告：

```
COACHZ baseband: 1137 s: the radio: persist not registered
```

## 三个回答的原样字节

从 wire 上取出来（本轮的 `FUXI_MODEM` dump），三个都是 QMI response：

**DMS Get Operating Mode（0x2d，事务 0x31）** —— 20 字节：

```
02 31 00 2d 00 0b 00 | 02 04 00 00 00 00 00 | 01 01 00 05 00 00
```

头：flag `02`（response）、事务 `31 00`、消息 `2d 00`、TLV 总长 `0b 00` = 11。
TLV `0x02`（result）4 字节，**error = 0**。TLV `0x01` 1 字节，值 `05` = **persist**。

**NAS Get Serving System（0x24，事务 0x32）** —— 24 字节：

```
02 32 00 24 00 0f 00 | 02 04 00 01 00 4a 00 | 01 05 00 00 00 00 00 00
```

TLV `0x02`（result）4 字节，**error = 0x004a = 74**。
TLV `0x01`（serving system）5 字节：`00 00 00 00 00`
—— 注册状态 0 = not registered，CS attach 0，PS attach 0，选中的网络 0，radio 接口数 0。
**没有 TLV `0x12`（Current PLMN）**，所以没有网络可言。

**NAS Get Signal Info（0x4f，事务 0x33）** —— 16 字节：

```
02 33 00 4f 00 07 00 | 02 04 00 01 00 4a 00
```

TLV `0x02`（result）4 字节，**error = 74**。**只有 result 一个 TLV**，
没有 TLV `0x14`（LTE signal strength），所以没有 RSSI/RSRQ/RSRP/SNR。

## error 74 是什么

74 = `QMI_PROTOCOL_ERROR_INFORMATION_UNAVAILABLE`。
枚举取自 libqmi 的 `src/libqmi-glib/qmi-errors.h`
（Chromium 镜像 `chromium.googlesource.com/chromiumos/third_party/libqmi/+/master/src/libqmi-glib/qmi-errors.h`），
同一份枚举里还有：

```
0    NONE
52   DEVICE_NOT_READY
53   NETWORK_NOT_READY
74   INFORMATION_UNAVAILABLE
94   NOT_SUPPORTED
```

**94 = NOT_SUPPORTED** 就是上一轮那个"modem 的 failure reason 94"——
它是 modem 自己说"SSCTL Get Failure Reason (0x0022) 这个消息我不实现"，不是我们被拒。

**74 的意思是"现在还没有这些信息"**，不是"拒绝"。对一个还没上网的 radio 来说，
问它信号强度而它答 unavailable，是对的答案。

## 关键的一步：74 的原因在它前面

`error 74` 是症状，不是原因。原因在 DMS 那一行：**模式是 5 = persist**。

Qualcomm 的 `dms_service_v01.h` 里 5 = `QMI_DMS_OPERATING_MODE_PERSIST`，
即"保持 LTE 保持注册在网，但让用户面（数据）不可用"。
它和 `ONLINE`（1）的区别就在这里。

参考实现的 bring-up 顺序（`hakios-umi-baseband-档案.md:911`）写得很清楚：

```
→ (bridge) umi_bb_modem_ready()  /  (自有) UIM→DMS online→NAS 注册
```

**UIM → DMS online → NAS 注册。** 我们现在停在第一步：UIM 通了（卡读出来了），
但**没有做 DMS online 这一步**，所以 NAS 注册和信号全都拿不到。

而 `EXCLUDED.md` 的 D1 已经把这两道闸分开了：

| 闸门 | 控制什么 | fuxi 状态（今天） |
|---|---|---|
| **RMTFS**（QRTR service 14） | modem 是否宣布协议栈服务 | ✅ **通了**（4/4 文件，assign 被接受，48 个服务） |
| **TFTP**（QRTR service 4096） | **modem 是否从 OFFLINE 转 ONLINE** | ❌ **全仓无 tftp** |

参考实现的 TFTP 服务器注释（`umi_modem_mhi.inc:685-687`）说明了为什么：

> The modem reads its carrier configuration (modem_pr/mcfg) here and stays offline
> (DMS DeviceNotReady) without it.

**modem 要从 TFTP 读它自己的 carrier 配置（modem_pr / mcfg），读不到就一直是 offline。**

本轮观察到的 **persist** 正好落在这个区间里：不是 offline（0），也不是 online（1），
是"上线但不给你数据面"——carrier 配置没拿到时 modem 的常态。

**所以下一步是 TFTP，不是 DMS Set Operating Mode。**
直接发 DMS Set Operating Mode (0x2e) 让它 online，
参考实现自己也只做到这一步、而 carrier 配置仍然缺着；先补 TFTP 才是那把钥匙。

## 顺带确认的三件事

### 卡：这一轮和上一轮逐字节相同

UIM Get Slot Status (0x0047) 的回答 102 字节，两轮完全一样：

```
02 23 00 47 00 5f 00 | 02 04 00 00 00 00 00 | 13 05 00 02 01 00 02 03 | 12 03 00 02 00 00 | 11 25 00 02 02 00 00 00 00 16 3b 9f 96 80 1f c7 80 31 e0 73 fe 21 1b 57 37 86 60 e2 03 80 98 c8 00 00 00 00 00 00 00 00 | 10 1f 00 02 02 00 00 00 01 00 00 00 01 0a <ICCID: 10 bytes, redacted> 01 00 00 00
```

TLV `0x11` 是 37 字节、装着 ATR；TLV `0x10` 是 31 字节、装着卡 1 的那一条（`02 02` = card 2 / state 2 一带，
后面 10 字节是 ICCID，**按记录规则在这里不落纸**；它在设备日志的原始包里）。

端口自己的解析（`coachz_bb_esim.h` 的 `cbe_parse_slots`）给出：
slot 1 `card=2 state=1 logical=1`（present 且 active），slot 2 `card=1 state=1 logical=2`（absent）。
ATR 22 字节（`3b 9f 96 … 98 c8`），ISO 7816-3 的 TCK 校验和成立。
ICCID 10 字节，半八位解码后是 20 位，以 **8986** 开头（中国移动）——只留这个前缀，位数和归属不落纸。

### RMTFS：仍然是 7 请求 7 回答

```
FUXI_MODEM: 1138 s: the rmtfs was announced 1 times, took 7 requests, answered 7; opened 2 closed 0, 5120 reads and 5120 writes of 5120 and 5120 sectors
```

### 流控表：这一轮用了 12 个目的地，表是 32 个槽

```
FUXI_MODEM: 1138 s: the qrtr flow has 17 packets pending over 12 destination(s), 1 resumes sent, 0 received
```

上一轮是 10 个目的地。**上一轮的 8 槽表在这一轮会丢第 9 个回答**，
现在 `QRTR_MAX_FLOWS` 是 32（`UniXNU/drivers/QcomBaseband/platform/fuxi/qcom_qrtr.h:83`），
余量够了。modem 依然一次 RESUME_TX 都没发（`1 resumes sent, 0 received`），
所以"信用还清了就能复用槽"这条补救在真机上帮不上忙——这一点没有变。

## 结论与下一步

**这一轮的问题已经回答完了，而且答案很干净：**

- 卡读出来了，SIM slot 1 present + active，ICCID 已解码。
- radio 是 **persist**，注册状态 not registered，信号 **information unavailable**。
- 三个请求都是只读的，没有碰过 radio 的状态，没有写任何东西。

**下一步是 TFTP（QRTR service 4096）**：让 modem 能读到它的 carrier 配置（modem_pr / mcfg）。
这是 `EXCLUDED.md` 点名的第二道闸，也是参考实现里 modem 从 offline 转 online 的那一步。
本板全仓没有 TFTP 的任何代码，这是一件从头写的事。

拿到 carrier 配置之后，radio 应当自己转 online，
那时候 NAS 的 Get Signal Info 才会给 TLV `0x14`——判据是**信号 TLV 出现**，而不是 error 变 0。

RF 仍然在 modem 手里（blocker B6 未解），我们没有做任何让 radio 发功率的事；
本轮只是问它状态。TFTP 也是只读的服务：读 modem 的配置，写请求照参考实现那样确认后丢弃。
