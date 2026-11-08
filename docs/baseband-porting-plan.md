# Xiaomi 13（fuxi / SM8550）基带移植方案

状态：设计方案，尚未实施或进行设备测试  
目标系统：UniXNU + iPadOS 17.7.11（21H461）  
目标硬件：Xiaomi 13（fuxi，Qualcomm SM8550）

## 1. 目标、边界与完成定义

基带移植不是一个单独驱动，而是两套原本不兼容的体系之间的系统工程：

```text
Qualcomm 硬件侧
MPSS remoteproc / PAS
  -> SMEM + SMP2P + IPCC
  -> GLINK
  -> QRTR
  -> QMI 控制面（DMS/NAS/UIM/WDS/...）
  -> IPA/GSI + QMAP/rmnet 数据面

iPadOS 用户态
CommCenter + CoreTelephony
  -> AppleBaseband / ARI 风格服务契约
  -> 系统蜂窝设置、状态栏和应用 API
```

SM8550 的 modem 是 SoC 内集成的 Qualcomm MPSS；iPadOS 中可见的 `AppleBasebandPCI` 则面向 Apple 已支持的基带硬件模型。两者不能直接等同。推荐做法是先完成一个原生 Qualcomm 通路，再用一层小而明确的兼容层把所需状态和操作映射给 CommCenter，而不是让 Apple 的 PCIe 驱动直接操作 MPSS。

项目采用五级完成定义，报告时不得混用：

| 等级 | 定义 | 最低证据 |
| --- | --- | --- |
| BB0：固件在线 | MPSS 经认证启动并完成 ready 握手 | PAS 返回、ready 事件、崩溃原因通道、连续启动记录 |
| BB1：控制面可用 | QRTR 可发现服务，QMI 可读取 modem、SIM 和注册状态 | DMS/NAS/UIM 的成功响应及重启恢复记录 |
| BB2：蜂窝数据可用 | WDS 建立会话，IPA/GSI 稳定承载 IPv4/IPv6 | 地址、路由、DNS、收发计数、持续流量及无 SMMU fault |
| BB3：iPadOS 集成 | CommCenter/CoreTelephony 正确显示并控制 BB1/BB2 | 系统 API、设置 UI、飞行模式、连接恢复和崩溃日志 |
| BB4：电话能力 | SMS、语音、IMS/VoLTE 及音频链路可用 | 呼入/呼出、双向音频、SMS、异常恢复和合规测试 |

首期建议只承诺 BB2，即物理 SIM 的 LTE/5G 数据。BB3 单独立项，BB4 最后进行。eSIM、紧急呼叫、运营商认证和可日用语音不属于首期目标。

## 2. 当前基线和已知缺口

截至本方案形成时，本项目的实际状态是：

- `porting.toml` 的 `network` 和 `audio` 均为 planned，且没有选中网络或音频组件。
- `dts/fuxi.dts` 尚无 modem、SMEM、SMP2P、GLINK、IPA/GSI 等节点。
- `board.toml` 主动禁用 `com.apple.CommCenter`，这是当前无基带驱动时正确的保护措施。
- 当前 PMGR bridge 只满足 AMFI 的服务契约，不是真实电源管理；基带不能通过它操作电源、复位或时钟。
- iPadOS 镜像带有 CommCenter/CoreTelephony 和 AppleBaseband 相关代码，但当前 kernel candidate 并未因此自动具备 Qualcomm 基带支持。
- cellular CommCenter 的 launchd 配置受设备树中的 `baseband` 节点控制；过早发布该节点会触发 KeepAlive 服务反复启动，而不是让蜂窝功能自动可用。
- 当前设备身份为 iPad7,1/J120 风格配置，是否还需要 cellular SKU capability、MobileGestalt 或产品策略调整，必须在 BB2 后审计，不能仅靠添加 `baseband` 节点判断。

## 3. 不可违反的安全规则

