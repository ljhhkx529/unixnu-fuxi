/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The audio probe, stage 0: read SMEM through the ported driver and say what is there.
 *
 * The device has an ADSP this tree has never spoken to. Before anything is voted, clocked or written, one thing can be
 * established with reads alone: whether the SMEM partition exists at the address the platform documents, whether the
 * items the audio line needs are allocated, and what the SMP2P inbound item says. That answers the question the binding
 * cannot: which of its numbers are this board's rather than sheng's.
 *
 * Nothing here writes: it maps one range read-only and parses it with package/QcomSmem.c, the same code the host tests
 * drive, so a device run also checks that parser against real memory.
 *
 * The report goes to the kernel log and, when asked, to the panic screen, because this phone has no serial console.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/libkern.h>
#include <kern/task.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>

extern "C" {
#include "QcomSmem.h"
#include "QcomSmp2p.h"

/* Boot argument fuxi-audio-probe=N: 0 off, 1 log the report, 2 also end the boot with it on the panic screen. */
int fuxi_audio_probe = 0;

/* Called from bsd_init by the audio patch, next to the touch probe. */
void native_audio_probe(void);
}

#define kSmemBase 0x81d00000ull
#define kSmemSize 0x200000u
#define kRemotePid 2u

enum {kItemOut = 429, kItemIn = 443, kItemCrash = 423, kItemGlinkDesc = 478, kItemGlinkTx = 479, kItemGlinkRx = 480};

static const uint16_t kAudioItems[] = {kItemOut, kItemIn, kItemCrash, kItemGlinkDesc, kItemGlinkTx, kItemGlinkRx};

