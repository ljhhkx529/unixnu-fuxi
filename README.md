# unixnu-fuxi：小米 13（fuxi）的 UniXNU 设备仓库

[![Build](https://github.com/OddlyLabs/unixnu-fuxi/actions/workflows/build.yml/badge.svg)](https://github.com/OddlyLabs/unixnu-fuxi/actions/workflows/build.yml)
[![Device documentation](https://github.com/OddlyLabs/unixnu-fuxi/actions/workflows/porting-record.yml/badge.svg)](https://github.com/OddlyLabs/unixnu-fuxi/actions/workflows/porting-record.yml)

Device repository for the Xiaomi 13 (fuxi, SM8550), created from UniXNU's `examples/device` template. It builds with the
fork [OddlyLabs/UniXNU](https://github.com/OddlyLabs/UniXNU) (branch `fuxi-fts-touch`), whose component catalog has
`FuxiFtsTouch`.

主文档：[fuxi_touch_panel_porting.md](fuxi_touch_panel_porting.md)。各子系统的状态以 [porting.toml](porting.toml) 为准。

## 现状

| 子系统 | 状态 |
|---|---|
| 触控（ST FTM5） | 驱动和分阶段探针已写好，内核在 CI 里编译；还没有上机 |
| 显示、存储、固件、CPU、GPU、网络、音频、电源 | 计划中；显示和电源的硬件资料已整理 |

## 相关仓库

| 仓库 | 作用 |
|---|---|
| [OddlyLabs/UniXNU](https://github.com/OddlyLabs/UniXNU) | fork 自 UniXNU/UniXNU。分支 `fuxi-fts-touch` 在组件目录里加了 `FuxiFtsTouch` |
| [OddlyLabs/FuxiFtsTouch](https://github.com/OddlyLabs/FuxiFtsTouch) | 触控组件：FTM5 协议和触点跟踪，纯 C，带主机测试；本仓库用子模块引用它 |
| 本仓库 | 板级文件、探针、启动钩子补丁、硬件资料 |

## 目录

| 路径 | 内容 |
|---|---|
| `board.toml`、`feeds.toml`、`configs/fuxi_defconfig` | 板子、组件、归档顺序、SoC 版本、内核配置 |
| `dts/fuxi.dts` | 设备树（包含 UniXNU-sm8550-common 的 `sm8550.dtsi`） |
| `patches/xnu/` | `fuxi-touch-probe.patch`：`bsd_init` 调用触控探针 |
| `package/` | 在构建树里是 `kits/fuxi`。`FuxiTouchProbe.cpp`（探针）、`FuxiTouchBoard.h`、`FuxiPanelBoard.h`（板级常量，每项注明来源）；`TouchBoard.h`、`QcomRpmh.*`、`SmmuBypass.h` 原样取自 UniXNU-sheng |
| `scripts/build-fuxi-touch.py` | 用 XNU 构建的编译记录编译探针和驱动，打包成 `libFuxiTouch.a` |
| `drivers/FuxiFtsTouch` | 子模块，指向 FuxiFtsTouch |
| `porting.toml` | 移植记录（UniXNU 格式） |
| `tests/board_test.c` | 板级常量测试 |
| `facts/` | 从官方设备树导出的触控、面板事实，以及 73 组面板 DSI 命令序列 |

本地另有 `dtb/`、`firmware/`、`power/`、`ref_src/` 四个目录，不进 Git（见 `.gitignore`）：官方 DTB 和触控固件是厂商文件；dmesg 里有使用痕迹；`ref_src/` 是 GPL 参考源码，只供查阅。

## 构建

需要 macOS 和 Xcode 26（UniXNU 也支持 Linux 工具链，见其文档）。

```bash
git clone --recurse-submodules https://github.com/OddlyLabs/unixnu-fuxi
```
```bash
git clone -b fuxi-fts-touch https://github.com/OddlyLabs/UniXNU
```
```bash
cd UniXNU && ./unixnu defconfig fuxi_defconfig -D ../unixnu-fuxi && ./unixnu build kernel
```

## CI（GitHub Actions）

| 任务 | 运行环境 | 检查内容 |
|---|---|---|
| Host tests | Ubuntu | FuxiFtsTouch 测试（gcc/clang，ASan/UBSan）、板级常量测试、脚本语法 |
| Kernel-target compile | macOS 26 + Xcode | 驱动的 C 代码按 arm64 内核代码编译，确认不调用 libc |
| fuxi kernel | macOS 26 + Xcode | 按 UniXNU 流程编译 fuxi 内核（XNU 构建、归档、链接、prelink），确认触控探针和驱动的符号在内核里 |
| Porting record | Ubuntu | 用 fork 里的 UniXNU 检查器验证 `porting.toml` |

CI 只上传文本日志。内核、kernelcache 和 IPSW 内容来自苹果软件，不上传。

## 上机（还没做）

- 只用 `fastboot boot` 临时启动，不写任何分区。
- 还需要：这台手机的唯一芯片 ID（payload 的 `--unique-chip-id`）、选定一个 uefi-fuxi 固件版本，然后构建 payload 和启动镜像。
- 默认启动参数是 `fuxi-touch=3 fuxi-touch-panic=1`：探针跑完第 3 阶段（上电、时钟、SPI、复位、读芯片 ID）后 panic，在 panic 屏上读 4 行 FTS 诊断，对照主文档第 7.2 节。

## 本地测试

```bash
cd drivers/FuxiFtsTouch && for t in fts-protocol touch-core; do gcc -std=c11 -Wall -Wextra -Werror -I. Test/$t.c FtsProtocol.c FtsTouchCore.c -o /tmp/$t && /tmp/$t; done
```

## 许可证

BSD-2-Clause-Patent，见 [LICENSE](LICENSE)。
