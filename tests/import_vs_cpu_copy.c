// Standalone test of the frame copy between GPUs (NVIDIA renders, Intel scans out), outside Hyprland.
// Uses only the render nodes (no DRM master), so it runs alongside any graphical session.
//
//   a: current aquamarine path. NVIDIA allocates the frame LINEAR, Intel imports it, copies it into
//      one of its own buffers and creates the EGL_ANDROID_native_fence (where Mesa aborted in Hyprland).
//   b: variant "2a". Intel allocates the LINEAR destination (system memory), NVIDIA imports it and
//      copies one of its native frames into it. The CPU only reads 1 pixel at the end to check.
//
// build: gcc -O2 -o import_vs_cpu_copy import_vs_cpu_copy.c $(pkg-config --cflags --libs gbm egl glesv2 libdrm)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
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
#define INTEL  "/dev/dri/renderD128"
#ifndef NVIDIA
#define NVIDIA "/dev/dri/renderD129"
#endif

static PFNEGLCREATEIMAGEKHRPROC                     createImage;
static PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC imageToRenderbuffer;
static PFNEGLCREATESYNCKHRPROC                      createSync;
static PFNEGLDUPNATIVEFENCEFDANDROIDPROC            dupFence;
static PFNEGLQUERYDMABUFMODIFIERSEXTPROC            queryMods;

#define PASSO(...) do { printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static double agora(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

typedef struct {
    const char*         nome;
    int                 fd;
    struct gbm_device*  gbm;
    EGLDisplay          dpy;
    EGLContext          ctx;
} Gpu;

static void abrir(Gpu* g, const char* nome, const char* no) {
    g->nome = nome;
    g->fd   = open(no, O_RDWR | O_CLOEXEC);
    if (g->fd < 0) { perror(no); exit(2); }
    g->gbm = gbm_create_device(g->fd);
    PASSO("[%s] gbm backend: %s", nome, gbm_device_get_backend_name(g->gbm));
    PFNEGLGETPLATFORMDISPLAYEXTPROC getDpy = (void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
    g->dpy = getDpy(EGL_PLATFORM_GBM_KHR, g->gbm, NULL);
    EGLint maj, min;
    if (!eglInitialize(g->dpy, &maj, &min)) { PASSO("[%s] eglInitialize failed 0x%x", nome, eglGetError()); exit(2); }
    PASSO("[%s] EGL %d.%d, vendor %s", nome, maj, min, eglQueryString(g->dpy, EGL_VENDOR));
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
    g->ctx = eglCreateContext(g->dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs);
    if (g->ctx == EGL_NO_CONTEXT) { PASSO("[%s] eglCreateContext failed 0x%x", nome, eglGetError()); exit(2); }
    eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g->ctx);
    PASSO("[%s] GL renderer: %s", nome, glGetString(GL_RENDERER));
}

static void usar(Gpu* g) { eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g->ctx); }

// is the LINEAR modifier importable? is it read-only (external-only)?
static void consultarLinear(Gpu* g) {
    EGLuint64KHR mods[64];
    EGLBoolean   ext[64];
    EGLint       n = 0;
    queryMods(g->dpy, DRM_FORMAT_XRGB8888, 64, mods, ext, &n);
    for (int i = 0; i < n; i++)
        if (mods[i] == DRM_FORMAT_MOD_LINEAR) {
            PASSO("[%s] XRGB8888 LINEAR: importable, external_only=%d (1 = cannot render into it)", g->nome, ext[i]);
            return;
        }
    PASSO("[%s] XRGB8888 LINEAR: NOT listed among %d modifiers", g->nome, n);
}

static struct gbm_bo* alocarLinear(Gpu* g, uint32_t flags) {
    uint64_t       lin = DRM_FORMAT_MOD_LINEAR;
    struct gbm_bo* bo  = gbm_bo_create_with_modifiers2(g->gbm, W, H, GBM_FORMAT_XRGB8888, &lin, 1, flags);
    if (!bo) {
        PASSO("[%s] LINEAR allocation with modifiers failed, trying GBM_BO_USE_LINEAR", g->nome);
        bo = gbm_bo_create(g->gbm, W, H, GBM_FORMAT_XRGB8888, flags | GBM_BO_USE_LINEAR);
    }
    if (!bo) { PASSO("[%s] LINEAR allocation failed", g->nome); exit(3); }
    PASSO("[%s] LINEAR bo %dx%d stride %u modifier 0x%llx", g->nome, W, H, gbm_bo_get_stride(bo),
          (unsigned long long)gbm_bo_get_modifier(bo));
    return bo;
}

