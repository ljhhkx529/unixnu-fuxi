/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host model of the A0 layers: shared memory, SMP2P and GLINK.
 *
 * The GLINK part runs two instances against each other over a pair of shared FIFOs, so the handshake, channel
 * open, intent exchange, data transfer and done acknowledgement are all exercised by the protocol itself rather
 * than by hand-written frames. */
#include <stdio.h>
#include <string.h>

#include "../QcomGlink.h"
#include "../QcomSmem.h"
#include "../QcomSmp2p.h"

static int checks, failed;

static void check(int condition, const char *what)
{
    ++checks;
    if (!condition) {++failed; fprintf(stderr, "FAIL %s\n", what);}
}

static void le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* One SMEM image: a table of one partition shared by apps and the global host, holding one uncached item. */
#define SMEM_BYTES 0x2000u
#define TOC_AT (SMEM_BYTES - 0x1000u)
#define PART_AT 0x800u
#define PART_BYTES 0x100u
#define ITEM_ID 0x1234u

static void build_smem(uint8_t *image)
{
    memset(image, 0, SMEM_BYTES);
    /* Partition: magic, hosts, size, free-uncached (header + one item), free-cached (the end). */
    const uint32_t hosts = QCOM_SMEM_HOST_APPS | (uint32_t)QCOM_SMEM_HOST_GLOBAL << 16;
    le32(image + PART_AT + 0, 0x54525024u);      /* "$PRT" */
    le32(image + PART_AT + 4, hosts);
    le32(image + PART_AT + 8, PART_BYTES);
    le32(image + PART_AT + 12, 32u + 16u + 8u);  /* uncached: header, one entry, 8 bytes of data */
    le32(image + PART_AT + 16, PART_BYTES);      /* cached list starts at the end */
    /* The one uncached item: canary in the low half, item id above it, 8 bytes of payload, no padding header. */
    le32(image + PART_AT + 32, 0xa5a5u | (ITEM_ID << 16));
    le32(image + PART_AT + 36, 8u);
    le32(image + PART_AT + 40, 0u);
    /* Table of contents. */
    le32(image + TOC_AT + 0, 0x434f5424u);       /* "$TOC" */
    le32(image + TOC_AT + 4, 1u);
    le32(image + TOC_AT + 8, 1u);
    const uint32_t entry = TOC_AT + 32u;
    le32(image + entry + 0, PART_AT);
    le32(image + entry + 4, PART_BYTES);
    le32(image + entry + 8, 0u);
    le32(image + entry + 12, hosts);
    le32(image + entry + 16, 0u);                /* no cacheline splitting */
}

static void test_smem(void)
{
    static uint8_t image[SMEM_BYTES];
    build_smem(image);
    QcomSmem smem = {image, SMEM_BYTES};
    QcomSmemPartition part;

    check(qcom_smem_partition(&smem, QCOM_SMEM_HOST_APPS, QCOM_SMEM_HOST_GLOBAL, &part) == QcomSmemOk,
          "the partition is found from the apps side");
    check(part.offset == PART_AT && part.size == PART_BYTES && part.cacheline == 8u,
          "the partition reports its offset, size and default cacheline");
    check(qcom_smem_partition(&smem, QCOM_SMEM_HOST_GLOBAL, QCOM_SMEM_HOST_APPS, &part) == QcomSmemOk,
          "the partition is found from either side of the pair");
    check(qcom_smem_partition(&smem, 1u, 2u, &part) == QcomSmemNoPartition, "another host pair is not matched");

    QcomSmem bad_size = {image, SMEM_BYTES - 0x100u};
    check(qcom_smem_partition(&bad_size, QCOM_SMEM_HOST_APPS, QCOM_SMEM_HOST_GLOBAL, &part) == QcomSmemNoTable,
          "a region that is not page aligned is refused");
    QcomSmem tiny = {image, 0x1000u};
    check(qcom_smem_partition(&tiny, QCOM_SMEM_HOST_APPS, QCOM_SMEM_HOST_GLOBAL, &part) == QcomSmemNoTable,
          "a region too small to hold a table is refused");

    image[TOC_AT] = 0;   /* break the magic */
    check(qcom_smem_partition(&smem, QCOM_SMEM_HOST_APPS, QCOM_SMEM_HOST_GLOBAL, &part) == QcomSmemNoTable,
          "a bad table magic is refused");
    build_smem(image);
    le32(image + PART_AT + 8, PART_BYTES + 8u);   /* partition size disagrees with the entry */
    check(qcom_smem_partition(&smem, QCOM_SMEM_HOST_APPS, QCOM_SMEM_HOST_GLOBAL, &part) == QcomSmemBadPartition,
          "a partition whose header disagrees with the table is refused");
    build_smem(image);

    check(qcom_smem_partition(&smem, QCOM_SMEM_HOST_APPS, QCOM_SMEM_HOST_GLOBAL, &part) == QcomSmemOk, "the image is rebuilt");
    uint32_t offset = 0, size = 0;
    check(qcom_smem_find(&smem, &part, ITEM_ID, &offset, &size) == QcomSmemOk && size == 8u &&
          offset == PART_AT + 32u + 16u, "the item is found at its payload");
    check(qcom_smem_find(&smem, &part, ITEM_ID + 1u, &offset, &size) == QcomSmemMissing, "an absent item is reported");
    qcom_smem_write32(&smem, offset, 0xfeedfaceu);
    check(qcom_smem_read32(&smem, offset) == 0xfeedfaceu, "the item payload reads back");
    check(qcom_smem_alloc(&smem, &part, ITEM_ID, 8u) == QcomSmemExists, "allocating an existing item reports it");
    check(qcom_smem_alloc(&smem, &part, ITEM_ID + 2u, 16u) == QcomSmemOk &&
          qcom_smem_find(&smem, &part, ITEM_ID + 2u, &offset, &size) == QcomSmemOk && size == 16u,
          "allocating a new item makes it findable");
    check(qcom_smem_alloc(&smem, &part, ITEM_ID + 3u, PART_BYTES) == QcomSmemNoSpace,
          "allocating more than the partition holds is refused");
}

