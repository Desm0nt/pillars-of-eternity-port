/* F2 system diagnostics (17.11.2026, 0.1.2-test2).  A Galaxy S25 Ultra (Snapdragon 8 Elite, Oryon cores, One UI on Android 17) runs the
 * first area at ~30 fps with the main thread at 98 % of a 4.47 GHz core, while the Mi 14 Ultra holds 60 there at 1-2 GHz: the S25 spends
 * 5-8x the cycles.  This file logs what the player's phone does differently, in one run, so no extra test builds are needed:
 *   [sys]    once: kernel, firmware, CPU features (hwcaps, MIDR per cluster), clusters, GPU (kgsl) files, the vendor's game / thermal /
 *            graphics properties, the main thread's scheduling (affinity, cpuset, cgroup, uclamp, nice)
 *   [bench]  at ~5 s and again at ~2 min (in the game): per CPU cluster, ns per operation of what an old engine on a new core may hit:
 *            plain loop (= the core's real clock), timer / cache-type register reads, a trapped ID register (kernel trap cost),
 *            LL/SC and LSE atomics, cross-cache-line loads, DC ZVA, memcpy / memset, malloc, page faults, mmap, cache flush, mutex,
 *            clock_gettime (vDSO or syscall), a plain syscall, sched_yield, an FP chain
 *   [thr]    every 10 s: the busiest threads by name (% of one core), the main / render thread's run-queue wait, frequency caps per cluster
 *            and online cores (GOS / thermal limits show here), changes of the main thread's scheduling
 * Logs only; personal identifiers (serials, IMEI, network) are never read.  sysdiag=0 turns it off. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/auxv.h>
#include <sys/resource.h>
#include <sys/system_properties.h>
#include "f2_boot.h"

#ifndef HWCAP_CPUID
#define HWCAP_CPUID (1 << 11)
#endif
#ifndef HWCAP_ATOMICS
#define HWCAP_ATOMICS (1 << 8)
#endif
#ifndef HWCAP_USCAT
#define HWCAP_USCAT (1 << 25)
#endif

static int s_main, s_render;
static long long mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000ll + t.tv_nsec; }
static int rd_str(const char *path, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t k = fread(out, 1, n - 1, f); fclose(f);
    out[k] = 0;
    while (k && (out[k - 1] == '\n' || out[k - 1] == ' ')) out[--k] = 0;
    for (size_t i = 0; i < k; i++) if (out[i] == '\n') out[i] = ' ';
    return 1;
}
static long rd_long(const char *path) { char b[64]; return rd_str(path, b, sizeof b) ? strtol(b, NULL, 10) : -1; }

/* ---------- [sys] ---------- */
typedef struct { char buf[900]; size_t w; const char *head; } lbuf_t;
static void lb_add(lbuf_t *l, const char *s)
{
    size_t n = strlen(s);
    if (l->w && l->w + n + 2 > sizeof l->buf) { f2_log("%s", l->buf); l->w = 0; }
    if (!l->w) l->w = (size_t)snprintf(l->buf, sizeof l->buf, "%s", l->head);
    l->w += (size_t)snprintf(l->buf + l->w, sizeof l->buf - l->w, " %s", s);
}
static void lb_flush(lbuf_t *l) { if (l->w) f2_log("%s", l->buf); l->w = 0; }

static const char *k_prop_exact[] = { "ro.build.fingerprint", "ro.vendor.build.fingerprint", "ro.product.model", "ro.product.device",
    "ro.product.manufacturer", "ro.board.platform", "ro.hardware", "ro.soc.manufacturer", "ro.soc.model", "ro.build.version.release",
    "ro.build.version.sdk", "ro.build.version.incremental", "ro.build.version.oneui", "ro.build.version.sem", "ro.build.PDA",
    "ro.vndk.version", "ro.zygote", "ro.product.cpu.abilist", "ro.config.low_ram", "ro.kernel.version", NULL };
static const char *k_prop_has[] = { "game", "gos", "siop", "dvfs", "thermal", "gpu", "vulkan", "vk.", "hwui", "angle", "graphics", "egl",
    "sched", "boost", "perf", "power", "dha", "refresh", "fps", "frame", "adreno", "kgsl", "surface_flinger", "renderengine", NULL };
