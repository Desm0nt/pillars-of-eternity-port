/* F2 hooks on the stock libunity.so (2018.1.4f1 Android arm64, il2cpp Development or Release):
 *   1. platform patch: SerializedFile::ReadMetadata<false|true> refuse files whose target platform is not 13 (Android);
 *      the game data says 38 (Switch).  One instruction per function: "ldr w1,[sp,#0xd0]" -> "mov w1,#13"
 *      (work/f2-app/data/platform_check.md).  Applied by GNU build id, old word checked.
 *   2. import hooks (GOT / PLT slots found by symbol name, so they do not depend on the variant):
 *        open fopen stat lstat access opendir realpath statfs   -> path redirect (f2_paths.c)
 *        __android_log_print / _vprint / _write                  -> copy of the engine's log lines in f2_log.txt
 *        dlsym                                                   -> wrappers for a few il2cpp_* and Vulkan entry points
 *        dlopen                                                  -> logged
 *        eglCreateContext / eglSwapBuffers                       -> "the engine fell back to GLES" + frame counter
 * libunity has no RELRO, its GOT is writable; text pages are made RWX for the 4-byte patch and RX again. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <dlfcn.h>
#include <link.h>
#include <elf.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <android/log.h>
#include "f2_boot.h"

static uintptr_t s_ubase;
static size_t s_usize;
static const ElfW(Dyn) *s_dyn;
static char s_build_id[64];
static const char *s_variant = "unknown";
static char s_unity_path[512];

uintptr_t f2_unity_base(void) { return s_ubase; }
size_t f2_unity_size(void) { return s_usize; }
const char *f2_unity_variant(void) { return s_variant; }

/* ------------------------------------------------------------------ locate libunity */
static int phdr_cb(struct dl_phdr_info *info, size_t sz, void *data)
{
    (void)sz; (void)data;
    const char *name = info->dlpi_name ? info->dlpi_name : "";
    const char *slash = strrchr(name, '/');
    if (strcmp(slash ? slash + 1 : name, "libunity.so") != 0) return 0;
    s_ubase = info->dlpi_addr;
    snprintf(s_unity_path, sizeof s_unity_path, "%s", name);
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type == PT_LOAD && ph->p_vaddr + ph->p_memsz > s_usize) s_usize = ph->p_vaddr + ph->p_memsz;
        else if (ph->p_type == PT_DYNAMIC) s_dyn = (const ElfW(Dyn) *)(s_ubase + ph->p_vaddr);
        else if (ph->p_type == PT_NOTE) {
            const uint8_t *p = (const uint8_t *)(s_ubase + ph->p_vaddr), *end = p + ph->p_memsz;
            while (p + 12 <= end) {
                uint32_t namesz, descsz, type;
                memcpy(&namesz, p, 4); memcpy(&descsz, p + 4, 4); memcpy(&type, p + 8, 4);
                const uint8_t *desc = p + 12 + ((namesz + 3) & ~3u);
                if (type == 3 /* NT_GNU_BUILD_ID */ && desc + descsz <= end) {
                    for (uint32_t k = 0; k < descsz && k < 20; k++) snprintf(s_build_id + 2 * k, 3, "%02x", desc[k]);
                }
                p = desc + ((descsz + 3) & ~3u);
            }
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ platform patch */
typedef struct { const char *build_id, *variant; uint32_t addr[2]; } f2_patch_t;
static const f2_patch_t k_patches[] = {
    { "77482bdd52bc8403622024b79c44fe6d12c4c6cc", "Release", { 0x9758d0, 0x976558 } },
    { "65a4f5f769a495172096da9070547c0df16764fe", "Development", { 0xe7aec0, 0xe7bc80 } },
};
#define PATCH_OLD 0xb940d3e1u   /* ldr w1, [sp, #0xd0] */
#define PATCH_NEW 0x528001a1u   /* mov w1, #13 */

static int patch_word(uintptr_t addr, uint32_t old, uint32_t neu)
{
    uint32_t cur;
    memcpy(&cur, (void *)addr, 4);
    if (cur == neu) return 0;
    if (cur != old) { f2_log("F2: [RED] patch at libunity+0x%lx: word is %08x, expected %08x: NOT patched", (unsigned long)(addr - s_ubase), cur, old); return -1; }
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t page = addr & ~(uintptr_t)(ps - 1);
    /* RWX in one step: adding PROT_WRITE to a mapping that already is executable needs no execmod */
    if (mprotect((void *)page, ps, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        f2_log("F2: [RED] patch at libunity+0x%lx: mprotect RWX failed: %s", (unsigned long)(addr - s_ubase), strerror(errno));
        return -1;
    }
    memcpy((void *)addr, &neu, 4);
    if (mprotect((void *)page, ps, PROT_READ | PROT_EXEC) != 0)
        f2_log("F2: patch at libunity+0x%lx: mprotect back to RX failed: %s (page stays RWX)", (unsigned long)(addr - s_ubase), strerror(errno));
    __builtin___clear_cache((char *)addr, (char *)addr + 4);
    return 0;
}

/* vk::CheckVulkanSupport knows four GPU vendors (NVIDIA 0x10de, ARM 0x13b5, Qualcomm 0x5143, ImgTec 0x1010) and refuses every other one
 * ("unknown vendor"): the engine then falls back to GLES, where our Vulkan-only shader data is unsupported.  Samsung Xclipse (0x144d,
 * an AMD design), Intel, a software rasteriser ... are such vendors.  Both "b.ne unknown" branches are sent to the ARM case instead, so an
 * unknown vendor is accepted under the same rule as Mali (API 1.0.11 or newer; anything 1.1+ passes).  f2_config.txt any_gpu=0 keeps the
 * engine's own list.  NOT run on such a GPU yet. */
typedef struct { const char *build_id; uint32_t a1, a2; } f2_vendor_patch_t;
static const f2_vendor_patch_t k_vendor[] = {
    { "77482bdd52bc8403622024b79c44fe6d12c4c6cc", 0x677d5c, 0x677d7c },   /* Release */
    { "65a4f5f769a495172096da9070547c0df16764fe", 0xa4d4a4, 0xa4d4c4 },   /* Development */
};
static void apply_vendor_patch(void)
{
    if (!f2_cfg.any_gpu) { f2_log("F2: Vulkan vendor list: the engine's own (any_gpu=0)"); return; }
    for (unsigned i = 0; i < sizeof k_vendor / sizeof k_vendor[0]; i++) {
        if (strcmp(k_vendor[i].build_id, s_build_id)) continue;
        int bad = patch_word(s_ubase + k_vendor[i].a1, 0x54000161u /* b.ne +0x2c (unknown vendor) */, 0x54000121u /* b.ne +0x24 (the ARM case) */);
        bad |= patch_word(s_ubase + k_vendor[i].a2, 0x54000061u /* b.ne +0xc (unknown vendor) */, 0xd503201fu /* nop: fall into the ARM case */);
        f2_log("F2: Vulkan vendor list: %s (libunity+0x%x, +0x%x)", bad ? "[RED] patch FAILED, unknown GPU vendors will be refused" : "any vendor is accepted as ARM is", k_vendor[i].a1, k_vendor[i].a2);
        return;
    }
}

/* UnityDefaultAllocator's page table (the player's bookkeeping of which pointers are its own) knows FIVE 4 GB windows of the address space
 * (5 slots keyed by the pointer's high 32 bits).  A sixth window -> "Using memoryadresses from more that 16GB of memory", and then the code
 * goes on with slot index -1, i.e. it uses a neighbouring field of the allocator as the table: memory corruption, and soon
 * MemoryManager::Deallocate hands a pointer to free() that malloc never returned -> abort (Xiaomi 13 Pro, Android 15, phone run 28: every
 * new game died while the first area loaded; on the Xiaomi 14 Ultra the heap happened to fit 4 windows).  Newer Android spreads malloc's
 * regions wider, so this is a matter of time on any phone.  Three patches per player variant (tools/alloc_sites.py prints the places):
 *   register (AllocationPage<0>): no free slot -> a flag is set in OUR memory and the allocation is simply not recorded;
 *   deregister (AllocationPage<1>): key not among the slots -> nothing to do;
 *   query (AllocationPage<2>, behind Contains()): key not among the slots -> "yes" once the flag is set (so the block is freed by the
 *   allocator it came from, header and all), "no" as before while every allocation is still recorded.
 * f2_config.txt alloc_fix=0 leaves the player as it is. */
static uint8_t s_alloc_overflow;      /* set by the patched register path */
typedef struct { const char *build_id; uint32_t reg_err, reg_old, reg_epi, query, dereg, dereg_epi; } f2_alloc_patch_t;
static const f2_alloc_patch_t k_alloc[] = {
    { "77482bdd52bc8403622024b79c44fe6d12c4c6cc", 0x476880, 0xd00055c8u, 0x476ab8, 0x476758, 0x476b28, 0x476ce4 },   /* Release */
    { "65a4f5f769a495172096da9070547c0df16764fe", 0x65de3c, 0xd0007feau, 0x65e074, 0x65dcf4, 0x65e0e8, 0x65e294 },   /* Development */
};
/* ImageConversion.LoadImage into a texture that is DXT1 / DXT5 already.  The game loads the player's portraits (loose PNG files) INTO the portrait
 * textures its prefabs carry, and those are DXT5 assets.  LoadMemoryBufferIntoTextureInternal: "compress" = the texture is DXT AND the GPU
 * samples DXT (GraphicsCaps::HasSampledTexture(99)) -> the decoder is told DXT5 -> and this ANDROID player has no DXT compressor: it returns
 * false with the texture untouched (same size: the prefab's placeholder stays) or re-made with garbage in it (another size).  On a phone
 * without BC the same call decodes to ARGB32 and works; Adreno has BC, so there the player's portrait was garbage / somebody else's
 * (owner, 12.11.2026).  The patch makes "compress" always 0 (and w23, w9, w23 -> mov w23, #0): the picture is decoded to ARGB32 everywhere. */
typedef struct { const char *build_id; uint32_t at; } f2_loadimage_patch_t;
static const f2_loadimage_patch_t k_loadimage[] = {
    { "77482bdd52bc8403622024b79c44fe6d12c4c6cc", 0x2f7dc0 },   /* Release */
    { "65a4f5f769a495172096da9070547c0df16764fe", 0x43ea1c },   /* Development */
};
static void apply_loadimage_patch(void)
{
    for (unsigned i = 0; i < sizeof k_loadimage / sizeof k_loadimage[0]; i++) {
        if (strcmp(k_loadimage[i].build_id, s_build_id)) continue;
        int bad = patch_word(s_ubase + k_loadimage[i].at, 0x0a170137u /* and w23, w9, w23 */, 0x52800017u /* mov w23, #0 */);
        f2_log("F2: LoadImage: %s (libunity+0x%x)", bad ? "[RED] patch FAILED: pictures loaded into DXT textures (player portraits) stay broken on GPUs with BC"
               : "a picture loaded into a DXT texture is decoded to ARGB32 (this player cannot compress DXT)", k_loadimage[i].at);
        return;
    }
}

static int patch_words(uintptr_t addr, uint32_t old_first, const uint32_t *w, int n)
{
    uint32_t cur;
    memcpy(&cur, (void *)addr, 4);
    if (cur == w[0]) return 0;
    if (cur != old_first) { f2_log("F2: [RED] patch at libunity+0x%lx: word is %08x, expected %08x: NOT patched", (unsigned long)(addr - s_ubase), cur, old_first); return -1; }
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t page = addr & ~(uintptr_t)(ps - 1), end = (addr + 4 * n + ps - 1) & ~(uintptr_t)(ps - 1);
    if (mprotect((void *)page, end - page, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) { f2_log("F2: [RED] patch at libunity+0x%lx: mprotect: %s", (unsigned long)(addr - s_ubase), strerror(errno)); return -1; }
    memcpy((void *)addr, w, 4 * (size_t)n);
    mprotect((void *)page, end - page, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)addr, (char *)addr + 4 * n);
    return 0;
}
static uint32_t enc_b(uintptr_t from, uintptr_t to) { return 0x14000000u | (uint32_t)((((intptr_t)to - (intptr_t)from) >> 2) & 0x3ffffff); }
static void apply_alloc_patch(void)
{
    if (!f2_cfg.alloc_fix) { f2_log("F2: allocator page table: the player's own (alloc_fix=0)"); return; }
    for (unsigned i = 0; i < sizeof k_alloc / sizeof k_alloc[0]; i++) {
        const f2_alloc_patch_t *p = &k_alloc[i];
        if (strcmp(p->build_id, s_build_id)) continue;
        uint64_t flag = (uint64_t)(uintptr_t)&s_alloc_overflow;
        /* register, at the head of the old error path:  ldr x8, =flag ; mov w9, #1 ; strb w9, [x8] ; b epilogue ; .quad flag */
        uint32_t reg[6] = { 0x58000088u, 0x52800029u, 0x39000109u, enc_b(s_ubase + p->reg_err + 12, s_ubase + p->reg_epi), 0, 0 };
        memcpy(&reg[4], &flag, 8);
        /* query, where the key was not found:  ldr x8, =flag ; ldrb w0, [x8] ; ret ; .quad flag */
        uint32_t qry[5] = { 0x58000068u, 0x39400100u, 0xd65f03c0u, 0, 0 };
        memcpy(&qry[3], &flag, 8);
        uint32_t der[1] = { enc_b(s_ubase + p->dereg, s_ubase + p->dereg_epi) };
        int bad = patch_words(s_ubase + p->reg_err, p->reg_old, reg, 6);
        bad |= patch_words(s_ubase + p->query, 0xaa1f03eau /* mov x10, xzr */, qry, 5);
        bad |= patch_words(s_ubase + p->dereg, 0xaa1f03e9u /* mov x9, xzr */, der, 1);
        f2_log("F2: allocator page table: %s (libunity+0x%x, +0x%x, +0x%x)", bad ? "[RED] patch FAILED: a heap wider than five 4 GB windows will crash the player"
               : "allocations beyond its five 4 GB windows are left unrecorded instead of corrupting it", p->reg_err, p->query, p->dereg);
        return;
    }
}
int f2_alloc_overflowed(void) { return s_alloc_overflow; }

/* ---- unity_trace=1 (f2_config.txt): call counters at the entries of the player's JNI natives and of its start-up functions, to see how far a
 * player variant gets (the Release player stopped before any engine work, phone run 29).  Same stub as the loader's game gates: the entry
 * instruction (a "stp ..., [sp, #-N]!" or "sub sp, sp, #N": checked) is replaced by a branch to a stub on a page mapped next to libunity that
 * saves x0..x30, calls the hook with the gate's index in x1, restores, runs the displaced instruction and branches back. */
typedef struct { const char *build_id; uint32_t off; const char *name; } f2_trace_t;
#define REL "77482bdd52bc8403622024b79c44fe6d12c4c6cc"
#define DEV "65a4f5f769a495172096da9070547c0df16764fe"
static const f2_trace_t k_trace[] = {
    { REL, 0x442640, "initJni" }, { REL, 0x442778, "nativeResume" }, { REL, 0x442820, "nativeFocusChanged" }, { REL, 0x44289c, "nativeRecreateGfxState" },
    { REL, 0x442904, "nativeRender" }, { REL, 0x43f644, "UnityPlayerLoop" }, { REL, 0x43e61c, "UnityInitApplication" }, { REL, 0x44271c, "nativePause" },
    { REL, 0x4426b4, "nativeDone" },
    { DEV, 0x60f278, "nativeRender" }, { DEV, 0x60bd84, "UnityPlayerLoop" }, { DEV, 0x60acbc, "UnityInitApplication" },
};
static unsigned long s_trace_n[sizeof k_trace / sizeof k_trace[0]];
static void trace_hook(uint64_t *regs, uint64_t idx)
{
    (void)regs;
    if (idx >= sizeof k_trace / sizeof k_trace[0]) return;
    unsigned long n = __atomic_add_fetch(&s_trace_n[idx], 1, __ATOMIC_RELAXED);
    if (n <= 3 || n == 10 || n == 100 || n == 1000) f2_log("F2: unity trace: %s call #%lu (thread %d)", k_trace[idx].name, n, (int)syscall(SYS_gettid));
}
static void *near_page(void)
{
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t want = (s_ubase + s_usize + ps - 1) & ~(uintptr_t)(ps - 1);
    for (int k = 2; k < 4096; k++) {
        uintptr_t a = want + (uintptr_t)k * 0x10000;
        if (a - s_ubase > 0x7000000) break;
        void *r = mmap((void *)a, ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (r == (void *)a) return r;
        if (r != MAP_FAILED) munmap(r, ps);
    }
    return NULL;
}
static void install_trace(void)
{
    if (!f2_cfg.unity_trace) return;
    uint32_t *page = (uint32_t *)near_page(), *w = page;
    if (!page) { f2_log("F2: unity trace: no page near libunity"); return; }
    int n = 0;
    void (*hook)(uint64_t *, uint64_t) = trace_hook;
    for (unsigned i = 0; i < sizeof k_trace / sizeof k_trace[0]; i++) {
        if (strcmp(k_trace[i].build_id, s_build_id)) continue;
        uintptr_t site = s_ubase + k_trace[i].off;
        uint32_t cur;
        memcpy(&cur, (void *)site, 4);
        if ((cur & 0xffc003e0u) != 0xa98003e0u && (cur & 0xff8003ffu) != 0xd10003ffu) { f2_log("F2: unity trace: %s entry word %08x is not a plain prologue: skipped", k_trace[i].name, cur); continue; }
        uint32_t *v = w;
        *w++ = 0xd10483ffu;                                                                        /* sub sp, sp, #0x120 */
        for (unsigned r = 0; r < 30; r += 2) *w++ = 0xa9000000u | (r << 15) | ((r + 1) << 10) | (31u << 5) | r;
        *w++ = 0xf9007bfeu;                                                                        /* str x30, [sp, #240] */
        *w++ = 0x910003e0u;                                                                        /* mov x0, sp */
        *w++ = 0xd2800001u | (i << 5);                                                             /* mov x1, #index */
        uint32_t *ldr = w++;
        *w++ = 0xd63f0200u;                                                                        /* blr x16 */
        for (unsigned r = 0; r < 30; r += 2) *w++ = 0xa9400000u | (r << 15) | ((r + 1) << 10) | (31u << 5) | r;
        *w++ = 0xf9407bfeu; *w++ = 0x910483ffu;                                                    /* ldr x30 ; add sp */
        *w++ = cur;
        *w = enc_b((uintptr_t)w, site + 4); w++;
        if ((uintptr_t)w & 7) *w++ = 0xd503201fu;
        *ldr = 0x58000010u | ((uint32_t)(w - ldr) << 5);
        memcpy(w, &hook, 8); w += 2;
        __builtin___clear_cache((char *)v, (char *)w);
        uint32_t b = enc_b(site, (uintptr_t)v);
        if (patch_word(site, cur, b) == 0) n++;
    }
    mprotect(page, sysconf(_SC_PAGESIZE), PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)page, (char *)w);
    f2_log("F2: unity trace: %d entry counters installed", n);
}

static void apply_patches(void)
{
    const f2_patch_t *p = NULL;
    for (unsigned i = 0; i < sizeof k_patches / sizeof k_patches[0]; i++)
        if (!strcmp(k_patches[i].build_id, s_build_id)) p = &k_patches[i];
    if (!p) {
        f2_log("F2: [RED] libunity build id %s is not one of the two known 2018.1.4f1 il2cpp arm64 players: platform patch NOT applied, "
               "every data file will be refused (\"created for another build target\")", s_build_id);
        return;
    }
    s_variant = p->variant;
    if (!f2_cfg.patch_platform) { f2_log("F2: platform patch disabled by f2_config.txt"); return; }
    int bad = 0;
    for (int i = 0; i < 2; i++) bad |= patch_word(s_ubase + p->addr[i], PATCH_OLD, PATCH_NEW);
    if (!bad)
        f2_once_milestone(M_PATCHED, "platform check patched (%s): libunity+0x%x and +0x%x now read target platform 13", p->variant, p->addr[0], p->addr[1]);
}

/* ------------------------------------------------------------------ async upload pump
 * The Switch data keeps texture pixels in .resS stream files, so Texture::CreateFileTextureUploadInstruction queues every
 * texture on the AsyncUploadManager and the scene integration (TimeSliceAwakeFromLoadQueue::IntegrateTimeSliced) waits
 * for AsyncUploadManager::HasCompleted(fence).  The Android 2018.1.4 player never built data like that (its Texture
 * transfer has SupportsAsyncUpload == false, pixels are inline), so its AsyncUploadManager::TimeSlicedUpdate() is an
 * empty "ret": nothing ever sends the AsyncResourceUpload command to the render thread and the first scene load waits
 * forever (phone run 02 / 03: last milestone M21, frames=0).  The rest of the machinery is in the binary (client
 * command, GfxDeviceWorker case, AsyncUploadManager::AsyncResourceUpload), only the trigger is missing.  We supply it:
 * TimeSlicedUpdate (called once per frame from the player loop and from PreloadManager::UpdatePreloadingSingleStep)
 * becomes a branch to a veneer page mapped next to libunity, which jumps here. */
typedef struct {
    const char *build_id;
    uint32_t tsu, get_gfx, get_mgr;                 /* TimeSlicedUpdate, GetGfxDevice, GetManagerFromContext */
    uint16_t vt_async;                              /* vtable offset of GfxDevice::AsyncResourceUpload(int,int,TextureID) */
    uint16_t qs_arr, qs_cur, qs_stride, qs_slice, qs_buf;   /* QualitySettings: array ptr, current index, element size, fields */
} f2_pump_t;
static const f2_pump_t k_pump[] = {
    { "77482bdd52bc8403622024b79c44fe6d12c4c6cc", 0x6b4f70, 0x5f2ff4, 0x502ed8, 0x7b8, 0x30, 0x4c, 0x88, 0x7c, 0x80 },
    { "65a4f5f769a495172096da9070547c0df16764fe", 0xa9388c, 0x9a206c, 0x708eac, 0x800, 0x48, 0x64, 0x90, 0x84, 0x88 },
};
static const f2_pump_t *s_pump;
unsigned long f2_cnt_pump;
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static void f2_async_upload_pump(void *mgr)
{
    (void)mgr;
    const f2_pump_t *p = s_pump;
    void *(*get_gfx)(void) = (void *(*)(void))(s_ubase + p->get_gfx);
    void *(*get_mgr)(int) = (void *(*)(int))(s_ubase + p->get_mgr);
    void *dev = get_gfx();
    char *qs = (char *)get_mgr(11);                 /* QualitySettings */
    if (!dev || !qs) return;
    char *arr = *(char **)(qs + p->qs_arr);
    int cur = *(int *)(qs + p->qs_cur);
    int slice = 2, buf = 4;
    if (arr && cur >= 0 && cur < 64) {
        slice = *(int *)(arr + (size_t)cur * p->qs_stride + p->qs_slice);
        buf = *(int *)(arr + (size_t)cur * p->qs_stride + p->qs_buf);
    }
    if (f2_cfg.pump_ms > 0) slice = f2_cfg.pump_ms;
    if (slice < 1 || slice > 100) slice = 2;
    if (buf < 2 || buf > 512) buf = 4;
    unsigned long n = __atomic_add_fetch(&f2_cnt_pump, 1, __ATOMIC_RELAXED);
    if (n == 1 && f2_cfg.crash) f2_crash_install();   /* libunity put its own crash handler on top during engine init */
    if (n == 1) f2_once_milestone(M_PUMP, "async upload pump running: GfxDevice %p, time slice %d ms, ring buffer %d MB (quality level %d)", dev, slice, buf, cur);
    void **vt = *(void ***)dev;
    ((void (*)(void *, int, int, uint64_t))vt[p->vt_async / 8])(dev, slice, buf, 0);
}

static void install_pump(void)
{
    const f2_pump_t *p = NULL;
    for (unsigned i = 0; i < sizeof k_pump / sizeof k_pump[0]; i++)
        if (!strcmp(k_pump[i].build_id, s_build_id)) p = &k_pump[i];
    if (!p) return;
    if (!f2_cfg.pump) { f2_log("F2: async upload pump disabled by f2_config.txt (scenes with stream textures will not finish loading)"); return; }
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t site = s_ubase + p->tsu, want = (s_ubase + s_usize + ps - 1) & ~(uintptr_t)(ps - 1);
    void *page = MAP_FAILED;
    for (int k = 0; k < 4096 && page == MAP_FAILED; k++) {
        uintptr_t a = want + (uintptr_t)k * 0x10000;
        if (a - site > 0x7000000) break;            /* keep the b within +-128 MB */
        void *r = mmap((void *)a, ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (r == (void *)a) page = r;
        else if (r != MAP_FAILED) munmap(r, ps);    /* an old kernel ignored the flag and put it elsewhere */
    }
    if (page == MAP_FAILED) { f2_log("F2: [RED] async upload pump: no free page within reach of libunity: NOT installed"); return; }
    uint32_t ven[4] = { 0x58000050u /* ldr x16, #8 */, 0xd61f0200u /* br x16 */, 0, 0 };
    uint64_t target = (uint64_t)(uintptr_t)f2_async_upload_pump;
    memcpy(&ven[2], &target, 8);
    memcpy(page, ven, sizeof ven);
    if (mprotect(page, ps, PROT_READ | PROT_EXEC) != 0) { f2_log("F2: [RED] async upload pump: veneer page not executable: %s", strerror(errno)); munmap(page, ps); return; }
    __builtin___clear_cache((char *)page, (char *)page + sizeof ven);
    s_pump = p;
    int64_t delta = (int64_t)((uintptr_t)page - site);
    uint32_t b = 0x14000000u | (uint32_t)((delta >> 2) & 0x3ffffff);
    if (patch_word(site, 0xd65f03c0u /* ret */, b) != 0) { s_pump = NULL; return; }
    f2_log("F2: async upload pump installed: libunity+0x%x (AsyncUploadManager::TimeSlicedUpdate) -> veneer %p -> %p", p->tsu, page, (void *)f2_async_upload_pump);
}

/* ------------------------------------------------------------------ path hooks */
static unsigned s_open_logged, s_fail_logged;

static const char *redir(const char *path, char *buf, size_t n, const char *what)
{
    int kind;
    const char *r = f2_redirect(path, buf, n, &kind);
    if (kind) {
        __atomic_add_fetch(&f2_cnt_opens, 1, __ATOMIC_RELAXED);
        f2_once_milestone(M_FIRST_DATA, "engine reached the data through the redirect: %s(%s) -> %s", what, path, r);
        if (strstr(path, "/level0") && !strstr(path, "/level0.")) f2_once_milestone(M_FIRST_SCENE, "engine opened level0 (first scene file)");
        if ((int)s_open_logged < f2_cfg.open_log) {
            s_open_logged++;
            f2_log("F2: %s %s -> %s", what, kind == 1 ? "[overlay]" : kind == 3 ? "[astc]" : kind == 4 ? "[defres]" : "[data]", r);
        }
    }
    return r;
}

static void fail_note(const char *what, const char *orig, const char *used, int err)
{
    if (orig == used) return;                 /* not one of ours */
    __atomic_add_fetch(&f2_cnt_open_fail, 1, __ATOMIC_RELAXED);
    /* the engine probes <file>.resS / .resG / .res / .split0 beside every serialized file: expected misses, a few are enough;
     * every other miss is worth a line (phone run 08: the cap was used up by the probes and hid the real failures) */
    size_t ul = strlen(used);
    int probe = (ul > 5 && (!strcmp(used + ul - 5, ".resS") || !strcmp(used + ul - 5, ".resG"))) || (ul > 4 && !strcmp(used + ul - 4, ".res")) ||
                (ul > 7 && !strcmp(used + ul - 7, ".split0"));
    static unsigned s_probe_logged;
    if (probe) { if (s_probe_logged < 12) { s_probe_logged++; f2_log("F2: %s FAILED %s (asked %s): %s  [stream-file probe, normal]", what, used, orig, strerror(err)); } }
    else if (s_fail_logged < 3000) { s_fail_logged++; f2_log("F2: %s FAILED %s (asked %s): %s", what, used, orig, strerror(err)); }
}

static int h_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) { va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap); }
    char buf[1100];
    const char *p = redir(path, buf, sizeof buf, "open");
    int fd = open(p, flags, mode);
    if (fd < 0) { int e = errno; fail_note("open", path, p, e); errno = e; }
    return fd;
}
static FILE *h_fopen(const char *path, const char *mode)
{
    char buf[1100];
    const char *p = redir(path, buf, sizeof buf, "fopen");
    FILE *f = fopen(p, mode);
    if (!f) { int e = errno; fail_note("fopen", path, p, e); errno = e; }
    return f;
}
static int h_stat(const char *path, struct stat *st)
{
    char buf[1100];
    int kind;
    const char *p = f2_redirect(path, buf, sizeof buf, &kind);
    int r = stat(p, st);
    if (r != 0 && kind) { int e = errno; fail_note("stat", path, p, e); errno = e; }
    return r;
}
static int h_lstat(const char *path, struct stat *st)
{
    char buf[1100];
    int kind;
    const char *p = f2_redirect(path, buf, sizeof buf, &kind);
    return kind ? stat(p, st) : lstat(p, st);
}
static int h_access(const char *path, int mode)
{
    char buf[1100];
    int kind;
    return access(f2_redirect(path, buf, sizeof buf, &kind), mode);
}
static DIR *h_opendir(const char *path)
{
    char buf[1100];
    return opendir(redir(path, buf, sizeof buf, "opendir"));
}
static char *h_realpath(const char *path, char *resolved)
{
    char buf[1100];
    int kind;
    return realpath(f2_redirect(path, buf, sizeof buf, &kind), resolved);
}
static int h_statfs(const char *path, struct statfs *st)
{
    char buf[1100];
    int kind;
    return statfs(f2_redirect(path, buf, sizeof buf, &kind), st);
}

