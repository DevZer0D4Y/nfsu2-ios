/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* The generated register model (g_eax/g_esp are thread-local there) and the
 * XBOX_PTR/MEM32 accessors; the register names below are its macros. */
#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

extern ptrdiff_t g_xbox_mem_offset;

/* ── CRT memmove/memcpy ───────────────────────────────────────
 *
 * Two copies of the MSVC CRT memmove are linked (0x002A7EE0 and 0x002A9450;
 * memcpy is the same body). Their tail and backward-copy paths dispatch
 * through UnwindDown-style vectors indexed with a *negative* register -- the
 * table address in the instruction is the end of the table -- which the
 * lifter's forward table reader cannot follow, so those jumps were lowered
 * to indirect tail calls into the middle of the function and failed to
 * resolve: every copy whose length was not a multiple of 4, and every
 * overlapping backward copy, silently lost its tail.
 *
 * Replaced by the host memmove. cdecl (dst, src, count), returns dst, and
 * esi/edi are preserved, as the original restores them. Generated bodies are
 * skipped by tools.recomp --exclude-manual reading this file. */
static void crt_memmove(void)
{
    uint32_t dst = MEM32(esp + 4);
    uint32_t src = MEM32(esp + 8);
    uint32_t n   = MEM32(esp + 12);

    if (n)
        memmove((void *)XBOX_PTR(dst), (const void *)XBOX_PTR(src), n);
    eax = dst;
    esp += 4;   /* return address; cdecl, the caller pops the arguments */
}

void sub_002A7EE0(void) { crt_memmove(); }

/* ── Bounding box against the view frustum, sub_0009A330 ─────────────
 *
 * thiscall (this, const float min[3], const float max[3], matrix), ret 12.
 * The box's centre c = (max + min) * K and half-size e = max - c (both
 * stored as floats, as the original does), then for each of six planes
 * (n, d) at [this] + 0x140 + 16k: r = |n|.e, dist = n.c + d; below K2 on
 * dist + r means outside (return 0); below K2 on dist - r means the box
 * straddles that plane. Returns 2 when inside all six, 1 when straddling.
 * With a matrix the box is first transformed by it (sub_0009A250, below).
 *
 * It is the hottest function of NFSU2's main thread in a race (~5% on the
 * console): every object, every view, every frame. Written in C it keeps
 * everything in registers. The arithmetic is the lifted code's -- doubles,
 * in the same order -- so results match it exactly; RECOMP_NATIVE_CHECK=1
 * runs both and reports any difference. RECOMP_NATIVE=0 turns it off. */
extern void sub_0009A330_gen(void);
extern void sub_0009A250_gen(void);

/* sub_0009A250: cdecl (matrix, float min[3], float max[3]) -- the box's
 * corners transformed by a 4x4 row-major matrix, as a box again (Arvo): both
 * start at the translation row, and for each row i and column j the larger
 * of min[i]*M[i][j] and max[i]*M[i][j] goes to max[j], the smaller to min[j].
 * As the original: the max product is rounded to float before the compare
 * (it goes through memory), the sums are kept in extended (here double)
 * precision and stored as floats at the end, and an unordered compare adds
 * the min product to max. */
static void box_transform(uint32_t m, float mn[3], float mx[3])
{
    double hi[3], lo[3];
    int i, j;
    for (j = 0; j < 3; j++)
        hi[j] = lo[j] = (double)MEMF(m + 0x30u + 4u * j);
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) {
            double mm = (double)MEMF(m + 16u * i + 4u * j);
            double e = (double)mn[i] * mm;
            double f = (double)(float)((double)mx[i] * mm);
            if (e < f) { hi[j] += f; lo[j] += e; }
            else       { hi[j] += e; lo[j] += f; }
        }
    for (j = 0; j < 3; j++) {
        mn[j] = (float)lo[j];
        mx[j] = (float)hi[j];
    }
}

