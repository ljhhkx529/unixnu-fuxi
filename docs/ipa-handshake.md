# 基带的 IPA 握手：已经做到哪一步

2026-11-23。Xiaomi 13 (fuxi, SM8550) 的 XNU 移植，QRTR 上 IPA host service 0x31 的实测记录。

## 一句话

0x31 是第四个 AP 侧服务，我们一直没发布它。发布之后 modem 立刻发来 `0x20`；我们答了成功之后它就不动了，等 AP 发 `0x21`。

## 四个服务（参考实现 `hakios.mpss.c:3998-4018`）

| 服务 | 端口 | 我们 |
|---|---|---|
| 14 RMTFS | 0x4002 | 有 |
| 4096 TFTP (v1/v3) | 0x4003 / 0x4004 | 有 |
| 64 locator | 0x4001 | 有 |
| **0x31 IPA host** | **0x4005** | 有（2026-11-23 加上） |

参考实现里 0x31 的 instance 字是 `0x101`（version 1 在低字节，instance 1 在高字节）。

## 握手（Linux `ipa_qmi.c`，参考实现 `hakios.mpss.c:4688-4707`）

```
(1) AP 宣布 0x31 v1 instance 1                            已做
(2) modem -> AP  0x20 INDICATION_REGISTER                  每次都来，11 字节
(3) AP -> modem 0x20 的 success 应答                       已做，modem 接受
(4) AP -> modem 的 0x31 instance 2 发 0x21 INIT_DRIVER      未做（见下）
(5) modem -> AP 0x35 DRIVER_INIT_COMPLETE -> AP 应答        待做
(6) AP 发 0x22 INIT_COMPLETE 指示                            待做
```

第 (4) 步没做的原因：`0x21` 的 TLV 里是 IPA 本地内存中各表的地址（`mem_offset + region.offset`），
而那块内存在这个移植里还没有映射。没有映射就发不出有意义的地址。

## 实测（run-20261123-001005）

```
383: the ipa host was announced: service 0x31 version 1 instance 1, on port 16389, to node 0
746: the ipa host service was asked message 0x0020, 11 bytes
748: the ipa host answered 0x0020 with a success result
```

报文原文（该 run 的 `:754-755`）：

```
01 00 00 00 01 00 00 00 00 00 00 00 21 00 00 00 00 00 00 00 0b 00 00 00
01 00 00 00 05 40 00 00 00 01 00 20 00 04 00 10 01 00 01 00
          src port 33              dst port 16389
QMI 体：00 01 00 20 00 04 00 10 01 00 01 00
        type 0, txn 1, msg 0x0020, len 4, TLV 0x10 len 1 = 01
```

## v5.5 的本地内存布局（第 (4) 步要用）

来源：`/tmp/x13/work/patches/modem16-epmap-20261031/ipa_data-v5.5.c.before-modem16`
（主线的 `drivers/net/ipa/data/ipa_data-v5.5.c`，`ipa_mem_local_data[]`，文件内 `:292-445`）。

| 区 | offset | size | canary |
|---|---|---|---|
| UC_EVENT_RING | 0x0000 | 0x1000 | 0 |
| UC_SHARED | 0x1000 | 0x0080 | 0 |
| UC_INFO | 0x1080 | 0x0200 | 0 |
| V4_FILTER_HASHED | 0x1288 | 0x0078 | 2 |
| V4_FILTER | 0x1308 | 0x0078 | 2 |
| V6_FILTER_HASHED | 0x1388 | 0x0078 | 2 |
| V6_FILTER | 0x1408 | 0x0078 | 2 |
| V4_ROUTE_HASHED | 0x1488 | 0x0098 | 2 |
| V4_ROUTE | 0x1528 | 0x0098 | 2 |
| V6_ROUTE_HASHED | 0x15c8 | 0x0098 | 2 |
| V6_ROUTE | 0x1668 | 0x0098 | 2 |
| MODEM_HEADER | 0x1708 | 0x0240 | 2 |
| AP_HEADER | 0x1948 | 0x01e0 | 0 |
| MODEM_PROC_CTX | 0x1b40 | 0x0b20 | 2 |
| AP_PROC_CTX | 0x2660 | 0x0200 | 0 |
| STATS_QUOTA_MODEM | 0x2868 | 0x0060 | 2 |
| STATS_QUOTA_AP | 0x28c8 | 0x0048 | 0 |
| STATS_TETHERING | 0x2910 | 0x03c0 | 0 |

