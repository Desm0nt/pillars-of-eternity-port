/* F2 touch as the PC mouse, the game's side (the port's hle_poe_touch.c, work/agent-touch/REPORT.md, reduced to what the Android player
 * does not do by itself).
 *
 * The Switch build keeps the PC input path next to the console one and one test picks it: GameInput.get_UsingGamepad (main+0x12efa90) =
 * InControl.InputManager.ActiveDevice != InputDevice.Null.  With "false" the game reads UnityEngine.Input's mouse and keys (PC bindings:
 * select / move / interact = Mouse0, rotate formation = Mouse1, pan = Mouse2, pause = Space, zoom = "=" / "-", close = Esc).  Unlike the
 * Switch player, the Android player HAS a mouse and a keyboard: the Java front end turns the fingers the pad leaves to the game into mouse
 * and key events for the Unity player (DmActivity / F2Touch.java), so nothing of Unity's input has to be emulated here.  What is left:
 *   gate "gamepad"   main+0x12efb34 (mov x19, x0 two instructions after the get_ActiveDevice call; the port moved it to +0x12efb2c with a cut): x0 = InputDevice.Null in touch mode
 *   gate "uicamera"  main+0x10a7730, entry of UICamera.Update (x0 = the UICamera): useMouse (+0x21) = 1 in touch mode (the scenes have
 *                    useMouse 0, useTouch 0, useController 1), its own value back in gamepad mode; the console cursors' flags
 *                    (GameInput statics +0x82 s_CursorWorldMap, +0x83 s_VirtualCursor) are cleared so GameInput.MousePosition is the mouse
 *   gate "camera"    main+0xc9e5f0, entry of CameraControl.DoUpdate (x0 = the CameraControl): EdgeScrollTempDisable (+0x141) = 1 in touch
 *                    mode (the "mouse" stays where the finger was lifted; the game would scroll when that is near an edge)
 * Mode: the Java side says which one is current (nativeTouchMode): a finger on the picture = touch, a pad button / stick or a controller =
 * gamepad (the console input) until the next touch.  f2_config.txt touch=0 switches all of it off (the stock console input). */
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include "f2_boot.h"

uintptr_t f2_game_base(void);
typedef void (*f2_gate_hook)(uint64_t *regs);

enum { M_INPUTDEVICE_TI = 0x39b3e58,   /* InControl.InputDevice_TypeInfo (Il2CppClass *), static field 0 = InputDevice.Null */
       M_GAMEINPUT_TI = 0x39b2d38,     /* GameInput_TypeInfo: statics +0x82 s_CursorWorldMap, +0x83 s_VirtualCursor */
       M_GAMECURSOR_TI = 0x39b2d40,    /* GameCursor_TypeInfo: static 0 = GameCursor.Instance; +0x28 m_ObjectsHidingCursor, +0x30 DisableCursor, +0x31 ShowCursor */
       GC_HIDING = 0x28, GC_DISABLE = 0x30, GC_SHOW = 0x31,
       CLS_STATIC = 0xb8, GI_WORLDMAP_CURSOR = 0x82, GI_VIRTUAL_CURSOR = 0x83, UICAM_USE_MOUSE = 0x21, CAM_EDGE_OFF = 0x141 };

static volatile int s_touch_mode;
static unsigned long s_hits[3];

void f2_touch_set_mode(int touch)
{
    touch = touch ? 1 : 0;
    if (s_touch_mode == touch) return;
    s_touch_mode = touch;
    static int n;
    if (n++ < 60) f2_log("F2: touch: mode %s (gate hits: gamepad %lu, uicamera %lu, camera %lu)", touch ? "touch (the PC mouse path)" : "gamepad (the console input)",
                         s_hits[0], s_hits[1], s_hits[2]);
}
static int touch_on(void) { return f2_cfg.touch && s_touch_mode; }
static uint64_t rd64(uint64_t a) { return a ? *(volatile uint64_t *)(uintptr_t)a : 0; }
static uint64_t statics(uint32_t ti) { uint64_t cls = rd64(f2_game_base() + ti); return cls ? rd64(cls + CLS_STATIC) : 0; }

