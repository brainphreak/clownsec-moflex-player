/* Moflex Store -- prototype.
 *
 * One aisle of a video rental store, in stereoscopic 3D, with real posters from the player's
 * own artwork cache on the shelves. This exists to answer three questions before anyone builds
 * the real thing:
 *
 *   1. does the frame rate hold with N textured quads, drawn TWICE for stereo?
 *   2. is walking in first person comfortable on a handheld screen?
 *   3. does the texture budget behave -- 6 MB of VRAM against a 1500-title catalogue?
 *
 * PERFORMANCE NOTES, because that is the point of the exercise:
 *
 *  - Posters are cached ALREADY TILED in the GPU's swizzled layout, at a fixed power-of-two
 *    size. Loading one is then a single fread straight into texture memory: no JPEG decode, no
 *    scaling, no swizzle at runtime. The first run builds that cache from the player's existing
 *    .p565 art; every run after is pure I/O.
 *  - One unit quad in the vertex buffer, reused for every poster with a per-poster matrix.
 *    Nothing about the shelf is uploaded per frame.
 *  - The room is a single draw call sharing one small repeating texture.
 *  - Lighting is baked into vertex colours. The PICA200 has no fragment shaders, only texture
 *    combiners, so per-pixel lighting is not on the table -- and fill rate is the scarce
 *    resource here anyway, with 400x240 drawn twice.
 *
 * Controls: circle pad = walk / turn, A = "select" (prints the title), START = exit.
 */
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include <stdarg.h>
#include <sys/stat.h>

#include "vshader_shbin.h"
#include "font8x8_basic.h"

/* ---------------- poster texture geometry ----------------
 * 128x256 is the smallest power-of-two box that holds a poster at a sane size; the image
 * occupies 128x182 of it, which is the source 132x188 aspect to within half a percent. */
/* SHELF size. A case three metres away covers about forty screen pixels, so 128x256 was four
 * times the texture anyone could see. At 64x128 the same 3 MB of texture holds four times as
 * many titles -- which is what buys three rows and a fuller shop.
 *
 * The case you PICK UP is the one that needs detail, and there is only ever one, so it gets a
 * single 128x256 texture of its own (g_detail), filled from the big cache on pickup. That is
 * the whole LOD scheme: small on the shelf, full in the hand. */
#define TEX_W   64
#define TEX_H   128
#define IMG_W   64
#define IMG_H   91
#define DET_W   128
#define DET_H   256
#define DET_IMG_W 128
#define DET_IMG_H 182
#define TEX_BYTES (TEX_W * TEX_H * 2)
#define VMAX    ((float)IMG_H / (float)TEX_H)

/* moviedata/ holds BOTH a decoded poster and the description, under the same key:
 *   <name>.p565  132x188 RGB565      <name>.nfo  "key: value" title/year/genres/desc
 * art/ is the catalogue's poster cache -- posters only, no text -- so it is the fallback. */
#define DATA_DIR  "sdmc:/moflex_player/moviedata"
#define ART_DIR   "sdmc:/moflex_player/art"
#define CACHE_DIR "sdmc:/moflex_player/store"
#define SRC_W 132
#define SRC_H 188

/* A store floor with freestanding units, not a corridor. The units are low enough to see
 * over (2.0 against a 1.55 eye height puts the sign of the next section in view from
 * anywhere), which is what makes the place read as a shop rather than a maze. */
#define STORE_HX     18.0f      /* floor spans +/-STORE_HX in x */
#define STORE_Z0      2.0f      /* and STORE_Z0 .. -STORE_DEPTH in z */
#define STORE_DEPTH  30.0f
#define CEIL_Y        4.2f
#define UNIT_LEN     12.0f      /* a bay: long enough to run from the wall to the walkway */
#define UNIT_DEPTH    1.0f
#define UNIT_H        2.0f
#define SIGN_Y        3.35f
#define MAX_SECTIONS  8
#define SEC_COLS      3
#define PER_ROW      46         /* spines along a 12-unit bay */
#define SHELF_CAP    (3 * PER_ROW * 2)   /* 3 rows, both faces */

#define MAX_POSTERS 320         /* spines cost no texture; this is only metadata */
#define ROOM_TEX 64

typedef struct { float x, y, z, u, v, s; } Vtx;

typedef struct {
    /* No texture. A case on a shelf is a SPINE: the same mesh and the same tiny texture, tinted
     * a different colour per title -- so a title costs nothing but its metadata and the shelves
     * scale to a whole catalogue. Only the SELECTED one turns face-on and loads a real cover,
     * into the single detail texture, because there is only ever one. */
    u16     tint;
    int     ok;
    float   x, y, z;            /* centre, world space */
    float   ay;                 /* which way the case FACES, in radians. Was a +/-1 flag, which
                                 * could only express four directions and fell apart the moment
                                 * a unit sat at 45 degrees. */
    char    name[80];           /* title, or the filename when there is no .nfo */
    char    genres[80];
    char    desc[400];
    int     year, runtime, hasinfo;
    int     shown;              /* on a shelf on the current page */
    int     is_more;            /* the "MORE MOVIES" case that turns the section over */
    int     sect, order;        /* which bay, and where in that bay's run */
    char    key[96];            /* cache key, so the detail texture can be built on pickup */
    char    srcpath[400];       /* the .p565 it came from, for that lazy build */
    int     src_w, src_h;
} Poster;

typedef struct {
    char    name[24];
    float   cx, cz;             /* unit centre on the floor */
    float   rot;                /* which way the unit runs. Islands all square to the room read
                                 * as crates dropped on a floor; a couple set at an angle make
                                 * it look laid out. */
    C3D_Tex sign;
    int     sign_ok;
    int     n;                  /* titles that belong here, not what fits */
    int     page, pages, cap;   /* a bay holds `cap`; the rest wait behind the MORE case */
    int     more_idx;           /* the MORE case for this bay, -1 if it all fits */
} Section;
static Section g_sec[MAX_SECTIONS];
static int     g_nsec = 0;

static Poster g_pos[MAX_POSTERS];
static int    g_nposters = 0;
static int    g_withinfo = 0;    /* how many came with a description */
/* The same room serves both stores; only the source of the shelves and the VERB differ.
 * Library: take one off the shelf and play it. Catalogue: take one and queue the download. */
enum { STORE_LIBRARY = 0, STORE_CATALOG = 1 };
static int    g_mode = STORE_LIBRARY;
static const char *verb(void) { return g_mode == STORE_CATALOG ? "QUEUE" : "PLAY"; }

/* ---------------- 3DS texture tiling ----------------
 * Textures are stored in 8x8 tiles, and within a tile the pixels are in Morton (z-order):
 * x contributes bits 0,2,4 and y bits 1,3,5. Done once, at cache-build time, never per frame. */
static inline u32 morton8(u32 x, u32 y) {
    x = (x | (x << 2)) & 0x33; x = (x | (x << 1)) & 0x55;
    y = (y | (y << 2)) & 0x33; y = (y | (y << 1)) & 0x55;
    return x | (y << 1);
}
static void tile_rgb565(const u16 *lin, u16 *out, int w, int h) {
    int tw = w / 8;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            u32 t = (u32)(y >> 3) * tw + (x >> 3);
            out[t * 64 + morton8(x & 7, y & 7)] = lin[y * w + x];
        }
}

/* ---------------- poster cache ----------------
 * Source: the player's own art cache, "<key>_<W>x<H>.p565", raw linear RGB565.
 * Destination: "<key>.t565", TEX_W x TEX_H, already tiled. */
static int build_cache_entry_sz(const char *artpath, int sw, int sh, const char *dst,
                                int tw, int th, int iw, int ih) {
    FILE *f = fopen(artpath, "rb");
    if (!f) return 0;
    size_t need = (size_t)sw * sh * 2;
    u16 *src = (u16 *)malloc(need);
    if (!src) { fclose(f); return 0; }
    size_t rd = fread(src, 1, need, f);
    fclose(f);
    if (rd != need) { free(src); return 0; }

    u16 *lin = (u16 *)calloc((size_t)tw * th, 2);
    u16 *til = (u16 *)malloc((size_t)tw * th * 2);
    if (!lin || !til) { free(src); free(lin); free(til); return 0; }
    for (int j = 0; j < ih; j++) {                 /* nearest scale into the used sub-rect */
        const u16 *row = src + (size_t)(j * sh / ih) * sw;
        u16 *d = lin + (size_t)j * tw;
        for (int i = 0; i < iw; i++) d[i] = row[i * sw / iw];
    }
    tile_rgb565(lin, til, tw, th);
    FILE *o = fopen(dst, "wb");
    size_t nb = (size_t)tw * th * 2;
    int ok = o && fwrite(til, 1, nb, o) == nb;
    if (o) fclose(o);
    free(src); free(lin); free(til);
    return ok;
}

/* the player's own .nfo shape: plain "key: value" lines */
static void read_nfo(const char *path, Poster *p) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *nl = strpbrk(line, "\r\n"); if (nl) *nl = 0;
        char *c = strchr(line, ':');
        if (!c) continue;
        *c = 0;
        const char *k = line, *v = c + 1;
        while (*v == ' ' || *v == '\t') v++;
        if      (!strcasecmp(k, "title"))   snprintf(p->name,   sizeof p->name,   "%s", v);
        else if (!strcasecmp(k, "genres"))  snprintf(p->genres, sizeof p->genres, "%s", v);
        else if (!strcasecmp(k, "desc"))    snprintf(p->desc,   sizeof p->desc,   "%s", v);
        else if (!strcasecmp(k, "year"))    p->year    = atoi(v);
        else if (!strcasecmp(k, "runtime")) p->runtime = atoi(v);
    }
    fclose(f);
    p->hasinfo = 1;
}

/* pretty name from "Some_Movie_2011_132x188.p565" */
static void pretty(const char *fn, char *out, size_t cap) {
    char t[128];
    snprintf(t, sizeof t, "%s", fn);
    char *u = strrchr(t, '_');                     /* drop the _WxH */
    if (u) *u = 0;
    for (char *p = t; *p; p++) if (*p == '_') *p = ' ';
    snprintf(out, cap, "%s", t);
}

/* Scan one directory of .p565 posters. moviedata/ files are a fixed 132x188 and carry a sibling
 * .nfo; art/ files put their size in the filename and have no text. */
static int scan_dir(const char *dir, int fixed_w, int fixed_h, int with_nfo, int *built) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int added = 0;
    while ((e = readdir(d)) && g_nposters < MAX_POSTERS) {
        size_t L = strlen(e->d_name);
        if (L < 6 || strcmp(e->d_name + L - 5, ".p565")) continue;
        int sw = fixed_w, sh = fixed_h;
        if (!sw) {                                   /* art/: "<key>_<W>x<H>.p565" */
            const char *u = strrchr(e->d_name, '_');
            if (!u || sscanf(u + 1, "%dx%d.p565", &sw, &sh) != 2) continue;
            if (sw <= 0 || sh <= 0 || sw > 1024 || sh > 1024) continue;
        }
        char key[160];
        snprintf(key, sizeof key, "%.*s", (int)(L - 5), e->d_name);

        char cache[400], big[400], src[400];
        snprintf(cache, sizeof cache, "%s/%s.s565", CACHE_DIR, key);   /* shelf: 64x128  */
        snprintf(big,   sizeof big,   "%s/%s.t565", CACHE_DIR, key);   /* detail: 128x256 */
        snprintf(src,   sizeof src,   "%s/%s", dir, e->d_name);

        (void)cache;
        /* Nothing is decoded or uploaded here now: the shelf needs no cover. Startup is a
         * directory listing and a few .nfo reads, and the big cache is built lazily, once, for
         * whichever case you actually look at. */
        (void)big;
        Poster *p = &g_pos[g_nposters];
        memset(p, 0, sizeof *p);
        snprintf(p->srcpath, sizeof p->srcpath, "%s", src);
        p->src_w = sw; p->src_h = sh;
        { unsigned h = 2166136261u;                        /* spine colour from the title */
          for (const char *c = key; *c; c++) h = (h ^ (unsigned char)*c) * 16777619u;
          static const u16 pal[10] = { 0xF9A6, 0xFB40, 0xFEA0, 0x9FE6, 0x2E8B,
                                       0x4C9F, 0x9A9F, 0xF81F, 0xC618, 0xFD4C };
          p->tint = pal[h % 10]; }
        p->ok = 1;
        snprintf(p->key, sizeof p->key, "%s", key);
        pretty(e->d_name, p->name, sizeof p->name);
        if (with_nfo) {
            char nfo[400];
            snprintf(nfo, sizeof nfo, "%s/%s.nfo", dir, key);
            read_nfo(nfo, p);
        }
        if (p->hasinfo) g_withinfo++;
        g_nposters++; added++;
    }
    closedir(d);
    return added;
}

