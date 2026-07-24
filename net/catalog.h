/* Parses a Clownsec/Zackk catalog.json into rich movie entries. */
#ifndef MOFLEX_CATALOG_H
#define MOFLEX_CATALOG_H

#define CAT_NAMELEN 160
#define CAT_URLLEN  512

typedef struct {
    char name[CAT_NAMELEN];   /* "Title (Year)" for the list */
    char title[128];
    char fname[CAT_NAMELEN];  /* filename to save as */
    char url[CAT_URLLEN];     /* download URL (fully encoded) */
    char art[CAT_URLLEN];     /* artwork/poster URL */
    char sub[CAT_URLLEN];     /* subtitle (.srt) URL from the catalog, "" if none */
    char genres[96];          /* "Genre1, Genre2, ..." */
    char category[32];        /* "Movies" / "TV Shows" / "Music" / ... (normalized across catalogs) */
    char desc[400];           /* description */
    char date[12];            /* dateAdded (YYYY-MM-DD) */
    int  year;
    int  runtime;             /* minutes */
    long long size;           /* file size in bytes (0 if unknown) */
    int  is_zip;              /* 1 = .zip (TV season) */
    int  is3d;                /* 1 = 3D, 0 = 2D, -1 = unknown (guess from filename) */
} CatEntry;

/* kind: 0 = clownsec shape (movies[]/tvShows[]), 1 = zackk shape (items[]).
 * dl_base   = download base URL (clownsec); "" means use the entry's own URL (zackk).
 * art_base  = artwork base URL. Returns entry count. */
int catalog_parse(const char *text, int kind, const char *dl_base, const char *art_base,
                  CatEntry *out, int max);

#endif
