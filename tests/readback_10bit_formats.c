// Standalone test of the 10-bit (2101010) copy between GPUs for AQ_MGPU_CPU_COPY, outside Hyprland.
// Uses only the render nodes (no DRM master), so it runs alongside any graphical session.
//
// For XRGB2101010 and XBGR2101010: NVIDIA allocates its native frame (as the patch does), paints a known
// color and glReadPixels reads into memory with each GL format/type combination. Shows which ones the driver
// accepts and in which DRM layout the bits land. Then measures the readback straight into a mapped Intel LINEAR bo.
//
// build: gcc -O2 -o readback_10bit_formats readback_10bit_formats.c $(pkg-config --cflags --libs gbm egl glesv2 libdrm)
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

#define W      5120
#define H      2160
#define INTEL  "/dev/dri/renderD128"
#define NVIDIA "/dev/dri/renderD129"

static PFNEGLCREATEIMAGEKHRPROC                     createImage;
static PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC imageToRenderbuffer;

#define PASSO(...) do { printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static double agora(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

typedef struct {
    const char*        nome;
    int                fd;
    struct gbm_device* gbm;
    EGLDisplay         dpy;
    EGLContext         ctx;
} Gpu;

static void abrir(Gpu* g, const char* nome, const char* no) {
    g->nome = nome;
    g->fd   = open(no, O_RDWR | O_CLOEXEC);
    if (g->fd < 0) { perror(no); exit(2); }
    g->gbm = gbm_create_device(g->fd);
    PFNEGLGETPLATFORMDISPLAYEXTPROC getDpy = (void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
    g->dpy = getDpy(EGL_PLATFORM_GBM_KHR, g->gbm, NULL);
    EGLint maj, min;
    if (!eglInitialize(g->dpy, &maj, &min)) { PASSO("[%s] eglInitialize failed 0x%x", nome, eglGetError()); exit(2); }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
    g->ctx = eglCreateContext(g->dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs);
    if (g->ctx == EGL_NO_CONTEXT) { PASSO("[%s] eglCreateContext failed 0x%x", nome, eglGetError()); exit(2); }
    eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g->ctx);
    PASSO("[%s] %s / %s", nome, glGetString(GL_RENDERER), glGetString(GL_VERSION));
}

static void usar(Gpu* g) { eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g->ctx); }

static const char* nomeFmt(uint32_t f) { return f == DRM_FORMAT_XRGB2101010 ? "XRGB2101010" : f == DRM_FORMAT_XBGR2101010 ? "XBGR2101010" : "?"; }

static GLuint fboDeBo(Gpu* g, struct gbm_bo* bo, uint32_t fmt) {
    int          dmafd = gbm_bo_get_fd(bo);
    uint64_t     mod   = gbm_bo_get_modifier(bo);
    const EGLint a[]   = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_LINUX_DRM_FOURCC_EXT, (EGLint)fmt,
                          EGL_DMA_BUF_PLANE0_FD_EXT, dmafd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                          EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)gbm_bo_get_stride(bo),
                          EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(mod & 0xffffffff),
                          EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(mod >> 32), EGL_NONE};
    EGLImageKHR  img   = createImage(g->dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, a);
    close(dmafd);
    if (img == EGL_NO_IMAGE_KHR) { PASSO("[%s] eglCreateImage %s failed 0x%x", g->nome, nomeFmt(fmt), eglGetError()); return 0; }
    GLuint rb, fbo;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    imageToRenderbuffer(GL_RENDERBUFFER, img);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) { PASSO("[%s] FBO %s incomplete 0x%x", g->nome, nomeFmt(fmt), st); return 0; }
    return fbo;
}

// 32-bit word read back -> channels per the DRM layout (bits 0-9, 10-19, 20-29)
static void decodificar(const char* rotulo, uint32_t w) {
    unsigned b0 = w & 0x3ff, b1 = (w >> 10) & 0x3ff, b2 = (w >> 20) & 0x3ff;
    const char* layout = (b0 == 1023 && b2 < 300) ? "R in the low bits = XBGR2101010"
                       : (b2 == 1023 && b0 < 300) ? "B in the low bits = XRGB2101010" : "unexpected layout";
    PASSO("    %-34s word 0x%08x -> bits 0-9=%4u 10-19=%4u 20-29=%4u  (%s)", rotulo, w, b0, b1, b2, layout);
}

