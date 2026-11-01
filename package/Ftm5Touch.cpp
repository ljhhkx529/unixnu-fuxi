// SPDX-License-Identifier: BSD-2-Clause-Patent
// fuxi touch: STMicroelectronics FingerTip (FTM5) on GENI SPI SE4. Brings the controller up from reset, keeps the
// scan engine running and publishes the finger set through native_touch_publish (the shared seqlock in TouchProbe.cpp).
// Protocol: 0xa0 scan mode, 0x87 FIFO read, 0xa7 sysinfo; 8-byte events, event id in ev[0], finger id in ev[1]>>4.
// Power: vdd 1.8 V (pm_humu_l12, resource "ldob12") and avdd 3.2 V (pm8010m_l6, resource "ldom6") are RPMh VRM
// resources on the apps RSC; both are voted on directly in the active set through the AOP command DB.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <libkern/libkern.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <string.h>
extern "C" {
#include "GeniSpi.h"
#include "SmmuBypass.h"
#include "TouchBoard.h"
void native_touch_publish(const int *ids, const int *xs, const int *ys, unsigned count);
extern char native_touch_rows[4][120];
extern char native_touch_trace[8][120];
extern volatile uint32_t native_touch_stage_req, native_touch_stage_done, native_touch_step,
    native_touch_frames, native_touch_valid, native_touch_contacts, native_touch_hid_reports;
extern volatile int native_touch_error;
extern volatile int native_touch_last_x, native_touch_last_y;
// HID service bring-up (HidInputProbe.cpp), republished to the boot UI while it is up.
extern volatile uint32_t native_hid_stage, native_hid_input_events[8];
extern volatile int native_hid_load_status, native_hid_input_results[8];
// Read by HidInputProbe: set before the thread starts, so the digitizer service can be created in the same boot
// without a new kernel argument.
volatile int native_fuxi_touch_enabled;
// Read by BootProgress: the touch pad overlay is diagnostic painting and can be isolated from the controller bring-up.
volatile int native_fuxi_touch_pad = 1;
// Read by BootProgress: raw TLMM pin-config words from the read-only dump mode (fuxi-touch-pins=8).
volatile uint32_t native_fuxi_touch_pins[6];
volatile int native_fuxi_touch_pins_n;
// On-screen SPI/pin diagnostics for the bring-up image (fuxi-touch-pins bit 0x10).
volatile uint32_t native_fuxi_touch_dbg[24];
}

