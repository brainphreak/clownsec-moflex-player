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
extern void   mobi_flush(AVCodecContext *);
extern int    mobi_close(AVCodecContext *);
extern int    mobi_opt;   /* decode-path selector; 14 = prefetch|idct-skip|dc (bit-exact, ~6-8% faster) */
extern size_t mobi_ctx_size(void);

#define SCR_W 400
#define SCR_H 240

/* ---- software volume (persisted) ---- */
static float g_vol = 1.0f;
static int   g_old3d_warn = 0;   /* Old 3DS + 3D content: show the "unsupported / perf issues" notice */
static int   g_lcd_ok = 0;       /* gsp::Lcd available -> bottom-screen-off feature usable */
static int   g_screen_off = 0;   /* bottom backlight is currently off (movie keeps playing on top) */

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
static void subenc_save(const char *movie, int enc) {
    mkdir("sdmc:/moflex_player", 0777); mkdir("sdmc:/moflex_player/resume", 0777);
    char p[256]; subenc_path(movie, p, sizeof p);
    FILE *f = fopen(p, "wb"); if (f) { fprintf(f, "%d\n", enc); fclose(f); }
}
long long moflex_resume_get(const char *path) { return (long long)resume_load(path); }

/* shared stores for the MP4 player (same files/keys, so position + volume stay consistent) */
void  moflex_resume_save(const char *path, long long us) { resume_save_us(path, (int64_t)us); }
void  moflex_resume_clear(const char *path)              { resume_clear(path); }
float moflex_vol_get(void)  { vol_load(); return g_vol; }
void  moflex_vol_set(float v) { if (v < 0.25f) v = 0.25f; if (v > 4.0f) v = 4.0f; g_vol = v; vol_save(); }

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
/* Convert+rotate one eye into a specific framebuffer (used by the blit worker). */
static void blit_to(AVFrame *out, u16 *fb, int W, int H) {
    if (y2r_video_blit_fb(out, fb, W, H)) return;   /* hardware color conversion */
    /* software fallback (per-pixel YUV->RGB565 + rotate) */
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

static void blit_eye(AVFrame *out, gfx3dSide_t side, int W, int H) {
    blit_to(out, (u16 *)gfxGetFramebuffer(GFX_TOP, side, NULL, NULL), W, H);
}

/* ---- blit worker: runs the Y2R+transpose for both eyes on the 2nd CPU core so it
 * overlaps the (slow) MobiClip decode on the main core -> ~doubles Old-3DS throughput. */
typedef struct { AVFrame *l, *r; u16 *fbl, *fbr; int w, h; } BlitJob;
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
/* auto-load a matching track at playback start: "<movie>.srt" beside the file, else moviedata/ */
static void subs_autoload(const char *moviepath) {
    g_nsubs = 0; g_sub_on = 0; g_sub_off = 0; g_sub_file[0] = 0;   /* offset is per-movie */
    { int e = subenc_load(moviepath); if (e >= 0) g_sub_enc = e; }  /* this movie's saved encoding (else keep last-used) */
    const char *b = strrchr(moviepath, '/'); b = b ? b + 1 : moviepath;
    char stem[256]; snprintf(stem, sizeof stem, "%s", b);
    char *dot = strrchr(stem, '.'); if (dot) *dot = 0;
    char cand[600];
    snprintf(cand, sizeof cand, "%.*s%s.srt", (int)(b - moviepath), moviepath, stem);   /* sidecar */
    if (subs_load(cand)) { g_sub_on = 1; return; }
    snprintf(cand, sizeof cand, "sdmc:/moflex_player/moviedata/%s.srt", stem);           /* moviedata */
    if (subs_load(cand)) { g_sub_on = 1; return; }
}

/* draw the active cue straight onto the (rotated) TOP framebuffer, both eyes in 3D */
static void sub_fbpx(u16 *fb, int x, int y, u16 c) {
    if ((unsigned)x < SCR_W && (unsigned)y < SCR_H) fb[x * SCR_H + (SCR_H - 1 - y)] = c;
}
static void sub_fbtext(u16 *fb, int x, int y, int sc, u16 col, const char *s) {
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
static void sub_fbfill(u16 *fb, u16 c) { int n = SCR_W * SCR_H; for (int i = 0; i < n; i++) fb[i] = c; }

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
static void sub_draw_lines(u16 *fb, int cx, int y0, char lines[SUB_MAXLN][SUB_LNW], int nl, int sc) {
    int lh = 8 * sc + 4;
    for (int i = 0; i < nl; i++) {
        const char *s = lines[i];
        int x = cx - u8_cplen(s) * 8 * sc / 2, y = y0 + i * lh;   /* center by glyph count, not bytes */
        sub_fbtext(fb, x - 1, y, sc, 0, s); sub_fbtext(fb, x + 1, y, sc, 0, s);
        sub_fbtext(fb, x, y - 1, sc, 0, s); sub_fbtext(fb, x, y + 1, sc, 0, s);
        sub_fbtext(fb, x, y, sc, 0xFFFF, s);
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
        u16 *fb = (u16 *)gfxGetFramebuffer(GFX_TOP, eye ? GFX_RIGHT : GFX_LEFT, NULL, NULL);
        int dx = is3d ? ((eye == 0) ? -g_sub_depth : g_sub_depth) : 0;   /* parallax between eyes */
        sub_draw_lines(fb, SCR_W / 2 + dx, y0, lines, nl, sc);
    }
}

/* debug timing overlay (per stereo pair), updated ~1x/sec from the loop */
static int g_dbg_fps = 0, g_dbg_dec10 = 0, g_dbg_blit10 = 0, g_dbg_aud10 = 0;

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
    if (g_old3d_warn)   /* Old 3DS can't keep up with 3D's doubled frame rate */
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

    /* action buttons (touch): BACK / OPEN / EXIT */
    ui_button(BKB_X, BTN_Y, BKB_W, BTN_H, "BACK", 0, UI_NEONP);
    ui_button(OPB_X, BTN_Y, OPB_W, BTN_H, "OPEN", 0, UI_NEON);
    ui_button(EXB_X, BTN_Y, EXB_W, BTN_H, "EXIT", 0, UI_RED);
    /* subtitles: CC toggle/options (glows when on) */
    ui_button(CC_X, CC_Y, CC_W, CC_H, "CC", g_sub_on, g_sub_on ? UI_NEON : UI_DIM);
    /* bottom-screen-off: a crescent-moon button (video keeps playing on top) */
    if (g_lcd_ok) {
        ui_button(DIM_X, DIM_Y, DIM_W, DIM_H, "", 0, UI_NEONP);
        int mx = DIM_X + DIM_W / 2, my = DIM_Y + DIM_H / 2, mr = 8;
        ui_fill_round(mx - mr, my - mr, 2 * mr, 2 * mr, mr, UI_NEONC);                 /* full disc */
        ui_fill_round(mx - mr + 6, my - mr - 2, 2 * mr, 2 * mr, mr, UI_BG2); /* carve -> crescent */
    }
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
        ui_text_center(UI_W / 2, 228, 1, UI_DIM, "A choose   B back");
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
            u16 *fb = (u16 *)gfxGetFramebuffer(GFX_TOP, eye ? GFX_RIGHT : GFX_LEFT, NULL, NULL);
            sub_fbfill(fb, UI_BG2);
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
        ui_text_center(UI_W / 2, 212, 1, UI_DIM, "LEFT / RIGHT adjust    A done");
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
        ui_text_center(UI_W / 2, 212, 1, UI_DIM, "LEFT / RIGHT adjust    A done");
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
        if (c < 0) return;                                   /* B closes the menu */
        msel = c;                                            /* stay on this row after the action */
        int a = act[c];
        if (a == 0) { if (g_nsubs > 0) g_sub_on = !g_sub_on; else sub_msg("No subtitles loaded.\nUse 'Load SRT file'."); }
        else if (a == 1) g_sub_top = !g_sub_top;
        else if (a == 2) g_sub_size = g_sub_size >= 3 ? 1 : g_sub_size + 1;
        else if (a == 3) sub_offset_menu();
        else if (a == 4) sub_depth_menu(is3d);
        else if (a == 5) sub_load_menu(moviepath);
        else if (a == 6) { g_sub_enc = (g_sub_enc + 1) % 5;   /* re-decode the loaded SRT with the new codepage */
                           subenc_save(moviepath, g_sub_enc);   /* remember this movie's encoding */
                           if (g_sub_file[0]) subs_load(g_sub_file); }
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
static void g_y2r_init(int W, int H) {
    y2rInit();
    Y2RU_ConversionParams p; memset(&p, 0, sizeof p);
    p.input_format = INPUT_YUV420_INDIV_8; p.output_format = OUTPUT_RGB_16_565;
    p.rotation = ROTATION_NONE; p.block_alignment = BLOCK_8_BY_8;
    p.input_line_width = (s16)W; p.input_lines = (s16)H;
    p.standard_coefficient = COEFFICIENT_ITU_R_BT_601_SCALING; p.alpha = 0xFF;
    Y2RU_SetConversionParams(&p);
    Y2RU_SetTransferEndInterrupt(true);
    Y2RU_GetTransferEndEvent(&g_y2r_done);
}
static void g_y2r_start(AVFrame *o, C3D_Tex *tex, int W, int H) {
    int cw = W / 2, ch = H / 2;
    GSPGPU_FlushDataCache(o->data[0], (u32)W * H);
    GSPGPU_FlushDataCache(o->data[1], (u32)cw * ch);
    GSPGPU_FlushDataCache(o->data[2], (u32)cw * ch);
    Y2RU_SetSendingY(o->data[0], (u32)W * H, (s16)W, 0);
    Y2RU_SetSendingU(o->data[1], (u32)cw * ch, (s16)cw, 0);
    Y2RU_SetSendingV(o->data[2], (u32)cw * ch, (s16)cw, 0);
    Y2RU_SetReceiving(tex->data, (u32)W * H * 2, (s16)(W * 2 * 8), (s16)((GT_W - W) * 2 * 8));
    svcClearEvent(g_y2r_done); Y2RU_StartConversion();
}
static void g_y2r_wait(C3D_Tex *tex) { svcWaitSynchronization(g_y2r_done, 300000000LL); C3D_TexFlush(tex); }

static void g_panel(C3D_RenderTarget *bot, C2D_Text *ttitle, C2D_Text *ttime, C2D_Text *thint,
                    int64_t cur, int64_t dur, int playing, float vol) {
    u32 wht = C2D_Color32(255, 255, 255, 255), gry = C2D_Color32(150, 150, 150, 255);
    C2D_TargetClear(bot, C2D_Color32(0, 0, 0, 255));
    C2D_SceneBegin(bot);
    C2D_DrawText(ttitle, C2D_WithColor, 6, 4, 0, 0.6f, 0.6f, wht);
    C2D_DrawText(ttime,  C2D_WithColor, 6, 26, 0, 0.5f, 0.5f, gry);
    double frac = dur > 0 ? (double)cur / (double)dur : 0; if (frac > 1) frac = 1;
    C2D_DrawRectSolid(BAR_X, BAR_Y, 0, BAR_W, BAR_H, C2D_Color32(60, 60, 60, 255));
    C2D_DrawRectSolid(BAR_X, BAR_Y, 0, (float)(BAR_W * frac), BAR_H, wht);
    C2D_DrawRectSolid(BAR_X + (float)(BAR_W * frac) - 2, BAR_Y - 3, 0, 5, BAR_H + 6, C2D_Color32(80, 160, 255, 255));
    if (playing) { C2D_DrawRectSolid(PLAY_CX - 9, PLAY_CY - 13, 0, 6, 26, wht); C2D_DrawRectSolid(PLAY_CX + 3, PLAY_CY - 13, 0, 6, 26, wht); }
    else C2D_DrawTriangle(PLAY_CX - 8, PLAY_CY - 13, wht, PLAY_CX - 8, PLAY_CY + 13, wht, PLAY_CX + 12, PLAY_CY, wht, 0);
    C2D_DrawRectSolid(VOL_X, VOL_Y, 0, 12, VOL_H, C2D_Color32(60, 60, 60, 255));
    C2D_DrawRectSolid(VOL_X, VOL_Y + VOL_H * (1.0f - vol / 4.0f), 0, 12, VOL_H * (vol / 4.0f), wht);
    C2D_DrawText(thint, C2D_WithColor, 8, 222, 0, 0.42f, 0.42f, gry);
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

    AVCodecContext ctx; memset(&ctx, 0, sizeof ctx);
    ctx.width = W; ctx.height = H; ctx.priv_data = calloc(1, mobi_ctx_size());
    if (!ctx.priv_data || mobi_init(&ctx) != 0) { free(ctx.priv_data); mfx_close(&m); fclose(f); return MOFLEX_ERROR; }
    mobi_opt = 0x1A0E;
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
    g_y2r_init(W, H);
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
    int priming = 0;   /* fill the ring BEFORE audio starts, so audio-0 lines up with video pair-0 */
    int64_t vskip = -1; /* after a seek, discard video pairs with ts < this (the seek target) so the
                         * video anchors at the SAME content time the audio seeks to, not the earlier
                         * keyframe it lands on -> no constant A/V offset after seek/resume */
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
        g_aw_stop = 0; g_aw_paused = 1; priming = 1;        /* hold audio until the video ring is primed */
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
                    if      (px >= BKB_X && px < BKB_X + BKB_W) { result = MOFLEX_QUIT_BACK; break; }
                    else if (px >= OPB_X && px < OPB_X + OPB_W) { result = MOFLEX_QUIT_OPEN; break; }
                    else if (px >= EXB_X && px < EXB_X + EXB_W) { result = MOFLEX_QUIT_EXIT; break; }
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
                              g_aw_seek_us = seek_to_us; g_aw_seek = 1;
                              g_aw_paused = 1; priming = 1; }   /* re-prime the ring before audio resumes */
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
                /* NO periodic resume write here: an in-loop SD write stalls the serialized FS and hitches
                 * decode every ~15s. Resume is saved on pause/exit only (like the classic path). */
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

        /* once the ring is primed, release audio: its content-0 now lines up with the first video pair
         * we're about to present (instead of audio racing ~ring-fill-time ahead at startup/seek). */
        if (priming && (wr - rd) >= NTB - 3) { priming = 0; if (have_audio) g_aw_paused = 0; }

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
            int64_t vrel = ring_ts[slot] - v_anchor;   /* SMOOTH synthetic per-pair time (not the coarse
                                                        * raw block ts) -> steady audio-slaved cadence */
            if (arel < 0) { if (!shown_landing) show_slot = slot; }   /* audio not started: show landing frame */
            else if (arel + (int64_t)g_vlead_ms * 1000 >= vrel) show_slot = slot;
            if (show_slot >= 0) { rd++; shown_landing = 1; last_shown = show_slot; cur_show_bts = ring_bts[slot];
                                  g_vid_us = cur_show_bts; dirty = 1; if ((wr - rd) < qmin) qmin = wr - rd; }
        }
        /* if we neither decoded nor have a frame to show, don't spin the CPU */
        if (!decoded && show_slot < 0 && !dirty) gspWaitForVBlank();

        if (dirty && last_shown >= 0) {
            int64_t ptime = cur_show_bts;
            /* Rebuild + re-parse the HUD/time text at most ~4x/sec (it used to churn EVERY video frame
             * because 'e' changes -- C2D_TextParse/Optimize per frame is pure CPU stolen from decode). */
            static u64 hud_next = 0;
            u64 hnow = osGetTime();
            if (hnow >= hud_next) {
                hud_next = hnow + 250;
                char tc[16], ts[64];
                fmt_time(ptime, tc, sizeof tc);
                int aerr = (have_audio && g_aud_play_us >= 0 && g_aud_start >= 0)
                           ? (int)(((cur_show_bts - v_anchor) - (g_aud_play_us - g_aud_start)) / 1000) : 0;
                int span = (wr > rd) ? (int)((ring_bts[(wr - 1) % NTB] - ring_bts[rd % NTB]) / 1000) : 0;
                /* d = raw decode capability in-player (stereo pairs/sec). If d>=24 the decoder keeps
                 * up and any deficit is render/present; if d<24 the audio worker is starving decode. */
                int dfps = (dec_ticks > 0) ? (int)((double)SYSCLOCK_ARM11 * dec_frames / (double)dec_ticks / 2.0) : 0;
                dec_ticks = 0; dec_frames = 0;   /* rolling: 'd' reflects the last ~250ms of decode */
                snprintf(ts, sizeof ts, "%s e%d q%d d%d o%d",
                         tc, aerr, wr - rd, dfps, g_vlead_ms);
                if (strcmp(ts, last_ts)) { strncpy(last_ts, ts, sizeof last_ts - 1);
                    C2D_TextBufClear(tmbuf); C2D_TextParse(&ttime, tmbuf, ts); C2D_TextOptimize(&ttime); }
            }
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
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    gfxSetScreenFormat(GFX_BOTTOM, GSP_RGB565_OES);
    gfxSet3D(false);
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

MoflexResult moflex_play(const char *path) {
    /* One path for BOTH consoles now: the classic audio-paced player. It plays 2D fluidly everywhere and
     * 3D perfectly on New 3DS; on Old 3DS 3D just can't keep up (decode is 2x the frames) and degrades --
     * that's expected. The old separate Old-3DS GPU pipeline (moflex_play_gpu) is retired, keeping one
     * proven code path. (moflex_play_gpu stays in the file, unused, for a future asm-decoder 3D revisit.) */
    init_luts();
    vol_load();

    FILE *f = fopen(path, "rb");
    if (!f) { printf("\x1b[2J\x1b[Hfopen failed\npress A\n"); mp_wait_key(); return MOFLEX_ERROR; }

    mobi_opt = 0x1A0E; /* entropy+sparse+prefetch + idct-skip + DC-only: bit-exact, ~6-8% faster decode (esp. Old 3DS) */

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
      if (!isnew && is3d) {
          /* Old-3DS 3D: use the decode-ahead ring + core-1 audio-worker GPU pipeline. Viable now that
           * the 0x1A0E decoder outruns display (~28 vs 24 pairs/sec) -- the ring cushions hard scenes,
           * keeping audio in sync. moflex_play_gpu re-opens the file, so close ours first. */
          mfx_close(&m); fclose(f);
          return moflex_play_gpu(path);
      }
      g_old3d_warn = 0; }
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

    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
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
            gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
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
                          if (!playing) { resume_save_us(path, cur_us); last_save = cur_us;
                                          if (vol_dirty) { vol_save(); vol_dirty = 0; } } }   /* checkpoint + flush volume while stopped (SD write can't stall the read loop) */
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
                          if (!playing) { resume_save_us(path, cur_us); last_save = cur_us;
                                          if (vol_dirty) { vol_save(); vol_dirty = 0; } } }   /* checkpoint + flush volume while stopped */
                    else if (px >= RW_CX - 18 && px <= RW_CX + 18) { seek_to_us = cur_us - 30000000; want_seek = 1; }
                    else if (px >= FF_CX - 18 && px <= FF_CX + 18) { seek_to_us = cur_us + 30000000; want_seek = 1; }
                } else if (py >= BTN_Y && py < BTN_Y + BTN_H) {   /* BACK / OPEN / EXIT */
                    if      (px >= BKB_X && px < BKB_X + BKB_W) { result = MOFLEX_QUIT_BACK; break; }
                    else if (px >= OPB_X && px < OPB_X + OPB_W) { result = MOFLEX_QUIT_OPEN; break; }
                    else if (px >= EXB_X && px < EXB_X + EXB_W) { result = MOFLEX_QUIT_EXIT; break; }
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

/* ---- shared subtitle API for the MP4 player (wrap the statics above) ---- */
void moflex_subs_autoload(const char *moviepath)      { subs_autoload(moviepath); }
void moflex_sub_menu(const char *moviepath, int is3d) { sub_menu(moviepath, is3d); }
void moflex_sub_overlay(int is3d, long long us)       { sub_overlay(is3d, (int64_t)us); }
int  moflex_subs_on(void)                             { return g_sub_on; }
