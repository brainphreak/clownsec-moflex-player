/* What a video file says it is -- movie or TV episode, which show, season, episode, episode title.
 * The SUPER MOFLEX's own info (kind= / title= / season= / episode= / eptitle=, written by Moflex
 * Studio 1.7+) decides; the file name ("Show (2009) (3D) (S) - S01e02 - Title.moflex") only fills in
 * what the file does not say, which is everything for files made before those fields existed.
 * Season 0 is a show's specials and episode 0 a season's special: both are episodes.
 * Plain C (stdio only) so it can be tested on a PC. */
#ifndef MOFLEX_EPMETA_H
#define MOFLEX_EPMETA_H
#include <stddef.h>

typedef struct {
    int  is_ep;            /* 1 = a TV episode */
    int  season, episode;  /* when is_ep */
    int  year;             /* the show's / film's year, 0 = unknown */
    int  from_file;        /* 1 = the file's own info said what it is (kind=) */
    char show[96];         /* the show's display name: "To Love Ru OVAs (2009)" */
    char key[96];          /* the show's grouping key (see epmeta_show_key) */
    char eptitle[96];      /* the episode's title, "" if unknown */
} EpMeta;

/* Fill m for the file at path (cached per path; cheap after the first call). Returns m->is_ep. */
int  epmeta_get(const char *path, EpMeta *m);

/* The file name alone (no file access). Returns m->is_ep. */
int  epmeta_from_name(const char *fname, EpMeta *m);

/* Forget cached results (a library scan starts over; a file may have been re-saved). */
void epmeta_cache_clear(void);

/* Grouping key for a show name: case, punctuation and the tags a file name carries -- a year,
 * "(3D)", "(2D)", "(S)" -- ignored, so "To Love Ru OVAs (2009) (3D) (S)" from a file name and
 * "To Love Ru OVAs" from a file's own info are the same show. */
void epmeta_show_key(const char *name, char *out, size_t cap);

/* 1 when the two names are the same show (same key). */
int  epmeta_same_show(const char *a, const char *b);

/* The episode's list label: "S04e00 - Title" (or just "S04e00"). */
void epmeta_label(const EpMeta *m, char *out, size_t cap);

/* Watch order for two episodes of one show: by season (season 0, the specials, after the
 * numbered seasons), then episode. <0, 0, >0 like strcmp. */
int  epmeta_order(const EpMeta *a, const EpMeta *b);

#endif
