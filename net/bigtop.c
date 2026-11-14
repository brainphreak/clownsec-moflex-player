/* Clownsec Big Top client. See bigtop.h. */
#include "bigtop.h"
#include "httpfile.h"
#include "downloader.h"
#include "cJSON.h"
#include <3ds.h>
#include <curl/curl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

#define BT_CFG   "sdmc:/moflex_player/bigtop.cfg"
#define BT_DPORT 32501
#ifndef SO_BROADCAST
#define SO_BROADCAST 0x0020
#endif

static char g_err[200];
const char *bt_error(void) { return g_err; }

int bt_cfg_load(BtCfg *c) {
    memset(c, 0, sizeof *c);
    FILE *f = fopen(BT_CFG, "rb");
    if (f) {
        char ln[160]; int i = 0;
        while (fgets(ln, sizeof ln, f) && i < 4) {
            char *nl = strpbrk(ln, "\r\n"); if (nl) *nl = 0;
            if (i == 0) snprintf(c->url, sizeof c->url, "%s", ln);
            if (i == 1) snprintf(c->name, sizeof c->name, "%s", ln);
            if (i == 2) snprintf(c->id, sizeof c->id, "%s", ln);
            if (i == 3) snprintf(c->client, sizeof c->client, "%s", ln);
            i++;
        }
        fclose(f);
    }
    if (!c->client[0]) {                      /* one id per console, made once */
        u64 t = osGetTime();
        snprintf(c->client, sizeof c->client, "3ds-%08lx%04lx", (unsigned long)(t & 0xffffffff),
                 (unsigned long)(svcGetSystemTick() & 0xffff));
        bt_cfg_save(c);
    }
    return c->url[0] != 0;
}
void bt_cfg_save(const BtCfg *c) {
    mkdir("sdmc:/moflex_player", 0777);
    FILE *f = fopen(BT_CFG, "wb"); if (!f) return;
    fprintf(f, "%s\n%s\n%s\n%s\n", c->url, c->name, c->id, c->client);
    fclose(f);
}

/* ---- HTTP ---- */
typedef struct { char *p; size_t n, cap; } Mem;
static size_t on_mem(char *p, size_t sz, size_t nm, void *u) {
    Mem *m = (Mem *)u; size_t k = sz * nm;
    if (m->n + k + 1 > m->cap) {
        if (m->n + k + 1 > (16u << 20)) return 0;
        size_t nc = m->cap ? m->cap : 65536; while (nc < m->n + k + 1) nc *= 2;
        char *g = (char *)realloc(m->p, nc); if (!g) return 0;
        m->p = g; m->cap = nc;
    }
    memcpy(m->p + m->n, p, k); m->n += k; m->p[m->n] = 0;
    return k;
}
/* GET (post=1: POST) base+path; body in *out. Returns the HTTP status, -1 when unreachable. */
static long bt_req(const char *base, const char *client, const char *path, char **out, int post, int quick) {
    *out = NULL;
    if (!downloader_init()) { snprintf(g_err, sizeof g_err, "Network is not available."); return -1; }
    CURL *e = curl_easy_init(); if (!e) return -1;
    char url[512]; snprintf(url, sizeof url, "%s%s", base, path);
    char ch[64]; snprintf(ch, sizeof ch, "X-BigTop-Client: %s", client ? client : "");
    struct curl_slist *hl = curl_slist_append(NULL, ch);
    Mem m = { 0 };
    curl_easy_setopt(e, CURLOPT_URL, url);
    curl_easy_setopt(e, CURLOPT_HTTPHEADER, hl);
    if (post) curl_easy_setopt(e, CURLOPT_POSTFIELDS, "");
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, quick ? 4L : 8L);
    curl_easy_setopt(e, CURLOPT_TIMEOUT, quick ? 8L : 60L);
    curl_easy_setopt(e, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, on_mem);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, &m);
    CURLcode rc = curl_easy_perform(e);
    long code = 0; curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hl); curl_easy_cleanup(e);
    if (rc != CURLE_OK) {
        snprintf(g_err, sizeof g_err, "Can't reach Big Top at %s (%s).", base, curl_easy_strerror(rc));
        free(m.p); return -1;
    }
    if (code >= 400) snprintf(g_err, sizeof g_err, "Big Top answered HTTP %ld.", code);
    *out = m.p;
    return code;
}