static uint32_t frustum_box_native(uint32_t self, uint32_t pmin, uint32_t pmax,
                                   uint32_t matrix, uint32_t *out_ecx, uint32_t *out_edx)
{
    const double K = (double)MEMF(0x003408BCu), K2 = (double)MEMF(0x0033FA8Cu);
    float mn[3] = { MEMF(pmin), MEMF(pmin + 4), MEMF(pmin + 8) };
    float mx[3] = { MEMF(pmax), MEMF(pmax + 4), MEMF(pmax + 8) };
    float ax, ay, az, bx, by, bz;
    if (matrix)
        box_transform(matrix, mn, mx);
    ax = mn[0]; ay = mn[1]; az = mn[2];
    bx = mx[0]; by = mx[1]; bz = mx[2];
    float cx = (float)(((double)bx + (double)ax) * K);
    float cy = (float)(((double)by + (double)ay) * K);
    float cz = (float)(((double)bz + (double)az) * K);
    float ex = (float)((double)bx - (double)cx);
    float ey = (float)((double)by - (double)cy);
    float ez = (float)((double)bz - (double)cz);
    uint32_t planes = MEM32(self) + 0x144u, k;
    int straddle = 0;

    for (k = 0; k < 6; k++) {
        uint32_t p = planes + 16u * k;
        double nx = MEMF(p - 4), ny = MEMF(p), nz = MEMF(p + 4), d = MEMF(p + 8);
        double r = (fabs(ny) * ey + fabs(nz) * ez) + fabs(nx) * ex;
        double dist = ((cy * ny + cz * nz) + cx * nx) + d;
        if (dist + r < K2) {
            *out_ecx = p;
            *out_edx = k + 1;
            return 0;
        }
        if (dist - r < K2)
            straddle = 1;
    }
    *out_ecx = planes + 96u;
    *out_edx = 7;
    return straddle ? 1u : 2u;
}

void sub_0009A330(void)
{
    static int mode = -1;               /* 0 lifted, 1 native, 2 native + check */
    static unsigned long calls, with_matrix, mismatches;
    uint32_t self = ecx, pmin = MEM32(esp + 4), pmax = MEM32(esp + 8);
    uint32_t matrix = MEM32(esp + 12), r, rc, rd;

    if (mode < 0) {
        const char *e = getenv("RECOMP_NATIVE"), *c = getenv("RECOMP_NATIVE_CHECK");
        mode = (e && *e == '0') ? 0 : (c && *c == '1') ? 2 : 1;
    }
    calls++;
    with_matrix += matrix != 0;
    if (mode == 0) {
        sub_0009A330_gen();
        return;
    }
    r = frustum_box_native(self, pmin, pmax, matrix, &rc, &rd);
    if (mode == 2) {
        sub_0009A330_gen();             /* pops its own arguments */
        if (eax != r && mismatches++ < 20)
            fprintf(stderr, "[native] sub_0009A330 mismatch: lifted %u native %u "
                    "(box %08X-%08X this %08X)\n", eax, r, pmin, pmax, self);
        if ((calls & 0xFFFFF) == 0)
            fprintf(stderr, "[native] sub_0009A330: %lu calls, %lu with a matrix, "
                    "%lu mismatches\n", calls, with_matrix, mismatches);
        return;
    }
    eax = r;
    ecx = rc;
    edx = rd;
    esp += 16;                          /* return address + three arguments */
}

/* sub_0009A250 on its own (it has other callers): box_transform on guest
 * memory. Returns max (eax), as the original leaves it; cdecl. */