namespace {
// fuxi DT pinctrl@f000000 covers 0xf000000..0x10000000, but the GPIO tile starts 1 MiB in, at the
// sm8550 TLMM base (0xf100000, 0x300000); the container head raises SError on access (t17).
constexpr uint64_t kTlmmBase = 0x0f100000ULL;
constexpr unsigned kResetGpio = 24;          // fts,reset-gpio = <&tlmm 24 0>; asserted low, released high
constexpr unsigned kIrqGpio = 25;            // fts,irq-gpio = <&tlmm 25 0x2008>, level low (polled here)
constexpr unsigned kPinMiso = 44, kPinMosi = 45, kPinClk = 46, kPinCs = 47;
constexpr unsigned kSpiHz = 12000000;        // fts@0 spi-max-frequency = <0xb71b00>
constexpr unsigned kMaxFingers = 10;
constexpr unsigned kEventSize = 8;
constexpr unsigned kFifoDepth = 32;
constexpr unsigned kSysinfoLen = 208;
constexpr size_t kDmaBytes = 64 * 1024;
constexpr unsigned kBufferBytes = 8 + 1 + kFifoDepth * kEventSize;

// RPMh VRM: apps_rsc drv@2 (reg-names "drv-2"), qcom,tcs-offset = 0xd00; TCS blocks are 672 bytes apart and a
// command slot is 24 bytes on RSC v3.0 (20 on v2.7). resource addresses come from aop_cmd_db_region@81c60000.
constexpr uint64_t kCmdDbBase = 0x81c60000ULL;
constexpr uint64_t kCmdDbSize = 0x20000;
constexpr uint64_t kRscDrv2Base = 0x17a20000ULL;
constexpr unsigned kRscTcsOffset = 0x0d00;
constexpr unsigned kRscTcsStride = 672;
constexpr unsigned kActiveTcsCount = 3;   // qcom,tcs-config begins with <ACTIVE_TCS(2) 3>

struct RscRegs {unsigned wait, control, status, cmdEnable, msgId, cmdAddr, cmdData, cmdStatus, cmdStride;};

constexpr uint8_t kCmdScan = 0xa0, kCmdFifo = 0x87, kCmdSysinfo = 0xa7;
constexpr uint8_t kEvReady = 0x03, kEvEnter = 0x13, kEvMotion = 0x23, kEvLeave = 0x33, kEvError = 0xf3;

struct Region {OSSharedPtr<IOMemoryMap> keep; uintptr_t va = 0;};

bool mapRegion(uint64_t phys, uint64_t length, Region &region) {
    IODeviceMemory *memory = IODeviceMemory::withRange(phys, length);
    if (!memory) return false;
    region.keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    memory->release();
    region.va = region.keep ? static_cast<uintptr_t>(region.keep->getVirtualAddress()) : 0;
    return region.va != 0;
}
uint32_t rd32(uintptr_t base, uint32_t reg) {return *reinterpret_cast<volatile uint32_t *>(base + reg);}
void wr32(uintptr_t base, uint32_t reg, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(base + reg) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
void cacheRange(void *p, size_t len, int op) {   // 0 clean, 1 clean+invalidate, 2 invalidate
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    size_t line = 4u << ((ctr >> 16) & 15);
    uintptr_t a = reinterpret_cast<uintptr_t>(p) & ~(line - 1), end = reinterpret_cast<uintptr_t>(p) + len;
    for (; a < end; a += line) {
        if (op == 0) __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
        else if (op == 1) __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
        else __asm__ volatile("dc ivac, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}
void dmaClean(void *, void *p, size_t len) {cacheRange(p, len, 0);}
void dmaCleanInval(void *, void *p, size_t len) {cacheRange(p, len, 1);}
void dmaInval(void *, void *p, size_t len) {cacheRange(p, len, 2);}
uint32_t smmuRd(void *ctx, unsigned o) {return rd32(reinterpret_cast<uintptr_t>(ctx), o);}
void smmuWr(void *ctx, unsigned o, uint32_t v) {wr32(reinterpret_cast<uintptr_t>(ctx), o, v);}
int smmuErrorCode(const char *e) {
    static const char *const names[] = {"SMMU AArch64 stage 1 unsupported", "DMA exceeds SMMU address width", "unsupported SMMU geometry",
        "conflicting stream mapping", "reserved context bank is not a bypass bank", "no free stream-match group",
        "SMMU bypass bank readback", "SMMU stream context readback", "SMMU stream mapping readback"};
    for (unsigned i = 0; i < sizeof names / sizeof *names; ++i) if (strcmp(e, names[i]) == 0) return static_cast<int>(i) + 1;
    return 99;
}
uint32_t ioRd(void *ctx, uint32_t reg) {return rd32(reinterpret_cast<uintptr_t>(ctx), reg);}
void ioWr(void *ctx, uint32_t reg, uint32_t value) {wr32(reinterpret_cast<uintptr_t>(ctx), reg, value);}
void ioDelayUs(void *, unsigned us) {
    if (us > 50) {IODelay(us); return;}
    uint64_t frequency = 0, start = 0, now = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(start));
    const uint64_t ticks = (frequency ? frequency : 19200000) * us / 1000000 + 1;
    do {__asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(now));} while (now - start < ticks);
}

struct Session {
    Region gccLow, gccS4, gccHalt, gccVote, tlmm, wrapper, se, smmu, cmddb, rsc;
    IOBufferMemoryDescriptor *dmaMem;
    uint64_t dmaPhys;
    int smmuSlot, smmuBank, smmuErr;
    struct geni_spi spi;
    uint64_t levelHz[TB_DFS_LEVELS];
    unsigned level, div;
    uint64_t actualHz;
    uint8_t txbuf[kBufferBytes], rxbuf[kBufferBytes];
    uint32_t vals[8];
    uint16_t chipId, fwVer, cfgVer, panelX, panelY;
    uint8_t sig;
    uint32_t reads, events, enters, motions, leaves, errors, readys, bad, fifo;
    int lastRc, lastLen;
    uint32_t lastIrq, lastStatus, lastTx, lastRx;
    RscRegs regs;
    uint32_t rscId, railAddr[2];
    int pwrRc[2];
    unsigned cmdDbOk, pwrTcs;
    uint32_t pwrIrqStatus, pwrCmdStatus, pwrTcsStatus;
    bool active[kMaxFingers];
    uint16_t cx[kMaxFingers], cy[kMaxFingers];
    bool dirty;
    unsigned pollMs;
};

void rows(const Session &s);
void fail(unsigned step, int code) {
    if (!native_touch_error) {native_touch_step = step; native_touch_error = code ? code : -1;}
}
void mark(const Session &s, unsigned step) {native_touch_step = step; rows(s);}

void rows(const Session &s) {
    snprintf(native_touch_rows[0], sizeof native_touch_rows[0],
        "FTS s%u/%u st%u e%d chip%04x fw%04x p%ux%u ev%u c%u %d,%d hid%u",
        native_touch_stage_done, native_touch_stage_req, native_touch_step, native_touch_error,
        s.chipId, s.fwVer, s.panelX, s.panelY, s.events, native_touch_contacts,
        native_touch_last_x, native_touch_last_y, native_touch_hid_reports);
    snprintf(native_touch_rows[1], sizeof native_touch_rows[1],
        "FTS gp%08x %08x %08x %08x vote%08x halt%08x %08x %08x",
        s.vals[0], s.vals[1], s.vals[2], s.vals[3], s.vals[4], s.vals[5], s.vals[6], s.vals[7]);
    snprintf(native_touch_rows[2], sizeof native_touch_rows[2],
        "FTS se fw%08x if%08x hw%08x L%u/%u %uHz rc%d sm%d/%d/e%d pa%llx",
        s.spi.info.fw_revision, s.spi.info.if_disable, s.spi.info.qup_hw_version, s.level, s.div,
        static_cast<unsigned>(s.actualHz), s.lastRc, s.smmuSlot, s.smmuBank, s.smmuErr, (unsigned long long)s.dmaPhys);
    snprintf(native_touch_rows[3], sizeof native_touch_rows[3],
        "FTS rd%u ev%u en%u mo%u le%u ry%u er%u bad%u fifo%u sig%02x poll%u",
        s.reads, s.events, s.enters, s.motions, s.leaves, s.readys, s.errors, s.bad, s.fifo, s.sig, s.pollMs);
    snprintf(native_touch_trace[0], sizeof native_touch_trace[0],
        "FTS sysinfo sig%02x chip%04x fw%04x cfg%04x p%ux%u", s.sig, s.chipId, s.fwVer, s.cfgVer, s.panelX, s.panelY);
    snprintf(native_touch_trace[1], sizeof native_touch_trace[1],
        "FTS xf rc%d n%d irq%08x gs%08x tx%08x rx%08x", s.lastRc, s.lastLen, s.lastIrq, s.lastStatus, s.lastTx, s.lastRx);
    snprintf(native_touch_trace[2], sizeof native_touch_trace[2], "FTS state act%u%u%u%u%u%u%u%u%u%u dirty%u",
        s.active[0], s.active[1], s.active[2], s.active[3], s.active[4], s.active[5], s.active[6], s.active[7],
        s.active[8], s.active[9], s.dirty ? 1u : 0u);
    native_fuxi_touch_dbg[14] = (native_touch_stage_done << 16) | (native_touch_step << 8);
    native_fuxi_touch_dbg[15] = static_cast<uint32_t>(native_touch_error);
}

// One full-duplex command: command bytes, one dummy byte, then the reply. The reply starts at rxbuf[cmd_len + 1]
// (the ST protocol); same framing as the mainline st-ftm5 driver.
int busRead(Session &s, const uint8_t *cmd, unsigned cmdLen, uint8_t *out, unsigned len) {
    const unsigned total = cmdLen + 1 + len;
    if (total > sizeof s.txbuf) return GENI_ERR_PARAM;
    memset(s.txbuf, 0, total);
    memcpy(s.txbuf, cmd, cmdLen);
    const int rc = geni_spi_transfer(&s.spi, s.txbuf, s.rxbuf, total, 0);
    if (rc) {
        if (!s.lastRc) {
            s.lastRc = rc; s.lastLen = static_cast<int>(total);
            s.lastIrq = s.spi.snap_irq; s.lastStatus = s.spi.snap_status;
            s.lastTx = s.spi.snap_txfs; s.lastRx = s.spi.snap_rxfs;
        }
        return rc;
    }
    memcpy(out, s.rxbuf + cmdLen + 1, len);
    return 0;
}
int busWrite(Session &s, const uint8_t *cmd, unsigned len) {
    return geni_spi_transfer(&s.spi, cmd, nullptr, len, 0);
}
int readEvents(Session &s, unsigned count, uint8_t *out) {
    const uint8_t cmd = kCmdFifo;
    return busRead(s, &cmd, 1, out, count * kEventSize);
}
uint16_t le16(const uint8_t *p) {return static_cast<uint16_t>(p[0] | (p[1] << 8));}

// TLMM: ctl = pull 1:0, mux 4:2, drive 8:6, output enable 9; io = out bit 1, in bit 0.
constexpr unsigned kPullDown = 1, kPullUp = 3;
uint32_t pinCfg(unsigned func, unsigned pull, unsigned drive, bool output) {
    return ((func & 15u) << 2) | (pull & 3u) | ((drive & 7u) << 6) | (output ? (1u << 9) : 0);
}
void pinOut(uintptr_t tlmm, unsigned gpio, bool value) {
    uint32_t io = rd32(tlmm, gpio * TB_GPIO_STRIDE + 4);
    wr32(tlmm, gpio * TB_GPIO_STRIDE + 4, value ? (io | 2u) : (io & ~2u));
}
bool configurePins(Session &s) {
    unsigned pins = 7;   // bit0 SPI 44-47, bit1 reset 24, bit2 irq 25
    if (!PE_parse_boot_argn("fuxi-touch-pins", &pins, sizeof pins)) pins = 7;
    if (pins & 8) {   // read-only dump: publish the live CTL words, write nothing
        static const unsigned probe[6] = {kResetGpio, kIrqGpio, kPinMiso, kPinMosi, kPinClk, kPinCs};
        for (unsigned i = 0; i < 6; ++i) native_fuxi_touch_pins[i] = rd32(s.tlmm.va, probe[i] * TB_GPIO_STRIDE);
        __asm__ volatile("dmb ish" ::: "memory");
        native_fuxi_touch_pins_n = 6;
        return true;
    }
    // qupv3_se4_spi pins: function qup1_se4_l0..l3, 6 mA. mi_ts_cs_active pulls CS up; the data/clk groups pull down.
    if (pins & 1) {
        wr32(s.tlmm.va, kPinMiso * TB_GPIO_STRIDE, pinCfg(1, kPullDown, TB_DRIVE_6MA, false));
        wr32(s.tlmm.va, kPinMosi * TB_GPIO_STRIDE, pinCfg(1, kPullDown, TB_DRIVE_6MA, true));
        wr32(s.tlmm.va, kPinClk * TB_GPIO_STRIDE, pinCfg(1, kPullDown, TB_DRIVE_6MA, true));
        wr32(s.tlmm.va, kPinCs * TB_GPIO_STRIDE, pinCfg(1, kPullUp, TB_DRIVE_6MA, true));
    }
    // mi_ts_active: reset driven high (asserted) from the start; irq is an input with the DT pull-up.
    if (pins & 2) {
        wr32(s.tlmm.va, kResetGpio * TB_GPIO_STRIDE, pinCfg(0, kPullUp, 3, true));
        pinOut(s.tlmm.va, kResetGpio, true);
    }
    if (pins & 4) wr32(s.tlmm.va, kIrqGpio * TB_GPIO_STRIDE, pinCfg(0, kPullUp, 3, false));
    if (pins & 16) {
        static const unsigned probe[6] = {kResetGpio, kIrqGpio, kPinMiso, kPinMosi, kPinClk, kPinCs};
        for (unsigned i = 0; i < 6; ++i) native_fuxi_touch_pins[i] = rd32(s.tlmm.va, probe[i] * TB_GPIO_STRIDE);
        __asm__ volatile("dmb ish" ::: "memory");
        native_fuxi_touch_pins_n = 6;
    }
    return true;
}

uint16_t rd16le(const volatile uint8_t *p) {return static_cast<uint16_t>(p[0] | (p[1] << 8));}
uint32_t rd32le(const volatile uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
// cmd-db layout (drivers/soc/qcom/cmd-db.c): header {le32 version; u8 magic[4]; rsc_hdr[8]; le32 checksum;
// le32 reserved; u8 data[]}; rsc_hdr is 16 bytes, entry_header 24 bytes with an 8-byte zero-padded id.
bool cmdDbFind(const volatile uint8_t *base, const char *name, uint32_t &addr) {
    static const uint8_t magic[4] = {0xdb, 0x30, 0x03, 0x0c};
    for (unsigned i = 0; i < 4; ++i) if (base[4 + i] != magic[i]) return false;
    const unsigned len = static_cast<unsigned>(strlen(name));
    if (!len || len > 8) return false;
    const volatile uint8_t *data = base + 144;
    for (unsigned h = 0; h < 8; ++h) {
        const volatile uint8_t *hdr = base + 8 + h * 16;
        if (!rd16le(hdr)) break;   // cmd_db_get_header stops at the first empty slave header
        const unsigned hdrOffset = rd16le(hdr + 2), count = rd16le(hdr + 6);
        for (unsigned e = 0; e < count; ++e) {
            const volatile uint8_t *ent = data + hdrOffset + e * 24;
            bool match = true;
            for (unsigned k = 0; k < 8 && match; ++k)
                if (ent[k] != (k < len ? static_cast<uint8_t>(name[k]) : 0)) match = false;
            if (match) {addr = rd32le(ent + 16); return true;}
        }
    }
    return false;
}
bool railAddrOk(uint32_t addr) {return addr && addr != 0xffffffffu && addr < 0x80000u && !(addr & 3u);}

// Vote one VRM resource (voltage, then enable) on the active TCS set and wait for the RSC completion bit.
// RSC_DRV_STATUS is non-zero while a TCS is idle; never program a block that still looks busy.
int rpmhEnableVrm(Session &s, uint32_t resource, uint32_t millivolts) {
    unsigned tcs = kActiveTcsCount;
    for (unsigned i = 0; i < kActiveTcsCount; ++i)
        if (rd32(s.rsc.va, kRscTcsOffset + i * kRscTcsStride + s.regs.status)) {tcs = i; break;}
    if (tcs == kActiveTcsCount) return -16;
    const uintptr_t t = s.rsc.va + kRscTcsOffset + tcs * kRscTcsStride;
    const uintptr_t tb = s.rsc.va + kRscTcsOffset;
    wr32(t, s.regs.cmdEnable, 0);
    wr32(t, s.regs.wait, 0);
    uint32_t control = rd32(t, s.regs.control);
    wr32(t, s.regs.control, control & ~(1u << 24));   // HW order: drop the trigger, then AMC mode
    wr32(t, s.regs.control, control & ~((1u << 24) | (1u << 16)));
    const uint32_t msgId = 8u | (1u << 16) | (1u << 8);   // 8-byte payload, write, response required
    const uint32_t cmdAddr[2] = {resource, resource + 4};  // VRM_VOLTAGE, VRM_ENABLE
    const uint32_t cmdData[2] = {millivolts, 1u};
    for (unsigned i = 0; i < 2; ++i) {
        wr32(t, s.regs.msgId + i * s.regs.cmdStride, msgId);
        wr32(t, s.regs.cmdAddr + i * s.regs.cmdStride, cmdAddr[i]);
        wr32(t, s.regs.cmdData + i * s.regs.cmdStride, cmdData[i]);
    }
    wr32(t, s.regs.wait, 3u);
    wr32(t, s.regs.cmdEnable, 3u);
    wr32(tb, 0x00, rd32(tb, 0x00) | (1u << tcs));
    wr32(tb, 0x08, 1u << tcs);
    wr32(t, s.regs.control, 1u << 16);                     // AMC mode enable
    wr32(t, s.regs.control, (1u << 16) | (1u << 24));      // trigger
    int rc = GENI_ERR_TIMEOUT;
    for (unsigned i = 0; i < 5000; ++i) {
        if (rd32(tb, 0x04) & (1u << tcs)) {rc = 0; break;}
        IODelay(100);
    }
    s.pwrTcs = tcs;
    s.pwrIrqStatus = rd32(tb, 0x04);
    s.pwrCmdStatus = rd32(t, s.regs.cmdStatus);
    s.pwrTcsStatus = rd32(t, s.regs.status);
    control = rd32(t, s.regs.control);
    wr32(t, s.regs.control, control & ~(1u << 24));
    wr32(t, s.regs.control, control & ~((1u << 24) | (1u << 16)));
    wr32(t, s.regs.cmdEnable, 0);
    wr32(t, s.regs.wait, 0);
    wr32(tb, 0x08, 1u << tcs);
    return rc;
}

// fuxi fts power-on: reset low, vdd 1.8 V, 1 ms, avdd 3.2 V, 5 ms, leaving reset low for resetChip.
bool powerOn(Session &s) {
    unsigned mode = 1;
    if (PE_parse_boot_argn("fuxi-touch-power", &mode, sizeof mode) && mode == 0) return true;
    mark(s, 5);
    if (!s.cmddb.va && !mapRegion(kCmdDbBase, kCmdDbSize, s.cmddb)) {s.pwrRc[0] = -12; return false;}
    if (!s.rsc.va && !mapRegion(kRscDrv2Base, 0x10000, s.rsc)) {s.pwrRc[0] = -13; return false;}
    const volatile uint8_t *db = reinterpret_cast<const volatile uint8_t *>(s.cmddb.va);
    s.rscId = rd32(s.rsc.va, 0);
    if (cmdDbFind(db, "ldob12", s.railAddr[0])) s.cmdDbOk |= 1;
    if (cmdDbFind(db, "ldom6", s.railAddr[1])) s.cmdDbOk |= 2;
    const uint32_t major = (s.rscId >> 16) & 0xffu;
    if (major >= 3) s.regs = RscRegs{0x20, 0x24, 0x28, 0x2c, 0x34, 0x38, 0x3c, 0x40, 24};
    else s.regs = RscRegs{0x10, 0x14, 0x18, 0x1c, 0x30, 0x34, 0x38, 0x3c, 20};
    if (mode == 2) {   // read-only: publish the idle/busy state of the three active TCS blocks
        s.pwrCmdStatus = rd32(s.rsc.va, kRscTcsOffset + s.regs.status) |
            (rd32(s.rsc.va, kRscTcsOffset + kRscTcsStride + s.regs.status) << 8) |
            (rd32(s.rsc.va, kRscTcsOffset + 2 * kRscTcsStride + s.regs.status) << 16);
        return true;
    }
    if (s.cmdDbOk != 3 || !railAddrOk(s.railAddr[0]) || !railAddrOk(s.railAddr[1]) ||
        ((s.railAddr[0] ^ s.railAddr[1]) & 0x70000u) || major < 2 || major > 4) {
        s.pwrRc[0] = -19;
        return false;
    }
    pinOut(s.tlmm.va, kResetGpio, false);
    s.pwrRc[0] = rpmhEnableVrm(s, s.railAddr[0], 1800);
    if (s.pwrRc[0]) return false;
    IODelay(1000);
    s.pwrRc[1] = rpmhEnableVrm(s, s.railAddr[1], 3200);
    if (s.pwrRc[1]) return false;
    IODelay(5000);
    return true;
}

bool snapshot(Session &s) {
    mark(s, 1);
    if (!mapRegion(TB_GCC_BASE, 0x1000, s.gccLow) || !mapRegion(TB_GCC_BASE + 0x18000, 0x1000, s.gccS4) ||
        !mapRegion(TB_GCC_BASE + 0x23000, 0x1000, s.gccHalt) || !mapRegion(TB_GCC_BASE + 0x52000, 0x1000, s.gccVote) ||
        !mapRegion(kTlmmBase, (kPinCs + 1) * TB_GPIO_STRIDE, s.tlmm)) {
        fail(1, -12);
        return false;
    }
    for (unsigned l = 0; l < TB_DFS_LEVELS; ++l) {
        uint32_t cfg = rd32(s.gccS4.va, (TB_S4_RCG + TB_RCG_PERF_CFG(l)) & 0xfff);
        uint32_t m = rd32(s.gccS4.va, (TB_S4_RCG + TB_RCG_PERF_M(l)) & 0xfff);
        uint32_t n = rd32(s.gccS4.va, (TB_S4_RCG + TB_RCG_PERF_N(l)) & 0xfff);
        s.levelHz[l] = tb_dfs_level_hz(cfg, m, n);
    }
    s.vals[0] = rd32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfff);
    s.vals[4] = rd32(s.gccHalt.va, TB_HALT_M_AHB & 0xfff);
    s.vals[5] = rd32(s.gccHalt.va, TB_HALT_S_AHB & 0xfff);
    s.vals[6] = rd32(s.gccHalt.va, TB_HALT_CORE & 0xfff);
    s.vals[7] = rd32(s.gccHalt.va, TB_HALT_CORE_2X & 0xfff);
    return true;
}

bool enable(Session &s) {
    unsigned hw = 7;   // bit0 GPLL0 enable, bit1 QUP vote + halt poll, bit2 pin mux
    if (!PE_parse_boot_argn("fuxi-touch-hw", &hw, sizeof hw)) hw = 7;
    mark(s, 2);
    if (hw & 1) {
        wr32(s.gccVote.va, TB_GCC_GPLL0_ENABLE & 0xfff, rd32(s.gccVote.va, TB_GCC_GPLL0_ENABLE & 0xfff) | 1u);
        for (unsigned i = 0; i < 10000 && !(rd32(s.gccLow.va, TB_GCC_GPLL0_MODE) & (1u << 31)); ++i) IODelay(10);
    }
    auto running = [](uint32_t cbcr) {return !(cbcr & TB_CBCR_CLK_OFF) || (cbcr & 2u);};
    bool ok = false;
    if (hw & 2) {
        wr32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfff, rd32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfff) | TB_VOTE_ALL);
        for (unsigned i = 0; i < 2000 && !ok; ++i) {
            ok = running(rd32(s.gccHalt.va, TB_HALT_M_AHB & 0xfff)) && running(rd32(s.gccHalt.va, TB_HALT_S_AHB & 0xfff)) &&
                 !(rd32(s.gccHalt.va, TB_HALT_CORE & 0xfff) & TB_CBCR_CLK_OFF) &&
                 !(rd32(s.gccHalt.va, TB_HALT_CORE_2X & 0xfff) & TB_CBCR_CLK_OFF) &&
                 !(rd32(s.gccS4.va, TB_HALT_S4 & 0xfff) & TB_CBCR_CLK_OFF);
            if (!ok) IODelay(10);
        }
    }
    s.vals[0] = rd32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfff);
    s.vals[4] = rd32(s.gccHalt.va, TB_HALT_M_AHB & 0xfff);
    s.vals[5] = rd32(s.gccHalt.va, TB_HALT_S_AHB & 0xfff);
    s.vals[6] = rd32(s.gccHalt.va, TB_HALT_CORE & 0xfff);
    s.vals[7] = rd32(s.gccHalt.va, TB_HALT_CORE_2X & 0xfff);
    if ((hw & 2) && !ok) {fail(2, -110); return false;}
    if (hw & 4) configurePins(s);
    mark(s, 3);
    return true;
}

