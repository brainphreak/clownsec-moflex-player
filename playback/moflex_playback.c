#include "moflex_playback.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "moflex_demux.h"
#include "mobicompat.h"
#include "adpcm_moflex.h"
#include "ui_gfx.h"
#include "y2r_video.h"

extern int    mobi_init(AVCodecContext *);
extern int    mobi_decode(AVCodecContext *, AVFrame *, int *, AVPacket *);
extern void   mobi_flush(AVCodecContext *);
extern int    mobi_close(AVCodecContext *);
extern size_t mobi_ctx_size(void);

#define SCR_W 400
#define SCR_H 240

/* ---- software volume (persisted) ---- */
static float g_vol = 1.0f;
static void vol_load(void) {
    FILE *f = fopen("sdmc:/moflex_player/volume.txt", "rb");
    if (f) { float v; if (fscanf(f, "%f", &v) == 1 && v >= 0.25f && v <= 4.0f) g_vol = v; fclose(f); }
}
static void vol_save(void) {
    mkdir("sdmc:/moflex_player", 0777);
    FILE *f = fopen("sdmc:/moflex_player/volume.txt", "wb");
    if (f) { fprintf(f, "%.2f\n", g_vol); fclose(f); }
}

/* ---- per-movie resume position, persisted to sdmc ---- */
static void resume_path(const char *movie, char *out, size_t cap) {
    const char *base = strrchr(movie, '/'); base = base ? base + 1 : movie;
    char key[160]; size_t j = 0;
    for (const char *p = base; *p && j + 1 < sizeof(key); p++) {
        char c = *p;
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '.';
        key[j++] = ok ? c : '_';
    }
    key[j] = 0;
    snprintf(out, cap, "sdmc:/moflex_player/resume/%s.pos", key);
}
static int64_t resume_load(const char *movie) {
    char p[256]; resume_path(movie, p, sizeof(p));
    FILE *f = fopen(p, "rb"); if (!f) return 0;
    long long us = 0; int ok = (fscanf(f, "%lld", &us) == 1); fclose(f);
    return (ok && us > 0) ? (int64_t)us : 0;
}
static void resume_save_us(const char *movie, int64_t us) {
    mkdir("sdmc:/moflex_player", 0777);
    mkdir("sdmc:/moflex_player/resume", 0777);
    char p[256]; resume_path(movie, p, sizeof(p));
    FILE *f = fopen(p, "wb"); if (f) { fprintf(f, "%lld\n", (long long)us); fclose(f); }
}
static void resume_clear(const char *movie) {
    char p[256]; resume_path(movie, p, sizeof(p));
    remove(p);
}
long long moflex_resume_get(const char *path) { return (long long)resume_load(path); }