void sub_0009A250(void)
{
    static int mode = -1;
    uint32_t m = MEM32(esp + 4), pmin = MEM32(esp + 8), pmax = MEM32(esp + 12);
    float mn[3], mx[3];
    int j;

    if (mode < 0) {
        const char *e = getenv("RECOMP_NATIVE"), *c = getenv("RECOMP_NATIVE_CHECK");
        mode = (e && *e == '0') ? 0 : (c && *c == '1') ? 2 : 1;
    }
    if (!mode) {
        sub_0009A250_gen();
        return;
    }
    for (j = 0; j < 3; j++) {
        mn[j] = MEMF(pmin + 4u * j);
        mx[j] = MEMF(pmax + 4u * j);
    }
    box_transform(m, mn, mx);
    if (mode == 2) {
        static unsigned long calls, bad;
        sub_0009A250_gen();             /* writes the guest's boxes itself */
        calls++;
        for (j = 0; j < 3; j++)
            if (memcmp(&mn[j], (const void *)XBOX_PTR(pmin + 4u * j), 4)
                || memcmp(&mx[j], (const void *)XBOX_PTR(pmax + 4u * j), 4)) {
                if (bad++ < 20)
                    fprintf(stderr, "[native] sub_0009A250 mismatch at %u: native %g %g,"
                            " lifted %g %g\n", j, mn[j], mx[j],
                            MEMF(pmin + 4u * j), MEMF(pmax + 4u * j));
                break;
            }
        if ((calls & 0xFFFFF) == 0)
            fprintf(stderr, "[native] sub_0009A250: %lu calls, %lu mismatches\n", calls, bad);
        return;
    }
    for (j = 0; j < 3; j++) {
        MEMF(pmin + 4u * j) = mn[j];
        MEMF(pmax + 4u * j) = mx[j];
    }
    eax = pmax;
    ecx = pmin + 12u;
    edx = m + 8u + 48u;
    esp += 4;                           /* cdecl: the caller pops the arguments */
}
void sub_002A9450(void) { crt_memmove(); }

/* ── D3D fence wait, D3D_BlockOnTime (0x002E8F20) ────────────
 *
 * stdcall (fence, flags), ret 8. The stock XDK 5849 D3D keeps its device at
 * 0x002F77A0, reached through the global at 0x002F7798:
 *   +0x2C  write sequence, bumped as fences are inserted
 *   +0x30  pointer to the semaphore the GPU releases as it completes work
 * and this spins until *(+0x30) catches up with the fence it was asked for.
 *
 * Without the pushbuffer executor nothing ever releases the semaphore, so the
 * GPU is reported caught up. With it (RECOMP_PB_EXEC) that would let D3D
 * reuse ring space and vertex memory the executor has not read yet (see the
 * toolkit's d3d8ltcg-device-context.md), so instead the real semaphore is
 * handed to the executor once and the original wait runs against it. */
#define NFSU2_D3D_DEVICE_PTR 0x002F7798u
extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);
extern void sub_002E8F20_gen(void);

void sub_002E8F20(void)
{
    static int exec = -1;
    uint32_t dev = MEM32(NFSU2_D3D_DEVICE_PTR);
    uint32_t sem = dev ? MEM32(dev + 0x30) : 0;

    if (exec < 0)
        exec = getenv("RECOMP_PB_EXEC") != NULL;
    if (exec) {
        static uint32_t registered;
        if (sem && sem != registered) {
            nv2a_pb_set_semaphore_target(sem);
            fprintf(stderr, "[D3D] GPU semaphore at 0x%08X\n", sem);
            registered = sem;
        }
        if (MEM32(esp) == 0x002E953Cu && MEM32(esp + 12) == 0x000AEDDBu) {
            /* The main loop (sub_000AEA90) calls BlockOnFence on the fence
             * of the frame it just built, right before Present: the whole
             * frame has to be through the GPU before the next one starts,
             * so the game and the pushbuffer executor take turns instead
             * of overlapping (Eden race: game waited ~50% of the time,
             * executor ~20% idle). RECOMP_FRAME_LAG=1 waits for the
             * previous frame's fence instead: one frame in flight, as on a
             * PC. Race frames checked on Linux, no corruption. */
            static int lag = -1;
            static uint32_t prev;
            if (lag < 0) {
                const char *e = getenv("RECOMP_FRAME_LAG");
                lag = e && *e == '1';
                fprintf(stderr, "[D3D] frame fence: %s\n",
                        lag ? "previous frame (RECOMP_FRAME_LAG=1)" : "this frame");
            }
            if (lag) {
                uint32_t mine = MEM32(esp + 4);
                if (prev)
                    MEM32(esp + 4) = prev;
                prev = mine;
            }
        }
        sub_002E8F20_gen();
        return;
    }
    if (sem)
        MEM32(sem) = MEM32(dev + 0x2C);     /* "GPU" is always caught up */
    eax = 0;
    esp += 4 + 8;                           /* return address, 2 stdcall args */
}

