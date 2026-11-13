#include "VkCopy.hpp"
#include <aquamarine/backend/Backend.hpp>
#include <aquamarine/allocator/Swapchain.hpp>
#include <drm_fourcc.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <format>
#include <optional>
#include <string>
#include <vector>

using namespace Aquamarine;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Math;
#define SP CSharedPointer
#define WP CWeakPointer

void Aquamarine::mgpuGpuCopyTrace(const std::string& msg) {
    static const char* PATH = getenv("AQ_MGPU_GPU_COPY_LOG");
    if (!PATH || !*PATH)
        return;
    if (FILE* f = fopen(PATH, "a")) {
        timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        fprintf(f, "[%ld.%03ld] %s\n", (long)t.tv_sec, t.tv_nsec / 1000000, msg.c_str());
        fflush(f);
        fclose(f);
    }
}

namespace {
    // Sets or clears environment variables for its lifetime and restores them after.
    // The NVIDIA Vulkan driver connects to the X server in $DISPLAY while enumerating devices. Inside the compositor
    // that X server is the compositor's own (lazy) Xwayland, which cannot answer while the compositor's thread is
    // blocked in that very call: a deadlock with no picture (seen 2026-11-13). Implicit layers (device selection,
    // overlays) have no business inside a compositor either.
    class CEnvGuard {
      public:
        void set(const char* name, const char* value) {
            const char* OLD = getenv(name);
            saved.push_back({name, OLD ? std::optional<std::string>(OLD) : std::nullopt});
            if (value)
                setenv(name, value, 1);
            else
                unsetenv(name);
        }
        ~CEnvGuard() {
            for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
                if (it->second)
                    setenv(it->first.c_str(), it->second->c_str(), 1);
                else
                    unsetenv(it->first.c_str());
            }
        }

      private:
        std::vector<std::pair<std::string, std::optional<std::string>>> saved;
    };
}

// ---------------- CVkLinearBuffer ----------------

Aquamarine::CVkLinearBuffer::~CVkLinearBuffer() {
    if (attrs.fds[0] >= 0)
        close(attrs.fds[0]);
    if (auto c = copier.lock(); c && c->device) {
        // a copy into this buffer may still be in flight when a swapchain is reconfigured: rare, so just wait
        vkDeviceWaitIdle(c->device);
        if (buffer)
            vkDestroyBuffer(c->device, buffer, nullptr);
        if (memory)
            vkFreeMemory(c->device, memory, nullptr);
    }
}

eBufferCapability Aquamarine::CVkLinearBuffer::caps() {
    return BUFFER_CAPABILITY_NONE;
}

eBufferType Aquamarine::CVkLinearBuffer::type() {
    return BUFFER_TYPE_DMABUF;
}

void Aquamarine::CVkLinearBuffer::update(const CRegion& damage) {
    ;
}

bool Aquamarine::CVkLinearBuffer::isSynchronous() {
    return false;
}

bool Aquamarine::CVkLinearBuffer::good() {
    return attrs.success;
}

