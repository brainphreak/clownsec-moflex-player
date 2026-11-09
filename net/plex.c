/* Plex Media Server source -- see plex.h. */
#include "plex.h"
#include "downloader.h"
#include "cJSON.h"
#include <3ds.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PLEX_CFG "sdmc:/moflex_player/plex.cfg"
static char g_err[96] = "";
static int  g_unreach = 0;      /* the last failure was "no answer", not an error from the server */
static int  g_probe = 0;        /* next request is a quick reachability probe: short timeouts */
static int  g_once = 0;         /* next request is a whole listing in one reply: 90 s cap */
static int  g_slow = 0;         /* the last request was cut off part-way (too slow / too big) */
static void (*g_progress)(int, int) = NULL;
const char *plex_error(void) { return g_err; }
int  plex_unreachable(void) { return g_unreach; }
void plex_set_progress(void (*fn)(int, int)) { g_progress = fn; }

int plex_cfg_load(PlexCfg *c) {
    memset(c, 0, sizeof *c);
    FILE *f = fopen(PLEX_CFG, "rb");
    if (f) {
        char ln[200]; int i = 0;
        while (fgets(ln, sizeof ln, f) && i < 6) {
            char *e = strpbrk(ln, "\r\n"); if (e) *e = 0;
            if (i == 0) snprintf(c->url, sizeof c->url, "%s", ln);
            if (i == 1) snprintf(c->token, sizeof c->token, "%s", ln);
            if (i == 2) snprintf(c->client, sizeof c->client, "%s", ln);
            if (i == 3) snprintf(c->account, sizeof c->account, "%s", ln);
            if (i == 4) snprintf(c->name, sizeof c->name, "%s", ln);
            if (i == 5) snprintf(c->machine, sizeof c->machine, "%s", ln);
            i++;
        }
        fclose(f);
    }
    if (!c->client[0]) {                      /* one id per console, kept with the settings */
        u64 t = svcGetSystemTick();
        snprintf(c->client, sizeof c->client, "clownsec-3ds-%08lx%08lx",
                 (unsigned long)(t >> 32), (unsigned long)(t ^ (t >> 17)));
    }
    size_t L = strlen(c->url);
    while (L && c->url[L - 1] == '/') c->url[--L] = 0;
    return c->url[0] != 0;
}
void plex_cfg_save(const PlexCfg *c) {
    mkdir("sdmc:/moflex_player", 0777);
    FILE *f = fopen(PLEX_CFG, "wb"); if (!f) return;
    fprintf(f, "%s\n%s\n%s\n%s\n%s\n%s\n", c->url, c->token, c->client, c->account, c->name, c->machine);
    fclose(f);
}

/* the client headers every request carries (token only when one is set) */
static int plex_headers(const PlexCfg *c, char h[][96], const char **out, int json) {
    int n = 0;
    snprintf(h[n], 96, "X-Plex-Client-Identifier: %s", c->client); out[n] = h[n]; n++;
    snprintf(h[n], 96, "X-Plex-Product: Clownsec 3DS");            out[n] = h[n]; n++;
    snprintf(h[n], 96, "X-Plex-Version: 1.0");                     out[n] = h[n]; n++;
    snprintf(h[n], 96, "X-Plex-Platform: Generic");                out[n] = h[n]; n++;
    snprintf(h[n], 96, "X-Plex-Device: Nintendo 3DS");             out[n] = h[n]; n++;
    snprintf(h[n], 96, "X-Plex-Device-Name: Nintendo 3DS");        out[n] = h[n]; n++;
    if (c->token[0]) { snprintf(h[n], 96, "X-Plex-Token: %s", c->token); out[n] = h[n]; n++; }
    if (json) { snprintf(h[n], 96, "Accept: application/json"); out[n] = h[n]; n++; }
    out[n] = NULL;
    return n;
}

