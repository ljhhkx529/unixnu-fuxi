#!/usr/bin/env python3
"""Generate fuxi GMU register/setup inputs from pinned DT and Adreno source."""
import argparse
import ast
import hashlib
import json
import re
import xml.etree.ElementTree as ET
from pathlib import Path

from fuxi_adreno_common import (ensure_adreno_cache, find_dtb,
                                combined_windows, fdt_prop, load_pins,
                                sha, verify_source)

GPU_NODE = '/soc/qcom,kgsl-3d0@3d00000'
GMU_NODE = '/soc/qcom,gmu@3d69000'
ORDERED = ['gmu', 'rscc', 'gmu_pdc', 'kgsl_3d0_reg_memory', 'cx_misc']
SOURCES = ['drivers/gpu/drm/msm/adreno/a6xx_gmu.c',
           'drivers/gpu/drm/msm/adreno/a6xx_gmu.h',
           'drivers/gpu/drm/msm/adreno/a6xx_gpu.c',
           'drivers/gpu/drm/msm/adreno/a6xx_gpu.h',
           'drivers/gpu/drm/msm/adreno/a6xx_catalog.c',
           'drivers/gpu/drm/msm/registers/adreno/a6xx.xml',
           'drivers/gpu/drm/msm/registers/adreno/a6xx_gmu.xml']
NS = {'r': 'http://nouveau.freedesktop.org/'}


def register(trees, regs, symbol, domain, name, variant=None):
    matches = []
    for tree in trees:
        for domain_node in tree.findall('r:domain', NS):
            if domain_node.attrib['name'] != domain:
                continue
            matches += [node for node in domain_node.findall('r:reg32', NS)
                        if node.attrib.get('name') == name and
                        (variant is None or node.attrib.get('variants') == variant)]
    if len(matches) != 1:
        raise SystemExit(f'Ambiguous/missing source register {symbol}')
    regs[symbol] = int(matches[0].attrib['offset'], 0)
    return matches[0]


def bit(trees, domain, name, field):
    for tree in trees:
        for domain_node in tree.findall('r:domain', NS):
            if domain_node.attrib['name'] != domain:
                continue
            for reg in domain_node.findall('r:reg32', NS):
                if reg.attrib.get('name') != name:
                    continue
                for entry in reg.findall('r:bitfield', NS):
                    if entry.attrib.get('name') == field:
                        return 1 << int(entry.attrib['pos'])
    raise SystemExit(f'Missing actual fatal bit {field}')