static void js(cJSON *o, const char *k, char *dst, size_t cap) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(v) && v->valuestring) snprintf(dst, cap, "%s", v->valuestring);
}
static long long jn(cJSON *o, const char *k) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v)) return (long long)v->valuedouble;
    if (cJSON_IsTrue(v)) return 1;
    return 0;
}
static void fill(BtItem *it, cJSON *e) {
    memset(it, 0, sizeof *it);
    js(e, "type", it->type, sizeof it->type); js(e, "id", it->id, sizeof it->id);
    js(e, "title", it->title, sizeof it->title); js(e, "genres", it->genres, sizeof it->genres);
    js(e, "summary", it->summary, sizeof it->summary);
    it->year = (int)jn(e, "year"); it->runtime = (int)jn(e, "runtime"); it->is3d = (int)jn(e, "is3d");
    it->poster = (int)jn(e, "poster"); it->season = (int)jn(e, "season"); it->episode = (int)jn(e, "episode");
    it->episodes = (int)jn(e, "episodes"); it->seen = (int)jn(e, "seen"); it->watched = (int)jn(e, "watched");
    it->super = (int)jn(e, "super"); it->pos = jn(e, "pos"); it->size = jn(e, "size");
}
static int fill_array(cJSON *arr, BtItem **out, const char *force_type) {
    int n = cJSON_GetArraySize(arr);
    BtItem *v = (BtItem *)calloc((size_t)(n ? n : 1), sizeof(BtItem));
    if (!v) { snprintf(g_err, sizeof g_err, "Not enough memory for the list."); return -1; }
    int i = 0; cJSON *e;
    cJSON_ArrayForEach(e, arr) {
        fill(&v[i], e);
        if (force_type) snprintf(v[i].type, sizeof v[i].type, "%s", force_type);
        i++;
    }
    *out = v;
    return i;
}

int bt_ping(const char *url, BtServer *info) {
    char *body = NULL;
    long code = bt_req(url, "", "/api/server", &body, 0, 1);
    int ok = 0;
    if (code == 200 && body) {
        cJSON *r = cJSON_Parse(body);
        char svc[32] = "";
        if (r) js(r, "service", svc, sizeof svc);
        if (r && !strcmp(svc, "clownsec-bigtop")) {
            ok = 1;
            if (info) {
                memset(info, 0, sizeof *info);
                snprintf(info->url, sizeof info->url, "%s", url);
                js(r, "name", info->name, sizeof info->name); js(r, "id", info->id, sizeof info->id);
                js(r, "version", info->version, sizeof info->version);
            }
        } else if (code == 200) snprintf(g_err, sizeof g_err, "That address isn't a Big Top server.");
        cJSON_Delete(r);
    }
    free(body);
    return ok;
}

int bt_discover(BtServer *out, int max, int wait_ms) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);   /* the 3DS may not need/know it */
    struct sockaddr_in me; memset(&me, 0, sizeof me);
    me.sin_family = AF_INET; me.sin_port = 0; me.sin_addr.s_addr = INADDR_ANY;
    bind(fd, (struct sockaddr *)&me, sizeof me);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);       /* never a blocking wait */
    static const char q[] = "CLOWNSEC-BIGTOP?";
    struct in_addr ip, mask, bcast;
    struct sockaddr_in to; memset(&to, 0, sizeof to);
    to.sin_family = AF_INET; to.sin_port = htons(BT_DPORT);
    int n = 0;
    u64 end = osGetTime() + (u64)wait_ms, next_send = 0;
    while (osGetTime() < end && n < max) {
        if (osGetTime() >= next_send) {                            /* ask a few times: UDP gets lost */
            if (SOCU_GetIPInfo(&ip, &mask, &bcast) == 0) {
                to.sin_addr = bcast;
                sendto(fd, q, sizeof q - 1, 0, (struct sockaddr *)&to, sizeof to);
            }
            to.sin_addr.s_addr = INADDR_BROADCAST;
            sendto(fd, q, sizeof q - 1, 0, (struct sockaddr *)&to, sizeof to);
            next_send = osGetTime() + 600;
        }
        char buf[512]; struct sockaddr_in from; socklen_t fl = sizeof from;
        int len = recvfrom(fd, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
        if (len <= 0) { svcSleepThread(20 * 1000 * 1000LL); continue; }
        buf[len] = 0;
        cJSON *r = cJSON_Parse(buf);
        char svc[32] = "";
        if (r) js(r, "service", svc, sizeof svc);
        if (r && !strcmp(svc, "clownsec-bigtop")) {
            BtServer s; memset(&s, 0, sizeof s);
            js(r, "name", s.name, sizeof s.name); js(r, "id", s.id, sizeof s.id);
            js(r, "version", s.version, sizeof s.version);
            int port = (int)jn(r, "port"); if (port <= 0) port = 32500;
            snprintf(s.url, sizeof s.url, "http://%s:%d", inet_ntoa(from.sin_addr), port);
            int dup = 0;
            for (int i = 0; i < n; i++) if (!strcmp(out[i].id, s.id)) dup = 1;
            if (!dup) out[n++] = s;
        }
        cJSON_Delete(r);
    }
    closesocket(fd);
    return n;
}

int bt_libraries(const BtCfg *c, BtItem **out) {
    *out = NULL;
    char *body = NULL;
    long code = bt_req(c->url, c->client, "/api/libraries", &body, 0, 0);
    if (code != 200 || !body) { free(body); return -1; }
    cJSON *r = cJSON_Parse(body); free(body);
    if (!cJSON_IsArray(r)) { cJSON_Delete(r); snprintf(g_err, sizeof g_err, "Big Top sent something unreadable."); return -1; }
    int n = fill_array(r, out, "lib");
    /* the counts, for the row: movies in "episodes", shows in "seen" (fill read neither) */
    int i = 0; cJSON *e;
    cJSON_ArrayForEach(e, r) {
        if (i < n) {
            js(e, "name", (*out)[i].title, sizeof (*out)[i].title);   /* a library sends "name", not "title" */
            (*out)[i].episodes = (int)jn(e, "movies"); (*out)[i].seen = (int)jn(e, "shows");
        }
        i++;
    }
    cJSON_Delete(r);
    return n;
}