typedef struct { char *p; size_t n, cap; } Mem;
static size_t on_mem(char *p, size_t sz, size_t nm, void *u) {
    Mem *m = (Mem *)u; size_t k = sz * nm;
    if (m->n + k + 1 > m->cap) {
        if (m->n + k + 1 > (8u << 20)) return 0;            /* 8 MB: no listing is that big */
        size_t nc = m->cap ? m->cap : 65536; while (nc < m->n + k + 1) nc *= 2;
        char *g = (char *)realloc(m->p, nc); if (!g) return 0;
        m->p = g; m->cap = nc;
    }
    memcpy(m->p + m->n, p, k); m->n += k; m->p[m->n] = 0;
    return k;
}
/* GET (or POST) url with the Plex headers; body in *out (malloc'd). Returns the HTTP status, or -1. */
static long plex_req(const PlexCfg *c, const char *url, char **out, int json, int post) {   /* post: 1 POST, 2 PUT */
    *out = NULL;
    if (!downloader_init()) { snprintf(g_err, sizeof g_err, "Network is not available."); return -1; }
    CURL *e = curl_easy_init(); if (!e) return -1;
    char hb[10][96]; const char *hp[11]; plex_headers(c, hb, hp, json);
    struct curl_slist *hl = NULL;
    for (int i = 0; hp[i]; i++) hl = curl_slist_append(hl, hp[i]);
    Mem m = { 0 };
    curl_easy_setopt(e, CURLOPT_URL, url);
    curl_easy_setopt(e, CURLOPT_HTTPHEADER, hl);
    if (post == 1) curl_easy_setopt(e, CURLOPT_POSTFIELDS, "");
    if (post == 2) curl_easy_setopt(e, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(e, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(e, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
    if (g_probe) {                                       /* is anything there? answer fast */
        curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 3L);
        curl_easy_setopt(e, CURLOPT_TIMEOUT, 5L);
    } else {
        /* a remote server's upload can be slow but steady: give up when it stalls (under
         * 256 bytes/s for 20 s); a whole listing also stops at 90 s and is fetched in pages */
        curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(e, CURLOPT_LOW_SPEED_LIMIT, 256L);
        curl_easy_setopt(e, CURLOPT_LOW_SPEED_TIME, 20L);
        if (g_once) curl_easy_setopt(e, CURLOPT_TIMEOUT, 90L);
    }
    curl_easy_setopt(e, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, on_mem);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, &m);
    CURLcode rc = curl_easy_perform(e);
    long code = 0; curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hl); curl_easy_cleanup(e);
    g_unreach = 0; g_slow = 0;
    if (rc != CURLE_OK) {
        /* cut off part-way: the server IS there, the reply is just too slow or too big */
        g_slow = (rc == CURLE_OPERATION_TIMEDOUT && m.n > 0) || rc == CURLE_WRITE_ERROR;
        if (!g_slow) g_unreach = rc == CURLE_COULDNT_CONNECT || rc == CURLE_OPERATION_TIMEDOUT ||
                    rc == CURLE_COULDNT_RESOLVE_HOST || rc == CURLE_RECV_ERROR || rc == CURLE_SEND_ERROR ||
                    rc == CURLE_GOT_NOTHING;
        snprintf(g_err, sizeof g_err, "Can't reach the Plex server (%s).", curl_easy_strerror(rc));
        free(m.p); return -1;
    }
    if (code == 401) snprintf(g_err, sizeof g_err, "Plex refused access: check the token.");
    else if (code >= 400) snprintf(g_err, sizeof g_err, "Plex answered HTTP %ld.", code);
    *out = m.p;
    return code;
}

static long plex_get(const PlexCfg *c, const char *url, char **out, int json) {
    return plex_req(c, url, out, json, 0);
}

static void jstr(cJSON *o, const char *k, char *dst, size_t cap) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(v) && v->valuestring) snprintf(dst, cap, "%s", v->valuestring);
}
static long long jnum(cJSON *o, const char *k) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (long long)v->valuedouble : 0;
}

/* One page of a listing: append its entries to *v (grown as needed). Returns how many entries the
 * page held (titled or not, so paging can tell a short page), and the container's totalSize. */
