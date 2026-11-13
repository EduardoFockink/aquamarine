# Hyprland on the NVIDIA GPU with the monitor on the Intel iGPU (aquamarine AQ_MGPU_CPU_COPY)

A small patch set for [aquamarine](https://github.com/hyprwm/aquamarine) (the rendering backend of
Hyprland) that makes **"NVIDIA renders, Intel scans out"** work on hybrid (Optimus) laptops where the
external display is wired to the Intel iGPU, plus the Arch Linux packaging and the standalone tests behind it.

This is a personal fork, not an upstream feature. This `packaging` branch holds the PKGBUILD, the patches
exported from the code branches, a pacman hook, and tests; the code lives in the branches listed below.

## Symptoms this fixes

You have a hybrid laptop (Intel iGPU + NVIDIA dGPU on the proprietary driver), the external monitor is
connected to a port wired to the Intel GPU (USB-C / Thunderbolt / DisplayPort alt mode is typical), and you
set `AQ_DRM_DEVICES` so the NVIDIA card comes first and does the rendering. Then:

- **login loop**: Hyprland starts, the screen stays black, and you are back at the display manager (SDDM, GDM...);
- `~/.cache/hyprland/hyprlandCrashReport*.txt` shows `Hyprland received signal 6(ABRT)` with this stack:

  ```
  dri_create_fence_fd
  Aquamarine::CDRMRenderer::recreateBlitSync()
  Aquamarine::CDRMRenderer::blit(...)
  Aquamarine::CDRMOutput::commitState(bool)
  Aquamarine::CDRMOutput::commit()
  ```

- `AQ_MGPU_NO_EXPLICIT=1` does not help (the fence is still created), and `AQ_FORCE_LINEAR_BLIT` cannot help
  either: the NVIDIA driver lists `LINEAR` only as `external_only`, so it cannot render to it.

