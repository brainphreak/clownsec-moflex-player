#include "moflex_playback.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <dirent.h>

#include "moflex_demux.h"
#include "cia_moflex.h"
#include "mobicompat.h"
#include "adpcm_moflex.h"
#include "ui_gfx.h"
#include "y2r_video.h"
#include <citro2d.h>   /* Old-3DS GPU playback path (no CPU transpose) */

extern int    mobi_init(AVCodecContext *);
extern int    mobi_decode(AVCodecContext *, AVFrame *, int *, AVPacket *);
extern void   mobi_recon_par_exit(void);   /* join the New-3DS decode-pipeline worker thread */
extern void   mobi_flush(AVCodecContext *);
extern int    mobi_close(AVCodecContext *);
extern int    mobi_opt;   /* decode-path selector; 14 = prefetch|idct-skip|dc (bit-exact, ~6-8% faster) */
extern size_t mobi_ctx_size(void);

#define SCR_W 400
#define SCR_H 240

/* ---- software volume (persisted) ---- */
static float g_vol = 1.0f;
static int   g_old3d_warn = 0;   /* Old 3DS + 3D content: show the "unsupported / perf issues" notice */
static int   g_sw_bank = -1;     /* software-banking depth (pairs banked ahead); -1 = not the ring engine */
#define SR_NB 16                 /* software banking ring depth (defined early so panel_draw can show it) */
static int   g_lcd_ok = 0;       /* gsp::Lcd available -> bottom-screen-off feature usable */
static int   g_screen_off = 0;   /* bottom backlight is currently off (movie keeps playing on top) */
static int   g_touch_locked = 0; /* touch lock: L toggles it; buttons still work; touch shows a toast */
static int   g_lock_toast = 0;   /* frames left to show the "touch locked" toast after a locked touch */
static int   g_backlight_on = 1; /* actual bottom backlight state (reconciled from g_screen_off + toast) */

/* ---- battery indicator (player panel) ---- */
static int   g_mcu_ok = 0;       /* mcu::HWC available -> exact 0-100% */
static int   g_batt_pct = -1;    /* cached level (-1 = unknown) */
static int   g_batt_chg = 0;     /* charging / plugged in */
static u64   g_batt_next = 0;    /* next refresh time (osGetTime ms) */
static void batt_refresh(void) {
    u64 now = osGetTime();
    if (g_batt_pct >= 0 && now < g_batt_next) return;   /* throttle: read every few seconds */
    g_batt_next = now + 4000;
    u8 v = 0;
    if (g_mcu_ok && R_SUCCEEDED(MCUHWC_GetBatteryLevel(&v))) g_batt_pct = v > 100 ? 100 : v;
    else { u8 lvl = 0; if (R_SUCCEEDED(PTMU_GetBatteryLevel(&lvl))) g_batt_pct = lvl * 20; }   /* 0-5 -> % */
    u8 c = 0; if (R_SUCCEEDED(PTMU_GetBatteryChargeState(&c))) g_batt_chg = c;
}
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
    char suf[24]; cia_selection_suffix(movie, suf, sizeof suf);   /* per-embedded-video for multi-moflex CIAs */
    snprintf(out, cap, "sdmc:/moflex_player/resume/%s%s.pos", key, suf);
}
static int64_t resume_load(const char *movie) {
    char p[256]; resume_path(movie, p, sizeof(p));
    FILE *f = fopen(p, "rb"); if (!f) return 0;
    long long us = 0; int ok = (fscanf(f, "%lld", &us) == 1); fclose(f);
    return (ok && us > 0) ? (int64_t)us : 0;
}
static void resume_save_us(const char *movie, int64_t us) {
    static int made = 0;   /* create the dir once, not on every checkpoint (each mkdir hits the SD/FS) */
    if (!made) { mkdir("sdmc:/moflex_player", 0777); mkdir("sdmc:/moflex_player/resume", 0777); made = 1; }
    char p[256]; resume_path(movie, p, sizeof(p));
    FILE *f = fopen(p, "wb"); if (f) { fprintf(f, "%lld\n", (long long)us); fclose(f); }
}
static void resume_clear(const char *movie) {
    char p[256]; resume_path(movie, p, sizeof(p));
    remove(p);
}

/* ---- per-movie subtitle encoding, persisted next to the resume file ---- */
static void subenc_path(const char *movie, char *out, size_t cap) {
    resume_path(movie, out, cap);                       /* .../<key>.pos */
    size_t L = strlen(out);
    if (L >= 4) snprintf(out + L - 4, cap - (L - 4), ".enc");   /* -> .../<key>.enc */
}
static int subenc_load(const char *movie) {             /* 0..4, or -1 if none saved */
    char p[256]; subenc_path(movie, p, sizeof p);
    FILE *f = fopen(p, "rb"); if (!f) return -1;
    int e = -1; if (fscanf(f, "%d", &e) != 1) e = -1; fclose(f);
    return (e >= 0 && e < 5) ? e : -1;
}
/* (subenc_save is gone -- the encoding is now written with the rest of the settings by subcfg_save;
 *  subenc_load stays as the read-side fallback for .enc files written by older builds.) */
long long moflex_resume_get(const char *path) { return (long long)resume_load(path); }
void moflex_resume_clear(const char *path) { resume_clear(path); }

/* ---- YUV->RGB565 LUTs ---- */
static int yl[256], rv[256], gu[256], gv[256], bu[256];
static u8  clamp8[1024];
static int luts_ready = 0;
static void init_luts(void) {
    if (luts_ready) return;
    for (int i = 0; i < 256; i++) {
        /* TV-RANGE BT.601 (16..235 -> 0..255). The content IS TV-range -- verified by histogramming the
 * decoded luma with the PADDING BARS EXCLUDED:
 *     Cash Machine   content 1..246   below 16: 0.10%   above 235: 0.79%
 *     Freaks & Geeks content 13..232  below 16: 0.00%   above 235: 0.00%
 * A brief v1.1/v1.2 experiment converted it as FULL-range on the strength of "38% of pixels are
 * below 16" -- but that 38% was 38.28% at EXACTLY Y=0: the pillarbox padding, which encoders write
 * as pure black regardless of range. It says nothing about the picture. Treating TV-range video as
 * full-range lifts the blacks and drops ~14% of the chroma gain, which is the washed-out, low-
 * contrast look users reported. The real bug was only ever RGB565 banding (fixed separately by the
 * 24-bit top screen). */
        yl[i] = 298 * (i - 16) + 128;
        rv[i] = 409 * (i - 128); gu[i] = -100 * (i - 128);
        gv[i] = -208 * (i - 128); bu[i] = 516 * (i - 128);
    }
    for (int i = 0; i < 1024; i++) { int v = i - 256; clamp8[i] = (u8)(v < 0 ? 0 : (v > 255 ? 255 : v)); }
    luts_ready = 1;
}
/* 24-BIT OUTPUT. The top screen used to be RGB565, which gives BLUE ONLY 5 BITS -- 32 levels --
 * so any smooth gradient could only ever land in ~32 hard steps. That is the banding users see
 * against the official player, and no colour-space fix can remove it: it is quantisation, not
 * mapping. (The full-range fix in v1.1 removed the 1.164x stretch that was AMPLIFYING it.)
 * The 3DS top screen takes GSP_BGR8_OES: 3 bytes per pixel, stored B,G,R -- the same byte order
 * Y2R's OUTPUT_RGB_24 produces, so the hardware path needs no swizzle. 8 bits per channel = 256
 * blue levels instead of 32. Costs 50% more framebuffer bandwidth on the blit. */
static inline void yuv2bgr8(int Y, int U, int V, u8 *px) {
    int y = yl[Y];
    px[0] = clamp8[((y + bu[U]) >> 8) + 256];               /* B */
    px[1] = clamp8[((y + gu[U] + gv[V]) >> 8) + 256];       /* G */
    px[2] = clamp8[((y + rv[V]) >> 8) + 256];               /* R */
}
/* Convert+rotate one eye into a specific framebuffer (used by the blit worker). */
static void blit_to(AVFrame *out, u8 *fb, int W, int H) {
    if (y2r_video_blit_fb(out, fb, W, H)) return;   /* hardware color conversion */
    /* software fallback (per-pixel YUV->BGR8 + rotate) */
    const uint8_t *Yp = out->data[0], *Up = out->data[1], *Vp = out->data[2];
    int ys = out->linesize[0], cs = out->linesize[1];
    int w = W < SCR_W ? W : SCR_W, h = H < SCR_H ? H : SCR_H;
    for (int x = 0; x < w; x++) {
        u8 *col = fb + (size_t)x * SCR_H * 3;
        int cx = x >> 1;
        for (int k = 0; k < h; k++) {
            int y = h - 1 - k;
            yuv2bgr8(Yp[y * ys + x], Up[(y >> 1) * cs + cx], Vp[(y >> 1) * cs + cx], col + k * 3);
        }
    }
}

static void blit_eye(AVFrame *out, gfx3dSide_t side, int W, int H) {
    blit_to(out, (u8 *)gfxGetFramebuffer(GFX_TOP, side, NULL, NULL), W, H);
}

/* ---- blit worker: runs the Y2R+transpose for both eyes on the 2nd CPU core so it
 * overlaps the (slow) MobiClip decode on the main core -> ~doubles Old-3DS throughput. */
typedef struct { AVFrame *l, *r; u8 *fbl, *fbr; int w, h; } BlitJob;
static volatile int bw_run = 0;
static LightEvent   bw_start, bw_done;
static BlitJob      bw_job;
static Thread       bw_thread = NULL;
static volatile u64 bw_ticks = 0;   /* last pair's blit time */

static void bw_fn(void *arg) {
    (void)arg;
    while (bw_run) {
        LightEvent_Wait(&bw_start);
        if (!bw_run) break;
        u64 t = svcGetSystemTick();
        blit_to(bw_job.l, bw_job.fbl, bw_job.w, bw_job.h);
        blit_to(bw_job.r, bw_job.fbr, bw_job.w, bw_job.h);
        bw_ticks = svcGetSystemTick() - t;
        LightEvent_Signal(&bw_done);
    }
}

static int bw_init(void) {
    bw_run = 1;
    LightEvent_Init(&bw_start, RESET_ONESHOT);
    LightEvent_Init(&bw_done, RESET_ONESHOT);
    bool isnew = false; APT_CheckNew3DS(&isnew);
    int core = isnew ? 2 : 1;                 /* New3DS: core2; Old3DS: syscore (core1) */
    if (!isnew) APT_SetAppCpuTimeLimit(80);   /* let our thread run on the syscore */
    s32 prio = 0x30; svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    bw_thread = threadCreate(bw_fn, NULL, 32 * 1024, prio - 1, core, true);
    if (!bw_thread) { bw_run = 0; return 0; }
    return 1;
}

