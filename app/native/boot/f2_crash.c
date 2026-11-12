/* F2 crash handler: SIGSEGV / SIGBUS / SIGILL / SIGFPE / SIGABRT / SIGTRAP.
 * Writes into logcat (tag F2) and f2_log.txt: signal, fault address, registers, pc / lr as module+offset
 * (the game image is "main+0x<Switch main address>"), a frame-pointer backtrace, a stack scan for return
 * addresses, the last runtime_invoke'd managed methods and the managed sentry stack -- then hands the signal
 * to whoever was installed before (ART / libunity / debuggerd, so the tombstone is still written).
 * tools/symbolize.py turns "main+0x.." / "libunity.so+0x.." / "libil2cpp.so+0x.." into names on the PC. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <dirent.h>
#include <ucontext.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include "f2_boot.h"
#include "../loader/f2_gen_gameinfo.h"

/* A short marker surviving the process's death, for CrashLog.java (java/.../CrashLog.java) at the NEXT boot: the full report already
 * lands in f2_log.txt (f2_log_raw below), but that file is only reachable with a file manager that can see Android/data; this one line
 * is read by Java (internal files dir, always ours) and turned into a player-reachable crash.txt. Kept to syscalls only (open/write/close
 * are async-signal-safe); the snprintf calls below follow the same convention the rest of this handler already relies on. */
static void write_native_marker(int sig, const siginfo_t *si, uintptr_t pc, const char *pc_desc)
{
    const char *dir = f2_cfg.internal_dir[0] ? f2_cfg.internal_dir : f2_cfg.files_dir;
    if (!dir[0]) return;
    char path[700], line[400];
    int pn = snprintf(path, sizeof path, "%s/f2_native_crash.txt", dir);
    if (pn <= 0 || pn >= (int)sizeof path) return;
    int n = snprintf(line, sizeof line, "signal=%d code=%d addr=%p pc=0x%lx(%s) time=%ld\n",
                      sig, si->si_code, si->si_addr, (unsigned long)pc, pc_desc ? pc_desc : "?", (long)time(NULL));
    if (n < 0) return;
    if (n > (int)sizeof line) n = (int)sizeof line;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { (void)!write(fd, line, (size_t)n); close(fd); }
}

typedef struct { uintptr_t lo, hi; char name[64]; } f2_module_t;
#define MAX_MODULES 400
static f2_module_t s_mod[MAX_MODULES];
static volatile int s_nmod;
static int s_pipe[2] = { -1, -1 };
static const int k_sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP };
#define NSIG_F2 (int)(sizeof k_sigs / sizeof k_sigs[0])
static struct sigaction s_old[NSIG_F2];
static volatile int s_in_handler;

static void handler(int sig, siginfo_t *si, void *uc);

static int mod_cb(struct dl_phdr_info *info, size_t sz, void *data)
{
    (void)sz;
    int *n = (int *)data;
    if (*n >= MAX_MODULES) return 1;
    uintptr_t lo = ~(uintptr_t)0, hi = 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD) continue;
        uintptr_t a = info->dlpi_addr + ph->p_vaddr;
        if (a < lo) lo = a;
        if (a + ph->p_memsz > hi) hi = a + ph->p_memsz;
    }
    if (hi <= lo) return 0;
    const char *name = info->dlpi_name && info->dlpi_name[0] ? info->dlpi_name : "(exe)";
    const char *slash = strrchr(name, '/');
    f2_module_t *m = &s_mod[*n];
    m->lo = lo; m->hi = hi;
    snprintf(m->name, sizeof m->name, "%s", slash ? slash + 1 : name);
    (*n)++;
    return 0;
}

void f2_crash_refresh_modules(void)
{
    static f2_module_t tmp[1];
    (void)tmp;
    int n = 0;
    /* the list is rebuilt in place; a crash during the rebuild sees a shorter list, never garbage names */
    s_nmod = 0;
    dl_iterate_phdr(mod_cb, &n);
    s_nmod = n;
}

