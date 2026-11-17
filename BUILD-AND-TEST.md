# fuxi 编译与测试手册

小米 13（fuxi / SM8550）UniXNU 设备仓库的本地编译、打包、上机与验证流程。

本文只覆盖**怎么做**，不覆盖**为什么**。架构与证据规则见
[`README.md`](README.md)、[`AGENTS.md`](AGENTS.md)、[`porting.toml`](porting.toml)，
音频线的分阶段设计见 [`docs/fuxi-audio-completion-roadmap.md`](docs/fuxi-audio-completion-roadmap.md)。

更新日期：2026-11-16

---

## 0. 五个必须先记住的规则

1. **Host PASS 不等于 device PASS。** 主机上的 16 套音频测试只证明状态机和报文形状，
   不证明任何硬件行为。两类结论在 `porting.toml` 的 `evidence` 里用
   `kind = "host"` 和 `kind = "device"` 分开记录。
2. **绝不刷分区。** 开发阶段只用 `fastboot boot`（临时启动），
   唯一例外是 `scripts/fuxi-audio-s0.sh` 用的 `fastboot flash boot`，
   那条路径已记录在案且只用于 S0 基线冻结。
3. **不用 Android boot / ADB / vendor ramdisk。** 需要的固件与事实只能来自可复现文件、
   主线源码、固定设备树或 UniXNU 真机只读测量。
4. **一次只扩大一个危险动作。** 同一轮里不要同时首次启用 PAS、GLINK、graph 和 unmute，
   否则失败无法归因。
5. **构建 payload 必须带 GPU addon 和 owner trust cache。** 缺任何一个都会让已经稳定的
   GPU/Metal 回归，且 UniXNU 的打包门禁会直接让构建失败。

---

## 1. 环境与目录

### 1.1 三个仓库的位置

本文所有命令都假设这三个目录同级：

```text
/Users/mac/Documents/deepseek-harness/default-workspace/
  unixnu-fuxi/     ← 设备仓库（本仓库）
  UniXNU/          ← 构建仓库，fork OddlyLabs/UniXNU，分支 fuxi
  uefi-fuxi/       ← UEFI 固件，fork OddlyLabs/uefi-fuxi，分支 fuxi
```

参考实现（可选，用于交叉核对）：

```text
/Users/mac/Documents/deepseek-harness/compare/UniXNU-sheng/
```

### 1.2 工具链要求

| 组件 | 要求 | 检查命令 |
| --- | --- | --- |
| 操作系统 | macOS + Xcode 26 | `xcodebuild -version` |
| 内核编译 | `clang` / `lld` / `llvm`（Xcode 自带或 Homebrew） | `xcrun --sdk iphoneos clang --version` |
| UEFI 固件 | `clang`、`lld`、llvm 工具、`uuid-dev`、BaseTools | `command -v uuidgen` |
| 镜像打包 | `dtc`、`mtools` | `command -v dtc mcopy` |
| 加密（可选） | Homebrew `openssl@3`（系统自带的 LibreSSL 不行） | `$(brew --prefix openssl@3)/bin/openssl version` |
| 上机 | Android platform-tools 的 `fastboot` | `fastboot devices -l` |
| Python | 3.13+（脚本用了 `tomllib`） | `python3 -V` |

Homebrew 安装：

```sh
brew install llvm lld mtools dtc openssl@3 unifdef
```

### 1.3 设备仓库的本地状态

```sh
cd /Users/mac/Documents/deepseek-harness/default-workspace/unixnu-fuxi
git branch --show-current          # 音频线在 V2 分支
git status --short                 # 当前有大量未提交的音频改动，不要清理
git submodule update --init --recursive
```

> **不要** `reset --hard`、`checkout --`、删除未跟踪文件或整体替换文件。
> 音频线的改动全部是未提交状态，丢了无法从 Git 恢复。

---

## 2. 编译

### 2.1 选配（每次改 `board.toml` 或 `configs/fuxi_defconfig` 后）

```sh
cd /Users/mac/Documents/deepseek-harness/default-workspace/UniXNU
./unixnu defconfig fuxi_defconfig -D ../unixnu-fuxi
./unixnu status --json
```

`defconfig` 会合并 arch、SoC、OS 三个 fragment 再叠加 `fuxi_defconfig`。
合并后的 archive 列表必须覆盖合并后配置要求的**每一个** archive，
不能只覆盖模板 defconfig 里那几个——这是配置区 `porting.toml` 里记的踩坑点。