同一文件的其它常数：

- `.modem_route_count = 11`（v4.2 是 8）
- `.imem_addr = 0x14688000`、`.imem_size = 0x2000`、`.smem_size = 0xb000`
- 但主线记录说 fuxi 要用 `0x146a8000`（`目标-M5.md:509`，DTB 改成厂商值才让基带稳定，
  是唯一让基带注册的那次改动）。文件里的是默认，实际生效的是 DTB 覆盖后的值，两者必须分清。

## 代码基础

| 文件 | 内容 | 缺口 |
|---|---|---|
| `ipa/coachz_ipa_qmi.h` | 0x20/0x21/0x22/0x35 的编解码器、状态机 | 内存表是 `ci_v42_mem`（v4.2） |
| `ipa/coachz_ipa_v55.h` | v5.5 的寄存器与端点表 | 没有本地内存布局 |
| `ipa/fuxi-endpoints.txt` | 端点 24 / 22 | `coachz_bb_data.h` 里仍是 v4.2 的 1 / 9 |
| `ipa/coachz_ipa_config.c` | 用 `ci_v42_mem` 写 canary、读 UC_SHARED | 同上 |

## 下一步的顺序

1. 映射 IPA 的寄存器、共享内存、GSI（设备树的 `qcom,ipa`），拿到 `mem_offset`。
2. 把上表抄成 `ci_v55_mem`（与 `ci_v42_mem` 同形状），`ci_init_driver_req` 加版本参数。
3. `lookup` 服务 0x31 instance 2，出现后发 `0x21`。
4. 等 `0x35`，应答；再发 `0x22`。
5. 端点号 1/9 换成 24/22。
6. 数据端口绑定（DPM Open Port / WDA Set Data Format / WDS Bind Mux），`coachz_bb_data.h` 已写好。

判据：`0x21` 之后 modem 回 `0x35`；之后 `data port step` 离开 0。

## 一条不能忘的警告

参考实现 `hakios.mpss.c:4694-4695`：

> 没有这个握手，modem 一进入 ONLINE，LTE 下行任务就会空指针崩溃（`enl2_dl`，三次三次）。

所以握手不是可选项：modem 一旦 ONLINE 而没有 IPA，它就会崩。

## 第 (1) 步的映射：已做，并已验证（2026-11-23）

`platform/fuxi/fuxi_ipa.c`（新增，已加进 `scripts/build.py` 的 fuxi 源列表）。
四个窗口来自设备树 `/soc/qcom,ipa@3e00000` 与 `/reserved-memory/`：

```
ipa-base 0x03e00000 0x84000    gsi-base 0x03e04000 0xfc000
ipa_fw_region@9b080000  0x9b080000 0x10000
ipa_gsi_region@9b090000 0x9b090000 0xa000
```

实机（`run-20261123-002238`）：

```
347: the ipa windows are mapped: reg 0x3e00000+0x84000, gsi 0x3e04000+0xfc000,
                                        fw 0x9b080000+0x10000, gsi_mem 0x9b090000+0xa000
348: the ipa registers are left unread until the IPA clock is voted (SHARED_MEM_SIZE pending)
349: the ipa fw region starts 00000000 00000000 00000000 00000000
350: the ipa gsi region starts 00000000 00000000 00000000 00000000
```

两个结论：

1. 四个窗口都映射成功，设备没有挂死（映射只建页表，不碰总线）。
2. `ipa_fw` 与 `ipa_gsi` 两个区全是零 —— 说明 IPA 固件**还没被装进去**（PIL 那个 `qcom,ipa_fws` 节点
   没有被走）。这和第 (4) 步缺的东西是同一件。

**故意没做的**：读 `CIPA_SHARED_MEM_SIZE`。`coachz_ipa_config.c:9-10` 写着 IPA 的 core clock 与
interconnect 必须先起（`wifi_ipa_power`），而本移植还没有 IPA 的时钟投票；读未上电的 AHB 块可能把总线挂死。
所以寄存器读取留给"上时钟"那一步。

## x13 的链条（设备树就是它的声明）与我们的差距

2026-11-23 实测：映射之后直接读 `GSI_STATUS`（0x12000）与 `SHARED_MEM_SIZE`（0x54），
日志停在 `QcomGlink: OPEN` 之后，硬件看门狗重启。已回滚为"只映射、不读"。

