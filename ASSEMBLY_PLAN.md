# MobiClip decoder — hand-assembly port plan (match the official 3DS player on Old 3DS)

## 0. Purpose of this document
Old 3DS (ARM11 MPCore, ~268 MHz) **cannot** decode our test moflex files (e.g. "A Minecraft
Movie") at full frame rate with the current **portable C** decoder — it dips below real time on
busy scenes, so video starves (ring empties, `q0`) and drifts behind audio. The **official
Nintendo/Actimagine player decodes the SAME files perfectly on the SAME hardware.** Therefore this
is a **software speed gap (~2×), not a hardware limit.** Their decoder is hand-written ARM assembly
tuned for this exact chip; ours is a C port of FFmpeg's `mobiclip.c`.

Goal: rewrite the **compute-bound** decoder hot loops in ARM assembly to roughly double throughput,
validated **bit-exact** against the C reference. This doc is the execution plan. Everything below is
concrete (file paths, function names, line numbers, reference decompiled addresses, gotchas).

**Read `memory/moflex-player-port.md` too** — it has the full campaign history and every dead end.

---

## 1. Hard-won findings — DO NOT re-derive these (they cost days)

1. **Motion compensation is MEMORY-LATENCY bound, not compute bound.** It is ~38% of decode time,
   spent *waiting on RAM* for reference-frame pixels. **Assembly there is proven useless** — we wrote
   `decoder/mc_asm.s` (half-pel: `mc_havg_a`/`mc_vavg_a`/`mc_diag_a`, UHADD8 + aligned word loads +
   `<<8` shift-neighbor, bit-exact first try) and it **tied/lost to GCC** (13.42 vs 13.21 ms/frame;
   with prefetch it got *worse*). C ports (`mobi_opt` bits 1/16/0x40/0x80) also never helped. **Do not
   spend time on motion-comp instruction tuning.** The ONLY thing that ever touched it was cache
   prefetch PLD (`mobi_opt` bit 8, ~5%).
2. **The compute-bound phases are where assembly wins:** the **entropy/VLC decoder** (bit-by-bit
   Huffman, branchy) and the **IDCT** (pure math). These are ~50% combined. GCC vs hand-scheduled
   assembly differ most here. THIS IS THE TARGET.
3. **The IDCT is already fused to pixels** in our C (no intermediate `mat[]` round-trip; see
   `add_coefficients_impl`), so the reference's "fused output" trick is already done. The remaining
   IDCT win is **hand-scheduling + specialized sparse paths** (see §4).
4. **Our frames are already word-aligned per row** (width 400, `linesize=400`, `400%4==0`), so the
   reference's "aligned word load" trick gives us nothing extra. Their `stride 0x400 (1024)` is just
   their buffer width, NOT a cache optimization (it would *hurt* our cache — rows further apart).
   **Do not chase the stride-1024 layout.**
5. Profiling must stay **OFF in shipping** — `#ifdef MOBI_PROFILE` (NOT `__3DS__`). Leaving it on
   inflated every measurement ~15% and produced misleading conclusions for a full day.
6. **Validation is non-negotiable and already built:** the `gputest` checksum harness (FNV framesum)
   does bit-exact A/B of any `mobi_opt` config against stock. Every asm change MUST be gated behind a
   `mobi_opt` bit and pass checksum=identical before you trust a speed number.

---

## 2. Target hardware (write asm for THIS)
- **ARM11 MPCore, ARMv6k**, `-march=armv6k -mtune=mpcore -mfloat-abi=hard`. Old 3DS app core =
  core 0 at ~268 MHz. `svcGetSystemTick` ≈ 268 MHz (`SYSCLOCK_ARM11`).
- 32 KB L1 I-cache + 32 KB L1 D-cache, **shared memory bus** with core 1 (the audio worker steals
  bus cycles — keep it light; already reduced its poll to 3 ms).
- Useful ARMv6 media ops: `USAT` (saturate — GCC already emits this for `av_clip_uint8`, so a clamp
  LUT is *slower*, don't use it), `UHADD8` (unsigned halving add ×4 bytes — but corrected avg needs
  `-((a&b)&0x01010101)` to match truncated `(a>>1)+(b>>1)`), `SEL`, `PKHBT/PKHTB`, `SSAT`, `SMLAD`,
  `LDRD/STRD`, `LDM/STM`, `PLD` (prefetch). **Avoid unaligned loads** — ARM11 penalty is what killed
  naive SWAR. `-munaligned-access` exists but don't rely on it in hot loops.
- Assembly builds already wired: `SFILES` in `app/Makefile`, `gputest/Makefile`, `gputest/Makefile.ip`
  compile `decoder/*.s`. Follow `mc_asm.s` for calling-convention/AAPCS style.

---

## 3. Our decoder map (files, functions, line numbers) — `decoder/mobiclip.c`
Decode entry: `mobi_decode()` → per-macroblock loop → `process_block()` (L1053) → phases:

| Phase | our C function | line | bound | asm priority |
|---|---|---|---|---|
| Entropy / VLC bit reader | `read_run_encoding()` L468 + `GetBitContext` ops | 468 | **compute** | **#1** |
| Coeff decode + IDCT + residual add (fused) | `add_coefficients_impl()` | 480 | mixed | **#2** |
| IDCT butterflies | `idct()` L431, `inverse4()` L418 | 418/431 | **compute** | **#2** |
| Intra prediction | `predict_intra()` L928, `pick_4..pick_8` L684-870 | 928 | light (3%) | low |
| Motion comp (half-pel) | `mc_asm.s` (disabled) / C paths | — | **memory** | **skip** |

Context struct `MobiClipContext` (L~315): `AVFrame *pic[6]` (frame pool, `current_pic` index),
`GetBitContext gb` (the bitstream reader — **the entropy hot state**), `int qtab[2][64]` (quant),
`MotionXY *motion`, `dct_tab_idx`, `quantizer`. Frame buffers via `ff_reget_buffer()` in
`decoder/mobicompat.c` L32 (`linesize[0]=width`, tightly packed, `calloc`+`PLANE_PAD`).

The IDCT (L431): size-4 = `inverse4` (4-pt butterfly); size-8 = even part via `inverse4(tmp)` + odd
part (`e,f,g,h` → `x0..x3`) then add/sub. Called per-row then per-column in `add_coefficients_impl`,
which reads run/level via `read_run_encoding`, dequantizes (`qtab`), places into `mat[64]` in zigzag,
runs row+col IDCT, then writes `dst[x] = clip(dst[x] + (mat[y*size+x] >> 6))` straight to the frame.

`GetBitContext` = FFmpeg's `get_bits.h` reader (`bitstream`, bit position, `get_bits`, `get_bits1`,
`get_sbits`, `show_bits`). This is the single most-executed piece of code (every coefficient).

