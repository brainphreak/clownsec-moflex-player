#!/bin/bash
# check.sh <tag>: build test_decode with the player's decoder config and md5 900 frames of the 3D sample
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$HERE/.." && pwd)"
DEC="$ROOT/decoder"; FSUP="$ROOT/ffmpeg_support"; OUT="$HERE/out_chk"; mkdir -p "$OUT"
INC="-I $DEC -I $FSUP/include -I $FSUP/include/libavcodec"
CF="-std=c11 -O2 -w -DMOBI_OPT_CONST=${OPT:-0x08bbda5e}"
for f in "$DEC/mobiclip.c" "$DEC/mobicompat.c" "$DEC/moflex_demux.c" "$FSUP/vlc.c" "$FSUP/golomb.c" "$FSUP/mathtables.c" "$FSUP/reverse.c" "$HERE/test_decode.c"; do
  cc -c $CF $INC "$f" -o "$OUT/$(basename "${f%.c}").o"; done
cc -O2 "$OUT"/*.o -o "$OUT/test_decode"
"$OUT/test_decode" "$HERE/media/mj3d.moflex" "$OUT/out.yuv" ${N:-900} >/dev/null 2>&1
echo "$1 $(md5 -q "$OUT/out.yuv") $(stat -f %z "$OUT/out.yuv")"
