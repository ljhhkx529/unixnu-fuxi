/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Fuxi's modem bring-up: the MPSS is started over PAS, then the GLINK edge and its SMP2P are attached so the
 * QRTR link can come up.
 *
 * The sequence is Linux's (drivers/remoteproc/qcom_q6v5_pas.c, qcom_pas_start), in its order:
 *
 *   1. the rails and the proxy power domains are voted for (cx.lvl, mss.lvl);
 *   2. the modem's device tree blob is authenticated and the modem is reset (PAS 0x26);
 *   3. the modem image is authenticated and the modem is reset (PAS 4);
 *   4. the AOP is told to release the modem (the load_state message);
 *   5. the modem's ready bits are watched over SMP2P.
 *
 * The SCM64 calls, the PAS argument layout, the RPMh votes, the MDT loading and the SMP2P bookkeeping are the
 * device repository's own layers, the same ones the ADSP bring-up already uses and that the ADSP proved on the
 * device. Only the modem's own numbers differ, and they are in FuxiModemScm.h.
 *
 * The success test is strict: the secure world says success with x0 == 0 and x1 == 0. The positive cookies the
 * first ADSP rounds saw (0x00cfffe5 and friends) were the artifact of shifted SMC register arguments, not
 * success values; the ADSP record in docs/fuxi-audio-test-results.md shows that an invalid PAS id returned the
 * same cookie.
 *
 * One kernel thread owns the state machine. Interrupt handlers only acknowledge their hardware, count the event
 * and wake that thread. Every wait has a deadline. */
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <pexpert/pexpert.h>

#include "FuxiModemPas.h"
#include "FuxiModemScm.h"
#include "FuxiAdspFirmware.h"
#include "QcomGlink.h"
#include "QcomMdt.h"
#include "QcomQmp.h"
#include "QcomRpmh.h"
#include "QcomScm.h"
#include "QcomSmem.h"
#include "QcomSmp2p.h"

extern "C" int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *)
    __attribute__((weak_import));
extern int printf(const char *format, ...);

/* The component's board adapter, in the QcomBaseband component. The link and the QRTR layer live there; this
 * file only starts the modem and hands the adapter its SMEM window and its doorbell. */
extern "C" int fuxi_modem_start(const void *config);
extern "C" void fuxi_modem_poll(void);
extern "C" void fuxi_modem_report(unsigned second);

/* The counters the bring-up produces, read back by the USB control channel's diagnostics entry point.
 * The symbol is external so that other half can see it: the channel enumerates long after this file's
 * bring-up has run, and the kernel's own print of the counters is long gone by then. */
extern "C" volatile int32_t native_fuxi_modem_stats[16] = {};

namespace {

/* ---- the board's numbers ---- */

/* The SMEM window and the platform registers, the same ones the ADSP bring-up uses. They are the SM8550's, and
 * the modem shares them: the SMEM region, the TCSR mutex, the IPCC and the AOSS mailbox. */
constexpr uint64_t kSmemBase = 0x81d00000ull;
constexpr uint32_t kSmemSize = 0x200000u;
constexpr uint64_t kTcsrMutex = 0x01f43000ull;
constexpr uint32_t kTcsrMutexSize = 0x10000u;
constexpr uint64_t kIpccBase = 0x00408000ull;
constexpr uint32_t kIpccSize = 0x1000u;
constexpr uint64_t kAossBase = 0x00c30000ull;
constexpr uint32_t kAossSize = 0x400u;
constexpr uint64_t kRscBase = 0x17a20000ull;
constexpr uint32_t kRscSize = 0x10000u;
constexpr uint32_t kRscTcsOffset = 0xd00u;
constexpr uint32_t kRscSpi = 5u;
constexpr uint32_t kIpccSpi = 229u;
constexpr uint64_t kCmdDbBase = 0x81c60000ull;
constexpr uint32_t kCmdDbSize = 0x20000u;

/* The modem's memory holes, from the SoC's device tree (mpss_region@8a800000 and q6_mpss_dtb_region@9b000000).
 * The first is the modem's own memory and the second holds its device tree blob. */
constexpr uint64_t kModemCarveoutBase = 0x8a800000ull;
constexpr uint64_t kModemCarveoutSize = 0x10800000ull;
constexpr uint64_t kModemDtbCarveoutBase = 0x9b000000ull;
constexpr uint64_t kModemDtbCarveoutSize = 0x80000ull;

/* The modem's SMP2P items and its GLINK items. The GLINK items are the ones every GLINK edge uses (Linux
 * drivers/rpmsg/qcom_glink_smem.c): 478 is the 32-byte descriptor, 479 the transmit FIFO and 480 the receive
 * FIFO. The SMP2P items are the modem's own, from the board's device tree (qcom,smp2p-modem). */
constexpr uint16_t kSmp2pOutItem = 0x1ac;
constexpr uint16_t kSmp2pInItem = 0x1b3;
constexpr uint16_t kGlinkDescItem = 478;
constexpr uint16_t kGlinkTxItem = 479;
constexpr uint16_t kGlinkRxItem = 480;
constexpr uint32_t kGlinkFifoBytes = 0x4000;
constexpr uint16_t kModemPid = 1;

/* The modem's GLINK channel and the intent it grants on it. The channel name and the intents are the board's
 * (qcom,modem_qrtr in the device tree). */
constexpr const char *kIpcRtrChannel = "IPCRTR";
constexpr uint32_t kIpcRtrIntentSize = 0x2000;
constexpr uint32_t kIpcRtrIntentCount = 8;

/* The AOP message that releases the modem. Linux's q6v5 sends it with the remoteproc's load_state name, which
 * for this remoteproc is "modem". */
constexpr const char *kLoadStateMessage = "{class: image, res: load_state, name: modem, val: on}";

/* The two proxy rails the modem's power domains need. Their names are the ones Linux's rpmhpd driver looks up in
 * the command database (cx.lvl and mss.lvl for the mss remoteproc). */
constexpr const char *kCxRail = "cx.lvl";
constexpr const char *kMssRail = "mss.lvl";

constexpr uint32_t kSpiIrqBase = 32;
constexpr unsigned kRscTcs = 2;
constexpr uint32_t kImageChunk = 256u * 1024u;

constexpr uint32_t kEvSmp2p = 1u << 0;
constexpr uint32_t kEvGlink = 1u << 1;
constexpr uint32_t kEvAop = 1u << 2;
constexpr uint32_t kEvRsc = 1u << 3;

/* The stages, as the report shows them. */
enum {
    kStIdle = 0,
    kStFirmware = 1,   /* the images are open and parsed */
    kStMapped = 2,     /* the windows are mapped */
    kStRails = 3,      /* the proxy rails are voted */
    kStDtb = 4,        /* the device tree blob is authenticated and the modem is reset */
    kStLoaded = 5,     /* the modem image is loaded and the modem is reset */
    kStLoadState = 6,  /* the AOP was told to release the modem */
    kStReady = 7,      /* the modem's ready bits arrived */
    kStLink = 8,       /* the GLINK edge and its channel are up */
    kStUp = 9,
};

/* ---- the owner's state ---- */

struct Window {
    IOMemoryMap *map;
    volatile uint8_t *va;
    uint64_t size;
};

struct Image {
    void *file;
    uint64_t size;
    QcomMdtImage mdt;
    IOBufferMemoryDescriptor *meta;
    bool reset;
    uint32_t init_x0, mem_x0;
};

struct Owner {
    Window smem_window, mutex_window, ipcc_window, aoss_window, rsc_window;
    QcomSmem smem;
    QcomSmemPartition part;
    QcomSmp2p smp2p;
    QcomQmp qmp;
    QcomGlink glink;
    QcomGlinkChannel *channel;
    QcomRscLayout layout;
    Image dtb, modem;

