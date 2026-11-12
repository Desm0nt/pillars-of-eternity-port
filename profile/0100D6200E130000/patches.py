# Pillars of Eternity (0100D6200E130000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
  # Pillars of Eternity 2.60.00 (v393216) main, Unity 2018.1's NVN texture binding (main+0x1a356d0, on the render thread): the same code as Untitled Goose Game's (Unity 2018.4, its
  # patches.py). A first pass over a draw's bindings puts each texture that is ready (an NVN texture ID at +0xd0) in a local array of 32 slots at sp+0x30 (main+0x1a35750) and skips the
  # others; the bindings that take their sampler from another slot (flag bit 10, listed at sp+0x130) then read that slot's texture from the array for its sampler ID (main+0x1a359e8,
  # +0x1a359f0). The array is never cleared, so a slot the first pass skipped holds whatever was on the stack: an older texture of the same call on the Switch, anything on this host
  # (Goose crashed there at address 0xd4 in about 1 launch in 5). The deferred binding now skips an empty slot, as the first pass skips a texture that is not ready (the slot keeps the
  # handle it had). Room for the check: both GOT entries (the device pointer +0xa28, the handle function +0xee0) are on one page, so the second adrp goes; the array base moves to x2,
  # which the call's own argument load overwrites. Found statically (2026-11-11); not seen on a device yet
  '68976c702865e548ad33367b1ed393db': [
    (0x1a359d4, '4a1545f9', '401545f9', 'deferred sampler slot: ldr x10,[x10,#0xa28] → ldr x0,[x10,#0xa28] (x10 keeps the GOT page)'),
    (0x1a359d8, '400140f9', '000040f9', 'deferred sampler slot: ldr x0,[x10] → ldr x0,[x0] (the device)'),
    (0x1a359e0, 'eac30091', 'e2c30091', 'deferred sampler slot: add x10,sp,#0x30 → add x2,sp,#0x30 (the slot array)'),
    (0x1a359e8, '497969f8', '497869f8', 'deferred sampler slot: ldr x9,[x10,x9,lsl #3] → ldr x9,[x2,x9,lsl #3] (the slot\'s texture)'),
    (0x1a359ec, '88f300b0', '290200b4', 'deferred sampler slot: adrp x8,0x38a6000 → cbz x9,0x1a35a30 (an empty slot: next binding)'),
    (0x1a359f4, '087147f9', '487147f9', 'deferred sampler slot: ldr x8,[x8,#0xee0] → ldr x8,[x10,#0xee0] (the handle function\'s GOT entry)'),
    # --- Touch played as the PC mouse (host dm/host/hle_poe_touch.c, DM_POE_TOUCH_MOUSE): evidence and register liveness in work/agent-touch/patches_fragment.py and REPORT.md.
    # get_UsingGamepad: mov x19,x0 moved right after the ActiveDevice call (adrp x20 / ldr x20 one instruction later), the place of observation gate 22; no behaviour of its own
    (0x12efb2c, '942b01b094a644f9f30300aa', 'f30300aa942b01b094a644f9', 'touch mouse: get_UsingGamepad, mov x19,x0 moved right after the ActiveDevice call (adrp x20 / ldr x20 one instruction later) for the OBS_POE_GAMEPAD gate'),
    # --- Wide picture (host dm/host/hle_poe.c, DM_POE_WIDE): Unity's screen apply (main+0x17238d0) refuses a size unless |h/w - 0.5625| <= 1e-6; the tolerance becomes 31.0.
    # Inert alone: only observation gate 29 makes a request that is not 16:9. Evidence in work/agent-wide/patches_fragment.py and REPORT.md
    (0x1723980, '21c94cbd', '01f0271e', 'wide picture: Unity screen apply aspect tolerance ldr s1,[x9,#0xcc8] (1e-6) -> fmov s1,#31.0 (any aspect; 16:9 sizes as before)'),
  ],
}

# Observation gates: one instruction replaced by "host callback, then the original instruction"; numbers = OBS_POE_* in dm/host/dm.h (branch poe of our nx2android clone).
# Without the callbacks, or with their switches off (DM_POE_TOUCH_MOUSE=0, DM_POE_WIDE=0, DM_POE_FPS=0), the gates change nothing. Found statically (2026-11-11), not run on a device
OBS = {
  '68976c702865e548ad33367b1ed393db': [
    # 22-26: touch played as the PC mouse (work/agent-touch/REPORT.md)
    (0x12efb2c, 22, 'aa0003f3', 'touch mouse: get_UsingGamepad, ActiveDevice (x0) right after the call'),
    (0x10a7730, 23, 'd10183ff', 'touch mouse: UICamera.Update entry, NGUI useMouse'),
    (0xc9e5f0, 24, 'd107c3ff', 'touch mouse: CameraControl.DoUpdate entry, no edge scrolling'),
    (0x17d8f00, 25, 'aa0003f4', 'touch mouse: Unity touch-to-mouse step, the host gesture tick'),
    (0x17d91ac, 26, 'bd00c009', 'touch mouse: Unity touch-to-mouse step, the stored mouse position'),
    (0x10aa2c0, 27, 'd102c3ff', 'touch mouse: UICamera.ProcessTouch entry, the press / release NGUI saw (diagnostic)'),
    (0x10aba80, 28, 'b9404e68', 'touch mouse: ProcessTouch release, clickNotification before OnClick (a tap keeps its click)'),
    (0x10abbb0, 31, 'f94002e8', 'touch mouse: ProcessTouch, right after Notify OnClick (diagnostic)'),
    (0xf8eaa0, 32, 'd101c3ff', 'touch mouse: UIMainMenuClickHandler.OnClick entry (diagnostic)'),
    # 29-30: picture aspect and present interval (work/agent-wide/REPORT.md)
    (0x17238d0, 29, 'f81b0ff9', 'Unity screen manager apply (w1 width, w2 height): request -> the screen aspect (DM_POE_WIDE)'),
    (0x1a2bd20, 30, 'b9804c08', 'Unity quality change: before vSyncCount -> present interval (DM_POE_FPS)'),
    (0x1a38cf8, 30, 'b9804c08', 'Unity NVN window creation: before vSyncCount -> builder present interval (DM_POE_FPS)'),
  ],
}
