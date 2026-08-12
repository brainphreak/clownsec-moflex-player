/* Background music: a folder of WAVs, shuffled, streamed straight to the DSP.
 *
 * Deliberately NOT mp3. An mp3 has to be decoded every frame on the same core that is drawing
 * the shop, and the shop is the thing we are trying to keep at sixty. 16-bit PCM is what the
 * DSP eats natively -- nothing decodes it, we only move bytes -- so the whole cost here is one
 * SD read every half second, on a thread of its own where a slow read cannot stall a frame.
 * The trade is disk: about 3.7 MB a minute at 32 kHz stereo. SD cards are large and frames
 * are not.
 *
 * Only two things are ever in flight: the buffer the DSP is playing and the one being filled.
 */
#include "music.h"
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#define MUS_MAX     64          /* tracks in the folder we will look at */
#define MUS_CHAN    7           /* a high channel: nothing else in here uses the DSP */
#define MUS_BUFS    3
/* ~0.5 s each at 32 kHz, so three of them is a second and a half of slack. Two quarter-second
 * buffers could not ride out the main thread sitting on the SD card to build a cover sheet,
 * and the track skipped. 196 KB of linear memory to never hear that again is a good trade. */
#define MUS_FRAMES  16384

static char        g_dir[256];
static char        g_name[MUS_MAX][128];
static int         g_n = 0;
static int         g_order[MUS_MAX];
static int         g_at = 0;            /* where we are in g_order */
static char        g_now[128];

static FILE       *g_f = NULL;
static long        g_left = 0;          /* bytes of sample data still unread */
static int         g_chan = 2, g_bits = 16;
static ndspWaveBuf g_wb[MUS_BUFS];
static s16        *g_buf[MUS_BUFS];
static int         g_ok = 0;            /* ndsp came up */
static int         g_playing = 0;
static volatile int g_run = 0, g_skip = 0;
static Thread      g_th = NULL;
static LightLock   g_lock;

/* ---- WAV: walk the chunks rather than assuming a 44-byte header, because plenty of
 * encoders write a LIST or a fact chunk in between and that shifts everything along. ---- */
static int wav_open(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char riff[12];
    if (fread(riff, 1, 12, f) != 12 || memcmp(riff, "RIFF", 4) || memcmp(riff + 8, "WAVE", 4)) {
        fclose(f); return 0;
    }
    int rate = 0, chan = 0, bits = 0, fmt_ok = 0;
    for (;;) {
        char id[4]; unsigned len;
        if (fread(id, 1, 4, f) != 4 || fread(&len, 4, 1, f) != 1) { fclose(f); return 0; }
        if (!memcmp(id, "fmt ", 4)) {
            unsigned short tag, ch, bps; unsigned sr;
            if (len < 16 || fread(&tag, 2, 1, f) != 1 || fread(&ch, 2, 1, f) != 1 ||
                fread(&sr, 4, 1, f) != 1) { fclose(f); return 0; }
            fseek(f, 6, SEEK_CUR);                       /* byte rate + block align */
            if (fread(&bps, 2, 1, f) != 1) { fclose(f); return 0; }
            if (len > 16) fseek(f, (long)len - 16, SEEK_CUR);
            if (tag != 1 || bps != 16 || ch < 1 || ch > 2) { fclose(f); return 0; }  /* PCM16 only */
            rate = (int)sr; chan = ch; bits = bps; fmt_ok = 1;
        } else if (!memcmp(id, "data", 4)) {
            if (!fmt_ok) { fclose(f); return 0; }
            g_f = f; g_left = (long)len; g_chan = chan; g_bits = bits;
            ndspChnReset(MUS_CHAN);
            ndspChnSetInterp(MUS_CHAN, NDSP_INTERP_LINEAR);
            ndspChnSetRate(MUS_CHAN, (float)rate);
            ndspChnSetFormat(MUS_CHAN, chan == 2 ? NDSP_FORMAT_STEREO_PCM16
                                                 : NDSP_FORMAT_MONO_PCM16);
            return 1;
        } else {
            if (fseek(f, (long)len + (len & 1), SEEK_CUR)) { fclose(f); return 0; }
        }
    }
}

static void wav_close(void) {
    if (g_f) { fclose(g_f); g_f = NULL; }
    g_left = 0;
}

/* Fill one buffer from the file and hand it to the DSP. Returns 0 at end of track. */
static int fill(int i) {
    if (!g_f || g_left <= 0) return 0;
    size_t want = (size_t)MUS_FRAMES * g_chan * 2;
    if ((long)want > g_left) want = (size_t)g_left;
    size_t got = fread(g_buf[i], 1, want, g_f);
    if (got < 2) return 0;
    g_left -= (long)got;
    memset(&g_wb[i], 0, sizeof g_wb[i]);
    g_wb[i].data_vaddr = g_buf[i];
    g_wb[i].nsamples   = (u32)(got / (g_chan * 2));
    DSP_FlushDataCache(g_buf[i], got);
    ndspChnWaveBufAdd(MUS_CHAN, &g_wb[i]);
    return 1;
}

