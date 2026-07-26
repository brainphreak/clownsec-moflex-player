#include "unzip.h"
#include "zip.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* extraction forensics: one small log per unzip run -- the last lines name the failing layer */
static void uz_log(const char *fmt, ...) {
    FILE *f = fopen("sdmc:/moflex_player/unzip_log.txt", "ab");
    if (!f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* create every parent directory of `path` (path ends in a filename) */
static void mkparents(char *path) {
    for (char *p = strchr(path, '/'); p; p = strchr(p + 1, '/')) {
        *p = 0;
        mkdir(path, 0777);
        *p = '/';
    }
}

/* Skip archive junk that isn't real content: macOS resource forks / metadata
 * (__MACOSX/, AppleDouble "._name", .DS_Store). These often have odd/long names
 * and were causing the all-or-nothing zip_extract() to report the whole archive
 * as failed even after every real file had already been written. */
static int is_junk(const char *name) {
    if (!name) return 1;
    if (strstr(name, "__MACOSX")) return 1;
    if (strstr(name, "/._")) return 1;
    if (name[0] == '.' && name[1] == '_') return 1;
    const char *base = strrchr(name, '/'); base = base ? base + 1 : name;
    if (!strcmp(base, ".DS_Store")) return 1;
    return 0;
}

/* chunk sink for zip_entry_extract: write + re-fire the progress callback so long files
 * stay responsive; returning a short count makes the extractor stop with an error. */
struct uz_stream { FILE *f; unzip_prog_cb cb; int done, total; const char *name; int cancel; };
static size_t uz_write(void *arg, uint64_t offset, const void *data, size_t size) {
    struct uz_stream *s = (struct uz_stream *)arg;
    (void)offset;
    if (s->cb && !s->cb(s->done, s->total, s->name)) { s->cancel = 1; return 0; }
    return fwrite(data, 1, size, s->f);
}

int unzip_to_dir_cb(const char *zip_path, const char *dest_dir, int *total_out, unzip_prog_cb cb) {
    if (total_out) *total_out = 0;
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", dest_dir);
    size_t L = strlen(dir);
    if (L && dir[L - 1] != '/' && L + 1 < sizeof(dir)) { dir[L] = '/'; dir[L + 1] = 0; }
    mkdir(dir, 0777);                     /* ensure the target folder exists */

    remove("sdmc:/moflex_player/unzip_log.txt");
    { struct stat zs; long long zsz = (stat(zip_path, &zs) == 0) ? (long long)zs.st_size : -1;
      uz_log("OPEN %s (size=%lld)", zip_path, zsz); }
    struct zip_t *z = zip_open(zip_path, 0, 'r');
    if (!z) {
        uz_log("zip_open FAILED");
        /* probe the raw layers so the log says WHICH one lies on-device */
        FILE *tf = fopen(zip_path, "rb");
        if (!tf) uz_log("probe: fopen FAILED");
        else {
            struct stat st; long long fsz = (stat(zip_path, &st) == 0) ? (long long)st.st_size : -1;
            int r = fseeko(tf, 0, SEEK_END);
            uz_log("probe: seek(END) rc=%d ftello=%lld stat=%lld", r, (long long)ftello(tf), fsz);
            unsigned char b[8] = {0};
            r = fseeko(tf, (long long)1 << 31, SEEK_SET);          /* just past 2GiB */
            size_t n2 = (r == 0) ? fread(b, 1, 4, tf) : 0;
            uz_log("probe: read@2GiB rc=%d n=%d", r, (int)n2);
            if (fsz > 22) {
                r = fseeko(tf, fsz - 22, SEEK_SET);                 /* the EOCD signature */
                size_t n3 = (r == 0) ? fread(b, 1, 4, tf) : 0;
                uz_log("probe: read@EOCD rc=%d n=%d bytes=%02X %02X %02X %02X (want 50 4B 05 06)",
                       r, (int)n3, b[0], b[1], b[2], b[3]);
            }
            fclose(tf);
        }
        return 0;
    }

    ssize_t n = zip_entries_total(z);
    uz_log("entries_total=%d", (int)n);
    /* pass 1: count the real files so progress has a denominator */
    int total = 0;
    for (ssize_t i = 0; i < n; i++) {
        int rc = zip_entry_openbyindex(z, i);
        if (rc != 0) { if (i < 20) uz_log("entry %d: openbyindex rc=%d", (int)i, rc); continue; }
        const char *name = zip_entry_name(z);
        if (i < 20) uz_log("entry %d: dir=%d junk=%d size=%llu name=%.60s", (int)i,
                           (int)zip_entry_isdir(z), is_junk(name),
                           (unsigned long long)zip_entry_size(z), name ? name : "(null)");
        if (name && !zip_entry_isdir(z) && !is_junk(name)) total++;
        zip_entry_close(z);
    }
    uz_log("countable files=%d", total);
    if (total_out) *total_out = total;

    int extracted = 0, canceled = 0;
    for (ssize_t i = 0; i < n && !canceled; i++) {
        if (zip_entry_openbyindex(z, i) != 0) continue;      /* skip an unreadable entry, keep going */
        const char *name = zip_entry_name(z);
        if (name && !zip_entry_isdir(z) && !is_junk(name)) {
            if (cb && !cb(extracted, total, name)) { zip_entry_close(z); canceled = 1; break; }
            char out[1024];
            snprintf(out, sizeof(out), "%s%s", dir, name);
            mkparents(out);                                  /* create any sub-folders inside the zip */
            /* Stream the entry out chunk by chunk (not zip_entry_fread): the callback runs
             * BETWEEN chunks, so a multi-minute episode file stays cancelable and the UI keeps
             * pumping. (fread also chmod()ed the result, which always fails on SD FAT -- truth
             * = the file exists with the exact uncompressed size.) */
            unsigned long long want = zip_entry_size(z);
            struct uz_stream s = { fopen(out, "wb"), cb, extracted, total, name, 0 };
            if (s.f) {
                zip_entry_extract(z, uz_write, &s);
                fclose(s.f);
                if (s.cancel) { remove(out); canceled = 1; }   /* half-written file: delete it */
            }
            struct stat st;
            if (!canceled && stat(out, &st) == 0 && (unsigned long long)st.st_size == want) extracted++;
        }
        zip_entry_close(z);
    }
    zip_close(z);
    if (canceled) { uz_log("CANCELED at %d/%d", extracted, total); return -1; }
    if (cb) cb(extracted, total, NULL);
    return extracted;
}

int unzip_to_dir(const char *zip_path, const char *dest_dir) {
    return unzip_to_dir_cb(zip_path, dest_dir, NULL, NULL);
}
