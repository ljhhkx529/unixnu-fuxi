/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Fuxi owner glue for the board-independent QcomAdsp ladder.
 *
 * fuxi-adsp=0 (default): no work.
 * fuxi-adsp=1: publish the generated binding and inventory device firmware.
 * fuxi-adsp=2: request runtime bring-up.  The seven owner hooks must all be
 *              installed before the gate opens; they perform, in order:
 *              firmware source, platform/carveout mapping, SMP2P, IPCC, QMP,
 *              PAS (RPMh + DTB + ADSP), then GLINK and the GPR/SPF readiness
 *              query.  Board numbers come from the generated binding.
 *
 * One kernel thread owns the state machine.  Interrupt handlers only
 * acknowledge their hardware, count the event and wake that thread.  Every
 * wait has a deadline; no protocol layer polls an unbounded register.
 *
 * The PAS and image sequence is the one Sheng's AdspProbe proved, using the
 * Fuxi Qcom* layers rather than a second protocol stack. */
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include "SmmuBypass.h"
#include "GeniI2c.h"
#include "CirrusCs35l41.h"
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <mach/kmod.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <pexpert/pexpert.h>

#include "FuxiAdspProbe.h"
#include "FuxiAdspFirmware.h"
#include "FuxiAudioBinding.h"
#include "FuxiAmpBringup.h"
#include "FuxiCs35l41.h"
#include "TouchBoard.h"
#include "QcomAdsp.h"
#include "QcomAudioGraph.h"
#include "QcomAudioAbi.h"
#include "FuxiGraphReply.h"
#include "QcomGlink.h"
#include "QcomMdt.h"
#include "QcomQmp.h"
#include "QcomRpmh.h"
#include "QcomScm.h"
#include "QcomSmem.h"
#include "QcomSmp2p.h"
#include "ShengGpr.h"

extern "C" int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *)
    __attribute__((weak_import));
/* The USB debug console carries printf; kprintf goes to the other console. */
extern int printf(const char *format, ...);
/* Physical window of the kernel and the boot image's GPU addon: the P3 low-DRAM
 * period buffer must not overlap either. */
extern "C" unsigned long gPhysBase, gPhysSize;
extern "C" uint64_t kvtophys(uint64_t);
extern "C" int native_ram_block_gpu_addon(const uint8_t **, uint64_t *) __attribute__((weak_import));

/* The audio service iOS's audio server plug-in opens (package/FuxiAudioService.cpp).  Its classes are registered by
 * the runtime between preModLoad and postModLoad; the service publishes itself once the speaker path is ready. */
extern "C" void native_audio_service_static_init(void) __asm__("__GLOBAL__sub_I_FuxiAudioService.cpp");
extern "C" void fuxi_audio_service_start(void);
extern "C" void fuxi_audio_service_publish(void);
/* The audio service's state counters (package/FuxiAudioService.cpp owns the storage). */
extern "C" volatile int32_t native_audio_service[8];
extern kmod_info_t g_kernel_kmod_info;

namespace {

constexpr uint32_t kStatsAbi = 2;
constexpr uint32_t kEvSmp2p = 1u << 0;
constexpr uint32_t kEvGlink = 1u << 1;
constexpr uint32_t kEvAop = 1u << 2;
constexpr uint32_t kEvRsc = 1u << 3;
constexpr uint32_t kEvSend = 1u << 4;
constexpr uint32_t kEvGpr = 1u << 5;

constexpr uint32_t kSpiIrqBase = 32;
constexpr unsigned kRscTcs = 2;  /* last active TCS; 0 and 1 showed no done */
constexpr uint32_t kGlinkStorageBytes = 512u * 20u;  /* fuxi DTB qcom,intents = <0x200 0x14> */
constexpr uint32_t kImageChunk = 256u * 1024u;
uint32_t gGraphProbeLevel;  /* fgraph=5 audits owned PCM and audio SMMU, without DSP map or playback */

/* The period answer handler is defined with the period pump, below the
 * receive path, so it is named here. */
static void graphHandleWriteDone(const FuxiGraphReply &reply);

/* Reads the amplifiers' interrupt status again, after the graphs run. */
int ampRereadStatus();

/* Unmutes the amplifiers: the only step that can make a sound. */
int ampUnmute(unsigned gain_index);

/* The generic "send the request that is already in the graph packet and
 * wait for its answer" helper, defined below with the graph probes. */
static int graphWaitOp(uint32_t source_port, uint32_t token, uint32_t opcode,
                        uint8_t kind, FuxiGraphReply &reply);

/* The speaker path's AudioReach topology.
 *
 * Two graphs make the path: a device graph that owns the hardware endpoint
 * and a stream graph that owns the shared memory endpoint.  The stream
 * graph's last connection crosses into the device graph's endpoint, which
 * is how the two are joined.
 *
 * The instance numbers are the board's own topology.  They are not read
 * from the DSP: the board gives them no other way, and the only board that
 * ran them (Kerfand's UniXNU-Fuxi-Arm64e-iOS17, same SoC and the same
 * part) used exactly these, which is what the connection below relies on.
 * The numbers are therefore a choice recorded in facts/audio/binding.toml
 * and proved by the device answering, not a measurement. */
constexpr uint32_t kArApmIid = 1u;
constexpr uint32_t kArSgDevice = 0x4005u;   /* the speaker endpoint's subgraph */
constexpr uint32_t kArSgStream = 0x4001u;   /* the shared memory stream's subgraph */
/* The container's own id is the subgraph's id: that is how the reference
 * client builds the graph open (one container per subgraph, named after
 * it).  The framework only checks that the module list names the same
 * container as the container configuration does. */
constexpr uint32_t kArContainerDevice = kArSgDevice;
constexpr uint32_t kArContainerStream = kArSgStream;
constexpr uint32_t kArIidEndpoint = 0x6051u;  /* the I2S sink */
constexpr uint32_t kArIidShmem = 0x6001u;     /* the shared memory endpoint */
constexpr uint32_t kArIidDecoder = 0x6002u;   /* the decoder */
constexpr uint32_t kArIidConverter = 0x6003u; /* the converter */

/* The period format: 48 kHz, 16 bit, stereo.  A period is 480 frames, so
 * 1920 bytes, and eight of them fill the buffer's first quarter. */
constexpr uint32_t kArRate = 48000u;
constexpr uint16_t kArBits = 16u;
constexpr uint16_t kArChannels = 2u;
constexpr uint32_t kArPeriodFrames = 480u;
constexpr uint32_t kArPeriodBytes = kArPeriodFrames * kArChannels * 2u;
constexpr uint32_t kArSlots = 8u;
constexpr uint32_t kArInFlight = 4u;
constexpr uint64_t kArBufferBytes = 64u * 1024u;

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
    uint64_t base;
    uint64_t region_size;
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
    QcomAdsp ladder;
    QcomAdspConfig config;
    Image dtb, adsp;
    GprSpfProbe spf;
    uint32_t pending;
    uint32_t rsc_done;
    uint32_t ipcc_irqs, rsc_irqs;
    uint32_t ipcc_irq_errors, rsc_irq_errors;
    uint32_t stage, limit;
    uint32_t completed_hooks, hook_mask;
    int last_error, failed_stage;
    bool smem_ready, smem_locked, ipcc_ready, rsc_ready, gLink_started;
    bool gpr_ready, glink_up, fatal, handover;
    uint32_t stop_entry;
    uint32_t scm_x0, scm_x1, scm_op, scm_pas;
    uint32_t fail_scm_x0, fail_scm_x1;
    uint32_t scm_resumes;
    uint32_t crash_word;
    uint32_t auth_cookie;
    uint32_t dtb_auth_cookie, adsp_auth_cookie;
    uint32_t smp2p_irqs, smp2p_kicks;
    uint32_t glink_irqs, aop_irqs;
    uint32_t crash_present, crash_size, crash_text;
    uint32_t qmp_pre_state, qmp_post_state, order_flags;
    uint32_t lcx_index, lmx_index, lcx_addr, lmx_addr, rsc_vote_done;
    uint32_t rsc_drv_id;
    uint32_t rsc_id0, rsc_id1, rsc_id2;
    uint32_t tcs_cmd_enable, tcs_wait, tcs_control, tcs_irq_enable, tcs_irq_status, tcs_status;
    uint32_t smp2p_ap_allocated, smp2p_first_ms, smp2p_check_rc;
    uint32_t smp2p_magic, smp2p_version, smp2p_pids, smp2p_counts, smp2p_flags;
    uint32_t smp2p_entry0_name0, smp2p_entry0_name1, smp2p_entry0_value;
    uint32_t smp2p_entry1_name0, smp2p_entry1_value;
    bool smp2p_bad_kicked, smp2p_header_ok;
    uint32_t qmp_response;
    uint32_t invalid_auth_cookie;
    uint32_t scm_stage_mask;
    uint32_t glink_desc_size, glink_tx_size, glink_rx_size;
    uint32_t glink_desc_present, glink_tx_present, glink_rx_present;
    uint32_t glink_version_agreed, glink_channel_up;
    uint32_t gpr_req_opcode, gpr_req_token, gpr_req_src_port, gpr_req_dst_port, gpr_req_size;
    uint32_t gpr_rsp_opcode, gpr_rsp_token, gpr_rsp_src_port, gpr_rsp_dst_port, gpr_rsp_size;
    uint32_t gpr_result_opcode, gpr_result_status, gpr_rsp_payload0, gpr_rsp_payload1, gpr_token_match;
    uint8_t gpr_req_raw[64], gpr_rsp_raw[64];
    uint32_t gpr_req_raw_len, gpr_rsp_raw_len;
    uint64_t gpr_next_query_ms;
    uint32_t loaded_segments;
    uint64_t meta_phys, meta_bytes;
    uint64_t mdt_header, mdt_hash, mdt_file, mdt_span;
    uint32_t mdt_count, mdt_reloc;
    uint8_t spf_packet[64];
    uint32_t spf_length;
    uint64_t spf_sent_ms;
    uint8_t graph_packet[512];
    FuxiGraphWait graph_wait;
    FuxiGraphReply graph_open_reply, graph_config_reply, graph_close_reply;
    FuxiGraphReply graph_prepare_reply, graph_start_reply;
    FuxiGraphReply graph_device_prepare_reply, graph_device_start_reply;
    uint32_t graph_miss_count, graph_miss_len, graph_miss_word;
    uint32_t graph_bind_port, graph_bind_token, graph_bind_apm;
    uint32_t amp_state[2], amp_error[2], amp_device_id[2], amp_revision[2], amp_eint1[2], amp_eint2[2];
    uint32_t amp_prepare[2], amp_geni_error[2], amp_last_error[2];
    /* The amplifier bring-up: the two GENI engines (the same engine, one per
     * address), the bit per amplifier that answered with its device id. */
    GeniI2c amp_geni[CS35_AMPS];
    uint32_t amp_found;
    FuxiGraphReply graph_map_reply, graph_write_reply;
    FuxiGraphReply graph_stream_reply, graph_stream_close_reply;
    FuxiGraphReply graph_playback_reply, graph_shmem_reply;
    uint32_t graph_req_size, graph_rsp_size;
    uint32_t graph_phase;
    IOBufferMemoryDescriptor *pcm_buffer;
    IOMemoryMap *pcm_map;
    volatile uint8_t *pcm_va;
    uint64_t pcm_pa;
    uint64_t pcm_segment_bytes;
    uint32_t audio_smmu_idr0, audio_smmu_idr1, audio_smmu_idr2;
    uint32_t audio_smmu_matches, audio_smmu_slot0, audio_smmu_smr0, audio_smmu_s2cr0;
    uint32_t audio_smmu_free_slot, audio_smmu_free_bank;
    uint32_t audio_smmu_candidate_sctlr, audio_smmu_candidate_cbar, audio_smmu_candidate_cba2r;
    uint32_t audio_bypass_err[3], audio_bypass_slot[3];
    /* The period queue: the address the DSP was given for each slot, and
     * whether that slot is still with the DSP. */
    uint64_t period_address[kArSlots];
    uint8_t period_active[kArSlots];
    uint32_t period_in_flight, period_sent, period_done, period_late;
    uint64_t period_dsp_address;   /* the buffer as the DSP takes it */
    uint32_t period_handle;        /* the DSP's handle for the mapped buffer */
    uint32_t period_counter;       /* how many periods have been written */
    uint32_t period_pump;          /* the pump is running */
    uint32_t period_status;        /* the last WRITE_DONE's status */
    uint32_t period_tone;          /* the tone generator is on */
    uint32_t amp_unmuted;          /* the amplifiers that were unmuted */
    uint32_t graph_device_ready, graph_stream_ready;
    /* The amplifiers after the power-up: the PLL lock and the error bits. */
    uint32_t amp_pll_lock, amp_errors, amp_pup, amp_powered;
    uint8_t storage[kGlinkStorageBytes];
};

/* The shared page of the audio service (QcomAudioAbi.h): the control page, the playback ring and the capture ring.
 * The iOS audio server's plug-in maps all three; the samples move through the playback ring and the DSP takes
 * them a period at a time.  This is sheng's arrangement (package/AudioProbe.cpp setupShared) with the board's
 * own numbers. */
struct Shared {
    IOBufferMemoryDescriptor *control, *outRing, *inRing;
    QcomAudioControl *ctl;
    int16_t *out, *in;
    volatile uint32_t serving;   // the service accepts requests
    volatile uint32_t want;      // QcomAudioOutputRunning / QcomAudioInputRunning as the plug-in asks
    uint64_t next;               // the playback period to send next (absolute, in periods)
    uint64_t frames;             // the frames the service has consumed
    uint32_t late;               // periods sent without fresh data
    uint64_t anchor;             // the host time the timeline starts at
    uint64_t ticks_per_frame_q32;
    uint32_t started;            // the first period has been placed on the timeline
};

Shared gShared;

Owner gOwner;
FuxiAdspPlatformHooks gHooks;
uint32_t gHookMask;
bool gStarted;

uint64_t nowMs()
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000000ull;
}

