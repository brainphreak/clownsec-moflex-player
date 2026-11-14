/* Plex Media Server as a media source (local network, plain HTTP).
 *
 * Browsing is Plex's JSON API; playback asks the universal transcoder for one continuous MPEG-TS
 * stream of H.264 + AAC sized for the top screen (verified against PMS 1.43: protocol=http,
 * a fresh session per start, offset= for seeks with timestamps restarting near zero).
 * The token is optional: with none, the server must list this network under "allowed without
 * auth"; with one, every request carries it as X-Plex-Token. Settings live on the SD card only. */
#ifndef PLEX_H
#define PLEX_H

#include <stdint.h>
#include "httpstream.h"

typedef struct {
    char url[128];      /* the server in use: "http://10.0.0.31:32400" (no trailing slash) */
    char token[64];     /* that server's access token; "" = none (server allows this network) */
    char client[40];    /* this console's X-Plex-Client-Identifier, made once */
    char account[64];   /* the plex.tv account token from linking; "" = not linked */
    char name[64];      /* the server's name, for the screens */
    char machine[48];   /* the server's clientIdentifier (from the account list); "" = typed address.
                         * Lets a saved server be found again at another address (home <-> away). */
} PlexCfg;

typedef struct {
    char   key[192];    /* what to list next ("/library/sections/1/all", ".../children") */
    char   rating[16];  /* ratingKey: the item itself */
    char   type[12];    /* "movie" "show" "season" "episode" "section" ... */
    char   title[96];
    char   edition[48];  /* Plex "edition" ("3D Half SBS", "3D Full SBS" ...), "" = none */
    char   thumb[160];
    char   poster[160];  /* the portrait art to show: the item's own, or the season/show's for an episode */
    char   summary[400];
    char   genres[80];   /* "Action, Sci-Fi" */
    char   crating[12];  /* content rating: "PG-13", "TV-MA" */
    int    width, height; /* the source video, from its first media version */
    int    index, parent_index, year;
    int    leaf_count, viewed_leaf;   /* shows/seasons: episodes, watched */
    int    viewed;
    long long duration_ms, offset_ms; /* offset = Plex's resume point */
    long long stamp;    /* a library (section): when its contents last changed -- the SD cache's key */
} PlexItem;

int  plex_cfg_load(PlexCfg *c);          /* 1 = a server is configured */

/* Linking without typing a token (what Plex's TV apps do): plex_pin_start gets a short code for
 * the viewer to enter at plex.tv/link; plex_pin_poll returns 1 with the token once they have.
 * Poll every couple of seconds; -1 = expired or failed. */
int  plex_pin_start(const PlexCfg *c, long long *id, char code[16]);
int  plex_pin_poll(const PlexCfg *c, long long id, char *token, int cap);
/* Is this token a plex.tv ACCOUNT token? (A link made before the account was stored apart left
 * it only as the server token.) 1 = yes, 0 = no / can't tell. */
int  plex_token_is_account(const PlexCfg *c, const char *token);
/* The linked account's servers that answer from where this 3DS is (plain HTTP), owned ones first;
 * each with its own access token (a friend's shared server has one of its own). Every server's
 * addresses are tried -- home-network ones first, then its internet (remote access) ones -- and
 * the first that answers is the one returned; servers that answer on none are left out. */
typedef struct { char name[64]; char url[128]; char token[64]; char machine[48]; int owned; } PlexServer;
int  plex_servers(const PlexCfg *c, PlexServer *out, int max);
/* The saved server stopped answering (e.g. it was chosen at home and this 3DS is now away):
 * look it up again through the linked account and switch to whichever of its addresses answers.
 * 1 = found at a working address (c updated and saved), 0 = not. */
int  plex_relocate(PlexCfg *c);
/* 1 = the last failed request never reached the server (no answer / timeout), as opposed to the
 * server answering with an error. Only then is plex_relocate worth trying. */
int  plex_unreachable(void);
/* Optional: called while a long listing loads page by page (got so far, total; total 0 = unknown). */
void plex_set_progress(void (*fn)(int got, int total));
void plex_cfg_save(const PlexCfg *c);

/* GET path (e.g. "/library/sections") and return its entries. *items is malloc'd (free it).
 * One reply as a rule; if that is cut off part-way (a big library through a slow remote
 * connection), it is fetched again in pages, so thousands of items load remotely too.
 * Returns the count, or -1 (plex_error() says why). */
int  plex_list(const PlexCfg *c, const char *path, PlexItem **items);
const char *plex_error(void);
/* Play-attempt log on the SD card (sdmc:/moflex_player/plex_log.txt): every request with its
 * answer and timing, the transcoder's decision, and how the stream arrived -- one file a user
 * can send when playback fails. plex_log_begin starts a fresh file and turns logging on;
 * plex_log_end turns it off. Tokens are never written. */
