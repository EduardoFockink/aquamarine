// Checks the patch's 10-bit shader (text extracted from Renderer.cpp into pack_shader.inc) on NVIDIA:
// native XBGR2101010 source with a known pattern -> patch shader -> RGBA8 -> glReadPixels GL_RGBA.
// Checks both outputs (swapRB 0 = XBGR2101010, 1 = XRGB2101010) word by word.
// build: g++ -std=c++20 -O2 -o pack_shader_check pack_shader_check.cpp $(pkg-config --cflags --libs gbm egl glesv2 libdrm)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>
#include <unistd.h>
#include "pack_shader.inc"

static const int W = 5120, H = 2160;

static GLuint comp(GLenum t, const std::string& s) {
    GLuint sh = glCreateShader(t);
    const char* c = s.c_str();
    glShaderSource(sh, 1, &c, nullptr);
    glCompileShader(sh);
    GLint ok;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[4096]; glGetShaderInfoLog(sh, sizeof log, nullptr, log); printf("ERROR in shader: %s\n", log); exit(6); }
    return sh;
}
static GLuint prog(const std::string& v, const std::string& f) {
    GLuint p = glCreateProgram();
    glAttachShader(p, comp(GL_VERTEX_SHADER, v));
    glAttachShader(p, comp(GL_FRAGMENT_SHADER, f));
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { printf("ERROR in link\n"); exit(6); }
    return p;
}

int main() {
    int fd = open("/dev/dri/renderD129", O_RDWR | O_CLOEXEC);
    gbm_device* gbm = gbm_create_device(fd);
    auto getDpy = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy = getDpy(EGL_PLATFORM_GBM_KHR, gbm, nullptr);
    eglInitialize(dpy, nullptr, nullptr);
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint ca[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
    printf("%s\n", glGetString(GL_RENDERER));
    auto createImage = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    auto toRb  = (PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC)eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
    auto toTex = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    gbm_bo* bo = gbm_bo_create(gbm, W, H, DRM_FORMAT_XBGR2101010, GBM_BO_USE_RENDERING);
    int dfd = gbm_bo_get_fd(bo);
    uint64_t mod = gbm_bo_get_modifier(bo);
    const EGLint a[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_XBGR2101010, EGL_DMA_BUF_PLANE0_FD_EXT, dfd,
                        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0, EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)gbm_bo_get_stride(bo),
                        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(mod & 0xffffffff), EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(mod >> 32), EGL_NONE};
    EGLImageKHR img = createImage(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, a);
    GLuint vao; glGenVertexArrays(1, &vao); glBindVertexArray(vao);

    // pattern in the source
    GLuint rb, fbo; glGenRenderbuffers(1, &rb); glBindRenderbuffer(GL_RENDERBUFFER, rb); toRb(GL_RENDERBUFFER, img);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo); glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    glViewport(0, 0, W, H);
    glUseProgram(prog(VERT_SRC_PACK, "#version 300 es\nprecision highp float; out vec4 o; void main() { ivec2 p = ivec2(gl_FragCoord.xy);"
                                     " o = vec4(float(p.x % 1024), float((p.x * 3 + p.y) % 1024), float(p.y % 1024), 1023.0) / 1023.0; }"));
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // patch shader, RGBA8 target
    GLuint tex; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex); toTex(GL_TEXTURE_2D, img);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLuint rb8, fbo8; glGenRenderbuffers(1, &rb8); glBindRenderbuffer(GL_RENDERBUFFER, rb8); glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
    glGenFramebuffers(1, &fbo8); glBindFramebuffer(GL_FRAMEBUFFER, fbo8); glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb8);
    GLuint p = prog(VERT_SRC_PACK, packFragSrc(false));
    printf("patch shader (sampler2D) compiled; samplerExternalOES variant: ");
    prog(VERT_SRC_PACK, packFragSrc(true));
    printf("compiled\n");
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "tex"), 0);
    glUniform2f(glGetUniformLocation(p, "invSize"), 1.f / W, 1.f / H);
    std::vector<uint32_t> mem((size_t)W * H);
    for (int swap = 0; swap < 2; swap++) {
        glUniform1i(glGetUniformLocation(p, "swapRB"), swap);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, mem.data());
        long erros = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                uint32_t r = x % 1024, g = (x * 3 + y) % 1024, b = y % 1024;
                uint32_t esperado = (swap ? (b | g << 10 | r << 20) : (r | g << 10 | b << 20)) | 3u << 30;
                if (mem[(size_t)y * W + x] != esperado && erros++ < 3)
                    printf("  (%d,%d) 0x%08x expected 0x%08x\n", x, y, mem[(size_t)y * W + x], esperado);
            }
        printf("swapRB=%d (%s): glError 0x%x, %ld wrong pixels out of %d\n", swap, swap ? "XRGB2101010" : "XBGR2101010", glGetError(), erros, W * H);
    }
    close(dfd);
    return 0;
}