### fuxi 设备树里 IPA 节点的依赖（`/soc/qcom,ipa@3e00000`）

```
clock-names        core_clk
clocks             <&rsc_clock_controller 12>        ← RSC，不是 GCC
interconnect-names ipa_to_llcc, llcc_to_ebi1, appss_to_ipa
interconnects      /soc/interconnect@1700000, @1, @24100000   ← RPMh
```

### 主线的顺序

1. 时钟框架把 `core_clk`（RSC 时钟控制器的 12 号）投票打开；
2. `icc` 通过 RPMh 投三条带宽路径；
3. **然后** `ipa_probe` 才读 `SHARED_MEM_SIZE`、装 `ipa_fws.mdt`、配端点与表；
4. 这之后 modem 的 IPA QMI 握手才有意义。

**要点：IPA 的核心时钟来自 RSC/RPMh，不是 GCC 分支。** 拿 GPU 那套 GCC 写法类推是错的。

### 本移植已有的东西

`package/QcomRpmh.h` 是一套完整的 RPMh 投票器：

- `qcom_cmd_db_find(db, bytes, name, &resource)` —— 在 cmd-db 里按名字找资源，拿到 RPMh 地址与档位；
- `qcom_rpmh_level_index()` —— 把档位换成索引；
- `qcom_rpmh_active_request()` / `qcom_rpmh_finish()` —— 生成一个 TCS 的寄存器写序列并收尾。

而且**已经在用**：`FuxiAdspProbe.cpp` 用它给 ADSP 投 `lcx.lvl` / `lmx.lvl`（`:850-935`，`kRscTcs = 2`，等 TCS done）。

### 差距一句话

| | 时钟投票 | 带宽投票 | 然后才碰寄存器 |
|---|---|---|---|
| 主线 x13 | 有（RSC，经 RPMh） | 有（三条路径） | 是 |
| 本移植 IPA（之前） | **无** | **无** | **否** ← 重启的原因 |
| 本移植 ADSP | 有（同一套 RPMh 投票器） | — | 是 |

### 下一步（全部只读或复用现有代码）

1. **读 cmd-db 里的资源名**（只读）：把命令库里的名字列出来，找到 IPA 时钟与那三条路径的条目。
   cmd-db 已经能安全读（GPU 探针 `kCmdDbBase = 0x81c60000` 早已在用）。
2. 用 `qcom_cmd_db_find` + `qcom_rpmh_level_index` + `qcom_rpmh_active_request` 投时钟与带宽，
   照 `FuxiAdspProbe.cpp:885-935` 的做法等 TCS done。
3. **之后**再读 `GSI_STATUS` 与 `SHARED_MEM_SIZE`。

**判据**：投票成功后 IPA 寄存器可读、`GSI_STATUS & 1` 为 1、手机不重启。
## 2026-11-23 01:00-01:25：时钟找到了、投票成功了、读寄存器仍然不行

### 第 1 步：命令库整表（mode 17，`run-20261123-004711`）

`0x81c60000` 处 166 条资源，四个 slave（id 3/3/5/4）。全表已 dump（rails、NoC 节点、PMIC 的 `ldob/smpd/clka`）。
**没有任何 IPA 命名的时钟资源。**

### 找到的链条（每一个都是本机文件里的一行）

| 事实 | 出处 |
|---|---|
| IPA 的 `clocks = <&rpmhcc 12>` | `fuxi-live.dts:29416`（vendor）、主线 DTB 同样 |
| 该 controller 是 `qcom,kalama-rpmh-clk` | `fuxi-live.dts:1036`（phandle 0x3b） |
| 第 12 号是 `RPMH_IPA_CLK` | `include/dt-bindings/clock/qcom,rpmh.h:21` |
| 它对应命令库资源 **`IP0`** | `drivers/clk/qcom/clk-rpmh.c:412` — `DEFINE_CLK_RPMH_BCM(ipa, "IP0")` |
| 投票值是 BCM 命令字 `0x60000001` | `clk-rpmh.c:271` + `include/soc/qcom/tcs.h:68-79` |
| 本机命令库里 `IP0` 在 `0x00050040` | 第 1 步的 dump |

