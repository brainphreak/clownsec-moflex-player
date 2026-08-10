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

/* ---------------- poster texture geometry ----------------
 * 128x256 is the smallest power-of-two box that holds a poster at a sane size; the image
 * occupies 128x182 of it, which is the source 132x188 aspect to within half a percent. */
#define TEX_W   128
#define TEX_H   256
#define IMG_W   128
#define IMG_H   182
#define TEX_BYTES (TEX_W * TEX_H * 2)
#define VMAX    ((float)IMG_H / (float)TEX_H)

#define ART_DIR   "sdmc:/moflex_player/art"
#define CACHE_DIR "sdmc:/moflex_player/store"

#define MAX_POSTERS 48          /* one aisle; the real thing would stream */
#define ROOM_TEX 64

typedef struct { float x, y, z, u, v, s; } Vtx;

typedef struct {
    C3D_Tex tex;
    int     ok;
    float   x, y, z;            /* centre, world space */
    float   ry;                 /* facing: +1 = normal points +x, -1 = -x */
    char    name[64];
} Poster;

static Poster g_pos[MAX_POSTERS];
static int    g_nposters = 0;

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

/* pretty name from "Some_Movie_2011_132x188.p565" */
static void pretty(const char *fn, char *out, size_t cap) {
    char t[128];
    snprintf(t, sizeof t, "%s", fn);
    char *u = strrchr(t, '_');                     /* drop the _WxH */
    if (u) *u = 0;
    for (char *p = t; *p; p++) if (*p == '_') *p = ' ';
    snprintf(out, cap, "%s", t);
}

static int load_posters(int *built) {
    mkdir(CACHE_DIR, 0777);
    DIR *d = opendir(ART_DIR);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) && g_nposters < MAX_POSTERS) {
        size_t L = strlen(e->d_name);
        if (L < 6 || strcmp(e->d_name + L - 5, ".p565")) continue;
        int sw = 0, sh = 0;
        const char *u = strrchr(e->d_name, '_');
        if (!u || sscanf(u + 1, "%dx%d.p565", &sw, &sh) != 2) continue;
        if (sw <= 0 || sh <= 0 || sw > 1024 || sh > 1024) continue;

        char key[128];
        snprintf(key, sizeof key, "%.*s", (int)(L - 5), e->d_name);
        char cache[256], art[256];
        snprintf(cache, sizeof cache, "%s/%s.t565", CACHE_DIR, key);
        snprintf(art,   sizeof art,   "%s/%s", ART_DIR, e->d_name);

        FILE *cf = fopen(cache, "rb");
        if (!cf) {
            if (!build_cache_entry(art, sw, sh, cache)) continue;
            (*built)++;
            cf = fopen(cache, "rb");
            if (!cf) continue;
        }
        Poster *p = &g_pos[g_nposters];
        if (!C3D_TexInit(&p->tex, TEX_W, TEX_H, GPU_RGB565)) { fclose(cf); continue; }
        /* straight into texture memory -- this is the whole reason the cache is pre-tiled */
        size_t got = fread(p->tex.data, 1, TEX_BYTES, cf);
        fclose(cf);
        if (got != TEX_BYTES) { C3D_TexDelete(&p->tex); continue; }
        C3D_TexFlush(&p->tex);
        C3D_TexSetFilter(&p->tex, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&p->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        p->ok = 1;
        pretty(e->d_name, p->name, sizeof p->name);
        g_nposters++;
    }
    closedir(d);
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

/* ---------------- geometry ---------------- */
#define AISLE_HALF   2.0f       /* wall at +/- this in x */
#define AISLE_LEN    24.0f
#define ROOM_VTX     24
static Vtx *g_roomv, *g_quadv;
static void *g_roomvbo, *g_quadvbo;

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
    g_roomv = (Vtx *)linearAlloc(sizeof(Vtx) * ROOM_VTX * 2);
    int n = 0;
    const float L = AISLE_LEN, H = 3.0f, W = AISLE_HALF;
    /* floor */
    push_quad(g_roomv, &n, -W, 0, 0,  W, 0, 0,  W, 0, -L, -W, 0, -L,  4, L / 2, 0.50f);
    /* ceiling (darker: nothing up there deserves attention) */
    push_quad(g_roomv, &n, -W, H, -L, W, H, -L, W, H, 0,  -W, H, 0,   4, L / 2, 0.30f);
    /* left wall, right wall */
    push_quad(g_roomv, &n, -W, 0, -L, -W, 0, 0,  -W, H, 0,  -W, H, -L, L / 2, 2, 0.62f);
    push_quad(g_roomv, &n,  W, 0, 0,   W, 0, -L,  W, H, -L,  W, H, 0,  L / 2, 2, 0.62f);
    return n;
}

