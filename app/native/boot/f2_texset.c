/* F2 texture set: the game's own BC textures (DXT1 / DXT5 / BC5) or the ASTC copies made on the PC by tools/data/astc_set.py.
 *
 * The stock 2018.1 player samples a TextureFormat through vk::s_TextureFormatInfo (DXT1 -> BC1_RGB, DXT5 -> BC3, ...); when the VkFormat has
 * no SAMPLED_IMAGE bit in vkGetPhysicalDeviceFormatProperties (vk::Initialize fills vk::s_FormatInfoTable with it) vk::ImageManager::CreateImage
 * walks the fallback chain to RGBA32 and decompresses on the CPU (DecompressNativeTextureFormat): 4x the memory for DXT5, 8x for DXT1, and the
 * decode time at every load.  Mali, PowerVR and stock Xclipse have no BC.
 *
 * <files>/overlay-astc/Data holds every file of the game that has BC textures, with those textures re-encoded as ASTC (other bytes as in the
 * overlay / RomFS copy).  When it is used the redirect (f2_paths.c) takes a file from it before overlay/Data and RomFS/Data.
 *
 * Decision, once at boot, before the player opens any data (f2_paths_init):
 *   f2_config.txt tex_set=astc  -> ASTC set (if the folder has files)       tex_set=bc -> the game's BC textures
 *   otherwise (auto)            -> ASTC set when the folder has files AND the GPU cannot sample BC (BC1 + BC3 not sampleable, or
 *                                  textureCompressionBC = 0) AND it can sample ASTC LDR 4x4 / 6x6; else BC.
 *   bc_hide=1 (simulation on Adreno): the player is shown a GPU without BC (f2_vkwrap.c) and auto treats the GPU as one without BC.
 *   No overlay-astc folder: no probe at all, BC (behaviour of the builds before this module).
 * The probe is a Vulkan instance of our own (libvulkan.so through dlopen, not the player's wrapped entry points): features + format properties
 * of physical device 0, destroyed again.  A file that is missing from RomFS/Data but present in overlay-astc is taken from overlay-astc in
 * either set (a phone that keeps only the ASTC copies of those files). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <vulkan/vulkan.h>
#include "f2_boot.h"

static char **s_list;
static int s_n, s_cap;
static long long s_bytes;
static char s_dir[512];
static int s_astc;               /* 1: the ASTC set is in use */
unsigned long f2_cnt_astc_opens;

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void scan(const char *dir, const char *rel)
{
    char p[1024];
    snprintf(p, sizeof p, "%s%s%s", dir, rel[0] ? "/" : "", rel);
    DIR *d = opendir(p);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        size_t nl = strlen(e->d_name);
        if (nl > 4 && !strcmp(e->d_name + nl - 4, ".tmp")) continue;     /* an unfinished push / build */
        char r[700], full[1100];
        snprintf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        snprintf(full, sizeof full, "%s/%s", dir, r);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { scan(dir, r); continue; }
        if (s_n == s_cap) {
            int nc = s_cap ? s_cap * 2 : 512;
            char **nlst = (char **)realloc(s_list, nc * sizeof *nlst);
            if (!nlst) break;
            s_list = nlst; s_cap = nc;
        }
        s_list[s_n++] = strdup(r);
        s_bytes += st.st_size;
    }
    closedir(d);
}