/* ---- the engine's own view of the input, for the diagnostics (f2_config.txt touch_log=1): engine icalls kept at registration */
void il2cpp_add_internal_call(const char *name, void (*method)(void));
static uint8_t (*s_mouse_button)(int32_t);
static uint8_t (*s_key_int)(int32_t);
static void (*s_mouse_pos)(float *xyz);
static int32_t (*s_touch_count)(void);
static void *(*s_obj_name)(void *obj);
static void (*s_send_message)(void *go, void *name, void *value, int32_t options);
static void (*s_send_message_up)(void *go, void *name, void *value, int32_t options);
static void str8(void *str, char *out, int cap)       /* Il2CppString: length +0x10, UTF-16 +0x14 */
{
    int n = str ? *(int32_t *)((char *)str + 0x10) : 0, i;
    for (i = 0; i < n && i < cap - 1; i++) { uint16_t c = *(uint16_t *)((char *)str + 0x14 + 2 * i); out[i] = c < 0x80 ? (char)c : '?'; }
    out[i] = 0;
}
static const char *obj_name(uint64_t obj, char *out, int cap)
{
    out[0] = 0;
    if (obj && s_obj_name && rd64(obj + 0x10)) str8(s_obj_name((void *)(uintptr_t)obj), out, cap);
    return out;
}
static int s_tl;
/* the object hierarchy, for the diagnostics: "name <- parent <- grandparent ..." of a GameObject, and its children's names */
static void *(*s_comp_go)(void *component);
static void *(*s_go_transform)(void *go);
static void *(*s_tr_parent)(void *transform);
static void *(*s_tr_child)(void *transform, int32_t i);
static int32_t (*s_tr_child_count)(void *transform);
static void chain_of(void *go, char *out, int cap)
{
    int w = 0; out[0] = 0;
    if (!go || !s_go_transform || !s_tr_parent || !s_comp_go) return;
    void *tr = s_go_transform(go);
    for (int i = 0; i < 6 && tr && w < cap - 60; i++) {
        char n[48];
        void *g = s_comp_go(tr);
        w += snprintf(out + w, cap - w, "%s\"%s\"", i ? " <- " : "", obj_name((uint64_t)(uintptr_t)g, n, sizeof n));
        tr = s_tr_parent(tr);
    }
}
static void children_of(void *go, char *out, int cap)
{
    int w = 0; out[0] = 0;
    if (!go || !s_go_transform || !s_tr_child || !s_tr_child_count || !s_comp_go) return;
    void *tr = s_go_transform(go);
    int n = tr ? s_tr_child_count(tr) : 0;
    for (int i = 0; i < n && i < 10 && w < cap - 60; i++) {
        char nm[48];
        void *c = s_tr_child(tr, i);
        w += snprintf(out + w, cap - w, "%s\"%s\"", i ? ", " : "", obj_name((uint64_t)(uintptr_t)(c ? s_comp_go(c) : NULL), nm, sizeof nm));
    }
}
/* GameObject.SendMessage as the game calls it: which object gets "On...Click" / "OnPress" and what it is called */
static void ov_send_message(void *go, void *name, void *value, int32_t options)
{
    /* The main menu's rows keep their collider on a child called "ButtonHighlight", while the script that acts (UIMainMenuClickHandler) sits on
     * its SIBLING "ButtonTexture" (phone run 22: "4OptionsButton" has the children "ButtonTexture" and "ButtonHighlight").  The console never
     * clicks with a pointer: its menu code calls the handler itself.  In touch mode a click that lands on such a highlight is sent to every child
     * of its parent (the highlight itself included). */
    if (f2_cfg.touch && s_touch_mode && name && go && s_go_transform && s_tr_parent && s_tr_child && s_tr_child_count && s_comp_go) {
        char m[24], n[40];
        str8(name, m, sizeof m);
        if (!strcmp(m, "OnClick") && !strcmp(obj_name((uint64_t)(uintptr_t)go, n, sizeof n), "ButtonHighlight")) {
            void *tr = s_go_transform(go), *par = tr ? s_tr_parent(tr) : NULL;
            int cnt = par ? s_tr_child_count(par) : 0;
            /* NGUI sends the click of one tap twice here (phone run 23): the second one within 150 ms goes to the highlight alone */
            static void *s_last_go; static struct timespec s_last_t;
            struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            long ms = (now.tv_sec - s_last_t.tv_sec) * 1000 + (now.tv_nsec - s_last_t.tv_nsec) / 1000000;
            int again = s_last_go == go && ms >= 0 && ms < 150;
            s_last_go = go; s_last_t = now;
            if (par && cnt > 0 && cnt <= 16 && !again) {
                static int lg;
                if (lg++ < 20) f2_log("F2: touch: click on a \"ButtonHighlight\": sent to the %d children of its parent", cnt);
                void *kids[16];
                for (int k = 0; k < cnt; k++) { void *c = s_tr_child(par, k); kids[k] = c ? s_comp_go(c) : NULL; }   /* the list first: a handler may change the hierarchy */
                for (int k = 0; k < cnt; k++) if (kids[k]) s_send_message(kids[k], name, value, options);
                return;
            }
        }
    }
    if (f2_cfg.touch_log && s_touch_mode && s_tl < 600) {
        char m[64], n[120];
        str8(name, m, sizeof m);
        if (!strncmp(m, "On", 2) && (strstr(m, "Click") || !strcmp(m, "OnPress") || !strcmp(m, "OnSelect"))) {
            char ch[400];
            s_tl++;
            chain_of(go, ch, sizeof ch);
            f2_log("F2: touch log: SendMessage(\"%s\") to GameObject %p: %s", m, go, ch);
        }
    }
    s_send_message(go, name, value, options);
}
void f2_touch_engine_icall(const char *name, void (*fn)(void))
{
    if (!strcmp(name, "UnityEngine.Input::GetMouseButton")) s_mouse_button = (uint8_t (*)(int32_t))fn;
    else if (!strcmp(name, "UnityEngine.Input::GetKeyInt")) s_key_int = (uint8_t (*)(int32_t))fn;
    else if (!strcmp(name, "UnityEngine.Input::INTERNAL_get_mousePosition")) s_mouse_pos = (void (*)(float *))fn;
    else if (!strcmp(name, "UnityEngine.Input::get_touchCount")) s_touch_count = (int32_t (*)(void))fn;
    else if (!strcmp(name, "UnityEngine.Object::GetName")) s_obj_name = (void *(*)(void *))fn;
    else if (!strcmp(name, "UnityEngine.Component::get_gameObject")) s_comp_go = (void *(*)(void *))fn;
    else if (!strcmp(name, "UnityEngine.GameObject::get_transform")) s_go_transform = (void *(*)(void *))fn;
    else if (!strcmp(name, "UnityEngine.Transform::get_parentInternal")) s_tr_parent = (void *(*)(void *))fn;
    else if (!strcmp(name, "UnityEngine.Transform::GetChild")) s_tr_child = (void *(*)(void *, int32_t))fn;
    else if (!strcmp(name, "UnityEngine.Transform::get_childCount")) s_tr_child_count = (int32_t (*)(void *))fn;
    else if (!strcmp(name, "UnityEngine.GameObject::SendMessageUpwards")) s_send_message_up = (void (*)(void *, void *, void *, int32_t))fn;
    else if (!strcmp(name, "UnityEngine.GameObject::SendMessage")) {
        s_send_message = (void (*)(void *, void *, void *, int32_t))fn;
        il2cpp_add_internal_call("UnityEngine.GameObject::SendMessage(System.String,System.Object,UnityEngine.SendMessageOptions)", (void (*)(void))ov_send_message);
    }
}
/* once a frame (UICamera.Update): the mouse buttons, Space and the position as UnityEngine.Input reports them; a line when they change */
static void input_state_log(void)
{
    if (!s_mouse_button || !s_mouse_pos || s_tl > 600) return;
    static int last = -1;
    float p[3] = {0, 0, 0};
    int b = (s_mouse_button(0) & 1) | (s_mouse_button(1) & 1) << 1 | (s_mouse_button(2) & 1) << 2 | (s_key_int ? (s_key_int(32) & 1) << 3 : 0)
            | (s_key_int ? (s_key_int(27) & 1) << 4 : 0);
    int tc = s_touch_count ? s_touch_count() : -1;
    int cur = b | (tc > 0 ? 1 << 8 : 0);
    if (cur == last) return;
    last = cur; s_tl++;
    s_mouse_pos(p);
    f2_log("F2: touch log: Input: mouse buttons L%d R%d M%d, Space %d, Escape %d, mouse %.0f,%.0f, touches %d", b & 1, b >> 1 & 1, b >> 2 & 1, b >> 3 & 1, b >> 4 & 1, p[0], p[1], tc);
}

