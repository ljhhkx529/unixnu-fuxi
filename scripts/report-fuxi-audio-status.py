#!/usr/bin/env python3
"""Run every audio check and print the status table this plan's section 9.1 shows.

The plan's matrix is only worth anything if its numbers come from running the suites, so this regenerates them:
one line per suite with the checks it reported, then the two gates, then the total. A suite that fails is shown
as FAIL and makes the whole report fail.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

SUITES = [
    ("安全固件容器", "scripts/check-qcom-mdt-host.py", "QcomMdt"),
    ("SMEM / SMP2P / GLINK", "scripts/check-qcom-a0-host.py", "QcomA0"),
    ("QMP / RPMh", "scripts/check-qcom-loader-host.py", "QcomLoader"),
    ("ADSP 加载顺序", "scripts/check-qcom-adsp-host.py", "QcomAdsp"),
    ("GPR / APM 编解码", "scripts/check-qcom-gpr-host.py", "QcomGpr"),
    ("AudioReach 载荷与应答", "scripts/check-qcom-audioreach-host.py", "QcomAudioReach"),
    ("静音数据面", "scripts/check-qcom-audio-path-host.py", "QcomAudioPath"),
    ("speaker 拓扑", "scripts/check-qcom-audio-topology-host.py", "QcomAudioTopology"),
    ("共享输出环", "scripts/check-qcom-audio-ring-host.py", "QcomAudioRing"),
    ("CS35L41 安全状态机", "scripts/check-cirrus-cs35l41-host.py", "CirrusCs35l41"),
    ("HAL 共享 ABI", "scripts/check-qcom-hal-abi-host.py", "QcomHalAbi"),
    ("GENI I2C 传输", "scripts/check-qcom-geni-i2c-host.py", "QcomGeniI2c"),
    ("采集统计", "scripts/check-qcom-audio-capture-host.py", "QcomAudioCapture"),
    ("恢复顺序（端到端）", "scripts/check-qcom-audio-session-host.py", "QcomAudioSession"),
]


def main() -> int:
    total = 0
    failures = 0
    print("| 对象 | Host 套件 | 项数 |")
    print("| --- | --- | --- |")
    for label, script, marker in SUITES:
        result = subprocess.run([sys.executable, str(ROOT / script)], capture_output=True, text=True)
        match = re.search(rf"PASS {marker}: (\d+) checks", result.stdout)
        if result.returncode == 0 and match:
            count = int(match.group(1))
            total += count
            print(f"| {label} | `{Path(script).name}` | {count} |")
        else:
            failures += 1
            print(f"| {label} | `{Path(script).name}` | FAIL |")

    for label, script in (("绑定", "scripts/check-fuxi-audio-binding.py"),
                          ("事实", "scripts/check-fuxi-audio-facts.py")):
        result = subprocess.run([sys.executable, str(ROOT / script)], capture_output=True, text=True)
        line = result.stdout.strip().splitlines()[-1] if result.stdout.strip() else "no output"
        if result.returncode != 0:
            failures += 1
        print(f"| {label} | `{Path(script).name}` | {'FAIL' if result.returncode else line} |")

    print()
    print(f"{len(SUITES)} 个套件，{total} 项主机检查，{failures} 处失败")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
