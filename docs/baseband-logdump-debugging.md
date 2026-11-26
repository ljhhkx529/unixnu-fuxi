# 基带 logdump 调试神技

本文是 fuxi 移植里**用 logdump 调试基带**的操作手册：怎么加探针、怎么把新 daemon 刷进系统、怎么在串口日志里读到它、以及这轮踩过的坑（尤其是“eSIM 事件”）。

---

## 0. 一分钟看懂链路

```
fuxi 用户态（logdump daemon，root，~20 s 启动）
   │ 写 /private/var/db/fuxi-logdump.txt（unbuffered）
   ▼
内核 fuxi_logdump_read()（150/210/270 s 各读一次）
   │ print 原始行到 kernel console
   ▼
USB CDC-ACM 串口 → collect-xnu-usb.py → /tmp/<run>/xnu-usb.log
```

要点：

- daemon 的 **printf 输出到 stdout（/dev/console）不会被采集**；只有**写文件**的内容会被内核回传。
- 所以每个探针都要 `fprintf(file, ...)`，并且 `fflush`。
- 内核读日志有 24000 字节上限；文件过大时只会看到前一段。

---

## 1. 部署新版 logdump 的五步

### 1.1 改 daemon 并编译

```bash
cd unixnu-fuxi
edited package/fuxi-logdump.m
python3 scripts/build-fuxi-logdump.py /tmp/logdump-new
codesign -dvvvv /tmp/logdump-new/fuxi-logdump 2>&1 | grep CandidateCDHash
```

记下新的 **sha256 cdhash**（例如 `b465525b2a2a9e014ba7ffe34d719a55e78b619d`）。

### 1.2 重打 GPU addon

addon 是 `NGPUADD2` 格式（24 字节头 + 每个条目 `plen/mode/size/packed` 24 字节 + 名字/压缩体，均 8 字节对齐）。把 `baseband/fuxi-logdump`、`bin/fuxi-logdump`、`bin/fuxi-logdump4` 三个候选名里的内容换成新二进制、mode 设 0o755：

```python
import struct, zlib
data = open('/tmp/gpu+whome10.ngpu','rb').read()
count = struct.unpack_from('<I', data, 8)[0]; at = 24; files = []
for i in range(count):
    plen, mode, size, packed = struct.unpack_from('<IIQQ', data, at); at += 24
    name = data[at:at+plen-1].decode(); at += (plen+7) & ~7
    body = zlib.decompress(data[at:at+packed]); at += (packed+7) & ~7
    files.append([name, mode, body])
new = open('/tmp/logdump-new/fuxi-logdump','rb').read()
for f in files:
    if f[0] in ('baseband/fuxi-logdump','bin/fuxi-logdump','bin/fuxi-logdump4'):
        f[2], f[1] = new, 0o755
out = b''
for name, mode, body in files:
    raw = name.encode() + b'\0'; packed = zlib.compress(body, 9)
    out += struct.pack('<IIQQ', len(raw), mode, len(body), len(packed))
    out += raw + b'\0' * ((-len(raw)) % 8) + packed + b'\0' * ((-len(packed)) % 8)
open('/tmp/gpu+whome11.ngpu','wb').write(b'NGPUADD2' + struct.pack('<IIQ', len(files), 0, 24+len(out)) + out)
```

### 1.3 扩展 trust cache bundle

格式：`u32 version(3) + u32 module0_start,module1_start,module2_start + 三个 module`；module = `u32 type(2) + uuid16 + u32 count + count×(sha256 20B + u32 status(2))`。追加新 cdhash（20 字节），去掉旧的 logdump cdhash：

```python
import struct, uuid
d = open('/tmp/fuxi-trust.bundle','rb').read()
_, s0, s1, s2 = struct.unpack_from('<4I', d)
own, orig1, orig2 = d[s0:s1], d[s1:s2], d[s2:]
n = struct.unpack_from('<I', own, 20)[0]
hashes = [own[24+24*i:24+24*i+20] for i in range(n)]
for drop in OLD_CDHASHES: hashes = [h for h in hashes if h.hex() != drop]
hashes.append(bytes.fromhex(NEW_CDHASH))
hashes = sorted(set(hashes))
ident = uuid.uuid5(uuid.NAMESPACE_URL, 'org.blu-sharky.adreno-metal:' + ''.join(h.hex() for h in hashes))
module = struct.pack('<I16sI', 2, ident.bytes, len(hashes)) + b''.join(h + bytes([2,0,0,0]) for h in hashes)
mods = [module, orig1, orig2]; cur, starts = 16, []
for m in mods: starts.append(cur); cur += len(m)
open('/tmp/fuxi-trustN.bundle','wb').write(struct.pack('<4I', 3, *starts) + b''.join(mods))
```