static const char *k_prop_skip[] = { "serial", "imei", "meid", "mac", "android_id", "sim", "ril", "phone", "gsm", "operator", "carrier",
    "bt.", "bluetooth", "wifi", "wlan", "ims", "account", "iccid", "msisdn", "location", "gps", "camera", "audio", NULL };
static int s_nprops;
static lbuf_t s_plb = { .head = "F2: [sys] props:" };
static void prop_val(void *cookie, const char *name, const char *val, uint32_t serial);
static void prop_cb(const prop_info *pi, void *cookie) { __system_property_read_callback(pi, prop_val, cookie); }
static void prop_val(void *cookie, const char *name, const char *val, uint32_t serial)
{
    (void)cookie; (void)serial;
    if (s_nprops >= 160 || !name || !val) return;
    for (int i = 0; k_prop_skip[i]; i++) if (strstr(name, k_prop_skip[i])) return;
    int want = 0;
    for (int i = 0; k_prop_exact[i] && !want; i++) if (!strcmp(name, k_prop_exact[i])) want = 1;
    for (int i = 0; k_prop_has[i] && !want; i++) if (strstr(name, k_prop_has[i])) want = 1;
    if (!want) return;
    char kv[200];
    snprintf(kv, sizeof kv, "%s=%s", name, val);
    lb_add(&s_plb, kv); s_nprops++;
}

typedef struct { int first, last, id; char cpus[32]; } policy_t;
static policy_t s_pol[8];
static int s_npol;
static void find_policies(void)
{
    s_npol = 0;
    for (int id = 0; id < 16 && s_npol < 8; id++) {
        char p[96], b[64];
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpufreq/policy%d/related_cpus", id);
        if (!rd_str(p, b, sizeof b)) continue;
        policy_t *q = &s_pol[s_npol++];
        q->id = id; snprintf(q->cpus, sizeof q->cpus, "%s", b);
        q->first = atoi(b); q->last = q->first;
        for (char *t = strtok(b, " "); t; t = strtok(NULL, " ")) q->last = atoi(t);
    }
}
static void pol_path(char *p, size_t n, int id, const char *f) { snprintf(p, n, "/sys/devices/system/cpu/cpufreq/policy%d/%s", id, f); }

struct f2_sched_attr { uint32_t size, policy; uint64_t flags; int32_t nice; uint32_t prio; uint64_t runtime, deadline, period; uint32_t util_min, util_max; };
static void sched_desc(int tid, char *out, size_t n)
{
    cpu_set_t cs; CPU_ZERO(&cs);
    char aff[64] = "?"; size_t w = 0;
    if (sched_getaffinity(tid, sizeof cs, &cs) == 0) {
        aff[0] = 0;
        for (int c = 0; c < 16 && w + 4 < sizeof aff; c++) if (CPU_ISSET(c, &cs)) w += (size_t)snprintf(aff + w, sizeof aff - w, "%s%d", w ? "," : "", c);
    }
    struct f2_sched_attr a; memset(&a, 0, sizeof a); a.size = sizeof a;
    long r = syscall(SYS_sched_getattr, tid, &a, sizeof a, 0);
    char p[96], cpuset[96] = "?", slack[32] = "?";
    snprintf(p, sizeof p, "/proc/self/task/%d/cpuset", tid); rd_str(p, cpuset, sizeof cpuset);
    snprintf(p, sizeof p, "/proc/self/task/%d/timerslack_ns", tid); rd_str(p, slack, sizeof slack);
    if (r == 0) snprintf(out, n, "cpus %s, cpuset %s, policy %u nice %d prio %u, uclamp %u-%u (flags 0x%llx), timerslack %s ns", aff, cpuset, a.policy,
                         a.nice, a.prio, a.util_min, a.util_max, (unsigned long long)a.flags, slack);
    else snprintf(out, n, "cpus %s, cpuset %s, sched_getattr failed, nice %d, timerslack %s ns", aff, cpuset, getpriority(PRIO_PROCESS, (id_t)tid), slack);
}