/* ── D3D interrupt handlers at DISPATCH_LEVEL ────────────────
 *
 * D3D services the GPU from its DPC (0x002F2530) and, with the interrupt
 * masked, from its own busy-waits (BlockUntilIdle and friends) on the game
 * thread: both call the vblank handler (0x002F1D80) and the PGRAPH handler
 * (0x002F22F0), which queues flips. On the Xbox the two never interleave --
 * one is a DPC, the other runs with the interrupt off. Here the DPC runs on
 * the kernel timer thread, and lifted code hands over the guest lock at any
 * function entry, so the vblank count could move between D3D computing a
 * flip's target vblank and checking it. D3D retires a flip only when the
 * count *equals* its target, so that flip stayed pending forever and
 * PersistDisplay, which waits for pending flips, never returned: the hang
 * starting a Quick Race or Career. Run at DISPATCH_LEVEL (the dispatch lock,
 * kernel_hal.c), neither can be interrupted by the other.
 *
 * Raising blocks on the lock that the timer thread holds while it waits for
 * the guest lock, so the guest lock is let go for the raise, as a kernel
 * call would; already at DISPATCH (inside the DPC) there is nothing to take. */
extern uint8_t xbox_KfRaiseIrql(uint8_t irql);
extern void xbox_KfLowerIrql(uint8_t irql);
extern uint8_t xbox_CurrentIrql(void);
extern int xbox_gil_suspend(void);
extern void xbox_gil_resume(int depth);
extern void xbox_Nv2aVblankTaken(void);

static uint8_t d3d_isr_enter(void)
{
    uint8_t old = xbox_CurrentIrql();

    if (old < 2) {
        int d = xbox_gil_suspend();
        old = xbox_KfRaiseIrql(2);
        xbox_gil_resume(d);
    }
    return old;
}

static void d3d_isr_leave(uint8_t old)
{
    if (old < 2)
        xbox_KfLowerIrql(old);
}

/* Flips retired by a handler call: D3D's retire index, at +0x1BC of the
 * block both handlers take in ecx (their own reads index the pending-flip
 * slots from it). The pushbuffer executor's FLIP_STALL waits on these, as
 * PGRAPH waits on the retire's PGRAPH_INCREMENT write. */
extern void nv2a_pb_flip_retired(unsigned n);

/* Vblank handler, thiscall. It ends by writing PCRTC_INTR and spinning until
 * PMC_INTR bit 24 drops. Its callers (the DPC, D3D's busy-waits) have read
 * the bits before calling it, so this is the vblank being taken: clear them
 * now, and the spin ends at once instead of holding the guest lock until the
 * runtime's ack thread comes round. */
extern void sub_002F1D80_gen(void);

void sub_002F1D80(void)
{
    uint32_t blk = ecx;
    uint32_t head = MEM32(blk + 0x1BC);
    uint8_t old = d3d_isr_enter();

    xbox_Nv2aVblankTaken();
    sub_002F1D80_gen();
    nv2a_pb_flip_retired(MEM32(blk + 0x1BC) - head);
    d3d_isr_leave(old);
}

/* PGRAPH handler, thiscall, ecx = the device's hardware block (+0 =
 * 0xFD000000). Reads a software-method trap from PGRAPH_INTR / NSOURCE /
 * TRAPPED_ADDR / TRAPPED_DATA, acknowledges it by writing PGRAPH_INTR back,
 * and acts on it: NOP(5) sets the event BlockOnTime sleeps on, a flip trap
 * queues the flip (sub_002F2080). The acknowledge is write-1-to-clear on
 * hardware and a no-op on RAM. Left pending, every turn of a busy-wait took
 * the same trap again (tens of thousands of flips a second, until the flip
 * targets were millions of vblanks ahead); guessed from the handler's
 * PGRAPH_FIFO writes, a trap posted during a call was sometimes dropped (the
 * fence event never set). So the executor posts traps under a lock held here
 * around the call, and a call that found one reports it taken, which clears
 * it (nv2a_pb_exec.c). */
