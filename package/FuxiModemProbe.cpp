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
#include <string.h>

#include "FuxiModemPas.h"
#include "FuxiModemScm.h"
#include "FuxiModemAssign.h"
#include "FuxiModemLowMeta.h"
#include "FuxiModemRmtfs.h"
#include "FuxiModemTftp.h"
#include "FuxiAdspFirmware.h"
#include "QcomMdt.h"
#include "QcomQmp.h"
#include "QcomRpmh.h"
#include "FuxiRpmhCorner.h"
#include "FuxiRscLayout.h"
#include "QcomScm.h"
#include "QcomSmem.h"
#include "QcomSmp2p.h"

extern "C" int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *)
    __attribute__((weak_import));
extern int printf(const char *format, ...);
/* Physical window of the kernel: the low metadata window must not be inside it. */
extern "C" unsigned long gPhysBase, gPhysSize;

/* The component's board adapter, in the QcomBaseband component. The link and the QRTR layer live there; this
 * file only starts the modem and hands the adapter its SMEM window and its doorbell. */
extern "C" int fuxi_modem_start(const void *config);
/* Publishes the bridge's IOKit services; called before the link exists, so CommCenter's early radio-module
 * decision sees them. Idempotent. */
extern "C" void fuxi_modem_publish(void);
extern "C" void fuxi_modem_poll(void);
extern "C" void fuxi_modem_report(unsigned second);
/* The board's RMTFS adapter: the four files, the buffer, and the callbacks the component's protocol
 * half calls. */
extern "C" int fuxi_rmtfs_prepare(void);
extern "C" int fuxi_rmtfs_share(void);
extern "C" int fuxi_rmtfs_inventory(void);
extern "C" const struct fuxi_rmtfs_ops *fuxi_rmtfs_ops(void);
extern "C" const struct fuxi_tftp_ops *fuxi_tftp_ops(void);
extern "C" void fuxi_rmtfs_counters(uint32_t *out, unsigned count);
extern "C" void fuxi_tftp_counters(uint32_t *out, unsigned count);

/* What the adapter's link has seen. The struct is the adapter's, but the probe cannot include its
 * header (the component is built separately), so the fields are spelled out here in the same order. */
struct LinkStatus {
    uint8_t ready;
    uint8_t hello_sent;
    uint8_t reserved[2];
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t tx_again;
    uint32_t open_failures;
    uint32_t qrtr_servers;
    /* Where the traffic stops. The link can be up while nothing arrives, and these say at which layer. */
    uint32_t glink_rx_messages;
    uint32_t glink_tx_messages;
    uint32_t glink_rx_data;
    uint32_t glink_dropped;
    uint32_t glink_unknown_opens;
    uint32_t rx_avail;
    uint32_t ch_up, ch_remote_open, ch_open_sent, ch_open_acked;
    uint32_t ch_local_advertised;
    uint32_t qrtr_rx, qrtr_malformed, qrtr_hello_received, qrtr_hello_dropped;
    uint32_t qrtr_remote_node, qrtr_hello_reply_node, qrtr_hello_reply_port;
    uint8_t qrtr_hello_reply_owed, qrtr_remote_node_known;
    uint8_t reserved2[2];
    uint32_t glink_remote_intents;
    uint32_t glink_remote_busy;
    uint32_t glink_no_intent, glink_intent_too_small, glink_intent_requested, glink_rx_done;
    uint32_t qrtr_lookups;
    /* The lookups the modem answered. A lookup it ignored is not counted, so a count that stays behind the number
       that went out says the modem did not answer it. */
    uint32_t qrtr_lookups_answered;
    /* The QRTR flow control: the DATA packets the peer has not credited yet, and the RESUME_TX packets sent and
       received. A peer that runs out of credit stops answering, and the credit comes back with RESUME_TX. */
    uint32_t qrtr_tx_pending, qrtr_resumes_sent, qrtr_resumes_received;
    /* The destinations the flow table is tracking: the modem opens a source port per stream, so this is the
       number of streams and the number the table has to hold. */
    uint32_t qrtr_tx_flows;
    /* The service registry's locator: how often it was announced, how many requests it took and how many of
       them were answered. */
    uint32_t locator_announces, locator_requests, locator_answered;
    /* RMTFS: the modem's remote file system, with the same three counts. */
    uint32_t rmtfs_announces, rmtfs_requests, rmtfs_answered;
    /* TFTP: the modem's carrier configuration, with the same counts and the reads and the writes. */
    uint32_t tftp_announces, tftp_requests, tftp_answered, tftp_reads, tftp_writes, tftp_missing;
    char last_unknown[32];
};
extern "C" void fuxi_modem_status(LinkStatus *status);

/* The counters the bring-up produces, read back by the USB control channel's diagnostics entry point.
 * The symbol is external so that other half can see it: the channel enumerates long after this file's
 * bring-up has run, and the kernel's own print of the counters is long gone by then. */
extern "C" volatile int32_t native_fuxi_modem_stats[16] = {};

/* The modem's RMTFS buffer, handed to the owners the header names: the hypervisor's own and the
 * modem's MSA. It is defined below, outside the anonymous namespace, because the board's RMTFS
 * adapter calls it. */
extern "C" int fuxi_modem_rmtfs_assign(uint64_t address, uint64_t size, unsigned owners);

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
/* The always-on processor's window. The board's power controller is at 0xc300000 of 0x400
 * (the CAF live device tree's power-controller@c300000 node; facts/audio/binding.toml's
 * aoss_base is the same value, and the ADSP probe already uses it). */
constexpr uint64_t kAossBase = 0x00c300000ull;
constexpr uint32_t kAossSize = 0x400u;
/* From the device tree and the two driver tables: fuxi-live.dts:29416 (clocks = <&rpmhcc 12>),
 * dt-bindings/clock/qcom,rpmh.h:21 (RPMH_IPA_CLK 12), clk-rpmh.c:412 (DEFINE_CLK_RPMH_BCM(ipa, "IP0")) and
 * tcs.h:68-79 (BCM_TCS_CMD). */
/* The IPA firmware's reserved region: the device tree's ipa_fw_region@9b080000 of 0x10000 (the same region the
 * kernel side maps to watch for the image). */
constexpr uint64_t kIpaFwBase = 0x9b080000ull;
constexpr uint64_t kIpaFwBytes = 0x00010000ull;
constexpr const char *kIpaClockResource = "IP0";
constexpr uint32_t kIpaClockEnable = 0x60000001u;
/* The two Bus Clock Manager resources the IPA paths land on, from drivers/interconnect/qcom/sm8550.c:
 *   bcm_cn0.name = "CN0" lists qhs_ipa and qsm_cfg -> the "config" path (ipa_data-v5.5.c:462-466)
 *   bcm_mc0.name = "MC0" lists ebi              -> the "memory" path (:456-460)
 * Both are keepalive (sm8550.c:1080, :1132) and bcm-voter.c:107-113 votes 1 on each when nothing else asks for
 * bandwidth: the lowest request there is, and it still opens the path. */
constexpr const char *kIpaConfigBcm = "CN0";
constexpr const char *kIpaMemoryBcm = "MC0";
constexpr uint32_t kIpaBcmKeepalive = 0x60004001u;   /* BCM_TCS_CMD(1, 1, 1, 1) */
/* The IPA window constants are gone with the read: see the note in mode 18. */
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

/* A metadata buffer in the low DRAM window: the slice's virtual address, the physical address the
 * secure world is given, and how many bytes the metadata is. */
struct LowMeta {
    volatile uint8_t *va;
    uint64_t phys;
    uint64_t bytes;
};

struct Image {
    void *file;
    uint64_t size;
    QcomMdtImage mdt;
    LowMeta meta;
    bool has_meta;
    bool reset;
    uint32_t init_x0, mem_x0;
};

struct Owner {
    Window smem_window, mutex_window, ipcc_window, aoss_window, rsc_window;
    QcomSmem smem;
    QcomSmemPartition part;
    QcomSmp2p smp2p;
    QcomQmp qmp;
    QcomRscLayout layout;
    Image dtb, modem;
    /* The IPA's own firmware: the AP loads it and the secure world releases the block with it. */
    Image ipa;

    uint32_t pending;
    uint32_t rsc_done;
    uint32_t ipa_clock_voted;
    uint32_t ipcc_irqs, rsc_irqs, smp2p_irqs, glink_irqs, aop_irqs;
    uint32_t ipcc_irq_errors;
    uint32_t stage;
    int last_error;

    uint32_t scm_x0, scm_x1, scm_op, scm_pas;
    uint32_t dtb_auth_cookie, modem_auth_cookie;
    uint32_t assign_x0;      /* what the secure world said about the memory assign */
    uint32_t assign_x1;
    uint32_t loaded_segments;
    uint32_t smp2p_ready, smp2p_fatal, smp2p_handover;
    uint32_t qmp_response;
    uint32_t cx_index, mss_index;
    uint32_t cx_addr, mss_addr;
    uint32_t mss_rail_found;   /* 0: the command database has no mss.lvl */
    uint32_t rsc_drv_id;       /* the RSC DRV2 id register, or the forced major version */
    uint32_t pas_supported[2];  /* the secure world's answer for the DTB's and the modem's pas id */
    uint32_t pas_support_x0[2]; /* what it said */
    uint32_t last_tx_again;   /* the link's send-again count the last time it was reported */
    uint32_t last_glink_rx;   /* the GLINK layer's message count the last time it was reported */