static void bw_exit(void) {
    if (!bw_thread) return;
    bw_run = 0;
    LightEvent_Signal(&bw_start);
    threadJoin(bw_thread, UINT64_MAX);
    threadFree(bw_thread);
    bw_thread = NULL;
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
#define VOL_Y 88
#define VOL_H 104
#define BACK_X 6
#define BACK_Y 220
/* bottom action buttons: BACK (home) / OPEN (new file) / EXIT (quit app) */
#define BTN_Y 208
#define BTN_H 28
#define BKB_X 8
#define BKB_W 96
#define OPB_X 112
#define OPB_W 96
#define EXB_X 216
#define EXB_W 96
/* CC (subtitles) toggle button -- between the FF control and the volume slider */
#define CC_X 244
#define CC_Y 104
#define CC_W 48
#define CC_H 28
/* DIM (bottom-screen-off) button -- below CC */
#define DIM_X 244
#define DIM_Y 138
#define DIM_W 48
#define DIM_H 28

static void fmt_time(int64_t us, char *o, int cap) {
    if (us < 0) us = 0;
    long t = (long)(us / 1000000);
    int h = t / 3600, m = (t / 60) % 60, s = t % 60;
    if (h > 0) snprintf(o, cap, "%d:%02d:%02d", h, m, s);
    else       snprintf(o, cap, "%d:%02d", m, s);
}

/* ---------- subtitles: external .srt (sidecar next to the movie, or in moviedata/) ---------- */
extern char font8x8_basic[128][8];   /* defined once in ui_gfx.c */
#include "font8x8_ext.h"             /* IBM-VGA-style Latin-1 + Greek + Turkish (matches font8x8_basic) */
#include "font512.h"                 /* fallback for Cyrillic/Hebrew/other foreign scripts */
#include "subcp.h"                   /* 8-bit codepage -> Unicode maps (Turkish/Cyrillic/Greek/...) */

/* decode one UTF-8 codepoint from *ps and advance past it */
static uint32_t u8_next(const char **ps) {
    const unsigned char *s = (const unsigned char *)*ps; uint32_t cp; int n;
    if (s[0] < 0x80) { cp = s[0]; n = 1; }
    else if ((s[0] & 0xE0) == 0xC0 && s[1]) { cp = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F); n = 2; }
    else if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2]) { cp = ((uint32_t)(s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); n = 3; }
    else if ((s[0] & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) { cp = ((uint32_t)(s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F); n = 4; }
    else { cp = '?'; n = 1; }
    *ps += n; return cp;
}
static int u8_cplen(const char *s) { int n = 0; while (*s) { u8_next(&s); n++; } return n; }

static const char sub_note_glyph[8] = { 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E, 0x0F, 0x06 };  /* ♪ */

/* 8x8 bitmap for a Unicode codepoint; NULL if we have no glyph (e.g. CJK). */
static const char *sub_glyph(uint32_t cp) {
    if (cp == 0x266A || cp == 0x266B) return sub_note_glyph;             /* ♪ ♫ */
    if (cp < 0x80)                    return font8x8_basic[cp];          /* ASCII (IBM VGA) */
    if (cp >= 0xA0 && cp <= 0xFF)     return font8x8_ext_latin[cp - 0xA0]; /* Latin-1 (matching style) */
    if (cp >= 0x390 && cp <= 0x3C9)   return font8x8_greek[cp - 0x390];  /* Greek (matching style) */
    switch (cp) {                                                        /* Turkish Ext-A (composed to match) */
        case 0x011E: return font8x8_turk[0]; case 0x011F: return font8x8_turk[1];   /* Ğ ğ */
        case 0x0130: return font8x8_turk[2]; case 0x0131: return font8x8_turk[3];   /* İ ı */
        case 0x015E: return font8x8_turk[4]; case 0x015F: return font8x8_turk[5];   /* Ş ş */
    }
    int lo = 0, hi = FONT512_MAP_N - 1;                                 /* Cyrillic/Hebrew/other: 512_8 fallback */
    while (lo <= hi) {
        int mid = (lo + hi) >> 1; unsigned c = font512_map[mid].cp;
        if (c == cp) return (const char *)font512[font512_map[mid].idx];
        if (c < cp)  lo = mid + 1; else hi = mid - 1;
    }
    return NULL;                                                         /* CJK/etc: no glyph */
}

#define SUB_MAX 4000
#define SUB_TXT 256   /* bytes: UTF-8 cues run longer than their glyph count */
typedef struct { int64_t s, e; char t[SUB_TXT]; } SubCue;
static SubCue *g_subs = NULL;
static int  g_nsubs = 0;
static int  g_sub_on = 0;      /* enabled */
static int  g_sub_top = 0;     /* 0 = bottom of the top screen, 1 = top */
static int  g_sub_depth = 0;   /* 3D parallax: per-eye horizontal shift (-=out toward you, +=into screen) */
static int  g_sub_size = 1;    /* font scale: 1 = small (sharpest), 2 = medium, 3 = large */
static int64_t g_sub_off = 0;  /* timing offset in us (+ = show later, - = show earlier) */
/* Encoding for 8-bit (non-UTF-8) SRTs -- UTF-8 is auto-detected; the various legacy codepages
 * can't be told apart from the bytes, so the user picks. Index into sub_cp_hi[] (see subcp.h). */
static int  g_sub_enc = 0;     /* 0=Western 1=Turkish 2=Cyrillic 3=Greek 4=Central-Euro */
static int  g_sub_mode = -1;   /* resolved for the loaded file: -1 = UTF-8, else 0..4 codepage */
static const char *g_sub_enc_name[5] = { "Western", "Turkish", "Cyrillic", "Greek", "Central Eur" };
static char g_sub_file[512] = "";

static int sub_parse_ts(const char *p, int64_t *us) {
    int h, m, s, ms;
    if (sscanf(p, "%d:%d:%d,%d", &h, &m, &s, &ms) == 4 ||
        sscanf(p, "%d:%d:%d.%d", &h, &m, &s, &ms) == 4) {
        *us = ((int64_t)h * 3600 + m * 60 + s) * 1000000LL + (int64_t)ms * 1000; return 1;
    }
    return 0;
}
/* a stray high byte (non-UTF-8 SRT) -> Unicode, treating it as Windows-1252 / Latin-1 */
static uint32_t cp1252(unsigned char c) {
    switch (c) {
        case 0x91: case 0x92: return 0x2019;   /* curly ' */
        case 0x93: case 0x94: return 0x201C;   /* curly " */
        case 0x96: case 0x97: return 0x2013;   /* en/em dash */
        case 0x85: return 0x2026;              /* ellipsis */
        default: break;
    }
    return (c >= 0xA0) ? c : '?';              /* 0xA0-0xFF map 1:1 to Latin-1 Unicode */
}
/* strip HTML-ish tags; normalize smart punctuation to ASCII; KEEP renderable UTF-8 (Latin-1,
 * Greek, ♪) so foreign languages display; anything we have no glyph for becomes '?'. Accepts
 * both UTF-8 and CP1252/Latin-1 single-byte SRTs, and re-emits everything as UTF-8. */
static void sub_clean(const char *in, char *out, int cap) {
    const unsigned char *p = (const unsigned char *)in; int o = 0;
    while (*p && o < cap - 6) {
        unsigned char c = *p;
        if (c == '<') { p++; while (*p && *p != '>') p++; if (*p) p++; continue; }   /* <i>, <font ...> */
        if (c < 0x80) { out[o++] = (char)c; p++; continue; }
        uint32_t cp; int n;
        if (g_sub_mode >= 0) { cp = sub_cp_hi[g_sub_mode][c - 0x80]; n = 1; }   /* 8-bit codepage */
        else if ((c & 0xE0) == 0xC0 && p[1]) { cp = ((uint32_t)(c & 0x1F) << 6) | (p[1] & 0x3F); n = 2; }   /* UTF-8 */
        else if ((c & 0xF0) == 0xE0 && p[1] && p[2]) { cp = ((uint32_t)(c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); n = 3; }
        else if ((c & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) { cp = ((uint32_t)(c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); n = 4; }
        else { cp = cp1252(c); n = 1; }   /* stray high byte in an otherwise-UTF-8 file */
        if      (cp == 0x2018 || cp == 0x2019 || cp == 0x201B) out[o++] = '\'';
        else if (cp == 0x201C || cp == 0x201D)                 out[o++] = '"';
        else if (cp == 0x2013 || cp == 0x2014)                 out[o++] = '-';
        else if (cp == 0x00A0)                                 out[o++] = ' ';   /* nbsp */
        else if (cp == 0x2026) { if (o < cap - 3) { out[o++]='.'; out[o++]='.'; out[o++]='.'; } }
        else if (sub_glyph(cp)) {                              /* renderable -> emit as UTF-8 */
            if (cp < 0x80) out[o++] = (char)cp;
            else if (cp < 0x800) { out[o++] = (char)(0xC0 | (cp >> 6)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
            else { out[o++] = (char)(0xE0 | (cp >> 12)); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
        }
        else out[o++] = '?';                                  /* no glyph for this codepoint */
        p += n;
    }
    out[o] = 0;
}
/* Is the whole file valid UTF-8? (unambiguous, so we can auto-detect it). Rewinds the file. */
static int sub_is_utf8(FILE *f) {
    rewind(f);
    int c, cont = 0;
    while ((c = fgetc(f)) != EOF) {
        unsigned char b = (unsigned char)c;
        if (cont) { if ((b & 0xC0) != 0x80) { rewind(f); return 0; } cont--; }
        else if (b < 0x80) continue;
        else if ((b & 0xE0) == 0xC0) cont = 1;
        else if ((b & 0xF0) == 0xE0) cont = 2;
        else if ((b & 0xF8) == 0xF0) cont = 3;
        else { rewind(f); return 0; }
    }
    rewind(f);
    return cont == 0;
}
static int subs_load(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    if (!g_subs) g_subs = (SubCue *)malloc(sizeof(SubCue) * SUB_MAX);
    if (!g_subs) { fclose(f); return 0; }
    /* Auto-detect UTF-8; otherwise decode as the user-selected 8-bit codepage. */
    g_sub_mode = sub_is_utf8(f) ? -1 : g_sub_enc;
    g_nsubs = 0;
    char line[512];
    while (g_nsubs < SUB_MAX && fgets(line, sizeof line, f)) {
        if (!strstr(line, "-->")) continue;                       /* the timing line of a cue */
        char sb[40], eb[40]; int64_t s, e;
        if (sscanf(line, "%39s --> %39s", sb, eb) != 2) continue;
        if (!sub_parse_ts(sb, &s) || !sub_parse_ts(eb, &e)) continue;
        char txt[SUB_TXT]; txt[0] = 0;
        while (fgets(line, sizeof line, f)) {                     /* text lines until a blank line */
            char *nl = strpbrk(line, "\r\n"); if (nl) *nl = 0;
            if (!line[0]) break;
            char clean[256]; sub_clean(line, clean, sizeof clean);   /* tags + smart quotes/accents -> ASCII */
            size_t tl = strlen(txt);
            if (tl && tl + 1 < SUB_TXT) txt[tl++] = '\n';
            snprintf(txt + tl, SUB_TXT - tl, "%s", clean);
        }
        SubCue *c = &g_subs[g_nsubs++]; c->s = s; c->e = e; snprintf(c->t, sizeof c->t, "%s", txt);
    }
    fclose(f);
    snprintf(g_sub_file, sizeof g_sub_file, "%s", path);
    return g_nsubs;
}
static const char *subs_active(int64_t us) {
    if (!g_sub_on) return NULL;
    int64_t u = us - g_sub_off;   /* +offset delays the cue window (subs appear later) */
    for (int i = 0; i < g_nsubs; i++) if (u >= g_subs[i].s && u < g_subs[i].e) return g_subs[i].t;
    return NULL;
}
/* ---- per-movie subtitle settings ----------------------------------------------------------
 * Everything the SUBTITLES menu exposes -- on/off, position, size, delay, 3D depth, encoding and
 * the chosen track -- is stored per movie in .../resume/<key>.sub, so reopening a film restores
 * exactly what the viewer set last time. (Previously only the encoding persisted, in <key>.enc;
 * that file is still read as a fallback so existing choices survive the upgrade.) Written once
 * when the menu closes -- never mid-playback, because an SD write stalls the FS service. */
static void subcfg_path(const char *movie, char *out, size_t cap) {
    resume_path(movie, out, cap);                                   /* .../<key>.pos */
    size_t L = strlen(out);
    if (L >= 4) snprintf(out + L - 4, cap - (L - 4), ".sub");       /* -> .../<key>.sub */
}
static void subcfg_save(const char *movie) {
    mkdir("sdmc:/moflex_player", 0777); mkdir("sdmc:/moflex_player/resume", 0777);
    char p[256]; subcfg_path(movie, p, sizeof p);
    FILE *f = fopen(p, "wb"); if (!f) return;
    fprintf(f, "1 %d %d %d %d %lld %d\n%s\n", g_sub_on, g_sub_top, g_sub_size, g_sub_depth,
            (long long)g_sub_off, g_sub_enc, g_sub_file);
    fclose(f);
}
static void subcfg_load(const char *movie) {
    char p[256]; subcfg_path(movie, p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) { int e = subenc_load(movie); if (e >= 0) g_sub_enc = e; return; }   /* pre-.sub: encoding only */
    int ver = 0, on = 0, top = 0, size = 1, depth = 0, enc = 0; long long off = 0;
    if (fscanf(f, "%d %d %d %d %d %lld %d", &ver, &on, &top, &size, &depth, &off, &enc) == 7 && ver == 1) {
        g_sub_on    = !!on;
        g_sub_top   = !!top;
        g_sub_size  = (size  >= 1 && size <= 3) ? size : 1;
        g_sub_depth = depth < -16 ? -16 : (depth > 16 ? 16 : depth);
        g_sub_off   = (int64_t)off;
        g_sub_enc   = (enc >= 0 && enc < 5) ? enc : 0;
        char line[512];
        if (fgets(line, sizeof line, f)) {                  /* consume the tail of the numbers line */
            if (fgets(line, sizeof line, f)) {              /* the track path is on its own line */
                size_t L = strlen(line);
                while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
                if (L) snprintf(g_sub_file, sizeof g_sub_file, "%s", line);
            }
        }
    }
    fclose(f);
}

/* auto-load a matching track at playback start: "<movie>.srt" beside the file, else moviedata/ */
static void subs_autoload(const char *moviepath) {
    g_nsubs = 0; g_sub_on = 0; g_sub_off = 0; g_sub_file[0] = 0;   /* offset is per-movie */
    subcfg_load(moviepath);   /* this movie's saved settings (else keep last-used) */
    if (g_sub_file[0]) {      /* the exact track they were watching with -- keeps their on/off choice */
        char want[512]; snprintf(want, sizeof want, "%s", g_sub_file);   /* subs_load() rewrites g_sub_file */
        if (subs_load(want)) return;
        g_sub_file[0] = 0;    /* the saved track is gone -> fall through to the sidecar search */
    }
    const char *b = strrchr(moviepath, '/'); b = b ? b + 1 : moviepath;
    char stem[256]; snprintf(stem, sizeof stem, "%s", b);
    char *dot = strrchr(stem, '.'); if (dot) *dot = 0;
    char cand[600];
    snprintf(cand, sizeof cand, "%.*s%s.srt", (int)(b - moviepath), moviepath, stem);   /* sidecar */
    if (subs_load(cand)) { g_sub_on = 1; return; }
    snprintf(cand, sizeof cand, "sdmc:/moflex_player/moviedata/%s.srt", stem);           /* moviedata */
    if (subs_load(cand)) { g_sub_on = 1; return; }
}

/* draw the active cue straight onto the (rotated) TOP framebuffer, both eyes in 3D.
 * The top screen is now 24-bit BGR8, so subtitle colours (RGB565 constants shared with the UI)
 * are expanded to 8 bits per channel and packed B | G<<8 | R<<16 to match sub_fbpx. */
static inline u32 rgb565_bgr8(u16 c) {
    u32 r = (u32)(((c >> 11) & 0x1F) * 255 / 31);
    u32 g = (u32)(((c >> 5)  & 0x3F) * 255 / 63);
    u32 b = (u32)(( c        & 0x1F) * 255 / 31);
    return b | (g << 8) | (r << 16);
}
static void sub_fbpx(u8 *fb, int x, int y, u32 c) {
    if ((unsigned)x < SCR_W && (unsigned)y < SCR_H) {
        u8 *p = fb + ((size_t)x * SCR_H + (SCR_H - 1 - y)) * 3;
        p[0] = (u8)(c & 0xFF); p[1] = (u8)((c >> 8) & 0xFF); p[2] = (u8)((c >> 16) & 0xFF);  /* B,G,R */
    }
}
static void sub_fbtext(u8 *fb, int x, int y, int sc, u32 col, const char *s) {
    while (*s) {
        uint32_t cp = u8_next(&s);                          /* one UTF-8 codepoint per glyph */
        const char *g = sub_glyph(cp); if (!g) g = font8x8_basic['?'];   /* no glyph -> '?' */
        for (int row = 0; row < 8; row++) { char bits = g[row];
            for (int c = 0; c < 8; c++) if (bits & (1 << c))
                for (int a = 0; a < sc; a++) for (int b = 0; b < sc; b++)
                    sub_fbpx(fb, x + c * sc + a, y + row * sc + b, col);
        }
        x += 8 * sc;
    }
}
static void sub_fbfill(u8 *fb, u32 c) { int n = SCR_W * SCR_H; for (int i = 0; i < n; i++) sub_fbpx(fb, i / SCR_H, SCR_H - 1 - (i % SCR_H), c); }

#define SUB_MAXLN 4
#define SUB_LNW  160                        /* line buffer in BYTES (UTF-8: up to 3 per glyph) */

/* Wrap a cue into display lines: keep the SRT's own line breaks (dialogue dashes),
 * and word-wrap only within each of those lines. Returns line count. */
static int sub_wrap(const char *t, char lines[SUB_MAXLN][SUB_LNW], int maxch) {
    if (maxch > (SUB_LNW - 1) / 3) maxch = (SUB_LNW - 1) / 3;   /* codepoints; guard the byte buffer */
    if (maxch < 1) maxch = 1;
    int nl = 0;
    for (const char *seg = t; *seg && nl < SUB_MAXLN; ) {
        const char *segend = seg; while (*segend && *segend != '\n') segend++;   /* one source line */
        char cur[SUB_LNW]; int cb = 0, cw = 0;   /* current line: byte length, codepoint width */
        for (const char *p = seg; p < segend && nl < SUB_MAXLN; ) {
            while (p < segend && (*p == ' ' || *p == '\t' || *p == '\r')) p++;
            if (p >= segend) break;
            const char *w = p; int ww = 0;       /* word: byte range [w,p), ww codepoints */
            while (p < segend && *p != ' ' && *p != '\t' && *p != '\r') { const char *q = p; u8_next(&q); p = q; ww++; }
            if (ww > maxch) {                    /* word longer than a line -> truncate at a codepoint edge */
                const char *q = w; for (int k = 0; k < maxch; k++) u8_next(&q); p = q; ww = maxch;
            }
            int wb = (int)(p - w);
            if (cw == 0) { memcpy(cur, w, wb); cb = wb; cw = ww; }
            else if (cw + 1 + ww <= maxch) { cur[cb++] = ' '; memcpy(cur + cb, w, wb); cb += wb; cw += 1 + ww; }
            else { cur[cb] = 0; snprintf(lines[nl++], SUB_LNW, "%s", cur); memcpy(cur, w, wb); cb = wb; cw = ww; }
        }
        if (cb > 0 && nl < SUB_MAXLN) { cur[cb] = 0; snprintf(lines[nl++], SUB_LNW, "%s", cur); }
        seg = (*segend == '\n') ? segend + 1 : segend;
    }
    return nl;
}
/* draw the wrapped lines centered at cx (white text + a thin black outline, no box) */
static void sub_draw_lines(u8 *fb, int cx, int y0, char lines[SUB_MAXLN][SUB_LNW], int nl, int sc) {
    int lh = 8 * sc + 4;
    for (int i = 0; i < nl; i++) {
        const char *s = lines[i];
        int x = cx - u8_cplen(s) * 8 * sc / 2, y = y0 + i * lh;   /* center by glyph count, not bytes */
        sub_fbtext(fb, x - 1, y, sc, 0, s); sub_fbtext(fb, x + 1, y, sc, 0, s);
        sub_fbtext(fb, x, y - 1, sc, 0, s); sub_fbtext(fb, x, y + 1, sc, 0, s);
        sub_fbtext(fb, x, y, sc, 0x00FFFFFF, s);          /* white, B|G|R all 0xFF */
    }
}
static void sub_overlay(int is3d, int64_t us) {
    const char *t = subs_active(us);
    if (!t || !t[0]) return;
    int sc = g_sub_size < 1 ? 1 : g_sub_size > 3 ? 3 : g_sub_size;
    int maxch = (SCR_W - 20) / (8 * sc);
    char lines[SUB_MAXLN][SUB_LNW]; int nl = sub_wrap(t, lines, maxch);
    if (nl == 0) return;
    int total = nl * (8 * sc + 4);
    int y0 = g_sub_top ? 12 : (SCR_H - 12 - total);
    for (int eye = 0; eye < (is3d ? 2 : 1); eye++) {
        u8 *fb = (u8 *)gfxGetFramebuffer(GFX_TOP, eye ? GFX_RIGHT : GFX_LEFT, NULL, NULL);
        int dx = is3d ? ((eye == 0) ? -g_sub_depth : g_sub_depth) : 0;   /* parallax between eyes */
        sub_draw_lines(fb, SCR_W / 2 + dx, y0, lines, nl, sc);
    }
}

/* debug timing overlay (per stereo pair), updated ~1x/sec from the loop */
static int g_dbg_fps = 0, g_dbg_dec10 = 0, g_dbg_blit10 = 0, g_dbg_aud10 = 0;

/* "touch is locked" toast box; drawn last so it sits on top of everything */
static void panel_lock_toast(void) {
    if (g_lock_toast <= 0) return;
    int bw = 184, bh = 42, bx = (UI_W - bw) / 2, by = (UI_H - bh) / 2;
    ui_fill_round(bx, by, bw, bh, 8, UI_BG2);
    ui_frame_round(bx, by, bw, bh, 8, UI_NEONC, 2);
    ui_text_center(UI_W / 2, by + 8, 1, UI_NEONC, "TOUCH IS LOCKED");
    ui_text_center(UI_W / 2, by + 24, 1, UI_INK, "Press L to unlock");
}

static void panel_draw(const char *title, int64_t cur, int64_t dur, int playing) {
    ui_begin(GFX_BOTTOM);
    ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);

    ui_text_clipped(10, 8, 1, UI_NEON, title, 10, VOL_X - 12);   /* clipped so it can't run into volume */

    char tc[16], td[16], line[40];
    fmt_time(cur, tc, sizeof(tc));
    fmt_time(dur, td, sizeof(td));
    snprintf(line, sizeof(line), "%s / %s", tc, td);
    ui_text(10, 26, 1, UI_NEONC, line);

    /* battery: small icon + % at the far right of the time row (cyan while charging) */
    batt_refresh();
    if (g_batt_pct >= 0) {
        char bs[8]; snprintf(bs, sizeof bs, "%d%%", g_batt_pct);
        u16 bcol = g_batt_chg ? UI_NEONC : (g_batt_pct <= 15 ? UI_RED : g_batt_pct <= 30 ? UI_RGB(255,180,60) : UI_NEON);
        /* icon pinned to the right edge (nub ends 10px from the edge, mirroring the time at x=10);
           the number sits just left of it and grows leftward, so the battery never moves. */
        int bw = 16, bh = 10, bx = 292, by = 24;
        int tx = (bx - 4) - ui_text_w(1, bs);                 /* number right-aligned against the icon */
        ui_text(tx, 26, 1, bcol, bs);
        ui_frame_round(bx, by, bw, bh, 2, bcol, 1);           /* body */
        ui_fill(bx + bw, by + 3, 2, bh - 6, bcol);           /* terminal nub */
        int fw = (bw - 4) * g_batt_pct / 100; if (fw < 1 && g_batt_pct > 0) fw = 1;
        ui_fill_round(bx + 2, by + 2, fw, bh - 4, 1, bcol);  /* charge fill */
    }
    if (g_sw_bank >= 0) { /* banking engine: live bank depth -> confirms the engine + shows it working */
        char bk[24]; snprintf(bk, sizeof bk, "buf %d/%d", g_sw_bank, SR_NB);
        ui_text(10, 42, 1, g_sw_bank > 2 ? UI_NEON : UI_RGB(255,180,60), bk);
    } else if (g_old3d_warn)   /* Old 3DS can't keep up with 3D's doubled frame rate */
        ui_text(10, 42, 1, UI_RED, "3D Has Performance Issues on Old3DS");

    /* progress bar: rounded track + neon fill + glowing knob */
    double frac = dur > 0 ? (double)cur / (double)dur : 0.0;
    if (frac > 1) frac = 1;
    ui_fill_round(BAR_X, BAR_Y, BAR_W, BAR_H, BAR_H / 2, TH_TRACK);
    int fw = (int)(BAR_W * frac);
    if (fw > 0) ui_fill_round(BAR_X, BAR_Y, fw, BAR_H, BAR_H / 2, UI_NEON);
    int kx = BAR_X + fw, ky = BAR_Y + BAR_H / 2, kr = 8;
    ui_glow_round(kx - kr, ky - kr, 2 * kr, 2 * kr, kr, UI_NEON, 4, 26);
    ui_fill_round(kx - kr, ky - kr, 2 * kr, 2 * kr, kr, UI_WHITE);

    /* transport: RW (<<)   glowing play/pause   FF (>>) */
    ui_play_l(RW_CX - 5, PLAY_CY, 14, UI_NEONC); ui_play_l(RW_CX + 6, PLAY_CY, 14, UI_NEONC);
    ui_play(FF_CX - 6, PLAY_CY, 14, UI_NEONC);   ui_play(FF_CX + 5, PLAY_CY, 14, UI_NEONC);
    int R = 24;
    ui_glow_round(PLAY_CX - R, PLAY_CY - R, 2 * R, 2 * R, R, UI_NEON, 6, 22);
    ui_vgrad_round(PLAY_CX - R, PLAY_CY - R, 2 * R, 2 * R, R, UI_BG2, TH_BG1);
    ui_frame_round(PLAY_CX - R, PLAY_CY - R, 2 * R, 2 * R, R, UI_NEON, 2);
    if (playing) ui_pause(PLAY_CX, PLAY_CY, 22, UI_NEON);
    else         ui_play(PLAY_CX + 2, PLAY_CY, 22, UI_NEON);

    /* volume slider (vertical, right) */
    ui_fill_round(VOL_X, VOL_Y, 12, VOL_H, 6, TH_TRACK);
    int vf = (int)(VOL_H * (g_vol / 4.0f));
    if (vf > 0) ui_fill_round(VOL_X, VOL_Y + VOL_H - vf, 12, vf, 6, UI_NEON);
    char vs[8]; snprintf(vs, sizeof(vs), "%d%%", (int)(g_vol * 100 + 0.5f));
    ui_text(VOL_X - 16, VOL_Y - 12, 1, UI_INK, vs);   /* label above the slider (frees the button row) */

    /* action buttons: SAME row as the home screen (OPEN VIDEO / MANAGE / ADD VIDEO) */
    ui_text(6, 190, 1, UI_INK, "Y=SUBS X=DARK A=PLAY L=LOCK");   /* key map (B=back is universal) */
    ui_button(BKB_X, BTN_Y, BKB_W, BTN_H, "OPEN VIDEO", 0, UI_NEON);
    ui_button(OPB_X, BTN_Y, OPB_W, BTN_H, "MANAGE",     0, UI_NEON);
    ui_button(EXB_X, BTN_Y, EXB_W, BTN_H, "ADD VIDEO",  0, UI_NEON);

    if (g_touch_locked)   /* below the battery so a long movie title can't overlap it */
        ui_text(UI_W - 8 - ui_text_w(1, "LOCKED"), 38, 1, UI_NEONC, "LOCKED");
    /* subtitles: CC toggle/options (glows when on) */
    ui_button(CC_X, CC_Y, CC_W, CC_H, "CC", g_sub_on, g_sub_on ? UI_NEON : UI_DIM);
    /* bottom-screen-off: a crescent-moon button (video keeps playing on top) */
    if (g_lcd_ok) {
        ui_button(DIM_X, DIM_Y, DIM_W, DIM_H, "", 0, UI_NEONP);
        int mx = DIM_X + DIM_W / 2, my = DIM_Y + DIM_H / 2, mr = 8;
        ui_fill_round(mx - mr, my - mr, 2 * mr, 2 * mr, mr, UI_NEONC);                 /* full disc */
        ui_fill_round(mx - mr + 6, my - mr - 2, 2 * mr, 2 * mr, mr, UI_BG2); /* carve -> crescent */
    }
    panel_lock_toast();   /* drawn LAST -> always on top of every control */
    ui_present();
}

/* ---- subtitle options (modal, bottom screen; top keeps showing the frozen frame) ---- */
static int sub_modal(const char *title, const char *const *items, int n, int start) {
    int sel = (start >= 0 && start < n) ? start : 0;
    int top = 40, step = (218 - top) / (n > 0 ? n : 1);   /* adaptive: fit n items above the footer */
    if (step > 30) step = 30;
    int bh = step - 4; if (bh > 26) bh = 26; if (bh < 15) bh = 15;
    while (aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown();
        if (k & KEY_B) return -1;
        if (k & KEY_DOWN) sel = (sel + 1) % n;
        if (k & KEY_UP)   sel = (sel + n - 1) % n;
        if (k & KEY_A) return sel;
        touchPosition tp; hidTouchRead(&tp);
        if (k & KEY_TOUCH)
            for (int i = 0; i < n; i++) { int by = top + i * step;
                if (tp.py >= by && tp.py < by + bh && tp.px >= 18 && tp.px < UI_W - 18) return i; }
        ui_begin(GFX_BOTTOM);
        ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);
        ui_text_center(UI_W / 2, 14, 2, UI_NEON, title);
        for (int i = 0; i < n; i++) { int by = top + i * step;
            ui_button(18, by, UI_W - 36, bh, items[i], i == sel, UI_NEONC); }
        ui_present();
        gspWaitForVBlank();
    }
    return -1;
}
static void sub_msg(const char *msg) {
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & (KEY_A | KEY_B | KEY_TOUCH)) break;
        ui_begin(GFX_BOTTOM);
        ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);
        char b[256]; snprintf(b, sizeof b, "%s", msg); int y = 90;
        for (char *p = b; p; ) { char *e = strchr(p, '\n'); if (e) *e = 0;
            ui_text_center(UI_W / 2, y, 1, UI_INK, p); y += 16; if (!e) break; p = e + 1; }
        ui_text_center(UI_W / 2, 210, 1, UI_DIM, "tap / A to continue");
        ui_present();
        gspWaitForVBlank();
    }
}
static int sub_srt_scan(const char *dir, char names[][128], char paths[][512], int start, int max) {
    DIR *d = opendir(dir); if (!d) return start;
    struct dirent *e; int n = start;
    while ((e = readdir(d)) && n < max) {
        size_t L = strlen(e->d_name);
        if (L > 4 && !strcasecmp(e->d_name + L - 4, ".srt")) {
            snprintf(names[n], 128, "%s", e->d_name);
            snprintf(paths[n], 512, "%s%s", dir, e->d_name);
            n++;
        }
    }
    closedir(d);
    return n;
}
static void sub_load_menu(const char *moviepath) {
    static char names[5][128]; static char paths[5][512];
    const char *b = strrchr(moviepath, '/');
    char dir[512]; int dl = b ? (int)(b - moviepath + 1) : 0;
    snprintf(dir, sizeof dir, "%.*s", dl, moviepath);        /* the movie's own folder */
    int nf = sub_srt_scan(dir, names, paths, 0, 5);
    nf = sub_srt_scan("sdmc:/moflex_player/moviedata/", names, paths, nf, 5);
    if (nf == 0) { sub_msg("No .srt files found in the\nmovie folder or moviedata."); return; }
    const char *items[5]; for (int i = 0; i < nf; i++) items[i] = names[i];
    int c = sub_modal("LOAD SRT", items, nf, 0);
    if (c < 0) return;
    if (subs_load(paths[c])) { g_sub_on = 1; sub_msg("Subtitles loaded."); }
    else sub_msg("Could not read that file.");
}
/* live depth (parallax) adjuster: sample caption previewed in 3D on the top screen */
static void sub_depth_menu(int is3d) {
    const char *sample = "Subtitle depth";
    int sc = g_sub_size < 1 ? 1 : g_sub_size > 3 ? 3 : g_sub_size;
    char lines[SUB_MAXLN][SUB_LNW]; int nl = sub_wrap(sample, lines, (SCR_W - 20) / (8 * sc));
    int hrep = 0;
    while (aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown(), kh = hidKeysHeld();
        if (k & (KEY_A | KEY_B)) break;
        int lr = (kh & KEY_LEFT) ? -1 : (kh & KEY_RIGHT) ? 1 : 0;   /* hold to repeat */
        if (!lr) hrep = 0;
        else { int fire = (k & (KEY_LEFT | KEY_RIGHT)) ? 1 : 0;
               if (!fire) { hrep++; if (hrep > 10 && hrep % 3 == 0) fire = 1; }
               if (fire) { g_sub_depth += lr; if (g_sub_depth < -16) g_sub_depth = -16; if (g_sub_depth > 16) g_sub_depth = 16; } }
        touchPosition tp; hidTouchRead(&tp);
        if ((k & KEY_TOUCH) && tp.py >= 150 && tp.py < 188) {
            if (tp.px >= 30 && tp.px < 150 && g_sub_depth > -16) g_sub_depth--;
            else if (tp.px >= 170 && tp.px < 290 && g_sub_depth < 16) g_sub_depth++;
        }
        /* top: neutral background + the sample caption at the current depth (both eyes) */
        for (int eye = 0; eye < (is3d ? 2 : 1); eye++) {
            u8 *fb = (u8 *)gfxGetFramebuffer(GFX_TOP, eye ? GFX_RIGHT : GFX_LEFT, NULL, NULL);
            sub_fbfill(fb, rgb565_bgr8(UI_BG2));
            int dx = is3d ? ((eye == 0) ? -g_sub_depth : g_sub_depth) : 0;
            sub_draw_lines(fb, SCR_W / 2 + dx, SCR_H - 48, lines, nl, sc);
        }
        ui_begin(GFX_BOTTOM);
        ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);
        ui_text_center(UI_W / 2, 20, 2, UI_NEON, "SUBTITLE DEPTH");
        char v[16]; snprintf(v, sizeof v, "%+d", g_sub_depth);
        ui_text_center(UI_W / 2, 74, 3, UI_NEONC, v);
        ui_button(30, 150, 120, 36, "< OUT", 0, UI_NEONP);
        ui_button(170, 150, 120, 36, "IN >", 0, UI_NEON);
        ui_present();
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    }
}
/* subtitle timing offset, in 0.25s steps (for lip-sync fine-tuning) */
static void sub_offset_menu(void) {
    int hrep = 0;
    while (aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown(), kh = hidKeysHeld();
        if (k & (KEY_A | KEY_B)) break;
        int lr = (kh & KEY_LEFT) ? -1 : (kh & KEY_RIGHT) ? 1 : 0;   /* hold to repeat */
        if (!lr) hrep = 0;
        else { int fire = (k & (KEY_LEFT | KEY_RIGHT)) ? 1 : 0;
               if (!fire) { hrep++; if (hrep > 10 && hrep % 3 == 0) fire = 1; }
               if (fire) { g_sub_off += (int64_t)lr * 250000;
                           if (g_sub_off < -60000000) g_sub_off = -60000000;
                           if (g_sub_off >  60000000) g_sub_off =  60000000; } }
        touchPosition tp; hidTouchRead(&tp);
        if ((k & KEY_TOUCH) && tp.py >= 150 && tp.py < 188) {
            if (tp.px >= 20 && tp.px < 155 && g_sub_off > -60000000) g_sub_off -= 250000;
            else if (tp.px >= 165 && tp.px < 300 && g_sub_off < 60000000) g_sub_off += 250000;
        }
        int64_t a = g_sub_off < 0 ? -g_sub_off : g_sub_off;   /* format without float printf */
        char v[24]; snprintf(v, sizeof v, "%c%d.%02d s", g_sub_off < 0 ? '-' : '+',
                             (int)(a / 1000000), (int)((a % 1000000) / 10000));
        ui_begin(GFX_BOTTOM);
        ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);
        ui_text_center(UI_W / 2, 20, 2, UI_NEON, "SUBTITLE DELAY");
        ui_text_center(UI_W / 2, 74, 3, UI_NEONC, v);
        ui_button(20, 150, 135, 36, "< EARLIER", 0, UI_NEONP);
        ui_button(165, 150, 135, 36, "LATER >", 0, UI_NEON);
        ui_present();
        gspWaitForVBlank();
    }
}
static void sub_menu(const char *moviepath, int is3d) {
    int msel = 0;                       /* keep the cursor on the item you just changed */
    for (;;) {
        char i0[28], i1[28], i2[28], i3[28], i4[28], i5[28];
        snprintf(i0, sizeof i0, "Subtitles: %s", g_sub_on ? "ON" : "OFF");
        snprintf(i1, sizeof i1, "Position: %s", g_sub_top ? "TOP" : "BOTTOM");
        snprintf(i2, sizeof i2, "Size: %s", g_sub_size == 1 ? "Small" : g_sub_size == 2 ? "Medium" : "Large");
        int64_t da = g_sub_off < 0 ? -g_sub_off : g_sub_off;
        snprintf(i3, sizeof i3, "Delay: %c%d.%02d s", g_sub_off < 0 ? '-' : '+',
                 (int)(da / 1000000), (int)((da % 1000000) / 10000));
        snprintf(i5, sizeof i5, "Encoding: %s%s", g_sub_enc_name[g_sub_enc], g_sub_mode < 0 ? " (UTF-8)" : "");
        const char *items[8]; int act[8], n = 0;
        items[n] = i0; act[n++] = 0;
        items[n] = i1; act[n++] = 1;
        items[n] = i2; act[n++] = 2;
        items[n] = i3; act[n++] = 3;
        items[n] = i5; act[n++] = 6;                         /* text encoding (for non-UTF-8 SRTs) */
        if (is3d) { snprintf(i4, sizeof i4, "Depth (3D): %+d", g_sub_depth); items[n] = i4; act[n++] = 4; }
        items[n] = "Load SRT file..."; act[n++] = 5;
        int c = sub_modal("SUBTITLES", items, n, msel);
        if (c < 0) { subcfg_save(moviepath); return; }        /* B closes the menu -> persist for this movie */
        msel = c;                                            /* stay on this row after the action */
        int a = act[c];
        if (a == 0) { if (g_nsubs > 0) g_sub_on = !g_sub_on; else sub_msg("No subtitles loaded.\nUse 'Load SRT file'."); }
        else if (a == 1) g_sub_top = !g_sub_top;
        else if (a == 2) g_sub_size = g_sub_size >= 3 ? 1 : g_sub_size + 1;
        else if (a == 3) sub_offset_menu();
        else if (a == 4) sub_depth_menu(is3d);
        else if (a == 5) sub_load_menu(moviepath);
        else if (a == 6) { g_sub_enc = (g_sub_enc + 1) % 5;   /* re-decode the loaded SRT with the new codepage */
                           if (g_sub_file[0]) { char want[512]; snprintf(want, sizeof want, "%s", g_sub_file);
                                                subs_load(want); } }   /* saved with the rest when the menu closes */
    }
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
                             int64_t *cur_us, int *vidx, int *left_ok, int *left_vidx, int is3d) {
    MfxPacket pkt;
    for (int guard = 0; guard < 4000; guard++) {           /* bound the scan */
        if (mfx_next_packet(m, &pkt) != 1) return;
        if (m->streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;  /* skip audio */
        *cur_us = m->ts;
        AVPacket ap; ap.data = pkt.data; ap.size = pkt.size; int got = 0;
        int ok = (mobi_decode(ctx, out, &got, &ap) >= 0 && got);
        if (!is3d) {                                        /* 2D: first decodable frame is the landing */
            if (ok) { blit_eye(out, GFX_LEFT, W, H); sub_overlay(0, *cur_us); gfxFlushBuffers(); gfxSwapBuffers(); (*vidx)++; return; }
            (*vidx)++; continue;
        }
        if ((*vidx & 1) == 0) {                             /* left eye */
            if (ok) { blit_eye(out, GFX_LEFT, W, H); *left_ok = 1; *left_vidx = *vidx; }
            else *left_ok = 0;
        } else {                                            /* right eye: present a matched pair */
            if (ok && *left_ok && *vidx == *left_vidx + 1) {
                blit_eye(out, GFX_RIGHT, W, H);
                sub_overlay(1, *cur_us);                    /* subtitle on the landing frame */
                gfxFlushBuffers(); gfxSwapBuffers();
                (*vidx)++;
                return;                                     /* landing frame shown */
            }
            *left_ok = 0;
        }
        (*vidx)++;
    }
}

/* ===================== Old-3DS GPU playback path =====================
 * citro2d: Y2R (hardware color) -> GPU texture -> C2D draw (GPU does the 90-deg rotate, so the
 * CPU never touches the cache-thrashing transpose that stutters the Old 3DS). Bottom panel is
 * drawn with C2D too. Same controls/seek/resume as the classic path. */
#define NWB 16                 /* deeper audio buffer -> more slack against fps dips */
#define ABUF_SAMPLES (16 * 1024)
#define VLEAD_US 20000         /* show each frame ~20ms ahead of its audible audio position (lip-sync bias) */
#define GT_W 512
#define GT_H 256
static Handle g_y2r_done;
static int g_y2r_bpp = 2;   /* Y2R output bytes/pixel: 2 = RGB565 (16-bit), 4 = RGBA8 (true 24-bit color) */
static void g_y2r_init(int W, int H, int bpp) {
    y2rInit();
    g_y2r_bpp = bpp;
    Y2RU_ConversionParams p; memset(&p, 0, sizeof p);
    p.input_format = INPUT_YUV420_INDIV_8;
    p.output_format = (bpp == 4) ? OUTPUT_RGB_32 : OUTPUT_RGB_16_565;   /* 4 -> 24-bit color, no gradient banding */
    p.rotation = ROTATION_NONE; p.block_alignment = BLOCK_8_BY_8;
    p.input_line_width = (s16)W; p.input_lines = (s16)H;
    p.standard_coefficient = COEFFICIENT_ITU_R_BT_601_SCALING; p.alpha = 0xFF;   /* TV range (content is 16..235) */
    Y2RU_SetConversionParams(&p);
    Y2RU_SetTransferEndInterrupt(true);
    Y2RU_GetTransferEndEvent(&g_y2r_done);
}
static void g_y2r_start(AVFrame *o, C3D_Tex *tex, int W, int H) {
    int cw = W / 2, ch = H / 2, bpp = g_y2r_bpp;
    GSPGPU_FlushDataCache(o->data[0], (u32)W * H);
    GSPGPU_FlushDataCache(o->data[1], (u32)cw * ch);
    GSPGPU_FlushDataCache(o->data[2], (u32)cw * ch);
    Y2RU_SetSendingY(o->data[0], (u32)W * H, (s16)W, 0);
    Y2RU_SetSendingU(o->data[1], (u32)cw * ch, (s16)cw, 0);
    Y2RU_SetSendingV(o->data[2], (u32)cw * ch, (s16)cw, 0);
    Y2RU_SetReceiving(tex->data, (u32)W * H * bpp, (s16)(W * bpp * 8), (s16)((GT_W - W) * bpp * 8));
    svcClearEvent(g_y2r_done); Y2RU_StartConversion();
}
/* NOTE: uses the raw cache syscall, NOT C3D_TexFlush (which goes through the shared GSP session) --
 * this runs on the decode thread and must not touch GSP while the main thread renders. */
static void g_y2r_wait(C3D_Tex *tex) {
    svcWaitSynchronization(g_y2r_done, 300000000LL);
    svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)tex->data, tex->size);
}