int f2_describe_game_addr(uintptr_t addr, char *out, size_t n)
{
    uintptr_t b = f2_game_base();
    if (!b || addr < b || addr >= b + F2_PATCH_OFF + 0x200000) return 0;
    uintptr_t off = addr - b;
    const char *zone = off < F2_TEXT_END ? "" : off < F2_BSS_START ? " [rodata/data]" : off < F2_PATCH_OFF ? " [bss/key page]" : " [x18 stubs / veneers]";
    snprintf(out, n, "main+0x%lx%s", (unsigned long)off, zone);
    return 1;
}

int f2_describe_addr(uintptr_t addr, char *out, size_t n)
{
    if (f2_describe_game_addr(addr, out, n)) return 1;
    int cnt = s_nmod;
    for (int i = 0; i < cnt; i++)
        if (addr >= s_mod[i].lo && addr < s_mod[i].hi) {
            if (!strcmp(s_mod[i].name, "main.f2")) continue;
            snprintf(out, n, "%s+0x%lx", s_mod[i].name, (unsigned long)(addr - s_mod[i].lo));
            return 1;
        }
    return 0;
}

static void out(const char *line) { f2_log_raw(line); }

/* EFAULT-safe read of 16 bytes */
static int safe_read(uintptr_t addr, uint64_t v[2])
{
    if (s_pipe[1] < 0 || addr < 0x10000 || (addr & 7)) return 0;
    if (write(s_pipe[1], (void *)addr, 16) != 16) return 0;
    return read(s_pipe[0], v, 16) == 16;
}

static void chain(int sig, siginfo_t *si, void *uc)
{
    int idx = -1;
    for (int i = 0; i < NSIG_F2; i++) if (k_sigs[i] == sig) idx = i;
    if (idx < 0) return;
    struct sigaction *o = &s_old[idx];
    if ((o->sa_flags & SA_SIGINFO) && o->sa_sigaction && (void *)o->sa_sigaction != (void *)handler) {
        o->sa_sigaction(sig, si, uc);
        return;
    }
    if (!(o->sa_flags & SA_SIGINFO) && o->sa_handler != SIG_DFL && o->sa_handler != SIG_IGN && o->sa_handler) {
        o->sa_handler(sig);
        return;
    }
    /* default action: put the default handler back and let the fault happen again */
    signal(sig, SIG_DFL);
    if (sig == SIGABRT || si->si_code <= 0) syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), sig);
}

