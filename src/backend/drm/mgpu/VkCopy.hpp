#pragma once

// AQ_MGPU_GPU_COPY: frames rendered by the primary GPU reach an output on a secondary GPU without the CPU touching
// pixels. The primary's Vulkan driver allocates LINEAR buffers in system RAM and exports them as dma-bufs; the
// secondary imports them as KMS framebuffers and scans them out. Each frame, the primary's copy engine copies the
// damaged rectangles of its native (e.g. NVIDIA block-linear) frame into one of them, waiting on the frame's render
// fence on the GPU, and hands back a sync_file that KMS waits on.
//
// Written for an NVIDIA primary (which cannot render into LINEAR, so the regular multi-GPU path cannot work, see
// AQ_MGPU_CPU_COPY) and an Intel secondary; nothing in it is NVIDIA-specific.

#include <aquamarine/allocator/Allocator.hpp>
#include <aquamarine/buffer/Buffer.hpp>
#include <aquamarine/misc/Attachment.hpp>
#include <hyprutils/math/Region.hpp>
#include <hyprutils/math/Vector2D.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/memory/WeakPtr.hpp>
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>

namespace Aquamarine {
    class CBackend;
    class CMgpuVkCopy;

    // AQ_MGPU_GPU_COPY_LOG=<file>: append one line per event to that file, flushed immediately, so a session that has
    // to be killed (no picture) still leaves evidence; the compositor's own log is buffered and loses its tail.
    void mgpuGpuCopyTrace(const std::string& msg);

    // A LINEAR buffer in system RAM, owned by the primary's Vulkan device, exported as a dma-buf.
    class CVkLinearBuffer : public IBuffer {
      public:
        virtual ~CVkLinearBuffer();
        virtual eBufferCapability caps();
        virtual eBufferType       type();
        virtual void              update(const Hyprutils::Math::CRegion& damage);
        virtual bool              isSynchronous();
        virtual bool              good();
        virtual SDMABUFAttrs      dmabuf();

        VkBuffer                  buffer = VK_NULL_HANDLE;
        VkDeviceMemory            memory = VK_NULL_HANDLE;
        uint32_t                  stride = 0;

        // CPU view of the pixels, for AQ_MGPU_GPU_COPY_VERIFY only. The memory is not CPU-cached (measured: ~85 MiB/s,
        // half a second for a 5120x2160 frame), so read small samples, never whole frames. nullptr on failure.
        const uint8_t*            map();

      private:
        CVkLinearBuffer() = default;

        Hyprutils::Memory::CWeakPointer<CMgpuVkCopy> copier;
        SDMABUFAttrs                                 attrs{.success = false};
        void*                                        mapped = nullptr;

        friend class CMgpuVkCopy;
    };

    // Feeds a CSwapchain with CVkLinearBuffers.
    class CVkLinearAllocator : public IAllocator {
      public:
        static Hyprutils::Memory::CSharedPointer<CVkLinearAllocator> create(Hyprutils::Memory::CSharedPointer<CMgpuVkCopy> copier,
                                                                            Hyprutils::Memory::CWeakPointer<CBackend>    backend);
        virtual Hyprutils::Memory::CSharedPointer<IBuffer>  acquire(const SAllocatorBufferParams& params, Hyprutils::Memory::CSharedPointer<CSwapchain> swapchain);
        virtual Hyprutils::Memory::CSharedPointer<CBackend> getBackend();
        virtual int                                         drmFD();
        virtual eAllocatorType                              type();

      private:
        Hyprutils::Memory::CSharedPointer<CMgpuVkCopy> copier;
        Hyprutils::Memory::CWeakPointer<CBackend>      backend;
    };

    // The primary's frame imported into Vulkan, cached on the frame's buffer; destroyed with it.
    class CVkImportedImageAttachment : public IAttachment {
      public:
        virtual ~CVkImportedImageAttachment();

