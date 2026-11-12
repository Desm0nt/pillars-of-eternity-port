// The launcher's entry: a plain Activity in front of the Unity one (DmActivity).
// The Unity player loads its libraries and starts looking for the game data inside DmActivity.onCreate, so by then the data folder has to be
// readable: asking for storage access from there put the system's settings page over a game that was already starting without its data
// (first pass of the setup work, 12.11.2026). So the question is asked HERE, before any engine code runs:
//   - the data is found (the setup folder Download/PillarsOfEternityMobile, or the developer's adb-pushed copy in the app's own folder) and
//     the game image exists (in the apk of a development build, or game/libf2game.so of the setup folder) -> DmActivity at once, nothing shown;
//   - otherwise a short page: what is missing, a button to allow file access (the same grant as the other ports: "all files access" on
//     Android 11+, the storage permission below), a button to look again. It looks again by itself whenever it comes back to the front.
// Tests over adb may keep starting DmActivity directly (it stays exported); this page is only the icon's way in.
package org.pillars.port;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;

public final class StartActivity extends Activity {
    private static final String TAG = "F2";
    private TextView status;
    private Button allow;
    private boolean launched;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        int pad = (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, 24, getResources().getDisplayMetrics());
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL); box.setPadding(pad, pad, pad, pad); box.setGravity(Gravity.CENTER_HORIZONTAL);
        TextView title = new TextView(this);
        title.setText("Pillars of Eternity"); title.setTextColor(Color.WHITE); title.setTextSize(22);
        box.addView(title);
        status = new TextView(this);
        status.setTextColor(0xffd0d0d0); status.setTextSize(15); status.setPadding(0, pad / 2, 0, pad / 2);
        box.addView(status);
        allow = new Button(this);
        allow.setText(Ui.t(this, "Разрешить доступ к файлам", "Allow file access"));
        allow.setOnClickListener(v -> askAccess());
        box.addView(allow, new LinearLayout.LayoutParams(-2, -2));
        Button again = new Button(this);
        again.setText(Ui.t(this, "Проверить снова", "Check again"));
        again.setOnClickListener(v -> check());
        box.addView(again, new LinearLayout.LayoutParams(-2, -2));
        ScrollView sv = new ScrollView(this);
        sv.setBackgroundColor(0xff101418); sv.setFillViewport(true); sv.addView(box);
        sv.setVisibility(View.INVISIBLE);               // nothing flashes when the game can simply start
        setContentView(sv);
    }
    @Override protected void onResume() { super.onResume(); check(); }
    @Override public void onRequestPermissionsResult(int code, String[] perms, int[] results) { super.onRequestPermissionsResult(code, perms, results); check(); }

    private void askAccess() {
        try {
            if (Build.VERSION.SDK_INT >= 30) {
                try { startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName()))); }
                catch (Exception e) { startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION)); }
            } else requestPermissions(new String[] {android.Manifest.permission.READ_EXTERNAL_STORAGE, android.Manifest.permission.WRITE_EXTERNAL_STORAGE}, 1);
        } catch (Exception e) { android.util.Log.e(TAG, "[java] start page: storage access request: " + e); }
    }

    private void check() {
        if (launched) return;
        boolean access = DmActivity.allFilesAccess(this);
        GameFolder.DataRoot dr = GameFolder.findDataRoot(this);
        File own = getExternalFilesDir(null);
        boolean devData = own != null && new File(own, "RomFS/Data/globalgamemanagers").isFile();
        boolean dataOk = dr.complete || devData;
        boolean imageOk = new File(getApplicationInfo().nativeLibraryDir, "libf2game.so").isFile()       // a development apk carries it
            || (dr.complete && dr.game != null) || (own != null && new File(own, "libf2game.so").isFile());
        android.util.Log.i(TAG, "[java] start page: file access " + access + ", setup folder: " + dr.why + ", own-folder data " + devData + ", game image " + imageOk);
        if (dataOk && imageOk) {
            launched = true;
            Intent i = new Intent(this, DmActivity.class);
            if (getIntent() != null && getIntent().getExtras() != null) i.putExtras(getIntent().getExtras());
            startActivity(i); finish(); overridePendingTransition(0, 0);
            return;
        }
        StringBuilder s = new StringBuilder();
        String folder = "Download/" + GameFolder.NAME;
        if (!access) {
            s.append(Ui.t(this, "Игре нужен доступ к файлам: её данные лежат в папке " + folder + ", туда же пишутся логи.",
                "The game needs file access: its data is in the folder " + folder + ", and the logs are written there too."));
        } else if (!dataOk) {
            s.append(Ui.t(this, "Данные игры не найдены.\nСкопируйте на телефон в Download папку " + GameFolder.NAME + ", которую сделал setup на компьютере.",
                "The game data was not found.\nCopy the folder " + GameFolder.NAME + " made by the PC setup into Download on this phone."));
            s.append("\n\n").append(dr.why);
        } else {
            s.append(Ui.t(this, "В папке " + folder + " нет файла game/libf2game.so. Скопируйте папку от setup целиком.",
                "The folder " + folder + " has no game/libf2game.so. Copy the whole folder made by the setup."));
        }
        status.setText(s);
        allow.setVisibility(access ? View.GONE : View.VISIBLE);
        ((View) status.getParent().getParent()).setVisibility(View.VISIBLE);
    }
}