void syncStats()
{
    volatile uint32_t *s = native_fuxi_adsp_stats;
    s[0] = kStatsAbi;
    s[3] = fuxi_audio_binding()->field_count;
    s[4] = fuxi_audio_binding()->state_count[FuxiAudioUnverified];
    s[5] = fuxi_audio_binding()->state_count[FuxiAudioChosen];
    s[6] = fuxi_audio_binding()->state_count[FuxiAudioVerified];
    s[7] = gHookMask;
    s[8] = gOwner.completed_hooks;
    s[9] = gOwner.stage ? gOwner.stage : gOwner.ladder.stage;
    s[10] = static_cast<uint32_t>(gOwner.last_error);
    s[11] = static_cast<uint32_t>(gOwner.failed_stage);
    s[16] = gOwner.scm_x0;
    s[17] = gOwner.scm_x1;
    s[18] = gOwner.scm_op;
    s[19] = gOwner.scm_pas;
    s[21] = gOwner.ipcc_irqs;
    s[22] = static_cast<uint32_t>(gOwner.glink.version_agreed) |
           (static_cast<uint32_t>(gOwner.glink_up) << 1) |
           (static_cast<uint32_t>(gOwner.glink.broken) << 2) |
           (static_cast<uint32_t>(gOwner.ipcc_irq_errors) << 8) |
           (static_cast<uint32_t>(gOwner.rsc_irq_errors) << 12);
    s[23] = gOwner.glink.rx_messages;
    s[24] = gOwner.glink.tx_messages;
    s[25] = gOwner.glink.unknown_opens;
    s[26] = gOwner.spf.last_opcode;
    s[27] = gOwner.spf.last_status;
    s[28] = gOwner.spf.state | (gOwner.spf.queries << 8) | (gOwner.spf.errors << 16) |
            (gOwner.spf.timeouts << 24);
    s[29] = static_cast<uint32_t>(gOwner.qmp.state);
    s[30] = (gOwner.limit & 0x1fu) | (gOwner.dtb.mdt.count << 8) |
            (gOwner.adsp.mdt.count << 16) | (gOwner.loaded_segments << 24);
    s[31] = (gOwner.fatal ? 1u : 0u) | (gOwner.handover ? 2u : 0u) |
            (gOwner.gpr_ready ? 4u : 0u) | (gOwner.gLink_started ? 8u : 0u) |
            (gOwner.glink_up ? 16u : 0u) | (gOwner.rsc_ready ? 32u : 0u) |
            (gOwner.ipcc_ready ? 64u : 0u) | (gOwner.smem_ready ? 128u : 0u);
    s[32] = static_cast<uint32_t>(gOwner.meta_phys);
    s[33] = static_cast<uint32_t>(gOwner.meta_phys >> 32);
    s[34] = static_cast<uint32_t>(gOwner.meta_bytes);
    s[35] = static_cast<uint32_t>(gOwner.mdt_header);
    s[36] = static_cast<uint32_t>(gOwner.mdt_hash);
    s[37] = static_cast<uint32_t>(gOwner.mdt_file);
    s[38] = (gOwner.mdt_count & 0xffu) | ((gOwner.mdt_reloc & 1u) << 8) |
            ((gOwner.mdt_span & 0xffffu) << 12);
    s[39] = static_cast<uint32_t>(gOwner.mdt_span >> 16);
    s[40] = gOwner.fail_scm_x0;
    s[41] = gOwner.fail_scm_x1;
    s[42] = gOwner.scm_resumes;
    s[43] = gOwner.limit;
    s[44] = gOwner.smp2p.in ? 1u : 0u;
    s[45] = static_cast<uint32_t>(qcom_smp2p_find_in(&gOwner.smp2p, fuxi_audio_binding()->adsp.ready_entry));
    s[46] = gOwner.rsc_irqs;
    s[47] = gOwner.crash_word;
    s[48] = gOwner.loaded_segments;
    s[49] = gOwner.auth_cookie;
    s[50] = gOwner.adsp.init_x0;
    s[51] = gOwner.adsp.mem_x0;
    s[52] = static_cast<uint32_t>(gOwner.mdt_span);
    s[53] = static_cast<uint32_t>(gOwner.mdt_span >> 32);
    s[54] = gOwner.dtb_auth_cookie;
    s[55] = gOwner.adsp_auth_cookie;
    s[56] = gOwner.crash_present;
    s[57] = gOwner.crash_size;
    s[58] = gOwner.crash_text;
    s[59] = gOwner.smp2p_irqs;
    s[60] = gOwner.smp2p_kicks;
    s[61] = gOwner.qmp_pre_state;
    s[62] = gOwner.qmp_post_state;
    s[63] = gOwner.order_flags;
    s[64] = (gOwner.lcx_index & 0xffu) | ((gOwner.lmx_index & 0xffu) << 8) | ((gOwner.rsc_vote_done & 1u) << 16);
    s[65] = gOwner.lcx_addr;
    s[66] = gOwner.lmx_addr;
    s[67] = gOwner.rsc_irqs;
    s[68] = gOwner.rsc_drv_id;
    s[69] = gOwner.layout.tcs_stride;
    s[70] = gOwner.rsc_id0;
    s[71] = gOwner.rsc_id1;
    s[72] = gOwner.rsc_id2;
    s[73] = gOwner.tcs_cmd_enable;
    s[74] = gOwner.tcs_wait;
    s[75] = gOwner.tcs_control;
    s[76] = gOwner.tcs_irq_enable;
    s[77] = gOwner.tcs_irq_status;
    s[78] = gOwner.tcs_status;
    s[79] = gOwner.rsc_vote_done;
    s[80] = gOwner.smp2p_ap_allocated;
    s[81] = static_cast<uint32_t>(gOwner.smp2p_check_rc);
    s[82] = gOwner.smp2p_magic;
    s[83] = gOwner.smp2p_version;
    s[84] = gOwner.smp2p_pids;
    s[85] = gOwner.smp2p_counts;
    s[86] = gOwner.smp2p_flags;
    s[87] = gOwner.smp2p_entry0_name0;
    s[88] = gOwner.smp2p_entry0_name1;
    s[89] = gOwner.smp2p_entry0_value;
    s[90] = gOwner.smp2p_entry1_name0;
    s[91] = gOwner.smp2p_entry1_value;
    s[92] = gOwner.smp2p_first_ms;
    s[93] = gOwner.smp2p.in ? 1u : 0u;
    s[94] = native_fuxi_adsp_stats[20];
    s[95] = gOwner.smp2p_kicks;
    s[96] = gOwner.scm_stage_mask;
    s[97] = gOwner.scm_x0;
    s[98] = gOwner.scm_x1;
    s[99] = gOwner.scm_op;
    s[100] = gOwner.scm_pas;
    s[101] = (gOwner.glink_desc_present ? 1u : 0u) |
             (gOwner.glink_tx_present ? 2u : 0u) |
             (gOwner.glink_rx_present ? 4u : 0u);
    s[102] = gOwner.glink_desc_size;
    s[103] = gOwner.glink_tx_size;
    s[104] = gOwner.glink_rx_size;
    s[105] = (gOwner.glink.version_agreed ? 1u : 0u) |
             (gOwner.glink_up ? 2u : 0u) |
             (gOwner.glink.broken ? 4u : 0u) |
             (gOwner.glink_channel_up ? 8u : 0u);
    s[106] = gOwner.glink.rx_messages;
    s[107] = gOwner.glink.tx_messages;
    s[108] = gOwner.glink.unknown_opens;
    s[109] = gOwner.ladder.stage;
    s[110] = static_cast<uint32_t>(gOwner.ladder.state);
    s[111] = gOwner.fatal ? 1u : 0u;
    s[112] = gGraphProbeLevel ? gOwner.graph_phase : gOwner.spf.answers;
    s[113] = gGraphProbeLevel >= 3u ? gOwner.graph_playback_reply.status :
             (gGraphProbeLevel ? QCOM_AR_OP_GRAPH_OPEN : gOwner.gpr_req_opcode);
    s[114] = gGraphProbeLevel >= 3u ? gOwner.graph_shmem_reply.status :
             (gGraphProbeLevel ? 0x1101u : gOwner.gpr_req_token);
    s[115] = gGraphProbeLevel ? gOwner.graph_open_reply.opcode : gOwner.gpr_rsp_opcode;
    s[116] = gGraphProbeLevel ? gOwner.graph_open_reply.result_opcode : gOwner.gpr_rsp_token;
    s[117] = gGraphProbeLevel ? gOwner.graph_open_reply.result_opcode : gOwner.gpr_result_opcode;
    s[118] = gGraphProbeLevel ? gOwner.graph_open_reply.status : gOwner.gpr_result_status;
    s[119] = gGraphProbeLevel ? gOwner.graph_close_reply.status : gOwner.gpr_rsp_payload0;
    s[120] = gGraphProbeLevel ? gOwner.graph_config_reply.status : gOwner.gpr_rsp_payload1;
    s[121] = gOwner.gpr_req_src_port;
    s[122] = gOwner.gpr_req_dst_port;
    s[123] = gOwner.gpr_rsp_src_port;
    s[124] = gOwner.gpr_rsp_dst_port;
    s[125] = gGraphProbeLevel ? (gOwner.graph_wait.active ? 0u : 1u) : gOwner.gpr_token_match;
    s[126] = gGraphProbeLevel ? gOwner.graph_req_size : gOwner.gpr_req_size;
    s[127] = gGraphProbeLevel ? gOwner.graph_rsp_size : gOwner.gpr_rsp_size;
    if (gGraphProbeLevel >= 5u && gOwner.graph_phase >= 21u) {
        /* Sixteen 12-bit DIAGSTATUS pages are the only persistent transport
         * available after an early audit.  Preserve page 0 (stage/error) and
         * page 15 (phase); use pages 1..11 for the owned-buffer/SMMU result. */
        s[101] = static_cast<uint32_t>(gOwner.pcm_pa) & 0xfffu;
        s[105] = static_cast<uint32_t>(gOwner.pcm_pa >> 12) & 0xfffu;
        s[28] = static_cast<uint32_t>(gOwner.pcm_pa >> 24) & 0xffffu;
        s[113] = static_cast<uint32_t>(gOwner.pcm_pa >> 40) & 0xfffu;
        s[114] = static_cast<uint32_t>(gOwner.pcm_segment_bytes >> 12) & 0xfffu;
        s[115] = gOwner.audio_smmu_free_slot & 0xfffu;
        s[117] = gOwner.audio_smmu_free_bank & 0xfffu;
        s[119] = gOwner.audio_smmu_candidate_sctlr & 0xfffu;
        s[120] = gOwner.audio_smmu_candidate_cbar & 0xfffu;
        s[96] = gOwner.audio_smmu_idr0 & 0xfffu;
        s[31] = gOwner.audio_smmu_idr1 & 0xfffu;
        s[116] = gOwner.audio_smmu_idr2 & 0xfffu;
    }
    if (gGraphProbeLevel) {
        /* P2 diagnostic: page 10/11 carry the prepare/start reply status, page
         * 12/7 their tokens. 0xffffffff means no reply arrived at all. */
        s[96] = gOwner.graph_prepare_reply.status;
        s[31] = gOwner.graph_start_reply.status;
        s[116] = gOwner.graph_prepare_reply.token;
        s[117] = gOwner.graph_start_reply.token;
        /* Start reply in full: page 6 opcode, 7 result_opcode, 8 length,
         * 9 status, 12 token, 11 src_port (all low 12 bits). */
        s[115] = gOwner.graph_start_reply.opcode & 0xfffu;
        s[117] = gOwner.graph_start_reply.result_opcode & 0xfffu;
        s[119] = gOwner.graph_start_reply.length & 0xfffu;
        s[120] = gOwner.graph_start_reply.status & 0xfffu;
        s[116] = gOwner.graph_start_reply.token & 0xfffu;
        s[31]  = gOwner.graph_start_reply.src_port & 0xfffu;
        if (gOwner.graph_bind_port) {
            s[96] = gOwner.graph_map_reply.status;       /* page 10: map status */
            s[31] = gOwner.graph_write_reply.status;     /* page 11: write status */
            /* Full map reply: page 6 opcode, 7 result_opcode, 8 length,
             * 9 status, 12 handle (all low 12 bits). */
            s[115] = gOwner.graph_map_reply.opcode & 0xfffu;
            s[117] = gOwner.graph_map_reply.result_opcode & 0xfffu;
            s[119] = gOwner.graph_map_reply.length & 0xfffu;
            s[120] = gOwner.graph_map_reply.status & 0xfffu;
            s[116] = gOwner.graph_map_reply.handle & 0xfffu;
        }
        if (!gOwner.graph_bind_port) {
            s[120] = gOwner.graph_miss_count & 0xfffu;  /* page 9 */
            s[121] = gOwner.graph_miss_len & 0x7fu;     /* page 13 bits 1..7 */
            s[122] = gOwner.graph_miss_word & 0x1fu;    /* page 13 bits 8..12 */
        }
    }
    if (gOwner.amp_prepare[0] || gOwner.amp_prepare[1] ||
        gOwner.amp_state[0] || gOwner.amp_state[1]) {
        /* The amplifier bring-up.  The stage says how far it got, the
         * identity is the device id of the two amplifiers, and the engine's
         * own numbers say whether the bus came up.  The serial log carries
         * the full 32-bit words: these pages are 12 bits wide. */
        s[115] = gOwner.amp_state[0] & 0xfffu;      /* page 6:  the stage */
        s[117] = gOwner.amp_error[1] & 0xfffu;       /* page 7:  the amplifiers found */
        s[119] = gOwner.amp_geni_error[0] & 0xfffu;  /* page 8:  the engine's protocol */
        s[120] = gOwner.amp_state[1] & 0xfffu;      /* page 9:  the DFS level */
        s[96]  = gOwner.amp_device_id[0] & 0xfffu;  /* page 10: amplifier T's device id */
        s[31]  = gOwner.amp_device_id[1] & 0xfffu;  /* page 11: amplifier B's device id */
        s[116] = gOwner.amp_revision[0] & 0xfffu;   /* page 12: amplifier T's revision */
    }
}

void setStage(uint32_t stage)
{
    if (stage > gOwner.stage) gOwner.stage = stage;
    if (stage > gOwner.ladder.stage) gOwner.ladder.stage = stage;
    syncStats();
}

void completeHook(uint32_t bit)
{
    gOwner.completed_hooks |= bit;
    syncStats();
}

void failStage(uint32_t stage, int error)
{
    if (!gOwner.stage || gOwner.stage < stage) gOwner.stage = stage;
    if (!gOwner.failed_stage) gOwner.failed_stage = -static_cast<int>(stage ? stage : 1u);
    gOwner.last_error = error;
    gOwner.ladder.last_error = error;
    gOwner.ladder.failed_stage = gOwner.failed_stage;
    gOwner.ladder.state = QcomAdspFailed;
    native_fuxi_adsp_stats[2] = static_cast<uint32_t>(error);
    syncStats();
    kprintf("FUXI_ADSP fail stage=%d hook=%02x err=%d scm=%08x/%08x op=%u pas=%u\n",
            gOwner.failed_stage, gOwner.completed_hooks, error, gOwner.scm_x0,
            gOwner.scm_x1, gOwner.scm_op, gOwner.scm_pas);
}

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

/* ---- SMEM: allocation under the TCSR hardware mutex, bounded ---- */
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

void readCrashReason()
{
    QcomSmemPartition global = {};
    uint32_t at = 0, size = 0;
    if (qcom_smem_partition(&gOwner.smem, QCOM_SMEM_HOST_GLOBAL, QCOM_SMEM_HOST_GLOBAL, &global) ||
        qcom_smem_find(&gOwner.smem, &global, static_cast<uint16_t>(fuxi_audio_binding()->platform.smp2p_crash_item), &at, &size) ||
        !size)
        return;
    gOwner.crash_word = qcom_smem_read32(&gOwner.smem, at);
    gOwner.crash_present = 1;
    gOwner.crash_size = size;
    gOwner.crash_text = gOwner.crash_word & 0xffu;
    char text[64];
    size_t n = 0;
    for (; n < sizeof text - 1 && n < size; ++n) {
        const char c = static_cast<char>(gOwner.smem_window.va[at + n]);
        if (!c) break;
        text[n] = c >= 32 && c < 127 ? c : '.';
    }
    text[n] = 0;
    if (n) kprintf("FUXI_ADSP crash=%s\n", text);
}

/* ---- secure world: every call keeps x0/x1 and the exact PAS stage ---- */
struct ScmResult { int64_t x0; uint64_t x1; };

void noteScm(unsigned stage, bool ok)
{
    if (ok) gOwner.scm_stage_mask |= 1u << stage;
    else gOwner.scm_stage_mask &= ~(1u << stage);
}

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
    gOwner.scm_resumes = static_cast<uint32_t>(resumes);
    gOwner.scm_x0 = static_cast<uint32_t>(result.x0);
    gOwner.scm_x1 = static_cast<uint32_t>(result.x1);
    gOwner.scm_op = command;
    gOwner.scm_pas = pas;
    syncStats();
    return result;
}

bool scmOk(const ScmResult &result) { return result.x0 == 0 && result.x1 == 0; }

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

/* ---- one signed split image ---- */
int firmwareRead(void *context, uint64_t offset, void *to, size_t length)
{
    return fuxi_adsp_fw_read(context, offset, to, length);
}

/* Reads the .mdt file, which is the file the secure world authenticates the metadata out of. */
int firmwareReadMdt(void *context, uint64_t offset, void *to, size_t length)
{
    return fuxi_adsp_fw_read_mdt(context, offset, to, length);
}

/* Builds the metadata blob: the ELF header followed by the hash segment, both out of the .mdt file
 * when the firmware has one. The joined image of the .bNN pieces has the hash at another offset in
 * another file, and the two are not the same bytes. */
int buildMetadata(Image &image, uint8_t *va, uint64_t bytes)
{
    return image.mdt.mdt_size
        ? qcom_mdt_metadata_from_mdt(&image.mdt, firmwareRead, image.file, firmwareReadMdt, image.file, va, bytes)
        : qcom_mdt_metadata(&image.mdt, firmwareRead, image.file, va, bytes);
}

int openImage(Image &image, const char *path, uint32_t size_slot, uint32_t segment_slot)
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
    /* The .mdt file's own size: the metadata is taken out of it. */
    image.mdt.mdt_size = fuxi_adsp_fw_mdt_size(image.file);
    native_fuxi_adsp_stats[size_slot] = static_cast<uint32_t>(image.size >> 10);
    native_fuxi_adsp_stats[segment_slot] = image.mdt.count;
    syncStats();
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

int authenticateHeader(Image &image, uint32_t pas, uint64_t base, unsigned stage)
{
    const uint64_t bytes = qcom_mdt_metadata_size(&image.mdt);
    if (!bytes) return -5;
    image.meta = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),
        /* The secure world accepted a metadata buffer in the high DRAM
         * segment (0x8_...); no 32-bit restriction is needed. */
        bytes, 0xfffffffffffff000ull);
    if (!image.meta || image.meta->prepare() != kIOReturnSuccess) return -12;
    uint8_t *va = static_cast<uint8_t *>(image.meta->getBytesNoCopy());
    if (!va || buildMetadata(image, va, bytes)) return -5;
    cleanToPoc(va, static_cast<size_t>(bytes));
    gOwner.meta_bytes = bytes;
    gOwner.mdt_header = image.mdt.header_size;
    gOwner.mdt_hash = image.mdt.segment[image.mdt.hash].file_size;
    gOwner.mdt_file = image.mdt.file_size;
    gOwner.mdt_count = image.mdt.count;
    gOwner.mdt_reloc = image.mdt.relocatable;
    gOwner.mdt_span = qcom_mdt_span(&image.mdt);
    const uint64_t phys = image.meta->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    gOwner.meta_phys = phys;
    /* Fuxi's secure world returns a positive image handle in x0 for a
     * successful INIT_IMAGE (observed 0x00cfffe5, x1=0); Sheng saw 0.  Keep
     * negative results as failures and let MEM_SETUP/AUTH prove the handle. */
    const ScmResult init = secureCall(QCOM_SCM_PAS_INIT_IMAGE,
                                      qcom_scm_args(2, QCOM_SCM_VAL, QCOM_SCM_RW, 0), pas, phys, 0, 0);
    image.init_x0 = static_cast<uint32_t>(init.x0);
    noteScm(stage, scmOk(init));
    if (!scmOk(init)) return -1;
    if (image.mdt.relocatable) {
        const ScmResult mem = secureCall(QCOM_SCM_PAS_MEM_SETUP, qcom_scm_args(3, 0, 0, 0), pas, base,
                                         qcom_mdt_span(&image.mdt), 0);
        image.mem_x0 = static_cast<uint32_t>(mem.x0);
        noteScm(stage + 1, scmOk(mem));
        if (!scmOk(mem)) return -2;
    } else {
        noteScm(stage + 1, true);
    }
    return 0;
}

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
            const size_t chunk = segment.file_size - done < kImageChunk ? static_cast<size_t>(segment.file_size - done) : kImageChunk;
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

void shutdownDsp(bool dtb, bool adsp)
{
    if (adsp && gOwner.adsp.reset) {
        (void)secureCall(QCOM_SCM_PAS_SHUTDOWN, qcom_scm_args(1, 0, 0, 0), fuxi_audio_binding()->platform.pas_adsp, 0, 0, 0);
        gOwner.adsp.reset = false;
    }
    if (dtb && gOwner.dtb.reset) {
        (void)secureCall(QCOM_SCM_PAS_SHUTDOWN, qcom_scm_args(1, 0, 0, 0), fuxi_audio_binding()->platform.pas_dtb, 0, 0, 0);
        gOwner.dtb.reset = false;
    }
}

/* ---- RPMh proxy votes (best effort evidence; a missing done IRQ is not fatal) ---- */
bool readCmdDb(uint8_t *copy)
{
    Window db = {};
    if (!mapWindow(db, fuxi_audio_binding()->platform.cmd_db_base, fuxi_audio_binding()->platform.cmd_db_size)) return false;
    for (uint64_t at = 0; at < fuxi_audio_binding()->platform.cmd_db_size; at += 4) {
        const uint32_t word = rd(db, at);
        memcpy(copy + at, &word, 4);
    }
    unmapWindow(db);
    return true;
}

int voteRails()
{
    const FuxiAdspPlatformBinding &p = fuxi_audio_binding()->platform;
    if (!gOwner.rsc_window.va) return -1;
    struct CmdDbCopy {
        uint8_t *bytes;
        uint32_t bytes_size;
        explicit CmdDbCopy(uint32_t size) : bytes(static_cast<uint8_t *>(IOMallocData(size))), bytes_size(size) {}
        ~CmdDbCopy() { if (bytes) IOFreeData(bytes, bytes_size); }
    } copy(static_cast<uint32_t>(p.cmd_db_size));
    QcomRpmhResource lcx = {}, lmx = {};
    uint32_t lcx_index = 0, lmx_index = 0;
    if (!copy.bytes || !readCmdDb(copy.bytes) ||
        qcom_cmd_db_find(copy.bytes, p.cmd_db_size, "lcx.lvl", &lcx) ||
        qcom_cmd_db_find(copy.bytes, p.cmd_db_size, "lmx.lvl", &lmx) ||
        qcom_rpmh_level_index(&lcx, 0xffffffffu, &lcx_index) ||
        qcom_rpmh_level_index(&lmx, 0xffffffffu, &lmx_index))
        return -2;

    gOwner.lcx_index = lcx_index; gOwner.lmx_index = lmx_index;
    gOwner.lcx_addr = lcx.address; gOwner.lmx_addr = lmx.address;
    const uint32_t addresses[2] = {lcx.address, lmx.address};
    const uint32_t values[2] = {lcx_index, lmx_index};
    QcomRpmhWrite writes[16];
    const unsigned count = qcom_rpmh_active_request(&gOwner.layout, kRscTcs, addresses, values, 2, writes, 16);
    if (!count) return -22;
    const uint32_t tcs_base = gOwner.layout.tcs_stride * kRscTcs;
    /* Sheng enables the TCS interrupt at TCS-block offset 0 before the
     * request.  The first Fuxi rounds omitted it, so no done IRQ was seen. */
    const uint32_t irq_enable = rd(gOwner.rsc_window, p.rsc_tcs_offset) | (1u << kRscTcs);
    wr(gOwner.rsc_window, p.rsc_tcs_offset, irq_enable);
    gOwner.tcs_irq_enable = rd(gOwner.rsc_window, p.rsc_tcs_offset);
    __atomic_store_n(&gOwner.rsc_done, 0, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < count; ++i)
        wr(gOwner.rsc_window, p.rsc_tcs_offset + writes[i].offset, writes[i].value);
    gOwner.tcs_cmd_enable = rd(gOwner.rsc_window, p.rsc_tcs_offset + tcs_base + gOwner.layout.cmd_enable);
    gOwner.tcs_wait = rd(gOwner.rsc_window, p.rsc_tcs_offset + tcs_base + gOwner.layout.wait_for_cmpl);
    gOwner.tcs_control = rd(gOwner.rsc_window, p.rsc_tcs_offset + tcs_base + gOwner.layout.control);
    gOwner.tcs_status = rd(gOwner.rsc_window, p.rsc_tcs_offset + tcs_base + gOwner.layout.status);
    gOwner.tcs_irq_status = rd(gOwner.rsc_window, p.rsc_tcs_offset + gOwner.layout.irq_status);
    uint64_t deadline = 0;
    clock_interval_to_deadline(100, kMillisecondScale, &deadline);
    while (!(__atomic_load_n(&gOwner.rsc_done, __ATOMIC_ACQUIRE) & (1u << kRscTcs)) && mach_absolute_time() < deadline)
        serviceEvents(waitEvents(20));
    bool done = (__atomic_load_n(&gOwner.rsc_done, __ATOMIC_ACQUIRE) & (1u << kRscTcs)) != 0;
    if (!done && (rd(gOwner.rsc_window, p.rsc_tcs_offset + gOwner.layout.irq_status) & (1u << kRscTcs))) done = true;
    const unsigned finish = qcom_rpmh_finish(&gOwner.layout, kRscTcs, writes, 16);
    for (unsigned i = 0; i < finish; ++i)
        wr(gOwner.rsc_window, p.rsc_tcs_offset + writes[i].offset, writes[i].value);
    gOwner.rsc_vote_done = done ? 1u : 0u;
    if (!done) gOwner.rsc_irq_errors |= 1u << 15;
    return 0;
}

/* ---- interrupts: acknowledge, record, wake ---- */
void ipccIrq(void *)
{
    uint32_t bits = 0;
    for (unsigned n = 0; n < 32; ++n) {
        const uint32_t id = rd(gOwner.ipcc_window, QCOM_IPCC_RECV_ID);
        if (id == QCOM_IPCC_NONE) break;
        wr(gOwner.ipcc_window, QCOM_IPCC_RECV_CLEAR, id);
        if (id == qcom_ipcc_id(QCOM_IPCC_CLIENT_LPASS, QCOM_IPCC_SIGNAL_SMP2P)) { bits |= kEvSmp2p; gOwner.smp2p_irqs++; }
        else if (id == qcom_ipcc_id(QCOM_IPCC_CLIENT_LPASS, QCOM_IPCC_SIGNAL_GLINK_QMP)) { bits |= kEvGlink; gOwner.glink_irqs++; }
        else if (id == qcom_ipcc_id(QCOM_IPCC_CLIENT_AOP, QCOM_IPCC_SIGNAL_GLINK_QMP)) { bits |= kEvAop; gOwner.aop_irqs++; }
        else gOwner.ipcc_irq_errors++;
    }
    __atomic_add_fetch(&gOwner.ipcc_irqs, 1, __ATOMIC_RELAXED);
    if (bits) wake(bits);
}

