/* Background music for the shop: WAVs (PCM16, or IMA ADPCM) from romfs:/music -- the song that
 * ships with the app -- plus the owner's own folder on the card, shuffled, streamed to the DSP.
 * No mp3: see music.c for why. */
#ifndef STORE_MUSIC_H
#define STORE_MUSIC_H

/* Collect romfs:/music and `dir`, shuffle, and start playing. Returns the track count, 0 if there is
 * nothing to play or no dsp firm (in which case the shop is simply quiet). */
int         music_init(const char *dir);
void        music_next(void);      /* skip to the next track -- what the jukebox does */
int         music_count(void);
const char *music_now(void);       /* the playing track's name, without its extension */
void        music_exit(void);

#endif
