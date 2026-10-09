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

static char        g_path[MUS_MAX][300];  /* full path: tracks come from the app (romfs) AND the card */
static char        g_name[MUS_MAX][128];
static int         g_n = 0;
static int         g_order[MUS_MAX];
static int         g_at = 0;            /* where we are in g_order */
static char        g_now[128];

static FILE       *g_f = NULL;
static long        g_left = 0;          /* bytes of sample data still unread */
static int         g_chan = 2, g_bits = 16;
/* IMA ADPCM (WAV format 0x11): four bits a sample, a quarter of PCM16. The song that ships
 * inside the app is stored this way -- 4.4 MB instead of 23 -- and decoding it is a few adds a
 * sample on this thread, nothing like an mp3. */
static int         g_ima = 0, g_block = 0, g_spb = 0;   /* is ADPCM; bytes and frames a block */
static u8         *g_stage = NULL;                      /* raw blocks before decoding */
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
            unsigned short balign;
            fseek(f, 4, SEEK_CUR);                       /* byte rate */
            if (fread(&balign, 2, 1, f) != 1 || fread(&bps, 2, 1, f) != 1) { fclose(f); return 0; }
            if (len > 16) fseek(f, (long)len - 16, SEEK_CUR);
            if (ch < 1 || ch > 2) { fclose(f); return 0; }
            if (tag == 1 && bps == 16) { g_ima = 0; }
            else if (tag == 0x11 && bps == 4 && balign > 4u * ch && balign <= 4096) {
                g_ima = 1; g_block = balign;
                g_spb = (balign - 4 * ch) * 2 / ch + 1;  /* the header holds one sample per channel */
            } else { fclose(f); return 0; }              /* PCM16 or IMA ADPCM only */
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

static const short ima_step[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,
    143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,
    1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,
    7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767 };
static const signed char ima_adj[16] = { -1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8 };
static inline s16 ima_nib(int n, int *pred, int *idx) {
    /* (2*delta+1)*step/8, the exact form -- what ffmpeg's encoder predicted with. The bit-by-bit
     * version in the original IMA paper rounds differently and drifts a little off every sample. */
    int step = ima_step[*idx], d = ((2 * (n & 7) + 1) * step) >> 3;
    int p = (n & 8) ? *pred - d : *pred + d;
    if (p > 32767) p = 32767; else if (p < -32768) p = -32768;
    *pred = p;
    int x = *idx + ima_adj[n]; *idx = x < 0 ? 0 : (x > 88 ? 88 : x);
    return (s16)p;
}
/* One Microsoft-IMA block -> interleaved PCM16. Per channel a 4-byte header (first sample, step
 * index), then 4 bytes = 8 samples of each channel in turn, low nibble first. */
static int ima_block(const u8 *in, int len, int ch, s16 *out) {
    int pred[2], idx[2];
    for (int c = 0; c < ch; c++) {
        pred[c] = (s16)(in[4 * c] | (in[4 * c + 1] << 8));
        idx[c] = in[4 * c + 2] > 88 ? 88 : in[4 * c + 2];
        out[c] = (s16)pred[c];
    }
    int f = 1;
    for (const u8 *p = in + 4 * ch; p + 4 * ch <= in + len; p += 4 * ch, f += 8)
        for (int c = 0; c < ch; c++)
            for (int b = 0; b < 4; b++) {
                int byte = p[c * 4 + b];
                out[(f + b * 2)     * ch + c] = ima_nib(byte & 15, &pred[c], &idx[c]);
                out[(f + b * 2 + 1) * ch + c] = ima_nib(byte >> 4, &pred[c], &idx[c]);
            }
    return f;
}

