#include "MgpuCopy.hpp"
#include "CpuCopy.hpp"
#include "VkCopy.hpp"
#include "../Renderer.hpp"
#include "FormatUtils.hpp"
#include "Shared.hpp"
#include <aquamarine/allocator/DRMDumb.hpp>
#include <aquamarine/allocator/Swapchain.hpp>
#include <aquamarine/backend/Backend.hpp>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <poll.h>
#include <random>
#include <unistd.h>
#include <unordered_map>
#include <vector>

using namespace Aquamarine;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Math;
#define SP CSharedPointer
#define WP CWeakPointer

using steady = std::chrono::steady_clock;

namespace {
    constexpr size_t BUFFERS = 2; // scanout buffers per output; the display flips between them

    // The swapchain rotates, so the buffer we get was last written BUFFERS frames ago. This remembers the damage of
    // the frames in between, to bring it up to date by copying as little as possible.
    struct SDamageJournal {
        std::deque<CRegion> damage; // newest first, one entry per written frame
        size_t              written = 0;

        CRegion outdated(const CRegion& now, const CRegion& full) const {
            if (written < BUFFERS)
                return full.copy();
            CRegion region = now.copy();
            for (size_t i = 0; i + 1 < BUFFERS && i < damage.size(); ++i)
                region.add(damage[i]);
            return region;
        }

        void wrote(CRegion&& now) {
            damage.push_front(std::move(now));
            while (damage.size() > BUFFERS)
                damage.pop_back();
            ++written;
        }

        void reset() {
            damage.clear();
            written = 0;
        }
    };

    // the linear buffers one engine writes for one output
    struct STarget {
        SP<IAllocator> allocator;
        SP<CSwapchain> swapchain;
        SDamageJournal journal;

        SP<IBuffer> next(const Vector2D& size, uint32_t format) {
            auto OPTIONS   = swapchain->currentOptions();
            OPTIONS.size   = size;
            OPTIONS.format = format;
            OPTIONS.length = BUFFERS;
            OPTIONS.cursor = false;
            if (const auto& CUR = swapchain->currentOptions(); CUR.size != OPTIONS.size || CUR.format != OPTIONS.format || CUR.length != OPTIONS.length) {
                journal.reset();
                if (!swapchain->reconfigure(OPTIONS))
                    return nullptr;
            }
            return swapchain->next(nullptr);
        }
    };

    struct SStats {
        uint64_t gpuFrames = 0, cpuFrames = 0, gpuFailures = 0, lateFences = 0, verifyOk = 0, verifyBad = 0;
        double   copyCpuMs = 0, copyCpuMaxMs = 0; // CPU time inside CMgpuVkCopy::copy()
        double   gpuMs = 0;                       // submit -> fence, on the frames that wait on the CPU
        uint64_t gpuMsSamples = 0;
    };
    SStats& stats() {
        static SStats s;
        return s;
    }

    bool gpuEnabled() {
        static const bool ENABLED = envEnabled("AQ_MGPU_GPU_COPY");
        return ENABLED;
    }
    bool cpuEnabled() {
        static const bool ENABLED = envEnabled("AQ_MGPU_CPU_COPY");
        return ENABLED;
    }
    // every n-th GPU-copied frame is checked; 0 = never
    uint64_t verifyEvery() {
        static const uint64_t N = [] {
            const char* env = getenv("AQ_MGPU_GPU_COPY_VERIFY");
            return env ? (uint64_t)std::max(0L, atol(env)) : 0UL;
        }();
        return N;
    }

    double threadCpuMs() {
        timespec t;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
        return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
    }

    // sleeps until the sync_file signals (or `ms` pass); true if it signalled
    bool waitFence(int fd, int ms) {
        pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
        int    ret;
        while ((ret = poll(&pfd, 1, ms)) < 0 && errno == EINTR) {
            ;
        }
        return ret > 0;
    }

    // Never destroyed on purpose: compositors keep the backend in a global, so outputs can be destroyed during
    // exit() after this library's statics are already gone (a plain static map crashed Hyprland on exit).
    SP<CMgpuVkCopy>& vkCopier() {
        static auto* copier = new SP<CMgpuVkCopy>();
        return *copier;
    }
    SP<CMgpuCpuReader>& cpuReader() {
        static auto* reader = new SP<CMgpuCpuReader>();
        return *reader;
    }
}

