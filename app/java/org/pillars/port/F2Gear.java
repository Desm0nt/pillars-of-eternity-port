// Pillars of Eternity F2: the gear (settings) dialog = the port's settings that mean something under the Unity player.
//   the controls: the shared sections the port shows (frontend-common GearPages: the on-screen pad with its opacity / size / layout editor / skins, the
//   stick dead zone, the gear button's look, the FPS counter, the menu language)
//   the page "Pillars of Eternity", the port's own switches re-made for F2 (next launch; DmActivity.writeFront -> files/f2_front.txt -> native/boot):
//     picture shape   pref aspect (0)    0 = the whole screen (wide=1: the game's 16:9 request is widened, f2_screen.c), 1 = 16:9 with bars (the window is 16:9)
//     frame rate      pref poe_fps (60)  fps=60 / 30
//   the game's language: files/f2_lang.txt, read by native/nn/f2_nn_misc.c at the start (DmActivity.writeGameLanguage)
// Not here: the port's nx2android switches without a meaning under the Unity player (Switch dock mode, Vulkan driver choice), and touch-as-the-mouse with its
// hold time, which F2 does not have yet.
package org.pillars.port;

import android.content.SharedPreferences;
import android.widget.LinearLayout;
import android.widget.RadioGroup;
import java.util.ArrayList;
import java.util.List;

final class F2Gear {
    private F2Gear() {}
    static final String P_WIDE = "aspect", P_FPS = "poe_fps", P_TOUCH = "poe_touch_mouse", P_HOLD = "poe_hold_ms";
    static final int[] HOLDS = {300, 400, 500, 700, 900};
    static final int[] FPS = {30, 60, 90, 120};
    // the codes the game knows (it reads the first two letters of what the "console" reports); "" = as it is now (f2_lang.txt untouched)
    static final String[] LANG_CODES = {"", "en", "ru", "de", "fr", "es", "it", "pl"};
    static final String[] LANG_NAMES = {null, "English", "Русский", "Deutsch", "Français", "Español", "Italiano", "Polski"};

    static List<GearSettings.Section> sections(DmActivity a, GearSettings g) {
        List<GearSettings.Section> s = new ArrayList<>();
        // the port own switches first, on the dialog itself (owner, 12.11.2026: behind a "Pillars of Eternity" button they were not found)
        s.add(fps(a)); s.add(picture()); s.add(touch()); s.add(hold()); s.add(storage(a));
        String[] l = LANG_NAMES.clone(); l[0] = g.t("Не менять", "Leave as it is");
        s.add(GearPages.gameLanguage(LANG_CODES, l, false));
        s.add(GearPages.fpsCounter());
        s.add(GearPages.gearButton());
        s.add(GearPages.padControls());
        s.add(GearPages.deadzone());
        s.add(GearPages.menuLanguage());
        return s;
    }