void rscIrq(void *)
{
    const uint32_t status = rd(gOwner.rsc_window, fuxi_audio_binding()->platform.rsc_tcs_offset + gOwner.layout.irq_status) & (1u << kRscTcs);
    if (!status) return;
    wr(gOwner.rsc_window, fuxi_audio_binding()->platform.rsc_tcs_offset + gOwner.layout.irq_clear, status);
    __atomic_fetch_or(&gOwner.rsc_done, status, __ATOMIC_RELEASE);
    __atomic_add_fetch(&gOwner.rsc_irqs, 1, __ATOMIC_RELAXED);
    wake(kEvRsc);
}

void kickAop(void *)
{
    wr(gOwner.ipcc_window, QCOM_IPCC_SEND, qcom_ipcc_id(QCOM_IPCC_CLIENT_AOP, QCOM_IPCC_SIGNAL_GLINK_QMP));
}

void kickGlink(void *)
{
    wr(gOwner.ipcc_window, QCOM_IPCC_SEND, qcom_ipcc_id(QCOM_IPCC_CLIENT_LPASS, QCOM_IPCC_SIGNAL_GLINK_QMP));
}

void kickSmp2p()
{
    wr(gOwner.ipcc_window, QCOM_IPCC_SEND, qcom_ipcc_id(QCOM_IPCC_CLIENT_LPASS, QCOM_IPCC_SIGNAL_SMP2P));
    gOwner.smp2p_kicks++;
}

/* ---- SMP2P ---- */
void serviceSmp2p()
{
    if (!gOwner.smp2p.in) {
        if (volatile uint8_t *in = item(static_cast<uint16_t>(fuxi_audio_binding()->platform.smp2p_in_item), nullptr)) {
            gOwner.smp2p.in = reinterpret_cast<volatile uint32_t *>(in);
            gOwner.smp2p_first_ms = static_cast<uint32_t>(nowMs());
            kickSmp2p();
        } else {
            gOwner.smp2p_check_rc = 0;
            return;
        }
    }
    volatile uint32_t *in = gOwner.smp2p.in;
    gOwner.smp2p_magic = in[0];
    gOwner.smp2p_version = in[1];
    gOwner.smp2p_pids = in[2];
    gOwner.smp2p_counts = in[3];
    gOwner.smp2p_flags = in[4];
    gOwner.smp2p_entry0_name0 = in[5];
    gOwner.smp2p_entry0_name1 = in[6];
    gOwner.smp2p_entry0_value = in[9];
    gOwner.smp2p_entry1_name0 = in[10];
    gOwner.smp2p_entry1_value = in[14];
    const uint16_t remote = static_cast<uint16_t>(fuxi_audio_binding()->platform.remote_pid);
    const int check = qcom_smp2p_check_in(in, 0, remote);
    gOwner.smp2p_check_rc = check;
    if (check) {
        if (!gOwner.smp2p_bad_kicked) {
            gOwner.smp2p_bad_kicked = true;
            kickSmp2p();
        }
        syncStats();
        return;
    }
    if (!gOwner.smp2p_header_ok) {
        gOwner.smp2p_header_ok = true;
        kickSmp2p();
    }
    uint32_t value = 0, changed = 0;
    if (qcom_smp2p_service(&gOwner.smp2p, fuxi_audio_binding()->adsp.ready_entry, &value, &changed))
        kickSmp2p();
    native_fuxi_adsp_stats[20] = value;
    gOwner.smp2p_entry0_value = value;
    if (changed & value & fuxi_audio_binding()->adsp.fatal_value) {
        gOwner.fatal = true;
        readCrashReason();
    }
    if ((value & (1u << 2)) != 0) gOwner.handover = true;
    syncStats();
}

/* ---- GPR / SPF readiness on adsp_apps ---- */
void dspState(void *, QcomGlinkChannel *, int up)
{
    gOwner.glink_up = up != 0;
    gOwner.glink_channel_up = up != 0;
    if (!up) fuxi_graph_wait_cancel(&gOwner.graph_wait);
    if (up) gOwner.glink_version_agreed = gOwner.glink.version_agreed;
    syncStats();
    if (up) wake(kEvGlink);
}

static uint32_t payloadWord(const GprPacket &packet, unsigned index)
{
    if (packet.payload_bytes < (index + 1u) * 4u) return 0;
    const uint8_t *p = packet.payload + index * 4u;
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
           static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}

void logGprBytes(const char *tag, const uint8_t *data, uint32_t length)
{
    char line[64];
    for (uint32_t at = 0; at < length; at += 8) {
        unsigned n = 0;
        for (uint32_t i = 0; i < 8 && at + i < length; ++i)
            n += snprintf(line + n, sizeof line - n, "%02x", data[at + i]);
        line[n] = 0;
        kprintf("%s%d %s\n", tag, at / 8, line);
    }
}

void dspReceive(void *context, QcomGlinkChannel *channel, const uint8_t *data, uint32_t length)
{
    if (channel != gOwner.channel) return;
    if (gGraphProbeLevel) {
        /* A period's answer is never a command's answer: the DSP took the
         * buffer and named it.  It belongs to the stream, so it is handled
         * here instead of going to the SPF state machine (which would drop
         * it) or to a command's wait (which would count it as a miss). */
        QcomArAnswer answer = {};
        if (qcom_ar_parse(data, length, &answer) == QcomArAnswerWriteDone) {
            FuxiGraphReply reply = {};
            reply.opcode = answer.opcode;
            reply.result_opcode = QCOM_AR_OP_WRITE;
            reply.status = answer.status;
            reply.handle = answer.handle;
            reply.address = answer.address;
            reply.token = answer.token;
            reply.length = length;
            graphHandleWriteDone(reply);
            wake(kEvGlink);
            return;
        }
    }
    if (gGraphProbeLevel && gOwner.graph_wait.active) {
        FuxiGraphReply reply;
        if (!fuxi_graph_wait_packet(&gOwner.graph_wait, data, length, 1, &reply)) {
            /* A packet arrived but did not match the expectation: keep its shape
             * so a silent "no reply" can be told apart from a mismatched reply. */
            ++gOwner.graph_miss_count;
            gOwner.graph_miss_len = length;
            gOwner.graph_miss_word = length >= 4u ?
                (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24) : 0;
            return;
        }
        {
            gOwner.graph_rsp_size = length;
            if (gOwner.graph_wait.kind == 11) gOwner.graph_write_reply = reply;
            else if (gOwner.graph_wait.kind == 10) gOwner.graph_map_reply = reply;
            else if (gOwner.graph_wait.kind == 13) gOwner.graph_start_reply = reply;
            else if (gOwner.graph_wait.kind == 12) gOwner.graph_prepare_reply = reply;
            else if (gOwner.graph_wait.kind == 9) gOwner.graph_device_start_reply = reply;
            else if (gOwner.graph_wait.kind == 8) gOwner.graph_device_prepare_reply = reply;
            else if (gOwner.graph_wait.kind == 7) gOwner.graph_shmem_reply = reply;
            else if (gOwner.graph_wait.kind == 6) gOwner.graph_playback_reply = reply;
            else if (gOwner.graph_wait.kind == 5) gOwner.graph_stream_close_reply = reply;
            else if (gOwner.graph_wait.kind == 4) gOwner.graph_stream_reply = reply;
            else if (gOwner.graph_wait.kind == 3) gOwner.graph_config_reply = reply;
            else if (gOwner.graph_wait.kind == 2) gOwner.graph_close_reply = reply;
            else gOwner.graph_open_reply = reply;
        }
        /* Irrelevant, late, malformed or unknown replies keep the wait
         * active.  Never feed graph traffic to the SPF state machine. */
        return;
    }
    if (!gOwner.gpr_rsp_raw_len && length) {
        gOwner.gpr_rsp_raw_len = length < sizeof gOwner.gpr_rsp_raw ? length : sizeof gOwner.gpr_rsp_raw;
        memcpy(gOwner.gpr_rsp_raw, data, gOwner.gpr_rsp_raw_len);
        logGprBytes("GPRR", gOwner.gpr_rsp_raw, gOwner.gpr_rsp_raw_len);
    }
    GprPacket packet = {};
    if (gpr_decode(data, length, &packet) == GprOk) {
        gOwner.gpr_rsp_opcode = packet.opcode;
        gOwner.gpr_rsp_token = packet.token;
        gOwner.gpr_rsp_src_port = packet.src_port;
        gOwner.gpr_rsp_dst_port = packet.dst_port;
        gOwner.gpr_rsp_size = packet.size;
        gOwner.gpr_rsp_payload0 = payloadWord(packet, 0);
        gOwner.gpr_rsp_payload1 = payloadWord(packet, 1);
        gOwner.gpr_token_match = packet.token == gOwner.gpr_req_token ? 1u : 0u;
        uint32_t opcode = 0, status = 0;
        if (!gpr_basic_result(&packet, &opcode, &status)) {
            gOwner.gpr_result_opcode = opcode;
            gOwner.gpr_result_status = status;
        }
    }
    const int result = gpr_spf_receive(&gOwner.spf, data, length);
    if (result != SpfIgnored) {
        native_fuxi_adsp_stats[26] = gOwner.spf.last_opcode;
        native_fuxi_adsp_stats[27] = gOwner.spf.last_status;
        syncStats();
        wake(kEvGpr);
    }
}

void serviceEvents(uint32_t bits)
{
    if (bits & kEvAop) (void)qcom_qmp_step(&gOwner.qmp);
    if (bits & (kEvSmp2p | kEvGlink)) serviceSmp2p();
    if (gOwner.gLink_started && (bits & (kEvGlink | kEvSend))) {
        if (!gOwner.glink.rx.data) {
            uint32_t size = 0;
            if (volatile uint8_t *rx = item(static_cast<uint16_t>(fuxi_audio_binding()->platform.glink_rx_item), &size)) {
                qcom_glink_set_rx(&gOwner.glink, rx, size);
                gOwner.glink_rx_present = 1;
                gOwner.glink_rx_size = size;
            }
        }
        (void)qcom_glink_poll(&gOwner.glink);
    }
}

/* ---- owner hooks ---- */
int hookFirmware(void *)
{
    int rc = openImage(gOwner.dtb, "modem:adsp_dtb", 12, 13);
    if (rc) return rc;
    rc = openImage(gOwner.adsp, "modem:adsp", 14, 15);
    if (rc) {
        releaseImage(gOwner.dtb);
        return rc;
    }
    setStage(QcomAdspStFirmware);
    return 0;
}

int hookMapCarveout(void *, uint64_t, uint64_t)
{
    const FuxiAdspPlatformBinding &p = fuxi_audio_binding()->platform;
    if (!mapWindow(gOwner.smem_window, p.smem_base, p.smem_size) ||
        !mapWindow(gOwner.mutex_window, p.tcsr_mutex, 0x1000) ||
        !mapWindow(gOwner.ipcc_window, p.ipcc_base, 0x1000) ||
        !mapWindow(gOwner.aoss_window, p.aoss_base, p.aoss_size) ||
        !mapWindow(gOwner.rsc_window, p.rsc_base, p.rsc_size))
        return -12;
    gOwner.smem = {gOwner.smem_window.va, static_cast<uint32_t>(p.smem_size)};
    if (qcom_smem_partition(&gOwner.smem, QCOM_SMEM_HOST_APPS, static_cast<uint16_t>(p.remote_pid), &gOwner.part))
        return -2;
    gOwner.smem_ready = true;
    gOwner.rsc_id0 = 0;
    gOwner.rsc_id1 = 0;
    gOwner.rsc_id2 = rd(gOwner.rsc_window, 0x00000);
    /* DRV2's ID register reads zero on this board even though the DTB gives
     * drv@2/tcs-offset 0xd00.  SM8550's RSC is the major-3 layout; force that
     * when the register is unreadable instead of silently using v2 offsets. */
    gOwner.rsc_drv_id = gOwner.rsc_id2 ? gOwner.rsc_id2 : (3u << 16);
    gOwner.layout = qcom_rsc_layout(gOwner.rsc_drv_id);
    setStage(QcomAdspStMapped);
    return 0;
}

int hookSmp2p(void *)
{
    const int rc = allocItem(static_cast<uint16_t>(fuxi_audio_binding()->platform.smp2p_out_item), QCOM_SMP2P_ITEM_BYTES);
    if (rc) return rc;
    /* Do not pre-create item 443: it is the DSP's outbound item.  Presence is
     * therefore evidence that the DSP itself executed its SMP2P init. */
    volatile uint8_t *out = item(static_cast<uint16_t>(fuxi_audio_binding()->platform.smp2p_out_item), nullptr);
    if (!out) return -2;
    qcom_smp2p_init_out(&gOwner.smp2p, reinterpret_cast<volatile uint32_t *>(out), 0,
                        static_cast<uint16_t>(fuxi_audio_binding()->platform.remote_pid));
    gOwner.stop_entry = static_cast<uint32_t>(qcom_smp2p_add_entry(&gOwner.smp2p, fuxi_audio_binding()->adsp.state_entry));
    if (static_cast<int32_t>(gOwner.stop_entry) < 0) return -22;
    setStage(QcomAdspStSmp2p);
    return 0;
}

int hookIpcc(void *)
{
    if (!armcpu_spi_attach) return -78;
    const FuxiAdspPlatformBinding &p = fuxi_audio_binding()->platform;
    int rc = 0;
    for (unsigned attempt = 0;; ++attempt) {
        rc = armcpu_spi_attach(kSpiIrqBase + p.ipcc_spi, false, ipccIrq, nullptr, nullptr, nullptr);
        if (rc != kIOReturnNotReady || attempt == 240) break;
        IOSleep(250);
    }
    if (rc) return rc;
    if (armcpu_spi_attach(kSpiIrqBase + p.rsc_spi, false, rscIrq, nullptr, nullptr, nullptr)) gOwner.rsc_irq_errors |= 0x80u;
    else gOwner.rsc_ready = true;
    const uint32_t enable[3] = {
        qcom_ipcc_id(QCOM_IPCC_CLIENT_LPASS, QCOM_IPCC_SIGNAL_SMP2P),
        qcom_ipcc_id(QCOM_IPCC_CLIENT_LPASS, QCOM_IPCC_SIGNAL_GLINK_QMP),
        qcom_ipcc_id(QCOM_IPCC_CLIENT_AOP, QCOM_IPCC_SIGNAL_GLINK_QMP),
    };
    for (uint32_t id : enable) wr(gOwner.ipcc_window, QCOM_IPCC_RECV_ENABLE, id);
    gOwner.ipcc_ready = true;
    /* Sheng rings SMP2P once after enabling the doorbells so a booting DSP
     * sees the outbound item.  It was missing from the first Fuxi rounds. */
    kickSmp2p();
    setStage(QcomAdspStDoorbells);
    return 0;
}

int hookQmp(void *, const char *message)
{
    const FuxiAdspPlatformBinding &p = fuxi_audio_binding()->platform;
    qcom_qmp_init(&gOwner.qmp, reinterpret_cast<volatile uint32_t *>(gOwner.aoss_window.va), p.aoss_size, kickAop, nullptr);
    for (unsigned phase = 0; phase < 4; ++phase) {
        if (qcom_qmp_step(&gOwner.qmp) == QcomQmpOpen) break;
        if (gOwner.qmp.state < 0) break;
        uint64_t deadline = 0;
        clock_interval_to_deadline(1000, kMillisecondScale, &deadline);
        while (gOwner.qmp.state != QcomQmpOpen && gOwner.qmp.state >= 0 && mach_absolute_time() < deadline)
            serviceEvents(waitEvents(10) | kEvAop);
    }
    native_fuxi_adsp_stats[29] = static_cast<uint32_t>(gOwner.qmp.state);
    if (gOwner.qmp.state != QcomQmpOpen) return gOwner.qmp.state < 0 ? gOwner.qmp.state : -60;
    gOwner.qmp_pre_state = static_cast<uint32_t>(gOwner.qmp.state);
    gOwner.order_flags |= 1u;
    /* The AOP must see the LPASS rails before it is asked to release the DSP.
     * Do not send load_state yet: the first Fuxi rounds sent it before
     * AUTH_AND_RESET, and the AOP then never released the DSP.  The one
     * request is sent by hookPas after the reset. */
    if (voteRails()) return -20;
    gOwner.qmp_pre_state = static_cast<uint32_t>(gOwner.qmp.state);
    /* With the RSC v3 layout the rail vote now completes; retry the normal
     * pre-reset load_state request as well as the post-reset one. */
    if (qcom_qmp_send(&gOwner.qmp, message) != QcomQmpSending) return -60;
    uint64_t deadline = 0;
    clock_interval_to_deadline(1000, kMillisecondScale, &deadline);
    while (gOwner.qmp.state == QcomQmpSending && mach_absolute_time() < deadline)
        serviceEvents(waitEvents(10) | kEvAop);
    if (gOwner.qmp.state != QcomQmpOpen) {
        qcom_qmp_abandon(&gOwner.qmp);
        return -60;
    }
    gOwner.qmp_response = gOwner.qmp.ram[gOwner.qmp.box / 4];
    gOwner.order_flags |= 2u;
    setStage(QcomAdspStLoadState);
    return 0;
}

int hookPas(void *)
{
    const FuxiAdspPlatformBinding &p = fuxi_audio_binding()->platform;
    /* Clear any state the bootloader or a previous attempt left behind; a
     * negative result only means that PAS was not running. */
    (void)secureCall(QCOM_SCM_PAS_SHUTDOWN, qcom_scm_args(1, 0, 0, 0), p.pas_adsp, 0, 0, 0);
    (void)secureCall(QCOM_SCM_PAS_SHUTDOWN, qcom_scm_args(1, 0, 0, 0), p.pas_dtb, 0, 0, 0);
    int rc = voteRails();
    if (rc) return rc;
    setStage(QcomAdspStRails);

    /* DTB first, as Sheng does.  The first device rounds showed that the
     * ADSP-only order returns SCM EBUSY, so the DTB hand-off is the gate.
     * PAS 36 (the candidate) is restored; PAS 1 returned SCM 1, so the
     * ID is not interchangeable. */
    rc = authenticateHeader(gOwner.dtb, p.pas_dtb, p.dtb_carveout_base, 0);
    gOwner.fail_scm_x0 = gOwner.scm_x0;
    gOwner.fail_scm_x1 = gOwner.scm_x1;
    if (!rc) rc = loadSegments(gOwner.dtb, p.dtb_carveout_base, p.dtb_carveout_size);
    if (!rc) {
        const ScmResult auth = secureCall(QCOM_SCM_PAS_AUTH_AND_RESET, qcom_scm_args(1, 0, 0, 0), p.pas_dtb, 0, 0, 0);
        noteScm(2, scmOk(auth));
        if (!scmOk(auth)) rc = -3;
    }
    if (rc) {
        shutdownDsp(true, false);
        (void)voteRails();
        return rc;
    }
    gOwner.dtb.reset = true;
    gOwner.dtb_auth_cookie = gOwner.scm_x0;
    setStage(QcomAdspStDtb);
    /* The DTB metadata and file are no longer needed once AUTH_AND_RESET
     * accepted them; releasing here leaves room for the bigger ADSP image. */
    releaseImage(gOwner.dtb);

    rc = authenticateHeader(gOwner.adsp, p.pas_adsp, fuxi_audio_binding()->adsp.carveout_base, 3);
    if (!rc) rc = loadSegments(gOwner.adsp, fuxi_audio_binding()->adsp.carveout_base, fuxi_audio_binding()->adsp.carveout_size);
    if (rc) {
        shutdownDsp(true, false);
        (void)voteRails();
        return rc;
    }
    setStage(QcomAdspStLoaded);
    const ScmResult adsp_auth = secureCall(QCOM_SCM_PAS_AUTH_AND_RESET, qcom_scm_args(1, 0, 0, 0), p.pas_adsp, 0, 0, 0);
    noteScm(5, scmOk(adsp_auth));
    if (!scmOk(adsp_auth)) {
        shutdownDsp(true, true);
        (void)voteRails();
        return -3;
    }
    gOwner.adsp.reset = true;
    gOwner.auth_cookie = gOwner.scm_x0;
    gOwner.adsp_auth_cookie = gOwner.scm_x0;
    gOwner.order_flags |= 4u;
    kickSmp2p();
    /* Ask the AOP to release the DSP only after AUTH_AND_RESET.  The earlier
     * request in hookQmp may arrive before the image is authenticated; this
     * second, bounded request is the one that starts the freshly reset DSP. */
    if (qcom_qmp_send(&gOwner.qmp, fuxi_audio_binding()->adsp.load_state_message) != QcomQmpSending) {
        shutdownDsp(true, true);
        return -60;
    }
    uint64_t deadline = 0;
    clock_interval_to_deadline(1000, kMillisecondScale, &deadline);
    while (gOwner.qmp.state == QcomQmpSending && mach_absolute_time() < deadline)
        serviceEvents(waitEvents(10) | kEvAop);
    if (gOwner.qmp.state != QcomQmpOpen) {
        qcom_qmp_abandon(&gOwner.qmp);
        shutdownDsp(true, true);
        return -60;
    }
    gOwner.qmp_post_state = static_cast<uint32_t>(gOwner.qmp.state);
    gOwner.qmp_response = gOwner.qmp.ram[gOwner.qmp.box / 4];
    gOwner.order_flags |= 8u;
    setStage(QcomAdspStStarted);
    return 0;
}

