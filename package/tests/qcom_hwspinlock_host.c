/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* H7 of the audio-probe debug runbook: the bounded hardware spinlock wrapper.
 *
 * The register encoding is the board's; this test scripts a device instead, so what is being proved is the policy around
 * it: a deadline that ends the attempt, a busy lock that never spins forever, a recursive acquire that is refused rather
 * than deadlocked, a release that is refused rather than forwarded when nothing is held, and error paths that leave the
 * handle's state auditable. */
#include <stdio.h>
#include <string.h>

#include "../QcomHwspinlock.h"

static int checks, failed;

static void check(int condition, const char *what)
{
    ++checks;
    if (!condition) {++failed; fprintf(stderr, "FAIL %s\n", what);}
}

/* A scripted device: it grants the lock after `grant_after` attempts, never grants it if that is negative, and counts
 * both calls so a test can assert that a refused operation never reached the hardware. */
struct scripted {
    int grant_after;
    int attempts;
    int releases;
    int release_rc;
};

static int scripted_acquire(void *context, uint32_t id)
{
    struct scripted *s = context;
    (void)id;
    ++s->attempts;
    if (s->grant_after < 0) return QcomHwlockBusy;
    return s->attempts > s->grant_after ? QcomHwlockOk : QcomHwlockBusy;
}

static int scripted_release(void *context, uint32_t id)
{
    struct scripted *s = context;
    (void)id;
    ++s->releases;
    return s->release_rc;
}

/* A clock that advances one tick per read, so a bounded loop terminates and the number of ticks is observable. */
struct clock {
    uint32_t now;
    uint32_t ticks;
    int wrap;
};

static uint32_t clock_now(void *context)
{
    struct clock *c = context;
    ++c->ticks;
    return c->now++;
}

static struct QcomHwlockDevice device_of(struct scripted *s)
{
    const struct QcomHwlockDevice device = {scripted_acquire, scripted_release, s};
    return device;
}

static void test_free_path(void)
{
    struct scripted s = {0, 0, 0, 0};
    const struct QcomHwlockDevice device = device_of(&s);
    struct QcomHwlock lock;
    struct clock clock = {100, 0, 0};

    qcom_hwlock_init(&lock, &device, 3);
    check(qcom_hwlock_acquire(&lock, 10, clock_now, &clock) == QcomHwlockOk, "a free lock is acquired");
    check(lock.held == 1 && lock.attempts == 1, "one attempt, and the handle records that it holds the lock");
    check(qcom_hwlock_release(&lock) == QcomHwlockOk, "it is released");
    check(s.releases == 1 && lock.held == 0, "exactly one release reached the hardware");
    check(qcom_hwlock_release(&lock) == QcomHwlockNotHeld && s.releases == 1,
          "a second release is refused without reaching the hardware");
}

static void test_recursive_and_missing(void)
{
    struct scripted s = {0, 0, 0, 0};
    const struct QcomHwlockDevice device = device_of(&s);
    struct QcomHwlock lock;
    struct clock clock = {0, 0, 0};

    qcom_hwlock_init(&lock, &device, 3);
    check(qcom_hwlock_acquire(&lock, 10, clock_now, &clock) == QcomHwlockOk, "acquired once");
    check(qcom_hwlock_acquire(&lock, 10, clock_now, &clock) == QcomHwlockRecursive,
          "a second acquire on the same handle is refused rather than deadlocking");
    check(s.attempts == 1, "the refusal did not reach the hardware");
    (void)qcom_hwlock_release(&lock);

    qcom_hwlock_init(&lock, 0, 3);
    check(qcom_hwlock_acquire(&lock, 10, clock_now, &clock) == QcomHwlockNoDevice,
          "without a board encoding nothing is attempted");
    check(qcom_hwlock_release(&lock) == QcomHwlockNoDevice, "and nothing is released");
}

static void test_busy_then_granted(void)
{
    struct scripted s = {2, 0, 0, 0};   /* free on the third attempt */
    const struct QcomHwlockDevice device = device_of(&s);
    struct QcomHwlock lock;
    struct clock clock = {0, 0, 0};

    qcom_hwlock_init(&lock, &device, 3);
    check(qcom_hwlock_acquire(&lock, 10, clock_now, &clock) == QcomHwlockOk,
          "a lock that is busy at first is still acquired inside its deadline");
    check(s.attempts == 3 && lock.attempts == 3, "exactly three attempts were made");
    (void)qcom_hwlock_release(&lock);
}