### 1.4 重打 payload + bootimg

```bash
cd UniXNU
export PATH=$PY:/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
./unixnu build payload -D ../unixnu-fuxi --uefi ../uefi-fuxi --unique-chip-id 0x0123456789abcdef \
  --owner-trust-cache /tmp/fuxi-trustN.bundle \
  --data-ramdisk out/fuxi/build/owner-data-fixture/owner-data-16g.dbr \
  --gpu-addon /tmp/gpu+whomeN.ngpu
./unixnu bootimg -D ../unixnu-fuxi --uefi ../uefi-fuxi --target DEBUG --fd /tmp/fuxi-firmware-good.fd \
  --dtb ../uefi-fuxi/Platforms/Xiaomi/fuxiPkg/FdtBlob/sm8550-xiaomi-fuxi.dtb -o /tmp/fuxi-xxx.img
```

### 1.5 刷入并采集

```bash
# 回 fastboot（需设备上日志 daemon 的 ctl 会话；超时常见，重试即可）
unixnu-fuxi/scripts/fuxi-usb-debug.sh ctl-one reboot-fastboot

# 引导
/opt/homebrew/bin/fastboot boot /tmp/fuxi-xxx.img

# 采集
python3 -B drivers/QcomXnuUsbDebug/scripts/collect-xnu-usb.py \
  --serial FuxiXnuUsbDebug --output /tmp/fuxi-xxx --wait 120 --seconds 280 --no-marker
```

---

## 2. 探针目录（这轮已经加进 daemon/内核的）

### 2.1 内核侧（桥）追踪

| 探针 | 输出 | 用途 |
|---|---|---|
| AppleBaseband selector 追踪 | `COACHZ board: sel N in ... out ... res ... mgr X bb Y` | 看 ABM 的 BootController 走到哪、manager/baseband 状态 |
| sahara READ_DATA 追踪 | `COACHZ sahara: rx/tx ... img 0x15 off 0x... len 0x... seg a/b have/expect/remaining` | 看镜像传送到第几段、停在哪 |
| HELLO 应答字段 | `hello status ... mode ... w8 ... w12 ...` | 看主机下一步要什么模式 |
| 通道首连 | `first user client on <name>` | ABM 开了哪些通道（pbl/sahara/qmux/refs/boot…） |

常用 grep：

```bash
grep -a 'COACHZ board: sel' xnu-usb.log | head -40
grep -a 'COACHZ sahara' xnu-usb.log | head -30
grep -a 'pbl register\|sendImage' xnu-usb.log
grep -a 'the radio: ' xnu-usb.log | tail -3
grep -a 'sahara state' xnu-usb.log | tail -2
```

### 2.2 用户态侧（logdump）

| 探针 | 输出前缀 | 用途 |
|---|---|---|
| MG 身份 | `# MGCopyAnswer ProductType/HWModelStr` | 确认机型身份 |
| basebandd/abm-helper | `# com.apple.basebandd.xpc REGISTERED` 等 | 服务是否注册、helper 是否运行 |
| IOKit | `# iokit AppleBaseband 1 { class = 3, compatible = "baseband,n41" }` | 服务在 IORegistry 的真实属性 |
| 蜂窝偏好 dump | `# telephony prefs at 5 paths`、`#   <path> ...` | CommCenter SIM/eSIM 配置（见坑 3） |
| CoreTelephony | `# ct CTSIMSupport...` | SIM 槽类型（见坑 4） |
| OSLog 尝试 | `# store attempt N finished/timed out` | ABM/BBU 私有日志（见坑 5） |

---

## 3. 实战配方

### 配方 A：状态栏 “No Service”

```bash
grep -a 'the radio: ' xnu-usb.log | tail -2        # 桥侧是否注册（460-0 / -dBm）
grep -a 'COACHZ board: sel 2[67]' xnu-usb.log     # ABM 是否进入 baseband state
grep -a 'first user client on' xnu-usb.log        # 有没有 boot/refs 通道
grep -a 'nas-indicate' xnu-usb.log | head -4      # 宿主 NAS 客户端数量
```

判读：

- 桥侧 `registered on 460-0` 但 UI No Service → 宿主（CommCenter/ABM）没拿到注册指示，查 NAS 客户端数（0 = 没注册客户端）。
- `board: sel 3/0/15/1` 循环、无 26/27 → ABM 没 booted，先解决 boot 流程。

### 配方 B：sahara 镜像传送失败

```bash
grep -a 'COACHZ sahara: tx' xnu-usb.log | head -12   # READ_DATA 的 offset/length 是否递进
grep -a 'sahara image read\|sahara finished\|sahara failed' xnu-usb.log
grep -a 'sahara state' xnu-usb.log | tail -1
```