1. 设备事实只以该机 stock live DT、Android 实测、分区元数据或明确固定版本的 fuxi 源码为依据；其他 SM8550 设备只作线索。
2. 只读使用 `modem` 固件输入，并为每个文件记录来源、分区、版本和 SHA-256。不得拿 Apple baseband firmware 替代 Qualcomm MPSS 固件。
3. 不写 `modemst1`、`modemst2`、`fsg`、`fsc`、`persist`，不复制其他设备的 EFS/NV，不修改 IMEI、射频校准和运营商安全材料。
4. 未完成控制面校验前保持 RF 离线；需要发射的测试使用合法 SIM、合适的屏蔽/实验环境和本地法规允许的频段。
5. 基带、IPA 或兼容层失败必须可关闭，不能拖死 AP、UFS、USB、显示和 GPU。每阶段都保留“不发布 baseband 节点、继续禁用 CommCenter”的回退路径。
6. 设备启动测试遵循仓库规则：只有设备所有者明确要求时才进行，且优先使用可恢复的临时启动，不把实验镜像写入启动分区。
7. 日志默认脱敏：IMEI、IMSI、ICCID、电话号码、基站标识和鉴权材料不得完整落盘。

## 4. 总体分层和代码归属建议

共享能力应进入 UniXNU 的 SM8550/Qualcomm 公共层：

- PAS/SCM 与 MPSS remoteproc
- SMEM、SMP2P、IPCC、GLINK
- QRTR、QMI 编解码和服务发现
- IPA/GSI、QMAP/rmnet 与网络接口适配
- 通用 SSR、crash reason、超时与资源生命周期管理

`unixnu-fuxi` 只保存板级事实和策略：

- carveout、MMIO、IRQ、SMMU SID、GPIO、时钟、regulator、interconnect 的 fuxi 绑定
- 固件 manifest、哈希和提取来源，不提交不可分发固件本体
- SIM 卡槽及 RF/天线开关的板级 wiring
- fuxi 的 bring-up 配置、脱敏日志和证据
- 是否启用 CommCenter 以及系统集成的产品策略

兼容 CommCenter 的 shim 应独立于底层 Qualcomm 传输实现。这样上游更新 QMI、IPA 或 Apple 用户态契约时，不会把板级配置和硬件驱动耦合在一起。

## 5. 分阶段实施计划

### 阶段 0：冻结范围、证据格式和恢复方案

目标：在接触基带前确定首期只做什么、如何证明以及失败后如何恢复。

工作项：

- 首期冻结为：单物理 SIM、数据业务、手动启停；暂不承诺 eSIM、SMS、语音、IMS、紧急呼叫和双卡。
- 建立统一事实表，至少包含：值、单位、来源、采集日期、stock build、可信度、是否在真机验证。
- 建立固件 manifest，记录 MPSS/MBA/IPA 相关文件的原分区、偏移/路径、大小、版本和哈希。
- 定义脱敏规则、崩溃日志格式、串口关键字和每阶段结果模板。
- 定义故障回退：CommCenter 保持禁用；不发布 `baseband`；modem 可被单独 reset/stop；网络接口可强制 down。

交付物：范围说明、事实表模板、固件 manifest、风险登记表和恢复检查单。

验收门槛：所有 proprietary 输入可追溯；所有写分区动作均被排除；测试人员能在不依赖基带的情况下恢复到当前启动路径。

停止条件：固件来源不明、设备 NV 已被非原机数据覆盖，或无法保证基带失败与 AP 隔离。

### 阶段 1：建立 Android/stock 黄金基线

目标：从 fuxi 本机得到完整的基带拓扑和正常行为，杜绝凭其他机型猜地址。

需要采集的事实：

- stock flattened DT、所有 fuxi overlay 以及最终生效节点。
- reserved memory：MBA/MPSS、SMEM、IPA、ramdump 等 carveout 的地址、大小和访问属性。
- remoteproc/PAS 名称、SCM 调用、reset、proxy/unproxy 资源和启动顺序。
- SMEM item、SMP2P edge、IPCC mailbox、GLINK edge/channel 名称，尤其是 `IPCRTR`。
- 所有 MMIO、IRQ、clock、regulator、power-domain、RPMh 和 interconnect vote。
- modem/IPA/GSI 对应的 SMMU domain、Stream ID、DMA 宽度和 coherency 属性。
- stock remoteproc、QRTR、QMI、rmnet/IPA 的驱动与模块版本、内核配置、服务/进程依赖。
- SIM 插入、飞行模式、注册、建立数据连接和断开连接时的事件序列及时间线。
- modem、modemst、fsg、fsc 等分区表；NV 分区只记录元数据和备份策略，不解析或修改内容。