/* ---- game actions asked for by a gesture, run on the main thread at the next UICamera.Update (the gate below).
 * 2 = toggle the game's pause (TimeController.SafePaused).
 * 1 = select the whole party: PartyMemberAI.SelectAll() (static, main+0xf31060).  The console build has no pointer way to do it: the PC's
 * drag box (Player.PerformSelection, main+0x1229630) has no caller left in this build, and a tap on a portrait selects one member. */
void f2_screen_reapply_fps(void);
static volatile int s_pending_action;
void f2_touch_game_action(int id) { s_pending_action = id; }
static void run_pending_action(void)
{
    int id = __atomic_exchange_n(&s_pending_action, 0, __ATOMIC_RELAXED);
    if (id == 1) {
        ((void (*)(void *))(f2_game_base() + 0xf31060))(NULL);
        static int lg;
        if (lg++ < 10) f2_log("F2: touch: PartyMemberAI.SelectAll() (two fingers held)");
    } else if (id == 3) {
        f2_screen_reapply_fps();      /* the display's refresh rate became known or changed (DmActivity.nativeDisplayHz) */
    } else if (id == 2) {
        /* pause: neither Space (the PC binding) nor the pad's Y pressed for the game toggles it while the game is on its PC input path
         * (owner, 12.11.2026).  The game's own switch is called instead: TimeController.Instance.SafePaused = !SafePaused
         * (get_Instance main+0xec5b50, get_SafePaused main+0xec72e0, set_SafePaused main+0xec6bc0; SafePaused does nothing where the game
         * does not allow a pause: cutscenes, loading). */
        void *tc = ((void *(*)(void *))(f2_game_base() + 0xec5b50))(NULL);
        static int lg;
        if (tc) {
            int was = ((uint8_t (*)(void *, void *))(f2_game_base() + 0xec72e0))(tc, NULL) & 1;
            ((void (*)(void *, uint8_t, void *))(f2_game_base() + 0xec6bc0))(tc, was ? 0 : 1, NULL);
            if (lg++ < 20) f2_log("F2: touch: TimeController.SafePaused %d -> %d (two-finger tap)", was, !was);
        } else if (lg++ < 20) f2_log("F2: touch: pause asked, but there is no TimeController yet");
    }
}

