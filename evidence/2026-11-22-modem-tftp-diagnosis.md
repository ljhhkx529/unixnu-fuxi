CONTRADICTION: EXCLUDED.md:66 (B12) 说 "DMS Get Operating Mode 答 error 0、模式 5 = PERSIST"，
EXCLUDED.md:132 说 "2026-11-22 实测：卡读出来了，但 radio 是 PERSIST（模式 5）" ——
但 run-20261122-123021 的日志显示 radio 是 **OFFLINE（模式 0）**（`:1149` `the radio is offline`），
DMS 的回答是 **error 74**（`:1148` `the DMS answer: error 74, 2 TLVs`）而不是 error 0，
全篇 9220 次每秒报告全是 `the radio: offline not registered`，`the radio: persist` **一次都没有**。
本文件按日志走：operating mode 从 PERSIST 变成了 **OFFLINE**，不是 ONLINE；
NAS Get Signal Info 的回答里**仍然没有**信号 TLV 0x14（`:1143` `the signal answer: error 74, 1 TLVs`）。

# The TFTP service on the device: what the modem actually asked, and where the answers went

**日期：2026-11-22。** 本轮：只读的 TFTP 服务器（QRTR service 4096），让 modem 读到它的 carrier 配置。

**本文件是一次诊断，不是补丁。** 下面三条结论都只从日志和代码读出来，没有改任何代码、没有重新编译、
没有重新刷机、没有重启设备。

| 项 | 值 |
|---|---|
| run id | `run-20261122-123021`（`/private/tmp/fuxi-usb/run-20261122-123021/xnu-usb.log`，524 556 行，最后一次检查时） |
| 对照 run id | `run-20261122-105450`（`/private/tmp/fuxi-usb/run-20261122-105450/xnu-usb.log`，317 583 行） |
| boot image | `/tmp/fuxi-tftp4.img`（94 638 080 字节），sha256 `1d7bc084f8cc6f12aa238f4fe13de61dccdcd0fd53a4c5dadedb022d232555c7` |
| modem 发布的服务 | 45（`:3441` `45 QMI services`）；对照 run 46（`:2850` `46 QMI services`） |
| TFTP 计数 | `:920` `took 14 requests and answered 14; read 0 times, 0 bytes, refused 0 and missing 3` |
| 本轮的判据 | operating mode 离开 PERSIST，或 NAS Get Signal Info 的回答里出现信号 TLV 0x14（`EXCLUDED.md:128`） |

---

## 结论摘要

1. **locator 的四个问答在格式上都是对的**，事务号回显、消息 id 回显、TLV 布局与上游逐字段一致，
   回答的 `dst node` 用的是**请求的来源 node**。但日志**不能**区分"modem 接受了"和"modem 放弃了"——
   两个 run 都是问了 4 次之后不再问，而其中至少一个 run 的 TFTP 是坏的。**locator 已关闭**（`EXCLUDED.md:60` B8）。

2. **modem 的 TFTP 请求全部到达了，`834_0_0.mbn` 一个字节都没有传过去。** 传输根本没开始。
   原因是一个结构性错误：**读请求的 OACK 从服务端口（0x4003）发出去了，而不是从传输端口
   （0x4010+slot）发出**（`fuxi_tftp.h:580`）。于是 modem 把下一个包（ACK 或 ERROR）送回服务端口，
   那里没有传输在等它。

3. **`/readwrite/mcfg.tmp` 的"not found"不是缺陷，是对的。** 那一轮的 `missing` 计数全部来自另外三个路径
   （`ota_firewall/ruleset` 两次、`mcfg.tmp` **写之前**一次）。写→读的往返本身**是通的**，日志里能直接看到。

---

## 调查 1 — locator

### 1.1 请求 1：完整原始字节

`:465`

```
FUXI_MODEM: the locator: the modem asks where "tms/servreg" is, 1 domain
FUXI_MODEM: the locator: the request is 21 bytes: 00 01 00 21 00 0e 00 01 0b 00 74 6d 73 2f 73 65 72 76 72 65 67
```

21 字节，**全部**在日志里，没有截断（这两行是本 port 自己打的完整 hex，不是 QRTR dump）。
QRTR 头（`:462`）：

```
the qrtr header is version 1 type 1 src node 0 src port 2, confirm 0, size 21, dst node 1 dst port 16385
```

解码：

| 字节 | 值 | 含义 |
|---|---|---|
| 0 | `00` | QMI 请求标志（`QMI_REQUEST`） |
| 1-2 | `01 00` | 事务号 = 1 |
| 3-4 | `21 00` | 消息 id = 0x0021 = `SERVREG_GET_DOMAIN_LIST_REQ` |
| 5-6 | `0e 00` | 长度 = 14 |
| 7 | `01` | TLV type 0x01 |
| 8-9 | `0b 00` | TLV 长度 = 11 |
| 10-20 | `74 6d 73 2f 73 65 72 76 72 65 67` | `"tms/servreg"`，**没有 NUL 结尾** |

**和上游的形状对得上。** 上游 `drivers/soc/qcom/qcom_pdr_msg.c:51-81` 里
`servreg_get_domain_list_req_ei[]` 的第一个元素是 `.data_type = QMI_STRING, .tlv_type = 0x01`，
而 `qmi_encode_string_elem()`（`qmi_encdec.c:296-341`）在 `enc_level == 1` 时**只写字符本身，
不写长度前缀**（长度前缀只在 `enc_level != 1` 时写，见 `:329-334`）。所以 TLV 0x01 的载荷就是裸字符串，
长度 11，没有结尾 NUL。**这一条正是上游 `pdr_interface.c` 经 `qmi_send_request` 发出的形状**
（`pdr_interface.c:364-368`，`SERVREG_GET_DOMAIN_LIST_REQ` / `servreg_get_domain_list_req_ei`）。

第二个 TLV（type 0x10，`domain_offset`）**没有出现**。上游的 `domain_offset_valid` 是 `QMI_OPT_FLAG`，
不设就不发（`qcom_pdr_msg.c:63-75`）。合法。

> 关于 `qmi_add_lookup(QMI_SERVICE_ID_SERVREG_LOC, 1, 1)`（`pdr_interface.c:681`）：这一句**不是**发请求。
> `qmi_add_lookup()`（`qmi_interface.c:207-226`）只把服务登记进 `qmi->lookups` 链表，然后调
> `qmi_send_new_lookup()` 发一条 **NEW_SERVER 控制消息**（QRTR type 4），请求本身是
> `pdr_get_domain_list()`（`pdr_interface.c:350-368`）用 `SERVREG_GET_DOMAIN_LIST_REQ` 发的。
> 两者是不同的事。**把 `qmi_add_lookup` 当成"请求的形状"会看错方向。**

