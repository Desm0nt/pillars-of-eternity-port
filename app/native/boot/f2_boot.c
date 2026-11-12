/* F2 boot: first code of ours that runs in the app process (called by our libmain.so from NativeLoader.load,
 * i.e. before libunity.so is loaded).  Opens the log, resolves folders, installs the crash handler; after
 * libunity is mapped, patches / hooks it (f2_hooks.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <jni.h>
#include "f2_boot.h"

int f2_alloc_overflowed(void);
f2_config_t f2_cfg = {
    .hooks = 1, .patch_platform = 1, .logtee = 1, .vkwrap = 1,
    .invoke_log = 60, .throw_log = 300, .open_log = 400, .heartbeat = 5, .crash = 1, .ctl = 1,
    .pump = 1, .pump_ms = 0, .wide = 1, .fps = 60, .msaa = 0, .touch = 1, .any_gpu = 1, .alloc_fix = 1,
};
unsigned long f2_cnt_opens, f2_cnt_open_fail, f2_cnt_frames, f2_cnt_shaders, f2_cnt_shader_fail,
    f2_cnt_pipelines, f2_cnt_pipeline_fail, f2_cnt_unity_lines, f2_cnt_unity_errors;

static unsigned char s_ms_done[64];
static struct timespec s_t0;

void f2_once_milestone(int id, const char *fmt, ...)
{
    if (id >= 0 && id < (int)sizeof(s_ms_done)) {
        if (__atomic_exchange_n(&s_ms_done[id], 1, __ATOMIC_RELAXED)) return;
    }
    char buf[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    double dt = (t.tv_sec - s_t0.tv_sec) + (t.tv_nsec - s_t0.tv_nsec) / 1e9;
    f2_log("F2: [M%02d] +%.2fs %s", id, dt, buf);
}

const char *f2_files_dir(void) { return f2_cfg.files_dir[0] ? f2_cfg.files_dir : NULL; }
const char *f2_save_dir(void) { return f2_cfg.save_dir[0] ? f2_cfg.save_dir : NULL; }
const char *f2_data_dir(void) { return f2_cfg.data_dir[0] ? f2_cfg.data_dir : NULL; }
const char *f2_overlay_dir(void) { return f2_cfg.overlay_dir[0] ? f2_cfg.overlay_dir : NULL; }
const char *f2_game_image_path(void) { return f2_cfg.game_image[0] ? f2_cfg.game_image : NULL; }

/* ---- JNI helpers ---- */
static int jcheck(JNIEnv *e)
{
    if ((*e)->ExceptionCheck(e)) { (*e)->ExceptionClear(e); return 1; }
    return 0;
}

static int jstr(JNIEnv *e, jstring s, char *out, size_t n)
{
    out[0] = 0;
    if (!s) return 0;
    const char *c = (*e)->GetStringUTFChars(e, s, NULL);
    if (!c) { jcheck(e); return 0; }
    snprintf(out, n, "%s", c);
    (*e)->ReleaseStringUTFChars(e, s, c);
    return out[0] != 0;
}

/* File f = ctx.<method>(arg); f.getAbsolutePath() */
static int jdir(JNIEnv *e, jobject ctx, const char *method, const char *sig, int has_arg, char *out, size_t n)
{
    out[0] = 0;
    jclass c = (*e)->GetObjectClass(e, ctx);
    jmethodID m = c ? (*e)->GetMethodID(e, c, method, sig) : NULL;
    if (jcheck(e) || !m) return 0;
    jobject f = has_arg ? (*e)->CallObjectMethod(e, ctx, m, (jobject)NULL) : (*e)->CallObjectMethod(e, ctx, m);
    if (jcheck(e) || !f) return 0;
    jclass cf = (*e)->GetObjectClass(e, f);
    jmethodID mp = cf ? (*e)->GetMethodID(e, cf, "getAbsolutePath", "()Ljava/lang/String;") : NULL;
    if (jcheck(e) || !mp) return 0;
    jstring s = (jstring)(*e)->CallObjectMethod(e, f, mp);
    if (jcheck(e)) return 0;
    return jstr(e, s, out, n);
}

static void java_paths(JNIEnv *e, jobject act)
{
    if (!e || !act) return;
    jdir(e, act, "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;", 1, f2_cfg.files_dir, sizeof f2_cfg.files_dir);
    jdir(e, act, "getFilesDir", "()Ljava/io/File;", 0, f2_cfg.internal_dir, sizeof f2_cfg.internal_dir);
    jclass c = (*e)->GetObjectClass(e, act);
    jmethodID m = c ? (*e)->GetMethodID(e, c, "getPackageCodePath", "()Ljava/lang/String;") : NULL;
    if (!jcheck(e) && m) {
        jstring s = (jstring)(*e)->CallObjectMethod(e, act, m);
        if (!jcheck(e)) jstr(e, s, f2_cfg.apk_path, sizeof f2_cfg.apk_path);
    }
}

static void prop(const char *name, char *out)
{
    out[0] = 0;
    __system_property_get(name, out);
}

