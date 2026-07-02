# Adding native `.moflex` (MobiClip 3D) playback to the MIVF player

This adds native `.moflex` support (MobiClip video in stereoscopic **3D** + IMA-ADPCM
audio) as a **self-contained, drop-in path**. Your existing `.mivf` code is not
touched — when the user opens a `.moflex`, you call one function; everything else
(demux, decode, 3D render, audio, A/V sync) is handled internally.

Everything is verified **bit-exact against ffmpeg** on PC, and runs **full speed on
New 3DS**. (Old 3DS currently ~half speed for 3D — see "Old 3DS" below.)

---

## 1. Copy the files in

Copy the whole `moflex_port/` folder into your repo (e.g. as `moflex/`). You need
these subfolders:

```
moflex/decoder/         moflex_demux.{c,h}  mobiclip.c  mobicompat.{c,h}  adpcm_moflex.{c,h}
moflex/ffmpeg_support/  vlc.c golomb.c mathtables.c reverse.c   include/  (bundled FFmpeg headers + config.h)
moflex/playback/        moflex_playback.{c,h}
```

(`pc_verify/` and `app/` are not needed in your build — they're the standalone
app and the correctness harness.)

## 2. Makefile changes

Your Makefile has `SOURCES:=source` and builds `INCLUDE` from `LIBDIRS`. Change:

```make
# was: SOURCES := source
SOURCES := source moflex/decoder moflex/ffmpeg_support moflex/playback
```

and extend the exported `INCLUDE` line (the one under `ifneq ($(BUILD),...)`):

```make
export INCLUDE := $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD) \
	-I$(CURDIR)/moflex/ffmpeg_support/include \
	-I$(CURDIR)/moflex/ffmpeg_support/include/libavcodec \
	-I$(CURDIR)/moflex/decoder \
	-I$(CURDIR)/moflex/playback
```

Note: your `CFLAGS` uses `-Wall`; the FFmpeg-derived files (`mobiclip.c`, `vlc.c`,
`golomb.c`) will emit **lots of harmless warnings**. They compile fine. If the noise
bothers you, add `-Wno-all` isn't a thing — simplest is to leave it, or move the
moflex files to a sub-make with `-w`. Nothing here is an error.

No new libraries: it links against `-lctru -lm` exactly like your player already does.

## 3. Hook it into file open (the only code change)

In `main()` your loop calls `play()` for the selected media. Add a dispatch on the
extension. At the top of `main.c`:

```c
#include "moflex_playback.h"   /* declares moflex_play() + MoflexResult */
```

Then where you currently do `int r = play();` (around the `g_hfix58_selected_media`
handling), branch by extension:

```c
int r;
size_t L = strlen(MIVF_PATH);
if (L > 7 && strcasecmp(MIVF_PATH + L - 7, ".moflex") == 0) {
    /* native moflex path — self-contained; uses ndsp channel 0 + top screen */
    MoflexResult mr = moflex_play(MIVF_PATH);
    gfxSet3D(false);                 /* back to 2D for your UI */
    r = (mr == MOFLEX_QUIT_EXIT) ? 1 : 0;   /* 1 = exit app, 0 = back to browser */
} else {
    r = play();                       /* your existing .mivf path, unchanged */
}
```

Also make your **file browser accept `.moflex`** (wherever it filters `.mivf`, allow
`.moflex` too) so the user can select them.

That's it. `moflex_play()`:
- sets the top screen to RGB565 + `gfxSet3D(true)`,
- reads the file via its own buffered reader (no need to wire your `mivf_io_ring`),
- decodes MobiClip → renders **left eye to `GFX_LEFT`, right eye to `GFX_RIGHT`**,
- decodes audio → **ndsp channel 0** (make sure `ndspInit()` has been called at
  startup — your player already does this),
- **B or START** returns to your menu.

## 4. Notes / gotchas

- **ndsp channel 0**: `moflex_play` resets and uses channel 0. Since it runs *instead
  of* your `play()`, there's no conflict. It clears its wavebufs on return.
- **3D**: it enables stereoscopic 3D. Call `gfxSet3D(false)` after it returns (shown
  above) so your 2D UI looks right.
- **Bottom screen**: `moflex_play` only touches the top screen; your bottom UI/console
  is left as-is (it won't be updated during playback).
- **FFmpeg headers** are bundled under `ffmpeg_support/include/` (LGPL — keep the
  license headers). A `SUINT` define lives in `mobicompat.h` because
  `libavutil/internal.h` isn't pulled in.

## 5. Tighter integration (optional, later)

If you'd rather reuse *your* renderer/`io_ring` instead of `moflex_play`'s built-in
loop, the decoder exposes a small C API (see `decoder/`):

```c
size_t mobi_ctx_size(void);
int  mobi_init(AVCodecContext *ctx);          /* set ctx.width/height, ctx.priv_data first */
int  mobi_decode(AVCodecContext*, AVFrame*, int *got, AVPacket*);
int  mobi_close(AVCodecContext *ctx);
```
The decoded `AVFrame` is planar YUV420 with `linesize[0]=w`, `linesize[1]=linesize[2]=w/2`
and **no padding** — i.e. it maps 1:1 onto your `M2Y0Frame` (`y`/`cb`/`cr`), so you can
point an `M2Y0Frame` at `out->data[0..2]` with zero copy and feed
`m2y0_to_top_rgb565_direct`. For 3D you'd render frame N to the left eye and N+1 to the
right (frames alternate L/R). Demux packets with `mfx_next_packet()`; audio packets go
to `adpcm_moflex_decode()`.



## Bonus: the HOME/POWER hang fix (separate from moflex)

Brianphreak mentioned HOME/POWER hangs the MIVF player (even at the menu), while B/START exit
cleanly. Cause: the APT hook powers the backlight **off on `APTHOOK_ONSUSPEND`**, which
fires whenever the HOME menu opens — so the HOME menu is drawn on a black (backlight-off)
screen and looks frozen. Fix: only kill the backlight on real **sleep**
(`APTHOOK_ONSLEEP`), not on HOME-menu suspend:

```c
case APTHOOK_ONSLEEP:                 /* real sleep / lid close ONLY */
    if (g_media_ctl.state == STATE_PLAYING) {
        g_media_ctl.state = STATE_PAUSED;
        g_mivf_park_resume_audio = true;
        if (audio.ready) ndspChnSetPaused(0, true);
    }
    GSPLCD_PowerOffAllBacklights();
    break;
case APTHOOK_ONSUSPEND:               /* HOME menu: pause A/V, leave backlight ON */
    if (g_media_ctl.state == STATE_PLAYING) {
        g_media_ctl.state = STATE_PAUSED;
        g_mivf_park_resume_audio = true;
        if (audio.ready) ndspChnSetPaused(0, true);
    }
    break;
```

`APTHOOK_ONRESTORE`/`ONWAKEUP` already turn the backlight back on, so both paths stay
balanced. (Minor: `main()` calls `aptInit()`/`aptExit()` explicitly even though libctru
already inits APT at startup — harmless but redundant; can be removed.)