const uint8_t* Aquamarine::CVkLinearBuffer::map() {
    if (!mapped) {
        const auto c = copier.lock();
        if (!c || !c->device || !memory || vkMapMemory(c->device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
            mapped = nullptr;
    }
    return (const uint8_t*)mapped; // unmapped by vkFreeMemory
}

SDMABUFAttrs Aquamarine::CVkLinearBuffer::dmabuf() {
    return attrs;
}

// ---------------- CVkLinearAllocator ----------------

SP<CVkLinearAllocator> Aquamarine::CVkLinearAllocator::create(SP<CMgpuVkCopy> copier, WP<CBackend> backend) {
    if (!copier)
        return nullptr;
    auto a     = SP<CVkLinearAllocator>(new CVkLinearAllocator());
    a->copier  = copier;
    a->backend = backend;
    return a;
}

SP<IBuffer> Aquamarine::CVkLinearAllocator::acquire(const SAllocatorBufferParams& params, SP<CSwapchain> swapchain) {
    auto buf = copier->allocate(params.size, params.format);
    if (!buf || !buf->good())
        return nullptr;
    return buf;
}

SP<CBackend> Aquamarine::CVkLinearAllocator::getBackend() {
    return backend.lock();
}

int Aquamarine::CVkLinearAllocator::drmFD() {
    return -1; // the buffers belong to a Vulkan device, not to a DRM fd
}

eAllocatorType Aquamarine::CVkLinearAllocator::type() {
    return AQ_ALLOCATOR_TYPE_DRM_DUMB; // closest existing kind: linear, scanout-only, never rendered into
}

// ---------------- CVkImportedImageAttachment ----------------

Aquamarine::CVkImportedImageAttachment::~CVkImportedImageAttachment() {
    auto c = copier.lock();
    if (!c || !c->device)
        return;
    vkDeviceWaitIdle(c->device);
    if (image)
        vkDestroyImage(c->device, image, nullptr);
    if (memory)
        vkFreeMemory(c->device, memory, nullptr);
}

// ---------------- CMgpuVkCopy ----------------

void Aquamarine::CMgpuVkCopy::log(int level, const std::string& msg) {
    mgpuGpuCopyTrace(msg);
    if (auto b = backend.lock())
        b->log((eBackendLogLevel)level, "drm: AQ_MGPU_GPU_COPY: " + msg);
}

VkFormat Aquamarine::CMgpuVkCopy::vkFormatFor(uint32_t drmFormat) {
    switch (drmFormat) {
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_ARGB8888: return VK_FORMAT_B8G8R8A8_UNORM;
        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_ABGR8888: return VK_FORMAT_R8G8B8A8_UNORM;
        case DRM_FORMAT_XBGR2101010:
        case DRM_FORMAT_ABGR2101010: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case DRM_FORMAT_XRGB2101010:
        case DRM_FORMAT_ARGB2101010: return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
        default: return VK_FORMAT_UNDEFINED;
    }
}

SP<CMgpuVkCopy> Aquamarine::CMgpuVkCopy::create(int primaryDrmFD, WP<CBackend> backend) {
    auto c     = SP<CMgpuVkCopy>(new CMgpuVkCopy());
    c->self    = c;
    c->backend = backend;
    if (!c->init(primaryDrmFD))
        return nullptr;
    return c;
}

static bool hasExtension(const std::vector<VkExtensionProperties>& exts, const char* name) {
    for (const auto& e : exts)
        if (!strcmp(e.extensionName, name))
            return true;
    return false;
}

bool Aquamarine::CMgpuVkCopy::init(int primaryDrmFD) {
    struct stat st;
    if (fstat(primaryDrmFD, &st) != 0) {
        log(AQ_LOG_ERROR, "fstat on the primary GPU fd failed");
        return false;
    }

    // everything up to the device creation runs without $DISPLAY / $WAYLAND_DISPLAY and without implicit layers
    CEnvGuard env;
    env.set("DISPLAY", nullptr);
    env.set("WAYLAND_DISPLAY", nullptr);
    env.set("VK_LOADER_LAYERS_DISABLE", "~implicit~");
    log(AQ_LOG_DEBUG, "creating the Vulkan instance (DISPLAY, WAYLAND_DISPLAY and implicit layers suppressed)");

    VkApplicationInfo    app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "aquamarine", .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        log(AQ_LOG_ERROR, "vkCreateInstance failed");
        return false;
    }

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devices(n);
    vkEnumeratePhysicalDevices(instance, &n, devices.data());

    const char*              REQUIRED[] = {"VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf", "VK_EXT_image_drm_format_modifier", "VK_KHR_external_semaphore_fd",
                                           "VK_EXT_physical_device_drm"};
    bool                     foreign    = false;
    std::vector<const char*> enable;
    for (auto pd : devices) {
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> exts(ne);
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, exts.data());
        bool all = true;
        for (auto r : REQUIRED)
            all = all && hasExtension(exts, r);
        if (!all)
            continue;

        VkPhysicalDeviceDrmPropertiesEXT drm   = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2      props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drm};
        vkGetPhysicalDeviceProperties2(pd, &props);
        const bool MATCH = (drm.hasPrimary && makedev(drm.primaryMajor, drm.primaryMinor) == st.st_rdev) || (drm.hasRender && makedev(drm.renderMajor, drm.renderMinor) == st.st_rdev);
        if (!MATCH)
            continue;

        physical = pd;
        foreign  = hasExtension(exts, "VK_EXT_queue_family_foreign");
        enable.assign(std::begin(REQUIRED), std::end(REQUIRED));
        if (foreign) {
            enable.push_back("VK_EXT_queue_family_foreign");
            otherFamily = VK_QUEUE_FAMILY_FOREIGN_EXT;
        }
        log(AQ_LOG_DEBUG, std::format("using Vulkan device {} (driver {}.{})", props.properties.deviceName, props.properties.driverVersion >> 22, (props.properties.driverVersion >> 14) & 0xff));
        break;
    }
    if (!physical) {
        log(AQ_LOG_ERROR, "no Vulkan device matching the primary GPU with dma-buf export, DRM modifiers and sync_file support");
        return false;
    }
    vkGetPhysicalDeviceMemoryProperties(physical, &memProps);

    // prefer a transfer-only queue: the copy engine, which runs beside the compositor's rendering
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qs(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &nq, qs.data());
    queueFamily = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++)
        if ((qs[i].queueFlags & VK_QUEUE_TRANSFER_BIT) && !(qs[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) {
            queueFamily = i;
            break;
        }
    if (queueFamily == UINT32_MAX)
        for (uint32_t i = 0; i < nq; i++)
            if (qs[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)) {
                queueFamily = i;
                break;
            }

    float                   prio = 0.5f;
    VkDeviceQueueCreateInfo dq   = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = queueFamily, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo      dci  = {.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                    .queueCreateInfoCount    = 1,
                                    .pQueueCreateInfos       = &dq,
                                    .enabledExtensionCount   = (uint32_t)enable.size(),
                                    .ppEnabledExtensionNames = enable.data()};
    if (vkCreateDevice(physical, &dci, nullptr, &device) != VK_SUCCESS) {
        log(AQ_LOG_ERROR, "vkCreateDevice failed");
        return false;
    }
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    proc.getMemoryFd           = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR");
    proc.getMemoryFdProperties = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(device, "vkGetMemoryFdPropertiesKHR");
    proc.importSemaphoreFd     = (PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(device, "vkImportSemaphoreFdKHR");
    proc.getSemaphoreFd        = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(device, "vkGetSemaphoreFdKHR");
    if (!proc.getMemoryFd || !proc.getMemoryFdProperties || !proc.importSemaphoreFd || !proc.getSemaphoreFd) {
        log(AQ_LOG_ERROR, "missing external memory / semaphore entry points");
        return false;
    }

    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = queueFamily};
    if (vkCreateCommandPool(device, &pci, nullptr, &pool) != VK_SUCCESS)
        return false;

    for (auto& s : slots) {
        VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VkFenceCreateInfo           fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkSemaphoreCreateInfo       sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkExportSemaphoreCreateInfo esi = {.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
        VkSemaphoreCreateInfo       se2 = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &esi};
        if (vkAllocateCommandBuffers(device, &cai, &s.cmd) != VK_SUCCESS || vkCreateFence(device, &fci, nullptr, &s.fence) != VK_SUCCESS ||
            vkCreateSemaphore(device, &sci, nullptr, &s.waitSem) != VK_SUCCESS || vkCreateSemaphore(device, &se2, nullptr, &s.doneSem) != VK_SUCCESS) {
            log(AQ_LOG_ERROR, "creating the command ring failed");
            return false;
        }
    }

    log(AQ_LOG_DEBUG, std::format("ready: queue family {} ({}), foreign queue family {}", queueFamily,
                                  (qs[queueFamily].queueFlags & VK_QUEUE_GRAPHICS_BIT) ? "graphics" : "transfer only", foreign ? "yes" : "no (using EXTERNAL)"));
    return true;
}

Aquamarine::CMgpuVkCopy::~CMgpuVkCopy() {
    if (device) {
        vkDeviceWaitIdle(device);
        for (auto& s : slots) {
            if (s.fence)
                vkDestroyFence(device, s.fence, nullptr);
            if (s.waitSem)
                vkDestroySemaphore(device, s.waitSem, nullptr);
            if (s.doneSem)
                vkDestroySemaphore(device, s.doneSem, nullptr);
        }
        if (pool)
            vkDestroyCommandPool(device, pool, nullptr);
        vkDestroyDevice(device, nullptr);
    }
    if (instance)
        vkDestroyInstance(instance, nullptr);
}

uint32_t Aquamarine::CMgpuVkCopy::memoryType(uint32_t bits, VkMemoryPropertyFlags wanted, VkMemoryPropertyFlags avoid) {
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        const auto F = memProps.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) && (F & wanted) == wanted && !(F & avoid))
            return i;
    }
    return UINT32_MAX;
}

