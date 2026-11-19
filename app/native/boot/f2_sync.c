/* f2_sync.c -- libunity's thread wake-ups (Galaxy S25 30 fps, 19.11.2026).
 *
 * Unity 2018.1's job queue puts its worker threads to sleep on a POSIX semaphore (libunity+0x29d558 sem_wait) after every job and
 * the main thread wakes them with sem_post (libunity+0x29d648) thousands of times per frame.  bionic's sem_post only enters the kernel
 * (futex wake) when a thread sleeps on the semaphore, and that wake is cheap on most kernels; on the S25 (One UI 8, Android 16) it cost
 * tens of microseconds, so the main thread spent more than half of every frame inside futex wakes (simpleperf: 91 % of its cycles in
 * libc syscall, ~21 k syscalls / s, ~95 k wake-ups / s).
 *
 * Fix: a worker spins for a short while (checking the semaphore without writing to it) before it goes to sleep.  While it spins it is
 * not a futex waiter, so the main thread's sem_post is a plain atomic add.  semspin= in f2_config.txt: -1 (default) auto = spin once a
 * 5 s window shows the main thread losing more than 8 % of the time in slow sem_post calls (latched for the run), 0 never, N = always,
 * N us.  sync_log=1 writes the counters every 5 s.  uaff=0 skips libunity's own sched_setaffinity calls (direct syscall 122). */
#include "f2_boot.h"
#include <semaphore.h>
#include <stdint.h>
#include <stdatomic.h>
#include <unistd.h>
#include <errno.h>

static inline uint64_t tick(void) { uint64_t v; __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(v)); return v; }
static uint64_t s_hz;
static uint64_t hz(void) { if (!s_hz) { uint64_t v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); s_hz = v ? v : 19200000; } return s_hz; }

static _Atomic uint64_t s_post_n, s_post_slow, s_post_slow_t, s_post_t;
static _Atomic uint64_t s_wait_n, s_wait_spin_ok, s_wait_sleep, s_spin_t;
static _Atomic uint64_t s_aff_n, s_aff_t, s_fut_n, s_fut_t;
static _Atomic int s_spin_us = -1;       /* resolved spin, -1 = auto (not yet decided) */
static int s_latched;

static uint64_t us2t(int us) { return (uint64_t)us * hz() / 1000000u; }

int f2_sem_post(sem_t *s)
{
    uint64_t t0 = tick();
    int r = sem_post(s);
    uint64_t dt = tick() - t0;
    atomic_fetch_add_explicit(&s_post_n, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&s_post_t, dt, memory_order_relaxed);
    if (dt > us2t(3)) {
        atomic_fetch_add_explicit(&s_post_slow, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&s_post_slow_t, dt, memory_order_relaxed);
    }
    return r;
}

int f2_sem_wait(sem_t *s)
{
    atomic_fetch_add_explicit(&s_wait_n, 1, memory_order_relaxed);
    int us = atomic_load_explicit(&s_spin_us, memory_order_relaxed);
    if (us > 0) {
        uint64_t t0 = tick(), end = t0 + us2t(us);
        for (;;) {
            int v = 0;
            if (sem_getvalue(s, &v) == 0 && v > 0 && sem_trywait(s) == 0) {
                atomic_fetch_add_explicit(&s_wait_spin_ok, 1, memory_order_relaxed);
                atomic_fetch_add_explicit(&s_spin_t, tick() - t0, memory_order_relaxed);
                return 0;
            }
            uint64_t t = tick();
            if (t >= end) { atomic_fetch_add_explicit(&s_spin_t, t - t0, memory_order_relaxed); break; }
            for (int i = 0; i < 16; i++) __asm__ volatile("yield");
        }
    }
    atomic_fetch_add_explicit(&s_wait_sleep, 1, memory_order_relaxed);
    return sem_wait(s);
}

long f2_syscall(long n, long a, long b, long c, long d, long e, long f)
{
    if (n == 122) {                                   /* sched_setaffinity */
        atomic_fetch_add_explicit(&s_aff_n, 1, memory_order_relaxed);
        if (!f2_cfg.uaff) return 0;
    }
    uint64_t t0 = tick();
    long r = syscall(n, a, b, c, d, e, f);
    uint64_t dt = tick() - t0;
    if (n == 122) atomic_fetch_add_explicit(&s_aff_t, dt, memory_order_relaxed);
    else if (n == 98) { atomic_fetch_add_explicit(&s_fut_n, 1, memory_order_relaxed); atomic_fetch_add_explicit(&s_fut_t, dt, memory_order_relaxed); }
    return r;
}

void f2_sync_init(void)
{
    if (f2_cfg.semspin >= 0) atomic_store(&s_spin_us, f2_cfg.semspin);
    f2_log("F2: sync: worker spin before sleep %s (semspin=%d), libunity sched_setaffinity %s",
           f2_cfg.semspin < 0 ? "auto (on when sem_post wake-ups are slow)" : f2_cfg.semspin ? "forced" : "off", f2_cfg.semspin,
           f2_cfg.uaff ? "kept" : "skipped (uaff=0)");
}

void f2_sync_report(double secs)
{
    uint64_t pn = atomic_exchange(&s_post_n, 0), pt = atomic_exchange(&s_post_t, 0);
    uint64_t ps = atomic_exchange(&s_post_slow, 0), pst = atomic_exchange(&s_post_slow_t, 0);
    uint64_t wn = atomic_exchange(&s_wait_n, 0), wok = atomic_exchange(&s_wait_spin_ok, 0), wsl = atomic_exchange(&s_wait_sleep, 0);
    uint64_t st = atomic_exchange(&s_spin_t, 0);
    uint64_t an = atomic_exchange(&s_aff_n, 0), at = atomic_exchange(&s_aff_t, 0);
    uint64_t fn = atomic_exchange(&s_fut_n, 0), ft = atomic_exchange(&s_fut_t, 0);
    double ms = 1000.0 / (double)hz();
    double slow_share = secs > 0 ? pst * ms / (secs * 1000.0) : 0;
    if (f2_cfg.semspin < 0 && !s_latched && pn >= 1000 && slow_share > 0.08) {
        s_latched = 1;
        atomic_store(&s_spin_us, 50);
        f2_log("F2: sync: slow thread wake-ups (%.0f %% of the time in sem_post, %.1f us per slow call): worker spin 50 us on from now",
               slow_share * 100, ps ? pst * ms * 1000 / ps : 0);
    }
    if (f2_cfg.sync_log)
        f2_log("F2: sync: sem_post %.0f/s (avg %.2f us; slow>3us %.0f/s avg %.1f us = %.0f %% of time) | sem_wait %.0f/s: spin hit %.0f/s, "
               "slept %.0f/s, spin %.0f ms/s | setaffinity %.0f/s (%.2f ms/s) | futex %.0f/s (%.2f ms/s) | spin %d us",
               pn / secs, pn ? pt * ms * 1000 / pn : 0, ps / secs, ps ? pst * ms * 1000 / ps : 0, slow_share * 100,
               wn / secs, wok / secs, wsl / secs, st * ms / secs, an / secs, at * ms / secs, fn / secs, ft * ms / secs,
               atomic_load(&s_spin_us));
}
