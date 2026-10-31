// Second 10-bit test for AQ_MGPU_CPU_COPY: glReadPixels of 2101010 straight into CPU memory
// (GL_RGB + GL_UNSIGNED_INT_2_10_10_10_REV, the only pair the NVIDIA driver accepts) is correct but takes
// ~330 ms per full frame (software conversion in the driver). Here, two alternatives:
//   pbo:    the same glReadPixels, but into a pixel pack buffer, then memcpy into the Intel bo
//   shader: NVIDIA redraws the 10-bit frame into an RGBA8 renderbuffer with the bits already packed
//           (each pixel becomes the XBGR2101010 word split into 4 bytes) and reads it via the fast 8-bit path
// The source frame has a pattern that depends on x and y; the result in the Intel LINEAR bo is checked
// word by word. Uses only render nodes: runs alongside the graphical session.
//
// build: gcc -O2 -o readback_10bit_pbo_vs_shader readback_10bit_pbo_vs_shader.c $(pkg-config --cflags --libs gbm egl glesv2 libdrm)
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
#define FMT    DRM_FORMAT_XBGR2101010

static PFNEGLCREATEIMAGEKHRPROC                     createImage;
static PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC imageToRenderbuffer;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC          imageToTexture;

#define PASSO(...) do { printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static double agora(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

typedef struct {
    int                fd;
    struct gbm_device* gbm;
    EGLDisplay         dpy;
    EGLContext         ctx;
} Gpu;

static void abrir(Gpu* g, const char* no) {
    g->fd = open(no, O_RDWR | O_CLOEXEC);
    if (g->fd < 0) { perror(no); exit(2); }
    g->gbm = gbm_create_device(g->fd);
    PFNEGLGETPLATFORMDISPLAYEXTPROC getDpy = (void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
    g->dpy = getDpy(EGL_PLATFORM_GBM_KHR, g->gbm, NULL);
    EGLint maj, min;
    if (!eglInitialize(g->dpy, &maj, &min)) exit(2);
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
    g->ctx = eglCreateContext(g->dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs);
    if (g->ctx == EGL_NO_CONTEXT) exit(2);
    eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g->ctx);
}

static void usar(Gpu* g) { eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g->ctx); }

static EGLImageKHR imagem(Gpu* g, struct gbm_bo* bo) {
    int          dmafd = gbm_bo_get_fd(bo);
    uint64_t     mod   = gbm_bo_get_modifier(bo);
    const EGLint a[]   = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_LINUX_DRM_FOURCC_EXT, FMT,
                          EGL_DMA_BUF_PLANE0_FD_EXT, dmafd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                          EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)gbm_bo_get_stride(bo),
                          EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(mod & 0xffffffff),
                          EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(mod >> 32), EGL_NONE};
    EGLImageKHR  img   = createImage(g->dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, a);
    close(dmafd);
    if (img == EGL_NO_IMAGE_KHR) { PASSO("eglCreateImage failed 0x%x", eglGetError()); exit(4); }
    return img;
}

static GLuint fboDeRb(GLuint rb) {
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { PASSO("FBO incomplete"); exit(5); }
    return fbo;
}

static GLuint programa(const char* fs) {
    static const char* vs = "#version 300 es\n"
                            "void main() { vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);"
                            " gl_Position = vec4(p, 0.0, 1.0); }\n";
    GLuint p = glCreateProgram();
    const char* src[2] = {vs, fs};
    GLenum      tipo[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
    for (int i = 0; i < 2; i++) {
        GLuint s = glCreateShader(tipo[i]);
        glShaderSource(s, 1, &src[i], NULL);
        glCompileShader(s);
        GLint ok;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[2048]; glGetShaderInfoLog(s, sizeof log, NULL, log); PASSO("shader: %s", log); exit(6); }
        glAttachShader(p, s);
    }
    glLinkProgram(p);
    return p;
}

