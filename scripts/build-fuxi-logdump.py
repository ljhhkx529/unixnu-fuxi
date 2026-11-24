#!/usr/bin/env python3
"""Build the telephony log daemon: the unified log's telephony entries on the serial console.

usage: build-fuxi-logdump.py OUTDIR [TARGET]

The daemon reads the system log through OSLogStore (the logging entitlements the probe daemon also carries),
keeps the entries whose process, subsystem or category names the cellular side, and prints them to stdout; its
launchd job hands stdout to /dev/console, which the captured boot log already carries. Signed ad hoc for the
owner trust module, like the board's other userspace binaries.
"""
import json
import plistlib
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "package"
if len(sys.argv) not in (2, 3):
    sys.exit(__doc__)
out = Path(sys.argv[1]).resolve()
target = sys.argv[2] if len(sys.argv) == 3 else "arm64-apple-ios17.0"
out.mkdir(parents=True, exist_ok=True)
ios = subprocess.check_output(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"], text=True).strip()
entitlements = out / "fuxi-logdump-entitlements.plist"
entitlements.write_bytes(plistlib.dumps({
    "com.apple.private.security.no-container": True,
    "com.apple.private.logging.admin": True,
    "com.apple.private.logging.diagnostic": True,
    "com.apple.private.logging.stream": True,
}))
binary = out / "fuxi-logdump"
subprocess.run(["xcrun", "clang", "-target", target, "-isysroot", ios, "-O2", "-g0", "-Wall", "-Wextra", "-Werror",
                "-fobjc-arc", str(PACKAGE / "fuxi-logdump.m"),
                "-framework", "Foundation", "-lz", "-o", str(binary)], check=True)
subprocess.run(["codesign", "--force", "--sign", "-", "--pagesize", "4096", "--identifier",
                "org.unixnu.fuxi.logdump", "--entitlements", str(entitlements), str(binary)], check=True)
subprocess.run(["codesign", "--verify", "--strict", str(binary)], check=True)
detail = subprocess.run(["codesign", "--display", "--verbose=4", str(binary)], capture_output=True, text=True,
                        check=True).stderr
assert re.search(r"^Page size=4096$", detail, re.M), "the signature must use 4 KiB pages"
cdhash = re.search(r"^CDHash=([0-9a-f]{40})$", detail, re.M).group(1)
(out / "trust.json").write_text(json.dumps({"cdhash": cdhash}, indent=2) + "\n")
print(f"{binary} CDHash {cdhash}")