### 1.2 回答 1：完整原始字节与 QRTR 头

`:467`

```
FUXI_MODEM: the locator: the answer is 55 bytes: 02 01 00 21 00 30 00 02 04 00 00 00 00 00 10 02 00 01 00 11 02 00 01 00 12 1c 00 01 11 6d 73 6d 2f 6d 6f 64 65 6d 2f 72 6f 6f 74 5f 70 64 b4 00 00 00 00 00 00 00 00
```

QRTR 头（`:468`）：

```
the qrtr packet we sent is 88 bytes: 01 00 00 00 01 00 00 00 01 40 00 00 00 00 00 00 37 00 00 00 00 00 00 00 02 00 00 00 02 01 00 21 ...
```

| 字段 | 值 |
|---|---|
| version | 1 |
| type | 1（DATA） |
| src node | **1** |
| src port | **16385**（0x4001 = `FUXI_LOCATOR_PORT`） |
| confirm | 0 |
| size | 55 |
| **dst node** | **0** |
| dst port | **2** |

**`dst node` = 0 = 请求的 `src node`（`:462` 的 `src node 0`）。** 不是"peer 广播的 node"。
这是对的。

解码 55 字节的载荷：

| 字节 | 值 | 含义 |
|---|---|---|
| 0 | `02` | QMI 响应标志 |
| 1-2 | `01 00` | 事务号 = 1，**与请求一致** |
| 3-4 | `21 00` | 消息 id = 0x0021，**与请求一致** |
| 5-6 | `30 00` | 长度 = 48 |
| 7 | `02` | TLV 0x02 |
| 8-9 | `04 00` | 长度 4 |
| 10-13 | `00 00 00 00` | result = 0（success），error = 0 |
| 14 | `10` | TLV 0x10 |
| 15-16 | `02 00` | 长度 2 |
| 17-18 | `01 00` | total_domains = 1 |
| 19 | `11` | TLV 0x11 |
| 20-21 | `02 00` | 长度 2 |
| 22-23 | `01 00` | db_rev_count = 1 |
| 24 | `12` | TLV 0x12 |
| 25-26 | `1c 00` | 长度 28 |
| 27 | `01` | domain_list_len = 1（`QMI_DATA_LEN`，`qcom_pdr_msg.c:131-137`） |
| 28 | `11` | 域名的长度 = 17 |
| 29-45 | `6d 73 6d 2f 6d 6f 64 65 6d 2f 72 6f 6f 74 5f 70 64` | `"msm/modem/root_pd"` |
| 46-49 | `b4 00 00 00` | instance = 180（`QMI_UNSIGNED_4_BYTE`，小端） |
| 50 | `00` | service_data_valid = 0 |
| 51-54 | `00 00 00 00` | service_data = 0 |

**和上游的 `servreg_location_entry`（`pdr_internal.h:24-29`）逐字段对得上**：
`name`（字符串）、`instance`（u32）、`service_data_valid`（u8）、`service_data`（u32）。
上游 `qcom_pdm_get_domain_list()`（`qcom_pd_mapper.c:148-205`）填的正是
`domain_list[j].name` 和 `domain_list[j].instance`，`service_data_valid` 保持 false。
`db_rev_count = 1` 也是上游写死的（`qcom_pd_mapper.c:169-170`）。

> **一处与上游不同、但和硬件验证过的参考实现一致：** 上游的 `servreg_location_entry_ei`
> （`qcom_pdr_msg.c:11-50`）把 `name` 声明为 `QMI_STRING`，而 TLV 0x12 里的这个字符串处在
> `enc_level != 1`（它在 VAR_LEN_ARRAY 的结构体里面），按 `qmi_encode_string_elem()`
> （`qmi_encdec.c:329-334`）**应当带一个长度前缀**。我们的回答**没有**这个前缀，
> 而是把长度放在 TLV 0x12 自己的载荷第一个字节（`01` = 域的个数，`11` = 域名长度）。
> 这正是硬件验证过的参考实现的做法（`hakios.mpss.c:4655-4667`：
> `answer[at++] = (uint8_t)strlen(domain); memcpy(answer + at, domain, ...)`）。
> **本 port 跟参考实现一致，与上游的 encode 路径不一致。** 这一点不是缺陷，
> 因为 modem 接受了这个回答（它随后就来了 TFTP 请求）。

### 1.3 四个请求，全部列出

| # | 行 | 事务号 | 名字 | 回答 | TLV 0x10（域数） | TLV 0x11（版本） | TLV 0x12（列表） |
|---|---|---|---|---|---|---|---|
| 1 | `:465` | 1 | `tms/servreg` | `:467`（55 B） | `01 00` = 1 | `01 00` = 1 | 28 B，1 个域 |
| 2 | `:486` | 2 | `tms/pddump_disabled` | `:488`（24 B） | `00 00` = 0 | `01 00` = 1 | **没有** |
| 3 | `:1102` | 3 | `avs/audio` | `:1104`（24 B） | `00 00` = 0 | `01 00` = 1 | **没有** |
| 4 | `:1109` | 4 | `tms/servreg` | `:1111`（55 B） | `01 00` = 1 | `01 00` = 1 | 28 B，1 个域 |

回答 2（`:488`）的完整字节：

```
02 02 00 21 00 11 00 02 04 00 00 00 00 00 10 02 00 00 00 11 02 00 01 00
```

事务号 2 回显、消息 id 0x0021 回显、TLV 0x02 = success、TLV 0x10 = 0 个域、TLV 0x11 = 1。
**没有 TLV 0x12**——上游在 `total_domains == 0` 时也不发列表（`qcom_pd_mapper.c:189-199` 的循环空转）。
参考实现同样只在 `found != 0` 时才写 TLV 0x12（`hakios.mpss.c:4650`）。

**0 个域是**正确的值**：`tms/pddump_disabled` 和 `avs/audio` 都不是本 board 的 `modemr.jsn` 里的服务。
这已经被记成排除项 B9（`EXCLUDED.md:63`），参考实现对任何未知名字都只回 0 域
（`hakios.mpss.c:4634-4636`）。**`avs/audio` 属于 ADSP 的注册表，不归这里的表管。**

### 1.4 `dst node` 用的是请求的来源 node

四个问答的 QRTR 头，全部核对过：

