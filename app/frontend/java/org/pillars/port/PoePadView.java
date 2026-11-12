// Pillars of Eternity's on-screen controls: the look and the stick logic of our Diablo III / Torchlight II pad (d3-port-repo D3PadView "gamepad" layout,
// docs/d3/TOUCH_DESIGN.md; port-kb IN-06, IN-09), without anything game-specific (skills, potions, world walking, menu mode) and with the touch screen kept for
// the game, which Pillars plays as the PC version by mouse (hle_poe_touch.c):
//   - two floating sticks, not drawn while idle: a finger that lands in a stick's zone (left: the left part below the top row; right: the lower right, the
//     buttons win) and DRAGS (past TAP_SLOP_DP) puts that stick's base where it went down and tilts it; a faint ring shows it while held, it goes with the finger;
//   - a finger in a stick zone that does not move is the game's: lifted before HOLD_COMMIT_MS = a tap on the picture (the host makes it a click); held still
//     that long = it becomes the game's touch from then on (a hold = right click, a drag after that = the mouse drag, e.g. the selection box);
//   - a finger anywhere else that hits no control goes to the game at once (Game.gameTouch), multi-touch;
//   - buttons as D3 / TL2: round, A/B/X/Y diamond on the right, L / ZL and R / ZR in the top corners, - ⚙ + on top, d-pad bottom left, L3 / R3 as their own
//     small buttons (a hidden stick has no knob to tap); a press lights the button; hit = exactly the drawn circle (no reach margin), so a tap beside a
//     button reaches the game;
//   - ⚙ is ours always (DOOM / D3 / TL2: the gear glyph, normal / faded "gear_mode" / "gear_alpha"), never a skin's menu sprite; it opens the pause menu;
//   - the pad hidden ("shown" false, the default): only ⚙ is drawn and hit, every other finger is the game's touch screen, no sticks;
//   - a real controller in use: only ⚙;
//   - skins (PadSkin) draw the face / shoulder buttons and the d-pad when the player picks one; "classic" (default) = D3's circles;
//   - opacity "poe_opacity", size "poe_scale", the layout editor (D3 / TL2: toolbar − + Grouped/Separate Hide/Show Reset Cancel Save; drag, a second finger
//     resizes); places "poe_pos_<group>" = "fx,fy,scale", "poe_btn_<name>" = "dx;dy;scale;hidden".
// Output: the native controller channel as every port's pad (DmActivity.nativePadKey / nativePadAxes, keycodes front_android.c maps), the game's fingers
// through Game.gameTouch (DmActivity -> nativeTouch).
package org.pillars.port;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.SystemClock;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

public class PoePadView extends View implements FrontMenu.Pad {
    // the Activity's side: the game's touch screen (the fingers that are the game's: pointer ids, positions in fractions of this view)
    interface Game { boolean gameTouchOn(); void gameTouch(int n, int[] ids, float[] xs, float[] ys); }
    static final String PREFS = DoomPadView.PREFS;   // "doom_controls", as every port's pad
    static final int PAD_DEVICE = DoomPadView.PAD_DEVICE;
    static final int SETTINGS = -1;
    static final int OPACITY_MIN = 10, OPACITY_MAX = 100, OPACITY_DEFAULT = 100, SCALE_MIN = 60, SCALE_MAX = 160, SCALE_DEFAULT = 100;
    static final float TAP_SLOP_DP = 14;        // a stick-zone finger that moves farther is the stick (D3PadView's tap slop)
    static final long HOLD_COMMIT_MS = 250;     // a stick-zone finger held still this long is the game's (a hold = right click)
    static final int GEAR_NORMAL = DoomPadView.GEAR_NORMAL, GEAR_FADED = DoomPadView.GEAR_FADED;
    static final int K_A = KeyEvent.KEYCODE_ENTER, K_B = KeyEvent.KEYCODE_ESCAPE, K_X = KeyEvent.KEYCODE_R, K_Y = KeyEvent.KEYCODE_F,
        K_L = KeyEvent.KEYCODE_Q, K_R = KeyEvent.KEYCODE_E, K_ZL = KeyEvent.KEYCODE_SHIFT_LEFT, K_ZR = KeyEvent.KEYCODE_CTRL_LEFT,
        K_PLUS = KeyEvent.KEYCODE_TAB, K_MINUS = KeyEvent.KEYCODE_M, K_UP = KeyEvent.KEYCODE_DPAD_UP, K_DOWN = KeyEvent.KEYCODE_DPAD_DOWN,
        K_LEFT = KeyEvent.KEYCODE_DPAD_LEFT, K_RIGHT = KeyEvent.KEYCODE_DPAD_RIGHT, K_L3 = KeyEvent.KEYCODE_BUTTON_THUMBL, K_R3 = KeyEvent.KEYCODE_BUTTON_THUMBR;