bool prepareDma(Session &s) {
    mark(s, 41);
    s.dmaMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),
                                                               2 * kDmaBytes, 0xfffffff000ULL);
    if (!s.dmaMem || s.dmaMem->prepare() != kIOReturnSuccess) {fail(41, -12); return false;}
    mark(s, 42);
    addr64_t segLen = 0;
    s.dmaPhys = s.dmaMem->getPhysicalSegment(0, &segLen);
    uint8_t *virt = static_cast<uint8_t *>(s.dmaMem->getBytesNoCopy());
    if (!virt || !s.dmaPhys || segLen < 2 * kDmaBytes) {fail(42, -14); return false;}
    memset(virt, 0, 2 * kDmaBytes);
    cacheRange(virt, 2 * kDmaBytes, 1);
    if (!mapRegion(TB_APPS_SMMU_BASE, TB_APPS_SMMU_SIZE, s.smmu)) {fail(43, -12); return false;}
    mark(s, 43);
    struct smmu_io io = {reinterpret_cast<void *>(s.smmu.va), smmuRd, smmuWr};
    struct smmu_plan plan = smmu_plan_bypass(&io, TB_QUP_SMMU_SID, s.dmaPhys, 2 * kDmaBytes);
    s.smmuSlot = static_cast<int>(plan.slot); s.smmuBank = static_cast<int>(plan.bank);
    if (plan.error) {s.smmuErr = smmuErrorCode(plan.error); fail(43, -100 - s.smmuErr); return false;}
    const char *e = smmu_configure_bypass(&io, &plan, TB_QUP_SMMU_SID);
    if (e) {s.smmuErr = smmuErrorCode(e); fail(44, -100 - s.smmuErr); return false;}
    s.spi.use_dma = 1;
    s.spi.dma.ctx = nullptr;
    s.spi.dma.clean = dmaClean;
    s.spi.dma.clean_inval = dmaCleanInval;
    s.spi.dma.inval = dmaInval;
    s.spi.dma.tx_virt = virt;
    s.spi.dma.rx_virt = virt + kDmaBytes;
    s.spi.dma.tx_phys = s.dmaPhys;
    s.spi.dma.rx_phys = s.dmaPhys + kDmaBytes;
    s.spi.dma.size = kDmaBytes;
    return true;
}

