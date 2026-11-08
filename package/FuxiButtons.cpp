// SPDX-License-Identifier: BSD-2-Clause-Patent
// Taken from SadNutWall/unixnu-fuxi package/FuxiButtons.cpp, including its short-tap fix (b529c30): the interrupt handler latches the
// edges of its own keys, and with nothing held any more the thread reports the latched edge as a complete press/release pair held for
// kMinPressMs. Both halves are kept: without the latch a tap shorter than the wake-up latency never reaches HID.
//
// Power and volume buttons of the fuxi board (boot argument fuxi-buttons=1), sent as the Consumer Power, Volume Increment and Volume
// Decrement bits of the HID input device's report 2 (drivers/ShengHidInput).
// The devicetree facts: power is the PMK8550 PON (SPMI SID 0, peripheral 0x13, interrupt 7 "pwrkey"), volume down its RESIN
// (interrupt 6), volume up the PM8550 GPIO 6 (SID 1, peripheral 0x8d, active low with a pull-up, gpio-keys). All three are PMIC
// peripheral interrupts the SPMI arbiter (spmi@c42d000, EE 0) gathers into its "periph_irq", PDC pin 1, which the PDC forwards as
// SPI 481 (qcom,pdc-ranges 0 -> 480). Register layout as the mainline spmi-pmic-arb (version 7) and qcom-pdc (version 3.2: the
// enable bit sits in IRQ_i_CFG) drivers use it:
// per APID in the "chnls" window +0x100 ACC_ENABLE, +0x104 IRQ_STATUS, +0x108 IRQ_CLEAR; the owner's accumulated status words at "intr" +
// 0x1000 * EE + 4 * word; in the peripheral 0x10 RT_STS, 0x11 SET_TYPE, 0x12 POLARITY_HIGH, 0x13 POLARITY_LOW, 0x14 LATCHED_CLR, 0x15 EN_SET.
// The interrupt handler acknowledges, latches the edges it saw and wakes one thread; the thread reports the live levels, or a latched
// edge when nothing is held any more. A tap shorter than the wake-up latency is up again before the levels are read, so without the
// latch only a long press would reach HID. A latched press is reported as a complete pair and held for kMinPressMs.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "BootResetCore.h"
}

extern "C" int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *) __attribute__((weak_import));
// [0] stage (1 mapped, 2 configured, 3 armed, -9 masked after an interrupt storm, other negatives: the step that failed), [1] interrupts, [2] reports, [3] pressed mask,
// [4] power APID, [5] GPIO APID, [6] interrupts of other APIDs cleared, [7] PDC version, [8] last error, [9] spurious interrupts
extern "C" {volatile int32_t fuxi_buttons_stats[10];}
/* What the two peripherals actually report, accumulated so a press cannot be missed between samples: [0] every status bit the
 * PON has ever raised, [1] its last one, [2] the same two for the GPIO APID, [4] every report-bit set the handler produced.
 * The key bits are the ones this board really asserts, not the ones the devicetree promised. */
extern "C" {volatile int32_t fuxi_buttons_edges[6];}

