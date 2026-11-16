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

/* the main thread, from UICamera.Update (f2_touch.c -> f2_screen_frame_tick) */
void f2_perf_note_main(void) { if (!s_main_tid) s_main_tid = my_tid(); }

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

/* per-thread CPU time (utime + stime, clock ticks) and the core it last ran on */
static int thread_cpu(int tid, unsigned long long *ticks, int *core)
{
    char p[64], b[1024];
    snprintf(p, sizeof p, "/proc/self/task/%d/stat", tid);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
    char *s = strrchr(b, ')');
    if (!s) return 0;
    s += 2;                 /* field 3 (state) */
    unsigned long long ut = 0, st = 0; int field = 3, cpu = -1;
    for (char *t = strtok(s, " "); t; t = strtok(NULL, " "), field++) {
        if (field == 14) ut = strtoull(t, NULL, 10);
        else if (field == 15) st = strtoull(t, NULL, 10);
        else if (field == 39) { cpu = atoi(t); break; }
    }
    *ticks = ut + st; *core = cpu;
    return 1;
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
    static unsigned long long main_t0, rend_t0; static int have0;
    unsigned long long mt = 0, rt = 0; int mc = -1, rc = -1;
    int okm = s_main_tid && thread_cpu(s_main_tid, &mt, &mc), okr = s_render_tid && thread_cpu(s_render_tid, &rt, &rc);
    long hz = sysconf(_SC_CLK_TCK); if (hz <= 0) hz = 100;
    double mp = -1, rp = -1;
    if (have0 && win_ms > 0) {
        if (okm) mp = (double)(mt - main_t0) * 1000.0 / hz / win_ms * 100.0;
        if (okr) rp = (double)(rt - rend_t0) * 1000.0 / hz / win_ms * 100.0;
    }
    main_t0 = mt; rend_t0 = rt; have0 = 1;
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
           "main %.0f%% on cpu%d @ %ld MHz, render %.0f%% on cpu%d @ %ld MHz | GPU %s | thermal headroom %.2f status %d",
           s_w_frames, s_w_frames ? s_w_sum_ms / s_w_frames : 0.0, s_w_max_ms, s_w_hist[0], s_w_hist[1], s_w_hist[2], s_w_hist[3], s_w_hist[4], s_w_hist[5],
           s_target_ns / 1e6, s_w_late, s_state > 0 ? "on" : s_state < 0 ? "off" : "-", s_w_hints, s_w_bad_reports ? " (report errors)" : "",
           mp, mc, core_mhz(mc), rp, rc, core_mhz(rc), gpu, head, tstat);
    (void)now;
}

/* the render thread, at the start of every vkQueuePresentKHR (before the pacer's sleep): frame_ns = the cap's period (0 = no cap) */
void f2_perf_present_begin(long long frame_ns)
{
    if (!f2_cfg.adpf && !f2_cfg.perflog) return;
    long long now = mono_ns();
    if (!s_render_tid) s_render_tid = my_tid();
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
        w0 = now; s_w_frames = s_w_late = s_w_hints = s_w_bad_reports = 0; s_w_sum_ms = s_w_max_ms = 0; memset(s_w_hist, 0, sizeof s_w_hist);
    }
}
/* ... and right after it returned: the next frame's work starts here (the pacer's sleep and the present's own wait are not work) */
void f2_perf_present_end(void)
{
    if (!f2_cfg.adpf && !f2_cfg.perflog) return;
    s_last_end = mono_ns();
}