static void handler(int sig, siginfo_t *si, void *ucv)
{
    if (__atomic_exchange_n(&s_in_handler, 1, __ATOMIC_SEQ_CST)) {
        /* our handler is re-installed on top of libunity's own (which chains back to the one it replaced = ours):
         * bound the ping-pong, then let the default action kill the process */
        static int s_depth;
        if (__atomic_add_fetch(&s_depth, 1, __ATOMIC_SEQ_CST) > 2) { signal(sig, SIG_DFL); return; }
        /* a second fault while reporting (or another thread crashing at the same time): do not report twice */
        if (s_in_handler == 2) { chain(sig, si, ucv); return; }
        s_in_handler = 2;
        out("F2: CRASH: second fault while writing the report; handing over");
        chain(sig, si, ucv);
        return;
    }
    ucontext_t *uc = (ucontext_t *)ucv;
    struct sigcontext *sc = &uc->uc_mcontext;
    uintptr_t pc = sc->pc, lr = sc->regs[30], sp = sc->sp, fp = sc->regs[29];
    char line[512], a[160], b[160], tname[32] = "?";
    prctl(PR_GET_NAME, tname, 0, 0, 0);
    const char *sn = sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : sig == SIGILL ? "SIGILL" : sig == SIGFPE ? "SIGFPE" :
                     sig == SIGABRT ? "SIGABRT" : sig == SIGTRAP ? "SIGTRAP" : "signal";
    out("F2: ================= CRASH =================");
    snprintf(line, sizeof line, "F2: CRASH %s (code %d) fault addr %p, thread %d \"%s\", libunity %s at %p, game image at %p",
             sn, si->si_code, si->si_addr, (int)syscall(SYS_gettid), tname, f2_unity_variant(), (void *)f2_unity_base(), (void *)f2_game_base());
    out(line);
    if (!f2_describe_addr(pc, a, sizeof a)) snprintf(a, sizeof a, "?");
    if (!f2_describe_addr(lr, b, sizeof b)) snprintf(b, sizeof b, "?");
    snprintf(line, sizeof line, "F2: CRASH pc %016lx = %s | lr %016lx = %s | sp %016lx fp %016lx", (unsigned long)pc, a, (unsigned long)lr, b,
             (unsigned long)sp, (unsigned long)fp);
    out(line);
    write_native_marker(sig, si, pc, a);   /* survives the process dying; CrashLog.java reads it at the next boot (f2_native_crash.txt) */
    /* what kind of trap is it */
    uintptr_t gb = f2_game_base();
    if (gb && pc >= gb && pc < gb + F2_TEXT_END) {
        uint32_t insn;
        memcpy(&insn, (void *)pc, 4);
        if (insn == (0xd4200000u | (0xf2u << 5))) {
            snprintf(line, sizeof line, "F2: CRASH = POISONED Switch-runtime entry main+0x%lx reached through a data pointer (needs a mapping in rt_map.tsv); called from %s",
                     (unsigned long)(pc - gb), b);
            out(line);
        } else if (pc - gb >= 0xa68e20 && pc - gb < 0xaea5d0) {
            snprintf(line, sizeof line, "F2: CRASH inside the Switch libil2cpp runtime area of main (0xa68e20..0xaea5d0): an entry that was not redirected");
            out(line);
        } else if (pc - gb >= 0x1713000) {
            snprintf(line, sizeof line, "F2: CRASH inside the Switch ENGINE part of main (>= 0x1713000): game code jumped into the Switch player");
            out(line);
        }
    }
    for (int i = 0; i < 30; i += 5) {
        snprintf(line, sizeof line, "F2:   x%-2d %016lx x%-2d %016lx x%-2d %016lx x%-2d %016lx x%-2d %016lx", i, (unsigned long)sc->regs[i], i + 1,
                 (unsigned long)sc->regs[i + 1], i + 2, (unsigned long)sc->regs[i + 2], i + 3, (unsigned long)sc->regs[i + 3], i + 4, (unsigned long)sc->regs[i + 4]);
        out(line);
    }
    /* registers that point into known modules */
    for (int i = 0; i < 31; i++)
        if (f2_describe_addr(sc->regs[i], a, sizeof a)) { snprintf(line, sizeof line, "F2:   x%d -> %s", i, a); out(line); }
    /* frame pointer chain */
    out("F2: backtrace (frame pointers):");
    snprintf(line, sizeof line, "F2:   #00 pc %s", f2_describe_addr(pc, a, sizeof a) ? a : "?");
    out(line);
    uintptr_t f = fp;
    int depth = 1;
    for (; depth < 48; depth++) {
        uint64_t v[2];
        if (!safe_read(f, v)) break;
        if (!f2_describe_addr(v[1], a, sizeof a)) snprintf(a, sizeof a, "%016lx", (unsigned long)v[1]);
        snprintf(line, sizeof line, "F2:   #%02d %s", depth, a);
        out(line);
        if (v[0] <= f || v[0] - f > 0x400000) break;
        f = v[0];
    }
    /* stack scan: anything on the stack that looks like a return address into a known module */
    out("F2: stack scan (words that point into code of known modules):");
    int found = 0;
    for (uintptr_t p = sp & ~(uintptr_t)15; found < 40 && p < sp + 0x3000; p += 16) {
        uint64_t v[2];
        if (!safe_read(p, v)) break;
        for (int k = 0; k < 2; k++)
            if (v[k] > 0x10000 && !(v[k] & 3) && f2_describe_addr(v[k], a, sizeof a) && !strstr(a, "[rodata") && !strstr(a, "[bss")) {
                snprintf(line, sizeof line, "F2:   sp+0x%04lx %s", (unsigned long)(p - sp + 8 * k), a);
                out(line);
                found++;
            }
    }
    f2_diag_dump_invokes(out);
    f2_diag_dump_managed_stack(out);
    out("F2: ================= end of crash report (the signal goes on to the previous handler) =================");
    fsync(f2_log_fd());
    s_in_handler = 2;
    chain(sig, si, ucv);
}