/* ---- YUV->RGB565 LUTs ---- */
static int yl[256], rv[256], gu[256], gv[256], bu[256];
static u8  clamp8[1024];
static int luts_ready = 0;
static void init_luts(void) {
    if (luts_ready) return;
    for (int i = 0; i < 256; i++) {
        yl[i] = 298 * (i - 16) + 128;
        rv[i] = 409 * (i - 128); gu[i] = -100 * (i - 128);
        gv[i] = -208 * (i - 128); bu[i] = 516 * (i - 128);
    }
    for (int i = 0; i < 1024; i++) { int v = i - 256; clamp8[i] = (u8)(v < 0 ? 0 : (v > 255 ? 255 : v)); }
    luts_ready = 1;
}
static inline u16 yuv2rgb565(int Y, int U, int V) {
    int y = yl[Y];
    int r = clamp8[((y + rv[V]) >> 8) + 256];
    int g = clamp8[((y + gu[U] + gv[V]) >> 8) + 256];
    int b = clamp8[((y + bu[U]) >> 8) + 256];
    return (u16)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static void blit_eye(AVFrame *out, gfx3dSide_t side, int W, int H) {
    if (y2r_video_blit(out, side, W, H)) return;   /* hardware color conversion */
    /* software fallback (per-pixel YUV->RGB565 + rotate) */
    u16 *fb = (u16 *)gfxGetFramebuffer(GFX_TOP, side, NULL, NULL);
    const uint8_t *Yp = out->data[0], *Up = out->data[1], *Vp = out->data[2];
    int ys = out->linesize[0], cs = out->linesize[1];
    int w = W < SCR_W ? W : SCR_W, h = H < SCR_H ? H : SCR_H;
    for (int x = 0; x < w; x++) {
        u16 *col = fb + x * SCR_H;
        int cx = x >> 1;
        for (int k = 0; k < h; k++) {
            int y = h - 1 - k;
            col[k] = yuv2rgb565(Yp[y * ys + x], Up[(y >> 1) * cs + cx], Vp[(y >> 1) * cs + cx]);
        }
    }
}

/* ---- control panel layout (bottom screen, 320x240) ---- */
#define BAR_X 14
#define BAR_Y 60
#define BAR_W 292
#define BAR_H 10
#define PLAY_CX 160
#define PLAY_CY 118
#define RW_CX 108
#define FF_CX 212
#define VOL_X 298
#define VOL_Y 96
#define VOL_H 108
#define BACK_X 6
#define BACK_Y 220

static void fmt_time(int64_t us, char *o, int cap) {
    if (us < 0) us = 0;
    long t = (long)(us / 1000000);
    int h = t / 3600, m = (t / 60) % 60, s = t % 60;
    if (h > 0) snprintf(o, cap, "%d:%02d:%02d", h, m, s);
    else       snprintf(o, cap, "%d:%02d", m, s);
}

static void panel_draw(const char *title, int64_t cur, int64_t dur, int playing) {
    ui_begin(GFX_BOTTOM);
    ui_clear(UI_BLACK);

    ui_text(6, 6, 1, UI_WHITE, title);

    char tc[16], td[16], line[40];
    fmt_time(cur, tc, sizeof(tc));
    fmt_time(dur, td, sizeof(td));
    snprintf(line, sizeof(line), "%s / %s", tc, td);
    ui_text(6, 28, 1, UI_GRAY, line);

    /* progress bar */
    double frac = dur > 0 ? (double)cur / (double)dur : 0.0;
    if (frac > 1) frac = 1;
    ui_fill(BAR_X, BAR_Y, BAR_W, BAR_H, UI_RGB(60, 60, 60));
    ui_fill(BAR_X, BAR_Y, (int)(BAR_W * frac), BAR_H, UI_WHITE);
    int kx = BAR_X + (int)(BAR_W * frac);
    ui_fill(kx - 2, BAR_Y - 3, 5, BAR_H + 6, UI_RGB(80, 160, 255));

    /* transport: RW (<<)   play/pause   FF (>>) */
    ui_play_l(RW_CX - 5, PLAY_CY, 15, UI_WHITE); ui_play_l(RW_CX + 6, PLAY_CY, 15, UI_WHITE);
    if (playing) ui_pause(PLAY_CX, PLAY_CY, 26, UI_WHITE);
    else         ui_play(PLAY_CX, PLAY_CY, 30, UI_WHITE);
    ui_play(FF_CX - 6, PLAY_CY, 15, UI_WHITE); ui_play(FF_CX + 5, PLAY_CY, 15, UI_WHITE);

    /* volume slider (vertical, right) */
    ui_fill(VOL_X, VOL_Y, 12, VOL_H, UI_RGB(60, 60, 60));
    int vf = (int)(VOL_H * (g_vol / 4.0f));
    ui_fill(VOL_X, VOL_Y + VOL_H - vf, 12, vf, UI_RGB(80, 160, 255));
    char vs[8]; snprintf(vs, sizeof(vs), "%d%%", (int)(g_vol * 100 + 0.5f));
    ui_text(VOL_X - 18, VOL_Y + VOL_H + 4, 1, UI_WHITE, vs);

    /* control hint */
    ui_text_center(UI_W / 2, 225, 1, UI_GRAY, "A play/pause  <> seek  ^v vol  B back");
    ui_present();
}

static void mp_wait_key(void) {
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & (KEY_A | KEY_B)) break;
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    }
}

/* seek to a target time (us); flush decoder so decode restarts at a keyframe */
static void do_seek(MfxDemux *m, AVCodecContext *ctx, int64_t target_us) {
    mfx_seek_time(m, target_us);
    mobi_flush(ctx);
}

/* After a seek the demuxer lands on a block boundary, but the video can't display
 * until the next decodable keyframe. Fast-scan there (video only, no audio, no
 * vblank pacing) and show that landing frame immediately, so seeking is responsive
 * and audio can then resume in sync instead of racing ahead of a frozen frame. */
