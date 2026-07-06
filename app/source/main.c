/* MoFlex 3D Player — standalone app.
 * MAIN: browse SD + play movies.  Y: ADD MOVIES (download/upload).  X: MANAGE (move/delete).
 * Top screen shows 3D video during playback; bottom is the UI console. */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <malloc.h>
#include <ctype.h>

#include "moflex_playback.h"
#include "httpd.h"
#include "downloader.h"
#include "catalog.h"
#include "poster.h"
#include "unzip.h"
#include "cia_moflex.h"
#include "ui_gfx.h"
#include "branding.h"

#define APP_VERSION "v1.0.0"
#define MAXE     1024
#define NAMELEN  256
#define PATHLEN  1024
#define ROWS     23
#define NAV_UP   (KEY_UP | KEY_CPAD_UP)
#define NAV_DOWN (KEY_DOWN | KEY_CPAD_DOWN)

/* held-key auto-repeat with acceleration. *hf = held-frame counter.
   Returns steps to move this frame (0/1) for the given held direction. */
static int nav_repeat(u32 kdown, u32 kheld, u32 mask, int *hf) {
    if (!(kheld & mask)) { *hf = 0; return 0; }
    if (kdown & mask) { *hf = 0; return 1; }        /* initial press */
    (*hf)++;
    if (*hf <= 16) return 0;                          /* ~0.27s delay before repeat */
    int s = *hf - 16;
    int interval = s > 75 ? 1 : (s > 30 ? 2 : 5);     /* accelerate the longer it's held */
    return (s % interval == 0) ? 1 : 0;
}
#define CATURL_PATH "sdmc:/moflex_player/catalog.txt"

typedef struct { char name[NAMELEN]; int is_dir; } Entry;

static Entry entries[MAXE];
static int   nentries;
static char  cwd[PATHLEN] = "sdmc:/";
static char  g_now_playing[NAMELEN] = "";   /* last-played movie name (ext hidden, for GUI title) */
static char  g_now_playing_path[PATHLEN + NAMELEN] = "";   /* full path, for resume from home */
static int   sel = 0, scroll = 0;

/* move (cut/paste) clipboard */
static int  move_pending = 0;
static char move_src[PATHLEN];
static char move_name[NAMELEN];

/* ---------- filesystem browser ---------- */

static int is_moflex(const char *n) {
    size_t L = strlen(n);
    /* playable = a plain .moflex OR a .cia with an embedded moflex (played in place) */
    return (L > 7 && strcasecmp(n + L - 7, ".moflex") == 0)
        || (L > 4 && strcasecmp(n + L - 4, ".cia") == 0);
}
static int cmp_entry(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
    return strcasecmp(x->name, y->name);
}
static int s_filter_movies = 1;        /* when set, only show playable movies (moflex + movie CIAs) */
static int s_manage_movies_only = 0;   /* manage-mode toggle (Y): all files vs movies only */
static void scan(void) {
    nentries = 0;
    DIR *d = opendir(cwd);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && nentries < MAXE) {
            if (e->d_name[0] == '.') continue;
            char full[PATHLEN + NAMELEN];
            snprintf(full, sizeof(full), "%s%s", cwd, e->d_name);
            struct stat st;
            int isdir = (stat(full, &st) == 0) && S_ISDIR(st.st_mode);
            if (!isdir && s_filter_movies) {                 /* movies-only view (play, or manage toggle) */
                if (!is_moflex(e->d_name)) continue;          /* only .moflex / .cia */
                if (cia_is_cia(e->d_name) && !cia_has_moflex(full)) continue;   /* only movie CIAs */
            }
            /* else (manage + show-all): list every file so it works as a general download/file tool */
            snprintf(entries[nentries].name, NAMELEN, "%s", e->d_name);
            entries[nentries].is_dir = isdir;
            nentries++;
        }
        closedir(d);
    }
    qsort(entries, nentries, sizeof(Entry), cmp_entry);
    if (sel >= nentries) sel = nentries ? nentries - 1 : 0;
    scroll = 0;
}
static int at_root(void) { return strcmp(cwd, "sdmc:/") == 0; }
static void go_up(void) {
    if (at_root()) return;
    size_t L = strlen(cwd);
    if (L && cwd[L-1] == '/') cwd[--L] = 0;
    char *s = strrchr(cwd, '/');
    if (s) s[1] = 0;
    scan();
}
static void enter_dir(const char *name) {
    size_t L = strlen(cwd);
    snprintf(cwd + L, PATHLEN - L, "%s/", name);
    scan();
}

enum { MODE_PLAY = 0, MODE_MANAGE = 1 };

static void draw_browser(int mode) {
    /* Redraw in place: home cursor, erase each line to EOL as we overwrite it (\x1b[K), and clear any
     * leftover lines at the end (\x1b[J). Avoids the \x1b[2J full-clear flash that tore during scrolling. */
    printf("\x1b[H");
    printf("=== %s ===\x1b[K\n", mode == MODE_MANAGE ? "MANAGE" : "OPEN");
    printf("%.38s\x1b[K\n", cwd);
    if (move_pending) printf("[MOVE] %.24s  R:paste\x1b[K\n", move_name);
    else if (mode == MODE_MANAGE) printf("A:open X:manage Y:%s B:up\x1b[K\n", s_manage_movies_only ? "show all" : "movies only");
    else printf("A:play/open  B:up/home  X:manage\x1b[K\n");
    printf("START:exit  SELECT:refresh\x1b[K\n\x1b[K\n");
    if (nentries == 0) { printf("(empty - no folders or .moflex)\x1b[K\n\x1b[J"); return; }
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + ROWS) scroll = sel - ROWS + 1;
    for (int i = scroll; i < nentries && i < scroll + ROWS; i++) {
        char disp[NAMELEN];
        snprintf(disp, sizeof(disp), "%s", entries[i].name);
        if (!entries[i].is_dir && mode != MODE_MANAGE) {   /* play mode: clean movie titles (no ext);
                                                            * manage mode: show real filenames + ext */
            size_t L = strlen(disp);
            if (L > 7 && !strcasecmp(disp + L - 7, ".moflex")) disp[L - 7] = 0;
            else if (L > 4 && !strcasecmp(disp + L - 4, ".zip")) disp[L - 4] = 0;
            else if (L > 4 && !strcasecmp(disp + L - 4, ".cia")) disp[L - 4] = 0;
        }
        printf("%c%s%.35s\x1b[K\n", i == sel ? '>' : ' ', entries[i].is_dir ? "[" : " ", disp);
    }
    printf("\x1b[J");   /* clear any lines left over from a previously longer view */
}

