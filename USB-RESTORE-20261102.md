# fuxi USB CDC restore / USB 日志恢复

Target / 目标：iPadOS 17.7.11 (21H461), iPad7,1, XNU 10063.141.1
`d8b80295118ef25ac3a784134bcf95cd8e88109f`, format 1 / old branch.

The existing UFS, GPU, PMGR, root policies and build baseline remain selected.
`fuxi-kernel-baseline.sh` is the previous board recipe; its launcher adds a
separate `darwin23-fuxi` USB profile. The previous three-hook USB patch is
retained as `.disabled` and the FuxiUsbCompat archive is no longer linked.

保留本板原有 UFS、GPU、PMGR、根卷策略与构建配方。旧三处 USB 挂钩停用，
由独立 Darwin 23 配置接入 PE、中央控制台、debug/panic 和正常 worker。

QcomXnuUsbDebug is pinned to the previously checked `83be5782ad7227d7d245862ce1ae3272cae14242`.
The fuxi-only exact-stream patch permits takeover of one revalidated firmware
USB mapping into a separate private translated DMA bank, retaining restoration
state. It never enables global SMMU bypass. SoC resources remain the supplied
SM8550 profile. Firmware must already own USB clocks, power, PHY and DEVICE role.
Reset control is disabled. VID/PID is 1209:316d, serial FuxiXnuUsbDebug.

采用之前检查过的驱动提交及 fuxi exact-stream 修复；只接管重新核对过的单个
固件 USB 映射，使用独立私有转换域，并保存恢复状态。不启用全局 SMMU bypass。
仍依赖固件已经初始化 USB 供电、时钟、PHY 和设备模式；复位控制关闭。

`build-fuxi-usb-debug.py` runs transport/DMA models, builds from actual kernel
compile records and checks the six XNU units with USB enabled and disabled.
The final kernel must contain the driver symbols. Host checks and hardware
enumeration/received logs are separate; no new phone test is claimed.

归档步骤运行传输/DMA 模型，按真实编译记录构建，并检查六个 XNU 单元的开启/
关闭配置。完整链接后核对驱动符号。主机检查不等同于手机枚举和日志采集成功。
本次尚无新的实机验收结果。