SP<CVkLinearBuffer> Aquamarine::CMgpuVkCopy::allocate(const Vector2D& size, uint32_t drmFormat) {
    const uint32_t W = (uint32_t)size.x, H = (uint32_t)size.y;
    if (!W || !H || vkFormatFor(drmFormat) == VK_FORMAT_UNDEFINED) {
        log(AQ_LOG_ERROR, std::format("cannot allocate a {}x{} buffer of format 0x{:x}", W, H, drmFormat));
        return nullptr;
    }

    auto buf    = SP<CVkLinearBuffer>(new CVkLinearBuffer());
    buf->copier = self;
    buf->stride = ((W * 4) + 255) & ~255u; // scanout engines want generous pitch alignment

    VkExternalMemoryBufferCreateInfo ebi = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkBufferCreateInfo               bci = {.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                            .pNext       = &ebi,
                                            .size        = (VkDeviceSize)buf->stride * H,
                                            .usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (vkCreateBuffer(device, &bci, nullptr, &buf->buffer) != VK_SUCCESS) {
        log(AQ_LOG_ERROR, "vkCreateBuffer failed");
        return nullptr;
    }

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(device, buf->buffer, &mr);
    // system RAM: the secondary GPU and its display engine must reach it
    uint32_t mt = memoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX)
        mt = memoryType(mr.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) {
        log(AQ_LOG_ERROR, "no system-RAM memory type can hold an exportable buffer");
        return nullptr;
    }
    VkMemoryDedicatedAllocateInfo ded = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = buf->buffer};
    VkExportMemoryAllocateInfo    exp = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, .pNext = &ded, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkMemoryAllocateInfo          mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &exp, .allocationSize = mr.size, .memoryTypeIndex = mt};
    if (vkAllocateMemory(device, &mai, nullptr, &buf->memory) != VK_SUCCESS || vkBindBufferMemory(device, buf->buffer, buf->memory, 0) != VK_SUCCESS) {
        log(AQ_LOG_ERROR, "allocating or binding the exportable memory failed");
        return nullptr;
    }

    int                   fd  = -1;
    VkMemoryGetFdInfoKHR  gfi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = buf->memory, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    if (proc.getMemoryFd(device, &gfi, &fd) != VK_SUCCESS || fd < 0) {
        log(AQ_LOG_ERROR, "exporting the buffer as a dma-buf failed");
        return nullptr;
    }

    buf->attrs.success    = true;
    buf->attrs.size       = size;
    buf->size             = size; // IBuffer::size: the atomic commit sizes the plane (SRC_W/H, CRTC_W/H) from it; 0x0 = black screen (2026-11-13)
    buf->attrs.format     = drmFormat;
    buf->attrs.modifier   = DRM_FORMAT_MOD_LINEAR;
    buf->attrs.planes     = 1;
    buf->attrs.offsets[0] = 0;
    buf->attrs.strides[0] = buf->stride;
    buf->attrs.fds[0]     = fd;
    log(AQ_LOG_DEBUG, std::format("allocated a linear {}x{} buffer (pitch {}) in memory type {}, exported as dma-buf", W, H, buf->stride, mt));
    return buf;
}

