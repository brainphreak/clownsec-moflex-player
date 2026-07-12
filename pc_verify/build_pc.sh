#!/bin/bash
# Self-contained PC build of the moflex decoder + verification harnesses.
# Proves the SAME code that runs on 3DS decodes bit-exactly vs ffmpeg on the host.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
DEC="$ROOT/decoder"
FSUP="$ROOT/ffmpeg_support"
INC="-I $DEC -I $FSUP/include -I $FSUP/include/libavcodec"
CFLAGS="-std=c11 -O2 -w"
OUT="$HERE/out"
mkdir -p "$OUT"

# shared objects
cc -c $CFLAGS $INC "$DEC/mobiclip.c"       -o "$OUT/mobiclip.o"
cc -c $CFLAGS $INC "$DEC/mobicompat.c"     -o "$OUT/mobicompat.o"
cc -c $CFLAGS $INC "$DEC/moflex_demux.c"   -o "$OUT/moflex_demux.o"
cc -c $CFLAGS $INC "$DEC/adpcm_moflex.c"   -o "$OUT/adpcm_moflex.o"
cc -c $CFLAGS $INC "$FSUP/vlc.c"           -o "$OUT/vlc.o"
cc -c $CFLAGS $INC "$FSUP/golomb.c"        -o "$OUT/golomb.o"
cc -c $CFLAGS $INC "$FSUP/mathtables.c"    -o "$OUT/mathtables.o"
cc -c $CFLAGS $INC "$FSUP/reverse.c"       -o "$OUT/reverse.o"

# host-only link stubs for the ARM-asm motion-comp routines (mc_asm.s is devkitARM-only)
cc -c $CFLAGS $INC "$HERE/mc_stub.c"        -o "$OUT/mc_stub.o"
# host C reference for mobi_entropy_asm (0x400) -- verifies the wiring; .s is device-only
cc -c $CFLAGS $INC "$HERE/entropy_c_ref.c"  -o "$OUT/entropy_c_ref.o"

# video test
cc -c $CFLAGS $INC "$HERE/test_decode.c"   -o "$OUT/test_decode.o"
cc -O2 "$OUT"/mobiclip.o "$OUT"/mobicompat.o "$OUT"/vlc.o "$OUT"/golomb.o \
       "$OUT"/mathtables.o "$OUT"/reverse.o "$OUT"/moflex_demux.o "$OUT"/mc_stub.o "$OUT"/entropy_c_ref.o "$OUT"/test_decode.o \
       -o "$HERE/test_decode"

# audio test
cc -c $CFLAGS $INC "$HERE/test_audio.c"    -o "$OUT/test_audio.o"
cc -O2 "$OUT"/moflex_demux.o "$OUT"/adpcm_moflex.o "$OUT"/test_audio.o -o "$HERE/test_audio"

echo ">>> built: $HERE/test_decode  and  $HERE/test_audio"
echo
echo "Verify vs ffmpeg (replace FILE):"
echo "  ./test_decode FILE.moflex ours.yuv 300"
echo "  ffmpeg -v error -i FILE.moflex -frames:v 300 -f rawvideo -pix_fmt yuv420p ref.yuv"
echo "  cmp ours.yuv ref.yuv   # must be identical"
echo "  ./test_audio FILE.moflex ours.pcm 5292000"
echo "  ffmpeg -v error -t 30 -i FILE.moflex -vn -f s16le ref.pcm"
echo "  cmp <(head -c 5292000 ours.pcm) ref.pcm   # must be identical"
