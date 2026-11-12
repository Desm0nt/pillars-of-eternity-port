/* F2: internal calls the Android libunity.so does not provide, and the few it provides that the Switch game
 * must see differently.
 *
 * (a) The 20 internal calls the game uses that the Android player never registers (19 x UnityEngine.Switch.*,
 *     1 dead System.Diagnostics.Process icall).  Signatures re-checked against dump.cs.
 * (b) Overrides.  libunity registers every icall under its SHORT name ("Type::Method"); the generated code asks
 *     InternalCalls::Resolve for the FULL name ("Type::Method(args)") first.  Registering a full name therefore
 *     wins over the engine's function without touching libunity or the registration order.
 *       UnityEngine.Application::get_platform()            -> RuntimePlatform.Switch (32) unless f2_platform.txt = android
 *       UnityEngine.Application::get_streamingAssetsPath() -> f2_managed_streaming_assets_path() if the boot layer defines it,
 *                                                             else f2_data_dir() + "/StreamingAssets"
 *       UnityEngine.Application::get_dataPath()            -> f2_managed_data_path() if defined, else f2_data_dir()
 *     (the game reads StreamingAssets with System.IO, so it needs a real directory there; the Android player's own
 *     answer is "jar:file://...apk!/assets".)
 * Registered from Runtime::Init (f2_runtime_gc_ready), i.e. before libunity registers its own.
 * Icalls are plain C functions: static methods get no `this`, no MethodInfo argument. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "f2_nn.h"

void il2cpp_add_internal_call(const char *name, void (*method)(void));
void *il2cpp_string_new(const char *str);

#define ICALL_ONCE(name) F2_ONCE("F2: icall stub %s", name)

static void sw_applet_begin(void) { ICALL_ONCE("UnityEngine.Switch.Applet::Begin"); }
static void sw_applet_end(void) { ICALL_ONCE("UnityEngine.Switch.Applet::End"); }
/* bool EnterNetworkConnecting(bool isLocalNetworkMode): no network -> false */
static uint8_t sw_net_enter(uint8_t local) { (void)local; ICALL_ONCE("UnityEngine.Switch.NetworkInterfaceWrapper::EnterNetworkConnecting -> false"); return 0; }
static uint8_t sw_net_weak_enter(void) { ICALL_ONCE("UnityEngine.Switch.NetworkInterfaceWrapper::WeakEnterNetworkConnecting -> false"); return 0; }
static void sw_net_leave(void) { ICALL_ONCE("UnityEngine.Switch.NetworkInterfaceWrapper::LeaveNetworkConnecting"); }
static uint8_t sw_net_is_connecting(void) { ICALL_ONCE("UnityEngine.Switch.NetworkInterfaceWrapper::IsNetworkConnecting -> false"); return 0; }
static void sw_net_wait(void) { ICALL_ONCE("UnityEngine.Switch.NetworkInterfaceWrapper::WaitForNetworkConnecting"); }
static uint8_t sw_net_is_accepted(void) { ICALL_ONCE("UnityEngine.Switch.NetworkInterfaceWrapper::IsNetworkAccepted -> false"); return 0; }
static uint8_t sw_net_is_finished(void) { ICALL_ONCE("UnityEngine.Switch.NetworkInterfaceWrapper::IsNetworkFinished -> true"); return 1; }
static int32_t sw_net_refcount(void) { ICALL_ONCE("UnityEngine.Switch.NetworkInterfaceWrapper::GetNetworkReferenceCount -> 0"); return 0; }

static void sw_set_focus_mode(int32_t mode) { F2_ONCE("F2: icall stub UnityEngine.Switch.Notification::SetFocusHandlingMode_INTERNAL(%d)", mode); }
/* Notification.FocusState: InFocus = 1, OutOfFocus = 2, Background = 3 */
static int32_t sw_focus_state(void) { ICALL_ONCE("UnityEngine.Switch.Notification::GetCurrentFocusState_INTERNAL -> InFocus"); return 1; }
static void sw_set_resume_notif(uint8_t on) { F2_ONCE("F2: icall stub UnityEngine.Switch.Notification::SetResumeNotificationEnabled_INTERNAL(%d)", on); }
static void sw_enter_exit_section(void) { ICALL_ONCE("UnityEngine.Switch.Notification::EnterExitRequestHandlingSection_INTERNAL"); }
static void sw_leave_exit_section(void) { ICALL_ONCE("UnityEngine.Switch.Notification::LeaveExitRequestHandlingSection_INTERNAL"); }
static void sw_set_opmode_notif(uint8_t on) { F2_ONCE("F2: icall stub UnityEngine.Switch.Notification::SetOperationModeChangedNotificationEnabled_INTERNAL(%d)", on); }
static void sw_set_perfmode_notif(uint8_t on) { F2_ONCE("F2: icall stub UnityEngine.Switch.Notification::SetPerformanceModeChangedNotificationEnabled_INTERNAL(%d)", on); }
/* Performance.PerformanceMode: Normal = 0 (handheld), Boost = 1 (docked).  Owner rule: docked look.
 * The game's ResolutionController picks 1600x900 for Boost, 1280x720 for Normal. */
static int32_t sw_perf_mode(void) { ICALL_ONCE("UnityEngine.Switch.Performance::GetMode_INTERNAL -> Boost (docked)"); return 1; }
/* instance method: `this` only */
static void sw_video_dispose(void *self) { (void)self; ICALL_ONCE("UnityEngine.Switch.SwitchVideoPlayer::Dispose"); }
static intptr_t sw_handle_dup(intptr_t h) { ICALL_ONCE("System.Diagnostics.Process/ProcessWaitHandle::ProcessHandle_duplicate"); return h; }

