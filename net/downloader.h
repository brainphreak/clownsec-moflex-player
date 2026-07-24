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

/* Download url -> dest file path. Returns true on success.
 * Resumable: data streams to a central temp file keyed by the URL
 * (sdmc:/moflex_player/downloads/<hash>.part) and is moved to dest only when complete -- so a
 * dropped transfer is retried automatically (resuming via HTTP Range), the partial survives app
 * restarts, and it resumes even if you later pick a DIFFERENT destination folder. A user cancel
 * keeps the partial for a later resume. */
bool download_to_file(const char *url, const char *dest, dl_progress_cb cb, void *user);

/* Bytes already downloaded for `url` (size of its leftover central partial), 0 if none.
 * Keyed by URL, so it finds the partial regardless of destination folder. */
long long download_partial_bytes(const char *url);

/* nonzero when the LAST download_to_file failure was a WRITE error (SD full / card trouble),
 * so the UI can say "SD full?" instead of blaming the network */
int download_write_failed(void);

/* last HTTP status a failed download saw (e.g. 404 = the file is missing on the server);
 * 0 when the failure never got an HTTP response */
long download_last_http(void);

/* Delete the leftover partial for `url` (for "start over"). */
void download_discard_partial(const char *url);

/* Download url -> malloc'd buffer (NUL-terminated). Caller frees *out.
 * Good for fetching the catalog JSON. Caps at max_bytes. */
/* Prime the thread-safe lazy init (call once on the main thread at startup). */
void downloader_prime(void);

bool download_to_mem(const char *url, char **out, size_t *out_len, size_t max_bytes);

/* Set a global abort hook: if it returns non-zero, the CURRENT transfer aborts. Used to make
 * poster loads cancellable so scrolling stays fluid. Pass NULL to clear. */
void download_set_abort(int (*abort_cb)(void));

#endif