static void prime_after_seek(MfxDemux *m, AVCodecContext *ctx, AVFrame *out, int W, int H,
                             int64_t *cur_us, int *vidx, int *left_ok, int *left_vidx) {
    MfxPacket pkt;
    for (int guard = 0; guard < 4000; guard++) {           /* bound the scan */
        if (mfx_next_packet(m, &pkt) != 1) return;
        if (m->streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;  /* skip audio */
        *cur_us = m->ts;
        AVPacket ap; ap.data = pkt.data; ap.size = pkt.size; int got = 0;
        int ok = (mobi_decode(ctx, out, &got, &ap) >= 0 && got);
        if ((*vidx & 1) == 0) {                             /* left eye */
            if (ok) { blit_eye(out, GFX_LEFT, W, H); *left_ok = 1; *left_vidx = *vidx; }
            else *left_ok = 0;
        } else {                                            /* right eye: present a matched pair */
            if (ok && *left_ok && *vidx == *left_vidx + 1) {
                blit_eye(out, GFX_RIGHT, W, H);
                gfxFlushBuffers(); gfxSwapBuffers();
                (*vidx)++;
                return;                                     /* landing frame shown */
            }
            *left_ok = 0;
        }
        (*vidx)++;
    }
}

MoflexResult moflex_play(const char *path) {
    init_luts();
    vol_load();

    FILE *f = fopen(path, "rb");
    if (!f) { printf("\x1b[2J\x1b[Hfopen failed\npress A\n"); mp_wait_key(); return MOFLEX_ERROR; }

    MfxDemux m;
    if (mfx_open(&m, f) != 0) { printf("\x1b[2J\x1b[Hmfx_open failed\npress A\n"); mp_wait_key(); fclose(f); return MOFLEX_ERROR; }

    int vi = -1, ai = -1;
    for (int i = 0; i < m.nb_streams; i++) {
        if (m.streams[i].media_type == MFX_TYPE_VIDEO && vi < 0) vi = i;
        if (m.streams[i].media_type == MFX_TYPE_AUDIO && ai < 0) ai = i;
    }
    if (vi < 0) { printf("\x1b[2J\x1b[Hno video\npress A\n"); mp_wait_key(); mfx_close(&m); fclose(f); return MOFLEX_ERROR; }
    int W = m.streams[vi].width, H = m.streams[vi].height;
    int arate = ai >= 0 ? m.streams[ai].sample_rate : 44100;
    int chn   = ai >= 0 ? m.streams[ai].channels    : 2;

    AVCodecContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.width = W; ctx.height = H;
    ctx.priv_data = calloc(1, mobi_ctx_size());
    if (!ctx.priv_data || mobi_init(&ctx) != 0) { free(ctx.priv_data); mfx_close(&m); fclose(f); return MOFLEX_ERROR; }
    AVFrame *out = av_frame_alloc();

    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    gfxSetDoubleBuffering(GFX_TOP, true);   /* MUST be on: both eyes go to the back
        buffer and appear together only on gfxSwapBuffers. Single-buffered here would
        make the left eye update before the right (branding_show() turns this off). */
    gfxSet3D(true);

    y2r_video_init(W, H);   /* hardware YUV->RGB; blit_eye falls back to software if unavailable */

    /* short movie title from filename */
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    char title[64]; snprintf(title, sizeof(title), "%.60s", base);
    { size_t L = strlen(title);                        /* hide extension */
      if (L > 7 && !strcasecmp(title + L - 7, ".moflex")) title[L - 7] = 0;
      else if (L > 4 && !strcasecmp(title + L - 4, ".zip")) title[L - 4] = 0; }

    /* audio */
    #define NWB 8
    #define ABUF_SAMPLES (16 * 1024)
    ndspWaveBuf wbuf[NWB];
    int16_t *abuf[NWB] = { 0 };
    int have_audio = (ai >= 0);
    memset(wbuf, 0, sizeof(wbuf));
    if (have_audio) {
        ndspChnReset(0);
        ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
        ndspChnSetRate(0, (float)arate);
        ndspChnSetFormat(0, chn == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
        float mix[12]; memset(mix, 0, sizeof(mix)); mix[0] = mix[1] = 1.0f;
        ndspChnSetMix(0, mix);
        for (int i = 0; i < NWB; i++) {
            abuf[i] = (int16_t *)linearAlloc(ABUF_SAMPLES * chn * sizeof(int16_t));
            wbuf[i].data_vaddr = abuf[i]; wbuf[i].status = NDSP_WBUF_DONE;
        }
    }

    MoflexResult result = MOFLEX_QUIT_BACK;
    int wb_idx = 0;
    /* 3D pairing: frames alternate L(even),R(odd) by absolute video index.
       Present a stereo frame ONLY when a consecutive L+R both decode, so the
       two eyes are always the same moment (never a mismatched pair). */
    int vidx = 0, left_ok = 0, left_vidx = -2;
    int playing = 1, dirty = 1, since_panel = 999;
    int64_t cur_us = 0, dur_us = m.duration_us;
    int64_t seek_to_us = 0; int want_seek = 0, shold = 0;
    MfxPacket pkt;

    /* auto-resume where we left off (unless within 10s of the end) */
    int64_t rpos = resume_load(path), last_save = 0;
    if (rpos > 3000000 && (dur_us <= 0 || rpos < dur_us - 10000000)) {
        seek_to_us = rpos; want_seek = 1; last_save = rpos;
    }

    while (aptMainLoop()) {
        hidScanInput();
        u32 kd = hidKeysDown(), kh = hidKeysHeld();
        touchPosition tp; hidTouchRead(&tp);

        /* ---- controls (direct, nothing to select): A play/pause,
               Left/Right (hold) rewind/FF, Up/Down volume, B back ---- */
        if (kd & KEY_B) { result = MOFLEX_QUIT_BACK; break; }
        if (kd & KEY_A) { playing = !playing; if (have_audio) ndspChnSetPaused(0, !playing); dirty = 1; }
        if (kd & KEY_UP)   { if (g_vol < 4.0f)  g_vol += 0.25f; vol_save(); dirty = 1; }
        if (kd & KEY_DOWN) { if (g_vol > 0.25f) g_vol -= 0.25f; vol_save(); dirty = 1; }
        {   /* Left/Right seek, hold to keep scrubbing (~30s steps) */
            int sdir = (kh & KEY_RIGHT) ? 1 : ((kh & KEY_LEFT) ? -1 : 0);
            if (sdir == 0) shold = 0;
            else {
                int fire = (kd & (KEY_RIGHT | KEY_LEFT)) ? 1 : 0;   /* initial press */
                if (!fire) { shold++; if (shold > 14 && (shold % 6 == 0)) fire = 1; }  /* repeat while held */
                if (fire) { seek_to_us = cur_us + (int64_t)sdir * 30000000; want_seek = 1; }
            }
        }

        /* ---- touch (always active) ---- */
        if ((kd | kh) & KEY_TOUCH) {
            int px = tp.px, py = tp.py;
            if (py >= BAR_Y - 8 && py <= BAR_Y + BAR_H + 8 && px >= BAR_X && px <= BAR_X + BAR_W) {
                if (kd & KEY_TOUCH) { seek_to_us = (int64_t)((double)(px - BAR_X) / BAR_W * dur_us); want_seek = 1; }
            } else if (px >= VOL_X - 6 && px <= VOL_X + 18 && py >= VOL_Y && py <= VOL_Y + VOL_H) {
                float nv = (float)(VOL_Y + VOL_H - py) / VOL_H * 4.0f;
                if (nv < 0.25f) nv = 0.25f; if (nv > 4.0f) nv = 4.0f;
                g_vol = nv; vol_save(); dirty = 1;
            } else if (kd & KEY_TOUCH) {
                if (py >= PLAY_CY - 20 && py <= PLAY_CY + 20) {
                    if (px >= PLAY_CX - 20 && px <= PLAY_CX + 20) { playing = !playing; if (have_audio) ndspChnSetPaused(0, !playing); dirty = 1; }
                    else if (px >= RW_CX - 18 && px <= RW_CX + 18) { seek_to_us = cur_us - 30000000; want_seek = 1; }
                    else if (px >= FF_CX - 18 && px <= FF_CX + 18) { seek_to_us = cur_us + 30000000; want_seek = 1; }
                } else if (py >= BACK_Y - 4 && px <= 180) { result = MOFLEX_QUIT_BACK; break; }
            }
        }

        /* ---- apply a requested seek (resume playback, flush stale audio) ---- */
        if (want_seek) {
            do_seek(&m, &ctx, seek_to_us);
            vidx = 0; left_ok = 0;          /* re-establish L/R pairing after seek */
            playing = 1;
            if (have_audio) {
                ndspChnWaveBufClear(0);
                ndspChnSetPaused(0, false);
                for (int i = 0; i < NWB; i++) wbuf[i].status = NDSP_WBUF_DONE;
                wb_idx = 0;
            }
            /* show the landing frame right away; audio then resumes synced to it */
            prime_after_seek(&m, &ctx, out, W, H, &cur_us, &vidx, &left_ok, &left_vidx);
            want_seek = 0; dirty = 1;
        }

        /* ---- decode one packet (if playing) ---- */
        if (playing) {
            int rc = mfx_next_packet(&m, &pkt);
            if (rc != 1) { playing = 0; dirty = 1; if (have_audio) ndspChnSetPaused(0, true); }
            else {
                cur_us = m.ts;
                if (cur_us < 0) cur_us = 0;
                if (dur_us > 0 && cur_us > dur_us) cur_us = dur_us;
                if (cur_us - last_save > 15000000 || cur_us < last_save) {  /* checkpoint ~every 15s */
                    resume_save_us(path, cur_us); last_save = cur_us;
                }
                int mt = m.streams[pkt.stream_index].media_type;
                if (mt == MFX_TYPE_AUDIO && have_audio) {
                    ndspWaveBuf *wb = &wbuf[wb_idx];
                    int spins = 0;
                    /* wait for a free audio buffer WITHOUT scanning input here --
                       scanning would eat the button-press edge the main loop needs. */
                    while (wb->status != NDSP_WBUF_FREE && wb->status != NDSP_WBUF_DONE) {
                        gspWaitForVBlank();
                        if (!aptMainLoop()) { result = MOFLEX_QUIT_EXIT; goto done; }
                        if (++spins > 150) break;
                    }
                    int fr = adpcm_moflex_decode(pkt.data, pkt.size, chn, abuf[wb_idx]);
                    if (fr > 0 && fr <= ABUF_SAMPLES) {
                        if (g_vol != 1.0f) {
                            int16_t *s = abuf[wb_idx]; int ns = fr * chn;
                            for (int i = 0; i < ns; i++) { int v = (int)(s[i] * g_vol);
                                s[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }
                        }
                        DSP_FlushDataCache(abuf[wb_idx], fr * chn * sizeof(int16_t));
                        memset(wb, 0, sizeof(*wb));
                        wb->data_vaddr = abuf[wb_idx]; wb->nsamples = fr;
                        ndspChnWaveBufAdd(0, wb);
                        wb_idx = (wb_idx + 1) % NWB;
                    }
                } else if (mt == MFX_TYPE_VIDEO) {
                    AVPacket ap; ap.data = pkt.data; ap.size = pkt.size;
                    int got = 0;
                    int ok = (mobi_decode(&ctx, out, &got, &ap) >= 0 && got);  /* decode every frame (keeps ref chain) */
                    if ((vidx & 1) == 0) {                /* left eye */
                        if (ok) { blit_eye(out, GFX_LEFT, W, H); left_ok = 1; left_vidx = vidx; }
                        else left_ok = 0;
                    } else {                              /* right eye: present only a matched pair */
                        if (ok && left_ok && vidx == left_vidx + 1) {
                            blit_eye(out, GFX_RIGHT, W, H);
                            gfxFlushBuffers(); gfxSwapBuffers();
                        }
                        left_ok = 0;
                    }
                    vidx++;                               /* always advance -> parity tracks the stream */
                }
            }
        } else {
            gspWaitForVBlank();
        }

        /* ---- panel redraw (throttled to spare CPU on old 3DS) ---- */
        if (dirty || ++since_panel > 20) {
            panel_draw(title, cur_us, dur_us, playing);
            since_panel = 0; dirty = 0;
        }
    }
    if (!aptMainLoop() && result == MOFLEX_QUIT_BACK) result = MOFLEX_QUIT_EXIT;

done:
    /* remember where we stopped (clear it if we watched to the end) */
    if (dur_us > 0 && cur_us >= dur_us - 10000000) resume_clear(path);
    else if (cur_us > 3000000) resume_save_us(path, cur_us);
    if (have_audio) { ndspChnWaveBufClear(0); ndspChnSetPaused(0, false);
        for (int i = 0; i < NWB; i++) if (abuf[i]) linearFree(abuf[i]); }
    y2r_video_exit();
    av_frame_free(&out);
    mobi_close(&ctx);
    free(ctx.priv_data);
    mfx_close(&m);
    fclose(f);
    return result;
}