namespace {
constexpr uint64_t kCore = 0xc400000, kObsrvr = 0xc440000, kChnls = 0xc500000, kIntr = 0xc4c0000, kCnfg = 0xc42d000, kPdc = 0xb220000;
constexpr uint32_t kPonPpid = 0x013, kGpioPpid = 0x18d;
constexpr uint8_t kPowerBit = 1u << 7, kResinBit = 1u << 6, kGpioBit = 1u << 0;
constexpr uint32_t kAccEnable = 0x100, kIrqStatus = 0x104, kIrqClear = 0x108;
constexpr uint8_t kRtSts = 0x10, kSetType = 0x11, kPolHigh = 0x12, kPolLow = 0x13, kLatchedClr = 0x14, kEnSet = 0x15;
constexpr uint32_t kPdcPin = 1, kSpi = 481, kPdcVersion = 0x1000, kPdcCfg = 0x110, kPdcEnableBank = 0x10;
// Report 2 bits (NativeHidInput): 1 Power, 2 Volume Increment, 3 Volume Decrement.
constexpr uint32_t kReportPower = 1u << 1, kReportVolumeUp = 1u << 2, kReportVolumeDown = 1u << 3;
// How long a reported press stays down before its release, however briefly the key was touched.
constexpr uint64_t kMinPressMs = 60;

struct Window {IOMemoryMap *map; volatile uint32_t *va;};
Window gCore, gCnfg, gObs, gChn, gIntr, gPdc;
br_arbiter gArb;
int gPon = -1, gGpio = -1;
uint32_t gApids;
volatile uint32_t gEvents;
volatile uint32_t gLatched;
uint64_t gTicksPerMs, gMinPressTicks;
void (*volatile gSink)(void *, uint32_t);
void *volatile gSinkContext;

bool mapWindow(Window &w, uint64_t base, uint64_t size) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}
volatile uint32_t *apidReg(int apid, uint32_t reg) {return gChn.va + (BR_CHNL_APID_STRIDE * (uint32_t)apid + reg) / 4;}
void fail(int step, int error) {fuxi_buttons_stats[8] = error; fuxi_buttons_stats[0] = -step;}

// The physical generic counter: it runs through the sleep in the thread below and needs no timer driver handle.
uint64_t tickNow() {
    uint64_t value;
    __asm__ volatile("isb sy" ::: "memory");
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(value));
    return value;
}

// The report 2 bits the edges of this status stand for. A bit is set by either edge, so the press and its release both arrive here.
uint32_t reportBits(int apid, uint32_t status) {
    uint32_t bits = 0;
    if (apid == gPon) {
        if (status & kPowerBit) bits |= kReportPower;
        if (status & kResinBit) bits |= kReportVolumeDown;
    }
    if (apid == gGpio && (status & kGpioBit)) bits |= kReportVolumeUp;
    return bits;
}

// The APID that serves ppid with its interrupts owned by this EE, as the mainline driver chooses it; -1 when there is none.
int findApid(uint32_t ppid) {
    int found = -1;
    for (uint32_t i = 0; i < gApids; ++i) {
        uint32_t map = gCore.va[(BR_APID_MAP + 4 * i) / 4];
        if (!map || ((map >> 8) & 0xfff) != ppid) continue;
        const bool irqOwner = (map >> 24) & 1;
        if (irqOwner && (gCnfg.va[(BR_CNFG_OWNER + 4 * i) / 4] & 7u) == gArb.ee) found = (int)i;
    }
    return found;
}
// Both edges of the given interrupt bits, latched state cleared, then enabled in the peripheral.
int armPeripheral(int apid, uint8_t bits) {
    uint8_t type = 0, high = 0, low = 0;
    int rc = br_read8(gObs.va, &gArb, apid, kSetType, &type);
    if (!rc) rc = br_read8(gObs.va, &gArb, apid, kPolHigh, &high);
    if (!rc) rc = br_read8(gObs.va, &gArb, apid, kPolLow, &low);
    if (!rc) rc = br_write8(gChn.va, apid, kSetType, type | bits);
    if (!rc) rc = br_write8(gChn.va, apid, kPolHigh, high | bits);
    if (!rc) rc = br_write8(gChn.va, apid, kPolLow, low | bits);
    if (!rc) rc = br_write8(gChn.va, apid, kLatchedClr, bits);
    if (!rc) rc = br_write8(gChn.va, apid, kEnSet, bits);
    if (!rc) *apidReg(apid, kAccEnable) = 1;
    return rc;
}