### 2.2 编译内核

```sh
cd /Users/mac/Documents/deepseek-harness/default-workspace/UniXNU
./unixnu build kernel -D ../unixnu-fuxi > /tmp/fuxi-kbuild-marker.log 2>&1; echo "kbuild=$?"
grep -a "error:" \
  "/Users/mac/Documents/deepseek-harness/default-workspace/UniXNU/out/fuxi/ipados-17.7.11-21H461/fuxi/logs/archive-20-build-fuxi-audio-runtime.log" \
  2>/dev/null | head -3
tail -1 /tmp/fuxi-kbuild-marker.log
```

`kbuild=0` 才算成功。音频 runtime 归档的单独编译记录在
`out/fuxi/ipados-17.7.11-21H461/fuxi/logs/archive-20-build-fuxi-audio-runtime.log`，
出问题时先看它。

产物位置：

```text
out/fuxi/ipados-17.7.11-21H461/fuxi/
  kernel/kernel.macho               ← prelink 后的内核
  tree/build/fuxi-audio-runtime/    ← 音频 runtime 的 .o 和 libFuxiAudioRuntime.a
  logs/                             ← 每个 archive 的编译记录
  payload/payload.json              ← 打包清单（含各项 sha256）
  boot/boot.img                     ← 最终启动镜像
```

### 2.3 组装 payload 与 boot.img

```sh
cd /Users/mac/Documents/deepseek-harness/default-workspace/UniXNU

./unixnu build payload -D ../unixnu-fuxi --uefi ../uefi-fuxi \
  --unique-chip-id 0x0123456789abcdef \
  --owner-trust-cache out/fuxi/ipados-17.7.11-21H461/fuxi/tree/build/adreno-metal-plugin/owner-trust.bundle \
  --data-ramdisk out/fuxi/build/owner-data-fixture/owner-data-16g.dbr \
  --gpu-addon out/fuxi/ipados-17.7.11-21H461/fuxi/tree/build/fuxi-gpu-addon.ngpu \
  > /tmp/fuxi-payload-marker.log 2>&1; echo "payload=$?"

./unixnu bootimg -D ../unixnu-fuxi --uefi ../uefi-fuxi --target DEBUG \
  --dtb ../uefi-fuxi/Platforms/Xiaomi/fuxiPkg/FdtBlob/sm8550-xiaomi-fuxi.dtb \
  -o /tmp/fuxi-marker.img > /tmp/fuxi-bootimg-marker.log 2>&1; echo "bootimg=$?"
```

三个 `echo` 必须全是 `0`。任何一个非 0 就停下来查日志，
**不要**用上一次遗留的 payload 或 boot.img 继续——`bootimg` 在 payload 失败时
会静默打包旧产物，这个坑已经踩过。

> `--unique-chip-id` 现在用的是固定测试值 `0x0123456789abcdef`。
> 当前镜像只跑到触控探针的 panic 报告，用不到用户数据；
> 将来要启动到系统时必须换成手机自己的值。

> `--data-ramdisk` 必须用 `out/fuxi/build/owner-data-fixture/owner-data-16g.dbr`。
> **不要**用 `out/ipsw/21H461/data.dbr`，它的 Data UUID 与内核配置不匹配。

### 2.4 固件（只改 UEFI 时才需要）

```sh
cd /Users/mac/Documents/deepseek-harness/default-workspace/UniXNU
./unixnu build firmware -D ../unixnu-fuxi --uefi ../uefi-fuxi --target DEBUG
```

固件卷固定 `0x300000` 字节。这一步不涉及苹果文件，可以在 Linux 上单独跑。

### 2.5 打包门禁（构建后必查）

```sh
cd /Users/mac/Documents/deepseek-harness/default-workspace/UniXNU

jq '{kernel_args, data_volume_uuid, gpu_addon_sha256, trust_cache_sha256}' \
  out/fuxi/ipados-17.7.11-21H461/fuxi/payload/payload.json

shasum -a 256 \
  out/fuxi/ipados-17.7.11-21H461/fuxi/kernel/kernel.macho \
  out/fuxi/ipados-17.7.11-21H461/fuxi/payload/payload.img \
  out/fuxi/ipados-17.7.11-21H461/fuxi/boot/boot.img
```

必须全部满足：

