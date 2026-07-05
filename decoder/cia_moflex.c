/* Locate and open a .moflex embedded inside an unencrypted 3DS CIA (RomFS), in place.
 * These are the "movie CIAs" some tools distribute (e.g. Zackk's library): the CIA content
 * is built NoCrypto, so each moflex sits in the clear as one contiguous region -- no keys,
 * no decryption, no extraction needed. We parse CIA -> NCCH -> RomFS to find the byte
 * offset+size of every embedded moflex, then point the demux at the chosen window. */
#include "cia_moflex.h"
#include <string.h>
#include <stdlib.h>

static uint32_t r32(FILE *f, int64_t o) {
    uint8_t b[4];
    if (fseeko(f, (off_t)o, SEEK_SET)) return 0;
    if (fread(b, 1, 4, f) != 4) return 0;
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static uint64_t r64(FILE *f, int64_t o) { return (uint64_t)r32(f, o) | ((uint64_t)r32(f, o + 4) << 32); }
static int64_t  al(int64_t x, int64_t a) { return (x + a - 1) & ~(a - 1); }

int cia_is_cia(const char *path) {
    size_t L = path ? strlen(path) : 0;
    return L > 4 && (path[L-4]=='.' && (path[L-3]=='c'||path[L-3]=='C')
                     && (path[L-2]=='i'||path[L-2]=='I') && (path[L-1]=='a'||path[L-1]=='A'));
}

/* Parse a UTF-16LE movie_title.csv: header row starts with '#'; each data row is a movie, one column
 * per language. Take EN (field 1), fall back to JP (field 0). Returns how many titles were read. */
static int parse_titles(FILE *f, int64_t off, uint32_t sz, char titles[][128], int max) {
    if (sz == 0) return 0;
    if (sz > 64u * 1024u) sz = 64u * 1024u;
    uint8_t *b = (uint8_t *)malloc(sz);
    if (!b) return 0;
    size_t got = 0;
    if (!fseeko(f, (off_t)off, SEEK_SET)) got = fread(b, 1, sz, f);
    int nt = 0; uint32_t i = 0;
    if (got >= 2 && b[0] == 0xFF && b[1] == 0xFE) i = 2;          /* skip UTF-16LE BOM */
    while (i + 1 < got && nt < max) {
        char fld[2][128]; int fi = 0, fj = 0; fld[0][0] = fld[1][0] = 0;
        int is_header = 0, first = 1, empty = 1;
        while (i + 1 < got) {
            char c = (b[i + 1] == 0) ? (char)b[i] : '?';
            i += 2;
            if (c == '\r') continue;
            if (c == '\n') break;
            empty = 0;
            if (first) { if (c == '#') is_header = 1; first = 0; }
            if (c == ',') { if (fi < 2) fi++; fj = 0; continue; }
            if (fi < 2 && fj < 127) { fld[fi][fj++] = c; fld[fi][fj] = 0; }
        }
        if (is_header || empty) continue;
        const char *t = fld[1][0] ? fld[1] : fld[0];
        while (*t == ' ') t++;                                     /* trim a leading space */
        if (t[0]) { snprintf(titles[nt], 128, "%s", t); nt++; }
    }
    free(b);
    return nt;
}

int cia_list_moflex(const char *path, CiaMoflex *out, int max) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int cnt = 0; int64_t toff = 0; uint32_t tsz = 0;   /* movie_title.csv location, if present */
    uint32_t hs = r32(f, 0), certs = r32(f, 8), tiks = r32(f, 12), tmds = r32(f, 16);
    if (hs != 0x2020) { fclose(f); return 0; }                       /* not a CIA */
    int64_t o = al(hs, 0x40); o = al(o + certs, 0x40); o = al(o + tiks, 0x40); o = al(o + tmds, 0x40);
    uint8_t m[4];
    if (fseeko(f, (off_t)(o + 0x100), SEEK_SET) || fread(m, 1, 4, f) != 4 || memcmp(m, "NCCH", 4)) { fclose(f); return 0; }
    uint8_t fl;
    if (fseeko(f, (off_t)(o + 0x18F), SEEK_SET) || fread(&fl, 1, 1, f) != 1) { fclose(f); return 0; }
    if (!(fl & 0x04)) { fclose(f); return 0; }                       /* encrypted content -> unsupported */
    int64_t romfs = o + (int64_t)r32(f, o + 0x1B0) * 0x200;
    uint8_t iv[4];
    if (fseeko(f, (off_t)romfs, SEEK_SET) || fread(iv, 1, 4, f) != 4 || memcmp(iv, "IVFC", 4)) { fclose(f); return 0; }
    uint32_t mhs = r32(f, romfs + 0x08), blog = r32(f, romfs + 0x4C);
    int64_t bs = 1LL << blog; if (bs < 0x40 || bs > 0x100000) bs = 0x1000;
    int64_t l3 = romfs + al(0x60 + mhs, bs);                          /* RomFS Level 3 (the filesystem) */
    if (r32(f, l3) != 0x28) l3 = romfs + 0x1000;                     /* fallback: standard makerom layout */
    if (r32(f, l3) != 0x28) { fclose(f); return 0; }
    uint32_t fmo = r32(f, l3 + 0x1C), fml = r32(f, l3 + 0x20), fdo = r32(f, l3 + 0x24);
    int64_t fdata = l3 + fdo;
    /* bulk-read the whole file-metadata region once and parse in RAM (fast: a couple of reads total,
     * so this is cheap enough to run while listing a folder to hide non-movie CIAs). */
    uint32_t cap = fml; if (cap > 512u * 1024u) cap = 512u * 1024u;
    uint8_t *buf = (uint8_t *)malloc(cap ? cap : 1);
    if (!buf) { fclose(f); return 0; }
    size_t got = 0;
    if (!fseeko(f, (off_t)(l3 + fmo), SEEK_SET)) got = fread(buf, 1, cap, f);
    for (int64_t p = 0; p + 0x20 <= (int64_t)got && cnt < max; ) {
        uint8_t *e = buf + p;
        uint64_t dofs = (uint64_t)e[0x08] | (uint64_t)e[0x09]<<8 | (uint64_t)e[0x0A]<<16 | (uint64_t)e[0x0B]<<24
                      | (uint64_t)e[0x0C]<<32 | (uint64_t)e[0x0D]<<40 | (uint64_t)e[0x0E]<<48 | (uint64_t)e[0x0F]<<56;
        uint64_t dsz  = (uint64_t)e[0x10] | (uint64_t)e[0x11]<<8 | (uint64_t)e[0x12]<<16 | (uint64_t)e[0x13]<<24
                      | (uint64_t)e[0x14]<<32 | (uint64_t)e[0x15]<<40 | (uint64_t)e[0x16]<<48 | (uint64_t)e[0x17]<<56;
        uint32_t nlen = (uint32_t)e[0x1C] | (uint32_t)e[0x1D]<<8 | (uint32_t)e[0x1E]<<16 | (uint32_t)e[0x1F]<<24;
        if (nlen == 0 || nlen > 512 || p + 0x20 + (int64_t)nlen > (int64_t)got) break;
        char nm[264]; int j = 0;
        for (uint32_t i = 0; i + 1 < nlen && j < 263; i += 2) nm[j++] = (char)e[0x20 + i];   /* UTF-16LE -> ASCII */
        nm[j] = 0;
        int NL = (int)strlen(nm);
        if (NL > 7 && !strcasecmp(nm + NL - 7, ".moflex")) {
            out[cnt].off = fdata + (int64_t)dofs; out[cnt].size = (int64_t)dsz;
            snprintf(out[cnt].name, sizeof out[cnt].name, "%s", nm);
            cnt++;
        } else if (!strcasecmp(nm, "movie_title.csv")) {
            toff = fdata + (int64_t)dofs; tsz = (uint32_t)dsz;      /* real display titles live here */
        }
        p += al(0x20 + nlen, 4);
    }
    free(buf);
    /* replace filenames with the human-readable titles (row order = movie order in the native menu) */
    if (toff && cnt > 0) {
        char titles[64][128];
        int nt = parse_titles(f, toff, tsz, titles, 64);
        for (int i = 0; i < cnt && i < nt; i++) snprintf(out[i].name, sizeof out[i].name, "%s", titles[i]);
    }
    fclose(f);
    return cnt;
}