/* draw an outlined button box (citro2d) + centered label */
static void gp_btn(int x, int y, int w, int h, C2D_Text *label, u32 col, int fill) {
    C2D_DrawRectSolid(x, y, 0, w, h, fill ? col : C2D_Color32(28, 28, 28, 255));
    if (!fill) {   /* outline */
        C2D_DrawRectSolid(x, y, 0, w, 2, col);           C2D_DrawRectSolid(x, y + h - 2, 0, w, 2, col);
        C2D_DrawRectSolid(x, y, 0, 2, h, col);           C2D_DrawRectSolid(x + w - 2, y, 0, 2, h, col);
    }
    if (label) {
        float tw = 0; C2D_TextGetDimensions(label, 0.5f, 0.5f, &tw, NULL);
        C2D_DrawText(label, C2D_WithColor, x + (w - tw) / 2, y + (h - 15) / 2, 0, 0.5f, 0.5f,
                     fill ? C2D_Color32(0, 0, 0, 255) : col);
    }
}
static void g_panel(C3D_RenderTarget *bot, C2D_Text *ttitle, C2D_Text *ttime, C2D_Text *thint,
                    int64_t cur, int64_t dur, int playing, float vol) {
    u32 wht = C2D_Color32(255, 255, 255, 255), gry = C2D_Color32(150, 150, 150, 255);
    u32 neon = C2D_Color32(80, 160, 255, 255), dim = C2D_Color32(90, 90, 90, 255), red = C2D_Color32(230, 70, 70, 255);
    /* fixed labels parsed once */
    static C2D_TextBuf lbuf; static C2D_Text tCC, tBACK, tOPEN, tEXIT; static int linit = 0;
    if (!linit) { lbuf = C2D_TextBufNew(64);
        C2D_TextParse(&tCC, lbuf, "CC");   C2D_TextOptimize(&tCC);
        C2D_TextParse(&tBACK, lbuf, "BACK"); C2D_TextOptimize(&tBACK);
        C2D_TextParse(&tOPEN, lbuf, "OPEN"); C2D_TextOptimize(&tOPEN);
        C2D_TextParse(&tEXIT, lbuf, "EXIT"); C2D_TextOptimize(&tEXIT); linit = 1; }
    static C2D_TextBuf bbuf; if (!bbuf) bbuf = C2D_TextBufNew(16);

    C2D_TargetClear(bot, C2D_Color32(0, 0, 0, 255));
    C2D_SceneBegin(bot);
    C2D_DrawText(ttitle, C2D_WithColor, 6, 4, 0, 0.6f, 0.6f, wht);
    C2D_DrawText(ttime,  C2D_WithColor, 6, 26, 0, 0.5f, 0.5f, gry);

    /* battery: icon + % pinned to the top-right */
    batt_refresh();
    if (g_batt_pct >= 0) {
        u32 bcol = g_batt_chg ? neon : (g_batt_pct <= 15 ? red :
                   g_batt_pct <= 30 ? C2D_Color32(255, 180, 60, 255) : C2D_Color32(80, 220, 120, 255));
        int bx = 292, by = 24, bw = 16, bh = 10;
        C2D_DrawRectSolid(bx, by, 0, bw, bh, bcol);
        C2D_DrawRectSolid(bx + 1, by + 1, 0, bw - 2, bh - 2, C2D_Color32(0, 0, 0, 255));   /* hollow body */
        int fw = (bw - 4) * g_batt_pct / 100; if (fw < 1 && g_batt_pct > 0) fw = 1;
        C2D_DrawRectSolid(bx + 2, by + 2, 0, fw, bh - 4, bcol);                            /* charge fill */
        C2D_DrawRectSolid(bx + bw, by + 3, 0, 2, bh - 6, bcol);                            /* terminal nub */
        char bs[8]; snprintf(bs, sizeof bs, "%d%%", g_batt_pct);
        C2D_Text tb; C2D_TextBufClear(bbuf); C2D_TextParse(&tb, bbuf, bs); C2D_TextOptimize(&tb);
        float tw = 0; C2D_TextGetDimensions(&tb, 0.45f, 0.45f, &tw, NULL);
        C2D_DrawText(&tb, C2D_WithColor, bx - 5 - tw, 25, 0, 0.45f, 0.45f, bcol);
    }

    /* seek bar */
    double frac = dur > 0 ? (double)cur / (double)dur : 0; if (frac > 1) frac = 1;
    C2D_DrawRectSolid(BAR_X, BAR_Y, 0, BAR_W, BAR_H, C2D_Color32(60, 60, 60, 255));
    C2D_DrawRectSolid(BAR_X, BAR_Y, 0, (float)(BAR_W * frac), BAR_H, wht);
    C2D_DrawRectSolid(BAR_X + (float)(BAR_W * frac) - 2, BAR_Y - 3, 0, 5, BAR_H + 6, neon);

    /* transport: RW (<<)   play/pause   FF (>>) */
    C2D_DrawTriangle(RW_CX + 7, PLAY_CY - 9, neon, RW_CX + 7, PLAY_CY + 9, neon, RW_CX - 3, PLAY_CY, neon, 0);
    C2D_DrawTriangle(RW_CX - 1, PLAY_CY - 9, neon, RW_CX - 1, PLAY_CY + 9, neon, RW_CX - 11, PLAY_CY, neon, 0);
    C2D_DrawTriangle(FF_CX - 7, PLAY_CY - 9, neon, FF_CX - 7, PLAY_CY + 9, neon, FF_CX + 3, PLAY_CY, neon, 0);
    C2D_DrawTriangle(FF_CX + 1, PLAY_CY - 9, neon, FF_CX + 1, PLAY_CY + 9, neon, FF_CX + 11, PLAY_CY, neon, 0);
    if (playing) { C2D_DrawRectSolid(PLAY_CX - 9, PLAY_CY - 13, 0, 6, 26, wht); C2D_DrawRectSolid(PLAY_CX + 3, PLAY_CY - 13, 0, 6, 26, wht); }
    else C2D_DrawTriangle(PLAY_CX - 8, PLAY_CY - 13, wht, PLAY_CX - 8, PLAY_CY + 13, wht, PLAY_CX + 12, PLAY_CY, wht, 0);

    /* volume: % label above the vertical slider */
    static C2D_TextBuf vbuf; if (!vbuf) vbuf = C2D_TextBufNew(16);
    { char vs[8]; snprintf(vs, sizeof vs, "%d%%", (int)(vol * 100 + 0.5f));
      C2D_Text tv; C2D_TextBufClear(vbuf); C2D_TextParse(&tv, vbuf, vs); C2D_TextOptimize(&tv);
      float tw = 0; C2D_TextGetDimensions(&tv, 0.45f, 0.45f, &tw, NULL);
      C2D_DrawText(&tv, C2D_WithColor, VOL_X + 6 - tw / 2, VOL_Y - 18, 0, 0.45f, 0.45f, wht); }
    C2D_DrawRectSolid(VOL_X, VOL_Y, 0, 12, VOL_H, C2D_Color32(60, 60, 60, 255));
    C2D_DrawRectSolid(VOL_X, VOL_Y + VOL_H * (1.0f - vol / 4.0f), 0, 12, VOL_H * (vol / 4.0f), neon);

    /* CC toggle (glows when subs on) */
    gp_btn(CC_X, CC_Y, CC_W, CC_H, &tCC, g_sub_on ? neon : dim, 0);

    /* bottom-screen-off: crescent moon (video keeps playing on top) */
    if (g_lcd_ok) {
        gp_btn(DIM_X, DIM_Y, DIM_W, DIM_H, NULL, C2D_Color32(120, 120, 200, 255), 0);
        int mx = DIM_X + DIM_W / 2, my = DIM_Y + DIM_H / 2, mr = 8;
        C2D_DrawCircleSolid(mx, my, 0, mr, C2D_Color32(200, 220, 255, 255));
        C2D_DrawCircleSolid(mx + 5, my - 2, 0, mr, C2D_Color32(28, 28, 28, 255));   /* carve -> crescent */
    }

    /* action buttons: BACK / OPEN / EXIT */
    gp_btn(BKB_X, BTN_Y, BKB_W, BTN_H, &tBACK, C2D_Color32(200, 120, 220, 255), 0);
    gp_btn(OPB_X, BTN_Y, OPB_W, BTN_H, &tOPEN, neon, 0);
    gp_btn(EXB_X, BTN_Y, EXB_W, BTN_H, &tEXIT, red, 0);

    C2D_DrawText(thint, C2D_WithColor, 8, 222, 0, 0.42f, 0.42f, gry);
}

/* ============ REUSE THE RELEASE PANEL/MENU: draw the app's software UI (panel_draw / sub_menu,
 * the exact look everyone already knows) onto the bottom screen as a citro2d texture. The UI renders
 * to an offscreen RGB565 buffer (ui_capture -> ui_pixels); we 8x8-tile that into a texture the same
 * way Y2R feeds the video textures (BLOCK_8_BY_8), so it lands upright with the shared sub-mapping.
 * This runs every frame the movie is playing, so subtitle size/depth/etc. preview live. ============ */
static C3D_Tex g_uitex; static C2D_Image g_uiimg; static int g_uitex_ok = 0;
static Tex3DS_SubTexture g_uisub;
static inline u32 r3_tile_off(u32 x, u32 y) {   /* 3DS 8x8 Morton (z-order) offset in a GT_W-wide tex */
    u32 m = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
    return ((y >> 3) * (GT_W >> 3) + (x >> 3)) * 64 + m;
}
static int ui_tex_init(void) {
    if (g_uitex_ok) return 1;
    if (!C3D_TexInit(&g_uitex, GT_W, GT_H, GPU_RGB565)) return 0;
    C3D_TexSetFilter(&g_uitex, GPU_LINEAR, GPU_LINEAR);
    g_uisub = (Tex3DS_SubTexture){ UI_W, UI_H, 0.0f, 1.0f, (float)UI_W / GT_W, 1.0f - (float)UI_H / GT_H };
    g_uiimg = (C2D_Image){ &g_uitex, &g_uisub };
    g_uitex_ok = 1; return 1;
}
static void ui_tex_free(void) { if (g_uitex_ok) { C3D_TexDelete(&g_uitex); g_uitex_ok = 0; } }

/* lift the offscreen UI buffer (already drawn) into the texture and draw it on the bottom screen */
static void ui_tex_present(C3D_RenderTarget *bot) {
    ui_capture(0);
    const u16 *src = ui_pixels();                /* pixel (x,y) at [x*UI_H + (UI_H-1-y)] (rotated fb) */
    u16 *dst = (u16 *)g_uitex.data;
    for (int y = 0; y < UI_H; y++)
        for (int x = 0; x < UI_W; x++)
            dst[r3_tile_off(x, y)] = src[x * UI_H + (UI_H - 1 - y)];
    C3D_TexFlush(&g_uitex);
    C2D_TargetClear(bot, C2D_Color32(0, 0, 0, 255));
    C2D_SceneBegin(bot);
    C2D_DrawImageAt(g_uiimg, 0, 0, 0, NULL, 1, 1);
}

/* Drop-in for g_panel(): render the release panel to the offscreen buffer, then texture it. */
static void g_panel_sw(C3D_RenderTarget *bot, const char *title, int64_t cur, int64_t dur, int playing) {
    if (!ui_tex_init()) return;
    ui_capture(1);
    panel_draw(title, cur, dur, playing);       /* fills g_buf; ui_present() is a no-op under capture */
    ui_tex_present(bot);
}

/* ==================== NON-BLOCKING SUBTITLE MENU (ring engine, live preview) ====================
 * The shipped sub_menu() is a blocking modal that draws to the framebuffer -- it can't run while
 * citro2d owns the GPU and the movie plays. This renders the same options to the offscreen buffer
 * every frame (textured onto the bottom like the panel) and takes one frame of input at a time, so
 * the movie keeps playing on top and size/position/depth/encoding changes preview live. ============ */
static int g_submenu = 0;      /* 0 = closed, 1 = options list, 2 = SRT file picker */
static int g_sub_sel = 0;      /* selected row in the options list */
static int g_sub_rep = 0;      /* left/right hold-repeat counter (delay/depth) */
static int g_srt_sel = 0, g_srt_n = 0;
static char g_srt_names[8][128], g_srt_paths[8][512];

/* action codes per row, in display order (depth row only when 3D). Returns row count. */
static int submenu_actions(int is3d, int *act) {
    int n = 0;
    act[n++] = 0; act[n++] = 1; act[n++] = 2; act[n++] = 3; act[n++] = 6;
    if (is3d) act[n++] = 4;
    act[n++] = 5;
    return n;
}
/* row layout shared by render + touch hit-testing (mirrors sub_modal's adaptive rows) */
static void submenu_layout(int n, int *top, int *step, int *bh) {
    *top = 40; *step = (218 - *top) / (n > 0 ? n : 1); if (*step > 30) *step = 30;
    *bh = *step - 4; if (*bh > 26) *bh = 26; if (*bh < 15) *bh = 15;
}
static void submenu_label(int a, char *r, int cap) {
    switch (a) {
        case 0: snprintf(r, cap, "Subtitles:  %s", g_sub_on ? "ON" : "OFF"); break;
        case 1: snprintf(r, cap, "Position:  %s", g_sub_top ? "Top" : "Bottom"); break;
        case 2: snprintf(r, cap, "Size:  %s", g_sub_size == 1 ? "Small" : g_sub_size == 2 ? "Medium" : "Large"); break;
        case 3: { int64_t da = g_sub_off < 0 ? -g_sub_off : g_sub_off;
                  snprintf(r, cap, "Delay:  %c%d.%02d s", g_sub_off < 0 ? '-' : '+', (int)(da / 1000000), (int)((da % 1000000) / 10000)); } break;
        case 6: snprintf(r, cap, "Encoding:  %s%s", g_sub_enc_name[g_sub_enc], g_sub_mode < 0 ? " (UTF-8)" : ""); break;
        case 4: snprintf(r, cap, "Depth (3D):  %+d", g_sub_depth); break;
        default: snprintf(r, cap, "Load SRT file..."); break;
    }
}
static void submenu_render(int is3d) {
    int act[8]; int n = submenu_actions(is3d, act);
    int top, step, bh; submenu_layout(n, &top, &step, &bh);
    ui_begin(GFX_BOTTOM);
    ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);
    ui_text_center(UI_W / 2, 14, 2, UI_NEON, "SUBTITLES");
    for (int i = 0; i < n; i++) {
        char r[40]; submenu_label(act[i], r, sizeof r);
        ui_button(18, top + i * step, UI_W - 36, bh, r, i == g_sub_sel, UI_NEONC);
    }
}
static void srtpicker_render(void) {
    int top, step, bh; submenu_layout(g_srt_n > 0 ? g_srt_n : 1, &top, &step, &bh);
    ui_begin(GFX_BOTTOM);
    ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);
    ui_text_center(UI_W / 2, 14, 2, UI_NEON, "LOAD SRT");
    if (g_srt_n == 0) {   /* same two-line message as the released sub_load_menu */
        ui_text_center(UI_W / 2, 100, 1, UI_INK, "No .srt files found in the");
        ui_text_center(UI_W / 2, 118, 1, UI_INK, "movie folder or moviedata.");
    }
    for (int i = 0; i < g_srt_n; i++)
        ui_button(18, top + i * step, UI_W - 36, bh, g_srt_names[i], i == g_srt_sel, UI_NEONC);
}
static void g_submenu_sw(C3D_RenderTarget *bot, int is3d) {
    if (!ui_tex_init()) return;
    ui_capture(1);
    if (g_submenu == 2) srtpicker_render(); else submenu_render(is3d);
    ui_tex_present(bot);
}
/* dark bottom screen: pure black -- with ONLY the lock toast when it flashes on (never the panel) */
static void g_dark_sw(C3D_RenderTarget *bot) {
    if (!ui_tex_init()) return;
    ui_capture(1);
    ui_begin(GFX_BOTTOM);
    ui_clear(0);          /* black */
    panel_lock_toast();
    ui_tex_present(bot);
}

/* open the SRT picker: scan the movie folder + moviedata for .srt files */
static void submenu_open_srt(const char *moviepath) {
    const char *b = strrchr(moviepath, '/');
    char dir[512]; int dl = b ? (int)(b - moviepath + 1) : 0;
    snprintf(dir, sizeof dir, "%.*s", dl, moviepath);
    g_srt_n = sub_srt_scan(dir, g_srt_names, g_srt_paths, 0, 8);
    g_srt_n = sub_srt_scan("sdmc:/moflex_player/moviedata/", g_srt_names, g_srt_paths, g_srt_n, 8);
    g_srt_sel = 0; g_submenu = 2;
}

/* return the row a touch at (px,py) hit, or -1; also reports which half (left/right) was tapped */
static int submenu_hit(int px, int py, int n, int *side) {
    int top, step, bh; submenu_layout(n, &top, &step, &bh);
    for (int i = 0; i < n; i++) { int by = top + i * step;
        if (py >= by && py < by + bh && px >= 18 && px < UI_W - 18) { *side = px < UI_W / 2 ? -1 : 1; return i; } }
    return -1;
}
/* one frame of menu input; returns 1 when the whole menu should close (config persisted). */
static int submenu_input(u32 kd, u32 kh, touchPosition tp, int is3d, const char *moviepath) {
    if (g_submenu == 2) {   /* SRT file picker */
        if (kd & KEY_B) { g_submenu = 1; return 0; }
        if (g_srt_n > 0) {
            if (kd & KEY_DOWN) g_srt_sel = (g_srt_sel + 1) % g_srt_n;
            if (kd & KEY_UP)   g_srt_sel = (g_srt_sel + g_srt_n - 1) % g_srt_n;
            int side, hit = (kd & KEY_TOUCH) ? submenu_hit(tp.px, tp.py, g_srt_n, &side) : -1;
            if (hit >= 0) g_srt_sel = hit;
            if ((kd & KEY_A) || hit >= 0) { if (subs_load(g_srt_paths[g_srt_sel])) g_sub_on = 1; g_submenu = 1; }
        }
        return 0;
    }
    int act[8]; int n = submenu_actions(is3d, act);
    if (g_sub_sel >= n) g_sub_sel = n - 1;
    if (g_sub_sel < 0)  g_sub_sel = 0;
    if (kd & KEY_B) { subcfg_save(moviepath); g_submenu = 0; return 1; }
    if (kd & KEY_DOWN) g_sub_sel = (g_sub_sel + 1) % n;
    if (kd & KEY_UP)   g_sub_sel = (g_sub_sel + n - 1) % n;

    int t_side = 0, t_row = (kd & KEY_TOUCH) ? submenu_hit(tp.px, tp.py, n, &t_side) : -1;
    if (t_row >= 0) g_sub_sel = t_row;
    int a = act[g_sub_sel];
    int press = (kd & KEY_RIGHT) ? 1 : (kd & KEY_LEFT) ? -1 : 0;   /* single tap: toggles/cycles */
    if (kd & KEY_A) press = 1;
    int held = (kh & KEY_RIGHT) ? 1 : (kh & KEY_LEFT) ? -1 : 0;    /* hold-repeat: delay/depth */
    int rep = 0;
    if (!held) g_sub_rep = 0;
    else { rep = (kd & (KEY_LEFT | KEY_RIGHT)) ? held : 0;
           if (!rep) { g_sub_rep++; if (g_sub_rep > 10 && g_sub_rep % 3 == 0) rep = held; } }
    if (t_row >= 0) {                                  /* a row was tapped this frame */
        if (a == 3 || a == 4) rep = t_side;            /* delay/depth: left half -, right half + */
        else press = 1;                                /* toggle/cycle/encoding/load: activate */
    }
    switch (a) {
        case 0: if (press) { if (g_nsubs > 0) g_sub_on = !g_sub_on; } break;
        case 1: if (press) g_sub_top = !g_sub_top; break;
        case 2: if (press > 0) g_sub_size = g_sub_size >= 3 ? 1 : g_sub_size + 1;
                else if (press < 0) g_sub_size = g_sub_size <= 1 ? 3 : g_sub_size - 1; break;
        case 3: if (rep) { g_sub_off += (int64_t)rep * 250000;
                           if (g_sub_off < -60000000) g_sub_off = -60000000;
                           if (g_sub_off >  60000000) g_sub_off =  60000000; } break;
        case 4: if (rep) { g_sub_depth += rep; if (g_sub_depth < -16) g_sub_depth = -16; if (g_sub_depth > 16) g_sub_depth = 16; } break;
        case 6: if (press) { g_sub_enc = (g_sub_enc + (press < 0 ? 4 : 1)) % 5;
                             if (g_sub_file[0]) { char w[512]; snprintf(w, sizeof w, "%s", g_sub_file); subs_load(w); } } break;
        default: if (press) submenu_open_srt(moviepath); break;   /* Load SRT... */
    }
    return 0;
}

/* ---- audio worker: runs on core 1, owns ndsp, reads its OWN file handle so it feeds the DSP
 * at real time completely decoupled from the video decode on core 0 (this is what the official
 * player does -- one worker thread -- and why its audio never stutters). ---- */
static volatile int   g_aw_stop, g_aw_paused, g_aw_seek;
static volatile int64_t g_aw_seek_us;
static volatile int64_t g_aud_play_us;   /* content timestamp the DSP is actually playing now */
static volatile int64_t g_aud_start = -1;/* audio timeline anchor (astart) -> video compares RELATIVE time */
static volatile int64_t g_vid_us;        /* content timestamp the video has reached (main thread) */
static void audio_worker(void *arg) {
    const char *path = (const char *)arg;
    FILE *af = fopen(path, "rb");
    if (!af) return;
    MfxDemux am;
    if (mfx_open_auto(&am, af, path) != 0) { fclose(af); return; }
    am.audio_only = 1;   /* skip video chunks entirely -> minimal core-1 work, less contention */
    int ai = -1;
    for (int i = 0; i < am.nb_streams; i++) if (am.streams[i].media_type == MFX_TYPE_AUDIO && ai < 0) ai = i;
    if (ai < 0) { mfx_close(&am); fclose(af); return; }
    int arate = am.streams[ai].sample_rate, chn = am.streams[ai].channels;
    ndspChnReset(0); ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE); ndspChnSetRate(0, (float)arate);
    ndspChnSetFormat(0, chn == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
    float mix[12]; memset(mix, 0, sizeof mix); mix[0] = mix[1] = 1.0f; ndspChnSetMix(0, mix);
    ndspWaveBuf wbuf[NWB]; int16_t *abuf[NWB]; memset(wbuf, 0, sizeof wbuf);
    for (int i = 0; i < NWB; i++) { abuf[i] = (int16_t *)linearAlloc(ABUF_SAMPLES * chn * 2); wbuf[i].data_vaddr = abuf[i]; wbuf[i].status = NDSP_WBUF_DONE; }
    int wb_idx = 0, was_paused = 0;
    int64_t astart = -1; long long played = 0; int slot_ns[NWB]; memset(slot_ns, 0, sizeof slot_ns);
    MfxPacket pkt;
    while (!g_aw_stop) {
        if (g_aw_seek) {
            mfx_seek_time(&am, g_aw_seek_us);
            ndspChnWaveBufClear(0);
            for (int i = 0; i < NWB; i++) wbuf[i].status = NDSP_WBUF_DONE;
            wb_idx = 0; g_aw_seek = 0;
            /* leave g_aud_play_us = -1 (offline); the first decoded audio packet below sets astart and
             * brings the clock online at the real landing position -> video holds until audio truly flows. */
            astart = -1; played = 0; memset(slot_ns, 0, sizeof slot_ns); g_aud_play_us = -1; g_aud_start = -1;
        }
        if (g_aw_paused) { if (!was_paused) { ndspChnSetPaused(0, true); was_paused = 1; } svcSleepThread(8000000); continue; }
        if (was_paused) { ndspChnSetPaused(0, false); was_paused = 0; }
        if (mfx_next_packet(&am, &pkt) != 1) { g_aw_paused = 1; continue; }   /* EOF -> idle */
        if (am.streams[pkt.stream_index].media_type != MFX_TYPE_AUDIO) continue;   /* skip video */
        ndspWaveBuf *wb = &wbuf[wb_idx];
        while (wb->status != NDSP_WBUF_FREE && wb->status != NDSP_WBUF_DONE) {
            if (g_aw_stop || g_aw_seek || g_aw_paused) break;
            svcSleepThread(3000000);   /* 3ms: buffers are deep, so poll lazily -> far fewer syscore
                                        * wakeups fighting the memory-bound video decoder on core 0 */
        }
        if (g_aw_stop || g_aw_seek || g_aw_paused) continue;
        int fr = adpcm_moflex_decode(pkt.data, pkt.size, chn, abuf[wb_idx]);
        if (fr > 0 && fr <= ABUF_SAMPLES) {
            if (astart < 0) { astart = am.ts; g_aud_start = am.ts; }   /* anchor the audio timeline */
            played += slot_ns[wb_idx]; slot_ns[wb_idx] = fr;   /* the slot we're reusing has finished */
            g_aud_play_us = astart + (int64_t)(played * 1000000LL / arate);
            float v = g_vol;
            if (v != 1.0f) { int16_t *s = abuf[wb_idx]; int ns = fr * chn;
                for (int i = 0; i < ns; i++) { int t = (int)(s[i] * v); s[i] = (int16_t)(t > 32767 ? 32767 : (t < -32768 ? -32768 : t)); } }
            DSP_FlushDataCache(abuf[wb_idx], fr * chn * 2);
            memset(wb, 0, sizeof *wb); wb->data_vaddr = abuf[wb_idx]; wb->nsamples = fr;
            ndspChnWaveBufAdd(0, wb); wb_idx = (wb_idx + 1) % NWB;
            /* audio is the MASTER clock: it always plays at natural 1.0x pitch. The video
             * thread slaves to g_aud_play_us (below). No rate-matching -> no pitch artifacts. */
        }
    }
    ndspChnWaveBufClear(0); ndspChnReset(0);
    for (int i = 0; i < NWB; i++) if (abuf[i]) linearFree(abuf[i]);
    mfx_close(&am); fclose(af);
}

