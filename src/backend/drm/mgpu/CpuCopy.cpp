#include "CpuCopy.hpp"
#include "../Renderer.hpp"
#include "Shared.hpp"
#include <hyprutils/os/FileDescriptor.hpp>
#include <cerrno>
#include <format>
#include <poll.h>
#include <string>
#include <unistd.h>

using namespace Aquamarine;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Math;
using namespace Hyprutils::OS;
#define SP CSharedPointer

static GLuint compileShader(GLenum type, const std::string& src) {
    const GLuint shader = glCreateShader(type);
    const char*  ptr    = src.c_str();
    glShaderSource(shader, 1, &ptr, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_FALSE) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint createProgram(const std::string& vert, const std::string& frag) {
    const GLuint v = compileShader(GL_VERTEX_SHADER, vert);
    const GLuint f = v ? compileShader(GL_FRAGMENT_SHADER, frag) : 0;
    if (!v || !f) {
        if (v)
            glDeleteShader(v);
        return 0;
    }
    const GLuint prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    glLinkProgram(prog);
    glDetachShader(prog, v);
    glDetachShader(prog, f);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (ok == GL_FALSE) {
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

static const std::string VERT_SRC_PACK = R"#(#version 300 es
void main() {
    // one triangle over the whole viewport: the scissor picks the rect
    vec2 pos = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
    gl_Position = vec4(pos, 0.0, 1.0);
})#";

static std::string packFragSrc(bool external) {
    return std::string("#version 300 es\n") + (external ? "#extension GL_OES_EGL_image_external_essl3 : require\n" : "") + R"#(
precision highp float;
precision highp int;

uniform highp )#" + (external ? "samplerExternalOES" : "sampler2D") + R"#( tex;
uniform vec2 invSize;
uniform int swapRB;
out vec4 fragColor;

void main() {
    // the image's first row is GL's y = 0 both in tex and in the target: no flip
    vec3  c = texture(tex, gl_FragCoord.xy * invSize).rgb;
    uvec3 v = uvec3(round(clamp(c, 0.0, 1.0) * 1023.0));
    if (swapRB == 1)
        v = v.bgr;
    // the X*2101010 word, split into its 4 bytes (lowest first) so an RGBA8 readback stores it as is
    uint w = v.r | (v.g << 10u) | (v.b << 20u) | (3u << 30u);
    fragColor = vec4(uvec4(w, w >> 8u, w >> 16u, w >> 24u) & 255u) / 255.0;
})#";
}

bool Aquamarine::CMgpuCpuReader::layoutFor(uint32_t drmFormat, eLayout& layout) {
    switch (drmFormat) {
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_ARGB8888: layout = eLayout::BGRA8; return true;
        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_ABGR8888: layout = eLayout::RGBA8; return true;
        case DRM_FORMAT_XBGR2101010:
        case DRM_FORMAT_ABGR2101010: layout = eLayout::XBGR2101010; return true;
        case DRM_FORMAT_XRGB2101010:
        case DRM_FORMAT_ARGB2101010: layout = eLayout::XRGB2101010; return true;
        default: return false;
    }
}

Aquamarine::CMgpuCpuReader::CMgpuCpuReader(SP<CDRMRenderer> renderer_) : renderer(renderer_) {
    ;
}

// Returns an FBO (cached in buf's output attachment, the one upstream's readBuffer() also uses) to read buf from,
// or 0 on failure. Needs a current context.
GLuint Aquamarine::CMgpuCpuReader::readFramebuffer(SP<CDRMRenderer> r, SP<IBuffer> buf) {
    auto att = buf->attachments.get<CDRMRendererBufferOutputAttachment>();
    if (!att || att->renderer != r->self) {
        att = makeShared<CDRMRendererBufferOutputAttachment>(r->self, nullptr, 0, 0);
        buf->attachments.add(att);
    }

    if (!att->eglImage) {
        att->eglImage = r->createEGLImage(buf->dmabuf());
        if (att->eglImage == EGL_NO_IMAGE_KHR) {
            r->backend->log(AQ_LOG_ERROR, std::format("mgpu (readFramebuffer): createEGLImage failed: {}", eglGetError()));
            return 0;
        }

        glGenRenderbuffers(1, &att->rbo);
        glBindRenderbuffer(GL_RENDERBUFFER, att->rbo);
        r->proc.glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, (GLeglImageOES)att->eglImage);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);

        glGenFramebuffers(1, &att->fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, att->fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, att->rbo);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            r->backend->log(AQ_LOG_ERROR, std::format("mgpu (readFramebuffer): glCheckFramebufferStatus failed: {}", glGetError()));
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            buf->attachments.remove(att);
            return 0;
        }
    }

    return att->fbo;
}