    // touch plays as the PC mouse (F2Touch + native/boot/f2_touch.c), or the stock console input with the on-screen gamepad
    static GearSettings.Section touch() {
        return new GearSettings.Section() {
            android.widget.CheckBox c;
            public void build(GearSettings s, LinearLayout box) {
                s.heading(box, s.when(s.t("Касания", "Touch"), false));
                c = s.check(box, s.t("Касания как мышь ПК (иначе — консольное управление геймпадом)", "Touch plays as the PC mouse (else the console's gamepad controls)"),
                    s.gamePrefs().getBoolean(P_TOUCH, true));
                s.note(box, s.t("Тап — левая кнопка; протянуть — перетаскивание; удержать — правая кнопка; два пальца — сдвиг камеры; щипок — зум; "
                    + "тап двумя пальцами — пауза; два пальца удержать — выбрать весь отряд; тап тремя — закрыть окно. Любая кнопка геймпада переключает игру на геймпад до следующего касания картинки.",
                    "Tap = left click; drag = dragging; hold = right click; two fingers = pan the camera; pinch = zoom; two-finger tap = pause; "
                    + "two fingers held = select the whole party; three-finger tap = close a window. Any gamepad button switches the game to the gamepad until the next touch on the picture."));
            }
            public void save(GearSettings s, SharedPreferences.Editor game, SharedPreferences.Editor pad) { game.putBoolean(P_TOUCH, c.isChecked()); }
        };
    }
    static GearSettings.Section hold() {
        return new GearSettings.Section() {
            RadioGroup r;
            public void build(GearSettings s, LinearLayout box) {
                s.heading(box, s.when(s.t("Удержание для правой кнопки, мс", "Hold for the right click, ms"), false));
                String[] l = new String[HOLDS.length]; for (int i = 0; i < l.length; i++) l[i] = String.valueOf(HOLDS[i]);
                r = s.radios(box, l, GearSettings.indexOf(HOLDS, s.gamePrefs().getInt(P_HOLD, 500), 2), true);
            }
            public void save(GearSettings s, SharedPreferences.Editor game, SharedPreferences.Editor pad) { game.putInt(P_HOLD, HOLDS[GearSettings.picked(r)]); }
        };
    }
    static GearSettings.Section picture() {
        return new GearSettings.Section() {
            RadioGroup r;
            public void build(GearSettings s, LinearLayout box) {
                s.heading(box, s.when(s.t("Формат картинки", "Picture shape"), false));
                r = s.radios(box, new String[] {s.t("Во весь экран (формат экрана)", "Full screen (the screen's aspect)"), s.t("16:9 с полями", "16:9 with bars")},
                    s.gamePrefs().getInt(P_WIDE, 0), false);
            }
            public void save(GearSettings s, SharedPreferences.Editor game, SharedPreferences.Editor pad) { game.putInt(P_WIDE, GearSettings.picked(r)); }
        };
    }
    // the storage grant for Download/PillarsOfEternityMobile (logs + the setup folder): DmActivity.ensureFileAccess() only asks
    // once per app version; this row is the retry the owner's reference ports offer after a decline (GameFolder.java's own
    // comment / DmActivity.ensureFileAccess).
    static GearSettings.Section storage(final DmActivity a) {
        return new GearSettings.Section() {
            public void build(GearSettings s, LinearLayout box) {
                boolean on = DmActivity.allFilesAccess(a);
                s.heading(box, s.when(s.t("Доступ к папке Download", "Access to the Download folder"), false));
                s.note(box, on
                    ? s.t("Разрешён: игра читает Download/PillarsOfEternityMobile (логи и, если он там есть, перенесённый PC-установщиком набор данных).",
                          "Granted: the game reads Download/PillarsOfEternityMobile (logs, and the PC setup folder if it is there).")
                    : s.t("Не разрешён: логи и папка с данными остаются во внутреннем хранилище приложения. Нажмите, чтобы разрешить, затем перезапустите игру.",
                          "Not granted: logs and the data folder stay in the app's own internal storage. Tap to grant it, then restart the game."));
                if (!on) {
                    android.widget.Button b = s.button(box, s.t("Открыть настройки доступа", "Open the access settings"));
                    b.setOnClickListener(v -> {
                        try {
                            a.startActivity(android.os.Build.VERSION.SDK_INT >= 30
                                ? new android.content.Intent(android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                      android.net.Uri.parse("package:" + a.getPackageName()))
                                : new android.content.Intent(android.provider.Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                                      android.net.Uri.parse("package:" + a.getPackageName())));
                        } catch (Exception e) { android.util.Log.e("pillars-f2", "storage settings: " + e); }
                    });
                }
            }
            public void save(GearSettings s, SharedPreferences.Editor game, SharedPreferences.Editor pad) { /* nothing to save: this row only shows status + a button */ }
        };
    }
    static GearSettings.Section fps(final DmActivity a) {
        return new GearSettings.Section() {
            RadioGroup r;
            public void build(GearSettings s, LinearLayout box) {
                s.heading(box, s.when(s.t("Частота кадров", "Frame rate"), true));
                r = s.radios(box, new String[] {"30", "60", "90", "120"}, GearSettings.indexOf(FPS, s.gamePrefs().getInt(P_FPS, 60), 1), true);
                s.note(box, s.t("30 — ровнее и холоднее на слабых телефонах; 90 и 120 — если экран и система дают такую частоту этому приложению",
                    "30 is steadier and cooler on weaker phones; 90 and 120 need a screen and a system that give this app that rate"));
            }
            public void save(GearSettings s, SharedPreferences.Editor game, SharedPreferences.Editor pad) { game.putInt(P_FPS, FPS[GearSettings.picked(r)]); }
            public void after(GearSettings s) { if (a != null) a.liveFps(); }      // no restart: the display and the game's cap follow at once
        };
    }
}