**所以 x13 的链条是：`clk_prepare_enable(core_clk)` → rpmh-clk → 命令库 `IP0` → RPMh TCS 投票。**

### 第 2 步：投票成功

`FuxiModemProbe.cpp` 的 `voteIpaClock()`，在 `stepRails()` 里、`voteRails()` 之后调用：

```
239: the ipa clock is IP0 at 0x00050040; its enable word is 0x60000001
245: the ipa clock vote completed
246: step: the pas ids          ← 探针继续走完了全部步骤
```

**投票是成功且无害的**：探针后续步骤（pas ids / smp2p / doorbells / images / link）全部正常，链路起来，
49 个 QMI 服务，TFTP 159/155。

### 但读寄存器仍然失败，而且这次拿到了确切原因

用户给出的内核异常：

```
Kernel data abort. at pc 0xfffffff00c02c9bc
esr: 0x96000010     far: 0xfffffffa33e90054
```

`FAR` 的低位 `0x3e90054` = IPA 窗口基址 `0x3e00000` + **`0x90054`**，而 `+0x54` 正是 `SHARED_MEM_SIZE` 的偏移。
**这不是总线挂死，是翻译错误：`ml_io_map(0x3e00000, 0x84000)` 返回了一个 VA，但那个 VA 的页表并不成立。**

所以之前的两次"挂死"（看门狗重启 / 探针停住）本质是同一个数据中止，只是表现不同。

**修法**：设备 MMIO 不能用 `ml_io_map` 这条路。本移植里**能读**的设备窗口都是探针的
`IODeviceMemory::withRange(...)->map(kIOMapAnywhere)`（RSC 的 `0x17a20000` 就是这样读 `drv_id` 的），
或者 `ml_io_map_wcomb`。下一步先把寄存器读取改成这条路，**只读一个字**（`SHARED_MEM_SIZE`）验证不中止。

### 抓包结果：投了时钟，modem 行为没有变化（`run-20261123-011734`，100 秒）

```
tftp : 159 requests / 155 answered / 12 reads / missed 4 / 0 send failures
ipa  : asked 1  (只有那条 0x0020)
radio: offline not registered
```

**完全静止。** 时钟不是 modem 等待的东西；它等的仍然是 AP 发出的 `0x21`，而 `0x21` 需要我们先能读 IPA 的本地内存。
## 2026-11-23 01:27 决定性一次：不是页表问题，是外部中止

mode 18（只投票 + 只读一个寄存器）的实机结果：

```
FUXI_MODEM: the ipa register window mapped (0xfffffff2215bf000 of 540672 bytes)
...
Kernel data abort. at pc 0xfffffff00c0287d0
       x21: 0xfffffff2215bf000        <- 窗口 VA，映射确实建立了
       esr: 0x96000010  far: 0xfffffff2215bf054
```

`ESR` 的 ISS = `0x10` → DFSC = `0b010000` = **Synchronous External Abort**（同步外部中止）。
**不是**翻译错误。

所以：**页表是好的，是 IPA 那个物理地址不响应。** 也就是说
**`RPMH_IPA_CLK`（cmd-db `IP0`）的票虽然 TCS 已 ack，IPA 块依然没有被打开。**

### 这一步推翻的两个假设

1. "读寄存器挂死是 `ml_io_map` 的页表问题" —— 不是。换成 `IODeviceMemory` + `kIOMapAnywhere` 照样中止，
   而且 VA 明显是建立好的。
2. "投了时钟就能读" —— 不能。时钟票完成 ≠ 块可访问。

### 还缺什么（候选，按可能性）

1. **IPA 的电源域**：除了 BCM 时钟，块可能还挂在一个 RPMh 电源域（`rpmhpd`）或 GDSC 后面。
   设备树里 IPA 节点没有 `power-domains` 属性，所以它不通过 DT 声明，需要在 GCC 里找 IPA 的 GDSC。
2. **BCM 投票值**：`0x60000001` 是从 `BCM_TCS_CMD(1, enable, 0, 1)` 推出来的；如果这个块的 tenant/valid
   位语义不同，票会被接受但不生效。
3. **GSI 块与 IPA 块是两个电源门**：`IPA_CFG` 与 `GSI` 可能各自独立。

### 现场状态