建议将每个事实分为三类：`confirmed-device`（真机直接证据）、`confirmed-source`（固定 fuxi 源码）和 `hypothesis`（待验证）。只有前两类可以进入板级配置。

交付物：`facts/baseband/` 证据集、地址/IRQ/SID 表、资源依赖图、正常启动时间线、固件 manifest v1。

验收门槛：每个将来要映射的地址、IRQ、SID 和 carveout 均有 fuxi 来源；能画出从 AP 启动 MPSS 到 rmnet 获得地址的完整 stock 时序。

停止条件：关键 carveout 与 AP 内存布局冲突，或 stock 证据对 SID、IRQ、固件版本存在未解释的矛盾。

### 阶段 2：只启动 MPSS，不开放 RF 和数据

目标：把 modem 作为可控 remote processor 安全启动，达到 BB0。

底层职责：

- PAS/SCM 封装：认证、装载、reset、shutdown 和错误码保留。
- MDT/ELF 分段装载：严格检查边界、对齐、重叠、权限和目标 carveout。
- reserved-memory 生命周期：AP 不分配、不覆盖，cache 属性与 stock 一致。
- 资源序列：按 stock 顺序完成 regulator、clock、power-domain 和 interconnect vote；不能借用当前假 PMGR bridge。
- 状态通道：ready、stop-ack、fatal、watchdog、ramdump/crash reason。
- 失败清理：任意一步失败均逆序释放 proxy 资源，禁止半启动状态继续进入下一阶段。

测试矩阵：冷启动、Android 后热启动、重复 stop/start、固件校验失败、ready 超时、modem watchdog、AP 正常关机。

交付物：MPSS 状态机说明、资源序列表、错误映射表、脱敏启动日志。

验收门槛：

- 固件认证与 ready 握手可重复成功。
- 连续 20 次启动/停止无 AP panic、无 carveout 污染、无 UFS/USB/GPU 回退。
- 固件错误、超时和 modem fatal 均能留下明确原因并安全停止。
- 全过程未写 modem NV 分区，RF 仍保持受控关闭。

### 阶段 3：打通 SMEM、SMP2P、IPCC 和 GLINK

目标：建立 AP 与 MPSS 的可靠双向 IPC，但暂不执行会改变网络状态的 QMI 请求。

工作包：

- SMEM：TOC/item 发现、边界检查、host ID、cache/memory barrier 语义。
- SMP2P：双方状态位、inbound/outbound edge、interrupt acknowledge、重启代际。
- IPCC：doorbell 路由、IRQ affinity、清中断顺序和丢中断诊断。
- GLINK：transport negotiation、channel open/close、intent/ring、credit/backpressure、wraparound。
- SSR：modem 重启时销毁旧 channel、唤醒等待者、禁止使用旧地址，ready 后重建。

交付物：IPC 状态机、通道清单、并发/内存序说明、错误注入结果。

验收门槛：

- MPSS edge 稳定进入 up，`IPCRTR` channel 成功打开。
- 双向小包、大包、ring wraparound 和 backpressure 均正确。
- 不用 busy-loop 代替中断；空闲时无持续 CPU 占用。
- 主动触发 modem restart 后，旧请求正确失败，新通道自动恢复，连续 50 次无泄漏或 use-after-free 迹象。

### 阶段 4：实现 QRTR 和只读 QMI 控制面

目标：完成服务发现和健壮的事务层，达到 BB1 的前半部分。

职责拆分：

- QRTR：node/port、HELLO、NEW_SERVER/DEL_SERVER、lookup、广播和 modem 代际管理。
- QMI/QMUX：header/TLV 编解码、transaction ID、request/response/indication、超时和取消。
- 服务优先级：CTL（如该固件需要）、DMS、NAS、UIM、WDS；Voice/WMS/IMS 后置。
- 所有解析器都必须拒绝越界长度、重复关键 TLV、未知版本和过大消息；未知非关键 TLV 可跳过。

第一轮只允许无副作用读取：

- DMS：版本、能力、运行状态；设备标识只在内存使用，日志脱敏。
- NAS：radio capability、信号指标、注册状态。
- UIM：卡存在、应用列表和 PIN 状态，不做 PIN 尝试。
- WDS：数据能力和 profile 列表元数据，不启动连接。

交付物：QMI 服务/版本矩阵、请求权限表、错误和超时语义、协议 fuzz/边界测试记录。

