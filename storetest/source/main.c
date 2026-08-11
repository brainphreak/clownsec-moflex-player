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
#include "music.h"
#include "art_data.h"
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
#define MUSIC_DIR "sdmc:/moflex_player/music"
#define DATA_DIR  "sdmc:/moflex_player/moviedata"
#define ART_DIR   "sdmc:/moflex_player/art"
#define CACHE_DIR "sdmc:/moflex_player/store"
#define SRC_W 132
#define SRC_H 188

/* A store floor with freestanding units, not a corridor. The units are low enough to see
 * over (2.0 against a 1.55 eye height puts the sign of the next section in view from
 * anywhere), which is what makes the place read as a shop rather than a maze. */
/* The ROOM is measured from the fixtures, not the other way round. These are only the starting
 * guesses -- g_hx and g_depth below are what the shop is actually built to, worked out once the
 * bays know their own lengths. A fixed room with variable bays left a hall of empty carpet up
 * the middle. */
#define STORE_Z0      2.0f
#define ROW_PITCH     5.4f      /* front-to-back spacing of the rows of bays */
static float g_gapz[2] = { -7.3f, -12.7f };   /* midway between rows: where wall art hangs */
static float g_hx    = 13.5f;
static float g_depth = 24.0f;
#define STORE_HX     g_hx
#define STORE_DEPTH  g_depth
#define CEIL_Y        4.2f
/* A bay is BUILT TO ITS SECTION now, not to a fixed size: a quiet genre gets a short unit, a
 * busy one a long one. That is what a shop looks like, and it is the only way to have every
 * shelf full instead of a hall of half-empty carpet. These are the limits. */
#define UNIT_LEN      6.3f
#define UNIT_LEN_MIN  3.0f
#define UNIT_DEPTH    1.0f
#define UNIT_H        2.0f
#define SIGN_Y        2.62f     /* hung low enough to clear the name across the back wall */
#define ROW_Y0        0.74f     /* centre of the bottom row */
#define ROW_DY        0.68f     /* row to row -- a case is 0.47 tall, so this is a shelf gap */
#define CASE_W        0.40f     /* a case on the shelf; PITCH_FACE is this plus the gap */
#define MAX_SECTIONS  11
#define SEC_COLS      3
#define BAY_ROWS      2
#define PITCH_SPINE   0.235f
#define PITCH_FACE    0.42f
/* A genre with fewer than this is not worth a unit -- a bay holding five films reads as a shop
 * closing down -- so it merges into OTHER. One with more than BAY_MAX gets a SECOND unit
 * instead of hiding the rest behind a MORE case. */
#define BAY_MIN       4
#define BAY_MAX      32
#define PER_ROW      22         /* the most cases a full-length bay holds in a row */
/* One side only. Stocking both faces doubled what had to be drawn, hid half of it behind the
 * unit, and put titles on a face you have to walk round the bay to reach. A shop merchandises
 * the side that faces the aisle. */
#define SHELF_CAP    (3 * PER_ROW)
/* How much of the shop is drawn at once. Every spine is its own draw call, and a shop full of
 * them is thousands of commands a frame -- past what the command buffer holds, and a GPU fed a
 * truncated command stream wedges the console. You cannot read a spine across the room anyway. */
/* THREE cases, not ten colours. A rental shop did not shelve publisher packaging -- every tape
 * went into the shop's own clamshell, so a shelf was uniform white and blue with the odd black
 * case among it. A rainbow is the most artificial thing you can put on a shelf. */
#define SPINE_COLOURS 3
#define SPINE_BUDGET 170
/* Every Nth case stands face out, as a shop does: a run of spines, a cover, more spines. The
 * covers are what make a shelf browsable; the spines are what make it a shop. */
/* How a shelf is merchandised. Face out is what a shop WANTS -- a cover sells, a spine does
 * not -- and spines are what it falls back to when the run outgrows the shelf. So the pattern
 * is mostly F, in blocks, with short runs of spines between where the shelf ran out of room.
 * Twelve of sixteen face out. */
/* Every case faces the aisle.
 *
 * The spines went. They were there to fit more titles into a bay for no texture cost, but a
 * spine tells you nothing -- you have to turn it face-on to know what it is, which loads the
 * cover anyway, so the saving was never real once you were browsing rather than walking past.
 * A shelf of covers is what a rental shop looks like and it is what reads at a glance. The
 * cost is fewer titles a bay, which is what the MORE case at the end of a section is for. */


/* Covers held at once, 16 KB apiece. Eight was timid -- an earlier build gave every one of
 * ninety-six cases its own texture and cost 1.5 MB, which this room has room for several times
 * over. Thirty-two is half a megabyte and keeps most of what you can actually see in real art,
 * with the blank clamshell behind the rest. */
#define COVER_VIEW   14.5f      /* a cover is drawn this far off; the cull does the rest */

/* Metadata is cheap -- about 1.2 KB a title, so even a thousand is well under 2 MB. What
 * costs is the cover bitmap at 16 KB each, and that is what cover_budget_bytes() rations. */
#define MAX_POSTERS 760
#define ROOM_TEX 64

typedef struct { float x, y, z, u, v, s; } Vtx;

typedef struct {
    /* No texture. A case on a shelf is a SPINE: the same mesh and the same tiny texture, tinted
     * a different colour per title -- so a title costs nothing but its metadata and the shelves
     * scale to a whole catalogue. Only the SELECTED one turns face-on and loads a real cover,
     * into the single detail texture, because there is only ever one. */
    int     col;                /* which spine texture, 0..SPINE_COLOURS-1 */
    int     ok;
    float   x, y, z;            /* centre, world space */
    float   ay;                 /* which way the case FACES, in radians. Was a +/-1 flag, which
                                 * could only express four directions and fell apart the moment
                                 * a unit sat at 45 degrees. */
    char    name[80];           /* title, or the filename when there is no .nfo */
    char    category[32];       /* "Movies" / "TV Shows" / "Music" -- what marks a music video */
    char    season[128];        /* "Show|S01" for an episode, "" for anything else */
    C3D_Tex tex;                /* its cover, owned outright -- see g_cov_n */
    int     tex_ok;             /* the ONLY thing that may bind tex; an uninitialised one wedges the GPU */
    int     copy_of;            /* a second copy of another title: -1 normally, else its index.
                                 * It has no texture of its own and binds the original's. */
    char    genres[80];
    char    desc[400];
    int     year, runtime, hasinfo;
    int     shown;              /* on a shelf on the current page */
    int     faceout;            /* stands face to the aisle rather than spine out */
    int     cover_state;        /* 0 untried, 1 cached and ready, -1 no art -- so a title with
                                 * no cover is not retried on every single frame */
    int     is_more;            /* the "MORE MOVIES" case that turns the section over */
    int     sect, order;        /* which bay, and where in that bay's run */
    C3D_Mtx model;              /* built once when it is placed. Rebuilding a translate, a
                                 * rotate and a scale for every case, every eye, every frame
                                 * was most of the cost of walking. */
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
    float   len;                /* built to fit what this section holds */
    int     per_row;            /* cases across this bay -- from its own length, not a constant */
    /* the L return: a short run turning the corner at the inner end. It is shelving too, and
     * stood empty while the long side carried everything. */
    float   Lx, Lz, Llen, Lay;
    int     Lper_row, Lcap;
    int     part;               /* 0, or which unit of a split genre this is */
    float   facedir;            /* which side the stock is on: +1 or -1 in the unit's own z.
                                 * Alternated down the room so bays face each other across an
                                 * aisle, the way a shop lays them out. */
    int     has_L;              /* an L return on the inner end -- no poster fits there */
    int     more_idx;           /* the MORE case for this bay, -1 if it all fits */
    int     hide;               /* nothing was filed here: build no unit, sign or blocker */
} Section;
static Section g_sec[MAX_SECTIONS];
static int     g_nsec = 0;
static int     g_new_idx = -1;   /* the NEW RELEASES rack: a real bay, just not a genre */

static Poster g_pos[MAX_POSTERS];
static int    g_nposters = 0;
static int    g_withinfo = 0;    /* how many came with a description */
/* Where the shelves came from. moviedata/ carries a .nfo beside each poster -- title, year,
 * genres, description. art/ is the catalogue's poster cache and has no text at all, so titles
 * from there have nothing to show AND no genre, which lands every one of them in GENERAL. */
static int    g_from_data = 0, g_from_art = 0;
static int    g_scan_capped = 0;   /* the scan hit MAX_POSTERS: titles exist that we never saw */
/* The same room serves both stores; only the source of the shelves and the VERB differ.
 * Library: take one off the shelf and play it. Catalogue: take one and queue the download. */
enum { STORE_LIBRARY = 0, STORE_CATALOG = 1 };
static int    g_mode = STORE_LIBRARY;
static int    g_drawn = 0;      /* cases actually drawn last frame */
static float  g_doorx = 0.0f;   /* where the door ended up, so the EXIT board follows it */
static float  g_jukex = 0.0f, g_jukez = 0.0f, g_jukerot = 0.0f;  /* stand beside it, press A */
static int    g_covers_on = 1;  /* SELECT: face-out covers on/off, to isolate the stutter */
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

/* Scale a linear RGB565 image already in memory into a texture, tiling as it goes.
 * The same arithmetic build_cache_entry_sz does, without the file at either end -- which is
 * what the built-in art needs, and what makes it cost nothing at startup. */
