/* F2 diagnostics on the il2cpp side:
 *   - wrappers handed to libunity for a few il2cpp_* API functions (through the dlsym hook): il2cpp_init,
 *     il2cpp_set_data_dir, il2cpp_runtime_invoke, il2cpp_add_internal_call, il2cpp_unhandled_exception
 *   - wrappers for three runtime entries the GAME code calls (the loader substitutes them in its redirect table):
 *     Exception::Raise (every managed throw, with its class, message and the throwing address in main),
 *     InternalCalls::Resolve (unresolved icall names), PlatformInvoke::Resolve (P/Invokes that go by name)
 *   - the last runtime_invoke'd methods and the managed (sentry) stack for the crash handler. */
#include "il2cpp-config.h"
#include "il2cpp-class-internals.h"
#include "il2cpp-object-internals.h"
#include "il2cpp-api.h"
#include "vm/Exception.h"
#include "vm/InternalCalls.h"
#include "vm/PlatformInvoke.h"
#include "vm/StackTrace.h"
#include "utils/StringUtils.h"
#include <string>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "f2_boot.h"

extern "C" void f2_hooks_il2cpp_lookup_stats(unsigned *n, unsigned *missing);

static unsigned long s_invokes, s_throws, s_icalls_added, s_nre;
static const MethodInfo *volatile s_last_invoke;
#define RING 64
static const MethodInfo *s_ring[RING];
static unsigned s_ring_pos;

static void method_name(const MethodInfo *m, char *out, size_t n)
{
    if (!m) { snprintf(out, n, "(null)"); return; }
    Il2CppClass *k = m->klass;
    snprintf(out, n, "%s%s%s::%s", (k && k->namespaze && k->namespaze[0]) ? k->namespaze : "", (k && k->namespaze && k->namespaze[0]) ? "." : "",
             k && k->name ? k->name : "?", m->name ? m->name : "?");
}