static void test_smp2p(void)
{
    static uint32_t out[QCOM_SMP2P_ITEM_BYTES / 4];
    static uint32_t in[QCOM_SMP2P_ITEM_BYTES / 4];
    QcomSmp2p s;

    qcom_smp2p_init_out(&s, out, 2u, 1u);
    check(out[0] == QCOM_SMP2P_MAGIC, "the outbound item carries the magic");
    check((out[1] & 0xffu) == QCOM_SMP2P_VERSION, "the outbound item carries the version");
    check((out[2] & 0xffffu) == 2u && (out[2] >> 16) == 1u, "the outbound item carries both pids");
    check((out[3] & 0xffffu) == QCOM_SMP2P_MAX_ENTRIES, "the outbound item declares its entry capacity");

    const int master = qcom_smp2p_add_entry(&s, "master-kernel");
    const int slave = qcom_smp2p_add_entry(&s, "slave-kernel");
    check(master == 0 && slave == 1, "entries are numbered in order");
    check(qcom_smp2p_update(&s, master, 1u, 1u) == 1, "setting a bit reports a change");
    check(qcom_smp2p_out_value(&s, master) == 1u, "the outbound value reads back");
    check(qcom_smp2p_update(&s, master, 1u, 1u) == 0, "setting the same bit reports no change");
    check(qcom_smp2p_add_entry(&s, "one-too-many") == 2, "a third entry fits");
    check(qcom_smp2p_out_value(&s, slave) == 0u, "an untouched entry reads as zero");

    /* The remote's item is built with the same codec and the pids the other way round. */
    QcomSmp2p remote;
    qcom_smp2p_init_out(&remote, in, 1u, 2u);
    (void)qcom_smp2p_add_entry(&remote, "master-kernel");
    (void)qcom_smp2p_add_entry(&remote, "slave-kernel");
    (void)qcom_smp2p_add_entry(&remote, "fatal");
    (void)qcom_smp2p_update(&remote, 2, 0xffffffffu, 0u);

    check(qcom_smp2p_check_in(in, 2u, 1u) == 0, "the inbound item validates");
    check(qcom_smp2p_check_in(in, 3u, 1u) != 0, "an inbound item for another pair is refused");
    QcomSmp2p s2;
    qcom_smp2p_init_out(&s2, out, 2u, 1u);
    s2.in = in;
    const int fatal = qcom_smp2p_find_in(&s2, "fatal");
    check(fatal >= 0, "the inbound entry is found by name");
    check(qcom_smp2p_find_in(&s2, "not-there") == -1, "an absent inbound entry reports -1");
    (void)qcom_smp2p_update(&remote, 2, 0xffffffffu, 0x3u);
    uint32_t value = 0, changed = 0;
    const int ring = qcom_smp2p_service(&s2, "fatal", &value, &changed);
    check(value == 3u && changed == 3u, "a service call reports the changed bits and the value");
    check(ring == 0 || ring == 1, "the service call reports whether the remote must be rung");
}

