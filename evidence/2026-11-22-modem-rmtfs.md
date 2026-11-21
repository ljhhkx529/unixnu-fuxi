# The modem reads its four files through a shared buffer

**日期：2026-11-22。** 本轮任务：实现只读的 RMTFS 服务器（QRTR service 14），让 modem 读到它自己的四个文件系统。

**结果：判据成立，扇区真的搬了，而且 modem 的整个栈跟着起来了。**
modem 发来了 `OPEN /boot/modem_fs1` 和 `OPEN /boot/modem_fs2`，
随后发来 ALLOC_BUFF 和 RW_IOVEC；这一轮在 assign 被接受之后，读了 5120 个扇区、写回 5120 个扇区。

**更大的结果**：assign 被接受之前 modem 只发布 2 个服务、UIM（0x0b）根本没有被发布；
assign 被接受之后它发布 48 个服务，UIM 在里面。
**RMTFS 就是卡住 modem 栈的那道门**——不是 IPA，也不是 TFTP。

两轮上机。第一轮证明协议对、assign 被拒；第二轮用一次扫描把拒绝的原因定下来；第三轮把定下来的形状
装回 port 再跑一次确认。镜像每次只刷 `boot` 分区（`fastboot flash boot`，serial `b4c62a1e`），
没有刷过任何其它分区。

| 项 | 值 |
|---|---|
| run id（第三轮，定形） | `run-20261122-045750`（`/private/tmp/fuxi-usb/run-20261122-045750/xnu-usb.log`） |
| run id（第二轮，扫描） | `run-20261122-044925`（`/private/tmp/fuxi-usb/run-20261122-044925/xnu-usb.log`） |
| run id（第一轮，判据） | `run-20261122-033419`（`/private/tmp/fuxi-usb/run-20261122-033419/xnu-usb.log`） |
| kernel sha256 | `30c630a413545c30344f3b1eb40c018e991eb6da66d7eb61ed947d5b5f0cfbc5` |
| payload.img sha256 | `da1869c8fa3c044dceb5388e190f2958d97a093e906a7346720c3fe28dfe2005` |
| boot image | `/tmp/fuxi-marker.img`（94 625 792 字节） |
| boot image sha256 | `873d3f0cb2f80994b1e4b71a81091ae5318d11e56bfd69844c812847a5514bed` |
| 驱动仓 commit | `786ea57`（UniXNU `main`，protocol 半） |
| 设备仓 commit | `e219186`（unixnu-fuxi `V2`，adapter 半） |
| 启动参数 | `fuxi-modem=15`，其余不变 |

## 判据：modem 发出 `OPEN /boot/modem_fs1`

**成立。** 三轮都成立，原样字节：

```
FUXI_MODEM: the rmtfs: the modem asks OPEN (message 1), 25 bytes
FUXI_MODEM: the rmtfs: the first OPEN is 25 bytes: 00 01 00 01 00 12 00 01 0f 00 2f 62 6f 6f 74 2f 6d 6f 64 65 6d 5f 66 73 31
```

拆开：类型 `00`（请求），事务 `01 00`，消息 id `01 00` = OPEN，TLV 总长 `12 00` = 18，
TLV 类型 `01`，长度 `0f 00` = 15，值是 ASCII `/boot/modem_fs1`。

**路径就是 TLV 1 的值本身，前面没有长度前缀** —— 和参考实现（`hakios.mpss.c:3765-3925`）
说的一样。第二轮里 modem 也发了 `OPEN /boot/modem_fs2`。

## 四个文件的清单

partition 是 GPT 里的名字，路径是 modem 问的路径。**没有一个字节被写过。**
分区是用 `native_ufs_part_read` 读进来的，只读；写请求只改内存里的副本。

