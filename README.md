# unixnu_fuxi：小米 13 触控与面板驱动

[![Build](https://github.com/OddlyLabs/unixnu-fuxi/actions/workflows/build.yml/badge.svg)](https://github.com/OddlyLabs/unixnu-fuxi/actions/workflows/build.yml)

面向 [UniXNU](https://github.com/UniXNU/UniXNU) 的小米 13（fuxi，SM8550）移植资料和触控驱动。

主文档：[fuxi_touch_panel_porting.md](fuxi_touch_panel_porting.md)

## 内容

| 路径 | 内容 |
|---|---|
| `drivers/FuxiFtsTouch/` | ST FingerTip FTM5 触控驱动，纯 C，不分配内存、不依赖 libc。`FtsProtocol.c` 负责协议：组帧、复位与就绪、读 FIFO、开始扫描；`FtsTouchCore.c` 负责事件到触点的跟踪，以及坐标映射。`Test/` 是假芯片主机测试 |
| `package/FuxiTouchProbe.cpp` | XNU 内核侧探针，启动参数 `fuxi-touch=1..4` 分阶段打开：快照 → RPMh 上电、时钟、引脚 → GENI SPI、复位、芯片 ID → 扫描、中断、发布触点 |
| `include/FuxiTouchBoard.h` | 触控板级常量（SPI4、GPIO、中断、电源、坐标、屏下指纹区、芯片 ID），每项注明来源 |
| `include/FuxiPanelBoard.h` | 面板与显示控制器常量（分辨率、DSC、时序、地址、GPIO、电源顺序、帧缓冲、亮度） |
| `tests/board_test.c` | 板级常量测试，并与驱动的协议常量交叉核对 |
| `ci/kernel-compile-check.py` | 用真实 XNU 构建留下的编译记录，按 sheng 编译自己触控探针的同一套参数，编译本项目的探针和驱动，再检查符号能否解析 |
| `ci/integrate-sheng.py` | 把探针和驱动接进 UniXNU-sheng 结构的源码树：替换 `TouchProbe.cpp`，并把驱动源码加进构建脚本。以后建 fuxi 设备仓库时可以照着用 |
| `.github/workflows/build.yml` | CI，见下 |
| `dts/fuxi.dts` | UniXNU 设备树草稿（格式同 UniXNU-sheng） |
| `facts/` | 从合并设备树导出的事实（触控、面板），以及 73 组逐条解码的面板 DSI 命令序列 |

本地另有 `dtb/`、`firmware/`、`power/`、`ref_src/` 四个目录，不进 Git（见 `.gitignore`）：
- 官方 DTB 和触控固件属于厂商文件；
- dmesg 里有使用痕迹；
- `ref_src/` 是 GPL 参考源码，只供查阅，不能复制进本项目。

## CI（GitHub Actions）

| 任务 | 运行环境 | 检查内容 |
|---|---|---|
| Host tests | Ubuntu | gcc 和 clang 跑协议、触点、板级测试（`-Werror`，ASan/UBSan）；集成脚本对 sheng 源码树试跑 |
| Kernel-target compile | macOS 26 + Xcode | 用 iOS SDK 的 clang 按 arm64 内核代码编译驱动（`-mkernel -ffreestanding -mgeneral-regs-only`），并确认除了 XNU 自带的 `memset` 一类函数，不引用任何外部符号 |
| Kernel compile check | macOS 26 + Xcode | 先按 UniXNU 的流程跑一遍 sheng 配置的 XNU 构建，拿到真实的内核编译参数。然后：用同一套参数编译 sheng 原版探针，作为对照；编译本项目的探针和驱动；检查它们引用的每个强符号都能在 XNU 目标文件或本项目目标文件里找到，并且没有和 XNU 重复定义的符号 |

为什么不完整链接一个内核：sheng 的内核构建里有一步 `build-sheng-platform-power.py`，它需要一个用苹果 kernelcache 预链接过的参考内核。这个参考内核只在维护者本地的旧构建产物里，全新的 CI 机器上没有，所以完整构建会停在这一步，走不到触控代码。fuxi 和 sheng 的触控总线完全相同（QUP1 SE4、GPIO44–47、中断 GPIO25），用 sheng 的构建参数检查是有效的。CI 只上传文本日志，不上传任何内核或镜像。

## 本地测试

```bash
cd drivers/FuxiFtsTouch && for t in fts-protocol touch-core; do gcc -std=c11 -Wall -Wextra -Werror -I. Test/$t.c FtsProtocol.c FtsTouchCore.c -o /tmp/$t && /tmp/$t; done
```

`facts/` 由本地的合并 DTB 生成；生成脚本和 DTB 都不在本仓库。