    // A control's place (D3PadView): anchor = (fx, fy) of the view + (gx, gy) dp, or its group's saved anchor; centre = anchor + (ox, oy) dp
    private static class Ctl {
        final String group, name; final float fx, fy, gx, gy, ox, oy, r; int pointer = -1;
        float dx, dy, ps = 1; boolean hidden;
        Ctl(String group, String name, float fx, float fy, float gx, float gy, float ox, float oy, float r) {
            this.group = group; this.name = name; this.fx = fx; this.fy = fy; this.gx = gx; this.gy = gy; this.ox = ox; this.oy = oy; this.r = r;
        }
    }
    private static final class Btn extends Ctl {
        final String label, skinName; final int key;
        Btn(String label, int key, String skinName, String group, float fx, float fy, float gx, float gy, float ox, float oy, float r) {
            super(group, group + "_" + (key < 0 ? "gear" : String.valueOf(key)), fx, fy, gx, gy, ox, oy, r); this.label = label; this.key = key; this.skinName = skinName;
        }
    }
    // home (where the editor shows it; its zone is measured from it): |x - home x| < zoneW of the width and y > home y - zoneUp of the height
    private static final class Stick extends Ctl {
        final boolean rightStick; final float zoneW, zoneUp;
        float x, y, baseX = Float.NaN, baseY;
        Stick(String group, boolean right, float fx, float fy, float r, float zoneW, float zoneUp) { super(group, group, fx, fy, 0, 0, 0, 0, r); rightStick = right; this.zoneW = zoneW; this.zoneUp = zoneUp; }
    }
    // a finger in a stick zone that has not decided yet (tap / hold for the game, or a drag for the stick)
    private static final class Pending { int id; Stick stick; float x0, y0, x, y; long t0; }

    private final float dp;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final List<Btn> btns = new ArrayList<>();
    private final Stick left, right;
    private final Map<String, float[]> pos = new HashMap<>();
    private final Map<Integer, Pending> pending = new HashMap<>();
    private final LinkedHashMap<Integer, float[]> game = new LinkedHashMap<>();   // the game's fingers: id -> {x, y} in px
    private boolean gameSent;
    private boolean split, shown, controllerMode, editMode, showFps;
    private float deadzone, op = 1f, scale = 1f;
    private int gearMode = GEAR_NORMAL, gearAlpha = DoomPadView.GEAR_ALPHA_DEFAULT;
    private PadSkin skin; private String skinPack = PadSkin.CLASSIC;

    public PoePadView(Context c) {
        super(c);
        dp = c.getResources().getDisplayMetrics().density;
        line.setStyle(Paint.Style.STROKE); line.setStrokeWidth(2 * dp);
        text.setTextAlign(Paint.Align.CENTER);
        // D3PadView's gamepad layout (2400x1080 ~ 914x411 dp), its floating sticks; + L3 / R3 buttons
        // the zones are kept to the screen's lower corners (owner, 11.11.2026): the middle of the picture is the game's, so a drag there is the mouse's
        // selection box, not a stick
        left = new Stick("lstick", false, 0.125f, 0.52f, 64, 0.125f, 0.10f);    // zone: x < 0.25 of the width, below 0.42 of the height
        right = new Stick("rstick", true, 0.80f, 0.80f, 50, 0.11f, 0.32f);      // zone: x 0.69..0.91, below 0.48 (the buttons there win)
        final float ax = 0.875f, ay = 0.48f, g = 50;
        add("X", K_X, "x", "abxy", ax, ay, 0, 0, 0, -g, 27); add("A", K_A, "a", "abxy", ax, ay, 0, 0, g, 0, 30);
        add("B", K_B, "b", "abxy", ax, ay, 0, 0, 0, g, 27); add("Y", K_Y, "y", "abxy", ax, ay, 0, 0, -g, 0, 27);
        add("R", K_R, "r", "r", 0.81f, 0.09f, 0, 0, 0, 0, 26); add("ZR", K_ZR, "zr", "zr", 0.935f, 0.09f, 0, 0, 0, 0, 28);
        add("+", K_PLUS, "plus", "plus", 0.62f, 0.09f, 0, 0, 0, 0, 22);
        add("↑", K_UP, null, "dpad", 0.265f, 0.82f, 0, 0, 0, -40, 21); add("→", K_RIGHT, null, "dpad", 0.265f, 0.82f, 0, 0, 40, 0, 21);
        add("↓", K_DOWN, null, "dpad", 0.265f, 0.82f, 0, 0, 0, 40, 21); add("←", K_LEFT, null, "dpad", 0.265f, 0.82f, 0, 0, -40, 0, 21);
        add("L", K_L, "l", "l", 0.19f, 0.09f, 0, 0, 0, 0, 26); add("ZL", K_ZL, "zl", "zl", 0.065f, 0.09f, 0, 0, 0, 0, 28);
        add("-", K_MINUS, "minus", "minus", 0.38f, 0.09f, 0, 0, 0, 0, 22);
        add("⚙", SETTINGS, null, "gear", 0.50f, 0.09f, 0, 0, 0, 0, 20);
        add("L3", K_L3, "ls", "l3", 0, 0.27f, 34, 0, 0, 0, 19); add("R3", K_R3, "rs", "r3", 1, 0.27f, -34, 0, 0, 0, 19);
        reloadSettings();
    }
    private void add(String label, int key, String skinName, String group, float fx, float fy, float gx, float gy, float ox, float oy, float r) {
        btns.add(new Btn(label, key, skinName, group, fx, fy, gx, gy, ox, oy, r));
    }
    private List<Ctl> all() { List<Ctl> a = new ArrayList<>(btns); a.add(left); a.add(right); return a; }
    private SharedPreferences prefs() { return getContext().getSharedPreferences(PREFS, Context.MODE_PRIVATE); }