struct Aquamarine::SMgpuOutput {
    STarget            gpu, cpu;
    int                lastFence        = -1;    // the GPU copy's fence handed to KMS last frame; closed on the next one
    bool               gpuCommitPending = false; // this commit's framebuffer came from the GPU copy
    bool               gpuOff           = false; // the GPU copy failed here: CPU copy until gpuRetryAt
    uint32_t           gpuFailures      = 0;
    steady::time_point gpuRetryAt;
    uint64_t           gpuFrames = 0;

    ~SMgpuOutput() {
        if (lastFence >= 0)
            close(lastFence);
    }
};

static std::unordered_map<const CDRMOutput*, SMgpuOutput>& outputs() {
    static auto* map = new std::unordered_map<const CDRMOutput*, SMgpuOutput>(); // never destroyed, see vkCopier()
    return *map;
}

SMgpuVerifyResult Aquamarine::mgpuVerifySamples(SP<CMgpuCpuReader> reader, SP<IBuffer> src, SP<CVkLinearBuffer> lin, const CRegion& copied, uint32_t seed) {
    constexpr int           BLOCK = 64, IN_COPIED = 12, ANYWHERE = 4;
    SMgpuVerifyResult       res;
    CMgpuCpuReader::eLayout layout;
    const uint8_t*          got = lin ? lin->map() : nullptr;
    if (!reader || !src || !got || !CMgpuCpuReader::layoutFor(src->dmabuf().format, layout))
        return res;

    const int W = lin->size.x, H = lin->size.y;
    // where the samples of the rendered frame are read back, at their own offsets; only those pages are ever touched
    uint8_t* const want = (uint8_t*)calloc(1, (size_t)lin->stride * H);
    if (!want)
        return res;

    std::minstd_rand            rng(seed * 2654435761u + 12345u);
    std::vector<pixman_box32_t> blocks;
    CRegion                     sample;
    auto                        pick = [&](int x1, int y1, int x2, int y2) {
        x1 = std::clamp(x1, 0, W), x2 = std::clamp(x2, 0, W), y1 = std::clamp(y1, 0, H), y2 = std::clamp(y2, 0, H);
        if (x2 <= x1 || y2 <= y1)
            return;
        const int BW = std::min(BLOCK, x2 - x1), BH = std::min(BLOCK, y2 - y1);
        const int X = x1 + (int)(rng() % (uint32_t)(x2 - x1 - BW + 1)), Y = y1 + (int)(rng() % (uint32_t)(y2 - y1 - BH + 1));
        blocks.push_back({X, Y, X + BW, Y + BH});
        sample.add(X, Y, BW, BH);
    };
    const auto RECTS = copied.getRects();
    for (int i = 0; i < IN_COPIED && !RECTS.empty(); ++i) {
        const auto& r = RECTS[rng() % RECTS.size()];
        pick(r.x1, r.y1, r.x2, r.y2);
    }
    const size_t FIRST_ANYWHERE = blocks.size();
    for (int i = 0; i < ANYWHERE; ++i)
        pick(0, 0, W, H);

    if (!reader->readRegionInto(src, want, lin->stride, layout, sample)) {
        free(want);
        return res;
    }

    // alpha / padding bits are not part of the picture: the CPU reader sets them, the GPU copy carries them over
    const uint32_t MASK = CMgpuCpuReader::isPacked(layout) ? 0x3FFFFFFFu : 0x00FFFFFFu;
    res.ran             = true;
    res.blocks          = blocks.size();
    for (size_t b = 0; b < blocks.size(); ++b) {
        const auto& r = blocks[b];
        for (int y = r.y1; y < r.y2; ++y) {
            const size_t ROW = (size_t)y * lin->stride;
            for (int x = r.x1; x < r.x2; ++x) {
                uint32_t scan, rend;
                memcpy(&scan, got + ROW + (size_t)x * 4, 4);
                memcpy(&rend, want + ROW + (size_t)x * 4, 4);
                ++res.pixels;
                if ((scan & MASK) != (rend & MASK)) {
                    if (!res.bad)
                        res.first = std::format("first at ({}, {}): scanout 0x{:08x}, rendered 0x{:08x}, {}", x, y, scan, rend,
                                                b < FIRST_ANYWHERE ? "inside this frame's copy" : "random spot of the frame");
                    ++res.bad;
                }
            }
        }
    }
    free(want);
    return res;
}