void plex_log_begin(const char *what);
void plex_log(const char *fmt, ...);
void plex_log_end(void);
/* SD-card copy of a big listing (a whole library), so it opens at once next time. Valid only
 * while the library's stamp (PlexItem.stamp from /library/sections) is unchanged and the copy is
 * under 3 days old. load: count, or -1 = no valid copy. */
int  plex_cache_load(const PlexCfg *c, const char *path, long long stamp, PlexItem **items);
void plex_cache_save(const PlexCfg *c, const char *path, long long stamp, const PlexItem *items, int n);
/* URL of the server's resized JPEG for a thumb path (fits w x h), for the poster cache. */
void plex_art_url(const PlexCfg *c, const char *thumb, int w, int h, char *out, int cap);

/* Start a transcoded stream of a movie/episode at offset_us. session (>= 24 bytes) receives
 * the session id to stop later. is3d: ask for a frame that keeps both eyes side by side (up to
 * 800x480 -- Plex keeps the layout: half SBS 1080p -> 800x450, full SBS -> 796x224); 0 = 400x240. */
HttpStream *plex_stream_open(const PlexCfg *c, const char *rating_key, int64_t offset_us,
                             int is3d, int burn_subs, char *session);

/* ---- subtitles ----
 * External text files download as-is and are drawn by the player (size/position/3D depth). An
 * EMBEDDED text track is copied out by Plex as a sidecar while the video plays (below) and drawn
 * the same way; an embedded PICTURE track (PGS, VobSub) can only be burned into the video. Either
 * needs the track to be the part's selected subtitle -- so: select, play, restore. */
typedef struct {
    long long id;
    char codec[12];      /* "srt" "ass" "pgs" "vobsub" ... */
    char lang[8];        /* "eng" */
    char title[48];
    char name[32];       /* the file's own track name ("forced", "SDH", "Signs"), often hidden by title */
    char key[96];        /* external file: "/library/streams/<id>"; "" = embedded */
    int  forced, selected;
} PlexSub;
typedef struct {
    long long id;
    char lang[8];        /* "eng" */
    char title[48];      /* Plex's own name: "English (AC3 5.1)" */
    int  selected;
} PlexAudio;
/* The item's subtitle tracks, its part id, its selected audio language and (aud != NULL) every
 * audio track. Returns the subtitle count, -1. */
int  plex_item_streams(const PlexCfg *c, const char *rating_key, PlexSub *subs, int max,
                       long long *part_id, char *audio_lang, int alcap,
                       PlexAudio *aud, int amax, int *nau);
/* make this the part's audio track: the transcoder plays the selected one. Like any Plex client,
 * the choice stays with the item for next time. */
int  plex_select_audio(const PlexCfg *c, long long part_id, long long audio_id);
int  plex_sub_is_text(const char *codec);   /* a text codec (srt, ass...), not a picture (pgs, vobsub) */
/* An EMBEDDED text track, copied out by Plex while the video plays (the track must be the part's
 * selected subtitle until finish). take() hands over whole SRT blocks in movie time as they come;
 * finish() stops it and, if the whole track arrived, saves it at save_path (1 = saved). */
typedef struct PlexSubFetch PlexSubFetch;
/* select_sub_id != 0: make that the part's selected subtitle first (the caller restores it) */
PlexSubFetch *plex_subs_fetch_start(const PlexCfg *c, const char *rating_key, long long start_s,
                                    long long part_id, long long select_sub_id);
int  plex_subs_fetch_take(PlexSubFetch *f, char *buf, int cap);
int  plex_subs_fetch_finish(PlexSubFetch *f, const char *save_path);
void plex_sub_cache_path(const PlexCfg *c, long long stream_id, char *out, int cap);
int  plex_select_sub(const PlexCfg *c, long long part_id, long long sub_id);   /* 0 = none */
int  plex_download(const PlexCfg *c, const char *path, const char *dest);     /* 1 = saved */
void plex_stream_stop(const PlexCfg *c, const char *session);
/* Ask the server, before the playback screen, whether it will transcode this item (same request
 * as plex_stream_open makes). 1 = yes; 0 = no / no answer, plex_error() has Plex's reason. */
int  plex_stream_check(const PlexCfg *c, const char *rating_key, int64_t offset_us, int is3d, int burn_subs);
/* Tell Plex where playback is (state: "playing" "paused" "stopped") -- resume + watched sync. */
void plex_timeline(const PlexCfg *c, const char *rating_key, const char *state,
                   long long pos_ms, long long dur_ms);

#endif
