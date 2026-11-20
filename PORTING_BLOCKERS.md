# 基带移植阻塞项

只记真正的阻塞和需要真机验证的项。每条给出：是什么、为什么卡住、需要什么才能解开。

`## 已发现的缺陷` 一节里的条目是代码里的真缺陷，写下它就必须当场修：钩子会挡住对其他文件的写入，
直到那个缺陷所在的文件被改过。上面这些条目不是缺陷，是等真机测量的开放问题，不挡路。

`## 待查的缺陷` 一节里的条目是**原因还没查清**的 bug。钩子会挡住对这些文件的下一次写入，
直到那次写入带上测量（printf）：同一个 bug 猜第二次之前，先量一次。

## 已发现的缺陷

<!-- 每条写下：缺陷是什么、它在哪个文件里、怎么确认修好了。修好后把这条移到下面的「已解除的阻塞」。 -->

## 待查的缺陷

<!-- 暂无条目。原因已经查清的条目应当移入下面的「已解除的阻塞」。 -->

## B1 板级 cmd-db 里 `mss.lvl` 是否存在 — 需运行时确认

**是什么**：modem 的代理电源域需要 `cx.lvl` 和 `mss.lvl` 两条 RPMh 资源。
`cx.lvl` 在本板 cmd-db 中已被证明（`UniXNU-sm8550-common/kits/Sm8550Platform/ShengGmuCommandDb.c` 见到 `gfx.lvl`、`mx.lvl`、`cx.lvl`、`gmxc.lvl`），
`mss.lvl` 尚未证明。

**为什么卡住**：投票找不到资源就发不出 ARC 请求，modem 起不来。

**怎么解**：运行时在 cmd-db 里查 `mss.lvl`。查不到就报出来（缺资源这一条事实本身要出现在日志里），
不要假定它存在。CAF live dts 的 `rpmh-regulator-msslvl` 说明厂商树里它存在，但本板 cmd-db 是另一份拷贝。

## B2 Modem 固件在设备上的文件名与分段布局 — 已确认真机（2026-11-19）

**是什么**：`fuxi_adsp_fw_open("modem:NAME")` 走 FAT 读 `image/NAME.b00`、`NAME.b01`……
`NAME` 是 `modem` 还是 `modem_dtb`，以及 modem 与 modem_dtb 各自的分段编号。

**为什么卡住**：找不到文件就载入不了。

**怎么解**：真机上枚举 `modem_a`/`modem_b` 分区的 `/image` 目录。读接口已经支持 `modem:` 前缀（`package/FuxiAdspFirmware.c`），
缺的是文件名。

**结论（真机，2026-11-19）**：`fuxi-modem=1` 的只读清点在设备上跑通，`modem_a` 分区的 `/image` 目录共 448 项，
其中就有 `modem.mdt`（6364 字节）与 `modem_dtb.mdt`（4044 字节），分段是 `modem.b00` 到 `modem.b31`，
中间缺 `b14`、`b21` 与 `b30`，共 29 段。secure world 要的两个名字，就是板上有的两个名字，
PAS 序列的文件查找没有可猜的了。同一分区还放着 `adsp.mdt`、`cdsp.mdt`、`cpusys_vm.mdt`、`ipa_fws.mdt`
等别的处理器固件，所以查 `modem.mdt` 时不能把这个分区当成 modem 专属。
`/image/modem_pr` 是目录，下面有 `mcfg` 与 `so` 两个子目录，本身不含固件镜像。
证据：`evidence/2026-11-19-modem-firmware-inventory.log`。

**这一条也暴露了清点路径的两个真缺陷**（host 测试看不到，因为 host 测试自己造镜像自己挂）：
一是探针先判断固件分区在不在、再去等它，而 `bsd_init` 里跑的时候 UFS 驱动还没起来，
所以设备上直接打印「the firmware partition is not there yet」就退出了——
等待的代码放在了判断之后，永远到不了；现在等待在前。二是每个目录只列 64 项，
所以第一次只报了 65 项，`/image/modem_pr` 根本没走到；现在上限 1024，并且把 modem 自己的目录也列了。

## B3 Modem 的 ready 信号来源 — 部分未确认

**是什么**：Linux 靠 remoteproc 的 `ready` 中断（`q6v5_ready_interrupt`）判定 modem 起来了。
CAF live dts 里 modem 的 `interrupts-extended` 有一组 `0x77`（modem SMP2P slave-kernel）条目，
名字是 wdog/fatal/handover/ready/stop-ack/shutdown-ack，ready 对应哪一条需要确认。

**为什么卡住**：不知道该等哪个信号，就无法区分「modem 起来了」和「modem 卡住了」。

**怎么解**：先按 SMP2P 的 `slave-kernel` 入向条目判定（与 ADSP 同构），再与 B2 一起在真机确认。

## B4 IPA v5.5 数据面 — 未做（不在卡1关键路径）

**是什么**：组件自带的 IPA 表针对 SC7180 v4.2；fuxi 是 v5.5，端点 id 也变了（AP_MODEM_RX、MODEM_AP_RX 从 23/21 移到 24/22）。

**为什么卡住**：表要按 fuxi 的寄存器重生成，需要 v5.5 的源码。

**怎么解**：卡1目标（读 SIM 状态、收信号）走 QRTR 控制通道，不需要 IPA。可延后。

## B5 NV / 校准文件是否由 AP 提供 — 未知

