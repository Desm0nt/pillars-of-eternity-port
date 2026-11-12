/* F2 boot / hook / diagnostics layer -- internal interface (native/boot). */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "../f2_app.h"
#ifdef __cplusplus
extern "C" {
#endif

#define F2_EXPORT __attribute__((visibility("default")))

/* ---- milestones (HANDOVER.txt lists them in this order) ---- */
enum {
    M_LIBMAIN = 1,        /* our libmain.so: NativeLoader.load entered */
    M_BOOT = 2,           /* libil2cpp.so loaded, f2_boot ran: log file open, paths resolved */
    M_DATA = 3,           /* game Data folder found and readable / overlay indexed */
    M_UNITY_LOADED = 4,   /* libunity.so dlopen'ed, variant identified */
    M_PATCHED = 5,        /* platform-38 patch applied */
    M_HOOKED = 6,         /* import hooks installed */
    M_UNITY_ONLOAD = 7,   /* libunity JNI_OnLoad returned */
    M_IL2CPP_LOOKUP = 8,  /* LoadIl2Cpp: dlsym pass over our library */
    M_IMAGE = 9,          /* "game image mapped" */
    M_IL2CPP_INIT = 10,   /* il2cpp_init returned */
    M_FIRST_DATA = 11,    /* engine opened its first redirected data file */
    M_FIRST_INVOKE = 12,  /* first il2cpp_runtime_invoke from libunity */
    M_ENGINE_NOGFX = 13,  /* "PlayerInitEngineNoGraphics OK" */
    M_VK_INSTANCE = 14,   /* vkCreateInstance ok */
    M_VK_DEVICE = 15,     /* vkCreateDevice ok (device name, API version) */
    M_GLES = 16,          /* eglCreateContext called: the engine fell back to GLES (our shaders are Vulkan-only) */
    M_ENGINE_GFX = 17,    /* "PlayerInitEngineGraphics OK" */
    M_SWAPCHAIN = 18,     /* vkCreateSwapchainKHR ok */
    M_FIRST_SHADER = 19,  /* first vkCreateShaderModule ok */
    M_FIRST_PIPELINE = 20,/* first vkCreateGraphicsPipelines ok */
    M_FIRST_SCENE = 21,   /* first scene load (engine opened level0 / first SceneManager callback) */
    M_FIRST_FRAME = 22,   /* first vkQueuePresentKHR */
    M_GAME_BOOT = 23,     /* the game's own boot entry ran (ConsoleBootstrap.Bootstrap -> nn.account / nn.fs calls) */
    M_FRAMES_100 = 24,    /* 100 frames presented */
    M_PUMP = 25,          /* async upload pump called for the first time (f2_hooks.c) */
};

/* ---- configuration (<files dir>/f2_config.txt, key=value per line; all optional) ---- */
typedef struct {
    char files_dir[512];      /* external files dir */
    char internal_dir[512];   /* internal files dir (/data/user/0/<pkg>/files) */
    char apk_path[512];
    char lib_dir[512];
    char data_dir[512];       /* the 15 GB Data folder (read only) */
    char data_real[512];      /* realpath(data_dir) */
    char overlay_dir[512];    /* overlay Data folder */
    char save_dir[512];
    char apk_data_prefix[600];/* "<apk>/assets/bin/Data/" as the engine spells it */
    int data_ok;
    int sdk;
    /* switches */
    int hooks;                /* 1: import hooks on libunity (0 = nothing is redirected: for bisecting only) */
    int patch_platform;       /* 1: platform-38 patch */
    int logtee;               /* 1: copy libunity's logcat lines into f2_log.txt */
    int vkwrap;               /* 1: Vulkan diagnostics wrappers */
    int invoke_log;           /* number of il2cpp_runtime_invoke calls logged by name */
    int throw_log;            /* number of managed throws logged */
    int open_log;             /* number of redirected opens logged */
    int heartbeat;            /* seconds between heartbeat lines (0 = off) */
    int crash;                /* 1: crash handler */
    int ctl;                  /* 1: control file f2_ctl.txt (pad buttons over adb) */
    int pump;                 /* 1: async upload pump (f2_hooks.c); without it scenes with stream textures never finish loading */
    int wide;                 /* 1: a 16:9 Screen.SetResolution request is widened to the window's aspect (f2_screen.c) */
    int fps;                  /* 0: the game's own; 60 | 30: forced */
    int icall_log;            /* 1: a line for every internal call the game resolves (f2_diag.cpp) */
    int msaa;                 /* -1: the game's own antiAliasing; 0/2/4/8 forced (f2_screen.c) */
    int touch;                /* 1: touch as the PC mouse: the game-side gates (f2_touch.c) */
    int touch_log;            /* 1: diagnostics gates in NGUI (f2_touch.c) */
    int any_gpu;              /* 1: the engine's Vulkan vendor list is opened to any vendor (f2_hooks.c) */
    int alloc_fix;            /* 1: the player's allocator page table survives a heap wider than five 4 GB windows (f2_hooks.c) */
    int unity_trace;          /* 1: call counters at the player's JNI natives and start-up functions (f2_hooks.c) */
    int tsb_fix;              /* 1: the player's ThreadedStreamBuffer hand-shake loads made load-acquire (lost wake-up = start-up hang; f2_hooks.c) */
    int stall_dump;           /* number of stall reports (thread list + stacks) per run when nothing moves for 10 s (f2_crash.c; 0 = off) */
    int pump_ms;              /* time slice of the pump per call, ms (0 = QualitySettings.asyncUploadTimeSlice) */
    char tex_set[16];         /* "" / auto: ASTC set when the GPU has no BC and overlay-astc has files; astc | bc: forced (f2_texset.c) */
    int bc_hide;              /* 1: the player is shown a GPU without BC (f2_vkwrap.c) -- Adreno test of the no-BC path; auto then picks ASTC */
    char astc_dir[512];       /* the ASTC set's Data folder (default <files>/overlay-astc/Data) */
    char game_image[512];     /* the game image file (libf2game.so), when it is not shipped inside the APK (clean APK: setup_f2.py's
                               <data root>/game/libf2game.so, written here by GameFolder.findDataRoot via f2_front.txt) */
    char defres_path[512];    /* DmActivity's one-time extraction of assets/f2/unity_default_resources (clean APK: overlay/data no
                               longer need to carry "unity default resources" themselves; f2_paths.c serves it from here as a last resort) */
} f2_config_t;
extern f2_config_t f2_cfg;

/* f2_log.c */
void f2_log_open(const char *path);
void f2_log_raw(const char *line);          /* async-signal-safe */
void f2_logv(const char *fmt, va_list ap);
int f2_log_fd(void);
void f2_log_file_only(const char *prefix, const char *text);

/* f2_boot.c : called by libmain.so */
F2_EXPORT int f2_boot(void *java_vm, void *jni_env, void *activity, const char *lib_dir);
F2_EXPORT void f2_unity_loaded(void *handle);
F2_EXPORT void f2_unity_onload_done(int version);
void f2_once_milestone(int id, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* f2_paths.c */
void f2_paths_init(void);
/* returns path itself or buf with the redirected path; *kind: 0 none, 1 overlay, 2 data folder, 3 ASTC set (overlay-astc), 4 the
 * APK's own extracted "unity default resources" (defres_path=, clean-APK setup folders that do not carry it in their overlay) */
const char *f2_redirect(const char *path, char *buf, size_t n, int *kind);

/* f2_texset.c */
void f2_texset_init(void);                                 /* f2_paths_init: probe + decision + the one log line */
int f2_texset_astc(void);                                  /* 1: the ASTC set is in use */
int f2_texset_has(const char *rel);
const char *f2_texset_dir(void);
const char *f2_texset_redirect(const char *rel, int overlay_has_it, char *buf, size_t n);   /* NULL = not from the ASTC set */
extern unsigned long f2_cnt_astc_opens;

/* f2_hooks.c */
void f2_hooks_install(void *unity_handle);
uintptr_t f2_unity_base(void);
size_t f2_unity_size(void);
const char *f2_unity_variant(void);

/* f2_vkwrap.c */
void *f2_vk_wrap_symbol(const char *name, void *real);   /* returns the wrapper or real */
void *f2_egl_hook(const char *name);                     /* import hook for an EGL name or NULL */
void f2_egl_set_real(const char *name, void *real);

/* f2_diag.cpp */
void *f2_diag_il2cpp_symbol(const char *name, void *real);      /* dlsym hook: wrappers for a few il2cpp_* API functions */
void *f2_diag_substitute(void *target);                         /* loader: wrapper for a runtime entry the game code calls */
void f2_diag_dump_invokes(void (*out)(const char *line));       /* crash handler: last runtime_invoke'd methods */
void f2_diag_dump_managed_stack(void (*out)(const char *line)); /* crash handler: sentry stack of the current thread */
void f2_diag_counters(unsigned long *invokes, unsigned long *throws, const char **last_invoke);

/* f2_ctl.c */
void f2_ctl_start(void);

/* f2_crash.c */
void f2_crash_install(void);
void f2_crash_refresh_modules(void);
int f2_describe_addr(uintptr_t addr, char *out, size_t n);     /* "libunity.so+0x1234" / "main+0x..." */
void f2_crash_dump_threads(const char *why);                   /* stall report: every thread's state, pc and stack frames */

/* counters shown by the heartbeat */
extern unsigned long f2_cnt_opens, f2_cnt_open_fail, f2_cnt_frames, f2_cnt_shaders, f2_cnt_shader_fail,
    f2_cnt_pipelines, f2_cnt_pipeline_fail, f2_cnt_unity_lines, f2_cnt_unity_errors, f2_cnt_pump;

#ifdef __cplusplus
}
#endif