SP<CVkImportedImageAttachment> Aquamarine::CMgpuVkCopy::importImage(SP<IBuffer> from) {
    const auto DMA = from->dmabuf();
    if (auto at = from->attachments.get<CVkImportedImageAttachment>(); at && at->modifier == DMA.modifier && at->size == DMA.size)
        return at;

    if (!DMA.success || DMA.planes != 1 || DMA.modifier == DRM_FORMAT_MOD_INVALID || vkFormatFor(DMA.format) == VK_FORMAT_UNDEFINED) {
        log(AQ_LOG_ERROR, std::format("cannot import the primary's buffer (planes {}, modifier 0x{:x}, format 0x{:x})", DMA.planes, DMA.modifier, DMA.format));
        return nullptr;
    }

    auto at      = makeShared<CVkImportedImageAttachment>();
    at->copier   = self;
    at->modifier = DMA.modifier;
    at->size     = DMA.size;

    VkSubresourceLayout                           layout = {.offset = DMA.offsets[0], .size = 0, .rowPitch = DMA.strides[0]};
    VkImageDrmFormatModifierExplicitCreateInfoEXT expl   = {.sType                       = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
                                                            .drmFormatModifier           = DMA.modifier,
                                                            .drmFormatModifierPlaneCount = 1,
                                                            .pPlaneLayouts               = &layout};
    VkExternalMemoryImageCreateInfo emi = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .pNext = &expl, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageCreateInfo               ici = {.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                           .pNext         = &emi,
                                           .imageType     = VK_IMAGE_TYPE_2D,
                                           .format        = vkFormatFor(DMA.format),
                                           .extent        = {(uint32_t)DMA.size.x, (uint32_t)DMA.size.y, 1},
                                           .mipLevels     = 1,
                                           .arrayLayers   = 1,
                                           .samples       = VK_SAMPLE_COUNT_1_BIT,
                                           .tiling        = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                                           .usage         = VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                           .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
                                           .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(device, &ici, nullptr, &at->image) != VK_SUCCESS) {
        log(AQ_LOG_ERROR, std::format("vkCreateImage for the primary's buffer failed (modifier 0x{:x})", DMA.modifier));
        return nullptr;
    }

    const int fd = dup(DMA.fds[0]);
    if (fd < 0)
        return nullptr;
    VkMemoryFdPropertiesKHR fdp = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    if (proc.getMemoryFdProperties(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fdp) != VK_SUCCESS) {
        close(fd);
        log(AQ_LOG_ERROR, "the primary's dma-buf is not importable");
        return nullptr;
    }
    VkMemoryRequirements imr;
    vkGetImageMemoryRequirements(device, at->image, &imr);
    const uint32_t mt = memoryType(imr.memoryTypeBits & fdp.memoryTypeBits, 0);
    if (mt == UINT32_MAX) {
        close(fd);
        log(AQ_LOG_ERROR, "no memory type can import the primary's dma-buf");
        return nullptr;
    }
    VkMemoryDedicatedAllocateInfo ded = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = at->image};
    VkImportMemoryFdInfoKHR       imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, .pNext = &ded, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = fd};
    VkMemoryAllocateInfo          mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = imr.size, .memoryTypeIndex = mt};
    if (vkAllocateMemory(device, &mai, nullptr, &at->memory) != VK_SUCCESS) {
        close(fd); // ownership moves to Vulkan only on success
        log(AQ_LOG_ERROR, "importing the primary's dma-buf failed");
        return nullptr;
    }
    if (vkBindImageMemory(device, at->image, at->memory, 0) != VK_SUCCESS) {
        log(AQ_LOG_ERROR, "binding the imported memory failed");
        return nullptr;
    }

    from->attachments.add(at);
    log(AQ_LOG_DEBUG, std::format("imported a {}x{} primary buffer (modifier 0x{:x})", DMA.size.x, DMA.size.y, DMA.modifier));
    return at;
}