- mode 18 的镜像把手机留在了崩溃状态：不在 fastboot、USB 控制 socket 也没了。
- 安全镜像已备好：`/tmp/fuxi-safe.img`（sha256 `5def2529e8ae93aef8c708ecf09a5d024bc493ba03c39ed5e4d76c8f21048b23`），
  内容是 mode 15 + 时钟投票 + 不读寄存器；手机一进 fastboot 就刷它。
## IP0 这条链到底包含什么（对照 x13 的 `ipa_power`，2026-11-23）

### x13 的顺序（`ipa_main.c:830-889`）

```
830  /* The clock and interconnects might not be ready when we are probed, 
831     so this might return -EPROBE_DEFER. */
833  power = ipa_power_init(dev, data->power_data);
...
884  /* Power needs to be active for config and setup */
885  pm_runtime_get_sync(dev);
889  ipa_config(ipa, data);          <- 第一次写寄存器
```

### `ipa_power.c` 实际投的东西

| 动作 | 行 | 我们 |
|---|---|---|
| `of_icc_bulk_get()`（按名字取互联路径） | `:72` | **无** |
| `icc_bulk_set_bw()`（设带宽） | `:81` | **无** |
| `icc_bulk_enable()` | `:100` | **无** |
| `clk_prepare_enable(power->core)` | `:104` | 有（`IP0` 的 BCM 票） |
| `clk_set_rate(core, 120 MHz)` | `:248` | 不需要（BCM 时钟无频率） |

**顺序是：先 `icc_bulk_enable`，再 `clk_prepare_enable`。**

### v5.5 的互联数据（`ipa_data-v5.5.c:455-474`，本机文件）

```
{ .name = "memory", .peak_bandwidth = 1900000, .average_bandwidth = 600000 }  /* 1.9 GBps / 600 MBps */
{ .name = "config", .peak_bandwidth =   76800, .average_bandwidth = 0      }  /* 76.8 MBps */
.core_clock_rate = 120 * 1000 * 1000
```

**fuxi 的 vendor DTS 给的是三条路径**（`ipa_to_llcc` / `llcc_to_ebi1` / `appss_to_ipa`，
`fuxi-live.dts:29412-29413`），与主线的两个名字不同——移植要按 vendor 这三条来。

### `ipa_fws` 不需要我们装

`ipa_main.c:895-906`：只有 `loader == IPA_LOADER_SELF`（AP 自己装）才 `ipa_firmware_load()`；
fuxi 是 **`IPA_LOADER_MODEM`**（modem 装 GSI 固件），所以跳过。**这也正好对上 UMI 从来不装它。**

### 结论：IP0 只有时钟那一半，缺的是互联

| 资源 | 状态 |
|---|---|
| 时钟 `RPMH_IPA_CLK`（cmd-db `IP0`） | ✅ 已投，TCS 已 ack |
| **互联 `ipa_to_llcc` / `llcc_to_ebi1` / `appss_to_ipa`** | ❌ **完全没有** |
| 顺序（互联先于时钟） | ❌ 未实现 |

**这正好解释同步外部中止**：NoC 到 IPA 的那条路没打开，CPU 的访问被互联层拒绝，
返回的是外部中止而不是页错误——**时钟票无论投多少次都修不了。**

## 2026-11-23 01:45 互联投票完成，上机无中止

### 映射怎么来的

| 路径 | BCM | 命令库地址 | 出处 |
|---|---|---|---|
| config | CN0 | 0x00050038 | sm8550.c:1077-1108，bcm_cn0.name="CN0"，节点表含 qhs_ipa 与 qsm_cfg |
| memory | MC0 | 0x00050000 | sm8550.c:1130-1135，bcm_mc0.name="MC0"，节点 ebi |
| clock  | IP0 | 0x00050040 | clk-rpmh.c:412 |

互联与时钟用的是同一个 BCM 编码（bcm-voter.c:136 与 clk-rpmh.c:271 都调 BCM_TCS_CMD）。
两个 BCM 都是 keepalive（sm8550.c:1080、:1132），而 bcm-voter.c:107-113 在无带宽请求时投 1，所以：

- BCM_TCS_CMD(1,1,1,1) = 0x60004001  互联（CN0、MC0）
- BCM_TCS_CMD(1,1,0,1) = 0x60000001  时钟（IP0）

顺序照 ipa_power.c:100-104：先互联，后时钟，三条命令放进同一个 TCS。

### 实机（run-20261123-014554）

