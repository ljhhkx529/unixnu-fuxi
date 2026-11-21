# The modem asks the locator where its own services are

**日期：2026-11-21。** 本轮任务：发布 QRTR service 64（servreg locator），并回答 GET_DOMAIN_LIST。

**结果：判据 A 成立。** modem 确实向 locator 发来了 GET_DOMAIN_LIST（0x0021），带的是它要问的服务名，我们按表里的格式回答了。

一次上机。镜像只刷了 `boot` 分区（`fastboot flash boot`，serial `b4c62a1e`），没有刷任何其它分区。

| 项 | 值 |
|---|---|
| run id | `run-20261121-205840`（`/private/tmp/fuxi-usb/run-20261121-205840/xnu-usb.log`） |
| kernel sha256 | `7f42ee98799e35fcce947bd0e841e5a5614da97153f678ac0d6ca105708e129d` |
| boot image | `/tmp/fuxi-servreg-locator.img`（94 617 600 字节） |
| boot image sha256 | `da980cc4cdd73ac5a2fa8e7c96180c593b550d6e9fe2deca8f822f8552a19844` |
| 驱动仓 commit | `c2ae87b992f7ad23c66383f5deab5532ea50088f`（UniXNU `main`） |
| 设备仓 commit | `9bde260f8666dfa2c98b8e602216155caee17db2`（unixnu-fuxi `V2`） |
| 采集时长 | 685 s（远超要求的 300 s），日志 1.6 MB |
| 启动参数 | `fuxi-modem=14`，其余不变 |

## 判据 A：DATA 包到达 <our_port>，带 0x0021，日志打印了被问的名字

**成立。** modem 问了两个名字。

```
FUXI_MODEM: the locator: the modem asks where "tms/servreg" is, 1 domain
FUXI_MODEM: the locator: the modem asks where "tms/pddump_disabled" is, 0 domains
```

## 判据 B：从来没有包到达 <our_port>

**不成立**（与 A 相反）。locator 在第一个 HELLO 之后就被问到，`locator_requests` 从 0 涨到 2 并保持不变。

## 判据 C：modem 问了一个表里没有的服务

**成立一次。** `tms/pddump_disabled` 不在表里，回答是 0 个 domain。这个名字决定了下一轮该读哪个 `.jsn`；本轮不读。

## 宣告（NEW_SERVER）的发送日志

宣告在 modem 的 HELLO 之后立刻发出，目标是 modem 的 node 0、控制端口：

```
FUXI_MODEM: the qrtr packet we sent is 52 bytes: 01 00 00 00 04 00 00 00 01 00 00 00 fe ff ff ff 00 00 00 00 14 00 00 00 00 00 00 00 fe ff ff ff 04 00 00 00 40 00 00 00 01 01 00 00 01 00 00 00 01 40 00 00
FUXI_MODEM: the locator was announced: service 0x40 version 1 instance 1, on port 16385, to node 0
```

拆开看：

- 头部：`01 00 00 00`（版本 1）`04 00 00 00`（类型 4 = NEW_SERVER）`01 00 00 00`（本地 node 1）
  `fe ff ff ff`（源端口 = 控制端口）`00 00 00 00` `14 00 00 00`（size 20）
  `00 00 00 00`（目的 node 0）`fe ff ff ff`（目的端口 = 控制端口）
- 主体：`04 00 00 00`（命令字 = NEW_SERVER）`40 00 00 00`（service 0x40）
  `01 01 00 00`（实例字 0x0101 = 版本 1、实例 1）`01 00 00 00`（本地 node 1）
  `01 40 00 00`（端口 0x4001 = 16385）

这与本轮任务要求的 `{4, 0x40, 0x0101, 1, <our_port>}` 逐字一致。

宣告只发了 1 次。整个 685 s 里 modem 的 HELLO 计数没有再涨，所以没有再宣告；代码里的重宣告条件是 `hellos > locator_hello_at`，即对端每发一次 HELLO 就重发一次。

## 第一个请求与回答（原始字节）

请求（21 字节）：

```
00 01 00 21 00 0e 00 01 0b 00 74 6d 73 2f 73 65 72 76 72 65 67
```

拆开：类型 `00`（请求），事务 `01 00`，消息 id `21 00` = 0x0021 = GET_DOMAIN_LIST，
TLV 总长 `0e 00` = 14，TLV 类型 `01`，长度 `0b 00` = 11，值是 ASCII `tms/servreg`。

**被问的名字是 TLV 1 里的一个普通字符串**，不是一个列表里的 0x01 元素。这一点和任务说明一致。

回答（55 字节）：

```
02 01 00 21 00 30 00 02 04 00 00 00 00 00 10 02 00 01 00 11 02 00 01 00 12 1c 00 01 11 6d 73 6d 2f 6d 6f 64 65 6d 2f 72 6f 6f 74 5f 70 64 b4 00 00 00 00 00 00 00 00
```

拆开：