int hookGlink(void *)
{
    const FuxiAdspPlatformBinding &p = fuxi_audio_binding()->platform;
    int rc = allocItem(static_cast<uint16_t>(p.glink_desc_item), 32);
    if (!rc) rc = allocItem(static_cast<uint16_t>(p.glink_tx_item), p.glink_fifo_bytes);
    if (rc) return rc;
    uint32_t desc_size = 0, tx_size = 0;
    volatile uint8_t *desc = item(static_cast<uint16_t>(p.glink_desc_item), &desc_size);
    volatile uint8_t *tx = item(static_cast<uint16_t>(p.glink_tx_item), &tx_size);
    if (!desc || !tx || desc_size < 32 || tx_size < p.glink_fifo_bytes) return -2;
    gOwner.glink_desc_present = 1;
    gOwner.glink_tx_present = 1;
    gOwner.glink_desc_size = desc_size;
    gOwner.glink_tx_size = tx_size;
    volatile uint32_t *words = reinterpret_cast<volatile uint32_t *>(desc);
    const QcomGlinkFifo tx_fifo = {&words[1], &words[0], tx, tx_size};
    const QcomGlinkFifo rx_fifo = {&words[3], &words[2], nullptr, 0};
    qcom_glink_init(&gOwner.glink, tx_fifo, rx_fifo, kickGlink, nullptr);
    const QcomGlinkClient client = {dspState, dspReceive};
    gOwner.channel = qcom_glink_channel(&gOwner.glink, p.glink_channel, p.glink_intent_size,
                                        p.glink_intent_count, gOwner.storage, &client, nullptr, 0);
    if (!gOwner.channel) return -12;
    gOwner.gLink_started = true;
    setStage(QcomAdspStGlink);
    return 0;
}

void rollbackOwner(void *, uint32_t)
{
    shutdownDsp(true, true);
    (void)voteRails();
    releaseImage(gOwner.dtb);
    releaseImage(gOwner.adsp);
    unmapWindow(gOwner.smem_window);
    unmapWindow(gOwner.mutex_window);
    unmapWindow(gOwner.ipcc_window);
    unmapWindow(gOwner.aoss_window);
    unmapWindow(gOwner.rsc_window);
    gOwner.smem_ready = false;
}

/* ---- readiness and link, through the existing QcomAdsp ladder ---- */
bool ladderDone(void *context)
{
    const QcomAdsp *ladder = static_cast<const QcomAdsp *>(context);
    return ladder->state == QcomAdspUp || ladder->state == QcomAdspFailed;
}

int waitReady()
{
    gOwner.ladder.state = QcomAdspReady;
    gOwner.ladder.stage = QcomAdspStStarted;
    gOwner.ladder.waits = 0;
    gOwner.ladder.failed_stage = 0;
    while (!ladderDone(&gOwner.ladder)) {
        serviceEvents(waitEvents(10) | kEvSmp2p);
        const int rc = qcom_adsp_step(&gOwner.ladder);
        gOwner.stage = gOwner.ladder.stage > gOwner.stage ? gOwner.ladder.stage : gOwner.stage;
        syncStats();
        if (gOwner.ladder.state == QcomAdspFailed) {
            gOwner.last_error = rc;
            gOwner.failed_stage = gOwner.ladder.failed_stage;
            readCrashReason();
            return rc;
        }
        if (gOwner.ladder.stage >= QcomAdspStReady) {
            gOwner.ladder.state = QcomAdspLink;
            gOwner.ladder.waits = 0;
            setStage(QcomAdspStReady);
            return 0;
        }
    }
    return gOwner.last_error ? gOwner.last_error : -60;
}

int waitGlink()
{
    gOwner.ladder.state = QcomAdspLink;
    gOwner.ladder.waits = 0;
    gOwner.ladder.link = &gOwner.glink;
    gOwner.ladder.channel = gOwner.channel;
    while (!ladderDone(&gOwner.ladder)) {
        serviceEvents(waitEvents(10) | kEvGlink);
        const int rc = qcom_adsp_step(&gOwner.ladder);
        gOwner.stage = gOwner.ladder.stage > gOwner.stage ? gOwner.ladder.stage : gOwner.stage;
        syncStats();
        if (gOwner.ladder.state == QcomAdspFailed) {
            gOwner.last_error = rc;
            gOwner.failed_stage = gOwner.ladder.failed_stage;
            return rc;
        }
        if (gOwner.ladder.state == QcomAdspUp) {
            gOwner.glink_up = true;
            setStage(QcomAdspStChannel);
            return 0;
        }
    }
    return gOwner.last_error ? gOwner.last_error : -60;
}

int graphWaitReply(uint64_t deadline)
{
    while (nowMs() < deadline) {
        serviceEvents(waitEvents(20) | kEvSend | kEvGlink | kEvGpr);
        if (!gOwner.glink_up || gOwner.glink.broken) {
            fuxi_graph_wait_cancel(&gOwner.graph_wait);
            return -ENOTCONN;
        }
        if (!gOwner.graph_wait.active) return 0;
        if (fuxi_graph_wait_timeout(&gOwner.graph_wait, deadline, nowMs())) return -ETIMEDOUT;
    }
    fuxi_graph_wait_cancel(&gOwner.graph_wait);
    return -ETIMEDOUT;
}

int graphSetCfg(uint32_t source_port, uint32_t token, uint8_t kind,
                uint32_t bytes, FuxiGraphReply &reply)
{
    if (!bytes) return -EINVAL;
    reply = {};
    reply.status = 0xffffffffu;
    const FuxiGraphExpect expect = {source_port, token, QCOM_AR_OP_SET_CFG, 0ull, 0u};
    fuxi_graph_wait_begin(&gOwner.graph_wait, &expect, kind);
    syncStats();
    const int sent = qcom_glink_send(&gOwner.glink, gOwner.channel, gOwner.graph_packet, bytes);
    if (sent != QcomGlinkOk) {
        fuxi_graph_wait_cancel(&gOwner.graph_wait);
        return sent;
    }
    const int rc = graphWaitReply(nowMs() + 1000u);
    if (rc) return rc;
    return reply.token == token && reply.status == 0 ? 0 : -EIO;
}

/* Sends one prepare/start/stop/close command for a single subgraph and
 * waits for the framework's answer.
 *
 * The reference client does exactly this: the command names one subgraph
 * in the SG_LIST parameter, and the framework answers with a BASIC_RESULT
 * that repeats the command and carries the status. */
static int graphCommandOp(uint32_t source_port, uint32_t token, uint32_t opcode,
                           uint32_t subgraph_id, uint8_t kind, FuxiGraphReply &reply)
{
    gOwner.graph_req_size = qcom_ar_graph_command(gOwner.graph_packet,
        sizeof gOwner.graph_packet, source_port, token, kArApmIid, opcode, subgraph_id);
    if (!gOwner.graph_req_size) {
        reply = {};
        reply.status = 0xffffffffu;
        return -EINVAL;
    }
    return graphWaitOp(source_port, token, opcode, kind, reply);
}