Related upstream reports with the same root cause on other GPU pairs:
[#408](https://github.com/hyprwm/aquamarine/issues/408) (NVIDIA + AMD, every frame through a CPU readback) and
[#220](https://github.com/hyprwm/aquamarine/issues/220) (secondary AMD output, "destination dmabuf unsupported").
KWin handles this setup with a CPU import (`chose cpu import with format AR24 and modifier 0x300000000cdb014`
in its debug log); this patch set gives aquamarine the same last-resort path.

Keywords: Hyprland NVIDIA Intel external monitor black screen, reverse PRIME, multi-GPU, hybrid graphics,
Optimus, aquamarine multigpu blit, BLOCK_LINEAR, dumb buffer, Wayland, Arch Linux.

## The problem

Hardware: Dell G7 7588 (i7-8750H with UHD 630, GTX 1050 Ti, NVIDIA proprietary driver), LG 39GX950B
5120x2160 connected over USB-C, which is wired to the **Intel** GPU.

To have the NVIDIA do the compositing while the Intel drives the display, aquamarine's multi-GPU path
(`AQ_DRM_DEVICES=nvidia:intel`) has the secondary (Intel) import the primary's (NVIDIA) buffer and blit it.
That cannot work here:

- the NVIDIA driver cannot render to the `LINEAR` buffers the Intel could import;
- importing the NVIDIA `BLOCK_LINEAR` buffer on the Intel side aborts in Mesa, so the session never starts.

## What the patch does

With `AQ_MGPU_CPU_COPY=1`, for an output on the secondary GPU the secondary never imports or renders from
the primary's buffers. Instead the **primary reads the frame back straight into a CPU-mapped linear dumb
buffer of the secondary**, which only scans it out. KWin and Mutter use the same technique as a last resort;
upstream aquamarine adopted it for evdi/DisplayLink in [#279](https://github.com/hyprwm/aquamarine/pull/279).

| Commit | What it adds |
| --- | --- |
| 1. CPU copy path | GBM keeps the primary's native layout for multi-GPU buffers; a 2-buffer dumb swapchain on the secondary; only the **damaged region** is read back (plus the damage of the frames in between, as the swapchain rotates). 8-bit formats. |
| 2. 10-bit (HDR) | Reading `2101010` into CPU memory is a software conversion in the NVIDIA driver (~330 ms per 5K frame). A shader repacks each pixel into an RGBA8 renderbuffer that reads back on the fast 8-bit path, byte-identical to `X*2101010`. |
| 3. Sleep, don't spin | `glReadPixels` waits for the GPU and the NVIDIA driver busy-waits; the patch waits on a native fence with `poll()` first. |

### AQ_MGPU_GPU_COPY (patch 0004, package 0.16.0-1.4): no CPU copy at all

The NVIDIA cannot render into `LINEAR`, but its **Vulkan** driver can allocate a `LINEAR` buffer in system RAM,
export it as a dma-buf, and copy into it with its copy engine. The Intel imports that dma-buf as a KMS framebuffer.
So each frame: the NVIDIA frame (native block-linear GBM buffer) is imported into Vulkan with its DRM modifier, the
copy waits on the frame's render fence on the GPU, copies only the damaged rectangles, and hands KMS a sync_file
(`IN_FENCE_FD`). The CPU records a command buffer and nothing else. 10-bit frames need no repacking: the copy is raw,
so the bits land in `X*2101010` as they are. If anything fails (no Vulkan device, unsupported format, KMS refusing
the framebuffer), that output falls back to `AQ_MGPU_CPU_COPY`.

Measured outside the session (`tests/vkcopy_class_check.cpp`, the class from the built library, 5120x2160):

| | CPU per full frame |
| --- | --- |
| `AQ_MGPU_CPU_COPY` (readback into the dumb buffer) | ~4.3 ms |
| `AQ_MGPU_GPU_COPY`, 8 bit | **0.15 ms** (copy engine: 3.65 ms of GPU, in the background) |
| `AQ_MGPU_GPU_COPY`, 10 bit | **0.25 ms** |

**Inside a compositor, keep Vulkan away from `$DISPLAY`.** The first build (0.16.0-1.2) left the screen black
at login: the NVIDIA Vulkan driver connects to the X server in `$DISPLAY` while enumerating devices, and inside
Hyprland that is Hyprland's own lazily started Xwayland, which cannot answer while Hyprland's thread is blocked in
that call. Reproduced with a fake X server that accepts and never answers (`DISPLAY=:99`): enumeration hangs.
0.16.0-1.3 clears `DISPLAY` and `WAYLAND_DISPLAY` and disables implicit layers only while the device is created.
`AQ_MGPU_GPU_COPY_LOG=<file>` writes a flushed trace of the path (the compositor's log is buffered and loses its
tail when a stuck session is killed).

**Set `IBuffer::size` on buffers you hand to KMS.** 0.16.0-1.3 also left the screen black: the atomic commit sizes
the plane (`SRC_W/H`, `CRTC_W/H`) from `IBuffer::size`, not from `dmabuf().size`, and the linear buffers left it at
0x0, so every commit failed and the modeset path fell back to the NVIDIA's native buffer, which the Intel cannot
scan out ("drmModeAddFB2WithModifiers unsupported and buffer has explicit modifiers"). The trace showed two frames
copied and then nothing. 0.16.0-1.4 sets it, and a size mismatch falls back to the CPU copy instead of committing;
`vkcopy_class_check` checks both sizes.

Feasibility tests: `tests/vk_dmabuf_export.c` (export, Intel import, KMS framebuffer, timing) and
`tests/vk_gbm_frame_copy.c` (GL frame -> Vulkan -> Intel with fences; Mesa does not abort on the Vulkan
sync_file, unlike the regular multi-GPU path).

### Environment variables

| Variable | Effect |
| --- | --- |
| `AQ_MGPU_CPU_COPY=1` | Enables the CPU copy path for outputs on a secondary GPU. |
| `AQ_MGPU_CPU_COPY_FULL=1` | Debug: copy the whole frame every time, ignoring damage. |
| `AQ_MGPU_CPU_COPY_SPIN=1` | Debug: skip the fence wait (behaviour before commit 3), for A/B measurements. |
| `AQ_MGPU_GPU_COPY=1` | Patch 0004: copy with the primary's Vulkan copy engine into exported linear buffers (no CPU copy). Tried before `AQ_MGPU_CPU_COPY`, which stays the fallback. |
| `AQ_MGPU_GPU_COPY_FULL=1` | Debug: copy the whole frame every time, ignoring damage. |
| `AQ_MGPU_GPU_COPY_LOG=<file>` | Append a flushed trace of the GPU copy path to that file. |

Compositor environment used with it (Hyprland, NVIDIA = `card0`, Intel = `card1`):

```lua
hl.env("AQ_DRM_DEVICES", "/dev/dri/card0:/dev/dri/card1") -- first renders, second scans out
hl.env("AQ_MGPU_CPU_COPY", "1")
hl.env("AQ_FORCE_LINEAR_BLIT", "0") -- let the NVIDIA render in its native layout
hl.env("AQ_NO_MODIFIERS", "1")      -- UHD 630: Y-tiled planes max out at 4096 px wide; 5120 needs X-tiled
```

### Marker files

The package installs `/usr/share/aquamarine/mgpu-cpu-copy` (8-bit copy available),
`/usr/share/aquamarine/mgpu-cpu-copy-10bit` (10-bit too) and, from 0.16.0-1.2, `/usr/share/aquamarine/mgpu-gpu-copy`. A compositor config can test them before enabling
the mode, so that when an upgrade brings back the official package the next login falls back to the
Intel-only mode instead of looping.

## Cost

Measured on the hardware above, 5120x2160, HDR at 48 Hz.

| Case | CPU |
| --- | --- |
| Typing / normal desktop use | ~0.2 ms per changed text line |
| Full-frame readback (standalone) | ~4.3 ms per frame |
| YouTube video, window, Hyprland process | 52.7% of a core → **43.6%** with commit 3 |
| YouTube video, fullscreen, Hyprland process | 61.8% of a core → **59.2%** with commit 3 |

With video on screen, `perf` shows ~80% of the remaining samples in the NVIDIA driver's `memcpy` reading the
readback staging memory: that is the cost of a CPU copy, and it only shows up when large areas change every
frame. Reading into a PBO first was measured and is slower (6.9 vs 4.3 ms per full frame).

## Branches and tags

| Branch / tag | Content |
| --- | --- |
| `main` | Mirror of upstream `main`. |
| `mgpu-module-main` | **Current.** The mgpu copy module (`src/backend/drm/mgpu`: GPU copy, CPU copy, fallback, verification) as one commit on upstream `main` (7c1c55b). Source of [`main/0001-*.patch`](main/); the same patch applies to `v0.16.0`. |
| `v0.16.0.r8-mgpu-2` | The exact code in package `0.16.0.r8.g7c1c55b-2`. |
| `mgpu-gpu-copy-0.16.0` | `AQ_MGPU_GPU_COPY` as one commit on top of `mgpu-cpu-copy-0.16.0`, before the module. Source of patch `0004` in this branch. |
| `v0.16.0-mgpu-1.4` | The exact code in package `0.16.0-1.4`. |
| `mgpu-cpu-copy-0.16.0` | The three commits on top of upstream `v0.16.0` (`libaquamarine.so.15`, needed by Hyprland 0.57.1). Source of the patches in this branch. |
| `v0.16.0-mgpu-1` | The exact code in package `0.16.0-1.1`. |
| `mgpu-cpu-copy-0.15.1` | The three commits on top of upstream `v0.15.1`. |
| `v0.15.1-mgpu-1.3` | The exact code in package `0.15.1-1.3`. |
| `mgpu-cpu-copy-main` | The same three commits on top of upstream `main` (after #279). Daily-driven from 2026-10-31 as a `0.15.1.r13` build, until `v0.16.0` was released on that same base. |
| `packaging` | This branch. |

Next step for `mgpu-cpu-copy-main`: reuse the dumb allocator and the CPU-copy fallback that
[#279](https://github.com/hyprwm/aquamarine/pull/279) added for evdi, to make the diff smaller and upstreamable.

## Build and install (Arch Linux)

```bash
git clone -b packaging https://github.com/EduardoFockink/aquamarine.git aquamarine-packaging
cd aquamarine-packaging
makepkg -si
sudo install -Dm644 aquamarine-mgpu-cpu-copy.hook /etc/pacman.d/hooks/aquamarine-mgpu-cpu-copy.hook
```

The new library is only picked up by a new compositor session (log out and back in). Keep the previous
package file around: if the session does not come up, switch to a TTY and reinstall it with
`sudo pacman -U <previous>.pkg.tar.zst`, or install the official package with `sudo pacman -S aquamarine`.

[`tools/safe-test.sh`](tools/safe-test.sh) automates that: armed before logging out, it rolls back to the
previous package and restarts the display manager unless the new login is confirmed within 120 s, so a
build that leaves the screen unusable fixes itself. The confirmation is a zenity dialog opened in the new
session (plus a notification): a notification alone is dropped by the shell's "do not disturb", which on
2026-11-13 rolled back a build that was working.
It also survives a reboot: the armed state is written to `/var/lib/aq-safe-test` and a boot-time unit
(`aq-safe-test-resume.service`) resumes it; `safe-test.sh disarm` removes both.

It watches the **newest** compositor and treats as new any compositor that was not running when it was armed,
and its rollback kills those before restarting the display manager. Until 2026-11-13 it took the first PID: a
stuck compositor left by an earlier failed try was still alive, the script waited for *that* one to exit, never
saw the real logout, and nothing rolled back. A warning now lists stale compositors; reboot before testing if
there are any.

A second net lives in the compositor config, not in the package: Hyprland only sets `AQ_MGPU_GPU_COPY` when
`~/.config/hypr/mgpu-gpu-copy-ok` exists, or for one login when `~/.config/hypr/mgpu-gpu-copy-trial` exists (the
config deletes it as soon as it reads it). A session with no picture then recovers with a plain reboot, back on
the CPU copy, with nothing to uninstall.

**The mgpu module (2026-11-13, package `0.16.0.r8.g7c1c55b-2`).** Both paths now live in their own directory,
`src/backend/drm/mgpu/` (`MgpuCopy` = per-output state and fallback, `VkCopy` = GPU copy, `CpuCopy` = CPU copy), and
upstream files only carry hooks: 36 added lines (`DRM.cpp` 19, `GBM.cpp` 12, three `friend` declarations, the
`vulkan` dependency) instead of ~560 lines inside `commitState()` and `readBuffer()`. One patch,
[`main/0001-drm-mgpu-copy-module-*.patch`](main/), applies to both v0.16.0 and main (checked: it builds on both).
Code: branch `mgpu-module-main`, worktree `../aquamarine-mgpu` (uncommitted). New with it:

- one damage journal shared by both engines;
- a GPU-copy failure switches that output to the CPU copy and is retried after a growing pause (30 s, 2 min, 8 min,
  32 min, then hourly) instead of staying off until the output is reset;
- counters in the trace every 1000 frames (CPU time of `copy()`, sampled GPU copy time, late fences, failures,
  CPU-copied frames);
- `AQ_MGPU_GPU_COPY_VERIFY=<n>`: every n-th frame, waits for the copy and compares 16 blocks of 64x64 pixels of the
  scanout buffer with the rendered frame (12 inside what the frame copied, 4 anywhere); a difference is traced as
  `VERIFY MISMATCH`. Report only. It costs 10-35 ms per check (the scanout memory is not CPU-cached: ~85 MiB/s), so
  it is for trial sessions. Why it exists: KWin 6.8 does the same Vulkan copy and disables the single-copy variant
  for NVIDIA sources because of unexplained glitches on Intel-connected monitors (KDE bug 527163), visible on the
  panel but not in screenshots.

**Snapshot of upstream main** ([`main/`](main/), packages `0.16.0.r8.g7c1c55b-1` and `-2`, 2026-11-13): `-1` is the
four patches (kept in `main/patches-r8-1/`), `-2` the module patch, on top of upstream `main` at 7c1c55b (v0.16.0 + 8 commits: #477 no all-zero HDR blob for SDR, #478 CRTC
recheck, #484 GBM performance regression, #485 async test). `-1` was built from a local rebase of the CPU-copy
commits plus the GPU-copy diff (not pushed; its patches are in `main/patches-r8-1/`). #484 now forces LINEAR on multi-GPU buffers whenever the
renderer lists it, and the renderer now always lists it; the `AQ_MGPU_CPU_COPY` hunk in `GBM.cpp` still clears the
modifiers after that, so the NVIDIA keeps rendering in its native layout. The version sorts above `0.16.0-1.4` and
below `0.16.1`, so the next release replaces it.

To refresh the patches after changing a code branch: `./export-patches.sh v0.16.0 mgpu-cpu-copy-0.16.0`,
then `updpkgsums`.

## Tests

Standalone programs in [`tests/`](tests/) that only use render nodes, so they run next to a graphical
session without touching it. See [`tests/README.md`](tests/README.md).

## Upstream

- [#279](https://github.com/hyprwm/aquamarine/pull/279) (merged 2026-10-30): CPU-copy fallback for evdi.
  Touches the same functions as this patch set, which therefore needs a rebase for the next release.
- [#408](https://github.com/hyprwm/aquamarine/issues/408) (open): NVIDIA primary with an AMD secondary,
  every frame through a CPU readback, ~50% of a core with video. Closest reported case.

## License

BSD-3-Clause, same as aquamarine. See the upstream `LICENSE`.