    @Override public void reloadSettings() {
        SharedPreferences p = prefs();
        deadzone = p.getFloat("move_dz", 0f);
        boolean fps = p.getBoolean("fps", true); if (fps && !showFps) postDelayed(fpsTick, 500); showFps = fps;
        gearMode = p.getInt("gear_mode", GEAR_NORMAL) == GEAR_NORMAL ? GEAR_NORMAL : GEAR_FADED;
        gearAlpha = clamp(p.getInt("gear_alpha", DoomPadView.GEAR_ALPHA_DEFAULT), 1, 100);
        op = clamp(p.getInt("poe_opacity", OPACITY_DEFAULT), OPACITY_MIN, OPACITY_MAX) / 100f;
        scale = clamp(p.getInt("poe_scale", SCALE_DEFAULT), SCALE_MIN, SCALE_MAX) / 100f;
        skinPack = p.getString("skin", PadSkin.CLASSIC);
        boolean sh = p.getBoolean("shown", false);
        if (sh != shown) { releaseAll(); shown = sh; }
        split = p.getBoolean("poe_split", false);
        if (!editMode) loadPlaces(p);
        invalidate();
    }
    private String posKey(String group) { return "poe_pos_" + group; }
    private String btnKey(Ctl k) { return "poe_btn_" + k.name; }
    private void loadPlaces(SharedPreferences p) {
        pos.clear();
        for (Ctl k : all()) {
            String v = p.getString(posKey(k.group), null);
            if (v != null) { String[] f = v.split(",");
                try { if (f.length >= 2) pos.put(k.group, new float[] {clampF(Float.parseFloat(f[0])), clampF(Float.parseFloat(f[1])), f.length >= 3 ? clampS(Float.parseFloat(f[2])) : 1f}); }
                catch (NumberFormatException e) { /* ignore */ } }
            k.dx = k.dy = 0; k.ps = 1; k.hidden = false;
            String b = p.getString(btnKey(k), null); if (b == null) continue;
            String[] f = b.split(";");
            try { if (f.length >= 3) { k.dx = Float.parseFloat(f[0]); k.dy = Float.parseFloat(f[1]); k.ps = clampS(Float.parseFloat(f[2])); k.hidden = f.length >= 4 && "1".equals(f[3]) && canHide(k); } }
            catch (NumberFormatException e) { k.dx = k.dy = 0; k.ps = 1; k.hidden = false; }
        }
    }
    private void savePlaces() {
        SharedPreferences.Editor ed = prefs().edit();
        for (Ctl k : all()) {
            float[] o = pos.get(k.group);
            if (o == null) ed.remove(posKey(k.group)); else ed.putString(posKey(k.group), String.format(Locale.ROOT, "%.4f,%.4f,%.4f", o[0], o[1], o[2]));
            if (k.dx == 0 && k.dy == 0 && k.ps == 1 && !k.hidden) ed.remove(btnKey(k));
            else ed.putString(btnKey(k), String.format(Locale.ROOT, "%.5f;%.5f;%.4f;%d", k.dx, k.dy, k.ps, k.hidden ? 1 : 0));
        }
        ed.apply();
    }
    static final float EDIT_SCALE_MIN = 0.5f, EDIT_SCALE_MAX = 2.5f;
    private static float clampS(float v) { return Math.max(EDIT_SCALE_MIN, Math.min(EDIT_SCALE_MAX, v)); }
    private static boolean canHide(Ctl k) { return k instanceof Btn && ((Btn) k).key != SETTINGS; }
    private static int clamp(int v, int lo, int hi) { return Math.max(lo, Math.min(hi, v)); }
    private static float clampF(float v) { return Math.max(0f, Math.min(1f, v)); }

    // ---- a real controller in use (DmActivity): only ⚙; everything held is released
    void setControllerMode(boolean on) { if (on == controllerMode) return; if (on) releaseAll(); controllerMode = on; invalidate(); }
    boolean controllerMode() { return controllerMode; }
    boolean shown() { return shown; }

    // ---- geometry (D3PadView)
    private float u() { return dp * scale; }
    private float ax(Ctl c) { float[] o = pos.get(c.group); return o != null ? o[0] * getWidth() : c.fx * getWidth() + c.gx * u(); }
    private float ay(Ctl c) { float[] o = pos.get(c.group); return o != null ? o[1] * getHeight() : c.fy * getHeight() + c.gy * u(); }
    private float gs(Ctl c) { float[] o = pos.get(c.group); return o != null ? o[2] : 1f; }
    private float cx(Ctl c) { return ax(c) + (c.ox * u() + c.dx * getWidth()) * gs(c); }
    private float cy(Ctl c) { return ay(c) + (c.oy * u() + c.dy * getHeight()) * gs(c); }
    private float rad(Ctl c) { return c.r * u() * gs(c) * c.ps; }
    private float sx(Stick s) { return Float.isNaN(s.baseX) ? cx(s) : s.baseX; }
    private float sy(Stick s) { return Float.isNaN(s.baseX) ? cy(s) : s.baseY; }
    private boolean shown(Btn b) {
        if (editMode) return true;
        if (b.key == SETTINGS) return true;
        if (controllerMode || !shown) return false;
        return !b.hidden;
    }
    private boolean sticksOn() { return shown && !controllerMode && !editMode; }
    // exactly the drawn circle (no reach margin: a tap beside a button is the game's); the nearest centre when circles overlap
    private Btn hit(float x, float y) {
        Btn best = null; float bestD = Float.MAX_VALUE;
        for (Btn b : btns) {
            if (!shown(b)) continue;
            float dx = x - cx(b), dy = y - cy(b), r = rad(b), d = dx * dx + dy * dy;
            if (d <= r * r && d < bestD) { best = b; bestD = d; }
        }
        return best;
    }
    private Stick zoneOf(float x, float y) {
        if (!sticksOn()) return null;
        for (Stick s : new Stick[] {right, left})
            if (s.pointer < 0 && Math.abs(x - cx(s)) < s.zoneW * getWidth() && y > cy(s) - s.zoneUp * getHeight()) return s;
        return null;
    }

