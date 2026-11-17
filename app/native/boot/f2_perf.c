/* F2 clock hints for the system (16.11.2026; a Galaxy S25 Ultra, Snapdragon 8 Elite / Adreno 830, Android 17: the first area ran
 * 60-82 fps for half a minute and then 21-30 fps for good, while the Mi 14 Ultra's 8 Gen 3 keeps 60 there.  nx2android measured the
 * same GPU family keeping 160 MHz without an ADPF session (KB nx2android-harvest NXH-D04).  The stock 2018.1 Unity player tells the
 * system nothing, so this does it for the player's threads:
 *   adpf=1 (default)  an ADPF performance hint session (APerformanceHint, Android 13+) for the main thread, the render thread and the
 *                     job workers; target = one frame of the cap; every present reports how long the frame took (the render thread's
 *                     time from the last present back to this one, the pacer's own sleep left out); frames running late send the
 *                     "workload grows" hint for the CPU AND the GPU (Android 16+), at most once a second.  0: nothing.
 *   perflog=1 (default) one "[perf]" line every 10 s: frame times, ADPF state, the CPU share of the main / render thread and the clock
 *                     of the core each ran on, the GPU clock and load when the kernel lets an app read them, the thermal headroom.
 *   prof=2000 (default) 17.11.2026, test2: ADPF did raise the S25's main thread to 4.47 GHz and fps stayed ~30, while the Mi 14 holds 60 in
 *                     the same spot at 1-2 GHz with ~6 ms frames; the S25 burns 5-8x the main-thread cycles.  A PC sampler on the main
 *                     thread (a POSIX timer on its own CPU-time clock, SIGPROF every N us of CPU it uses) writes "[prof]" lines every
 *                     10 s: the share per module and the hottest PCs / return addresses (main+0x.. / libX.so+0x.., tools/symbolize.py
 *                     names them).  The [perf] line also splits the main thread's CPU into user / kernel and counts its page faults and
 *                     context switches (a trap or fault storm shows up as kernel time).  0: off.
 * Everything is looked up with dlsym in libandroid.so: an older Android just leaves the parts it lacks out. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dlfcn.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <signal.h>
#include <ucontext.h>
#include <pthread.h>
#include <stdatomic.h>
#include "f2_boot.h"

typedef struct APerformanceHintManager APerformanceHintManager;
typedef struct APerformanceHintSession APerformanceHintSession;
typedef struct AThermalManager AThermalManager;

static APerformanceHintManager *(*p_get_manager)(void);
static APerformanceHintSession *(*p_create)(APerformanceHintManager *, const int32_t *, size_t, int64_t);
static int (*p_update_target)(APerformanceHintSession *, int64_t);
static int (*p_report)(APerformanceHintSession *, int64_t);
static int (*p_set_threads)(APerformanceHintSession *, const int32_t *, size_t);
static int (*p_power_eff)(APerformanceHintSession *, bool);
static int (*p_increase)(APerformanceHintSession *, bool, bool, const char *);
static AThermalManager *(*p_thermal_acquire)(void);
static float (*p_headroom)(AThermalManager *, int);
static int (*p_thermal_status)(AThermalManager *);

static APerformanceHintSession *s_session;
static AThermalManager *s_thermal;
static int s_main_tid, s_render_tid, s_tids[8], s_ntids, s_state;    /* s_state: 0 not tried, 1 on, -1 unavailable */
static int64_t s_target_ns;
static long long s_last_end;        /* when the previous present returned (ns) */
static long long s_last_hint;
static unsigned s_late_run;
/* 10 s window */
static unsigned s_w_frames, s_w_late, s_w_hints, s_w_bad_reports;
static double s_w_sum_ms, s_w_max_ms;
static unsigned s_w_hist[6];        /* frame work ms: <8.3 <16.7 <25 <33.3 <50 more */

static long long mono_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000000000ll + ts.tv_nsec; }
static int my_tid(void) { return (int)syscall(SYS_gettid); }

