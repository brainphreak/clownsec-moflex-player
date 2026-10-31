/* MP4/H.264 (New-3DS MVD hardware) + AAC-LC (Helix) player.
 * Decoding, the B-frame reorder queue and the audio-master clock are MP4's own; EVERYTHING the
 * viewer sees or touches is the moflex player's, through player_kit: the same panel, CC / AUDIO /
 * VIDEO menus, dark screen, L+R lock, subtitle renderer and GPU presentation (zoom, convergence,
 * ghost fix, picture settings). Each decoded frame is scaled into a texture and drawn exactly the
 * way the ring engine draws a moflex frame. 2D and side-by-side 3D. */
#include "mp4_play.h"
#include "mp4_demux.h"
#include "mp4_mvd.h"
#include "mp4_aac.h"
#include "moflex_playback.h"   /* MoflexResult + shared resume/volume stores */
#include "player_kit.h"
#include "ui_gfx.h"

#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#define NWB          16
#define AAC_MAXSAMP  2048
#define SEEK_STEP_US (30LL * 1000000)   /* the moflex player's step */

/* ---- audio state (ndsp channel 0) ---- */
static Mp4Aac      g_aac;
static ndspWaveBuf g_wb[NWB];
static int16_t    *g_abuf[NWB];
static int         g_slot_ns[NWB];
static int         g_have_audio;
static int         g_a_next;
static long long   g_a_played;
static int         g_a_started;
static int64_t     g_a_base_us;      /* media time at g_a_played==0 (moves on seek) */

/* ---- player state ---- */
static int         g_playing = 1;

/* wall clock, used as the master when a file has no audio (audio files use the DSP position) */
static int64_t     g_clk_base_us;
static u64         g_clk_t0;
static int         g_clk_running;
static void clk_set(int64_t us, int running) { g_clk_base_us = us; g_clk_t0 = osGetTime(); g_clk_running = running; }
static void clk_pause(void)  { if (g_clk_running) { g_clk_base_us += (int64_t)(osGetTime() - g_clk_t0) * 1000; g_clk_running = 0; } }
static void clk_resume(void) { if (!g_clk_running) { g_clk_t0 = osGetTime(); g_clk_running = 1; } }

/* ---- HOME/sleep: the kit releases gsp::Lcd (holding it across an APT suspend hard-locks the
 * GSP) and asks for a repaint on return; audio and the wall clock pause alongside. ---- */
static aptHookCookie g_mp4_apt_cookie;
static void mp4_apt_hook(APT_HookType t, void *u) {
    (void)u;
    switch (t) {
        case APTHOOK_ONSUSPEND: case APTHOOK_ONSLEEP:
            kit_suspend();
            if (g_have_audio) ndspChnSetPaused(0, true);
            else clk_pause();                /* silent file: the wall clock must not run under HOME
                                              * or the player fast-forwards to catch up on return */
            break;
        case APTHOOK_ONRESTORE: case APTHOOK_ONWAKEUP:
            kit_restore();                   /* the loop re-applies 3D and repaints */
            if (g_have_audio && g_playing) ndspChnSetPaused(0, false);
            else if (g_playing) clk_resume();
            break;
        default: break;
    }
}

/* time of a sample in microseconds */
static int64_t v_time_us(Mp4 *m, int i) {
    if (m->v_timescale <= 0) return 0;
    return (int64_t)(mp4_v_dts(m, i) * 1000000LL / (int64_t)m->v_timescale);
}
static int64_t v_cts_us(Mp4 *m, int i) {   /* DISPLAY time: with B-frames this != decode time */
    if (m->v_timescale <= 0) return 0;
    return (int64_t)(mp4_v_cts(m, i) * 1000000LL / (int64_t)m->v_timescale);
}
/* B-frame reorder queue: decoded frames wait here (by slot) until their DISPLAY time comes up.
 * Depth 4 covers x264's default reorder distance with margin; adds ~4 frames of start latency. */
#define PQ_DEPTH_MAX 4
static struct { int64_t cts; int slot; } g_pq[PQ_DEPTH_MAX];
static int g_pq_depth = PQ_DEPTH_MAX;   /* MVD slots - 1: 3 when a 1080p stream only fit 4 slots */
static int g_pq_n = 0;
/* Display-time pool: cts of fed-but-not-yet-surfaced samples. The hardware holds/reorders
 * pictures internally with B-frames (a fed frame may surface 1-2 feeds later, or never);
 * pairing each SURFACED picture with the SMALLEST pooled display time keeps the presented
 * timeline gapless no matter how the decoder delays -- labeling outputs with the fed
 * frame's own cts dropped 2 of 3 frames (measured: 125ms present gaps at 41.7ms content). */
/* Pending display times, one per frame FED to the decoder, popped smallest-first as pictures
 * come back out. The pairing is an invariant: one push per frame in, one pop per picture out.
 * MVD holds a whole reorder buffer of frames before it emits the first, so this must be deep
 * enough for the worst DPB in the wild -- 16 was not, and overflowing it silently DROPPED a
 * timestamp while the frame still went to the decoder. Every lost entry slides the whole
 * mapping by one frame, which is why MP4 drifted further out of sync the longer it played
 * and why starting over fixed it. */
#define FT_MAX 16
static int64_t g_ft[FT_MAX];
static int     g_ftn = 0;
static int     g_ft_full = 0;      /* display times DROPPED because the pool was full */
static int     g_ft_dry = 0;       /* pictures that surfaced with no display time left */
static long    g_ft_push = 0, g_ft_pop = 0;   /* frames fed vs pictures surfaced */
static int64_t ft_pop_min(void) {
    int mi = 0;
    for (int i = 1; i < g_ftn; i++) if (g_ft[i] < g_ft[mi]) mi = i;
    int64_t v = g_ft[mi]; g_ft[mi] = g_ft[--g_ftn];
    return v;
}
static int g_present_log = 12;   /* countdown: log the first presents after start/seek */

static int64_t a_time_us(Mp4 *m, int i) {
    if (m->a_timescale <= 0) return 0;
    return (int64_t)(mp4_a_dts(m, i) * 1000000LL / (int64_t)m->a_timescale);
}

/* One tap per stylus contact. The resistive panel drops out for a frame or two when pressure
 * eases mid-press, which reads as lift + press -- a second tap -- so a normal-length tap toggled
 * the moon (or play) twice and only a quick jab worked. A touch-down counts only after the
 * stylus has been up for TAP_GAP_MS; call this on EVERY scan (the loop and the frame wait). */
