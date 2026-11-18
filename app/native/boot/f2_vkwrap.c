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
#include <stdarg.h>
#include <stdlib.h>
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

static void rp_device(VkDevice d, uint32_t vendor);
static void rp_note_pipelines(uint32_t count, const VkGraphicsPipelineCreateInfo *ci, const VkPipeline *p);
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
    if (r == VK_SUCCESS && dev) rp_device(*dev, pp.vendorID);
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
        rp_note_pipelines(count, ci, p);
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

/* ==== render passes: rp_log (diagnostics, f2_config rp_log=1) and rp_opt (tile-GPU savings) ====
 * The 2018.1 Vulkan backend builds every VkRenderPass itself (vk::RenderPasses::GetRenderPass, Release libunity+0x659ad0): per attachment
 * loadOp LOAD / CLEAR / DONT_CARE and storeOp STORE / DONT_CARE come from GfxRTLoadAction / GfxRTStoreAction (camera clear flags,
 * RenderTexture.DiscardContents), stencil ops = depth ops, no STORE_OP_NONE (api 1.0).  A frame of a level (phone runs 18.11, PROGRESS [43]):
 * 21 passes, each in its own command buffer, one submit, 371 MiB of tile load / store per frame at 2000x900, much of it dead: depth that
 * nobody tests or samples is loaded and stored around full-screen blits, a clear-only pass stores what the next pass loads, the swapchain
 * pass loads and stores a depth buffer used nowhere else.  On a tiler (Mali, PowerVR; Adreno in GMEM mode) every LOAD reads the
 * attachment from memory into the tile and every STORE writes it back.
 *
 * rp_opt (f2_config rp_opt: -1 auto = 7 on ARM Mali / Imagination PowerVR, otherwise 0; bits 1 dead loads / stores, 2 clear fold,
 * 4 checked colour NOLOAD) learns the frame.  Every frame records the passes (render pass, attachment images, the original ops, how the
 * draws use each attachment) and the other touches of those images (sampled by a draw or a dispatch through a descriptor set, copied,
 * blitted, cleared).  At the present a liveness pass over two copies of the frame (the next frame counts) finds
 *   - stores whose contents nobody reads before the image is cleared or loaded with DONT_CARE,
 *   - loads whose contents the pass does not need (no depth / stencil test, no draw needing the old colour) and nobody needs after it,
 *   - a clear-only pass (no draws, no clears) followed by a pass that loads the same image: that load becomes a CLEAR with the clear
 *     values the clear-only pass used in THIS frame, and the clear-only pass's store dies (D3's "CLEAR twin"),
 *   - colour loads that the pass's first draw overwrites (opaque, full write mask, full viewport and scissor, 3-6 vertices, one instance,
 *     no depth / stencil test: a full-screen blit) or a full vkCmdClearAttachments: those are checked in THIS frame: the pass's
 *     vkCmdBeginRenderPass is held back until its first draw / clear (binding and dynamic-state commands are legal outside a pass), and
 *     the override is dropped (for good) when that command does not cover the attachment.  Such loads never make an earlier store die.
 * Overrides are applied once the frame structure (the passes and their images, in order) has been the same for 8 frames, and only those
 * found in every frame since; a structure change stops them at once (the pass where the frame leaves the plan and all after it run as
 * the engine asked).  They are applied by beginning a VkRenderPass variant that differs only in load / store ops: render pass
 * compatibility ignores those, so the same framebuffer and pipelines work.  Passes with several subpasses, resolves or secondary command
 * buffers, attachments of > 8-attachment passes and frames whose pass command buffers are submitted in another order are left alone.
 * Barriers are not used (Unity puts each pass's barriers in a command buffer of their own submitted before it, recorded after it). */
static PFN_vkCreateFramebuffer r_create_fb;
static PFN_vkCmdBeginRenderPass r_begin_rp;
static PFN_vkCmdPipelineBarrier r_barrier;
static PFN_vkCreateImage r_create_image;
static PFN_vkCreateImageView r_create_view;
static PFN_vkGetSwapchainImagesKHR r_sc_images;
static PFN_vkAllocateDescriptorSets r_alloc_ds;
static PFN_vkUpdateDescriptorSets r_upd_ds;
static PFN_vkUpdateDescriptorSetWithTemplate r_upd_tmpl;
static PFN_vkUpdateDescriptorSetWithTemplate r_upd_tmpl_khr;
static PFN_vkCmdBindDescriptorSets r_bind_ds;
static PFN_vkCmdBindPipeline r_bind_pipe;
static PFN_vkCmdSetViewport r_set_vp;
static PFN_vkCmdSetScissor r_set_sc;
static PFN_vkCmdEndRenderPass r_end_rp;
static PFN_vkCmdNextSubpass r_next_sp;
static PFN_vkCmdDraw r_draw;
static PFN_vkCmdDrawIndexed r_draw_ix;
static PFN_vkCmdDrawIndirect r_draw_ind;
static PFN_vkCmdDrawIndexedIndirect r_draw_ix_ind;
static PFN_vkCmdDispatch r_dispatch;
static PFN_vkCmdDispatchIndirect r_dispatch_ind;
static PFN_vkCmdCopyImage r_copy_img;
static PFN_vkCmdBlitImage r_blit_img;
static PFN_vkCmdResolveImage r_resolve_img;
static PFN_vkCmdCopyBufferToImage r_copy_b2i;
static PFN_vkCmdCopyImageToBuffer r_copy_i2b;
static PFN_vkCmdClearColorImage r_clr_c;
static PFN_vkCmdClearDepthStencilImage r_clr_z;
static PFN_vkCmdClearAttachments r_clr_att;
static PFN_vkCmdExecuteCommands r_exec;
static PFN_vkBeginCommandBuffer r_begin_cb;
static PFN_vkEndCommandBuffer r_end_cb;
static PFN_vkQueueSubmit r_submit;

static VkDevice s_dev;
static int s_track;      /* images / passes / draws are followed (rp_log or rp_opt in effect); decided at vkCreateDevice */
static int s_opt;        /* rp_opt feature bits in effect, 0 = off */
static pthread_mutex_t s_rp_mu = PTHREAD_MUTEX_INITIALIZER;

/* ---- tables (open addressing keyed by the Vulkan handle; allocated at vkCreateDevice when s_track) ---- */
typedef struct { uint64_t h; unsigned id, natt, nres, nsub, big; uint8_t fmt_b[8], samples[8], load[8], store[8], depth[8], stencil[8]; int8_t col[8];
                 unsigned long w_n; double w_bytes; VkRenderPassCreateInfo *ci; unsigned nvar; uint32_t vmask[16]; VkRenderPass var[16]; } rp_ent_t;
typedef struct { uint64_t h; uint32_t w, hgt; unsigned id, natt; uint64_t key[8]; } fb_ent_t;
typedef struct { uint64_t h; uint32_t w, hgt, id; uint8_t fmt_b, depth, sc, att; } img_ent_t;
typedef struct { uint64_t h; uint64_t img; } view_ent_t;
#define DS_N 12
typedef struct { uint64_t h; uint8_t n, ovf; uint32_t key[DS_N]; uint64_t img[DS_N]; } ds_ent_t;
typedef struct { uint64_t h; uint8_t ztest, stest, dyn_vp, dyn_sc, has_vp, has_sc, blend, wfull; VkViewport vp; VkRect2D sc; } pipe_ent_t;
typedef struct {
    uint64_t h; unsigned id;
    int in_pass, fp, cplx;                      /* fp: index of the current pass in the frame record, -1 none */
    const rp_ent_t *rpe; unsigned rp, fb, sub, draws, clr, natt, nrd; int32_t rx, ry; uint32_t rw, rh;
    uint64_t att[8], rd[32];                    /* trace: the pass's images, all sampled images */
    uint64_t bound[8]; int rdirty;
    uint64_t pipe_h; VkViewport vp; VkRect2D sc; uint8_t has_vp, has_sc;
    uint64_t ord[16]; unsigned nord;            /* rp_opt: attachment images sampled in the current pass */
    int pend; VkRenderPassBeginInfo pbi; VkClearValue pcv[8]; VkSubpassContents pcont; uint32_t pmask, pvbits; rp_ent_t *prpe; int pord;
} cb_ent_t;
#define RP_CAP 2048
#define FB_CAP 8192
#define IMG_CAP 16384
#define VIEW_CAP 32768
#define DS_CAP 32768
#define PIPE_CAP 8192
#define CB_CAP 1024
static rp_ent_t s_rp[RP_CAP];
static fb_ent_t *s_fb;
static img_ent_t *s_img;
static view_ent_t *s_view;
static ds_ent_t *s_ds;
static pipe_ent_t *s_pipe;
static cb_ent_t *s_cb;
static unsigned s_rp_ids, s_img_ids, s_fb_ids, s_cb_ids;
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
static int is_depth_fmt(VkFormat f) { return f >= VK_FORMAT_D16_UNORM && f <= VK_FORMAT_D32_SFLOAT_S8_UINT; }
static int has_stencil_fmt(VkFormat f) { return f >= VK_FORMAT_S8_UINT && f <= VK_FORMAT_D32_SFLOAT_S8_UINT; }
static size_t tab_slot(uint64_t h, size_t cap) { return (size_t)((h * 0x9e3779b97f4a7c15ull) >> 40) & (cap - 1); }
static int s_tab_full;
static void tab_full(void);
/* never overwrites: a table that cannot take an entry turns rp_opt off for the run (a lost image / view could hide a read) */
static void *tab_find(void *tab, size_t stride, size_t cap, uint64_t h, int add)
{
    if (!tab || !h) return NULL;
    size_t j = tab_slot(h, cap);
    for (size_t k = 0; k < 64; k++, j = (j + 1) & (cap - 1)) {
        uint64_t *e = (uint64_t *)((char *)tab + j * stride);
        if (*e == h) return e;
        if (!*e) { if (!add) return NULL; memset(e, 0, stride); *e = h; return e; }
    }
    if (add) tab_full();
    return NULL;
}
/* removal with backward shift (no tombstones): later entries of the run move into the hole when their home allows it */
static void tab_del(void *tab, size_t stride, size_t cap, uint64_t h)
{
    if (!tab || !h) return;
    size_t m = cap - 1, i = tab_slot(h, cap);
    int found = 0;
    for (size_t k = 0; k < 64; k++, i = (i + 1) & m) {
        uint64_t *e = (uint64_t *)((char *)tab + i * stride);
        if (!*e) return;
        if (*e == h) { found = 1; break; }
    }
    if (!found) return;
    size_t j = i;
    for (;;) {
        j = (j + 1) & m;
        uint64_t *ej = (uint64_t *)((char *)tab + j * stride);
        if (!*ej) break;
        size_t home = tab_slot(*ej, cap);
        int stays = i <= j ? (home > i && home <= j) : (home > i || home <= j);
        if (stays) continue;
        memcpy((char *)tab + i * stride, ej, stride);
        i = j;
    }
    memset((char *)tab + i * stride, 0, stride);
}
static rp_ent_t *rp_find(uint64_t h, int add)
{
    if (!h) return NULL;
    size_t j = tab_slot(h, RP_CAP);
    for (size_t k = 0; k < RP_CAP; k++, j = (j + 1) & (RP_CAP - 1)) {
        if (s_rp[j].h == h) return &s_rp[j];
        if (!s_rp[j].h) { if (!add) return NULL; memset(&s_rp[j], 0, sizeof s_rp[j]); s_rp[j].h = h; return &s_rp[j]; }
    }
    return NULL;
}
#define FIND(tab, cap, h, add) tab_find(tab, sizeof *(tab), cap, (uint64_t)(h), add)
#define DEL(tab, cap, h) tab_del(tab, sizeof *(tab), cap, (uint64_t)(h))
static img_ent_t *img_get(uint64_t h) { return (img_ent_t *)FIND(s_img, IMG_CAP, h, 0); }
static uint64_t view_img(VkImageView v) { view_ent_t *e = (view_ent_t *)FIND(s_view, VIEW_CAP, v, 0); return e ? e->img : 0; }
static cb_ent_t *cb_get(VkCommandBuffer cb)
{
    cb_ent_t *c = (cb_ent_t *)FIND(s_cb, CB_CAP, (uintptr_t)cb, 1);
    if (c && !c->id) { c->id = ++s_cb_ids; c->fp = -1; }
    return c;
}
static const char *const k_ld[3] = { "L", "C", "-" }, *const k_st[2] = { "S", "-" };
static void rp_desc_ops(const rp_ent_t *e, uint32_t mask, char *out, size_t n)
{
    size_t w = 0; out[0] = 0;
    for (unsigned i = 0; i < e->natt && w + 24 < n; i++) {
        unsigned l = e->load[i], s = e->store[i], m = (mask >> (3 * i)) & 7;
        if (m & 1) l = 2;
        if (m & 2) l = 1;
        if (m & 4) s = 1;
        w += (size_t)snprintf(out + w, n - w, "%s%s%uB%s%s%s", i ? " " : "", e->depth[i] ? "z" : "c", e->fmt_b[i], e->samples[i] > 1 ? "xN" : "",
                              l < 3 ? k_ld[l] : "?", s < 2 ? k_st[s] : "?");
    }
    if (e->nres && w + 12 < n) snprintf(out + w, n - w, " +%u resolve", e->nres);
}
static void rp_desc(const rp_ent_t *e, char *out, size_t n) { rp_desc_ops(e, 0, out, n); }