    // ---- output
    private static void key(int k, boolean down) { DmActivity.nativePadKey(PAD_DEVICE, k, down); }
    private void moveStick(Stick s, float x, float y) {
        float dx = x - sx(s), dy = y - sy(s), d = (float) Math.sqrt(dx * dx + dy * dy), r = rad(s);
        if (d > r) { dx *= r / d; dy *= r / d; }
        s.x = dx; s.y = dy;
    }
    private void grab(Stick s, int id, float x0, float y0) { s.baseX = x0; s.baseY = y0; s.pointer = id; s.x = s.y = 0; }
    private void release(Stick s) { s.pointer = -1; s.x = s.y = 0; s.baseX = Float.NaN; }
    private float[] shape(float x, float y) {   // radial dead-zone compensation (gear "dead-zone compensation")
        float m = (float) Math.sqrt(x * x + y * y);
        if (m < 0.04f) return new float[] {0, 0};
        float o = Math.min(1f, deadzone + (1f - deadzone) * Math.min(1f, m));
        return new float[] {x / m * o, y / m * o};
    }
    private boolean axesSent;
    private void axes() {
        if (controllerMode || editMode) return;
        float[] lv = left.pointer >= 0 ? shape(left.x / rad(left), left.y / rad(left)) : new float[] {0, 0};
        float[] rv = right.pointer >= 0 ? shape(right.x / rad(right), right.y / rad(right)) : new float[] {0, 0};
        boolean any = lv[0] != 0 || lv[1] != 0 || rv[0] != 0 || rv[1] != 0;
        if (!any && !axesSent) return;   // nothing held: no axes (a free touch must not count as controller use)
        axesSent = any;
        DmActivity.nativePadAxes(PAD_DEVICE, lv[0], lv[1], rv[0], rv[1], 0, 0, 0, 0);
    }
    // the game's fingers as they are now (an empty set once the last one lifted)
    private void sendGame() {
        if (game.isEmpty() && !gameSent) return;
        int n = game.size(), i = 0; int[] ids = new int[n]; float[] xs = new float[n], ys = new float[n];
        float w = Math.max(1, getWidth()), h = Math.max(1, getHeight());
        for (Map.Entry<Integer, float[]> e : game.entrySet()) { ids[i] = e.getKey(); xs[i] = e.getValue()[0] / w; ys[i] = e.getValue()[1] / h; i++; }
        gameSent = n > 0;
        if (getContext() instanceof Game) ((Game) getContext()).gameTouch(n, ids, xs, ys);
    }
    private boolean gameOn() { return getContext() instanceof Game && ((Game) getContext()).gameTouchOn(); }
    // a stick-zone finger held still: it is the game's from now on (where it is, within the tap slop of where it went down)
    private final Runnable commitHeld = this::commitPending;
    private void commitPending() {
        long now = SystemClock.uptimeMillis(); boolean any = false, again = false;
        for (Pending p : new ArrayList<>(pending.values())) {
            if (now - p.t0 >= HOLD_COMMIT_MS) { pending.remove(p.id); if (gameOn()) { game.put(p.id, new float[] {p.x, p.y}); any = true; } }
            else again = true;
        }
        if (any) sendGame();
        if (again) { removeCallbacks(commitHeld); postDelayed(commitHeld, 30); }
    }
    