static int load_posters(int *built) {
    mkdir(CACHE_DIR, 0777);
    /* moviedata first: those entries come with a description, which is what the info panel
     * is for. art/ only tops up the shelf when there is room left. */
    scan_dir(DATA_DIR, SRC_W, SRC_H, 1, built);
    scan_dir(ART_DIR,  0,     0,     0, built);
    return g_nposters;
}

/* a placeholder poster so the prototype still runs on a console with no art cached */
static void make_placeholder(Poster *p, int idx) {
    static const u16 pal[10] = { 0xF9A6, 0xFB40, 0xFEA0, 0x9FE6, 0x2E8B,
                                 0x4C9F, 0x9A9F, 0xF81F, 0xC618, 0xFD4C };
    p->tint = pal[idx % 10];
    p->ok = 1;
    snprintf(p->name, sizeof p->name, "Placeholder %d", idx + 1);
    if (0) {
    u16 *lin = (u16 *)calloc(TEX_W * TEX_H, 2);
    u16 *til = (u16 *)malloc(TEX_BYTES);
    if (!lin || !til) { free(lin); free(til); return; }
    int hue = (idx * 37) % 6;
    for (int y = 0; y < IMG_H; y++)
        for (int x = 0; x < IMG_W; x++) {
            int v = 6 + (y * 20) / IMG_H;
            int r = (hue == 0 || hue == 3 || hue == 5) ? v : v / 3;
            int g = (hue == 1 || hue == 3 || hue == 4) ? v : v / 3;
            int b = (hue == 2 || hue == 4 || hue == 5) ? v : v / 3;
            int border = (x < 4 || x >= IMG_W - 4 || y < 4 || y >= IMG_H - 4);
            lin[y * TEX_W + x] = border ? 0xFFFF
                                        : (u16)(((r & 0x1F) << 11) | ((g * 2 & 0x3F) << 5) | (b & 0x1F));
        }
    tile_rgb565(lin, til, TEX_W, TEX_H);
    free(lin); free(til);
    }
}

/* The bottom screen is composed into a fixed block of lines and emitted in ONE pass from the
 * home position, rather than addressed row by row with escape sequences. Absolute addressing
 * kept going wrong -- rows landing under one another, the console scrolling once anything
 * touched the last line -- and none of that can happen if the whole panel is simply reprinted
 * from the top every frame, each line padded to a constant width so it needs no clearing. */
#define PANEL_ROWS 28
#define PANEL_MAXC 63
/* Taken from the console itself rather than assumed. Guessing at 39 is how the control lines
 * ended up running off the right edge. */
static int  g_cols = 39;
static char g_panel[PANEL_ROWS][PANEL_MAXC + 1];
static void panel_size(void) {
    PrintConsole *c = consoleGetDefault();
    if (c && c->consoleWidth > 8) g_cols = c->consoleWidth - 1;
    if (g_cols > PANEL_MAXC) g_cols = PANEL_MAXC;
}
static void panel_clear(void) {
    for (int r = 0; r < PANEL_ROWS; r++) { memset(g_panel[r], ' ', g_cols); g_panel[r][g_cols] = 0; }
}
static void panel_set(int row, const char *t) {
    if (row < 0 || row >= PANEL_ROWS) return;
    int n = (int)strlen(t);
    if (n > g_cols) n = g_cols;
    memcpy(g_panel[row], t, n);
}
static void panel_fmt(int row, const char *fmt, ...) {
    char b[128]; va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    panel_set(row, b);
}
/* word-wrap into the panel; returns the row after the last one used */
static int panel_wrap(int row, int maxrows, const char *t) {
    int used = 0;
    while (*t && used < maxrows) {
        while (*t == ' ') t++;
        if (!*t) break;
        int n = (int)strlen(t);
        if (n > g_cols - 1) {
            n = g_cols - 1;
            while (n > 0 && t[n] != ' ' && t[n] != 0) n--;
            if (n <= 0) n = g_cols - 1;
        }
        char line[PANEL_MAXC + 2];
        snprintf(line, sizeof line, " %.*s", n, t);
        panel_set(row + used, line);
        t += n; used++;
    }
    return row + used;
}
static void panel_flush(void) {
    printf("\x1b[1;1H");                      /* home, 1-based as ANSI actually specifies */
    for (int r = 0; r < PANEL_ROWS; r++) {
        fputs(g_panel[r], stdout);
        if (r < PANEL_ROWS - 1) fputc('\n', stdout);   /* no newline on the last: never scroll */
    }
}

/* Print `t` word-wrapped into `cols`, starting at console row `row`, at most `maxrows` lines.
 * The console is 40 columns on the bottom screen; a 400-character description is about ten
 * lines, so it fits without scrolling. */
static int wrap_print(int row, int cols, int maxrows, const char *t) {
    char line[64];
    int used = 0;
    while (*t && used < maxrows) {
        while (*t == ' ') t++;
        if (!*t) break;
        int n = (int)strlen(t);
        if (n > cols) {
            n = cols;
            while (n > 0 && t[n] != ' ' && t[n] != 0) n--;   /* back up to a space */
            if (n <= 0) n = cols;                            /* one long word: hard break */
        }
        snprintf(line, sizeof line, "%.*s", n, t);
        printf("\x1b[%d;0H\x1b[2K %s", row + used, line);
        t += n; used++;
    }
    for (int i = used; i < maxrows; i++) printf("\x1b[%d;0H\x1b[2K", row + i);
    return used;
}

/* ---------------- text into a texture ---------------- */
static void draw_glyph(u16 *lin, int W, int H, int x, int y, int sc, u16 col, unsigned c) {
    if (c > 127) c = '?';
    const char *g = font8x8_basic[c];
    for (int r = 0; r < 8; r++)
        for (int b = 0; b < 8; b++)
            if (g[r] & (1 << b))
                for (int sy = 0; sy < sc; sy++)
                    for (int sx = 0; sx < sc; sx++) {
                        int px = x + b * sc + sx, py = y + r * sc + sy;
                        if (px >= 0 && px < W && py >= 0 && py < H) lin[py * W + px] = col;
                    }
}
static void draw_text(u16 *lin, int W, int H, int x, int y, int sc, u16 col, const char *t) {
    for (int i = 0; t[i]; i++) draw_glyph(lin, W, H, x + i * 8 * sc, y, sc, col, (unsigned char)t[i]);
}
/* word-wrapped block; returns the y just past the last line */
static int draw_wrap(u16 *lin, int W, int H, int x, int y, int sc, u16 col,
                     const char *t, int cols, int maxlines, int *truncated) {
    char line[80];
    int used = 0;
    if (truncated) *truncated = 0;
    while (*t && used < maxlines) {
        while (*t == ' ') t++;
        if (!*t) break;
        int n = (int)strlen(t);
        if (n > cols) {
            n = cols;
            while (n > 0 && t[n] != ' ' && t[n] != 0) n--;
            if (n <= 0) n = cols;
        }
        if (n > (int)sizeof line - 1) n = (int)sizeof line - 1;
        snprintf(line, sizeof line, "%.*s", n, t);
        /* last line with more still to come: elide, so it reads as a blurb and not a cut-off */
        if (used == maxlines - 1 && t[n]) {
            int k = (int)strlen(line);
            while (k > 0 && k > cols - 3) line[--k] = 0;
            snprintf(line + k, sizeof line - k, "...");
            if (truncated) *truncated = 1;
        }
        draw_text(lin, W, H, x, y + used * 9 * sc, sc, col, line);
        t += n; used++;
    }
    return y + used * 9 * sc;
}

/* ---------------- hanging section signs ----------------
 * A 256x64 texture with the genre name drawn at 2x from the 8x8 font, centred, on a dark
 * board. Built once at startup; there are only a handful of sections. */
#define SIGN_W 256
#define SIGN_H 64
static void make_sign_tex_col(C3D_Tex *t, const char *text, u16 board, u16 edge, u16 ink, int sc) {
    if (!C3D_TexInit(t, SIGN_W, SIGN_H, GPU_RGB565)) return;
    u16 *lin = (u16 *)calloc(SIGN_W * SIGN_H, 2);
    u16 *til = (u16 *)malloc(SIGN_W * SIGN_H * 2);
    if (!lin || !til) { free(lin); free(til); C3D_TexDelete(t); return; }
    for (int y = 0; y < SIGN_H; y++)
        for (int x = 0; x < SIGN_W; x++) {
            int b = (x < 3 || x >= SIGN_W - 3 || y < 3 || y >= SIGN_H - 3);
            lin[y * SIGN_W + x] = b ? edge : board;
        }
    int len = (int)strlen(text);
    int maxc = (SIGN_W - 16) / (8 * sc);
    if (len > maxc) len = maxc;
    int x0 = (SIGN_W - len * 8 * sc) / 2, y0 = (SIGN_H - 8 * sc) / 2;
    for (int i = 0; i < len; i++)
        draw_glyph(lin, SIGN_W, SIGN_H, x0 + i * 8 * sc, y0, sc, ink, (unsigned char)text[i]);
    tile_rgb565(lin, til, SIGN_W, SIGN_H);
    memcpy(t->data, til, SIGN_W * SIGN_H * 2);
    C3D_TexFlush(t);
    C3D_TexSetFilter(t, GPU_LINEAR, GPU_LINEAR);
    free(lin); free(til);
}
/* The house colours: a deep blue board with a yellow rule and yellow type. Two colours do more
 * for "this is a video shop" than any amount of geometry -- and they are one constant each, so
 * a theme setting could swap the whole place over later. */
#define TH_BLUE   0x0193      /* deep blue board */
#define TH_YELLOW 0xFEA0      /* signage yellow */
#define TH_WHITE  0xFFFF
static void make_sign_tex(C3D_Tex *t, const char *text) {
    make_sign_tex_col(t, text, TH_BLUE, TH_YELLOW, TH_YELLOW, 2);
}
/* store name and fire-exit board */
static C3D_Tex g_storesign, g_exitsign;
static int     g_store_ok = 0, g_exit_ok = 0;

/* The full-resolution front of whatever is in your hand. One texture, filled on pickup. */
static C3D_Tex g_detail;
static int     g_detail_ok = 0, g_detail_for = -1;
static void load_detail(const Poster *q, int idx) {
    if (!g_detail_ok || g_detail_for == idx) return;
    char big[400];
    snprintf(big, sizeof big, "%s/%s.t565", CACHE_DIR, q->key);
    FILE *f = fopen(big, "rb");
    if (!f) {                                   /* built once, the first time you look at it */
        if (!build_cache_entry_sz(q->srcpath, q->src_w, q->src_h, big,
                                  DET_W, DET_H, DET_IMG_W, DET_IMG_H)) return;
        f = fopen(big, "rb");
    }
    if (!f) return;
    size_t got = fread(g_detail.data, 1, (size_t)DET_W * DET_H * 2, f);
    fclose(f);
    if (got != (size_t)DET_W * DET_H * 2) return;
    C3D_TexFlush(&g_detail);
    g_detail_for = idx;
}

/* ---------------- the back of the case ----------------
 * ONE texture, redrawn when you pick something up, because only one case is ever in your hand.
 * That buys a big sheet (256x512, ~256 KB) for the cost of a single poster slot.
 *
 * It is a PROP, not a reading surface: a case filling half of a 400x240 screen gives about six
 * pixels per character, so this is laid out like the back of a VHS box -- title bar, a blurb,
 * runtime, a barcode strip -- while the bottom screen stays where the description is actually
 * read. 256x364 of the sheet is used, which is the same 0.711 aspect as the poster front. */
#define BACK_W 256
#define BACK_H 512
#define BACK_USED 364
static C3D_Tex g_back;
static int     g_back_ok = 0, g_back_for = -1;

