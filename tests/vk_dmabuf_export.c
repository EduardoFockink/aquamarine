// Standalone test of a zero-CPU-copy path for "NVIDIA renders, Intel scans out" (option 3 of the 2026-11-13
// investigation): the NVIDIA allocates a LINEAR buffer through Vulkan, exports it as a dma-buf, and copies its
// native (block-linear) frame into it with its copy engine; the Intel imports that dma-buf. No CPU touches pixels.
//
//   1. NVIDIA (Vulkan): exportable VkBuffer of stride*H bytes; tries every memory type it may live in.
//   2. NVIDIA: a 5120x2160 B8G8R8A8 image (optimal tiling) cleared to a known colour, copied into the buffer.
//   3. Intel (EGL/GLES, Mesa): imports the dma-buf as XRGB8888 LINEAR, samples it, reads pixels back to check.
//   4. Intel (KMS, primary node, no master): imports it with GBM and tries drmModeAddFB2 (scanout-capable?).
//   5. Timing: NVIDIA copy (GPU timestamps and CPU time of submit + wait), Intel full-frame blit from it.
//
// Uses render nodes for rendering; step 4 opens card1 only to create and remove a framebuffer object, which does
// not change what is on the screen.
//
// build: gcc -O2 -o vk_dmabuf_export vk_dmabuf_export.c $(pkg-config --cflags --libs vulkan gbm egl glesv2 libdrm)

#include <vulkan/vulkan.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef W
#define W 5120
#endif
#ifndef H
#define H 2160
#endif
#define STRIDE (W * 4)
#define INTEL_RENDER "/dev/dri/renderD128"
#define INTEL_CARD   "/dev/dri/card1"

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)
static double agora(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static VkInstance inst; static VkPhysicalDevice pd; static VkDevice dev; static VkQueue fila; static uint32_t qfam;
static VkCommandPool pool; static VkPhysicalDeviceMemoryProperties memprops;
static PFN_vkGetMemoryFdKHR getMemoryFd;

static void vk_iniciar(void) {
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vk_dmabuf_export", 1, NULL, 0, VK_API_VERSION_1_2 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL };
    CK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, NULL);
    VkPhysicalDevice ds[8]; if (n > 8) n = 8; vkEnumeratePhysicalDevices(inst, &n, ds);
    for (uint32_t i = 0; i < n; i++) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(ds[i], &p); if (p.vendorID == 0x10de) { pd = ds[i]; printf("[NVIDIA] Vulkan device: %s, driver %u.%u\n", p.deviceName, p.driverVersion >> 22, (p.driverVersion >> 14) & 0xff); } }
    if (!pd) { printf("FAIL: no NVIDIA Vulkan device\n"); exit(1); }
    vkGetPhysicalDeviceMemoryProperties(pd, &memprops);
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties qs[16]; if (nq > 16) nq = 16; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qs);
    /* prefer a transfer-only queue (the copy engine), fall back to graphics */
    qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) if ((qs[i].queueFlags & VK_QUEUE_TRANSFER_BIT) && !(qs[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) { qfam = i; break; }
    int so_copia = qfam != UINT32_MAX;
    if (!so_copia) for (uint32_t i = 0; i < nq; i++) if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfam = i; break; }
    printf("[NVIDIA] queue family %u (%s), timestampValidBits %u\n", qfam, so_copia ? "transfer only = copy engine" : "graphics", qs[qfam].timestampValidBits);
    const char *exts[] = { "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf" };
    float prio = 1.0f;
    VkDeviceQueueCreateInfo dq = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &dq, 0, NULL, 2, exts, NULL };
    CK(vkCreateDevice(pd, &dci, NULL, &dev));
    vkGetDeviceQueue(dev, qfam, 0, &fila);
    getMemoryFd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR");
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam };
    CK(vkCreateCommandPool(dev, &pci, NULL, &pool));
}

static uint32_t tipo_mem(uint32_t bits, VkMemoryPropertyFlags quer) {
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++) if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & quer) == quer) return i;
    return UINT32_MAX;
}