#define TAP_GAP_MS 150
static u64 g_touch_held_ms = 0;
static u32 tap_filter(u32 kd, u32 kh) {
    u64 now = osGetTime();
    if ((kd & KEY_TOUCH) && now - g_touch_held_ms < TAP_GAP_MS) kd &= ~KEY_TOUCH;   /* bounce */
    if (kh & KEY_TOUCH) g_touch_held_ms = now;
    return kd;
}

/* current media position (us). Audio is the master when present; otherwise a wall clock. */
static int64_t media_now_us(void) {
    if (g_have_audio) {
        if (!g_a_started) return g_a_base_us;
        return g_a_base_us + (int64_t)(g_a_played * 1000000LL / (long long)g_aac.rate);
    }
    return g_clk_base_us + (g_clk_running ? (int64_t)(osGetTime() - g_clk_t0) * 1000 : 0);
}

/* refill free/done wavebufs from the next AAC access units, applying volume */
static void pump_audio(Mp4 *m, uint8_t *atmp) {
    if (!g_have_audio) return;
    for (int i = 0; i < NWB; i++) {
        ndspWaveBuf *wb = &g_wb[i];
        if (wb->status != NDSP_WBUF_FREE && wb->status != NDSP_WBUF_DONE) continue;
        if (wb->status == NDSP_WBUF_DONE) { g_a_played += g_slot_ns[i]; g_slot_ns[i] = 0; }
        if (g_a_next >= m->a_count) continue;
        Mp4Sample *as = &m->asamples[g_a_next];
        int n  = mp4_read_sample_audio(m, as, atmp);
        int sp = (n == (int)mp4_s_size(as)) ? mp4_aac_decode(&g_aac, atmp, n, g_abuf[i]) : 0;
        g_a_next++;
        if (sp <= 0) continue;
        kit_audio_gain(g_abuf[i], sp * g_aac.channels);   /* volume + auto level, as moflex */
        DSP_FlushDataCache(g_abuf[i], sp * g_aac.channels * 2);
        memset(wb, 0, sizeof *wb);
        wb->data_vaddr = g_abuf[i]; wb->nsamples = sp; g_slot_ns[i] = sp;
        ndspChnWaveBufAdd(0, wb);
        g_a_started = 1;
    }
}

/* Seek to target_us: reset MVD + decode from the preceding keyframe to the target, reset the audio
 * ring to the matching AAC sample, and rebase the clock. Returns the landing frame's slot (for the
 * caller to present) or -1. */
static int do_seek(Mp4 *m, uint8_t *vbuf, uint8_t *atmp, int64_t dur_us, int64_t target_us, int *vi) {
    if (target_us < 0) target_us = 0;
    if (dur_us > 0 && target_us > dur_us) target_us = dur_us;
    int64_t tdts = (m->v_timescale > 0) ? target_us * (int64_t)m->v_timescale / 1000000LL : 0;
    int kf = mp4_keyframe_before(m, tdts); if (kf < 0) kf = 0;
    int tvi = kf; while (tvi + 1 < m->v_count && v_time_us(m, tvi + 1) <= target_us) tvi++;

    mp4_mvd_reset();
    g_pq_n = 0; g_ftn = 0;   /* seek invalidates queued frames + pooled display times */
    extern void mvd_log(const char *fmt, ...);
    u64 sk0 = osGetTime();
    int got = 0, prod = 0;
    for (int j = kf; j <= tvi; j++) {
        int c = 0;
        while (mp4_mvd_pending()) { if (!mp4_mvd_pump(NULL, 0, &c)) break; prod++; }   /* drain */
        int nn = mp4_read_sample(m, &m->vsamples[j], vbuf);
        int r = (nn == (int)mp4_s_size(&m->vsamples[j])) ? mp4_mvd_pump(vbuf, nn, &c) : 0;
        if (r) { got = r; prod++; }
    }
    mvd_log("SEEK to=%llds kf=%d tvi=%d decoded=%d produced=%d took=%lums",
            (long long)(target_us / 1000000), kf, tvi, tvi - kf + 1, prod,
            (unsigned long)(osGetTime() - sk0));
    g_present_log = 12;   /* trace the next dozen presents */
    *vi = tvi + 1;

    if (g_have_audio) {
        ndspChnWaveBufClear(0);
        for (int i = 0; i < NWB; i++) { g_wb[i].status = NDSP_WBUF_DONE; g_slot_ns[i] = 0; }
        g_a_played = 0; g_a_started = 0;
        int ai = 0; while (ai + 1 < m->a_count && a_time_us(m, ai + 1) <= target_us) ai++;
        g_a_next = ai; g_a_base_us = (m->a_count > 0) ? a_time_us(m, ai) : target_us;
        pump_audio(m, atmp);
    } else {
        g_a_base_us = target_us;
    }
    clk_set(target_us, g_playing);
    return got - 1;
}

/* ---- frame -> texture ----
 * A decoded frame (or each half of a side-by-side one) is aspect-fit into the KIT_FRAME area of a
 * ring-sized RGB565 texture, black around it -- the same picture the old blit put on the screen,
 * but now the GPU draws it, so zoom, convergence and the ghost fix work exactly as on moflex.
 * Two texture sets alternate: the CPU fills one while the GPU may still be drawing the other. */
static C3D_Tex g_tex[2][2];
static int     g_tex_n = 0;           /* eyes allocated per set (1 = 2D, 2 = SBS) */
static Tex3DS_SubTexture g_texsub = { KIT_FRAME_W, KIT_FRAME_H, 0.0f, 1.0f,
                                      (float)KIT_FRAME_W / KIT_TEX_W, 1.0f - (float)KIT_FRAME_H / KIT_TEX_H };
static u32 g_tx_x[KIT_FRAME_W], g_tx_y[KIT_FRAME_H];   /* tiled-offset halves (x part + y part) */
static u16 g_pqlut[65536];            /* picture settings, folded into one lookup per pixel */
static int g_pq_cur = 0, g_pq_key = -1;
static void tex_tables(void) {   /* r3_tile_off() split so the inner loop is one add */
    for (int x = 0; x < KIT_FRAME_W; x++)
        g_tx_x[x] = (u32)((x >> 3) * 64 + ((x & 1) | ((x & 2) << 1) | ((x & 4) << 2)));
    for (int y = 0; y < KIT_FRAME_H; y++)
        g_tx_y[y] = (u32)((y >> 3) * (KIT_TEX_W >> 3) * 64 + (((y & 1) << 1) | ((y & 2) << 2) | ((y & 4) << 3)));
}
/* Brightness / contrast / saturation from the VIDEO menu. moflex applies them in the Y2R colour
 * conversion; MVD converts colour itself, so here they are the same three operations on RGB:
 * saturation about the pixel's luma, contrast about mid-grey, brightness as an offset. */
