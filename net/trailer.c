#include "trailer.h"
#include "catalog.h"
#include "movieinfo.h"
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* find a [4cc][u32 len] section in the CSXTRA payload; 1 = found (f positioned at body) */
static int find_section(FILE *f, const char *cc, long long *body, unsigned *len) {
    if (fseeko(f, -16, SEEK_END)) return 0;
    long long fsz = (long long)ftello(f) + 16;
    u8 ft[16];
    if (fread(ft, 1, 16, f) != 16 || memcmp(ft + 8, "CSXTRA01", 8)) return 0;
    long long off = 0;
    for (int i = 7; i >= 0; i--) off = (off << 8) | ft[i];
    if (off <= 0 || off >= fsz - 16) return 0;
    long long p = off, end = fsz - 16;
    while (p + 8 <= end) {
        u8 h[8];
        if (fseeko(f, p, SEEK_SET) || fread(h, 1, 8, f) != 8) return 0;
        unsigned l = h[4] | (h[5] << 8) | ((unsigned)h[6] << 16) | ((unsigned)h[7] << 24);
        if (p + 8 + (long long)l > end) return 0;
        if (!memcmp(h, cc, 4)) { *body = p + 8; *len = l; return 1; }
        p += 8 + l;
    }
    return 0;
}

int trailer_import_movieinfo(const char *moviepath) {
    return trailer_import_movieinfo_key(moviepath, moviepath, 0);
}

int trailer_import_movieinfo_key(const char *moviepath, const char *savekey, int show_level) {
    FILE *f = fopen(moviepath, "rb");
    if (!f) return 0;
    long long noff = 0; unsigned nlen = 0;
    if (!find_section(f, "NFO0", &noff, &nlen) || nlen == 0 || nlen > 65536) { fclose(f); return 0; }
    char *nfo = (char *)malloc(nlen + 1);
    if (!nfo || fseeko(f, noff, SEEK_SET) || fread(nfo, 1, nlen, f) != nlen) { free(nfo); fclose(f); return 0; }
    nfo[nlen] = 0;

    CatEntry e; memset(&e, 0, sizeof e);
    e.is3d = -1;
    char showdesc[sizeof e.desc]; showdesc[0] = 0;
    for (char *line = nfo; line && *line; ) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) { *nl = 0; nl++; while (*nl == '\r' || *nl == '\n') nl++; }
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = 0; const char *v = eq + 1;
            if      (!strcmp(line, "title"))    snprintf(e.title, sizeof e.title, "%s", v);
            else if (!strcmp(line, "year"))     e.year = atoi(v);
            else if (!strcmp(line, "desc"))     snprintf(e.desc, sizeof e.desc, "%s", v);
            else if (!strcmp(line, "genres"))   snprintf(e.genres, sizeof e.genres, "%s", v);
            else if (!strcmp(line, "category")) snprintf(e.category, sizeof e.category, "%s", v);
            else if (!strcmp(line, "runtime"))  e.runtime = atoi(v);
            else if (!strcmp(line, "date"))     snprintf(e.date, sizeof e.date, "%s", v);
            else if (!strcmp(line, "is3d"))     e.is3d = atoi(v);
            else if (!strcmp(line, "showdesc")) snprintf(showdesc, sizeof showdesc, "%s", v);
        }
        line = nl;
    }
    free(nfo);
    if (show_level && showdesc[0]) snprintf(e.desc, sizeof e.desc, "%s", showdesc);
    if (!e.title[0]) { fclose(f); return 0; }
    if (e.year > 0) snprintf(e.name, sizeof e.name, "%s (%d)", e.title, e.year);
    else            snprintf(e.name, sizeof e.name, "%s", e.title);
    if (!e.category[0]) snprintf(e.category, sizeof e.category, "Movies");
    { const char *b = strrchr(savekey, '/'); snprintf(e.fname, sizeof e.fname, "%s", b ? b + 1 : savekey); }

    /* poster: pre-scaled RGB565 (ART5) -- exact library format, straight copy */
    u16 *poster = NULL; int pw = 0, ph = 0;
    long long aoff = 0; unsigned alen = 0;
    if (find_section(f, "ART5", &aoff, &alen) && alen > 4) {
        u8 hd[4];
        if (!fseeko(f, aoff, SEEK_SET) && fread(hd, 1, 4, f) == 4) {
            pw = hd[0] | (hd[1] << 8); ph = hd[2] | (hd[3] << 8);
            if (pw > 0 && ph > 0 && alen == 4u + (unsigned)pw * ph * 2 && pw <= 256 && ph <= 256) {
                poster = (u16 *)malloc((size_t)pw * ph * 2);
                if (poster && fread(poster, 1, (size_t)pw * ph * 2, f) != (size_t)pw * ph * 2) {
                    free(poster); poster = NULL;
                }
            }
        }
    }
    fclose(f);
    movieinfo_save(savekey, &e, poster, pw, ph);
    free(poster);
    return 1;
}
