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
#include <time.h>

#include "moflex_playback.h"
#include "httpd.h"
#include "downloader.h"
#include "catalog.h"
#include "poster.h"
#include "movieinfo.h"
#include "unzip.h"
#include "cia_moflex.h"
#include "ui_gfx.h"
#include "branding.h"

#define APP_VERSION "v1.0.0"
#ifndef BUILD_TAG
#define BUILD_TAG "dev"      /* set by the Makefile: date +%y%m%d.%H%M of the build */
#endif
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
static char  s_presel[NAMELEN] = "";   /* entry to highlight on the next browser() entry (BACK from player) */

/* Point the browser at a file: cwd = its folder, and highlight+scroll to it on entry. */
static void browser_preselect_path(const char *path) {
    const char *b = strrchr(path, '/');
    if (!b) return;
    snprintf(cwd, sizeof cwd, "%.*s", (int)(b - path + 1), path);   /* folder, incl. trailing '/' */
    snprintf(s_presel, sizeof s_presel, "%s", b + 1);
}

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
static int s_show_hidden = 0;          /* play-mode toggle (Y): reveal system/hidden folders + dotfiles */

/* System / app folders hidden by default so the browser isn't cluttered with things that are
 * never movies. The Y toggle reveals them (along with dotfiles). */
static int is_hidden_dir(const char *n) {
    static const char *sys[] = {
        "3ds", "dcim", "fbi", "gm9", "cheats", "cias", "luma", "nintendo 3ds",
        "roms", "system volume information", "themes", "moviedata", NULL
    };
    for (int i = 0; sys[i]; i++) if (!strcasecmp(n, sys[i])) return 1;
    return 0;
}

static void scan(void) {
    nentries = 0;
    DIR *d = opendir(cwd);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && nentries < MAXE) {
            if (e->d_name[0] == '.' && !s_show_hidden) continue;   /* dotfiles hidden unless revealed */
            char full[PATHLEN + NAMELEN];
            snprintf(full, sizeof(full), "%s%s", cwd, e->d_name);
            struct stat st;
            int isdir = (stat(full, &st) == 0) && S_ISDIR(st.st_mode);
            if (isdir && !s_show_hidden && is_hidden_dir(e->d_name)) continue;   /* system folders */
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

/* graphical-list layout + shared row helpers (defined further down with the browser) */
#define BR_ROWS   6
#define BR_ROWH   27
#define BR_LIST_Y 46
/* one-shot marquee: pause at the start, scroll to reveal the end, pause, snap back, repeat --
 * far easier to read than an endless crawl. `key` = unique id of the selected row (per list). */
static int g_mq_key = -1, g_mq_off = 0, g_mq_hold = 0, g_mq_tick = 0;
static int marquee_off(int key, int textw, int avail, int *nr) {
    if (key != g_mq_key) { g_mq_key = key; g_mq_off = 0; g_mq_hold = 110; g_mq_tick = 0; *nr = 1; }
    int maxoff = textw - avail;
    if (maxoff <= 0) { if (g_mq_off) { g_mq_off = 0; *nr = 1; } return 0; }
    int before = g_mq_off;
    if (g_mq_hold > 0) g_mq_hold--;                                  /* holding (at the start, or the end) */
    else if (g_mq_off < maxoff) { if (++g_mq_tick >= 2) { g_mq_tick = 0; if (++g_mq_off >= maxoff) g_mq_hold = 70; } }
    else { g_mq_off = 0; g_mq_hold = 110; }                          /* end revealed -> snap back + pause */
    if (g_mq_off != before) *nr = 1;
    return g_mq_off;
}
static void icon_folder(int x, int y, u16 c);
static void icon_movie(int x, int y, u16 c);
static void icon_file(int x, int y, u16 c);
static void strip_ext(char *d);
static int  ui_menu(const char *title, const char *subtitle, const char *const *items, int n);
static int  scrape_one(const char *moviepath);           /* Get Info for one movie (defined later) */
static void lib_getinfo_menu(int *idx, int ni, int csel); /* library Get Info: this / all-missing (defined later) */


/* ---------- small modal helpers ---------- */

/* center a string, truncating with ".." if it would exceed maxw pixels (never overflows) */
static void ui_text_fit(int cx, int y, int scale, u16 c, const char *s, int maxw) {
    if (ui_text_w(scale, s) <= maxw) { ui_text_center(cx, y, scale, c, s); return; }
    int maxch = maxw / (8 * scale) - 2; if (maxch < 1) maxch = 1;
    char buf[160]; snprintf(buf, sizeof buf, "%.*s..", ui_u8_bytes(s, maxch), s);   /* cut on a codepoint */
    ui_text_center(cx, y, scale, c, buf);
}
/* left-aligned truncate-to-fit (x is the LEFT edge, unlike ui_text_fit which centers on cx) */
static void ui_text_left_fit(int x, int y, int scale, u16 c, const char *s, int maxw) {
    if (ui_text_w(scale, s) <= maxw) { ui_text(x, y, scale, c, s); return; }
    int maxch = maxw / (8 * scale) - 2; if (maxch < 1) maxch = 1;
    char buf[160]; snprintf(buf, sizeof buf, "%.*s..", ui_u8_bytes(s, maxch), s);   /* cut on a codepoint */
    ui_text(x, y, scale, c, buf);
}
/* center a string, shrinking the scale (maxscale..1) until it fits -- keeps the whole thing readable */
static void ui_text_center_fit(int cx, int y, int maxscale, u16 c, const char *s, int maxw) {
    int sc = maxscale;
    while (sc > 1 && ui_text_w(sc, s) > maxw) sc--;
    ui_text_center(cx, y, sc, c, s);
}
/* neon message popup with a Back button (replaces the old console result screens) */
static void msg_screen(const char *title, const char *body) {
    int redraw = 1, tdown = 0, tx0 = 0, ty0 = 0;
    while (aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown(), ku = hidKeysUp();
        if (k & (KEY_B | KEY_A)) break;
        touchPosition tp; hidTouchRead(&tp);
        if (k & KEY_TOUCH) { tdown = 1; tx0 = tp.px; ty0 = tp.py; }
        else if ((ku & KEY_TOUCH) && tdown) { tdown = 0; if (ty0 >= 206 && ty0 < 234 && tx0 >= 110 && tx0 < 210) break; }
        if (redraw) {
            ui_begin(GFX_BOTTOM);
            ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);
            ui_text_center(UI_W / 2, 14, 2, UI_NEON, title);
            ui_glow_round(28, 42, UI_W - 56, 2, 1, UI_NEON, 3, 30);
            ui_fill_round(28, 42, UI_W - 56, 2, 1, UI_NEON);
            int ly = 74; const char *p = body; char line[80];
            while (*p) { int j = 0; while (*p && *p != '\n' && j < 79) line[j++] = *p++;
                line[j] = 0; if (*p == '\n') p++;
                ui_text_fit(UI_W / 2, ly, 1, UI_INK, line, UI_W - 16); ly += 16; }
            ui_button(110, 206, 100, 28, "Back", 0, UI_NEONP);
            ui_present(); redraw = 0;
        }
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    }
}

static int confirm(const char *prompt) {
    int redraw = 1, tdown = 0, tx0 = 0, ty0 = 0;
    int bw = 116, bh = 36, by = 158, yesx = 30, nox = UI_W - 30 - bw;
    while (aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown(), ku = hidKeysUp();
        if (k & KEY_A) return 1;
        if (k & KEY_B) return 0;
        touchPosition tp; hidTouchRead(&tp);
        if (k & KEY_TOUCH) { tdown = 1; tx0 = tp.px; ty0 = tp.py; }
        else if ((ku & KEY_TOUCH) && tdown) { tdown = 0;
            if (ty0 >= by && ty0 < by + bh) {
                if (tx0 >= yesx && tx0 < yesx + bw) return 1;
                if (tx0 >= nox  && tx0 < nox + bw)  return 0;
            }
        }
        if (redraw) {
            ui_begin(GFX_BOTTOM);
            ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);
            int ly = 66; const char *p = prompt; char line[64];   /* draw prompt lines (split on '\n') */
            while (*p) {
                int j = 0; while (*p && *p != '\n' && j < 63) line[j++] = *p++;
                line[j] = 0; if (*p == '\n') p++;
                ui_text_fit(UI_W / 2, ly, 1, UI_INK, line, UI_W - 16); ly += 16;
            }
            ui_button(yesx, by, bw, bh, "YES", 1, UI_NEON);
            ui_button(nox,  by, bw, bh, "NO",  0, UI_RED);
            ui_present(); redraw = 0;
        }
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    }
    return 0;
}

/* Generic two-button modal: d-pad moves the highlight, A activates it, B backs out.
 * body may contain '\n' line breaks. Returns 0 (left button), 1 (right), or -1 on B. */
static int prompt2(const char *title, const char *body, const char *lbl0, const char *lbl1) {
    int redraw = 1, tdown = 0, tx0 = 0, ty0 = 0, psel = 0;
    int bw = 116, bh = 36, by = 158, x0 = 30, x1 = UI_W - 30 - bw;
    while (aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown(), ku = hidKeysUp();
        if (k & (KEY_LEFT | KEY_RIGHT)) { psel = !psel; redraw = 1; }
        if (k & KEY_A) return psel;
        if (k & KEY_B) return -1;
        touchPosition tp; hidTouchRead(&tp);
        if (k & KEY_TOUCH) { tdown = 1; tx0 = tp.px; ty0 = tp.py; }
        else if ((ku & KEY_TOUCH) && tdown) { tdown = 0;
            if (ty0 >= by && ty0 < by + bh) {
                if (tx0 >= x0 && tx0 < x0 + bw) return 0;
                if (tx0 >= x1 && tx0 < x1 + bw) return 1;
            }
        }
        if (redraw) {
            ui_begin(GFX_BOTTOM);
            ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);
            ui_text_center(UI_W / 2, 48, 2, UI_NEON, title);
            int ly = 88; const char *p = body; char line[64];
            while (*p) {
                int j = 0; while (*p && *p != '\n' && j < 63) line[j++] = *p++;
                line[j] = 0; if (*p == '\n') p++;
                ui_text_fit(UI_W / 2, ly, 1, UI_INK, line, UI_W - 16); ly += 16;
            }
            ui_button(x0, by, bw, bh, lbl0, psel == 0, UI_NEON);
            ui_button(x1, by, bw, bh, lbl1, psel == 1, UI_NEONP);
            ui_present(); redraw = 0;
        }
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    }
    return -1;
}

/* Pre-played movie: ask whether to resume at the saved time or start fresh.
 * D-pad moves between the buttons, A activates the highlighted one, B backs out.
 * Returns 1 = resume, 0 = start over, -1 = back to the previous screen. */
static int resume_prompt(const char *name, long long rp_us) {
    int redraw = 1, tdown = 0, tx0 = 0, ty0 = 0, sel = 0;   /* 0 = RESUME, 1 = START OVER */
    int bw = 116, bh = 36, by = 158, resx = 30, begx = UI_W - 30 - bw;
    int s = (int)(rp_us / 1000000);
    char tbuf[16];
    if (s >= 3600) snprintf(tbuf, sizeof tbuf, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else           snprintf(tbuf, sizeof tbuf, "%d:%02d", s / 60, s % 60);
    while (aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown(), ku = hidKeysUp();
        if (k & (KEY_LEFT | KEY_RIGHT)) { sel = !sel; redraw = 1; }   /* two buttons: either key flips */
        if (k & KEY_A) return sel == 0 ? 1 : 0;
        if (k & KEY_B) return -1;
        touchPosition tp; hidTouchRead(&tp);
        if (k & KEY_TOUCH) { tdown = 1; tx0 = tp.px; ty0 = tp.py; }
        else if ((ku & KEY_TOUCH) && tdown) { tdown = 0;
            if (ty0 >= by && ty0 < by + bh) {
                if (tx0 >= resx && tx0 < resx + bw) return 1;
                if (tx0 >= begx && tx0 < begx + bw) return 0;
            }
        }
        if (redraw) {
            ui_begin(GFX_BOTTOM);
            ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);
            ui_text_fit(UI_W / 2, 52, 1, UI_DIM, name, UI_W - 16);
            char q[48]; snprintf(q, sizeof q, "Resume at %s?", tbuf);
            ui_text_center(UI_W / 2, 84, 2, UI_NEON, q);
            ui_button(resx, by, bw, bh, "RESUME",     sel == 0, UI_NEON);
            ui_button(begx, by, bw, bh, "RESTART", sel == 1, UI_NEONP);
            ui_present(); redraw = 0;
        }
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    }
    return -1;
}

/* ---------- UPLOAD (web server, on-demand only) ---------- */