// Repacks `region` of the 10 bit buffer `from` into pack's RGBA8 FBO (see CpuCopy.hpp) and returns that FBO, or 0 on
// failure. Needs a current context.
GLuint Aquamarine::CMgpuCpuReader::packFramebuffer(SP<CDRMRenderer> r, SP<IBuffer> from, const CRegion& region, bool swapRB) {
    // `from`'s texture, cached like blit() does
    auto att = from->attachments.get<CDRMRendererBufferInputAttachment>();
    if (!att || att->renderer != r->self) {
        att = makeShared<CDRMRendererBufferInputAttachment>(r->self, r->glTex(from), std::vector<uint8_t>());
        from->attachments.add(att);
    }
    if (!att->tex || !att->tex->image || !att->tex->texid) {
        r->backend->log(AQ_LOG_ERROR, "mgpu (packFramebuffer): no texture for the source buffer");
        return 0;
    }

    const int  I    = att->tex->target == GL_TEXTURE_EXTERNAL_OES ? 1 : 0;
    const auto SIZE = from->dmabuf().size;

    if (!pack.program[I]) {
        pack.program[I] = createProgram(VERT_SRC_PACK, packFragSrc(I == 1));
        if (!pack.program[I]) {
            r->backend->log(AQ_LOG_ERROR, "mgpu (packFramebuffer): 10 bit pack shader failed");
            return 0;
        }
        pack.tex[I]     = glGetUniformLocation(pack.program[I], "tex");
        pack.invSize[I] = glGetUniformLocation(pack.program[I], "invSize");
        pack.swapRB[I]  = glGetUniformLocation(pack.program[I], "swapRB");
    }

    if (!pack.vao)
        glGenVertexArrays(1, &pack.vao);

    if (!pack.fbo || pack.size != SIZE) {
        if (!pack.rbo)
            glGenRenderbuffers(1, &pack.rbo);
        if (!pack.fbo)
            glGenFramebuffers(1, &pack.fbo);

        glBindRenderbuffer(GL_RENDERBUFFER, pack.rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, SIZE.x, SIZE.y);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, pack.fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, pack.rbo);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            r->backend->log(AQ_LOG_ERROR, std::format("mgpu (packFramebuffer): glCheckFramebufferStatus failed: {}", glGetError()));
            pack.size = {};
            return 0;
        }
        pack.size = SIZE;
        r->backend->log(AQ_LOG_DEBUG, std::format("mgpu (packFramebuffer): 10 bit pack target is {}x{}", SIZE.x, SIZE.y));
    }

    glBindFramebuffer(GL_FRAMEBUFFER, pack.fbo);
    glViewport(0, 0, SIZE.x, SIZE.y);
    glDisable(GL_BLEND);
    r->useProgram(pack.program[I]);

    glActiveTexture(GL_TEXTURE0);
    att->tex->bind();
    att->tex->setTexParameter(GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    att->tex->setTexParameter(GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glUniform1i(pack.tex[I], 0);
    glUniform2f(pack.invSize[I], 1.F / SIZE.x, 1.F / SIZE.y);
    glUniform1i(pack.swapRB[I], swapRB ? 1 : 0);
    glBindVertexArray(pack.vao);

    glEnable(GL_SCISSOR_TEST);
    region.forEachRect([&](const pixman_box32_t& rect) {
        if (rect.x2 <= rect.x1 || rect.y2 <= rect.y1)
            return;
        glScissor(rect.x1, rect.y1, rect.x2 - rect.x1, rect.y2 - rect.y1);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    });
    glDisable(GL_SCISSOR_TEST);

    glBindVertexArray(0);
    att->tex->unbind();
    return pack.fbo;
}

int Aquamarine::CMgpuCpuReader::createNativeFenceFD() {
    const auto r = renderer.lock();
    if (!r)
        return -1;
    CEglContextGuard eglContext(*r);
    EGLSyncKHR       sync = r->proc.eglCreateSyncKHR(r->egl.display, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
    if (sync == EGL_NO_SYNC_KHR)
        return -1;
    glFlush(); // otherwise the fd may not be valid yet
    const int fd = r->proc.eglDupNativeFenceFDANDROID(r->egl.display, sync);
    r->proc.eglDestroySyncKHR(r->egl.display, sync);
    return fd == EGL_NO_NATIVE_FENCE_FD_ANDROID ? -1 : fd;
}

void Aquamarine::CMgpuCpuReader::waitForGpuAsleep(SP<CDRMRenderer> r) {
    EGLSyncKHR sync = r->proc.eglCreateSyncKHR(r->egl.display, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
    if (sync == EGL_NO_SYNC_KHR) {
        TRACE(r->backend->log(AQ_LOG_TRACE, "mgpu (waitForGpuAsleep): failed to create a native fence"));
        return;
    }

    // we need to flush otherwise we might not get a valid fd
    glFlush();

    const int fd = r->proc.eglDupNativeFenceFDANDROID(r->egl.display, sync);
    if (r->proc.eglDestroySyncKHR(r->egl.display, sync) != EGL_TRUE)
        TRACE(r->backend->log(AQ_LOG_TRACE, "mgpu (waitForGpuAsleep): failed to destroy sync"));
    if (fd == EGL_NO_NATIVE_FENCE_FD_ANDROID) {
        TRACE(r->backend->log(AQ_LOG_TRACE, "mgpu (waitForGpuAsleep): failed to dup the fence fd"));
        return;
    }

    pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
    while (poll(&pfd, 1, 1000) < 0 && errno == EINTR) {
        ;
    }
    close(fd);
}

bool Aquamarine::CMgpuCpuReader::readRegionInto(SP<IBuffer> from, uint8_t* dst, uint32_t dstStride, eLayout layout, const CRegion& region, int waitFD) {
    const auto r = renderer.lock();
    if (!r || !from || !dst || dstStride % 4 != 0)
        return false;

    CEglContextGuard eglContext(*r);

    // the client may still be rendering into `from`
    if (waitFD >= 0 && !CFileDescriptor::isReadable(waitFD))
        r->waitOnSync(waitFD);

    const auto SIZE    = from->dmabuf().size;
    CRegion    clipped = region.copy(); // intersect() returns a reference: keep the copy alive
    clipped.intersect(0, 0, SIZE.x, SIZE.y);

    const auto FBO = isPacked(layout) ? packFramebuffer(r, from, clipped, layout == eLayout::XRGB2101010) : readFramebuffer(r, from);
    if (!FBO)
        return false;

    // packed pixels are already the final bytes in R, G, B, A order
    const GLenum GLFORMAT = layout == eLayout::BGRA8 ? GL_BGRA_EXT : GL_RGBA;

    // the image's first row is GL's y = 0, so a rect maps 1:1 onto the destination memory
    glBindFramebuffer(GL_FRAMEBUFFER, FBO);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, dstStride / 4);

    static const bool SPIN = envEnabled("AQ_MGPU_CPU_COPY_SPIN"); // debug: let glReadPixels do the waiting
    if (!SPIN)
        waitForGpuAsleep(r);

    bool ok = true;
    clipped.forEachRect([&](const pixman_box32_t& rect) {
        const int W = rect.x2 - rect.x1, H = rect.y2 - rect.y1;
        if (W <= 0 || H <= 0)
            return;
        glReadPixels(rect.x1, rect.y1, W, H, GLFORMAT, GL_UNSIGNED_BYTE, dst + (size_t)rect.y1 * dstStride + (size_t)rect.x1 * 4);
        if (const auto ERR = glGetError(); ERR != GL_NO_ERROR) {
            r->backend->log(AQ_LOG_ERROR, std::format("mgpu (readRegionInto): glReadPixels failed: 0x{:x}", ERR));
            ok = false;
        }
    });

    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return ok;
}
