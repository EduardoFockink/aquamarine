// Standalone test of the full chain for a zero-CPU-copy "NVIDIA renders, Intel scans out" path, the way it would
// sit inside aquamarine:
//
//   1. NVIDIA: GBM buffer with the driver's native modifier (what aquamarine's swapchain allocates), rendered with
//      GLES through an EGLImage (the compositor's frame), then an EGL native fence -> sync_file fd.
//   2. NVIDIA (Vulkan): imports that dma-buf as a VkImage with the explicit DRM modifier, waits on the GL fence as a
//      semaphore (no CPU wait), copies the frame on the transfer queue (copy engine) into a LINEAR buffer allocated
//      in system RAM and exported as dma-buf, and signals a sync_file fd.
//   3. Intel: imports the linear dma-buf, waits on the Vulkan sync_file inside its own GL (EGL_ANDROID_native_fence),
//      blits, reads back one pixel; KMS: drmModeAddFB2 with the format (scanout-capable?).
//   4. The bits in the linear buffer are checked through a CPU map (verification only).
//
// Run for XRGB8888 ("8") and XBGR2101010 ("10", the HDR format). Render nodes only; step 3 creates and removes a
// framebuffer object on card1, which does not change the screen.
//
// build: gcc -O2 -o vk_gbm_frame_copy vk_gbm_frame_copy.c $(pkg-config --cflags --libs vulkan gbm egl glesv2 libdrm)
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
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define W 5120
#define H 2160
#define STRIDE (W * 4)
#define NVIDIA_RENDER "/dev/dri/renderD129"
#define INTEL_RENDER  "/dev/dri/renderD128"
#define INTEL_CARD    "/dev/dri/card1"

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)
#define OK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf(" (line %d, egl 0x%x, gl 0x%x)\n", __LINE__, eglGetError(), glGetError()); exit(1); } } while (0)
static double ms_cpu(void) { struct timespec t; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static double ms_relogio(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

typedef struct { int fd; struct gbm_device *gbm; EGLDisplay dpy; EGLContext ctx; } Gl;
static PFNEGLCREATEIMAGEKHRPROC createImage;
static PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC imageToRb;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageToTex;
static PFNEGLCREATESYNCKHRPROC createSync;
static PFNEGLDUPNATIVEFENCEFDANDROIDPROC dupFence;
static PFNEGLWAITSYNCKHRPROC waitSync;
static PFNEGLQUERYDMABUFMODIFIERSEXTPROC queryMods;

static void gl_abrir(Gl *g, const char *no) {
    g->fd = open(no, O_RDWR | O_CLOEXEC); OK(g->fd >= 0, "open %s", no);
    g->gbm = gbm_create_device(g->fd);
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    g->dpy = gpd(EGL_PLATFORM_GBM_KHR, g->gbm, NULL);
    EGLint a, b; OK(eglInitialize(g->dpy, &a, &b), "eglInitialize %s", no);
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ca[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
    g->ctx = eglCreateContext(g->dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca); OK(g->ctx, "context %s", no);
    eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g->ctx);
}
static void usar(Gl *g) { eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g->ctx); }
static EGLImageKHR importar(Gl *g, int fd, uint32_t fmt, uint32_t pitch, uint32_t off, uint64_t mod) {
    EGLint at[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_LINUX_DRM_FOURCC_EXT, (EGLint)fmt, EGL_DMA_BUF_PLANE0_FD_EXT, fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)off,
                    EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)pitch, EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(mod & 0xffffffff), EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(mod >> 32), EGL_NONE };
    return createImage(g->dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, at);
}

