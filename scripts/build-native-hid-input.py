#!/usr/bin/env python3
"""Build fuxi HID link stubs.

fuxi has not produced the original v9 IOHIDFamily prelink candidate required by
the core build-native-hid-input.py audit, so the display archive links against
these no-op allocators instead. The legacy display backend does not depend on
an owned HID device.
"""
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/native-hid-input'
OUT.mkdir(parents=True, exist_ok=True)

sdk = subprocess.check_output(
    ['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True, timeout=30
).strip()

stub = ROOT / 'kits/sheng/HidInputStub.c'
stub_obj = OUT / 'NativeHidInput.late.o'
empty_asm = OUT / 'empty.S'
empty_obj = OUT / 'hid-original-bindings.o'

subprocess.run([
    'xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-Werror',
    '-isysroot', sdk,
    '-c', str(stub), '-o', str(stub_obj),
], check=True, timeout=90)

empty_asm.write_text('.section __TEXT,__text\n')
subprocess.run([
    'xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-Werror',
    '-isysroot', sdk,
    '-c', str(empty_asm), '-o', str(empty_obj),
], check=True, timeout=90)

print(f'fuxi HID stub: {stub_obj}')