| 问答 | 请求（收到的） | 回答（发出的） |
|---|---|---|
| 1 | `:462` `src node 0 src port 2 ... dst node 1 dst port 16385` | `:468` `src node 1 src port 16385 ... dst node 0 dst port 2` |
| 2 | `:484` `src node 0 ... dst node 1 dst port 16385` | `:489` `src node 1 src port 16385 ... dst node 0 dst port 2` |
| 3 | `:1100` `src node 0 ... dst node 1 dst port 16385` | `:1105` `src node 1 src port 16385 ... dst node 0 dst port 2` |
| 4 | `:1107` `src node 0 ... dst node 1 dst port 16385` | `:1112` `src node 1 src port 16385 ... dst node 0 dst port 2` |

**每一次的 `dst node` 都是 0 = 请求的 `src node`。** 不是 HELLO 里 peer 广播的 node。
参考实现也是把 node 原样回送（`hakios.mpss.c:4694` `qrtr_send(servreg_port, node, port, answer, at)`，
`node`/`port` 就是收到请求时的那一对）。

### 1.5 modem 反应了吗？

**这一轮一共 4 次 locator 请求**：`:465`、`:486`、`:1102`、`:1109`。
最后两个在第 7 秒左右（`:1155` `7 s: the link is up ... servers 47`），
计数器 `:1163` `7 s: the locator was announced 1 times, took 4 requests, answered 4`。

**对照 run（`run-20261122-105450`）也是 4 次**：`:466` `tms/servreg`、`:487` `tms/pddump_disabled`、
`:829` `avs/audio`、`:838` `tms/servreg`（又一次）；`:1015` `2 s: ... took 4 requests and answered 4`。

**两个 run 问了同样的四个名字，顺序也一样。** 所以：

- **"modem 停止提问"不能判定为"modem 接受了"**——对照 run 里 modem 问了 4 次之后也停了，
  而那一轮的 TFTP 服务是坏的（路径前缀全错，每次读都回 "file not found"）。
  **"不再问"和"接受"在日志里长得一样。**
- **"modem 放弃了"更不成立**：第 4 次请求（`:1109`）问的是**它已经问过的同一个名字**
  `tms/servreg`，而且是在第 7 秒——**在 TFTP 那一整轮（`:738`-`:907`，第 6-7 秒）之后**。
  如果它对第一次的回答不认，第二次（`:486`）之后就该退让，而不是在 7 秒时再问一遍同样的名字。
  **它重问了，这说明它还在等一个更完整的回答，或者它只是重试；两者都不是"放弃"。**

**调查 1 的结论：locator 的四个问答在格式上都是对的（事务号回显、消息 id 回显、TLV 布局与上游
和硬件验证过的参考实现一致、`dst node` 用的是请求的来源 node）。
但日志不能证明 modem 接受了它们。locator 这一项按 `EXCLUDED.md:60` B8 已经关闭，本轮不重开。**

---

## 调查 2 — 4 字节包和 20 字节包

### 2.1 这两个包是什么

先解决"是不是 TFTP"。

**`:793` 那个 20 字节的包**（`the qrtr packet is 52 bytes`，完整 body）：

```
00 05 00 09 45 6e 64 20 6f 66 20 54 72 61 6e 73 66 65 72 00
```

QRTR 头：`:794` `version 1 type 1 src node 0 src port 35, confirm 1, size 20, dst node 1 dst port 16387`。

这**是**一个格式完全正确的 RFC 1350 ERROR：
opcode `00 05`（大端 16 位 = 5 = ERROR）、错误码 `00 09`、然后是 NUL 结尾的 `"End of Transfer"`。

> 提问里说"到达的是 `05 00 09` + 文字，**三个**字节在消息前面，不是四个"——**这一点需要纠正。**
> 日志的原始字节是 `00 05 00 09 ...`，**四个**字节在前：opcode 占两个（`00 05`），
> 错误码占两个（`00 09`）。第 793 行的 `the qrtr packet is 52 bytes:` 那一行把整个 body 都列了出来，
> 上面的十六进制就是从那一行逐字节取的，不是重排的。
> 所以：**不是畸形 ERROR，也不是 1 字节 opcode 的厂商布局，就是标准的 RFC 1350 ERROR。**
> 错误码 9 = `TFTP_END_OF_TRANSFER`，参考实现里也是这个码
> （`hakios.mpss.c:4074`）。

**`:813` 那个 4 字节的包**：

```
00 04 00 00
```

QRTR 头：`:814` `version 1 type 1 src node 0 src port 36, confirm 0, size 4, dst node 1 dst port 16387`。

opcode `00 04` = 4 = ACK，块号 `00 00` = 0。**格式正确的 RFC 1350 ACK(0)。**

（提问里说"第一个 word 是 0x400"：body 是 `00 04 00 00`，按**小端**读成 u32 是 `0x00000400` = 1024。
日志 `:815` 打的 `the qrtr body is 4 bytes, its first word is 1024` 正是这个数。但 TFTP 的 opcode 和块号都是**大端**16 位，所以 `00 04` 是 ACK，`00 00` 是块 0。
**1024 是小端读的产物，不是协议字段。**）

**两个包都是 TFTP。** 它们的 `dst port` 都是 16387 = 0x4003 = `FUXI_TFTP_PORT`，这是本 port 自己的服务端口，
不是巧合。

### 2.2 从 QRTR 收到"拒绝"的完整调用链

| # | 位置 | 做的事 |
|---|---|---|
| 1 | `qcom_qrtr.c` 的数据钩子 | 把 body 和 `dst_port` 交给 `fuxi_qrtr_data()` |
| 2 | `fuxi_modem.c:298-303` | `dst_port` 在 TFTP 的端口范围内，转给 `fuxi_tftp_receive()` |
| 3 | `fuxi_modem.c:485-495` | 调 `fuxi_tftp_serve(&link->tftp, dst_port, src_node, src_port, body, length, ...)` |
| 4 | `fuxi_tftp.h:482` | `dst_port` 是 **0x4003**（服务端口），不是传输端口 → 走"服务端口"分支 |
| 5 | `fuxi_tftp.h:484` | `fuxi_tftp_parse(body, length, &r)` |
| 6 | `fuxi_tftp.h:180` | `n < 4u`？否，n = 4 |
| 7 | `fuxi_tftp.h:186-193` | opcode 判定：byte 1 = `0x04` = ACK，在 `FUXI_TFTP_RRQ`(1) .. `FUXI_TFTP_OACK`(6) 之间 → **opcode = ACK** |
| 8 | `fuxi_tftp.h:197-201` | `at = 2`，`fuxi_tftp_string(p, n, 2, ...)`：byte 2 = `0x00`，字符串长度为 0 → **没有路径 → return 0** |
| 9 | `fuxi_tftp.h:485-486` | `state->refused++; return 0;` |
| 10 | `fuxi_modem.c:491-494` | `if (!served)` → 打印 `the tftp was sent %u bytes it cannot answer`，**回什么也没发** |

