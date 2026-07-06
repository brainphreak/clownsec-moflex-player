#include "catalog.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static int is_unreserved(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
}
/* percent-encode a raw filename into a URL path component */
static void url_encode(const char *s, char *o, size_t cap) {
    static const char *hex = "0123456789ABCDEF";
    size_t j = 0;
    for (; *s && j + 4 < cap; s++) {
        unsigned c = (unsigned char)*s;
        if (is_unreserved(c)) o[j++] = (char)c;
        else { o[j++] = '%'; o[j++] = hex[c >> 4]; o[j++] = hex[c & 15]; }
    }
    o[j] = 0;
}

/* Make an existing URL safe without double-encoding: percent-encode only clearly-unsafe
 * characters (spaces, brackets, quotes, control), leaving %XX, '/', ':', '?', '&', '=' intact.
 * Handles archive.org links that carry raw spaces while not breaking already-encoded ones. */
static void url_fixup(const char *s, char *o, size_t cap) {
    static const char *hex = "0123456789ABCDEF";
    size_t j = 0;
    for (; *s && j + 4 < cap; s++) {
        unsigned c = (unsigned char)*s;
        if (c <= 0x20 || c == '[' || c == ']' || c == '"' || c == '<' || c == '>' ||
            c == '{' || c == '}' || c == '|' || c == '\\' || c == '^' || c == '`') {
            o[j++] = '%'; o[j++] = hex[c >> 4]; o[j++] = hex[c & 15];
        } else o[j++] = (char)c;
    }
    o[j] = 0;
}

static int is_drive_url(const char *u) {
    return u && (strstr(u, "drive.google.com") || strstr(u, "drive.usercontent.google.com")) != NULL;
}
/* pull a Google Drive file id out of a /file/d/<id>/ or ?id=<id> URL, or a bare-id filename field */
static int drive_id(const char *url, const char *fname, char *out, size_t cap) {
    const char *p = strstr(url, "/d/");
    if (p) { p += 3; size_t j = 0; while (*p && *p != '/' && *p != '?' && j + 1 < cap) out[j++] = *p++; out[j] = 0; if (j) return 1; }
    p = strstr(url, "id=");
    if (p) { p += 3; size_t j = 0; while (*p && *p != '&' && j + 1 < cap) out[j++] = *p++; out[j] = 0; if (j) return 1; }
    if (fname && fname[0] && !strchr(fname, '.') && strlen(fname) >= 20) { snprintf(out, cap, "%s", fname); return 1; }
    return 0;
}
/* sanitize a title into a FAT-safe base filename (no extension -- sniffed after download) */
static void savename(const char *title, int year, char *out, size_t cap) {
    size_t j = 0;
    for (const char *s = title; *s && j + 1 < cap; s++) {
        char c = *s;
        if (strchr("/\\:*?\"<>|", c)) c = ' ';
        out[j++] = c;
    }
    out[j] = 0;
    if (year > 0) { size_t k = strlen(out); snprintf(out + k, cap - k, " (%d)", year); }
}

static const char *sgets(cJSON *o, const char *k) {
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, k));
    return v ? v : "";
}
static int sgeti(cJSON *o, const char *k) {
    cJSON *n = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(n) ? n->valueint : 0;
}
static long long sgeti64(cJSON *o, const char *k) {   /* file sizes exceed int32 */
    cJSON *n = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(n) ? (long long)n->valuedouble : 0;
}
static void join_genres(cJSON *o, const char *key, char *out, size_t cap) {
    out[0] = 0;
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsArray(arr)) { const char *s = sgets(o, key); snprintf(out, cap, "%s", s); return; }
    size_t j = 0; cJSON *g;
    cJSON_ArrayForEach(g, arr) {
        const char *s = cJSON_GetStringValue(g);
        if (!s) continue;
        int need = (int)strlen(s) + (j ? 2 : 0);
        if (j + need + 1 >= cap) break;
        if (j) { out[j++] = ','; out[j++] = ' '; }
        strcpy(out + j, s); j += strlen(s);
    }
    out[j] = 0;
}

static int ends_ci(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    if (lf > ls) return 0;
    for (size_t i = 0; i < lf; i++)
        if (tolower((unsigned char)s[ls - lf + i]) != tolower((unsigned char)suf[i])) return 0;
    return 1;
}

static void fill_common(CatEntry *e, const char *title, int year, const char *desc,
                        const char *date, int runtime, int is_zip) {
    snprintf(e->title, sizeof(e->title), "%s", title);
    e->year = year; e->runtime = runtime; e->is_zip = is_zip;
    snprintf(e->desc, sizeof(e->desc), "%s", desc);
    snprintf(e->date, sizeof(e->date), "%s", date);
    char yr[12]; snprintf(yr, sizeof(yr), "(%d)", year);   /* avoid double year */
    if (year > 0 && !strstr(title, yr))
        snprintf(e->name, sizeof(e->name), "%s (%d)%s", title, year, is_zip ? " [ZIP]" : "");
    else
        snprintf(e->name, sizeof(e->name), "%s%s", title, is_zip ? " [ZIP]" : "");
}