/* ------------------------------------------------------------------ log tee */
static const struct { const char *needle; int id; const char *what; } k_marks[] = {
    { "PlayerInitEngineNoGraphics OK", M_ENGINE_NOGFX, "engine initialised without graphics (globalgamemanagers loaded)" },
    { "PlayerInitEngineGraphics OK", M_ENGINE_GFX, "engine graphics initialised" },
};
static const char *const k_bad[] = {
    "Desired shader compiler platform", "Mismatched serialization", "created for another build target", "Failed to load",
    "can't be loaded", "Unable to initialize", "not supported", "Could not", "NullReferenceException", "Exception:",
    "is corrupted", "Invalid serialized file", "Shader error", "Unsupported", "vulkan", "Vulkan", "GLES", "Renderer:", "Version:",
};
static unsigned s_flagged;

static void log_tee(int prio, const char *tag, const char *text)
{
    __atomic_add_fetch(&f2_cnt_unity_lines, 1, __ATOMIC_RELAXED);
    if (prio >= ANDROID_LOG_ERROR) __atomic_add_fetch(&f2_cnt_unity_errors, 1, __ATOMIC_RELAXED);
    char pfx[48];
    static const char lv[] = "??VDIWEF";
    snprintf(pfx, sizeof pfx, "U/%c/%s: ", lv[prio >= 0 && prio < 8 ? prio : 0], tag ? tag : "");
    f2_log_file_only(pfx, text);
    for (unsigned i = 0; i < sizeof k_marks / sizeof k_marks[0]; i++)
        if (strstr(text, k_marks[i].needle)) f2_once_milestone(k_marks[i].id, "%s", k_marks[i].what);
    if (s_flagged < 400)
        for (unsigned i = 0; i < sizeof k_bad / sizeof k_bad[0]; i++)
            if (strstr(text, k_bad[i])) {
                s_flagged++;
                char one[400];
                snprintf(one, sizeof one, "%s", text);
                char *nl = strchr(one, '\n');
                if (nl) *nl = 0;
                f2_log("F2: engine says: %s", one);       /* also under our logcat tag: one grep shows the whole story */
                break;
            }
}
static int h_log_write(int prio, const char *tag, const char *text)
{
    log_tee(prio, tag, text ? text : "");
    return __android_log_write(prio, tag, text);
}
static int h_log_vprint(int prio, const char *tag, const char *fmt, va_list ap)
{
    char buf[8192];
    vsnprintf(buf, sizeof buf, fmt, ap);
    log_tee(prio, tag, buf);
    return __android_log_write(prio, tag, buf);
}
static int h_log_print(int prio, const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = h_log_vprint(prio, tag, fmt, ap);
    va_end(ap);
    return r;
}

