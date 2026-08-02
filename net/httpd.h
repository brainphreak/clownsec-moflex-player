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

/* If an upload is currently being received, fills done/total (bytes) + the filename and
 * returns 1; otherwise returns 0. Lets the on-device UI show a progress bar. */
int httpd_upload_progress(long *done, long *total, char *name, int namecap);

/* Pop the path of an upload that just completed (1 = got one, 0 = none pending). The UI thread
 * drains this so a file sent from the browser lands in the library with its info, instead of
 * waiting for the user to notice it and rescan. */
int httpd_take_upload(char *path, int cap);

#endif