| 检查项 | 要求 |
| --- | --- |
| `data_volume_uuid` | `B2A023C5-CC73-45C1-A9E6-26AC8BDD2822` |
| `gpu_addon_sha256` | 非空 |
| `trust_cache_sha256` | 等于本轮指定的 owner bundle 哈希 |
| `kernel_args` | 不含 `fuxi-audio-probe` 或其它自动音频启动参数 |
| payload 内核哈希 | 等于刚编出的 `kernel.macho` 哈希 |

UniXNU 已经加了门禁：命令行含 `sheng-gpu-addon=1` 时，
缺 `--gpu-addon` 或 `--owner-trust-cache` 必须构建失败。

把三个哈希记下来，每轮实验都要写进记录。

---

## 3. Host 测试

Host 测试全在设备仓库里跑，不需要手机，也不需要 macOS 特有工具
（个别脚本需要 `xcrun`）。

### 3.1 一键汇总（推荐先跑这个）

```sh
cd /Users/mac/Documents/deepseek-harness/default-workspace/unixnu-fuxi
python3 scripts/report-fuxi-audio-status.py
```

它依次跑 16 套音频 host 套件（输出每套的检查项数）和 15 个门禁脚本，
最后打印 `N 个套件，M 项主机检查，K 处失败`。
**任何 FAIL 先修复再构建。**

2026-11-16 在当前工作区的实测结果是
`16 个套件，20592 项主机检查，0 处失败`。
其中 GENI I2C（2169 项）和解析器模糊测试（17841 项）占了大头。

### 3.2 逐个跑

```sh
cd /Users/mac/Documents/deepseek-harness/default-workspace/unixnu-fuxi

# 板级协议层
python3 scripts/check-qcom-mdt-host.py            # 固件容器解析
python3 scripts/check-qcom-a0-host.py             # SMEM / SMP2P / GLINK
python3 scripts/check-qcom-loader-host.py         # QMP / RPMh
python3 scripts/check-qcom-adsp-host.py           # ADSP 加载顺序
python3 scripts/check-qcom-adsp-client-host.py    # ADSP 客户端层
python3 scripts/check-gpr-host.py                 # GPR 报文
python3 scripts/check-qcom-audio-graph-host.py    # AudioReach 构造
python3 scripts/check-qcom-audio-path-host.py     # 静音数据面
python3 scripts/check-qcom-audio-topology-host.py # speaker 拓扑
python3 scripts/check-qcom-audio-ring-host.py     # 共享输出环
python3 scripts/check-qcom-audio-abi-host.py      # 共享音频页 ABI
python3 scripts/check-qcom-audio-capture-host.py  # 采集统计
python3 scripts/check-qcom-audio-service-host.py  # 服务侧端到端
python3 scripts/check-qcom-audio-fuzz-host.py     # 解析器模糊测试
python3 scripts/check-geni-i2c-host.py            # GENI I2C
python3 scripts/check-cirrus-cs35l41-host.py      # CS35L41 安全状态机

# 门禁与记录
python3 scripts/check-fuxi-audio-binding.py            # 板级 binding 对账
python3 scripts/check-fuxi-audio-runtime-binding.py    # 运行时 binding ABI
python3 scripts/check-fuxi-audio-binding-selftest.py   # 门禁自己的测试
python3 scripts/check-fuxi-audio-facts.py              # 板级事实
python3 scripts/check-fuxi-audio-records.py            # 移植记录与文件一致
python3 scripts/check-fuxi-audio-sheng-refs.py         # 与 sheng 源码交叉核对
python3 scripts/check-fuxi-audio-test-results.py       # 结果数据库格式

# 探针阶段门禁
python3 scripts/check-fuxi-audio-probe-h0.py           # 默认启动不可达 audio probe
python3 scripts/check-fuxi-audio-probe-h5.py           # SMEM parser corpus
python3 scripts/check-fuxi-audio-probe-h6.py           # shadow 分配 byte-diff
python3 scripts/check-fuxi-audio-descriptor-host.py    # G2.1 descriptor 生命周期
python3 scripts/check-qcom-hwspinlock-host.py          # H7 hwspinlock 模型
python3 scripts/check-qcom-mapping-host.py             # H8 mapping 合约
python3 scripts/check-qcom-smem-alloc-host.py          # H6 分配差异
python3 scripts/check-qcom-tcsr-hwlock-host.py         # TCSR 编码
python3 scripts/check-fuxi-graph-reply-host.py         # graph reply 匹配器
```

### 3.3 非音频线的其它测试