---

## 4. Reference decoder map (Ghidra decompile: `~/Downloads/all_decompiled_source.c`, + `code.bin`)
Stripped (all `FUN_`), but these are IDENTIFIED and are the blueprint:

- **Entropy/VLC:** `FUN_0010e7e4` (@0x10e7e4, ~line 33602) + **`FUN_0010f168`** = the **bit reader**
  (the `CARRY4(x*0x10000,..)` are bit tests; `code*` dispatch = a **bit-tree / table-driven VLC**).
  This is our `read_run_encoding` + `GetBitContext`. **Port this to asm first (biggest compute win).**
- **IDCT:** `FUN_0010ef2c` (~line 33900-34060). Recognizable by `mat[0] += 0x20; ... >> 6` and the
  8-pt butterfly. Key techniques to replicate in asm:
  - **Fully-unrolled** row and column butterflies (no loops).
  - **MANY specialized sparse paths dispatched by coefficient pattern** (`param < 0x7d` = DC-only,
    `< 0x85`, `< 0xa1` … each a separate straight-line routine). Our C has only crude
    idct-skip/DC-only (`mobi_opt` bits 2/4). The reference has a *dispatch tree* of ~half a dozen
    unrolled variants for common sparsities. **This is a real win we have NOT ported.**
  - Writes straight to `dst` via the clamp path (fused; we already fuse — keep it, skip the LUT,
    use `USAT`).
- **Motion comp:** `FUN_00112430` + `FUN_0011257c` (@0x112430, ~line 34916). Aligned `uint*` loads +
  `* 0x100` (<<8) shift-neighbor + SWAR avg (`DAT_001126b4 = 0x7F7F7F7F`, `>>1`), writes `int*` coeff
  buffer, stride `0x400`. **We already matched this and it did not help (memory bound). Skip.**
- Movie load entry `FUN_0017a648` (allocs 0x400-stride buffers). ctx offsets in reference: coeff
  buffer `+0x1b8`, clamp-table base `*(ctx+0x44)+0x40`, frame stride `0x400`.
- Services used (from `code.bin` strings): `dsp::DSP`, `y2r:u`, `gsp::Gpu` — SAME hardware we use;
  one worker thread (`svc 0x08 CreateThread`). No secret codec path. The edge is purely the tuned
  decoder + the worker freeing the main core (we already replicate the worker architecture).

---

## 5. THE PLAN — order of work (each step: implement → checksum-verify → measure)

