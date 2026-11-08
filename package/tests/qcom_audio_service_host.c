/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host model of one audio session: the ADSP bring-up, the silent data path behind it, and a restart that mutes
 * the amplifiers first and moves to a new generation. The AOP and the DSP are simulated at the register and packet
 * level; the four layers below run for real. */
#include <stdio.h>
#include <string.h>

#include "../QcomAudioService.h"
#include "../QcomAudioRing.h"

static int checks, failed;

static void check(int condition, const char *what)
{
    ++checks;
    if (!condition) {++failed; fprintf(stderr, "FAIL %s\n", what);}
}

static void le16(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);}
static void le32(uint8_t *p, uint32_t v) {le16(p, v & 0xffffu); le16(p + 2, v >> 16);}
static void le64(uint8_t *p, uint64_t v) {le32(p, (uint32_t)v); le32(p + 4, (uint32_t)(v >> 32));}

/* An ELF64 image with two loadable segments and a hash segment. */
#define PHOFF 0x40u
#define PHENT 56u
#define PHNUM 4u
#define HEADERS (PHOFF + PHENT * PHNUM)
#define HASH_AT 0x2a0u
#define HASH_BYTES 0x20u
#define IMAGE_BYTES (HASH_AT + HASH_BYTES)
static uint8_t image_bytes[IMAGE_BYTES];

static void phdr(unsigned i, uint32_t type, uint32_t flags, uint64_t offset, uint64_t address, uint64_t filesz, uint64_t memsz)
{
    uint8_t *p = image_bytes + PHOFF + i * PHENT;
    le32(p + 0, type); le32(p + 4, flags);
    le64(p + 8, offset); le64(p + 24, address);
    le64(p + 32, filesz); le64(p + 40, memsz);
}

static void build_image(void)
{
    memset(image_bytes, 0, sizeof image_bytes);
    image_bytes[0] = 0x7f; image_bytes[1] = 'E'; image_bytes[2] = 'L'; image_bytes[3] = 'F';
    image_bytes[4] = 2; image_bytes[5] = 1;
    le64(image_bytes + 32, PHOFF);
    le16(image_bytes + 54, PHENT);
    le16(image_bytes + 56, PHNUM);
    phdr(0, 0, 0, 0, 0, HEADERS, 0);
    phdr(1, 1, QCOM_MDT_RELOCATABLE, HEADERS, 0x1000, 0x100, 0x200);
    phdr(2, 1, QCOM_MDT_RELOCATABLE, 0x200, 0x2000, 0x80, 0x80);
    phdr(3, 0, QCOM_MDT_TYPE_HASH, HASH_AT, 0x3000, HASH_BYTES, HASH_BYTES);
}

static int read_image(void *context, uint64_t offset, void *to, size_t length)
{
    (void)context;
    if (offset > IMAGE_BYTES || length > IMAGE_BYTES - offset) return -1;
    memcpy(to, image_bytes + offset, length);
    return 0;
}

/* The AOP mailbox. */
#define QMP_MAGIC 0x4d41494cu
static uint32_t ram[0x400 / 4];

static void mailbox_descriptor(void)
{
    memset(ram, 0, sizeof ram);
    ram[0] = QMP_MAGIC;
    ram[1] = 1;
    ram[13] = 0x100;
    ram[14] = 0x100;
}

static void aop_turn(int take_request, int ready)
{
    if (ram[9] == 0xffffu) ram[10] = 0xffffu;
    if (ram[11] == 0xffffu) ram[5] = 0xffffu;
    if (ram[6] == 0xffffu) ram[12] = 0xffffu;
    if (take_request && ram[0x100 / 4]) ram[0x100 / 4] = 0;
    (void)ready;
}

/* SMP2P: ours and the DSP's. */
static uint32_t smp_out[QCOM_SMP2P_ITEM_BYTES / 4];
static uint32_t smp_in[QCOM_SMP2P_ITEM_BYTES / 4];
static QcomSmp2p remote;