static void gate_gamepad(uint64_t *regs)
{
    s_hits[0]++;
    if (!touch_on()) return;
    uint64_t s = statics(M_INPUTDEVICE_TI), null = s ? rd64(s) : 0;
    if (null) regs[0] = null;
}

static struct { uint64_t cam; uint8_t orig; } s_uic[32];
static int s_nuic;
static void gate_uicamera(uint64_t *regs)
{
    s_hits[1]++;
    if (!f2_cfg.touch) return;
    uint64_t c = regs[0];
    if (!c) return;
    if (f2_cfg.touch_log && s_touch_mode) input_state_log();
    if (s_pending_action) run_pending_action();
    uint8_t *m = (uint8_t *)(uintptr_t)(c + UICAM_USE_MOUSE);
    int k = -1;
    for (int i = 0; i < s_nuic; i++) if (s_uic[i].cam == c) { k = i; break; }
    if (s_touch_mode) {
        if (k < 0 && !*m) {
            if (s_nuic == 32) s_nuic = 0;            /* scenes come and go: an old camera's row is reused (its object is gone) */
            s_uic[s_nuic].cam = c; s_uic[s_nuic++].orig = 0;
            static int lg;
            if (lg++ < 8) f2_log("F2: touch: UICamera %#llx: useMouse on (useTouch %d, useController %d)", (unsigned long long)c, m[1], m[4]);
        }
        *m = 1;
        uint64_t gi = statics(M_GAMEINPUT_TI);
        if (gi) { *(uint8_t *)(uintptr_t)(gi + GI_WORLDMAP_CURSOR) = 0; *(uint8_t *)(uintptr_t)(gi + GI_VIRTUAL_CURSOR) = 0; }
        /* GameCursor.DisableCursor: UICamera.ProcessMouse (main+0x10a8b20) takes Input.GetMouseButtonDown only while it is off, and
         * GameCursor.Update sets it from m_ObjectsHidingCursor (the title intro, loading screens, cutscenes, the HUD) unless a window is
         * open.  The console never shows the hardware cursor, so a request left in that list hides nothing there; with the mouse it
         * swallows every press while hover keeps working (phone run 17: ProcessTouch saw the release only).  Off before each ProcessMouse. */
        uint64_t gcs = statics(M_GAMECURSOR_TI), gc = gcs ? rd64(gcs) : 0;
        if (gc && *(uint8_t *)(uintptr_t)(gc + GC_DISABLE) == 1) {
            *(uint8_t *)(uintptr_t)(gc + GC_DISABLE) = 0;
            static int lg2;
            if (lg2++ < 12) { uint64_t hid = rd64(gc + GC_HIDING); f2_log("F2: touch: GameCursor.DisableCursor was on (%d objects hiding the cursor, ShowCursor %d): off for touch",
                                                                             hid ? *(int *)(uintptr_t)(hid + 0x18) : -1, *(uint8_t *)(uintptr_t)(gc + GC_SHOW)); }
        }
    } else if (k >= 0) { *m = s_uic[k].orig; s_uic[k] = s_uic[--s_nuic]; }
}

