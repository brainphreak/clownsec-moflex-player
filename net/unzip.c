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
    if (!z) { uz_log("zip_open FAILED"); return 0; }

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

    int extracted = 0;
    for (ssize_t i = 0; i < n; i++) {
        if (zip_entry_openbyindex(z, i) != 0) continue;      /* skip an unreadable entry, keep going */
        const char *name = zip_entry_name(z);
        if (name && !zip_entry_isdir(z) && !is_junk(name)) {
            if (cb) cb(extracted, total, name);
            char out[1024];
            snprintf(out, sizeof(out), "%s%s", dir, name);
            mkparents(out);                                  /* create any sub-folders inside the zip */
            /* Do NOT trust zip_entry_fread's return: after writing the file it chmod()s the
             * archive's unix permissions, and chmod always fails on the 3DS SD -> every perfectly
             * extracted file reported failure ("Extract FAILED" with all files present).
             * Truth = the file exists with the exact uncompressed size. */
            unsigned long long want = zip_entry_size(z);
            zip_entry_fread(z, out);
            struct stat st;
            if (stat(out, &st) == 0 && (unsigned long long)st.st_size == want) extracted++;
        }
        zip_entry_close(z);
    }
    zip_close(z);
    if (cb) cb(extracted, total, NULL);
    return extracted;
}

int unzip_to_dir(const char *zip_path, const char *dest_dir) {
    return unzip_to_dir_cb(zip_path, dest_dir, NULL, NULL);
}
