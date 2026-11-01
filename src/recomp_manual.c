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
#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif

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
 * The MSVC CRT memmove (0x002442A0, Carbon's 0x0032C800 byte for byte;
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

void sub_002442A0(void) { crt_memmove(); }

/* ── D3D fence wait, D3D_BlockOnTime (0x00351770) ────────────
 *
 * Burnout's D3D is the link-time-optimised build (D3D8LTCG), so the calling
 * conventions are its own: the fence comes in eax, the flags on the stack,
 * ret 4. The device (0x0035D6A0) is reached through the global at 0x0035FB48:
 *   +0x2C  write sequence, bumped as fences are inserted
 *   +0x30  pointer to the semaphore the GPU releases as it completes work
 * and this spins until *(+0x30) catches up with the fence it was asked for.
 * Same body as NFSU2/Carbon's BlockOnTime apart from the conventions.
 *
 * Without the pushbuffer executor nothing ever releases the semaphore, so the
 * GPU is reported caught up. With it (RECOMP_PB_EXEC) that would let D3D
 * reuse ring space and vertex memory the executor has not read yet (see the
 * toolkit's d3d8ltcg-device-context.md), so instead the real semaphore is
 * handed to the executor once and the original wait runs against it. */
#define B3_D3D_DEVICE_PTR 0x0035FB48u
extern unsigned long long GetTickCount64(void);
extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);
extern void sub_00351770_gen(void);

void sub_00351770(void)
{
    static int exec = -1;
    uint32_t dev = MEM32(B3_D3D_DEVICE_PTR);
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
        if (getenv("B3_D3D_TRACE")) {
            uint32_t fence = eax, ret = MEM32(esp), flags = MEM32(esp + 4);
            uint64_t t0 = GetTickCount64();
            sub_00351770_gen();
            if (GetTickCount64() - t0 >= 20)
                fprintf(stderr, "[d3d] BlockOnTime %llu ms: fence %u flags %u seq %u sem %u ret %08X\n",
                        (unsigned long long)(GetTickCount64() - t0), fence, flags,
                        MEM32(dev + 0x2C), MEM32(sem), ret);
            return;
        }
        sub_00351770_gen();
        return;
    }
    if (sem)
        MEM32(sem) = MEM32(dev + 0x2C);     /* "GPU" is always caught up */
    eax = 0;
    esp += 4 + 4;                           /* return address, the flags */
}

/* ── D3D interrupt handlers at DISPATCH_LEVEL ────────────────
 *
 * D3D services the GPU from its DPC and, with the interrupt masked, from its
 * own busy-waits (BlockUntilIdle and friends) on the game thread: both call
 * the vblank handler (0x00356DA0) and the PGRAPH handler (0x00357000), which
 * queues flips. On the Xbox the two never interleave -- one is a DPC, the
 * other runs with the interrupt off. Here the DPC runs on the kernel timer
 * thread, and lifted code hands over the guest lock at any function entry, so
 * the vblank count could move between D3D computing a flip's target vblank
 * and checking it. D3D retires a flip only when the count *equals* its
 * target, so that flip stayed pending forever and PersistDisplay, which
 * waits for pending flips, never returned (NFSU2: the hang starting a race).
 * Run at DISPATCH_LEVEL (the dispatch lock, kernel_hal.c), neither can be
 * interrupted by the other.
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
 * block both handlers are handed (their own reads index the pending-flip
 * slots from it). The pushbuffer executor's FLIP_STALL waits on these, as
 * PGRAPH waits on the retire's PGRAPH_INCREMENT write. */
extern void nv2a_pb_flip_retired(unsigned n);

/* Vblank handler; the block comes in eax (LTCG; NFSU2/Carbon's is thiscall).
 * It ends by writing PCRTC_INTR and spinning until PMC_INTR bit 24 drops.
 * Its callers (the DPC, D3D's busy-waits) have read the bits before calling
 * it, so this is the vblank being taken: clear them now, and the spin ends at
 * once instead of holding the guest lock until the runtime's ack thread
 * comes round. */
extern void sub_00356DA0_gen(void);

void sub_00356DA0(void)
{
    uint32_t blk = eax;
    /* Read only once at DISPATCH: read before, a retire by the other
     * handler (DPC thread vs a D3D busy-wait) in between was counted by
     * both, the executor's flip index ran one off for good, and every
     * FLIP_STALL then waited out its 250 ms (Carbon, console, 3.7 fps). */
    uint8_t old = d3d_isr_enter();
    uint32_t head = MEM32(blk + 0x1BC);

    xbox_Nv2aVblankTaken();
    sub_00356DA0_gen();
    nv2a_pb_flip_retired(MEM32(blk + 0x1BC) - head);
    if (getenv("B3_D3D_TRACE")) {
        static unsigned n;
        if (n++ % 120 == 0)
            fprintf(stderr, "[d3d] vblank #%u blk %08X retire %u vbl %u pend %08X/%08X\n", n, blk,
                    MEM32(blk + 0x1BC), MEM32(blk + 0x1C0), MEM32(blk + 0x174), MEM32(blk + 0x180));
    }
    d3d_isr_leave(old);
}

/* PGRAPH handler, thiscall, ecx = the device's hardware block (+0 =
 * 0xFD000000). Reads a software-method trap from PGRAPH_INTR / NSOURCE /
 * TRAPPED_ADDR / TRAPPED_DATA, acknowledges it by writing PGRAPH_INTR back,
 * and acts on it: NOP(5) sets the event BlockOnTime sleeps on, a flip trap
 * queues the flip (sub_0035A457). The acknowledge is write-1-to-clear on
 * hardware and a no-op on RAM. Left pending, every turn of a busy-wait took
 * the same trap again; a trap posted during a call was sometimes dropped
 * (the fence event never set). So the executor posts traps under a lock held
 * here around the call, and a call that found one reports it taken, which
 * clears it (nv2a_pb_exec.c). */