/* P4: bring the two CS35L41 amplifiers up to MUTED_READY, using the board binding. */
static uint32_t geniRead(void *ctx, uint32_t reg)
{
    return *reinterpret_cast<volatile uint32_t *>(reinterpret_cast<uintptr_t>(ctx) + reg);
}
static void __attribute__((unused)) geniWrite(void *ctx, uint32_t reg, uint32_t value)
{
    *reinterpret_cast<volatile uint32_t *>(reinterpret_cast<uintptr_t>(ctx) + reg) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Brings the engine back to idle the way the GENI I2C driver does it
 * (geni_i2c_stop_xfer): a transfer that stops answering is cancelled, then
 * aborted, and every status bit is cleared.  Without this a NACK leaves the
 * command active and the next transfer overruns it. */
static void ampRecoverBus(GeniI2c *g)
{
    const uint32_t before = geniRead(g->io.ctx, GENI_M_IRQ_STATUS);
    geniWrite(g->io.ctx, GENI_M_CMD_CTRL, GENI_CMD_CTRL_CANCEL);
    for (unsigned i = 0; i < 500u; ++i) {
        if (geniRead(g->io.ctx, GENI_M_IRQ_STATUS) & GENI_M_CMD_CANCEL) break;
        IODelay(20);
    }
    if (!(geniRead(g->io.ctx, GENI_M_IRQ_STATUS) & GENI_M_CMD_CANCEL)) {
        geniWrite(g->io.ctx, GENI_M_CMD_CTRL, GENI_CMD_CTRL_ABORT);
        for (unsigned i = 0; i < 500u; ++i) {
            if (geniRead(g->io.ctx, GENI_M_IRQ_STATUS) & GENI_M_CMD_ABORT) break;
            IODelay(20);
        }
    }
    geniWrite(g->io.ctx, GENI_M_IRQ_CLEAR, 0xffffffffu);
    g->phase = GeniIdle;
    printf("fuxi-amp: bus recovered, M_IRQ was %08x now %08x\n", before,
           geniRead(g->io.ctx, GENI_M_IRQ_STATUS));
}

static int __attribute__((unused)) ampTransfer(void *ctx, const uint8_t *bytes, uint32_t length,
                       uint32_t read_length, uint32_t *value)
{
    GeniI2c *g = reinterpret_cast<GeniI2c *>(ctx);
    int rc = read_length ? geni_i2c_start_read_bytes(g, bytes, length, read_length)
                         : geni_i2c_start_write_bytes(g, bytes, length);
    if (rc) return -1;
    for (unsigned stage = 0; stage < 3u; ++stage) {
        for (unsigned i = 0; i < 1000u; ++i) {
            rc = geni_i2c_poll(g);
            if (rc == GeniDone) {
                /* The wire is big-endian: the step runner wants the register
                 * value, not the raw FIFO word. */
                if (read_length && value) *value = cs35_wire_value(g->rx);
                return 0;
            }
            if (rc == GeniFailed) { ampRecoverBus(g); return -1; }
            IODelay(10);
        }
        if (geni_i2c_timeout(g) != GeniPending) break;
    }
    ampRecoverBus(g);
    return -1;
}

/* The amplifier bring-up, in stages, so a run says where it stopped.
 *
 * The stages and what each one does:
 *   1  map the QUP2 SE0 window (16 KiB) and the five GCC pages, and read
 *      the clock state.  Nothing is written.
 *   2  as 1, plus the clock votes: the GPLL0, the QUP wrapper's core and AHB
 *      clocks and the QUP2 SE0 branch.
 *   3  as 2, plus the pins: the I2C pins on the QUP2 SE0 function with a
 *      pull-up, and the two amplifier resets driven low and then released.
 *   4  as 3, plus the engine: the DFS level that gives 19.2 MHz, the clock
 *      and the SCL counters, and then geni_i2c_prepare.
 *   5  as 4, plus the amplifiers' identity: the init program of each
 *      amplifier, which leaves it powered down.
 *
 * The amplifier is never unmuted here: the P4 goal is the identity and the
 * PLL lock, and an unmuted amplifier makes noise. */
static uint32_t gAmpStage;
static uint32_t gAmpLevel;
static uint32_t gAmpGpll0Mode, gAmpGpll0Enable, gAmpVote1, gAmpVote2;
static uint32_t gAmpHalts[FUXI_AMP_HALTS_COUNT];
static uint32_t gAmpSeProto, gAmpSeClock, gAmpSeCounters, gAmpSeConfig;
static uint32_t gAmpDfsLevel;
static uint64_t gAmpDfsHz;
static uint32_t gAmpRcgCmd, gAmpRcgDfsr;
static uint32_t gAmpDeviceId[CS35_AMPS], gAmpRevision[CS35_AMPS];
static uint32_t gAmpEint1[CS35_AMPS], gAmpEint2[CS35_AMPS];
/* The pins the bring-up looks at: the two I2C lines, the two resets, the two
 * interrupt lines, and the four secondary MI2S lines that carry the
 * amplifier link. */
static uint32_t gAmpPinBefore[10][2];
static uint32_t gAmpPinAfter[10][2];
static uint32_t gAmpFailures;
/* The engine's window, kept across the graph start so the amplifiers' PLL
 * lock can be read once the serial clock is running. */
static Window gAmpI2cWindow;

/* The GENI engine's own registers, beyond the ones GeniI2c.h names. */
constexpr uint32_t kGeniForceDefault = 0x20;      /* GeniSpi.h: GENI_FORCE_DEFAULT_REG */
constexpr uint32_t kGeniOutputCtrl = 0x24;        /* GeniSpi.h: GENI_OUTPUT_CTRL */
constexpr uint32_t kGeniCgcCtrl = 0x28;           /* GeniSpi.h: SE_GENI_CGC_CTRL */
constexpr uint32_t kGeniClkSel = 0x7c;            /* GeniSpi.h: SE_GENI_CLK_SEL */
constexpr uint32_t kGeniDmaGeneralCfg = 0xe30;    /* GeniSpi.h: SE_DMA_GENERAL_CFG */
constexpr uint32_t kGeniGsiEventEn = 0xe18;       /* GeniSpi.h: SE_GSI_EVENT_EN */
constexpr uint32_t kGeniDefaultCgc = 0x7fu;
constexpr uint32_t kGeniDefaultIoOut = 0x7fu;
constexpr uint32_t kGeniDmaCgcOn = 0xfu;

/* The GCC page a register lives in, and the offset inside it. */
static uint32_t gccRead(const Window &page, uint32_t offset)
{
    return rd(page, offset & 0xfffu);
}

static void gccWrite(const Window &page, uint32_t offset, uint32_t value)
{
    wr(page, offset & 0xfffu, value);
}

/* The branch halt register of a QUP branch, read from the page it lives in. */
static uint32_t haltRead(const Window &low, const Window &p18, const Window &p1e,
                         const Window &p23, uint32_t reg)
{
    switch (reg >> 12) {
    case 0x00: return gccRead(low, reg);
    case 0x18: return gccRead(p18, reg);
    case 0x1e: return gccRead(p1e, reg);
    case 0x23: return gccRead(p23, reg);
    default: return 0;
    }
}

/* Reads the DFS level's frequency from the RCG's perf registers and picks the
 * level that is exactly the I2C source clock (19.2 MHz, the TCXO).
 * The register layout is Linux's clk-rcg2.c: the perf registers sit 0x1c,
 * 0x5c and 0x9c past the RCG's command register, one set per level. */
static unsigned findDfsLevel(const Window &page, uint32_t rcg, uint64_t target,
                             uint64_t *out_hz)
{
    const uint32_t base = rcg & 0xfffu;
    for (unsigned level = 0; level < TB_DFS_LEVELS; ++level) {
        const uint32_t cfg = gccRead(page, base + TB_RCG_PERF_CFG(level));
        const uint32_t m = gccRead(page, base + TB_RCG_PERF_M(level));
        const uint32_t n = gccRead(page, base + TB_RCG_PERF_N(level));
        const uint64_t hz = tb_dfs_level_hz(cfg, m, n);
        if (hz == target) {if (out_hz) *out_hz = hz; return level;}
    }
    return 0xffu;
}

/* The amplifier identity: runs the init program and reads the device id, the
 * revision and the two interrupt status words back. */
static int runAmpProgram(unsigned amp, const Cs35Step *steps, unsigned count,
                         Cs35Run *run)
{
    const uint8_t address = fuxi_cs35_address(amp);
    gOwner.amp_geni[amp].address = address;
    cs35_run_start(run, steps, count);
    for (unsigned guard = 0; guard < 512u; ++guard) {
        Cs35Action action;
        const int what = cs35_run_next(run, &action);
        if (what == Cs35Finished) return 0;
        if (what == Cs35Wait) {IOSleep(action.delay_ms); continue;}
        uint32_t value = 0;
        if (ampTransfer(&gOwner.amp_geni[amp], action.bytes, action.length,
                        action.read_length, &value))
            return -1;
        cs35_run_result(run, value);
    }
    return -1;
}

/* Reads the two amplifiers' identity over the I2C bus. */
static int probeAmpIdentity(GeniI2c *bus)
{
    Cs35Step steps[FUXI_CS35_MAX_STEPS];
    unsigned found = 0;
    for (unsigned amp = 0; amp < CS35_AMPS; ++amp) {
        gOwner.amp_geni[amp] = *bus;
        gOwner.amp_geni[amp].address = fuxi_cs35_address(amp);
        const unsigned count = fuxi_cs35_init_program(steps, fuxi_cs35_slot(amp));
        Cs35Run run;
        if (runAmpProgram(amp, steps, count, &run)) {++gAmpFailures; continue;}
        gAmpDeviceId[amp] = run.reads[0];
        gAmpRevision[amp] = run.reads[1];
        gAmpEint1[amp] = run.reads[2];    /* IRQ1_STATUS4 */
        gAmpEint2[amp] = run.reads[3];    /* IRQ1_STATUS3 */
        if (gAmpDeviceId[amp] == CS35_CHIP_ID) found |= 1u << amp;
    }
    gOwner.amp_found = found;
    return found ? 0 : -1;
}

int probeOwnedAmp()
{
    /* The staged bring-up of the two CS35L41 amplifiers.  The boot argument
     * fuxi-amp=<n> says how far to go: 1 reads the clock state, 2 votes the
     * clocks, 3 sets the pins and the resets, 4 brings the I2C engine up, and
     * 5 reads the amplifiers' identity. */
    gOwner.amp_prepare[0] = 0xAAu;   /* probe entered */
    syncStats();
    const FuxiAudioBinding *b = fuxi_audio_binding();
    if (!b) { gOwner.amp_prepare[0] = 0xEEu; return -EINVAL; }
    Window i2c = {};
    /* The window is 16 KiB: the engine's interrupt and clock registers reach
     * 0xe30, and a smaller window turns those writes into an abort. */
    if (!mapWindow(i2c, FUXI_AMP_I2C_BASE, FUXI_AMP_I2C_SIZE)) {
        gOwner.amp_prepare[0] = 0xEEu;   /* mapping failed */
        syncStats();
        return -ENOMEM;
    }
    gOwner.amp_prepare[0] = 0xCCu;   /* mapping ok */
    gAmpStage = FUXI_AMP_STAGE_MAP;
    /* The five GCC pages the bring-up touches. */
    Window gccLow = {}, gcc18 = {}, gcc1e = {}, gcc23 = {}, gccVote = {};
    const bool gccMapped = mapWindow(gccLow, FUXI_AMP_GCC_BASE + FUXI_AMP_GCC_PAGE_LOW, FUXI_AMP_GCC_PAGE_SIZE) &&
                           mapWindow(gcc18, FUXI_AMP_GCC_BASE + FUXI_AMP_GCC_PAGE_18, FUXI_AMP_GCC_PAGE_SIZE) &&
                           mapWindow(gcc1e, FUXI_AMP_GCC_BASE + FUXI_AMP_GCC_PAGE_1E, FUXI_AMP_GCC_PAGE_SIZE) &&
                           mapWindow(gcc23, FUXI_AMP_GCC_BASE + FUXI_AMP_GCC_PAGE_23, FUXI_AMP_GCC_PAGE_SIZE) &&
                           mapWindow(gccVote, FUXI_AMP_GCC_BASE + FUXI_AMP_GCC_PAGE_52, FUXI_AMP_GCC_PAGE_SIZE);
    if (!gccMapped) {
        unmapWindow(gccLow); unmapWindow(gcc18); unmapWindow(gcc1e);
        unmapWindow(gcc23); unmapWindow(gccVote);
        unmapWindow(i2c);
        gOwner.amp_prepare[0] = 0xEEu;
        syncStats();
        return -ENOMEM;
    }
    /* The clock state before anything is written: the GPLL0's mode (bit 31 is
     * the lock), its enable, the two vote registers and the branch halts. */
    gAmpGpll0Mode = gccRead(gccLow, FUXI_AMP_GPLL0_MODE);
    gAmpGpll0Enable = gccRead(gccVote, FUXI_AMP_GPLL0_ENABLE);
    gAmpVote1 = gccRead(gccVote, FUXI_AMP_VOTE1);
    gAmpVote2 = gccRead(gccVote, FUXI_AMP_VOTE2);
    for (unsigned i = 0; i < FUXI_AMP_HALTS_COUNT; ++i)
        gAmpHalts[i] = haltRead(gccLow, gcc18, gcc1e, gcc23, fuxi_amp_halt_registers[i]);
    gAmpStage = FUXI_AMP_STAGE_GCC_READ;
    printf("fuxi-amp: level %u stage %u gpll0 %08x enable %08x vote1 %08x vote2 %08x\n",
           gAmpLevel, gAmpStage, gAmpGpll0Mode, gAmpGpll0Enable, gAmpVote1, gAmpVote2);
    printf("fuxi-amp: halts");
    for (unsigned i = 0; i < FUXI_AMP_HALTS_COUNT; ++i) printf(" %08x", gAmpHalts[i]);
    printf("\n");
    printf("fuxi-amp: branches");
    for (unsigned i = 0; i < FUXI_AMP_HALTS_COUNT; ++i)
        printf(" %s:%08x%s", fuxi_amp_halt_names[i], gAmpHalts[i],
               fuxi_amp_branch_ok(gAmpHalts[i], fuxi_amp_halt_hwcg[i]) ? "" : "(off)");
    printf("\n");

    if (gAmpLevel >= 2u) {
        /* The votes: the GPLL0 first, then the five QUP wrapper 2 branches
         * that carry the transfer (the core, the two AHB clocks, the core 2x
         * clock and the SE0 branch).  All five live in the second vote
         * register; the first one holds the wrapper 1 branches, which the
         * touch driver owns. */
        gccWrite(gccVote, FUXI_AMP_GPLL0_ENABLE, gAmpGpll0Enable | 1u);
        for (unsigned i = 0; i < 10000u && !(gccRead(gccLow, FUXI_AMP_GPLL0_MODE) & (1u << 31)); ++i)
            IODelay(10);
        gAmpGpll0Mode = gccRead(gccLow, FUXI_AMP_GPLL0_MODE);
        gccWrite(gccVote, FUXI_AMP_VOTE2, gAmpVote2 | FUXI_AMP_VOTE2_BITS);
        /* Wait for the branches: a branch whose clock is off reads bit 31 of
         * its CBCR, and the FSM status field (bits 30:28) reads 2 when the
         * branch is on (Linux clk-branch.c).  The two AHB branches are gated
         * by the hardware, and Linux never waits for those: clk-branch.c's
         * clk_branch_in_hwcg_mode returns early when the branch's hwcg bit
         * is set, so their bit 31 does not mean the clock is off. */
        bool running = false;
        for (unsigned i = 0; i < 2000u && !running; ++i) {
            running = true;
            for (unsigned h = 0; h < FUXI_AMP_HALTS_COUNT; ++h) {
                const uint32_t v = haltRead(gccLow, gcc18, gcc1e, gcc23, fuxi_amp_halt_registers[h]);
                if (!fuxi_amp_branch_ok(v, fuxi_amp_halt_hwcg[h])) running = false;
            }
            if (!running) IODelay(10);
        }
        gAmpVote1 = gccRead(gccVote, FUXI_AMP_VOTE1);
        gAmpVote2 = gccRead(gccVote, FUXI_AMP_VOTE2);
        for (unsigned i = 0; i < FUXI_AMP_HALTS_COUNT; ++i)
            gAmpHalts[i] = haltRead(gccLow, gcc18, gcc1e, gcc23, fuxi_amp_halt_registers[i]);
        gAmpStage = FUXI_AMP_STAGE_GCC_VOTE;
        printf("fuxi-amp: level %u stage %u gpll0 %08x vote1 %08x vote2 %08x running %u\n",
               gAmpLevel, gAmpStage, gAmpGpll0Mode, gAmpVote1, gAmpVote2, running ? 1u : 0u);
        printf("fuxi-amp: halts");
        for (unsigned i = 0; i < FUXI_AMP_HALTS_COUNT; ++i) printf(" %08x", gAmpHalts[i]);
        printf("\n");
        printf("fuxi-amp: branches");
        for (unsigned i = 0; i < FUXI_AMP_HALTS_COUNT; ++i)
            printf(" %s:%08x%s", fuxi_amp_halt_names[i], gAmpHalts[i],
                   fuxi_amp_branch_ok(gAmpHalts[i], fuxi_amp_halt_hwcg[i]) ? "" : "(off)");
        printf("\n");
        if (!running) {
            printf("fuxi-amp: the QUP2 clocks did not come up, stopping here\n");
            gOwner.amp_prepare[0] = 0xDDu;
            syncStats();
            unmapWindow(i2c);
            unmapWindow(gccLow); unmapWindow(gcc18); unmapWindow(gcc1e);
            unmapWindow(gcc23); unmapWindow(gccVote);
            return -EIO;
        }
    }

    if (gAmpLevel >= 3u) {
        /* The pins.  Each pin's control and input/output register are read
         * before and after the write, so the log shows what the boot loader
         * left and what this probe did. */
        const unsigned pins[10] = {FUXI_AMP_I2C_SDA_GPIO, FUXI_AMP_I2C_SCL_GPIO,
                                   FUXI_AMP_RESET_T_GPIO, FUXI_AMP_RESET_B_GPIO,
                                   FUXI_AMP_IRQ_T_GPIO, FUXI_AMP_IRQ_B_GPIO,
                                   FUXI_AMP_MI2S_SCK_GPIO, FUXI_AMP_MI2S_DATA0_GPIO,
                                   FUXI_AMP_MI2S_WS_GPIO, FUXI_AMP_MI2S_DATA1_GPIO};
        for (unsigned i = 0; i < 10; ++i) {
            Window pin = {};
            if (!mapWindow(pin, fuxi_amp_tlmm_ctl(pins[i]), FUXI_AMP_GPIO_STRIDE)) continue;
            gAmpPinBefore[i][0] = rd(pin, 0);
            gAmpPinBefore[i][1] = rd(pin, 4);
            unmapWindow(pin);
        }
        printf("fuxi-amp: pins before");
        for (unsigned i = 0; i < 10; ++i) printf(" g%u:%08x/%08x", pins[i], gAmpPinBefore[i][0], gAmpPinBefore[i][1]);
        printf("\n");
        /* The I2C pins on the QUP2 SE0 function with a pull-up; the amplifier
         * resets as outputs. */
        for (unsigned i = 0; i < 2; ++i) {
            Window pin = {};
            if (!mapWindow(pin, fuxi_amp_tlmm_ctl(pins[i]), FUXI_AMP_GPIO_STRIDE)) continue;
            wr(pin, 0, fuxi_amp_i2c_pin_cfg());
            unmapWindow(pin);
        }
        for (unsigned i = 2; i < 4; ++i) {
            Window pin = {};
            if (!mapWindow(pin, fuxi_amp_tlmm_ctl(pins[i]), FUXI_AMP_GPIO_STRIDE)) continue;
            /* The reset pin is an eGPIO: Linux claims it by setting bit 12
             * of the control register (pinctrl-msm.c takes the ownership
             * when the pin has the eGPIO function, bit 11). */
            const uint32_t ctl = rd(pin, 0);
            /* The output value goes in before the output is enabled, the way
             * Linux's msm_gpio_direction_output does it, so the pin does not
             * glitch to whatever the boot loader left behind. */
            wr(pin, 4, 0);                                   /* output low: the reset asserts */
            wr(pin, 0, (ctl & ~0x1ffu) | fuxi_amp_reset_pin_cfg() |
                  FUXI_AMP_OUTPUT_ENABLE |
                  ((ctl & FUXI_AMP_EGPIO_PRESENT) ? FUXI_AMP_EGPIO_ENABLE : 0u));
            unmapWindow(pin);
        }
        IOSleep(2);
        for (unsigned i = 2; i < 4; ++i) {
            Window pin = {};
            if (!mapWindow(pin, fuxi_amp_tlmm_ctl(pins[i]), FUXI_AMP_GPIO_STRIDE)) continue;
            wr(pin, 4, FUXI_AMP_IO_OUTPUT);                 /* release: the output goes high */
            unmapWindow(pin);
        }
        IOSleep(5);
        /* The secondary MI2S (I2S1) lines that carry the amplifier link: the
         * clock, the word select and the two data lines on the I2S1
         * function at 8 mA, the way the tree's aux1_pcm_*_active nodes ask
         * for them.  Without this the amplifier's serial port has no
         * clock and no data, so even a running graph stays silent. */
        for (unsigned i = 6; i < 10; ++i) {
            Window pin = {};
            if (!mapWindow(pin, fuxi_amp_tlmm_ctl(pins[i]), FUXI_AMP_GPIO_STRIDE)) continue;
            wr(pin, 0, fuxi_amp_mi2s_pin_cfg());
            unmapWindow(pin);
        }
        for (unsigned i = 0; i < 10; ++i) {
            Window pin = {};
            if (!mapWindow(pin, fuxi_amp_tlmm_ctl(pins[i]), FUXI_AMP_GPIO_STRIDE)) continue;
            gAmpPinAfter[i][0] = rd(pin, 0);
            gAmpPinAfter[i][1] = rd(pin, 4);
            unmapWindow(pin);
        }
        printf("fuxi-amp: pins after");
        for (unsigned i = 0; i < 10; ++i) printf(" g%u:%08x/%08x", pins[i], gAmpPinAfter[i][0], gAmpPinAfter[i][1]);
        printf("\n");
        gAmpStage = FUXI_AMP_STAGE_PINS;
    }

    if (gAmpLevel >= 4u) {
        /* The engine.  The DFS level that gives 19.2 MHz comes from the
         * QUP2 SE0 branch's own clock source. */
        gAmpDfsLevel = findDfsLevel(gcc1e, FUXI_AMP_RCG_SE0, FUXI_AMP_DFS_HZ, &gAmpDfsHz);
        printf("fuxi-amp: dfs level %u at %llu Hz\n", gAmpDfsLevel,
               (unsigned long long)gAmpDfsHz);
        /* The branch's own command register: bit 31 says the root is off and
         * the DFS register at +0x14 holds the level the branch runs at (bit 0
         * the dynamic scaling is on, bits 4:1 the level).  Linux's clk-rcg2.c
         * reads the same register to work out the rate. */
        const uint32_t rcgCmd = gccRead(gcc1e, FUXI_AMP_RCG_SE0 + FUXI_AMP_RCG_CMD);
        const uint32_t rcgDfsr = gccRead(gcc1e, FUXI_AMP_RCG_SE0 + FUXI_AMP_RCG_DFSR);
        gAmpRcgCmd = rcgCmd;
        gAmpRcgDfsr = rcgDfsr;
        printf("fuxi-amp: se0 rcg cmd %08x dfsr %08x level %u dfs %u\n",
               rcgCmd, rcgDfsr, FUXI_AMP_DFSR_LEVEL(rcgDfsr),
               (rcgDfsr & FUXI_AMP_DFSR_ENABLED) ? 1u : 0u);
        /* The engine's own registers, read from the SE window. */
        const uint32_t fw = rd(i2c, GENI_FW_REVISION);
        const uint32_t clock = rd(i2c, GENI_CLK_CFG);
        const uint32_t counters = rd(i2c, GENI_SCL_COUNTERS);
        gAmpSeProto = (fw >> 8) & 0xffu;
        gAmpSeClock = clock;
        gAmpSeCounters = counters;
        printf("fuxi-amp: se fw %08x proto %u clock %08x counters %08x ifdisable %08x\n",
               fw, gAmpSeProto, clock, counters, rd(i2c, GENI_IF_DISABLE));
        if (gAmpSeProto != GENI_PROTO_I2C) {
            printf("fuxi-amp: the engine runs protocol %u, not I2C\n", gAmpSeProto);
        } else {
            /* The engine's clock gating and its output control, the same
             * writes GeniSpi.c's geni_spi_init makes before it talks to the
             * touch controller over QUP1 SE4. */
            wr(i2c, kGeniCgcCtrl, rd(i2c, kGeniCgcCtrl) | kGeniDefaultCgc);
            wr(i2c, kGeniDmaGeneralCfg, rd(i2c, kGeniDmaGeneralCfg) | kGeniDmaCgcOn);
            wr(i2c, kGeniOutputCtrl, kGeniDefaultIoOut);
            wr(i2c, kGeniForceDefault, 1u);
            wr(i2c, kGeniGsiEventEn, 0);
            /* The DFS level selects which of the branch's levels drives the
             * engine: Linux's i2c-qcom-geni.c writes the level it picked
             * into SE_GENI_CLK_SEL, and GeniSpi.c does the same for the touch
             * controller.  Only write it when the level is known: the boot
             * loader's own selection is overwritten blindly, the engine runs
             * at the wrong rate and the transfers fail. */
            if (gAmpDfsLevel != 0xffu) {
                wr(i2c, kGeniClkSel, gAmpDfsLevel);
                printf("fuxi-amp: clk sel %u\n", gAmpDfsLevel);
            } else {
                printf("fuxi-amp: no DFS level gives %llu Hz, keeping the boot loader's\n",
                       (unsigned long long)FUXI_AMP_DFS_HZ);
            }
            /* The engine's own divider and the I2C timing: 19.2 MHz in, a
             * divider of 2 and the counters 5 / 11 / 22 for 400 kHz (Linux
             * i2c-qcom-geni.c's geni_i2c_clk_map_19p2mhz). */
            wr(i2c, GENI_CLK_CFG, FUXI_AMP_CLK_CFG);
            wr(i2c, GENI_SCL_COUNTERS, FUXI_AMP_SCL_COUNTERS);
            printf("fuxi-amp: clock cfg %08x counters %08x\n",
                   rd(i2c, GENI_CLK_CFG), rd(i2c, GENI_SCL_COUNTERS));
            uint32_t config = 0;
            GeniI2c bus = {};
            const GeniIo io = {geniRead, geniWrite,
                               reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(i2c.va))};
            const int prepared = geni_i2c_prepare(&bus, &io, fuxi_cs35_address(0), &config);
            gAmpSeConfig = config;
            printf("fuxi-amp: geni_i2c_prepare %d config %08x\n", prepared, config);
            if (!prepared) {
                gOwner.amp_geni[0] = bus;
                gOwner.amp_geni[1] = bus;
                gOwner.amp_prepare[1] = 1u;
            } else {
                gOwner.amp_prepare[1] = 0u;
            }
            gAmpStage = FUXI_AMP_STAGE_BUS;
        }
    }

    if (gAmpLevel >= 5u && gOwner.amp_prepare[1]) {
        const int rc = probeAmpIdentity(&gOwner.amp_geni[0]);
        printf("fuxi-amp: identity rc %d found %x\n", rc, gOwner.amp_found);
        for (unsigned amp = 0; amp < CS35_AMPS; ++amp)
            printf("fuxi-amp: amp %u id %08x rev %08x st4 %08x st3 %08x\n",
                   amp, gAmpDeviceId[amp], gAmpRevision[amp], gAmpEint1[amp], gAmpEint2[amp]);
        gAmpStage = FUXI_AMP_STAGE_IDENTITY;
    }

    if (gAmpLevel >= 6u && gOwner.amp_found) {
        /* The power-up, in the only order that is safe: the amplifiers are
         * muted first, then powered up, and only then are they unmuted.
         *
         * The order matters because the init program leaves the digital
         * volume at 0 dB and the analog gain at 17.5 dB.  Powering an
         * amplifier up in that state makes it output at full scale the
         * instant its serial port is enabled, so the mute program runs
         * first: the digital volume to the part's mute code and the analog
         * gain to zero. */
        uint32_t locked = 0, errors = 0, powered = 0, pup = 0;
        for (unsigned amp = 0; amp < CS35_AMPS; ++amp) {
            if (!(gOwner.amp_found & (1u << amp))) continue;
            Cs35Step steps[FUXI_CS35_MAX_STEPS];
            Cs35Run run;
            /* 1: the mute. */
            const unsigned mute_count = fuxi_cs35_mute_program(steps);
            if (runAmpProgram(amp, steps, mute_count, &run)) {
                printf("fuxi-amp: amp %u mute failed\n", amp);
                continue;
            }
            /* 2: the power-up. */
            const unsigned up_count = fuxi_cs35_power_up_program(steps);
            if (runAmpProgram(amp, steps, up_count, &run)) {
                printf("fuxi-amp: amp %u power up failed\n", amp);
                continue;
            }
            powered |= 1u << amp;
            /* 3: the status.  The power-up read the interrupt status word,
             * which carries the power-up done bit. */
            const unsigned status_count = fuxi_cs35_status_program(steps);
            if (runAmpProgram(amp, steps, status_count, &run)) {
                printf("fuxi-amp: amp %u status failed\n", amp);
                continue;
            }
            gAmpEint1[amp] = run.reads[0];
            gAmpEint2[amp] = run.reads[1];
            if (run.reads[0] & CS35_PUP_DONE) pup |= 1u << amp;
            if (run.reads[1] & CS35_EINT2_PLL_LOCK) locked |= 1u << amp;
            if (run.reads[0] & CS35_EINT1_ERRORS) errors |= 1u << amp;
            printf("fuxi-amp: amp %u up st1 %08x st3 %08x pup %u pll %u\n",
                   amp, gAmpEint1[amp], gAmpEint2[amp],
                   (run.reads[0] & CS35_PUP_DONE) ? 1u : 0u,
                   (run.reads[1] & CS35_EINT2_PLL_LOCK) ? 1u : 0u);
        }
        gOwner.amp_pll_lock = locked;
        gOwner.amp_errors = errors;
        gOwner.amp_pup = pup;
        gOwner.amp_powered = powered;
        printf("fuxi-amp: powered %x pup %x pll %x errors %x\n",
               powered, pup, locked, errors);
        /* The amplifiers stay muted: the unmute happens only when the
         * graph is running and the periods are flowing, and that is a
         * separate step. */
        gAmpStage = FUXI_AMP_STAGE_DONE;
    }

    /* The published numbers: the identity of the two amplifiers and the
     * engine's state. */
    gOwner.amp_device_id[0] = gAmpDeviceId[0];
    gOwner.amp_revision[0] = gAmpRevision[0];
    gOwner.amp_eint1[0] = gAmpEint1[0];
    gOwner.amp_eint2[0] = gAmpEint2[0];
    gOwner.amp_device_id[1] = gAmpDeviceId[1];
    gOwner.amp_revision[1] = gAmpRevision[1];
    gOwner.amp_eint1[1] = gAmpEint1[1];
    gOwner.amp_eint2[1] = gAmpEint2[1];
    gOwner.amp_state[0] = gAmpStage;
    gOwner.amp_state[1] = gAmpDfsLevel;
    gOwner.amp_error[0] = gAmpFailures;
    gOwner.amp_error[1] = gOwner.amp_found;
    gOwner.amp_geni_error[0] = gAmpSeProto;
    gOwner.amp_geni_error[1] = gAmpSeConfig;
    gOwner.amp_last_error[0] = gAmpGpll0Mode;
    gOwner.amp_last_error[1] = gAmpVote2;
    /* The engine's window is kept when the amplifiers were powered up: the
     * PLL can only lock once the DSP's endpoint clock is running, so the
     * status has to be read again after the graphs are started. */
    if (gAmpLevel >= 6u && gOwner.amp_powered) {
        unmapWindow(gAmpI2cWindow);
        gAmpI2cWindow = i2c;
    } else {
        unmapWindow(i2c);
    }
    unmapWindow(gccLow); unmapWindow(gcc18); unmapWindow(gcc1e);
    unmapWindow(gcc23); unmapWindow(gccVote);
    syncStats();
    return 0;
}

/* Unmutes the amplifiers.
 *
 * This is the last step and it is the only one that can make a sound, so it
 * runs only when the whole path is proven: the graphs are started, the
 * periods are flowing and both amplifiers' PLLs are locked.
 *
 * The digital volume goes back to 0 dB (the field counts down into the
 * attenuation, so 0 is the loud end) and the analog gain to the index the
 * board asks for.  The gain index is deliberately kept low: the board's
 * own init program uses the loudest index, and a first sound at that gain
 * is a very loud sound. */
int ampUnmute(unsigned gain_index)
{
    if (!gOwner.amp_powered) return -EINVAL;
    if (gOwner.amp_pll_lock != gOwner.amp_powered) {
        printf("fuxi-amp: not unmuting: the PLLs are not locked (%x of %x)\n",
               gOwner.amp_pll_lock, gOwner.amp_powered);
        return -EIO;
    }
    uint32_t unmuted = 0;
    for (unsigned amp = 0; amp < CS35_AMPS; ++amp) {
        if (!(gOwner.amp_found & (1u << amp))) continue;
        Cs35Step steps[FUXI_CS35_MAX_STEPS];
        Cs35Run run;
        const unsigned count = fuxi_cs35_unmute_program(steps, gain_index);
        if (runAmpProgram(amp, steps, count, &run)) {
            printf("fuxi-amp: amp %u unmute failed\n", amp);
            continue;
        }
        unmuted |= 1u << amp;
        printf("fuxi-amp: amp %u unmuted, gain index %u\n", amp, gain_index);
    }
    gOwner.amp_unmuted = unmuted;
    printf("fuxi-amp: unmuted %x\n", unmuted);
    syncStats();
    return unmuted ? 0 : -EIO;
}