/* ---- PC sampler of the main thread (slot 0) and the render thread (slot 1) (prof=N us) ---- */
#define PROF_N 8192
static uintptr_t s_ppc[2][2][PROF_N], s_plr[2][2][PROF_N];        /* [slot][generation][sample] */
static atomic_uint s_pcnt[2][2], s_pgen;
static int s_prof_on[2], s_prof_sig_ok;
static volatile int s_prof_tid[2];

static void prof_sig(int sig, siginfo_t *si, void *ucv)
{
    (void)sig; (void)si;
    ucontext_t *uc = (ucontext_t *)ucv;
    int slot = (int)syscall(SYS_gettid) == s_prof_tid[1] ? 1 : 0;
    unsigned g = atomic_load_explicit(&s_pgen, memory_order_relaxed) & 1;
    unsigned i = atomic_fetch_add_explicit(&s_pcnt[slot][g], 1, memory_order_relaxed);
    if (i < PROF_N) { s_ppc[slot][g][i] = (uintptr_t)uc->uc_mcontext.pc; s_plr[slot][g][i] = (uintptr_t)uc->uc_mcontext.regs[30]; }
}

static void prof_start(int slot)
{
    if (f2_cfg.prof <= 0 || s_prof_on[slot]) return;
    int us = f2_cfg.prof < 500 ? 500 : f2_cfg.prof;
    if (!s_prof_sig_ok) {
        struct sigaction old, sa;
        if (sigaction(SIGPROF, NULL, &old) == 0 && old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN) {
            f2_log("F2: prof: SIGPROF already has a handler (%p): sampler off", (void *)old.sa_handler); s_prof_sig_ok = -1; return;
        }
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = prof_sig; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigemptyset(&sa.sa_mask);
        if (sigaction(SIGPROF, &sa, NULL) != 0) { f2_log("F2: prof: sigaction failed"); s_prof_sig_ok = -1; return; }
        s_prof_sig_ok = 1;
    }
    if (s_prof_sig_ok < 0) return;
    s_prof_tid[slot] = my_tid();
    clockid_t cid;
    if (pthread_getcpuclockid(pthread_self(), &cid) != 0) { f2_log("F2: prof: no CPU-time clock for this thread"); return; }
    struct sigevent sev; memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_THREAD_ID; sev.sigev_signo = SIGPROF; sev.sigev_notify_thread_id = my_tid();
    timer_t t;
    if (timer_create(cid, &sev, &t) != 0) { f2_log("F2: prof: timer_create failed"); return; }
    struct itimerspec it; memset(&it, 0, sizeof it);
    it.it_interval.tv_nsec = it.it_value.tv_nsec = us * 1000L;
    if (timer_settime(t, 0, &it, NULL) != 0) { f2_log("F2: prof: timer_settime failed"); return; }
    s_prof_on[slot] = 1;
    f2_log("F2: prof: %s thread %d sampled every %d us of its CPU time", slot ? "render" : "main", my_tid(), us);
}

/* executable mappings from /proc/self/maps: names what dl_iterate_phdr does not list (libunity / libil2cpp in the app's linker
 * namespace, anonymous code); file+0x.. is the file offset, which is the vaddr for the usual .so layout */
