/* HTTPS/HTTP downloader for the 3DS (via the httpc service).
 * Follows redirects; SSL cert verification disabled (3DS cert store is limited). */
#ifndef MOFLEX_DOWNLOADER_H
#define MOFLEX_DOWNLOADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* progress callback: downloaded/total bytes (total may be 0 if unknown).
 * return false to cancel the transfer. */
typedef bool (*dl_progress_cb)(void *user, uint32_t downloaded, uint32_t total);

bool downloader_init(void);
void downloader_exit(void);

/* Download url -> dest file path. Returns true on success. */
bool download_to_file(const char *url, const char *dest, dl_progress_cb cb, void *user);

/* Download url -> malloc'd buffer (NUL-terminated). Caller frees *out.
 * Good for fetching the catalog JSON. Caps at max_bytes. */
bool download_to_mem(const char *url, char **out, size_t *out_len, size_t max_bytes);

/* Set a global abort hook: if it returns non-zero, the CURRENT transfer aborts. Used to make
 * poster loads cancellable so scrolling stays fluid. Pass NULL to clear. */
void download_set_abort(int (*abort_cb)(void));

#endif
