// Measures the CPU cost of the NVIDIA -> Intel dumb buffer readback (what AQ_MGPU_CPU_COPY does every frame).
// Compares the current patch path (synchronous glReadPixels straight into the mapped dumb buffer) with a PBO readback
// (NVIDIA copies into one of its buffers in RAM, the CPU only memcpys the rows). Runs offscreen.
// build: g++ -std=c++20 -O2 -o readback_cpu_cost readback_cpu_cost.cpp $(pkg-config --cflags --libs gbm egl glesv2 libdrm)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <drm.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <fcntl.h>
#include <gbm.h>
#include <sys/mman.h>
#include <poll.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <functional>
#include <vector>
#include <unistd.h>

static const int W = 5120, H = 2160, N = 30;

static double agora(clockid_t c) {
    timespec t;
    clock_gettime(c, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

// runs f N times (plus 3 warm-up runs) and prints CPU and wall-clock ms per frame
static void medir(const char* nome, const std::function<void()>& f) {
    for (int i = 0; i < 3; ++i)
        f();
    const double c0 = agora(CLOCK_PROCESS_CPUTIME_ID), w0 = agora(CLOCK_MONOTONIC);
    for (int i = 0; i < N; ++i)
        f();
    const double c = (agora(CLOCK_PROCESS_CPUTIME_ID) - c0) / N, w = (agora(CLOCK_MONOTONIC) - w0) / N;
    printf("  %-44s CPU %6.2f ms  wall %6.2f ms  (glError 0x%x)\n", nome, c, w, glGetError());
}

int main(int argc, char** argv) {
    // NVIDIA without a display, like the aquamarine renderer
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

    // RGBA8 source (the patch's 10-bit path also ends in an RGBA8 before reading)
    GLuint rb, fbo;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    glClearColor(0.2f, 0.4f, 0.6f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();

    // destination: a real dumb buffer on Intel (card1), mapped as the patch does
    int ifd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
    drm_mode_create_dumb cd{};
    cd.width = W, cd.height = H, cd.bpp = 32;
    if (ifd < 0 || drmIoctl(ifd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) {
        printf("ERROR: dumb buffer on Intel\n");
        return 1;
    }
    drm_mode_map_dumb md{};
    md.handle = cd.handle;
    drmIoctl(ifd, DRM_IOCTL_MODE_MAP_DUMB, &md);
    auto* dumb = (uint8_t*)mmap(nullptr, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, ifd, md.offset);
    const uint32_t STRIDE = cd.pitch;
    std::vector<uint8_t> comum((size_t)STRIDE * H);
    printf("Intel dumb buffer: pitch %u, %llu bytes\n", STRIDE, (unsigned long long)cd.size);

    GLuint pbo;
    glGenBuffers(1, &pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)W * H * 4, nullptr, GL_STREAM_READ);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);

    struct Ret { int x, y, w, h; const char* nome; };
    const Ret rets[] = {{0, 0, W, H, "full screen 5120x2160"}, {640, 270, 3840, 1620, "player 3840x1620"}};

    for (const auto& r : rets) {
        printf("%s:\n", r.nome);
        // 1) like the patch: straight into the destination, ROW_LENGTH = pitch
        auto direto = [&](uint8_t* base) {
            glPixelStorei(GL_PACK_ROW_LENGTH, STRIDE / 4);
            glReadPixels(r.x, r.y, r.w, r.h, GL_RGBA, GL_UNSIGNED_BYTE, base + (size_t)r.y * STRIDE + (size_t)r.x * 4);
            glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        };
        medir("synchronous -> Intel dumb (current)", [&] { direto(dumb); });
        medir("synchronous -> plain memory (control)", [&] { direto(comum.data()); });

        // 2) PBO: NVIDIA copies the tightly packed rectangle (no ROW_LENGTH), the CPU copies row by row
        medir("PBO + memcpy -> Intel dumb", [&] {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
            glReadPixels(r.x, r.y, r.w, r.h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            GLsync s = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            glClientWaitSync(s, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000);
            glDeleteSync(s);
            const size_t LINHA = (size_t)r.w * 4;
            auto* src = (const uint8_t*)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, LINHA * r.h, GL_MAP_READ_BIT);
            if (src)
                for (int y = 0; y < r.h; ++y)
                    memcpy(dumb + (size_t)(r.y + y) * STRIDE + (size_t)r.x * 4, src + y * LINHA, LINHA);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        });
    }

    // 3) GPU busy before the readback, as in Hyprland (composition + 10-bit shader right before):
    // if CPU time tracks wall-clock time while waiting for the GPU, the driver is spinning (busy-wait)
    const char* VS = "#version 300 es\nvoid main(){vec2 p=vec2(gl_VertexID==1?3.0:-1.0,gl_VertexID==2?3.0:-1.0);gl_Position=vec4(p,0,1);}";
    const char* FS = "#version 300 es\nprecision highp float;out vec4 o;uniform int n;void main(){vec2 c=gl_FragCoord.xy;float a=0.0;"
                     "for(int i=0;i<n;++i)a+=sin(c.x*0.01+float(i))*cos(c.y*0.01);o=vec4(a,0,0,1);}";
    auto sh = [](GLenum t, const char* s) { GLuint h = glCreateShader(t); glShaderSource(h, 1, &s, nullptr); glCompileShader(h); return h; };
    GLuint prog = glCreateProgram();
    glAttachShader(prog, sh(GL_VERTEX_SHADER, VS));
    glAttachShader(prog, sh(GL_FRAGMENT_SHADER, FS));
    glLinkProgram(prog);
    GLuint vao, rb2, fbo2;
    glGenVertexArrays(1, &vao);
    glGenRenderbuffers(1, &rb2);
    glBindRenderbuffer(GL_RENDERBUFFER, rb2);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
    glGenFramebuffers(1, &fbo2);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo2);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb2);
    const int ITER = argc > 1 ? atoi(argv[1]) : 40;
    auto carga = [&] {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo2);
        glViewport(0, 0, W, H);
        glUseProgram(prog);
        glUniform1i(glGetUniformLocation(prog, "n"), ITER);
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glFlush();
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    };
    printf("GPU busy before the readback (shader with %d iterations, __GL_YIELD=%s):\n", ITER, getenv("__GL_YIELD") ? getenv("__GL_YIELD") : "(default)");
    medir("load only + glFinish (GPU time)", [&] { carga(); glFinish(); glBindFramebuffer(GL_FRAMEBUFFER, fbo2); });
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    const Ret& p0 = rets[1];
    medir("load + synchronous player -> dumb (current)", [&] {
        carga();
        glPixelStorei(GL_PACK_ROW_LENGTH, STRIDE / 4);
        glReadPixels(p0.x, p0.y, p0.w, p0.h, GL_RGBA, GL_UNSIGNED_BYTE, dumb + (size_t)p0.y * STRIDE + (size_t)p0.x * 4);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    });

    // 4) wait for the GPU sleeping in the kernel: native fence (sync_file) + poll(), then read
    auto createSync  = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    auto destroySync = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    auto dupFence    = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");
    auto esperarDormindo = [&] {
        const EGLint at[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, EGL_NO_NATIVE_FENCE_FD_ANDROID, EGL_NONE};
        EGLSyncKHR s = createSync(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, at);
        glFlush();
        int ffd = dupFence(dpy, s);
        destroySync(dpy, s);
        if (ffd < 0) {
            printf("ERROR: no native fence\n");
            exit(2);
        }
        pollfd pf{ffd, POLLIN, 0};
        poll(&pf, 1, 1000);
        close(ffd);
    };
    medir("load + fence/poll + synchronous player -> dumb", [&] {
        carga();
        esperarDormindo();
        glPixelStorei(GL_PACK_ROW_LENGTH, STRIDE / 4);
        glReadPixels(p0.x, p0.y, p0.w, p0.h, GL_RGBA, GL_UNSIGNED_BYTE, dumb + (size_t)p0.y * STRIDE + (size_t)p0.x * 4);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    });
    medir("load + PBO + fence/poll + memcpy player -> dumb", [&] {
        carga();
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
        glReadPixels(p0.x, p0.y, p0.w, p0.h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        esperarDormindo();
        const size_t LINHA = (size_t)p0.w * 4;
        auto* src = (const uint8_t*)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, LINHA * p0.h, GL_MAP_READ_BIT);
        if (src)
            for (int y = 0; y < p0.h; ++y)
                memcpy(dumb + (size_t)(p0.y + y) * STRIDE + (size_t)p0.x * 4, src + y * LINHA, LINHA);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    });

    // checks that the PBO delivered the right pixels (glClear color: 0.2,0.4,0.6 -> 51,102,153)
    const uint8_t* p = dumb + (size_t)300 * STRIDE + 700 * 4;
    printf("checked pixel: %u %u %u %u (expected 51 102 153 255)\n", p[0], p[1], p[2], p[3]);
    return 0;
}
