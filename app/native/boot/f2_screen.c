/* F2 picture settings (the port's "picture shape" and "frame rate" switches, done at the engine's managed boundary):
 *   wide=1 (default)  the game asks UnityEngine.Screen.SetResolution for a 16:9 size (ResolutionController: 1600x900 in "boost",
 *                     1280x720 otherwise); the Android player scales that buffer to the whole window, which stretches it on a
 *                     20:9 phone (phone run 06).  The request is widened to the window's aspect at the same height
 *                     (1600x900 -> 2000x900 on 2400x1080); the game lays its UI out for the wider picture by itself.
 *                     With wide=0 the Java side makes the window 16:9 with bars instead (DmActivity), nothing is changed here.
 *   fps=0 (default)   the game's own vSyncCount / targetFrameRate;  fps=60 | 30: forced (vSyncCount 1 | 2 and targetFrameRate).
 * How: libunity registers its icalls under short names ("Type::Method"); the generated code resolves the full name first, so a
 * full-name registration of ours wins and calls the engine's function with the changed arguments (as f2_switch_icalls.c). */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "f2_boot.h"

void il2cpp_add_internal_call(const char *name, void (*method)(void));

static void (*s_set_resolution)(int32_t, int32_t, int32_t, int32_t);
static void (*s_set_vsync)(int32_t);
static void (*s_set_target)(int32_t);
static void (*s_set_aa)(int32_t);
static uint32_t s_surf_w, s_surf_h;

/* the window's size = the first swapchain the engine creates (before the game asks for its own resolution) */
void f2_screen_note_swapchain(uint32_t w, uint32_t h)
{
    if (!s_surf_w && w && h) { s_surf_w = w; s_surf_h = h; f2_log("F2: screen: window %ux%u", w, h); }
}

/* fps=30 | 60 | 90 | 120 (the gear's choice).
 * HOW THE 2018.1 ANDROID PLAYER PACES A FRAME (libunity Release: InitializationPlayerUpdateTimeRegistrator::Forward +0x7c8ee4,
 * TimeManager::Sync +0x77179c, GetActualTargetFrameRate(int) +0x7c9718; read with tools/udis.py):
 *   vs = QualitySettings.vSyncCount;  rate = vs > 0 ? screen refresh / vs : Application.targetFrameRate (30 when it is -1)
 *   vs > 0 and vs <= caps.maxVSyncInterval: NO wait at all, the swapchain's FIFO paces = one frame per display refresh whatever vs is
 *   else Sync(rate): rate <= 60 and 60 / rate whole (or vs > 0) -> wait until the Choreographer's vsync counter advanced by round(60 / rate)
 *                    otherwise                                  -> sleep until 1 / rate after the last frame
 * The "60" there is a constant: the player takes every display for a 60 Hz one.  On a 120 Hz panel "wait 1 vsync" is 120 fps and "wait 2" is 60
 * (owner, 12.11.2026, Xiaomi 13 Pro: 60 in the gear ran at 90 = as fast as the phone could, 30 ran at 60).
 * So the cap is made with vSyncCount 0 and a targetFrameRate chosen for the number of vsyncs it turns into:
 *   display / fps whole (= W vsyncs per frame): targetFrameRate = 60 / W          (120 Hz: 60 -> 30, 30 -> 15, 120 -> 60; 60 Hz: 60 -> 60, 30 -> 30)
 *   otherwise (90 on 120 Hz, 60 on 90 Hz):      the sleep path with the real rate (a rate the vsync path would take, e.g. 60 on 90 Hz, gets +1)
 * The display's rate comes from Java (DmActivity -> nativeDisplayHz).  A check on top (f2_screen_note_fps, every 5 s from the present counter):
 * frames coming clearly faster than the cap mean the counter ticks faster than the display claims -> the tick rate is taken from the measurement. */
