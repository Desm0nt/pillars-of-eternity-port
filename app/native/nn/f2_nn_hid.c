/* F2: nn.hid -- one controller in handheld style (NpadId.Handheld = 0x20, NpadStyle.Handheld = 2), neutral by
 * default.  f2_nn_pad_set() lets a later on-screen pad feed it.
 *
 * What the game does (GripDigital.InControl.Switch.SwitchInputDeviceManager + InControl.Source.Switch.Joycon):
 *   ctor: Npad.Initialize, NpadJoy.SetHoldType(Horizontal), SetSupportedIdType([No1, Handheld]), SetSupportedStyleSet
 *   every frame: Npad.GetStyleSet(id) for each supported id -> a device is attached while the style is neither
 *   None nor Invalid; Joycon.Update -> Npad.GetState(ref NpadState, id, style) -> NpadHandheld.GetState.
 * With no connected style the game asks for the controller applet (ControllerSupport.Show) -- hence the handheld pad.
 * The game does not call nn.hid.TouchScreen at all (touch goes through UnityEngine.Input, which the Android player
 * feeds natively); the TouchScreen wrappers report "no touches". */
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "f2_nn.h"

enum { STYLE_FULLKEY = 1, STYLE_HANDHELD = 2, STYLE_JOYDUAL = 4, STYLE_JOYLEFT = 8, STYLE_JOYRIGHT = 16 };
enum { ID_HANDHELD = 0x20 };
enum { ATTR_HANDHELD = 0x3f };       /* IsConnected | IsWired | left/right connected + wired */

/* nn::hid::Npad*State (0x28 bytes): the managed NpadState adds preButtons at +0x28 itself */
typedef struct {
    int64_t sampling;
    uint64_t buttons;
    int32_t lx, ly, rx, ry;
    uint32_t attributes;
    uint32_t pad;
} nn_npad_state_t;
_Static_assert(sizeof(nn_npad_state_t) == 0x28, "NpadState");

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t s_buttons;
static int32_t s_lx, s_ly, s_rx, s_ry;
static int64_t s_sampling;
static uint32_t s_supported_styles = STYLE_FULLKEY | STYLE_HANDHELD | STYLE_JOYDUAL;
static int32_t s_hold_type, s_activation_mode, s_comm_mode;
static uint8_t s_style_event[8];

void f2_nn_pad_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry)
{
    pthread_mutex_lock(&s_lock);
    s_buttons = buttons; s_lx = lx; s_ly = ly; s_rx = rx; s_ry = ry;
    pthread_mutex_unlock(&s_lock);
}

static int connected(uint32_t id) { return id == ID_HANDHELD; }

static void fill_state(nn_npad_state_t *st, uint32_t id)
{
    if (!st) return;
    pthread_mutex_lock(&s_lock);
    st->sampling = ++s_sampling;
    if (connected(id)) {
        st->buttons = s_buttons; st->lx = s_lx; st->ly = s_ly; st->rx = s_rx; st->ry = s_ry;
        st->attributes = ATTR_HANDHELD;
    } else {
        st->buttons = 0; st->lx = st->ly = st->rx = st->ry = 0; st->attributes = 0;
    }
    pthread_mutex_unlock(&s_lock);
}

void nn_hid_InitializeNpad(void) { F2_ONCE("F2: nn hid.Npad.Initialize (one handheld-style controller, neutral)"); }
void nn_hid_SetSupportedStyleSet(uint32_t styles) { F2_ONCE("F2: nn hid.Npad.SetSupportedStyleSet(0x%x)", styles); s_supported_styles = styles; }
uint32_t nn_hid_GetSupportedStyleSet(void) { F2_ONCE("F2: nn hid.Npad.GetSupportedStyleSet"); return s_supported_styles; }

void nn_hid_SetSupportedIdType(const uint32_t *ids, uint64_t count)
{
    char buf[128]; int n = 0;
    buf[0] = 0;
    for (uint64_t i = 0; ids && i < count && n < 100; i++) n += snprintf(buf + n, sizeof buf - n, " 0x%x", ids[i]);
    F2_ONCE("F2: nn hid.Npad.SetSupportedIdType(%llu ids:%s)", (unsigned long long)count, buf);
}