/* ---- overrides of engine icalls ---- */
static int32_t ov_get_platform(void)
{
    F2_ONCE("F2: icall override UnityEngine.Application::get_platform -> 32 (Switch)");
    return 32;
}
/* Folder the managed code gets: the boot layer's choice if it provides one, else the shared Data folder. */
static const char *managed_streaming_assets(void)
{
    static char buf[1024];
    const char *p = &f2_managed_streaming_assets_path ? f2_managed_streaming_assets_path() : NULL;
    if (p && *p) return p;
    p = f2_data_dir();
    if (!p || !*p) return NULL;
    if (!buf[0]) snprintf(buf, sizeof buf, "%s/StreamingAssets", p);
    return buf;
}
static const char *managed_data(void)
{
    const char *p = &f2_managed_data_path ? f2_managed_data_path() : NULL;
    if (p && *p) return p;
    p = f2_data_dir();
    return p && *p ? p : NULL;
}
static void *ov_streaming_assets_path(void)
{
    const char *p = managed_streaming_assets();
    F2_ONCE("F2: icall override UnityEngine.Application::get_streamingAssetsPath -> %s", p);
    return il2cpp_string_new(p);
}
static void *ov_data_path(void)
{
    const char *p = managed_data();
    F2_ONCE("F2: icall override UnityEngine.Application::get_dataPath -> %s", p);
    return il2cpp_string_new(p);
}

static const struct { const char *name; void *fn; } k_icalls[] = {
    { "UnityEngine.Switch.Applet::Begin", sw_applet_begin },
    { "UnityEngine.Switch.Applet::End", sw_applet_end },
    { "UnityEngine.Switch.NetworkInterfaceWrapper::EnterNetworkConnecting", sw_net_enter },
    { "UnityEngine.Switch.NetworkInterfaceWrapper::GetNetworkReferenceCount", sw_net_refcount },
    { "UnityEngine.Switch.NetworkInterfaceWrapper::IsNetworkAccepted", sw_net_is_accepted },
    { "UnityEngine.Switch.NetworkInterfaceWrapper::IsNetworkConnecting", sw_net_is_connecting },
    { "UnityEngine.Switch.NetworkInterfaceWrapper::IsNetworkFinished", sw_net_is_finished },
    { "UnityEngine.Switch.NetworkInterfaceWrapper::LeaveNetworkConnecting", sw_net_leave },
    { "UnityEngine.Switch.NetworkInterfaceWrapper::WaitForNetworkConnecting", sw_net_wait },
    { "UnityEngine.Switch.NetworkInterfaceWrapper::WeakEnterNetworkConnecting", sw_net_weak_enter },
    { "UnityEngine.Switch.Notification::EnterExitRequestHandlingSection_INTERNAL", sw_enter_exit_section },
    { "UnityEngine.Switch.Notification::GetCurrentFocusState_INTERNAL", sw_focus_state },
    { "UnityEngine.Switch.Notification::LeaveExitRequestHandlingSection_INTERNAL", sw_leave_exit_section },
    { "UnityEngine.Switch.Notification::SetFocusHandlingMode_INTERNAL", sw_set_focus_mode },
    { "UnityEngine.Switch.Notification::SetOperationModeChangedNotificationEnabled_INTERNAL", sw_set_opmode_notif },
    { "UnityEngine.Switch.Notification::SetPerformanceModeChangedNotificationEnabled_INTERNAL", sw_set_perfmode_notif },
    { "UnityEngine.Switch.Notification::SetResumeNotificationEnabled_INTERNAL", sw_set_resume_notif },
    { "UnityEngine.Switch.Performance::GetMode_INTERNAL", sw_perf_mode },
    { "UnityEngine.Switch.SwitchVideoPlayer::Dispose", sw_video_dispose },
    { "System.Diagnostics.Process/ProcessWaitHandle::ProcessHandle_duplicate", sw_handle_dup },
};

void f2_nn_ensure_init(void);

void f2_register_switch_icalls(void)
{
    f2_nn_ensure_init();      /* configuration files first: the platform override depends on them */
    for (unsigned i = 0; i < sizeof(k_icalls) / sizeof(k_icalls[0]); i++)
        il2cpp_add_internal_call(k_icalls[i].name, (void (*)(void))k_icalls[i].fn);
    unsigned ov = 0;
    if (f2_nn_report_switch_platform()) {
        il2cpp_add_internal_call("UnityEngine.Application::get_platform()", (void (*)(void))ov_get_platform); ov++;
    }
    if (managed_streaming_assets()) {
        il2cpp_add_internal_call("UnityEngine.Application::get_streamingAssetsPath()", (void (*)(void))ov_streaming_assets_path); ov++;
    }
    if (managed_data()) {
        il2cpp_add_internal_call("UnityEngine.Application::get_dataPath()", (void (*)(void))ov_data_path); ov++;
    }
    f2_log("F2: %u Switch-only internal calls registered, %u engine icalls overridden (platform %s, streamingAssetsPath %s, dataPath %s)",
           (unsigned)(sizeof(k_icalls) / sizeof(k_icalls[0])), ov, f2_nn_report_switch_platform() ? "Switch" : "engine's own",
           managed_streaming_assets() ? managed_streaming_assets() : "engine's own", managed_data() ? managed_data() : "engine's own");
}
