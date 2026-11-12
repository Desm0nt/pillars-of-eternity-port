// Pillars of Eternity F2: touch plays the game as the PC version is played by mouse - the port's gestures (hle_poe_touch.c, poe-port/docs/FRONTEND.txt),
// made here in Java because the Android Unity player has a real mouse and keyboard: the fingers the on-screen pad leaves to the game (PoePadView.Game)
// become MOUSE and KEY events injected into the Unity player (UnityPlayer.injectEvent), which the game reads through UnityEngine.Input once it is on its
// PC input path (native/boot/f2_touch.c: GameInput.UsingGamepad false, NGUI useMouse on, no edge scrolling).
//   tap                    left click at the finger
//   drag                   left button held: selection box, dragging items
//   hold still (HOLD ms)   right button, held until the finger lifts (a drag after that = right drag)
//   two fingers, drag      middle button: pans the camera
//   pinch                  "=" / "-" (zoom), one step per 15 % of spread
//   two fingers held still the whole party is selected (PartyMemberAI.SelectAll, natively)
//   two-finger tap         pause (the console's pause button, the pad's Y);   three-finger tap: Escape (close the window)
//   while a finger rests   the mouse is under it (hover: highlights, tooltips)
// Positions arrive as pixels of the Unity player's view. All of it runs on the UI thread (the pad's touch handling and a Handler).
package org.pillars.port;

import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.MotionEvent;

final class F2Touch {
    interface Out { void inject(InputEvent e); void log(String line); void padPulse(int padThemeBit); void gameAction(int id); }
    private static final int IDLE = 0, PENDING = 1, DRAG = 2, HOLD = 3, TWO = 4, WAIT = 5;
    private static final long TAP_MS = 70, TWO_TAP_MS = 450;
    private final Out out; private final Handler h = new Handler(Looper.getMainLooper());
    private final float slop;
    private int holdMs = 500;
    private int st = IDLE, prim = -1, sec = -1, maxFingers, buttons, lines;
    private float sx, sy, mx, my, mid0x, mid0y, pinchBase;
    private boolean twoMoved, midDown;
    private long t0, downTime;
    // two fingers held still: the whole party is selected (the console build has no selection box; a tap on a portrait selects one member)
    private final Runnable twoHoldTimer = () -> { if (st == TWO && !twoMoved) { twoMoved = true; selectAll(); } };
    private final Runnable holdTimer = () -> { if (st == PENDING) { press(MotionEvent.BUTTON_SECONDARY, sx, sy); st = HOLD; note("hold -> right button"); } };

    F2Touch(Out out, float dp) { this.out = out; slop = 14 * dp; }
    void setHoldMs(int ms) { holdMs = Math.max(200, Math.min(2000, ms)); }

    // the game's fingers now: ids, positions in view pixels (n = 0: none)
    void fingers(int n, int[] ids, float[] xs, float[] ys) {
        long now = SystemClock.uptimeMillis();
        int pi = index(n, ids, prim), si = index(n, ids, sec);
        switch (st) {
            case IDLE:
                if (n == 0) return;
                prim = ids[0]; sx = xs[0]; sy = ys[0]; t0 = now; maxFingers = n;
                hover(sx, sy); st = PENDING;
                h.removeCallbacks(holdTimer); h.postDelayed(holdTimer, holdMs);
                if (n >= 2) enterTwo(n, ids, xs, ys, now);
                break;
            case PENDING:
                if (pi < 0) {   // lifted before it moved or was held: a tap
                    h.removeCallbacks(holdTimer);
                    if (maxFingers == 1) { click(sx, sy); note("tap"); }
                    st = n > 0 ? WAIT : IDLE; break;
                }
                if (n >= 2) { h.removeCallbacks(holdTimer); enterTwo(n, ids, xs, ys, now); break; }
                if (Math.hypot(xs[pi] - sx, ys[pi] - sy) > slop) {
                    h.removeCallbacks(holdTimer);
                    press(MotionEvent.BUTTON_PRIMARY, sx, sy); move(xs[pi], ys[pi]); st = DRAG; note("drag");
                }
                break;
            case DRAG:
                if (pi < 0) { release(); st = n > 0 ? WAIT : IDLE; } else move(xs[pi], ys[pi]);
                break;
            case HOLD:
                if (pi < 0) { release(); st = n > 0 ? WAIT : IDLE; } else move(xs[pi], ys[pi]);
                break;
            case TWO:
                maxFingers = Math.max(maxFingers, n);
                if (pi < 0 || si < 0) {
                    h.removeCallbacks(twoHoldTimer);
                    if (midDown) { release(); midDown = false; }
                    else if (!twoMoved && now - t0 < TWO_TAP_MS) {
                        if (maxFingers >= 3) { key(KeyEvent.KEYCODE_ESCAPE); note("three-finger tap -> Escape"); }
                        // pause: the console build does not listen to Space (phone run 20: the key arrives, nothing happens); its pause is the pad's Y
                        else { out.gameAction(2); note("two-finger tap -> pause (TimeController.SafePaused, natively)"); }
                    }
                    st = n > 0 ? WAIT : IDLE; break;
                }
                float cx = (xs[pi] + xs[si]) / 2, cy = (ys[pi] + ys[si]) / 2, spread = (float) Math.hypot(xs[pi] - xs[si], ys[pi] - ys[si]);
                if (!midDown && Math.hypot(cx - mid0x, cy - mid0y) > slop) { press(MotionEvent.BUTTON_TERTIARY, mid0x, mid0y); midDown = true; twoMoved = true; note("two-finger drag -> middle button (pan)"); }
                if (midDown) move(cx, cy);
                if (pinchBase > 1) {
                    float r = spread / pinchBase;
                    if (r > 1.15f) { key(KeyEvent.KEYCODE_EQUALS); pinchBase = spread; twoMoved = true; note("pinch out -> ="); }
                    else if (r < 0.87f) { key(KeyEvent.KEYCODE_MINUS); pinchBase = spread; twoMoved = true; note("pinch in -> -"); }
                }
                break;
            case WAIT:
                if (n == 0) st = IDLE;
                break;
        }
    }
    private void enterTwo(int n, int[] ids, float[] xs, float[] ys, long now) {
        int pi = index(n, ids, prim); if (pi < 0) { st = WAIT; return; }
        int si = pi == 0 ? 1 : 0; sec = ids[si];
        mid0x = (xs[pi] + xs[si]) / 2; mid0y = (ys[pi] + ys[si]) / 2; pinchBase = (float) Math.hypot(xs[pi] - xs[si], ys[pi] - ys[si]);
        twoMoved = false; midDown = false; maxFingers = Math.max(maxFingers, n); st = TWO;
        h.removeCallbacks(twoHoldTimer); h.postDelayed(twoHoldTimer, holdMs + 100);
    }
    // everything let go (the pause menu opens, the app loses focus, the game goes to its console input)
    void reset() {
        h.removeCallbacks(holdTimer); h.removeCallbacks(twoHoldTimer);
        if (buttons != 0) release();
        st = IDLE; prim = sec = -1; midDown = false;
    }
    private static int index(int n, int[] ids, int id) { if (id < 0) return -1; for (int i = 0; i < n; i++) if (ids[i] == id) return i; return -1; }