void nn_hid_BindStyleSetUpdateEvent(uint32_t id) { F2_ONCE("F2: nn hid.Npad.BindStyleSetUpdateEvent"); if (id < 8) s_style_event[id] = 1; }
/* true once after binding (the style set "changed" from nothing to the current one) */
uint32_t nn_hid_IsStyleSetUpdated(uint32_t id)
{
    F2_ONCE("F2: nn hid.Npad.IsStyleSetUpdated");
    uint32_t i = id < 8 ? id : 7;
    uint32_t r = s_style_event[i];
    s_style_event[i] = 0;
    return r;
}
void nn_hid_DestroyStyleSetUpdateEvent(uint32_t id) { F2_ONCE("F2: nn hid.Npad.DestroyStyleSetUpdateEvent"); if (id < 8) s_style_event[id] = 0; }

uint32_t nn_hid_GetStyleSet(uint32_t id)
{
    F2_ONCE("F2: nn hid.Npad.GetStyleSet (first id 0x%x; Handheld 0x20 -> style Handheld, others -> None)", id);
    return connected(id) ? STYLE_HANDHELD : 0;
}

void nn_hid_Disconnect(uint32_t id) { F2_ONCE("F2: nn hid.Npad.Disconnect(0x%x) ignored", id); }
uint32_t nn_hid_GetPlayerLedPattern(uint32_t id) { F2_ONCE("F2: nn hid.Npad.GetPlayerLedPattern"); return id == 0 ? 1 : 0; }

nn_result_t nn_hid_GetControllerColor1(uint64_t *out, uint32_t id)
{
    F2_ONCE("F2: nn hid.Npad.GetControllerColor");
    (void)id;
    if (out) *out = 0;                       /* NpadControllerColor = 2 x Color4u8 */
    return NN_OK;
}
nn_result_t nn_hid_GetControllerColor2(uint64_t *left, uint64_t *right, uint32_t id)
{
    F2_ONCE("F2: nn hid.Npad.GetControllerColor (left, right)");
    (void)id;
    if (left) *left = 0;
    if (right) *right = 0;
    return NN_OK;
}

/* Npad{FullKey,Handheld,JoyDual,JoyLeft,JoyRight}.GetState(ref state, id) */
void nn_hid_GetNpadState(nn_npad_state_t *st, uint32_t id)
{
    F2_ONCE("F2: nn hid.Npad*.GetState (first id 0x%x)", id);
    fill_state(st, id);
}

/* ...GetStates(states[], count, id) -> number written */
int32_t nn_hid_GetNpadStates(nn_npad_state_t *st, int32_t count, uint32_t id)
{
    F2_ONCE("F2: nn hid.Npad*.GetStates (first id 0x%x)", id);
    if (!st || count < 1) return 0;
    fill_state(st, id);
    return 1;
}

uint32_t nn_hid_GetJoyAssignment(uint32_t id) { (void)id; F2_ONCE("F2: nn hid.NpadJoy.GetAssignment -> Dual"); return 0; }
void nn_hid_noop_id(uint32_t id) { (void)id; F2_ONCE("F2: nn hid.NpadJoy assignment call ignored"); }
nn_result_t nn_hid_MergeSingleAsDual(uint32_t a, uint32_t b) { (void)a; (void)b; F2_ONCE("F2: nn hid.NpadJoy.MergeSingleAsDual ignored"); return NN_OK; }
void nn_hid_SetHoldType(int32_t t) { F2_ONCE("F2: nn hid.NpadJoy.SetHoldType(%d)", t); s_hold_type = t; }
int32_t nn_hid_GetHoldType(void) { F2_ONCE("F2: nn hid.NpadJoy.GetHoldType"); return s_hold_type; }
void nn_hid_SetHandheldActivationMode(int32_t m) { F2_ONCE("F2: nn hid.NpadJoy.SetHandheldActivationMode(%d)", m); s_activation_mode = m; }
int32_t nn_hid_GetHandheldActivationMode(void) { F2_ONCE("F2: nn hid.NpadJoy.GetHandheldActivationMode"); return s_activation_mode; }
void nn_hid_SetCommunicationMode(int32_t m) { F2_ONCE("F2: nn hid.NpadJoy.SetCommunicationMode(%d)", m); s_comm_mode = m; }
int32_t nn_hid_GetCommunicationMode(void) { F2_ONCE("F2: nn hid.NpadJoy.GetCommunicationMode"); return s_comm_mode; }