static int fps_forced(void) { return f2_cfg.fps == 30 || f2_cfg.fps == 60 || f2_cfg.fps == 90 || f2_cfg.fps == 120; }
static float s_display_hz;      /* what Java says */
static float s_tick_hz;         /* what the frame counter proved (0 = nothing yet) */
static int s_wait_vsyncs;       /* W of the cap in use, 0 = the sleep path */
static int target_for(int fps, int *vsyncs)
{
    float hz = s_tick_hz > 20 ? s_tick_hz : s_display_hz > 20 ? s_display_hz : 60;
    float ratio = hz / (float)fps;
    int whole = (int)(ratio + 0.5f);
    if (whole >= 1 && whole <= 6 && ratio - (float)whole < 0.08f && (float)whole - ratio < 0.08f && 60 % whole == 0) { *vsyncs = whole; return 60 / whole; }
    *vsyncs = 0;
    return (fps <= 60 && 60 % fps == 0) ? fps + 1 : fps;
}
static void apply_fps(void)
{
    if (!fps_forced()) return;
    int w = 0, target = target_for(f2_cfg.fps, &w);
    static int last_target = -1, last_fps; static float last_hz, last_tick;
    if (target == last_target && last_fps == f2_cfg.fps && last_hz == s_display_hz && last_tick == s_tick_hz) return;
    last_target = target; last_fps = f2_cfg.fps; last_hz = s_display_hz; last_tick = s_tick_hz; s_wait_vsyncs = w;
    if (s_set_vsync) s_set_vsync(0);
    if (s_set_target) s_set_target(target);
    f2_log("F2: screen: frame rate %d on a %.0f Hz display (vsync counter %.0f Hz): vSyncCount 0, targetFrameRate %d = %s", f2_cfg.fps, s_display_hz,
           s_tick_hz > 20 ? s_tick_hz : s_display_hz, target, w ? (w == 1 ? "every vsync" : w == 2 ? "every 2nd vsync" : "every Nth vsync") : "the engine's timer");
}
void f2_screen_set_display_hz(float hz) { if (hz != s_display_hz) { s_display_hz = hz; s_tick_hz = 0; } }
void f2_screen_reapply_fps(void) { apply_fps(); }       /* main thread only (f2_touch.c run_pending_action) */
/* the gear's frame rate, changed while the game runs (owner, 12.11.2026: "why a restart, the PC switches it on the fly"): any thread; the cap
 * itself is made again on the main thread (game action 3).  The display may change its mode for the new rate: Java reports that separately. */
void f2_screen_set_fps(int fps)
{
    if (fps != 30 && fps != 60 && fps != 90 && fps != 120) return;
    if (fps == f2_cfg.fps) return;
    f2_log("F2: screen: frame rate %d -> %d (the gear, live)", f2_cfg.fps, fps);
    f2_cfg.fps = fps;
    s_tick_hz = 0;
}

/* every 5 s from the present counter (f2_vkwrap.c, any thread): the cap is W vsyncs per frame, so fps * W is the vsync counter's real rate when the
 * game keeps up.  Only "faster than asked" can be told from load; two windows in a row are needed.  The correction itself runs on the main thread. */
void f2_touch_game_action(int id);
void f2_screen_note_fps(float fps)
{
    static int over;
    if (!fps_forced() || !s_wait_vsyncs || fps < (float)f2_cfg.fps * 1.2f) { over = 0; return; }
    if (++over < 2) return;
    over = 0;
    static const float known[] = { 60, 90, 120, 144, 165, 240 };
    float real = fps * (float)s_wait_vsyncs, best = 0;
    for (unsigned i = 0; i < sizeof known / sizeof known[0]; i++) if (real > known[i] * 0.85f) best = known[i];
    float cur = s_tick_hz > 20 ? s_tick_hz : s_display_hz;
    if (best <= cur) return;
    f2_log("F2: screen: %.1f fps with a cap of %d: the vsync counter runs at about %.0f Hz, not %.0f -> the cap is made again for %.0f Hz", fps, f2_cfg.fps, real, cur, best);
    s_tick_hz = best;
    f2_touch_game_action(3);
}

static void ov_set_resolution(int32_t w, int32_t h, int32_t mode, int32_t rate)
{
    int32_t nw = w, nh = h;
    if (f2_cfg.wide && s_surf_w && s_surf_h && w > 0 && h > 0 && w * 9 == h * 16) {
        if ((uint64_t)s_surf_w * 9 > (uint64_t)s_surf_h * 16 + s_surf_h) nw = (int32_t)(((uint64_t)h * s_surf_w / s_surf_h + 1) & ~1ull);        /* wider */
        else if ((uint64_t)s_surf_w * 9 + s_surf_h < (uint64_t)s_surf_h * 16) nh = (int32_t)(((uint64_t)w * s_surf_h / s_surf_w + 1) & ~1ull);   /* narrower */
    }
    static int n;
    if (n++ < 8) f2_log("F2: screen: Screen.SetResolution(%d, %d, mode %d, %d Hz) -> %d x %d (window %ux%u, wide=%d)", w, h, mode, rate, nw, nh, s_surf_w, s_surf_h, f2_cfg.wide);
    s_set_resolution(nw, nh, mode, fps_forced() && f2_cfg.fps > 60 ? f2_cfg.fps : rate);
    apply_fps();
}
static void ov_set_vsync(int32_t v)
{
    int32_t nv = fps_forced() ? 0 : v;
    static int n;
    if (n++ < 4) f2_log("F2: screen: QualitySettings.vSyncCount = %d -> %d (fps=%d)", v, nv, f2_cfg.fps);
    s_set_vsync(nv);
}
static void ov_set_target(int32_t v)
{
    int unused_w; int32_t nv = fps_forced() ? target_for(f2_cfg.fps, &unused_w) : v;
    static int n;
    if (n++ < 4) f2_log("F2: screen: Application.targetFrameRate = %d -> %d (fps=%d)", v, nv, f2_cfg.fps);
    s_set_target(nv);
}

