# Moflex Store — prototype

One aisle of a video rental store in stereoscopic 3D, with real posters from the player's own
artwork cache. Separate `.3dsx`, separate Makefile — it shares no code with the player and
cannot affect it.

## What it is for

It exists to answer three questions before anyone builds the real thing:

1. **Does the frame rate hold?** N textured quads, drawn *twice* for stereo, on a 268 MHz GPU.
2. **Is it comfortable?** First-person movement in stereo on a handheld can be nauseating.
3. **Does the texture budget behave?** 6 MB of VRAM against a ~1500-title catalogue.

The on-screen readout reports fps, poster count, texture KB and cache-build time so those
questions get numbers rather than impressions.

## Running it

Copy `moflex_storetest.3dsx` to the SD card and launch it from the Homebrew Launcher.

It reads the player's existing artwork cache at `sdmc:/moflex_player/art` (the `*.p565` files).
On the **first run** it converts what it finds into `sdmc:/moflex_player/store` and reports how
many it built; that run is slower. Every run after is pure file reads.

If no art is cached it falls back to coloured placeholders, so it still runs on a fresh console.

Controls: circle pad walks and turns, the 3D slider sets depth, A "selects" the highlighted
poster, START exits.

## Why it is built the way it is

- **Posters are cached already tiled**, at a fixed power-of-two size (128x256 RGB565, 64 KB).
  The 3DS stores textures in 8x8 tiles with the pixels inside each tile in Morton order, so a
  normal image has to be swizzled before the GPU can sample it. Doing that at cache-build time
  means loading a poster is one `fread` straight into texture memory — no JPEG decode, no
  scaling, no swizzle while walking.
- **One unit quad**, reused for every poster with a per-poster matrix. Nothing about the shelf is
  uploaded per frame.
- **The room is a single draw call** sharing one small repeating texture.
- **Lighting is baked into vertex colours.** The PICA200 has no fragment shaders, only texture
  combiners — and fill rate is the scarce resource here anyway, with 400x240 drawn twice.
- **One `C3D_FrameBegin`/`FrameEnd` per frame**, both eyes inside it. Beginning a frame per eye
  submits two command buffers and waits twice, which halves the rate for nothing.

## What it deliberately does not do

No streaming, no LOD, no collision, no catalogue integration — one static aisle of up to 48
posters. That is the point: it is cheap enough to throw away.

The real thing needs distance-based LOD, because 40 posters at 64 KB already fills VRAM. A video
store solves this on its own: distant shelves show **spines** from a shared atlas, and only the
covers you are facing get a full texture. Turning to face a shelf and watching them resolve is
what browsing actually looks like.
