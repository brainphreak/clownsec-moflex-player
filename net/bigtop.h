/* Clownsec Big Top: the player's side of the moflex media server (github.com/brainphreak/
 * clownsec-bigtop). Finds a server on the network, lists its libraries / shows / episodes, and
 * gives the player a "bigtop:" path per video that opens as a stream from the server. */
#ifndef BIGTOP_H
#define BIGTOP_H

#include <stdio.h>

typedef struct {
    char url[128];       /* "http://192.168.1.20:32500" */
    char name[64];
    char id[24];         /* the server's own id: survives an address change */
    char client[32];     /* this console's id, for per-console watch progress */
} BtCfg;
int  bt_cfg_load(BtCfg *c);           /* 1 = a server is set up */
void bt_cfg_save(const BtCfg *c);

/* one row of a list: library, movie, show, season or episode */
typedef struct {
    char type[8];        /* "lib" "movie" "show" "season" "episode" */
    char id[40];
    char title[96];
    int  year, runtime, is3d, poster, season, episode, episodes, seen, watched, super;
    long long pos, size;          /* resume point (ms), file size */
    char genres[96];
    char summary[420];
} BtItem;

typedef struct { char url[128]; char name[64]; char id[24]; char version[24]; } BtServer;

/* Ask the network who runs Big Top (UDP broadcast); answers collected for wait_ms. */
int  bt_discover(BtServer *out, int max, int wait_ms);
/* Is there a Big Top at url? Fills info. 1 = yes. */
int  bt_ping(const char *url, BtServer *info);

int  bt_libraries(const BtCfg *c, BtItem **out);                 /* count, or -1 (bt_error) */
/* path: "/api/libraries/<id>", "/api/shows/<id>" (its seasons), "/api/shows/<id>/seasons/<n>" */
int  bt_list(const BtCfg *c, const char *path, BtItem **out);
int  bt_item(const BtCfg *c, const char *id, BtItem *out);       /* fresh details (resume point) */
const char *bt_error(void);

void bt_poster_url(const BtCfg *c, const BtItem *it, int w, int h, char *out, int cap);

/* Where this console is in a video ("playing" / "paused" / "stopped"); blocking, call off the UI. */
void bt_progress(const BtCfg *c, const char *item_id, long long pos_ms, long long dur_ms, const char *state);
void bt_watched(const BtCfg *c, const char *item_id, int on);

/* The player's path for an item: "bigtop:/<server id>/<item id>/<title>.moflex" -- the title so
 * the playback screen and the per-video settings read right. */
void bt_media_path(const BtCfg *c, const BtItem *it, const char *title, char *out, int cap);
/* moflex_set_open_hook target: opens a bigtop: path as a stream from the server set up. */
FILE *bt_open_media(const char *path);
/* the item id inside a bigtop: path ("" if it isn't one) */
void bt_path_item(const char *path, char *id, int cap);

#endif
