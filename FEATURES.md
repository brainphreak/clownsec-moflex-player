# CLOWNSEC VIDEO — Complete Feature List

Everything the player does, in one place. The [README](README.md) has the guided tour,
screenshots, and setup instructions; this is the full inventory.

## Playback

- **Native moflex/MobiClip decoding** — portable C port of the MobiClip video +
  IMA-ADPCM audio decoders, verified bit-exact against FFmpeg. No on-device FFmpeg.
- **2D & 3D auto-detection** — every file is probed and played as flat 2D or
  frame-interleaved stereoscopic 3D automatically.
- **Rock-solid 3D eye pairing** — left/right frames are paired and presented atomically
  (both eyes in one flip), so depth is never reversed and the eyes never desync, even
  after seeks or dropped frames.
- **Movies inside `.cia` files** — plays the moflex embedded in unencrypted movie CIAs
  in place, no extraction; a CIA holding several movies gets a picker with real titles.
- **Audio-master A/V sync** via `ndsp` — no long-term drift.
- **Decode-ahead engine** — frames are decoded ahead of time and banked, absorbing heavy
  scenes instead of stuttering on them.
- **Responsive seeking** — draggable seek bar (touch or D-pad), fast-forward / rewind,
  hold-to-scrub. The landing frame appears immediately and audio resumes in sync.
- **Resume playback** — position is saved per movie (and per episode inside a
  multi-movie CIA) and auto-resumes; finishing a movie clears its resume point.
- **Watched / unwatched badges** in the browser for everything you've finished.
- **Volume boost** — software gain up to 400% for quiet sources; volume slider and
  Up/Down buttons.
- **Simple, direct controls** — A play/pause, Left/Right seek (hold to scrub), Up/Down
  volume, B back; everything also touchable. No focus/select model to fight.
- **Touch lock** — L locks the touch screen (buttons keep working) so a stray touch
  can't seek.
- **Bottom screen off during playback** — the moon button powers off the bottom
  backlight while the movie plays on top; biggest single speed/battery win on Old 3DS.
- **HQ color toggle (Old 3DS)** — fast 16-bit color by default for the deepest
  decode-ahead cushion; the HQ button switches to 24-bit for cleaner gradients.
  Remembered across launches. (New 3DS is always 24-bit.)
- **3D branding screen** — stereoscopic logo on the top screen while idle.

## Subtitles (closed captions)

- **External SubRip (`.srt`) subtitles** on any movie, 2D or 3D.
- **Auto-load** — a matching `.srt` next to the movie (or in `moviedata/`) loads and
  turns on automatically.
- **CC menu during playback** — on/off, position (top/bottom), size (3 steps),
  per-movie delay in 0.25 s steps, and on 3D movies a **subtitle depth** control.
- **Load any SRT** — pick a differently-named `.srt` from the movie's folder.
- **UTF-8 handling** — smart quotes, dashes, accents, and the ♪ music note render.

## Library & metadata

- **Library browser** — every local movie, organized by category and genre, with counts;
  TV seasons collapse into one show entry that opens an episode list.
- **Search everywhere** with X.
- **Top-screen info panel** — poster art, title, year, runtime, genres, description,
  file size, and a 3D/2D badge for the highlighted movie.
- **Get Info scraper** — X on a movie matches it against the catalogs and writes its
  `.nfo` + poster; batch mode fills a whole folder in one pass.
- **Hand-written metadata** — drop a plain-text `.nfo` and a `.jpg`/`.png` poster into
  `moflex_player/moviedata/` for movies no catalog knows.
- **Forgiving filename matching** — bracketed tags, separators, and case are ignored, so
  `A_Goofy_Movie_(1995)_x264` and `A Goofy Movie (1995)` match the same entry.
- **Interactive library rescan** — Rescan Library sweeps the SD card for new videos on
  demand, with live progress.
- **Hidden system folders** — `3DS`, `DCIM`, `Nintendo 3DS`, etc. stay out of the way
  (Y reveals them).

## Catalog & downloads

- **Catalog browser** — browse the Clownsec / Zackk archives (movies and TV), with
  posters and full details on the top screen; add your own catalog sources.
- **Fast downloads** — transfers go through the 3DS system HTTP module, so TLS is
  decrypted by hardware instead of the CPU, and SD writes run in parallel with the
  network through a double-buffered writer. Live speed readout in Mbps and KB/s.
- **Download queue** — line up movies or whole seasons; they download one after another
  in the background while you keep browsing. QUEUE button jumps to the list; items can
  be reordered (Move to Top) or removed.
- **Per-file destination** — every file added to the queue picks its own folder.
- **Resumable transfers** — interrupted or cancelled downloads keep their partial data
  and resume where they left off, no matter which folder was chosen.
- **Season zips extract themselves** — with a progress bar, junk-file filtering, and
  automatic library add of every episode.
- **Direct URL download** — grab any file by URL with a filename prompt and folder
  picker.
- **Auto-retry with backoff** and a stall timeout, so a flaky connection never hangs
  the queue forever.

## Background downloads & power

- **Downloads survive a closed lid** — while anything is transferring, closing the lid
  only turns the screens off; the Wi-Fi is held alive and the queue keeps working.
- **Notification LED** — when the queue finishes, the LED breathes **green** (visible
  with the lid closed); a failed download breathes **red**.
- **Sleep or power off when done** — choose on the queue screen what a closed lid does
  once the queue empties: sleep (default) or power off. Lid open: neither.
- **Stock behavior otherwise** — with nothing downloading, the lid sleeps the console
  normally.
- **Idle screen-off** — 5 minutes without input in the menus turns both backlights off;
  any button or touch wakes them and the waking press isn't acted on. Playback is never
  affected.

## Getting files onto the console

- **Built-in Wi-Fi upload server** — open the shown URL in any browser on your computer
  or phone and drop files straight onto the SD card, with live progress on the 3DS.
- **File manager** — browse, move, delete, and create folders (all files, with a
  movies-only toggle).

## Interface

- **Themes** — recolor the entire UI: Clownsec (neon default), Matrix, Nintendo, Light,
  Amber, Ice, or build your own in the Custom editor (R/G/B sliders per palette item,
  live preview). Saved and restored across launches.
- **Neon touch UI** — every screen works by touch and by buttons; B always goes back.
- **Consistent navigation** — hold-to-repeat lists, page jumps, marquee scrolling for
  long titles, scroll bars.
- **Home screen = the player** — ▶ resumes the loaded movie; B out of playback lands
  back where you were.
- **Recently played** list.
- **Update check** — the player checks for new builds and prompts when one is available.

## Performance

- **Full speed on New 3DS** — 2D and 3D.
- **Old 3DS substantially sped up** — hand-optimized decoder (verified bit-exact),
  hardware Y2R color conversion, GPU rendering, decode-ahead banking, and graceful
  catch-up on heavy scenes. 2D plays smoothly; most 3D clips play smoothly with the
  bottom screen off.
- **Downloads on their own CPU core** (New 3DS) so a running queue never fights the UI.
