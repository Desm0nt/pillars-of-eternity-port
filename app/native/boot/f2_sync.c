/* f2_sync.c -- libunity's thread wake-ups (Galaxy S25 30 fps, 19.11.2026).
 *
 * Unity 2018.1's job queue puts its worker threads to sleep on a POSIX semaphore (libunity+0x29d558 sem_wait) after every job and
 * the main thread wakes them with sem_post (libunity+0x29d648) thousands of times per frame.  bionic's sem_post only enters the kernel
 * (futex wake) when a thread sleeps on the semaphore; that wake is cheap on most kernels, but on the S25 (One UI, Android 16) the main
 * thread spent more than half of every frame inside it (simpleperf: 91 % of its cycles in libc syscall, ~21 k syscalls / s,
 * ~95 k wake-ups / s, ~3400 per frame).
 *
 * Remedies (all in libunity's sem_wait, so sem_post finds no sleeper and stays a plain atomic add):
 *   1 / 2  spin 50 / 200 us (sem_getvalue + trywait, no writes while spinning) before sleeping;
 *   3      poll: spin 50 us, then check every 250 us for up to 20 ms, then sleep (an idle game still sleeps);
 *   4      spin 200 us and skip libunity's own sched_setaffinity calls (raw syscall 122; only tried when it calls them).
 * semspin= in f2_config.txt: -1 (default) auto, 0 never, N = spin N us from the start.
 * AUTO: when a 5 s window shows the main thread losing more than 8 % of the time in slow sem_post calls, the remedies are tried in
 * turn (10 s each, two rounds, the first 5 s of each slot not counted), the best fps is kept and remembered in f2_sync_choice.txt,
 * which the next start applies at once.  Every window is logged ("F2: sync:"), so a player's log shows which remedy helped. */
#include "f2_boot.h"
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

static inline uint64_t tick(void) { uint64_t v; __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(v)); return v; }
static uint64_t s_hz;
static uint64_t hz(void) { if (!s_hz) { uint64_t v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); s_hz = v ? v : 19200000; } return s_hz; }
static uint64_t us2t(unsigned us) { return (uint64_t)us * hz() / 1000000u; }

typedef struct { const char *name; unsigned spin_us; unsigned poll; int uaff; } remedy_t;
static const remedy_t R[] = {
    { "none", 0, 0, 1 },
    { "spin 50 us", 50, 0, 1 },
    { "spin 200 us", 200, 0, 1 },
    { "poll (spin 50 us, then every 250 us up to 20 ms)", 50, 1, 1 },
    { "spin 200 us + no libunity setaffinity", 200, 0, 0 },
};
#define NR ((int)(sizeof R / sizeof R[0]))
static remedy_t s_forced;                           /* semspin=N */
static _Atomic(const remedy_t *) s_cur = &R[0];

static _Atomic uint64_t s_post_n, s_post_slow, s_post_slow_t, s_post_t;
static _Atomic uint64_t s_wait_n, s_wait_spin_ok, s_wait_poll_ok, s_wait_sleep, s_spin_t;
static _Atomic uint64_t s_aff_n, s_aff_t, s_fut_n, s_fut_t;

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

static int try_take(sem_t *s) { int v = 0; return sem_getvalue(s, &v) == 0 && v > 0 && sem_trywait(s) == 0; }

int f2_sem_wait(sem_t *s)
{
    const remedy_t *m = atomic_load_explicit(&s_cur, memory_order_relaxed);
    atomic_fetch_add_explicit(&s_wait_n, 1, memory_order_relaxed);
    if (m->spin_us) {
        uint64_t t0 = tick(), end = t0 + us2t(m->spin_us);
        for (;;) {
            if (try_take(s)) {
                atomic_fetch_add_explicit(&s_wait_spin_ok, 1, memory_order_relaxed);
                atomic_fetch_add_explicit(&s_spin_t, tick() - t0, memory_order_relaxed);
                return 0;
            }
            uint64_t t = tick();
            if (t >= end) { atomic_fetch_add_explicit(&s_spin_t, t - t0, memory_order_relaxed); break; }
            for (int i = 0; i < 16; i++) __asm__ volatile("yield");
        }
        if (m->poll) {
            uint64_t pend = tick() + us2t(20000);
            while (tick() < pend) {
                usleep(250);
                if (try_take(s)) { atomic_fetch_add_explicit(&s_wait_poll_ok, 1, memory_order_relaxed); return 0; }
            }
        }
    }
    atomic_fetch_add_explicit(&s_wait_sleep, 1, memory_order_relaxed);
    return sem_wait(s);
}

