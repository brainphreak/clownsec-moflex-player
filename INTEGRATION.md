# Adding native `.moflex` (MobiClip 3D) playback to the MIVF player

This adds native **MobiClip / moflex** playback as a **self-contained, drop-in path**.
Your existing `.mivf` code is not touched — when the user opens a `.moflex` (or a
movie `.cia`), you call one function; everything else (demux, decode, 2D/3D render,
audio, A/V sync, resume) is handled internally.

What it does now (updated from the first version we sent):

- **Plays `.moflex`** — MobiClip video + IMA-ADPCM audio, verified bit-exact vs ffmpeg.
- **Plays `.moflex` embedded inside an unencrypted `.cia`** in place (the "movie CIAs"
  some libraries distribute — the moflex sits in the clear in the RomFS, no extraction).
  If a CIA holds several movies you can list them and let the user pick (real titles).
- **Auto-detects 2D vs 3D** per file and sets stereoscopic 3D only when the content is
  actually frame-interleaved 3D. No caller flag needed.
- **Resume** — remembers where each movie was stopped (per file, and per embedded movie
  inside a multi-movie CIA) under `sdmc:/moflex_player/resume/`.

Runs **full speed on New 3DS** (2D and 3D). On **Old 3DS**, 2D is smooth; 3D decodes the
doubled frame count too slowly to stay smooth, so the player shows a small
"3D Has Performance Issues on Old3DS" note during Old-3DS 3D playback (2D is unaffected).

---

## 1. Copy the files in

Copy the whole `moflex_port/` folder into your repo (e.g. as `moflex/`). You need these
subfolders (new files vs the first version are marked **NEW**):

```
moflex/decoder/         moflex_demux.{c,h}  mobiclip.c  mobicompat.{c,h}  adpcm_moflex.{c,h}
                        cia_moflex.{c,h}   NEW  (CIA-embedded moflex)
                        mc_asm.s           NEW  (ARM motion-comp; referenced at link time — required)
moflex/ffmpeg_support/  vlc.c golomb.c mathtables.c reverse.c   include/  (bundled FFmpeg headers)
moflex/playback/        moflex_playback.{c,h}
                        y2r_video.{c,h}    NEW  (hardware YUV->RGB via the Y2R block)
moflex/ui/              ui_gfx.{c,h}  font8x8_basic.h   NEW  (the in-player control panel)
```