/* msaa=0 (default): the game asks QualitySettings.antiAliasing = 4, and with an MSAA back buffer the 2018.1 Vulkan backend renders the game's own
 * deferred pipeline (PE_GameRender: MRT G-buffer cameras + OnRenderImage) black: everything but particles and UI (phone runs 08-13; run 14 with
 * msaa=0 is right).  -1: the game's own value; 2 / 4 / 8: forced (for testing) */
static void ov_set_aa(int32_t v)
{
    int32_t nv = f2_cfg.msaa >= 0 ? f2_cfg.msaa : v;
    static int n;
    if (n++ < 4) f2_log("F2: screen: QualitySettings.antiAliasing = %d -> %d (msaa=%d)", v, nv, f2_cfg.msaa);
    s_set_aa(nv);
}

/* diagnostics: which asset bundles and assets the game asks for and what it gets.  An Il2CppString: length at +0x10, UTF-16 at +0x14;
 * a UnityEngine.Object: m_CachedPtr (the native object) at +0x10 */
int f2_describe_addr(uintptr_t addr, char *out, size_t n);
static void str8(void *s, char *out, int cap)
{
    int n = s ? *(int32_t *)((char *)s + 0x10) : 0, i;
    for (i = 0; i < n && i < cap - 1; i++) { uint16_t c = *(uint16_t *)((char *)s + 0x14 + 2 * i); out[i] = c < 0x80 ? (char)c : '?'; }
    out[i] = 0;
}
static void *native_of(void *obj) { return obj ? *(void **)((char *)obj + 0x10) : NULL; }
static void callers(char *out, size_t cap)
{
    size_t w = 0; out[0] = 0;
    void **fp = (void **)__builtin_frame_address(1);
    for (int i = 0; i < 5 && fp && w + 180 < cap; i++) {
        char d[170];
        if (!f2_describe_addr((uintptr_t)fp[1], d, sizeof d)) snprintf(d, sizeof d, "%p", fp[1]);
        w += snprintf(out + w, cap - w, " <- %s", d);
        void **next = (void **)fp[0];
        if (next <= fp || (uintptr_t)next - (uintptr_t)fp > 0x100000) break;
        fp = next;
    }
}
static void *(*s_ab_load)(void *, uint32_t, uint64_t);
static void *(*s_ab_asset)(void *, void *, void *);
static void *(*s_ab_asset_async)(void *, void *, void *);
static void *(*s_abr_asset)(void *);
static void (*s_ab_unload)(void *, uint8_t);
static int s_ab_lines;
static void *s_quiet_bundle;        /* the big shared "objectbundle": thousands of table loads, only failures are logged */
static void *s_req[64]; static unsigned s_nreq;   /* requests made on the other bundles */
static void *ov_ab_load(void *path, uint32_t crc, uint64_t offset)
{
    char name[400], who[900];
    str8(path, name, sizeof name);
    void *r = s_ab_load(path, crc, offset);
    callers(who, sizeof who);
    size_t nl = strlen(name);
    if (r && nl >= 12 && !strcmp(name + nl - 12, "objectbundle")) s_quiet_bundle = r;
    if (s_ab_lines++ < 600) f2_log("F2: AssetBundle.LoadFromFile(\"%s\", crc %u, offset %llu) -> %p (native %p)%s; callers:%s", name, crc, (unsigned long long)offset, r,
                                   native_of(r), r && native_of(r) ? "" : "  [RED: no bundle]", who);
    return r;
}
static void *ov_ab_asset(void *self, void *name, void *type)
{
    char n[300]; str8(name, n, sizeof n);
    void *r = s_ab_asset(self, name, type);
    if (self == s_quiet_bundle && r && native_of(r)) return r;
    if (s_ab_lines++ < 600) f2_log("F2: AssetBundle(%p).LoadAsset(\"%s\") -> %p (native %p)%s", self, n, r, native_of(r), r && native_of(r) ? "" : "  [RED: no asset]");
    return r;
}
static void *ov_ab_asset_async(void *self, void *name, void *type)
{
    char n[300]; str8(name, n, sizeof n);
    void *r = s_ab_asset_async(self, name, type);
    if (self == s_quiet_bundle) return r;
    s_req[s_nreq++ % 64] = r;
    if (s_ab_lines++ < 600) f2_log("F2: AssetBundle(%p).LoadAssetAsync(\"%s\") -> request %p", self, n, r);
    return r;
}
static void *ov_abr_asset(void *self)
{
    void *r = s_abr_asset(self);
    int mine = 0;
    for (int i = 0; i < 64; i++) if (s_req[i] == self) mine = 1;
    if (!mine && r && native_of(r)) return r;
    if (s_ab_lines++ < 600) f2_log("F2: AssetBundleRequest(%p).asset -> %p (native %p)%s", self, r, native_of(r), r && native_of(r) ? "" : "  [RED: no asset]");
    return r;
}
static void ov_ab_unload(void *self, uint8_t all)
{
    if (s_ab_lines++ < 600) f2_log("F2: AssetBundle(%p).Unload(%d)", self, all);
    s_ab_unload(self, all);
}