// report only: nothing changes course because of a mismatch
static void verifyFrame(SMgpuOutput& o, SP<CMgpuCpuReader> reader, SP<IBuffer> src, SP<CVkLinearBuffer> lin, const CRegion& copied) {
    const auto T0  = steady::now();
    const auto RES = mgpuVerifySamples(reader, src, lin, copied, (uint32_t)o.gpuFrames);
    const auto MS  = std::chrono::duration<double, std::milli>(steady::now() - T0).count();
    auto&      s   = stats();
    if (!RES.ran)
        mgpuGpuCopyTrace("verify: skipped (no reader, no CPU view of the scanout buffer, unknown format, or the read-back failed)");
    else if (RES.bad) {
        ++s.verifyBad;
        mgpuGpuCopyTrace(std::format("VERIFY MISMATCH on frame {}: {} of {} sampled pixels differ, {}", o.gpuFrames, RES.bad, RES.pixels, RES.first));
    } else if (++s.verifyOk <= 3 || s.verifyOk % 100 == 0)
        mgpuGpuCopyTrace(
            std::format("verify ok on frame {} ({} pixels in {} blocks, {:.1f} ms; {} checks ok, {} bad so far)", o.gpuFrames, RES.pixels, RES.blocks, MS, s.verifyOk, s.verifyBad));
}

bool Aquamarine::CMgpuCopy::enabled() {
    return gpuEnabled() || cpuEnabled();
}

// the primary's reader, created with (or after) its renderer
SP<CMgpuCpuReader> Aquamarine::CMgpuCopy::reader(SP<CDRMBackend> primary) {
    if (!primary)
        return nullptr;
    if (!primary->rendererState.renderer)
        primary->initMgpu();
    const auto RENDERER = primary->rendererState.renderer;
    if (!RENDERER)
        return nullptr;
    if (!cpuReader() || !cpuReader()->usesRenderer(RENDERER))
        cpuReader() = makeShared<CMgpuCpuReader>(RENDERER);
    return cpuReader();
}

void Aquamarine::CMgpuCopy::forget(const CDRMOutput* output) {
    outputs().erase(output);
}

void Aquamarine::CMgpuCopy::gpuFailed(const CDRMOutput* output, SMgpuOutput& o, const std::string& why) {
    // try again later: 30 s, 2 min, 8 min, 32 min, then hourly. A retry that fails costs at most one frame.
    const auto PAUSE = std::chrono::seconds(std::min<uint64_t>(3600, 30ULL << (2 * std::min<uint32_t>(o.gpuFailures, 4))));
    o.gpuOff         = true;
    o.gpuRetryAt     = steady::now() + PAUSE;
    ++o.gpuFailures;
    ++stats().gpuFailures;
    o.gpu.journal.reset();

    const auto MSG = std::format("{}{}; failure {} on this output, next try in {} s", why, cpuEnabled() ? "; using AQ_MGPU_CPU_COPY" : "", o.gpuFailures, PAUSE.count());
    mgpuGpuCopyTrace("FAILED: " + MSG);
    if (const auto BACKEND = output->backend.lock(); BACKEND)
        BACKEND->backend->log(AQ_LOG_ERROR, "drm: AQ_MGPU_GPU_COPY: " + MSG);
}

void Aquamarine::CMgpuCopy::commitResult(const CDRMOutput* output, bool ok, bool onlyTest) {
    if (!gpuEnabled())
        return;
    const auto IT = outputs().find(output);
    if (IT == outputs().end() || !IT->second.gpuCommitPending)
        return;
    IT->second.gpuCommitPending = false;
    // safety net: if KMS refuses the framebuffer the GPU copy produced (a plane may not take a linear buffer from
    // another device), the next frame goes through the CPU copy
    if (!ok)
        gpuFailed(output, IT->second, std::format("KMS refused the commit{}", onlyTest ? " (test)" : ""));
}

