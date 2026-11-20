# 基带的数据面起来了（2026-11-20）

## 结论

基带的**链路**在实机上起来了：GLINK 的 IPCRTR 通道被 modem 打开，第一个 HELLO 发出去，
`glink_up` 变成 1。加上前面已经证明的 PAS 引导（阶段 9，31 个分段，31 段全部拷完），
**modem 现在是完整跑起来的**。

**这一步没有证明的**：卡 1 的 SIM 还没有被读。链路起来只说明 QRTR 的控制通道通了，
QMI 的 UIM 服务还没有被 NEW_SERVER 宣告出来，SIM 的状态也没有被问过。RF 仍然关着。

## 之前为什么不起来：两个缺陷都在本仓

### 缺陷一：探针自己又建了一条 GLINK，和组件里的那条抢同一批 SMEM 项

`package/FuxiModemProbe.cpp` 的 `stepGlink()` 原来自己做这件事：

```cpp
int rc = allocItem(kGlinkDescItem, 32);          // 478
if (!rc) rc = allocItem(kGlinkTxItem, kGlinkFifoBytes);  // 479
...
qcom_glink_init(&gOwner.glink, tx_fifo, rx_fifo, kickGlink, nullptr);
gOwner.channel = qcom_glink_channel(&gOwner.glink, kIpcRtrChannel, ..., &client, nullptr, 0);
```

而组件里的适配器 `drivers/QcomBaseband/platform/fuxi/fuxi_modem.c` 的 `fuxi_glink_start()`
建的**是同一条边**：同样是 478/479/480 三个项（`FUXI_GLINK_DESC_ITEM` / `FUXI_GLINK_TX_ITEM` /
`FUXI_GLINK_RX_ITEM`），同样的四字描述符布局。两边各自 `qcom_glink_init` 一次，
于是同一个描述符上有两个读者。

而且探针这一条的**状态回调是空的**：

```cpp
const QcomGlinkClient client = {nullptr, nullptr};
```

`QcomGlinkClient::state` 是链路状态变化的回调（`package/QcomGlink.h:46`）。传空，
`glink_up` 就永远没有人置位，所以诊断页上 `glink_up` 一直是 0。

### 缺陷二：组件的 poll 从来没有被调用过

`package/FuxiModemProbe.cpp` 里声明了

```cpp
extern "C" void fuxi_modem_poll(void);
extern "C" void fuxi_modem_report(unsigned second);
```

但**两个都没有被调用**。适配器的 `fuxi_modem_poll()` 是唯一会跑 GLINK 的地方
（`qcom_glink_poll`），也是唯一会发第一个 HELLO 的地方（`fuxi_send_hello`）。
没人跑它，modem 打开通道的动作就没人应答，HELLO 也发不出去。

**顺序上的要点**：modem 是**主动打开**通道的一方，适配器是应答的一方
（`fuxi_modem.c` 里 `qcom_glink_channel(..., 0)` 的最后一个参数是 0，表示不主动开口）。
所以在 modem 起来之前，适配器是干等着的——**必须有人在 modem 起来之后继续跑 poll**，
这正是探针的 owner 线程该做的事。

## 改了什么

**`package/FuxiModemProbe.cpp`**

- `stepGlink()` 不再建自己的 GLINK，只做三件事：确认 478/479/480 三个项存在
  （描述符 32 字节，发送与接收 FIFO 各 16 KiB），确认它们的内容对，把 `glink_started` 置位。
- `stepAdapter()` 把 GLINK 的门铃交给适配器（`kickGlink`），以前传的是 `nullptr`，
  所以适配器即使有东西要说也没有办法敲 modem 那边的门。
- 新增 `waitLink()`：跑适配器的 poll，直到 modem 打开通道且 HELLO 发出，超时 10 秒有界。
  它同时把适配器的状态读回来填 `glink_up`。
- `serviceEvents()` 里那段自己 poll GLINK 的代码删掉了，GLINK 现在只有适配器一个读者。

**`drivers/QcomBaseband/platform/fuxi/fuxi_modem.c`**

- 新增 `fuxi_modem_status()`：把适配器的链路状态（`ready`、`hello_sent`、
  收发的包数、`qrtr_servers`）交给外面读。组件原来把状态关在自己的 `static fuxi_modem_link` 里，
  板级的探针没有办法知道链路有没有起来。
- `hello_sent` 是新加的一个位：通道开了不等于 HELLO 发出去了，两件事分开记。

## 实机证据

镜像：`/tmp/fuxi-link2.img`，boot image sha256 `fdeabe59c44724f5a691e19acd829cd8e80e25a23b042b66b66ea020f5634421`，
kernel.macho sha256 `ea2bf0e75b4d340a78717f87665b2f27ed2e8cc0b924228d88dfe5ddd9cca0de`。
日志：`/private/tmp/fuxi-usb/run-20261120-200040/xnu-usb.log`。

```
FUXI_MODEM: pas 4 step 2 answers 0/0
FUXI_MODEM: the GLINK items are there (desc 32 bytes, tx 16384, rx 16384)
FUXI_MODEM: the QRTR adapter took the link
FUXI_MODEM: step: the link
FUXI_MODEM: the link is up and the first HELLO went out (tx 1, rx 0)
FUXI_MODEM: the modem is up, stage 9
```

诊断页（`python3 scripts/live-fuxi-modem-state.py`）：

```
{"directories_found": 0, "dtb_pas_answer": 0, "dtb_pas_supported": 1, "entries_listed": 0,
 "glink_up": 1, "ipcc_irqs": 17, "last_error": 0, "loaded_segments": 31, "modem_pas_answer": 0,
 "modem_pas_supported": 1, "mss_rail_found": 1, "scm_x0": 0, "scm_x1": 0, "smp2p_fatal": 0,
 "smp2p_ready": 1, "stage": 9}
```

和上一轮相比变的：`glink_up` 从 0 变成 1，`smp2p_ready` 从 4 变成 1，
`ipcc_irqs` 从 7 变成 17（多出来的 10 次是 GLINK 的门铃）。

## 主机检查

- `python3 drivers/QcomBaseband/scripts/check.py`：全部 PASS，含
  `fuxi_modem_hello_model_test` 和 `fuxi_modem_integration_test`
  （后者用一个假的 modem 对端跑真实的 `fuxi_modem.c`：假 modem 打开 IPCRTR 通道、
  给一个 intent、发自己的 QRTR HELLO）。
- `./unixnu build kernel`：内核链接通过。

## 这一步没有证明的

- **卡 1 的 SIM 没有被读**。链路起来是必要条件，不是充分条件。QMI 的 UIM 服务
  要先被 modem 用 NEW_SERVER 宣告出来，桥再问它卡的状态。
- `tx 1, rx 0`：**modem 的 HELLO 回包还没有到**。适配器里那个「回包要欠着、
  等下一次事件再冲」的机制（`qrtr_flush_hello_reply`）还没有被触发过，
  因为适配器的 poll 在探针的 `waitLink()` 返回之后就没有再跑过。
  **owner 线程必须在链路起来之后继续跑 poll**，否则 modem 宣告的服务永远收不到。
- `qrtr_servers` 是 0，和 `rx 0` 一致。
- RF 仍然关着。

## 下一步

owner 线程在 `waitLink()` 成功之后要**继续**跑适配器的 poll（而不是返回），这样
modem 的 NEW_SERVER 才会被收到，桥才会有服务可以问。这是走向「读卡 1」的下一步。
