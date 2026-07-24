/* MP4/H.264 (New-3DS MVD hardware) + AAC-LC (Helix) player with the same on-screen controls as
 * the moflex player: bottom-screen panel (progress bar, play/pause, RW/FF, volume, BACK/OPEN/EXIT,
 * battery), A/L/R/Up/Down/B + touch, keyframe seek, auto-resume. Audio is the master clock. 2D and
 * side-by-side 3D. (Subtitles / bottom-screen-off are moflex-only for now.) */
#include "mp4_play.h"
#include "mp4_demux.h"
#include "mp4_mvd.h"
#include "mp4_aac.h"
#include "moflex_playback.h"   /* MoflexResult + shared resume/volume stores */
#include "ui_gfx.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- bottom-panel layout (mirrors the moflex player) ---- */
#define BAR_X 14
#define BAR_Y 60
#define BAR_W 292
#define BAR_H 10
#define PLAY_CX 160
#define PLAY_CY 118
#define RW_CX 108
#define FF_CX 212
#define VOL_X 298
#define VOL_Y 88
#define VOL_H 104
#define BTN_Y 208
#define BTN_H 28
#define BKB_X 8
#define BKB_W 96
#define OPB_X 112
#define OPB_W 96
#define EXB_X 216
#define EXB_W 96
#define CC_X 244
#define CC_Y 104
#define CC_W 48
#define CC_H 28
#define DIM_X 244
#define DIM_Y 138
#define DIM_W 48
#define DIM_H 28

#define NWB          16
#define AAC_MAXSAMP  2048
#define SEEK_STEP_US (10LL * 1000000)

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
static float       g_pvol = 1.0f;
static int         g_playing = 1;
static int         g_lcd_ok = 0, g_screen_off = 0;   /* bottom-screen-off (gsp::Lcd) */

/* wall clock, used as the master when a file has no audio (audio files use the DSP position) */
static int64_t     g_clk_base_us;
static u64         g_clk_t0;
static int         g_clk_running;
static void clk_set(int64_t us, int running) { g_clk_base_us = us; g_clk_t0 = osGetTime(); g_clk_running = running; }
static void clk_pause(void)  { if (g_clk_running) { g_clk_base_us += (int64_t)(osGetTime() - g_clk_t0) * 1000; g_clk_running = 0; } }
static void clk_resume(void) { if (!g_clk_running) { g_clk_t0 = osGetTime(); g_clk_running = 1; } }

/* ---- battery (local; New-3DS panel) ---- */
static int  g_batt = -1, g_batt_chg = 0, g_mcu_ok = 0;
static u64  g_batt_next = 0;
static void batt_refresh(void) {
    u64 now = osGetTime();
    if (g_batt >= 0 && now < g_batt_next) return;
    g_batt_next = now + 4000;
    u8 v = 0;
    if (g_mcu_ok && R_SUCCEEDED(MCUHWC_GetBatteryLevel(&v))) g_batt = v > 100 ? 100 : v;
    else { u8 l = 0; if (R_SUCCEEDED(PTMU_GetBatteryLevel(&l))) g_batt = l * 20; }
    u8 c = 0; if (R_SUCCEEDED(PTMU_GetBatteryChargeState(&c))) g_batt_chg = c;
}

static void fmt_time(int64_t us, char *o, size_t n) {
    if (us < 0) us = 0;
    int t = (int)(us / 1000000), h = t / 3600, m = (t % 3600) / 60, s = t % 60;
    if (h > 0) snprintf(o, n, "%d:%02d:%02d", h, m, s);
    else       snprintf(o, n, "%d:%02d", m, s);
}

