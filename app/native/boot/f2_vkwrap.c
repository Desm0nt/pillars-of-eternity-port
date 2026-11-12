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

static VKAPI_ATTR VkResult VKAPI_CALL w_create_rp(VkDevice d, const VkRenderPassCreateInfo *ci, const VkAllocationCallbacks *al, VkRenderPass *p)
{
    VkResult r = r_create_rp(d, ci, al, p);
    static unsigned bad;
    if (r != VK_SUCCESS && bad++ < 20) f2_log("F2: [RED] vkCreateRenderPass FAILED %d (attachments %u)", r, ci ? ci->attachmentCount : 0);
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
static VKAPI_ATTR VkResult VKAPI_CALL w_present(VkQueue q, const VkPresentInfoKHR *pi)
{
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
    if (t - s_window_start >= 5000) {
        f2_log("F2: frames: %lu total, %.1f fps over the last %.1f s (shaders %lu, pipelines %lu, pipeline time %.0f ms)", n,
               s_window_frames * 1000.0 / (t - s_window_start), (t - s_window_start) / 1000.0, f2_cnt_shaders, f2_cnt_pipelines, s_pipe_ms);
        f2_screen_note_fps((float)(s_window_frames * 1000.0 / (t - s_window_start)));      /* the frame cap checks itself against this */
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
