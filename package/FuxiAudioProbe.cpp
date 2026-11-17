/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Fixed, CDC-driven audio bring-up operations.
 *
 * This file deliberately has no boot argument, delayed thread, mapping or
 * dereference path. The diagnostic transport calls one fixed operation at a
 * time from its bounded worker. The descriptor is the sole retained resource.
 */
#include <IOKit/IOMemoryDescriptor.h>
#include <sys/systm.h>
#include <pexpert/pexpert.h>
#include "FuxiAudioDescriptor.h"

namespace {

FuxiAudioDescriptorState gState;
volatile unsigned gProbeEntry;

void *createDescriptor(void *, uint64_t physical, uint64_t length)
{
    return IOMemoryDescriptor::withPhysicalAddress(
        static_cast<mach_vm_address_t>(physical), static_cast<mach_vm_size_t>(length), kIODirectionIn);
}

void releaseDescriptor(void *, void *opaque)
{
    static_cast<IOMemoryDescriptor *>(opaque)->release();
}

const FuxiAudioDescriptorBackend gBackend = {createDescriptor, releaseDescriptor, nullptr};

}  // namespace

extern "C" int fuxi_audio_diag_run(unsigned step)
{
    return fuxi_audio_descriptor_run(&gState, &gBackend, step);
}

extern "C" unsigned fuxi_audio_diag_resources(void)
{
    extern volatile uint32_t native_fuxi_adsp_stats[128] __attribute__((weak));
    static unsigned page_counter;
    unsigned result = fuxi_audio_descriptor_resources(&gState) | gProbeEntry;
    if (!native_fuxi_adsp_stats || !native_fuxi_adsp_stats[0]) return result;

    const unsigned page = __atomic_fetch_add(&page_counter, 1, __ATOMIC_RELAXED) & 0xfu;
    uint32_t value = 0;
    switch (page) {
    case 0:
        value = (native_fuxi_adsp_stats[9] & 0x1fu) |
                ((native_fuxi_adsp_stats[10] & 0x7fu) << 5);
        break;
    case 1: value = native_fuxi_adsp_stats[101] & 0xfffu; break;
    case 2: value = native_fuxi_adsp_stats[105] & 0xfffu; break;
    case 3: value = native_fuxi_adsp_stats[28] & 0xffu; break;
    case 4: value = native_fuxi_adsp_stats[113] & 0xfffu; break;
    case 5: value = native_fuxi_adsp_stats[114] & 0xfffu; break;
    case 6: value = native_fuxi_adsp_stats[115] & 0xfffu; break;
    case 7: value = native_fuxi_adsp_stats[117] & 0xfffu; break;
    case 8: value = native_fuxi_adsp_stats[119] & 0xfffu; break;
    case 9: value = native_fuxi_adsp_stats[120] & 0xfffu; break;
    case 10: value = native_fuxi_adsp_stats[96] & 0xfffu; break;
    case 11: value = native_fuxi_adsp_stats[31] & 0xfffu; break;
    case 12: value = native_fuxi_adsp_stats[116] & 0xfffu; break;
    case 13: value = (native_fuxi_adsp_stats[125] & 1u) |
                     ((native_fuxi_adsp_stats[121] & 0x7fu) << 1) |
                     ((native_fuxi_adsp_stats[122] & 0x1fu) << 8); break;
    case 14: value = (native_fuxi_adsp_stats[28] >> 8) & 0xffu; break;
    case 15: value = native_fuxi_adsp_stats[112] & 0xfffu; break;
    }
    result |= (page & 0xfu) << 16;
    result |= (value & 0xfffu) << 20;
    return result;
}

extern "C" void native_fuxi_adsp_probe_mode(unsigned mode) __attribute__((weak));

/* The separately force-loaded runtime owns ADSP work. With no fuxi-adsp boot
 * argument it returns without doing anything; descriptor diagnostics remain
 * CDC-driven and retain their independent lifetime. */
extern "C" void native_audio_probe(void)
{
    unsigned mode = 0;
    gProbeEntry = 1u << 8; /* bsd_init reached the board audio entry. */
    if (PE_parse_boot_argn("fuxi-adsp", &mode, sizeof mode)) {
        gProbeEntry |= 1u << 9;
        gProbeEntry |= (mode & 0xfu) << 12;
    }
    if (native_fuxi_adsp_probe_mode) {
        gProbeEntry |= 1u << 10; /* about to enter the runtime */
        native_fuxi_adsp_probe_mode(mode);
        gProbeEntry |= 1u << 11; /* runtime returned */
    }
}