/* ------------------------------------------------------------------ dlopen / dlsym */
static void *s_il2cpp_handle;
static unsigned s_il2cpp_lookups, s_il2cpp_missing;

static void *h_dlopen(const char *name, int flags)
{
    void *h = dlopen(name, flags);
    f2_log("F2: libunity dlopen(%s) -> %p%s%s", name ? name : "(null)", h, h ? "" : " : ", h ? "" : dlerror());
    if (h && name && strstr(name, "libil2cpp.so")) s_il2cpp_handle = h;
    return h;
}
static void *h_dlsym(void *handle, const char *name)
{
    void *p = dlsym(handle, name);
    if (!name) return p;
    if (!strncmp(name, "il2cpp_", 7)) {
        s_il2cpp_lookups++;
        if (!p) { s_il2cpp_missing++; f2_log("F2: [RED] LoadIl2Cpp: %s is not exported by our libil2cpp.so", name); }
        if (s_il2cpp_lookups == 1) f2_once_milestone(M_IL2CPP_LOOKUP, "LoadIl2Cpp: libunity started its dlsym pass over our libil2cpp.so (handle %p)", handle);
        return f2_diag_il2cpp_symbol(name, p);
    }
    if (name[0] == 'v' && name[1] == 'k') {
        if (!p) f2_log("F2: libvulkan has no %s", name);
        return f2_cfg.vkwrap ? f2_vk_wrap_symbol(name, p) : p;
    }
    return p;
}
void f2_hooks_il2cpp_lookup_stats(unsigned *n, unsigned *missing) { *n = s_il2cpp_lookups; *missing = s_il2cpp_missing; }

