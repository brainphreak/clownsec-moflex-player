/* A live moflex stream (Big Top transcoding an MP4/MKV to moflex as it plays) read as an ordinary
 * FILE*. The body arrives through httpstream; the last LIVE_WINDOW bytes are kept, so the demuxer's
 * short backward jumps (2D/3D probe rewind, seek landing, audio-trim rewind) work. A seek to another
 * time reopens the stream there: the server restarts its encoder at that time. */
#ifndef LIVEMOFLEX_H
#define LIVEMOFLEX_H

#include <stdint.h>
#include <stdio.h>

typedef struct LiveMoflex LiveMoflex;

/* Open url (the server adds the start time from "at=<ms>") as a stream starting at t_us. *fp gets
 * the FILE* to hand to mfx_open_live (closing it with fclose closes the stream). NULL on failure. */
LiveMoflex *live_open(const char *url, int64_t t_us, FILE **fp);
/* the same, for a title lasting dur_us (0 = unknown): mfx_open on the FILE then needs nothing else
 * (live_install_hook) */
LiveMoflex *live_open_dur(const char *url, int64_t t_us, int64_t dur_us, FILE **fp);
/* make mfx_open recognise live FILEs (once, at start-up) */
void live_install_hook(void);
/* mfx_open_live's seek callback: the stream positioned at its start for time t_us -- rewinds when it
 * already starts there and its start is still buffered, else reopens. Returns the stream's start
 * time, or -1. */
int64_t live_seek(void *lm, int64_t t_us);
/* stats for logs: bytes received, longest wait for data (ms), reopens */
void live_stats(LiveMoflex *lm, unsigned long long *bytes, unsigned *wait_max_ms, unsigned *reopens);

/* for an error message: "http=<code> curl=<rc> done=<state> got=<bytes>" */
void live_status(LiveMoflex *lm, char *buf, int n);

#endif
