# GPU 提交整理说明

这次整理把 GPU 收尾期写成"每测一次提交一次"的历史，压成了按语义划分的干净提交。**内容没有任何变化**：
整理前后 `git diff` 为空（唯一例外是 Adreno740 的 pin 与 gitlink 指向压缩后的新 SHA，因为子模块自己也被压缩了）。

## 三个仓库的干净序列

### UniXNU（`main`，4 个提交）

| 提交 | 内容 |
| --- | --- |
| `components: AdrenoMetal and GopFramebuffer back to official` | 两个只提供诊断的 fork 回官方提交（`6d7f734`、`e47c1b8`），含各自 gitlink |
| `Adreno740: board policy as data, and an audit-only command stream` | Adreno740 分支状态 + gitlink + `components.toml` |
| `mesa: Turnip honours the kernel's command-stream capabilities` | `patches/mesa/turnip-darwin-command-policy.patch`、`adreno-compiler-inputs.json` 登记、`check-turnip-fuxi-command-policy.py` |
| `scripts: build the updated components against this machine` | 插件桥、host 检查、translator 输入 |

每个提交的 `components.toml` revision 与其 gitlink 一致（逐提交校验过），所以任何一次 `git checkout` 都能构建。

### Adreno740（`fuxi` 分支）

| 提交 | 内容 |
| --- | --- |
| `feat(publish): board policy as data, and two command-stream capabilities` | 版本化的 publish policy 结构 + 两个能力位 |
| `Adreno740: the command-stream workarounds are gone; the runtime decides` | 删除 CP_THREAD_CONTROL 改写、`CP_WAIT_MEM_WRITES` 替换、空间接缓冲区实验、`SYNC_THREADS` 开关；只保留审计计数 |

历史上那三个 `diag(...)` 提交的内容已随这次的删除一起消失，但为不破坏既有提交链，未重写更早的历史。

### unixnu-fuxi（`V2`，计划期 12 → 3 个提交）

| 提交 | 内容 |
| --- | --- |
| `gpu: the A740 runs the default path, with nothing disabled` | `board.toml` 的 GPU 参数 + 两个探针（审计计数、停点快照、WC 审计） |
| `docs: what differs from sheng, and the plan that cleaned the port up` | `docs/fuxi-vs-sheng-gpu-gap.md`、`docs/upstream-clean-gpu-plan.md` |
| `docs: the baseband porting plan` | `docs/baseband-porting-plan.md`（与 GPU 无关，单独成提交） |

`efa08b6`（"the GPU renders and the display presents through it"）之前的 bring-up 叙述历史保持原样——它是这台机器怎么被弄亮的记录，`docs/gpu-bringup.md` 与之对应。

## 怎么复核

```sh
# 内容未变：整理前后的树相同（Adreno740 pin 除外）
git diff --stat backup/pre-clean-<MMDD> HEAD

# 每个提交都自洽：pin == gitlink
python3 - <<'PY'
import re, subprocess
sh = lambda *a: subprocess.run(a, capture_output=True, text=True).stdout
for c in sh('git', 'log', '--format=%H', '-4').split():
    txt = sh('git', 'show', c + ':components.toml')
    block = re.search(r'id = "Adreno740"(.*?)(?=\n\[\[component\]\]|\Z)', txt, re.S).group(1)
    pin = re.search(r'revision = "([0-9a-f]{40})"', block).group(1)
    link = sh('git', 'ls-tree', c, 'drivers/Adreno740').split()[2]
    print(c[:7], 'OK' if pin == link else 'MISMATCH')
PY
```

三个仓库都留了 `backup/pre-clean-<MMDD>` 分支，需要对比或回退时用它。

## 整理后的验收

- `check-turnip-fuxi-command-policy.py`：18/18 通过（含补丁序列逐字节还原 pinned 源码）
- 三个子模块工作树 clean，且 `components.toml` revision == `HEAD`
- 真机：fence 71/71 retired、GOP 23/23 done、无 panic/reset/SMMU fault