extern void sub_00357000_gen(void);
extern void nv2a_pb_trap_lock(void);
extern void nv2a_pb_trap_unlock(void);
extern void nv2a_pb_trap_taken(void);

void sub_00357000(void)
{
    volatile uint32_t *nv = (volatile uint32_t *)XBOX_PTR(0xFD000000u);
    uint32_t blk = ecx;
    uint8_t old = d3d_isr_enter();
    uint32_t head = MEM32(blk + 0x1BC);     /* at DISPATCH, as above */
    uint32_t nsource;

    nv2a_pb_trap_lock();
    nsource = nv[0x400108 / 4];
    sub_00357000_gen();
    nv2a_pb_flip_retired(MEM32(blk + 0x1BC) - head);   /* a flip queued on its vblank retires at once */
    if (nsource)
        nv2a_pb_trap_taken();
    nv2a_pb_trap_unlock();
    if (getenv("B3_D3D_TRACE")) {
        static unsigned n;
        if (n++ < 40 || n % 200 == 0)
            fprintf(stderr, "[d3d] pgraph #%u nsource %08X retire %u\n", n, nsource, MEM32(blk + 0x1BC));
    }
    d3d_isr_leave(old);
}

/* ── Float to integer, sub_00244DF0 (_ftol2) ──────────────────
 *
 * MSVC's CRT _ftol2: st(0) truncated to a 64-bit integer in edx:eax, popped.
 * Every (int)float cast in the title calls it; same body as Carbon's
 * 0x0032C47C (NFSU2's hottest main-thread function in a race, 4.4%).
 * It rounds with fistp
 * (the control word's mode), then corrects towards zero by the sign of the
 * float (x - r): x >= 0 and x - r < 0 -> r - 1; x < 0 (sign of (float)x)
 * and x - r > 0 -> r + 1. r == 0 or the integer indefinite is returned as
 * is. Exact, including ecx, the frame it publishes and the x87 top.
 * RECOMP_NATIVE=0 lifted, RECOMP_NATIVE_CHECK=1 both and compare. */
extern void sub_00244DF0_gen(void);

__attribute__((noinline, cold)) static int64_t ftol_fist_slow(double x, uint16_t cw)
{
    return recomp_fist(x, cw, 64);
}

static inline int64_t ftol_fist(double x, uint16_t cw)
{
    if (__builtin_expect(((cw >> 10) & 3u) == 0 && fabs(x) < 0x1p63, 1))
        return (int64_t)rint(x);        /* host rounding is never changed: nearest */
    return ftol_fist_slow(x, cw);       /* other modes, NaN, out of range */
}

static inline void ftol2_native(double x, uint32_t *peax, uint32_t *pedx, uint32_t *pecx)
{
    int64_t r = ftol_fist(x, g_fp_control_word);
    uint32_t lo = (uint32_t)r, hi = (uint32_t)((uint64_t)r >> 32), d;
    float fx = (float)x, fd;
    uint32_t sx;

    if (lo == 0 && (hi & 0x7FFFFFFFu) == 0) {   /* 0 or the indefinite */
        *peax = lo;
        *pedx = hi;
        return;
    }
    fd = (float)(x - (double)r);
    memcpy(&d, &fd, 4);
    memcpy(&sx, &fx, 4);
    if (sx & 0x80000000u) {
        d ^= 0x80000000u;
        r += d >= 0x80000001u;          /* carry of d + 0x7FFFFFFF */
    } else {
        r -= d >= 0x80000001u;          /* borrow */
    }
    *peax = (uint32_t)r;
    *pedx = (uint32_t)((uint64_t)r >> 32);
    *pecx = d + 0x7FFFFFFFu;
}

/* Modes other than plain native, out of line so that the hot path below is
 * small enough for LTO to inline into its lifted callers. */
static int s_ftol_mode = -1;            /* 0 lifted, 1 native, 2 native + check */

__attribute__((noinline, cold)) static void ftol2_slow(void)
{
    double x = g_fp_stack[g_fp_top & 7];
    uint32_t a, d, c = ecx, frame = esp - 4u;

    if (s_ftol_mode < 0) {
        const char *e = getenv("RECOMP_NATIVE"), *k = getenv("RECOMP_NATIVE_CHECK");
        s_ftol_mode = (e && *e == '0') ? 0 : (k && *k == '1') ? 2 : 1;
    }
    if (s_ftol_mode == 0) {
        sub_00244DF0_gen();
        return;
    }
    ftol2_native(x, &a, &d, &c);
    if (s_ftol_mode == 2) {
        static unsigned long calls, bad;
        int top = (g_fp_top + 1) & 7;
        uint32_t sp = esp + 4u;
        sub_00244DF0_gen();
        calls++;
        if ((eax != a || edx != d || ecx != c || g_fp_top != top || esp != sp
             || g_ebp != frame) && bad++ < 20)
            fprintf(stderr, "[native] sub_00244DF0 mismatch for %.17g: lifted %08X:%08X "
                    "ecx %08X, native %08X:%08X ecx %08X\n", x, edx, eax, ecx, d, a, c);
        if ((calls & 0xFFFFFF) == 0)
            fprintf(stderr, "[native] sub_00244DF0: %lu calls, %lu mismatches\n", calls, bad);
        return;
    }
    eax = a;
    edx = d;
    ecx = c;
    g_fp_top = (g_fp_top + 1) & 7;
    g_ebp = g_seh_ebp = frame;
    esp += 4;
}

inline void sub_00244DF0(void)
{
    uint32_t a, d, c, frame;
    int top;

    if (__builtin_expect(s_ftol_mode != 1, 0)) {
        ftol2_slow();                   /* first call, lifted or check mode */
        return;
    }
    top = g_fp_top;
    c = ecx;
    frame = esp - 4u;
    ftol2_native(g_fp_stack[top & 7], &a, &d, &c);
    eax = a;
    edx = d;
    ecx = c;
    g_fp_top = (top + 1) & 7;
    g_ebp = g_seh_ebp = frame;          /* as its push ebp / mov ebp, esp left them */
    esp += 4;                           /* return address */
}


