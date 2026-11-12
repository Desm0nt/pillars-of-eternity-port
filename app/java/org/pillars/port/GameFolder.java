package org.pillars.port;

import android.content.Context;
import android.os.Environment;
import java.io.File;
import java.io.FileOutputStream;

// F2's own trimmed twin of DOOM's GameFolder.java (doom-port/src/dm/android/.../GameFolder.java, issue #11 / BUGS_AND_FIXES 2.28): a
// folder under Download that a file manager can reach without root, so a tester's logs (and, once Piece B lands, the game data this app
// reads) sit next to each other the same way they do in every other port.
//   Storage access: measured WRONG in an earlier version of this comment -- declaring READ/WRITE_EXTERNAL_STORAGE +
//   requestLegacyExternalStorage in the manifest is NOT enough on a real Android 11+ phone (confirmed on Android 16, targetSdk 29:
//   both permissions come back granted=false with no requestPermissions call anywhere). DmActivity.ensureFileAccess() now asks for
//   real: Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION on API 30+ (Environment.isExternalStorageManager(), doom-port's own
//   SaveTransfer.allFilesAccess), requestPermissions(READ/WRITE_EXTERNAL_STORAGE) below that. Without the grant every method below
//   simply returns null/incomplete (silent on purpose -- the caller decides what that means).
//   NOT done here (left for the Piece B setup-folder work, which needs the fuller multi-volume search DOOM's version has): looking at SD
//   cards / USB volumes.
final class GameFolder {
    static final String NAME = DmActivity.DATA_DIR;   // "PillarsOfEternityMobile" (DmActivity.DATA_DIR): Download/PillarsOfEternityMobile

    private GameFolder() {}

    // Download/<NAME>/<sub> (sub may be null/"" for the folder itself) if it exists or can be created AND a file can actually be written
    // there now (a probe file, removed at once) -- not just "the call did not throw", since scoped-storage refusals on some OEM skins are
    // silent. Returns null on any failure; callers fall back to the app's own external/internal files dir.
    static File writable(Context c, String sub) {
        try {
            File base = new File(Environment.getExternalStorageDirectory(), "Download/" + NAME);
            File dir = (sub == null || sub.isEmpty()) ? base : new File(base, sub);
            if (!dir.isDirectory() && !dir.mkdirs() && !dir.isDirectory()) return null;
            File probe = new File(dir, ".f2_write_test");
            try (FileOutputStream os = new FileOutputStream(probe)) { os.write(0); }
            probe.delete();
            return dir;
        } catch (Exception e) {
            return null;
        }
    }

    // ---- Piece B: the folder the setup script (poe-port/tools/setup_f2.py) lays out, the same Download/<NAME> root the logs already
    // use. Read-only (no probe write: the game Data tree is >= 15 GB and must never be touched by this app), so this works even when the
    // folder came from an SD card / USB drive mounted read-only. Layout, matching setup_f2.py and native/boot/f2_paths.c's own naming:
    //   Download/PillarsOfEternityMobile/RomFS/Data        (required: the 15 GB loose Switch Data tree)
    //   Download/PillarsOfEternityMobile/overlay/Data       (required: shaders in Unity's Vulkan container + edited globalgamemanagers)
    //   Download/PillarsOfEternityMobile/overlay-astc/Data  (optional: ASTC texture set for GPUs without BC)
    //   Download/PillarsOfEternityMobile/game/libf2game.so  (optional: the game image, only when the apk is the clean build)
    //   Download/PillarsOfEternityMobile/save               (the save; same tree nn.fs already writes under Android/data today)
    // "Complete" = RomFS/Data has the two files every build needs (globalgamemanagers, the first scene) AND overlay/Data is not empty --
    // a folder with only RomFS/Data (the player stopped the copy early, or only the data step of setup finished) is reported as
    // incomplete rather than silently falling back, so the boot log says exactly what is missing. game/libf2game.so is independent of
    // "complete": a dev-full apk (tools/build_apk.py F2_APK_FULL=1) never needs it, so its absence is not an error.
    static final class DataRoot {
        final File root, data, overlay, astc, game, save;
        final boolean complete; final String why;
        DataRoot(File root, File data, File overlay, File astc, File game, File save, boolean complete, String why) {
            this.root = root; this.data = data; this.overlay = overlay; this.astc = astc; this.game = game; this.save = save; this.complete = complete; this.why = why;
        }
    }
    static DataRoot findDataRoot(Context c) {
        File root = new File(Environment.getExternalStorageDirectory(), "Download/" + NAME);
        File data = new File(root, "RomFS/Data"), overlay = new File(root, "overlay/Data"), astc = new File(root, "overlay-astc/Data");
        File game = new File(root, "game/libf2game.so"), save = new File(root, "save");
        if (!root.isDirectory()) return new DataRoot(root, data, overlay, astc, null, save, false, "no " + root.getAbsolutePath() + " (run the PC setup script, or keep using the adb-pushed Android/data copy)");
        boolean dataOk = new File(data, "globalgamemanagers").isFile() && new File(data, "level0").isFile();
        boolean overlayOk = overlay.isDirectory() && overlay.list() != null && overlay.list().length > 0;
        boolean astcOk = astc.isDirectory() && astc.list() != null && astc.list().length > 0;
        boolean gameOk = game.isFile();
        boolean complete = dataOk && overlayOk;
        String why = complete ? "RomFS/Data + overlay/Data present" + (astcOk ? " + overlay-astc/Data" : " (no ASTC texture set: BC only)") + (gameOk ? " + game/libf2game.so" : "")
            : "incomplete: " + (!dataOk ? "RomFS/Data missing globalgamemanagers/level0" : "") + (!dataOk && !overlayOk ? "; " : "") + (!overlayOk ? "overlay/Data missing or empty" : "");
        return new DataRoot(root, data, overlay, astcOk ? astc : null, gameOk ? game : null, save, complete, why);
    }
}