// imports a bo (from either GPU) as the renderbuffer of an FBO on the current GPU
static GLuint fboDeBo(Gpu* g, struct gbm_bo* bo, const char* papel) {
    int           dmafd = gbm_bo_get_fd(bo);
    uint64_t      mod   = gbm_bo_get_modifier(bo);
    const EGLint  a[]   = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_XRGB8888,
                           EGL_DMA_BUF_PLANE0_FD_EXT, dmafd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                           EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)gbm_bo_get_stride(bo),
                           EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(mod & 0xffffffff),
                           EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(mod >> 32), EGL_NONE};
    EGLImageKHR   img   = createImage(g->dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, a);
    close(dmafd);
    if (img == EGL_NO_IMAGE_KHR) { PASSO("[%s] import %s: eglCreateImage failed 0x%x", g->nome, papel, eglGetError()); exit(4); }
    GLuint rb, fbo;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    imageToRenderbuffer(GL_RENDERBUFFER, img);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    PASSO("[%s] %s imported as FBO: %s (glError 0x%x)", g->nome, papel,
          st == GL_FRAMEBUFFER_COMPLETE ? "complete" : "INCOMPLETE", glGetError());
    if (st != GL_FRAMEBUFFER_COMPLETE) exit(5);
    return fbo;
}

static int fence(Gpu* g) {
    PASSO("[%s] creating EGL_ANDROID_native_fence (the step where Mesa aborted)...", g->nome);
    const EGLint a[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, EGL_NO_NATIVE_FENCE_FD_ANDROID, EGL_NONE};
    EGLSyncKHR   s   = createSync(g->dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, a);
    if (s == EGL_NO_SYNC_KHR) { PASSO("[%s] eglCreateSync failed 0x%x", g->nome, eglGetError()); return -1; }
    glFlush();
    int fd = dupFence(g->dpy, s);
    PASSO("[%s] fence ok, fd %d", g->nome, fd);
    return fd;
}

static void blit(GLuint de, GLuint para) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, de);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, para);
    glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

static void conferirPixel(GLuint fbo, const char* onde) {
    uint8_t px[4] = {0};
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    PASSO("pixel in %s: R=%d G=%d B=%d (expected green: 0 255 0)", onde, px[0], px[1], px[2]);
}

