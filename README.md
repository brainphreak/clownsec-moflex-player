# CLOWNSEC VIDEO — Native moflex/MobiClip 3D Player for the 3DS

A homebrew video player for the Nintendo 3DS that plays `.moflex` (MobiClip video +
IMA-ADPCM audio) files **natively** — decoded in portable C, no on-device FFmpeg — in
**stereoscopic 3D** on the top screen with synchronized audio.

The decoder is a standalone port of FFmpeg's MobiClip/moflex path, verified **bit-exact
against FFmpeg on PC** before ever touching hardware.

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

- **Stereoscopic 3D playback** — the frame-interleaved left/right eyes are paired and
  presented atomically, so the two views are always the same moment (no eye desync).
- **Audio + A/V sync** via `ndsp`, audio-master pacing.
- **Responsive seeking** — a draggable seek bar (touch + D-pad), FF/RW, and hold-to-scrub.
  After a seek the landing frame is shown immediately and audio resumes synced to it.
- **Resume** — playback position is saved per movie and auto-resumes where you left off.
- **Software volume boost** up to 400% for quiet sources.
- **Catalog browser** — browse the Clownsec / Zackk archives from `catalog.json`, with a
  top-screen info panel (poster art, year, runtime, genres, file size, description).
- **Downloader** — pull movies and TV seasons directly to the SD card (zips are extracted
  into their own folder), with a destination-folder picker and create-folder support.
- **Built-in web server** — upload files to the console from a browser over Wi-Fi.
- **File manager** — move / delete / create folders on the SD card.
- **3D branding** on the top screen while idle.

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

- **Old 3DS support** — move YUV→RGB color conversion + screen rotation off the ARM11 CPU
  onto hardware (Y2R / GPU via citro3d). This is currently the pipeline's bottleneck and is
  what keeps Old 3DS from hitting full speed.

## Credits & license

- The MobiClip/moflex decode path is derived from **FFmpeg** (LGPL) — see `ffmpeg_support/`
  and `decoder/`.
- Bundled: [cJSON](https://github.com/DaveGamble/cJSON) (MIT),
  [kuba--/zip](https://github.com/kuba--/zip) (Unlicense),
  [stb_image](https://github.com/nothings/stb) (public domain),
  dhepper font8x8 (public domain).
- MobiClip is a proprietary Actimagine/Nintendo codec; there is no open encoder.
