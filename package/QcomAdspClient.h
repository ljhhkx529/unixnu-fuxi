/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Clients of the audio DSP, in sheng's shape: a client registers a channel by name at any time, before or after the
 * DSP is up, and gets its state and messages through callbacks that run on the DSP thread - they must not block, so
 * they copy what they need and wake their own thread. Sends are queued and go out from the DSP thread, so a caller
 * never waits for the DSP and never touches the link itself.
 *
 * The queue is what makes the non-blocking promise real: a send from an interrupt or a work loop lands in a slot and
 * returns, and the pump hands it to GLINK when there is an intent for it. A queue that is full is refused rather than
 * grown or dropped silently, which the caller sees as -1. */
#ifndef QCOM_ADSPCLIENT_H
#define QCOM_ADSPCLIENT_H

#include <stdint.h>

#include "QcomGlink.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QCOM_ADSP_MAX_MESSAGE 512u
#define QCOM_ADSP_MAX_CLIENTS 4u
#define QCOM_ADSP_QUEUE 8u

typedef struct QcomAdspChannel QcomAdspChannel;

typedef struct {
    void (*state)(void *context, int up);
    void (*receive)(void *context, const uint8_t *data, uint32_t length);
} QcomAdspClient;

/* Binds the link every channel is created on; call once, before the first channel. */
int qcom_adsp_bind(QcomGlink *link);

/* Registers a channel: intent_size/intent_count are the receive buffers offered to the DSP, initiator says whether we
 * open it (otherwise we answer the DSP's open). Returns null when out of slots or memory. */
QcomAdspChannel *qcom_adsp_channel(const char *name, uint32_t intent_size, uint32_t intent_count, int initiator,
                                   const QcomAdspClient *client, void *context);
/* Queues one message: 0 queued, -1 queue full or too big, -2 bad arguments. */
int qcom_adsp_send(QcomAdspChannel *channel, const void *data, uint32_t length);
/* The DSP thread: flushes every queue through the link and delivers what arrived. */
int qcom_adsp_pump(void);
/* Looks a channel up by name; used by the probe to find its own. */
QcomAdspChannel *qcom_adsp_find(const char *name);
/* How many messages are waiting for the DSP, and how many the channel managed to send. */
uint32_t qcom_adsp_queued(const QcomAdspChannel *channel);
uint32_t qcom_adsp_sent(const QcomAdspChannel *channel);

#ifdef __cplusplus
}
#endif

#endif
