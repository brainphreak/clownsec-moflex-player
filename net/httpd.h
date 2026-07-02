/* Lightweight HTTP file-transfer server for the 3DS.
 * Serves a browser UI to list / upload / delete / browse .moflex files over WiFi.
 * Runs on its own thread; mostly idle (blocked on accept) until a client connects. */
#ifndef MOFLEX_HTTPD_H
#define MOFLEX_HTTPD_H

#include <stdbool.h>

/* Start the server (soc init + listen thread). Returns false on failure. */
bool httpd_start(void);
/* Stop + clean up. */
void httpd_stop(void);
/* Human-readable "http://x.x.x.x:8080" (or "wifi off" if no IP). */
const char *httpd_url(void);

#endif