def number(expression, regs):
    expression = expression.strip()
    if '?' in expression:
        if not expression.startswith('adreno_is_a740_family(adreno_gpu)'):
            raise SystemExit('Unrecognized conditional')
        expression = expression.split('?', 1)[1].split(':', 1)[0].strip()
    expression = re.sub(r'BIT\((\d+)\)', lambda m: str(1 << int(m[1])), expression)
    expression = expression.replace('seqmem0_drv0_reg',
                                    str(regs['REG_RSCC_SEQ_MEM_0_DRV0_A740']))
    for name, value in regs.items():
        original = name.replace('REG_', 'REG_A6XX_', 1)
        expression = re.sub(r'\b' + original + r'\b', str(value), expression)
    parsed = ast.parse(expression, mode='eval')
    if any(not isinstance(node, (ast.Expression, ast.Constant, ast.BinOp, ast.Add))
           for node in ast.walk(parsed)):
        raise SystemExit('Unsupported source expression ' + expression)
    return eval(compile(parsed, '<pinned-source-constant>', 'eval'),
                {'__builtins__': {}})


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
    verify_source(cache, pins, SOURCES)
    fuxi_path = root / 'scripts' / 'data' / 'fuxi-adreno-inputs.json'
    if fuxi_path.is_file():
        fuxi_pins = json.loads(fuxi_path.read_text())
        if (sha(dtb) != fuxi_pins['board'].get('dtb_sha256') or
                dtb.stat().st_size != fuxi_pins['board'].get('dtb_bytes')):
            raise SystemExit('fuxi.dtb differs from pinned board input')
        if fuxi_pins['board'].get('gpu_node') != GPU_NODE:
            raise SystemExit('fuxi GPU node path differs')
        if fuxi_pins['board'].get('gmu_node') != GMU_NODE:
            raise SystemExit('fuxi GMU node path differs')
    if fdt_prop(dtb, GPU_NODE, 'compatible', 's')[0] != 'qcom,adreno-gpu-gen7-2-1':
        raise SystemExit('fuxi GPU identity differs')
    if fdt_prop(dtb, GMU_NODE, 'compatible', 's')[0] != 'qcom,gen7-gmu':
        raise SystemExit('fuxi GMU identity differs')

    windows_by_name = combined_windows(dtb, GPU_NODE, GMU_NODE)
    windows = [windows_by_name[name] for name in ORDERED]

    gpu_source = (cache / 'drivers/gpu/drm/msm/adreno/a6xx_gpu.c').read_text()
    if 'cx_misc_mmio = msm_ioremap(pdev, "cx_mem")' not in gpu_source:
        raise SystemExit('Actual CX binding differs')
    trees = [ET.parse(cache / ('drivers/gpu/drm/msm/registers/adreno/' + name)).getroot()
             for name in ('a6xx.xml', 'a6xx_gmu.xml')]
    regs = {}
    for name in ['GMU_CM3_ITCM_START', 'GMU_CM3_DTCM_START', 'GMU_CM3_SYSRESET',
                 'GMU_CM3_BOOT_CONFIG', 'GMU_CM3_FW_INIT_RESULT', 'GMU_CM3_CFG',
                 'GMU_HFI_QTBL_ADDR', 'GMU_HFI_QTBL_INFO', 'GMU_HFI_CTRL_INIT',
                 'GMU_HFI_CTRL_STATUS', 'GMU_HOST2GMU_INTR_SET', 'GMU_SYS_BUS_CONFIG',
                 'GMU_ICACHE_CONFIG', 'GMU_DCACHE_CONFIG', 'GMU_AHB_FENCE_RANGE_0',
                 'GMU_GMU2HOST_INTR_INFO', 'GMU_AO_HOST_INTERRUPT_STATUS']:
        register(trees, regs, 'REG_' + name, 'A6XX', name)
    for name in ['GPU_GMU_CX_GMU_CX_FALNEXT_INTF', 'GPU_GMU_CX_GMU_CX_FAL_INTF']:
        register(trees, regs, 'REG_' + name, 'A6XX', name, 'A6XX')
    for index in (8, 9, 10):
        register(trees, regs, f'REG_GMU_GENERAL_{index}', 'A6XX',
                 f'GMU_GENERAL_{index}', 'A7XX')
    for name in ['GPU_RSCC_RSC_STATUS0_DRV0', 'RSCC_PDC_SLAVE_ID_DRV0',
                 'RSCC_HIDDEN_TCS_CMD0_ADDR', 'RSCC_HIDDEN_TCS_CMD0_DATA',
                 'RSCC_OVERRIDE_START_ADDR', 'RSCC_PDC_SEQ_START_ADDR',
                 'RSCC_PDC_MATCH_VALUE_LO', 'RSCC_PDC_MATCH_VALUE_HI',
                 'RSCC_SEQ_BUSY_DRV0']:
        register(trees, regs, 'REG_' + name, 'A6XX', name)
    register(trees, regs, 'REG_RSCC_SEQ_MEM_0_DRV0_A740', 'A6XX',
             'RSCC_SEQ_MEM_0_DRV0_A740', 'A7XX')
    register(trees, regs, 'REG_CX_MISC_TCM_RET_CNTL', 'A6XX_CX_MISC',
             'CX_MISC_TCM_RET_CNTL', 'A7XX-')
    register(trees, regs, 'REG_GBIF_QSB_SIDE0', 'A6XX', 'GBIF_QSB_SIDE0')
    for name in ['SEQ_START_ADDR', 'ENABLE_PDC']:
        register(trees, regs, 'REG_PDC_GPU_' + name, 'A6XX_PDC', 'GPU_' + name)

    fatal = sum(bit(trees, 'A6XX', 'GMU_AO_HOST_INTERRUPT_STATUS', name)
                for name in ['WDOG_BITE', 'FENCE_ERR', 'HOST_AHB_BUS_ERROR'])
    cm3 = bit(trees, 'A6XX', 'GMU_GMU2HOST_INTR_INFO', 'CM3_FAULT')

    source = (cache / 'drivers/gpu/drm/msm/adreno/a6xx_gmu.c').read_text()
    init = source[source.index('static void a6xx_gmu_rpmh_init('):
                  source.index('/*\n * The lowest 16 bits')]
    setup = []
    prefix = init[init.index('/* Disable SDE clock gating */'):
                  init.index('/* The second spin')]
    for reg, value in re.findall(r'gmu_write_rscc\(gmu,\s*([^,]+),\s*([\s\S]*?)\);', prefix):
        setup.append((1, 4 * number(reg, regs), number(value, regs)))
    if 'seqmem0_drv0_reg = REG_A7XX_RSCC_SEQ_MEM_0_DRV0_A740;' not in init:
        raise SystemExit('A740 sequencer selection absent')
    sequence = init[init.index('gmu_write_rscc(gmu, seqmem0_drv0_reg,'):
                    init.index('} else {', init.index('gmu_write_rscc(gmu, seqmem0_drv0_reg,'))]
    for reg, value in re.findall(r'gmu_write_rscc\(gmu,\s*([^,]+),\s*([^;]+)\);', sequence):
        setup.append((1, 4 * number(reg, regs), number(value, regs)))
    tail = init[init.index('setup_pdc:'):]
    for reg, value in re.findall(r'pdc_write\(pdcptr,\s*([^,]+),\s*([^;]+)\);', tail):
        setup.append((2, 4 * number(reg, regs), number(value, regs)))
    if len(setup) != 19:
        raise SystemExit('Actual A740 RSC/PDC sequence length differs: ' + str(len(setup)))
    for window, offset, value in setup:
        if (offset & 3 or offset + 4 > windows[window]['bytes'] or
                not 0 <= value <= 0xffffffff):
            raise SystemExit('Generated setup outside actual resource')

    catalog = (cache / 'drivers/gpu/drm/msm/adreno/a6xx_catalog.c').read_text()
    record = re.search(r'\.chip_ids = ADRENO_CHIP_IDS\(0x43050a01\)([\s\S]*?)\.preempt_record_size', catalog)
    chip = int(re.search(r'\.gmu_chipid = (0x[0-9a-f]+)', record[1])[1], 0)
    firmware = pins['firmware']['files']['qcom/gmu_gen70200.bin']

    out.mkdir(parents=True, exist_ok=True)
    defines = '\n'.join('#define ' + key + ' UINT32_C(' + hex(value) + ')'
                        for key, value in regs.items())
    text = '/* Generated from pinned fuxi DT and Adreno source. Do not commit. */\n'
    text += '#include <stdint.h>\n#include <stddef.h>\n' + defines + '\n'
    text += '#define SHENG_GMU_CHIP_ID UINT32_C(0x43050a01)\n'
    text += '#define SHENG_GMU_FW_CHIP_ID UINT32_C(' + hex(chip) + ')\n'
    text += '#define SHENG_GMU_CORE_GPU_DELTA ' + hex(windows[0]['base'] - windows[3]['base']) + 'u\n'
    text += '#define SHENG_GMU_FIRMWARE_BYTES ' + str(firmware['bytes']) + 'u\n'
    text += '#define SHENG_GMU_AO_FATAL_MASK ' + hex(fatal) + 'u\n'
    text += '#define SHENG_GMU_CM3_FATAL_MASK ' + hex(cm3) + 'u\n'
    text += 'static const uint8_t sheng_gmu_firmware_sha256[32]={' + \
        ','.join('0x' + firmware['sha256'][i:i + 2] for i in range(0, 64, 2)) + '};\n'
    text += 'static const uint64_t sheng_gmu_window_physical[5]={' + \
        ','.join(hex(window['base']) for window in windows) + '};\n'
    text += 'static const size_t sheng_gmu_window_bytes[5]={' + \
        ','.join(hex(window['bytes']) for window in windows) + '};\n'
    text += 'typedef struct {unsigned window;size_t byte_offset;uint32_t value;} ShengGmuSetupWrite;\n'
    text += '#define SHENG_GMU_RSC_SETUP_COUNT 19u\n'
    text += 'static const ShengGmuSetupWrite sheng_gmu_rsc_setup[19]={\n'
    text += '\n'.join('{' + str(window) + ',' + hex(offset) + ',' + hex(value) + '},'
                      for window, offset, value in setup) + '\n};\n'
    header = out / 'ShengGmuSource.h'
    header.write_text(text)

    report = dict(linux_revision=pins['linux']['commit'],
                  dtb_sha256=sha(dtb), registers=regs, window_names=ORDERED,
                  windows=windows, rsc_setup=setup,
                  gmu_firmware_chip_id=chip, firmware_policy=firmware,
                  generated_header_sha256=sha(header), source_analysis_only=True,
                  hardware_registers_observed=False)
    (out / 'fuxi-gmu-source-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: pinned fuxi DT windows and A740 RSC/PDC sequence; source evidence only')
    return out, report


if __name__ == '__main__':
    prepare()