```sh
cd /Users/mac/Documents/deepseek-harness/default-workspace/unixnu-fuxi

# 触控驱动（子模块）
cd drivers/FuxiFtsTouch
for t in fts-protocol touch-core; do
  gcc -std=c11 -Wall -Wextra -Werror -I. Test/$t.c FtsProtocol.c FtsTouchCore.c -o /tmp/$t && /tmp/$t
done
cd ../..

# 板级常量
gcc -std=c11 -Wall -Wextra -Werror -Ipackage -Idrivers/FuxiFtsTouch \
  tests/board_test.c drivers/FuxiFtsTouch/FtsTouchCore.c -o /tmp/board_test && /tmp/board_test

# 脚本语法
python3 -m py_compile scripts/*.py ci/*.py
bash -n ci/decrypt-boot-image.sh
```

### 3.4 需要外部参考的检查

`check-fuxi-audio-sheng-refs.py` 会读 sheng 的 `AdspProbe.cpp`，
把 binding 里从 sheng 预测来的 SM8550 值读回来比对。找不到 sheng checkout 时它报
`SKIP` 而不是失败。

```sh
python3 scripts/check-fuxi-audio-sheng-refs.py \
  /Users/mac/Documents/deepseek-harness/compare/UniXNU-sheng
```

### 3.5 固件输入清单

```sh
# 盘点 fuxi-firmware checkout 里的 CS35L41 输入与两张 PAS 镜像
python3 scripts/prepare-fuxi-audio-firmware.py .cache/fuxi-firmware
python3 scripts/prepare-fuxi-audio-firmware.py .cache/fuxi-firmware --require-adsp
```

`--require-adsp` 把"缺 PAS 镜像"从警告变成硬失败，用于要出 runtime 镜像的构建。
当前 `.cache/fuxi-firmware` 里 CS35L41 的 11 项输入齐全，
但**两张 PAS 镜像（adsp_dtb / adsp）不在仓库里**——它们从设备本地的
`modem_a`/`modem_b` FAT 镜像里读。

### 3.6 报告生成

```sh
python3 scripts/report-fuxi-audio-probe-reads.py  # 把无来源字段整理成设备读取清单
python3 scripts/report-fuxi-hal-audit.py           # 列出 HAL 插件与 21H461 审计项
```

---

## 4. 上机

### 4.1 先看手机在哪个状态

```sh
cd /Users/mac/Documents/deepseek-harness/default-workspace/unixnu-fuxi
./scripts/fuxi-audio-device.sh
echo "exit=$?"
```

退出码就是答案：

| 退出码 | 状态 | 含义 |
| --- | --- | --- |
| 0 | `fastboot` | bootloader 在听，可以刷/启动镜像 |
| 1 | `serial` | 跑着 XNU 的内核，CDC 控制通道能应答 |
| 2 | `absent` | 手机关了或在别的系统里 |

`absent` 不是故障，通常只是手机关了。

### 4.2 USB / 串口 / fastboot 状态检查

```sh
echo "=== usb ==="; ioreg -p IOUSB -l -w 0 2>/dev/null | grep -i "kUSBProductString" | head -2
echo "=== fastboot ==="; fastboot devices -l 2>&1
echo "=== cdc ==="; ls /dev/cu.usbmodem* 2>/dev/null || echo none
```

三种输出对应三种状态：

- `/dev/cu.usbmodem*` 存在：手机在 XNU CDC 里
- `fastboot devices -l` 有序列号：手机在 fastboot
- 两者都没有：先查 USB 枚举和线缆，不要继续发命令

### 4.3 从串口重启到 fastboot

```sh
cd /Users/mac/Documents/deepseek-harness/default-workspace/unixnu-fuxi
./scripts/fuxi-usb-debug.sh stop >/dev/null 2>&1 || true
sleep 3
./scripts/fuxi-usb-debug.sh start >/dev/null 2>&1
sleep 10
echo "=== ping ==="; ./scripts/fuxi-usb-debug.sh ctl ping 2>&1 | tail -1
echo "=== reboot ==="; ./scripts/fuxi-usb-debug.sh ctl reboot-fastboot 2>&1 | tail -1
sleep 12
fastboot devices -l 2>&1
```

`reboot-fastboot` 成功时 USB 会立刻断开，
所以 "control socket closed before a complete reply" **可以是正常表现**。
判断成功与否只认 `fastboot devices -l`。

### 4.4 刷入并启动镜像