static void sys_once(void)
{
    struct utsname u;
    if (uname(&u) == 0) f2_log("F2: [sys] kernel %s %s | %s", u.release, u.machine, u.version);
    unsigned long hw = getauxval(AT_HWCAP), hw2 = getauxval(AT_HWCAP2);
    f2_log("F2: [sys] hwcap 0x%lx hwcap2 0x%lx: atomics(LSE) %d uscat(LSE2) %d cpuid-emulation %d | page %ld", hw, hw2, !!(hw & HWCAP_ATOMICS),
           !!(hw & HWCAP_USCAT), !!(hw & HWCAP_CPUID), sysconf(_SC_PAGESIZE));
    find_policies();
    for (int i = 0; i < s_npol; i++) {
        char p[96], gov[32] = "?", avail[200] = "";
        int id = s_pol[i].id;
        pol_path(p, sizeof p, id, "scaling_governor"); rd_str(p, gov, sizeof gov);
        pol_path(p, sizeof p, id, "scaling_available_frequencies"); rd_str(p, avail, sizeof avail);
        long cmax, cmin, smax, smin;
        pol_path(p, sizeof p, id, "cpuinfo_max_freq"); cmax = rd_long(p);
        pol_path(p, sizeof p, id, "cpuinfo_min_freq"); cmin = rd_long(p);
        pol_path(p, sizeof p, id, "scaling_max_freq"); smax = rd_long(p);
        pol_path(p, sizeof p, id, "scaling_min_freq"); smin = rd_long(p);
        char part[48] = "?";
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/regs/identification/midr_el1", s_pol[i].first);
        rd_str(p, part, sizeof part);
        f2_log("F2: [sys] cluster policy%d cpus [%s] midr %s | hw %ld-%ld MHz, now allowed %ld-%ld MHz, governor %s | steps %s", id, s_pol[i].cpus, part,
               cmin / 1000, cmax / 1000, smin / 1000, smax / 1000, gov, avail);
    }
    char b[200];
    if (rd_str("/sys/devices/system/cpu/online", b, sizeof b)) f2_log("F2: [sys] cpus online %s", b);
    /* kgsl: whatever an app may read */
    static const char *kg[] = { "gpu_model", "gpuclk", "max_gpuclk", "min_clock_mhz", "max_clock_mhz", "devfreq/governor", "devfreq/min_freq",
        "devfreq/max_freq", "devfreq/available_frequencies", "thermal_pwrlevel", "max_pwrlevel", "min_pwrlevel", "default_pwrlevel", "throttling",
        "force_clk_on", "idle_timer", "gpu_busy_percentage", "gpubusy", "temp", NULL };
    lbuf_t kl = { .head = "F2: [sys] kgsl:" };
    for (int i = 0; kg[i]; i++) {
        char p[96], v[160], kv[200];
        snprintf(p, sizeof p, "/sys/class/kgsl/kgsl-3d0/%s", kg[i]);
        if (rd_str(p, v, sizeof v)) { snprintf(kv, sizeof kv, "%s=%s", kg[i], v); lb_add(&kl, kv); }
    }
    if (!kl.w) f2_log("F2: [sys] kgsl: nothing readable");
    lb_flush(&kl);
    s_nprops = 0; s_plb.w = 0;
    __system_property_foreach(prop_cb, NULL);
    lb_flush(&s_plb);
    if (rd_str("/proc/self/cgroup", b, sizeof b)) f2_log("F2: [sys] cgroup %s", b);
    char sd[300];
    if (s_main) { sched_desc(s_main, sd, sizeof sd); f2_log("F2: [sys] main thread %d: %s", s_main, sd); }
}