    // ---- the mouse
    private void hover(float x, float y) { mx = x; my = y; if (buttons == 0) mouse(MotionEvent.ACTION_HOVER_MOVE, SystemClock.uptimeMillis()); }
    private void press(int button, float x, float y) {
        if (buttons != 0) release();
        mx = x; my = y; mouse(MotionEvent.ACTION_HOVER_MOVE, SystemClock.uptimeMillis());
        buttons = button; downTime = SystemClock.uptimeMillis(); mouse(MotionEvent.ACTION_DOWN, downTime);
    }
    private void move(float x, float y) { if (x == mx && y == my) return; mx = x; my = y; mouse(buttons != 0 ? MotionEvent.ACTION_MOVE : MotionEvent.ACTION_HOVER_MOVE, SystemClock.uptimeMillis()); }
    private void release() {
        if (buttons == 0) return;
        int was = buttons; long now = SystemClock.uptimeMillis();
        // the button is still in the state of the UP event's predecessor; UP itself carries none
        buttons = 0; mouse(MotionEvent.ACTION_UP, now, was);
        mouse(MotionEvent.ACTION_HOVER_MOVE, now);
    }
    private void click(float x, float y) {
        press(MotionEvent.BUTTON_PRIMARY, x, y);
        h.postDelayed(() -> { if (buttons == MotionEvent.BUTTON_PRIMARY && st != DRAG) release(); }, TAP_MS);
    }
    private void mouse(int action, long now) { mouse(action, now, 0); }
    private void mouse(int action, long now, int releasedButton) {
        MotionEvent.PointerProperties[] pp = {new MotionEvent.PointerProperties()}; MotionEvent.PointerCoords[] pc = {new MotionEvent.PointerCoords()};
        pp[0].id = 0; pp[0].toolType = MotionEvent.TOOL_TYPE_MOUSE;
        pc[0].x = mx; pc[0].y = my; pc[0].pressure = buttons != 0 ? 1 : 0; pc[0].size = 1;
        long dt = action == MotionEvent.ACTION_HOVER_MOVE ? now : downTime;
        MotionEvent e = MotionEvent.obtain(dt, now, action, 1, pp, pc, 0, buttons, 1, 1, 0, 0, InputDevice.SOURCE_MOUSE, 0);
        try { out.inject(e); } finally { e.recycle(); }
    }
    // ---- a key tap (pause, close, zoom)
    private void key(int code) {
        long now = SystemClock.uptimeMillis();
        out.inject(new KeyEvent(now, now, KeyEvent.ACTION_DOWN, code, 0, 0, KeyCharacterMap.VIRTUAL_KEYBOARD, 0, 0, InputDevice.SOURCE_KEYBOARD));
        h.postDelayed(() -> { long t = SystemClock.uptimeMillis(); out.inject(new KeyEvent(now, t, KeyEvent.ACTION_UP, code, 0, 0, KeyCharacterMap.VIRTUAL_KEYBOARD, 0, 0, InputDevice.SOURCE_KEYBOARD)); }, TAP_MS);
    }
    private void selectAll() { out.gameAction(1); note("two fingers held -> select the whole party"); }
    private void note(String what) { if (lines++ < 300) out.log("touch: " + what + " at " + Math.round(mx) + "," + Math.round(my)); }
}
