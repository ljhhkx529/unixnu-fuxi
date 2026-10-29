# fuxi: Lineage vendor DTS -> mainline 映射

本表只覆盖 fuxi XNU / Linux reader 当前会消费的节点。Lineage 仓库的
完整 vendor overlay 不能整份搬进 mainline；面板、触摸、PMIC、audio、
camera 等 `qcom,` 自定义节点暂不迁移。

来源：

- Lineage source: `LineageOS/android_kernel_xiaomi_sm8550-devicetrees`
  commit `43b5ada5654ed84c25f74789bc8f6326b7bacd20`，路径 `qcom/`
- 真机 vendor DT: `uefi-fuxi/Resources/DTBs/fuxi.dtb` 反编译值
- mainline 基线: `unixnu-fuxi/dts/sm8550.dtsi`

## 1. 内存与 reserved-memory

| 项目 | Lineage / vendor 值 | mainline 目标 | 处理 |
| --- | --- | --- | --- |
| XNU DRAM 窗口 | `board.toml` `dram_size = 0x80000000`（SMEM 观测） | `memory@80000000` reg `0x80000000/0x80000000` | 保持，不改成整机 RAM |
| splash | vendor `/reserved-memory/splash_region` label `cont_splash_region`，`0xb8000000/0x2b00000` | `framebuffer@b8000000` | 已一致 |
| GPU zap 微码 | `gpu_micro_code_region@9b09a000`，`0x9b09a000/0x2000` | `gpu-microcode@9b09a000`，经 `&gpu/zap-shader` 引用 | 本次加入 |
| AOP command DB | `aop_cmd_db_region@81c60000`，`0x81c60000/0x20000` | `aop-cmd-db@81c60000`，`compatible = "qcom,cmd-db"` | 本次加入 |
| qseecom 等共享池 | vendor 大量 `shared-dma-pool` | 不迁移 | XNU 不使用 |

## 2. GPU

| 项目 | Lineage / vendor 值 | mainline 目标 | 处理 |
| --- | --- | --- | --- |
| 节点 | `msm_gpu: qcom,kgsl-3d0@3d00000` | `gpu: gpu@3d00000` | 已在 `sm8550.dtsi` |
| compatible | vendor dtb `qcom,adreno-gpu-gen7-2-1`,`qcom,kgsl-3d0`；Lineage base 文件为 `gen7-2-0` | `qcom,adreno-43050a01`,`qcom,adreno` | 保持，chip id `0x43050a01` 与 `AdrenoCompilerProbe.c` 一致 |
| reg 主窗口 | `kgsl_3d0_reg_memory` `0x3d00000/0x40000` | 同 | 一致 |
| `cx_dbgc` | `0x3d61000/0x800` | 同 | 一致 |
| `cx_misc` | `0x3d9e000/0x1000` | `cx_mem`（Linux mainline 命名） | 保持 mainline 命名 |
| `rscc` / `isense_cntl` / `qdss_*` | vendor GPU 节点还有多段 | 不放进 `&gpu` | XNU 从 `&gmu` 的 `rscc` 窗口读 RSCC |
| interrupt | SPI 300，level-high | `interrupts = <0 300 4 0>` | 一致 |
| SMMU SIDs | vendor CB：`gfx3d_user=0`,`gfx3d_lpac=1`,`gfx3d_secure=2` | `iommus = <&gpu_smmu 0 0>,<&gpu_smmu 1 0>` | 一致 |
| zap | `zap-shader { memory-region = <&gpu_micro_code_mem>; }` | 同 | 本次加入 |
| OPP | vendor `qcom,gpu-pwrlevel-bins` 0 档，9 个 `qcom,gpu-freq`/`qcom,level`/`qcom,bus-freq`/`qcom,acd-level` | mainline `gpu_opp_table`，`opp-hz`/`opp-level`/`opp-peak-kBps`/`qcom,opp-acd-level` | 本次加入，数值取自 fuxi vendor DTB，不是 sheng |
| GMU 绑定 | vendor 无显式 `qcom,gmu` | `qcom,gmu = <&gmu>`，`#cooling-cells = <2>` | 本次加入 |

## 3. GMU