/* Reads the amplifiers' interrupt status again.
 *
 * The PLL lock is a consequence of the serial clock, and the serial clock
 * only runs once the DSP's endpoint is started, so the first reading (right
 * after the power-up) cannot show a lock.  This is the reading that matters:
 * it is taken after the graphs run. */
int ampRereadStatus()
{
    if (!gOwner.amp_powered || !gAmpI2cWindow.va) return -EINVAL;
    uint32_t locked = 0, errors = 0, pup = 0;
    for (unsigned amp = 0; amp < CS35_AMPS; ++amp) {
        if (!(gOwner.amp_found & (1u << amp))) continue;
        Cs35Step steps[FUXI_CS35_MAX_STEPS];
        Cs35Run run;
        const unsigned count = fuxi_cs35_status_program(steps);
        if (runAmpProgram(amp, steps, count, &run)) {
            printf("fuxi-amp: amp %u status re-read failed\n", amp);
            continue;
        }
        gAmpEint1[amp] = run.reads[0];
        gAmpEint2[amp] = run.reads[1];
        if (run.reads[0] & CS35_PUP_DONE) pup |= 1u << amp;
        if (run.reads[1] & CS35_EINT2_PLL_LOCK) locked |= 1u << amp;
        if (run.reads[0] & CS35_EINT1_ERRORS) errors |= 1u << amp;
        printf("fuxi-amp: amp %u later st1 %08x st3 %08x pup %u pll %u\n",
               amp, gAmpEint1[amp], gAmpEint2[amp],
               (run.reads[0] & CS35_PUP_DONE) ? 1u : 0u,
               (run.reads[1] & CS35_EINT2_PLL_LOCK) ? 1u : 0u);
    }
    gOwner.amp_pll_lock = locked;
    gOwner.amp_errors = errors;
    gOwner.amp_pup = pup;
    gOwner.amp_eint1[0] = gAmpEint1[0];
    gOwner.amp_eint1[1] = gAmpEint1[1];
    gOwner.amp_eint2[0] = gAmpEint2[0];
    gOwner.amp_eint2[1] = gAmpEint2[1];
    printf("fuxi-amp: later powered %x pup %x pll %x errors %x\n",
           gOwner.amp_powered, pup, locked, errors);
    syncStats();
    return 0;
}

static int graphWaitOp(uint32_t source_port, uint32_t token, uint32_t opcode,
                       uint8_t kind, FuxiGraphReply &reply)
{
    reply = {};
    reply.status = 0xffffffffu;
    const FuxiGraphExpect expect = {source_port, token, opcode, 0ull, 0u};
    fuxi_graph_wait_begin(&gOwner.graph_wait, &expect, kind);
    syncStats();
    const int sent = qcom_glink_send(&gOwner.glink, gOwner.channel,
                                     gOwner.graph_packet, gOwner.graph_req_size);
    if (sent != QcomGlinkOk) { fuxi_graph_wait_cancel(&gOwner.graph_wait); return sent; }
    const int rc = graphWaitReply(nowMs() + 1000u);
    if (rc) return rc;
    return reply.token == token && reply.status == 0 ? 0 : -EIO;
}

/* Hands the owned low-DRAM period buffer to the DSP: maps it, so the DSP has
 * a handle that a write can be made against.
 *
 * The address the DSP takes is not the physical one: its buffer address
 * carries the audio stream's index in the top word (the physical address in
 * the low word and the stream's own number and 0xf in bits 32:35).  A bare
 * 36-bit address names a different stream and the transfer is refused.  The
 * index is the audio stream 0x1001's own: 0x1001 and 0xf is 1. */
int graphBindPcm()
{
    constexpr uint32_t bytes = kArBufferBytes;
    if (!gOwner.graph_bind_port || !gOwner.pcm_pa) return -EINVAL;
    gOwner.period_dsp_address = qcom_ar_dsp_address(gOwner.pcm_pa, 0x1001u);
    for (unsigned slot = 0; slot < kArSlots; ++slot)
        gOwner.period_address[slot] = gOwner.period_dsp_address +
                                     (uint64_t)slot * kArPeriodBytes;
    gOwner.graph_req_size = qcom_ar_map(gOwner.graph_packet, sizeof gOwner.graph_packet,
        gOwner.graph_bind_port, gOwner.graph_bind_token + 7u, gOwner.graph_bind_apm,
        gOwner.period_dsp_address, bytes);
    if (!gOwner.graph_req_size) return -EINVAL;
    int rc = graphWaitOp(gOwner.graph_bind_port, gOwner.graph_bind_token + 7u,
                         QCOM_AR_OP_MEM_MAP, 10u, gOwner.graph_map_reply);
    if (rc) return rc;
    gOwner.period_handle = gOwner.graph_map_reply.handle;
    printf("fuxi-graph: map rc=%d handle=%x pa=%llx dsp=%llx bytes=%u\n",
           rc, gOwner.period_handle, (unsigned long long)gOwner.pcm_pa,
           (unsigned long long)gOwner.period_dsp_address, bytes);
    return gOwner.period_handle ? 0 : -EIO;
}

/* The cache lines the DSP reads have to be clean before it sees them. */
static void graphClean(const volatile void *p, uint32_t length)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const uint64_t line = 4ull << ((ctr >> 16) & 15u);
    for (uintptr_t at = reinterpret_cast<uintptr_t>(p) & ~(line - 1u),
                   end = reinterpret_cast<uintptr_t>(p) + length;
         at < end; at += line)
        __asm__ volatile("dc cvac, %0" :: "r"(at) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Allocates the three shared pages and fills the control page.
 *
 * sheng's setupShared() (package/AudioProbe.cpp) is the reference: three kIOMemoryKernelUserShared buffers (the
 * control page, the playback ring and the capture ring), the control page filled with the board's numbers and the
 * anchors taken once, so one timeline holds for the whole boot.  The tick rate is computed in 128 bits: with this
 * board's 19.2 MHz timer (timebase 625/12) 1e9 * 12 << 32 overflows 64 bits, which made the shared timeline run
 * 3.5 times too fast for the plug-in and the audio server (113.7 ticks per frame instead of 400). */
static bool setupShared()
{
    const IOOptionBits shared = static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryKernelUserShared);
    gShared.control = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, shared, QCOM_AUDIO_PAGE_BYTES, QCOM_AUDIO_PAGE_BYTES);
    gShared.outRing = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, shared,
        QCOM_AUDIO_RING_FRAMES * QCOM_AUDIO_OUT_CHANNELS * 2, QCOM_AUDIO_PAGE_BYTES);
    gShared.inRing = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, shared,
        QCOM_AUDIO_RING_FRAMES * QCOM_AUDIO_IN_CHANNELS * 2, QCOM_AUDIO_PAGE_BYTES);
    if (!gShared.control || !gShared.outRing || !gShared.inRing) return false;
    gShared.ctl = static_cast<QcomAudioControl *>(gShared.control->getBytesNoCopy());
    gShared.out = static_cast<int16_t *>(gShared.outRing->getBytesNoCopy());
    gShared.in = static_cast<int16_t *>(gShared.inRing->getBytesNoCopy());
    memset(gShared.control->getBytesNoCopy(), 0, QCOM_AUDIO_PAGE_BYTES);
    memset(gShared.out, 0, (size_t)QCOM_AUDIO_RING_FRAMES * QCOM_AUDIO_OUT_CHANNELS * 2);
    memset(gShared.in, 0, (size_t)QCOM_AUDIO_RING_FRAMES * QCOM_AUDIO_IN_CHANNELS * 2);
    mach_timebase_info_data_t tb;
    clock_timebase_info(&tb);
    const uint64_t anchor = mach_absolute_time();
    QcomAudioAbiConfig config = {};
    config.rate = QCOM_AUDIO_RATE;
    config.out_channels = QCOM_AUDIO_OUT_CHANNELS;
    config.in_channels = QCOM_AUDIO_IN_CHANNELS;
    config.period_frames = QCOM_AUDIO_PERIOD_FRAMES;
    config.ring_frames = QCOM_AUDIO_RING_FRAMES;
    /* The lead the plug-in is given: 300 ms, so the audio server writes that far ahead (sheng's kLeadFrames). */
    config.lead_frames = 30u * QCOM_AUDIO_PERIOD_FRAMES;
    config.host_clock_hz = static_cast<uint32_t>(1000000000ULL * tb.denom / tb.numer);
    config.anchor_host = anchor;
    if (qcom_audio_abi_init(gShared.ctl, &config) != QcomAudioAbiOk) return false;
    /* The tick rate in 128 bits (see the note above), written back so the plug-in reads the same number. */
    const uint64_t ticks = static_cast<uint64_t>(((__uint128_t)1000000000ULL * tb.denom << 32) /
                                                  ((uint64_t)QCOM_AUDIO_RATE * tb.numer));
    gShared.ticks_per_frame_q32 = ticks;
    gShared.ctl->ticks_per_frame_q32 = ticks;
    gShared.anchor = anchor;
    gShared.next = 0;
    gShared.frames = 0;
    gShared.late = 0;
    gShared.started = 0;
    return true;
}

/* The frame of the shared timeline that plays now. */
static uint64_t sharedFramesNow()
{
    if (!gShared.ctl || !gShared.ticks_per_frame_q32) return 0;
    const uint64_t now = mach_absolute_time();
    return now > gShared.anchor
        ? static_cast<uint64_t>(((__uint128_t)(now - gShared.anchor) << 32) / gShared.ticks_per_frame_q32)
        : 0;
}

/* Fills one slot of the period buffer.
 *
 * The buffer is silent by default.  A tone is only written when the boot
 * argument asks for one, because an amplifier that is unmuted while the
 * buffer holds something else is a noise source. */
static void graphFillSlot(unsigned slot, bool tone)
{
    if (!gOwner.pcm_va) return;
    volatile int16_t *const dst = reinterpret_cast<volatile int16_t *>(gOwner.pcm_va) +
                                  (uint32_t)(slot * kArPeriodBytes / 2u);
    if (!tone) {
        for (uint32_t i = 0; i < kArPeriodFrames * kArChannels; ++i) dst[i] = 0;
    } else {
        /* A 440 Hz sine, 48 kHz, stereo.  The phase runs across the periods
         * so the tone does not restart every 10 ms. */
        static uint32_t phase;
        const uint32_t step = (440u * (1u << 24)) / kArRate;
        for (uint32_t frame = 0; frame < kArPeriodFrames; ++frame) {
            const int32_t v = (int32_t)(phase >> 8);
            dst[frame * kArChannels] = (int16_t)v;
            dst[frame * kArChannels + 1u] = (int16_t)v;
            phase += step;
        }
    }
    graphClean(dst, kArPeriodBytes);
}

/* Copies one period out of the shared playback ring into the DSP's slot.
 *
 * The ring is not a whole number of periods, so a period can wrap; the frames taken are cleared afterwards, so a
 * frame the plug-in did not write in time plays as silence instead of what the ring held one turn earlier
 * (sheng's submit()).  The ring's frames are the ones the audio server wrote, so this is where iOS's sound
 * becomes the DSP's sound. */
static void sharedFillSlot(unsigned slot)
{
    if (!gOwner.pcm_va || !gShared.out) return;
    volatile int16_t *const dst = reinterpret_cast<volatile int16_t *>(gOwner.pcm_va) +
                                  (uint32_t)(slot * kArPeriodBytes / 2u);
    /* The first period the plug-in's timeline serves is the one that plays now: the DSP started its
     * stream before the audio server connected, so the ring's position is the play position and not
     * the anchor.  From here on the periods follow one another (sheng's gAudio.next). */
    if (!gShared.started) {
        gShared.next = sharedFramesNow() / kArPeriodFrames;
        gShared.started = 1;
    }
    const uint64_t period = gShared.next;
    const uint32_t at = static_cast<uint32_t>((period * kArPeriodFrames) % QCOM_AUDIO_RING_FRAMES);
    const uint32_t head = QCOM_AUDIO_RING_FRAMES - at < kArPeriodFrames ? QCOM_AUDIO_RING_FRAMES - at : kArPeriodFrames;
    memcpy(const_cast<int16_t *>(dst), gShared.out + (size_t)at * kArChannels,
           (size_t)head * kArChannels * 2);
    if (head < kArPeriodFrames)
        memcpy(const_cast<int16_t *>(dst) + (size_t)head * kArChannels, gShared.out,
               (size_t)(kArPeriodFrames - head) * kArChannels * 2);
    /* The frames are taken: clear them, so a frame the audio server did not write in time plays as silence. */
    memset(gShared.out + (size_t)at * kArChannels, 0, (size_t)head * kArChannels * 2);
    if (head < kArPeriodFrames)
        memset(gShared.out, 0, (size_t)(kArPeriodFrames - head) * kArChannels * 2);
    /* Late: the audio server was running and had not written the period yet. */
    if (gShared.ctl && (gShared.want & QcomAudioOutputRunning) &&
        gShared.ctl->out_written < (period + 1) * kArPeriodFrames)
        gShared.late++;
    if (gShared.ctl) gShared.ctl->out_frames = gShared.frames;
    gShared.frames += kArPeriodFrames;
    graphClean(dst, kArPeriodBytes);
}

/* Sends one period to the DSP.
 *
 * The write is fire and forget: the DSP answers with WRITE_DONE when it
 * has taken the buffer, and that answer is what frees the slot for the next
 * period.  Four periods are in flight at a time and the buffer has eight
 * slots, so a period that comes back late never collides with one that is
 * still on its way. */
static int graphSendPeriod(unsigned slot, bool tone)
{
    if (!gOwner.period_handle || !gOwner.pcm_va) return -EINVAL;
    /* With the iOS audio server's plug-in connected, the periods carry the
     * samples the audio server wrote into the shared ring; without it, the
     * board's own tone (or silence). */
    if (gShared.serving)
        sharedFillSlot(slot);
    else
        graphFillSlot(slot, tone);
    gOwner.graph_req_size = qcom_ar_write(gOwner.graph_packet, sizeof gOwner.graph_packet,
        gOwner.graph_bind_port, gOwner.period_counter + 1u, kArIidShmem,
        gOwner.period_address[slot], gOwner.period_handle, kArPeriodBytes);
    if (!gOwner.graph_req_size) return -EINVAL;
    const int sent = qcom_glink_send(&gOwner.glink, gOwner.channel,
                                     gOwner.graph_packet, gOwner.graph_req_size);
    if (sent != QcomGlinkOk) return sent;
    gOwner.period_active[slot] = 1;
    ++gOwner.period_in_flight;
    ++gOwner.period_sent;
    if (gShared.serving) ++gShared.next;
    return 0;
}

/* The answer to a period's write: the DSP names the buffer it took. */
static void graphHandleWriteDone(const FuxiGraphReply &reply)
{
    gOwner.period_status = reply.status;
    for (unsigned slot = 0; slot < kArSlots; ++slot) {
        if (!gOwner.period_active[slot]) continue;
        if (gOwner.period_address[slot] != reply.address) continue;
        gOwner.period_active[slot] = 0;
        if (gOwner.period_in_flight) --gOwner.period_in_flight;
        ++gOwner.period_done;
        return;
    }
    printf("fuxi-graph: write done for an address nobody sent: %llx status %x\n",
           (unsigned long long)reply.address, reply.status);
}

/* Keeps the DSP's queue fed: as many periods in flight as the reference
 * client keeps (four), and never more than one write per slot. */
static void graphPumpPeriods(bool tone)
{
    if (!gOwner.period_pump) return;
    /* The board decides: fuxi-tone=1 puts a 440 Hz sine in the periods
     * instead of silence.  It is separate from the amplifier's state, so a
     * muted amplifier still hears nothing. */
    if (gOwner.period_tone) tone = true;
    while (gOwner.period_in_flight < kArInFlight) {
        const unsigned slot = gOwner.period_counter % kArSlots;
        if (gOwner.period_active[slot]) return;
        const int rc = graphSendPeriod(slot, tone);
        if (rc) {
            printf("fuxi-graph: period %u slot %u could not be sent: %d\n",
                   gOwner.period_counter, slot, rc);
            return;
        }
        ++gOwner.period_counter;
    }
}

/* Prepares and starts the two graphs, then starts the period pump.
 *
 * The order is the reference client's: the buffer is mapped first (the map
 * is what gives the writes a handle), the device graph (the endpoint that
 * pulls the audio) is prepared and started first, and the stream graph
 * (the one that feeds it) second.  The stream graph's open already names
 * the device graph's endpoint, so the connection exists before either of
 * them runs. */
int graphRun()
{
    const uint32_t source_port = fuxi_audio_binding()->platform.gpr_port;
    const uint32_t token = 0x1101u;
    if (!gOwner.graph_device_ready || !gOwner.graph_stream_ready) return -EINVAL;
    if (!gOwner.period_handle) return -EINVAL;

    int rc = graphCommandOp(source_port, token + 5u, QCOM_AR_OP_GRAPH_PREPARE,
                            kArSgDevice, 8u, gOwner.graph_device_prepare_reply);
    printf("fuxi-graph: device prepare rc=%d status=%x\n",
           rc, gOwner.graph_device_prepare_reply.status);
    if (rc) return rc;
    rc = graphCommandOp(source_port, token + 6u, QCOM_AR_OP_GRAPH_PREPARE,
                         kArSgStream, 12u, gOwner.graph_prepare_reply);
    printf("fuxi-graph: stream prepare rc=%d status=%x\n",
           rc, gOwner.graph_prepare_reply.status);
    if (rc) return rc;
    gOwner.graph_phase = 23u;
    syncStats();

    rc = graphCommandOp(source_port, token + 7u, QCOM_AR_OP_GRAPH_START,
                         kArSgDevice, 9u, gOwner.graph_device_start_reply);
    printf("fuxi-graph: device start rc=%d status=%x\n",
           rc, gOwner.graph_device_start_reply.status);
    if (rc) return rc;
    rc = graphCommandOp(source_port, token + 8u, QCOM_AR_OP_GRAPH_START,
                         kArSgStream, 13u, gOwner.graph_start_reply);
    printf("fuxi-graph: stream start rc=%d status=%x\n",
           rc, gOwner.graph_start_reply.status);
    if (rc) return rc;
    gOwner.graph_phase = 24u;
    syncStats();

    /* The pump starts silent.  The amplifier is still muted, so the DSP
     * plays silence into a muted amplifier: nothing is heard, and the
     * periods still prove that the whole path carries data. */
    gOwner.period_pump = 1;
    graphPumpPeriods(false);
    printf("fuxi-graph: running, periods sent %u in flight %u handle %x\n",
           gOwner.period_sent, gOwner.period_in_flight, gOwner.period_handle);
    return 0;
}