static MoflexResult moflex_play_gpu(const char *path) {
    vol_load();
    FILE *f = fopen(path, "rb");
    if (!f) return MOFLEX_ERROR;
    MfxDemux m;
    if (mfx_open_auto(&m, f, path) != 0) { fclose(f); return MOFLEX_ERROR; }
    int vi = -1, ai = -1;
    for (int i = 0; i < m.nb_streams; i++) {
        if (m.streams[i].media_type == MFX_TYPE_VIDEO && vi < 0) vi = i;
        if (m.streams[i].media_type == MFX_TYPE_AUDIO && ai < 0) ai = i;
    }
    if (vi < 0) { mfx_close(&m); fclose(f); return MOFLEX_ERROR; }
    int W = m.streams[vi].width, H = m.streams[vi].height;
    int arate = ai >= 0 ? m.streams[ai].sample_rate : 44100, chn = ai >= 0 ? m.streams[ai].channels : 2;
    int have_audio = (ai >= 0);
    int is3d = mfx_detect_stereo(&m);   /* 3D interleaved vs flat 2D (2D = half the frames -> Old 3DS keeps up) */
    /* per-frame display cadence from the stream timebase -- the container block timestamps are coarse
     * (carried-forward, many frames share one), so pacing per-frame off them makes the video hang then
     * burst. Packets are mono eyes, so a stereo PAIR spans 2 frames. Clamp sane; default 25 pairs/s. */
    int64_t pair_dur = 40000;
    if (m.streams[vi].tb_den > 0) {
        /* the stream timebase IS the per-displayed-frame period (e.g. 1001/24000 = 23.976fps).
         * Do NOT double it -- the earlier x2 made the video play at half speed. */
        int64_t pd = (int64_t)m.streams[vi].tb_num * 1000000 / m.streams[vi].tb_den;
        if (pd >= 16000 && pd <= 100000) pair_dur = pd;
    }
    if (is3d && m.tb_is_eye) pair_dur *= 2;   /* Nintendo declares the EYE rate: a pair lasts 2x */

    AVCodecContext ctx; memset(&ctx, 0, sizeof ctx);
    ctx.width = W; ctx.height = H; ctx.priv_data = calloc(1, mobi_ctx_size());
    if (!ctx.priv_data || mobi_init(&ctx) != 0) { free(ctx.priv_data); mfx_close(&m); fclose(f); return MOFLEX_ERROR; }
    mobi_opt = 14;
    AVFrame *fL = av_frame_alloc(), *fR = av_frame_alloc();

    const char *bn = strrchr(path, '/'); bn = bn ? bn + 1 : path;
    char title[64];
    const char *cia_title = cia_selection_title(path);   /* movie title if playing inside a CIA */
    if (cia_title) snprintf(title, sizeof title, "%.60s", cia_title);
    else {
        snprintf(title, sizeof title, "%.60s", bn);
        size_t L = strlen(title);
        if (L > 7 && !strcasecmp(title + L - 7, ".moflex")) title[L - 7] = 0;
        else if (L > 4 && !strcasecmp(title + L - 4, ".zip")) title[L - 4] = 0;
        else if (L > 4 && !strcasecmp(title + L - 4, ".cia")) title[L - 4] = 0;
    }

    /* citro2d presents in 24-bit; the app had the screens in RGB565 (software UI) -> switch to the
     * gfxInitDefault default so the display reads citro2d's output correctly (restored on exit). */
    gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);
    gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);
    gfxSet3D(is3d);
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE); C2D_Init(C2D_DEFAULT_MAX_OBJECTS); C2D_Prepare();
    C3D_RenderTarget *topL = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget *topR = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);
    C3D_RenderTarget *bot  = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    #define NTB 12           /* decode-ahead ring: decode banks ready pairs here during calm scenes
                              * (1.55x real time) so busy scenes (0.87x) coast on them -> no drift.
                              * ~0.5s of video; each pair is one step late so R-eye Y2R overlaps next L. */
    C3D_Tex texL[NTB], texR[NTB];
    Tex3DS_SubTexture sub = { (u16)W, (u16)H, 0.0f, 1.0f, (float)W / GT_W, 1.0f - (float)H / GT_H };
    C2D_Image imgL[NTB], imgR[NTB];
    for (int i = 0; i < NTB; i++) {
        C3D_TexInit(&texL[i], GT_W, GT_H, GPU_RGB565); C3D_TexSetFilter(&texL[i], GPU_LINEAR, GPU_LINEAR);
        C3D_TexInit(&texR[i], GT_W, GT_H, GPU_RGB565); C3D_TexSetFilter(&texR[i], GPU_LINEAR, GPU_LINEAR);
        imgL[i] = (C2D_Image){ &texL[i], &sub }; imgR[i] = (C2D_Image){ &texR[i], &sub };
    }
    g_y2r_init(W, H, 2);   /* this path uses RGB565 textures above */
    C2D_TextBuf sbuf = C2D_TextBufNew(128), tmbuf = C2D_TextBufNew(48);
    C2D_Text ttitle, thint, ttime;
    C2D_TextParse(&ttitle, sbuf, title); C2D_TextOptimize(&ttitle);
    C2D_TextParse(&thint, sbuf, "A pause <>seek ^v vol  L/R=A/V sync  B back"); C2D_TextOptimize(&thint);
    char last_ts[64] = "";
    C2D_TextParse(&ttime, tmbuf, " "); C2D_TextOptimize(&ttime);
    u32 black = C2D_Color32(0, 0, 0, 255);

    MoflexResult result = MOFLEX_QUIT_BACK;
    int vidx = 0, left_ok = 0, left_vidx = -2, playing = 1, dirty = 1;
    /* decode-ahead ring: wr = pairs decoded-in (monotonic), rd = pairs presented; slot = idx % NTB.
     * last_rp = slot whose R-eye Y2R is still in flight (waited before the next L start). */
    int wr = 0, rd = 0, last_rp = -1, shown_landing = 0, last_shown = -1;
    int64_t ring_ts[NTB], ring_bts[NTB]; int ring_ready[NTB] = { 0 };   /* synthetic + raw block ts */
    int64_t cur_show_ts = 0, cur_show_bts = 0, next_vts = 0, v_anchor = 0; int have_anchor = 0;
    int64_t cal_aud0 = -1; int cal_frames = 0;                    /* calibrate pair_dur from the AUDIO clock */
    int g_vlead_ms = 20, qmin = 99;                               /* DIAG: live A/V trim (L/R) + min ring depth */
    int64_t ce_prev = 0; u64 ce_wall = 0; int ce_have = 0;        /* outer rate lock: drive drift->0 via pair_dur */
    u64 t0_wall = 0; int64_t t0_us_base = 0;   /* real-time clock to keep video locked to audio */
    /* capacity HUD: time spent purely in mobi_decode vs content-time produced -> decode capacity
     * (mono fps the CPU can sustain) vs demand (mono fps the content needs). cap>=need => banking wins. */
    u64 dec_ticks = 0; int dec_frames = 0; int64_t win_us0 = -1; u64 win_w0 = 0; int hud_cap = 0, hud_need = 0;
    int64_t cur_us = 0, dur_us = m.duration_us, seek_to_us = 0; int want_seek = 0, shold = 0;
    int64_t rpos = resume_load(path), last_save = 0;
    if (rpos > 3000000 && (dur_us <= 0 || rpos < dur_us - 10000000)) { seek_to_us = rpos; want_seek = 1; last_save = rpos; }
    MfxPacket pkt;

    /* audio on a WORKER thread (core 1) with its OWN file handle -> fed in real time, decoupled
     * from the video decode on core 0. This is the stutter fix (what the official player does). */
    Thread awt = NULL;
    if (have_audio) {
        g_aw_stop = 0; g_aw_paused = 0;
        g_aud_play_us = -1;                                 /* "audio not started yet" -> video won't race */
        g_aw_seek = want_seek; g_aw_seek_us = seek_to_us;   /* worker seeks to the resume pos too */
        APT_SetAppCpuTimeLimit(30);                          /* light audio worker on the syscore */
        s32 aprio = 0x30; svcGetThreadPriority(&aprio, CUR_THREAD_HANDLE);
        awt = threadCreate(audio_worker, (void *)path, 16 * 1024, aprio - 1, 1, false);
        if (!awt) have_audio = 0;
    }

    while (aptMainLoop()) {
        hidScanInput();
        u32 kd = hidKeysDown(), kh = hidKeysHeld();
        touchPosition tp; hidTouchRead(&tp);
        if (kd & KEY_B) { result = MOFLEX_QUIT_BACK; break; }
        if (kd & KEY_A) { playing = !playing; if (have_audio) g_aw_paused = !playing; t0_wall = 0; dirty = 1; }
        if (kd & KEY_UP)   { if (g_vol < 4.0f) g_vol += 0.25f; vol_save(); dirty = 1; }
        if (kd & KEY_DOWN) { if (g_vol > 0.25f) g_vol -= 0.25f; vol_save(); dirty = 1; }
        /* DIAG: L/R shoulders trim A/V sync live. R = video shown earlier (fixes audio-early),
         * L = video later. Find where lips match, then read 'o' off the HUD. */
        if (kd & KEY_R) { if (g_vlead_ms <  400) g_vlead_ms += 15; dirty = 1; }
        if (kd & KEY_L) { if (g_vlead_ms > -400) g_vlead_ms -= 15; dirty = 1; }
        /* DIAG: X/Y fine-tune the frame rate (pair_dur). If video drifts BEHIND audio, frames are too
         * slow -> Y shrinks pair_dur; if it races AHEAD, X grows it. Dial until it holds sync, read 'pd'. */
        if (kd & KEY_Y) { if (pair_dur > 16000) pair_dur -= 250; dirty = 1; }
        if (kd & KEY_X) { if (pair_dur < 100000) pair_dur += 250; dirty = 1; }
        { int sdir = (kh & KEY_RIGHT) ? 1 : ((kh & KEY_LEFT) ? -1 : 0);
          if (!sdir) shold = 0;
          else { int fire = (kd & (KEY_RIGHT | KEY_LEFT)) ? 1 : 0;
                 if (!fire) { shold++; if (shold > 14 && (shold % 6 == 0)) fire = 1; }
                 if (fire) { seek_to_us = cur_us + (int64_t)sdir * 30000000; want_seek = 1; } } }
        if ((kd | kh) & KEY_TOUCH) {
            int px = tp.px, py = tp.py;
            if (py >= BAR_Y - 8 && py <= BAR_Y + BAR_H + 8 && px >= BAR_X && px <= BAR_X + BAR_W) {
                if (kd & KEY_TOUCH) { seek_to_us = (int64_t)((double)(px - BAR_X) / BAR_W * dur_us); want_seek = 1; }
            } else if (px >= VOL_X - 6 && px <= VOL_X + 18 && py >= VOL_Y && py <= VOL_Y + VOL_H) {
                float nv = (float)(VOL_Y + VOL_H - py) / VOL_H * 4.0f; if (nv < 0.25f) nv = 0.25f; if (nv > 4.0f) nv = 4.0f;
                g_vol = nv; vol_save(); dirty = 1;
            } else if (kd & KEY_TOUCH) {
                if (py >= PLAY_CY - 20 && py <= PLAY_CY + 20) {
                    if (px >= PLAY_CX - 20 && px <= PLAY_CX + 20) { playing = !playing; if (have_audio) g_aw_paused = !playing; t0_wall = 0; dirty = 1; }
                    else if (px >= RW_CX - 18 && px <= RW_CX + 18) { seek_to_us = cur_us - 30000000; want_seek = 1; }
                    else if (px >= FF_CX - 18 && px <= FF_CX + 18) { seek_to_us = cur_us + 30000000; want_seek = 1; }
                } else if (py >= BTN_Y && py < BTN_Y + BTN_H) {   /* BACK / OPEN / EXIT */
                    if      (px >= BKB_X && px < BKB_X + BKB_W) { result = MOFLEX_QUIT_OPEN; break; }   /* OPEN VIDEO */
                    else if (px >= OPB_X && px < OPB_X + OPB_W) { result = MOFLEX_QUIT_MANAGE; break; }   /* MANAGE VIDEOS */
                    else if (px >= EXB_X && px < EXB_X + EXB_W) { result = MOFLEX_QUIT_ADD; break; }   /* ADD VIDEO */
                }
            }
        }
        if (want_seek) {
            do_seek(&m, &ctx, seek_to_us);
            vidx = 0; left_ok = 0; playing = 1;
            wr = 0; rd = 0; last_rp = -1; shown_landing = 0; last_shown = -1;   /* drop the whole ring */
            for (int i = 0; i < NTB; i++) ring_ready[i] = 0;
            have_anchor = 0; cal_aud0 = -1; cal_frames = 0; qmin = 99; ce_have = 0;   /* re-anchor; reset diag */
            if (have_audio) { g_aud_play_us = -1;   /* audio clock offline until worker re-locks at new pos */
                              g_aw_seek_us = seek_to_us; g_aw_seek = 1; g_aw_paused = 0; }
            t0_wall = 0; g_vid_us = seek_to_us;   /* reset the pace clock so it re-locks at the new position */
            want_seek = 0; dirty = 1;
        }

        /* ---- DECODE: fill the ring full-tilt whenever there's space. In calm scenes the decoder runs
         * 1.55x real time, so this BANKS ready pairs; in busy scenes (0.87x) the display coasts on them.
         * This is what turns the raw surplus into smooth, drift-free playback. ---- */
        int decoded = 0;
        if (playing && (wr - rd) < NTB - 2) {
            decoded = 1;
            if (mfx_next_packet(&m, &pkt) != 1) { playing = 0; if (have_audio) g_aw_paused = 1; dirty = 1; }
            else {
                cur_us = m.ts; if (cur_us < 0) cur_us = 0; if (dur_us > 0 && cur_us > dur_us) cur_us = dur_us;
                if (cur_us - last_save > 15000000 || cur_us < last_save) { resume_save_us(path, cur_us); last_save = cur_us; }
                int mt = m.streams[pkt.stream_index].media_type;
                if (mt == MFX_TYPE_VIDEO && !is3d) {            /* 2D: one full frame per ring slot */
                    int got = 0, fill = wr % NTB;
                    AVPacket ap; ap.data = pkt.data; ap.size = pkt.size;
                    u64 _dt = svcGetSystemTick();
                    int ok = (mobi_decode(&ctx, fL, &got, &ap) >= 0 && got);
                    dec_ticks += svcGetSystemTick() - _dt; dec_frames++;
                    if (ok) {
                        if (last_rp >= 0) { g_y2r_wait(&texL[last_rp]); ring_ready[last_rp] = 1; last_rp = -1; }
                        g_y2r_start(fL, &texL[fill], W, H);
                        if (!have_anchor) { next_vts = cur_us; v_anchor = cur_us; have_anchor = 1; }
                        ring_ts[fill] = next_vts; ring_bts[fill] = cur_us; next_vts += pair_dur;
                        ring_ready[fill] = 0; last_rp = fill; wr++;
                    }
                    vidx++;
                } else if (mt == MFX_TYPE_VIDEO) {              /* 3D: pair L+R into one ring slot */
                    int eye = vidx & 1; AVFrame *dst = eye ? fR : fL; int got = 0, fill = wr % NTB;
                    AVPacket ap; ap.data = pkt.data; ap.size = pkt.size;
                    u64 _dt = svcGetSystemTick();
                    int ok = (mobi_decode(&ctx, dst, &got, &ap) >= 0 && got);
                    dec_ticks += svcGetSystemTick() - _dt; dec_frames++;
                    if (eye == 0) {
                        if (ok) {
                            left_ok = 1; left_vidx = vidx;
                            /* the previous pair's R-eye conversion overlapped THIS L decode -> its wait is
                             * free now; do it BEFORE starting this L (one Y2R engine/event) and mark it ready. */
                            if (last_rp >= 0) { g_y2r_wait(&texR[last_rp]); ring_ready[last_rp] = 1; last_rp = -1; }
                            g_y2r_start(fL, &texL[fill], W, H);
                        } else left_ok = 0;
                    } else {
                        if (ok && left_ok && vidx == left_vidx + 1) {
                            g_y2r_wait(&texL[fill]); g_y2r_start(fR, &texR[fill], W, H);
                            /* even, smooth per-frame timestamp from the synthetic clock, re-anchored to the
                             * coarse block ts so it can't drift off the true content time. */
                            /* Pure synthetic frame clock: anchor + k*pair_dur. Do NOT steer it toward the
                             * block ts here -- that ts is throttled to our present rate, so steering to it
                             * fights the outer rate lock. pair_dur is corrected ONLY by the rate lock (which
                             * uses the audio-drift signal). ring_bts keeps the raw block ts for that signal. */
                            if (!have_anchor) { next_vts = cur_us; v_anchor = cur_us; have_anchor = 1; }
                            ring_ts[fill] = next_vts; ring_bts[fill] = cur_us;
                            next_vts += pair_dur;
                            ring_ready[fill] = 0; last_rp = fill; wr++;
                        }
                        left_ok = 0;
                    }
                    vidx++;
                    /* NOTE: measuring the frame rate at runtime is a trap -- when the ring fills, decode
                     * is throttled to the present rate, so any measurement just re-measures whatever
                     * (possibly wrong) rate we're already using and reinforces it. The container's declared
                     * timebase (pair_dur, set once from tb) is the ground truth; X/Y fine-tunes it. */
                    (void)cal_aud0; (void)cal_frames;
                }
            }
        }

        /* ---- clock = elapsed audio time since AUDIO started; video uses elapsed time since VIDEO started.
         * Comparing RELATIVE times cancels any offset between the two demuxes' timelines (they disagreed by
         * seconds, which froze the video at startup). No audio -> real-time wall clock. ---- */
        int64_t arel;   /* audio content elapsed (us), or -1 if audio not flowing yet */
        if (have_audio) {
            arel = (g_aud_play_us >= 0 && g_aud_start >= 0) ? (g_aud_play_us - g_aud_start) : -1;
        } else {
            if (!t0_wall && (wr - rd) > 0 && ring_ready[rd % NTB]) { t0_wall = osGetTime(); }
            arel = t0_wall ? (int64_t)(osGetTime() - t0_wall) * 1000 : -1;
        }

        /* ---- PRESENT: show each ready frame when the audio's elapsed time reaches that frame's elapsed
         * time (+ a small constant lead). Pinned to audio -> cannot drift; no frame-rate estimation. ---- */
        int show_slot = -1;
        if (playing && (wr - rd) > 0 && ring_ready[rd % NTB]) {
            int slot = rd % NTB;
            int64_t vrel = ring_bts[slot] - v_anchor;   /* this frame's elapsed video time */
            if (arel < 0) { if (!shown_landing) show_slot = slot; }   /* audio not started: show landing frame */
            else if (arel + (int64_t)g_vlead_ms * 1000 >= vrel) show_slot = slot;
            if (show_slot >= 0) { rd++; shown_landing = 1; last_shown = show_slot; cur_show_bts = ring_bts[slot];
                                  g_vid_us = cur_show_bts; dirty = 1; if ((wr - rd) < qmin) qmin = wr - rd; }
        }
        /* if we neither decoded nor have a frame to show, don't spin the CPU */
        if (!decoded && show_slot < 0 && !dirty) gspWaitForVBlank();

        if (dirty && last_shown >= 0) {
            int64_t ptime = cur_show_bts;
            char tc[16], ts[64];
            fmt_time(ptime, tc, sizeof tc);
            /* DIAG HUD: e = video-audio error ms (pinned per-frame, so it should stay ~CONSTANT now, not
             * grow); q = ring depth now/worst; sp = ring content span ms (coarse-ts check); o = lead (L/R). */
            int aerr = (have_audio && g_aud_play_us >= 0 && g_aud_start >= 0)
                       ? (int)(((cur_show_bts - v_anchor) - (g_aud_play_us - g_aud_start)) / 1000) : 0;
            int span = (wr > rd) ? (int)((ring_bts[(wr - 1) % NTB] - ring_bts[rd % NTB]) / 1000) : 0;
            snprintf(ts, sizeof ts, "%s e%d q%d/%d sp%d o%d",
                     tc, aerr, wr - rd, qmin == 99 ? 0 : qmin, span, g_vlead_ms);
            if (strcmp(ts, last_ts)) { strncpy(last_ts, ts, sizeof last_ts - 1);
                C2D_TextBufClear(tmbuf); C2D_TextParse(&ttime, tmbuf, ts); C2D_TextOptimize(&ttime); }
            C3D_FrameBegin(0);
            C2D_TargetClear(topL, black); C2D_SceneBegin(topL); C2D_DrawImageAt(imgL[last_shown], 0, 0, 0, NULL, 1, 1);
            C2D_TargetClear(topR, black); C2D_SceneBegin(topR); C2D_DrawImageAt(is3d ? imgR[last_shown] : imgL[last_shown], 0, 0, 0, NULL, 1, 1);
            g_panel(bot, &ttitle, &ttime, &thint, ptime, dur_us, playing, g_vol);
            C3D_FrameEnd(0);
            dirty = 0;
        }
    }
    if (!aptMainLoop() && result == MOFLEX_QUIT_BACK) result = MOFLEX_QUIT_EXIT;
gdone:
    if (dur_us > 0 && cur_us >= dur_us - 10000000) resume_clear(path);
    else if (cur_us > 3000000) resume_save_us(path, cur_us);
    if (awt) { g_aw_stop = 1; threadJoin(awt, 2000000000LL); threadFree(awt); }   /* stop audio worker */
    C2D_TextBufDelete(sbuf); C2D_TextBufDelete(tmbuf);
    for (int i = 0; i < NTB; i++) { C3D_TexDelete(&texL[i]); C3D_TexDelete(&texR[i]); }
    gspWaitForVBlank(); gspWaitForVBlank();
    C2D_Fini(); C3D_Fini(); y2rExit();
    /* restore the software-UI framebuffer format for the home/browser screens */
    gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);      /* 24-bit: RGB565 gave blue only 5 bits -> banding */
    gfxSetScreenFormat(GFX_BOTTOM, GSP_RGB565_OES);   /* UI panel stays 16-bit */
    gfxSet3D(false);
    av_frame_free(&fL); av_frame_free(&fR);
    mobi_close(&ctx); free(ctx.priv_data); mfx_close(&m); fclose(f);
    return result;
}

/* ================= DECODE-AHEAD RING PLAYER (3D) ==========================================
 * Banks decoded pairs during calm scenes (decoder runs ~1.55x real time there) and coasts on
 * them through busy bursts, presented on an AUDIO-MASTER clock. This is the avtest engine, wired
 * to the player's citro2d UI / seek / resume / subtitles.
 *
 *  - SINGLE demuxer: the interleaved audio packets are fed inline to ndsp (no second file handle,
 *    so the two never thrash the SD seeking against each other).
 *  - NOSKIP catch-up: the decoder NEVER skips, so no reference is ever corrupted (always a clean
 *    picture). When decode falls behind in heavy action, the present loop simply discards the
 *    stale banked pairs -- they were still decoded, so the reference chain stays perfect -- and
 *    re-locks to audio. Cost is a little lip-sync slip during sustained action, recovered in calm.
 *  - ATOMIC pair: both eyes are drawn inside ONE C3D_FrameEnd, joined by explicit ring slot, so a
 *    left/right pair can never split, tear across a flip, or show reversed.
 *
 * NOT YET ROUTED. Textures are RGB565 here (reusing g_y2r_*). Before this replaces the classic 3D
 * path, the Y2R + textures must move to 24-bit (OUTPUT_RGB_24 + GPU_RGB8) or 3D gradients would
 * band again (the classic path blits 24-bit to the software framebuffer). See task list. */

/* internal sentinel: the ring couldn't allocate on this console -> caller runs the classic path */
#define MOFLEX_FALLBACK ((MoflexResult)100)

/* --- single-demux audio bank for the ring player (r3_ = ring-3d, kept separate from the classic
 *     inline wbuf[] and the gpu-path audio_worker so nothing clashes). Sized SMALL: on the Old 3DS
 *     the linear heap is tight, and an oversized reserve here starves the video ring to zero (which
 *     then faults on `wr % NB`). 24 x 2048 stereo = 192KB is ~1.1s of read-ahead, plenty. --- */
#define R3_AWB  48            /* wavebufs: deep audio read-ahead so audio rides through decode dips */
#define R3_ABUF 2048          /* max samples/ch per audio packet (real ~2k) */
static ndspWaveBuf r3_wb[R3_AWB]; static int16_t *r3_ab[R3_AWB];
static int r3_awi, r3_nawb, r3_arate, r3_achn, r3_aok, r3_acnt[R3_AWB];
static long long r3_aplayed; static int64_t r3_apos;   /* elapsed audible us, -1 = not started */
/* A/V seek anchor: audio restarts at the first audio packet after the landing (content r3_audio_t0),
 * which can differ from the video landing by up to one packet. We capture BOTH once, right after the
 * audio re-locks, into a CONSTANT r3_av_skew (bounded), and add it to apos in the present compare.
 * Captured once (never re-derived from the drifting bts-rts) so it can't grow -> no runaway. */
