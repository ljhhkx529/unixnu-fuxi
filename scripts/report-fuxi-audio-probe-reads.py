#!/usr/bin/env python3
"""Turn the binding's unsourced fields into a list of reads for a device session.

Ninety-odd fields in facts/audio/binding.toml have no source yet: the amplitude of the whole device phase. This groups
them by the read that would settle each one - a register, an SMEM item, a mailbox descriptor, a clock - and says which
fields that read fills. The point is that a session with the phone becomes a checklist rather than an improvisation, and
that a read which cannot be taken without a bring-up first is labelled as such instead of being counted as available.
"""

from __future__ import annotations

import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINDING = ROOT / "facts/audio/binding.toml"


def unsourced(binding: dict) -> dict[str, list[str]]:
    """Every entry that is not verified, grouped by its dotted table path."""
    groups: dict[str, list[str]] = {}

    def walk(table: dict, prefix: str) -> None:
        for name, entry in table.items():
            if not isinstance(entry, dict):
                continue
            path = f"{prefix}.{name}" if prefix else name
            if {"value", "state", "evidence"} <= set(entry):
                if entry.get("state") != "verified":
                    groups.setdefault(prefix, []).append(name)
            else:
                walk(entry, path)

    walk(binding, "")
    return groups


READS = [
    ("amplifier", "两颗粒的 chip id 与 revision", "I2C 0x40 / 0x42，寄存器 0x00 与 0x04（当前假设的 8 位寻址，先只读）",
     ["amp.registers", "amp.b", "amp.t"], "否：只需要 I2C 控制器可用"),
    ("amplifier", "OTP trim 与 boost 能力", "同一器件，OTP 区读回", ["amp.registers"], "否"),
    ("amplifier", "中断状态初值（上电后、配置前）", "同一器件，EINT1/EINT2 只读", ["amp.registers"], "否"),
    ("adsp", "SMEM in/out/crash 项是否可读", "SMEM 表项 443 / 429 / 423", ["adsp.entries", "adsp.values"], "否：只读 SMEM 头"),
    ("adsp", "SMP2P 的 entry 名与位布局", "SMEM 项 429/443 内的字符串与位", ["adsp.entries", "adsp.values"], "否：只读 SMEM 头"),
    ("adsp", "AOSS 邮箱描述符", "aoss 窗口内 QMP 描述符（magic、mailbox 偏移与大小）", ["adsp.timeouts"], "否：只读窗口"),
    ("adsp", "carveout 是否落在可用 DRAM", "SMEM 的内存表与 /memory 节点", ["adsp.carveout"], "否：只读表项"),
    ("adsp", "GLINK 通道名", "SMEM 项 478-480 的描述符字符串", ["adsp.entries"], "否"),
    ("path", "GPR 端口号与 APM 实例号", "来自 DSP 的 SPF 应答（需要先起 DSP）", ["path", "path.graph"], "是：需要 ADSP 起来"),
    ("path", "拓扑实例号与接口词", "APM 的模块列表应答（需要开图）", ["path.graph", "path.endpoint", "path.topology"], "是：需要开图"),
    ("path", "缓冲地址与 map handle 的真实回值", "MEM_MAP 应答（需要开图）", ["path"], "是"),
    ("abi", "host clock 频率", "内核里 CoreAudio 的常数地址，或 mach_absolute_time 的换算", ["abi"], "是：需要内核运行"),
    ("abi", "环与页的几何", "由速率与周期推导，不是读出来的", ["abi", "path.ring"], "否：是选择，不是测量"),
    ("path", "格式是否被端点接受", "SET_CFG 的应答（端点拒绝时给出错误码）", ["path.format"], "是：需要开图"),
    ("capture", "WCD938x 的枚举与目标麦的 slot", "SoundWire 枚举应答（需要 TX macro 上电）", ["capture"], "是"),
]

# The fields each group actually covers, so the report cannot claim more than the binding has.
def main() -> int:
    binding = tomllib.loads(BINDING.read_text())
    groups = unsourced(binding)
    total = sum(len(v) for v in groups.values())

    print("| 组 | 读什么 | 在哪里 | 填哪些表 | 是否需要先 bring-up |")
    print("| --- | --- | --- | --- | --- |")
    covered: set[str] = set()
    for group, what, where, tables, needs in READS:
        names = ", ".join(sorted({f"`{t}`" for t in tables if t in groups}))
        for t in tables:
            covered.add(t)
        print(f"| {group} | {what} | {where} | {names} | {needs} |")

    structural = {"path.references"}
    leftover = {t: v for t, v in groups.items() if t not in covered and t not in structural}
    print()
    print(f"绑定里 {total} 个字段未验证，分布在 {len(groups)} 张表；上表覆盖 {len(covered & set(groups))} 张。")
    for table in sorted(structural & set(groups)):
        print(f"  结构字段，由代码填充而非读数：{table} ({len(groups[table])} 个字段)")
    for table, names in sorted(leftover.items()):
        print(f"  未被任何读数覆盖：{table} ({len(names)} 个字段)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
