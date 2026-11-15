#!/bin/bash
# Engine test channel build: clownsec_player_test.cia + .3dsx + latest.txt (UniqueID 0xf8a2f).
# Installs beside the official app and updates only from the engine-test release. See RELEASING.md.
set -e
cd "$(dirname "$0")"
BIN="$(cd ../tools/bin && pwd)"
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export DEVKITARM=${DEVKITARM:-/opt/devkitpro/devkitARM}
unset UPDATE_TEST_URL
export BUILD_TAG=${BUILD_TAG:-$(./buildnum.sh)}
make clean BUILD_TAG=$BUILD_TAG >/dev/null
make TEST_CHANNEL=1 BUILD_TAG=$BUILD_TAG
"$BIN/bannertool" makesmdh -s "CLOWNSEC VIDEO TEST" -l "MOFLEX VIDEO PLAYER (engine test)" -p "brAinphreAk" \
    -i icon.png -o clownsec_player_test.smdh
"$DEVKITPRO/tools/bin/3dsxtool" clownsec_player.elf clownsec_player_test.3dsx --smdh=clownsec_player_test.smdh
"$BIN/bannertool" makebanner -i banner.png -a banner_audio.wav -o banner.bin
"$BIN/makerom" -f cia -o clownsec_player_test.cia -elf clownsec_player.elf -rsf cia_test.rsf \
    -icon clownsec_player_test.smdh -banner banner.bin -exefslogo -target t
printf '%s\n%s\n' "$BUILD_TAG" "$(md5 -q clownsec_player_test.cia)" > latest_test.txt
echo "built engine test $BUILD_TAG: clownsec_player_test.cia/.3dsx, latest_test.txt (upload as latest.txt)"