bool openBus(Session &s) {
    mark(s, 4);
    s.spi.poll_limit = 400000;
    s.spi.poll_only = 2;
    if (!mapRegion(TB_QUP_WRAPPER_BASE, 0x2000, s.wrapper) || !mapRegion(TB_SE_BASE, TB_SE_SIZE, s.se)) {fail(4, -12); return false;}
    s.spi.se = (struct geni_io){reinterpret_cast<void *>(s.se.va), ioRd, ioWr, ioDelayUs};
    s.spi.wrapper = (struct geni_io){reinterpret_cast<void *>(s.wrapper.va), ioRd, ioWr, ioDelayUs};
    mark(s, 31);
    int r = geni_spi_read_info(&s.spi);
    if (r) {fail(4, r); return false;}
    if (s.spi.info.fifo_disabled && !prepareDma(s)) return false;
    r = geni_spi_init(&s.spi);
    if (r) {fail(4, r); return false;}
    return true;
}

bool configureClock(Session &s) {
    struct tb_clock_choice choice = {};
    if (tb_choose_clock(s.levelHz, TB_DFS_LEVELS, s.spi.oversampling, kSpiHz, &choice)) {fail(5, -22); return false;}
    s.level = choice.level;
    s.div = choice.div;
    s.actualHz = choice.actual_hz;
    int r = geni_spi_set_clock(&s.spi, kSpiHz, static_cast<unsigned>(choice.sclk_hz), choice.level);
    if (r) {fail(5, r); return false;}
    r = geni_spi_setup(&s.spi, 0, 0);
    if (r) {fail(5, r); return false;}
    return true;
}

