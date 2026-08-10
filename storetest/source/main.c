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
#include <sys/stat.h>

#include "vshader_shbin.h"
#include "font8x8_basic.h"

/* ---------------- poster texture geometry ----------------
 * 128x256 is the smallest power-of-two box that holds a poster at a sane size; the image
 * occupies 128x182 of it, which is the source 132x188 aspect to within half a percent. */
#define TEX_W   128
#define TEX_H   256
#define IMG_W   128
#define IMG_H   182
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
#define STORE_HX     22.0f      /* floor spans +/-STORE_HX in x */
#define STORE_Z0      2.0f      /* and STORE_Z0 .. -STORE_DEPTH in z */
#define STORE_DEPTH  34.0f
#define CEIL_Y        4.2f
#define UNIT_LEN      6.0f      /* shelf unit: long axis (x) */
#define UNIT_DEPTH    1.0f
#define UNIT_H        2.0f
#define SIGN_Y        3.35f
#define MAX_SECTIONS  8
#define SEC_COLS      3

#define MAX_POSTERS 48          /* one aisle; the real thing would stream */
#define ROOM_TEX 64

typedef struct { float x, y, z, u, v, s; } Vtx;

typedef struct {
    C3D_Tex tex;
    int     ok;
    float   x, y, z;            /* centre, world space */
    float   ry;                 /* facing: +1 = normal points +x, -1 = -x */
    char    name[80];           /* title, or the filename when there is no .nfo */
    char    genres[80];
    char    desc[400];
    int     year, runtime, hasinfo;
} Poster;

typedef struct {
    char    name[24];
    float   cx, cz;             /* unit centre on the floor */
    C3D_Tex sign;
    int     sign_ok;
    int     n;                  /* posters assigned */
} Section;
static Section g_sec[MAX_SECTIONS];
static int     g_nsec = 0;