static void upload_screen(void) {
    int ok = httpd_start();
    int redraw = 1, tdown = 0, tx0 = 0, ty0 = 0, wasactive = 0;
    long lastdone = -1;
    while (aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown(), ku = hidKeysUp();
        if (k & KEY_B) break;
        touchPosition tp; hidTouchRead(&tp);
        if (k & KEY_TOUCH) { tdown = 1; tx0 = tp.px; ty0 = tp.py; }
        else if ((ku & KEY_TOUCH) && tdown) tdown = 0;
        /* poll the server thread for live upload progress */
        long done = 0, total = 0; char nm[128] = "";
        int active = ok ? httpd_upload_progress(&done, &total, nm, sizeof nm) : 0;
        if (active) { if (done != lastdone) { lastdone = done; redraw = 1; } wasactive = 1; }
        else if (wasactive) { wasactive = 0; lastdone = -1; redraw = 1; }   /* just finished */
        if (redraw) {
            ui_begin(GFX_BOTTOM);
            ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);
            ui_text_center(UI_W / 2, 12, 2, UI_NEON, "UPLOAD");
            ui_glow_round(28, 40, UI_W - 56, 2, 1, UI_NEON, 3, 30);
            ui_fill_round(28, 40, UI_W - 56, 2, 1, UI_NEON);
            if (!ok) {
                ui_text_center(UI_W / 2, 92, 1, UI_RED, "Could not start server.");
                ui_text_center(UI_W / 2, 110, 1, UI_DIM, "Is Wi-Fi connected?");
            } else if (active) {
                char sub[160]; snprintf(sub, sizeof sub, "Receiving: %s", nm);
                ui_text_fit(UI_W / 2, 74, 1, UI_NEONC, sub, UI_W - 16);
                int bx = 20, bw = UI_W - 40, by = 104, bh = 16;
                ui_fill_round(bx, by, bw, bh, bh / 2, TH_TRACK);
                int fw = total > 0 ? (int)((long long)bw * done / total) : 0;
                if (fw > 0) ui_fill_round(bx, by, fw, bh, bh / 2, UI_NEON);
                int pct = total > 0 ? (int)((long long)done * 100 / total) : 0;
                char pl[48];
                if (total >= 1048576) snprintf(pl, sizeof pl, "%ld / %ld MB   %d%%", done / 1048576, total / 1048576, pct);
                else                  snprintf(pl, sizeof pl, "%ld / %ld KB   %d%%", done / 1024, total / 1024, pct);
                ui_text_center(UI_W / 2, by + bh + 8, 1, UI_INK, pl);
            } else {
                ui_text_center(UI_W / 2, 62, 1, UI_INK, "Server ON. On the same Wi-Fi, open:");
                ui_text_center_fit(UI_W / 2, 84, 2, UI_NEONC, httpd_url(), UI_W - 16);
                ui_text_center(UI_W / 2, 118, 1, UI_DIM, "Upload any files from the page.");
                ui_text_center(UI_W / 2, 134, 1, UI_DIM, "Browse folders + delete there too.");
            }
            ui_present();
            redraw = 0;
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
    if (hidKeysHeld() & KEY_TOUCH) { touchPosition tp; hidTouchRead(&tp);   /* tap CANCEL */
        if (tp.py >= 206 && tp.py < 234 && tp.px >= 110 && tp.px < 210) return false; }
    u64 now = osGetTime();
    if (now - g_last_prog >= 100) {
        g_last_prog = now;
        ui_begin(GFX_BOTTOM);
        ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);
        ui_text_center(UI_W / 2, 12, 2, UI_NEON, "DOWNLOADING");
        ui_glow_round(28, 40, UI_W - 56, 2, 1, UI_NEON, 3, 30);
        ui_fill_round(28, 40, UI_W - 56, 2, 1, UI_NEON);
        ui_text_fit(UI_W / 2, 68, 1, UI_NEONC, g_dl_name, UI_W - 16);
        int bx = 20, bw = UI_W - 40, by = 104, bh = 16;
        ui_fill_round(bx, by, bw, bh, bh / 2, TH_TRACK);
        char pl[48];
        if (t) {
            int fw = (int)((long long)bw * d / t); if (fw > 0) ui_fill_round(bx, by, fw, bh, bh / 2, UI_NEON);
            int pct = (int)((u64)d * 100 / t);
            if (t >= 1048576) snprintf(pl, sizeof pl, "%lu / %lu MB   %d%%", (unsigned long)(d / 1048576), (unsigned long)(t / 1048576), pct);
            else              snprintf(pl, sizeof pl, "%lu / %lu KB   %d%%", (unsigned long)(d / 1024), (unsigned long)(t / 1024), pct);
        } else snprintf(pl, sizeof pl, "%lu KB", (unsigned long)(d / 1024));
        ui_text_center(UI_W / 2, by + bh + 8, 1, UI_INK, pl);
        ui_button(110, 206, 100, 28, "CANCEL", 0, UI_RED);
        ui_present();
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
    snprintf(sources[2].name, 64, "3DSmovies Archive");
    snprintf(sources[2].url, CAT_URLLEN,
             "https://drive.google.com/uc?export=download&id=1I0OO_f_7pZxBbG249ivEufUV6vJ2oXC-");
    sources[2].dl_base[0] = 0;   /* items carry their own (archive.org / Drive) URL */
    sources[2].art_base[0] = 0;  /* items carry full artwork URLs */
    sources[2].kind = 1;         /* items[] shape (auto-detected anyway) */
    nsources = 3;
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

/* System keyboard: 1 on OK with non-empty text, 0 on cancel/empty. */
static int kbd_text(const char *hint, char *out, size_t cap) {
    SwkbdState s;
    swkbdInit(&s, SWKBD_TYPE_NORMAL, 2, -1);
    swkbdSetHintText(&s, hint);
    out[0] = 0;
    if (swkbdInputText(&s, out, cap) != SWKBD_BUTTON_RIGHT) return 0;
    return out[0] != 0;
}

/* case-insensitive substring (for search) */
static int ci_contains(const char *hay, const char *needle) {
    if (!needle[0]) return 1;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return 1;
    }
    return 0;
}

/* Remove a user-added source (index >= 3; the built-in Clownsec/Zackk/3DSmovies stay). */
#define BUILTIN_SOURCES 3
static void remove_source(int idx) {
    if (idx < BUILTIN_SOURCES || idx >= nsources) return;
    FILE *f = fopen(SOURCES_FILE, "wb");
    if (f) {
        for (int i = BUILTIN_SOURCES; i < nsources; i++) {
            if (i == idx) continue;
            Source *s = &sources[i];
            fprintf(f, "%s|%s|%s|%s|%d\n", s->name, s->url, s->dl_base, s->art_base, s->kind);
        }
        fclose(f);
    }
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
        u32 k = hidKeysDown(), kh = hidKeysHeld(), ku = hidKeysUp();
        static int td = 0, tx0 = 0, ty0 = 0, tsc0 = 0, tmv = 0;
        const int VIS = 5, ry0 = 54, rh = 28, gap = 4;
        if (nav_repeat(k, kh, NAV_DOWN, &hd)) { if (psel < total - 1) psel++; redraw = 1; }
        if (nav_repeat(k, kh, NAV_UP,   &hu)) { if (psel > 0) psel--;         redraw = 1; }
        if (k & KEY_B) return 0;
        int newf = (k & KEY_X) ? 1 : 0, act = (k & KEY_A) ? psel : -1;
        touchPosition tp; hidTouchRead(&tp);
        if (k & KEY_TOUCH) { td = 1; tx0 = tp.px; ty0 = tp.py; tsc0 = pscroll; tmv = 0; }
        else if ((kh & KEY_TOUCH) && td) { int dy = tp.py - ty0; if (dy > 6 || dy < -6) tmv = 1;
            if (tmv && total > VIS) { int maxs = total - VIS, ns = tsc0 - dy / (rh + gap);
                if (ns < 0) ns = 0; if (ns > maxs) ns = maxs; if (ns != pscroll) { pscroll = ns; redraw = 1; } } }
        else if ((ku & KEY_TOUCH) && td) { td = 0;
            if (!tmv) {
                if (ty0 >= 210 && ty0 < 236) { if (tx0 < 100) act = 0; else if (tx0 < 206) newf = 1; else return 0; }
                else if (ty0 >= ry0) { int i = pscroll + (ty0 - ry0) / (rh + gap);
                    if (i >= 0 && i < total && i < pscroll + VIS) { psel = i; act = i; } }
            }
        }
        if (newf) {                                /* create a new folder here */
            char nm[NAMELEN] = "";
            SwkbdState s; swkbdInit(&s, SWKBD_TYPE_NORMAL, 2, -1);
            swkbdSetHintText(&s, "New folder name");
            if (swkbdInputText(&s, nm, sizeof(nm)) == SWKBD_BUTTON_RIGHT && nm[0]) {
                char np[PATHLEN]; snprintf(np, sizeof(np), "%s%s", dir, nm);
                mkdir(np, 0777); rescan = 1;
            }
        }
        if (act >= 0) {
            if (act == 0) { snprintf(out, cap, "%s", dir); return 1; }    /* save here */
            else if (!is_root && act == 1) {                             /* go up */
                size_t L = strlen(dir); if (L && dir[L - 1] == '/') dir[L - 1] = 0;
                char *sl = strrchr(dir, '/'); if (sl) sl[1] = 0;
                psel = 0; rescan = 1;
            } else {                                                     /* enter folder */
                int fi = act - base;
                if (fi >= 0 && fi < nd) {
                    char nx[PATHLEN]; snprintf(nx, sizeof(nx), "%s%s/", dir, list[fi]);
                    snprintf(dir, sizeof(dir), "%s", nx); psel = 0; rescan = 1;
                }
            }
        }
        if (psel < pscroll) pscroll = psel; if (psel >= pscroll + VIS) pscroll = psel - VIS + 1;
        if (redraw) {
            ui_begin(GFX_BOTTOM);
            ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);
            ui_text_center(UI_W / 2, 12, 2, UI_NEON, "SAVE TO");
            char pth[64]; int mc = (UI_W - 20) / 8;
            if ((int)strlen(dir) > mc) snprintf(pth, sizeof pth, "...%s", dir + strlen(dir) - (mc - 3));
            else snprintf(pth, sizeof pth, "%s", dir);
            ui_text(10, 34, 1, UI_DIM, pth);
            ui_fill_round(8, 46, UI_W - 16, 1, 0, TH_LINE);
            for (int r = 0; r < VIS; r++) { int i = pscroll + r; if (i >= total) break;
                int by = ry0 + r * (rh + gap), selrow = (i == psel), rw = UI_W - 16;
                if (selrow) { ui_glow_round(8, by, rw, rh, 7, UI_NEON, 3, 16);
                    ui_vgrad_round(8, by, rw, rh, 7, TH_SEL, TH_SELLO);
                    ui_frame_round(8, by, rw, rh, 7, UI_NEON, 1); }
                char lb[NAMELEN]; int textx = 18; u16 tc = selrow ? UI_WHITE : UI_INK;
                if (i == 0) { snprintf(lb, sizeof lb, "Save in this folder"); tc = UI_NEON; }
                else if (!is_root && i == 1) snprintf(lb, sizeof lb, ".. (up a folder)");
                else { snprintf(lb, sizeof lb, "%s", list[i - base]); icon_folder(18, by + (rh - 16) / 2, UI_RGB(240, 205, 110)); textx = 46; }
                ui_text(textx, by + (rh - 8) / 2, 1, tc, lb);
            }
            if (total > VIS) { int th = VIS * (rh + gap) - gap, ty = ry0, maxs = total - VIS;
                int hh = th * VIS / total; if (hh < 12) hh = 12; int hy = ty + (th - hh) * pscroll / maxs;
                ui_fill_round(UI_W - 6, ty, 3, th, 1, TH_TRACK);
                ui_fill_round(UI_W - 6, hy, 3, hh, 1, UI_NEON); }
            ui_button(8, 210, 90, 26, "SAVE", 0, UI_NEON);
            ui_button(102, 210, 100, 26, "NEW +", 0, UI_NEONC);
            ui_button(206, 210, 106, 26, "Back", 0, UI_NEONP);
            ui_present();
            redraw = 0;
        }
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    }
    return 0;
}

