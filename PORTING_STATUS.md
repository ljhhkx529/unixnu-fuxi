# 基带移植状态

按模块记录状态、已跑的检查和剩余工作。主机结果与真机结果分开写。

## 阶段总览

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| S0 | 合并 UniXNU 上游，不带基带 | 已完成 |
| S1 | 冻结 fuxi 基带事实到 `facts/baseband/` | 已完成 |
| S2 | 引入上游 QcomBaseband 组件 | 已完成 |
| S3 | fuxi 板级适配与 IPA v5.5 表重生成 | QRTR 已完成，IPA 未做 |
| S4 | 通过 PAS 启动 MPSS，RF 保持关闭 | 主机完成，未上真机 |
| S5 | 打开 GLINK 的 IPCRTR/DS 通道，QRTR 起来 | 进行中 |
| S6 | 只读 QMI 控制面 | 未开始 |
| S7 | 读卡1信号指标 | 未开始 |

## 模块状态

### QRTR 协议层 — `UniXNU/drivers/QcomBaseband/platform/fuxi/qcom_qrtr.c`

状态：**主机测试通过，链接进内核，未上真机**。

已完成：

- 32 字节 `qrtr_hdr_v1` 组帧，载荷补齐到 4 字节，头与载荷一个连续缓冲区。
- 节点/端口语义按 Linux `net/qrtr/af_qrtr.c` 与 Trogdor 参考实现校正：
  - 本地节点 id = 1（原为 0）。
  - 控制包的源端口 = `QRTR_PORT_CTRL`（原硬编码 0）。
  - DATA 包的源端口 = 客户端自己的本地端口（原硬编码 0），modem 按它回包。
  - 回包的路由端口取帧头的 `dst_port`（原误用 `src_port`）。
  - 首个 HELLO 发往广播节点 `0xffffffff`；回 HELLO 按对端 HELLO 携带的节点 id 寻址。
  - DEL_CLIENT 发往广播节点。
- 新服务/删服务名称服务，`qrtr_lookup` 取最低实例。

检查：`UniXNU/drivers/QcomBaseband/tests/fuxi_qrtr_model_test.c`，由组件的 `scripts/check.py` 以
`-fsanitize=address,undefined` 编译运行。覆盖 header、hello、framing、control_ports、new_server、data、peer_hello。

### fuxi 适配层 — `UniXNU/drivers/QcomBaseband/platform/fuxi/fuxi_modem.c`

状态：**已链接，未上真机**。

- GLINK 描述符四字顺序按 Linux `qcom_glink_smem.c` 核对：AP 复位 tx head 与 rx tail。
- 通道 `IPCRTR` 由 modem 开启，适配层应答（`open_now = 0`）。
- 桥接钩子：`coachz_baseband_configure`（发送与释放）、`coachz_baseband_qrtr`（数据）、`coachz_baseband_new_server`（服务）。
- 数据面 `coachz_modem_data_send` 返回 `ENETDOWN`（IPA 未实现）；控制面不走这条路径。

集成测试发现并修掉两个真缺陷（读代码看不出来，是把真实适配层拉起来跑出来的）：

1. **HELLO 早于通道上线。** 适配层原来一启动就把 HELLO 发了出去，那时 GLINK 通道还没开，packet 无处可去，
   直接丢了。现在 HELLO 记为"欠着"，由 owner 的 poll 在通道上线后再发。
2. **回给 modem 的 HELLO 被永久丢掉。** QRTR 层原来用一次性 `qrtr_send` 回 HELLO，而此时 modem 的接收
   intent 还被"带着它自己 HELLO 的那个 packet"占着，回包必被拒；一拒就再没人重试，modem 永远学不到我们的
   节点 id。现在回包也记为"欠着"，在下一个事件里 flush。

测试：`UniXNU/drivers/QcomBaseband/tests/fuxi_modem_integration_test.c`。它把真实的 `fuxi_modem.c` 拉起来，
配一份假的 SMEM 与一个假的 modem 对端（对端开 IPCRTR 通道、给 intent、发自己的 QRTR HELLO），检查 AP 的
HELLO 以 `QRTR_TYPE_HELLO` 发往广播节点与控制端口、链接转 up、回 HELLO 发给 modem 的节点 id、远端节点被学到。
对上面两种旧行为它都会失败。

### Modem PAS 启动 — `unixnu-fuxi/package/FuxiModemProbe.cpp`

