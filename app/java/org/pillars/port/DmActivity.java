// Pillars of Eternity F2 (the stock Android Unity player + our runtime): the F2 twin of the port's DmActivity (poe-port/frontend), so that the port's
// shared front end runs unchanged on top of the Unity player. The front end's classes are compiled from poe-port/frontend as they are (package
// org.pillars.port; all but its DmActivity and PoeGear, which this file and F2Gear replace) into classes2.dex next to Unity's classes.dex.
//   the window: Unity's own (UnityPlayerActivity: a UnityPlayer with its SurfaceView), drawn under the camera cut-out too; over it a transparent
//   layer with our on-screen gamepad (PoePadView: floating sticks, round buttons, our gear) and the pause menu (FrontMenu) with the gear (GearSettings)
//   the pad and real controllers: the game reads nn.hid (the Switch plugin), never Unity's joysticks, so both feed our handheld pad
//   (nativeInput -> native/nn/f2_nn_hid.c f2_nn_pad_set)
//   touch: every finger the pad does not take is the game's: re-made as touch events for the Unity player (UnityPlayer.injectEvent), which the
//   game sees through UnityEngine.Input as on any Android Unity game
//   settings: SharedPreferences as in the port ("doom_controls" the pad, "doom_graphics" the gear, "front" the menu language); the game's language is
//   files/f2_lang.txt (read by native/nn/f2_nn_misc.c at the start)
// Natives are in libil2cpp.so (native/boot/f2_jni.c); the engine loads that library itself, System.loadLibrary only makes its JNI names visible here.
package org.pillars.port;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.hardware.input.InputManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.SystemClock;
import android.provider.Settings;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.FrameLayout;
import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStream;
import java.lang.ref.WeakReference;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