int scanOn(Session &s) {
    static const uint8_t cmd[3] = {kCmdScan, 0x00, 0x01};
    const int rc = busWrite(s, cmd, sizeof cmd);
    native_fuxi_touch_dbg[2] = static_cast<uint32_t>(rc);
    return rc;
}

// fts_system_reset in fts.c: assert reset low for 10 ms, release high, then poll the FIFO for
// CONTROLLER_READY (0x03) for GENERAL_TIMEOUT (100 ms), three attempts. The line must stay high
// afterwards; holding it low keeps the IC in reset and also drags IRQ low.
int resetChip(Session &s) {
    unsigned useReset = 1;
    if (PE_parse_boot_argn("fuxi-touch-reset", &useReset, sizeof useReset) && useReset == 0) return 0;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        pinOut(s.tlmm.va, kResetGpio, false);
        IODelay(10000);
        pinOut(s.tlmm.va, kResetGpio, true);
        for (unsigned i = 0; i < 50; ++i) {
            uint8_t ev[kEventSize];
            const int rc = readEvents(s, 1, ev);
            if (rc) return rc;
            ++s.reads;
            if (ev[0] != 0x00) ++s.events;
            if (ev[0] == kEvReady) return 0;
            if (ev[0] == kEvError) ++s.errors;
            IODelay(2000);
        }
    }
    return GENI_ERR_TIMEOUT;
}

