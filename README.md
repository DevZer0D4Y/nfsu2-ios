# NFSU2 for iPhone and iPad

Unofficial native port of the original Xbox **Need for Speed: Underground 2**, based on [myUdav4iik/nfsu2-ios](https://github.com/myUdav4iik/nfsu2-ios) and [antoxa2584x/nfsu2-sw](https://github.com/antoxa2584x/nfsu2-sw). Uses static recompilation and a native Metal renderer; no JIT is required.

**[Download the IPA](dist/NFSU2-PAL-0.3-unsigned.ipa)** · [Changelog](CHANGELOG.md) · [Region support](docs/REGIONS.md) · [Build guide](docs/IOS_BUILD.md)

This repository contains source and build tools. **Game discs, extracted game files, translated game-code output, signing credentials and music are not included.** Supply your own disc. The icon artwork is the supplied game cover; game names/artwork belong to their respective owners. This project is not affiliated with EA or Microsoft.

## Changes made for this port

### Display and frame pacing

- Fullscreen landscape presentation on iPhone and iPad, with an optional aspect-fit mode. The macOS host also supports fullscreen and the F11 shortcut.
- Uncapped presentation by default, replacing the earlier 60 FPS ceiling. Set `NFSU2_FPS_LIMIT` to a positive value up to 1000 to select a ceiling, or `0` for uncapped. This removes the port's limiter; it does not guarantee a particular FPS or bypass the display's refresh rate.
- PAL presentation uses an immediate-flip override while preserving the game's buffer and fence flags.
- iOS defaults to **4× internal width and height**, or 16× the original pixel count. Lower `RECOMP_GL_SCALE` to reduce GPU load. This is internal rendering resolution, not a texture replacement pack.
- ProMotion and Game Mode opt-in metadata are included. The device and operating system determine their actual behavior.

### Touch controls and controllers

- Racing-style gameplay controls inspired by the supplied reference: left/right steering on the left and pedal/action controls on the right, with secondary driving actions available in the layout.
- A separate simple Menu layout for navigation and confirmation, plus a manual Menu/Drive toggle.
- The PAL build reads game state to switch to Drive during gameplay and back to Menu for pause/options/frontend screens. A manual choice persists until the next detected state transition. Unsupported executable profiles do not inherit PAL-specific state addresses.
- **Edit** customizes Menu and Drive independently: drag controls, change size and opacity, show or hide buttons, and change input mappings. Save, Cancel and Reset keep edits deliberate and the two profiles separate.
- Layouts adapt to phone/tablet sizes and safe areas, with at least 44-point touch targets. Multiple held inputs, sliding between controls and cancellation are handled.
- Changing layouts, opening the editor, backgrounding or disconnecting input releases held actions so steering or acceleration cannot remain stuck.
- SDL-supported physical controllers take over from the touch pad. Disconnecting restores neutral touch input; the editor remains accessible while a controller is connected. `NFSU2_TOUCH=2` keeps the touch graphics visible with a controller.
- The game and audio pause while the layout editor is open and while the app is in the background; GPU submissions are also suspended in the background.

### Personal MP3 soundtrack

- **Edit → My Music → Add MP3s from Files** imports one or several tracks into the app's `Documents/Music` directory.
- A **My Music** switch enables or disables playback; **Next Track** and playlist volume control the persistent playlist. Tracks advance and loop through the playlist.
- Imports use coordinated access to Files providers, validate tracks before publishing them, preserve duplicate imports under distinct names and skip invalid tracks during playback.
- Playback responds to app backgrounding, audio interruptions and headphone removal.
- Your playlist plays over game audio. Lower the game's original Music volume in its audio options to retain engine/effect sounds without the original soundtrack. MP3s are supplied by you and are not included in the app download.

### Disc import, regions and saves

- Streaming Xbox ISO extraction supports bare XDVDFS images and supported full-disc partition layouts, independently of the disc's country.
- The importer checks filesystem bounds and paths, preflights the executable before a large extraction, and uses buffered streaming rather than loading the whole ISO into memory.
- Replacement imports are staged and published with rollback/recovery, preserving existing `UDATA` and `TDATA` saves and retaining the previous installation until replacement succeeds.
- Added a verified **European PAL** executable profile, generation tools and native overrides alongside the existing NTSC-U profile. Known mismatched executables receive an explanation instead of being launched with the wrong translation.
- Each app build contains **one translated executable**. The committed IPA is PAL Europe; arbitrary NTSC, Japanese and other revisions still require their own verified translation. See [region hashes and build instructions](docs/REGIONS.md).
- **Edit → Game files → Extracted game directory** shows the effective read directory and compiled profile, with a Copy Path action. The default is **Files → On My iPhone/iPad → NFSU2 → game**, with `default.xbe` and `NFSUNDER/` directly inside it.

### Rendering and CPU optimizations

- Cache Metal vertex/fragment uniform bindings, transform-constant bindings, textures and samplers within a valid encoder lifetime. Invalidate the caches when a new encoder/pass requires fresh bindings.
- Reuse unchanged 192-row transform-constant uploads within the same command buffer. Changed constants, untracked generations and instanced draws retain fresh allocations; uploads never carry over to a new command buffer.
- Share a pre-draw feedback snapshot between matching texture stages, reducing repeated copies within a draw while preserving a new snapshot for each draw.
- Use a render-pass load-action clear for full-surface clears, avoiding the previous load and clear-quad work. Partial clears and clears inside an active pass retain their appropriate behavior.
- Add native PAL CRT, VP6/video-conversion fast paths and use `-O2` for Apple generated code. Optional Clang thin-LTO support remains a build-time choice.
- Collect asynchronous GPU completion timings, plus CPU, thermal and audio diagnostics through `RECOMP_FPS_LOG`, without adding a GPU wait to measure completion.
- Keep changes scoped to their relevant Apple/rendering paths rather than replacing the other platform backends.

### App identity and public distribution

- Supplied NFSU2 game-cover icon, display name **NFSU2**, bundle identifier **`com.devz.nfsu2`**, version **0.3**, build **2**.
- Public source excludes game discs/assets, generated game code, personal music, signing credentials and local developer logs/notes. Source test fixtures are synthetic.
- The committed unsigned IPA includes a SHA-256 checksum. Packaging removes provisioning profiles and code signatures, and replaces FFmpeg's informational local configure prefix without changing Mach-O offsets. Sign the app for your own device before installation.
- Added the [changelog](CHANGELOG.md), [region guide](docs/REGIONS.md), updated [build guide](docs/IOS_BUILD.md) and the support/community links below.

## Validation and current limits

- Seven input and frame-pacing tests passed, covering uncapped and selected capped rates.
- UIKit checks cover six phone/tablet geometries, minimum target sizes, customization, Save/Cancel/Reset, controller handoff, held-input release, automatic layout transitions, import rollback and save preservation.
- MP3 checks cover decoding/playback, imports, duplicates, playlist persistence, switching, volume and interruption/lifecycle handling. XISO tests use synthetic images, including malformed inputs.
- Metal GPU checks validate pixels, encoder state, clears, feedback snapshots and transform constants at native and 4× scale, including changed constants and instanced draws.
- Installed and launched on an **iPhone 16 Pro Max**. All **191 transferred disc files** matched their source sizes and SHA-1 checksums on-device. Earlier testing on an iPhone 14 Pro Max confirmed that the PAL game and touch controls ran.
- A synthetic constant-upload workload reduced CPU submission time by roughly **7–9%**. This is not a measured improvement to gameplay FPS. Sustained race performance and real race/pause/options layout transitions still need device validation; automated transitions passing does not establish every in-game screen is covered.
- At 4× scale, demanding scenes may still be GPU-limited. Uncapped presentation does not promise 60 or 120 FPS. Reduce the internal scale if needed.

## Requirements and installation

1. Download [NFSU2-PAL-0.3-unsigned.ipa](dist/NFSU2-PAL-0.3-unsigned.ipa) and [its SHA-256 checksum](dist/NFSU2-PAL-0.3-unsigned.ipa.sha256).
2. Sign and install the IPA using your preferred sideloading tool, such as AltStore, SideStore, Sideloadly or ESign. The public IPA contains no developer signature or provisioning profile.
3. Supply your own original Xbox **European PAL** disc image matching the [supported executable](docs/REGIONS.md). The current IPA does not run arbitrary regional executables.
4. Launch **NFSU2** and select your ISO, or copy the extracted files into the directory below. Keep enough free space for both your ISO and its extracted contents during import; the verified European disc extracts to about 4.45 GB.

The app targets iPhone and iPad on iOS/iPadOS 16 or later; the native Metal path requires iOS 17 or later. Performance varies with the device, resolution and temperature. Lower `RECOMP_GL_SCALE` if 4× is too demanding.

The bundle identifier is **`com.devz.nfsu2`**. Keep the same bundle ID and signing identity when updating to retain app data. Builds installed with an older identifier have separate storage; back up their game files and saves before changing apps.

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

## Credits

- [myUdav4iik/nfsu2-ios](https://github.com/myUdav4iik/nfsu2-ios), for the original iOS/macOS port.
- [antoxa2584x/nfsu2-sw](https://github.com/antoxa2584x/nfsu2-sw), for the Xbox static recompilation project and toolkit.
- SDL, FFmpeg and the contributors to the included rendering and runtime components. Their existing licenses and notices continue to apply.
- Electronic Arts and the original developers, for Need for Speed: Underground 2. Game content and cover artwork remain their owners' property.

## Support me

If you want to support me and stay up to date with this and my other projects:

[Ko-Fi](https://ko-fi.com/dev_zer0) · [Discord](https://discord.gg/uFChheZEWX) · [YouTube](https://www.youtube.com/@develop_erZ)
