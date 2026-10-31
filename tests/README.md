# Standalone tests

Small programs written while developing AQ_MGPU_CPU_COPY. They open only render nodes (no DRM master), so
they run next to a graphical session without touching it. Device paths are hard-coded for the machine
they were written on: `/dev/dri/renderD129` (and `card0`) is the NVIDIA, `card1` / `renderD128` the Intel.
Adjust them before running elsewhere. Each file's first lines explain it and give its `build:` command.

| File | Question it answers |
| --- | --- |
| `import_vs_cpu_copy.c` | Does the regular multi-GPU path (Intel imports the NVIDIA frame) work, and does the alternative (NVIDIA writes into an Intel-allocated LINEAR buffer)? Case `a` reproduces the Mesa abort seen in Hyprland. |
| `readback_10bit_formats.c` | Which `glReadPixels` format/type pairs the NVIDIA driver accepts for `2101010` frames, which DRM layout the bits land in, and how long the readback takes. |
| `readback_10bit_pbo_vs_shader.c` | 10-bit readback through a PBO vs. repacking with a shader into RGBA8 first; checks the result word by word. |
| `pack_shader_check.cpp` | Runs the exact 10-bit repack shader of the patch (`pack_shader.inc`) and checks both `XBGR2101010` and `XRGB2101010` outputs word by word. |
| `readback_cpu_cost.cpp` | CPU cost per frame of the readback into a real Intel dumb buffer: synchronous vs. PBO, with an idle or busy GPU, and waiting on a native fence first. Basis of commit 3. |

`pack_shader.inc` is the shader source extracted from the patch, shared by `pack_shader_check.cpp`.