long f2_syscall(long n, long a, long b, long c, long d, long e, long f)
{
    if (n == 122) {                                   /* sched_setaffinity */
        atomic_fetch_add_explicit(&s_aff_n, 1, memory_order_relaxed);
        if (!f2_cfg.uaff || !atomic_load_explicit(&s_cur, memory_order_relaxed)->uaff) return 0;
    }
    uint64_t t0 = tick();
    long r = syscall(n, a, b, c, d, e, f);
    uint64_t dt = tick() - t0;
    if (n == 122) atomic_fetch_add_explicit(&s_aff_t, dt, memory_order_relaxed);
    else if (n == 98) { atomic_fetch_add_explicit(&s_fut_n, 1, memory_order_relaxed); atomic_fetch_add_explicit(&s_fut_t, dt, memory_order_relaxed); }
    return r;
}

/* ------------------------------------------------------------------ auto choice */
static char s_choice_path[600];
static int s_saved = -1;                 /* remedy remembered from an earlier run, -1 none */
static int s_phase;                      /* 0 watching, 1 trying remedies, 2 settled */
static int s_slot, s_win;                /* trial slot (0 .. 2*NR-1) and window inside it */
static double s_fps_sum[NR]; static int s_fps_n[NR];
static double s_aff_rate;

static void set_remedy(int i, const char *why)
{
    atomic_store(&s_cur, &R[i]);
    f2_log("F2: sync: remedy -> %s (%s)", R[i].name, why);
}

void f2_sync_init(void)
{
    snprintf(s_choice_path, sizeof s_choice_path, "%s/f2_sync_choice.txt", f2_cfg.files_dir);
    if (f2_cfg.semspin > 0) {
        s_forced = (remedy_t){ "forced spin (semspin=)", (unsigned)f2_cfg.semspin, 0, 1 };
        atomic_store(&s_cur, &s_forced);
        s_phase = 2;
    } else if (f2_cfg.semspin == 0) s_phase = 2;
    FILE *fp = fopen(s_choice_path, "r");
    if (fp) { int i = -1; if (fscanf(fp, "remedy=%d", &i) == 1 && i >= 0 && i < NR) s_saved = i; fclose(fp); }
    f2_log("F2: sync: semspin=%d (%s), remembered remedy %s, libunity sched_setaffinity %s", f2_cfg.semspin,
           f2_cfg.semspin < 0 ? "auto: remedies are tried when thread wake-ups turn out slow" : f2_cfg.semspin ? "forced spin" : "off",
           s_saved >= 0 ? R[s_saved].name : "none", f2_cfg.uaff ? "kept" : "skipped (uaff=0)");
}

static void settle(void)
{
    static const int cost_order[NR] = { 0, 1, 2, 4, 3 };   /* cheapest first: none, spin 50, spin 200, spin 200 + no affinity, poll */
    double fmax = -1; char line[512]; int o = 0;
    for (int i = 0; i < NR; i++) {
        if (!s_fps_n[i]) continue;
        double f = s_fps_sum[i] / s_fps_n[i];
        o += snprintf(line + o, sizeof line - o, "%s%s %.1f", o ? ", " : "", R[i].name, f);
        if (f > fmax) fmax = f;
    }
    int best = 0; double bf = 0;
    for (int k = 0; k < NR; k++) {                             /* the cheapest remedy within 1 fps of the best */
        int i = cost_order[k];
        if (s_fps_n[i] && s_fps_sum[i] / s_fps_n[i] >= fmax - 1.0) { best = i; bf = s_fps_sum[i] / s_fps_n[i]; break; }
    }
    f2_log("F2: sync: trial done, average fps per remedy: %s", line);
    set_remedy(best, "best fps of the trial, kept for this run and the next starts");
    FILE *fp = fopen(s_choice_path, "w");
    if (fp) { fprintf(fp, "remedy=%d\n# %s, %.1f fps in the trial\n", best, R[best].name, bf); fclose(fp); }
    s_phase = 2;
}