typedef struct { uintptr_t lo, hi, off; char name[56]; } pmap_t;
#define PMAP_N 2048
static pmap_t s_pmap[PMAP_N];
static int s_npmap;
static void pmap_load(void)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char b[512]; int n = 0;
    while (fgets(b, sizeof b, f) && n < PMAP_N) {
        unsigned long lo, hi, off; char perms[8]; int pos = 0;
        if (sscanf(b, "%lx-%lx %7s %lx %*s %*s%n", &lo, &hi, perms, &off, &pos) < 4 || perms[2] != 'x') continue;
        char *path = b + pos; while (*path == ' ') path++;
        path[strcspn(path, "\n")] = 0;
        const char *sl = strrchr(path, '/');
        pmap_t *m = &s_pmap[n++];
        m->lo = lo; m->hi = hi; m->off = off;
        if (*path) snprintf(m->name, sizeof m->name, "%s", sl ? sl + 1 : path);
        else snprintf(m->name, sizeof m->name, "anon@%lx", lo);
        char *plus = strchr(m->name, '+'); if (plus) *plus = '_';          /* "+" separates name and offset in the lines */
        char *spc = strchr(m->name, ' '); while (spc) { *spc = '_'; spc = strchr(spc, ' '); }
    }
    fclose(f);
    s_npmap = n;                    /* maps are sorted by address */
}
static int describe(uintptr_t a, char *out, size_t n)
{
    if (f2_describe_addr(a, out, n)) return 1;
    int l = 0, h = s_npmap - 1;
    while (l <= h) {
        int m = (l + h) / 2;
        if (a < s_pmap[m].lo) h = m - 1;
        else if (a >= s_pmap[m].hi) l = m + 1;
        else {
            int w = snprintf(out, n, "%s+0x%lx", s_pmap[m].name, (unsigned long)(a - s_pmap[m].lo + s_pmap[m].off));
            Dl_info di;                 /* an exported symbol close by (libc, the linker, system libs) */
            if (w > 0 && (size_t)w < n && dladdr((void *)a, &di) && di.dli_sname && di.dli_saddr && a >= (uintptr_t)di.dli_saddr &&
                a - (uintptr_t)di.dli_saddr < 0x2000)
                snprintf(out + w, n - (size_t)w, "<%.40s+0x%lx>", di.dli_sname, (unsigned long)(a - (uintptr_t)di.dli_saddr));
            return 1;
        }
    }
    return 0;
}

static int cmp_up(const void *a, const void *b) { uintptr_t x = *(const uintptr_t *)a, y = *(const uintptr_t *)b; return x < y ? -1 : x > y; }
typedef struct { uintptr_t key; unsigned n; } prof_hit_t;
static int cmp_hit(const void *a, const void *b) { const prof_hit_t *x = a, *y = b; return x->n < y->n ? 1 : x->n > y->n ? -1 : 0; }

/* masks and sorts v (n entries) and keeps the k biggest buckets in top (most first) */
static int prof_top(uintptr_t *v, unsigned n, uintptr_t mask, prof_hit_t *top, int k)
{
    for (unsigned i = 0; i < n; i++) v[i] &= mask;
    qsort(v, n, sizeof *v, cmp_up);
    int nt = 0;
    for (unsigned i = 0; i < n;) {
        unsigned j = i; while (j < n && v[j] == v[i]) j++;
        prof_hit_t h = { v[i], j - i };
        if (nt < k) { top[nt++] = h; qsort(top, (size_t)nt, sizeof *top, cmp_hit); }
        else if (h.n > top[k - 1].n) { top[k - 1] = h; qsort(top, (size_t)nt, sizeof *top, cmp_hit); }
        i = j;
    }
    return nt;
}