static void pq_refresh(void) {
    int b, c, sa; kit_pq(&b, &c, &sa);
    int key = ((b + 64) << 16) | ((c + 64) << 8) | (sa + 128);
    if (key == g_pq_key) return;
    g_pq_key = key; g_pq_cur = (b || c || sa);
    if (!g_pq_cur) return;
    float con = 1.0f + c * 0.01f, sat = 1.0f + sa * 0.01f;
    for (int v = 0; v < 65536; v++) {
        int r = ((v >> 11) & 31) << 3, g = ((v >> 5) & 63) << 2, bl = (v & 31) << 3;
        r |= r >> 5; g |= g >> 6; bl |= bl >> 5;
        float y = 0.299f * r + 0.587f * g + 0.114f * bl;
        float o[3] = { y + (r - y) * sat, y + (g - y) * sat, y + (bl - y) * sat };
        int q[3];
        for (int k = 0; k < 3; k++) {
            float t = (o[k] - 128.0f) * con + 128.0f + b;
            q[k] = t < 0 ? 0 : (t > 255 ? 255 : (int)(t + 0.5f));
        }
        g_pqlut[v] = (u16)(((q[0] >> 3) << 11) | ((q[1] >> 2) << 5) | (q[2] >> 3));
    }
}
static int  g_lb_count = 0;
/* Letterbox in a decoded eye, mapped into KIT_FRAME coordinates (incl. the fit's own bars), for
 * the VIDEO menu's Fill screen -- lb_sample's rules on an RGB565 frame. */
static int px_lit(u16 v) {
    int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    return ((r * 8 * 77 + g * 4 * 150 + b * 8 * 29) >> 8) > 30;   /* LB_BLACK */
}
static void lb_measure(const u16 *src, int stride, int sx, int sw, int sh,
                       int dxoff, int dyoff, int dw, int dh) {
    int t, b, l, r, x, y;
    for (y = 0; y < sh / 2; y++) { const u16 *row = src + (size_t)y * stride + sx; int lit = 0;
        for (x = 0; x < sw; x += 8) if (px_lit(row[x])) { lit = 1; break; } if (lit) break; }
    t = y;
    for (y = 0; y < sh / 2; y++) { const u16 *row = src + (size_t)(sh - 1 - y) * stride + sx; int lit = 0;
        for (x = 0; x < sw; x += 8) if (px_lit(row[x])) { lit = 1; break; } if (lit) break; }
    b = y;
    if (t >= sh / 2 - 1 && b >= sh / 2 - 1) return;          /* wholly black frame: tells us nothing */
    for (x = 0; x < sw / 2; x++) { int lit = 0;
        for (y = t; y < sh - b; y += 8) if (px_lit(src[(size_t)y * stride + sx + x])) { lit = 1; break; } if (lit) break; }
    l = x;
    for (x = 0; x < sw / 2; x++) { int lit = 0;
        for (y = t; y < sh - b; y += 8) if (px_lit(src[(size_t)y * stride + sx + sw - 1 - x])) { lit = 1; break; } if (lit) break; }
    r = x;
    kit_lb_merge(dyoff + t * dh / sh, (KIT_FRAME_H - dyoff - dh) + b * dh / sh,
                 dxoff + l * dw / sw, (KIT_FRAME_W - dxoff - dw) + r * dw / sw);
}
static void frame_to_tex(int slot, int fmt, int set) {   /* fmt: 0 2D, 1 SBS, 2 half SBS */
    int w, h, stride; const u16 *src = mp4_mvd_frame(slot, &w, &h, &stride);
    if (!src || w <= 0 || h <= 0) return;
    pq_refresh();
    int measure = (g_lb_count++ % 24) == 0;          /* a few times a second is plenty */
    for (int eye = 0; eye < (fmt ? 2 : 1) && eye < g_tex_n; eye++) {
        int sw = fmt ? w / 2 : w, sx = eye ? sw : 0, sh = h, dw, dh;
        /* half SBS squeezes each eye to half width: fit it as if it were twice as wide, so the
         * sampling below stretches it back out */
        int aw = (fmt == 2) ? sw * 2 : sw;
        if (aw * KIT_FRAME_H <= sh * KIT_FRAME_W) { dh = KIT_FRAME_H; dw = aw * KIT_FRAME_H / sh; }
        else                                      { dw = KIT_FRAME_W; dh = sh * KIT_FRAME_W / aw; }
        if (dw < 1) dw = 1; if (dw > KIT_FRAME_W) dw = KIT_FRAME_W;
        if (dh < 1) dh = 1; if (dh > KIT_FRAME_H) dh = KIT_FRAME_H;
        int dxoff = (KIT_FRAME_W - dw) / 2, dyoff = (KIT_FRAME_H - dh) / 2;
        static int srcx[KIT_FRAME_W];
        for (int dx = 0; dx < dw; dx++) srcx[dx] = sx + dx * sw / dw;
        u16 *dst = (u16 *)g_tex[set][eye].data;
        for (int y = 0; y < KIT_FRAME_H; y++) {
            u32 ty = g_tx_y[y];
            int in = (y >= dyoff && y < dyoff + dh);
            if (!in) { for (int x = 0; x < KIT_FRAME_W; x++) dst[ty + g_tx_x[x]] = 0; continue; }
            const u16 *row = src + (size_t)((y - dyoff) * sh / dh) * stride;
            for (int x = 0; x < dxoff; x++) dst[ty + g_tx_x[x]] = 0;
            if (g_pq_cur) for (int x = 0; x < dw; x++) dst[ty + g_tx_x[dxoff + x]] = g_pqlut[row[srcx[x]]];
            else          for (int x = 0; x < dw; x++) dst[ty + g_tx_x[dxoff + x]] = row[srcx[x]];
            for (int x = dxoff + dw; x < KIT_FRAME_W; x++) dst[ty + g_tx_x[x]] = 0;
        }
        C3D_TexFlush(&g_tex[set][eye]);
        if (measure && eye == 0) lb_measure(src, stride, sx, sw, sh, dxoff, dyoff, dw, dh);
    }
}
static void tex_free(void) {
    for (int s = 0; s < 2; s++) for (int e = 0; e < g_tex_n; e++) C3D_TexDelete(&g_tex[s][e]);
    g_tex_n = 0;
}
static int tex_alloc(int eyes) {
    g_tex_n = 0;
    for (int e = 0; e < eyes; e++) {
        for (int s = 0; s < 2; s++) {
            if (!C3D_TexInit(&g_tex[s][e], KIT_TEX_W, KIT_TEX_H, GPU_RGB565)) {
                for (int s2 = 0; s2 < s; s2++) C3D_TexDelete(&g_tex[s2][e]);
                tex_free(); return 0;
            }
            C3D_TexSetFilter(&g_tex[s][e], GPU_LINEAR, GPU_LINEAR);
            memset(g_tex[s][e].data, 0, KIT_TEX_W * KIT_TEX_H * 2);   /* black until a frame lands */
            C3D_TexFlush(&g_tex[s][e]);
        }
        g_tex_n = e + 1;
    }
    tex_tables();
    return 1;
}

