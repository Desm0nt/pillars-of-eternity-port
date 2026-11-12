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