static int64_t r3_audio_t0 = -1;   /* content ts of the first audio sample after (re)start */
static int64_t r3_av_skew = 0;     /* constant apos offset = audio_origin - video_origin */
static int     r3_av_ready = 0;    /* r3_av_skew has been captured for this segment */
static u64 r3_last;   /* last poll tick, for the phase-locked clock */
static void r3_audio_close(void);   /* used by the setup failure path below */
static void r3_audio_setup(int arate, int chn) {
    r3_arate = arate; r3_achn = chn; r3_awi = 0; r3_aplayed = 0; r3_apos = -1; r3_last = 0;
    r3_audio_t0 = -1; r3_av_skew = 0; r3_av_ready = 0;
    ndspChnReset(0); ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE); ndspChnSetRate(0, (float)arate);
    ndspChnSetFormat(0, chn == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
    float mix[12]; memset(mix, 0, sizeof mix); mix[0] = mix[1] = 1.0f; ndspChnSetMix(0, mix);
    memset(r3_wb, 0, sizeof r3_wb); memset(r3_acnt, 0, sizeof r3_acnt);
    for (r3_nawb = 0; r3_nawb < R3_AWB; r3_nawb++) {
        r3_ab[r3_nawb] = (int16_t *)linearAlloc(R3_ABUF * chn * 2);
        if (!r3_ab[r3_nawb]) break;
        r3_wb[r3_nawb].status = NDSP_WBUF_DONE;
    }
    if (r3_nawb < 8) { r3_audio_close(); return; }   /* leaking these bricked later C2D_Init -> dark screens */
    ndspChnSetPaused(0, true);   /* paused: bank audio during pre-roll, unpause aligned to content-0 */
    r3_aok = 1;
}
/* advance the audio clock from ACTUAL DSP playback. Counting only FINISHED wavebufs makes the clock
 * jump ~one audio packet (~23-46ms) at a time -- comparable to a frame period, so the video-present
 * decision fires in coarse chunks and frames show for uneven refresh counts (judder). Add the DSP's
 * live sample position WITHIN the playing wavebuf so the clock advances smoothly (per-sample), which
 * paces the video evenly. Kept monotonic so the wavebuf boundary never nudges it backward. */
static void r3_audio_poll(void) {
    if (!r3_aok) return;
    int adv = 0;
    for (int i = 0; i < r3_nawb; i++)
        if (r3_wb[i].status == NDSP_WBUF_DONE && !r3_acnt[i] && r3_wb[i].nsamples > 0) { r3_aplayed += r3_wb[i].nsamples; r3_acnt[i] = 1; adv = 1; }
    (void)adv;
    if (r3_apos < 0) return;
    /* PHASE-LOCKED clock: advance purely on smooth real time (monotonic tick) EVERY poll -- never steps
     * or jumps -> even video cadence. Then GENTLY pull toward the true audio position (a small fraction
     * per poll) to stay in A/V sync, instead of snapping. `audio` (last completed wavebuf) is a lower
     * bound on the true position; keep apos in [audio, audio+~100ms] with soft corrections. */
    u64 now = svcGetSystemTick();
    if (r3_last == 0) r3_last = now;
    int64_t dt = (int64_t)((now - r3_last) * 1000000LL / SYSCLOCK_ARM11);
    r3_last = now;
    r3_apos += dt;                                                  /* smooth free-run */
    int64_t audio = (int64_t)(r3_aplayed * 1000000LL / r3_arate);
    if (r3_apos < audio)                 r3_apos += (audio - r3_apos) / 8;               /* fell behind -> ease up */
    else if (r3_apos > audio + 100000)   r3_apos -= (r3_apos - audio - 100000) / 8;      /* too far ahead -> ease back */
}
/* buffer one audio packet. 1 = consumed, 0 = no free wavebuf (hold the packet and retry). */
static int r3_audio_feed(MfxPacket *pkt) {
    if (!r3_aok) return 1;
    ndspWaveBuf *w = &r3_wb[r3_awi];
    if (w->status != NDSP_WBUF_FREE && w->status != NDSP_WBUF_DONE) return 0;   /* full -> hold */
    if (w->status == NDSP_WBUF_DONE && !r3_acnt[r3_awi] && w->nsamples > 0) { r3_aplayed += w->nsamples; r3_acnt[r3_awi] = 1; }
    int fr = adpcm_moflex_decode(pkt->data, pkt->size, r3_achn, r3_ab[r3_awi]);
    if (fr <= 0 || fr > R3_ABUF) return 1;                                       /* bad -> drop */
    if (g_vol != 1.0f) { int16_t *s = r3_ab[r3_awi]; int ns = fr * r3_achn;
        for (int i = 0; i < ns; i++) { int t = (int)(s[i] * g_vol); s[i] = (int16_t)(t > 32767 ? 32767 : (t < -32768 ? -32768 : t)); } }
    DSP_FlushDataCache(r3_ab[r3_awi], fr * r3_achn * 2);
    memset(w, 0, sizeof *w); w->data_vaddr = r3_ab[r3_awi]; w->nsamples = fr;
    ndspChnWaveBufAdd(0, w); r3_acnt[r3_awi] = 0; r3_awi = (r3_awi + 1) % r3_nawb;
    return 1;
}
static void r3_audio_flush(void) {   /* on seek: drop everything queued, clock offline until re-lock */
    if (!r3_aok) return;
    ndspChnWaveBufClear(0);
    for (int i = 0; i < r3_nawb; i++) { r3_wb[i].status = NDSP_WBUF_DONE; r3_acnt[i] = 1; }
    r3_awi = 0; r3_aplayed = 0; r3_apos = -1; r3_last = 0;
    r3_audio_t0 = -1; r3_av_skew = 0; r3_av_ready = 0;   /* re-anchor A/V on the next lock */
    ndspChnSetPaused(0, true);
}
static void r3_audio_close(void) {   /* safe to call in ANY state: frees partial allocations too */
    ndspChnWaveBufClear(0); ndspChnReset(0);
    for (int i = 0; i < R3_AWB; i++) if (r3_ab[i]) { linearFree(r3_ab[i]); r3_ab[i] = NULL; }
    r3_nawb = 0; r3_aok = 0;
}

/* ---- compressed-video BACKLOG. Phase 1 stashes video packets here while feeding audio ahead, so
 * audio is NEVER throttled by the (slow) video decode -- the whole point. Phase 2 drains it one
 * frame at a time into the pair ring. Each entry keeps the packet's absolute movie time (m.ts). ---- */
/* Compressed-video backlog (avtest's model): each entry carries its container ts (for the seek bar /
 * subtitles / time display) and its keyframe flag. The pacing clock is dpair*pair_dur (smooth,
 * advances on decode AND skip), NOT the container ts (which is coarse/carried-forward and freezes the
 * picture if used for pacing). Keyframe PAIR positions are tracked separately (r3_kfp) so catch-up can
 * skip to the nearest keyframe at/before the audio clock. */
#define R3_VQ 256
static uint8_t *r3_vq[R3_VQ]; static int r3_vqn[R3_VQ]; static int64_t r3_vqts[R3_VQ]; static uint8_t r3_vqkf[R3_VQ];
static int r3_vqh, r3_vqt, r3_vqc;
#define R3_KFPN 256
static int r3_kfp[R3_KFPN]; static int r3_kfph, r3_kfpt, r3_kfpc;   /* buffered keyframe PAIR positions */
static int r3_pushv;                                                /* video packets pushed (pair = /2) */
static void r3_kfp_enq(int pos) {
    if (r3_kfpc >= R3_KFPN) { r3_kfph = (r3_kfph + 1) % R3_KFPN; r3_kfpc--; }
    r3_kfp[r3_kfpt] = pos; r3_kfpt = (r3_kfpt + 1) % R3_KFPN; r3_kfpc++;
}
static int r3_vq_push(const uint8_t *d, int n, int64_t ts, int kf) {
    if (r3_vqc >= R3_VQ) return 0;
    uint8_t *p = (uint8_t *)malloc(n); if (!p) return 0;
    memcpy(p, d, n); r3_vq[r3_vqt] = p; r3_vqn[r3_vqt] = n; r3_vqts[r3_vqt] = ts; r3_vqkf[r3_vqt] = (uint8_t)kf;
    r3_vqt = (r3_vqt + 1) % R3_VQ; r3_vqc++;
    if (kf && (r3_pushv & 1) == 0) r3_kfp_enq(r3_pushv / 2);   /* keyframe on a left eye -> its pair pos */
    r3_pushv++;
    return 1;
}
static int  r3_vq_head_is_kf(void) { return r3_vqc > 0 && r3_vqkf[r3_vqh]; }
static uint8_t *r3_vq_pop(int *n, int64_t *ts, int *kf) {
    if (r3_vqc <= 0) return NULL;
    uint8_t *p = r3_vq[r3_vqh]; *n = r3_vqn[r3_vqh]; *ts = r3_vqts[r3_vqh]; if (kf) *kf = r3_vqkf[r3_vqh];
    r3_vqh = (r3_vqh + 1) % R3_VQ; r3_vqc--; return p;
}
static void r3_vq_clear(void) { int n, kf; int64_t ts; while (r3_vqc > 0) free(r3_vq_pop(&n, &ts, &kf));
    r3_vqh = r3_vqt = r3_vqc = 0; r3_kfph = r3_kfpt = r3_kfpc = 0; r3_pushv = 0; }

#define R3_NB_MAX 40           /* ring bound; real depth grown at runtime from the linear heap */
static C3D_Tex r3_texL[R3_NB_MAX], r3_texR[R3_NB_MAX];
static C2D_Image r3_imgL[R3_NB_MAX], r3_imgR[R3_NB_MAX];
/* ring bookkeeping kept OFF the stack: moflex_play_ring's frame stacks on top of moflex_play's, and
 * the decoder's VLC init (ff_vlc_init_from_lengths) needs a lot of stack -- together they overflowed
 * the main-thread stack (data abort in VLC init). Static here + a thin dispatcher (below) fix it. */
static int     r3_ready[R3_NB_MAX];
static int64_t r3_rts[R3_NB_MAX];   /* pacing time (relative, resets on seek) */
static int64_t r3_bts[R3_NB_MAX];   /* absolute movie time (m.ts) for subs/seekbar/display */

/* draw the active cue onto the CURRENT citro2d scene (one eye), centred, with a 1px outline for
 * legibility. dx = per-eye horizontal shift for 3D subtitle depth. Uses the same g_sub_* settings
 * (size / top-vs-bottom / on) as the software path, so the SUBTITLES menu drives both. */
static void r3_draw_sub(C2D_Text *t, int dx, u32 col, u32 outline) {
    float sc = 0.5f + 0.25f * (float)(g_sub_size - 1);   /* 1->0.5  2->0.75  3->1.0 */
    float w = 0, h = 0; C2D_TextGetDimensions(t, sc, sc, &w, &h);
    /* AlignCenter centers EVERY line of a multi-line cue (block-left positioning made 2-line
     * cues look left-aligned); x is then the center line, not the left edge */
    float x = SCR_W * 0.5f + (float)dx;
    float y = g_sub_top ? 8.0f : (SCR_H - h - 10.0f);
    for (int oy = -1; oy <= 1; oy++) for (int ox = -1; ox <= 1; ox++)
        if (ox || oy) C2D_DrawText(t, C2D_WithColor | C2D_AlignCenter, x + ox, y + oy, 0, sc, sc, outline);
    C2D_DrawText(t, C2D_WithColor | C2D_AlignCenter, x, y, 0, sc, sc, col);
}

/* HOME/sleep handling for the GPU ring path. Without releasing Y2R before the applet takes the GPU,
 * the transition hard-locks the GSP ("won't close from HOME"). Release Y2R + pause audio on the way
 * out; re-acquire + resume on the way back. citro3d's own gfx state is restored by libctru's hook. */
static aptHookCookie g_ring_apt_cookie;
static volatile int g_ring_apt_audio = 0, g_ring_apt_playing = 1, g_ring_apt_redraw = 0;
static int g_ring_apt_w = 0, g_ring_apt_h = 0, g_ring_apt_bpp = 2;
/* The decode thread does Y2R while holding its lock; the hook (main thread) must not y2rExit()
 * mid-conversion. On suspend it sets g_ring_susp so the worker parks, then briefly takes the lock to
 * drain any in-flight produce before releasing Y2R. On restore it clears the flag and wakes it. */
static volatile int g_ring_susp = 0;
static LightLock  *g_ring_lock  = NULL;   /* &s.lock  when threaded, else NULL (Y2R on main thread) */
static LightEvent *g_ring_wake  = NULL;   /* &s.wake  when threaded */
static void mp_ring_apt_hook(APT_HookType hook, void *param) {
    (void)param;
    switch (hook) {
        case APTHOOK_ONSUSPEND:
        case APTHOOK_ONSLEEP:
            /* Hand back EVERY video-only hardware resource before the HOME/sleep applet takes the
             * GPU. Menus don't hold Y2R or gsp::Lcd; keeping them open across the transition
             * hard-locks the GSP (needs a power-off). Release them here, re-acquire on restore. */
            if (g_ring_apt_audio) ndspChnSetPaused(0, true);
            g_ring_susp = 1;                            /* stop the worker from starting a new Y2R... */
            if (g_ring_lock) { LightLock_Lock(g_ring_lock); LightLock_Unlock(g_ring_lock); }  /* ...and drain any in flight */
            if (g_screen_off) { GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM); g_screen_off = 0; } g_backlight_on = 1;
            if (g_lcd_ok) gspLcdExit();                 /* release gsp::Lcd (else HOME hard-locks) */
            y2rExit();                                  /* release Y2R for the applet */
            gfxSet3D(false);
            break;
        case APTHOOK_ONRESTORE:
        case APTHOOK_ONWAKEUP:
            g_y2r_init(g_ring_apt_w, g_ring_apt_h, g_ring_apt_bpp);      /* re-acquire Y2R */
            if (g_lcd_ok) gspLcdInit();                                  /* re-acquire gsp::Lcd */
            g_backlight_on = 1;                                          /* applet left the backlight on */
            if (g_ring_apt_audio && g_ring_apt_playing) ndspChnSetPaused(0, false);
            g_ring_susp = 0;                            /* let the worker produce again */
            if (g_ring_wake) LightEvent_Signal(g_ring_wake);
            g_ring_apt_redraw = 1;
            break;
        default: break;
    }
}

/* ============================ THREADED RING ENGINE ============================
 * A DECODE THREAD produces pairs into the texture ring (read packets -> feed audio -> decode -> Y2R),
 * and the MAIN THREAD presents them on a steady tick cadence. Decoupling present from decode is what
 * keeps the frame cadence even through heavy scenes (fixes the single-threaded loop's action judder),
 * and lets decode run ahead to build a cushion. Producer/consumer, like the official player. */
typedef struct {
    MfxDemux *m; AVCodecContext *ctx; AVFrame *fL, *fR;
    int W, H, is3d, NB, have_audio; int64_t pair_dur, dur_us;
    volatile int eye_swap;               /* 3D: stream at the seek landing starts on a RIGHT eye ->
                                            route the first of each popped pair to the R texture */
    volatile int wr, rd;                 /* ring: worker advances wr, main advances rd (both atomic ints) */
    volatile long long dpair;            /* decode content position (pairs) */
    volatile long long cur_us;           /* last decoded movie-time (us), for resume/HUD */
    volatile int skipping;               /* catch-up: dropping toward a keyframe */
    volatile int done;                   /* demux hit EOF */
    volatile int run;                    /* worker loop control */
    volatile int paused;                 /* playback paused OR seeking: worker idles */
    volatile int audio_pre_full;         /* worker's audio bank filled before pre-roll (audio-front-loaded) */
    volatile long long wk_dec; volatile int wk_n; volatile long long wk_dmax;   /* worker decode ticks / pairs / max (HUD) */
    int has_pending; MfxPacket pending;  /* a demuxed packet held across produce calls (audio bank was full) */
    LightLock lock;                      /* held by worker while touching demux/decoder/ring; by main during seek */
    LightEvent wake;                     /* main -> worker: slot freed / unpaused / seek finished */
} R3S;

/* Decode+bank ONE pair into ring slot wr%NB (or drop a pair while catching up). Runs under s->lock. */
static void r3_produce(R3S *s) {
    /* PHASE 1: keep the video backlog full + feed audio to the DSP bank */
    while (!s->done && r3_vqc < R3_VQ) {
        if (!s->has_pending) { if (mfx_next_packet(s->m, &s->pending) != 1) { s->done = 1; break; } s->has_pending = 1; }
        int mt = s->m->streams[s->pending.stream_index].media_type;
        if (mt == MFX_TYPE_AUDIO && s->have_audio) {
            if (r3_audio_feed(&s->pending)) {
                if (r3_audio_t0 < 0) { int64_t at = s->m->ts; r3_audio_t0 = at < 0 ? 0 : at; }  /* audio origin */
                s->has_pending = 0;
            }
            else { s->audio_pre_full = 1; break; }        /* bank full: hold packet, let main unpause audio */
        } else if (mt == MFX_TYPE_VIDEO) {
            int64_t vts = s->m->ts; if (vts < 0) vts = 0; if (s->dur_us > 0 && vts > s->dur_us) vts = s->dur_us;
            if (!r3_vq_push(s->pending.data, s->pending.size, vts, s->pending.keyframe)) break;
            s->has_pending = 0;
        } else s->has_pending = 0;
    }
    if (r3_vqc < 2) return;   /* not enough for a pair yet */

    /* CATCH-UP: if video has fallen > DRIFT_MAX pairs behind the audio clock, skip GOPs to the nearest
     * buffered keyframe at/before the audio position (never past it -> no freeze). */
    long long dpair = s->dpair;
    int apos_pair = (r3_apos >= 0) ? (int)(r3_apos / s->pair_dur) : -1;
    while (r3_kfpc > 0 && r3_kfp[r3_kfph] < (int)dpair) { r3_kfph = (r3_kfph + 1) % R3_KFPN; r3_kfpc--; }
    int target  = (r3_kfpc > 0) ? r3_kfp[r3_kfph] : -1;
    int canskip = (apos_pair >= 0) && (target > (int)dpair) && (target <= apos_pair);
    int drift   = (apos_pair >= 0) ? apos_pair - (int)dpair : 0;
    const int DRIFT_MAX = 20;   /* let the present drop-loop absorb decode spikes; only skip to a keyframe
                                 * when SUSTAINED far behind (spikes recover via the fast frames after them) */
    if (!s->skipping && canskip && drift > DRIFT_MAX) s->skipping = 1;
    if (s->skipping) {
        /* CLEAN catch-up: discard packets WITHOUT decoding until the queue head is a KEYFRAME, then
         * flush and resume decoding there. Never decode a P-frame against references we skipped --
         * that corrupts the whole GOP and the blockiness bleeds into the following calm scenes. The
         * cost is a brief freeze on the last clean frame, resolved the instant a keyframe lands. */
        if (r3_vqc == 0) { s->dpair = dpair + 1; return; }                 /* nothing buffered yet -> hold */
        if (r3_vq_head_is_kf()) { s->skipping = 0; mobi_flush(s->ctx); }   /* landed clean -> decode below */
        else {
            int n, kf; int64_t ts; free(r3_vq_pop(&n, &ts, &kf)); if (r3_vqc > 0) free(r3_vq_pop(&n, &ts, &kf));
            s->dpair = dpair + 1;
            return;
        }
    }

    /* PHASE 2: decode a pair, Y2R into the ring slot (inline -- off the present path now). */
    u64 _pt = svcGetSystemTick();
    int slot = s->wr % s->NB, n, kf, gotL = 0, gotR = 0; int64_t vts;
    AVPacket ap;
    uint8_t *p = r3_vq_pop(&n, &vts, &kf); ap.data = p; ap.size = n;
    int okL = (mobi_decode(s->ctx, s->fL, &gotL, &ap) >= 0 && gotL); free(p);
    s->cur_us = vts;
    if (!s->is3d) {
        if (okL) { g_y2r_start(s->fL, &r3_texL[slot], s->W, s->H); g_y2r_wait(&r3_texL[slot]);
                   r3_rts[slot] = dpair * s->pair_dur; r3_bts[slot] = vts;
                   __sync_synchronize(); r3_ready[slot] = 1; s->wr = s->wr + 1; }
    } else if (r3_vqc > 0) {
        int64_t vts2; p = r3_vq_pop(&n, &vts2, &kf); ap.data = p; ap.size = n;
        int gotR = 0; int okR = (mobi_decode(s->ctx, s->fR, &gotR, &ap) >= 0 && gotR); free(p);
        if (okL && okR) {
            /* eye routing: normally (first,second) = (L,R); after a seek that landed on a RIGHT
             * eye (pairs span block boundaries in some Nintendo muxes) the stream runs R,L,R,L --
             * route the first frame to the R texture so depth stays correct. */
            C3D_Tex *tA = s->eye_swap ? &r3_texR[slot] : &r3_texL[slot];
            C3D_Tex *tB = s->eye_swap ? &r3_texL[slot] : &r3_texR[slot];
            g_y2r_start(s->fL, tA, s->W, s->H); g_y2r_wait(tA);
            g_y2r_start(s->fR, tB, s->W, s->H); g_y2r_wait(tB);
            r3_rts[slot] = dpair * s->pair_dur; r3_bts[slot] = vts;
            __sync_synchronize(); r3_ready[slot] = 1; s->wr = s->wr + 1;   /* publish AFTER the data is ready */
        }
    }
    { long long _pd = svcGetSystemTick() - _pt; s->wk_dec += _pd; if (_pd > s->wk_dmax) s->wk_dmax = _pd; s->wk_n++; }
    s->dpair = dpair + 1;
}

static void r3_dec_thread(void *arg) {
    R3S *s = (R3S *)arg;
    while (s->run) {
        LightLock_Lock(&s->lock);
        int wait = s->paused || g_ring_susp || (s->wr - s->rd >= s->NB - 1) || (s->done && r3_vqc == 0);
        if (!wait) r3_produce(s);
        LightLock_Unlock(&s->lock);
        if (wait) LightEvent_Wait(&s->wake);   /* nothing to do -> sleep until main pokes us */
    }
}

