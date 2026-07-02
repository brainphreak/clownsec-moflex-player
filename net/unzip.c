#include "unzip.h"
#include "zip.h"
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>

bool unzip_to_dir(const char *zip_path, const char *dest_dir) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", dest_dir);
    size_t L = strlen(dir);
    if (L && dir[L - 1] != '/' && L + 1 < sizeof(dir)) { dir[L] = '/'; dir[L + 1] = 0; }
    mkdir(dir, 0777);                     /* ensure the target folder exists */
    return zip_extract(zip_path, dir, NULL, NULL) == 0;
}
