# Changelog

## 0.3 — 2026-10-15

### Mobile experience

- Fullscreen landscape play with optional aspect-fit presentation.
- Uncapped presentation by default; optional FPS ceiling through `NFSU2_FPS_LIMIT`.
- 4× internal width and height by default on iOS; adjustable render scale.
- Racing-style touch controls and a separate simple menu layout. Both layouts support independent positioning, size, opacity, visibility and button mappings, with Save, Cancel and Reset.
- Automatic PAL Menu/Drive switching, including pause/options/frontend detection. A manual toggle remains available.
- Physical controllers take over from the touch pad; disconnecting restores neutral touch input.
- MP3 import from Files, persistent playlist, playback switch, volume and Next Track. Lower the original soundtrack's Music volume in-game to hear only your playlist with game effects.
- Background suspension for the game, GPU work and audio.

### Import and distribution

- Streaming Xbox ISO extraction with path/bounds checks, executable preflight, staged replacement and save preservation.
- PAL Europe and NTSC-U executable profiles. Each build contains one matching executable translation; region-neutral ISO extraction does not provide universal regional playback.
- Active extracted directory and compiled profile shown under **Edit → Game files**. Default directory: **Files → On My iPhone/iPad → NFSU2 → game**.
- New game-cover icon, display name **NFSU2**, bundle ID **com.devz.nfsu2**, version **0.3**, build **2**.
- Public unsigned PAL IPA and SHA-256 checksum in `dist/`; sign for your own device before installation.
- Public source cleaned of local developer notes and compiled test binaries. Game files, generated game code, music and signing material remain excluded from source.

### Performance

- Cache Metal uniform, constant-buffer, texture and sampler bindings within their valid encoder lifetime.
- Reuse unchanged transform-constant uploads within a command buffer; keep fresh allocations for changed constants and instanced draws.
- Combine matching feedback-stage snapshots into one copy per draw.
- Use render-pass clears for complete surface clears while preserving partial and active-pass behavior.
- Native PAL CRT/video conversion fast paths and Apple generated-code optimization.
- Add asynchronous GPU timing diagnostics without waiting for GPU completion.

### Validation and limitations

- Seven input/pacing tests passed, alongside UIKit layout/customization/lifecycle/import tests, MP3 playback tests, XISO checks and Metal GPU regression checks at native and 4× scale.
- Installed and launched on an iPhone 16 Pro Max. All 191 transferred disc files matched source sizes and SHA-1 checksums on-device.
- Synthetic constant-upload benchmarks reduced CPU submission time by about 7–9% for that workload; this is not a measured gameplay FPS gain.
- Real race/pause automatic transitions and sustained gameplay performance still need device validation. Arbitrary regions and executable revisions require their own verified translation.