```
FUXI_RMTFS: the four files
FUXI_RMTFS: the partition modemst1 (/boot/modem_fs1) is 8388608 bytes, read into memory of ours
FUXI_RMTFS: the partition modemst2 (/boot/modem_fs2) is 8388608 bytes, read into memory of ours
FUXI_RMTFS: the partition fsc (/boot/modem_fsc) is 1048576 bytes, read into memory of ours
FUXI_RMTFS: the partition fsg (/boot/modem_fsg) is 8388608 bytes, read into memory of ours
FUXI_RMTFS: 4 of 4 files are there
```

四个都在，没有一个是猜的名字。

## 缓冲区与 assign 的结果

缓冲区的地址是 `0x9c900000`，大小 `0x280000`（2 620 480 字节）——设备树自己的
`qcom,rmtfs_sharedmem@0` 节点写的就是 `reg = <0x00 0x280000>`，参考实现也是这个大小
（`RMTFS_BUFFER_SIZE 0x280000`）。地址在 4 GiB 以下，这是必须的：modem 的请求里带的是 32 位地址。

```
FUXI_RMTFS: the buffer is 9c900000 of 2621440 bytes, mapped write-combine
```

**这个大小是 modem 自己要出来的**，不是我们猜的。ALLOC_BUFF 的请求字节：

```
FUXI_MODEM: the qrtr packet is 56 bytes: 01 00 00 00 01 00 00 00 00 00 00 00 0b 00 00 00 01 00 00 00 26 00 00 00 01 00 00 00 02 40 00 00 00 01 00 04 00 0e 00 01 04 00 01 00 00 00 02 04 00 00 00 28 00 00 00 00
```

TLV 1 = `01 00 00 00`（要一个缓冲区），TLV 2 = `00 00 28 00` = 0x00280000。
**modem 要的字节数就是我们手上这块窗口的字节数**，协议和大小两边都对上了。

### assign 的形状：第一轮被拒，第二轮定因

参考实现 `rmtfs_share` 交的是三个 owner：HLOS、MSS_MSA、NAV（`option("modem_no_nav") ? 2 : 3`）。
第一轮照抄三个，安全世界拒了：

```
FUXI_MODEM: the assign rmtfs hands 9c900000 of 2621440 bytes to 3 owner(s): 3 15 43
FUXI_MODEM: the assign rmtfs's answer is 4294967294/86
FUXI_MODEM: the assign rmtfs was refused (scm 4294967294/86)
```

x0 = 4294967294 = 有符号 **-2**，x1 = 86。这个拒绝本身不说原因。可能出错的只有两处：
**owner 的个数**和**内存本身**。

第二轮把两种可能一次分开（四种形状，同一份内存映射、同一套调用形状）：

```
FUXI_MODEM: the assign rmtfs hands 9c900000 of 2621440 bytes to 3 owner(s): 3 15 43
FUXI_MODEM: the assign rmtfs's answer is 4294967294/86
FUXI_RMTFS: the shape the compute DSP's region, three owners was refused
FUXI_MODEM: the assign rmtfs hands 9c900000 of 2621440 bytes to 2 owner(s): 3 15
FUXI_MODEM: the assign rmtfs's answer is 0/0
FUXI_MODEM: the assign rmtfs was accepted
FUXI_RMTFS: the modem may use the buffer: 9c900000 of 2621440 bytes, shared as the compute DSP's region, two owners
```

**结论：被拒的是第三个 owner（NAV，VMID 43），不是内存。** 同一块地址 `0x9c900000`、
同样的大小、少一个 owner，立刻就接受了。

设备树也是这么说的：它声明的虚拟机是 0x3、0x2d、0x31、0x32，**树里没有 0x2b**，
它自己的 `qcom,rmtfs_sharedmem@0` 节点也没有 `qcom,vm-nav-path`。参考实现里
`modem_no_nav` 这个选项就是给这样的板子用的。

**同一个 helper 的另一处 assign（DSM，0xd4d00000）一直成功**，用的是同一份 arginfo（0x1117）
和同一个寄存器形状，所以 port 自己的 block 排布、ext block、cleanToPoc 都不是原因。

