/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "QcomAdsp.h"

#define DEFAULT_MAILBOX_STEPS 16u
#define DEFAULT_READY_STEPS 64u
#define DEFAULT_LINK_STEPS 32u

static uint32_t steps_or(uint32_t value, uint32_t fallback) {return value ? value : fallback;}

static int fail(QcomAdsp *a, int reason)
{
    a->last_error = reason;
    a->state = QcomAdspFailed;
    /* The stage it stopped in, as sheng records it: the log then says where, not only what. */
    if (!a->failed_stage) a->failed_stage = -(int32_t)(a->stage ? a->stage : 1u);
    return reason;
}

void qcom_adsp_stage(QcomAdsp *a, uint32_t stage)
{
    if (!a || a->state == QcomAdspFailed) return;
    if (stage > a->stage) a->stage = stage;
}

/* The two SMP2P entries this state machine reads; looked up once, then cached by index. */
static int inbound_index(const QcomAdsp *a, const char *name)
{
    if (!a->smp2p || !a->smp2p->in || !name) return -1;
    return qcom_smp2p_find_in(a->smp2p, name);
}

static int inbound_bits(QcomAdsp *a, const char *name, uint32_t mask, int *present)
{
    const int index = inbound_index(a, name);
    *present = index >= 0;
    if (index < 0) return 0;
    return (qcom_smp2p_in_value(a->smp2p, index) & mask) == mask;
}

void qcom_adsp_init(QcomAdsp *a, const QcomAdspConfig *config, const QcomAdspHooks *hooks,
                    const QcomMdtImage *image, QcomQmp *mailbox, QcomSmp2p *state, QcomGlink *link,
                    QcomGlinkChannel *channel)
{
    for (uint32_t i = 0; i < sizeof *a; ++i) ((uint8_t *)a)[i] = 0;
    if (config) a->config = *config;
    if (hooks) a->hooks = *hooks;
    a->image = image;
    a->mailbox = mailbox;
    a->state = QcomAdspIdle;
    a->link = link;
    a->channel = channel;
    /* The SMP2P object the caller passes is kept, so the DSP's entries can be read every step. */
    a->smp2p = state;
    a->last_error = QcomAdspOk;
}

int qcom_adsp_segment(const QcomAdsp *a, unsigned index, uint64_t *offset)
{
    if (!offset || index >= QCOM_MDT_MAX_SEGMENTS) return QcomAdspBadConfig;
    if (!a->segment_placed[index]) return QcomAdspWrongState;
    *offset = a->segment_offset[index];
    return QcomAdspOk;
}

static int placement(QcomAdsp *a)
{
    if (!a->image) return fail(a, QcomAdspBadConfig);
    if (!a->config.carveout_size) return fail(a, QcomAdspBadConfig);
    const uint64_t span = qcom_mdt_span(a->image);
    if (!span || span > a->config.carveout_size) return fail(a, QcomAdspBadImage);
    a->span = span;
    for (unsigned i = 0; i < a->image->count; ++i) {
        if (!a->image->segment[i].load) continue;
        uint64_t at = 0;
        const int rc = qcom_mdt_place(a->image, i, a->config.carveout_base, a->config.carveout_size, &at);
        if (rc != QcomMdtOk) return fail(a, QcomAdspBadImage);
        a->segment_offset[i] = at;
        a->segment_placed[i] = 1;
    }
    qcom_adsp_stage(a, QcomAdspStFirmware);
    a->state = QcomAdspAuth;
    return QcomAdspOk;
}

int qcom_adsp_step(QcomAdsp *a)
{
    if (!a) return QcomAdspBadConfig;
    if (a->state == QcomAdspFailed) return a->last_error;
    if (a->state == QcomAdspUp) return QcomAdspOk;
    ++a->steps;

    switch (a->state) {
    case QcomAdspIdle:
        if (!a->config.state_entry || !a->config.load_state_message) return fail(a, QcomAdspBadConfig);
        a->state = QcomAdspPlacement;
        return QcomAdspOk;
    case QcomAdspPlacement:
        return placement(a);
    case QcomAdspAuth: {
        if (!a->hooks.auth) return fail(a, QcomAdspBadConfig);
        const int rc = a->hooks.auth(a->hooks.context, a->image);
        if (rc == QcomAdspAgain) return QcomAdspAgain;
        if (rc != QcomAdspOk) return fail(a, QcomAdspAuthFailed);
        a->state = QcomAdspMemory;
        return QcomAdspOk;
    }
    case QcomAdspMemory: {
        if (!a->hooks.memory) return fail(a, QcomAdspBadConfig);
        const int rc = a->hooks.memory(a->hooks.context, a->config.carveout_base, a->span);
        if (rc == QcomAdspAgain) return QcomAdspAgain;
        if (rc != QcomAdspOk) return fail(a, QcomAdspMemoryFailed);
        a->state = QcomAdspMailbox;
        return QcomAdspOk;
    }
    case QcomAdspMailbox: {
        if (!a->mailbox) return fail(a, QcomAdspBadConfig);
        const int rc = qcom_qmp_step(a->mailbox);
        if (rc == QcomQmpBadDescriptor) return fail(a, QcomAdspMailboxFailed);
        if (rc != QcomQmpOpen) {
            if (++a->waits > steps_or(a->config.mailbox_timeout, DEFAULT_MAILBOX_STEPS))
                return fail(a, QcomAdspMailboxFailed);
            return QcomAdspAgain;
        }
        /* One request: the AOP starts the DSP on it. Reaching the ready stage means the mailbox accepted it. */
        const int sent = qcom_qmp_send(a->mailbox, a->config.load_state_message);
        if (sent != QcomQmpSending) return fail(a, QcomAdspMailboxFailed);
        qcom_adsp_stage(a, QcomAdspStLoadState);
        a->waits = 0;
        a->state = QcomAdspReady;
        return QcomAdspOk;
    }
    case QcomAdspReady: {
        int ready_present = 0, fatal_present = 0;
        const int ready = inbound_bits(a, a->config.ready_entry, a->config.ready_value, &ready_present);
        const int fatal = inbound_bits(a, a->config.fatal_entry, a->config.fatal_value, &fatal_present);
        if (fatal_present && fatal) return fail(a, QcomAdspFatal);
        if (ready_present && ready) {
            qcom_adsp_stage(a, QcomAdspStReady);
            a->waits = 0;
            a->state = QcomAdspLink;
            return QcomAdspOk;
        }
        if (++a->waits > steps_or(a->config.ready_timeout, DEFAULT_READY_STEPS)) return fail(a, QcomAdspReadyTimeout);
        return QcomAdspAgain;
    }
    case QcomAdspLink: {
        if (!a->link || !a->channel) return fail(a, QcomAdspBadConfig);
        const int rc = qcom_glink_poll(a->link);
        if (rc == QcomGlinkCorrupt) return fail(a, QcomAdspLinkFailed);
        if (a->channel->up) {
            qcom_adsp_stage(a, QcomAdspStChannel);
            a->waits = 0;
            a->state = QcomAdspUp;
            return QcomAdspOk;
        }
        qcom_adsp_stage(a, QcomAdspStGlink);
        if (++a->waits > steps_or(a->config.link_timeout, DEFAULT_LINK_STEPS)) return fail(a, QcomAdspLinkFailed);
        return QcomAdspAgain;
    }
    default:
        return fail(a, QcomAdspWrongState);
    }
}