static void gate_camera(uint64_t *regs)
{
    s_hits[2]++;
    if (!touch_on() || !regs[0]) return;
    *(uint8_t *)(uintptr_t)(regs[0] + CAM_EDGE_OFF) = 1;
}

/* ---- diagnostics (f2_config.txt touch_log=1): what NGUI makes of the mouse.  UICamera statics (class +0xb8): +0x60 currentTouchID,
 * +0x68 currentTouch (MouseOrTouch: +0x20 totalDelta x,y, +0x30 current, +0x38 pressed, +0x40 dragged, +0x4c clickNotification) */
enum { M_UICAMERA_TI = 0x39b3e48 };
static void gate_process_touch(uint64_t *regs)      /* UICamera.ProcessTouch(bool pressed, bool unpressed) entry: x0 this, w1, w2 */
{
    int pressed = (int)(regs[1] & 1), unpressed = (int)(regs[2] & 1);
    if (!pressed && !unpressed) return;
    if (s_tl++ > 400) return;
    uint64_t st = statics(M_UICAMERA_TI), t = st ? rd64(st + 0x68) : 0;
    if (!t) { f2_log("F2: touch log: ProcessTouch(pressed %d, unpressed %d): no currentTouch", pressed, unpressed); return; }
    float dx, dy; memcpy(&dx, (void *)(uintptr_t)(t + 0x20), 4); memcpy(&dy, (void *)(uintptr_t)(t + 0x24), 4);
    char n1[100], n2[100];
    f2_log("F2: touch log: ProcessTouch(pressed %d, unpressed %d) id %d: current %#llx \"%s\" pressed %#llx \"%s\" dragged %#llx clickNotification %d totalDelta %.1f,%.1f",
           pressed, unpressed, *(int *)(uintptr_t)(st + 0x60), (unsigned long long)rd64(t + 0x30), obj_name(rd64(t + 0x30), n1, sizeof n1),
           (unsigned long long)rd64(t + 0x38), obj_name(rd64(t + 0x38), n2, sizeof n2),
           (unsigned long long)rd64(t + 0x40), *(int *)(uintptr_t)(t + 0x4c), dx, dy);
}
static void gate_click_sent(uint64_t *regs) { if (s_tl++ < 400) f2_log("F2: touch log: NGUI sent On...Click to object %#llx", (unsigned long long)regs[19]); }
static void gate_menu_refresh(uint64_t *regs)     /* UIMainMenuClickHandler.RefreshState entry (from OnEnable): where each handler sits */
{
    static int n;
    if (n++ >= 16 || !s_comp_go) return;
    void *go = s_comp_go((void *)(uintptr_t)regs[0]);
    char ch[400], kids[400];
    chain_of(go, ch, sizeof ch); children_of(go, kids, sizeof kids);
    f2_log("F2: touch log: UIMainMenuClickHandler %#llx (type %d) on %s; children: %s", (unsigned long long)regs[0], *(int *)(uintptr_t)(regs[0] + 0x38), ch, kids);
}
static void gate_menu_click(uint64_t *regs) { if (s_tl++ < 400) f2_log("F2: touch log: UIMainMenuClickHandler.OnClick(%#llx)", (unsigned long long)regs[0]); }