/* Half-SBS from the file name (see the fmt rule in mp4_play). Tokens are matched whole, so an
 * "lr" inside a word ("Ultra") does not count. */
static int name_has_token(const char *name, const char *tok) {
    size_t tl = strlen(tok);
    for (const char *p = name; *p; p++) {
        if (strncasecmp(p, tok, tl)) continue;
        int pre = (p == name) || !isalnum((unsigned char)p[-1]);
        int post = !isalnum((unsigned char)p[tl]);
        if (pre && post) return 1;
    }
    return 0;
}
static int fmt_from_name(const char *path) {
    const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
    static const char *const HALF[] = { "hsbs", "h-sbs", "h.sbs", "h_sbs", "halfsbs", "half-sbs",
                                        "half.sbs", "half_sbs", "half sbs" };
    for (size_t i = 0; i < sizeof HALF / sizeof HALF[0]; i++) if (name_has_token(b, HALF[i])) return 2;
    if (name_has_token(b, "sbs") || name_has_token(b, "lr")) return 2;   /* a 16:9 SBS frame is half */
    return 0;
}

/* quick full-screen message (bottom, ui) with B/A to dismiss. Lines are word-wrapped to the
 * screen -- the long ones used to run off both edges. */
static void mp4_msg(const char *m) {
    gfxSetScreenFormat(GFX_BOTTOM, GSP_RGB565_OES);
    char lines[12][48]; int nl = 0;
    const int maxw = UI_W - 20;
    const char *p = m;
    while (*p && nl < 12) {
        const char *e = strchr(p, '\n'); size_t L = e ? (size_t)(e - p) : strlen(p);
        char para[256]; snprintf(para, sizeof para, "%.*s", (int)(L < 255 ? L : 255), p);
        char *w = para;
        do {   /* greedy word wrap of one paragraph */
            char cur[48] = ""; 
            while (*w) {
                while (*w == ' ') w++;
                char *we = w; while (*we && *we != ' ') we++;
                char trial[96]; snprintf(trial, sizeof trial, "%s%s%.*s", cur, cur[0] ? " " : "", (int)(we - w), w);
                if (cur[0] && (ui_text_w(1, trial) > maxw || strlen(trial) >= sizeof cur)) break;
                snprintf(cur, sizeof cur, "%.47s", trial);
                w = we;
            }
            if (nl < 12) snprintf(lines[nl++], sizeof lines[0], "%s", cur);
        } while (*w && nl < 12);
        p = e ? e + 1 : p + L;
    }
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & (KEY_A | KEY_B)) break;
        ui_begin(GFX_BOTTOM);
        ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);
        int y0 = 100 - nl * 7; if (y0 < 16) y0 = 16;
        for (int i = 0; i < nl; i++) ui_text_center(UI_W / 2, y0 + i * 14, 1, UI_NEON, lines[i]);
        ui_text_center(UI_W / 2, 214, 1, UI_DIM, "Press B");
        ui_present();
        gspWaitForVBlank();
    }
}

