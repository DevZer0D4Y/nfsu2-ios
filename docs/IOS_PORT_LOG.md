# iOS port log

Dated entries, newest last. What worked, what broke, open questions. This file is the
project's memory between sessions; `docs/UPSTREAM_NOTES.md` is upstream's.

---

## 2026-10-10 — Phase 0: reconnaissance

### Environment

| | |
|---|---|
| Host | MacBook Air M1 (4P+4E cores), 16 GB, macOS 27.0.1, **16 KB pages**, 8 MB main-thread stack |
| Toolchain | Xcode 27.1 / Apple clang 21.0.0, CMake 4.4.4, Ninja 1.13.0 (Python), pkgconf 3.0.7 |
| Homebrew | sdl2 2.32.10, libepoxy 1.5.10, openssl@3 3.6.4, molten-vk 1.4.2, vulkan-loader/headers/tools/validationlayers 1.4.363, glslang 16.6.0, spirv-tools 1.4.363 |
| Python | 3.13.7, venv `xboxrecomp/.venv` (gitignored) with capstone 5.0.7 |
| Vulkan | `vulkaninfo`: Apple M1, MoltenVK 1.4.2, apiVersion 1.4.357 |
| Game | NTSC-U disc unpacked to `~/nfsu2/game` (185 files, 2.5 GB): `B3/`, `NFSUNDER/`, `default.xbe`, `update.xbe`, `dashupdate.xbe` |
| `default.xbe` | 4,038,656 bytes, SHA-1 `428b5a70b9400f19cc5be7679db6c72a6f80a1c9`, title ID `0x4541005A` "NFS Underground 2", version 1, region North America, retail entry `0x0021B1CE` (= `NFSU2_ENTRY_POINT`), thunk table `0x0033F800` |

The port is pinned to this exact XBE: `src/main.c` (entry point, the `0x3A4C64` game-time cap, which
reads 3.0 in this image), 25 overrides in `src/recomp_manual.c` and 17 seeds in
`config/seed_functions.json` are addresses in it. A PAL or NTSC-J disc is a different build.

### Architecture: lifted code → runtime/kernel → NV2A renderers → APU/audio → USB/input

**Translator (offline, Python + capstone).** `tools/regen.sh` runs, inside `xboxrecomp/`:
`tools.disasm` (seeded by `config/seed_functions.json`) → `tools.func_id` → `tools.abi_analysis` →
`tools.recomp --all --split 250 --exclude-manual src/recomp_manual.c --mmio-sections DSOUND,XPP`.
Output (`gen/`, never committed): `recomp_*.c` chunks of 250 functions, `recomp_funcs.h`, the dispatch
table, and a copy of `xboxrecomp/templates/runtime/recomp_types.h`.

**Lifted code** (`gen/recomp_*.c`). Every guest function is `void sub_XXXXXXXX(void)`:
- Registers are thread-local globals (`RECOMP_TLS` = `__thread`, `recomp_types.h:159-168`):
  `g_eax..g_edi`, `g_esp`, x87 stack `double g_fp_stack[8]` + `g_fp_top`, `g_xmm0-7`, `g_mm0-7`.
  The translator shadows them in C locals inside functions (`_localize_registers`, `_localize_x87_stack`)
  and stores them back around calls.
- Guest memory goes through `MEM8/16/32`, `MEMF/MEMD`
  (`*(volatile T *)XBOX_PTR(va)`, `recomp_types.h:472-496`).
- Indirect calls: `RECOMP_ICALL*` look the VA up in `recomp_lookup_manual` (hand-written overrides),
  `recomp_lookup` (flat table, `recomp_dispatch_init`) and `recomp_lookup_kernel` (synthetic kernel thunks
  at `0xFE0000xx`) (`recomp_types.h:1042-1130`).
- DSOUND and XPP are lifted with `MMIO_RD/WR`, which send VAs ≥ `0xFD000000` to `xbox_mmio_read/write`
  (device models registered with `xbox_MmioRegister`, e.g. the APU from `src/main.c:386`).
- Poll loops carry `RECOMP_SPIN_HINT()` (pause, wake hardware threads, yield); function entries carry
  `RECOMP_PREEMPT()` for the guest lock.

**Runtime / kernel** (`xboxrecomp/src/kernel`, `xboxrecomp/src/platform`):
- `xbox_memory_layout.c`: XBE loader, guest address space, TIB/TLS, heap, contiguous arena,
  NV2A/MCPX/flash apertures, the NV2A ack/flag/tick threads, watchdog, `RECOMP_PEEK/POKE`.
- `kernel_bridge.c`: the 378 kernel ordinals as bridges that read arguments from the guest stack; guest
  threads (`bridge_spawn_thread`), the **guest lock** (GIL: FIFO ticket on a CRITICAL_SECTION + condvar,
  `kernel_bridge.c:573-775`), the DPC/timer thread, vblank at 59.94 Hz, IRQL.
- `kernel_{file,path,sync,thread,hal,rtl,crypto,ob,io,memory}.c`, `guest_vmem.c` (high-VA reservations).
- `nv2a_pb_exec.c`: the pushbuffer executor. It runs on `nv2a_ack_thread`, decodes NV2A methods into a
  register shadow and calls the back end (`nv2a_backend.h`: clear / draw / flip / draw_raw).
- `win32_compat.c`: Win32 primitives on POSIX, which everything above is written against.

**NV2A renderers** (back ends of the executor):
- `nv2a_gl/` is OpenGL 3.3 core. Shaders are generated from NV2A vertex programs and register combiners
  (`gl_vsh.c`, `gl_psh.c`). It has an optional GL thread (`RECOMP_GL_THREAD=1`) and `progcache.bin`.
- `nv2a_vk/` is Vulkan 1.3. It uses the same generators in a Vulkan dialect, compiled by glslang at run
  time, with dynamic rendering, push descriptors, dynamic vertex input, instancing, cube maps, and caches
  `vkspirv.bin`, `vkpipes.bin`, `vkpipecache.bin`.
- `d3d/` is the HLE D3D8 (D3D11 on Windows, `d3d8_gl.c` + libepoxy on POSIX, `d3d8_null.c` on Switch).
  It is linked, but NFSU2 draws through the NV2A path (`RECOMP_PB_EXEC=1`).
- `src/movie_crop.c` hooks raw batches to scale movie quads in 16:9.

**APU / audio**:
- `xboxrecomp/src/apu` is xemu's MCPX APU: VP voices, ADPCM, mixbins, GP/EP stubs. A frame thread is
  paced by wall clock, where 1500 frames/s means real time.
- Host output goes through `apu_xaudio2.c`: XAudio2 on Windows, SDL2 `SDL_QueueAudio` elsewhere.
- The EA mixer itself is lifted guest code: thread `0x00274CA0`, three 5.1 ring voices.
- Movies are VP6 in `ZZDATA*.BIN`. The decoder at `sub_002618F0` is either FFmpeg (`src/movie_vp6.c`,
  only with `-DNFSU2_FFMPEG_DIR`) or the lifted decoder.

**USB / input**:
- `usb/ohci.c` emulates the two MCPX OHCI host controllers. The title's XPP library enumerates a USB Xbox
  pad on them; the ISR runs guest code on the OHCI thread.
- `usb_gamepad.c` handles XID reports, rumble and `RECOMP_PAD_SCRIPT`/`RECOMP_PAD2_SCRIPT`.
- `input/xinput_device.c` reads SDL GameController or keyboard (`RECOMP_KEYBOARD=1`) into XINPUT_STATE.
- `xinput_nx.c` is the Switch input path.

**Boot order** (`src/main.c`):
1. Install the crash reporter and set the defaults `RECOMP_VBLANK=1`, `RECOMP_AC97_READY=plain`,
   `RECOMP_USB=1`, `RECOMP_PB_EXEC=1`.
2. Load `$NFSU2_GAME_DIR/default.xbe` (default `./game`), then `xbox_MemoryLayoutInit` and
   `g_xbox_mem_offset`.
3. Patch the game-time cap.
4. `mcpx_apu_init_standalone` + APU MMIO; `xbox_OhciInit`; NIC stand-in; register the renderer.
5. `xbox_kernel_init`, text patch, `xbox_path_init`, `xbox_kernel_bridge_init`.
6. Set `g_esp = XBOX_STACK_TOP`, call `recomp_dispatch_init`, start the watchdog.
7. Take the GIL and call `xbe_entry_point()` (guest entry `0x0021B1CE`) **on the calling thread**:
   `main()` on Linux, a 16 MB pthread on Switch.

### Platform layer, and what is Linux- or Switch-specific

The platform layer is `xboxrecomp/src/platform/win32_compat.{c,h}` + `xbox_winnt.h`: threads, events,
mutexes, SRW locks, condition variables, atomics, heap, timers, `VirtualAlloc`/`MapViewOfFileEx`, files and
time, implemented on POSIX. Host glue lives in `src/main.c` and `src/switch_nx.c`.

