/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host model of the ADSP bring-up order: a real image, the real mailbox, SMP2P pair and GLINK transport from
 * the layers below, and an AOP simulated one register at a time. Every way the bring-up can fail is injected. */
#include <stdio.h>
#include <string.h>

#include "../QcomAdsp.h"

static int checks, failed;

static void check(int condition, const char *what)
{
    ++checks;
    if (!condition) {++failed; fprintf(stderr, "FAIL %s\n", what);}
}

static void le16(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);}
static void le32(uint8_t *p, uint32_t v) {le16(p, v & 0xffffu); le16(p + 2, v >> 16);}
static void le64(uint8_t *p, uint64_t v) {le32(p, (uint32_t)v); le32(p + 4, (uint32_t)(v >> 32));}

/* An ELF64 image: header segment at 0, two loadable segments at 0x1000 and 0x2000, a hash segment at 0x2a0. */
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

static void build_image(uint64_t second_address)
{
    memset(image_bytes, 0, sizeof image_bytes);
    image_bytes[0] = 0x7f; image_bytes[1] = 'E'; image_bytes[2] = 'L'; image_bytes[3] = 'F';
    image_bytes[4] = 2; image_bytes[5] = 1;
    le64(image_bytes + 32, PHOFF);
    le16(image_bytes + 54, PHENT);
    le16(image_bytes + 56, PHNUM);
    phdr(0, 0, 0, 0, 0, HEADERS, 0);
    phdr(1, 1, QCOM_MDT_RELOCATABLE, HEADERS, 0x1000, 0x100, 0x200);
    phdr(2, 1, QCOM_MDT_RELOCATABLE, 0x200, second_address, 0x80, 0x80);
    phdr(3, 0, QCOM_MDT_TYPE_HASH, HASH_AT, 0x3000, HASH_BYTES, HASH_BYTES);
}

static int read_image(void *context, uint64_t offset, void *to, size_t length)
{
    (void)context;
    if (offset > IMAGE_BYTES || length > IMAGE_BYTES - offset) return -1;
    memcpy(to, image_bytes + offset, length);
    return 0;
}

/* The simulated AOP mailbox. */
#define QMP_MAGIC 0x4d41494cu
static uint32_t ram[0x400 / 4];

static void mailbox_descriptor(void)
{
    memset(ram, 0, sizeof ram);
    ram[0] = QMP_MAGIC;
    ram[1] = 1;
    ram[13] = 0x100;   /* mailbox size */
    ram[14] = 0x100;   /* mailbox offset */
}

/* One turn of the simulated AOP: answer the link, offer the channel, ack ours, and take the request. */
static void aop_turn(int take_request)
{
    if (ram[9] == 0xffffu) ram[10] = 0xffffu;              /* link offered -> ack it */
    if (ram[11] == 0xffffu) ram[5] = 0xffffu;              /* channel offered -> offer ours */
    if (ram[6] == 0xffffu) ram[12] = 0xffffu;              /* our channel ack -> ack the channel */
    if (take_request && ram[0x100 / 4]) ram[0x100 / 4] = 0; /* take the load-state request */
}

/* The two SMP2P items: ours and the DSP's. */
static uint32_t smp_out[QCOM_SMP2P_ITEM_BYTES / 4];
static uint32_t smp_in[QCOM_SMP2P_ITEM_BYTES / 4];
static QcomSmp2p remote;

#define READY_BITS 1u
#define FATAL_BITS 1u

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
    (void)qcom_smp2p_update(&remote, 2, 0xffffffffu, 0u);
    ours.in = smp_in;
}

/* GLINK: our side and the peer it talks to. */
#define FIFO_BYTES 512u
static uint32_t h_ab, t_ab, h_ba, t_ba;
static uint8_t d_ab[FIFO_BYTES], d_ba[FIFO_BYTES];
/* The channels' receive storage outlives the rig; a compound literal would dangle once rig_init returned. */
static uint8_t storage_ours[2 * 256], storage_peer[2 * 256];

static void state_noop(void *c, QcomGlinkChannel *ch, int up) {(void)c; (void)ch; (void)up;}
static void recv_noop(void *c, QcomGlinkChannel *ch, const uint8_t *d, uint32_t n) {(void)c; (void)ch; (void)d; (void)n;}

/* Hooks. */
static int auth_calls, memory_calls, auth_result, memory_result;