void f2_sync_report(double secs, double fps)
{
    uint64_t pn = atomic_exchange(&s_post_n, 0), pt = atomic_exchange(&s_post_t, 0);
    uint64_t ps = atomic_exchange(&s_post_slow, 0), pst = atomic_exchange(&s_post_slow_t, 0);
    uint64_t wn = atomic_exchange(&s_wait_n, 0), wok = atomic_exchange(&s_wait_spin_ok, 0), wpo = atomic_exchange(&s_wait_poll_ok, 0);
    uint64_t wsl = atomic_exchange(&s_wait_sleep, 0), st = atomic_exchange(&s_spin_t, 0);
    uint64_t an = atomic_exchange(&s_aff_n, 0), at = atomic_exchange(&s_aff_t, 0);
    uint64_t fn = atomic_exchange(&s_fut_n, 0), ft = atomic_exchange(&s_fut_t, 0);
    if (secs <= 0) return;
    double ms = 1000.0 / (double)hz();
    double slow_share = pst * ms / (secs * 1000.0);
    s_aff_rate = an / secs;
    const remedy_t *cur = atomic_load(&s_cur);
    if (f2_cfg.sync_log)
        f2_log("F2: sync: %.1f fps | sem_post %.0f/s (avg %.2f us; slow>3us %.0f/s avg %.1f us = %.0f %% of the time) | sem_wait %.0f/s: "
               "spin hit %.0f/s, poll hit %.0f/s, slept %.0f/s, spin %.0f ms/s | setaffinity %.0f/s (%.2f ms/s) | futex %.0f/s (%.2f ms/s) | %s",
               fps, pn / secs, pn ? pt * ms * 1000 / pn : 0, ps / secs, ps ? pst * ms * 1000 / ps : 0, slow_share * 100,
               wn / secs, wok / secs, wpo / secs, wsl / secs, st * ms / secs, an / secs, at * ms / secs, fn / secs, ft * ms / secs, cur->name);
    if (f2_cfg.semspin >= 0) return;
    if (s_phase == 0) {
        if (!f2_cfg.sync_trial && (pn < 1000 || slow_share < 0.08)) return;
        f2_log("F2: sync: slow thread wake-ups: %.0f %% of the time in sem_post, %.1f us per slow call, %.1f fps", slow_share * 100,
               ps ? pst * ms * 1000 / ps : 0, fps);
        if (s_saved >= 0 && !f2_cfg.sync_trial) { set_remedy(s_saved, "remembered from an earlier run"); s_phase = 2; return; }
        s_phase = 1; s_slot = 0; s_win = 0;
        set_remedy(1 % NR, "trial 1");
        return;
    }
    if (s_phase != 1) return;
    int r = (s_slot + 1) % NR;                       /* order: 1 2 3 4 0, twice */
    if (++s_win < 2) return;                         /* the first window of a slot settles */
    s_fps_sum[r] += fps; s_fps_n[r]++;
    s_win = 0;
    for (;;) {
        if (++s_slot >= 2 * NR) { settle(); return; }
        int nr = (s_slot + 1) % NR;
        if (nr == 4 && s_aff_rate < 1) continue;    /* libunity does not call setaffinity here: nothing to skip */
        char why[32]; snprintf(why, sizeof why, "trial %d of %d", s_slot + 1, 2 * NR);
        set_remedy(nr, why);
        return;
    }
}