int main(int argc, char** argv) {
    const char* modo = argc > 1 ? argv[1] : "a";
    Gpu nv, in;
    abrir(&nv, "NVIDIA", NVIDIA);
    abrir(&in, "Intel", INTEL);
    createImage         = (void*)eglGetProcAddress("eglCreateImageKHR");
    imageToRenderbuffer = (void*)eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
    createSync          = (void*)eglGetProcAddress("eglCreateSyncKHR");
    dupFence            = (void*)eglGetProcAddress("eglDupNativeFenceFDANDROID");
    queryMods           = (void*)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
    consultarLinear(&nv);
    consultarLinear(&in);

    if (strcmp(modo, "a") == 0) {
        PASSO("== A: NVIDIA allocates LINEAR, Intel imports and copies (current aquamarine path)");
        struct gbm_bo* src = alocarLinear(&nv, GBM_BO_USE_RENDERING);
        usar(&nv);
        GLuint fsrc = fboDeBo(&nv, src, "LINEAR source (NVIDIA renders)");
        glClearColor(0, 1, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        PASSO("[NVIDIA] frame painted green");

        usar(&in);
        GLuint fsrcIn = fboDeBo(&in, src, "NVIDIA source (Intel reads)");
        struct gbm_bo* dst = gbm_bo_create(in.gbm, W, H, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
        PASSO("[Intel] scanout destination modifier 0x%llx", (unsigned long long)gbm_bo_get_modifier(dst));
        GLuint fdst = fboDeBo(&in, dst, "scanout destination");
        double t0 = agora();
        blit(fsrcIn, fdst);
        int f = fence(&in);
        glFinish();
        PASSO("[Intel] copy of 1 frame: %.2f ms", (agora() - t0) * 1000);
        if (f >= 0) close(f);
        conferirPixel(fdst, "Intel destination");
    } else if (strcmp(modo, "d") == 0) {
        PASSO("== D: patch path: NVIDIA native gbm_bo_create(RENDERING), imports with the real modifier, reads into Intel");
        struct gbm_bo* src = gbm_bo_create(nv.gbm, W, H, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING);
        if (!src) { PASSO("[NVIDIA] gbm_bo_create(RENDERING) failed"); return 7; }
        PASSO("[NVIDIA] native bo: stride %u modifier 0x%llx", gbm_bo_get_stride(src), (unsigned long long)gbm_bo_get_modifier(src));
        usar(&nv);
        GLuint fsrc = fboDeBo(&nv, src, "NVIDIA native bo (with the real modifier)");
        glClearColor(0, 1, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        struct gbm_bo* dst = alocarLinear(&in, GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
        uint32_t       stride;
        void*          mapa = NULL;
        uint8_t*       ptr  = gbm_bo_map(dst, 0, 0, W, H, GBM_BO_TRANSFER_READ_WRITE, &stride, &mapa);
        usar(&nv);
        glBindFramebuffer(GL_FRAMEBUFFER, fsrc);
        glPixelStorei(GL_PACK_ROW_LENGTH, stride / 4);
        double t0 = agora();
        glReadPixels(0, 0, W, H, GL_BGRA_EXT, GL_UNSIGNED_BYTE, ptr);
        PASSO("[NVIDIA->Intel] full frame readback: %.2f ms (glError 0x%x)", (agora() - t0) * 1000, glGetError());
        PASSO("Intel memory, pixel (0,0) bytes B G R X = %d %d %d %d (expected 0 255 0 x)", ptr[0], ptr[1], ptr[2], ptr[3]);
        gbm_bo_unmap(dst, mapa);
    } else if (strcmp(modo, "c") == 0) {
        PASSO("== C: NVIDIA renders natively; the NVIDIA driver reads (glReadPixels) straight into a mapped Intel LINEAR bo");
        struct gbm_bo* dst = alocarLinear(&in, GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
        uint32_t       stride;
        void*          mapa = NULL;
        uint8_t*       ptr  = gbm_bo_map(dst, 0, 0, W, H, GBM_BO_TRANSFER_WRITE, &stride, &mapa);
        if (!ptr) { PASSO("[Intel] gbm_bo_map failed"); return 6; }
        PASSO("[Intel] bo mapped for writing, stride %u", stride);
        usar(&nv);
        GLuint rb, fsrc;
        glGenRenderbuffers(1, &rb);
        glBindRenderbuffer(GL_RENDERBUFFER, rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
        glGenFramebuffers(1, &fsrc);
        glBindFramebuffer(GL_FRAMEBUFFER, fsrc);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
        glClearColor(0, 1, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        glPixelStorei(GL_PACK_ROW_LENGTH, stride / 4); // writes with the Intel bo's stride
        // damaged area: full screen, editor window, a few lines of text
        const int areas[][2] = {{W, H}, {2560, 1350}, {1600, 120}};
        for (int i = 0; i < 3; i++) {
            int    aw = areas[i][0], ah = areas[i][1];
            double t0 = agora();
            clock_t c0 = clock();
            for (int k = 0; k < 20; k++)
                glReadPixels(0, 0, aw, ah, GL_BGRA_EXT, GL_UNSIGNED_BYTE, ptr); // XRGB8888 = B,G,R,X in memory
            double ms  = (agora() - t0) * 1000 / 20;
            double cpu = (double)(clock() - c0) / CLOCKS_PER_SEC * 1000 / 20;
            PASSO("[NVIDIA->Intel] area %4dx%-4d: %6.2f ms per frame, CPU %6.2f ms per frame (glError 0x%x)", aw, ah, ms, cpu, glGetError());
        }
        gbm_bo_unmap(dst, mapa);
        usar(&in);
        GLuint fdstIn = fboDeBo(&in, dst, "Intel bo (read back)");
        conferirPixel(fdstIn, "Intel buffer");
    } else {
        PASSO("== B: Intel allocates the LINEAR destination, NVIDIA copies into it (variant 2a)");
        struct gbm_bo* dst = alocarLinear(&in, GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
        usar(&nv);
        GLuint rb, fsrc;
        glGenRenderbuffers(1, &rb);
        glBindRenderbuffer(GL_RENDERBUFFER, rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
        glGenFramebuffers(1, &fsrc);
        glBindFramebuffer(GL_FRAMEBUFFER, fsrc);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
        glClearColor(0, 1, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        PASSO("[NVIDIA] native (block-linear) frame painted green");
        GLuint fdst = fboDeBo(&nv, dst, "Intel LINEAR destination (NVIDIA writes)");
        double t0 = agora();
        blit(fsrc, fdst);
        int f = fence(&nv);
        glFinish();
        PASSO("[NVIDIA] copy of 1 frame into Intel memory: %.2f ms", (agora() - t0) * 1000);
        if (f >= 0) close(f);
        usar(&in);
        GLuint fdstIn = fboDeBo(&in, dst, "destination (Intel reads back)");
        conferirPixel(fdstIn, "Intel buffer");
    }
    PASSO("END without aborting");
    return 0;
}