extern void sub_002F22F0_gen(void);
extern void nv2a_pb_trap_lock(void);
extern void nv2a_pb_trap_unlock(void);
extern void nv2a_pb_trap_taken(void);

void sub_002F22F0(void)
{
    volatile uint32_t *nv = (volatile uint32_t *)XBOX_PTR(0xFD000000u);
    uint32_t blk = ecx;
    uint32_t head = MEM32(blk + 0x1BC);
    uint8_t old = d3d_isr_enter();
    uint32_t nsource;

    nv2a_pb_trap_lock();
    nsource = nv[0x400108 / 4];
    sub_002F22F0_gen();
    nv2a_pb_flip_retired(MEM32(blk + 0x1BC) - head);   /* a flip queued on its vblank retires at once */
    if (nsource)
        nv2a_pb_trap_taken();
    nv2a_pb_trap_unlock();
    d3d_isr_leave(old);
}

/* ── Stream read, unlocked precheck (0x00072F90) ─────────────
 *
 * cdecl (stream). Checks a 'STRM' stream's byte count at +8 and reads its
 * buffer descriptor through +0xC -- before taking the stream's lock
 * (sub_00251C41, the call after the reads). The stream thread fills those
 * fields in at the same moment. On the Xbox's one CPU the gap is almost
 * never hit; with guest threads running in parallel, the reader saw a count
 * with +0xC still holding the allocator's 0xAA fill and dereferenced
 * 0xAAAAAAAA -- the crash after Start, nearly every time on a Switch, now
 * and then on a PC. A descriptor that is not a pointer yet means the stream
 * is not ready: answer as the function does with no data (0), and the caller
 * asks again. */
extern void sub_00072F90_gen(void);

void sub_00072F90(void)
{
    uint32_t stream = MEM32(esp + 4);

    if (stream && MEM32(stream + 8)) {
        uint32_t hdr = MEM32(stream);
        uint32_t desc = MEM32(stream + 0xC);
        if (hdr && MEM32(hdr) == 0x4D525453u                /* 'STRM' */
                && (desc < 0x10000u || desc == 0xAAAAAAAAu)) {
            /* Not ready: let the stream thread (which may be waiting for the
             * guest lock) get on with filling it in, then say "no data". */
            recomp_spin_yield();
            eax = 0;
            esp += 4;                   /* return address; the caller pops the argument */
            return;
        }
    }
    sub_00072F90_gen();
}

/* ── VP6 movie frames, sub_002618F0 ──────────────────────────
 *
 * cdecl (decoder, data, size, width, height): decodes one VP6 frame (the
 * payload of an EA MV0K/MV0F chunk) with On2's decoder. The decoder block:
 *   +0x1B0/+0x1B4  picture width/height    +0x1B8/+0x1BC  Y/UV stride
 *   +0x21C/+0x220/+0x224  Y/U/V offsets in a frame buffer
 *   +0x244  buffer being decoded   +0x254  last decoded (sub_0026144D's
 *           output, and the reference) -- swapped after every frame
 * Planes are bottom-up with a 48 (Y) / 24 (UV) pixel border for motion
 * vectors past the edge. The movie player (sub_0025F909) then passes +0x254
 * on to be drawn.
 *
 * Lifted, this is most of the CPU time of a movie on the Switch, so by
 * default FFmpeg decodes the frame (src/movie_vp6.c) into +0x244 and the
 * buffers are swapped as the original does. The border is not filled in:
 * only On2's own motion compensation read it. NFSU2_NATIVE_VP6=0 runs the
 * lifted decoder, =2 runs both and compares the pictures. */
#ifdef NFSU2_NATIVE_VP6
#include "movie_vp6.h"

extern void sub_002618F0_gen(void);