/* ── XMV colour conversion, sub_002CD892 ────────────────────
 *
 * The XMV player's YUV 4:2:0 -> 32-bit pixel converter for one 16x16 block,
 * hand-written MMX/SSE: stdcall, 7 arguments (Y, U, V, Y pitch, UV pitch,
 * destination, destination pitch), ret 28. The coefficients come in
 * registers the caller set up once per frame: mm4 (zero), mm5 (pmaddwd
 * weights), mm6 (chroma bias), mm7 (luma bias and the alpha byte, << 20),
 * xmm4-xmm7 (chroma weights). Eight pixels by two rows a step, left half
 * then right half, eight steps down.
 *
 * Half of the game thread on Linux during the intro and menu movies, and
 * the Switch ran those at 4 fps. Lifted, every op goes through the
 * thread-local guest registers, reloaded after each store since a guest
 * store may alias them. This is the same sequence of the same lane helpers
 * on locals, so it gives the same bytes; contraction is off so that a*b+c
 * is not fused into an FMA (which rounds once) on the Switch.
 * RECOMP_NATIVE=0 lifted, RECOMP_NATIVE_CHECK=1 both and compare. */
extern void sub_002CD892_gen(void);

typedef struct {
    RecompMmx m4, m5, m7;
    RecompXmm x4, x5, x6, x7;
} XmvCoef;

/* cvtps2pi, psrad 15, packssdw, packuswb: clamp(round(f) >> 15, 0, 255).
 * Out of range cvtps2pi gives 0x80000000, which clamps to 0 (an ARM
 * conversion would saturate instead, hence the explicit check). */
static inline uint32_t xmv_u8(float f)
{
    int32_t i;
#if defined(__i386__) || defined(__x86_64__)
    i = _mm_cvtss_si32(_mm_set_ss(f));
#else
    if (!(f >= -2147483648.0f && f < 2147483648.0f))
        return 0;
    i = (int32_t)__builtin_rintf(f);    /* host rounding is never changed: nearest */
#endif
    i >>= 15;
    return i < 0 ? 0u : i > 255 ? 255u : (uint32_t)i;
}

static inline RecompMmx xmv_pack(float lo, float hi, RecompMmx m4)
{
    RecompMmx r;

    if (__builtin_expect(m4.q == 0, 1)) {   /* mm4 is the zero register here */
        r.ud[0] = xmv_u8(lo);
        r.ud[1] = xmv_u8(hi);
        return r;
    }
    r = MMX_FROM_PS(lo, hi, 0);
    r = MMX_PSRAD(r, 15u);
    r = MMX_PACKSSDW(r, m4);
    r = MMX_PACKUSWB(r, m4);
    r = MMX_PUNPCKLBW(r, m4);
    return MMX_PUNPCKLWD(r, m4);
}

/* loc_002CD998: four luma bytes in y -> four pixels in *o0, *o1. */
static inline void xmv_quad(const XmvCoef *k, RecompMmx y, RecompXmm x1, RecompXmm x2,
                            RecompXmm *x0, RecompXmm *x3, RecompMmx *o0, RecompMmx *o1)
{
    RecompMmx a, b, c;

    a = MMX_PUNPCKLBW(y, k->m4);
    a = MMX_PADDW(a, k->m7);
    b = MMX_PSHUFW(a, 250u);
    a = MMX_PSHUFW(a, 80u);
    a = MMX_PMADDWD(a, k->m5);
    b = MMX_PMADDWD(b, k->m5);
    *x0 = XMM_FROM_PI(*x0, a);
    *x3 = XMM_FROM_PI(*x3, b);
    *x0 = XMM_SHUFFLE(*x0, *x3, 0x44);
    *x3 = XMM_ADD(XMM_MUL(x1, k->x4), *x0);
    a = MMX_PSLLD(k->m7, 20u);
    a = MMX_POR(a, xmv_pack(x3->f[0], x3->f[1], k->m4));
    b = MMX_PSLLD(k->m7, 20u);
    *x3 = XMM_SHUFFLE(*x3, *x3, 0x2E);
    b = MMX_POR(b, xmv_pack(x3->f[0], x3->f[1], k->m4));
    *x3 = XMM_ADD(XMM_MUL(x2, k->x7), *x0);
    a = MMX_POR(a, MMX_PSLLQ(xmv_pack(x3->f[0], x3->f[1], k->m4), 16u));
    *x3 = XMM_SHUFFLE(*x3, *x3, 0x2E);
    b = MMX_POR(b, MMX_PSLLQ(xmv_pack(x3->f[0], x3->f[1], k->m4), 16u));
    *x3 = XMM_MUL(x2, k->x5);
    *x0 = XMM_ADD(*x0, *x3);
    *x3 = XMM_MUL(x1, k->x6);
    *x0 = XMM_ADD(*x0, *x3);
    c = MMX_PSLLQ(xmv_pack(x0->f[0], x0->f[1], k->m4), 8u);
    a = MMX_POR(a, c);
    *x0 = XMM_SHUFFLE(*x0, *x0, 0x2E);
    c = MMX_PSLLQ(xmv_pack(x0->f[0], x0->f[1], k->m4), 8u);
    b = MMX_POR(b, c);
    *o0 = a;
    *o1 = b;
}

static inline uint32_t xmv_ld32(uint32_t va)
{
    uint32_t v;
    memcpy(&v, (const void *)XBOX_PTR(va), 4);
    return v;
}