- 199: the ipa power: clock IP0 at 0x00050040, paths CN0 at 0x00050038 and MC0 at 0x00050000
- 244: the ipa power vote completed (interconnects CN0 and MC0, clock IP0)
- link up, 50 servers
- **没有任何 kernel abort**

### 但 modem 行为仍然没有变化（观察 90 秒）

- tftp: 159 requests / 155 answered / 12 reads / missed 4 / 0 send failures
- ipa : asked 1（仍是那条 0x0020）
- radio: offline not registered

原因和之前一样：modem 等的是 AP 发出的 0x21 INIT_DRIVER，而 0x21 需要先读到 IPA 本地内存的基址。
互联投票是必要条件，不是触发条件。

### 下一个也是唯一剩下的验证点

现在时钟与两条路径都投了，读 SHARED_MEM_SIZE 是否还会中止，就是"投票够不够"的判据：

- 读成功：投票足够，拿到 mem_offset/mem_size，可以往 0x21 走；
- 仍然同步外部中止：块还有一个电源门（GDSC），需要另找。

这一步有中止风险，所以放在单独的 mode 18 里，只有显式选择该模式才会跑，正常启动不受影响。

## 2026-11-23 02:00 读寄存器的第三次尝试：仍然中止

在**时钟（IP0）+ 两条互联（CN0、MC0）全部投票且 vote completed** 之后，用 `sheng-ipa-read=1` 跑了一次读：

- 246: the ipa window mapped (0xfffffff9140b4000 of 1048576 bytes)
- 247: reading SHARED_MEM_SIZE at +0x54
- 然后：Kernel data abort. at pc 0xfffffff00c0287b8 ... x20: 0xfffffff9140b4000

**x20 就是刚映射的窗口 VA**，所以映射没问题，是 IPA 的物理地址仍然不应答。

### 结论

| 条件 | 状态 |
|---|---|
| 时钟 RPMH_IPA_CLK（IP0） | 已投、已 ack |
| 互联 CN0（config）与 MC0（memory） | 已投、已 ack |
| **电源域 / GDSC** | **仍是关的 —— 这就是那道门** |

**时钟与互联是必要条件，不是充分条件。** 设备树 IPA 节点没有 `power-domains` 属性，所以这个门不通过 DT 声明，
要在 GCC 里找 IPA 的 GDSC（或它所属的 RPMh 电源域）。

### 现场

- `sheng-ipa-read` 已从 board.toml 移除，读只在该启动参数存在时执行；
- 安全镜像 `/tmp/fuxi-safe2.img`（sha256 `7310affdfce8630e470dd8bd9ec05e7eec1e568c448fc64d4957c41baf27a571`）已在跑：
  **无中止**、link up、50 servers；
- 时钟与互联的投票保留（Power 已投，且对 modem 无害）。

## 2026-11-23 02:27-02:50  IPA 固件通过 PAS 装载成功，寄存器仍然不应答

### 做了什么

按 vendor 的 qcom,ipa_fws（fuxi-live.dts:29394-29400，qcom,pil-tz-generic、qcom,pas-id = <0x0f>、
qcom,pil-force-shutdown）与主线的默认装载者（ipa_main.c:737-741：两个属性都没有时是 IPA_LOADER_SELF，
即 AP 自己装；x13 的 目标-M5.md:404 记录缺 ipa_fws.mdt 时 probe 返回 -2）复用现成的 PAS：

- FuxiModemPas.h：新增 FuxiModemPasImageIpa、FUXI_IPA_PAS 0x0f；
- FuxiModemLowMeta.h：窗口 0x3000 改为 0x5000，IPA 元数据占 +0x3000 的 2 页
  （第一次给 1 页被拒：a metadata of 4204 bytes does not fit the window slot）；
- FuxiModemProbe.cpp：stepIpaFirmware()，顺序 PAS_SHUTDOWN、INIT_IMAGE、MEM_SETUP、装段、AUTH_AND_RESET，
  在 stepRails() 里、时钟与互联投票之后调用。

### 结果：PAS 链完全成功

249: the metadata is 4204 bytes at 81483000, hash 4, relocatable, from the mdt file
250: pas 15 step 0 answers 0/0     INIT_IMAGE
251: pas 15 step 1 answers 0/0     MEM_SETUP
253: step: the ipa reset
254: pas 15 step 2 answers 0/0     AUTH_AND_RESET
255: the ipa firmware is loaded and the block released