    // ---- touches
    @Override public boolean onTouchEvent(MotionEvent e) {
        int a = e.getActionMasked(), i = e.getActionIndex(), id = e.getPointerId(i);
        float x = e.getX(i), y = e.getY(i);
        if (editMode) { editTouch(e, a, id, x, y); return true; }
        switch (a) {
        case MotionEvent.ACTION_DOWN: case MotionEvent.ACTION_POINTER_DOWN: {
            Btn b = hit(x, y); Stick z;
            if (b != null && b.key == SETTINGS) { b.pointer = id; }
            else if (b != null) { if (b.pointer < 0) { b.pointer = id; key(b.key, true); } }
            else if ((z = zoneOf(x, y)) != null && gameOn()) {   // a stick or the game's: decided by what the finger does
                Pending p = new Pending(); p.id = id; p.stick = z; p.x0 = p.x = x; p.y0 = p.y = y; p.t0 = SystemClock.uptimeMillis(); pending.put(id, p);
                removeCallbacks(commitHeld); postDelayed(commitHeld, HOLD_COMMIT_MS);
            }
            else if (gameOn()) game.put(id, new float[] {x, y});
            break;
        }
        case MotionEvent.ACTION_MOVE:
            for (int k = 0; k < e.getPointerCount(); k++) {
                int pid = e.getPointerId(k); float px = e.getX(k), py = e.getY(k);
                Pending p = pending.get(pid);
                if (p != null) {
                    p.x = px; p.y = py;
                    if (Math.hypot(px - p.x0, py - p.y0) > TAP_SLOP_DP * dp) {   // a drag: the stick, from where the finger went down
                        pending.remove(pid);
                        if (p.stick.pointer < 0 && sticksOn()) { grab(p.stick, pid, p.x0, p.y0); moveStick(p.stick, px, py); }
                    }
                }
                if (pid == left.pointer) moveStick(left, px, py);
                if (pid == right.pointer) moveStick(right, px, py);
                float[] g = game.get(pid); if (g != null) { g[0] = px; g[1] = py; }
            }
            break;
        case MotionEvent.ACTION_UP: case MotionEvent.ACTION_POINTER_UP: case MotionEvent.ACTION_CANCEL: {
            boolean all = a != MotionEvent.ACTION_POINTER_UP, cancel = a == MotionEvent.ACTION_CANCEL;
            for (Btn b : btns) if (b.pointer >= 0 && (b.pointer == id || all)) {
                if (b.key == SETTINGS) { if (!cancel && b.pointer == id && hit(x, y) == b) post(this::openMenu); }
                else key(b.key, false);
                b.pointer = -1;
            }
            if (left.pointer >= 0 && (id == left.pointer || all)) release(left);
            if (right.pointer >= 0 && (id == right.pointer || all)) release(right);
            Pending p = pending.remove(id);
            if (p != null && !cancel && gameOn()) {   // a still tap in a stick zone: the game's tap, down and up at once (the host keeps a short tap)
                game.put(id, new float[] {p.x0, p.y0}); sendGame();
            }
            if (all) { pending.clear(); game.clear(); } else game.remove(id);
            break;
        }
        }
        axes(); sendGame(); invalidate();
        return true;
    }
    private void openMenu() { if (getContext() instanceof FrontMenu.Owner) { FrontMenu m = ((FrontMenu.Owner) getContext()).frontMenu(); if (m != null) m.open(); } }

    // ---- FrontMenu.Pad
    @Override public void releaseAll() {
        for (Btn b : btns) if (b.pointer >= 0) { if (b.key >= 0) key(b.key, false); b.pointer = -1; }
        if (left.pointer >= 0) release(left);
        if (right.pointer >= 0) release(right);
        pending.clear(); removeCallbacks(commitHeld);
        game.clear(); sendGame();
        if (axesSent && !controllerMode) { axesSent = false; DmActivity.nativePadAxes(PAD_DEVICE, 0, 0, 0, 0, 0, 0, 0, 0); }
        invalidate();
    }
    private Runnable editDone;
    @Override public void startEdit(Runnable done) {
        editDone = done; releaseAll(); editMode = true; edSel = null; edPtr = edPtr2 = edBtnDown = -1;
        edSavedPos.clear(); for (Map.Entry<String, float[]> en : pos.entrySet()) edSavedPos.put(en.getKey(), en.getValue().clone());
        edSavedCtl.clear(); for (Ctl k : all()) edSavedCtl.put(k, new float[] {k.dx, k.dy, k.ps, k.hidden ? 1 : 0});
        android.util.Log.i(DmActivity.TAG, "[java] layout editor open"); invalidate();
    }
    @Override public boolean editing() { return editMode; }
    @Override public void endEdit(boolean save) {
        if (!editMode) return;
        if (save) savePlaces();
        else {
            pos.clear(); for (Map.Entry<String, float[]> en : edSavedPos.entrySet()) pos.put(en.getKey(), en.getValue().clone());
            for (Ctl k : all()) { float[] v = edSavedCtl.get(k); if (v != null) { k.dx = v[0]; k.dy = v[1]; k.ps = v[2]; k.hidden = v[3] != 0; } }
        }
        editMode = false; edPtr = edPtr2 = edBtnDown = -1; edSel = null; invalidate();
        android.util.Log.i(DmActivity.TAG, "[java] layout editor " + (save ? "saved" : "cancelled"));
        Runnable d = editDone; editDone = null; if (d != null) d.run();
    }
    @Override public void resetLayout() { clearPlaces(); savePlaces(); invalidate(); }
    @Override public String prefsName() { return PREFS; }
    @Override public String[] sliderKeys() { return new String[] {"poe_opacity", "poe_scale"}; }
    @Override public int[] sliderDefaults() { return new int[] {OPACITY_DEFAULT, SCALE_DEFAULT}; }
    @Override public String[] presets() { return null; }   // one layout
    @Override public View view() { return this; }