static int build_tex_mem(const u16 *src, int sw, int sh, C3D_Tex *t,
                         int tw, int th, int iw, int ih) {
    if (!C3D_TexInit(t, tw, th, GPU_RGB565)) return 0;
    u16 *lin = (u16 *)calloc((size_t)tw * th, 2);
    u16 *til = (u16 *)malloc((size_t)tw * th * 2);
    if (!lin || !til) { free(lin); free(til); C3D_TexDelete(t); return 0; }
    for (int j = 0; j < ih; j++) {
        const u16 *row = src + (size_t)(j * sh / ih) * sw;
        u16 *d = lin + (size_t)j * tw;
        for (int i = 0; i < iw; i++) d[i] = row[i * sw / iw];
    }
    tile_rgb565(lin, til, tw, th);
    memcpy(t->data, til, (size_t)tw * th * 2);
    C3D_TexFlush(t);
    C3D_TexSetFilter(t, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(t, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    free(lin); free(til);
    return 1;
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
        else if (!strcasecmp(k, "category")) snprintf(p->category, sizeof p->category, "%s", v);
        else if (!strcasecmp(k, "desc"))    snprintf(p->desc,   sizeof p->desc,   "%s", v);
        else if (!strcasecmp(k, "year"))    p->year    = atoi(v);
        else if (!strcasecmp(k, "runtime")) p->runtime = atoi(v);
    }
    fclose(f);
    p->hasinfo = 1;
}

static int isdig(char c) { return c >= '0' && c <= '9'; }

/* "Show - S01e05 - Episode Title" -> key "Show|S01", shelf name "Show  Season 01".
 *
 * A season is one case on the shelf, not thirteen identical ones. The tag is matched on the
 * NAME rather than the metadata because a catalog match gives every episode of a season the
 * same title and the same description -- the episode number only survives in the filename. */
static int season_key(const char *nm, char *key, size_t kcap, char *show, size_t scap) {
    /* Two shapes, because releases use both and insisting on exactly SxxExx let whole seasons
     * back onto the shelf one episode at a time:  S1E2 / S01E02 / s01e2, and 1x02 / 01x02.
     * The season number is normalised to two digits so S1E2 and S01E05 land in the same bay. */
    for (const char *p = nm; p[0]; p++) {
        int se = -1, ep_at = 0;
        if (p[0] == 'S' || p[0] == 's') {
            int d = isdig(p[1]) ? (isdig(p[2]) ? 2 : 1) : 0;
            if (d) {
                const char *e = p + 1 + d;
                if ((e[0] == 'E' || e[0] == 'e') && isdig(e[1])) {
                    se = (d == 1) ? (p[1] - '0') : ((p[1] - '0') * 10 + (p[2] - '0'));
                    ep_at = 1;
                }
            }
        } else if (isdig(p[0])) {
            int d = isdig(p[1]) ? 2 : 1;
            const char *e = p + d;
            if ((e[0] == 'x' || e[0] == 'X') && isdig(e[1])) {
                /* only when it is a standalone token, or "2001" looks like season 20 */
                if (p == nm || p[-1] == ' ' || p[-1] == '-' || p[-1] == '_' || p[-1] == '.') {
                    se = (d == 1) ? (p[0] - '0') : ((p[0] - '0') * 10 + (p[1] - '0'));
                    ep_at = 1;
                }
            }
        }
        if (!ep_at || se < 0) continue;
        int pre = (int)(p - nm);
        while (pre > 0 && (nm[pre-1] == ' ' || nm[pre-1] == '-' ||
                           nm[pre-1] == '_' || nm[pre-1] == '.')) pre--;      /* drop the " - " */
        if (pre <= 0) return 0;
        snprintf(key,  kcap, "%.*s|S%02d", pre, nm, se);
        snprintf(show, scap, "%.*s  Season %02d", pre, nm, se);
        return 1;
    }
    return 0;
}

/* music videos are not what anyone walks into a rental shop for */
static int is_music(const Poster *p) {
    /* "Music", "Music Video", "Music Videos" -- and a genre naming it anywhere in the list,
     * not only first. Matching the exact word "Music" let most of them straight onto a shelf. */
    if (!strncasecmp(p->category, "Music", 5)) return 1;
    for (const char *q = p->genres; *q; q++)
        if ((q == p->genres || q[-1] == ',' || q[-1] == ' ') && !strncasecmp(q, "Music", 5)) return 1;
    return 0;
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
    while ((e = readdir(d))) {
        if (g_nposters >= MAX_POSTERS) { g_scan_capped = 1; break; }
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
        p->copy_of = -1;
        snprintf(p->srcpath, sizeof p->srcpath, "%s", src);
        p->src_w = sw; p->src_h = sh;
        { unsigned h = 2166136261u;                        /* spine colour from the title */
          for (const char *c = key; *c; c++) h = (h ^ (unsigned char)*c) * 16777619u;
          /* mostly the house case, a few black, the odd grey -- not an even spread */
          unsigned r = h % 10;
          p->col = (r < 7) ? 0 : (r < 9) ? 1 : 2; }
        p->ok = 1;
        snprintf(p->key, sizeof p->key, "%s", key);
        pretty(e->d_name, p->name, sizeof p->name);
        {
            /* moviedata/ keeps "<stem>.nfo" beside "<stem>.p565". art/ names its posters
             * "<key>_<W>x<H>.p565", and the sidecar the catalog browser leaves is just
             * "<key>.nfo" -- so the size has to come off before looking. */
            char base[160];
            snprintf(base, sizeof base, "%s", key);
            if (!with_nfo) { char *u2 = strrchr(base, '_'); if (u2) *u2 = 0; }
            char nfo[400];
            snprintf(nfo, sizeof nfo, "%s/%s.nfo", dir, base);
            read_nfo(nfo, p);
        }
        /* by here the .nfo has been read, so the category and the real title are known */
        if (is_music(p)) { memset(p, 0, sizeof *p); continue; }
        {   char sk[128], show[80];
            if (season_key(key, sk, sizeof sk, show, sizeof show) ||
                season_key(p->name, sk, sizeof sk, show, sizeof show)) {
                int dup = 0;
                for (int j = 0; j < g_nposters && !dup; j++)
                    if (g_pos[j].season[0] && !strcasecmp(g_pos[j].season, sk)) dup = 1;
                if (dup) { memset(p, 0, sizeof *p); continue; }   /* this season is already stocked */
                snprintf(p->season, sizeof p->season, "%s", sk);
                snprintf(p->name,   sizeof p->name,   "%s", show);
            }
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
    g_from_data = scan_dir(DATA_DIR, SRC_W, SRC_H, 1, built);
    g_from_art  = scan_dir(ART_DIR,  0,     0,     0, built);
    return g_nposters;
}

/* a placeholder poster so the prototype still runs on a console with no art cached */
static void make_placeholder(Poster *p, int idx) {
    p->col = (idx % 10 < 7) ? 0 : (idx % 10 < 9) ? 1 : 2;
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
/* Only the rows that CHANGED.
 *
 * Reprinting the whole block every frame was 1092 glyphs, and libctru draws each one 8x8 pixels
 * at a time on the CPU -- about four million pixel writes a second, which is real money at
 * 268 MHz and was most of the stutter. Almost nothing on this panel changes between frames;
 * the two lines that do are the frame rate and the count. Rows are addressed one at a time and
 * never end in a newline, so nothing can scroll. */
static char g_shown_panel[PANEL_ROWS][PANEL_MAXC + 1];
static int  g_panel_primed = 0;
static void panel_flush(void) {
    for (int r = 0; r < PANEL_ROWS; r++) {
        if (g_panel_primed && !memcmp(g_shown_panel[r], g_panel[r], g_cols)) continue;
        printf("\x1b[%d;1H%s", r + 1, g_panel[r]);     /* 1-based, as ANSI specifies */
        memcpy(g_shown_panel[r], g_panel[r], g_cols + 1);
    }
    g_panel_primed = 1;
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
/* The shop name, on two lines, each scaled up until it fills the width.
 *
 * One line of 8px type at 1x on a 256 board left the text a third the height of its own frame,
 * which reads as a label rather than as signage. Splitting it lets both lines run nearly the
 * full width, and the scale is computed from the longest line rather than picked, so a
 * different name still fills the board. */
#define SIGN2_W 512
#define SIGN2_H 128
static void make_sign_tex2(C3D_Tex *t, const char *l1, const char *l2,
                           u16 board, u16 edge, u16 ink) {
    if (!C3D_TexInit(t, SIGN2_W, SIGN2_H, GPU_RGB565)) return;
    u16 *lin = (u16 *)calloc(SIGN2_W * SIGN2_H, 2);
    u16 *til = (u16 *)malloc(SIGN2_W * SIGN2_H * 2);
    if (!lin || !til) { free(lin); free(til); C3D_TexDelete(t); return; }
    for (int y = 0; y < SIGN2_H; y++)
        for (int x = 0; x < SIGN2_W; x++) {
            int b = (x < 5 || x >= SIGN2_W - 5 || y < 5 || y >= SIGN2_H - 5);
            lin[y * SIGN2_W + x] = b ? edge : board;
        }
    const char *L[2] = { l1, l2 };
    int sc[2], w[2];
    const int avail = SIGN2_W - 40;
    for (int i = 0; i < 2; i++) {
        int len = (int)strlen(L[i]); if (len < 1) len = 1;
        sc[i] = avail / (len * 8);
        if (sc[i] > 8) sc[i] = 8;
        if (sc[i] < 1) sc[i] = 1;
        w[i] = len * 8 * sc[i];
    }
    int th = sc[0] * 8 + sc[1] * 8 + 10;              /* both lines plus the gap between them */
    int y = (SIGN2_H - th) / 2;
    for (int i = 0; i < 2; i++) {
        int x0 = (SIGN2_W - w[i]) / 2;
        for (const char *c = L[i]; *c; c++) {
            draw_glyph(lin, SIGN2_W, SIGN2_H, x0, y, sc[i], ink, (unsigned char)*c);
            x0 += 8 * sc[i];
        }
        y += sc[i] * 8 + 10;
    }
    tile_rgb565(lin, til, SIGN2_W, SIGN2_H);
    memcpy(t->data, til, SIGN2_W * SIGN2_H * 2);
    C3D_TexFlush(t);
    C3D_TexSetFilter(t, GPU_LINEAR, GPU_LINEAR);
    free(lin); free(til);
}

/* store name and fire-exit board */
static C3D_Tex g_storesign, g_exitsign, g_jukesign;
static int     g_store_ok = 0, g_exit_ok = 0;

/* Covers for the cases that stand face out.
 *
 * A shelf of nothing but spines is unbrowsable -- you want a run of edges, then a cover, then
 * Every case faces the aisle, and the nearest of them get a real cover from this pool. Eight at 64x128 is
 * 128 KB, and one is loaded per frame so walking down an aisle never hitches. */
/* Every case owns its cover, uploaded once and never touched again.
 *
 * This used to be a 32-slot pool that shuffled covers in and out as you walked, which is why
 * anything further than a few metres stood there white until you closed on it. The pool was
 * never about memory: a cover is 16 KB, so the whole shop is under 6 MB against the 29 MB of
 * linear space actually free. It was about a budget that turned out not to exist. Owning the
 * texture outright deletes the pool, the eviction, the RAM staging copy and the streaming --
 * nothing loads while you walk because nothing is left to load. */
static int g_cov_n = 0;         /* how many got one before linear space ran out */

/* One cover shared by every restock case, from store/restock.p565 (a 132x188 raw, the same
 * shape the player caches its posters in). Without it they wear the blank clamshell. */
static C3D_Tex g_restock;
static int     g_restock_ok = 0;
/* A wide banner for the back wall, from store/banner.p565 -- a 2:1 landscape raw. Unlike a
 * poster it fills its texture edge to edge, so it draws on the full-UV sign quad. */
#define BAN_W 256
#define BAN_H 128
#define BANNERS 2
static C3D_Tex g_banner[BANNERS];
static int     g_banner_ok[BANNERS];
static void load_banner(int i) {
    char src[400], small[400];
    snprintf(src,   sizeof src,   "%s/banner%d.p565", CACHE_DIR, i);
    snprintf(small, sizeof small, "%s/banner%d.b565", CACHE_DIR, i);
    FILE *sf = fopen(src, "rb");
    if (!sf) {                                  /* nothing supplied: use the one we ship with */
        g_banner_ok[i] = build_tex_mem(art_banner[i], ART_BAN_W, ART_BAN_H, &g_banner[i],
                                       BAN_W, BAN_H, BAN_W, BAN_H);
        return;
    }
    fclose(sf);
    FILE *f = fopen(small, "rb");
    if (!f) {
        if (!build_cache_entry_sz(src, BAN_W * 2, BAN_H * 2, small, BAN_W, BAN_H, BAN_W, BAN_H)) return;
        f = fopen(small, "rb");
        if (!f) return;
    }
    if (!C3D_TexInit(&g_banner[i], BAN_W, BAN_H, GPU_RGB565)) { fclose(f); return; }
    size_t got = fread(g_banner[i].data, 1, (size_t)BAN_W * BAN_H * 2, f);
    fclose(f);
    if (got != (size_t)BAN_W * BAN_H * 2) { C3D_TexDelete(&g_banner[i]); return; }
    C3D_TexSetFilter(&g_banner[i], GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&g_banner[i], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexFlush(&g_banner[i]);
    g_banner_ok[i] = 1;
}

static void load_restock(void) {
    char src[400], small[400];
    snprintf(src,   sizeof src,   "%s/restock.p565", CACHE_DIR);
    snprintf(small, sizeof small, "%s/restock.w565", CACHE_DIR);
    FILE *sf = fopen(src, "rb");
    if (!sf) {                                  /* nothing supplied: use the one we ship with */
        g_restock_ok = build_tex_mem(art_restock, ART_WALL_W, ART_WALL_H, &g_restock,
                                     TEX_W, TEX_H, IMG_W, IMG_H);
        return;
    }
    fclose(sf);
    FILE *f = fopen(small, "rb");
    if (!f) {
        if (!build_cache_entry_sz(src, SRC_W, SRC_H, small, TEX_W, TEX_H, IMG_W, IMG_H)) return;
        f = fopen(small, "rb");
        if (!f) return;
    }
    if (!C3D_TexInit(&g_restock, TEX_W, TEX_H, GPU_RGB565)) { fclose(f); return; }
    size_t got = fread(g_restock.data, 1, (size_t)TEX_W * TEX_H * 2, f);
    fclose(f);
    if (got != (size_t)TEX_W * TEX_H * 2) { C3D_TexDelete(&g_restock); return; }
    C3D_TexSetFilter(&g_restock, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&g_restock, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexFlush(&g_restock);
    g_restock_ok = 1;
}

/* Every cover, tiled and ready, sitting in main RAM.
 *
 * This is the one that mattered. Opening a file on the SD card costs milliseconds -- not the
 * microseconds an fread costs once it is open -- and the walk loop was paying that twice a
 * frame. A 16 ms frame cannot afford even one. It is why the old build that loaded everything
 * at startup walked smoothly and every streaming version since has not: the rescaling was
 * never the problem, the open was.
 *
 * 16 KB a title, so the whole catalogue is a couple of megabytes. Past the budget a title
 * falls back to reading from disk, so a catalogue too big to hold still works -- it just
 * hitches the way it used to. */
#define COVER_BYTES ((size_t)TEX_W * TEX_H * 2)
static unsigned g_frame = 0;
/* Build and upload every cover ONCE, up front, with the work on screen.
 *
 * Building one means reading the source, rescaling it, tiling it and writing it out; loading it
 * means an SD open, which costs milliseconds. Neither belongs in a frame. Both happen here,
 * while you are standing still, and the second run finds the caches built and only uploads. */
static void prebuild_covers(int *built) {
    char path[400];
    for (int i = 0; i < g_nposters; i++) {
        Poster *q = &g_pos[i];
        q->tex_ok = 0;
        if (!q->ok || q->is_more || !q->srcpath[0]) continue;
        snprintf(path, sizeof path, "%s/%s.w565", CACHE_DIR, q->key);
        FILE *f = fopen(path, "rb");
        if (!f) {
            if (!build_cache_entry_sz(q->srcpath, q->src_w, q->src_h, path,
                                      TEX_W, TEX_H, IMG_W, IMG_H)) { q->cover_state = -1; continue; }
            (*built)++;
            f = fopen(path, "rb");
            if (!f) { q->cover_state = -1; continue; }
        }
        /* An uninitialised C3D_Tex handed to the GPU locks the console, so tex_ok is only ever
         * set once the init AND the read have both come off. */
        if (!C3D_TexInit(&q->tex, TEX_W, TEX_H, GPU_RGB565)) { fclose(f); q->cover_state = -1; continue; }
        size_t got = fread(q->tex.data, 1, COVER_BYTES, f);
        fclose(f);
        if (got != COVER_BYTES) { C3D_TexDelete(&q->tex); q->cover_state = -1; continue; }
        C3D_TexSetFilter(&q->tex, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&q->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        C3D_TexFlush(&q->tex);
        q->tex_ok = 1; q->cover_state = 1; g_cov_n++;
        if ((i & 7) == 0) {                       /* say what it is doing; this takes a while */
            printf("\x1b[8;1H  preparing covers  %d / %d   ", i + 1, g_nposters);
            gfxFlushBuffers(); gspWaitForVBlank();
        }
    }
}

/* The full-resolution front of whatever is in your hand. One texture, filled on pickup. */
static C3D_Tex g_detail;
static int     g_detail_ok = 0, g_detail_for = -1;
static void load_detail(Poster *q, int idx) {
    /* cover_state < 0 means the art could not be built. Without that test this retries the
     * whole read-rescale-write every frame, and the selection scan calls it every frame. */
    if (!g_detail_ok || g_detail_for == idx || q->cover_state < 0) return;
    char big[400];
    snprintf(big, sizeof big, "%s/%s.t565", CACHE_DIR, q->key);
    FILE *f = fopen(big, "rb");
    if (!f) {                                   /* built once, the first time you look at it */
        if (!build_cache_entry_sz(q->srcpath, q->src_w, q->src_h, big,
                                  DET_W, DET_H, DET_IMG_W, DET_IMG_H)) { q->cover_state = -1; return; }
        f = fopen(big, "rb");
    }
    if (!f) { q->cover_state = -1; return; }
    size_t got = fread(g_detail.data, 1, (size_t)DET_W * DET_H * 2, f);
    fclose(f);
    if (got != (size_t)DET_W * DET_H * 2) { q->cover_state = -1; return; }
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
#define OUTSIDES 3              /* one street scene per window, so the view is not repeated */
static C3D_Tex g_outside[OUTSIDES];
static int     g_outside_ok[OUTSIDES];
static void px(u16 *l, int W, int H, int x, int y, u16 c) {
    if (x >= 0 && x < W && y >= 0 && y < H) l[y * W + x] = c;
}
static void box2(u16 *l, int W, int H, int x0, int y0, int x1, int y1, u16 c) {
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) px(l, W, H, x, y, c);
}
static void make_outside_tex(int v) {
    const int W = 256, H = 128;
    C3D_Tex *T = &g_outside[v];
    if (!C3D_TexInit(T, W, H, GPU_RGB565)) return;
    u16 *lin = (u16 *)malloc(W * H * 2);
    u16 *til = (u16 *)malloc(W * H * 2);
    if (!lin || !til) { free(lin); free(til); C3D_TexDelete(T); return; }
    const u16 sky = 0x0821, far_ = 0x18E3, lot = 0x2124, bay = 0x6B4D;
    const u16 lampglow = 0xFF98, win = 0xFDA0;
    const u16 carcol[4] = { 0x8000, 0x0011, 0x7BEF, 0xA145 };

    for (int y = 0; y < H; y++)                              /* sky, darker toward the top */
        for (int x = 0; x < W; x++)
            lin[y * W + x] = (y < 52) ? (u16)(sky + ((y / 14) << 5)) : lot;
    for (int i = 0; i < 40; i++) px(lin, W, H, (i * 6197 + v * 331) % W, (i * 977 + v * 7) % 44, 0x8410);

    /* a low skyline with lit windows */
    for (int b = 0; b < 7; b++) {
        int bx = 4 + b * 36 + v * 9, bw = 18 + ((b + v) * 7) % 14, bh = 12 + (b * 11 + v * 13) % 20;
        box2(lin, W, H, bx, 52 - bh, bx + bw, 51, far_);
        for (int wy = 52 - bh + 3; wy < 50; wy += 5)
            for (int wx = bx + 2; wx < bx + bw - 2; wx += 5)
                if (((wx * 7 + wy * 3) % 5) < 2) box2(lin, W, H, wx, wy, wx + 1, wy + 2, win);
    }
    /* painted bays */
    for (int i = 0; i < 9; i++) box2(lin, W, H, 10 + i * 28, 74, 11 + i * 28, 104, bay);
    box2(lin, W, H, 0, 70, W - 1, 71, bay);

    /* cars: body, cabin, wheels, and a pair of lights */
    for (int c = 0; c < 3 + (v & 1); c++) {
        int cx = 18 + c * 62 + v * 21, cy = 84 + ((c + v) % 2) * 10;
        u16 col = carcol[(c + v) % 4];
        if (cx > W - 36) cx -= W - 36;
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
        int lx = 40 + l * 80 + v * 11;
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
    memcpy(T->data, til, W * H * 2);
    C3D_TexFlush(T);
    C3D_TexSetFilter(T, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(T, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    free(lin); free(til);
    g_outside_ok[v] = 1;
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
    for (int v = 0; v < OUTSIDES; v++) make_outside_tex(v);   /* a street per window */
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
/* Ten spine textures, one per colour, instead of one texture and a tint uniform.
 *
 * The uniform meant touching the vertex shader, and that is the one change in this whole
 * prototype that was never once seen working -- the console locked hard from the commit it
 * arrived in. A texture bind costs the same as a uniform write and needs no shader at all, so
 * this buys the same varied shelf with the shader left exactly as it was. Ten of them at
 * 16x64 is 20 KB. */
static C3D_Tex g_spine[SPINE_COLOURS];
static int     g_spine_ok = 0;
static void make_spine_tex(void) {
    const int W = 16, H = 64;
    u16 *lin = (u16 *)malloc(W * H * 2);
    u16 *til = (u16 *)malloc(W * H * 2);
    if (!lin || !til) { free(lin); free(til); return; }
    for (int c = 0; c < SPINE_COLOURS; c++) {
        if (!C3D_TexInit(&g_spine[c], W, H, GPU_RGB565)) { free(lin); free(til); return; }
        /* 0: the house case, white with a blue head and foot -- most of the shelf
         * 1: a black case, the ones that never got re-cased
         * 2: a grey case, slightly worn */
        u16 shell   = (c == 0) ? 0xE73C : (c == 1) ? 0x18E3 : 0x8410;
        u16 shellhi = (c == 0) ? 0xFFFF : (c == 1) ? 0x39E7 : 0xAD55;
        u16 shelllo = (c == 0) ? 0xAD55 : (c == 1) ? 0x1082 : 0x630C;
        u16 band    = (c == 0) ? TH_BLUE : (c == 1) ? 0x4208 : 0x39E7;
        u16 paper   = (c == 1) ? 0xC618 : 0xFFFF;
        u16 ink     = (c == 1) ? 0x8410 : 0x6B4D;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                u16 v = shell;
                if (x <= 1)                 v = shelllo;      /* shadowed edge */
                else if (x >= W - 2)        v = shellhi;      /* lit edge */
                if (y < 2 || y > H - 3)     v = shelllo;      /* caps */
                else if (y < 9)             v = band;         /* head band */
                else if (y > H - 12)        v = band;         /* foot band */
                else if (y >= 12 && y < 34) {                 /* the printed label */
                    v = paper;
                    if (x <= 2 || x >= W - 3)    v = shell;
                    else if (y >= 15 && y <= 16) v = ink;      /* lines of title */
                    else if (y >= 19 && y <= 20) v = ink;
                    else if (y >= 24 && y <= 24) v = ink;
                }
                lin[y * W + x] = v;
            }
        tile_rgb565(lin, til, W, H);
        memcpy(g_spine[c].data, til, W * H * 2);
        C3D_TexFlush(&g_spine[c]);
        C3D_TexSetFilter(&g_spine[c], GPU_NEAREST, GPU_NEAREST);
    }
    free(lin); free(til);
    g_spine_ok = 1;
}

/* The front of the house clamshell, blank.
 *
 * Most of a shelf faces out now, and only eight cases can hold a real cover at once. A face-out
 * case beyond reading distance wears this instead: the shop's own case seen from the front,
 * which is what you actually see across a room. The real art arrives as you walk up to it.
 * Same LOD idea as the spines, one step closer. */
static C3D_Tex g_front;
static int     g_front_ok = 0;
static void make_front_tex(void) {
    const int W = 32, H = 64;
    if (!C3D_TexInit(&g_front, W, H, GPU_RGB565)) return;
    u16 *lin = (u16 *)malloc(W * H * 2), *til = (u16 *)malloc(W * H * 2);
    if (!lin || !til) { free(lin); free(til); C3D_TexDelete(&g_front); return; }
    const u16 shell = 0xE73C, edge = 0xAD55, band = TH_BLUE, paper = 0xFFFF, ink = 0xC618;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            u16 v = shell;
            if (x < 2 || x >= W - 2 || y < 2 || y >= H - 2) v = edge;
            else if (y < 12)            v = band;             /* head */
            else if (y > H - 14)        v = band;             /* foot */
            else if (x > 4 && x < W - 5 && y > 16 && y < H - 18) {
                v = paper;                                    /* the art window, blank */
                if (((x * 3 + y * 5) % 17) < 2) v = ink;       /* a hint of print */
            }
            lin[y * W + x] = v;
        }
    tile_rgb565(lin, til, W, H);
    memcpy(g_front.data, til, W * H * 2);
    C3D_TexFlush(&g_front);
    C3D_TexSetFilter(&g_front, GPU_LINEAR, GPU_LINEAR);
    free(lin); free(til);
    g_front_ok = 1;
}

/* a plain white sheet for the light fittings and as a safe fallback */
static C3D_Tex g_white;
static int     g_white_ok = 0;
static void make_white_tex(void) {
    const int W = 8, H = 8;
    if (!C3D_TexInit(&g_white, W, H, GPU_RGB565)) return;
    u16 *lin = (u16 *)malloc(W * H * 2), *til = (u16 *)malloc(W * H * 2);
    if (!lin || !til) { free(lin); free(til); C3D_TexDelete(&g_white); return; }
    for (int i = 0; i < W * H; i++) lin[i] = 0xFFFF;
    tile_rgb565(lin, til, W, H);
    memcpy(g_white.data, til, W * H * 2);
    C3D_TexFlush(&g_white);
    C3D_TexSetFilter(&g_white, GPU_NEAREST, GPU_NEAREST);
    free(lin); free(til);
    g_white_ok = 1;
}

/* Framed posters: the walls and the ends of the units. Real covers from the shelves rather
 * than invented art -- a video shop advertises what it has in stock. Six of them at 64x128 is
 * 96 KB, which is the whole decorating budget.
 *
 * If you later want fixed art up there instead, this is the hook: drop a 132x188 .p565 in as
 * store/wallN.p565 and it will be used in preference to a title from the shelves. */
/* Enough distinct titles that the same face does not stare back at you from three walls. There
 * are up to ~20 places a poster can hang, so a few still repeat -- but never side by side. */
/* Framed art is drawn about a metre wide, four times the size a case is, so it gets four
 * times the pixels. The ratio is deliberately the same as a cover's -- 182/256 == 91/128 --
 * so the poster quad's UVs fit both without a second quad. */
#define WALL_TEX_W 128
#define WALL_TEX_H 256
#define WALL_IMG_W 128
#define WALL_IMG_H 182
#define WALLPOSTERS 16
static C3D_Tex g_wall[WALLPOSTERS];
static int     g_wall_ok[WALLPOSTERS];
static int     g_wall_user[WALLPOSTERS];   /* 1 = supplied art, 0 = a cover borrowed off a shelf */
static int     g_wall_n = 0;

static void make_wall_posters(void) {
    for (int i = 0; i < WALLPOSTERS; i++) {
        /* Your own art first: store/wallN.p565, a 132x188 raw like the player's own caches.
         * It is only ever READ -- the scaled copy goes to a separate file, so dropping art in
         * here can never destroy it. */
        char user[400], small[400];
        snprintf(user,  sizeof user,  "%s/wall%d.p565", CACHE_DIR, i);
        snprintf(small, sizeof small, "%s/wall%d.x565", CACHE_DIR, i);
        int from_user = 0;
        { FILE *uf = fopen(user, "rb");
          if (uf) { fclose(uf); from_user = 1; } }
        if (!from_user && i < ART_WALLS) {
            /* built in: no file to copy anywhere, and still overridable by one */
            if (build_tex_mem(art_wall[i], ART_WALL_W, ART_WALL_H, &g_wall[i],
                              WALL_TEX_W, WALL_TEX_H, WALL_IMG_W, WALL_IMG_H)) {
                g_wall_ok[i] = 1; g_wall_user[i] = 1; g_wall_n++;
            }
            continue;
        }
        Poster *q = NULL;
        if (!from_user) {
            /* otherwise borrow a title from the shelves, spread across the catalogue */
            if (g_nposters <= 0) return;
            int pick = (int)((long)i * g_nposters / WALLPOSTERS);
            if (pick >= g_nposters) pick = g_nposters - 1;
            q = &g_pos[pick];
            if (!q->srcpath[0]) continue;
            snprintf(small, sizeof small, "%s/%s.x565", CACHE_DIR, q->key);  /* framed, not shelf */
        }
        FILE *cf = fopen(small, "rb");
        if (!cf) {
            const char *src = from_user ? user : q->srcpath;
            int sw = from_user ? SRC_W : q->src_w, sh = from_user ? SRC_H : q->src_h;
            if (!build_cache_entry_sz(src, sw, sh, small,
                                      WALL_TEX_W, WALL_TEX_H, WALL_IMG_W, WALL_IMG_H)) continue;
            cf = fopen(small, "rb");
            if (!cf) continue;
        }
        if (!C3D_TexInit(&g_wall[i], WALL_TEX_W, WALL_TEX_H, GPU_RGB565)) { fclose(cf); continue; }
        size_t got = fread(g_wall[i].data, 1, (size_t)WALL_TEX_W * WALL_TEX_H * 2, cf);
        fclose(cf);
        if (got != (size_t)WALL_TEX_W * WALL_TEX_H * 2) { C3D_TexDelete(&g_wall[i]); continue; }
        C3D_TexFlush(&g_wall[i]);
        C3D_TexSetFilter(&g_wall[i], GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&g_wall[i], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        g_wall_ok[i] = 1; g_wall_user[i] = from_user; g_wall_n++;
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
        if (fx * fx + fz * fz > 9.0f) continue;
        /* and on the side of the unit you are standing on: a case faces along its own ay, so
         * only one turned toward you may be taken. Reaching through the back of a bay and
         * lifting a case off the far side is not something a shop allows. */
        if (sinf(g_pos[i].ay) * fx + cosf(g_pos[i].ay) * fz > 0.0f) continue;
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

/* is `name` one of the genres this title lists, at any position? */
static int genre_listed(const char *genres, const char *name) {
    if (!genres[0] || !name[0]) return 0;
    size_t nl = strlen(name);
    for (const char *q = genres; *q; ) {
        while (*q == ' ' || *q == ',') q++;
        if (!*q) break;
        const char *e = q; while (*e && *e != ',') e++;
        size_t len = (size_t)(e - q);
        while (len && q[len - 1] == ' ') len--;
        if (len == nl && !strncasecmp(q, name, nl)) return 1;
        q = *e ? e + 1 : e;
    }
    return 0;
}

/* Shelve `src` again, in section k. Metadata is 1.2 KB and the cover is shared with the
 * original, so a second copy costs almost nothing. */
static int add_copy(int src, int k) {
    if (g_nposters >= MAX_POSTERS) return -1;
    int m = g_nposters++;
    g_pos[m] = g_pos[src];
    memset(&g_pos[m].tex, 0, sizeof g_pos[m].tex);   /* NOT its own -- and must not be deleted */
    g_pos[m].tex_ok  = 0;
    g_pos[m].copy_of = (g_pos[src].copy_of >= 0) ? g_pos[src].copy_of : src;
    g_pos[m].sect    = k;
    g_pos[m].order   = g_sec[k].n++;
    return m;
}

/* The idx'th genre a title lists, upper-cased. Returns 0 when there are no more.
 *
 * A title with no genres answers GENERAL to its first, which is what first_genre says too.
 * The two MUST agree: sections are counted with one and filled with the other, so if this
 * returned nothing for an empty list the GENERAL bay would be built and never filled. */
static int genre_token(const char *g, int idx, char *out, size_t cap) {
    if (!g || !g[0]) {
        if (idx != 0) return 0;
        snprintf(out, cap, "GENERAL");
        return 1;
    }
    const char *p = g;
    for (int k = 0; ; k++) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) return 0;
        const char *e = p; while (*e && *e != ',') e++;
        if (k == idx) {
            size_t len = (size_t)(e - p);
            while (len && p[len - 1] == ' ') len--;
            if (len >= cap) len = cap - 1;
            for (size_t i = 0; i < len; i++) {
                char c = p[i];
                out[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
            }
            out[len] = 0;
            return len > 0;
        }
        p = *e ? e + 1 : e;
    }
}

static void place_section(int k);
static void bake_spines(void);
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
    /* Build the section list from what is actually there.
     *
     * A genre too small for a unit merges into OTHER; one too big takes a second unit rather
     * than hiding half of itself behind a MORE case. Then each bay is built to the length its
     * contents need. The shop ends up the size of the library instead of the library rattling
     * around inside a fixed shop. */
    int spare = MAX_SECTIONS;
    int other = 0;
    for (int i = 0; i < uniq && spare > 2; i++) {   /* two held back: OTHER and NEW RELEASES */
        if (count[i] < BAY_MIN) { other += count[i]; continue; }
        /* A genre may take a SECOND bay when it has the stock for one, but never a third --
         * that is what once turned ten genres into seventeen bays and a great deal of floor to
         * cross. Two is what fills the centre column without the room running away. Anything
         * past two bays' worth restocks instead, which costs nothing while every title is
         * already resident. */
        int units = (count[i] >= BAY_MAX * 2) ? 2 : 1;
        if (units > spare - 1) units = spare - 1;
        for (int u = 0; u < units; u++) {
            int share = count[i] / units + ((u < count[i] % units) ? 1 : 0);
            if (units > 1) snprintf(g_sec[g_nsec].name, 24, "%.14s %d", names[i], u + 1);
            else           snprintf(g_sec[g_nsec].name, 24, "%s", names[i]);
            g_sec[g_nsec].part = u;
            g_sec[g_nsec].n    = share;      /* provisional: the real tally follows below */
            g_nsec++; spare--;
        }
    }
    for (int i = 0; i < uniq; i++) if (count[i] >= BAY_MIN) continue; else (void)0;
    snprintf(g_sec[g_nsec].name, 24, "%s", other ? "OTHER" : "GENERAL");
    int other_idx = g_nsec++;
    /* A rack of its own in the back corner, angled to the room. It is an ordinary bay in every
     * respect -- unit, boards, sign, blocker all come from the same code -- it just takes its
     * stock by year rather than by genre, and it is placed by hand instead of from PLAN. */
    g_new_idx = -1;
    if (g_nsec < MAX_SECTIONS) {
        g_new_idx = g_nsec;
        snprintf(g_sec[g_nsec].name, 24, "NEW RELEASES");
        g_sec[g_nsec].part = 0;
        g_sec[g_nsec].n = 14;                  /* provisional: a corner rack, not a full bay */
        g_nsec++;
    }
    /* Build each bay to its contents: a row holds `len / PITCH_FACE` cases, BAY_ROWS of them. */
    for (int k = 0; k < g_nsec; k++) {
        /* a face takes more shelf than a spine, so the mix decides how much a bay holds */
        float avg = PITCH_FACE;                  /* one width now: everything faces out */
        float need = ((float)g_sec[k].n / (float)BAY_ROWS) * avg + 0.5f;
        if (need < UNIT_LEN_MIN) need = UNIT_LEN_MIN;
        if (need > UNIT_LEN)     need = UNIT_LEN;
        g_sec[k].len = need;
        g_sec[k].per_row = (int)((need - 0.30f) / avg);
        if (g_sec[k].per_row < 1) g_sec[k].per_row = 1;
        if (g_sec[k].per_row > PER_ROW) g_sec[k].per_row = PER_ROW;
        g_sec[k].Lper_row = (int)((3.2f - 0.30f) / avg);
        g_sec[k].Lcap = 0;                          /* filled in once has_L is known */
    }

    /* Fit the room to the fixtures. The walkway wants about four metres between the two runs
     * of bays; the depth follows from how many rows of bays there are. */
    {
        float maxlen = UNIT_LEN_MIN;
        for (int k = 0; k < g_nsec; k++) if (g_sec[k].len > maxlen) maxlen = g_sec[k].len;
        /* The walkway is whatever gap is left between the two runs of bays, so the room comes
         * out only as wide as it needs to be. It used to clear 2.4 a side off the longest bay
         * with an 8.5 floor under it, which on a short bay left the whole middle of the shop
         * as bare carpet. 1.5 a side is a walkway; the rest was floor to cross. */
        /* Past six sections a third column runs down the middle of the room, so the floor has
         * to carry three bays across instead of two: bay, aisle, bay, aisle, bay. */
        int centre = (g_nsec > 6);
        g_hx = centre ? (maxlen * 1.5f + 1.6f) : (maxlen + 1.5f);
        if (g_hx < 7.2f)  g_hx = 7.2f;
        int side = g_nsec < 6 ? g_nsec : 6;
        int rows = centre ? 3 : (side + 1) / 2;     /* they fill in left/right pairs */
        if (rows < 1) rows = 1;
        g_depth = 6.0f + rows * ROW_PITCH + 3.4f;   /* door end + aisles + the back run */
        if (g_depth < 13.0f) g_depth = 13.0f;
    }

    /* Bays, the way a rental shop is actually laid out: units run OUT FROM THE WALLS with
     * their far end against the wall, leaving a clear walkway up the middle of the room that
     * reaches every section. Islands floating in open carpet read as crates; this reads as a
     * shop you can navigate. The back corners turn in to close the room off. */
    /* left column, right column, then a pair across the back -- spaced to the room's depth */
    float rowz[3];
    for (int r = 0; r < 3; r++) rowz[r] = -6.4f - r * ROW_PITCH;
    g_gapz[0] = (rowz[0] + rowz[1]) * 0.5f;   /* midway between rows: where wall art hangs */
    g_gapz[1] = (rowz[1] + rowz[2]) * 0.5f;
    /* left, right, left, right... A column-at-a-time order put the first three bays all on
     * one wall, so a shop with three sections had a bare side. */
    /* Left and right walls first, because a shop with a bare side reads as unfinished, then
     * the centre column, then the back wall. An x sign of 0 IS the centre: cx works out to
     * zero through the same expression the wall bays use, with no special case. */
    const float PLAN[MAX_SECTIONS][3] = {          /* x sign, z, rotation */
        { -1.0f, rowz[0], 0.0f },
        {  1.0f, rowz[0], 0.0f },
        { -1.0f, rowz[1], 0.0f },
        {  1.0f, rowz[1], 0.0f },
        { -1.0f, rowz[2], 0.0f },
        {  1.0f, rowz[2], 0.0f },
        {  0.0f, rowz[0], 0.0f },                  /* the centre column */
        {  0.0f, rowz[1], 0.0f },
        {  0.0f, rowz[2], 0.0f },
        {  1.0f, -99.0f, 0.0f },                   /* -99 marks the back wall run; the left
                                                    * half of it belongs to NEW RELEASES */
    };
    for (int i = 0; i < g_nsec; i++) {
        /* the far end sits against the wall; the near end reaches toward the walkway by
         * however long this bay needs to be */
        if (i == g_new_idx) {
            /* Square to the room like everything else. On the diagonal its sign hung straight
             * out while the bay ran at 45, and the end poster stood off the end of it -- the
             * sign and poster code both assume a bay faces the way the room does. */
            g_sec[i].cx  = -STORE_HX + g_sec[i].len * 0.5f + 1.4f;
            g_sec[i].cz  = STORE_Z0 - STORE_DEPTH + UNIT_DEPTH * 0.5f + 0.06f;   /* flush */
            g_sec[i].rot = 0.0f;
            g_sec[i].facedir = 1.0f;
            g_sec[i].has_L = 0;
            make_sign_tex(&g_sec[i].sign, g_sec[i].name);
            g_sec[i].sign_ok = 1;
            continue;
        }
        float side = PLAN[i][0];
        int   back = (PLAN[i][1] < -90.0f);
        if (side == 0.0f && back) side = -1.0f;     /* nothing parks under the shop name */
        if (back) { g_sec[i].cx = side * (g_sec[i].len * 0.5f + 1.4f);
                    g_sec[i].cz = STORE_Z0 - STORE_DEPTH + UNIT_DEPTH * 0.5f + 0.06f; }
                    /* Flush to the wall. Standing it off left a strip of floor you could see
                     * but not use, which reads worse than no gap at all -- and these face
                     * forward, so there is nothing behind them to reach. */
        else      { g_sec[i].cx = side * (STORE_HX - g_sec[i].len * 0.5f - 0.15f);
                    g_sec[i].cz = PLAN[i][1]; }
        g_sec[i].rot = PLAN[i][2];
        /* Every bay faces the door. Alternating them meant half the shop had its stock on
         * the far side, so you walked past a plain wooden back and had to go round to see
         * anything -- and the covers on it were facing a wall. */
        g_sec[i].facedir = 1.0f;
        /* An L return needs a bay long enough that turning the corner is worth it. The old
         * threshold wanted a whole unit over the minimum, which after the stock was filtered
         * no section reached -- so the shop had no returns in it at all. */
        g_sec[i].has_L   = (i < 6) && (g_sec[i].len > UNIT_LEN_MIN + 0.4f) && !back;
        /* the return runs along z at the inner end, facing the walkway */
        float inner = g_sec[i].cx + ((g_sec[i].cx < 0) ? g_sec[i].len * 0.5f : -g_sec[i].len * 0.5f);
        g_sec[i].Llen = 3.2f;
        /* Clear of the bay, not half inside it. Lx was the bay's own end, and the return is a
         * unit deep, so half of it sat on top of the last column of cases -- which is why the
         * restock case came out sliced down the middle. It starts where the bay stops. */
        g_sec[i].Lx   = inner + ((g_sec[i].cx < 0) ? UNIT_DEPTH * 0.5f : -UNIT_DEPTH * 0.5f);
        g_sec[i].Lz   = g_sec[i].cz + 1.6f;
        g_sec[i].Lay  = (g_sec[i].cx < 0) ? C3D_Angle(0.25f) : C3D_Angle(-0.25f);
        make_sign_tex(&g_sec[i].sign, g_sec[i].name);
        g_sec[i].sign_ok = 1;
    }

    /* Only NOW is the provisional count spent. It used to be cleared before the bay lengths
     * were worked out, so `need` was always 0.5 and every bay in the shop clamped to the
     * minimum length whatever it held -- which is why raising UNIT_LEN never changed anything.
     * The lengths are set above; from here the count is rebuilt for real. */
    for (int k = 0; k < g_nsec; k++) g_sec[k].n = 0;

    /* Which bay each title belongs to. A split genre has several units named "COMEDY 1",
     * "COMEDY 2" -- match on the genre and take whichever of its units is emptiest, so the
     * pair fill evenly rather than one being full and one bare. */
    /* Titles WITH a description are shelved first, and the ones without take what is left.
     *
     * A bay shows one page at a time and restocks for the rest, so this does not hide anything
     * -- it decides what is on the shelf when you walk in. A case with no title and no blurb
     * is the least useful thing we can put in front of someone, so it goes behind the ones
     * that can actually answer a question about themselves. */
    for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < g_nposters; i++) {
        if ((g_pos[i].hasinfo ? 0 : 1) != pass) continue;
        /* EVERY genre it lists, in order, not just the first.
         *
         * A section exists only for a genre that is first on enough titles, but assignment
         * used to look at the first genre and nothing else -- so a film listed
         * "Mystery, Horror" went to the catch-all because Mystery was too small, with the
         * HORROR bay standing next to it. Its first genre still wins where that bay exists;
         * the rest are what it falls back on before giving up and going to OTHER. */
        int k = other_idx;                             /* OTHER unless some genre matches */
        int best = -1;
        char g[24];
        for (int gi = 0; best < 0 && genre_token(g_pos[i].genres, gi, g, sizeof g); gi++) {
            size_t gl = strlen(g);
            for (int j = 0; j < g_nsec; j++) {
                if (j == g_new_idx) continue;          /* stocked by year, not by genre */
                if (strncmp(g_sec[j].name, g, gl)) continue;
                char t = g_sec[j].name[gl];
                if (t != 0 && t != ' ') continue;      /* "COMEDY" must not match "COMEDYDRAMA" */
                if (best < 0 || g_sec[j].n < g_sec[best].n) best = j;
            }
        }
        if (best >= 0) k = best;
        g_pos[i].sect  = k;
        g_pos[i].order = g_sec[k].n++;
    }


    /* A bay that cannot hold its whole genre gets a RESTOCK case in the top-left slot: pick it up,
     * press the verb, and the shelf turns over to the next lot. Only where it is needed -- a
     * bay with room to spare should not carry a control nobody has to press. */
    for (int k = 0; k < g_nsec; k++) {
        g_sec[k].Lcap = g_sec[k].has_L ? BAY_ROWS * g_sec[k].Lper_row : 0;
        g_sec[k].cap  = BAY_ROWS * g_sec[k].per_row + g_sec[k].Lcap;
    }

    /* An empty bay is given to whichever genre has the most it cannot show.
     *
     * The catch-all earns this regularly now that a film can be filed under any genre it
     * lists: there is often nothing left over for it. Rather than stand there with a lit sign
     * and no stock -- or vanish and leave a hole in the floor plan -- it becomes a second bay
     * for whatever is most overflowing, taking the stock that was behind that bay's restock.
     * The shop keeps its shape and one more genre stops needing a restock to be seen. */
    for (int k = 0; k < g_nsec; k++) {
        if (k == g_new_idx || g_sec[k].n > 0) continue;
        int src = -1;
        for (int j = 0; j < g_nsec; j++) {
            if (j == k || j == g_new_idx || g_sec[j].n <= g_sec[j].cap) continue;
            if (src < 0 || (g_sec[j].n - g_sec[j].cap) > (g_sec[src].n - g_sec[src].cap)) src = j;
        }
        if (src < 0) continue;                     /* nothing is overflowing: it will be hidden */
        int moved = 0;
        for (int i = 0; i < g_nposters && moved < g_sec[k].cap; i++) {
            if (g_pos[i].is_more || g_pos[i].sect != src) continue;
            if (g_pos[i].order < g_sec[src].cap) continue;   /* page one stays where it is */
            g_pos[i].sect = k;
            moved++;
        }
        if (!moved) continue;
        g_sec[src].n = 0; g_sec[k].n = 0;          /* both runs renumber from the start */
        for (int i = 0; i < g_nposters; i++) {
            if (g_pos[i].is_more) continue;
            if      (g_pos[i].sect == src) g_pos[i].order = g_sec[src].n++;
            else if (g_pos[i].sect == k)   g_pos[i].order = g_sec[k].n++;
        }
        /* "HORROR" and "HORROR 2", however many times it has already happened */
        char base[24]; snprintf(base, sizeof base, "%s", g_sec[src].name);
        { size_t b = strlen(base);
          while (b > 2 && base[b-1] >= '0' && base[b-1] <= '9') b--;
          if (b > 1 && base[b-1] == ' ') base[b-1] = 0; }
        int nth = 1;
        for (int j = 0; j < g_nsec; j++)
            if (j != k && !strncmp(g_sec[j].name, base, strlen(base))) nth++;
        snprintf(g_sec[k].name, 24, "%.16s %d", base, nth);
        if (g_sec[k].sign_ok) C3D_TexDelete(&g_sec[k].sign);   /* it said OTHER a moment ago */
        make_sign_tex(&g_sec[k].sign, g_sec[k].name);
        g_sec[k].sign_ok = 1;
    }

    /* The new-releases rack takes the highest years first. Not a release date -- nothing on
     * the card carries one -- but the year is what a shop would have gone by anyway. */
    if (g_new_idx >= 0) {
        int base = g_nposters;
        int cap  = g_sec[g_new_idx].cap;
        for (int slot = 0; slot < cap; slot++) {
            int best = -1;
            for (int i = 0; i < base; i++) {
                if (!g_pos[i].ok || g_pos[i].is_more || g_pos[i].copy_of >= 0) continue;
                if (g_pos[i].sect == g_new_idx || !g_pos[i].year) continue;
                int dup = 0;                           /* not twice on the same rack */
                for (int j = base; j < g_nposters && !dup; j++)
                    if (g_pos[j].copy_of == i) dup = 1;
                if (dup) continue;
                if (best < 0 || g_pos[i].year > g_pos[best].year) best = i;
            }
            if (best < 0) break;
            if (add_copy(best, g_new_idx) < 0) break;
        }
    }

    /* A bay with four films in it and room for forty looks stripped, and how empty it looks
     * depends entirely on how big the person's library is -- which we do not control. So fill
     * it, the way a shop would: first with films that name this genre further down their list
     * and were filed elsewhere on their first, then with second and third copies of what is
     * already there. A rental shop carrying four copies of the same new release is what the
     * shelves actually looked like. */
    for (int k = 0; k < g_nsec; k++) {
        if (g_sec[k].n == 0 || g_sec[k].n >= g_sec[k].cap) continue;
        int named = strcasecmp(g_sec[k].name, "OTHER") && strcasecmp(g_sec[k].name, "GENERAL");
        int base = g_nposters;                     /* snapshot: we are appending as we go */
        if (named)
            for (int i = 0; i < base && g_sec[k].n < g_sec[k].cap; i++)
                if (g_pos[i].ok && !g_pos[i].is_more && g_pos[i].sect != k &&
                    g_pos[i].copy_of < 0 && genre_listed(g_pos[i].genres, g_sec[k].name))
                    if (add_copy(i, k) < 0) break;
        /* then round-robin over this section's own stock until the bay is full */
        int guard = 0;
        while (g_sec[k].n < g_sec[k].cap && guard < MAX_POSTERS) {
            int placed = 0;
            for (int i = 0; i < base && g_sec[k].n < g_sec[k].cap; i++) {
                if (!g_pos[i].ok || g_pos[i].is_more || g_pos[i].sect != k) continue;
                if (g_pos[i].copy_of >= 0) continue;
                if (add_copy(i, k) < 0) { guard = MAX_POSTERS; break; }
                placed = 1;
            }
            if (!placed) break;                    /* nothing in here to copy: leave the gap */
            guard++;
        }
    }

    /* Copies stand NEXT TO the title they are copies of.
     *
     * The fill goes round the shelf adding one copy of each title per lap, which spreads them
     * evenly -- and left them scattered down the run, one here and one four slots along, which
     * looks like a filing error rather than stock. A shop with four of a title puts the four
     * together and that is the whole visual point of having them. So the run is renumbered by
     * TITLE: each one keeps the position it first appeared at, and its copies follow it.
     *
     * Copies filed here off a secondary genre have their original on another shelf entirely;
     * they group among themselves, wherever the first of them landed. */
    {   static char done[MAX_POSTERS];
        for (int k = 0; k < g_nsec; k++) {
            memset(done, 0, sizeof done);
            int next = 0;
            for (int i = 0; i < g_nposters; i++) {
                if (g_pos[i].is_more || g_pos[i].sect != k) continue;
                int root = g_pos[i].copy_of >= 0 ? g_pos[i].copy_of : i;
                if (done[root]) continue;
                done[root] = 1;
                for (int j = 0; j < g_nposters; j++) {
                    if (g_pos[j].is_more || g_pos[j].sect != k) continue;
                    int r = g_pos[j].copy_of >= 0 ? g_pos[j].copy_of : j;
                    if (r == root) g_pos[j].order = next++;
                }
            }
        }
    }

    for (int k = 0; k < g_nsec; k++) {
        g_sec[k].more_idx = -1;
        g_sec[k].page = 0;
        if (g_sec[k].n > g_sec[k].cap && g_nposters < MAX_POSTERS) {
            g_sec[k].cap -= 1;                         /* the MORE case takes a slot */
            int m = g_nposters++;
            memset(&g_pos[m], 0, sizeof g_pos[m]);
            g_pos[m].ok = 1; g_pos[m].is_more = 1; g_pos[m].sect = k; g_pos[m].copy_of = -1;
            g_pos[m].col = 1;   /* a black case: the MORE marker stands out on a white run */
            snprintf(g_pos[m].name, sizeof g_pos[m].name, "RESTOCK %s", g_sec[k].name);
            g_sec[k].more_idx = m;
        }
        int cap = g_sec[k].cap > 0 ? g_sec[k].cap : 1;
        g_sec[k].pages = (g_sec[k].n + cap - 1) / cap;
        if (g_sec[k].pages < 1) g_sec[k].pages = 1;
    }
    /* A bay with a lit sign over it and not one case on it looks like a fault, so it is not
     * built at all. The catch-all earns this regularly: once a title can be filed under any
     * genre it lists rather than only its first, there is often nothing left over for it. */
    for (int k = 0; k < g_nsec; k++) g_sec[k].hide = (g_sec[k].n == 0);
    for (int k = 0; k < g_nsec; k++) place_section(k);
    bake_spines();
}

/* Position one bay's stock for its current page. Called again when the MORE case is used. */
/* Where each slot sits along a row.
 *
 * Multiplying a slot's index by ITS OWN pitch only works if every case is the same width. Once
 * a row mixes spines at 0.235 and covers at 0.34, slot five is in a different place depending
 * on which kind of case you ask -- which is why the faces sat on top of the spines. Walk the
 * row and accumulate the widths instead, then centre the whole run. */
static float g_slotx[1024];
static void row_offsets(int first_slot, int count) {
    (void)first_slot;                     /* uniform widths: the slot no longer changes the pitch */
    float x = -(count * PITCH_FACE) * 0.5f;
    for (int i = 0; i < count && i < 1024; i++) {
        g_slotx[i] = x + PITCH_FACE * 0.5f;
        x += PITCH_FACE;
    }
}

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
        /* every Nth one turns its face to the aisle */
        p->faceout = 1;                  /* the restock case faces out too -- it wears a cover */
        int main_cap = BAY_ROWS * S->per_row;
        if (sl < main_cap) {                          /* the long side */
            int row  = (BAY_ROWS - 1) - (sl / S->per_row), colp = sl % S->per_row;
            if (row < 0) { p->shown = 0; continue; }
            int rowfirst = ((BAY_ROWS - 1) - row) * S->per_row;
            int inrow = total_slots - rowfirst;
            if (inrow > S->per_row) inrow = S->per_row;
            if (inrow < 1) inrow = 1;
            row_offsets(rowfirst, inrow);
            float lx = g_slotx[colp];
            float lz = S->facedir * (UNIT_DEPTH * 0.5f + 0.02f);
            float ca = cosf(S->rot), sa = sinf(S->rot);
            p->x  = S->cx + lx * ca + lz * sa;
            p->z  = S->cz - lx * sa + lz * ca;
            p->y  = ROW_Y0 + row * ROW_DY;
            p->ay = S->rot + ((S->facedir < 0) ? C3D_Angle(0.5f) : 0.0f);
        } else {                                      /* round the corner, onto the return */
            if (!S->has_L) { p->shown = 0; continue; }
            int t = sl - main_cap;
            int row = (BAY_ROWS - 1) - (t / S->Lper_row), colp = t % S->Lper_row;
            if (row < 0) { p->shown = 0; continue; }
            int rowfirst = main_cap + ((BAY_ROWS - 1) - row) * S->Lper_row;
            int left = total_slots - rowfirst;
            if (left > S->Lper_row) left = S->Lper_row;
            if (left < 1) left = 1;
            row_offsets(rowfirst, left);
            float along = g_slotx[colp];
            float outn  = (UNIT_DEPTH * 0.5f + 0.02f) * ((S->cx < 0) ? 1.0f : -1.0f);
            p->x  = S->Lx + outn;                     /* the return faces the walkway */
            p->z  = S->Lz + along;
            p->y  = ROW_Y0 + row * ROW_DY;
            p->ay = S->Lay;
        }
        /* bake the model matrix now */
        Mtx_Identity(&p->model);
        Mtx_Translate(&p->model, p->x, p->y, p->z, true);
        Mtx_RotateY(&p->model, p->ay, true);
        if (p->faceout) Mtx_Scale(&p->model, CASE_W, CASE_W * (float)IMG_H / (float)IMG_W, 1.0f);
        else            Mtx_Scale(&p->model, 0.20f, 0.56f, 1.0f);
    }
}

/* ---------------- geometry ---------------- */
#define ROOM_VTX     5200        /* shell + units + counter + wall shelving */
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
        if (g_sec[i].hide) continue;
        push_box_rot(g_roomv, &n, g_sec[i].cx, UNIT_H * 0.5f, g_sec[i].cz,
                     g_sec[i].len * 0.5f, UNIT_H * 0.5f, UNIT_DEPTH * 0.5f, g_sec[i].rot,
                     3, 1, 0.52f);
        /* A board under each row and one over the top, standing a little proud of the face.
         * Without them the cases hang on a flat slab -- the boards are what make it read as
         * shelving. They go in the same batch as the unit, so they cost no extra draw. */
        const float CH = CASE_W * (float)IMG_H / (float)IMG_W;
        for (int r = 0; r <= BAY_ROWS; r++) {
            float by = ROW_Y0 + r * ROW_DY - CH * 0.5f - 0.035f;
            push_box_rot(g_roomv, &n, g_sec[i].cx, by, g_sec[i].cz,
                         g_sec[i].len * 0.5f, 0.035f, UNIT_DEPTH * 0.5f + 0.045f,
                         g_sec[i].rot, 3, 1, 0.70f);
        }
    }
    /* The jukebox, opposite the counter. A cabinet with a lit arch on the front -- it is a
     * prop, so it is two boxes and a panel, but it is a landmark you can walk to and press. */
    {   g_jukex = STORE_HX - 0.46f; g_jukez = STORE_Z0 - 3.2f;    /* flat against the right wall */
        g_jukerot = -1.5708f;                                     /* facing the counter across the room */
        push_box_rot(g_roomv, &n, g_jukex, 0.62f, g_jukez, 0.55f, 0.62f, 0.40f, g_jukerot, 1, 1, 0.44f);
        push_box_rot(g_roomv, &n, g_jukex, 1.34f, g_jukez, 0.50f, 0.16f, 0.36f, g_jukerot, 1, 1, 0.66f); }

    /* Back of house, in the far right corner: a service door and the crates that pile up
     * beside one. Geometry only -- no texture work -- but an empty corner reads as an
     * unfinished room, and a door reads as somewhere the stock comes from. */
    {   float sx = STORE_HX - 2.3f, sz = STORE_Z0 - STORE_DEPTH + 0.10f;
        push_box(g_roomv, &n, sx, 1.05f, sz, 0.62f, 1.05f, 0.07f, 1, 1, 0.26f);   /* the door */
        push_box(g_roomv, &n, sx, 2.18f, sz, 0.70f, 0.09f, 0.09f, 1, 1, 0.60f);   /* its lintel */
        push_box(g_roomv, &n, sx + 0.44f, 1.02f, sz + 0.10f, 0.05f, 0.10f, 0.05f, 1, 1, 0.85f);
        push_box(g_roomv, &n, sx + 1.45f, 0.34f, sz + 0.75f, 0.42f, 0.34f, 0.38f, 1, 1, 0.46f);
        push_box(g_roomv, &n, sx + 1.38f, 0.94f, sz + 0.68f, 0.36f, 0.26f, 0.32f, 1, 1, 0.54f);
        push_box(g_roomv, &n, sx + 2.15f, 0.28f, sz + 0.55f, 0.34f, 0.28f, 0.30f, 1, 1, 0.42f); }

    /* the counter: a long wood block by the door, a register on top, and a returns box */
    /* Hard against the left wall, and shorter. It reached far enough into the room to foul
     * the nearest bay, and the returns bin on its far end read as a second counter. */
    { float ccx = -STORE_HX + 2.15f;
      push_box(g_roomv, &n, ccx, 0.55f, -1.6f, 2.05f, 0.55f, 0.7f, 4, 1, 0.62f);
      push_box(g_roomv, &n, ccx - 0.7f, 1.28f, -1.6f, 0.6f, 0.18f, 0.45f, 1, 1, 0.40f);
      push_box(g_roomv, &n, ccx - 0.7f, 1.60f, -1.75f, 0.5f, 0.14f, 0.22f, 1, 1, 0.78f); }
    /* An L on the end of two bays: a short return that turns the corner, which is what stops a
     * rank of units reading as a row of identical slabs. */
    for (int i = 0; i < g_nsec; i++) {
        if (g_sec[i].hide || !g_sec[i].has_L) continue;
        push_box_rot(g_roomv, &n, g_sec[i].Lx, UNIT_H * 0.5f, g_sec[i].Lz,
                     UNIT_DEPTH * 0.5f, UNIT_H * 0.5f, g_sec[i].Llen * 0.5f, 0.0f, 1, 1, 0.48f);
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

    /* blockers: every unit, plus the counter, the returns bin and the jukebox */
    g_nblock = 0;
    g_block[g_nblock].cx = g_jukex; g_block[g_nblock].cz = g_jukez;
    g_block[g_nblock].hx = 0.95f;   g_block[g_nblock].hz = 0.80f;
    g_block[g_nblock].rot = g_jukerot; g_nblock++;
    /* the crates by the staff door -- walking through a stack of them spoils the illusion */
    g_block[g_nblock].cx = STORE_HX - 0.5f;
    g_block[g_nblock].cz = STORE_Z0 - STORE_DEPTH + 0.85f;
    g_block[g_nblock].hx = 1.4f; g_block[g_nblock].hz = 0.9f;
    g_block[g_nblock].rot = 0.0f; g_nblock++;
    for (int i = 0; i < g_nsec; i++) {
        if (g_sec[i].hide) continue;
        g_block[g_nblock].cx = g_sec[i].cx; g_block[g_nblock].cz = g_sec[i].cz;
        g_block[g_nblock].hx = g_sec[i].len * 0.5f + 0.42f;
        g_block[g_nblock].hz = UNIT_DEPTH * 0.5f + 0.42f;
        g_block[g_nblock].rot = g_sec[i].rot; g_nblock++;
    }
    for (int i = 0; i < g_nsec; i++) {
        if (!g_sec[i].has_L) continue;
        g_block[g_nblock++] = (Blocker){ g_sec[i].Lx, g_sec[i].Lz,
                                         UNIT_DEPTH * 0.5f + 0.42f,
                                         g_sec[i].Llen * 0.5f + 0.42f, 0.0f };
    }
    /* The counter, and only the counter. There was a second blocker here for the returns bin
     * that used to sit on the far end of it -- the bin went and its blocker stayed, so a metre
     * and a half of open floor beside the counter still stopped you dead. And the counter's
     * own was 3.0 wide against a counter of 2.05, so it reached out further than the wood did.
     * Both now come from the same numbers the geometry uses. */
    { float ccx = -STORE_HX + 2.15f;
      g_block[g_nblock++] = (Blocker){ ccx, -1.6f, 2.05f + 0.42f, 0.7f + 0.42f, 0.0f }; }
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
/* All the spines, baked into ONE buffer in world space and grouped by case colour.
 *
 * A case is a fixed thing on a fixed shelf, so there is no reason to send the GPU a matrix and
 * a draw call for each one. Six vertices per case are transformed once, on the CPU, when the
 * shelf is laid out; a frame then draws each colour as a single range. Three draws instead of
 * a hundred and seventy, and no per-case matrix work at all -- which is what the stutter was.
 * Rebuilt only when a shelf is restocked. */
static Vtx *g_spinev;
static int  g_spine_first[SPINE_COLOURS], g_spine_count[SPINE_COLOURS];
static int  g_front_first, g_front_count;      /* the face-out cases, blank fronts */

/* transform the unit quad by a case's matrix and append it, in world space */
static void bake_case(Vtx *dst, int *n, const C3D_Mtx *m, const Vtx *src) {
    for (int k = 0; k < 6; k++) {
        float x = src[k].x, y = src[k].y, z = src[k].z;
        Vtx *o = &dst[(*n)++];
        o->x = m->r[0].x * x + m->r[0].y * y + m->r[0].z * z + m->r[0].w;
        o->y = m->r[1].x * x + m->r[1].y * y + m->r[1].z * z + m->r[1].w;
        o->z = m->r[2].x * x + m->r[2].y * y + m->r[2].z * z + m->r[2].w;
        o->u = src[k].u; o->v = src[k].v; o->s = src[k].s;
    }
}
static void bake_spines(void) {
    if (!g_spinev || !g_quadv) return;
    int n = 0;
    for (int c = 0; c < SPINE_COLOURS; c++) {
        g_spine_first[c] = n;
        for (int i = 0; i < g_nposters; i++) {
            Poster *p = &g_pos[i];
            if (!p->ok || !p->shown || p->faceout || p->col != c) continue;
            if (n + 6 > MAX_POSTERS * 6) break;
            bake_case(g_spinev, &n, &p->model, g_quadv);
        }
        g_spine_count[c] = n - g_spine_first[c];
    }
    /* The blank clamshell, for the faces that have NO art of their own.
     *
     * It used to be baked for every face and the real cover drawn 0.01 in front of it. A fixed
     * view-space nudge is not a fixed depth-buffer nudge -- the further off and the shallower
     * the angle, the smaller the gap becomes -- so at a sharp angle down a shelf the two
     * z-fought and the blank one won. That is the white. Anything with a cover is not drawn
     * twice any more, which fixes it and saves the vertices. */
    g_front_first = n;
    for (int i = 0; i < g_nposters; i++) {
        Poster *p = &g_pos[i];
        if (!p->ok || !p->shown || !p->faceout) continue;
        if (p->is_more) { if (g_restock_ok) continue; }
        else { int ti = p->copy_of >= 0 ? p->copy_of : i; if (g_pos[ti].tex_ok) continue; }
        if (n + 6 > MAX_POSTERS * 6) break;
        bake_case(g_spinev, &n, &p->model, g_quadv);
    }
    g_front_count = n - g_front_first;
}

static void build_quad(void) {
    g_quadv  = (Vtx *)linearAlloc(sizeof(Vtx) * 6);
    g_spinev = (Vtx *)linearAlloc(sizeof(Vtx) * MAX_POSTERS * 6);
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

/* Never submit an empty draw.
 *
 * This is what locked the console. Removing the wall stock set its vertex count to zero, but
 * the draw call itself stayed -- so every frame, in both eyes, the GPU was handed a
 * zero-vertex primitive. The PICA200 does not ignore that, it wedges, and HOME and START go
 * with it. It bit on the very first frame, which is why one panel update appeared and then
 * nothing. Every draw in this file goes through here now. */
static void draw_range(int first, int count) {
    if (count <= 0) return;
    C3D_DrawArrays(GPU_TRIANGLES, first, count);
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
    make_white_tex();
    make_front_tex();
    make_sign_tex2(&g_storesign, "CLOWNSEC VIDEO", "RENTALS", TH_BLUE, TH_YELLOW, TH_YELLOW);
    /* the cabinet is a wooden box until it says what it is */
    make_sign_tex_col(&g_jukesign, "MUSIC", TH_BLUE, TH_YELLOW, TH_YELLOW, 4);
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

    printf("\x1b[6;1H  MOFLEX STORE");
    prebuild_covers(&built);                /* the slow part, done where you are standing still */
    load_restock();                         /* BEFORE: the bake needs to know it has art */
    build_sections();                       /* genres -> units -> poster positions */
    for (int i = 0; i < BANNERS; i++) load_banner(i);
    make_wall_posters();                    /* decorate: unit ends and the bare walls */
    int roomn = build_room();               /* needs the unit positions */
    /* Music starts LAST, once nothing else wants the card.
     *
     * It used to start before the wall art was built, so the feeder thread was trying to read
     * a track while the main thread sat on the SD building sixteen poster caches -- and it
     * lost, every time, which is what the stuttering was. Nothing else reads the card after
     * this point, so from here it has the bus to itself. */
    int music_n = music_init(MUSIC_DIR);    /* quiet if the folder is empty or dsp is missing */
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
        g_frame++;
        u32 kd = hidKeysDown();
        if (kd & KEY_START) break;
        /* SELECT turns the face-out covers off. They are the only thing in this room that
         * touches the SD card while you walk, so the frame rate either jumps when they are off
         * -- the stutter is the cover pool -- or it does not, and it is draw volume. */
        if (kd & KEY_SELECT) g_covers_on = !g_covers_on;
        /* R skips a track from anywhere; the jukebox by the counter does the same if you walk
         * up to it and press A with no case highlighted. */
        int at_juke = 0;
        {   float jdx = cx - g_jukex, jdz = cz - g_jukez;
            at_juke = (music_n > 0) && (jdx * jdx + jdz * jdz < 2.6f * 2.6f); }
        if (music_n > 0 && ((kd & KEY_R) || (at_juke && sel < 0 && held < 0 && (kd & KEY_A))))
            music_next();
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
            bake_spines();                       /* the shelf changed: rebuild the batch */
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
                if (d > 2.0f || d < 1e-4f) continue;
                /* In FRONT of the bay, not merely on the right side of it. Out in the middle
                 * of the aisle you are about 2.2 from a face, so this asks you to step up to
                 * the shelf before anything locks on -- otherwise cases lit up as you walked
                 * past a whole run of them. */
                float fnx = sinf(g_pos[i].ay), fnz = cosf(g_pos[i].ay);
                if (-(fnx * dx + fnz * dz) / d < 0.55f) continue;
                float dot = (dx * ax + dy * ay + dz * az) / d;
                if (dot < bestscore) continue;
                bestscore = dot; sel = i;            /* the best-aimed case wins, not the nearest */
            }
        }

        float slider = osGet3DSliderState();
        /* Parallax at infinity is iod/focal, so a convergence plane 2.2 away in a room
         * twenty-five deep put the whole far end of the shop miles off the screen -- which is
         * what ghosts. Half the separation and the plane pushed out past the near bays: the
         * far wall settles down, and the case in your hand still stands off the screen. */
        float iod = slider * 0.14f;

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
                                0.05f, 60.0f, eye ? iod : -iod, 5.0f, false);
            C3D_RenderTargetClear(tgt, C3D_CLEAR_ALL, 0x101418FF, 0);
            C3D_FrameDrawOn(tgt);
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocProjection, &proj);
            /* the shell, three materials, three draws */
            set_buf(g_roomvbo, roomn);
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocModelview, &view);
            bind_tex(g_mat_ok ? &g_carpet : &g_room, 1);
            draw_range(0, g_n_floor);
            bind_tex(&g_room, 1);
            draw_range(g_n_floor, g_n_shell);
            bind_tex(g_mat_ok ? &g_wood : &g_room, 1);
            draw_range(g_n_floor + g_n_shell, g_n_units);
            if (g_covers_ok) {
                bind_tex(&g_covers, g_covers_ok);
                draw_range(g_n_floor + g_n_shell + g_n_units, g_n_cover);
            }
            if (g_n_light > 0) {
                bind_tex(&g_white, g_white_ok);
                draw_range(g_n_floor + g_n_shell + g_n_units + g_n_cover, g_n_light);
            }

            /* shopfront fittings: windows and a door on the near wall, signs above */
            set_buf(g_signvbo, 6);
            if (g_mat_ok) {
                /* The front wall is divided into four equal bays and each fitting is built to
                 * one of them. They used to be fixed at six units wide whatever the room, so
                 * once the shop shrank the windows overlapped each other and the door. */
                const int FRONT_BAYS = 4, DOOR_BAY = 2;
                float bw = (STORE_HX * 2.0f) / (float)FRONT_BAYS;
                for (int w = 0; w < FRONT_BAYS; w++) {
                    if (w == DOOR_BAY) continue;
                    int v = w % OUTSIDES;             /* a different stretch of street each time */
                    bind_tex(g_outside_ok[v] ? &g_outside[v] : &g_glass, 1);
                    float wx = -STORE_HX + bw * ((float)w + 0.5f);
                    C3D_Mtx m; Mtx_Copy(&m, &view);
                    Mtx_Translate(&m, wx, 1.95f, STORE_Z0 - 0.05f, true);
                    Mtx_Scale(&m, bw * 0.88f, 2.4f, 1.0f);
                    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocModelview, &m);
                    draw_range(0, 6);
                }
                float dx = -STORE_HX + bw * ((float)DOOR_BAY + 0.5f);
                float dw = bw * 0.55f; if (dw > 2.4f) dw = 2.4f;
                bind_tex(&g_door, g_mat_ok);
                { C3D_Mtx m; Mtx_Copy(&m, &view);
                  Mtx_Translate(&m, dx, 1.15f, STORE_Z0 - 0.05f, true);
                  Mtx_Scale(&m, dw, 2.3f, 1.0f);
                  C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocModelview, &m);
                  draw_range(0, 6); }
                g_doorx = dx;                              /* the exit board hangs over it */
            }
            if (g_store_ok) {                          /* MUSIC across the front of the cabinet */
                bind_tex(&g_jukesign, 1);
                C3D_Mtx m; Mtx_Copy(&m, &view);
                Mtx_Translate(&m, g_jukex + sinf(g_jukerot) * 0.41f, 1.12f,
                              g_jukez + cosf(g_jukerot) * 0.41f, true);
                Mtx_RotateY(&m, g_jukerot, true);
                Mtx_Scale(&m, 0.86f, 0.86f * (float)SIGN_H / (float)SIGN_W, 1.0f);
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocModelview, &m);
                draw_range(0, 6);
            }
            if (g_store_ok) {                              /* name across the back wall */
                bind_tex(&g_storesign, g_store_ok);
                C3D_Mtx m; Mtx_Copy(&m, &view);
                Mtx_Translate(&m, 0.0f, 3.10f, STORE_Z0 - STORE_DEPTH + 0.06f, true);
                /* Sized to its type, not to the wall -- and capped so the top of the board
                 * stays under the ceiling. Once the bays were sized properly the room grew,
                 * the board grew with it, and it went straight through the roof. */
                float nw = STORE_HX * 0.55f;
                if (nw > 7.0f) nw = 7.0f;
                Mtx_Scale(&m, nw, nw * (float)SIGN2_H / (float)SIGN2_W, 1.0f);
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocModelview, &m);
                draw_range(0, 6);
            }
            if (g_exit_ok) {                               /* over the door */
                bind_tex(&g_exitsign, g_exit_ok);
                C3D_Mtx m; Mtx_Copy(&m, &view);
                Mtx_Translate(&m, g_doorx, 2.75f, STORE_Z0 - 0.10f, true);
                Mtx_RotateY(&m, C3D_Angle(0.5f), true);
                Mtx_Scale(&m, 1.8f, 1.8f * (float)SIGN_H / (float)SIGN_W, 1.0f);
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocModelview, &m);
                draw_range(0, 6);
            }

            /* The shelves: every case is the SAME spine mesh with a different tint. No cover
             * textures at all, which is what lets a whole catalogue stand on these shelves --
             * and a ten-pixel spine could not show a title anyway. The selected one turns
             * face-on below and shows the real cover, because only ever one is selected. */
            /* Every spine in the shop, in three draws. They are baked in world space, so the
             * model matrix is just the view -- no per-case matrix, no per-case draw. */
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocModelview, &view);
            set_buf(g_spinev, MAX_POSTERS * 6);
            int drawn = 0;
            for (int c = 0; c < SPINE_COLOURS; c++) {
                if (g_spine_count[c] <= 0) continue;
                bind_tex(&g_spine[c], g_spine_ok);
                draw_range(g_spine_first[c], g_spine_count[c]);
                drawn += g_spine_count[c] / 6;
            }

            /* Every face-out case, blank, in one draw. The near ones are redrawn just proud
             * of these with their real cover on. */
            if (g_front_count > 0) {
                bind_tex(&g_front, g_front_ok);
                draw_range(g_front_first, g_front_count);
                drawn += g_front_count / 6;
            }

            /* the ones close enough to read: their actual art, a hair in front */
            set_buf(g_quadvbo, 6);
            /* vis: every face-out case in view, held or not -- what the pool must not evict.
             * need: the ones still without a slot. */
            for (int i = 0; i < g_nposters; i++) {
                if (!g_pos[i].ok || !g_pos[i].shown || !g_pos[i].faceout) continue;
                if (i == held || i == sel) continue;   /* held and selected are drawn separately */
                int more = g_pos[i].is_more;
                if (!g_covers_on) continue;
                int ti = g_pos[i].copy_of >= 0 ? g_pos[i].copy_of : i;   /* a copy shares its art */
                if (!(more ? g_restock_ok : g_pos[ti].tex_ok)) continue;
                float dxs = g_pos[i].x - cx, dzs = g_pos[i].z - cz;
                float d2 = dxs * dxs + dzs * dzs;
                if (d2 > COVER_VIEW * COVER_VIEW) continue;
                /* Behind the camera only. There WAS a second test here that skipped a case
                 * whose face was turned away, and it is what made a shelf go white when you
                 * looked along it from a sharp angle: the blank clamshell underneath comes
                 * from one batched buffer with no such test, so at the angle where the two
                 * disagreed the cover vanished and the blank one stayed. A cull may only
                 * remove what nothing else is drawing. */
                if (dxs * fwx + dzs * fwz < -0.5f) continue;
                C3D_Mtx m;
                Mtx_Multiply(&m, &view, &g_pos[i].model);
                /* A hair toward the camera so it sits on top of the blank one. Nudged in VIEW
                 * space -- after the multiply the translation is already in the camera's
                 * frame, so a world-space offset here would push it sideways instead. */
                m.r[2].w += 0.004f;   /* nothing to fight with now; just off the woodwork */
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocModelview, &m);
                bind_tex(more ? &g_restock : &g_pos[ti].tex, more ? g_restock_ok : g_pos[ti].tex_ok);
                draw_range(0, 6);
                drawn++;
            }
            if (eye == 0) g_drawn = drawn;
            /* The highlighted case: eased forward a few millimetres and a touch larger, and
             * that is all. It used to come 22 cm off the shelf and swing square to the camera,
             * which reads as picking it up -- and picking it up is a button, not a glance. A
             * highlight only has to say WHICH one, so it stays in the plane of the shelf. */
            if (sel >= 0 && sel != held && g_pos[sel].ok) {
                Poster *q = &g_pos[sel];
                float nx = sinf(q->ay), nz = cosf(q->ay);     /* the way this case faces */
                C3D_Mtx m;
                Mtx_Copy(&m, &view);
                Mtx_Translate(&m, q->x + nx * 0.035f, q->y, q->z + nz * 0.035f, true);
                Mtx_RotateY(&m, q->ay, true);                /* stays square to the SHELF */
                Mtx_Scale(&m, CASE_W * 1.10f,
                          CASE_W * 1.10f * (float)IMG_H / (float)IMG_W, 1.0f);
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocModelview, &m);
                /* Its pool cover, which is already in hand -- NOT a fresh detail texture.
                 * Sweeping the stick changes the selection almost every frame, and loading the
                 * big sheet meant an SD open per frame while aiming. The full one is for the
                 * case in your hand, where a single hitch on a deliberate button press is fine. */
                int st = g_pos[sel].copy_of >= 0 ? g_pos[sel].copy_of : sel;
                bind_tex(g_pos[sel].is_more ? (g_restock_ok ? &g_restock : &g_front)
                         : (g_detail_ok && g_detail_for == sel) ? &g_detail
                         : (g_pos[st].tex_ok ? &g_pos[st].tex : &g_front), 1);
                draw_range(0, 6);
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
                bind_tex((g_detail_ok && g_detail_for == held) ? &g_detail : &g_spine[g_pos[held].col], 1);
                draw_range(0, 6);
                if (g_back_ok) {
                    bind_tex(&g_back, g_back_ok);                /* back: the printed card */
                    draw_range(6, 6);
                }
                bind_tex(&g_room, 1);                    /* the four edges */
                draw_range(12, 24);
                set_buf(g_quadvbo, 6);
            }

            /* framed posters: an end cap on each unit, and a few around the walls */
            if (g_wall_n > 0) {
                /* Every frame takes the NEXT unused poster and stops when they run out, rather
                 * than wrapping -- wrapping is why the same art turned up on a bay end and on
                 * the wall beside it. */
                /* Two lists, because they are not interchangeable. Art someone went and made
                 * belongs on a wall where it is looked at; a cover borrowed off a shelf is
                 * filler and belongs on the end of a bay. Mixing them is how a poster ended up
                 * on a bay end and again on the wall beside it. */
                int wlU[WALLPOSTERS], wnU = 0, wlB[WALLPOSTERS], wnB = 0;
                for (int i = 0; i < WALLPOSTERS; i++) {
                    if (!g_wall_ok[i]) continue;
                    if (g_wall_user[i]) wlU[wnU++] = i; else wlB[wnB++] = i;
                }
                int wu = 0, wb = 0;
                /* the POSTER quad, not the sign quad: a cover fills only the top IMG_H of its
                 * texture box, so a full 0..1 quad shows it squashed up top over a black band */
                set_buf(g_quadvbo, 6);
                for (int i = 0; i < g_nsec; i++) {
                    if (g_sec[i].hide) continue;
                    for (int e = 0; e < 2; e++) {
                        /* the inner end of a bay with an L return is up against the return --
                         * a poster there is half-buried by it */
                        int inner = (g_sec[i].cx < 0) ? 1 : 0;
                        if (g_sec[i].has_L && e == inner) continue;
                        int k;                                   /* bay ends: borrowed first */
                        if (wb < wnB)      k = wlB[wb++];
                        else if (wu < wnU) k = wlU[wu++];
                        else continue;
                        bind_tex(&g_wall[k], g_wall_ok[k]);
                        C3D_Mtx m; Mtx_Copy(&m, &view);
                        Mtx_Translate(&m, g_sec[i].cx + (e ? 1 : -1) * (g_sec[i].len * 0.5f + 0.03f),
                                      1.30f, g_sec[i].cz, true);
                        Mtx_RotateY(&m, e ? C3D_Angle(0.25f) : C3D_Angle(-0.25f), true);
                        Mtx_Scale(&m, 0.62f, 0.62f * (float)IMG_H / (float)IMG_W, 1.0f);
                        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocModelview, &m);
                        draw_range(0, 6);
                    }
                }
                /* On the side walls at eye height, in the GAPS between the rows of bays. Hung
                 * up near the ceiling they read as a border rather than as stock, and at their
                 * old height the bays covered the wall anyway. g_gapz carries the midpoints,
                 * so these follow the layout when it changes.
                 *
                 * The back wall carries two, low and wide apart: four of them at 2.70 sat
                 * straight over the shop name. */
                const float WP[6][4] = {          /* x, y, z, facing (radians about y) */
                    { -STORE_HX + 0.08f, 1.62f, g_gapz[0],  1.5708f },
                    { -STORE_HX + 0.08f, 1.62f, g_gapz[1],  1.5708f },
                    {  STORE_HX - 0.08f, 1.62f, g_gapz[0], -1.5708f },
                    {  STORE_HX - 0.08f, 1.62f, g_gapz[1], -1.5708f },
                    /* Flanking the name, and high enough to clear the bays standing in front
                     * of the back wall -- at 0.82 of the half-width they were behind the back
                     * run on one side and the staff door on the other. */
                    { -5.2f, 2.75f, STORE_Z0 - STORE_DEPTH + 0.08f, 0.0f },
                    {  5.2f, 2.75f, STORE_Z0 - STORE_DEPTH + 0.08f, 0.0f },
                };
                for (int i = 0; i < 6; i++) {
                    int k;                                       /* walls: the supplied art */
                    if (wu < wnU)      k = wlU[wu++];
                    else if (wb < wnB) k = wlB[wb++];
                    else continue;
                    bind_tex(&g_wall[k], g_wall_ok[k]);
                    C3D_Mtx m; Mtx_Copy(&m, &view);
                    Mtx_Translate(&m, WP[i][0], WP[i][1], WP[i][2], true);
                    Mtx_RotateY(&m, WP[i][3], true);
                    Mtx_Scale(&m, 1.05f, 1.05f * (float)IMG_H / (float)IMG_W, 1.0f);
                    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocModelview, &m);
                    draw_range(0, 6);
                }
                /* the wide ones: back wall below the name, and over the door on the way out */
                {
                    /* One low across the back, well under the name -- it used to run up into
                     * it. One high on a side wall, above the bays where nothing else goes; it
                     * was over the shopfront before, straight across the door and windows. */
                    const float BN[BANNERS][5] = {   /* x, y, z, facing, half-width */
                        { 0.0f,             1.18f, STORE_Z0 - STORE_DEPTH + 0.08f, 0.0f,    4.3f },
                        /* higher up the wall: at 2.90 its bottom edge sat on the top of the
                         * portrait poster hanging in the same gap */
                        { -STORE_HX + 0.10f, 3.25f, g_gapz[0],                     1.5708f, 3.4f },
                    };
                    int any = 0;
                    for (int i = 0; i < BANNERS; i++) if (g_banner_ok[i]) any = 1;
                    if (any) set_buf(g_signvbo, 6);  /* a banner fills its texture: full UVs */
                    for (int i = 0; i < BANNERS; i++) {
                        if (!g_banner_ok[i]) continue;
                        bind_tex(&g_banner[i], g_banner_ok[i]);
                        float bw = BN[i][4];
                        if (bw > STORE_HX * 0.55f) bw = STORE_HX * 0.55f;
                        C3D_Mtx m; Mtx_Copy(&m, &view);
                        Mtx_Translate(&m, BN[i][0], BN[i][1], BN[i][2], true);
                        Mtx_RotateY(&m, BN[i][3], true);
                        Mtx_Scale(&m, bw, bw * (float)BAN_H / (float)BAN_W, 1.0f);
                        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocModelview, &m);
                        draw_range(0, 6);
                    }
                }
            }

            /* section signs, hung over each unit. Two quads back to back so the name reads the
             * right way round from both sides -- one quad with culling off shows its text
             * mirrored from behind. */
            set_buf(g_signvbo, 6);
            for (int i = 0; i < g_nsec; i++) {
                if (g_sec[i].hide || !g_sec[i].sign_ok) continue;
                bind_tex(&g_sec[i].sign, g_sec[i].sign_ok);
                for (int f = 0; f < 2; f++) {
                    C3D_Mtx m;
                    Mtx_Copy(&m, &view);
                    Mtx_Translate(&m, g_sec[i].cx, SIGN_Y, g_sec[i].cz + (f ? -0.03f : 0.03f), true);
                    if (f) Mtx_RotateY(&m, C3D_Angle(0.5f), true);
                    /* as wide as its bay, never wider: a fixed 3.4 hung past the end of a
                     * short unit and straight through the wall behind it */
                    float sw = g_sec[i].len * 0.85f;
                    if (sw > 3.4f) sw = 3.4f;
                    if (sw < 1.6f) sw = 1.6f;
                    Mtx_Scale(&m, sw, sw * (float)SIGN_H / (float)SIGN_W, 1.0f);
                    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocModelview, &m);
                    draw_range(0, 6);
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
                panel_set(4, " Take this one and press the button to");
                panel_set(5, " restock the shelf with the next lot.");
            } else if (q->desc[0]) panel_wrap(4, 19, q->desc);
            else            panel_set(4, q->hasinfo ? " (no description in the .nfo)"
                                                    : " (no .nfo for this one - poster only)");
            if (held >= 0) panel_fmt(24, " in hand");
        } else {
            panel_set(0, " MOFLEX STORE  (prototype)");
            panel_fmt(2, " %d cases, %d with info%s", g_nposters, g_withinfo,
                      g_scan_capped ? "   CAPPED" : "");
            panel_fmt(3, " moviedata %d   art %d", g_from_data, g_from_art);
            panel_fmt(4, " built %d   load %llums", built, (unsigned long long)t_load);
            if (music_n > 0) panel_fmt(19, " playing  %.30s", music_now());
            panel_fmt(21, " covers %d/%d  %uKB linear", g_cov_n, g_nposters,
                      (unsigned)((size_t)g_cov_n * COVER_BYTES / 1024));
            panel_fmt(22, " meta %uKB (%u B each)",
                      (unsigned)((size_t)g_nposters * sizeof(Poster) / 1024),
                      (unsigned)sizeof(Poster));
            panel_fmt(23, " linear free %uKB", (unsigned)(linearSpaceFree() / 1024));
            panel_fmt(5, " fps %2d   eyes %d", fps, (slider > 0.0f ? 2 : 1));
            panel_set(6, " walk up to a case for its info");
            panel_set(8, " sections");
            for (int i = 0; i < g_nsec && i < 8; i++)
                panel_fmt(9 + i, "   %-16s %d", g_sec[i].name, g_sec[i].n);
        }
        if (held >= 0) {
            if (music_n > 0) panel_set(23, at_juke ? " A: next track   R: next track"
                                                    : " R: next track");
            panel_fmt(25, " fps %2d  drawn %d  covers %s", fps, g_drawn, g_covers_on ? "on" : "OFF");
            panel_set(26, " pad turn/zoom   d-pad next");
            panel_fmt(27, " %s: Y    put back: B",
                      g_pos[held].is_more ? "MORE" : verb());
        } else {
            panel_fmt(25, " fps %2d  drawn %d  covers %s", fps, g_drawn, g_covers_on ? "on" : "OFF");
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
    for (int v = 0; v < OUTSIDES; v++) if (g_outside_ok[v]) C3D_TexDelete(&g_outside[v]);
    if (g_store_ok) { C3D_TexDelete(&g_storesign); C3D_TexDelete(&g_jukesign); }
    if (g_exit_ok)  C3D_TexDelete(&g_exitsign);
    if (g_covers_ok) C3D_TexDelete(&g_covers);
    if (g_spine_ok) for (int i = 0; i < SPINE_COLOURS; i++) C3D_TexDelete(&g_spine[i]);
    if (g_white_ok)  C3D_TexDelete(&g_white);
    if (g_front_ok)  C3D_TexDelete(&g_front);
    music_exit();
    for (int i = 0; i < g_nposters; i++) if (g_pos[i].tex_ok) C3D_TexDelete(&g_pos[i].tex);
    if (g_restock_ok) C3D_TexDelete(&g_restock);
    for (int i = 0; i < BANNERS; i++) if (g_banner_ok[i]) C3D_TexDelete(&g_banner[i]);
    for (int i = 0; i < WALLPOSTERS; i++) if (g_wall_ok[i]) C3D_TexDelete(&g_wall[i]);
    if (g_back_ok) C3D_TexDelete(&g_back);
    if (g_detail_ok) C3D_TexDelete(&g_detail);
    shaderProgramFree(&program);
    DVLB_Free(vsh_dvlb);
    C3D_Fini();
    gfxExit();
    return 0;
}