static int plex_parse_page(cJSON *mc, int sections, PlexItem **pv, int *pn, int *pcap, int *total_size) {
    /* sections and folders come as Directory, playable items as Metadata */
    cJSON *arrs[2] = { cJSON_GetObjectItemCaseSensitive(mc, "Directory"),
                       cJSON_GetObjectItemCaseSensitive(mc, "Metadata") };
    int here = cJSON_GetArraySize(arrs[0]) + cJSON_GetArraySize(arrs[1]);
    *total_size = (int)jnum(mc, "totalSize");
    if (*pn + here > *pcap) {
        int nc = *pcap ? *pcap : 64; while (nc < *pn + here) nc *= 2;
        PlexItem *g = (PlexItem *)realloc(*pv, (size_t)nc * sizeof(PlexItem));
        if (!g) return -1;
        memset(g + *pcap, 0, (size_t)(nc - *pcap) * sizeof(PlexItem));
        *pv = g; *pcap = nc;
    }
    PlexItem *v = *pv; int n = *pn;
    for (int a = 0; a < 2; a++) {
        cJSON *e; cJSON_ArrayForEach(e, arrs[a]) {
            PlexItem *it = &v[n];
            jstr(e, "title", it->title, sizeof it->title);
            jstr(e, "type", it->type, sizeof it->type);
            jstr(e, "ratingKey", it->rating, sizeof it->rating);
            jstr(e, "thumb", it->thumb, sizeof it->thumb);
            jstr(e, "editionTitle", it->edition, sizeof it->edition);
            jstr(e, "summary", it->summary, sizeof it->summary);
            jstr(e, "contentRating", it->crating, sizeof it->crating);
            {   cJSON *g = cJSON_GetObjectItemCaseSensitive(e, "Genre"), *ge; int gl = 0;
                cJSON_ArrayForEach(ge, g) {
                    char t[32] = ""; jstr(ge, "tag", t, sizeof t);
                    if (t[0] && gl + (int)strlen(t) + 3 < (int)sizeof it->genres)
                        gl += snprintf(it->genres + gl, sizeof it->genres - (size_t)gl, "%s%s", gl ? ", " : "", t);
                } }
            /* portrait art: an episode's own thumb is a landscape still, so use its season's
             * (else the show's) poster */
            {   char pt[160] = "", gt[160] = "";
                jstr(e, "parentThumb", pt, sizeof pt); jstr(e, "grandparentThumb", gt, sizeof gt);
                char ty[12] = ""; jstr(e, "type", ty, sizeof ty);
                const char *pick = it->thumb;
                if (!strcmp(ty, "episode")) pick = pt[0] ? pt : gt[0] ? gt : it->thumb;
                snprintf(it->poster, sizeof it->poster, "%s", pick); }
            {   cJSON *md = cJSON_GetObjectItemCaseSensitive(e, "Media");
                cJSON *m0 = cJSON_GetArrayItem(md, 0);
                if (m0) { it->width = (int)jnum(m0, "width"); it->height = (int)jnum(m0, "height"); } }
            char key[160] = ""; jstr(e, "key", key, sizeof key);
            if (sections) {                    /* a library: list everything in it */
                snprintf(it->key, sizeof it->key, "/library/sections/%s/all", key);
                snprintf(it->type, sizeof it->type, "section");
            } else snprintf(it->key, sizeof it->key, "%s", key);
            it->index = (int)jnum(e, "index"); it->parent_index = (int)jnum(e, "parentIndex");
            it->year = (int)jnum(e, "year");
            it->leaf_count = (int)jnum(e, "leafCount"); it->viewed_leaf = (int)jnum(e, "viewedLeafCount");
            it->viewed = jnum(e, "viewCount") > 0;
            it->duration_ms = jnum(e, "duration"); it->offset_ms = jnum(e, "viewOffset");
            if (it->title[0]) n++;
        }
    }
    *pn = n;
    return here;
}

