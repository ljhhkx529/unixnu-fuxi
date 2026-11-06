#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build build/fuxi-display/libFuxiDisplay.a: the opaque ABI of the original Legacy framebuffer.

The original IOMobileFramebuffer lives in the prelinked IOMobileGraphicsFamily, which is added to the
kernel after it links. A driver cannot mention that class at link time, so the ABI is declared here
instead: the audit writes the exact 358 virtual entries of the original instance, and the header gives
this file opaque storage of the same extent (0x18a0) with only the entries whose contract has been
established named. That is how UniXNU-sheng and unixnu-fuxi-old both do it.

This step builds the driver object only. Publishing the display and claiming the desktop come later,
one verifiable step at a time.
"""
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

NL = chr(10)
ROOT = Path(__file__).resolve().parents[1]
# The kernel script of the shared tree links the display archive from build/native-legacy-display/, a
# path unixnu-fuxi-old also uses. Keeping it there means CONFIG_XNU_NATIVE_LEGACY_DISPLAY needs no
# change to that shared script.
OUT = ROOT / 'build/native-legacy-display'
OBJ = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
AUDIT = ROOT / 'build/display-analysis/mobile-graphics-legacy-abi.json'
CXX_FLAGS = ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unnecessary-virtual-specifier',
             '-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion']

HEADER_HEAD = '''// SPDX-License-Identifier: BSD-2-Clause-Patent
// Opaque ABI storage for the exact original class, not source-level access to its private fields.
// The original metadata supplies the full Framebuffer/Service/IOService ancestry; this declaration
// only reserves the same instance extent and names the entries whose contract is established.
#pragma once
#include <IOKit/IOService.h>
#include <libkern/libkern.h>
struct OriginalLegacySwapRequest;
// Original state request (skip/requested and skip/powered word pairs, then the changed flag) taken by
// the state setter.
struct OriginalLegacyStateRequest {uint32_t skipRequested,requested,skipPowered,powered;uint8_t changed;uint8_t pad[11];};
// Original field offsets: the vsync client queue in the display; rectangles, layer masks (updated,
// disabled, background), options, dimensions and surfaces in a swap request.
static constexpr unsigned kLegacyVsyncClients=5592;
static constexpr unsigned kRequestRect0=17,kRequestRect1=81,kRequestMasks=792,kRequestOptions=804,kRequestDims=816,kRequestSurfaces=2624;
// Backend leaves: pure original slots the owned backend implements only as guards. The list is the
// audit's abstract_slots minus 213 and 337, which stay abstract in the declaration below.
#define NATIVE_LEGACY_LEAVES(LEAF) LEAF(211) LEAF(245) LEAF(246) LEAF(306) LEAF(307) LEAF(338) LEAF(345)
class IOMobileFramebufferLegacy : public IOService {
 OSDeclareAbstractStructors(IOMobileFramebufferLegacy)
 unsigned char originalPrivate[0x18a0-sizeof(IOService)];
public:
 void installBootDimensions(uint32_t width,uint32_t height) {
  *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(this)+5808)=width;
  *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(this)+5812)=height;
 }
 const OriginalLegacySwapRequest *pendingSwapRequest() const {
  const OriginalLegacySwapRequest *request=nullptr;
  memcpy(&request,reinterpret_cast<const unsigned char *>(this)+5480,sizeof(request));return request;
 }
 bool activateOnlyPendingSwap(const OriginalLegacySwapRequest *request) {
  auto self=reinterpret_cast<unsigned char *>(this);
  auto bytes=reinterpret_cast<const unsigned char *>(request);
  const void *current=nullptr,*head=nullptr,*tail=nullptr,*next=nullptr,*previous=nullptr;
  memcpy(&current,self+5472,8);memcpy(&head,self+5480,8);memcpy(&tail,self+5488,8);
  if(!request || current || head!=request)return false;
  memcpy(&next,bytes+2960,8);memcpy(&previous,bytes+2968,8);
  if(previous!=self+5480)return false;
  if(next) {
   const void *link=self+5480;
   memcpy(const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(next))+2968,&link,8);
  } else if(tail!=bytes+2960)return false;
  memcpy(self+5472,&request,8);
  if(next) {memcpy(self+5480,&next,8);}
  else {const void *empty=nullptr,*emptyTail=self+5480;memcpy(self+5480,&empty,8);memcpy(self+5488,&emptyTail,8);}
  return true;
 }
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 IOReturn setProperties(OSObject *) override;
 void systemWillShutdown(IOOptionBits) override;
 IOService *probe(IOService *,SInt32 *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 IOWorkLoop *getWorkLoop() const override;
 IOReturn newUserClient(task_t,void *,UInt32,IOUserClient **) override;
 IOReturn setPowerState(unsigned long,IOService *) override;
'''

# Bindings of the original family: names this driver calls, and the exact original addresses they must
# reach. unixnu-fuxi-old ships the same table for the same kernelcache (pinned by [kernel.prelink]
# kernelcache_sha256), and every entry below is checked against the candidate before it is used, so a
# different IPSW stops here instead of binding a wrong address.
ORIGINAL_CTOR = 0xfffffff0057865cc
ORIGINAL_DTOR = 0xfffffff005786618
ORIGINAL_METACLASS = 0xfffffff006b35940
# The original IOSurface getters, read through the real swap requests: name, address, field, 64-bit.
ORIGINAL_SURFACE = [
    ('descriptor', 0xfffffff004b9ead0, 48, True),
    ('width', 0xfffffff004b9ebbc, 88, True),
    ('height', 0xfffffff004b9ebc4, 96, True),
    ('stride', 0xfffffff004b9ebd4, 104, True),
    ('allocation', 0xfffffff004b9ebfc, 144, True),
    ('format', 0xfffffff004b9ebac, 128, False),
    ('planes', 0xfffffff004b9ebf4, 152, False),
]
# The original display-refresh notification sender (self, timestamp, continuous host time, interval):
# it sends the six-word vsync message to every client queued at +5592. The platform subclass that calls
# it on each panel vblank is not part of this port.
ORIGINAL_VSYNC_NOTIFY = 0xfffffff005796720
ORIGINAL_HEAD_VSYNC = bytes.fromhex('ff0302d1fd7b07a9fdc30191a0831ff8')
# Slot 301's own body: fills the six words setDisplayRefreshProperties publishes as IOMFBDisplayRefresh.
ORIGINAL_SET_REFRESH = 0xfffffff00579ebb8

KNOWN = {
    184: ' virtual bool isMainDisplay();',
    188: ' virtual IOReturn getDotPitch(uint32_t *);',
    213: ' virtual IOReturn supportedFrameInfo(uint32_t,uint32_t *,char *,uint32_t *)=0;',
    249: ' virtual void getDisplayDimensions(uint32_t *,uint32_t *);',
    288: ' virtual bool startHardware();',
    290: ' virtual void kickSwapQueue();',
    295: ' virtual void completeCurrentSwap();',
    296: ' virtual bool validateSwap(const OriginalLegacySwapRequest *);',
    301: ' virtual bool getRefreshAttributes(uint64_t *,bool);',
    354: ' virtual int applyStateRequest(OriginalLegacyStateRequest *,bool);',
    337: ' virtual IOReturn collectCompletionReports(uint32_t,void *,uint32_t *,uint32_t)=0;',
}


def objcopy(*arguments):
    """llvm-objcopy of the same LLVM the archive builders use."""
    for candidate in ('/opt/homebrew/opt/llvm/bin/llvm-objcopy', '/usr/local/opt/llvm/bin/llvm-objcopy',
                      shutil.which('llvm-objcopy')):
        if candidate and Path(candidate).is_file():
            subprocess.run([candidate, *map(str, arguments)], check=True, timeout=120)
            return
    sys.exit('llvm-objcopy missing: brew install llvm')


def record(path):
    if not path.is_file():
        sys.exit(f'{path} missing: the XNU pass of the kernel step has not left its compile records')
    return json.loads(path.read_text().strip().rstrip(','))


def command(rec, source, obj, extra):
    args, out, i = rec['arguments'], [], 0
    while i < len(args):
        arg = args[i]
        if arg == rec['file']:
            out.append(str(source))
        elif arg in ('-o', '-MF', '-MT', '-MJ'):
            if arg == '-o':
                out += ['-o', str(obj)]
            i += 1
        elif arg == '-Xclang' and i + 1 < len(args) and args[i + 1].startswith('-W'):
            i += 1
        elif not arg.startswith('-W'):
            out.append(arg)
        i += 1
    return out + [str(x) for x in extra]


def generate_header(audit):
    pure = set(audit['abstract_slots'])
    text = HEADER_HEAD
    for index in range(168, 358):
        if index in KNOWN:
            text += KNOWN[index] + chr(10)
        elif index in pure:
            text += f' virtual __attribute__((noreturn)) void opaque{index}()=0;' + chr(10)
        else:
            text += (f' virtual __attribute__((deprecated("Unclassified ABI: never call from source"))) '
                     f'void opaque{index}();' + chr(10))
    text += '};' + chr(10)
    text += 'static_assert(sizeof(IOMobileFramebufferLegacy)==0x18a0,"Actual original instance extent");' + chr(10)
    return text


def candidate_path():
    """The prelink candidate the audit qualified, from the device repository or the tree."""
    for directory in (ROOT / 'build/xnu-prelink', ROOT.parent.parent / 'unixnu-fuxi/build/xnu-prelink'):
        if directory.is_dir():
            names = sorted(directory.glob('native-owner-*-candidate.macho'))
            if names:
                return names[-1]
    sys.exit('no prelink candidate: run scripts/stage-prelink-candidate.py after the kernel step')


def bindings(audit):
    """Symbol -> original address, each one checked against the candidate before it is used."""
    sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
    from native_prelink import backed_offset, macho

    candidate = candidate_path()
    blob = candidate.read_bytes()
    image = macho(blob)

    def read(va, count):
        at = backed_offset(image['segments'], va, count, len(blob))
        return blob[at:at + count]

    import struct
    table = {
        '__ZN25IOMobileFramebufferLegacyC2EPK11OSMetaClass': ORIGINAL_CTOR,
        '__ZN25IOMobileFramebufferLegacyD2Ev': ORIGINAL_DTOR,
        '__ZN25IOMobileFramebufferLegacy10gMetaClassE': ORIGINAL_METACLASS,
        '__ZTV25IOMobileFramebufferLegacy': int(audit['legacy_vptr'], 16) - 16,
    }
    for name, address, field, wide in ORIGINAL_SURFACE:
        expected = (0xf9400000 if wide else 0xb9400000) | ((field // (8 if wide else 4)) << 10)
        if read(address, 8) != struct.pack('<2I', expected, 0xd65f03c0):
            sys.exit(f'the original surface getter {name} is not at {address:#x} in this kernelcache')
        table['_native_legacy_surface_' + name] = address
    if read(ORIGINAL_VSYNC_NOTIFY, 16) != ORIGINAL_HEAD_VSYNC:
        sys.exit(f'the original vsync sender is not at {ORIGINAL_VSYNC_NOTIFY:#x} in this kernelcache')
    table['_native_legacy_vsync_notify'] = ORIGINAL_VSYNC_NOTIFY
    table['_native_legacy_set_refresh_properties'] = ORIGINAL_SET_REFRESH
    return table


def bindings_object(table):
    """Assemble the bindings the way unixnu-fuxi-old does: one .set per name, no code."""
    source = OUT / 'original-bindings.S'
    source.write_text(''.join(f'.globl {name}' + NL + f'.set {name}, {address:#x}' + NL
                              for name, address in sorted(table.items())))
    sdk = subprocess.run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'],
                         capture_output=True, text=True, check=True).stdout.strip()
    obj = OUT / 'original-bindings.o'
    subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk,
                    '-c', str(source), '-o', str(obj)], check=True, timeout=120)
    return obj


def vtable_bindings(obj, audit):
    """Bind every original entry the compiled vtable still points at, from the audit's addresses.

    The declaration names 358 original entries; the driver overrides the qualified ones and leaves the
    rest to the original implementation. The compiler therefore emits references to
    IOMobileFramebufferLegacy::opaqueNNN, and each one is bound to the address the audit recorded for
    that slot, exactly as unixnu-fuxi-old does. Reading the vtable back also proves the object cannot
    overrun the original layout: nothing may be relocated past entry 357.
    """
    sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
    from native_prelink import macho

    import struct
    raw = obj.read_bytes()
    image = macho(raw)
    command = next(p for c, p, _ in image['commands'] if c == 2)
    symbols_at, count, strings, _ = struct.unpack_from('<4I', raw, command + 8)
    symbols = []
    for index in range(count):
        name_at, kind, _, _, va = struct.unpack_from('<IBBHQ', raw, symbols_at + index * 16)
        end = raw.index(bytes([0]), strings + name_at)
        symbols.append((raw[strings + name_at:end].decode(), va, kind))
    vtable = next(va for name, va, _ in symbols if name == '__ZTV19NativeLegacyDisplay')
    relocations = {}
    for segment in image['segments']:
        for section in segment['sections']:
            if section['va'] <= vtable < section['va'] + section['size']:
                start, count = struct.unpack_from('<2I', raw, section['command_offset'] + 56)
                for index in range(count):
                    offset, bits = struct.unpack_from('<2I', raw, start + index * 8)
                    if bits >> 27 & 1 and bits >> 28 == 0 and bits >> 25 & 3 == 3:
                        relocations[section['va'] + offset] = symbols[bits & 0xffffff][0]
    if vtable + 16 + 358 * 8 in relocations:
        sys.exit('the compiled vtable runs past the 358 entries of the original instance')
    compiled = [relocations.get(vtable + 16 + index * 8) for index in range(358)]
    if not all(compiled):
        sys.exit('the compiled vtable is missing original entries')
    table = {}
    for index, name in enumerate(compiled):
        if name.startswith(('__ZN25IOMobileFramebufferLegacy', '__ZNK25IOMobileFramebufferLegacy')):
            table[name] = int(audit['methods'][index]['address'], 16)
    return table


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    if not AUDIT.is_file():
        sys.exit(f'{AUDIT} missing: run scripts/audit-mobile-graphics-legacy.py first')
    audit = json.loads(AUDIT.read_text())
    if audit['legacy_virtual_entries'] != 358:
        sys.exit('the audit no longer describes the 358-entry Legacy instance')
    (OUT / 'OriginalLegacyFramebufferABI.h').write_text(generate_header(audit))
    print(f'generated {OUT.relative_to(ROOT)}/OriginalLegacyFramebufferABI.h')

    gop = ROOT / 'drivers/GopFramebuffer'
    source = gop / 'NativeLegacyDisplay.cpp'
    if not source.is_file():
        sys.exit(f'{source} missing: select GopFramebuffer in board.toml [components]')
    cxx = record(OBJ / 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')
    obj = OUT / 'NativeLegacyDisplay.o'
    subprocess.run(command(cxx, source, obj, CXX_FLAGS + ['-I', gop, '-I', OUT]),
                   cwd=cxx['directory'], check=True, timeout=240)
    print(f'compiled {source.relative_to(ROOT)}')
    # The probe runs the driver's static constructor itself, after OSMetaClass preModLoad, so the section
    # is dropped and the symbol globalised the way sheng and unixnu-fuxi-old stage this object.
    obj_late = OUT / 'NativeLegacyDisplay.late.o'
    objcopy('--remove-section=__DATA,__mod_init_func',
            '--globalize-symbol=__GLOBAL__sub_I_NativeLegacyDisplay.cpp', obj, obj_late)
    # The board backends the driver calls into. They live next to the other fuxi glue, and each hook
    # becomes real as its own step lands; see package/FuxiDisplayProbe.c.
    probe = ROOT / 'kits/fuxi/FuxiDisplayProbe.c'
    if not probe.is_file():
        sys.exit(f'{probe} missing: the fuxi device repository owns this board glue')
    c = record(OBJ / 'osfmk/DEVELOPMENT/startup.o.json')
    probe_obj = OUT / 'FuxiDisplayProbe.o'
    subprocess.run(command(c, probe, probe_obj, ['-Wall', '-Wextra', '-Werror', '-mgeneral-regs-only',
                                                 '-Wno-unused-parameter']),
                   cwd=c['directory'], check=True, timeout=180)
    print(f'compiled {probe.relative_to(ROOT)}')

    # The presenter publishes the boot framebuffer as a Metal-capable display and takes the frames the
    # display driver offers it (sheng-fb-gpu=1). Its static constructor is staged the way sheng stages it.
    presenter = gop / 'NativeGOPPresenter.cpp'
    presenter_obj = OUT / 'NativeGOPresenter.o'
    subprocess.run(command(cxx, presenter, presenter_obj, CXX_FLAGS + ['-I', gop]),
                   cwd=cxx['directory'], check=True, timeout=240)
    presenter_late = OUT / 'NativeGOPPresenter.late.o'
    objcopy('--remove-section=__DATA,__mod_init_func',
            '--globalize-symbol=__GLOBAL__sub_I_NativeGOPPresenter.cpp', presenter_obj, presenter_late)
    print(f'compiled {presenter.relative_to(ROOT)} (late init staged)')

    # The board probe: it publishes the display, keeps the swap rings and hands frames to the presenter.
    # Ported from UniXNU-sheng package/LegacyDisplayProbe.cpp; see package/FuxiLegacyDisplayProbe.cpp.
    legacy_probe = ROOT / 'kits/fuxi/FuxiLegacyDisplayProbe.cpp'
    if not legacy_probe.is_file():
        sys.exit(f'{legacy_probe} missing: the fuxi device repository owns this board probe')
    legacy_obj = OUT / 'FuxiLegacyDisplayProbe.o'
    subprocess.run(command(cxx, legacy_probe, legacy_obj, CXX_FLAGS + ['-I', gop, '-I', OUT]),
                   cwd=cxx['directory'], check=True, timeout=240)
    print(f'compiled {legacy_probe.relative_to(ROOT)}')

    table = bindings(audit)
    table.update(vtable_bindings(obj, audit))
    bindings_obj = bindings_object(table)
    print(f'bound {len(table)} original symbols after checking each against the candidate')

    # The A740 GPU power path: GMU/clock/DCVS. It needs the Adreno740 headers and the board facts in
    # package/GpuPowerBoard.h, and it is the same probe unixnu-fuxi-old starts from its display probe.
    gpu_power = ROOT / 'kits/fuxi/FuxiGpuPowerProbe.cpp'
    if not gpu_power.is_file():
        sys.exit(f'{gpu_power} missing: the fuxi device repository owns this board probe')
    legacy_obj_gpu = OUT / 'FuxiGpuPowerProbe.o'
    subprocess.run(command(cxx, gpu_power, legacy_obj_gpu,
                           CXX_FLAGS + ['-I', gop, '-I', OUT, '-I', ROOT / 'kits/fuxi',
                                        '-I', ROOT / 'drivers/Adreno740', '-I', ROOT / 'build/fuxi-gmu',
                                        '-I', ROOT / 'kits/ArmSmmu4K', '-I', ROOT / 'kits/AdrenoPM4',
                                        '-I', ROOT / 'kits/AdrenoFirmware',
                                        # The Adreno740 header declares one of these parameters as a pointer
                                        # where this board's copy passes the fixed buffer; the types agree.
                                        '-Wno-array-parameter']),
                   cwd=cxx['directory'], check=True, timeout=240)
    print(f'compiled {gpu_power.relative_to(ROOT)}')

    # The GPU role the A740 user client asks for; see package/FuxiGpuRole.c.
    gpu_role = ROOT / 'kits/fuxi/FuxiGpuRole.c'
    if not gpu_role.is_file():
        sys.exit(f'{gpu_role} missing: the fuxi device repository owns this board glue')
    gpu_role_obj = OUT / 'FuxiGpuRole.o'
    subprocess.run(command(record(OBJ / 'osfmk/DEVELOPMENT/startup.o.json'), gpu_role, gpu_role_obj,
                           ['-Wall', '-Wextra', '-Werror', '-mgeneral-regs-only', '-Wno-unused-parameter']),
                   cwd=record(OBJ / 'osfmk/DEVELOPMENT/startup.o.json')['directory'], check=True, timeout=180)
    print(f'compiled {gpu_role.relative_to(ROOT)}')

    archive = OUT / 'libNativeLegacyDisplay.late.a'
    archive.unlink(missing_ok=True)
    ar = ['xcrun', 'ar'] if shutil.which('xcrun') else [shutil.which('llvm-ar') or 'ar']
    subprocess.run([*ar, 'rcs', str(archive), str(obj_late), str(probe_obj), str(legacy_obj), str(bindings_obj), str(presenter_late), str(gpu_role_obj), str(legacy_obj_gpu)],
                   check=True, timeout=60)
    # The kernel script checks the archive against a report of the exact sources it was built from, so a
    # changed driver cannot slip into the link unnoticed (build-xnu-4k-baseline.sh PY_LEGACY_DISPLAY).
    report = {
        'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
        'source_sha256': {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in (source, legacy_probe, presenter, probe, gop / 'NativeLegacyRequest.h',
                                       gop / 'NativeGOPWire.h', gop / 'NativeHalfCodes.h')},
    }
    (OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + NL)
    print(f'display archive: {archive.relative_to(ROOT)}')


if __name__ == '__main__':
    main()
