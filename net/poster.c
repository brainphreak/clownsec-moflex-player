#include "poster.h"
#include "downloader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG          /* also accept PNG posters for hand-added sidecars */
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"

#define ART_DIR "sdmc:/moflex_player/art"

/* map a cache key (art filename/url) to a filesystem-safe name */
static void sanitize(const char *k, char *o, size_t cap) {
    size_t j = 0;
    for (; *k && j + 1 < cap; k++) {
        char c = *k;
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '.';
        o[j++] = ok ? c : '_';
    }
    o[j] = 0;
}

static inline u16 to565(int r, int g, int b) {
    return (u16)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* nearest-neighbour scale a decoded RGB (3-byte) image into the pw x ph RGB565 box */
static void scale_rgb(const unsigned char *rgb, int w, int h, u16 *out, int pw, int ph) {
    for (int j = 0; j < ph; j++) {
        int sy = j * h / ph;
        const unsigned char *row = rgb + (size_t)sy * w * 3;
        u16 *dst = out + (size_t)j * pw;
        for (int i = 0; i < pw; i++) {
            const unsigned char *p = row + (size_t)(i * w / pw) * 3;
            dst[i] = to565(p[0], p[1], p[2]);
        }
    }
}

int poster_decode_file(const char *path, u16 *out, int pw, int ph) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 16 * 1024 * 1024) { fclose(f); return 0; }
    unsigned char *buf = (unsigned char *)malloc((size_t)n);
    if (!buf) { fclose(f); return 0; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (rd != (size_t)n) { free(buf); return 0; }
    int w = 0, h = 0, comp = 0;
    unsigned char *rgb = stbi_load_from_memory(buf, (int)n, &w, &h, &comp, 3);
    free(buf);
    if (!rgb || w <= 0 || h <= 0) { if (rgb) stbi_image_free(rgb); return 0; }
    scale_rgb(rgb, w, h, out, pw, ph);
    stbi_image_free(rgb);
    return 1;
}

int poster_get(const char *art_url, const char *cache_key, u16 *out, int pw, int ph) {
    if (!art_url || !art_url[0]) return 0;
    size_t need = (size_t)pw * ph * sizeof(u16);

    char safe[128];
    sanitize((cache_key && cache_key[0]) ? cache_key : art_url, safe, sizeof(safe));
    char path[256];
    snprintf(path, sizeof(path), "%s/%s_%dx%d.p565", ART_DIR, safe, pw, ph);

    /* cache hit -> straight read */
    FILE *cf = fopen(path, "rb");
    if (cf) {
        size_t rd = fread(out, 1, need, cf);
        fclose(cf);
        if (rd == need) return 1;
    }

    /* download the JPEG into memory and decode it */
    char *jpg = NULL; size_t jlen = 0;
    if (!download_to_mem(art_url, &jpg, &jlen, 4 * 1024 * 1024)) return 0;
    int w = 0, h = 0, comp = 0;
    unsigned char *rgb = stbi_load_from_memory((const unsigned char *)jpg, (int)jlen, &w, &h, &comp, 3);
    free(jpg);
    if (!rgb || w <= 0 || h <= 0) { if (rgb) stbi_image_free(rgb); return 0; }

    scale_rgb(rgb, w, h, out, pw, ph);
    stbi_image_free(rgb);

    /* best-effort cache write */
    mkdir("sdmc:/moflex_player", 0777);
    mkdir(ART_DIR, 0777);
    FILE *wf = fopen(path, "wb");
    if (wf) { fwrite(out, 1, need, wf); fclose(wf); }
    return 1;
}