int graphOpenProbe()
{
    /* fgraph=6: same bring-up as fgraph=5 but the graph stays open (no close),
     * so the running stream can consume the owned PCM buffer. */
    const bool persistent = gGraphProbeLevel >= 6u;
    const uint32_t source_port = fuxi_audio_binding()->platform.gpr_port;
    const uint32_t token = 0x1101u;

    /* The device graph owns the hardware endpoint: the secondary MI2S (I2S)
     * sink the amplifiers listen on. */
    const QcomArGraphSpec spec = {
        kArApmIid, kArSgDevice, QCOM_AR_PERF_LOW_LATENCY, QCOM_AR_DIR_TX,
        QCOM_AR_SCENARIO_PLAYBACK, kArContainerDevice, QCOM_AR_POS_STREAM, 8192u, GPR_DOMAIN_ADSP,
    };
    const QcomArGraphModule modules[1] = {{QCOM_AR_MOD_I2S_SINK, kArIidEndpoint, 1u, 0u}};
    /* The stream graph owns the shared memory endpoint and the two
     * converters that turn the shared memory's PCM into the endpoint's
     * format.  Its last connection crosses into the device graph. */
    const QcomArGraphSpec stream = {
        kArApmIid, kArSgStream, QCOM_AR_PERF_LOW_LATENCY, QCOM_AR_DIR_RX,
        QCOM_AR_SCENARIO_PLAYBACK, kArContainerStream, QCOM_AR_POS_STREAM, 8192u, GPR_DOMAIN_ADSP,
    };
    const QcomArGraphModule stream_modules[3] = {
        {QCOM_AR_MOD_WR_SHMEM, kArIidShmem, 0u, 1u},
        {QCOM_AR_MOD_PCM_DEC, kArIidDecoder, 1u, 1u},
        {QCOM_AR_MOD_PCM_CNV, kArIidConverter, 1u, 1u},
    };
    const QcomArGraphLink stream_links[3] = {
        {kArIidShmem, kArIidDecoder}, {kArIidDecoder, kArIidConverter},
        {kArIidConverter, kArIidEndpoint},
    };
    /* Bind parameters are needed after the PCM probe, so record them here. */
    gOwner.graph_bind_port = source_port;
    gOwner.graph_bind_token = token;
    gOwner.graph_bind_apm = kArApmIid;

    gOwner.graph_req_size = qcom_ar_graph_open(gOwner.graph_packet, sizeof gOwner.graph_packet,
                                               source_port, token, &spec, modules, 1u, nullptr, 0u);
    if (!gOwner.graph_req_size) return -EINVAL;
    gOwner.graph_open_reply = {};
    gOwner.graph_open_reply.status = 0xffffffffu;
    const FuxiGraphExpect open_expect = {source_port, token, QCOM_AR_OP_GRAPH_OPEN, 0ull, 0u};
    fuxi_graph_wait_begin(&gOwner.graph_wait, &open_expect, 1u);
    gOwner.graph_phase = 1u;
    syncStats();
    printf("fuxi-graph: device open send port=%x bytes=%u sg=%x container=%x iid=%x\n",
           source_port, gOwner.graph_req_size, spec.subgraph_id, spec.container_id,
           kArIidEndpoint);
    int rc = qcom_glink_send(&gOwner.glink, gOwner.channel, gOwner.graph_packet, gOwner.graph_req_size);
    if (rc != QcomGlinkOk) { fuxi_graph_wait_cancel(&gOwner.graph_wait); return rc; }
    gOwner.graph_phase = 2u;
    syncStats();
    rc = graphWaitReply(nowMs() + 1000u);
    printf("fuxi-graph: device open reply rc=%d token=%x status=%x bytes=%u\n",
           rc, gOwner.graph_open_reply.token, gOwner.graph_open_reply.status, gOwner.graph_rsp_size);
    if (rc) return rc;
    gOwner.graph_phase = 3u;
    syncStats();
    if (gOwner.graph_open_reply.token != token || gOwner.graph_open_reply.status != 0) return -EIO;
    gOwner.graph_device_ready = 1u;

    const QcomArI2sInterface intf = {
        QCOM_AR_LPAIF, QCOM_AR_I2S_SECONDARY, QCOM_AR_I2S_SD1, QCOM_AR_WS_INTERNAL,
    };
    const QcomArFormat format = {kArRate, kArBits, kArChannels};
    gOwner.graph_req_size = qcom_ar_config_hw_endpoint(
        gOwner.graph_packet, sizeof gOwner.graph_packet, source_port, token + 1u,
        spec.apm_iid, modules[0].instance, &intf, &format);
    if (!gOwner.graph_req_size) return -EINVAL;
    gOwner.graph_config_reply = {};
    gOwner.graph_config_reply.status = 0xffffffffu;
    const FuxiGraphExpect config_expect = {source_port, token + 1u, QCOM_AR_OP_SET_CFG, 0ull, 0u};
    fuxi_graph_wait_begin(&gOwner.graph_wait, &config_expect, 3u);
    gOwner.graph_phase = 7u;
    syncStats();
    rc = qcom_glink_send(&gOwner.glink, gOwner.channel, gOwner.graph_packet, gOwner.graph_req_size);
    if (rc != QcomGlinkOk) { fuxi_graph_wait_cancel(&gOwner.graph_wait); return rc; }
    rc = graphWaitReply(nowMs() + 1000u);
    printf("fuxi-graph: endpoint config rc=%d token=%x status=%x\n",
           rc, gOwner.graph_config_reply.token, gOwner.graph_config_reply.status);
    const int config_rc = rc ? rc :
        (gOwner.graph_config_reply.token == token + 1u && gOwner.graph_config_reply.status == 0 ? 0 : -EIO);
    if (!config_rc) gOwner.graph_phase = 8u;
    syncStats();

    int stream_rc = 0;
    if (gGraphProbeLevel >= 2u && !config_rc) {
        gOwner.graph_stream_reply = {};
        gOwner.graph_stream_reply.status = 0xffffffffu;
        gOwner.graph_stream_close_reply.status = 0xffffffffu;
        gOwner.graph_req_size = qcom_ar_graph_open(
            gOwner.graph_packet, sizeof gOwner.graph_packet, source_port, token + 2u,
            &stream, stream_modules, 3u, stream_links, 3u);
        if (!gOwner.graph_req_size) stream_rc = -EINVAL;
        else {
            const FuxiGraphExpect stream_expect = {source_port, token + 2u, QCOM_AR_OP_GRAPH_OPEN, 0ull, 0u};
            fuxi_graph_wait_begin(&gOwner.graph_wait, &stream_expect, 4u);
            gOwner.graph_phase = 9u;
            syncStats();
            printf("fuxi-graph: stream open send sg=%x container=%x bytes=%u\n",
                   stream.subgraph_id, stream.container_id, gOwner.graph_req_size);
            rc = qcom_glink_send(&gOwner.glink, gOwner.channel, gOwner.graph_packet, gOwner.graph_req_size);
            if (rc != QcomGlinkOk) {
                fuxi_graph_wait_cancel(&gOwner.graph_wait);
                stream_rc = rc;
            } else {
                rc = graphWaitReply(nowMs() + 1000u);
                stream_rc = rc ? rc :
                    (gOwner.graph_stream_reply.token == token + 2u &&
                     gOwner.graph_stream_reply.status == 0 ? 0 : -EIO);
            }
            if (!stream_rc) gOwner.graph_phase = 10u;
            syncStats();
            printf("fuxi-graph: stream open rc=%d status=%x bytes=%u\n",
                   stream_rc, gOwner.graph_stream_reply.status, gOwner.graph_req_size);
        }
        const bool stream_opened = stream_rc == 0;
        if (stream_opened) gOwner.graph_stream_ready = 1u;
        if (stream_opened && gGraphProbeLevel >= 3u) {
            gOwner.graph_playback_reply.status = 0xffffffffu;
            gOwner.graph_shmem_reply.status = 0xffffffffu;
            gOwner.graph_phase = 15u;
            gOwner.graph_req_size = qcom_ar_config_playback_stream(
                gOwner.graph_packet, sizeof gOwner.graph_packet, source_port, token + 3u,
                stream.apm_iid, kArIidDecoder, kArIidConverter, &format);
            stream_rc = graphSetCfg(source_port, token + 3u, 6u,
                                    gOwner.graph_req_size, gOwner.graph_playback_reply);
            printf("fuxi-graph: playback config rc=%d status=%x\n",
                   stream_rc, gOwner.graph_playback_reply.status);
            if (!stream_rc) {
                gOwner.graph_phase = 16u;
                gOwner.graph_req_size = qcom_ar_config_media_format(
                    gOwner.graph_packet, sizeof gOwner.graph_packet, source_port,
                    token + 4u, kArIidShmem, &format);
                stream_rc = graphSetCfg(source_port, token + 4u, 7u,
                                        gOwner.graph_req_size, gOwner.graph_shmem_reply);
                printf("fuxi-graph: shmem config rc=%d status=%x\n",
                       stream_rc, gOwner.graph_shmem_reply.status);
            }
            if (!stream_rc) gOwner.graph_phase = 17u;
            syncStats();
            /* The diagnostic levels (fgraph=3..5) run the graphs and then
             * close them again, so they need the prepare and start.  The
             * running level (fgraph=6) does that later, after the buffer
             * has been mapped, because the map is what the writes need. */
            if (!stream_rc && !persistent) {
                int prc = graphCommandOp(source_port, token + 5u, QCOM_AR_OP_GRAPH_PREPARE,
                                         kArSgDevice, 8u, gOwner.graph_device_prepare_reply);
                printf("fuxi-graph: device prepare rc=%d status=%x\n",
                       prc, gOwner.graph_device_prepare_reply.status);
                if (!prc)
                    prc = graphCommandOp(source_port, token + 6u, QCOM_AR_OP_GRAPH_PREPARE,
                                         kArSgStream, 12u, gOwner.graph_prepare_reply);
                if (!prc) {
                    printf("fuxi-graph: stream prepare rc=%d status=%x\n",
                           prc, gOwner.graph_prepare_reply.status);
                    int src = graphCommandOp(source_port, token + 7u, QCOM_AR_OP_GRAPH_START,
                                             kArSgDevice, 9u, gOwner.graph_device_start_reply);
                    printf("fuxi-graph: device start rc=%d status=%x\n",
                           src, gOwner.graph_device_start_reply.status);
                    if (!src)
                        src = graphCommandOp(source_port, token + 8u, QCOM_AR_OP_GRAPH_START,
                                             kArSgStream, 13u, gOwner.graph_start_reply);
                    if (!src)
                        printf("fuxi-graph: stream start rc=%d status=%x\n",
                               src, gOwner.graph_start_reply.status);
                }
                gOwner.graph_phase = 23u;
                syncStats();
            }
        }
        if (stream_opened && !persistent) {
            const uint32_t stream_close_token = token + 11u;
            gOwner.graph_stream_close_reply = {};
            gOwner.graph_stream_close_reply.status = 0xffffffffu;
            gOwner.graph_req_size = qcom_ar_graph_command(
                gOwner.graph_packet, sizeof gOwner.graph_packet, source_port,
                stream_close_token, kArApmIid, QCOM_AR_OP_GRAPH_CLOSE, stream.subgraph_id);
            if (!gOwner.graph_req_size) { if (!stream_rc) stream_rc = -EINVAL; }
            else {
                const FuxiGraphExpect stream_close_expect = {
                    source_port, stream_close_token, QCOM_AR_OP_GRAPH_CLOSE, 0ull, 0u,
                };
                fuxi_graph_wait_begin(&gOwner.graph_wait, &stream_close_expect, 5u);
                gOwner.graph_phase = 11u;
                syncStats();
                rc = qcom_glink_send(&gOwner.glink, gOwner.channel,
                                     gOwner.graph_packet, gOwner.graph_req_size);
                int close_rc = 0;
                if (rc != QcomGlinkOk) {
                    fuxi_graph_wait_cancel(&gOwner.graph_wait);
                    close_rc = rc;
                } else {
                    rc = graphWaitReply(nowMs() + 1000u);
                    close_rc = rc ? rc :
                        (gOwner.graph_stream_close_reply.token == stream_close_token &&
                         gOwner.graph_stream_close_reply.status == 0 ? 0 : -EIO);
                }
                if (!close_rc) gOwner.graph_phase = 12u;
                if (!stream_rc) stream_rc = close_rc;
                syncStats();
            }
        }
    }

    if (persistent) return 0;   /* keep the graph running */
    /* The close tokens come after every other token this function used:
     * the open (0), the endpoint config (1), the stream open (2), the
     * playback config (3), the shared memory config (4), the two prepares
     * (5, 6), the two starts (7, 8) and the stream close (11). */
    const uint32_t device_close_token = token + 12u;
    gOwner.graph_req_size = qcom_ar_graph_command(gOwner.graph_packet, sizeof gOwner.graph_packet,
                                                  source_port, device_close_token, kArApmIid,
                                                  QCOM_AR_OP_GRAPH_CLOSE, spec.subgraph_id);
    if (!gOwner.graph_req_size) return -EINVAL;
    gOwner.graph_close_reply = {};
    gOwner.graph_close_reply.status = 0xffffffffu;
    const FuxiGraphExpect close_expect = {source_port, device_close_token, QCOM_AR_OP_GRAPH_CLOSE, 0ull, 0u};
    fuxi_graph_wait_begin(&gOwner.graph_wait, &close_expect, 2u);
    gOwner.graph_phase = 4u;
    syncStats();
    rc = qcom_glink_send(&gOwner.glink, gOwner.channel, gOwner.graph_packet, gOwner.graph_req_size);
    if (rc != QcomGlinkOk) { fuxi_graph_wait_cancel(&gOwner.graph_wait); return rc; }
    gOwner.graph_phase = 5u;
    syncStats();
    rc = graphWaitReply(nowMs() + 1000u);
    if (rc) return rc;
    gOwner.graph_phase = config_rc || stream_rc ? 14u :
                         (gGraphProbeLevel >= 3u ? 18u : (gGraphProbeLevel >= 2u ? 13u : 6u));
    syncStats();
    if (gOwner.graph_close_reply.token != device_close_token || gOwner.graph_close_reply.status != 0) return -EIO;
    return config_rc ? config_rc : stream_rc;
}

int probeOwnedLowPcm()
{
    constexpr uint64_t bytes = 64u * 1024u;
    gOwner.pcm_buffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        static_cast<IOOptionBits>(kIODirectionInOut) |
        static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous) |
        static_cast<IOOptionBits>(kIOMemoryMapperNone),
        bytes, 0x00000000fffff000ull);
    if (!gOwner.pcm_buffer) return -ENOMEM;
    if (gOwner.pcm_buffer->prepare() != kIOReturnSuccess) {
        gOwner.pcm_buffer->release();
        gOwner.pcm_buffer = nullptr;
        return -EIO;
    }
    addr64_t segment_bytes = 0;
    const uint64_t pa = gOwner.pcm_buffer->getPhysicalSegment(0, &segment_bytes, kIOMemoryMapperNone);
    gOwner.pcm_pa = pa;
    gOwner.pcm_segment_bytes = segment_bytes;
    kprintf("FUXI_ADSP PCM owned pa=%llx span=%llx bytes=%llx\n",
            pa, static_cast<uint64_t>(segment_bytes), bytes);
    if (!gOwner.pcm_buffer->getBytesNoCopy() || (pa & 0xfffu) ||
        pa >= 0x100000000ull || segment_bytes < bytes ||
        pa > 0x100000000ull - bytes) {
        gOwner.pcm_buffer->complete();
        gOwner.pcm_buffer->release();
        gOwner.pcm_buffer = nullptr;
        return -ERANGE;
    }
    return 0;
}

static uint32_t smmu_rd_adapter(void *ctx, unsigned offset)
{
    Window window = {};
    window.va = reinterpret_cast<volatile uint8_t *>(ctx);
    return rd(window, offset);
}
static void smmu_wr_adapter(void *ctx, unsigned offset, uint32_t value)
{
    Window window = {};
    window.va = reinterpret_cast<volatile uint8_t *>(ctx);
    wr(window, offset, value);
}

int probeOwnedPcmSmmu()
{
    constexpr uint64_t bytes = 64u * 1024u;
    /* The stock fuxi DTB's msm-audio-ion node names streams 0x1001 and
     * 0x1061 and a 0x10000000..0x1fffffff IOVA pool.  This probe only reads
     * their current apps-SMMU state; it never changes a stream mapping. */
    constexpr uint64_t smmu_base = 0x15000000ull;
    /* P3 of docs/audio-porting.md: the DSP takes a 32-bit address, so the period
     * buffer is a fixed low-DRAM range (the gap 0xCCF80000..0xD8000000 the UEFI
     * map leaves conventional), never a runtime allocation (which lands above 4 GiB).
     * The range must not overlap the kernel's physical window or the GPU addon. */
    constexpr uint64_t kLowPa = 0xd0000000ull;
    IODeviceMemory *low = IODeviceMemory::withRange(kLowPa, bytes);
    gOwner.pcm_map = low ? low->map(kIOMapAnywhere | kIOMapWriteCombineCache) : nullptr;
    if (low) low->release();
    gOwner.pcm_va = gOwner.pcm_map ? reinterpret_cast<volatile uint8_t *>(gOwner.pcm_map->getVirtualAddress()) : nullptr;
    if (!gOwner.pcm_va) return -ENOMEM;
    gOwner.pcm_pa = kLowPa;
    gOwner.pcm_segment_bytes = bytes;
    if (kLowPa < gPhysBase + gPhysSize && gPhysBase < kLowPa + bytes) return -ERANGE;
    {
        const uint8_t *addon = nullptr; uint64_t addon_bytes = 0;
        if (native_ram_block_gpu_addon && native_ram_block_gpu_addon(&addon, &addon_bytes) && addon && addon_bytes) {
            const uint64_t addon_pa = kvtophys(reinterpret_cast<uint64_t>(addon));
            if (addon_pa && kLowPa < addon_pa + addon_bytes && addon_pa < kLowPa + bytes) return -ERANGE;
        }
    }
    for (uint64_t i = 0; i < bytes; ++i) gOwner.pcm_va[i] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    kprintf("FUXI_ADSP PCM owned pa=%llx span=%llx (fixed low DRAM)\n",
            gOwner.pcm_pa, static_cast<uint64_t>(bytes));
    Window smmu = {};
    if (!mapWindow(smmu, smmu_base, 0xd0000u)) return -ENOMEM;
    const uint32_t idr0 = rd(smmu, 0x20), idr1 = rd(smmu, 0x24), idr2 = rd(smmu, 0x28);
    gOwner.audio_smmu_idr0 = idr0;
    gOwner.audio_smmu_idr1 = idr1;
    gOwner.audio_smmu_idr2 = idr2;
    const unsigned groups = (idr0 & 0xffu) < 128u ? (idr0 & 0xffu) : 128u;
    const unsigned banks = idr1 & 0xffu;
    const unsigned s2_banks = (idr1 >> 16) & 0xffu;
    const unsigned pages = 1u << (((idr1 >> 28) & 7u) + 1u);
    kprintf("FUXI_ADSP audio SMMU idr=%08x/%08x/%08x groups=%u\n",
            idr0, idr1, idr2, groups);
    bool used_bank[128] = {};
    unsigned matches = 0;
    gOwner.audio_smmu_free_slot = 0xfffu;
    gOwner.audio_smmu_free_bank = 0xfffu;
    for (unsigned slot = 0; slot < groups; ++slot) {
        const uint32_t smr = rd(smmu, 0x800u + 4u * slot);
        if (!(smr & 0x80000000u)) {
            if (gOwner.audio_smmu_free_slot == 0xfffu) gOwner.audio_smmu_free_slot = slot;
            continue;
        }
        const uint32_t s2cr = rd(smmu, 0xc00u + 4u * slot);
        if (((s2cr >> 16) & 3u) == 0 && (s2cr & 0xffu) < 128u)
            used_bank[s2cr & 0xffu] = true;
        const uint32_t mask = (smr >> 16) & 0x7fffu;
        if (((smr ^ 0x1001u) & 0xffffu & ~mask) &&
            ((smr ^ 0x1081u) & 0xffffu & ~mask) &&
            ((smr ^ 0x1061u) & 0xffffu & ~mask)) continue;
        kprintf("FUXI_ADSP audio SMMU slot=%u smr=%08x s2cr=%08x\n",
                slot, smr, s2cr);
        if (!matches) {
            gOwner.audio_smmu_slot0 = slot;
            gOwner.audio_smmu_smr0 = smr;
            gOwner.audio_smmu_s2cr0 = s2cr;
        }
        ++matches;
    }
    gOwner.audio_smmu_matches = matches;
    if (banks <= 128u && banks > s2_banks + 1u) {
        for (unsigned bank = s2_banks; bank + 1u < banks; ++bank) {
            if (used_bank[bank]) continue;
            const uint64_t cb = static_cast<uint64_t>(pages + bank) * 4096u;
            const uint32_t sctlr = rd(smmu, cb);
            if (sctlr & 1u) continue;
            gOwner.audio_smmu_free_bank = bank;
            gOwner.audio_smmu_candidate_sctlr = sctlr;
            gOwner.audio_smmu_candidate_cbar = rd(smmu, 0x1000u + 4u * bank);
            gOwner.audio_smmu_candidate_cba2r = rd(smmu, 0x1800u + 4u * bank);
            break;
        }
    }
    kprintf("FUXI_ADSP audio SMMU matched=%u\n", matches);
    /* P3 final step: put the two audio streams in pass-through for the fixed
     * low-DRAM period buffer. A translating context resets this board
     * (audio-porting.md), so only the shared bypass bank is used. */
    {
        int abyp = 0;
        PE_parse_boot_argn("fuxi-abyp", &abyp, sizeof abyp);
        if (abyp) {
            struct smmu_io io = { const_cast<uint8_t *>(smmu.va), smmu_rd_adapter, smmu_wr_adapter };
            /* The reference client bypasses three audio streams: the
             * playback one, the record one and the shared memory one. */
            static const uint16_t streams[3] = { 0x1001, 0x1081, 0x1061 };
            for (unsigned i = 0; i < 3; ++i) {
                struct smmu_plan plan = smmu_plan_bypass(&io, streams[i], gOwner.pcm_pa, bytes);
                const char *error = plan.error ? plan.error : smmu_configure_bypass(&io, &plan, streams[i]);
                gOwner.audio_bypass_err[i] = error ? 1u : 0u;
                gOwner.audio_bypass_slot[i] = plan.error ? 0xfffu : plan.slot;
                kprintf("FUXI_ADSP bypass stream=%04x slot=%u bank=%u %s\n",
                        streams[i], plan.slot, plan.bank, error ? error : "ok");
            }
        }
    }
    unmapWindow(smmu);
    return 0;
}

