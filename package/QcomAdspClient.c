/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "QcomAdspClient.h"

#include <string.h>

typedef struct {
    uint32_t length;
    uint8_t data[QCOM_ADSP_MAX_MESSAGE];
} QcomAdspMessage;

struct QcomAdspChannel {
    char name[QCOM_GLINK_NAME];
    uint32_t intent_size, intent_count;
    int initiator;
    QcomAdspClient client;
    void *context;
    uint8_t *storage;
    QcomGlinkChannel *glink;
    QcomAdspMessage *queue;
    uint32_t head, count;
    uint32_t sent, deferred, refused;
};

static QcomGlink *gLink;
static QcomAdspChannel gChannels[QCOM_ADSP_MAX_CLIENTS];
static QcomAdspMessage gQueues[QCOM_ADSP_MAX_CLIENTS][QCOM_ADSP_QUEUE];
static uint8_t gStorage[QCOM_ADSP_MAX_CLIENTS][4u * 512u];
static uint32_t gChannelCount;

int qcom_adsp_bind(QcomGlink *link)
{
    if (!link) return -1;
    gLink = link;
    return 0;
}

/* The GLINK callbacks carry the channel, so a message lands in the client's callback on this thread. */
static void glink_state(void *context, QcomGlinkChannel *glink, int up)
{
    (void)glink;
    QcomAdspChannel *c = context;
    if (c->client.state) c->client.state(c->context, up);
}

static void glink_receive(void *context, QcomGlinkChannel *glink, const uint8_t *data, uint32_t length)
{
    (void)glink;
    QcomAdspChannel *c = context;
    if (c->client.receive) c->client.receive(c->context, data, length);
}

QcomAdspChannel *qcom_adsp_channel(const char *name, uint32_t intent_size, uint32_t intent_count, int initiator,
                                   const QcomAdspClient *client, void *context)
{
    if (!name || !client || !gLink) return 0;
    if (gChannelCount >= QCOM_ADSP_MAX_CLIENTS) return 0;
    if (!intent_size || !intent_count || intent_count > 4u || intent_size > 512u) return 0;
    if (strlen(name) >= QCOM_GLINK_NAME) return 0;

    QcomAdspChannel *c = &gChannels[gChannelCount];
    for (uint32_t i = 0; i < sizeof *c; ++i) ((uint8_t *)c)[i] = 0;
    for (uint32_t i = 0; i < sizeof c->name - 1u && name[i]; ++i) c->name[i] = name[i];
    c->intent_size = intent_size;
    c->intent_count = intent_count;
    c->initiator = initiator;
    c->client = *client;
    c->context = context;
    c->storage = gStorage[gChannelCount];
    c->queue = gQueues[gChannelCount];
    const QcomGlinkClient hooks = {glink_state, glink_receive};
    c->glink = qcom_glink_channel(gLink, c->name, intent_size, intent_count, c->storage, &hooks, c, initiator);
    if (!c->glink) return 0;
    ++gChannelCount;
    return c;
}

QcomAdspChannel *qcom_adsp_find(const char *name)
{
    if (!name) return 0;
    for (uint32_t i = 0; i < gChannelCount; ++i)
        if (!strcmp(gChannels[i].name, name)) return &gChannels[i];
    return 0;
}

int qcom_adsp_send(QcomAdspChannel *c, const void *data, uint32_t length)
{
    if (!c || (!data && length)) return -2;
    if (!length || length > QCOM_ADSP_MAX_MESSAGE) return -1;
    if (c->count >= QCOM_ADSP_QUEUE) {++c->refused; return -1;}
    QcomAdspMessage *m = &c->queue[(c->head + c->count) % QCOM_ADSP_QUEUE];
    m->length = length;
    memcpy(m->data, data, length);
    ++c->count;
    return 0;
}

uint32_t qcom_adsp_queued(const QcomAdspChannel *c) {return c ? c->count : 0;}
uint32_t qcom_adsp_sent(const QcomAdspChannel *c) {return c ? c->sent : 0;}

int qcom_adsp_pump(void)
{
    if (!gLink) return -1;
    int rc = 0;
    for (uint32_t i = 0; i < gChannelCount; ++i) {
        QcomAdspChannel *c = &gChannels[i];
        if (!c->glink || !c->glink->up) continue;
        while (c->count) {
            QcomAdspMessage *m = &c->queue[c->head];
            const int sent = qcom_glink_send(gLink, c->glink, m->data, m->length);
            if (sent != QcomGlinkOk) {
                /* No intent or no room yet: the message stays at the head and goes out next time. */
                ++c->deferred;
                rc = 1;
                break;
            }
            c->head = (c->head + 1u) % QCOM_ADSP_QUEUE;
            --c->count;
            ++c->sent;
        }
    }
    (void)qcom_glink_poll(gLink);
    return rc;
}
