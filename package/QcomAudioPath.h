/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The silent AudioReach data path: framework readiness, graph open, endpoint and stream configuration, buffer map,
 * graph start, then periods written into the mapped buffer.
 *
 * Every command is built by QcomAudioGraph, which is sheng's builder with the board taken out, so the packets are
 * the ones sheng sends and only the instances, the interface and the format differ. One transaction is outstanding
 * at a time and carries a token whose upper byte is the restart generation, so a reply from before an ADSP restart
 * can never be applied to the new bring-up. Periods are matched by the address they were written to, and their
 * window is board configuration. */
#ifndef QCOM_AUDIOPATH_H
#define QCOM_AUDIOPATH_H

#include <stdint.h>

#include "QcomAudioGraph.h"
#include "QcomGlink.h"
#include "ShengGpr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QCOM_AUDIOPATH_MAX_PERIODS 4u

enum {
    QcomPathIdle = 0,
    QcomPathSpf = 1,
    QcomPathOpen = 2,
    QcomPathEndpoint = 3,
    QcomPathStream = 4,
    QcomPathMap = 5,
    QcomPathStart = 6,
    QcomPathRunning = 7,
    QcomPathStopping = 8,   /* the teardown: stop the graph, close it, unmap the buffer */
    QcomPathFailed = -1,
};

enum {
    QcomPathOk = 0,
    QcomPathAgain = 1,
    QcomPathBadConfig = -1,
    QcomPathSpfTimeout = -2,
    QcomPathSpfError = -3,
    QcomPathCommandFailed = -4,
    QcomPathTimeout = -5,
    QcomPathMapFailed = -6,
    QcomPathWrongState = -7,
};

typedef struct {
    uint32_t gpr_port, token_seed, timeout_steps;
    uint32_t apm_iid;
    /* the graph, from the board's topology */
    const QcomArGraphSpec *graph;
    const QcomArGraphModule *modules;
    uint32_t module_count;
    const QcomArGraphLink *links;
    uint32_t link_count;
    /* the instances inside it, and where the audio leaves the DSP */
    uint32_t endpoint_instance, stream_instance, shmem_instance;
    const QcomArI2sInterface *endpoint_interface;
    const QcomArFormat *endpoint_format, *stream_format;
    /* the buffer the DSP reads from */
    uint64_t buffer_address;
    uint32_t buffer_bytes, periods_in_flight;
} QcomAudioPathConfig;

typedef struct {
    QcomAudioPathConfig config;
    QcomGlink *link;
    QcomGlinkChannel *channel;
    GprSpfProbe spf;
    uint32_t token, counter, generation, outstanding_opcode;
    int state, last_error, outstanding;
    uint32_t steps, waits, submitted, completed, late, unknown, timeouts, cancelled;
    uint32_t map_handle, cursor;
    uint32_t stopping;     /* which teardown step is in flight, zero when not stopping */
    uint32_t stage_sent, tx_bytes, tx_pending;
    struct {uint64_t address; uint32_t bytes; uint8_t active;} period[QCOM_AUDIOPATH_MAX_PERIODS];
    uint32_t periods_in_flight;
    uint8_t tx[GPR_MAX_PACKET];
} QcomAudioPath;

void qcom_audio_path_init(QcomAudioPath *p, const QcomAudioPathConfig *config, QcomGlink *link,
                          QcomGlinkChannel *channel);
int qcom_audio_path_step(QcomAudioPath *p);
int qcom_audio_path_receive(QcomAudioPath *p, const uint8_t *data, uint32_t length);
int qcom_audio_path_restart(QcomAudioPath *p, uint32_t generation);
int qcom_audio_path_period(QcomAudioPath *p, uint32_t bytes);
/* Begins the teardown: stop the graph, close it, unmap the buffer. The caller keeps stepping until the path reports
 * QcomPathIdle, so the order is the same one the acceptance record asks for and is not skipped when a stop is early. */
int qcom_audio_path_stop(QcomAudioPath *p);
uint32_t qcom_audio_path_in_flight(const QcomAudioPath *p);

#ifdef __cplusplus
}
#endif

#endif