### 定形之后的确认跑（第三轮）

把两 owner 装回 port 再跑，assign 第一次就过：

```
FUXI_MODEM: the assign rmtfs hands 9c900000 of 2621440 bytes to 2 owner(s): 3 15
FUXI_MODEM: the assign rmtfs's answer is 0/0
FUXI_MODEM: the assign rmtfs was accepted
FUXI_RMTFS: the modem may use the buffer: 9c900000 of 2621440 bytes
```

## 扇区真的搬了

第三轮的计数器（每秒一行，稳定不涨）：

```
FUXI_MODEM: 109 s: the rmtfs was announced 1 times, took 7 requests, answered 6; opened 2 closed 0, 5120 reads and 5120 writes of 5120 and 5120 sectors
```

**读 5120 个扇区、写 5120 个扇区。** 第一轮是 0 读 0 写（assign 被拒，ALLOC_BUFF 的回答里地址是 0，
所以 modem 一个 RW_IOVEC 都没发）。

### `answered 6` 比 `took 7` 少一个：一个被丢掉的回答

计数器写着 **7 个请求、6 个回答**。少的那一个是第 4 条 RW_IOVEC，也就是那条**写**请求
（方向 1，5120 个扇区）——它的扇区搬完了（计数器的 5120 写就是它），
但**它的回答没有发出去**。

原因在 QRTR 层，不在 RMTFS：**QRTR 的流控表按目的地记账，modem 每开一条流就换一个源端口。**
第三轮那 7 个 RMTFS 请求分别来自 7 个不同的源端口（8、9、11、12、13、14、110），
整轮跑下来 modem 一共用过 **9 个源端口**，而 port 的流控表只有 **8 个槽**。
表满了，第 9 个目的地（端口 110，也就是那条写请求）的回答就被拒了，
**日志上什么也看不出来**——那句"the rmtfs's answer was refused"没有出现，
因为它不是 RMTFS 拒的。

这个缺陷已经修掉（UniXNU commit `0826e10`）：表放宽到 32 个槽，
一个信用还干净的槽可以被复用，报告里也带上"在用几个目的地"，
这样表快满的时候看得见。**这个修复没有上机验证**——修完的时候设备已经不在 USB 上了。

5120 个扇区 × 512 字节 = 2 620 480 字节，正好是整个缓冲区大小：modem 把 fs1 和 fs2 都搬了一遍。

## RW_IOVEC 的原始字节

**这个 dump 被截断了：日志里只有 72 字节包的前 64 字节**，entry 的尾部（地址和扇区数）
没有进日志。下面这段是从**有**的字节里读出来的，剩下的字段是标出来的推断，不是观测。

```
FUXI_MODEM: the qrtr packet is 72 bytes: 01 00 00 00 01 00 00 00 00 00 00 00 0c 00 00 00 00 00 00 00 26 00 00 00 01 00 00 00 02 40 00 00 00 01 00 03 00 1f 00 01 04 00 01 00 00 00 02 01 00 00 03 0d 00 01 01 00 00 00 00 02 90 9c 01 00
FUXI_MODEM: the qrtr packet is 72 bytes: 01 00 00 00 01 00 00 00 00 00 00 00 0d 00 00 00 01 00 00 00 26 00 00 00 01 00 00 00 02 40 00 00 00 01 00 03 00 1f 00 01 04 00 02 00 00 00 02 01 00 00 03 0d 00 01 01 00 00 00 00 02 90 9c 01 00
FUXI_MODEM: the qrtr packet is 72 bytes: 01 00 00 00 01 00 00 00 00 00 00 00 0e 00 00 00 00 00 00 00 26 00 00 00 01 00 00 00 02 40 00 00 00 01 00 03 00 1f 00 01 04 00 01 00 00 00 02 01 00 00 03 0d 00 01 02 00 00 00 00 04 90 9c fe 13
FUXI_MODEM: the qrtr packet is 72 bytes: 01 00 00 00 01 00 00 00 00 00 00 00 6e 00 00 00 00 00 00 00 26 00 00 00 01 00 00 00 02 40 00 00 00 01 00 03 00 1f 00 01 04 00 02 00 00 00 02 01 00 01 03 0d 00 01 00 00 00 00 00 00 90 9c 00 14
```