/* ------------------------------------------------------------------ stall report (the heartbeat, f2_boot.c)
 * The in-process twin of tools/phone_stacks.py, for a tester's log: for every thread of the process its name, state, the system call it
 * sits in and the user pc (/proc/self/task/<tid>/syscall), then the words of its stack (from that sp up) that are return addresses into
 * code: in an executable mapping AND right after a BL / BLR.  A scan, not an unwind: a stale frame left by an earlier call can show up;
 * read each line left to right (innermost first).  Reads go through /proc/self/mem (an unmapped address is an error, never a fault).
 * tools/symbolize.py turns "libunity.so+0x.." into names on the PC. */
typedef struct { uintptr_t lo, hi; } f2_xrange_t;
static f2_xrange_t s_x[2048];
static int s_nx;
static uintptr_t s_va_mask;
static int in_code(uintptr_t a)
{
    int lo = 0, hi = s_nx - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (a < s_x[m].lo) hi = m - 1; else if (a >= s_x[m].hi) lo = m + 1; else return 1; }
    return 0;
}
static int read_file(const char *path, char *out, size_t n)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { out[0] = 0; return -1; }
    ssize_t r = read(fd, out, n - 1);
    close(fd);
    if (r < 0) r = 0;
    out[r] = 0;
    while (r > 0 && (out[r - 1] == '\n' || out[r - 1] == ' ')) out[--r] = 0;
    return (int)r;
}
static const char *sys_name(long nr)
{
    switch (nr) {
    case 98: return "futex"; case 101: return "nanosleep"; case 115: return "clock_nanosleep"; case 73: return "ppoll"; case 22: return "epoll_pwait";
    case 63: return "read"; case 64: return "write"; case 29: return "ioctl"; case 72: return "pselect6"; case 207: return "recvfrom"; case 56: return "openat";
    case 67: return "pread64"; case 124: return "sched_yield"; case 130: return "tkill"; case 131: return "tgkill"; default: return "?";
    }
}
void f2_crash_dump_threads(const char *why)
{
    char line[1000], a[160], path[96], comm[40], stat[400], sc[300], wchan[64];
    f2_crash_refresh_modules();
    /* executable mappings, sorted as the kernel lists them */
    s_nx = 0; s_va_mask = 0;
    FILE *mp = fopen("/proc/self/maps", "r");
    if (mp) {
        char ml[600];
        uintptr_t top = 0;
        while (fgets(ml, sizeof ml, mp)) {
            unsigned long lo, hi; char perm[8];
            if (sscanf(ml, "%lx-%lx %7s", &lo, &hi, perm) != 3) continue;
            if (hi > top) top = hi;
            if (perm[2] == 'x' && s_nx < (int)(sizeof s_x / sizeof s_x[0])) { s_x[s_nx].lo = lo; s_x[s_nx].hi = hi; s_nx++; }
        }
        fclose(mp);
        for (s_va_mask = 1; s_va_mask && s_va_mask < top; s_va_mask <<= 1) {}
        s_va_mask = s_va_mask ? s_va_mask - 1 : ~(uintptr_t)0;
    }
    int mem = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    DIR *d = opendir("/proc/self/task");
    int nthreads = 0;
    if (d) { struct dirent *e; while ((e = readdir(d))) if (e->d_name[0] != '.') nthreads++; rewinddir(d); }
    f2_log("F2: ======== stall report: %s; %d threads; libunity %s at %p, game image at %p ========", why, nthreads,
           f2_unity_variant(), (void *)f2_unity_base(), (void *)f2_game_base());
    f2_log("F2: stall: per thread: state / system call / user pc, then return addresses found on its stack (innermost first; a scan: stale frames possible)");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            int tid = atoi(e->d_name);
            snprintf(path, sizeof path, "/proc/self/task/%d/comm", tid); read_file(path, comm, sizeof comm);
            snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid); read_file(path, stat, sizeof stat);
            snprintf(path, sizeof path, "/proc/self/task/%d/wchan", tid); read_file(path, wchan, sizeof wchan);
            snprintf(path, sizeof path, "/proc/self/task/%d/syscall", tid); read_file(path, sc, sizeof sc);
            const char *rp = strrchr(stat, ')');
            char state = rp && rp[1] == ' ' ? rp[2] : '?';
            /* syscall: "nr a1 .. a6 sp pc" (blocked in a call), "-1 sp pc" (blocked, no call), "running" */
            long nr = -2; unsigned long sp = 0, pc = 0;
            if (strncmp(sc, "running", 7) != 0) {
                char *tok[12], *save = NULL; int nt = 0;
                for (char *s = strtok_r(sc, " ", &save); s && nt < 12; s = strtok_r(NULL, " ", &save)) tok[nt++] = s;
                if (nt >= 3) { nr = strtol(tok[0], NULL, 0); sp = strtoul(tok[nt - 2], NULL, 0); pc = strtoul(tok[nt - 1], NULL, 0); }
            }
            int w = 0;
            if (nr == -2) w = snprintf(line, sizeof line, "F2: stall: %5d %-16s %c running (on a CPU: no pc without a signal)", tid, comm, state);
            else {
                if (!f2_describe_addr(pc, a, sizeof a)) snprintf(a, sizeof a, "%lx", pc);
                w = snprintf(line, sizeof line, "F2: stall: %5d %-16s %c %s%s %ld(%s) pc %s |", tid, comm, state, wchan[0] && strcmp(wchan, "0") ? wchan : "",
                             wchan[0] && strcmp(wchan, "0") ? "" : "-", nr, nr >= 0 ? sys_name(nr) : "none", a);
            }
            if (sp && mem >= 0 && w > 0) {
                static uint64_t stk[0x4000 / 8];
                ssize_t got = pread(mem, stk, sizeof stk, (off_t)(sp & ~(uintptr_t)7));
                int frames = 0;
                for (ssize_t i = 0; got > 0 && i < got / 8 && frames < 16 && w < (int)sizeof line - 60; i++) {
                    uintptr_t v = (uintptr_t)stk[i];
                    if (!in_code(v)) { v &= s_va_mask; if (!in_code(v)) continue; }
                    uint32_t insn = 0;
                    if (v < 4 || !in_code(v - 4) || pread(mem, &insn, 4, (off_t)(v - 4)) != 4) continue;
                    if ((insn & 0xfc000000u) != 0x94000000u && (insn & 0xfffffc1fu) != 0xd63f0000u) continue;   /* bl / blr before it */
                    if (!f2_describe_addr(v, a, sizeof a)) snprintf(a, sizeof a, "%lx", (unsigned long)v);
                    w += snprintf(line + w, sizeof line - w, " %s", a);
                    frames++;
                }
            }
            f2_log("%s", line);
        }
        closedir(d);
    }
    if (mem >= 0) close(mem);
    f2_diag_dump_invokes(out);
    f2_log("F2: ======== end of stall report ========");
    fsync(f2_log_fd());
}

void f2_crash_install(void)
{
    if (s_pipe[0] < 0) {
        if (pipe(s_pipe) == 0) {
            fcntl(s_pipe[0], F_SETFL, O_NONBLOCK); fcntl(s_pipe[1], F_SETFL, O_NONBLOCK);
            fcntl(s_pipe[0], F_SETFD, FD_CLOEXEC); fcntl(s_pipe[1], F_SETFD, FD_CLOEXEC);
        }
    }
    f2_crash_refresh_modules();
    int n = 0;
    for (int i = 0; i < NSIG_F2; i++) {
        struct sigaction cur, sa;
        if (sigaction(k_sigs[i], NULL, &cur) != 0) continue;
        if ((cur.sa_flags & SA_SIGINFO) && (void *)cur.sa_sigaction == (void *)handler) continue;   /* already ours on top */
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = handler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        if (sigaction(k_sigs[i], &sa, &s_old[i]) == 0) n++;
    }
    if (n) f2_log("F2: crash handler installed for %d signals (%d modules known)", n, s_nmod);
}