/* greedy word-wrap helper for the top-screen info panel */
static void ui_text_wrap(int x, int *y, int scale, u16 col, const char *s, int maxcw, int maxlines) {
    char buf[160];   /* holds up to maxcw codepoints (UTF-8: several bytes each) */
    int lines = 0;
    while (*s && lines < maxlines) {
        while (*s == ' ') s++;
        if (!*s) break;
        const char *p = s; int cp = 0, lastsp = -1;      /* lastsp = byte offset of the last space */
        while (*p && cp < maxcw) {
            if (*p == ' ') lastsp = (int)(p - s);
            p += ui_u8_bytes(p, 1);                       /* advance one whole codepoint */
            cp++;
        }
        int cut = (*p && lastsp > 0) ? lastsp : (int)(p - s);   /* break at the last space if the line is full */
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

/* If an earlier download of `url` was interrupted, a central partial remains (keyed by URL, so
 * it's found even if you now pick a different folder). Ask whether to resume it or start over.
 * Returns 1 to proceed (download_to_file resumes any kept partial). */
static int download_resume_check(const char *url) {
    long long have = download_partial_bytes(url);
    if (have <= 0) return 1;                       /* nothing partial -> fresh download */
    char sz[24]; fmt_size(have, sz, sizeof sz);
    char msg[96];
    snprintf(msg, sizeof msg, "Unfinished download found\n(%s already saved).", sz);
    int c = prompt2("DOWNLOAD", msg, "RESUME", "RESTART");   /* d-pad + A; B backs out */
    if (c < 0) return 0;                                     /* B -> don't download at all */
    if (c == 1) download_discard_partial(url);               /* RESTART: drop the partial */
    return 1;
}

/* download failed/cancelled: partial kept for resume; offer deleting it instead (B = keep + back) */
static void download_cancel_msg(const char *url) {
    if (prompt2("DOWNLOAD", "Cancelled -- progress saved.\nStart it again to resume.", "OK", "DELETE FILE") == 1)
        download_discard_partial(url);
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
    ui_clear(UI_BG);
    int y = 8;
    ui_text_wrap(10, &y, 2, UI_WHITE, e->name, 24, 2);       /* title (16px, ~24/line) */

    /* poster box on the left */
    int px0 = 10, py0 = 48;
    if (poster) {
        for (int j = 0; j < POSTER_H; j++)
            for (int i = 0; i < POSTER_W; i++)
                ui_px(px0 + i, py0 + j, poster[j * POSTER_W + i]);
    } else {
        ui_fill(px0, py0, POSTER_W, POSTER_H, UI_BG2);
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
    if (meta[0]) { ui_text(tx, ty, 1, UI_NEONC, meta); ty += 14; }
    if (sz[0])   { ui_text(tx, ty, 1, UI_NEON, sz);   ty += 14; }
    {   /* stereoscopic flag (also shown for TV/zip entries) */
        int is3d = (e->is3d >= 0) ? e->is3d : cat_is_3d(e);   /* catalog/.nfo flag, else the filename */
        ui_text(tx, ty, 1, is3d ? UI_NEONP : UI_DIM,
                is3d ? "3D  (stereoscopic)" : "2D"); ty += 14;
    }
    if (e->genres[0]) { ui_text_wrap(tx, &ty, 1, UI_GRAY, e->genres, 30, 3); ty += 4; }
    if (e->desc[0])   ui_text_wrap(tx, &ty, 1, UI_INK, e->desc, 30, 12);
    ui_present();
}

/* Top-screen panel for a highlighted folder: a simple drawn folder icon + its name. */
static void draw_folder_top(const char *name) {
    ui_begin(GFX_TOP);
    ui_clear(UI_BG);
    int cx = UI_TW / 2, iy = 70, w = 108, h = 78, x = cx - w / 2;
    u16 tab = UI_RGB(210, 180, 90), body = UI_RGB(240, 205, 110);
    ui_fill(x, iy - 14, 46, 16, tab);                 /* folder tab */
    ui_fill(x, iy, w, h, body);                       /* folder body */
    ui_fill(x + 4, iy + 4, w - 8, h - 8, UI_RGB(250, 224, 150));   /* inner highlight */
    ui_text_center(cx, iy + h + 16, 1, UI_GRAY, "folder");
    char nm[48]; snprintf(nm, sizeof nm, "%.46s", name);
    ui_text_center(cx, iy + h + 32, 1, UI_WHITE, nm);
    ui_present();
}

/* The browser's top screen for the highlighted entry: folder icon, movie info panel (poster +
 * metadata from moviedata/), or the flat CLOWNSEC logo when a movie has no metadata. All 2D. */
static u16 g_top_poster[POSTER_W * POSTER_H];
static void browser_show_top(int is_dir, const char *name, const char *fullpath) {
    gfxSet3D(false);
    if (is_dir) { draw_folder_top(name); return; }
    CatEntry meta;
    int haveinfo   = movieinfo_load(fullpath, &meta);
    int haveposter = movieinfo_poster(fullpath, g_top_poster, POSTER_W, POSTER_H);
    if (!haveinfo && !haveposter) { branding_show_2d(); return; }   /* no metadata -> logo */
    if (!haveinfo) {   /* poster only: build a minimal entry from the filename */
        memset(&meta, 0, sizeof meta);
        meta.is3d = -1;                                           /* -> fall back to the filename */
        snprintf(meta.name,  sizeof meta.name,  "%s", name);
        char *dot = strrchr(meta.name, '.'); if (dot) *dot = 0;   /* drop extension for the title */
        snprintf(meta.fname, sizeof meta.fname, "%s", name);      /* keeps the 2D/3D badge */
    }
    draw_info_top(&meta, haveposter ? g_top_poster : NULL);
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

/* Google Drive (and similar) serve files with no extension in the name; give the saved file the
 * right extension by sniffing its magic bytes so the browser recognizes and plays it. */
static void fix_download_ext(char *dest, size_t cap) {
    size_t L = strlen(dest);
    if ((L > 7 && !strcasecmp(dest + L - 7, ".moflex")) ||
        (L > 4 && !strcasecmp(dest + L - 4, ".cia"))    ||
        (L > 4 && !strcasecmp(dest + L - 4, ".zip"))) return;   /* already typed */
    FILE *f = fopen(dest, "rb"); if (!f) return;
    unsigned char b[4] = {0}; size_t r = fread(b, 1, 4, f); fclose(f);
    if (r < 4) return;
    const char *ext = ".moflex";
    if (b[0] == 0x20 && b[1] == 0x20 && b[2] == 0 && b[3] == 0) ext = ".cia";
    else if (b[0] == 'P' && b[1] == 'K' && b[2] == 3 && b[3] == 4) ext = ".zip";
    char nd[PATHLEN + NAMELEN]; snprintf(nd, sizeof nd, "%s%s", dest, ext);
    if (rename(dest, nd) == 0) snprintf(dest, cap, "%s", nd);
}

/* ---- catalog category / genre / sort helpers ---- */
static const CatEntry *g_scat; static int g_smode;   /* 0 = title, 1 = year, 2 = date added */
static int idx_cmp(const void *a, const void *b) {
    const CatEntry *x = &g_scat[*(const int *)a], *y = &g_scat[*(const int *)b];
    if (g_smode == 1) { if (x->year != y->year) return y->year - x->year; }       /* newest year first */
    else if (g_smode == 2) { int c = strcmp(y->date, x->date); if (c) return c; }  /* newest date first */
    return strcasecmp(x->name, y->name);                                           /* title A-Z (+ tiebreak) */
}
static int strrow_cmp(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }
static int distinct_categories(const CatEntry *cat, int nc, char out[][32], int max) {
    int n = 0;
    for (int i = 0; i < nc; i++) {
        if (!cat[i].category[0]) continue;
        int f = 0; for (int j = 0; j < n; j++) if (!strcasecmp(out[j], cat[i].category)) { f = 1; break; }
        if (!f && n < max) snprintf(out[n++], 32, "%s", cat[i].category);
    }
    return n;
}
static int genre_match(const char *genres, const char *want) {
    if (!want || !want[0]) return 1;
    for (const char *g = genres; *g; ) {
        while (*g == ' ' || *g == ',') g++;
        char tok[32]; int t = 0; while (*g && *g != ',' && t < 31) tok[t++] = *g++;
        while (t > 0 && tok[t - 1] == ' ') t--; tok[t] = 0;
        if (t && !strcasecmp(tok, want)) return 1;
    }
    return 0;
}
static int distinct_genres(const CatEntry *cat, int nc, const char *category, char out[][32], int max) {
    int n = 0;
    for (int i = 0; i < nc && n < max; i++) {
        if (category[0] && strcasecmp(cat[i].category, category)) continue;
        for (const char *g = cat[i].genres; *g && n < max; ) {
            while (*g == ' ' || *g == ',') g++;
            if (!*g) break;
            char tok[32]; int t = 0; while (*g && *g != ',' && t < 31) tok[t++] = *g++;
            while (t > 0 && tok[t - 1] == ' ') t--; tok[t] = 0;
            if (t) { int f = 0; for (int j = 0; j < n; j++) if (!strcasecmp(out[j], tok)) { f = 1; break; }
                if (!f && n < max) snprintf(out[n++], 32, "%s", tok); }
        }
    }
    return n;
}
/* neon scrolling picker for a list of short strings. Returns index, -2 (Show All), or -1 (back). */
static int catalog_pick(const char *title, const char *subtitle, char items[][32], int n, int show_all, const char *extra) {
    int has_extra = (extra && extra[0]) ? 1 : 0;
    int total = n + (show_all ? 1 : 0) + has_extra;
    int sel = 0, top = 0, redraw = 1, td = 0, tx0 = 0, ty0 = 0, tsc0 = 0, tmv = 0;
    const int VIS = 5, ry0 = 54, rh = 30, gap = 4;
    while (aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown(), kh = hidKeysHeld(), ku = hidKeysUp();
        if (k & KEY_B) return -1;
        if (k & KEY_X) return -4;   /* search (callers that don't search treat it as back) */
        if (k & KEY_DOWN) { if (sel < total - 1) sel++; redraw = 1; }
        if (k & KEY_UP)   { if (sel > 0) sel--; redraw = 1; }
        int act = (k & KEY_A) ? sel : -1;
        touchPosition tp; hidTouchRead(&tp);
        if (k & KEY_TOUCH) { td = 1; tx0 = tp.px; ty0 = tp.py; tsc0 = top; tmv = 0; }
        else if ((kh & KEY_TOUCH) && td) { int dy = tp.py - ty0; if (dy > 6 || dy < -6) tmv = 1;
            if (tmv && total > VIS) { int maxs = total - VIS, ns = tsc0 - dy / (rh + gap);
                if (ns < 0) ns = 0; if (ns > maxs) ns = maxs; if (ns != top) { top = ns; redraw = 1; } } }
        else if ((ku & KEY_TOUCH) && td) { td = 0; if (!tmv) for (int r = 0; r < VIS; r++) { int i = top + r; if (i >= total) break;
            int by = ry0 + r * (rh + gap); if (ty0 >= by && ty0 < by + rh) { sel = i; act = i; break; } } }
        (void)tx0;
        if (act >= 0) {
            if (has_extra && act == total - 1) return -3;        /* the extra (e.g. Rescan) item */
            if (show_all && act == 0) return -2;
            return act - (show_all ? 1 : 0);
        }
        if (sel < top) top = sel; if (sel >= top + VIS) top = sel - VIS + 1;
        if (redraw) {
            ui_begin(GFX_BOTTOM);
            ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);
            ui_text_center(UI_W / 2, 12, 2, UI_NEON, title);
            if (subtitle && subtitle[0]) ui_text_fit(UI_W / 2, 34, 1, UI_NEONP, subtitle, UI_W - 16);
            ui_glow_round(28, 48, UI_W - 56, 2, 1, UI_NEON, 3, 30); ui_fill_round(28, 48, UI_W - 56, 2, 1, UI_NEON);
            for (int r = 0; r < VIS; r++) { int i = top + r; if (i >= total) break;
                int by = ry0 + r * (rh + gap), selrow = (i == sel), rw = UI_W - 16;
                if (selrow) { ui_glow_round(8, by, rw, rh, 7, UI_NEON, 3, 16);
                    ui_vgrad_round(8, by, rw, rh, 7, TH_SEL, TH_SELLO);
                    ui_frame_round(8, by, rw, rh, 7, UI_NEON, 1); }
                int all = (show_all && i == 0), ex = (has_extra && i == total - 1);
                const char *lbl = all ? "* Show All" : ex ? extra : items[i - (show_all ? 1 : 0)];
                ui_text_fit(UI_W / 2, by + (rh - 8) / 2, 1, selrow ? UI_WHITE : (all || ex ? UI_NEON : UI_INK), lbl, rw - 16);
            }
            if (total > VIS) { int th = VIS * (rh + gap) - gap, ty = ry0, maxs = total - VIS;
                int hh = th * VIS / total; if (hh < 12) hh = 12; int hy = ty + (th - hh) * top / maxs;
                ui_fill_round(UI_W - 6, ty, 3, th, 1, TH_TRACK);
                ui_fill_round(UI_W - 6, hy, 3, hh, 1, UI_NEON); }
            ui_text(8, 226, 1, UI_DIM, "X - search");   /* left-aligned so it can't read as a row */
            ui_present(); redraw = 0;
        }
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    }
    return -1;
}

static void lib_add_downloaded(const char *path, const CatEntry *src);   /* defined with the library code */

static void catalog_browse(const Source *src) {
    ui_begin(GFX_BOTTOM); ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);
    ui_text_center(UI_W / 2, 100, 2, UI_NEON, "Fetching...");
    ui_text_fit(UI_W / 2, 130, 1, UI_DIM, src->name, UI_W - 16); ui_present();
    gfxFlushBuffers(); gfxSwapBuffers();

    char *json = NULL; size_t len = 0;
    if (!download_to_mem(src->url, &json, &len, 32 * 1024 * 1024)) {
        msg_screen("DOWNLOAD", "Fetch failed.\nIs Wi-Fi on? Is the URL right?"); return;
    }
    int cap = 4096;
    CatEntry *cat = (CatEntry *)malloc(sizeof(CatEntry) * cap);
    int nc = cat ? catalog_parse(json, src->kind, src->dl_base, src->art_base, cat, cap) : 0;
    free(json);
    if (nc <= 0) {
        msg_screen("DOWNLOAD", "No movies found here.\nThe URL must be a catalog.json file\n(Clownsec/Zackk format), not a web page.");
        free(cat); return;
    }
    gfxSet3D(false);   /* top screen becomes a 2D info panel while browsing */
    u16 *poster  = (u16 *)malloc((size_t)POSTER_W * POSTER_H * sizeof(u16));
    u16 *pworker = (u16 *)malloc((size_t)POSTER_W * POSTER_H * sizeof(u16));
    if (pworker) pw_start(pworker);   /* background loader -> scrolling never blocks on a poster */
    static char cats[24][32]; int ncat = distinct_categories(cat, nc, cats, 24);
    qsort(cats, ncat, 32, strrow_cmp);   /* categories alphabetical */
    int *idx = (int *)malloc(sizeof(int) * nc);
    int sortmode = 0;

    while (idx && aptMainLoop()) {
        /* ---- navigation: pick a category (+ optional genre), Show All, or X = search ---- */
        char filt_cat[32] = "", filt_genre[32] = "", filt_search[64] = "";
        if (ncat > 1) {
            static char cdisp[24][32];   /* category rows with counts (selection uses cats[]) */
            for (int k = 0; k < ncat; k++) {
                int n = 0;
                for (int i = 0; i < nc; i++) if (!strcasecmp(cat[i].category, cats[k])) n++;
                snprintf(cdisp[k], 32, "%.24s (%d)", cats[k], n);
            }
            int c = catalog_pick("CATEGORY", src->name, cdisp, ncat, 1, NULL);
            if (c == -1) break;
            if (c == -4) { if (!kbd_text("Search this catalog", filt_search, sizeof filt_search)) continue; }
            else if (c >= 0) {
                snprintf(filt_cat, sizeof filt_cat, "%s", cats[c]);
                static char gens[64][32]; int ng = distinct_genres(cat, nc, filt_cat, gens, 64);
                qsort(gens, ng, 32, strrow_cmp);   /* genres alphabetical */
                if (ng > 0) {
                    static char gdisp[64][32];   /* genre rows with counts (within this category) */
                    for (int k = 0; k < ng; k++) {
                        int n = 0;
                        for (int i = 0; i < nc; i++)
                            if (!strcasecmp(cat[i].category, filt_cat) && genre_match(cat[i].genres, gens[k])) n++;
                        snprintf(gdisp[k], 32, "%.24s (%d)", gens[k], n);
                    }
                    const char *mi[2] = { "View All", "Pick a Genre" };
                    int mm = ui_menu(filt_cat, NULL, mi, 2);
                    if (mm < 0) continue;
                    if (mm == 1) { int g = catalog_pick("GENRE", filt_cat, gdisp, ng, 0, NULL);
                                   if (g < 0) continue; snprintf(filt_genre, sizeof filt_genre, "%s", gens[g]); }
                }
            }
        }
        /* ---- build the filtered + sorted index over cat[] ---- */
cb_rebuild:;   /* X-search inside the list jumps back here with filt_search set */
        int ni = 0;
        for (int i = 0; i < nc; i++) {
            if (filt_cat[0] && strcasecmp(cat[i].category, filt_cat)) continue;
            if (filt_genre[0] && !genre_match(cat[i].genres, filt_genre)) continue;
            if (filt_search[0] && !ci_contains(cat[i].name, filt_search)) continue;   /* search box */
            idx[ni++] = i;
        }
        if (ni == 0) { msg_screen("CATALOG", filt_search[0] ? "No videos match that search." : "Nothing here.");
                       if (filt_search[0]) { filt_search[0] = 0; goto cb_rebuild; }   /* back to unfiltered */
                       if (ncat > 1) continue; else break; }
        g_scat = cat; g_smode = sortmode; qsort(idx, ni, sizeof(int), idx_cmp);

        int csel = 0, cscroll = 0, redraw = 1, hfu = 0, hfd = 0, requested = 0;
        int shown = -1, phave = 0, settle = 0;
        int td = 0, tx0 = 0, ty0 = 0, tsc0 = 0, tmv = 0, tbar = 0, leave = 0;
    while (aptMainLoop() && !leave) {
        hidScanInput();
        u32 k = hidKeysDown(), kh = hidKeysHeld(), ku = hidKeysUp();
        if (k & KEY_B) { leave = (ncat > 1) ? 1 : 2; break; }
        int want_dl = 0;
        if (nav_repeat(k, kh, NAV_DOWN, &hfd)) { if (csel < ni - 1) csel++; redraw = 1; }
        if (nav_repeat(k, kh, NAV_UP, &hfu))   { if (csel > 0) csel--; redraw = 1; }
        if (k & KEY_RIGHT) { csel += BR_ROWS; if (csel > ni - 1) csel = ni - 1; redraw = 1; }
        if (k & KEY_LEFT)  { csel -= BR_ROWS; if (csel < 0) csel = 0; redraw = 1; }
        if (k & KEY_X) {   /* search WITHIN this view (whatever category/genre is filtered) */
            char q[64];
            if (kbd_text("Search", q, sizeof q)) { snprintf(filt_search, sizeof filt_search, "%s", q); goto cb_rebuild; }
            redraw = 1;
        }
        if (k & KEY_R) {   /* jump to next first-letter group */
            char c = firstc(cat[idx[csel]].name); int i = csel;
            while (i < ni && firstc(cat[idx[i]].name) == c) i++;
            if (i < ni) csel = i; redraw = 1;
        }
        if (k & KEY_L) {   /* jump to start of this letter group, or previous */
            int i = csel; char c = firstc(cat[idx[i]].name);
            while (i > 0 && firstc(cat[idx[i - 1]].name) == c) i--;
            if (i == csel && i > 0) { i--; char p = firstc(cat[idx[i]].name);
                while (i > 0 && firstc(cat[idx[i - 1]].name) == p) i--; }
            csel = i; redraw = 1;
        }
        if (k & KEY_A) want_dl = 1;
        if (k & KEY_Y) { sortmode = (sortmode + 1) % 3; g_smode = sortmode;   /* cycle sort */
            qsort(idx, ni, sizeof(int), idx_cmp); csel = 0; cscroll = 0; shown = -1; redraw = 1; }
        if (csel < cscroll) cscroll = csel;
        if (csel >= cscroll + BR_ROWS) cscroll = csel - BR_ROWS + 1;
        /* touch: list drag, scrollbar drag, tap a row, DOWNLOAD / Back buttons */
        touchPosition tp; hidTouchRead(&tp);
        if (k & KEY_TOUCH) { td = 1; tx0 = tp.px; ty0 = tp.py; tsc0 = cscroll; tmv = 0; tbar = (tp.px >= UI_W - 14 && tp.py >= BR_LIST_Y && tp.py < 206); }
        else if ((kh & KEY_TOUCH) && td) {
            int maxs = ni > BR_ROWS ? ni - BR_ROWS : 0;
            if (tbar) {   /* scrollbar: proportional jump through the whole list */
                int th = BR_ROWS * BR_ROWH - 6, rel = tp.py - BR_LIST_Y; if (rel < 0) rel = 0; if (rel > th) rel = th;
                int ns = th ? maxs * rel / th : 0; if (ns != cscroll) { cscroll = ns; csel = cscroll; redraw = 1; } tmv = 1;
            } else { int dy = tp.py - ty0; if (dy > 6 || dy < -6) tmv = 1;
                if (tmv && maxs) { int ns = tsc0 - dy / BR_ROWH; if (ns < 0) ns = 0; if (ns > maxs) ns = maxs;
                    if (ns != cscroll) { cscroll = ns; redraw = 1; } } }
        } else if ((ku & KEY_TOUCH) && td) { td = 0;
            if (!tmv) {
                if (ty0 >= 210 && ty0 < 236) {           /* DOWNLOAD / SORT / Back */
                    if (tx0 < 104) want_dl = 1;
                    else if (tx0 < 208) { sortmode = (sortmode + 1) % 3; g_smode = sortmode;
                        qsort(idx, ni, sizeof(int), idx_cmp); csel = 0; cscroll = 0; shown = -1; redraw = 1; }
                    else leave = (ncat > 1) ? 1 : 2;
                } else if (ty0 >= BR_LIST_Y) { int i = cscroll + (ty0 - BR_LIST_Y) / BR_ROWH;
                    if (i >= 0 && i < ni && i < cscroll + BR_ROWS) { csel = i; redraw = 1; } }
            }
        }
        if (leave) break;
        if (want_dl) {
            CatEntry *e = &cat[idx[csel]];
            char destdir[PATHLEN];
            if (!pick_folder(destdir, sizeof(destdir))) { redraw = 1; goto cont; }   /* cancelled */
            char dest[PATHLEN + NAMELEN];
            snprintf(dest, sizeof(dest), "%s%s", destdir, e->fname);
            snprintf(g_dl_name, sizeof(g_dl_name), "%s", e->fname);
            g_last_prog = 0;
            if (!download_resume_check(e->url)) { redraw = 1; goto cont; }   /* B on the prompt -> back */
            bool ok = download_to_file(e->url, dest, dl_progress, NULL);
            if (ok && !e->is_zip) {   /* persist metadata + poster so the info panel shows for the local file */
                fix_download_ext(dest, sizeof dest);   /* Drive items have no extension -> sniff + add it */
                if (!phave && e->art[0] && poster) phave = poster_get(e->art, e->fname, poster, POSTER_W, POSTER_H);
                movieinfo_save(dest, e, phave ? poster : NULL, POSTER_W, POSTER_H);
                lib_add_downloaded(dest, e);   /* show it in the library right away (info + poster already saved) */
            }
            if (ok && e->is_zip && !file_is_zip(dest)) {
                remove(dest);   /* not a real zip (likely a 404 page) */
                msg_screen("DOWNLOAD", "Download failed: the server did\nnot return a zip (not found).");
            } else if (ok && e->is_zip) {
                if (confirm("Extract the zip now?\n(No = keep the .zip file)")) {
                    char stem[NAMELEN]; snprintf(stem, sizeof(stem), "%s", e->fname);
                    size_t sl = strlen(stem); if (sl > 4) stem[sl - 4] = 0;   /* drop .zip */
                    char folder[PATHLEN + NAMELEN];
                    snprintf(folder, sizeof(folder), "%s%s", destdir, stem);
                    ui_begin(GFX_BOTTOM); ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);
                    ui_text_center(UI_W / 2, 100, 2, UI_NEON, "Extracting...");
                    ui_text_fit(UI_W / 2, 130, 1, UI_DIM, stem, UI_W - 16); ui_present();
                    gfxFlushBuffers(); gfxSwapBuffers();
                    int nf = unzip_to_dir(dest, folder);
                    remove(dest);   /* delete the .zip after extracting */
                    char m[128];
                    if (nf > 0) snprintf(m, sizeof m, "Extracted %d file%s into:\n%s", nf, nf == 1 ? "" : "s", stem);
                    else        snprintf(m, sizeof m, "Extract FAILED.\n%s", stem);
                    msg_screen("DOWNLOAD", m);
                } else {
                    char m[128]; snprintf(m, sizeof m, "Zip saved (not extracted):\n%s", e->fname);
                    msg_screen("DOWNLOAD", m);
                }
            } else if (ok) {
                char m[160]; snprintf(m, sizeof m, "%s\nDownload complete.", g_dl_name);
                msg_screen("DOWNLOAD", m);
            } else download_cancel_msg(e->url);   /* progress saved; offers DELETE FILE, B keeps it */
            redraw = 1;
        }
    cont:
        if (csel != shown) { redraw = 1; phave = 0; settle = 0; requested = 0; }   /* moved -> drop poster */
        /* debounced request to the background loader, then poll -- scrolling never blocks on a poster */
        if (!phave && !requested && cat[idx[csel]].art[0] && ++settle >= 6) { pw_request(idx[csel], cat[idx[csel]].art, cat[idx[csel]].fname); requested = 1; }
        if (!phave && requested && pw_done == idx[csel]) {
            if (pw_ok && poster && pworker) { memcpy(poster, pworker, (size_t)POSTER_W * POSTER_H * 2); phave = 1; }
            requested = 0; redraw = 1;
        }
        /* marquee the selected long title (reveal, then reset) */
        { int nr = 0;
          if (ni && csel >= cscroll && csel < cscroll + BR_ROWS)
              marquee_off(2000000 + csel, ui_text_w(1, cat[idx[csel]].name), (UI_W - 16) - 46, &nr);
          else marquee_off(-3, 0, 0, &nr);
          if (nr) redraw = 1; }
        if (redraw) {
            ui_begin(GFX_BOTTOM);
            ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);
            ui_text(10, 8, 2, UI_NEON, "CATALOG");
            char hdr[24]; snprintf(hdr, sizeof hdr, "%d / %d", csel + 1, ni);
            ui_text(UI_W - (int)strlen(hdr) * 8 - 10, 12, 1, UI_NEONP, hdr);
            char sub[80]; snprintf(sub, sizeof sub, "%s%s%s", filt_cat[0] ? filt_cat : "All", filt_genre[0] ? " / " : "", filt_genre);
            ui_text_left_fit(10, 30, 1, UI_NEONC, sub, UI_W - 20);
            ui_fill_round(8, 42, UI_W - 16, 1, 0, TH_LINE);
            for (int r = 0; r < BR_ROWS; r++) { int i = cscroll + r; if (i >= ni) break;
                const CatEntry *ce = &cat[idx[i]];
                int ry = BR_LIST_Y + r * BR_ROWH, rw = UI_W - 16, rh = BR_ROWH - 4, selrow = (i == csel);
                if (selrow) { ui_glow_round(8, ry, rw, rh, 7, UI_NEON, 3, 16);
                    ui_vgrad_round(8, ry, rw, rh, 7, TH_SEL, TH_SELLO);
                    ui_frame_round(8, ry, rw, rh, 7, UI_NEON, 1); }
                if (ce->is_zip) icon_folder(18, ry + 3, UI_NEONP); else icon_movie(18, ry + 3, UI_NEONC);
                int tx = 46, txr = UI_W - 16, ty = ry + (rh - 8) / 2, tw = ui_text_w(1, ce->name);
                u16 tc = selrow ? UI_WHITE : UI_INK;
                if (tw <= txr - tx) ui_text(tx, ty, 1, tc, ce->name);
                else if (selrow) ui_text_clipped(tx - g_mq_off, ty, 1, tc, ce->name, tx, txr);
                else { char disp[NAMELEN]; snprintf(disp, sizeof disp, "%s", ce->name);
                    int cm = (txr - tx) / 8; if ((int)strlen(disp) > cm) disp[cm] = 0; ui_text(tx, ty, 1, tc, disp); }
            }
            if (ni > BR_ROWS) { int th = BR_ROWS * BR_ROWH - 6, ty = BR_LIST_Y, maxs = ni - BR_ROWS;
                int hh = th * BR_ROWS / ni; if (hh < 12) hh = 12; int hy = ty + (th - hh) * cscroll / maxs;
                ui_fill_round(UI_W - 6, ty, 3, th, 1, TH_TRACK);
                ui_fill_round(UI_W - 6, hy, 3, hh, 1, UI_NEON); }
            const char *sm = sortmode == 0 ? "Sort: Title" : sortmode == 1 ? "Sort: Year" : "Sort: Date";
            ui_button(8, 210, 96, 26, "DOWNLOAD", 0, UI_NEON);
            ui_button(108, 210, 100, 26, sm, 0, UI_NEONC);
            ui_button(212, 210, 100, 26, "Back", 0, UI_NEONP);
            ui_present();
            draw_info_top(&cat[idx[csel]], phave ? poster : NULL);
            shown = csel; redraw = 0;
        }
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    }
        if (leave == 2) break;   /* B in the list with no category level -> exit the catalog */
    }
    pw_stop();
    branding_show();   /* restore the 3D logo on the top screen */
    free(poster);
    free(pworker);
    free(idx);
    free(cat);
}

/* ================= Library: one flat, categorized view of every local movie ================= */
#define LIB_MAX   3000
#define LIB_CACHE "sdmc:/moflex_player/library.cache"
#define LIB_MAGIC 0x4C494235   /* 'LIB5' -- bump to invalidate old caches when the scan changes */
static CatEntry *g_lib = NULL;   /* every playable movie on the SD, with its .nfo metadata */
static int       g_lib_n = 0;