static void prof_slot(int slot, unsigned g);
static void prof_line(void)
{
    if (!s_prof_on[0] && !s_prof_on[1]) return;
    static long long refreshed;
    long long now = mono_ns();
    if (now - refreshed > 60000000000ll) { f2_crash_refresh_modules(); pmap_load(); refreshed = now; }
    unsigned g = atomic_fetch_add_explicit(&s_pgen, 1, memory_order_relaxed) & 1;     /* the handler now fills the other buffers */
    for (int slot = 0; slot < 2; slot++) if (s_prof_on[slot]) prof_slot(slot, g);
}
static void prof_slot(int slot, unsigned g)
{
    const char *who = slot ? "render" : "main";
    unsigned n = atomic_load_explicit(&s_pcnt[slot][g], memory_order_relaxed), lost = 0;
    if (n > PROF_N) { lost = n - PROF_N; n = PROF_N; }
    static uintptr_t v[PROF_N];
    if (!n) { atomic_store(&s_pcnt[slot][g], 0); f2_log("F2: [prof] %s 10s: no samples", who); return; }
    /* share per module */
    char mods[8][40]; unsigned mcnt[8] = {0}; int nm = 0; unsigned unk = 0;
    for (unsigned i = 0; i < n; i++) {
        char d[128];
        if (!describe(s_ppc[slot][g][i], d, sizeof d)) { unk++; continue; }
        char *plus = strchr(d, '+'); if (plus) *plus = 0;
        char *lt = strchr(d, '<'); if (lt) *lt = 0;
        int m = 0; while (m < nm && strcmp(mods[m], d)) m++;
        if (m == nm) { if (nm == 8) { unk++; continue; } snprintf(mods[nm++], sizeof mods[0], "%s", d); }
        mcnt[m]++;
    }
    char line[900]; size_t w = 0;
    w += (size_t)snprintf(line + w, sizeof line - w, "F2: [prof] %s 10s: %u samples (%d us CPU each)%s | by module:", who, n + lost, f2_cfg.prof,
                          lost ? " (buffer full, the first 8192 counted)" : "");
    for (int m = 0; m < nm && w < sizeof line - 48; m++) w += (size_t)snprintf(line + w, sizeof line - w, " %s %.1f%%", mods[m], mcnt[m] * 100.0 / n);
    if (unk && w < sizeof line - 32) w += (size_t)snprintf(line + w, sizeof line - w, " ? %.1f%%", unk * 100.0 / n);
    f2_log("%s", line);
    /* the hottest PCs (16-byte buckets) and return addresses */
    for (int pass = 0; pass < 2; pass++) {
        memcpy(v, pass ? s_plr[slot][g] : s_ppc[slot][g], n * sizeof *v);
        prof_hit_t top[16]; int nt = prof_top(v, n, pass ? ~(uintptr_t)3 : ~(uintptr_t)15, top, 16);
        w = (size_t)snprintf(line, sizeof line, "F2: [prof] %s top %s:", who, pass ? "return addresses (x30)" : "PCs");
        for (int i = 0; i < nt && w < sizeof line - 90; i++) {
            char d[128]; if (!describe(top[i].key, d, sizeof d)) snprintf(d, sizeof d, "%p", (void *)top[i].key);
            char *sp = strchr(d, ' '); if (sp) *sp = 0;
            w += (size_t)snprintf(line + w, sizeof line - w, " %s %.1f%%", d, top[i].n * 100.0 / n);
        }
        f2_log("%s", line);
    }
    atomic_store(&s_pcnt[slot][g], 0);
}

/* the main thread, from UICamera.Update (f2_touch.c -> f2_screen_frame_tick) */
void f2_sysdiag_start(int main_tid);
void f2_sysdiag_note_render(int tid);
void f2_perf_note_main(void) { if (!s_main_tid) { s_main_tid = my_tid(); prof_start(0); f2_sysdiag_start(s_main_tid); } }

static void resolve(void)
{
    void *h = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) return;
    p_get_manager = (APerformanceHintManager *(*)(void))dlsym(h, "APerformanceHint_getManager");
    p_create = (APerformanceHintSession *(*)(APerformanceHintManager *, const int32_t *, size_t, int64_t))dlsym(h, "APerformanceHint_createSession");
    p_update_target = (int (*)(APerformanceHintSession *, int64_t))dlsym(h, "APerformanceHint_updateTargetWorkDuration");
    p_report = (int (*)(APerformanceHintSession *, int64_t))dlsym(h, "APerformanceHint_reportActualWorkDuration");
    p_set_threads = (int (*)(APerformanceHintSession *, const int32_t *, size_t))dlsym(h, "APerformanceHint_setThreads");
    p_power_eff = (int (*)(APerformanceHintSession *, bool))dlsym(h, "APerformanceHint_setPreferPowerEfficiency");
    p_increase = (int (*)(APerformanceHintSession *, bool, bool, const char *))dlsym(h, "APerformanceHint_notifyWorkloadIncrease");
    p_thermal_acquire = (AThermalManager *(*)(void))dlsym(h, "AThermal_acquireManager");
    p_headroom = (float (*)(AThermalManager *, int))dlsym(h, "AThermal_getThermalHeadroom");
    p_thermal_status = (int (*)(AThermalManager *))dlsym(h, "AThermal_getCurrentThermalStatus");
}