| Concern | Where | Linux / Switch / Apple today |
|---|---|---|
| File paths | `kernel_path.c` maps `D:\` → game dir and `T:\ U:\ Z:\` → save dir. `fix_case` matches names case-insensitively (on case-sensitive hosts) | POSIX save dir defaults to `$XDG_DATA_HOME/xboxrecomp` or `~/.local/share/xboxrecomp` (`kernel_path.c:556-567`). Switch uses `sdmc:/switch/nfsu2x/{game,save}` (`main.c:72-78`). NFSU2's own saves go to `<game>/UDATA`. The runtime writes `xbox_kernel.log` (`kernel_thunks.c:412`), `progcache.bin` (`nv2a_gl.c:868`) and `vk*.bin` (`nv2a_vk.c:1236-1262`) to the **current directory** |
| File I/O | `kernel_file.c` (POSIX fds, `XBOX_DIR_FD` sentinel for directories) | Switch: `.bss` bounce buffer around reads/writes (no guest memory in IPC), FAT can't hold sparse partition images |
| Threads | `win32_compat.c` `CreateThread` (pthread + trampoline); guest threads `kernel_bridge.c:902`; DPC/timer `:3332`; NV2A ack/flag/tick `xbox_memory_layout.c:1408-1413`; OHCI ISR `ohci.c:1429`; APU frame thread | Stack size is the pthread default unless given. Switch forces ≥ 8 MB (`win32_compat.c:1086-1092`). Linux gets 8 MB from `RLIMIT_STACK`, **Darwin 512 KB**. Switch also has core spreading, priorities 0x2C/0x2D/0x3B and a profiler. Linux-only: `cpu_set_t` affinity in `xbox_guest_pin` (`win32_compat.c:886-891`) |
| Memory mapping | `xbox_memory_layout.c` over `CreateFileMapping`/`MapViewOfFileEx`/`VirtualAlloc` | Linux: `memfd_create` + `MAP_FIXED_NOREPLACE`. Switch: svc shared or code memory, `svcMapProcessMemory` aliasing. Apple: `shm_open` (`win32_compat.c:2402-2416`) + `mach_vm_map(VM_FLAGS_FIXED)` placeholders (`:1776-1803`, `:2466-2474`) |
| Exceptions | `src/main.c:120-201` | Windows VEH + dbghelp. POSIX `sigaction` for SEGV/BUS/ILL/FPE with `sigaltstack` (boot thread only) and `execinfo` backtrace, printing the guest registers, guest stack and recent ICALLs. Switch uses libnx's CPU exception hook (`switch_nx.c`). POSIX `AddVectoredExceptionHandler` is a stub (`win32_compat.c:2756`), so `RECOMP_WATCH` and the AC97 write trap only work on Windows. SEH (`RtlUnwind`, `RaiseException`) is not emulated |
| Logging | stdout/stderr unbuffered (`main.c:301-302`); `xbox_log` → `xbox_kernel.log` (`XBOX_LOG_LEVEL`, `RECOMP_QUIET`, `RECOMP_NO_LOG`) | Switch has a `log:` devoptab with timestamps flushed to SD every 0.5 s, `nfsu2x_env.txt` for settings, and `[perf]` lines (`switch_nx.c`) |
| Debugger / system info | `IsDebuggerPresent`, `GlobalMemoryStatusEx` | Linux uses `/proc/self/status` and `sysinfo`. Apple uses sysctl `KERN_PROC` / `hw.memsize` / `vm.swapusage` |

### How guest memory is mapped: base + offset

Not fixed addresses. Every lifted access is
`XBOX_PTR(va) = (uintptr_t)(uint32_t)va + g_xbox_mem_offset` (`recomp_types.h:472`), where
`g_xbox_mem_offset` is the host address of guest VA 0 (`XBOX_MAP_START` = 0). It is set in
`xbox_MemoryLayoutInit` (`xbox_memory_layout.c:2225`, `:2501`) and read back with
`xbox_GetMemoryOffset()`. On Windows the offset can be 0 when a low base is free (`try_bases[]`). On
arm64 macOS it never is, because `__PAGEZERO` covers 0–4 GB and the OS picks the base. **The
"base-pointer translation behind a macro" that `CLAUDE.md` anticipates for iOS already exists.**

Host layout, relative to `base = g_xbox_mem_offset`:

| Guest VA | Size | What | Backing |
|---|---|---|---|
| `0x00000000` | 64 MB | RAM (XBE image from `0x00010000`, TIB `0x4000`, kernel data `0x00740000`, stack `0x00780000` 8 MB, heap `0x00F80000` → RAM end) | file mapping, base view |
| `0x04000000`… | 28 × 64 MB | RAM mirrors (26-bit bus wrap) | views of the same mapping; base + mirrors reserved as one 1.81 GB span first |
| up to `0x7FFE0000` | ≈ 0.19 GB | extended VMA for `guest_vmem.c` | `MEM_RESERVE` |
| `0x80000000` | 64 MB | contiguous window (`MmAllocateContiguousMemory`), fake kernel PE at `0x80010000` | its own file mapping, *not* aliased to RAM |
| `0xF0000000` | 64 MB | tiled aperture | second view of the contiguous mapping |
| `0xFD000000` | 16 MB | NV2A registers | plain RAM, polled by the ack/flag threads, executor |
| `0xFE800000` | 8 MB | MCPX: APU (MMIO model), AC97, USB OHCI, NIC | plain RAM + registered MMIO handlers |
| `0xFF000000` | 1 MB | flash | zeroed RAM |

Only base + mirrors are reserved atomically. The other ranges are placed at exact `base + VA` addresses
afterwards and are best-effort: a failure prints a `WARNING` and init still returns TRUE. The host
footprint therefore spans ≈ 4 GB of virtual address space from `base`, while committed memory is about
64 + 64 MB plus the touched aperture pages. `xboxrecomp/tests/memory_layout_posix` asserts this model on a
POSIX host without game files: base alignment to the host page, mirrors alias in both directions,
teardown/re-init including every aperture, and `RECOMP_TRAP_NULL` trapping page 0 while leaving the TIB
intact.

### x86 → arm64 audit

Most of this is already exercised on aarch64 by the Switch build (GCC). What is new on Apple is clang,
Darwin's ABI and libc, and 16 KB pages.

| Item | Finding | Risk on Apple arm64 |
|---|---|---|
| Inline asm | Only `yield` (`__aarch64__`) or `__builtin_ia32_pause` (x86) in `recomp_spin_hint` (`recomp_types.h:299-311`) | none |
| `__builtin_ia32_*` / intrinsics | `<xmmintrin.h>` and `_mm_cvt(t)ss_si32` only under x86 guards (`recomp_types.h:63-65`, `:1216-1224`, portable `#else` uses `nearbyint`/`trunc`). `recomp_manual.c:1435-1483` YUV converter has NEON / SSE2 / scalar paths. SSE/MMX are lane-wise C unions | none |
| x87 | `double g_fp_stack[8]`, not 80-bit `long double`, so results are the same on every host. RC bits honoured for `frndint`/`fist` (`recomp_types.h:366-390`); precision control (PC) not modelled | same as Linux/Switch |
| float → int | `cvt(t)ss2si`/`cvt(t)sd2si` lifted as plain `(int32_t)` casts (`lifter.py:3252-3261`). NaN/out-of-range gives `0x80000000` on x86 but saturates on arm64 (NaN → 0). `cvtss2si` truncates instead of rounding (41 sites, upstream-known) | same as Switch |
| Integer division | `idiv`/`div` lifted as C `/` and `%` (`lifter.py:2084-2086`). x86 traps (#DE) on ÷0 and INT_MIN÷-1; arm64 returns 0 / INT_MIN | behaviour differs only where a title relies on #DE |
| FP contraction | arm64 clang defaults to `-ffp-contract=on` and fuses `a*b+c` within an expression into FMA. GCC's `__attribute__((optimize("fp-contract=off")))` on the bit-exact native overrides (`recomp_manual.c:583`, `:1201`) is **ignored by clang**, so `RECOMP_NATIVE_CHECK` may report mismatches | numerical drift, not crashes |
| Unaligned atomics | `lock xadd`/`cmpxchg` on misaligned guest addresses fall back to a spinlock on `__aarch64__` (`recomp_types.h:235-272`; Apple clang defines `__aarch64__`). Plain unaligned loads/stores are fine on Apple normal memory | none |
| `long` / `char` | LP64, same as Linux x86-64; guest types are fixed width. `char` is **signed** on Apple arm64, like x86 and unlike Linux/Switch aarch64 | none |
| Alignment / page size | 16 KB host pages. `RECOMP_TRAP_NULL` already handled (TIB moved to `0x4000`, `xbox_memory_layout.h:369-389`); `mach_map_fixed` rounds to `vm_page_size`. `guest_vmem.c` commits 4 KB guest pages with `mprotect`, which gives EINVAL on unaligned 16 KB sub-ranges; mitigated because reservations are mapped RW (`prot_from_page(PAGE_NOACCESS)` → RW, `win32_compat.c:1829`) | low |
| TLS | Guest registers are `__thread`. On Darwin each function resolves them through the TLV thunk (the Switch saw the same with `-mtp=soft`) | perf only |
| setjmp | The lifted CRT `setjmp` uses native `setjmp`/`longjmp`, which on Darwin saves the signal mask (a syscall) | perf only, if hot |
| MXCSR | FTZ/DAZ and rounding are not modelled; host defaults apply (same on x86 hosts) | none new |
| Misc | `unsigned __int128` (`qemu_shim.h:96`), `__builtin_return_address` (`kernel_hal.c:24`): fine on clang | none |
| Toolchain | `-lstdc++` (`nv2a_vk/CMakeLists.txt:35`) does not exist in the macOS SDK (libc++ only) | link error |

### Files that will need an Apple/iOS branch

| File | Reason |
|---|---|
| `CMakeLists.txt` | Apple host sources; iOS bundle/entitlements; `-ffp-contract=off` for `recomp_manual.c` under clang; FFmpeg as a dylib on iOS |
| `src/main.c` | The host must own the main thread on Apple (no generic `main()`); iOS default game/save dirs in `Documents/` |
| `src/recomp_manual.c` | GCC-only `optimize("fp-contract=off")` is ignored by clang |
| `xboxrecomp/src/platform/win32_compat.c` | glibc affinity breaks the macOS build; 8 MB thread-stack floor (Darwin default 512 KB); iOS: `<mach/mach_vm.h>` is `#error unsupported` in the iOS SDK, `shm_open` vs sandbox, no `PROT_EXEC`, priorities → QoS |
| `xboxrecomp/src/kernel/CMakeLists.txt` | OpenSSL is linked but unused (portable crypto); not available on iOS |
| `xboxrecomp/src/d3d/CMakeLists.txt` | `d3d8_gl.c` needs libepoxy (no iOS build); use `d3d8_null.c` like the Switch |
| `xboxrecomp/src/nv2a_vk/CMakeLists.txt` | `stdc++` → `c++`; MoltenVK framework instead of the loader on iOS |
| `xboxrecomp/src/nv2a_vk/nv2a_vk.c` | Portability enumeration/subset (loader on macOS); window adoption + surface on the main thread; no off-main `SDL_PollEvent`; **static vertex-input pipelines** (MoltenVK lacks `VK_EXT_vertex_input_dynamic_state`); cache dir is the CWD |
| `xboxrecomp/src/nv2a_gl/nv2a_gl.c` | Window/context creation and `SDL_PollEvent` on the executor thread (AppKit needs the main thread); macOS-only renderer |
| `xboxrecomp/src/kernel/kernel_path.c` | POSIX save-dir default (XDG / `~/.local/share`) is wrong for an iOS sandbox; host must pass `Documents/` paths |
| `xboxrecomp/src/kernel/kernel_thunks.c` | `xbox_kernel.log` goes to the CWD (unwritable on iOS); host `chdir`s to `Library/Caches` |
| `xboxrecomp/src/kernel/xbox_memory_layout.c` | ≈ 4 GB of VA from one base; iOS VA limits may need a single up-front reservation and fewer mirrors |
| `xboxrecomp/src/kernel/kernel_memory.c` | Guest `PAGE_EXECUTE*` protections reach `mmap`/`mprotect` via the direct `Nt*`/`Mm*` paths; iOS refuses `PROT_EXEC` |
| `xboxrecomp/src/kernel/guest_vmem.c` | 4 KB commit granularity on 16 KB host pages |
| `xboxrecomp/src/input/xinput_device.c` | SDL GameController init/update on the OHCI thread; Apple's controller frameworks post on the main run loop; touch overlay later |
| `xboxrecomp/src/apu/apu_xaudio2.c` | iOS audio session/interruptions; stop the device in background |
| `xboxrecomp/templates/runtime/recomp_types.h` | Nothing required; TLS cost on Darwin is a later perf item |
| `tools/build_ffmpeg_vp6.sh` | Uses `nproc` (GNU coreutils); only `switch`/`linux` targets, needs `macos`/`ios` (LGPL dylib) |

---

## 2026-10-10 — Phase 0: macOS bring-up

### Step 1: runtime without game files (`xboxrecomp/tests/*_posix`)

- Build failed in exactly one place: `xbox_guest_pin()` uses glibc's `cpu_set_t` / `CPU_SET` /
  `pthread_setaffinity_np` (`win32_compat.c`), which Darwin lacks. This is an NFSU2-port addition
  (`RECOMP_GUEST_ONE_CORE`); the rest of the toolkit's Apple paths compiled.
  **Fix:** `#elif defined(__APPLE__)` in `xbox_guest_core()` logs that the switch is ignored and keeps
  core = -1; `xbox_guest_pin()` compiles the affinity block only `#elif !defined(__APPLE__)`. Darwin
  has no hard thread affinity, and the switch is a diagnostic.
- After that the whole runtime (kernel, APU, NV2A, USB, input, GL renderer, d3d8_gl + epoxy) builds with
  Apple clang: 0 errors, 71 warnings.
- `memory_layout_posix_test` (synthetic `test.xbe`): **0 failures**.
  - Base `0x7000000000` (offset +0x7000000000). Host page 16384; base and size are page-aligned.
  - All 28 mirrors alias in both directions.
  - Contiguous `0x80000000`, NV2A `0xFD000000`, MCPX `0xFE800000`, flash `0xFF000000` and tiled
    `0xF0000000` all map, including on re-init. The tiled ↔ contiguous alias is verified.
  - `RECOMP_TRAP_NULL` faults on guest 0 with the TIB at `0x4000` intact.
  - Side finding: `xbox_MemoryLayoutShutdown` does not release the extended-VMA reservation, so the
    second init logs `Extended VMA: FAILED ... (error 8)`. Harmless for a title that initialises once.
- `kernel_memory_posix`: passes.

### Step 2: lift the XBE (`tools/regen.sh`): **blocked, not by macOS**

- `PATH=xboxrecomp/.venv/bin:$PATH NFSU2_XBE=~/nfsu2/game/default.xbe NFSU2_GEN_DIR=$PWD/gen tools/regen.sh`
  stops immediately: `ModuleNotFoundError: No module named 'tools.disasm.output'`.
- Cause: the vendored toolkit is missing three of its own modules, imported by `tools/disasm/disasm.py:20`,
  `tools/func_id/identify.py:20`, `tools/recomp/__main__.py:29` (and `tools/test_doctor.py:161`):
  `xboxrecomp/tools/{disasm,func_id,recomp}/output.py`. They are not ignored by any `.gitignore` here and
  appear nowhere in this repo's history, so they were dropped when upstream vendored the toolkit. Upstream
  regenerates from a separate full checkout (`/root/nfsu2x/xboxrecomp-pr128`, see UPSTREAM_NOTES "Layout
  and builds"), which is why this never showed up there. It breaks regen on every OS.
- Candidate fix: restore the three files verbatim from the toolkit's origin, `sp00nznet/xboxrecomp` main
  @ `193e2995ffae` (2026-10-07, MIT). That is 292 + 212 + 113 lines, stdlib only. Their exported names
  and signatures match every call site in the vendored tree (`OutputWriter(...)`/`write_all`,
  `print_stats`, `write_results`, `write_summary`, `generate_header`). Copies for review are in the
  session scratchpad, not in the repo.
  **Not applied:** it brings external code into the shared upstream tree, so it waits for the owner's decision.
- No macOS-specific regen issue found so far (the pipeline is pure Python, no subprocess or GNU tools).
- **Resolved (owner approved):** the three modules were restored verbatim (commit "xboxrecomp: restore
  missing ...").
- Second blocker: `tools.disasm` needs the XBE parser's `<stem>_analysis.json` and looks for it next to
  the XBE. `regen.sh` never produced it; upstream evidently had one lying beside its XBE. **Fix:**
  `regen.sh` now runs `tools.xbe_parser --json` into the ignored `xboxrecomp/tools/disasm/output/` and
  passes `--analysis-json`, so nothing is written into the game directory. Not macOS-specific.
- **Result:** regen succeeds on macOS in **181 s** (M1, Python 3.13, capstone 5.0.7):
  - 19,539 functions loaded, 19,536 translated, 0 failed;
  - 2,494,663 lines of C in 79 files (`gen/`, 130 MB), plus ≈140 MB of tool output, all ignored;
  - 3 functions excluded for `recomp_manual.c`, 22 wrapped as `sub_X_gen`;
  - 670 functions with MMIO accessors (DSOUND, XPP);
  - 159 unresolved call targets stubbed; 3,501 `TODO:` sites (`RECOMP_UNIMPL`, reported at runtime).
- No macOS-specific change was needed for the lift itself.

### Step 3: Vulkan renderer on MoltenVK: **blocked (as expected)**

- `cmake -S . -B build-macos-vk -G Ninja -DNFSU2_GEN_DIR=$PWD/gen -DNFSU2_VULKAN=ON -DCMAKE_PREFIX_PATH=/opt/homebrew -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)`
  configures with CMake 4.4.4 (no policy override needed).
- `cmake --build build-macos-vk -j6`: **all 2.49 M lines of lifted C compile with Apple clang 21 with no errors**,
  and it links (25 MB binary). The predicted `-lstdc++` problem did not happen: Apple's linker resolved it to
  `/usr/lib/libc++.1.dylib` (`otool -L`), so `nv2a_vk/CMakeLists.txt` is unchanged.
  Linked: Homebrew SDL2, libepoxy, OpenSSL, glslang 16, SPIRV-Tools, `libvulkan.1.dylib` (loader).
- Headless run (`RECOMP_VK_HEADLESS=1`, cwd `build-macos-vk/run/`, killed after 60 s): **the title boots on
  arm64 macOS.**
  - Memory layout is complete: base `0x7000000000`, 14/14 sections, 152/152 kernel thunks bridged, all
    apertures, 28/28 mirrors.
  - The APU is up and the guest starts at `0x0021B1CE`.
  - The disc streams (`ZZDATA*.BIN`; the opens that fail are the title probing ZZDATA10–15 and TDATA
    `contentmeta.xbx`, which the disc does not have).
  - The pushbuffer executor keeps up (DMA_PUT == DMA_GET), traps are handled, and OHCI0 enumerates the pad.
  - Unlifted `in/out dx` and `wbinvd` instructions are reached as no-ops.
  - No crash in 60 s.
- Renderer: `[VK] vkCreateInstance failed`. The loader hides MoltenVK unless the instance enables
  `VK_KHR_portability_enumeration`. **Fix** (`nv2a_vk.c`, `#if defined(__APPLE__)`, 20 lines): enable it
  plus `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR` when the loader offers it, and enable
  `VK_KHR_portability_subset` at device creation when the device offers it (required by the spec).
- Rerun: `[VK] Apple M1, Vulkan 1.3.357`, then
  `[VK] vkCreateDevice failed (needs push descriptors, dynamic vertex input, Vulkan 1.3)`.
  `vulkaninfo` on this machine (MoltenVK 1.4.2) lists `VK_KHR_push_descriptor`, `VK_KHR_dynamic_rendering`,
  `VK_EXT_extended_dynamic_state`, `VK_KHR_portability_subset`, but **not
  `VK_EXT_vertex_input_dynamic_state`**, and MoltenVK's own user guide does not list it either.
  `nv2a_vk.c` requests it at device creation and calls `vkCmdSetVertexInputEXT` for every draw.
  **Vulkan is blocked on Apple until nv2a_vk has a static vertex-input path. Switching to OpenGL.**

### Step 4: OpenGL renderer: macOS host owns the main thread

- `build-macos` (GL, default options): all of `gen/` plus the runtime build in **127 s** (`-j6`, M1), 25 MB binary.
- Unmodified run: `NSInternalInconsistencyException: 'API misuse: setting the main menu on a non-main
  thread'`, then abort. The stack is `nv2a_ack_thread → nv2a_pb_scan → clear_surface → gl_clear → ready →
  SDL_InitSubSystem(SDL_INIT_VIDEO) → Cocoa_RegisterApp → -[NSApplication setMainMenu:]`. The renderer
  initialises SDL video lazily on the pushbuffer-executor thread, which Linux/X11 tolerates and AppKit does
  not (window creation and event pumping are main-thread-only too).
- **Fix** (the Switch's own pattern):
  - New `src/apple/apple_main.c` owns `main()`. It calls `SDL_Init(VIDEO)`, creates the 1280×720 GL
    3.3-core window and context on the main thread, releases the context and hands it to the renderer
    through the existing weak hook `nv2a_gl_adopt_window()`.
  - It runs the title (`nfsu2_game_main()`) on a 16 MB pthread and pumps events on the main thread with
    `SDL_WaitEvent`. SDL's Cocoa GL backend `dispatch_sync`s `NSOpenGLContext update` from the render
    thread to the main queue, so the main thread blocks nowhere else.
  - `src/main.c` exposes `nfsu2_game_main()` instead of `main()` on Apple (`#elif defined(__APPLE__)`).
  - `nv2a_gl.c` skips its per-present `SDL_PollEvent` drain on Apple, since AppKit events must not be
    pumped off-main.
  - Root `CMakeLists.txt` adds the file `if(APPLE)`.
- **Result (75 s run):**
  - `[GL] Apple / Apple M1 / 4.1 Metal - 91.7`, `[AUDIO] SDL coreaudio output: 48000 Hz, 2 ch`.
  - Movies play in order, `MOVIES\ealogo.vp6` → `THX_LOGO` → `PSA` → `FMVOpening`, through the
    **lifted** VP6 decoder (no FFmpeg build).
  - `[fps] 42–52 fps`, vblank 59.8–60.0 Hz. No crash.
  - Frame dumps (`RECOMP_GL_DUMP`, 1/s) show the EA Games logo, the NFSU2 key art and the trailer, all
    rendered correctly.

### Step 5: intro movie → main menu: **reached**

- Pad script (times from the first pad read):
  `RECOMP_PAD_SCRIPT="40000:start:300,50000:start:300,58000:a:200,66000:a:200"`.
  1. Start at 40 s skips `FMVOpening`.
  2. Start at 50 s leaves "Please press START to begin" (title, soundtrack credit, EA © line, all
     rendered correctly); the title then scans `UDATA\4541005a`.
  3. "Xbox hard disk: Would you like to create a new profile?" appears with **No** selected; A at 58 s
     picks No.
  4. **Main Menu**: NFSU2 logo, "Quick Race" carousel, the 3D menu car, "No Profile", Y Help.
  5. A at 66 s opens "Quick Race Mode Select / Circuit".
  All of this is checked from `RECOMP_GL_DUMP` frames (BMP → PNG with `sips`).
- `[fps]` 57–60 fps in the menus, vblank 59.6–60.0 Hz; movies 42–52 fps through the lifted VP6 decoder.
- No crash in any run after the host fix. The only `.ips` report (15:10:50) is the AppKit abort from
  Step 4. So the predicted 512 KB default-stack overflow on guest/ISR/DPC threads **did not show up** on
  the way to the menu, and per the "confirm before fixing" rule `CreateThread` is unchanged. Races
  (deeper call chains) remain the case to watch.
- `RECOMP_NATIVE_CHECK=1` (runs each native override against the lifted code) to the menu:
  - 0 mismatches: `sub_000113A0`, `sub_0004B940`, `sub_0004BC20`, `sub_002EEA80`, `sub_0025ECB4`
    (YUV rows, 524k rows).
  - Mismatches: `sub_002213EA` (SSE 4×4 multiply, one of the two functions carrying GCC's ignored
    `optimize("fp-contract=off")`), 20 reported; and `sub_0004B260` (sine of a 16-bit angle, x87
    result), 683 of 4096 calls.
  - This confirms the audit item: under Apple clang the native overrides are no longer bit-exact with the
    lifted code. Last-bit drift, not crashes. Fix in Phase 1: build `recomp_manual.c` (and probably
    `gen/`) with `-ffp-contract=off` under clang, then re-run the check. For `sub_0004B260`, also compare
    Apple libm's `sin` with glibc's before blaming contraction.

## Status at the end of Phase 0 (2026-10-10)

**Done on arm64 macOS (M1, macOS 27.0.1):**
- XBE lifted by `tools/regen.sh` (181 s).
- Toolkit runtime tests pass.
- Game built with Apple clang (≈2 min).
- Boot → EA / THX / PSA / FMVOpening movies → title → profile prompt → **Main Menu** → Quick Race mode
  select, on the **OpenGL** renderer (Apple GL 4.1 on Metal), with CoreAudio sound, at ~60 fps in the
  menus.

**Changes to shared upstream files (all small and guarded, or verbatim restorations):**
- `win32_compat.c`: no thread affinity on Darwin.
- `nv2a_vk.c`: portability enumeration/subset on Apple.
- `nv2a_gl.c`: no off-main event drain on Apple.
- `src/main.c`: `nfsu2_game_main()` on Apple.
- `CMakeLists.txt`: Apple host source.
- `tools/regen.sh`: generates the analysis JSON.
- `xboxrecomp/tools/{disasm,func_id,recomp}/output.py`: restored from upstream.

New file: `src/apple/apple_main.c`.

**Blocking / open:**
1. **Vulkan on Apple:** MoltenVK 1.4.2 has no `VK_EXT_vertex_input_dynamic_state`, which `nv2a_vk.c`
   requires. This blocks Phase 1 as written ("the existing Vulkan renderer on MoltenVK").
2. Native overrides drift under clang FP contraction (above).
3. Not done: the FFmpeg VP6 build for macOS (the lifted decoder is fast enough on M1), any race,
   controller input (only the scripted pad was used), `RECOMP_GL_THREAD`.
4. Upstream hygiene: the extended-VMA reservation is not released by `xbox_MemoryLayoutShutdown`.
   `tools/build_ffmpeg_vp6.sh` uses `nproc` (absent on macOS).

## Phase 1 plan: iOS boot (intro movie + main menu on an iPhone)

### Order of work (validate each step on macOS first, where possible)

1. **Renderer (macOS first, ask gate: >20 lines in `nv2a_vk.c`).** Give `nv2a_vk` a static vertex-input
   path for drivers without `VK_EXT_vertex_input_dynamic_state`:
   - put the vertex binding/attribute layout (formats, offsets, present mask; strides too unless
     `VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE` is used) into the pipeline key;
   - build `VkPipelineVertexInputStateCreateInfo` per pipeline;
   - keep `vkCmdSetVertexInputEXT` where the extension exists (Switch/NVK, Linux);
   - extend `vkpipes.bin` so the prewarm covers the new variants.
   Done when the macOS Vulkan build reaches the main menu on MoltenVK with `RECOMP_VK_VALIDATION=1`
   clean (validation layers are installed). Also needed: window adoption, surface creation on the main
   thread, and no off-main `SDL_PollEvent` — the same treatment `nv2a_gl` got.
   - *Alternative if this proves large:* start Phase 3 (native Metal back end) early. It needs no SPIR-V
     or MoltenVK, but it is the bigger job; the decision belongs to the owner.
2. **Dependencies for iOS arm64** (`tools/build_ios_deps.sh`, new):
   - SDL2 2.32 static from source;
   - MoltenVK.xcframework (LunarG SDK or a `make ios` build; Homebrew's is macOS-only);
   - glslang + SPIRV-Tools static;
   - FFmpeg VP6 LGPL as a **dynamic** framework (`build_ffmpeg_vp6.sh ios` target, also fixing `nproc`).
3. **iOS host** (new files):
   - `src/apple/apple_main.c`, shared, gains an iOS branch: SDL's `SDL_main`, a full-screen window,
     the 16 MB game thread (iOS's main-thread stack is 1 MB and secondary threads get 512 KB).
   - `src/ios/ios_host.m` (thin Objective-C):
     - resolve `Documents/` and `Library/Caches/`;
     - `chdir(Caches)` so `xbox_kernel.log`, `progcache.bin` and `vk*.bin` land somewhere writable;
     - read `Documents/nfsu2x_env.txt` (`KEY=VALUE`, port of `switch_nx.c`'s reader) into `setenv`;
     - redirect stdout/stderr to `Documents/nfsu2x_log.txt`;
     - set `NFSU2_GAME_DIR=Documents/game` and pass the save dir `Documents/save` (no XDG fallback).
   - `src/ios/ios_lifecycle.c`: on `SDL_APP_WILLENTERBACKGROUND`, take the GIL from the host thread,
     pause the SDL audio device and stop the executor before its next submit; reverse on foreground.
4. **Platform layer for iOS** (`xboxrecomp/src/platform/ios_vm.c`, new; small `#if TARGET_OS_IOS` hooks
   in `win32_compat.c`):
   - `<mach/mach_vm.h>` is `#error unsupported` in the iOS 27.1 SDK, so use `vm_map` / `vm_allocate`
     / `vm_deallocate` from `<mach/vm_map.h>` (64-bit `vm_address_t` on arm64) for fixed placement.
   - `CreateFileMapping` is backed by `mach_make_memory_entry_64(MAP_MEM_NAMED_CREATE)` and views by
     `vm_map(..., entry, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE)` inside our own reservation, instead of
     `shm_open`, which the sandbox may refuse.
   - `prot_from_page` drops `PROT_EXEC` on iOS (no JIT; guest `PAGE_EXECUTE*` reaches `mprotect` via
     `kernel_memory.c`).
   - Guest thread priorities map to QoS: the APU/audio thread is user-interactive and above game threads.
5. **CMake for iOS** (`-G Xcode -DCMAKE_SYSTEM_NAME=iOS`, `ios/` for Info.plist and entitlements):
   - `d3d8_null.c` instead of `d3d8_gl.c` (no libepoxy), no OpenSSL, no `nv2a_gl`;
   - MoltenVK framework instead of the loader;
   - `UIFileSharingEnabled` + `LSSupportsOpeningDocumentsInPlace` so the disc can be copied into
     `Documents/game` with the Files app or Finder;
   - entitlements `com.apple.developer.kernel.increased-memory-limit` and
     `com.apple.developer.kernel.extended-virtual-addressing`.

### Memory-mapping strategy

- **Keep upstream's base + offset model.** Every lifted access is already
  `(uint32_t)va + g_xbox_mem_offset`; no new translation macro is needed, and there are no fixed low
  addresses (macOS already runs it with the base at `0x7000000000`).
- iOS needs about **4 GB of virtual address space** from that base (RAM + 28 mirrors 1.81 GB,
  extended VMA 0.19 GB, apertures up to +0xFF100000), but only about 150 MB+ of it is ever dirty.
  - Reserve the whole 4 GB window with **one** `vm_allocate(VM_FLAGS_ANYWHERE)` first, so every aperture
    is carved out of our own reservation instead of racing other mappings (today only base + mirrors are
    reserved together).
  - Then map views with `VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE`.
- If iOS refuses a 4 GB reservation without `extended-virtual-addressing` (or the personal team cannot
  get that entitlement), shrink the footprint:
  - measure on macOS whether NFSU2 ever touches the 64 MB RAM mirrors (`xbox_ProtectMirrorsForDebug`
    makes them read-only and a write faults);
  - if it doesn't, map no mirrors on iOS (−1.8 GB) and keep only the contiguous window and the apertures.
- Dirty-memory budget: 64 MB RAM, ≤64 MB contiguous, ~25 MB apertures, renderer textures (keep the
  texture budget conservative), host heap. Use `increased-memory-limit` and log `os_proc_available_memory()`
  at boot.

### Risks expected on device

1. **VA limit / entitlements:** the 4 GB reservation may fail without extended virtual addressing;
   a free personal team may not be allowed that entitlement or increased-memory-limit (7-day
   provisioning as well).
2. **Sandbox memory APIs:** `shm_open` blocked; `VM_FLAGS_FIXED` maps refused in some ranges. Test with a
   tiny iOS probe app before porting the whole game.
3. **Renderer:** the static-vertex-input path multiplies pipeline variants, so expect first-use hitches
   (Metal pipeline compiles; prewarm from `vkpipes.bin`). Other MoltenVK limits to watch:
   - Metal always has primitive restart on, so index `0xFFFF` in a 65,536-vertex strip batch would
     restart;
   - no D24S8 (the renderer already falls back to D32S8);
   - BC textures may be unsupported on some iPhone GPUs (CPU decode fallback exists).
4. **Threads:** a 1 MB main thread and 512 KB default thread stacks; deeper race call chains may overflow
   where the menus did not. Thermal throttling on long runs.
5. **Lifecycle:** any GPU submission in the background gets the app killed, so pause must stop the
   executor, not just the guest.
6. **Performance:** Darwin TLS accessor cost for the `__thread` register file, lifted code at `-O1`,
   Darwin `setjmp` saving the signal mask. Upstream's Switch measurements (TLS, GIL handover, executor
   cost per draw) are the guide; profile with Instruments.
7. **Toolchain / legal:** every dependency rebuilt for iOS arm64. FFmpeg must stay a dynamic LGPL
   library. The APU and `nv2a_regs.h` are LGPL-2.1 code linked statically into the app, which needs
   object files or a dynamic library for relinking before any distribution.
8. **Files:** iOS APFS is case-sensitive; `fix_case` already handles the disc's mixed-case names.

## 2026-10-10 — Phase 0 follow-up: real controller input

- Owner report: the game starts, but the gamepad (DualSense over Bluetooth) does nothing, so the first
  movie cannot be skipped.
- Cause (code): on POSIX nothing calls `xbox_InputInit()` (`xinput_device.c`), the only place that starts
  SDL's game-controller subsystem and opens pads. So `xbox_InputGetState()` always answered
  `ERROR_DEVICE_NOT_CONNECTED` (Linux included; upstream drove menus with `RECOMP_PAD_SCRIPT`, and the
  Switch has its own path). Also: keyboard input (`RECOMP_KEYBOARD`) exists only on Windows, and
  `xbox_FramebufferKeyDown` is a stub elsewhere (`fb_present.c:368`).
- **Fix:**
  - `apple_main.c` starts `SDL_INIT_GAMECONTROLLER` on the main thread with video. macOS delivers
    controller arrival and input through the main run loop that thread pumps.
  - `xbox_InputGetState()` rescans when a port has no attached pad, at most once a second per port.
    This also covers late discovery and reconnects. It logs `[INPUT] port N: <name>`.
- Verified: `[INPUT] port 0: DualSense Wireless Controller`, `InputGetState=0` once a second
  (`RECOMP_INPUT_DIAG=1`). Button presses still need checking by hand.
- Controller confirmed working by the owner (DualSense: Options = Start, Cross = A).

### GPU load on macOS (owner report: "GPU at 100%")

Sampled `ioreg` IOAccelerator "Device Utilization %" once a second, 2026-10-10:

| Situation | Device utilization |
|---|---|
| Game closed (Spotify helper/renderer, WindowServer, browser running) | 62–72% |
| Main menu, default | ≈99% (12 s average) |
| Main menu, `RECOMP_GL_SCALE=0.5` | ≈97% |

- Most of the "100%" is the Mac's existing load; the game adds about 30+ points and saturates the GPU.
- Half the render resolution barely changes it, so the cost is not fill-rate. It is per-draw work: about
  356 draws/frame (`[cull]` line), ≈21k draws/s, each going through `nv2a_gl`, then Apple's OpenGL → Metal
  translation layer.
- The game still holds 53–60 fps with a 59.9 Hz vblank, so it is not GPU-limited. Apple's utilization is
  relative to the current GPU clock (DVFS).
- Not fixed in Phase 0: upstream freezes `nv2a_gl` ("renderer work goes into Vulkan only"). The
  Vulkan-on-MoltenVK fallback (Phase 1) or the native Metal renderer (Phase 3) removes the GL translation
  layer.
- For a precise breakdown: `sudo powermetrics --samplers gpu_power` (GPU frequency and active residency).

## 2026-10-10 — Vulkan renderer on MoltenVK: working (main menu and a race)

Owner approved the >20-line change to `nv2a_vk.c`.

- **Static vertex input** when the device lacks `VK_EXT_vertex_input_dynamic_state` (MoltenVK):
  - `s_static_vi` is detected at device creation; the extension and its feature struct are requested
    only where offered.
  - The 16 vertex formats are added to `PipeKey` (`vertex_formats_key()`, the same rule
    `upload_vertices` uses: the title's format for attributes uploaded as stored, float4 otherwise).
  - Pipelines get one binding per location. Strides stay dynamic (`VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE`
    + `vkCmdBindVertexBuffers2`, core 1.3), which keeps the number of variants down.
  - Prewarm records go to `vkpipes_sv.bin` (record + 16 formats), so `vkpipes.bin` keeps its format on
    Switch/Linux. Devices that have the extension are unchanged.
- **Fans → triangle lists** on portability drivers (`s_fan_lists`). NV2A `TRIANGLE_FAN`/`POLYGON` went out
  as `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN` via dynamic topology. Metal has no fans, and MoltenVK's
  emulation did not apply, so they drew as strips: every full-screen quad lost its second triangle (a
  diagonal black half on title and movies) and glyph quads were garbled. Converted on the CPU like quads.
  MoltenVK reports `vertexAttributeAccessBeyondStride = true` and stride alignment 1, which ruled out the
  stride-0 constant attributes first.
- **Window/surface on the main thread** (as for GL): weak hooks `nv2a_vk_adopt_window()` and
  `nv2a_vk_host_surface()`. `apple_main.c` creates the `SDL_WINDOW_VULKAN` window and makes the surface
  with `dispatch_sync_f` to the main queue. No off-main `SDL_PollEvent` on Apple.
- **Result:**
  - `[VK] swapchain 1280x720, 3 images`, prewarm from cache.
  - Movies, title, loading screen (controller diagram text sharp), main menu, and a Quick Race (world,
    HUD, minimap, tachometer) all render correctly.
  - 58–60 fps in menus, 30 fps in the race (the title's own race rate).
- Validation (`RECOMP_VK_VALIDATION=1`, Homebrew layers): only `VUID-vkQueueSubmit-pSignalSemaphores-00067`.
  The "rendered" semaphore is per frame-in-flight (2), not per swapchain image (3). That is pre-existing
  and platform-independent; logged as an upstream follow-up.
- GPU (`ioreg`, quiet baseline 4–12%): GL main menu ≈22%, Vulkan main menu ≈25%. The earlier "100%" was
  mostly other apps plus a low-clocked GPU.
- MoltenVK also reports `samplerMipLodBias = false` (any LOD bias the title sets is ignored) and
  `pointPolygons = false`.

## 2026-10-10 — Phase 1 (iOS boot): started, **not yet built**

- Targets available: Simulator runtimes iOS 27.0/27.1 (iPhone 18 Pro); paired device iPhone 13 Pro
  (iPhone14,2, A15, 6 GB); signing identity "Apple Development" (account type not yet confirmed).
- `tools/build_ios_deps.sh sim` (committed) built SDL2 2.32.10 static, glslang 16.6.0 (no optimizer) and
  MoltenVK 1.4.2 (official static library; simulator slice taken from `MoltenVK-all.tar`) into
  `~/nfsu2/deps/sim` in 52 s. The device target is not built yet.
- Written, **not compiled yet** (stopped on disk space: 1.3 GiB free, 1.7 GB swap):
  - `win32_compat.c`: on `TARGET_OS_IPHONE`, `vm_map` instead of the unsupported `<mach/mach_vm.h>`,
    and no `PROT_EXEC` in guest protections. `shm_open` is kept until a device run shows whether the
    sandbox allows it.
  - CMake: iOS → `d3d8_null.c` (no libepoxy), no OpenSSL; `nv2a_vk` links MoltenVK statically with
    its frameworks; the root makes an app bundle (`ios/Info.plist.in`: Files-app sharing, landscape)
    and links `SDL2main`.
  - `src/ios/ios_host.c`: `Documents/game` (`NFSU2_GAME_DIR`), `Documents/nfsu2x_env.txt`,
    `Documents/nfsu2x_log.txt`, saves in `Library/Application Support`, cwd `Library/Caches`.
  - `apple_main.c`: iOS full-screen Vulkan window, landscape hint, `ios_host_setup()`.
- **Next:**
  1. Free disk.
  2. Configure:
     `cmake -S . -B build-ios-sim -G Ninja -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 -DCMAKE_PREFIX_PATH=$HOME/nfsu2/deps/sim -DNFSU2_GEN_DIR=$PWD/gen -DNFSU2_VULKAN=ON`
  3. Build, then `xcrun simctl install` / `launch` with `SIMCTL_CHILD_NFSU2_GAME_DIR=$HOME/nfsu2/game`.
  4. Then the device build (signing, entitlements), the lifecycle pause, and the memory-reservation
     check on the A15.

## 2026-10-10 — iOS Simulator: boots to the main menu

- Free disk 2.1 GiB after cleanup. `build-ios-sim` (Ninja, `CMAKE_SYSTEM_NAME=iOS`,
  `iphonesimulator`, arm64, deps from `~/nfsu2/deps/sim`) **compiled and linked first time** in 77 s:
  all of `gen/`, the runtime, MoltenVK and SDL2. App bundle 34 MB, ad-hoc signed.
- Launch 1: SIGTRAP in `_UIApplicationEvaluateRuntimeIssueForNoSceneLifecycleAdoption` (lldb attached to
  `simctl launch --wait-for-debugger`). **Apps built with the iOS 27 SDK must adopt the UIScene
  lifecycle**; SDL2 2.32 still makes its UIWindow the old way. Fix: `Info.plist` declares one scene
  configuration (no delegate class), and `src/ios/ios_scene.m` (thin Objective-C, ARC) attaches SDL's
  `UIWindow` to the connected `UIWindowScene` right after `SDL_CreateWindow`.
- Launch 2: the guest boots and the Vulkan device comes up (2622×1206 swapchain), but every pipeline
  fails. MoltenVK: `VK_ERROR_FEATURE_NOT_PRESENT: ... does not support
  VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE (macOS 14.0 or iOS/tvOS 17.0, plus either Apple4 or Mac2
  GPU)`; the simulator GPU is not Apple4-class. Fix (`nv2a_vk.c`): at the first such failure, switch
  once to `s_static_stride`. The strides join the formats in the pipeline key (`vertex_strides_key`, the
  same rule `upload_vertices` uses) and bind with `vkCmdBindVertexBuffers`. These pipelines are not
  recorded for prewarm. A real A15 (Apple8, iOS 27) should keep dynamic strides.
- Launch 3 (`SIMCTL_CHILD_NFSU2_GAME_DIR=$HOME/nfsu2/game`, pad script): **ealogo → THX → PSA →
  FMVOpening → title → "PROFILE has been loaded successfully." → Main Menu**, 52–60 fps, vblank 59.9 Hz,
  0 pipeline failures. Checked from `simctl io screenshot`.
- Gotcha: `simctl launch --stdout/--stderr` into a file that an earlier, still-running instance writes to
  gives misleading logs; terminate first and use fresh file names.

## 2026-10-10 — **Phase 1 milestone: runs on an iPhone 13 Pro**

Device: iPhone 13 Pro (iPhone14,2, A15, 6 GB), iOS 27. Signing: paid Individual team `YOURTEAMID`,
automatic signing with the team's wildcard profile, bundle ID `com.yourname.nfsu2x`.
**No special entitlements were needed.**

Build and deploy:
```sh
tools/build_ios_deps.sh device
cmake -S . -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_PREFIX_PATH=$HOME/nfsu2/deps/device \
  -DCMAKE_FIND_ROOT_PATH=$HOME/nfsu2/deps/device -DNFSU2_GEN_DIR=$PWD/gen -DNFSU2_VULKAN=ON \
  -DNFSU2_IOS_BUNDLE_ID=com.yourname.nfsu2x -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=YOURTEAMID \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_STYLE=Automatic -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY="Apple Development"
xcodebuild -project build-ios/nfsu2_recomp.xcodeproj -scheme nfsu2_recomp -configuration Release \
  -destination 'generic/platform=iOS' -allowProvisioningUpdates build
xcrun devicectl device install app --device <UDID> build-ios/Release-iphoneos/nfsu2_recomp.app
xcrun devicectl device copy to --device <UDID> --domain-type appDataContainer \
  --domain-identifier com.yourname.nfsu2x --source ~/nfsu2/game --destination Documents/game   # 76 s, 2.5 GB
```
After a CMake change, re-run `cmake build-ios` before `xcodebuild`: the regeneration did not pick up
`ios_vm.c` by itself.

Device runs:
1. `[ios] no connected window scene`, then `CreateFileMapping failed (error 8)`, then FATAL (black screen).
   - **The sandbox refuses `shm_open`** (the Simulator does not).
   - On a device the window scene connects after launch.
   - **Fixes:** `xboxrecomp/src/platform/ios_vm.c` backs file mappings with a named Mach memory entry
     (`mach_make_memory_entry_64(MAP_MEM_NAMED_CREATE)`, views with `vm_map(... VM_FLAGS_FIXED |
     VM_FLAGS_OVERWRITE)` after the existing `mach_map_fixed` claim). `win32_compat.c` swaps only the
     create, map and close calls under `TARGET_OS_IPHONE`. `ios_scene.m` turns the run loop (up to 2 s)
     until the scene connects.
2. **The whole memory model maps on iOS without extended-virtual-addressing:** base `0x7000000000`, RAM
   + 28/28 mirrors, extended VMA, contiguous window, NV2A/MCPX/flash apertures, tiled alias verified.
   Vulkan runs on `Apple A15 GPU`; EA logo and THX play. Then
   `kIOGPUCommandBufferCallbackErrorBackgroundExecutionNotPermitted` → `VK_ERROR_DEVICE_LOST`: the app
   went to the background (auto-lock during the movies). **Fixes:** `SDL_HINT_IDLE_TIMER_DISABLED`, and a
   lifecycle gate: `apple_main.c` follows `SDL_APP_WILLENTERBACKGROUND` / `DIDENTERFOREGROUND` (event
   watch, main thread), and `nv2a_vk.c` calls a weak `nv2a_vk_host_gate()` before every acquire/submit,
   which blocks in the background. The guest then stalls behind its pushbuffer.
3. **Runs well (owner confirmed):**
   - movies with sound (CoreAudio), profile load, main menu at 59–60 fps, races at a steady 30 fps (the
     title's rate);
   - three background → foreground round trips without losing the device;
   - A15 keeps dynamic vertex strides;
   - no Metal or MoltenVK errors.
- Log retrieval: `devicectl device copy from ... Documents/nfsu2x_log.txt` sometimes times out (error 60)
  while the file is growing; retrying works.

**Open (Phase 2 material):**
- touch controls (the owner has a DualSense; pair it with the phone);
- a game-file import UI (today: Finder or `devicectl` into `Documents/game`);
- FFmpeg VP6 for iOS (the lifted decoder suffices on A15);
- background pause of audio and the guest (only GPU work is gated today);
- thermal and long-session testing;
- a performance profile (Instruments);
- the pre-existing semaphore-reuse validation warning;
- the native-override FMA drift.

## 2026-10-10 — Phase 2: controller, touch controls, background pause, game import

- **DualSense on the iPhone works** (owner confirmed): the SDL controller path from Phase 0 (rescan
  of empty ports in `xinput_device.c`) carries over unchanged.
- **Touch controls** (`src/ios/ios_touch.m`, commit `f8a24a6`):
  - A transparent `UIView` over SDL's Metal view (added after the Vulkan surface is made, so it stays
    on top) draws: a floating stick (left half, it centres where the finger lands), a D-pad,
    BACK/START, A/B/X/Y, WHITE/BLACK (SDL's shoulder buttons) and BRAKE/GAS (the triggers). Each
    press gives a light haptic tap; a finger sliding from one control to another moves the press.
  - It feeds a **virtual SDL game controller** (`SDL_JoystickAttachVirtualEx`, 15 buttons, 6 axes),
    so `xinput_device.c` reads it like any pad and no shared code changed. SDL's automatic mapping
    numbers the buttons and axes exactly as the `SDL_CONTROLLER_*` enums: checked on macOS with a
    throwaway SDL test (every button lands on its own enum; triggers read 0 at rest, 32767 pressed).
  - While a physical controller is connected the overlay hides and the virtual pad is detached, so
    the real pad gets port 0 (`SDL_JOYDEVICEADDED/REMOVED` on the main loop). `NFSU2_TOUCH=0` turns
    it off, `=2` keeps it on with a controller. The accelerometer is no longer a joystick.
  - The iOS Simulator always reports a controller of its own ("Gamepad"), so the overlay hides there
    unless `NFSU2_TOUCH=2`. Layout checked from Simulator screenshots; touch input itself is
    **awaiting a device test** (the Simulator cannot be tapped from the command line).
  - Button functions in NFSU2 are the Xbox ones; the in-game controls screen lists them.
- **Background pause** (`apple_main.c`, `xa2_pause` in `apu_xaudio2.c`, commit `fa5017b`):
  - Before: the device log showed the guest running on in the background (~28k kernel calls before
    suspension) and the APU looping the title's sound buffers, which nothing refilled.
  - Now, on `SDL_APP_WILLENTERBACKGROUND` (also Control Center / notification pull-down), a host
    thread takes the guest lock and holds it until `DIDENTERFOREGROUND`, the SDL audio device is
    paused (its queue dropped on resume), and the GPU gate stays as before. The main thread never
    waits for the lock.
  - Simulator: `[ios] guest paused` right after going to Settings; no kernel calls and a fixed
    `DMA_PUT` while away; back to 59.9 fps after return. Device test pending.
- **Game import** (`src/ios/ios_import.m`, commit `b4e09ba`):
  - When `Documents/game/default.xbe` is missing, a screen asks for the extracted disc folder (system
    folder picker; a folder with the disc one level down also works). It checks free space, copies
    with a progress bar into `game.import` (4 MB chunks, screen kept awake, background task), checks
    `default.xbe` (4,038,656 bytes, SHA-1 `428b5a70…`), then moves the items into `Documents/game`.
    A failed import removes `game.import`. Finder/Files copies still work and skip the screen.
  - `NFSU2_IMPORT_FROM=<dir>` runs the same import without the picker. Simulator: 2,598 MB in ~6 s,
    `diff -rq` against the source identical, then boot to 60 fps from the copy. A fake
    `default.xbe` is refused with its SHA-1 shown and nothing left behind.
- macOS GL and Vulkan builds still compile after each step.

**Open:**
- device test of the touch controls and the background pause;
- FFmpeg VP6 for iOS (an LGPL dylib, per CLAUDE.md; the lifted decoder is enough on the A15 today);
- thermal, long-session testing and an Instruments profile;
- the semaphore-reuse validation warning; the native-override FMA drift.

## 2026-10-10 — Phase 3 plan: native Metal renderer (`nv2a_metal`)

Owner asked to start Phase 3 (Phase 2 device tests of touch and background pause still pending).

**Shape:** a third back end beside `nv2a_gl`/`nv2a_vk`, registered with the executor exactly like
`nv2a_vk` (`draw_raw`, `NV2A_BACKEND_RAW_DIRECT`), ported from `nv2a_vk.c` (same surface/depth/texture
caches, render scale, cube maps from surfaces, instancing, program cache). New files only:
- `xboxrecomp/src/nv2a_metal/nv2a_metal_msl.c` (plain C): MSL source for a program. It reuses the
  shared generators unchanged: the vertex program body from `nv2a_gl_vsh_program()` and the combiner
  body from `nv2a_gl_psh()` in their Vulkan dialect, wrapped in an MSL prelude (GLSL names as MSL
  typedefs/macros; the vertex body runs as a member function of a struct holding the register file).
  Clip space differs from Vulkan only in y (Metal NDC y is up).
- `xboxrecomp/src/nv2a_metal/nv2a_metal.m` (Objective-C, ARC): one `MTLDevice`, queue and
  `CAMetalLayer`; 3 frames in flight, each with its own ring buffer; render passes per surface,
  clears as quads, pipelines keyed on program + blend + colour mask + depth + vertex formats
  (strides dynamic, `MTLBufferLayoutStrideDynamic`, iOS 17 / macOS 14), depth-stencil states cached.
- `xboxrecomp/tests/nv2a_metal_msl/`: compiles the MSL of every program in a `progcache.bin`
  with the Metal compiler (no game data in the repo; the file comes from a run).
- Host: `apple_main.c` makes a Metal view/layer on the main thread when `NFSU2_RENDERER=metal`;
  small guarded hooks in `src/main.c` and CMake. MoltenVK stays the default until Metal matches it.

**Order:** MSL + offline test → back end on macOS (main menu, race; frame dumps against Vulkan) →
iOS Simulator → device (owner) → caches/perf (binary archive, instancing) → default switch.

## 2026-10-10 — Phase 3: native Metal renderer runs on macOS, the Simulator and the iPhone

Commits `8554bef` (MSL + test), `8aa11b9` (back end + wiring), `518ecbe` (static-stride fallback).

- **Shaders** (`nv2a_metal_msl.c`): gl_vsh.c / gl_psh.c Vulkan-dialect output reused unchanged, wrapped
  in MSL (typedefs and macros for the GLSL names; the vertex body as a member function of a struct
  holding the register file; fragment header replaced, `return fragColor;` appended; clip y negated).
  - Gotcha: a `#define texture(t, uv)` macro also expanded inside `[[texture(n)]]` attributes; the
    macros are now defined after the function header.
  - `xboxrecomp/tests/nv2a_metal_msl` (macOS): 200 random vertex programs + combiner states, every
    record of a `progcache.bin`, the renderer's copy/clear shaders, and a rendered triangle that pins
    orientation (pixel (0,0) top left) and winding (clockwise front kept, counter-clockwise culled).
    `build-macos-tests-mtl/nv2a_metal_msl_test build-macos-vk/run/progcache.bin` → 463 pairs,
    0 failures. Metal's own shader cache made the second run 3.7 s instead of 56 s.
- **Back end** (`nv2a_metal.m`, ARC): ported from nv2a_vk.c (surfaces, depth per zeta address and size,
  texture cache, render scale, cube maps from surfaces, instancing, progcache.bin prewarm +
  `mtlpipes.bin`). Metal-specific: clears are quads (no mid-pass clear), presents and cube faces are
  textured quads (blits do not scale), a draw sampling its own target reads a copy, front-and-back
  culled triangles are skipped (no such cull mode), S32K attributes are converted on the CPU (Metal has
  no scaled formats), the border wrap mode is `ClampToZero` (= transparent black border).
  Dynamic vertex strides need an Apple4+ GPU; the iOS Simulator's GPU is not, so strides go into the
  pipeline key there (same as nv2a_vk's `s_static_stride`).
- **Host:** `NFSU2_RENDERER=metal` (env / `nfsu2x_env.txt`) picks it; `apple_main.c` makes the window
  with `SDL_WINDOW_METAL` and the view/layer on the main thread, the background gate is shared with
  Vulkan. Default stays Vulkan (MoltenVK). Shared edits: `src/main.c` +7 lines, CMake +18 lines.
- **macOS (M1)**, Metal API validation on (`MTL_DEBUG_LAYER=1`): movies, title, profile load, main menu,
  Quick Race mode select with the 3D car and reflections, 58 fps; no validation errors. A Quick Race:
  world, lighting, colour grade, HUD, minimap all correct, 30 fps (the title's rate), ~2300 draws →
  ~1370 draw calls (~900 instances appended). First-use compiles ~100 ms per program under validation.
- **iOS Simulator** (iPhone 18 Pro, "Apple iOS simulator GPU"): strides in the pipelines, BC no;
  boots to the Quick Race menu at 59.9 fps; `simctl io screenshot` shows the presented frame correct.
- **iPhone 13 Pro (A15)**: `[MTL] ready: BC no, surfaces up to 16384, vertex strides dynamic`,
  prewarm 70 programs in 2.8 s; menus 59.6–59.9 fps; open-world driving 30.0 fps steady with ~850 draws
  → ~600 draw calls; a frame dump shows rain, lights, car and minimap rendered correctly.
- The same device run on MoltenVK for comparison was cut short: the app went to the background
  (screen locked), and the log shows the Phase 2 background pause working on the device:
  `[ios] background: GPU work, guest and audio paused`, then `[ios] guest paused`.
- Regression: macOS Vulkan default still reaches the menu (57–58 fps); macOS GL and iOS device builds OK.

**Open:**
- Metal vs MoltenVK on the device: CPU time per frame and GPU time (Instruments / `RECOMP_FPS_LOG`
  in a heavier scene), thermals over a long session; then decide whether Metal becomes the default.
- Pipeline compile hitches on first use: async prewarm and/or an `MTLBinaryArchive`.
- Partial-mask colour clears (logged, as in nv2a_vk); point sprites use size 1.
- Device test of the touch controls (Phase 2) still pending.

## 2026-10-11 — Phase 3: measuring Metal against MoltenVK, parallel prewarm

- **`[perf]` line** (`src/apple/apple_perf.m`, commit `fcde7e8`; with `RECOMP_FPS_LOG=1`): every 10 s the
  process's CPU time, the six busiest threads (Mach thread times; the title's main thread is named
  `game`) and the thermal state (`NSProcessInfo`; iOS has no C API for it).
- **macOS race A/B (M1, same race script, 30 fps cap):** Metal 53–55% of a core, renderer/executor
  thread 25–26%; MoltenVK 55–56%, executor 26–27%. Draws per frame ~2450 → ~1500 calls on both
  (instancing behaves alike). `sample` of the Metal executor thread: ~40% waiting in
  `nv2a_pb_exec_method` (traps / flips), most of the rest decoding (`draw_primitive`, `cull_batch`,
  `draw_inline_array`); `mtl_draw_raw` and below only ~11% of the thread (~3% of a core). On the M1
  the back end is too small a part of the frame for the API to show; the A15 (slower CPU, MoltenVK's
  translation on top) is where a difference could appear.
- **Parallel prewarm** (commit `d8b4a9b`): every recorded library and pipeline goes to Metal's
  asynchronous compiler at once. macOS, warm shader cache: 53 programs + 99 pipelines 79 → 26 ms.
  Menu run with Metal validation: picture correct, no first-use compile during the run.
- **Device A/B not done:** the iPhone was only reachable over Wi-Fi (`available (paired)`): settings
  copy and launch went through, but log read-back hung, and earlier the app was backgrounded by the
  screen lock. Needs the phone on USB, unlocked, Auto-Lock off. The phone's `nfsu2x_env.txt` may still
  hold the A/B settings (Metal, race pad script, `RECOMP_FPS_LOG=1`).

## 2026-10-11 — Owner report: laggy cutscenes, speech breaking up; FFmpeg VP6 on Apple

- **Owner:** cutscenes are very laggy; someone talking over the radio sometimes tears up / lags.
- **Device log** (a later launch of mine, the owner's session was overwritten): thermal state already
  `serious` ~20 s after launch (after several back-to-back test runs); during the intro movies the
  game thread at 61% of a core, the executor 31%. SDL output queue 8 x 256 samples (43 ms).
- **Where a movie frame's time goes** (macOS `sample` of the game thread during the intro movies):
  62% spinning in D3D's fence wait `sub_002E8F20` (BlockOnTime: `recomp_spin_yield` → `swtch_pri`,
  i.e. a core kept busy waiting for the executor), 29% `yuv_row_native` (the NEON YUV→ARGB rows,
  ~2.6 ms/frame), 6% the VP6 decode itself. The spin is upstream's design (a sleep there slowed the
  game, UPSTREAM_NOTES); not changed without device numbers. On a phone it costs heat, and heat
  throttles everything — the likely link between the movies and the sound.
- **FFmpeg for Apple** (commit `a149720`): `tools/build_ffmpeg_apple.sh macos|sim|device` builds the
  vp6-only LGPL 2.1 FFmpeg 7.1.1 (sha256 `733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1`,
  no gpg here to check the signature) as `avcodec`/`avutil` **dynamic frameworks** (`@rpath`
  install names), `cmake/apple_ffmpeg.cmake` links and ships them (Xcode embeds and signs, the Ninja
  Simulator build copies them, macOS uses an rpath) with `FFmpeg-COPYING.LGPLv2.1`. Root CMake: one
  `if(APPLE ...)` branch in front of upstream's FFmpeg block.
  - macOS `NFSU2_NATIVE_VP6=2`: 900 frames, 0 differ; 0.9 ms/frame. Game thread during movies 66-70%
    → 60-62% (the rest is the spin and the colour rows).
  - Simulator: decoder FFmpeg, 0.6 ms/frame, APU 1500 frames/s, 0 underruns.
  - Device: built with the frameworks embedded and signed (team YOURTEAMID), installed.
- **Audio diagnostics on Apple** (commit `41ff7dc`): the `[perf]` report now has the APU rate (1500 =
  real time), pacing, output-queue underruns/drops and stale ring-voice samples, so the next device
  log tells whether speech breaks up because the APU falls behind, the game's mixer is late, or the
  output queue runs dry. Next: a device session with movies and the radio speech, then decide (queue
  depth for iOS, the fence spin, thermal).

## 2026-10-11 — Device A/B: Metal is the default; movies via FFmpeg on the phone

- **Getting logs off the phone:** `devicectl device copy from` hung (even for a 100-byte file, also
  after restarting the Mac's CoreDeviceService) after several runs; installs and process control still
  worked. Runs are now driven with `devicectl device process launch --console` and the settings as
  `DEVICECTL_CHILD_*` launch variables: `NFSU2_LOG_CONSOLE=1` keeps the log on the console,
  `NFSU2_ENV_FILE=0` skips `nfsu2x_env.txt` (`src/ios/ios_host.c`, commit `48fa407`). Later, with the app
  stopped, file copies worked again (37 s for a small file).
- **iPhone 13 Pro, same scripted run (movies → menus → driving), 3 min each, Metal first:**

  | | Metal | MoltenVK |
  |---|---|---|
  | prewarm | 105 programs, 192 pipelines, 27 ms | 106 programs, 150 pipelines, 259 ms |
  | movies (CPU) | 108–117% | 107–124% |
  | menus | 47–51%, 59.9 fps | 49–52%, 59.9 fps |
  | driving (CPU / renderer thread) | 48–54% / 21–24% | 50–57% / 23–26% |
  | draws → calls | ~2450 → ~1500 | ~2500 → ~1550 |
  | thermal | nominal → fair | fair → serious (ran second, warmer phone) |

  Pictures correct throughout (earlier dumps: menus, race, open world). **Metal is now the default on
  Apple** (commit `b76aeac`); `NFSU2_RENDERER=vulkan` picks MoltenVK.
- **Movies on the phone:** FFmpeg 0.5–1.1 ms/frame, presented at 59.9 fps. The game thread during
  movies is still ~60–65% of a core: D3D's fence spin and the YUV rows (see the movie profile above).
- **Sound in these runs:** APU 1500 frames/s throughout, no output underruns after the first 10 s of
  the first launch. One spike of stale ring samples (2.1%) in the window where FMVOpening starts —
  the Mac shows the same 2.19% at the same point, so it is the movie switch, not the device. The
  owner's speech breaking up (career radio calls) was not reached by the scripted run: needs a log of
  a real session. The phone's settings file now holds only `RECOMP_FPS_LOG=1` (no pad script).

## 2026-10-12 — Radio speech in career free-roam drops out (owner report, open)

- **Owner:** cutscenes now look fine. In career free-roam the radio speech plays a word or two, then
  seconds of silence, then another word or two; not distorted. Cutscene audio and everything else play
  fine.
- **Owner's device session log** (pulled with `devicectl copy from`, works when the app is not
  running): CPU ~50% of a core, thermal **nominal** throughout, APU 1500 frames/s, 0 output underruns,
  stale ring samples ~0% (two spikes at loads), every file read succeeded (the only short reads are save
  metadata at EOF). So not the device being slow, hot, or the output running dry.
- **How NFSU2 plays sound** (new `RECOMP_APU_VOICE_LOG=1` in `apu_vp.c`: voice on/off/pause, stream
  packet lists, stream starvation, ms stamps): the title switches on all its hardware voices once
  (v0BA–v0D7 looping buffers, v0F4–v0F6 rings) and never off; no voice is a DirectSound stream (no SSL
  packets at all). Its software engine refills those buffers, so speech that goes silent means the
  game feeds it late or not at all — the cause is on the guest side (speech streaming/decoding or its
  timing), not in the APU's stream path.
- Mac reproduction with the phone's save (`~/nfsu2/game-phone`: links to the disc + the phone's
  `UDATA`): the pad script reaches career explore mode (Main Menu → left → A …, garage
  "Return to Explore Mode?" → left → A), but driving/radio triggers could not be scripted, and audio
  cannot be judged by ear here. **Next:** the owner checks free-roam radio on the Mac build (same save,
  DualSense over Bluetooth); if it breaks up there too it is a port bug shared by all platforms,
  otherwise iOS-specific.
- **Game files ruled out (owner asked whether the image could be bad):** new `NFSU2_VERIFY=1` (launch
  variable or settings line, `ios_import.m`) logs `[verify] <sha1> <bytes> <path>` for every disc file
  before boot (UDATA/TDATA skipped). iPhone: 185 files, 2598 MB, 0 unreadable, 3 s; identical to
  `shasum` of `~/nfsu2/game` on the Mac, file for file. `default.xbe` is the known NTSC-U build. The
  original `.7z` is gone, so the image itself cannot be checked against its archive CRCs; a damaged
  image would affect the Mac the same way, which the owner's Mac test will show.
- **Owner's Mac test (2026-10-12):** career free-roam radio speech plays correctly on the macOS build
  with the same game files (phone's save). So the disc image and the lifted code are fine and the
  dropouts are iOS-specific. Owner chose not to test further on the iPhone for now: **left open,
  not fixed** (no change since the owner's phone test touches it). Candidates for when it is picked up:
  iOS thread scheduling of the title's stream/speech worker threads (default QoS can land them on
  efficiency cores; CLAUDE.md's "map thread priorities to QoS" is not done yet), then a device run with
  `RECOMP_APU_VOICE_LOG=1` + a `RECOMP_AUDIO_DUMP` of the radio moment compared with the Mac.

## 2026-10-12 — Thread QoS on Apple (to-do item: "map thread priorities to QoS")

- **Measured first** (`[perf]` lines now show each busy thread's scheduling priority, `pN`, which reflects
  its QoS class): the title's main thread, the executor and every worker ran at **p31 = the default
  class**; only system threads higher (p46). Darwin pthreads do not inherit their creator's QoS, and iOS
  readily runs default-class work on the efficiency cores — a candidate for the iOS-only radio dropouts.
- **Change** (`xboxrecomp/src/platform/apple_qos.c`, new; two guarded calls in `win32_compat.c`, one in
  `apple_main.c`; `RECOMP_QOS=0` off): a thread sets its own class from its Win32 priority when it starts
  and when it changes its own priority — above normal..time critical (APU frame thread, EA mixer):
  user-interactive; normal (game threads, the title's main thread): user-interactive −1; below normal /
  lowest: user-initiated; idle: utility. Audio stays above the game threads.
- **macOS:** busy threads p46 (audio p47); menus 59.9 fps, APU 1500 frames/s. Same race, A/B:
  `RECOMP_QOS=0` 74–88% of a core, QoS on 69–79% — no extra CPU from spin-waits at the higher class;
  30 fps, 0 underruns, ~0% stale ring samples.
- **iPhone:** built, installed; a short console run booted normally through the movies, then the phone
  locked (background pause worked), so no `[perf]` readout from the device. Whether this cures the radio
  dropouts needs the owner's ear in career free-roam (left to the owner).