/* two biased chroma dwords -> [c0, c0, c1, c1] */
static inline RecompXmm xmv_chroma(RecompXmm x, RecompMmx c)
{
    x = XMM_FROM_PI(x, c);
    return XMM_SHUFFLE(x, x, 0x50);
}

/* The same four pixels as xmv_quad, as 4-lane vectors (NEON on the Switch,
 * SSE2 on x86). Every lane is one pixel: luma L = (y + bias) * weight as
 * pmaddwd and cvtpi2ps give it, then B = U*x4 + L, R = V*x7 + L,
 * G = (L + V*x5) + U*x6, in that order, each rounded once. xmv_quad only
 * shuffles these lanes into the two MMX halves. A third of the game thread
 * in the menu movie was the scalar version (#6). */
typedef float    XmvV4f __attribute__((vector_size(16)));
typedef int32_t  XmvV4i __attribute__((vector_size(16)));
typedef uint32_t XmvV4u __attribute__((vector_size(16)));

typedef struct {
    XmvV4u m5a, m5b;        /* pmaddwd weights for pixel 0..3: [w0 w2 w0 w2], [w1 w3 w1 w3] */
    XmvV4i alpha;           /* mm7 dwords << 20 */
    XmvV4f x4, x5, x6, x7;
    uint16_t m7w[4];        /* luma bias per pixel */
} XmvVec;

static inline XmvV4f xmv_v4f(RecompXmm x)
{
    XmvV4f v;
    memcpy(&v, x.f, 16);
    return v;
}

static void xmv_vec_init(XmvVec *v, const XmvCoef *k)
{
    int i;

    for (i = 0; i < 4; i++) {
        v->m5a[i] = (uint32_t)(int32_t)k->m5.w[(i & 1) * 2];
        v->m5b[i] = (uint32_t)(int32_t)k->m5.w[(i & 1) * 2 + 1];
        v->alpha[i] = (int32_t)(k->m7.ud[i & 1] << 20);
        v->m7w[i] = k->m7.uw[i];
    }
    v->x4 = xmv_v4f(k->x4);
    v->x5 = xmv_v4f(k->x5);
    v->x6 = xmv_v4f(k->x6);
    v->x7 = xmv_v4f(k->x7);
}

/* xmv_u8 on four lanes */
static inline XmvV4i xmv_u8x4(XmvV4f f)
{
    const XmvV4i zero = { 0, 0, 0, 0 }, max = { 255, 255, 255, 255 };
    XmvV4i i;
#if defined(__aarch64__)
    const XmvV4f lo = { -2147483648.0f, -2147483648.0f, -2147483648.0f, -2147483648.0f };
    const XmvV4f hi = { 2147483648.0f, 2147483648.0f, 2147483648.0f, 2147483648.0f };

    /* fcvtns saturates; cvtps2pi's 0x80000000 (NaN, out of range) ends as 0 */
    i = (XmvV4i)vcvtnq_s32_f32((float32x4_t)f) & ((f >= lo) & (f < hi));
#elif defined(__SSE2__)
    i = (XmvV4i)_mm_cvtps_epi32((__m128)f);
#else
    int n;
    for (n = 0; n < 4; n++)
        i[n] = (int32_t)xmv_u8(f[n]) << 15;
#endif
    i >>= 15;
    i &= ~(i < zero);
    return (i & ~(i > max)) | (max & (i > max));
}

__attribute__((optimize("fp-contract=off")))
static inline void xmv_quad4(const XmvVec *v, uint32_t y, XmvV4f u, XmvV4f w, uint32_t dst)
{
    XmvV4u a;
    XmvV4f l, b, r, g;
    XmvV4i p;
    int i;

    for (i = 0; i < 4; i++)                     /* punpcklbw, paddw */
        a[i] = (uint32_t)(int32_t)(int16_t)(uint16_t)(((y >> (8 * i)) & 0xFFu) + v->m7w[i]);
    l = __builtin_convertvector((XmvV4i)(a * v->m5a + a * v->m5b), XmvV4f);
    b = u * v->x4 + l;
    r = w * v->x7 + l;
    g = (l + w * v->x5) + u * v->x6;
    p = v->alpha | xmv_u8x4(b) | (xmv_u8x4(g) << 8) | (xmv_u8x4(r) << 16);
    memcpy((void *)XBOX_PTR(dst), &p, 16);
}