Not needed in your build: `app/`, `pc_verify/`, `gputest/`, `bench/` (standalone app +
test harnesses), and `ui/branding.*` + `ui/clownsec_logo.h` (our app's logo only).

## 2. Makefile changes

**Sources** — add the four moflex folders (the `.s` file is picked up automatically by
the devkitPro rules as long as its folder is in `SOURCES`):

```make
# was: SOURCES := source
SOURCES := source moflex/decoder moflex/ffmpeg_support moflex/playback moflex/ui
```

**Includes** — extend the exported `INCLUDE` line:

```make
export INCLUDE := $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD) \
	-I$(CURDIR)/moflex/ffmpeg_support/include \
	-I$(CURDIR)/moflex/ffmpeg_support/include/libavcodec \
	-I$(CURDIR)/moflex/decoder \
	-I$(CURDIR)/moflex/playback \
	-I$(CURDIR)/moflex/ui
```

**Libraries** — this now uses citro2d/citro3d (GPU-assisted color conversion + the
Old-3DS render pipeline), so add them **before** `-lctru`:

```make
# was: LIBS := -lctru -lm
LIBS := -lcitro2d -lcitro3d -lctru -lm
```

(citro2d/citro3d ship with libctru in devkitPro — no separate install.) Your existing
libs stay; just make sure citro2d/citro3d appear ahead of `-lctru`.

The FFmpeg-derived files (`mobiclip.c`, `vlc.c`, `golomb.c`) emit **lots of harmless
warnings** under `-Wall`. They compile fine; nothing here is an error.

## 3. Hook it into file open (the only required code change)

At the top of `main.c`:

```c
#include "moflex_playback.h"   /* moflex_play() + MoflexResult */
#include "cia_moflex.h"        /* cia_is_cia() + the CIA helpers */
```

Where you currently do `int r = play();`, branch by extension. A `.moflex` **or** a movie
`.cia` both go to `moflex_play()`:

```c
int r;
size_t L = strlen(MIVF_PATH);
int is_moflex = (L > 7 && strcasecmp(MIVF_PATH + L - 7, ".moflex") == 0);
int is_cia    = cia_is_cia(MIVF_PATH);

if (is_moflex || is_cia) {
    if (is_cia && !cia_has_moflex(MIVF_PATH)) {
        /* a .cia that is an app, not a movie -> let your UI show "not a movie" */
        r = 0;
    } else {
        MoflexResult mr = moflex_play(MIVF_PATH);   /* self-contained: video+audio+3D+resume */
        gfxSet3D(false);                            /* back to 2D for your UI */
        r = (mr == MOFLEX_QUIT_EXIT) ? 1 : 0;       /* 1 = exit app, 0 = back to browser */
    }
} else {
    r = play();                                     /* your existing .mivf path, unchanged */
}
```

Also make your **file browser accept `.moflex` and `.cia`** (wherever it filters `.mivf`).

`moflex_play()` then, on its own:
- auto-detects 2D vs 3D and sets the top screen (`gfxSet3D(true)` only for real 3D),
- reads the file via its own buffered reader (for a `.cia` it opens the embedded moflex
  window in place — no extraction),
- decodes MobiClip → left eye `GFX_LEFT`, right eye `GFX_RIGHT` (2D → left only),
- decodes audio → **ndsp channel 0** (your player already calls `ndspInit()` at startup),
- draws its own control panel on the **bottom screen** (A play/pause, Left/Right seek,
  Up/Down volume, B back; touch works too),
- auto-resumes and checkpoints position under `sdmc:/moflex_player/`,
- **B** returns to your menu.

## 4. Optional: multi-movie CIAs (picker + real titles)

A single movie `.cia` can contain several moflex (episode packs). To let the user pick
which one — and show the real movie names (read from the CIA's `movie_title.csv`) — list
them, pick one, then set it as the selection before calling `moflex_play()`:

```c
CiaMoflex list[32];
int n = cia_list_moflex(MIVF_PATH, list, 32);   /* list[i].name = real title */
if (n > 1) {
    int sel = your_menu_pick(list, n);          /* your own list UI; list[i].name to display */
    cia_set_selection(MIVF_PATH, list[sel].off, list[sel].size, list[sel].name);
}
moflex_play(MIVF_PATH);                          /* plays the selected one; title shows in-player */
cia_clear_selection();
```

If you skip this, `moflex_play()` on a `.cia` just plays the first embedded moflex.

## 5. Notes / gotchas

- **ndsp channel 0**: `moflex_play` resets and uses channel 0. It runs *instead of* your
  `play()`, so there's no conflict; it clears its wavebufs on return.
- **2D/3D is automatic**: no caller flag. Just call `gfxSet3D(false)` after it returns so
  your 2D UI looks right (shown above).
- **Bottom screen**: `moflex_play` draws its own control panel there during playback (via
  `ui/ui_gfx`). It restores nothing — redraw your own bottom UI after it returns.
- **Y2R service (CIA builds only)**: the hardware color path uses the `y2r:u` service.
  Homebrew `.3dsx` has access automatically. If you ship your player as a **CIA**, add
  `y2r:u` to the service-access list in your RSF, or `y2r_video_init` fails and the player
  falls back to a slower software color conversion (still correct, just slower).
- **`mc_asm.s` is required at link time**: `mobiclip.c` references its symbols even though
  they're only *called* on a runtime-gated fast path. Keep the file in `SOURCES` or you'll
  get undefined-reference link errors.
- **FFmpeg headers** are bundled under `ffmpeg_support/include/` (LGPL — keep the license
  headers). A `SUINT` define lives in `mobicompat.h` because `libavutil/internal.h` isn't
  pulled in.

## 6. Tighter integration (optional, later)

If you'd rather reuse *your* renderer/`io_ring` instead of `moflex_play`'s built-in loop,
the decoder exposes a small C API (see `decoder/`):

```c
size_t mobi_ctx_size(void);
int  mobi_init(AVCodecContext *ctx);          /* set ctx.width/height, ctx.priv_data first */
int  mobi_decode(AVCodecContext*, AVFrame*, int *got, AVPacket*);
int  mobi_close(AVCodecContext *ctx);
```

The decoded `AVFrame` is planar YUV420 with `linesize[0]=w`, `linesize[1]=linesize[2]=w/2`
and **no padding** — it maps 1:1 onto your `M2Y0Frame` (`y`/`cb`/`cr`), so you can point an
`M2Y0Frame` at `out->data[0..2]` with zero copy and feed `m2y0_to_top_rgb565_direct`. For
3D, render frame N to the left eye and N+1 to the right (frames alternate L/R). Demux with
`mfx_next_packet()`; for a `.cia` open with `mfx_open_auto()` (windowed) instead of
`mfx_open()`; audio packets go to `adpcm_moflex_decode()`. `mfx_detect_stereo()` tells you
2D vs 3D.

---

## Bonus: the HOME/POWER hang fix (separate from moflex)

Brainphreak mentioned HOME/POWER hangs the MIVF player (even at the menu), while B/START
exit cleanly. Cause: the APT hook powers the backlight **off on `APTHOOK_ONSUSPEND`**,
which fires whenever the HOME menu opens — so the HOME menu is drawn on a black
(backlight-off) screen and looks frozen. Fix: only kill the backlight on real **sleep**
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
