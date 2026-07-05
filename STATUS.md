# Overnight status — moflex_port

Worked autonomously on the three deliverables. Nothing destructive: your friend's
`source/main.c` was NOT modified, nothing was pushed to git. All work is under
`moflex_port/`.

## ✅ 1. Self-contained package (was: depended on a temporary scratch dir)
- Bundled the FFmpeg headers + `config.h` into `ffmpeg_support/include/` (705 headers).
  `moflex_port` now builds with **no external FFmpeg tree**.
- `app/` builds a `.3dsx` against the bundle — **verified compiles** under devkitARM.
- `pc_verify/build_pc.sh` rewritten to be self-contained; **re-verified bit-exact**
  vs ffmpeg for both video (200 frames) and audio (12s) from the packaged code.

## ✅ 2. Reusable playback module + standalone app
- Extracted the whole play loop into `playback/moflex_playback.c` — one call:
  `moflex_play("sdmc:/movie.moflex")` (demux + MobiClip 3D decode + ndsp audio +
  A/V sync + B/START to exit). Used by BOTH the standalone app and your friend's player.
- `app/` is now a real standalone player: **bottom-screen file browser** (scans
  `sdmc:/` for `.moflex`, D-pad to select, A to play, START to exit) → `moflex_play`.
- **Builds clean.** Deployed `clownsec_player.3dsx` to the repo root and both Citra sdmc
  dirs. ⚠️ The browser is **built but not yet hardware-tested** (couldn't reliably
  drive Citra's UI while you were asleep) — please give it a run.

## ✅ 3. Integration guide for your friend
- `INTEGRATION.md`: exact files to copy, Makefile changes, the one-line-ish dispatch
  hook (`.moflex` → `moflex_play`), gotchas, an optional tighter-integration API, and
  the **HOME/POWER hang fix** written out (backlight-off should be `ONSLEEP` only, not
  `ONSUSPEND`).

## Layout now
```
moflex_port/
  decoder/        moflex_demux, mobiclip, mobicompat, adpcm_moflex   (portable core)
  ffmpeg_support/ vlc/golomb/mathtables/reverse .c + include/ (bundled FFmpeg headers)
  playback/       moflex_playback.{c,h}   (reusable moflex_play())
  app/            standalone player (source/main.c + Makefile) -> clownsec_player.3dsx
  pc_verify/      build_pc.sh + test_decode.c + test_audio.c (bit-exact vs ffmpeg)
  README.md  INTEGRATION.md  STATUS.md
```

## Not done / next
- Standalone app: **hardware test the browser**; add on-screen controls beyond exit
  (pause, seek, progress) — currently only B/START (exit) are wired.
- SMDH: uses default icon + title; add a custom icon if you want.
- Old 3DS full speed: GPU (citro3d) blit path (documented in INTEGRATION.md §6).
- Optional: PTS-perfect A/V sync (current is audio-master, no long-term drift).

## Build commands
- App:  `cd app && make`  (needs `DEVKITPRO`/`DEVKITARM` exported)
- Verify on PC: `cd pc_verify && bash build_pc.sh` then compare to ffmpeg