int waitGpr()
{
    gpr_spf_init(&gOwner.spf, fuxi_audio_binding()->platform.gpr_port, 50);
    gOwner.spf_length = 0;
    gOwner.spf_sent_ms = 0;
    gOwner.gpr_next_query_ms = 0;
    gOwner.gpr_req_raw_len = 0;
    gOwner.gpr_rsp_raw_len = 0;
    const uint64_t deadline = nowMs() + 20000;
    while (nowMs() < deadline) {
        if (gOwner.spf.state == SpfReady) {
            gOwner.gpr_ready = true;
            syncStats();
            return 0;
        }
        if (gOwner.spf.state == SpfGaveUp) return -60;
        if (gOwner.spf.state == SpfWaiting) {
            if (gOwner.spf_sent_ms && nowMs() - gOwner.spf_sent_ms >= 1000) {
                const int timeout = gpr_spf_timeout(&gOwner.spf);
                gOwner.spf_sent_ms = 0;
                        if (timeout == SpfDone) return -60;
            }
        } else if (!gOwner.spf_length) {
            gOwner.spf_length = gpr_spf_next(&gOwner.spf, gOwner.spf_packet);
        }
        if (gOwner.spf_length) {
            if (!gOwner.gpr_req_raw_len) {
                gOwner.gpr_req_raw_len = gOwner.spf_length < sizeof gOwner.gpr_req_raw ? gOwner.spf_length : sizeof gOwner.gpr_req_raw;
                memcpy(gOwner.gpr_req_raw, gOwner.spf_packet, gOwner.gpr_req_raw_len);
                logGprBytes("GPRQ", gOwner.gpr_req_raw, gOwner.gpr_req_raw_len);
            }
            GprPacket request = {};
            if (gpr_decode(gOwner.spf_packet, gOwner.spf_length, &request) == GprOk) {
                gOwner.gpr_req_opcode = request.opcode;
                gOwner.gpr_req_token = request.token;
                gOwner.gpr_req_src_port = request.src_port;
                gOwner.gpr_req_dst_port = request.dst_port;
                gOwner.gpr_req_size = request.size;
            }
            const int rc = qcom_glink_send(&gOwner.glink, gOwner.channel, gOwner.spf_packet, gOwner.spf_length);
            if (rc == QcomGlinkOk) {
                gOwner.spf_length = 0;
                gOwner.spf_sent_ms = nowMs();
            } else if (rc != QcomGlinkAgain) {
                return rc;
            }
        }
        serviceEvents(waitEvents(20) | kEvGpr | kEvGlink | kEvSend);
    }
    return -60;
}

void ownerThread(void *, wait_result_t)
{
    thread_set_thread_name(current_thread(), "fuxi-adsp");
    const FuxiAudioBinding *binding = fuxi_audio_binding();
    gOwner.config = binding->adsp;
    /* Keep this as a compile-time stage limit: PE_parse_boot_argn matched the
     * "fuxi-adsp=2" prefix when asked for "fuxi-adsp-limit", clamping the
     * first ready round back to 10.  Round N changes this one line. */
    gOwner.limit = 14;

    if (gHookMask != FuxiAdspHookAll) {
        failStage(QcomAdspStMapped, QcomAdspBadConfig);
        return;
    }

    /* bsd_init precedes NativeUfs; wait for its readiness on this worker with
     * one bounded deadline, as the mode-1 inventory thread does. */
    for (unsigned attempt = 0; attempt < 120 && !fuxi_adsp_fw_ready(); ++attempt) IOSleep(250);
    if (!fuxi_adsp_fw_ready()) { failStage(QcomAdspStFirmware, ETIMEDOUT); return; }
    int rc = gHooks.firmware_source(gHooks.context);
    if (rc) { failStage(QcomAdspStFirmware, rc); return; }
    completeHook(FuxiAdspHookFirmware);
    rc = gHooks.map_carveout(gHooks.context, 0, 0);
    if (rc) { failStage(QcomAdspStMapped, rc); return; }
    completeHook(FuxiAdspHookCarveout);
    rc = gHooks.smp2p_setup(gHooks.context);
    if (rc) { failStage(QcomAdspStSmp2p, rc); return; }
    completeHook(FuxiAdspHookSmp2p);
    rc = gHooks.ipcc_attach(gHooks.context);
    if (rc) { failStage(QcomAdspStDoorbells, rc); return; }
    completeHook(FuxiAdspHookIpcc);
    rc = gHooks.qmp_load_state(gHooks.context, binding->adsp.load_state_message);
    if (rc) { failStage(QcomAdspStLoadState, rc); return; }
    completeHook(FuxiAdspHookQmp);
    rc = gHooks.pas_auth_reset(gHooks.context);
    if (rc) { failStage(gOwner.stage ? gOwner.stage : QcomAdspStStarted, rc); return; }
    completeHook(FuxiAdspHookPas);

    if (gOwner.limit <= QcomAdspStStarted) {
        native_fuxi_adsp_stats[2] = 1;
        setStage(QcomAdspStStarted);
        kprintf("FUXI_ADSP stopped after PAS limit=%u\n", gOwner.limit);
        return;
    }

    qcom_adsp_init(&gOwner.ladder, &gOwner.config, nullptr, &gOwner.adsp.mdt, &gOwner.qmp,
                   &gOwner.smp2p, nullptr, nullptr);
    gOwner.ladder.hooks.auth = nullptr;
    gOwner.ladder.hooks.memory = nullptr;
    rc = waitReady();
    if (rc) { failStage(QcomAdspStReady, rc); rollbackOwner(nullptr, 0); return; }
    releaseImage(gOwner.dtb);
    releaseImage(gOwner.adsp);

    if (gOwner.limit <= QcomAdspStReady) {
        native_fuxi_adsp_stats[2] = 1;
        kprintf("FUXI_ADSP ready limit=%u smp=%08x\n", gOwner.limit, native_fuxi_adsp_stats[20]);
        return;
    }

    rc = gHooks.glink_setup(gHooks.context);
    if (rc) { failStage(QcomAdspStGlink, rc); rollbackOwner(nullptr, 0); return; }
    completeHook(FuxiAdspHookGlink);
    rc = waitGlink();
    if (rc) { failStage(QcomAdspStChannel, rc); rollbackOwner(nullptr, 0); return; }

    if (gOwner.limit <= QcomAdspStChannel) {
        native_fuxi_adsp_stats[2] = 1;
        kprintf("FUXI_ADSP glink limit=%u rx=%u tx=%u\n", gOwner.limit, gOwner.glink.rx_messages, gOwner.glink.tx_messages);
        return;
    }

    if (gGraphProbeLevel) {
        /* Let CDC attach before exercising the first APM graph command. */
        const uint64_t graph_after_ms = nowMs() + 70000u;
        while (nowMs() < graph_after_ms && gOwner.glink_up && !gOwner.glink.broken)
            serviceEvents(waitEvents(100) | kEvSend | kEvGlink);
        /* The order is the reference client's: the buffer and the
         * amplifiers first (they are the board's own hardware and take the
         * most time to settle), then the graphs, then the map, then the
         * run. */
        if (gGraphProbeLevel >= 4u) {
            gOwner.graph_phase = gGraphProbeLevel >= 5u ? 21u : 19u;
            syncStats();
            rc = gGraphProbeLevel >= 5u ? probeOwnedPcmSmmu() : probeOwnedLowPcm();
            gOwner.graph_phase = rc ? 20u : (gGraphProbeLevel >= 5u ? 22u : 19u);
            syncStats();
            kprintf("FUXI_ADSP PCM allocation rc=%d phase=%u\n", rc, gOwner.graph_phase);
        }
        {
            int amp = 0;
            if (PE_parse_boot_argn("fuxi-amp", &amp, sizeof amp) && amp) {
                gAmpLevel = static_cast<uint32_t>(amp);
                (void)probeOwnedAmp();
                gOwner.graph_phase = 26u;
                syncStats();
            }
            {
                int tone = 0;
                if (PE_parse_boot_argn("fuxi-tone", &tone, sizeof tone) && tone) {
                    gOwner.period_tone = 1u;
                    printf("fuxi-graph: the tone generator is on\n");
                }
            }
        }
        if (!rc) {
            rc = graphOpenProbe();
            if (!rc && gGraphProbeLevel >= 6u) {
                const int bind = graphBindPcm();
                kprintf("FUXI_ADSP bind rc=%d handle=%x\n",
                        bind, gOwner.period_handle);
                if (!bind) {
                    gOwner.graph_phase = 25u;
                    syncStats();
                    rc = graphRun();
                    printf("fuxi-graph: run rc=%d\n", rc);
                    if (!rc) {
                            /* The shared page of the audio service: the iOS
                             * audio server's plug-in maps it, and the
                             * periods carry the samples it writes.  It is
                             * set up before the service is published, so the
                             * plug-in never sees a half-built page. */
                            if (!setupShared())
                                printf("fuxi-audio: the shared page could not be set up\n");
                            else
                                printf("fuxi-audio: the shared page is ready (ticks/frame %llu)\n",
                                       (unsigned long long)gShared.ticks_per_frame_q32);
                            /* The service is published once the speaker path
                             * is up, so the plug-in finds it. */
                            fuxi_audio_service_publish();
                        /* The amplifier's PLL can only lock now that the
                         * endpoint's clock is running. */
                        (void)ampRereadStatus();
                        /* The unmute is the last step.  It runs only when the
                         * board asks for it: fuxi-amp=7 means "power up,
                         * wait for the lock and unmute". */
                        if (gAmpLevel >= 7u) {
                            /* The board's own init program uses the loudest
                             * index (17, which is 17.5 dB).  The first sound
                             * is made at a lower index so that a mistake in
                             * the path is heard as a quiet tone and not as a
                             * full-scale one.  fuxi-gain overrides it. */
                            unsigned gain = 8u;
                            PE_parse_boot_argn("fuxi-gain", &gain, sizeof gain);
                            (void)ampUnmute(gain);
                        }
                    }
                }
            }
        }
        native_fuxi_adsp_stats[2] = 1;
        setStage(QcomAdspStChannel);
        if (rc) {
            failStage(QcomAdspStChannel, rc);
            kprintf("FUXI_ADSP graph open failed rc=%d status=%08x close=%08x\n",
                    rc, gOwner.graph_open_reply.status, gOwner.graph_close_reply.status);
        } else {
            kprintf("FUXI_ADSP graph open ok status=0 close=0\n");
        }
        /* The stream runs here: the DSP's answers free the slots, and the
         * pump fills them again. */
        /* The period counter only advances when a slot is refilled, so it can
         * sit at the same value for many iterations. Printing on every
         * iteration in which the counter happens to be a multiple of 100
         * therefore repeated the same line many times, which flooded the CDC
         * console and starved the control channel. Report only when the
         * counter actually advanced. */
        uint32_t last_reported_periods = 0;
        for (;;) {
            serviceEvents(waitEvents(50) | kEvSend | kEvGlink);
            graphPumpPeriods(false);
            if (gOwner.period_sent != last_reported_periods &&
                !(gOwner.period_sent % 100u)) {
                last_reported_periods = gOwner.period_sent;
                printf("fuxi-graph: periods sent %u done %u in flight %u status %x\n",
                       gOwner.period_sent, gOwner.period_done, gOwner.period_in_flight,
                       gOwner.period_status);
            }
        }
    }

    rc = waitGpr();
    if (rc) { failStage(QcomAdspStChannel, rc); rollbackOwner(nullptr, 0); return; }
    native_fuxi_adsp_stats[2] = 1;
    setStage(QcomAdspStChannel);
    kprintf("FUXI_ADSP ready stage=%u smp=%08x glink=%u/%u gpr=%08x/%u\n",
            gOwner.stage, native_fuxi_adsp_stats[20], gOwner.glink.rx_messages,
            gOwner.glink.tx_messages, gOwner.spf.last_opcode, gOwner.spf.last_status);

    /* Stay event-driven so the DSP keeps its SMP2P acks and GLINK doorbell. */
    for (;;) serviceEvents(waitEvents(1000) | kEvSend);
}

uint32_t hookMask(const FuxiAdspPlatformHooks *hooks)
{
    if (!hooks) return 0;
    uint32_t mask = 0;
    if (hooks->firmware_source) mask |= FuxiAdspHookFirmware;
    if (hooks->map_carveout) mask |= FuxiAdspHookCarveout;
    if (hooks->pas_auth_reset) mask |= FuxiAdspHookPas;
    if (hooks->smp2p_setup) mask |= FuxiAdspHookSmp2p;
    if (hooks->ipcc_attach) mask |= FuxiAdspHookIpcc;
    if (hooks->qmp_load_state) mask |= FuxiAdspHookQmp;
    if (hooks->glink_setup) mask |= FuxiAdspHookGlink;
    return mask;
}

void inventoryThread(void *, wait_result_t)
{
    for (unsigned attempt = 0; attempt < 120 && !fuxi_adsp_fw_ready(); ++attempt) IOSleep(250);
    const int rc = fuxi_adsp_fw_ready() ? fuxi_adsp_inventory_firmware() : ETIMEDOUT;
    native_fuxi_adsp_stats[2] = static_cast<uint32_t>(rc ? rc : 1);
    native_fuxi_adsp_stats[10] = static_cast<uint32_t>(rc);
    kprintf("FUXI_ADSP inventory rc=%d dtb_kib=%u dtb_segments=%u adsp_kib=%u adsp_segments=%u\n",
            rc, native_fuxi_adsp_stats[12], native_fuxi_adsp_stats[13],
            native_fuxi_adsp_stats[14], native_fuxi_adsp_stats[15]);
}

} // namespace

extern "C" volatile uint32_t native_fuxi_adsp_stats[128] = {};

extern "C" int fuxi_adsp_install_platform(const FuxiAdspPlatformHooks *hooks)
{
    if (!hooks || gStarted) return -1;
    if (gHookMask == FuxiAdspHookAll) return 0;
    gHooks = *hooks;
    gHookMask = hookMask(hooks);
    native_fuxi_adsp_stats[7] = gHookMask;
    syncStats();
    return gHookMask == FuxiAdspHookAll ? 0 : 1;
}

extern "C" int fuxi_adsp_inventory_firmware(void)
{
    void *dtb = nullptr, *adsp = nullptr;
    uint64_t dtb_size = 0, adsp_size = 0;
    int rc = fuxi_adsp_fw_open("modem:adsp_dtb", &dtb, &dtb_size);
    if (!rc) {
        QcomMdtImage image = {};
        rc = qcom_mdt_parse(&image, dtb_size, firmwareRead, dtb);
        if (!rc) {
            native_fuxi_adsp_stats[12] = static_cast<uint32_t>(dtb_size >> 10);
            native_fuxi_adsp_stats[13] = image.count;
        }
        fuxi_adsp_fw_close(dtb);
    }
    if (rc) return rc;
    rc = fuxi_adsp_fw_open("modem:adsp", &adsp, &adsp_size);
    if (!rc) {
        QcomMdtImage image = {};
        rc = qcom_mdt_parse(&image, adsp_size, firmwareRead, adsp);
        if (!rc) {
            native_fuxi_adsp_stats[14] = static_cast<uint32_t>(adsp_size >> 10);
            native_fuxi_adsp_stats[15] = image.count;
        }
        fuxi_adsp_fw_close(adsp);
    }
    return rc;
}

extern "C" void native_fuxi_adsp_probe_mode(unsigned mode)
{
    if (mode == 0 || gStarted) return;
    if (mode > 2) mode = 2;
    unsigned graph_probe = 0;
    gGraphProbeLevel = mode == 2 &&
        PE_parse_boot_argn("fgraph", &graph_probe, sizeof graph_probe) && graph_probe <= 6
        ? graph_probe : 0;
    native_fuxi_adsp_stats[1] = mode;
    native_fuxi_adsp_stats[0] = kStatsAbi;

    const FuxiAudioBinding *binding = fuxi_audio_binding();
    native_fuxi_adsp_stats[3] = binding->field_count;
    native_fuxi_adsp_stats[4] = binding->state_count[FuxiAudioUnverified];
    native_fuxi_adsp_stats[5] = binding->state_count[FuxiAudioChosen];
    native_fuxi_adsp_stats[6] = binding->state_count[FuxiAudioVerified];
    native_fuxi_adsp_stats[7] = gHookMask;

    if (mode == 1) {
        gStarted = true;
        native_fuxi_adsp_stats[2] = EINPROGRESS;
        native_fuxi_adsp_stats[10] = EINPROGRESS;
        thread_t thread = nullptr;
        if (kernel_thread_start(inventoryThread, nullptr, &thread) == KERN_SUCCESS) {
            thread_deallocate(thread);
        } else {
            native_fuxi_adsp_stats[2] = ENOMEM;
            native_fuxi_adsp_stats[10] = ENOMEM;
        }
        return;
    }

    /* Install the seven owner hooks before opening the gate. A caller may
     * install its own set first; an incomplete set still refuses here and
     * leaves gStarted false so a corrected call can retry. */
    if (gHookMask != FuxiAdspHookAll) {
        const FuxiAdspPlatformHooks hooks = {
            hookFirmware, hookMapCarveout, hookPas, hookSmp2p, hookIpcc, hookQmp, hookGlink, rollbackOwner, nullptr
        };
        (void)fuxi_adsp_install_platform(&hooks);
    }
    if (gHookMask != FuxiAdspHookAll) {
        native_fuxi_adsp_stats[2] = static_cast<uint32_t>(QcomAdspBadConfig);
        native_fuxi_adsp_stats[10] = static_cast<uint32_t>(QcomAdspBadConfig);
        return;
    }

    gStarted = true;
    native_fuxi_adsp_stats[2] = EINPROGRESS;
    native_fuxi_adsp_stats[10] = 0;
    thread_t thread = nullptr;
    if (kernel_thread_start(ownerThread, nullptr, &thread) != KERN_SUCCESS) {
        native_fuxi_adsp_stats[2] = ENOMEM;
        native_fuxi_adsp_stats[10] = ENOMEM;
        return;
    }
    thread_deallocate(thread);
}
extern "C" void native_fuxi_adsp_probe(void)
{
    unsigned mode = 0;
    if (PE_parse_boot_argn("fuxi-adsp", &mode, sizeof mode)) native_fuxi_adsp_probe_mode(mode);
}

/* The audio service's entry points (package/FuxiAudioService.cpp opens them).
 *
 * The plug-in maps the three shared pages and asks for the playback and the capture to start and stop; the
 * samples themselves never pass through here (sheng's sheng_audio_memory and sheng_audio_request). */
extern "C" IOBufferMemoryDescriptor *fuxi_audio_memory(uint32_t type)
{
    if (!__atomic_load_n(&gShared.serving, __ATOMIC_ACQUIRE)) return nullptr;
    IOBufferMemoryDescriptor *m = type == QcomAudioMemoryControl ? gShared.control :
                                  type == QcomAudioMemoryOutput ? gShared.outRing :
                                  type == QcomAudioMemoryInput ? gShared.inRing : nullptr;
    if (m) m->retain();
    return m;
}

/* The plug-in's requests: the last 16, each ms since boot << 4 | selector, oldest first from [16] (the next slot). */
extern "C" {volatile uint64_t native_audio_requests[17];}
extern "C" int fuxi_audio_request(uint32_t selector)
{
    if (!__atomic_load_n(&gShared.serving, __ATOMIC_ACQUIRE)) return -1;
    const uint64_t slot = __atomic_fetch_add(&native_audio_requests[16], 1, __ATOMIC_RELAXED) % 16;
    native_audio_requests[slot] = nowMs() << 4 | (selector & 15);
    switch (selector) {
    case QcomAudioStartOutput: __atomic_or_fetch(&gShared.want, QcomAudioOutputRunning, __ATOMIC_ACQ_REL); break;
    case QcomAudioStopOutput: __atomic_and_fetch(&gShared.want, (uint32_t)~(uint32_t)QcomAudioOutputRunning, __ATOMIC_ACQ_REL); break;
    case QcomAudioStartInput: __atomic_or_fetch(&gShared.want, QcomAudioInputRunning, __ATOMIC_ACQ_REL); break;
    case QcomAudioStopInput: __atomic_and_fetch(&gShared.want, (uint32_t)~(uint32_t)QcomAudioInputRunning, __ATOMIC_ACQ_REL); break;
    default: return -1;
    }
    if (gShared.ctl)
        __atomic_or_fetch(&gShared.ctl->state, gShared.want & (QcomAudioOutputRunning | QcomAudioInputRunning), __ATOMIC_ACQ_REL);
    return 0;
}

/* Publishes the audio service the iOS audio server's plug-in opens.
 *
 * The classes are registered late: their static initializer is taken out of __mod_init_func by the build (the
 * archive builder's objcopy) and run here, between preModLoad and postModLoad, as sheng's publishService()
 * does for ShengAudioService.cpp. */
extern "C" void fuxi_audio_service_publish(void)
{
    if (!gShared.ctl) return;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {printf("fuxi-audio: preModLoad failed\n"); return;}
    native_audio_service_static_init();
    if (OSMetaClass::postModLoad(handle)) {printf("fuxi-audio: postModLoad failed\n"); return;}
    fuxi_audio_service_start();
    /* The service is up: the plug-in may map the pages now. */
    __atomic_store_n(&gShared.serving, 1u, __ATOMIC_RELEASE);
    printf("fuxi-audio: service published, state %d\n", native_audio_service[0]);
}