MoflexResult mp4_play(const char *path) {
    bool isnew = false; APT_CheckNew3DS(&isnew);
    if (!isnew) { mp4_msg("MP4 needs a New 3DS (hardware H.264)."); return MOFLEX_QUIT_BACK; }

    Mp4 m;
    if (!mp4_open(&m, path)) {
        mp4_msg(mp4_open_error == MP4_ERR_NOMEM
                    ? "Not enough memory to load this\nMP4's index. Close other apps\nand try again."
                    : "Could not open the MP4 (no H.264 track).");
        return MOFLEX_QUIT_BACK;
    }

    /* Side-by-side 3D = a double-wide frame. Detect by ASPECT, not absolute width, so any
     * resolution works: two 16:9 halves = 3.55:1, two 400x240 halves = 3.33:1, while normal 2D
     * films top out around 2.4:1. Threshold 2.7:1 splits them cleanly. */
    int sbs = (m.height > 0 && m.width * 10 > m.height * 27);
    /* fmt: 0 = 2D, 1 = SBS, 2 = half SBS. A double-wide frame is SBS by its shape alone. A normal
     * 16:9 frame can be 2D or HALF SBS (two squeezed eyes) and the pixels cannot say which, so
     * the name decides -- "HSBS", "Half-SBS", or an "SBS" / "LR" tag on a frame too narrow to be
     * full SBS -- and the VIDEO menu's Format row overrides it per video. */
    int fmt = sbs ? 1 : fmt_from_name(path);
    { int saved = kit_fmt_load(path); if (saved >= 0) fmt = saved; }
    int is3d = (fmt != 0);
    /* (the screens are set up by kit_gpu_begin once the file has passed every check below) */

    /* VALIDATE BEFORE TOUCHING THE HARDWARE. There was no check at all here: an HEVC (hvc1/hev1) file
     * matched nothing in the stsd parser, so width/height/avcc_len all stayed 0 and went straight into
     * mvdstdInit() -- a zero-size linearAlloc and a 0x0 frame handed to the video hardware. That is the
     * crash. Same for an oversized file: MVD has real limits and we allocate w*h*2 up front. */
    if (!m.has_video || m.vfourcc != MP4_FOURCC_AVC1) {
        char msg[96];
        uint32_t fc = m.vfourcc;
        if (!m.has_video)
            snprintf(msg, sizeof msg, "No video track in this MP4.");
        else
            snprintf(msg, sizeof msg,
                     "Video is '%c%c%c%c', not H.264.\nRe-encode with x264 (H.264/AVC).",
                     (char)(fc >> 24), (char)(fc >> 16), (char)(fc >> 8), (char)fc);
        mp4_msg(msg);
        mp4_close(&m); return MOFLEX_QUIT_BACK;
    }
    if (m.width <= 0 || m.height <= 0 || m.avcc_len <= 0) {
        mp4_msg("MP4 video track is missing its H.264 config (avcC).");
        mp4_close(&m); return MOFLEX_QUIT_BACK;
    }
    /* The container can LIE about the codec (an HEVC stream remuxed under an 'avc1' label
     * hard-crashes the MVD sysmodule = full system crash). A real H.264 avcC starts with
     * version 1 and its first SPS NAL has type 7 -- HEVC bits fail both. */
    if (m.avcc[0] != 1 || (m.avcc_len > 8 && (m.avcc[8] & 0x1F) != 7)) {
        mp4_msg("Video stream is not valid H.264\n(mislabeled file?). Re-encode\nwith x264 (H.264/AVC).");
        mp4_close(&m); return MOFLEX_QUIT_BACK;
    }
    /* PROFILE. MVD is 8-bit 4:2:0 only, and nothing above checks for that. A 10-bit encode --
     * which is what you get by transcoding a 10-bit HEVC source without forcing the pixel
     * format -- is High 10, passes every test above, initialises the hardware happily, and then
     * NEVER RETURNS A PICTURE. On screen that is a frozen frame whose timeline still moves when
     * you seek, which tells the user nothing. Name the reason instead.
     * avcC byte 1 is AVCProfileIndication. */
    {
        int prof = m.avcc[1];
        if (prof != 66 && prof != 77 && prof != 88 && prof != 100) {
            const char *what;
            switch (prof) {
                case 110: what = "10-bit (High 10)";      break;
                case 122: what = "4:2:2 (High 4:2:2)";    break;
                case 244: what = "4:4:4 (High 4:4:4)";    break;
                case 44:  what = "CAVLC 4:4:4 Intra";     break;
                default:  what = NULL;                    break;
            }
            char msg[160];
            if (what)
                snprintf(msg, sizeof msg,
                         "This is %s H.264.\nThe 3DS decoder is 8-bit 4:2:0 only.\n"
                         "Re-encode with -pix_fmt yuv420p", what);
            else
                snprintf(msg, sizeof msg,
                         "H.264 profile %d is not supported\n(8-bit 4:2:0 only). Re-encode with\n"
                         "-profile:v high -pix_fmt yuv420p", prof);
            mp4_msg(msg);
            mp4_close(&m); return MOFLEX_QUIT_BACK;
        }
    }
    /* SIZE LIMIT, before the hardware is touched. 1080p does not fit beside the screens in this
     * memory mode, and when it was let through the mvd SYSMODULE either produced nothing (player
     * stuck) or data-aborted -- a full system crash no app can catch. 1280x720 is the largest
     * frame verified on hardware, so anything with more coded pixels is refused here, side-by-side
     * 3D included (two 720p eyes = 2560x720). */
    {
        int cw = (m.width + 15) & ~15, ch = (m.height + 15) & ~15;
        if ((long)cw * ch > 1280L * 720L) {
            char msg[200];
            snprintf(msg, sizeof msg,
                     "Not enough memory to play this %dx%d video.\n"
                     "Re-encode at 1280x720 - or better, the 3DS's native size: "
                     "400x240 for 2D, 800x240 for 3D side-by-side.",
                     m.width, m.height);
            mp4_msg(msg);
            mp4_close(&m); return MOFLEX_QUIT_BACK;
        }
    }

    /* title = filename without extension */
    char title[128];
    { const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
      snprintf(title, sizeof title, "%s", b);
      size_t L = strlen(title); if (L > 4 && !strcasecmp(title + L - 4, ".mp4")) title[L - 4] = 0; }

    moflex_subs_autoload(path);   /* load a matching .srt (sidecar or moviedata/) if one exists */
    moflex_defaults_apply_subs(NULL);   /* the viewer's subtitle default (no language tags here) */

    /* ---- the moflex player's screens FIRST, then the decoder ----
     * The decoder's frame buffers are huge (4 MB a slot at 1080p) and come out of the same linear
     * heap as the framebuffers, the GPU command buffer and the textures. Starting the decoder
     * first let a 1080p file take nearly all of it, and the screens could not be set up after:
     * both stayed black with the player stuck. With the screens reserved first, a file that is
     * too big is refused by the decoder instead, with a message that says so. */
    MoflexResult result = MOFLEX_QUIT_BACK;
    KitGpu gpu;
    if (!kit_gpu_begin(&gpu, is3d)) {
        mp4_msg("Could not start the graphics for playback.");
        mp4_close(&m); return MOFLEX_QUIT_BACK;
    }
    if (!tex_alloc(2)) {                  /* both eyes always: the Format row can switch to 3D */
        kit_gpu_end(&gpu, 0);
        mp4_msg("Not enough memory for the video textures.");
        mp4_close(&m); return MOFLEX_QUIT_BACK;
    }

    uint32_t maxsz = 1;
    for (int i = 0; i < m.v_count; i++) if (mp4_s_size(&m.vsamples[i]) > maxsz) maxsz = mp4_s_size(&m.vsamples[i]);
    mp4_mvd_hint_max_sample(maxsz);
    {   int mi = mp4_mvd_init(m.width, m.height, m.avcc, m.avcc_len, m.nal_length_size);
        if (mi <= 0) {
            char msg[200];
            if (mi < 0)   /* the decoder + its frame buffers did not fit in video memory */
                snprintf(msg, sizeof msg,
                         "Not enough memory to play this %dx%d video.\n"
                         "Re-encode at 1280x720 - or better, the 3DS's native size: "
                         "400x240 for 2D, 800x240 for 3D side-by-side.",
                         m.width, m.height);
            else
                snprintf(msg, sizeof msg,
                         "The 3DS video decoder refused this H.264 stream (profile/level). "
                         "Re-encode with -profile:v high -level 4.1 -pix_fmt yuv420p.");
            tex_free(); kit_gpu_end(&gpu, 0);
            mp4_msg(msg);
            mp4_close(&m); return MOFLEX_QUIT_BACK;
        }
        g_pq_depth = mp4_mvd_slots() - 1;
        if (g_pq_depth > PQ_DEPTH_MAX) g_pq_depth = PQ_DEPTH_MAX;
        if (g_pq_depth < 1) g_pq_depth = 1;
    }

    uint8_t *buf = (uint8_t *)malloc(maxsz + 16);
    if (!buf) { mp4_mvd_exit(); tex_free(); kit_gpu_end(&gpu, 0); mp4_close(&m); return MOFLEX_ERROR; }

    /* ---- audio ---- */
    uint8_t *atmp = NULL;
    memset(g_wb, 0, sizeof g_wb); memset(g_abuf, 0, sizeof g_abuf); memset(g_slot_ns, 0, sizeof g_slot_ns);
    g_have_audio = 0; g_a_next = 0; g_a_played = 0; g_a_started = 0; g_a_base_us = 0;
    if (m.has_audio && mp4_aac_open(&g_aac, m.asc, m.asc_len, m.a_rate, m.a_channels)) {
        uint32_t amax = 1;
        for (int i = 0; i < m.a_count; i++) if (mp4_s_size(&m.asamples[i]) > amax) amax = mp4_s_size(&m.asamples[i]);
        atmp = (uint8_t *)malloc(amax + 16);
        int ok = (atmp != NULL);
        for (int i = 0; i < NWB && ok; i++) {
            g_abuf[i] = (int16_t *)linearAlloc(AAC_MAXSAMP * g_aac.channels * 2);
            if (!g_abuf[i]) ok = 0; else g_wb[i].status = NDSP_WBUF_DONE;
        }
        if (ok) {
            ndspChnReset(0);
            ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
            ndspChnSetRate(0, (float)g_aac.rate);
            ndspChnSetFormat(0, g_aac.channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
            float mix[12]; memset(mix, 0, sizeof mix); mix[0] = mix[1] = 1.0f; ndspChnSetMix(0, mix);
            g_have_audio = 1;
        } else {
            for (int i = 0; i < NWB; i++) if (g_abuf[i]) { linearFree(g_abuf[i]); g_abuf[i] = NULL; }
            free(atmp); atmp = NULL; mp4_aac_close(&g_aac);
        }
    }

    C2D_Image img[2][2];
    for (int st = 0; st < 2; st++) for (int e = 0; e < 2; e++)
        img[st][e] = (C2D_Image){ &g_tex[st][e < g_tex_n ? e : 0], &g_texsub };
    kit_film_begin(is3d);
    kit_fmt_set(fmt);
    kit_saves_baseline();                 /* a menu closed with no change writes nothing */
    aptHook(&g_mp4_apt_cookie, mp4_apt_hook, NULL);   /* HOME/sleep must release gsp::Lcd */
    g_playing = 1;

    double dur_s = mp4_duration_s(&m);
    int64_t dur_us = (int64_t)(dur_s * 1e6);

    int vi = 0;
    int64_t cur_us = 0;
    g_pq_n = 0;        /* the reorder queue is static: entries from a PREVIOUS session (B-exit
                        * leaves up to 4 queued) would present as phantom frames with stale
                        * timestamps -- the frozen-bar + fast-forward-catch-up bug */
    g_ftn = 0; g_ft_full = 0; g_ft_dry = 0; g_ft_push = 0; g_ft_pop = 0;
    g_present_log = 12; g_lb_count = 0; g_pq_key = -1;

    int shown = -1;                       /* texture set on screen (-1: none yet, set 0 is black) */
    int shown_slot = -1;                  /* the MVD slot it came from (re-converted while paused) */
    #define SHOW_SLOT(sl) do { int ns_ = shown < 0 ? 0 : 1 - shown; frame_to_tex((sl), fmt, ns_); \
                               shown = ns_; shown_slot = (sl); } while (0)

    /* Paint the panel over a black picture NOW (both buffers), as the ring does while it loads:
     * a resume decodes up to a keyframe interval before the first picture exists. */
    for (int f = 0; f < 2; f++) kit_draw(&gpu, img[0][0], img[0][1], is3d, title, 0, dur_us, 1);

    /* PRE-ROLL GATE. The sound and the clock wait until the first picture is on screen. Starting
     * them at once (the old way) meant the decoder's first pictures were already late when they
     * surfaced: the film opened on sound with a black picture and found its sync seconds later,
     * at a slightly different spot each time. Armed at open and after every seek. */
    int gate = 1;
    int gate_fed = 0;                     /* frames fed since open/seek with no picture back yet */
    #define GATE_ARM() do { gate = 1; gate_fed = 0; if (g_have_audio) ndspChnSetPaused(0, true); clk_pause(); } while (0)

    /* auto-resume (like moflex): jump to the saved position if any */
    int64_t resume_us = (int64_t)moflex_resume_get(path);
    if (g_have_audio) pump_audio(&m, atmp);
    if (resume_us > 3000000 && (dur_us <= 0 || resume_us < dur_us - 5000000)) {
        int sl = do_seek(&m, buf, atmp, dur_us, resume_us, &vi);
        cur_us = resume_us;
        if (sl >= 0) SHOW_SLOT(sl);
    } else
        clk_set(0, 1);
    GATE_ARM();

    int video_failed = 0;                 /* the watchdog already tore the screens down */
    int quit = 0, was_paused = 0, vol_dirty = 0, shold = 0;
    int scrubbing = 0, vol_drag = 0; int64_t scrub_us = 0;
    int want_seek = 0; int64_t seek_to = 0;
    int dirty = 1, dirty2 = 0, last_sec = -1, drops = 0;
    u64 behind_since = 0;                 /* when the picture fell well behind the sound (0 = it isn't) */

    /* One loop, the ring engine's shape: input -> seek -> decode/present when due -> draw when
     * something changed, else wait a refresh. No second input scan inside a frame wait. */
    while (!quit && aptMainLoop()) {
        hidScanInput();
        u32 kh = hidKeysHeld(), kd = tap_filter(hidKeysDown(), kh);
        touchPosition tp; hidTouchRead(&tp);
        if (kit_take_redraw()) { gfxSet3D(is3d); dirty = 1; }   /* back from HOME/sleep */
        {   int nf;                       /* Format changed in the VIDEO menu */
            if (kit_fmt_take_change(&nf)) {
                fmt = nf; is3d = (fmt != 0);
                gfxSet3D(is3d); kit_set_view3d(is3d);
                kit_fmt_save(path, fmt);
                if (shown_slot >= 0 && !g_playing) SHOW_SLOT(shown_slot);   /* playing: next frame */
                dirty = 1;
            } }
        kit_input(&kd, &kh, &dirty);

        if (kit_menu_open()) {            /* CC / AUDIO / VIDEO own input; the film plays on */
            kit_menu_input(kd, kh, tp, is3d, path);
            /* repaint on input only: an idle open menu must cost no more than the panel */
            if (kd || (kh & (KEY_TOUCH | KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN))) dirty = 1;
        } else {
            if (kd & KEY_B) { result = MOFLEX_QUIT_BACK; break; }
            int toggle = (kd & KEY_A) ? 1 : 0;
            if (kd & KEY_UP)     { kit_vol_step(1);  vol_dirty = 1; dirty = 1; }
            if (kd & KEY_DOWN)   { kit_vol_step(-1); vol_dirty = 1; dirty = 1; }
            if (kd & KEY_Y)      { kit_toggle_subs(); dirty = 1; }
            if (kd & KEY_SELECT) { kit_open_subs_menu(); dirty = 1; }
            if (kd & KEY_X)      { kit_toggle_dark(); dirty = 1; }
            {   /* D-pad seek: accumulate while held, land once on release (the ring's rule) */
                int sdir = (kh & KEY_RIGHT) ? 1 : ((kh & KEY_LEFT) ? -1 : 0);
                if (sdir == 0) shold = 0;
                else { int fire = (kd & (KEY_RIGHT | KEY_LEFT)) ? 1 : 0;
                       if (!fire) { shold++; if (shold > 8 && (shold % 3 == 0)) fire = 1; }
                       if (fire) { seek_to = (want_seek ? seek_to : cur_us) + (int64_t)sdir * SEEK_STEP_US;
                                   want_seek = 1; dirty = 1; } } }
            if (kd & KEY_TOUCH) {
                switch (kit_touch(tp.px, tp.py, is3d)) {
                    case KIT_T_SCRUB:   scrubbing = 1; break;
                    case KIT_T_VOL:     vol_drag = 1; break;
                    case KIT_T_PLAY:    toggle = 1; break;
                    case KIT_T_RW:      seek_to = cur_us - SEEK_STEP_US; want_seek = 1; break;
                    case KIT_T_FF:      seek_to = cur_us + SEEK_STEP_US; want_seek = 1; break;
                    case KIT_T_OPEN:    result = MOFLEX_QUIT_OPEN;   quit = 1; break;
                    case KIT_T_MANAGE:  result = MOFLEX_QUIT_MANAGE; quit = 1; break;
                    case KIT_T_ADD:     result = MOFLEX_QUIT_ADD;    quit = 1; break;
                    default: break;
                }
                dirty = 1;
            }
            if (toggle) { g_playing = !g_playing; dirty = 1; }
            if (scrubbing) {              /* drag previews, release seeks */
                if (kh & KEY_TOUCH) { scrub_us = kit_scrub_us(tp.px, dur_us); dirty = 1; }
                else { seek_to = scrub_us; want_seek = 1; scrubbing = 0; }
            }
            if (vol_drag) {
                if (kh & KEY_TOUCH) { if (kit_vol_drag(tp.py)) vol_dirty = 1; dirty = 1; }
                else vol_drag = 0;
            }
        }
        if (quit) break;

        if (want_seek && !scrubbing && !(kh & (KEY_LEFT | KEY_RIGHT))) {
            if (seek_to < 0) seek_to = 0;
            if (dur_us > 0 && seek_to > dur_us) seek_to = dur_us;
            if (was_paused) { g_playing = 1; }   /* a seek resumes, as on moflex */
            int sl = do_seek(&m, buf, atmp, dur_us, seek_to, &vi);
            cur_us = seek_to; want_seek = 0;
            if (sl >= 0) SHOW_SLOT(sl);
            GATE_ARM();
            behind_since = 0;
            dirty = 1;
        }

        int newframe = 0;
        if (!g_playing) {
            if (!was_paused) {
                if (g_have_audio) ndspChnSetPaused(0, true);
                clk_pause();
                if (vol_dirty) { kit_vol_save(); vol_dirty = 0; }
                kit_flush_saves(path);    /* menu changes: written now that nothing streams */
                if (cur_us > 3000000 && (dur_us <= 0 || cur_us < dur_us - 5000000)) moflex_resume_save(path, cur_us);
                was_paused = 1; dirty = 1;
            }
            /* Picture settings changed on a PAUSED frame: re-convert it, or nothing would change
             * until playback resumed -- exactly where you want to judge them (the ring does the
             * same). Nothing decodes while paused, so the frame's slot is still intact. */
            if (shown_slot >= 0) {
                int b, c, sa; kit_pq(&b, &c, &sa);
                if ((((b + 64) << 16) | ((c + 64) << 8) | (sa + 128)) != g_pq_key) { SHOW_SLOT(shown_slot); dirty = 1; }
            }
        } else {
            if (was_paused) { if (!gate) { if (g_have_audio) ndspChnSetPaused(0, false); clk_resume(); }
                              was_paused = 0; dirty = 1; }
            pump_audio(&m, atmp);

            /* ---- EOF: only after the reorder queue has fully drained ---- */
            if (vi >= m.v_count && g_pq_n == 0 && !mp4_mvd_pending()) {
                moflex_resume_clear(path);
                moflex_watched_set(path, 1);
                result = MOFLEX_EOF; break;
            }

            /* ---- decode ahead: keep the reorder queue full so frames can present in DISPLAY
             * order (B-frames arrive out of order; presenting by decode order judders motion) ---- */
            int fed_pass = 0;
            while (g_pq_n < g_pq_depth && (vi < m.v_count || mp4_mvd_pending())) {
                /* Bounded: a decoder that takes frames but returns no pictures used to keep this
                 * loop feeding the whole file with the screen frozen and B dead. A few feeds per
                 * pass, then back out to input and drawing. */
                if (fed_pass >= 8) break;
                int consumed = 0, r = 0;
                u64 t0 = osGetTime(), t1 = t0, t2;
                if (!mp4_mvd_pending() && vi < m.v_count) {
                    Mp4Sample *sm = &m.vsamples[vi];
                    int n = mp4_read_sample(&m, sm, buf);
                    t1 = osGetTime();
                    if (g_ftn < FT_MAX) { g_ft[g_ftn++] = v_cts_us(&m, vi); g_ft_push++; }
                    else g_ft_full++;                       /* timestamp DROPPED, frame still decoded */
                    r = (n == (int)mp4_s_size(sm)) ? mp4_mvd_pump(buf, n, &consumed) : 0;
                    vi++;
                } else {
                    r = mp4_mvd_pump(NULL, 0, &consumed);   /* drain a held picture */
                    if (!r && vi >= m.v_count) break;       /* tail fully drained */
                }
                t2 = osGetTime();
                pump_audio(&m, atmp);
                {   /* stall forensics: any pump spending >50ms is a hiccup -- log it */
                    extern void mvd_log(const char *fmt, ...);
                    u32 rd = (u32)(t1 - t0), dec = (u32)(t2 - t1), aud = (u32)(osGetTime() - t2);
                    if (rd + dec + aud > 50)
                        mvd_log("SPIKE f=%d read=%lums pump=%lums audio=%lums",
                                vi, (unsigned long)rd, (unsigned long)dec, (unsigned long)aud);
                }
                if (consumed) { fed_pass++; if (gate) gate_fed++; }
                if (r) gate_fed = 0;
                if (r && g_ftn > 0) { g_pq[g_pq_n].cts = ft_pop_min(); g_ft_pop++; g_pq[g_pq_n].slot = r - 1; g_pq_n++; }
                else if (r) g_ft_dry++;            /* picture surfaced with no display time to give it */
                else if (!consumed) break;         /* dry + nothing fed: nothing more this pass */
            }

            /* ~10 s of video fed and still not one picture: the hardware is not decoding this
             * stream (too big for its working memory, or something it silently rejects) */
            if (gate && gate_fed > 240) {
                tex_free(); kit_film_end(); aptUnhook(&g_mp4_apt_cookie);
                if (g_have_audio) { ndspChnWaveBufClear(0); ndspChnReset(0); }
                kit_gpu_end(&gpu, 0);
                mp4_msg("Not enough memory to play this video.\n"
                        "Re-encode at 1280x720 - or better, the 3DS's native size: "
                        "400x240 for 2D, 800x240 for 3D side-by-side.");
                result = MOFLEX_QUIT_BACK; video_failed = 1; break;
            }
            if (g_pq_n > 0 && gate) {
                /* first picture after open/seek: show it, THEN let the sound and clock run */
                int mi = 0;
                for (int i = 1; i < g_pq_n; i++) if (g_pq[i].cts < g_pq[mi].cts) mi = i;
                cur_us = g_pq[mi].cts;
                SHOW_SLOT(g_pq[mi].slot); newframe = 1;
                g_pq[mi] = g_pq[--g_pq_n];
                gate = 0;
                if (g_have_audio) ndspChnSetPaused(0, false);
                else clk_set(cur_us, 1);
            } else if (g_pq_n > 0) {
                /* Every picture that is due now, oldest first; only the NEWEST of them is shown.
                 * On time that is exactly one. Running late (a slow stretch, a menu redrawing
                 * every frame) it skips the stale ones and catches up -- the old loop showed
                 * every frame late, so any slowdown became permanent drift -- and something is
                 * always shown, never a black screen waiting to be on time. */
                int64_t now = media_now_us();
                int pick = -1; int64_t pick_cts = 0;
                while (g_pq_n > 0) {
                    int mi = 0;
                    for (int i = 1; i < g_pq_n; i++) if (g_pq[i].cts < g_pq[mi].cts) mi = i;
                    if (g_pq[mi].cts > now) break;
                    if (pick >= 0) drops++;
                    pick = g_pq[mi].slot; pick_cts = g_pq[mi].cts;
                    g_pq[mi] = g_pq[--g_pq_n];    /* consumed */
                }
                if (pick >= 0) {
                    cur_us = pick_cts;
                    /* RESYNC. Skipping stale pictures catches up only as fast as the decoder can
                     * outrun the film, and a 720p stream has little to spare: after a long stall
                     * (a menu being worked, a slow card) it could stay seconds behind. If it is
                     * still well behind after a while, jump the picture to the sound instead --
                     * a short pause, then in sync. Normal playback is never more than a frame
                     * or two behind, so this does not fire on its own. */
                    if (now - cur_us > 750000 && !gate) {
                        u64 tnow = osGetTime();
                        if (!behind_since) behind_since = tnow;
                        else if (tnow - behind_since > 1500 && !want_seek) {
                            extern void mvd_log(const char *fmt, ...);
                            mvd_log("RESYNC video=%lldms audio=%lldms", (long long)(cur_us / 1000), (long long)(now / 1000));
                            seek_to = now; want_seek = 1; behind_since = 0;
                        }
                    } else behind_since = 0;
                    if (g_present_log > 0) {
                        extern void mvd_log(const char *fmt, ...);
                        g_present_log--;
                        mvd_log("PRESENT cts=%lldms late=%lldms queued=%d drops=%d",
                                (long long)(cur_us / 1000), (long long)((now - cur_us) / 1000), g_pq_n, drops);
                    }
                    SHOW_SLOT(pick);
                    newframe = 1;
                    /* A/V drift readout, once a minute. Negative = video behind the audio. */
                    if (g_have_audio) {
                        static int64_t s_drift_next = 60000000LL;
                        if (cur_us < s_drift_next - 120000000LL) s_drift_next = cur_us + 60000000LL;
                        if (cur_us >= s_drift_next) {
                            extern void mvd_log(const char *fmt, ...);
                            mvd_log("AV t=%llds video-vs-audio=%+lldms pool=%d fed=%ld out=%ld lost=%d dropped_ts=%d drops=%d",
                                    (long long)(cur_us / 1000000), (long long)((cur_us - media_now_us()) / 1000),
                                    g_ftn, g_ft_push, g_ft_pop, g_ft_dry, g_ft_full, drops);
                            s_drift_next = cur_us + 60000000LL;
                        }
                    }
                }
            }
        }

        int64_t disp_us = scrubbing ? scrub_us : (want_seek ? seek_to : cur_us);
        int sec = (int)(disp_us / 1000000);
        if (sec != last_sec) { last_sec = sec; dirty = 1; }   /* the panel clock ticks */
        if (newframe || dirty || dirty2) {
            /* citro3d double-buffers each target: a repaint with no new picture runs twice so
             * both buffers carry it (the ring's dirty2 rule) */
            int st = shown < 0 ? 0 : shown;
            kit_draw(&gpu, img[st][0], img[st][1], is3d, title, disp_us, dur_us, g_playing);
            dirty2 = dirty && !newframe; dirty = 0;
        } else gspWaitForVBlank();
    }
    if (!aptMainLoop() && result == MOFLEX_QUIT_BACK) result = MOFLEX_QUIT_EXIT;
    #undef SHOW_SLOT
    #undef GATE_ARM

    /* ---- save state on exit ---- */
    if (vol_dirty) kit_vol_save();
    kit_flush_saves(path);
    if (result != MOFLEX_EOF) {
        if (cur_us > 3000000 && (dur_us <= 0 || cur_us < dur_us - 5000000)) moflex_resume_save(path, cur_us);
        else if (dur_us > 0 && cur_us >= dur_us - 5000000) moflex_resume_clear(path);
    }
    if (!video_failed) {
        aptUnhook(&g_mp4_apt_cookie);
        kit_film_end();
        if (g_have_audio) { ndspChnWaveBufClear(0); ndspChnReset(0); }
        tex_free();
        kit_gpu_end(&gpu, result == MOFLEX_QUIT_EXIT);
    }

    if (g_have_audio) {
        ndspChnWaveBufClear(0); ndspChnReset(0);
        for (int i = 0; i < NWB; i++) if (g_abuf[i]) { linearFree(g_abuf[i]); g_abuf[i] = NULL; }
        mp4_aac_close(&g_aac);
        g_have_audio = 0;
    }
    free(atmp);
    mp4_mvd_exit();
    free(buf);
    mp4_close(&m);
    return result;
}