/* ---- touch screen: TouchScreenState<N> = { s64 samplingNumber; s32 count; s32 reserved; TouchState[N] } ---- */
void nn_hid_InitializeTouchScreen(void) { F2_ONCE("F2: nn hid.TouchScreen.Initialize (touch is served by UnityEngine.Input, this reports none)"); }
void nn_hid_GetTouchScreenState(int64_t *st)
{
    F2_ONCE("F2: nn hid.TouchScreen.GetState -> 0 touches");
    if (!st) return;
    pthread_mutex_lock(&s_lock);
    st[0] = ++s_sampling;
    pthread_mutex_unlock(&s_lock);
    st[1] = 0;
}
int32_t nn_hid_GetTouchScreenStates(int64_t *st, int32_t count)
{
    F2_ONCE("F2: nn hid.TouchScreen.GetStates -> 0 touches");
    if (!st || count < 1) return 0;
    nn_hid_GetTouchScreenState(st);
    return 1;
}

/* ---- vibration: two devices so that the game's arrays are filled; values are dropped ---- */
int32_t nn_hid_GetVibrationDeviceHandles(uint32_t *out, int32_t count, uint32_t id, uint32_t style)
{
    F2_ONCE("F2: nn hid.Vibration.GetDeviceHandles(id 0x%x, style 0x%x) -> %d", id, style, connected(id) && count >= 2 ? 2 : 0);
    if (!out || !connected(id) || count < 2) return 0;
    out[0] = 0x00200100u; out[1] = 0x00200101u;      /* opaque to the game */
    return 2;
}
void nn_hid_GetVibrationDeviceInfo(int32_t *out, uint32_t handle)
{
    F2_ONCE("F2: nn hid.Vibration.GetDeviceInfo");
    if (out) { out[0] = 1; out[1] = (handle & 1) ? 2 : 1; }   /* LinearResonantActuator; Left / Right */
}
void nn_hid_InitializeVibrationDevice(uint32_t handle) { (void)handle; F2_ONCE("F2: nn hid.Vibration.InitializeDevice"); }
void nn_hid_SendVibrationValue(uint32_t handle) { (void)handle; F2_ONCE("F2: nn hid.Vibration.SendValue (dropped)"); }
void nn_hid_GetActualVibrationValue(float *out, uint32_t handle)
{
    (void)handle; F2_ONCE("F2: nn hid.Vibration.GetActualValue");
    if (out) { out[0] = 0; out[1] = 160; out[2] = 0; out[3] = 320; }
}
uint32_t nn_hid_IsVibrationPermitted(void) { F2_ONCE("F2: nn hid.Vibration.IsPermitted -> false"); return 0; }

/* ---- applets ---- */
nn_result_t nn_hid_ShowControllerSupport(void)
{
    F2_ONCE("F2: nn hid.ControllerSupport.Show requested (no applet on Android; returns success)");
    return NN_OK;
}
nn_result_t nn_hid_ShowControllerSupportInfo(uint8_t *info)
{
    F2_ONCE("F2: nn hid.ControllerSupport.Show(resultInfo) requested (no applet on Android; returns success, 1 player)");
    if (info) { memset(info, 0, 8); info[0] = 1; ((uint32_t *)info)[1] = ID_HANDHELD; }   /* { s8 playerCount; NpadIdType selectedId } */
    return NN_OK;
}