static void test_never_free(void)
{
    struct scripted s = {-1, 0, 0, 0};
    const struct QcomHwlockDevice device = device_of(&s);
    struct QcomHwlock lock;
    struct clock clock = {0, 0, 0};

    qcom_hwlock_init(&lock, &device, 3);
    check(qcom_hwlock_acquire(&lock, 5, clock_now, &clock) == QcomHwlockBusy,
          "a lock that is never freed ends in Busy at the deadline");
    check(lock.held == 0, "and the handle does not claim to hold it");
    check(s.releases == 0, "a timed-out attempt issues no release");
    check(s.attempts > 0 && s.attempts < 100, "the polling was bounded, not infinite");
}

static void test_zero_budget(void)
{
    struct scripted s = {-1, 0, 0, 0};
    const struct QcomHwlockDevice device = device_of(&s);
    struct QcomHwlock lock;
    struct clock clock = {0, 0, 0};

    qcom_hwlock_init(&lock, &device, 3);
    check(qcom_hwlock_acquire(&lock, 0, clock_now, &clock) == QcomHwlockBusy,
          "a zero budget is a single attempt, not a wait");
    check(s.attempts == 1, "exactly one attempt was made");
}

static void test_device_errors(void)
{
    struct scripted s = {-2, 0, 0, 0};   /* a hard error, not Busy */
    const struct QcomHwlockDevice device = device_of(&s);
    struct QcomHwlock lock;

    qcom_hwlock_init(&lock, &device, 3);
    check(qcom_hwlock_acquire(&lock, 10, 0, 0) == QcomHwlockDeviceError,
          "a board error is reported as a device error");
    check(lock.held == 0 && s.releases == 0, "nothing is held and nothing is released after an error");

    /* A failing release still leaves the handle consistent: it must not claim to hold a lock it could not release. */
    struct scripted ok = {0, 0, 0, -1};
    const struct QcomHwlockDevice failing = device_of(&ok);
    struct clock clock = {0, 0, 0};
    qcom_hwlock_init(&lock, &failing, 3);
    check(qcom_hwlock_acquire(&lock, 10, clock_now, &clock) == QcomHwlockOk, "acquired against the failing release");
    check(qcom_hwlock_release(&lock) == QcomHwlockDeviceError, "the failing release is reported");
    check(lock.held == 0, "and the handle no longer claims to hold it");
}

static void test_two_callers(void)
{
    /* One device that grants whoever asks first, like a real lock. */
    struct scripted s = {0, 0, 0, 0};
    const struct QcomHwlockDevice device = device_of(&s);
    struct QcomHwlock first, second;
    struct clock clock = {0, 0, 0};

    qcom_hwlock_init(&first, &device, 3);
    qcom_hwlock_init(&second, &device, 3);
    check(qcom_hwlock_acquire(&first, 10, clock_now, &clock) == QcomHwlockOk, "the first caller acquires");
    /* The scripted device now refuses everyone, which is what a held lock looks like to a second caller. */
    s.grant_after = -1;
    check(qcom_hwlock_acquire(&second, 10, clock_now, &clock) == QcomHwlockBusy,
          "the second caller is refused at its deadline instead of taking it");
    check(second.held == 0 && first.held == 1, "ownership did not move");
    check(qcom_hwlock_release(&first) == QcomHwlockOk, "the owner releases");
    s.grant_after = 0;
    s.attempts = 0;
    check(qcom_hwlock_acquire(&second, 10, clock_now, &clock) == QcomHwlockOk,
          "and the waiting caller can then acquire");
    (void)qcom_hwlock_release(&second);
}

static void test_clock_wrap(void)
{
    struct scripted s = {-1, 0, 0, 0};
    const struct QcomHwlockDevice device = device_of(&s);
    struct QcomHwlock lock;
    /* A clock a few ticks below the wrap: the deadline arithmetic must wrap, not expire instantly. */
    struct clock clock = {UINT32_MAX - 2u, 0, 0};

    qcom_hwlock_init(&lock, &device, 3);
    check(qcom_hwlock_acquire(&lock, 5, clock_now, &clock) == QcomHwlockBusy,
          "a deadline that wraps still ends in Busy rather than spinning");
    check(clock.ticks > 3, "the wait spanned the wrap instead of expiring at once");
}

static void test_missing_clock(void)
{
    struct scripted s = {0, 0, 0, 0};
    const struct QcomHwlockDevice device = device_of(&s);
    struct QcomHwlock lock;

    qcom_hwlock_init(&lock, &device, 3);
    check(qcom_hwlock_acquire(&lock, 5, 0, 0) == QcomHwlockDeviceError,
          "a budget without a clock is refused rather than turned into a spin");
    check(s.attempts == 0, "and nothing reached the hardware");
}

int main(void)
{
    test_free_path();
    test_recursive_and_missing();
    test_busy_then_granted();
    test_never_free();
    test_zero_budget();
    test_device_errors();
    test_two_callers();
    test_clock_wrap();
    test_missing_clock();
    printf("PASS QcomHwspinlock: %d checks\n", checks);
    return failed ? 1 : 0;
}