    uint32_t pending;
    uint32_t rsc_done;
    uint32_t ipcc_irqs, rsc_irqs, smp2p_irqs, glink_irqs, aop_irqs;
    uint32_t ipcc_irq_errors;
    uint32_t stage;
    int last_error;

    uint32_t scm_x0, scm_x1, scm_op, scm_pas;
    uint32_t dtb_auth_cookie, modem_auth_cookie;
    uint32_t loaded_segments;
    uint32_t smp2p_ready, smp2p_fatal, smp2p_handover;
    uint32_t qmp_response;
    uint32_t cx_index, mss_index;
    uint32_t cx_addr, mss_addr;
    uint32_t mss_rail_found;   /* 0: the command database has no mss.lvl */

    bool smem_ready, smem_locked, ipcc_ready, rsc_ready, glink_started, glink_up, fatal, handover;
    bool modem_reset, dtb_reset;
};

Owner gOwner;

void syncStats()
{
    native_fuxi_modem_stats[0] = static_cast<int32_t>(gOwner.stage);
    native_fuxi_modem_stats[1] = gOwner.last_error;
    native_fuxi_modem_stats[2] = static_cast<int32_t>(gOwner.scm_x0);
    native_fuxi_modem_stats[3] = static_cast<int32_t>(gOwner.scm_x1);
    native_fuxi_modem_stats[4] = static_cast<int32_t>(gOwner.smp2p_ready);
    native_fuxi_modem_stats[5] = static_cast<int32_t>(gOwner.smp2p_fatal);
    native_fuxi_modem_stats[6] = static_cast<int32_t>(gOwner.ipcc_irqs);
    native_fuxi_modem_stats[7] = static_cast<int32_t>(gOwner.loaded_segments);
    native_fuxi_modem_stats[8] = static_cast<int32_t>(gOwner.mss_rail_found);
    native_fuxi_modem_stats[9] = static_cast<int32_t>(gOwner.glink_up);
}

void setStage(uint32_t stage)
{
    if (stage > gOwner.stage) gOwner.stage = stage;
    syncStats();
}

/* ---- the secure world ---- */

struct ScmResult {
    int64_t x0;
    uint64_t x1;
};

/* One SMC64 call to the peripheral authentication service. The arguments go in as the service's convention says:
 * x0 the function id, x1 the argument-type word, x2 the PAS id and x3..x5 the call's own arguments. */
ScmResult secureCall(uint32_t command, uint32_t arginfo, uint32_t pas, uint64_t a, uint64_t b, uint64_t c)
{
    ScmResult result = {-1, 0};
    unsigned resumes = 0;
    for (unsigned busy = 0; busy < 20; ++busy) {
        uint64_t function = qcom_scm_fn(QCOM_SCM_SVC_PIL, command), session = 0;
        for (;;) {
            register uint64_t x0 __asm__("x0") = function;
            register uint64_t x1 __asm__("x1") = arginfo;
            register uint64_t x2 __asm__("x2") = pas;
            register uint64_t x3 __asm__("x3") = a;
            register uint64_t x4 __asm__("x4") = b;
            register uint64_t x5 __asm__("x5") = c;
            register uint64_t x6 __asm__("x6") = session;
            register uint64_t x7 __asm__("x7") = 0;
            __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
                             :: "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
            result = {static_cast<int64_t>(x0), x1};
            if (static_cast<int64_t>(x0) != QCOM_SCM_INTERRUPTED || ++resumes > 100000) break;
            function = QCOM_SCM_INTERRUPTED;
            session = x6;
        }
        if (result.x0 != QCOM_SCM_V2_EBUSY) break;
        IOSleep(30);
    }
    gOwner.scm_x0 = static_cast<uint32_t>(result.x0);
    gOwner.scm_x1 = static_cast<uint32_t>(result.x1);
    gOwner.scm_op = command;
    gOwner.scm_pas = pas;
    syncStats();
    return result;
}

void cleanToPoc(const void *start, size_t length)
{
    uint64_t ctr = 0;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const uintptr_t line = static_cast<uintptr_t>(4) << ((ctr >> 16) & 15);
    for (uintptr_t at = reinterpret_cast<uintptr_t>(start) & ~(line - 1);
         at < reinterpret_cast<uintptr_t>(start) + length; at += line)
        __asm__ volatile("dc civac, %0" :: "r"(at) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

/* ---- windows and registers ---- */

bool mapWindow(Window &window, uint64_t base, uint64_t size, IOOptionBits cache = kIOMapInhibitCache)
{
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    window.map = memory ? memory->map(kIOMapAnywhere | cache) : nullptr;
    if (memory) memory->release();
    window.va = window.map ? reinterpret_cast<volatile uint8_t *>(window.map->getVirtualAddress()) : nullptr;
    window.size = window.map ? size : 0;
    return window.va != nullptr;
}

void unmapWindow(Window &window)
{
    if (window.map) window.map->release();
    window = {};
}

uint32_t rd(const Window &window, uint64_t at)
{
    return *reinterpret_cast<volatile uint32_t *>(window.va + at);
}

void wr(const Window &window, uint64_t at, uint32_t value)
{
    *reinterpret_cast<volatile uint32_t *>(window.va + at) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

void wake(uint32_t bits)
{
    __atomic_fetch_or(&gOwner.pending, bits, __ATOMIC_RELEASE);
    thread_wakeup(reinterpret_cast<event_t>(&gOwner.pending));
}

/* The SMEM hardware mutex: the allocation path must hold it. */
bool lockSmem()
{
    for (unsigned attempt = 0; attempt < 1000; ++attempt) {
        wr(gOwner.mutex_window, 0, 1);
        if (rd(gOwner.mutex_window, 0) == 1) {
            gOwner.smem_locked = true;
            return true;
        }
        IODelay(10);
    }
    return false;
}

void unlockSmem()
{
    if (!gOwner.smem_locked) return;
    wr(gOwner.mutex_window, 0, 0);
    gOwner.smem_locked = false;
}

int allocItem(uint16_t item, uint32_t size)
{
    if (!lockSmem()) return -16;
    const int rc = qcom_smem_alloc(&gOwner.smem, &gOwner.part, item, size);
    unlockSmem();
    return rc == QcomSmemExists ? 0 : rc;
}

volatile uint8_t *item(uint16_t id, uint32_t *size)
{
    uint32_t at = 0, bytes = 0;
    if (qcom_smem_find(&gOwner.smem, &gOwner.part, id, &at, &bytes)) return nullptr;
    if (size) *size = bytes;
    return gOwner.smem_window.va + at;
}

/* ---- the doorbells ---- */

void ipccIrq(void *)
{
    uint32_t bits = 0;
    for (unsigned n = 0; n < 32; ++n) {
        const uint32_t id = rd(gOwner.ipcc_window, QCOM_IPCC_RECV_ID);
        if (id == QCOM_IPCC_NONE) break;
        wr(gOwner.ipcc_window, QCOM_IPCC_RECV_CLEAR, id);
        if (id == qcom_ipcc_id(FUXI_IPCC_CLIENT_MPSS, QCOM_IPCC_SIGNAL_SMP2P)) {
            bits |= kEvSmp2p;
            gOwner.smp2p_irqs++;
        } else if (id == qcom_ipcc_id(FUXI_IPCC_CLIENT_MPSS, QCOM_IPCC_SIGNAL_GLINK_QMP)) {
            bits |= kEvGlink;
            gOwner.glink_irqs++;
        } else if (id == qcom_ipcc_id(QCOM_IPCC_CLIENT_AOP, QCOM_IPCC_SIGNAL_GLINK_QMP)) {
            bits |= kEvAop;
            gOwner.aop_irqs++;
        } else {
            gOwner.ipcc_irq_errors++;
        }
    }
    gOwner.ipcc_irqs++;
    wake(bits);
}

void rscIrq(void *)
{
    gOwner.rsc_irqs++;
    __atomic_fetch_or(&gOwner.rsc_done, 1u << kRscTcs, __ATOMIC_RELEASE);
    wake(kEvRsc);
}

/* The doorbell that says something is waiting for the modem. The modem's own client is MPSS, not LPASS: the
 * ADSP uses LPASS because that is the ADSP's client. */
void kickGlink(void *)
{
    wr(gOwner.ipcc_window, QCOM_IPCC_SEND, qcom_ipcc_id(FUXI_IPCC_CLIENT_MPSS, QCOM_IPCC_SIGNAL_GLINK_QMP));
}

void kickSmp2p()
{
    wr(gOwner.ipcc_window, QCOM_IPCC_SEND, qcom_ipcc_id(FUXI_IPCC_CLIENT_MPSS, QCOM_IPCC_SIGNAL_SMP2P));
}

void kickAop(void *)
{
    wr(gOwner.ipcc_window, QCOM_IPCC_SEND, qcom_ipcc_id(QCOM_IPCC_CLIENT_AOP, QCOM_IPCC_SIGNAL_GLINK_QMP));
}

/* ---- the images ---- */

int firmwareRead(void *context, uint64_t offset, void *to, size_t length)
{
    return fuxi_adsp_fw_read(context, offset, to, length);
}

int openImage(Image &image, const char *path)
{
    image = {};
    int rc = fuxi_adsp_fw_open(path, &image.file, &image.size);
    if (rc) {
        image.file = nullptr;
        return rc;
    }
    rc = qcom_mdt_parse(&image.mdt, image.size, firmwareRead, image.file);
    if (rc) {
        fuxi_adsp_fw_close(image.file);
        image.file = nullptr;
        return -1000 + rc;
    }
    return 0;
}

void releaseImage(Image &image)
{
    if (image.meta) {
        image.meta->release();
        image.meta = nullptr;
    }
    if (image.file) {
        fuxi_adsp_fw_close(image.file);
        image.file = nullptr;
    }
}

/* Hands the metadata to the secure world and, for a relocatable image, tells it where the image will live.
 * The metadata buffer has to be in memory the secure world can read, so it is a physically contiguous
 * allocation cleaned to the point of coherency before the call. */
int authenticateHeader(Image &image, uint32_t pas, uint64_t base)
{
    const uint64_t bytes = qcom_mdt_metadata_size(&image.mdt);
    if (!bytes) return -5;
    image.meta = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),
        bytes, 0xfffffffffffff000ull);
    if (!image.meta || image.meta->prepare() != kIOReturnSuccess) return -12;
    uint8_t *va = static_cast<uint8_t *>(image.meta->getBytesNoCopy());
    if (!va || qcom_mdt_metadata(&image.mdt, firmwareRead, image.file, va, bytes)) return -5;
    cleanToPoc(va, static_cast<size_t>(bytes));
    const uint64_t phys = image.meta->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);

    unsigned steps[FuxiModemPasStepCount];
    const unsigned count = fuxi_modem_pas_steps(image.mdt.relocatable ? 1 : 0, steps, FuxiModemPasStepCount);
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t command = fuxi_modem_pas_command(steps[i]);
        const uint32_t arginfo = fuxi_modem_pas_arginfo(steps[i]);
        ScmResult result = {-1, 0};
        if (steps[i] == FuxiModemPasInitImage)
            result = secureCall(command, arginfo, pas, phys, 0, 0);
        else if (steps[i] == FuxiModemPasMemSetup)
            result = secureCall(command, arginfo, pas, base, qcom_mdt_span(&image.mdt), 0);
        else
            result = secureCall(command, arginfo, pas, 0, 0, 0);
        if (steps[i] == FuxiModemPasInitImage) image.init_x0 = static_cast<uint32_t>(result.x0);
        if (steps[i] == FuxiModemPasMemSetup) image.mem_x0 = static_cast<uint32_t>(result.x0);
        if (!fuxi_modem_scm_ok(result.x0, result.x1)) return -1;
    }
    return 0;
}

