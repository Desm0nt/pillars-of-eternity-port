package org.pillars.port;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.os.Build;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.TimeUnit;

// F2's twin of doom-port's CrashLog.java (BUGS_AND_FIXES 2.22/2.56), trimmed to what F2 has: a tester can send one folder
// (GameFolder.writable(c, "logs"), i.e. Download/PillarsOfEternityMobile/logs, falling back to the app's own files when that is not
// writable) and the owner can diagnose a failure from it alone, the way doom-port / diablo3-port testers already do.
//   boot.log / boot.prev.log: COPIES of the native log F2 already keeps at <external files>/f2_log.txt (+ f2_log.prev.txt, the previous
//     run, renamed by native/boot/f2_boot.c at every boot). The originals are untouched and keep their own names: F2's own scripts
//     (tools/phone_*.sh, tools/alloc_sites.py, PROGRESS*.txt) read files/f2_log.txt and must keep working.
//   crash.txt / crash.prev.txt: a header (app build, device, Android, memory; the GPU name / Vulkan driver and the textureCompressionBC /
//     ASTC features are already the first lines native/boot/f2_vkwrap.c and f2_texset.c log at boot, so they are in the "head" excerpt of
//     the native log included right below the header) + why + the native log's head/tail + the logcat of this pid.
//   logcat.txt / logcat.prev.txt: a background `logcat` child mirrors this process's own lines (Java, native, the driver, the crash
//     buffer) from the first moment of the run, so a silent kill (OOM killer, a driver abort outside our signal handler) still leaves a
//     file with everything up to that moment.
//   sources of a report: (1) native/boot/f2_crash.c's signal handler, which besides its full report in f2_log.txt now also drops a short
//     async-signal-safe marker (f2_native_crash.txt, in the internal files dir) that survives the process dying; (2) a Java uncaught
//     exception (DmActivity installs the handler); (3) ApplicationExitInfo (Android 11+) read at the next start when the session marker
//     (f2_session.txt) says the previous run did not call sessionEnd() -- a kill with no signal and no Java exception (low memory, ANR,
//     "excessive resource usage", a background SIGKILL while the game was in front).
// NOT ported from doom-port (not applicable to F2 yet): per-driver boot-<driver>.log (F2 has one Vulkan driver, the system one: no driver
// manager / DriverManager choice exists here, see PROGRESS.txt and unity-native-player.md) and the SafeMode marker wipe (F2 does not use
// frontend-common's SafeMode -- GearHost.safeMode() stays null -- so there is nothing of its to wipe).
final class CrashLog {
    static final String SESSION = "f2_session.txt", NATIVE_MARK = "f2_native_crash.txt";
    static final int HEAD_BYTES = 48 << 10, TAIL_BYTES = 512 << 10, LOGCAT_LINES = 4000;
    static final long MIRROR_MAX = 8L << 20;

    private CrashLog() {}

    // Download/PillarsOfEternityMobile/logs when writable (GameFolder), else <external files>/logs, else <files>/logs.
    private static boolean loggedFallback;
    static File dir(Context c) {
        File pub = GameFolder.writable(c, "logs");
        if (pub != null) return pub;
        if (!loggedFallback) {   // once per process: dir() is called often (every mirror/copy)
            loggedFallback = true;
            android.util.Log.i(DmActivity.TAG, "[java] Download/" + GameFolder.NAME + "/logs is not writable (storage access "
                + (DmActivity.allFilesAccess(c) ? "granted, but the folder itself is not writable" : "NOT granted") + ") -- logs go to the app's own storage instead");
        }
        File ext = c.getExternalFilesDir(null);
        File d = ext != null ? new File(ext, "logs") : new File(c.getFilesDir(), "logs");
        if (!d.isDirectory() && !d.mkdirs()) d = new File(c.getFilesDir(), "logs");
        d.mkdirs();
        return d;
    }
    static File report(Context c) { return new File(dir(c), "crash.txt"); }