public class DmActivity extends com.unity3d.player.UnityPlayerActivity implements InputManager.InputDeviceListener, FrontMenu.Host, FrontMenu.Owner, GearHost,
        PoePadView.Game {
    static final String TAG = "pillars-f2";
    static final String GAME_PREFS = "doom_graphics";            // GearHost.gamePrefs: the shared name every port uses
    static final String DATA_DIR = "PillarsOfEternityMobile";
    static volatile boolean libraryLoaded;

    // ---- libil2cpp.so (native/boot/f2_jni.c)
    static native void nativeInput(int buttons, float lx, float ly, float rx, float ry);
    static native void nativeFrameStats(float[] out);
    static native void nativeLog(String line);
    static native void nativeTouchMode(boolean touch);
    static native void nativeGameAction(int id);
    static native void nativeDisplayHz(float hz);
    static native void nativeSetFps(int fps);

    private static WeakReference<DmActivity> current = new WeakReference<>(null);
    private FrameLayout layer; private PoePadView pad; private FrontMenu front; private F2Touch touch; private boolean touchMouse;
    private boolean started;
    private InputManager inputs;

    @Override protected void onCreate(Bundle b) {
        installCrashHandler();               // a Java exception before the engine exists would otherwise just vanish
        CrashLog.startLogcatCapture(getApplicationContext());   // from the first moment of the process (its own uid's lines only)
        // ORDER: the last run's marker is read (and removed) FIRST, this run's marker is written after.  The other way round (until
        // 12.11.2026) every start read its OWN fresh marker and wrote a false crash.txt, and the last run's real pid / state were lost.
        String last = CrashLog.checkPrevious(getApplicationContext(), (f, why) ->
            runOnUiThread(() -> crumb("crash log: " + why + (f != null ? " -> " + f.getAbsolutePath() : " (report not written)"))));
        CrashLog.session(this, "boot");
        if (last != null) crumb("crash log: " + last);
        // (storage access is asked by StartActivity, the icon's entry, BEFORE this activity: here the engine is already starting)
        writeGameLanguage();                 // before the engine starts: native/nn reads files/f2_lang.txt once
        writeFront();                        // and native/boot reads files/f2_front.txt (GameFolder.findDataRoot needs ensureFileAccess above)
        super.onCreate(b);                   // UnityPlayer: loads libmain.so -> our libil2cpp.so, libunity.so
        current = new WeakReference<>(this);
        fitWindow(gamePrefs().getInt(F2Gear.P_WIDE, 0) == 1, getIntent() == null ? null : getIntent().getStringExtra("box"));
        // the picture under the camera cut-out as well (the 2018.1 player knows nothing of cut-outs: its window stopped short of it, a black strip)
        WindowManager.LayoutParams lp = getWindow().getAttributes();
        lp.layoutInDisplayCutoutMode = Build.VERSION.SDK_INT >= 30 ? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
            : WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        getWindow().setAttributes(lp);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        // am start ... --ez wake true (tests over adb while the phone is locked): shown over the lock screen with the screen on, as the DEVELOPMENT
        // player's Java does by itself; without it the Release player waits for the unlock (no focus -> no resume -> no frame: phone run 29)
        if (getIntent() != null && getIntent().getBooleanExtra("wake", false))
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_SHOW_WHEN_LOCKED | WindowManager.LayoutParams.FLAG_DISMISS_KEYGUARD | WindowManager.LayoutParams.FLAG_TURN_SCREEN_ON);
        applyRefresh();
        try { System.loadLibrary("il2cpp"); libraryLoaded = true; }
        catch (UnsatisfiedLinkError e) { android.util.Log.e(TAG, "libil2cpp.so (JNI names): " + e); }
        firstRun();
        Intent it = getIntent();             // am start ... --es skin <pack> --ez pad <true|false>: the same choices the menus make (tests over adb)
        if (it != null) {
            String sk = it.getStringExtra("skin");
            if (sk != null) padPrefs().edit().putString("skin", sk).apply();
            if (it.hasExtra("pad")) padPrefs().edit().putBoolean("shown", it.getBooleanExtra("pad", false)).apply();
        }
        try { GearSettings.applyEnv(this); } catch (Throwable t) { android.util.Log.e(TAG, "gear env: " + t); }
        layer = new FrameLayout(this);
        // touch as the PC mouse (the gear's switch, on by default): the game's fingers become mouse / key events (F2Touch); off = they stay touches
        touchMouse = gamePrefs().getBoolean(F2Gear.P_TOUCH, true);
        touch = new F2Touch(new F2Touch.Out() {
            public void inject(android.view.InputEvent e) { if (mUnityPlayer != null) mUnityPlayer.injectEvent(e); }
            public void log(String line) { crumb(line); }
            public void padPulse(int bit) { pulse(bit); }
            public void gameAction(int id) { if (libraryLoaded) try { nativeGameAction(id); } catch (UnsatisfiedLinkError e) { /* an older library */ } }
        }, getResources().getDisplayMetrics().density);
        touch.setHoldMs(gamePrefs().getInt(F2Gear.P_HOLD, 500));
        pad = new PoePadView(this); layer.addView(pad, new FrameLayout.LayoutParams(-1, -1));
        front = new FrontMenu(this, this, pad, layer);
        addContentView(layer, new ViewGroup.LayoutParams(-1, -1));
        immersive();
        inputs = getSystemService(InputManager.class); if (inputs != null) inputs.registerInputDeviceListener(this, null);
        started = true;
        if (!refreshNote.isEmpty()) crumb(refreshNote);
        crumb("F2 front end up: " + Build.MANUFACTURER + " " + Build.MODEL + ", Android " + Build.VERSION.RELEASE + ", pad " + (padPrefs().getBoolean("shown", false) ? "shown" : "hidden")
            + ", skin " + padPrefs().getString("skin", PadSkin.CLASSIC) + ", cut-out mode " + lp.layoutInDisplayCutoutMode);
    }
    @Override public void immersive() {
        WindowInsetsController c = getWindow().getInsetsController();
        if (c != null) { c.hide(WindowInsets.Type.systemBars()); c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE); }
    }
    @Override public SharedPreferences gamePrefs() { return getSharedPreferences(GAME_PREFS, MODE_PRIVATE); }
    SharedPreferences padPrefs() { return getSharedPreferences(PoePadView.PREFS, MODE_PRIVATE); }

    // ---- the first start: the pad SHOWN (unlike the port: F2 has no touch-as-mouse yet, the pad is how the game is played), the classic D3 / TL2 look,
    // the menus in Russian
    static final String DEFAULTS_KEY = "f2_defaults_v1";
    private void firstRun() {
        SharedPreferences cp = padPrefs();
        if (!cp.getBoolean(DEFAULTS_KEY, false)) {
            cp.edit().putBoolean(DEFAULTS_KEY, true).putBoolean("shown", true).putString("skin", PadSkin.CLASSIC).apply();
            SharedPreferences fp = getSharedPreferences(Ui.PREFS, MODE_PRIVATE);
            if (!fp.contains("ui_lang")) fp.edit().putString("ui_lang", "ru").apply();
        }
    }
    // The game's language: the gear's pref "lang" ("" = leave the file alone; the default without a file is the native side's) -> files/f2_lang.txt
    private void writeGameLanguage() {
        String l = gamePrefs().getString("lang", "");
        File ext = getExternalFilesDir(null);
        if (l.isEmpty() || ext == null) return;
        try (OutputStream o = new FileOutputStream(new File(ext, "f2_lang.txt"))) { o.write((l + "\n").getBytes(StandardCharsets.UTF_8)); }
        catch (Exception e) { android.util.Log.e(TAG, "f2_lang.txt: " + e); }
    }

    // The gear's frame rate, the display's side: the window asks for a display mode of the same size at that refresh rate (60 for the game's 30).
    // Nothing is asked until the player picks a rate. The system may refuse (some phones keep unknown apps at 60): the log line says what came out
    private String refreshNote = "";
    private void applyRefresh() {
        int fps = gamePrefs().getInt(F2Gear.P_FPS, 60); float want = fps == 30 ? 60 : fps;   // (60 is the default: asked for as well)
        android.view.Display d = getWindowManager().getDefaultDisplay(); android.view.Display.Mode cur = d.getMode(), best = null;
        StringBuilder all = new StringBuilder();
        for (android.view.Display.Mode m : d.getSupportedModes()) {
            all.append(' ').append(m.getPhysicalWidth()).append('x').append(m.getPhysicalHeight()).append('@').append(Math.round(m.getRefreshRate()));
            if (m.getPhysicalWidth() == cur.getPhysicalWidth() && m.getPhysicalHeight() == cur.getPhysicalHeight() && Math.abs(m.getRefreshRate() - want) < 1.5f) best = m;
        }
        WindowManager.LayoutParams lp = getWindow().getAttributes();
        if (best != null) lp.preferredDisplayModeId = best.getModeId();
        lp.preferredRefreshRate = want;
        getWindow().setAttributes(lp);
        refreshNote = "frame rate " + fps + ": asked the display for " + Math.round(want) + " Hz (" + (best != null ? "mode " + best.getModeId() : "no such mode") + "; now "
            + Math.round(cur.getRefreshRate()) + " Hz; modes:" + all + ")";
    }

    // The gear's frame rate was saved while the game runs: the display is asked for the new rate and the native side makes its cap again, at once
    // (the UI thread: GearSettings.Section.after). The display may take a moment to change its mode: its rate is reported again a little later
    void liveFps() {
        int fps = gamePrefs().getInt(F2Gear.P_FPS, 60);
        applyRefresh(); crumb(refreshNote);
        if (libraryLoaded) try { nativeSetFps(fps); } catch (UnsatisfiedLinkError e) { /* an older library */ }
        reportHz();
        if (layer != null) { layer.postDelayed(this::reportHz, 700); layer.postDelayed(this::reportHz, 2000); }
    }

    // the rate the display runs at now goes to the native side, which makes the frame cap from it (f2_screen.c); said in the log when it changes
    private float lastHz;
    private void reportHz() {
        float hz = getWindowManager().getDefaultDisplay().getRefreshRate();
        if (libraryLoaded) try { nativeDisplayHz(hz); } catch (UnsatisfiedLinkError e) { /* an older library */ }
        if (Math.abs(hz - lastHz) > 0.5f) { lastHz = hz; crumb("display runs at " + hz + " Hz"); }
        // a hint for the system's refresh-rate choice as well (Android 11+): the picture's own rate
        if (Build.VERSION.SDK_INT >= 30 && mUnityPlayer != null) {
            int fps = gamePrefs().getInt(F2Gear.P_FPS, 60);
            for (int i = 0; i < mUnityPlayer.getChildCount(); i++) {
                android.view.View v = mUnityPlayer.getChildAt(i);
                if (v instanceof android.view.SurfaceView) {
                    android.view.Surface sf = ((android.view.SurfaceView) v).getHolder().getSurface();
                    if (sf != null && sf.isValid()) try { sf.setFrameRate(fps == 30 ? 60 : fps, android.view.Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE); } catch (Exception e) { /* released meanwhile */ }
                }
            }
        }
    }

    // ---- storage access for Download/PillarsOfEternityMobile (logs AND, once granted, the data folder): measured on a real Android 16
    // phone (targetSdk 29, requestLegacyExternalStorage=true) that the manifest's declared READ/WRITE_EXTERNAL_STORAGE are NOT granted
    // by themselves -- "declared" is not "granted"; the legacy-storage compatibility behaviour some OS versions give a targetSdk<=29 app
    // does NOT cover this device. Same check and flow as doom-port's DmActivity.ensureFileAccess / SaveTransfer.allFilesAccess: API 30+
    // needs the special "All files access" grant (Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, no normal runtime dialog for
    // it); below that, the ordinary READ/WRITE_EXTERNAL_STORAGE runtime permissions (requestPermissions) are enough.
    //   Ordering problem specific to F2 (DOOM can gate native loading on this; F2 cannot): UnityPlayerActivity.onCreate (super.onCreate,
    //   called right after this) starts the engine immediately -- there is no "wait for the permission, THEN load" step like DOOM's
    //   startHost(). So: ask once per app version (never block), let this run proceed on whatever GameFolder.findDataRoot() finds right
    //   now (the Android/data fallback if access is refused -- same as if the folder were simply absent, nothing crashes), and if the
    //   player grants it after returning from Settings (or after answering a runtime dialog) while the engine had NOT found a complete
    //   folder yet, restart this activity once (recreate() would keep the already-running engine; a fresh Intent does not) so onCreate
    //   runs again from the top and this time finds the folder before writeFront()/super.onCreate().
    private static final String[] LEGACY_STORAGE = {android.Manifest.permission.READ_EXTERNAL_STORAGE, android.Manifest.permission.WRITE_EXTERNAL_STORAGE};
    private static final int REQ_ALL_FILES = 4714, REQ_LEGACY_STORAGE = 4715;
    static boolean allFilesAccess(Context c) {
        return Build.VERSION.SDK_INT >= 30 ? Environment.isExternalStorageManager()
            : c.checkSelfPermission(android.Manifest.permission.WRITE_EXTERNAL_STORAGE) == android.content.pm.PackageManager.PERMISSION_GRANTED;
    }
    private SharedPreferences internalPrefs() { return getSharedPreferences("f2_internal", MODE_PRIVATE); }
    private void ensureFileAccess() {
        if (allFilesAccess(this)) return;
        String ver; try { ver = getPackageManager().getPackageInfo(getPackageName(), 0).versionName; } catch (Exception e) { ver = "?"; }
        String askedKey = "file_access_asked_" + ver;
        if (internalPrefs().getBoolean(askedKey, false)) {
            android.util.Log.i(TAG, "[java] storage access for Download/" + DATA_DIR + " was declined earlier this version; using the app's own folder");
            return;
        }
        internalPrefs().edit().putBoolean(askedKey, true).apply();
        try {
            if (Build.VERSION.SDK_INT >= 30)
                startActivityForResult(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName())), REQ_ALL_FILES);
            else requestPermissions(LEGACY_STORAGE, REQ_LEGACY_STORAGE);
        } catch (Exception e) { android.util.Log.e(TAG, "storage access request: " + e); }
    }
    @Override public void onRequestPermissionsResult(int code, String[] perms, int[] results) {
        super.onRequestPermissionsResult(code, perms, results);
        if (code == REQ_LEGACY_STORAGE) restartIfNowAccessible();
    }
    @Override protected void onActivityResult(int code, int result, Intent data) {
        super.onActivityResult(code, result, data);
        if (code == REQ_ALL_FILES) restartIfNowAccessible();
    }
    // Only while the engine has not started (libraryLoaded false, i.e. this is still the SAME onCreate that called ensureFileAccess):
    // a grant that arrives after the engine is already running is for the NEXT launch (restarting a running Unity player mid-game would
    // lose the session; the folder is re-checked at every start anyway).
    private void restartIfNowAccessible() {
        if (libraryLoaded || !allFilesAccess(this)) return;
        android.util.Log.i(TAG, "[java] storage access granted -- restarting once so the data folder is found before the engine starts");
        Intent i = new Intent(this, DmActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(i); finishAndRemoveTask();
    }

    // The gear's next-launch choices for the native side (native/boot/f2_boot.c reads files/f2_front.txt after f2_config.txt): the picture's shape and the
    // frame rate. A choice never saved writes nothing (the native defaults: wide=1, the game's own frame rate)
    //   Piece B: data_dir / overlay_dir / astc_dir are native/boot/f2_boot.c's own recognised keys (read_config_file), already wired for
    //   the developer's f2_config.txt; writing them here from GameFolder.findDataRoot() is the ONLY change needed for the engine to read
    //   the player-copied Download/PillarsOfEternityMobile folder instead of the adb-pushed Android/data one -- no native code touched.
    //   Not found / incomplete: the keys are left out, so f2_paths.c falls through to its own candidates (own files dir, then the
    //   Android/data copy): today's developer flow keeps working unchanged.
    private void writeFront() {
        File ext = getExternalFilesDir(null); if (ext == null) return;
        SharedPreferences p = gamePrefs();
        StringBuilder s = new StringBuilder("# written by the Java front end (DmActivity) at each start: the player's choices in the gear\n");
        if (p.contains(F2Gear.P_WIDE)) s.append("wide=").append(p.getInt(F2Gear.P_WIDE, 0) == 0 ? 1 : 0).append('\n');
        s.append("fps=").append(p.getInt(F2Gear.P_FPS, 60)).append('\n');
        if (p.contains(F2Gear.P_TOUCH)) s.append("touch=").append(p.getBoolean(F2Gear.P_TOUCH, true) ? 1 : 0).append('\n');
        GameFolder.DataRoot dr = GameFolder.findDataRoot(this);
        if (dr.complete) {
            s.append("data_dir=").append(dr.data.getAbsolutePath()).append('\n');
            s.append("overlay_dir=").append(dr.overlay.getAbsolutePath()).append('\n');
            if (dr.astc != null) s.append("astc_dir=").append(dr.astc.getAbsolutePath()).append('\n');
            if (dr.game != null) s.append("game_image=").append(dr.game.getAbsolutePath()).append('\n');   // clean apk: native/loader/f2_loader.c
            // save_dir is deliberately NOT redirected to dr.save here: an existing player's save already lives under the app's own
            // storage (f2_cfg's default <files>/save), and silently pointing a running install at an empty folder would look like a
            // lost save. dr.save is where setup_f2.py creates an EMPTY save folder for a brand-new install; wiring it in is a one-line
            // follow-up (uncomment) once there is an explicit "import this save" choice in the gear, matching how the other ports gate it.
        }
        crumb("game data folder: " + dr.root.getAbsolutePath() + " (storage access " + (allFilesAccess(this) ? "granted" : "NOT granted -- Download is unreadable; grant it in the gear or Android Settings, then restart the app")
            + "): " + (dr.complete ? "using it (" + dr.why + ")" : dr.why + " -- falling back to the app's own storage"));
        File defres = extractDefRes();   // clean apk: Unity's own "unity default resources" ships as an asset, never needs a Unity install
        if (defres != null) s.append("defres_path=").append(defres.getAbsolutePath()).append('\n');
        try (OutputStream o = new FileOutputStream(new File(ext, "f2_front.txt"))) { o.write(s.toString().getBytes(StandardCharsets.UTF_8)); }
        catch (Exception e) { android.util.Log.e(TAG, "f2_front.txt: " + e); }
    }
    // assets/f2/unity_default_resources (tools/build_apk.py: a copy of the Unity Editor's own AndroidPlayer/Data/"unity default
    // resources", made at BUILD time by whoever has Unity installed -- never the player) -> <internal files>/f2_defres.bin, once
    // (skipped when a file of the same size is already there). native/boot/f2_paths.c serves it (defres_path=) only when neither
    // the overlay nor the data folder already has a copy (an overlay built the old way, PROGRESS_data.txt, still carries its own).
    private File extractDefRes() {
        File out = new File(getFilesDir(), "f2_defres.bin");
        android.content.res.AssetManager am = getAssets();   // the activity's shared instance -- never close it
        try {
            long assetLen;
            try (android.content.res.AssetFileDescriptor fd = am.openFd("f2/unity_default_resources")) { assetLen = fd.getLength(); }
            if (out.isFile() && out.length() == assetLen) return out;
            try (java.io.InputStream in = am.open("f2/unity_default_resources"); OutputStream o = new FileOutputStream(out)) {
                byte[] buf = new byte[1 << 16];
                for (int n; (n = in.read(buf)) > 0; ) o.write(buf, 0, n);
            }
            return out;
        } catch (Exception e) { android.util.Log.e(TAG, "assets/f2/unity_default_resources: " + e); return out.isFile() ? out : null; }
    }
    // The picture's shape. The game is asked to draw the window's own shape, whatever it is (native/boot/f2_screen.c widens or heightens its 16:9
    // request: 20:9 phones, 4:3 and 16:10 tablets, unfolded foldables), so normally the Unity view is the whole window. Only "16:9 with bars" in the
    // gear makes it a 16:9 box (the player's choice), and "am start ... --es box W:H" makes it a W:H box to try another screen's shape on this phone.
    // The box is the Unity player's view (so its surface); the pad's layer stays the whole window.
    private void fitWindow(boolean bars, String test) {
        if (mUnityPlayer == null) return;
        android.util.DisplayMetrics m = new android.util.DisplayMetrics(); getWindowManager().getDefaultDisplay().getRealMetrics(m);
        int h = Math.min(m.widthPixels, m.heightPixels), w = Math.max(m.widthPixels, m.heightPixels);
        long aw = 0, ah = 0;
        if (test != null && test.matches("\\d+:\\d+")) { String[] p = test.split(":"); aw = Long.parseLong(p[0]); ah = Long.parseLong(p[1]); }
        else if (bars && (long) w * 9 > (long) h * 16 + h / 50) { aw = 16; ah = 9; }
        if (aw <= 0 || ah <= 0) return;
        int bw = w, bh = h;
        if (w * ah > h * aw) bw = (int) (h * aw / ah); else bh = (int) (w * ah / aw);
        mUnityPlayer.setLayoutParams(new FrameLayout.LayoutParams(bw, bh, android.view.Gravity.CENTER));
        getWindow().getDecorView().setBackgroundColor(0xff000000);
        android.util.Log.i(TAG, "picture box " + bw + "x" + bh + " in the " + w + "x" + h + " screen");
    }

    // ---- FrontMenu.Host: the shared pause menu. The game is not held yet (it keeps running behind the menu): the pad is released, nothing else
    @Override public FrontMenu frontMenu() { return front; }
    @Override public String gameName() { return "Pillars of Eternity"; }
    @Override public boolean ready() { return started; }
    @Override public void hold(boolean on) { if (pad != null) pad.releaseAll(); if (touch != null) touch.reset(); ctlButtons = 0; send(); }
    @Override public void gameSettings() { new GearSettings(this).show(); }
    @Override public int modeKind() { return FrontMenu.MODE_NONE; }
    @Override public boolean docked() { return false; }
    @Override public boolean setDocked(boolean on) { return false; }
    @Override public FrontMenu.Saves saves() { return null; }
    @Override public void quit() {
        crumb("quit from the pause menu"); CrashLog.sessionEnd(this); CrashLog.mirrorBootLogs(this);   // System.exit skips onStop/onDestroy
        finishAndRemoveTask(); new android.os.Handler(getMainLooper()).postDelayed(() -> System.exit(0), 300);
    }
    @Override public String[] portRows() {
        boolean shown = padPrefs().getBoolean("shown", false);
        return new String[] {shown ? Ui.t(this, "Скрыть экранный геймпад", "Hide the on-screen gamepad") : Ui.t(this, "Показать экранный геймпад", "Show the on-screen gamepad")};
    }
    @Override public void portRow(int i) {
        if (i != 0) return;
        boolean on = !padPrefs().getBoolean("shown", false);
        padPrefs().edit().putBoolean("shown", on).apply(); pad.reloadSettings();
        crumb("on-screen gamepad " + (on ? "shown" : "hidden") + " (pause menu)");
        front.close();
    }
    @Override public void onBackPressed() { if (front != null && started) front.back(); else super.onBackPressed(); }

    // ---- GearHost: the shared gear; F2's sections are F2Gear
    private final float[] gearStats = new float[4];
    @Override public Activity activity() { return this; }
    @Override public String dataDirName() { return DATA_DIR; }
    @Override public FrontMenu.Pad pad() { return pad; }
    @Override public FrontMenu front() { return front; }
    @Override public boolean libraryLoaded() { return libraryLoaded; }
    @Override public boolean started() { return started; }
    @Override public VkDrivers drivers() { return null; }
    @Override public List<GearSettings.Section> gearSections(GearSettings g) { return F2Gear.sections(this, g); }
    @Override public float[] frameStats() { if (!libraryLoaded) return null; try { nativeFrameStats(gearStats); } catch (Throwable t) { gearStats[0] = 0; } return gearStats; }
    @Override public void crumb(String line) {
        android.util.Log.i(TAG, "[java] " + line);
        if (libraryLoaded) try { nativeLog(line); } catch (UnsatisfiedLinkError e) { /* an older library */ }
    }

    // ---- the pad's output (the calls every port's pad makes): keycodes of the pad device and stick axes -> the game's nn.hid handheld pad, together with
    // what a real controller holds
    private static int padButtons, ctlButtons; private static final float[] padAxes = new float[4], ctlAxes = new float[4];
    private static boolean modeTouch;
    private static void mode(boolean t) {
        if (modeTouch == t || !libraryLoaded) return;
        modeTouch = t;
        try { nativeTouchMode(t); } catch (UnsatisfiedLinkError e) { /* an older library */ }
    }
    private static int pulseButtons;
    // a console button pressed for a moment on the game's behalf (F2Touch: the two-finger tap = the console's pause button), without leaving touch mode
    static void pulse(int bit) {
        pulseButtons |= bit; send();
        new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(() -> { pulseButtons &= ~bit; send(); }, 150);
    }
    private static void send() {
        if (!libraryLoaded) return;
        if ((padButtons | ctlButtons) != 0 || Math.abs(padAxes[0] + ctlAxes[0]) > 0.3f || Math.abs(padAxes[1] + ctlAxes[1]) > 0.3f
            || Math.abs(padAxes[2] + ctlAxes[2]) > 0.3f || Math.abs(padAxes[3] + ctlAxes[3]) > 0.3f) mode(false);   // the pad / a controller: the console input
        float[] a = new float[4];
        for (int i = 0; i < 4; i++) a[i] = Math.max(-1, Math.min(1, padAxes[i] + ctlAxes[i]));
        try { nativeInput(padButtons | ctlButtons | pulseButtons, a[0], a[1], a[2], a[3]); } catch (UnsatisfiedLinkError e) { libraryLoaded = false; }
    }
    static boolean nativePadKey(int device, int keyCode, boolean down) {
        int bit = padBit(keyCode); if (bit == 0) return false;
        if (down) padButtons |= bit; else padButtons &= ~bit;
        send(); return true;
    }
    static void nativePadAxes(int device, float x, float y, float z, float rz, float lt, float rt, float hx, float hy) {
        padAxes[0] = x; padAxes[1] = y; padAxes[2] = z; padAxes[3] = rz; send();
    }
    static boolean nativeLookDelta(float dx, float dy, float k) { return false; }
    private static int padBit(int k) {   // the keycodes the pad sends (DOOM's front_android.c map), as PadTheme bits = nn::hid NpadButton bits
        switch (k) {
            case KeyEvent.KEYCODE_ENTER: return PadTheme.A; case KeyEvent.KEYCODE_ESCAPE: return PadTheme.B; case KeyEvent.KEYCODE_R: return PadTheme.X; case KeyEvent.KEYCODE_F: return PadTheme.Y;
            case KeyEvent.KEYCODE_Q: return PadTheme.L; case KeyEvent.KEYCODE_E: return PadTheme.R; case KeyEvent.KEYCODE_SHIFT_LEFT: return PadTheme.ZL; case KeyEvent.KEYCODE_CTRL_LEFT: return PadTheme.ZR;
            case KeyEvent.KEYCODE_TAB: return PadTheme.PLUS; case KeyEvent.KEYCODE_M: return PadTheme.MINUS; case KeyEvent.KEYCODE_BUTTON_THUMBL: return PadTheme.LS; case KeyEvent.KEYCODE_BUTTON_THUMBR: return PadTheme.RS;
            case KeyEvent.KEYCODE_DPAD_UP: return PadTheme.UP; case KeyEvent.KEYCODE_DPAD_DOWN: return PadTheme.DOWN; case KeyEvent.KEYCODE_DPAD_LEFT: return PadTheme.LEFT; case KeyEvent.KEYCODE_DPAD_RIGHT: return PadTheme.RIGHT;
            default: return 0;
        }
    }
    // a real controller's keys by place (Android's A is the bottom button = the Switch's B), as nx2android's default map
    private static int ctlBit(int k) {
        switch (k) {
            case KeyEvent.KEYCODE_BUTTON_A: return PadTheme.B; case KeyEvent.KEYCODE_BUTTON_B: return PadTheme.A; case KeyEvent.KEYCODE_BUTTON_X: return PadTheme.Y; case KeyEvent.KEYCODE_BUTTON_Y: return PadTheme.X;
            case KeyEvent.KEYCODE_BUTTON_L1: return PadTheme.L; case KeyEvent.KEYCODE_BUTTON_R1: return PadTheme.R; case KeyEvent.KEYCODE_BUTTON_L2: return PadTheme.ZL; case KeyEvent.KEYCODE_BUTTON_R2: return PadTheme.ZR;
            case KeyEvent.KEYCODE_BUTTON_START: return PadTheme.PLUS; case KeyEvent.KEYCODE_BUTTON_SELECT: return PadTheme.MINUS;
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return PadTheme.LS; case KeyEvent.KEYCODE_BUTTON_THUMBR: return PadTheme.RS;
            case KeyEvent.KEYCODE_DPAD_UP: return PadTheme.UP; case KeyEvent.KEYCODE_DPAD_DOWN: return PadTheme.DOWN; case KeyEvent.KEYCODE_DPAD_LEFT: return PadTheme.LEFT; case KeyEvent.KEYCODE_DPAD_RIGHT: return PadTheme.RIGHT;
            default: return 0;
        }
    }

    // ---- PoePadView.Game: the fingers the pad gives the game, as fractions of the window. They become ordinary touch events for the Unity player
    // (down / pointer down / move / pointer up / up, one pointer change per event as Android sends them)
    private final List<Integer> touchIds = new ArrayList<>();
    private final float[] touchX = new float[64], touchY = new float[64];
    private long touchDown;
    @Override public boolean gameTouchOn() { return started && !(front != null && front.isOpen()); }
    @Override public void gameTouch(int n, int[] ids, float[] xs, float[] ys) {
        if (mUnityPlayer == null) return;
        if (touchMouse) {   // the PC mouse path: gestures -> mouse and key events
            float lw = layer.getWidth(), lh = layer.getHeight(), lx = mUnityPlayer.getLeft(), ly = mUnityPlayer.getTop();
            float[] px = new float[n], py = new float[n];
            for (int i = 0; i < n; i++) { px[i] = xs[i] * lw - lx; py[i] = ys[i] * lh - ly; }
            if (n > 0) mode(true);
            touch.fingers(n, ids, px, py);
            return;
        }
        // fractions of the pad's layer (the whole window) -> pixels of the Unity player's view (smaller when the picture has bars)
        float w = layer.getWidth(), h = layer.getHeight(), ox = mUnityPlayer.getLeft(), oy = mUnityPlayer.getTop();
        long now = SystemClock.uptimeMillis();
        Set<Integer> live = new HashSet<>();
        for (int i = 0; i < n; i++) if (ids[i] >= 0 && ids[i] < 64) live.add(ids[i]);
        for (int k = touchIds.size() - 1; k >= 0; k--) {   // lifted fingers, one event each
            int id = touchIds.get(k); if (live.contains(id)) continue;
            inject(touchIds.size() == 1 ? MotionEvent.ACTION_UP : MotionEvent.ACTION_POINTER_UP | k << MotionEvent.ACTION_POINTER_INDEX_SHIFT, now);
            touchIds.remove(k);
        }
        boolean moved = false;
        for (int i = 0; i < n; i++) {
            int id = ids[i]; if (id < 0 || id >= 64) continue;
            float x = xs[i] * w - ox, y = ys[i] * h - oy;
            if (!touchIds.contains(id)) {
                touchX[id] = x; touchY[id] = y; touchIds.add(id);
                if (touchIds.size() == 1) touchDown = now;
                inject(touchIds.size() == 1 ? MotionEvent.ACTION_DOWN : MotionEvent.ACTION_POINTER_DOWN | (touchIds.size() - 1) << MotionEvent.ACTION_POINTER_INDEX_SHIFT, now);
            } else if (touchX[id] != x || touchY[id] != y) { touchX[id] = x; touchY[id] = y; moved = true; }
        }
        if (moved && !touchIds.isEmpty()) inject(MotionEvent.ACTION_MOVE, now);
    }
    private void inject(int action, long now) {
        int n = touchIds.size(); if (n == 0) return;
        MotionEvent.PointerProperties[] pp = new MotionEvent.PointerProperties[n]; MotionEvent.PointerCoords[] pc = new MotionEvent.PointerCoords[n];
        for (int i = 0; i < n; i++) {
            int id = touchIds.get(i);
            pp[i] = new MotionEvent.PointerProperties(); pp[i].id = id; pp[i].toolType = MotionEvent.TOOL_TYPE_FINGER;
            pc[i] = new MotionEvent.PointerCoords(); pc[i].x = touchX[id]; pc[i].y = touchY[id]; pc[i].pressure = 1; pc[i].size = 1;
        }
        MotionEvent e = MotionEvent.obtain(touchDown, now, action, n, pp, pc, 0, 0, 1, 1, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0);
        try { mUnityPlayer.injectEvent(e); } finally { e.recycle(); }
    }

    // ---- controllers: keys and axes of real gamepads go to the game's pad (or the menu); the on-screen pad steps aside once one sends input, a touch
    // brings it back
    private final Set<Integer> usedPads = new HashSet<>();
    private void padUsed(int id) { if (usedPads.add(id) || !pad.controllerMode()) pad.setControllerMode(true); }
    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        int k = e.getKeyCode(); boolean down = e.getAction() == KeyEvent.ACTION_DOWN;
        if (front != null && front.key(e)) return true;   // Back / Home / Menu, the menu's navigation
        if (started && FrontMenu.isController(e.getDevice(), e.getSource()) && (down || e.getAction() == KeyEvent.ACTION_UP)) {
            int bit = ctlBit(k);
            if (bit != 0) { if (down) ctlButtons |= bit; else ctlButtons &= ~bit; send(); padUsed(e.getDeviceId()); return true; }
        }
        return super.dispatchKeyEvent(e);
    }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent e) {
        if (front != null && front.motion(e)) return true;
        if (started && e.getAction() == MotionEvent.ACTION_MOVE && e.getDevice() != null && !e.getDevice().isVirtual() && (e.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) {
            ctlAxes[0] = dead(e.getAxisValue(MotionEvent.AXIS_X)); ctlAxes[1] = dead(e.getAxisValue(MotionEvent.AXIS_Y));
            ctlAxes[2] = dead(e.getAxisValue(MotionEvent.AXIS_Z)); ctlAxes[3] = dead(e.getAxisValue(MotionEvent.AXIS_RZ));
            float lt = Math.max(e.getAxisValue(MotionEvent.AXIS_LTRIGGER), e.getAxisValue(MotionEvent.AXIS_BRAKE)), rt = Math.max(e.getAxisValue(MotionEvent.AXIS_RTRIGGER), e.getAxisValue(MotionEvent.AXIS_GAS));
            float hx = e.getAxisValue(MotionEvent.AXIS_HAT_X), hy = e.getAxisValue(MotionEvent.AXIS_HAT_Y);
            int b = ctlButtons & ~(PadTheme.ZL | PadTheme.ZR | PadTheme.LEFT | PadTheme.RIGHT | PadTheme.UP | PadTheme.DOWN);
            if (lt > 0.5f) b |= PadTheme.ZL; if (rt > 0.5f) b |= PadTheme.ZR;
            if (hx < -0.5f) b |= PadTheme.LEFT; if (hx > 0.5f) b |= PadTheme.RIGHT; if (hy < -0.5f) b |= PadTheme.UP; if (hy > 0.5f) b |= PadTheme.DOWN;
            ctlButtons = b; send();
            if (Math.abs(ctlAxes[0]) > 0.5f || Math.abs(ctlAxes[1]) > 0.5f || Math.abs(ctlAxes[2]) > 0.5f || Math.abs(ctlAxes[3]) > 0.5f || hx != 0 || hy != 0) padUsed(e.getDeviceId());
            return true;
        }
        return super.dispatchGenericMotionEvent(e);
    }
    private static float dead(float v) { return Math.abs(v) < 0.12f ? 0 : v; }
    @Override public boolean dispatchTouchEvent(MotionEvent e) {   // a touch brings the pad back from controller mode
        if (pad != null && pad.controllerMode() && e.getActionMasked() == MotionEvent.ACTION_DOWN && !(front != null && front.isOpen())) pad.setControllerMode(false);
        return super.dispatchTouchEvent(e);
    }
    @Override public void onInputDeviceAdded(int id) {}
    @Override public void onInputDeviceChanged(int id) {}
    @Override public void onInputDeviceRemoved(int id) {
        usedPads.remove(id);
        if (usedPads.isEmpty()) { ctlButtons = 0; for (int i = 0; i < 4; i++) ctlAxes[i] = 0; send(); if (pad != null) pad.setControllerMode(false); }
    }

    // ---- lifecycle
    @Override protected void onResume() {
        super.onResume(); immersive();
        resumed = true; reportHz();
        CrashLog.session(this, "fg");
        if (layer != null) { layer.removeCallbacks(hzTick); layer.postDelayed(hzTick, 1500); }
    }
    // the display's rate is looked at every few seconds while the game is in front: the system changes it on its own (power saving, heat, touch boost)
    private boolean resumed;
    private final Runnable hzTick = new Runnable() { @Override public void run() { if (!resumed) return; reportHz(); if (layer != null) layer.postDelayed(this, 4000); } };
    @Override protected void onPause() {
        resumed = false; if (pad != null) pad.releaseAll(); if (touch != null) touch.reset(); ctlButtons = 0; send();
        crumb("activity paused"); CrashLog.session(this, "bg");
        mirrorLogsAsync();
        super.onPause();
    }
    @Override protected void onStop() { crumb("activity stopped"); mirrorLogsAsync(); super.onStop(); }
    // A locked phone never gets window focus for this activity (UnityPlayer only starts its engine with focus: UNP-08, PROGRESS.txt [21]),
    // so a run stuck at frames=0 with only initJni in the log is the lock screen, not a player bug -- say so plainly in the one log a
    // tester would send, not just in the owner's own notes.
    private boolean hadFocus = true;
    @Override public void onWindowFocusChanged(boolean f) {
        super.onWindowFocusChanged(f);
        if (f) immersive();
        if (f != hadFocus) { hadFocus = f; crumb("window focus " + (f ? "gained" : "lost") + (f ? "" : " (locked screen or another window on top: the Unity engine will not render/advance until focus returns)")); }
    }
    @Override protected void onDestroy() {
        if (inputs != null) inputs.unregisterInputDeviceListener(this);
        if (isFinishing()) { crumb("activity finishing"); CrashLog.sessionEnd(this); mirrorLogsAsync(); }
        if (current.get() == this) current.clear();
        super.onDestroy();
    }
    private void mirrorLogsAsync() {
        final Context app = getApplicationContext();
        new Thread(() -> CrashLog.mirrorBootLogs(app), "f2-logcopy").start();
    }

    // ---- crash reporting (CrashLog.java): a Java exception anywhere in this process is otherwise invisible to a tester who cannot
    // attach a debugger. Chained to whatever handler was there before (ART's own, which would otherwise print to logcat only and exit).
    private void installCrashHandler() {
        final Thread.UncaughtExceptionHandler prev = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((t, e) -> {
            try {
                CrashLog.writeNow(getApplicationContext(), "uncaught Java exception on thread \"" + t.getName() + "\": " + e, CrashLog.stackTrace(e));
                CrashLog.sessionEnd(getApplicationContext());
            } catch (Throwable ignore) { /* must not throw from here */ }
            if (prev != null) prev.uncaughtException(t, e); else { android.util.Log.e(TAG, "uncaught", e); System.exit(1); }
        });
    }

    // ---- GearHost.logsDir(): the folder a tester should zip and send (GearSettings / any future dialog that names it)
    @Override public File logsDir() { return CrashLog.dir(this); }
}
