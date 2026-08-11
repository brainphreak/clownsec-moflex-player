/* Background music for the shop: a folder of PCM16 WAVs, shuffled, streamed to the DSP.
 * Nothing decodes -- see music.c for why this is not mp3. */
#ifndef STORE_MUSIC_H
#define STORE_MUSIC_H

/* Scan `dir` for .wav, shuffle, and start playing. Returns the track count, 0 if there is
 * nothing to play or no dsp firm (in which case the shop is simply quiet). */
int         music_init(const char *dir);
void        music_next(void);      /* skip to the next track -- what the jukebox does */
int         music_count(void);
const char *music_now(void);       /* the playing track's name, without its extension */
void        music_exit(void);

#endif
