/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The TCSR mutex encoding, against the semantics of drivers/hwspinlock/qcom_hwspinlock.c.
 *
 * A register file stands in for the mutex block, so the two properties that matter can be counted rather than assumed:
 * an acquire is one write of the owner id followed by one read-back, and an acquire only succeeds when the read-back
 * returns that id. The board numbers are fuxi's, taken from its own device tree: base 0x1f40000, stride 0x1000, and SMEM
 * uses lock 3, which is the register at 0x1f43000.
 */
#include <stdio.h>
#include <string.h>

#include "../QcomTcsrHwlock.h"

static int checks, failed;

static void check(int condition, const char *what)
{
    ++checks;
    if (!condition) {++failed; fprintf(stderr, "FAIL %s\n", what);}
}

#define BLOCK_BYTES 0x20000u

struct register_file {
    uint32_t words[BLOCK_BYTES / 4];
    int reads;
    int writes;
    uint32_t last_offset;
    uint32_t last_value;
    int ignore_writes;   /* simulates losing the race: the register keeps whatever it had */
};

static uint32_t file_read(void *context, uint32_t offset)
{
    struct register_file *f = context;
    ++f->reads;
    f->last_offset = offset;
    return f->words[(offset & (BLOCK_BYTES - 1u)) / 4u];
}

static void file_write(void *context, uint32_t offset, uint32_t value)
{
    struct register_file *f = context;
    ++f->writes;
    f->last_offset = offset;
    f->last_value = value;
    if (!f->ignore_writes) f->words[(offset & (BLOCK_BYTES - 1u)) / 4u] = value;
}

static struct QcomTcsrIo io_of(struct register_file *f)
{
    struct QcomTcsrIo io = {file_read, file_write, f};
    return io;
}

static const struct QcomTcsrConfig kFuxi = {
    0x1f40000u,   /* base of the first lock, from the board's syscon property */
    0x1000u,      /* stride, from the same property and the mainline match data */
    32u,          /* QCOM_MUTEX_NUM_LOCKS */
    1u            /* QCOM_MUTEX_APPS_PROC_ID */
};

static void test_offsets(void)
{
    uint32_t offset = 0;
    check(qcom_tcsr_offset(&kFuxi, 0, &offset) == QcomTcsrOk && offset == 0x1f40000u, "lock 0 is the block base");
    check(qcom_tcsr_offset(&kFuxi, 3, &offset) == QcomTcsrOk && offset == 0x1f43000u,
          "SMEM's lock 3 is the register at 0x1f43000");
    check(qcom_tcsr_offset(&kFuxi, 31, &offset) == QcomTcsrOk && offset == 0x1f5f000u, "the last lock in the bank");
    check(qcom_tcsr_offset(&kFuxi, 32, &offset) == QcomTcsrBadId, "lock 32 is outside the bank");
    check(qcom_tcsr_offset(&kFuxi, UINT32_MAX, &offset) == QcomTcsrBadId, "a huge id cannot wrap into the block");
}

static void test_acquire_protocol(void)
{
    struct register_file f = {0};
    struct QcomTcsrIo io = io_of(&f);
    struct QcomTcsr tcsr;
    qcom_tcsr_init(&tcsr, &kFuxi, &io);

    check(qcom_tcsr_try_acquire(&tcsr, 3) == QcomHwlockOk, "a free lock is acquired");
    check(f.writes == 1 && f.last_value == 1u && f.last_offset == 0x1f43000u,
          "the acquire wrote the processor id to lock 3's register");
    check(f.reads == 1, "and read it back exactly once");
    check(f.words[0x3000u / 4u] == 1u, "the register now holds our id");

    check(qcom_tcsr_release(&tcsr, 3) == QcomHwlockOk, "it is released");
    check(f.words[0x3000u / 4u] == 0u, "the register is back to zero");
    check(f.writes == 2 && f.reads == 2, "release is one read and one write");
}

static void test_lost_race(void)
{
    struct register_file f = {0};
    struct QcomTcsrIo io = io_of(&f);
    struct QcomTcsr tcsr;
    qcom_tcsr_init(&tcsr, &kFuxi, &io);

    /* Our write lands nowhere and the register keeps another processor's id: the read-back is what tells us. */
    f.words[0x3000u / 4u] = 7u;
    f.ignore_writes = 1;
    check(qcom_tcsr_try_acquire(&tcsr, 3) == QcomHwlockBusy,
          "a write that loses the race is reported as busy, because the read-back disagrees");
}

static void test_release_of_foreign_lock(void)
{
    struct register_file f = {0};
    struct QcomTcsrIo io = io_of(&f);
    struct QcomTcsr tcsr;
    qcom_tcsr_init(&tcsr, &kFuxi, &io);

    f.words[0x3000u / 4u] = 9u;
    check(qcom_tcsr_release(&tcsr, 3) == QcomTcsrOwnerMismatch,
          "releasing a lock someone else owns is reported");
    check(f.words[0x3000u / 4u] == 0u, "and the register is cleared anyway, as the mainline driver does");
}

static void test_out_of_range_does_not_touch_registers(void)
{
    struct register_file f = {0};
    struct QcomTcsrIo io = io_of(&f);
    struct QcomTcsr tcsr;
    qcom_tcsr_init(&tcsr, &kFuxi, &io);

    check(qcom_tcsr_try_acquire(&tcsr, 32) == QcomTcsrBadId, "an id outside the bank is refused");
    check(qcom_tcsr_release(&tcsr, 40) == QcomTcsrBadId, "and so is releasing one");
    check(f.reads == 0 && f.writes == 0, "nothing reached the registers");
}

static void test_policy_over_the_encoding(void)
{
    struct register_file f = {0};
    struct QcomTcsrIo io = io_of(&f);
    struct QcomTcsr tcsr;
    qcom_tcsr_init(&tcsr, &kFuxi, &io);

    struct QcomHwlock lock;

    qcom_hwlock_init(&lock, &tcsr.device, 3);
    /* A zero budget is one attempt, which is enough because the lock is free; a busy lock would need a clock, which the
     * policy layer refuses to invent. */
    check(qcom_hwlock_acquire(&lock, 0, 0, 0) == QcomHwlockOk,
          "the policy layer acquires through the encoding with a single attempt");
    check(qcom_hwlock_acquire(&lock, 0, 0, 0) == QcomHwlockRecursive,
          "and refuses to acquire it twice without touching the registers again");
    check(f.writes == 1, "so the encoding was asked once");
    check(qcom_hwlock_release(&lock) == QcomHwlockOk, "the policy layer releases it");
    check(f.words[0x3000u / 4u] == 0u, "which clears the register");
}

int main(void)
{
    test_offsets();
    test_acquire_protocol();
    test_lost_race();
    test_release_of_foreign_lock();
    test_out_of_range_does_not_touch_registers();
    test_policy_over_the_encoding();
    printf("PASS QcomTcsrHwlock: %d checks\n", checks);
    return failed ? 1 : 0;
}
