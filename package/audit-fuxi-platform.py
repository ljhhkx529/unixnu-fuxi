#!/usr/bin/env python3
"""Check fuxi mainline-style board DTS against pinned board resources.

Unlike the sheng audit, the input here is the board DTS owned by this
repository (dts/sm8550-xiaomi-fuxi.dts + dts/sm8550.dtsi), not an external Linux
tree.  This keeps the mainline-style DTB and the GPU platform audit in one
place while still asserting the exact Adreno 740 / GMU / SMMU resources.
"""
from pathlib import Path
import hashlib
import json
import subprocess


ROOT = Path(__file__).resolve().parents[1]
DTS = ROOT / "dts" / "sm8550-xiaomi-fuxi.dts"
OUT = ROOT / "build" / "fuxi-platform"
DTB = OUT / "sm8550-xiaomi-fuxi.dtb"

GPU = "/soc/gpu@3d00000"
GMU = "/soc/gmu@3d68000"
SMMU = "/soc/iommu@3da0000"
GPUCC = "/soc/clock-controller@3d90000"
AOSS_QMP = "/soc/power-management@c300000"
APPS_SMMU = "/soc/iommu@15000000"
UFS = "/soc/ufshc@1d84000"
GPU_OPP = GPU + "/opp-table"
GMU_OPP = GMU + "/opp-table"
ZAP = GPU + "/zap-shader"
CMD_DB = "/reserved-memory/aop-cmd-db@81c60000"
GPU_MICROCODE = "/reserved-memory/gpu-microcode@9b09a000"


def run(*args):
    return subprocess.run(args, check=True, text=True,
                          stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT).stdout


def prop(node, name, kind="x"):
    return run("fdtget", "-t", kind, str(DTB), node, name).strip().split()


def prop_str(node, name):
    return run("fdtget", "-t", "s", str(DTB), node, name).strip()


def cells(node, name):
    return [int(word, 16) for word in prop(node, name)]