| 项目 | Lineage / vendor 值 | mainline 目标 | 处理 |
| --- | --- | --- | --- |
| 节点 | vendor `gmu: qcom,gmu@3d69000`（节点名与 reg 基址不一致） | `gmu: gmu@3d68000` | 按真实 reg 基址命名 |
| reg `gmu` | `0x3d68000/0x37000` | 同 | fuxi 专用，不能回退 sheng 的 `0x3d6a000/0x35000` |
| reg `gmu_pdc` | `0xb280000/0x10000` | 同 | 一致 |
| reg 第三窗口 | vendor `gmu_ao_blk_dec0` `0x3d40000/0x10000` | mainline `rscc` `0x3d50000/0x10000` | XNU `GpuPowerProbe` 用 RSCC `0x3d50000`；取自 vendor GPU 的 `rscc` 段 |
| interrupt | SPI 304/305，level-high | `interrupts = <0 304 4 0>,<0 305 4 0>` | 一致 |
| interrupt-names | `hfi`,`gmu` | 同 | 一致 |
| SMMU SID | `iommus = <&kgsl_smmu 0x5 0>` | `iommus = <&gpu_smmu 5 0>` | 一致 |
| clocks/power | vendor `gmu_clk/cxo_clk/axi_clk/memnoc_clk/ahb_clk/hub_clk` + `vddcx/vdd` | mainline `clocks` 7 路、`power-domains = <&gpucc 0>,<&gpucc 1>`、`qcom,qmp = <&aoss_qmp>` | 本次加入，clock/power id 来自 SoC 公共定义 |
| OPP | vendor `qcom,gmu-freq-table = <220000000,64>,<550000000,128>` | `gmu_opp_table` 2 档 | 本次加入，数值为 fuxi 真机表 |

## 4. GPU SMMU

| 项目 | Lineage / vendor 值 | mainline 目标 | 处理 |
| --- | --- | --- | --- |
| 节点 | `kgsl_smmu: kgsl-smmu@3da0000` | `gpu_smmu: iommu@3da0000` | 已在 `sm8550.dtsi` |
| reg | base `0x3da0000/0x40000`，tcu `0x3de6000/0x40` | base `0x3da0000/0x40000` | XNU 只用 base 窗口，不迁移 tcu |
| compatible | `qcom,qsmmu-v500`,`qcom,adreno-smmu` | `qcom,sm8550-smmu-500`,`qcom,adreno-smmu`,`qcom,smmu-500`,`arm,mmu-500` | 保持 mainline |
| 一致性 | vendor 无 `dma-coherent`，XNU probe 按 dma-coherent 语义跑 | `dma-coherent` | 本次加入 |
| interrupt/clocks/power | vendor `kgsl_smmu` 有完整 interrupt 与 `gpu_cc_hlos1_vote_gpu_smmu` 等 clock | mainline `#global-interrupts/interrupts/clocks/power-domains` | 本次加入，clock id 为 SoC 公共值 |

## 5. UFS

| 项目 | vendor 值 | mainline / XNU | 处理 |
| --- | --- | --- | --- |
| UFS HC | `0x1d84000/0x3000` | `sm8550.dtsi` 已声明 | 一致 |
| apps SMMU SID | `iommus = <&apps_smmu 0x60>` | `&ufs { iommus = <&apps_smmu 0x60 0>; }`，`apps_smmu` 节点启用 | 本次加入，DTS 与驱动硬编码一致 |

## 6. 明确不迁移

- `qcom,msm-id`、`qcom,board-id`、`xiaomi,miboard-id`：ABL/QCom boot 匹配，非 mainline/XNU 语义。
- panel、touch、pinctrl、regulator、PMIC、audio、camera 的 vendor 节点：绑定与驱动不同，XNU 不消费。

## 本次 DTS 变更

- `sm8550-xiaomi-fuxi.dts`：
  - 增加 `gpu-microcode@9b09a000`、`aop-cmd-db@81c60000`
  - `&gpu` 增加 `gpu_zap_shader: zap-shader`，`firmware-name = "qcom/sm8550/fuxi/a740_zap.mbn"`
  - `&gpu_smmu` 增加 `dma-coherent`
- 重新编译 `sm8550-xiaomi-fuxi.dtb` 验证通过。
- 新增 `package/audit-fuxi-platform.py`：编译并校验 fuxi mainline DTB 的 GPU/GMU/SMMU 资源、SID、zap、command-db；运行已 PASS。
