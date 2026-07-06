#include "httpd.h"

#include <3ds.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#define SOC_ALIGN   0x1000
#define SOC_BUFSZ   0x100000
#define PORT        8080
#define HDRBUF      8192
#define IOCHUNK     16384

static u32 *g_soc_buf;
static volatile int g_listen_fd = -1;
static volatile bool g_stop;
static Thread g_thread;
static char g_url[64] = "wifi off";

/* ---------- helpers ---------- */

static void url_decode(char *s) {
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '%' && p[1] && p[2]) {
            int hi = p[1], lo = p[2];
            hi = (hi <= '9') ? hi - '0' : (hi | 0x20) - 'a' + 10;
            lo = (lo <= '9') ? lo - '0' : (lo | 0x20) - 'a' + 10;
            *o++ = (char)((hi << 4) | lo);
            p += 2;
        } else if (*p == '+') {
            *o++ = ' ';
        } else {
            *o++ = *p;
        }
    }
    *o = 0;
}

static void html_escape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const char *p = in; *p && o + 6 < cap; p++) {
        switch (*p) {
            case '&': memcpy(out + o, "&amp;", 5); o += 5; break;
            case '<': memcpy(out + o, "&lt;", 4);  o += 4; break;
            case '>': memcpy(out + o, "&gt;", 4);  o += 4; break;
            case '"': memcpy(out + o, "&quot;", 6);o += 6; break;
            default:  out[o++] = *p;
        }
    }
    out[o] = 0;
}

static void send_all(int fd, const char *buf, int len) {
    int off = 0;
    while (off < len) {
        int n = send(fd, buf + off, len - off, 0);
        if (n <= 0) break;
        off += n;
    }
}

static void send_str(int fd, const char *s) { send_all(fd, s, (int)strlen(s)); }

static void send_status(int fd, const char *status, const char *ctype, const char *body) {
    char hdr[256];
    int bl = body ? (int)strlen(body) : 0;
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
             "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n",
             status, ctype, bl);
    send_str(fd, hdr);
    if (body) send_all(fd, body, bl);
}

/* ---------- directory listing page ---------- */

static void get_query(const char *path, const char *key, char *out, size_t cap) {
    out[0] = 0;
    const char *q = strchr(path, '?');
    if (!q) return;
    char keyeq[32];
    snprintf(keyeq, sizeof(keyeq), "%s=", key);
    const char *p = strstr(q, keyeq);
    if (!p) return;
    p += strlen(keyeq);
    size_t i = 0;
    while (*p && *p != '&' && i + 1 < cap) out[i++] = *p++;
    out[i] = 0;
    url_decode(out);
}

static int is_moflex(const char *n) {
    size_t L = strlen(n);
    return L > 7 && strcasecmp(n + L - 7, ".moflex") == 0;
}