**是什么**：modem 运行时可能向 AP 索取 NV 或射频校准文件，那就需要 rmtfs 等价物。

**为什么卡住**：若需要，控制面之外还要多一条文件服务路径。

**怎么解**：modem 起来后看它是否发请求。现有 FAT 读 `modem_a`/`modem_b` 的能力已经覆盖常见布局。

## B6 射频必须保持关闭直到控制面验证 — 约束，不是缺陷

modem 起来后 RF 会自动搜网。要求：控制面（SIM 状态、信号）验证之前不做任何射频控制动作，
不写 `modemst1`/`modemst2`/`fsg`/`fsc`/`persist`，不碰 IMEI 与射频校准数据。

## B7 上游 UniXNU 合并 — 与基带并行推进

`UniXNU` fork 落后上游 611 个提交、领先 161 个，`merge-tree` 干跑报 98 处冲突（73 处在 `docs/`）。
基带代码放在 `drivers/QcomBaseband/platform/fuxi/` 与 `unixnu-fuxi/package/` 两处，与上游冲突面小，
合并时可先保留基带改动。

## 已解除的阻塞

- **基带事实的自检脚本**。此前 `facts/baseband/` 的 binding 与 manifest 之间没有任何一致性检查。
  `scripts/check-fuxi-baseband-facts.py` 现在检查每条 evidence 都能在 manifest 里找到、confirmed 的值
  不能靠设计决定支撑、以及本板设备树仍声明 SMP2P 对、IPCRTR 通道、`mss.lvl` 轨、remote pid 与中断顺序。
  写它时发现并修掉三个真缺陷：DTB 内存洞引用了 manifest 里没有的 `linux.sm8550.dtsi.mpss_dtb_region`；
  `rails.mss_in_cmd_db` 的 evidence 是 `none`；manifest 把 modem 的 `0x77 0x02` 说成 fatal，而 CAF 的
  `interrupts-extended` 里 fatal 是 `0x77 0x00`、handover 才是 `0x77 0x02`。代码本来就对
  （`package/FuxiModemProbe.cpp` 用 Linux 的编号，fatal 0x1、ready 0x2），错的是记录。
- **modem 中断位的顺序**。CAF 的设备树比 Linux 多一个 handover（bit 2）。CAF 的
  `interrupts-extended` 是 `0x77 0x00`、`0x77 0x02`、`0x77 0x01`、`0x77 0x03`、`0x77 0x07`，
  `interrupt-names` 依次是 wdog、fatal、handover、ready、stop-ack、shutdown-ack，所以第二个数才是位号：
  `0x77 0x00` 是 fatal，`0x77 0x01` 是 handover。Linux 的 `sm8550.dtsi` 少一个 handover，因此 fatal 是
  bit 0、ready 是 bit 1。
- **modem 的 PAS id**。此前记为「推断 0x26（38），无一手出处」。已从 Linux `drivers/remoteproc/qcom_q6v5_pas.c`
  的 `sm8550_mpss_resource` 确认：镜像 PAS id = 4，DTB PAS id = 0x26，`proxy_pd_names = {"cx","mss"}`。
- **fuxi UEFI 是否已经起了 modem**。`uefi-fuxi` 里搜 `modem.mdt`/`remoteproc-mss`/`mpss` 无结果，
  `Silicon/Qualcomm/QcomPkg/Library/` 无 modem 库。所以 AP 侧必须自己做 PAS 引导，这正是缺失的那一块。
- **ADSP 的 SMC 成功判据**。早期注释把正数 cookie 当成功值，真机证明那是寄存器参数错位造成的假象。
  正确判据是严格 `x0 == 0 && x1 == 0`，ADSP 已在真机上按此跑通。
- **基带起来了（2026-11-20）**。此前记在「待查的缺陷」里的「modem 镜像的 AUTH_AND_RESET
  不返回」已经查清并解决：原因不在调用、不在 metadata 的地址、不在 region assign，而在
  **AOSS 窗口的地址少了一位十六进制**——`package/FuxiModemProbe.cpp` 的 `kAossBase` 写的是
  `0x00c30000`，而板上 always-on processor 的窗口在 `0xc300000`（CAF live dts 的
  `power-controller@c300000` 节点，reg `0xc300000` of `0x400`；`facts/audio/binding.toml` 的
  `aoss_base` 也是这个值）。映射到了空的地址，读第一个描述符字就挂死。修正之后描述符按上游预期
  读出（word 0 是 `QMP_MAGIC` 0x4d41494c，word 1 是 `QMP_VERSION` 1，word 13 是邮箱大小 728，
  word 14 是偏移 296），握手打开，load_state 有回应，blob 与镜像的 AUTH_AND_RESET 都被接受，
  31 个分段全部拷完，探针到阶段 9（up）。证据 `evidence/2026-11-20-modem-up.md`。
  同一轮里修掉两个 bring-up 路径上的缺陷：`waitEvents` 忽略了 `assert_wait_timeout` 的返回值，
  所以每个「有界」等待其实都是无界的（`package/FuxiAdspProbe.cpp` 里同样的缺陷也修了）；
  `qcom_qmp_step` 只从中断处理里被调用，所以握手从来没被发起过。
  **这一轮没有证明的**：`glink_up` 仍然是 0，卡 1 的 SIM 还没有被读；`smp2p_ready` 读到的是 4，
  是该项自己的编码，还没有和上游的 `q6v5_ready_interrupt` 对过；RF 仍然关着。