```sh
cd /Users/mac/Documents/deepseek-harness/default-workspace/unixnu-fuxi
echo "=== fastboot ==="; fastboot devices -l 2>&1
if fastboot devices 2>/dev/null | grep -q .; then
  fastboot -s b4c62a1e flash boot /tmp/fuxi-fg5.img 2>&1 | tail -2
  timeout 8 fastboot -s b4c62a1e reboot 2>&1 | tail -1
else
  ./scripts/fuxi-usb-debug.sh start >/dev/null 2>&1
  sleep 6
  ./scripts/fuxi-usb-debug.sh ctl reboot-fastboot 2>&1 | tail -1
  sleep 12
  fastboot devices -l 2>&1
fi
```

这条命令的逻辑是：手机已经在 fastboot 就直接刷；不在就先通过 CDC 让它重启进 fastboot，
再刷。刷的是 `boot` 分区然后重启。

临时启动（不写分区）的版本：

```sh
fastboot boot /tmp/fuxi-marker.img
```

> **注意**：这个 bootloader 承认 `fastboot boot`，但之后**不一定**重新暴露 XNU CDC 设备。
> `scripts/fuxi-audio-s0.sh` 因此改用 `fastboot flash boot` + `fastboot reboot`
> 的"项目已验证"流程。如果 `fastboot boot` 之后 CDC 不出现，不要反复重刷，
> 先看 §7 的排查表。

### 4.5 挂上 CDC 收集器

```sh
cd /Users/mac/Documents/deepseek-harness/default-workspace/unixnu-fuxi
./scripts/fuxi-usb-debug.sh stop >/dev/null 2>&1 || true
./scripts/fuxi-usb-debug.sh start
./scripts/fuxi-usb-debug.sh ctl ping
./scripts/fuxi-usb-debug.sh status
```

采集目录在 `/tmp/fuxi-usb/run-<时间戳>/`，日志是 `xnu-usb.log`。

如果 supervisor 报 `Connection refused`，重做一次 `stop` + `start`。
这是 host 侧的代理生命周期问题，不是设备故障。

定位当前捕获并筛关键行：

```sh
run_dir=$(./scripts/fuxi-usb-debug.sh status 2>&1 | sed -n 's/^run: //p')
rg -n -i \
  'FUXI_ADSP|audio|glink|gpr|apm|cs35|i2c|fuxi-metal|fuxi-display|panic|data abort|smmu.*fault|gpu.*reset|launchd' \
  "$run_dir/xnu-usb.log"
```

### 4.6 CDC 控制通道命令

`fuxi-usb-debug.sh ctl <verb>` 支持的 verb（来自 `drivers/QcomXnuUsbDebug/scripts/xnu_usb_control.py`）：

| Verb | 用途 |
| --- | --- |
| `hello` | 握手（`ctl` 会自动先发一次） |
| `ping` | 活性检查 |
| `status` | 通道与缓冲区统计 |
| `reboot` | 重启 |
| `reboot-fastboot` | 重启进 fastboot |
| `screenshot` / `shotnext` | 屏幕抓取 |
| `diagstatus` | 读诊断状态页（当前状态视图） |
| `diagarm` / `diagnext` / `diagcancel` / `diagreset` | 推进固定操作 ID 的诊断阶梯 |

---

## 5. 音频专项流程

### 5.1 实时读诊断状态

`fuxi_audio_diag_resources()` 把 16 个 12-bit 的诊断页复用进一个 32-bit 的 `resources` 字段，
所以要连续读几次才能采全所有页。轮询器会做这件事：

```sh
cd /Users/mac/Documents/deepseek-harness/default-workspace/unixnu-fuxi
python3 scripts/live-fuxi-audio-result.py --interval 2
```

它写 `tests/fuxi-audio/live.json`（当前快照）和
`tests/fuxi-audio/live-history.jsonl`（时间序列）。
只读，每次一个 DIAGSTATUS 请求；正式结果数据库仍然是 append-only。

`live.json` 的关键字段：

| 字段 | 含义 |
| --- | --- |
| `session` | CDC session id |
| `page` | 当前复用页号（0–15） |
| `value` | 该页的 12-bit 值 |
| `stage` | page 0 时解出的阶段号 |
| `last_error` | page 0 时解出的 errno |
| `raw` | 回包的全部固定字段 |

### 5.2 记录一轮结果

```sh
python3 scripts/show-fuxi-audio-test-results.py             # 看最新一轮
python3 scripts/show-fuxi-audio-test-results.py --id R-GRAPH-02
python3 scripts/show-fuxi-audio-test-results.py --json

python3 scripts/record-fuxi-audio-run.py --run-json /tmp/fuxi-run.json
python3 scripts/check-fuxi-audio-test-results.py
```