// source frame pattern and the expected XBGR2101010 word at (x, y) (bits 30-31 ignored)
static const char* FS_PADRAO = "#version 300 es\n"
                               "precision highp float; out vec4 o;\n"
                               "void main() { ivec2 p = ivec2(gl_FragCoord.xy);"
                               " o = vec4(float(p.x % 1024), float((p.x * 3 + p.y) % 1024), float(p.y % 1024), 1023.0) / 1023.0; }\n";
static uint32_t esperado(int x, int y) { return (uint32_t)(x % 1024) | (uint32_t)((x * 3 + y) % 1024) << 10 | (uint32_t)(y % 1024) << 20; }

// pack: each 10-bit texel becomes the 4 bytes of the XBGR2101010 word (byte 0 = bits 0-7) in R,G,B,A
// (BGRA = 1: swaps R and B so that glReadPixels GL_BGRA_EXT returns the bytes in the same order)
static const char* FS_EMPACOTAR = "#version 300 es\n"
                                  "precision highp float; precision highp int; uniform highp sampler2D t; uniform int bgra; out vec4 o;\n"
                                  "void main() { vec3 c = texelFetch(t, ivec2(gl_FragCoord.xy), 0).rgb;"
                                  " uvec3 v = uvec3(round(clamp(c, 0.0, 1.0) * 1023.0));"
                                  " uint w = v.r | (v.g << 10) | (v.b << 20) | (3u << 30);"
                                  " vec4 b = vec4(uvec4(w, w >> 8, w >> 16, w >> 24) & 255u) / 255.0;"
                                  " o = bgra == 1 ? b.bgra : b; }\n";

static long conferir(uint8_t* ptr, uint32_t stride, int aw, int ah) {
    long erros = 0;
    for (int y = 0; y < ah; y++)
        for (int x = 0; x < aw; x++) {
            uint32_t w = *(uint32_t*)(ptr + (size_t)y * stride + (size_t)x * 4) & 0x3fffffff;
            if (w != esperado(x, y) && erros++ < 3)
                PASSO("      mismatch at (%d,%d): 0x%08x, expected 0x%08x", x, y, w, esperado(x, y));
        }
    return erros;
}