/* the player's threads by name: main, render ("UnityGfxDeviceW"), job workers ("Job.Worker N"); at most 8 */
static void pick_threads(void)
{
    s_ntids = 0;
    if (s_main_tid) s_tids[s_ntids++] = s_main_tid;
    if (s_render_tid && s_render_tid != s_main_tid) s_tids[s_ntids++] = s_render_tid;
    DIR *d = opendir("/proc/self/task");
    if (!d) return;
    struct dirent *e;
    int workers = 0;
    while ((e = readdir(d)) && s_ntids < 8) {
        int tid = atoi(e->d_name);
        if (tid <= 0 || tid == s_main_tid || tid == s_render_tid) continue;
        char p[64], name[32] = {0};
        snprintf(p, sizeof p, "/proc/self/task/%d/comm", tid);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (fgets(name, sizeof name, f)) name[strcspn(name, "\n")] = 0;
        fclose(f);
        if (!strncmp(name, "Job.Worker", 10) && workers < 4) { s_tids[s_ntids++] = tid; workers++; }
    }
    closedir(d);
}

static void open_session(void)
{
    s_state = -1;
    if (!f2_cfg.adpf) { f2_log("F2: perf: ADPF off (adpf=0)"); return; }
    resolve();
    if (!p_get_manager || !p_create || !p_report) { f2_log("F2: perf: ADPF not available on this Android (no APerformanceHint in libandroid.so)"); return; }
    APerformanceHintManager *m = p_get_manager();
    pick_threads();
    if (!m || !s_ntids) { f2_log("F2: perf: ADPF: no manager (%p) or no threads (%d)", (void *)m, s_ntids); return; }
    s_session = p_create(m, s_tids, (size_t)s_ntids, s_target_ns);
    if (!s_session) { f2_log("F2: perf: ADPF: createSession refused (%d threads, target %.2f ms)", s_ntids, s_target_ns / 1e6); return; }
    if (p_power_eff) p_power_eff(s_session, false);
    s_state = 1;
    char list[160]; size_t w = 0; list[0] = 0;
    for (int i = 0; i < s_ntids && w + 12 < sizeof list; i++) w += (size_t)snprintf(list + w, sizeof list - w, "%s%d", i ? "," : "", s_tids[i]);
    f2_log("F2: perf: ADPF session on: threads %s (main %d, render %d), target %.2f ms, power efficiency off %s, load-up hints %s",
           list, s_main_tid, s_render_tid, s_target_ns / 1e6, p_power_eff ? "yes" : "n/a", p_increase ? "CPU+GPU" : "n/a (Android < 16)");
}