typedef struct {
    uint32_t y, u, v, y_stride, uv_stride, w, h;
} Vp6Planes;

static Vp6Planes vp6_planes(uint32_t dec, uint32_t buf)
{
    Vp6Planes p;
    p.y_stride = MEM32(dec + 0x1B8);
    p.uv_stride = MEM32(dec + 0x1BC);
    p.y = buf + MEM32(dec + 0x21C) + (p.y_stride + 1u) * 48u;
    p.u = buf + MEM32(dec + 0x220) + (p.uv_stride + 1u) * 24u;
    p.v = buf + MEM32(dec + 0x224) + (p.uv_stride + 1u) * 24u;
    p.w = MEM32(dec + 0x1B0);
    p.h = MEM32(dec + 0x1B4);
    return p;
}

/* NFSU2_NATIVE_VP6=2: FFmpeg into a scratch picture, compared with what the
 * lifted decoder left at +0x254. */
static void vp6_check(uint32_t dec, uint32_t data, uint32_t size)
{
    static unsigned long frames, bad;
    static uint8_t *pic;
    Vp6Planes p = vp6_planes(dec, MEM32(dec + 0x254));
    uint32_t cw = p.w / 2, row, diff = 0;

    if (p.w > 4096 || p.h > 4096)
        return;
    if (!pic && !(pic = malloc(4096u * 4096u * 3u / 2u)))
        return;
    if (nfsu2_vp6_decode(dec, (const uint8_t *)XBOX_PTR(data), (int)size,
                         pic, pic + p.w * p.h, pic + p.w * p.h + cw * (p.h / 2),
                         (int)p.w, (int)cw, (int)p.w, (int)p.h) == 0) {
        for (row = 0; row < p.h; row++)
            diff += memcmp(pic + row * p.w, (const void *)XBOX_PTR(p.y + row * p.y_stride), p.w) != 0;
        for (row = 0; row < p.h / 2; row++) {
            diff += memcmp(pic + p.w * p.h + row * cw,
                           (const void *)XBOX_PTR(p.u + row * p.uv_stride), cw) != 0;
            diff += memcmp(pic + p.w * p.h + cw * (p.h / 2) + row * cw,
                           (const void *)XBOX_PTR(p.v + row * p.uv_stride), cw) != 0;
        }
    } else {
        diff = 1;
    }
    frames++;
    if (diff && bad++ < 20)
        fprintf(stderr, "[movie] VP6 check: frame %lu (%u bytes) differs in %u rows\n",
                frames, size, diff);
    if (frames % 300 == 0)
        fprintf(stderr, "[movie] VP6 check: %lu frames, %lu differ\n", frames, bad);
}

void sub_002618F0(void)
{
    int mode = nfsu2_vp6_mode();
    uint32_t dec = MEM32(esp + 4), data = MEM32(esp + 8), size = MEM32(esp + 12);
    uint32_t cur;
    Vp6Planes p;

    if (mode != 1) {
        sub_002618F0_gen();
        if (mode == 2 && eax == 0)
            vp6_check(dec, data, size);
        return;
    }
    cur = MEM32(dec + 0x244);
    p = vp6_planes(dec, cur);
    if (nfsu2_vp6_decode(dec, (const uint8_t *)XBOX_PTR(data), (int)size,
                         (uint8_t *)XBOX_PTR(p.y), (uint8_t *)XBOX_PTR(p.u),
                         (uint8_t *)XBOX_PTR(p.v), (int)p.y_stride, (int)p.uv_stride,
                         (int)p.w, (int)p.h) == 0) {
        MEM32(dec + 0x244) = MEM32(dec + 0x254);
        MEM32(dec + 0x254) = cur;
    }
    /* A frame FFmpeg rejects leaves the last picture up. */
    MEM32(dec + 0x1E8) = size;
    MEM32(0x00469D04u) += 1;            /* the decoder's frame counter */
    eax = 0;
    esp += 4;                           /* cdecl: the caller pops the arguments */
}
#else
/* Built without FFmpeg (NFSU2_FFMPEG_DIR): the lifted decoder. */
extern void sub_002618F0_gen(void);
void sub_002618F0(void) { sub_002618F0_gen(); }
#endif

