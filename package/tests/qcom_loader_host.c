/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host model of the two remaining A0 pieces: the AOSS QMP mailbox handshake and the RPMh proxy vote
 * (command database lookup plus the register writes of one active request in an RSC TCS). */
#include <stdio.h>
#include <string.h>

#include "../QcomQmp.h"
#include "../QcomRpmh.h"

static int checks, failed;

static void check(int condition, const char *what)
{
    ++checks;
    if (!condition) {++failed; fprintf(stderr, "FAIL %s\n", what);}
}

static void le16(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);}
static void le32(uint8_t *p, uint32_t v) {le16(p, v & 0xffffu); le16(p + 2, v >> 16);}

/* QMP: the descriptor the AOP leaves, then the four-way handshake. */
#define QMP_MAGIC 0x4d41494cu
#define D_UCORE_LINK 3
#define D_UCORE_LINK_ACK 4
#define D_UCORE_CH 5
#define D_UCORE_CH_ACK 6
#define D_MCORE_LINK 9
#define D_MCORE_LINK_ACK 10
#define D_MCORE_CH 11
#define D_MCORE_CH_ACK 12
#define D_MCORE_SIZE 13
#define D_MCORE_OFFSET 14
#define UP 0x0000ffffu

static uint32_t ram[0x400 / 4];
static int kicks;

static void kick(void *context) {(void)context; ++kicks;}

static void descriptor(uint32_t box, uint32_t size)
{
    memset(ram, 0, sizeof ram);
    ram[0] = QMP_MAGIC;
    ram[1] = 1;
    ram[D_MCORE_SIZE] = size;
    ram[D_MCORE_OFFSET] = box;
}

static void test_qmp(void)
{
    QcomQmp q;
    kicks = 0;
    qcom_qmp_init(&q, ram, sizeof ram, kick, 0);
    memset(ram, 0, sizeof ram);
    check(qcom_qmp_step(&q) == QcomQmpBadDescriptor, "a ram without the descriptor is refused");

    descriptor(0x100, 0x100);
    /* A refused descriptor is terminal, so the handshake starts from a fresh instance. */
    qcom_qmp_init(&q, ram, sizeof ram, kick, 0);
    check(qcom_qmp_step(&q) == QcomQmpLinkWait, "the link is offered and the AOP is rung");
    check(kicks == 1 && ram[D_MCORE_LINK] == UP, "the link bit is up and the kick happened");
    check(ram[D_UCORE_LINK_ACK] == ram[D_UCORE_LINK], "our link state is acknowledged in the same step");
    check(qcom_qmp_step(&q) == QcomQmpLinkWait, "the state stays until the AOP answers");

    ram[D_MCORE_LINK_ACK] = UP;
    check(qcom_qmp_step(&q) == QcomQmpChannelWait && ram[D_MCORE_CH] == UP, "the AOP's link ack moves on to the channel");
    ram[D_UCORE_CH] = UP;
    check(qcom_qmp_step(&q) == QcomQmpAckWait && ram[D_UCORE_CH_ACK] == UP, "the AOP's channel moves on to its ack");
    check(qcom_qmp_step(&q) == QcomQmpAckWait, "the state stays until the AOP acks the channel");
    ram[D_MCORE_CH_ACK] = UP;
    check(qcom_qmp_step(&q) == QcomQmpOpen, "the mailbox is open");

    /* One message: the length word, then the text padded with zeroes. */
    check(qcom_qmp_send(&q, "adsp on") == QcomQmpSending, "a message is queued");
    check(ram[0x100 / 4] == QCOM_QMP_MESSAGE, "the mailbox carries the message length");
    check(memcmp((const uint8_t *)(ram + 0x100 / 4 + 1), "adsp on", 7) == 0, "the mailbox carries the text");
    check(qcom_qmp_send(&q, "second") == QcomQmpBusy, "a second message while one is pending is refused");
    ram[0x100 / 4] = 0;
    check(qcom_qmp_step(&q) == QcomQmpOpen, "the AOP taking the message returns the mailbox to open");

    check(qcom_qmp_send(&q, "again") == QcomQmpSending, "another message is queued");
    qcom_qmp_abandon(&q);
    check(q.state == QcomQmpOpen && ram[0x100 / 4] == 0, "abandoning clears the mailbox and reopens it");

    char too_long[QCOM_QMP_MESSAGE + 8];
    memset(too_long, 'a', sizeof too_long);
    too_long[sizeof too_long - 1] = 0;
    check(qcom_qmp_send(&q, too_long) == QcomQmpTooLong, "a message that does not fit is refused");

    /* A boot stage that left the channel open is used as it is. */
    descriptor(0x100, 0x100);
    ram[D_UCORE_LINK] = UP; ram[D_MCORE_LINK] = UP; ram[D_MCORE_LINK_ACK] = UP;
    ram[D_MCORE_CH] = UP; ram[D_UCORE_CH] = UP; ram[D_MCORE_CH_ACK] = UP;
    QcomQmp q2;
    qcom_qmp_init(&q2, ram, sizeof ram, kick, 0);
    check(qcom_qmp_step(&q2) == QcomQmpOpen, "an already open channel is adopted");

    /* Descriptor refusals. */
    descriptor(0x102, 0x100);
    QcomQmp q3;
    qcom_qmp_init(&q3, ram, sizeof ram, kick, 0);
    check(qcom_qmp_step(&q3) == QcomQmpBadDescriptor, "a mailbox that is not word aligned is refused");
    descriptor(0x100, 8);
    qcom_qmp_init(&q3, ram, sizeof ram, kick, 0);
    check(qcom_qmp_step(&q3) == QcomQmpBadDescriptor, "a mailbox too small for one message is refused");
    qcom_qmp_init(&q3, ram, sizeof ram, kick, 0);
    QcomQmp idle;
    qcom_qmp_init(&idle, ram, sizeof ram, kick, 0);
    check(qcom_qmp_send(&idle, "x") == QcomQmpBusy, "a send before the mailbox is open is refused");
}