/* recurse the SD (skipping the hidden system folders), collecting .moflex / movie-.cia files */
/* TV episodes: a catalog match gives every episode of a show the SAME title, so the library showed
 * one name 10x with no way to tell episodes apart. If the filename carries an SxxEyy tag, display
 * the filename instead (it already reads "Show - S01e01 - Episode Title"); the show's poster,
 * category, and genres still come from the info. "(3D)" tags and the extension are stripped. */
static int has_episode_tag(const char *s) {
    for (const char *p = s; *p; p++)
        if ((*p == 's' || *p == 'S') && isdigit((unsigned char)p[1])) {
            const char *q = p + 1; while (isdigit((unsigned char)*q)) q++;
            if ((*q == 'e' || *q == 'E') && isdigit((unsigned char)q[1])) return 1;
        }
    return 0;
}
static void lib_episode_name(CatEntry *c) {
    if (!has_episode_tag(c->fname)) return;
    snprintf(c->name, sizeof c->name, "%s", c->fname);
    strip_ext(c->name);
    char *d = c->name;                       /* drop every "(3D)" tag + collapse the gap it leaves */
    for (char *p = c->name; *p; ) {
        if (p[0] == '(' && p[1] == '3' && (p[2] == 'D' || p[2] == 'd') && p[3] == ')') {
            p += 4;
            if (d > c->name && d[-1] == ' ' && (*p == ' ' || *p == 0)) d--;
            continue;
        }
        *d++ = *p++;
    }
    *d = 0;
    while (d > c->name && d[-1] == ' ') *--d = 0;   /* trim trailing spaces */
}

static void lib_scan_dir(const char *dir, int depth) {
    if (g_lib_n >= LIB_MAX || depth > 8) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && g_lib_n < LIB_MAX) {
        if (e->d_name[0] == '.') continue;
        char full[PATHLEN + NAMELEN];
        snprintf(full, sizeof full, "%s%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st)) continue;
        if (S_ISDIR(st.st_mode)) {
            if (is_hidden_dir(e->d_name)) continue;          /* skip 3DS / Nintendo / luma / etc. */
            char sub[PATHLEN];
            snprintf(sub, sizeof sub, "%s/", full);
            lib_scan_dir(sub, depth + 1);
        } else if (is_moflex(e->d_name)) {
            if (cia_is_cia(e->d_name) && !cia_has_moflex(full)) continue;   /* skip non-movie CIAs */
            CatEntry *c = &g_lib[g_lib_n];
            movieinfo_load(full, c);                          /* name/genres/category/year/is3d from .nfo, if any */
            snprintf(c->url, sizeof c->url, "%s", full);      /* the path we play + the poster key */
            snprintf(c->fname, sizeof c->fname, "%s", e->d_name);
            if (!c->name[0]) { snprintf(c->name, sizeof c->name, "%s", e->d_name); strip_ext(c->name); }
            if (!c->category[0]) snprintf(c->category, sizeof c->category, "Uncategorized");   /* Get Info fills the real one */
            lib_episode_name(c);   /* episodes display their filename, not the shared show title */
            c->is_zip = 0;
            time_t mt = st.st_mtime; struct tm *tmv = localtime(&mt);
            if (tmv) strftime(c->date, sizeof c->date, "%Y-%m-%d", tmv);   /* file date -> "sort by date added" */
            g_lib_n++;
            if ((g_lib_n & 7) == 0) {                          /* live progress */
                ui_begin(GFX_BOTTOM); ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);
                ui_text_center(UI_W / 2, 96, 2, UI_NEON, "Scanning...");
                char m[32]; snprintf(m, sizeof m, "%d found", g_lib_n);
                ui_text_center(UI_W / 2, 128, 1, UI_DIM, m); ui_present();
                gfxFlushBuffers(); gfxSwapBuffers();
            }
        }
    }
    closedir(d);
}

static void lib_save_cache(void) {
    mkdir("sdmc:/moflex_player", 0777);                       /* cache so later opens are instant */
    FILE *f = fopen(LIB_CACHE, "wb");
    if (f) { int magic = LIB_MAGIC; fwrite(&magic, sizeof magic, 1, f);
        fwrite(&g_lib_n, sizeof g_lib_n, 1, f); fwrite(g_lib, sizeof(CatEntry), g_lib_n, f); fclose(f); }
}

static void lib_rescan(void) {
    if (!g_lib) g_lib = (CatEntry *)malloc(sizeof(CatEntry) * LIB_MAX);
    if (!g_lib) return;
    g_lib_n = 0;
    ui_begin(GFX_BOTTOM); ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);
    ui_text_center(UI_W / 2, 96, 2, UI_NEON, "Scanning...");
    ui_text_center(UI_W / 2, 128, 1, UI_DIM, "searching the SD card for movies"); ui_present();
    gfxFlushBuffers(); gfxSwapBuffers();
    lib_scan_dir("sdmc:/", 0);
    lib_save_cache();
}

/* reload a library entry's metadata from its .nfo, preserving the path + file date */
static void lib_refresh_entry(int i) {
    if (i < 0 || i >= g_lib_n) return;
    CatEntry *ce = &g_lib[i];
    char path[CAT_URLLEN]; snprintf(path, sizeof path, "%s", ce->url);
    char date[12];         snprintf(date, sizeof date, "%s", ce->date);
    movieinfo_load(path, ce);
    snprintf(ce->url, sizeof ce->url, "%s", path);
    snprintf(ce->date, sizeof ce->date, "%s", date);
    if (!ce->category[0]) snprintf(ce->category, sizeof ce->category, "Uncategorized");
    lib_episode_name(ce);
    ce->is_zip = 0;
}

/* the category a library entry is shown under: "Moflex" (a format) folds into Uncategorized */
static const char *lib_disp_cat(const CatEntry *e) {
    return (e->category[0] && strcasecmp(e->category, "Moflex")) ? e->category : "Uncategorized";
}
/* distinct display-categories across the library (folding Moflex/empty into Uncategorized) */
static int lib_distinct_categories(char out[][32], int max) {
    int n = 0;
    for (int i = 0; i < g_lib_n && n < max; i++) {
        const char *cc = lib_disp_cat(&g_lib[i]);
        int dup = 0; for (int j = 0; j < n; j++) if (!strcasecmp(out[j], cc)) { dup = 1; break; }
        if (!dup) snprintf(out[n++], 32, "%s", cc);
    }
    return n;
}

static int lib_load_cache(void) {   /* cache-only: movie count, 0 if no valid cache (never scans) */
    if (g_lib_n > 0) return g_lib_n;                          /* already in RAM this session */
    if (!g_lib) g_lib = (CatEntry *)malloc(sizeof(CatEntry) * LIB_MAX);
    if (!g_lib) return 0;
    FILE *f = fopen(LIB_CACHE, "rb");
    if (f) { int magic = 0, n = 0;
        if (fread(&magic, sizeof magic, 1, f) == 1 && magic == LIB_MAGIC &&   /* stale/old cache -> rescan */
            fread(&n, sizeof n, 1, f) == 1 && n > 0 && n <= LIB_MAX &&
            (int)fread(g_lib, sizeof(CatEntry), n, f) == n) g_lib_n = n;
        fclose(f); }
    return g_lib_n;
}
static int lib_load(void) {   /* returns the movie count; loads the cache, else scans once */
    if (lib_load_cache() == 0) lib_rescan();
    return g_lib_n;
}

/* ---- startup new-movie detection: walk the SD with the same filters as the library scan and
 * collect movie files that aren't in the cached library (added via browser/PC/upload). ---- */
#define NEWLIST_MAX 64
static char (*s_newlist)[PATHLEN + NAMELEN];   /* first NEWLIST_MAX new paths (malloc'd for the check) */
static int  s_new_n;                           /* TOTAL new movies found (may exceed NEWLIST_MAX) */

static int lib_has_path(const char *full) {
    for (int i = 0; i < g_lib_n; i++) if (!strcmp(g_lib[i].url, full)) return 1;
    return 0;
}
static void lib_detect_dir(const char *dir, int depth) {
    if (depth > 8) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char full[PATHLEN + NAMELEN];
        snprintf(full, sizeof full, "%s%s", dir, e->d_name);
        /* d_type avoids a stat() per entry (the slow part of the walk on FAT) */
        int isdir;
        if (e->d_type != DT_UNKNOWN) isdir = (e->d_type == DT_DIR);
        else { struct stat st; if (stat(full, &st)) continue; isdir = S_ISDIR(st.st_mode); }
        if (isdir) {
            if (is_hidden_dir(e->d_name)) continue;
            char sub[PATHLEN];
            snprintf(sub, sizeof sub, "%s/", full);
            lib_detect_dir(sub, depth + 1);
        } else if (is_moflex(e->d_name)) {
            if (lib_has_path(full)) continue;                              /* cheap string check first */
            if (cia_is_cia(e->d_name) && !cia_has_moflex(full)) continue;  /* open only NEW CIAs */
            if (s_newlist && s_new_n < NEWLIST_MAX) snprintf(s_newlist[s_new_n], sizeof s_newlist[0], "%s", full);
            s_new_n++;
        }
    }
    closedir(d);
}

/* Append the detected new files straight into the library (no second SD walk). */
static void lib_add_new(void) {
    int tracked = s_new_n < NEWLIST_MAX ? s_new_n : NEWLIST_MAX;
    for (int i = 0; i < tracked && g_lib_n < LIB_MAX; i++) {
        const char *full = s_newlist[i];
        const char *b = strrchr(full, '/'); b = b ? b + 1 : full;
        CatEntry *c = &g_lib[g_lib_n];
        movieinfo_load(full, c);                          /* .nfo if it already has one */
        snprintf(c->url, sizeof c->url, "%s", full);
        snprintf(c->fname, sizeof c->fname, "%s", b);
        if (!c->name[0]) { snprintf(c->name, sizeof c->name, "%s", b); strip_ext(c->name); }
        if (!c->category[0]) snprintf(c->category, sizeof c->category, "Uncategorized");
        lib_episode_name(c);
        c->is_zip = 0;
        struct stat st;
        if (!stat(full, &st)) { time_t mt = st.st_mtime; struct tm *tmv = localtime(&mt);
            if (tmv) strftime(c->date, sizeof c->date, "%Y-%m-%d", tmv); }
        g_lib_n++;
    }
    lib_save_cache();
}

/* load the library cache into RAM if present, WITHOUT falling back to a full SD scan */
static int lib_load_cache_only(void) {
    if (g_lib_n > 0) return g_lib_n;
    if (!g_lib) g_lib = (CatEntry *)malloc(sizeof(CatEntry) * LIB_MAX);
    if (!g_lib) return 0;
    FILE *f = fopen(LIB_CACHE, "rb");
    if (f) { int magic = 0, n = 0;
        if (fread(&magic, sizeof magic, 1, f) == 1 && magic == LIB_MAGIC &&
            fread(&n, sizeof n, 1, f) == 1 && n > 0 && n <= LIB_MAX &&
            (int)fread(g_lib, sizeof(CatEntry), n, f) == n) g_lib_n = n;
        fclose(f); }
    return g_lib_n;
}

/* Add (or update) a just-downloaded catalog movie in the library so it shows immediately
 * with its info + poster -- no rescan needed. Skips if no library exists yet (the file has a
 * .nfo, so the first library scan will include it correctly). */
static void lib_add_downloaded(const char *path, const CatEntry *src) {
    if (g_lib_n == 0) { lib_load_cache_only(); if (g_lib_n == 0) return; }  /* no cache -> leave to scan */
    if (!g_lib) return;
    int at = -1;
    for (int i = 0; i < g_lib_n; i++) if (!strcmp(g_lib[i].url, path)) { at = i; break; }  /* re-download */
    if (at < 0) { if (g_lib_n >= LIB_MAX) return; at = g_lib_n++; }
    CatEntry *c = &g_lib[at];
    *c = *src;                                          /* title/genres/category/year/runtime/is3d/desc/art */
    snprintf(c->url, sizeof c->url, "%s", path);        /* local path we play + poster key */
    const char *bn = strrchr(path, '/'); bn = bn ? bn + 1 : path;
    snprintf(c->fname, sizeof c->fname, "%s", bn);
    const char *title = src->title[0] ? src->title : src->name;
    if (title[0]) snprintf(c->name, sizeof c->name, "%s", title);
    else { snprintf(c->name, sizeof c->name, "%s", bn); strip_ext(c->name); }
    if (!c->category[0]) snprintf(c->category, sizeof c->category, "Uncategorized");
    lib_episode_name(c);
    c->is_zip = 0;
    time_t now = time(NULL); struct tm *tmv = localtime(&now);
    if (tmv) strftime(c->date, sizeof c->date, "%Y-%m-%d", tmv);
    lib_save_cache();
}

/* ---- the scrollable movie list for one category/genre filter ----
 * returns LL_BACK (back a level), LL_PLAY (out[] set), or LL_RESCAN (X). */
enum { LL_BACK = 0, LL_PLAY = 1, LL_RESCAN = 2 };
static char s_lib_search[64] = "";   /* active library search query ("" = off) */
static int lib_idxbuf[LIB_MAX];
static int library_list(const char *filt_cat, const char *filt_genre, u16 *poster, int *sortmode, char *out, size_t cap) {
ll_rebuild:;   /* X-search inside the list jumps back here with s_lib_search set */
    int *idx = lib_idxbuf, ni = 0;
    for (int i = 0; i < g_lib_n; i++) {
        if (filt_cat[0] && strcasecmp(lib_disp_cat(&g_lib[i]), filt_cat)) continue;
        if (filt_genre[0] && !genre_match(g_lib[i].genres, filt_genre)) continue;
        if (s_lib_search[0] && !ci_contains(g_lib[i].name, s_lib_search)) continue;   /* search box */
        idx[ni++] = i;
    }
    if (ni == 0) {
        msg_screen("LIBRARY", s_lib_search[0] ? "No videos match that search." : "Nothing here.");
        if (s_lib_search[0]) { s_lib_search[0] = 0; goto ll_rebuild; }   /* back to the unfiltered list */
        return LL_BACK;
    }
    g_scat = g_lib; g_smode = *sortmode; qsort(idx, ni, sizeof(int), idx_cmp);

    int csel = 0, cscroll = 0, redraw = 1, hfu = 0, hfd = 0;
    int shown = -1, phave = 0, settle = 0;
    int td = 0, tx0 = 0, ty0 = 0, tsc0 = 0, tmv = 0, tbar = 0, result = -1, marq_off = 0;
    while (aptMainLoop() && result < 0) {
        hidScanInput();
        u32 k = hidKeysDown(), kh = hidKeysHeld(), ku = hidKeysUp();
        if (k & KEY_B) { result = LL_BACK; break; }
        int play = 0, info = 0;
        if (nav_repeat(k, kh, NAV_DOWN, &hfd)) { if (csel < ni - 1) csel++; redraw = 1; }
        if (nav_repeat(k, kh, NAV_UP, &hfu))   { if (csel > 0) csel--; redraw = 1; }
        if (k & KEY_RIGHT) { csel += BR_ROWS; if (csel > ni - 1) csel = ni - 1; redraw = 1; }
        if (k & KEY_LEFT)  { csel -= BR_ROWS; if (csel < 0) csel = 0; redraw = 1; }
        if (k & KEY_R) { char cc = firstc(g_lib[idx[csel]].name); int i = csel;
            while (i < ni && firstc(g_lib[idx[i]].name) == cc) i++; if (i < ni) csel = i; redraw = 1; }
        if (k & KEY_L) { int i = csel; char cc = firstc(g_lib[idx[i]].name);
            while (i > 0 && firstc(g_lib[idx[i - 1]].name) == cc) i--;
            if (i == csel && i > 0) { i--; char p = firstc(g_lib[idx[i]].name);
                while (i > 0 && firstc(g_lib[idx[i - 1]].name) == p) i--; }
            csel = i; redraw = 1; }
        if (k & KEY_A) play = 1;
        if (k & KEY_X) {   /* search WITHIN this view (whatever category/genre is filtered) */
            char q[64];
            if (kbd_text("Search", q, sizeof q)) { snprintf(s_lib_search, sizeof s_lib_search, "%s", q); goto ll_rebuild; }
            redraw = 1;
        }
        if (k & KEY_Y) { *sortmode = (*sortmode + 1) % 3; g_smode = *sortmode;
            qsort(idx, ni, sizeof(int), idx_cmp); csel = 0; cscroll = 0; shown = -1; redraw = 1; }
        if (csel < cscroll) cscroll = csel;
        if (csel >= cscroll + BR_ROWS) cscroll = csel - BR_ROWS + 1;
        touchPosition tp; hidTouchRead(&tp);
        if (k & KEY_TOUCH) { td = 1; tx0 = tp.px; ty0 = tp.py; tsc0 = cscroll; tmv = 0; tbar = (tp.px >= UI_W - 14 && tp.py >= BR_LIST_Y && tp.py < 206); }
        else if ((kh & KEY_TOUCH) && td) {
            int maxs = ni > BR_ROWS ? ni - BR_ROWS : 0;
            if (tbar) { int th = BR_ROWS * BR_ROWH - 6, rel = tp.py - BR_LIST_Y; if (rel < 0) rel = 0; if (rel > th) rel = th;
                int ns = th ? maxs * rel / th : 0; if (ns != cscroll) { cscroll = ns; csel = cscroll; redraw = 1; } tmv = 1;
            } else { int dy = tp.py - ty0; if (dy > 6 || dy < -6) tmv = 1;
                if (tmv && maxs) { int ns = tsc0 - dy / BR_ROWH; if (ns < 0) ns = 0; if (ns > maxs) ns = maxs;
                    if (ns != cscroll) { cscroll = ns; redraw = 1; } } }
        } else if ((ku & KEY_TOUCH) && td) { td = 0;
            if (!tmv) {
                if (ty0 >= 210 && ty0 < 236) {               /* PLAY / INFO / SORT */
                    if (tx0 < 104) play = 1;
                    else if (tx0 < 208) info = 1;
                    else { *sortmode = (*sortmode + 1) % 3; g_smode = *sortmode;
                        qsort(idx, ni, sizeof(int), idx_cmp); csel = 0; cscroll = 0; shown = -1; redraw = 1; }
                } else if (ty0 >= BR_LIST_Y) { int i = cscroll + (ty0 - BR_LIST_Y) / BR_ROWH;
                    if (i >= 0 && i < ni && i < cscroll + BR_ROWS) {
                        if (i == csel) play = 1; else { csel = i; redraw = 1; } }   /* tap to select, tap again to play */
                }
            }
        }
        if (result >= 0) break;
        if (play) { snprintf(out, cap, "%s", g_lib[idx[csel]].url); result = LL_PLAY; break; }
        if (info) { lib_getinfo_menu(idx, ni, csel); shown = -1; redraw = 1; }   /* This / All-missing */
        if (csel != shown) { redraw = 1; phave = 0; settle = 0; }   /* moved -> reload local poster */
        /* Don't hit the SD for a poster while scrolling: a no-artwork ("Uncategorized") item
         * costs 3 failed FAT lookups (.p565/.jpg/.png) that would stall a held scroll. Load
         * only once the list has settled and no nav key is held / no drag is in progress. */
        int scrolling = (kh & (NAV_UP | NAV_DOWN)) || td;
        if (!scrolling && phave == 0 && ++settle >= 4) {
            phave = (poster && movieinfo_poster(g_lib[idx[csel]].url, poster, POSTER_W, POSTER_H)) ? 1 : 2;
            redraw = 1;
        }
        {   /* reveal-then-reset marquee of the selected title */
            int nr = 0, avail = (UI_W - 16) - 46 - (g_lib[idx[csel]].year > 0 ? 40 : 0);
            marq_off = (csel >= cscroll && csel < cscroll + BR_ROWS)
                     ? marquee_off(csel, ui_text_w(1, g_lib[idx[csel]].name), avail, &nr) : 0;
            if (nr) redraw = 1;
        }
        if (redraw) {
            ui_begin(GFX_BOTTOM);
            ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);
            ui_text(10, 8, 2, UI_NEON, "LIBRARY");
            char hdr[24]; snprintf(hdr, sizeof hdr, "%d / %d", csel + 1, ni);
            ui_text(UI_W - (int)strlen(hdr) * 8 - 10, 12, 1, UI_NEONP, hdr);
            char sh[80]; snprintf(sh, sizeof sh, "%s%s%s", filt_cat[0] ? filt_cat : "All", filt_genre[0] ? " / " : "", filt_genre);
            ui_text_left_fit(10, 30, 1, UI_NEONC, sh, UI_W - 20);
            ui_fill_round(8, 42, UI_W - 16, 1, 0, TH_LINE);
            for (int r = 0; r < BR_ROWS; r++) { int i = cscroll + r; if (i >= ni) break;
                const CatEntry *ce = &g_lib[idx[i]];
                int ry = BR_LIST_Y + r * BR_ROWH, rw = UI_W - 16, rh = BR_ROWH - 4, selrow = (i == csel);
                if (selrow) { ui_glow_round(8, ry, rw, rh, 7, UI_NEON, 3, 16);
                    ui_vgrad_round(8, ry, rw, rh, 7, TH_SEL, TH_SELLO);
                    ui_frame_round(8, ry, rw, rh, 7, UI_NEON, 1); }
                icon_movie(18, ry + 3, UI_NEONC);
                int tx = 46, txr = UI_W - 16, ty = ry + (rh - 8) / 2;
                char yr[8] = "";
                if (ce->year > 0) { snprintf(yr, sizeof yr, "%d", ce->year); txr -= 40; }   /* reserve room for the year */
                int tw = ui_text_w(1, ce->name);
                u16 tc = selrow ? UI_WHITE : UI_INK;
                if (tw <= txr - tx) ui_text(tx, ty, 1, tc, ce->name);
                else if (selrow) ui_text_clipped(tx - marq_off, ty, 1, tc, ce->name, tx, txr);
                else { char disp[NAMELEN]; snprintf(disp, sizeof disp, "%s", ce->name);
                    int cm = (txr - tx) / 8; if ((int)strlen(disp) > cm) disp[cm] = 0; ui_text(tx, ty, 1, tc, disp); }
                if (yr[0]) ui_text(UI_W - 16 - ui_text_w(1, yr), ty, 1, selrow ? UI_NEONC : UI_GRAY, yr);
            }
            if (ni > BR_ROWS) { int th = BR_ROWS * BR_ROWH - 6, ty = BR_LIST_Y, maxs = ni - BR_ROWS;
                int hh = th * BR_ROWS / ni; if (hh < 12) hh = 12; int hy = ty + (th - hh) * cscroll / maxs;
                ui_fill_round(UI_W - 6, ty, 3, th, 1, TH_TRACK);
                ui_fill_round(UI_W - 6, hy, 3, hh, 1, UI_NEON); }
            const char *sm = *sortmode == 0 ? "Sort: Title" : *sortmode == 1 ? "Sort: Year" : "Sort: Date";
            ui_button(8, 210, 96, 26, "PLAY", 0, UI_NEON);
            ui_button(108, 210, 100, 26, "INFO", 0, UI_NEONC);
            ui_button(212, 210, 100, 26, sm, 0, UI_NEONP);
            ui_present();
            draw_info_top(&g_lib[idx[csel]], phave == 1 ? poster : NULL);
            shown = csel; redraw = 0;
        }
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    }
    s_lib_search[0] = 0;   /* an in-list search never leaks into the next list view */
    return result < 0 ? LL_BACK : result;
}