/* ---- rp_opt: the render pass create info kept for variants ---- */
static VkRenderPassCreateInfo *rp_copy(const VkRenderPassCreateInfo *ci)
{
    if (ci->pNext || ci->flags) return NULL;
    size_t n = sizeof *ci + ci->subpassCount * sizeof(VkSubpassDescription) + ci->attachmentCount * sizeof(VkAttachmentDescription)
             + ci->dependencyCount * sizeof(VkSubpassDependency);
    for (uint32_t s = 0; s < ci->subpassCount; s++) {
        const VkSubpassDescription *d = &ci->pSubpasses[s];
        n += (d->inputAttachmentCount + 2 * d->colorAttachmentCount + 1) * sizeof(VkAttachmentReference) + d->preserveAttachmentCount * sizeof(uint32_t);
    }
    char *m = (char *)calloc(1, n + 16);
    if (!m) return NULL;
    VkRenderPassCreateInfo *o = (VkRenderPassCreateInfo *)m;
    char *q = m + sizeof *o;
    *o = *ci;
    VkSubpassDescription *sp = (VkSubpassDescription *)q; q += ci->subpassCount * sizeof *sp;
    VkAttachmentDescription *at = (VkAttachmentDescription *)q; q += ci->attachmentCount * sizeof *at;
    VkSubpassDependency *dp = (VkSubpassDependency *)q; q += ci->dependencyCount * sizeof *dp;
    if (ci->attachmentCount) memcpy(at, ci->pAttachments, ci->attachmentCount * sizeof *at);
    if (ci->dependencyCount) memcpy(dp, ci->pDependencies, ci->dependencyCount * sizeof *dp);
    for (uint32_t s = 0; s < ci->subpassCount; s++) {
        const VkSubpassDescription *d = &ci->pSubpasses[s];
        sp[s] = *d;
#define CP(field, cnt, T) if (d->field && (cnt)) { memcpy(q, d->field, (size_t)(cnt) * sizeof(T)); sp[s].field = (const T *)q; q += (size_t)(cnt) * sizeof(T); }
        CP(pInputAttachments, d->inputAttachmentCount, VkAttachmentReference)
        CP(pColorAttachments, d->colorAttachmentCount, VkAttachmentReference)
        CP(pResolveAttachments, d->colorAttachmentCount, VkAttachmentReference)
        CP(pDepthStencilAttachment, 1, VkAttachmentReference)
        CP(pPreserveAttachments, d->preserveAttachmentCount, uint32_t)
#undef CP
    }
    o->pAttachments = at; o->pSubpasses = sp; o->pDependencies = dp;
    return o;
}
/* mask, 3 bits per attachment a (<< 3a): 1 load -> DONT_CARE, 2 load -> CLEAR, 4 store -> DONT_CARE (stencil ops follow the depth ops) */
static VkRenderPass rp_variant(rp_ent_t *e, uint32_t mask)
{
    for (unsigned i = 0; i < e->nvar; i++) if (e->vmask[i] == mask) return e->var[i];
    if (!e->ci || !s_dev || e->nvar >= 16) return VK_NULL_HANDLE;
    VkAttachmentDescription at[8];
    memcpy(at, e->ci->pAttachments, e->natt * sizeof at[0]);
    for (unsigned a = 0; a < e->natt; a++) {
        uint32_t m = (mask >> (3 * a)) & 7;
        int ds = e->depth[a] || e->stencil[a];
        if (m & 1) { at[a].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; if (ds) at[a].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; }
        if (m & 2) { at[a].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; if (ds) at[a].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; }
        if (m & 4) { at[a].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; if (ds) at[a].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; }
    }
    VkRenderPassCreateInfo ci = *e->ci;
    ci.pAttachments = at;
    VkRenderPass rp = VK_NULL_HANDLE;
    VkResult r = r_create_rp(s_dev, &ci, NULL, &rp);
    char d[200]; rp_desc_ops(e, mask, d, sizeof d);
    if (r != VK_SUCCESS) { f2_log("F2: [RED] rp_opt: render pass #%u variant %s FAILED %d (that pass runs as the engine asked)", e->id, d, r); return VK_NULL_HANDLE; }
    e->vmask[e->nvar] = mask; e->var[e->nvar] = rp; e->nvar++;
    f2_log("F2: rp_opt: render pass #%u variant %s", e->id, d);
    return rp;
}

/* ---- rp_opt: the frame record and the plan ---- */
#define FP_MAX 160
#define FE_MAX 6144
typedef struct {
    unsigned rp_id; uint8_t natt, full, cplx; unsigned draws, nclr; uint64_t cb; double px;
    uint64_t key[8]; uint8_t load[8], store[8], isz[8], bytes[8];
    uint8_t dec[8];         /* how the pass treats the old contents: 0 not at all, 1 uses them, 2 the first draw covers it, 3 a full clear */
    VkClearValue cv[8];     /* the clear values the engine gave */
    uint32_t mask, vmask;   /* overrides applied this frame (vmask: the checked ones among them) */
} fpass_t;
typedef struct { uint8_t kind, att; uint16_t pass; uint64_t img; } fev_t;
enum { EV_PASS, EV_READ, EV_KEEP };
typedef struct { unsigned rp_id; uint8_t natt; uint64_t key[8]; uint32_t mask, vbits; uint8_t fsrc[8], fatt[8]; } plan_t;
static fpass_t s_fp[FP_MAX];
static fev_t s_fe[FE_MAX];
static unsigned s_fpn, s_fen, s_sub_cur;
static int s_fr_bad, s_fr_dev;
static const char *s_fr_why;
static plan_t s_pl[FP_MAX], s_new[FP_MAX];
static unsigned s_pln, s_stable;
static uint64_t s_sig;
static struct { unsigned long frames, ld, st, fold, v_ok, v_no, missed; double saved; } s_os;
/* a plan is used after 60 frames with the same structure (a reader that comes every N frames shows up first), and an image whose
 * unchecked override ever turned out wrong (dropped from the plan while the structure stayed, or a missed prediction) is never
 * touched again in this run: at most one bad frame per image per run, not one per scene change */
#define PLAN_AFTER 60
#define NEVER_CAP 256
static uint64_t s_never[NEVER_CAP];
static unsigned s_nnever;
static int never_has(uint64_t k) { for (unsigned i = 0; i < s_nnever; i++) if (s_never[i] == k) return 1; return 0; }
static void never_add(uint64_t k)
{
    if (!k || never_has(k)) return;
    if (s_nnever < NEVER_CAP) s_never[s_nnever++] = k;
    f2_log("F2: rp_opt: an override on an image turned out wrong this frame: that image is left alone from now on (%u such images)", s_nnever);
}
static void tab_full(void)
{
    if (!s_tab_full) f2_log("F2: rp_opt: a tracking table is full: rp_opt OFF for this run");
    s_tab_full = 1; s_opt = 0; s_stable = 0; s_pln = 0;
}
static void fr_bad(const char *why) { if (!s_fr_bad) { s_fr_bad = 1; s_fr_why = why; } }
static void ev(int kind, unsigned att, unsigned pass, uint64_t img)
{
    if (!img) return;
    if (s_fen >= FE_MAX) { fr_bad("too many events"); return; }
    fev_t *e = &s_fe[s_fen++];
    e->kind = (uint8_t)kind; e->att = (uint8_t)att; e->pass = (uint16_t)pass; e->img = img;
}
static void ev_img(int kind, uint64_t img_handle)
{
    img_ent_t *e = img_get(img_handle);
    if (e && e->att) ev(kind, 0, s_fpn, e->sc ? 1 : img_handle);
}

/* ---- rp_log v2: an ordered trace of one whole frame every 20 s, at most 8 per run (rp_log=1) ----
 * Lines "F2: rptN cbK ...": PASS (render pass id #, framebuffer, its images with load / store as the engine asked, draws,
 * vkCmdClearAttachments, the images its draws sample), BAR (image layout barriers), COPY / BLIT / RESOLVE / B2I / I2B / CLRC / CLRZ,
 * DISPATCH, EXEC, SUBMIT; then the legend of the images seen.  Images are named iN by creation order, "sc" = a swapchain image. */