/* does a filename/title carry a "3D" marker? (used for mixed catalogs) */
static int fn_has_3d(const char *s) {
    for (; s && s[0] && s[1]; s++)
        if (s[0] == '3' && (s[1] == 'D' || s[1] == 'd')) return 1;
    return 0;
}

/* Build one CatEntry from a zackk "file object" (its filename/fileType/archiveUrl/fileSize) plus a
 * "meta" object (title/year/category/genres/artwork/description). `dispname` overrides the display
 * name (used for a season part). Returns 1 if it produced a playable entry. */
static int zackk_add(CatEntry *e, cJSON *fobj, cJSON *meta, const char *dispname, const char *art_base) {
    char enc[CAT_URLLEN];
    const char *fn = sgets(fobj, "filename");
    const char *ft = sgets(fobj, "fileType");
    const char *url = sgets(fobj, "archiveUrl");
    int iszip = ends_ci(fn, ".zip");
    int ismof = ends_ci(fn, ".moflex") || !strcasecmp(ft, "moflex");
    int iscia = ends_ci(fn, ".cia")    || !strcasecmp(ft, "cia");
    int drive = is_drive_url(url);
    if ((!iszip && !ismof && !iscia && !drive) || !url[0]) return 0;
    fill_common(e, sgets(meta, "title"), sgeti(meta, "year"), sgets(meta, "description"),
                sgets(meta, "dateAdded"), sgeti(meta, "runtime"), iszip);
    if (dispname && dispname[0]) snprintf(e->name, sizeof e->name, "%s", dispname);   /* season part title */
    e->size = sgeti64(fobj, "fileSize");
    snprintf(e->category, sizeof e->category, "%s", sgets(meta, "category"));
    join_genres(meta, "genres", e->genres, sizeof e->genres);
    if (drive && !(iszip || ismof || iscia)) {
        char id[96];
        if (!drive_id(url, fn, id, sizeof id)) return 0;
        snprintf(e->url, sizeof e->url, "https://drive.usercontent.google.com/download?id=%s&export=download&confirm=t", id);
        savename(dispname && dispname[0] ? dispname : sgets(meta, "title"), sgeti(meta, "year"), e->fname, sizeof e->fname);
    } else {
        snprintf(e->fname, sizeof e->fname, "%s", fn);
        url_fixup(url, e->url, sizeof e->url);
    }
    const char *aw = sgets(meta, "artwork");
    if (!aw[0]) e->art[0] = 0;
    else if (!strncmp(aw, "http", 4)) url_fixup(aw, e->art, sizeof e->art);
    else { url_encode(aw, enc, sizeof enc); snprintf(e->art, sizeof e->art, "%s%s", art_base, enc); }
    e->is3d = (fn_has_3d(fn) || fn_has_3d(e->name)) ? 1 : 0;
    return 1;
}