static void mkdirs(const char *path)
{
    char b[600];
    snprintf(b, sizeof b, "%s", path);
    for (char *p = b + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(b, 0771); *p = '/'; }
    mkdir(b, 0771);
}

/* f2_config.txt = ours (debugging over adb); f2_front.txt = the player's choices, written by the Java front end at each start
 * (DmActivity.writeFront), read second so that it wins for the keys it has */
static void read_config_file(const char *file)
{
    char p[700], line[700];
    snprintf(p, sizeof p, "%s/%s", f2_cfg.files_dir, file);
    FILE *f = fopen(p, "r");
    if (!f) { f2_log("F2: no %s (defaults)", p); return; }
    while (fgets(line, sizeof line, f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        if (line[0] == '#' || !line[0]) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "data_dir")) snprintf(f2_cfg.data_dir, sizeof f2_cfg.data_dir, "%s", v);
        else if (!strcmp(k, "overlay_dir")) snprintf(f2_cfg.overlay_dir, sizeof f2_cfg.overlay_dir, "%s", v);
        else if (!strcmp(k, "save_dir")) snprintf(f2_cfg.save_dir, sizeof f2_cfg.save_dir, "%s", v);
        else if (!strcmp(k, "hooks")) f2_cfg.hooks = atoi(v);
        else if (!strcmp(k, "patch_platform")) f2_cfg.patch_platform = atoi(v);
        else if (!strcmp(k, "logtee")) f2_cfg.logtee = atoi(v);
        else if (!strcmp(k, "vkwrap")) f2_cfg.vkwrap = atoi(v);
        else if (!strcmp(k, "invoke_log")) f2_cfg.invoke_log = atoi(v);
        else if (!strcmp(k, "throw_log")) f2_cfg.throw_log = atoi(v);
        else if (!strcmp(k, "open_log")) f2_cfg.open_log = atoi(v);
        else if (!strcmp(k, "heartbeat")) f2_cfg.heartbeat = atoi(v);
        else if (!strcmp(k, "crash")) f2_cfg.crash = atoi(v);
        else if (!strcmp(k, "ctl")) f2_cfg.ctl = atoi(v);
        else if (!strcmp(k, "pump")) f2_cfg.pump = atoi(v);
        else if (!strcmp(k, "pump_ms")) f2_cfg.pump_ms = atoi(v);
        else if (!strcmp(k, "wide")) f2_cfg.wide = atoi(v);
        else if (!strcmp(k, "unity_trace")) f2_cfg.unity_trace = atoi(v);
        else if (!strcmp(k, "alloc_fix")) f2_cfg.alloc_fix = atoi(v);
        else if (!strcmp(k, "any_gpu")) f2_cfg.any_gpu = atoi(v);
        else if (!strcmp(k, "touch")) f2_cfg.touch = atoi(v);
        else if (!strcmp(k, "touch_log")) f2_cfg.touch_log = atoi(v);
        else if (!strcmp(k, "msaa")) f2_cfg.msaa = atoi(v);
        else if (!strcmp(k, "icall_log")) f2_cfg.icall_log = atoi(v);
        else if (!strcmp(k, "fps")) f2_cfg.fps = atoi(v);
        else if (!strcmp(k, "tex_set")) snprintf(f2_cfg.tex_set, sizeof f2_cfg.tex_set, "%s", v);
        else if (!strcmp(k, "bc_hide")) f2_cfg.bc_hide = atoi(v);
        else if (!strcmp(k, "astc_dir")) snprintf(f2_cfg.astc_dir, sizeof f2_cfg.astc_dir, "%s", v);
        else if (!strcmp(k, "game_image")) snprintf(f2_cfg.game_image, sizeof f2_cfg.game_image, "%s", v);
        else if (!strcmp(k, "defres_path")) snprintf(f2_cfg.defres_path, sizeof f2_cfg.defres_path, "%s", v);
        else { f2_log("F2: %s: unknown key '%s'", file, k); continue; }
        f2_log("F2: %s: %s=%s", file, k, v);
    }
    fclose(f);
}
static void read_config(void) { read_config_file("f2_config.txt"); read_config_file("f2_front.txt"); }