/* Copies the image's loadable segments into the carveout. The segments are placed by the same rule the MDT
 * layer uses, so an image that does not fit is refused rather than truncated. */
int loadSegments(Image &image, uint64_t base, uint64_t region_size)
{
    Window region = {};
    if (!mapWindow(region, base, region_size, kIOMapWriteCombineCache)) return -12;
    uint8_t *bounce = static_cast<uint8_t *>(IOMallocData(kImageChunk));
    int rc = bounce ? 0 : -12;
    for (unsigned index = 0; !rc && index < image.mdt.count; ++index) {
        const QcomMdtSegment &segment = image.mdt.segment[index];
        if (!segment.load) continue;
        uint64_t offset = 0;
        if (qcom_mdt_place(&image.mdt, index, base, region_size, &offset)) {
            rc = -7;
            break;
        }
        uint8_t *to = const_cast<uint8_t *>(region.va) + offset;
        for (uint64_t done = 0; !rc && done < segment.file_size;) {
            const size_t chunk = segment.file_size - done < kImageChunk ? static_cast<size_t>(segment.file_size - done)
                                                                         : kImageChunk;
            if (fuxi_adsp_fw_read(image.file, segment.file_offset + done, bounce, chunk)) {
                rc = -5;
                break;
            }
            memcpy(to + done, bounce, chunk);
            done += chunk;
        }
        if (!rc && segment.memory_size > segment.file_size)
            bzero(to + segment.file_size, static_cast<size_t>(segment.memory_size - segment.file_size));
        if (!rc) ++gOwner.loaded_segments;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    if (bounce) IOFreeData(bounce, kImageChunk);
    unmapWindow(region);
    syncStats();
    return rc;
}

void shutdownModem(bool dtb, bool modem)
{
    if (modem && gOwner.modem.reset) {
        (void)secureCall(QCOM_SCM_PAS_SHUTDOWN, qcom_scm_args(1, 0, 0, 0), fuxi_modem_pas_id(FuxiModemPasImageModem), 0, 0, 0);
        gOwner.modem.reset = false;
    }
    if (dtb && gOwner.dtb.reset) {
        (void)secureCall(QCOM_SCM_PAS_SHUTDOWN, qcom_scm_args(1, 0, 0, 0), fuxi_modem_pas_id(FuxiModemPasImageDtb), 0, 0, 0);
        gOwner.dtb.reset = false;
    }
}

/* ---- the RPMh proxy votes ---- */

uint32_t waitEvents(uint32_t ms);
void serviceEvents(uint32_t bits);

bool readCmdDb(uint8_t *copy)
{
    Window db = {};
    if (!mapWindow(db, kCmdDbBase, kCmdDbSize)) return false;
    for (uint64_t at = 0; at < kCmdDbSize; at += 4) {
        const uint32_t word = rd(db, at);
        memcpy(copy + at, &word, 4);
    }
    unmapWindow(db);
    return true;
}

/* Votes the two rails the modem's power domains need. The command database on this board is known to hold cx.lvl;
 * mss.lvl is looked up at run time and its absence is reported rather than assumed. */
int voteRails()
{
    if (!gOwner.rsc_window.va) return -1;
    struct CmdDbCopy {
        uint8_t *bytes;
        uint32_t bytes_size;
        explicit CmdDbCopy(uint32_t size) : bytes(static_cast<uint8_t *>(IOMallocData(size))), bytes_size(size) {}
        ~CmdDbCopy() { if (bytes) IOFreeData(bytes, bytes_size); }
    } copy(kCmdDbSize);
    if (!copy.bytes || !readCmdDb(copy.bytes)) return -2;

    QcomRpmhResource cx = {}, mss = {};
    uint32_t cx_index = 0, mss_index = 0;
    if (qcom_cmd_db_find(copy.bytes, kCmdDbSize, kCxRail, &cx)) return -2;
    if (qcom_cmd_db_find(copy.bytes, kCmdDbSize, kMssRail, &mss)) {
        /* The modem's own rail is not in this board's command database.  That is a fact about the board, not a
         * reason to guess: the vote below goes out for the rails that are there and the missing one is reported. */
        gOwner.mss_rail_found = 0;
        printf("FUXI_MODEM: %s is not in this board's command database\n", kMssRail);
    } else {
        gOwner.mss_rail_found = 1;
    }
    if (qcom_rpmh_level_index(&cx, 0xffffffffu, &cx_index)) return -2;
    if (gOwner.mss_rail_found && qcom_rpmh_level_index(&mss, 0xffffffffu, &mss_index)) return -2;

    gOwner.cx_index = cx_index;
    gOwner.cx_addr = cx.address;
    gOwner.mss_index = mss_index;
    gOwner.mss_addr = mss.address;

    uint32_t addresses[2], values[2];
    unsigned count = 0;
    addresses[count] = cx.address;
    values[count++] = cx_index;
    if (gOwner.mss_rail_found) {
        addresses[count] = mss.address;
        values[count++] = mss_index;
    }
    QcomRpmhWrite writes[16];
    const unsigned written = qcom_rpmh_active_request(&gOwner.layout, kRscTcs, addresses, values, count, writes, 16);
    if (!written) return -22;

    const uint32_t irq_enable = rd(gOwner.rsc_window, kRscTcsOffset) | (1u << kRscTcs);
    wr(gOwner.rsc_window, kRscTcsOffset, irq_enable);
    __atomic_store_n(&gOwner.rsc_done, 0, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < written; ++i)
        wr(gOwner.rsc_window, kRscTcsOffset + writes[i].offset, writes[i].value);

    uint64_t deadline = 0;
    clock_interval_to_deadline(100, kMillisecondScale, &deadline);
    while (!(__atomic_load_n(&gOwner.rsc_done, __ATOMIC_ACQUIRE) & (1u << kRscTcs)) && mach_absolute_time() < deadline)
        serviceEvents(waitEvents(20));
    bool done = (__atomic_load_n(&gOwner.rsc_done, __ATOMIC_ACQUIRE) & (1u << kRscTcs)) != 0;
    if (!done && (rd(gOwner.rsc_window, kRscTcsOffset + gOwner.layout.irq_status) & (1u << kRscTcs))) done = true;
    const unsigned finish = qcom_rpmh_finish(&gOwner.layout, kRscTcs, writes, 16);
    for (unsigned i = 0; i < finish; ++i)
        wr(gOwner.rsc_window, kRscTcsOffset + writes[i].offset, writes[i].value);
    return 0;
}

/* ---- the events ---- */

/* Waits for the owner to be woken. The pending bits are taken out of the word, so a bit that arrives during the
 * service is not lost. The timeout is the upper bound on the wait, not a delay: the interrupt handlers wake the
 * owner as soon as something happens. */
uint32_t waitEvents(uint32_t ms)
{
    uint32_t bits = __atomic_exchange_n(&gOwner.pending, 0, __ATOMIC_ACQ_REL);
    if (bits) return bits;
    assert_wait_timeout(reinterpret_cast<event_t>(&gOwner.pending), THREAD_UNINT, ms, kMillisecondScale);
    bits = __atomic_exchange_n(&gOwner.pending, 0, __ATOMIC_ACQ_REL);
    if (bits) {
        clear_wait(current_thread(), THREAD_AWAKENED);
        return bits;
    }
    thread_block(THREAD_CONTINUE_NULL);
    return __atomic_exchange_n(&gOwner.pending, 0, __ATOMIC_ACQ_REL);
}

void serviceEvents(uint32_t bits);

void serviceSmp2p()
{
    if (!gOwner.smp2p.in) {
        if (volatile uint8_t *in = item(kSmp2pInItem, nullptr)) {
            gOwner.smp2p.in = reinterpret_cast<volatile uint32_t *>(in);
            kickSmp2p();
        }
        return;
    }
    uint32_t value = 0, changed = 0;
    if (qcom_smp2p_service(&gOwner.smp2p, "slave-kernel", &value, &changed)) kickSmp2p();
    gOwner.smp2p_ready = value;
    /* The modem's fatal bit is bit 0 and its ready bit is bit 1, the same numbering the ADSP uses on the same
     * kind of SMP2P pair. */
    if (value & 0x1u) gOwner.fatal = true;
    if (value & 0x2u) gOwner.smp2p_ready = 1;
    if (value & 0x4u) gOwner.handover = true;
    syncStats();
}

void serviceEvents(uint32_t bits)
{
    if (bits & kEvRsc) (void)0;
    if (bits & kEvAop) (void)qcom_qmp_step(&gOwner.qmp);
    if (bits & (kEvSmp2p | kEvGlink)) serviceSmp2p();
    if (gOwner.glink_started && (bits & kEvGlink)) {
        if (!gOwner.glink.rx.data) {
            uint32_t size = 0;
            if (volatile uint8_t *rx = item(kGlinkRxItem, &size)) {
                qcom_glink_set_rx(&gOwner.glink, rx, size);
            }
        }
        (void)qcom_glink_poll(&gOwner.glink);
    }
}

/* ---- the steps ---- */

int stepFirmware()
{
    int rc = openImage(gOwner.dtb, "modem:modem_dtb");
    if (rc) {
        printf("FUXI_MODEM: the modem device tree blob is not there (%d)\n", rc);
        return rc;
    }
    rc = openImage(gOwner.modem, "modem:modem");
    if (rc) {
        printf("FUXI_MODEM: the modem image is not there (%d)\n", rc);
        return rc;
    }
    setStage(kStFirmware);
    return 0;
}

int stepMap()
{
    if (!mapWindow(gOwner.smem_window, kSmemBase, kSmemSize)) return -12;
    if (!mapWindow(gOwner.mutex_window, kTcsrMutex, kTcsrMutexSize)) return -12;
    if (!mapWindow(gOwner.ipcc_window, kIpccBase, kIpccSize)) return -12;
    if (!mapWindow(gOwner.aoss_window, kAossBase, kAossSize)) return -12;
    if (!mapWindow(gOwner.rsc_window, kRscBase, kRscSize)) return -12;
    gOwner.smem.base = gOwner.smem_window.va;
    gOwner.smem.size = kSmemSize;
    /* The modem's partition is the one shared with the modem itself: local pid 0, remote pid 1. */
    if (qcom_smem_partition(&gOwner.smem, QCOM_SMEM_HOST_APPS, kModemPid, &gOwner.part)) return -2;
    gOwner.smem_ready = true;
    setStage(kStMapped);
    return 0;
}

int stepRails()
{
    /* The RSC's own register layout: the id register says which major version it is, and the version moved the
     * TCS registers. */
    gOwner.layout = qcom_rsc_layout(rd(gOwner.rsc_window, 0));
    const int rc = voteRails();
    if (rc) {
        printf("FUXI_MODEM: the proxy rails could not be voted (%d)\n", rc);
        return rc;
    }
    setStage(kStRails);
    return 0;
}

/* The device tree blob first, then the modem image, as Linux does. */
int stepPas()
{
    (void)secureCall(QCOM_SCM_PAS_SHUTDOWN, qcom_scm_args(1, 0, 0, 0), fuxi_modem_pas_id(FuxiModemPasImageModem), 0, 0, 0);
    (void)secureCall(QCOM_SCM_PAS_SHUTDOWN, qcom_scm_args(1, 0, 0, 0), fuxi_modem_pas_id(FuxiModemPasImageDtb), 0, 0, 0);

    int rc = authenticateHeader(gOwner.dtb, fuxi_modem_pas_id(FuxiModemPasImageDtb), kModemDtbCarveoutBase);
    if (!rc) rc = loadSegments(gOwner.dtb, kModemDtbCarveoutBase, kModemDtbCarveoutSize);
    if (!rc) {
        const ScmResult auth = secureCall(fuxi_modem_pas_command(FuxiModemPasAuthAndReset),
                                           fuxi_modem_pas_arginfo(FuxiModemPasAuthAndReset),
                                           fuxi_modem_pas_id(FuxiModemPasImageDtb), 0, 0, 0);
        if (!fuxi_modem_scm_ok(auth.x0, auth.x1)) rc = -3;
        else gOwner.dtb_auth_cookie = gOwner.scm_x0;
    }
    if (rc) {
        printf("FUXI_MODEM: the modem device tree blob was refused (scm %u/%u)\n", gOwner.scm_x0, gOwner.scm_x1);
        shutdownModem(true, false);
        return rc;
    }
    gOwner.dtb.reset = true;
    setStage(kStDtb);
    /* The metadata and the file are not needed once the blob was accepted. */
    releaseImage(gOwner.dtb);

    rc = authenticateHeader(gOwner.modem, fuxi_modem_pas_id(FuxiModemPasImageModem), kModemCarveoutBase);
    if (!rc) rc = loadSegments(gOwner.modem, kModemCarveoutBase, kModemCarveoutSize);
    if (rc) {
        printf("FUXI_MODEM: the modem image was refused (%d)\n", rc);
        shutdownModem(true, false);
        return rc;
    }
    setStage(kStLoaded);
    const ScmResult auth = secureCall(fuxi_modem_pas_command(FuxiModemPasAuthAndReset),
                                       fuxi_modem_pas_arginfo(FuxiModemPasAuthAndReset),
                                       fuxi_modem_pas_id(FuxiModemPasImageModem), 0, 0, 0);
    if (!fuxi_modem_scm_ok(auth.x0, auth.x1)) {
        printf("FUXI_MODEM: the modem was not released (scm %u/%u)\n", gOwner.scm_x0, gOwner.scm_x1);
        shutdownModem(true, true);
        return -3;
    }
    gOwner.modem.reset = true;
    gOwner.modem_auth_cookie = gOwner.scm_x0;
    return 0;
}

/* The AOP mailbox: the handshake, then the message that releases the modem. */
int stepQmp()
{
    qcom_qmp_init(&gOwner.qmp, reinterpret_cast<volatile uint32_t *>(gOwner.aoss_window.va), kAossSize, kickAop,
                  nullptr);
    uint64_t deadline = 0;
    clock_interval_to_deadline(1000, kMillisecondScale, &deadline);
    while (gOwner.qmp.state != QcomQmpOpen && mach_absolute_time() < deadline)
        serviceEvents(waitEvents(10) | kEvAop);
    if (gOwner.qmp.state != QcomQmpOpen) {
        qcom_qmp_abandon(&gOwner.qmp);
        return -60;
    }
    if (qcom_qmp_send(&gOwner.qmp, kLoadStateMessage) != QcomQmpSending) return -60;
    deadline = 0;
    clock_interval_to_deadline(1000, kMillisecondScale, &deadline);
    while (gOwner.qmp.state == QcomQmpSending && mach_absolute_time() < deadline)
        serviceEvents(waitEvents(10) | kEvAop);
    if (gOwner.qmp.state != QcomQmpOpen) {
        qcom_qmp_abandon(&gOwner.qmp);
        return -60;
    }
    gOwner.qmp_response = gOwner.qmp.ram[gOwner.qmp.box / 4];
    setStage(kStLoadState);
    return 0;
}

/* The modem's SMP2P pair: the AP writes the outbound item and reads the modem's inbound one. */
int stepSmp2p()
{
    const int rc = allocItem(kSmp2pOutItem, QCOM_SMP2P_ITEM_BYTES);
    if (rc) return rc;
    volatile uint8_t *out = item(kSmp2pOutItem, nullptr);
    if (!out) return -2;
    qcom_smp2p_init_out(&gOwner.smp2p, reinterpret_cast<volatile uint32_t *>(out), 0, kModemPid);
    if (qcom_smp2p_add_entry(&gOwner.smp2p, "master-kernel") < 0) return -22;
    return 0;
}

int stepIpcc()
{
    if (!armcpu_spi_attach) return -78;
    int rc = 0;
    for (unsigned attempt = 0;; ++attempt) {
        rc = armcpu_spi_attach(kSpiIrqBase + kIpccSpi, false, ipccIrq, nullptr, nullptr, nullptr);
        if (rc != kIOReturnNotReady || attempt == 240) break;
        IOSleep(250);
    }
    if (rc) return rc;
    if (armcpu_spi_attach(kSpiIrqBase + kRscSpi, false, rscIrq, nullptr, nullptr, nullptr)) gOwner.ipcc_irq_errors |= 0x80u;
    else gOwner.rsc_ready = true;
    const uint32_t enable[3] = {
        qcom_ipcc_id(FUXI_IPCC_CLIENT_MPSS, QCOM_IPCC_SIGNAL_SMP2P),
        qcom_ipcc_id(FUXI_IPCC_CLIENT_MPSS, QCOM_IPCC_SIGNAL_GLINK_QMP),
        qcom_ipcc_id(QCOM_IPCC_CLIENT_AOP, QCOM_IPCC_SIGNAL_GLINK_QMP),
    };
    for (uint32_t id : enable) wr(gOwner.ipcc_window, QCOM_IPCC_RECV_ENABLE, id);
    gOwner.ipcc_ready = true;
    kickSmp2p();
    return 0;
}

/* The GLINK edge, and with it the QRTR link. The adapter in the component owns the QRTR protocol; this only
 * hands it the SMEM window and the doorbell, and opens the channel the modem opens. */
int stepGlink()
{
    int rc = allocItem(kGlinkDescItem, 32);
    if (!rc) rc = allocItem(kGlinkTxItem, kGlinkFifoBytes);
    if (rc) return rc;
    uint32_t desc_size = 0, tx_size = 0;
    volatile uint8_t *desc = item(kGlinkDescItem, &desc_size);
    volatile uint8_t *tx = item(kGlinkTxItem, &tx_size);
    if (!desc || !tx || desc_size < 32 || tx_size < kGlinkFifoBytes) return -2;
    volatile uint32_t *words = reinterpret_cast<volatile uint32_t *>(desc);
    const QcomGlinkFifo tx_fifo = {&words[1], &words[0], tx, tx_size};
    const QcomGlinkFifo rx_fifo = {&words[3], &words[2], nullptr, 0};
    qcom_glink_init(&gOwner.glink, tx_fifo, rx_fifo, kickGlink, nullptr);
    static uint8_t storage[kIpcRtrIntentSize * kIpcRtrIntentCount];
    const QcomGlinkClient client = {nullptr, nullptr};
    gOwner.channel = qcom_glink_channel(&gOwner.glink, kIpcRtrChannel, kIpcRtrIntentSize, kIpcRtrIntentCount, storage,
                                        &client, nullptr, 0);
    if (!gOwner.channel) return -12;
    gOwner.glink_started = true;
    setStage(kStLink);
    return 0;
}

/* The component's adapter takes over the link: it finds the modem's items itself, so the window it needs is the
 * SMEM one. The doorbell is the one above. */
struct LinkConfig {
    volatile uint8_t *smem_base;
    uint32_t smem_size;
    void (*kick)(void *context);
    void *kick_context;
};

int stepAdapter()
{
    const LinkConfig config = {gOwner.smem_window.va, kSmemSize, nullptr, nullptr};
    const int rc = fuxi_modem_start(&config);
    if (rc) {
        printf("FUXI_MODEM: the QRTR adapter refused the link (%d)\n", rc);
        return rc;
    }
    return 0;
}

/* Waits for the modem's ready bits over SMP2P. */
int waitReady()
{
    uint64_t deadline = 0;
    clock_interval_to_deadline(5000, kMillisecondScale, &deadline);
    while (mach_absolute_time() < deadline) {
        serviceEvents(waitEvents(10) | kEvSmp2p);
        if (gOwner.fatal) {
            printf("FUXI_MODEM: the modem reported a fatal condition\n");
            return -8;
        }
        if (gOwner.smp2p_ready) {
            setStage(kStReady);
            return 0;
        }
    }
    printf("FUXI_MODEM: the modem did not report ready within 5 s\n");
    return -6;
}

void ownerThread(void *, wait_result_t)
{
    thread_set_thread_name(current_thread(), "fuxi-modem");

    int rc = stepFirmware();
    if (rc) goto failed;
    rc = stepMap();
    if (rc) goto failed;
    rc = stepRails();
    if (rc) goto failed;
    rc = stepSmp2p();
    if (rc) goto failed;
    rc = stepIpcc();
    if (rc) goto failed;
    rc = stepPas();
    if (rc) goto failed;
    rc = stepQmp();
    if (rc) goto failed;
    rc = waitReady();
    if (rc) goto failed;
    rc = stepGlink();
    if (rc) goto failed;
    rc = stepAdapter();
    if (rc) goto failed;
    setStage(kStUp);
    printf("FUXI_MODEM: the modem is up, stage %u\n", gOwner.stage);
    return;

failed:
    gOwner.last_error = rc;
    printf("FUXI_MODEM: the bring-up stopped at stage %u (%d)\n", gOwner.stage, rc);
    syncStats();
}

}  // namespace

