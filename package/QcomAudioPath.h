/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The silent AudioReach data path: SPF readiness, graph open, endpoint and stream configuration, buffer map,
 * graph start, then periods written into the mapped buffer.
 *
 * One transaction is outstanding at a time and carries a token. A reply that does not match it is counted as
 * late instead of being applied to the wrong request, and a restart generation makes every reply from the
 * previous generation stale, so an ADSP restart cannot be mistaken for an answer to the new bring-up.
 *
 * Nothing here is board specific: the graph, the endpoint, the formats and the buffer arrive in
 * QcomAudioPathConfig, and the bytes travel over GLINK through QcomGpr, so the host tests run the real
 * transport with a simulated DSP on the other side. */
#ifndef QCOM_AUDIOPATH_H
#define QCOM_AUDIOPATH_H

#include <stdint.h>

#include "QcomAudioReach.h"
#include "QcomGlink.h"
#include "ShengGpr.h"

/* Periods that may be in the DSP's hands at once. The board chooses how many; one is the safe default. */
#define QCOM_AUDIOPATH_MAX_PERIODS 4u

#ifdef __cplusplus
extern "C" {
#endif

enum {
    QcomPathIdle = 0,
    QcomPathSpf = 1,        /* waiting for the framework to report ready */
    QcomPathOpen = 2,       /* graph open */
    QcomPathEndpoint = 3,   /* hardware endpoint configuration */
    QcomPathStream = 4,     /* stream module configuration */
    QcomPathMap = 5,        /* buffer map */
    QcomPathStart = 6,      /* graph start */
    QcomPathRunning = 7,
    QcomPathFailed = -1,
};

enum {
    QcomPathOk = 0,
    QcomPathAgain = 1,
    QcomPathBadConfig = -1,
    QcomPathSpfTimeout = -2,
    QcomPathSpfError = -3,
    QcomPathCommandFailed = -4,   /* the DSP answered with a non-zero status */
    QcomPathTimeout = -5,         /* no answer within the step budget */
    QcomPathMapFailed = -6,
    QcomPathWrongState = -7,
};

typedef struct {
    uint32_t gpr_port;            /* the GPR port the APM answers on */
    uint32_t token_seed;
    uint32_t timeout_steps;
    const QcomArGraph *graph;
    const QcomArEndpoint *endpoint;
    const QcomArFormat *endpoint_format;
    const QcomArFormat *stream_format;
    uint32_t stream_instance;
    uint64_t buffer_address;
    uint32_t buffer_bytes;
    uint32_t periods_in_flight;   /* 1..QCOM_AUDIOPATH_MAX_PERIODS; zero means one */
} QcomAudioPathConfig;

typedef struct {
    QcomAudioPathConfig config;
    QcomGlink *link;
    QcomGlinkChannel *channel;
    GprSpfProbe spf;
    uint32_t token, counter, generation, outstanding_opcode;
    int state, last_error, outstanding;
    uint32_t steps, waits, submitted, completed, late, unknown, timeouts, cancelled;
    uint32_t map_handle, cursor;   /* how far into the mapped buffer the next period starts */
    uint32_t stage_sent;   /* the current stage's command went out and its reply is pending */
    /* The periods the DSP has not answered yet, matched by the address each one was written to. */
    struct {uint64_t address; uint32_t bytes; uint8_t active;} period[QCOM_AUDIOPATH_MAX_PERIODS];
    uint32_t periods_in_flight;
    uint32_t tx_bytes, tx_pending;   /* an encoded frame GLINK has not taken yet */
    uint8_t apm[GPR_MAX_PACKET];   /* the APM payload the builders write */
    uint8_t tx[GPR_MAX_PACKET];    /* the GPR packet QcomGpr frames around it */
} QcomAudioPath;

void qcom_audio_path_init(QcomAudioPath *p, const QcomAudioPathConfig *config, QcomGlink *link,
                          QcomGlinkChannel *channel);
/* One step. QcomPathAgain when it is waiting for the DSP. */
int qcom_audio_path_step(QcomAudioPath *p);
/* Feeds one GLINK message, normally from the channel's receive callback. */
int qcom_audio_path_receive(QcomAudioPath *p, const uint8_t *data, uint32_t length);
/* After an ADSP restart: back to the start with a new generation, so replies from the old one are stale. */
int qcom_audio_path_restart(QcomAudioPath *p, uint32_t generation);
/* Queues one period once the path runs, up to the configured number in flight. The DSP's answer, matched by the
 * address the period was written to, is counted as a completed period; an answer for an address that is not
 * outstanding is counted as late, and a restart counts whatever was still in flight as cancelled. */
int qcom_audio_path_period(QcomAudioPath *p, uint32_t bytes);
/* How many periods the DSP still owes an answer for. */
uint32_t qcom_audio_path_in_flight(const QcomAudioPath *p);

#ifdef __cplusplus
}
#endif

#endif