状态：**进行中**。

实参已从一手出处确认（见 `PORTING_MAP.md` 的实参来源表）：PAS id 4 / DTB 0x26、两个内存洞、`cx.lvl` 与 `mss.lvl` 两条轨、
IPCC client 2（MPSS）、通道名 `IPCRTR`、SMP2P 项 0x1b3/0x1ac、remote pid 1。

顺序按 Linux `qcom_pas_start`：先 DTB 后镜像；每段为 `INIT_IMAGE` → `MEM_SETUP`（可重定位时）→ 载入分段 →
`AUTH_AND_RESET`；随后 QMP `load_state` 放行；再等 modem 的 ready 信号。

成功判据用严格 `x0 == 0 && x1 == 0`。ADSP 早期出现的正数 cookie（`0xcfffe5`）是 SMC 寄存器参数错位的产物，
不是成功值，ADSP 已经在真机上证明（见 `docs/fuxi-audio-test-results.md` 的 R-SCM-BUG-01 与 R-SCM-STRICT-01）。

PAS 序列现在只有一处定义：`package/FuxiModemPas.h`。probe 直接驱动它，顺序、每步的命令号与参数类型、
两个 PAS id、成功判据都在这个头文件里。主机检查 `scripts/check-fuxi-modem-pas-host.py` 直接检查这个头文件，
把顺序调换或把成功判据放宽都会失败。

实参记录在 `facts/baseband/`：`binding.toml` 给每个值标注状态（confirmed / inferred / chosen / unverified）
与其出处，`manifest.toml` 说明每个出处能证明什么、不能证明什么。

### IPA v5.5 数据面

状态：**未做**。不在卡1目标的关键路径上：SIM 状态与信号信息走 QRTR 控制通道。

### 内核接线

状态：**已链接，未上真机**。

`patches/xnu/fuxi-modem-probe.patch` 在 `bsd_init` 里（`DEVELOPMENT || DEBUG` 下）调用 `native_fuxi_modem_probe`，
probe 自身只在 `fuxi-modem` 启动参数存在时才动作，正常开机不付代价。内核链接通过，预链接的 `kernel.macho`
带有 `_native_fuxi_modem_probe`、`_fuxi_modem_start`、`_fuxi_modem_poll`、`_fuxi_modem_report` 与
`_qrtr_flush_hello_reply`。

### iPadOS 接入

状态：**未做**。

- `/chosen/coachz-baseband` 尚未加进 fuxi 设备树。
- `kern_exec.c` 的 `disabled.plist` 复通 CommCenter 的改动尚未做（参考 Trogdor 的做法）。

## 已跑的检查

| 检查 | 类型 | 结果 |
| --- | --- | --- |
| `qcom_qrtr_model_test.c`（组件 `scripts/check.py`） | 主机 | PASS |
| `fuxi_modem_integration_test.c`（组件 `scripts/check.py`） | 主机 | PASS；对两种旧行为都会失败 |
| `scripts/check-fuxi-modem-pas-host.py` | 主机 | PASS |
| 内核链接（含 `libQcomBaseband.a` force-load 与 modem probe 接线） | 主机 | PASS，见 `porting.toml` 的 host 记录 |
| 基带镜像真机启动到 launchd | 真机 | PASS（桥本身静默，符合 `/chosen/coachz-baseband` 未设的预期） |
| modem 真机启动 | 真机 | **未跑** |

## 剩余工作

1. ~~在真机上定位 modem 固件~~：**已完成**（2026-11-19）。`modem_a` 分区 `/image` 目录里就是
   `modem.mdt` 与 `modem_dtb.mdt`，分段 `modem.b00`–`modem.b31` 缺 b14/b21/b30。
2. ~~`mss.lvl` 轨是否存在~~：**已确认存在**（2026-11-19 真机读命令数据库）。`cx.lvl` 在 `0x30000`
   10 个档，`mss.lvl` 在 `0x30060` 7 个档。之前"本板没有 mss.lvl"的说法是从 sheng 板推断的，不成立。
3. 内核构建 → `fastboot flash boot` → 真机取 DIAGSTATUS 证据。
4. QRTR 服务发现 → QMI `UIM`/`NAS` 只读调用 → 卡1状态与信号。
5. `/chosen/coachz-baseband` 加进 fuxi 设备树，`disabled.plist` 复通 CommCenter。