__attribute__((optimize("fp-contract=off")))
static void xmv_yuv_native(void)
{
    uint32_t a = esp + 4;                       /* past the return address */
    uint32_t ys = MEM32(a), us = MEM32(a + 4), vs = MEM32(a + 8);
    uint32_t ypitch = MEM32(a + 12), uvpitch = MEM32(a + 16);
    uint32_t dst = MEM32(a + 20), dpitch = MEM32(a + 24);
    uint32_t n;
    XmvCoef k = { mm4, mm5, mm7, xmm4, xmm5, xmm6, xmm7 };
    RecompMmx m6 = mm6, r0 = mm0, r1 = mm1, r2 = mm2, r3 = mm3, u, v;
    RecompXmm x0 = xmm0, x1 = xmm1, x2 = xmm2, x3 = xmm3;
    int vec = k.m4.q == 0;                      /* mm4 is the zero register here */
    XmvVec kv;

    if (vec)
        xmv_vec_init(&kv, &k);
    for (n = 16;;) {
        u = MMX_PUNPCKLBW(MMX_FROM32(xmv_ld32(us)), k.m4);
        r3 = u;
        x1 = xmv_chroma(x1, MMX_PADDD(MMX_PUNPCKLWD(u, k.m4), m6));
        v = MMX_PUNPCKLBW(MMX_FROM32(xmv_ld32(vs)), k.m4);
        r1 = v;
        x2 = xmv_chroma(x2, MMX_PADDD(MMX_PUNPCKLWD(v, k.m4), m6));
        r3 = MMX_PUNPCKHWD(r3, r1);

        if (vec) {
            xmv_quad4(&kv, xmv_ld32(ys), xmv_v4f(x1), xmv_v4f(x2), dst);
            xmv_quad4(&kv, xmv_ld32(ys + ypitch), xmv_v4f(x1), xmv_v4f(x2), dst + dpitch);
        } else {
            xmv_quad(&k, MMX_FROM32(xmv_ld32(ys)), x1, x2, &x0, &x3, &r0, &r1);
            MMX_STORE(dst, r0);
            MMX_STORE(dst + 8, r1);
            xmv_quad(&k, MMX_FROM32(xmv_ld32(ys + ypitch)), x1, x2, &x0, &x3, &r0, &r1);
            MMX_STORE(dst + dpitch, r0);
            MMX_STORE(dst + dpitch + 8, r1);
        }

        r0 = MMX_PADDD(MMX_PSRLD(MMX_PSLLD(r3, 16u), 16u), m6);
        x1 = xmv_chroma(x1, r0);
        r3 = MMX_PADDD(MMX_PSRLD(r3, 16u), m6);
        x2 = xmv_chroma(x2, r3);

        if (vec) {
            xmv_quad4(&kv, xmv_ld32(ys + 4), xmv_v4f(x1), xmv_v4f(x2), dst + 0x10);
            xmv_quad4(&kv, xmv_ld32(ys + ypitch + 4), xmv_v4f(x1), xmv_v4f(x2),
                      dst + dpitch + 0x10);
        } else {
            xmv_quad(&k, MMX_FROM32(xmv_ld32(ys + 4)), x1, x2, &x0, &x3, &r0, &r1);
            MMX_STORE(dst + 0x10, r0);
            MMX_STORE(dst + 0x18, r1);
            xmv_quad(&k, MMX_FROM32(xmv_ld32(ys + ypitch + 4)), x1, x2, &x0, &x3, &r0, &r1);
            MMX_STORE(dst + dpitch + 0x10, r0);
            MMX_STORE(dst + dpitch + 0x18, r1);
        }

        if (--n == 0)
            break;
        if (n & 1) {                            /* right half of the same rows */
            dst += 0x20; ys += 8; us += 4; vs += 4;
        } else {                                /* next two rows, back to the left */
            dst += dpitch * 2 - 32;
            ys += ypitch * 2 - 8;
            us += uvpitch - 4;
            vs += uvpitch - 4;
        }
    }
    if (vec)    /* the registers the scalar steps leave: its last quad again */
        xmv_quad(&k, MMX_FROM32(xmv_ld32(ys + ypitch + 4)), x1, x2, &x0, &x3, &r0, &r1);
    /* r2 is only the packing scratch: the last value it held */
    r2 = xmv_pack(x0.f[0], x0.f[1], k.m4);
    r2 = MMX_PSLLQ(r2, 8u);
    mm0 = r0; mm1 = r1; mm2 = r2; mm3 = r3;
    xmm0 = x0; xmm1 = x1; xmm2 = x2; xmm3 = x3;
    eax = 0;
    ecx = us;
    edx = vs;
    g_ebp = g_seh_ebp = esp - 4;                /* as its push ebp / mov ebp, esp left them */
    esp += 4 + 28;                              /* return address, ret 28 */
}

void sub_002CD892(void)
{
    static int mode = -1;                       /* 0 lifted, 1 native, 2 both + compare */

    if (__builtin_expect(mode < 0, 0)) {
        const char *e = getenv("RECOMP_NATIVE"), *k = getenv("RECOMP_NATIVE_CHECK");
        mode = (e && *e == '0') ? 0 : (k && *k == '1') ? 2 : 1;
    }
    if (mode == 1) {
        xmv_yuv_native();
        return;
    }
    if (mode == 0) {
        sub_002CD892_gen();
        return;
    }
    {
        static unsigned long calls, bad;
        static uint8_t want[16][64];
        uint32_t sp = esp, dst = MEM32(esp + 4 + 20), dpitch = MEM32(esp + 4 + 24);
        uint32_t r_eax, r_ecx, r_edx, r_esp, row;
        uint64_t r_mm[4];
        RecompXmm r_x[4];

        sub_002CD892_gen();
        for (row = 0; row < 16; row++)
            memcpy(want[row], (const void *)XBOX_PTR(dst + row * dpitch), 64);
        r_eax = eax; r_ecx = ecx; r_edx = edx; r_esp = esp;
        r_mm[0] = mm0.q; r_mm[1] = mm1.q; r_mm[2] = mm2.q; r_mm[3] = mm3.q;
        r_x[0] = xmm0; r_x[1] = xmm1; r_x[2] = xmm2; r_x[3] = xmm3;
        /* the lifted run clobbered the scratch registers the native one reads
         * as its starting values; only lanes neither reads before writing */
        esp = sp;
        ecx = MEM32(sp + 4 + 4);
        edx = MEM32(sp + 4 + 8);
        xmv_yuv_native();
        calls++;
        for (row = 0; row < 16; row++)
            if (memcmp(want[row], (const void *)XBOX_PTR(dst + row * dpitch), 64))
                break;
        if ((row < 16 || eax != r_eax || ecx != r_ecx || edx != r_edx || esp != r_esp
             || mm0.q != r_mm[0] || mm1.q != r_mm[1] || mm2.q != r_mm[2] || mm3.q != r_mm[3]
             || memcmp(&xmm0, &r_x[0], 16) || memcmp(&xmm1, &r_x[1], 16)
             || memcmp(&xmm2, &r_x[2], 16) || memcmp(&xmm3, &r_x[3], 16)) && bad++ < 20)
            fprintf(stderr, "[native] sub_002CD892 mismatch at dst %08X: row %u, regs "
                    "eax %d ecx %d edx %d esp %d mm %d%d%d%d xmm %d%d%d%d\n", dst, row,
                    eax != r_eax, ecx != r_ecx, edx != r_edx, esp != r_esp,
                    mm0.q != r_mm[0], mm1.q != r_mm[1], mm2.q != r_mm[2], mm3.q != r_mm[3],
                    !!memcmp(&xmm0, &r_x[0], 16), !!memcmp(&xmm1, &r_x[1], 16),
                    !!memcmp(&xmm2, &r_x[2], 16), !!memcmp(&xmm3, &r_x[3], 16));
        if ((calls & 0xFFFF) == 0)
            fprintf(stderr, "[native] sub_002CD892: %lu calls, %lu mismatches\n", calls, bad);
    }
}


