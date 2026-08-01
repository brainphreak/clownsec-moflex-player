/* CSXTRA trailer reader (metadata side): imports embedded library info + poster art from a
 * SUPER MOFLEX so the file needs no scraping -- works fully offline. */
#ifndef MOFLEX_TRAILER_H
#define MOFLEX_TRAILER_H

/* If the file carries NFO0 (key=value metadata) and/or ART5 (pre-scaled RGB565 poster),
 * write them into moviedata/ for the library. Returns 1 if info was imported. */
int trailer_import_movieinfo(const char *moviepath);

#endif