    // ---- the layout editor (D3PadView / TL2: toolbar − + Grouped/Separate Hide/Show Reset Cancel Save)
    private static final int ED_BUTTONS = 7;
    private int edPtr = -1, edPtr2 = -1, edBtnDown = -1; private Ctl edSel;
    private float edX0, edY0, edD0, edDx0, edDy0, edPs0; private float[] edO0 = {0, 0, 1};
    private final Map<String, float[]> edSavedPos = new HashMap<>(); private final Map<Ctl, float[]> edSavedCtl = new HashMap<>();
    private final RectF[] edRects = new RectF[ED_BUTTONS]; private final RectF selRect = new RectF();
    private void clearPlaces() { pos.clear(); for (Ctl k : all()) { k.dx = k.dy = 0; k.ps = 1; k.hidden = false; } }
    private boolean selOne() { return edSel != null && (split || edSel instanceof Stick); }
    private boolean inSel(Ctl k) { return editMode && edSel != null && (k == edSel || (!selOne() && k.group.equals(edSel.group))); }
    private Ctl hitAny(float x, float y) {
        Ctl best = null; float bestD = Float.MAX_VALUE;
        for (Ctl c : all()) {
            float dx = x - cx(c), dy = y - cy(c), r = Math.max(rad(c), 18 * dp) * (c instanceof Stick ? 1.0f : 1.25f), d = dx * dx + dy * dy;
            if (d <= r * r && d < bestD) { best = c; bestD = d; }
        }
        return best;
    }
    private void edGrab(float x, float y) {
        edX0 = x; edY0 = y; if (edSel == null) return;
        float[] o = pos.get(edSel.group);
        edO0 = o != null ? o.clone() : new float[] {ax(edSel) / getWidth(), ay(edSel) / getHeight(), 1f};
        edDx0 = edSel.dx; edDy0 = edSel.dy; edPs0 = edSel.ps;
    }
    private void edMove(float x, float y) {
        float w = getWidth(), h = getHeight(); if (w <= 0 || h <= 0 || edSel == null) return;
        float ddx = x - edX0, ddy = y - edY0;
        if (selOne() && !(edSel instanceof Stick)) { float g = gs(edSel); edSel.dx = edDx0 + ddx / (g * w); edSel.dy = edDy0 + ddy / (g * h); }
        else pos.put(edSel.group, new float[] {clampF(edO0[0] + ddx / w), clampF(edO0[1] + ddy / h), edO0[2]});
    }
    private void edScale(float f) {
        if (edSel == null) return;
        if (selOne() && !(edSel instanceof Stick)) edSel.ps = clampS(edPs0 * f);
        else pos.put(edSel.group, new float[] {edO0[0], edO0[1], clampS(edO0[2] * f)});
    }
    private void edScaleAll(float f) {
        java.util.Set<String> done = new java.util.HashSet<>();
        for (Ctl k : all()) { if (!done.add(k.group)) continue; float[] o = pos.get(k.group);
            pos.put(k.group, o != null ? new float[] {o[0], o[1], clampS(o[2] * f)} : new float[] {ax(k) / getWidth(), ay(k) / getHeight(), clampS(f)}); }
    }
    private boolean canHideSel() { for (Ctl k : all()) if (inSel(k) && canHide(k)) return true; return false; }
    private boolean selHidden() { boolean any = false; for (Ctl k : all()) if (inSel(k) && canHide(k)) { if (!k.hidden) return false; any = true; } return any; }
    private void edToggleHide() { if (!canHideSel()) return; boolean hide = !selHidden(); for (Ctl k : all()) if (inSel(k) && canHide(k)) k.hidden = hide; }
    private RectF edButton(int i) {
        float W = getWidth(), H = getHeight(), gap = H * 0.02f, bw = Math.min(H * 0.17f, (W - 32 * dp - (ED_BUTTONS - 1) * gap) / ED_BUTTONS), bh = H * 0.085f;
        float total = ED_BUTTONS * bw + (ED_BUTTONS - 1) * gap, x0 = (W - total) / 2, y0 = H * 0.30f;
        if (edRects[i] == null) edRects[i] = new RectF();
        edRects[i].set(x0 + i * (bw + gap), y0, x0 + i * (bw + gap) + bw, y0 + bh); return edRects[i];
    }
    private int edButtonAt(float x, float y) { for (int i = 0; i < ED_BUTTONS; i++) if (edButton(i).contains(x, y)) return i; return -1; }
    private boolean edAction(int bt) {
        switch (bt) {
        case 0: case 1: { float f = bt == 1 ? 1.1f : 1 / 1.1f; if (edSel == null) edScaleAll(f); else { edGrab(edX0, edY0); edScale(f); } break; }
        case 2: split = !split; prefs().edit().putBoolean("poe_split", split).apply(); break;
        case 3: edToggleHide(); break;
        case 4: clearPlaces(); edSel = null; break;
        case 5: endEdit(false); return true;
        default: endEdit(true); return true;
        }
        return false;
    }
    private void editTouch(MotionEvent e, int a, int id, float x, float y) {
        if (a == MotionEvent.ACTION_DOWN || a == MotionEvent.ACTION_POINTER_DOWN) {
            int bt = edButtonAt(x, y);
            if (bt >= 0 && edPtr < 0) { edBtnDown = bt; edPtr = id; }
            else if (edPtr >= 0 && edBtnDown < 0 && edPtr2 < 0 && edSel != null) {
                int k1 = e.findPointerIndex(edPtr);
                if (k1 >= 0) { edD0 = (float) Math.hypot(x - e.getX(k1), y - e.getY(k1)); edPtr2 = id; edGrab(e.getX(k1), e.getY(k1)); }
            } else if (edPtr < 0) { edSel = hitAny(x, y); edBtnDown = -1; edPtr = edSel != null ? id : -1; if (edSel != null) edGrab(x, y); }
        } else if (a == MotionEvent.ACTION_MOVE) {
            if (edPtr >= 0 && edBtnDown < 0 && edSel != null) {
                int k1 = e.findPointerIndex(edPtr), k2 = edPtr2 >= 0 ? e.findPointerIndex(edPtr2) : -1;
                if (k1 >= 0 && k2 >= 0 && edD0 > 1) edScale((float) Math.hypot(e.getX(k1) - e.getX(k2), e.getY(k1) - e.getY(k2)) / edD0);
                else if (k1 >= 0) edMove(e.getX(k1), e.getY(k1));
            }
        } else if (a == MotionEvent.ACTION_UP || a == MotionEvent.ACTION_POINTER_UP || a == MotionEvent.ACTION_CANCEL) {
            if (a == MotionEvent.ACTION_POINTER_UP && id == edPtr2) { edPtr2 = -1; int k1 = e.findPointerIndex(edPtr); if (k1 >= 0) edGrab(e.getX(k1), e.getY(k1)); }
            else if (id == edPtr || a != MotionEvent.ACTION_POINTER_UP) {
                int bt = edBtnDown; edPtr = edPtr2 = edBtnDown = -1;
                if (bt >= 0 && a != MotionEvent.ACTION_CANCEL && bt == edButtonAt(x, y) && edAction(bt)) return;
            }
        }
        invalidate();
    }