/* ---------- small modal helpers ---------- */

static int confirm(const char *prompt) {
    printf("\x1b[2J\x1b[H\n%s\n\nA: yes    B: no\n", prompt);
    while (aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown();
        if (k & KEY_A) return 1;
        if (k & KEY_B) return 0;
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    }
    return 0;
}

/* ---------- UPLOAD (web server, on-demand only) ---------- */

static void upload_screen(void) {
    int ok = httpd_start();
    int drawn = 0;
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_B) break;
        if (!drawn) {
            printf("\x1b[2J\x1b[H=== UPLOAD (web server) ===\n\n");
            if (ok) {
                printf("Server ON. On a computer or phone on\nthe same Wi-Fi, open:\n\n  %s\n\n", httpd_url());
                printf("Upload any files from the page.\nBrowse folders + delete there too.\n\n");
            } else {
                printf("Could not start server.\nIs Wi-Fi connected?\n\n");
            }
            printf("B: stop server + back\n");
            drawn = 1;
        }
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    }
    if (ok) httpd_stop();
}

/* ---------- DOWNLOAD (unistore catalog) ---------- */

static char g_dl_name[NAMELEN];
static u64  g_last_prog;

static bool dl_progress(void *u, u32 d, u32 t) {
    (void)u;
    hidScanInput();
    if (hidKeysDown() & KEY_B) return false;   /* cancel */
    u64 now = osGetTime();
    if (now - g_last_prog >= 200) {
        g_last_prog = now;
        printf("\x1b[2J\x1b[H=== DOWNLOAD ===\n\n%.36s\n\n", g_dl_name);
        if (t) printf("%lu / %lu KB  (%d%%)\n", (unsigned long)(d/1024),
                      (unsigned long)(t/1024), (int)((u64)d * 100 / t));
        else   printf("%lu KB\n", (unsigned long)(d/1024));
        printf("\nB: cancel\n");
        gfxFlushBuffers(); gfxSwapBuffers();
    }
    return true;
}

/* ---- unistore sources: 2 preloaded + user-added (sdmc:/moflex_player/sources.txt) ---- */
#define SRC_MAX 64
#define SOURCES_FILE "sdmc:/moflex_player/sources.txt"
typedef struct {
    char name[64];
    char url[CAT_URLLEN];      /* catalog.json URL */
    char dl_base[128];         /* download base ("" => entry carries its own URL) */
    char art_base[128];        /* artwork base */
    int  kind;                 /* 0 = clownsec shape, 1 = zackk shape */
} Source;
static Source sources[SRC_MAX];
static int    nsources;

static void ensure_cfg_dir(void) { mkdir("sdmc:/moflex_player", 0777); }

static void load_sources(void) {
    memset(sources, 0, sizeof(sources));
    nsources = 0;
    snprintf(sources[0].name, 64, "Clownsec 3D Archive");
    snprintf(sources[0].url, CAT_URLLEN, "https://clownsec.com/3ds/data/catalog.json");
    snprintf(sources[0].dl_base, 128, "https://www.clownsec.com/3ds/");
    snprintf(sources[0].art_base, 128, "https://clownsec.com/3ds/");
    sources[0].kind = 0;
    snprintf(sources[1].name, 64, "Zackks 3DS Archive");
    snprintf(sources[1].url, CAT_URLLEN, "https://www.clownsec.com/3ds/zackks/data/catalog.json");
    sources[1].dl_base[0] = 0;   /* zackk entries carry their own archiveUrl */
    snprintf(sources[1].art_base, 128, "https://clownsec.com/3ds/zackks/artwork/");
    sources[1].kind = 1;
    nsources = 2;
    FILE *f = fopen(SOURCES_FILE, "rb");
    if (f) {
        char line[CAT_URLLEN + 80];
        while (fgets(line, sizeof(line), f) && nsources < SRC_MAX) {
            line[strcspn(line, "\r\n")] = 0;
            if (!line[0]) continue;
            Source *s = &sources[nsources];
            /* user format: name|catalog_url[|dl_base[|art_base[|kind]]] */
            char *cur = line, *p = strchr(cur, '|');
            if (p) { *p = 0; snprintf(s->name, 64, "%s", cur); cur = p + 1;
                     char *p2 = strchr(cur, '|'); if (p2) *p2 = 0;
                     snprintf(s->url, CAT_URLLEN, "%s", cur);
                     if (p2) { cur = p2 + 1; char *p3 = strchr(cur, '|'); if (p3) *p3 = 0;
                               snprintf(s->dl_base, 128, "%s", cur);
                               if (p3) { cur = p3 + 1; char *p4 = strchr(cur, '|'); if (p4) *p4 = 0;
                                         snprintf(s->art_base, 128, "%s", cur);
                                         if (p4) s->kind = atoi(p4 + 1); } } }
            else   { snprintf(s->name, 64, "%.60s", cur);
                     snprintf(s->url, CAT_URLLEN, "%s", cur); }
            nsources++;
        }
        fclose(f);
    }
}

static void add_source_swkbd(void) {
    char name[64] = "", url[CAT_URLLEN] = "";
    SwkbdState s;
    swkbdInit(&s, SWKBD_TYPE_NORMAL, 2, -1);
    swkbdSetHintText(&s, "Source name");
    if (swkbdInputText(&s, name, sizeof(name)) != SWKBD_BUTTON_RIGHT || !name[0]) return;
    swkbdInit(&s, SWKBD_TYPE_NORMAL, 2, -1);
    swkbdSetHintText(&s, "catalog.json URL (https://...)");
    if (swkbdInputText(&s, url, sizeof(url)) != SWKBD_BUTTON_RIGHT || !url[0]) return;
    ensure_cfg_dir();
    FILE *f = fopen(SOURCES_FILE, "ab");
    if (f) { fprintf(f, "%s|%s\n", name, url); fclose(f); }
    load_sources();
}

