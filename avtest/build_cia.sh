#!/bin/bash
# avtest as an installable CIA, requesting SystemMode 96MB (Dev1) so the decoded-ahead ring gets a
# much bigger linear heap than the 64MB default allows. NB is sized at runtime from linearSpaceFree(),
# so it picks up the extra memory automatically -- nothing else to change.
set -e
cd "$(dirname "$0")"
BIN="$(cd ../tools/bin && pwd)"
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro} DEVKITARM=${DEVKITARM:-/opt/devkitpro/devkitARM}
make
"$BIN/bannertool" makesmdh -s "MoFlex AVTEST 96MB" -l "moflex A/V test, extended memory" \
    -p "brAinphreAk" -i ../app/icon.png -o avtest.smdh
"$BIN/bannertool" makebanner -i ../app/banner.png -a ../app/banner_audio.wav -o avtest_banner.bin
"$BIN/makerom" -f cia -o moflex_avtest.cia -elf moflex_avtest.elf -rsf cia.rsf \
    -icon avtest.smdh -banner avtest_banner.bin -exefslogo -target t
cp -f moflex_avtest.cia moflex_avtest.3dsx "$HOME/Downloads/" 2>/dev/null || true
echo "built + deployed moflex_avtest.cia (96MB)"