/* ---------- [bench] ---------- */
static volatile uint64_t s_sink;
static void b_loop(long n) { __asm__ volatile("1: subs %0, %0, #1\n b.ne 1b" : "+r"(n) :: "cc"); }
static void b_cntvct(long n) { uint64_t v = 0; for (long i = 0; i < n; i++) __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v)); s_sink = v; }
static void b_ctr(long n) { uint64_t v = 0; for (long i = 0; i < n; i++) __asm__ volatile("mrs %0, ctr_el0" : "=r"(v)); s_sink = v; }
static void b_midr(long n) { uint64_t v = 0; for (long i = 0; i < n; i++) __asm__ volatile("mrs %0, midr_el1" : "=r"(v)); s_sink = v; }
static uint64_t s_atom[32] __attribute__((aligned(128)));
static void b_llsc(long n)
{
    __asm__ volatile("1: ldaxr x9, [%1]\n add x9, x9, #1\n stlxr w10, x9, [%1]\n cbnz w10, 1b\n subs %0, %0, #1\n b.ne 1b"
                     : "+r"(n) : "r"(&s_atom[0]) : "x9", "x10", "memory", "cc");
}
static void b_lse(long n)
{
    __asm__ volatile(".arch_extension lse\n mov x11, #1\n1: ldaddal x11, x9, [%1]\n subs %0, %0, #1\n b.ne 1b"
                     : "+r"(n) : "r"(&s_atom[0]) : "x9", "x11", "memory", "cc");
}
static void b_cas(long n)
{
    __asm__ volatile(".arch_extension lse\n1: ldr x9, [%1]\n mov x10, x9\n add x11, x9, #1\n casal x10, x11, [%1]\n subs %0, %0, #1\n b.ne 1b"
                     : "+r"(n) : "r"(&s_atom[0]) : "x9", "x10", "x11", "memory", "cc");
}
static uint8_t *s_buf, *s_buf2;
#define BUF_SZ (8u << 20)
static void b_ld_cross(long n)     /* 8-byte loads that straddle a 64-byte line */
{
    uint64_t acc = 0; const uint8_t *p = s_buf + 60;
    for (long i = 0; i < n; i++) { uint64_t v; memcpy(&v, p + ((i & 1023) << 6), 8); acc += v; }
    s_sink = acc;
}
static void b_ld_aligned(long n)
{
    uint64_t acc = 0; const uint8_t *p = s_buf;
    for (long i = 0; i < n; i++) { uint64_t v; memcpy(&v, p + ((i & 1023) << 6), 8); acc += v; }
    s_sink = acc;
}
static void b_ldar(long n) { uint64_t acc = 0; for (long i = 0; i < n; i++) acc += __atomic_load_n(&s_atom[(i & 7) * 2], __ATOMIC_ACQUIRE); s_sink = acc; }
static void b_memcpy(long n) { for (long i = 0; i < n; i++) { memcpy(s_buf2, s_buf, BUF_SZ); __asm__ volatile("" ::: "memory"); } s_sink = s_buf2[123]; }
static void b_memset(long n) { for (long i = 0; i < n; i++) { memset(s_buf, (int)i + 1, BUF_SZ); __asm__ volatile("" ::: "memory"); } s_sink = s_buf[77]; }
static long s_zva_block;
static void b_zva(long n)
{
    for (long k = 0; k < n; k++)
        for (uint8_t *p = s_buf; p < s_buf + BUF_SZ; p += s_zva_block) __asm__ volatile("dc zva, %0" :: "r"(p) : "memory");
}
static void b_malloc(long n) { for (long i = 0; i < n; i++) { void *p = malloc(64 + (i & 63)); s_sink += (uintptr_t)p; free(p); } }
static void b_malloc_big(long n) { for (long i = 0; i < n; i++) { void *p = malloc(256 << 10); ((volatile char *)p)[0] = 1; s_sink += (uintptr_t)p; free(p); } }
static void b_fault(long n)        /* n = pages */
{
    size_t sz = (size_t)n * 4096;
    uint8_t *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return;
    for (long i = 0; i < n; i++) p[i * 4096] = 1;
    munmap(p, sz);
}
static void b_mmap(long n) { for (long i = 0; i < n; i++) { void *p = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); if (p != MAP_FAILED) munmap(p, 65536); } }
static void b_flush(long n) { for (long i = 0; i < n; i++) __builtin___clear_cache((char *)s_buf, (char *)s_buf + 65536); }
static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;
static void b_mutex(long n) { for (long i = 0; i < n; i++) { pthread_mutex_lock(&s_mx); pthread_mutex_unlock(&s_mx); } }
static void b_clock(long n) { struct timespec t; for (long i = 0; i < n; i++) clock_gettime(CLOCK_MONOTONIC, &t); s_sink = (uint64_t)t.tv_nsec; }
static void b_syscall(long n) { for (long i = 0; i < n; i++) s_sink += (uint64_t)syscall(SYS_getppid); }
static void b_yield(long n) { for (long i = 0; i < n; i++) sched_yield(); }
static void b_fma(long n)
{
    double a = 1.0, b = 0.999999, c = 1e-9;
    for (long i = 0; i < n; i++) __asm__ volatile("fmadd %d0, %d0, %d1, %d2" : "+w"(a) : "w"(b), "w"(c));
    s_sink = (uint64_t)a;
}