/* unit quad in the XY plane, reused for every poster */
static void build_quad(void) {
    g_quadv = (Vtx *)linearAlloc(sizeof(Vtx) * 6);
    int n = 0;
    push_quad(g_quadv, &n, -0.5f, -0.5f, 0,  0.5f, -0.5f, 0,
                            0.5f,  0.5f, 0, -0.5f,  0.5f, 0,  1.0f, VMAX, 1.0f);
    /* v is flipped below by the model matrix; texture rows start at the poster's top */
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
    int roomn = build_room();
    build_quad();
    g_roomvbo = g_roomv; g_quadvbo = g_quadv;

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

    /* lay the posters along both walls, two rows */
    for (int i = 0; i < g_nposters; i++) {
        int side = i & 1;                                  /* alternate walls */
        int idx  = i >> 1;
        int row  = idx & 1;
        int col  = idx >> 1;
        g_pos[i].x  = side ? (AISLE_HALF - 0.02f) : -(AISLE_HALF - 0.02f);
        g_pos[i].y  = row ? 1.95f : 1.05f;
        g_pos[i].z  = -1.5f - col * 1.15f;
        g_pos[i].ry = side ? -1.0f : 1.0f;
    }

    float cx = 0, cz = -0.5f, yaw = 0;
    const float EYE = 1.55f;
    int frames = 0, fps = 0; u64 t0 = osGetTime();
    int sel = -1;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kd = hidKeysDown();
        if (kd & KEY_START) break;

        circlePosition cp; hidCircleRead(&cp);
        float fx = cp.dx / 156.0f, fy = cp.dy / 156.0f;
        if (fabsf(fx) < 0.15f) fx = 0;
        if (fabsf(fy) < 0.15f) fy = 0;
        yaw -= fx * 0.045f;                                  /* turn, not strafe: gentler in stereo */
        cx  += sinf(yaw) * fy * 0.09f;
        cz  -= cosf(yaw) * fy * 0.09f;
        if (cx >  AISLE_HALF - 0.45f) cx =  AISLE_HALF - 0.45f;
        if (cx < -AISLE_HALF + 0.45f) cx = -AISLE_HALF + 0.45f;
        if (cz >  -0.3f)        cz = -0.3f;
        if (cz < -AISLE_LEN + 1.0f) cz = -AISLE_LEN + 1.0f;

        /* what am I looking at? nearest poster ahead, within reach */
        sel = -1; float best = 3.2f;
        float vdx = -sinf(yaw), vdz = -cosf(yaw);
        for (int i = 0; i < g_nposters; i++) {
            float dx = g_pos[i].x - cx, dz = g_pos[i].z - cz;
            float d = sqrtf(dx * dx + dz * dz);
            if (d > best) continue;
            float dy = g_pos[i].y - EYE;
            if (fabsf(dy) > 1.1f) continue;
            if ((dx * vdx + dz * vdz) / (d + 1e-4f) < 0.55f) continue;   /* must be in front */
            best = d; sel = i;
        }

        float slider = osGet3DSliderState();
        float iod = slider * 0.28f;                 /* gentle: an aisle already has lots of depth */

        C3D_Mtx view;
        Mtx_Identity(&view);
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

            /* posters: one draw each, unit quad + per-poster matrix */
            set_buf(g_quadvbo, 6);
            for (int i = 0; i < g_nposters; i++) {
                if (!g_pos[i].ok) continue;
                float pop = (i == sel) ? 0.10f : 0.0f;     /* the highlighted one steps out */
                C3D_Mtx m;
                Mtx_Copy(&m, &view);
                Mtx_Translate(&m, g_pos[i].x + g_pos[i].ry * pop, g_pos[i].y, g_pos[i].z, true);
                Mtx_RotateY(&m, g_pos[i].ry > 0 ? C3D_Angle(0.25f) : C3D_Angle(-0.25f), true);
                Mtx_Scale(&m, 0.62f, 0.62f * (float)IMG_H / (float)IMG_W, 1.0f);
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocModelview, &m);
                C3D_TexBind(0, &g_pos[i].tex);
                C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
            }
        }
        C3D_FrameEnd(0);

        frames++;
        if (osGetTime() - t0 >= 1000) { fps = frames; frames = 0; t0 = osGetTime(); }

        printf("\x1b[0;0H\x1b[2K  MOFLEX STORE  (prototype)");
        printf("\x1b[2;0H\x1b[2K  posters %d  (%d from art, %d placeholder)", g_nposters,
               g_nposters - placeheld, placeheld);
        printf("\x1b[3;0H\x1b[2K  cache built this run: %d   load %llums", built,
               (unsigned long long)t_load);
        printf("\x1b[4;0H\x1b[2K  VRAM tex: %d KB", (int)((g_nposters * TEX_BYTES) / 1024));
        printf("\x1b[6;0H\x1b[2K  fps %2d   eyes %d   slider %.2f", fps,
               (slider > 0.0f ? 2 : 1), slider);
        printf("\x1b[8;0H\x1b[2K  %s", sel >= 0 ? g_pos[sel].name : "(nothing in reach)");
        printf("\x1b[12;0H\x1b[2K  circle pad: walk / turn");
        printf("\x1b[13;0H\x1b[2K  3D slider : depth      START: exit");
        if ((kd & KEY_A) && sel >= 0)
            printf("\x1b[10;0H\x1b[2K  >> selected: %s", g_pos[sel].name);
    }

    for (int i = 0; i < g_nposters; i++) if (g_pos[i].ok) C3D_TexDelete(&g_pos[i].tex);
    C3D_TexDelete(&g_room);
    shaderProgramFree(&program);
    DVLB_Free(vsh_dvlb);
    C3D_Fini();
    gfxExit();
    return 0;
}