body 去掉 7 字节头之后（**这些是从上面那 64 个字节里直接读出来的**）：

| 位置 | 字节 | 含义 |
|---|---|---|
| 0 | `00` | 请求 |
| 1–2 | `01 00` / `02 00` / `0e 00` / `6e 00` | 事务 |
| 3–4 | `03 00` | 消息 id 3 = RW_IOVEC |
| 5–6 | `1f 00` | TLV 总长 31 |
| TLV 1 | `01 04 00 01 00 00 00` | 文件号 = 1（第一条 = modem_fs1） |
| TLV 1（第四条） | `01 04 00 02 00 00 00` | 文件号 = 2（= modem_fs2） |
| TLV 2 | `02 01 00 00` | 方向 = 0（读） |
| TLV 2（第四条） | `02 01 00 01` | 方向 = 1（写） |
| TLV 3 | `03 0d 00 ...` | 13 字节：count = 1，然后一条 entry |

TLV 3 的 13 字节里只有前 9 个字节在日志里：`01`（count = 1），然后 entry 的头两个 u32
（扇区号、地址），**entry 的第三个 u32（扇区数）没有进日志**。

**entry 是 12 字节，这一点是确定的，不是推的**：TLV 3 声明的长度是 13，
而 count 的 1 字节加上三个 u32（扇区号、地址、扇区数）正好是 13。和参考实现说的一样。
TLV 区的总长是 31（`1f 00`），body 是 38 字节，38 − 7（body 头）= 31，自洽。
TLV 1（3 + 4 = 7）+ TLV 2（3 + 1 = 4）+ TLV 3（3 + 13 = 16）= 27，
所以 TLV 区的最后 4 个字节是**第四个 TLV**（3 字节头 + 1 字节值），
它没有进日志，内容不明。

四条请求合起来（地址都是 32 位，`physical` 是缓冲区里的绝对地址）：

| # | 文件号 | 方向 | 扇区号 | 地址 | 扇区数 |
|---|---|---|---|---|---|
| 1 | 1（modem_fs1） | 0（读） | 1 | `0x9c900200` | 1 |
| 2 | 2（modem_fs2） | 0（读） | 1 | `0x9c900200` | 1 |
| 3 | 1（modem_fs1） | 0（读） | 2 | `0x9c900400` | 0x13fe = 5118 |
| 4 | 2（modem_fs2） | **1（写）** | 0 | `0x9c900000` | 0x1400 = 5120 |

**扇区数的高 16 位没有进日志，但它们只能是 0，这一点是证明过的**：
第 3 条从偏移 0x400 起搬 5118 个扇区，5118 × 512 = 0x27fc00，
加上 0x400 正好到 0x280000；第 4 条从偏移 0 起搬 5120 个扇区，
5120 × 512 = 0x280000。两条都在缓冲区的末尾一个字节不差地收住。
**如果高 16 位不是 0，搬的字节就会超出缓冲区，而 port 的校验会拒掉那个请求**
（`physical - buffer + bytes > buffer_size`），计数器就不会是 5120。
计数器读的就是 5120 读 / 5120 写，所以高 16 位是 0。

**这组数字和计数器严丝合缝**：读 = 1 + 1 + 5118 = **5120**，写 = **5120**。

**要把它变成直接的观测，需要再跑一次**（dump 上限已经改成整包，
UniXNU commit `6d50651`）。第 4 次刷机后设备没有回到 USB，那次跑没做成。

## 这一轮没有发生的事

- **没有写过 modemst1 / modemst2 / fsc / fsg 的任何一个字节。** 分区是用只读的
  `native_ufs_part_read` 读进内存的，那个接口根本没有写的一头。写请求只改内存副本。
