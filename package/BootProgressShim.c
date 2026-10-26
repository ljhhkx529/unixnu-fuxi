// SPDX-License-Identifier: MIT
// Minimal XNU_BOOT_PROGRESS=1 link shim for fuxi.
//
// fuxi enables CONFIG_XNU_BOOT_PROGRESS only so load_init_program() runs
// sheng_boot_watchdog_policy() before the unchanged launchd exec (the Sheng
// launchd-isolation fix). The on-screen boot-progress dashboard is not used:
// board.toml carries no boot-progress=<mode> boot-arg, so every stage hook
// below is a no-op. BootWatchdogPolicy.o is force-loaded next to this file;
// BootLogStubs.o keeps supplying the boot_progress_* data arrays.
//
// The one exception is the UFS log writer: startup.c calls boot_log_tick()
// once before IOKit is up, so on fuxi nothing retries it after IOLockGroup
// and NativeUfs are ready. This shim therefore runs a one-second retry loop,
// mirroring BootProgress.c's monitor, until boot_log_tick() starts its writer.
#include <stdint.h>

#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/sched.h>

extern void boot_log_tick(unsigned) __attribute__((weak_import));

static volatile uint32_t boot_log_retry_started;

static void boot_log_retry_thread(void *arg, wait_result_t result)
{
    (void)arg; (void)result;
    for (;;) {
        if (&boot_log_tick) boot_log_tick(0);
        assert_wait_timeout((event_t)&boot_log_retry_started, THREAD_UNINT,
                            1000, NSEC_PER_MSEC);
        thread_block(THREAD_CONTINUE_NULL);
    }
}

void boot_progress_init(void) {}
void boot_progress_start(void)
{
    thread_t thread;

    if (boot_log_retry_started) return;
    boot_log_retry_started = 1;
    if (kernel_thread_start_priority(boot_log_retry_thread, 0,
                                     BASEPRI_KERNEL, &thread) != KERN_SUCCESS) {
        boot_log_retry_started = 0;
        return;
    }
    thread_deallocate(thread);
}
int boot_progress_active(void) { return 0; }
void boot_progress_bind(uintptr_t base, unsigned width, unsigned height,
                        unsigned rowbytes, unsigned depth)
{
    (void)base; (void)width; (void)height; (void)rowbytes; (void)depth;
}
void boot_progress_putc(char ch) { (void)ch; }
void boot_progress_stage(const char *message) { (void)message; }
void boot_progress_irq(uint64_t pc, uint64_t cpsr, uint64_t sp)
{
    (void)pc; (void)cpsr; (void)sp;
}
void boot_progress_poll(void) {}
void boot_progress_user_breakpoint(void *state, unsigned kind)
{
    (void)state; (void)kind;
}
void boot_progress_user_abort(void *state, unsigned kind, uint64_t address)
{
    (void)state; (void)kind; (void)address;
}
void boot_progress_critical_exit(int pid, char *message, unsigned long length)
{
    (void)pid; (void)message; (void)length;
}
void boot_progress_process_stage(int pid, const char *name, const char *text)
{
    (void)pid; (void)name; (void)text;
}
void boot_progress_exit(int pid, const char *name, int status, unsigned ns, uint64_t code)
{
    (void)pid; (void)name; (void)status; (void)ns; (void)code;
}
