#pragma once

// AQ_MGPU_CPU_COPY's engine: the primary GPU reads a region of its own frame straight into CPU memory (a linear
// dumb buffer of the secondary GPU, which only scans it out). Also used to read samples of a frame back for
// AQ_MGPU_GPU_COPY_VERIFY. It works inside the primary's CDRMRenderer context (it is a friend of that class) but
// keeps all of its own GL state here, so upstream's Renderer.cpp stays untouched.

#include <aquamarine/buffer/Buffer.hpp>
#include <hyprutils/math/Region.hpp>
#include <hyprutils/math/Vector2D.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/memory/WeakPtr.hpp>
#include <GLES3/gl3.h>
#include <cstdint>

namespace Aquamarine {
    class CDRMRenderer;

    class CMgpuCpuReader {
      public:
        // Memory layout readRegionInto writes, 4 bytes per pixel
        enum class eLayout : uint8_t {
            BGRA8,       // [AX]RGB8888
            RGBA8,       // [AX]BGR8888
            XBGR2101010, // [AX]BGR2101010 (alpha bits set)
            XRGB2101010, // [AX]RGB2101010 (alpha bits set)
        };

        // false if the format has no layout here
        static bool layoutFor(uint32_t drmFormat, eLayout& layout);
        static bool isPacked(eLayout layout) {
            return layout == eLayout::XBGR2101010 || layout == eLayout::XRGB2101010;
        }

        explicit CMgpuCpuReader(Hyprutils::Memory::CSharedPointer<CDRMRenderer> renderer);

        bool usesRenderer(const Hyprutils::Memory::CSharedPointer<CDRMRenderer>& r) const {
            return renderer.lock() == r;
        }

        // Reads only `region` of `from` (a buffer the renderer can import) into CPU memory at `dst`, laid out as
        // `layout` with `dstStride` bytes per row. Synchronous: when it returns, the pixels are in `dst`.
        bool readRegionInto(Hyprutils::Memory::CSharedPointer<IBuffer> from, uint8_t* dst, uint32_t dstStride, eLayout layout, const Hyprutils::Math::CRegion& region,
                            int waitFD = -1);

        // A sync_file signalled when the work submitted so far in the renderer's context is done, or -1
        int createNativeFenceFD();

      private:
        GLuint readFramebuffer(Hyprutils::Memory::CSharedPointer<CDRMRenderer> r, Hyprutils::Memory::CSharedPointer<IBuffer> buf);
        GLuint packFramebuffer(Hyprutils::Memory::CSharedPointer<CDRMRenderer> r, Hyprutils::Memory::CSharedPointer<IBuffer> from, const Hyprutils::Math::CRegion& region,
                               bool swapRB);

        // Blocks until the GPU has finished everything submitted on the context, asleep in the kernel on a native
        // fence. The NVIDIA driver instead busy-waits inside glReadPixels: with video on a 5K output that wait
        // burned most of the compositor's ~60% CPU (measured 2026-10-31). If no fence can be made it returns at
        // once and glReadPixels waits as before.
        void waitForGpuAsleep(Hyprutils::Memory::CSharedPointer<CDRMRenderer> r);

        Hyprutils::Memory::CWeakPointer<CDRMRenderer> renderer;

        // 10 bit: the driver can't read 2101010 into CPU memory quickly (NVIDIA converts in software, ~330 ms for
        // 5120x2160), so a shader first repacks each pixel's 32-bit word into an RGBA8 renderbuffer, which reads
        // back on the fast 8 bit path. Created on first use.
        struct {
            GLuint                    program[2] = {0, 0}; // GL_TEXTURE_2D, GL_TEXTURE_EXTERNAL_OES
            GLint                     tex[2] = {-1, -1}, invSize[2] = {-1, -1}, swapRB[2] = {-1, -1};
            GLuint                    vao = 0, fbo = 0, rbo = 0;
            Hyprutils::Math::Vector2D size;
        } pack;
    };
}