/* The read-only inventory of the firmware partition: what the modem's files are called and how big they
 * are. This is the one part of the bring-up that proves nothing by running the modem, and it is the part
 * that says which files to look for at all. It reads the partition and lists it; it starts nothing.
 *
 * The listing goes into the stats array, so the USB control channel can read it back: the kernel's own
 * print is lost, because the channel enumerates long after the boot sequence. */
int inventoryFirmware()
{
    /* The root of the volume first, so the partition itself is proven, then the firmware's directory.
     * The modem's own images live in a directory of their own, so that one is listed too. */
    const char *const directories[] = {"/", "/image", "/image/modem_pr"};
    unsigned count = 0;
    for (unsigned d = 0; d < 3; ++d) {
        for (unsigned index = 0; index < 1024; ++index) {
            char name[256];
            uint64_t size = 0;
            int is_directory = 0;
            const int rc = fuxi_adsp_fw_list(directories[d], index, name, sizeof name, &size, &is_directory);
            if (rc) {
                printf("FUXI_MODEM: the listing of %s failed (%d)\n", directories[d], rc);
                return rc;
            }
            if (!name[0]) break;
            /* The names are the whole point, so they go out one line at a time, and the counts into the
             * stats array for the channel to read back. */
            printf("FUXI_MODEM: %s/%s %llu%s\n", directories[d], name, size, is_directory ? " (directory)" : "");
            ++count;
        }
    }
    native_fuxi_modem_stats[10] = static_cast<int32_t>(count);
    /* The count of the entries that are directories, so the reader knows whether /image was found. */
    unsigned directories_found = 0;
    for (unsigned index = 0; index < 1024; ++index) {
        char name[256];
        uint64_t size = 0;
        int is_directory = 0;
        if (fuxi_adsp_fw_list("/image", index, name, sizeof name, &size, &is_directory)) break;
        if (!name[0]) break;
        if (is_directory) ++directories_found;
    }
    native_fuxi_modem_stats[11] = static_cast<int32_t>(directories_found);
    syncStats();
    printf("FUXI_MODEM: the firmware partition holds %u entries\n", count);
    return 0;
}