namespace {

char gReport[1600];
size_t gUsed;

void say(const char *format, ...)
{
    if (gUsed + 1u >= sizeof gReport) return;
    va_list args;
    va_start(args, format);
    const int wrote = vsnprintf(gReport + gUsed, sizeof gReport - gUsed, format, args);
    va_end(args);
    if (wrote > 0) gUsed += (size_t)wrote;
    if (gUsed >= sizeof gReport) gUsed = sizeof gReport - 1u;
}

/* The same bytes as words, which is what a descriptor is: eight of them for the GLINK header, the first few of a FIFO. */
void sayWords(const QcomSmem *smem, uint32_t offset, uint32_t words)
{
    for (uint32_t i = 0; i < words; ++i) say(" %08x", qcom_smem_read32(smem, offset + 4u * i));
}

/* The first bytes of an item as text, so a name or a descriptor can be recognised without trusting it. */
void sayBytes(const QcomSmem *smem, uint32_t offset, uint32_t limit)
{
    char text[33];
    uint32_t used = 0;
    for (uint32_t i = 0; i < limit && used < sizeof text - 1u; ++i) {
        const uint32_t word = qcom_smem_read32(smem, offset + (i & ~3u));
        const uint8_t c = (uint8_t)(word >> (8u * (i & 3u)));
        text[used++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
    }
    text[used] = 0;
    say("'%s'", text);
}

void run()
{
    say("audio probe: smem 0x%llx + 0x%x\n", kSmemBase, kSmemSize);
    /* A write stage needs a writable mapping; a read-only one faults the moment qcom_smem_alloc stores its entry, which
     * is exactly how the first attempt at stage 4 took the phone down. The mapping is chosen by what the caller asked
     * for, so a read-only probe stays read-only. */
    const bool writing = fuxi_audio_probe >= 4;
    IOMemoryDescriptor *descriptor = IOMemoryDescriptor::withPhysicalAddress(
        kSmemBase, kSmemSize, writing ? kIODirectionInOut : kIODirectionIn);
    if (!descriptor) {say("smem: no descriptor\n"); return;}
    IOMemoryMap *map = descriptor->createMappingInTask(
        kernel_task, 0, kIOMapAnywhere | (writing ? 0 : kIOMapReadOnly));
    descriptor->release();
    if (!map || !map->getVirtualAddress()) {say("smem: not mapped\n"); if (map) map->release(); return;}

    QcomSmem smem;
    smem.base = (volatile uint8_t *)map->getVirtualAddress();
    smem.size = kSmemSize;

    /* The partition the APPS host shares with the ADSP, then the global one. */
    QcomSmemPartition part;
    const int rc = qcom_smem_partition(&smem, QCOM_SMEM_HOST_APPS, (uint16_t)kRemotePid, &part);
    say("partition apps/adsp: rc %d", rc);
    if (rc == QcomSmemOk) say(" at 0x%x size 0x%x cacheline %u\n", part.offset, part.size, part.cacheline);
    else say("\n");

    if (rc == QcomSmemOk) {
        for (unsigned i = 0; i < sizeof kAudioItems / sizeof kAudioItems[0]; ++i) {
            uint32_t offset = 0, size = 0;
            const int found = qcom_smem_find(&smem, &part, kAudioItems[i], &offset, &size);
            if (found != QcomSmemOk) {say("item %u: rc %d\n", kAudioItems[i], found); continue;}
            say("item %u: at 0x%x size %u first 32 ", kAudioItems[i], offset, size);
            sayBytes(&smem, offset, 32u);
            say("\n");
            /* Words as well: a descriptor or a FIFO header is numbers, not text. */
            if (size >= 32u && offset < smem.size) {
                say("  words:");
                sayWords(&smem, offset, 8u);
                say("\n");
            }
        }
    }

    /* Stage 4 creates the items the audio bring-up needs and nothing else. It is the first thing this probe does that
     * writes: an allocation inside the partition the boot loader already reserved, no clock, no rail, no DSP start. The
     * SMEM hardware mutex is sheng's to take and this probe does not, because the DSP is not running to race with. */
    if (fuxi_audio_probe >= 4 && rc == QcomSmemOk) {
        const uint16_t want[] = {kItemOut, kItemIn, kItemCrash, kItemGlinkRx};
        const uint32_t sizes[] = {QCOM_SMP2P_ITEM_BYTES, QCOM_SMP2P_ITEM_BYTES, 0x1000u, 0x4000u};
        for (unsigned i = 0; i < sizeof want / sizeof want[0]; ++i) {
            const int alloc = qcom_smem_alloc(&smem, &part, want[i], sizes[i]);
            say("alloc %u (%u bytes): rc %d%s\n", want[i], sizes[i], alloc,
                alloc == QcomSmemExists ? " (already there)" : "");
        }
    }

    /* Everything the partition holds, not only the six ids the audio line needs: what the boot loader set up is worth
     * knowing before anything is allocated. */
    if (rc == QcomSmemOk) {
        const int items = qcom_smem_item_count(&smem, &part);
        say("partition holds %d uncached items:", items);
        for (int i = 0; i < items && i < 24; ++i) {
            uint16_t id = 0;
            uint32_t at = 0, bytes = 0;
            if (qcom_smem_item_at(&smem, &part, (uint32_t)i, &id, &at, &bytes) == QcomSmemOk)
                say(" %u@0x%x/%u", id, at, bytes);
        }
        say("\n");
    }

    /* The global partition carries the item table itself; its presence confirms the base is right. */
    QcomSmemPartition global;
    const int grc = qcom_smem_partition(&smem, QCOM_SMEM_HOST_GLOBAL, QCOM_SMEM_HOST_GLOBAL, &global);
    say("partition global: rc %d", grc);
    if (grc == QcomSmemOk) say(" at 0x%x size 0x%x\n", global.offset, global.size);
    else say("\n");

    map->release();
}

}  // namespace

extern "C" void native_audio_probe(void)
{
    /* The argument is read here so the patch that calls this stays one line. */
    int value = 0;
    if (PE_parse_boot_argn("fuxi-audio-probe", &value, sizeof value)) fuxi_audio_probe = value;
    if (!fuxi_audio_probe) return;
    gUsed = 0;
    gReport[0] = 0;
    run();
    IOLog("fuxi-audio-probe:\n%s", gReport);
    if (fuxi_audio_probe >= 2) panic("fuxi-audio-probe:\n%s", gReport);
}