### Step 0 — Baseline & harness (0.5 day)
- Build `gputest` (decode-only checksum matrix) and `gputest/ip` (in-player fps). Confirm the FNV
  framesum path. Establish current ms/frame and fps on a KNOWN busy segment of the target file
  (`SEG_FRAC`, `SEG_PAIRS` in `gputest/ip/main.c`). Turn `MOBI_PROFILE` ON temporarily to get the
  per-phase tick buckets (`mobi_prof[]`: 0=intra 1/5=entropy 4=transform 3=motion); confirm entropy+
  IDCT ≈ 50% and motion ≈ 38%. Turn it OFF for all speed measurements.

### Step 1 — Assembly VLC/entropy bit reader (HIGHEST VALUE, ~1-2 days)
- Rewrite the coefficient entropy loop (`read_run_encoding` + the `get_bits`/`get_bits1`/`get_sbits`
  calls inside `add_coefficients_impl`) as one hand-scheduled asm routine, modeled on reference
  `FUN_0010f168`/`FUN_0010e7e4`. Keep the bitstream state (bit accumulator + count) **in registers**
  across the whole coefficient run; refill 32 bits at a time with a single word load + `REV` if
  needed. Use a **table-driven VLC** (leading-zero/`CLZ` + table index) instead of bit-at-a-time
  `get_bits1` loops. This is where C→asm pays the most (branchy, register-pressure heavy).
- Gate behind a new `mobi_opt` bit (e.g. `0x200`). Route only when applicable; fall back to C.
- **Verify checksum IDENTICAL**, then measure. Expect the largest single gain here.

### Step 2 — Assembly IDCT with sparse-path dispatch (~1-2 days)
- Port `idct()`/`inverse4()` to a fully-unrolled asm routine. Then add the reference's **sparse
  dispatch**: inspect the coefficient pattern (which rows/cols are nonzero, DC-only, etc.) and jump to
  a specialized straight-line variant (DC-only, single-row, few-coeff, full). Mirror the reference's
  `param<0x7d / <0x85 / <0xa1 …` thresholds. Keep the fused `USAT` write to `dst` (no LUT).
- Gate behind another `mobi_opt` bit (e.g. `0x400`). **Checksum IDENTICAL**, then measure.

### Step 3 — Combine + integrate (~0.5 day)
- Best combined config becomes the Old-3DS default (replacing `mobi_opt=14`). Wire it in
  `moflex_playback.c` `moflex_play_gpu()` (Old-3DS path only; New-3DS classic path stays on its
  proven config — do NOT touch it).
- Re-measure full-movie: does the ring stay non-empty (`q>0`) through busy scenes? Target: sustained
  decode ≥ content frame rate so the decode-ahead buffer never starves.

### Step 4 — Only if still short: cache/prefetch + motion-comp software pipelining
- If entropy+IDCT asm isn't enough, revisit motion comp NOT via faster instructions but via
  **software-pipelined PLD**: prefetch the NEXT macroblock's reference block while decoding the
  current one (hide the RAM latency that bounds it). This is the only motion-comp lever with headroom.
- Consider a **2nd worker thread for entropy vs pixel** only as a last resort (core-1 contention has
  historically hurt; measure).

---

## 6. Validation protocol (mandatory for every step)
1. Implement asm behind a fresh `mobi_opt` bit; C path stays default (`mobi_opt` bit clear).
2. `gputest` checksum matrix: the new config's FNV framesum MUST byte-match stock over the whole
   segment. If not identical, it's a correctness bug — fix before believing any speed number.
3. Only then compare ms/frame (decode-only) AND in-player fps (`gputest/ip`, includes Y2R contention).
   A win must show in BOTH; some opts help pure decode but regress in-player.
4. Keep every experiment behind its bit so nothing destabilizes the shipped default.

## 7. Known pitfalls (from 4 days of this)
- Don't trust the container timebase for frame rate (it lied: `tb=1001/24000` but true rate differs).
  (Sync is a separate, now-understood problem — see memory file; NOT part of this asm plan.)
- Don't add a clamp LUT (GCC's `USAT` beats it). Don't do stride-1024. Don't asm-optimize motion comp
  instructions. Don't leave `MOBI_PROFILE` on when measuring.
- Keep the New-3DS classic path (`moflex_play`) untouched — it's the known-good shipping product.
- Bit-exactness first, speed second, every single time.

## 8. Success criterion
Old 3DS sustains decode ≥ the content's true frame rate on busy scenes (`q` never hits 0 in the
player HUD), so the existing decode-ahead ring + audio-worker + relative-timeline sync (all already
built and working) deliver smooth, in-sync 3D playback matching the official player.