static void serve_listing(int fd, const char *dir) {
    /* stream the page directly to the socket in chunks */
    char line[2048], esc[1024], enc[1024];

    send_str(fd,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n");

    send_str(fd,
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>MoFlex Transfer</title>"
        "<style>body{font-family:sans-serif;max-width:760px;margin:1em auto;padding:0 1em}"
        "a{text-decoration:none}li{margin:.3em 0}.d{font-weight:bold}"
        "button{cursor:pointer}#log{color:#666;white-space:pre}</style>"
        "<h2>MoFlex Transfer</h2>");

    snprintf(line, sizeof(line), "<p>Folder: <code>%s</code></p>", dir);
    send_str(fd, line);

    /* upload + new-folder widgets */
    send_str(fd,
        "<p><input type=file id=f multiple> "
        "<button onclick=up()>Upload here</button></p>"
        "<p><input id=nf placeholder='new folder name'> "
        "<button onclick=mkd()>Create folder</button></p>"
        "<div id=log></div><ul>");

    /* parent link */
    if (strcmp(dir, "sdmc:/") != 0) {
        char up[1024];
        snprintf(up, sizeof(up), "%s", dir);
        size_t L = strlen(up);
        if (L && up[L-1] == '/') up[--L] = 0;
        char *sl = strrchr(up, '/');
        if (sl) sl[1] = 0;
        html_escape(up, esc, sizeof(esc));
        snprintf(line, sizeof(line), "<li class=d><a href='/?dir=%s'>.. (up)</a></li>", esc);
        send_str(fd, line);
    }

    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char full[1200];
            snprintf(full, sizeof(full), "%s%s", dir, e->d_name);
            struct stat st;
            int isdir = (stat(full, &st) == 0) && S_ISDIR(st.st_mode);
            (void)is_moflex;   /* transfer tool: list ALL files (upload/manage anything, not just movies) */
            html_escape(e->d_name, esc, sizeof(esc));
            if (isdir) {
                char sub[1200]; snprintf(sub, sizeof(sub), "%s%s/", dir, e->d_name);
                char subesc[1024]; html_escape(sub, subesc, sizeof(subesc));
                snprintf(line, sizeof(line),
                    "<li class=d>&#128193; <a href='/?dir=%s'>%s/</a></li>", subesc, esc);
            } else {
                char fullesc[1024]; html_escape(full, fullesc, sizeof(fullesc));
                long kb = (long)(st.st_size / 1024);
                snprintf(line, sizeof(line),
                    "<li>&#127909; %s <small>(%ld KB)</small> "
                    "<button onclick=\"rm('%s')\">delete</button></li>",
                    esc, kb, fullesc);
            }
            send_str(fd, line);
            (void)enc;
        }
        closedir(d);
    }

    send_str(fd, "</ul>");

    /* JS: PUT uploads with a real progress bar (XHR upload.onprogress) + delete. */
    {
        char dq[2200];
        snprintf(dq, sizeof(dq),
            "<script>const DIR=\"%s\";"
            "function up(){const fs=document.getElementById('f').files;if(!fs.length)return;"
            "let i=0;const log=document.getElementById('log');"
            "(function nx(){if(i>=fs.length){location.reload();return;}"
            "const f=fs[i],x=new XMLHttpRequest();"
            "x.open('PUT','/up?path='+encodeURIComponent(DIR+f.name));"
            "x.upload.onprogress=function(e){var p=e.lengthComputable?Math.floor(e.loaded*100/e.total):0;"
            "log.innerHTML='Uploading '+(i+1)+'/'+fs.length+': '+f.name+"
            "'<br><progress value='+e.loaded+' max='+e.total+'></progress> '+p+'%%'+"
            "' ('+Math.floor(e.loaded/1048576)+'/'+Math.floor(e.total/1048576)+' MB)';};"
            "x.onload=function(){log.textContent='Saved '+f.name;i++;nx();};"
            "x.onerror=function(){log.textContent='Error uploading '+f.name;};"
            "x.send(f);})();}"
            "function rm(p){if(!confirm('Delete '+p+'?'))return;"
            "fetch('/rm?path='+encodeURIComponent(p),{method:'GET'}).then(r=>location.reload());}"
            "function mkd(){var n=document.getElementById('nf').value;if(!n)return;"
            "fetch('/mkdir?path='+encodeURIComponent(DIR+n),{method:'GET'}).then(r=>location.reload());}"
            "</script>", dir);
        send_str(fd, dq);
    }
}

/* ---------- PUT upload ---------- */

/* live upload progress, polled by the on-device UI (the web page has its own bar) */
static volatile long g_up_done = 0, g_up_total = 0;
static volatile int  g_up_active = 0;
static char g_up_name[128];
int httpd_upload_progress(long *done, long *total, char *name, int namecap) {
    if (!g_up_active) return 0;
    if (done)  *done  = g_up_done;
    if (total) *total = g_up_total;
    if (name && namecap) snprintf(name, namecap, "%s", g_up_name);
    return 1;
}

static void handle_put(int fd, const char *path, char *hdrbuf, int hdr_total, int body_start) {
    char fpath[1024];
    get_query(path, "path", fpath, sizeof(fpath));
    if (fpath[0] == 0 || strncmp(fpath, "sdmc:/", 6) != 0) {
        send_status(fd, "400 Bad Request", "text/plain", "bad path");
        return;
    }

    long content_len = -1;
    /* case-insensitive search for Content-Length */
    for (char *p = hdrbuf; *p; p++) {
        if (!strncasecmp(p, "content-length:", 15)) {
            content_len = atol(p + 15);
            break;
        }
    }
    if (content_len < 0) { send_status(fd, "411 Length Required", "text/plain", "need length"); return; }

    FILE *out = fopen(fpath, "wb");
    if (!out) { send_status(fd, "500 Error", "text/plain", "open failed"); return; }

    { const char *b = strrchr(fpath, '/'); b = b ? b + 1 : fpath; snprintf(g_up_name, sizeof g_up_name, "%s", b); }
    g_up_total = content_len; g_up_done = 0; g_up_active = 1;   /* -> on-device progress bar */

    long remaining = content_len;
    /* bytes already read past the header */
    int have = hdr_total - body_start;
    if (have > 0) {
        int w = (int)((have < remaining) ? have : remaining);
        fwrite(hdrbuf + body_start, 1, w, out);
        remaining -= w; g_up_done += w;
    }

    char *buf = (char *)malloc(IOCHUNK);
    while (remaining > 0 && buf) {
        int want = (int)(remaining < IOCHUNK ? remaining : IOCHUNK);
        int n = recv(fd, buf, want, 0);
        if (n <= 0) break;
        fwrite(buf, 1, n, out);
        remaining -= n; g_up_done += n;
    }
    free(buf);
    fclose(out);
    g_up_active = 0;

    if (remaining == 0) send_status(fd, "200 OK", "text/plain", "ok");
    else                send_status(fd, "500 Error", "text/plain", "incomplete");
}