static int hook_auth(void *context, const QcomMdtImage *image)
{
    (void)context; (void)image;
    ++auth_calls;
    if (auth_result == 2 && auth_calls < 2) return QcomAdspAgain;
    return auth_result == 2 ? QcomAdspOk : auth_result;
}
static int hook_memory(void *context, uint64_t base, uint64_t span)
{
    (void)context; (void)base; (void)span;
    ++memory_calls;
    return memory_result;
}

typedef struct {
    QcomAdsp adsp;
    QcomQmp mailbox;
    QcomSmp2p ours;
    QcomGlink link, peer;
    QcomGlinkChannel *channel, *peer_channel;
    QcomMdtImage image;
} Rig;

static void rig_init(Rig *r, const QcomAdspConfig *config, int with_hooks)
{
    build_image(0x2000);
    check(qcom_mdt_parse(&r->image, IMAGE_BYTES, read_image, 0) == QcomMdtOk, "the image parses");
    build_smp2p();
    mailbox_descriptor();
    qcom_qmp_init(&r->mailbox, ram, sizeof ram, 0, 0);
    r->ours = (QcomSmp2p){0};
    r->ours.out = smp_out;
    r->ours.in = smp_in;

    const QcomGlinkFifo o_tx = {&h_ab, &t_ab, d_ab, FIFO_BYTES};
    const QcomGlinkFifo o_rx = {&h_ba, &t_ba, d_ba, FIFO_BYTES};
    const QcomGlinkFifo p_tx = {&h_ba, &t_ba, d_ba, FIFO_BYTES};
    const QcomGlinkFifo p_rx = {&h_ab, &t_ab, d_ab, FIFO_BYTES};
    qcom_glink_init(&r->link, o_tx, o_rx, 0, 0);
    qcom_glink_init(&r->peer, p_tx, p_rx, 0, 0);
    const QcomGlinkClient client = {state_noop, recv_noop};
    r->channel = qcom_glink_channel(&r->link, "adsp_apps", 256, 2, storage_ours, &client, 0, 1);
    r->peer_channel = qcom_glink_channel(&r->peer, "adsp_apps", 256, 2, storage_peer, &client, 0, 0);

    QcomAdspHooks hooks = {0};
    if (with_hooks) {hooks.auth = hook_auth; hooks.memory = hook_memory;}
    auth_calls = memory_calls = 0;
    auth_result = memory_result = 0;
    qcom_adsp_init(&r->adsp, config, &hooks, &r->image, &r->mailbox, &r->ours, &r->link, r->channel);
}

static QcomAdspConfig base_config(void)
{
    QcomAdspConfig c = {0};
    c.carveout_base = 0x9ea00000ull;
    c.carveout_size = 0x02000000ull;
    c.state_entry = "master-kernel";
    c.ready_entry = "slave-kernel";
    c.fatal_entry = "fatal";
    c.ready_value = READY_BITS;
    c.fatal_value = FATAL_BITS;
    c.load_state_message = "adsp: on";
    c.mailbox_timeout = 24;
    c.ready_timeout = 24;
    c.link_timeout = 24;
    return c;
}

/* Drives the machine with the AOP answering, returning the final state. set_ready/set_fatal are applied once the
 * request has been taken. */
static int drive(Rig *r, int steps, int set_ready, int set_fatal, int answer)
{
    int rc = QcomAdspAgain;
    for (int i = 0; i < steps; ++i) {
        rc = qcom_adsp_step(&r->adsp);
        if (rc < 0) return rc;
        if (r->adsp.state == QcomAdspUp) return QcomAdspOk;
        if (answer) aop_turn(1);
        if (ram[0x100 / 4] == 0 && r->adsp.state >= QcomAdspReady) {
            if (set_ready) (void)qcom_smp2p_update(&remote, 1, 0xffffffffu, READY_BITS);
            if (set_fatal) (void)qcom_smp2p_update(&remote, 2, 0xffffffffu, FATAL_BITS);
            aop_turn(1);
            qcom_glink_poll(&r->peer);
        }
    }
    return rc;
}

static void test_happy(void)
{
    Rig *r = &(Rig){0};
    const QcomAdspConfig config = base_config();
    rig_init(r, &config, 1);
    check(r->channel && r->peer_channel, "the rig registers both GLINK channels");
    const int rc = drive(r, 64, 1, 0, 1);
    check(rc == QcomAdspOk, "the bring-up completes");
    check(r->adsp.state == QcomAdspUp, "the state machine is up");
    check(auth_calls == 1 && memory_calls == 1, "authentication and memory setup each ran once");

    uint64_t at = 0;
    check(qcom_adsp_segment(&r->adsp, 1, &at) == QcomAdspOk && at == 0, "the first loadable segment lands at the carveout base");
    check(qcom_adsp_segment(&r->adsp, 2, &at) == QcomAdspOk && at == 0x1000, "the second lands by its offset from the lowest");
    check(qcom_adsp_segment(&r->adsp, 3, &at) == QcomAdspWrongState, "the hash segment is not placed");
}

