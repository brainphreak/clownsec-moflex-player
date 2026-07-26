/* Extract a .zip archive into a destination folder (creates it).
 * Returns the number of real files extracted (0 = nothing extracted / failed,
 * -1 = canceled by the progress callback; partial files are removed). */
#ifndef MOFLEX_UNZIP_H
#define MOFLEX_UNZIP_H

/* progress: called before each real file AND repeatedly while its data streams out
 * (so the UI stays responsive during multi-minute files). Return nonzero to keep
 * going, 0 to CANCEL the extraction. */
typedef int (*unzip_prog_cb)(int done, int total, const char *name);

/* total_out (may be NULL): number of real files IN the archive (0 = archive unreadable).
 * Compare with the return value to detect partial extraction. */
int unzip_to_dir_cb(const char *zip_path, const char *dest_dir, int *total_out, unzip_prog_cb cb);
int unzip_to_dir(const char *zip_path, const char *dest_dir);

#endif