/* ---------- request dispatch ---------- */

static void handle_client(int fd) {
    char *hdr = (char *)malloc(HDRBUF);
    if (!hdr) return;

    /* read until end of headers */
    int total = 0, body_start = -1;
    while (total < HDRBUF - 1) {
        int n = recv(fd, hdr + total, HDRBUF - 1 - total, 0);
        if (n <= 0) break;
        total += n;
        hdr[total] = 0;
        char *end = strstr(hdr, "\r\n\r\n");
        if (end) { body_start = (int)(end - hdr) + 4; break; }
    }
    if (body_start < 0) { free(hdr); return; }

    char method[8] = {0}, path[1024] = {0};
    sscanf(hdr, "%7s %1023s", method, path);

    if (!strcmp(method, "GET")) {
        if (!strncmp(path, "/rm", 3)) {
            char fpath[1024];
            get_query(path, "path", fpath, sizeof(fpath));
            if (fpath[0] && !strncmp(fpath, "sdmc:/", 6)) remove(fpath);
            send_status(fd, "200 OK", "text/plain", "ok");
        } else if (!strncmp(path, "/mkdir", 6)) {
            char fpath[1024];
            get_query(path, "path", fpath, sizeof(fpath));
            if (fpath[0] && !strncmp(fpath, "sdmc:/", 6)) mkdir(fpath, 0777);
            send_status(fd, "200 OK", "text/plain", "ok");
        } else {
            char dir[1024];
            get_query(path, "dir", dir, sizeof(dir));
            if (dir[0] == 0) strcpy(dir, "sdmc:/");
            size_t L = strlen(dir);
            if (L == 0 || dir[L-1] != '/') { if (L+1 < sizeof(dir)) { dir[L]='/'; dir[L+1]=0; } }
            serve_listing(fd, dir);
        }
    } else if (!strcmp(method, "PUT")) {
        handle_put(fd, path, hdr, total, body_start);
    } else {
        send_status(fd, "405 Method Not Allowed", "text/plain", "no");
    }
    free(hdr);
}

static void server_thread(void *arg) {
    (void)arg;
    while (!g_stop) {
        struct sockaddr_in cli;
        socklen_t cl = sizeof(cli);
        int c = accept(g_listen_fd, (struct sockaddr *)&cli, &cl);
        if (c < 0) {
            if (g_stop) break;
            svcSleepThread(50000000);   /* 50ms; listen sock is non-blocking */
            continue;
        }
        /* client inherits the listen sock's O_NONBLOCK -> force it blocking so
           recv() waits for data during large uploads (else they fail partway) */
        fcntl(c, F_SETFL, fcntl(c, F_GETFL, 0) & ~O_NONBLOCK);
        handle_client(c);
        closesocket(c);
    }
}

/* ---------- lifecycle ---------- */

/* soc is initialized once by the app (net_soc_init in main); shared with curl. */
bool httpd_start(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(PORT);
    sa.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(fd, 4) != 0) {
        closesocket(fd); return false;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);  /* clean shutdown via poll */
    g_listen_fd = fd;
    g_stop = false;

    struct in_addr in;
    in.s_addr = gethostid();
    snprintf(g_url, sizeof(g_url), "http://%s:%d", inet_ntoa(in), PORT);

    /* server thread: freeing syscore time lets it run on core 1 (old+new 3DS) */
    APT_SetAppCpuTimeLimit(30);
    g_thread = threadCreate(server_thread, NULL, 16 * 1024, 0x3F, 1, false);
    if (!g_thread) { closesocket(fd); g_listen_fd = -1; return false; }
    return true;
}

void httpd_stop(void) {
    g_stop = true;
    if (g_listen_fd >= 0) { closesocket(g_listen_fd); g_listen_fd = -1; }
    if (g_thread) { threadJoin(g_thread, 2000000000ULL); threadFree(g_thread); g_thread = NULL; }
    strcpy(g_url, "wifi off");
}

const char *httpd_url(void) { return g_url; }