    // The native log F2 already writes (native/boot/f2_boot.c: f2_log_open, renamed from the previous run at every boot).
    static File bootLog(Context c) { File ext = c.getExternalFilesDir(null); return new File(ext, "f2_log.txt"); }
    static File bootPrevLog(Context c) { File ext = c.getExternalFilesDir(null); return new File(ext, "f2_log.prev.txt"); }

    // ---- live logcat of this process, from the first moment of the run (started once, from DmActivity.onCreate)
    private static Process logcat;
    static synchronized void startLogcatCapture(Context c) {
        if (logcat != null) return;
        try {
            File d = dir(c), now = new File(d, "logcat.txt"), prev = new File(d, "logcat.prev.txt");
            if (now.isFile()) { prev.delete(); now.renameTo(prev); }
            new File(d, "logcat.txt.1").delete();
            logcat = new ProcessBuilder("logcat", "-v", "threadtime", "-b", "main,system,crash", "-f", now.getAbsolutePath(), "-r", "8192", "-n", "1")
                .redirectErrorStream(true).start();
            android.util.Log.i(DmActivity.TAG, "[diag] logcat capture to " + now + " (pid " + android.os.Process.myPid() + ")");
        } catch (Exception e) { android.util.Log.w(DmActivity.TAG, "[diag] logcat capture failed: " + e); }
    }

    // ---- boot.log / boot.prev.log next to crash.txt, so a tester can zip just dir(c) and send it. Capped at MIRROR_MAX (head + tail;
    // F2's own files/f2_log.txt stay the full, uncapped originals). Called when the activity backgrounds, stops or is destroyed.
    static synchronized void mirrorBootLogs(Context c) {
        copyLog(bootLog(c), new File(dir(c), "boot.log"));
        copyLog(bootPrevLog(c), new File(dir(c), "boot.prev.log"));
    }
    private static void copyLog(File from, File to) {
        if (!from.isFile()) return;
        try (RandomAccessFile in = new RandomAccessFile(from, "r")) {
            try (OutputStream out = new FileOutputStream(to)) {
                long n = in.length();
                if (n > MIRROR_MAX) {
                    byte[] head = new byte[HEAD_BYTES]; in.readFully(head); out.write(head);
                    out.write(("\n---- [" + (n - HEAD_BYTES - MIRROR_MAX) + " bytes left out] ----\n").getBytes(StandardCharsets.UTF_8));
                    in.seek(n - MIRROR_MAX);
                }
                byte[] buf = new byte[1 << 16];
                for (int k; (k = in.read(buf)) > 0; ) out.write(buf, 0, k);
            }
        } catch (IOException e) { android.util.Log.w(DmActivity.TAG, "crash log: copy " + from + " -> " + to + ": " + e); }
    }