/* ---- Vulkan on the NVIDIA ---- */
static VkPhysicalDevice pd; static VkDevice dev; static VkQueue fila; static uint32_t qfam; static VkPhysicalDeviceMemoryProperties memprops;
static PFN_vkGetMemoryFdKHR getMemoryFd; static PFN_vkGetMemoryFdPropertiesKHR getMemoryFdProps;
static PFN_vkImportSemaphoreFdKHR importSemFd; static PFN_vkGetSemaphoreFdKHR getSemFd;
static uint32_t tipo_mem(uint32_t bits, VkMemoryPropertyFlags quer) {
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++) if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & quer) == quer) return i;
    return UINT32_MAX;
}
static void vk_iniciar(void) {
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vk_gbm_frame_copy", 1, NULL, 0, VK_API_VERSION_1_2 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL };
    VkInstance inst; CK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t n = 8; VkPhysicalDevice ds[8]; vkEnumeratePhysicalDevices(inst, &n, ds);
    for (uint32_t i = 0; i < n; i++) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(ds[i], &p); if (p.vendorID == 0x10de) pd = ds[i]; }
    OK(pd, "no NVIDIA Vulkan device");
    vkGetPhysicalDeviceMemoryProperties(pd, &memprops);
    uint32_t nq = 16; VkQueueFamilyProperties qs[16]; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qs);
    qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) if ((qs[i].queueFlags & VK_QUEUE_TRANSFER_BIT) && !(qs[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) { qfam = i; break; }
    OK(qfam != UINT32_MAX, "no transfer-only queue");
    const char *exts[] = { "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf", "VK_EXT_image_drm_format_modifier", "VK_KHR_external_semaphore_fd" };
    float prio = 1.0f;
    VkDeviceQueueCreateInfo dq = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &dq, 0, NULL, 4, exts, NULL };
    CK(vkCreateDevice(pd, &dci, NULL, &dev));
    vkGetDeviceQueue(dev, qfam, 0, &fila);
    getMemoryFd = (void *)vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR");
    getMemoryFdProps = (void *)vkGetDeviceProcAddr(dev, "vkGetMemoryFdPropertiesKHR");
    importSemFd = (void *)vkGetDeviceProcAddr(dev, "vkImportSemaphoreFdKHR");
    getSemFd = (void *)vkGetDeviceProcAddr(dev, "vkGetSemaphoreFdKHR");
}