static void *heartbeat(void *arg)
{
    (void)arg;
    unsigned long last[6] = {0};
    int idle = 0;
    for (;;) {
        sleep(f2_cfg.heartbeat);
        if (f2_cfg.crash) f2_crash_install();          /* no-op while ours is on top */
        unsigned long inv = 0, thr = 0;
        const char *li = NULL;
        f2_diag_counters(&inv, &thr, &li);
        unsigned long cur[6] = {inv, thr, f2_cnt_opens, f2_cnt_frames, f2_cnt_shaders, f2_cnt_unity_lines};
        int same = !memcmp(cur, last, sizeof cur);
        memcpy(last, cur, sizeof cur);
        idle = same ? idle + 1 : 0;
        /* quiet when nothing moves, except a line every 6th beat so that a hang is visible in the log */
        if (same && idle % 6 != 1) continue;
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        f2_log("F2: hb +%lds invokes=%lu throws=%lu opens=%lu(fail %lu, astc %lu) frames=%lu shaders=%lu(fail %lu) pipelines=%lu(fail %lu) "
               "unity_lines=%lu(err %lu) pump=%lu%s last_invoke=%s",
               (long)(t.tv_sec - s_t0.tv_sec), inv, thr, f2_cnt_opens, f2_cnt_open_fail, f2_cnt_astc_opens, f2_cnt_frames, f2_cnt_shaders,
               f2_cnt_shader_fail, f2_cnt_pipelines, f2_cnt_pipeline_fail, f2_cnt_unity_lines, f2_cnt_unity_errors, f2_cnt_pump,
               same ? " [NO PROGRESS]" : "", li ? li : "-");
        static int said_overflow;
        if (!said_overflow && f2_alloc_overflowed()) { said_overflow = 1; f2_log("F2: the player's allocator ran out of its five 4 GB windows: later allocations are unrecorded (handled by the alloc_fix patch)"); }
    }
    return NULL;
}

int f2_boot(void *java_vm, void *jni_env, void *activity, const char *lib_dir)
{
    static int done;
    if (__atomic_exchange_n(&done, 1, __ATOMIC_RELAXED)) return 0;
    (void)java_vm;
    clock_gettime(CLOCK_MONOTONIC, &s_t0);
    snprintf(f2_cfg.lib_dir, sizeof f2_cfg.lib_dir, "%s", lib_dir ? lib_dir : "");
    java_paths((JNIEnv *)jni_env, (jobject)activity);
    if (!f2_cfg.files_dir[0]) {
        /* no Java answer: derive the package from the process name (never a built-in id) */
        char pkg[200] = "";
        int fd = open("/proc/self/cmdline", O_RDONLY);
        if (fd >= 0) { ssize_t r = read(fd, pkg, sizeof pkg - 1); if (r > 0) pkg[r] = 0; close(fd); }
        if (!pkg[0]) snprintf(pkg, sizeof pkg, "%s", getprogname());
        char *colon = strchr(pkg, ':');
        if (colon) *colon = 0;
        snprintf(f2_cfg.files_dir, sizeof f2_cfg.files_dir, "/storage/emulated/0/Android/data/%s/files", pkg);
    }
    const char *env = getenv("F2_FILES_DIR");            /* PC dry run */
    if (env) snprintf(f2_cfg.files_dir, sizeof f2_cfg.files_dir, "%s", env);
    mkdirs(f2_cfg.files_dir);

    char p[700], q[700];
    snprintf(p, sizeof p, "%s/f2_log.txt", f2_cfg.files_dir);
    snprintf(q, sizeof q, "%s/f2_log.prev.txt", f2_cfg.files_dir);
    rename(p, q);
    f2_log_open(p);

    char model[PROP_VALUE_MAX], rel[PROP_VALUE_MAX], sdk[PROP_VALUE_MAX], hw[PROP_VALUE_MAX], abi[PROP_VALUE_MAX];
    prop("ro.product.model", model); prop("ro.build.version.release", rel); prop("ro.build.version.sdk", sdk);
    prop("ro.board.platform", hw); prop("ro.product.cpu.abi", abi);
    f2_cfg.sdk = atoi(sdk);
    time_t now = time(NULL);
    char when[64] = "";
    struct tm tmv;
    if (localtime_r(&now, &tmv)) strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tmv);
    f2_once_milestone(M_BOOT, "f2_boot: build %s %s, log %s (%s) | device %s, Android %s (sdk %s), %s, %s, page %ld, pid %d",
                      __DATE__, __TIME__, f2_log_fd() >= 0 ? p : "(log file NOT open)", when, model, rel, sdk, hw, abi,
                      sysconf(_SC_PAGESIZE), (int)getpid());
    f2_log("F2: lib dir %s | apk %s | files %s | internal %s", f2_cfg.lib_dir, f2_cfg.apk_path, f2_cfg.files_dir, f2_cfg.internal_dir);

    read_config();
    if (!f2_cfg.save_dir[0]) snprintf(f2_cfg.save_dir, sizeof f2_cfg.save_dir, "%s/save", f2_cfg.files_dir);
    mkdirs(f2_cfg.save_dir);
    f2_paths_init();
    if (f2_cfg.crash) f2_crash_install();
    if (f2_cfg.heartbeat > 0) {
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&th, &at, heartbeat, NULL) == 0) pthread_setname_np(th, "f2-heartbeat");
        pthread_attr_destroy(&at);
    }
    if (f2_cfg.ctl) f2_ctl_start();
    return 0;
}

void f2_unity_loaded(void *handle)
{
    f2_hooks_install(handle);
    f2_crash_refresh_modules();
}

void f2_unity_onload_done(int version)
{
    f2_once_milestone(M_UNITY_ONLOAD, "libunity JNI_OnLoad returned 0x%x", version);
    f2_crash_refresh_modules();
    if (f2_cfg.crash) f2_crash_install();   /* again: on top of anything libunity installed */
}