static void rebuild_back(const Poster *q) {
    if (!g_back_ok) return;
    u16 *lin = (u16 *)calloc(BACK_W * BACK_H, 2);
    u16 *til = (u16 *)malloc(BACK_W * BACK_H * 2);
    if (!lin || !til) { free(lin); free(til); return; }
    const u16 card = 0x2124, bar = 0x8000, ink = 0xFFFF, dim = 0xC618, edge = 0x630C;

    /* Everything below is measured off these, so nothing can run past an edge or land on the
     * footer -- both of which happened when the positions were hand-picked constants. */
    const int M = 14;                       /* margin on every side */
    const int TXT_W = BACK_W - 2 * M;
    const int FOOT_H = 96;                  /* runtime + barcode + year live here */
    const int FOOT_Y = BACK_USED - FOOT_H;

    for (int y = 0; y < BACK_USED; y++)
        for (int x = 0; x < BACK_W; x++) {
            int b = (x < 4 || x >= BACK_W - 4 || y < 4 || y >= BACK_USED - 4);
            lin[y * BACK_W + x] = b ? edge : card;
        }
    for (int y = 10; y < 46; y++)                        /* title bar */
        for (int x = M - 4; x < BACK_W - (M - 4); x++) lin[y * BACK_W + x] = bar;
    { int cols = TXT_W / 16;                             /* 2x glyphs are 16 px wide */
      char t[40]; snprintf(t, sizeof t, "%.*s", cols, q->name);
      draw_text(lin, BACK_W, BACK_H, M, 18, 2, ink, t); }

    int y = 54;
    if (q->genres[0]) {
        char g[48]; snprintf(g, sizeof g, "%.*s", TXT_W / 8, q->genres);
        draw_text(lin, BACK_W, BACK_H, M, y, 1, dim, g);
        y += 14;
    }
    y += 6;

    /* The blurb at 2x. At 1x an 8-pixel glyph lands at roughly six screen pixels on a held
     * case and is genuinely unreadable; 2x is about twelve and legible. The cost is fourteen
     * characters a line, so what fits here is a blurb -- the bottom screen keeps the rest. */
    { int cols  = TXT_W / 16;
      int lines = (FOOT_Y - y) / 18;
      int trunc = 0;
      if (lines > 0) {
          if (q->desc[0]) draw_wrap(lin, BACK_W, BACK_H, M, y, 2, ink, q->desc, cols, lines, &trunc);
          else            draw_text(lin, BACK_W, BACK_H, M, y, 1, dim, "No description on file.");
      } }

    { char rt[32];
      if (q->runtime) snprintf(rt, sizeof rt, "RUNNING TIME %d MIN", q->runtime);
      else            snprintf(rt, sizeof rt, "RUNNING TIME --");
      draw_text(lin, BACK_W, BACK_H, M, FOOT_Y + 6, 1, dim, rt); }

    { int bw = 96, bx = M, by = FOOT_Y + 26, bh = 34;    /* barcode */
      for (int x = 0; x < bw; x++) {
          if (!(((x * 7919) >> 3) & 1)) continue;
          for (int yy = by; yy < by + bh && yy < BACK_USED - 6; yy++)
              lin[yy * BACK_W + (bx + x)] = ink;
      } }

    if (q->year) {                                       /* right-aligned FROM ITS OWN WIDTH --
                                                          * a fixed x assumed four digits and
                                                          * hung off the edge */
        char yr[16]; snprintf(yr, sizeof yr, "%d", q->year);
        int w = (int)strlen(yr) * 16;
        int x = BACK_W - M - w;
        if (x < M) x = M;
        draw_text(lin, BACK_W, BACK_H, x, FOOT_Y + 30, 2, dim, yr);
    }

    tile_rgb565(lin, til, BACK_W, BACK_H);
    memcpy(g_back.data, til, BACK_W * BACK_H * 2);
    C3D_TexFlush(&g_back);
    free(lin); free(til);
}

/* ---------------- materials ----------------
 * A shop is mostly told by its surfaces: red carpet underfoot, brown wood shelving, painted
 * walls. Each is a 64x64 repeating texture -- 8 KB apiece -- and the shell is drawn as ranges
 * of one vertex buffer, so the whole room is still a handful of draws. */
static C3D_Tex g_room;      /* walls + ceiling */
static C3D_Tex g_carpet, g_wood, g_glass, g_door;
static int     g_mat_ok = 0;

/* Bind, but never hand the GPU a C3D_Tex that was never created. An uninitialised one is a
 * garbage pointer, and the hardware does not fault on that -- it wedges, and the console goes
 * with it: no HOME, no START, power cycle. Every bind goes through here now. */
static C3D_Tex *g_lastgood = NULL;
static void bind_tex(C3D_Tex *t, int ok) {
    if (ok && t && t->data) { C3D_TexBind(0, t); g_lastgood = t; }
    else if (g_lastgood)     C3D_TexBind(0, g_lastgood);
}
static void upload_tex(C3D_Tex *t, u16 *lin, int w, int h) {
    u16 *til = (u16 *)malloc((size_t)w * h * 2);
    if (!til) return;
    tile_rgb565(lin, til, w, h);
    memcpy(t->data, til, (size_t)w * h * 2);
    C3D_TexFlush(t);
    C3D_TexSetFilter(t, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(t, GPU_REPEAT, GPU_REPEAT);
    free(til);
}
/* What you can see through the shopfront: a car park at night. Drawn once at 256x128 -- big
 * enough that a car reads as a car -- and shown whole on each window rather than tiled, so it
 * is a view rather than wallpaper. */
static C3D_Tex g_outside;
static int     g_outside_ok = 0;
static void px(u16 *l, int W, int H, int x, int y, u16 c) {
    if (x >= 0 && x < W && y >= 0 && y < H) l[y * W + x] = c;
}
static void box2(u16 *l, int W, int H, int x0, int y0, int x1, int y1, u16 c) {
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) px(l, W, H, x, y, c);
}
static void make_outside_tex(void) {
    const int W = 256, H = 128;
    if (!C3D_TexInit(&g_outside, W, H, GPU_RGB565)) return;
    u16 *lin = (u16 *)malloc(W * H * 2);
    u16 *til = (u16 *)malloc(W * H * 2);
    if (!lin || !til) { free(lin); free(til); C3D_TexDelete(&g_outside); return; }
    const u16 sky = 0x0821, far_ = 0x18E3, lot = 0x2124, bay = 0x6B4D;
    const u16 lampglow = 0xFF98, win = 0xFDA0;
    const u16 carcol[4] = { 0x8000, 0x0011, 0x7BEF, 0xA145 };

    for (int y = 0; y < H; y++)                              /* sky, darker toward the top */
        for (int x = 0; x < W; x++)
            lin[y * W + x] = (y < 52) ? (u16)(sky + ((y / 14) << 5)) : lot;
    for (int i = 0; i < 40; i++) px(lin, W, H, (i * 6197) % W, (i * 977) % 44, 0x8410);  /* stars */

    /* a low skyline with lit windows */
    for (int b = 0; b < 7; b++) {
        int bx = 4 + b * 36, bw = 18 + (b * 7) % 14, bh = 12 + (b * 11) % 20;
        box2(lin, W, H, bx, 52 - bh, bx + bw, 51, far_);
        for (int wy = 52 - bh + 3; wy < 50; wy += 5)
            for (int wx = bx + 2; wx < bx + bw - 2; wx += 5)
                if (((wx * 7 + wy * 3) % 5) < 2) box2(lin, W, H, wx, wy, wx + 1, wy + 2, win);
    }
    /* painted bays */
    for (int i = 0; i < 9; i++) box2(lin, W, H, 10 + i * 28, 74, 11 + i * 28, 104, bay);
    box2(lin, W, H, 0, 70, W - 1, 71, bay);

    /* cars: body, cabin, wheels, and a pair of lights */
    for (int c = 0; c < 4; c++) {
        int cx = 18 + c * 62, cy = 84 + (c % 2) * 10;
        u16 col = carcol[c % 4];
        box2(lin, W, H, cx, cy, cx + 34, cy + 11, col);          /* body */
        box2(lin, W, H, cx + 8, cy - 6, cx + 25, cy - 1, col);    /* cabin */
        box2(lin, W, H, cx + 10, cy - 5, cx + 23, cy - 2, 0x2965);/* glass */
        box2(lin, W, H, cx + 4, cy + 11, cx + 9, cy + 14, 0x1082);
        box2(lin, W, H, cx + 25, cy + 11, cx + 30, cy + 14, 0x1082);
        box2(lin, W, H, cx + 33, cy + 3, cx + 34, cy + 5, 0xFFE0);/* headlight */
        box2(lin, W, H, cx, cy + 3, cx + 1, cy + 5, 0xF800);      /* tail light */
    }
    /* lamp posts with a pool of light */
    for (int l = 0; l < 3; l++) {
        int lx = 40 + l * 80;
        box2(lin, W, H, lx, 30, lx + 1, 74, 0x39E7);
        box2(lin, W, H, lx - 5, 28, lx + 6, 31, lampglow);
        for (int r = 1; r < 16; r++)
            for (int x = lx - r; x <= lx + r; x++)
                if (((x + r) % 3) == 0) px(lin, W, H, x, 74 + r / 2, 0x4A69);
    }
    /* the window itself: frame and a centre mullion */
    for (int y = 0; y < H; y++) { px(lin, W, H, 0, y, 0x4208); px(lin, W, H, 1, y, 0x4208);
                                  px(lin, W, H, W-1, y, 0x4208); px(lin, W, H, W-2, y, 0x4208);
                                  px(lin, W, H, W/2, y, 0x4208); px(lin, W, H, W/2+1, y, 0x4208); }
    for (int x = 0; x < W; x++) { px(lin, W, H, x, 0, 0x4208); px(lin, W, H, x, 1, 0x4208);
                                  px(lin, W, H, x, H-1, 0x4208); px(lin, W, H, x, H-2, 0x4208); }
    tile_rgb565(lin, til, W, H);
    memcpy(g_outside.data, til, W * H * 2);
    C3D_TexFlush(&g_outside);
    C3D_TexSetFilter(&g_outside, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&g_outside, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    free(lin); free(til);
    g_outside_ok = 1;
}

static void make_materials(void) {
    const int N = 64;
    u16 *lin = (u16 *)malloc(N * N * 2);
    if (!lin) return;
    C3D_TexInit(&g_carpet, N, N, GPU_RGB565);          /* blue-grey commercial carpet */
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
        int n = ((x * 13 + y * 7) % 5) + ((x ^ y) & 1);
        int r = 7 + (n >> 1), g = 17 + n, b = 11 + (n >> 1);
        if (x % 32 == 0 || y % 32 == 0) { r = 9; g = 21; b = 14; }   /* faint weave lines */
        lin[y * N + x] = (u16)((r << 11) | (g << 5) | b);
    }
    upload_tex(&g_carpet, lin, N, N);
    C3D_TexInit(&g_wood, N, N, GPU_RGB565);            /* brown planks with grain */
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
        int grain = ((x * 5 + ((y >> 3) * 3)) % 11);
        int v = 9 + (grain > 8 ? 3 : grain > 5 ? 1 : 0);
        int r = v, g = (v * 2) / 3, b = v / 3;
        if (x % 16 == 0) { r = 6; g = 4; b = 2; }
        lin[y * N + x] = (u16)((r << 11) | ((g * 2) << 5) | b);
    }
    upload_tex(&g_wood, lin, N, N);
    C3D_TexInit(&g_glass, N, N, GPU_RGB565);           /* plain glass: the fallback, and it has
                                                        * to EXIST -- it is bound whenever the
                                                        * car park fails to build */
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
        int frame = (x < 3 || x >= N - 3 || y < 3 || y >= N - 3 || x == N / 2);
        int glow = (y > N - 22 && ((x * 11) % 23) < 3) ? 8 : 0;
        lin[y * N + x] = frame ? 0x4208 : (u16)((glow << 11) | ((2 + glow) << 5) | (6 + glow));
    }
    upload_tex(&g_glass, lin, N, N);
    free(lin);
    make_outside_tex();                                /* the car park, drawn at its own size */
    lin = (u16 *)malloc(N * N * 2);
    if (!lin) return;
    C3D_TexInit(&g_door, N, N, GPU_RGB565);            /* panelled door with a handle */
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
        int panel = (x > 8 && x < N - 8 && ((y > 8 && y < 28) || (y > 36 && y < 56)));
        int v = panel ? 12 : 8;
        lin[y * N + x] = (u16)((v << 11) | (((v * 2) / 3 * 2) << 5) | (v / 3));
        if (x > N - 16 && x < N - 11 && y > 30 && y < 36) lin[y * N + x] = 0xFFE0;
    }
    upload_tex(&g_door, lin, N, N);
    free(lin);
    g_mat_ok = 1;
}

/* PAINT, not a grid. The old one drew a line every 16 texels, which tiled into graph paper
 * across every wall in the shop. This is a flat warm colour with a little roller mottle -- the
 * mottle only exists so a large flat wall does not band. */
/* The edge of a case: caps top and bottom, a label band near the top, and a shadow/highlight
 * pair down the sides so a row reads as separate objects rather than one striped wall. Drawn
 * white, because the per-title tint does the colouring. */
static C3D_Tex g_spine;
static int     g_spine_ok = 0;
static void make_spine_tex(void) {
    const int W = 16, H = 64;
    if (!C3D_TexInit(&g_spine, W, H, GPU_RGB565)) return;
    u16 *lin = (u16 *)malloc(W * H * 2);
    u16 *til = (u16 *)malloc(W * H * 2);
    if (!lin || !til) { free(lin); free(til); C3D_TexDelete(&g_spine); return; }
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            u16 v = 0xFFFF;
            if (x <= 1)               v = 0x8410;           /* shadowed left edge */
            if (y < 3 || y > H - 4)   v = 0x6B4D;           /* caps */
            else if (y > 9 && y < 18) v = 0xD69A;           /* label band */
            lin[y * W + x] = v;
        }
    tile_rgb565(lin, til, W, H);
    memcpy(g_spine.data, til, W * H * 2);
    C3D_TexFlush(&g_spine);
    C3D_TexSetFilter(&g_spine, GPU_NEAREST, GPU_NEAREST);
    free(lin); free(til);
    g_spine_ok = 1;
}