static int rodar(int dez) {
    uint32_t fmt = dez ? DRM_FORMAT_XBGR2101010 : DRM_FORMAT_XRGB8888;
    VkFormat vkfmt = dez ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VK_FORMAT_B8G8R8A8_UNORM;
    printf("\n==== %s ====\n", dez ? "XBGR2101010 (10-bit, HDR)" : "XRGB8888 (8-bit)");
    Gl nv, in;
    gl_abrir(&nv, NVIDIA_RENDER); gl_abrir(&in, INTEL_RENDER);
    createImage = (void *)eglGetProcAddress("eglCreateImageKHR"); imageToRb = (void *)eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
    imageToTex = (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES"); createSync = (void *)eglGetProcAddress("eglCreateSyncKHR");
    dupFence = (void *)eglGetProcAddress("eglDupNativeFenceFDANDROID"); waitSync = (void *)eglGetProcAddress("eglWaitSyncKHR");
    queryMods = (void *)eglGetProcAddress("eglQueryDmaBufModifiersEXT");

    /* ---- 1. NVIDIA native GBM buffer, rendered with GLES ---- */
    usar(&nv);
    uint64_t mods[64]; EGLBoolean ext[64]; EGLint nm = 0;
    queryMods(nv.dpy, fmt, 64, mods, ext, &nm);
    uint64_t rend[64]; int nr = 0;
    for (int i = 0; i < nm; i++) if (!ext[i]) rend[nr++] = mods[i];
    printf("[NVIDIA] %d modifiers, %d renderable\n", nm, nr);
    OK(nr > 0, "no renderable modifier");
    struct gbm_bo *bo = gbm_bo_create_with_modifiers2(nv.gbm, W, H, fmt, rend, nr, GBM_BO_USE_RENDERING);
    OK(bo, "gbm_bo_create_with_modifiers2");
    uint64_t mod = gbm_bo_get_modifier(bo); uint32_t bpitch = gbm_bo_get_stride_for_plane(bo, 0), boff = gbm_bo_get_offset(bo, 0);
    int bofd = gbm_bo_get_fd_for_plane(bo, 0);
    printf("[NVIDIA] frame buffer: modifier 0x%016llx, pitch %u, offset %u\n", (unsigned long long)mod, bpitch, boff);
    EGLImageKHR nimg = importar(&nv, bofd, fmt, bpitch, boff, mod); OK(nimg, "NVIDIA EGLImage of its own buffer");
    GLuint rb, fbo; glGenRenderbuffers(1, &rb); glBindRenderbuffer(GL_RENDERBUFFER, rb); imageToRb(GL_RENDERBUFFER, nimg);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo); glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    OK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "NVIDIA FBO");
    /* known colour: R=1.0, G=0.5, B=0.25 (8-bit: ff 80 40; 10-bit: 1023, 512, 256 with round-to-nearest... see check) */
    glClearColor(1.0f, 0.5f, 0.25f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
    /* one more draw-ish op so the fence covers real work */
    glEnable(GL_SCISSOR_TEST); glScissor(0, 0, 64, 64); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT); glDisable(GL_SCISSOR_TEST);
    EGLSyncKHR gsync = createSync(nv.dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL); OK(gsync, "NVIDIA native fence");
    glFlush();
    int glfence = dupFence(nv.dpy, gsync); OK(glfence >= 0, "dup GL fence fd");
    printf("[NVIDIA] GL frame rendered, native fence fd %d\n", glfence);

    /* ---- 2. Vulkan: import the frame, wait on the GL fence, copy to an exported LINEAR sysmem buffer ---- */
    VkImageDrmFormatModifierExplicitCreateInfoEXT expl = { VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT, NULL, mod, 1, NULL };
    VkSubresourceLayout lay = { boff, 0, bpitch, 0, 0 }; expl.pPlaneLayouts = &lay;
    VkExternalMemoryImageCreateInfo emi = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &expl, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &emi, 0, VK_IMAGE_TYPE_2D, vkfmt, { W, H, 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                              VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage vimg; CK(vkCreateImage(dev, &ici, NULL, &vimg));
    VkMemoryFdPropertiesKHR fdp = { VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
    int fdcopia = dup(bofd); CK(getMemoryFdProps(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fdcopia, &fdp));
    VkMemoryRequirements imr; vkGetImageMemoryRequirements(dev, vimg, &imr);
    VkMemoryDedicatedAllocateInfo idd = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, vimg, VK_NULL_HANDLE };
    VkImportMemoryFdInfoKHR imp = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, &idd, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fdcopia };
    VkMemoryAllocateInfo iai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, imr.size, tipo_mem(imr.memoryTypeBits & fdp.memoryTypeBits, 0) };
    VkDeviceMemory imem; CK(vkAllocateMemory(dev, &iai, NULL, &imem)); CK(vkBindImageMemory(dev, vimg, imem, 0));
    printf("[Vulkan] NVIDIA GL frame imported as VkImage (DRM modifier)\n");

    VkExternalMemoryBufferCreateInfo ebi = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ebi, 0, (VkDeviceSize)STRIDE * H, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VkBuffer lin; CK(vkCreateBuffer(dev, &bci, NULL, &lin));
    VkMemoryRequirements lmr; vkGetBufferMemoryRequirements(dev, lin, &lmr);
    VkMemoryDedicatedAllocateInfo ldd = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, lin };
    VkExportMemoryAllocateInfo exp = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &ldd, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkMemoryAllocateInfo lai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exp, lmr.size, tipo_mem(lmr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkDeviceMemory lmem; CK(vkAllocateMemory(dev, &lai, NULL, &lmem)); CK(vkBindBufferMemory(dev, lin, lmem, 0));
    VkMemoryGetFdInfoKHR gfi = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, lmem, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    int linfd; CK(getMemoryFd(dev, &gfi, &linfd));

    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore sem_gl; CK(vkCreateSemaphore(dev, &sci, NULL, &sem_gl));
    VkExportSemaphoreCreateInfo esi = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, NULL, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT };
    VkSemaphoreCreateInfo sci2 = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &esi };
    VkSemaphore sem_fim; CK(vkCreateSemaphore(dev, &sci2, NULL, &sem_fim));
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam };
    VkCommandPool pool; CK(vkCreateCommandPool(dev, &pci, NULL, &pool));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; VkFence fen; CK(vkCreateFence(dev, &fci, NULL, &fen));

    double c0 = ms_cpu(), r0 = ms_relogio();
    VkImportSemaphoreFdInfoKHR isf = { VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR, NULL, sem_gl, VK_SEMAPHORE_IMPORT_TEMPORARY_BIT, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, glfence };
    CK(importSemFd(dev, &isf));
    VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    CK(vkBeginCommandBuffer(cb, &cbi));
    VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    /* acquire from the foreign (GL) owner, keep the content (old layout GENERAL) */
    VkImageMemoryBarrier acq = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 VK_QUEUE_FAMILY_FOREIGN_EXT, qfam, vimg, rng };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &acq);
    VkBufferImageCopy reg = { 0, W, H, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
    vkCmdCopyImageToBuffer(cb, vimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, lin, 1, &reg);
    VkBufferMemoryBarrier rel = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, 0, qfam, VK_QUEUE_FAMILY_FOREIGN_EXT, lin, 0, VK_WHOLE_SIZE };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 1, &rel, 0, NULL);
    CK(vkEndCommandBuffer(cb));
    VkPipelineStageFlags espera = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 1, &sem_gl, &espera, 1, &cb, 1, &sem_fim };
    CK(vkQueueSubmit(fila, 1, &si, fen));
    VkSemaphoreGetFdInfoKHR sgf = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, NULL, sem_fim, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT };
    int vkfence = -1; CK(getSemFd(dev, &sgf, &vkfence));
    double c1 = ms_cpu();
    printf("[Vulkan] copy submitted (waits on the GL fence on the GPU), out sync_file fd %d; CPU to record+submit+export: %.3f ms\n", vkfence, c1 - c0);

    /* ---- 3. Intel: import, wait on the Vulkan fence in its own GL, blit, check ---- */
    usar(&in);
    EGLImageKHR iimg = importar(&in, linfd, fmt, STRIDE, 0, DRM_FORMAT_MOD_LINEAR);
    printf("[Intel] import of the linear dma-buf: %s\n", iimg ? "OK" : "FAILED");
    OK(iimg, "Intel import");
    EGLint sat[] = { EGL_SYNC_NATIVE_FENCE_FD_ANDROID, vkfence, EGL_NONE };
    EGLSyncKHR isync = createSync(in.dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, sat);
    printf("[Intel] EGL native fence from the Vulkan sync_file: %s\n", isync ? "created (Mesa did not abort)" : "FAILED");
    OK(isync, "Intel native fence");
    OK(waitSync(in.dpy, isync, 0), "eglWaitSyncKHR");
    GLuint tex; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex); imageToTex(GL_TEXTURE_2D, iimg);
    GLuint dst; glGenTextures(1, &dst); glBindTexture(GL_TEXTURE_2D, dst); glTexStorage2D(GL_TEXTURE_2D, 1, dez ? GL_RGB10_A2 : GL_RGBA8, W, H);
    GLuint fs[2]; glGenFramebuffers(2, fs);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fs[0]); glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fs[1]); glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
    printf("[Intel] read FBO on the import: %s, draw FBO: %s\n", glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE ? "complete" : "INCOMPLETE",
           glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE ? "complete" : "INCOMPLETE");
    glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    GLenum eb = glGetError();
    printf("[Intel] glBlitFramebuffer: %s (0x%x)\n", eb == GL_NO_ERROR ? "ok" : "GL ERROR", eb);
    if (eb != GL_NO_ERROR) {   /* fallback the compositor would use anyway: draw a full-screen triangle sampling the import */
        const char *vs = "#version 300 es\nout vec2 uv; void main(){ vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); uv = p; gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }";
        const char *fsrc = "#version 300 es\nprecision highp float; uniform sampler2D t; in vec2 uv; out vec4 c; void main(){ c = texture(t, uv); }";
        GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER), pr = glCreateProgram();
        glShaderSource(v, 1, &vs, NULL); glCompileShader(v); glShaderSource(f, 1, &fsrc, NULL); glCompileShader(f);
        glAttachShader(pr, v); glAttachShader(pr, f); glLinkProgram(pr); glUseProgram(pr);
        GLuint vao; glGenVertexArrays(1, &vao); glBindVertexArray(vao);
        glBindFramebuffer(GL_FRAMEBUFFER, fs[1]); glViewport(0, 0, W, H);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        printf("[Intel] shader copy instead: GL 0x%x\n", glGetError());
    }
    glFinish();
    double r1 = ms_relogio();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fs[1]);
    unsigned char px[4] = {0}, pc[4] = {0};
    if (dez) {   /* GLES reads RGB10_A2 only as GL_UNSIGNED_INT_2_10_10_10_REV; scale to 8 bits for the same check */
        uint32_t w1 = 0, w2 = 0;
        glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, &w1); glReadPixels(10, 10, 1, 1, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, &w2);
        for (int k = 0; k < 3; k++) { px[k] = (unsigned char)((((w1 >> (10 * k)) & 1023) * 255 + 511) / 1023); pc[k] = (unsigned char)((((w2 >> (10 * k)) & 1023) * 255 + 511) / 1023); }
    } else { glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px); glReadPixels(10, 10, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pc); }
    printf("[Intel] after GPU wait + blit, centre pixel R=%02x G=%02x B=%02x (expected ff 80 40 within 1)  -> %s\n", px[0], px[1], px[2],
           (px[0] >= 0xfe && abs(px[1] - 0x80) <= 1 && abs(px[2] - 0x40) <= 1) ? "CORRECT" : "WRONG");
    printf("[Intel] corner that GL scissor-cleared to black (first rows of the buffer): R=%02x G=%02x B=%02x -> %s\n", pc[0], pc[1], pc[2], (pc[0] < 2 && pc[1] < 2 && pc[2] < 2) ? "CORRECT" : "WRONG");
    printf("[chain] GL fence -> Vulkan copy -> Intel wait+blit: %.2f ms wall clock, all on the GPUs\n", r1 - r0);

    /* ---- 4. the raw bits through a CPU map (verification only) ---- */
    void *map; CK(vkMapMemory(dev, lmem, 0, VK_WHOLE_SIZE, 0, &map));
    uint32_t pal = ((uint32_t *)map)[(size_t)(H / 2) * W + W / 2];
    if (dez) printf("[bits] XBGR2101010 word 0x%08x: R=%u G=%u B=%u (expected 1023, 511/512, 255/256)\n", pal, pal & 1023, (pal >> 10) & 1023, (pal >> 20) & 1023);
    else printf("[bits] XRGB8888 word 0x%08x: R=%02x G=%02x B=%02x\n", pal, (pal >> 16) & 255, (pal >> 8) & 255, pal & 255);
    vkUnmapMemory(dev, lmem);

    /* ---- 5. scanout check ---- */
    int cfd = open(INTEL_CARD, O_RDWR | O_CLOEXEC); uint32_t h = 0;
    if (cfd >= 0 && drmPrimeFDToHandle(cfd, linfd, &h) == 0) {
        uint32_t hs[4] = { h }, ps[4] = { STRIDE }, os[4] = { 0 }, fb = 0; uint64_t ms[4] = { DRM_FORMAT_MOD_LINEAR };
        int r = drmModeAddFB2WithModifiers(cfd, W, H, fmt, hs, ps, os, ms, &fb, DRM_MODE_FB_MODIFIERS);
        printf("[Intel KMS] framebuffer %s LINEAR %dx%d from the NVIDIA buffer: %s%s\n", dez ? "XBGR2101010" : "XRGB8888", W, H, r == 0 ? "created" : "refused: ", r == 0 ? "" : strerror(-r));
        if (r == 0) drmModeRmFB(cfd, fb);
        close(cfd);
    }
    return 0;
}

int main(int argc, char **argv) {
    vk_iniciar();
    if (argc < 2 || !strcmp(argv[1], "8")) rodar(0);
    if (argc < 2 || !strcmp(argv[1], "10")) rodar(1);
    return 0;
}