/* time of a sample in microseconds */
static int64_t v_time_us(Mp4 *m, int i) {
    if (m->v_timescale <= 0) return 0;
    return (int64_t)(m->vsamples[i].dts * 1000000LL / (int64_t)m->v_timescale);
}
static int64_t a_time_us(Mp4 *m, int i) {
    if (m->a_timescale <= 0) return 0;
    return (int64_t)(m->asamples[i].dts * 1000000LL / (int64_t)m->a_timescale);
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
        int n  = mp4_read_sample(m, as, atmp);
        int sp = (n == (int)as->size) ? mp4_aac_decode(&g_aac, atmp, n, g_abuf[i]) : 0;
        g_a_next++;
        if (sp <= 0) continue;
        if (g_pvol != 1.0f) {
            int16_t *s = g_abuf[i]; int ns = sp * g_aac.channels;
            for (int k = 0; k < ns; k++) { int t = (int)(s[k] * g_pvol); s[k] = (int16_t)(t > 32767 ? 32767 : (t < -32768 ? -32768 : t)); }
        }
        DSP_FlushDataCache(g_abuf[i], sp * g_aac.channels * 2);
        memset(wb, 0, sizeof *wb);
        wb->data_vaddr = g_abuf[i]; wb->nsamples = sp; g_slot_ns[i] = sp;
        ndspChnWaveBufAdd(0, wb);
        g_a_started = 1;
    }
}

/* draw the bottom-screen control panel */
static void draw_panel(const char *title, int64_t cur, int64_t dur) {
    ui_begin(GFX_BOTTOM);
    ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);
    ui_text_clipped(10, 8, 1, UI_NEON, title, 10, VOL_X - 12);

    char tc[16], td[16], line[40];
    fmt_time(cur, tc, sizeof tc); fmt_time(dur, td, sizeof td);
    snprintf(line, sizeof line, "%s / %s", tc, td);
    ui_text(10, 26, 1, UI_NEONC, line);

    batt_refresh();
    if (g_batt >= 0) {
        char bs[8]; snprintf(bs, sizeof bs, "%d%%", g_batt);
        u16 bc = g_batt_chg ? UI_NEONC : (g_batt <= 15 ? UI_RED : g_batt <= 30 ? UI_RGB(255,180,60) : UI_NEON);
        int bw = 16, bh = 10, bx = 292, by = 24;
        int tx = (bx - 4) - ui_text_w(1, bs);
        ui_text(tx, 26, 1, bc, bs);
        ui_frame_round(bx, by, bw, bh, 2, bc, 1);
        ui_fill(bx + bw, by + 3, 2, bh - 6, bc);
        int fw = (bw - 4) * g_batt / 100; if (fw < 1 && g_batt > 0) fw = 1;
        ui_fill_round(bx + 2, by + 2, fw, bh - 4, 1, bc);
    }

    double frac = dur > 0 ? (double)cur / (double)dur : 0.0; if (frac > 1) frac = 1; if (frac < 0) frac = 0;
    ui_fill_round(BAR_X, BAR_Y, BAR_W, BAR_H, BAR_H / 2, TH_TRACK);
    int fw = (int)(BAR_W * frac);
    if (fw > 0) ui_fill_round(BAR_X, BAR_Y, fw, BAR_H, BAR_H / 2, UI_NEON);
    int kx = BAR_X + fw, ky = BAR_Y + BAR_H / 2, kr = 8;
    ui_glow_round(kx - kr, ky - kr, 2 * kr, 2 * kr, kr, UI_NEON, 4, 26);
    ui_fill_round(kx - kr, ky - kr, 2 * kr, 2 * kr, kr, UI_WHITE);

    ui_play_l(RW_CX - 5, PLAY_CY, 14, UI_NEONC); ui_play_l(RW_CX + 6, PLAY_CY, 14, UI_NEONC);
    ui_play(FF_CX - 6, PLAY_CY, 14, UI_NEONC);   ui_play(FF_CX + 5, PLAY_CY, 14, UI_NEONC);
    int R = 24;
    ui_glow_round(PLAY_CX - R, PLAY_CY - R, 2 * R, 2 * R, R, UI_NEON, 6, 22);
    ui_vgrad_round(PLAY_CX - R, PLAY_CY - R, 2 * R, 2 * R, R, UI_BG2, TH_BG1);
    ui_frame_round(PLAY_CX - R, PLAY_CY - R, 2 * R, 2 * R, R, UI_NEON, 2);
    if (g_playing) ui_pause(PLAY_CX, PLAY_CY, 22, UI_NEON);
    else           ui_play(PLAY_CX + 2, PLAY_CY, 22, UI_NEON);

    ui_fill_round(VOL_X, VOL_Y, 12, VOL_H, 6, TH_TRACK);
    int vf = (int)(VOL_H * (g_pvol / 4.0f));
    if (vf > 0) ui_fill_round(VOL_X, VOL_Y + VOL_H - vf, 12, vf, 6, UI_NEON);
    char vs[8]; snprintf(vs, sizeof vs, "%d%%", (int)(g_pvol * 100 + 0.5f));
    ui_text(VOL_X - 16, VOL_Y - 12, 1, UI_INK, vs);

    ui_button(BKB_X, BTN_Y, BKB_W, BTN_H, "BACK", 0, UI_NEONP);
    ui_button(OPB_X, BTN_Y, OPB_W, BTN_H, "OPEN", 0, UI_NEON);
    ui_button(EXB_X, BTN_Y, EXB_W, BTN_H, "EXIT", 0, UI_RED);

    /* subtitles: CC toggle/options (glows when on) */
    int cc = moflex_subs_on();
    ui_button(CC_X, CC_Y, CC_W, CC_H, "CC", cc, cc ? UI_NEON : UI_DIM);
    /* bottom-screen-off: crescent-moon button (video keeps playing on top) */
    if (g_lcd_ok) {
        ui_button(DIM_X, DIM_Y, DIM_W, DIM_H, "", 0, UI_NEONP);
        int mx = DIM_X + DIM_W / 2, my = DIM_Y + DIM_H / 2, mr = 8;
        ui_fill_round(mx - mr, my - mr, 2 * mr, 2 * mr, mr, UI_NEONC);
        ui_fill_round(mx - mr + 6, my - mr - 2, 2 * mr, 2 * mr, mr, UI_BG2);
    }
    ui_present();
}