static double run_ns(void (*fn)(long), long n)
{
    long long t0 = mono(); fn(n); long long t1 = mono();
    return (double)(t1 - t0) / (double)n;
}

static void bench_cluster(const policy_t *q, const char *when)
{
    cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(q->last, &cs);
    if (sched_setaffinity((pid_t)syscall(SYS_gettid), sizeof cs, &cs) != 0) { f2_log("F2: [bench] %s cpu%d: cannot pin", when, q->last); return; }
    for (int i = 0; i < 200 && sched_getcpu() != q->last; i++) { sched_yield(); usleep(500); }
    int on0 = sched_getcpu();
    b_loop(200000000);                                     /* ~50-150 ms of load so the governor raises the clock */
    char p[96]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", q->last);
    long f0 = rd_long(p);
    double loop = run_ns(b_loop, 100000000);
    long f1 = rd_long(p);
    unsigned long hw = getauxval(AT_HWCAP);
    uint64_t midr = 0, dcz = 0;
    if (hw & HWCAP_CPUID) __asm__ volatile("mrs %0, midr_el1" : "=r"(midr));
    __asm__ volatile("mrs %0, dczid_el0" : "=r"(dcz));
    s_zva_block = (dcz & 16) ? 0 : (4l << (dcz & 15));
    double cnt = run_ns(b_cntvct, 1000000), ctr = run_ns(b_ctr, 1000000), mid = (hw & HWCAP_CPUID) ? run_ns(b_midr, 20000) : -1;
    double llsc = run_ns(b_llsc, 2000000), lse = (hw & HWCAP_ATOMICS) ? run_ns(b_lse, 2000000) : -1, cas = (hw & HWCAP_ATOMICS) ? run_ns(b_cas, 2000000) : -1;
    double ldar = run_ns(b_ldar, 4000000), lda = run_ns(b_ld_aligned, 4000000), ldx = run_ns(b_ld_cross, 4000000);
    double mcp = run_ns(b_memcpy, 8), mst = run_ns(b_memset, 8), zva = s_zva_block ? run_ns(b_zva, 8) : -1;
    double mal = run_ns(b_malloc, 400000), malb = run_ns(b_malloc_big, 2000);
    double flt = run_ns(b_fault, 8192), mmp = run_ns(b_mmap, 4000), fls = run_ns(b_flush, 2000);
    double mtx = run_ns(b_mutex, 1000000), clk = run_ns(b_clock, 1000000), sys = run_ns(b_syscall, 200000), yld = run_ns(b_yield, 20000);
    double fma = run_ns(b_fma, 20000000);
    double gb = (double)BUF_SZ;
    int on1 = sched_getcpu();
    if (on0 != q->last || on1 != q->last) f2_log("F2: [bench] %s: asked for cpu%d, ran on cpu%d..cpu%d (the system moved the thread)", when, q->last, on0, on1);
    f2_log("F2: [bench] %s cpu%d (policy%d, midr 0x%llx, clock %ld->%ld MHz): loop %.0f MHz-equiv | ns/op: cntvct %.2f ctr_el0 %.2f midr(trap) %.0f | "
           "ldaxr/stlxr %.2f ldaddal %.2f casal %.2f ldar %.2f ld %.2f ld-cross-line %.2f | fmadd chain %.2f",
           when, q->last, q->id, (unsigned long long)midr, f0 / 1000, f1 / 1000, 1000.0 / loop, cnt, ctr, mid, llsc, lse, cas, ldar, lda, ldx, fma);
    f2_log("F2: [bench] %s cpu%d: memcpy %.1f GB/s memset %.1f GB/s dc-zva %.1f GB/s (block %ld) | malloc64 %.0f ns malloc256K %.0f ns | page fault %.0f ns "
           "mmap+munmap64K %.0f ns clear_cache64K %.0f ns | mutex %.1f ns clock_gettime %.1f ns syscall %.0f ns sched_yield %.0f ns",
           when, q->last, gb / mcp, gb / mst, zva > 0 ? gb / zva : -1.0, s_zva_block, mal, malb, flt, mmp, fls, mtx, clk, sys, yld);
}