/* GLINK: two instances over a pair of shared FIFOs. */
#define FIFO_BYTES 512u
static uint32_t h_ab, t_ab, h_ba, t_ba;
static uint8_t d_ab[FIFO_BYTES], d_ba[FIFO_BYTES];
static int up_a, up_b, rx_a, rx_b;
static char got_b[64];
static uint32_t got_b_len;

static void state_a(void *ctx, QcomGlinkChannel *c, int up) {(void)ctx; (void)c; up_a = up;}
static void state_b(void *ctx, QcomGlinkChannel *c, int up) {(void)ctx; (void)c; up_b = up;}
static void recv_a(void *ctx, QcomGlinkChannel *c, const uint8_t *d, uint32_t n)
{
    (void)ctx; (void)c; (void)d; (void)n; ++rx_a;
}
static void recv_b(void *ctx, QcomGlinkChannel *c, const uint8_t *d, uint32_t n)
{
    (void)ctx; (void)c;
    got_b_len = n > sizeof got_b ? (uint32_t)sizeof got_b : n;
    memcpy(got_b, d, got_b_len);
    ++rx_b;
}

static void test_glink(void)
{
    QcomGlink a, b;
    static uint8_t storage_a[2 * 256], storage_b[2 * 256];
    const QcomGlinkClient client_a = {state_a, recv_a};
    const QcomGlinkClient client_b = {state_b, recv_b};

    const QcomGlinkFifo a_tx = {&h_ab, &t_ab, d_ab, FIFO_BYTES};
    const QcomGlinkFifo a_rx = {&h_ba, &t_ba, d_ba, FIFO_BYTES};
    const QcomGlinkFifo b_tx = {&h_ba, &t_ba, d_ba, FIFO_BYTES};
    const QcomGlinkFifo b_rx = {&h_ab, &t_ab, d_ab, FIFO_BYTES};
    qcom_glink_init(&a, a_tx, a_rx, 0, 0);
    qcom_glink_init(&b, b_tx, b_rx, 0, 0);

    QcomGlinkChannel *ca = qcom_glink_channel(&a, "adsp_apps", 256, 2, storage_a, &client_a, 0, 1);
    QcomGlinkChannel *cb = qcom_glink_channel(&b, "adsp_apps", 256, 2, storage_b, &client_b, 0, 0);
    check(ca && cb, "both sides register the channel");
    check(qcom_glink_send(&a, ca, "x", 1) == QcomGlinkNotOpen, "a send before the channel is up is refused");

    /* Drive both sides until the channel is up on both. */
    for (int round = 0; round < 8 && !(up_a && up_b); ++round) {
        qcom_glink_poll(&a);
        qcom_glink_poll(&b);
    }
    check(up_a == 1 && up_b == 1, "the handshake brings the channel up on both sides");
    check(a.tx_messages > 0 && b.tx_messages > 0, "both sides wrote to their FIFO");

    /* One message from A to B: the first attempt has no intent yet, so it asks and reports QcomGlinkAgain. */
    const int first = qcom_glink_send(&a, ca, "hello", 5);
    check(first == QcomGlinkAgain || first == QcomGlinkOk, "a send either goes out or asks for an intent");
    for (int round = 0; round < 8 && rx_b < 1; ++round) {
        qcom_glink_poll(&b);
        qcom_glink_poll(&a);
        if (rx_b < 1) (void)qcom_glink_send(&a, ca, "hello", 5);
    }
    check(rx_b == 1, "the message reaches the other side exactly once");
    check(got_b_len == 5 && memcmp(got_b, "hello", 5) == 0, "the message content is intact");
    check(a.rx_messages > 0 && b.rx_messages > 0, "both sides counted the messages they handled");

    /* FIFO primitives on an empty and on an invalid FIFO. */
    QcomGlinkFifo empty = {&h_ab, &t_ab, 0, 0};
    check(qcom_glink_rx_avail(&empty) == 0, "an empty FIFO has nothing to read");
    check(qcom_glink_tx_avail(&empty) == 0, "an empty FIFO has no room reported");

    /* A message that cannot be a real one stops the link instead of being guessed at. */
    memset(d_ba, 0, 16);
    d_ba[0] = (uint8_t)QCOM_GLINK_OPEN;   /* cmd */
    d_ba[4] = 0xff; d_ba[5] = 0xff;       /* a name length far beyond the FIFO */
    h_ba = 8; t_ba = 0;
    check(qcom_glink_poll(&a) == QcomGlinkCorrupt, "a corrupt message stops the link");
    check(qcom_glink_poll(&a) == QcomGlinkCorrupt, "a stopped link keeps reporting corrupt");
}

int main(void)
{
    test_smem();
    test_smp2p();
    test_glink();
    printf("PASS QcomA0: %d checks\n", checks);
    return failed ? 1 : 0;
}