        VkImage                                      image  = VK_NULL_HANDLE;
        VkDeviceMemory                               memory = VK_NULL_HANDLE;
        uint64_t                                     modifier = 0;
        Hyprutils::Math::Vector2D                    size;
        bool                                         acquired = false; // first use needs the initial layout transition
        Hyprutils::Memory::CWeakPointer<CMgpuVkCopy> copier;
    };

    class CMgpuVkCopy {
      public:
        // primaryDrmFD: any fd of the primary GPU (card or render node); the Vulkan device is matched by DRM node
        static Hyprutils::Memory::CSharedPointer<CMgpuVkCopy> create(int primaryDrmFD, Hyprutils::Memory::CWeakPointer<CBackend> backend);
        ~CMgpuVkCopy();

        Hyprutils::Memory::CSharedPointer<CVkLinearBuffer> allocate(const Hyprutils::Math::Vector2D& size, uint32_t drmFormat);

        // Copies `region` (buffer pixels, row 0 first) of `from` into `to`. waitFD: sync_file the copy waits for on the
        // GPU (dup'ed here, the caller keeps its own), or -1. Returns a sync_file signalled when the copy is complete,
        // owned by the caller, or -1 on failure.
        int copy(Hyprutils::Memory::CSharedPointer<IBuffer> from, Hyprutils::Memory::CSharedPointer<CVkLinearBuffer> to, const Hyprutils::Math::CRegion& region,
                 int waitFD);

        // VkFormat a DRM fourcc is copied as (the copy is raw: any 4-byte format of the same layout works)
        static VkFormat vkFormatFor(uint32_t drmFormat);

        VkDevice                                     device = VK_NULL_HANDLE;
        Hyprutils::Memory::CWeakPointer<CMgpuVkCopy> self;

      private:
        CMgpuVkCopy() = default;
        bool                                                         init(int primaryDrmFD);
        uint32_t                                                     memoryType(uint32_t bits, VkMemoryPropertyFlags wanted, VkMemoryPropertyFlags avoid = 0);
        Hyprutils::Memory::CSharedPointer<CVkImportedImageAttachment> importImage(Hyprutils::Memory::CSharedPointer<IBuffer> from);
        void                                                         log(int level, const std::string& msg);

        Hyprutils::Memory::CWeakPointer<CBackend> backend;
        VkInstance                                instance = VK_NULL_HANDLE;
        VkPhysicalDevice                          physical = VK_NULL_HANDLE;
        VkPhysicalDeviceMemoryProperties          memProps{};
        VkQueue                                   queue       = VK_NULL_HANDLE;
        uint32_t                                  queueFamily = 0;
        uint32_t                                  otherFamily = VK_QUEUE_FAMILY_EXTERNAL; // FOREIGN_EXT when available: the other side is another device
        VkCommandPool                             pool        = VK_NULL_HANDLE;

        // a small ring: a slot is reused only after its fence signalled (frames are paced by page flips, so this never waits)
        static constexpr size_t SLOTS = 4;
        struct SSlot {
            VkCommandBuffer cmd       = VK_NULL_HANDLE;
            VkFence         fence     = VK_NULL_HANDLE;
            VkSemaphore     waitSem   = VK_NULL_HANDLE; // imported from the render fence (temporary import)
            VkSemaphore     doneSem   = VK_NULL_HANDLE; // exported as a sync_file
            bool            submitted = false;
        };
        std::array<SSlot, SLOTS> slots;
        size_t                   nextSlot = 0;

        struct {
            PFN_vkGetMemoryFdKHR           getMemoryFd           = nullptr;
            PFN_vkGetMemoryFdPropertiesKHR getMemoryFdProperties = nullptr;
            PFN_vkImportSemaphoreFdKHR     importSemaphoreFd     = nullptr;
            PFN_vkGetSemaphoreFdKHR        getSemaphoreFd        = nullptr;
        } proc;

        friend class CVkLinearBuffer;
        friend class CVkImportedImageAttachment;
    };
};