static MoflexResult moflex_play_ring(const char *path) {
    vol_load();
    FILE *f = fopen(path, "rb");
    if (!f) return MOFLEX_ERROR;
    MfxDemux m;
    if (mfx_open_auto(&m, f, path) != 0) { fclose(f); return MOFLEX_ERROR; }
    int vi = -1, ai = -1;
    for (int i = 0; i < m.nb_streams; i++) {
        if (m.streams[i].media_type == MFX_TYPE_VIDEO && vi < 0) vi = i;
        if (m.streams[i].media_type == MFX_TYPE_AUDIO && ai < 0) ai = i;
    }
    if (vi < 0) { mfx_close(&m); fclose(f); return MOFLEX_ERROR; }
    int W = m.streams[vi].width, H = m.streams[vi].height;
    int arate = ai >= 0 ? m.streams[ai].sample_rate : 44100, chn = ai >= 0 ? m.streams[ai].channels : 2;
    int have_audio = (ai >= 0);
    int is3d = mfx_detect_stereo(&m);

    int64_t pair_dur = 40000;   /* per-displayed-frame period from the stream timebase */
    if (m.streams[vi].tb_den > 0) {
        int64_t pd = (int64_t)m.streams[vi].tb_num * 1000000 / m.streams[vi].tb_den;
        if (pd >= 16000 && pd <= 100000) pair_dur = pd;
    }
    if (is3d && m.tb_is_eye) pair_dur *= 2;   /* Nintendo declares the EYE rate: a pair lasts 2x */

    AVCodecContext ctx; memset(&ctx, 0, sizeof ctx);
    ctx.width = W; ctx.height = H; ctx.priv_data = calloc(1, mobi_ctx_size());
    if (!ctx.priv_data || mobi_init(&ctx) != 0) { free(ctx.priv_data); mfx_close(&m); fclose(f); return MOFLEX_ERROR; }
    mobi_opt = 0x1BDA5E;   /* shipped fast path + missing-ref tolerance + SIMD residual add (0x40). NOTE: no pipeline --
                            * the decode THREAD gives the headroom the New-3DS pipeline used to, and the
                            * threaded producer decodes plain sequential frames (no L/R lag bookkeeping). */
    bool r3_isnew = false; APT_CheckNew3DS(&r3_isnew);
    /* 24-bit color (RGBA8) on New-3DS to kill gradient banding; 16-bit (RGB565) on Old-3DS where the
     * doubled texture size would halve the decode-ahead cushion. */
    int r3_bpp = r3_isnew ? 4 : 2;
    GPU_TEXCOLOR r3_tf = r3_isnew ? GPU_RGBA8 : GPU_RGB565;
    g_ring_apt_bpp = r3_bpp;
    AVFrame *fL = av_frame_alloc(), *fR = av_frame_alloc();

    const char *bn = strrchr(path, '/'); bn = bn ? bn + 1 : path;
    char title[64];
    const char *cia_title = cia_selection_title(path);
    if (cia_title) snprintf(title, sizeof title, "%.60s", cia_title);
    else {
        snprintf(title, sizeof title, "%.60s", bn);
        size_t L = strlen(title);
        if (L > 7 && !strcasecmp(title + L - 7, ".moflex")) title[L - 7] = 0;
        else if (L > 4 && !strcasecmp(title + L - 4, ".zip")) title[L - 4] = 0;
        else if (L > 4 && !strcasecmp(title + L - 4, ".cia")) title[L - 4] = 0;
    }
    subs_autoload(path);

    /* CLEAN gfx re-init, exactly like the standalone avtest that renders smoothly on Old 3DS. The app
     * leaves the screens configured for its SOFTWARE UI (console on the bottom + a custom framebuffer
     * format); citro2d rendering into that garbled state is what produced the comb/doubled screens.
     * Tear it all down and gfxInitDefault() fresh so citro2d owns a known-good state (the app re-inits
     * its own console after moflex_play() returns, so this is safe). Restored the same way on exit. */
    gfxExit();
    gfxInitDefault();
    /* GPU setup. On the Old 3DS the (unpinned) linear heap can be too small for citro3d's command
     * buffer or the screen targets -- and a FAILED C3D_Init followed by C2D drawing is a write to an
     * unmapped target (the "data abort / write / translation section" crash). Check every step and
     * fall back to the classic (no-GPU) path instead of faulting. */
    gfxSet3D(is3d);
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) {
        gfxSet3D(false); av_frame_free(&fL); av_frame_free(&fR);
        mobi_close(&ctx); free(ctx.priv_data); mfx_close(&m); fclose(f);
        return MOFLEX_FALLBACK;
    }
    if (!C2D_Init(C2D_DEFAULT_MAX_OBJECTS)) {   /* unchecked before: silent no-op draws = dark screens */
        C3D_Fini();
        gfxSet3D(false); av_frame_free(&fL); av_frame_free(&fR);
        mobi_close(&ctx); free(ctx.priv_data); mfx_close(&m); fclose(f);
        return MOFLEX_FALLBACK;
    }
    C2D_Prepare();
    C3D_RenderTarget *topL = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget *topR = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);
    C3D_RenderTarget *bot  = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    if (!topL || !topR || !bot) {
        C2D_Fini(); C3D_Fini(); gfxSet3D(false);
        av_frame_free(&fL); av_frame_free(&fR);
        mobi_close(&ctx); free(ctx.priv_data); mfx_close(&m); fclose(f);
        return MOFLEX_FALLBACK;
    }

    /* services for the panel controls (same as the classic path): battery level + bottom-screen backlight */
    ptmuInit(); g_mcu_ok = R_SUCCEEDED(mcuHwcInit()); g_batt_pct = -1; g_batt_next = 0;
    g_lcd_ok = R_SUCCEEDED(gspLcdInit()); g_screen_off = 0;

    /* grow the pair ring until the linear heap is nearly spent (reserve audio bank + GPU slack) */
    Tex3DS_SubTexture sub = { (u16)W, (u16)H, 0.0f, 1.0f, (float)W / GT_W, 1.0f - (float)H / GT_H };
    const size_t R3_RESERVE = (size_t)(R3_AWB * R3_ABUF * chn * 2) + (2u << 20);
    int NB = 0;
    for (; NB < R3_NB_MAX; NB++) {
        if (linearSpaceFree() < (size_t)(2 * GT_W * GT_H * r3_bpp) + R3_RESERVE) break;
        if (!C3D_TexInit(&r3_texL[NB], GT_W, GT_H, r3_tf)) break;
        if (!C3D_TexInit(&r3_texR[NB], GT_W, GT_H, r3_tf)) { C3D_TexDelete(&r3_texL[NB]); break; }
        C3D_TexSetFilter(&r3_texL[NB], GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetFilter(&r3_texR[NB], GPU_LINEAR, GPU_LINEAR);
        r3_imgL[NB] = (C2D_Image){ &r3_texL[NB], &sub }; r3_imgR[NB] = (C2D_Image){ &r3_texR[NB], &sub };
    }
    /* Not enough linear heap for a usable ring on this console -> tear down and let the caller run the
     * classic path (which needs no GPU ring). This is what prevented the `wr % NB` divide-by-zero fault. */
    if (NB < 2) {
        for (int i = 0; i < NB; i++) { C3D_TexDelete(&r3_texL[i]); C3D_TexDelete(&r3_texR[i]); }
        C2D_Fini(); C3D_Fini();
        gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES); gfxSetScreenFormat(GFX_BOTTOM, GSP_RGB565_OES); gfxSet3D(false);
        av_frame_free(&fL); av_frame_free(&fR);
        mobi_close(&ctx); free(ctx.priv_data); mfx_close(&m); fclose(f);
        return MOFLEX_FALLBACK;
    }
    g_y2r_init(W, H, r3_bpp);
    if (have_audio) { r3_audio_setup(arate, chn); if (!r3_aok) have_audio = 0; }   /* alloc failed -> wall-clock */
    /* Register our suspend hook: the libctru default only saves the GPU, but we also hold Y2R,
     * gsp::Lcd (moon), and an ndsp channel -- keeping those across a HOME/sleep transition hard-locks
     * the GSP (needs a power-off). The hook hands them back on suspend and re-acquires on restore. */
    g_ring_apt_w = W; g_ring_apt_h = H; g_ring_apt_bpp = r3_bpp;
    g_ring_apt_audio = have_audio; g_ring_apt_playing = 1;
    aptHook(&g_ring_apt_cookie, mp_ring_apt_hook, NULL);

    C2D_TextBuf sbuf = C2D_TextBufNew(256), tmbuf = C2D_TextBufNew(48);
    C2D_Text ttitle, thint, ttime;
    C2D_TextParse(&ttitle, sbuf, title); C2D_TextOptimize(&ttitle);
    C2D_TextParse(&thint, sbuf, "A pause  <>seek  ^v vol  SELECT subs  B back"); C2D_TextOptimize(&thint);
    C2D_TextParse(&ttime, tmbuf, " "); C2D_TextOptimize(&ttime);
    C2D_TextBuf subbuf = C2D_TextBufNew(512);
    u32 black = C2D_Color32(0, 0, 0, 255), subcol = C2D_Color32(255, 255, 255, 255), subout = C2D_Color32(0, 0, 0, 255);

    MoflexResult result = MOFLEX_QUIT_BACK;
    /* ring: wr = pairs banked (monotonic), rd = pairs presented; slot = idx % NB.
     * rts[slot] = that pair's content-time (us) for the audio-master present. */
    int rd = 0;
    int *ready = r3_ready; int64_t *rts = r3_rts, *bts = r3_bts;   /* off-stack (see decl) */
    memset(ready, 0, sizeof r3_ready);
    int64_t last_bts = 0;
    int playing = 1, gated = 0, last_shown = -1, dirty = 1;
    const int PRIME = (NB >= 16) ? 8 : (NB > 2 ? NB / 2 : 1);   /* pre-roll depth, clamped to a small ring */
    int64_t cur_us = 0, dur_us = m.duration_us, seek_to_us = 0; int want_seek = 0, shold = 0;
    int scrubbing = 0; int64_t scrub_us = 0;   /* touch-drag on the bar: preview while held, seek on release */
    int vol_drag = 0;                           /* touch-drag on the volume slider (applies live) */
    g_submenu = 0; g_sub_sel = 0;               /* subtitle menu starts closed */
    g_touch_locked = 0; g_lock_toast = 0; g_backlight_on = 1;   /* touch unlocked, backlight on */
    int64_t rpos = resume_load(path), last_save = 0;
    if (rpos > 3000000 && (dur_us <= 0 || rpos < dur_us - 10000000)) { seek_to_us = rpos; want_seek = 1; last_save = rpos; }
    int vol_dirty = 0, save_pend = 0, save_delay = 0;
    char last_ts[64] = "", last_sub[256] = "";
    u64 r3_wall0 = 0; int r3_wallset = 0;                 /* fallback real-time clock when no audio */
    u64 pause_wall = 0;                                   /* when the pause began (clock re-anchor on resume) */
    C2D_Text tsub; int sub_valid = 0;                     /* cached parsed cue (re-parsed only on change) */

    /* profiler + CADENCE METER (v2/v3 = frames held 2/3 refreshes = good; bad = held 1 or 4+ = judder) */
    u64 pf_dec = 0, pf_y2r = 0, pf_gpu = 0, pf_idle = 0, pf_rd = 0, pf_dmax = 0, pf_rdmax = 0; int pf_n = 0; char pf_str[80] = "";
    #define PF_MS(t) ((double)(t) * 1000.0 / SYSCLOCK_ARM11)
    u64 cad_last = 0; int cad_v2 = 0, cad_v3 = 0, cad_vx = 0;
    const double VS_MS = 1000.0 / 59.834;   /* 3DS top-screen refresh period (~16.71ms) */

    /* ---- start the DECODE THREAD (producer). It fills the ring; this (main) thread only presents. ---- */
    R3S s; memset(&s, 0, sizeof s);
    s.m = &m; s.ctx = &ctx; s.fL = fL; s.fR = fR;
    s.W = W; s.H = H; s.is3d = is3d; s.NB = NB; s.have_audio = have_audio;
    s.pair_dur = pair_dur; s.dur_us = dur_us; s.run = 1;
    s.paused = want_seek ? 1 : 0;   /* if resuming, stay paused until the seek/prime below sets us up */
    LightLock_Init(&s.lock); LightEvent_Init(&s.wake, RESET_ONESHOT);
    /* THREAD ONLY ON NEW-3DS. Old-3DS is memory-bandwidth-bound (proven): a second core decoding while
     * the main core presents just contends on the bus and DOUBLES decode time (d52 -> d91). So Old-3DS
     * decodes INLINE on the main thread (single-threaded, like before) -- no worker. */
    int threaded = r3_isnew;
    Thread dthr = 0;
    if (threaded) {
        s32 dprio = 0x30; svcGetThreadPriority(&dprio, CUR_THREAD_HANDLE);
        dthr = threadCreate(r3_dec_thread, &s, 128 * 1024, dprio - 1, 2, false);   /* free core 2 on New-3DS */
        if (!dthr) threaded = 0;   /* couldn't create the thread -> fall back to inline decode */
    }
    g_ring_susp = 0;
    g_ring_lock = threaded ? &s.lock : NULL;   /* the suspend hook drains the worker through these */
    g_ring_wake = threaded ? &s.wake : NULL;
    #define WORKER_LOCK()   do { if (threaded) LightLock_Lock(&s.lock); } while (0)   /* pause the worker for a seek */
    #define WORKER_UNLOCK() do { if (threaded) { LightLock_Unlock(&s.lock); LightEvent_Signal(&s.wake); } } while (0)

    /* paint a clean loading screen NOW: the ring build + pre-roll take a few seconds on Old 3DS, and
     * without this the freshly gfxInitDefault'd framebuffers show garbage (the "corrupt bottom, looks
     * hung on open"). Black video + the UI panel so it reads as loading, not crashed. */
    for (int fpre = 0; fpre < 2; fpre++) {   /* both back buffers */
        C3D_FrameBegin(0);
        C2D_TargetClear(topL, black); C2D_SceneBegin(topL);
        C2D_TargetClear(topR, black); C2D_SceneBegin(topR);
        g_panel_sw(bot, title, 0, dur_us, 1);   /* release-style panel via texture (matches the shipped UI) */
        C3D_FrameEnd(0);
    }

    while (aptMainLoop()) {
        hidScanInput();
        u32 kd = hidKeysDown(), kh = hidKeysHeld();
        touchPosition tp; hidTouchRead(&tp);
        g_ring_apt_playing = playing;   /* so the suspend hook resumes audio only if we were playing */

        /* L toggles the touch lock (buttons always work; only touch is gated). */
        if (kd & KEY_L) { g_touch_locked = !g_touch_locked; g_lock_toast = 0; dirty = 1; }

        /* Dark bottom screen: ANY button wakes it; touching wakes it when unlocked, or (when locked)
         * flashes the "touch locked" message. Either way a touch on the dark screen NEVER reaches the
         * (invisible) controls. */
        if (g_screen_off) {
            if (kd & ~KEY_TOUCH) { g_screen_off = 0; dirty = 1; kd = 0; }   /* a button woke it (consumed) */
            else if (kd & KEY_TOUCH) {
                if (g_touch_locked) g_lock_toast = 90;   /* stay dark; flash the message (backlight below) */
                else g_screen_off = 0;                   /* touch woke it */
                dirty = 1;
            }
            kd &= ~KEY_TOUCH; kh &= ~KEY_TOUCH;          /* touch on a dark screen drives no control */
        }
        else if (g_touch_locked) {                       /* lit + locked: touch only shows the message */
            if (kd & KEY_TOUCH) { g_lock_toast = 90; dirty = 1; }
            kd &= ~KEY_TOUCH; kh &= ~KEY_TOUCH;
        }

        if (g_lock_toast > 0) { g_lock_toast--; dirty = 1; }

        /* reconcile the physical backlight: on unless the user darkened it -- and even then, briefly on
         * to show the touch-locked toast so they can read why touch isn't waking it. */
        if (g_lcd_ok) {
            int on = (!g_screen_off) || (g_lock_toast > 0);
            if (on != g_backlight_on) { if (on) GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM);
                                        else GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTTOM); g_backlight_on = on; }
        }

        if (g_submenu) {                       /* subtitle menu owns input while open; movie plays on */
            submenu_input(kd, kh, tp, is3d, path);
            dirty = 1;
        } else {
        if (kd & KEY_B) { result = MOFLEX_QUIT_BACK; break; }
        if (kd & KEY_A) {
            if (s.done && (s.wr - rd) == 0) { seek_to_us = 0; want_seek = 1; }   /* at the end: replay */
            else { playing = !playing; if (have_audio) ndspChnSetPaused(0, !playing);
                   if (!playing) pause_wall = osGetTime();
                   else { r3_last = 0; if (r3_wallset) r3_wall0 += osGetTime() - pause_wall; }
                   s.paused = !playing; if (playing) LightEvent_Signal(&s.wake);
                   save_pend = !playing; save_delay = 30; }
        }
        if (kd & KEY_UP)   { int s = (int)(g_vol / 0.25f + 0.0001f); g_vol = (s + 1) * 0.25f; if (g_vol > 4.0f) g_vol = 4.0f; vol_dirty = 1; }
        if (kd & KEY_DOWN) { int s = (int)(g_vol / 0.25f + 0.9999f); g_vol = (s - 1) * 0.25f; if (g_vol < 0.25f) g_vol = 0.25f; vol_dirty = 1; }
        if (kd & KEY_Y)      { g_sub_on = !g_sub_on; dirty = 1; }   /* Y: quick-toggle subtitles */
        if (kd & KEY_SELECT) { g_submenu = 1; g_sub_sel = 0; dirty = 1; }   /* SELECT: open subtitle options */
        if (kd & KEY_X && g_lcd_ok) { g_screen_off = !g_screen_off; dirty = 1; }   /* X: toggle dark screen */
        {   int sdir = (kh & KEY_RIGHT) ? 1 : ((kh & KEY_LEFT) ? -1 : 0);
            if (sdir == 0) shold = 0;
            else { int fire = (kd & (KEY_RIGHT | KEY_LEFT)) ? 1 : 0;
                   if (!fire) { shold++; if (shold > 8 && (shold % 3 == 0)) fire = 1; }
                   /* ACCUMULATE the target while held (relative to the pending seek, not the playing
                    * cur_us) -- the actual seek fires on RELEASE below, so holding flies backward in
                    * 30s steps and lands once instead of oscillating (seek->play->seek). */
                   if (fire) { seek_to_us = (want_seek ? seek_to_us : cur_us) + (int64_t)sdir * 30000000;
                               want_seek = 1; } } }
        /* NOTE: sub_menu() renders via the app's SOFTWARE framebuffer UI (ui_begin/ui_present ->
         * gfxSwapBuffers), which deadlocks against citro2d owning the GPU here -> the app hangs.
         * Disabled in the ring path for now; subtitles still auto-load and draw (r3_draw_sub). A
         * citro2d subtitle menu (or teardown/re-init around sub_menu) is the proper follow-up. */
        /* if (kd & KEY_SELECT) sub_menu(...);  // <- hangs under citro2d; do not call here */
        if (kd & KEY_TOUCH) {
            int px = tp.px, py = tp.py;
            if (py >= BAR_Y - 8 && py <= BAR_Y + BAR_H + 8 && px >= BAR_X && px <= BAR_X + BAR_W) {
                scrubbing = 1;   /* begin drag; the seek fires on release (tracked in the scrub block below) */
            } else if (px >= VOL_X - 14 && px <= VOL_X + 26 && py >= VOL_Y - 8 && py <= VOL_Y + VOL_H + 8) {
                vol_drag = 1;    /* begin volume drag; applies live (tracked in the vol block below) */
            } else if (py >= PLAY_CY - 20 && py <= PLAY_CY + 20 && px >= PLAY_CX - 20 && px <= PLAY_CX + 20) {
                playing = !playing; if (have_audio) ndspChnSetPaused(0, !playing);
                if (!playing) pause_wall = osGetTime();
                else { r3_last = 0; if (r3_wallset) r3_wall0 += osGetTime() - pause_wall; }
                s.paused = !playing; if (playing) LightEvent_Signal(&s.wake); save_pend = !playing; save_delay = 30;
            } else if (py >= PLAY_CY - 20 && py <= PLAY_CY + 20 && px >= RW_CX - 18 && px <= RW_CX + 18) {
                seek_to_us = cur_us - 30000000; want_seek = 1;
            } else if (py >= PLAY_CY - 20 && py <= PLAY_CY + 20 && px >= FF_CX - 18 && px <= FF_CX + 18) {
                seek_to_us = cur_us + 30000000; want_seek = 1;
            } else if (px >= CC_X && px < CC_X + CC_W && py >= CC_Y && py < CC_Y + CC_H) {
                g_submenu = 1; g_sub_sel = 0; dirty = 1;   /* CC: open the subtitle options menu */
            } else if (g_lcd_ok && px >= DIM_X && px < DIM_X + DIM_W && py >= DIM_Y && py < DIM_Y + DIM_H) {
                g_screen_off = 1; dirty = 1;   /* moon: darken (a button or touch wakes it; reconcile handles backlight) */
            } else if (py >= BTN_Y && py < BTN_Y + BTN_H) {
                if      (px >= BKB_X && px < BKB_X + BKB_W) { result = MOFLEX_QUIT_OPEN; break; }   /* OPEN VIDEO */
                else if (px >= OPB_X && px < OPB_X + OPB_W) { result = MOFLEX_QUIT_MANAGE; break; }   /* MANAGE VIDEOS */
                else if (px >= EXB_X && px < EXB_X + EXB_W) { result = MOFLEX_QUIT_ADD; break; }   /* ADD VIDEO */
            }
        }

        /* ---- touch-drag the progress bar: while the finger is down, track its X into scrub_us and
         *      show it as a live preview (below); the actual seek fires once, on release. ---- */
        if (scrubbing) {
            if (kh & KEY_TOUCH) {
                int px = tp.px; if (px < BAR_X) px = BAR_X; if (px > BAR_X + BAR_W) px = BAR_X + BAR_W;
                scrub_us = (int64_t)((double)(px - BAR_X) / BAR_W * (dur_us > 0 ? dur_us : 0));
                dirty = 1;
            } else {   /* released -> seek to where they let go and resume playing there */
                seek_to_us = scrub_us; want_seek = 1; scrubbing = 0;
            }
        }

        /* ---- touch-drag the vertical volume slider: top = 400%, bottom = 25%, snapped to 25% steps,
         *      applied live (no release needed). ---- */
        if (vol_drag) {
            if (kh & KEY_TOUCH) {
                float f = 1.0f - (float)(tp.py - VOL_Y) / VOL_H; if (f < 0) f = 0; if (f > 1) f = 1;
                int st = (int)(f * 4.0f / 0.25f + 0.5f); float nv = st * 0.25f;
                if (nv < 0.25f) nv = 0.25f; if (nv > 4.0f) nv = 4.0f;
                if (nv != g_vol) { g_vol = nv; vol_dirty = 1; }
                dirty = 1;
            } else vol_drag = 0;
        }
        }   /* end transport input (skipped while the subtitle menu is open) */

        /* ---- seek: pause the worker (own demux+decoder), reset ring/demux/audio, PRIME the landing
         *      frame into slot 0, then resume. Held D-pad only accumulates the target; fires on release. ---- */
        if (want_seek && !(kh & (KEY_LEFT | KEY_RIGHT))) {
            WORKER_LOCK();   /* blocks until the decode thread finishes its current pair and releases */
            if (dur_us > 0) { if (seek_to_us < 0) seek_to_us = 0; if (seek_to_us > dur_us) seek_to_us = dur_us; }
            do_seek(&m, &ctx, seek_to_us);
            for (int i = rd; i < s.wr; i++) ready[i % NB] = 0;
            s.wr = 0; rd = 0; s.rd = 0; s.dpair = 0; s.skipping = 0; s.done = 0; s.has_pending = 0; last_shown = -1;
            r3_vq_clear();
            if (have_audio) r3_audio_flush();
            gated = 0; s.audio_pre_full = 0; playing = 1; r3_wallset = 0;
            cur_us = seek_to_us; s.cur_us = seek_to_us;
            /* 3D EYE PARITY: pairs can span block boundaries (Nintendo muxes), so a marker landing
             * starts on a RIGHT eye about half the time -- nothing in the packets says which. But
             * block arithmetic does: each block's packet count vs its ts span shifts the phase by
             * (count - span); since the phase is only ever 0 or 1, the first nonzero step resolves
             * the landing's eye exactly (validated 50/50 landings, 0 wrong, on movie.moflex).
             * Peek forward counting blocks, then re-seek to the landing for the real prime. */
            s.eye_swap = 0;
            if (is3d) {
                /* Judge each block AS IT ARRIVES under BOTH timebase conventions (Nintendo tb =
                 * EYE period; our encoder tb = PAIR period), and stop the instant both die or a
                 * verdict lands. A candidate is trusted only if every span sits exactly on its
                 * frame grid (eps 0.02; ms-rounded muxes drift and are rejected) with steps in
                 * -1..+1; the first exact +-1 pins the landing parity, anything else = no swap.
                 * Tight caps: our files kill both candidates after ONE block (~100 packets), so
                 * this can't stall the load like the old 20000-packet collection did.
                 * Validated (pc_verify/test_stereo11.c): movie.moflex 50/50 landings correct,
                 * our encodes 0 false swaps, max 173 packets peeked. */
                /* eye period is uniformly pair_dur/2 now that tb_is_eye corrects the pacing */
                MfxPacket pk; int64_t bt = m.ts; long cnt = 0; int nb = 0;
                int okc = 1, decided = 0;
                double eye = (double)pair_dur * 0.5;
                for (long g = 0; g < 1500 && nb < 4 && !decided && okc; g++) {
                    if (mfx_next_packet(&m, &pk) != 1) break;
                    if (m.streams[pk.stream_index].media_type != MFX_TYPE_VIDEO) continue;
                    if (m.ts != bt) {
                        double fx = (double)(m.ts - bt) / eye;
                        long F = (long)(fx + 0.5);
                        double d = fx - (double)F; if (d < 0) d = -d;
                        if (d > 0.02) okc = 0;                    /* inexact grid (ms-rounded mux) -> never swap */
                        else {
                            int step = (int)(cnt - F);
                            if (step == 1 || step == -1) { if (step == -1) s.eye_swap = 1; decided = 1; }
                            else if (step != 0) okc = 0;          /* wild step -> distrust */
                        }
                        nb++; bt = m.ts; cnt = 0;
                    }
                    cnt++;
                }
                do_seek(&m, &ctx, seek_to_us);   /* rewind to the landing for the real prime */
            }
            /* FAST PRIME: decode the landing keyframe pair straight into slot 0 (bounded keyframe scan;
             * else take the next decodable frame) so the picture appears at once and audio resyncs.
             * Eye routing honors s.eye_swap (first frame -> R when the landing starts on a right eye). */
            {
                MfxPacket pk; int64_t lts = seek_to_us; int vseen = 0;
                for (int guard = 0; guard < 4000; guard++) {
                    if (mfx_next_packet(&m, &pk) != 1) break;
                    if (m.streams[pk.stream_index].media_type != MFX_TYPE_VIDEO) continue;
                    vseen++;
                    /* 3D: a pair may only START at an EVEN offset from the landing -- that keeps the
                     * stream's eye phase (which s.eye_swap already resolved). Files with keyframes on
                     * RIGHT eyes (community encodes keyframe both eyes) otherwise made the prime start
                     * a pair on an R: reversed depth + left eye one frame behind, randomly per seek. */
                    if (is3d && !(vseen & 1)) continue;
                    if (!pk.keyframe && vseen < 30) continue;
                    lts = m.ts;
                    AVPacket ap; ap.data = pk.data; ap.size = pk.size; int gL = 0;
                    if (!(mobi_decode(&ctx, fL, &gL, &ap) >= 0 && gL)) continue;
                    if (is3d) {
                        int haveR = 0; MfxPacket pr;
                        for (int g2 = 0; g2 < 8; g2++) { if (mfx_next_packet(&m, &pr) != 1) break;
                            if (m.streams[pr.stream_index].media_type == MFX_TYPE_VIDEO) { haveR = 1; vseen++; break; } }
                        int gR = 0;
                        if (!haveR || !(mobi_decode(&ctx, fR, &gR, &(AVPacket){ .data = pr.data, .size = pr.size }) >= 0 && gR)) continue;
                        C3D_Tex *tA = s.eye_swap ? &r3_texR[0] : &r3_texL[0];
                        C3D_Tex *tB = s.eye_swap ? &r3_texL[0] : &r3_texR[0];
                        g_y2r_start(fL, tA, W, H); g_y2r_wait(tA);
                        g_y2r_start(fR, tB, W, H); g_y2r_wait(tB);
                    } else { g_y2r_start(fL, &r3_texL[0], W, H); g_y2r_wait(&r3_texL[0]); }
                    rts[0] = 0; bts[0] = lts; ready[0] = 1; s.wr = 1; s.dpair = 1; cur_us = lts; s.cur_us = lts;
                    break;
                }
            }
            s.paused = 0;
            WORKER_UNLOCK();   /* resume the decode thread from the new position */
            want_seek = 0;
        }

        if (kd) dirty = 1;   /* any control input -> repaint the held frame / panel */

        if (have_audio && playing) r3_audio_poll();   /* clock FROZEN while paused -> pause is instant */

        /* Old-3DS (not threaded): decode a pair INLINE here, once per iteration, when there's ring room.
         * (New-3DS does this on the decode thread instead.) */
        int inline_produced = 0;
        if (!threaded && playing && !s.done && (s.wr - rd) < NB - 1) { r3_produce(&s); inline_produced = 1; }

        /* pre-roll: unpause audio once enough pairs are banked, or the worker's audio bank filled first */
        if (have_audio && !gated && playing && (s.wr >= PRIME || s.audio_pre_full)) { gated = 1; ndspChnSetPaused(0, false); r3_apos = 0; }

        int wr = s.wr; __sync_synchronize();   /* snapshot the write index, then see the frame data behind it */
        int64_t apos;
        if (have_audio) apos = r3_apos;
        else { if (!r3_wallset && wr >= PRIME) { r3_wall0 = osGetTime(); r3_wallset = 1; }
               apos = r3_wallset ? (int64_t)(osGetTime() - r3_wall0) * 1000 : -1; }

        /* ---- present (audio-master): drop stale banked pairs (content-time already passed), show the due
         *      pair. rts and apos share origin 0 (content-0 at start, seek-pos after a seek), so compare
         *      directly. Both eyes in ONE C3D frame -> atomic. ---- */
        /* Present each frame at the NEAREST refresh to its content time, not the first refresh after it:
         * bias the compare by half a refresh (~8.35ms) so a frame due mid-refresh rounds to the closer
         * VSync instead of always the next one -- removes the occasional over-hold blip. */
        const int64_t VS_HALF = 8356;
        int show = -1;
        if (playing && (wr - rd) > 0 && ready[rd % NB]) {
            /* Capture the A/V anchor ONCE, the first frame after audio re-locks: right now bts-rts equals
             * the true video origin (before any cadence drift). skew = audio_origin - video_origin, held
             * constant thereafter and ignored if implausibly large (bad/absent timestamps). */
            if (have_audio && !r3_av_ready && apos >= 0 && r3_audio_t0 >= 0) {
                int64_t sk = r3_audio_t0 - (bts[rd % NB] - rts[rd % NB]);
                r3_av_skew = (sk < -300000 || sk > 300000) ? 0 : sk;
                r3_av_ready = 1;
            }
            int64_t ae = (have_audio && apos >= 0) ? apos + r3_av_skew : apos;
            while ((wr - rd) > 1 && ready[(rd + 1) % NB] && rts[(rd + 1) % NB] <= ae + VS_HALF) { ready[rd % NB] = 0; rd++; }
            if (apos < 0) { if (last_shown < 0) show = rd % NB; }             /* pre-roll: show the landing pair */
            else if (ae + VS_HALF >= rts[rd % NB]) show = rd % NB;
        }

        /* ---- draw: video pair (or held frame) + subtitle overlay + bottom panel, one GPU frame ---- */
        int64_t disp_us = (show >= 0) ? bts[show] : (last_shown >= 0 ? last_bts : cur_us);
        if (want_seek && (kh & (KEY_LEFT | KEY_RIGHT))) { disp_us = seek_to_us < 0 ? 0 : seek_to_us; dirty = 1; }  /* preview the accumulating seek target */
        if (scrubbing) { disp_us = scrub_us; dirty = 1; }   /* preview the dragged position while the finger is down */
        const char *cue = subs_active(disp_us);   /* NULL if subs off or no cue now */
        char tc[16], td[16], ts[64];
        fmt_time(disp_us, tc, sizeof tc);
        fmt_time(dur_us, td, sizeof td);
        snprintf(ts, sizeof ts, "%s / %s", tc, td);   /* clean current / duration (debug HUD removed) */
        (void)pf_str;
        if (strcmp(ts, last_ts)) { snprintf(last_ts, sizeof last_ts, "%s", ts);
                                   C2D_TextBufClear(tmbuf); C2D_TextParse(&ttime, tmbuf, ts); C2D_TextOptimize(&ttime); }
        if (cue && *cue) {                          /* (re)parse only when the cue text changes */
            if (!sub_valid || strcmp(cue, last_sub)) {
                snprintf(last_sub, sizeof last_sub, "%s", cue);
                C2D_TextBufClear(subbuf); C2D_TextParse(&tsub, subbuf, cue); C2D_TextOptimize(&tsub); sub_valid = 1;
            }
        } else { sub_valid = 0; last_sub[0] = 0; }

        /* Draw ONLY when a new pair is due (show>=0) or the UI changed (dirty). Between presents the
         * loop keeps decoding at full speed to BANK ahead -- drawing every iteration would peg the loop
         * to the ~60Hz GPU pace and kill the calm-scene surplus that lets busy scenes coast. */
        if ((show >= 0 || dirty) && (show >= 0 || last_shown >= 0)) {
            int b = (show >= 0) ? show : last_shown;
            C3D_FrameBegin(0);
            C2D_TargetClear(topL, black); C2D_SceneBegin(topL);
            C2D_DrawImageAt(r3_imgL[b], 0, 0, 0, NULL, 1, 1);
            if (sub_valid) r3_draw_sub(&tsub, is3d ? -g_sub_depth : 0, subcol, subout);
            C2D_TargetClear(topR, black); C2D_SceneBegin(topR);
            C2D_DrawImageAt(is3d ? r3_imgR[b] : r3_imgL[b], 0, 0, 0, NULL, 1, 1);
            if (sub_valid) r3_draw_sub(&tsub, is3d ? g_sub_depth : 0, subcol, subout);
            if (g_screen_off) g_dark_sw(bot);                    /* dark: black + toast only, never the panel */
            else if (g_submenu) g_submenu_sw(bot, is3d);         /* subtitle options (movie keeps playing) */
            else g_panel_sw(bot, title, disp_us, dur_us, playing);   /* panel (lock toast on top) */
            u64 _tg = svcGetSystemTick(); C3D_FrameEnd(0); pf_gpu += svcGetSystemTick() - _tg;
            dirty = 0;
            if (show >= 0) {
                u64 nt = svcGetSystemTick();   /* cadence: refreshes this newly-shown frame replaced */
                if (cad_last) { int v = (int)((double)(nt - cad_last) * 1000.0 / SYSCLOCK_ARM11 / VS_MS + 0.5);
                                if (v == 2) cad_v2++; else if (v == 3) cad_v3++; else cad_vx++; }
                cad_last = nt;
                ready[show] = 0; last_shown = show; last_bts = bts[show]; cur_us = bts[show]; rd++;
            }
        }
        if (s.rd != rd) { s.rd = rd; LightEvent_Signal(&s.wake); }   /* freed slot(s) -> wake the decode thread */

        /* ---- deferred resume/volume checkpoint (off the keypress; written while idle) ---- */
        if (save_pend && --save_delay <= 0) {
            if (cur_us != last_save) { resume_save_us(path, cur_us); last_save = cur_us; }
            if (vol_dirty) { vol_save(); vol_dirty = 0; }
            save_pend = 0;
        }
        if (vol_dirty && !save_pend) { vol_save(); vol_dirty = 0; }

        /* movie finished: STOP in the player (last frame held, bar parked at the end) instead of
         * exiting as if B was pressed. A replays from the start; B leaves as usual. */
        if (s.done && (s.wr - rd) == 0 && playing) {
            playing = 0; s.paused = 1;
            if (have_audio) ndspChnSetPaused(0, true);
            resume_clear(path);                      /* finished -> no resume point */
            if (dur_us > 0) cur_us = dur_us;
            dirty = 1;
        }
        /* No banking on this thread now: when no new frame is due (and no UI change), pace to VBlank so
         * the present-due check runs once per refresh -> steady cadence. Present iterations (show>=0)
         * skip this, so C3D_FrameEnd is never double-waited. */
        if (show < 0 && !dirty && !inline_produced) { u64 _ti = svcGetSystemTick(); gspWaitForVBlank(); pf_idle += svcGetSystemTick() - _ti; pf_n++; }

        /* Roll the HUD every ~24 presented frames. CADENCE is the judder meter: v2/v3 = frames held 2/3
         * refreshes (good), bad = held 1 or 4+ (judder). d/D = worker decode avg/max ms; i = main idle. */
        if (cad_v2 + cad_v3 + cad_vx >= 24) {
            int wn = s.wk_n > 0 ? s.wk_n : 1;
            snprintf(pf_str, sizeof pf_str, " v2:%d v3:%d bad:%d d%.0f D%.0f i%.0f",
                     cad_v2, cad_v3, cad_vx, PF_MS(s.wk_dec) / wn, PF_MS(s.wk_dmax), PF_MS(pf_idle) / (pf_n > 0 ? pf_n : 1));
            cad_v2 = cad_v3 = cad_vx = 0; pf_idle = 0; pf_n = 0;
            s.wk_dec = 0; s.wk_n = 0; s.wk_dmax = 0;
            last_ts[0] = 0;   /* force the clock string to re-parse with the new numbers */
        }
    }
    if (!aptMainLoop() && result == MOFLEX_QUIT_BACK) result = MOFLEX_QUIT_EXIT;
    aptUnhook(&g_ring_apt_cookie);

    /* stop + join the decode thread (New-3DS) before freeing anything it uses (demux, decoder, ring) */
    if (threaded) {
        s.run = 0; s.paused = 0; g_ring_susp = 0; LightEvent_Signal(&s.wake);
        threadJoin(dthr, UINT64_MAX); threadFree(dthr);
    }
    g_ring_lock = NULL; g_ring_wake = NULL;   /* s.lock/s.wake go out of scope after this */

    if (dur_us > 0 && cur_us >= dur_us - 10000000) resume_clear(path);
    else if (cur_us > 3000000) resume_save_us(path, cur_us);
    if (vol_dirty) vol_save();
    /* release panel services (restore backlight first so we never leave the screen dark) */
    if (g_screen_off) { GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM); g_screen_off = 0; }
    if (g_lcd_ok) { gspLcdExit(); g_lcd_ok = 0; }
    if (g_mcu_ok) { mcuHwcExit(); g_mcu_ok = 0; }
    ptmuExit();
    r3_audio_close();   /* unconditional: also frees a partial bank when setup failed */
    r3_vq_clear();
    C2D_TextBufDelete(sbuf); C2D_TextBufDelete(tmbuf); C2D_TextBufDelete(subbuf);
    for (int i = 0; i < NB; i++) { C3D_TexDelete(&r3_texL[i]); C3D_TexDelete(&r3_texR[i]); }
    ui_tex_free();   /* release the software-UI panel texture before C3D shuts down */
    gspWaitForVBlank(); gspWaitForVBlank();
    C2D_Fini(); C3D_Fini(); y2rExit();
    gfxSet3D(false);
    /* Hand a CLEAN gfx state back to the app ONLY when returning to it (BACK/OPEN). On EXIT the app is
     * closing -- doing gfxExit()+gfxInitDefault() during the applet close hangs on "closing software",
     * so skip it and let the process tear down. */
    if (result != MOFLEX_QUIT_EXIT) { gfxExit(); gfxInitDefault(); }
    av_frame_free(&fL); av_frame_free(&fR);
    mobi_close(&ctx); free(ctx.priv_data); mfx_close(&m); fclose(f);
    return result;
}