CMgpuCopy::eResult Aquamarine::CMgpuCopy::copyFrame(CDRMOutput* output, SDRMConnectorCommitData& data, bool onlyTest, SP<CDRMFB>& fb) {
    if (!enabled() || !output || !data.outputState.buffer)
        return eResult::NOT_HANDLED;

    auto& o            = outputs()[output];
    o.gpuCommitPending = false;

    // both engines replace the commit's in-fence: keep what the compositor passed
    const int IN_FENCE = data.outputState.explicitInFence;

    if (gpuEnabled()) {
        if (o.gpuOff && steady::now() >= o.gpuRetryAt) {
            o.gpuOff = false;
            mgpuGpuCopyTrace(std::format("trying the GPU copy again (after {} failures on this output)", o.gpuFailures));
        }
        if (!o.gpuOff) {
            std::string why;
            if (gpuCopy(output, o, data, IN_FENCE, onlyTest, fb, why)) {
                o.gpuCommitPending = true;
                return eResult::COPIED;
            }
            data.outputState.explicitInFence = IN_FENCE;
            fb.reset();
            gpuFailed(output, o, why);
        }
    }

    if (cpuEnabled())
        return cpuCopy(output, o, data, IN_FENCE, fb) ? eResult::COPIED : eResult::FAILED;

    return eResult::NOT_HANDLED;
}