// Primary interrupt context: acknowledge in the arbiter and the peripheral, record which of our keys it was, then wake the thread. An APID
// that is not ours but raised the line only gets its arbiter status cleared (mainline cleanup_irq): reading another APID's registers can
// end in a synchronous external abort (int-227 panicked reading ACC_ENABLE of APIDs it did not own).
void buttonsIrq(void *) {
    bool ours = false;
    for (uint32_t word = 0; word * 32 < gApids; ++word) {
        uint32_t pending = gIntr.va[word];
        while (pending) {
            const int apid = (int)(word * 32 + __builtin_ctz(pending));
            pending &= pending - 1;
            if (apid != gPon && apid != gGpio) {
                *apidReg(apid, kIrqClear) = 0xff;
                fuxi_buttons_stats[6] = fuxi_buttons_stats[6] + 1;
                continue;
            }
            const uint32_t status = *apidReg(apid, kIrqStatus);
            *apidReg(apid, kIrqClear) = status;
            if (status) {
                br_write8(gChn.va, apid, kLatchedClr, (uint8_t)status);
                const uint32_t bits = reportBits(apid, status);
                if (apid == gPon) {__atomic_fetch_or(&fuxi_buttons_edges[0], (int32_t)status, __ATOMIC_RELEASE);
                                   fuxi_buttons_edges[1] = (int32_t)status;}
                else {__atomic_fetch_or(&fuxi_buttons_edges[2], (int32_t)status, __ATOMIC_RELEASE);
                      fuxi_buttons_edges[3] = (int32_t)status;}
                __atomic_fetch_or(&fuxi_buttons_edges[4], (int32_t)bits, __ATOMIC_RELEASE);
                __atomic_fetch_or(&gLatched, bits, __ATOMIC_RELEASE);
            }
            ours = true;
        }
    }
    __asm__ volatile("dsb sy" ::: "memory");
    // A line that keeps firing without one of our peripherals behind it would hold this core in the handler: after 1000 such interrupts
    // in a row the PDC pin is masked, which drops the line, and stage -9 records it.
    static uint32_t idle;
    if (!ours) {
        fuxi_buttons_stats[9] = fuxi_buttons_stats[9] + 1;
        if (++idle >= 1000) {
            gPdc.va[(kPdcCfg + 4 * kPdcPin) / 4] = 4u;
            __asm__ volatile("dsb sy" ::: "memory");
            fuxi_buttons_stats[0] = -9;
        }
        return;
    }
    idle = 0;
    fuxi_buttons_stats[1] = fuxi_buttons_stats[1] + 1;
    __atomic_fetch_add(&gEvents, 1, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&gEvents);
}

uint32_t pressed() {
    uint8_t pon = 0, gpio = 0xff;
    uint32_t mask = 0;
    if (!br_read8(gObs.va, &gArb, gPon, kRtSts, &pon)) {
        if (pon & kPowerBit) mask |= kReportPower;
        if (pon & kResinBit) mask |= kReportVolumeDown;
    }
    if (gGpio >= 0 && !br_read8(gObs.va, &gArb, gGpio, kRtSts, &gpio) && !(gpio & kGpioBit)) mask |= kReportVolumeUp;
    return mask;
}

void report(uint32_t mask) {
    fuxi_buttons_stats[3] = (int32_t)mask;
    void (*sink)(void *, uint32_t) = __atomic_load_n(&gSink, __ATOMIC_ACQUIRE);
    if (sink) {sink(gSinkContext, mask); fuxi_buttons_stats[2] = fuxi_buttons_stats[2] + 1;}
}