/* Framed posters: the walls and the ends of the units. Real covers from the shelves rather
 * than invented art -- a video shop advertises what it has in stock. Six of them at 64x128 is
 * 96 KB, which is the whole decorating budget.
 *
 * If you later want fixed art up there instead, this is the hook: drop a 132x188 .p565 in as
 * store/wallN.p565 and it will be used in preference to a title from the shelves. */
#define WALLPOSTERS 6
static C3D_Tex g_wall[WALLPOSTERS];
static int     g_wall_ok[WALLPOSTERS];
static int     g_wall_n = 0;

static void make_wall_posters(void) {
    for (int i = 0; i < WALLPOSTERS; i++) {
        /* Your own art first: store/wallN.p565, a 132x188 raw like the player's own caches.
         * It is only ever READ -- the scaled copy goes to a separate file, so dropping art in
         * here can never destroy it. */
        char user[400], small[400];
        snprintf(user,  sizeof user,  "%s/wall%d.p565", CACHE_DIR, i);
        snprintf(small, sizeof small, "%s/wall%d.w565", CACHE_DIR, i);
        int from_user = 0;
        { FILE *uf = fopen(user, "rb");
          if (uf) { fclose(uf); from_user = 1; } }
        Poster *q = NULL;
        if (!from_user) {
            /* otherwise borrow a title from the shelves, spread across the catalogue */
            if (g_nposters <= 0) return;
            int pick = (int)((long)i * g_nposters / WALLPOSTERS);
            if (pick >= g_nposters) pick = g_nposters - 1;
            q = &g_pos[pick];
            if (!q->srcpath[0]) continue;
            snprintf(small, sizeof small, "%s/%s.w565", CACHE_DIR, q->key);
        }
        FILE *cf = fopen(small, "rb");
        if (!cf) {
            const char *src = from_user ? user : q->srcpath;
            int sw = from_user ? SRC_W : q->src_w, sh = from_user ? SRC_H : q->src_h;
            if (!build_cache_entry_sz(src, sw, sh, small, TEX_W, TEX_H, IMG_W, IMG_H)) continue;
            cf = fopen(small, "rb");
            if (!cf) continue;
        }
        if (!C3D_TexInit(&g_wall[i], TEX_W, TEX_H, GPU_RGB565)) { fclose(cf); continue; }
        size_t got = fread(g_wall[i].data, 1, (size_t)TEX_W * TEX_H * 2, cf);
        fclose(cf);
        if (got != (size_t)TEX_W * TEX_H * 2) { C3D_TexDelete(&g_wall[i]); continue; }
        C3D_TexFlush(&g_wall[i]);
        C3D_TexSetFilter(&g_wall[i], GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&g_wall[i], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        g_wall_ok[i] = 1; g_wall_n++;
    }
}

static void make_room_tex(void) {
    C3D_TexInit(&g_room, ROOM_TEX, ROOM_TEX, GPU_RGB565);
    u16 *lin = (u16 *)malloc(ROOM_TEX * ROOM_TEX * 2);
    u16 *til = (u16 *)malloc(ROOM_TEX * ROOM_TEX * 2);
    for (int y = 0; y < ROOM_TEX; y++)
        for (int x = 0; x < ROOM_TEX; x++) {
            int n = ((x * 37 + y * 17) % 7) + ((x * 5 ^ y * 3) % 3);   /* soft mottle */
            int r = 26 + (n >> 2), g = 52 + (n >> 1), b = 24 + (n >> 2);
            if (r > 31) r = 31; if (g > 63) g = 63; if (b > 31) b = 31;
            lin[y * ROOM_TEX + x] = (u16)((r << 11) | (g << 5) | b);
        }
    tile_rgb565(lin, til, ROOM_TEX, ROOM_TEX);
    memcpy(g_room.data, til, ROOM_TEX * ROOM_TEX * 2);
    C3D_TexFlush(&g_room);
    C3D_TexSetFilter(&g_room, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&g_room, GPU_REPEAT, GPU_REPEAT);
    free(lin); free(til);
}

/* A row of anonymous cases, for the shelves against the walls.
 *
 * There is no point spending a texture slot on a title nobody can read from across the room --
 * this one strip, repeated, reads as a wall of stock at any distance you would actually see it
 * from. The real titles live on the units you can walk up to. */
static C3D_Tex g_covers;
static int     g_covers_ok = 0;
static void make_covers_tex(void) {
    const int W = 128, H = 64;
    if (!C3D_TexInit(&g_covers, W, H, GPU_RGB565)) return;
    u16 *lin = (u16 *)calloc(W * H, 2);
    u16 *til = (u16 *)malloc(W * H * 2);
    if (!lin || !til) { free(lin); free(til); C3D_TexDelete(&g_covers); return; }
    for (int i = 0; i < W * H; i++) lin[i] = 0x1082;              /* shadowed gap behind */
    const u16 pal[8] = { 0xF800, 0xFD20, 0xFFE0, 0x07E0, 0x04FF, 0x781F, 0xFB56, 0xAD55 };
    int x = 2;
    for (int c = 0; x < W - 4; c++) {
        int w = 7 + (c * 5) % 4;                                   /* varied spine widths */
        u16 col = pal[(c * 3) % 8];
        u16 dark = (u16)((col >> 1) & 0x7BEF);
        for (int yy = 6; yy < H - 4; yy++)
            for (int xx = x; xx < x + w && xx < W; xx++) {
                int edge = (xx == x || xx == x + w - 1 || yy == 6 || yy == H - 5);
                int band = (yy < 14);                              /* a title band on each */
                lin[yy * W + xx] = edge ? dark : (band ? 0xFFFF : col);
            }
        x += w + 2;
    }
    tile_rgb565(lin, til, W, H);
    memcpy(g_covers.data, til, W * H * 2);
    C3D_TexFlush(&g_covers);
    C3D_TexSetFilter(&g_covers, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&g_covers, GPU_REPEAT, GPU_REPEAT);
    free(lin); free(til);
    g_covers_ok = 1;
}

/* Step the selection to the next case in a screen direction.
 *
 * Scored in the CAMERA's frame, not the shelf's, so "right" always means right on screen no
 * matter which side of a unit you are standing on. Aiming an analog stick at a case is fiddly
 * -- and would be hopeless once these are spines ten pixels wide -- so the d-pad walks the
 * shelf discretely instead. */
static int step_sel(int cur, float dirx, float diry, float dirz, float cx, float cz, float eye);
static int step_sel(int cur, float dirx, float diry, float dirz, float cx, float cz, float eye) {
    if (cur < 0 || cur >= g_nposters) return cur;
    float ox = g_pos[cur].x, oy = g_pos[cur].y, oz = g_pos[cur].z;
    int best = cur; float bestscore = 1e9f;
    for (int i = 0; i < g_nposters; i++) {
        if (i == cur || !g_pos[i].ok || !g_pos[i].shown) continue;
        float dx = g_pos[i].x - ox, dy = g_pos[i].y - oy, dz = g_pos[i].z - oz;
        float along = dx * dirx + dy * diry + dz * dirz;
        if (along < 0.05f) continue;                       /* must be in the pressed direction */
        float px = dx - along * dirx, py = dy - along * diry, pz = dz - along * dirz;
        float perp = sqrtf(px * px + py * py + pz * pz);
        if (perp > 0.75f) continue;                        /* not on this run of shelf */
        /* nearest along the axis, penalising drift off it */
        float score = along + perp * 2.5f;
        /* and stay on the face you are actually looking at */
        float fx = g_pos[i].x - cx, fz = g_pos[i].z - cz;
        if (fx * fx + fz * fz > 36.0f) continue;
        (void)eye;
        if (score < bestscore) { bestscore = score; best = i; }
    }
    return best;
}

/* ---------------- sections ----------------
 * The first genre named in a title's .nfo decides its section. The most populous genres get a
 * unit each; whatever is left over goes to a general section, because a shop with a shelf
 * holding one film looks broken. */
static void first_genre(const char *g, char *out, size_t cap) {
    if (!g || !g[0]) { snprintf(out, cap, "GENERAL"); return; }
    size_t j = 0;
    for (const char *p = g; *p && *p != ',' && j + 1 < cap; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c -= 32;
        out[j++] = c;
    }
    out[j] = 0;
    while (j > 0 && out[j - 1] == ' ') out[--j] = 0;
    if (!out[0]) snprintf(out, cap, "GENERAL");
}

static void place_section(int k);
static void build_sections(void) {
    /* static: at 320 titles these are 9 KB, and a .3dsx main thread has little to spare */
    static char names[MAX_POSTERS][24];
    static int  count[MAX_POSTERS];
    int  uniq = 0;
    for (int i = 0; i < g_nposters; i++) {
        char g[24]; first_genre(g_pos[i].genres, g, sizeof g);
        int k = -1;
        for (int j = 0; j < uniq; j++) if (!strcmp(names[j], g)) { k = j; break; }
        if (k < 0 && uniq < MAX_POSTERS) { k = uniq++; snprintf(names[k], 24, "%s", g); count[k] = 0; }
        if (k >= 0) count[k]++;
    }
    /* biggest genres first, capped at MAX_SECTIONS-1 so there is always room for GENERAL */
    for (int a = 0; a < uniq; a++)
        for (int b = a + 1; b < uniq; b++)
            if (count[b] > count[a]) {
                int t = count[a]; count[a] = count[b]; count[b] = t;
                char tmp[24]; memcpy(tmp, names[a], 24); memcpy(names[a], names[b], 24);
                memcpy(names[b], tmp, 24);
            }
    int want = uniq < (MAX_SECTIONS - 1) ? uniq : (MAX_SECTIONS - 1);
    for (int i = 0; i < want; i++) {
        if (count[i] < 2) break;                       /* not worth a whole unit */
        snprintf(g_sec[g_nsec].name, 24, "%s", names[i]);
        g_nsec++;
    }
    snprintf(g_sec[g_nsec].name, 24, "GENERAL");
    g_nsec++;

    /* A FLOOR PLAN, not a grid. Evenly spaced islands square to the room leave a hall of empty
     * carpet and read as crates; a real shop runs units in ranks with walking aisles between
     * them and turns the back corners in to face you as you come down the room. */
    /* Bays, the way a rental shop is actually laid out: units run OUT FROM THE WALLS with
     * their far end against the wall, leaving a clear walkway up the middle of the room that
     * reaches every section. Islands floating in open carpet read as crates; this reads as a
     * shop you can navigate. The back corners turn in to close the room off. */
    static const float PLAN[MAX_SECTIONS][3] = {   /* x, z, rotation */
        { -11.6f,  -5.0f, 0.0f },   /* left wall, three bays */
        { -11.6f, -12.0f, 0.0f },
        { -11.6f, -19.0f, 0.0f },
        {  11.6f,  -5.0f, 0.0f },   /* right wall, three bays */
        {  11.6f, -12.0f, 0.0f },
        {  11.6f, -19.0f, 0.0f },
        {  -7.0f, -25.5f, 0.0f },   /* across the back */
        {   7.0f, -25.5f, 0.0f },
    };
    for (int i = 0; i < g_nsec; i++) {
        g_sec[i].cx  = PLAN[i][0];
        g_sec[i].cz  = PLAN[i][1];
        g_sec[i].rot = PLAN[i][2];
        make_sign_tex(&g_sec[i].sign, g_sec[i].name);
        g_sec[i].sign_ok = 1;
    }

    /* Which bay each title belongs to, and where in that bay's run. */
    for (int i = 0; i < g_nposters; i++) {
        char g[24]; first_genre(g_pos[i].genres, g, sizeof g);
        int k = g_nsec - 1;                            /* GENERAL unless a section matches */
        for (int j = 0; j < g_nsec; j++) if (!strcmp(g_sec[j].name, g)) { k = j; break; }
        g_pos[i].sect  = k;
        g_pos[i].order = g_sec[k].n++;
    }

    /* A bay that cannot hold its whole genre gets a MORE case in the top-left slot: pick it up,
     * press the verb, and the shelf turns over to the next lot. Only where it is needed -- a
     * bay with room to spare should not carry a control nobody has to press. */
    for (int k = 0; k < g_nsec; k++) {
        g_sec[k].cap = SHELF_CAP;
        g_sec[k].more_idx = -1;
        g_sec[k].page = 0;
        if (g_sec[k].n > SHELF_CAP && g_nposters < MAX_POSTERS) {
            g_sec[k].cap = SHELF_CAP - 1;              /* the MORE case takes a slot */
            int m = g_nposters++;
            memset(&g_pos[m], 0, sizeof g_pos[m]);
            g_pos[m].ok = 1; g_pos[m].is_more = 1; g_pos[m].sect = k;
            g_pos[m].tint = TH_YELLOW;
            snprintf(g_pos[m].name, sizeof g_pos[m].name, "MORE %s", g_sec[k].name);
            g_sec[k].more_idx = m;
        }
        int cap = g_sec[k].cap > 0 ? g_sec[k].cap : 1;
        g_sec[k].pages = (g_sec[k].n + cap - 1) / cap;
        if (g_sec[k].pages < 1) g_sec[k].pages = 1;
    }
    for (int k = 0; k < g_nsec; k++) place_section(k);
}

/* Position one bay's stock for its current page. Called again when the MORE case is used. */
static void place_section(int k) {
    Section *S = &g_sec[k];
    int first = S->page * S->cap, last = first + S->cap;
    int base = (S->more_idx >= 0) ? 1 : 0;             /* the MORE case owns the top-left */
    int onshow = S->n - first;
    if (onshow > S->cap) onshow = S->cap;
    if (onshow < 0) onshow = 0;
    int total_slots = onshow + base;

    for (int i = 0; i < g_nposters; i++) {
        Poster *p = &g_pos[i];
        if (!p->ok || p->sect != k) continue;
        int sl;
        if (p->is_more) sl = 0;
        else {
            if (p->order < first || p->order >= last) { p->shown = 0; continue; }
            sl = base + (p->order - first);
        }
        p->shown = 1;
        int per_face = 3 * PER_ROW;
        int face = (sl / per_face) & 1;
        int idx  = sl % per_face;
        int row  = 2 - (idx / PER_ROW), colp = idx % PER_ROW;
        int inrow = total_slots - (2 - row) * PER_ROW;  /* rows fill from the top */
        if (inrow > PER_ROW) inrow = PER_ROW;
        if (inrow < 1) inrow = 1;
        float lx = (colp - (inrow - 1) * 0.5f) * 0.235f;
        float lz = face ? -(UNIT_DEPTH * 0.5f + 0.02f) : (UNIT_DEPTH * 0.5f + 0.02f);
        float ca = cosf(S->rot), sa = sinf(S->rot);
        p->x  = S->cx + lx * ca + lz * sa;
        p->z  = S->cz - lx * sa + lz * ca;
        p->y  = 0.46f + row * 0.62f;
        p->ay = S->rot + (face ? C3D_Angle(0.5f) : 0.0f);
    }
}

/* ---------------- geometry ---------------- */
#define ROOM_VTX     1200        /* shell + units + counter + wall shelving */
static Vtx *g_roomv, *g_quadv, *g_signv;
static void *g_roomvbo, *g_quadvbo, *g_signvbo;

/* Room geometry is pushed into a fixed buffer, and every new fitting adds to it. Running off
 * the end writes into whatever linear memory follows and then hands it to the GPU, which is the
 * other way to wedge the console. Refuse instead. */
static int g_room_full = 0;
static void push_quad(Vtx *v, int *n,
                      float ax, float ay, float az, float bx, float by, float bz,
                      float cx, float cy, float cz, float dx, float dy, float dz,
                      float ur, float vr, float shade) {
    if (*n + 6 > ROOM_VTX) { g_room_full = 1; return; }
    /* two triangles, wound so the front face is toward the aisle */
    Vtx q[6] = {
        {ax, ay, az, 0,  0,  shade}, {bx, by, bz, ur, 0,  shade}, {cx, cy, cz, ur, vr, shade},
        {ax, ay, az, 0,  0,  shade}, {cx, cy, cz, ur, vr, shade}, {dx, dy, dz, 0,  vr, shade},
    };
    memcpy(v + *n, q, sizeof q);
    *n += 6;
}

/* things you cannot walk through: the shelf units, plus the counter */
typedef struct { float cx, cz, hx, hz, rot; } Blocker;
/* sections + the L returns + the counter + the returns bin. Sized with room to spare: the
 * L returns were added without growing this, which is one past the end. */
static Blocker g_block[MAX_SECTIONS * 2 + 8];
static int     g_nblock = 0;

/* a box turned about its own centre */
static void push_box_rot(Vtx *v, int *n, float cx, float cy, float cz,
                         float hx, float hy, float hz, float rot,
                         float ur, float vr, float sh) {
    float ca = cosf(rot), sa = sinf(rot);
    #define RX(lx, lz) (cx + (lx) * ca + (lz) * sa)
    #define RZ(lx, lz) (cz - (lx) * sa + (lz) * ca)
    float y0 = cy - hy, y1 = cy + hy;
    /* the four uprights, then the top */
    push_quad(v, n, RX(-hx,hz),y0,RZ(-hx,hz), RX(hx,hz),y0,RZ(hx,hz),
                    RX(hx,hz),y1,RZ(hx,hz),   RX(-hx,hz),y1,RZ(-hx,hz), ur, vr, sh);
    push_quad(v, n, RX(hx,-hz),y0,RZ(hx,-hz), RX(-hx,-hz),y0,RZ(-hx,-hz),
                    RX(-hx,-hz),y1,RZ(-hx,-hz), RX(hx,-hz),y1,RZ(hx,-hz), ur, vr, sh);
    push_quad(v, n, RX(-hx,-hz),y0,RZ(-hx,-hz), RX(-hx,hz),y0,RZ(-hx,hz),
                    RX(-hx,hz),y1,RZ(-hx,hz),   RX(-hx,-hz),y1,RZ(-hx,-hz), 1, vr, sh * 0.86f);
    push_quad(v, n, RX(hx,hz),y0,RZ(hx,hz), RX(hx,-hz),y0,RZ(hx,-hz),
                    RX(hx,-hz),y1,RZ(hx,-hz), RX(hx,hz),y1,RZ(hx,hz), 1, vr, sh * 0.86f);
    push_quad(v, n, RX(-hx,-hz),y1,RZ(-hx,-hz), RX(hx,-hz),y1,RZ(hx,-hz),
                    RX(hx,hz),y1,RZ(hx,hz),     RX(-hx,hz),y1,RZ(-hx,hz), ur, 1, sh * 1.15f);
    #undef RX
    #undef RZ
}
static void push_box(Vtx *v, int *n, float cx, float cy, float cz,
                     float hx, float hy, float hz, float ur, float vr, float sh) {
    float x0 = cx - hx, x1 = cx + hx, y0 = cy - hy, y1 = cy + hy, z0 = cz - hz, z1 = cz + hz;
    push_quad(v, n, x0,y0,z1, x1,y0,z1, x1,y1,z1, x0,y1,z1, ur, vr, sh);          /* +z */
    push_quad(v, n, x1,y0,z0, x0,y0,z0, x0,y1,z0, x1,y1,z0, ur, vr, sh);          /* -z */
    push_quad(v, n, x0,y0,z0, x0,y0,z1, x0,y1,z1, x0,y1,z0, 1, vr, sh * 0.86f);   /* -x */
    push_quad(v, n, x1,y0,z1, x1,y0,z0, x1,y1,z0, x1,y1,z1, 1, vr, sh * 0.86f);   /* +x */
    push_quad(v, n, x0,y1,z0, x1,y1,z0, x1,y1,z1, x0,y1,z1, ur, 1, sh * 1.15f);   /* top */
}

static int g_n_floor, g_n_shell, g_n_units, g_n_cover, g_n_light;
static int build_room(void) {
    g_roomv = (Vtx *)linearAlloc(sizeof(Vtx) * ROOM_VTX);
    int n = 0;
    const float X = STORE_HX, Z0 = STORE_Z0, Z1 = STORE_Z0 - STORE_DEPTH, H = CEIL_Y;
    /* group 1: the carpet */
    push_quad(g_roomv, &n, -X, 0, Z0,  X, 0, Z0,  X, 0, Z1, -X, 0, Z1, 14, 12, 0.88f);
    g_n_floor = n;
    /* group 2: ceiling + four walls */
    push_quad(g_roomv, &n, -X, H, Z1,  X, H, Z1,  X, H, Z0, -X, H, Z0, 12, 10, 0.78f);
    push_quad(g_roomv, &n, -X, 0, Z1, -X, 0, Z0, -X, H, Z0, -X, H, Z1, 10, 2, 0.86f);
    push_quad(g_roomv, &n,  X, 0, Z0,  X, 0, Z1,  X, H, Z1,  X, H, Z0, 10, 2, 0.86f);
    push_quad(g_roomv, &n, -X, 0, Z1,  X, 0, Z1,  X, H, Z1, -X, H, Z1, 12, 2, 0.82f);
    push_quad(g_roomv, &n,  X, 0, Z0, -X, 0, Z0, -X, H, Z0,  X, H, Z0, 12, 2, 0.82f);
    g_n_shell = n - g_n_floor;

    /* one shelf unit per section: a box you can see over, with a lighter top so it reads as a
     * surface rather than a wall */
    for (int i = 0; i < g_nsec; i++) {
        push_box_rot(g_roomv, &n, g_sec[i].cx, UNIT_H * 0.5f, g_sec[i].cz,
                     UNIT_LEN * 0.5f, UNIT_H * 0.5f, UNIT_DEPTH * 0.5f, g_sec[i].rot,
                     3, 1, 0.52f);
    }
    /* the counter: a long wood block by the door, a register on top, and a returns box */
    push_box(g_roomv, &n, -12.0f, 0.55f, -1.6f, 4.0f, 0.55f, 0.7f, 4, 1, 0.62f);
    push_box(g_roomv, &n, -13.6f, 1.28f, -1.6f, 0.6f, 0.18f, 0.45f, 1, 1, 0.40f);  /* register base */
    push_box(g_roomv, &n, -13.6f, 1.60f, -1.75f, 0.5f, 0.14f, 0.22f, 1, 1, 0.78f); /* its screen */
    push_box(g_roomv, &n,  -8.2f, 0.60f, -1.6f, 0.9f, 0.60f, 0.6f, 1, 1, 0.50f);   /* returns bin */
    /* An L on the end of two bays: a short return that turns the corner, which is what stops a
     * rank of units reading as a row of identical slabs. */
    for (int i = 0; i < g_nsec && i < 6; i += 2) {
        float ex = g_sec[i].cx + ((g_sec[i].cx < 0) ? UNIT_LEN * 0.5f : -UNIT_LEN * 0.5f);
        push_box_rot(g_roomv, &n, ex, UNIT_H * 0.5f, g_sec[i].cz + 1.6f,
                     UNIT_DEPTH * 0.5f, UNIT_H * 0.5f, 1.6f, 0.0f, 1, 1, 0.48f);
    }
    g_n_units = n - g_n_floor - g_n_shell;

    /* The strip of anonymous covers along the walls is gone. It was a stand-in for stock we
     * could not afford to texture, and beside real spines on real units it read as wallpaper --
     * the one thing in the room that looked painted on. The bays hold the stock now. */
    g_n_cover = 0;

    /* Strip lights. Nothing is actually lit -- this GPU has no lights and the shading is baked
     * -- but a bright white fitting under the ceiling reads as one, and it is what stops the
     * room feeling like a basement. Their own group, so they get a white texture instead of
     * the wood the shelving uses. */
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 2; c++) {
            float lz = -3.0f - r * 11.0f;
            float lx = (c ? 1.0f : -1.0f) * 9.0f;
            push_box(g_roomv, &n, lx, CEIL_Y - 0.14f, lz, 7.0f, 0.055f, 0.30f, 1, 1, 1.0f);
        }
    g_n_light = n - g_n_floor - g_n_shell - g_n_units - g_n_cover;
    if (g_room_full) {           /* say so rather than draw something that was never written */
        g_n_light = 0;
    }

    /* blockers: every unit, plus the counter and the returns bin */
    g_nblock = 0;
    for (int i = 0; i < g_nsec; i++) {
        g_block[g_nblock].cx = g_sec[i].cx; g_block[g_nblock].cz = g_sec[i].cz;
        g_block[g_nblock].hx = UNIT_LEN * 0.5f + 0.42f;
        g_block[g_nblock].hz = UNIT_DEPTH * 0.5f + 0.42f;
        g_block[g_nblock].rot = g_sec[i].rot; g_nblock++;
    }
    for (int i = 0; i < g_nsec && i < 6; i += 2) {
        float ex = g_sec[i].cx + ((g_sec[i].cx < 0) ? UNIT_LEN * 0.5f : -UNIT_LEN * 0.5f);
        g_block[g_nblock++] = (Blocker){ ex, g_sec[i].cz + 1.6f,
                                         UNIT_DEPTH * 0.5f + 0.42f, 1.6f + 0.42f, 0.0f };
    }
    g_block[g_nblock++] = (Blocker){ -12.0f, -1.6f, 4.4f, 1.1f, 0.0f };
    g_block[g_nblock++] = (Blocker){  -8.2f, -1.6f, 1.3f, 1.0f, 0.0f };
    return n;
}

