#pragma once

#include <aquamarine/backend/DRM.hpp>
#include "FormatUtils.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>
#define __gl2_h_ // define guard for gl2ext.h
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <optional>
#include <tuple>
#include <vector>
#include <span>

namespace Aquamarine {

    class CGBMAllocator;

    class CGLTex {
      public:
        CGLTex() = default;
        void     bind();
        void     unbind();
        void     setTexParameter(GLenum pname, GLint param);
        EGLImage image  = nullptr;
        GLuint   texid  = 0;
        GLuint   target = GL_TEXTURE_2D;

      private:
        enum eTextureParam : uint8_t {
            TEXTURE_PAR_WRAP_S = 0,
            TEXTURE_PAR_WRAP_T,
            TEXTURE_PAR_MAG_FILTER,
            TEXTURE_PAR_MIN_FILTER,
            TEXTURE_PAR_LAST,
        };

        inline constexpr std::optional<size_t>             getCacheStateIndex(GLenum pname);
        std::array<std::optional<GLint>, TEXTURE_PAR_LAST> m_cachedStates;
    };

    class CDRMRendererBufferInputAttachment : public IAttachment {
      public:
        CDRMRendererBufferInputAttachment(Hyprutils::Memory::CWeakPointer<CDRMRenderer> renderer_, CGLTex&& tex, std::vector<uint8_t> intermediateBuf_);
        virtual ~CDRMRendererBufferInputAttachment();

        Hyprutils::Memory::CUniquePointer<CGLTex>     tex;
        std::vector<uint8_t>                          intermediateBuf;

        Hyprutils::Memory::CWeakPointer<CDRMRenderer> renderer;
    };

    class CDRMRendererBufferOutputAttachment : public IAttachment {
      public:
        CDRMRendererBufferOutputAttachment(Hyprutils::Memory::CWeakPointer<CDRMRenderer> renderer_, EGLImageKHR image, GLuint fbo_, GLuint rbo_);
        virtual ~CDRMRendererBufferOutputAttachment();

        EGLImageKHR                                   eglImage = nullptr;
        GLuint                                        fbo = 0, rbo = 0;

        Hyprutils::Memory::CWeakPointer<CDRMRenderer> renderer;
    };

    // marks a destination this renderer can't draw into, blit() goes straight to the CPU copy
    class CDRMRendererBufferCPUOnlyAttachment : public IAttachment {
      public:
        CDRMRendererBufferCPUOnlyAttachment(Hyprutils::Memory::CWeakPointer<CDRMRenderer> renderer_) : renderer(renderer_) {}

        Hyprutils::Memory::CWeakPointer<CDRMRenderer> renderer;
    };

    // CEglContextGuard is a RAII abstraction for the EGL context.
    // On initialization, it sets the EGL context to the renderer's display,
    // and on destruction, it restores the previous EGL context.
    class CEglContextGuard {
      public:
        CEglContextGuard(const CDRMRenderer& renderer_);
        ~CEglContextGuard();

        // No copy or move constructors
        CEglContextGuard(const CEglContextGuard&)            = delete;
        CEglContextGuard& operator=(const CEglContextGuard&) = delete;
        CEglContextGuard(CEglContextGuard&&)                 = delete;
        CEglContextGuard& operator=(CEglContextGuard&&)      = delete;

      private:
        const CDRMRenderer& renderer;
        struct {
            EGLDisplay display = nullptr;
            EGLContext context = nullptr;
            EGLSurface draw = nullptr, read = nullptr;
        } savedEGLState;
    };

    class CDRMRenderer {
      public:
        ~CDRMRenderer();

        static Hyprutils::Memory::CSharedPointer<CDRMRenderer> attempt(Hyprutils::Memory::CSharedPointer<CBackend> backend_, int drmFD);
        static Hyprutils::Memory::CSharedPointer<CDRMRenderer> attempt(Hyprutils::Memory::CSharedPointer<CBackend>      backend_,
                                                                       Hyprutils::Memory::CSharedPointer<CGBMAllocator> allocator_);

        int                                                    drmFD = -1;

        struct SBlitResult {
            bool               success = false;
            std::optional<int> syncFD;
        };

        SBlitResult blit(Hyprutils::Memory::CSharedPointer<IBuffer> from, Hyprutils::Memory::CSharedPointer<IBuffer> to,
                         Hyprutils::Memory::CSharedPointer<CDRMRenderer> primaryRenderer, int waitFD = -1);

        // Memory layout readRegionInto writes, 4 bytes per pixel. Pick the one matching `from`'s format.
        enum class eCpuCopyLayout : uint8_t {
            BGRA8,       // [AX]RGB8888
            RGBA8,       // [AX]BGR8888
            XBGR2101010, // [AX]BGR2101010 (alpha bits set)
            XRGB2101010, // [AX]RGB2101010 (alpha bits set)
        };

        // Reads only `region` of `from` (a buffer this renderer can import) straight into CPU memory at `dst`,
        // laid out as `layout` with `dstStride` bytes per row. Synchronous: when it returns, the pixels are in `dst`.
        // Used by AQ_MGPU_CPU_COPY so the secondary GPU never reads (or renders from) the primary's buffers.
        bool readRegionInto(Hyprutils::Memory::CSharedPointer<IBuffer> from, uint8_t* dst, uint32_t dstStride, eCpuCopyLayout layout, const Hyprutils::Math::CRegion& region,
                            int waitFD = -1);
        // can't be a SP<> because we call it from buf's ctor...
        void clearBuffer(IBuffer* buf);