static Poster g_pos[MAX_POSTERS];
static int    g_nposters = 0;
static int    g_withinfo = 0;    /* how many came with a description */

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
static int build_cache_entry(const char *artpath, int sw, int sh, const char *dst) {
    FILE *f = fopen(artpath, "rb");
    if (!f) return 0;
    size_t need = (size_t)sw * sh * 2;
    u16 *src = (u16 *)malloc(need);
    if (!src) { fclose(f); return 0; }
    size_t rd = fread(src, 1, need, f);
    fclose(f);
    if (rd != need) { free(src); return 0; }

    u16 *lin = (u16 *)calloc(TEX_W * TEX_H, 2);
    u16 *til = (u16 *)malloc(TEX_BYTES);
    if (!lin || !til) { free(src); free(lin); free(til); return 0; }
    for (int j = 0; j < IMG_H; j++) {              /* nearest scale into the used sub-rect */
        const u16 *row = src + (size_t)(j * sh / IMG_H) * sw;
        u16 *d = lin + (size_t)j * TEX_W;
        for (int i = 0; i < IMG_W; i++) d[i] = row[i * sw / IMG_W];
    }
    tile_rgb565(lin, til, TEX_W, TEX_H);
    FILE *o = fopen(dst, "wb");
    int ok = o && fwrite(til, 1, TEX_BYTES, o) == TEX_BYTES;
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

        char cache[400], src[400];
        snprintf(cache, sizeof cache, "%s/%s.t565", CACHE_DIR, key);
        snprintf(src,   sizeof src,   "%s/%s", dir, e->d_name);

        FILE *cf = fopen(cache, "rb");
        if (!cf) {
            if (!build_cache_entry(src, sw, sh, cache)) continue;
            (*built)++;
            cf = fopen(cache, "rb");
            if (!cf) continue;
        }
        Poster *p = &g_pos[g_nposters];
        memset(p, 0, sizeof *p);
        if (!C3D_TexInit(&p->tex, TEX_W, TEX_H, GPU_RGB565)) { fclose(cf); break; }  /* out of VRAM */
        /* straight into texture memory -- this is the whole reason the cache is pre-tiled */
        size_t got = fread(p->tex.data, 1, TEX_BYTES, cf);
        fclose(cf);
        if (got != TEX_BYTES) { C3D_TexDelete(&p->tex); continue; }
        C3D_TexFlush(&p->tex);
        C3D_TexSetFilter(&p->tex, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&p->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        p->ok = 1;
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
    if (!C3D_TexInit(&p->tex, TEX_W, TEX_H, GPU_RGB565)) return;
    u16 *lin = (u16 *)calloc(TEX_W * TEX_H, 2);
    u16 *til = (u16 *)malloc(TEX_BYTES);
    if (!lin || !til) { free(lin); free(til); C3D_TexDelete(&p->tex); return; }
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
    memcpy(p->tex.data, til, TEX_BYTES);
    C3D_TexFlush(&p->tex);
    C3D_TexSetFilter(&p->tex, GPU_LINEAR, GPU_LINEAR);
    free(lin); free(til);
    p->ok = 1;
    snprintf(p->name, sizeof p->name, "Placeholder %d", idx + 1);
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
                     const char *t, int cols, int maxlines) {
    char line[80];
    int used = 0;
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
static void make_sign_tex(C3D_Tex *t, const char *text) {
    if (!C3D_TexInit(t, SIGN_W, SIGN_H, GPU_RGB565)) return;
    u16 *lin = (u16 *)calloc(SIGN_W * SIGN_H, 2);
    u16 *til = (u16 *)malloc(SIGN_W * SIGN_H * 2);
    if (!lin || !til) { free(lin); free(til); C3D_TexDelete(t); return; }
    const u16 board = 0x1082, edge = 0x4208, ink = 0xFFFF;
    for (int y = 0; y < SIGN_H; y++)
        for (int x = 0; x < SIGN_W; x++) {
            int b = (x < 3 || x >= SIGN_W - 3 || y < 3 || y >= SIGN_H - 3);
            lin[y * SIGN_W + x] = b ? edge : board;
        }
    int len = (int)strlen(text);
    if (len > 15) len = 15;                       /* 15 chars at 2x is 240 of 256 px */
    int tw = len * 16, x0 = (SIGN_W - tw) / 2, y0 = (SIGN_H - 16) / 2;
    for (int i = 0; i < len; i++) {
        unsigned c = (unsigned char)text[i];
        if (c > 127) c = '?';
        const char *g = font8x8_basic[c];
        for (int r = 0; r < 8; r++)
            for (int b = 0; b < 8; b++)
                if (g[r] & (1 << b))
                    for (int sy = 0; sy < 2; sy++)
                        for (int sx = 0; sx < 2; sx++) {
                            int px = x0 + i * 16 + b * 2 + sx, py = y0 + r * 2 + sy;
                            if (px >= 0 && px < SIGN_W && py >= 0 && py < SIGN_H)
                                lin[py * SIGN_W + px] = ink;
                        }
    }
    tile_rgb565(lin, til, SIGN_W, SIGN_H);
    memcpy(t->data, til, SIGN_W * SIGN_H * 2);
    C3D_TexFlush(t);
    C3D_TexSetFilter(t, GPU_LINEAR, GPU_LINEAR);
    free(lin); free(til);
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

    for (int y = 0; y < BACK_USED; y++)
        for (int x = 0; x < BACK_W; x++) {
            int b = (x < 4 || x >= BACK_W - 4 || y < 4 || y >= BACK_USED - 4);
            lin[y * BACK_W + x] = b ? edge : card;
        }
    /* title bar across the top, like a spine label */
    for (int y = 10; y < 44; y++)
        for (int x = 8; x < BACK_W - 8; x++) lin[y * BACK_W + x] = bar;
    { char t[24]; snprintf(t, sizeof t, "%.15s", q->name);
      draw_text(lin, BACK_W, BACK_H, 14, 18, 2, ink, t); }

    int y = 54;
    if (q->genres[0]) { char g[40]; snprintf(g, sizeof g, "%.30s", q->genres);
                        draw_text(lin, BACK_W, BACK_H, 12, y, 1, dim, g); y += 14; }
    y += 4;
    if (q->desc[0]) y = draw_wrap(lin, BACK_W, BACK_H, 12, y, 1, ink, q->desc, 30, 22);
    else            draw_text(lin, BACK_W, BACK_H, 12, y, 1, dim, "No description on file.");

    /* runtime + a barcode block, the two things every VHS back really had */
    char rt[32];
    if (q->runtime) snprintf(rt, sizeof rt, "RUNNING TIME  %d MIN", q->runtime);
    else            snprintf(rt, sizeof rt, "RUNNING TIME  --");
    draw_text(lin, BACK_W, BACK_H, 12, BACK_USED - 74, 1, dim, rt);
    for (int x = 0; x < 92; x++) {
        int w = ((x * 7919) >> 3) & 1;                 /* deterministic stripes */
        if (!w) continue;
        for (int yy = BACK_USED - 56; yy < BACK_USED - 20; yy++)
            lin[yy * BACK_W + (12 + x)] = ink;
    }
    if (q->year) { char yr[16]; snprintf(yr, sizeof yr, "%d", q->year);
                   draw_text(lin, BACK_W, BACK_H, BACK_W - 60, BACK_USED - 40, 2, dim, yr); }

    tile_rgb565(lin, til, BACK_W, BACK_H);
    memcpy(g_back.data, til, BACK_W * BACK_H * 2);
    C3D_TexFlush(&g_back);
    free(lin); free(til);
}

/* ---------------- room texture: one small repeating pattern, one draw call ---------------- */
static C3D_Tex g_room;
static void make_room_tex(void) {
    C3D_TexInit(&g_room, ROOM_TEX, ROOM_TEX, GPU_RGB565);
    u16 *lin = (u16 *)malloc(ROOM_TEX * ROOM_TEX * 2);
    u16 *til = (u16 *)malloc(ROOM_TEX * ROOM_TEX * 2);
    for (int y = 0; y < ROOM_TEX; y++)
        for (int x = 0; x < ROOM_TEX; x++) {
            int line = (x % 16 == 0) || (y % 16 == 0);
            int n = ((x * 7 + y * 13) % 5);           /* cheap speckle so it is not flat */
            int c = line ? 7 : 13 + n;
            lin[y * ROOM_TEX + x] = (u16)(((c & 0x1F) << 11) | (((c + 1) * 2 & 0x3F) << 5) | (c & 0x1F));
        }
    tile_rgb565(lin, til, ROOM_TEX, ROOM_TEX);
    memcpy(g_room.data, til, ROOM_TEX * ROOM_TEX * 2);
    C3D_TexFlush(&g_room);
    C3D_TexSetFilter(&g_room, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&g_room, GPU_REPEAT, GPU_REPEAT);
    free(lin); free(til);
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

static void build_sections(void) {
    char names[MAX_POSTERS][24];
    int  count[MAX_POSTERS];
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

    /* grid: SEC_COLS across, rows going away from the door */
    for (int i = 0; i < g_nsec; i++) {
        int col = i % SEC_COLS, row = i / SEC_COLS;
        g_sec[i].cx = (col - (SEC_COLS - 1) * 0.5f) * 13.0f;
        g_sec[i].cz = -7.0f - row * 11.0f;
        make_sign_tex(&g_sec[i].sign, g_sec[i].name);
        g_sec[i].sign_ok = 1;
    }

    /* place each poster on its section's unit: two rows, both faces, filling along the length */
    int slot[MAX_SECTIONS]; memset(slot, 0, sizeof slot);
    for (int i = 0; i < g_nposters; i++) {
        char g[24]; first_genre(g_pos[i].genres, g, sizeof g);
        int k = g_nsec - 1;                            /* GENERAL unless a section matches */
        for (int j = 0; j < g_nsec; j++) if (!strcmp(g_sec[j].name, g)) { k = j; break; }
        int sl = slot[k]++;
        int per_face = 12;                             /* 2 rows x 6 along the unit */
        int face = (sl / per_face) & 1;                /* front (+z) then back (-z) */
        int idx  = sl % per_face;
        int row  = idx / 6, colp = idx % 6;
        g_pos[i].x  = g_sec[k].cx + (colp - 2.5f) * 0.92f;
        g_pos[i].y  = row ? 1.52f : 0.72f;
        g_pos[i].z  = g_sec[k].cz + (face ? -(UNIT_DEPTH * 0.5f + 0.02f)
                                          :  (UNIT_DEPTH * 0.5f + 0.02f));
        g_pos[i].ry = face ? -2.0f : 2.0f;             /* 2.0 marks "faces +/-z", see the draw */
        g_sec[k].n++;
    }
}

/* ---------------- geometry ---------------- */
#define ROOM_VTX     (6 * 6 + MAX_SECTIONS * 5 * 6)   /* shell + a box per unit */
static Vtx *g_roomv, *g_quadv, *g_signv;
static void *g_roomvbo, *g_quadvbo, *g_signvbo;

static void push_quad(Vtx *v, int *n,
                      float ax, float ay, float az, float bx, float by, float bz,
                      float cx, float cy, float cz, float dx, float dy, float dz,
                      float ur, float vr, float shade) {
    /* two triangles, wound so the front face is toward the aisle */
    Vtx q[6] = {
        {ax, ay, az, 0,  0,  shade}, {bx, by, bz, ur, 0,  shade}, {cx, cy, cz, ur, vr, shade},
        {ax, ay, az, 0,  0,  shade}, {cx, cy, cz, ur, vr, shade}, {dx, dy, dz, 0,  vr, shade},
    };
    memcpy(v + *n, q, sizeof q);
    *n += 6;
}

static int build_room(void) {
    g_roomv = (Vtx *)linearAlloc(sizeof(Vtx) * ROOM_VTX);
    int n = 0;
    const float X = STORE_HX, Z0 = STORE_Z0, Z1 = STORE_Z0 - STORE_DEPTH, H = CEIL_Y;
    /* floor and ceiling */
    push_quad(g_roomv, &n, -X, 0, Z0,  X, 0, Z0,  X, 0, Z1, -X, 0, Z1, 12, 10, 0.52f);
    push_quad(g_roomv, &n, -X, H, Z1,  X, H, Z1,  X, H, Z0, -X, H, Z0, 12, 10, 0.28f);
    /* four walls */
    push_quad(g_roomv, &n, -X, 0, Z1, -X, 0, Z0, -X, H, Z0, -X, H, Z1, 10, 2, 0.60f);
    push_quad(g_roomv, &n,  X, 0, Z0,  X, 0, Z1,  X, H, Z1,  X, H, Z0, 10, 2, 0.60f);
    push_quad(g_roomv, &n, -X, 0, Z1,  X, 0, Z1,  X, H, Z1, -X, H, Z1, 12, 2, 0.56f);
    push_quad(g_roomv, &n,  X, 0, Z0, -X, 0, Z0, -X, H, Z0,  X, H, Z0, 12, 2, 0.56f);

    /* one shelf unit per section: a box you can see over, with a lighter top so it reads as a
     * surface rather than a wall */
    for (int i = 0; i < g_nsec; i++) {
        float cx = g_sec[i].cx, cz = g_sec[i].cz;
        float hx = UNIT_LEN * 0.5f, hz = UNIT_DEPTH * 0.5f, h = UNIT_H;
        push_quad(g_roomv, &n, cx-hx,0,cz+hz, cx+hx,0,cz+hz, cx+hx,h,cz+hz, cx-hx,h,cz+hz, 3,1, 0.44f);
        push_quad(g_roomv, &n, cx+hx,0,cz-hz, cx-hx,0,cz-hz, cx-hx,h,cz-hz, cx+hx,h,cz-hz, 3,1, 0.44f);
        push_quad(g_roomv, &n, cx-hx,0,cz-hz, cx-hx,0,cz+hz, cx-hx,h,cz+hz, cx-hx,h,cz-hz, 1,1, 0.38f);
        push_quad(g_roomv, &n, cx+hx,0,cz+hz, cx+hx,0,cz-hz, cx+hx,h,cz-hz, cx+hx,h,cz+hz, 1,1, 0.38f);
        push_quad(g_roomv, &n, cx-hx,h,cz-hz, cx+hx,h,cz-hz, cx+hx,h,cz+hz, cx-hx,h,cz+hz, 3,1, 0.70f);
    }
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
static int uLocProjection, uLocModelview;

static void scene_init(void) {
    vsh_dvlb = DVLB_ParseFile((u32 *)vshader_shbin, vshader_shbin_size);
    shaderProgramInit(&program);
    shaderProgramSetVsh(&program, &vsh_dvlb->DVLE[0]);
    C3D_BindProgram(&program);
    uLocProjection = shaderInstanceGetUniformLocation(program.vertexShader, "projection");
    uLocModelview  = shaderInstanceGetUniformLocation(program.vertexShader, "modelView");

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
    build_quad();
    build_signquad();
    build_box();
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

    while (aptMainLoop()) {
        hidScanInput();
        u32 kd = hidKeysDown();
        if (kd & KEY_START) break;
        if ((kd & KEY_A) && held < 0 && sel >= 0) {                /* take it off the shelf */
            held = sel; spin = 0.0f;
            if (g_back_for != sel) { rebuild_back(&g_pos[sel]); g_back_for = sel; }
        }
        if ((kd & KEY_B) && held >= 0)            held = -1;       /* put it back */
        hold_t += ((held >= 0) ? 0.14f : -0.14f);                  /* ~7 frames each way */
        if (hold_t > 1.0f) hold_t = 1.0f;
        if (hold_t < 0.0f) hold_t = 0.0f;

        circlePosition cp; hidCircleRead(&cp);
        float fx = cp.dx / 156.0f, fy = cp.dy / 156.0f;
        if (held >= 0) {
            /* the pad turns the case over instead of walking you around */
            spin += fx * 0.075f;
            if (fabsf(fx) < 0.15f) {                 /* let go and it settles to a face */
                float snap = (spin < 0 ? -1.0f : 1.0f) * 3.14159265f
                             * (float)((int)(fabsf(spin) / 3.14159265f + 0.5f));
                spin += (snap - spin) * 0.18f;
            }
            fx = 0; fy = 0;
        }
        /* The circle pad MOVES and the d-pad LOOKS. Turning used to be on the circle pad's x
         * axis, which meant you could not walk diagonally at all: pushing the pad at an angle
         * spun you instead of sliding you sideways. Movement is now purely translation in the
         * direction you are facing, which is what "walk toward what I am looking at" means. */
        u32 kh = hidKeysHeld();
        if (kh & KEY_DUP)    pitch += 0.035f;
        if (kh & KEY_DDOWN)  pitch -= 0.035f;
        /* Pitch STAYS where you put it. It used to spring back to level when the d-pad was
         * released, which no first-person game does -- you look down at the bottom shelf and
         * it drifts off it while you are still reading. X snaps back to level instead. */
        if (kd & KEY_X) pitch = 0.0f;
        if (pitch >  0.55f) pitch =  0.55f;
        if (pitch < -0.55f) pitch = -0.55f;
        if (held < 0) {
            if (kh & KEY_DRIGHT) yaw -= 0.040f;              /* turning right lowers yaw */
            if (kh & KEY_DLEFT)  yaw += 0.040f;
        }
        if (fabsf(fx) < 0.15f) fx = 0;
        if (fabsf(fy) < 0.15f) fy = 0;
        /* Single-stick, the way the console's own games do it: the pad's x axis TURNS you and
         * its y axis walks. Strafing is real but rare, so it sits on the shoulder buttons where
         * it costs nothing to ignore. */
        yaw -= fx * 0.045f;
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
        for (int i = 0; i < g_nsec; i++) {
            float hx = UNIT_LEN * 0.5f + 0.42f, hz = UNIT_DEPTH * 0.5f + 0.42f;
            float dx = cx - g_sec[i].cx, dz = cz - g_sec[i].cz;
            if (fabsf(dx) < hx && fabsf(dz) < hz) {
                float ox = hx - fabsf(dx), oz = hz - fabsf(dz);
                if (ox < oz) cx = g_sec[i].cx + (dx < 0 ? -hx : hx);
                else         cz = g_sec[i].cz + (dz < 0 ? -hz : hz);
            }
        }

        /* what am I looking at? nearest poster ahead, within reach.
         * Frozen while a case is held: the selection IS the held case until it goes back. */
        if (held < 0) {
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

            /* room: one texture, one draw */
            C3D_TexBind(0, &g_room);
            set_buf(g_roomvbo, roomn);
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocModelview, &view);
            C3D_DrawArrays(GPU_TRIANGLES, 0, roomn);

            /* posters on the shelves: one draw each, unit quad + per-poster matrix. Flat
             * cards, because they sit flush against the unit and nobody can see their edges. */
            set_buf(g_quadvbo, 6);
            for (int i = 0; i < g_nposters; i++) {
                if (!g_pos[i].ok || i == held) continue;      /* the held one is drawn below */
                float pop = (i == sel && held < 0) ? 0.10f : 0.0f;
                float nx = (fabsf(g_pos[i].ry) > 1.5f) ? 0.0f : (g_pos[i].ry > 0 ? 1.0f : -1.0f);
                float nz = (fabsf(g_pos[i].ry) > 1.5f) ? (g_pos[i].ry > 0 ? 1.0f : -1.0f) : 0.0f;
                float ay;
                if (fabsf(g_pos[i].ry) > 1.5f) ay = (g_pos[i].ry > 0) ? 0.0f : C3D_Angle(0.5f);
                else                           ay = (g_pos[i].ry > 0) ? C3D_Angle(0.25f)
                                                                     : C3D_Angle(-0.25f);
                C3D_Mtx m;
                Mtx_Copy(&m, &view);
                Mtx_Translate(&m, g_pos[i].x + nx * pop, g_pos[i].y, g_pos[i].z + nz * pop, true);
                Mtx_RotateY(&m, ay, true);
                Mtx_Scale(&m, 0.62f, 0.62f * (float)IMG_H / (float)IMG_W, 1.0f);
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocModelview, &m);
                C3D_TexBind(0, &g_pos[i].tex);
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
                const float D = 0.78f;
                /* where it is coming FROM: its slot on the shelf */
                float sx = q->x, sy = q->y, sz = q->z;
                float ay0;
                if (fabsf(q->ry) > 1.5f) ay0 = (q->ry > 0) ? 0.0f : C3D_Angle(0.5f);
                else                     ay0 = (q->ry > 0) ? C3D_Angle(0.25f) : C3D_Angle(-0.25f);
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
                float sc = 0.62f + (0.40f - 0.62f) * t;
                Mtx_Scale(&m, sc, sc * (float)IMG_H / (float)IMG_W, 1.0f);
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocModelview, &m);

                set_buf(g_boxvbo, 36);
                C3D_TexBind(0, &q->tex);                    /* front: the poster */
                C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
                if (g_back_ok) {
                    C3D_TexBind(0, &g_back);                /* back: the printed card */
                    C3D_DrawArrays(GPU_TRIANGLES, 6, 6);
                }
                C3D_TexBind(0, &g_room);                    /* the four edges */
                C3D_DrawArrays(GPU_TRIANGLES, 12, 24);
                set_buf(g_quadvbo, 6);
            }

            /* section signs, hung over each unit. Two quads back to back so the name reads the
             * right way round from both sides -- one quad with culling off shows its text
             * mirrored from behind. */
            set_buf(g_signvbo, 6);
            for (int i = 0; i < g_nsec; i++) {
                if (!g_sec[i].sign_ok) continue;
                C3D_TexBind(0, &g_sec[i].sign);
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
         * copy of the poster, which you are already looking at in 3D. It only falls back to the
        if (sel >= 0) {
            Poster *q = &g_pos[sel];
            printf("\x1b[0;0H\x1b[2K %.37s", q->name);
            char sub[64]; sub[0] = 0;
            if (q->year && q->runtime)      snprintf(sub, sizeof sub, "%d   %d min", q->year, q->runtime);
            else if (q->year)               snprintf(sub, sizeof sub, "%d", q->year);
            else if (q->runtime)            snprintf(sub, sizeof sub, "%d min", q->runtime);
            printf("\x1b[1;0H\x1b[2K %s", sub);
            printf("\x1b[2;0H\x1b[2K %.37s", q->genres);
            if (q->desc[0]) wrap_print(4, 37, 19, q->desc);
            else            wrap_print(4, 37, 19, q->hasinfo ? "(no description in the .nfo)"
                                                             : "(no .nfo for this one - poster only)");
            printf("\x1b[24;0H\x1b[2K %s", held >= 0 ? "[in hand]  B puts it back"
                                                       : "A takes it off the shelf");
            printf("\x1b[25;0H\x1b[2K");
        } else {
            printf("\x1b[0;0H\x1b[2K MOFLEX STORE  (prototype)");
            printf("\x1b[1;0H\x1b[2K");
            printf("\x1b[2;0H\x1b[2K %d posters, %d with info", g_nposters, g_withinfo);
            printf("\x1b[3;0H\x1b[2K %d KB texture  %d built  %llums",
                   (int)((g_nposters * TEX_BYTES) / 1024), built, (unsigned long long)t_load);
            printf("\x1b[4;0H\x1b[2K fps %2d   eyes %d", fps, (slider > 0.0f ? 2 : 1));
            printf("\x1b[6;0H\x1b[2K walk up to a case for its info");
            printf("\x1b[8;0H\x1b[2K sections");
            int r = 9;
            for (int i = 0; i < g_nsec && i < 8; i++, r++)
                printf("\x1b[%d;0H\x1b[2K   %-16s %d", r, g_sec[i].name, g_sec[i].n);
            for (; r <= 25; r++) printf("\x1b[%d;0H\x1b[2K", r);
        }
        /* pinned to the bottom of the 30-row console, not floating in the middle */
        printf("\x1b[27;0H\x1b[2K pad walk+look  L/R strafe  d-pad look");
        printf("\x1b[28;0H\x1b[2K A take  B back(turn it: pad)  START exit");
    }

    for (int i = 0; i < g_nposters; i++) if (g_pos[i].ok) C3D_TexDelete(&g_pos[i].tex);
    for (int i = 0; i < g_nsec; i++) if (g_sec[i].sign_ok) C3D_TexDelete(&g_sec[i].sign);
    C3D_TexDelete(&g_room);
    if (g_back_ok) C3D_TexDelete(&g_back);
    shaderProgramFree(&program);
    DVLB_Free(vsh_dvlb);
    C3D_Fini();
    gfxExit();
    return 0;
}
