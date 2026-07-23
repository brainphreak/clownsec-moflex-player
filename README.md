# CLOWNSEC VIDEO — Native moflex/MobiClip 3D Player for the 3DS

<p align="center">
  <img src="screenshots/00-clownsec-hero.jpg" width="440" alt="CLOWNSEC 3DS Video Player — MOFLEX Player">
</p>

A homebrew video player for the Nintendo 3DS that plays `.moflex` (MobiClip video +
IMA-ADPCM audio) files **natively** — decoded in portable C, no on-device FFmpeg — with
synchronized audio. It auto-detects **stereoscopic 3D** vs flat **2D** and plays each
correctly, including movies embedded inside `.cia` files.

Full speed on **New 3DS** (2D and 3D). On **Old 3DS**, 2D plays smoothly and **3D has been
substantially sped up** — see **[3DS performance](#3ds-performance)** below for the HQ toggle
and the one tip that makes the biggest difference on the older hardware.

The decoder is a standalone port of FFmpeg's MobiClip/moflex path, verified **bit-exact
against FFmpeg on PC** before ever touching hardware.

## 3DS performance

A lot of recent work went into playback speed, especially **3D on the Old 3DS**, where every
frame is decoded twice (once per eye):

- **Faster decode + a decode-ahead engine.** The MobiClip decoder was hand-optimized and the
  3D player now decodes frames ahead of time and banks them, so it can absorb heavy scenes
  instead of stuttering on them. Most clips play smoothly on the Old 3DS now.
- **HQ button (Old 3DS only).** To stay smooth by default, the Old 3DS plays in a lighter
  **16-bit color** mode. Tap the on-screen **HQ** button — bottom-left of the player, opposite
  the moon — to switch to **24-bit color** for cleaner gradients on a movie that can afford it.
  Your choice is remembered across launches. (The New 3DS is always 24-bit, so it has no HQ
  button.)
- **Best performance: turn off the bottom screen.** Tap the **moon** button during playback to
  power off the bottom backlight — the movie keeps playing on top. This hands the console more
  time to spend on decoding and is the single biggest thing you can do to smooth out a demanding
  3D movie on the Old 3DS (it also saves battery). Any button press wakes the screen back up.

## Download

Grab the latest prebuilt binaries from the
[**Releases**](https://github.com/brainphreak/clownsec-moflex-player/releases/latest) page —
no building required:

- **`clownsec_player.3dsx`** — copy to your SD card's `/3ds/` folder and run from the
  Homebrew Launcher.
- **`clownsec_player.cia`** — install with FBI (or any CIA installer) for a HOME-menu icon.

### Testing in an emulator (Citra)

The player paces the video to the audio clock, so it needs the emulator's **DSP audio** to
actually work — otherwise the picture sits frozen at `0:00`. On real hardware this is never
an issue; it only affects emulators.

If the timer won't advance past `0:00` in Citra, give the emulator the 3DS **DSP firmware**:

1. Dump it from your own 3DS with [**DSP1**](https://github.com/zoogie/DSP1/releases) (run
   the `.3dsx`/`.cia` on the console; it writes `dspfirm.cdc` to the SD card's `/3ds/` folder).
   *(The firmware is copyrighted — dump your own; don't share the file.)*
2. Copy that `dspfirm.cdc` into the **`3ds` folder** of your emulator's virtual SD card, i.e.
   `<emulator SD>/3ds/dspfirm.cdc`. The SD-card location depends on your OS and install type:

   | OS / install | Virtual SD card path |
   |--------------|----------------------|
   | macOS | `~/Library/Application Support/Citra/sdmc/` |
   | Windows (installed) | `%APPDATA%\Citra\sdmc\` |
   | Windows (portable) | `<Citra folder>\user\sdmc\` |
   | Linux | `~/.local/share/citra-emu/sdmc/` |

   So on macOS the file goes at `~/Library/Application Support/Citra/sdmc/3ds/dspfirm.cdc`.
   (Citra forks such as Lime3DS/Azahar use their own folders — check that emulator's SD path.)
3. Restart the emulator; audio (and the video pacing) will work.

## Features

This is the short tour — the complete list of everything the player does lives in
[**FEATURES.md**](FEATURES.md).

- **2D & 3D with auto-detection** — each file is detected as frame-interleaved 3D or flat 2D
  and played correctly. For 3D, the left/right eyes are paired and presented atomically so the
  two views are always the same moment (no eye desync).
- **Plays movies embedded in `.cia` files** — the moflex inside an unencrypted movie CIA is
  played in place (no extraction); when a CIA holds several movies you get a picker with their
  real titles.
- **Audio + A/V sync** via `ndsp`, audio-master pacing.
- **Responsive seeking** — a draggable seek bar (touch + D-pad), FF/RW, and hold-to-scrub.
  After a seek the landing frame is shown immediately and audio resumes synced to it.
- **Resume** — playback position is saved per movie (and per episode inside a multi-movie CIA)
  and auto-resumes where you left off.
- **Watched / Unwatched indicators** — movies you finish are marked as watched in the browser,
  so you can see at a glance what you've already seen; the rest keep their resume point.
- **Software volume boost** up to 400% for quiet sources.
- **Subtitles (closed captions)** — drop a `.srt` next to a movie (works on **2D and 3D**) and it
  loads automatically. The in-player **CC** button toggles them and adjusts position, size,
  timing/delay, and — on 3D video — the subtitle depth. See [Subtitles](#subtitles-closed-captions).
- **Catalog browser** — browse the Clownsec / Zackk archives from `catalog.json`, with a
  top-screen info panel (poster art, year, runtime, genres, file size, description, and a
  3D/2D badge).
- **Downloader** — pull movies and TV seasons straight to the SD card (zips are extracted into
  their own folder), or grab any file by direct URL, with a destination-folder picker.
- **Download queue** — line up several movies or whole seasons and they download one after
  another in the background, with a live speed readout (Mbps / KB/s) showing real throughput.
  Every file you add picks its own destination folder. The **QUEUE** button jumps to the queue;
  downloads keep running while you browse, and interrupted transfers resume where they left off.
  Downloads run through the 3DS system HTTP module (TLS decrypted in hardware) with SD writes
  overlapped in parallel — the same techniques the fastest 3DS downloaders use.
- **Close the lid and walk away** — an active queue keeps downloading with the lid shut (the
  screens just turn off). When everything finishes, the **notification LED breathes green**
  (red if something failed) and the console **goes to sleep** — or **powers off**, your choice
  on the queue screen. See [Background downloads](#background-downloads-close-the-lid-and-walk-away).
- **Idle screen-off** — sitting in the menus untouched for 5 minutes (downloads or not) turns
  both backlights off to save battery; any button or touch wakes them, and the waking press
  isn't acted on. Video playback is never interrupted.
- **Built-in web server** — upload any files to the console from a browser over Wi-Fi.
- **File manager** — browse, move, delete, and create folders on the SD card (all files, with a
  movies-only toggle).
- **Local artwork + info** — poster and details show on the top screen for your own movies in
  Open Video, and a **Get Info** action fetches them from the catalogs for anything missing.
- **Hidden system folders** — the browser hides `3DS`, `DCIM`, `Nintendo 3DS`, etc. by default
  (press **Y** to reveal them), so you only see your movies and folders.
- **Themes** — recolor the whole UI. Pick from built-in themes (Clownsec, Matrix, Nintendo,
  Light, Amber, Ice) or build your own with the **Custom** color editor (R/G/B sliders for each
  of the accents, text, and background). Open it from the swatch chip on the home screen or press
  **Y**; your choice is saved and applied everywhere. See [Themes](#themes).
- **Turn off the bottom screen** during playback — tap the **moon** button on the player to power
  off the bottom backlight while the movie keeps playing up top (saves battery / kills the glow in
  the dark). Any button press wakes it back up.
- **3D branding** on the top screen while idle.

## Screenshots

<table>
<tr>
<td align="center"><img src="screenshots/02-player-open-video.png" width="340"><br><sub>Open Video — browse the SD card; tap to select, tap again to play</sub></td>
<td align="center"><img src="screenshots/08-player-playing-video.png" width="340"><br><sub>Playback — video on the top screen, neon touch controls below</sub></td>
</tr>
<tr>
<td align="center" colspan="2"><img src="screenshots/13-player-subtitles.png" width="300"><br><sub>Subtitles — a `.srt` overlaid on the video (2D or 3D), with the CC options menu below</sub></td>
</tr>
<tr>
<td align="center"><img src="screenshots/14-player-themes.png" width="340"><br><sub>Themes — live-preview picker (Clownsec, Matrix, Nintendo, Light, Amber, Ice, Custom)</sub></td>
<td align="center"><img src="screenshots/20-player-screen-off.png" width="340"><br><sub>Tap the moon to turn off the bottom screen — the movie keeps playing on top</sub></td>
</tr>
<tr>
<td align="center"><img src="screenshots/15-player-theme-matrix.png" width="340"><br><sub>Matrix theme</sub></td>
<td align="center"><img src="screenshots/16-player-theme-nintendo.png" width="340"><br><sub>Nintendo theme</sub></td>
</tr>
<tr>
<td align="center"><img src="screenshots/17-player-theme-amber.png" width="340"><br><sub>Amber theme — every screen (home, browser, player) recolors</sub></td>
<td align="center"><img src="screenshots/18-player-theme-custom.png" width="340"><br><sub>Custom editor — R/G/B sliders per palette item, with a live preview</sub></td>
</tr>
<tr>
<td align="center"><img src="screenshots/07-player-download-from-catalog.png" width="340"><br><sub>Catalog browser — poster + details on top, download from the list</sub></td>
<td align="center"><img src="screenshots/03-player-scrape-info.png" width="340"><br><sub>Get Info — auto-fetch poster + details from the catalogs</sub></td>
</tr>
<tr>
<td align="center"><img src="screenshots/04-player-manage-files.png" width="340"><br><sub>Manage — move, delete, and organize files on the SD card</sub></td>
<td align="center"><img src="screenshots/05-player-add-videos.png" width="340"><br><sub>Add Videos — download from a catalog, or upload over Wi-Fi</sub></td>
</tr>
<tr>
<td align="center"><img src="screenshots/06-player-download-sources.png" width="340"><br><sub>Download — pick a catalog source (or add your own)</sub></td>
<td align="center"><img src="screenshots/12-player-downloading-file.png" width="340"><br><sub>Downloading a movie straight to the SD card</sub></td>
</tr>
<tr>
<td align="center"><img src="screenshots/09-player-upload.png" width="340"><br><sub>Upload — the built-in Wi-Fi server (open the URL on a computer)</sub></td>
<td align="center"><img src="screenshots/10-player-upload-from-browser.png" width="340"><br><sub>…the upload page from your computer/phone browser</sub></td>
</tr>
<tr>
<td align="center" colspan="2"><img src="screenshots/11-player-upload-to-3ds.png" width="340"><br><sub>Live upload progress on the 3DS as the file arrives</sub></td>
</tr>
</table>

## Background downloads (close the lid and walk away)

Queue up a batch of movies, shut the lid, and come back later — the player takes care of the
rest:

- **The queue survives a closed lid.** While anything is downloading, closing the lid only
  turns the screens off: the Wi-Fi stays up and the queue keeps working through its items.
  (Normally the 3DS would sleep and kill the connection; the player holds the console awake
  and the wireless alive only while a transfer is running.)
- **The notification LED tells you how it went.** When the whole queue finishes, the LED
  breathes **green** — visible with the lid closed, StreetPass-style. If a download failed,
  it breathes **red** instead (the partial file is kept, so starting it again resumes).
- **Sleep or power off when done.** While a queue is downloading, the bottom row of the
  **QUEUE** screen cycles what a closed lid does once the queue empties: **sleep** (default)
  or **POWER OFF**. With the lid open, neither happens — you're there.
- **Every file picks its destination.** Each add opens the folder picker, and items in one
  queue can go to different folders.
- **Nothing downloading?** The lid behaves completely normally, and the menus themselves go
  dark after 5 idle minutes to save battery (any button wakes them).

## Movie info & artwork

When you highlight a movie in **Open Video**, its poster and details show on the top screen.
That metadata lives in one hidden folder, **`sdmc:/moflex_player/moviedata/`**, keyed by the
movie's filename (extension dropped) — so it works wherever the movie is on the card, and moving
the file doesn't lose its art. For `Some Movie (2001).moflex`:

- **`Some Movie (2001).nfo`** — plain text, hand-writable:
  ```
  title: Some Movie
  year: 2001
  runtime: 108
  genres: Drama, Thriller
  desc: A one-line description shown on the info panel.
  ```
- **`Some Movie (2001).jpg`** (or `.png`) — the poster (optional). Decoded and cached on first view.

**Get Info (fetch it automatically):** highlight a movie and press **X** in Open Video:

- **This movie** — matches it against the catalogs (Clownsec first, then Zackk, then any sources
  you've added) and writes the `.nfo` + poster. Forces a refresh even if it already has info.
- **All movies in this folder** — batch: fills in every movie that's missing art + description,
  fetching each catalog only once. Skips ones already complete.

Catalog **downloads** write this automatically, so those movies show their art with no extra step.

**Matching / naming:** a filename is reduced to `Title` + `year` for matching — all bracketed
groups are dropped (`(Extended)`, `(Subtitled)`, `(3D)`, `[HD]`), everything after the year is
ignored, and separators (spaces, `_`, `.`, `-`) and case don't matter. So
`A_Goofy_Movie_(1995)_x264...`, `A.Goofy.Movie.1995`, and `A Goofy Movie (1995)` all match the
same catalog entry, and a TV episode like `Doctor Who (2005) - S01E03` collapses to the show.
For best results (and to disambiguate remakes) name files **`Title (YEAR).moflex`**.

## Building

Requires [devkitPro](https://devkitpro.org/) with the 3DS toolchain and portlibs:

```sh
sudo dkp-pacman -S 3ds-dev 3ds-curl 3ds-mbedtls 3ds-zlib
```

### `.3dsx` (Homebrew Launcher)

```sh
cd app
make
```

Produces `app/clownsec_player.3dsx`. Copy it to your SD card's `/3ds/` folder.

### `.cia` (installable title)

The CIA build needs two third-party tools placed in `tools/bin/`:

- [`makerom`](https://github.com/3DSGuy/Project_CTR/releases)
- [`bannertool`](https://github.com/Epicpkmn11/bannertool/releases)

Then:

```sh
cd app
./build_cia.sh
```

Produces `app/clownsec_player.cia`. Install with FBI or your CIA installer of choice.

> Note: the CIA exheader (`app/cia.rsf`) maps the DSP hardware registers
> (`IORegisterMapping`) that `ndsp` audio writes to — without them a real 3DS throws a
> data-abort at launch (emulators don't enforce this).

## Project layout

| Path | Purpose |
|------|---------|
| `decoder/` | Portable MobiClip video + `adpcm_moflex` audio decoders and the moflex demuxer |
| `ffmpeg_support/` | The FFmpeg support C files + bundled headers needed by the decoder |
| `playback/` | Reusable `moflex_play(path)` — one call plays a file in 3D with audio |
| `app/` | The standalone player app (browser, catalog, downloader, web server, CIA build) |
| `net/` | Downloader (libcurl), web server, catalog parser, poster/JPEG decode |
| `ui/` | Tiny immediate-mode drawing, top-screen branding |
| `thirdparty/` | Bundled cJSON, kuba--/zip, stb_image, font8x8 |
| `pc_verify/` | PC harness to compare decode output bit-exact against FFmpeg |
| `tools/` | `gen_logo.py` (banner→RGB565), CIA build binaries in `tools/bin/` |

## Roadmap

- **Smooth 3D on Old 3DS.** 2D already plays well on Old 3DS; 3D delivers twice the frames and
  the portable-C MobiClip decode can't sustain that in real time on the Old 3DS CPU. Hardware
  color conversion (Y2R) and a decode-ahead pipeline are already in place, so the remaining gap
  is raw decode throughput — likely closable only with hand-tuned ARM assembly for the hot
  decode loops (as the official player does).

## Adding your own info & artwork (for movies not in a catalog)

If a movie isn't in any catalog, you can hand-author its poster and details. Everything lives in
one hidden folder on the SD card:

```
sdmc:/moflex_player/moviedata/
```

Files are named after the **movie's filename with the extension removed**. For a movie
`My Home Video (2024).moflex`, add either or both of:

| File | What it is |
|------|------------|
| `moviedata/My Home Video (2024).nfo`             | text details (see below) |
| `moviedata/My Home Video (2024).jpg` (or `.png`) | the poster image |

Because it's keyed by filename, the metadata follows the movie wherever it is on the card. You can
prepare these on a computer and drop them in over the **Wi-Fi upload page** (browse into `moviedata/`)
or by putting them straight on the SD card.

### The `.nfo` file

Plain text, one `field: value` per line. Every field is optional — include what you have:

```
title: My Home Video
year: 2024
runtime: 96
genres: Comedy, Family
3d: yes
desc: A one-line-ish description shown on the info panel.
```

- **title** — the heading (falls back to the filename if omitted)
- **year** — release year · **runtime** — minutes
- **genres** — comma-separated
- **3d** — `yes` or `no`; if omitted, it's guessed from `(3D)` in the filename
- **desc** — the description (keep it to a single line)

### The poster

A **`.jpg` or `.png`** with the same base name. Any size works — it's scaled to fit and cached — but
a portrait image around **132×188** or larger looks best. It's decoded the first time you view the
movie and cached after that, so it shows instantly on repeat views.

Then just highlight the movie in **Open Video** and your poster + details appear on the top screen.
(If the movie *is* in a catalog, **X → Get Info** writes all of this for you automatically.)

## Subtitles (closed captions)

The player can overlay external **SubRip (`.srt`)** subtitles on any movie — **2D or 3D**. There is
no embedded subtitle track in moflex, so subtitles always come from a sidecar file you supply.

<p align="center"><img src="screenshots/13-player-subtitles.png" width="300"></p>

### Adding a subtitle file

Name the `.srt` after the movie and put it in **either** place:

| File | Where |
|------|-------|
| `My Home Video (2024).srt`               | right next to the movie file |
| `moviedata/My Home Video (2024).srt`     | in the hidden `sdmc:/moflex_player/moviedata/` folder |

When you start the movie, a matching `.srt` is **loaded automatically** and turned on. You can copy
files over the Wi-Fi upload page or straight onto the SD card. (Only 16-bit standard SubRip text is
needed — the same `.srt` files used by any video player. UTF-8 smart quotes, dashes, accents, and
the ♪ music note are handled.)

### The CC button

During playback, tap **CC** on the bottom screen (or press **SELECT**) to open subtitle options:

- **Subtitles: on / off**
- **Position: bottom / top** of the screen
- **Size: small / medium / large**
- **Delay:** shift subtitles earlier or later in **0.25 s** steps for lip-sync (per movie)
- **Depth (3D):** push the captions into the screen or out toward you — *3D video only*
- **Load SRT file…:** pick any `.srt` in the movie's folder or `moviedata/` (handy when the
  sidecar isn't named to match)

Position, size, and depth are remembered across movies; the delay resets per movie.

## Themes

The entire interface — home, browser, library, catalog, and the player controls — can be
recolored. Open the theme picker from the little **color-swatch chip** at the top-left of the home
screen, or press **Y**.

<p align="center">
<img src="screenshots/14-player-themes.png" width="300">
&nbsp;&nbsp;
<img src="screenshots/17-player-theme-amber.png" width="300">
</p>

In the picker, move up/down (or tap a row) to **preview a theme live** — the whole UI recolors
instantly — then press **A** or **B** to apply. Your choice is saved to the SD card and restored on
next launch. Built-in themes:

| Theme | Look |
|-------|------|
| **Clownsec** | the default neon — green / purple / cyan on black |
| **Matrix** | green monochrome on near-black |
| **Nintendo** | grey background, blue accent, red |
| **Light** | dark text on a near-white background |
| **Amber** | retro amber / orange terminal |
| **Ice** | cyan / white on deep blue |

### Custom colors

Select **Custom** and press **X** to open the color editor:

<p align="center">
<img src="screenshots/18-player-theme-custom.png" width="300">
&nbsp;&nbsp;
<img src="screenshots/19-player-theme-custom-bg.png" width="300">
</p>

- **L / R** — cycle the palette item: *Accent 1/2/3, Danger, Bright text, Text, Muted, Background*
- **D-pad** — pick and adjust the **R / G / B** channel (or tap a slider to set it directly)
- **Y** — reset to defaults · **B** — save and exit

The editor's own chrome stays a fixed, readable dark scheme no matter what you pick, and a live
**preview** panel shows the themed result. Panel, row, and border shades are derived from your base
colors, so even light backgrounds stay coherent. Custom colors are saved to
`sdmc:/moflex_player/custom.cfg`.

## Credits & license

- The MobiClip/moflex decode path is derived from **FFmpeg** (LGPL) — see `ffmpeg_support/`
  and `decoder/`.
- Bundled: [cJSON](https://github.com/DaveGamble/cJSON) (MIT),
  [kuba--/zip](https://github.com/kuba--/zip) (Unlicense),
  [stb_image](https://github.com/nothings/stb) (public domain),
  dhepper font8x8 (public domain).
- MobiClip is a proprietary Actimagine/Nintendo codec; there is no open encoder.
