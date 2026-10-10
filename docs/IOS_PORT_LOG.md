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