验收门槛：服务枚举在每次 MPSS 启动后稳定；1000 次只读事务无错误累积；未知 TLV 和超时不会损坏内存；SSR 后所有服务重新发现且旧 transaction 不会误配给新响应。

### 阶段 5：SIM、射频开关与网络注册

目标：以最小副作用验证物理 SIM、RF 和运营商注册，不建立数据会话。

严格按以下顺序推进：

1. UIM 报告物理卡存在及应用类型。
2. 读取 ICCID/IMSI 供内部匹配，日志仅保留不可逆摘要或末四位。
3. 验证 PIN/PUK 状态；测试期间禁止自动重复 PIN，避免锁卡。
4. 建立单一 radio state owner，避免多个进程同时切 RF。
5. 查询支持制式和频段，和 fuxi stock 基线对照。
6. 执行 PLMN 扫描、自动选择、注册、注销和飞行模式切换。
7. 记录 LTE/NR 模式、驻留小区和信号质量；敏感基站信息脱敏。

交付物：SIM 状态机、radio owner 规则、注册时间线、运营商/制式兼容矩阵。

验收门槛：物理 SIM 可重复识别；RF off/on 与注册/注销各 50 次无卡锁死、无 MPSS 崩溃；弱信号、无 SIM、PIN locked、无服务和漫游状态均能区分；飞行模式最终一定使 RF 离线。

停止条件：发现 NV/校准不一致、异常高频重启、不可控发射或 SIM PIN 尝试计数异常。

### 阶段 6：IPA/GSI 和蜂窝数据面

目标：建立与 QMI 控制面分离、可观察、可恢复的数据通路，达到 BB2。

工作包：

- IPA firmware/PAS（若该 stock 版本需要独立装载）及版本握手。
- IPA/GSI MMIO、IRQ、SMMU mapping、channel/event ring 和 doorbell。
- endpoint、filter/routing、header insertion、checksum offload 和 aggregation。
- QMAP/rmnet mux、WDS client endpoint、IPv4/IPv6 family 和 MTU。
- WDS start/stop network 与 IPA channel 的事务化编排；任一失败均清理整个半连接状态。
- XNU 网络适配：首期发布自有 `rmnet0`/`pdp_ip0` 风格接口，不依赖 AppleBasebandPCI。
- 地址、gateway、DNS、MTU 和路由从 modem 的实际响应建立，不写死 Android 参数。

数据路径验收分层：

1. GSI ring 自检和 DMA 映射正确，无 SMMU fault。
2. modem 与 AP 间收发计数增长，QMAP mux 正确。
3. 获得 IPv4 或 IPv6 地址并能访问 gateway/DNS。
4. ICMP、DNS、TCP、UDP 和双栈分别通过。
5. 大包、碎片/PMTU、上下行并发和长流量通过。
6. 断网、基站切换、RF off/on、PDP stop/start 和 SSR 后恢复。

交付物：IPA/GSI 资源表、数据包路径图、接口生命周期、性能和故障测试报告。

验收门槛：

- IPv4/IPv6、DNS 和 MTU 与 WDS 响应一致，接口 down 后无残留路由。
- 10 分钟冒烟和 1 小时持续流量无 DMA/SMMU fault、ring stall 或不可解释丢包。
- 100 次 PDP 建立/断开无资源增长；modem crash 时接口立即 down，恢复后需重新授权连接，不能静默复用旧会话。
- USB、UFS、GPU、显示和触控基线不回退。

### 阶段 7：独立用户态 broker 和诊断工具

目标：在不依赖 CommCenter 的情况下形成稳定的蜂窝产品边界，完整证明 BB1/BB2。

建议建立 owner-signed 的 `fuxi-basebandd`（名称可调整），通过最小化 IOKit UserClient 访问内核通路。它只暴露版本化的高层能力：

- modem 状态与固件版本
- SIM/PIN 的只读状态
- RF 与注册状态
- 信号指标
- 启动/停止一个数据会话
- 数据接口参数、统计和最近一次错误
- 受控 SSR/诊断，仅限开发 entitlement

不应把任意 QMI、任意物理地址映射或 DMA buffer 直接开放给普通用户态。每个 selector 都要有 entitlement、长度、状态和调用者检查；API 必须携带版本和 modem generation。

