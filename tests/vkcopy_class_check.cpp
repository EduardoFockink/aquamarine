// Checks the AQ_MGPU_GPU_COPY classes themselves (src/backend/drm/mgpu), linked from the built library, outside any
// compositor: a real GL frame on the NVIDIA (GBM buffer with the native modifier, like aquamarine's swapchain) is
// copied by CMgpuVkCopy into one of its exported linear buffers, first whole, then only a damaged rectangle, in
// 8-bit and 10-bit formats. Pixels are checked through a CPU map of the linear buffer (verification only), so row 0
// of the buffer must be GL's y = 0, as aquamarine's damage regions assume. Also times copy() on the CPU.
//
// It also runs AQ_MGPU_GPU_COPY_VERIFY's check (mgpuVerifySamples) against a real CDRMRenderer on the NVIDIA: the
// CPU reader and the GPU copy must agree on a correct copy (no false alarm in a session), and a frame that changed
// without being copied must be reported.
//
// build (from the aquamarine worktree with the mgpu module):
//   g++ -std=c++23 -O2 -I include -I src/include -I src/backend/drm -I src/backend/drm/mgpu -I build \
//       -o build/vkcopy_class_check ../packaging/tests/vkcopy_class_check.cpp -L build -laquamarine \
//       -Wl,-rpath,$PWD/build $(pkg-config --cflags --libs hyprutils vulkan gbm egl glesv2 libdrm pixman-1)
#include "VkCopy.hpp"
#include "CpuCopy.hpp"
#include "MgpuCopy.hpp"
#include "Renderer.hpp"
#include <aquamarine/backend/Backend.hpp>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <poll.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

using namespace Aquamarine;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Math;

static const int W = 5120, H = 2160;

class CFrame : public IBuffer {
  public:
    SDMABUFAttrs      a;
    eBufferCapability caps() override { return BUFFER_CAPABILITY_NONE; }
    eBufferType       type() override { return BUFFER_TYPE_DMABUF; }
    void              update(const CRegion&) override {}
    bool              isSynchronous() override { return false; }
    bool              good() override { return true; }
    SDMABUFAttrs      dmabuf() override { return a; }
};