/* ---- APT (HOME button / sleep) handling during playback ----
 * Pressing HOME hands the screens + GPU to the HOME-menu applet. Without a hook the player keeps
 * its custom GPU state (RGB565 + 3D + double-buffer) and active audio, which deadlocks the
 * transition -> the console hangs. The hook pauses audio + lights the bottom screen on the way
 * out, and re-applies our screen config + resumes audio on the way back. */
static aptHookCookie g_apt_cookie;
static volatile int  g_apt_is3d = 0, g_apt_have_audio = 0, g_apt_playing = 1, g_apt_redraw = 0;
static int g_apt_w = 0, g_apt_h = 0, g_apt_y2r = 0;

static void mp_apt_hook(APT_HookType hook, void *param) {
    (void)param;
    switch (hook) {
        case APTHOOK_ONSUSPEND:
        case APTHOOK_ONSLEEP:
            /* Hand back EVERY video-only hardware resource before the HOME/sleep applet takes the
             * GPU. Menus don't hold Y2R or gsp::Lcd; keeping them open across the transition
             * hard-locks the GSP (needs a power-off). Release them here, re-acquire on restore. */
            if (g_apt_have_audio) ndspChnSetPaused(0, true);                 /* silence while suspended */
            if (g_screen_off) { GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM); g_screen_off = 0; }
            if (g_lcd_ok) gspLcdExit();                                      /* release gsp::Lcd */
            if (g_apt_y2r) { y2r_video_drain(); y2r_video_exit(); }          /* release Y2R */
            gfxSet3D(false);                                                 /* the HOME menu is 2D */
            break;
        case APTHOOK_ONRESTORE:
        case APTHOOK_ONWAKEUP:
            if (g_apt_y2r) y2r_video_init(g_apt_w, g_apt_h);                 /* re-acquire Y2R */
            if (g_lcd_ok) gspLcdInit();                                      /* re-acquire gsp::Lcd */
            if (g_apt_have_audio && g_apt_playing) ndspChnSetPaused(0, false);   /* resume only if we were playing */
            g_apt_redraw = 1;   /* the loop re-applies our screen format AFTER libctru's gfx hook restores */
            break;
        default: break;
    }
}

/* Classic audio-paced player: software framebuffer, no GPU ring. Handles 2D everywhere and is the
 * fallback for 3D when the ring engine can't run. Reached only via the thin moflex_play() dispatcher
 * below -- never called directly, so its big stack frame never stacks under moflex_play_ring's. */
static MoflexResult moflex_play_classic(const char *path) {
    init_luts();
    vol_load();

    FILE *f = fopen(path, "rb");
    if (!f) { printf("\x1b[2J\x1b[Hfopen failed\npress A\n"); mp_wait_key(); return MOFLEX_ERROR; }

    mobi_opt = 14;   /* prefetch + idct-skip + DC-only: bit-exact, ~6-8% faster decode (esp. Old 3DS) */

    MfxDemux m;
    if (mfx_open_auto(&m, f, path) != 0) { printf("\x1b[2J\x1b[Hmfx_open failed\npress A\n"); mp_wait_key(); fclose(f); return MOFLEX_ERROR; }

    int vi = -1, ai = -1;
    for (int i = 0; i < m.nb_streams; i++) {
        if (m.streams[i].media_type == MFX_TYPE_VIDEO && vi < 0) vi = i;
        if (m.streams[i].media_type == MFX_TYPE_AUDIO && ai < 0) ai = i;
    }
    if (vi < 0) { printf("\x1b[2J\x1b[Hno video\npress A\n"); mp_wait_key(); mfx_close(&m); fclose(f); return MOFLEX_ERROR; }
    int W = m.streams[vi].width, H = m.streams[vi].height;
    int arate = ai >= 0 ? m.streams[ai].sample_rate : 44100;
    int chn   = ai >= 0 ? m.streams[ai].channels    : 2;
    int is3d  = mfx_detect_stereo(&m);   /* frame-interleaved 3D vs flat 2D (auto from frame-rate ratio) */

    { bool isnew = false; APT_CheckNew3DS(&isnew);
      g_old3d_warn = (!isnew && is3d); }   /* Old 3DS can't decode the doubled 3D frames in real time */
    /* The video is shown as its frame is READ, while audio sits buffered ahead of what's heard -> the
     * picture leads the sound by ~the audio-queue depth. Shallower queue = tighter lip-sync. 2D races
     * ahead (light decode) so it needs the shallowest; 3D a bit deeper for slack against decode dips. */
    int nwb = is3d ? 8 : 4;   /* tuned for lip-sync (deeper queue = picture leads sound more) */

    AVCodecContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.width = W; ctx.height = H;
    ctx.priv_data = calloc(1, mobi_ctx_size());
    if (!ctx.priv_data || mobi_init(&ctx) != 0) { free(ctx.priv_data); mfx_close(&m); fclose(f); return MOFLEX_ERROR; }
    AVFrame *out = av_frame_alloc();
    /* two decoded-frame sets (L,R each): decode into one while the worker blits the other */
    AVFrame *setL[2] = { av_frame_alloc(), av_frame_alloc() };
    AVFrame *setR[2] = { av_frame_alloc(), av_frame_alloc() };
    int cur_set = 0, pair_in_flight = 0;

    gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);   /* 24-bit video (RGB565 banded the gradients) */
    gfxSetDoubleBuffering(GFX_TOP, true);   /* MUST be on: both eyes go to the back
        buffer and appear together only on gfxSwapBuffers. Single-buffered here would
        make the left eye update before the right (branding_show() turns this off). */
    gfxSet3D(is3d);                          /* 3D on for interleaved stereo; off = flat 2D */

    int use_y2r = y2r_video_init(W, H);   /* hardware YUV->RGB conversion */
    int y2r_started = 0;                   /* a Y2R(left-eye) conversion is in flight */
    (void)bw_init; (void)setL; (void)setR; (void)cur_set; (void)pair_in_flight;

    /* short movie title: the embedded movie's real name if playing inside a CIA, else the filename */
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    char title[64];
    const char *cia_title = cia_selection_title(path);
    if (cia_title) snprintf(title, sizeof(title), "%.60s", cia_title);
    else {
        snprintf(title, sizeof(title), "%.60s", base);
        size_t L = strlen(title);                      /* hide extension */
        if (L > 7 && !strcasecmp(title + L - 7, ".moflex")) title[L - 7] = 0;
        else if (L > 4 && !strcasecmp(title + L - 4, ".zip")) title[L - 4] = 0;
        else if (L > 4 && !strcasecmp(title + L - 4, ".cia")) title[L - 4] = 0;
    }

    subs_autoload(path);   /* load a matching .srt (sidecar or moviedata/) if one exists */

    /* audio */
    #undef NWB
    #define NWB 16
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
    int playing = 1, dirty = 1, since_panel = 999, vol_dirty = 0;
    /* Deferred checkpoint. An SD write goes through the FS service and takes hundreds of ms; doing it
     * straight from the pause handler blocked the loop inside fclose(), so the pause -- and the next
     * keypress, which hidScanInput() never got to sample -- appeared to hang. Arm it on pause instead
     * and let the idle branch write it once the paused frame is already on screen. */
    int save_pend = 0, save_delay = 0;
    #define SAVE_DELAY_FRAMES 30   /* ~0.5s: a quick pause/unpause never touches the card at all */
    int64_t cur_us = 0, dur_us = m.duration_us;
    int64_t seek_to_us = 0; int want_seek = 0, shold = 0;
    MfxPacket pkt;

    /* perf timing: accumulate decode/blit/audio ticks, report per second */
    u64 dbg_dec = 0, dbg_blit = 0, dbg_aud = 0; int dbg_vf = 0, dbg_pairs = 0, dbg_ap = 0; u64 dbg_win = osGetTime();

    /* auto-resume where we left off (unless within 10s of the end) */
    int64_t rpos = resume_load(path), last_save = 0;
    if (rpos > 3000000 && (dur_us <= 0 || rpos < dur_us - 10000000)) {
        seek_to_us = rpos; want_seek = 1; last_save = rpos;
    }

    g_lcd_ok = R_SUCCEEDED(gspLcdInit());   /* per-screen backlight control for the bottom-screen-off button */
    g_screen_off = 0;
    ptmuInit();                                          /* charging state + coarse level */
    g_mcu_ok = R_SUCCEEDED(mcuHwcInit());                /* exact 0-100% (needs mcu::HWC in the CIA RSF) */
    g_batt_pct = -1; g_batt_next = 0;

    /* handle HOME/sleep cleanly (release Y2R + gsp::Lcd, pause audio, restore GPU state) */
    g_apt_is3d = is3d; g_apt_have_audio = have_audio; g_apt_playing = playing; g_apt_redraw = 0;
    g_apt_w = W; g_apt_h = H; g_apt_y2r = use_y2r;
    aptHook(&g_apt_cookie, mp_apt_hook, NULL);

    while (aptMainLoop()) {
        g_apt_playing = playing;                       /* keep the hook's resume decision in sync */
        if (g_apt_redraw) {                            /* returned from HOME: re-apply our screen config + repaint */
            g_apt_redraw = 0; dirty = 1;
            gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);   /* keep 24-bit after returning from HOME */
            gfxSetDoubleBuffering(GFX_TOP, true);
            gfxSet3D(is3d);
        }
        hidScanInput();
        u32 kd = hidKeysDown(), kh = hidKeysHeld();
        touchPosition tp; hidTouchRead(&tp);

        if (g_screen_off) {   /* bottom screen is dark: the first input just wakes it (no action) */
            if (kd) { GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM); g_screen_off = 0; dirty = 1; }
            goto after_input;
        }

        /* ---- controls (direct, nothing to select): A play/pause,
               Left/Right (hold) rewind/FF, Up/Down volume, B back ---- */
        if (kd & KEY_B) { result = MOFLEX_QUIT_BACK; break; }
        if (kd & KEY_A) { playing = !playing; if (have_audio) ndspChnSetPaused(0, !playing); dirty = 1;
                          save_pend = !playing; save_delay = SAVE_DELAY_FRAMES; }   /* arm only; unpausing disarms it */
        if (kd & KEY_UP)   { int s = (int)(g_vol / 0.25f + 0.0001f); g_vol = (s + 1) * 0.25f; if (g_vol > 4.0f) g_vol = 4.0f; vol_dirty = 1; dirty = 1; }   /* snap up to the 25% grid, capped at 400% */
        if (kd & KEY_DOWN) { int s = (int)(g_vol / 0.25f + 0.9999f); g_vol = (s - 1) * 0.25f; if (g_vol < 0.25f) g_vol = 0.25f; vol_dirty = 1; dirty = 1; }
        {   /* Left/Right seek, hold to keep scrubbing (~30s steps) */
            int sdir = (kh & KEY_RIGHT) ? 1 : ((kh & KEY_LEFT) ? -1 : 0);
            if (sdir == 0) shold = 0;
            else {
                int fire = (kd & (KEY_RIGHT | KEY_LEFT)) ? 1 : 0;   /* initial press */
                if (!fire) { shold++; if (shold > 14 && (shold % 6 == 0)) fire = 1; }  /* repeat while held */
                if (fire) { seek_to_us = cur_us + (int64_t)sdir * 30000000; want_seek = 1; }
            }
        }

        if (kd & KEY_SELECT) {   /* SELECT: subtitle options */
            if (have_audio) ndspChnSetPaused(0, true);
            sub_menu(path, is3d);
            if (have_audio) ndspChnSetPaused(0, !playing);
            dirty = 1;
            if (!playing) { want_seek = 1; seek_to_us = cur_us; }   /* re-render the frozen frame with the new subtitle */
        }

        /* ---- touch (always active) ---- */
        if ((kd | kh) & KEY_TOUCH) {
            int px = tp.px, py = tp.py;
            if (py >= BAR_Y - 8 && py <= BAR_Y + BAR_H + 8 && px >= BAR_X && px <= BAR_X + BAR_W) {
                if (kd & KEY_TOUCH) { seek_to_us = (int64_t)((double)(px - BAR_X) / BAR_W * dur_us); want_seek = 1; }
            } else if (px >= VOL_X - 6 && px <= VOL_X + 18 && py >= VOL_Y - 10 && py <= VOL_Y + VOL_H + 10) {
                float nv = (float)(VOL_Y + VOL_H - py) / VOL_H * 4.0f;   /* extends past the ends so 0% and 400% are reachable */
                if (nv < 0.25f) nv = 0.25f; if (nv > 4.0f) nv = 4.0f;
                g_vol = nv; vol_dirty = 1; dirty = 1;   /* save deferred; dragging the slider must not write to SD each tick */
            } else if (kd & KEY_TOUCH) {
                if (g_lcd_ok && px >= DIM_X && px < DIM_X + DIM_W && py >= DIM_Y && py < DIM_Y + DIM_H) {
                    GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTTOM); g_screen_off = 1;   /* dark until any button press */
                } else if (px >= CC_X && px < CC_X + CC_W && py >= CC_Y && py < CC_Y + CC_H) {   /* CC: subtitle options */
                    if (have_audio) ndspChnSetPaused(0, true);
                    sub_menu(path, is3d);
                    if (have_audio) ndspChnSetPaused(0, !playing);
                    dirty = 1;
                    if (!playing) { want_seek = 1; seek_to_us = cur_us; }   /* re-render the frozen frame with the new subtitle */
                } else if (py >= PLAY_CY - 20 && py <= PLAY_CY + 20) {
                    if (px >= PLAY_CX - 20 && px <= PLAY_CX + 20) { playing = !playing; if (have_audio) ndspChnSetPaused(0, !playing); dirty = 1;
                          save_pend = !playing; save_delay = SAVE_DELAY_FRAMES; }   /* arm only (same as KEY_A) */
                    else if (px >= RW_CX - 18 && px <= RW_CX + 18) { seek_to_us = cur_us - 30000000; want_seek = 1; }
                    else if (px >= FF_CX - 18 && px <= FF_CX + 18) { seek_to_us = cur_us + 30000000; want_seek = 1; }
                } else if (py >= BTN_Y && py < BTN_Y + BTN_H) {   /* BACK / OPEN / EXIT */
                    if      (px >= BKB_X && px < BKB_X + BKB_W) { result = MOFLEX_QUIT_OPEN; break; }   /* OPEN VIDEO */
                    else if (px >= OPB_X && px < OPB_X + OPB_W) { result = MOFLEX_QUIT_MANAGE; break; }   /* MANAGE VIDEOS */
                    else if (px >= EXB_X && px < EXB_X + EXB_W) { result = MOFLEX_QUIT_ADD; break; }   /* ADD VIDEO */
                }
            }
        }
    after_input: ;

        /* ---- apply a requested seek (resume playback, flush stale audio) ---- */
        if (want_seek) {
            if (y2r_started) { y2r_video_drain(); y2r_started = 0; }   /* drop any in-flight Y2R */
            do_seek(&m, &ctx, seek_to_us);
            vidx = 0; left_ok = 0;          /* re-establish L/R pairing after seek */
            if (have_audio) {
                ndspChnWaveBufClear(0);
                ndspChnSetPaused(0, !playing);   /* keep paused if we were paused (seek doesn't auto-play) */
                for (int i = 0; i < NWB; i++) wbuf[i].status = NDSP_WBUF_DONE;
                wb_idx = 0;
            }
            /* show the landing frame right away; audio then resumes synced to it */
            prime_after_seek(&m, &ctx, out, W, H, &cur_us, &vidx, &left_ok, &left_vidx, is3d);
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
                /* NOTE: no periodic resume write here -- an SD write mid-stream stalls the FS
                 * service and hitches the video (the shared SDMC path). We checkpoint only when
                 * stopped: on pause (above) and on exit (done:), when the read loop is idle. */
                int mt = m.streams[pkt.stream_index].media_type;
                if (mt == MFX_TYPE_AUDIO && have_audio) {
                    u64 ta = svcGetSystemTick();
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
                        wb_idx = (wb_idx + 1) % nwb;   /* nwb = 16 (3D) or 4 (2D) -> tighter 2D A/V */
                    }
                    dbg_aud += svcGetSystemTick() - ta; dbg_ap++;
                } else if (mt == MFX_TYPE_VIDEO) {
                    AVPacket ap; ap.data = pkt.data; ap.size = pkt.size;
                    int got = 0, eye = vidx & 1;
                    u64 t0 = svcGetSystemTick();
                    int ok = (mobi_decode(&ctx, out, &got, &ap) >= 0 && got);  /* decode every frame (keeps ref chain) */
                    dbg_dec += svcGetSystemTick() - t0; dbg_vf++;
                    (void)y2r_started; (void)use_y2r;
                    if (!is3d) {                           /* 2D: every frame is a full flat frame -> present it */
                        if (ok) { u64 tb = svcGetSystemTick(); blit_eye(out, GFX_LEFT, W, H); dbg_blit += svcGetSystemTick() - tb;
                                  sub_overlay(0, cur_us);
                                  gfxFlushBuffers(); gfxSwapBuffers(); dbg_pairs++; }
                    } else if (eye == 0) {                 /* left eye */
                        if (ok) { u64 tb = svcGetSystemTick(); blit_eye(out, GFX_LEFT, W, H); dbg_blit += svcGetSystemTick() - tb; left_ok = 1; left_vidx = vidx; }
                        else left_ok = 0;
                    } else {                              /* right eye: present only a matched pair */
                        if (ok && left_ok && vidx == left_vidx + 1) {
                            u64 tb = svcGetSystemTick(); blit_eye(out, GFX_RIGHT, W, H); dbg_blit += svcGetSystemTick() - tb;
                            sub_overlay(1, cur_us);        /* draw the cue on both eyes */
                            gfxFlushBuffers(); gfxSwapBuffers();
                            dbg_pairs++;
                        }
                        left_ok = 0;
                    }
                    vidx++;                               /* always advance -> parity tracks the stream */
                    /* report timing ~once per second */
                    u64 nowm = osGetTime();
                    if (nowm - dbg_win >= 1000) {
                        double tpms = SYSCLOCK_ARM11 / 1000.0;   /* ticks per ms (Old 3DS clock) */
                        g_dbg_fps = dbg_pairs;
                        g_dbg_dec10  = dbg_vf ? (int)(dbg_dec * 10.0 / tpms / dbg_vf) : 0;
                        g_dbg_blit10 = dbg_pairs ? (int)(dbg_blit * 10.0 / tpms / (dbg_pairs * 2)) : 0;
                        g_dbg_aud10  = dbg_pairs ? (int)(dbg_aud * 10.0 / tpms / dbg_pairs) : 0;
                        dbg_dec = dbg_blit = dbg_aud = 0; dbg_vf = dbg_pairs = dbg_ap = 0; dbg_win = nowm;
                    }
                }
            }
        } else {
            gspWaitForVBlank();
            /* paused + idle: the pause is on screen and the decoder is stopped, so the card can take
             * as long as it likes here without anyone noticing. */
            if (save_pend && --save_delay <= 0) {
                if (cur_us != last_save) { resume_save_us(path, cur_us); last_save = cur_us; }
                if (vol_dirty) { vol_save(); vol_dirty = 0; }
                save_pend = 0;
            }
        }

        /* ---- panel redraw (throttled; skipped while the bottom screen is dark) ---- */
        if (!g_screen_off && (dirty || ++since_panel > 20)) {
            panel_draw(title, cur_us, dur_us, playing);
            since_panel = 0; dirty = 0;
        }
    }
    if (!aptMainLoop() && result == MOFLEX_QUIT_BACK) result = MOFLEX_QUIT_EXIT;