结果名含义（完整表见 `tests/fuxi-audio/README.md`）：

| 结果 | 含义 |
| --- | --- |
| `PASS_READY` | 六段 PAS 严格 x0/x0，SMP2P item 443 header 有效且 `slave-kernel` ready |
| `PASS_GLINK` | `PASS_READY` + item 478/479/480 + GLINK 版本一致 + `adsp_apps` 通道 up |
| `PASS_GPR` | `PASS_GLINK` + 收到 `SPF_STATE` ready 回包 |
| `BLOCKED_GPR` | 有限 GPR/SPF 查询跑了，但没收到 ready |
| `PASS_SPEAKER` | 完整扬声器验收：两颗功放分别过、5/5 start/tone/stop、mute 恢复、无 fault、平台回归绿 |
| `BLOCKED` | 某个具体阶段停住，对象里记了 stage/error 和 SCM x0/x1 |

> **`PASS_READY` 不是扬声器 PASS**，不能从中推断有声音。

### 5.3 S0 安全基线

```sh
cd /Users/mac/Documents/deepseek-harness/default-workspace/unixnu-fuxi
./scripts/fuxi-audio-s0.sh
```

每跑一次记录一轮，**跑三次**，三份记录合起来才是 S0 的门禁。
脚本会检查镜像里的内核确实是刚编出来的、Data UUID 正确、
GPU addon 和 owner trust 都在、并且默认启动里没有跑任何 audio step。

记录追加在 `facts/audio/s0-boots.txt`。

### 5.4 音频探针镜像（一站式）

```sh
cd /Users/mac/Documents/deepseek-harness/default-workspace/unixnu-fuxi
./scripts/fuxi-audio-probe.sh run     # 编译 + 启动 + 打报告
./scripts/fuxi-audio-probe.sh build   # 只编译
./scripts/fuxi-audio-probe.sh report  # 只打最新报告
./scripts/fuxi-audio-probe.sh status  # 手机在哪
```

这个脚本用**临时副本**的 `board.toml` 加上 `fuxi-audio-probe=1`，
不改动仓库里的 `board.toml`，也不改 defconfig。
它会校验 payload 里带的内核就是刚编出来的那一份。

### 5.5 分阶段测试矩阵

| 镜像 | boot args | 测试内容 | PASS |
| --- | --- | --- | --- |
| I1 | `fuxi-adsp=1` | 只读 firmware inventory | 两个 MDT parsed、error=0、系统正常启动 |
| A1 | `fuxi-adsp=2`，audio off | mapping + metadata + PAS | SCM=0、stage 至少 10，无 AP panic |
| A2 | 同上 | SMP2P ready | stage=11、fatal=0 |
| A3 | 同上 | GLINK + `adsp_apps` | 版本一致、通道 up、stage≥13 |
| C1 | ADSP up，speaker graph off | 两颗功放 muted-ready | ID/revision/PLL/fault PASS，仍无声音 |
| D1 | ADSP up，amps mute | graph + zero PCM | 32/32 periods，无 fault |
| T1 | `fuxi-audio=1`，单 B | 1 秒 −30 dBFS tone | 可闻、完成、立即 mute、无 fault |
| T2 | 单 T | 同上 | 同上 |
| T3 | B+T | 同上 | 两路可闻、完成、无 fault |
| R5 | B+T | 5 次 start/tone/stop | 5/5，资源归零，GPU/CDC/Data 正常 |

当前 `board.toml` 的 `kernel_args` 里 `fuxi-adsp=2` 和 `fgraph=6` 已经打开，
`fuxi-amp=1` 也开着——这些是当前 bring-up 的位置，不是默认发布配置。

---

## 6. 其它子系统

### 6.1 USB 调试组件

```sh
python3 scripts/prepare-fuxi-usb-debug.py \
  --module drivers/QcomXnuUsbDebug --xnu refer/xnu --output .cache/fuxi-qud-module
```

`scripts/run-fuxi-usb-kernel.py` 是内核编译配方的包装器，会在编译前把
USB 调试源以固定的 Darwin23/fuxi profile 打进构建树。
设 `FUXI_USB_REFERENCE_ONLY=1` 则只做 reference 链接，不产出可上机镜像。

### 6.2 GPU / 显示 / 存储

