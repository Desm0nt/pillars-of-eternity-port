/* F2 libmain.so: replaces Unity's 9 KB libmain.so (same contract: JNI_OnLoad registers
 * com.unity3d.player.NativeLoader.load(String) / unload(); load() dlopen's libunity.so from the native library
 * folder and calls its JNI_OnLoad).  Ours additionally loads libil2cpp.so FIRST and lets it prepare the process
 * (log file, folders, crash handler: f2_boot) and, once libunity is mapped but before any of its code runs,
 * patch and hook it (f2_unity_loaded).  The stock libmain also tries an optional libMGD.so (Mali Graphics
 * Debugger); ours does not. */
#include <jni.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <android/log.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "F2", __VA_ARGS__)

static void *s_unity, *s_il2cpp;

static jobject current_activity(JNIEnv *env)
{
    jclass c = (*env)->FindClass(env, "com/unity3d/player/UnityPlayer");
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return NULL; }
    jfieldID f = c ? (*env)->GetStaticFieldID(env, c, "currentActivity", "Landroid/app/Activity;") : NULL;
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return NULL; }
    return f ? (*env)->GetStaticObjectField(env, c, f) : NULL;
}

static jboolean Load(JNIEnv *env, jclass cls, jstring jdir)
{
    (void)cls;
    char dir[512] = "", path[700];
    const char *d = jdir ? (*env)->GetStringUTFChars(env, jdir, NULL) : NULL;
    if (d) { snprintf(dir, sizeof dir, "%s", d); (*env)->ReleaseStringUTFChars(env, jdir, d); }
    LOG("F2: [M01] libmain (F2) NativeLoader.load(%s)", dir);
    if (s_unity) return JNI_TRUE;
    JavaVM *vm = NULL;
    if ((*env)->GetJavaVM(env, &vm) < 0 || !vm) { (*env)->FatalError(env, "Unable to retrieve Java VM"); return JNI_FALSE; }

    /* 1. our runtime first: log, folders, crash handler */
    snprintf(path, sizeof path, "%s/libil2cpp.so", dir);
    s_il2cpp = dlopen(path, RTLD_NOW);
    if (!s_il2cpp) s_il2cpp = dlopen("libil2cpp.so", RTLD_NOW);
    if (!s_il2cpp) LOG("F2: [RED] cannot load %s: %s (continuing without the F2 boot layer: the engine will fail at LoadIl2Cpp)", path, dlerror());
    int (*boot)(void *, void *, void *, const char *) = s_il2cpp ? (int (*)(void *, void *, void *, const char *))dlsym(s_il2cpp, "f2_boot") : NULL;
    void (*unity_loaded)(void *) = s_il2cpp ? (void (*)(void *))dlsym(s_il2cpp, "f2_unity_loaded") : NULL;
    void (*onload_done)(int) = s_il2cpp ? (void (*)(int))dlsym(s_il2cpp, "f2_unity_onload_done") : NULL;
    if (s_il2cpp && (!boot || !unity_loaded)) LOG("F2: [RED] libil2cpp.so has no f2_boot / f2_unity_loaded: wrong library?");
    if (boot) boot(vm, env, current_activity(env), dir);

    /* 2. the engine */
    snprintf(path, sizeof path, "%s/libunity.so", dir);
    void *h = dlopen(path, RTLD_LAZY);
    if (!h) h = dlopen("libunity.so", RTLD_LAZY);
    if (!h) {
        char msg[1024];
        snprintf(msg, sizeof msg, "Unable to load library: %s [%s]", path, dlerror());
        LOG("F2: [RED] %s", msg);
        (*env)->FatalError(env, msg);
        return JNI_FALSE;
    }
    if (unity_loaded) unity_loaded(h);

    /* 3. its JNI_OnLoad, as the stock libmain does */
    jint (*onload)(JavaVM *, void *) = (jint (*)(JavaVM *, void *))dlsym(h, "JNI_OnLoad");
    jint ver = JNI_VERSION_1_6;
    if (onload) {
        ver = onload(vm, NULL);
        if (ver > JNI_VERSION_1_6) { (*env)->FatalError(env, "Unsupported VM version"); return JNI_FALSE; }
    } else LOG("F2: [RED] libunity.so has no JNI_OnLoad");
    if (onload_done) onload_done(ver);
    s_unity = h;
    return JNI_TRUE;
}

static jboolean Unload(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    LOG("F2: NativeLoader.unload (libraries stay mapped)");
    return JNI_TRUE;
}

static const JNINativeMethod k_methods[] = {
    { "load", "(Ljava/lang/String;)Z", (void *)Load },
    { "unload", "()Z", (void *)Unload },
};

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)reserved;
    JNIEnv *env = NULL;
    (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6);
    if (!env) return -1;
    jclass c = (*env)->FindClass(env, "com/unity3d/player/NativeLoader");
    if (!c || (*env)->RegisterNatives(env, c, k_methods, 2) < 0) {
        (*env)->FatalError(env, "com/unity3d/player/NativeLoader");
        return -1;
    }
    LOG("F2: libmain (F2 replacement) loaded");
    return JNI_VERSION_1_6;
}