- offset 不递进或 remaining 不减：检查分块/段解析。
- DONE 之后主机反复重开 HELLO：**不要**改 DONE_RESP 的 status 位（见坑 1）。

### 配方 C：eSIM/实体卡显示错误

1. 先确认桥侧分支：`grep -a 'eSIM branch' xnu-usb.log` → 应为 `off, UIM passes through`；
2. 确认 ADT 没有 eUICC：`board.toml` 的 `[adt.baseband.u32]` 不应有 `euicc-chip-id`；
3. 看 DONE_RESP 状态位：`coachz_bb_sahara.h` 必须是 `last ? 1u : 0u`（见坑 1）；
4. 仍不行再看 prefs dump（坑 3）。

---

## 4. 五个坑（血泪版）

### 坑 1：DONE_RESP 的 status 位 = eSIM 触发器

把 `cbs_put32(p + 8, last ? 0u : 1u);` 改成 `0 = 最后一张` 后，**设置里 SIM 行变成 “eSIM”**（且不可逆地进入 eSIM 状态机），恢复 `last ? 1u : 0u` 后立刻回到实体卡逻辑。

**结论：这个位不要动。**

### 坑 2：kernel command line 上限 1023 字节

`board.toml` 的 `kernel_args` 已经 1036 字节时再加 `coachz-esim=1` 会构建失败：

```
unixnu: the kernel command line is 1036 bytes; the limit is 1023
```

要加 boot-arg，先删等长参数；或临时在代码里强制（诊断完立即回滚）。

### 坑 3：logdump 采样太早读不到 CommCenter 偏好

daemon 在 ~20 s 采样时 `/private/var/preferences/com.apple.CommCenter.plist` 等还不存在（`missing`）。把偏好 dump 放在 **90–150 s** 再取，并遍历 `/private/var/mobile/Library/Preferences` 的全量文件名（大小写不敏感的关键字匹配）。

### 坑 4：CoreTelephony 私有符号在 17.7 上已消失

`CTSIMSupportCopySimSlotMapping` 等在 iOS 17.7.11 的 CoreTelephony 里 `dlsym missing`。改用公开 API：

```objc
CTTelephonyNetworkInfo *info = [[CTTelephonyNetworkInfo alloc] init];
info.serviceSubscriberCellularProviders;   // key = service id, value = CTCarrier（carrierName/MCC/MNC）
[CTSubscriberInfo subscribers];            // IMSI
```

### 坑 5：OSLogStore 在 daemon 里返回 nil

`+[OSLogStore storeWithScope:error:]` 在 daemon 里可能返回 `nil`（error 也是 nil），且**卡住整个 daemon**。处理方式：

- 先 `dlopen("/System/Library/Frameworks/OSLog.framework/OSLog")` 再取类；
- 把查询放进 **fork 出来的子进程**，父进程 `waitpid` 带超时，超时直接 `SIGKILL`，最多重试 3 次；
- 写文件的是子进程，父进程继续跑，不会把 daemon 拖死。

---

## 5. 快速命令索引

| 操作 | 命令 |
|---|---|
| 编译 daemon | `cd unixnu-fuxi && python3 scripts/build-fuxi-logdump.py /tmp/logdump-new` |
| 取 cdhash | `codesign -dvvvv /tmp/logdump-new/fuxi-logdump 2>&1 | grep CandidateCDHash` |
| 采集 | `python3 -B drivers/QcomXnuUsbDebug/scripts/collect-xnu-usb.py --serial FuxiXnuUsbDebug --output /tmp/run --wait 120 --seconds 280 --no-marker` |
| 回 fastboot | `./scripts/fuxi-usb-debug.sh ctl-one reboot-fastboot` |
| 引导 | `fastboot boot /tmp/fuxi-xxx.img` |
| 看 ABM 状态机 | `grep -a 'board: sel' /tmp/run/xnu-usb.log` |
| 看 sahara | `grep -a 'COACHZ sahara' /tmp/run/xnu-usb.log` |
| 看射频 | `grep -a 'the radio: ' /tmp/run/xnu-usb.log | tail -2` |
| 看探针 | `grep -a '# iokit\|# ct \|# telephony prefs\|# store attempt' /tmp/run/xnu-usb.log` |

---

## 6. 结论（这一轮学到的最重要一条）

**基带调试里，“日志里的现象”和“内核状态机里的因果”经常隔着一层：DONE_RESP 一个状态位就能把整个 SIM 分支从实体卡切到 eSIM。**
先加**可观测性**（selector/sahara/HELLO/IOKit/prefs），再做**单变量改动**，每改一个位都要 A/B 对比验证，别一次动多处。

---

*文件：unixnu-fuxi/docs/baseband-logdump-debugging.md*