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
| `vk_dmabuf_export.c` | Can the NVIDIA's Vulkan export a LINEAR system-RAM buffer, copy a frame into it with its copy engine, and can the Intel import it, read it and make it a KMS framebuffer? How much CPU/GPU per frame? (yes; 3.65 ms GPU, 0.04 ms CPU) |
| `vk_gbm_frame_copy.c` | Full chain for AQ_MGPU_GPU_COPY: GL frame in a native NVIDIA GBM buffer -> Vulkan import with the DRM modifier -> copy waiting on the GL fence -> Intel waits on the Vulkan sync_file. 8 and 10 bit. |
| `vkcopy_class_check.cpp` | The mgpu module's classes from the built library (`src/backend/drm/mgpu`): `CMgpuVkCopy` full and damage-only copies land on the right rows/columns, in 8 and 10 bit, `IBuffer::size` equal to `dmabuf().size` (0x0 was the 0.16.0-1.3 black screen), the CPU time of `copy()`; and `mgpuVerifySamples` against a real `CDRMRenderer`: no difference on a correct copy, a rect that changed without being copied is reported. `READ_SPEED=1` times a CPU read of the whole scanout buffer. Build line inside. |
| `gl_shm_upload_cost.c` | What a software-rendered client costs the compositor: CPU time of `glTexSubImage2D` on the NVIDIA for a 5117x2157 shm buffer (3.8 ms, 18% of a core at 48 Hz) and for smaller rects. |
| `readback_cpu_cost.cpp` | CPU cost per frame of the readback into a real Intel dumb buffer: synchronous vs. PBO, with an idle or busy GPU, and waiting on a native fence first. Basis of commit 3. |

`pack_shader.inc` is the shader source extracted from the patch, shared by `pack_shader_check.cpp`.
| `x_mudo.py` | A fake X server that accepts connections and never answers, like a compositor's lazy Xwayland while the compositor is blocked. With `DISPLAY=:99`, `vkcopy_class_check` proves Vulkan setup does not touch `$DISPLAY` (the 0.16.0-1.2 login deadlock). |