bool Aquamarine::CMgpuCopy::gpuCopy(CDRMOutput* output, SMgpuOutput& o, SDRMConnectorCommitData& data, int inFence, bool onlyTest, SP<CDRMFB>& fb, std::string& why) {
    const auto     BACKEND   = output->backend.lock();
    const auto     PRIMARY   = BACKEND ? BACKEND->primary.lock() : nullptr;
    const auto&    STATE     = data.outputState;
    const uint32_t COMMITTED = STATE.committed;
    if (!BACKEND || !PRIMARY) {
        why = "no primary GPU";
        return false;
    }

    if (!vkCopier()) {
        static bool tried = false;
        if (!tried) {
            tried      = true;
            vkCopier() = CMgpuVkCopy::create(PRIMARY->gpu->fd, BACKEND->backend);
        }
    }
    const auto COPIER = vkCopier();
    if (!COPIER) {
        why = "no usable Vulkan device on the primary GPU";
        return false;
    }

    // the bytes are copied as they are: scan them out as the opaque variant of the primary's format
    const auto DMA     = STATE.buffer->dmabuf();
    uint32_t   scanFmt = DMA.format;
    switch (DMA.format) {
        case DRM_FORMAT_ARGB8888: scanFmt = DRM_FORMAT_XRGB8888; break;
        case DRM_FORMAT_ABGR8888: scanFmt = DRM_FORMAT_XBGR8888; break;
        case DRM_FORMAT_ARGB2101010: scanFmt = DRM_FORMAT_XRGB2101010; break;
        case DRM_FORMAT_ABGR2101010: scanFmt = DRM_FORMAT_XBGR2101010; break;
        default: break;
    }
    if (CMgpuVkCopy::vkFormatFor(scanFmt) == VK_FORMAT_UNDEFINED) {
        why = std::format("unsupported format {}", fourccToName(DMA.format));
        return false;
    }

    if (!o.gpu.allocator) {
        o.gpu.allocator = CVkLinearAllocator::create(COPIER, BACKEND->backend);
        o.gpu.swapchain = CSwapchain::create(o.gpu.allocator, BACKEND);
    }
    const auto SIZE = STATE.buffer->size;
    const auto BUF  = o.gpu.next(SIZE, scanFmt);
    if (!BUF) {
        why = "the linear swapchain has no buffer";
        return false;
    }
    // the atomic commit sizes the plane from IBuffer::size: a mismatch is a black screen, not a failed commit
    if (BUF->size != SIZE || BUF->dmabuf().size != SIZE) {
        why = std::format("the linear buffer is {} (dmabuf {}), the frame is {}", BUF->size, BUF->dmabuf().size, SIZE);
        return false;
    }
    const auto LINEAR = reinterpretPointerCast<CVkLinearBuffer>(BUF);

    CRegion full;
    full.add(0, 0, SIZE.x, SIZE.y);
    static const bool ALWAYS_FULL = envEnabled("AQ_MGPU_GPU_COPY_FULL"); // debug: ignore damage
    const bool        HAS_DAMAGE  = (COMMITTED & COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_DAMAGE) && !STATE.damage.empty();
    CRegion           damage      = (HAS_DAMAGE && !ALWAYS_FULL) ? STATE.damage.copy() : full.copy();
    const CRegion     TO_COPY     = o.gpu.journal.outdated(damage, full);

    // what the copy waits for: the commit's render fence, or (without one) everything this process submitted so far
    // on the primary, the same ordering AQ_MGPU_CPU_COPY relies on
    const bool EXPLICIT = (COMMITTED & COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_EXPLICIT_IN_FENCE) && inFence >= 0;
    const bool VERIFY   = verifyEvery() && o.gpuFrames % verifyEvery() == verifyEvery() - 1;
    // the primary's renderer is only brought up when it is needed, as before
    const auto READER   = (!EXPLICIT || VERIFY) ? reader(PRIMARY) : nullptr;
    const int  OWN_WAIT = !EXPLICIT && READER ? READER->createNativeFenceFD() : -1;

    const double CPU0 = threadCpuMs();
    const int    DONE = COPIER->copy(STATE.buffer, LINEAR, TO_COPY, EXPLICIT ? inFence : OWN_WAIT);
    const double CPU  = threadCpuMs() - CPU0;
    const auto   SUBMITTED = steady::now();
    if (OWN_WAIT >= 0)
        close(OWN_WAIT);
    if (DONE < 0) {
        why = "the copy failed";
        return false;
    }

    auto& s = stats();
    if (o.lastFence >= 0) {
        if (!waitFence(o.lastFence, 0))
            ++s.lateFences; // the previous frame's copy is still not done, a whole frame later
        close(o.lastFence);
    }
    o.lastFence = DONE;

    const bool KMS_WAITS = EXPLICIT && output->supportsExplicit && !VERIFY;
    if (KMS_WAITS)
        data.outputState.explicitInFence = DONE; // KMS waits for the copy, the CPU never does
    else {
        // no in-fence on this commit (or the frame is being verified): wait here, asleep, so the display never
        // shows a half copy
        waitFence(DONE, 100);
        data.outputState.explicitInFence = -1;
        s.gpuMs += std::chrono::duration<double, std::milli>(steady::now() - SUBMITTED).count();
        ++s.gpuMsSamples;
    }

    if (VERIFY)
        verifyFrame(o, READER, STATE.buffer, LINEAR, TO_COPY);

    fb = CDRMFB::create(BUF, output->backend, nullptr);
    if (!fb) {
        why = "the secondary GPU refused the linear buffer as a framebuffer";
        return false;
    }

    o.gpu.journal.wrote(std::move(damage));
    o.cpu.journal.reset(); // the dumb buffers, if any, are stale from now on
    ++o.gpuFrames;
    ++s.gpuFrames;
    s.copyCpuMs += CPU;
    s.copyCpuMaxMs = std::max(s.copyCpuMaxMs, CPU);

    if (s.gpuFrames <= 3 || s.gpuFrames % 1000 == 0)
        mgpuGpuCopyTrace(std::format("frame {} copied ({} rects, {} fence{}) | copy() CPU avg {:.3f} ms, max {:.3f} ms | GPU copy ~{:.2f} ms ({} samples) | late fences {} | "
                                     "GPU failures {} | CPU-copied frames {} | verify ok {}, bad {}",
                                     s.gpuFrames, TO_COPY.getRects().size(), KMS_WAITS ? "KMS in-" : "CPU-polled", onlyTest ? ", test commit" : "", s.copyCpuMs / s.gpuFrames,
                                     s.copyCpuMaxMs, s.gpuMsSamples ? s.gpuMs / s.gpuMsSamples : 0.0, s.gpuMsSamples, s.lateFences, s.gpuFailures, s.cpuFrames, s.verifyOk,
                                     s.verifyBad));
    static bool announced = false;
    if (!announced) {
        announced = true;
        BACKEND->backend->log(AQ_LOG_DEBUG, std::format("drm: AQ_MGPU_GPU_COPY: active ({} {}x{}, {} fence)", fourccToName(scanFmt), SIZE.x, SIZE.y,
                                                         KMS_WAITS ? "KMS in-" : "CPU-polled"));
    }
    return true;
}

