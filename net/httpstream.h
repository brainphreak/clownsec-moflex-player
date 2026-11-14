/* A live HTTP response read as a stream: a worker thread runs curl and writes the body into a
 * ring buffer; the player drains it without ever blocking. When the ring is full the worker
 * waits, so a fast server (Plex transcodes many times faster than real time) is held to what the
 * player consumes. hs_close aborts the transfer at once (seek, exit). */
#ifndef HTTPSTREAM_H
#define HTTPSTREAM_H

#include <stddef.h>

typedef struct HttpStream HttpStream;

/* Start fetching url. headers: NULL-terminated "Name: value" strings (copied), or NULL.
 * ring_bytes: buffer size (e.g. 2 MB). NULL if the worker could not start. */
HttpStream *hs_open(const char *url, const char *const *headers, size_t ring_bytes);
/* the same, with the worker on a chosen core first (1 = the system core: a moflex plays its
 * decode on core 2, which would starve a lowest-priority download there) */
HttpStream *hs_open_core(const char *url, const char *const *headers, size_t ring_bytes, int core);
/* Copy up to n buffered bytes into p; returns how many (0 = nothing yet). Never blocks. */
size_t hs_read(HttpStream *s, void *p, size_t n);
size_t hs_buffered(HttpStream *s);
/* The transfer has ended: 1 = completed, -1 = failed (see hs_http), 0 = still running. */
int    hs_done(HttpStream *s);
long   hs_http(HttpStream *s);          /* HTTP status once known, else 0 */
unsigned long long hs_total(HttpStream *s);   /* body bytes received so far */
void   hs_close(HttpStream *s);         /* abort + join + free */

#endif