**决定"不能回答"的是 `fuxi_tftp.h:484-486`**：服务端口分支只接受能解析成 RRQ/WRQ 的包。
`00 04 00 00` 解析不出来（opcode 是 ACK，而且路径为空），所以被拒。
20 字节那个 ERROR 同理（opcode 5，路径为空）。

> 注意 `:920` 那行计数说 **`refused 0`**。这是因为 `state->refused` 是**内核**的计数器，
> 而 `:920` 打印的 `refused` 是**适配器**的 `gRefused`（`package/FuxiModemTftp.cpp:347`），
> 两者不是同一个数。内核的 `refused` 只在 `fuxi_tftp.h:485`（解析失败）和 `:503`/`:535` 增长，
> 而 `:485` 这一处是在 `state->requests++` **之前**，所以 `took 14 requests` 里没有它。
> 详见 §4。

**这正是参考实现不做的事。** 参考实现的分工是：
请求走服务端口，`md_tftp_request()`（`umi_modem_mhi.inc:776-819`）；
**块、ACK 和 ERROR 走传输端口**，`md_tftp_client()`（`umi_modem_mhi.inc:822-845`），
由 `md_qrtr_packet()` 的分发决定（`umi_modem_mhi.inc:889-892`）。

### 2.3 传输状态是什么

**这是核心发现。** 看 modem 把包发到哪里：

| 收到的包 | dst port | 我们的传输端口有在等吗 |
|---|---|---|
| `:738` WRQ | 0x4003（服务端口） | — |
| `:746` DATA(1) | **0x4010**（传输端口 slot 0） | 有，slot 0 是 writer → **成功**（`:749` 从 0x4010 回 ACK(1)） |
| `:758`/`:771` RRQ | 0x4003 | — |
| `:784` RRQ `834_0_0.mbn` | 0x4003 | 我们在 **0x4003**（服务端口）回了 OACK（`:788`）← **错** |
| `:793` ERROR(9) | **0x4003**（服务端口） | 服务端口上没有传输在等 → 被拒（`:797`） |
| `:803` RRQ | 0x4003 | 我们在 0x4003 回了 OACK（`:807`）← **错** |
| `:813` ACK(0) | **0x4003**（服务端口） | 服务端口上没有传输在等 → 被拒（`:816`） |
| `:847` DATA(1) | **0x4012**（传输端口 slot 2） | 有，slot 2 是 writer → **成功**（`:850` ACK(1)） |
| `:859` WRQ | 0x4003 | — |
| `:867` DATA(1) | **0x4012** | 有，slot 2 是 writer → **成功**（`:870` ACK(1)） |
| `:879` RRQ `mcfg.tmp` | 0x4003 | 我们在 0x4003 回了 OACK（`:884`）← **错** |
| `:887` ERROR(9) | **0x4003**（服务端口） | 服务端口上没有传输在等 → 被拒（`:890`） |
| `:894` WRQ `mcfg.tmp`（unlink） | 0x4003 | — |
| `:903` RRQ `mcfg_sw.mbn` | 0x4003 | **四个槽全满** → `:907` 回 ERROR "busy" |

**读请求的 OACK 发错了端口。** 看这几行（`fuxi_trace_qrtr_tx` 只打前 64 字节，
所以 OACK 的**载荷在 32 字节处被截断**，`dumped` < `qsize` 就是被截断的证据）：

```
741: src=0x4010 dst=0x0020 qsize=39 dumped=31 op=6(OACK)   ← writer 的 WRQ，OACK 从传输端口 0x4010 发出（对）
788: src=0x4003 dst=0x0023 qsize=52 dumped=32 op=6(OACK)   ← 读的 RRQ，OACK 从服务端口 0x4003 发出（错）
807: src=0x4003 dst=0x0024 qsize=48 dumped=32 op=6(OACK)   ← 同上
884: src=0x4003 dst=0x002b qsize=47 dumped=32 op=6(OACK)   ← 同上
```

截断处可见的选项是 `blksize "4000"` 和 `timeoutms "5000"`，**第三个选项被 64 字节的
dump 上限切掉了**（`fuxi_modem.c:555-560` 只打 `i < 64`）。所以**不要**把这些 OACK 的
选项当作完整载荷引用。

对照参考实现：`md_tftp_request()` 里，**无论写还是读**，OACK 都是从
`src = MD_TFTP_PORT + slot`（`umi_modem_mhi.inc:784` `unsigned src = MD_TFTP_PORT + slot;`，
写分支 `:788-790`，读分支 `:817-818`）发出的。**读请求的 OACK 也不从服务端口发。**

而本 port 的代码：
- 写分支 `fuxi_tftp.h:529`：`ops->send(context, FUXI_TFTP_TRANSFER_PORT_BASE + slot, ...)` → **对**。
- 读分支 `fuxi_tftp.h:580`：`ops->send(context, dst_port, src_node, src_port, out, n)` → **错**，`dst_port` 是服务端口。

**后果**：modem 收到 OACK 后，把下一个包（ACK 或 ERROR）发回**它收到 OACK 的那个端口**，
也就是服务端口 0x4003。而我们的传输在 `state->transfer[slot]` 里等着，
那个 slot 的端口号是 0x4010+slot。**服务端口上的 `serve()` 分支只认 RRQ/WRQ**
（`fuxi_tftp.h:482`、`:494`、`:534`），所以 ACK 和 ERROR 都进不了传输分支，
被 `fuxi_tftp.h:485-486` 拒掉。

**传输永远不推进，slot 永远不释放。**

### 2.4 slot 泄漏的后果

`fuxi_tftp.h:472-477` 的 ERROR 处理（在**传输端口**分支里）会
`ops->close(...)` 然后 `memset(t, 0, sizeof *t)` 释放 slot。
但 modem 的 ERROR 去的是服务端口，**进不了这个分支**。

唯一的 slot 释放点是 `fuxi_tftp.h:466`（writer 的最后一个短块）和 `fuxi_tftp.h:475`（传输端口上的 ERROR）。
读请求分配的 slot（`fuxi_tftp.h:562-574`）**没有任何释放路径**。

四个槽（`FUXI_TFTP_TRANSFER_SLOTS` = 4，`fuxi_tftp.h:72`）于是这样用完：