int readSysinfo(Session &s) {
    static const uint8_t cmd[3] = {kCmdSysinfo, 0x00, 0x00};
    uint8_t d[kSysinfoLen];
    const int rc = busRead(s, cmd, sizeof cmd, d, sizeof d);
    native_fuxi_touch_dbg[13] = static_cast<uint32_t>(rc);
    if (rc) return rc;
    s.sig = d[0];
    native_fuxi_touch_dbg[0] = static_cast<uint32_t>(d[0]) | (static_cast<uint32_t>(d[1]) << 8) |
                               (static_cast<uint32_t>(d[2]) << 16) | (static_cast<uint32_t>(d[3]) << 24);
    if (d[0] != 0xa5) return 0;
    s.chipId = le16(d + 10);
    s.fwVer = le16(d + 16);
    s.cfgVer = le16(d + 20);
    s.panelX = le16(d + 80);
    s.panelY = le16(d + 82);
    native_fuxi_touch_dbg[1] = s.chipId | (static_cast<uint32_t>(s.fwVer) << 16);
    return 0;
}

int scaleX(int v) {
    if (v < 0) v = 0;
    if (v > 10799) v = 10799;
    return v * 30479 / 10799;
}
int scaleY(int v) {
    if (v < 0) v = 0;
    if (v > 23999) v = 23999;
    return v * 20319 / 23999;
}
void publishSlots(Session &s) {
    if (!s.dirty) return;
    int ids[kMaxFingers], xs[kMaxFingers], ys[kMaxFingers];
    unsigned n = 0;
    for (unsigned id = 0; id < kMaxFingers; ++id) {
        if (!s.active[id]) continue;
        ids[n] = static_cast<int>(id);
        xs[n] = scaleX(s.cx[id]);
        ys[n] = scaleY(s.cy[id]);
        ++n;
    }
    native_touch_contacts = n;
    native_touch_frames = native_touch_frames + 1;
    if (n) {native_touch_last_x = xs[0]; native_touch_last_y = ys[0];}
    native_touch_publish(ids, xs, ys, n);
    s.dirty = false;
}