int f2_texset_has(const char *rel)
{
    int lo = 0, hi = s_n;
    while (lo < hi) {
        int mid = (lo + hi) / 2, c = strcmp(rel, s_list[mid]);
        if (!c) return 1;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return 0;
}
int f2_texset_astc(void) { return s_astc; }
const char *f2_texset_dir(void) { return s_dir; }

typedef struct { int ok; char name[256]; unsigned vendor, device; int feat_bc, feat_astc, bc1, bc3, bc5, astc4, astc6; double ms; char why[160]; } probe_t;

static int sampled(PFN_vkGetPhysicalDeviceFormatProperties fp, VkPhysicalDevice pd, VkFormat f)
{
    VkFormatProperties p;
    memset(&p, 0, sizeof p);
    fp(pd, f, &p);
    return (p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
}

static void probe_gpu(probe_t *r)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    memset(r, 0, sizeof *r);
    void *h = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) { snprintf(r->why, sizeof r->why, "dlopen libvulkan.so: %s", dlerror()); return; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance ci = gipa ? (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance") : NULL;
    if (!ci) { snprintf(r->why, sizeof r->why, "no vkCreateInstance"); return; }
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "f2-texset-probe", 1, "f2", 1, VK_API_VERSION_1_0};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai, 0, NULL, 0, NULL};
    VkInstance inst = VK_NULL_HANDLE;
    VkResult vr = ci(&ici, NULL, &inst);
    if (vr != VK_SUCCESS) { snprintf(r->why, sizeof r->why, "vkCreateInstance -> %d", vr); return; }
    PFN_vkDestroyInstance di = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
    PFN_vkEnumeratePhysicalDevices ep = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties gp = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceFeatures gf = (PFN_vkGetPhysicalDeviceFeatures)gipa(inst, "vkGetPhysicalDeviceFeatures");
    PFN_vkGetPhysicalDeviceFormatProperties fp = (PFN_vkGetPhysicalDeviceFormatProperties)gipa(inst, "vkGetPhysicalDeviceFormatProperties");
    VkPhysicalDevice pds[4];
    uint32_t n = 4;
    if (!ep || !gp || !gf || !fp) snprintf(r->why, sizeof r->why, "instance functions missing");
    else if ((vr = ep(inst, &n, pds)) < 0 || n == 0) snprintf(r->why, sizeof r->why, "vkEnumeratePhysicalDevices -> %d, %u devices", vr, n);
    else {
        VkPhysicalDeviceProperties pp;
        VkPhysicalDeviceFeatures f;
        memset(&pp, 0, sizeof pp); memset(&f, 0, sizeof f);
        gp(pds[0], &pp); gf(pds[0], &f);
        snprintf(r->name, sizeof r->name, "%s", pp.deviceName);
        r->vendor = pp.vendorID; r->device = pp.deviceID;
        r->feat_bc = f.textureCompressionBC; r->feat_astc = f.textureCompressionASTC_LDR;
        r->bc1 = sampled(fp, pds[0], VK_FORMAT_BC1_RGB_UNORM_BLOCK); r->bc3 = sampled(fp, pds[0], VK_FORMAT_BC3_UNORM_BLOCK);
        r->bc5 = sampled(fp, pds[0], VK_FORMAT_BC5_UNORM_BLOCK);
        r->astc4 = sampled(fp, pds[0], VK_FORMAT_ASTC_4x4_UNORM_BLOCK); r->astc6 = sampled(fp, pds[0], VK_FORMAT_ASTC_6x6_UNORM_BLOCK);
        r->ok = 1;
    }
    if (di) di(inst, NULL);
    /* libvulkan.so stays loaded: the player dlopens it next anyway */
    clock_gettime(CLOCK_MONOTONIC, &t1);
    r->ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
}

