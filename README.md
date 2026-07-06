# CLOWNSEC VIDEO — Native moflex/MobiClip 3D Player for the 3DS

A homebrew video player for the Nintendo 3DS that plays `.moflex` (MobiClip video +
IMA-ADPCM audio) files **natively** — decoded in portable C, no on-device FFmpeg — with
synchronized audio. It auto-detects **stereoscopic 3D** vs flat **2D** and plays each
correctly, including movies embedded inside `.cia` files.

Full speed on **New 3DS** (2D and 3D). On **Old 3DS**, 2D plays smoothly; 3D delivers twice
the frames and the decode can't sustain that in real time, so it isn't smooth there (the
player shows a note during Old-3DS 3D playback).

The decoder is a standalone port of FFmpeg's MobiClip/moflex path, verified **bit-exact
against FFmpeg on PC** before ever touching hardware.

<p align="center">
  <img src="screenshots/player-title.png" width="440" alt="Clownsec Video — home screen">
</p>

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
- **Software volume boost** up to 400% for quiet sources.
- **Catalog browser** — browse the Clownsec / Zackk archives from `catalog.json`, with a
  top-screen info panel (poster art, year, runtime, genres, file size, description, and a
  3D/2D badge).
- **Downloader** — pull movies and TV seasons straight to the SD card (zips are extracted into
  their own folder), or grab any file by direct URL, with a destination-folder picker.
- **Built-in web server** — upload any files to the console from a browser over Wi-Fi.
- **File manager** — browse, move, delete, and create folders on the SD card (all files, with a
  movies-only toggle).
- **3D branding** on the top screen while idle.

## Screenshots

<table>
<tr>
<td align="center"><img src="screenshots/player-open-video.png" width="380"><br><sub>Open a video — browse the SD card for a <code>.moflex</code> or movie <code>.cia</code></sub></td>
<td align="center"><img src="screenshots/player-main-controls.png" width="380"><br><sub>Playback — video up top, touch controls below (play/pause, seek, volume)</sub></td>
</tr>
<tr>
<td align="center"><img src="screenshots/player-add-movies-catalog.png" width="380"><br><sub>Catalog browser — info panel with the <b>3D (stereoscopic)</b> badge</sub></td>
<td align="center"><img src="screenshots/player-add-movies-catalog-2.png" width="380"><br><sub>Catalog browser — a <b>2D</b> title</sub></td>
</tr>
<tr>
<td align="center"><img src="screenshots/player-add-movies.png" width="380"><br><sub>Add videos — download from a catalog, or upload over Wi-Fi</sub></td>
<td align="center"><img src="screenshots/player-add-movies-download.png" width="380"><br><sub>Download — pick a catalog source, a direct URL, or add your own</sub></td>
</tr>
<tr>
<td align="center"><img src="screenshots/player-add-movies-catalog-save-dialog.png" width="380"><br><sub>Save-to — choose the destination folder (or make a new one)</sub></td>
<td align="center"><img src="screenshots/player-add-movies-catalog-save-dloading.png" width="380"><br><sub>Downloading straight to the SD card</sub></td>
</tr>
<tr>
<td align="center"><img src="screenshots/player-add-movies-upload.png" width="380"><br><sub>Built-in web server — upload any files from a browser over Wi-Fi</sub></td>
<td align="center"><img src="screenshots/player-file-management.png" width="380"><br><sub>File manager — browse all files (with a movies-only toggle)</sub></td>
</tr>
<tr>
<td align="center" colspan="2"><img src="screenshots/player-file-management-2.png" width="380"><br><sub>File manager — delete or move any file</sub></td>
</tr>
</table>

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

## Credits & license

- The MobiClip/moflex decode path is derived from **FFmpeg** (LGPL) — see `ffmpeg_support/`
  and `decoder/`.
- Bundled: [cJSON](https://github.com/DaveGamble/cJSON) (MIT),
  [kuba--/zip](https://github.com/kuba--/zip) (Unlicense),
  [stb_image](https://github.com/nothings/stb) (public domain),
  dhepper font8x8 (public domain).
- MobiClip is a proprietary Actimagine/Nintendo codec; there is no open encoder.