/* ------------------------------------------------------------------ GOT patching */
typedef struct { const char *name; void *fn; int on; } f2_hook_t;
static f2_hook_t s_hooks[] = {
    { "open", h_open, 1 }, { "fopen", h_fopen, 1 }, { "stat", h_stat, 1 }, { "lstat", h_lstat, 1 }, { "access", h_access, 1 },
    { "opendir", h_opendir, 1 }, { "realpath", h_realpath, 1 }, { "statfs", h_statfs, 1 },
    { "__android_log_write", h_log_write, 2 }, { "__android_log_vprint", h_log_vprint, 2 }, { "__android_log_print", h_log_print, 2 },
    { "dlopen", h_dlopen, 1 }, { "dlsym", h_dlsym, 1 },
    { "eglCreateContext", NULL, 3 }, { "eglSwapBuffers", NULL, 3 },
};

static int hook_relocs(const ElfW(Rela) *rela, size_t count, const ElfW(Sym) *symtab, const char *strtab)
{
    int n = 0;
    for (size_t i = 0; i < count; i++) {
        uint32_t type = ELF64_R_TYPE(rela[i].r_info), sym = ELF64_R_SYM(rela[i].r_info);
        if (type != R_AARCH64_JUMP_SLOT && type != R_AARCH64_GLOB_DAT && type != R_AARCH64_ABS64) continue;
        const char *name = strtab + symtab[sym].st_name;
        for (unsigned h = 0; h < sizeof s_hooks / sizeof s_hooks[0]; h++) {
            if (strcmp(name, s_hooks[h].name) != 0) continue;
            if (s_hooks[h].on == 2 && !f2_cfg.logtee) continue;
            void **slot = (void **)(s_ubase + rela[i].r_offset);
            void *fn = s_hooks[h].fn;
            if (s_hooks[h].on == 3) {
                fn = f2_egl_hook(name);
                if (!fn) continue;
                f2_egl_set_real(name, *slot);
            }
            long ps = sysconf(_SC_PAGESIZE);
            mprotect((void *)((uintptr_t)slot & ~(uintptr_t)(ps - 1)), ps, PROT_READ | PROT_WRITE);
            *slot = fn;
            n++;
        }
    }
    return n;
}