安全世界三次都是 0/0（成功），固件进了 ipa_fw_region，块由 TZ 释放。

### 但寄存器仍然外部中止

打开 sheng-ipa-read=1 后（镜像 21bc9f9f）：

pc 0xfffffff00c02c7b8   x20 0xfffffffa78c47000   far 0xfffffffa78c47054   esr 0x96000010

far 仍是窗口 VA 加 0x54，esr 仍是同步外部中止。

### 现在能排除的

- 时钟 RPMH_IPA_CLK（IP0）：已投、已 ack，排除
- 互联 CN0 / MC0：已投、已 ack，排除
- 电源域 / GDSC：三份参考（x13 设备树、vendor DTS、UMI）都没有，不存在
- IPA 固件未装载：已由 PAS 装载并释放，排除
- 页表 / 映射：VA 建立、窗口映射成功，排除

剩下的候选只有两个：

1. 时钟票 ack 了但没生效（RSC 确认的是命令写入，不是资源应用）；
2. IPA 的寄存器块被 XPU 或保护域挡住，TZ 只为 modem 侧的 IPA 开放。

### 现场

- 开放读的镜像（21bc9f9f）**确实中止**（far = 窗口 VA + 0x54，esr 0x96000010）；
- 之前记的"带固件步骤的镜像卡在 GPU"是**错的**：那次日志的最后三行是 `SXNU1R ... REBOOT-FASTBOOT OK`，
  是本会话自己发的 `ctl reboot-fastboot` 把它踢进 fastboot，不是卡死；
- **但**带固件步骤的镜像（cdf3ecab）跑完 `step: the link` 之后**没有走到 adapter 交接**：
  没有任何 `FUXI modem:` 报告，日志停在 68 KB；同一时刻 ICC 镜像（无固件步骤）能到 52 个服务、TFTP 159/155。
  所以固件那一步在成功之外**改变了时序**，需要挪出正常路径（例如放在 adapter 起来之后，或单独的启动参数后面）；
- 当前运行的是验证过的 /tmp/fuxi-icc.img：无中止、link up、TFTP 159/155、49 个 QMI 服务、时钟与互联投票保留。

## 2026-11-23 03:00 结论：读 IPA 寄存器的探针"失效"，已从代码里移除

用户指出：**探针失效了，才导致 kernel abort**。核对证据后确认属实——不是"有风险"，是**无效**：

| 前提 | 结果 |
|---|---|
| 只映射 | 中止（VA …fa33e90000 + 54） |
| 时钟 IP0 已投 | 中止（…f2215bf000 + 54） |
| 时钟 + 互联 CN0/MC0 已投 | 中止（…f9140b4000 + 54） |
| 再加 IPA 固件 PAS 0x0f 装载并释放 | 中止（…fa78c47000 + 54） |

四次都是 `esr 0x96000010`（同步外部中止）、`far` 都是窗口 VA 加 0x54。

**所以这个处理器根本读不到 IPA 的寄存器块**，无论前面把时钟、互联、固件做得多完整。

### 代码处理（默认路径回到验证过的行为）

- `readIpaRegisters()` **整段删除**，`sheng-ipa-read` 启动参数一并删除；
- 窗口常量（kIpaWindowBase 等）随之删除；
- `stepIpaFirmware()` 保留（它本身成功：pas 15 三步都是 0/0），但改由 `sheng-ipa-fw` 启动参数控制，
  默认不跑——因为跑它的镜像没有走到 adapter 交接；
- **默认路径 = 时钟 + 互联投票**，即验证过的 /tmp/fuxi-icc.img 的行为。

### 后果：0x21 INIT_DRIVER 无法正确构造

`0x21` 的每个 TLV 都是 `mem_offset + 区偏移`，而 `mem_offset` 来自 `CIPA_SHARED_MEM_SIZE`——
**正是那个读不到的寄存器**。所以 QMI 握手在这条路上到此为止。

### 剩下两个未验证的假设

1. 时钟票被 RSC ack 但**没有真正生效**；
2. IPA 寄存器块被 **XPU/保护域**挡住，只有 modem 侧能用。

两者都**不能**靠再读一次 IPA 来区分（每次都是中止）。要验证只能从 RSC/GCC 侧的状态寄存器入手。
