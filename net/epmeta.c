#include "epmeta.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdint.h>

/* ---- the file's own info: the NFO0 section of the CSXTRA trailer ---- */
static int nfo_section(FILE *f, long long *body, unsigned *len) {
    if (fseeko(f, -16, SEEK_END)) return 0;
    long long fsz = (long long)ftello(f) + 16;
    unsigned char ft[16];
    if (fread(ft, 1, 16, f) != 16 || memcmp(ft + 8, "CSXTRA01", 8)) return 0;
    long long off = 0;
    for (int i = 7; i >= 0; i--) off = (off << 8) | ft[i];
    if (off <= 0 || off >= fsz - 16) return 0;
    long long p = off, end = fsz - 16;
    for (int guard = 0; p + 8 <= end && guard < 64; guard++) {   /* NFO0 comes first; never walk far */
        unsigned char h[8];
        if (fseeko(f, p, SEEK_SET) || fread(h, 1, 8, f) != 8) return 0;
        unsigned l = h[4] | (h[5] << 8) | ((unsigned)h[6] << 16) | ((unsigned)h[7] << 24);
        if (l == 0 || p + 8 + (long long)l > end) return 0;
        if (!memcmp(h, "NFO0", 4)) { *body = p + 8; *len = l; return 1; }
        p += 8 + l;
    }
    return 0;
}

typedef struct {
    char title[96], kind[16], eptitle[96];
    int year, season, episode, have_se;
} Nfo;

static int nfo_read(const char *path, Nfo *n) {
    memset(n, 0, sizeof *n);
    size_t L = strlen(path);
    if (L < 7 || strcasecmp(path + L - 7, ".moflex")) return 0;   /* only a moflex has the trailer */
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    long long off; unsigned len;
    int ok = nfo_section(f, &off, &len) && len > 0 && len <= 65536;
    char *buf = ok ? (char *)malloc(len + 1) : NULL;
    if (ok && (!buf || fseeko(f, off, SEEK_SET) || fread(buf, 1, len, f) != len)) ok = 0;
    fclose(f);
    if (!ok) { free(buf); return 0; }
    buf[len] = 0;
    int have_s = 0, have_e = 0;
    for (char *line = buf; line && *line; ) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) { *nl = 0; nl++; while (*nl == '\r' || *nl == '\n') nl++; }
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = 0; const char *v = eq + 1;
            if      (!strcmp(line, "title"))   snprintf(n->title, sizeof n->title, "%s", v);
            else if (!strcmp(line, "year"))    n->year = atoi(v);
            else if (!strcmp(line, "kind"))    snprintf(n->kind, sizeof n->kind, "%s", v);
            else if (!strcmp(line, "eptitle")) snprintf(n->eptitle, sizeof n->eptitle, "%s", v);
            else if (!strcmp(line, "season")  && isdigit((unsigned char)*v)) { n->season = atoi(v); have_s = 1; }
            else if (!strcmp(line, "episode") && isdigit((unsigned char)*v)) { n->episode = atoi(v); have_e = 1; }
        }
        line = nl;
    }
    n->have_se = have_s && have_e;
    free(buf);
    return 1;
}

/* ---- the file name ---- */
static const char *tag_at(const char *s) {          /* the SxxEyy tag, or NULL */
    for (const char *p = s; *p; p++)
        if ((*p == 's' || *p == 'S') && isdigit((unsigned char)p[1])) {
            const char *q = p + 1; while (isdigit((unsigned char)*q)) q++;
            if ((*q == 'e' || *q == 'E') && isdigit((unsigned char)q[1])) return p;
        }
    return NULL;
}

static void strip_tags(char *s) {                    /* drop "(3D)" "(S)" and the extension */
    char *dot = strrchr(s, '.');
    if (dot && (!strcasecmp(dot, ".moflex") || !strcasecmp(dot, ".mp4") || !strcasecmp(dot, ".cia"))) *dot = 0;
    char *d = s;
    for (char *p = s; *p; ) {
        int skip = 0;
        if (p[0] == '(' && p[1] == '3' && (p[2] == 'D' || p[2] == 'd') && p[3] == ')') skip = 4;
        else if (p[0] == '(' && (p[1] == 'S' || p[1] == 's') && p[2] == ')') skip = 3;
        if (skip) { p += skip; if (d > s && d[-1] == ' ' && (*p == ' ' || !*p)) d--; continue; }
        *d++ = *p++;
    }
    *d = 0;
    while (d > s && (d[-1] == ' ' || d[-1] == '-')) *--d = 0;
}

