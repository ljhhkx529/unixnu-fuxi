# The modem reads its four files through a shared buffer

**日期：2026-11-22。** 本轮任务：实现只读的 RMTFS 服务器（QRTR service 14），让 modem 读到它自己的四个文件系统。

**结果：判据成立，且扇区真的搬了。** modem 发来了 `OPEN /boot/modem_fs1` 和 `OPEN /boot/modem_fs2`，
随后发来 ALLOC_BUFF 和 RW_IOVEC；这一轮在 assign 被接受之后，读了 5120 个扇区、写回 5120 个扇区。

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

5120 个扇区 × 512 字节 = 2 620 480 字节，正好是整个缓冲区大小：modem 把 fs1 和 fs2 都搬了一遍。

## RW_IOVEC 的原始字节

```
FUXI_MODEM: the qrtr packet is 72 bytes: 01 00 00 00 01 00 00 00 00 00 00 00 0c 00 00 00 00 00 00 00 26 00 00 00 01 00 00 00 02 40 00 00 00 01 00 03 00 1f 00 01 04 00 01 00 00 00 02 01 00 00 03 0d 00 01 01 00 00 00 00 02 90 9c 01 00
FUXI_MODEM: the qrtr packet is 72 bytes: 01 00 00 00 01 00 00 00 00 00 00 00 0d 00 00 00 01 00 00 00 26 00 00 00 01 00 00 00 02 40 00 00 00 01 00 03 00 1f 00 01 04 00 02 00 00 00 02 01 00 00 03 0d 00 01 01 00 00 00 00 02 90 9c 01 00
FUXI_MODEM: the qrtr packet is 72 bytes: 01 00 00 00 01 00 00 00 00 00 00 00 0e 00 00 00 00 00 00 00 26 00 00 00 01 00 00 00 02 40 00 00 00 01 00 03 00 1f 00 01 04 00 01 00 00 00 02 01 00 00 03 0d 00 01 02 00 00 00 00 04 90 9c fe 13
```

body 去掉 7 字节头之后：

| 位置 | 字节 | 含义 |
|---|---|---|
| 0 | `00` | 请求 |
| 1–2 | `01 00` / `02 00` | 事务 |
| 3–4 | `03 00` | 消息 id 3 = RW_IOVEC |
| 5–6 | `1f 00` | TLV 总长 31 |
| TLV 1 | `01 04 00 01 00 00 00` | 文件号 = 1（第一条 = modem_fs1） |
| TLV 2 | `02 01 00 00` | 方向 = 0（读） |
| TLV 3 | `03 0d 00 ...` | 13 字节：count = 1，然后一条 entry |

entry 12 字节：`01 00 00 00`（扇区号 1）、`00 02 90 9c`（地址 = 0x9c900200）、
`01 00 00 00`（扇区数 1）。第二条 entry 是扇区 2、地址 `0x9c900400`。
第三行是同一个文件的扇区 2、地址 `0x9c9000fe`（`04 90 9c fe` = 0x9c9cfe04 之后…见下），
方向同样是 0。

**地址落在缓冲区里**：`0x9c900200` = 缓冲区起点 + 0x200。地址是 32 位，
所以缓冲区的低 4 GiB 之内是硬要求。

## 这一轮没有发生的事

- **没有写过 modemst1 / modemst2 / fsc / fsg 的任何一个字节。** 分区是用只读的
  `native_ufs_part_read` 读进内存的，那个接口根本没有写的一头。写请求只改内存副本。
- modem 仍然只发布两个服务（0x42 SERVREG_NOTIF、0x2b SSCTL），**UIM（0x0b）仍然没有发布**：
  port 问了三次（version 0 instance 0 到 node 7907、version 1 instance 0 到 node 0、
  version 1 instance 2 到 node 7907），一次都没有被回答。
- modem 仍然问了 locator 里的 `tms/servreg`（1 个 domain）和 `tms/pddump_disabled`（0 个 domain），
  两个都回答了。
- modem 的 SMP2P 仍然只有两条：`smp2p` 值 0x0，`slave-kernel` 值 0x6。
- 没有发过 SSCTL 0x0020/0x0021，也没有发过 SERVREG_NOTIF 0x0024。
- 没有做 TFTP，也没有做 IPA。本轮任务就只到 RMTFS。

## 结论与下一步

**RMTFS 这一层是通的**：modem 认我们这个服务，会读它的文件，扇区真的搬了。
但 modem 的栈还是没有起来——UIM 没有发布，SIM 还是读不到。

**RMTFS 不是充分条件**：modem 拿到自己的文件之后，仍然没有把 UIM 发出来。
所以下一步要么是 IPA（不是 TFTP——按 EXCLUDED.md 的分级，IPA 排在 TFTP 前面），
要么是别的东西。按本轮任务的规定，如果判据过了就停在这里；如果它没过，
结论应该是"RMTFS 在这里不够"，而不是"RMTFS 没有用"。

判据是过了的。所以这一轮停在 RMTFS 通这一层，下一轮走 IPA。