/* RPMh: the command database and the register writes of one request. */
static uint8_t db[256];

static void build_db(void)
{
    memset(db, 0, sizeof db);
    le32(db + 0, 0);                     /* version */
    le32(db + 4, QCOM_CMD_DB_MAGIC);
    /* Slave 0 record at byte 8: id, header offset, data offset, entry count, version, reserved. */
    le16(db + 8, 1);
    le16(db + 10, 0);                    /* headers at 144 */
    le16(db + 12, 24);                   /* data at 168 */
    le16(db + 14, 1);
    le16(db + 16, 1);
    /* Entry 0 at 144: an 8-byte id, two priorities, the address, the aux length and offset. */
    memcpy(db + 144, "arc_lcx", 7);
    le32(db + 144 + 8, 0); le32(db + 144 + 12, 0);
    le32(db + 144 + 16, 0x1f40000u);     /* the resource address */
    le16(db + 144 + 20, 6);              /* three u16 levels */
    le16(db + 144 + 22, 0);              /* aux data at 168 */
    le16(db + 168, 16); le16(db + 170, 32); le16(db + 172, 64);
}

static void test_rpmh(void)
{
    build_db();
    QcomRpmhResource r;
    check(qcom_cmd_db_find(db, sizeof db, "arc_lcx", &r) == QcomRpmhOk, "the resource is found in the database");
    check(r.address == 0x1f40000u && r.level_count == 3, "the resource carries its address and levels");
    check(r.levels[0] == 16 && r.levels[1] == 32 && r.levels[2] == 64, "the levels are the aux data");
    check(qcom_cmd_db_find(db, sizeof db, "arc_lmx", &r) == QcomRpmhMissing, "an absent resource is reported");
    le32(db + 4, 0);
    check(qcom_cmd_db_find(db, sizeof db, "arc_lcx", &r) == QcomRpmhNoDb, "a database without the magic is refused");
    check(qcom_cmd_db_find(db, 64, "arc_lcx", &r) == QcomRpmhNoDb, "a database shorter than its header is refused");
    build_db();
    le16(db + 144 + 22, 0x7000u);
    check(qcom_cmd_db_find(db, sizeof db, "arc_lcx", &r) == QcomRpmhCorrupt, "an aux offset outside the database is refused");
    build_db();

    uint32_t index = 0xffffffffu;
    check(qcom_rpmh_level_index(&r, 0, &index) == QcomRpmhOk && index == 0, "a zero level turns the vote off at index zero");
    check(qcom_rpmh_level_index(&r, 16, &index) == QcomRpmhOk && index == 0, "the first level is index zero");
    check(qcom_rpmh_level_index(&r, 20, &index) == QcomRpmhOk && index == 1, "a level between two picks the higher one");
    check(qcom_rpmh_level_index(&r, 1000, &index) == QcomRpmhOk && index == 2, "a level above all picks the highest");
    QcomRpmhResource empty = {0x1000u, {0}, 0};
    check(qcom_rpmh_level_index(&empty, 16, &index) == QcomRpmhNoLevel, "a resource without levels reports it");

    /* Two register layouts: the RSC's major version 3 moved the TCS registers. */
    const QcomRscLayout v3 = qcom_rsc_layout(0x01030000u);
    const QcomRscLayout v2 = qcom_rsc_layout(0x01020000u);
    check(v3.wait_for_cmpl == 0x20 && v2.wait_for_cmpl == 0x10, "the version decides where the completion register sits");
    check(v3.cmd_msgid == 0x34 && v2.cmd_msgid == 0x30, "the version decides where the command registers sit");
    check(v3.tcs_stride == v2.tcs_stride && v3.irq_clear == v2.irq_clear, "the stride and the interrupt clear are shared");

    uint32_t addresses[4] = {0x1f40000u, 0x17a20000u, 0, 0};
    uint32_t values[4] = {3u, 1u, 0, 0};
    QcomRpmhWrite w[32];
    const unsigned n = qcom_rpmh_active_request(&v3, 2, addresses, values, 2, w, 32);
    check(n == 12, "an active request of two commands is twelve writes");
    check(w[0].offset == v3.tcs_stride * 2u + v3.control && w[0].value == 0, "the control register is cleared first");
    check(w[1].offset == v3.irq_clear && w[1].value == (1u << 2), "the TCS interrupt is cleared for this TCS");
    check(w[2].offset == v3.tcs_stride * 2u + v3.cmd_msgid && w[2].value == 0x00010108u, "the command carries its message id");
    check(w[3].value == addresses[0] && w[4].value == values[0], "the first command carries the address and the value");
    check(w[5].offset == v3.tcs_stride * 2u + v3.cmd_stride + v3.cmd_msgid, "the second command follows the stride");
    check(w[8].offset == v3.tcs_stride * 2u + v3.wait_for_cmpl && w[8].value == 3u, "the completion mask covers both commands");
    check(w[9].offset == v3.tcs_stride * 2u + v3.cmd_enable && w[9].value == 3u, "the command enable covers both");
    check(w[10].value == (1u << 16), "the control is set to active mode before the trigger");
    check(w[11].offset == w[10].offset && (w[11].value & (1u << 24)), "the last write triggers the TCS");
    check(qcom_rpmh_active_request(&v3, 0, addresses, values, 5, w, 32) == 0, "more than four commands is refused");
    check(qcom_rpmh_active_request(&v3, 0, addresses, values, 0, w, 32) == 0, "an empty request is refused");
    check(qcom_rpmh_active_request(&v3, 0, addresses, values, 2, w, 8) == 0, "a write list that is too small is refused");

    check(qcom_rpmh_finish(&v3, 1, w, 32) == 3, "finishing a TCS is three writes");
    check(w[0].value == (1u << 1) && w[1].offset == v3.tcs_stride + v3.control && w[2].offset == v3.tcs_stride + v3.cmd_enable,
          "finishing clears the interrupt, the control and the enable");
    check(qcom_rpmh_finish(&v3, 1, w, 2) == 0, "finishing with a list too small is refused");
}

int main(void)
{
    test_qmp();
    test_rpmh();
    printf("PASS QcomLoader: %d checks\n", checks);
    return failed ? 1 : 0;
}