void f2_hooks_install(void *unity_handle)
{
    (void)unity_handle;
    dl_iterate_phdr(phdr_cb, NULL);
    if (!s_ubase || !s_dyn) { f2_log("F2: [RED] libunity.so not found among the loaded modules: nothing patched or hooked"); return; }
    f2_once_milestone(M_UNITY_LOADED, "libunity.so mapped at %p (%zu MB) from %s, build id %s", (void *)s_ubase, s_usize >> 20, s_unity_path, s_build_id);
    apply_patches();
    apply_alloc_patch();
    apply_vendor_patch();
    apply_loadimage_patch();
    install_trace();
    f2_log("F2: libunity variant: %s", s_variant);
    install_pump();
    if (!f2_cfg.hooks) { f2_log("F2: import hooks disabled by f2_config.txt (the engine will not find its data)"); return; }
    const ElfW(Rela) *rela = NULL, *jmprel = NULL;
    size_t relasz = 0, pltrelsz = 0;
    const ElfW(Sym) *symtab = NULL;
    const char *strtab = NULL;
    for (const ElfW(Dyn) *d = s_dyn; d->d_tag != DT_NULL; d++) {
        /* bionic leaves d_ptr values unrelocated: add the load base */
        switch (d->d_tag) {
        case DT_RELA: rela = (const ElfW(Rela) *)(s_ubase + d->d_un.d_ptr); break;
        case DT_RELASZ: relasz = d->d_un.d_val; break;
        case DT_JMPREL: jmprel = (const ElfW(Rela) *)(s_ubase + d->d_un.d_ptr); break;
        case DT_PLTRELSZ: pltrelsz = d->d_un.d_val; break;
        case DT_SYMTAB: symtab = (const ElfW(Sym) *)(s_ubase + d->d_un.d_ptr); break;
        case DT_STRTAB: strtab = (const char *)(s_ubase + d->d_un.d_ptr); break;
        }
    }
    if (!symtab || !strtab) { f2_log("F2: [RED] libunity dynamic section unreadable: no hooks"); return; }
    int n = 0;
    if (jmprel) n += hook_relocs(jmprel, pltrelsz / sizeof(ElfW(Rela)), symtab, strtab);
    if (rela) n += hook_relocs(rela, relasz / sizeof(ElfW(Rela)), symtab, strtab);
    f2_once_milestone(M_HOOKED, "%d import slots of libunity hooked (file redirect%s, dlsym, dlopen, egl)", n, f2_cfg.logtee ? ", log copy" : "");
    if (n < 10) f2_log("F2: [RED] fewer hooks than expected (%d): the redirect may be incomplete", n);
}
