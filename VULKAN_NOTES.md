# Vulkan renderer (nfsu2x-vulkan.nro) — state and next steps (2026-09-29)

## State

- `xboxrecomp/src/nv2a_vk/nv2a_vk.c`, built with `NFSU2_VULKAN=ON`
  (`VULKAN=1 bash switch/build.sh` -> `switch/nfsu2x/nfsu2x-vulkan.nro`).
  Setup, driver build and debugging switches: CLAUDE.md, "Vulkan build".
- Runs on the console (first hardware run 2026-09-29): race fps about the
  same as the GL build. Expected: the GL build was already limited by the
  game's main thread and the executor, not by the GL calls alone, and this
  first version records every draw on the executor thread (no render thread).
- Linux (lavapipe, validation layer on): boot, movies, menus and a Quick Race
  match the GL renderer's frames.
- Eden cannot run it (no NVK GPU submission ever completes there).

## Next steps, in order

1. **Measure first.** Add `[perf]` lines for the Vulkan build: draws and
   pipeline binds per frame, pass restarts (begin_rendering), barriers, ring
   MB per frame, texture uploads, and executor CPU per draw. Profile a race
   with `RECOMP_NX_PROFILE=1` and the matching ELF
   (`/root/nfsu2x/elf_archive/nfsu2x-vulkan_<md5>.elf`) to see how the
   executor thread splits between decode, our recording and NVK.
2. **A render thread** (like `RECOMP_GL_THREAD`). The GL queue (q_* in
   nv2a_gl.c) already copies everything a draw needs; move it into a shared
   file and replay into nv2a_vk on its own thread. This is what took the GL
   build from executor-bound to game-bound on the console.
3. **Less per-draw work:**
   - vertex constants: 3 KB `c[192]` copied into the ring for every draw.
     Reuse the previous block when `vp_const_gen` is unchanged (same frame),
     and split it from the small per-draw uniforms (surf, aa, m, vpoff);
   - `vkCmdSetVertexInputEXT` and 16 vertex-buffer binds every draw: cache the
     layout (hash of formats/strides/present mask) and set only on change;
   - push descriptors every draw: skip when textures and UBO offsets are
     unchanged;
   - fragment block: rebuild only when the combiner constants / fog / alpha
     registers changed.
4. **Fewer pass breaks and barriers.** Today every target switch and every
   texture upload ends the rendering pass and issues a full memory barrier
   (`barrier_all`). Track hazards per image (written as attachment ->
   sampled; sampled -> written) and barrier only those; do texture uploads
   before the pass that needs them (collect in a pre-pass command buffer
   submitted first) instead of splitting the pass.
5. **Shader and pipeline caching.** DONE (2026-09-29): progcache.bin (shared with GL),
   vkspirv.bin, vkpipes.bin, vkpipecache.bin next to the NRO; prewarm at boot
   (`[VK] prewarm:` line). Linux: race-start compiles 46 -> 1. Original plan: Save `VkPipelineCache` to
   `sdmc:/switch/nfsu2x/vkpipe.bin` at exit and every few minutes, and
   precompile at boot from the program records like `progcache.bin` (the
   GL `prog_cache_*` code), including the pipeline variants seen. Cache the
   SPIR-V too, so glslang does not run again. Watch `[hitch]` lines at race
   start.
6. **NVK specifics** (StevensND/nfsmw-nx docs/mesa.md, native-renderer.md):
   - ZCULL: the log says "ZCULL ... usos incompatibles (TRANSFER_DST?)" --
     our depth images ask for TRANSFER_DST only for the initial
     `vkCmdClearDepthStencilImage`. Clear them with loadOp CLEAR on first use
     instead and drop TRANSFER_DST, so NVK can enable ZCULL (they measured
     ~2 ms GPU per race frame);
   - uniforms already come from UBOs (NVK maps them to constant memory);
   - try `NVK_SWITCH_CPU_WRITE_MEM_UNCACHED` (default on) and the set-4 /
     draw shortcuts only if the NVK part of the profile is large.
7. **Merge draws with identical state** (consecutive draws, same pipeline,
   textures, uniforms): one indexed draw with concatenated vertex ranges.
8. **Present:** check the pacing on the console (immediate vs FIFO; 2 vs 3
   frames in flight); the swapchain is 1280x720 -- use 1920x1080 when docked.
9. **Missing features** (not performance): loading screen,
   clears with a partial colour mask, RECOMP_GL_WATCH.
10. **Build:** LTO and PGO for the lifted code (PERF_NOTES.md items 3-4) apply
    to both builds.

## Measuring

Console only (Eden cannot run it; lavapipe says nothing about speed). Same
race, same `nfsu2x_env.txt`, GL vs Vulkan NRO; compare `[perf]` fps, the
`% of a core per thread` line and `[hitch]` lines.
