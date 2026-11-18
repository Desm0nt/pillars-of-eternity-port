/* F2 Vulkan / EGL diagnostics.  libunity loads libvulkan.so with dlopen, takes vkGetInstanceProcAddr /
 * vkCreateInstance / ... with dlsym (LoadVulkanLibraryPhase1) and every other function with
 * vkGetInstanceProcAddr (Phase2).  Our dlsym hook hands out wrappers for a handful of entry points; they
 * only observe (results, counts, timing) and never change arguments or results -- except:
 *   bc_hide=1 (f2_config.txt): the physical-device queries hide BC.  vk::Initialize fills vk::s_FormatInfoTable from
 *   vkGetPhysicalDeviceFormatProperties for VkFormat 1..184 and vk::ImageManager::CreateImage falls back to RGBA32 + CPU decompression for a
 *   format without SAMPLED_IMAGE, so with BC formats reported as unsupported the player behaves as on a GPU without BC (Mali, PowerVR):
 *   the "before" of the ASTC set (f2_texset.c), measurable on Adreno.  Features: textureCompressionBC = 0; image format queries for a BC
 *   format: VK_ERROR_FORMAT_NOT_SUPPORTED. */
#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <vulkan/vulkan.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include "f2_boot.h"

static PFN_vkGetInstanceProcAddr r_gipa;
static PFN_vkGetDeviceProcAddr r_gdpa;
static PFN_vkCreateInstance r_create_instance;
static PFN_vkCreateDevice r_create_device;
static PFN_vkCreateShaderModule r_create_shader;
static PFN_vkCreateGraphicsPipelines r_create_gp;
static PFN_vkCreateComputePipelines r_create_cp;
void f2_screen_note_fps(float fps);
static PFN_vkCreateSwapchainKHR r_create_swapchain;
static PFN_vkQueuePresentKHR r_present;
static PFN_vkAcquireNextImageKHR r_acquire;
static PFN_vkCreateRenderPass r_create_rp;
static VkInstance s_instance;

/* ---- bc_hide: a GPU without BC as the player sees it ---- */
static PFN_vkGetPhysicalDeviceFeatures r_gpd_features;
static PFN_vkGetPhysicalDeviceFeatures2 r_gpd_features2, r_gpd_features2_khr;
static PFN_vkGetPhysicalDeviceFormatProperties r_gpd_fmt;
static PFN_vkGetPhysicalDeviceFormatProperties2 r_gpd_fmt2, r_gpd_fmt2_khr;
static PFN_vkGetPhysicalDeviceImageFormatProperties r_gpd_ifmt;
static PFN_vkGetPhysicalDeviceImageFormatProperties2 r_gpd_ifmt2, r_gpd_ifmt2_khr;
static unsigned long s_bc_hidden;

static int is_bc(VkFormat f) { return f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK; }
static void hid(const char *fn)
{
    if (__atomic_add_fetch(&s_bc_hidden, 1, __ATOMIC_RELAXED) == 1)
        f2_log("F2: bc_hide: %s is the first query that hides BC from the player (simulated GPU without BC)", fn);
}
static VKAPI_ATTR void VKAPI_CALL w_gpd_features(VkPhysicalDevice pd, VkPhysicalDeviceFeatures *f)
{
    r_gpd_features(pd, f);
    if (f && f->textureCompressionBC) { f->textureCompressionBC = VK_FALSE; hid("vkGetPhysicalDeviceFeatures"); }
}
static VKAPI_ATTR void VKAPI_CALL w_gpd_features2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f)
{
    r_gpd_features2(pd, f);
    if (f && f->features.textureCompressionBC) { f->features.textureCompressionBC = VK_FALSE; hid("vkGetPhysicalDeviceFeatures2"); }
}
static VKAPI_ATTR void VKAPI_CALL w_gpd_features2_khr(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f)
{
    r_gpd_features2_khr(pd, f);
    if (f && f->features.textureCompressionBC) { f->features.textureCompressionBC = VK_FALSE; hid("vkGetPhysicalDeviceFeatures2KHR"); }
}
static VKAPI_ATTR void VKAPI_CALL w_gpd_fmt(VkPhysicalDevice pd, VkFormat fmt, VkFormatProperties *p)
{
    r_gpd_fmt(pd, fmt, p);
    if (p && is_bc(fmt)) { memset(p, 0, sizeof *p); hid("vkGetPhysicalDeviceFormatProperties"); }
}
static VKAPI_ATTR void VKAPI_CALL w_gpd_fmt2(VkPhysicalDevice pd, VkFormat fmt, VkFormatProperties2 *p)
{
    r_gpd_fmt2(pd, fmt, p);
    if (p && is_bc(fmt)) { memset(&p->formatProperties, 0, sizeof p->formatProperties); hid("vkGetPhysicalDeviceFormatProperties2"); }
}
static VKAPI_ATTR void VKAPI_CALL w_gpd_fmt2_khr(VkPhysicalDevice pd, VkFormat fmt, VkFormatProperties2 *p)
{
    r_gpd_fmt2_khr(pd, fmt, p);
    if (p && is_bc(fmt)) { memset(&p->formatProperties, 0, sizeof p->formatProperties); hid("vkGetPhysicalDeviceFormatProperties2KHR"); }
}
static VKAPI_ATTR VkResult VKAPI_CALL w_gpd_ifmt(VkPhysicalDevice pd, VkFormat fmt, VkImageType t, VkImageTiling tl, VkImageUsageFlags u,
                                                 VkImageCreateFlags fl, VkImageFormatProperties *p)
{
    if (is_bc(fmt)) { if (p) memset(p, 0, sizeof *p); hid("vkGetPhysicalDeviceImageFormatProperties"); return VK_ERROR_FORMAT_NOT_SUPPORTED; }
    return r_gpd_ifmt(pd, fmt, t, tl, u, fl, p);
}
static VKAPI_ATTR VkResult VKAPI_CALL w_gpd_ifmt2(VkPhysicalDevice pd, const VkPhysicalDeviceImageFormatInfo2 *i, VkImageFormatProperties2 *p)
{
    if (i && is_bc(i->format)) { if (p) memset(&p->imageFormatProperties, 0, sizeof p->imageFormatProperties); hid("vkGetPhysicalDeviceImageFormatProperties2"); return VK_ERROR_FORMAT_NOT_SUPPORTED; }
    return r_gpd_ifmt2(pd, i, p);
}
static VKAPI_ATTR VkResult VKAPI_CALL w_gpd_ifmt2_khr(VkPhysicalDevice pd, const VkPhysicalDeviceImageFormatInfo2 *i, VkImageFormatProperties2 *p)
{
    if (i && is_bc(i->format)) { if (p) memset(&p->imageFormatProperties, 0, sizeof p->imageFormatProperties); hid("vkGetPhysicalDeviceImageFormatProperties2KHR"); return VK_ERROR_FORMAT_NOT_SUPPORTED; }
    return r_gpd_ifmt2_khr(pd, i, p);
}
unsigned long f2_vk_bc_hidden(void) { return s_bc_hidden; }