bool setup() {
    // A frequency of zero would only mean a board without the generic timer; the minimum press then simply is not held.
    uint64_t frequency = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    gTicksPerMs = frequency / 1000;
    gMinPressTicks = gTicksPerMs * kMinPressMs;
    gArb.ee = 0;
    if (!mapWindow(gCore, kCore, 0x3000) || !mapWindow(gCnfg, kCnfg, 0x4000) || !mapWindow(gIntr, kIntr, 0x1000) || !mapWindow(gPdc, kPdc, 0x2000)) {
        fail(1, -12); return false;
    }
    gArb.core = gCore.va; gArb.cnfg = gCnfg.va;
    const uint32_t version = gCore.va[0];
    gApids = gCore.va[BR_FEATURES / 4] & BR_FEATURES_APIDS_MASK;
    if (version < BR_VERSION_V7_MIN || version >= BR_VERSION_V8_MIN || !gApids || gApids > 1024) {fail(2, (int)version); return false;}
    if (!mapWindow(gObs, kObsrvr, ((uint64_t)BR_OBS_APID_STRIDE * gApids + 0xfff) & ~0xfffull) ||
        !mapWindow(gChn, kChnls, (uint64_t)BR_CHNL_APID_STRIDE * gApids)) {fail(3, -12); return false;}
    fuxi_buttons_stats[0] = 1;
    gPon = findApid(kPonPpid); gGpio = findApid(kGpioPpid);
    fuxi_buttons_stats[4] = gPon; fuxi_buttons_stats[5] = gGpio;
    if (gPon < 0) {fail(4, -2); return false;}
    int rc = armPeripheral(gPon, kPowerBit | kResinBit);
    if (!rc && gGpio >= 0) rc = armPeripheral(gGpio, kGpioBit);
    if (rc) {fail(5, rc); return false;}
    // PDC pin 1: level high, enabled.
    const uint32_t pdcVersion = gPdc.va[kPdcVersion / 4];
    fuxi_buttons_stats[7] = (int32_t)pdcVersion;
    if (pdcVersion >= 0x30200) {
        gPdc.va[(kPdcCfg + 4 * kPdcPin) / 4] = (1u << 3) | 4u;
    } else {
        gPdc.va[(kPdcCfg + 4 * kPdcPin) / 4] = 4u;
        gPdc.va[(kPdcEnableBank + 4 * (kPdcPin / 32)) / 4] |= 1u << (kPdcPin % 32);
    }
    __asm__ volatile("dsb sy" ::: "memory");
    fuxi_buttons_stats[0] = 2;
    if (!armcpu_spi_attach) {fail(6, -78); return false;}
    // The probe runs before the interrupt controller driver has started (int-228: kIOReturnNotReady at 4 s), so wait for it, up to a minute.
    for (unsigned tries = 0;; ++tries) {
        rc = armcpu_spi_attach(32 + kSpi, false, buttonsIrq, nullptr, nullptr, nullptr);
        if (rc != kIOReturnNotReady || tries == 240) break;
        IOSleep(250);
    }
    if (rc) {fail(6, rc); return false;}
    fuxi_buttons_stats[0] = 3;
    return true;
}

void buttonsThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "fuxi-buttons");
    if (!setup()) return;
    uint32_t seen = gEvents, last = 0;
    uint64_t pressAt = 0;
    for (;;) {
        const uint32_t levels = pressed();
        // A held button reports its own level; with nothing held any more the edges the handler latched are the only trace of a tap.
        const uint32_t latch = (levels || last) ? 0 : __atomic_exchange_n(&gLatched, 0, __ATOMIC_ACQ_REL);
        const uint32_t mask = levels ? levels : (last ? 0 : latch);
        if (mask != last) {
            if (mask) pressAt = tickNow();
            else {
                const uint64_t held = tickNow() - pressAt;
                // The consumers of the power key act on a press of a length, so a shorter pair is stretched rather than dropped.
                if (held < gMinPressTicks) {
                    IOSleep((uint32_t)((gMinPressTicks - held) / gTicksPerMs) + 1);
                    if (pressed()) continue;  // held again during the sleep: the pair stays open
                }
                // The edges of this pair are spent; left latched they would be read back as another tap.
                __atomic_store_n(&gLatched, 0, __ATOMIC_RELEASE);
            }
            last = mask;
            report(mask);
            // A tap is over before it is reported, so run its release now rather than waiting for an edge that came before it.
            if (mask && !levels) continue;
        }
        assert_wait((event_t)&gEvents, THREAD_UNINT);
        if (gEvents != seen) clear_wait(current_thread(), THREAD_AWAKENED);
        else thread_block(THREAD_CONTINUE_NULL);
        seen = gEvents;
    }
}
}  // namespace

extern "C" void native_buttons_set_sink(void (*sink)(void *, uint32_t), void *context) {
    gSinkContext = context;
    __atomic_store_n(&gSink, sink, __ATOMIC_RELEASE);
}

extern "C" void fuxi_buttons_probe(void) {
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("fuxi-buttons", &enabled, sizeof enabled) || enabled != 1 || fuxi_buttons_stats[0]) return;
    fuxi_buttons_stats[0] = 1;
    thread_t thread = nullptr;
    if (kernel_thread_start(buttonsThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else fail(7, -12);
}