/* called for every icall libunity registers (f2_diag.cpp) */
void f2_screen_engine_icall(const char *name, void (*fn)(void))
{
    if (!name || !fn) return;
    if (!strcmp(name, "UnityEngine.Screen::SetResolution")) {
        s_set_resolution = (void (*)(int32_t, int32_t, int32_t, int32_t))fn;
        il2cpp_add_internal_call("UnityEngine.Screen::SetResolution(System.Int32,System.Int32,UnityEngine.FullScreenMode,System.Int32)", (void (*)(void))ov_set_resolution);
    } else if (!strcmp(name, "UnityEngine.AssetBundle::LoadFromFile")) {
        s_ab_load = (void *(*)(void *, uint32_t, uint64_t))fn;
        il2cpp_add_internal_call("UnityEngine.AssetBundle::LoadFromFile(System.String,System.UInt32,System.UInt64)", (void (*)(void))ov_ab_load);
    } else if (!strcmp(name, "UnityEngine.AssetBundle::LoadAsset_Internal")) {
        s_ab_asset = (void *(*)(void *, void *, void *))fn;
        il2cpp_add_internal_call("UnityEngine.AssetBundle::LoadAsset_Internal(System.String,System.Type)", (void (*)(void))ov_ab_asset);
    } else if (!strcmp(name, "UnityEngine.AssetBundle::LoadAssetAsync_Internal")) {
        s_ab_asset_async = (void *(*)(void *, void *, void *))fn;
        il2cpp_add_internal_call("UnityEngine.AssetBundle::LoadAssetAsync_Internal(System.String,System.Type)", (void (*)(void))ov_ab_asset_async);
    } else if (!strcmp(name, "UnityEngine.AssetBundleRequest::get_asset")) {
        s_abr_asset = (void *(*)(void *))fn;
        il2cpp_add_internal_call("UnityEngine.AssetBundleRequest::get_asset()", (void (*)(void))ov_abr_asset);
    } else if (!strcmp(name, "UnityEngine.AssetBundle::Unload")) {
        s_ab_unload = (void (*)(void *, uint8_t))fn;
        il2cpp_add_internal_call("UnityEngine.AssetBundle::Unload(System.Boolean)", (void (*)(void))ov_ab_unload);
    } else if (!strcmp(name, "UnityEngine.QualitySettings::set_antiAliasing")) {
        s_set_aa = (void (*)(int32_t))fn;
        il2cpp_add_internal_call("UnityEngine.QualitySettings::set_antiAliasing(System.Int32)", (void (*)(void))ov_set_aa);
    } else if (!strcmp(name, "UnityEngine.QualitySettings::set_vSyncCount")) {
        s_set_vsync = (void (*)(int32_t))fn;
        il2cpp_add_internal_call("UnityEngine.QualitySettings::set_vSyncCount(System.Int32)", (void (*)(void))ov_set_vsync);
    } else if (!strcmp(name, "UnityEngine.Application::set_targetFrameRate")) {
        s_set_target = (void (*)(int32_t))fn;
        il2cpp_add_internal_call("UnityEngine.Application::set_targetFrameRate(System.Int32)", (void (*)(void))ov_set_target);
    }
}