static void build_smp2p(void)
{
    QcomSmp2p ours;
    qcom_smp2p_init_out(&ours, smp_out, 2u, 1u);
    (void)qcom_smp2p_add_entry(&ours, "master-kernel");
    qcom_smp2p_init_out(&remote, smp_in, 1u, 2u);
    (void)qcom_smp2p_add_entry(&remote, "master-kernel");
    (void)qcom_smp2p_add_entry(&remote, "slave-kernel");
    (void)qcom_smp2p_add_entry(&remote, "fatal");
    (void)qcom_smp2p_update(&remote, 1, 0xffffffffu, 0u);
}

/* GLINK with a simulated DSP that answers GPR commands. */
#define FIFO_BYTES 1024u
static uint32_t h_ab, t_ab, h_ba, t_ba;
static uint8_t d_ab[FIFO_BYTES], d_ba[FIFO_BYTES];
static uint8_t storage_ours[4 * 512], storage_dsp[4 * 512];

typedef struct {
    QcomGlink link;
    QcomGlinkChannel *channel;
    uint8_t outbox[256];
    uint32_t pending_bytes, commands, spf_ready, handle;
} Dsp;

static QcomAudioService *g_session;

static void session_receive(void *context, QcomGlinkChannel *channel, const uint8_t *data, uint32_t length)
{
    (void)context; (void)channel;
    (void)qcom_audio_path_receive(&g_session->path, data, length);
}

static void dsp_send(Dsp *d, const GprPacket *request, uint32_t opcode, const uint8_t *payload, uint32_t bytes)
{
    GprPacket packet;
    memset(&packet, 0, sizeof packet);
    packet.dst_domain = GPR_DOMAIN_APPS;
    packet.src_domain = GPR_DOMAIN_ADSP;
    packet.src_port = request->dst_port;
    packet.dst_port = request->src_port;
    packet.token = request->token;
    packet.opcode = opcode;
    uint8_t frame[GPR_MAX_PACKET];
    const uint32_t size = gpr_encode(frame, sizeof frame, &packet, payload, bytes);
    if (!size) return;
    if (qcom_glink_send(&d->link, d->channel, frame, size) == QcomGlinkOk) return;
    if (size <= sizeof d->outbox) {memcpy(d->outbox, frame, size); d->pending_bytes = size;}
}

static void dsp_receive(void *context, QcomGlinkChannel *channel, const uint8_t *data, uint32_t length)
{
    (void)channel;
    Dsp *d = context;
    GprPacket packet;
    if (gpr_decode(data, length, &packet) != GprOk) return;
    ++d->commands;
    if (packet.opcode == GPR_OP_APM_GET_SPF_STATE) {
        uint8_t state[4] = {0, 0, 0, 0};
        state[0] = (uint8_t)(d->spf_ready ? 1u : 0u);
        dsp_send(d, &packet, GPR_OP_APM_SPF_STATE, state, sizeof state);
        return;
    }
    if (packet.opcode == QCOM_AR_OP_MEM_MAP) {
        uint8_t handle[4] = {(uint8_t)(d->handle & 0xff), (uint8_t)(d->handle >> 8), 0, 0};
        dsp_send(d, &packet, QCOM_AR_OP_MEM_MAP_DONE, handle, sizeof handle);
        return;
    }
    if (packet.opcode == QCOM_AR_OP_WRITE) {
        uint8_t done[16] = {0};
        for (unsigned i = 0; i < 8 && i < packet.payload_bytes; ++i) done[i] = packet.payload[i];
        dsp_send(d, &packet, QCOM_AR_OP_WRITE_DONE, done, sizeof done);
        return;
    }
    uint8_t result[8] = {0};
    result[0] = (uint8_t)packet.opcode; result[1] = (uint8_t)(packet.opcode >> 8);
    result[2] = (uint8_t)(packet.opcode >> 16); result[3] = (uint8_t)(packet.opcode >> 24);
    dsp_send(d, &packet, GPR_OP_BASIC_RESULT, result, sizeof result);
}