int main(void) {
    Gpu nv, in;
    abrir(&nv, "NVIDIA", NVIDIA);
    abrir(&in, "Intel", INTEL);
    createImage         = (void*)eglGetProcAddress("eglCreateImageKHR");
    imageToRenderbuffer = (void*)eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");

    const uint32_t fmts[] = {DRM_FORMAT_XRGB2101010, DRM_FORMAT_XBGR2101010};
    for (int f = 0; f < 2; f++) {
        const uint32_t fmt = fmts[f];
        PASSO("== %s (painted color: R=1.0 G=0.5 B=0.25 -> expected R=1023 G~512 B~256)", nomeFmt(fmt));
        usar(&nv);
        PASSO("[NVIDIA] gbm_device_is_format_supported(RENDERING)=%d (SCANOUT|RENDERING)=%d",
              gbm_device_is_format_supported(nv.gbm, fmt, GBM_BO_USE_RENDERING),
              gbm_device_is_format_supported(nv.gbm, fmt, GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT));
        struct gbm_bo* src = gbm_bo_create(nv.gbm, W, H, fmt, GBM_BO_USE_RENDERING);
        if (!src) {
            uint64_t mods[] = {0x300000000cdb014ULL, 0x300000000606014ULL, 0x300000000e08014ULL};
            src = gbm_bo_create_with_modifiers2(nv.gbm, W, H, fmt, mods, 3, GBM_BO_USE_RENDERING);
            PASSO("[NVIDIA] gbm_bo_create failed; with block-linear modifiers: %s", src ? "ok" : "failed too");
        }
        if (!src) { PASSO("[NVIDIA] gbm_bo_create(%s, RENDERING) failed", nomeFmt(fmt)); continue; }
        PASSO("[NVIDIA] native bo: stride %u modifier 0x%llx", gbm_bo_get_stride(src), (unsigned long long)gbm_bo_get_modifier(src));
        GLuint fsrc = fboDeBo(&nv, src, fmt);
        if (!fsrc) continue;
        glClearColor(1.0f, 0.5f, 0.25f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();

        GLint rf = 0, rt = 0;
        glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &rf);
        glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &rt);
        PASSO("[NVIDIA] GL_IMPLEMENTATION_COLOR_READ_FORMAT/TYPE = 0x%x / 0x%x", rf, rt);

        struct { const char* nome; GLenum fmt, tipo; } combos[] = {
            {"GL_RGBA + 2_10_10_10_REV", GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV},
            {"GL_BGRA_EXT + 2_10_10_10_REV", GL_BGRA_EXT, GL_UNSIGNED_INT_2_10_10_10_REV},
            {"GL_RGB + 2_10_10_10_REV", GL_RGB, GL_UNSIGNED_INT_2_10_10_10_REV},
        };
        for (int c = 0; c < 3; c++) {
            uint32_t px = 0xdeadbeef;
            while (glGetError() != GL_NO_ERROR) {}
            glReadPixels(W / 2, H / 2, 1, 1, combos[c].fmt, combos[c].tipo, &px);
            GLenum e = glGetError();
            if (e != GL_NO_ERROR)
                PASSO("    %-34s REJECTED (glError 0x%x)", combos[c].nome, e);
            else
                decodificar(combos[c].nome, px);
        }

        // real readback: straight into a CPU-mapped Intel LINEAR bo, as the patch does with the dumb buffer
        const uint32_t dstFmt = fmt; // changed below if only GL_RGBA works
        uint64_t       lin    = DRM_FORMAT_MOD_LINEAR;
        usar(&in);
        struct gbm_bo* dst = gbm_bo_create_with_modifiers2(in.gbm, W, H, dstFmt, &lin, 1, GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR);
        if (!dst) dst = gbm_bo_create(in.gbm, W, H, dstFmt, GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR);
        if (!dst) { PASSO("[Intel] LINEAR bo %s failed", nomeFmt(dstFmt)); continue; }
        uint32_t stride;
        void*    mapa = NULL;
        uint8_t* ptr  = gbm_bo_map(dst, 0, 0, W, H, GBM_BO_TRANSFER_WRITE, &stride, &mapa);
        if (!ptr) { PASSO("[Intel] gbm_bo_map failed"); continue; }
        usar(&nv);
        glBindFramebuffer(GL_FRAMEBUFFER, fsrc);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glPixelStorei(GL_PACK_ROW_LENGTH, stride / 4);
        const int areas[][2] = {{W, H}, {2560, 1350}, {1600, 120}};
        for (int c = 2; c < 3; c++) {
            for (int i = 0; i < 3; i++) {
                int    aw = areas[i][0], ah = areas[i][1];
                while (glGetError() != GL_NO_ERROR) {}
                double  t0 = agora();
                clock_t c0 = clock();
                for (int k = 0; k < 10; k++)
                    glReadPixels(0, 0, aw, ah, combos[c].fmt, combos[c].tipo, ptr);
                GLenum e = glGetError();
                PASSO("    %-30s area %4dx%-4d: %6.2f ms/frame, CPU %6.2f ms (glError 0x%x)", combos[c].nome, aw, ah,
                      (agora() - t0) * 100, (double)(clock() - c0) / CLOCKS_PER_SEC * 100, e);
            }
        }
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        decodificar("Intel bo, pixel (100,100)", *(uint32_t*)(ptr + 100 * stride + 400));
        gbm_bo_unmap(dst, mapa);
        gbm_bo_destroy(dst);
        gbm_bo_destroy(src);
    }

    // 8-bit reference (current patch path)
    {
        usar(&nv);
        struct gbm_bo* src = gbm_bo_create(nv.gbm, W, H, DRM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING);
        GLuint         fsrc = src ? fboDeBo(&nv, src, DRM_FORMAT_XRGB8888) : 0;
        if (fsrc) {
            uint8_t* mem = malloc((size_t)W * H * 4);
            glBindFramebuffer(GL_FRAMEBUFFER, fsrc);
            double t0 = agora();
            for (int k = 0; k < 10; k++)
                glReadPixels(0, 0, W, H, GL_BGRA_EXT, GL_UNSIGNED_BYTE, mem);
            PASSO("== reference XRGB8888 GL_BGRA_EXT/UNSIGNED_BYTE full frame (plain memory): %.2f ms", (agora() - t0) * 100);
            free(mem);
        }
    }
    PASSO("END without aborting");
    return 0;
}
