/* A FILE* that reads a file over HTTP (Range requests), so code written for files on the SD card
 * -- the moflex demuxer, the super-moflex trailer reader -- plays from a server unchanged.
 *
 * Reading drains a download running ahead on a worker thread (HttpStream). A seek inside what has
 * already arrived just skips; any other seek restarts the download at the new byte. fread blocks
 * while data is on its way (like a slow card) and fails after a long stall. */
#ifndef HTTPFILE_H
#define HTTPFILE_H

#include <stdio.h>

/* size < 0: ask the server (one HEAD request). ring_bytes: read-ahead (0 = default 2 MB). */
FILE *httpfile_open(const char *url, long long size, size_t ring_bytes);

#endif