bool Aquamarine::CMgpuCopy::cpuCopy(CDRMOutput* output, SMgpuOutput& o, SDRMConnectorCommitData& data, int inFence, SP<CDRMFB>& fb) {
    const auto     BACKEND   = output->backend.lock();
    const auto     PRIMARY   = BACKEND ? BACKEND->primary.lock() : nullptr;
    const auto&    STATE     = data.outputState;
    const uint32_t COMMITTED = STATE.committed;
    if (!BACKEND)
        return false;

    const auto READER = reader(PRIMARY);
    if (!READER) {
        BACKEND->backend->log(AQ_LOG_ERROR, "drm: AQ_MGPU_CPU_COPY: no renderer on the primary GPU");
        return false;
    }

    // the dumb buffer gets the same layout as the primary's buffer, except that 10 bit always goes opaque: planes
    // often scan out [XA]RGB8888 and X*2101010 but not A*2101010 (Intel gen9 doesn't)
    const auto              DMA     = STATE.buffer->dmabuf();
    uint32_t                dumbFmt = DMA.format;
    CMgpuCpuReader::eLayout layout;
    if (!CMgpuCpuReader::layoutFor(DMA.format, layout)) {
        BACKEND->backend->log(AQ_LOG_ERROR, std::format("drm: AQ_MGPU_CPU_COPY: unsupported format {}", fourccToName(DMA.format)));
        return false;
    }
    if (layout == CMgpuCpuReader::eLayout::XBGR2101010)
        dumbFmt = DRM_FORMAT_XBGR2101010;
    else if (layout == CMgpuCpuReader::eLayout::XRGB2101010)
        dumbFmt = DRM_FORMAT_XRGB2101010;

    if (!o.cpu.allocator) {
        o.cpu.allocator = CDRMDumbAllocator::create(BACKEND->gpu->fd, BACKEND->backend);
        if (!o.cpu.allocator) {
            BACKEND->backend->log(AQ_LOG_ERROR, "drm: AQ_MGPU_CPU_COPY: no dumb allocator on the secondary GPU");
            return false;
        }
        o.cpu.swapchain = CSwapchain::create(o.cpu.allocator, BACKEND);
    }

    const auto SIZE = STATE.buffer->size;
    const auto BUF  = o.cpu.next(SIZE, dumbFmt);
    if (!BUF) {
        BACKEND->backend->log(AQ_LOG_ERROR, "drm: AQ_MGPU_CPU_COPY: the dumb swapchain has no buffer");
        return false;
    }

    CRegion full;
    full.add(0, 0, SIZE.x, SIZE.y);
    static const bool ALWAYS_FULL = envEnabled("AQ_MGPU_CPU_COPY_FULL"); // debug: ignore damage
    const bool        HAS_DAMAGE  = (COMMITTED & COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_DAMAGE) && !STATE.damage.empty();
    CRegion           damage      = (HAS_DAMAGE && !ALWAYS_FULL) ? STATE.damage.copy() : full.copy();
    const CRegion     TO_COPY     = o.cpu.journal.outdated(damage, full);

    const auto [PTR, FMT, LEN] = BUF->beginDataPtr(0);
    const auto STRIDE          = BUF->dmabuf().strides.at(0);
    const bool COPIED          = PTR &&
        READER->readRegionInto(STATE.buffer, PTR, STRIDE, layout, TO_COPY, (COMMITTED & COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_EXPLICIT_IN_FENCE) ? inFence : -1);
    BUF->endDataPtr();
    if (!COPIED) {
        BACKEND->backend->log(AQ_LOG_ERROR, "drm: AQ_MGPU_CPU_COPY: reading the primary's buffer failed");
        return false;
    }

    o.cpu.journal.wrote(std::move(damage));
    ++stats().cpuFrames;

    // the pixels are already in place: there is nothing for KMS to wait on
    data.outputState.explicitInFence = -1;

    fb = CDRMFB::create(BUF, output->backend, nullptr);
    return !!fb;
}