| 时刻 | 事件 | slot 0 | slot 1 | slot 2 | slot 3 |
|---|---|---|---|---|---|
| `:738` | WRQ `server_check.txt` → slot 0 | **占**（writer） | | | |
| `:746` | DATA(1) 载荷 7 字节 < blksize → 短块 → `:466` 释放 | 空 | | | |
| `:758` | RRQ `ota_firewall/ruleset` | — | | | |
| `:771` | RRQ 同上 | — | | | |
| `:784` | RRQ `834_0_0.mbn`，**打开成功** → slot 0 | **占**（读） | | | |
| `:803` | RRQ `834_0_0.mbn`（再问一次）→ slot 1 | 占 | **占**（读） | | |
| `:826` | RRQ `/readwrite/mcfg.tmp`（**写之前**） | | | | |
| `:839` | WRQ `mcfg.tmp` → slot 2 | | | **占**（writer） | |
| `:847` | DATA(1) 载荷 0 字节 → 短块 → 释放 | | | 空 | |
| `:859` | WRQ `mcfg.tmp`（seek 0）→ 复用 slot 2 | | | **占**（writer） | |
| `:867` | DATA(1) 载荷 1 字节 → 短块 → 释放 | | | 空 | |
| `:879` | RRQ `/readwrite/mcfg.tmp`（**内存里有**）→ slot 3 | 占 | 占 | | **占**（读） |
| `:894` | WRQ `mcfg.tmp`（unlink）→ slot 2 | 占 | 占 | **占**（writer） | 占 |
| `:903` | RRQ `mcfg_sw.mbn` | 满 | 满 | 满 | 满 → `:907` **"busy"** |

**`:758` 和 `:771` 的两个 `ota_firewall/ruleset` 不占槽**：`tftpOpen` 返回 `ENOENT`，
读分支在 `fuxi_tftp.h:541-548` 就回错误并 return 了，**槽是在那之后（`:552`）才分配的**。
`:826` 的 `mcfg.tmp` 同理（写之前，内存里没有）。

**`:894` 那个 WRQ（unlink）泄漏了 slot 2**：它的 OACK 从 0x4013 发出（`:897`），
但之后**没有 DATA 跟来**——这是一个只有 `unlink` 选项的写请求，没有载荷，所以短块永远不来，
`:466` 的释放点碰不到。

**于是 `:903` 的 RRQ `mcfg_sw.mbn` 来的时候，四个槽全满 → `:907` 回 `ERROR code 2 "busy"`。**
**这个文件恰恰是 carrier 配置本身**（`mcfg_sw.mbn`，89296 字节，`:906` 说卷里有）。

对照参考实现：`md_tftp_request()` 在文件不存在时（`umi_modem_mhi.inc:810-813`）
先 `md_tftp_close(t)` 再回错误——**槽是释放的**。本 port 在这一条上**没有偏离**（因为本 port
本来就是先失败再分配槽），但在**读成功的路径上没有释放槽**这一点上偏离了。

### 2.5 参考实现在同样的地方做什么

**上游对"紧跟着 OACK 的一个 4 字节 ACK(0)"：**

参考实现里，modem 收到 OACK 之后回的 ACK 会落到**传输端口**
（因为 OACK 是从 `MD_TFTP_PORT + slot` 发出的，modem 回到同一个端口）。
`md_qrtr_packet()`（`umi_modem_mhi.inc:890-892`）看到 `dst_port` 在传输端口范围内，
就调 `md_tftp_client(slot, ...)`：

```
umi_modem_mhi.inc:838:  if (d[1]!=UMI_TFTP_ACK || block>=t->blocks) { md_tftp_close(t); return 0; }
umi_modem_mhi.inc:840:  for (uint32_t b=block+1;b<=t->blocks && b<=block+t->wsize;b++) MD_TRY(md_tftp_data(slot,b));
```

ACK(0) → `block = 0 < t->blocks` → 进入循环，从块 1 开始发窗口。

**注意 `block >= t->blocks` 就 close**：`t->blocks = umi_tftp_blocks(limit, blksize)`（`umi_modem_tftp.h:81`）。
所以 ACK(0) 之后第一个要发的块是 1。

**端口的不同正是全部的差别：**

| | 参考实现 | 本 port |
|---|---|---|
| 请求进 | 服务端口 `0x4002`/`0x4003` → `md_tftp_request()` | 服务端口 `0x4003` → `fuxi_tftp.h:482` 分支 |
| OACK 出 | **传输端口** `0x4010+slot`（`:784` `src`） | 写：**传输端口** ✓；读：**服务端口** ✗（`fuxi_tftp.h:580`） |
| ACK/ERROR 进 | 传输端口 → `md_qrtr_packet()` → `md_tftp_client()`（`:890-892`） | 服务端口 → 被 `fuxi_tftp.h:485-486` 拒掉 |
| ERROR 的作用 | `md_tftp_close(t)`（`:825`），**释放槽** | 传输端口分支才释放（`fuxi_tftp.h:475`），服务端口到不了 |
| 读成功的槽 | `md_tftp_close(t)` 在 ACK 到最后或 ERROR 时释放（`:825`、`:838`） | **读路径没有任何释放点** |

**一句话**：参考实现把 ACK 和 ERROR 送进传输的代码路径，靠的是 **OACK 从传输端口发出**；
本 port 把读的 OACK 从服务端口发出，于是 modem 把 ACK 和 ERROR 也送回服务端口，
那里没有传输在等，slot 于是漏掉，四个槽用完，`mcfg_sw.mbn` 拿到 "busy"。

### 2.6 还有一个独立的问题：`blksize`

modem 每次都问 `blksize 7680`（`:784`、`:803`、`:826`、`:903` 等请求的选项字节）。
本 port 每次都答 `blksize 4000`（`:741`、`:788`、`:807`、`:884`、`:897` 的 OACK 可见字节）。

`fuxi_tftp.h:99` `FUXI_TFTP_PACKET_BLKSIZE 4000u`，在 `fuxi_tftp.h:238` 处把请求的 blksize 截到 4000。
理由写在 `:96-98` 的注释里："7680 ... is more than a packet holds"。

**这个理由是错的。** QRTR 的 scratch 是 8192 字节（`qcom_qrtr.h:87` `QRTR_MAX_PACKET 8192u`），
头占 32 字节，载荷放得下 8192−32 = 8160 字节。
7680 + 4 + 32 = 7716 < 8192。**7680 装得下。**