done:
    aptUnhook(&g_apt_cookie);
    if (g_screen_off) { GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM); g_screen_off = 0; }   /* never leave it dark */
    if (g_lcd_ok) { gspLcdExit(); g_lcd_ok = 0; }
    if (g_mcu_ok) { mcuHwcExit(); g_mcu_ok = 0; }
    ptmuExit();
    g_old3d_warn = 0;
    if (vol_dirty) { vol_save(); vol_dirty = 0; }   /* persist any volume change made during playback */
    /* remember where we stopped (clear it if we watched to the end) */
    if (dur_us > 0 && cur_us >= dur_us - 10000000) resume_clear(path);
    else if (cur_us > 3000000) resume_save_us(path, cur_us);
    if (have_audio) { ndspChnWaveBufClear(0); ndspChnSetPaused(0, false);
        for (int i = 0; i < NWB; i++) if (abuf[i]) linearFree(abuf[i]); }
    if (y2r_started) y2r_video_drain();   /* let any in-flight Y2R finish before freeing frames */
    y2r_video_exit();
    av_frame_free(&out);
    av_frame_free(&setL[0]); av_frame_free(&setL[1]);
    av_frame_free(&setR[0]); av_frame_free(&setR[1]);
    mobi_close(&ctx);
    free(ctx.priv_data);
    mfx_close(&m);
    fclose(f);
    return result;
}

/* Public entry: a THIN dispatcher. It must keep a tiny stack frame -- whichever player it calls runs
 * with that frame still live underneath, and the decoder's VLC init (ff_vlc_init_from_lengths) is
 * stack-hungry; a big frame here stacked under moflex_play_ring's overflowed the main-thread stack
 * (data abort in VLC init). So: peek 3D-ness on a HEAP demux, then hand off. 3D tries the ring engine
 * (banking + audio-master); if it can't allocate on this console it returns MOFLEX_FALLBACK and we run
 * the classic path. 2D always uses classic. */
/* ===== SOFTWARE-RENDERED decode-ahead banking player (3D) ================================
 * Same engine as moflex_play_ring (two-phase audio via r3_*, decode-ahead bank, audio-master
 * present, atomic L/R pair) but rendered through the PROVEN software path -- blit_eye (hardware
 * Y2R -> 24-bit, no banding), gfxSwapBuffers (atomic pair), panel_draw / sub_overlay / sub_menu,
 * and the classic HOME hook. No citro2d -> none of the GPU/format/menu conflicts.
 *
 * mobi_decode returns a frame that ALIASES the decoder's 6-slot pic pool, which later decodes
 * overwrite -- so each decoded frame is COPIED OUT into an owned YUV420 ring buffer. blit_eye
 * gets a lightweight AVFrame view (sr_wrap) over the ring slot at present time. */
/* SR_NB defined near the top (panel_draw shows the bank depth). ~0.67s of pairs at 24fps. */
static uint8_t *sr_yL[SR_NB], *sr_yR[SR_NB];
static int64_t  sr_rts[SR_NB], sr_bts[SR_NB];
static int      sr_ready[SR_NB];
static void sr_copyout(AVFrame *s, uint8_t *d, int W, int H) {   /* pic-pool plane -> packed YUV420 */
    int cw = W / 2, ch = H / 2;
    for (int y = 0; y < H;  y++) memcpy(d + (size_t)y * W,  s->data[0] + (size_t)y * s->linesize[0], W);
    d += (size_t)W * H;
    for (int y = 0; y < ch; y++) memcpy(d + (size_t)y * cw, s->data[1] + (size_t)y * s->linesize[1], cw);
    d += (size_t)cw * ch;
    for (int y = 0; y < ch; y++) memcpy(d + (size_t)y * cw, s->data[2] + (size_t)y * s->linesize[2], cw);
}
static void sr_wrap(AVFrame *f, uint8_t *b, int W, int H) {      /* view over a ring slot for blit_eye */
    int cw = W / 2, ch = H / 2;
    f->data[0] = b;                               f->linesize[0] = W;
    f->data[1] = b + (size_t)W * H;               f->linesize[1] = cw;
    f->data[2] = b + (size_t)W * H + (size_t)cw * ch; f->linesize[2] = cw;
    f->width = W; f->height = H;
}

static MoflexResult moflex_play_soft(const char *path) {
    init_luts(); vol_load();
    FILE *f = fopen(path, "rb");
    if (!f) return MOFLEX_ERROR;
    MfxDemux m;
    if (mfx_open_auto(&m, f, path) != 0) { fclose(f); return MOFLEX_ERROR; }
    int vi = -1, ai = -1;
    for (int i = 0; i < m.nb_streams; i++) {
        if (m.streams[i].media_type == MFX_TYPE_VIDEO && vi < 0) vi = i;
        if (m.streams[i].media_type == MFX_TYPE_AUDIO && ai < 0) ai = i;
    }
    if (vi < 0) { mfx_close(&m); fclose(f); return MOFLEX_ERROR; }
    int W = m.streams[vi].width, H = m.streams[vi].height;
    int arate = ai >= 0 ? m.streams[ai].sample_rate : 44100, chn = ai >= 0 ? m.streams[ai].channels : 2;
    int have_audio = (ai >= 0);
    int is3d = mfx_detect_stereo(&m);
    int64_t pair_dur = 40000;
    if (m.streams[vi].tb_den > 0) { int64_t pd = (int64_t)m.streams[vi].tb_num * 1000000 / m.streams[vi].tb_den;
        if (pd >= 16000 && pd <= 100000) pair_dur = pd; }
    if (is3d && m.tb_is_eye) pair_dur *= 2;   /* Nintendo declares the EYE rate: a pair lasts 2x */

    AVCodecContext ctx; memset(&ctx, 0, sizeof ctx);
    ctx.width = W; ctx.height = H; ctx.priv_data = calloc(1, mobi_ctx_size());
    if (!ctx.priv_data || mobi_init(&ctx) != 0) { free(ctx.priv_data); mfx_close(&m); fclose(f); return MOFLEX_ERROR; }
    mobi_opt = 0x1BDA1E;
    AVFrame *fdec = av_frame_alloc();
    AVFrame vwrapL, vwrapR; memset(&vwrapL, 0, sizeof vwrapL); memset(&vwrapR, 0, sizeof vwrapR);

    /* owned YUV ring -> too big for this console? fall back to the classic path. */
    size_t fsz = (size_t)W * H * 3 / 2;
    int okalloc = 1;
    for (int i = 0; i < SR_NB; i++) { sr_yL[i] = malloc(fsz); sr_yR[i] = malloc(fsz); if (!sr_yL[i] || !sr_yR[i]) okalloc = 0; }
    if (!okalloc) {
        for (int i = 0; i < SR_NB; i++) { free(sr_yL[i]); free(sr_yR[i]); sr_yL[i] = sr_yR[i] = NULL; }
        av_frame_free(&fdec); mobi_close(&ctx); free(ctx.priv_data); mfx_close(&m); fclose(f);
        return MOFLEX_FALLBACK;
    }

    char title[64]; const char *bn = strrchr(path, '/'); bn = bn ? bn + 1 : path;
    const char *cia_title = cia_selection_title(path);
    if (cia_title) snprintf(title, sizeof title, "%.60s", cia_title);
    else { snprintf(title, sizeof title, "%.60s", bn); size_t L = strlen(title);
        if (L > 7 && !strcasecmp(title + L - 7, ".moflex")) title[L - 7] = 0;
        else if (L > 4 && !strcasecmp(title + L - 4, ".zip")) title[L - 4] = 0;
        else if (L > 4 && !strcasecmp(title + L - 4, ".cia")) title[L - 4] = 0; }
    subs_autoload(path);

    gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);   /* 24-bit software blit (no banding) */
    gfxSetDoubleBuffering(GFX_TOP, true);        /* both eyes to the back buffer -> appear together on swap */
    gfxSet3D(is3d);
    int use_y2r = y2r_video_init(W, H);

    g_lcd_ok = R_SUCCEEDED(gspLcdInit()); g_screen_off = 0;
    ptmuInit(); g_mcu_ok = R_SUCCEEDED(mcuHwcInit()); g_batt_pct = -1; g_batt_next = 0;

    int playing = 1;
    g_apt_is3d = is3d; g_apt_have_audio = have_audio; g_apt_playing = playing; g_apt_redraw = 0;
    g_apt_w = W; g_apt_h = H; g_apt_y2r = use_y2r;
    aptHook(&g_apt_cookie, mp_apt_hook, NULL);

    if (have_audio) { r3_audio_setup(arate, chn); if (!r3_aok) have_audio = 0; }

    int wr = 0, rd = 0; int64_t dpair = 0, last_bts = 0;
    int left_ok = 0, svid = 0; int64_t l_ts = 0, l_rts = 0, v_anchor = 0; int have_anchor = 0;
    int gated = 0, has_pending = 0, done = 0, last_shown = -1, dirty = 1;
    const int PRIME = (SR_NB >= 16) ? 8 : (SR_NB > 2 ? SR_NB / 2 : 1);
    int64_t cur_us = 0, dur_us = m.duration_us, seek_to_us = 0; int want_seek = 0, shold = 0;
    int64_t rpos = resume_load(path), last_save = 0;
    if (rpos > 3000000 && (dur_us <= 0 || rpos < dur_us - 10000000)) { seek_to_us = rpos; want_seek = 1; last_save = rpos; }
    int vol_dirty = 0, save_pend = 0, save_delay = 0;
    MfxPacket pending;
    u64 r3_wall0 = 0; int r3_wallset = 0; u64 pause_wall = 0;
    memset(sr_ready, 0, sizeof sr_ready);
    MoflexResult result = MOFLEX_QUIT_BACK;

    while (aptMainLoop()) {
        g_apt_playing = playing;
        if (g_apt_redraw) { g_apt_redraw = 0; dirty = 1;
            gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES); gfxSetDoubleBuffering(GFX_TOP, true); gfxSet3D(is3d); }
        hidScanInput();
        u32 kd = hidKeysDown(), kh = hidKeysHeld();
        touchPosition tp; hidTouchRead(&tp);

        if (g_screen_off) { if (kd) { GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM); g_screen_off = 0; dirty = 1; } goto sr_after_input; }

        if (kd & KEY_B) { result = MOFLEX_QUIT_BACK; break; }
        if (kd & KEY_A) {
            if (done && r3_vqc == 0 && (wr - rd) == 0) { seek_to_us = 0; want_seek = 1; }   /* at the end: replay */
            else { playing = !playing; if (have_audio) ndspChnSetPaused(0, !playing); dirty = 1; save_pend = !playing; save_delay = 30;
                    if (!playing) pause_wall = osGetTime();
                    else { r3_last = 0; if (r3_wallset) r3_wall0 += osGetTime() - pause_wall; } }
        }
        if (kd & KEY_UP)   { int s = (int)(g_vol / 0.25f + 0.0001f); g_vol = (s + 1) * 0.25f; if (g_vol > 4.0f) g_vol = 4.0f; vol_dirty = 1; dirty = 1; }
        if (kd & KEY_DOWN) { int s = (int)(g_vol / 0.25f + 0.9999f); g_vol = (s - 1) * 0.25f; if (g_vol < 0.25f) g_vol = 0.25f; vol_dirty = 1; dirty = 1; }
        {   int sdir = (kh & KEY_RIGHT) ? 1 : ((kh & KEY_LEFT) ? -1 : 0);
            if (!sdir) shold = 0;
            else { int fire = (kd & (KEY_RIGHT | KEY_LEFT)) ? 1 : 0;
                   if (!fire) { shold++; if (shold > 14 && (shold % 6 == 0)) fire = 1; }
                   if (fire) { seek_to_us = cur_us + (int64_t)sdir * 30000000; want_seek = 1; } } }
        if (kd & KEY_SELECT) { if (have_audio) ndspChnSetPaused(0, true); sub_menu(path, is3d);
                               if (have_audio) ndspChnSetPaused(0, !playing); dirty = 1;
                               if (!playing) { want_seek = 1; seek_to_us = cur_us; } }
        if ((kd | kh) & KEY_TOUCH) {
            int px = tp.px, py = tp.py;
            if (py >= BAR_Y - 8 && py <= BAR_Y + BAR_H + 8 && px >= BAR_X && px <= BAR_X + BAR_W) {
                if (kd & KEY_TOUCH) { seek_to_us = (int64_t)((double)(px - BAR_X) / BAR_W * dur_us); want_seek = 1; }
            } else if (px >= VOL_X - 6 && px <= VOL_X + 18 && py >= VOL_Y - 10 && py <= VOL_Y + VOL_H + 10) {
                float nv = (float)(VOL_Y + VOL_H - py) / VOL_H * 4.0f; if (nv < 0.25f) nv = 0.25f; if (nv > 4.0f) nv = 4.0f;
                g_vol = nv; vol_dirty = 1; dirty = 1;
            } else if (kd & KEY_TOUCH) {
                if (g_lcd_ok && px >= DIM_X && px < DIM_X + DIM_W && py >= DIM_Y && py < DIM_Y + DIM_H) {
                    GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTTOM); g_screen_off = 1;
                } else if (px >= CC_X && px < CC_X + CC_W && py >= CC_Y && py < CC_Y + CC_H) {
                    if (have_audio) ndspChnSetPaused(0, true); sub_menu(path, is3d);
                    if (have_audio) ndspChnSetPaused(0, !playing); dirty = 1;
                    if (!playing) { want_seek = 1; seek_to_us = cur_us; }
                } else if (py >= PLAY_CY - 20 && py <= PLAY_CY + 20) {
                    if (px >= PLAY_CX - 20 && px <= PLAY_CX + 20) { playing = !playing; if (have_audio) ndspChnSetPaused(0, !playing); dirty = 1; save_pend = !playing; save_delay = 30;
                    if (!playing) pause_wall = osGetTime();
                    else { r3_last = 0; if (r3_wallset) r3_wall0 += osGetTime() - pause_wall; } }
                    else if (px >= RW_CX - 18 && px <= RW_CX + 18) { seek_to_us = cur_us - 30000000; want_seek = 1; }
                    else if (px >= FF_CX - 18 && px <= FF_CX + 18) { seek_to_us = cur_us + 30000000; want_seek = 1; }
                } else if (py >= BTN_Y && py < BTN_Y + BTN_H) {
                    if      (px >= BKB_X && px < BKB_X + BKB_W) { result = MOFLEX_QUIT_OPEN; break; }   /* OPEN VIDEO */
                    else if (px >= OPB_X && px < OPB_X + OPB_W) { result = MOFLEX_QUIT_MANAGE; break; }   /* MANAGE VIDEOS */
                    else if (px >= EXB_X && px < EXB_X + EXB_W) { result = MOFLEX_QUIT_ADD; break; }   /* ADD VIDEO */
                }
            }
        }
    sr_after_input:
        if (kd) dirty = 1;

        if (want_seek) {
            if (dur_us > 0) { if (seek_to_us < 0) seek_to_us = 0; if (seek_to_us > dur_us) seek_to_us = dur_us; }
            do_seek(&m, &ctx, seek_to_us);
            for (int i = rd; i < wr; i++) sr_ready[i % SR_NB] = 0;
            wr = rd = 0; svid = 0; left_ok = 0; dpair = 0; last_shown = -1; have_anchor = 0;
            r3_vq_clear(); if (have_audio) r3_audio_flush();
            gated = 0; has_pending = 0; done = 0; playing = 1; r3_wallset = 0;
            cur_us = seek_to_us; want_seek = 0; dirty = 1;
        }

        int worked = 0;
        /* Phase 1: keep AUDIO flowing above all else. Audio is fed until its bank is full; video is
         * stashed in the backlog while there's room, and DROPPED once the backlog is full (that only
         * happens when decode has fallen behind real time, i.e. Old-3DS 3D -- dropping keeps audio
         * smooth + bounds A/V lag instead of freezing the read and starving the DSP). */
        if (playing) {
            int guard = 0;
            while (!done && guard++ < 128) {
                if (!has_pending) { if (mfx_next_packet(&m, &pending) != 1) { done = 1; break; } has_pending = 1; }
                int mt = m.streams[pending.stream_index].media_type;
                if (mt == MFX_TYPE_AUDIO && have_audio) {
                    if (r3_audio_feed(&pending)) { has_pending = 0; worked = 1; }
                    else if (!gated) { has_pending = 0; }   /* pre-roll: bank full -> drop, keep priming video */
                    else break;                             /* playing: bank full -> audio is ahead, pause reading */
                }
                else if (mt == MFX_TYPE_VIDEO) {
                    int64_t vts = m.ts; if (vts < 0) vts = 0; if (dur_us > 0 && vts > dur_us) vts = dur_us;
                    r3_vq_push(pending.data, pending.size, vts, svid);   /* silently drops if backlog full */
                    has_pending = 0; worked = 1; svid++;
                }
                else has_pending = 0;
            }
        }
        /* Phase 2: decode ONE frame from the backlog, copy it OUT of the pic pool into the ring */
        if (playing && r3_vqc > 0 && (wr - rd) < SR_NB - 1) {
            int n, sv; int64_t vts; uint8_t *p = r3_vq_pop(&n, &vts, &sv);
            int eye = sv & 1; int64_t prts = (int64_t)(sv >> 1) * pair_dur;
            cur_us = vts;
            int slot = wr % SR_NB, got = 0;
            AVPacket ap; ap.data = p; ap.size = n;
            int ok = (mobi_decode(&ctx, fdec, &got, &ap) >= 0 && got);
            free(p);
            if (!is3d) {
                if (ok) { sr_copyout(fdec, sr_yL[slot], W, H); sr_rts[slot] = prts; sr_bts[slot] = cur_us; sr_ready[slot] = 1; wr++; dpair++; }
            } else if (eye == 0) {
                left_ok = ok; l_ts = cur_us; l_rts = prts;
                if (ok) sr_copyout(fdec, sr_yL[slot], W, H);
            } else {
                if (ok && left_ok) {
                    sr_copyout(fdec, sr_yR[slot], W, H);
                    sr_rts[slot] = l_rts; sr_bts[slot] = l_ts; sr_ready[slot] = 1; wr++; dpair++;
                }
                left_ok = 0;
            }
            worked = 1;
        }

        if (have_audio && !gated && playing && wr >= PRIME) { gated = 1; ndspChnSetPaused(0, false); r3_apos = 0; }
        if (have_audio && playing) r3_audio_poll();   /* clock frozen while paused -> instant pause */

        int64_t apos;
        if (have_audio) apos = r3_apos;
        else { if (!r3_wallset && wr >= PRIME) { r3_wall0 = osGetTime(); r3_wallset = 1; }
               apos = r3_wallset ? (int64_t)(osGetTime() - r3_wall0) * 1000 : -1; }

        int show = -1;
        if (playing && (wr - rd) > 0 && sr_ready[rd % SR_NB]) {
            if (!have_anchor) { v_anchor = sr_rts[rd % SR_NB]; have_anchor = 1; }
            while ((wr - rd) > 1 && sr_ready[(rd + 1) % SR_NB] && (sr_rts[(rd + 1) % SR_NB] - v_anchor) <= apos) { sr_ready[rd % SR_NB] = 0; rd++; }
            if (apos < 0) { if (last_shown < 0) show = rd % SR_NB; }
            else if (apos >= (sr_rts[rd % SR_NB] - v_anchor)) show = rd % SR_NB;
        }

        /* present a due pair, or repaint the held frame on a UI change -- both eyes + panel then ONE swap */
        int draw = (show >= 0) ? show : ((dirty && last_shown >= 0) ? last_shown : -1);
        if (draw >= 0) {
            sr_wrap(&vwrapL, sr_yL[draw], W, H); blit_eye(&vwrapL, GFX_LEFT, W, H);
            if (is3d) { sr_wrap(&vwrapR, sr_yR[draw], W, H); blit_eye(&vwrapR, GFX_RIGHT, W, H); }
            sub_overlay(is3d, sr_bts[draw]);
            g_sw_bank = wr - rd;   /* live bank depth for the panel readout */
            if (!g_screen_off) panel_draw(title, sr_bts[draw], dur_us, playing);
            gfxFlushBuffers(); gfxSwapBuffers();
            if (show >= 0) { sr_ready[show] = 0; last_shown = show; last_bts = sr_bts[show]; rd++; }
            dirty = 0;
        }

        if (save_pend && --save_delay <= 0) {
            if (cur_us != last_save) { resume_save_us(path, cur_us); last_save = cur_us; }
            if (vol_dirty) { vol_save(); vol_dirty = 0; }
            save_pend = 0;
        }
        if (vol_dirty && !save_pend) { vol_save(); vol_dirty = 0; }

        /* movie finished: STOP in the player (A replays from the start, B leaves) */
        if (done && r3_vqc == 0 && (wr - rd) == 0 && playing) {
            playing = 0;
            if (have_audio) ndspChnSetPaused(0, true);
            resume_clear(path);
            if (dur_us > 0) cur_us = dur_us;
            dirty = 1;
        }
        if (show < 0 && !worked && !dirty) gspWaitForVBlank();
    }
    if (!aptMainLoop() && result == MOFLEX_QUIT_BACK) result = MOFLEX_QUIT_EXIT;

    aptUnhook(&g_apt_cookie);
    if (g_screen_off) { GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM); g_screen_off = 0; }
    if (g_lcd_ok) { gspLcdExit(); g_lcd_ok = 0; }
    if (g_mcu_ok) { mcuHwcExit(); g_mcu_ok = 0; }
    ptmuExit(); g_old3d_warn = 0; g_sw_bank = -1;
    if (vol_dirty) vol_save();
    if (dur_us > 0 && cur_us >= dur_us - 10000000) resume_clear(path);
    else if (cur_us > 3000000) resume_save_us(path, cur_us);
    r3_audio_close();   /* unconditional: also frees a partial bank when setup failed */
    r3_vq_clear();
    y2r_video_exit();
    for (int i = 0; i < SR_NB; i++) { free(sr_yL[i]); free(sr_yR[i]); sr_yL[i] = sr_yR[i] = NULL; }
    av_frame_free(&fdec);
    gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES); gfxSetScreenFormat(GFX_BOTTOM, GSP_RGB565_OES); gfxSet3D(false);
    mobi_close(&ctx); free(ctx.priv_data); mfx_close(&m); fclose(f);
    return result;
}

/* 3D -> GPU engine (moflex_play_ring): the GPU rotates the screen for FREE, so decode has surplus to
 * bank (avtest proved this is smooth on Old 3DS). It now gfxExit/gfxInitDefault's to a clean state to
 * avoid the software-UI gfx conflict. USE_RING_3D=0 falls back to the software banking engine (which
 * is decode+transpose-bound on Old 3DS). Either way, if the engine can't allocate -> classic. */
#define USE_RING_3D 1
MoflexResult moflex_play(const char *path) {
    int is3d = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        MfxDemux *m = (MfxDemux *)malloc(sizeof *m);
        if (m) {
            if (mfx_open_auto(m, f, path) == 0) { is3d = mfx_detect_stereo(m); mfx_close(m); }
            free(m);
        }
        fclose(f);
    }
    /* RING_3D_ENABLE=0: 3D uses the CLASSIC path -- stable on ALL content (smooth audio, lower framerate
     * on heavy scenes, never freezes/skips). The GPU banking engine (moflex_play_ring) helps only when
     * decode OUTPACES real time (light content); on content that decodes below real time it needs
     * avtest's full keyframe catch-up (skip to a keyframe at/before the audio clock, advancing the
     * clock) -- my simplified drop overshoots audio and freezes. Flip to 1 to A/B the ring. */
    #define RING_3D_ENABLE 1
#if RING_3D_ENABLE
    if (is3d) {
        bool isnew = false; APT_CheckNew3DS(&isnew);
        if (isnew) {                                      /* New-3DS: threaded RING engine -- decode
                                                           * outpaces real time, banks ahead, judder-free,
                                                           * fluid seek. */
            MoflexResult r = moflex_play_ring(path);
            if (r != MOFLEX_FALLBACK) return r;
        }
        /* Old-3DS 3D -> stable CLASSIC path below (v1.0 behavior). The threaded ring engine only helps
         * when decode outpaces real time (New-3DS); on Old-3DS heavy files it can't, so keep the proven
         * classic path here. Experimental Old-3DS A/V-sync work lives on the old3ds-avsync branch. */
    }
#else
    (void)is3d;
#endif
    return moflex_play_classic(path);
}