/* Fill one buffer from the file and hand it to the DSP. Returns 0 at end of track. */
static int fill(int i) {
    if (!g_f || g_left <= 0) return 0;
    if (g_ima) {
        int nb = MUS_FRAMES / g_spb;                     /* whole blocks that fit the buffer */
        long want = (long)nb * g_block;
        if (want > g_left) want = g_left;
        size_t got = fread(g_stage, 1, (size_t)want, g_f);
        if (got < (size_t)(4 * g_chan)) return 0;
        g_left -= (long)got;
        int frames = 0;
        for (size_t off = 0; off + 4u * g_chan < got; off += g_block) {
            int blen = (got - off < (size_t)g_block) ? (int)(got - off) : g_block;
            frames += ima_block(g_stage + off, blen, g_chan, g_buf[i] + (size_t)frames * g_chan);
        }
        if (frames <= 0) return 0;
        memset(&g_wb[i], 0, sizeof g_wb[i]);
        g_wb[i].data_vaddr = g_buf[i];
        g_wb[i].nsamples   = (u32)frames;
        DSP_FlushDataCache(g_buf[i], (size_t)frames * g_chan * 2);
        ndspChnWaveBufAdd(MUS_CHAN, &g_wb[i]);
        return 1;
    }
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
    if (!wav_open(g_path[g_order[g_at]])) return 0;
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

static void add_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && g_n < MUS_MAX) {
        size_t L = strlen(e->d_name);
        if (L < 5 || strcasecmp(e->d_name + L - 4, ".wav")) continue;
        int dup = 0;                                  /* the shipped song also copied to the card */
        for (int k = 0; k < g_n && !dup; k++) if (!strcasecmp(g_name[k], e->d_name)) dup = 1;
        if (dup) continue;
        snprintf(g_name[g_n], sizeof g_name[0], "%s", e->d_name);
        snprintf(g_path[g_n], sizeof g_path[0], "%s/%s", dir, e->d_name);
        g_order[g_n] = g_n;
        g_n++;
    }
    closedir(d);
}

int music_init(const char *dir) {
    g_n = 0; g_at = 0; g_playing = 0; g_skip = 0; g_now[0] = 0;   /* entered again: start over */
    add_dir("romfs:/music");                          /* what ships with the app */
    add_dir(dir);                                     /* and whatever the owner put on the card */
    if (g_n <= 0) return 0;

    /* Shuffle, so it is not the same track every time the shop opens. osGetTime is the only
     * thing here that differs between runs. */
    /* The clock in ms alone barely moves the seed between visits, and the first pick of this
     * LCG barely moves with the seed: the same opening song kept coming back. Mix in the CPU tick
     * (sub-microsecond, different every entry) and stir before drawing. */
    unsigned seed = (unsigned)osGetTime() ^ (unsigned)svcGetSystemTick() ^ 0x9E3779B9u;
    for (int k = 0; k < 4; k++) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; }
    for (int i = g_n - 1; i > 0; i--) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;   /* xorshift32 */
        int j = (int)(seed % (unsigned)(i + 1));
        int t = g_order[i]; g_order[i] = g_order[j]; g_order[j] = t;
    }
    /* and never open with the song the last visit opened with */
    { char last[128] = ""; FILE *lf = fopen("sdmc:/moflex_player/store/lastsong.txt", "rb");
      if (lf) { if (fgets(last, sizeof last, lf)) { char *nl = strchr(last, '\n'); if (nl) *nl = 0; } fclose(lf); }
      if (g_n > 1 && last[0] && !strcmp(g_name[g_order[0]], last)) {
          int t = g_order[0]; g_order[0] = g_order[1]; g_order[1] = t;
      }
      lf = fopen("sdmc:/moflex_player/store/lastsong.txt", "wb");
      if (lf) { fprintf(lf, "%s\n", g_name[g_order[0]]); fclose(lf); } }

    if (ndspInit() != 0) return 0;          /* no dsp firm dumped -> the shop is just quiet */
    g_ok = 1;
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnSetMix(MUS_CHAN, (float[12]){ 0.7f, 0.7f });   /* under the room, not over it */
    for (int i = 0; i < MUS_BUFS; i++) {
        g_buf[i] = (s16 *)linearAlloc((size_t)MUS_FRAMES * 2 * 2);
        if (!g_buf[i]) { music_exit(); return 0; }
    }
    g_stage = (u8 *)malloc((size_t)MUS_FRAMES * 2);    /* MUS_FRAMES stereo frames at 4 bits */
    if (!g_stage) { music_exit(); return 0; }
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
    free(g_stage); g_stage = NULL;
    g_playing = 0;
}