int main(void) {
    Gpu nv, in;
    abrir(&nv, NVIDIA);
    abrir(&in, INTEL);
    createImage         = (void*)eglGetProcAddress("eglCreateImageKHR");
    imageToRenderbuffer = (void*)eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
    imageToTexture      = (void*)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    // source: NVIDIA native bo (like the Hyprland swapchain in nvidia-usbc mode), with the pattern
    usar(&nv);
    struct gbm_bo* src = gbm_bo_create(nv.gbm, W, H, FMT, GBM_BO_USE_RENDERING);
    if (!src) { PASSO("gbm_bo_create XBGR2101010 failed"); return 3; }
    EGLImageKHR img = imagem(&nv, src);
    GLuint      rbSrc;
    glGenRenderbuffers(1, &rbSrc);
    glBindRenderbuffer(GL_RENDERBUFFER, rbSrc);
    imageToRenderbuffer(GL_RENDERBUFFER, img);
    GLuint fboSrc = fboDeRb(rbSrc);
    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glViewport(0, 0, W, H);
    glUseProgram(programa(FS_PADRAO));
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    PASSO("NVIDIA native XBGR2101010 source (modifier 0x%llx) with pattern", (unsigned long long)gbm_bo_get_modifier(src));

    // destination: CPU-mapped Intel LINEAR bo (in aquamarine it is a dumb buffer, same layout)
    usar(&in);
    uint64_t       lin = DRM_FORMAT_MOD_LINEAR;
    struct gbm_bo* dst = gbm_bo_create_with_modifiers2(in.gbm, W, H, FMT, &lin, 1, GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR);
    if (!dst) dst = gbm_bo_create(in.gbm, W, H, FMT, GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR);
    if (!dst) dst = gbm_bo_create(in.gbm, W, H, FMT, GBM_BO_USE_LINEAR);
    PASSO("Intel bo: %s", dst ? "allocated" : "allocation failed");
    uint32_t       stride;
    void*          mapa = NULL;
    uint8_t*       ptr  = dst ? gbm_bo_map(dst, 0, 0, W, H, GBM_BO_TRANSFER_READ_WRITE, &stride, &mapa) : NULL;
    if (!ptr) { PASSO("Intel LINEAR bo failed"); return 3; }
    usar(&nv);

    const int areas[][2] = {{W, H}, {2560, 1350}, {1600, 120}};

    // --- pbo
    GLuint pbo;
    glGenBuffers(1, &pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)W * H * 4, NULL, GL_STREAM_READ);
    glBindFramebuffer(GL_FRAMEBUFFER, fboSrc);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    for (int i = 0; i < 3; i++) {
        int aw = areas[i][0], ah = areas[i][1];
        memset(ptr, 0, (size_t)stride * H);
        while (glGetError() != GL_NO_ERROR) {}
        double t0 = agora(), tr = 0;
        for (int k = 0; k < 5; k++) {
            glPixelStorei(GL_PACK_ROW_LENGTH, aw);
            glReadPixels(0, 0, aw, ah, GL_RGB, GL_UNSIGNED_INT_2_10_10_10_REV, 0);
            double t1   = agora();
            uint8_t* m  = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)aw * ah * 4, GL_MAP_READ_BIT);
            tr += agora() - t1;
            if (!m) { PASSO("glMapBufferRange failed 0x%x", glGetError()); break; }
            for (int y = 0; y < ah; y++)
                memcpy(ptr + (size_t)y * stride, m + (size_t)y * aw * 4, (size_t)aw * 4);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
        PASSO("pbo    area %4dx%-4d: %7.2f ms/frame (%.2f ms waiting for the map), glError 0x%x, errors %ld", aw, ah,
              (agora() - t0) * 200, tr * 200, glGetError(), conferir(ptr, stride, aw, ah));
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);

    // --- shader: source texture -> RGBA8 renderbuffer with the bits packed -> 8-bit glReadPixels
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    imageToTexture(GL_TEXTURE_2D, img);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLuint rb8;
    glGenRenderbuffers(1, &rb8);
    glBindRenderbuffer(GL_RENDERBUFFER, rb8);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
    GLuint fbo8 = fboDeRb(rb8);
    GLuint prog = programa(FS_EMPACOTAR);
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "t"), 0);
    for (int bgra = 0; bgra < 2; bgra++) {
        glUniform1i(glGetUniformLocation(prog, "bgra"), bgra);
        for (int i = 0; i < 3; i++) {
            int aw = areas[i][0], ah = areas[i][1];
            memset(ptr, 0, (size_t)stride * H);
            while (glGetError() != GL_NO_ERROR) {}
            glPixelStorei(GL_PACK_ROW_LENGTH, stride / 4);
            double t0 = agora(), td = 0;
            for (int k = 0; k < 10; k++) {
                double t1 = agora();
                glBindFramebuffer(GL_FRAMEBUFFER, fbo8);
                glEnable(GL_SCISSOR_TEST);
                glScissor(0, 0, aw, ah);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                glDisable(GL_SCISSOR_TEST);
                glFinish();
                td += agora() - t1;
                glReadPixels(0, 0, aw, ah, bgra ? GL_BGRA_EXT : GL_RGBA, GL_UNSIGNED_BYTE, ptr);
            }
            PASSO("shader %s area %4dx%-4d: %7.2f ms/frame (%.2f ms drawing), glError 0x%x, errors %ld", bgra ? "BGRA" : "RGBA", aw, ah,
                  (agora() - t0) * 100, td * 100, glGetError(), conferir(ptr, stride, aw, ah));
            glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        }
    }
    gbm_bo_unmap(dst, mapa);
    PASSO("END without aborting");
    return 0;
}