void f2_texset_init(void)
{
    f2_config_t *c = &f2_cfg;
    /* beside the overlay the redirect already uses: <root>/overlay/Data -> <root>/overlay-astc/Data (follows a moved data root) */
    size_t ol = strlen(c->overlay_dir);
    if (c->astc_dir[0]) snprintf(s_dir, sizeof s_dir, "%s", c->astc_dir);
    else if (ol > 13 && !strcmp(c->overlay_dir + ol - 13, "/overlay/Data")) snprintf(s_dir, sizeof s_dir, "%.*s/overlay-astc/Data", (int)(ol - 13), c->overlay_dir);
    else snprintf(s_dir, sizeof s_dir, "%s/overlay-astc/Data", c->files_dir);
    scan(s_dir, "");
    if (s_n) qsort(s_list, s_n, sizeof s_list[0], cmp_str);
    int mode = !strcmp(c->tex_set, "astc") ? 1 : !strcmp(c->tex_set, "bc") ? 2 : 0;
    if (c->bc_hide && !c->vkwrap) f2_log("F2: [RED] bc_hide=1 needs vkwrap=1 (the Vulkan wrappers): the player will still see BC");
    if (c->tex_set[0] && !mode && strcmp(c->tex_set, "auto")) f2_log("F2: f2_config.txt tex_set=%s: unknown value (auto / astc / bc), taken as auto", c->tex_set);
    char folder[200];
    snprintf(folder, sizeof folder, "%d files, %.0f MB", s_n, s_bytes / 1048576.0);
    if (!s_n) {
        s_astc = 0;
        f2_log("F2: texture set = BC (the game's own): no ASTC set at %s%s%s", s_dir,
               mode == 1 ? "  [RED: tex_set=astc asked, but the folder is empty or missing: push overlay-astc/Data]" : "",
               c->bc_hide ? "  (bc_hide=1: the player sees no BC -> every BC texture is decompressed to RGBA32 on the CPU)" : "");
        return;
    }
    probe_t p;
    probe_gpu(&p);
    char gpu[400];
    if (p.ok)
        snprintf(gpu, sizeof gpu, "GPU %s (vendor 0x%x device 0x%x): BC feature %d, BC1/BC3/BC5 sampled %d/%d/%d, ASTC LDR feature %d, ASTC 4x4/6x6 sampled %d/%d, probe %.0f ms",
                 p.name, p.vendor, p.device, p.feat_bc, p.bc1, p.bc3, p.bc5, p.feat_astc, p.astc4, p.astc6, p.ms);
    else snprintf(gpu, sizeof gpu, "GPU probe failed (%s, %.0f ms)", p.why, p.ms);
    int gpu_bc = p.ok && p.feat_bc && p.bc1 && p.bc3;
    int gpu_astc = p.ok && p.astc4 && p.astc6;
    int bc_seen = gpu_bc && !c->bc_hide;          /* what the player will see */
    const char *why;
    if (mode == 1) { s_astc = 1; why = "tex_set=astc in f2_config.txt"; }
    else if (mode == 2) { s_astc = 0; why = "tex_set=bc in f2_config.txt"; }
    else if (!p.ok) { s_astc = 0; why = "auto, GPU unknown -> keep BC"; }
    else if (bc_seen) { s_astc = 0; why = "auto: the GPU samples BC"; }
    else if (!gpu_astc) { s_astc = 0; why = "auto: the GPU samples neither BC nor ASTC (the player decompresses BC on the CPU either way)"; }
    else { s_astc = 1; why = c->bc_hide && gpu_bc ? "auto: BC hidden from the player by bc_hide=1 (simulation), ASTC sampled" : "auto: no BC on this GPU, ASTC sampled"; }
    f2_log("F2: texture set = %s (%s) | %s | %s: %s%s%s", s_astc ? "ASTC" : "BC (the game's own)", why, gpu, s_dir, folder,
           c->bc_hide ? " | bc_hide=1: the player is shown no BC formats" : "",
           s_astc && p.ok && !gpu_astc ? "  [RED: this GPU does not sample ASTC: the player will decompress the ASTC set on the CPU]" : "");
    if (!s_astc && !bc_seen && p.ok)
        f2_log("F2: [RED] texture set BC on a GPU %s: every DXT/BC texture is decompressed to RGBA32 on the CPU at load (4-8x memory)",
               c->bc_hide ? "with BC hidden (bc_hide=1)" : "without BC");
}

/* f2_paths.c: the file to open for <rel> when overlay-astc has it; NULL = not ours */
const char *f2_texset_redirect(const char *rel, int overlay_has_it, char *buf, size_t n)
{
    if (!s_n || !f2_texset_has(rel)) return NULL;
    if (!s_astc) {
        /* BC set: only a file the overlay and the data folder both lack (the ASTC copy then is the only one) */
        if (overlay_has_it) return NULL;
        char d[1100];
        snprintf(d, sizeof d, "%s/%s", f2_cfg.data_dir, rel);
        if (access(d, F_OK) == 0) return NULL;
        static int said;
        if (!said) { said = 1; f2_log("F2: %s is missing from the data folder: the ASTC copy is used (BC set)", rel); }
    }
    __atomic_add_fetch(&f2_cnt_astc_opens, 1, __ATOMIC_RELAXED);
    snprintf(buf, n, "%s/%s", s_dir, rel);
    return buf;
}
