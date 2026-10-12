# Third-party code in the Apple builds (macOS, iOS, iPadOS)

The Apple host and Metal renderer of this fork are built from this repository plus the libraries
below. Each keeps its own licence and copyright. This file lists them; it does not replace their
licence texts, which ship with their sources (paths given). No game data, lifted code (`gen/`) or
keys are part of this repository or of anything built from it for sharing.

## In this repository

| Component | Licence | Notes |
|---|---|---|
| `xboxrecomp/` (static recompilation toolkit, runtime, renderers) | MIT, `xboxrecomp/LICENSE` | © 2026 sp00nz and contributors (`xboxrecomp/CONTRIBUTORS.md`) |
| MCPX APU sources and `nv2a_regs.h` inside `xboxrecomp/src/apu`, `src/nv2a` | LGPL-2.1-or-later, `xboxrecomp/LICENSES/LGPL-2.1.txt` | extracted from xemu; © espes, Jannik Vogel, Matt Borgerson. Details in `xboxrecomp/NOTICE` |
| The NFSU2 port (`src/`, `tools/`, `config/`) | none stated upstream | from [antoxa2584x/nfsu2-sw](https://github.com/antoxa2584x/nfsu2-sw) by Anton Artemov, which has no licence file: all rights reserved by its author unless they say otherwise |
| The Apple/iOS additions of this fork (`src/apple`, `src/ios`, `xboxrecomp/src/nv2a_metal`, `xboxrecomp/src/platform/apple_qos.c`, `ios_vm.c`, `cmake/`, `tools/build_*apple*.sh`, `tools/build_ios_deps.sh`) | follow the files they sit beside (MIT inside `xboxrecomp/`) | |

## Libraries linked into the app

| Library | Version | Licence | Linked | Licence text |
|---|---|---|---|---|
| SDL2 | 2.32.10 | zlib | statically | `LICENSE.txt` in the SDL2 source |
| MoltenVK (with SPIRV-Cross and the Vulkan headers) | 1.4.2 | Apache-2.0 | statically (iOS); Homebrew's dynamic library on macOS | `LICENSE` in the MoltenVK release |
| glslang | 16.6.0 | BSD-3-Clause (core), BSD-2-Clause, MIT, Apache-2.0 for some files | statically | `LICENSE.txt` and `LICENSES/` in the glslang source |
| FFmpeg (libavcodec, libavutil; VP6 decoder only) | 7.1.1 | LGPL-2.1-or-later | **dynamically**, as `avcodec.framework` / `avutil.framework` inside the app | `FFmpeg-COPYING.LGPLv2.1`, shipped in the app bundle |
| Apple frameworks (Metal, QuartzCore, UIKit, AppKit, ...) | system | Apple SDK terms | system | — |

FFmpeg is built by `tools/build_ffmpeg_apple.sh` from the unmodified 7.1.1 release
(`https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz`, sha256
`733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1`) with
`--disable-everything --enable-decoder=vp6` and no GPL or non-free parts (`License: LGPL version 2.1
or later` in its configure output). It stays a separate dynamic library, so it can be replaced with
another build of the same version and configuration. The xemu-derived APU code is linked statically;
its source is in this repository, which is what the LGPL asks for that.

## Trademarks and game content

Need for Speed and Need for Speed: Underground 2 are trademarks of Electronic Arts; Xbox is a
trademark of Microsoft. This is an unofficial fan project, not affiliated with or endorsed by either.
You must own the game; the build reads your own copy of the disc and nothing else.