static void test_failures(void)
{
    /* A carveout smaller than the image span. */
    Rig *r = &(Rig){0};
    QcomAdspConfig config = base_config();
    config.carveout_size = 0x100;
    rig_init(r, &config, 1);
    check(drive(r, 8, 0, 0, 0) == QcomAdspBadImage, "a carveout smaller than the image is refused");

    /* A segment that does not fit even though the span does. */
    r = &(Rig){0};
    config = base_config();
    config.carveout_size = 0x1100;   /* span is 0x2000 after rounding, so this fails on the span */
    rig_init(r, &config, 1);
    check(drive(r, 8, 0, 0, 0) == QcomAdspBadImage, "a carveout smaller than the rounded span is refused");

    /* No hooks at all. */
    r = &(Rig){0};
    config = base_config();
    rig_init(r, &config, 0);
    check(drive(r, 8, 0, 0, 0) == QcomAdspBadConfig, "a bring-up without an authentication hook is refused");

    /* Authentication failing, and authentication needing another turn. */
    r = &(Rig){0};
    config = base_config();
    rig_init(r, &config, 1);
    auth_result = -5;
    check(drive(r, 8, 0, 0, 0) == QcomAdspAuthFailed, "a failed authentication stops the bring-up");

    r = &(Rig){0};
    rig_init(r, &config, 1);
    auth_result = 2;   /* again once, then done */
    check(drive(r, 64, 1, 0, 1) == QcomAdspOk && auth_calls >= 2, "an authentication that needs another turn is retried");

    /* Memory setup failing. */
    r = &(Rig){0};
    rig_init(r, &config, 1);
    memory_result = -6;
    check(drive(r, 16, 0, 0, 0) == QcomAdspMemoryFailed, "a failed memory setup stops the bring-up");

    /* The mailbox descriptor is wrong. */
    r = &(Rig){0};
    rig_init(r, &config, 1);
    memset(ram, 0, sizeof ram);
    check(drive(r, 16, 0, 0, 1) == QcomAdspMailboxFailed, "a mailbox without a descriptor stops the bring-up");

    /* The AOP never answers, and the DSP never reports ready. */
    r = &(Rig){0};
    rig_init(r, &config, 1);
    check(drive(r, 200, 0, 0, 0) == QcomAdspMailboxFailed, "a mailbox that never opens times out");

    r = &(Rig){0};
    rig_init(r, &config, 1);
    check(drive(r, 200, 0, 0, 1) == QcomAdspReadyTimeout, "a DSP that never reports ready times out");

    /* The DSP reports a fatal condition while it is coming up. */
    r = &(Rig){0};
    rig_init(r, &config, 1);
    check(drive(r, 200, 0, 1, 1) == QcomAdspFatal, "a fatal condition from the DSP stops the bring-up");

    /* A link that never comes up: a peer with no channel of that name never answers the open. */
    r = &(Rig){0};
    rig_init(r, &config, 1);
    {
        const QcomGlinkFifo p_tx = {&h_ba, &t_ba, d_ba, FIFO_BYTES};
        const QcomGlinkFifo p_rx = {&h_ab, &t_ab, d_ab, FIFO_BYTES};
        qcom_glink_init(&r->peer, p_tx, p_rx, 0, 0);
    }
    check(drive(r, 400, 1, 0, 1) == QcomAdspLinkFailed, "a link that never opens times out");

    /* Once failed, the state stays failed with the same reason. */
    r = &(Rig){0};
    config = base_config();
    config.carveout_size = 0x100;
    rig_init(r, &config, 1);
    check(drive(r, 8, 0, 0, 0) == QcomAdspBadImage, "the first failure is reported");
    check(qcom_adsp_step(&r->adsp) == QcomAdspBadImage && r->adsp.state == QcomAdspFailed,
          "a failed bring-up stays failed with its reason");
}

int main(void)
{
    test_happy();
    test_failures();
    printf("PASS QcomAdsp: %d checks\n", checks);
    return failed ? 1 : 0;
}
