/* CSXTRA trailer reader (metadata side): imports embedded library info + poster art from a
 * SUPER MOFLEX so the file needs no scraping -- works fully offline. */
#ifndef MOFLEX_TRAILER_H
#define MOFLEX_TRAILER_H

/* If the file carries NFO0 (key=value metadata) and/or ART5 (pre-scaled RGB565 poster),
 * write them into moviedata/ for the library. Returns 1 if info was imported. */
int trailer_import_movieinfo(const char *moviepath);

/* Same, but SAVE against a different key (a SHOW folder) and prefer the 'showdesc' field --
 * used so a season folder's library tile gets the series description + poster from any
 * episode's trailer. */
int trailer_import_movieinfo_key(const char *moviepath, const char *savekey, int show_level);

/* 1 if the file carries a CSXTRA trailer at all (16-byte footer probe). */
int trailer_present(const char *moviepath);

#endif