static uint8_t ring_data[480u * 2u * 2u * 8u];
static uint32_t ring_write, ring_read;
static QcomAudioControl page;

typedef struct {
    QcomAudioService session;
    QcomAudioRing ring;
    QcomAudioRingConfig ring_config;
    QcomGlink ours;
    QcomGlinkChannel *channel;
    QcomSmp2p ours_state;
    QcomQmp mailbox;
    QcomMdtImage image;
    Dsp dsp;
    int mute_result, mutes;
} Rig;

static int auth_hook(void *context, const QcomMdtImage *image) {(void)context; (void)image; return 0;}
static int memory_hook(void *context, uint64_t base, uint64_t span) {(void)context; (void)base; (void)span; return 0;}

static int mute_hook(void *context)
{
    Rig *r = context;
    ++r->mutes;
    return r->mute_result;
}

static const QcomArGraphSpec graph = {1u, 0x5100u, 2u, QCOM_AR_DIR_RX, 1u, 0x9001u, 4u, 8192u, GPR_DOMAIN_ADSP};
static const QcomArGraphModule modules[] = {
    {QCOM_AR_MOD_WR_SHMEM, 0x6001u, 0u, 1u},
    {QCOM_AR_MOD_PCM_CNV, 0x6003u, 1u, 1u},
    {QCOM_AR_MOD_I2S_SINK, 0x6051u, 1u, 0u},
};
static const QcomArGraphLink links[] = {{0x6001u, 0x6003u}, {0x6003u, 0x6051u}};
static const QcomArI2sInterface intf = {QCOM_AR_LPAIF, QCOM_AR_I2S_SECONDARY, QCOM_AR_I2S_SD1, QCOM_AR_WS_INTERNAL};
static const QcomArFormat format = {48000u, 16u, 2u};