/* gate "click": UICamera.ProcessTouch main+0x10aba80 (ldr w8, [x19, #0x4c]; x19 = currentTouch): the release of a press, deciding by
 * currentTouch.clickNotification (0 None, 1 Always, 2 BasedOnDelta) whether On...Click is sent.  The mouse gets 2 and loses it once
 * totalDelta passes the click threshold; a finger tap that NGUI saw pressed and released on one object keeps its click (the port's PT_CLICK) */
static void gate_click_note(uint64_t *regs)
{
    if (!touch_on()) return;
    uint64_t t = regs[19], uc = statics(0x39b3e48);
    if (!t || !uc) return;
    int id = *(int *)(uintptr_t)(uc + 0x60), *note = (int *)(uintptr_t)(t + 0x4c);
    if (*note == 0 && id == -1) {
        *note = 1;
        static int lg;
        if (lg++ < 20) f2_log("F2: touch: NGUI had dropped the click of a tap: sent");
    }
}

/* the loader asks for the gates while the game image is still writable (native/loader/f2_loader.c) */
void f2_touch_gates(void (*add)(uint32_t off, uint32_t expect, f2_gate_hook hook, const char *what))
{
    if (!f2_cfg.touch) { f2_log("F2: touch: off by f2_config.txt (the stock console input)"); return; }
    add(0x12efb34, 0xaa0003f3u, gate_gamepad, "touch/gamepad (GameInput.get_UsingGamepad)");
    add(0x10a7730, 0xd10183ffu, gate_uicamera, "touch/uicamera (UICamera.Update)");
    add(0xc9e5f0, 0xd107c3ffu, gate_camera, "touch/camera (CameraControl.DoUpdate)");
    add(0x10aba80, 0xb9404e68u, gate_click_note, "touch/click (UICamera.ProcessTouch release)");
    if (f2_cfg.touch_log) {
        add(0x10aa2c0, 0xd102c3ffu, gate_process_touch, "touch log (UICamera.ProcessTouch)");
        add(0x10abbb0, 0xf94002e8u, gate_click_sent, "touch log (after Notify OnClick)");
        add(0xf8eaa0, 0xd101c3ffu, gate_menu_click, "touch log (UIMainMenuClickHandler.OnClick)");
        add(0xf8e6c0, 0xd10143ffu, gate_menu_refresh, "touch log (UIMainMenuClickHandler.RefreshState)");
    }
}