/* ── Simulation steps per frame (0x001B58E0) ─────────────────
 *
 * thiscall-like: esi = timing state, two stack args (min, max steps; the
 * main loop passes 1, 4), ret 8, eax = how many 1/60 s simulation steps
 * the main loop (0x00016ADC) runs this frame. The state's first dword is a
 * mode the game sets (0x4D5390 = state of the game object at 0x4A71A0):
 * 1 counts the TSC ticks that really elapsed and clamps them to [min, max];
 * 0, which races use, always answers min -- one step per rendered frame,
 * because the Xbox holds 60 fps there. Below 60 fps that is slow motion:
 * at 48 fps the race clock ran at 0.80 of real time, and on the Switch the
 * speedometer read 150 mph while the world crawled past. Mode 0 is run as
 * mode 1, so the simulation keeps real time at any frame rate (the time
 * accounting is the same in both; only the answer differs).
 * B3_FRAME_LOCK=1: the original one step per frame. */
extern void sub_001B58E0_gen(void);

void sub_001B58E0(void)
{
    static int lock = -1;
    uint32_t st = esi;

    if (lock < 0) {
        const char *e = getenv("B3_FRAME_LOCK");
        lock = e && *e == '1';
    }
    if (!lock && MEM32(st) == 0) {
        MEM32(st) = 1;
        sub_001B58E0_gen();
        MEM32(st) = 0;
    } else {
        sub_001B58E0_gen();
    }
}

/* ── Driver Details > Settings > Gameplay: picture options ────
 *
 * Three rows under Takedown Camera / Crash Mode Impact Time, as NFSU2's
 * Options -> Video: Resolution Scale 1x..2.5x (nv2a_vk_scale_pct, applied
 * at the next flip), Anti-Aliasing Off/On (FXAA at present) and
 * Anisotropic Filtering Off/2x..16x. Vulkan build only. Kept in
 * b3x_options.txt (sdmc:/switch/b3x/ on the Switch, the working directory
 * elsewhere): scale=, fxaa=, aniso=; loaded at boot (main.c).
 *
 * How the screen works (all hand-built, no data file):
 *   sub_00084E20  builds it: screen object 0x55AC44 (+4 cursor u16, +6 row
 *                 count u16, rows of 0x14 bytes from +8: label pointer,
 *                 value-list pointer (u32 string ids), value count, 4),
 *                 then sub_000EF170 makes the list widget 0x550ACC, which
 *                 keeps pointers to the header (eax) and the rows (arg),
 *                 and sub_000EF600(widget, row, value) sets each value.
 *   sub_00084F90  input, ecx = the screen: pad block 0x4AE724 + 0x188 *
 *                 [0x4AED45], +0x28/+0x34 up/down, +0x10/+0x1C left/right;
 *                 rows 0/1 flip their byte (0x4AE1DB/DC) and redraw with
 *                 sub_000F0CB0(edx = widget, ecx = row, value); any other
 *                 row just plays the click.
 * The screen object has room for two rows (the next screen starts at
 * 0x55AC80), so the widget gets a five-row copy in guest memory instead.
 * Value texts are string ids: the string table (pointer list at
 * [[0x4D532C] + 0xC], 0xF91 entries) is replaced by a longer copy with the
 * new texts appended; "Off"/"On" are the game's own 0x18B/0x18A. */
#ifdef __SWITCH__
#  define B3X_OPTIONS_FILE "sdmc:/switch/b3x/b3x_options.txt"
#else
#  define B3X_OPTIONS_FILE "b3x_options.txt"
#endif
#define GP_SCREEN     0x0055AC44u
#define GP_WIDGET     0x00550ACCu
#define GP_ROW_SIZE   0x14u
#define STR_TABLE_PTR 0x004D532Cu
#define STR_OFF       0x18Bu
#define STR_ON        0x18Au

#ifdef NFSU2_VULKAN
extern volatile int nv2a_vk_scale_pct;
extern volatile int nv2a_vk_fxaa;
extern volatile int nv2a_vk_aniso;
#endif
extern uint32_t xbox_ContiguousAlloc(uint32_t size, uint32_t alignment);

static const int s_scale_pct[] = { 100, 150, 200, 250 };
static const int s_aniso_lvl[] = { 1, 2, 4, 8, 16 };
static int s_scale_idx = 1;             /* 1.5x */
static int s_scale_saved;               /* b3x_options.txt had scale= */
static int s_fxaa = 1;
static int s_aniso_idx = 4;             /* 16x */

static void opt_apply(void)
{
#ifdef NFSU2_VULKAN
    if (s_scale_saved)
        nv2a_vk_scale_pct = s_scale_pct[s_scale_idx];
    nv2a_vk_fxaa = s_fxaa;
    nv2a_vk_aniso = s_aniso_lvl[s_aniso_idx];
#endif
}

static void opt_log(void)
{
    fprintf(stderr, "[options] resolution scale %d%%%s, anti-aliasing %s, anisotropic %dx\n",
            s_scale_pct[s_scale_idx], s_scale_saved ? "" : " (RECOMP_GL_SCALE)",
            s_fxaa ? "on" : "off", s_aniso_lvl[s_aniso_idx]);
}

