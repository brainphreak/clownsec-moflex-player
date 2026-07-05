#include "unzip.h"
#include "zip.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <stdio.h>

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

int unzip_to_dir(const char *zip_path, const char *dest_dir) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", dest_dir);
    size_t L = strlen(dir);
    if (L && dir[L - 1] != '/' && L + 1 < sizeof(dir)) { dir[L] = '/'; dir[L + 1] = 0; }
    mkdir(dir, 0777);                     /* ensure the target folder exists */

    struct zip_t *z = zip_open(zip_path, 0, 'r');
    if (!z) return 0;

    ssize_t n = zip_entries_total(z);
    int extracted = 0;
    for (ssize_t i = 0; i < n; i++) {
        if (zip_entry_openbyindex(z, i) != 0) continue;      /* skip an unreadable entry, keep going */
        const char *name = zip_entry_name(z);
        if (name && !zip_entry_isdir(z) && !is_junk(name)) {
            char out[1024];
            snprintf(out, sizeof(out), "%s%s", dir, name);
            mkparents(out);                                  /* create any sub-folders inside the zip */
            if (zip_entry_fread(z, out) == 0) extracted++;   /* one real file failing doesn't abort the rest */
        }
        zip_entry_close(z);
    }
    zip_close(z);
    return extracted;
}