    bool smem_ready, smem_locked, ipcc_ready, rsc_ready, glink_started, glink_up, fatal, handover;
    /* The modem's SMP2P entries are dumped once, when they first arrive. */
    bool smp2p_dumped;
    /* Whether the modem's crash reason has already been read, so it is read once and not every second. */
    bool crash_reason_read;
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
    native_fuxi_modem_stats[12] = static_cast<int32_t>(gOwner.pas_supported[0]);
    native_fuxi_modem_stats[13] = static_cast<int32_t>(gOwner.pas_supported[1]);
    native_fuxi_modem_stats[14] = static_cast<int32_t>(gOwner.pas_support_x0[0]);
    native_fuxi_modem_stats[15] = static_cast<int32_t>(gOwner.pas_support_x0[1]);
    /* The assign's answer rides in the entries-listed and directories-found slots, which only the
       read-only inventory modes use. The runtime path never fills them, and the assign is what the
       runtime path needs to report. */
    native_fuxi_modem_stats[10] = static_cast<int32_t>(gOwner.assign_x0);
    native_fuxi_modem_stats[11] = static_cast<int32_t>(gOwner.assign_x1);
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

/* One SMC64 call. The six register values go in as the service's convention says: x0 the function id,
   x1 the argument-type word, and x2..x5 the call's own arguments. The quirk's session register is x6,
   so a call that wants to be resumed carries it there, and x7 is unused. */
ScmResult smcCall(uint32_t svc, uint32_t command, uint32_t arginfo,
                   uint64_t x2, uint64_t x3, uint64_t x4, uint64_t x5)
{
    ScmResult result = {-1, 0};
    unsigned resumes = 0;
    for (unsigned busy = 0; busy < 20; ++busy) {
        uint64_t function = qcom_scm_fn(svc, command), session = 0;
        for (;;) {
            register uint64_t x0 __asm__("x0") = function;
            register uint64_t x1 __asm__("x1") = arginfo;
            register uint64_t r2 __asm__("x2") = x2;
            register uint64_t r3 __asm__("x3") = x3;
            register uint64_t r4 __asm__("x4") = x4;
            register uint64_t r5 __asm__("x5") = x5;
            register uint64_t x6 __asm__("x6") = session;
            register uint64_t x7 __asm__("x7") = 0;
            __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(r2), "+r"(r3), "+r"(r4), "+r"(r5), "+r"(x6), "+r"(x7)
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
    gOwner.scm_pas = static_cast<uint32_t>(x2);
    syncStats();
    return result;
}

/* One SMC64 call to the peripheral authentication service. Its convention puts the PAS id in x2 and
   the call's own arguments in x3..x5. */
ScmResult secureCall(uint32_t command, uint32_t arginfo, uint32_t pas, uint64_t a, uint64_t b, uint64_t c)
{
    return smcCall(QCOM_SCM_SVC_PIL, command, arginfo, pas, a, b, c);
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

/* The modem's own account of why it stopped. The crash reason lives in SMEM item 421 (the mpss_resource entry
   of Linux's remoteproc: crash_reason_smem 421), and the item holds the reason code and the register state the
   modem saved when it went down. A modem that answered and then went quiet has written something there, and
   that is the only statement of the cause that comes from the modem itself.

   The item is read, never written: the crash reason belongs to the modem. */
void readCrashReason()
{
    if (!gOwner.smem_ready) return;
    uint32_t size = 0;
    volatile uint8_t *data = item(421, &size);
    if (!data) {
        printf("FUXI_MODEM: the modem's crash reason item is not there\n");
        return;
    }
    printf("FUXI_MODEM: the modem's crash reason item is %u bytes:", size);
    for (uint32_t i = 0; i < size && i < 64; ++i) printf(" %02x", data[i]);
    printf("\n");
    /* The reason code is the first word (Linux's qcom_smem: the crash reason is the reason code followed by the
       registers). The names Linux gives them are in include/linux/soc/qcom/qmi.h's crash reason list, which the
       port does not carry, so the code is reported as a number. */
    if (size >= 4) printf("FUXI_MODEM: the modem's crash reason code is %u\n", *(volatile uint32_t *)data);
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

/* Reads the .mdt file, which is the file the secure world authenticates the metadata out of. */
int firmwareReadMdt(void *context, uint64_t offset, void *to, size_t length)
{
    return fuxi_adsp_fw_read_mdt(context, offset, to, length);
}

/* Builds the metadata blob for an image: the ELF header followed by the hash segment, both taken out
 * of the .mdt file, which is the form the secure world authenticates (Linux drivers/soc/qcom/
 * mdt_loader.c, qcom_mdt_read_metadata). The joined image of the .bNN pieces has the hash at another
 * offset in another file, and the two are not the same bytes. */
int buildMetadata(Image &image, uint8_t *va, uint64_t bytes)
{
    return image.mdt.mdt_size
        ? qcom_mdt_metadata_from_mdt(&image.mdt, firmwareRead, image.file, firmwareReadMdt, image.file, va, bytes)
        : qcom_mdt_metadata(&image.mdt, firmwareRead, image.file, va, bytes);
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
    /* The .mdt file is the header part followed by the hash segment, and it is what the secure world
     * authenticates, so its size is recorded and its bytes are read on their own. */
    image.mdt.mdt_size = fuxi_adsp_fw_mdt_size(image.file);
    return 0;
}

void releaseImage(Image &image)
{
    /* The metadata is a slice of the low window, so there is nothing to release: the window stays
     * mapped for the rest of the boot. What has to happen is that the slice is cleared, so a later
     * image cannot find the previous one's bytes there. */
    if (image.has_meta && image.meta.va) {
        for (uint64_t i = 0; i < image.meta.bytes; ++i) image.meta.va[i] = 0;
        __asm__ volatile("dsb sy" ::: "memory");
        image.meta = {};
        image.has_meta = false;
    }
    if (image.file) {
        fuxi_adsp_fw_close(image.file);
        image.file = nullptr;
    }
}

/* The page the metadata lives in. Linux wants the metadata's buffer to be physically contiguous, page
 * aligned and not cacheable (drivers/firmware/qcom/qcom_scm.c, qcom_scm_pas_init_image), so the buffer
 * is a whole page and the page is mapped without a cache. */
constexpr uint64_t kMetadataPageBytes = 4096;

/* The metadata's buffer, in a fixed low DRAM window.
 *
 * The window is mapped once, when the modem's sequence starts, and every metadata buffer is a slice of
 * it. The address is fixed and below 4 GiB because upstream's is: Linux takes it from
 * dma_alloc_coherent on the SCM platform device, whose masks default to 32 bits, so upstream's
 * metadata always lands below 4 GiB or the allocation fails (the mainline fuxi port saw -12 and fixed
 * it with CONFIG_ZONE_DMA and CONFIG_ZONE_DMA32, whose only effect is to move the allocation below
 * 4 GiB).
 *
 * A physical mask cannot do this here. XNU's initWithPhysicalMask turns the mask into a highestMask
 * and an alignment, and IOKernelAllocateWithPhysicalRestrict drops any maximum physical address of
 * 0xFFFFFFFF or below to zero and substitutes KMA_LOMEM instead; on arm64 vm_lopage_needed is false
 * (only i386 sets it), so the low-memory request is ignored and the allocation lands wherever it
 * lands. That is why the audio chain's 32-bit mask produced 0x8CFF84000 and the modem's metadata
 * 0x8d39ab000, both above 4 GiB.
 *
 * So the address is fixed, and IODeviceMemory::withRange is the mechanism: the audio chain already
 * proved that recipe on this board (package/FuxiAdspProbe.cpp, probeOwnedPcmSmmu). */
struct LowMetaWindow {
    IOMemoryMap *map;
    volatile uint8_t *va;
    uint64_t size;
    bool mapped;
};

LowMetaWindow gLowMeta;

/* Maps the low DRAM window the metadata lives in. The window has to be outside everything else on the
 * board, and the header holds the list of what it must not touch; the check is here rather than in the
 * header so that it runs on the device, where the port's own windows are known. */
bool mapLowMeta()
{
    if (gLowMeta.mapped) return true;
    const uint64_t base = FUXI_MODEM_LOW_META_BASE;
    const uint64_t size = FUXI_MODEM_LOW_META_SPAN;
    /* The port's own windows and the modem's carveouts are checked here, on the device. The list in the
     * header is the device tree's; these are the ones the port itself maps. */
    if (fuxi_modem_low_meta_overlaps(base, size, kModemCarveoutBase, kModemCarveoutSize) ||
        fuxi_modem_low_meta_overlaps(base, size, kModemDtbCarveoutBase, kModemDtbCarveoutSize) ||
        fuxi_modem_low_meta_overlaps(base, size, FUXI_MODEM_DSM_BASE, FUXI_MODEM_DSM_SIZE)) {
        printf("FUXI_MODEM: the low metadata window at %llx overlaps a region the modem owns\n",
               static_cast<unsigned long long>(base));
        return false;
    }
    /* The kernel's own physical window. The kernel lives above 4 GiB on this board, so a window below
     * 4 GiB cannot be inside it, but the check is made anyway: it costs nothing and it is the check
     * that would catch the window being moved. */
    if (gPhysBase && gPhysBase < base + size && base < gPhysBase + gPhysSize) {
        printf("FUXI_MODEM: the low metadata window at %llx is inside the kernel's physical window\n",
               static_cast<unsigned long long>(base));
        return false;
    }
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    if (!memory) {
        printf("FUXI_MODEM: the low metadata window at %llx could not be described\n",
               static_cast<unsigned long long>(base));
        return false;
    }
    /* kIOMapAnywhere is required: without it the fixed mapping fails silently, which is what the
     * audio chain found (the map() call reports nothing and the page is not there). */
    gLowMeta.map = memory->map(kIOMapAnywhere | kIOMapWriteCombineCache);
    memory->release();
    if (!gLowMeta.map) {
        printf("FUXI_MODEM: the low metadata window at %llx could not be mapped\n",
               static_cast<unsigned long long>(base));
        return false;
    }
    gLowMeta.va = reinterpret_cast<volatile uint8_t *>(gLowMeta.map->getVirtualAddress());
    gLowMeta.size = size;
    gLowMeta.mapped = gLowMeta.va != nullptr;
    if (!gLowMeta.mapped) {
        gLowMeta.map->release();
        gLowMeta.map = nullptr;
        printf("FUXI_MODEM: the low metadata window at %llx mapped to nothing\n",
               static_cast<unsigned long long>(base));
        return false;
    }
    /* The whole window is zeroed before anything is put in it, so a slice that is not written is still
     * zero, which is what the secure world expects for the part of the page the metadata does not
     * reach. */
    for (uint64_t i = 0; i < size; ++i) gLowMeta.va[i] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    printf("FUXI_MODEM: the low metadata window is %llx of %llu bytes, below 4 GiB\n",
           static_cast<unsigned long long>(base), static_cast<unsigned long long>(size));
    return true;
}

/* A metadata buffer in the low window. It is a slice of the mapped window, so its address is the
 * window's address plus the slice's offset, and it is the address the secure world is given.
   The caller fills it and cleans it to the point of coherency before the call. */
bool allocLowMeta(unsigned which, uint64_t bytes, LowMeta &out)
{
    if (!mapLowMeta()) return false;
    if (!fuxi_modem_low_meta_fits(which, bytes)) {
        printf("FUXI_MODEM: a metadata of %llu bytes does not fit the window's slot\n",
               static_cast<unsigned long long>(bytes));
        return false;
    }
    out.va = gLowMeta.va + fuxi_modem_low_meta_offset(which);
    out.phys = fuxi_modem_low_meta_address(which);
    out.bytes = bytes;
    return true;
}

/* A metadata buffer for the diagnostics. Every attempt needs a fresh buffer, because the secure world
   may write into it, and the diagnostics make their attempts one after another, so the window's blob
   slot is zeroed before each one. */
bool allocDiagnosticMeta(Image &image, uint64_t bytes)
{
    if (!allocLowMeta(FuxiModemLowMetaDtb, bytes, image.meta)) return false;
    image.has_meta = true;
    const uint64_t occupied = fuxi_modem_low_meta_occupied(bytes);
    for (uint64_t i = 0; i < occupied; ++i) image.meta.va[i] = 0;
    return true;
}

/* A page of memory the secure world can read: physically contiguous and page aligned.
   The caller fills it and cleans it to the point of coherency before the call.

   The page is not mapped without a cache. Upstream's own allocation is dma_alloc_coherent, which on
   ARM64 is cacheable but coherent, and it cleans nothing: the coherence is the DMA API's job. The
   port's equivalent of that coherence is the cleanToPoc the caller does. The uncacheable mapping
   (kIOMapInhibitCache, which becomes MAP_MEM_IO and VM_WIMG_IO) was tried on the device on
   2026-11-20 and the modem's thread never got past the allocation, so it is not used. */
IOBufferMemoryDescriptor *allocSecurePage()
{
    IOBufferMemoryDescriptor *buffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, static_cast<IOOptionBits>(kIODirectionInOut) |
                         static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),
        kMetadataPageBytes, 0xfffffffffffff000ull);
    if (!buffer || buffer->prepare() != kIOReturnSuccess) {
        if (buffer) buffer->release();
        return nullptr;
    }
    uint8_t *va = static_cast<uint8_t *>(buffer->getBytesNoCopy());
    if (!va) {
        buffer->release();
        return nullptr;
    }
    bzero(va, static_cast<size_t>(kMetadataPageBytes));
    return buffer;
}

/* Hands the metadata to the secure world and, for a relocatable image, tells it where the image will live.
 * The metadata buffer has to be in memory the secure world can read, so it is a physically contiguous
 * allocation cleaned to the point of coherency before the call. */
/* Tells the secure world where the metadata lives and, for a relocatable image, where the image will live.
   It stops short of the reset on purpose: Linux's qcom_mdt_pas_load does the metadata and the segment load
   and leaves "authentication of the segments done by a separate call", so the reset belongs after the
   segments are in the carveout, not before. The caller does that call.

   The metadata lives in a fixed low DRAM window, below 4 GiB, because upstream's does: Linux takes the
   metadata's address from dma_alloc_coherent on the SCM platform device, whose masks default to 32
   bits, so upstream's metadata always lands below 4 GiB or the allocation fails. The window's
   reasoning is in FuxiModemLowMeta.h.

   The buffer is page aligned and the rest of the page is zero, which Linux also asks for
   (drivers/firmware/qcom/qcom_scm.c, qcom_scm_pas_init_image: "During the scm call memory protection
   will be enabled for the meta data blob, so make sure it's physically contiguous, 4K aligned and
   non-cachable to avoid XPU violations"). The page is mapped write-combined rather than uncacheable:
   the uncacheable mapping hung the modem's thread on the device on 2026-11-20, and the write-combined
   one is what the audio chain proved on this board. */
int authenticateHeader(Image &image, uint32_t pas, uint64_t base, unsigned which)
{
    const uint64_t bytes = qcom_mdt_metadata_size(&image.mdt);
    if (!bytes) return -5;
    if (!allocLowMeta(which, bytes, image.meta)) return -12;
    image.has_meta = true;
    uint8_t *va = const_cast<uint8_t *>(image.meta.va);
    /* The page is zeroed first, so the part of the page the metadata does not reach is zero. */
    const uint64_t occupied = fuxi_modem_low_meta_occupied(bytes);
    for (uint64_t i = 0; i < occupied; ++i) va[i] = 0;
    /* The metadata is the ELF header followed by the hash segment, both taken out of the .mdt file:
     * that is the form the secure world authenticates (Linux drivers/soc/qcom/mdt_loader.c,
     * qcom_mdt_read_metadata). The joined image of the .bNN pieces has the hash at another offset in
     * another file, and the two are not the same bytes. */
    if (buildMetadata(image, va, bytes)) return -5;
    /* The whole page is cleaned, not just the metadata, because the page is what the secure world is
     * given. */
    cleanToPoc(va, static_cast<size_t>(occupied));
    const uint64_t phys = image.meta.phys;
    /* The secure world reads this metadata buffer. Its address and size are what it looks at, so
     * they are printed: a refusal that is not about the pas id is most likely about them. */
    printf("FUXI_MODEM: the metadata is %llu bytes at %llx, hash %d, %s, from the %s\n",
           static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(phys),
           image.mdt.hash, image.mdt.relocatable ? "relocatable" : "fixed",
           image.mdt.mdt_size ? "mdt file" : "joined image");
    if (phys & 0xfffull) printf("FUXI_MODEM: the metadata's address is not page aligned\n");
    if (phys >= 0x100000000ull) printf("FUXI_MODEM: the metadata's address is above 4 GiB\n");

    unsigned steps[FuxiModemPasStepCount];
    const unsigned count = fuxi_modem_pas_header_steps(image.mdt.relocatable ? 1 : 0, steps, FuxiModemPasStepCount);
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
        printf("FUXI_MODEM: pas %u step %u answers %u/%u\n", pas, steps[i],
               static_cast<uint32_t>(result.x0), static_cast<uint32_t>(result.x1));
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


bool readCmdDb(uint8_t *copy);

/* The whole command database, entry by entry: the resource name, the RPMh address the vote goes to, and the levels
 * the resource takes (its aux data). Read only: it maps the database, copies it and prints. It is here because the
 * IPA's clock and its three interconnect paths are voted through this table and their names have to be read off the
 * board rather than guessed. */
int dumpCmdDb()
{
    struct CmdDbCopy {
        uint8_t *bytes;
        uint32_t bytes_size;
        explicit CmdDbCopy(uint32_t size) : bytes(static_cast<uint8_t *>(IOMallocData(size))), bytes_size(size) {}
        ~CmdDbCopy() { if (bytes) IOFreeData(bytes, bytes_size); }
    } copy(kCmdDbSize);
    if (!copy.bytes || !readCmdDb(copy.bytes)) {
        printf("FUXI_MODEM: the command database cannot be read\n");
        return -1;
    }
    const uint8_t *db = copy.bytes;
    const uint32_t magic = (uint32_t)db[4] | (uint32_t)db[5] << 8 | (uint32_t)db[6] << 16 | (uint32_t)db[7] << 24;
    printf("FUXI_MODEM: the command database magic %#010x, %u bytes at %#llx\n", magic, kCmdDbSize, kCmdDbBase);
    if (magic != 0x0c0330dbu) {
        printf("FUXI_MODEM: not a command database\n");
        return -2;
    }
    unsigned total = 0;
    for (unsigned slave_index = 0; slave_index < 8; ++slave_index) {
        const uint8_t *slave = db + 8 + 16 * slave_index;
        const unsigned id = (unsigned)slave[0] | (unsigned)slave[1] << 8;
        if (!id) break;
        const unsigned heads = 144u + ((unsigned)slave[2] | (unsigned)slave[3] << 8);
        const unsigned data = 144u + ((unsigned)slave[4] | (unsigned)slave[5] << 8);
        const unsigned count = (unsigned)slave[6] | (unsigned)slave[7] << 8;
        printf("FUXI_MODEM: cmd-db slave %u (id %u) holds %u entries\n", slave_index, id, count);
        for (unsigned e = 0; e < count; ++e) {
            const uint8_t *entry = db + heads + 24u * e;
            char name[9];
            for (unsigned i = 0; i < 8; ++i) {
                const uint8_t c = entry[i];
                name[i] = (c >= 32u && c < 127u) ? static_cast<char>(c) : '.';
            }
            name[8] = 0;
            const uint32_t address = (uint32_t)entry[16] | (uint32_t)entry[17] << 8 | (uint32_t)entry[18] << 16 |
                                     (uint32_t)entry[19] << 24;
            const unsigned aux_length = (unsigned)entry[20] | (unsigned)entry[21] << 8;
            const unsigned aux_offset = (unsigned)entry[22] | (unsigned)entry[23] << 8;
            printf("FUXI_MODEM: cmd-db %-8s address %#010x levels", name, address);
            for (unsigned l = 0; l + 1 < aux_length && l < 16u; l += 2) {
                const unsigned level = (unsigned)db[data + aux_offset + l] | (unsigned)db[data + aux_offset + l + 1] << 8;
                printf(" %u", level);
            }
            printf("\n");
            ++total;
        }
    }
    printf("FUXI_MODEM: the command database holds %u entries\n", total);
    return 0;
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
    /* The corner is the highest one, which is what Linux votes: its sm8550_mpss_resource
     * carries no performance state, so it asks for INT_MAX and rpmhpd steps back one entry
     * from the end. The helper that says so is the one the host test pins. A rail with no
     * levels has no corner to vote, so the vote is refused rather than sent as zero. */
    if (!cx.level_count) return -2;
    cx_index = fuxi_rpmh_highest_corner(&cx);
    if (gOwner.mss_rail_found) {
        if (!mss.level_count) return -2;
        mss_index = fuxi_rpmh_highest_corner(&mss);
    }

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


/* The IPA's core clock, and the read that was hanging before it.
 *
 * The device tree gives the IPA node clocks = <&rpmhcc 12> (fuxi-live.dts:29416; the mainline DTB agrees: the
 * provider is qcom,kalama-rpmh-clk, index 12). Index 12 of that controller is RPMH_IPA_CLK
 * (dt-bindings/clock/qcom,rpmh.h:21), and clk-rpmh.c:412 defines it as DEFINE_CLK_RPMH_BCM(ipa, "IP0") -- so the
 * vote goes to the command database's IP0 resource, which this board has at 0x50040.
 *
 * A BCM clock votes a command word and not a level: clk-rpmh.c:271 sends BCM_TCS_CMD(1, enable, 0, 1), and with
 * tcs.h:68-79 that is commit bit 30 | valid bit 29 | vote_y 1 = 0x60000001.
 *
 * Nothing here is guessed: each of those four facts is a line in a file on this machine. */
int voteIpaClock()
{
    if (!gOwner.rsc_window.va) return -1;
    struct CmdDbCopy {
        uint8_t *bytes;
        uint32_t bytes_size;
        explicit CmdDbCopy(uint32_t size) : bytes(static_cast<uint8_t *>(IOMallocData(size))), bytes_size(size) {}
        ~CmdDbCopy() { if (bytes) IOFreeData(bytes, bytes_size); }
    } copy(kCmdDbSize);
    if (!copy.bytes || !readCmdDb(copy.bytes)) return -2;
    QcomRpmhResource ipa = {}, config = {}, memory = {};
    if (qcom_cmd_db_find(copy.bytes, kCmdDbSize, kIpaClockResource, &ipa) ||
        qcom_cmd_db_find(copy.bytes, kCmdDbSize, kIpaConfigBcm, &config) ||
        qcom_cmd_db_find(copy.bytes, kCmdDbSize, kIpaMemoryBcm, &memory)) {
        printf("FUXI_MODEM: the ipa power resources are not all in this board command database\n");
        return -3;
    }
    printf("FUXI_MODEM: the ipa power: clock %s at %#010x, paths %s at %#010x and %s at %#010x\n",
           kIpaClockResource, ipa.address, kIpaConfigBcm, config.address, kIpaMemoryBcm, memory.address);
    /* The reference order is the interconnects first, the clock second (ipa_power.c:100-104), and all three fit in
       one TCS (qcom_rpmh_active_request takes up to four commands). */
    const uint32_t addresses[3] = {config.address, memory.address, ipa.address};
    const uint32_t values[3] = {kIpaBcmKeepalive, kIpaBcmKeepalive, kIpaClockEnable};
    QcomRpmhWrite writes[16];
    const unsigned written = qcom_rpmh_active_request(&gOwner.layout, kRscTcs, addresses, values, 3, writes, 16);
    if (!written) return -4;
    const uint32_t irq_enable = rd(gOwner.rsc_window, kRscTcsOffset) | (1u << kRscTcs);
    wr(gOwner.rsc_window, kRscTcsOffset, irq_enable);
    __atomic_store_n(&gOwner.rsc_done, 0, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < written; ++i) wr(gOwner.rsc_window, kRscTcsOffset + writes[i].offset, writes[i].value);
    uint64_t deadline = 0;
    clock_interval_to_deadline(100, kMillisecondScale, &deadline);
    while (!(__atomic_load_n(&gOwner.rsc_done, __ATOMIC_ACQUIRE) & (1u << kRscTcs)) && mach_absolute_time() < deadline)
        serviceEvents(waitEvents(20));
    bool done = (__atomic_load_n(&gOwner.rsc_done, __ATOMIC_ACQUIRE) & (1u << kRscTcs)) != 0;
    if (!done && (rd(gOwner.rsc_window, kRscTcsOffset + gOwner.layout.irq_status) & (1u << kRscTcs))) done = true;
    const unsigned finish = qcom_rpmh_finish(&gOwner.layout, kRscTcs, writes, 16);
    for (unsigned i = 0; i < finish; ++i) wr(gOwner.rsc_window, kRscTcsOffset + writes[i].offset, writes[i].value);
    gOwner.ipa_clock_voted = done ? 1u : 2u;
    printf("FUXI_MODEM: the ipa power vote %s (interconnects %s and %s, clock %s)\n", done ? "completed" : "did NOT complete", kIpaConfigBcm, kIpaMemoryBcm, kIpaClockResource);
    return done ? 0 : -5;
}

/* Waits for the owner to be woken. The pending bits are taken out of the word, so a bit that arrives during the
 * service is not lost. The timeout is the upper bound on the wait, not a delay: the interrupt handlers wake the
 * owner as soon as something happens. */
uint32_t waitEvents(uint32_t ms)
{
    uint32_t bits = __atomic_exchange_n(&gOwner.pending, 0, __ATOMIC_ACQ_REL);
    if (bits) return bits;
    if (!assert_wait_timeout(reinterpret_cast<event_t>(&gOwner.pending), THREAD_UNINT, ms, kMillisecondScale)) {
        /* The wait timed out with nothing to report. The wait is taken off the thread here, so the
         * caller sees the timeout instead of blocking forever: a step that waits for the always-on
         * processor or for the modem's doorbells has to be able to give up and report. */
        clear_wait(current_thread(), THREAD_TIMED_OUT);
        return 0;
    }
    bits = __atomic_exchange_n(&gOwner.pending, 0, __ATOMIC_ACQ_REL);
    if (bits) {
        clear_wait(current_thread(), THREAD_AWAKENED);
        return bits;
    }
    /* The wait was woken but the bits were already taken, so there is nothing to report. Blocking here
     * without a timeout would strand the thread: the doorbell that woke it has already been accounted for,
     * and no one else would wake it again. The caller treats a timeout as "nothing happened", so the
     * thread returns and the caller polls again. */
    clear_wait(current_thread(), THREAD_AWAKENED);
    return 0;
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
    /* The GLINK itself belongs to the adapter in the component now, and the adapter's poll runs it. */
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
    /* The shape of each image, because the secure world may refuse one of them and the reason is
     * not in its answer. The counts, the header size and the relocatable flag are what decide which
     * steps the image gets, and the metadata's size and address are what the secure world looks at. */
    const Image *const images[2] = {&gOwner.dtb, &gOwner.modem};
    const char *const names[2] = {"dtb", "image"};
    for (unsigned i = 0; i < 2; ++i) {
        const QcomMdtImage *mdt = &images[i]->mdt;
        printf("FUXI_MODEM: the %s has %u segments, a %llu byte header, hash %d, %s, "
               "a %llu byte file, a %llu byte span, %llu to %llu\n",
               names[i], mdt->count, static_cast<unsigned long long>(mdt->header_size), mdt->hash,
               mdt->relocatable ? "relocatable" : "fixed",
               static_cast<unsigned long long>(mdt->file_size),
               static_cast<unsigned long long>(qcom_mdt_span(mdt)),
               static_cast<unsigned long long>(mdt->min_address),
               static_cast<unsigned long long>(mdt->max_address));
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
    /* The windows as they were mapped, so a fault in one of them can be told apart from a fault
     * in what is read out of it. The always-on processor's window is the one the mailbox lives
     * in, and it is the one the modem's handshake needs. */
    printf("FUXI_MODEM: the shared memory window is at %p of %llu bytes\n",
           (const void *)(const uintptr_t)gOwner.smem_window.va, (unsigned long long)gOwner.smem_window.size);
    printf("FUXI_MODEM: the interrupt window is at %p of %llu bytes\n",
           (const void *)(const uintptr_t)gOwner.ipcc_window.va, (unsigned long long)gOwner.ipcc_window.size);
    printf("FUXI_MODEM: the always-on processor's window is at %p of %llu bytes\n",
           (const void *)(const uintptr_t)gOwner.aoss_window.va, (unsigned long long)gOwner.aoss_window.size);
    printf("FUXI_MODEM: the resource's window is at %p of %llu bytes\n",
           (const void *)(const uintptr_t)gOwner.rsc_window.va, (unsigned long long)gOwner.rsc_window.size);
    gOwner.smem.base = gOwner.smem_window.va;
    gOwner.smem.size = kSmemSize;
    /* The modem's partition is the one shared with the modem itself: local pid 0, remote pid 1. */
    if (qcom_smem_partition(&gOwner.smem, QCOM_SMEM_HOST_APPS, kModemPid, &gOwner.part)) return -2;
    gOwner.smem_ready = true;
    setStage(kStMapped);
    return 0;
}

int stepIpaFirmware();   /* defined with the other PAS steps, below */

int stepRails()
{
    /* The RSC's own register layout: the id register says which major version it is, and the version moved the
     * TCS registers.  The DRV2 id register reads zero on this board, so the major version is forced to 3
     * rather than let the zero choose the version 2 layout: with that layout the TCS registers sit at the
     * wrong offsets and the request never completes.  The ADSP bring-up reads the same window and found
     * the same zero, so the fallback is the one the hardware is known to need. */
    const uint32_t drv_id = rd(gOwner.rsc_window, 0);
    gOwner.rsc_drv_id = fuxi_rsc_drv_id(drv_id);
    gOwner.layout = fuxi_rsc_layout(drv_id);
    const int rc = voteRails();
    if (rc) {
        printf("FUXI_MODEM: the proxy rails could not be voted (%d)\n", rc);
        return rc;
    }
    /* The IPA clock, and nothing after it.
       2026-11-23 01:11: with the vote completed, the very next read (SHARED_MEM_SIZE at 0x54) still wedged the probe
       thread: the log stops at "step: the proxy rails" and no later step runs, while the rest of the system boots.
       So the clock is not the only thing gating the block on this SoC -- a power domain or a GDSC sits in front of
       it and does not show up in the device tree as a clock consumer. The vote is kept because it completed and is
       harmless; the read waits for that power domain to be found. */
    const int clock_rc = voteIpaClock();
    if (clock_rc) printf("FUXI_MODEM: the ipa clock vote did not complete (%d)\n", clock_rc);
    /* The firmware step is behind its own boot argument. It works -- the secure world answers 0/0 on all three
       steps and the block is released -- but the image that runs it did not reach the adapter handover, so it stays
       off the normal path until that is understood. The votes above are not gated: they are harmless and proven. */
    {
        uint32_t firmware = 0;
        if (PE_parse_boot_argn("sheng-ipa-fw", &firmware, sizeof firmware) && firmware) (void)stepIpaFirmware();
    }

    setStage(kStRails);
    return 0;
}

/* The memory-ownership step: the modem's DSM region is handed from the hypervisor's own owner to the
 * modem's MSA before the modem is started at all.
 *
 * This is the one structural difference between the ADSP's path and the modem's, and the ADSP's path
 * does not have it: Linux drivers/remoteproc/qcom_q6v5_pas.c, sm8550_mpss_resource, carries
 * region_assign_idx 2, region_assign_count 1 and region_assign_vmid QCOM_SCM_VMID_MSS_MSA, while the
 * ADSP's entry carries no region_assign_idx. Linux runs it in the probe, after the memory regions are
 * resolved and before any PAS call, so that is where it goes here too.
 *
 * The call is the memory-protection service, not the peripheral authentication service, and it takes
 * seven arguments. Seven arguments do not fit in SMC64's registers the way the PAS calls do, so
 * upstream's transport (drivers/firmware/qcom/qcom_scm-smc.c) puts the first three in x2..x4 and the
 * rest in a separate block whose address is what x5 carries. */
int stepAssign()
{
    printf("FUXI_MODEM: the assign starts\n");
    const unsigned src_count = 1, dest_count = 1;
    const uint64_t block_bytes = fuxi_modem_assign_block_size(src_count, dest_count);
    const uint64_t ext_bytes = FUXI_MODEM_ASSIGN_EXT_SIZE;

    /* One allocation holds both blocks: the assignment's own block and the external arguments. They
     * are separate because the secure world reads the external block by address, and the assignment
     * block's parts have to start on 64-byte boundaries. */
    const uint64_t total = block_bytes + ext_bytes;
    if (total > kMetadataPageBytes) return -5;
    IOBufferMemoryDescriptor *buffer = allocSecurePage();
    if (!buffer) return -12;
    uint8_t *va = static_cast<uint8_t *>(buffer->getBytesNoCopy());
    if (!va) {
        buffer->release();
        return -5;
    }
    printf("FUXI_MODEM: the assign has its page\n");

    const uint64_t base = buffer->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    uint8_t *block = va;
    uint8_t *ext = va + block_bytes;

    /* The memory-to-map record: the region's address and its size. It is the second part of the
     * assignment block, and the secure world is given its address. */
    const uint64_t map_offset = fuxi_modem_assign_mem_map_offset(src_count);
    const uint64_t mem_to_map_phys = base + map_offset;
    const uint64_t dest_offset = fuxi_modem_assign_perm_offset(src_count, dest_count);
    const uint64_t dest_phys = base + dest_offset;
    const uint64_t src_size = (uint64_t)src_count * FUXI_MODEM_ASSIGN_SRC_ENTRY;
    const uint64_t dest_size = (uint64_t)dest_count * FUXI_MODEM_ASSIGN_PERM_SIZE;

    fuxi_modem_assign_write_block(block, FUXI_MODEM_DSM_BASE, FUXI_MODEM_DSM_SIZE,
                                  FUXI_MODEM_VMID_HLOS, FUXI_MODEM_VMID_MSS_MSA, FUXI_MODEM_PERM_RW);
    fuxi_modem_assign_write_ext(ext, src_size, dest_phys, dest_size);
    printf("FUXI_MODEM: the assign has written its blocks\n");
    cleanToPoc(va, static_cast<size_t>(total));
    printf("FUXI_MODEM: the assign has cleaned its page\n");

    printf("FUXI_MODEM: the assign hands %llx of %llu bytes from vmid %u to vmid %u, the block is at %llx\n",
           static_cast<unsigned long long>(FUXI_MODEM_DSM_BASE),
           static_cast<unsigned long long>(FUXI_MODEM_DSM_SIZE),
           FUXI_MODEM_VMID_HLOS, FUXI_MODEM_VMID_MSS_MSA, static_cast<unsigned long long>(base));

    const uint64_t ext_phys = base + block_bytes;
    const ScmResult result = smcCall(FUXI_MODEM_MP_SVC, FUXI_MODEM_MP_ASSIGN, fuxi_modem_assign_arginfo(),
                                     mem_to_map_phys, FUXI_MODEM_ASSIGN_MEM_MAP_SIZE, base, ext_phys);
    gOwner.assign_x0 = gOwner.scm_x0;
    gOwner.assign_x1 = gOwner.scm_x1;
    buffer->release();
    printf("FUXI_MODEM: the assign's answer is %u/%u\n", gOwner.scm_x0, gOwner.scm_x1);
    if (!fuxi_modem_scm_ok(result.x0, result.x1)) {
        printf("FUXI_MODEM: the assign was refused (scm %u/%u)\n", gOwner.scm_x0, gOwner.scm_x1);
        return -3;
    }
    return 0;
}

/* The device tree blob first, then the modem image, as Linux does.
 *
 * Linux splits this into two phases and the port now does too, because the always-on processor's
 * message belongs between them. Upstream drivers/remoteproc/qcom_q6v5_pas.c, qcom_pas_start, and
 * remoteproc_core.c, rproc_start:
 *
 *   the load phase: rproc_start calls rproc_load_segments, which calls qcom_pas_load, which does
 *   the device tree blob's INIT_IMAGE (and MEM_SETUP if the blob is relocatable) and copies the
 *   blob's segments into its carveout. Nothing else.
 *
 *   the start phase: qcom_pas_start calls qcom_q6v5_prepare, which votes for the interconnect's
 *   bandwidth and sends "{class: image, res: load_state, name: modem, val: on}" to the always-on
 *   processor; then the blob's AUTH_AND_RESET; then the modem image's own INIT_IMAGE, MEM_SETUP
 *   and segment copy; then the modem image's AUTH_AND_RESET; then the wait for the modem's first
 *   words.
 *
 * The port had the always-on processor's message after both resets, and on 2026-11-20 the modem
 * image's AUTH_AND_RESET never returned with it there. Both upstream Linux and the reference port
 * for the same modem in XNU (hakios's drivers/modem/hakios.mpss.c, step 7) put the message before
 * the reset, so it goes here too. */

/* The load phase: the device tree blob's metadata is given to the secure world and its segments
 * are copied into its carveout. The blob is not reset yet. */
int stepPasLoad()
{
    printf("FUXI_MODEM: step: the modem's shutdown\n");
    (void)secureCall(QCOM_SCM_PAS_SHUTDOWN, qcom_scm_args(1, 0, 0, 0), fuxi_modem_pas_id(FuxiModemPasImageModem), 0, 0, 0);
    (void)secureCall(QCOM_SCM_PAS_SHUTDOWN, qcom_scm_args(1, 0, 0, 0), fuxi_modem_pas_id(FuxiModemPasImageDtb), 0, 0, 0);

    printf("FUXI_MODEM: step: the device tree blob's metadata\n");
    const int rc = authenticateHeader(gOwner.dtb, fuxi_modem_pas_id(FuxiModemPasImageDtb), kModemDtbCarveoutBase,
                                       FuxiModemLowMetaDtb);
    if (!rc) {
        printf("FUXI_MODEM: step: the device tree blob's segments\n");
        return loadSegments(gOwner.dtb, kModemDtbCarveoutBase, kModemDtbCarveoutSize);
    }
    printf("FUXI_MODEM: the modem device tree blob was refused (scm %u/%u)\n", gOwner.scm_x0, gOwner.scm_x1);
    shutdownModem(true, false);
    return rc;
}

/* The start phase: the blob is reset, then the modem image is given to the secure world and reset
 * as well. */
int stepPasStart()
{
    int rc = 0;
    /* The reset comes after the segments are in the carveout, once per image. */
    printf("FUXI_MODEM: step: the device tree blob's reset\n");
    const ScmResult auth = secureCall(fuxi_modem_pas_command(fuxi_modem_pas_reset_step),
                                       fuxi_modem_pas_arginfo(fuxi_modem_pas_reset_step),
                                       fuxi_modem_pas_id(FuxiModemPasImageDtb), 0, 0, 0);
    printf("FUXI_MODEM: pas %u step %u answers %u/%u\n", fuxi_modem_pas_id(FuxiModemPasImageDtb),
           fuxi_modem_pas_reset_step, static_cast<uint32_t>(auth.x0), static_cast<uint32_t>(auth.x1));
    if (!fuxi_modem_scm_ok(auth.x0, auth.x1)) rc = -3;
    else gOwner.dtb_auth_cookie = gOwner.scm_x0;
    if (rc) {
        printf("FUXI_MODEM: the modem device tree blob was refused (scm %u/%u)\n", gOwner.scm_x0, gOwner.scm_x1);
        shutdownModem(true, false);
        return rc;
    }
    gOwner.dtb.reset = true;
    setStage(kStDtb);
    /* The metadata is not released here. Linux keeps both metadata buffers until the modem has come
     * up (drivers/remoteproc/qcom_q6v5_pas.c, qcom_pas_start: qcom_pas_metadata_release is after
     * qcom_q6v5_wait_for_start), because the secure world turns on memory protection for that page
     * during the call and the page has to stay put until the call is done with it. Releasing it
     * right after the authentication would hand the page back while the secure world may still be
     * reading it. */

    printf("FUXI_MODEM: step: the modem image's metadata\n");
    rc = authenticateHeader(gOwner.modem, fuxi_modem_pas_id(FuxiModemPasImageModem), kModemCarveoutBase,
                            FuxiModemLowMetaImage);
    if (!rc) {
        printf("FUXI_MODEM: step: the modem image's segments\n");
        rc = loadSegments(gOwner.modem, kModemCarveoutBase, kModemCarveoutSize);
    }
    if (rc) {
        printf("FUXI_MODEM: the modem image was refused (%d)\n", rc);
        shutdownModem(true, false);
        return rc;
    }
    setStage(kStLoaded);
    printf("FUXI_MODEM: step: the modem image's reset\n");
    const ScmResult image_auth = secureCall(fuxi_modem_pas_command(fuxi_modem_pas_reset_step),
                                             fuxi_modem_pas_arginfo(fuxi_modem_pas_reset_step),
                                             fuxi_modem_pas_id(FuxiModemPasImageModem), 0, 0, 0);
    printf("FUXI_MODEM: pas %u step %u answers %u/%u\n", fuxi_modem_pas_id(FuxiModemPasImageModem),
           fuxi_modem_pas_reset_step, static_cast<uint32_t>(image_auth.x0), static_cast<uint32_t>(image_auth.x1));
    if (!fuxi_modem_scm_ok(image_auth.x0, image_auth.x1)) {
        printf("FUXI_MODEM: the modem was not released (scm %u/%u)\n", gOwner.scm_x0, gOwner.scm_x1);
        shutdownModem(true, true);
        return -3;
    }
    gOwner.modem.reset = true;
    gOwner.modem_auth_cookie = gOwner.scm_x0;
    return 0;
}

/* The IPA's firmware: the third image the AP hands the secure world. Nothing of the IPA answers the CPU until
 * this has been authenticated and released, which is what the aborts of 2026-11-23 were measuring.
 *
 * The four facts it rests on, each a line on this machine: the device tree gives the IPA its clock and its
 * interconnects and no power domain at all (fuxi-live.dts:29411-29416); the firmware node is a PIL node with
 * qcom,pil-force-shutdown and pas id 0x0f (:29394-29400); mainline loads the image from the AP because the node
 * names neither modem-init nor qcom,gsi-loader (ipa_main.c:737-741), and its probe returned -2 when the file was
 * missing (docs/目标-M5.md:404); and the region the images land in is all zeros on this board today, which is how
 * the absence of that load was visible at all. */
int stepIpaFirmware()
{
    const uint32_t pas = fuxi_modem_pas_id(FuxiModemPasImageIpa);
    printf("FUXI_MODEM: step: the ipa's firmware\n");
    /* qcom,pil-force-shutdown: the vendor driver forces the PIL down before it loads, so a stale image cannot
       survive a reboot of the IPA. The call is harmless when nothing is running. */
    (void)secureCall(QCOM_SCM_PAS_SHUTDOWN, qcom_scm_args(1, 0, 0, 0), pas, 0, 0, 0);
    int rc = openImage(gOwner.ipa, "modem:ipa_fws");
    if (rc) {
        printf("FUXI_MODEM: the ipa firmware is not there (%d)\n", rc);
        return rc;
    }
    printf("FUXI_MODEM: step: the ipa's metadata\n");
    rc = authenticateHeader(gOwner.ipa, pas, kIpaFwBase, FuxiModemLowMetaIpa);
    if (!rc) {
        printf("FUXI_MODEM: step: the ipa's segments\n");
        rc = loadSegments(gOwner.ipa, kIpaFwBase, kIpaFwBytes);
    }
    if (rc) {
        printf("FUXI_MODEM: the ipa firmware was refused (%d)\n", rc);
        return rc;
    }
    printf("FUXI_MODEM: step: the ipa's reset\n");
    const ScmResult reset = secureCall(fuxi_modem_pas_command(fuxi_modem_pas_reset_step),
                                       fuxi_modem_pas_arginfo(fuxi_modem_pas_reset_step), pas, 0, 0, 0);
    printf("FUXI_MODEM: pas %u step %u answers %u/%u\n", pas, fuxi_modem_pas_reset_step,
           static_cast<uint32_t>(reset.x0), static_cast<uint32_t>(reset.x1));
    if (!fuxi_modem_scm_ok(reset.x0, reset.x1)) {
        printf("FUXI_MODEM: the ipa was not released (scm %u/%u)\n", gOwner.scm_x0, gOwner.scm_x1);
        return -3;
    }
    gOwner.ipa.reset = true;
    printf("FUXI_MODEM: the ipa firmware is loaded and the block released\n");
    return 0;
}
/* The AOP mailbox: the handshake, then the message that releases the modem.
 *
 * The handshake and the message are two steps here, because Linux's order puts the message
 * *before* the modem is authenticated and the port had it after.
 * Linux drivers/remoteproc/qcom_q6v5.c, qcom_q6v5_prepare: the vote for the bandwidth comes
 * first, then q6v5_load_state_toggle(q6v5, true), which sends
 * "{class: image, res: load_state, name: modem, val: on}" to the always-on processor. That is
 * qcom_q6v5_prepare, which qcom_pas_start calls before anything else, so the message is on its
 * way before the device tree blob's AUTH_AND_RESET and long before the modem image's.
 *
 * The port had it the other way round, and the modem image's AUTH_AND_RESET never came back on
 * 2026-11-20. The reference port for the same modem in XNU, hakios's drivers/modem/hakios.mpss.c
 * (step 7), also sends it before: "ask TrustZone to authenticate the image and let the DSP out of
 * reset" comes after the mailbox message "{class: image, res: load_state, name: modem, val: on}".
 */

/* Opens the mailbox to the always-on processor. The channel has to be open before anything can be
 * sent through it.
 *
 * The handshake is stepped here, not left to the interrupt. qcom_qmp_step is what writes the first
 * half of the handshake out (the link state and the doorbell that asks the always-on processor to
 * look at it), so a caller that only waits for an interrupt never starts it: the interrupt is the
 * always-on processor's answer, and the question has to be asked first. The audio chain's own
 * mailbox does the same thing (package/FuxiAdspProbe.cpp, hookQmp: qcom_qmp_step in a loop before
 * the wait), and that one works on this board. */
int stepQmpOpen()
{
    qcom_qmp_init(&gOwner.qmp, reinterpret_cast<volatile uint32_t *>(gOwner.aoss_window.va), kAossSize, kickAop,
                  nullptr);
    /* The descriptor as it stands before anything is written to it: the always-on processor's side,
     * then ours, so a stall in the handshake says which half is missing an answer.
     *
     * The window is read word by word and each word is printed before the next one is read, so
     * the last word that arrives names the read that faulted. A read that faults kills the
     * kernel, so the word count is kept small and the words that matter come first: the magic
     * and the version are what upstream checks first. */
    printf("FUXI_MODEM: the mailbox is at %p of %u bytes\n", (const void *)(const uintptr_t)gOwner.aoss_window.va,
           kAossSize);
    printf("FUXI_MODEM: the mailbox descriptor before the handshake\n");
    volatile uint32_t *descriptor = reinterpret_cast<volatile uint32_t *>(gOwner.aoss_window.va);
    for (unsigned word = 0; word < 16; ++word) {
        const uint32_t value = descriptor[word];
        printf("FUXI_MODEM: the descriptor word %u is %08x\n", word, value);
    }
    /* The handshake is four steps and the always-on processor answers each with an interrupt, so the
     * loop steps it and waits for the answer, over and over, until the channel is open. */
    for (unsigned phase = 0; phase < 8; ++phase) {
        if (qcom_qmp_step(&gOwner.qmp) == QcomQmpOpen) break;
        if (gOwner.qmp.state < 0) break;
        uint64_t deadline = 0;
        clock_interval_to_deadline(1000, kMillisecondScale, &deadline);
        while (gOwner.qmp.state != QcomQmpOpen && gOwner.qmp.state >= 0 && mach_absolute_time() < deadline)
            serviceEvents(waitEvents(10) | kEvAop);
        printf("FUXI_MODEM: the handshake is at phase %u, the state is %d\n", phase, gOwner.qmp.state);
    }
    if (gOwner.qmp.state != QcomQmpOpen) {
        printf("FUXI_MODEM: the mailbox to the always-on processor did not open (state %d)\n", gOwner.qmp.state);
        printf("FUXI_MODEM: the mailbox descriptor after the handshake\n");
        for (unsigned word = 0; word < 16; ++word) {
            printf("FUXI_MODEM: the descriptor word %u is %08x\n", word,
                   gOwner.aoss_window.va ? gOwner.aoss_window.va[word] : 0u);
        }
        qcom_qmp_abandon(&gOwner.qmp);
        return -60;
    }
    printf("FUXI_MODEM: the mailbox to the always-on processor is open (box %u of %u bytes)\n",
           gOwner.qmp.box, gOwner.qmp.box_size);
    return 0;
}

/* The message itself: the always-on processor is told the modem's image is loaded and may be
 * released. */
int stepQmpLoadState()
{
    if (gOwner.qmp.state != QcomQmpOpen) {
        printf("FUXI_MODEM: the mailbox is not open (state %d), the message is not sent\n", gOwner.qmp.state);
        return -60;
    }
    printf("FUXI_MODEM: step: the always-on processor's load state\n");
    if (qcom_qmp_send(&gOwner.qmp, kLoadStateMessage) != QcomQmpSending) {
        printf("FUXI_MODEM: the load state message did not go in (state %d)\n", gOwner.qmp.state);
        return -60;
    }
    uint64_t deadline = 0;
    clock_interval_to_deadline(1000, kMillisecondScale, &deadline);
    while (gOwner.qmp.state == QcomQmpSending && mach_absolute_time() < deadline)
        serviceEvents(waitEvents(10) | kEvAop);
    if (gOwner.qmp.state != QcomQmpOpen) {
        printf("FUXI_MODEM: the always-on processor did not take the message (state %d)\n", gOwner.qmp.state);
        qcom_qmp_abandon(&gOwner.qmp);
        return -60;
    }
    gOwner.qmp_response = gOwner.qmp.ram[gOwner.qmp.box / 4];
    printf("FUXI_MODEM: the always-on processor answered the load state with %u\n", gOwner.qmp_response);
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

/* The GLINK edge, and with it the QRTR link. The adapter in the component owns the QRTR protocol and the
 * GLINK itself, so this only makes sure the three items exist and hands the adapter the SMEM window and the
 * doorbell. It used to build a second GLINK on the same items, which left two readers on one descriptor and
 * no way for the link's state to reach the report: the modem opened the channel, the adapter's GLINK never
 * heard it, and the link stayed down. */
struct LinkConfig {
    volatile uint8_t *smem_base;
    uint32_t smem_size;
    void (*kick)(void *context);
    void *kick_context;
    /* RMTFS: the board's adapter for the modem's remote file system. The struct is the component's,
       but the probe cannot include its header (the component is built separately), so the fields are
       spelled out here in the same order. */
    struct fuxi_rmtfs_ops rmtfs_ops;
    void *rmtfs_context;
    /* TFTP: the board's adapter for the modem's carrier configuration. The struct is the board's own
       header's, and the probe can include it: the header is in the board's package/ directory. */
    struct fuxi_tftp_ops tftp_ops;
    void *tftp_context;
};

int stepGlink()
{
    /* The three items are the GLINK edge's own: the descriptor, the transmit FIFO and the receive FIFO
     * (Linux drivers/rpmsg/qcom_glink_smem.c: SMEM_GLINK_NATIVE_XPRT_DESCRIPTOR 478, FIFO_0 479, FIFO_1 480).
     * They have to exist before the adapter looks for them, because the modem opens the channel into them. */
    int rc = allocItem(kGlinkDescItem, 32);
    if (!rc) rc = allocItem(kGlinkTxItem, kGlinkFifoBytes);
    if (rc) {
        printf("FUXI_MODEM: the GLINK items could not be made (%d)\n", rc);
        return rc;
    }
    uint32_t desc_size = 0, tx_size = 0, rx_size = 0;
    volatile uint8_t *desc = item(kGlinkDescItem, &desc_size);
    volatile uint8_t *tx = item(kGlinkTxItem, &tx_size);
    volatile uint8_t *rx = item(kGlinkRxItem, &rx_size);
    if (!desc || desc_size < 32 || !tx || tx_size < kGlinkFifoBytes || !rx || rx_size == 0) {
        printf("FUXI_MODEM: the GLINK items are missing (desc %u, tx %u, rx %u)\n", desc_size, tx_size, rx_size);
        return -2;
    }
    printf("FUXI_MODEM: the GLINK items are there (desc %u bytes, tx %u, rx %u)\n", desc_size, tx_size, rx_size);
    gOwner.glink_started = true;
    setStage(kStLink);
    return 0;
}

/* The component's adapter takes over the link: it finds the modem's items itself, so the window it needs is the
 * SMEM one. The doorbell is the one above. */
int stepAdapter()
{
    /* The board's adapter for RMTFS: the four files read into memory, and the buffer. */
    const struct fuxi_rmtfs_ops *ops = fuxi_rmtfs_ops();
    const struct fuxi_tftp_ops *tftp = fuxi_tftp_ops();
    const LinkConfig config = {gOwner.smem_window.va, kSmemSize, kickGlink, nullptr, *ops, nullptr, *tftp, nullptr};
    const int rc = fuxi_modem_start(&config);
    if (rc) {
        printf("FUXI_MODEM: the QRTR adapter refused the link (%d)\n", rc);
        return rc;
    }
    printf("FUXI_MODEM: the QRTR adapter took the link\n");
    return 0;
}

/* Runs the adapter's GLINK and its QRTR until the modem opens the channel and the first HELLO goes out.
 *
 * The adapter answers what the modem asks for, so the modem's channel open is only answered if something
 * runs the link. The bring-up used to stop right after handing the link over, which left the channel open
 * unanswered and the link down. */
int waitLink()
{
    LinkStatus status = {};
    uint64_t deadline = 0;
    clock_interval_to_deadline(10000, kMillisecondScale, &deadline);
    while (mach_absolute_time() < deadline) {
        /* The GLINK doorbell wakes the probe, so the event wait is what runs the link. */
        serviceEvents(waitEvents(10) | kEvGlink);
        fuxi_modem_poll();
        fuxi_modem_status(&status);
        gOwner.glink_up = status.ready ? 1 : 0;
        if (status.ready && status.hello_sent) {
            printf("FUXI_MODEM: the link is up and the first HELLO went out (tx %u, rx %u)\n",
                   status.tx_packets, status.rx_packets);
            return 0;
        }
    }
    fuxi_modem_status(&status);
    gOwner.glink_up = status.ready ? 1 : 0;
    printf("FUXI_MODEM: the link did not come up within 10 s (ready %u, hello %u, tx %u, rx %u, again %u, "
           "open failures %u, servers %u)\n",
           status.ready, status.hello_sent, status.tx_packets, status.rx_packets, status.tx_again,
           status.open_failures, status.qrtr_servers);
    return -6;
}

/* Keeps the link running for as long as the modem is up.
 *
 * Everything after the first HELLO arrives on the modem's own schedule: the reply to our HELLO, the
 * NEW_SERVER announcements for the services it offers, and the answers to what the bridge asks it.
 * The GLINK is a shared-memory transport and nobody pushes anything into it, so every one of those
 * waits for somebody to read the receive FIFO. The thread that brought the modem up is the thread
 * that has to keep reading it.
 *
 * The report goes out once a second, so the USB control channel sees the counters move rather than
 * a link that looks the same every time it is asked. */
void serviceLink()
{
    LinkStatus status = {};
    uint64_t next_report = 0;
    unsigned second = 0;
    for (;;) {
        serviceEvents(waitEvents(100) | kEvGlink);
        fuxi_modem_poll();
        fuxi_modem_status(&status);
        gOwner.glink_up = status.ready ? 1 : 0;
        if (mach_absolute_time() >= next_report) {
            clock_interval_to_deadline(1, kSecondScale, &next_report);
            second++;
            /* The heartbeat goes out every second whether or not anything moved, so a silent link and a
             * dead thread do not look the same. The counters below say where the traffic stops. */
            printf("FUXI_MODEM: %u s: the link is %s, QRTR tx %u rx %u, servers %u, glink irqs %u\n", second,
                   status.ready ? "up" : "down", status.tx_packets, status.rx_packets,
                   status.qrtr_servers, gOwner.glink_irqs);
            printf("FUXI_MODEM: %u s: glink rx %u tx %u data %u dropped %u unknown %u, fifo %u bytes, "
                   "channel up %u remote %u sent %u acked %u, our intents %u, their intents %u of which %u are held, "
                   "no room %u, too small %u, asked %u, done %u\n",
                   second, status.glink_rx_messages, status.glink_tx_messages, status.glink_rx_data,
                   status.glink_dropped, status.glink_unknown_opens, status.rx_avail,
                   status.ch_up, status.ch_remote_open, status.ch_open_sent, status.ch_open_acked,
                   status.ch_local_advertised, status.glink_remote_intents, status.glink_remote_busy,
                   status.glink_no_intent, status.glink_intent_too_small, status.glink_intent_requested,
                   status.glink_rx_done);
            /* The name service's state: the modem's node id, and whether the answer to its HELLO is still
             * owed. If the answer never lands, the modem never announces its services. */
            printf("FUXI_MODEM: %u s: qrtr rx %u bad %u, hello got %u, reply owed %u to node %u port %u, "
                   "their node %u known %u, hello dropped %u\n",
                   second, status.qrtr_rx, status.qrtr_malformed, status.qrtr_hello_received,
                   status.qrtr_hello_reply_owed, status.qrtr_hello_reply_node, status.qrtr_hello_reply_port,
                   status.qrtr_remote_node, status.qrtr_remote_node_known, status.qrtr_hello_dropped);
            /* The flow control: a peer that is out of credit stops answering, and the credit comes back with a
             * RESUME_TX. A pending count that sits at the high watermark says the link is waiting for one.
             * The count is the sum over the destinations, and the number of them is beside it: a table that is
             * filling up is what stops the answers from going out. */
            printf("FUXI_MODEM: %u s: the qrtr flow has %u packets pending over %u destination(s), %u resumes sent, %u received\n",
                   second, status.qrtr_tx_pending, status.qrtr_tx_flows, status.qrtr_resumes_sent, status.qrtr_resumes_received);
            /* The modem's own state over SMP2P. The fatal bit says the modem died rather than stopped
               answering, which is the difference between a modem that crashed on the request it was given
               and a modem that is out of credit. */
            printf("FUXI_MODEM: %u s: the modem's status is ready %u, fatal %u, handover %u\n", second,
                   gOwner.smp2p_ready, gOwner.fatal, gOwner.handover);
            if (status.last_unknown[0]) printf("FUXI_MODEM: %u s: the last unknown channel name is %s\n",
                                                second, status.last_unknown);
            /* The service lookups that went out. The modem only publishes the services it is asked about, so a
             * lookup count of 0 with a live link means the modem's list was never asked for. */
            printf("FUXI_MODEM: %u s: the service lookups sent %u\n", second, status.qrtr_lookups);
            /* The lookups the modem answered. A lookup it did not answer is not counted, so this count says
             * whether the modem took the question at all. */
            printf("FUXI_MODEM: %u s: the service lookups answered %u\n", second, status.qrtr_lookups_answered);
            /* The service registry's locator: how often it was announced to the modem, and what the modem asked
             * of it. A request count that stays at zero says the modem never asked the locator. */
            printf("FUXI_MODEM: %u s: the locator was announced %u times, took %u requests, answered %u\n", second,
                   status.locator_announces, status.locator_requests, status.locator_answered);
            /* RMTFS: the same counts for the modem's remote file system, and what it moved through the
             * buffer. An open count of zero says the modem never asked for a file. */
            uint32_t rmtfs[7] = {};
            fuxi_rmtfs_counters(rmtfs, 7);
            printf("FUXI_MODEM: %u s: the rmtfs was announced %u times, took %u requests, answered %u; "
                   "opened %u closed %u, %u reads and %u writes of %u and %u sectors\n",
                   second, status.rmtfs_announces, status.rmtfs_requests, status.rmtfs_answered, rmtfs[0],
                   rmtfs[1], rmtfs[2], rmtfs[3], rmtfs[4], rmtfs[5]);
            /* TFTP: the same counts for the modem's carrier configuration, and what it read out of the
             * volume. A request count that stays at zero says the modem never asked for it, which is the
             * gate this service is behind. */
            uint32_t tftp[6] = {};
            fuxi_tftp_counters(tftp, 6);
            printf("FUXI_MODEM: %u s: the tftp was announced %u times, took %u requests and answered %u; "
                   "read %u times, %u bytes, refused %u and missing %u\n",
                   second, status.tftp_announces, status.tftp_requests, status.tftp_answered, tftp[2], tftp[3],
                   tftp[4], tftp[5]);
            /* The bridge's own report: the QMI services the modem announced, the clients and the queue state. It is
             * the only place that names the service ids, and the service ids are how a client finds the service it
             * wants. Without this call the bridge stays silent and the announced services are never on the record. */
            fuxi_modem_report(second);
            /* The modem's own SMP2P entries, dumped once. The port reads one entry ("slave-kernel") and
               ignores the rest, but the modem publishes a set: an entry the port does not know is one the modem
               is waiting on, and the set is the only place that shows what it wants. It is dumped here and not
               where the entries arrive, because the entries arrive before the USB log is attached and a print
               made then is lost. */
            if (!gOwner.smp2p_dumped && gOwner.smp2p.in) {
                gOwner.smp2p_dumped = 1;
                const uint32_t valid = gOwner.smp2p.in[3] >> 16;
                printf("FUXI_MODEM: the modem's SMP2P has %u entries\n", valid);
                for (uint32_t i = 0; i < valid && i < QCOM_SMP2P_MAX_ENTRIES; ++i) {
                    const volatile uint32_t *entry = &gOwner.smp2p.in[5 + 5 * i];
                    char name[17] = {0};
                    for (unsigned b = 0; b < 16; ++b) {
                        const char c = (char)(entry[b / 4] >> (8 * (b % 4)));
                        if (!c) break;
                        name[b] = c;
                    }
                    printf("FUXI_MODEM: the modem's SMP2P entry %u is %s, value 0x%x\n", i, name, entry[4]);
                }
            }
            /* When the modem stops answering, its own account of why is in SMEM. The crash reason item is read
               once, the first second in which no answer came for a while, so the log says why the modem went
               quiet instead of only that it did. */
            if (!gOwner.crash_reason_read && second > 20 && status.glink_rx_messages == gOwner.last_glink_rx &&
                status.glink_rx_data > 0) {
                gOwner.crash_reason_read = true;
                readCrashReason();
            }
            gOwner.last_tx_again = status.tx_again;
            gOwner.last_glink_rx = status.glink_rx_messages;
            syncStats();
        }
    }
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

/* The secure world can be asked which PAS ids it accepts, without an image to hand it. The answer
 * separates the two things a failed INIT_IMAGE could mean: the id itself is unknown to the secure
 * world, or the id is fine and something about the image is wrong. The command takes the id in x2
 * and returns zero in x0 when the id is supported. */
int stepPasIds()
{
    const uint32_t ids[2] = {
        fuxi_modem_pas_id(FuxiModemPasImageDtb),
        fuxi_modem_pas_id(FuxiModemPasImageModem),
    };
    for (unsigned i = 0; i < 2; ++i) {
        const ScmResult result = secureCall(QCOM_SCM_PAS_IS_SUPPORTED,
                                             qcom_scm_args(1, QCOM_SCM_VAL, 0, 0), ids[i], 0, 0, 0);
        gOwner.pas_supported[i] = result.x0 == 0 && result.x1 == 0 ? 1u : 0u;
        gOwner.pas_support_x0[i] = static_cast<uint32_t>(result.x0);
        printf("FUXI_MODEM: the secure world %s the pas id %u (scm %u/%u)\n",
               gOwner.pas_supported[i] ? "accepts" : "does not accept", ids[i],
               gOwner.pas_support_x0[i], static_cast<uint32_t>(result.x1));
    }
    return 0;
}

/* The instrument has to be checked before its answers are believed. Two things are asked here.

   First: does the support query actually answer about the id it was given? The query is made with two
   ids that are known to work (the adsp's device tree blob id and the adsp's id) and two that cannot
   possibly work. A query that says yes to everything is not answering about the id, and every answer
   it gave before is worthless.

   Second: does the secure world give a different answer for an id it does not know than for an id it
   knows but an image it will not take? If the two answers are the same, the refusal does not say which
   of the two it was, and the error code is worth nothing. If they differ, the refusal's code names the
   reason, and the modem's refusal can be read against it.

   The blobs' first loadable bytes are read too, because a blob whose content is not what its name says
   is the other way a refusal can come about, and the first four bytes say what a blob is. */
int pasInstrument()
{
    const uint32_t known[2] = {36u, 1u};
    const uint32_t bogus[2] = {0xffffu, 0x1234u};
    for (unsigned i = 0; i < 2; ++i) {
        for (unsigned j = 0; j < 2; ++j) {
            const uint32_t id = j ? bogus[i] : known[i];
            const ScmResult result = secureCall(QCOM_SCM_PAS_IS_SUPPORTED,
                                                 qcom_scm_args(1, QCOM_SCM_VAL, 0, 0), id, 0, 0, 0);
            printf("FUXI_MODEM: the instrument: the support query for the pas id %u gave scm %u/%u (%s)\n",
                   id, static_cast<uint32_t>(result.x0), static_cast<uint32_t>(result.x1),
                   result.x0 == 0 && result.x1 == 0 ? "accepted" : "refused");
        }
    }
    /* The same call with an id that cannot work, so its answer is the one a refusal for an unknown id
     * looks like. */
    const char *const paths[2] = {"modem:modem_dtb", "modem:adsp_dtb"};
    for (unsigned image = 0; image < 2; ++image) {
        Image blob = {};
        if (openImage(blob, paths[image])) {
            printf("FUXI_MODEM: the instrument could not open %s\n", paths[image]);
            continue;
        }
        for (unsigned s = 0; s < blob.mdt.count; ++s) {
            const QcomMdtSegment &segment = blob.mdt.segment[s];
            if (!segment.load) continue;
            uint8_t head[8] = {};
            firmwareRead(blob.file, segment.file_offset, head, sizeof head);
            printf("FUXI_MODEM: the instrument: %s segment %u, %llu bytes, first bytes %02x %02x %02x %02x\n",
                   paths[image], s, static_cast<unsigned long long>(segment.file_size),
                   head[0], head[1], head[2], head[3]);
        }
        releaseImage(blob);
    }
    /* The modem's blob against an id that cannot work: the answer is what a refusal for an unknown id
     * looks like, to be compared against the modem's blob against its own id. */
    {
        Image blob = {};
        if (!openImage(blob, "modem:modem_dtb")) {
            const uint64_t bytes = qcom_mdt_metadata_size(&blob.mdt);
            if (allocDiagnosticMeta(blob, bytes)) {
                uint8_t *va = const_cast<uint8_t *>(blob.meta.va);
                if (!buildMetadata(blob, va, bytes)) {
                    cleanToPoc(va, static_cast<size_t>(bytes));
                    const uint64_t phys = blob.meta.phys;
                    const ScmResult result = secureCall(QCOM_SCM_PAS_INIT_IMAGE,
                                                         qcom_scm_args(2, QCOM_SCM_VAL, QCOM_SCM_RW, 0),
                                                         0xffffu, phys, 0, 0);
                    printf("FUXI_MODEM: the instrument: the modem's blob against an id that cannot work gave scm %u/%u\n",
                           static_cast<uint32_t>(result.x0), static_cast<uint32_t>(result.x1));
                }
            }
            releaseImage(blob);
        }
    }
    return 0;
}

/* Which of the two things a refused INIT_IMAGE is about: the pas id or the image. The two device tree
   blobs live in the same partition, and the adsp's one is the blob the secure world is known to accept.
   So the same call is made four ways: the modem's blob and the adsp's blob, each against the modem's
   pas id and against the adsp's.

   A refusal that follows the id and not the image means the id is wrong. A refusal that follows the
   image and not the id means the blob is wrong. A refusal of all four means the call itself is wrong.
   The four results are published as well, so the reader does not have to parse the log.

   Nothing is loaded and nothing is reset: the reset would release a processor, and a matrix that
   resets a processor it does not own is not an experiment. Only INIT_IMAGE, which hands over a
   buffer and returns an answer.

   The second round repeats each attempt with MEM_SETUP after the INIT_IMAGE, because the two blobs
   differ in one more thing: the adsp's is relocatable and the modem's is fixed, so the modem's blob
   gets the memory setup step and the adsp's does not. The memory setup names the region the image
   will live in, and there are two candidate regions for the modem's blob: the blob's own hole and
   the modem image's hole. Both are tried.

   Each attempt starts with a shutdown of that pas id, so an attempt does not see the state a
   previous one left behind, and each gets a fresh metadata buffer, because the secure world is
   allowed to write into the buffer it is given. */
int pasMatrix()
{
    const char *const paths[2] = {"modem:modem_dtb", "modem:adsp_dtb"};
    const uint32_t ids[2] = {fuxi_modem_pas_id(FuxiModemPasImageDtb), 36u};
    /* The region a memory setup may name: the blob's own hole, and the modem image's hole. */
    const uint64_t bases[2] = {kModemDtbCarveoutBase, kModemCarveoutBase};
    const uint64_t spans[2] = {kModemDtbCarveoutSize, kModemCarveoutSize};
    uint32_t results[4] = {};
    uint32_t setup_results[4] = {};
    for (unsigned image = 0; image < 2; ++image) {
        Image blob = {};
        int rc = openImage(blob, paths[image]);
        if (rc) {
            printf("FUXI_MODEM: the matrix could not open %s (%d)\n", paths[image], rc);
            return rc;
        }
        printf("FUXI_MODEM: the matrix: %s is %llu bytes in %u segments, %s, hash %d\n", paths[image],
               static_cast<unsigned long long>(blob.size), blob.mdt.count,
               blob.mdt.relocatable ? "relocatable" : "fixed", blob.mdt.hash);
        for (unsigned s = 0; s < blob.mdt.count; ++s) {
            const QcomMdtSegment &segment = blob.mdt.segment[s];
            printf("FUXI_MODEM: the matrix:   segment %u at %llx, %llu bytes in the file, %llu in memory, flags %x, %s\n",
                   s, static_cast<unsigned long long>(segment.address),
                   static_cast<unsigned long long>(segment.file_size),
                   static_cast<unsigned long long>(segment.memory_size), segment.flags,
                   segment.load ? "loadable" : "not loadable");
        }
        const uint64_t bytes = qcom_mdt_metadata_size(&blob.mdt);
        for (unsigned id = 0; id < 2; ++id) {
            for (unsigned with_setup = 0; with_setup < 3; ++with_setup) {
                /* A fresh buffer for every attempt: the secure world may write into it. */
                if (!allocDiagnosticMeta(blob, bytes)) {
                    printf("FUXI_MODEM: the matrix could not allocate the metadata of %s\n", paths[image]);
                    releaseImage(blob);
                    return -12;
                }
                uint8_t *va = const_cast<uint8_t *>(blob.meta.va);
                if (buildMetadata(blob, va, bytes)) {
                    printf("FUXI_MODEM: the matrix could not read the metadata of %s\n", paths[image]);
                    releaseImage(blob);
                    return -5;
                }
                cleanToPoc(va, static_cast<size_t>(bytes));
                const uint64_t phys = blob.meta.phys;
                /* Start from a clean pas id, so an attempt does not see what a previous one left. */
                (void)secureCall(QCOM_SCM_PAS_SHUTDOWN, qcom_scm_args(1, 0, 0, 0), ids[id], 0, 0, 0);
                const ScmResult init = secureCall(QCOM_SCM_PAS_INIT_IMAGE,
                                                  qcom_scm_args(2, QCOM_SCM_VAL, QCOM_SCM_RW, 0),
                                                  ids[id], phys, 0, 0);
                const bool accepted = init.x0 == 0 && init.x1 == 0;
                uint32_t setup = 0xffffffffu;
                if (with_setup) {
                    const ScmResult mem = secureCall(QCOM_SCM_PAS_MEM_SETUP,
                                                     qcom_scm_args(3, 0, 0, 0), ids[id],
                                                     bases[with_setup - 1], spans[with_setup - 1], 0);
                    setup = mem.x0 == 0 && mem.x1 == 0 ? 1u : 0u;
                }
                printf("FUXI_MODEM: the matrix: %s, the pas id %u, %s: init scm %u/%u (%s), setup %u\n",
                       paths[image], ids[id],
                       with_setup == 0 ? "no setup" :
                       with_setup == 1 ? "setup in the blob's hole" : "setup in the modem's hole",
                       static_cast<uint32_t>(init.x0), static_cast<uint32_t>(init.x1),
                       accepted ? "accepted" : "refused", setup);
                if (with_setup == 0) {
                    results[image * 2 + id] = accepted ? 1u : 0u;
                    setup_results[image * 2 + id] = 0xffffffffu;
                } else {
                    setup_results[image * 2 + id] = setup;
                }
                blob.has_meta = false;
                blob.meta = {};
            }
        }
        releaseImage(blob);
    }
    for (unsigned i = 0; i < 4; ++i) native_fuxi_modem_stats[12 + i] = static_cast<int32_t>(results[i]);
    printf("FUXI_MODEM: the matrix: modem_dtb/0x26 %u, modem_dtb/36 %u, adsp_dtb/0x26 %u, adsp_dtb/36 %u\n",
           results[0], results[1], results[2], results[3]);
    return 0;
}

/* Which pas id the modem's own blob actually goes with. The support query says which ids the secure
   world knows, but that is not the same question as which id takes this blob: the query was shown to
   accept ids whose images it still refuses.

   So every id the secure world accepts is asked to take the modem's blob. The ids are swept in two
   passes: the first pass asks the support query over the range and keeps the ones it accepts, the
   second pass hands the blob to each of them in turn.

   A refusal of every accepted id means the secure world knows the id and still will not take this
   blob, which is a fact about the blob and not about the id. An acceptance names the id.

   Nothing is reset, so no processor is released. */
int pasIdSweep()
{
    uint32_t accepted[64];
    unsigned count = 0;
    for (uint32_t id = 0; id < 256 && count < 64; ++id) {
        const ScmResult result = secureCall(QCOM_SCM_PAS_IS_SUPPORTED,
                                             qcom_scm_args(1, QCOM_SCM_VAL, 0, 0), id, 0, 0, 0);
        if (result.x0 == 0 && result.x1 == 0) accepted[count++] = id;
    }
    printf("FUXI_MODEM: the sweep: the secure world accepts %u ids:", count);
    for (unsigned i = 0; i < count; ++i) printf(" %u", accepted[i]);
    printf("\n");

    Image blob = {};
    int rc = openImage(blob, "modem:modem_dtb");
    if (rc) {
        printf("FUXI_MODEM: the sweep could not open the blob (%d)\n", rc);
        return rc;
    }
    const uint64_t bytes = qcom_mdt_metadata_size(&blob.mdt);
    for (unsigned i = 0; i < count; ++i) {
        if (!allocDiagnosticMeta(blob, bytes)) {
            printf("FUXI_MODEM: the sweep could not allocate the metadata\n");
            releaseImage(blob);
            return -12;
        }
        uint8_t *va = const_cast<uint8_t *>(blob.meta.va);
        if (buildMetadata(blob, va, bytes)) {
            printf("FUXI_MODEM: the sweep could not read the metadata\n");
            releaseImage(blob);
            return -5;
        }
        cleanToPoc(va, static_cast<size_t>(bytes));
        const uint64_t phys = blob.meta.phys;
        (void)secureCall(QCOM_SCM_PAS_SHUTDOWN, qcom_scm_args(1, 0, 0, 0), accepted[i], 0, 0, 0);
        const ScmResult init = secureCall(QCOM_SCM_PAS_INIT_IMAGE,
                                           qcom_scm_args(2, QCOM_SCM_VAL, QCOM_SCM_RW, 0),
                                           accepted[i], phys, 0, 0);
        const bool took = init.x0 == 0 && init.x1 == 0;
        printf("FUXI_MODEM: the sweep: the pas id %u took the blob: %s (scm %u/%u)\n",
               accepted[i], took ? "yes" : "no",
               static_cast<uint32_t>(init.x0), static_cast<uint32_t>(init.x1));
        blob.has_meta = false;
        blob.meta = {};
        if (took) {
            printf("FUXI_MODEM: the sweep: the modem's blob goes with the pas id %u\n", accepted[i]);
            native_fuxi_modem_stats[12] = static_cast<int32_t>(accepted[i]);
            break;
        }
    }
    releaseImage(blob);
    return 0;
}

/* The modem's main image against the same instrument the blobs went through, and the hash segments of
   both blobs compared.

   The sweep showed that every pas id the secure world accepts refuses the modem's blob, so the
   refusal is in the blob. The blob is two things: the segment that is loaded, and the hash segment
   that goes into the metadata beside the headers. The hash segment is the one the secure world
   authenticates, so it is compared byte for byte between the blob that is accepted and the blob that
   is refused: the two are the same length, so a difference is a difference of content.

   The modem's main image is put through the same query, so the answer says whether the refusal is
   about the modem's firmware in general or about its device tree blob only. */
int pasHashCompare()
{
    const char *const paths[2] = {"modem:modem_dtb", "modem:adsp_dtb"};
    Image blob[2] = {};
    for (unsigned image = 0; image < 2; ++image) {
        if (openImage(blob[image], paths[image])) {
            printf("FUXI_MODEM: the hash compare could not open %s\n", paths[image]);
            continue;
        }
        const QcomMdtSegment &hash = blob[image].mdt.segment[blob[image].mdt.hash];
        printf("FUXI_MODEM: the hash compare: %s, the hash segment is %u, %llu bytes at the file offset %llu\n",
               paths[image], blob[image].mdt.hash,
               static_cast<unsigned long long>(hash.file_size),
               static_cast<unsigned long long>(hash.file_offset));
        uint8_t head[32] = {};
        firmwareRead(blob[image].file, hash.file_offset, head, sizeof head);
        printf("FUXI_MODEM: the hash compare:   head:");
        for (unsigned i = 0; i < sizeof head; ++i) printf(" %02x", head[i]);
        printf("\n");
    }
    /* The same hash bytes, byte for byte, so a difference in the middle shows up as a count. */
    if (blob[0].file && blob[1].file) {
        const QcomMdtSegment &a = blob[0].mdt.segment[blob[0].mdt.hash];
        const QcomMdtSegment &b = blob[1].mdt.segment[blob[1].mdt.hash];
        const uint64_t common = a.file_size < b.file_size ? a.file_size : b.file_size;
        uint8_t buf_a[256], buf_b[256];
        uint64_t different = 0;
        for (uint64_t at = 0; at < common;) {
            const size_t chunk = common - at < sizeof buf_a ? static_cast<size_t>(common - at) : sizeof buf_a;
            firmwareRead(blob[0].file, a.file_offset + at, buf_a, chunk);
            firmwareRead(blob[1].file, b.file_offset + at, buf_b, chunk);
            for (size_t i = 0; i < chunk; ++i) if (buf_a[i] != buf_b[i]) ++different;
            at += chunk;
        }
        printf("FUXI_MODEM: the hash compare: %llu bytes compared, %llu differ\n",
               static_cast<unsigned long long>(common), static_cast<unsigned long long>(different));
    }
    for (unsigned image = 0; image < 2; ++image) releaseImage(blob[image]);

    /* The modem's main image, against the id the support query says it accepts. */
    Image main = {};
    if (openImage(main, "modem:modem")) {
        printf("FUXI_MODEM: the hash compare could not open the modem's image\n");
        return -1;
    }
    printf("FUXI_MODEM: the hash compare: the modem's image is %llu bytes in %u segments, %s, hash %u\n",
           static_cast<unsigned long long>(main.size), main.mdt.count,
           main.mdt.relocatable ? "relocatable" : "fixed", main.mdt.hash);
    const uint64_t bytes = qcom_mdt_metadata_size(&main.mdt);
    if (!allocDiagnosticMeta(main, bytes)) {
        printf("FUXI_MODEM: the hash compare could not allocate the metadata\n");
        releaseImage(main);
        return -12;
    }
    uint8_t *va = const_cast<uint8_t *>(main.meta.va);
    if (buildMetadata(main, va, bytes)) {
        printf("FUXI_MODEM: the hash compare could not read the metadata\n");
        releaseImage(main);
        return -5;
    }
    cleanToPoc(va, static_cast<size_t>(bytes));
    const uint64_t phys = main.meta.phys;
    (void)secureCall(QCOM_SCM_PAS_SHUTDOWN, qcom_scm_args(1, 0, 0, 0), fuxi_modem_pas_id(FuxiModemPasImageModem), 0, 0, 0);
    const ScmResult init = secureCall(QCOM_SCM_PAS_INIT_IMAGE,
                                       qcom_scm_args(2, QCOM_SCM_VAL, QCOM_SCM_RW, 0),
                                       fuxi_modem_pas_id(FuxiModemPasImageModem), phys, 0, 0);
    printf("FUXI_MODEM: the hash compare: the modem's image against the pas id %u gave scm %u/%u (%s)\n",
           fuxi_modem_pas_id(FuxiModemPasImageModem), static_cast<uint32_t>(init.x0),
           static_cast<uint32_t>(init.x1), init.x0 == 0 && init.x1 == 0 ? "accepted" : "refused");
    releaseImage(main);
    return 0;
}

/* The last shape of the question. The blob is built by the reader from the split pieces: the headers
   out of the b00 file and the hash segment out of the b02 file, joined into one buffer. The partition
   also carries the whole thing as one file, the b00 file's headers with the hash segment packed
   right behind them, under the name the secure world's caller uses.

   That packed form is what Linux's qcom_mdt_pas_load hands over, and it is what a caller that reads
   the .mdt file gets. The two forms are not the same bytes: the split form has the hash segment at
   the file offset the program header names, and the packed form has it right behind the headers. So
   the two are built and compared, byte for byte, and the packed one is handed to the secure world.

   If the packed one is taken and the split one is not, the reader's join is wrong. If neither is
   taken, the blob's content is what the secure world refuses, and the question is answered.

   The hash segment's own head is dumped as well, since the secure world authenticates that. */
int pasPacked()
{
    const char *const paths[2] = {"modem:modem_dtb", "modem:adsp_dtb"};
    for (unsigned image = 0; image < 2; ++image) {
        Image blob = {};
        if (openImage(blob, paths[image])) {
            printf("FUXI_MODEM: the packed test could not open %s\n", paths[image]);
            continue;
        }
        const QcomMdtSegment &hash = blob.mdt.segment[blob.mdt.hash];
        const uint64_t headers = blob.mdt.header_size;
        /* The packed form: the headers, then the hash segment right behind them, which is the .mdt
           file's own layout and the form the secure world authenticates. */
        const uint64_t bytes = headers + hash.file_size;
        uint8_t *packed = static_cast<uint8_t *>(IOMallocData(bytes));
        if (!packed) {
            printf("FUXI_MODEM: the packed test could not allocate\n");
            releaseImage(blob);
            continue;
        }
        firmwareReadMdt(blob.file, 0, packed, static_cast<size_t>(headers));
        firmwareReadMdt(blob.file, headers, packed + headers, static_cast<size_t>(hash.file_size));
        /* The split form, the way the reader joins it: the same headers, then the hash segment at the
           place the program header names, which in the joined image is another file at another offset. */
        uint8_t *split = static_cast<uint8_t *>(IOMallocData(bytes));
        if (split) {
            firmwareRead(blob.file, 0, split, static_cast<size_t>(headers));
            firmwareRead(blob.file, hash.file_offset, split + headers, static_cast<size_t>(hash.file_size));
            unsigned differing = 0;
            for (uint64_t i = 0; i < bytes; ++i) if (packed[i] != split[i]) ++differing;
            printf("FUXI_MODEM: the packed test: %s: the packed and the split forms differ in %u of %llu bytes\n",
                   paths[image], differing, static_cast<unsigned long long>(bytes));
            printf("FUXI_MODEM: the packed test: %s: the mdt file is %llu bytes, the hash segment is %llu bytes "
                   "at the joined offset %llu\n", paths[image],
                   static_cast<unsigned long long>(blob.mdt.mdt_size),
                   static_cast<unsigned long long>(hash.file_size),
                   static_cast<unsigned long long>(hash.file_offset));
        }
        /* The packed metadata goes to the secure world, in a buffer of its own. */
        if (allocDiagnosticMeta(blob, bytes)) {
            uint8_t *va = const_cast<uint8_t *>(blob.meta.va);
            memcpy(va, packed, static_cast<size_t>(bytes));
            cleanToPoc(va, static_cast<size_t>(bytes));
            const uint64_t phys = blob.meta.phys;
            const uint32_t id = image ? 36u : fuxi_modem_pas_id(FuxiModemPasImageDtb);
            (void)secureCall(QCOM_SCM_PAS_SHUTDOWN, qcom_scm_args(1, 0, 0, 0), id, 0, 0, 0);
            const ScmResult init = secureCall(QCOM_SCM_PAS_INIT_IMAGE,
                                               qcom_scm_args(2, QCOM_SCM_VAL, QCOM_SCM_RW, 0),
                                               id, phys, 0, 0);
            printf("FUXI_MODEM: the packed test: %s, the packed metadata at %llx against the pas id %u gave scm %u/%u (%s)\n",
                   paths[image], static_cast<unsigned long long>(phys), id,
                   static_cast<uint32_t>(init.x0), static_cast<uint32_t>(init.x1),
                   init.x0 == 0 && init.x1 == 0 ? "accepted" : "refused");
        }
        if (packed) IOFreeData(packed, bytes);
        if (split) IOFreeData(split, bytes);
        releaseImage(blob);
    }
    return 0;
}

/* The bytes of the two blobs, so a difference in them can be seen rather than guessed at. The
   loadable segment of each is dumped, its head and its tail, and the first bytes of the piece file
   it comes from are dumped as well: the joined image is built by the reader, and a piece that the
   reader took from the wrong place would show up as a difference between the two.

   The head of a Qualcomm configuration blob is a magic and a length, so the length it declares is
   compared against the length the file has. A blob that is shorter than it says it is, is a blob
   the secure world will refuse, and that is a fact about the file, not about the call. */
/* The two slots, side by side. The default path takes the first of "modem_b" and "modem_a" that
 * carries a FAT volume, so a reader of the inventory never learns which slot it was reading. That
 * matters here: the phone's modem firmware lives in one slot and the other may be empty, so the
 * question "which slot did we read" has to be asked of the device rather than assumed.
 *
 * Each slot is mounted on its own and its /image directory is listed, so the two can be compared
 * directly. Only the modem's own files are named; the rest of the directory is counted, because the
 * partition is shared with the other remote processors' firmware.
 *
 * Read only: a listing touches no register and starts nothing. */
int slotSurvey()
{
    static const char *const slots[2] = {"modem_a", "modem_b"};
    for (unsigned s = 0; s < 2; ++s) {
        unsigned entries = 0;
        unsigned modem_files = 0;
        int have_image_dir = 0;
        /* The partition's identity: its LUN, where it starts and how big it is. Two slots that are
         * the same region have the same three numbers, so this is what tells two slots apart from
         * one partition seen twice. */
        uint32_t lun = 0;
        uint64_t first = 0, blocks = 0;
        const int info = fuxi_adsp_fw_slot_info(slots[s], &lun, &first, &blocks);
        printf("FUXI_MODEM: the slot %s is at lun %u, first block %llu, %llu blocks (%s)\n", slots[s], lun,
               static_cast<unsigned long long>(first), static_cast<unsigned long long>(blocks),
               info ? "not found" : "found");
        /* The names that the PAS sequence asks for: the modem's device tree blob and the modem's
         * image, each in its header piece and its split segments. */
        static const char *const wanted[4] = {"modem_dtb.mdt", "modem.mdt", "modem_dtb.b00", "modem.b00"};
        uint64_t dtb_size = 0, image_size = 0;
        for (unsigned index = 0; index < 1024; ++index) {
            char name[256];
            uint64_t size = 0;
            int is_directory = 0;
            const int rc = fuxi_adsp_fw_list_slot(slots[s], "/image", index, name, sizeof name, &size, &is_directory);
            if (rc) {
                printf("FUXI_MODEM: the slot %s has no readable /image (%d)\n", slots[s], rc);
                break;
            }
            if (!name[0]) break;
            ++entries;
            if (is_directory) {
                have_image_dir = 1;
                continue;
            }
            for (unsigned w = 0; w < 4; ++w) {
                if (strcmp(name, wanted[w])) continue;
                ++modem_files;
                if (w < 2) {
                    printf("FUXI_MODEM: the slot %s has %s, %llu bytes\n", slots[s], name,
                           static_cast<unsigned long long>(size));
                    if (w == 0) dtb_size = size;
                    else image_size = size;
                }
                break;
            }
        }
        printf("FUXI_MODEM: the slot %s: %u entries in /image, %s, %u of the modem's files, "
               "the blob %llu bytes, the image %llu bytes\n", slots[s], entries,
               have_image_dir ? "with directories" : "without directories", modem_files,
               static_cast<unsigned long long>(dtb_size), static_cast<unsigned long long>(image_size));
    }
    /* The two slots' modem blobs, compared byte for byte. Two slots that hold the same firmware
     * are the same firmware, and the secure world will refuse both the same way; two that differ
     * are different firmwares, and the refusal belongs to one of them.
     *
     * The .mdt file is compared as well, since that is the file the metadata is taken out of. */
    {
        void *blob[2] = {nullptr, nullptr};
        uint64_t size[2] = {0, 0};
        void *mdt[2] = {nullptr, nullptr};
        uint64_t mdt_size[2] = {0, 0};
        for (unsigned s = 0; s < 2; ++s) {
            const int rc = fuxi_adsp_fw_read_slot(slots[s], "modem_dtb", &size[s], &blob[s], &mdt[s], &mdt_size[s]);
            printf("FUXI_MODEM: the slot %s's modem_dtb: %llu bytes (%d), the mdt %llu bytes\n", slots[s],
                   static_cast<unsigned long long>(size[s]), rc,
                   static_cast<unsigned long long>(mdt_size[s]));
        }
        if (blob[0] && blob[1]) {
            if (size[0] != size[1]) {
                printf("FUXI_MODEM: the two slots' blobs differ in size, %llu and %llu\n",
                       static_cast<unsigned long long>(size[0]), static_cast<unsigned long long>(size[1]));
            } else {
                const uint8_t *a = static_cast<const uint8_t *>(blob[0]);
                const uint8_t *b = static_cast<const uint8_t *>(blob[1]);
                unsigned differ = 0;
                unsigned first_at = 0;
                for (uint64_t i = 0; i < size[0]; ++i) {
                    if (a[i] == b[i]) continue;
                    if (!differ) first_at = (unsigned)i;
                    ++differ;
                }
                printf("FUXI_MODEM: the two slots' blobs: %llu bytes compared, %u differ, the first at %u\n",
                       static_cast<unsigned long long>(size[0]), differ, first_at);
            }
        }
        if (mdt[0] && mdt[1] && mdt_size[0] == mdt_size[1]) {
            const uint8_t *a = static_cast<const uint8_t *>(mdt[0]);
            const uint8_t *b = static_cast<const uint8_t *>(mdt[1]);
            unsigned differ = 0;
            unsigned first_at = 0;
            for (uint64_t i = 0; i < mdt_size[0]; ++i) {
                if (a[i] == b[i]) continue;
                if (!differ) first_at = (unsigned)i;
                ++differ;
            }
            printf("FUXI_MODEM: the two slots' mdt files: %llu bytes compared, %u differ, the first at %u\n",
                   static_cast<unsigned long long>(mdt_size[0]), differ, first_at);
        }
        for (unsigned s = 0; s < 2; ++s) {
            if (blob[s]) IOFreeData(blob[s], size[s]);
            if (mdt[s]) IOFreeData(mdt[s], mdt_size[s]);
        }
    }
    return 0;
}

/* Each slot's own blob, handed to the secure world in turn. The slot survey showed the two slots
 * hold different firmware: their blobs are the same size and differ in the bytes inside the hash
 * segment. So the refusal may belong to one of them and not the other, and the only way to know is
 * to offer each one.
 *
 * The blob is parsed from the slot's own bytes, the metadata is built out of that parse the same
 * way the runtime path builds it, and the secure world is asked with the modem's pas id. Nothing is
 * loaded and nothing is reset: only INIT_IMAGE, which hands over a buffer and takes an answer back. */
/* A blob held whole in memory, so the parse and the metadata see exactly the bytes of one slot. */
struct slot_bytes {
    const uint8_t *bytes;
    uint64_t size;
};

/* Reads from a blob held whole in memory, which is how a slot's own copy is handed over: the bytes
 * are copied out of the partition first, so the parse and the metadata see exactly the slot's file. */
int slotRead(void *context, uint64_t offset, void *to, size_t length)
{
    struct slot_bytes *b = static_cast<struct slot_bytes *>(context);
    if (!b || offset + length > b->size) return -1;
    memcpy(to, static_cast<const uint8_t *>(b->bytes) + offset, length);
    return 0;
}

int slotPas()
{
    static const char *const slots[2] = {"modem_a", "modem_b"};
    for (unsigned s = 0; s < 2; ++s) {
        uint64_t size = 0;
        void *raw = nullptr;
        void *mdt = nullptr;
        uint64_t mdt_size = 0;
        const int rc = fuxi_adsp_fw_read_slot(slots[s], "modem_dtb", &size, &raw, &mdt, &mdt_size);
        if (rc || !raw) {
            printf("FUXI_MODEM: the slot %s's blob could not be read (%d)\n", slots[s], rc);
            if (raw) IOFreeData(raw, size);
            continue;
        }
        QcomMdtImage image = {};
        struct slot_bytes blob = {static_cast<const uint8_t *>(raw), size};
        const int parsed = qcom_mdt_parse(&image, size, slotRead, &blob);
        if (parsed) {
            printf("FUXI_MODEM: the slot %s's blob did not parse (%d)\n", slots[s], parsed);
            IOFreeData(raw, size);
            continue;
        }
        image.mdt_size = mdt_size;
        struct slot_bytes mdt_blob = {static_cast<const uint8_t *>(mdt), mdt_size};
        printf("FUXI_MODEM: the slot %s's blob: %llu bytes, %u segments, %s, hash %d, a %llu byte header, "
               "a %llu byte mdt\n",
               slots[s], static_cast<unsigned long long>(size), image.count,
               image.relocatable ? "relocatable" : "fixed", image.hash,
               static_cast<unsigned long long>(image.header_size),
               static_cast<unsigned long long>(mdt_size));
        const uint64_t bytes = qcom_mdt_metadata_size(&image);
        uint8_t *va = static_cast<uint8_t *>(IOMallocData(bytes));
        if (!va) {
            printf("FUXI_MODEM: the slot %s's metadata could not be allocated\n", slots[s]);
            IOFreeData(raw, size);
            if (mdt) IOFreeData(mdt, mdt_size);
            continue;
        }
        /* The metadata is the ELF header followed by the hash segment, both out of the slot's .mdt
         * file: that is the form the secure world authenticates. */
        const int built = mdt_size
            ? qcom_mdt_metadata_from_mdt(&image, slotRead, &blob, slotRead, &mdt_blob, va, bytes)
            : qcom_mdt_metadata(&image, slotRead, &blob, va, bytes);
        if (built) {
            printf("FUXI_MODEM: the slot %s's metadata did not build (%d)\n", slots[s], built);
            IOFreeData(va, bytes);
            IOFreeData(raw, size);
            if (mdt) IOFreeData(mdt, mdt_size);
            continue;
        }
        cleanToPoc(va, static_cast<size_t>(bytes));
        Image slot_image = {};
        if (!allocDiagnosticMeta(slot_image, bytes)) {
            printf("FUXI_MODEM: the slot %s's metadata buffer could not be prepared\n", slots[s]);
            IOFreeData(va, bytes);
            IOFreeData(raw, size);
            if (mdt) IOFreeData(mdt, mdt_size);
            continue;
        }
        uint8_t *target = const_cast<uint8_t *>(slot_image.meta.va);
        memcpy(target, va, static_cast<size_t>(bytes));
        cleanToPoc(target, static_cast<size_t>(bytes));
        const uint64_t phys = slot_image.meta.phys;
        const uint32_t id = fuxi_modem_pas_id(FuxiModemPasImageDtb);
        (void)secureCall(QCOM_SCM_PAS_SHUTDOWN, qcom_scm_args(1, 0, 0, 0), id, 0, 0, 0);
        const ScmResult init = secureCall(QCOM_SCM_PAS_INIT_IMAGE,
                                           qcom_scm_args(2, QCOM_SCM_VAL, QCOM_SCM_RW, 0),
                                           id, phys, 0, 0);
        printf("FUXI_MODEM: the slot %s's blob, %llu bytes of metadata at %llx, against the pas id %u gave "
               "scm %u/%u (%s)\n", slots[s], static_cast<unsigned long long>(bytes),
               static_cast<unsigned long long>(phys), id,
               static_cast<uint32_t>(init.x0), static_cast<uint32_t>(init.x1),
               init.x0 == 0 && init.x1 == 0 ? "accepted" : "refused");
        slot_image.has_meta = false;
        slot_image.meta = {};
        IOFreeData(va, bytes);
        IOFreeData(raw, size);
        if (mdt) IOFreeData(mdt, mdt_size);
    }
    return 0;
}

int pasBytes()
{
    const char *const paths[2] = {"modem:modem_dtb", "modem:adsp_dtb"};
    for (unsigned image = 0; image < 2; ++image) {
        Image blob = {};
        if (openImage(blob, paths[image])) {
            printf("FUXI_MODEM: the bytes could not open %s\n", paths[image]);
            continue;
        }
        for (unsigned s = 0; s < blob.mdt.count; ++s) {
            const QcomMdtSegment &segment = blob.mdt.segment[s];
            if (!segment.load) continue;
            printf("FUXI_MODEM: the bytes: %s segment %u: %llu bytes in the file, %llu in memory, at %llx, "
                   "the file offset %llu\n", paths[image], s,
                   static_cast<unsigned long long>(segment.file_size),
                   static_cast<unsigned long long>(segment.memory_size),
                   static_cast<unsigned long long>(segment.address),
                   static_cast<unsigned long long>(segment.file_offset));
            uint8_t head[64] = {};
            uint8_t tail[32] = {};
            firmwareRead(blob.file, segment.file_offset, head, sizeof head);
            if (segment.file_size > sizeof tail)
                firmwareRead(blob.file, segment.file_offset + segment.file_size - sizeof tail, tail, sizeof tail);
            printf("FUXI_MODEM: the bytes:   head:");
            for (unsigned i = 0; i < sizeof head; ++i) printf(" %02x", head[i]);
            printf("\nFUXI_MODEM: the bytes:   tail:");
            for (unsigned i = 0; i < sizeof tail; ++i) printf(" %02x", tail[i]);
            printf("\n");
            /* A configuration blob's head carries a length. Two lengths are compared: the one the
             * blob declares and the one the file has. */
            const uint32_t declared = static_cast<uint32_t>(head[4] | head[5] << 8 | head[6] << 16 | head[7] << 24);
            const uint32_t declared2 = static_cast<uint32_t>(head[8] | head[9] << 8 | head[10] << 16 | head[11] << 24);
            printf("FUXI_MODEM: the bytes:   the blob declares %u and %u, the segment has %llu bytes\n",
                   declared, declared2, static_cast<unsigned long long>(segment.file_size));
        }
        releaseImage(blob);
    }
    return 0;
}

/* One more shape of the same question. The matrix showed that the blob the secure world takes is the
   relocatable one, and that the fixed one is refused with the code the secure world gives for an
   image it will not take. There is one thing the fixed blob has that the relocatable one does not:
   it names the address it lives at. Linux does the memory setup only for a relocatable image, because
   a fixed image already knows where it goes.

   So the remaining possibility is that the secure world wants the fixed image's segment to be in
   place before it accepts the image, and not afterwards: the order the port uses is init, then load,
   then reset, and that order is Linux's for a relocatable image.

   This test loads the segment into the blob's own hole first, then makes the init call. It is a
   different order, and it is the only order left that has not been tried.

   The segment goes into the blob's hole at 0x9b000000, which is the address the fixed blob's own
   program header names. Nothing is reset, so no processor is released: the secure world is asked
   whether it will take the image, and the answer is what it says. */
int pasFixedFirst()
{
    Image blob = {};
    int rc = openImage(blob, "modem:modem_dtb");
    if (rc) {
        printf("FUXI_MODEM: the fixed-first test could not open the blob (%d)\n", rc);
        return rc;
    }
    printf("FUXI_MODEM: the fixed-first test: the blob is %s, its segment is at %llx\n",
           blob.mdt.relocatable ? "relocatable" : "fixed",
           static_cast<unsigned long long>(blob.mdt.segment[1].address));
    /* The segment goes into the hole the blob names. The hole is not mapped yet, so it is mapped
     * for the write and unmapped again. */
    Window hole = {};
    if (!mapWindow(hole, kModemDtbCarveoutBase, kModemDtbCarveoutSize, kIOMapWriteCombineCache)) {
        printf("FUXI_MODEM: the fixed-first test could not map the blob's hole\n");
        releaseImage(blob);
        return -12;
    }
    uint8_t *bounce = static_cast<uint8_t *>(IOMallocData(kImageChunk));
    uint64_t written = 0;
    for (unsigned s = 0; s < blob.mdt.count && bounce; ++s) {
        const QcomMdtSegment &segment = blob.mdt.segment[s];
        if (!segment.load) continue;
        uint64_t offset = 0;
        if (qcom_mdt_place(&blob.mdt, s, kModemDtbCarveoutBase, kModemDtbCarveoutSize, &offset)) {
            printf("FUXI_MODEM: the fixed-first test could not place segment %u\n", s);
            continue;
        }
        uint8_t *to = const_cast<uint8_t *>(hole.va) + offset;
        for (uint64_t done = 0; done < segment.file_size;) {
            const size_t chunk = segment.file_size - done < kImageChunk ? static_cast<size_t>(segment.file_size - done)
                                                                         : kImageChunk;
            if (fuxi_adsp_fw_read(blob.file, segment.file_offset + done, bounce, chunk)) break;
            memcpy(to + done, bounce, chunk);
            done += chunk;
        }
        written += segment.file_size;
        printf("FUXI_MODEM: the fixed-first test: segment %u is at %llx in the hole, %llu bytes\n", s,
               static_cast<unsigned long long>(kModemDtbCarveoutBase + offset),
               static_cast<unsigned long long>(segment.file_size));
    }
    if (bounce) IOFreeData(bounce, kImageChunk);
    __asm__ volatile("dsb sy" ::: "memory");
    unmapWindow(hole);
    printf("FUXI_MODEM: the fixed-first test: %llu bytes went into the hole\n",
           static_cast<unsigned long long>(written));

    const uint64_t bytes = qcom_mdt_metadata_size(&blob.mdt);
    if (!allocDiagnosticMeta(blob, bytes)) {
        printf("FUXI_MODEM: the fixed-first test could not allocate the metadata\n");
        releaseImage(blob);
        return -12;
    }
    uint8_t *va = const_cast<uint8_t *>(blob.meta.va);
    if (buildMetadata(blob, va, bytes)) {
        printf("FUXI_MODEM: the fixed-first test could not read the metadata\n");
        releaseImage(blob);
        return -5;
    }
    cleanToPoc(va, static_cast<size_t>(bytes));
    const uint64_t phys = blob.meta.phys;
    const ScmResult init = secureCall(QCOM_SCM_PAS_INIT_IMAGE,
                                       qcom_scm_args(2, QCOM_SCM_VAL, QCOM_SCM_RW, 0),
                                       fuxi_modem_pas_id(FuxiModemPasImageDtb), phys, 0, 0);
    printf("FUXI_MODEM: the fixed-first test: the init against the pas id %u gave scm %u/%u (%s)\n",
           fuxi_modem_pas_id(FuxiModemPasImageDtb), static_cast<uint32_t>(init.x0),
           static_cast<uint32_t>(init.x1), init.x0 == 0 && init.x1 == 0 ? "accepted" : "refused");
    releaseImage(blob);
    return 0;
}

void ownerThread(void *, wait_result_t)
{
    thread_set_thread_name(current_thread(), "fuxi-modem");

    int rc = stepFirmware();
    if (rc) goto failed;
    printf("FUXI_MODEM: step: the windows\n");
    rc = stepMap();
    if (rc) goto failed;
    /* The memory assign comes before the rails and before any PAS call. Linux runs it in the probe,
     * right after the memory regions are resolved and before the clocks, so the modem's memory already
     * belongs to the modem when the secure world is asked to authenticate it. A refusal here does not
     * stop the bring-up: the rest of the sequence still runs, and whether the blob is then accepted
     * is the fact this round is after. */
    rc = stepAssign();
    if (rc) printf("FUXI_MODEM: the assign did not succeed (%d), the sequence continues\n", rc);
    /* RMTFS: the four files are read into memory and the buffer is mapped, and the buffer is handed
     * to the modem. This is before the modem is started at all, because the modem asks for the files
     * as soon as its stack comes up, and the buffer has to be shared before it can ask.
     *
     * The share is the same kind of assign as the one above, and it names three owners: the
     * hypervisor's own, the modem's MSA and the navigation processor. */
    printf("FUXI_MODEM: step: the modem's files\n");
    rc = fuxi_rmtfs_prepare();
    if (rc) printf("FUXI_MODEM: the rmtfs did not prepare (%d), the sequence continues\n", rc);
    rc = fuxi_rmtfs_share();
    if (rc) printf("FUXI_MODEM: the rmtfs buffer was not shared (%d), the sequence continues\n", rc);
    printf("FUXI_MODEM: step: the proxy rails\n");
    rc = stepRails();
    if (rc) goto failed;
    printf("FUXI_MODEM: step: the pas ids\n");
    rc = stepPasIds();
    if (rc) goto failed;
    printf("FUXI_MODEM: step: the smp2p item\n");
    rc = stepSmp2p();
    if (rc) goto failed;
    printf("FUXI_MODEM: step: the doorbells\n");
    rc = stepIpcc();
    if (rc) goto failed;
    /* The order here is Linux's, and it is not the order the port had.
     *
     * Linux drivers/remoteproc/qcom_q6v5_pas.c: qcom_pas_load (the remoteproc's load callback)
     * hands the device tree blob to the secure world and copies the blob's segments into the blob's
     * carveout. That happens before the remoteproc's start callback, qcom_pas_start, is called at
     * all (drivers/remoteproc/remoteproc_core.c, rproc_start: rproc_load_segments comes before
     * rproc->ops->start).
     *
     * qcom_pas_start then does, in this order:
     *
     *   1. qcom_q6v5_prepare, which votes for the interconnect's bandwidth and sends
     *      "{class: image, res: load_state, name: modem, val: on}" to the always-on processor
     *      (drivers/remoteproc/qcom_q6v5.c, q6v5_load_state_toggle)
     *   2. the proxy power domains, the clocks and the regulators
     *   3. the blob's AUTH_AND_RESET
     *   4. qcom_mdt_pas_load for the modem image, which is its INIT_IMAGE, its MEM_SETUP and the
     *      copy of its segments into the modem's carveout
     *   5. the modem image's AUTH_AND_RESET
     *   6. the wait for the modem's first words
     *
     * The always-on processor's message is sent where Linux sends it, before the blob's reset. The
     * reference port for the same modem in XNU, hakios's drivers/modem/hakios.mpss.c (step 7),
     * sends it before the reset too. */
    rc = stepPasLoad();
    if (rc) goto failed;
    printf("FUXI_MODEM: step: the mailbox to the always-on processor\n");
    rc = stepQmpOpen();
    if (rc) goto failed;
    rc = stepQmpLoadState();
    if (rc) goto failed;
    rc = stepPasStart();
    if (rc) goto failed;
    rc = waitReady();
    if (rc) goto failed;
    /* The first of the four states the bring-up is judged by: the modem's own subsystem is up means the PAS
     * start was accepted and the modem answered ready, not that anything speaks to it yet. */
    printf("FUXI_STAGE 1/4 modem: SSR/PAS the modem subsystem is running (start accepted, ready acknowledged)\n");
    /* The modem is up, so the secure world is done with the metadata pages and both of them can go
     * back to the allocator. Linux does this at the same point (qcom_pas_start, after
     * qcom_q6v5_wait_for_start). */
    releaseImage(gOwner.modem);
    releaseImage(gOwner.dtb);
    rc = stepGlink();
    if (rc) goto failed;
    rc = stepAdapter();
    if (rc) goto failed;
    /* The link has to come up before the bring-up is over: the modem opens the IPCRTR channel into the
     * GLINK items, and only the adapter's poll answers it. The SIM is read over this link, so a modem
     * that is up but whose link never came up is not a modem that can read a card. */
    printf("FUXI_MODEM: step: the link\n");
    rc = waitLink();
    if (rc) goto failed;
    setStage(kStUp);
    printf("FUXI_MODEM: the modem is up, stage %u\n", gOwner.stage);
    /* The thread stays alive and keeps the link running. The modem opens the channel and says HELLO once,
     * but everything after that arrives on its own schedule: the reply to our HELLO, the NEW_SERVER
     * announcements for the services it offers, and the answers to what the bridge asks it. Nothing
     * arrives unless somebody polls the link, so the thread that brought the modem up is the thread that
     * has to keep it running. */
    serviceLink();
    return;

failed:
    /* The metadata pages go back to the allocator whether the bring-up succeeded or not: the secure
     * world is not reading them once the call that handed them over has returned. */
    releaseImage(gOwner.modem);
    releaseImage(gOwner.dtb);
    gOwner.last_error = rc;
    printf("FUXI_MODEM: the bring-up stopped at stage %u (%d)\n", gOwner.stage, rc);
    syncStats();
}

}  // namespace

/* The modem's RMTFS buffer, handed to three owners.
 *
 * The buffer is the one the modem moves its files' sectors through, and the modem cannot reach it
 * until the secure world has been asked to let it in. The assign is the same call the modem's own
 * region is assigned with, and it names three owners: the hypervisor's own, the modem's MSA and the
 * navigation processor (hakios.mpss.c, rmtfs_share: the destinations are HLOS, MSS_MSA and NAV, all
 * with read and write).
 *
 * The call's shape is the one in stepAssign above: the memory to map, the size of the source list,
 * the new owners' records and their size go into the block of external arguments, and the first
 * three arguments go in the registers. */
/* One assign: the memory at the given address, given to the given owners.
 *
 * The source owner is the hypervisor's own and the new owners are the ones named. The call's shape is
 * the one in stepAssign above: the memory to map, the size of the source list, the new owners' records
 * and their size go into the block of external arguments, and the first three arguments go in the
 * registers.
 *
 * The answer is printed as it comes, so a refused call says which shape was refused. */
static int fuxi_modem_assign_once(uint64_t address, uint64_t size, const uint32_t *dest_vmids,
                                   const uint32_t *dest_perms, unsigned dest_count, const char *label)
{
    const unsigned src_count = 1;
    const uint64_t block_bytes = fuxi_modem_assign_block_size(src_count, dest_count);
    const uint64_t ext_bytes = FUXI_MODEM_ASSIGN_EXT_SIZE;
    const uint64_t total = block_bytes + ext_bytes;
    if (total > kMetadataPageBytes) return -5;
    IOBufferMemoryDescriptor *buffer = allocSecurePage();
    if (!buffer) return -12;
    uint8_t *va = static_cast<uint8_t *>(buffer->getBytesNoCopy());
    if (!va) {
        buffer->release();
        return -5;
    }
    const uint64_t base = buffer->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    uint8_t *block = va;
    uint8_t *ext = va + block_bytes;

    /* The three parts of the block. The source is the hypervisor's own owner; the new owners are the
       ones the caller named. */
    const uint32_t src_vmids[1] = {FUXI_MODEM_VMID_HLOS};
    fuxi_modem_assign_write_block_many(block, address, size, src_vmids, src_count, dest_vmids, dest_perms,
                                        dest_count);

    /* Where the secure world is told the parts are. */
    const uint64_t map_offset = fuxi_modem_assign_mem_map_offset(src_count);
    const uint64_t mem_to_map_phys = base + map_offset;
    const uint64_t dest_offset = fuxi_modem_assign_perm_offset(src_count, dest_count);
    const uint64_t dest_phys = base + dest_offset;
    const uint64_t src_size = (uint64_t)src_count * FUXI_MODEM_ASSIGN_SRC_ENTRY;
    const uint64_t dest_size = (uint64_t)dest_count * FUXI_MODEM_ASSIGN_PERM_SIZE;
    fuxi_modem_assign_write_ext(ext, src_size, dest_phys, dest_size);
    cleanToPoc(va, static_cast<size_t>(total));

    printf("FUXI_MODEM: the assign %s hands %llx of %llu bytes to %u owner(s):", label,
           static_cast<unsigned long long>(address), static_cast<unsigned long long>(size), dest_count);
    for (unsigned i = 0; i < dest_count; ++i) printf(" %u", dest_vmids[i]);
    printf("\n");

    const uint64_t ext_phys = base + block_bytes;
    const ScmResult result = smcCall(FUXI_MODEM_MP_SVC, FUXI_MODEM_MP_ASSIGN, fuxi_modem_assign_arginfo(),
                                     mem_to_map_phys, FUXI_MODEM_ASSIGN_MEM_MAP_SIZE, base, ext_phys);
    gOwner.assign_x0 = gOwner.scm_x0;
    gOwner.assign_x1 = gOwner.scm_x1;
    buffer->release();
    printf("FUXI_MODEM: the assign %s's answer is %u/%u\n", label, gOwner.scm_x0, gOwner.scm_x1);
    if (!fuxi_modem_scm_ok(result.x0, result.x1)) {
        printf("FUXI_MODEM: the assign %s was refused (scm %u/%u)\n", label, gOwner.scm_x0, gOwner.scm_x1);
        return -3;
    }
    printf("FUXI_MODEM: the assign %s was accepted\n", label);
    return 0;
}

/* The buffer, handed to the modem.
 *
 * The buffer is the one the modem moves its files' sectors through, and the modem cannot reach it
 * until the secure world has been asked to let it in. The assign is the same call the modem's own
 * region is assigned with.
 *
 * The owners are the hypervisor's own and the modem's MSA. The reference names a third one, the
 * navigation processor (hakios.mpss.c, rmtfs_share: the destinations are HLOS, MSS_MSA and NAV, all
 * with read and write), and it has a boot option for a board that cannot take it
 * (`option("modem_no_nav") ? 2 : 3`).
 *
 * This board cannot take it, and the secure world says so: on 2026-11-22 the assign with the three
 * owners was refused (the answer was -2 with the answer word 86), and the same call with two owners
 * was accepted at once, and the modem read and wrote its files through the buffer afterwards. The
 * device tree agrees: it declares the virtual machines 0x3, 0x2d, 0x31 and 0x32, and there is no 0x2b
 * (the navigation processor) anywhere in it, and its own rmtfs node carries no qcom,vm-nav-path. */
extern "C" int fuxi_modem_rmtfs_assign(uint64_t address, uint64_t size, unsigned owners)
{
    printf("FUXI_MODEM: the rmtfs assign starts\n");
    if (owners < 1) owners = 1;
    if (owners > 3) owners = 3;
    const uint32_t three[3] = {FUXI_MODEM_VMID_HLOS, FUXI_MODEM_VMID_MSS_MSA, FUXI_MODEM_VMID_NAV};
    const uint32_t rw[3] = {FUXI_MODEM_PERM_RW, FUXI_MODEM_PERM_RW, FUXI_MODEM_PERM_RW};
    return fuxi_modem_assign_once(address, size, three, rw, owners, "rmtfs");
}

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
    const char *const directories[] = {"/", "/image", "/image/modem_pr",
        /* The carrier configuration tree: the modem asks for files below these, and one of them
         * (mcfg_ddr/rf/rf_static_834_00_00.mbn) is the only request the port has not been able to open. */
        "/image/modem_pr/mcfg", "/image/modem_pr/mcfg/configs",
        /* The two directories the modem reads its MBN lists from: its own strings name mbn_hw.txt and mbn_sw.txt
           right after the .dig files it already asked for and got. */
        "/image/modem_pr/mcfg/configs/mcfg_hw", "/image/modem_pr/mcfg/configs/mcfg_sw",
        "/image/modem_pr/mcfg/configs/mcfg_ddr", "/image/modem_pr/mcfg/configs/mcfg_ddr/rf"};
    unsigned count = 0;
    for (unsigned d = 0; d < sizeof directories / sizeof directories[0]; ++d) {
        for (unsigned index = 0; index < 1024; ++index) {
            char name[256];
            uint64_t size = 0;
            int is_directory = 0;
            const int rc = fuxi_adsp_fw_list(directories[d], index, name, sizeof name, &size, &is_directory);
            if (rc) {
                /* A directory the tree does not carry is not the end of the inventory: mcfg_ddr is absent on this
                   board and the listing of it is exactly what says so. Keeping the walk going is what lets the
                   second slot be listed too. */
                printf("FUXI_MODEM: the listing of %s failed (%d)\n", directories[d], rc);
                continue;
            }
            if (!name[0]) break;
            /* The names are the whole point, so they go out one line at a time, and the counts into the
             * stats array for the channel to read back. */
            printf("FUXI_MODEM: %s/%s %llu%s\n", directories[d], name, size, is_directory ? " (directory)" : "");
            ++count;
        }
    }
    /* The same carrier configuration directory on the other slot: the port serves modem_a's volume, and the
     * one file the modem asks for and cannot get (mcfg_ddr/rf/rf_static_834_00_00.mbn) has to be looked for
     * somewhere before it is called absent. */
    {
        static const char *const other[] = {
            "/image/modem_pr",
            "/image/modem_pr/mcfg/configs",
            /* The one file the modem asks for and cannot get from modem_a: if the other slot carries a newer or older
               copy of the tree, it is here. */
            "/image/modem_pr/mcfg/configs/mcfg_ddr",
            "/image/modem_pr/mcfg/configs/mcfg_ddr/rf"};
        for (unsigned o = 0; o < sizeof other / sizeof other[0]; ++o) {
            for (unsigned index = 0; index < 128; ++index) {
                char name[256];
                uint64_t size = 0;
                int is_directory = 0;
                const int rc = fuxi_adsp_fw_list_slot("modem_b", other[o], index, name, sizeof name, &size, &is_directory);
                if (rc) { printf("FUXI_MODEM: modem_b:%s failed (%d)\n", other[o], rc); break; }
                if (!name[0]) break;
                printf("FUXI_MODEM: modem_b:%s/%s %llu%s\n", other[o], name, size, is_directory ? " (directory)" : "");
            }
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

/* Dumps one file of the firmware partition, whole, as hex and as ASCII. Read only: it reads the
 * partition and prints, it starts nothing and touches no register.
 *
 * The file is the modem's own service registry table. The table is what the modem expects the AP to
 * serve on QRTR service 64, so its exact bytes have to be read before any answer is written. It is
 * printed and not parsed here: the bytes are the fact, and what they mean is read afterwards.
 *
 * The dump goes out through printf, so it reaches the USB control channel as well as the console. */
int dumpRegistryTable()
{
    static const char *const path = "image/modemr.jsn";
    uint64_t size = 0;
    void *bytes = NULL;
    const int rc = fuxi_adsp_fw_read_file(path, &size, &bytes);
    if (rc) {
        printf("FUXI_MODEM: %s could not be read (%d)\n", path, rc);
        return rc;
    }
    printf("FUXI_MODEM: %s is %llu bytes\n", path, (unsigned long long)size);
    const uint8_t *data = (const uint8_t *)bytes;
    for (uint64_t at = 0; at < size; at += 16) {
        printf("FUXI_MODEM: %04llx:", (unsigned long long)at);
        for (unsigned i = 0; i < 16; ++i) {
            if (at + i < size) printf(" %02x", data[at + i]);
            else printf("   ");
        }
        printf("  |");
        for (unsigned i = 0; i < 16 && at + i < size; ++i) {
            const uint8_t c = data[at + i];
            printf("%c", (c >= 0x20 && c < 0x7f) ? (char)c : '.');
        }
        printf("|\n");
    }
    printf("FUXI_MODEM: %s is %llu bytes, dumped\n", path, (unsigned long long)size);
    if (bytes) IOFreeData(bytes, size);
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
    /* Mode 16 inventories the four partitions the modem asks for, read only. It starts no modem and
     * touches no register: it only says which of them this board carries and how big they are, which
     * is the fact the RMTFS service needs before anything else. */
    /* Mode 17 reads the command database out, entry by entry. Read only: it maps the database and prints, it
     * starts nothing and touches no register. The IPA\'s clock and its three interconnect paths are voted through it. */
    if (mode == 17) {
        (void)dumpCmdDb();
        return;
    }
    /* Mode 18 votes the IPA clock and then reads its registers. It exists as a separate mode precisely because the
       read is the risky half: it can be run alone, and if it wedges, nothing else in the bring-up has been disturbed. */
    /* Mode 18 is where the IPA register read used to live. It no longer reads: every attempt ended in a
       synchronous external abort (esr 0x96000010, far = the window VA + 0x54) with the clock voted, both
       interconnects voted, the window mapped, and the firmware loaded and released by the secure world through
       PAS id 0x0f. The block does not answer this processor at all, so the probe that reads it is not "risky" --
       it is ineffective, and an ineffective probe that takes the kernel down is worse than none. The mode is kept
       as the record of that, and it only says so. */
    if (mode == 18) {
        printf("FUXI_MODEM: the ipa register read is disabled: the block returns a synchronous external abort to this\n");
        printf("FUXI_MODEM: processor with the clock, both interconnects and the firmware all in place (2026-11-23)\n");
        return;
    }
    if (mode == 16) {
        (void)fuxi_rmtfs_inventory();
        return;
    }
    /* Mode 5 runs the pas matrix: the same INIT_IMAGE call against both device tree blobs and both
     * pas ids, to tell a wrong id from a wrong image. It hands over a buffer and takes an answer back;
     * it loads nothing and resets no processor. */
    if (mode == 5) {
        (void)pasMatrix();
        return;
    }
    /* Mode 6 checks the instrument the matrix is built on: it asks the support query about ids that
     * cannot work, to see whether the query answers about the id at all, and it asks for a refusal
     * against an id that cannot work, to see what such a refusal looks like. */
    if (mode == 6) {
        (void)pasInstrument();
        return;
    }
    /* Mode 7 puts the fixed blob's segment into its hole before the init call, which is the one
     * order the matrix did not try: the order a fixed image may need, since it names its own address
     * and the relocatable one does not. */
    if (mode == 7) {
        (void)pasFixedFirst();
        return;
    }
    /* Mode 8 dumps the two blobs' bytes, so a difference between them can be seen rather than
     * guessed at: the head and the tail of each loadable segment, and the lengths a configuration
     * blob declares against the length the file has. */
    if (mode == 8) {
        (void)pasBytes();
        return;
    }
    /* Mode 9 sweeps the pas ids the secure world accepts and asks each one to take the modem's own
     * blob, to find the id the blob goes with. It resets nothing. */
    if (mode == 9) {
        (void)pasIdSweep();
        return;
    }
    /* Mode 10 compares the hash segments of the two blobs byte for byte, and puts the modem's main
     * image through the same query, so the refusal is known to be about the blob or about the modem's
     * firmware as a whole. */
    if (mode == 10) {
        (void)pasHashCompare();
        return;
    }
    /* Mode 11 hands the secure world the metadata in the packed form, the headers with the hash
     * segment right behind them, which is the form a caller that reads the .mdt file builds. */
    if (mode == 11) {
        (void)pasPacked();
        return;
    }
    /* Mode 12 lists the two slots side by side, so which one carries the modem's firmware is a fact
     * from the device and not an assumption about which one the reader prefers. */
    if (mode == 12) {
        (void)slotSurvey();
        return;
    }
    /* Mode 13 hands each slot's own blob to the secure world, so the two firmwares are offered one
     * after the other and the refusal can be told apart from a firmware that is accepted. */
    if (mode == 13) {
        (void)slotPas();
        return;
    }
    /* Mode 14 serves the modem's service registry locator, QRTR service 64, and dumps the table the
     * answers come from. The table is the modem's own, /image/modemr.jsn, and it is dumped first so a run's
     * log holds the table that the answers were built out of. The mode then starts the modem and keeps the
     * link running: the locator is announced to the modem and answers what the modem asks of it, and
     * neither happens without a live link. */
    if (mode == 14) {
        (void)dumpRegistryTable();
        /* The mode does not return: it falls through to the bring-up, which is what the locator needs. */
    } else if (mode == 15) {
        /* Mode 15 serves the modem's remote file system, QRTR service 14, on top of the locator's
         * bring-up: the four files are read into memory, the buffer is shared with the modem, and
         * RMTFS is announced and answered. The mode does not return either, because the service needs
         * a live link. */
        printf("FUXI_MODEM: mode 15: the modem's remote file system\n");
    } else if (mode < 4) {
        /* Mode 4 and up start the modem: they vote rails, load the images over PAS and let the modem's
         * firmware run. The radio stays off until a later step says otherwise. */
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
    case 12: value = static_cast<uint32_t>(native_fuxi_modem_stats[12]) & 0xffu; break; /* the dtb's pas id accepted */
    case 13: value = static_cast<uint32_t>(native_fuxi_modem_stats[13]) & 0xffu; break; /* the modem's pas id accepted */
    case 14: value = static_cast<uint32_t>(native_fuxi_modem_stats[14]) & 0xffffu; break; /* what it said for the dtb */
    case 15: value = static_cast<uint32_t>(native_fuxi_modem_stats[15]) & 0xffffu; break; /* what it said for the modem */
    default: value = 0; break;
    }
    return (page & 0xfu) << 16 | (value & 0xfffu) << 20;
}