#define TB_CAP (1024 * 1024)
static char *s_tb;
static size_t s_tbw;
static int s_tb_full, s_tr_on;
static unsigned s_tr_n;
static unsigned long s_ds_copies, s_ds_tmpl;
static double s_tr_last;
static uint8_t s_tr_ref[1u << 13];      /* image ids seen in the trace (mod 65536) */
static int tracing(void) { return __atomic_load_n(&s_tr_on, __ATOMIC_ACQUIRE); }
static void tb(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void tb(const char *fmt, ...)
{
    if (!s_tb || s_tb_full) return;
    if (s_tbw + 1100 > TB_CAP) { s_tb_full = 1; return; }
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(s_tb + s_tbw, 1000, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > 999) n = 999;
    s_tbw += (size_t)n; s_tb[s_tbw++] = '\n';
}
static const char *iname(uint64_t h, char *o)
{
    img_ent_t *e = h > 1 ? img_get(h) : NULL;
    if (h == 1 || (e && e->sc)) { strcpy(o, "sc"); return o; }
    if (!e) { strcpy(o, "i?"); return o; }
    s_tr_ref[(e->id & 0xffff) >> 3] |= (uint8_t)(1u << (e->id & 7));
    sprintf(o, "i%u", e->id);
    return o;
}
static void add_rd(cb_ent_t *c, uint64_t img) { if (!img) return; for (unsigned i = 0; i < c->nrd; i++) if (c->rd[i] == img) return; if (c->nrd < 32) c->rd[c->nrd++] = img; }
static size_t list_imgs(char *o, size_t n, const uint64_t *imgs, unsigned cnt)
{
    size_t w = 0; char nm[24]; o[0] = 0;
    for (unsigned i = 0; i < cnt && w + 14 < n; i++) w += (size_t)snprintf(o + w, n - w, "%s%s", i ? " " : "", iname(imgs[i], nm));
    return w;
}
static void rt_dump(void)
{
    unsigned t = s_tr_n;
    f2_log("F2: rpt%u ---- frame trace %u (%zu bytes%s; descriptor copies so far %lu, template updates %lu, not followed) ----", t, t, s_tbw,
           s_tb_full ? ", TRUNCATED" : "", s_ds_copies, s_ds_tmpl);
    size_t a = 0;
    while (a < s_tbw) {
        size_t b = a; while (b < s_tbw && s_tb[b] != '\n') b++;
        s_tb[b] = 0;
        f2_log("F2: rpt%u %s", t, s_tb + a);
        a = b + 1;
    }
    char line[900]; size_t w = 0; line[0] = 0;
    for (size_t j = 0; j < IMG_CAP; j++) {
        img_ent_t *e = &s_img[j];
        if (!e->h || e->sc || !(s_tr_ref[(e->id & 0xffff) >> 3] & (1u << (e->id & 7)))) continue;
        w += (size_t)snprintf(line + w, sizeof line - w, " i%u=%ux%u/%uB%s", e->id, e->w, e->hgt, e->fmt_b, e->depth ? "z" : "");
        if (w > 760) { f2_log("F2: rpt%u images:%s", t, line); w = 0; line[0] = 0; }
    }
    if (w) f2_log("F2: rpt%u images:%s", t, line);
    f2_log("F2: rpt%u ---- end ----", t);
    s_tbw = 0; s_tb_full = 0; memset(s_tr_ref, 0, sizeof s_tr_ref);
}

static void rt_alloc(void)
{
    if (s_img) return;
    s_fb = (fb_ent_t *)calloc(FB_CAP, sizeof *s_fb); s_img = (img_ent_t *)calloc(IMG_CAP, sizeof *s_img);
    s_view = (view_ent_t *)calloc(VIEW_CAP, sizeof *s_view); s_ds = (ds_ent_t *)calloc(DS_CAP, sizeof *s_ds);
    s_pipe = (pipe_ent_t *)calloc(PIPE_CAP, sizeof *s_pipe); s_cb = (cb_ent_t *)calloc(CB_CAP, sizeof *s_cb);
    if (f2_cfg.rp_log) s_tb = (char *)malloc(TB_CAP);
    if (!s_fb || !s_img || !s_view || !s_ds || !s_pipe || !s_cb || (f2_cfg.rp_log && !s_tb)) {
        f2_log("F2: [RED] rp_log / rp_opt: no memory for the tables: both off");
        free(s_fb); free(s_img); free(s_view); free(s_ds); free(s_pipe); free(s_cb); free(s_tb);
        s_fb = NULL; s_img = NULL; s_view = NULL; s_ds = NULL; s_pipe = NULL; s_cb = NULL; s_tb = NULL;
        s_track = 0; s_opt = 0;
    }
}
/* from w_create_device */
static void rp_device(VkDevice d, uint32_t vendor)
{
    s_dev = d;
    int opt = f2_cfg.rp_opt < 0 ? (vendor == 0x13B5 || vendor == 0x1010 ? 7 : 0) : (f2_cfg.rp_opt & 7);
    s_opt = opt;
    s_track = f2_cfg.rp_log || opt;
    if (s_track) rt_alloc();
    f2_log("F2: rp_opt: %s (rp_opt=%d in f2_config, GPU vendor 0x%x; bits 1 dead loads / stores, 2 clear fold, 4 checked colour NOLOAD)",
           s_opt ? (s_opt == 7 ? "ON" : "ON (some parts)") : "off", f2_cfg.rp_opt, vendor);
}

static void rp_note_create(const VkRenderPassCreateInfo *ci, VkRenderPass p)
{
    if (!ci || !p) return;
    pthread_mutex_lock(&s_rp_mu);
    rp_ent_t *e = rp_find((uint64_t)p, 1);
    if (e) {
        uint64_t h = e->h;
        free(e->ci);
        memset(e, 0, sizeof *e); e->h = h;
        e->id = ++s_rp_ids;
        e->big = ci->attachmentCount > 8;
        e->natt = ci->attachmentCount < 8 ? ci->attachmentCount : 8;
        e->nsub = ci->subpassCount;
        for (unsigned i = 0; i < 8; i++) e->col[i] = -1;
        for (unsigned i = 0; i < e->natt; i++) {
            const VkAttachmentDescription *a = &ci->pAttachments[i];
            e->depth[i] = (uint8_t)is_depth_fmt(a->format); e->stencil[i] = (uint8_t)has_stencil_fmt(a->format);
            e->fmt_b[i] = (uint8_t)fmt_bytes(a->format); e->samples[i] = (uint8_t)a->samples;
            e->load[i] = (uint8_t)a->loadOp; e->store[i] = (uint8_t)a->storeOp;
            if (a->format == VK_FORMAT_S8_UINT) { e->load[i] = (uint8_t)a->stencilLoadOp; e->store[i] = (uint8_t)a->stencilStoreOp; }
        }
        for (unsigned s = 0; s < ci->subpassCount; s++) if (ci->pSubpasses[s].pResolveAttachments)
            for (unsigned c = 0; c < ci->pSubpasses[s].colorAttachmentCount; c++)
                if (ci->pSubpasses[s].pResolveAttachments[c].attachment != VK_ATTACHMENT_UNUSED) e->nres++;
        if (ci->subpassCount)
            for (unsigned c = 0; c < ci->pSubpasses[0].colorAttachmentCount && c < 8; c++) {
                uint32_t a = ci->pSubpasses[0].pColorAttachments[c].attachment;
                if (a < 8) e->col[a] = (int8_t)c;
            }
        if (s_opt && !e->big && e->nsub == 1 && !e->nres) e->ci = rp_copy(ci);
        if (f2_cfg.rp_log && e->id <= 120) {
            char d[200]; rp_desc(e, d, sizeof d);
            f2_log("F2: rp_log: render pass #%u: %s (%u subpasses; c/z colour/depth, bytes per pixel, L/C/- load/clear/dont-care, S/- store/dont-care)",
                   e->id, d, ci->subpassCount);
        }
    }
    pthread_mutex_unlock(&s_rp_mu);
}
static VKAPI_ATTR VkResult VKAPI_CALL w_create_rp(VkDevice d, const VkRenderPassCreateInfo *ci, const VkAllocationCallbacks *al, VkRenderPass *p)
{
    VkResult r = r_create_rp(d, ci, al, p);
    static unsigned bad;
    if (r != VK_SUCCESS && bad++ < 20) f2_log("F2: [RED] vkCreateRenderPass FAILED %d (attachments %u)", r, ci ? ci->attachmentCount : 0);
    if (r == VK_SUCCESS && p && s_track) rp_note_create(ci, *p);
    return r;
}

/* ---- object creation ---- */
static VKAPI_ATTR VkResult VKAPI_CALL w_create_image(VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *al, VkImage *img)
{
    VkResult r = r_create_image(d, ci, al, img);
    if (s_track && r == VK_SUCCESS && img && ci) {
        pthread_mutex_lock(&s_rp_mu);
        img_ent_t *e = (img_ent_t *)FIND(s_img, IMG_CAP, *img, 1);
        if (e) {
            e->id = ++s_img_ids;
            e->w = ci->extent.width; e->hgt = ci->extent.height; e->fmt_b = (uint8_t)fmt_bytes(ci->format);
            e->depth = (uint8_t)(is_depth_fmt(ci->format) || (ci->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT));
        }
        pthread_mutex_unlock(&s_rp_mu);
    }
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL w_create_view(VkDevice d, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *al, VkImageView *v)
{
    VkResult r = r_create_view(d, ci, al, v);
    if (s_track && r == VK_SUCCESS && v && ci) {
        pthread_mutex_lock(&s_rp_mu);
        view_ent_t *e = (view_ent_t *)FIND(s_view, VIEW_CAP, *v, 1);
        if (e) e->img = (uint64_t)ci->image;
        pthread_mutex_unlock(&s_rp_mu);
    }
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL w_sc_images(VkDevice d, VkSwapchainKHR sc, uint32_t *count, VkImage *imgs)
{
    VkResult r = r_sc_images(d, sc, count, imgs);
    if (s_track && (r == VK_SUCCESS || r == VK_INCOMPLETE) && imgs && count) {
        pthread_mutex_lock(&s_rp_mu);
        for (uint32_t i = 0; i < *count; i++) {
            img_ent_t *e = (img_ent_t *)FIND(s_img, IMG_CAP, imgs[i], 1);
            if (e && !e->sc) { e->id = ++s_img_ids; e->sc = 1; e->fmt_b = 4; }
        }
        pthread_mutex_unlock(&s_rp_mu);
    }
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL w_create_fb(VkDevice d, const VkFramebufferCreateInfo *ci, const VkAllocationCallbacks *al, VkFramebuffer *fb)
{
    VkResult r = r_create_fb(d, ci, al, fb);
    if (s_track && r == VK_SUCCESS && fb && ci) {
        pthread_mutex_lock(&s_rp_mu);
        fb_ent_t *e = (fb_ent_t *)FIND(s_fb, FB_CAP, *fb, 1);
        if (e) {
            e->w = ci->width; e->hgt = ci->height; e->id = ++s_fb_ids;
            e->natt = ci->attachmentCount < 8 ? ci->attachmentCount : 8;
            for (unsigned i = 0; i < e->natt; i++) {
                uint64_t img = ci->pAttachments ? view_img(ci->pAttachments[i]) : 0;
                img_ent_t *ie = img ? img_get(img) : NULL;
                if (ie) ie->att = 1;
                e->key[i] = ie ? (ie->sc ? 1 : img) : 0;
            }
        }
        pthread_mutex_unlock(&s_rp_mu);
    }
    return r;
}
/* rp_opt / rp_log: what a pipeline does to the attachments (from w_create_gp) */
static void rp_note_pipelines(uint32_t count, const VkGraphicsPipelineCreateInfo *ci, const VkPipeline *p)
{
    if (!s_track || !ci || !p) return;
    pthread_mutex_lock(&s_rp_mu);
    for (uint32_t i = 0; i < count; i++) {
        if (!p[i]) continue;
        pipe_ent_t *e = (pipe_ent_t *)FIND(s_pipe, PIPE_CAP, p[i], 1);
        if (!e) continue;
        const VkGraphicsPipelineCreateInfo *g = &ci[i];
        const VkPipelineDepthStencilStateCreateInfo *z = g->pDepthStencilState;
        if (z) {
            e->ztest = (uint8_t)((z->depthTestEnable && z->depthCompareOp != VK_COMPARE_OP_ALWAYS) || z->depthBoundsTestEnable);
            e->stest = (uint8_t)(z->stencilTestEnable != 0);
        }
        const VkPipelineColorBlendStateCreateInfo *b = g->pColorBlendState;
        if (b && b->pAttachments)
            for (uint32_t k = 0; k < b->attachmentCount && k < 8; k++) {
                if (b->pAttachments[k].blendEnable) e->blend |= (uint8_t)(1u << k);
                if ((b->pAttachments[k].colorWriteMask & 0xf) == 0xf) e->wfull |= (uint8_t)(1u << k);
            }
        if (b && b->logicOpEnable) e->blend = 0xff;
        const VkPipelineDynamicStateCreateInfo *dy = g->pDynamicState;
        if (dy) for (uint32_t k = 0; k < dy->dynamicStateCount; k++) {
            if (dy->pDynamicStates[k] == VK_DYNAMIC_STATE_VIEWPORT) e->dyn_vp = 1;
            if (dy->pDynamicStates[k] == VK_DYNAMIC_STATE_SCISSOR) e->dyn_sc = 1;
        }
        const VkPipelineViewportStateCreateInfo *v = g->pViewportState;
        if (v && v->pViewports && v->viewportCount) { e->vp = v->pViewports[0]; e->has_vp = 1; }
        if (v && v->pScissors && v->scissorCount) { e->sc = v->pScissors[0]; e->has_sc = 1; }
    }
    pthread_mutex_unlock(&s_rp_mu);
}
static VKAPI_ATTR VkResult VKAPI_CALL w_alloc_ds(VkDevice d, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *sets)
{
    VkResult r = r_alloc_ds(d, ai, sets);
    if (s_track && r == VK_SUCCESS && ai && sets) {
        pthread_mutex_lock(&s_rp_mu);
        for (uint32_t i = 0; i < ai->descriptorSetCount; i++) {
            ds_ent_t *e = (ds_ent_t *)FIND(s_ds, DS_CAP, sets[i], 1);
            if (e) { e->n = 0; e->ovf = 0; }
        }
        pthread_mutex_unlock(&s_rp_mu);
    }
    return r;
}
static VKAPI_ATTR void VKAPI_CALL w_upd_ds(VkDevice d, uint32_t nw, const VkWriteDescriptorSet *wr, uint32_t nc, const VkCopyDescriptorSet *cp)
{
    if (s_track && ((wr && nw) || nc)) {
        pthread_mutex_lock(&s_rp_mu);
        for (uint32_t i = 0; wr && i < nw; i++) {
            const VkWriteDescriptorSet *x = &wr[i];
            if (!x->pImageInfo) continue;
            if (x->descriptorType != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && x->descriptorType != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
                x->descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && x->descriptorType != VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) continue;
            ds_ent_t *e = (ds_ent_t *)FIND(s_ds, DS_CAP, x->dstSet, 1);
            if (!e) continue;
            for (uint32_t k = 0; k < x->descriptorCount; k++) {
                uint32_t key = (x->dstBinding << 12) | ((x->dstArrayElement + k) & 0xfff);
                uint64_t img = view_img(x->pImageInfo[k].imageView);
                unsigned j = 0;
                while (j < e->n && e->key[j] != key) j++;
                if (j == e->n) { if (e->n >= DS_N) { e->ovf = 1; continue; } e->n++; }
                e->key[j] = key; e->img[j] = img;
            }
        }
        if (nc && cp) {                       /* a copy: the destination may get any image; never seen in this engine (rp_log 18.11) */
            s_ds_copies += nc;
            for (uint32_t i = 0; i < nc; i++) { ds_ent_t *e = (ds_ent_t *)FIND(s_ds, DS_CAP, cp[i].dstSet, 1); if (e) e->ovf = 1; }
        }
        pthread_mutex_unlock(&s_rp_mu);
    }
    r_upd_ds(d, nw, wr, nc, cp);
}
static void tmpl_seen(void)
{
    static int said;
    pthread_mutex_lock(&s_rp_mu);
    s_ds_tmpl++;
    if (s_opt) { s_opt = 0; s_stable = 0; s_pln = 0; }
    pthread_mutex_unlock(&s_rp_mu);
    if (!said) { said = 1; f2_log("F2: rp_opt: the engine updates descriptor sets through templates, whose images are not followed: rp_opt OFF for this run"); }
}
static VKAPI_ATTR void VKAPI_CALL w_upd_tmpl(VkDevice d, VkDescriptorSet s, VkDescriptorUpdateTemplate t, const void *data)
{ if (s_track) tmpl_seen(); r_upd_tmpl(d, s, t, data); }
static VKAPI_ATTR void VKAPI_CALL w_upd_tmpl_khr(VkDevice d, VkDescriptorSet s, VkDescriptorUpdateTemplate t, const void *data)
{ if (s_track) tmpl_seen(); r_upd_tmpl_khr(d, s, t, data); }

/* ---- command buffer state ---- */
static VKAPI_ATTR VkResult VKAPI_CALL w_begin_cb(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi)
{
    if (s_track) {
        pthread_mutex_lock(&s_rp_mu);
        cb_ent_t *c = cb_get(cb);
        if (c) {
            c->in_pass = 0; c->fp = -1; c->pend = 0; c->pipe_h = 0; c->has_vp = c->has_sc = 0; c->rdirty = 1;
            memset(c->bound, 0, sizeof c->bound);
            if (tracing()) {
                const VkCommandBufferInheritanceInfo *h = bi ? bi->pInheritanceInfo : NULL;
                rp_ent_t *e = h && h->renderPass ? rp_find((uint64_t)h->renderPass, 0) : NULL;
                fb_ent_t *f = h && h->framebuffer ? (fb_ent_t *)FIND(s_fb, FB_CAP, h->framebuffer, 0) : NULL;
                if (h && h->renderPass) tb("cb%u BEGIN secondary inside pass #%u fb%u", c->id, e ? e->id : 0, f ? f->id : 0);
                else tb("cb%u BEGIN", c->id);
            }
        }
        pthread_mutex_unlock(&s_rp_mu);
    }
    return r_begin_cb(cb, bi);
}
static VKAPI_ATTR void VKAPI_CALL w_bind_ds(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout lay, uint32_t first, uint32_t n,
                                            const VkDescriptorSet *sets, uint32_t ndyn, const uint32_t *dyn)
{
    if (s_track && sets) {
        pthread_mutex_lock(&s_rp_mu);
        cb_ent_t *c = cb_get(cb);
        if (c) { for (uint32_t i = 0; i < n; i++) if (first + i < 8) c->bound[first + i] = (uint64_t)sets[i]; c->rdirty = 1; }
        pthread_mutex_unlock(&s_rp_mu);
    }
    r_bind_ds(cb, bp, lay, first, n, sets, ndyn, dyn);
}
static VKAPI_ATTR void VKAPI_CALL w_bind_pipe(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p)
{
    if (s_track && bp == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        pthread_mutex_lock(&s_rp_mu);
        cb_ent_t *c = cb_get(cb);
        if (c) c->pipe_h = (uint64_t)p;
        pthread_mutex_unlock(&s_rp_mu);
    }
    r_bind_pipe(cb, bp, p);
}
static VKAPI_ATTR void VKAPI_CALL w_set_vp(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport *v)
{
    if (s_track && first == 0 && n && v) {
        pthread_mutex_lock(&s_rp_mu);
        cb_ent_t *c = cb_get(cb);
        if (c) { c->vp = v[0]; c->has_vp = 1; }
        pthread_mutex_unlock(&s_rp_mu);
    }
    r_set_vp(cb, first, n, v);
}
static VKAPI_ATTR void VKAPI_CALL w_set_sc(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D *s)
{
    if (s_track && first == 0 && n && s) {
        pthread_mutex_lock(&s_rp_mu);
        cb_ent_t *c = cb_get(cb);
        if (c) { c->sc = s[0]; c->has_sc = 1; }
        pthread_mutex_unlock(&s_rp_mu);
    }
    r_set_sc(cb, first, n, s);
}
/* the images the bound descriptor sets let the draws / dispatches sample: rp_opt keeps the attachment images (c->ord, or straight into the
 * frame record outside a pass), the trace all of them */
static void collect_reads(cb_ent_t *c, int in_pass)
{
    int tr = tracing();
    for (int s = 0; s < 8; s++) {
        if (!c->bound[s]) continue;
        ds_ent_t *d = (ds_ent_t *)FIND(s_ds, DS_CAP, c->bound[s], 0);
        if (!d) { if (s_opt) fr_bad("a bound descriptor set is unknown"); continue; }
        if (d->ovf && s_opt) fr_bad("a bound descriptor set has too many images");
        for (unsigned k = 0; k < d->n; k++) {
            uint64_t img = d->img[k];
            if (!img) continue;
            if (tr) add_rd(c, img);
            if (!s_opt) continue;
            img_ent_t *ie = img_get(img);
            if (!ie || !ie->att) continue;
            uint64_t key = ie->sc ? 1 : img;
            if (!in_pass) { ev(EV_READ, 0, s_fpn, key); continue; }
            unsigned i = 0;
            while (i < c->nord && c->ord[i] != key) i++;
            if (i == c->nord) { if (c->nord < 16) c->ord[c->nord++] = key; else fr_bad("a pass samples too many attachment images"); }
        }
    }
}

/* ---- the pass itself ---- */
typedef struct { int indirect; uint32_t count, inst; } draw_t;
static int area_covered(const cb_ent_t *c, float x0, float y0, float x1, float y1)
{
    return x0 <= (float)c->rx && y0 <= (float)c->ry && x1 >= (float)c->rx + (float)c->rw && y1 >= (float)c->ry + (float)c->rh;
}
static const pipe_ent_t *cur_pipe(const cb_ent_t *c) { return (const pipe_ent_t *)FIND(s_pipe, PIPE_CAP, c->pipe_h, 0); }
static int draw_covers(const cb_ent_t *c, int slot, const draw_t *d)
{
    const pipe_ent_t *P = cur_pipe(c);
    if (!P || !d || slot < 0 || slot > 7 || d->indirect || d->inst != 1 || d->count < 3 || d->count > 6) return 0;
    if (P->ztest || P->stest || ((P->blend >> slot) & 1) || !((P->wfull >> slot) & 1)) return 0;
    const VkViewport *v = P->dyn_vp ? (c->has_vp ? &c->vp : NULL) : (P->has_vp ? &P->vp : NULL);
    const VkRect2D *s = P->dyn_sc ? (c->has_sc ? &c->sc : NULL) : (P->has_sc ? &P->sc : NULL);
    if (!v || !s) return 0;
    float vy0 = v->height < 0 ? v->y + v->height : v->y, vy1 = v->height < 0 ? v->y : v->y + v->height;
    if (!area_covered(c, v->x, vy0, v->x + v->width, vy1)) return 0;
    return area_covered(c, (float)s->offset.x, (float)s->offset.y, (float)s->offset.x + (float)s->extent.width, (float)s->offset.y + (float)s->extent.height);
}
static int clear_covers(const cb_ent_t *c, const rp_ent_t *e, unsigned a, uint32_t na, const VkClearAttachment *ca, uint32_t nr, const VkClearRect *cr)
{
    int full = 0;
    for (uint32_t i = 0; cr && i < nr; i++)
        if (cr[i].baseArrayLayer == 0 && cr[i].layerCount >= 1 &&
            area_covered(c, (float)cr[i].rect.offset.x, (float)cr[i].rect.offset.y, (float)cr[i].rect.offset.x + (float)cr[i].rect.extent.width,
                         (float)cr[i].rect.offset.y + (float)cr[i].rect.extent.height)) full = 1;
    if (!full || !e || a >= e->natt) return 0;
    for (uint32_t i = 0; ca && i < na; i++) {
        if (e->depth[a] || e->stencil[a]) {
            VkImageAspectFlags need = (e->depth[a] ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (e->stencil[a] ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
            if ((ca[i].aspectMask & need) == need) return 1;
        } else if ((ca[i].aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) && e->col[a] >= 0 && ca[i].colorAttachment == (uint32_t)e->col[a]) return 1;
    }
    return 0;
}
/* the real vkCmdBeginRenderPass with the overrides `mask` (rp_log counts what really moves) -- under s_rp_mu */
static void begin_real(VkCommandBuffer cb, cb_ent_t *c, const VkRenderPassBeginInfo *bi, VkSubpassContents cont, rp_ent_t *e, uint32_t mask, uint32_t vmask)
{
    VkRenderPassBeginInfo b = *bi;
    if (mask && e) {
        VkRenderPass v = rp_variant(e, mask);
        if (v) b.renderPass = v; else mask = 0;
    }
    if (!mask) b = *bi;
    fb_ent_t *f = (fb_ent_t *)FIND(s_fb, FB_CAP, bi->framebuffer, 0);
    double px = (double)bi->renderArea.extent.width * bi->renderArea.extent.height;
    s_rw.begins++;
    if (f && (bi->renderArea.extent.width < f->w || bi->renderArea.extent.height < f->hgt)) s_rw.partial++;
    if (e) {
        double moved = 0;
        for (unsigned i = 0; i < e->natt; i++) {
            double by = px * e->fmt_b[i] * e->samples[i];
            unsigned l = e->load[i] < 3 ? e->load[i] : 0, s = e->store[i] < 2 ? e->store[i] : 0, m = (mask >> (3 * i)) & 7;
            unsigned l0 = l, s0 = s;
            if (m & 1) l = 2;
            if (m & 2) l = 1;
            if (m & 4) s = 1;
            if (e->samples[i] > 1) s_rw.msaa++;
            if (e->depth[i]) { s_rw.dld[l]++; s_rw.dst[s]++; if (!l) s_rw.b_dl += by; if (!s) s_rw.b_ds += by; }
            else { s_rw.ld[l]++; s_rw.st[s]++; if (!l) s_rw.b_cl += by; if (!s) s_rw.b_cs += by; }
            moved += (!l ? by : 0) + (!s ? by : 0);
            if (!l0 && l) { s_os.ld++; s_os.saved += by; }
            if (!s0 && s) { s_os.st++; s_os.saved += by; }
            if (m & 2) s_os.fold++;
        }
        s_rw.resolve += e->nres;
        e->w_n++; e->w_bytes += moved;
    }
    if (c && c->fp >= 0) { s_fp[c->fp].mask = mask; s_fp[c->fp].vmask = vmask & mask; }
    r_begin_rp(cb, &b, cont);
}
/* a held-back vkCmdBeginRenderPass goes to the driver now; trig 1 = a draw (d), 2 = vkCmdClearAttachments, 0 = anything else */
static void flush_pending(VkCommandBuffer cb, cb_ent_t *c, int trig, const draw_t *d, uint32_t na, const VkClearAttachment *ca, uint32_t nr, const VkClearRect *cr)
{
    if (!c || !c->pend) return;
    c->pend = 0;
    uint32_t mask = c->pmask;
    const rp_ent_t *e = c->prpe;
    for (unsigned a = 0; e && a < e->natt; a++) {
        uint32_t bit = 1u << (3 * a);
        if (!(c->pvbits & bit) || !(mask & bit)) continue;
        int ok = 0;
        if (trig == 1 && !e->depth[a] && !e->stencil[a]) ok = draw_covers(c, e->col[a], d);
        if (trig == 2) ok = clear_covers(c, e, a, na, ca, nr, cr);
        if (ok) s_os.v_ok++;
        else {
            mask &= ~bit; s_os.v_no++;
            if (c->pord >= 0 && (unsigned)c->pord < s_pln) { s_pl[c->pord].mask &= ~bit; s_pl[c->pord].vbits &= ~bit; }   /* for good */
        }
    }
    begin_real(cb, c, &c->pbi, c->pcont, c->prpe, mask, c->pvbits);
}
static VKAPI_ATTR void VKAPI_CALL w_begin_rp(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, VkSubpassContents cont)
{
    if (!s_track || !bi) { r_begin_rp(cb, bi, cont); return; }
    pthread_mutex_lock(&s_rp_mu);
    rp_ent_t *e = rp_find((uint64_t)bi->renderPass, 0);
    fb_ent_t *f = (fb_ent_t *)FIND(s_fb, FB_CAP, bi->framebuffer, 0);
    cb_ent_t *c = cb_get(cb);
    uint32_t mask = 0, vbits = 0;
    VkClearValue cv[8];
    VkRenderPassBeginInfo b = *bi;
    if (c) {
        if (c->pend) flush_pending(cb, c, 0, NULL, 0, NULL, 0, NULL);   /* cannot happen: a pass inside a pass */
        c->in_pass = 1; c->rpe = e; c->rp = e ? e->id : 0; c->fb = f ? f->id : 0; c->natt = f ? f->natt : 0;
        if (f) memcpy(c->att, f->key, sizeof c->att);
        c->sub = 0; c->draws = 0; c->clr = 0; c->nrd = 0; c->nord = 0; c->rdirty = 1;
        c->rx = bi->renderArea.offset.x; c->ry = bi->renderArea.offset.y; c->rw = bi->renderArea.extent.width; c->rh = bi->renderArea.extent.height;
        c->fp = -1; c->cplx = cont != VK_SUBPASS_CONTENTS_INLINE;
        if (s_opt) {
            if (s_fpn >= FP_MAX) fr_bad("too many passes");
            else {
                int i = (int)s_fpn++;
                fpass_t *p = &s_fp[i];
                memset(p, 0, sizeof *p);
                c->fp = i;
                p->cb = (uint64_t)(uintptr_t)cb;
                p->rp_id = e ? e->id : 0;
                p->natt = (uint8_t)(f ? f->natt : 0);
                p->full = f && bi->renderArea.offset.x == 0 && bi->renderArea.offset.y == 0 && bi->renderArea.extent.width == f->w && bi->renderArea.extent.height == f->hgt;
                p->cplx = (uint8_t)(!e || !f || e->big || e->nsub != 1 || e->nres || !e->ci || f->natt != e->natt || cont != VK_SUBPASS_CONTENTS_INLINE);
                p->px = (double)bi->renderArea.extent.width * bi->renderArea.extent.height;
                for (unsigned a = 0; a < p->natt; a++) {
                    p->key[a] = f->key[a];
                    if (!p->key[a]) p->cplx = 1;
                    if (e && a < e->natt) {
                        p->load[a] = e->load[a]; p->store[a] = e->store[a]; p->isz[a] = (uint8_t)(e->depth[a] || e->stencil[a]); p->bytes[a] = e->fmt_b[a];
                    }
                    if (bi->pClearValues && a < bi->clearValueCount) p->cv[a] = bi->pClearValues[a];
                    ev(EV_PASS, a, (unsigned)i, p->key[a]);
                }
                /* the plan */
                if (s_stable >= PLAN_AFTER && !s_fr_dev) {
                    if ((unsigned)i >= s_pln) s_fr_dev = 1;
                    else {
                        const plan_t *pl = &s_pl[i];
                        if (pl->rp_id != p->rp_id || pl->natt != p->natt || memcmp(pl->key, p->key, sizeof p->key)) s_fr_dev = 1;
                        else if (!p->cplx && pl->mask) {
                            mask = pl->mask; vbits = pl->vbits;
                            unsigned n = bi->clearValueCount;
                            int cvinit = 0;
                            if (n > 8) mask = 0;
                            for (unsigned a = 0; a < 8 && mask; a++) {
                                if (!((mask >> (3 * a)) & 2)) continue;
                                unsigned s = pl->fsrc[a], sa = pl->fatt[a];
                                if (s >= (unsigned)i || sa >= s_fp[s].natt || s_fp[s].key[sa] != p->key[a]) { mask &= ~(7u << (3 * a)); continue; }
                                if (!cvinit) { memset(cv, 0, sizeof cv); if (bi->pClearValues && n) memcpy(cv, bi->pClearValues, n * sizeof cv[0]); cvinit = 1; }
                                if (n < a + 1) { memset(cv + n, 0, (a + 1 - n) * sizeof cv[0]); n = a + 1; }
                                cv[a] = s_fp[s].cv[sa];
                                b.pClearValues = cv; b.clearValueCount = n;
                            }
                        }
                    }
                }
            }
        }
    }
    if (c && (vbits & mask)) {           /* hold the begin back until the first command that needs the pass */
        c->pend = 1; c->pbi = b; c->pcont = cont; c->pmask = mask; c->pvbits = vbits; c->prpe = e; c->pord = c->fp;
        if (b.pClearValues && b.clearValueCount <= 8) { memcpy(c->pcv, b.pClearValues, b.clearValueCount * sizeof c->pcv[0]); c->pbi.pClearValues = c->pcv; }
        else if (b.clearValueCount > 8) { c->pend = 0; begin_real(cb, c, &b, cont, e, 0, 0); }
    } else begin_real(cb, c, &b, cont, e, mask, vbits);
    pthread_mutex_unlock(&s_rp_mu);
}
static void note_draw(VkCommandBuffer cb, const draw_t *d)
{
    if (!s_track) return;
    pthread_mutex_lock(&s_rp_mu);
    cb_ent_t *c = cb_get(cb);
    if (c) {
        if (c->in_pass && (c->rdirty || tracing())) { collect_reads(c, 1); c->rdirty = 0; }
        /* a pass that samples one of its own attachments (seen: #14 samples i1510 while drawing to it) needs that attachment's contents */
        uint8_t selfrd[8] = { 0 };
        if (c->in_pass)
            for (unsigned a = 0; a < c->natt && a < 8; a++)
                for (unsigned i = 0; i < c->nord; i++) if (c->att[a] && c->ord[i] == c->att[a]) selfrd[a] = 1;
        if (c->pend) {
            for (unsigned a = 0; a < 8; a++) if (selfrd[a]) c->pvbits &= ~(1u << (3 * a)), c->pmask &= ~(1u << (3 * a));
            flush_pending(cb, c, 1, d, 0, NULL, 0, NULL);
        }
        if (c->in_pass) {
            c->draws++;
            if (c->fp >= 0) {
                fpass_t *p = &s_fp[c->fp];
                const rp_ent_t *e = c->rpe;
                const pipe_ent_t *P = cur_pipe(c);
                p->draws++;
                for (unsigned a = 0; a < p->natt; a++) {
                    if (selfrd[a]) { p->dec[a] = 1; continue; }
                    if (p->dec[a]) continue;
                    if (p->isz[a]) { if (!P || P->ztest || P->stest) p->dec[a] = 1; }   /* a draw that only writes depth: undecided */
                    else p->dec[a] = (uint8_t)(draw_covers(c, e ? e->col[a] : -1, d) ? 2 : 1);
                }
            }
        } else if (tracing()) tb("cb%u DRAW outside a render pass", c->id);
    }
    pthread_mutex_unlock(&s_rp_mu);
}
static VKAPI_ATTR void VKAPI_CALL w_draw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi)
{ draw_t d = { 0, vc, ic }; note_draw(cb, &d); r_draw(cb, vc, ic, fv, fi); }
static VKAPI_ATTR void VKAPI_CALL w_draw_ix(VkCommandBuffer cb, uint32_t xc, uint32_t ic, uint32_t fx, int32_t vo, uint32_t fi)
{ draw_t d = { 0, xc, ic }; note_draw(cb, &d); r_draw_ix(cb, xc, ic, fx, vo, fi); }
static VKAPI_ATTR void VKAPI_CALL w_draw_ind(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s)
{ draw_t d = { 1, 0, 0 }; note_draw(cb, &d); r_draw_ind(cb, b, o, n, s); }
static VKAPI_ATTR void VKAPI_CALL w_draw_ix_ind(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s)
{ draw_t d = { 1, 0, 0 }; note_draw(cb, &d); r_draw_ix_ind(cb, b, o, n, s); }
static void note_dispatch(VkCommandBuffer cb, const char *what)
{
    if (!s_track) return;
    pthread_mutex_lock(&s_rp_mu);
    cb_ent_t *c = cb_get(cb);
    if (c) {
        unsigned keep = c->nrd; uint64_t save[32]; memcpy(save, c->rd, sizeof save);
        c->nrd = 0;
        collect_reads(c, 0);
        if (tracing()) { char o[400]; list_imgs(o, sizeof o, c->rd, c->nrd); tb("cb%u %s images[%s]", c->id, what, o); }
        c->nrd = keep; memcpy(c->rd, save, sizeof save);
    }
    pthread_mutex_unlock(&s_rp_mu);
}
static VKAPI_ATTR void VKAPI_CALL w_dispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) { note_dispatch(cb, "DISPATCH"); r_dispatch(cb, x, y, z); }
static VKAPI_ATTR void VKAPI_CALL w_dispatch_ind(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o) { note_dispatch(cb, "DISPATCH-INDIRECT"); r_dispatch_ind(cb, b, o); }
static VKAPI_ATTR void VKAPI_CALL w_next_sp(VkCommandBuffer cb, VkSubpassContents sc)
{
    if (s_track) {
        pthread_mutex_lock(&s_rp_mu);
        cb_ent_t *c = cb_get(cb);
        if (c) { flush_pending(cb, c, 0, NULL, 0, NULL, 0, NULL); c->sub++; if (c->fp >= 0) s_fp[c->fp].cplx = 1; }
        pthread_mutex_unlock(&s_rp_mu);
    }
    r_next_sp(cb, sc);
}
static VKAPI_ATTR void VKAPI_CALL w_clr_att(VkCommandBuffer cb, uint32_t na, const VkClearAttachment *a, uint32_t nr, const VkClearRect *r)
{
    if (s_track) {
        pthread_mutex_lock(&s_rp_mu);
        cb_ent_t *c = cb_get(cb);
        if (c) {
            if (c->pend) flush_pending(cb, c, 2, NULL, na, a, nr, r);
            if (c->in_pass) {
                c->clr += na;
                if (c->fp >= 0) {
                    fpass_t *p = &s_fp[c->fp];
                    p->nclr += na;
                    for (unsigned k = 0; k < p->natt; k++) if (!p->dec[k] && clear_covers(c, c->rpe, k, na, a, nr, r)) p->dec[k] = 3;
                }
            } else if (tracing()) tb("cb%u CLEARATT outside a pass", c->id);
        }
        pthread_mutex_unlock(&s_rp_mu);
    }
    r_clr_att(cb, na, a, nr, r);
}
static VKAPI_ATTR void VKAPI_CALL w_end_rp(VkCommandBuffer cb)
{
    if (s_track) {
        pthread_mutex_lock(&s_rp_mu);
        cb_ent_t *c = cb_get(cb);
        if (c) {
            if (c->pend) flush_pending(cb, c, 0, NULL, 0, NULL, 0, NULL);
            if (c->in_pass && c->fp >= 0) {
                fpass_t *p = &s_fp[c->fp];
                for (unsigned i = 0; i < c->nord; i++) ev(EV_READ, 0, (unsigned)c->fp, c->ord[i]);
                /* a load the plan dropped without a check, but the pass tested depth / stencil after all: missed (this frame may show it) */
                for (unsigned a = 0; a < p->natt; a++) {
                    uint32_t bit = 1u << (3 * a);
                    if ((p->mask & bit) && !(p->vmask & bit) && p->dec[a] == 1) {
                        s_os.missed++;
                        if ((unsigned)c->fp < s_pln) s_pl[c->fp].mask &= ~bit;
                        never_add(p->key[a]);
                    }
                }
            }
            if (c->in_pass && tracing()) {
                char at[300], rd[400], nm2[24]; size_t w = 0; at[0] = 0;
                const rp_ent_t *e = c->rpe;
                for (unsigned i = 0; i < c->natt && w + 30 < sizeof at; i++) {
                    int ok = e && i < e->natt;
                    w += (size_t)snprintf(at + w, sizeof at - w, "%s%s %s%s%s", i ? ", " : "", iname(c->att[i], nm2), ok && e->depth[i] ? "z" : "c",
                                          ok && e->load[i] < 3 ? k_ld[e->load[i]] : "?", ok && e->store[i] < 2 ? k_st[e->store[i]] : "?");
                }
                list_imgs(rd, sizeof rd, c->rd, c->nrd);
                uint32_t m = c->fp >= 0 ? s_fp[c->fp].mask : 0;
                tb("cb%u PASS #%u fb%u %ux%u [%s] draws %u clears %u%s reads[%s]%s", c->id, c->rp, c->fb, c->rw, c->rh, at, c->draws, c->clr,
                   c->sub ? " (+subpasses)" : "", rd, m ? " (rp_opt overrides applied)" : "");
            }
            c->in_pass = 0; c->fp = -1;
        }
        pthread_mutex_unlock(&s_rp_mu);
    }
    r_end_rp(cb);
}
static VKAPI_ATTR VkResult VKAPI_CALL w_end_cb(VkCommandBuffer cb)
{
    if (s_track) {
        pthread_mutex_lock(&s_rp_mu);
        cb_ent_t *c = cb_get(cb);
        if (c && c->pend) flush_pending(cb, c, 0, NULL, 0, NULL, 0, NULL);
        pthread_mutex_unlock(&s_rp_mu);
    }
    return r_end_cb(cb);
}
static VKAPI_ATTR void VKAPI_CALL w_barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags df,
                                            uint32_t nm, const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b,
                                            uint32_t ni, const VkImageMemoryBarrier *im)
{
    if (s_track) {
        pthread_mutex_lock(&s_rp_mu);
        s_rw.bar++;
        if ((src | dst) & (VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT)) s_rw.bar_all++;
        if (dst & (VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT))
            s_rw.bar_top++;
        s_rw.bar_img += ni;
        cb_ent_t *c = cb_get(cb);
        if (c && c->pend) flush_pending(cb, c, 0, NULL, 0, NULL, 0, NULL);
        if (c && tracing() && im && ni) {
            char o[700], nm2[24]; size_t w = 0; o[0] = 0;
            for (uint32_t i = 0; i < ni && w + 40 < sizeof o; i++)
                w += (size_t)snprintf(o + w, sizeof o - w, " %s:%d>%d", iname((uint64_t)im[i].image, nm2), im[i].oldLayout, im[i].newLayout);
            tb("cb%u BAR%s %x>%x%s", c->id, c->in_pass ? " (in pass)" : "", src, dst, o);
        }
        pthread_mutex_unlock(&s_rp_mu);
    }
    r_barrier(cb, src, dst, df, nm, m, nb, b, ni, im);
}
/* transfers: a source is read, a destination keeps whatever the transfer does not cover (the record stays on the safe side) */
static void note_xfer(VkCommandBuffer cb, const char *what, VkImage src, VkImage dst)
{
    if (!s_track) return;
    pthread_mutex_lock(&s_rp_mu);
    if (s_opt) { if (src) ev_img(EV_READ, (uint64_t)src); if (dst) ev_img(EV_KEEP, (uint64_t)dst); }
    cb_ent_t *c = cb_get(cb);
    if (c && tracing()) {
        char a[24], b[24];
        tb("cb%u %s %s > %s", c->id, what, src ? iname((uint64_t)src, a) : "buffer", dst ? iname((uint64_t)dst, b) : "buffer");
    }
    pthread_mutex_unlock(&s_rp_mu);
}
static VKAPI_ATTR void VKAPI_CALL w_copy_img(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy *r)
{ note_xfer(cb, "COPY", s, d); r_copy_img(cb, s, sl, d, dl, n, r); }
static VKAPI_ATTR void VKAPI_CALL w_blit_img(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageBlit *r, VkFilter f)
{ note_xfer(cb, "BLIT", s, d); r_blit_img(cb, s, sl, d, dl, n, r, f); }
static VKAPI_ATTR void VKAPI_CALL w_resolve_img(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageResolve *r)
{ note_xfer(cb, "RESOLVE", s, d); r_resolve_img(cb, s, sl, d, dl, n, r); }
static VKAPI_ATTR void VKAPI_CALL w_copy_b2i(VkCommandBuffer cb, VkBuffer b, VkImage d, VkImageLayout dl, uint32_t n, const VkBufferImageCopy *r)
{ note_xfer(cb, "B2I", VK_NULL_HANDLE, d); r_copy_b2i(cb, b, d, dl, n, r); }
static VKAPI_ATTR void VKAPI_CALL w_copy_i2b(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkBuffer b, uint32_t n, const VkBufferImageCopy *r)
{ note_xfer(cb, "I2B", s, VK_NULL_HANDLE); r_copy_i2b(cb, s, sl, b, n, r); }
static VKAPI_ATTR void VKAPI_CALL w_clr_c(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearColorValue *v, uint32_t n, const VkImageSubresourceRange *r)
{ note_xfer(cb, "CLRC", VK_NULL_HANDLE, i); r_clr_c(cb, i, l, v, n, r); }
static VKAPI_ATTR void VKAPI_CALL w_clr_z(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearDepthStencilValue *v, uint32_t n, const VkImageSubresourceRange *r)
{ note_xfer(cb, "CLRZ", VK_NULL_HANDLE, i); r_clr_z(cb, i, l, v, n, r); }
static VKAPI_ATTR void VKAPI_CALL w_exec(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer *cbs)
{
    if (s_track) {
        pthread_mutex_lock(&s_rp_mu);
        cb_ent_t *c = cb_get(cb);
        if (c) {
            if (c->pend) flush_pending(cb, c, 0, NULL, 0, NULL, 0, NULL);
            if (c->fp >= 0) s_fp[c->fp].cplx = 1;
            if (tracing() && cbs) {
                char o[400]; size_t w = 0; o[0] = 0;
                for (uint32_t i = 0; i < n && w + 12 < sizeof o; i++) { cb_ent_t *k = cb_get(cbs[i]); w += (size_t)snprintf(o + w, sizeof o - w, " cb%u", k ? k->id : 0); }
                tb("cb%u EXEC%s", c->id, o);
            }
        }
        pthread_mutex_unlock(&s_rp_mu);
    }
    r_exec(cb, n, cbs);
}
static VKAPI_ATTR VkResult VKAPI_CALL w_submit(VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence f)
{
    if (s_track && si) {
        pthread_mutex_lock(&s_rp_mu);
        char o[600]; size_t w = 0; o[0] = 0;
        int tr = tracing();
        for (uint32_t i = 0; i < n; i++) {
            if (tr && w + 20 < sizeof o) w += (size_t)snprintf(o + w, sizeof o - w, "%s", i ? " |" : "");
            for (uint32_t k = 0; k < si[i].commandBufferCount; k++) {
                uint64_t h = (uint64_t)(uintptr_t)si[i].pCommandBuffers[k];
                while (s_sub_cur < s_fpn && s_fp[s_sub_cur].cb == h) s_sub_cur++;   /* the passes must run in the order they were recorded */
                if (tr && w + 12 < sizeof o) { cb_ent_t *c = cb_get(si[i].pCommandBuffers[k]); w += (size_t)snprintf(o + w, sizeof o - w, " cb%u", c ? c->id : 0); }
            }
        }
        if (tr) tb("SUBMIT%s%s", o, f ? " (+fence)" : "");
        pthread_mutex_unlock(&s_rp_mu);
    }
    return r_submit(q, n, si, f);
}

/* ---- objects going away: their entries go too (handles are reused) ---- */
static PFN_vkDestroyImage r_destroy_image;
static PFN_vkDestroyImageView r_destroy_view;
static PFN_vkDestroyFramebuffer r_destroy_fb;
static PFN_vkDestroyPipeline r_destroy_pipe;
static PFN_vkFreeDescriptorSets r_free_ds;
static PFN_vkCmdBeginQuery r_begin_query;
static PFN_vkCmdWriteTimestamp r_timestamp;
static VKAPI_ATTR void VKAPI_CALL w_destroy_image(VkDevice d, VkImage i, const VkAllocationCallbacks *al)
{ if (s_track && i) { pthread_mutex_lock(&s_rp_mu); DEL(s_img, IMG_CAP, i); pthread_mutex_unlock(&s_rp_mu); } r_destroy_image(d, i, al); }
static VKAPI_ATTR void VKAPI_CALL w_destroy_view(VkDevice d, VkImageView v, const VkAllocationCallbacks *al)
{ if (s_track && v) { pthread_mutex_lock(&s_rp_mu); DEL(s_view, VIEW_CAP, v); pthread_mutex_unlock(&s_rp_mu); } r_destroy_view(d, v, al); }
static VKAPI_ATTR void VKAPI_CALL w_destroy_fb(VkDevice d, VkFramebuffer f, const VkAllocationCallbacks *al)
{ if (s_track && f) { pthread_mutex_lock(&s_rp_mu); DEL(s_fb, FB_CAP, f); pthread_mutex_unlock(&s_rp_mu); } r_destroy_fb(d, f, al); }
static VKAPI_ATTR void VKAPI_CALL w_destroy_pipe(VkDevice d, VkPipeline p, const VkAllocationCallbacks *al)
{ if (s_track && p) { pthread_mutex_lock(&s_rp_mu); DEL(s_pipe, PIPE_CAP, p); pthread_mutex_unlock(&s_rp_mu); } r_destroy_pipe(d, p, al); }
static VKAPI_ATTR VkResult VKAPI_CALL w_free_ds(VkDevice d, VkDescriptorPool pool, uint32_t n, const VkDescriptorSet *sets)
{
    if (s_track && sets) { pthread_mutex_lock(&s_rp_mu); for (uint32_t i = 0; i < n; i++) DEL(s_ds, DS_CAP, sets[i]); pthread_mutex_unlock(&s_rp_mu); }
    return r_free_ds(d, pool, n, sets);
}
/* commands that must stay on their side of the render pass begin: a held-back begin goes first */
static void flush_any(VkCommandBuffer cb)
{
    if (!s_track) return;
    pthread_mutex_lock(&s_rp_mu);
    cb_ent_t *c = (cb_ent_t *)FIND(s_cb, CB_CAP, (uintptr_t)cb, 0);
    if (c && c->pend) flush_pending(cb, c, 0, NULL, 0, NULL, 0, NULL);
    pthread_mutex_unlock(&s_rp_mu);
}
static VKAPI_ATTR void VKAPI_CALL w_begin_query(VkCommandBuffer cb, VkQueryPool p, uint32_t q, VkQueryControlFlags f) { flush_any(cb); r_begin_query(cb, p, q, f); }
static VKAPI_ATTR void VKAPI_CALL w_timestamp(VkCommandBuffer cb, VkPipelineStageFlagBits st, VkQueryPool p, uint32_t q) { flush_any(cb); r_timestamp(cb, st, p, q); }

/* ---- rp_opt: at the present, learn from the frame just recorded ---- */
static uint64_t frame_sig(void)
{
    uint64_t h = 1469598103934665603ull;
#define MIX(v) do { h ^= (uint64_t)(v); h *= 1099511628211ull; } while (0)
    MIX(s_fpn);
    for (unsigned i = 0; i < s_fpn; i++) { MIX(s_fp[i].rp_id); MIX(s_fp[i].natt); for (unsigned a = 0; a < s_fp[i].natt; a++) MIX(s_fp[i].key[a]); }
#undef MIX
    return h;
}
static void plan_from_frame(void)
{
    uint64_t keys[64]; unsigned nk = 0;
    for (unsigned i = 0; i < s_fpn; i++) {
        plan_t *n = &s_new[i];
        memset(n, 0, sizeof *n);
        n->rp_id = s_fp[i].rp_id; n->natt = s_fp[i].natt; memcpy(n->key, s_fp[i].key, sizeof n->key);
    }
    for (unsigned j = 0; j < s_fen; j++) {
        if (s_fe[j].kind != EV_PASS) continue;
        unsigned k = 0; while (k < nk && keys[k] != s_fe[j].img) k++;
        if (k == nk && nk < 64) keys[nk++] = s_fe[j].img;
    }
    for (unsigned k = 0; k < nk; k++) {
        uint64_t K = keys[k];
        if (never_has(K)) continue;
        /* clear fold, forward: a clear-only pass, then the next touch of the image is a pass that loads it */
        if (s_opt & 2) {
            int src = -1, satt = 0;
            for (unsigned j = 0; j < s_fen; j++) {
                const fev_t *e = &s_fe[j];
                if (e->img != K) continue;
                if (e->kind != EV_PASS) { src = -1; continue; }
                const fpass_t *p = &s_fp[e->pass]; unsigned a = e->att;
                int folded = 0;
                if (src >= 0 && !p->cplx && p->full && p->load[a] == VK_ATTACHMENT_LOAD_OP_LOAD) {
                    s_new[e->pass].mask |= 2u << (3 * a); s_new[e->pass].fsrc[a] = (uint8_t)src; s_new[e->pass].fatt[a] = (uint8_t)satt; folded = 1;
                }
                int s0 = src, sa0 = satt;
                src = -1;
                if (!p->cplx && p->full && !p->draws && !p->nclr && (p->load[a] == VK_ATTACHMENT_LOAD_OP_CLEAR || folded)) {
                    src = folded ? s0 : (int)e->pass; satt = folded ? sa0 : (int)a;
                }
            }
        }
        /* liveness, backward over two copies of the frame (the second sweep decides) */
        int live = 0;
        for (int sweep = 0; sweep < 2; sweep++) {
            if (K == 1) live = 1;                     /* a swapchain image: the present reads it */
            for (int j = (int)s_fen - 1; j >= 0; j--) {
                const fev_t *e = &s_fe[j];
                if (e->img != K) continue;
                if (e->kind == EV_READ) { live = 1; continue; }
                if (e->kind == EV_KEEP) continue;
                const fpass_t *p = &s_fp[e->pass]; unsigned a = e->att;
                plan_t *n = &s_new[e->pass];
                int folded = (n->mask >> (3 * a)) & 2;
                int L = p->load[a] == VK_ATTACHMENT_LOAD_OP_LOAD && !folded;
                int use = p->cplx || p->dec[a] == 1;
                int ovw = !p->cplx && (p->dec[a] == 2 || p->dec[a] == 3);
                if (sweep == 1 && !p->cplx) {
                    if ((s_opt & 1) && !live && p->store[a] == VK_ATTACHMENT_STORE_OP_STORE) n->mask |= 4u << (3 * a);
                    if (L && !use) {
                        if (!live && (s_opt & 1)) n->mask |= 1u << (3 * a);
                        else if (ovw && (s_opt & 4)) { n->mask |= 1u << (3 * a); n->vbits |= 1u << (3 * a); }
                    }
                }
                live = L ? (use || live) : 0;        /* a checked NOLOAD never kills: the load may come back this frame */
            }
        }
    }
}
static void opt_frame_end(void)
{
    if (!s_opt) { s_fpn = s_fen = s_sub_cur = 0; s_fr_bad = s_fr_dev = 0; return; }
    if (!s_fr_bad && s_sub_cur != s_fpn) fr_bad("pass command buffers were submitted in another order (or not yet)");
    if (s_fr_bad || !s_fpn) {
        static unsigned long said;
        if (s_stable >= PLAN_AFTER || (s_fr_bad && said++ < 5)) f2_log("F2: rp_opt: frame not learned (%s)%s", s_fr_bad ? s_fr_why : "no passes",
                                                                       s_stable >= PLAN_AFTER ? ": overrides stopped" : "");
        s_stable = 0; s_pln = 0; s_sig = 0;
    } else {
        uint64_t sig = frame_sig();
        plan_from_frame();
        if (sig != s_sig || s_fpn != s_pln) {
            if (s_stable >= PLAN_AFTER) f2_log("F2: rp_opt: the frame changed (%u passes): overrides stopped, learning again", s_fpn);
            memcpy(s_pl, s_new, s_fpn * sizeof s_pl[0]); s_pln = s_fpn; s_sig = sig; s_stable = 0;
        } else {
            for (unsigned i = 0; i < s_pln; i++) {
                uint32_t keep = s_new[i].mask;
                for (unsigned a = 0; a < 8; a++)
                    if ((keep & s_pl[i].mask) >> (3 * a) & 2)
                        if (s_new[i].fsrc[a] != s_pl[i].fsrc[a] || s_new[i].fatt[a] != s_pl[i].fatt[a]) keep &= ~(2u << (3 * a));
                s_pl[i].vbits |= s_new[i].vbits;
                uint32_t lost = s_pl[i].mask & ~keep & ~s_pl[i].vbits;   /* unchecked overrides this frame says were wrong */
                for (unsigned a = 0; a < s_pl[i].natt; a++) if ((lost >> (3 * a)) & 7) never_add(s_pl[i].key[a]);
                s_pl[i].mask &= keep;
                s_pl[i].vbits &= s_pl[i].mask;
            }
            if (s_stable < 1000000) s_stable++;
            if (s_stable == PLAN_AFTER) {
                unsigned ld = 0, st = 0, fo = 0, vb = 0; double mib = 0;
                for (unsigned i = 0; i < s_pln; i++)
                    for (unsigned a = 0; a < s_fp[i].natt; a++) {
                        uint32_t m = (s_pl[i].mask >> (3 * a)) & 7; double by = s_fp[i].px * s_fp[i].bytes[a];
                        if ((m & 1) && ((s_pl[i].vbits >> (3 * a)) & 1)) vb++; else if (m & 1) { ld++; mib += by; }
                        if (m & 2) { fo++; mib += by; }
                        if (m & 4) { st++; mib += by; }
                    }
                f2_log("F2: rp_opt: frame plan on (%u passes, the same for %u frames): %u loads and %u stores dropped, %u loads made clears, "
                       "%u colour loads checked per frame; %.1f MiB per frame less (without the checked ones)", s_pln, s_stable, ld, st, fo, vb, mib / 1048576.0);
            }
        }
    }
    if (s_stable > PLAN_AFTER) s_os.frames++;
    s_fpn = s_fen = s_sub_cur = 0; s_fr_bad = s_fr_dev = 0; s_fr_why = NULL;
    for (size_t j = 0; j < CB_CAP; j++) if (s_cb[j].h && !s_cb[j].in_pass) s_cb[j].fp = -1;
}

/* called from w_present every 5 s with the frames of that window */
static void rp_report(unsigned frames, double secs)
{
    if (!frames || secs <= 0) return;
    pthread_mutex_lock(&s_rp_mu);
    const double F = frames, MB = 1048576.0, tot = s_rw.b_cl + s_rw.b_cs + s_rw.b_dl + s_rw.b_ds;
    if (f2_cfg.rp_log) {
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
        f2_log("F2: rp_log: frame traces so far %u%s", s_tr_n, s_tb ? "" : " (no trace buffer)");
    }
    if (s_opt) f2_log("F2: rp_opt: %lu of %u frames by the plan; per frame %.1f loads + %.1f stores dropped, %.1f loads made clears, checked colour "
                      "loads %.1f kept off / %.1f put back, %lu missed predictions; %.1f MiB per frame less (%.0f MB/s)",
                      s_os.frames, frames, s_os.ld / F, s_os.st / F, s_os.fold / F, s_os.v_ok / F, s_os.v_no / F, s_os.missed, s_os.saved / F / MB,
                      s_os.saved / MB / secs);
    for (size_t j = 0; j < RP_CAP; j++) { s_rp[j].w_n = 0; s_rp[j].w_bytes = 0; }
    memset(&s_rw, 0, sizeof s_rw);
    memset(&s_os, 0, sizeof s_os);
    pthread_mutex_unlock(&s_rp_mu);
}
/* from w_present: rp_opt learns from the frame; rp_log: the frame after an armed present is traced, the next present dumps it */
static void rp_present(unsigned long frame, double t)
{
    if (!s_track) return;
    pthread_mutex_lock(&s_rp_mu);
    opt_frame_end();
    if (s_tb) {
        if (tracing()) { __atomic_store_n(&s_tr_on, 0, __ATOMIC_RELEASE); rt_dump(); }
        else if (s_tr_n < 8 && frame > 30 && t - s_tr_last >= 20000) {
            s_tbw = 0; s_tb_full = 0; memset(s_tr_ref, 0, sizeof s_tr_ref); s_tr_n++;
            for (size_t j = 0; j < CB_CAP; j++) if (s_cb[j].h) { s_cb[j].rdirty = 1; }
            tb("(frame %lu)", frame);
            s_tr_last = t;
            __atomic_store_n(&s_tr_on, 1, __ATOMIC_RELEASE);
        }
    }
    pthread_mutex_unlock(&s_rp_mu);
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
        if (f2_cfg.rp_log || s_opt) rp_report(s_window_frames, (t - s_window_start) / 1000.0);
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
    rp_present(n, t);
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
    if (f2_cfg.rp_log || f2_cfg.rp_opt) {   /* rp_log / rp_opt: the hooks only pass through until vkCreateDevice turns them on */
        W("vkCreateFramebuffer", r_create_fb, w_create_fb)
        W("vkCmdBeginRenderPass", r_begin_rp, w_begin_rp)
        W("vkCmdPipelineBarrier", r_barrier, w_barrier)
        W("vkCreateImage", r_create_image, w_create_image)
        W("vkCreateImageView", r_create_view, w_create_view)
        W("vkGetSwapchainImagesKHR", r_sc_images, w_sc_images)
        W("vkAllocateDescriptorSets", r_alloc_ds, w_alloc_ds)
        W("vkUpdateDescriptorSets", r_upd_ds, w_upd_ds)
        W("vkUpdateDescriptorSetWithTemplate", r_upd_tmpl, w_upd_tmpl)
        W("vkUpdateDescriptorSetWithTemplateKHR", r_upd_tmpl_khr, w_upd_tmpl_khr)
        W("vkCmdBindDescriptorSets", r_bind_ds, w_bind_ds)
        W("vkCmdBindPipeline", r_bind_pipe, w_bind_pipe)
        W("vkCmdSetViewport", r_set_vp, w_set_vp)
        W("vkCmdSetScissor", r_set_sc, w_set_sc)
        W("vkCmdEndRenderPass", r_end_rp, w_end_rp)
        W("vkCmdNextSubpass", r_next_sp, w_next_sp)
        W("vkCmdDraw", r_draw, w_draw)
        W("vkCmdDrawIndexed", r_draw_ix, w_draw_ix)
        W("vkCmdDrawIndirect", r_draw_ind, w_draw_ind)
        W("vkCmdDrawIndexedIndirect", r_draw_ix_ind, w_draw_ix_ind)
        W("vkCmdDispatch", r_dispatch, w_dispatch)
        W("vkCmdDispatchIndirect", r_dispatch_ind, w_dispatch_ind)
        W("vkCmdCopyImage", r_copy_img, w_copy_img)
        W("vkCmdBlitImage", r_blit_img, w_blit_img)
        W("vkCmdResolveImage", r_resolve_img, w_resolve_img)
        W("vkCmdCopyBufferToImage", r_copy_b2i, w_copy_b2i)
        W("vkCmdCopyImageToBuffer", r_copy_i2b, w_copy_i2b)
        W("vkCmdClearColorImage", r_clr_c, w_clr_c)
        W("vkCmdClearDepthStencilImage", r_clr_z, w_clr_z)
        W("vkCmdClearAttachments", r_clr_att, w_clr_att)
        W("vkCmdExecuteCommands", r_exec, w_exec)
        W("vkBeginCommandBuffer", r_begin_cb, w_begin_cb)
        W("vkEndCommandBuffer", r_end_cb, w_end_cb)
        W("vkQueueSubmit", r_submit, w_submit)
        W("vkDestroyImage", r_destroy_image, w_destroy_image)
        W("vkDestroyImageView", r_destroy_view, w_destroy_view)
        W("vkDestroyFramebuffer", r_destroy_fb, w_destroy_fb)
        W("vkDestroyPipeline", r_destroy_pipe, w_destroy_pipe)
        W("vkFreeDescriptorSets", r_free_ds, w_free_ds)
        W("vkCmdBeginQuery", r_begin_query, w_begin_query)
        W("vkCmdWriteTimestamp", r_timestamp, w_timestamp)
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