/* GET url and append its entries. Returns how many the reply held, or -1. */
static int plex_fetch(const PlexCfg *c, const char *url, int sections, PlexItem **v, int *n, int *cap, int *total) {
    char *body = NULL;
    long code = plex_get(c, url, &body, 1);
    if (code != 200 || !body) { free(body); return -1; }
    cJSON *root = cJSON_Parse(body); free(body);
    cJSON *mc = root ? cJSON_GetObjectItemCaseSensitive(root, "MediaContainer") : NULL;
    if (!mc) {
        cJSON_Delete(root);
        if (!root) g_slow = 1;                 /* a reply too big to parse: try it in pages */
        snprintf(g_err, sizeof g_err, "Unexpected reply from Plex."); return -1;
    }
    int here = plex_parse_page(mc, sections, v, n, cap, total);
    cJSON_Delete(root);
    if (here < 0) { g_slow = 1; snprintf(g_err, sizeof g_err, "Out of memory for this list."); }
    return here;
}

#define PLEX_PAGE 200
int plex_list(const PlexCfg *c, const char *path, PlexItem **items) {
    *items = NULL;
    int sections = !strcmp(path, "/library/sections");
    PlexItem *v = NULL; int n = 0, cap = 0, total = 0;
    /* the whole list in one reply -- fine on the home network, even for thousands of items */
    char url[640]; snprintf(url, sizeof url, "%s%s", c->url, path);
    g_once = 1; int r = plex_fetch(c, url, sections, &v, &n, &cap, &total); g_once = 0;
    if (r < 0 && g_slow) {
        /* cut off part-way (a big library through a remote server's home upload is several MB):
         * fetch it again a page at a time, each one small enough to arrive */
        free(v); v = NULL; n = cap = 0;
        for (int start = 0;;) {
            snprintf(url, sizeof url, "%s%s%cX-Plex-Container-Start=%d&X-Plex-Container-Size=%d",
                     c->url, path, strchr(path, '?') ? '&' : '?', start, PLEX_PAGE);
            r = plex_fetch(c, url, sections, &v, &n, &cap, &total);
            if (r < 0) break;
            start += r;
            /* done: a short page, or everything (a server that ignores paging sends no totalSize) */
            if (r < PLEX_PAGE || total <= 0 || start >= total) break;
            if (g_progress) g_progress(start, total);
        }
    }
    if (r < 0) { free(v); return -1; }
    if (!v) v = (PlexItem *)calloc(1, sizeof(PlexItem));
    if (!v) return -1;
    *items = v;
    return n;
}

static void urlenc(const char *s, char *o, size_t cap) {
    size_t n = 0;
    for (; *s && n + 4 < cap; s++) {
        unsigned char ch = (unsigned char)*s;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') o[n++] = (char)ch;
        else n += (size_t)snprintf(o + n, cap - n, "%%%02X", ch);
    }
    o[n] = 0;
}