void handleEvent(Session &s, const uint8_t *ev) {
    const unsigned id = ev[1] >> 4;
    switch (ev[0]) {
    case kEvEnter:
    case kEvMotion:
        if (id >= kMaxFingers) break;
        s.active[id] = true;
        s.cx[id] = le16(ev + 2);
        s.cy[id] = le16(ev + 4);
        s.dirty = true;
        if (ev[0] == kEvEnter) ++s.enters; else ++s.motions;
        break;
    case kEvLeave:
        if (id >= kMaxFingers) break;
        if (s.active[id]) s.dirty = true;
        s.active[id] = false;
        ++s.leaves;
        break;
    case kEvReady:   // controller restarted on its own: scanning is off again
        for (unsigned i = 0; i < kMaxFingers; ++i) {
            if (s.active[i]) {s.active[i] = false; s.dirty = true;}
        }
        ++s.readys;
        scanOn(s);
        break;
    case kEvError:
        ++s.errors;
        break;
    default:
        ++s.bad;
        break;
    }
}

void drain(Session &s) {
    for (unsigned round = 0; round < 4; ++round) {
        uint8_t ev[kEventSize];
        int rc = readEvents(s, 1, ev);
        if (rc) {fail(10, rc); return;}
        ++s.reads;
        if (ev[0] == 0x00) break;
        handleEvent(s, ev);
        ++s.events;
        unsigned remaining = ev[7] & 0x1f;
        if (remaining > kFifoDepth - 1) remaining = kFifoDepth - 1;
        if (remaining > s.fifo) s.fifo = remaining;
        if (!remaining) continue;
        uint8_t batch[kFifoDepth * kEventSize];
        rc = readEvents(s, remaining, batch);
        if (rc) {fail(10, rc); return;}
        ++s.reads;
        for (unsigned i = 0; i < remaining; ++i) {
            const uint8_t *e = batch + i * kEventSize;
            if (e[0] == 0x00) break;
            handleEvent(s, e);
            ++s.events;
        }
    }
    publishSlots(s);
}

void touchThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "fuxi-ftm5");
    Session *s = static_cast<Session *>(IOMallocType(Session));
    if (!s) {fail(0, -12); return;}
    bzero(s, sizeof *s);
    auto publishDbg = [s]() {
        if (!s->tlmm.va) return;
        native_fuxi_touch_dbg[3] = rd32(s->tlmm.va, kIrqGpio * TB_GPIO_STRIDE + 4);
        native_fuxi_touch_dbg[4] = rd32(s->tlmm.va, kResetGpio * TB_GPIO_STRIDE + 4);
        native_fuxi_touch_dbg[5] = static_cast<uint32_t>(s->lastRc);
        native_fuxi_touch_dbg[6] = s->lastStatus;
        native_fuxi_touch_dbg[7] = s->lastTx;
        native_fuxi_touch_dbg[8] = s->lastRx;
        native_fuxi_touch_dbg[9] = s->reads;
        native_fuxi_touch_dbg[10] = s->events;
        native_fuxi_touch_dbg[11] = (s->errors << 16) | (s->bad & 0xffff);
        native_fuxi_touch_dbg[12] = static_cast<uint32_t>(s->sig) | (static_cast<uint32_t>(s->fifo) << 8);
        native_fuxi_touch_dbg[16] = (static_cast<uint32_t>(s->pwrRc[0]) & 0xffffu) |
                                    (static_cast<uint32_t>(s->pwrRc[1]) << 16);
        native_fuxi_touch_dbg[17] = s->cmdDbOk | (s->pwrTcs << 8);
        native_fuxi_touch_dbg[18] = (native_hid_stage & 0xffu) | ((native_hid_input_events[4] & 0xffu) << 8) |
            ((native_hid_input_events[2] & 0xffu) << 16) | ((native_hid_input_events[6] & 0xffu) << 24);
        native_fuxi_touch_dbg[19] = ((uint32_t)native_hid_load_status & 0xffffu) << 16 |
            ((uint32_t)native_hid_input_results[6] & 0xffffu);
        native_fuxi_touch_dbg[20] = s->rscId;
        native_fuxi_touch_dbg[21] = s->pwrIrqStatus;
        native_fuxi_touch_dbg[22] = s->pwrCmdStatus;
        native_fuxi_touch_dbg[23] = s->pwrTcsStatus;
    };
    unsigned pollMs = 4;
    if (!PE_parse_boot_argn("fuxi-touch-poll", &pollMs, sizeof pollMs) || pollMs < 1 || pollMs > 50) pollMs = 4;
    s->pollMs = pollMs;
    unsigned stopStage = 9;
    if (!PE_parse_boot_argn("fuxi-touch-stage", &stopStage, sizeof stopStage) || stopStage < 1) stopStage = 9;
    do {
        if (!snapshot(*s)) break;
        native_touch_stage_done = 1;
        rows(*s);
        if (stopStage <= 1) break;
        if (!enable(*s)) break;
        native_touch_stage_done = 2;
        rows(*s);
        if (stopStage <= 2) break;
        if (!openBus(*s) || !configureClock(*s)) break;
        native_touch_stage_done = 3;
        mark(*s, 6);
        if (stopStage <= 3) break;
        powerOn(*s);   // a failure is published through pwrRc; reset/sysinfo still show what the chip does
        publishDbg();
        int rc = resetChip(*s);
        if (rc) {fail(6, rc); break;}
        mark(*s, 7);
        rc = readSysinfo(*s);
        if (rc) {fail(7, rc); break;}
        mark(*s, 8);
        rc = scanOn(*s);
        if (rc) {fail(8, rc); break;}
        native_touch_stage_done = 4;
        rows(*s);
        publishDbg();
        unsigned tick = 0;
        for (;;) {
            IOSleep(s->pollMs);
            drain(*s);
            publishDbg();
            if ((++tick & 255) == 0) rows(*s);
        }
    } while (0);
    publishDbg();
    rows(*s);
}
}  // namespace

extern "C" void fuxi_touch_probe(void) {
    static int started;
    unsigned enabled = 1;
    if (PE_parse_boot_argn("fuxi-touch", &enabled, sizeof enabled) && enabled == 0) return;
    if (started) return;
    started = 1;
    unsigned hid = 1;
    native_fuxi_touch_enabled = !PE_parse_boot_argn("fuxi-touch-hid", &hid, sizeof hid) || hid != 0;
    unsigned pad = 1;
    native_fuxi_touch_pad = !PE_parse_boot_argn("fuxi-touch-pad", &pad, sizeof pad) || pad != 0;
    native_touch_stage_req = 4;
    thread_t thread = nullptr;
    if (kernel_thread_start(touchThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else {native_fuxi_touch_enabled = 0; native_touch_error = -12;}
}