- modem 的 SMP2P 仍然只有两条：`smp2p` 值 0x0，`slave-kernel` 值 0x6。
- 没有发过 SSCTL 0x0020/0x0021，也没有发过 SERVREG_NOTIF 0x0024。
- 没有做 TFTP，也没有做 IPA。本轮任务就只到 RMTFS。

## modem 的栈起来了：48 个服务，UIM 在里面

**这一轮最大的结果不在 RMTFS 这一层，在它之后。** 三轮跑下来，差别只在 assign 的形状：

| run | assign | rmtfs 计数 | QRTR 服务数 | 发布的 0x0b |
|---|---|---|---|---|
| 033419 | 被拒 | 0 读 / 0 写 | 2 | 没有 |
| 044925 | 接受（2 个 owner） | 5120 / 5120 | 48 | **有** |
| 045750 | 接受（2 个 owner） | 5120 / 5120 | 48 | **有** |

第一轮和第三轮之间只差一个东西：那个 assign 的形状。第一轮 modem 只发布两个服务
（0x42 SERVREG_NOTIF、0x2b SSCTL），UIM 根本没有被发布，port 问了三次都没人答。
第三轮 modem 发布了 **48 个服务**，UIM（0x0b）在里面：

```
COACHZ baseband: 27 s: the modem offers service 0x0b, version 1, instance 0, at node 0 port 60
COACHZ baseband: 27 s: 44 QMI services, 2 clients, dropped 0/0; ...
```

**RMTFS 就是卡住 modem 栈的那道门。** 这不是 IPA，也不是 TFTP。
这一轮之前 port 里那条"IPA 排在 TFTP 前面"的下一步，是被这个结果顶掉的。

**48 这个数**：QRTR 层记的 server 数是 48，桥自己的表里是 44 个 QMI 服务。
差的 4 个是同一个服务 id 的不同版本/实例（0x09 和 0x0a 是 version 2、
0x17 有 version 1 和 2、0x2b 是 version 2、0x10 是 version 2）。
按 service id 去重是 **43 个**，其中 UIM 是 0x0b。

modem 发布的 43 个服务 id：

```
0x01 0x02 0x03 0x04 0x05 0x07 0x08 0x09 0x0a 0x0b 0x0c 0x0f 0x10 0x11 0x15 0x16 0x17
0x18 0x1a 0x1d 0x22 0x24 0x2a 0x2b 0x2e 0x2f 0x30 0x31 0x32 0x33 0x37 0x42 0x44 0x49
0x4a 0x4c 0x4d 0x4e 0x55 0x56 0x57 0x58 0xe4
```

**它们分两批到**：27 s 那一批 43 个里的 42 个一次全到（2 s 时先到了 11 个），
0x10 是 51 s 才到的。UIM 在 27 s 那一批里。

**port 问 UIM 的那三次 lookup 依然没有答案**：modem 是自己发布 UIM 的，
不是被 port 问出来的。所以下一步不是"再问一次"，而是**自己去找 UIM 的实例**
（node 0 port 60），往那个端口发一个只读的请求。

## 结论与下一步

**RMTFS 这一层是通的**：modem 认我们这个服务，会读它的文件，扇区真的搬了。
而且它不只是通：**它把 modem 的整个栈带起来了**，modem 从发布两个服务变成发布 48 个，
UIM 在里面。

所以本轮任务规定的"判据过了就停"是成立的，但**下一步不是 IPA**：
既然 UIM 已经发布了，下一步就是**绑到 UIM 上问它卡的状态**，
而不是去补 IPA 或 TFTP。卡状态、ICCID、IMSI 都在 UIM 里。

modem 一直在报的 `failure reason: error 94, 0 bytes of reason` 还没有解释，
0 字节的理由是 modem 自己说的，它现在不再挡着 UIM 了，但原因还没找到。
