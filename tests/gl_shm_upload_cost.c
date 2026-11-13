// What does it cost the compositor when a client renders in software and commits wl_shm buffers?
//
// A compositor uploads each committed shm buffer into a texture with glTexSubImage2D on its main thread. This
// measures the CPU time of that upload on the NVIDIA driver for a window that fills a 5120x2160 output at scale
// 5/3 (5117x2157 pixels, ~42 MiB), for the full surface and for a small damaged rect, and for the two ways a
// compositor can describe BGRA pixels (GL_BGRA_EXT, or GL_RGBA plus a swizzle). Run it under `perf record` to
// get the driver addresses of the copy, to compare against a profile of the compositor.
//
// Render node only (surfaceless context on renderD129); it does not touch the screen.
//
// build: gcc -O2 -o gl_shm_upload_cost gl_shm_upload_cost.c $(pkg-config --cflags --libs gbm egl glesv2)
// run:   ./gl_shm_upload_cost [seconds per case, default 3] [x y w h: only this rect]
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define W 5117
#define H 2157
#define NVIDIA_RENDER "/dev/dri/renderD129"

#define OK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf(" (line %d, egl 0x%x, gl 0x%x)\n", __LINE__, eglGetError(), glGetError()); exit(1); } } while (0)
static double ms_cpu(void) { struct timespec t; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static double ms_clock(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static void run(const char *name, GLenum format, int x, int y, int w, int h, const uint8_t *pixels, double seconds) {
    // the way a compositor uploads a damaged rect out of a larger client buffer
    glPixelStorei(GL_UNPACK_ROW_LENGTH, W);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, x);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, y);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, format, GL_UNSIGNED_BYTE, pixels); // warm up
    glFinish();
    OK(glGetError() == GL_NO_ERROR, "%s: upload", name);

    int    n = 0;
    double c0 = ms_cpu(), t0 = ms_clock();
    while (ms_clock() - t0 < seconds * 1000.0) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, format, GL_UNSIGNED_BYTE, pixels);
        glFlush(); // a compositor does not wait for the GPU here
        n++;
    }
    glFinish();
    double cpu = (ms_cpu() - c0) / n, wall = (ms_clock() - t0) / n;
    printf("  %-34s %4dx%-4d %6.1f MiB: %6.3f ms CPU per upload (%.3f ms wall) -> %4.1f %% of a core at 48 Hz\n", name, w, h, w * (double)h * 4 / 1048576.0, cpu,
           wall, cpu * 48 / 10.0);
}

int main(int argc, char **argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 3.0;
    int    fd      = open(NVIDIA_RENDER, O_RDWR | O_CLOEXEC);
    OK(fd >= 0, "open %s", NVIDIA_RENDER);
    struct gbm_device              *gbm = gbm_create_device(fd);
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay                      dpy = gpd(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    EGLint                          a, b;
    OK(eglInitialize(dpy, &a, &b), "eglInitialize");
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint     ca[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
    EGLContext ctx  = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
    OK(ctx, "context");
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
    printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));

    uint8_t *pixels = malloc((size_t)W * H * 4);
    for (size_t i = 0; i < (size_t)W * H * 4; i++)
        pixels[i] = (uint8_t)(i * 2654435761u >> 24);

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    // GL_RGBA8 storage, uploaded as GL_RGBA (what a compositor using a swizzle for BGRA does)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    OK(glGetError() == GL_NO_ERROR, "storage RGBA8");
    if (argc > 5) { // one custom rect: ./gl_shm_upload_cost <seconds> <x> <y> <w> <h>, to profile a single case
        run("custom rect", GL_RGBA, atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), pixels, seconds);
        return 0;
    }
    printf("texture RGBA8, pixels given as GL_RGBA:\n");
    run("full surface", GL_RGBA, 0, 0, W, H, pixels, seconds);
    run("half surface (tiled next to another)", GL_RGBA, 0, 0, W / 2, H, pixels, seconds);
    run("damage 600x40 (a line of text)", GL_RGBA, 300, 900, 600, 40, pixels, seconds);

    // GL_BGRA_EXT storage and upload (EXT_texture_format_BGRA8888)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT, W, H, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, NULL);
    if (glGetError() == GL_NO_ERROR) {
        printf("texture BGRA, pixels given as GL_BGRA_EXT:\n");
        run("full surface", GL_BGRA_EXT, 0, 0, W, H, pixels, seconds);
    } else
        printf("GL_BGRA_EXT storage not accepted\n");
    return 0;
}