/* Seek to target_us: reset MVD + decode from the preceding keyframe to the target (no display but
 * the landing frame), reset the audio ring to the matching AAC sample, and rebase the clock. */
static void do_seek(Mp4 *m, uint8_t *vbuf, uint8_t *atmp, int sbs, int64_t dur_us,
                    int64_t target_us, int *vi) {
    if (target_us < 0) target_us = 0;
    if (dur_us > 0 && target_us > dur_us) target_us = dur_us;
    int64_t tdts = (m->v_timescale > 0) ? target_us * (int64_t)m->v_timescale / 1000000LL : 0;
    int kf = mp4_keyframe_before(m, tdts); if (kf < 0) kf = 0;
    int tvi = kf; while (tvi + 1 < m->v_count && v_time_us(m, tvi + 1) <= target_us) tvi++;

    mp4_mvd_reset();
    int got = 0;
    for (int j = kf; j <= tvi; j++) {
        int nn = mp4_read_sample(m, &m->vsamples[j], vbuf);
        got = (nn == (int)m->vsamples[j].size && mp4_mvd_decode(vbuf, nn));
    }
    if (got) { mp4_mvd_present(sbs); moflex_sub_overlay(sbs, target_us); gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank(); }
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
}

/* quick full-screen message (bottom, ui) with B/A to dismiss */
static void mp4_msg(const char *m) {
    gfxSetScreenFormat(GFX_BOTTOM, GSP_RGB565_OES);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & (KEY_A | KEY_B)) break;
        ui_begin(GFX_BOTTOM);
        ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);
        ui_text_center(UI_W / 2, 90, 1, UI_NEON, m);
        ui_text_center(UI_W / 2, 150, 1, UI_DIM, "Press B");
        ui_present();
        gspWaitForVBlank();
    }
}

