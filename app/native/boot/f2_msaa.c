/* F2 msaa: give back anti-aliasing to particles and UI (the game's later, direct-to-backbuffer cameras) without
 * forcing the deferred G-buffer render black (work/f2-msaa/REPORT.md: with QualitySettings.antiAliasing = 4 the
 * 2018.1 Vulkan backend renders PE_GameRender's own MRT G-buffer pass black on this device; msaa=0 avoids it but
 * loses AA on particles and UI too, which on the Switch *are* antialiased).
 *
 * Variant 2 of the report: leave QualitySettings.antiAliasing = 4 (so the back buffer / later overlay cameras keep
 * MSAA) but force Camera.allowMSAA = false on the game's own area cameras, so their camera-stack temp target
 * (UnityEngine engine: CameraStackRenderingState::GetCameraStackTempTextureDesc, main+0x74611c) comes back at
 * 1 sample, matching the 1-sample RenderTextures PE_GameRender already renders its G-buffer into
 * (PE_GameRender.CreateRenderTarget, main+0x1316650) -- which is the report's hypothesis H1 (a render pass mixing
 * 1x and 4x attachments) side-stepped rather than proven.  Only active when f2_cfg.msaa forces MSAA on for testing
 * (4 or 8); with the current default (msaa=0) this module does nothing, matching "do nothing otherwise".
 *
 * How the cameras are found, verified with tools/adis.py + tools/symbolize.py on the Switch image and
 * tools/udis.py on libunity.so (see work/f2-msaa/ for the raw listings this was built from):
 *   - PE_GameRender.OnRenderImage (main+0x13114F0) is itself a Camera image-effect callback, so the camera it is
 *     invoked for *is* the game's main area camera.  Inside it, main+0x1311ab0..0x1311ae4 reads:
 *       mainCamera = this.gameObject.GetComponent<Camera>();
 *       CopyMainCameraSettingsToGBufferCamera(this.m_camera_dynamicGBuffer, mainCamera);
 *     (CopyMainCameraSettingsToGBufferCamera, main+0x1312820, confirmed by its own body: entry has
 *     mov x20,x2 / mov x19,x1 then x19.set_orthographic(x20.get_orthographic()) etc, i.e. param 1 = dest camera,
 *     param 2 = main camera -- matching x1 = this.m_camera_dynamicGBuffer (+0x180), x2 = the GetComponent result).
 *   - The gate sits at main+0x1311adc ("ldr x1, [x20, #0x180]"), one instruction after the GetComponent<Camera>
 *     call and before the CopyMainCameraSettingsToGBufferCamera call (both of those are "bl", PC-relative, so they
 *     cannot be gated themselves).  At that point x0 already holds mainCamera (the GetComponent<Camera> result,
 *     untouched since the call returned) and x20 still holds "this" (PE_GameRender.Instance, callee-saved).  The
 *     displaced instruction is a plain register+immediate load, not PC-relative: safe to run after the hook.
 *   - PE_GameRender's own fields (dump.cs) give the other two off-screen cameras SetupCameras() creates, read
 *     directly from "this" rather than re-deriving them: m_camera_dynamicShadowBuffer (+0x178, RVA offsets per
 *     dump.cs field layout) and m_camera_dynamicGBuffer (+0x180).  Both are rendered into the 1-sample
 *     RenderTextures PE_GameRender.CreateRenderTarget allocates, so they get the same treatment as the main
 *     camera.  (SetupCameras creates them via GameObject.AddComponent<Camera>(); this module does not re-trace
 *     that path since the fields already give a stable, guaranteed-initialized pointer by the time
 *     OnRenderImage runs every frame.)
 *   - Camera.allowMSAA has no managed wrapper in this game's (Switch-built) UnityEngine.dll, but libunity.so still
 *     registers the icall under its full name regardless -- confirmed in the engine binary's string table
 *     ("UnityEngine.Camera::set_allowMSAA" / "get_allowMSAA" both present) and by disassembling the registered
 *     targets (tools/udis.py "Camera_Set_Custom_PropAllowMSAA" / "Camera_Get_Custom_PropAllowMSAA"): both take the
 *     MANAGED Camera object pointer directly (they call Scripting::GetCachedPtrFromScriptingWrapper on it
 *     themselves, i.e. deref +0x10, same as the port's own convention for a UnityEngine.Object's native pointer)
 *     and raise a managed NullReferenceException from native code if that pointer is null -- so this module always
 *     NULL-checks before calling either. */
