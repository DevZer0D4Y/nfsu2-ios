# NFSU2 for iPhone and iPad

Unofficial native port of the original Xbox **Need for Speed: Underground 2**, based on [myUdav4iik/nfsu2-ios](https://github.com/myUdav4iik/nfsu2-ios) and [antoxa2584x/nfsu2-sw](https://github.com/antoxa2584x/nfsu2-sw). Uses static recompilation and a native Metal renderer; no JIT is required.

This repository contains source and build tools. **Game discs, extracted game files, translated game-code output, signing credentials and music are not included.** Supply your own disc. The icon artwork is the supplied game cover; game names/artwork belong to their respective owners. This project is not affiliated with EA or Microsoft.

## Playing

- Import your Xbox `.iso` or an extracted folder through the app's Files picker.
- The read directory is **Files → On My iPhone/iPad → NFSU2 → game** (`Documents/game` in the app container). Put `default.xbe` and `NFSUNDER/` directly inside `game`, without another enclosing folder. **Edit → Game files → Extracted game directory** also shows the active read path and compiled region profile.
- The ISO filesystem reader handles discs independently of region. **Executable compatibility is separate:** each build contains a translation for a specific executable. PAL Europe and NTSC-U profiles are documented in [Region support](docs/REGIONS.md). A PAL build cannot run an arbitrary NTSC/Japanese executable merely by accepting its ISO.
- PAL controls automatically switch between Drive and Menu for gameplay and pause/options/frontend. A manual layout toggle remains available. **Edit** customizes the two layouts independently: button positions, size, opacity, visibility and mappings.
- SDL-supported physical controllers take over from the touch pad; disconnecting restores touch input.
- **Edit → My Music → Add MP3s from Files** imports your tracks. Use the **My Music** switch, **Next Track** and playlist volume. To hear only your playlist with engine sounds, lower the original soundtrack's Music volume in the game's audio options.
- The image fills the screen. Presentation defaults to uncapped; iOS defaults to 4× internal width/height (16× the pixels). Actual performance depends on hardware, scene and heat.

Saves stay in `Documents/game/UDATA` and `TDATA`. Import preserves existing saves and keeps the previous installation until the replacement succeeds. Backgrounding pauses the game, audio, custom music and GPU work.

## Settings

Optional overrides go in `Documents/nfsu2x_env.txt`, one `KEY=VALUE` per line:

| Setting | Effect |
|---|---|
| `RECOMP_GL_SCALE=4` | Internal render scale; 4 is the iOS default. Lower values reduce GPU work. |
| `NFSU2_FPS_LIMIT=0` | Uncapped presentation; positive values up to 1000 set a ceiling. |
| `NFSU2_SCREEN=fit` | Preserve aspect ratio instead of filling the screen. |
| `NFSU2_AUTO_CONTROLS=0` | Use manual Menu/Drive selection. |
| `NFSU2_TOUCH=2` | Keep touch graphics visible with a physical controller. |
| `RECOMP_FPS_LOG=1` | Log FPS, GPU command timings, CPU, thermal and audio diagnostics. |

## Build and validation

See [iOS build instructions](docs/IOS_BUILD.md) and [region profiles](docs/REGIONS.md). The default bundle ID is `com.devz.nfsu2`; override `NFSU2_IOS_BUNDLE_ID` and select your own signing team when building. Native VP6 movies use separately linked LGPL FFmpeg frameworks.

Source-level regressions are in `tests/apple_touch`, `tests/metal_bindings` and `tests/xiso`. They use synthetic fixtures; no retail disc or music is required. The Metal checks validate GPU pixels, encoder state, feedback snapshots and transform constants at native and 4× scale. CPU submission microbenchmarks do not establish an iPhone gameplay FPS gain.

Preserve the licenses included in this repository and its vendored components.