/* Opens both images and parses them, read only. Nothing is mapped, no register is touched and no
 * modem is started: this proves the file path, which is the part that can be wrong on its own. The
 * split image's segments are read out of the partition and joined, so a missing or mismatched
 * segment shows here rather than as a failure much later in the sequence. */
int openFirmware()
{
    int rc = openImage(gOwner.dtb, "modem:modem_dtb");
    if (rc) {
        printf("FUXI_MODEM: the modem device tree blob is not there (%d)\n", rc);
        return rc;
    }
    printf("FUXI_MODEM: the device tree blob is %llu bytes in %u segments\n",
           (unsigned long long)gOwner.dtb.size, gOwner.dtb.mdt.count);
    rc = openImage(gOwner.modem, "modem:modem");
    if (rc) {
        printf("FUXI_MODEM: the modem image is not there (%d)\n", rc);
        return rc;
    }
    printf("FUXI_MODEM: the modem image is %llu bytes in %u segments\n",
           (unsigned long long)gOwner.modem.size, gOwner.modem.mdt.count);
    /* The address range and whether the image is relocatable: these two decide the PAS sequence's
     * memory setup step, so the image has to be read before anything is voted or loaded. */
    printf("FUXI_MODEM: the image spans %llx to %llx, %s\n",
           (unsigned long long)gOwner.modem.mdt.min_address,
           (unsigned long long)gOwner.modem.mdt.max_address,
           gOwner.modem.mdt.relocatable ? "relocatable" : "fixed");
    /* The hash segment's index: the parse fails without it, so its presence is a fact and its
     * place in the segment list is where the metadata comes from. */
    printf("FUXI_MODEM: the hash is segment %d, the metadata is %llu bytes\n",
           gOwner.modem.mdt.hash,
           (unsigned long long)qcom_mdt_metadata_size(&gOwner.modem.mdt));
    /* The image's segments, so a gap in the numbering is visible in the log. */
    for (unsigned i = 0; i < gOwner.modem.mdt.count; ++i) {
        const QcomMdtSegment &segment = gOwner.modem.mdt.segment[i];
        printf("FUXI_MODEM:   segment %u at %llx, %llu bytes in the file, %s\n", i,
               (unsigned long long)segment.address, (unsigned long long)segment.file_size,
               segment.load ? "loadable" : "not loadable");
    }
    setStage(kStFirmware);
    return 0;
}

