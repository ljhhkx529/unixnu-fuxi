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
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/fuxi-display'
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

    archive = OUT / 'libFuxiDisplay.a'
    archive.unlink(missing_ok=True)
    ar = ['xcrun', 'ar'] if shutil.which('xcrun') else [shutil.which('llvm-ar') or 'ar']
    subprocess.run([*ar, 'rcs', str(archive), str(obj), str(probe_obj)], check=True, timeout=60)
    print(f'display archive: {archive.relative_to(ROOT)}')


if __name__ == '__main__':
    main()
