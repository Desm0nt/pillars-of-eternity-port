/* F2 JNI section for our Java front end (work/f2-app/java/org/pillars/port/DmActivity.java, the F2 twin of the port's
 * DmActivity): the on-screen pad and real controllers feed the game's nn.hid handheld pad (native/nn/f2_nn_hid.c), the
 * FPS counter reads the present statistics (f2_vkwrap.c), Java lines go into f2_log.txt.
 * The activity calls System.loadLibrary("il2cpp") after the engine has loaded this library, so the names resolve here. */
#include <jni.h>
#include <stdint.h>
#include "f2_boot.h"

void f2_nn_pad_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
void f2_frame_stats(float out[4]);
void f2_touch_set_mode(int touch);
void f2_touch_game_action(int id);
void f2_screen_set_display_hz(float hz);
void f2_screen_set_fps(int fps);

static int32_t axis(float v) { if (v > 1) v = 1; if (v < -1) v = -1; return (int32_t)(v * 32767.0f); }

/* buttons: PadTheme / nn::hid NpadButton bits (A=1 B=2 X=4 Y=8 LS RS L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN); sticks -1..1, Android's y (down = +) */
JNIEXPORT void JNICALL Java_org_pillars_port_DmActivity_nativeInput(JNIEnv *e, jclass c, jint buttons, jfloat lx, jfloat ly, jfloat rx, jfloat ry)
{
    (void)e; (void)c;
    f2_nn_pad_set((uint64_t)(uint32_t)buttons, axis(lx), axis(-ly), axis(rx), axis(-ry));
}

JNIEXPORT void JNICALL Java_org_pillars_port_DmActivity_nativeFrameStats(JNIEnv *e, jclass c, jfloatArray out)
{
    (void)c;
    float st[4];
    f2_frame_stats(st);
    if (out && (*e)->GetArrayLength(e, out) >= 4) (*e)->SetFloatArrayRegion(e, out, 0, 4, st);
}

/* which input the player is using now: 1 = a finger on the picture (the PC mouse path), 0 = the pad / a controller (the console input) */
JNIEXPORT void JNICALL Java_org_pillars_port_DmActivity_nativeTouchMode(JNIEnv *e, jclass c, jboolean touch)
{
    (void)e; (void)c;
    f2_touch_set_mode(touch ? 1 : 0);
}

/* a game action for a gesture (f2_touch.c f2_touch_game_action): 1 = select the whole party */
JNIEXPORT void JNICALL Java_org_pillars_port_DmActivity_nativeGameAction(JNIEnv *e, jclass c, jint id)
{
    (void)e; (void)c;
    f2_touch_game_action(id);
}

/* the refresh rate the display runs at now; the frame cap is re-made for it on the main thread (f2_screen.c) */
JNIEXPORT void JNICALL Java_org_pillars_port_DmActivity_nativeDisplayHz(JNIEnv *e, jclass c, jfloat hz)
{
    (void)e; (void)c;
    f2_screen_set_display_hz(hz);
    f2_touch_game_action(3);
}

/* the gear's frame rate, picked while the game runs */
JNIEXPORT void JNICALL Java_org_pillars_port_DmActivity_nativeSetFps(JNIEnv *e, jclass c, jint fps)
{
    (void)e; (void)c;
    f2_screen_set_fps(fps);
    f2_touch_game_action(3);
}

JNIEXPORT void JNICALL Java_org_pillars_port_DmActivity_nativeLog(JNIEnv *e, jclass c, jstring line)
{
    (void)c;
    const char *s = line ? (*e)->GetStringUTFChars(e, line, NULL) : NULL;
    if (s) { f2_log("F2: [java] %s", s); (*e)->ReleaseStringUTFChars(e, line, s); }
}