static void utf16(Il2CppString *s, char *out, size_t n)
{
    out[0] = 0;
    if (!s) return;
    int len = s->length;
    if (len < 0) len = 0;
    if (len > 300) len = 300;
    std::string u = il2cpp::utils::StringUtils::Utf16ToUtf8(s->chars, len);
    snprintf(out, n, "%s", u.c_str());
    for (char *p = out; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
}

static void describe_exception(Il2CppException *ex, char *out, size_t n)
{
    if (!ex) { snprintf(out, n, "(null exception)"); return; }
    Il2CppClass *k = ((Il2CppObject *)ex)->klass;
    char msg[400];
    utf16(ex->message, msg, sizeof msg);
    int w = snprintf(out, n, "%s%s%s: %s", (k && k->namespaze && k->namespaze[0]) ? k->namespaze : "", (k && k->namespaze && k->namespaze[0]) ? "." : "",
                     k && k->name ? k->name : "?", msg);
    Il2CppException *in = ex->inner_ex;
    if (in && w > 0 && (size_t)w < n) {
        Il2CppClass *ik = ((Il2CppObject *)in)->klass;
        utf16(in->message, msg, sizeof msg);
        snprintf(out + w, n - w, "  <- inner %s: %s", ik && ik->name ? ik->name : "?", msg);
    }
}

/* ------------------------------------------------------------------ il2cpp API wrappers (libunity side) */
extern "C" void f2_engine_icall(const char *name, void (*fn)(void));
static void (*r_init)(const char *);
static void (*r_set_data_dir)(const char *);
static Il2CppObject *(*r_runtime_invoke)(const MethodInfo *, void *, void **, Il2CppException **);
static void (*r_add_internal_call)(const char *, Il2CppMethodPointer);
static void (*r_unhandled_exception)(Il2CppException *);

static void w_init(const char *domain)
{
    unsigned n = 0, miss = 0;
    f2_hooks_il2cpp_lookup_stats(&n, &miss);
    f2_log("F2: LoadIl2Cpp looked up %u il2cpp_* functions, %u missing; libunity registered %lu internal calls so far; il2cpp_init(\"%s\") ...",
           n, miss, s_icalls_added, domain ? domain : "");
    r_init(domain);
    size_t count = 0;
    il2cpp_domain_get_assemblies(il2cpp_domain_get(), &count);
    f2_once_milestone(M_IL2CPP_INIT, "il2cpp_init returned: %zu assemblies, game image at %p", count, (void *)f2_game_base());
    f2_crash_refresh_modules();
}

static void w_set_data_dir(const char *dir)
{
    /* clean APK (no Managed/ shipped): the engine still calls this with whatever it extracted from the APK's assets (possibly
     * nothing, if assets/bin/Data/Managed is empty/absent -- that extraction step itself was not seen to check a file count: no
     * "Managed"/"Metadata" path string exists in libunity.so's own strings, only the bare literal "/il2cpp", consistent with it
     * extracting whatever is there, even zero files, without refusing to go on). OUR runtime (this libil2cpp.so) is what actually
     * reads global-metadata.dat/*.dat later, using whatever path is stored by r_set_data_dir below -- so substituting it here,
     * before that read happens, is enough regardless of what the engine passed in. f2_cfg.data_dir is the loose Switch Data
     * folder (RomFS/Data, from setup_f2.py or the adb-pushed dev copy), which already has a Managed/ subfolder with these files. */
    char redirected[600];
    const char *use = dir;
    if (f2_data_dir()) {
        snprintf(redirected, sizeof redirected, "%s/Managed", f2_data_dir());
        char probe[700];
        snprintf(probe, sizeof probe, "%s/Metadata/global-metadata.dat", redirected);
        if (access(probe, R_OK) == 0) use = redirected;
    }
    f2_log("F2: il2cpp_set_data_dir(%s)%s  (global-metadata.dat is read from <that>/Metadata)",
           use ? use : "(null)", use != dir ? (dir && *dir ? " [redirected from the engine's own extraction]" : " [the APK ships no Managed/: redirected to the data folder]") : "");
    r_set_data_dir(use);
}

/* The engine is the stock ANDROID player, the managed UnityEngine is the SWITCH build: managed callbacks the Android engine
 * expects (UnityConnect / Analytics / RemoteSettings, _AndroidJNIHelper ...) do not exist, the engine logs "Unable to find
 * method ..." and later invokes the NULL ScriptingMethodPtr anyway (phone run 04: SIGSEGV in il2cpp_method_get_param_count(NULL)
 * from scripting_method_invoke).  A missing callback is treated as "nobody listens": the call is a no-op. */
static void where(void *ret, char *out, size_t n);
static uint32_t (*r_method_get_param_count)(const MethodInfo *);
static Il2CppObject *(*r_runtime_invoke_convert_args)(const MethodInfo *, void *, Il2CppObject **, int, Il2CppException **);
static unsigned long s_null_invokes;

static void log_null_method(const char *api)
{
    unsigned long n = __atomic_add_fetch(&s_null_invokes, 1, __ATOMIC_RELAXED);
    if (n > 40) return;
    char line[1100];
    int w = snprintf(line, sizeof line, "F2: %s(NULL method) ignored #%lu; callers:", api, n);
    void **fp = (void **)__builtin_frame_address(0);
    for (int i = 0; i < 8 && fp && w > 0 && (size_t)w < sizeof line - 220; i++) {
        char d[200];
        where(fp[1], d, sizeof d);
        w += snprintf(line + w, sizeof line - w, " <- %s", d);
        void **next = (void **)fp[0];
        if (next <= fp || (uintptr_t)next - (uintptr_t)fp > 0x100000) break;
        fp = next;
    }
    f2_log("%s", line);
}

static uint32_t w_method_get_param_count(const MethodInfo *method)
{
    if (!method) { log_null_method("il2cpp_method_get_param_count"); return 0; }
    return r_method_get_param_count(method);
}

static Il2CppObject *w_runtime_invoke_convert_args(const MethodInfo *method, void *obj, Il2CppObject **params, int count, Il2CppException **exc)
{
    if (!method) { log_null_method("il2cpp_runtime_invoke_convert_args"); if (exc) *exc = NULL; return NULL; }
    return r_runtime_invoke_convert_args(method, obj, params, count, exc);
}

static Il2CppObject *w_runtime_invoke(const MethodInfo *method, void *obj, void **params, Il2CppException **exc)
{
    if (!method) { log_null_method("il2cpp_runtime_invoke"); if (exc) *exc = NULL; return NULL; }
    unsigned long n = __atomic_add_fetch(&s_invokes, 1, __ATOMIC_RELAXED);
    s_last_invoke = method;
    s_ring[__atomic_fetch_add(&s_ring_pos, 1, __ATOMIC_RELAXED) % RING] = method;
    char name[300];
    if (n == 1) { method_name(method, name, sizeof name); f2_once_milestone(M_FIRST_INVOKE, "first il2cpp_runtime_invoke from libunity: %s", name); }
    else if ((long)n <= f2_cfg.invoke_log) { method_name(method, name, sizeof name); f2_log("F2: invoke #%lu %s", n, name); }
    if (method && method->klass && method->name && !strcmp(method->name, "Awake") && method->klass->name &&
        !strcmp(method->klass->name, "GameState"))
        f2_once_milestone(M_GAME_BOOT, "the game's GameState.Awake is being invoked (boot entry -> ConsoleBootstrap)");
    Il2CppException *local = NULL;
    Il2CppObject *r = r_runtime_invoke(method, obj, params, exc ? exc : &local);
    Il2CppException *e = exc ? *exc : local;
    if (e) {
        static unsigned logged;
        if (logged < 200) {
            logged++;
            char d[900];
            describe_exception(e, d, sizeof d);
            method_name(method, name, sizeof name);
            f2_log("F2: managed exception escaped %s: %s", name, d);
        }
    }
    return r;
}

static void w_add_internal_call(const char *name, Il2CppMethodPointer fn)
{
    __atomic_add_fetch(&s_icalls_added, 1, __ATOMIC_RELAXED);
    r_add_internal_call(name, fn);
    f2_engine_icall(name, (void (*)(void))fn);   /* the modules that keep or override engine icalls (f2_gates.c) */
}

static void w_unhandled_exception(Il2CppException *ex)
{
    char d[900];
    describe_exception(ex, d, sizeof d);
    f2_log("F2: il2cpp_unhandled_exception: %s", d);
    r_unhandled_exception(ex);
}

extern "C" void *f2_diag_il2cpp_symbol(const char *name, void *real)
{
    if (!real) return real;
#define W(n, r, w) if (!strcmp(name, n)) { r = (__typeof__(r))real; return (void *)w; }
    W("il2cpp_init", r_init, w_init)
    W("il2cpp_set_data_dir", r_set_data_dir, w_set_data_dir)
    W("il2cpp_runtime_invoke", r_runtime_invoke, w_runtime_invoke)
    W("il2cpp_runtime_invoke_convert_args", r_runtime_invoke_convert_args, w_runtime_invoke_convert_args)
    W("il2cpp_method_get_param_count", r_method_get_param_count, w_method_get_param_count)
    W("il2cpp_add_internal_call", r_add_internal_call, w_add_internal_call)
    W("il2cpp_unhandled_exception", r_unhandled_exception, w_unhandled_exception)
#undef W
    return real;
}

/* ------------------------------------------------------------------ game-side runtime entries */
static void where(void *ret, char *out, size_t n)
{
    if (!f2_describe_addr((uintptr_t)ret, out, n)) snprintf(out, n, "%p", ret);
}

static NORETURN void g_raise(Il2CppException *ex, Il2CppSequencePoint *sp, MethodInfo *last)
{
    unsigned long n = __atomic_add_fetch(&s_throws, 1, __ATOMIC_RELAXED);
    if ((long)n <= f2_cfg.throw_log || n % 500 == 0) {
        char d[900], w[160];
        describe_exception(ex, d, sizeof d);
        where(__builtin_return_address(0), w, sizeof w);
        f2_log("F2: throw #%lu %s  (at %s)", n, d, w);
    }
    il2cpp::vm::Exception::Raise(ex, sp, last);
}

static NORETURN void g_raise_nre(Il2CppSequencePoint *sp)
{
    unsigned long n = __atomic_add_fetch(&s_nre, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s_throws, 1, __ATOMIC_RELAXED);
    if ((long)n <= f2_cfg.throw_log || n % 500 == 0) {
        char w[160];
        where(__builtin_return_address(0), w, sizeof w);
        f2_log("F2: NullReferenceException #%lu raised by game code at %s", n, w);
    }
    il2cpp::vm::Exception::RaiseNullReferenceException(sp);
}

static Il2CppMethodPointer g_icall_resolve(const char *name)
{
    Il2CppMethodPointer p = il2cpp::vm::InternalCalls::Resolve(name);
    if (p && f2_cfg.icall_log) {   /* each generated wrapper resolves its icall once: this is the list of engine functions the game has used so far */
        static unsigned n;
        if (n < 6000) { n++; f2_log("F2: icall resolved #%u %s", n, name ? name : "(null)"); }
    }
    if (!p) {
        static unsigned logged;
        if (logged < 200) {
            logged++;
            char w[160];
            where(__builtin_return_address(0), w, sizeof w);
            f2_log("F2: [RED] UNRESOLVED internal call %s (asked at %s): the caller will raise MissingMethodException", name ? name : "(null)", w);
        }
    }
    return p;
}

static Il2CppMethodPointer g_pinvoke_resolve(const PInvokeArguments &a)
{
    static unsigned logged;
    if (logged < 100) {
        logged++;
        char w[160];
        where(__builtin_return_address(0), w, sizeof w);
        std::string mod((const char *)a.moduleName.Str(), a.moduleName.Length());
        std::string ep(a.entryPoint.Str(), a.entryPoint.Length());
        f2_log("F2: P/Invoke by name %s!%s (at %s): DllNotFoundException expected unless the library exists", mod.c_str(), ep.c_str(), w);
    }
    return il2cpp::vm::PlatformInvoke::Resolve(a);
}

extern "C" void *f2_diag_substitute(void *target)
{
    typedef NORETURN void (*raise_t)(Il2CppException *, Il2CppSequencePoint *, MethodInfo *);
    typedef NORETURN void (*nre_t)(Il2CppSequencePoint *);
    if (target == (void *)(raise_t)&il2cpp::vm::Exception::Raise) return (void *)g_raise;
    if (target == (void *)(nre_t)&il2cpp::vm::Exception::RaiseNullReferenceException) return (void *)g_raise_nre;
    if (target == (void *)&il2cpp::vm::InternalCalls::Resolve) return (void *)g_icall_resolve;
    if (target == (void *)&il2cpp::vm::PlatformInvoke::Resolve) return (void *)g_pinvoke_resolve;
    return target;
}

/* ------------------------------------------------------------------ for the crash handler / heartbeat */
extern "C" void f2_diag_counters(unsigned long *invokes, unsigned long *throws, const char **last_invoke)
{
    static char name[300];
    *invokes = s_invokes; *throws = s_throws;
    const MethodInfo *m = s_last_invoke;
    if (m) { method_name(m, name, sizeof name); *last_invoke = name; } else *last_invoke = NULL;
}

extern "C" void f2_diag_dump_invokes(void (*out)(const char *line))
{
    char line[400], name[300];
    unsigned pos = s_ring_pos;
    snprintf(line, sizeof line, "F2: last runtime_invoke'd methods (newest first), %lu invokes, %lu throws in total:", s_invokes, s_throws);
    out(line);
    for (unsigned i = 0; i < RING && i < pos && i < 24; i++) {
        const MethodInfo *m = s_ring[(pos - 1 - i) % RING];
        if (!m) break;
        method_name(m, name, sizeof name);
        snprintf(line, sizeof line, "F2:   #%u %s", i, name);
        out(line);
    }
}

static void frame_cb(const Il2CppStackFrameInfo *info, void *user)
{
    void (*out)(const char *) = (void (*)(const char *))user;
    char line[400], name[300];
    method_name(info->method, name, sizeof name);
    snprintf(line, sizeof line, "F2:   managed %s", name);
    out(line);
}

extern "C" void f2_diag_dump_managed_stack(void (*out)(const char *line))
{
    if (!f2_game_base()) return;
    out("F2: managed stack of the crashing thread (stack-trace sentries, innermost last):");
    il2cpp_current_thread_walk_frame_stack(frame_cb, (void *)out);
}