static void bench_all(const char *when)
{
    if (!s_buf) {
        s_buf = mmap(NULL, BUF_SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        s_buf2 = mmap(NULL, BUF_SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (s_buf == MAP_FAILED || s_buf2 == MAP_FAILED) { s_buf = NULL; f2_log("F2: [bench] no memory"); return; }
        memset(s_buf, 1, BUF_SZ); memset(s_buf2, 2, BUF_SZ);
    }
    cpu_set_t all; CPU_ZERO(&all); sched_getaffinity(0, sizeof all, &all);
    for (int i = 0; i < s_npol; i++) bench_cluster(&s_pol[i], when);
    sched_setaffinity(0, sizeof all, &all);
}

/* ---------- [thr] every 10 s ---------- */
#define TMAX 512
typedef struct { int tid; unsigned long long ticks; } tprev_t;
static tprev_t s_prev[TMAX];
static int s_nprev;
static unsigned long long prev_ticks(int tid) { for (int i = 0; i < s_nprev; i++) if (s_prev[i].tid == tid) return s_prev[i].ticks; return 0; }
typedef struct { int tid, core; double pct; char name[20]; } tcur_t;
static int cmp_tcur(const void *a, const void *b) { const tcur_t *x = a, *y = b; return x->pct < y->pct ? 1 : x->pct > y->pct ? -1 : 0; }
static int stat_of(int tid, unsigned long long *ticks, int *core)
{
    char p[64], b[1024];
    snprintf(p, sizeof p, "/proc/self/task/%d/stat", tid);
    FILE *f = fopen(p, "r"); if (!f) return 0;
    size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
    char *s = strrchr(b, ')'); if (!s) return 0;
    s += 2;
    unsigned long long ut = 0, st = 0; int field = 3, cpu = -1; char *save = NULL;
    for (char *t = strtok_r(s, " ", &save); t; t = strtok_r(NULL, " ", &save), field++) {
        if (field == 14) ut = strtoull(t, NULL, 10);
        else if (field == 15) st = strtoull(t, NULL, 10);
        else if (field == 39) { cpu = atoi(t); break; }
    }
    *ticks = ut + st; *core = cpu; return 1;
}
static void schedstat(int tid, unsigned long long *run, unsigned long long *wait)
{
    char p[64], b[128]; *run = *wait = 0;
    snprintf(p, sizeof p, "/proc/self/task/%d/schedstat", tid);
    if (rd_str(p, b, sizeof b)) sscanf(b, "%llu %llu", run, wait);
}

static void thr_window(double win_s)
{
    static tcur_t cur[TMAX];
    static tprev_t next[TMAX];
    int nc = 0, nn = 0; double total = 0;
    long hz = sysconf(_SC_CLK_TCK); if (hz <= 0) hz = 100;
    DIR *d = opendir("/proc/self/task");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && nn < TMAX) {
        int tid = atoi(e->d_name);
        if (tid <= 0) continue;
        unsigned long long t; int core;
        if (!stat_of(tid, &t, &core)) continue;
        next[nn].tid = tid; next[nn].ticks = t; nn++;
        unsigned long long pt = prev_ticks(tid);
        if (!s_nprev || t < pt) continue;
        double pct = (double)(t - pt) / hz / win_s * 100.0;
        total += pct;
        if (pct < 1.0) continue;
        tcur_t *c = &cur[nc++];
        c->tid = tid; c->core = core; c->pct = pct;
        char p[64]; snprintf(p, sizeof p, "/proc/self/task/%d/comm", tid);
        if (!rd_str(p, c->name, sizeof c->name)) snprintf(c->name, sizeof c->name, "?");
        for (char *q = c->name; *q; q++) if (*q == ' ') *q = '_';
    }
    closedir(d);
    int had = s_nprev;
    memcpy(s_prev, next, sizeof(tprev_t) * (size_t)nn); s_nprev = nn;
    if (!had) return;
    qsort(cur, (size_t)nc, sizeof *cur, cmp_tcur);
    char line[900]; size_t w = (size_t)snprintf(line, sizeof line, "F2: [thr] 10s: process %.0f%% of one core over %d threads | busiest:", total, nn);
    for (int i = 0; i < nc && i < 12 && w < sizeof line - 48; i++)
        w += (size_t)snprintf(line + w, sizeof line - w, " %s%s(%d) %.0f%%@cpu%d", cur[i].name, cur[i].tid == s_main ? "[main]" : cur[i].tid == s_render ? "[render]" : "",
                              cur[i].tid, cur[i].pct, cur[i].core);
    f2_log("%s", line);
    /* run-queue wait of the main / render thread */
    static unsigned long long mr0, mw0, rr0, rw0;
    unsigned long long mr, mw, rr = 0, rw = 0;
    schedstat(s_main, &mr, &mw);
    if (s_render) schedstat(s_render, &rr, &rw);
    char caps[500]; size_t cw = 0; caps[0] = 0;
    for (int i = 0; i < s_npol && cw < sizeof caps - 80; i++) {
        char p[96]; int id = s_pol[i].id;
        pol_path(p, sizeof p, id, "scaling_cur_freq"); long cur_f = rd_long(p);
        pol_path(p, sizeof p, id, "scaling_max_freq"); long mx = rd_long(p);
        pol_path(p, sizeof p, id, "cpuinfo_max_freq"); long hw = rd_long(p);
        pol_path(p, sizeof p, id, "scaling_min_freq"); long mn = rd_long(p);
        cw += (size_t)snprintf(caps + cw, sizeof caps - cw, "%s[%d-%d] %ld (allowed %ld-%ld of %ld)", i ? ", " : "", s_pol[i].first, s_pol[i].last,
                               cur_f / 1000, mn / 1000, mx / 1000, hw / 1000);
    }
    char online[64] = "?"; rd_str("/sys/devices/system/cpu/online", online, sizeof online);
    f2_log("F2: [thr] main waited %.0f ms for a core (ran %.0f ms), render waited %.0f ms (ran %.0f ms) | MHz %s | online %s",
           mr0 ? (mw - mw0) / 1e6 : -1.0, mr0 ? (mr - mr0) / 1e6 : -1.0, rr0 ? (rw - rw0) / 1e6 : -1.0, rr0 ? (rr - rr0) / 1e6 : -1.0, caps, online);
    mr0 = mr; mw0 = mw; rr0 = rr; rw0 = rw;
    static char last_sd[300];
    char sd[300]; sched_desc(s_main, sd, sizeof sd);
    if (strcmp(sd, last_sd)) { f2_log("F2: [thr] main thread scheduling now: %s", sd); snprintf(last_sd, sizeof last_sd, "%s", sd); }
}

static void *diag_thread(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "f2diag");
    sleep(2);
    sys_once();
    sleep(3);
    bench_all("start");
    long long t0 = mono(), last = mono();
    int bench2 = 0, bench3 = 0;
    for (;;) {
        sleep(10);
        long long now = mono();
        thr_window((now - last) / 1e9); last = now;
        if (!bench2 && now - t0 > 110000000000ll) { bench2 = 1; bench_all("in game"); }
        if (!bench3 && now - t0 > 400000000000ll) { bench3 = 1; bench_all("after 7 min"); }
    }
    return NULL;
}

/* from f2_perf.c: the main thread is known (once) */
void f2_sysdiag_start(int main_tid)
{
    if (!f2_cfg.sysdiag || s_main) return;
    s_main = main_tid;
    pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, diag_thread, NULL) != 0) f2_log("F2: [sys] diagnostics thread not started");
    pthread_attr_destroy(&a);
}
void f2_sysdiag_note_render(int tid) { if (!s_render) s_render = tid; }