/* Browse the library: category -> (View All / Pick a Genre) -> list. Back steps down one level. */
static int library_view(char *out, size_t cap) {
    if (lib_load() == 0) {
        msg_screen("LIBRARY", "No movies found on the SD card.\nDownload or add some first.");
        return 0;
    }
    gfxSet3D(false);
    u16 *poster = (u16 *)malloc((size_t)POSTER_W * POSTER_H * sizeof(u16));
    int sortmode = 0, chose = 0;

    while (aptMainLoop() && !chose) {
        static char cats[24][32]; int ncat = lib_distinct_categories(cats, 24);
        qsort(cats, ncat, 32, strrow_cmp);
        static char cdisp[24][32];   /* display rows with per-category counts (selection uses cats[]) */
        for (int k = 0; k < ncat; k++) {
            int n = 0;
            for (int i = 0; i < g_lib_n; i++) if (!strcasecmp(lib_disp_cat(&g_lib[i]), cats[k])) n++;
            snprintf(cdisp[k], 32, "%.24s (%d)", cats[k], n);
        }
        char sub[24]; snprintf(sub, sizeof sub, "%d videos", g_lib_n);
        int c = catalog_pick("LIBRARY", sub, cdisp, ncat, 1, "* Rescan Library");
        if (c == -1) break;                                   /* B -> leave the library */
        if (c == -3) { lib_rescan(); if (g_lib_n == 0) { msg_screen("LIBRARY", "No movies found."); break; } continue; }
        if (c == -4) {                                        /* X -> search the whole library */
            char q[64];
            if (kbd_text("Search your library", q, sizeof q)) {
                snprintf(s_lib_search, sizeof s_lib_search, "%s", q);
                int r = library_list("", "", poster, &sortmode, out, cap);
                s_lib_search[0] = 0;
                if (r == LL_PLAY) chose = 1;
                else if (r == LL_RESCAN) lib_rescan();
            }
            continue;
        }

        int rescan = 0;
        if (c == -2) {                                        /* Show All -> list; back -> category */
            int r = library_list("", "", poster, &sortmode, out, cap);
            if (r == LL_PLAY) chose = 1; else if (r == LL_RESCAN) rescan = 1;
        } else {                                              /* a category */
            char fc[32]; snprintf(fc, sizeof fc, "%s", cats[c]);
            static char gens[64][32]; int ng = distinct_genres(g_lib, g_lib_n, fc, gens, 64);
            for (int gi = 0; gi < ng; )   /* "Moflex" is a format, not a genre -> hide it */
                if (!strcasecmp(gens[gi], "Moflex")) { for (int gj = gi; gj < ng - 1; gj++) memcpy(gens[gj], gens[gj + 1], 32); ng--; }
                else gi++;
            qsort(gens, ng, 32, strrow_cmp);
            if (ng == 0) {                                    /* no genres -> straight to the list */
                int r = library_list(fc, "", poster, &sortmode, out, cap);
                if (r == LL_PLAY) chose = 1; else if (r == LL_RESCAN) rescan = 1;
            } else {
                int sub_back = 0;
                while (!sub_back && !chose && !rescan && aptMainLoop()) {
                    const char *mi[2] = { "View All", "Pick a Genre" };
                    int mm = ui_menu(fc, NULL, mi, 2);
                    if (mm < 0) { sub_back = 1; break; }       /* back -> category list */
                    if (mm == 0) {                             /* View All of this category */
                        int r = library_list(fc, "", poster, &sortmode, out, cap);
                        if (r == LL_PLAY) chose = 1; else if (r == LL_RESCAN) rescan = 1;
                    } else {                                   /* Pick a Genre */
                        static char gdisp[64][32];   /* genre rows with counts (within this category) */
                        for (int k = 0; k < ng; k++) {
                            int n = 0;
                            for (int i = 0; i < g_lib_n; i++)
                                if (!strcasecmp(lib_disp_cat(&g_lib[i]), fc) && genre_match(g_lib[i].genres, gens[k])) n++;
                            snprintf(gdisp[k], 32, "%.24s (%d)", gens[k], n);
                        }
                        int gen_back = 0;
                        while (!gen_back && !chose && !rescan && aptMainLoop()) {
                            int g = catalog_pick("GENRE", fc, gdisp, ng, 0, NULL);
                            if (g < 0) { gen_back = 1; break; }         /* back -> View All / Pick a Genre */
                            int r = library_list(fc, gens[g], poster, &sortmode, out, cap);
                            if (r == LL_PLAY) chose = 1; else if (r == LL_RESCAN) rescan = 1;
                            /* back from the list -> re-show the GENRE picker */
                        }
                    }
                }
            }
        }
        if (rescan) lib_rescan();                             /* categories may change -> rebuild at the top */
    }
    branding_show();
    free(poster);
    return chose ? 1 : 0;
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
    if (!download_resume_check(url)) return;   /* B on the prompt -> back */
    bool ok = download_to_file(url, dest, dl_progress, NULL);
    if (!ok) { download_cancel_msg(url); return; }   /* progress saved; offers DELETE FILE, B keeps it */
    size_t fl = strlen(fname);
    if (fl > 4 && !strcasecmp(fname + fl - 4, ".zip") && file_is_zip(dest) &&
        confirm("Extract the zip now?\n(No = keep the .zip file)")) {
        char stem[NAMELEN]; snprintf(stem, sizeof(stem), "%s", fname); stem[fl - 4] = 0;
        char folder[PATHLEN + NAMELEN]; snprintf(folder, sizeof(folder), "%s%s", destdir, stem);
        ui_begin(GFX_BOTTOM); ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);
        ui_text_center(UI_W / 2, 100, 2, UI_NEON, "Extracting...");
        ui_text_fit(UI_W / 2, 130, 1, UI_DIM, stem, UI_W - 16); ui_present();
        gfxFlushBuffers(); gfxSwapBuffers();
        int nf = unzip_to_dir(dest, folder);
        remove(dest);
        char m[96]; snprintf(m, sizeof m, nf > 0 ? "Extracted %d file%s." : "Extract FAILED.", nf, nf == 1 ? "" : "s");
        msg_screen("DOWNLOAD", m);
    } else {
        msg_screen("DOWNLOAD", "Download complete.");
    }
}

static void download_screen(void) {
    load_sources();
    int m = 0, top = 0, redraw = 1, tdown = 0, tx0 = 0, ty0 = 0, tmoved = 0;
    const int VIS = 5, bx = 16, bw = UI_W - 32, bh = 26, gap = 6, y0 = 56;
    while (aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown(), kh = hidKeysHeld(), ku = hidKeysUp();
        int total = nsources + 2;   /* + URL download + add source */
        if (k & KEY_B) break;
        if (k & KEY_DOWN) { m = (m + 1) % total; redraw = 1; }
        if (k & KEY_UP)   { m = (m - 1 + total) % total; redraw = 1; }
        if (k & KEY_X && m >= BUILTIN_SOURCES && m < nsources) {   /* remove a user-added source */
            if (confirm("Remove this source?")) { remove_source(m); total = nsources + 2; if (m >= total) m = total - 1; }
            redraw = 1;
        }
        int act = -1;
        if (k & KEY_A) act = m;
        touchPosition tp; hidTouchRead(&tp);
        if (k & KEY_TOUCH) { tdown = 1; tx0 = tp.px; ty0 = tp.py; tmoved = 0; }
        else if ((kh & KEY_TOUCH) && tdown) { if (abs(tp.px - tx0) > 8 || abs(tp.py - ty0) > 8) tmoved = 1; }
        else if ((ku & KEY_TOUCH) && tdown) { tdown = 0;
            if (!tmoved) for (int r = 0; r < VIS; r++) { int i = top + r; if (i >= total) break;
                int by = y0 + r * (bh + gap);
                if (tx0 >= bx && tx0 < bx + bw && ty0 >= by && ty0 < by + bh) { m = i; act = i; break; } }
        }
        if (act >= 0) {
            if (act < nsources) catalog_browse(&sources[act]);
            else if (act == nsources) download_url_direct();
            else add_source_swkbd();
            load_sources(); redraw = 1;
        }
        if (m < top) top = m; if (m >= top + VIS) top = m - VIS + 1;
        if (redraw) {
            ui_begin(GFX_BOTTOM);
            ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);
            ui_text_center(UI_W / 2, 12, 2, UI_NEON, "DOWNLOAD");
            ui_text_center(UI_W / 2, 34, 1, UI_NEONP, "choose a source");
            ui_glow_round(28, 48, UI_W - 56, 2, 1, UI_NEON, 3, 30);
            ui_fill_round(28, 48, UI_W - 56, 2, 1, UI_NEON);
            for (int r = 0; r < VIS; r++) { int i = top + r; if (i >= total) break;
                int by = y0 + r * (bh + gap); char lbl[64];
                if (i < nsources) snprintf(lbl, sizeof lbl, "%s%s", sources[i].name, i >= BUILTIN_SOURCES ? "  *" : "");
                else if (i == nsources) snprintf(lbl, sizeof lbl, "+ Download from URL");
                else snprintf(lbl, sizeof lbl, "+ Add source");
                ui_button(bx, by, bw, bh, lbl, i == m, i >= nsources ? UI_NEONC : UI_NEON);
            }
            ui_text_center(UI_W / 2, 224, 1, UI_DIM, "* added by you   X removes");
            ui_present(); redraw = 0;
        }
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    }
}

/* ---------- ADD MOVIES menu ---------- */

/* Reusable neon menu page: title (+ optional subtitle) + vertical touch buttons.
 * Returns the chosen index, or -1 on B/cancel. Draws the bottom screen only. */
static int ui_menu(const char *title, const char *subtitle, const char *const *items, int n) {
    int sel = 0, redraw = 1, tdown = 0, tx0 = 0, ty0 = 0, tmoved = 0;
    int bx = 24, bw = UI_W - 48, bh = 32, gap = 8, y0 = subtitle ? 74 : 62;
    while (aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown(), kh = hidKeysHeld(), ku = hidKeysUp();
        if (k & KEY_B) return -1;
        if (k & KEY_DOWN) { sel = (sel + 1) % n; redraw = 1; }
        if (k & KEY_UP)   { sel = (sel + n - 1) % n; redraw = 1; }
        if (k & KEY_A) return sel;
        touchPosition tp; hidTouchRead(&tp);
        if (k & KEY_TOUCH) { tdown = 1; tx0 = tp.px; ty0 = tp.py; tmoved = 0; }
        else if ((kh & KEY_TOUCH) && tdown) { if (abs(tp.px - tx0) > 8 || abs(tp.py - ty0) > 8) tmoved = 1; }
        else if ((ku & KEY_TOUCH) && tdown) { tdown = 0;
            if (!tmoved) for (int i = 0; i < n; i++) { int by = y0 + i * (bh + gap);
                if (tx0 >= bx && tx0 < bx + bw && ty0 >= by && ty0 < by + bh) return i; }
        }
        if (redraw) {
            ui_begin(GFX_BOTTOM);
            ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);
            ui_text_center(UI_W / 2, 14, 2, UI_NEON, title);
            ui_glow_round(28, 40, UI_W - 56, 2, 1, UI_NEON, 3, 34);
            ui_fill_round(28, 40, UI_W - 56, 2, 1, UI_NEON);
            if (subtitle && subtitle[0]) ui_text_fit(UI_W / 2, 50, 1, UI_NEONC, subtitle, UI_W - 16);
            for (int i = 0; i < n; i++) { int by = y0 + i * (bh + gap);
                ui_button(bx, by, bw, bh, items[i], i == sel, UI_NEON); }
            ui_present(); redraw = 0;
        }
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    }
    return -1;
}

static void add_movies_menu(void) {
    for (;;) {
        const char *items[] = { "DOWNLOAD  (catalog / URL)", "UPLOAD  (over Wi-Fi)" };
        int c = ui_menu("ADD VIDEO", NULL, items, 2);
        if (c < 0) break;
        if (c == 0) download_screen(); else upload_screen();
    }
}

/* ---------- MANAGE menu (move / delete) ---------- */

/* ---- metadata scraper: fill moviedata/ from the catalogs already known to the app
 * (Clownsec first, then Zackk, then user-added sources) -- no external DB, no API keys. ---- */