/* per-thread CPU time (utime + stime, clock ticks) and the core it last ran on; optional: user / kernel ticks, minor / major faults */
static int thread_cpu_ex(int tid, unsigned long long *ticks, int *core, unsigned long long *u, unsigned long long *k,
                         unsigned long long *minf, unsigned long long *majf)
{
    char p[64], b[1024];
    snprintf(p, sizeof p, "/proc/self/task/%d/stat", tid);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
    char *s = strrchr(b, ')');
    if (!s) return 0;
    s += 2;                 /* field 3 (state) */
    unsigned long long ut = 0, st = 0, mi = 0, ma = 0; int field = 3, cpu = -1;
    char *save = NULL;
    for (char *t = strtok_r(s, " ", &save); t; t = strtok_r(NULL, " ", &save), field++) {
        if (field == 10) mi = strtoull(t, NULL, 10);
        else if (field == 12) ma = strtoull(t, NULL, 10);
        else if (field == 14) ut = strtoull(t, NULL, 10);
        else if (field == 15) st = strtoull(t, NULL, 10);
        else if (field == 39) { cpu = atoi(t); break; }
    }
    *ticks = ut + st; *core = cpu;
    if (u) *u = ut;
    if (k) *k = st;
    if (minf) *minf = mi;
    if (majf) *majf = ma;
    return 1;
}
static int thread_cpu(int tid, unsigned long long *ticks, int *core) { return thread_cpu_ex(tid, ticks, core, NULL, NULL, NULL, NULL); }
/* voluntary / involuntary context switches of a thread */
static void thread_csw(int tid, unsigned long long *vol, unsigned long long *invol)
{
    char p[64], b[160]; *vol = *invol = 0;
    snprintf(p, sizeof p, "/proc/self/task/%d/status", tid);
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fgets(b, sizeof b, f)) {
        if (!strncmp(b, "voluntary_ctxt_switches:", 24)) *vol = strtoull(b + 24, NULL, 10);
        else if (!strncmp(b, "nonvoluntary_ctxt_switches:", 27)) *invol = strtoull(b + 27, NULL, 10);
    }
    fclose(f);
}
static long read_long(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long v = -1;
    if (fscanf(f, "%ld", &v) != 1) v = -1;
    fclose(f);
    return v;
}
static long core_mhz(int core)
{
    if (core < 0) return -1;
    char p[96]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", core);
    long k = read_long(p);
    return k > 0 ? k / 1000 : -1;
}

static void perf_line(long long now, double win_ms)
{
    static unsigned long long main_t0, rend_t0, mu0, mk0, mf0, mj0, cv0, ci0; static int have0;
    unsigned long long mt = 0, rt = 0, mu = 0, mk = 0, mf = 0, mj = 0, cv = 0, ci = 0; int mc = -1, rc = -1;
    int okm = s_main_tid && thread_cpu_ex(s_main_tid, &mt, &mc, &mu, &mk, &mf, &mj), okr = s_render_tid && thread_cpu(s_render_tid, &rt, &rc);
    if (okm) thread_csw(s_main_tid, &cv, &ci);
    long hz = sysconf(_SC_CLK_TCK); if (hz <= 0) hz = 100;
    double mp = -1, rp = -1;
    char mx[160] = "";
    if (have0 && win_ms > 0) {
        if (okm) {
            mp = (double)(mt - main_t0) * 1000.0 / hz / win_ms * 100.0;
            snprintf(mx, sizeof mx, " (user %.0f%% kernel %.0f%%, faults %llu minor %llu major, switches %llu vol %llu invol)",
                     (double)(mu - mu0) * 1000.0 / hz / win_ms * 100.0, (double)(mk - mk0) * 1000.0 / hz / win_ms * 100.0,
                     mf - mf0, mj - mj0, cv - cv0, ci - ci0);
        }
        if (okr) rp = (double)(rt - rend_t0) * 1000.0 / hz / win_ms * 100.0;
    }
    main_t0 = mt; rend_t0 = rt; mu0 = mu; mk0 = mk; mf0 = mf; mj0 = mj; cv0 = cv; ci0 = ci; have0 = 1;
    long gclk = read_long("/sys/class/kgsl/kgsl-3d0/gpuclk");
    if (gclk < 0) gclk = read_long("/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq");
    long gbusy = read_long("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage");
    char gpu[64];
    if (gclk > 0 || gbusy >= 0) snprintf(gpu, sizeof gpu, "%ld MHz, busy %ld%%", gclk > 0 ? gclk / 1000000 : -1, gbusy);
    else snprintf(gpu, sizeof gpu, "not readable");
    float head = -1; int tstat = -1;
    if (!s_thermal && p_thermal_acquire) s_thermal = p_thermal_acquire();
    if (s_thermal) { if (p_headroom) head = p_headroom(s_thermal, 0); if (p_thermal_status) tstat = p_thermal_status(s_thermal); }
    f2_log("F2: [perf] 10s: %u frames, work avg %.1f max %.1f ms (<8.3:%u <16.7:%u <25:%u <33.3:%u <50:%u more:%u), target %.2f ms, late %u | ADPF %s, load-up hints %u%s | "
           "main %.0f%%%s on cpu%d @ %ld MHz, render %.0f%% on cpu%d @ %ld MHz | GPU %s | thermal headroom %.2f status %d",
           s_w_frames, s_w_frames ? s_w_sum_ms / s_w_frames : 0.0, s_w_max_ms, s_w_hist[0], s_w_hist[1], s_w_hist[2], s_w_hist[3], s_w_hist[4], s_w_hist[5],
           s_target_ns / 1e6, s_w_late, s_state > 0 ? "on" : s_state < 0 ? "off" : "-", s_w_hints, s_w_bad_reports ? " (report errors)" : "",
           mp, mx, mc, core_mhz(mc), rp, rc, core_mhz(rc), gpu, head, tstat);
    (void)now;
}

