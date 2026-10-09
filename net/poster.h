/* Poster/artwork fetch + JPEG decode + cache for the catalog info panel. */
#ifndef MOFLEX_POSTER_H
#define MOFLEX_POSTER_H
#include <3ds.h>

/* Fetch (or load from sdmc cache), decode the JPEG at art_url, scale to pw x ph,
 * and write it row-major RGB565 into out (pw*ph u16s). cache_key is sanitized to a
 * filename so repeat views are instant. Returns 1 on success, 0 on no-art/failure. */
int poster_get(const char *art_url, const char *cache_key, u16 *out, int pw, int ph);

/* Write a metadata sidecar next to a cached catalog poster, as "<sanitized key>.nfo" in the
 * art directory, in the same "key: value" shape movieinfo uses. The poster cache is otherwise
 * pixels only -- the catalog itself is never saved -- so anything reading art/ back off the
 * card has a title and a picture and nothing else. Best effort; skipped if one already exists. */
void poster_save_meta(const char *cache_key, const char *nfo_text);

/* Decode a local JPEG/PNG file and nearest-scale it into out (pw*ph RGB565). No caching --
 * the caller owns the cache. Returns 1 on success, 0 on failure. */
int poster_decode_file(const char *path, u16 *out, int pw, int ph);

#endif
