/* Play a .moflex that is embedded (unencrypted) inside a 3DS .cia, in place. */
#ifndef CIA_MOFLEX_H
#define CIA_MOFLEX_H
#include <stdio.h>
#include <stdint.h>
#include "moflex_demux.h"

typedef struct { char name[128]; int64_t off, size; } CiaMoflex;

/* List every unencrypted .moflex inside a CIA's RomFS. Returns the count (0 = none / encrypted). */
int cia_list_moflex(const char *path, CiaMoflex *out, int max);

/* Locate the first embedded .moflex. Returns 1 + byte offset/size, else 0. */
int cia_find_moflex(const char *path, int64_t *out_off, int64_t *out_size);

/* Fast yes/no: does this CIA contain at least one playable (unencrypted) moflex? Cheap enough to
 * call while listing a folder (bulk-reads the file table). */
int cia_has_moflex(const char *path);

/* True if `path` looks like a .cia by extension. */
int cia_is_cia(const char *path);

/* Which embedded moflex to open for `path` on the next mfx_open_auto (used when a CIA has several).
 * Cleared automatically once consumed elsewhere; call cia_clear_selection() when done. */
void cia_set_selection(const char *path, int64_t off, int64_t size, const char *title);
void cia_clear_selection(void);

/* The selected embedded movie's display title for `path` (from movie_title.csv), or NULL.
 * The player uses this so its on-screen title is the movie, not the CIA filename. */
const char *cia_selection_title(const char *path);

/* If a CIA selection is active for `path`, writes a unique per-moflex suffix (e.g. "_138820") to
 * `out` so the resume position is tracked per embedded video; otherwise writes "". */
void cia_selection_suffix(const char *path, char *out, unsigned cap);

/* Open a demux from a path that is either a plain .moflex or a CIA with an embedded moflex.
 * If a selection was set for this path it opens that one; else the first. Returns 0 on success. */
int mfx_open_auto(MfxDemux *m, FILE *f, const char *path);

#endif
