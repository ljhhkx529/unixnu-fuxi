#!/usr/bin/env python3
"""Shared pinned-input plumbing for fuxi Adreno prepare scripts."""
import base64
import hashlib
import json
import os
import subprocess
import sys
import urllib.request
from pathlib import Path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def find_dtb(root, explicit=None):
    """Resolve the fuxi compiled device tree: explicit arg, env, then the
    standard sibling checkout layout around this build tree."""
    if explicit:
        path = Path(explicit).expanduser()
        if path.is_file():
            return path.resolve()
        raise SystemExit(f'fuxi.dtb not found: {path}')
    for name in ('FUXI_DTB', 'UNIXNU_DTB'):
        value = os.environ.get(name)
        if value:
            path = Path(value).expanduser()
            if path.is_file():
                return path.resolve()
    candidates = [
        root.parents[3] / 'uefi-fuxi' / 'Resources' / 'DTBs' / 'fuxi.dtb',
        root.parents[2] / 'uefi-fuxi' / 'Resources' / 'DTBs' / 'fuxi.dtb',
        Path(os.getcwd()).parents[3] / 'uefi-fuxi' / 'Resources' / 'DTBs' / 'fuxi.dtb',
    ]
    for path in candidates:
        if path.is_file():
            return path.resolve()
    raise SystemExit('no fuxi.dtb: pass --dtb FILE or set FUXI_DTB/UNIXNU_DTB')


def fdt_prop(dtb, node, prop, kind='x'):
    return subprocess.check_output(
        ['fdtget', '-t', kind, str(dtb), node, prop], text=True).split()


def fdt_prop_str(dtb, node, prop):
    return subprocess.check_output(
        ['fdtget', '-t', 's', str(dtb), node, prop], text=True).strip()


def fdt_cells(dtb, node, prop):
    return [int(word, 16) for word in fdt_prop(dtb, node, prop)]


def fdt_ranges(dtb, node):
    values = fdt_cells(dtb, node, 'reg')
    if len(values) % 2:
        raise SystemExit(f'{node}: reg is not 32-bit address/size pairs')
    return [dict(base=values[i], bytes=values[i + 1])
            for i in range(0, len(values), 2)]


def combined_windows(dtb, gpu_node, gmu_node):
    ranges = {}
    for node in (gpu_node, gmu_node):
        names = fdt_prop(dtb, node, 'reg-names', 's')
        regions = fdt_ranges(dtb, node)
        if len(names) != len(regions):
            raise SystemExit(f'{node}: reg-names/reg cardinality differs')
        for name, region in zip(names, regions):
            if name in ranges and ranges[name] != region:
                raise SystemExit(f'{node}: duplicate window {name}')
            ranges[name] = region
    return ranges


def ensure_adreno_cache(root, cache):
    """Fetch the SHA-pinned Linux/firmware inputs if any required file is absent."""
    required = [
        'drivers/gpu/drm/msm/adreno/a6xx_gmu.c',
        'drivers/gpu/drm/msm/adreno/a6xx_gpu.c',
        'drivers/gpu/drm/msm/adreno/a6xx_catalog.c',
        'drivers/gpu/drm/msm/registers/adreno/a6xx.xml',
        'drivers/gpu/drm/msm/registers/adreno/a6xx_gmu.xml',
        'drivers/soc/qcom/cmd-db.c',
        'include/soc/qcom/cmd-db.h',
    ]
    if all((cache / name).is_file() for name in required):
        return
    fetcher = root / 'scripts' / 'fetch-adreno-inputs.py'
    if not fetcher.is_file():
        raise SystemExit('missing adreno source cache and no scripts/fetch-adreno-inputs.py')
    print('fuxi-adreno: fetching pinned Adreno source/firmware inputs')
    subprocess.run([sys.executable, fetcher], cwd=root, check=True)


def load_pins(root):
    path = root / 'scripts' / 'data' / 'adreno-inputs.json'
    if not path.is_file():
        raise SystemExit(f'missing {path}')
    return json.loads(path.read_text())


def verify_source(cache, pins, names):
    for name in names:
        expected = pins['linux']['files'].get(name)
        if not expected:
            raise SystemExit(f'no pinned source entry: {name}')
        path = cache / name
        if not path.is_file() or sha(path) != expected['sha256']:
            raise SystemExit(f'pinned source differs: {name}')


def ensure_fuxi_firmware(root, local, files):
    """Download the SHA-pinned fuxi blobs from the existing fuxi firmware repo."""
    for key, expected in files.items():
        path = local / key
        if path.is_file():
            blob = path.read_bytes()
        else:
            url = expected['repository'].replace('https://github.com/', 'https://api.github.com/repos/') + '/git/blobs/' + expected['git_blob_sha1']
            request = urllib.request.Request(url, headers={
                'User-Agent': 'fuxi-offline-adreno-audit',
                'Accept': 'application/vnd.github.raw+json',
            })
            with urllib.request.urlopen(request, timeout=30) as response:
                blob = response.read()
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(blob)
        if (len(blob) != expected['bytes'] or
                hashlib.sha256(blob).hexdigest() != expected['sha256'] or
                hashlib.sha1(b'blob ' + str(len(blob)).encode() + b'\0' + blob).hexdigest() != expected['git_blob_sha1']):
            raise SystemExit('pinned fuxi firmware differs: ' + key)