/* the render thread, at the start of every vkQueuePresentKHR (before the pacer's sleep): frame_ns = the cap's period (0 = no cap) */
void f2_perf_present_begin(long long frame_ns)
{
    if (!f2_cfg.adpf && !f2_cfg.perflog) return;
    long long now = mono_ns();
    if (!s_render_tid) { s_render_tid = my_tid(); f2_sysdiag_note_render(s_render_tid); prof_start(1); }
    int64_t target = frame_ns > 0 ? frame_ns : 16666667;
    if (target != s_target_ns) {
        s_target_ns = target;
        if (s_session && p_update_target) p_update_target(s_session, target);
    }
    /* the session is opened once the main thread is known (or after ~10 s of frames without it) */
    static unsigned frames_seen; frames_seen++;
    if (s_state == 0 && (s_main_tid || frames_seen > 600)) open_session();
    static long long rescan;
    if (s_session && p_set_threads && now - rescan > 30000000000ll) {          /* job workers may start later: look again every 30 s */
        int old = s_ntids; int32_t keep[8]; memcpy(keep, s_tids, sizeof keep);
        pick_threads();
        if (s_ntids != old || memcmp(keep, s_tids, sizeof(int32_t) * (size_t)s_ntids)) p_set_threads(s_session, s_tids, (size_t)s_ntids);
        rescan = now;
    }
    if (s_last_end > 0) {
        long long work = now - s_last_end;
        if (work > 0 && work < 2000000000ll) {
            if (s_session && p_report(s_session, work) != 0) s_w_bad_reports++;
            double ms = work / 1e6;
            s_w_frames++; s_w_sum_ms += ms; if (ms > s_w_max_ms) s_w_max_ms = ms;
            int b = ms < 8.33 ? 0 : ms < 16.67 ? 1 : ms < 25 ? 2 : ms < 33.3 ? 3 : ms < 50 ? 4 : 5; s_w_hist[b]++;
            if (work > target + target / 8) {
                s_w_late++;
                if (++s_late_run >= 3 && s_session && p_increase && now - s_last_hint > 1000000000ll) {
                    p_increase(s_session, true, true, "f2 frames late");
                    s_last_hint = now; s_w_hints++; s_late_run = 0;
                }
            } else s_late_run = 0;
        }
    }
    static long long w0;
    if (!w0) w0 = now;
    if (now - w0 >= 10000000000ll) {
        if (f2_cfg.perflog) perf_line(now, (now - w0) / 1e6);
        prof_line();
        w0 = now; s_w_frames = s_w_late = s_w_hints = s_w_bad_reports = 0; s_w_sum_ms = s_w_max_ms = 0; memset(s_w_hist, 0, sizeof s_w_hist);
    }
}
/* ... and right after it returned: the next frame's work starts here (the pacer's sleep and the present's own wait are not work) */
void f2_perf_present_end(void)
{
    if (!f2_cfg.adpf && !f2_cfg.perflog) return;
    s_last_end = mono_ns();
}