int bt_list(const BtCfg *c, const char *path, BtItem **out) {
    *out = NULL;
    char *body = NULL;
    long code = bt_req(c->url, c->client, path, &body, 0, 0);
    if (code != 200 || !body) { free(body); return -1; }
    cJSON *r = cJSON_Parse(body); free(body);
    if (!r) { snprintf(g_err, sizeof g_err, "Big Top sent something unreadable."); return -1; }
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(r, "items");
    if (!arr) arr = cJSON_GetObjectItemCaseSensitive(r, "seasons");   /* a show: its seasons */
    int n = arr ? fill_array(arr, out, NULL) : 0;
    cJSON_Delete(r);
    return n;
}

int bt_item(const BtCfg *c, const char *id, BtItem *out) {
    char path[96]; snprintf(path, sizeof path, "/api/items/%s", id);
    char *body = NULL;
    long code = bt_req(c->url, c->client, path, &body, 0, 1);
    int ok = 0;
    if (code == 200 && body) {
        cJSON *r = cJSON_Parse(body);
        if (r) { fill(out, r); ok = 1; }
        cJSON_Delete(r);
    }
    free(body);
    return ok;
}

void bt_poster_url(const BtCfg *c, const BtItem *it, int w, int h, char *out, int cap) {
    const char *kind = !strcmp(it->type, "show") ? "shows" : "items";
    char id[40]; snprintf(id, sizeof id, "%s", it->id);
    if (!strcmp(it->type, "season")) {                    /* "<show>/<n>": the show's poster */
        char *sl = strchr(id, '/'); if (sl) *sl = 0;
        kind = "shows";
    }
    snprintf(out, (size_t)cap, "%s/api/%s/%s/poster?w=%d&h=%d", c->url, kind, id, w, h);
}

void bt_progress(const BtCfg *c, const char *item_id, long long pos_ms, long long dur_ms, const char *state) {
    char path[160], *body = NULL;
    snprintf(path, sizeof path, "/api/items/%s/progress?pos=%lld&dur=%lld&state=%s", item_id, pos_ms, dur_ms, state);
    bt_req(c->url, c->client, path, &body, 1, 1);
    free(body);
}
void bt_watched(const BtCfg *c, const char *item_id, int on) {
    char path[128], *body = NULL;
    snprintf(path, sizeof path, "/api/items/%s/watched?on=%d", item_id, on ? 1 : 0);
    bt_req(c->url, c->client, path, &body, 1, 1);
    free(body);
}

/* ---- playback paths ---- */
static BtCfg g_play_cfg;                 /* the server the last path was made for */
static struct { char id[40]; long long size; } g_sizes[8];
static int g_sizes_next;

void bt_media_path(const BtCfg *c, const BtItem *it, const char *title, char *out, int cap) {
    g_play_cfg = *c;
    int known = 0;
    for (int i = 0; i < 8; i++) if (!strcmp(g_sizes[i].id, it->id)) { g_sizes[i].size = it->size; known = 1; }
    if (!known) {
        snprintf(g_sizes[g_sizes_next].id, sizeof g_sizes[0].id, "%s", it->id);
        g_sizes[g_sizes_next].size = it->size;
        g_sizes_next = (g_sizes_next + 1) % 8;
    }
    /* the title is the path's last part: the player shows it and keys this video's settings by it.
     * '/' would split it, so it goes. */
    char t[100]; snprintf(t, sizeof t, "%s", title && title[0] ? title : it->title);
    for (char *p = t; *p; p++) if (*p == '/' || *p == '\\' || *p == ':') *p = '-';
    snprintf(out, (size_t)cap, "bigtop:/%s/%s/%s.moflex", c->id[0] ? c->id : "server", it->id, t);
}

void bt_path_item(const char *path, char *id, int cap) {
    id[0] = 0;
    if (strncmp(path, "bigtop:/", 8)) return;
    const char *a = strchr(path + 8, '/'); if (!a) return;
    const char *b = strchr(a + 1, '/'); if (!b) return;
    snprintf(id, (size_t)cap, "%.*s", (int)(b - a - 1), a + 1);
}

FILE *bt_open_media(const char *path) {
    char id[40]; bt_path_item(path, id, sizeof id);
    if (!id[0]) return NULL;                         /* not ours: the card */
    if (!g_play_cfg.url[0]) bt_cfg_load(&g_play_cfg);
    long long size = -1;
    for (int i = 0; i < 8; i++) if (!strcmp(g_sizes[i].id, id) && g_sizes[i].size > 0) size = g_sizes[i].size;
    char url[256]; snprintf(url, sizeof url, "%s/api/items/%s/file", g_play_cfg.url, id);
    return httpfile_open(url, size, 0);
}
