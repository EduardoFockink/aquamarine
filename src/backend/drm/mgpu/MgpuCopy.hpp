#pragma once

// "The primary GPU renders, a secondary GPU only scans out", for pairs where the regular multi-GPU path cannot work
// (an NVIDIA primary cannot render into LINEAR, and a Mesa secondary cannot read NVIDIA's native layout). The
// secondary never imports the primary's frame. Instead, each frame, the primary copies the damaged region into a
// linear buffer the secondary can scan out:
//
//   AQ_MGPU_GPU_COPY  the primary's Vulkan copy engine writes into a buffer it exported (VkCopy.hpp): ~0 CPU
//   AQ_MGPU_CPU_COPY  the primary reads the region back into a dumb buffer of the secondary (CpuCopy.hpp)
//
// With both set, the GPU copy is used and the CPU copy is its fallback, per output: a failure switches that output
// to the CPU copy and the GPU copy is tried again after a growing pause.
//
// Debugging: AQ_MGPU_GPU_COPY_LOG=<file> (flushed trace with counters), AQ_MGPU_GPU_COPY_VERIFY=<n> (every n-th
// frame, compare samples of the scanout buffer against the rendered frame), AQ_MGPU_{GPU,CPU}_COPY_FULL (ignore
// damage).
//
// Everything lives in this directory. Upstream files only carry the hooks: DRM.cpp (prepareAsyncCommitData,
// commitState x2, ~CDRMOutput, releaseMgpuResources), GBM.cpp (keep the primary's native layout), and one friend
// declaration each in CDRMOutput, CDRMBackend and CDRMRenderer. State is kept here, not in those classes, so the
// public headers' layout and the ABI stay as they are.

#include <aquamarine/backend/DRM.hpp>
#include <cstdint>
#include <string>

namespace Aquamarine {
    class CMgpuCpuReader;
    class CVkLinearBuffer;
    struct SMgpuOutput;

    struct SMgpuVerifyResult {
        bool        ran    = false; // false: nothing could be compared (no reader, no CPU view, unknown format)
        uint64_t    pixels = 0, bad = 0, blocks = 0;
        std::string first; // where and what the first difference is
    };

    // AQ_MGPU_GPU_COPY_VERIFY: compares samples of the scanout buffer with the rendered frame, once the copy is
    // complete: 64x64 blocks, most inside `copied` (what this frame's copy wrote), some anywhere in the frame (a
    // stale region there means the damage accounting is wrong). Alpha / padding bits are ignored. A whole-frame
    // compare is not possible: the scanout buffer reads back at ~85 MiB/s.
    SMgpuVerifyResult mgpuVerifySamples(Hyprutils::Memory::CSharedPointer<CMgpuCpuReader> reader, Hyprutils::Memory::CSharedPointer<IBuffer> rendered,
                                        Hyprutils::Memory::CSharedPointer<CVkLinearBuffer> scanout, const Hyprutils::Math::CRegion& copied, uint32_t seed);

    class CMgpuCopy {
      public:
        enum class eResult : uint8_t {
            NOT_HANDLED, // not enabled (or only the GPU copy is, and it failed): the caller goes on as upstream does
            COPIED,      // `fb` holds the frame, data.outputState.explicitInFence is what KMS has to wait for
            FAILED,      // the commit cannot go on
        };

        // AQ_MGPU_CPU_COPY or AQ_MGPU_GPU_COPY is set
        static bool enabled();

        // CDRMOutput::commitState(): the output is on a secondary GPU and data.outputState.buffer is a new frame
        static eResult copyFrame(CDRMOutput* output, SDRMConnectorCommitData& data, bool onlyTest, Hyprutils::Memory::CSharedPointer<CDRMFB>& fb);
        // CDRMOutput::commitState(): what KMS said about the commit copyFrame() prepared
        static void commitResult(const CDRMOutput* output, bool ok, bool onlyTest);
        // the output is going away or releasing its multi-GPU resources
        static void forget(const CDRMOutput* output);

      private:
        static bool                                              gpuCopy(CDRMOutput* output, SMgpuOutput& o, SDRMConnectorCommitData& data, int inFence, bool onlyTest,
                                                                         Hyprutils::Memory::CSharedPointer<CDRMFB>& fb, std::string& why);
        static bool                                              cpuCopy(CDRMOutput* output, SMgpuOutput& o, SDRMConnectorCommitData& data, int inFence,
                                                                         Hyprutils::Memory::CSharedPointer<CDRMFB>& fb);
        static void                                              gpuFailed(const CDRMOutput* output, SMgpuOutput& o, const std::string& why);
        static Hyprutils::Memory::CSharedPointer<CMgpuCpuReader> reader(Hyprutils::Memory::CSharedPointer<CDRMBackend> primary);
    };
}