int cia_find_moflex(const char *path, int64_t *out_off, int64_t *out_size) {
    CiaMoflex list[16];
    int n = cia_list_moflex(path, list, 16);
    if (n <= 0) return 0;
    *out_off = list[0].off; *out_size = list[0].size;
    return 1;
}

int cia_has_moflex(const char *path) {
    CiaMoflex one[1];
    return cia_list_moflex(path, one, 1) > 0;   /* stops at the first match */
}

/* which moflex the next auto-open should use (set by the picker in the app) */
static char    g_sel_path[512] = "";
static int64_t g_sel_off = 0, g_sel_size = 0;
static char    g_sel_title[128] = "";

void cia_set_selection(const char *path, int64_t off, int64_t size, const char *title) {
    snprintf(g_sel_path, sizeof g_sel_path, "%s", path ? path : "");
    g_sel_off = off; g_sel_size = size;
    snprintf(g_sel_title, sizeof g_sel_title, "%s", title ? title : "");
}
void cia_clear_selection(void) { g_sel_path[0] = 0; g_sel_title[0] = 0; }

const char *cia_selection_title(const char *path) {
    if (g_sel_path[0] && path && !strcmp(g_sel_path, path) && g_sel_title[0]) return g_sel_title;
    return NULL;
}

void cia_selection_suffix(const char *path, char *out, unsigned cap) {
    if (g_sel_path[0] && path && !strcmp(g_sel_path, path))
        snprintf(out, cap, "_%llx", (unsigned long long)g_sel_off);
    else if (cap) out[0] = 0;
}

int mfx_open_auto(MfxDemux *m, FILE *f, const char *path) {
    if (cia_is_cia(path)) {
        if (g_sel_path[0] && path && !strcmp(g_sel_path, path))       /* a specific moflex was chosen */
            return mfx_open_window(m, f, g_sel_off, g_sel_size);
        int64_t off, sz;
        if (cia_find_moflex(path, &off, &sz))                         /* else the first one */
            return mfx_open_window(m, f, off, sz);
    }
    return mfx_open(m, f);
}
