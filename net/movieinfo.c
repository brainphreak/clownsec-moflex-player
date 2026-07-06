#include "movieinfo.h"
#include "poster.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

/* basename of a path (after the last '/') */
static const char *base_of(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* the movie's filename with its extension removed -> the moviedata key (kept verbatim so users
 * can author "<name>.nfo" to match their movie file). */
static void stem_of(const char *moviepath, char *out, size_t cap) {
    snprintf(out, cap, "%s", base_of(moviepath));
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
}

static void data_path(const char *moviepath, const char *ext, char *out, size_t cap) {
    char stem[192]; stem_of(moviepath, stem, sizeof stem);
    snprintf(out, cap, "%s/%s%s", MOVIEDATA_DIR, stem, ext);
}

static int file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* "Complete" = has BOTH a description (.nfo) AND a poster -- so the batch scan skips a title
 * only when it truly has art + info, and retries anything still missing a poster. */
int movieinfo_have(const char *moviepath) {
    char p[320];
    data_path(moviepath, ".nfo", p, sizeof p); if (!file_exists(p)) return 0;   /* need description */
    data_path(moviepath, ".p565", p, sizeof p); if (file_exists(p)) return 1;   /* need a poster */
    data_path(moviepath, ".jpg",  p, sizeof p); if (file_exists(p)) return 1;
    data_path(moviepath, ".png",  p, sizeof p); if (file_exists(p)) return 1;
    return 0;
}

int movieinfo_load(const char *moviepath, CatEntry *out) {
    memset(out, 0, sizeof *out);
    out->is3d = -1;   /* unknown unless the .nfo says so -> caller falls back to the filename */
    char p[320]; data_path(moviepath, ".nfo", p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *nl = strpbrk(line, "\r\n"); if (nl) *nl = 0;
        char *c = strchr(line, ':');
        if (!c) continue;
        *c = 0; const char *key = line, *val = c + 1;
        while (*val == ' ' || *val == '\t') val++;
        if      (!strcasecmp(key, "title"))   { snprintf(out->title, sizeof out->title, "%s", val);
                                                 snprintf(out->name,  sizeof out->name,  "%s", val); }
        else if (!strcasecmp(key, "year"))    out->year    = atoi(val);
        else if (!strcasecmp(key, "runtime")) out->runtime = atoi(val);
        else if (!strcasecmp(key, "genres"))  snprintf(out->genres, sizeof out->genres, "%s", val);
        else if (!strcasecmp(key, "desc"))    snprintf(out->desc,   sizeof out->desc,   "%s", val);
        else if (!strcasecmp(key, "3d"))      out->is3d = (!strcasecmp(val, "yes") || val[0] == '1' ||
                                                           !strcasecmp(val, "true")) ? 1 : 0;
    }
    fclose(f);
    if (!out->name[0]) stem_of(moviepath, out->name, sizeof out->name);   /* fall back to the filename */
    snprintf(out->fname, sizeof out->fname, "%s", base_of(moviepath));    /* keeps the 2D/3D badge working */
    struct stat st; if (stat(moviepath, &st) == 0) out->size = (long long)st.st_size;
    return 1;
}

int movieinfo_poster(const char *moviepath, u16 *out, int pw, int ph) {
    size_t need = (size_t)pw * ph * sizeof(u16);
    char cache[320]; data_path(moviepath, ".p565", cache, sizeof cache);

    FILE *cf = fopen(cache, "rb");                    /* decoded-cache hit -> straight read */
    if (cf) { size_t rd = fread(out, 1, need, cf); fclose(cf); if (rd == need) return 1; }

    char img[320];                                    /* else decode a sidecar image and cache it */
    data_path(moviepath, ".jpg", img, sizeof img);
    if (!file_exists(img)) { data_path(moviepath, ".png", img, sizeof img); if (!file_exists(img)) return 0; }
    if (!poster_decode_file(img, out, pw, ph)) return 0;

    mkdir("sdmc:/moflex_player", 0777);
    mkdir(MOVIEDATA_DIR, 0777);
    FILE *wf = fopen(cache, "wb");
    if (wf) { fwrite(out, 1, need, wf); fclose(wf); }
    return 1;
}

void movieinfo_save(const char *moviepath, const CatEntry *e, const u16 *poster, int pw, int ph) {
    mkdir("sdmc:/moflex_player", 0777);
    mkdir(MOVIEDATA_DIR, 0777);

    char p[320]; data_path(moviepath, ".nfo", p, sizeof p);
    FILE *f = fopen(p, "wb");
    if (f) {
        const char *title = e->title[0] ? e->title : e->name;
        fprintf(f, "title: %s\n", title);
        if (e->year)    fprintf(f, "year: %d\n",    e->year);
        if (e->runtime) fprintf(f, "runtime: %d\n", e->runtime);
        if (e->genres[0]) fprintf(f, "genres: %s\n", e->genres);
        if (e->is3d >= 0) fprintf(f, "3d: %s\n", e->is3d ? "yes" : "no");
        if (e->desc[0])   fprintf(f, "desc: %s\n",   e->desc);
        fclose(f);
    }
    if (poster) {
        data_path(moviepath, ".p565", p, sizeof p);
        FILE *pf = fopen(p, "wb");
        if (pf) { fwrite(poster, 1, (size_t)pw * ph * sizeof(u16), pf); fclose(pf); }
    }
}
