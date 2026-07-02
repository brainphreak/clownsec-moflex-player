/* Poster/artwork fetch + JPEG decode + cache for the catalog info panel. */
#ifndef MOFLEX_POSTER_H
#define MOFLEX_POSTER_H
#include <3ds.h>

/* Fetch (or load from sdmc cache), decode the JPEG at art_url, scale to pw x ph,
 * and write it row-major RGB565 into out (pw*ph u16s). cache_key is sanitized to a
 * filename so repeat views are instant. Returns 1 on success, 0 on no-art/failure. */
int poster_get(const char *art_url, const char *cache_key, u16 *out, int pw, int ph);

#endif
