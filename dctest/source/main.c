/* Dual-core reconstruction SCALING test.
 *
 * Question: on the 3DS ARM11, does using a SECOND CPU core add real throughput for decode
 * reconstruction (motion-comp + inverse-transform), or does the shared memory bus cap it?
 *
 * Method: run the REAL decode reconstruction kernels (mobi_recon_bench, from mobiclip.c -- diagonal
 * half-pel motion comp + full 8x8 2D IDCT) over a frame-sized buffer:
 *   SINGLE: one core does the whole frame.
 *   DUAL:   core A does the top half, core B does the bottom half, in parallel.
 * speedup = single / dual. ~2x => the bus has headroom and dual-core reconstruction is worth building;
 * ~1.0-1.3x => memory-bus limited, dual-core won't help much.
 *
 * Old 3DS: worker runs on the syscore (core 1) with APT_SetAppCpuTimeLimit(80).
 * New 3DS: worker runs on core 2 (a free app core).
 * START = exit.
 */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void mobi_recon_bench(const uint8_t *ref, uint8_t *dst, int W, int H, int y0, int y1);

#define W     512
#define H     480          /* multiple of 8; ~frame-scale so memory pressure is realistic */
#define ITERS 40           /* passes per timing run (enough to be stable) */

static uint8_t *g_ref, *g_dst;
static volatile int g_run = 1, g_y0, g_y1;
static LightEvent g_start, g_done;

static void worker(void *arg) {
    (void)arg;
    while (g_run) {
        LightEvent_Wait(&g_start);
        if (!g_run) break;
        for (int i = 0; i < ITERS; i++) mobi_recon_bench(g_ref, g_dst, W, H, g_y0, g_y1);
        LightEvent_Signal(&g_done);
    }
}

int main(void) {
    osSetSpeedupEnable(true);          /* New 3DS: 804MHz. svcGetSystemTick still counts real time. */
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    bool isnew = false; APT_CheckNew3DS(&isnew);
    printf("Dual-core reconstruction scaling\n");
    printf("Console: %s 3DS\n\n", isnew ? "NEW" : "OLD");

    g_ref = (uint8_t *)malloc((size_t)W * (H + 9));   /* +9 rows: MC reads one block-row past */
    g_dst = (uint8_t *)malloc((size_t)W * H);
    if (!g_ref || !g_dst) { printf("alloc failed\n"); goto wait; }
    for (size_t i = 0; i < (size_t)W * (H + 9); i++) g_ref[i] = (uint8_t)(i * 7);
    memset(g_dst, 0, (size_t)W * H);

    int core = isnew ? 2 : 1;
    if (!isnew) APT_SetAppCpuTimeLimit(80);            /* let the worker run on the syscore */
    LightEvent_Init(&g_start, RESET_ONESHOT);
    LightEvent_Init(&g_done,  RESET_ONESHOT);
    s32 prio = 0x30; svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    Thread th = threadCreate(worker, NULL, 32 * 1024, prio - 1, core, false);
    if (!th) { printf("worker thread create FAILED (core %d)\n", core); goto wait; }

    printf("frame %dx%d, %d iters, best of 3...\n\n", W, H, ITERS);

    u64 best_single = ~0ull, best_dual = ~0ull;
    for (int r = 0; r < 3; r++) {
        u64 t = svcGetSystemTick();
        for (int i = 0; i < ITERS; i++) mobi_recon_bench(g_ref, g_dst, W, H, 0, H);
        u64 dt = svcGetSystemTick() - t; if (dt < best_single) best_single = dt;
    }
    for (int r = 0; r < 3; r++) {
        g_y0 = H / 2; g_y1 = H;                        /* worker: bottom half */
        u64 t = svcGetSystemTick();
        LightEvent_Signal(&g_start);                   /* kick worker */
        for (int i = 0; i < ITERS; i++) mobi_recon_bench(g_ref, g_dst, W, H, 0, H / 2);  /* main: top half */
        LightEvent_Wait(&g_done);                      /* both done */
        u64 dt = svcGetSystemTick() - t; if (dt < best_dual) best_dual = dt;
    }

    double ms_s = best_single * 1000.0 / SYSCLOCK_ARM11;
    double ms_d = best_dual   * 1000.0 / SYSCLOCK_ARM11;
    printf("single-core (full frame):  %6.2f ms\n", ms_s);
    printf("dual-core   (split halves): %6.2f ms\n", ms_d);
    printf("\n   SPEEDUP: %.2fx\n\n", ms_d > 0 ? ms_s / ms_d : 0);
    printf("~1.7-2.0x: bus has headroom ->\n  dual-core reconstruction WORTH building.\n");
    printf("~1.0-1.3x: memory-bus limited ->\n  won't help; tune single-core instead.\n");

    g_run = 0; LightEvent_Signal(&g_start); threadJoin(th, UINT64_MAX); threadFree(th);

wait:
    printf("\nSTART to exit.\n");
    while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & KEY_START) break; gspWaitForVBlank(); }
    gfxExit();
    return 0;
}