HttpStream *plex_stream_open(const PlexCfg *c, const char *rating_key, int64_t offset_us,
                             int is3d, int burn_subs, char *session) {
    static unsigned seq = 0;
    u64 t = svcGetSystemTick();
    snprintf(session, 24, "c3ds%08lx%04x", (unsigned long)(t >> 8), (++seq) & 0xFFFF);
    int W = is3d ? 800 : 400, H = is3d ? 480 : 240;
    /* the profile that makes Plex answer with one MPEG-TS stream of H.264 + AAC at our size */
    char prof[800];
    snprintf(prof, sizeof prof,
             "add-transcode-target(type=videoProfile&context=streaming&protocol=http&container=mpegts"
             "&videoCodec=h264&audioCodec=aac)"
             "+add-limitation(scope=videoCodec&scopeName=h264&type=upperBound&name=video.width&value=%d)"
             "+add-limitation(scope=videoCodec&scopeName=h264&type=upperBound&name=video.height&value=%d)"
             /* stereo: a 5.1 source otherwise arrives as 6-channel AAC, which the 3DS plays as silence */
             "+add-limitation(scope=videoAudioCodec&scopeName=aac&type=upperBound&name=audio.channels&value=2)",
             W, H);
    char pe[1600]; urlenc(prof, pe, sizeof pe);
    char q[2048];
    snprintf(q, sizeof q,
             "path=%%2Flibrary%%2Fmetadata%%2F%s&mediaIndex=0&partIndex=0&protocol=http"
             "&offset=%lld&fastSeek=1&directPlay=0&directStream=0&videoResolution=%dx%d"
             "&maxVideoBitrate=%d&subtitles=%s&session=%s&X-Plex-Session-Identifier=%s"
             "&X-Plex-Client-Profile-Extra=%s",
             rating_key, (long long)(offset_us / 1000000), W, H, is3d ? 2000 : 700,
             burn_subs ? "burn" : "none", session, session, pe);
    char url[2400], *body = NULL;
    /* the decision first: newer servers expect it, and it says whether the item can be played */
    snprintf(url, sizeof url, "%s/video/:/transcode/universal/decision?%s", c->url, q);
    long code = plex_get(c, url, &body, 1);
    free(body);
    if (code != 200) return NULL;
    snprintf(url, sizeof url, "%s/video/:/transcode/universal/start.ts?%s", c->url, q);
    char hb[10][96]; const char *hp[11]; plex_headers(c, hb, hp, 0);
    HttpStream *s = hs_open(url, hp, 2u << 20);
    if (!s) snprintf(g_err, sizeof g_err, "Could not start the stream.");
    return s;
}
void plex_stream_stop(const PlexCfg *c, const char *session) {
    if (!session || !session[0]) return;
    char url[400], *body = NULL;
    snprintf(url, sizeof url, "%s/video/:/transcode/universal/stop?session=%s", c->url, session);
    plex_get(c, url, &body, 0); free(body);
}
void plex_timeline(const PlexCfg *c, const char *rating_key, const char *state,
                   long long pos_ms, long long dur_ms) {
    char url[400], *body = NULL;
    snprintf(url, sizeof url, "%s/:/timeline?ratingKey=%s&key=%%2Flibrary%%2Fmetadata%%2F%s&state=%s"
             "&time=%lld&duration=%lld", c->url, rating_key, rating_key, state, pos_ms, dur_ms);
    plex_get(c, url, &body, 0); free(body);
}

/* ---- plex.tv linking + server discovery ---- */
int plex_pin_start(const PlexCfg *c, long long *id, char code[16]) {
    PlexCfg anon = *c; anon.token[0] = 0;     /* a new link must not ride an old token */
    char *body = NULL;
    long st = plex_req(&anon, "https://plex.tv/api/v2/pins?strong=false", &body, 1, 1);
    if (st < 200 || st >= 300 || !body) {
        if (st > 0) snprintf(g_err, sizeof g_err, "plex.tv answered HTTP %ld.", st);
        free(body); return 0;
    }
    cJSON *r = cJSON_Parse(body); free(body);
    *id = r ? jnum(r, "id") : 0; code[0] = 0;
    if (r) jstr(r, "code", code, 16);
    cJSON_Delete(r);
    if (!*id || !code[0]) { snprintf(g_err, sizeof g_err, "Unexpected reply from plex.tv."); return 0; }
    return 1;
}
int plex_pin_poll(const PlexCfg *c, long long id, char *token, int cap) {
    PlexCfg anon = *c; anon.token[0] = 0;
    char url[96], *body = NULL;
    snprintf(url, sizeof url, "https://plex.tv/api/v2/pins/%lld", id);
    long st = plex_req(&anon, url, &body, 1, 0);
    if (st == 404) { free(body); snprintf(g_err, sizeof g_err, "The code expired."); return -1; }
    if (st != 200 || !body) { free(body); return 0; }   /* network hiccup: keep polling */
    cJSON *r = cJSON_Parse(body); free(body);
    token[0] = 0;
    if (r) jstr(r, "authToken", token, (size_t)cap);
    cJSON_Delete(r);
    return token[0] ? 1 : 0;
}
/* Does THIS Plex server (machine id; "" = any) answer at url? Quick: a few seconds at most. A
 * home-network address can belong to some other device -- or another Plex server -- on whatever
 * network the 3DS is on now, so the id it answers with has to match. */