/* Reads the board's command database and looks the modem's rails up in it, without voting anything.
 * The database is a read-only window, so this touches no register and starts nothing: it answers
 * which of the modem's two rails this board's own copy carries, which is a question about the board
 * and not about the modem. */
int inspectRails()
{
    struct CmdDbCopy {
        uint8_t *bytes;
        uint32_t bytes_size;
        explicit CmdDbCopy(uint32_t size) : bytes(static_cast<uint8_t *>(IOMallocData(size))), bytes_size(size) {}
        ~CmdDbCopy() { if (bytes) IOFreeData(bytes, bytes_size); }
    } copy(kCmdDbSize);
    if (!copy.bytes || !readCmdDb(copy.bytes)) {
        printf("FUXI_MODEM: the command database could not be read\n");
        return -2;
    }
    /* The rails the modem's power domains need, and the ones the audio chain already proved work
     * on this board, so a difference between them is a fact about the board's copy. */
    static const char *const names[] = {kCxRail, kMssRail, "gpu.lvl", "gfx.lvl", "cx.lvl", "mx.lvl", "gmxc.lvl"};
    unsigned found = 0;
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; ++i) {
        QcomRpmhResource resource = {};
        if (qcom_cmd_db_find(copy.bytes, kCmdDbSize, names[i], &resource)) {
            printf("FUXI_MODEM: %s is not in this board's command database\n", names[i]);
            continue;
        }
        uint32_t index = 0;
        const int rc = qcom_rpmh_level_index(&resource, 0xffffffffu, &index);
        printf("FUXI_MODEM: %s is at %llx, its highest level is index %u\n", names[i],
               (unsigned long long)resource.address, rc ? 0 : index);
        ++found;
    }
    printf("FUXI_MODEM: the command database carries %u of the %u names looked up\n", found,
           (unsigned)(sizeof names / sizeof names[0]));
    gOwner.mss_rail_found = 0;
    {
        QcomRpmhResource mss = {};
        if (!qcom_cmd_db_find(copy.bytes, kCmdDbSize, kMssRail, &mss)) gOwner.mss_rail_found = 1;
    }
    syncStats();
    return 0;
}

