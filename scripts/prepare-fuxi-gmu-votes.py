#!/usr/bin/env python3
"""Derive fuxi GPU/GMU OPPs, BCM policy and command-db reservation."""
import argparse
import json
import re
from pathlib import Path

from fuxi_adreno_common import (ensure_adreno_cache, find_dtb, fdt_cells,
                                fdt_prop, fdt_prop_str, load_pins, sha,
                                verify_source)

GPU_NODE = '/soc/qcom,kgsl-3d0@3d00000'
GMU_NODE = '/soc/qcom,gmu@3d69000'
PWR_BIN = GPU_NODE + '/qcom,gpu-pwrlevel-bins/qcom,gpu-pwrlevels-0'
CATALOG = 'drivers/gpu/drm/msm/adreno/a6xx_catalog.c'


def prepare():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dtb', help='compiled fuxi device tree')
    parser.add_argument('--root', default=Path(__file__).resolve().parents[1], type=Path)
    parser.add_argument('--cache', default=None, type=Path)
    parser.add_argument('--inputs', default=None, type=Path)
    parser.add_argument('--out', default=None, type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    cache = (args.cache or root / '.cache' / 'adreno-source').resolve()
    inputs = args.inputs or root / 'scripts' / 'data' / 'adreno-inputs.json'
    out = (args.out or root / 'build' / 'fuxi-gmu').resolve()
    dtb = find_dtb(root, args.dtb)

    pins = json.loads(inputs.read_text())
    ensure_adreno_cache(root, cache)
    verify_source(cache, pins, [CATALOG])

    if fdt_prop(dtb, GPU_NODE, 'compatible', 's')[0] != 'qcom,adreno-gpu-gen7-2-1':
        raise SystemExit('fuxi GPU identity differs')

    bus_table = fdt_cells(dtb, GPU_NODE, 'qcom,bus-table-ddr')
    if not bus_table or len(bus_table) > 16 or len(set(bus_table)) != len(bus_table):
        raise SystemExit('fuxi DDR bus table is not a deduplicated bounded table')
    if any(value < 0 or value > 0xffffffff for value in bus_table):
        raise SystemExit('fuxi DDR bus table value is unrepresentable')

    children = []
    try:
        levels = subprocess_list(dtb, PWR_BIN)
    except Exception:
        levels = []
    if not levels:
        raise SystemExit('fuxi has no enabled GPU pwrlevel bin')
    records = []
    for child in levels:
        path = PWR_BIN + '/' + child
        hz = one(dtb, path, 'qcom,gpu-freq')
        level = one(dtb, path, 'qcom,level')
        acd = one(dtb, path, 'qcom,acd-level')
        bus_index = one(dtb, path, 'qcom,bus-freq')
        if not hz or hz > 0xffffffff or not 0 < level <= 65535:
            raise SystemExit('fuxi GPU OPP is unrepresentable')
        if bus_index >= len(bus_table):
            raise SystemExit('fuxi GPU OPP bus-freq exceeds the DDR table')
        records.append(dict(hz=hz, level=level, kbps=bus_table[bus_index],
                            acd_present=1, acd=acd))
    records.sort(key=lambda entry: entry['hz'])
    if len(records) + 1 > 16 or len({entry['hz'] for entry in records}) != len(records):
        raise SystemExit('fuxi GPU OPP count/order is invalid')
    if any(records[index]['level'] < records[index - 1]['level']
           for index in range(1, len(records))):
        raise SystemExit('fuxi GPU OPP levels are nonmonotonic')
    gpu_records = [dict(hz=0, level=0, kbps=0, acd_present=0, acd=0)] + records

    gmu_freq = fdt_cells(dtb, GMU_NODE, 'qcom,gmu-freq-table')
    if len(gmu_freq) % 2 or not gmu_freq:
        raise SystemExit('fuxi GMU frequency table is not flat pairs')
    gmu_records = [dict(hz=0, level=0, kbps=0, acd_present=0, acd=0)]
    for index in range(0, len(gmu_freq), 2):
        hz, level = gmu_freq[index], gmu_freq[index + 1]
        if not hz or hz > 0xffffffff or not 0 < level <= 65535:
            raise SystemExit('fuxi GMU OPP is unrepresentable')
        gmu_records.append(dict(hz=hz, level=level, kbps=0,
                                acd_present=0, acd=0))
    gmu_records.sort(key=lambda entry: entry['hz'])
    if len(gmu_records) > 4 or len({entry['hz'] for entry in gmu_records}) != len(gmu_records):
        raise SystemExit('fuxi GMU OPP count/order is invalid')

    catalog = (cache / CATALOG).read_text()
    record = re.search(r'\.chip_ids = ADRENO_CHIP_IDS\(0x43050a01\)([\s\S]*?)\.preempt_record_size', catalog)
    entries = re.findall(r'\{\s*\.name = "([A-Z0-9]+)",([^{}]+)\}', record[1])
    if [entry[0] for entry in entries] != ['SH0', 'MC0', 'ACV']:
        raise SystemExit('fuxi A740 BCM order mismatch')
    policies = []
    for name, body in entries:
        if name != 'ACV':
            match = re.fullmatch(r'\s*\.buswidth = (\d+)\s*', body)
            if not match:
                raise SystemExit('Unrecognized proportional BCM policy')
            policies.append(dict(name=name, buswidth=int(match[1]), fixed=0,
                                 perfmode=0, perfmode_kbps=0))
        else:
            match = re.fullmatch(r'\s*\.fixed = true,\s*\.perfmode = BIT\((\d+)\),\s*\.perfmode_bw = (\d+),\s*', body)
            if not match or int(match[1]) >= 14:
                raise SystemExit('Unrecognized fixed BCM policy')
            policies.append(dict(name=name, buswidth=0, fixed=1,
                                 perfmode=1 << int(match[1]),
                                 perfmode_kbps=int(match[2])))

    bandwidths = sorted({entry['kbps'] for entry in gpu_records})

    db_node = '/reserved-memory/aop_cmd_db_region@81c60000'
    if fdt_prop(dtb, db_node, 'compatible', 's') != ['qcom,cmd-db']:
        raise SystemExit('fuxi command-db binding differs')
    values = fdt_cells(dtb, db_node, 'reg')
    if len(values) != 4:
        raise SystemExit('fuxi command-db range format differs')
    db_base = (values[0] << 32) | values[1]
    db_bytes = (values[2] << 32) | values[3]
    if db_bytes < 144 or db_bytes > 1048576:
        raise SystemExit('fuxi command-db reservation size is unsupported')

    out.mkdir(parents=True, exist_ok=True)
    text = '/* Generated from pinned fuxi DT and A740 catalog. */\n#include <stdint.h>\n'
    text += '#define SHENG_GMU_VOTES_CHIP_ID 0x43050a01u\n'
    text += 'typedef struct {uint64_t hz;uint32_t level,kbps,acd_present,acd;} ShengGmuOpp;\n'
    for kind, records_kind in (('gpu', gpu_records), ('gmu', gmu_records)):
        text += '#define SHENG_' + kind.upper() + '_OPPS ' + str(len(records_kind)) + 'u\n'
        text += 'static const ShengGmuOpp sheng_' + kind + '_opps[]={\n'
        text += '\n'.join('{' + ','.join(str(int(entry[key])) for key in
                                           ['hz', 'level', 'kbps', 'acd_present', 'acd']) + '},'
                          for entry in records_kind) + '\n};\n'
    text += '#define SHENG_GPU_BANDWIDTHS ' + str(len(bandwidths)) + 'u\n'
    text += 'static const uint32_t sheng_gpu_bandwidths[]={' + \
        ','.join(map(str, bandwidths)) + '};\n'
    text += 'static const AdrenoHfiBcm sheng_bcm_policy[3]={\n'
    text += '\n'.join('{.buswidth=' + str(entry['buswidth']) +
                      ',.fixed=' + str(entry['fixed']) +
                      ',.perfmode=' + str(entry['perfmode']) +
                      ',.perfmode_kbps=' + str(entry['perfmode_kbps']) + '},'
                      for entry in policies) + '\n};\n'
    header = out / 'ShengGmuVotesSource.h'
    header.write_text(text)

    db_header = out / 'ShengGmuCommandDbSource.h'
    db_header.write_text('/* Pinned fuxi DT reservation, not observed hardware memory. */\n'
                         '#define SHENG_GMU_COMMAND_DB_BASE UINT64_C(' + hex(db_base) + ')\n'
                         '#define SHENG_GMU_COMMAND_DB_BYTES ' + str(db_bytes) + 'u\n')

    report = dict(linux_revision=pins['linux']['commit'], dtb_sha256=sha(dtb),
                  command_db_base=db_base, command_db_bytes=db_bytes,
                  command_db_header_sha256=sha(db_header), opps=dict(
                      gpu=gpu_records, gmu=gmu_records),
                  bandwidths_kbps=bandwidths, bcms=policies,
                  generated_header_sha256=sha(header),
                  actual_command_db_available=False, hardware_ready=False)
    (out / 'fuxi-gmu-votes-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: pinned fuxi GPU/GMU OPPs, DDR bandwidths, ACD and A740 BCM policy; actual command-db remains a required input')
    return out, report


def subprocess_list(dtb, node):
    import subprocess
    return subprocess.check_output(['fdtget', '-l', str(dtb), node], text=True).split()


def one(dtb, node, prop):
    values = fdt_cells(dtb, node, prop)
    if len(values) != 1:
        raise SystemExit(f'Expected one cell {prop} at {node}')
    return values[0]


if __name__ == '__main__':
    prepare()