    // ---- drawing (D3PadView's classic look; a skin draws the buttons and the d-pad when one is picked; ⚙ always ours)
    private final float[] fstats = new float[4];
    private final Runnable fpsTick = new Runnable() { public void run() { if (!showFps) return; try { DmActivity.nativeFrameStats(fstats); } catch (UnsatisfiedLinkError e) { fstats[0] = 0; } invalidate(); postDelayed(this, 500); } };
    private float cornerCx, cornerCy, cornerR, safeLeft;
    @Override public android.view.WindowInsets onApplyWindowInsets(android.view.WindowInsets in) {
        cornerR = 0; safeLeft = 0;
        if (android.os.Build.VERSION.SDK_INT >= 31) {
            android.view.RoundedCorner rc = in.getRoundedCorner(android.view.RoundedCorner.POSITION_TOP_LEFT);
            if (rc != null) { cornerR = rc.getRadius(); cornerCx = rc.getCenter().x; cornerCy = rc.getCenter().y; }
            safeLeft = in.getInsets(android.view.WindowInsets.Type.displayCutout()).left;
        }
        invalidate(); return super.onApplyWindowInsets(in);
    }
    private float cornerLeft(float y) { if (cornerR <= 0 || y >= cornerCy) return 0; float dy = cornerCy - y; return dy >= cornerR ? cornerCx : cornerCx - (float) Math.sqrt(cornerR * cornerR - dy * dy); }
    private void drawFps(Canvas c) {
        if (!showFps || fstats[0] <= 0) return;
        String t = String.format(Locale.ROOT, "%.0f fps  %.1f ms (max %.0f)%s", fstats[0], fstats[1], fstats[2], fstats[3] >= 0 ? String.format(Locale.ROOT, "  GPU %.1f", fstats[3]) : "");
        text.setTextSize(15 * dp); text.setColor(0xd0ffffff);
        float y = text.getTextSize(), top = y + text.ascent(), left = Math.max(18 * dp, Math.max(safeLeft, cornerLeft(top)) + 6 * dp);
        float w = text.measureText(t), x = left + w / 2;
        fill.setColor(0x80000000); c.drawRect(0, 0, x + w / 2 + 6 * dp, y + 5 * dp, fill);
        c.drawText(t, x, y, text);
    }
    @Override protected void onDraw(Canvas c) {
        super.onDraw(c);
        if (!editMode) drawFps(c);
        if (editMode) { c.drawColor(0x50000000); drawStick(c, left); drawStick(c, right); for (Btn b : btns) drawBtn(c, b); drawEditor(c); return; }
        if (sticksOn()) { drawStick(c, left); drawStick(c, right); }
        for (Btn b : btns) if (shown(b)) drawBtn(c, b);
    }
    private float k(Ctl b) { return editMode ? (inSel(b) ? 1f : Math.max(op, 0.5f)) * (b.hidden ? 0.4f : 1f) : op; }
    private PadSkin skin() { if (skin == null) skin = new PadSkin(getContext()); skin.select(skinPack, PadSkin.layoutFor(getWidth(), getHeight())); return skin; }
    // hidden sticks (D3 / D2R Mobile): an idle stick is not drawn; while held, a faint ring at its base and the knob; the editor shows the homes
    private void drawStick(Canvas c, Stick s) {
        if (!editMode && s.pointer < 0) return;
        float x = sx(s), y = sy(s), r = rad(s), k = editMode ? k(s) : op * 0.5f;
        fill.setColor(0xffffffff); fill.setAlpha(Math.round(0x20 * k)); c.drawCircle(x, y, r, fill);
        line.setColor(editMode ? 0xffffd040 : 0xffffffff); line.setAlpha(editMode ? 0xe0 : Math.round(0x90 * k)); c.drawCircle(x, y, r, line);
        fill.setAlpha(Math.round((s.pointer >= 0 ? 0x80 : 0x40) * k)); c.drawCircle(x + s.x, y + s.y, r * 0.42f, fill);
        if (editMode) { text.setTextSize(13 * dp); text.setColor(0xffffd040); c.drawText(s.rightStick ? "R" : "L", x, y + 5 * dp, text); }
    }
    private void drawBtn(Canvas c, Btn b) {
        boolean gear = b.key == SETTINGS, on = b.pointer >= 0;
        float k = gear ? (gearMode == GEAR_FADED && !editMode && !on ? gearAlpha / 100f : 1f) : k(b);
        float x = cx(b), y = cy(b), r = rad(b);
        if (gear || !drawSkinned(c, b, x, y, r, k, on)) {
            fill.setColor(0xffffffff); fill.setAlpha(Math.round((on ? 0x80 : 0x30) * k));
            line.setColor(editMode ? 0xffffd040 : 0xffffffff); line.setAlpha(editMode ? 0xe0 : Math.round(0x90 * k));
            c.drawCircle(x, y, r, fill); c.drawCircle(x, y, r, line);
            float ts = gear ? 22 * u() : 15 * u(); text.setTextSize(ts); float w = text.measureText(b.label);
            if (w > 1.7f * r) { ts *= 1.7f * r / w; text.setTextSize(ts); }
            text.setColor(0xffffffff); text.setAlpha(Math.round(0xd0 * k));
            c.drawText(b.label, x, y + ts * 0.35f, text);
        }
        if (editMode && b.hidden) { line.setColor(0xffff5050); line.setAlpha(0xe0); float d = r * 0.75f; c.drawLine(x - d, y + d, x + d, y - d, line); }
    }
    private boolean drawSkinned(Canvas c, Btn b, float x, float y, float r, float k, boolean on) {
        PadSkin s = skin(); if (!s.on()) return false;
        float sa = s.packAlpha() * k;
        if (b.key == K_UP || b.key == K_DOWN || b.key == K_LEFT || b.key == K_RIGHT) {
            if (!s.has("dpad")) return false;
            if (b.key != K_UP) return true;
            int bits = 0; float gap = 0;
            for (Btn d : btns) if (d.group.equals(b.group)) { gap = Math.max(gap, (float) Math.hypot(cx(d) - ax(d), cy(d) - ay(d)));
                if (d.pointer >= 0) bits |= d.key == K_UP ? PadTheme.UP : d.key == K_DOWN ? PadTheme.DOWN : d.key == K_LEFT ? PadTheme.LEFT : PadTheme.RIGHT; }
            return s.draw(c, "dpad", ax(b), ay(b), gap + r, gap + r, false, bits, 0, 0, sa, getHeight());
        }
        if (b.skinName == null) return false;
        return s.draw(c, b.skinName, x, y, r, r, on, 0, 0, 0, sa, getHeight());   // inside the hit circle's box
    }
    private void drawEditor(Canvas c) {
        boolean ru = Ui.ru(getContext()), sh = selHidden(), ch = canHideSel();
        String[] names = ru ? new String[] {"−", "+", split ? "Раздельно" : "Группами", sh ? "Показать" : "Скрыть", "Сброс", "Отмена", "Сохранить"}
                            : new String[] {"−", "+", split ? "Separate" : "Grouped", sh ? "Show" : "Hide", "Reset", "Cancel", "Save"};
        float H = getHeight(), W = getWidth();
        line.setStrokeWidth(1.5f * dp);
        for (int i = 0; i < ED_BUTTONS; i++) {
            RectF r = edButton(i);
            fill.setColor(i == edBtnDown ? 0xff1fb6c9 : i == ED_BUTTONS - 1 ? 0xe0157c8a : 0xd0202833); line.setColor(0xff7fe3f0);
            float rr = r.height() / 2; c.drawRoundRect(r, rr, rr, fill); c.drawRoundRect(r, rr, rr, line);
            float ts = H * 0.04f; text.setTextSize(ts); float tw = text.measureText(names[i]); if (tw > r.width() * 0.9f) { ts *= r.width() * 0.9f / tw; text.setTextSize(ts); }
            text.setColor(0xffffffff); text.setAlpha(i == 3 && !ch ? 0x60 : 0xff);
            c.drawText(names[i], r.centerX(), r.centerY() + ts * 0.35f, text);
        }
        line.setStrokeWidth(2 * dp);
        selRect.setEmpty();
        for (Ctl k : all()) if (inSel(k)) { float x = cx(k), y = cy(k), r = rad(k); selRect.union(x - r, y - r, x + r, y + r); }
        if (!selRect.isEmpty()) { line.setColor(0xff7fe3f0); selRect.inset(-4 * dp, -4 * dp); c.drawRoundRect(selRect, 12 * dp, 12 * dp, line); }
        String hint = ru ? (split ? "Тяните кнопку пальцем (каждая отдельно)" : "Тяните кнопку пальцем (группа — целиком)") + ", второй палец — размер; −/+ без выбора меняют все; «Скрыть» убирает выбранное из игры. Круги L / R — дома стиков: от них считаются их зоны"
                         : (split ? "Drag a button (every button on its own)" : "Drag a button (its group moves as one)") + "; a second finger resizes it; − / + with none chosen resize all; Hide takes the chosen out of the game. The L / R circles are the sticks' homes: their zones are measured from them";
        float ts = H * 0.032f; text.setTextSize(ts); float tw = text.measureText(hint); if (tw > W * 0.96f) { ts *= W * 0.96f / tw; text.setTextSize(ts); }
        text.setColor(0xffffffff); text.setAlpha(220);
        c.drawText(hint, W / 2f, edButton(0).bottom + H * 0.06f, text);
    }
}