static int plex_answers(const PlexCfg *c, const char *url, const char *token, const char *machine) {
    PlexCfg t = *c; snprintf(t.token, sizeof t.token, "%s", token);
    char u[200], *body = NULL; snprintf(u, sizeof u, "%s/identity", url);
    g_probe = 1; long st = plex_get(&t, u, &body, 1); g_probe = 0;
    int ok = st == 200 && body;
    if (ok && machine && machine[0]) {
        cJSON *r = cJSON_Parse(body);
        cJSON *mc = r ? cJSON_GetObjectItemCaseSensitive(r, "MediaContainer") : NULL;
        char id[48] = ""; if (mc) jstr(mc, "machineIdentifier", id, sizeof id);
        ok = !strcmp(id, machine);
        cJSON_Delete(r);
    }
    free(body);
    return ok;
}
int plex_servers(const PlexCfg *c, PlexServer *out, int max) {
    PlexCfg acct = *c;                        /* plex.tv wants the ACCOUNT token */
    if (c->account[0]) snprintf(acct.token, sizeof acct.token, "%s", c->account);
    char *body = NULL;
    long st = plex_get(&acct, "https://plex.tv/api/v2/resources?includeHttps=0&includeRelay=0", &body, 1);
    if (st != 200 || !body) { free(body); return -1; }
    cJSON *r = cJSON_Parse(body); free(body);
    int n = 0;
    for (int pass = 0; pass < 2 && n < max; pass++) {    /* owned servers first, then shared ones */
        cJSON *e; cJSON_ArrayForEach(e, r) {
            if (n >= max) break;
            char prov[64] = ""; jstr(e, "provides", prov, sizeof prov);
            if (!strstr(prov, "server")) continue;
            int owned = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(e, "owned"));
            if ((pass == 0) != owned) continue;
            PlexServer *s = &out[n]; memset(s, 0, sizeof *s);
            jstr(e, "name", s->name, sizeof s->name);
            jstr(e, "clientIdentifier", s->machine, sizeof s->machine);
            jstr(e, "accessToken", s->token, sizeof s->token);
            if (!s->token[0]) snprintf(s->token, sizeof s->token, "%s", acct.token);
            s->owned = owned;
            /* its addresses: the home network's first (fast, and the only ones that work when the
             * router can't loop back), then the internet ones remote access publishes */
            cJSON *cs = cJSON_GetObjectItemCaseSensitive(e, "connections");
            for (int want_local = 1; want_local >= 0 && !s->url[0]; want_local--) {
                cJSON *cn; cJSON_ArrayForEach(cn, cs) {
                    char proto[8] = "", addr[64] = "";
                    jstr(cn, "protocol", proto, sizeof proto); jstr(cn, "address", addr, sizeof addr);
                    int local = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cn, "local"));
                    int relay = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cn, "relay"));
                    if (local != want_local || relay || strcmp(proto, "http") || !addr[0] || strchr(addr, ':'))
                        continue;                 /* (IPv6 addresses skipped) */
                    char url[128]; snprintf(url, sizeof url, "http://%s:%lld", addr, jnum(cn, "port"));
                    if (plex_answers(c, url, s->token, s->machine)) { snprintf(s->url, sizeof s->url, "%s", url); break; }
                }
            }
            if (s->url[0]) n++;                   /* answered somewhere: list it */
        }
    }
    cJSON_Delete(r);
    return n;
}

int plex_relocate(PlexCfg *c) {
    if (!c->account[0] || (!c->machine[0] && !c->name[0])) return 0;   /* a typed address: leave it */
    char err[96]; snprintf(err, sizeof err, "%s", g_err);
    PlexServer sv[8]; int n = plex_servers(c, sv, 8);
    for (int i = 0; i < n; i++) {
        /* the same server: by its id, or (set up before ids were kept) by its name */
        int same = c->machine[0] ? !strcmp(sv[i].machine, c->machine) : !strcmp(sv[i].name, c->name);
        if (!same) continue;
        snprintf(c->url, sizeof c->url, "%s", sv[i].url);
        snprintf(c->token, sizeof c->token, "%s", sv[i].token);
        snprintf(c->machine, sizeof c->machine, "%s", sv[i].machine);
        plex_cfg_save(c);
        return 1;
    }
    snprintf(g_err, sizeof g_err, "%s", err);   /* keep the original reason for the screen */
    return 0;
}