int Aquamarine::CMgpuVkCopy::copy(SP<IBuffer> from, SP<CVkLinearBuffer> to, const CRegion& region, int waitFD) {
    if (!from || !to || !to->good())
        return -1;

    const auto IMG = importImage(from);
    if (!IMG)
        return -1;

    auto& slot = slots[nextSlot];
    nextSlot   = (nextSlot + 1) % SLOTS;
    if (slot.submitted) {
        // never blocks in practice: frames are paced by page flips, a slot comes back SLOTS frames later
        vkWaitForFences(device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
        vkResetFences(device, 1, &slot.fence);
        slot.submitted = false;
    }

    bool waiting = false;
    if (waitFD >= 0) {
        const int fd = dup(waitFD);
        VkImportSemaphoreFdInfoKHR isf = {.sType      = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
                                          .semaphore  = slot.waitSem,
                                          .flags      = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
                                          .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
                                          .fd         = fd};
        if (fd >= 0 && proc.importSemaphoreFd(device, &isf) == VK_SUCCESS)
            waiting = true;
        else if (fd >= 0)
            close(fd);
    }

    const uint32_t                 OTHER = otherFamily;
    const VkImageSubresourceRange  RANGE = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkCommandBufferBeginInfo       cbi   = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkResetCommandBuffer(slot.cmd, 0);
    vkBeginCommandBuffer(slot.cmd, &cbi);

    // take the frame from its owner (GL) keeping its content: GENERAL, never UNDEFINED
    VkImageMemoryBarrier acquire = {.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                    .srcAccessMask       = 0,
                                    .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
                                    .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
                                    .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    .srcQueueFamilyIndex = OTHER,
                                    .dstQueueFamilyIndex = queueFamily,
                                    .image               = IMG->image,
                                    .subresourceRange    = RANGE};
    vkCmdPipelineBarrier(slot.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &acquire);

    const auto                     SIZE = from->dmabuf().size;
    CRegion                        clipped = region.copy();
    clipped.intersect(0, 0, SIZE.x, SIZE.y);
    std::vector<VkBufferImageCopy> regions;
    clipped.forEachRect([&](const pixman_box32_t& r) {
        const int W = r.x2 - r.x1, H = r.y2 - r.y1;
        if (W <= 0 || H <= 0)
            return;
        regions.push_back({.bufferOffset      = (VkDeviceSize)r.y1 * to->stride + (VkDeviceSize)r.x1 * 4,
                           .bufferRowLength   = to->stride / 4,
                           .bufferImageHeight = 0,
                           .imageSubresource  = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .imageOffset       = {r.x1, r.y1, 0},
                           .imageExtent       = {(uint32_t)W, (uint32_t)H, 1}});
    });
    if (!regions.empty())
        vkCmdCopyImageToBuffer(slot.cmd, IMG->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to->buffer, (uint32_t)regions.size(), regions.data());

    // give both back: the frame to GL, the linear buffer to the secondary GPU's display engine
    VkImageMemoryBarrier  release  = acquire;
    release.srcAccessMask          = VK_ACCESS_TRANSFER_READ_BIT;
    release.dstAccessMask          = 0;
    release.oldLayout              = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    release.newLayout              = VK_IMAGE_LAYOUT_GENERAL;
    release.srcQueueFamilyIndex    = queueFamily;
    release.dstQueueFamilyIndex    = OTHER;
    VkBufferMemoryBarrier handOver = {.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                      .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
                                      .dstAccessMask       = 0,
                                      .srcQueueFamilyIndex = queueFamily,
                                      .dstQueueFamilyIndex = OTHER,
                                      .buffer              = to->buffer,
                                      .offset              = 0,
                                      .size                = VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(slot.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 1, &handOver, 1, &release);
    vkEndCommandBuffer(slot.cmd);

    const VkPipelineStageFlags STAGE = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo               si    = {.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                        .waitSemaphoreCount   = waiting ? 1u : 0u,
                                        .pWaitSemaphores      = &slot.waitSem,
                                        .pWaitDstStageMask    = &STAGE,
                                        .commandBufferCount   = 1,
                                        .pCommandBuffers      = &slot.cmd,
                                        .signalSemaphoreCount = 1,
                                        .pSignalSemaphores    = &slot.doneSem};
    if (vkQueueSubmit(queue, 1, &si, slot.fence) != VK_SUCCESS) {
        log(AQ_LOG_ERROR, "vkQueueSubmit failed");
        return -1;
    }
    slot.submitted = true;

    int                     out = -1;
    VkSemaphoreGetFdInfoKHR sgf = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, .semaphore = slot.doneSem, .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
    if (proc.getSemaphoreFd(device, &sgf, &out) != VK_SUCCESS || out < 0) {
        log(AQ_LOG_ERROR, "exporting the copy's sync_file failed");
        return -1;
    }
    return out;
}