static int start_track(int which) {
    wav_close();
    ndspChnWaveBufClear(MUS_CHAN);
    if (g_n <= 0) return 0;
    g_at = ((which % g_n) + g_n) % g_n;
    char path[420];
    snprintf(path, sizeof path, "%s/%s", g_dir, g_name[g_order[g_at]]);
    if (!wav_open(path)) return 0;
    LightLock_Lock(&g_lock);
    snprintf(g_now, sizeof g_now, "%s", g_name[g_order[g_at]]);
    LightLock_Unlock(&g_lock);
    for (int i = 0; i < MUS_BUFS; i++) { memset(&g_wb[i], 0, sizeof g_wb[i]); fill(i); }
    return 1;
}

/* The feeder. Everything that touches the card lives here so a slow read cannot stall a frame. */
static void mus_thread(void *arg) {
    (void)arg;
    while (g_run) {
        if (g_skip) {                       /* the jukebox was pressed */
            g_skip = 0;
            int tries = 0;
            while (tries++ < g_n && !start_track(g_at + 1)) { }
            continue;
        }
        int queued = 0, idle = 1;
        for (int i = 0; i < MUS_BUFS; i++) {
            if (g_wb[i].status == NDSP_WBUF_DONE || g_wb[i].status == NDSP_WBUF_FREE) {
                if (fill(i)) queued = 1;
            } else idle = 0;
        }
        if (!queued && idle) {              /* both buffers played out: on to the next track */
            int tries = 0;
            while (tries++ < g_n && !start_track(g_at + 1)) { }
        }
        svcSleepThread(40000000LL);         /* 40 ms; a buffer is 250 ms, so this is unhurried */
    }
}

int music_init(const char *dir) {
    snprintf(g_dir, sizeof g_dir, "%s", dir);
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) && g_n < MUS_MAX) {
        size_t L = strlen(e->d_name);
        if (L < 5 || strcasecmp(e->d_name + L - 4, ".wav")) continue;
        snprintf(g_name[g_n], sizeof g_name[0], "%s", e->d_name);
        g_order[g_n] = g_n;
        g_n++;
    }
    closedir(d);
    if (g_n <= 0) return 0;

    /* Shuffle, so it is not the same track every time the shop opens. osGetTime is the only
     * thing here that differs between runs. */
    unsigned seed = (unsigned)osGetTime() | 1u;
    for (int i = g_n - 1; i > 0; i--) {
        seed = seed * 1103515245u + 12345u;
        int j = (int)((seed >> 16) % (unsigned)(i + 1));
        int t = g_order[i]; g_order[i] = g_order[j]; g_order[j] = t;
    }

    if (ndspInit() != 0) return 0;          /* no dsp firm dumped -> the shop is just quiet */
    g_ok = 1;
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnSetMix(MUS_CHAN, (float[12]){ 0.7f, 0.7f });   /* under the room, not over it */
    for (int i = 0; i < MUS_BUFS; i++) {
        g_buf[i] = (s16 *)linearAlloc((size_t)MUS_FRAMES * 2 * 2);
        if (!g_buf[i]) { music_exit(); return 0; }
    }
    LightLock_Init(&g_lock);
    int tries = 0;
    while (tries++ < g_n && !start_track(0)) { }
    if (!g_f) { music_exit(); return 0; }

    g_run = 1;
    s32 prio = 0x30; svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    g_th = threadCreate(mus_thread, NULL, 16 * 1024, prio - 1, -2, false);
    if (!g_th) { g_run = 0; music_exit(); return 0; }
    g_playing = 1;
    return g_n;
}

void music_next(void) { if (g_playing) g_skip = 1; }

int music_count(void) { return g_n; }

const char *music_now(void) {
    static char out[128];
    if (!g_playing) return "";
    LightLock_Lock(&g_lock);
    snprintf(out, sizeof out, "%s", g_now);
    LightLock_Unlock(&g_lock);
    char *dot = strrchr(out, '.'); if (dot) *dot = 0;    /* drop the .wav */
    return out;
}

void music_exit(void) {
    if (g_run) { g_run = 0; if (g_th) { threadJoin(g_th, 2000000000LL); threadFree(g_th); g_th = NULL; } }
    if (g_ok) { ndspChnWaveBufClear(MUS_CHAN); ndspExit(); g_ok = 0; }
    wav_close();
    for (int i = 0; i < MUS_BUFS; i++) if (g_buf[i]) { linearFree(g_buf[i]); g_buf[i] = NULL; }
    g_playing = 0;
}