/* The entry point the kernel patch calls, and the boot argument that turns it on. The argument mirrors the
 * ADSP's: fuxi-modem=1 starts the bring-up, 0 does nothing. */
extern "C" void native_fuxi_modem_probe(void)
{
    unsigned mode = 0;
    if (!PE_parse_boot_argn("fuxi-modem", &mode, sizeof mode) || mode == 0) return;
    /* The UFS driver is not up when the kernel calls this, so the wait comes before the check:
     * the probe is asked for the partition long before any partition can be read. */
    for (unsigned attempt = 0; attempt < 120 && !fuxi_adsp_fw_ready(); ++attempt) IOSleep(250);
    if (!fuxi_adsp_fw_ready()) {
        printf("FUXI_MODEM: the firmware partition is not there yet\n");
        return;
    }
    /* Mode 1 only inventories the firmware partition, read only. It starts no modem and touches no
     * register, so it can be run on a device whose radio policy forbids anything else. */
    if (mode == 1) {
        (void)inventoryFirmware();
        return;
    }
    /* Mode 2 opens and parses the images, still read only: no window is mapped and no register is
     * touched, but the whole file path is exercised against the board's own files. */
    if (mode == 2) {
        (void)openFirmware();
        return;
    }
    /* Mode 3 reads the board's command database and looks the modem's rails up in it, still read
     * only: no vote goes out and no register is written, so it answers which rails this board
     * carries without touching the power rails at all. */
    if (mode == 3) {
        (void)inspectRails();
        return;
    }
    /* Mode 4 and up start the modem: they vote rails, load the images over PAS and let the
     * modem's firmware run. The radio stays off until a later step says otherwise. */
    if (mode < 4) {
        printf("FUXI_MODEM: the mode %u does nothing\n", mode);
        return;
    }
    thread_t thread;
    if (kernel_thread_start(ownerThread, nullptr, &thread) != KERN_SUCCESS) {
        printf("FUXI_MODEM: the owner thread did not start\n");
        return;
    }
    thread_deallocate(thread);
}