static void local_stem(const char *path, char *o, size_t cap) {   /* basename without extension */
    const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
    snprintf(o, cap, "%s", b);
    char *d = strrchr(o, '.'); if (d && d != o) *d = 0;
}
static int four_digits(const char *p) {
    return isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1]) &&
           isdigit((unsigned char)p[2]) && isdigit((unsigned char)p[3]);
}
static int year_val(const char *p) { return (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0'); }

/* position of a TV episode marker "SxxExx" at a word boundary, or NULL. Everything from here on
 * (the episode number + episode title) is dropped so a local episode reduces to its show name. */
static const char *episode_pos(const char *name) {
    for (const char *p = name; *p; p++) {
        if (p != name && isalnum((unsigned char)p[-1])) continue;
        if ((p[0] == 's' || p[0] == 'S') && isdigit((unsigned char)p[1])) {
            const char *q = p + 1; while (isdigit((unsigned char)*q)) q++;
            if ((*q == 'e' || *q == 'E') && isdigit((unsigned char)q[1])) return p;
        }
    }
    return NULL;
}

/* Split a movie/show name into a normalized title (letters+digits, lower-case) and a year.
 * ALL bracketed groups -- (Extended), (Subtitled), (3D), (2001), [HD] -- are dropped, and the
 * title is taken from the text BEFORE the year. So "Black Hawk Down (Extended) (2001) (3D)" and
 * "Black Hawk Down (2001)" both reduce to title="blackhawkdown", year="2001"; a TV episode
 * "Doctor Who (2005) Season 1 ..." and a catalog "Doctor Who (2005) (3D) - Season 1" both reduce
 * to "doctorwho"/"2005". Punctuation (dashes, colons, dots) is stripped either way. */
static void parse_title_year(const char *name, char *tnorm, size_t cap, char yr[5]) {
    yr[0] = 0;
    const char *ypos = NULL;
    for (const char *p = name; *p; p++)                     /* parenthesized year preferred */
        if (p[0] == '(' && four_digits(p + 1) && p[5] == ')') {
            int y = year_val(p + 1); if (y >= 1900 && y <= 2099) { memcpy(yr, p + 1, 4); yr[4] = 0; ypos = p; break; }
        }
    if (!yr[0])                                             /* else a bare year token */
        for (const char *p = name; *p; p++)
            if (four_digits(p) && !isdigit((unsigned char)p[4]) && (p == name || !isdigit((unsigned char)p[-1]))) {
                int y = year_val(p); if (y >= 1900 && y <= 2099) { memcpy(yr, p, 4); yr[4] = 0; ypos = p; break; }
            }
    const char *end = ypos ? ypos : name + strlen(name);
    const char *epos = episode_pos(name);                  /* TV episode -> keep only the show name */
    if (epos && epos < end) end = epos;
    size_t j = 0; int depth = 0;
    for (const char *p = name; p < end; p++) {
        char c = *p;
        if (c == '(' || c == '[' || c == '{') { depth++; continue; }
        if (c == ')' || c == ']' || c == '}') { if (depth > 0) depth--; continue; }
        if (depth > 0) continue;                            /* skip anything inside brackets */
        if (isalnum((unsigned char)c) && j + 1 < cap) tnorm[j++] = (char)tolower((unsigned char)c);
    }
    tnorm[j] = 0;
}
static int scr_match(const char *localstem, const CatEntry *e) {
    char lt[256], et[256], ft[256], tt[256], ly[5], ey[5], fy[5], ty[5];
    parse_title_year(localstem, lt, sizeof lt, ly);
    if (!lt[0]) return 0;
    parse_title_year(e->name, et, sizeof et, ey);
    char fstem[NAMELEN]; snprintf(fstem, sizeof fstem, "%s", e->fname);
    char *d = strrchr(fstem, '.'); if (d) *d = 0;
    parse_title_year(fstem, ft, sizeof ft, fy);
    parse_title_year(e->title, tt, sizeof tt, ty);          /* the clean show/movie title (no "Season N [ZIP]") */

    char ls[262], es[262], fs[262], ts[270];                /* "titleyear" keys */
    snprintf(ls, sizeof ls, "%s%s", lt, ly);
    snprintf(es, sizeof es, "%s%s", et, ey);
    snprintf(fs, sizeof fs, "%s%s", ft, fy);
    if (e->year > 0) snprintf(ts, sizeof ts, "%s%d", tt, e->year);   /* title[] has no year -> use the year field */
    else             snprintf(ts, sizeof ts, "%s%s", tt, ty);
    if (!strcmp(ls, es) || !strcmp(ls, fs) || !strcmp(ls, ts)) return 1;   /* title+year match (movies & TV) */
    /* no year in the filename -> match title only (remakes may pick the wrong year; add a year to be exact) */
    if (!ly[0] && strlen(lt) >= 4 && (!strcmp(lt, et) || !strcmp(lt, ft) || !strcmp(lt, tt))) return 1;
    return 0;
}
/* Fetch a catalog entry's poster into pb, with one retry for transient network failures.
 * Returns 1 if a poster was decoded, 0 if there's no art or the download kept failing. */
static int fetch_poster(const CatEntry *e, u16 *pb) {
    if (!pb || !e->art[0]) return 0;
    if (poster_get(e->art, e->fname, pb, POSTER_W, POSTER_H)) return 1;
    return poster_get(e->art, e->fname, pb, POSTER_W, POSTER_H);   /* retry once */
}

/* Try to find + save catalog metadata for ONE movie (forces a refresh; used by X:Get info).
 * Returns 0 = no match, 1 = saved with poster, 2 = saved but the poster wouldn't download. */
static int scrape_one(const char *moviepath) {
    load_sources();
    char lstem[192]; local_stem(moviepath, lstem, sizeof lstem);
    int cap = 4096; CatEntry *cat = (CatEntry *)malloc(sizeof(CatEntry) * cap);
    u16 *pb = (u16 *)malloc((size_t)POSTER_W * POSTER_H * sizeof(u16));
    if (!cat) { free(pb); return 0; }
    int found = 0;
    for (int s = 0; s < nsources && !found; s++) {
        printf("\x1b[2J\x1b[H=== GET INFO ===\n\nChecking %.28s ...\n", sources[s].name);
        gfxFlushBuffers(); gfxSwapBuffers();
        char *json = NULL; size_t len = 0;
        if (!download_to_mem(sources[s].url, &json, &len, 32 * 1024 * 1024)) continue;
        int nc = catalog_parse(json, sources[s].kind, sources[s].dl_base, sources[s].art_base, cat, cap);
        free(json);
        for (int i = 0; i < nc; i++) if (scr_match(lstem, &cat[i])) {
            int poster_ok = fetch_poster(&cat[i], pb);
            movieinfo_save(moviepath, &cat[i], poster_ok ? pb : NULL, POSTER_W, POSTER_H);
            found = poster_ok ? 1 : 2;
            break;
        }
    }
    free(pb); free(cat);
    return found;
}
/* Batch: fill in every movie in the current folder that is MISSING metadata (skips ones that
 * already have art + description). Each catalog is fetched only once. */
static void scrape_folder(void) {
    load_sources();
    int cap = 4096; CatEntry *cat = (CatEntry *)malloc(sizeof(CatEntry) * cap);
    u16 *pb = (u16 *)malloc((size_t)POSTER_W * POSTER_H * sizeof(u16));
    static char done[MAXE];
    int todo = 0;
    for (int i = 0; i < nentries; i++) {
        done[i] = 1;
        if (!entries[i].is_dir && is_moflex(entries[i].name)) {
            char full[PATHLEN + NAMELEN]; snprintf(full, sizeof full, "%s%s", cwd, entries[i].name);
            if (!movieinfo_have(full)) { done[i] = 0; todo++; }   /* skip ones that already have info */
        }
    }
    int got = 0;
    if (!cat || todo == 0) {
        msg_screen("GET INFO", todo == 0 ? "Every movie here already has info." : "Out of memory.");
        free(pb); free(cat); return;
    }
    int cancelled = 0;
    for (int s = 0; s < nsources && got < todo && !cancelled; s++) {
        printf("\x1b[2J\x1b[H=== GET INFO (all) ===\n\nFetching %.24s catalog...\n(%d/%d matched)\n",
               sources[s].name, got, todo);
        gfxFlushBuffers(); gfxSwapBuffers();
        char *json = NULL; size_t len = 0;
        if (!download_to_mem(sources[s].url, &json, &len, 32 * 1024 * 1024)) continue;
        int nc = catalog_parse(json, sources[s].kind, sources[s].dl_base, sources[s].art_base, cat, cap);
        free(json);
        int scanned = 0;
        for (int i = 0; i < nentries && got < todo; i++) {
            if (done[i]) continue;
            hidScanInput();
            if (hidKeysDown() & KEY_B) { cancelled = 1; break; }
            scanned++;
            /* live progress (redraw in place, no flash) -- shown while this title's poster downloads */
            printf("\x1b[H=== GET INFO (all) ===\x1b[K\n\nSource: %.24s\x1b[K\nMatched: %d / %d\x1b[K\n"
                   "Checking %d: %.28s\x1b[K\n\nB: cancel\x1b[K\n\x1b[J",
                   sources[s].name, got, todo, scanned, entries[i].name);
            gfxFlushBuffers(); gfxSwapBuffers();
            char full[PATHLEN + NAMELEN]; snprintf(full, sizeof full, "%s%s", cwd, entries[i].name);
            char lstem[192]; local_stem(full, lstem, sizeof lstem);
            for (int c = 0; c < nc; c++) if (scr_match(lstem, &cat[c])) {
                int poster_ok = fetch_poster(&cat[c], pb);
                movieinfo_save(full, &cat[c], poster_ok ? pb : NULL, POSTER_W, POSTER_H);
                done[i] = 1; got++; break;   /* matched (priority-first); a missing poster is
                                              * refetched next run since movieinfo_have needs one */
            }
        }
    }
    free(pb); free(cat);
    char m[128]; snprintf(m, sizeof m, "%sMatched %d of %d movie%s.\n%s",
           cancelled ? "Cancelled.\n" : "", got, todo, todo == 1 ? "" : "s",
           (got < todo && !cancelled) ? "(no catalog match for the rest)" : "");
    msg_screen("GET INFO", m);
}

/* Library GET INFO: refresh just this movie, in place. */
static void lib_scrape_one(int i) {
    char path[CAT_URLLEN]; snprintf(path, sizeof path, "%s", g_lib[i].url);
    int r = scrape_one(path);
    if (r) { lib_refresh_entry(i); lib_save_cache(); }
    msg_screen("GET INFO", r == 1 ? "Info + artwork saved."
                         : r == 2 ? "Info saved, but the poster\ndidn't download. Try again for the art."
                                  : "No catalog had this title.");
}

/* Library GET INFO (batch): fill in every movie in the current list that's MISSING info. */
static void lib_scrape_missing(int *idx, int ni) {
    load_sources();
    int cap = 4096; CatEntry *cat = (CatEntry *)malloc(sizeof(CatEntry) * cap);
    u16 *pb = (u16 *)malloc((size_t)POSTER_W * POSTER_H * sizeof(u16));
    static char done[LIB_MAX];
    int todo = 0;
    for (int i = 0; i < ni; i++) { done[i] = movieinfo_have(g_lib[idx[i]].url) ? 1 : 0; if (!done[i]) todo++; }
    if (!cat || todo == 0) {
        msg_screen("GET INFO", todo == 0 ? "Every movie here already has info." : "Out of memory.");
        free(pb); free(cat); return;
    }
    int got = 0, cancelled = 0;
    for (int s = 0; s < nsources && got < todo && !cancelled; s++) {
        printf("\x1b[2J\x1b[H=== GET INFO (all) ===\n\nFetching %.24s catalog...\n(%d/%d matched)\n",
               sources[s].name, got, todo);
        gfxFlushBuffers(); gfxSwapBuffers();
        char *json = NULL; size_t len = 0;
        if (!download_to_mem(sources[s].url, &json, &len, 32 * 1024 * 1024)) continue;
        int nc = catalog_parse(json, sources[s].kind, sources[s].dl_base, sources[s].art_base, cat, cap);
        free(json);
        int scanned = 0;
        for (int i = 0; i < ni && got < todo; i++) {
            if (done[i]) continue;
            hidScanInput();
            if (hidKeysDown() & KEY_B) { cancelled = 1; break; }
            scanned++;
            printf("\x1b[H=== GET INFO (all) ===\x1b[K\n\nSource: %.24s\x1b[K\nMatched: %d / %d\x1b[K\n"
                   "Checking %d: %.28s\x1b[K\n\nB: cancel\x1b[K\n\x1b[J",
                   sources[s].name, got, todo, scanned, g_lib[idx[i]].name);
            gfxFlushBuffers(); gfxSwapBuffers();
            char lstem[192]; local_stem(g_lib[idx[i]].url, lstem, sizeof lstem);
            for (int c = 0; c < nc; c++) if (scr_match(lstem, &cat[c])) {
                int poster_ok = fetch_poster(&cat[c], pb);
                movieinfo_save(g_lib[idx[i]].url, &cat[c], poster_ok ? pb : NULL, POSTER_W, POSTER_H);
                lib_refresh_entry(idx[i]);
                done[i] = 1; got++; break;
            }
        }
    }
    free(pb); free(cat);
    lib_save_cache();
    char m[128]; snprintf(m, sizeof m, "%sMatched %d of %d movie%s.\n%s",
           cancelled ? "Cancelled.\n" : "", got, todo, todo == 1 ? "" : "s",
           (got < todo && !cancelled) ? "(no catalog match for the rest)" : "");
    msg_screen("GET INFO", m);
}

/* ---- startup: detect movies added outside the app (browser download, PC copy, upload) and offer
 * a rescan; after rescanning, offer art + info download for the new arrivals. ---- */
static void startup_new_movie_check(void) {
    if (lib_load_cache() == 0) {
        /* FIRST RUN: no library yet -- the most important time to create it. */
        if (prompt2("VIDEO LIBRARY", "No video library yet.\nScan the SD card for videos now?",
                    "SCAN", "LATER") != 0) return;
        lib_rescan();
        if (g_lib_n == 0) { msg_screen("LIBRARY", "No videos found on the SD card.\nDownload or add some first."); return; }
        int *idx = (int *)malloc(sizeof(int) * g_lib_n); int ni = 0;   /* offer art+info for all missing */
        if (idx) {
            for (int j = 0; j < g_lib_n; j++) if (!movieinfo_have(g_lib[j].url)) idx[ni++] = j;
            if (ni > 0) {
                if (prompt2("GET INFO", "Download art & info?", "DOWNLOAD", "SKIP") == 0) lib_scrape_missing(idx, ni);
            }
            free(idx);
        }
        return;
    }

    ui_begin(GFX_BOTTOM); ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);
    ui_text_center(UI_W / 2, 104, 1, UI_DIM, "Checking for new movies..."); ui_present();
    gfxFlushBuffers(); gfxSwapBuffers();

    s_newlist = malloc(sizeof *s_newlist * NEWLIST_MAX);
    s_new_n = 0;
    lib_detect_dir("sdmc:/", 0);
    if (s_new_n == 0) { free(s_newlist); s_newlist = NULL; return; }

    char msg[96];
    snprintf(msg, sizeof msg, "%d new video%s found on the SD card.\nAdd new videos to Library?",
             s_new_n, s_new_n == 1 ? "" : "s");
    if (prompt2("NEW VIDEOS", msg, "ADD", "LATER") != 0) { free(s_newlist); s_newlist = NULL; return; }
    if (s_new_n <= NEWLIST_MAX) lib_add_new();   /* instant: append the found files, no second walk */
    else lib_rescan();                           /* too many to have tracked -> full rescan */

    /* offer art + info for the new arrivals that don't have any yet */
    static int idx[NEWLIST_MAX]; int ni = 0;
    int tracked = s_new_n < NEWLIST_MAX ? s_new_n : NEWLIST_MAX;
    for (int i = 0; i < tracked; i++) {
        if (movieinfo_have(s_newlist[i])) continue;
        for (int j = 0; j < g_lib_n; j++)
            if (!strcmp(g_lib[j].url, s_newlist[i])) { idx[ni++] = j; break; }
    }
    free(s_newlist); s_newlist = NULL;
    if (ni == 0) return;
    if (prompt2("GET INFO", "Download art & info?", "DOWNLOAD", "SKIP") == 0) lib_scrape_missing(idx, ni);
}

static void lib_getinfo_menu(int *idx, int ni, int csel) {
    const char *items[3] = { "This movie", "All here (missing only)", "Cancel" };
    int c = ui_menu("GET INFO", g_lib[idx[csel]].name, items, 3);
    if (c == 0) lib_scrape_one(idx[csel]);
    else if (c == 1) lib_scrape_missing(idx, ni);
}

/* GET INFO chooser (X in Open Video): fetch metadata for just this movie, or every movie in
 * the folder that's missing it. */
static void getinfo_menu(void) {
    if (nentries == 0) return;
    int is_movie = !entries[sel].is_dir && is_moflex(entries[sel].name);
    const char *items[3]; int act[3]; int n = 0;
    if (is_movie) { items[n] = "This movie";                act[n++] = 0; }
    items[n] = "All movies here (missing only)"; act[n++] = 1;
    items[n] = "Cancel";                         act[n++] = 2;
    int c = ui_menu("GET INFO", is_movie ? entries[sel].name : NULL, items, n);
    if (c < 0) return;
    int a = act[c];
    if (a == 0) {
        char full[PATHLEN + NAMELEN]; snprintf(full, sizeof full, "%s%s", cwd, entries[sel].name);
        int r = scrape_one(full);   /* forces a refresh, even if it already has info */
        const char *msg = r == 1 ? "Info + artwork saved."
                        : r == 2 ? "Info saved, but the poster\ndidn't download. Try again for the art."
                                 : "No catalog had this title.";
        msg_screen("GET INFO", msg);
    } else if (a == 1) {
        scrape_folder();
    }
}

static void manage_menu(void) {
    if (nentries == 0) return;
    char full[PATHLEN + NAMELEN];
    snprintf(full, sizeof(full), "%s%s", cwd, entries[sel].name);
    const char *items[] = { "RENAME", "DELETE", "MOVE (cut)", "CANCEL" };
    int c = ui_menu("MANAGE", entries[sel].name, items, 4);
    if (c == 0) {   /* rename in place (keyboard pre-filled with the current name) */
        char nn[NAMELEN];
        SwkbdState s;
        swkbdInit(&s, SWKBD_TYPE_NORMAL, 2, -1);
        swkbdSetHintText(&s, "New name");
        swkbdSetInitialText(&s, entries[sel].name);
        if (swkbdInputText(&s, nn, sizeof nn) == SWKBD_BUTTON_RIGHT && nn[0] && strcmp(nn, entries[sel].name)) {
            char nf[PATHLEN + NAMELEN];
            snprintf(nf, sizeof nf, "%s%s", cwd, nn);
            if (rename(full, nf) == 0) scan();
            else msg_screen("RENAME", "Could not rename\n(name in use or invalid).");
        }
    }
    else if (c == 1) { if (confirm("Delete this item?")) { remove(full); scan(); } }
    else if (c == 2) {
        snprintf(move_src, sizeof(move_src), "%s", full);
        snprintf(move_name, sizeof(move_name), "%s", entries[sel].name);
        move_pending = 1;
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
    int sel = 0, top = 0, redraw = 1, tdown = 0, tx0 = 0, ty0 = 0, tsc0 = 0, tmoved = 0;
    const int VIS = 5, ry0 = 54, rh = 30, gap = 4;
    while (aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown(), kh = hidKeysHeld(), ku = hidKeysUp();
        if (k & KEY_B) return -1;
        if (k & KEY_DOWN) { if (sel < n - 1) sel++; redraw = 1; }
        if (k & KEY_UP)   { if (sel > 0) sel--; redraw = 1; }
        if (k & KEY_A) return sel;
        touchPosition tp; hidTouchRead(&tp);
        if (k & KEY_TOUCH) { tdown = 1; tx0 = tp.px; ty0 = tp.py; tsc0 = top; tmoved = 0; }
        else if ((kh & KEY_TOUCH) && tdown) { int dy = tp.py - ty0;
            if (dy > 6 || dy < -6) tmoved = 1;
            if (tmoved && n > VIS) { int maxs = n - VIS, ns = tsc0 - dy / (rh + gap);
                if (ns < 0) ns = 0; if (ns > maxs) ns = maxs; if (ns != top) { top = ns; redraw = 1; } } }
        else if ((ku & KEY_TOUCH) && tdown) { tdown = 0;
            if (!tmoved) for (int r = 0; r < VIS; r++) { int i = top + r; if (i >= n) break;
                int by = ry0 + r * (rh + gap);
                if (ty0 >= by && ty0 < by + rh) return i; } }
        if (sel < top) top = sel; if (sel >= top + VIS) top = sel - VIS + 1;
        if (redraw) {
            ui_begin(GFX_BOTTOM);
            ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);
            ui_text_center(UI_W / 2, 12, 2, UI_NEON, "CHOOSE VIDEO");
            char sub[32]; snprintf(sub, sizeof sub, "%d in this CIA", n);
            ui_text_center(UI_W / 2, 34, 1, UI_NEONP, sub);
            ui_glow_round(28, 48, UI_W - 56, 2, 1, UI_NEON, 3, 30);
            ui_fill_round(28, 48, UI_W - 56, 2, 1, UI_NEON);
            for (int r = 0; r < VIS; r++) { int i = top + r; if (i >= n) break;
                int by = ry0 + r * (rh + gap), selrow = (i == sel), rw = UI_W - 16;
                if (selrow) { ui_glow_round(8, by, rw, rh, 7, UI_NEON, 3, 16);
                    ui_vgrad_round(8, by, rw, rh, 7, TH_SEL, TH_SELLO);
                    ui_frame_round(8, by, rw, rh, 7, UI_NEON, 1); }
                icon_movie(18, by + (rh - 16) / 2, UI_NEONC);
                char nm[64]; snprintf(nm, sizeof nm, "%s", list[i].name); strip_ext(nm);
                char sz[16]; snprintf(sz, sizeof sz, "%lldMB", (long long)(list[i].size / 1000000));
                int nmax = (rw - 46 - (int)strlen(sz) * 8 - 16) / 8;
                if (nmax > 0 && (int)strlen(nm) > nmax) nm[nmax] = 0;
                ui_text(46, by + (rh - 8) / 2, 1, selrow ? UI_WHITE : UI_INK, nm);
                ui_text(8 + rw - (int)strlen(sz) * 8 - 8, by + (rh - 8) / 2, 1, UI_DIM, sz);
            }
            if (n > VIS) { int th = VIS * (rh + gap) - gap, ty = ry0, maxs = n - VIS;
                int hh = th * VIS / n; if (hh < 12) hh = 12; int hy = ty + (th - hh) * top / maxs;
                ui_fill_round(UI_W - 6, ty, 3, th, 1, TH_TRACK);
                ui_fill_round(UI_W - 6, hy, 3, hh, 1, UI_NEON); }
            ui_present(); redraw = 0;
        }
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    }
    return -1;
}

static MoflexResult play_movie(const char *path) {
    cia_clear_selection();
    const char *title_src = path; char titlebuf[NAMELEN];
    if (cia_is_cia(path)) {
        CiaMoflex list[64];
        int nc = cia_list_moflex(path, list, 64);
        if (nc == 0) {
            msg_screen("NOT A MOVIE", "This CIA has no playable video inside\n(it's an app, or the content is encrypted).");
            branding_show();
            return MOFLEX_QUIT_BACK;
        }
        int sel = 0;
        if (nc > 1) {                                   /* several videos -> let the user choose */
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
    { long long rp = moflex_resume_get(path);   /* pre-played -> resume, start fresh, or back out */
      if (rp > 3000000) {
          int rc = resume_prompt(g_now_playing, rp);
          if (rc < 0) { cia_clear_selection(); branding_show(); return MOFLEX_QUIT_BACK; }   /* B -> back */
          if (rc == 0) moflex_resume_clear(path);                                            /* start over */
      } }
    MoflexResult r = moflex_play(path);
    cia_clear_selection();
    consoleInit(GFX_BOTTOM, NULL);
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
    branding_show();               /* restore the 3D logo on the top screen */
    return r;
}

/* ---------- browser (used by OPEN and MANAGE) ---------- */
/* returns: 1 = exit app, 0 = back to the player home, 2 = a file was picked (sel_out set) */

/* ---------- graphical (touch) Open-Video browser ---------- */
typedef struct { int x, y, w, h; const char *label; u16 accent; } ActBtn;
static ActBtn g_act[3] = {   /* .accent set live at draw time (theme colours aren't compile-time constant) */
    {   8, 210, 94, 26, "OPEN", 0 },
    { 112, 210, 94, 26, "INFO", 0 },
    { 216, 210, 96, 26, "Back", 0 },
};
static void icon_folder(int x, int y, u16 c) {
    ui_fill_round(x, y, 9, 4, 1, c);          /* tab */
    ui_fill_round(x, y + 3, 20, 13, 3, c);    /* body */
}
static void icon_movie(int x, int y, u16 c) {
    ui_frame_round(x, y + 1, 20, 14, 3, c, 1);
    ui_play(x + 11, y + 8, 8, c);
}
static void icon_file(int x, int y, u16 c) {   /* generic document (manage: non-movie files) */
    ui_frame_round(x + 2, y, 16, 16, 2, c, 1);
    ui_fill_round(x + 5, y + 4, 10, 1, 0, c);
    ui_fill_round(x + 5, y + 7, 10, 1, 0, c);
    ui_fill_round(x + 5, y + 10, 7, 1, 0, c);
}
static void strip_ext(char *d) {
    size_t L = strlen(d);
    if (L > 7 && !strcasecmp(d + L - 7, ".moflex")) d[L - 7] = 0;
    else if (L > 4 && !strcasecmp(d + L - 4, ".zip")) d[L - 4] = 0;
    else if (L > 4 && !strcasecmp(d + L - 4, ".cia")) d[L - 4] = 0;
}
static void browser_draw_gfx(int mode) {
    int manage = (mode == MODE_MANAGE);
    ui_begin(GFX_BOTTOM);
    ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);
    ui_text(10, 8, 2, UI_NEON, manage ? "MANAGE" : "FILESYSTEM");
    if (manage && move_pending) ui_text(UI_W - 96, 9, 1, UI_NEONC, "MOVE ready");
    else if (!manage && s_show_hidden) ui_text(UI_W - 74, 9, 1, UI_NEONP, "SYS on");
    /* current path (tail-trimmed to fit) */
    char pth[64]; int mc = (UI_W - 20) / 8;
    if ((int)strlen(cwd) > mc) snprintf(pth, sizeof pth, "...%s", cwd + strlen(cwd) - (mc - 3));
    else snprintf(pth, sizeof pth, "%s", cwd);
    ui_text(10, 30, 1, UI_DIM, pth);
    ui_fill_round(8, 42, UI_W - 16, 1, 0, TH_LINE);

    int maxs = nentries > BR_ROWS ? nentries - BR_ROWS : 0;
    if (scroll > maxs) scroll = maxs; if (scroll < 0) scroll = 0;
    if (nentries == 0) ui_text_center(UI_W / 2, 116, 1, UI_DIM, "(nothing here)");
    for (int r = 0; r < BR_ROWS; r++) {
        int i = scroll + r; if (i >= nentries) break;
        int ry = BR_LIST_Y + r * BR_ROWH, rw = UI_W - 16, rh = BR_ROWH - 4, selrow = (i == sel);
        if (selrow) {
            ui_glow_round(8, ry, rw, rh, 7, UI_NEON, 3, 16);
            ui_vgrad_round(8, ry, rw, rh, 7, TH_SEL, TH_SELLO);
            ui_frame_round(8, ry, rw, rh, 7, UI_NEON, 1);
        }
        if (entries[i].is_dir)      icon_folder(18, ry + 3, UI_RGB(240, 205, 110));
        else if (is_moflex(entries[i].name)) icon_movie(18, ry + 3, UI_NEONC);
        else                        icon_file(18, ry + 3, UI_DIM);
        char disp[NAMELEN]; snprintf(disp, sizeof disp, "%s", entries[i].name);
        if (!entries[i].is_dir && !manage) strip_ext(disp);   /* manage shows real filenames */
        int tx = 46, txr = UI_W - 16, ty = ry + (rh - 8) / 2, tw = ui_text_w(1, disp);
        u16 tc = selrow ? UI_WHITE : UI_INK;
        if (tw <= txr - tx) {
            ui_text(tx, ty, 1, tc, disp);
        } else if (selrow) {   /* marquee the selected long title (reveal, then reset) */
            ui_text_clipped(tx - g_mq_off, ty, 1, tc, disp, tx, txr);
        } else {
            int cmax = (txr - tx) / 8; if ((int)strlen(disp) > cmax) disp[cmax] = 0;
            ui_text(tx, ty, 1, tc, disp);
        }
    }
    if (nentries > BR_ROWS) {   /* scrollbar */
        int th = BR_ROWS * BR_ROWH - 6, ty = BR_LIST_Y;
        int hh = th * BR_ROWS / nentries; if (hh < 12) hh = 12;
        int hy = ty + (th - hh) * scroll / maxs;
        ui_fill_round(UI_W - 6, ty, 3, th, 1, TH_TRACK);
        ui_fill_round(UI_W - 6, hy, 3, hh, 1, UI_NEON);
    }
    const char *bl0 = manage ? (move_pending ? "PASTE" : "EDIT") : "OPEN";
    const char *bl1 = manage ? (s_manage_movies_only ? "MOVIES" : "ALL") : "INFO";
    ui_button(g_act[0].x, g_act[0].y, g_act[0].w, g_act[0].h, bl0, 0, UI_NEON);
    ui_button(g_act[1].x, g_act[1].y, g_act[1].w, g_act[1].h, bl1, 0, UI_NEONC);
    ui_button(g_act[2].x, g_act[2].y, g_act[2].w, g_act[2].h, "Back", 0, UI_NEONP);
    ui_present();
}
static void browser_redraw(int mode) { browser_draw_gfx(mode); }
#define BR_FOLLOW() do { \
        if (sel < scroll) scroll = sel; else if (sel >= scroll + BR_ROWS) scroll = sel - BR_ROWS + 1; } while (0)

static int browser(int mode, char *sel_out, size_t sel_cap) {
    s_filter_movies = (mode != MODE_MANAGE) ? 1 : s_manage_movies_only;   /* play=movies; manage=toggle */
    scan();
    if (s_presel[0]) {   /* BACK from the player: highlight + scroll to the movie that was playing */
        for (int i = 0; i < nentries; i++)
            if (!strcmp(entries[i].name, s_presel)) { sel = i; break; }
        s_presel[0] = 0;
        BR_FOLLOW();
    }
    browser_redraw(mode);
    if (mode == MODE_PLAY) gfxSet3D(false);   /* top becomes a 2D poster/info panel while browsing */
    int hfu = 0, hfd = 0;
    int top_sel = -1, top_settle = 0, top_pending = 1;   /* debounced top-screen render (play mode) */
    int tdown = 0, tx0 = 0, ty0 = 0, tsc0 = 0, tmoved = 0;   /* touch state (play mode) */
    while (aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown();
        u32 kh = hidKeysHeld();
        u32 ku = hidKeysUp();
        if (k & KEY_START) { if (mode == MODE_PLAY) branding_show(); return 1; }

        if (nentries) {
            if (nav_repeat(k, kh, NAV_DOWN, &hfd)) { if (sel < nentries - 1) sel++; BR_FOLLOW(); browser_redraw(mode); }
            if (nav_repeat(k, kh, NAV_UP,   &hfu)) { if (sel > 0) sel--;           BR_FOLLOW(); browser_redraw(mode); }
            if (k & KEY_RIGHT) { sel += BR_ROWS; if (sel > nentries - 1) sel = nentries - 1; BR_FOLLOW(); browser_redraw(mode); }
            if (k & KEY_LEFT)  { sel -= BR_ROWS; if (sel < 0) sel = 0; BR_FOLLOW(); browser_redraw(mode); }
        }
        if (k & KEY_SELECT) { scan(); browser_redraw(mode); top_pending = 1; top_sel = -1; }
        if (k & KEY_Y) {
            if (mode == MODE_MANAGE) {   /* toggle: all files vs movies only */
                s_manage_movies_only = !s_manage_movies_only;
                s_filter_movies = s_manage_movies_only;
                if (sel > 0) sel = 0;
                scan(); browser_redraw(mode);
            } else {                     /* play mode: reveal / hide system folders + dotfiles */
                s_show_hidden = !s_show_hidden;
                sel = 0; scroll = 0; scan(); browser_redraw(mode); top_pending = 1; top_sel = -1;
            }
        }
        if (k & KEY_R && move_pending) { do_paste(); browser_redraw(mode); }
        if (k & KEY_B) {
            if (at_root()) { if (mode == MODE_PLAY) branding_show(); return 0; }   /* leave -> home menu */
            go_up(); browser_redraw(mode); top_pending = 1; top_sel = -1;
        }
        if (k & KEY_X && nentries) {
            if (mode == MODE_PLAY) getinfo_menu();   /* Open Video: X = Get Info (Manage lives in its own screen) */
            else                   manage_menu();
            scan(); browser_redraw(mode); top_pending = 1; top_sel = -1;
        }
        if (k & KEY_A && nentries) {
            if (entries[sel].is_dir) {
                enter_dir(entries[sel].name);
                browser_redraw(mode); top_pending = 1; top_sel = -1;
            } else if (mode == MODE_MANAGE) {
                manage_menu(); scan(); browser_redraw(mode);
            } else {   /* MODE_PLAY: hand the selection back to main to play */
                if (sel_out) snprintf(sel_out, sel_cap, "%s%s", cwd, entries[sel].name);
                return 2;
            }
        }
        /* ---- touch: drag to scroll, tap a row, tap an action button (both modes) ---- */
        {
            touchPosition tp; hidTouchRead(&tp);
            if (k & KEY_TOUCH) { tdown = 1; tx0 = tp.px; ty0 = tp.py; tsc0 = scroll; tmoved = 0; }
            else if ((kh & KEY_TOUCH) && tdown) {
                int dy = tp.py - ty0;
                if (dy > 6 || dy < -6) tmoved = 1;
                if (tmoved && nentries > BR_ROWS) {
                    int maxs = nentries - BR_ROWS, ns = tsc0 - dy / BR_ROWH;
                    if (ns < 0) ns = 0; if (ns > maxs) ns = maxs;
                    if (ns != scroll) { scroll = ns; browser_redraw(mode); }
                }
            } else if ((ku & KEY_TOUCH) && tdown) {
                tdown = 0;
                if (!tmoved) {
                    int acted = 0;
                    for (int i = 0; i < 3 && !acted; i++) {   /* action buttons */
                        ActBtn *b = &g_act[i];
                        if (tx0 < b->x || tx0 >= b->x + b->w || ty0 < b->y || ty0 >= b->y + b->h) continue;
                        acted = 1;
                        if (i == 2) {   /* Back */
                            if (at_root()) { if (mode == MODE_PLAY) branding_show(); return 0; }
                            go_up(); browser_redraw(mode); top_pending = 1; top_sel = -1;
                        } else if (i == 1) {   /* INFO (play) / FILTER (manage) */
                            if (mode == MODE_MANAGE) { s_manage_movies_only = !s_manage_movies_only;
                                s_filter_movies = s_manage_movies_only; sel = 0; scroll = 0; scan(); browser_redraw(mode); }
                            else if (nentries) { getinfo_menu(); scan(); browser_redraw(mode); top_pending = 1; top_sel = -1; }
                        } else {   /* i==0: OPEN (play) / PASTE|EDIT (manage) */
                            if (mode == MODE_MANAGE) {
                                if (move_pending) { do_paste(); browser_redraw(mode); }
                                else if (nentries) { manage_menu(); scan(); browser_redraw(mode); }
                            } else if (nentries) {
                                if (entries[sel].is_dir) { enter_dir(entries[sel].name); browser_redraw(mode); top_pending = 1; top_sel = -1; }
                                else { if (sel_out) snprintf(sel_out, sel_cap, "%s%s", cwd, entries[sel].name); return 2; }
                            }
                        }
                    }
                    if (!acted && ty0 >= BR_LIST_Y && nentries) {   /* tap a list row */
                        int i = scroll + (ty0 - BR_LIST_Y) / BR_ROWH;
                        if (i >= 0 && i < nentries && i < scroll + BR_ROWS) {
                            if (i != sel) {   /* first tap: just select (so you can then tap INFO / OPEN) */
                                sel = i; top_sel = -1; top_pending = 1; browser_redraw(mode);
                            } else {          /* tap the already-selected row: open it */
                                top_sel = -1; top_pending = 1;
                                if (entries[i].is_dir) { enter_dir(entries[i].name); browser_redraw(mode); }
                                else if (mode == MODE_MANAGE) { manage_menu(); scan(); browser_redraw(mode); }
                                else { if (sel_out) snprintf(sel_out, sel_cap, "%s%s", cwd, entries[i].name); return 2; }
                            }
                        }
                    }
                }
            }
        }
        /* marquee: reveal the selected row's long title, then reset (both modes) */
        {
            int nr = 0;
            if (nentries && sel >= scroll && sel < scroll + BR_ROWS) {
                char d[NAMELEN]; snprintf(d, sizeof d, "%s", entries[sel].name);
                if (!entries[sel].is_dir && mode == MODE_PLAY) strip_ext(d);
                marquee_off(1000000 + sel, ui_text_w(1, d), (UI_W - 16) - 46, &nr);
            } else marquee_off(-2, 0, 0, &nr);
            if (nr) browser_redraw(mode);
        }
        /* debounced top-screen panel for the highlighted entry (play mode only) */
        if (mode == MODE_PLAY) {
            if (sel != top_sel) { top_sel = sel; top_settle = 0; top_pending = 1; }
            if (top_pending && ++top_settle >= 5) {
                if (!nentries) branding_show_2d();
                else {
                    char full[PATHLEN + NAMELEN];
                    snprintf(full, sizeof full, "%s%s", cwd, entries[sel].name);
                    browser_show_top(entries[sel].is_dir, entries[sel].name, full);
                }
                top_pending = 0;
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
    {   8, 200,  94, 32, "OPEN VIDEO", 0 },
    { 110, 200, 100, 32, "MANAGE",     2 },
    { 218, 200,  94, 32, "ADD VIDEO", 1 },
};
#define HOME_PCX (UI_W / 2)   /* PLAY circle centre */
#define HOME_PCY 120
#define HOME_PR  27

/* the little theme swatch on the home screen (tap it, or press Y, to open Themes) */
#define THSW_X 8
#define THSW_Y 6
#define THSW_W 54
#define THSW_H 16

/* CUSTOM colour editor. Its own chrome is fixed (stays readable whatever colours
 * you pick); a small preview panel shows the live theme. Selecting Custom is
 * implied on entry, and everything is saved on the way out. */
static void custom_editor(void) {
    theme_set(theme_custom_index());        /* activate Custom so the preview is live */
    int ne = theme_elem_count();
    int elem = 0, ch = 0, redraw = 1;
    /* fixed editor chrome (deliberately NOT themed) */
    const u16 CBG = UI_RGB(14,15,20), CFG = UI_RGB(232,236,244), CMUT = UI_RGB(120,128,145),
              CACC = UI_RGB(70,210,255), CTRK = UI_RGB(40,42,52);
    const u16 chcol[3] = { UI_RGB(255,90,90), UI_RGB(90,220,110), UI_RGB(96,150,255) };
    const int TKX = 44, TKW = 208, ROWY = 60, ROWH = 32;
    while (aptMainLoop()) {
        hidScanInput();
        u32 kd = hidKeysDown(), kr = hidKeysDownRepeat();
        if (kd & (KEY_B | KEY_START | KEY_X)) break;
        if (kd & KEY_L) { elem = (elem + ne - 1) % ne; redraw = 1; }
        if (kd & KEY_R) { elem = (elem + 1) % ne;      redraw = 1; }
        if (kd & KEY_UP)   { ch = (ch + 2) % 3; redraw = 1; }
        if (kd & KEY_DOWN) { ch = (ch + 1) % 3; redraw = 1; }
        if (kr & KEY_LEFT)  { theme_custom_set(elem, ch, theme_custom_get(elem, ch) - 3); redraw = 1; }
        if (kr & KEY_RIGHT) { theme_custom_set(elem, ch, theme_custom_get(elem, ch) + 3); redraw = 1; }
        if (kd & KEY_Y) { theme_custom_reset(); redraw = 1; }
        if (kd & KEY_TOUCH) {
            touchPosition t; hidTouchRead(&t);
            if (t.py < 52) {                              /* element arrows */
                if (t.px < 60)       { elem = (elem + ne - 1) % ne; redraw = 1; }
                else if (t.px > 260) { elem = (elem + 1) % ne;      redraw = 1; }
            }
            for (int c = 0; c < 3; c++) {                 /* tap a slider track to set it */
                int y = ROWY + c * ROWH;
                if ((int)t.py >= y - 4 && (int)t.py < y + 22) {
                    int v = ((int)t.px - TKX) * 255 / TKW;
                    ch = c; theme_custom_set(elem, c, v); redraw = 1;
                }
            }
        }
        if (redraw) {
            ui_begin(GFX_BOTTOM);
            ui_clear(CBG);
            ui_text_center(UI_W / 2, 8, 2, CACC, "CUSTOM COLORS");
            /* element selector */
            ui_text(20, 32, 2, CMUT, "<");
            ui_text_center(UI_W / 2 - 8, 32, 2, CFG, theme_elem_name(elem));
            ui_text(UI_W - 30, 32, 2, CMUT, ">");
            { u16 ec = UI_RGB(theme_custom_get(elem,0), theme_custom_get(elem,1), theme_custom_get(elem,2));
              ui_fill_round(UI_W - 70, 32, 30, 16, 4, ec); ui_frame_round(UI_W - 70, 32, 30, 16, 4, CMUT, 1); }
            /* R/G/B sliders */
            for (int c = 0; c < 3; c++) {
                int y = ROWY + c * ROWH;
                ui_text(22, y, 2, c == ch ? CACC : CFG, c == 0 ? "R" : c == 1 ? "G" : "B");
                ui_fill_round(TKX, y + 4, TKW, 8, 4, CTRK);
                int val = theme_custom_get(elem, c);
                int fw = val * TKW / 255;
                if (fw > 0) ui_fill_round(TKX, y + 4, fw, 8, 4, chcol[c]);
                int kx = TKX + fw;
                ui_fill_round(kx - 4, y + 1, 8, 14, 4, c == ch ? CACC : CFG);
                char vb[8]; snprintf(vb, sizeof vb, "%d", val);
                ui_text(TKX + TKW + 8, y, 2, CFG, vb);
            }
            /* live preview panel (uses the actual theme) */
            ui_text(20, ROWY + 3 * ROWH + 2, 1, CMUT, "PREVIEW");
            int py = ROWY + 3 * ROWH + 14;
            ui_fill_round(16, py, UI_W - 32, 34, 8, UI_BG2);
            ui_frame_round(16, py, UI_W - 32, 34, 8, TH_LINE, 1);
            ui_text(26, py + 6, 1, UI_INK, "Sample");
            ui_fill_round(26, py + 20, 8, 8, 3, UI_NEON);
            ui_fill_round(38, py + 20, 8, 8, 3, UI_NEONP);
            ui_fill_round(50, py + 20, 8, 8, 3, UI_NEONC);
            ui_button(UI_W - 116, py + 4, 92, 26, "PLAY", 1, UI_NEON);
            ui_text_center(UI_W / 2, 228, 1, CMUT, "L/R item  dpad adjust  Y reset  B done");
            ui_present(); redraw = 0;
        }
        gspWaitForVBlank();
    }
    theme_save();
}

/* THEMES picker: a live-previewing list -- moving the selection recolors the
 * whole UI instantly, and the choice is saved to SD on the way out. */
static void themes_screen(void) {
    int n = theme_count();
    int sel = theme_current();
    int redraw = 1;
    const int ROW_Y0 = 46, ROW_H = 24;
    while (aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown();
        if (k & (KEY_A | KEY_B | KEY_START | KEY_Y)) break;
        if (k & KEY_X) { custom_editor(); sel = theme_current(); redraw = 1; }   /* jump to + edit Custom */
        if (k & KEY_DOWN) { sel = (sel + 1) % n;     theme_set(sel); redraw = 1; }
        if (k & KEY_UP)   { sel = (sel + n - 1) % n; theme_set(sel); redraw = 1; }
        if (k & KEY_TOUCH) {
            touchPosition t; hidTouchRead(&t);
            int i = ((int)t.py - ROW_Y0) / ROW_H;
            if (i >= 0 && i < n) { sel = i; theme_set(sel); redraw = 1; }
        }
        if (redraw) {
            ui_begin(GFX_BOTTOM);
            ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);
            ui_text_center(UI_W / 2, 14, 2, UI_NEON, "THEMES");
            for (int i = 0; i < n; i++) {
                int ry = ROW_Y0 + i * ROW_H;
                if (i == sel) {
                    ui_fill_round(10, ry, UI_W - 20, ROW_H - 4, 6, TH_SEL);
                    ui_frame_round(10, ry, UI_W - 20, ROW_H - 4, 6, UI_NEON, 1);
                }
                u16 a, a2, a3, bg;
                theme_preview(i, &a, &a2, &a3, &bg);
                int sx = 18, sy = ry + 3, sw = 30, sh = ROW_H - 10;
                ui_fill_round(sx, sy, sw, sh, 3, bg);
                ui_frame_round(sx, sy, sw, sh, 3, TH_LINE, 1);
                ui_fill_round(sx + 4,  sy + sh / 2 - 2, 5, 5, 2, a);
                ui_fill_round(sx + 12, sy + sh / 2 - 2, 5, 5, 2, a2);
                ui_fill_round(sx + 20, sy + sh / 2 - 2, 5, 5, 2, a3);
                ui_text(58, ry + 6, 1, i == sel ? UI_WHITE : UI_INK, theme_name(i));
            }
            ui_text_center(UI_W / 2, 224, 1, UI_DIM, "up/dn preview   A/B apply   X custom");
            ui_present(); redraw = 0;
        }
        gspWaitForVBlank();
    }
    theme_save();
}

static void home_draw(int bsel, long long rpos) {
    ui_begin(GFX_BOTTOM);
    ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);   /* dark gradient backdrop */

    int loaded = g_now_playing[0] != 0;

    /* title + neon divider */
    ui_text_center(UI_W / 2, 12, 2, UI_NEON, "CLOWNSEC");
    ui_text(UI_W - 52, 3, 1, UI_DIM, APP_VERSION);
    { const char *bt = "b" BUILD_TAG;   /* build stamp under the version -> "which 1.0.0 is this?" */
      ui_text(UI_W - 6 - ui_text_w(1, bt), 14, 1, UI_DIM, bt); }

    /* theme swatch button (tap or press Y) */
    {
        u16 a, a2, a3, bg; theme_preview(theme_current(), &a, &a2, &a3, &bg);
        ui_fill_round(THSW_X, THSW_Y, THSW_W, THSW_H, 4, UI_BG2);
        ui_frame_round(THSW_X, THSW_Y, THSW_W, THSW_H, 4, TH_LINE, 1);
        ui_fill_round(THSW_X + 6,  THSW_Y + 5, 6, 6, 2, a);
        ui_fill_round(THSW_X + 15, THSW_Y + 5, 6, 6, 2, a2);
        ui_fill_round(THSW_X + 24, THSW_Y + 5, 6, 6, 2, a3);
        ui_text(THSW_X + 36, THSW_Y + 4, 1, UI_DIM, "Y");
    }
    ui_text_center(UI_W / 2, 32, 1, UI_NEONP, "3DS VIDEO PLAYER");
    ui_glow_round(28, 46, UI_W - 56, 2, 1, UI_NEON, 3, 34);
    ui_fill_round(28, 46, UI_W - 56, 2, 1, UI_NEON);

    /* now-playing card */
    int cx = 16, cy = 58, cw = UI_W - 32, ch = 128;
    ui_fill_round(cx, cy, cw, ch, 12, UI_BG2);
    ui_frame_round(cx, cy, cw, ch, 12, TH_LINE, 1);

    if (loaded) {
        char nm[80]; snprintf(nm, sizeof(nm), "%s", g_now_playing);
        int maxc = (cw - 16) / 8; if ((int)strlen(nm) > maxc) nm[maxc] = 0;
        ui_text_center(UI_W / 2, cy + 12, 1, UI_NEONC, nm);
    } else {
        ui_text_center(UI_W / 2, cy + 12, 1, UI_DIM, "No video loaded");
    }

    /* big glowing PLAY circle */
    int pcx = HOME_PCX, pcy = HOME_PCY, R = HOME_PR;
    u16 pcol = loaded ? UI_NEON : UI_INK;
    ui_glow_round(pcx - R, pcy - R, 2 * R, 2 * R, R, pcol, 6, 22);
    ui_vgrad_round(pcx - R, pcy - R, 2 * R, 2 * R, R, UI_BG2, TH_BG1);
    ui_frame_round(pcx - R, pcy - R, 2 * R, 2 * R, R, pcol, 2);
    ui_play(pcx + 3, pcy, 22, pcol);

    ui_text_center(pcx, cy + ch - 30, 1, UI_INK, loaded ? "PLAY / RESUME" : "PLAY - choose a video");
    if (loaded && rpos > 0) {
        long t = (long)(rpos / 1000000);
        char rt[40]; snprintf(rt, sizeof(rt), "resume at %ld:%02ld", t / 60, t % 60);
        ui_text_center(pcx, cy + ch - 16, 1, UI_DIM, rt);
    }

    /* three neon buttons */
    for (int i = 0; i < 3; i++) {
        HomeBtn *b = &g_btns[i];
        ui_button(b->x, b->y, b->w, b->h, b->label, i == bsel, UI_NEON);
    }
}

static int home_gui(void) {
    static int bsel = 0;   /* remember the highlighted button across screens (B back keeps it) */
    int redraw = 1;
    /* read the resume position ONCE (not every frame -- that hammered the SD card) */
    long long rpos = g_now_playing_path[0] ? moflex_resume_get(g_now_playing_path) : 0;
    while (aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown();
        if (k & KEY_START) return -1;
        if (k & KEY_Y) { themes_screen(); redraw = 1; }
        if (k & KEY_RIGHT) { bsel = (bsel + 1) % 3; redraw = 1; }
        if (k & KEY_LEFT)  { bsel = (bsel + 2) % 3; redraw = 1; }
        if (k & KEY_A) return g_btns[bsel].choice;

        if (k & KEY_TOUCH) {
            touchPosition t; hidTouchRead(&t);
            if (t.px >= THSW_X && t.px < THSW_X + THSW_W &&
                t.py >= THSW_Y && t.py < THSW_Y + THSW_H) { themes_screen(); redraw = 1; continue; }
            for (int i = 0; i < 3; i++) {
                HomeBtn *b = &g_btns[i];
                if (t.px >= b->x && t.px < b->x + b->w && t.py >= b->y && t.py < b->y + b->h)
                    return b->choice;
            }
            /* PLAY button: resume the loaded movie, or open the browser if none */
            if (t.px > HOME_PCX - HOME_PR - 6 && t.px < HOME_PCX + HOME_PR + 6 &&
                t.py > HOME_PCY - HOME_PR - 6 && t.py < HOME_PCY + HOME_PR + 6)
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

/* OPEN VIDEO chooser: Library (categorized) or Filesystem (folders). Returns 0 = back, 1 = Library, -1 = cancel. */
static int open_pick(void) {
    int sel = 0, redraw = 1;
    while (aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown();
        if (k & KEY_B) return -1;
        if (k & (KEY_DOWN | KEY_RIGHT)) { sel = 1; redraw = 1; }
        if (k & (KEY_UP | KEY_LEFT))    { sel = 0; redraw = 1; }
        if (k & KEY_A) return sel;
        if (k & KEY_TOUCH) { touchPosition tp; hidTouchRead(&tp);
            if (tp.py >= 66 && tp.py < 118)  return 0;
            if (tp.py >= 134 && tp.py < 186) return 1; }
        if (redraw) {
            ui_begin(GFX_BOTTOM);
            ui_vgrad_round(0, 0, UI_W, UI_H, 0, TH_BG1, UI_BG);
            ui_text_center(UI_W / 2, 18, 2, UI_NEON, "OPEN VIDEO");
            ui_button(34, 66, UI_W - 68, 52, "LIBRARY", sel == 0, UI_NEON);
            ui_button(34, 134, UI_W - 68, 52, "FILESYSTEM", sel == 1, UI_NEONP);
            ui_present(); redraw = 0;
        }
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    }
    return -1;
}

/* OPEN VIDEO: pick a source, then a movie. Returns 0 = home, 1 = exit app, 2 = play out[]. */
static int open_video(char *out, size_t cap) {
    for (;;) {
        int pick = open_pick();
        if (pick < 0) return 0;                                 /* back -> home */
        if (pick == 0) { if (library_view(out, cap)) return 2;  /* Library; backed out -> chooser */ }
        else { int r = browser(MODE_PLAY, out, cap);            /* Filesystem */
               if (r == 1) return 1;
               if (r == 2) return 2; }                           /* r==0 backed out -> chooser */
    }
}

/* Play a movie and handle the player's exit buttons. Returns 1 if the app should exit, else 0 (home).
 *  - B / BACK   -> TRUE back: to the home screen if the movie was resumed from home, else re-open the
 *                  file browser at the movie's folder with it highlighted.
 *  - OPEN VIDEO -> the source chooser (Library / Filesystem).
 *  - MANAGE     -> the manage browser, then home.
 *  - ADD VIDEO  -> the add-video menu (download / upload), then home. */
static int play_and_handle(const char *path, int from_home) {
    MoflexResult r = play_movie(path);
    for (;;) {
        char np[PATHLEN + NAMELEN];
        if (r == MOFLEX_QUIT_OPEN) {
            int b = open_video(np, sizeof np);
            if (b == 1) return 1;
            if (b == 2) { from_home = 0; r = play_movie(np); continue; }   /* picked via browser now */
            return 0;
        }
        if (r == MOFLEX_QUIT_BACK) {
            if (from_home) return 0;                 /* came from the home screen -> back IS home */
            if (g_now_playing_path[0]) browser_preselect_path(g_now_playing_path);   /* highlight that movie */
            int b = browser(MODE_PLAY, np, sizeof np);
            if (b == 1) return 1;
            if (b == 2) { r = play_movie(np); continue; }
            return 0;                                /* backed out to root -> home */
        }
        if (r == MOFLEX_QUIT_MANAGE) { browser(MODE_MANAGE, NULL, 0); return 0; }
        if (r == MOFLEX_QUIT_ADD)    { add_movies_menu(); return 0; }
        return (r == MOFLEX_QUIT_EXIT) ? 1 : 0;      /* EOF -> home; app-close -> exit */
    }
}

/* ---------- main ---------- */

#define SOC_BUF_SZ 0x100000
int main(void) {
    osSetSpeedupEnable(true);   /* unlock New 3DS 804MHz clock (no-op on old 3DS) */
    gfxInitDefault();
    ndspInit();
    theme_load();               /* restore the saved theme before anything draws */

    /* sockets: init once, shared by the web server (UPLOAD) and curl (DOWNLOAD) */
    u32 *soc_buf = (u32 *)memalign(0x1000, SOC_BUF_SZ);
    if (soc_buf) socInit(soc_buf, SOC_BUF_SZ);

    gfxSetScreenFormat(GFX_BOTTOM, GSP_RGB565_OES);   /* match ui_gfx (u16) */
    consoleInit(GFX_BOTTOM, NULL);
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
    branding_show();                                  /* 3D CLOWNSEC logo on top */

    startup_new_movie_check();   /* movies added outside the app -> offer rescan (+ art/info) */

    int running = 1;
    while (running && aptMainLoop()) {
        int choice = home_gui();
        if (choice < 0) break;
        if (choice == 0) {                       /* OPEN -> Library/Filesystem chooser -> play -> home */
            char path[PATHLEN + NAMELEN];
            int r = open_video(path, sizeof(path));
            if (r == 1) running = 0;
            else if (r == 2 && play_and_handle(path, 0)) running = 0;
        }
        else if (choice == 1) { add_movies_menu(); }
        else if (choice == 2) { if (browser(MODE_MANAGE, NULL, 0) == 1) running = 0; }
        else if (choice == 3) {                  /* PLAY: resume the loaded movie; B goes back HERE */
            if (g_now_playing_path[0] && play_and_handle(g_now_playing_path, 1)) running = 0;
        }
    }

    downloader_exit();
    socExit();
    ndspExit();
    gfxExit();
    return 0;
}