交付物：broker 契约文档、权限模型、CLI 验证报告、崩溃和更新兼容策略。

验收门槛：独立 CLI 能完成 BB1/BB2 的全部操作；杀死或重启 broker 不会让 RF/数据面失控；无权限进程不能打开控制接口；旧 generation 的句柄全部拒绝。

### 阶段 8：审计并接入 CommCenter/CoreTelephony

目标：通过兼容层把已验证的 Qualcomm 能力交给 iPadOS，达到 BB3。此阶段不能反过来成为底层 bring-up 工具。

先做契约审计：

- 当前 21H461 的 CommCenter 实际匹配哪个 IOService/class，读取哪些 DT/property/personality。
- 它打开哪些 UserClient type/selector、共享内存、notification port 和异步回调。
- `AppleBaseband`、`AppleBasebandPCIControl`、ARI 等组件中哪些是用户态必需契约，哪些只适用于 Apple PCIe modem。
- 当前 kernel candidate 需要预链接哪些组件；每个组件的 provider 和硬件假设是什么。
- CommCenter、basebandd 和相关 helper 的 sandbox、entitlement、用户 `_wireless` 和文件权限要求。
- iPad7,1/J120 配置对 cellular capability、设置面板和 MobileGestalt 的约束。

推荐架构：最小 `AppleBaseband` contract shim 对外满足必要服务，对内调用阶段 7 的 broker；不要让 AppleBasebandPCI 直接映射 Qualcomm MMIO、加载 Qualcomm 固件或拥有 IPA。

启用必须采用三道闸门：

1. 保持设备树没有 `baseband`，以开发方式手动启动 CommCenter，观察缺失契约和退出原因。
2. shim 契约完整、CommCenter 可稳定启动/停止后，才在测试配置发布 `baseband` 节点。
3. 连续稳定并能退回 noncellular 模式后，才从 `sheng-disable-jobs` 中移除 `com.apple.CommCenter`。

交付物：21H461 契约矩阵、shim 映射表、ADT/personality 设计、entitlement 清单和回退开关。

验收门槛：

- CommCenter 单实例稳定，无 KeepAlive crash loop。
- CoreTelephony 能区分无 SIM、PIN locked、无服务、注册、漫游、数据连接等状态。
- 设置 UI 和状态栏只呈现真实底层状态；UI 操作能正确控制 RF/PDP。
- 飞行模式、重启、CommCenter 重启和 MPSS SSR 后状态最终一致。
- 禁用测试开关后能恢复 noncellular plist，不影响系统启动。

### 阶段 9：SMS、语音、音频和 IMS（独立项目）

目标：在 BB3 稳定后逐项增加电话能力，达到 BB4。

推荐顺序：

1. WMS 短信收发和状态报告；先处理纯文本，再处理多段、编码和异常消息。
2. Voice QMI 的呼叫状态机，但先不宣称音频可用。
3. AP/MPSS/ADSP 的 voice audio path、GPR、codec、mic/speaker/earpiece 路由。
4. IMS 注册、VoLTE 呼叫、网络切换和运营商 profile。
5. 来电 UI、静音、音量、蓝牙/耳机路由等系统集成。
6. 紧急呼叫最后处理，并需要专门的法规、运营商和失效安全验证。

这一阶段依赖目前仍未实现的音频栈。仅出现呼叫状态或对端振铃，不代表“通话可用”；必须有稳定的双向音频和正确的音频路由。

验收门槛：SMS 与语音分别出具测试报告；语音需覆盖呼入、呼出、拒接、挂断、保持、弱网、掉网、SSR、双向音频和路由恢复。IMS/VoLTE 未经运营商和法规验证时不得标记为日用完成。

### 阶段 10：电源管理、SSR 和长期稳定性

目标：把实验室可用提升到可重复、可恢复的工程状态。

工作项：

- suspend/resume 时 MPSS、GLINK、IPA/GSI、网络接口的次序和 wake source。
- RF off/on、飞行模式、低电量、温控限制、关机和重启。
- MPSS SSR、IPA fatal、GLINK transport loss、QMI timeout 和 SIM hot-state 变化。
- QMI thermal 与系统 thermal policy 的最小映射，不伪造温度或功耗。
- crash dump 的访问控制、大小限制、脱敏和轮转。
- watchdog 只能重启失效子系统；设置退避和熔断，禁止无限 crash loop。