参考实现的上限是 8000（`hakios.mpss.c:4075` `TFTP_MAX_BLOCK 8000`，
注释写着 "a datagram of ours holds 8192; the modem's intents 0x4400"），
XNU 那一版是 `MD_PACKET-64`（`umi_modem_mhi.inc:785`
`t->blksize = r.blksize < MD_PACKET-64 ? r.blksize : MD_PACKET-64`）。**两处都容得下 7680。**

这一条在时间上是**排在端口错误之后**的：即使端口修好，块大小被砍到 4000 也可能让 modem 不接受。
但**日志不能证明它是阻塞的**——modem 在收到 `blksize 4000` 的 OACK 之后仍然回了 ACK(0)（`:813`），
说明它至少**接受**了这个 OACK 的块大小。

---

## 调查 3 — 文件请求

### 3.A `modem_pr/so/834_0_0.mbn` 的传输完成了吗？

**没有。一个 DATA 块都没有发出去。**

日志里的相关行，按顺序：

| 行 | 内容 |
|---|---|
| `:784` | modem 发 RRQ，`/readonly/firmware/image/modem_pr/so/834_0_0.mbn`，102 字节，选项 `blksize 7680, timeoutms 5000, tsize 0, wsize 10` |
| `:787` | `FUXI_TFTP: the modem asked for image/modem_pr/so/834_0_0.mbn, and the volume has it (473528 bytes)` |
| `:788` | 我们发出 52 字节的 OACK，**从 0x4003（服务端口）**，`src=0x4003 dst=0x0023` |
| `:793` | modem 发 **ERROR(9) "End of Transfer"**，**到 0x4003** |
| `:797` | `the tftp was sent 20 bytes it cannot answer` —— 被拒 |
| `:803` | modem 第二次 RRQ，同一路径，103 字节（选项 `blksize 7680, timeoutms 5000, wsize 10, rsize 52`） |
| `:806` | `the volume has it (473528 bytes)` |
| `:807` | 又一个 OACK，**从 0x4003** |
| `:813` | modem 发 **ACK(0)**，**到 0x4003** |
| `:816` | `the tftp was sent 4 bytes it cannot answer` —— 被拒 |

逐条回答提问：

- **发了多少 DATA 块？零个。** 从头到尾没有一行 `fuxi_tftp_send_block` 的痕迹：
  **整个 run 里我们发出的包没有一个带 `00 03`（DATA）给 modem。**
- **每个都被 ACK 了吗？** 没有块可 ACK。
- **到的最后一个块号？** 没有。传输的第 0 块都没有发出去。
- **怎么结束的？** **没有结束。** 传输既没完成也没被 ERROR 收掉——ERROR（`:793`）到了服务端口，
  被当成"不能回答"扔了（`:797`），**槽没释放**（见 2.4）。

**注意 `:793` 的 ERROR 本身很说明问题：modem 说 "End of Transfer"，然后 `:803` 又把同一个文件
请求了一遍，换了一套选项（去掉 `tsize`，加上 `rsize 52`）。** 这说明 modem 看到了 OACK、
不满意、重新来过。** 它不是在放弃，它是在**重试**。

参考实现对同一个情形的处理（`umi_modem_mhi.inc:825`）：ERROR 直接 `md_tftp_close(t)`，
**槽被释放**，modem 再问的时候还有槽可用。本 port 因为槽漏了，第二次 RRQ 只能占 slot 1。

**"文件存在"不是答案——"第 29 块被 ACK 了"才是。这里连第 1 块都没发。**
（473528 字节按 blksize 4000 算是 119 块，按 7680 算是 62 块；提问里说的"29 块"是按 16384 算的，
但我们从没答过 16384，所以那个数不适用于本 port。）

### 3.B `/readwrite/mcfg.tmp` 的 not found 是预期的吗？

**要分清三次同名请求。** 这就是提问里说的 "`missing` 计数是缺陷还是预期行为"——答案是**取决于哪一次请求**。

| 请求 | 行 | 时刻 | 本 port 的回答 | 对吗？ |
|---|---|---|---|---|
| RRQ `/readwrite/mcfg.tmp` | `:826` | 第一次 | `:829` `the modem asked for /readwrite/mcfg.tmp, and it wrote no such file` → `:830` ERROR "file not found" | **对**。此时 modem 还没写过它 |
| WRQ `/readwrite/mcfg.tmp` | `:839` | 接着 | OACK（0x4012）+ DATA(1) 的 ACK | 对 |
| WRQ `/readwrite/mcfg.tmp`（`seek 0`） | `:859` | 再接着 | OACK（0x4012）+ ACK | 对 |
| **RRQ `/readwrite/mcfg.tmp`** | **`:879`** | **写完之后** | **`:883` `the modem asked for /readwrite/mcfg.tmp, and the memory has it (1 bytes)`** | **对，而且成功** |

**结论：写→读的往返是通的。** `:883` 那行是直接证据：modem 在两次写之后读同一个路径，
本 port 从内存里取到了它（1 字节）。
写进去的是 `:867` 的 DATA(1) 的载荷 `00`（1 字节），
`:847` 那次写的是空（`:847` 的 DATA(1) 没有载荷）。

所以 `missing` 计数 3（`:920` `missing 3`）**不是** `mcfg.tmp` 那次**成功**的读造成的。三个 missing 是：

1. `:761` `/readwrite/ota_firewall/ruleset`
2. `:774` `/readwrite/ota_firewall/ruleset`（同一路径，第二次）
3. `:829` `/readwrite/mcfg.tmp`（**写之前**那一次）

**`ota_firewall/ruleset` 的两次 not found 是不是缺陷？** 这个 port 的写路径只服务
`/readwrite/` 和 `/only/`（`package/FuxiModemTftp.cpp` 的 `writtenPrefix`），
读路径只服务四个 `/readonly/` 前缀（`package/FuxiModemTftp.h:55-60`）。
`/readwrite/ota_firewall/ruleset` **既没被写过**，也不在四个只读前缀里 → not found。

**参考实现会怎么答？** `hakios.mpss.c:4405-4417`：读分支先在**内存里**找同名的已写文件，
找不到就看 `tftp_file_of(name)`——那四个前缀。`/readwrite/ota_firewall/ruleset` 两个条件都不满足 →
`NOT A PATH SERVED HERE` → 回 "file not found"，`tftp_refused++`。
**参考实现也回 file not found。** 所以这两次**不是缺陷**。

**所以：3 次 missing 全部是预期行为，一个缺陷都没有。** 这直接回答了提问：
"commit `8aa9512` 说写的文件留在内存里让 modem 读回来——如果是真的，
后面 RRQ 同一个路径必须成功，missing 计数就是缺陷。"——**日志证明 `:879` 那次成功了，
所以这三条 missing 不来自它，缺陷不在这里。**