int epmeta_from_name(const char *fname, EpMeta *m) {
    memset(m, 0, sizeof *m);
    const char *b = strrchr(fname, '/'); b = b ? b + 1 : fname;
    const char *t = tag_at(b);
    if (!t) return 0;
    m->is_ep = 1;
    m->season = atoi(t + 1);
    const char *q = t + 1; while (isdigit((unsigned char)*q)) q++;
    m->episode = atoi(q + 1);
    q++; while (isdigit((unsigned char)*q)) q++;
    while (*q == ' ' || *q == '-') q++;
    snprintf(m->eptitle, sizeof m->eptitle, "%s", q);
    strip_tags(m->eptitle);
    size_t L = (size_t)(t - b);
    while (L > 0 && (b[L - 1] == ' ' || b[L - 1] == '-')) L--;
    if (L >= sizeof m->show) L = sizeof m->show - 1;
    memcpy(m->show, b, L); m->show[L] = 0;
    strip_tags(m->show);
    /* the year in the name: the last "(19xx)/(20xx)" */
    for (const char *p = m->show; (p = strchr(p, '(')); p++)
        if ((p[1] == '1' || p[1] == '2') && isdigit((unsigned char)p[2]) && isdigit((unsigned char)p[3]) &&
            isdigit((unsigned char)p[4]) && p[5] == ')') m->year = atoi(p + 1);
    epmeta_show_key(m->show, m->key, sizeof m->key);
    return 1;
}

/* ---- keys ---- */
static int is_tag_group(const char *p, size_t n) {   /* inside "( )": a year, 3D, 2D, S, SBS... */
    char g[16];
    if (n == 0 || n >= sizeof g) return 0;
    memcpy(g, p, n); g[n] = 0;
    if (n == 4 && isdigit((unsigned char)g[0]) && isdigit((unsigned char)g[1]) &&
        isdigit((unsigned char)g[2]) && isdigit((unsigned char)g[3])) return 1;
    static const char *const T[] = { "3d", "2d", "s", "super", "sbs", "hsbs", "fsbs" };
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) if (!strcasecmp(g, T[i])) return 1;
    return 0;
}

void epmeta_show_key(const char *name, char *out, size_t cap) {
    size_t o = 0;
    for (const char *p = name; *p && o + 1 < cap; p++) {
        if (*p == '(') {
            const char *c = strchr(p, ')');
            if (c && is_tag_group(p + 1, (size_t)(c - p - 1))) { p = c; continue; }
        }
        unsigned char ch = (unsigned char)*p;
        if (isalnum(ch)) out[o++] = (char)tolower(ch);
        else if (ch >= 0x80) out[o++] = (char)ch;    /* keep non-ASCII letters as they are */
    }
    out[o] = 0;
}

int epmeta_same_show(const char *a, const char *b) {
    char ka[96], kb[96];
    epmeta_show_key(a, ka, sizeof ka); epmeta_show_key(b, kb, sizeof kb);
    return ka[0] && !strcmp(ka, kb);
}

void epmeta_label(const EpMeta *m, char *out, size_t cap) {
    if (m->eptitle[0]) snprintf(out, cap, "S%02de%02d - %s", m->season, m->episode, m->eptitle);
    else               snprintf(out, cap, "S%02de%02d", m->season, m->episode);
}

int epmeta_order(const EpMeta *a, const EpMeta *b) {
    int sa = a->season ? a->season : 1 << 20, sb = b->season ? b->season : 1 << 20;   /* specials last */
    if (sa != sb) return sa < sb ? -1 : 1;
    if (a->episode != b->episode) return a->episode < b->episode ? -1 : 1;
    return 0;
}

/* ---- cache: the library and the episode list ask about the same files again and again ---- */
#define EPC_N 128
static struct { uint64_t h; EpMeta m; } s_epc[EPC_N];
static int s_epc_n, s_epc_next;

static uint64_t fnv(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

void epmeta_cache_clear(void) { s_epc_n = 0; s_epc_next = 0; }

int epmeta_get(const char *path, EpMeta *m) {
    uint64_t h = fnv(path);
    for (int i = 0; i < s_epc_n; i++) if (s_epc[i].h == h) { *m = s_epc[i].m; return m->is_ep; }

    epmeta_from_name(path, m);                       /* the fallback for whatever the file leaves out */
    Nfo n;
    if (nfo_read(path, &n)) {
        int kind_ep = !strcasecmp(n.kind, "episode");
        if (n.kind[0] && !kind_ep) {                 /* the file says it is a film / short / music video */
            memset(m, 0, sizeof *m);
            m->from_file = 1;
        } else {
            if (kind_ep) {
                m->from_file = 1;
                if (n.have_se) { m->is_ep = 1; m->season = n.season; m->episode = n.episode; }
                else if (!m->is_ep) { m->is_ep = 1; m->season = 0; m->episode = 0; }
            }
            if (m->is_ep) {
                if (n.title[0]) {                    /* the show's name: the file's own */
                    int y = n.year ? n.year : m->year;
                    if (y > 0) snprintf(m->show, sizeof m->show, "%.88s (%d)", n.title, y);
                    else       snprintf(m->show, sizeof m->show, "%s", n.title);
                    m->year = y;
                    epmeta_show_key(m->show, m->key, sizeof m->key);
                }
                if (n.eptitle[0]) snprintf(m->eptitle, sizeof m->eptitle, "%s", n.eptitle);
            }
        }
    }
    int slot = s_epc_n < EPC_N ? s_epc_n++ : (s_epc_next++ % EPC_N);
    s_epc[slot].h = h; s_epc[slot].m = *m;
    return m->is_ep;
}
