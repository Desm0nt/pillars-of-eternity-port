/* F2: what happens to the pictures the game loads from loose files at run time (GUIUtils.LoadTexture2DFromPath: portraits of the player's
 * characters, StreamingAssets/Data/art/gui/Portraits/..., read with System.IO and handed to the engine's ImageConversion.LoadImage).
 * Owner, 12.11.2026: the player's portrait is garbage (large) / another character's picture (small); the save's thumbnail of it is garbage in
 * every save, the companion's (a bundle asset) is fine.  The first lines of each kind go to the log, so a tester's log shows which step fails:
 *   "texload: path ..."      the game asked for a file            (gate at the entry of GUIUtils.LoadTexture2DFromPath, main+0xf72010)
 *   "texload: file exists"   System.IO.File.Exists said yes       (gate at main+0xf72194, reached only then)
 *   "texload: LoadImage ..." the engine's decoder: bytes in, result out (icall override, the engine's own function is called) */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "f2_boot.h"

void il2cpp_add_internal_call(const char *name, void (*method)(void));
typedef void (*f2_gate_hook)(uint64_t *regs);

static unsigned s_n_path, s_n_exists, s_n_load;
static int s_same;          /* the request in hand repeats the previous one: its lines are skipped */
#define CAP 24

static void str_utf8(uint64_t s, char *out, size_t n)       /* Il2CppString: int32 length at +0x10, UTF-16 at +0x14 */
{
    out[0] = 0;
    if (!s) { snprintf(out, n, "(null)"); return; }
    int32_t len = *(int32_t *)(uintptr_t)(s + 0x10);
    const uint16_t *c = (const uint16_t *)(uintptr_t)(s + 0x14);
    size_t o = 0;
    for (int32_t i = 0; i < len && i < 400 && o + 4 < n; i++) {
        uint16_t ch = c[i];
        if (ch < 0x80) out[o++] = (char)ch;
        else if (ch < 0x800) { out[o++] = (char)(0xc0 | (ch >> 6)); out[o++] = (char)(0x80 | (ch & 0x3f)); }
        else { out[o++] = (char)(0xe0 | (ch >> 12)); out[o++] = (char)(0x80 | ((ch >> 6) & 0x3f)); out[o++] = (char)(0x80 | (ch & 0x3f)); }
    }
    out[o] = 0;
}

static void gate_path(uint64_t *regs)
{
    /* a static method of the generated code: x0 is unused (null), the arguments start at x1 = path, w2 / w3 = size, x4 = texture.
     * The same picture is asked for again and again (every portrait refresh): a repeat of the last path is not logged. */
    static uint64_t last_hash;
    char p[700];
    str_utf8(regs[1], p, sizeof p);
    uint64_t h = 1469598103934665603ull;
    for (const char *c = p; *c; c++) h = (h ^ (uint8_t)*c) * 1099511628211ull;
    s_same = h == last_hash;
    last_hash = h;
    if (s_same || s_n_path++ >= CAP) return;
    f2_log("F2: texload: path %s  (%d x %d asked, texture %s)", p, (int)regs[2], (int)regs[3], regs[4] ? "given" : "new");
}
static void gate_exists(uint64_t *regs)
{
    if (s_same || s_n_exists++ >= CAP) return;
    char p[700];
    str_utf8(regs[20], p, sizeof p);           /* x20 = the combined path at that point */
    f2_log("F2: texload: file exists: %s", p);
}
void f2_texdiag_gates(void (*add)(uint32_t off, uint32_t expect, f2_gate_hook hook, const char *what))
{
    add(0xf72010, 0xf81c0ff7u, gate_path, "texload (GUIUtils.LoadTexture2DFromPath)");
    add(0xf72194, 0xaa1f03e0u, gate_exists, "texload (after File.Exists)");
}

static uint8_t (*s_load_image)(void *tex, void *data, uint8_t non_readable);
static uint8_t ov_load_image(void *tex, void *data, uint8_t non_readable)
{
    uint8_t r = s_load_image(tex, data, non_readable);
    if ((!s_same || r == 0) && s_n_load++ < CAP) {        /* a failure is always worth its line */
        /* Il2CppArray: object header 16 bytes, bounds 8, max_length 8, then the elements */
        uint64_t len = data ? *(uint64_t *)((uint8_t *)data + 0x18) : 0;
        const uint8_t *b = data ? (const uint8_t *)data + 0x20 : (const uint8_t *)"\0\0\0\0\0\0\0\0";
        f2_log("F2: texload: LoadImage(texture %p, %llu bytes: %02x %02x %02x %02x %02x %02x %02x %02x, markNonReadable %d) -> %d", tex,
               (unsigned long long)len, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], non_readable, r);
    }
    return r;
}
void f2_texdiag_engine_icall(const char *name, void (*fn)(void))
{
    if (!name || !fn) return;
    if (!strcmp(name, "UnityEngine.ImageConversion::LoadImage")) {
        s_load_image = (uint8_t (*)(void *, void *, uint8_t))fn;
        il2cpp_add_internal_call("UnityEngine.ImageConversion::LoadImage(UnityEngine.Texture2D,System.Byte[],System.Boolean)", (void (*)(void))ov_load_image);
        f2_log("F2: texload: the engine's ImageConversion.LoadImage is at %p (wrapped for the log)", (void *)fn);
    }
}