void f2_screen_note_swapchain(uint32_t w, uint32_t h);
static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void join(char *out, size_t n, uint32_t count, const char *const *names)
{
    size_t w = 0;
    out[0] = 0;
    for (uint32_t i = 0; i < count && names && w + 2 < n; i++)
        w += snprintf(out + w, n - w, "%s%s", i ? " " : "", names[i] ? names[i] : "?");
}

static VKAPI_ATTR VkResult VKAPI_CALL w_create_instance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *al, VkInstance *inst)
{
    char ext[700], lay[300];
    join(ext, sizeof ext, ci ? ci->enabledExtensionCount : 0, ci ? ci->ppEnabledExtensionNames : NULL);
    join(lay, sizeof lay, ci ? ci->enabledLayerCount : 0, ci ? ci->ppEnabledLayerNames : NULL);
    const VkApplicationInfo *ai = ci ? ci->pApplicationInfo : NULL;
    VkResult r = r_create_instance(ci, al, inst);
    f2_log("F2: vkCreateInstance -> %d | engine %s, api request %u.%u.%u | extensions: %s | layers: %s", r, ai && ai->pEngineName ? ai->pEngineName : "?",
           ai ? VK_VERSION_MAJOR(ai->apiVersion) : 0, ai ? VK_VERSION_MINOR(ai->apiVersion) : 0, ai ? VK_VERSION_PATCH(ai->apiVersion) : 0, ext, lay);
    if (r == VK_SUCCESS && inst) { s_instance = *inst; f2_once_milestone(M_VK_INSTANCE, "graphics API = VULKAN: vkCreateInstance ok"); }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL w_create_device(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *al, VkDevice *dev)
{
    char ext[900];
    join(ext, sizeof ext, ci ? ci->enabledExtensionCount : 0, ci ? ci->ppEnabledExtensionNames : NULL);
    VkResult r = r_create_device(pd, ci, al, dev);
    VkPhysicalDeviceProperties pp;
    memset(&pp, 0, sizeof pp);
    PFN_vkGetPhysicalDeviceProperties gp = r_gipa && s_instance ? (PFN_vkGetPhysicalDeviceProperties)r_gipa(s_instance, "vkGetPhysicalDeviceProperties") : NULL;
    if (gp) gp(pd, &pp);
    f2_log("F2: vkCreateDevice -> %d | %s, vendor 0x%x device 0x%x, api %u.%u.%u, driver 0x%x | extensions: %s", r, pp.deviceName, pp.vendorID, pp.deviceID,
           VK_VERSION_MAJOR(pp.apiVersion), VK_VERSION_MINOR(pp.apiVersion), VK_VERSION_PATCH(pp.apiVersion), pp.driverVersion, ext);
    if (ci && ci->pEnabledFeatures) {
        const VkPhysicalDeviceFeatures *f = ci->pEnabledFeatures;
        f2_log("F2: vkCreateDevice features: BC %d ASTC %d ETC2 %d geometry %d tess %d samplerAniso %d depthClamp %d shaderClipDistance %d shaderCullDistance %d "
               "fragmentStoresAndAtomics %d", f->textureCompressionBC, f->textureCompressionASTC_LDR, f->textureCompressionETC2, f->geometryShader,
               f->tessellationShader, f->samplerAnisotropy, f->depthClamp, f->shaderClipDistance, f->shaderCullDistance, f->fragmentStoresAndAtomics);
    }
    if (f2_cfg.bc_hide) f2_log("F2: bc_hide: %lu BC queries answered 'not supported' before vkCreateDevice%s", s_bc_hidden,
                               s_bc_hidden ? "" : "  [RED: the player asked nothing we could hide: check vkwrap=1]");
    if (r == VK_SUCCESS) f2_once_milestone(M_VK_DEVICE, "Vulkan device created: %s (api %u.%u.%u)", pp.deviceName, VK_VERSION_MAJOR(pp.apiVersion),
                                           VK_VERSION_MINOR(pp.apiVersion), VK_VERSION_PATCH(pp.apiVersion));
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL w_create_shader(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *al, VkShaderModule *m)
{
    VkResult r = r_create_shader(d, ci, al, m);
    if (r == VK_SUCCESS) {
        unsigned long n = __atomic_add_fetch(&f2_cnt_shaders, 1, __ATOMIC_RELAXED);
        if (n == 1) f2_once_milestone(M_FIRST_SHADER, "first vkCreateShaderModule ok (%zu bytes, SPIR-V version word 0x%x, generator 0x%x)", ci->codeSize,
                                      ci->codeSize >= 12 ? ci->pCode[1] : 0, ci->codeSize >= 12 ? ci->pCode[2] : 0);
    } else {
        unsigned long n = __atomic_add_fetch(&f2_cnt_shader_fail, 1, __ATOMIC_RELAXED);
        if (n <= 40) f2_log("F2: [RED] vkCreateShaderModule FAILED %d (%zu bytes, words %08x %08x %08x)", r, ci ? ci->codeSize : 0,
                            ci && ci->codeSize >= 12 ? ci->pCode[0] : 0, ci && ci->codeSize >= 12 ? ci->pCode[1] : 0, ci && ci->codeSize >= 12 ? ci->pCode[2] : 0);
    }
    return r;
}

static double s_pipe_ms;
static VKAPI_ATTR VkResult VKAPI_CALL w_create_gp(VkDevice d, VkPipelineCache c, uint32_t count, const VkGraphicsPipelineCreateInfo *ci,
                                                  const VkAllocationCallbacks *al, VkPipeline *p)
{
    double t = now_ms();
    VkResult r = r_create_gp(d, c, count, ci, al, p);
    t = now_ms() - t;
    s_pipe_ms += t;
    if (r == VK_SUCCESS) {
        unsigned long n = __atomic_add_fetch(&f2_cnt_pipelines, count, __ATOMIC_RELAXED);
        if (n == count) f2_once_milestone(M_FIRST_PIPELINE, "first vkCreateGraphicsPipelines ok (%.1f ms): our SPIR-V went through the driver's compiler", t);
        if (t > 200) f2_log("F2: slow graphics pipeline: %.0f ms (total %.0f ms in %lu pipelines)", t, s_pipe_ms, n);
    } else {
        unsigned long n = __atomic_add_fetch(&f2_cnt_pipeline_fail, 1, __ATOMIC_RELAXED);
        if (n <= 40) f2_log("F2: [RED] vkCreateGraphicsPipelines FAILED %d (stages %u, %.1f ms)", r, ci ? ci->stageCount : 0, t);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL w_create_cp(VkDevice d, VkPipelineCache c, uint32_t count, const VkComputePipelineCreateInfo *ci,
                                                  const VkAllocationCallbacks *al, VkPipeline *p)
{
    VkResult r = r_create_cp(d, c, count, ci, al, p);
    static unsigned ok, bad;
    if (r == VK_SUCCESS) { if (ok++ < 3) f2_log("F2: vkCreateComputePipelines ok (#%u; GPU skinning kernels are compute)", ok); }
    else if (bad++ < 20) f2_log("F2: [RED] vkCreateComputePipelines FAILED %d", r);
    return r;
}

/* ---- rp_log=1 (f2_config.txt, off by default): what the player's own render passes cost a tile GPU ----
 * The 2018.1 Vulkan backend builds every VkRenderPass itself (vk::RenderPasses::GetRenderPass, Release libunity+0x659ad0): per attachment
 * loadOp LOAD / CLEAR / DONT_CARE and storeOp STORE / DONT_CARE come from the engine's GfxRTLoadAction / GfxRTStoreAction (camera clear
 * flags, RenderTexture.DiscardContents, the backbuffer's depth), stencil ops = depth ops, no STORE_OP_NONE (api 1.0, no extension).
 * These wrappers only count: every 5 s, per frame, how many passes begin, how many attachments are loaded from / stored to memory and
 * how many bytes that is (render area x format size x samples), the pipeline barriers and their stages, and the passes that move the most
 * bytes.  On a tiler (Mali, PowerVR) a LOAD reads the attachment into the tile memory and a STORE writes it back: that is the traffic the
 * DOOM / D3 NOLOAD / STORE_DEAD switches remove.  The pass structure is the engine's, the same on every GPU, so an Adreno run shows it. */
static PFN_vkCreateFramebuffer r_create_fb;
static PFN_vkCmdBeginRenderPass r_begin_rp;
static PFN_vkCmdPipelineBarrier r_barrier;
typedef struct { uint64_t h; unsigned id, natt, nres; uint8_t fmt_b[8], samples[8], load[8], store[8], depth[8];
                 unsigned long w_n; double w_bytes; } rp_ent_t;
typedef struct { uint64_t h; uint32_t w, hgt; } fb_ent_t;
#define RP_CAP 2048
#define FB_CAP 8192
static rp_ent_t s_rp[RP_CAP];
static fb_ent_t s_fb[FB_CAP];
static unsigned s_rp_ids;
static pthread_mutex_t s_rp_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { unsigned long begins, ld[3], st[2], dld[3], dst[2], msaa, resolve, partial, bar, bar_all, bar_img, bar_top;
                double b_cl, b_cs, b_dl, b_ds; } s_rw;
static unsigned fmt_bytes(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SRGB: case VK_FORMAT_S8_UINT: return 1;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM: case VK_FORMAT_D16_UNORM: case VK_FORMAT_R5G6B5_UNORM_PACK16:
    case VK_FORMAT_B5G6R5_UNORM_PACK16: case VK_FORMAT_R4G4B4A4_UNORM_PACK16: case VK_FORMAT_A1R5G5B5_UNORM_PACK16: return 2;
    case VK_FORMAT_D16_UNORM_S8_UINT: return 3;
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R32G32_SFLOAT: return 8;
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return 5;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    default: return 4;
    }
}
static size_t rp_slot(uint64_t h, size_t cap) { return (size_t)((h * 0x9e3779b97f4a7c15ull) >> 40) & (cap - 1); }
static rp_ent_t *rp_find(uint64_t h, int add)
{
    size_t j = rp_slot(h, RP_CAP);
    for (size_t k = 0; k < RP_CAP; k++, j = (j + 1) & (RP_CAP - 1)) {
        if (s_rp[j].h == h) return &s_rp[j];
        if (!s_rp[j].h) { if (!add) return NULL; memset(&s_rp[j], 0, sizeof s_rp[j]); s_rp[j].h = h; return &s_rp[j]; }
    }
    return NULL;
}
static fb_ent_t *fb_find(uint64_t h, int add)
{
    size_t j = rp_slot(h, FB_CAP);
    for (size_t k = 0; k < 64; k++, j = (j + 1) & (FB_CAP - 1)) {
        if (s_fb[j].h == h) return &s_fb[j];
        if (!s_fb[j].h) { if (!add) return NULL; s_fb[j].h = h; return &s_fb[j]; }
    }
    if (add) { j = rp_slot(h, FB_CAP); s_fb[j].h = h; return &s_fb[j]; }     /* a long run: overwrite the home slot */
    return NULL;
}
static const char *const k_ld[3] = { "L", "C", "-" }, *const k_st[2] = { "S", "-" };
static void rp_desc(const rp_ent_t *e, char *out, size_t n)
{
    size_t w = 0; out[0] = 0;
    for (unsigned i = 0; i < e->natt && w + 24 < n; i++)
        w += (size_t)snprintf(out + w, n - w, "%s%s%uB%s%s%s", i ? " " : "", e->depth[i] ? "z" : "c", e->fmt_b[i], e->samples[i] > 1 ? "xN" : "",
                              e->load[i] < 3 ? k_ld[e->load[i]] : "?", e->store[i] < 2 ? k_st[e->store[i]] : "?");
    if (e->nres && w + 12 < n) snprintf(out + w, n - w, " +%u resolve", e->nres);
}
static void rp_note_create(const VkRenderPassCreateInfo *ci, VkRenderPass p)
{
    if (!ci || !p) return;
    pthread_mutex_lock(&s_rp_mu);
    rp_ent_t *e = rp_find((uint64_t)p, 1);
    if (e) {
        uint64_t h = e->h;
        memset(e, 0, sizeof *e); e->h = h;
        e->id = ++s_rp_ids;
        e->natt = ci->attachmentCount < 8 ? ci->attachmentCount : 8;
        for (unsigned i = 0; i < e->natt; i++) {
            const VkAttachmentDescription *a = &ci->pAttachments[i];
            e->depth[i] = a->format >= VK_FORMAT_D16_UNORM && a->format <= VK_FORMAT_D32_SFLOAT_S8_UINT;
            e->fmt_b[i] = (uint8_t)fmt_bytes(a->format); e->samples[i] = (uint8_t)a->samples;
            e->load[i] = (uint8_t)a->loadOp; e->store[i] = (uint8_t)a->storeOp;
            if (a->format == VK_FORMAT_S8_UINT) { e->load[i] = (uint8_t)a->stencilLoadOp; e->store[i] = (uint8_t)a->stencilStoreOp; }
        }
        for (unsigned s = 0; s < ci->subpassCount; s++) if (ci->pSubpasses[s].pResolveAttachments)
            for (unsigned c = 0; c < ci->pSubpasses[s].colorAttachmentCount; c++)
                if (ci->pSubpasses[s].pResolveAttachments[c].attachment != VK_ATTACHMENT_UNUSED) e->nres++;
        if (e->id <= 120) {
            char d[200]; rp_desc(e, d, sizeof d);
            f2_log("F2: rp_log: render pass #%u: %s (%u subpasses; c/z colour/depth, bytes per pixel, L/C/- load/clear/dont-care, S/- store/dont-care)",
                   e->id, d, ci->subpassCount);
        }
    }
    pthread_mutex_unlock(&s_rp_mu);
}
static VKAPI_ATTR VkResult VKAPI_CALL w_create_fb(VkDevice d, const VkFramebufferCreateInfo *ci, const VkAllocationCallbacks *al, VkFramebuffer *fb)
{
    VkResult r = r_create_fb(d, ci, al, fb);
    if (r == VK_SUCCESS && fb && ci) {
        pthread_mutex_lock(&s_rp_mu);
        fb_ent_t *e = fb_find((uint64_t)*fb, 1);
        if (e) { e->w = ci->width; e->hgt = ci->height; }
        pthread_mutex_unlock(&s_rp_mu);
    }
    return r;
}
static VKAPI_ATTR void VKAPI_CALL w_begin_rp(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, VkSubpassContents c)
{
    if (bi) {
        pthread_mutex_lock(&s_rp_mu);
        rp_ent_t *e = rp_find((uint64_t)bi->renderPass, 0);
        fb_ent_t *f = fb_find((uint64_t)bi->framebuffer, 0);
        double px = (double)bi->renderArea.extent.width * bi->renderArea.extent.height;
        s_rw.begins++;
        if (f && (bi->renderArea.extent.width < f->w || bi->renderArea.extent.height < f->hgt)) s_rw.partial++;
        if (e) {
            double moved = 0;
            for (unsigned i = 0; i < e->natt; i++) {
                double b = px * e->fmt_b[i] * e->samples[i];
                unsigned l = e->load[i] < 3 ? e->load[i] : 0, s = e->store[i] < 2 ? e->store[i] : 0;
                if (e->samples[i] > 1) s_rw.msaa++;
                if (e->depth[i]) { s_rw.dld[l]++; s_rw.dst[s]++; if (!l) s_rw.b_dl += b; if (!s) s_rw.b_ds += b; }
                else { s_rw.ld[l]++; s_rw.st[s]++; if (!l) s_rw.b_cl += b; if (!s) s_rw.b_cs += b; }
                moved += (!l ? b : 0) + (!s ? b : 0);
            }
            s_rw.resolve += e->nres;
            e->w_n++; e->w_bytes += moved;
        }
        pthread_mutex_unlock(&s_rp_mu);
    }
    r_begin_rp(cb, bi, c);
}
static VKAPI_ATTR void VKAPI_CALL w_barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags df,
                                            uint32_t nm, const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b,
                                            uint32_t ni, const VkImageMemoryBarrier *im)
{
    __atomic_add_fetch(&s_rw.bar, 1, __ATOMIC_RELAXED);
    if ((src | dst) & (VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT)) __atomic_add_fetch(&s_rw.bar_all, 1, __ATOMIC_RELAXED);
    if (dst & (VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT))
        __atomic_add_fetch(&s_rw.bar_top, 1, __ATOMIC_RELAXED);
    if (ni) __atomic_add_fetch(&s_rw.bar_img, ni, __ATOMIC_RELAXED);
    r_barrier(cb, src, dst, df, nm, m, nb, b, ni, im);
}
/* called from w_present every 5 s with the frames of that window */
static void rp_report(unsigned frames, double secs)
{
    if (!frames || secs <= 0) return;
    pthread_mutex_lock(&s_rp_mu);
    const double F = frames, MB = 1048576.0, tot = s_rw.b_cl + s_rw.b_cs + s_rw.b_dl + s_rw.b_ds;
    f2_log("F2: rp_log: per frame %.1f passes (%.1f with a partial render area); colour attachments load %.1f clear %.1f dont-care %.1f, store %.1f "
           "dont-care %.1f; depth load %.1f clear %.1f dont-care %.1f, store %.1f dont-care %.1f; msaa %.1f resolve %.1f",
           s_rw.begins / F, s_rw.partial / F, s_rw.ld[0] / F, s_rw.ld[1] / F, s_rw.ld[2] / F, s_rw.st[0] / F, s_rw.st[1] / F,
           s_rw.dld[0] / F, s_rw.dld[1] / F, s_rw.dld[2] / F, s_rw.dst[0] / F, s_rw.dst[1] / F, s_rw.msaa / F, s_rw.resolve / F);
    f2_log("F2: rp_log: per frame tile traffic MB: colour load %.1f store %.1f, depth load %.1f store %.1f (total %.1f MB per frame, %.0f MB/s); "
           "barriers %.1f (all-commands / all-graphics %.1f, waiting at the vertex stages or earlier %.1f, image barriers %.1f)",
           s_rw.b_cl / F / MB, s_rw.b_cs / F / MB, s_rw.b_dl / F / MB, s_rw.b_ds / F / MB, tot / F / MB, tot / MB / secs,
           s_rw.bar / F, s_rw.bar_all / F, s_rw.bar_top / F, s_rw.bar_img / F);
    for (int k = 0; k < 8; k++) {             /* the passes that move the most bytes */
        rp_ent_t *best = NULL;
        for (size_t j = 0; j < RP_CAP; j++) if (s_rp[j].h && s_rp[j].w_bytes > 0 && (!best || s_rp[j].w_bytes > best->w_bytes)) best = &s_rp[j];
        if (!best) break;
        char d[200]; rp_desc(best, d, sizeof d);
        f2_log("F2: rp_log:   top %d: pass #%u x%.1f per frame, %.1f MB per frame: %s", k + 1, best->id, best->w_n / F, best->w_bytes / F / MB, d);
        best->w_bytes = -1;               /* taken; all cleared below */
    }
    for (size_t j = 0; j < RP_CAP; j++) { s_rp[j].w_n = 0; s_rp[j].w_bytes = 0; }
    memset(&s_rw, 0, sizeof s_rw);
    pthread_mutex_unlock(&s_rp_mu);
}

static VKAPI_ATTR VkResult VKAPI_CALL w_create_rp(VkDevice d, const VkRenderPassCreateInfo *ci, const VkAllocationCallbacks *al, VkRenderPass *p)
{
    VkResult r = r_create_rp(d, ci, al, p);
    static unsigned bad;
    if (r != VK_SUCCESS && bad++ < 20) f2_log("F2: [RED] vkCreateRenderPass FAILED %d (attachments %u)", r, ci ? ci->attachmentCount : 0);
    if (r == VK_SUCCESS && p && f2_cfg.rp_log) rp_note_create(ci, *p);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL w_create_swapchain(VkDevice d, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *al, VkSwapchainKHR *s)
{
    /* the frame cap runs with vSyncCount 0 (f2_screen.c): whatever present mode the engine picks for that, the display's own pace (FIFO) is kept */
    VkSwapchainCreateInfoKHR fifo;
    if (ci && ci->presentMode != VK_PRESENT_MODE_FIFO_KHR) {
        static int said;
        if (!said) { said = 1; f2_log("F2: vkCreateSwapchainKHR: present mode %d asked -> FIFO", ci->presentMode); }
        fifo = *ci; fifo.presentMode = VK_PRESENT_MODE_FIFO_KHR; ci = &fifo;
    }
    VkResult r = r_create_swapchain(d, ci, al, s);
    f2_log("F2: vkCreateSwapchainKHR -> %d | %ux%u format %d colorspace %d images>=%u present mode %d transform 0x%x", r, ci ? ci->imageExtent.width : 0,
           ci ? ci->imageExtent.height : 0, ci ? ci->imageFormat : 0, ci ? ci->imageColorSpace : 0, ci ? ci->minImageCount : 0, ci ? ci->presentMode : 0,
           ci ? ci->preTransform : 0);
    if (r == VK_SUCCESS && ci) f2_screen_note_swapchain(ci->imageExtent.width, ci->imageExtent.height);
    if (r == VK_SUCCESS) f2_once_milestone(M_SWAPCHAIN, "swapchain %ux%u created", ci->imageExtent.width, ci->imageExtent.height);
    return r;
}

static double s_last_present, s_window_start;
static unsigned s_window_frames;
/* frame statistics for the on-screen FPS counter (Java DmActivity.nativeFrameStats): {fps, mean ms, max ms, GPU ms (unknown = 0)} of the last 0.5 s */
static double s_st_start, s_st_max;
static unsigned s_st_frames;
static float s_stats[4];
void f2_frame_stats(float out[4])
{
    if (now_ms() - s_last_present > 1500) { out[0] = out[1] = out[2] = out[3] = 0; return; }
    for (int i = 0; i < 4; i++) out[i] = s_stats[i];
}

/* The 2018.1 Vulkan backend creates the swapchain with the identity transform; on a phone held in landscape the surface's
 * own transform is a rotation, so every present / acquire answers VK_SUBOPTIMAL_KHR, and the engine logs each one with a
 * 10-frame backtrace (VKSwapChain.cpp:477; 1978 times in 45 s of phone run 05).  The picture is right (the compositor
 * rotates it), there is nothing the engine could do about it, so the driver's hint is not passed on. */
/* the present pacer (f2_screen.c, pace=1): a fixed grid of 1 / fps; a present more than a frame late starts the grid again from itself */
extern long long f2_pace_ns;
static void pace_wait(long long per)
{
    static long long next;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    long long now = ts.tv_sec * 1000000000ll + ts.tv_nsec;
    if (!next || now - next > per || next - now > 2 * per) next = now;
    else if (next > now) {
        ts.tv_sec = (time_t)(next / 1000000000ll); ts.tv_nsec = (long)(next % 1000000000ll);
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
    }
    next += per;
}
static VKAPI_ATTR VkResult VKAPI_CALL w_present(VkQueue q, const VkPresentInfoKHR *pi)
{
    long long pace = __atomic_load_n(&f2_pace_ns, __ATOMIC_ACQUIRE);
    if (pace > 0) pace_wait(pace);
    VkResult r = r_present(q, pi);
    unsigned long n = __atomic_add_fetch(&f2_cnt_frames, 1, __ATOMIC_RELAXED);
    double t = now_ms();
    if (n == 1) { f2_once_milestone(M_FIRST_FRAME, "FIRST FRAME presented (vkQueuePresentKHR -> %d)", r); s_window_start = t; s_st_start = t; }
    if (n == 100) f2_once_milestone(M_FRAMES_100, "100 frames presented");
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { static unsigned bad; if (bad++ < 20) f2_log("F2: vkQueuePresentKHR -> %d", r); }
    if (r == VK_SUBOPTIMAL_KHR) {
        static int said;
        if (!said) { said = 1; f2_log("F2: vkQueuePresentKHR answers VK_SUBOPTIMAL_KHR (identity swapchain on a rotated surface): reported to the engine as VK_SUCCESS from now on"); }
        if (pi && pi->pResults) for (uint32_t i = 0; i < pi->swapchainCount; i++) if (pi->pResults[i] == VK_SUBOPTIMAL_KHR) pi->pResults[i] = VK_SUCCESS;
        r = VK_SUCCESS;
    }
    s_window_frames++;
    /* pacing diagnostics (the 30 fps judder, 16.11.2026): intervals between present calls in buckets of the 120 Hz grid */
    static double s_pp_last; static unsigned s_pp_b[8]; static double s_pp_min = 1e9, s_pp_max;
    if (s_pp_last > 0) {
        double d = t - s_pp_last;
        static const double edge[7] = { 12.5, 20.8, 29.2, 37.5, 45.8, 54.2, 70.0 };     /* ~1, 2, 3, 4, 5, 6 ticks of 8.33 ms, more */
        int b = 0; while (b < 7 && d >= edge[b]) b++;
        s_pp_b[b]++; if (d < s_pp_min) s_pp_min = d; if (d > s_pp_max) s_pp_max = d;
    }
    s_pp_last = t;
    if (t - s_window_start >= 5000) {
        if (f2_cfg.pacelog) f2_log("F2: pacing: present-to-present ms <12.5:%u <20.8:%u <29.2:%u <37.5:%u <45.8:%u <54.2:%u <70:%u more:%u (min %.1f max %.1f)",
               s_pp_b[0], s_pp_b[1], s_pp_b[2], s_pp_b[3], s_pp_b[4], s_pp_b[5], s_pp_b[6], s_pp_b[7], s_pp_min, s_pp_max);
        for (int i = 0; i < 8; i++) s_pp_b[i] = 0; s_pp_min = 1e9; s_pp_max = 0;
        f2_log("F2: frames: %lu total, %.1f fps over the last %.1f s (shaders %lu, pipelines %lu, pipeline time %.0f ms)", n,
               s_window_frames * 1000.0 / (t - s_window_start), (t - s_window_start) / 1000.0, f2_cnt_shaders, f2_cnt_pipelines, s_pipe_ms);
        f2_screen_note_fps((float)(s_window_frames * 1000.0 / (t - s_window_start)));
        if (f2_cfg.rp_log) rp_report(s_window_frames, (t - s_window_start) / 1000.0);      /* the frame cap checks itself against this */
        s_window_start = t; s_window_frames = 0;
    }
    if (s_last_present > 0 && t - s_last_present > s_st_max) s_st_max = t - s_last_present;
    s_st_frames++;
    if (t - s_st_start >= 500) {
        double dt = t - s_st_start;
        s_stats[0] = (float)(s_st_frames * 1000.0 / dt); s_stats[1] = (float)(dt / s_st_frames); s_stats[2] = (float)s_st_max; s_stats[3] = 0;
        s_st_start = t; s_st_frames = 0; s_st_max = 0;
    }
    s_last_present = t;
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL w_acquire(VkDevice d, VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, VkFence fence, uint32_t *index)
{
    VkResult r = r_acquire(d, sc, timeout, sem, fence, index);
    return r == VK_SUBOPTIMAL_KHR ? VK_SUCCESS : r;
}

static void *wrap(const char *name, void *real);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL w_gipa(VkInstance inst, const char *name)
{
    PFN_vkVoidFunction p = r_gipa(inst, name);
    if (!p || !name) return p;
    return (PFN_vkVoidFunction)wrap(name, (void *)p);
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL w_gdpa(VkDevice dev, const char *name)
{
    PFN_vkVoidFunction p = r_gdpa(dev, name);
    if (!p || !name) return p;
    return (PFN_vkVoidFunction)wrap(name, (void *)p);
}

static void *wrap(const char *name, void *real)
{
    if (!real) return real;
#define W(n, r, w) if (!strcmp(name, n)) { r = (__typeof__(r))real; return (void *)w; }
    W("vkGetInstanceProcAddr", r_gipa, w_gipa)
    W("vkGetDeviceProcAddr", r_gdpa, w_gdpa)
    W("vkCreateInstance", r_create_instance, w_create_instance)
    W("vkCreateDevice", r_create_device, w_create_device)
    W("vkCreateShaderModule", r_create_shader, w_create_shader)
    W("vkCreateGraphicsPipelines", r_create_gp, w_create_gp)
    W("vkCreateComputePipelines", r_create_cp, w_create_cp)
    W("vkCreateRenderPass", r_create_rp, w_create_rp)
    W("vkCreateSwapchainKHR", r_create_swapchain, w_create_swapchain)
    W("vkQueuePresentKHR", r_present, w_present)
    W("vkAcquireNextImageKHR", r_acquire, w_acquire)
    if (f2_cfg.rp_log) {
        W("vkCreateFramebuffer", r_create_fb, w_create_fb)
        W("vkCmdBeginRenderPass", r_begin_rp, w_begin_rp)
        W("vkCmdPipelineBarrier", r_barrier, w_barrier)
    }
    if (f2_cfg.bc_hide) {
        W("vkGetPhysicalDeviceFeatures", r_gpd_features, w_gpd_features)
        W("vkGetPhysicalDeviceFeatures2", r_gpd_features2, w_gpd_features2)
        W("vkGetPhysicalDeviceFeatures2KHR", r_gpd_features2_khr, w_gpd_features2_khr)
        W("vkGetPhysicalDeviceFormatProperties", r_gpd_fmt, w_gpd_fmt)
        W("vkGetPhysicalDeviceFormatProperties2", r_gpd_fmt2, w_gpd_fmt2)
        W("vkGetPhysicalDeviceFormatProperties2KHR", r_gpd_fmt2_khr, w_gpd_fmt2_khr)
        W("vkGetPhysicalDeviceImageFormatProperties", r_gpd_ifmt, w_gpd_ifmt)
        W("vkGetPhysicalDeviceImageFormatProperties2", r_gpd_ifmt2, w_gpd_ifmt2)
        W("vkGetPhysicalDeviceImageFormatProperties2KHR", r_gpd_ifmt2_khr, w_gpd_ifmt2_khr)
    }
#undef W
    return real;
}

void *f2_vk_wrap_symbol(const char *name, void *real)
{
    static int said;
    if (!said) { said = 1; f2_log("F2: libunity is loading the Vulkan library (first symbol %s)", name); }
    return wrap(name, real);
}

/* ---- EGL: only to see a GLES fallback (our shader data has Vulkan programs only) ---- */
static void *(*r_egl_create_context)(void *, void *, void *, const int *);
static unsigned (*r_egl_swap)(void *, void *);

static void *w_egl_create_context(void *dpy, void *cfg, void *share, const int *attr)
{
    void *c = r_egl_create_context(dpy, cfg, share, attr);
    int ver = 0;
    for (const int *a = attr; a && a[0] != 0x3038 /* EGL_NONE */; a += 2) if (a[0] == 0x3098 /* EGL_CONTEXT_CLIENT_VERSION */) ver = a[1];
    f2_once_milestone(M_GLES, "eglCreateContext(GLES %d) -> %p : the engine is creating a GLES context (a probe is normal; if no Vulkan device follows, "
                      "it FELL BACK TO GLES and every game shader will be unsupported)", ver, c);
    return c;
}
static unsigned w_egl_swap(void *dpy, void *surf)
{
    static unsigned n;
    if (n++ == 0) f2_log("F2: [RED] eglSwapBuffers: frames are presented through GLES, not Vulkan");
    __atomic_add_fetch(&f2_cnt_frames, 1, __ATOMIC_RELAXED);
    return r_egl_swap(dpy, surf);
}

void *f2_egl_hook(const char *name)
{
    if (!strcmp(name, "eglCreateContext")) return (void *)w_egl_create_context;
    if (!strcmp(name, "eglSwapBuffers")) return (void *)w_egl_swap;
    return NULL;
}
void f2_egl_set_real(const char *name, void *real)
{
    if (!strcmp(name, "eglCreateContext")) r_egl_create_context = (void *(*)(void *, void *, void *, const int *))real;
    else if (!strcmp(name, "eglSwapBuffers")) r_egl_swap = (unsigned (*)(void *, void *))real;
}
