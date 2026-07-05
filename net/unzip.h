/* Extract a .zip archive into a destination folder (creates it).
 * Returns the number of real files extracted (0 = nothing extracted / failed). */
#ifndef MOFLEX_UNZIP_H
#define MOFLEX_UNZIP_H
int unzip_to_dir(const char *zip_path, const char *dest_dir);
#endif