int main(int argc, char **argv) {
    vk_iniciar();

    /* ---- 1. exportable LINEAR buffer on the NVIDIA ---- */
    VkExternalMemoryBufferCreateInfo ebi = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkPhysicalDeviceExternalBufferInfo qbi = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO, NULL, 0, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkExternalBufferProperties qbp = { VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES };
    vkGetPhysicalDeviceExternalBufferProperties(pd, &qbi, &qbp);
    VkExternalMemoryFeatureFlags ef = qbp.externalMemoryProperties.externalMemoryFeatures;
    printf("[NVIDIA] dma-buf buffer: exportable=%d importable=%d dedicated_only=%d\n", !!(ef & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT), !!(ef & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT), !!(ef & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT));

    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ebi, 0, (VkDeviceSize)STRIDE * H, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VkBuffer buf; CK(vkCreateBuffer(dev, &bci, NULL, &buf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, buf, &mr);
    printf("[NVIDIA] buffer %zu bytes, allowed memory types 0x%x:\n", (size_t)mr.size, mr.memoryTypeBits);
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++) if (mr.memoryTypeBits & (1u << i)) {
        VkMemoryPropertyFlags f = memprops.memoryTypes[i].propertyFlags;
        printf("    type %u: %s%s%s%s (heap %u%s)\n", i, f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? "DEVICE_LOCAL " : "", f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? "HOST_VISIBLE " : "",
               f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? "HOST_COHERENT " : "", f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? "HOST_CACHED " : "", memprops.memoryTypes[i].heapIndex,
               memprops.memoryHeaps[memprops.memoryTypes[i].heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? ", VRAM" : ", system RAM");
    }
    /* the Intel must reach it: system RAM. Prefer HOST_VISIBLE (lives in sysmem); argv[1] = "vram" forces DEVICE_LOCAL to compare */
    int quer_vram = argc > 1 && !strcmp(argv[1], "vram");
    uint32_t mt = quer_vram ? tipo_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) : tipo_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == UINT32_MAX) { printf("FAIL: no suitable memory type\n"); return 1; }
    VkMemoryDedicatedAllocateInfo ded = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, buf };
    VkExportMemoryAllocateInfo exp = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &ded, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exp, mr.size, mt };
    VkDeviceMemory mem; CK(vkAllocateMemory(dev, &mai, NULL, &mem));
    CK(vkBindBufferMemory(dev, buf, mem, 0));
    VkMemoryGetFdInfoKHR gfi = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, mem, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    int fd = -1; CK(getMemoryFd(dev, &gfi, &fd));
    printf("[NVIDIA] allocated in memory type %u and exported as dma-buf fd %d\n", mt, fd);

    /* ---- 2. a native frame, cleared, copied into the buffer ---- */
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, { W, H, 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                              VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage img; CK(vkCreateImage(dev, &ici, NULL, &img));
    VkMemoryRequirements imr; vkGetImageMemoryRequirements(dev, img, &imr);
    VkMemoryAllocateInfo iai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, imr.size, tipo_mem(imr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VkDeviceMemory imem; CK(vkAllocateMemory(dev, &iai, NULL, &imem)); CK(vkBindImageMemory(dev, img, imem, 0));

    VkQueryPoolCreateInfo qpi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 2, 0 };
    VkQueryPool qp; CK(vkCreateQueryPool(dev, &qpi, NULL, &qp));
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);

    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fen; CK(vkCreateFence(dev, &fci, NULL, &fen));
    VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    /* known colour: R=0x33 G=0x99 B=0xff -> XRGB8888 word 0x003399ff (bytes ff 99 33 00). A transfer-only queue cannot
     * clear images, so a VRAM buffer is filled with the word and copied into the native image once, before timing. */
    VkBufferCreateInfo sci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, (VkDeviceSize)STRIDE * H, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VkBuffer fonte; CK(vkCreateBuffer(dev, &sci, NULL, &fonte));
    VkMemoryRequirements smr; vkGetBufferMemoryRequirements(dev, fonte, &smr);
    VkMemoryAllocateInfo sai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, smr.size, tipo_mem(smr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VkDeviceMemory smem; CK(vkAllocateMemory(dev, &sai, NULL, &smem)); CK(vkBindBufferMemory(dev, fonte, smem, 0));
    {
        VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        CK(vkBeginCommandBuffer(cb, &cbi));
        vkCmdFillBuffer(cb, fonte, 0, VK_WHOLE_SIZE, 0x003399ffu);
        VkBufferMemoryBarrier bb = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, fonte, 0, VK_WHOLE_SIZE };
        VkImageMemoryBarrier b1 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, rng };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &bb, 1, &b1);
        VkBufferImageCopy r0 = { 0, W, H, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
        vkCmdCopyBufferToImage(cb, fonte, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r0);
        VkImageMemoryBarrier b2 = b1; b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b2.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
        CK(vkEndCommandBuffer(cb));
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL };
        CK(vkQueueSubmit(fila, 1, &si, fen)); CK(vkWaitForFences(dev, 1, &fen, VK_TRUE, UINT64_MAX)); CK(vkResetFences(dev, 1, &fen));
    }
    int N = 60; double cpu_total = 0, gpu_total = 0;
    for (int it = 0; it <= N; it++) {
        CK(vkResetCommandBuffer(cb, 0));
        VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        CK(vkBeginCommandBuffer(cb, &cbi));
        vkCmdResetQueryPool(cb, qp, 0, 2);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, qp, 0);
        VkBufferImageCopy reg = { 0, W, H, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
        vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &reg);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, qp, 1);
        CK(vkEndCommandBuffer(cb));
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL };
        struct timespec c0, c1; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c0);
        CK(vkQueueSubmit(fila, 1, &si, fen));
        CK(vkWaitForFences(dev, 1, &fen, VK_TRUE, UINT64_MAX)); CK(vkResetFences(dev, 1, &fen));
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c1);
        uint64_t ts[2]; CK(vkGetQueryPoolResults(dev, qp, 0, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        if (it > 0) { cpu_total += (c1.tv_sec - c0.tv_sec) * 1e3 + (c1.tv_nsec - c0.tv_nsec) / 1e6; gpu_total += (ts[1] - ts[0]) * props.limits.timestampPeriod / 1e6; }
    }
    printf("[NVIDIA] copy of a %dx%d frame into the dma-buf: GPU %.2f ms, CPU %.3f ms (submit + fence wait, process CPU time) per frame, avg of %d\n", W, H, gpu_total / N, cpu_total / N, N);

    /* ---- 3. Intel imports the dma-buf and reads it ---- */
    int ifd = open(INTEL_RENDER, O_RDWR | O_CLOEXEC);
    struct gbm_device *ig = gbm_create_device(ifd);
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay idpy = gpd(EGL_PLATFORM_GBM_KHR, ig, NULL);
    EGLint maj, min; if (!eglInitialize(idpy, &maj, &min)) { printf("FAIL: Intel eglInitialize\n"); return 1; }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ca[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
    EGLContext ictx = eglCreateContext(idpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
    eglMakeCurrent(idpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ictx);
    printf("[Intel] GL renderer: %s\n", glGetString(GL_RENDERER));
    PFNEGLCREATEIMAGEKHRPROC createImage = (void *)eglGetProcAddress("eglCreateImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageToTex = (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    EGLint at[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_XRGB8888,
                    EGL_DMA_BUF_PLANE0_FD_EXT, fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0, EGL_DMA_BUF_PLANE0_PITCH_EXT, STRIDE,
                    EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(DRM_FORMAT_MOD_LINEAR & 0xffffffff), EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(DRM_FORMAT_MOD_LINEAR >> 32), EGL_NONE };
    EGLImageKHR eimg = createImage(idpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, at);
    printf("[Intel] import of the NVIDIA dma-buf as XRGB8888 LINEAR: %s (0x%x)\n", eimg ? "OK" : "FAILED", eglGetError());
    if (eimg) {
        GLuint tex; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex); imageToTex(GL_TEXTURE_2D, eimg);
        /* Intel-native destination (what the scanout buffer would be) and a full-frame blit from the import */
        GLuint dst; glGenTextures(1, &dst); glBindTexture(GL_TEXTURE_2D, dst); glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
        GLuint fs[2]; glGenFramebuffers(2, fs);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fs[0]); glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fs[1]); glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
        printf("[Intel] read FBO on the import: %s\n", glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE ? "complete" : "INCOMPLETE");
        glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST); glFinish();
        double t0 = agora(); for (int k = 0; k < 20; k++) glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST); glFinish();
        printf("[Intel] full-frame GPU blit from the import into an Intel buffer: %.2f ms per frame (wall, incl. GPU)\n", (agora() - t0) * 1000 / 20);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fs[1]);
        unsigned char px[4] = {0}; glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        printf("[Intel] pixel read back after the blit: R=%02x G=%02x B=%02x (expected 33 99 ff) -> %s\n", px[0], px[1], px[2], (px[0] == 0x33 && px[1] == 0x99 && px[2] == 0xff) ? "CORRECT" : "WRONG");
    }

    /* ---- 4. scanout: Intel KMS framebuffer from the same dma-buf ---- */
    int cfd = open(INTEL_CARD, O_RDWR | O_CLOEXEC);
    uint32_t handle = 0;
    if (cfd >= 0 && drmPrimeFDToHandle(cfd, fd, &handle) == 0) {
        uint32_t hs[4] = { handle }, ps[4] = { STRIDE }, os[4] = { 0 }, fb = 0;
        uint64_t mods[4] = { DRM_FORMAT_MOD_LINEAR };
        int r = drmModeAddFB2WithModifiers(cfd, W, H, DRM_FORMAT_XRGB8888, hs, ps, os, mods, &fb, DRM_MODE_FB_MODIFIERS);
        printf("[Intel] KMS framebuffer (scanout) from the NVIDIA dma-buf: %s%s\n", r == 0 ? "created" : "refused: ", r == 0 ? "" : strerror(-r));
        if (r == 0) drmModeRmFB(cfd, fb);
    } else printf("[Intel] could not import the dma-buf on %s (%s)\n", INTEL_CARD, strerror(errno));
    return 0;
}