/* ── DirectSound DSP command post (0x0032EB65) ───────────────
 *
 * thiscall, ecx = the DSP-side object. Copies a command block into the GP
 * DSP's scratch area, stores the command code at scratch + 0x810, and spins
 * until the DSP zeroes it (this loop does re-read memory). The scratch block
 * is ***(this + 8 + 0x10), the same chain the runtime's RECOMP_DSP_ACK notes
 * describe. There is no DSP, so register the word with the runtime's
 * completion list before running the original body. */
extern int xbox_ApuDspAckWord(uint32_t va);
extern void sub_0032EB65_gen(void);

void sub_0032EB65(void)
{
    uint32_t obj = MEM32(ecx + 8);
    uint32_t blk = obj ? MEM32(obj + 0x10) : 0;
    uint32_t scratch = blk ? MEM32(blk) : 0;

    if (scratch)
        xbox_ApuDspAckWord(scratch + 0x810);
    sub_0032EB65_gen();
}

/* ── DirectSound AC'97 channel reset (0x0033518D) ────────────
 *
 * thiscall, ecx = channel object. Sets RR (bit 1) in the channel's NABM
 * control byte, then waits for the controller to clear it -- except MSVC
 * hoisted the load out of the loop, so the original reads the byte once and
 * spins on the register copy forever unless the bit is already clear at that
 * one read:
 *
 *     mov  byte [eax+0xFEC0010B], 2
 *     mov  cl, [eax+0xFEC0010B]
 *     and  cl, 2
 *   L: test cl, cl
 *     jne  L
 *
 * On hardware the reset has completed by then. The Windows runtime answers it
 * with a write trap on the NABM page; there is no fault handling on POSIX or
 * Switch, so this is the same function with the reset completing at once.
 * Everything else -- the lock taken through sub_0032BFF0/sub_0032C012, the
 * buffer-descriptor base store, the 0xFEC0017C write for channel 1 and the
 * sub_00334F97 re-arm -- is as the original does it. */
void sub_0033518D(void)
{
    uint32_t ebp_saved = g_ebp, frame, chan, nabm;

    PUSH32(esp, ebp_saved);
    frame = esp;
    g_ebp = frame;
    g_seh_ebp = frame;
    esp -= 8;
    MEM32(frame - 4) = 0;
    PUSH32(esp, esi);
    esi = ecx;
    chan = esi;

    ecx = frame - 8;
    PUSH32(esp, 0x003351A1u); RECOMP_ABI_CALL(0x0032BFF0u, sub_0032BFF0);

    nabm = MEM32(MEM32(chan) * 4 + 0x335DE0u);
    MEM8(nabm + 0xFEC0010Bu) = 2;
    MEM8(nabm + 0xFEC0010Bu) &= (uint8_t)~2u;   /* reset complete */
    eax = nabm;
    SET_LO8(ecx, 0);

    MEM32(nabm + 0xFEC00100u) = MEM32(chan + 0x1C);
    if (MEM32(chan) == 1)
        MEM32(0xFEC0017Cu) = MEM32(chan + 0x28);

    PUSH32(esp, 1);
    PUSH32(esp, 1);
    ecx = chan;
    PUSH32(esp, MEM8(chan + 0x24));
    PUSH32(esp, MEM8(chan + 0x25));
    eax = MEM8(chan + 0x25);
    g_ebp = frame;
    g_seh_ebp = frame;
    PUSH32(esp, 0x003351F4u); RECOMP_ABI_CALL(0x00334F97u, sub_00334F97);

    ecx = frame - 8;
    g_ebp = frame;
    g_seh_ebp = frame;
    PUSH32(esp, 0x003351FCu); RECOMP_ABI_CALL(0x0032C012u, sub_0032C012);

    POP32(esp, esi);
    esp = frame;
    POP32(esp, ebp_saved);
    g_ebp = ebp_saved;
    esp += 4;
}

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}