    // ---- the session marker: the game is running (pid, state, time); removed on a clean end (DmActivity: onCreate "boot", onStart/onResume
    // "fg", onStop "bg", sessionEnd() in onDestroy()/quit() when the activity is really finishing).
    static void session(Context c, String state) {
        try (OutputStream os = new FileOutputStream(new File(c.getFilesDir(), SESSION))) {
            os.write(("pid=" + android.os.Process.myPid() + "\nstate=" + state + "\ntime=" + System.currentTimeMillis() + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) { android.util.Log.w(DmActivity.TAG, "crash log: session marker: " + e); }
    }
    static void sessionEnd(Context c) { new File(c.getFilesDir(), SESSION).delete(); }

    // ---- a report from what is known right now (a Java exception): synchronous (a few hundred ms: logcat -d).
    static File writeNow(Context c, String reason, String details) {
        File log = bootLog(c);
        return write(c, reason, details, log, snapshot(log), android.os.Process.myPid(), null);
    }

    // ---- at the start: did the last run end badly? Returns a one-line summary for the boot log, or null when nothing is reported.
    // The report itself is written on a background thread (logcat -d of the LAST run's pid takes a moment); done(report, why) runs when it
    // is on disk (report may be null if writing failed).
    interface Done { void written(File report, String why); }
    static String checkPrevious(Context c, Done done) {
        File marker = new File(c.getFilesDir(), NATIVE_MARK), sess = new File(c.getFilesDir(), SESSION);
        String mark = readSmall(marker), session = readSmall(sess);
        marker.delete(); sess.delete();
        if (mark == null && session == null) return null;
        int pid = 0; String state = "?"; long started = 0;
        if (session != null) for (String l : session.split("\n")) {
            try {
                if (l.startsWith("pid=")) pid = Integer.parseInt(l.substring(4).trim());
                else if (l.startsWith("state=")) state = l.substring(6).trim();
                else if (l.startsWith("time=")) started = Long.parseLong(l.substring(5).trim());
            } catch (NumberFormatException e) { /* keep the defaults */ }
        }
        if (mark == null && pid == android.os.Process.myPid()) return null;   // this very process (the activity was re-created): nothing ended
        String exit = null; int reason = -1, status = 0;
        if (Build.VERSION.SDK_INT >= 30) {
            try {
                ActivityManager am = c.getSystemService(ActivityManager.class);
                List<ApplicationExitInfo> l = am == null ? null : am.getHistoricalProcessExitReasons(c.getPackageName(), pid, 1);
                if (l != null && !l.isEmpty() && (pid == 0 || l.get(0).getPid() == pid)) {
                    ApplicationExitInfo x = l.get(0); reason = x.getReason(); status = x.getStatus();
                    exit = "exit reason " + exitName(reason) + " (" + reason + "), status " + status + ", importance " + x.getImportance()
                        + ", pss " + (x.getPss() >> 10) + " MB, rss " + (x.getRss() >> 10) + " MB" + (x.getDescription() != null ? ", \"" + x.getDescription() + "\"" : "");
                }
            } catch (RuntimeException e) { exit = "exit reason unavailable: " + e; }
        }
        boolean front = !"bg".equals(state);
        String why;
        if (mark != null) why = "native crash (" + mark.trim().replace('\n', ' ') + ")";
        else if (reason >= 0) {
            boolean bad = reason == ApplicationExitInfo.REASON_CRASH || reason == ApplicationExitInfo.REASON_CRASH_NATIVE || reason == ApplicationExitInfo.REASON_ANR
                || reason == ApplicationExitInfo.REASON_INITIALIZATION_FAILURE || reason == ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE
                || reason == ApplicationExitInfo.REASON_DEPENDENCY_DIED
                || (front && (reason == ApplicationExitInfo.REASON_SIGNALED || reason == ApplicationExitInfo.REASON_LOW_MEMORY || reason == ApplicationExitInfo.REASON_UNKNOWN
                    || (reason == ApplicationExitInfo.REASON_EXIT_SELF && status != 0)));
            if (!bad) return "last run (pid " + pid + ", " + state + ") ended normally: " + exit;
            why = "the app was ended while the game was " + (front ? "in front" : "in the background") + ": " + exit;
        } else {
            String none = Build.VERSION.SDK_INT >= 30 ? "Android has no exit record for pid " + pid : "no exit reason below Android 11";
            if (!front) return "last run (pid " + pid + ") ended in the background (" + none + "), not reported";
            why = "the app ended while the game was in front (state " + state + "; " + (exit != null ? exit : none) + ")";
        }
        if (started > 0) why += "; that run started " + stamp(started);
        final String reasonText = why;
        final File log = bootLog(c);
        final byte[][] snap = snapshot(log);   // now: f2_boot.c already moved f2_log.txt to f2_log.prev.txt for the NEW run
        final int fpid = pid; final String exitText = exit;
        final Context app = c.getApplicationContext();
        new Thread(() -> {
            File f = write(app, reasonText, null, log, snap, fpid, exitText);
            if (done != null) done.written(f, reasonText);
        }, "f2-crashlog").start();
        return "last run: " + why;
    }

    // ---- writing
    private static File write(Context c, String reason, String details, File log, byte[][] snap, int pid, String exit) {
        File f = report(c), prev = new File(f.getParentFile(), "crash.prev.txt");
        StringBuilder sb = new StringBuilder(1 << 20);
        sb.append("Pillars of Eternity Mobile (F2) crash report\n").append("reason: ").append(reason).append('\n');
        if (exit != null && !reason.contains(exit)) sb.append("exit: ").append(exit).append('\n');
        sb.append("written: ").append(stamp(System.currentTimeMillis())).append('\n');
        header(c, sb);
        if (details != null) sb.append("\n---- details ----\n").append(details).append('\n');
        sb.append("\n---- native log ").append(log.getAbsolutePath()).append(" (").append(log.length()).append(" bytes); GPU name / Vulkan driver / ")
          .append("textureCompressionBC / ASTC features and the chosen texture set are near the top ----\n");
        if (snap == null) sb.append("(no log)\n");
        else {
            sb.append(new String(snap[0], StandardCharsets.UTF_8));
            if (snap[1] != null) sb.append("\n...\n---- native log, last ").append(snap[1].length >> 10).append(" KB ----\n").append(new String(snap[1], StandardCharsets.UTF_8));
        }
        sb.append("\n---- logcat of the process").append(pid > 0 ? " " + pid : "").append(" (main + crash buffers, last ").append(LOGCAT_LINES).append(" lines) ----\n");
        sb.append(logcat(pid));
        try {
            if (f.isFile()) { prev.delete(); if (!f.renameTo(prev)) f.delete(); }
            try (OutputStream os = new FileOutputStream(f)) { os.write(sb.toString().getBytes(StandardCharsets.UTF_8)); }
            try { android.media.MediaScannerConnection.scanFile(c, new String[] {f.getAbsolutePath()}, null, null); } catch (RuntimeException e) { /* only helps MTP file views */ }
            android.util.Log.w(DmActivity.TAG, "crash log: " + reason + " -> " + f.getAbsolutePath());
            return f;
        } catch (IOException | RuntimeException e) { android.util.Log.e(DmActivity.TAG, "crash log: cannot write " + f + ": " + e); return null; }
    }

    static void header(Context c, StringBuilder sb) {
        String ver = "?";
        try { android.content.pm.PackageInfo pi = c.getPackageManager().getPackageInfo(c.getPackageName(), 0); ver = pi.versionName + " (" + pi.getLongVersionCode() + ")"; }
        catch (Exception e) { /* keep ? */ }
        sb.append("app: ").append(c.getPackageName()).append(' ').append(ver).append('\n');
        sb.append("device: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL).append(" (").append(Build.DEVICE).append(", ").append(Build.PRODUCT).append(")\n");
        sb.append("SoC: ").append(Build.VERSION.SDK_INT >= 31 ? Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL + ", " : "").append("hardware ").append(Build.HARDWARE).append(", board ").append(Build.BOARD).append('\n');
        sb.append("Android: ").append(Build.VERSION.RELEASE).append(" (SDK ").append(Build.VERSION.SDK_INT).append("), build ").append(Build.DISPLAY).append('\n');
        sb.append("ABI: ").append(Build.SUPPORTED_ABIS.length > 0 ? Build.SUPPORTED_ABIS[0] : "?").append('\n');
        try {
            android.content.SharedPreferences p = c.getSharedPreferences("doom_graphics", Context.MODE_PRIVATE);
            sb.append("chosen settings (gear): fps=").append(p.getInt(F2Gear.P_FPS, 60)).append(" wide=").append(p.getInt(F2Gear.P_WIDE, 0))
              .append(" touch_mouse=").append(p.getBoolean(F2Gear.P_TOUCH, true)).append(" hold_ms=").append(p.getInt(F2Gear.P_HOLD, 500)).append('\n');
        } catch (Exception e) { /* gear prefs not readable: skip the line */ }
        ActivityManager am = c.getSystemService(ActivityManager.class);
        if (am != null) { ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo(); am.getMemoryInfo(mi);
            sb.append("memory: ").append(mi.totalMem >> 20).append(" MB, available now ").append(mi.availMem >> 20).append(" MB").append(mi.lowMemory ? " (low)" : "").append('\n'); }
    }

    // The log's head and tail (null tail when the whole file already fits).
    static byte[][] snapshot(File log) {
        if (log == null || !log.isFile()) return null;
        try (RandomAccessFile r = new RandomAccessFile(log, "r")) {
            long n = r.length();
            if (n <= HEAD_BYTES + TAIL_BYTES) { byte[] all = new byte[(int) n]; r.readFully(all); return new byte[][] {all, null}; }
            byte[] head = new byte[HEAD_BYTES], tail = new byte[TAIL_BYTES];
            r.readFully(head); r.seek(n - TAIL_BYTES); r.readFully(tail);
            return new byte[][] {head, tail};
        } catch (IOException e) { return null; }
    }

    // logcat -d of one process (pid 0: everything this app may read, i.e. its own lines: an app can read its own uid's lines without
    // any permission).
    static String logcat(int pid) {
        List<String> cmd = new ArrayList<>();
        cmd.add("logcat"); cmd.add("-d"); cmd.add("-v"); cmd.add("threadtime"); cmd.add("-b"); cmd.add("main"); cmd.add("-b"); cmd.add("crash");
        cmd.add("-t"); cmd.add(String.valueOf(LOGCAT_LINES));
        if (pid > 0) cmd.add("--pid=" + pid);
        Process p = null;
        try {
            p = new ProcessBuilder(cmd).redirectErrorStream(true).start();
            ByteArrayOutputStream b = new ByteArrayOutputStream(); byte[] buf = new byte[1 << 16];
            try (InputStream in = p.getInputStream()) { for (int k; (k = in.read(buf)) > 0 && b.size() < (4 << 20); ) b.write(buf, 0, k); }
            p.waitFor(3, TimeUnit.SECONDS);
            String s = b.toString("UTF-8");
            return s.isEmpty() ? "(no lines of this process left in the log buffer)\n" : s;
        } catch (IOException | InterruptedException e) { return "(logcat failed: " + e + ")\n"; }
        finally { if (p != null) p.destroy(); }
    }

    private static String exitName(int r) {
        switch (r) {
            case 1: return "EXIT_SELF"; case 2: return "SIGNALED"; case 3: return "LOW_MEMORY"; case 4: return "CRASH"; case 5: return "CRASH_NATIVE";
            case 6: return "ANR"; case 7: return "INITIALIZATION_FAILURE"; case 8: return "PERMISSION_CHANGE"; case 9: return "EXCESSIVE_RESOURCE_USAGE";
            case 10: return "USER_REQUESTED"; case 11: return "USER_STOPPED"; case 12: return "DEPENDENCY_DIED"; case 13: return "OTHER";
            case 14: return "FREEZER"; case 15: return "PACKAGE_STATE_CHANGE"; case 16: return "PACKAGE_UPDATED"; default: return "UNKNOWN";
        }
    }
    static String stamp(long ms) { return new SimpleDateFormat("yyyy-MM-dd HH:mm:ss Z", Locale.ROOT).format(new Date(ms)); }
    static String readSmall(File f) {
        if (!f.isFile()) return null;
        try (InputStream in = new java.io.FileInputStream(f)) {
            ByteArrayOutputStream b = new ByteArrayOutputStream(); byte[] buf = new byte[4096];
            for (int k; (k = in.read(buf)) > 0; ) b.write(buf, 0, k);
            return b.toString("UTF-8");
        } catch (IOException e) { return ""; }
    }
    static String stackTrace(Throwable t) {
        java.io.StringWriter w = new java.io.StringWriter(); t.printStackTrace(new java.io.PrintWriter(w)); return w.toString();
    }
}