MoflexResult mp4_play(const char *path) {
    bool isnew = false; APT_CheckNew3DS(&isnew);
    if (!isnew) { mp4_msg("MP4 needs a New 3DS (hardware H.264)."); return MOFLEX_QUIT_BACK; }

    Mp4 m;
    if (!mp4_open(&m, path)) { mp4_msg("Could not open the MP4 (no H.264 track)."); return MOFLEX_QUIT_BACK; }

    /* Side-by-side 3D = a double-wide frame. Detect by ASPECT, not absolute width, so any
     * resolution works: two 16:9 halves = 3.55:1, two 400x240 halves = 3.33:1, while normal 2D
     * films top out around 2.4:1. Threshold 2.7:1 splits them cleanly. */
    int sbs = (m.height > 0 && m.width * 10 > m.height * 27);
    gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);   /* 24-bit like moflex: no banding, and the shared
                                                  * subtitle overlay writes 3-byte pixels */
    gfxSet3D(sbs);
    gfxSetDoubleBuffering(GFX_TOP, true);
    gfxSetScreenFormat(GFX_BOTTOM, GSP_RGB565_OES);
    gfxSetDoubleBuffering(GFX_BOTTOM, false);

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
    /* ANY resolution the hardware can decode is accepted -- the present step aspect-fits each
     * frame (or each SBS half) into the 400x240 screen, so 480p/720p/1080p files just work.
     * Only refuse what MVD itself cannot do (H.264 level 4.x frame limit). */
    if (m.width > MP4_MAX_W || m.height > MP4_MAX_H) {
        char msg[128];
        snprintf(msg, sizeof msg,
                 "Video is %dx%d - beyond the\nhardware limit (%dx%d max).",
                 m.width, m.height, MP4_MAX_W, MP4_MAX_H);
        mp4_msg(msg);
        mp4_close(&m); return MOFLEX_QUIT_BACK;
    }

    if (!mp4_mvd_init(m.width, m.height, m.avcc, m.avcc_len, m.nal_length_size)) {
        mp4_msg("MVD init failed (unsupported H.264 profile/level).");
        mp4_close(&m); return MOFLEX_QUIT_BACK;
    }

    uint32_t maxsz = 1;
    for (int i = 0; i < m.v_count; i++) if (m.vsamples[i].size > maxsz) maxsz = m.vsamples[i].size;
    uint8_t *buf = (uint8_t *)malloc(maxsz + 16);
    if (!buf) { mp4_mvd_exit(); mp4_close(&m); return MOFLEX_ERROR; }

    /* ---- audio ---- */
    uint8_t *atmp = NULL;
    memset(g_wb, 0, sizeof g_wb); memset(g_abuf, 0, sizeof g_abuf); memset(g_slot_ns, 0, sizeof g_slot_ns);
    g_have_audio = 0; g_a_next = 0; g_a_played = 0; g_a_started = 0; g_a_base_us = 0;
    if (m.has_audio && mp4_aac_open(&g_aac, m.asc, m.asc_len, m.a_rate, m.a_channels)) {
        uint32_t amax = 1;
        for (int i = 0; i < m.a_count; i++) if (m.asamples[i].size > amax) amax = m.asamples[i].size;
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

    g_mcu_ok = R_SUCCEEDED(mcuHwcInit());
    g_lcd_ok = R_SUCCEEDED(gspLcdInit()); g_screen_off = 0;   /* bottom-screen-off button */
    g_pvol = moflex_vol_get();
    g_playing = 1;

    /* title = filename without extension */
    char title[128];
    { const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
      snprintf(title, sizeof title, "%s", b);
      size_t L = strlen(title); if (L > 4 && !strcasecmp(title + L - 4, ".mp4")) title[L - 4] = 0; }

    moflex_subs_autoload(path);   /* load a matching .srt (sidecar or moviedata/) if one exists */

    double dur_s = mp4_duration_s(&m);
    int64_t dur_us = (int64_t)(dur_s * 1e6);
    double fps = (dur_s > 0 && m.v_count > 1) ? (m.v_count - 1) / dur_s : 30.0;
    if (fps < 1.0 || fps > 120.0) fps = 30.0;

    int vi = 0;
    int64_t cur_us = 0;

    /* auto-resume (like moflex): jump to the saved position if any */
    int64_t resume_us = (int64_t)moflex_resume_get(path);
    if (g_have_audio) pump_audio(&m, atmp);
    if (resume_us > 3000000 && (dur_us <= 0 || resume_us < dur_us - 5000000))
        do_seek(&m, buf, atmp, sbs, dur_us, resume_us, &vi);
    else
        clk_set(0, 1);   /* no-audio wall clock starts now */

    MoflexResult result = MOFLEX_QUIT_BACK;
    int quit = 0, was_paused = 0, vol_dirty = 0, hrep = 0;
    int scrub = 0; int64_t scrub_us = 0;
    int seek_req = 0; int64_t seek_to = 0;
    int last_sig = -1;

    while (aptMainLoop() && !quit) {
        /* ---- input ---- */
        hidScanInput();
        u32 kd = hidKeysDown(), kh = hidKeysHeld();
        touchPosition tp; hidTouchRead(&tp);

        /* bottom screen off: any input wakes it (and is swallowed so it doesn't hit a control) */
        if (g_screen_off) {
            if (kd || (kh & KEY_TOUCH)) { GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM); g_screen_off = 0; }
            kd = 0; kh &= ~KEY_TOUCH; last_sig = -1;
        }

        if (kd & KEY_B) { result = MOFLEX_QUIT_BACK; break; }
        if (kd & KEY_A) { g_playing = !g_playing; }
        if (kd & KEY_UP)   { int s = (int)(g_pvol / 0.25f + 0.0001f); g_pvol = (s + 1) * 0.25f; if (g_pvol > 4.0f) g_pvol = 4.0f; vol_dirty = 1; }
        if (kd & KEY_DOWN) { int s = (int)(g_pvol / 0.25f + 0.9999f); g_pvol = (s - 1) * 0.25f; if (g_pvol < 0.25f) g_pvol = 0.25f; vol_dirty = 1; }
        /* L/R (hold to repeat) = seek -/+ 10s */
        { int lr = (kh & KEY_LEFT) ? -1 : (kh & KEY_RIGHT) ? 1 : 0;
          if (!lr) hrep = 0;
          else { int fire = (kd & (KEY_LEFT | KEY_RIGHT)) ? 1 : 0;
                 if (!fire) { hrep++; if (hrep > 8 && hrep % 4 == 0) fire = 1; }
                 if (fire) { seek_req = 1; seek_to = cur_us + lr * SEEK_STEP_US; } } }

        /* touch */
        if (kh & KEY_TOUCH) {
            int x = tp.px, y = tp.py;
            if (y >= BAR_Y - 12 && y <= BAR_Y + BAR_H + 12 && x >= BAR_X - 6 && x <= BAR_X + BAR_W + 6) {
                double f = (double)(x - BAR_X) / BAR_W; if (f < 0) f = 0; if (f > 1) f = 1;
                scrub = 1; scrub_us = (int64_t)(f * dur_us);
            } else if (x >= VOL_X - 8 && x <= VOL_X + 20 && y >= VOL_Y && y <= VOL_Y + VOL_H) {
                float nv = 4.0f * (float)(VOL_Y + VOL_H - y) / VOL_H;
                nv = ((int)(nv / 0.25f + 0.5f)) * 0.25f; if (nv < 0.25f) nv = 0.25f; if (nv > 4.0f) nv = 4.0f;
                g_pvol = nv; vol_dirty = 1;
            }
        }
        if (kd & KEY_TOUCH) {   /* one-shot taps */
            int x = tp.px, y = tp.py;
            int dx = x - PLAY_CX, dy = y - PLAY_CY;
            if (dx * dx + dy * dy <= 26 * 26) g_playing = !g_playing;
            else if (y >= PLAY_CY - 16 && y <= PLAY_CY + 16 && x >= RW_CX - 20 && x <= RW_CX + 20) { seek_req = 1; seek_to = cur_us - SEEK_STEP_US; }
            else if (y >= PLAY_CY - 16 && y <= PLAY_CY + 16 && x >= FF_CX - 20 && x <= FF_CX + 20) { seek_req = 1; seek_to = cur_us + SEEK_STEP_US; }
            else if (x >= CC_X && x <= CC_X + CC_W && y >= CC_Y && y <= CC_Y + CC_H) {   /* subtitles menu */
                if (g_have_audio) ndspChnSetPaused(0, true);
                clk_pause();
                moflex_sub_menu(path, sbs);
                if (g_have_audio && g_playing) ndspChnSetPaused(0, false);
                clk_resume();
                last_sig = -1;
            }
            else if (g_lcd_ok && x >= DIM_X && x <= DIM_X + DIM_W && y >= DIM_Y && y <= DIM_Y + DIM_H) {   /* bottom screen off */
                GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTTOM); g_screen_off = 1;
            }
            else if (y >= BTN_Y && y <= BTN_Y + BTN_H) {
                if (x >= BKB_X && x <= BKB_X + BKB_W) { result = MOFLEX_QUIT_BACK; break; }
                if (x >= OPB_X && x <= OPB_X + OPB_W) { result = MOFLEX_QUIT_OPEN; break; }
                if (x >= EXB_X && x <= EXB_X + EXB_W) { result = MOFLEX_QUIT_EXIT; break; }
            }
        }
        if (scrub && !(kh & KEY_TOUCH)) { seek_req = 1; seek_to = scrub_us; scrub = 0; }   /* commit on release */

        if (seek_req) { do_seek(&m, buf, atmp, sbs, dur_us, seek_to, &vi); cur_us = seek_to; seek_req = 0; }

        /* ---- paused: hold, keep panel live, defer SD writes to here ---- */
        if (!g_playing) {
            if (!was_paused) {
                if (g_have_audio) ndspChnSetPaused(0, true);
                clk_pause();
                if (vol_dirty) { moflex_vol_set(g_pvol); vol_dirty = 0; }
                if (cur_us > 3000000 && (dur_us <= 0 || cur_us < dur_us - 5000000)) moflex_resume_save(path, cur_us);
                was_paused = 1;
            }
            int64_t shown = scrub ? scrub_us : cur_us;
            draw_panel(title, shown, dur_us);
            gspWaitForVBlank();
            continue;
        }
        if (was_paused) { if (g_have_audio) ndspChnSetPaused(0, false); clk_resume(); was_paused = 0; }

        /* ---- EOF ---- */
        if (vi >= m.v_count) {   /* finished: clear resume + mark watched (library badges/sort) */
            moflex_resume_clear(path);
            moflex_watched_set(path, 1);
            result = MOFLEX_EOF; break;
        }

        /* ---- decode next video frame ---- */
        Mp4Sample *s = &m.vsamples[vi];
        int n = mp4_read_sample(&m, s, buf);
        int got = (n == (int)s->size && mp4_mvd_decode(buf, n));
        pump_audio(&m, atmp);
        cur_us = v_time_us(&m, vi);

        /* ---- wait until this frame is due (audio-master), staying responsive ---- */
        for (;;) {
            pump_audio(&m, atmp);
            int64_t now = media_now_us();
            int sig = (int)(cur_us / 250000) ^ (g_playing << 20) ^ ((int)(g_pvol * 4) << 22) ^ (g_batt << 24);
            if (sig != last_sig) { draw_panel(title, scrub ? scrub_us : cur_us, dur_us); last_sig = sig; }
            if (now >= cur_us) break;
            if (!aptMainLoop()) { quit = 1; break; }
            gspWaitForVBlank();
            /* poll for pause/seek/back so the wait doesn't feel laggy */
            hidScanInput();
            u32 wkd = hidKeysDown();
            if (wkd & KEY_B) { result = MOFLEX_QUIT_BACK; quit = 1; break; }
            if (wkd & KEY_A) { g_playing = 0; break; }
            if (wkd & KEY_TOUCH) break;   /* re-handle taps at the top */
        }
        if (quit) break;
        if (!g_playing) continue;   /* got paused mid-wait -> re-handle */

        if (got) { mp4_mvd_present(sbs); moflex_sub_overlay(sbs, cur_us); gfxFlushBuffers(); gfxSwapBuffers(); }
        gspWaitForVBlank();
        vi++;
    }

    /* ---- save state on exit ---- */
    if (vol_dirty) moflex_vol_set(g_pvol);
    if (result != MOFLEX_EOF) {
        if (cur_us > 3000000 && (dur_us <= 0 || cur_us < dur_us - 5000000)) moflex_resume_save(path, cur_us);
        else if (dur_us > 0 && cur_us >= dur_us - 5000000) moflex_resume_clear(path);
    }

    if (g_have_audio) {
        ndspChnWaveBufClear(0); ndspChnReset(0);
        for (int i = 0; i < NWB; i++) if (g_abuf[i]) { linearFree(g_abuf[i]); g_abuf[i] = NULL; }
        mp4_aac_close(&g_aac);
    }
    if (g_screen_off) { GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM); g_screen_off = 0; }
    if (g_lcd_ok) { gspLcdExit(); g_lcd_ok = 0; }
    if (g_mcu_ok) { mcuHwcExit(); g_mcu_ok = 0; }
    free(atmp);
    mp4_mvd_exit();
    free(buf);
    mp4_close(&m);
    return result;
}
