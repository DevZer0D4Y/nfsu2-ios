# NFSU2 iOS — Need for Speed: Underground 2 on iPhone/iPad (Metal)

Unofficial fan port of the **original Xbox (NTSC-U) release of NFSU2** to iOS/iPadOS,
built on top of `antoxa2584x/nfsu2-sw` (Xbox static recompilation via `xboxrecomp`).
The game's code is already lifted to C and runs natively on Linux and Switch.
**This fork adds an iOS host and, eventually, a native Metal renderer.**

No game data is ever included or committed. The user supplies their own extracted disc
(`default.xbe`, `NFSUNDER/`, ...).

## Upstream notes — read first

Upstream's engineering notes were moved to `docs/UPSTREAM_NOTES.md`, alongside
`PERF_NOTES.md` and `VULKAN_NOTES.md`. They explain the kernel, GIL, NV2A pushbuffer,
APU and translator quirks. Read the relevant one before touching those systems.

@docs/UPSTREAM_NOTES.md

## Goals, in order

1. **Phase 0, macOS bring-up.** Build and run the existing Linux SDL2 path on an Apple
   Silicon Mac (arm64, OpenGL or Vulkan via MoltenVK). This proves the lifted C and
   runtime work on Apple arm64 before iOS adds sandboxing and memory limits.
2. **Phase 1, iOS boot.** Xcode project, iOS arm64, SDL2 for window/input/audio,
   the existing Vulkan renderer on MoltenVK. Milestone: intro movie and main menu
   on a real iPhone.
3. **Phase 2, playable.** Controller via GameController/SDL, touch overlay, saves,
   game-file import through the Files app, sensible defaults for memory and threads.
4. **Phase 3, native Metal renderer.** A new NV2A backend (`nv2a_metal`) alongside the
   GL and Vulkan ones, consuming the same pushbuffer/state, with shaders in MSL.
   MoltenVK stays as a fallback until Metal matches it.
5. **Later:** reuse the host for Carbon (`nfsuc-sw` shares the XDK 5849 runtime).

Don't jump ahead. Each phase must boot on the device before the next starts.

## Ground rules

- **Never commit game data or lifted output.** `gen/`, `*.xbe`, `NFSUNDER/`, `TDATA/`,
  `UDATA/`, `progcache.bin` and dumps are gitignored and must stay that way.
- **Keep upstream mergeable.** Put iOS code in new files (`src/ios/`, `xboxrecomp/.../ios_*`,
  `ios/` for the Xcode project). Changes to shared files must be small and wrapped in
  `#if defined(__APPLE__)` / `#if TARGET_OS_IOS`. Never reformat upstream files.
- **Keep the `NFSU2_` / `RECOMP_` env var names.** On iOS they're read from
  `Documents/nfsu2x_env.txt` (same `KEY=VALUE` format as the Switch build).
- **Don't hand-edit lifted code.** Fixes go in the translator, `config/seed_functions.json`,
  or `src/recomp_manual.c` overrides, as upstream does.
- **Don't change code you haven't confirmed is involved.** When stuck, add logging or a
  `RECOMP_*` trace, reproduce, and explain the cause before patching.
- **Validate on macOS first** when possible. It's faster to debug than the device, and
  ASan/UBSan work there.

## iOS constraints to respect

- **No JIT, W^X.** Static recompilation is fine. Never generate or `mprotect` executable
  code at runtime.
- **Guest memory.** The Xbox has 64 MB of RAM at fixed low addresses. Check how the
  runtime maps guest memory (direct mapping vs. base + offset). iOS may refuse
  fixed low-address `mmap`. If so, switch to a base-pointer translation behind a macro
  and measure the cost.
- **Memory limit.** Use the `com.apple.developer.kernel.increased-memory-limit`
  entitlement. Keep the texture cache budget (`RECOMP_GL_TEX_MB`) conservative.
- **Sandbox paths.** The game directory is `Documents/game/`. Saves (`UDATA`), logs and
  the shader cache go in `Documents/` or `Library/Caches/`. No absolute `/root/...` defaults.
- **Lifecycle.** On background, pause the guest (hold the GIL, stop audio) and never
  issue Metal/GL work while backgrounded, or the app will be killed.
- **Threads.** Map Switch thread-priority options to QoS classes. Keep audio threads
  above game threads.
- **Movies.** VP6 FMVs go through FFmpeg (`NFSU2_NATIVE_VP6`). Build an LGPL, VP6-only
  FFmpeg for iOS arm64 (adapt `tools/build_ffmpeg_vp6.sh`) and keep it dynamically
  linked for LGPL compliance.

## Build (target workflow — update as it becomes real)

```sh
# Lift the XBE to C, done once on the Mac
NFSU2_XBE=/path/to/game/default.xbe NFSU2_GEN_DIR=$PWD/gen tools/regen.sh

# Phase 0: macOS
cmake -S . -B build-macos -G Ninja -DNFSU2_GEN_DIR=$PWD/gen
cmake --build build-macos
NFSU2_GAME_DIR=/path/to/game build-macos/nfsu2_recomp

# Phase 1+: iOS
cmake -S . -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS \
      -DCMAKE_OSX_ARCHITECTURES=arm64 -DNFSU2_GEN_DIR=$PWD/gen
# then build/sign in Xcode with a personal team
```

The lifted C is huge, so expect long compile times. Use `LIFT_ONLY=1` when only the
translator or `recomp_manual.c` changed.

## Code style

- C11 for the runtime, matching upstream style (indent, naming, comments).
- Objective-C (`.m`) only where Apple APIs need it (Metal setup, UIKit lifecycle,
  file import). Keep it thin and expose plain C functions to the runtime.
- Swift/SwiftUI is allowed for the launcher UI only (game-folder picker, settings),
  never in the game loop.
- Metal: one `MTLDevice`, command queue and `CAMetalLayer`. Triple-buffer per-frame
  data. Cache pipeline states keyed on NV2A state, mirroring `RECOMP_PROG_CACHE`.

## Working with me

- Before a non-trivial change, state the plan in a few lines: which files and why.
- After a change, say how it was verified (built for which target, booted to where,
  which log lines confirm it).
- Track progress and findings in `docs/IOS_PORT_LOG.md`: dated entries, what worked,
  what broke, open questions. This is the project's memory between sessions.
- If a step needs something only the user can do (Apple signing, extracting the disc,
  testing on device), stop and say exactly what's needed.

## Legal

Unofficial fan project, not affiliated with or endorsed by EA or Microsoft. The user
must own the game. No game assets, no lifted code and no keys go in the repo or any
release. Preserve upstream licenses (xboxrecomp is MIT; check the repo's own license).