/* The held case is a BOX, not a card: a VHS has thickness and you notice its absence the
 * moment you turn one over. Laid out as three ranges in one buffer so each can take its own
 * texture: front is the poster, back is the printed card, the four edges are plain.
 *
 * The back face's u runs the other way. Spin the case 180 degrees and world +x is on your
 * LEFT, so without mirroring, every line of text on the back reads backwards. */
#define BOX_T 0.055f
static Vtx *g_boxv;
static void *g_boxvbo;
static void build_box(void) {
    g_boxv = (Vtx *)linearAlloc(sizeof(Vtx) * 36);
    const float h = 0.5f, t = BOX_T * 0.5f;
    const float vlo = 1.0f - VMAX, vhi = 1.0f;
    Vtx v[36] = {
        /* front (+z): poster */
        {-h,-h, t, 0,vlo,1.0f}, { h,-h, t, 1,vlo,1.0f}, { h, h, t, 1,vhi,1.0f},
        {-h,-h, t, 0,vlo,1.0f}, { h, h, t, 1,vhi,1.0f}, {-h, h, t, 0,vhi,1.0f},
        /* back (-z): printed card, u mirrored */
        { h,-h,-t, 0,vlo,0.92f}, {-h,-h,-t, 1,vlo,0.92f}, {-h, h,-t, 1,vhi,0.92f},
        { h,-h,-t, 0,vlo,0.92f}, {-h, h,-t, 1,vhi,0.92f}, { h, h,-t, 0,vhi,0.92f},
        /* edges: left, right, top, bottom */
        {-h,-h,-t, 0,0,0.55f}, {-h,-h, t, 1,0,0.55f}, {-h, h, t, 1,1,0.55f},
        {-h,-h,-t, 0,0,0.55f}, {-h, h, t, 1,1,0.55f}, {-h, h,-t, 0,1,0.55f},
        { h,-h, t, 0,0,0.55f}, { h,-h,-t, 1,0,0.55f}, { h, h,-t, 1,1,0.55f},
        { h,-h, t, 0,0,0.55f}, { h, h,-t, 1,1,0.55f}, { h, h, t, 0,1,0.55f},
        {-h, h, t, 0,0,0.72f}, { h, h, t, 1,0,0.72f}, { h, h,-t, 1,1,0.72f},
        {-h, h, t, 0,0,0.72f}, { h, h,-t, 1,1,0.72f}, {-h, h,-t, 0,1,0.72f},
        {-h,-h,-t, 0,0,0.40f}, { h,-h,-t, 1,0,0.40f}, { h,-h, t, 1,1,0.40f},
        {-h,-h,-t, 0,0,0.40f}, { h,-h, t, 1,1,0.40f}, {-h,-h, t, 0,1,0.40f},
    };
    memcpy(g_boxv, v, sizeof v);
}