```sh
python3 scripts/make-fuxi-gpu-addon.py          # 构建 GPU addon（带 publish marker）
python3 scripts/audit-mobile-graphics-legacy.py  # 读 prelinked 里的 IOMobileGraphicsFamily
python3 scripts/stage-prelink-candidate.py      # 暂存 prelink 候选供上面的审计用
python3 scripts/build-userdata-root-image.py    # 构建 userdata 原始镜像
python3 scripts/seed-data-skip-setup.py         # 做一个直接进桌面的 Data seed
python3 scripts/build-fuxi-display.py           # 显示组件
python3 scripts/prepare-fuxi-gmu.py             # GMU 准备
python3 scripts/prepare-fuxi-gmu-votes.py       # GMU 投票
```

`audit-mobile-graphics-legacy.py` 需要一个 prelink 候选，
它先在设备仓库里找，因为构建树是从零重建的、构建产物不保留。
`stage-prelink-candidate.py` 负责把内核步骤的输出复制过去并检查两个不变量。

### 6.3 音频 HAL 插件

```sh
python3 scripts/build-fuxi-audio-hal.py OUTDIR [TARGET]
```

`TARGET` 默认 `arm64-apple-ios17.0`。产出 `OUTDIR/FuxiAudio.driver` 和
`OUTDIR/trust.json`（可执行文件的 CDHash，进 owner trust cache）。
**这只是构建，不是运行**——21H461 的 ABI 审计和全部设备检查都还在后面。

---

## 7. 排查表

| 现象 | 首查 | 禁止做法 |
| --- | --- | --- |
| `kbuild` 非 0 | `logs/archive-*.log` 里的 `error:` | 跳过构建直接打包 |
| `payload` 非 0 但 `bootimg` 成功 | payload 是否缺 `--gpu-addon` / `--owner-trust-cache` | 用上一次的 boot.img 上机 |
| `bootimg` 打包了旧 payload | 三个日志都要看 | 相信 `bootimg=0` |
| `data_volume_uuid` 不对 | `--data-ramdisk` 指错了 | 改 UUID 凑数 |
| fastboot devices 为空 | 手机在别的模式，或线缆 | 反复重刷 |
| `ctl ping` 报 socket not ready | 手机还没进 XNU，或没带 `qcom-usb-ctl=1` | 重刷镜像 |
| `ctl` 报 `Connection refused` | supervisor 生命周期问题 | 重做 `stop` + `start` |
| `fastboot boot` 后 CDC 不出现 | bootloader 不保证重新暴露 CDC | 改用 `fastboot flash boot` + `reboot` |
| CDC 恢复但 session 变了 | 设备复位过 | 认为是 host 问题 |
| `AUDIO step=` 出现在默认启动 | `board.toml` 里有音频开关 | 带着它做 S0 |
| 设备复位且 poll 计数低 | 上一次危险动作（SE 寄存器写、PAS、unmute） | 一轮里同时开多个 |
| host 全 PASS 但设备没声音 | 硬件证据缺失 | 用 host 结果替代 device 结论 |

更多诊断规则见 `docs/fuxi-audio-probe-debug-runbook.md` 的
第 8 节故障分类和第 9 节明确禁区。

---

## 8. 记录与 CI

### 8.1 移植记录

`porting.toml` 是共享记录。改了哪个子系统的文件，就要同步更新对应 area 的
`findings` 或 `evidence`，并跑检查器：

```sh
cd /Users/mac/Documents/deepseek-harness/default-workspace/UniXNU
python3 scripts/porting-record.py check --device ../unixnu-fuxi
```

主机结果和设备观察分开记录，并写清楚做测试时的 commit。

### 8.2 CI 任务

| 任务 | 运行环境 | 检查内容 |
| --- | --- | --- |
| Host tests | Ubuntu 24.04 | FuxiFtsTouch 测试（gcc/clang，ASan/UBSan）、板级常量测试、脚本语法 |
| Kernel-target compile | macOS 26 + Xcode | 触控 C 代码按 arm64 内核代码编译，确认不调 libc |
| fuxi boot image | macOS 26 + Xcode | 编译内核、归档、链接、prelink，检查触控探针/PMGR 桥/HID 绑定在核心里，再构建 payload、固件、启动镜像，加密后上传 |
| Porting record | Ubuntu 24.04 | 用 fork 里的 UniXNU 检查器验证 `porting.toml` |

启动镜像里有苹果 kernelcache 的内容，所以只上传用 `ci/boot-image-recipient.pem`
加密过的版本；私钥只在机主自己的电脑上。其余产物只上传文本日志。

更新上游后要改 `.github/workflows/build.yml` 里固定的两个提交
（`UNIXNU_REF`、`UEFI_REF`），让 CI 重跑。

