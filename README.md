# aquamarine + AQ_MGPU_CPU_COPY

A small patch set for [aquamarine](https://github.com/hyprwm/aquamarine) (the rendering backend of
Hyprland) that makes **"NVIDIA renders, Intel scans out"** work on hybrid laptops where the external
display is wired to the Intel iGPU, plus the Arch Linux packaging and the standalone tests behind it.

This is a personal fork, not an upstream feature. This `packaging` branch holds the PKGBUILD, the patches
exported from the code branches, a pacman hook, and tests; the code lives in the branches listed below.

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

### Environment variables

| Variable | Effect |
| --- | --- |
| `AQ_MGPU_CPU_COPY=1` | Enables the CPU copy path for outputs on a secondary GPU. |
| `AQ_MGPU_CPU_COPY_FULL=1` | Debug: copy the whole frame every time, ignoring damage. |
| `AQ_MGPU_CPU_COPY_SPIN=1` | Debug: skip the fence wait (behaviour before commit 3), for A/B measurements. |

Compositor environment used with it (Hyprland, NVIDIA = `card0`, Intel = `card1`):

```lua
hl.env("AQ_DRM_DEVICES", "/dev/dri/card0:/dev/dri/card1") -- first renders, second scans out
hl.env("AQ_MGPU_CPU_COPY", "1")
hl.env("AQ_FORCE_LINEAR_BLIT", "0") -- let the NVIDIA render in its native layout
hl.env("AQ_NO_MODIFIERS", "1")      -- UHD 630: Y-tiled planes max out at 4096 px wide; 5120 needs X-tiled
```

### Marker files

The package installs `/usr/share/aquamarine/mgpu-cpu-copy` (8-bit copy available) and
`/usr/share/aquamarine/mgpu-cpu-copy-10bit` (10-bit too). A compositor config can test them before enabling
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
| `mgpu-cpu-copy-0.15.1` | The three commits on top of upstream `v0.15.1`. |
| `v0.15.1-mgpu-1.3` | The exact code in package `0.15.1-1.3`. |
| `packaging` | This branch. |

Planned: `mgpu-cpu-copy-main`, the same feature rebuilt on top of upstream `main`, reusing the CPU-copy
fallback that [#279](https://github.com/hyprwm/aquamarine/pull/279) added for evdi.

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

To refresh the patches after changing a code branch: `./export-patches.sh v0.15.1 mgpu-cpu-copy-0.15.1`,
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