#include <stdint.h>
#include <string.h>
#include "f2_boot.h"

typedef void (*f2_gate_hook)(uint64_t *regs);
typedef void (*f2_gate_add)(uint32_t off, uint32_t expect, f2_gate_hook hook, const char *what);

/* PE_GameRender field offsets (work/il2cpp/dump.cs, class PE_GameRender ~ line 286940) */
enum { OFF_SHADOW_CAM = 0x178, OFF_GBUFFER_CAM = 0x180 };

static void (*s_set_allow_msaa)(void *managed_camera, uint8_t value);
static uint8_t (*s_get_allow_msaa)(void *managed_camera);

/* called for every icall libunity registers (f2_diag.cpp); we only ever need the pointers, never override --
 * nothing in the game's managed code calls Camera.allowMSAA, so there is no icall name to resolve first here */
void f2_msaa_engine_icall(const char *name, void (*fn)(void))
{
    if (!name || !fn) return;
    if (!strcmp(name, "UnityEngine.Camera::set_allowMSAA")) s_set_allow_msaa = (void (*)(void *, uint8_t))fn;
    else if (!strcmp(name, "UnityEngine.Camera::get_allowMSAA")) s_get_allow_msaa = (uint8_t (*)(void *))fn;
}

static int msaa_forced_on(void) { return f2_cfg.msaa == 4 || f2_cfg.msaa == 8; }

static int s_log_n;
static void set_no_msaa(void *cam, const char *what)
{
    if (!cam || !s_set_allow_msaa) return;
    int had_old = s_get_allow_msaa != NULL;
    uint8_t old = had_old ? s_get_allow_msaa(cam) : 0;
    s_set_allow_msaa(cam, 0);
    if (s_log_n < 24) { s_log_n++; f2_log("F2: msaa: %s %p: allowMSAA %s-> 0", what, cam, had_old ? (old ? "1 " : "0 ") : "? "); }
}

static void *s_last_owner, *s_last_main, *s_last_shadow, *s_last_gbuf;

/* gate "msaa/onrender": main+0x1311adc, inside PE_GameRender.OnRenderImage (see file header).  Regs at the hook:
 *   x0  = mainCamera (just returned by GetComponent<Camera>() on this.gameObject -- the main area camera)
 *   x20 = this (PE_GameRender.Instance); +0x178 / +0x180 give the shadow-buffer / G-buffer cameras */
static void gate_onrender(uint64_t *regs)
{
    if (!msaa_forced_on()) return;
    void *self = (void *)(uintptr_t)regs[20];
    if (!self) return;
    void *main_cam = (void *)(uintptr_t)regs[0];
    void *shadow_cam = *(void **)((char *)self + OFF_SHADOW_CAM);
    void *gbuf_cam = *(void **)((char *)self + OFF_GBUFFER_CAM);
    if (self == s_last_owner && main_cam == s_last_main && shadow_cam == s_last_shadow && gbuf_cam == s_last_gbuf) return;
    s_last_owner = self; s_last_main = main_cam; s_last_shadow = shadow_cam; s_last_gbuf = gbuf_cam;
    if (s_log_n < 24) { s_log_n++; f2_log("F2: msaa: PE_GameRender %p: main camera %p, dynamicShadowBuffer %p, dynamicGBuffer %p", self, main_cam, shadow_cam, gbuf_cam); }
    set_no_msaa(main_cam, "main camera");
    set_no_msaa(shadow_cam, "dynamicShadowBuffer camera");
    set_no_msaa(gbuf_cam, "dynamicGBuffer camera");
}

/* the loader asks for the gates while the game image is still writable (native/loader/f2_loader.c) */
void f2_msaa_gates(f2_gate_add add)
{
    if (!msaa_forced_on()) { f2_log("F2: msaa: off (msaa=%d; forcing allowMSAA=false on the area cameras only kicks in with msaa=4 or 8)", f2_cfg.msaa); return; }
    add(0x1311adc, 0xf940c281u, gate_onrender, "msaa/onrender (PE_GameRender.OnRenderImage, after this.gameObject.GetComponent<Camera>())");
}