void b3x_options_load(void)
{
    char line[128];
    FILE *f = fopen(B3X_OPTIONS_FILE, "r");
    int i;

    if (f) {
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "fxaa=", 5))
                s_fxaa = line[5] != '0';
            else if (!strncmp(line, "aniso=", 6)) {
                int a = atoi(line + 6);
                for (i = 0; i < 5; i++)
                    if (s_aniso_lvl[i] == a)
                        s_aniso_idx = i;
            } else if (!strncmp(line, "scale=", 6)) {
                int pct = atoi(line + 6);
                for (i = 0; i < 4; i++)
                    if (s_scale_pct[i] == pct) {
                        s_scale_idx = i;
                        s_scale_saved = 1;
                    }
            }
        }
        fclose(f);
    }
    if (!s_scale_saved) {
        /* RECOMP_GL_SCALE rules until the row is changed; the row shows the
         * nearest step. */
        const char *e = getenv("RECOMP_GL_SCALE");
        double k = e && *e ? strtod(e, NULL) : 1.0, best = 1e9;
        for (i = 0; i < 4; i++) {
            double d = fabs(k * 100.0 - s_scale_pct[i]);
            if (d < best) { best = d; s_scale_idx = i; }
        }
    }
    opt_apply();
    opt_log();
}

static void opt_save(void)
{
    FILE *f = fopen(B3X_OPTIONS_FILE, "w");
    if (!f) {
        fprintf(stderr, "[options] cannot write " B3X_OPTIONS_FILE "\n");
        return;
    }
    if (s_scale_saved)
        fprintf(f, "scale=%d\n", s_scale_pct[s_scale_idx]);
    fprintf(f, "fxaa=%d\naniso=%d\n", s_fxaa, s_aniso_lvl[s_aniso_idx]);
    fclose(f);
}

/* The rows exist in the Vulkan build only (the GL renderer has no such
 * switches); the wrappers below are always built, as the lifter takes
 * every function defined in this file as overridden. */
#ifdef NFSU2_VULKAN
#  define B3X_GP_ROWS 1
#else
#  define B3X_GP_ROWS 0
#endif

/* New texts: labels (used as pointers) and values (used as string ids). */
enum { T_SCALE, T_AA, T_ANISO, T_1X, T_15X, T_2X, T_25X, T_4X, T_8X, T_16X, T_COUNT };
static const char *const s_texts[T_COUNT] = {
    "RESOLUTION SCALE", "ANTI-ALIASING", "ANISOTROPIC FILTERING",
    "1x", "1.5x", "2x", "2.5x", "4x", "8x", "16x",
};
static uint32_t s_text_va[T_COUNT];     /* UTF-16 copies in guest memory */
static uint32_t s_text_id[T_COUNT];     /* their ids in the extended table */
static uint32_t s_ext_list;             /* our pointer list, 0 = not built */
static uint32_t s_ext_count;
static uint32_t s_rows_va;              /* five rows */
static uint32_t s_vals_va;              /* value-id lists */

typedef struct { int text; uint32_t list_off; int n; int *value; } GpRow;
static GpRow s_gp[3] = {
    { T_SCALE, 0x00, 4, &s_scale_idx },
    { T_AA,    0x10, 2, &s_fxaa },
    { T_ANISO, 0x18, 5, &s_aniso_idx },
};

/* The string table with our texts appended; rebuilt if the game reloads
 * its own (its pointer list then is no longer ours). */
static int gp_strings(void)
{
    uint32_t tab = MEM32(STR_TABLE_PTR), list, n, i;

    if (!tab)
        return 0;
    list = MEM32(tab + 0xC);
    if (s_ext_list && list == s_ext_list)
        return 1;
    n = MEM32(tab + 8);
    if (!n || n > 0x10000)
        return 0;
    if (!s_text_va[0])
        for (i = 0; i < T_COUNT; i++) {
            size_t len = strlen(s_texts[i]), k;
            uint32_t va = xbox_ContiguousAlloc((uint32_t)(len + 1) * 2, 4);
            if (!va)
                return 0;
            for (k = 0; k <= len; k++)
                MEM16(va + (uint32_t)k * 2) = (uint8_t)s_texts[i][k];
            s_text_va[i] = va;
        }
    s_ext_list = xbox_ContiguousAlloc((n + T_COUNT) * 4, 16);
    if (!s_ext_list)
        return 0;
    for (i = 0; i < n; i++)
        MEM32(s_ext_list + i * 4) = MEM32(list + i * 4);
    for (i = 0; i < T_COUNT; i++) {
        s_text_id[i] = n + i;
        MEM32(s_ext_list + (n + i) * 4) = s_text_va[i];
    }
    s_ext_count = n + T_COUNT;
    MEM32(tab + 0xC) = s_ext_list;
    MEM32(tab + 8) = s_ext_count;

    /* Value lists: scale 1x..2.5x at +0, Off/On at +0x10, aniso at +0x18. */
    if (!s_vals_va)
        s_vals_va = xbox_ContiguousAlloc(0x30, 16);
    if (!s_vals_va)
        return 0;
    MEM32(s_vals_va + 0x00) = s_text_id[T_1X];
    MEM32(s_vals_va + 0x04) = s_text_id[T_15X];
    MEM32(s_vals_va + 0x08) = s_text_id[T_2X];
    MEM32(s_vals_va + 0x0C) = s_text_id[T_25X];
    MEM32(s_vals_va + 0x10) = STR_OFF;
    MEM32(s_vals_va + 0x14) = STR_ON;
    MEM32(s_vals_va + 0x18) = STR_OFF;
    MEM32(s_vals_va + 0x1C) = s_text_id[T_2X];
    MEM32(s_vals_va + 0x20) = s_text_id[T_4X];
    MEM32(s_vals_va + 0x24) = s_text_id[T_8X];
    MEM32(s_vals_va + 0x28) = s_text_id[T_16X];
    return 1;
}