static void wait_back(void) {
    while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & KEY_B) break;
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank(); }
}

/* fetch a source's unistore, list its moflex/zip, download (+extract zips) to cwd */
static int cat_cmp(const void *a, const void *b) {
    return strcasecmp(((const CatEntry *)a)->name, ((const CatEntry *)b)->name);
}
static char firstc(const char *s) { return (char)toupper((unsigned char)s[0]); }

/* a real zip starts with "PK\x03\x04" (or \x05\x06 empty / \x07\x08 spanned).
   Guards against a 404/HTML page being mistaken for a zip. */
static bool file_is_zip(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    unsigned char h[4] = {0};
    size_t n = fread(h, 1, 4, f);
    fclose(f);
    return n == 4 && h[0] == 'P' && h[1] == 'K' && (h[2] == 3 || h[2] == 5 || h[2] == 7);
}

/* Pick (and optionally create) a destination folder under sdmc:/, starting at cwd.
 * Returns 1 with `out` = chosen dir (trailing '/'), or 0 if cancelled. */
static int pick_folder(char *out, size_t cap) {
    static char dir[PATHLEN];
    static char list[256][NAMELEN];
    snprintf(dir, sizeof(dir), "%s", cwd);
    int nd = 0, psel = 0, pscroll = 0, redraw = 1, rescan = 1, hu = 0, hd = 0;
    while (aptMainLoop()) {
        if (rescan) {
            nd = 0;
            DIR *d = opendir(dir);
            if (d) {
                struct dirent *de;
                while ((de = readdir(d)) && nd < 256)
                    if (de->d_type == DT_DIR && strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
                        snprintf(list[nd++], NAMELEN, "%s", de->d_name);
                closedir(d);
            }
            rescan = 0; redraw = 1;
        }
        int is_root = (strcmp(dir, "sdmc:/") == 0);
        int base = 1 + (is_root ? 0 : 1);        /* [save here] (+ [..]) before folders */
        int total = base + nd;
        if (psel >= total) psel = total - 1;
        if (psel < 0) psel = 0;

        hidScanInput();
        u32 k = hidKeysDown(), kh = hidKeysHeld();
        if (nav_repeat(k, kh, NAV_DOWN, &hd)) { if (psel < total - 1) psel++; redraw = 1; }
        if (nav_repeat(k, kh, NAV_UP,   &hu)) { if (psel > 0) psel--;         redraw = 1; }
        if (k & KEY_B) return 0;
        if (k & KEY_X) {                          /* create a new folder here */
            char nm[NAMELEN] = "";
            SwkbdState s; swkbdInit(&s, SWKBD_TYPE_NORMAL, 2, -1);
            swkbdSetHintText(&s, "New folder name");
            if (swkbdInputText(&s, nm, sizeof(nm)) == SWKBD_BUTTON_RIGHT && nm[0]) {
                char np[PATHLEN]; snprintf(np, sizeof(np), "%s%s", dir, nm);
                mkdir(np, 0777);
                rescan = 1;
            }
        }
        if (k & KEY_A) {
            if (psel == 0) { snprintf(out, cap, "%s", dir); return 1; }   /* save here */
            else if (!is_root && psel == 1) {                            /* go up */
                size_t L = strlen(dir);
                if (L && dir[L - 1] == '/') dir[L - 1] = 0;
                char *sl = strrchr(dir, '/'); if (sl) sl[1] = 0;
                psel = 0; rescan = 1;
            } else {                                                     /* enter folder */
                int fi = psel - base;
                if (fi >= 0 && fi < nd) {
                    char nx[PATHLEN]; snprintf(nx, sizeof(nx), "%s%s/", dir, list[fi]);
                    snprintf(dir, sizeof(dir), "%s", nx);
                    psel = 0; rescan = 1;
                }
            }
        }
        if (redraw) {
            printf("\x1b[2J\x1b[H=== SAVE TO ===\n%.38s\n\n", dir);
            printf("A:open/select  X:new folder  B:cancel\n\n");
            if (psel < pscroll) pscroll = psel;
            if (psel >= pscroll + ROWS) pscroll = psel - ROWS + 1;
            for (int i = pscroll; i < total && i < pscroll + ROWS; i++) {
                char lb[NAMELEN + 8];
                if (i == 0)                     snprintf(lb, sizeof(lb), "[ Save in this folder ]");
                else if (!is_root && i == 1)    snprintf(lb, sizeof(lb), ".. (up)");
                else                            snprintf(lb, sizeof(lb), "[D] %s", list[i - base]);
                printf("%c%.38s\n", i == psel ? '>' : ' ', lb);
            }
            redraw = 0;
        }
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    }
    return 0;
}

/* greedy word-wrap helper for the top-screen info panel */
static void ui_text_wrap(int x, int *y, int scale, u16 col, const char *s, int maxcw, int maxlines) {
    char buf[96];
    int lines = 0;
    while (*s && lines < maxlines) {
        while (*s == ' ') s++;
        if (!*s) break;
        int take = 0, lastsp = -1;
        while (s[take] && take < maxcw) { if (s[take] == ' ') lastsp = take; take++; }
        int cut = (s[take] && lastsp > 0) ? lastsp : take;   /* break at last space if the line is full */
        int n = cut < (int)sizeof(buf) - 1 ? cut : (int)sizeof(buf) - 1;
        memcpy(buf, s, n); buf[n] = 0;
        ui_text(x, *y, scale, col, buf);
        *y += 8 * scale + 2;
        s += cut;
        lines++;
    }
}

/* human-readable byte size, e.g. "1.05 GB" / "980 MB" */
static void fmt_size(long long b, char *o, int cap) {
    if (b <= 0) { o[0] = 0; return; }
    double gb = b / 1073741824.0, mb = b / 1048576.0;
    if (gb >= 1.0)      snprintf(o, cap, "%.2f GB", gb);
    else if (mb >= 1.0) snprintf(o, cap, "%.0f MB", mb);
    else                snprintf(o, cap, "%lld KB", b / 1024);
}

#define POSTER_W 132
#define POSTER_H 188

/* rich movie info on the TOP screen (2D) while scrolling the catalog.
 * poster (row-major RGB565, POSTER_W x POSTER_H) may be NULL. */
/* 3D moflex are distributed with "3D" in the filename (e.g. "Title (2025) (3D).moflex");
 * 2D ones don't. Used to flag stereoscopic movies in the catalog info panel. */
static int cat_is_3d(const CatEntry *e) {
    const char *hay = e->fname[0] ? e->fname : e->name;
    for (const char *p = hay; p[0] && p[1]; p++)
        if (p[0] == '3' && (p[1] == 'D' || p[1] == 'd')) return 1;
    return 0;
}

static void draw_info_top(const CatEntry *e, const u16 *poster) {
    ui_begin(GFX_TOP);
    ui_clear(UI_RGB(8, 10, 16));
    int y = 8;
    ui_text_wrap(10, &y, 2, UI_WHITE, e->name, 24, 2);       /* title (16px, ~24/line) */

    /* poster box on the left */
    int px0 = 10, py0 = 48;
    if (poster) {
        for (int j = 0; j < POSTER_H; j++)
            for (int i = 0; i < POSTER_W; i++)
                ui_px(px0 + i, py0 + j, poster[j * POSTER_W + i]);
    } else {
        ui_fill(px0, py0, POSTER_W, POSTER_H, UI_RGB(24, 28, 40));
        ui_text_center(px0 + POSTER_W / 2, py0 + POSTER_H / 2 - 4, 1, UI_GRAY,
                       e->art[0] ? "loading..." : "no art");
    }

    /* details column to the right of the poster */
    int tx = px0 + POSTER_W + 12, ty = 50;
    char meta[96], sz[24];
    fmt_size(e->size, sz, sizeof(sz));
    if (e->runtime && e->year) snprintf(meta, sizeof(meta), "%d   %d min", e->year, e->runtime);
    else if (e->year)          snprintf(meta, sizeof(meta), "%d", e->year);
    else                       meta[0] = 0;
    if (meta[0]) { ui_text(tx, ty, 1, UI_RGB(120, 200, 255), meta); ty += 14; }
    if (sz[0])   { ui_text(tx, ty, 1, UI_RGB(120, 255, 160), sz);   ty += 14; }
    if (!e->is_zip) {   /* stereoscopic flag so you can tell 3D from 2D before downloading */
        int is3d = cat_is_3d(e);
        ui_text(tx, ty, 1, is3d ? UI_RGB(255, 120, 200) : UI_RGB(150, 150, 150),
                is3d ? "3D  (stereoscopic)" : "2D"); ty += 14;
    }
    if (e->genres[0]) { ui_text_wrap(tx, &ty, 1, UI_GRAY, e->genres, 30, 3); ty += 4; }
    if (e->desc[0])   ui_text_wrap(tx, &ty, 1, UI_RGB(205, 205, 205), e->desc, 30, 12);
    ui_present();
}

/* ---- background poster loader: keeps scrolling perfectly fluid (the main loop never blocks on a
 * poster download/decode; a worker thread does it and the UI just polls for the result). ---- */
static volatile int pw_run = 0, pw_req = -1, pw_done = -2, pw_ok = 0;
static char      pw_url[256], pw_key[NAMELEN];
static u16      *pw_buf = NULL;
static LightLock pw_lock;
static Thread    pw_thread = NULL;

static void pw_fn(void *arg) {
    (void)arg;
    while (pw_run) {
        int id; char url[256], key[NAMELEN];
        LightLock_Lock(&pw_lock);
        id = pw_req; snprintf(url, sizeof url, "%s", pw_url); snprintf(key, sizeof key, "%s", pw_key);
        LightLock_Unlock(&pw_lock);
        if (id < 0 || id == pw_done) { svcSleepThread(12000000); continue; }   /* nothing new -> idle */
        int ok = url[0] && pw_buf && poster_get(url, key, pw_buf, POSTER_W, POSTER_H);
        LightLock_Lock(&pw_lock);
        if (pw_req == id) { pw_ok = ok; pw_done = id; }   /* publish only if still the wanted selection */
        LightLock_Unlock(&pw_lock);
    }
}
static void pw_start(u16 *buf) {
    pw_buf = buf; pw_run = 1; pw_req = -1; pw_done = -2; pw_ok = 0;
    LightLock_Init(&pw_lock);
    s32 prio = 0x30; svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    pw_thread = threadCreate(pw_fn, NULL, 32 * 1024, prio, -1, false);   /* -1 = default app core (preemptive) */
}
static void pw_stop(void) {
    if (!pw_thread) return;
    pw_run = 0; threadJoin(pw_thread, 2000000000LL); threadFree(pw_thread); pw_thread = NULL;
}
static void pw_request(int id, const char *url, const char *key) {
    LightLock_Lock(&pw_lock);
    pw_req = id;
    snprintf(pw_url, sizeof pw_url, "%s", url ? url : "");
    snprintf(pw_key, sizeof pw_key, "%s", key ? key : "");
    LightLock_Unlock(&pw_lock);
}

static void catalog_browse(const Source *src) {
    printf("\x1b[2J\x1b[H=== DOWNLOAD ===\n\nFetching catalog...\n%.38s\n", src->url);
    gfxFlushBuffers(); gfxSwapBuffers();

    char *json = NULL; size_t len = 0;
    if (!download_to_mem(src->url, &json, &len, 32 * 1024 * 1024)) {
        printf("\nFetch failed. Wi-Fi on? URL ok?\nB: back\n"); wait_back(); return;
    }
    int cap = 4096;
    CatEntry *cat = (CatEntry *)malloc(sizeof(CatEntry) * cap);
    int nc = cat ? catalog_parse(json, src->kind, src->dl_base, src->art_base, cat, cap) : 0;
    free(json);
    if (nc <= 0) {
        printf("\nNo .moflex/.zip found in this catalog.\nB: back\n"); wait_back();
        free(cat); return;
    }
    if (nc > 1) qsort(cat, nc, sizeof(CatEntry), cat_cmp);   /* alphabetical */

    gfxSet3D(false);   /* top screen becomes a 2D info panel while browsing */
    u16 *poster  = (u16 *)malloc((size_t)POSTER_W * POSTER_H * sizeof(u16));
    u16 *pworker = (u16 *)malloc((size_t)POSTER_W * POSTER_H * sizeof(u16));
    if (pworker) pw_start(pworker);   /* background loader -> scrolling never blocks on a poster */
    int csel = 0, cscroll = 0, redraw = 1, hfu = 0, hfd = 0, requested = 0;
    int shown = -1, phave = 0, settle = 0;   /* poster debounce state */
    while (aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown();
        u32 kh = hidKeysHeld();
        if (k & KEY_B) break;
        if (nav_repeat(k, kh, NAV_DOWN, &hfd)) { if (csel < nc - 1) csel++; redraw = 1; }
        if (nav_repeat(k, kh, NAV_UP, &hfu))   { if (csel > 0) csel--; redraw = 1; }
        if (k & KEY_RIGHT) { csel += ROWS; if (csel > nc - 1) csel = nc - 1; redraw = 1; }
        if (k & KEY_LEFT)  { csel -= ROWS; if (csel < 0) csel = 0; redraw = 1; }
        if (k & KEY_R) {   /* jump to next first-letter group */
            char c = firstc(cat[csel].name); int i = csel;
            while (i < nc && firstc(cat[i].name) == c) i++;
            if (i < nc) csel = i; redraw = 1;
        }
        if (k & KEY_L) {   /* jump to start of this letter group, or previous */
            int i = csel; char c = firstc(cat[i].name);
            while (i > 0 && firstc(cat[i - 1].name) == c) i--;
            if (i == csel && i > 0) { i--; char p = firstc(cat[i].name);
                while (i > 0 && firstc(cat[i - 1].name) == p) i--; }
            csel = i; redraw = 1;
        }
        if (k & KEY_A) {
            CatEntry *e = &cat[csel];
            char destdir[PATHLEN];
            if (!pick_folder(destdir, sizeof(destdir))) { redraw = 1; continue; }   /* cancelled */
            char dest[PATHLEN + NAMELEN];
            snprintf(dest, sizeof(dest), "%s%s", destdir, e->fname);
            snprintf(g_dl_name, sizeof(g_dl_name), "%s", e->fname);
            g_last_prog = 0;
            bool ok = download_to_file(e->url, dest, dl_progress, NULL);
            if (ok && e->is_zip && !file_is_zip(dest)) {
                remove(dest);   /* not a real zip (likely a 404 page) */
                printf("\x1b[2J\x1b[H=== DOWNLOAD ===\n\nDownload failed: the server did not\n"
                       "return a zip (file not found at URL).\n\nB: back\n");
            } else if (ok && e->is_zip) {
                if (confirm("Extract the zip now?\n(No = keep the .zip file)")) {
                    char stem[NAMELEN]; snprintf(stem, sizeof(stem), "%s", e->fname);
                    size_t sl = strlen(stem); if (sl > 4) stem[sl - 4] = 0;   /* drop .zip */
                    char folder[PATHLEN + NAMELEN];
                    snprintf(folder, sizeof(folder), "%s%s", destdir, stem);
                    printf("\x1b[2J\x1b[H=== DOWNLOAD ===\n\nExtracting %.30s ...\n", stem);
                    gfxFlushBuffers(); gfxSwapBuffers();
                    int nf = unzip_to_dir(dest, folder);
                    remove(dest);   /* delete the .zip after extracting */
                    if (nf > 0) printf("\nExtracted %d file%s.\nFolder: %.30s\n\nB: back\n", nf, nf == 1 ? "" : "s", stem);
                    else        printf("\nExtract FAILED.\nFolder: %.30s\n\nB: back\n", stem);
                } else {
                    printf("\x1b[2J\x1b[H=== DOWNLOAD ===\n\nZip saved (not extracted):\n%.32s\n\nB: back\n", e->fname);
                }
            } else {
                printf("\x1b[2J\x1b[H=== DOWNLOAD ===\n\n%s\n%s\n\nB: back\n",
                       g_dl_name, ok ? "Done." : "Failed / cancelled.");
            }
            wait_back();
            redraw = 1;
        }
        if (csel != shown) { redraw = 1; phave = 0; settle = 0; requested = 0; }   /* moved -> drop poster */
        if (redraw) {
            printf("\x1b[2J\x1b[HDOWNLOAD  %d/%d  [%c]\n", csel + 1, nc, firstc(cat[csel].name));
            printf("Up/Dn:1  <>:page  L/R:letter\n");
            printf("A:get  B:back  save:%.14s\n\n", cwd);
            if (csel < cscroll) cscroll = csel;
            if (csel >= cscroll + ROWS) cscroll = csel - ROWS + 1;
            for (int i = cscroll; i < nc && i < cscroll + ROWS; i++)
                printf("%c%.37s\n", i == csel ? '>' : ' ', cat[i].name);
            draw_info_top(&cat[csel], phave ? poster : NULL);
            shown = csel;
            redraw = 0;
        }
        /* debounced request to the background loader (a few idle frames), then poll for its result --
         * NOTHING here blocks, so scrolling is always fluid no matter how slow the poster is. */
        if (!phave && !requested && cat[csel].art[0] && ++settle >= 6) {
            pw_request(csel, cat[csel].art, cat[csel].fname);
            requested = 1;
        }
        if (!phave && requested && pw_done == csel) {
            if (pw_ok && poster && pworker) { memcpy(poster, pworker, (size_t)POSTER_W * POSTER_H * 2); phave = 1; }
            requested = 0; redraw = 1;   /* redraw to show the poster (or leave the panel text-only) */
        }
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    }
    pw_stop();
    branding_show();   /* restore the 3D logo on the top screen */
    free(poster);
    free(pworker);
    free(cat);
}

/* Enter any URL on the keyboard and download it straight to a chosen folder (cia, moflex, anything). */
static void download_url_direct(void) {
    char url[CAT_URLLEN] = "";
    SwkbdState s;
    swkbdInit(&s, SWKBD_TYPE_NORMAL, 2, -1);
    swkbdSetHintText(&s, "File URL: https://... (.cia/.moflex/any)");
    if (swkbdInputText(&s, url, sizeof(url)) != SWKBD_BUTTON_RIGHT || !url[0]) return;
    /* default filename = last path segment of the URL, minus any ?query/#frag */
    char fname[NAMELEN];
    const char *slash = strrchr(url, '/'); const char *nm = slash ? slash + 1 : url;
    snprintf(fname, sizeof(fname), "%s", nm);
    char *cut = strpbrk(fname, "?#"); if (cut) *cut = 0;
    if (!fname[0]) snprintf(fname, sizeof(fname), "download.bin");
    /* let the user confirm/rename the save filename */
    swkbdInit(&s, SWKBD_TYPE_NORMAL, 2, -1);
    swkbdSetHintText(&s, "Save as (filename)");
    swkbdSetInitialText(&s, fname);
    if (swkbdInputText(&s, fname, sizeof(fname)) != SWKBD_BUTTON_RIGHT || !fname[0]) return;
    char destdir[PATHLEN];
    if (!pick_folder(destdir, sizeof(destdir))) return;
    char dest[PATHLEN + NAMELEN];
    snprintf(dest, sizeof(dest), "%s%s", destdir, fname);
    snprintf(g_dl_name, sizeof(g_dl_name), "%s", fname);
    g_last_prog = 0;
    consoleInit(GFX_BOTTOM, NULL);
    printf("\x1b[2J\x1b[H=== DOWNLOAD URL ===\n\n-> %.32s\n\n", fname);
    gfxFlushBuffers(); gfxSwapBuffers();
    bool ok = download_to_file(url, dest, dl_progress, NULL);
    if (!ok) { remove(dest); printf("\nFailed / cancelled.\n\nB: back\n"); wait_back(); return; }
    size_t fl = strlen(fname);
    if (fl > 4 && !strcasecmp(fname + fl - 4, ".zip") && file_is_zip(dest) &&
        confirm("Extract the zip now?\n(No = keep the .zip file)")) {
        char stem[NAMELEN]; snprintf(stem, sizeof(stem), "%s", fname); stem[fl - 4] = 0;
        char folder[PATHLEN + NAMELEN]; snprintf(folder, sizeof(folder), "%s%s", destdir, stem);
        printf("\x1b[2J\x1b[H=== DOWNLOAD URL ===\n\nExtracting %.28s ...\n", stem);
        gfxFlushBuffers(); gfxSwapBuffers();
        int nf = unzip_to_dir(dest, folder);
        remove(dest);
        printf(nf > 0 ? "\nExtracted %d file%s.\n\nB: back\n" : "\nExtract FAILED.\n\nB: back\n",
               nf, nf == 1 ? "" : "s");
    } else {
        printf("\nDone.\n\nB: back\n");
    }
    wait_back();
}

static void download_screen(void) {
    load_sources();
    int m = 0, redraw = 1;
    while (aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown();
        if (k & KEY_B) break;
        int total = nsources + 2;   /* + URL download + add source */
        if (k & KEY_DOWN) { m = (m + 1) % total; redraw = 1; }
        if (k & KEY_UP)   { m = (m - 1 + total) % total; redraw = 1; }
        if (k & KEY_A) {
            if (m < nsources) catalog_browse(&sources[m]);
            else if (m == nsources) download_url_direct();
            else add_source_swkbd();
            redraw = 1;
        }
        if (redraw) {
            printf("\x1b[2J\x1b[H=== DOWNLOAD: choose source ===\n");
            printf("Save to: %.30s\n", cwd);
            printf("A:open  B:back\n\n");
            for (int i = 0; i < nsources; i++)
                printf("%c %.36s\n", i == m ? '>' : ' ', sources[i].name);
            printf("%c + Download from URL\n", m == nsources ? '>' : ' ');
            printf("%c + Add source (keyboard)\n", m == nsources + 1 ? '>' : ' ');
            redraw = 0;
        }
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    }
}

/* ---------- ADD MOVIES menu ---------- */

static void add_movies_menu(void) {
    const char *items[] = { "DOWNLOAD  (from your website)", "UPLOAD    (from a computer)" };
    int m = 0, redraw = 1;
    while (aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown();
        if (k & KEY_B) break;
        if (k & (KEY_UP | KEY_DOWN)) { m ^= 1; redraw = 1; }
        if (k & KEY_A) {
            if (m == 0) download_screen(); else upload_screen();
            redraw = 1;
        }
        if (redraw) {
            printf("\x1b[2J\x1b[H=== ADD MOVIES ===\n\nUp/Down select   A: open   B: back\n\n");
            for (int i = 0; i < 2; i++) printf("%c %s\n", i == m ? '>' : ' ', items[i]);
            redraw = 0;
        }
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    }
}

/* ---------- MANAGE menu (move / delete) ---------- */

static void manage_menu(void) {
    if (nentries == 0) return;
    char full[PATHLEN + NAMELEN];
    snprintf(full, sizeof(full), "%s%s", cwd, entries[sel].name);
    const char *items[] = { "DELETE", "MOVE (cut)", "CANCEL" };
    int m = 0, redraw = 1;
    while (aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown();
        if (k & KEY_B) break;
        if (k & KEY_DOWN) { m = (m + 1) % 3; redraw = 1; }
        if (k & KEY_UP)   { m = (m + 2) % 3; redraw = 1; }
        if (k & KEY_A) {
            if (m == 0) {
                if (confirm("Delete this item?")) { remove(full); scan(); }
            } else if (m == 1) {
                snprintf(move_src, sizeof(move_src), "%s", full);
                snprintf(move_name, sizeof(move_name), "%s", entries[sel].name);
                move_pending = 1;
            }
            break;
        }
        if (redraw) {
            printf("\x1b[2J\x1b[H=== MANAGE ===\n\n%.38s\n\n", entries[sel].name);
            for (int i = 0; i < 3; i++) printf("%c %s\n", i == m ? '>' : ' ', items[i]);
            printf("\nB: back\n");
            redraw = 0;
        }
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    }
}

static void do_paste(void) {
    char dest[PATHLEN + NAMELEN];
    snprintf(dest, sizeof(dest), "%s%s", cwd, move_name);
    if (strcmp(dest, move_src) != 0) rename(move_src, dest);
    move_pending = 0;
    scan();
}

/* play a movie file, then restore the menu screens. returns the play result. */
/* Choose which embedded moflex to play when a CIA holds several. Returns index, or -1 = back. */
static int pick_moflex(const CiaMoflex *list, int n) {
    int sel = 0, top = 0, prev = -1, hold = 0;
    for (;;) {
        if (!aptMainLoop()) return -1;
        if (sel != prev) {                                  /* redraw only on change (in place, no flash) */
            if (sel < top) top = sel; if (sel >= top + 20) top = sel - 19;
            printf("\x1b[H=== CHOOSE VIDEO ===\x1b[K\n%d in this CIA\x1b[K\nUp/Dn  A:play  B:back\x1b[K\n\x1b[K\n", n);
            for (int i = top; i < top + 20; i++) {
                if (i < n) {
                    char nm[64]; snprintf(nm, sizeof nm, "%s", list[i].name);
                    size_t L = strlen(nm); if (L > 7 && !strcasecmp(nm + L - 7, ".moflex")) nm[L - 7] = 0;
                    printf("%c%.30s %lldMB\x1b[K\n", i == sel ? '>' : ' ', nm, (long long)(list[i].size / 1000000));
                } else printf("\x1b[K\n");
            }
            printf("\x1b[J");
            prev = sel;
        }
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
        hidScanInput();
        u32 k = hidKeysDown(), kh = hidKeysHeld();
        if (kh & (KEY_DOWN | KEY_UP)) hold++; else hold = 0;
        int step = (k & (KEY_DOWN | KEY_UP)) || (hold > 12 && hold % 4 == 0);
        if (step && (kh & KEY_DOWN) && sel < n - 1) sel++;
        if (step && (kh & KEY_UP)   && sel > 0)     sel--;
        if (k & KEY_A) return sel;
        if (k & KEY_B) return -1;
    }
}

static MoflexResult play_movie(const char *path) {
    cia_clear_selection();
    const char *title_src = path; char titlebuf[NAMELEN];
    if (cia_is_cia(path)) {
        CiaMoflex list[64];
        int nc = cia_list_moflex(path, list, 64);
        if (nc == 0) {
            consoleInit(GFX_BOTTOM, NULL);
            printf("\x1b[2J\x1b[H=== NOT A MOVIE ===\n\nThis CIA has no playable video inside\n"
                   "(it's an app, or its content is\nencrypted).\n\nB: back\n");
            wait_back(); branding_show();
            return MOFLEX_QUIT_BACK;
        }
        int sel = 0;
        if (nc > 1) {                                   /* several videos -> let the user choose */
            consoleInit(GFX_BOTTOM, NULL);
            sel = pick_moflex(list, nc);
            if (sel < 0) { branding_show(); return MOFLEX_QUIT_BACK; }
        }
        /* title = the embedded movie's real name (from movie_title.csv); only fall back to the CIA's own
         * filename for a lone unnamed moflex (nicer than a bare "movie"). */
        { size_t L = strlen(list[sel].name);
          int raw = (L > 7 && !strcasecmp(list[sel].name + L - 7, ".moflex"));
          if (!raw || nc > 1) { snprintf(titlebuf, sizeof titlebuf, "%s", list[sel].name); title_src = titlebuf; } }
        cia_set_selection(path, list[sel].off, list[sel].size, title_src == titlebuf ? titlebuf : NULL);
    }
    snprintf(g_now_playing_path, sizeof(g_now_playing_path), "%s", path);
    { const char *base = strrchr(title_src, '/'); base = base ? base + 1 : title_src;
      snprintf(g_now_playing, sizeof(g_now_playing), "%s", base);
      size_t L = strlen(g_now_playing);   /* hide extension in the title */
      if (L > 7 && !strcasecmp(g_now_playing + L - 7, ".moflex")) g_now_playing[L - 7] = 0;
      else if (L > 4 && !strcasecmp(g_now_playing + L - 4, ".zip")) g_now_playing[L - 4] = 0;
      else if (L > 4 && !strcasecmp(g_now_playing + L - 4, ".cia")) g_now_playing[L - 4] = 0; }
    MoflexResult r = moflex_play(path);
    cia_clear_selection();
    consoleInit(GFX_BOTTOM, NULL);
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
    branding_show();               /* restore the 3D logo on the top screen */
    return r;
}

/* ---------- browser (used by OPEN and MANAGE) ---------- */
/* returns: 1 = exit app, 0 = back to the player home, 2 = a file was picked (sel_out set) */

static int browser(int mode, char *sel_out, size_t sel_cap) {
    s_filter_movies = (mode != MODE_MANAGE) ? 1 : s_manage_movies_only;   /* play=movies; manage=toggle */
    scan();
    draw_browser(mode);
    int hfu = 0, hfd = 0;
    while (aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown();
        u32 kh = hidKeysHeld();
        if (k & KEY_START) return 1;

        if (nentries) {
            if (nav_repeat(k, kh, NAV_DOWN, &hfd)) { if (sel < nentries - 1) sel++; draw_browser(mode); }
            if (nav_repeat(k, kh, NAV_UP,   &hfu)) { if (sel > 0) sel--;           draw_browser(mode); }
            if (k & KEY_RIGHT) { sel += ROWS; if (sel > nentries - 1) sel = nentries - 1; draw_browser(mode); }
            if (k & KEY_LEFT)  { sel -= ROWS; if (sel < 0) sel = 0; draw_browser(mode); }
        }
        if (k & KEY_SELECT) { scan(); draw_browser(mode); }
        if (k & KEY_Y && mode == MODE_MANAGE) {   /* toggle: all files vs movies only */
            s_manage_movies_only = !s_manage_movies_only;
            s_filter_movies = s_manage_movies_only;
            if (sel > 0) sel = 0;
            scan(); draw_browser(mode);
        }
        if (k & KEY_R && move_pending) { do_paste(); draw_browser(mode); }
        if (k & KEY_B) {
            if (at_root()) return 0;           /* leave browser -> home menu */
            go_up(); draw_browser(mode);
        }
        if (k & KEY_X && nentries) { manage_menu(); scan(); draw_browser(mode); }
        if (k & KEY_A && nentries) {
            if (entries[sel].is_dir) {
                enter_dir(entries[sel].name);
                draw_browser(mode);
            } else if (mode == MODE_MANAGE) {
                manage_menu(); scan(); draw_browser(mode);
            } else {   /* MODE_PLAY: hand the selection back to main to play */
                if (sel_out) snprintf(sel_out, sel_cap, "%s%s", cwd, entries[sel].name);
                return 2;
            }
        }
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    }
    return 1;
}

/* ---------- graphical home screen (mockup: title, play, seek bar, 3 buttons) --- */
/* returns 0=OPEN, 1=ADD MOVIES, 2=MANAGE, -1=exit */

typedef struct { int x, y, w, h; const char *label; int choice; } HomeBtn;
static HomeBtn g_btns[3] = {
    {   6, 210,  92, 24, "OPEN VIDEO",    0 },
    { 106, 210, 108, 24, "MANAGE VIDEOS", 2 },
    { 222, 210,  92, 24, "ADD VIDEOS",    1 },
};

static void home_draw(int bsel, long long rpos) {
    ui_begin(GFX_BOTTOM);
    ui_clear(UI_BLACK);

    int loaded = g_now_playing[0] != 0;

    /* app title */
    ui_text_center(UI_W / 2, 12, 2, UI_WHITE, "CLOWNSEC VIDEO");
    ui_text(UI_W - 52, 2, 1, UI_RGB(90, 90, 90), APP_VERSION);   /* version (bump for releases) */

    /* loaded movie name, or a prompt */
    if (loaded) {
        char nm[80]; snprintf(nm, sizeof(nm), "%s", g_now_playing);
        int maxc = (UI_W - 12) / 8;
        if ((int)strlen(nm) > maxc) nm[maxc] = 0;
        ui_text_center(UI_W / 2, 44, 1, UI_RGB(120, 200, 255), nm);
    } else {
        ui_text_center(UI_W / 2, 44, 1, UI_GRAY, "No video loaded");
    }

    /* big PLAY button (not a scrubber) */
    int pcx = UI_W / 2, pcy = 104;
    ui_play(pcx, pcy, 46, loaded ? UI_RGB(120, 255, 160) : UI_WHITE);
    ui_text_center(pcx, pcy + 36, 1, UI_GRAY,
                   loaded ? "PLAY / RESUME" : "PLAY - choose a video");

    /* real resume time as plain text (no fake progress bar) */
    if (loaded && rpos > 0) {
        long t = (long)(rpos / 1000000);
        char rt[40]; snprintf(rt, sizeof(rt), "resume at %ld:%02ld", t / 60, t % 60);
        ui_text_center(pcx, pcy + 50, 1, UI_RGB(150, 150, 150), rt);
    }

    /* three buttons */
    for (int i = 0; i < 3; i++) {
        HomeBtn *b = &g_btns[i];
        if (i == bsel) {
            ui_fill(b->x, b->y, b->w, b->h, UI_WHITE);
            ui_text(b->x + 4, b->y + 8, 1, UI_BLACK, b->label);
        } else {
            ui_text(b->x + 4, b->y + 8, 1, UI_WHITE, b->label);
        }
    }
}

static int home_gui(void) {
    int bsel = 0, redraw = 1;
    /* read the resume position ONCE (not every frame -- that hammered the SD card) */
    long long rpos = g_now_playing_path[0] ? moflex_resume_get(g_now_playing_path) : 0;
    while (aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown();
        if (k & KEY_START) return -1;
        if (k & KEY_RIGHT) { bsel = (bsel + 1) % 3; redraw = 1; }
        if (k & KEY_LEFT)  { bsel = (bsel + 2) % 3; redraw = 1; }
        if (k & KEY_A) return g_btns[bsel].choice;

        if (k & KEY_TOUCH) {
            touchPosition t; hidTouchRead(&t);
            for (int i = 0; i < 3; i++) {
                HomeBtn *b = &g_btns[i];
                if (t.px >= b->x && t.px < b->x + b->w && t.py >= b->y && t.py < b->y + b->h)
                    return b->choice;
            }
            /* PLAY button: resume the loaded movie, or open the browser if none */
            if (t.px > UI_W/2 - 34 && t.px < UI_W/2 + 34 && t.py > 78 && t.py < 148)
                return g_now_playing_path[0] ? 3 : 0;
        }

        if (redraw) {               /* only redraw on change, not every frame */
            home_draw(bsel, rpos);
            ui_present();           /* offscreen -> fb in one shot (no flicker) */
            redraw = 0;
        }
        gspWaitForVBlank();
    }
    return -1;
}

/* ---------- main ---------- */

#define SOC_BUF_SZ 0x100000
int main(void) {
    osSetSpeedupEnable(true);   /* unlock New 3DS 804MHz clock (no-op on old 3DS) */
    gfxInitDefault();
    ndspInit();

    /* sockets: init once, shared by the web server (UPLOAD) and curl (DOWNLOAD) */
    u32 *soc_buf = (u32 *)memalign(0x1000, SOC_BUF_SZ);
    if (soc_buf) socInit(soc_buf, SOC_BUF_SZ);

    gfxSetScreenFormat(GFX_BOTTOM, GSP_RGB565_OES);   /* match ui_gfx (u16) */
    consoleInit(GFX_BOTTOM, NULL);
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
    branding_show();                                  /* 3D CLOWNSEC logo on top */

    int running = 1;
    while (running && aptMainLoop()) {
        int choice = home_gui();
        if (choice < 0) break;
        if (choice == 0) {                       /* OPEN / PLAY-empty -> browser -> play -> back home */
            char path[PATHLEN + NAMELEN];
            int r = browser(MODE_PLAY, path, sizeof(path));
            if (r == 1) running = 0;
            else if (r == 2 && play_movie(path) == MOFLEX_QUIT_EXIT) running = 0;
        }
        else if (choice == 1) { add_movies_menu(); }
        else if (choice == 2) { if (browser(MODE_MANAGE, NULL, 0) == 1) running = 0; }
        else if (choice == 3) {                  /* PLAY: resume the loaded movie */
            if (g_now_playing_path[0] && play_movie(g_now_playing_path) == MOFLEX_QUIT_EXIT) running = 0;
        }
    }

    downloader_exit();
    socExit();
    ndspExit();
    gfxExit();
    return 0;
}