int catalog_parse(const char *text, int kind, const char *dl_base, const char *art_base,
                  CatEntry *out, int max) {
    cJSON *root = cJSON_Parse(text);
    if (!root) return 0;
    int n = 0;
    char enc[CAT_URLLEN];

    /* auto-detect the catalog shape from its contents, so a user-added source works whatever
     * kind was declared: "items" -> zackk shape (moflex/zip/CIA), "movies"/"tvShows" -> clownsec. */
    if (cJSON_GetObjectItemCaseSensitive(root, "items")) kind = 1;
    else if (cJSON_GetObjectItemCaseSensitive(root, "movies") ||
             cJSON_GetObjectItemCaseSensitive(root, "tvShows")) kind = 0;

    if (kind == 0) {
        /* clownsec: movies[] + tvShows[] */
        cJSON *movies = cJSON_GetObjectItemCaseSensitive(root, "movies");
        cJSON *mv;
        cJSON_ArrayForEach(mv, movies) {
            if (n >= max) break;
            const char *fn = sgets(mv, "filename");
            if (!fn[0]) continue;
            CatEntry *e = &out[n];
            fill_common(e, sgets(mv, "title"), sgeti(mv, "year"), sgets(mv, "description"),
                        sgets(mv, "dateAdded"), sgeti(mv, "runtime"), ends_ci(fn, ".zip"));
            e->size = sgeti64(mv, "fileSize");
            join_genres(mv, "genres", e->genres, sizeof(e->genres));
            { const char *ty = sgets(mv, "type");   /* clownsec movies[] mixes movies / music videos / etc. */
              if (!ty[0] || !strcasecmp(ty, "movie")) snprintf(e->category, sizeof e->category, "Movies");
              else if (!strcasecmp(ty, "musicvideo")) snprintf(e->category, sizeof e->category, "Music Videos");
              else {   /* any other type -> a readable label (capitalized, pluralized) so it isn't hidden */
                  char c[32]; snprintf(c, sizeof c, "%s", ty);
                  c[0] = (char)toupper((unsigned char)c[0]);
                  size_t L = strlen(c); if (L && c[L - 1] != 's' && L + 1 < sizeof c) { c[L] = 's'; c[L + 1] = 0; }
                  snprintf(e->category, sizeof e->category, "%s", c);
              } }
            snprintf(e->fname, sizeof(e->fname), "%s", fn);
            url_encode(fn, enc, sizeof(enc));
            snprintf(e->url, sizeof(e->url), "%s%s", dl_base, enc);
            const char *aw = sgets(mv, "artwork");
            if (aw[0]) { url_encode(aw, enc, sizeof(enc)); snprintf(e->art, sizeof(e->art), "%s%s", art_base, enc); }
            else e->art[0] = 0;
            e->is3d = (fn_has_3d(e->fname) || fn_has_3d(e->name)) ? 1 : 0;   /* "(3D)" marks 3D files */
            n++;
        }
        /* tvShows[] -> one entry per season zip */
        cJSON *shows = cJSON_GetObjectItemCaseSensitive(root, "tvShows");
        cJSON *sh;
        cJSON_ArrayForEach(sh, shows) {
            const char *title = sgets(sh, "title");
            const char *aw = sgets(sh, "artwork");
            /* TV files live at <dl_base>tv/<folder>/<filename> (folder falls back to title) */
            const char *folder = sgets(sh, "folder"); if (!folder[0]) folder = title;
            char encfolder[CAT_URLLEN]; url_encode(folder, encfolder, sizeof(encfolder));
            char arturl[CAT_URLLEN]; arturl[0] = 0;
            if (aw[0]) { url_encode(aw, enc, sizeof(enc)); snprintf(arturl, sizeof(arturl), "%s%s", art_base, enc); }
            cJSON *zips = cJSON_GetObjectItemCaseSensitive(sh, "seasonZips");
            cJSON *z;
            cJSON_ArrayForEach(z, zips) {
                if (n >= max) break;
                const char *zfn = cJSON_IsString(z) ? z->valuestring
                                : cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(z, "filename"));
                if (!zfn || !zfn[0]) continue;
                CatEntry *e = &out[n];
                fill_common(e, title, sgeti(sh, "year"), sgets(sh, "description"),
                            sgets(sh, "dateAdded"), 0, 1);
                e->size = cJSON_IsObject(z) ? sgeti64(z, "fileSize") : 0;
                /* prefer the zip's own name for disambiguation (Season N) */
                char stem[CAT_NAMELEN]; snprintf(stem, sizeof(stem), "%s", zfn);
                size_t sl = strlen(stem); if (sl > 4 && ends_ci(stem, ".zip")) stem[sl - 4] = 0;
                snprintf(e->name, sizeof(e->name), "%s [ZIP]", stem);
                join_genres(sh, "genres", e->genres, sizeof(e->genres));
                snprintf(e->category, sizeof(e->category), "TV Shows");
                snprintf(e->fname, sizeof(e->fname), "%s", zfn);
                url_encode(zfn, enc, sizeof(enc));
                snprintf(e->url, sizeof(e->url), "%stv/%s/%s", dl_base, encfolder, enc);
                snprintf(e->art, sizeof(e->art), "%s", arturl);
                e->is3d = (fn_has_3d(e->fname) || fn_has_3d(e->name)) ? 1 : 0;
                n++;
            }
        }
    } else {
        /* zackk: items[] (keep moflex/zip AND cia -- CIAs carry an embedded moflex we can play) */
        cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
        cJSON *it;
        cJSON_ArrayForEach(it, items) {
            if (n >= max) break;
            if (!cJSON_IsObject(it)) continue;
            cJSON *parts = cJSON_GetObjectItemCaseSensitive(it, "parts");
            if (cJSON_IsArray(parts) && cJSON_GetArraySize(parts) > 0) {
                cJSON *pt;                              /* TV shows / wrestling / anime: one entry per part */
                cJSON_ArrayForEach(pt, parts) {
                    if (n >= max) break;
                    if (cJSON_IsObject(pt) && zackk_add(&out[n], pt, it, sgets(pt, "title"), art_base)) n++;
                }
            } else if (zackk_add(&out[n], it, it, NULL, art_base)) {   /* a single downloadable file */
                n++;
            }
        }
    }
    (void)enc;

    cJSON_Delete(root);
    return n;
}