最终耐久门槛：

- 100 次数据连接/断开，100 次 RF off/on，50 次 modem SSR。
- 8 小时空闲注册、1 小时上下行并发、多个 suspend/resume 周期。
- 无 AP panic、DMA/SMMU fault、资源持续增长、CommCenter crash loop 或不可恢复的失联。
- 所有失败均有可归类原因，且关闭 cellular 后系统其他功能保持当前基线。

## 6. 阶段依赖与禁止跨越的闸门

```text
事实基线
  -> MPSS/BB0
    -> IPC
      -> QRTR/QMI
        -> SIM/注册/BB1
          -> IPA/GSI/BB2
            -> 独立 broker
              -> CommCenter/BB3
                -> SMS
                  -> Voice + Audio + IMS/BB4

所有阶段 -> SSR/故障恢复 -> 长期稳定性
```

以下捷径应明确禁止：

- MPSS ready 之前启用 CommCenter。
- QRTR/QMI 未稳定就开始 IPA 长流量。
- 为让 UI 出现蜂窝菜单而提前添加 `baseband` 节点。
- 用 AppleBasebandPCI 的类名掩盖 Qualcomm integrated modem 的硬件差异。
- 把 Android 的固定地址、SID 或时钟序列直接复制到板级配置而没有 fuxi 真机证据。
- 在独立数据路径尚未通过前，通过修改系统 UI 判断驱动成功。
- 在音频/IMS 未完成时把“能拨号”报告为“语音可用”。

## 7. 测试证据和阶段报告格式

每次设备测试都应同时记录：

- UniXNU、设备仓库、UEFI、固件 manifest 的精确 revision/hash。
- 启动方式和 boot args；host 构建通过与设备观察必须分开写。
- stock/UniXNU 对照条件、SIM/运营商/制式（敏感值脱敏）。
- 状态时间线：AP boot、MPSS ready、GLINK up、QRTR discover、register、PDP up。
- 关键计数器前后值、错误码、SMMU fault、SSR generation 和资源清理结果。
- 明确结论：达到 BB0/BB1/BB2/BB3/BB4 中哪一级，以及没有达到什么。

单项通过不能代替整级验收。例如：`NAS registered` 只能证明注册控制面；`ifnet up` 不能证明数据可达；状态栏有信号格不能证明基带真实在线。

## 8. 推荐的首轮工作包

首轮不写驱动，先完成以下设计输入：

1. 从 fuxi stock 系统生成最终 live DT 和 modem/IPA 资源表。
2. 固化当前 stock build 的 remoteproc、QRTR、QMI、IPA/GSI/rmnet 启动时间线。
3. 建立 MPSS/IPA 固件 manifest 和不可写 NV 分区清单。
4. 对照 UniXNU 当前 AP 内存图，检查所有 modem/IPA carveout 是否冲突。
5. 输出公共组件边界和 fuxi 板级 binding 草案。
6. 为阶段 2 设计一个只启动、只观察、RF 受控关闭的 MPSS 测试配置。

完成这六项后，才能可靠估算阶段 2—6 的工程量。目前最大的未知量不是 QMI 消息本身，而是 fuxi 固件版本对应的 PAS/资源序列、IPA/GSI/SMMU 绑定，以及 21H461 的 CommCenter 最小契约。

## 9. 推荐里程碑

| 里程碑 | 包含阶段 | 对外可声称的结果 |
| --- | --- | --- |
| M0：事实冻结 | 0–1 | 有完整、可追溯的 fuxi 基带硬件和固件规格 |
| M1：基带在线 | 2–3 | MPSS 与 AP IPC 可重复工作，即 BB0 |
| M2：控制面 | 4–5 | SIM、信号和网络注册可读可控，即 BB1 |
| M3：数据可用 | 6–7 | 独立工具可稳定使用蜂窝数据，即 BB2 |
| M4：系统集成 | 8 | iPadOS UI/API 与真实基带状态一致，即 BB3 |
| M5：电话能力 | 9–10 | SMS/语音/IMS 经稳定性与合规测试，即 BB4 |

从风险和维护成本看，M3 是最合理的第一版发布目标。它能证明 Qualcomm 全链路真实可用，同时把容易随 iPadOS 版本变化的 CommCenter 兼容层隔离在后续里程碑中。