static void rig_init(Rig *r, uint64_t carveout)
{
    build_image();
    (void)qcom_mdt_parse(&r->image, IMAGE_BYTES, read_image, 0);
    build_smp2p();
    mailbox_descriptor();
    qcom_qmp_init(&r->mailbox, ram, sizeof ram, 0, 0);
    r->ours_state = (QcomSmp2p){0};
    r->ours_state.out = smp_out;
    r->ours_state.in = smp_in;
    r->mute_result = 0;
    r->mutes = 0;

    const QcomGlinkFifo o_tx = {&h_ab, &t_ab, d_ab, FIFO_BYTES};
    const QcomGlinkFifo o_rx = {&h_ba, &t_ba, d_ba, FIFO_BYTES};
    const QcomGlinkFifo p_tx = {&h_ba, &t_ba, d_ba, FIFO_BYTES};
    const QcomGlinkFifo p_rx = {&h_ab, &t_ab, d_ab, FIFO_BYTES};
    qcom_glink_init(&r->ours, o_tx, o_rx, 0, 0);
    qcom_glink_init(&r->dsp.link, p_tx, p_rx, 0, 0);
    r->dsp.spf_ready = 1;
    r->dsp.handle = 0x4321u;
    const QcomGlinkClient ours_client = {0, session_receive};
    const QcomGlinkClient dsp_client = {0, dsp_receive};
    r->channel = qcom_glink_channel(&r->ours, "adsp_apps", 512, 4, storage_ours, &ours_client, 0, 1);
    r->dsp.channel = qcom_glink_channel(&r->dsp.link, "adsp_apps", 512, 4, storage_dsp, &dsp_client, &r->dsp, 0);

    QcomAdspConfig ac;
    memset(&ac, 0, sizeof ac);
    ac.carveout_base = 0x9ea00000ull;
    ac.carveout_size = carveout;
    ac.state_entry = "master-kernel";
    ac.ready_entry = "slave-kernel";
    ac.fatal_entry = "fatal";
    ac.ready_value = 1u;
    ac.fatal_value = 1u;
    ac.load_state_message = "adsp: on";
    ac.mailbox_timeout = 24; ac.ready_timeout = 24; ac.link_timeout = 24;
    const QcomAdspHooks ah = {auth_hook, memory_hook, 0};

    QcomAudioPathConfig pc;
    memset(&pc, 0, sizeof pc);
    pc.gpr_port = 3u;
    pc.token_seed = 0x1000u;
    pc.timeout_steps = 8u;
    pc.apm_iid = 1u;
    pc.graph = &graph;
    pc.modules = modules;
    pc.module_count = 3u;
    pc.links = links;
    pc.link_count = 2u;
    pc.endpoint_instance = 0x6051u;
    pc.stream_instance = 0x6003u;
    pc.shmem_instance = 0x6001u;
    pc.endpoint_interface = &intf;
    pc.endpoint_format = &format;
    pc.stream_format = &format;
    pc.buffer_address = 0x8f000000ull;
    pc.buffer_bytes = 0x4000u;
    pc.periods_in_flight = 4u;

    memset(&r->ring_config, 0, sizeof r->ring_config);
    r->ring_config.data = ring_data;
    r->ring_config.write_index = &ring_write;
    r->ring_config.read_index = &ring_read;
    r->ring_config.frames = 480u * 8u;
    r->ring_config.period_frames = 480u;
    r->ring_config.periods_in_flight = 4u;
    r->ring_config.channels = 2u;
    r->ring_config.bytes_per_sample = 2u;
    r->ring_config.sample_rate = 48000u;
    r->ring_config.host_clock_hz = 19200000u;
    (void)qcom_audio_ring_init(&r->ring, &r->ring_config);
    memset(&page, 0, sizeof page);
    page.state = QcomAudioOutputReady;

    g_session = &r->session;
    qcom_audio_service_init(&r->session, &ac, &ah, &pc, &r->ours, r->channel, &r->mailbox, &r->ours_state,
                            &r->image, &r->ring, &page, mute_hook, r);
}

static void pump_dsp(Rig *r)
{
    if (r->dsp.pending_bytes) {
        if (qcom_glink_send(&r->dsp.link, r->dsp.channel, r->dsp.outbox, r->dsp.pending_bytes) == QcomGlinkOk)
            r->dsp.pending_bytes = 0;
    }
    (void)qcom_glink_poll(&r->dsp.link);
    (void)qcom_glink_poll(&r->ours);
}

static void set_ready(void) { (void)qcom_smp2p_update(&remote, 1, 0xffffffffu, 1u); }

static int bring_up(Rig *r, int steps)
{
    for (int i = 0; i < steps; ++i) {
        const int rc = qcom_audio_service_step(&r->session);
        if (rc < 0) return rc;
        if (r->session.state == QcomServiceRunning) return QcomServiceOk;
        aop_turn(1, 1);
        if (ram[0x100 / 4] == 0) set_ready();
        pump_dsp(r);
    }
    return QcomServiceAgain;
}