| 位置 | 字节 | 含义 |
|---|---|---|
| 0 | `02` | 回答 |
| 1–2 | `01 00` | 事务，回显请求的事务 |
| 3–4 | `21 00` | 消息 id，回显请求的 0x0021 |
| 5–6 | `30 00` | TLV 总长 48 |
| TLV | `02 04 00 00 00 00` | 结果 = 0（成功），错误 = 0 |
| TLV | `10 02 00 01` | domain 总数 = 1 |
| TLV | `11 02 00 01` | 表的版本 = **1** |
| TLV | `12 1c ...` | 列表，28 字节 |

TLV 0x12 的内容：`00` 之后的第一个字节是 count（`01`），然后每个 domain 记录是
长度字节 `11`（17）、域名 `msm/modem/root_pd`（**不带结尾 NUL**）、实例 `b4 00 00 00`
（180，小端）、service_data_valid `00`、service_data `00 00 00 00`。

三点容易做错的地方，都对了：

1. **TLV 0x11 是 1，不是 1.1.1。** 表自己的 `sr_version` 是 major 1 / minor 1 / patch 1，
   但回答里的版本字段是常量 1。这是从 `hakios.mpss.c:4644-4648` 抄来的。
2. **域名不带结尾 NUL。** 长度字节 17，`msm/modem/root_pd` 正好 17 个字符。
3. **实例是小端的 180。** `b4 00 00 00`，不是 `00 00 00 b4`。

## 第二个请求与回答（表里没有的名字）

请求（29 字节）：

```
00 02 00 21 00 16 00 01 13 00 74 6d 73 2f 70 64 64 75 6d 70 5f 64 69 73 61 62 6c 65 64
```

回答（24 字节）：

```
02 02 00 21 00 11 00 02 04 00 00 00 00 00 10 02 00 00 00 11 02 00 01 00
```

结果成功（0/0），domain 总数 0，版本 1，**没有 TLV 0x12**：没有 domain 就没有列表。
这是任务说的"表里没有的名字"的正确形状，不是错误。

## 表的来源：modemr.jsn 的完整 dump

模式 14 先 dump 了 `/image/modemr.jsn`（710 字节），整份文件都在日志里。从 dump 重建出来的内容：

```json
{
    "sr_version": { "major": 1, "minor": 1, "patch": 1 },
    "sr_domain": {
        "soc": "msm", "domain": "modem", "subdomain": "root_pd",
        "qmi_instance_id": 180
    },
    "sr_service": [
        { "provider": "tms", "service": "servreg",
          "service_data_valid": 0, "service_data": 0,
          "dcomment": "data can be used to configure ACK timeout.Make data_valid 1 as well.Unit is second" },
        { "provider": "gps", "service": "gps_service",
          "service_data_valid": 0, "service_data": 0,
          "dcomment": "No comment" }
    ]
}
```

这就是回答里那一个 domain 的出处：域名 `msm` + `modem` + `root_pd` = `msm/modem/root_pd`，
实例 `qmi_instance_id` = 180，两个服务 `tms/servreg` 和 `gps/gps_service`。

**注意 `tms/servreg` 的 `dcomment`**：它说 service_data 是用来配置 ACK 超时的，
单位是秒，并且要连 service_data_valid 一起置 1。表里现在是 0，所以回答里也是 0。
这个字段还没有被用到。

**`gps/gps_service` 没有被问。** 本轮 modem 只问了 `tms/servreg` 和
`tms/pddump_disabled`。所以回答里 domain 只有 1 个，不是 2 个——这正是任务预告的
"如果 modem 问 tms/servreg，found 是 1 不是 2"。gps 的那条要等 modem 问它。

## 这一轮没有发生的事

- modem 仍然只发布两个服务（0x42 SERVREG_NOTIF、0x2b SSCTL），685 s 里各 636 次，
  没有多出第三个。**UIM（0x0b）仍然没有发布**：port 问了三次（version 0 instance 0 到
  node 7907、version 1 instance 0 到 node 0、version 1 instance 2 到 node 7907），
  一次都没有被回答。
- modem 的状态字是 `ready 1, fatal 1, handover 1`，和之前每一轮一样。按 EXCLUDED.md
  B6/B7，fatal 是 modem 自己的定时 handover 状态字，不是崩溃，也不是阻塞。
- modem 的 SMP2P 仍然只有两条：`smp2p` 值 0x0，`slave-kernel` 值 0x6。
- 没有发过 SSCTL 0x0020/0x0021，也没有发过 SERVREG_NOTIF 0x0024（EXCLUDED.md C1–C4）。
- 没有读表以外的任何 `.jsn` 文件。

## 下一步该看什么

`tms/pddump_disabled` 这个名字决定下一轮读哪个 `.jsn`。它不在 modemr.jsn 里，
所以它在 modem 的别的表里——多半是 `modemPD.json` 或同族的一个。找到它，
才知道 modem 为什么要问一个 pddump 的开关，以及它期望的 service_data 是什么
（表里 `tms/servreg` 的 dcomment 说 service_data 是 ACK 超时的秒数，
如果 modem 问的是同一个域的另一条服务，数据字段的含义可能不同）。

另外：modem 问了 locator，却没有因此多发布服务。这说明 locator 的发布是必要的
（modem 会来问），但不是充分条件——它还在等别的东西。按小米 10 的记录，
另一个是 QRTR TFTP（service 4096）。本轮任务没有要求做 TFTP，也没有排除它。