/* The diagnostics entry point the USB debug channel calls. The bring-up's own kprintf is lost: the channel
 * enumerates long after bsd_init, and the modem's sequence runs before it. So the counters the bring-up
 * produced are read back here, from the same array the report prints.
 *
 * The pages rotate, the same way the audio probe's do: the channel's resource page is a single word, so
 * the values are handed out one page at a time and the reader samples it repeatedly. The word carries the
 * page in bits 16..19 and the value in bits 20..31, the layout the audio's uses. */
extern "C" unsigned fuxi_modem_diag_resources(void)
{
    static unsigned page_counter;
    const unsigned page = __atomic_fetch_add(&page_counter, 1, __ATOMIC_RELAXED) & 0xfu;
    uint32_t value = 0;
    switch (page) {
    case 0: value = static_cast<uint32_t>(native_fuxi_modem_stats[0]) & 0xffu; break;  /* stage */
    case 1: value = static_cast<uint32_t>(native_fuxi_modem_stats[1]) & 0xffffu; break; /* last error */
    case 2: value = static_cast<uint32_t>(native_fuxi_modem_stats[2]) & 0xffffu; break; /* scm x0 */
    case 3: value = static_cast<uint32_t>(native_fuxi_modem_stats[3]) & 0xffffu; break; /* scm x1 */
    case 4: value = static_cast<uint32_t>(native_fuxi_modem_stats[4]) & 0xffu; break;    /* smp2p ready */
    case 5: value = static_cast<uint32_t>(native_fuxi_modem_stats[5]) & 0xffu; break;    /* smp2p fatal */
    case 6: value = static_cast<uint32_t>(native_fuxi_modem_stats[6]) & 0xffffu; break; /* ipcc irqs */
    case 7: value = static_cast<uint32_t>(native_fuxi_modem_stats[7]) & 0xffu; break;    /* segments */
    case 8: value = static_cast<uint32_t>(native_fuxi_modem_stats[8]) & 0xffu; break;    /* mss rail */
    case 9: value = static_cast<uint32_t>(native_fuxi_modem_stats[9]) & 0xffu; break;    /* glink up */
    case 10: value = static_cast<uint32_t>(native_fuxi_modem_stats[10]) & 0xffffu; break; /* entries listed */
    case 11: value = static_cast<uint32_t>(native_fuxi_modem_stats[11]) & 0xffffu; break; /* directories found */
    default: value = 0; break;
    }
    return (page & 0xfu) << 16 | (value & 0xfffu) << 20;
}
