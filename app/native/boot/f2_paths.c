/* F2 data folders and the path redirect.
 *
 * The engine (libunity) asks for "<apk>/assets/bin/Data/<rel>" (data folder = application path + assets/bin/Data);
 * such entries are not in the APK, so its APK file system declines and the local (POSIX) file system opens the
 * path through libc -- where our import hooks (f2_hooks.c) call f2_redirect():
 *     <apk>/assets/bin/Data/<rel>   ->  <overlay>/<rel>   if the overlay has that file
 *                                   ->  <data dir>/<rel>  otherwise (the 15 GB tree, never written)
 *     <data dir>/<rel>              ->  <overlay>/<rel>   if the overlay has it (AssetBundle.LoadFromFile with the
 *                                       managed streamingAssetsPath, which is <data dir>/StreamingAssets)
 *     either form                   ->  <overlay-astc>/<rel> before both when the ASTC texture set is in use (f2_texset.c)
 * Files that really are in the APK (boot.config, Managed/ = il2cpp metadata) never reach libc with that path. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "f2_boot.h"

#define MAX_OVERLAY 256
static char *s_overlay[MAX_OVERLAY];
static int s_noverlay;
static size_t s_apk_len, s_data_len, s_real_len;

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void scan_overlay(const char *dir, const char *rel)
{
    char p[1024];
    snprintf(p, sizeof p, "%s%s%s", dir, rel[0] ? "/" : "", rel);
    DIR *d = opendir(p);
    if (!d) { if (!rel[0]) f2_log("F2: overlay folder %s: %s", p, strerror(errno)); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char r[700], full[1100];
        snprintf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        snprintf(full, sizeof full, "%s/%s", dir, r);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) scan_overlay(dir, r);
        else if (s_noverlay < MAX_OVERLAY) {
            s_overlay[s_noverlay++] = strdup(r);
            f2_log("F2: overlay file %s (%lld bytes)", r, (long long)st.st_size);
        }
    }
    closedir(d);
}

static int overlay_has(const char *rel)
{
    int lo = 0, hi = s_noverlay;
    while (lo < hi) {
        int mid = (lo + hi) / 2, c = strcmp(rel, s_overlay[mid]);
        if (!c) return 1;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return 0;
}

static int try_data_dir(const char *dir)
{
    char p[700];
    struct stat st;
    snprintf(p, sizeof p, "%s/level0", dir);
    int e1 = stat(dir, &st) ? errno : (S_ISDIR(st.st_mode) ? 0 : ENOTDIR);
    int e2 = e1 ? e1 : (access(p, R_OK) ? errno : 0);
    f2_log("F2: data folder candidate %s: %s%s", dir, e2 ? strerror(e2) : "readable",
           e2 == EACCES ? "  <-- the app has no storage access to it (see HANDOVER: grant / move the folder)" : "");
    return e2 == 0;
}

void f2_paths_init(void)
{
    f2_config_t *c = &f2_cfg;
    if (!c->overlay_dir[0]) snprintf(c->overlay_dir, sizeof c->overlay_dir, "%s/overlay/Data", c->files_dir);
    if (c->data_dir[0]) c->data_ok = try_data_dir(c->data_dir);
    else {
        const char *cand[4];
        char own[600];
        snprintf(own, sizeof own, "%s/RomFS/Data", c->files_dir);
        cand[0] = own;
        cand[1] = "/storage/emulated/0/PillarsMobile/RomFS/Data";
        cand[2] = "/sdcard/PillarsMobile/RomFS/Data";
        cand[3] = "/storage/emulated/0/Download/PillarsMobile/RomFS/Data";
        for (int i = 0; i < 4 && !c->data_ok; i++)
            if (try_data_dir(cand[i])) { snprintf(c->data_dir, sizeof c->data_dir, "%s", cand[i]); c->data_ok = 1; }
        if (!c->data_ok) snprintf(c->data_dir, sizeof c->data_dir, "%s", own);
    }
    if (!realpath(c->data_dir, c->data_real)) snprintf(c->data_real, sizeof c->data_real, "%s", c->data_dir);
    if (c->apk_path[0]) snprintf(c->apk_data_prefix, sizeof c->apk_data_prefix, "%s/assets/bin/Data", c->apk_path);
    s_apk_len = strlen(c->apk_data_prefix);
    s_data_len = strlen(c->data_dir);
    s_real_len = strlen(c->data_real);
    scan_overlay(c->overlay_dir, "");
    qsort(s_overlay, s_noverlay, sizeof s_overlay[0], cmp_str);
    int need[] = {overlay_has("globalgamemanagers"), overlay_has("unity default resources") || c->defres_path[0] /* the apk's copy */, overlay_has("resources.assets"),
                  overlay_has("StreamingAssets/assetbundles/objectbundle")};
    if (c->data_ok)
        f2_once_milestone(M_DATA, "data folder %s (real %s), overlay %s: %d files%s", c->data_dir, c->data_real, c->overlay_dir,
                          s_noverlay, (need[0] && need[1] && need[2] && need[3]) ? "" : "  [RED: overlay incomplete: the engine will read Switch-format files]");
    else
        f2_log("F2: [RED] game Data folder NOT readable (tried the candidates above; set data_dir= in %s/f2_config.txt). "
               "The engine will fail to load globalgamemanagers.", c->files_dir);
    if (!s_noverlay)
        f2_log("F2: [RED] overlay folder %s is empty or missing: push work/f2-app/overlay/Data there", c->overlay_dir);
    f2_texset_init();
}

static int has_prefix(const char *path, const char *pfx, size_t n)
{
    return n && !strncmp(path, pfx, n) && (path[n] == '/' || path[n] == 0);
}

const char *f2_redirect(const char *path, char *buf, size_t n, int *kind)
{
    *kind = 0;
    if (!path || path[0] != '/') return path;
    const char *rel = NULL;
    int from_apk = 0;
    if (has_prefix(path, f2_cfg.apk_data_prefix, s_apk_len)) { rel = path + s_apk_len; from_apk = 1; }
    else if (has_prefix(path, f2_cfg.data_dir, s_data_len)) rel = path + s_data_len;
    else if (has_prefix(path, f2_cfg.data_real, s_real_len)) rel = path + s_real_len;
    if (!rel) return path;
    while (*rel == '/') rel++;
    if (*rel) {
        /* the ASTC set first: its files are built from the overlay's copy where the overlay has one (tools/data/astc_set.py) */
        const char *a = f2_texset_redirect(rel, overlay_has(rel), buf, n);
        if (a) { *kind = 3; return a; }
    }
    if (*rel && overlay_has(rel)) {
        snprintf(buf, n, "%s/%s", f2_cfg.overlay_dir, rel);
        *kind = 1;
        return buf;
    }
    /* "unity default resources" (Data root; distinct from the Switch's own "Resources/unity default resources"): the Android
     * player's own asset, not in any Switch dump. Older overlays (built with a local Unity editor, PROGRESS_data.txt) still carry
     * it and are matched by overlay_has() above; a clean-APK setup folder's overlay does not, so DmActivity's one-time extraction
     * of assets/f2/unity_default_resources (f2_front.txt defres_path=) is the last resort. */
    if (!strcmp(rel, "unity default resources") && f2_cfg.defres_path[0]) {
        snprintf(buf, n, "%s", f2_cfg.defres_path);
        *kind = 4;
        return buf;
    }
    if (!from_apk) return path;      /* already a data-folder path */
    snprintf(buf, n, "%s%s%s", f2_cfg.data_dir, *rel ? "/" : "", rel);
    *kind = 2;
    return buf;
}