/* full-texture quad for the signs (the poster quad only maps the used part of its box) */
static void build_signquad(void) {
    g_signv = (Vtx *)linearAlloc(sizeof(Vtx) * 6);
    Vtx q[6] = {
        {-0.5f, -0.5f, 0, 0.0f, 0.0f, 1.0f}, { 0.5f, -0.5f, 0, 1.0f, 0.0f, 1.0f},
        { 0.5f,  0.5f, 0, 1.0f, 1.0f, 1.0f}, {-0.5f, -0.5f, 0, 0.0f, 0.0f, 1.0f},
        { 0.5f,  0.5f, 0, 1.0f, 1.0f, 1.0f}, {-0.5f,  0.5f, 0, 0.0f, 1.0f, 1.0f},
    };
    memcpy(g_signv, q, sizeof q);
}

/* Unit quad in the XY plane, reused for every poster.
 *
 * Written out rather than pushed through push_quad because of the V axis: on the PICA200 v=0
 * is the BOTTOM of the texture, not the top. The poster occupies texture rows 0..IMG_H-1 --
 * that is, the TOP of the image data -- so in texture coordinates it lives between
 * 1-VMAX and 1, not between 0 and VMAX. Getting that backwards samples the black padding for
 * the lower half of the quad and shows the poster flipped and shifted up into the rest. */
static void build_quad(void) {
    g_quadv = (Vtx *)linearAlloc(sizeof(Vtx) * 6);
    const float vlo = 1.0f - VMAX;      /* quad bottom  -> last row of the image */
    const float vhi = 1.0f;             /* quad top     -> first row of the image */
    Vtx q[6] = {
        {-0.5f, -0.5f, 0, 0.0f, vlo, 1.0f},
        { 0.5f, -0.5f, 0, 1.0f, vlo, 1.0f},
        { 0.5f,  0.5f, 0, 1.0f, vhi, 1.0f},
        {-0.5f, -0.5f, 0, 0.0f, vlo, 1.0f},
        { 0.5f,  0.5f, 0, 1.0f, vhi, 1.0f},
        {-0.5f,  0.5f, 0, 0.0f, vhi, 1.0f},
    };
    memcpy(g_quadv, q, sizeof q);
}

/* ---------------- main ---------------- */
static DVLB_s *vsh_dvlb;
static shaderProgram_s program;
static int uLocProjection, uLocModelview, uLocTint;

static void scene_init(void) {
    vsh_dvlb = DVLB_ParseFile((u32 *)vshader_shbin, vshader_shbin_size);
    shaderProgramInit(&program);
    shaderProgramSetVsh(&program, &vsh_dvlb->DVLE[0]);
    C3D_BindProgram(&program);
    uLocProjection = shaderInstanceGetUniformLocation(program.vertexShader, "projection");
    uLocModelview  = shaderInstanceGetUniformLocation(program.vertexShader, "modelView");
    uLocTint       = shaderInstanceGetUniformLocation(program.vertexShader, "tint");

    C3D_AttrInfo *ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);   /* position */
    AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);   /* texcoord */
    AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 1);   /* baked shade */

    /* texture * vertex colour, in one combiner stage. Nothing per-pixel beyond this. */
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);

    /* Depth test ON: posters overlap the walls and each other down the aisle, and without it
     * they draw in submission order. */
    C3D_DepthTest(true, GPU_GEQUAL, GPU_WRITE_ALL);
    /* Culling OFF for the prototype. A quad facing the wrong way silently disappears, which is
     * the least informative failure there is; the geometry here is far too small for culling
     * to be worth that risk. */
    C3D_CullFace(GPU_CULL_NONE);
}

/* rgb565 -> the shader's tint uniform */
static void set_tint(u16 c) {
    float r = ((c >> 11) & 0x1F) / 31.0f, g = ((c >> 5) & 0x3F) / 63.0f, b = (c & 0x1F) / 31.0f;
    C3D_FVUnifSet(GPU_VERTEX_SHADER, uLocTint, r, g, b, 1.0f);
}
static void set_buf(void *vbo, int nverts) {
    C3D_BufInfo *buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, vbo, sizeof(Vtx), 3, 0x210);
    (void)nverts;
}