extern void sub_000EF170_gen(void);
extern void sub_00084E20_gen(void);
extern void sub_00084F90_gen(void);
extern void sub_000EF600(void);
extern void sub_000F0CB0(void);

/* List widget: for the Gameplay screen, five rows instead of its two. */
void sub_000EF170(void)
{
    uint32_t rows = MEM32(esp + 4), i;

    if (B3X_GP_ROWS && esi == GP_WIDGET && rows == GP_SCREEN + 8 && eax == GP_SCREEN + 4
            && gp_strings()) {
        if (!s_rows_va)
            s_rows_va = xbox_ContiguousAlloc(5 * GP_ROW_SIZE, 16);
        if (s_rows_va) {
            for (i = 0; i < 2 * GP_ROW_SIZE; i += 4)
                MEM32(s_rows_va + i) = MEM32(rows + i);
            for (i = 0; i < 3; i++) {
                uint32_t r = s_rows_va + (2 + i) * GP_ROW_SIZE;
                MEM32(r + 0x0) = s_text_va[s_gp[i].text];
                MEM32(r + 0x4) = 0;
                MEM32(r + 0x8) = s_vals_va + s_gp[i].list_off;
                MEM8(r + 0xC) = (uint8_t)s_gp[i].n;
                MEM8(r + 0xD) = 4;
                MEM8(r + 0xE) = 0;
                MEM8(r + 0xF) = 0;
                MEM32(r + 0x10) = 0;
            }
            MEM16(GP_SCREEN + 6) = 5;
            MEM32(esp + 4) = s_rows_va;
        }
    }
    sub_000EF170_gen();
}

/* Set row `row` of the Gameplay list to `value`, as the builder does. */
static void gp_set(uint32_t row, uint32_t value)
{
    uint32_t r = s_rows_va + row * GP_ROW_SIZE;
    xmm5.f[0] = MEMF((uint32_t)MEM8(r + 0xD) * 4 + 0x00572104u);
    PUSH32(esp, value);
    PUSH32(esp, row);
    PUSH32(esp, GP_WIDGET);
    PUSH32(esp, 0x00084F37u); RECOMP_ABI_CALL(0x000EF600u, sub_000EF600);
}

void sub_00084E20(void)
{
    int i;

    sub_00084E20_gen();
    if (s_rows_va && MEM16(GP_SCREEN + 6) == 5 && MEM32(GP_WIDGET + 0x10) == s_rows_va)
        for (i = 0; i < 3; i++)
            gp_set(2 + i, (uint32_t)*s_gp[i].value);
}

/* Left/right on our rows: step the value, redraw, apply, save. The original
 * then plays its click for any row past the second. */
void sub_00084F90(void)
{
    uint32_t screen = ecx;

    if (screen == GP_SCREEN && MEM32(0x00557A64u) == 5 && s_rows_va
            && MEM16(screen + 6) == 5) {
        uint32_t pad = 0x004AE724u + 0x188u * (uint32_t)(int32_t)(int8_t)MEM8(0x004AED45u);
        uint32_t row = MEM16(screen + 4);
        int left = MEM16(pad + 0x10) != 0, right = MEM16(pad + 0x1C) != 0;
        if (row >= 2 && row < 5 && (left || right)
                && !MEM16(pad + 0x28) && !MEM16(pad + 0x34)) {
            GpRow *g = &s_gp[row - 2];
            int v = *g->value + (right ? 1 : -1);
            if (v < 0) v = g->n - 1;
            if (v >= g->n) v = 0;
            *g->value = v;
            if (g->value == &s_scale_idx)
                s_scale_saved = 1;
            opt_apply();
            opt_save();
            opt_log();
            ecx = row;
            edx = GP_WIDGET;
            PUSH32(esp, (uint32_t)v);
            PUSH32(esp, 0x0008513Du); RECOMP_ABI_CALL(0x000F0CB0u, sub_000F0CB0);
            ecx = screen;
        }
    }
    sub_00084F90_gen();
}

/* ── DirectSound DSP command post (0x002F7EC2) ───────────────
 *
 * thiscall, ecx = the DSP-side object. Copies a command block into the GP
 * DSP's scratch area, stores the command code at scratch + 0x810, and spins
 * until the DSP zeroes it (this loop does re-read memory). The scratch block
 * is ***(this + 8 + 0x10), the same chain the runtime's RECOMP_DSP_ACK notes
 * describe. There is no DSP, so register the word with the runtime's
 * completion list before running the original body. */
extern int xbox_ApuDspAckWord(uint32_t va);
extern void sub_002F7EC2_gen(void);

void sub_002F7EC2(void)
{
    uint32_t obj = MEM32(ecx + 8);
    uint32_t blk = obj ? MEM32(obj + 0x10) : 0;
    uint32_t scratch = blk ? MEM32(blk) : 0;

    if (scratch)
        xbox_ApuDspAckWord(scratch + 0x810);
    sub_002F7EC2_gen();
}

/* ── DirectSound AC'97 channel reset (0x002FE736) ────────────
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
 * Everything else -- the lock taken through sub_002F3F70/sub_002F3F92, the
 * buffer-descriptor base store, the 0xFEC0017C write for channel 1 and the
 * sub_002FE540 re-arm -- is as the original does it. */
void sub_002FE736(void)
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
    PUSH32(esp, 0x002FE74Au); RECOMP_ABI_CALL(0x002F3F70u, sub_002F3F70);

    nabm = MEM32(MEM32(chan) * 4 + 0x00300448u);
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
    PUSH32(esp, 0x002FE79Du); RECOMP_ABI_CALL(0x002FE540u, sub_002FE540);

    ecx = frame - 8;
    g_ebp = frame;
    g_seh_ebp = frame;
    PUSH32(esp, 0x002FE7A5u); RECOMP_ABI_CALL(0x002F3F92u, sub_002F3F92);

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