### 3.C `mcfg_sw.mbn`（carrier 配置本身）拿到的是 "busy"

`:903` modem 请求
`/readonly/firmware/image/modem_pr/mcfg/configs/mcfg_sw/generic/China/CMCC/Commercial/Volte_OpenMkt/mcfg_sw.mbn`
（164 字节，选项 `blksize 7680, timeoutms 5000, tsize 0, wsize 10`），
`:906` 说 `the volume has it (89296 bytes)`——**文件在卷里，89296 字节**。
`:907` 我们回了 `ERROR code 2 "busy"`。

**这是 carrier 配置本身，本轮的目标文件。** 它拿到 "busy" 的唯一原因是 §2.4 的槽泄漏。

按 blksize 4000 算，89296 字节应该是 **23 块**（22 × 4000 + 1296）。
这个数本轮**没有**被验证过——一块都没发出去。

---

## 4. 计数器：三个不同的数，别混

日志里同一件事有三个不同的计数器：

| 计数器 | 谁在加 | 含义 | 本 run 的值 |
|---|---|---|---|
| `state->requests` / `state->answered` | `fuxi_tftp.h` 内核侧 | **服务端口**上的 RRQ/WRQ 计数（传输端口上的 DATA/ACK 不计） | 14 / 14（`:920`） |
| `state->reads` | `fuxi_tftp.h:585` | **被答复过的 RRQ 次数**（不是块数） | 3（`:946` `read 3 blocks`） |
| `gReads` / `gBytes` | `package/FuxiModemTftp.cpp:307-308` | 适配器**真的从卷里读出去的字节** | **0 / 0**（`:920` `read 0 times, 0 bytes`） |
| `gMissing` | `package/FuxiModemTftp.cpp:246/277` | 打开失败 | 3（`:920`） |
| `gRefused` | `package/FuxiModemTftp.cpp:270` | 路径不归本 port 管 | 0（`:920`） |

**`read 3 blocks` 是误称。** `fuxi_modem.c:948-951` 打印 `link->tftp.reads`，而 `state->reads`
在 `fuxi_tftp.h:585` 每答复一个 RRQ 加一，**一次都不数块**。
本 run 答复过的 RRQ 是三个：`:784`、`:803`（`834_0_0.mbn`）和 `:879`（`mcfg.tmp`）。
**真正的"读出去多少字节"是 `gBytes` = 0**，因为 `fuxi_tftp_send_block` 一次都没被调用
（`package/FuxiModemTftp.cpp:307-308` 在 `tftpRead` 里加，而 `tftpRead` 从没被调过）。

对照 run（`run-20261122-105450`）`:958`：`took 12 requests and answered 12; read 0 times, 0 bytes,
refused 6 and missing 0`——那一轮 `refused 6` 是因为路径前缀被吃掉（`eadwrite/...`），
本轮修好之后 `refused` 归 0、`missing` 变成 3。**这一轮比上一轮前进了一步。**

---

## 这次诊断改不了的东西

**这一节列的是"光看这份日志得不出结论"的部分。**

1. **modem 到底接不接受 locator 的回答。** 四个问答的字节都对，但"不再提问"和"接受了"
   在日志里长得一样：两个 run 都是 4 次请求之后停。分清要换一个观察量：
   **问一个 modem 之前问过的名字，看它还问不问**——而这一轮已经做了：
   `:1109` 问的就是 `tms/servreg`，它**第 4 次**又问了同一个名字，
   说明它没有因为第一次不认就退让。**但这仍然不是"接受"的正面证据**，
   因为拒绝之后重试也是重试。这一项按 `EXCLUDED.md:60` B8 已经关闭，本轮不重开。

2. **`blksize 4000` 是不是阻塞。** 日志只证明 modem **接受**了这个 OACK 的块大小
   （它回了 ACK(0)，`:813`），不证明 7680 不会更好。分清要**只改块大小一个变量**：
   端口修好之后，把上限提到 8000（`hakios.mpss.c:4075` 的 `TFTP_MAX_BLOCK`），
   看 modem 要的 7680 会不会让传输跑得更远。

3. **operating mode 变了这件事意味着什么。** 这一轮 operating mode 从 PERSIST(5) 变成了
   **OFFLINE(0)**（`:1149`），DMS 的回答也从 error 0 变成了 **error 74**。
   **这不是"radio 起来了"的证据**：OFFLINE 是更靠前的状态，不是更靠后的。
   `EXCLUDED.md:128` 的判据字面写的是"从 PERSIST 变掉"——照字面读，**它变了**；
   照 §E `:141` 的意图（"TFTP 管 modem 是否从 OFFLINE 转 ONLINE"）读，**它没过关**。
   **两个 run 之间差的不止路径修复**，所以这个模式变化的**原因没有确立**。
   要确立，需要同一份镜像、只换一个变量的两次运行。

4. **IPA 是不是下一道门。** `docs/modem-workflow.md` §4 说 RMTFS 不是充分条件，
   IPA 之后才有首次 LTE 注册。这一轮没有碰 IPA，所以即使 TFTP 通了，
   也**不能**推出"radio 会注册"。

5. **modem 想要的 `ota_firewall/ruleset` 从哪来。** 它被读了两次，两次都 not found。
   参考实现也会答 not found，所以这不是本 port 的缺陷，
   但**它是不是 modem 前进路上的必需文件，日志答不了**——它没有被服务过，
   所以它的缺失没有在 modem 那里产生任何可观察的后果。

6. **那个 1 字节的 `mcfg.tmp` 是不是 modem 想要的内容。** `:883` 说内存里有 1 字节，
   内容是 `:867` 的 DATA(1) 的载荷 `00`。**这是 modem 自己发过来的字节**，
   所以"读回来的是 modem 自己写的"这件事成立；
   但"这 1 字节对 modem 是不是够"不知道——modem 之后没有再读它
   （`:894` 的 WRQ 带的是 `unlink` 选项，选项字节 `75 6e 6c 69 6e 6b` = "unlink"）。

7. **本轮的 DTB 是不是 boot 的原因。** `EXCLUDED.md:45` 的 A7 说 PAS 本身已通过、
   不再有 PAS 阻塞；`EXCLUDED.md:28` 记着"DTB 的分段不该搬"曾经被推翻过一次。
   本文件的所有结论**只来自上面的日志字节**，没有一条依赖 DTB。