int main(void) {
    gfxInitDefault();
    gfxSet3D(true);                       /* the entire point */
    consoleInit(GFX_BOTTOM, NULL);
    /* Single-buffer the bottom screen, exactly as the player does (mp4_play.c:328). The console
     * writes into whichever back buffer is current, and this rewrites only the lines that
     * changed -- so with two buffers the pair hold different text and alternate every frame.
     * That is the dark band sweeping across and the letters fading in and out. */
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
    panel_size();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

    C3D_RenderTarget *tL = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTarget *tR = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(tL, GFX_TOP, GFX_LEFT,
        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    C3D_RenderTargetSetOutput(tR, GFX_TOP, GFX_RIGHT,
        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));

    scene_init();
    make_room_tex();
    make_materials();
    make_covers_tex();
    make_spine_tex();
    make_sign_tex_col(&g_storesign, "3DS VIDEO RENTALS", TH_BLUE, TH_YELLOW, TH_YELLOW, 1);
    g_store_ok = 1;
    /* the exit board stays green: that one is a fire sign, not branding */
    make_sign_tex_col(&g_exitsign,  "EXIT",              0x0140, 0x07E0, TH_WHITE, 2);
    g_exit_ok = 1;
    build_quad();
    build_signquad();
    build_box();
    g_detail_ok = C3D_TexInit(&g_detail, DET_W, DET_H, GPU_RGB565);
    if (g_detail_ok) { C3D_TexSetFilter(&g_detail, GPU_LINEAR, GPU_LINEAR);
                       C3D_TexSetWrap(&g_detail, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE); }
    g_back_ok = C3D_TexInit(&g_back, BACK_W, BACK_H, GPU_RGB565);
    if (g_back_ok) { C3D_TexSetFilter(&g_back, GPU_LINEAR, GPU_LINEAR);
                     C3D_TexSetWrap(&g_back, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE); }

    int built = 0;
    u64 t_load0 = osGetTime();
    int found = load_posters(&built);
    u64 t_load = osGetTime() - t_load0;
    int placeheld = 0;
    if (found < 12) {                       /* top up so the aisle is never half empty */
        for (int i = found; i < 24 && i < MAX_POSTERS; i++) {
            make_placeholder(&g_pos[i], i);
            if (g_pos[i].ok) { g_nposters++; placeheld++; }
        }
    }

    build_sections();                       /* genres -> units -> poster positions */
    make_wall_posters();                    /* decorate: unit ends and the bare walls */
    int roomn = build_room();               /* needs the unit positions */
    g_roomvbo = g_roomv; g_quadvbo = g_quadv; g_signvbo = g_signv; g_boxvbo = g_boxv;

    float cx = 0, cz = STORE_Z0 - 1.5f, yaw = 0, pitch = 0;
    const float EYE = 1.55f;
    /* Which way "forward" is, in one place, read by BOTH movement and picking so they cannot
     * disagree again -- which is exactly what went wrong: movement used (+sin, -cos) while
     * picking used (-sin, -cos). The x terms were opposite. Walking down the aisle hides it
     * completely, because at yaw 0 the x term is zero; it only shows when you face a wall,
     * where forward and back swap over. */
    const float FWD = 1.0f;
    int frames = 0, fps = 0; u64 t0 = osGetTime();
    int sel = -1;
    /* "grab": A pulls the highlighted case off the shelf and turns it to face you, B puts it
     * back. hold_t runs 0..1 so both directions are the same animation played either way --
     * and a case swinging out toward your face is the clearest demonstration of the stereo. */
    int   held = -1;
    float hold_t = 0.0f;
    float spin   = 0.0f;      /* radians about the case's own y axis while it is in your hand */
    /* How far down the view axis the held case sits. Pulling it closer is the zoom, and it is
     * also what makes the printed back legible: the same texture over more screen pixels. */
    float hold_d = 0.78f;
    const float HOLD_NEAR = 0.34f, HOLD_FAR = 1.15f;
    int   aim_lock = 0;       /* the d-pad picked something: ease the view onto it */
    /* Picking a case up is LOOKING, and must stay free of consequences -- you turn it over,
     * read the back, and put it back. The verb is a separate deliberate press while it is in
     * your hand: rent it in the catalogue store, play it in the library store. Y rather than a
     * second A, so a stray double-tap cannot commit anything. */
    char  toast[48] = ""; int toast_t = 0;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kd = hidKeysDown();
        if (kd & KEY_START) break;
        if ((kd & KEY_A) && held < 0 && sel >= 0) {                /* take it off the shelf */
            held = sel; spin = 0.0f; hold_d = 0.78f;
            if (g_back_for != sel) { rebuild_back(&g_pos[sel]); g_back_for = sel; }
            load_detail(&g_pos[sel], sel);          /* small on the shelf, full in the hand */
        }
        if ((kd & KEY_B) && held >= 0)            held = -1;       /* put it back, no consequence */
        if ((kd & KEY_Y) && held >= 0 && g_pos[held].is_more) {
            /* restock this bay with the next lot */
            int k = g_pos[held].sect;
            g_sec[k].page = (g_sec[k].page + 1) % g_sec[k].pages;
            place_section(k);
            int lo = g_sec[k].page * g_sec[k].cap + 1;
            int hi = lo + g_sec[k].cap - 1;
            if (hi > g_sec[k].n) hi = g_sec[k].n;
            snprintf(toast, sizeof toast, "%s  %d-%d of %d", g_sec[k].name, lo, hi, g_sec[k].n);
            toast_t = 150;
            held = -1; sel = -1; aim_lock = 0;
        } else if ((kd & KEY_Y) && held >= 0) {
            /* the commitment. In the player this queues the download (catalogue) or starts
             * playback (library); here it just reports what it would do. */
            snprintf(toast, sizeof toast, "%s  %.24s", verb(), g_pos[held].name);
            toast_t = 150;
            held = -1;
        }
        if (toast_t > 0) toast_t--;
        hold_t += ((held >= 0) ? 0.14f : -0.14f);                  /* ~7 frames each way */
        if (hold_t > 1.0f) hold_t = 1.0f;
        if (hold_t < 0.0f) hold_t = 0.0f;

        circlePosition cp; hidCircleRead(&cp);
        float fx = cp.dx / 156.0f, fy = cp.dy / 156.0f;
        /* Deadzone. This went missing in an edit, and without it stick rest-drift never reads
         * as zero -- so the held case slowly zoomed itself to a limit and the view crept. */
        if (fabsf(fx) < 0.15f) fx = 0.0f;
        if (fabsf(fy) < 0.15f) fy = 0.0f;
        if (held >= 0) {
            /* left/right turns the case over, up/down pulls it closer or pushes it away */
            spin += fx * 0.075f;
            if (fabsf(fx) < 0.15f) {                 /* let go and it settles to a face */
                float snap = (spin < 0 ? -1.0f : 1.0f) * 3.14159265f
                             * (float)((int)(fabsf(spin) / 3.14159265f + 0.5f));
                spin += (snap - spin) * 0.18f;
            }
            hold_d -= fy * 0.020f;
            if (hold_d < HOLD_NEAR) hold_d = HOLD_NEAR;
            if (hold_d > HOLD_FAR)  hold_d = HOLD_FAR;
            fx = 0; fy = 0;                          /* neither axis walks you while holding */
        }
        /* The circle pad MOVES and the d-pad LOOKS. Turning used to be on the circle pad's x
         * axis, which meant you could not walk diagonally at all: pushing the pad at an angle
         * spun you instead of sliding you sideways. Movement is now purely translation in the
         * direction you are facing, which is what "walk toward what I am looking at" means. */
        u32 kh = hidKeysHeld();
        /* Pitch STAYS where you put it. It used to spring back to level when the d-pad was
         * released, which no first-person game does -- you look down at the bottom shelf and
         * it drifts off it while you are still reading. X snaps back to level instead. */
        if (kd & KEY_X) { pitch = 0.0f; aim_lock = 0; }
        if (pitch >  0.55f) pitch =  0.55f;
        if (pitch < -0.55f) pitch = -0.55f;
        /* Walk away and the shelf lets go, so the d-pad goes back to looking around. Without
         * this the selection stayed latched from across the room and there was no way to tilt
         * up at the signs or the ceiling. */
        if (held < 0 && sel >= 0) {
            float dx = g_pos[sel].x - cx, dz = g_pos[sel].z - cz;
            if (dx * dx + dz * dz > 16.0f) { sel = -1; aim_lock = 0; }
        }
        if (held < 0 && (kd & KEY_B) && sel >= 0) { sel = -1; aim_lock = 0; }  /* let go on purpose */

        /* NOTHING SELECTED: the d-pad is a head. Look up at the signs, round the room. */
        if (sel < 0 && held < 0) {
            if (kh & KEY_DUP)    pitch += 0.030f;
            if (kh & KEY_DDOWN)  pitch -= 0.030f;
            if (kh & KEY_DLEFT)  yaw   += 0.035f;
            if (kh & KEY_DRIGHT) yaw   -= 0.035f;
            if (pitch >  0.75f) pitch =  0.75f;
            if (pitch < -0.60f) pitch = -0.60f;
        }
        /* SOMETHING SELECTED: the d-pad steps along the shelf and the camera eases onto it.
         * Turning stays on the analog stick, so the two never fight over an axis. */
        if (sel >= 0 && (kd & (KEY_DLEFT | KEY_DRIGHT | KEY_DUP | KEY_DDOWN))) {
            float rx = cosf(yaw), rz = -sinf(yaw);        /* camera right, on the floor plane */
            int nsel = sel;
            if (kd & KEY_DRIGHT) nsel = step_sel(sel,  rx, 0,  rz, cx, cz, EYE);
            if (kd & KEY_DLEFT)  nsel = step_sel(sel, -rx, 0, -rz, cx, cz, EYE);
            if (kd & KEY_DUP)    nsel = step_sel(sel, 0,  1, 0, cx, cz, EYE);
            if (kd & KEY_DDOWN)  nsel = step_sel(sel, 0, -1, 0, cx, cz, EYE);
            if (nsel != sel) {
                sel = nsel; aim_lock = 1;
                if (held >= 0) {          /* holding one: swap it for the next along the shelf */
                    held = sel; spin = 0.0f;
                    rebuild_back(&g_pos[sel]); g_back_for = sel;
                    load_detail(&g_pos[sel], sel);
                }
            }
        }
        if (fabsf(fx) < 0.15f) fx = 0;
        if (fabsf(fy) < 0.15f) fy = 0;
        /* Single-stick, the way the console's own games do it: the pad's x axis TURNS you and
         * its y axis walks. Strafing is real but rare, so it sits on the shoulder buttons where
         * it costs nothing to ignore. */
        if (fx != 0.0f) aim_lock = 0;                    /* touch the stick and you take over */
        yaw -= fx * 0.045f;
        if (aim_lock && sel >= 0) {
            /* ease onto the selected case rather than snapping: a snap in stereo is jarring */
            float dx = g_pos[sel].x - cx, dz = g_pos[sel].z - cz;
            float dy = g_pos[sel].y - EYE;
            float want_yaw = atan2f(-dx, -dz);
            float dyaw = want_yaw - yaw;
            while (dyaw >  3.14159265f) dyaw -= 6.28318531f;
            while (dyaw < -3.14159265f) dyaw += 6.28318531f;
            yaw += dyaw * 0.22f;
            float want_pitch = atan2f(dy, sqrtf(dx * dx + dz * dz));
            pitch += (want_pitch - pitch) * 0.22f;
        }
        float fwx = FWD * -sinf(yaw), fwz = FWD * -cosf(yaw);
        float rgx =  cosf(yaw),       rgz = -sinf(yaw);
        float strafe = ((kh & KEY_R) ? 1.0f : 0.0f) - ((kh & KEY_L) ? 1.0f : 0.0f);
        cx  += (fwx * fy + rgx * strafe * 0.75f) * 0.09f;
        cz  += (fwz * fy + rgz * strafe * 0.75f) * 0.09f;
        if (cx >  STORE_HX - 0.6f) cx =  STORE_HX - 0.6f;
        if (cx < -STORE_HX + 0.6f) cx = -STORE_HX + 0.6f;
        if (cz >  STORE_Z0 - 0.6f) cz =  STORE_Z0 - 0.6f;
        if (cz < STORE_Z0 - STORE_DEPTH + 0.6f) cz = STORE_Z0 - STORE_DEPTH + 0.6f;
        /* keep out of the shelf units: push to the nearest face of whichever box you are in.
         * Crude, but a box is a box and you cannot walk through one. */
        for (int i = 0; i < g_nblock; i++) {
            /* test in the unit's OWN frame, so a turned unit blocks along its real sides
             * rather than along an invisible square */
            float ca = cosf(g_block[i].rot), sa = sinf(g_block[i].rot);
            float dx = cx - g_block[i].cx, dz = cz - g_block[i].cz;
            float lx =  dx * ca - dz * sa, lz = dx * sa + dz * ca;
            float hx = g_block[i].hx, hz = g_block[i].hz;
            if (fabsf(lx) < hx && fabsf(lz) < hz) {
                float ox = hx - fabsf(lx), oz = hz - fabsf(lz);
                if (ox < oz) lx = (lx < 0 ? -hx : hx);
                else         lz = (lz < 0 ? -hz : hz);
                cx = g_block[i].cx + lx * ca + lz * sa;
                cz = g_block[i].cz - lx * sa + lz * ca;
            }
        }

        /* what am I looking at? nearest poster ahead, within reach.
         * Frozen while a case is held: the selection IS the held case until it goes back. */
        if (held < 0 && !aim_lock) {
            /* Pick in THREE dimensions. Distance used to ignore y entirely, so the two rows of
             * a shelf were exactly equidistant and the first one in the array always won --
             * the bottom row could never be selected however you stood. Now the aim direction
             * carries pitch and the score is the angle to the case, so looking down picks the
             * lower row the way you would expect. */
            sel = -1;
            float cp_ = cosf(pitch);
            float ax = fwx * cp_, ay = sinf(pitch), az = fwz * cp_;
            float bestscore = 0.80f;                 /* minimum cos(angle) to count as "aimed at" */
            for (int i = 0; i < g_nposters; i++) {
                if (!g_pos[i].shown) continue;
                float dx = g_pos[i].x - cx, dy = g_pos[i].y - EYE, dz = g_pos[i].z - cz;
                float d = sqrtf(dx * dx + dy * dy + dz * dz);
                if (d > 3.2f || d < 1e-4f) continue;
                float dot = (dx * ax + dy * ay + dz * az) / d;
                if (dot < bestscore) continue;
                bestscore = dot; sel = i;            /* the best-aimed case wins, not the nearest */
            }
        }

        float slider = osGet3DSliderState();
        float iod = slider * 0.28f;                 /* gentle: an aisle already has lots of depth */

        C3D_Mtx view;
        Mtx_Identity(&view);
        Mtx_RotateX(&view, -pitch * FWD, true);
        Mtx_RotateY(&view, -yaw, true);
        Mtx_Translate(&view, -cx, -EYE, -cz, true);

        /* ONE frame, both eyes inside it. Begin/End per eye submits two command buffers a
         * frame and waits twice -- it halves the rate for nothing. */
        int eyes = (slider > 0.0f) ? 2 : 1;
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        for (int eye = 0; eye < eyes; eye++) {
            C3D_RenderTarget *tgt = eye ? tR : tL;
            C3D_Mtx proj;
            Mtx_PerspStereoTilt(&proj, C3D_AngleFromDegrees(58.0f), C3D_AspectRatioTop,
                                0.05f, 60.0f, eye ? iod : -iod, 2.2f, false);
            C3D_RenderTargetClear(tgt, C3D_CLEAR_ALL, 0x101418FF, 0);
            C3D_FrameDrawOn(tgt);
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocProjection, &proj);
            /* Start every eye at full white. The tint is only ASSIGNED inside the spine loop,
             * so on the first frame it held whatever the uniform powers up as -- zero -- and
             * every surface in the room was multiplied by it. A black screen that looked like
             * a hang. Anything that does not want a tint must say so. */
            set_tint(0xFFFF);

            /* the shell, three materials, three draws */
            set_buf(g_roomvbo, roomn);
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocModelview, &view);
            bind_tex(g_mat_ok ? &g_carpet : &g_room, 1);
            C3D_DrawArrays(GPU_TRIANGLES, 0, g_n_floor);
            bind_tex(&g_room, 1);
            C3D_DrawArrays(GPU_TRIANGLES, g_n_floor, g_n_shell);
            bind_tex(g_mat_ok ? &g_wood : &g_room, 1);
            C3D_DrawArrays(GPU_TRIANGLES, g_n_floor + g_n_shell, g_n_units);
            if (g_covers_ok) {
                bind_tex(&g_covers, g_covers_ok);
                C3D_DrawArrays(GPU_TRIANGLES, g_n_floor + g_n_shell + g_n_units, g_n_cover);
            }
            if (g_spine_ok && g_n_light > 0) {          /* the spine sheet is plain white */
                bind_tex(&g_spine, g_spine_ok);
                C3D_DrawArrays(GPU_TRIANGLES,
                               g_n_floor + g_n_shell + g_n_units + g_n_cover, g_n_light);
            }

            /* shopfront fittings: windows and a door on the near wall, signs above */
            set_buf(g_signvbo, 6);
            if (g_mat_ok) {
                bind_tex(g_outside_ok ? &g_outside : &g_glass, 1);
                for (int w = 0; w < 4; w++) {
                    float wx = -13.5f + w * 9.0f;
                    if (w == 2) continue;                  /* the door goes in this gap */
                    C3D_Mtx m; Mtx_Copy(&m, &view);
                    Mtx_Translate(&m, wx, 1.9f, STORE_Z0 - 0.05f, true);
                    Mtx_Scale(&m, 6.0f, 2.6f, 1.0f);
                    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocModelview, &m);
                    C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
                }
                bind_tex(&g_door, g_mat_ok);
                { C3D_Mtx m; Mtx_Copy(&m, &view);
                  Mtx_Translate(&m, 4.5f, 1.15f, STORE_Z0 - 0.05f, true);
                  Mtx_Scale(&m, 2.6f, 2.3f, 1.0f);
                  C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocModelview, &m);
                  C3D_DrawArrays(GPU_TRIANGLES, 0, 6); }
            }
            if (g_store_ok) {                              /* name across the back wall */
                bind_tex(&g_storesign, g_store_ok);
                C3D_Mtx m; Mtx_Copy(&m, &view);
                Mtx_Translate(&m, 0.0f, 3.3f, STORE_Z0 - STORE_DEPTH + 0.06f, true);
                Mtx_Scale(&m, 15.0f, 15.0f * (float)SIGN_H / (float)SIGN_W, 1.0f);
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocModelview, &m);
                C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
            }
            if (g_exit_ok) {                               /* over the door */
                bind_tex(&g_exitsign, g_exit_ok);
                C3D_Mtx m; Mtx_Copy(&m, &view);
                Mtx_Translate(&m, 4.5f, 2.75f, STORE_Z0 - 0.10f, true);
                Mtx_RotateY(&m, C3D_Angle(0.5f), true);
                Mtx_Scale(&m, 1.8f, 1.8f * (float)SIGN_H / (float)SIGN_W, 1.0f);
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocModelview, &m);
                C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
            }

            /* The shelves: every case is the SAME spine mesh with a different tint. No cover
             * textures at all, which is what lets a whole catalogue stand on these shelves --
             * and a ten-pixel spine could not show a title anyway. The selected one turns
             * face-on below and shows the real cover, because only ever one is selected. */
            set_buf(g_quadvbo, 6);
            bind_tex(&g_spine, g_spine_ok);
            for (int i = 0; i < g_nposters; i++) {
                if (!g_pos[i].ok || !g_pos[i].shown || i == held || i == sel) continue;
                C3D_Mtx m;
                Mtx_Copy(&m, &view);
                Mtx_Translate(&m, g_pos[i].x, g_pos[i].y, g_pos[i].z, true);
                Mtx_RotateY(&m, g_pos[i].ay, true);
                Mtx_Scale(&m, 0.20f, 0.56f, 1.0f);           /* an edge, not a face */
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocModelview, &m);
                set_tint(g_pos[i].tint);
                C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
            }
            set_tint(0xFFFF);

            /* the selected case: proud of the shelf and turned to face you, wearing its real
             * cover. This is the whole reason spines are affordable -- you only ever need one */
            if (sel >= 0 && sel != held && g_pos[sel].ok) {
                Poster *q = &g_pos[sel];
                float nx = sinf(q->ay), nz = cosf(q->ay);     /* the way this case faces */
                C3D_Mtx m;
                Mtx_Copy(&m, &view);
                Mtx_Translate(&m, q->x + nx * 0.22f, q->y, q->z + nz * 0.22f, true);
                Mtx_RotateY(&m, yaw, true);                  /* square to the viewer */
                Mtx_Scale(&m, 0.40f, 0.40f * (float)DET_IMG_H / (float)DET_IMG_W, 1.0f);
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocModelview, &m);
                bind_tex((g_detail_ok && g_detail_for == sel) ? &g_detail : &g_spine, 1);
                C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
            }

            /* The case in your hand, as a box with thickness.
             *
             * Placed RELATIVE TO THE CAMERA, including pitch, so it stays dead centre however
             * you are looking. It used to sit at a fixed world point at eye height, which slid
             * off the screen the moment you looked down. */
            if (held >= 0 && hold_t > 0.0f && g_pos[held].ok) {
                Poster *q = &g_pos[held];
                float t = hold_t * hold_t * (3.0f - 2.0f * hold_t);
                float cp_ = cosf(pitch);
                float f3x = fwx * cp_, f3y = sinf(pitch), f3z = fwz * cp_;
                const float D = hold_d;
                /* where it is coming FROM: its slot on the shelf */
                float sx = q->x, sy = q->y, sz = q->z;
                float ay0 = q->ay;
                /* where it is going TO: arm's length down the view axis */
                float hx = cx + f3x * D, hy = EYE + f3y * D, hz = cz + f3z * D;
                float da = yaw - ay0;                       /* short way round, or it spins */
                while (da >  3.14159265f) da -= 6.28318531f;
                while (da < -3.14159265f) da += 6.28318531f;

                C3D_Mtx m;
                Mtx_Copy(&m, &view);
                Mtx_Translate(&m, sx + (hx - sx) * t, sy + (hy - sy) * t, sz + (hz - sz) * t, true);
                Mtx_RotateY(&m, ay0 + da * t, true);
                Mtx_RotateX(&m, pitch * t, true);           /* square to the view when held */
                Mtx_RotateY(&m, spin * t, true);            /* turning it over */
                float sc = 0.42f + (0.40f - 0.42f) * t;
                Mtx_Scale(&m, sc, sc * (float)IMG_H / (float)IMG_W, 1.0f);
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocModelview, &m);

                set_buf(g_boxvbo, 36);
                bind_tex((g_detail_ok && g_detail_for == held) ? &g_detail : &g_spine, 1);
                C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
                if (g_back_ok) {
                    bind_tex(&g_back, g_back_ok);                /* back: the printed card */
                    C3D_DrawArrays(GPU_TRIANGLES, 6, 6);
                }
                bind_tex(&g_room, 1);                    /* the four edges */
                C3D_DrawArrays(GPU_TRIANGLES, 12, 24);
                set_buf(g_quadvbo, 6);
            }

            /* framed posters: an end cap on each unit, and a few around the walls */
            if (g_wall_n > 0) {
                set_buf(g_signvbo, 6);
                int w = 0;
                for (int i = 0; i < g_nsec; i++) {
                    for (int e = 0; e < 2; e++) {
                        int k = (w++) % WALLPOSTERS;
                        if (!g_wall_ok[k]) continue;
                        bind_tex(&g_wall[k], g_wall_ok[k]);
                        C3D_Mtx m; Mtx_Copy(&m, &view);
                        Mtx_Translate(&m, g_sec[i].cx + (e ? 1 : -1) * (UNIT_LEN * 0.5f + 0.03f),
                                      1.30f, g_sec[i].cz, true);
                        Mtx_RotateY(&m, e ? C3D_Angle(0.25f) : C3D_Angle(-0.25f), true);
                        Mtx_Scale(&m, 0.62f, 0.62f * (float)IMG_H / (float)IMG_W, 1.0f);
                        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocModelview, &m);
                        C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
                    }
                }
                /* on the walls, above the stock so they are not hidden by it */
                static const float WP[8][4] = {   /* x, y, z, facing (radians about y) */
                    { -STORE_HX + 0.08f, 2.70f,  -6.0f,  1.5708f },
                    { -STORE_HX + 0.08f, 2.70f, -18.0f,  1.5708f },
                    {  STORE_HX - 0.08f, 2.70f,  -6.0f, -1.5708f },
                    {  STORE_HX - 0.08f, 2.70f, -18.0f, -1.5708f },
                    { -7.0f, 2.70f, STORE_Z0 - STORE_DEPTH + 0.08f, 0.0f },
                    {  7.0f, 2.70f, STORE_Z0 - STORE_DEPTH + 0.08f, 0.0f },
                    { -16.0f, 2.70f, STORE_Z0 - STORE_DEPTH + 0.08f, 0.0f },
                    {  16.0f, 2.70f, STORE_Z0 - STORE_DEPTH + 0.08f, 0.0f },
                };
                for (int i = 0; i < 8; i++) {
                    int k = i % WALLPOSTERS;
                    if (!g_wall_ok[k]) continue;
                    bind_tex(&g_wall[k], g_wall_ok[k]);
                    C3D_Mtx m; Mtx_Copy(&m, &view);
                    Mtx_Translate(&m, WP[i][0], WP[i][1], WP[i][2], true);
                    Mtx_RotateY(&m, WP[i][3], true);
                    Mtx_Scale(&m, 1.05f, 1.05f * (float)IMG_H / (float)IMG_W, 1.0f);
                    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocModelview, &m);
                    C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
                }
            }

            /* section signs, hung over each unit. Two quads back to back so the name reads the
             * right way round from both sides -- one quad with culling off shows its text
             * mirrored from behind. */
            set_buf(g_signvbo, 6);
            for (int i = 0; i < g_nsec; i++) {
                if (!g_sec[i].sign_ok) continue;
                bind_tex(&g_sec[i].sign, g_sec[i].sign_ok);
                for (int f = 0; f < 2; f++) {
                    C3D_Mtx m;
                    Mtx_Copy(&m, &view);
                    Mtx_Translate(&m, g_sec[i].cx, SIGN_Y, g_sec[i].cz + (f ? -0.03f : 0.03f), true);
                    if (f) Mtx_RotateY(&m, C3D_Angle(0.5f), true);
                    Mtx_Scale(&m, 3.4f, 3.4f * (float)SIGN_H / (float)SIGN_W, 1.0f);
                    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocModelview, &m);
                    C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
                }
            }
        }
        C3D_FrameEnd(0);

        frames++;
        if (osGetTime() - t0 >= 1000) { fps = frames; frames = 0; t0 = osGetTime(); }

        /* The bottom screen is the info panel, the way the player shows a title -- not a second
         * copy of the poster, which you are already looking at in 3D. */
        panel_clear();
        if (sel >= 0) {
            Poster *q = &g_pos[sel];
            panel_fmt(0, " %s", q->name);
            if (q->year && q->runtime) panel_fmt(1, " %d   %d min", q->year, q->runtime);
            else if (q->year)          panel_fmt(1, " %d", q->year);
            else if (q->runtime)       panel_fmt(1, " %d min", q->runtime);
            if (q->genres[0]) panel_fmt(2, " %s", q->genres);
            if (q->is_more) {
                Section *S = &g_sec[q->sect];
                int lo = S->page * S->cap + 1, hi = lo + S->cap - 1;
                if (hi > S->n) hi = S->n;
                panel_fmt(1, " showing %d-%d of %d", lo, hi, S->n);
                panel_fmt(2, " page %d of %d", S->page + 1, S->pages);
                panel_set(4, " Take this one and press the button");
                panel_set(5, " to restock the shelf with the next.");
            } else if (q->desc[0]) panel_wrap(4, 19, q->desc);
            else            panel_set(4, q->hasinfo ? " (no description in the .nfo)"
                                                    : " (no .nfo for this one - poster only)");
            if (held >= 0) panel_fmt(24, " in hand");
        } else {
            panel_set(0, " MOFLEX STORE  (prototype)");
            panel_fmt(2, " %d posters, %d with info", g_nposters, g_withinfo);
            panel_fmt(3, " %d KB texture   %d built   %llums",
                      (int)((g_nposters * TEX_BYTES) / 1024), built, (unsigned long long)t_load);
            panel_fmt(4, " fps %2d   eyes %d", fps, (slider > 0.0f ? 2 : 1));
            panel_set(6, " walk up to a case for its info");
            panel_set(8, " sections");
            for (int i = 0; i < g_nsec && i < 8; i++)
                panel_fmt(9 + i, "   %-16s %d", g_sec[i].name, g_sec[i].n);
        }
        if (held >= 0) {
            panel_set(26, " pad turn/zoom   d-pad next");
            panel_fmt(27, " %s: Y    put back: B",
                      g_pos[held].is_more ? "MORE" : verb());
        } else {
            panel_set(26, sel >= 0 ? " pad walk/turn   d-pad pick"
                                   : " pad walk/turn   d-pad look");
            panel_set(27, sel >= 0 ? " take: A   let go: B   exit: START"
                                   : " strafe: L/R   level: X   exit: START");
        }
        if (toast_t > 0) panel_fmt(22, " %s", toast);
        panel_flush();
    }

    for (int i = 0; i < g_nsec; i++) if (g_sec[i].sign_ok) C3D_TexDelete(&g_sec[i].sign);
    C3D_TexDelete(&g_room);
    if (g_mat_ok) { C3D_TexDelete(&g_carpet); C3D_TexDelete(&g_wood);
                    C3D_TexDelete(&g_glass);  C3D_TexDelete(&g_door); }
    if (g_outside_ok) C3D_TexDelete(&g_outside);
    if (g_store_ok) C3D_TexDelete(&g_storesign);
    if (g_exit_ok)  C3D_TexDelete(&g_exitsign);
    if (g_covers_ok) C3D_TexDelete(&g_covers);
    if (g_spine_ok)  C3D_TexDelete(&g_spine);
    for (int i = 0; i < WALLPOSTERS; i++) if (g_wall_ok[i]) C3D_TexDelete(&g_wall[i]);
    if (g_back_ok) C3D_TexDelete(&g_back);
    if (g_detail_ok) C3D_TexDelete(&g_detail);
    shaderProgramFree(&program);
    DVLB_Free(vsh_dvlb);
    C3D_Fini();
    gfxExit();
    return 0;
}
