<div align="center">

<img src="https://cdn2.steamgriddb.com/logo/4b29fa4efe4fb7bc667c7b301b74d52d.png" alt="Need for Speed: Underground 2" width="520">

### On iPhone, iPad and Mac

The original Xbox (NTSC-U) release of **Need for Speed: Underground 2**, recompiled to native code
and running on Apple devices with a native **Metal** renderer. No emulator.

![iOS](https://img.shields.io/badge/iOS-16%2B-000000?logo=apple&logoColor=white)
![iPadOS](https://img.shields.io/badge/iPadOS-16%2B-000000?logo=apple&logoColor=white)
![macOS](https://img.shields.io/badge/macOS-Apple%20silicon-000000?logo=apple&logoColor=white)
![Metal](https://img.shields.io/badge/Metal-renderer-8A8A8A?logo=apple&logoColor=white)

[Features](#features) · [Status](#status) · [Getting started](#getting-started) · [Controls](#controls) · [Credits](#credits)

</div>

---

> [!IMPORTANT]
> **No game data is included, and none may be shared.** You need your own copy of the Xbox disc.
> The game's code is translated on your own Mac from your own disc; the app you build is for you.

This is a fork of [antoxa2584x/nfsu2-sw](https://github.com/antoxa2584x/nfsu2-sw), the static
recompilation of NFSU2 for Nintendo Switch and Linux. This fork adds an Apple host: iPhone, iPad and
macOS. The original README (Switch and Linux) is in
[docs/UPSTREAM_README.md](docs/UPSTREAM_README.md).

## Features

- **Native Metal renderer** (iOS 17+ / macOS 14+), with Vulkan through MoltenVK as the fallback
  (and on iOS 16)
- **Touch controls** on iPhone and iPad: stick, gas and brake pedals, face buttons, haptics
- **Controllers**: DualSense, Xbox and other MFi controllers; the touch controls hide while one is
  connected
- **Mac keyboard** as a controller when no pad is connected
- **Game import in the app**: pick your extracted disc folder; the app checks it and copies it in
- **Smooth movies** decoded with FFmpeg (VP6, LGPL, shipped as separate frameworks)
- **Background pause**: leaving the app pauses the game, its sound and all GPU work
- **Widescreen 16:9**, using the game's own wide mode
- Saves stay with the game, in the app's `Documents/game/UDATA`

## Status

| Device | State |
|---|---|
| iPhone 13 Pro (A15, iOS 27) | Movies, menus (60 fps), Quick Race and Career free-roam (30 fps, the game's own cap) |
| Mac (M1) | The same; keyboard or controller |
| iOS Simulator | Boots to the menus |

Known issues:

- In Career free-roam on the iPhone, radio speech can drop out between words (fine on the Mac).
- The first time a new effect appears after installing, it can stutter briefly while its shader
  compiles; later runs have it ready.
- Not yet tested: long play sessions (heat), iPad.

The full development log, with measurements, is in [docs/IOS_PORT_LOG.md](docs/IOS_PORT_LOG.md).

## Getting started

You need an Apple-silicon Mac with Xcode, your own NFSU2 Xbox disc (NTSC-U) and, for the phone, an
Apple ID signed in to Xcode. In short:

1. Install the tools (Homebrew, Python with capstone).
2. Extract your disc and translate the game's code to C on your Mac (`tools/regen.sh`). This step
   also runs on Windows under WSL2; see
   [No Mac? Lift the code on Windows](docs/IOS_BUILD.md#no-mac-lift-the-code-on-windows).
3. Build the libraries for iOS (`tools/build_ios_deps.sh`, `tools/build_ffmpeg_apple.sh`).
4. Build and install the app from Xcode's command line.
5. Copy your game folder to the phone and pick it in the app.

Every command is in **[docs/IOS_BUILD.md](docs/IOS_BUILD.md)**, including the Mac version.

## Controls

**Touch** (no controller connected): left side of the screen is a floating stick; on the right are
the pedals (gas, brake), A/B/X/Y, White/Black, Start and Back.

**Mac keyboard** (no controller connected):

| Keys | Does |
|---|---|
| W / S | gas / brake, reverse |
| Arrow keys, A / D | steer; the arrows also move through menus |
| Enter | confirm, Press START, pause |
| Esc or Backspace | back |
| Space / Left Shift | X / Y |
| Q / E | White / Black |
| Tab / P | Back / Start |

The in-game controls screen shows what each Xbox button does.

**Settings** go in `nfsu2x_env.txt` (on the phone: Files → NFSU2), one `KEY=VALUE` per line, for
example `NFSU2_RENDERER=vulkan` (MoltenVK instead of Metal), `NFSU2_TOUCH=0` (no touch controls),
`RECOMP_GL_SCALE=1.5` (sharper picture) or `RECOMP_FPS_LOG=1` (performance log). More in
[docs/IOS_BUILD.md](docs/IOS_BUILD.md).

## Credits

- **[antoxa2584x/nfsu2-sw](https://github.com/antoxa2584x/nfsu2-sw)** by Anton Artemov: the NFSU2
  recompilation for Switch and Linux that this fork builds on.
- **[xboxrecomp](https://github.com/sp00nznet/xboxrecomp)** by sp00nz and contributors (MIT): the
  static recompilation toolkit and runtime.
- **[xemu](https://github.com/xemu-project/xemu)**: the Xbox audio processor code (LGPL-2.1).
- **FFmpeg, SDL2, MoltenVK, glslang**: see [docs/APPLE_NOTICES.md](docs/APPLE_NOTICES.md) for every
  library and its licence.

<div align="center">
<sub>Need for Speed and Underground are trademarks of Electronic Arts; Xbox is a trademark of
Microsoft. This is an unofficial fan project, not affiliated with or endorsed by EA or Microsoft.
You must own the game.</sub>
</div>
