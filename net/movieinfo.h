/* Per-movie metadata + poster, stored in ONE hidden folder keyed by filename, so the info
 * follows a movie wherever it lives on the SD card and can be hand-authored for local files.
 *
 *   sdmc:/moflex_player/moviedata/<name>.nfo   plain "key: value" text (title/year/runtime/genres/desc)
 *   sdmc:/moflex_player/moviedata/<name>.jpg    poster (or .png) -- optional, user- or catalog-provided
 *   sdmc:/moflex_player/moviedata/<name>.p565   decoded poster cache (generated on first view)
 *
 * <name> is the movie's filename with its extension removed (verbatim, so it is easy to author). */
#ifndef MOFLEX_MOVIEINFO_H
#define MOFLEX_MOVIEINFO_H
#include <3ds.h>
#include "catalog.h"

#define MOVIEDATA_DIR "sdmc:/moflex_player/moviedata"

/* Load <movie>.nfo into `out` (name/title/year/runtime/genres/desc filled; `fname` set to the
 * movie's basename so the 2D/3D badge still works; `size` from the file). Returns 1 if a .nfo
 * exists, else 0 (out left zeroed). */
int movieinfo_load(const char *moviepath, CatEntry *out);

/* Load the poster for a movie into `out` (pw*ph RGB565): decoded-cache hit, else decode a
 * sidecar .jpg/.png and cache it. Returns 1 if a poster exists, else 0. */
int movieinfo_poster(const char *moviepath, u16 *out, int pw, int ph);

/* True if a movie has ANY moviedata (an .nfo or a poster) -- used to decide info-panel vs logo. */
int movieinfo_have(const char *moviepath);

/* Write <movie>.nfo from a catalog entry, and cache its poster (`poster` = pw*ph RGB565, or NULL). */
void movieinfo_save(const char *moviepath, const CatEntry *e, const u16 *poster, int pw, int ph);

#endif
