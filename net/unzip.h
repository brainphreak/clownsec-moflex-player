/* Extract a .zip archive into a destination folder (creates it). */
#ifndef MOFLEX_UNZIP_H
#define MOFLEX_UNZIP_H
#include <stdbool.h>
bool unzip_to_dir(const char *zip_path, const char *dest_dir);
#endif