static double cpuMs() { timespec t; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static int    falhas = 0;
#define CHECK(c, ...) do { bool ok_ = (c); printf("  [%s] ", ok_ ? "ok" : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) falhas++; } while (0)

int main() {
    int                nvfd = open("/dev/dri/renderD129", O_RDWR | O_CLOEXEC);
    gbm_device*        gbm  = gbm_create_device(nvfd);
    auto               gpd  = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay         dpy  = gpd(EGL_PLATFORM_GBM_KHR, gbm, nullptr);
    EGLint             ma, mi;
    eglInitialize(dpy, &ma, &mi);
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint     ca[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
    EGLContext ctx  = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
    auto createImage = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    auto imageToRb   = (PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC)eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
    auto createSync  = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    auto dupFence    = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");
    auto queryMods   = (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)eglGetProcAddress("eglQueryDmaBufModifiersEXT");

    // the class needs an fd of the primary GPU to match its Vulkan device: the render node works
    // regression (2026-11-13): inside the compositor $DISPLAY is the compositor's own Xwayland, which cannot answer
    // while the compositor is blocked; the NVIDIA Vulkan driver connects to it during enumeration -> deadlock. Run
    // this with DISPLAY pointing at a server that never answers (x_mudo.py): create() must still return.
    const char* d0 = getenv("DISPLAY"); std::string antes = d0 ? d0 : "(unset)";
    timespec ta; clock_gettime(CLOCK_MONOTONIC, &ta);
    auto copier = CMgpuVkCopy::create(nvfd, {});
    timespec tb; clock_gettime(CLOCK_MONOTONIC, &tb);
    if (!copier) { printf("FAIL: CMgpuVkCopy::create\n"); return 1; }
    const char* d1 = getenv("DISPLAY");
    printf("create(): %.0f ms with DISPLAY=%s; DISPLAY afterwards: %s\n", (tb.tv_sec - ta.tv_sec) * 1e3 + (tb.tv_nsec - ta.tv_nsec) / 1e6, antes.c_str(), d1 ? d1 : "(unset)");
    CHECK(antes == (d1 ? d1 : "(unset)"), "DISPLAY restored after create()");

    // a real aquamarine renderer on the NVIDIA, on a backend that touches no device, for the CPU reader
    SBackendImplementationOptions impl; impl.backendType = AQ_BACKEND_NULL; impl.backendRequestMode = AQ_BACKEND_REQUEST_MANDATORY;
    SBackendOptions               bopts; bopts.logFunction = [](eBackendLogLevel l, std::string m) { if (l >= AQ_LOG_ERROR) printf("  (aquamarine) %s\n", m.c_str()); };
    auto backend  = CBackend::create({impl}, bopts);
    auto renderer = backend ? CDRMRenderer::attempt(backend, nvfd) : nullptr;
    CHECK(!!renderer, "CDRMRenderer on the NVIDIA, outside a session");
    auto reader = renderer ? makeShared<CMgpuCpuReader>(renderer) : nullptr;
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);

    for (uint32_t fmt : {DRM_FORMAT_XRGB8888, DRM_FORMAT_XBGR2101010}) {
        const bool DEZ = fmt == DRM_FORMAT_XBGR2101010;
        printf("== %s ==\n", DEZ ? "XBGR2101010" : "XRGB8888");
        uint64_t mods[64]; EGLBoolean ext[64]; EGLint nm = 0;
        queryMods(dpy, fmt, 64, mods, ext, &nm);
        uint64_t rend[64]; int nr = 0;
        for (int i = 0; i < nm; i++) if (!ext[i]) rend[nr++] = mods[i];
        gbm_bo* bo = gbm_bo_create_with_modifiers2(gbm, W, H, fmt, rend, nr, GBM_BO_USE_RENDERING);
        auto    frame = makeShared<CFrame>();
        frame->a = {.success = true, .size = Vector2D(W, H), .format = fmt, .modifier = gbm_bo_get_modifier(bo), .planes = 1};
        frame->a.strides[0] = gbm_bo_get_stride_for_plane(bo, 0); frame->a.offsets[0] = gbm_bo_get_offset(bo, 0); frame->a.fds[0] = gbm_bo_get_fd_for_plane(bo, 0);
        EGLint at[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_LINUX_DRM_FOURCC_EXT, (EGLint)fmt, EGL_DMA_BUF_PLANE0_FD_EXT, frame->a.fds[0], EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)frame->a.offsets[0],
                       EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)frame->a.strides[0], EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(frame->a.modifier & 0xffffffff),
                       EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(frame->a.modifier >> 32), EGL_NONE};
        EGLImageKHR img = createImage(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, at);
        GLuint rb, fbo; glGenRenderbuffers(1, &rb); glBindRenderbuffer(GL_RENDERBUFFER, rb); imageToRb(GL_RENDERBUFFER, img);
        glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo); glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);

        auto lin = copier->allocate(Vector2D(W, H), fmt);
        CHECK(lin && lin->good(), "allocate(): linear %dx%d, pitch %u, exported fd %d", W, H, lin ? lin->stride : 0, lin ? lin->dmabuf().fds[0] : -1);
        // regression (2026-11-13): IBuffer::size (not only dmabuf().size) sizes the KMS plane; 0x0 gave a black screen
        CHECK(lin->size == Vector2D(W, H) && lin->dmabuf().size == Vector2D(W, H), "IBuffer::size and dmabuf().size are %gx%g / %gx%g", lin->size.x, lin->size.y, lin->dmabuf().size.x, lin->dmabuf().size.y);
        CHECK(lin->dmabuf().modifier == DRM_FORMAT_MOD_LINEAR && lin->dmabuf().planes == 1 && lin->dmabuf().strides[0] == lin->stride, "dmabuf(): LINEAR, 1 plane, pitch %u", lin->dmabuf().strides[0]);
        const void* map = lin->map();
        CHECK(map != nullptr, "map(): CPU view of the linear buffer");
        auto px = [&](int x, int y) { return ((uint32_t*)map)[(size_t)y * (lin->stride / 4) + x]; };
        auto canal = [&](uint32_t w, int c) -> int { return DEZ ? (int)((w >> (10 * c)) & 1023) : (int)((w >> (16 - 8 * c)) & 255); }; // c: 0=R 1=G 2=B
        auto fence = [&]() { EGLSyncKHR s = createSync(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr); glFlush(); return dupFence(dpy, s); };
        auto wait  = [&](int fd) { pollfd p = {fd, POLLIN, 0}; poll(&p, 1, 1000); close(fd); };

        // frame 1: whole frame red-ish, full copy
        glDisable(GL_SCISSOR_TEST); glClearColor(1.0f, 0.0f, 0.0f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
        int     gf = fence();
        CRegion full; full.add(0, 0, W, H);
        int     done = copier->copy(frame, lin, full, gf);
        close(gf);
        CHECK(done >= 0, "copy(full) returned a sync_file");
        if (getenv("READ_SPEED")) { // how fast can the CPU read the linear buffer back? (sizes the verify mode)
            wait(dup(done));
            timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
            uint64_t sum = 0; const uint64_t* q = (const uint64_t*)map; const size_t n = (size_t)lin->stride * H / 8;
            for (size_t i = 0; i < n; ++i) sum += q[i];
            clock_gettime(CLOCK_MONOTONIC, &b);
            printf("  CPU read of the whole linear buffer (%.1f MiB): %.1f ms (sum %llx)\n", n * 8 / 1048576.0, (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6, (unsigned long long)sum);
        }
        wait(done);
        uint32_t p0 = px(W / 2, H / 2);
        CHECK(canal(p0, 0) == (DEZ ? 1023 : 255) && canal(p0, 1) == 0 && canal(p0, 2) == 0, "full copy: centre is red (word 0x%08x)", p0);
        if (reader) { // verify: a correct full copy must compare equal
            timespec va, vb; clock_gettime(CLOCK_MONOTONIC, &va);
            auto v = mgpuVerifySamples(reader, frame, lin, full, 1);
            clock_gettime(CLOCK_MONOTONIC, &vb);
            eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            CHECK(v.ran && v.pixels > 0 && v.bad == 0, "verify after a full copy: %llu pixels in %llu blocks, %llu differ %s (%.1f ms)", (unsigned long long)v.pixels,
                  (unsigned long long)v.blocks, (unsigned long long)v.bad, v.first.c_str(), (vb.tv_sec - va.tv_sec) * 1e3 + (vb.tv_nsec - va.tv_nsec) / 1e6);
        }

        // frame 2: GL draws green only in a rectangle at GL (x=100, y=50, 300x200); damage = the same rect in buffer
        // coords. The copy must touch exactly those rows of the linear buffer, counted from row 0.
        glEnable(GL_SCISSOR_TEST); glScissor(100, 50, 300, 200); glClearColor(0.0f, 1.0f, 0.0f, 1.0f); glClear(GL_COLOR_BUFFER_BIT); glDisable(GL_SCISSOR_TEST);
        // and blue everywhere else in the GL frame, which must NOT reach the linear buffer (not damaged)
        glEnable(GL_SCISSOR_TEST); glScissor(1000, 1000, 500, 500); glClearColor(0.0f, 0.0f, 1.0f, 1.0f); glClear(GL_COLOR_BUFFER_BIT); glDisable(GL_SCISSOR_TEST);
        gf = fence();
        CRegion dmg; dmg.add(100, 50, 300, 200);
        double c0 = cpuMs();
        done = copier->copy(frame, lin, dmg, gf);
        double c1 = cpuMs();
        close(gf);
        wait(done);
        uint32_t in = px(250, 150), out = px(1200, 1200), edge = px(99, 150), below = px(250, 250);
        CHECK(canal(in, 1) == (DEZ ? 1023 : 255) && canal(in, 0) == 0, "damaged rect (row 150, col 250) is green (0x%08x)", in);
        CHECK(canal(edge, 0) == (DEZ ? 1023 : 255) && canal(edge, 1) == 0, "just left of the rect (col 99) is still red (0x%08x)", edge);
        CHECK(canal(below, 0) == (DEZ ? 1023 : 255), "just below the rect (row 250) is still red (0x%08x)", below);
        CHECK(canal(out, 0) == (DEZ ? 1023 : 255) && canal(out, 2) == 0, "blue area outside the damage was not copied (0x%08x)", out);
        printf("  CPU time of copy() with a 300x200 damage: %.3f ms\n", c1 - c0);
        if (reader) {
            // verify, damage-only: the 12 blocks inside the copied rect must match (the 4 random ones may land on the
            // blue square that was deliberately not copied, so sync the buffers first with a full copy)
            gf = fence(); done = copier->copy(frame, lin, full, gf); close(gf); wait(done);
            auto v = mgpuVerifySamples(reader, frame, lin, dmg, 2);
            eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            CHECK(v.ran && v.pixels > 0 && v.bad == 0, "verify with a 300x200 copied region: %llu pixels, %llu differ %s", (unsigned long long)v.pixels, (unsigned long long)v.bad, v.first.c_str());
            // and it must notice: the frame changes in a rect, nothing is copied
            glEnable(GL_SCISSOR_TEST); glScissor(2000, 500, 400, 400); glClearColor(1.0f, 1.0f, 1.0f, 1.0f); glClear(GL_COLOR_BUFFER_BIT); glDisable(GL_SCISSOR_TEST);
            glFinish();
            CRegion stale; stale.add(2000, 500, 400, 400);
            v = mgpuVerifySamples(reader, frame, lin, stale, 3);
            eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            CHECK(v.ran && v.bad > 0, "verify reports a rect that changed without being copied: %llu of %llu pixels differ, %s", (unsigned long long)v.bad, (unsigned long long)v.pixels, v.first.c_str());
        }

        // steady state: full-frame copies, CPU time of copy() only (the GPU works in the background)
        double cpu = 0; const int N = 30;
        for (int i = 0; i < N; i++) {
            glClearColor(0.0f, 0.0f, (i & 1) ? 1.0f : 0.5f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
            gf      = fence();
            double a = cpuMs();
            done     = copier->copy(frame, lin, full, gf);
            cpu += cpuMs() - a;
            close(gf);
            wait(done);
        }
        printf("  CPU time of copy() for a full %dx%d frame: %.3f ms (average of %d)\n", W, H, cpu / N, N);
        glDeleteFramebuffers(1, &fbo); glDeleteRenderbuffers(1, &rb);
        frame.reset(); // drops the imported-image attachment
        gbm_bo_destroy(bo);
    }
    printf(falhas ? "\n%d CHECK(S) FAILED\n" : "\nall checks passed\n", falhas);
    return falhas ? 1 : 0;
}