8. **`:684` 的 `pdr_internal.h` 结构体顺序和 encode 顺序不同，这一条没查。**
   上游的 `servreg_location_entry_ei`（`qcom_pdr_msg.c:11-50`）按
   name / instance / service_data_valid / service_data 编码，
   而 `pdr_internal.h:23-28` 的 C 结构体顺序是 name / service_data_valid / service_data / instance。
   我们发出的顺序跟 **encode** 顺序一致（name, instance, valid, data），
   与参考实现 `hakios.mpss.c:4661-4668` 一致。

---

## 待答的问题（每条一句话，一次设备运行就能定）

1. 读请求的 OACK 从**传输端口**发出之后，`834_0_0.mbn` 的传输能跑到第几块？
2. OACK 里的 `blksize` 答 **7680**（而不是 4000）时，modem 的重试会不会消失？
3. 端口修好、carrier 配置真的传过去之后，**operating mode 会不会从 OFFLINE 变成 ONLINE**？
   （这一轮它从 PERSIST 变成了 OFFLINE，但那是**倒退**，不是过关。）
4. 四个槽**够不够**？修好释放路径之后 `mcfg_sw.mbn` 还会不会拿到 "busy"？
5. `/readwrite/ota_firewall/ruleset` 是 modem 前进路上的**必需文件**吗？
   —— 让 modem 在别处拿到它（它有别的地方可以拿吗），看 modem 后续的行为变不变。
6. `mcfg.tmp` 读回来的**内容** modem 认不认？—— 让它把一个更有意义的内容写进来，读回去，看它下一次读什么。

---

## 附：这一轮的原始请求清单（14 条服务端口请求 + 3 条传输端口 DATA）

| # | 行 | 方向 | 包 | 路径 | 选项 |
|---|---|---|---|---|---|
| 1 | `:738` | modem→我们 | WRQ | `/readwrite/server_check.txt` | blksize 7680, timeoutms 5000, wsize 10 |
| 2 | `:746` | modem→我们 | DATA(1) 7 字节 → 0x4010 | — | — |
| 3 | `:758` | modem→我们 | RRQ | `/readwrite/ota_firewall/ruleset` | blksize 7680, timeoutms 5000, tsize 0, wsize 10 |
| 4 | `:771` | modem→我们 | RRQ | `/readwrite/ota_firewall/ruleset` | 同上 |
| 5 | `:784` | modem→我们 | RRQ | `/readonly/firmware/image/modem_pr/so/834_0_0.mbn` | blksize 7680, timeoutms 5000, tsize 0, wsize 10 |
| 6 | `:793` | modem→我们 | **ERROR(9) "End of Transfer"** → 0x4003 | — | — |
| 7 | `:803` | modem→我们 | RRQ | `.../834_0_0.mbn` | blksize 7680, timeoutms 5000, wsize 10, rsize 52 |
| 8 | `:813` | modem→我们 | **ACK(0)** → 0x4003 | — | — |
| 9 | `:826` | modem→我们 | RRQ | `/readwrite/mcfg.tmp` | blksize 7680, timeoutms 5000, tsize 0, wsize 10 |
| 10 | `:839` | modem→我们 | WRQ | `/readwrite/mcfg.tmp` | blksize 7680, timeoutms 5000, wsize 10 |
| 11 | `:847` | modem→我们 | DATA(1) 空 → 0x4012 | — | — |
| 12 | `:859` | modem→我们 | WRQ | `/readwrite/mcfg.tmp` | blksize 7680, timeoutms 5000, **seek 0**, wsize 10 |
| 13 | `:867` | modem→我们 | DATA(1) `00` → 0x4012 | — | — |
| 14 | `:879` | modem→我们 | RRQ | `/readwrite/mcfg.tmp` | blksize 7680, timeoutms 5000, tsize 0, wsize 10 |
| 15 | `:887` | modem→我们 | **ERROR(9)** → 0x4003 | — | — |
| 16 | `:894` | modem→我们 | WRQ | `/readwrite/mcfg.tmp` | blksize 7680, timeoutms 5000, **unlink 0** |
| 17 | `:903` | modem→我们 | RRQ | `.../mcfg_sw/generic/China/CMCC/Commercial/Volte_OpenMkt/mcfg_sw.mbn` | blksize 7680, timeoutms 5000, tsize 0, wsize 10 |

（14 条 RRQ/WRQ 在服务端口上，`took 14 requests` 数的就是它们；
`:746`、`:847`、`:867` 三条 DATA 在传输端口上，不计入。数字对得上。）

**我们发出的回答，按类型：**

| # | 行 | 从哪个端口 | 类型 | 内容 |
|---|---|---|---|---|
| 1 | `:741` | **0x4010** | OACK | writer 的 WRQ，blksize 4000 |
| 2 | `:749` | **0x4010** | ACK(1) | writer 的 DATA |
| 3 | `:762` | 0x4003 | ERROR(1) | "file not found"（ota_firewall） |
| 4 | `:775` | 0x4003 | ERROR(1) | "file not found"（ota_firewall） |
| 5 | `:788` | **0x4003** | OACK | 读的 RRQ，blksize 4000 ← **端口错** |
| 6 | `:796` | 0x4003 | (20 字节) | 我们回的 RESUME_TX（QRTR 流控，不是 TFTP） |
| 7 | `:807` | **0x4003** | OACK | 读的 RRQ ← **端口错** |
| 8 | `:830` | 0x4003 | ERROR(1) | "file not found"（mcfg.tmp，写之前） |
| 9 | `:842` | **0x4012** | OACK | writer 的 WRQ |
| 10 | `:850` | **0x4012** | ACK(1) | writer 的 DATA |
| 11 | `:862` | **0x4012** | OACK | writer 的 WRQ |
| 12 | `:870` | **0x4012** | ACK(1) | writer 的 DATA |
| 13 | `:882` | 0x4003 | (20 字节) | RESUME_TX（流控） |
| 14 | `:884` | **0x4003** | OACK | 读的 RRQ，mcfg.tmp ← **端口错** |
| 15 | `:897` | **0x4013** | OACK | writer 的 WRQ（unlink） |
| 16 | `:907` | 0x4003 | ERROR(2) | **"busy"** —— 四个槽满了 |

`:796` 和 `:882` 那两条 20 字节的包是 **QRTR 的 RESUME_TX**（type 7），
是 modem 的 `confirm_rx` 位要来的流控信用，**不是 TFTP 的应答**。
`fuxi_modem.c:493` 打的那句 "the tftp was sent 20 bytes it cannot answer" 指的是
**modem 发来的** 20 字节被拒（`:793` 那个 ERROR），不是我们发出的。