        struct SShader {
            ~SShader();
            void   createVao();

            GLuint program = 0;
            GLint  proj = -1, tex = -1, posAttrib = -1, texAttrib = -1;
            GLuint shaderVao = 0, shaderVboPos = 0, shaderVboUv = 0;
        } shader, shaderExt;

        struct {
            PFNEGLGETPLATFORMDISPLAYEXTPROC               eglGetPlatformDisplayEXT               = nullptr;
            PFNEGLCREATEIMAGEKHRPROC                      eglCreateImageKHR                      = nullptr;
            PFNEGLDESTROYIMAGEKHRPROC                     eglDestroyImageKHR                     = nullptr;
            PFNGLEGLIMAGETARGETTEXTURE2DOESPROC           glEGLImageTargetTexture2DOES           = nullptr;
            PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC glEGLImageTargetRenderbufferStorageOES = nullptr;
            PFNEGLQUERYDMABUFFORMATSEXTPROC               eglQueryDmaBufFormatsEXT               = nullptr;
            PFNEGLQUERYDMABUFMODIFIERSEXTPROC             eglQueryDmaBufModifiersEXT             = nullptr;
            PFNEGLDESTROYSYNCKHRPROC                      eglDestroySyncKHR                      = nullptr;
            PFNEGLWAITSYNCKHRPROC                         eglWaitSyncKHR                         = nullptr;
            PFNEGLCREATESYNCKHRPROC                       eglCreateSyncKHR                       = nullptr;
            PFNEGLDUPNATIVEFENCEFDANDROIDPROC             eglDupNativeFenceFDANDROID             = nullptr;
            PFNEGLDEBUGMESSAGECONTROLKHRPROC              eglDebugMessageControlKHR              = nullptr;
            PFNEGLQUERYDEVICESEXTPROC                     eglQueryDevicesEXT                     = nullptr;
            PFNEGLQUERYDEVICESTRINGEXTPROC                eglQueryDeviceStringEXT                = nullptr;
            PFNGLREADNPIXELSEXTPROC                       glReadnPixelsEXT                       = nullptr;
        } proc;

        struct {
            bool EXT_read_format_bgra               = false;
            bool EXT_texture_format_BGRA8888        = false;
            bool EXT_platform_device                = false;
            bool KHR_platform_gbm                   = false;
            bool EXT_image_dma_buf_import           = false;
            bool EXT_image_dma_buf_import_modifiers = false;
            bool KHR_context_flush_control          = false;
            bool KHR_display_reference              = false;
            bool IMG_context_priority               = false;
            bool EXT_create_context_robustness      = false;
        } exts;

        struct {
            EGLDisplay display        = nullptr;
            EGLContext context        = nullptr;
            EGLSync    lastBlitSync   = nullptr;
            int        lastBlitSyncFD = -1;
        } egl;

        CGLTex                                        glTex(Hyprutils::Memory::CSharedPointer<IBuffer> buf);
        bool readBuffer(Hyprutils::Memory::CSharedPointer<IBuffer> buf, std::span<uint8_t> out, GLenum format = GL_RGBA, uint32_t stride = 0);

        Hyprutils::Memory::CWeakPointer<CDRMRenderer> self;
        std::vector<SGLFormat>                        formats;

      private:
        CDRMRenderer() = default;

        EGLImageKHR                                           createEGLImage(const SDMABUFAttrs& attrs);
        GLuint                                                readFramebuffer(Hyprutils::Memory::CSharedPointer<IBuffer> buf);
        GLuint                                                packFramebuffer(Hyprutils::Memory::CSharedPointer<IBuffer> from, const Hyprutils::Math::CRegion& region, bool swapRB);

        // readRegionInto for 10 bit: the driver can't read 2101010 into CPU memory quickly (NVIDIA converts in
        // software, ~330 ms for 5120x2160), so a shader first repacks each pixel's 32-bit word into an RGBA8
        // renderbuffer, which reads back on the fast 8 bit path. Created on first use.
        struct {
            GLuint                    program[2] = {0, 0}; // GL_TEXTURE_2D, GL_TEXTURE_EXTERNAL_OES
            GLint                     tex[2] = {-1, -1}, invSize[2] = {-1, -1}, swapRB[2] = {-1, -1};
            GLuint                    vao = 0, fbo = 0, rbo = 0;
            Hyprutils::Math::Vector2D size;
        } cpuCopyPack;
        bool                                                  verifyDestinationDMABUF(const SDMABUFAttrs& attrs);
        void                                                  waitOnSync(int fd);
        int                                                   recreateBlitSync();

        void                                                  loadEGLAPI();
        EGLDeviceEXT                                          eglDeviceFromDRMFD(int drmFD);
        void                                                  initContext();
        void                                                  initResources();
        bool                                                  initDRMFormats();
        std::optional<std::vector<std::pair<uint64_t, bool>>> getModsForFormat(EGLint format);
        bool                                                  hasModifiers = false;
        void                                                  useProgram(GLuint prog);
        GLuint                                                m_currentProgram = 0;

        Hyprutils::Memory::CWeakPointer<CBackend>             backend;

        friend class CEglContextGuard;
        friend class CDRMRendererBufferInputAttachment;
        friend class CDRMRendererBufferOutputAttachment;
    };
};