def require(value, message):
    if not value:
        raise SystemExit(message)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    run("dtc", "-I", "dts", "-O", "dtb", "-o", str(DTB), str(DTS))

    gpu_compat = prop_str(GPU, "compatible")
    gmu_compat = prop_str(GMU, "compatible")
    smmu_compat = prop_str(SMMU, "compatible")
    require(gpu_compat == "qcom,adreno-43050a01 qcom,adreno",
            "fuxi GPU compatible differs")
    require(gmu_compat == "qcom,adreno-gmu-740.1 qcom,adreno-gmu",
            "fuxi GMU compatible differs")
    require("arm,mmu-500" in smmu_compat and
            "qcom,sm8550-smmu-500" in smmu_compat,
            "fuxi GPU SMMU compatible differs")
    require(prop_str(GPU, "status") == "okay" and
            prop_str(GMU, "status") == "okay" and
            prop_str(SMMU, "status") == "okay",
            "fuxi GPU/GMU/SMMU must be enabled")

    require(cells(GPU, "reg") ==
            [0, 0x3D00000, 0, 0x40000,
             0, 0x3D9E000, 0, 0x1000,
             0, 0x3D61000, 0, 0x800],
            "fuxi GPU reg differs")
    require(cells(GMU, "reg") ==
            [0, 0x3D68000, 0, 0x37000,
             0, 0x3D50000, 0, 0x10000,
             0, 0xB280000, 0, 0x10000],
            "fuxi GMU reg differs")
    require(cells(SMMU, "reg") == [0, 0x3DA0000, 0, 0x40000],
            "fuxi GPU SMMU reg differs")

    smmu_phandle = cells(SMMU, "phandle")[0]
    gpucc_phandle = cells(GPUCC, "phandle")[0]
    aoss_qmp_phandle = cells(AOSS_QMP, "phandle")[0]
    apps_smmu_phandle = cells(APPS_SMMU, "phandle")[0]
    gmu_phandle = cells(GMU, "phandle")[0]
    require(cells(GPU, "iommus") == [smmu_phandle, 0, 0,
                                     smmu_phandle, 1, 0],
            "fuxi GPU streams must be SIDs 0 and 1")
    require(cells(GMU, "iommus") == [smmu_phandle, 5, 0],
            "fuxi GMU stream must be SID 5")
    require(cells(UFS, "iommus") == [apps_smmu_phandle, 0x60, 0],
            "fuxi UFS stream must be apps_smmu SID 0x60")

    require(cells(GPU, "operating-points-v2") ==
            cells(GPU_OPP, "phandle"),
            "fuxi GPU OPP phandle binding differs")
    require(cells(GPU, "qcom,gmu") == [gmu_phandle],
            "fuxi GPU must bind its GMU")
    require(cells(GMU, "operating-points-v2") ==
            cells(GMU_OPP, "phandle"),
            "fuxi GMU OPP phandle binding differs")
    require(cells(GMU, "power-domains") ==
            [gpucc_phandle, 0, gpucc_phandle, 1],
            "fuxi GMU power-domains must be GPU_CC CX/GX")
    require(cells(GMU, "qcom,qmp") == [aoss_qmp_phandle],
            "fuxi GMU must bind aoss_qmp")
    require(cells(SMMU, "power-domains") == [gpucc_phandle, 0],
            "fuxi GPU SMMU power-domain must be GPU_CC CX")
    require(cells(SMMU, "#global-interrupts") == [1],
            "fuxi GPU SMMU global-interrupt count differs")

    gpu_opps = run("fdtget", "-l", str(DTB), GPU_OPP).split()
    require(len(gpu_opps) == 9,
            "fuxi GPU OPP table must have 9 enabled entries")
    gmu_opps = run("fdtget", "-l", str(DTB), GMU_OPP).split()
    require(len(gmu_opps) == 2,
            "fuxi GMU OPP table must have 2 enabled entries")

    require(prop_str(ZAP, "firmware-name") ==
            "qcom/sm8550/fuxi/a740_zap.mbn",
            "fuxi ZAP firmware path differs")
    require(cells(ZAP, "memory-region") == cells(GPU_MICROCODE, "phandle"),
            "fuxi ZAP memory-region must bind gpu-microcode")
    require(cells(GPU_MICROCODE, "reg") == [0, 0x9B09A000, 0, 0x2000],
            "fuxi gpu-microcode reservation differs")
    require(prop_str(CMD_DB, "compatible") == "qcom,cmd-db" and
            cells(CMD_DB, "reg") == [0, 0x81C60000, 0, 0x20000],
            "fuxi command-db reservation differs")

    # dma-coherent is an empty property: fdtget reports its name but no value.
    require("dma-coherent" in run("fdtget", "-p", str(DTB), SMMU).split(),
            "fuxi GPU SMMU must be dma-coherent")

    resources = {
        "gpu": dict(node=GPU, compatible=gpu_compat,
                    reg=cells(GPU, "reg"), streams=[0, 1]),
        "gmu": dict(node=GMU, compatible=gmu_compat,
                    reg=cells(GMU, "reg"), stream=5),
        "smmu": dict(node=SMMU, compatible=smmu_compat,
                     reg=cells(SMMU, "reg")),
    }
    (OUT / "fuxi-platform.json").write_text(json.dumps(dict(
        compiled_dtb_sha256=hashlib.sha256(DTB.read_bytes()).hexdigest(),
        resources=resources,
        zap_reserved_bytes=0x2000,
        command_db_reserved_bytes=0x20000,
        static_only=True,
        hardware_registers_observed=False,
        gmu_started=False,
        gpu_submission_qualified=False,
        metal_qualified=False), indent=2) + "\n")
    print("PASS: pinned fuxi A740 identity, GPU/GMU/SMMU resources, "
          "stream bindings, ZAP and command-db reservations; "
          "source evidence only")


if __name__ == "__main__":
    main()