int plex_token_is_account(const PlexCfg *c, const char *token) {
    if (!token || !token[0]) return 0;
    PlexCfg t = *c; snprintf(t.token, sizeof t.token, "%s", token);
    char *body = NULL;
    long st = plex_get(&t, "https://plex.tv/api/v2/user", &body, 1);
    free(body);
    return st == 200;
}

void plex_art_url(const PlexCfg *c, const char *thumb, int w, int h, char *out, int cap) {
    char enc[400]; urlenc(thumb, enc, sizeof enc);
    int n = snprintf(out, (size_t)cap, "%s/photo/:/transcode?width=%d&height=%d&minSize=1&upscale=1&url=%s",
                     c->url, w, h, enc);
    if (c->token[0] && n > 0 && n < cap) snprintf(out + n, (size_t)(cap - n), "&X-Plex-Token=%s", c->token);
}

int plex_item_streams(const PlexCfg *c, const char *rating_key, PlexSub *subs, int max,
                      long long *part_id, char *audio_lang, int alcap) {
    char url[256], *body = NULL;
    snprintf(url, sizeof url, "%s/library/metadata/%s", c->url, rating_key);
    *part_id = 0; if (alcap) audio_lang[0] = 0;
    if (plex_get(c, url, &body, 1) != 200 || !body) { free(body); return -1; }
    cJSON *r = cJSON_Parse(body); free(body);
    cJSON *mc = r ? cJSON_GetObjectItemCaseSensitive(r, "MediaContainer") : NULL;
    cJSON *md = mc ? cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(mc, "Metadata"), 0) : NULL;
    cJSON *me = md ? cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(md, "Media"), 0) : NULL;
    cJSON *pt = me ? cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(me, "Part"), 0) : NULL;
    int n = 0, have_audio = 0;
    if (pt) {
        *part_id = jnum(pt, "id");
        cJSON *st; cJSON_ArrayForEach(st, cJSON_GetObjectItemCaseSensitive(pt, "Stream")) {
            int type = (int)jnum(st, "streamType");
            int sel = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(st, "selected"));
            if (type == 2 && alcap && (sel || !have_audio)) {     /* the audio that will play */
                jstr(st, "languageCode", audio_lang, (size_t)alcap); have_audio = 1;
            }
            if (type != 3 || n >= max) continue;
            PlexSub *s = &subs[n++]; memset(s, 0, sizeof *s);
            s->id = jnum(st, "id");
            jstr(st, "codec", s->codec, sizeof s->codec);
            jstr(st, "languageCode", s->lang, sizeof s->lang);
            jstr(st, "displayTitle", s->title, sizeof s->title);
            jstr(st, "key", s->key, sizeof s->key);
            s->forced = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(st, "forced")) || jnum(st, "forced") == 1;
            s->selected = sel;
        }
    }
    cJSON_Delete(r);
    return n;
}
int plex_select_sub(const PlexCfg *c, long long part_id, long long sub_id) {
    if (!part_id) return 0;
    char url[256], *body = NULL;
    snprintf(url, sizeof url, "%s/library/parts/%lld?subtitleStreamID=%lld&allParts=1", c->url, part_id, sub_id);
    long st = plex_req(c, url, &body, 0, 2);
    free(body);
    return st == 200;
}
int plex_download(const PlexCfg *c, const char *path, const char *dest) {
    char url[400], *body = NULL;
    snprintf(url, sizeof url, "%s%s", c->url, path);
    long st = plex_get(c, url, &body, 0);
    if (st != 200 || !body) { free(body); return 0; }
    FILE *f = fopen(dest, "wb");
    if (!f) { free(body); return 0; }
    fwrite(body, 1, strlen(body), f); fclose(f); free(body);
    return 1;
}