### 8.3 一轮实验的最小记录

每轮至少保存：

- `boot.img` SHA-256、`kernel.macho` SHA-256
- `gpu_addon_sha256`、`trust_cache_sha256`、`data_volume_uuid`
- CDC session id
- 完整 boot args
- ADSP/audio stats（哪个 page、什么值）
- 最后一个成功的 stage、精确 errno / SCM x0/x1
- GPU fence（submitted/retired）、GOP（offer/done）
- panic / fault 状态
- 捕获目录路径

---

## 9. 命令速查

```sh
D=/Users/mac/Documents/deepseek-harness/default-workspace/unixnu-fuxi
U=/Users/mac/Documents/deepseek-harness/default-workspace/UniXNU

# 编译
cd $U && ./unixnu defconfig fuxi_defconfig -D ../unixnu-fuxi
cd $U && ./unixnu build kernel -D ../unixnu-fuxi
cd $U && ./unixnu build payload -D ../unixnu-fuxi --uefi ../uefi-fuxi \
  --unique-chip-id 0x0123456789abcdef \
  --owner-trust-cache out/fuxi/ipados-17.7.11-21H461/fuxi/tree/build/adreno-metal-plugin/owner-trust.bundle \
  --data-ramdisk out/fuxi/build/owner-data-fixture/owner-data-16g.dbr \
  --gpu-addon out/fuxi/ipados-17.7.11-21H461/fuxi/tree/build/fuxi-gpu-addon.ngpu
cd $U && ./unixnu bootimg -D ../unixnu-fuxi --uefi ../uefi-fuxi --target DEBUG \
  --dtb ../uefi-fuxi/Platforms/Xiaomi/fuxiPkg/FdtBlob/sm8550-xiaomi-fuxi.dtb -o /tmp/fuxi-marker.img

# Host 测试
cd $D && python3 scripts/report-fuxi-audio-status.py

# 上机
cd $D && ./scripts/fuxi-audio-device.sh
cd $D && ./scripts/fuxi-usb-debug.sh start
cd $D && ./scripts/fuxi-usb-debug.sh ctl ping
cd $D && ./scripts/fuxi-usb-debug.sh ctl reboot-fastboot
cd $D && fastboot boot /tmp/fuxi-marker.img

# 诊断与记录
cd $D && python3 scripts/live-fuxi-audio-result.py --interval 2
cd $D && python3 scripts/record-fuxi-audio-run.py --run-json /tmp/fuxi-run.json
cd $D && python3 scripts/check-fuxi-audio-test-results.py
```

---

## 附录 A：相关文档

| 文档 | 内容 |
| --- | --- |
| [`README.md`](README.md) | 仓库总览、现状、构建与上机 |
| [`AGENTS.md`](AGENTS.md) | Agent 行为准则、事实来源规则 |
| [`porting.toml`](porting.toml) | 移植记录主文件（13 个 area） |
| [`fuxi_touch_panel_porting.md`](fuxi_touch_panel_porting.md) | 触控与面板硬件事实 |
| [`USB-RESTORE-20261102.md`](USB-RESTORE-20261102.md) | USB 调试恢复流程 |
| [`docs/fuxi-audio-completion-roadmap.md`](docs/fuxi-audio-completion-roadmap.md) | 音频 S0–S18 阶段总图与验收 |
| [`docs/fuxi-audio-first-sound-agent-runbook.md`](docs/fuxi-audio-first-sound-agent-runbook.md) | 首次出声的 Agent 操作指南 |
| [`docs/fuxi-audio-probe-debug-runbook.md`](docs/fuxi-audio-probe-debug-runbook.md) | 故障隔离规则、H0–H8 / G0–G6 用例 |
| [`docs/fuxi-audio-porting-log.md`](docs/fuxi-audio-porting-log.md) | 逐轮真机证据 |
| [`docs/fuxi-audio-probe-debug-log.md`](docs/fuxi-audio-probe-debug-log.md) | 已执行阶段的证据摘要 |
| [`docs/UNDO.md`](docs/UNDO.md) | 临时取舍与恢复清单 |
| [`tests/fuxi-audio/README.md`](tests/fuxi-audio/README.md) | 结果数据库格式与当前前沿 |
| [`docs/gpu-bringup.md`](docs/gpu-bringup.md) | GPU bring-up |

## 附录 B：许可证

BSD-2-Clause-Patent，见 [`LICENSE`](LICENSE)。
