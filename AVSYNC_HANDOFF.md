# Old-3DS 3D moflex A/V sync — session handoff

## Goal
Play 3D (frame-interleaved L/R) `.moflex` video **smoothly on Old 3DS** with **smooth, in-sync, non-stuttering audio** — matching the official player. The engine is being proven in an isolated test harness (`avtest/`) with NO player UI, then will be ported into the real player's Old-3DS-3D path (`playback/moflex_playback.c` → `moflex_play_gpu`).

## TEST BUILD RIGHT NOW
`~/Downloads/moflex_avtest.3dsx` (commit `f85ee41`, the **YUV-ring**). Put a `.moflex` in `sdmc:/` root; it plays the first one. B/START = exit. Bottom screen HUD:
- `d` = raw decode rate (pairs/sec; **≥24 = keeping up**, dips to ~15 on heavy action = the deficit)
- `drop` = frames dropped by catch-up skip
- `bk` = compressed backlog depth, `kf` = keyframes buffered
- `fps`, `q` = decoded-ring depth (0 = starved), `e` = A/V error ms (near 0 = synced), `apos` = audio ms

**What to check on the YUV-ring build:** does the bigger cushion (NBUF 40→80, ~3.3s) make the action **freezes noticeably rarer**? Watch `q` — it should hold >0 for longer into action. If it **crashes on launch** = out of memory, lower `NBUF` (avtest/source/main.c:29).

## Build / deploy
```
cd /Users/brainphreak/3ds-player/moflex_port/avtest
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
make && cp moflex_avtest.3dsx ~/Downloads/
```
(The real player builds via `app/build_cia.sh`, auto-copies to ~/Downloads. NEVER write to sdmc from inside the playback loop. No AI mention in commits/README.)

## THE ENGINE (avtest/source/main.c) — this part WORKS
Single demuxer, single thread, **two-phase loop**:
- **Phase 1 (audio priority):** read ahead, feed audio to a deep ndsp bank (`AWB=256`, `ABUF=8192` samples/buf), stash video packets (+keyframe flag) into a compressed **backlog** (`VQN=512`). Keeps the audio clock advancing during slow decode → **audio never stutters**.
- **Phase 2 (video, bounded):** decode ONE pair/iter from the backlog into the decoded ring.
- `audio_poll()` advances `g_apos` from FINISHED wavebufs EVERY iter (not just on feed — else the clock freezes → deadlock).
- **Pre-roll:** channel starts PAUSED; prime `PRIME=12` pairs; then unpause + `g_apos=0` (aligns content-0 so audio isn't ahead at start).
- **Present is audio-master by `rts[]`** (each ring slot's true content-time, since drops leave content gaps). Drops stale pairs to stay locked to audio.
- **YUV-ring (newest):** decoded frames stored as raw YUV (144KB/eye) in the NORMAL heap; Y2R→texture only at DISPLAY into a 2-slot ping-pong pool. Frees the linear heap and ~40% smaller/frame → deeper cushion.

Confirmed by user across builds: **smooth in-sync audio, clean picture in calm scenes, no freeze in calm scenes.**

## THE CORE PROBLEM (unsolved, fundamental)
On heavy action the **decoder can't keep up: `d`≈15 vs 24 needed.** Our mobiclip is ~28 pairs/sec avg but dips to 15 on coefficient-heavy action. Forcing sync then has **exactly 3 outcomes, all confirmed on-device — there is no 4th:**
1. **Skip + `mobi_flush`** → CLEAN picture but **multi-second FREEZE**. ← user's preferred trade
2. **Skip without flush / deep-copy seed** → quick recovery but **PIXELATION/blur artifacts**
3. **No skip** → clean but **DRIFTS out of sync**

**Why the freeze length can't be tuned shorter:** mobiclip's "keyframe" = the intra bit (`moflex_demux.c` acc[0]&0x80), NOT a clean IDR. P-frames after our resync keyframe still reference frames from BEFORE the skip. `mobi_flush` nulls those (`mobiclip.c:1360-1363` null-data → decode error) → nothing decodes → pool never refills → holds until the next NATURAL keyframe (seconds). `DRIFT_MAX`, nearest-vs-farthest target, catch-up size all change freeze **FREQUENCY only, never LENGTH**.

**Why the seed can't fix it:** motion vectors were computed vs the TRUE previous frame (never decoded after a skip). Any reference we substitute (keyframe copy) is valid-but-WRONG pixels → artifacts. `mobi_seed_refs()` exists in mobiclip.c (deep-copy version, builds/runs, no crash) but produces propagating artifacts — do NOT use it.

**The official player avoids all this by decoding fast enough to NEVER skip.** We exhausted decoder speedups (assembly lost to GCC; sparse IDCT is the best at `mobi_opt=0x1A0E`, bit-exact). So matching it would need a genuinely faster decoder we don't have.

## CURRENT STRATEGY (this build)
Can't fix the freeze → make it RARE by carrying a bigger CLEAN cushion so fewer bursts trigger a skip. That's the YUV-ring (just built). Current skip config: `mobi_flush` resync to the NEAREST keyframe at/before the audio clock, `DRIFT_MAX=6` (~0.25s). Tuning knob `DRIFT_MAX` (avtest/source/main.c ~line 271): higher = fewer freezes/looser sync, lower = tighter sync/more freezes.

## THINGS TRIED (don't redo)
- Two demuxers (audio+video) → SD seek-thrash halved decode to 12fps. → ONE demuxer.
- Core-1 audio worker → same 12fps (it was the 2nd demux, NOT core contention).
- Pure backlog (no frame-drop) → video lag grew unbounded, `e` increasing, sync broke.
- `mobi_flush` + overshoot to future keyframe → froze waiting for audio to reach it.
- External `av_frame_ref` seed → CRASH (compat av_frame_ref is a shallow pointer-share; ff_reget reused the shared buffer → corruption/UAF).
- `ABUF=16384` × `AWB=256` (16MB) + NBUF=32 textures (16MB) = ~32MB → OOM crash. Linear heap is the tight one.

## NEXT STEPS
1. Test the YUV-ring build; tune `NBUF` up if RAM allows / down if OOM; tune `DRIFT_MAX`.
2. If freezes acceptably rare → **port the whole engine into `moflex_play_gpu`** (drop its core-1 worker AND per-frame UI panel; add single-demux + Phase-1 audio + YUV-ring + flush-skip).
3. Longer shot for the freeze itself: only a faster decoder eliminates it (official-player parity). Not currently achievable.

## Key files
- `avtest/source/main.c` — the test engine (all the above).
- `decoder/mobiclip.c` — decoder; `mobi_flush` (1753), `mobi_seed_refs` (after it, unused), decode ref/null path (1360-1363), frame-type bit (1668).
- `decoder/moflex_demux.c:357` — keyframe flag = intra bit.
- `decoder/mobicompat.c` — `av_frame_ref` is SHALLOW (47), `ff_reget_buffer` (32).
- Memory: `~/.claude/.../memory/moflex-player-port.md` — full campaign record incl. the "★★ CATCH-UP IS FUNDAMENTALLY 3-WAY" and "DEFINITIVE CAMPAIGN RECORD" entries.

## Recent commits (avtest)
`f85ee41` YUV-ring · `ba25f3f` small+frequent freezes+ring40 · `cc0e341` flush+drift-threshold · `b85453a` revert-to-no-flush · `2adb79d` deep-copy-seed(artifacts) · `ac8048f` no-flush-skip(ghosting) · `169d61e` backlog+keyframe-drop · `3e574c7` audio-clock-from-DSP.