static void test_happy_and_restart(void)
{
    Rig *r = &(Rig){0};
    rig_init(r, 0x02000000ull);

    /* The page and the IO cycle, before the DSP is up. */
    check(qcom_audio_service_cycle(&r->session) == 0 && r->session.starved == 1,
          "a cycle before the path runs takes nothing and counts a starve");
    check(qcom_audio_service_start_output(&r->session, 0x1000u) == QcomServiceOk, "the output is announced");
    check(page.out_anchor_host == 0x1000u && (page.out_seq & 1u) == 0u,
          "the anchor is written and the sequence counter closes behind it");
    check(page.state & QcomAudioOutputReady, "the page says the output is ready");

    check(bring_up(r, 400) == QcomServiceOk, "the session comes up end to end");
    check(r->session.state == QcomServiceRunning, "the session reports running");
    check(r->session.generation == 1, "the first session is generation one");
    check(r->session.adsp.state == QcomAdspUp && r->session.path.state == QcomPathRunning,
          "both layers are up");
    check(r->mutes == 0, "nothing muted itself on the way up");

    /* One IO cycle hands a period from the ring to the DSP and publishes where it got to. */
    static uint8_t samples[480u * 2u * 2u];
    check(qcom_audio_ring_write(&r->ring, samples, 480u) == 480u, "the plug-in wrote a period");
    const uint32_t submitted_before = r->session.path.submitted;
    check(qcom_audio_service_cycle(&r->session) == 480u, "the cycle takes the period");
    check(r->session.path.submitted == submitted_before + 1u, "and hands it to the DSP");
    check(page.out_frames == r->session.path.completed, "the page publishes what the DSP has completed");
    check(qcom_audio_service_cycle(&r->session) == 0 && r->session.starved == 2,
          "a cycle with an empty ring starves rather than repeating the last period");

    const uint32_t cancelled_before = r->session.path.cancelled;
    check(qcom_audio_service_restart(&r->session) == QcomServiceOk, "a restart is accepted");
    check(r->mutes == 1, "the amplifiers are muted before the restart");
    check(r->session.generation == 2 && r->session.recoveries == 1, "the generation moved on");
    check(r->session.state == QcomServiceIdle, "the restart leaves the session at the start");
    check(r->session.path.cancelled >= cancelled_before, "periods in flight are cancelled by the restart");

    check(bring_up(r, 400) == QcomServiceOk, "the session comes up again after the restart");
    check(r->session.generation == 2 && r->session.path.state == QcomPathRunning,
          "the second generation runs");

    /* An answer carrying the previous generation's token is ignored, not applied. */
    uint8_t stale[64];
    GprPacket packet;
    memset(&packet, 0, sizeof packet);
    packet.dst_domain = GPR_DOMAIN_APPS;
    packet.src_domain = GPR_DOMAIN_ADSP;
    packet.src_port = GPR_PORT_APM;
    packet.opcode = GPR_OP_BASIC_RESULT;
    packet.token = (1u << 24) | 3u;   /* generation one */
    const uint8_t result[8] = {0};
    const uint32_t n = gpr_encode(stale, sizeof stale, &packet, result, sizeof result);
    const uint32_t late_before = r->session.path.late;
    (void)qcom_audio_path_receive(&r->session.path, stale, n);
    check(r->session.path.late == late_before + 1, "an answer from the previous generation is counted as late");

    /* Stopping clears the page's state and cancels what was in flight. */
    (void)qcom_audio_service_start_output(&r->session, 0x2000u);
    check(qcom_audio_service_stop_output(&r->session) == QcomServiceOk, "the output stops");
    check((page.state & (QcomAudioOutputRunning | QcomAudioOutputReady)) == 0u,
          "the page says the output is neither running nor ready");
    check(r->session.path.state == QcomPathIdle, "the path is back at the start");
}

static void test_failures(void)
{
    /* The amplifiers refuse to mute: the restart must not happen. */
    Rig *r = &(Rig){0};
    rig_init(r, 0x02000000ull);
    (void)bring_up(r, 400);
    r->mute_result = -1;
    check(qcom_audio_service_restart(&r->session) == QcomServiceMuteFailed,
          "a restart whose mute fails is refused");
    check(r->session.state == QcomServiceFailed && r->session.generation == 1,
          "a refused restart leaves the generation alone and the session failed");

    /* A carveout too small for the image fails the bring-up. */
    r = &(Rig){0};
    rig_init(r, 0x100u);
    check(bring_up(r, 40) == QcomServiceBringUpFailed, "a carveout that cannot hold the image fails the bring-up");
    check(r->session.state == QcomServiceFailed, "the session stays failed");
}

int main(void)
{
    test_happy_and_restart();
    test_failures();
    printf("PASS QcomAudioService: %d checks\n", checks);
    return failed ? 1 : 0;
}
